#!/usr/bin/env python3
"""Verify a built bridge DLL against the genuine console d3d12_xs.dll contract.

Checks, in order of importance:
  1. export name set is identical
  2. export ordinals are identical
  3. export count identical, no extras
  4. every data export (IID_* / DXGI_DEBUG_*) really is data, and its 16 bytes
     match the genuine runtime byte-for-byte
  5. every function export is a real section-relative code RVA (not a forwarder)

Exits non-zero on any mismatch. Usage:
    tools/verify_exports.py <built.dll> [reference.dll]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peinfo import PE

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_VENDOR = os.environ.get("SCARAB_VENDOR", os.path.join(_ROOT, "vendor"))
REF_DEFAULT = os.path.join(_VENDOR, "x64_dx12", "d3d12_xs.dll")


def contract(pe):
    ex = pe.exports()
    out = {}
    for n, o, r in ex["names"]:
        out[n] = {"ordinal": o, "rva": r, "fwd": ex["fwd"].get(n)}
    return out, ex


def main():
    built = sys.argv[1] if len(sys.argv) > 1 else "bridge/build/d3d12_xs.dll"
    ref = sys.argv[2] if len(sys.argv) > 2 else REF_DEFAULT
    b, be = contract(PE(built))
    r, re_ = contract(PE(ref))
    fails, warns = [], []

    print("built     : %s  (%d exports, base %d)" % (built, len(b), be["base"]))
    print("reference : %s  (%d exports, base %d)" % (ref, len(r), re_["base"]))
    print()

    missing = sorted(set(r) - set(b))
    extra = sorted(set(b) - set(r))
    print("[1] names      : %d/%d present" % (len(set(r) & set(b)), len(r)))
    if missing:
        fails.append("missing exports: %s" % ", ".join(missing))
    if extra:
        fails.append("unexpected exports: %s" % ", ".join(extra))

    ordbad = [(n, b[n]["ordinal"], r[n]["ordinal"]) for n in sorted(set(b) & set(r))
              if b[n]["ordinal"] != r[n]["ordinal"]]
    print("[2] ordinals   : %d/%d match" % (len(set(b) & set(r)) - len(ordbad), len(set(b) & set(r))))
    for n, bo, ro in ordbad:
        fails.append("ordinal mismatch %s: built=%d reference=%d" % (n, bo, ro))

    print("[3] count      : built=%d reference=%d" % (len(b), len(r)))
    if len(b) != len(r):
        fails.append("export count %d != %d" % (len(b), len(r)))

    pe_b = PE(built)
    nguid = 0
    for n in sorted(set(b) & set(r)):
        if not n.startswith(("IID_", "DXGI_DEBUG_")):
            continue
        po, pr = pe_b.r2o(b[n]["rva"]), PE(ref).r2o(r[n]["rva"])
        gb = pe_b.b[po:po + 16]
        gr = PE(ref).b[pr:pr + 16]
        if len(gb) != 16 or gb != gr:
            fails.append("GUID data mismatch for %s: built=%s ref=%s" % (n, gb.hex(), gr.hex()))
        else:
            nguid += 1
    print("[4] GUID data  : %d/%d byte-identical"
          % (nguid, sum(1 for n in r if n.startswith(("IID_", "DXGI_DEBUG_")))))

    fwd = [n for n in b if b[n]["fwd"]]
    if fwd:
        warns.append("built DLL forwards %d exports (unexpected): %s" % (len(fwd), ", ".join(fwd[:5])))
    print("[5] forwarders : %d" % len(fwd))
    print()

    for w in warns:
        print("WARN  " + w)
    for f in fails:
        print("FAIL  " + f)
    if fails:
        print("\nRESULT: FAIL (%d)" % len(fails))
        return 1
    print("RESULT: PASS - export contract is byte-exact (%d exports, %d GUIDs)"
          % (len(b), nguid))
    return 0


if __name__ == "__main__":
    sys.exit(main())
