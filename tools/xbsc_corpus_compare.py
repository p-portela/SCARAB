#!/usr/bin/env python3
"""Compare the vendor disassembler's output against our own walker, corpus-wide.

The question this answers (docs/isa/shader-isa-notes.md section 4): are the sweep
failures a wrong opcode table, or a misaligned stream walk?

It needs no build and no GPU: "ours" is rdna2_classify's length-aware walker, and
"the vendor's" is what tools/xbsc_corpus.sh captured from XBSC_DisassembleRawShader.
The comparison is deliberately name-independent where it can be -- instruction
count and the word position of s_endpgm are the same fact on both sides -- so a
naming-style difference cannot be mistaken for a decode difference.

  tools/xbsc_corpus_compare.py <vendor.txt> [corpus-root]
"""
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "components", "shader-bridge"))
import rdna2_classify as rc  # noqa: E402

HEADER = re.compile(r"^@@ (?P<path>.+?) rc=(?P<rc>-?\d+) g=(?P<g>-?\d+) len=(?P<len>\d+)$")
SKIP = ("shader main", "end")
# vendor spellings that mean "this word is not a valid instruction"
INVALID = ("invalid", "unknown", "illegal")


def load_vendor(path):
    """Yield (file_path, rc, [mnemonic, ...], text) per record."""
    with open(path, "r", errors="replace") as fh:
        cur = None
        for raw in fh:
            line = raw.rstrip("\r\n")
            m = HEADER.match(line)
            if m:
                cur = {"path": m.group("path"), "rc": int(m.group("rc")), "text": []}
                continue
            if line == "@@END":
                if cur is not None:
                    yield cur
                cur = None
                continue
            if cur is not None:
                cur["text"].append(line)


def mnemonics(text):
    out = []
    for ln in text:
        s = ln.strip()
        if not s or s in SKIP or s.startswith(("asic(", "type(")):
            continue
        out.append(s.split()[0])
    return out


def our_walk(posix_path):
    """Our decode: the length-aware walker. Returns (instrs, words)."""
    with open(posix_path, "rb") as fh:
        blob = fh.read()
    words = list(struct.unpack("<%dI" % (len(blob) // 4), blob[: len(blob) // 4 * 4]))
    return list(rc.walk(words)), words


def norm(name):
    return name.strip().lower().replace("_", "")


def stage_of(path):
    """VS/PS/CS from the shader name; falls back to the leading token."""
    n = os.path.basename(path)
    m = re.search(r"(?:^|_)(VS|PS|CS|GS|HS|DS)(?:_|$)", n)
    return m.group(1) if m else n.split("_", 1)[0][:6]


def main():
    vendor = sys.argv[1]

    buckets = Counter()
    per_stage = defaultdict(Counter)
    align_examples = defaultdict(list)
    endpgm_pos_ours = Counter()
    coverage_short = 0

    total = 0
    for rec in load_vendor(vendor):
        total += 1
        posix = rec["path"]
        if posix.startswith("Z:"):
            posix = posix[2:]
        vm = mnemonics(rec["text"])

        try:
            ours, words = our_walk(posix)
        except OSError:
            buckets["unreadable"] += 1
            continue

        stage = stage_of(posix)

        # our view of the same facts, taken from the raw words
        epgm_word = words.index(0xBF810000) if 0xBF810000 in words else None
        covered = sum(n for _i, n, _f in ours)
        if covered != len(words):
            coverage_short += 1
        ours_before_epgm = sum(1 for i, _n, _f in ours if epgm_word is not None and i < epgm_word)

        v_epgm = next((i for i, m in enumerate(vm) if m == "s_endpgm"), None)
        v_invalid = sum(1 for m in vm if any(bad in m.lower() for bad in INVALID))
        v_codeend = sum(1 for m in vm if m == "s_code_end")

        if rec["rc"] != 0 or not vm:
            b = "vendor_no_output"
        elif len(vm) != len(ours):
            b = "count_differs"
        elif v_epgm is not None and epgm_word is not None and v_epgm != ours_before_epgm:
            b = "endpgm_position_differs"
        elif v_invalid:
            b = "ALIGNED_but_vendor_flags_invalid"
        else:
            b = "aligned"
        buckets[b] += 1
        per_stage[stage][b] += 1

        if b not in ("aligned",) and len(align_examples[b]) < 6:
            align_examples[b].append(
                (os.path.basename(posix)[:52], len(ours), len(vm),
                 ours_before_epgm, v_epgm, v_invalid, v_codeend, epgm_word))

        if epgm_word is not None and ours_before_epgm:
            endpgm_pos_ours[bool(v_epgm == ours_before_epgm)] += 1

    print("records compared: %d" % total)
    print()
    print("buckets:")
    for b, c in buckets.most_common():
        print("  %-34s %6d  %5.1f%%" % (b, c, 100.0 * c / max(total, 1)))
    print()
    print("our walker's word coverage exact: %d of %d" % (total - coverage_short, total))
    print("s_endpgm position agrees (vendor vs our walk): %s"
          % dict(endpgm_pos_ours))
    print("(mnemonic agreement is not reported: the two walks disagree on boundaries, so")
    print(" comparing item k of one against item k of the other would measure the drift,")
    print(" not the names. Fit the boundaries first.)")
    print()
    print("per stage:")
    for st, c in sorted(per_stage.items()):
        print("  %-6s %s" % (st, dict(c)))
    print()
    for b, ex in align_examples.items():
        print("examples -- %s   (file, ours_instrs, vendor_instrs, our_idx_before_endpgm,"
              " vendor_endpgm_pos, vendor_invalid, vendor_code_end, endpgm_word_idx)" % b)
        for e in ex:
            print("  %s" % (e,))
        print()


if __name__ == "__main__":
    main()
