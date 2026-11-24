#!/usr/bin/env python3
"""G5: unit tests for the analysis tooling.

These cover the parts of the pipeline that everything else depends on being right:
the PE parser (used by every measurement in docs/), the forwarding-stub generator
(whose output must match the real header exactly), and the IID extraction that
the wrapper's QI routing is built on.

Runs with pytest if available, otherwise standalone:
    python3 tests/test_tools.py
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(ROOT, "components", "shader-bridge"))

VENDOR = os.environ.get("SCARAB_VENDOR", os.path.join(ROOT, "vendor"))
REF = os.path.join(VENDOR, "x64_dx12", "d3d12_xs.dll")
SHARED_REF = os.path.join(VENDOR, "d3d12", "os", "d3d12.dll")
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
    from gen_def import load_mapping
    inc = open(os.path.join(ROOT, "components", "extension-wrapper", "src",
                            "iid_console.inc"), encoding="utf-8").read()
    defined = dict(re.findall(r"const GUID (\w+) = \{0x([0-9a-f]+),", inc))
    assert "IID_ID3D12ConsDmaCommandList" in defined
    assert defined["IID_ID3D12ConsDmaCommandList"] == "30bc77b9"

    # Our source symbols deliberately differ from the vendor's export names (the
    # .def aliases them, see tools/gen_def.py). Map exported name -> source symbol
    # with the one shared definition so the two can never drift apart.
    pe = PE(REF)
    mapping = load_mapping(REF, SHARED_REF)
    real = {}
    for n, o, r in pe.exports()["names"]:
        if n.startswith("IID_"):
            real[mapping[n]] = pe.b[pe.r2o(r):pe.r2o(r) + 16]
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


# --------------------------------------------------------------- repo hygiene --
def _git(*args):
    import subprocess
    return subprocess.run(["git", "-C", ROOT] + list(args),
                          capture_output=True, text=True)


def test_repo_hygiene_no_artifacts_tracked():
    """No artefact may ever enter the index.

    Regression guard with a real history: the first .gitignore used `vendor/`, a
    directory pattern, which does NOT match a symbolic link - so `git add -A`
    committed the vendor symlink itself. Only an ad-hoc check caught it, which is
    why this test exists.
    """
    if not os.path.isdir(os.path.join(ROOT, ".git")):
        print("SKIP (not a git checkout)")
        return
    tracked = _git("ls-files").stdout.split()
    bad = [p for p in tracked
           if re.match(r"^(vendor|corpora|build)(/|$)", p)
           or p.endswith((".dll", ".exe", ".log", ".obj", ".lib", ".pdb"))
           or os.path.basename(p) in ("gen_device.inc", "gen_clist.inc")]
    assert not bad, "artefacts are tracked: %s" % bad[:5]


def test_repo_hygiene_ignore_rules_cover_the_symlinks():
    """The ignore rules must match the links, not just hypothetical directories.

    `vendor` is a symlink on a Linux checkout, so a `vendor/` pattern silently
    fails to match it. --no-index because a tracked path is reported as
    not-ignored, which would mask exactly the failure being guarded against.
    """
    if not os.path.isdir(os.path.join(ROOT, ".git")):
        print("SKIP (not a git checkout)")
        return
    for name in ("vendor", "corpora"):
        r = _git("check-ignore", "-q", "--no-index", name)
        assert r.returncode == 0, "%s is not ignored (rc=%d)" % (name, r.returncode)
    for gen in ("gen_device.inc", "gen_clist.inc"):
        r = _git("check-ignore", "-q", "--no-index", gen)
        assert r.returncode == 0, "%s is not ignored" % gen


def test_platform_module_discriminates_on_the_host_not_the_target():
    """WIN32 is true when cross-compiling *to* Windows, so it identifies the
    target. Using it to decide how to launch a test executable silently runs a PE
    through the shell. CMAKE_CROSSCOMPILING is the host question.
    """
    p = os.path.join(ROOT, "cmake", "scarab-platform.cmake")
    txt = open(p, encoding="utf-8").read()
    assert "CMAKE_CROSSCOMPILING" in txt, "platform module does not test the host"
    assert not re.search(r"^if\(WIN32\)", txt, re.M), \
        "platform module branches on WIN32, which describes the target"
    assert "find_package(Python3" in txt, \
        "python must be resolved, not assumed to be `python3` (absent on Windows)"


# ------------------------------------------------- device_slot_usage.py -----
def _ins(addr, ins, comment=""):
    return (addr, ins, comment)


def _dev_ins(addr, ins, target):
    """An objdump-style comment naming a global. Built by concatenation on purpose: the
    `(%rip)` in the instruction text is itself a format conversion, so `%`-formatting an
    instruction string silently eats the next argument."""
    return (addr, ins, ins + "  # " + hex(target))


def test_slot_usage_parsers():
    """The tool reads objdump text; a parser that is wrong reports slots that are wrong,
    so each form it depends on is pinned here."""
    import device_slot_usage as dsu

    assert dsu.rip_global("mov 0x5166a6d(%rip),%rax", "mov  0x5166a6d(%rip),%rax        # 0x1475df048") \
        == ("rax", 0x1475df048)
    assert dsu.rip_global("mov (%rcx),%rax", "mov (%rcx),%rax") == (None, None)
    assert dsu.vtable_load("mov (%rcx),%rax") == ("rcx", "rax")
    assert dsu.reg_copy("mov %rcx,%rsi") == ("rcx", "rsi")
    assert dsu.slot_call("call   *0x428(%rax)") == (133, "rax")
    assert dsu.slot_call("call   *(%rax)") == (0, "rax")
    assert dsu.slot_call("call   0x144e9b660") == (None, None)
    assert dsu.written_register("mov %rax,%rdx") == "rdx"
    assert dsu.written_register("xor %r8d,%r8d") == "r8", "a 32-bit write clears the 64-bit reg"
    assert dsu.written_register("call   *0x428(%rax)") is None
    assert dsu.canon_reg("eax") == "rax" and dsu.canon_reg("r9d") == "r9"


def test_slot_usage_proves_only_register_tracked_calls():
    """A call is attributed to the device only when the receiver provably came from the
    device pointer in the same straight line. A pointer that goes into a struct field
    must produce no slot at all - a wrong slot is worse than a missing one."""
    import device_slot_usage as dsu

    dev = 0x1475df048
    proven = [
        _dev_ins(0x1000, "mov 0x1234(%rip),%rcx", dev),
        _ins(0x1007, "mov (%rcx),%rax", "mov (%rcx),%rax"),
        _ins(0x100a, "call *0x270(%rax)", "call   *0x270(%rax)"),
    ]
    got = dsu.scan_device_calls(proven, {dev})
    assert [c["slot"] for c in got] == [78], got

    # through another register, and through a copy of the device pointer
    indirect = [
        _dev_ins(0x2000, "mov 0x1234(%rip),%rax", dev),
        _ins(0x2007, "mov %rax,%rcx", "mov %rax,%rcx"),
        _ins(0x200a, "mov (%rcx),%rdx", "mov (%rcx),%rdx"),
        _ins(0x200d, "call *0x3b8(%rdx)", "call   *0x3b8(%rdx)"),
    ]
    assert [c["slot"] for c in dsu.scan_device_calls(indirect, {dev})] == [119]

    # the device pointer stored into a struct field, then a call on the field: unknown
    unproven = [
        _dev_ins(0x3000, "mov 0x1234(%rip),%rax", dev),
        _ins(0x3007, "mov %rax,0x18(%rsi)", "mov %rax,0x18(%rsi)"),
        _ins(0x300b, "mov (%rsi),%rax", "mov (%rsi),%rax"),
        _ins(0x300e, "call *0x420(%rax)", "call   *0x420(%rax)"),
    ]
    assert [c["slot"] for c in dsu.scan_device_calls(unproven, {dev})] == [None]

    # a stale register: the vtable register is overwritten before the call, so no slot
    clobbered = [
        _dev_ins(0x4000, "mov 0x1234(%rip),%rcx", dev),
        _ins(0x4007, "mov (%rcx),%rax", "mov (%rcx),%rax"),
        _ins(0x400a, "mov $0x1,%rax", "mov    $0x1,%rax"),
        _ins(0x4011, "call *0x270(%rax)", "call   *0x270(%rax)"),
    ]
    assert [c["slot"] for c in dsu.scan_device_calls(clobbered, {dev})] == [None]


def test_slot_usage_function_lookup_and_family_growth():
    """Slot -> function entry is what makes a slot actionable, and family growth is what
    finds the per-subsystem copies of the device pointer without swallowing the image."""
    import device_slot_usage as dsu

    entries = [(0x1000, 0x1100), (0x2000, 0x2100)]
    assert dsu.containing_function(entries, 0x1050) == (0x1000, 0x100)
    assert dsu.containing_function(entries, 0x1100) is None      # end is exclusive
    assert dsu.containing_function(entries, 0x1500) is None

    dev, copy = 0x1475df048, 0x1475df000
    stream = [
        _dev_ins(0x5000, "mov 0x1234(%rip),%rax", dev),
        _dev_ins(0x5007, "mov %rax,0x5678(%rip)", copy),
        _ins(0x500e, "mov $0x2,%rax", "mov    $0x2,%rax"),
        _dev_ins(0x5015, "mov %rax,0x9abc(%rip)", 0xdeadbeef),
    ]
    assert dsu.grow_device_family(stream, dev) == {dev, copy}


def test_measured_device_slots_are_committed():
    """The measurement the wrapper's slot evidence rests on is committed next to the
    catalogue, with its provenance, so a reader can check the numbers the docs cite
    instead of taking them on trust."""
    tsv = os.path.join(ROOT, "components", "extension-wrapper", "docs",
                       "device-slot-measurements.tsv")
    assert os.path.exists(tsv), "missing %s" % tsv
    lines = open(tsv, encoding="utf-8").read().splitlines()
    head = [l for l in lines if l.startswith("#")]
    assert any("sha256" in l for l in head), "the measurement does not name its input"
    assert any("device_slot_usage" in l for l in head), "the measurement does not name its tool"
    rows = [l.split("\t") for l in lines if l and not l.startswith("#")]
    assert rows[0] == ["slot", "call_site", "function_entry", "via_global"], rows[0]
    slots = {}
    for slot, call_site, fn, glob in rows[1:]:
        assert call_site.startswith("0x") and glob.startswith("0x")
        assert fn == "-" or fn.startswith("0x")
        slots.setdefault(int(slot), set()).add(call_site)
    # The frame-pacing trio at 78/79/80 is the anchor that makes the rest readable:
    # its order is measured, and it fixes where the console's appended block begins.
    for slot in (78, 79, 80):
        assert slot in slots, "the frame-trio anchor at slot %d is missing" % slot
    assert slots[78] != slots[79] and slots[79] != slots[80], "anchors share a call site"


def test_measured_device_slots_tool_is_wired_into_the_repo():
    """The tool must run with no hidden dependency and skip (77) rather than lie when the
    host has no disassembler."""
    tool = os.path.join(TOOLS, "device_slot_usage.py")
    assert os.path.exists(tool)
    txt = open(tool, encoding="utf-8").read()
    assert "return 77" in txt, "the tool does not use the suite's SKIP convention"
    assert "peinfo import PE" in txt, "the tool should reuse the repo's PE parser"
    import subprocess
    r = subprocess.run([sys.executable, tool, "--help"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


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
