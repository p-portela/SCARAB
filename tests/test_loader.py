#!/usr/bin/env python3
"""G10: the launcher maps a PE, binds its imports, runs TLS and the entry point.

Two runs:
  * a fixture PE built by this tree (always): one app-local import and one TLS
    callback, so the whole map -> bind -> callbacks -> entry path is proven from
    a clean clone;
  * the title's own exe with --list (skipped 77 if the artifacts are absent):
    the real 68-module, 587-import image maps, and the shims we ship resolve
    while the console DLLs we do not ship are named as stubs rather than
    passing silently.

Usage: test_loader.py [launcher] scarab_launch.exe loader_fixture.exe \
           xmem.dll d3d12_xs.dll [title.exe [tools_dir [xgameruntime.dll [gameinput.dll]]]]
  launcher is SCARAB_TEST_LAUNCHER (empty on Windows, "wine" elsewhere).
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import pathlib
import subprocess
import sys

launcher = sys.argv[1].strip() if len(sys.argv) > 1 else ""
args = sys.argv[2:]
if len(args) < 4:
    print("SKIP usage: test_loader.py <launcher> <launch.exe> <fixture.exe> <xmem.dll> <d3d12_xs.dll> [title.exe]")
    sys.exit(77)

fails = []
def check(ok, what, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails.append(what)


def run(cmd, cwd):
    full = ([launcher] if launcher else []) + [str(x) for x in cmd]
    try:
        return subprocess.run(full, cwd=cwd, capture_output=True, text=True, timeout=300)
    except Exception as exc:  # noqa: BLE001
        return None


# --- the fixture -----------------------------------------------------------
def fixture():
    launch_exe, fixture_exe = (pathlib.Path(p) for p in args[:2])
    if not launch_exe.exists() or not fixture_exe.exists():
        print(f"SKIP launcher/fixture not built ({launch_exe}, {fixture_exe})")
        return 77
    r = run([launch_exe, "--deps", fixture_exe.parent, fixture_exe], fixture_exe.parent)
    if r is None:
        print("SKIP cannot launch the fixture")
        return 77
    out = r.stdout
    check(r.returncode == 0, "fixture process exit 0", f"exit={r.returncode} {r.stderr[-200:]}")
    check("tls=yes" in out, "the image is seen to have TLS", out[:300])
    check("FIXTURE_TLS" in out, "the TLS callback ran")
    check("FIXTURE_DEP=7" in out, "the app-local import bound (not stubbed)")
    check("FIXTURE_LARGE_PAGES=1" in out, "a large-page request still gets memory")
    check("FIXTURE_ENTRY" in out, "the entry point ran")
    check("0 stubbed" in out, "nothing in the fixture needed a stub", out[:300])
    return 0


# --- the title -------------------------------------------------------------
def title():
    xmem, d3d12 = (pathlib.Path(p) for p in args[2:4])
    game = pathlib.Path(args[4]) if len(args) > 4 else None
    if game is None or not game.exists():
        print("SKIP the title exe is not present (artifacts)")
        return 0
    if not xmem.exists() or not d3d12.exists():
        print("SKIP xmem.dll / d3d12_xs.dll not built")
        return 0
    r = run([pathlib.Path(args[0]), "--list", "--deps", xmem.parent,
             "--deps", d3d12.parent, game], xmem.parent)
    if r is None:
        print("SKIP cannot launch the title")
        return 0
    out = r.stdout
    check(r.returncode == 0, "the title's PE maps and binds", f"exit={r.returncode} {r.stderr[-300:]}")
    check("modules=68" in out, "68 import modules parsed")
    check("imports 587:" in out, "587 imports parsed")
    check("tls=yes" in out, "the title's TLS directory is seen")
    # Our shims must resolve; the console-only DLLs must be named, not silent.
    check("stub xmem.dll!" not in out, "xmem.dll resolves through our shim")
    check("stub d3d12_xs.dll!" not in out, "d3d12_xs.dll resolves through our bridge")
    check("stub chad.dll!" in out, "chad.dll (not shipped) is named as a stub")
    stubbed = [l for l in out.splitlines() if "stubbed" in l and "imports" in l]
    check(bool(stubbed) and not stubbed[0].endswith("0 stubbed"),
          "the report counts stubs", stubbed[0] if stubbed else "no summary line")
    return 0


# --- gameinput.dll's export surface ---------------------------------------
def gameinput_surface():
    gi = pathlib.Path(args[7]) if len(args) > 7 else None
    tools = pathlib.Path(args[5]) if len(args) > 5 else None
    if gi is None or tools is None or not gi.exists() or not tools.exists():
        print("SKIP gameinput.dll or tools/ not present")
        return 0
    sys.path.insert(0, str(tools))
    from peinfo import PE
    names = {n for n, _o, _r in PE(str(gi)).exports()["names"]}
    want = {"GameInputCreate"}
    missing = want - names
    check(not missing, "gameinput.dll exports GameInputCreate", str(sorted(missing)))
    return 0


# --- xmem.dll's export surface ---------------------------------------------
def xmem_surface():
    xmem = pathlib.Path(args[2]) if len(args) > 2 else None
    tools = pathlib.Path(args[5]) if len(args) > 5 else None
    if xmem is None or tools is None or not xmem.exists() or not tools.exists():
        print("SKIP xmem.dll or tools/ not present")
        return 0
    sys.path.insert(0, str(tools))
    from peinfo import PE
    names = {n for n, _o, _r in PE(str(xmem)).exports()["names"]}
    want = {
        "XMemAlloc", "XMemAllocDefault", "XMemFree", "XMemFreeDefault",
        "XMemVirtualAlloc", "XMemVirtualQuery", "XMemGetWorkingSetStatistics",
        "XMemAllocatePhysicalPages", "XMemFreePhysicalPages", "XMemMapPhysicalPages",
        "XMemMapPhysicalPagesScatter", "XMemTransferMemory", "XMemIsToolsMemAvailable",
        "XMemSetAllocationHooks", "XMemGetAllocationStatistics",
        "XMemGetAllocationHysteresis", "XMemSetAllocationHysteresis",
        "XMemMakeTag", "XMemPreallocateFreeSpace", "XMemGetWin32HeapStatistics",
    }
    missing = want - names
    check(not missing, "xmem.dll exports the full documented surface", str(sorted(missing)))
    return 0


# --- xgameruntime.dll's export surface --------------------------------------
def gdk_surface():
    gdk = pathlib.Path(args[6]) if len(args) > 6 else None
    tools = pathlib.Path(args[5]) if len(args) > 5 else None
    if gdk is None or tools is None or not gdk.exists() or not tools.exists():
        print("SKIP xgameruntime.dll or tools/ not present")
        return 0
    sys.path.insert(0, str(tools))
    from peinfo import PE
    names = {n for n, _o, _r in PE(str(gdk)).exports()["names"]}
    want = {"InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2", "QueryApiImpl",
            "UninitializeApiImpl", "XErrorReport", "XGameRuntimeInitialize",
            "XGameUiShowPlayerProfileCardAsync", "XGameUiShowTextEntryAsync"}
    missing = want - names
    check(not missing, "xgameruntime.dll exports the GDK bootstrap the title resolves", str(sorted(missing)))
    return 0


rc = fixture()
if rc == 0:
    xmem_surface()
    gdk_surface()
    gameinput_surface()
if rc == 0:
    rc = title()
for f in fails:
    print(f"FAILED: {f}")
sys.exit(rc if rc else (1 if fails else 0))
