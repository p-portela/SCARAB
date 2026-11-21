#!/usr/bin/env python3
"""Extract the shader library from a the target title (RPF9) shaders.rpf.

Format (verified against the target title 1.0.2.1 Scarlett, shaders.rpf 411,695,600 B):

  RPF9 container
    0x00  magic "RPF9" (on disk: 39 46 50 52 = '9FPR')
    0x04  u32 f1, 0x08 u32 f2, 0x0C u32 f3
    0x10  u64 per-archive id
    0x18  u64 constant 0x00047a00 / 0x00000150
    0x20  u16 flag=1, u16 type, u32 count
    0x28  u32 key-id, u32 version (0x00090f01/2)
    0x30  16 zero bytes
    0x40  AES-ECB-encrypted TOC  (blocks 0 and 1 identical across all archives)

  Shader record (inside the file data, NOT encrypted)
    [u8 len+1][name ASCII][u8 0x00][u8 0x32]
    [u32 compressedSize][u32 0x000DDCE7][u32 compressedSize][u32 uncompressedSize]
    [raw deflate stream, no zlib header]

  What the deflate stream contains (IMPORTANT - not portable shader bytecode):
    a DXBC-shaped container with chunks SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV.
    The 'DXIL' chunk is NOT LLVM bitcode (no BC C0 DE anywhere in the file).
    It holds two sub-sections: 'XDAT' (reflection/tables) and 'XBCH', where
    'XBCH' is zlib-compressed (78 9c) and decompresses to magic 'FSSM'
    version 0x101 - a low-entropy (~1.7-2.2 bits/byte), 30%-nonzero structured
    record with 0xDEADBEEF sentinel dwords, ending in a 20-byte-record binding
    table and the length-prefixed shader name. FSSM is the publisher's own shader
    state/definition format, NOT DXIL/DXBC program code. Do not treat the
    extracted blobs as PC-loadable shaders.

Usage:  python3 extract_shaders.py shaders.rpf outdir
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import os, re, sys, zlib, csv, hashlib, struct, collections

NAME_RE = re.compile(rb"[A-Za-z][A-Za-z0-9_]{5,90}_Wrapped")
DESC_OFF = 18          # deflate stream starts 18 bytes after the name's end
CAP      = 1 << 20     # max compressed shader size to read

def inflate_at(mv, off):
    o = zlib.decompressobj(-15)
    out = o.decompress(mv[off:off + CAP])
    return out, CAP - len(o.unused_data)

def dxbc_chunks(b):
    if b[:4] != b"DXBC":
        return []
    n = struct.unpack_from("<I", b, 28)[0]
    offs = struct.unpack_from("<%dI" % n, b, 32)
    return [b[o:o + 4].decode("latin1") for o in offs if o + 8 <= len(b)]

def main(rpf, outdir):
    raw = open(rpf, "rb").read()
    mv = memoryview(raw)
    assert raw[:4] == b"9FPR", "not an RPF9 archive: %r" % raw[:4]
    os.makedirs(outdir, exist_ok=True)
    names = [(m.start(), m.end(), m.group().decode()) for m in NAME_RE.finditer(raw)]
    rows, seen = [], set()
    total_unc = total_cmp = 0
    for _s, e, nm in names:
        out, cons = inflate_at(mv, e + DESC_OFF)
        if out[:4] != b"DXBC":
            continue
        h = hashlib.sha256(out).hexdigest()
        total_unc += len(out); total_cmp += cons
        if (nm, h) in seen:
            continue
        seen.add((nm, h))
        stage = nm.split("_")[0]
        sdir = os.path.join(outdir, stage)
        os.makedirs(sdir, exist_ok=True)
        fn = os.path.join(sdir, nm + ".dxbc")
        if os.path.exists(fn):
            fn = os.path.join(sdir, "%s.%s.dxbc" % (nm, h[:8]))
        with open(fn, "wb") as fh:
            fh.write(out)
        ch = dxbc_chunks(out)
        rows.append(dict(name=nm, stage=stage, unc=len(out), cmp=cons,
                         dxbc_size=struct.unpack_from("<I", out, 24)[0],
                         kind="DXIL" if "DXIL" in ch else "DXBC",
                         chunks=";".join(ch), sha256=h))
    with open(os.path.join(outdir, "index.csv"), "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)
    print("records %d  distinct blobs %d  uncompressed %.1f MB  compressed %.1f MB"
          % (len(names), len(rows), total_unc / 1e6, total_cmp / 1e6))
    print("stages:", collections.Counter(r["stage"] for r in rows).most_common())

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "shaders")
