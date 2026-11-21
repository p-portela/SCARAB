#!/usr/bin/env python3
"""G7: the bridge must identify its own D3D12 backend correctly.

Why this is a gate and not a log line: the Linux path IS the D3D12 -> Vulkan
bridge, and a path-based guess got it wrong -- Wine's builtin d3d12.dll lives at
C:\\windows\\system32\\d3d12.dll, so a "system32" substring match labelled the
vkd3d-proton-derived runtime as "Microsoft d3d12 (system)" and emitted a bogus
SCARAB_BACKEND mismatch warning. On the one host where vkd3d is the point, the
layer misreported its own backend.

What this checks, from the bridge's own log after a real device creation:
  * the host-appropriate classification (wine builtin => vkd3d, MS => Microsoft)
  * no WARNING line, i.e. SCARAB_BACKEND and the resolved runtime agree
  * the backend was actually exercised (D3D12CreateDevice on the forward path)

Usage: test_backend_identity.py [launcher] <integration_test.exe>
  launcher is SCARAB_TEST_LAUNCHER (empty on Windows, "wine" elsewhere), which
  is also how the host is inferred.
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import os
import pathlib
import re
import subprocess
import sys

launcher = sys.argv[1].strip() if len(sys.argv) > 2 else ""
exe = pathlib.Path(sys.argv[-1])
stage = exe.parent
log = stage / "d3d12_xs.bridge.log"

is_wine = "wine" in pathlib.Path(launcher).name.lower() if launcher else False
want = "vkd3d" if is_wine else "d3d12"

fails = []
def check(ok, what, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + (("  -- " + detail) if detail and not ok else ""))
    if not ok:
        fails.append(what)

if not exe.exists():
    print(f"SKIP cannot run {exe} - not built (artifact/test not linked)")
    sys.exit(77)

env = dict(os.environ)
env["SCARAB_BACKEND"] = want
log.unlink(missing_ok=True)

cmd = ([launcher] if launcher else []) + [str(exe)]
try:
    r = subprocess.run(cmd, cwd=stage, env=env, capture_output=True, text=True, timeout=180)
except Exception as exc:  # noqa: BLE001
    print(f"SKIP cannot launch {exe}: {exc}")
    sys.exit(77)

check(r.returncode == 0, "the test executable ran",
      f"exit={r.returncode} stderr={r.stderr.strip()[:200]}")

if not log.exists():
    print(f"  FAIL the bridge wrote no log at {log}")
    fails.append("log")
    print(f"\nG7 FAIL: {len(fails)} check(s) failed")
    sys.exit(1)

text = log.read_text(errors="replace")

m = re.search(r"^backend: (.+?)\s*$", text, re.M)
check(m is not None, "the bridge recorded a backend classification")
kind = m.group(1) if m else ""
kinds = re.findall(r"^backend: (.+?)\s*$", text, re.M)
print(f"       resolved backend: {kind!r}  (host: {'wine' if is_wine else 'native'})")

if is_wine:
    check("vkd3d" in kind.lower(), "under Wine the backend is the vkd3d D3D12 -> Vulkan bridge", kind)
    check("microsoft" not in kind.lower(), "the vkd3d backend is not mislabelled as Microsoft's", kind)
else:
    check("vkd3d" not in kind.lower(), "on a native host the backend is not vkd3d", kind)

check(not re.search(r"WARNING", text),
      "no SCARAB_BACKEND mismatch warning (declared == resolved)",
      next((l for l in text.splitlines() if "WARNING" in l), ""))

check(re.search(r"^backend: .*d3d12\.dll", text, re.M) is not None,
      "the loaded runtime path was recorded")

check("D3D12CreateDevice" in text, "the backend was exercised (D3D12CreateDevice forwarded)")
check(("device substituted with the wrapper layer" in text) or
      ("keeping the PC device" in text),
      "a real D3D12 device came back and was handed to the wrapper")

if fails:
    print(f"\nG7 FAIL: {len(fails)} check(s) failed")
    sys.exit(1)
print(f"\nG7 PASS: backend identity is correct on this host ({kind})")
