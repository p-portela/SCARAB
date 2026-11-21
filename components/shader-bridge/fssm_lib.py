#!/usr/bin/env python3
"""FSSM extraction helper: .dxbc container -> DXIL chunk -> XBCH -> zlib -> FSSM.

Stdlib only. Verified against the target title (Scarlett) shaders extracted from shaders.rpf.

Outer container (file): DXBC magic, chunk table at 0x1C (count) / 0x20 (offsets).
Chunks seen: SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV

DXIL chunk body (a second-level DXIL container):
  0x00 u32 0x00010066            (marker)
  0x04 u32 0x61                  (0x61 = 97; header-size-ish)
  0x08 'DXIL'                    (container fourcc)
  0x0C u32 0x106                 (version)
  0x10 u32 containerSize         (== chunk body size - 12)
  0x14 u32 partCount
  0x18 ...                       (part table / XDAT ...)
  ...  'XDAT'  data tables
  ...  'XBCH'  compressed shader (this module extracts this)
  ...  'DXIL' / 'NOBC' trailing container
"""
import struct, zlib


def dxbc_chunks(b):
    """dict fourcc -> (data_offset, size) for the outer DXBC container."""
    if b[:4] != b"DXBC":
        raise ValueError("not DXBC")
    n = struct.unpack_from("<I", b, 28)[0]
    offs = struct.unpack_from("<%dI" % n, b, 32)
    out = {}
    for o in offs:
        fourcc = b[o:o + 4]
        sz = struct.unpack_from("<I", b, o + 4)[0]
        out[fourcc] = (o + 8, sz)
    return out


def dxil_body(b):
    off, sz = dxbc_chunks(b)[b"DXIL"]
    return b[off:off + sz]


def find_tag(body, tag, start=0):
    i = body.find(tag, start)
    return i


def xbch_header(b):
    body = dxil_body(b)
    i = body.find(b"XBCH")
    ver, csize, z0, usize, z1 = struct.unpack_from("<5I", body, i + 4)
    return dict(off=i, version=ver, compressedSize=csize, z0=z0,
                uncompressedSize=usize, z1=z1)


def fssm_from_dxbc(b):
    """Full chain: .dxbc bytes -> decompressed FSSM bytes."""
    body = dxil_body(b)
    i = body.find(b"XBCH")
    assert i >= 0, "no XBCH"
    ver, csize, z0, usize, z1 = struct.unpack_from("<5I", body, i + 4)
    z = body[i + 24:i + 24 + csize]
    out = zlib.decompress(z)
    assert out[:4] == b"FSSM", out[:4]
    return out


if __name__ == "__main__":
    import sys
    b = open(sys.argv[1], "rb").read()
    print("xbch header:", xbch_header(b))
    f = fssm_from_dxbc(b)
    print("fssm len:", len(f), "magic:", f[:4],
          "version: 0x%x" % struct.unpack_from("<I", f, 4)[0])
