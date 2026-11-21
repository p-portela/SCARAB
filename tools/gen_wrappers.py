#!/usr/bin/env python3
"""Generate ABI-exact forwarding implementations for D3D12 COM interface wrappers.

The wrapper layer must implement every method of the genuine interfaces. Doing
that by hand risks an off-by-one vtable slot, which is a silent memory-corruption
bug. So we parse the real Agility SDK header (vendor/d3d12/include/d3d12.h) and
emit one forwarding stub per method. The wrapper class inherits from the real
interface, so the compiler additionally verifies via `override` that every
signature matches -- the generated code cannot drift from the ABI.

Usage:
    python3 tools/gen_wrappers.py ID3D12Device4      > wrap/src/gen_device.inc
    python3 tools/gen_wrappers.py ID3D12GraphicsCommandList6 > wrap/src/gen_clist.inc
"""
import re, sys, os

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_VENDOR = os.environ.get("XTL_VENDOR", os.path.join(_ROOT, "vendor"))
H = os.path.join(_VENDOR, "d3d12", "include", "d3d12.h")
SKIP = {
    # IUnknown: hand-written, because QueryInterface is where the console IIDs
    # (IID_ID3D12ConsDmaCommandList etc.) are served.
    "QueryInterface", "AddRef", "Release",
    # hand-written: interception points (list.GetDevice must return the *wrapped*
    # device; device.CreateCommandList is where console command lists are created)
    "GetDevice", "CreateCommandList",
}


def load():
    return open(H, encoding="utf-8", errors="replace").read()


def eval_cond(expr, defined):
    """Evaluate a simple #if condition for our target (clang, _WIN32, C++)."""
    e = re.sub(r"defined\s*\(\s*(\w+)\s*\)",
               lambda m: "True" if m.group(1) in defined else "False", expr)
    e = e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    try:
        return bool(eval(e, {"__builtins__": {}}, {}))
    except Exception:
        return False


def resolve_conditionals(body, defined=("__cplusplus", "_WIN32")):
    """Resolve #if/#else/#endif inside an interface body.

    d3d12.h declares some methods twice: an MSVC-only form guarded by
    `#if defined(_MSC_VER) || !defined(_WIN32)` and a by-value form in the
    #else branch (23 occurrences). We compile with clang on Windows, so the
    else branch is the live one. Without this step the parser sees both and
    emits two conflicting declarations of the same method.
    """
    ds = set(defined)
    lines = body.split("\n")
    out, i = [], 0
    while i < len(lines):
        s = lines[i].strip()
        if s.startswith("#if"):
            cond = eval_cond(s[3:].strip(), ds)
            depth, j, haselse, elsej = 1, i + 1, False, None
            while j < len(lines) and depth:
                t = lines[j].strip()
                if t.startswith("#if"):
                    depth += 1
                elif t.startswith("#endif"):
                    depth -= 1
                elif t.startswith("#else") and depth == 1:
                    haselse, elsej = True, j
                j += 1
            if cond:
                out.extend(lines[i + 1:(elsej if haselse else j - 1)])
            elif haselse:
                out.extend(lines[elsej + 1:j - 1])
            i = j
            continue
        if s.startswith(("#else", "#endif")):
            i += 1
            continue
        out.append(lines[i])
        i += 1
    return "\n".join(out)


def interfaces(txt):
    """Map interface name -> (base, [(ret, name, params_raw)])."""
    out = {}
    for m in re.finditer(r'MIDL_INTERFACE\("[0-9a-fA-F-]+"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)\s*\n?\s*\{', txt):
        name, base = m.group(1), m.group(2)
        body_start = m.end()
        # scan to the closing "};" of this block (lines are 4-space indented)
        end = txt.find("\n    };", body_start)
        if end < 0:
            continue
        body = txt[body_start:end]
        body = resolve_conditionals(body)
        methods = []
        for d in re.finditer(r"virtual\s+(.+?)\s*STDMETHODCALLTYPE\s+(\w+)\s*\(", body, re.S):
            ret = " ".join(d.group(1).split())
            mname = d.group(2)
            p = d.end()
            depth, i = 1, p
            while i < len(body) and depth:
                if body[i] == "(":
                    depth += 1
                elif body[i] == ")":
                    depth -= 1
                i += 1
            params = body[p:i - 1]
            methods.append((ret, mname, params))
        out[name] = (base, methods)
    return out


def chain(ifaces, name):
    """Own methods of name plus all inherited, in vtable order (base first)."""
    seq = []
    cur = name
    while cur and cur in ifaces:
        base, methods = ifaces[cur]
        seq.append((cur, methods))
        cur = base
    seq.reverse()
    return seq


def argname(param):
    p = re.sub(r"_In[^ ]*\s*|_Out[^ ]*\s*|_COM_[A-Za-z_]*\s*|_Reserved_\s*|_When_[^ ]*\s*", "", param).strip()
    p = p.strip()
    # take the trailing identifier, ignoring array/pointer decorations
    m = re.findall(r"[A-Za-z_]\w*", p)
    if not m:
        return None
    nm = m[-1]
    # "const D3D12_X *pFoo" -> pFoo ; "REFIID riid" -> riid
    return nm


def split_params(params):
    parts, depth, cur = [], 0, ""
    for ch in params:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


def emit(ifaces, target, wrapper_member="m_pc", indent="    "):
    lines = []
    total = 0
    for ifname, methods in chain(ifaces, target):
        lines.append("%s// ---- from %s ----" % (indent, ifname))
        for ret, name, params in methods:
            if name in SKIP:
                continue
            args = []
            for p in split_params(params):
                if not p.strip():
                    continue
                a = argname(p)
                if a is None:
                    lines.append("%s// !! could not parse arg in %s::%s: %r" % (indent, ifname, name, p))
                    continue
                args.append(a)
            sig = " ".join(params.split())
            if sig.strip() == "void":
                sig = ""
                args = []
            call = "%s->%s(%s)" % (wrapper_member, name, ", ".join(args))
            rets = " ".join(ret.split())
            if rets == "void":
                lines.append("%svoid STDMETHODCALLTYPE %s(%s) override { %s; }"
                             % (indent, name, sig, call))
            else:
                lines.append("%s%s STDMETHODCALLTYPE %s(%s) override { return %s; }"
                             % (indent, rets, name, sig, call))
            total += 1
    lines.append("%s// total forwarded methods: %d" % (indent, total))
    return "\n".join(lines), total


def main():
    txt = load()
    ifaces = interfaces(txt)
    target = sys.argv[1] if len(sys.argv) > 1 else "ID3D12Device4"
    body, n = emit(ifaces, target)
    sys.stderr.write("parsed %d interfaces; emitted %d methods for %s\n" % (len(ifaces), n, target))
    print(body)


if __name__ == "__main__":
    main()
