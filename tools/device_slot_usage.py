#!/usr/bin/env python3
"""Measure which console D3D12 device methods a title actually calls, from its own code.

Why this exists
---------------
The console appends methods to the PC D3D12 interfaces, and the slot *order* of that
appended block is not recovered (docs/d3d12-xs-wrapper.md §4, "assumption #1"). The
cheapest way to attack an order assumption is not to guess harder but to read the
slots out of a real title: its vtable dispatch is the ABI, so a call site plus the
arguments it passes is direct evidence.

What it does
------------
1. finds the global the title stores its device in (the out-parameter of the
   D3D12XboxCreateDevice call), and the other globals that global is copied into;
2. disassembles .text;
3. follows each load of those globals through registers until an indirect call on the
   device is *proven* - `mov <glob>,%dev` then `mov (%dev),%vt` then `call *N(%vt)`;
4. reports the slot (N/8) per call site, with the call-site address and the function
   entry it sits in (.pdata), so each slot can be taken to a decompiler.

Objective output only: slot numbers, call sites, function entries. Naming a slot is
decompilation work, not this tool's job.

    python3 tools/device_slot_usage.py <title.exe> --tsv out.tsv --json out.json

Exits 77 (the suite's SKIP convention, as in G6/G7) when no disassembler is found, so
a machine without one stays meaningful.
"""
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Pedro Portela
import argparse
import bisect
import json
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peinfo import PE  # noqa: E402

REG64 = ("rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")

DISASSEMBLERS = (
    ("objdump", ["objdump", "-d"]),
    ("llvm-objdump", ["llvm-objdump", "--disassemble", "--no-show-raw-insn"]),
)


# ---------------------------------------------------------------------------
# pure helpers - unit-tested in tests/test_tools.py
# ---------------------------------------------------------------------------
def iat_slots(pe):
    """{slot_va: 'dll!name'} for every import, keyed by the IAT slot address."""
    rva, _ = pe.dd(1)
    out = {}
    if not rva:
        return out
    o = pe.r2o(rva)
    i = 0
    while True:
        ent = o + i * 20
        oft, _tds, _fc, nrva, fta = struct.unpack_from("<IIIII", pe.b, ent)
        if nrva == 0 and oft == 0 and fta == 0:
            break
        p = pe.r2o(nrva)
        dll = pe.b[p:pe.b.find(b"\0", p)].decode("latin1")
        thunk_rva = oft or fta
        thunk = pe.r2o(thunk_rva)
        k = 0
        while True:
            v = struct.unpack_from("<Q", pe.b, thunk + k * 8)[0]
            if v == 0:
                break
            if v >> 63:
                name = "#%d" % (v & 0xFFFF)
            else:
                po = pe.r2o(v & 0x7FFFFFFF)
                name = pe.b[po + 2:pe.b.find(b"\0", po + 2)].decode("latin1")
            out[fta + k * 8 + pe.image_base] = "%s!%s" % (dll, name)
            k += 1
            if k > 5000:
                break
        i += 1
        if i > 200:
            break
    return out


def pdata_functions(pe):
    """Sorted [(begin_rva, end_rva)] from .pdata RUNTIME_FUNCTION entries."""
    for name, va, vsz, raw, rsz in pe.sections:
        if name == ".pdata":
            out = []
            for o in range(raw, raw + rsz - 11, 12):
                beg, end, _un = struct.unpack_from("<III", pe.b, o)
                if beg:
                    out.append((beg, end))
            out.sort()
            return out
    return []


def containing_function(entries, rva):
    """The enclosing (begin_rva, size) for a code RVA, or None."""
    if not entries:
        return None
    begins = [e[0] for e in entries]
    i = bisect.bisect_right(begins, rva) - 1
    if i >= 0 and entries[i][0] <= rva < entries[i][1]:
        return entries[i][0], entries[i][1] - entries[i][0]
    return None


def rip_global(ins, comment):
    """A load/store of a global through rip-relative addressing: (reg, global_va).

    `mov 0x1234(%rip),%rax  # 0x1475df048` -> ('rax', 0x1475df048)
    """
    m = re.match(r"mov\s+0x[0-9a-f]+\(%rip\),%(\w+)", ins)
    c = re.search(r"# 0x([0-9a-f]+)", comment)
    if m and c:
        return m.group(1), int(c.group(1), 16)
    return None, None


def vtable_load(ins):
    """`mov (%rdev),%rvt` -> ('rdev', 'rvt')."""
    m = re.match(r"mov\s+\(%(\w+)\),%(\w+)", ins)
    return (m.group(1), m.group(2)) if m else (None, None)


def reg_copy(ins):
    """`mov %rsrc,%rdst` -> ('rsrc', 'rdst')."""
    m = re.match(r"mov\s+%(\w+),%(\w+)", ins)
    return (m.group(1), m.group(2)) if m else (None, None)


def slot_call(ins):
    """`call *0x428(%rvt)` -> (78, 'rvt'); `call *(%rvt)` -> (0, 'rvt')."""
    m = re.match(r"call\s+\*0x([0-9a-f]+)\(%(\w+)\)", ins)
    if m:
        return int(m.group(1), 16) // 8, m.group(2)
    m = re.match(r"call\s+\*\(%(\w+)\)", ins)
    if m:
        return 0, m.group(1)
    return None, None


def scan_device_calls(instructions, device_globals, window=14):
    """Device method calls, proven by register flow.

    `instructions` is an iterable of (addr, ins, comment); the scan is linear, which is
    all a straight-line call sequence needs. A device pointer that only travels through
    a struct field or a function argument is deliberately *not* followed: it cannot be
    proven locally, and a wrong slot number is worse than a missing one. A register that
    is written by anything else stops being the device, which is what keeps a stale
    value from producing a slot.
    """
    state = {}
    pending = []
    results = []
    for addr, ins, comment in instructions:
        handled = False
        reg, glob = rip_global(ins, comment)
        if glob is not None and glob in device_globals:
            state = {canon_reg(reg): "device"}
            pending.append({"ref": addr, "glob": glob, "slot": None, "at": None,
                            "left": window})
            continue
        srcreg, dstreg = vtable_load(ins)
        dst = canon_reg(dstreg)
        if srcreg is not None and dst and state.get(canon_reg(srcreg)) == "device":
            state[dst] = "vtable"
            handled = True
        else:
            srcreg, dstreg = reg_copy(ins)
            src, dst = canon_reg(srcreg), canon_reg(dstreg)
            if src in state and dst:
                state[dst] = state[src]
                handled = True
        if not handled:
            written = written_register(ins)
            if written:
                state.pop(written, None)
        slot, vtreg = slot_call(ins)
        if slot is not None and state.get(canon_reg(vtreg)) == "vtable":
            for ph in pending:
                if ph["slot"] is None:
                    ph["slot"] = slot
                    ph["at"] = addr
        for ph in pending:
            ph["left"] -= 1
        done = [ph for ph in pending if ph["left"] <= 0]
        results.extend(done)
        pending = [ph for ph in pending if ph["left"] > 0]
    results.extend(pending)
    for r in results:
        r.pop("left", None)
    return results


def indirect_targets(instructions, slot_va):
    """Every way a call can reach an import slot: the slot itself, or a `jmp [slot]`
    trampoline. A title built this way has one trampoline per import (378 of them, the
    console block at 0x145f3fcc0-0x145f401e0 in the reference title), so a call-site scan
    that only looks for `call *slot(%rip)` finds nothing at all."""
    targets = set()
    for addr, ins, comment in instructions:
        if not ins.startswith("jmp"):
            continue
        c = re.search(r"# 0x([0-9a-f]+)", comment)
        if c and int(c.group(1), 16) == slot_va:
            targets.add(addr)
    targets.add(slot_va)
    return targets


WRITES_DEST = re.compile(r"^(?:mov|movzx|movsx|movsxd|lea|add|sub|and|or|xor|imul|shl|shr|sar|"
                         r"neg|not|inc|dec|cmov\w+|set\w+|pop|movabs|vmovups|vmovdqu|vpxor|"
                         r"pxor|vxorps|xorps|movaps|movups|movdqa|vzeroupper|vxorps)\s+.*?,%(\w+)$")

# A write to the 32-bit half of a register ends the value in the 64-bit one too, so the
# two spellings have to map onto the same register or the tracking leaks a stale device.
REG_ALIAS = {}
for _r64, _rest in (("rax", "eax"), ("rbx", "ebx"), ("rcx", "ecx"), ("rdx", "edx"),
                    ("rsi", "esi"), ("rdi", "edi"), ("rbp", "ebp"), ("rsp", "esp")):
    REG_ALIAS[_rest] = _r64
for _n in range(8, 16):
    REG_ALIAS["r%dd" % _n] = "r%d" % _n
    REG_ALIAS["r%dw" % _n] = "r%d" % _n
    REG_ALIAS["r%db" % _n] = "r%d" % _n


def canon_reg(reg):
    """Map any spelling of a register onto one name, or None if it is not a GPR."""
    if reg in REG64:
        return reg
    return REG_ALIAS.get(reg)


def written_register(ins):
    """The register an instruction writes, when it can be told from the last operand."""
    if ins.startswith("call"):
        return None          # clobbers per ABI; the local scan does not need to model it
    m = WRITES_DEST.match(ins)
    return canon_reg(m.group(1)) if m else None


def grow_device_family(instructions, out_global):
    """Globals the out-pointer value is stored into, once per subsystem.

    Register-tracked like the call scan: a register holds the device only until
    something else is written to it, which is what stops the family from swallowing
    every global in the image.
    """
    family = {out_global}
    state = {}
    for _addr, ins, comment in instructions:
        reg, glob = rip_global(ins, comment)
        if glob is not None and reg is not None:
            state[reg] = "device" if glob == out_global else None
            continue
        m = re.match(r"mov\s+%(\w+),0x[0-9a-f]+\(%rip\)", ins)
        c = re.search(r"# 0x([0-9a-f]+)", comment)
        if m and c and state.get(m.group(1)) == "device":
            family.add(int(c.group(1), 16))
            continue
        dst = written_register(ins)
        if dst:
            state.pop(dst, None)
    return family


def find_device_globals(instructions, create_targets):
    """The device global and the globals it is copied into.

    The console create call is `D3D12XboxCreateDevice(0, &params, &riid, &out)`, so the
    out-pointer arrives in r9 as an `lea` of a global shortly before the call. Any global
    the value of that pointer is later stored into joins the family: titles keep one copy
    per subsystem.
    """
    out_global = None
    last_lea = {}
    for _addr, ins, comment in instructions:
        m = re.match(r"lea\s+0x[0-9a-f]+\(%rip\),%(\w+)", ins)
        c = re.search(r"# 0x([0-9a-f]+)", comment)
        if m and c:
            last_lea[m.group(1)] = int(c.group(1), 16)
        if not ins.startswith("call"):
            continue
        direct = re.match(r"call\s+0x([0-9a-f]+)", ins)
        c2 = re.search(r"# 0x([0-9a-f]+)", comment)
        reached = None
        if direct and int(direct.group(1), 16) in create_targets:
            reached = int(direct.group(1), 16)
        elif c2 and int(c2.group(1), 16) in create_targets:
            reached = int(c2.group(1), 16)
        if reached is not None and "r9" in last_lea:
            out_global = last_lea["r9"]
    if out_global is None:
        return set()
    return grow_device_family(instructions, out_global)


# ---------------------------------------------------------------------------
# disassembly
# ---------------------------------------------------------------------------
LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(.*)$")


def pick_disassembler(explicit=None):
    for name, argv in DISASSEMBLERS:
        if explicit and explicit != name:
            continue
        if shutil.which(argv[0]):
            return name, argv
    return None, None


def disassemble(path, lo, hi, argv):
    """Yield (addr, instruction, comment) for a VA range."""
    p = subprocess.Popen(argv + ["--start-address=0x%x" % lo, "--stop-address=0x%x" % hi, path],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    assert p.stdout is not None
    for line in p.stdout:
        m = LINE.match(line)
        if m:
            yield int(m.group(1), 16), m.group(3).split("#")[0].strip(), m.group(3)
    p.wait()


def text_range(pe):
    for name, va, _vsz, _raw, rsz in pe.sections:
        if name == ".text":
            return pe.image_base + va, pe.image_base + va + rsz
    raise SystemExit("no .text section")


def main():
    ap = argparse.ArgumentParser(
        description="Measure the console D3D12 device method slots a title calls.")
    ap.add_argument("binary", help="the title executable")
    ap.add_argument("--json")
    ap.add_argument("--tsv")
    ap.add_argument("--disassembler", choices=[n for n, _ in DISASSEMBLERS])
    ap.add_argument("--device-global", action="append", default=[],
                    help="0xADDR; omit to discover it from the create call")
    ap.add_argument("--window", type=int, default=14)
    args = ap.parse_args()

    name, argv = pick_disassembler(args.disassembler)
    if not argv:
        print("no disassembler found (objdump or llvm-objdump); SKIP", file=sys.stderr)
        return 77

    pe = PE(args.binary)
    slots = iat_slots(pe)
    lo, hi = text_range(pe)
    instructions = list(disassemble(args.binary, lo, hi, argv))

    if args.device_global:
        device_globals = {int(g, 16) for g in args.device_global}
    else:
        create_slot = next((va for va, n in slots.items()
                            if n.endswith("!D3D12XboxCreateDevice")), None)
        if create_slot is None:
            print("this binary does not import D3D12XboxCreateDevice", file=sys.stderr)
            return 77
        device_globals = find_device_globals(instructions,
                                             indirect_targets(instructions, create_slot))
    if not device_globals:
        print("no device global found; pass --device-global", file=sys.stderr)
        return 1

    calls = scan_device_calls(instructions, device_globals, window=args.window)
    proven = [c for c in calls if c["slot"] is not None]
    # one row per call site: two loads of the same global in one sequence resolve to the
    # same call, and counting it twice would inflate the slot's site count
    seen_keys = set()
    unique = []
    for c in proven:
        key = (c["slot"], c["at"])
        if key not in seen_keys:
            seen_keys.add(key)
            unique.append(c)
    proven = unique
    entries = pdata_functions(pe)

    for c in proven:
        fn = containing_function(entries, c["at"] - pe.image_base)
        c["function"] = fn[0] + pe.image_base if fn else None
        c["function_size"] = fn[1] if fn else None
        c["ref_hex"] = "0x%x" % c["ref"]
        c["glob_hex"] = "0x%x" % c["glob"]
        c["at_hex"] = "0x%x" % c["at"]
        c["function_hex"] = "0x%x" % c["function"] if c["function"] else None

    hist = {}
    for c in proven:
        hist[c["slot"]] = hist.get(c["slot"], 0) + 1

    print("%s: %d device globals, %d loads, %d proven calls"
          % (os.path.basename(args.binary), len(device_globals), len(calls), len(proven)))
    for slot in sorted(hist):
        print("   slot %4d  %d" % (slot, hist[slot]))

    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"binary": os.path.basename(args.binary),
                       "device_globals": ["0x%x" % g for g in sorted(device_globals)],
                       "calls": proven}, fh, indent=1)
    if args.tsv:
        import hashlib
        with open(args.binary, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()
        with open(args.tsv, "w") as fh:
            fh.write("# device method slot usage, measured from a title's own code\n")
            fh.write("# source: %s sha256:%s\n" % (os.path.basename(args.binary), digest))
            fh.write("# tool: tools/device_slot_usage.py (disassembler: %s)\n" % name)
            fh.write("# method: the device pointer's globals are followed through registers "
                     "until an indirect\n")
            fh.write("#         call on the device is proven; a slot with no proof is absent, "
                     "not guessed\n")
            fh.write("# grade: objective - slot numbers and call sites only. Naming a slot is\n")
            fh.write("#        decompilation work and is recorded separately.\n")
            fh.write("slot\tcall_site\tfunction_entry\tvia_global\n")
            for c in sorted(proven, key=lambda c: (c["slot"], c["at"])):
                fh.write("%d\t%s\t%s\t%s\n"
                         % (c["slot"], c["at_hex"], c["function_hex"] or "-", c["glob_hex"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
