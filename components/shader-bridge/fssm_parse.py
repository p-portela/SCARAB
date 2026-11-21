#!/usr/bin/env python3
"""Parser skeleton for the publisher FSSM shader container (the target title, Scarlett).

FSSM turns out to hold AMD RDNA/gfx10 (RDNA2 on Scarlett) GPU **machine code**,
not the publisher IR/bytecode and not DXIL.  This module documents the container layout
that was measured and walks the instruction stream with the RDNA2 family classifier
in ``tools/rdna2_classify.py`` (encoding rules taken from AMD's machine-readable
RDNA2 ISA XML via the upstream shader recompiler RdnaDecoder).

Two input forms are accepted:
  * a decompressed FSSM blob                  -> parse_fssm(data)
  * an extracted ``.dxbc`` container          -> fssm_from_dxbc(data) then parse_fssm

Measured layout (offsets are byte offsets from the start of the FSSM blob):

  0x00  'FSSM'                       magic
  0x04  u32 0x101                    version
  0x08  u64  == len(blob) - 16       payload size
  0x10  u64  6                       constant across all 25,889 blobs
  0x18  ...                          8-byte slots; 0xdeadbeefdeadbeef = "unset"
  0x78  u32 pair (2, N)
  0x80  u64  0x44 | 0x45             a count
  0xc0  u64  code_off                offset of the RDNA2 code section  (OBSERVED)
  0xc8  u64  code_size               size of that section, 16-byte aligned (OBSERVED)
  0xe8  u64  code_off + code_size    == 0xc0 + 0xc8  (400/400 sampled)  (OBSERVED)
  0xf0  u64  >= code_end             points into the post-code metadata/tables
  0xf8  u32 pair, 0x100 u32 pair, 0x110 u32 pair ...   small metadata fields

  code_off           16-byte sub-header, then the RDNA2 instruction stream,
                     then 0xBF9F0000 (SOPP s_code_end) filler up to code_end
  code_end .. eof    metadata / binding tables, then the length-prefixed shader
                     name record: [-1][0][u16 len][name ASCII]

Stdlib only.
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from rdna2_classify import (  # noqa: E402
    TWO_WORD, family, name_of, sopp_opcode, vop1_opcode, vop2_opcode,
)

DEAD = 0xDEADBEEFDEADBEEF
CODE_OFF_FIELD = 0xC0
CODE_SIZE_FIELD = 0xC8
CODE_END_FIELD = 0xE8
META_PTR_FIELD = 0xF0
CODE_SUBHEADER = 16


class FssmError(Exception):
    pass


class FssmHeader:
    """The measured fixed fields of an FSSM blob."""

    __slots__ = ("magic", "version", "size", "const6", "code_off", "code_size",
                 "code_end", "meta_ptr")

    def __init__(self, data):
        if data[:4] != b"FSSM":
            raise FssmError("bad magic %r" % data[:4])
        self.magic = data[:4]
        self.version = struct.unpack_from("<I", data, 4)[0]
        self.size = struct.unpack_from("<Q", data, 8)[0]
        self.const6 = struct.unpack_from("<Q", data, 0x10)[0]
        self.code_off = struct.unpack_from("<Q", data, CODE_OFF_FIELD)[0]
        self.code_size = struct.unpack_from("<Q", data, CODE_SIZE_FIELD)[0]
        self.code_end = struct.unpack_from("<Q", data, CODE_END_FIELD)[0]
        self.meta_ptr = struct.unpack_from("<Q", data, META_PTR_FIELD)[0]
            # 0xf0 is either an offset into the post-code metadata/tables region or the
            # 0xdeadbeefdeadbeef "unset" sentinel (common for small shaders).

    def __repr__(self):
        return ("FssmHeader(version=0x%x, size=%d, code_off=0x%x, code_size=0x%x, "
                "code_end=0x%x, meta_ptr=0x%x)" % (
                    self.version, self.size, self.code_off, self.code_size,
                    self.code_end, self.meta_ptr))


class FssmFile:
    def __init__(self, data):
        self.data = data
        self.header = FssmHeader(data)

    # -- code section -----------------------------------------------------
    def code_bytes(self):
        h = self.header
        return self.data[h.code_off + CODE_SUBHEADER:h.code_end]

    def code_subheader(self):
        h = self.header
        return struct.unpack_from("<4I", self.data, h.code_off)

    def is_code_consistent(self):
        h = self.header
        return (h.code_off + h.code_size == h.code_end
                and h.code_end <= len(self.data)
                and h.code_off % 8 == 0 and h.code_size % 16 == 0)

    # -- instruction stream ----------------------------------------------
    def instructions(self):
        """Walk the code section as RDNA2 instructions.

        Yields dicts: {off, words, family, name, mnemonic?}.  Lengths are the
        documented base widths (VOP3/VOP3P/SMEM/MUBUF/MTBUF/FLAT/DS/EXP/MIMG are
        two dwords, everything else one).  Literal-operand extension (+1 dword)
        is deliberately NOT applied: this is a skeleton, so a walk can drift on
        instructions with a 32-bit literal.  Branch targets still validate at
        ~100%, so use it for statistics, not for exact disassembly.
        """
        h = self.header
        start = h.code_off + CODE_SUBHEADER
        body = self.data[start:h.code_end]
        n = len(body) // 4
        words = struct.unpack_from("<%dI" % n, body)
        i = 0
        while i < n:
            w = words[i]
            fam = family(w)
            length = 2 if fam in TWO_WORD or fam == "MIMG" else 1
            yield {
                "off": start + i * 4,
                "words": words[i:i + length],
                "family": fam,
                "name": name_of(fam, w),
            }
            i += length

    def code_stats(self):
        """Family / SOPP / VOP1 / VOP2 histograms over the code section."""
        fams = {}
        sopp = {}
        vop1 = {}
        vop2 = {}
        nwords = 0
        for ins in self.instructions():
            fams[ins["family"]] = fams.get(ins["family"], 0) + 1
            nwords += len(ins["words"])
            if ins["family"] == "SOPP":
                k = name_of("SOPP", ins["words"][0])
                sopp[k] = sopp.get(k, 0) + 1
            elif ins["family"] == "VOP1":
                k = name_of("VOP1", ins["words"][0])
                vop1[k] = vop1.get(k, 0) + 1
            elif ins["family"] == "VOP2":
                k = name_of("VOP2", ins["words"][0])
                vop2[k] = vop2.get(k, 0) + 1
        return {"words": nwords, "families": fams, "sopp": sopp,
                "vop1": vop1, "vop2": vop2}

    def branch_report(self):
        """Validate s_branch / s_cbranch SIMM16 targets against the code body.

        A real program has essentially all branch targets inside the body; a
        byte-hash or IR stream does not.  (Measured: 12,786 / 12,789 = 100%.)
        """
        body = self.code_bytes()
        n = len(body) // 4
        words = struct.unpack_from("<%dI" % n, body)
        intgt = out = 0
        i = 0
        while i < n:
            w = words[i]
            fam = family(w)
            if fam == "SOPP" and sopp_opcode(w) in (2, 4, 5, 6, 7, 8, 9):
                simm = struct.unpack("<h", struct.pack("<H", w & 0xFFFF))[0]
                t = i + 1 + simm
                if 0 <= t < n:
                    intgt += 1
                else:
                    out += 1
            i += 2 if fam in TWO_WORD or fam == "MIMG" else 1
        return {"in_range": intgt, "out_of_range": out}

    # -- metadata / tail --------------------------------------------------
    def metadata(self):
        return self.data[self.header.code_end:self.header.meta_ptr]

    def tail(self):
        return self.data[self.header.meta_ptr:]

    def shader_name(self):
        """The length-prefixed ASCII shader name record ([-1][0][u16 len][name])."""
        i = self.data.rfind(b"\xff\xff\xff\xff")
        while i >= 0:
            if (i + 8 + 4 <= len(self.data)
                    and self.data[i + 4:i + 8] == b"\x00\x00\x00\x00"):
                ln = struct.unpack_from("<H", self.data, i + 10)[0]
                nm = self.data[i + 12:i + 12 + ln]
                if ln and all(33 <= c < 127 for c in nm):
                    return nm.decode("ascii")
            i = self.data.rfind(b"\xff\xff\xff\xff", 0, i)
        return None


def _fssm_from_dxbc(data):
    try:
        from fssm_lib import fssm_from_dxbc
    except ImportError as exc:  # pragma: no cover
        raise FssmError("fssm_lib.py not importable: %s" % exc)
    return fssm_from_dxbc(data)


def load(path):
    """Load an FSSM blob from either a raw FSSM file or an extracted .dxbc."""
    data = open(path, "rb").read()
    if data[:4] == b"DXBC":
        data = _fssm_from_dxbc(data)
    return FssmFile(data)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        f = load(path)
        h = f.header
        print("=" * 72)
        print(path)
        print("  header         :", h)
        print("  size check     : [0x08]=%d  len-16=%d  %s"
              % (h.size, len(f.data) - 16, "OK" if h.size == len(f.data) - 16 else "MISMATCH"))
        print("  section table  : 0x%x + 0x%x == 0x%x : %s"
              % (h.code_off, h.code_size, h.code_end,
                 "OK" if h.code_off + h.code_size == h.code_end else "MISMATCH"))
        print("  code subheader : %s" % (f.code_subheader(),))
        print("  shader name    : %s" % (f.shader_name(),))
        st = f.code_stats()
        print("  code words     : %d" % st["words"])
        top = sorted(st["families"].items(), key=lambda kv: -kv[1])[:8]
        print("  families       : %s" % ", ".join("%s=%d" % kv for kv in top))
        print("  SOPP           : %s"
              % ", ".join("%s=%d" % kv for kv in
                          sorted(st["sopp"].items(), key=lambda kv: -kv[1])[:8]))
        print("  branches       : %s" % (f.branch_report(),))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
