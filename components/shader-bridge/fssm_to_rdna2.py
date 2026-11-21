#!/usr/bin/env python3
"""FSSM (the target title / Scarlett shader container) -> raw RDNA2 code words + metadata.

Front half of the bridge into the upstream shader recompiler's shader recompiler
(the recompiler source tree), whose input is

    ShaderBinary { ShaderStage stage; uint64_t codeAddress;
                   span<const uint32_t> code; uint64_t headerAddress;
                   span<const std::byte> header; }

This tool supplies `code` (the RDNA2 word stream) and the metadata that identifies
the shader. The `header` (console-side resource/ABI metadata) is NOT produced here --
that is the open piece; see docs/fssm-analysis.md and docs/shader-abi-notes.md.

Verified container layout (observed, the target title 1.0.2.1 Scarlett):

  shader file (.dxbc)
    DXBC-shaped container, chunks SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV
    'DXIL' chunk payload:
       u32 0x00010066, u32 0x1a0, 'DXIL', ..., 'XDAT' section, 'XBCH' section
       'XBCH' = fourcc + u32 version=3 + u32 compressedSize + u32 0
                + u32 uncompressedSize + u32 0 + zlib stream (78 9c)
       zlib -> FSSM payload

  FSSM payload
    0x00 'FSSM'
    0x04 u32 0x101
    0x08 u64 filelen - 16
    0x10 u64 6
    0xc0 u64 code_off   (8-aligned; mod 16 is 0 or 8)
    0xc8 u64 code_size
    0xe8 u64 code_end   (== code_off + code_size)
    code_off + 0x00 : 16-byte sub-header, (0,0,0,0) or (8,1,0,0)
    code_off + 0x10 : raw RDNA2 instructions, padded with 0xBF9F0000 (s_code_end)
    code_end  -> eof : binding/metadata tables, then the shader name record

Usage:
    python3 fssm_to_rdna2.py <file.dxbc|shaders.rpf> <outdir> [--limit N]
"""
import os
import re
import sys
import json
import zlib
import struct
import hashlib
import argparse

NAME_RE = re.compile(rb"[A-Za-z][A-Za-z0-9_]{5,90}_Wrapped")
S_CODE_END = 0xBF9F0000
S_ENDPGM = 0xBF810000


def find_xbch(b):
    """Locate the real XBCH section header: fourcc followed by u32 version == 3.

    A plain find(b'XBCH') can hit a coincidental match inside data; 107 of 25,889
    files did exactly that during development. Validate instead of trusting.
    """
    i = -1
    while True:
        i = b.find(b"XBCH", i + 1)
        if i < 0:
            return None
        if i + 8 <= len(b) and struct.unpack_from("<I", b, i + 4)[0] == 3:
            return i
    return None


def unwrap(path):
    """file bytes -> (FSSM payload, outer container info)."""
    b = open(path, "rb").read()
    info = {}
    if b[:4] == b"FSSM":
        return b, {"wrapper": "bare-fssm"}
    if b[:4] != b"DXBC":
        raise ValueError("not a DXBC-shaped shader container: %r" % b[:4])
    n = struct.unpack_from("<I", b, 28)[0]
    offs = struct.unpack_from("<%dI" % n, b, 32)
    chunks = {}
    for o in offs:
        if o + 8 > len(b):
            continue
        chunks[b[o:o + 4].decode("latin1")] = (o, struct.unpack_from("<I", b, o + 4)[0])
    info["chunks"] = list(chunks)
    i = find_xbch(b)
    if i is None:
        raise ValueError("no valid XBCH section")
    ver, csize, _, usize, _ = struct.unpack_from("<5I", b, i + 4)
    payload = zlib.decompress(b[i + 24:])
    info.update(xbchVersion=ver, xbchCompressed=csize, xbchUncompressed=usize)
    if payload[:4] != b"FSSM":
        raise ValueError("XBCH did not decompress to FSSM: %r" % payload[:4])
    info["chunks"] = list(chunks)
    return payload, info


def parse_fssm(payload):
    """FSSM payload -> dict with header fields and the code region."""
    if payload[:4] != b"FSSM":
        raise ValueError("not FSSM")
    ver = struct.unpack_from("<I", payload, 4)[0]
    declared = struct.unpack_from("<Q", payload, 8)[0]
    six = struct.unpack_from("<Q", payload, 0x10)[0]
    code_off = struct.unpack_from("<Q", payload, 0xC0)[0]
    code_size = struct.unpack_from("<Q", payload, 0xC8)[0]
    code_end = struct.unpack_from("<Q", payload, 0xE8)[0]
    meta_ptr = struct.unpack_from("<Q", payload, 0xF0)[0]
    sub = payload[code_off:code_off + 0x10]
    return {
        "fssmVersion": ver,
        "declaredLen": declared,
        "field0x10": six,
        "codeOff": code_off,
        "codeSize": code_size,
        "codeEnd": code_end,
        "metaPtr": meta_ptr,
        "subHeader": sub.hex(),
        "codeConsistent": code_end == code_off + code_size,
        "lenConsistent": declared == len(payload) - 16,
    }


def code_words(payload, hdr):
    """The RDNA2 word stream: from code_off + 0x10 to code_end."""
    start = hdr["codeOff"] + 0x10
    end = hdr["codeEnd"]
    raw = payload[start:end]
    raw = raw[:len(raw) - (len(raw) % 4)]
    return list(struct.unpack("<%dI" % (len(raw) // 4), raw))


def walk(words):
    """Minimal structural sanity walk: family classification + terminator check."""
    fams = {}
    endpgm = codeend = 0
    for w in words:
        if w == S_ENDPGM:
            endpgm += 1
        if w == S_CODE_END:
            codeend += 1
        if (w & 0x80000000) == 0:
            sel = (w >> 25) & 0x3F
            fam = "VOPC" if sel == 0x3E else ("VOP1" if sel == 0x3F else "VOP2")
        elif (w & 0xC0000000) == 0x80000000:
            op = (w >> 23) & 0x7F
            fam = {0x7D: "SOP1", 0x7E: "SOPC", 0x7F: "SOPP"}.get(op, "SOPK" if op >= 0x60 else "SOP2")
        else:
            fam = {0x32: "VINTRP", 0x33: "VOP3P", 0x35: "VOP3", 0x36: "DS", 0x37: "FLAT",
                   0x38: "MUBUF", 0x3A: "MTBUF", 0x3C: "MIMG", 0x3D: "SMEM", 0x3E: "EXP"}.get(w >> 26, "?")
        fams[fam] = fams.get(fam, 0) + 1
    return fams, endpgm, codeend


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("outdir")
    ap.add_argument("--limit", type=int, default=0, help="stop after N shaders")
    ap.add_argument("--pack", action="store_true", help="treat input as shaders.rpf")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    os.makedirs(os.path.join(args.outdir, "rdna2"), exist_ok=True)
    rows = []

    if args.pack:
        raw = open(args.input, "rb").read()
        names = [(m.start(), m.end(), m.group().decode()) for m in NAME_RE.finditer(raw)]
        work = []
        skipped = 0
        for s, e, nm in names:
            # RPF9 shader records store a RAW deflate stream (no zlib header),
            # unlike the XBCH section inside, which IS zlib (78 9c).
            try:
                d = zlib.decompressobj(-15)
                payload = d.decompress(memoryview(raw)[e + 18:e + 18 + (1 << 20)])
            except Exception:
                skipped += 1
                continue
            if payload[:4] != b"DXBC":
                skipped += 1
                continue
            work.append((nm, payload))
        print("pack: %d records found, %d usable, %d skipped" % (len(names), len(work), skipped))
    else:
        nm = os.path.basename(args.input)[:-5]
        work = [(nm, open(args.input, "rb").read())]

    seen = set()
    for nm, blob in work:
        try:
            payload, info = unwrap_blob(blob)
            hdr = parse_fssm(payload)
        except Exception as ex:
            rows.append({"name": nm, "error": str(ex)})
            continue
        words = code_words(payload, hdr)
        h = hashlib.sha256(payload).hexdigest()
        if (nm, h) in seen:
            continue
        seen.add((nm, h))
        fams, endpgm, codeend = walk(words)
        stage = nm.split("_")[0]
        out = os.path.join(args.outdir, "rdna2", "%s.%s.rdna2" % (nm, h[:8]))
        with open(out, "wb") as fh:
            fh.write(struct.pack("<%dI" % len(words), *words))
        rows.append(dict(name=nm, stage=stage, file=os.path.relpath(out, args.outdir),
                         codeWords=len(words), codeBytes=len(words) * 4,
                         endpgm=endpgm, codeEndPad=codeend, families=fams,
                         sha256=h, **hdr))
        if args.limit and len(rows) >= args.limit:
            break

    with open(os.path.join(args.outdir, "rdna2_index.json"), "w") as fh:
        json.dump(rows, fh, indent=1)
    ok = [r for r in rows if "error" not in r]
    print("emitted %d shaders (%d errors)" % (len(ok), len(rows) - len(ok)))
    if ok:
        print("all consistent (code_end == code_off+code_size): %s" %
              all(r["codeConsistent"] for r in ok))
        print("all with s_endpgm: %s" % all(r["endpgm"] for r in ok))
        print("code words: min %d median %d max %d" % (
            min(r["codeWords"] for r in ok),
            sorted(r["codeWords"] for r in ok)[len(ok) // 2],
            max(r["codeWords"] for r in ok)))
        agg = {}
        for r in ok:
            for k, v in r["families"].items():
                agg[k] = agg.get(k, 0) + v
        print("family mix:", sorted(agg.items(), key=lambda kv: -kv[1])[:12])
    print("index:", os.path.join(args.outdir, "rdna2_index.json"))


def unwrap_blob(b):
    """Same as unwrap() but from bytes rather than a path."""
    if b[:4] == b"FSSM":
        return b, {"wrapper": "bare-fssm"}
    if b[:4] != b"DXBC":
        raise ValueError("not DXBC-shaped: %r" % b[:4])
    i = find_xbch(b)
    if i is None:
        raise ValueError("no valid XBCH section")
    ver, csize, _, usize, _ = struct.unpack_from("<5I", b, i + 4)
    payload = zlib.decompress(b[i + 24:])
    if payload[:4] != b"FSSM":
        raise ValueError("XBCH did not decompress to FSSM")
    return payload, {"xbchVersion": ver, "xbchCompressed": csize, "xbchUncompressed": usize}


if __name__ == "__main__":
    main()
