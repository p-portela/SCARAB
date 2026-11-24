#!/usr/bin/env python3
"""Diff our walk's instruction boundaries against the vendor's, and name the cause.

The vendor's boundaries are recovered by prefix probing (see below): feed it the
first j words and count the instructions it prints. n(j) rises only when the
prefix completes an instruction, so the rises are its boundaries -- the one fact
its disassembly text does not expose.

Every boundary our walker has and the vendor does not is a place we split an
instruction the vendor keeps whole. Those are the drift sources. They are grouped
by the words they sit on, so the rule falls out of the data.

  tools/xbsc_boundaries.py <blob.rdna2> [blob2 ...] [--limit N]
"""
import os
import re
import struct
import subprocess
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "components", "shader-bridge"))
import rdna2_classify as rc  # noqa: E402

WINE = os.environ.get("SCARAB_WINE", os.path.expanduser("~/opt/wine/bin/wine"))
DLL = os.path.join(os.environ.get("SCARAB_VENDOR", os.path.join(ROOT, "vendor")),
                   "x64_dx12", "xbsc_xs.dll")
EXE = os.environ.get("SCARAB_XBSC_EXE", "/tmp/xbsc-corpus/xbsc_probe.exe")
REC = re.compile(r"^@@ .*?/(?P<name>[^/]+)\.rdna2 rc=(\d+) g=(-?\d+) len=(\d+)$(.*?)^@@END$",
                 re.M | re.S)


def vendor_boundaries(words, workdir):
    """Return the set of word indices at which the vendor starts an instruction."""
    os.makedirs(workdir, exist_ok=True)
    man = []
    for j in range(1, len(words) + 1):
        p = os.path.join(workdir, "p%05d.rdna2" % j)
        with open(p, "wb") as fh:
            fh.write(struct.pack("<%dI" % j, *words[:j]))
        man.append("Z:" + p)
    mpath = os.path.join(workdir, "manifest.txt")
    with open(mpath, "w") as fh:
        fh.write("\n".join(man) + "\n")
    env = dict(os.environ, WINEDEBUG="-all",
               WINEPREFIX=os.environ.get("WINEPREFIX", os.path.expanduser("~/.wine-scarab")))
    out = subprocess.run([WINE, EXE, "Z:" + DLL, "Z:" + mpath, "batch"],
                         capture_output=True, text=True, env=env).stdout
    counts, last = {}, {}
    for m in re.finditer(r"^@@ .*?p(\d+)\.rdna2 rc=(\d+) g=(-?\d+) len=(\d+)$(.*?)^@@END$",
                         out, re.M | re.S):
        body = m.group(5)
        lines = [l.strip() for l in body.splitlines()
                 if l.strip() and l.strip() not in ("shader main", "end")
                 and not l.strip().startswith(("asic(", "type("))
                 and not re.match(r"^label_[0-9A-Fa-f]+:$", l.strip())]
        j = int(m.group(1))
        counts[j] = len(lines)
        last[j] = lines[-1] if lines else ""
    bounds, prev = {0}, 0
    for j in range(1, len(words) + 1):
        n = counts.get(j)
        if n is not None and n > prev:
            # The instruction that completed at word j-1 ends here, so the next
            # instruction (if any) starts at word j. The final j is the end of
            # the stream, not the start of an instruction.
            #
            # Exception: the vendor decodes an instruction with a literal operand
            # as soon as its non-literal words are present, printing the missing
            # literal as 0x00000000. That makes the literal word look like a new
            # instruction start. When the just-printed line carries an 8-digit
            # hex literal, it still consumes word j, so the next start is j+1.
            nxt = j
            if re.search(r"0x[0-9A-Fa-f]{8}", last.get(j, "")) and j + 1 < len(words):
                nxt = j + 1
            if nxt < len(words):
                bounds.add(nxt)
            prev = n
    return bounds


def classify(words, i, ours_fams):
    """Why might the vendor keep words[i-1] and words[i] in one instruction?"""
    prev_fam = rc.family(words[i - 1]) if i else "?"
    fam = rc.family(words[i])
    low = words[i - 1] & 0xFF
    if low == 0xFF and prev_fam in rc.LITERAL_FAMS:
        return "literal dword after %s (SRC0=0xFF)" % prev_fam
    if prev_fam == "VINTRP":
        return "second VINTRP dword"
    if prev_fam in ("VOP3", "VOP3P"):
        return "third dword after %s (VOP3 literal)" % prev_fam
    if prev_fam in ("SMEM", "MUBUF", "MTBUF", "MIMG", "FLAT", "DS", "EXP"):
        return "third dword after %s" % prev_fam
    return "unexplained: prev=%s(0x%02x) this=%s" % (prev_fam, low, fam)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    limit = None
    for a in sys.argv[1:]:
        if a.startswith("--limit"):
            limit = int(a.split("=")[1]) if "=" in a else None
    causes = Counter()
    totals = Counter()
    per_blob = []
    for path in args[:limit or len(args)]:
        data = open(path, "rb").read()
        words = list(struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))
        ours = set(i for i, _n, _f in rc.walk(words))
        tag = re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.basename(path))[:40]
        tag = tag.rstrip(".") or "blob"   # Wine strips a trailing dot from a path
        vend = vendor_boundaries(words, "/tmp/xbsc-bnd/" + tag)
        ours_only = sorted(ours - vend)
        vend_only = sorted(vend - ours)
        for i in ours_only:
            causes[classify(words, i, None)] += 1
        totals["ours"] += len(ours)
        totals["vendor"] += len(vend)
        totals["ours_only"] += len(ours_only)
        totals["vend_only"] += len(vend_only)
        per_blob.append((os.path.basename(path)[:52], len(ours), len(vend),
                         len(ours_only), len(vend_only)))
        print("%-54s ours=%-5d vendor=%-5d ours_only=%-4d vendor_only=%-4d"
              % per_blob[-1])
    print()
    print("totals:", dict(totals))
    print()
    print("causes of our extra boundaries (we split, vendor doesn't):")
    for c, n in causes.most_common(20):
        print("  %-52s %5d" % (c, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
