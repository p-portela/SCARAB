#!/usr/bin/env python3
"""G5: unit tests for the analysis tooling.

These cover the parts of the pipeline that everything else depends on being right:
the PE parser (used by every measurement in docs/), the forwarding-stub generator
(whose output must match the real header exactly), and the IID extraction that
the wrapper's QI routing is built on.

Runs with pytest if available, otherwise standalone:
    python3 tests/test_tools.py
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(ROOT, "components", "shader-bridge"))

VENDOR = os.environ.get("XTL_VENDOR", os.path.join(ROOT, "vendor"))
REF = os.path.join(VENDOR, "x64_dx12", "d3d12_xs.dll")
HEADER = os.path.join(VENDOR, "d3d12", "include", "d3d12.h")

have = os.path.exists(REF)
need = lambda: (not have) and (print("SKIP (artifacts not linked)") or True)


# ---------------------------------------------------------------- peinfo ----
def test_peinfo_exports_and_ordinals():
    if need():
        return
    from peinfo import PE
    pe = PE(REF)
    ex = pe.exports()
    names = {n: o for n, o, _ in ex["names"]}
    assert len(names) == 116, "expected 116 exports, got %d" % len(names)
    assert names["D3D12ZeroMemoryX"] == 1
    assert names["D3D12CreateDevice"] == 5
    assert names["InternalDriverCallX"] == 116
    assert sum(1 for n in names if n.startswith(("IID_", "DXGI_DEBUG_"))) == 93


def test_peinfo_iid_bytes_match_pc_header():
    """The load-bearing measurement of the whole design (E1).

    IID_ID3D12Device read out of the console runtime must equal the value the PC
    header declares. If this ever fails, forwarding is no longer valid.
    """
    if need():
        return
    from peinfo import PE
    pe = PE(REF)
    got = None
    for n, o, r in pe.exports()["names"]:
        if n == "IID_ID3D12Device":
            got = pe.b[pe.r2o(r):pe.r2o(r) + 16]
    assert got is not None, "IID_ID3D12Device not exported"
    d1, d2, d3 = struct.unpack_from("<IHH", got, 0)
    assert (hex(d1), hex(d2), hex(d3)) == ("0x189819f1", "0x1db6", "0x4b57")
    assert got[8:16] == bytes([0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7])

    txt = open(HEADER, encoding="utf-8", errors="replace").read()
    m = re.search(r"DEFINE_GUID\(IID_ID3D12Device\s*,\s*([^;]+)\);", txt)
    assert m, "IID_ID3D12Device not defined in the PC header"
    vals = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))]
    assert vals[0] == d1 and vals[1] == d2 and vals[2] == d3, "GUID differs from PC header"
    assert bytes(vals[3:11]) == got[8:16], "GUID tail differs from PC header"


def test_peinfo_loads_every_console_dll():
    if need():
        return
    from peinfo import PE
    d = os.path.join(VENDOR, "x64_dx12")
    expect = {
        "d3d12_xs.dll": 116, "xgs12_pc_xs.dll": 21, "xbsc_xs.dll": 43,
        "dxcompiler_xs.dll": 4, "xg_xs.dll": 27, "xdxrofflinebvh.dll": 119,
    }
    for f, n in expect.items():
        p = os.path.join(d, f)
        if not os.path.exists(p):
            continue
        assert len(PE(p).exports()["names"]) == n, f


# ----------------------------------------------------------- gen_wrappers ---
def test_gen_wrappers_matches_the_real_header():
    """The generator must produce exactly the live declarations.

    Counts are pinned: a drift here means the wrapper no longer covers the
    interface it claims to, which the compiler would only catch on recompile.
    """
    if not os.path.exists(HEADER):
        print("SKIP (artifacts not linked)")
        return
    import gen_wrappers as g
    ifaces = g.interfaces(g.load())
    body_dev, n_dev = g.emit(ifaces, "ID3D12Device4")
    body_cl, n_cl = g.emit(ifaces, "ID3D12GraphicsCommandList6")
    assert n_dev == 53, "ID3D12Device4 forwarding count changed: %d" % n_dev
    assert n_cl == 76, "ID3D12GraphicsCommandList6 forwarding count changed: %d" % n_cl
    # no half-parsed declarations may survive
    for body in (body_dev, body_cl):
        assert "= 0;" not in body, "generator emitted an unparsed declaration"
        for line in body.splitlines():
            if "STDMETHODCALLTYPE" in line and "override" not in line and "//" not in line:
                raise AssertionError("malformed generated line: " + line[:120])


def test_gen_wrappers_resolves_msvc_only_guards():
    """d3d12.h declares 23 methods twice behind `#if defined(_MSC_VER) || !defined(_WIN32)`.

    We compile with clang on Windows, so the #else branch is live. Both branches
    must never appear together, or the generated code declares the same method
    twice with different signatures.
    """
    if not os.path.exists(HEADER):
        print("SKIP (artifacts not linked)")
        return
    import gen_wrappers as g
    txt = g.load()
    assert txt.count("#if defined(_MSC_VER) || !defined(_WIN32)") >= 20
    ifaces = g.interfaces(txt)
    for name in ("ID3D12Device", "ID3D12GraphicsCommandList"):
        _base, methods = ifaces[name]
        seen = [m[1] for m in methods]
        dupes = {m for m in seen if seen.count(m) > 1}
        assert not dupes, "%s declares %s twice after guard resolution" % (name, dupes)


# ------------------------------------------------------------ wrapper IIDs --
def test_wrapper_console_iids_come_from_the_runtime():
    """The wrapper's PROVEN console IIDs must be the bytes the runtime exports.

    A CANDIDATE entry must never leak into src/iid_console.inc: only GUIDs that
    are actually exported may be used for QI routing.
    """
    if need():
        return
    from peinfo import PE
    inc = open(os.path.join(ROOT, "components", "extension-wrapper", "src",
                            "iid_console.inc"), encoding="utf-8").read()
    defined = dict(re.findall(r"const GUID (\w+) = \{0x([0-9a-f]+),", inc))
    assert "IID_ID3D12ConsDmaCommandList" in defined
    assert defined["IID_ID3D12ConsDmaCommandList"] == "30bc77b9"

    pe = PE(REF)
    real = {}
    for n, o, r in pe.exports()["names"]:
        if n.startswith("IID_"):
            real[n] = pe.b[pe.r2o(r):pe.r2o(r) + 16]
    assert "IID_ID3D12ConsDmaCommandList" in real, "runtime no longer exports this IID"
    d1 = struct.unpack_from("<I", real["IID_ID3D12ConsDmaCommandList"], 0)[0]
    assert d1 == int(defined["IID_ID3D12ConsDmaCommandList"], 16)


def test_extension_catalogue_is_present_and_complete():
    p = os.path.join(ROOT, "components", "extension-wrapper", "docs",
                     "console-extension-catalogue.tsv")
    assert os.path.exists(p), "catalogue missing"
    rows = [l for l in open(p, encoding="utf-8").read().splitlines() if l.strip()]
    methods = [r for r in rows if r.startswith("method\t")]
    enums = [r for r in rows if r.startswith("enum\t")]
    assert len(methods) >= 100, "expected >=100 recovered methods, got %d" % len(methods)
    assert len(enums) >= 100, "expected >=100 recovered symbols, got %d" % len(enums)
    for want in ("CreateCommandQueueX", "FillRawBuffer", "SetFrameIntervalX",
                 "OMSetFrontAndBackStencilRef"):
        assert any(want in m for m in methods), "catalogue lost %s" % want


# ------------------------------------------------------------------- runner --
def main():
    tests = [(n, f) for n, f in sorted(globals().items())
             if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("  ok   %s" % name)
        except Exception as ex:
            failed += 1
            print("  FAIL %s: %s" % (name, ex))
    print("%d tests, %d failures" % (len(tests), failed))
    return failed


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
