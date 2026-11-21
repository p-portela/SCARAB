#!/usr/bin/env python3
"""Generate the d3d12_xs.dll export contract (.def) from the console runtime.

The .def is *derived data*, not source. It is the export table of the runtime we
are a drop-in replacement for, so committing it would mean committing the
vendor's export-name list into this repository. It is generated at build time
from the artifact the user supplies (`SCARAB_VENDOR`) instead - the same rule
that keeps `vendor/` and `corpora/` out of git.

The ABI requires the *exported* names to be byte-exact, but our source symbols do
not have to be: several of the runtime's console-only names carry a vendor token
that has no place in this repository's source. For those the .def aliases the
real export name onto our own symbol:

    EXPORTS  <real-name>=<internal-name> [@ordinal] [DATA]

Both linkers support that form, so the built DLL is byte-exact while the source
stays clean. Gate G1 checks the result against this same artifact.

The token is **derived, never hardcoded**: a console-only export sits beside the
shared export it extends (`D3D12<T>CreateDevice` next to `D3D12CreateDevice`), and
the inserted infix is the token. The mapping is then *verified* against the
symbols this repository actually defines, so a drifted rename fails loudly here
rather than producing a .def that exports the wrong names.

Usage: gen_def.py <reference.dll> <shared-reference.dll> <out.def> [out.csv]
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peinfo import PE  # noqa: E402

# Our replacement for the derived vendor token. Ours, so it may be hardcoded.
OUR_TOKEN = "Cons"
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _prefix(a, b):
    i = 0
    while i < len(a) and i < len(b) and a[i] == b[i]:
        i += 1
    return i


def _suffix(a, b, lo):
    j = 0
    while j < len(a) - lo and j < len(b) and a[-1 - j] == b[-1 - j]:
        j += 1
    return j


def derive_token(console_names, shared_names):
    """The vendor token inserted into console-only export names, from data alone.

    Candidates come from aligning a console-only name against a shared name it
    extends. The winner is the candidate that occurs inside the most console-only
    names - a token used by one family of names, not a coincidence of two.
    """
    shared = set(shared_names)
    scores, appear = {}, {}
    for c in console_names:
        if c in shared:
            continue
        for s in shared:
            i = _prefix(c, s)
            if i == 0:
                continue
            j = _suffix(c, s, i)
            if j < 6:                      # need a real shared tail
                continue
            infix = c[i:len(c) - j]
            if not infix.isalpha() or not (1 <= len(infix) <= 16):
                continue
            scores[infix] = scores.get(infix, 0) + 1
    for tok in scores:
        appear[tok] = sum(1 for c in console_names if tok in c)
    if not scores:
        return None, 0, []
    ranked = sorted(scores, key=lambda t: (appear[t], scores[t]), reverse=True)
    best = ranked[0]
    rival = max([appear[r] for r in ranked[1:]], default=0)
    return best, appear[best], [r for r in ranked[1:] if appear[r] == appear[best]]


def source_symbols():
    """Every identifier this repository defines that looks like an export symbol."""
    pat = re.compile(r"\b(?:IID_)?(?:ID3D12|D3D12)[A-Za-z0-9_]+\b")
    found = set()
    for root, dirs, files in os.walk(os.path.join(SRC, "components")):
        dirs[:] = [d for d in dirs if d not in ("generated", "__pycache__")]
        for name in files:
            if not name.endswith((".c", ".cc", ".cpp", ".h", ".inc")):
                continue
            try:
                text = open(os.path.join(root, name), encoding="utf-8").read()
            except OSError:
                continue
            found.update(pat.findall(text))
    return found


def load_mapping(ref, shared_ref):
    """{export name -> our source symbol} for a runtime and its shared loader.

    Single definition of the mapping; tools/tests import this rather than
    restating the rule (a restated rule is how the two drift apart).
    """
    names = [n for n, _o, _r in PE(ref).exports()["names"]]
    shared = [n for n, _o, _r in PE(shared_ref).exports()["names"]]
    token, _used, _tied = derive_token(names, shared)
    if not token:
        raise RuntimeError("could not derive the console-only vendor token from "
                           "%s + %s" % (ref, shared_ref))
    return {n: n.replace(token, OUR_TOKEN) for n in names}


def is_data(name):
    """GUID exports are data, not code: they must be exported as DATA."""
    return name.startswith(("IID_", "DXGI_DEBUG_"))


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    ref, shared_ref, out_def = sys.argv[1], sys.argv[2], sys.argv[3]
    out_csv = sys.argv[4] if len(sys.argv) > 4 else None

    pe = PE(ref)
    exports = sorted(pe.exports()["names"], key=lambda t: t[1])     # by ordinal
    if not exports:
        print("FAIL no exports found in %s" % ref)
        return 1

    token, used, tied = derive_token([n for n, _, _ in exports], [n for n, _o, _r in PE(shared_ref).exports()["names"]])
    if not token:
        print("FAIL could not derive the console-only vendor token from %s + %s"
              % (ref, shared_ref))
        return 1
    print("derived token: %r (inserted into %d console-only export names)"
          % (token, used))
    if tied:
        print("WARN token candidates tied on coverage: %s" % ", ".join(tied))

    def internal(name):
        return name.replace(token, OUR_TOKEN)

    # The mapping must land on symbols this repository actually defines, or the
    # link would fail with unresolved exports and nobody would know why.
    have = source_symbols()
    unmapped = [n for n, _, _ in exports if internal(n) not in have and n != internal(n)]
    if unmapped:
        print("FAIL %d export(s) map to symbols this repository does not define, "
              "so the .def would not link:" % len(unmapped))
        for n in unmapped[:10]:
            print("       %s -> %s" % (n, internal(n)))
        return 1

    lines = [
        "; d3d12_xs.dll export contract",
        "; GENERATED by tools/gen_def.py from the console runtime the user supplies",
        "; (SCARAB_VENDOR). Do not edit and do not commit: it is the vendor's export",
        "; table. Where an export name differs from our source symbol the .def",
        "; aliases it (EXPORTS real=internal), so the vendor token stays out of the",
        "; source while the wire format stays byte-exact. Enforced by gate G1.",
        "EXPORTS",
    ]
    aliased = 0
    for name, ordinal, _rva in exports:
        sym = internal(name)
        if sym != name:
            aliased += 1
            entry = "    %s=%s" % (name, sym)
        else:
            entry = "    %s" % name
        entry += " @%d" % ordinal
        if is_data(name):
            entry += " DATA"
        lines.append(entry)

    def write(path, text, newline):
        with open(path, "w", encoding="utf-8", newline=newline) as fh:
            fh.write(text)

    write(out_def, "\n".join(lines) + "\n", "\n")
    print("wrote %s: %d exports, %d aliased" % (out_def, len(exports), aliased))

    if out_csv:
        rows = ["ordinal,name,kind"]
        for name, ordinal, _rva in exports:
            rows.append("%d,%s,%s" % (ordinal, name, "data" if is_data(name) else "func"))
        write(out_csv, "\r\n".join(rows) + "\r\n", "")
        print("wrote %s" % out_csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
