#!/usr/bin/env python3
"""G1: the bridge's export contract must equal the console runtime's, byte-exact.

This is the gate that makes "drop-in replacement" a checkable claim rather than
an assertion. It delegates the comparison to tools/verify_exports.py and then
asserts the specific counts, so a build that exports the right names but a wrong
ordinal, or a wrong GUID byte, fails here.

Run standalone:
    python3 tests/test_export_contract.py [built.dll] [reference.dll]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.environ.get("SCARAB_VENDOR", os.path.join(ROOT, "vendor"))
REF = os.path.join(VENDOR, "x64_dx12", "d3d12_xs.dll")


def find_built():
    for base in (os.environ.get("SCARAB_BUILD_DIR", os.path.join(ROOT, "build")),):
        for dirpath, _dirs, files in os.walk(base):
            if "d3d12_xs.dll" in files:
                return os.path.join(dirpath, "d3d12_xs.dll")
    return None


def main():
    built = sys.argv[1] if len(sys.argv) > 1 else find_built()
    ref = sys.argv[2] if len(sys.argv) > 2 else REF
    if not built:
        print("FAIL: no built d3d12_xs.dll found; build first")
        return 1
    if not os.path.exists(ref):
        print("SKIP: reference runtime not present (%s) - see docs/PROVENANCE.md" % ref)
        return 0

    cmd = [sys.executable, os.path.join(ROOT, "tools", "verify_exports.py"), built, ref]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode != 0:
        print("FAIL: verify_exports reported a mismatch")
        return 1

    expectations = [
        ("[1] names      : 116/116 present", "all 116 export names present"),
        ("[2] ordinals   : 116/116 match", "all 116 ordinals match"),
        ("[3] count      : built=116 reference=116", "export count is 116"),
        ("[4] GUID data  : 93/93 byte-identical", "all 93 GUID exports byte-identical"),
        ("[5] forwarders : 0", "no unexpected forwarders"),
    ]
    bad = [why for needle, why in expectations if needle not in r.stdout]
    if bad:
        for w in bad:
            print("FAIL: " + w)
        return 1
    print("PASS: export contract is byte-exact (116 exports, 93 GUIDs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
