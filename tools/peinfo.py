#!/usr/bin/env python3
"""Minimal PE parser: sections, exports (name+ordinal+rva), imports, version strings, magic scan.

No external deps. Used to compare console/PC D3D12 and console shader-toolchain DLLs.
"""
import struct, sys, json, re


class PE:
    def __init__(self, path):
        self.path = path
        self.b = open(path, "rb").read()
        b = self.b
        assert b[:2] == b"MZ", "not an MZ"
        e = struct.unpack_from("<I", b, 0x3C)[0]
        self.e_lfanew = e
        assert b[e:e + 4] == b"PE\0\0", "not a PE"
        coff = e + 4
        self.machine, self.nsec = struct.unpack_from("<HH", b, coff)
        self.tds = struct.unpack_from("<I", b, coff + 4)[0]
        self.chars = struct.unpack_from("<H", b, coff + 18)[0]
        optsz = struct.unpack_from("<H", b, coff + 16)[0]
        self.opt = coff + 20
        self.magic = struct.unpack_from("<H", b, self.opt)[0]
        self.is64 = self.magic == 0x20B
        self.image_base = struct.unpack_from("<Q" if self.is64 else "<I", b, self.opt + 24)[0]
        self.entry = struct.unpack_from("<I", b, self.opt + 16)[0]
        self.subsys = struct.unpack_from("<H", b, self.opt + (68 if self.is64 else 68))[0]
        self.dll_chars = struct.unpack_from("<H", b, self.opt + (70 if self.is64 else 70))[0]
        self.ddir_off = self.opt + (0x70 if self.is64 else 0x60)
        self.nddir = struct.unpack_from("<I", b, self.opt + (0x6C if self.is64 else 0x5C))[0]
        self.secoff = self.opt + optsz
        self.sections = []
        for i in range(self.nsec):
            o = self.secoff + i * 40
            name = b[o:o + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rsz, ro = struct.unpack_from("<IIII", b, o + 8)
            self.sections.append((name, va, vsz, ro, rsz))

    def section_of_rva(self, rva):
        for name, va, vsz, ro, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                return name, va, vsz, ro, rsz
        return None

    def r2o(self, rva):
        for name, va, vsz, ro, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                return ro + (rva - va)
        return None

    def dd(self, idx):
        o = self.ddir_off + idx * 8
        if o + 8 > len(self.b):
            return (0, 0)
        return struct.unpack_from("<II", self.b, o)

    def exports(self):
        rva, size = self.dd(0)
        if not rva:
            return {"base": 0, "nfunc": 0, "names": [], "by_ordinal": {}, "fwd": {}}
        o = self.r2o(rva)
        nfun, nnam = struct.unpack_from("<II", self.b, o + 20)
        a_func = struct.unpack_from("<I", self.b, o + 28)[0]
        a_name = struct.unpack_from("<I", self.b, o + 32)[0]
        a_ord = struct.unpack_from("<I", self.b, o + 36)[0]
        base = struct.unpack_from("<I", self.b, o + 16)[0]
        funcs = [struct.unpack_from("<I", self.b, self.r2o(a_func) + i * 4)[0] for i in range(nfun)]
        ords = [struct.unpack_from("<H", self.b, self.r2o(a_ord) + i * 2)[0] for i in range(nnam)]
        a_dll = struct.unpack_from("<I", self.b, o + 12)[0]
        dllname = ""
        if a_dll:
            p = self.r2o(a_dll)
            dllname = self.b[p:self.b.find(b"\0", p)].decode("latin1")
        names, fwd = [], {}
        for i in range(nnam):
            p = self.r2o(struct.unpack_from("<I", self.b, self.r2o(a_name) + i * 4)[0])
            nm = self.b[p:self.b.find(b"\0", p)].decode("latin1")
            ov = ords[i]
            rv = funcs[ov] if ov < len(funcs) else 0
            if rva <= rv < rva + size:
                fo = self.r2o(rv)
                fstr = self.b[fo:self.b.find(b"\0", fo)].decode("latin1")
                fwd[nm] = fstr
            names.append((nm, base + ov, rv))
        return {"dllname": dllname, "base": base, "nfunc": nfun, "nnames": nnam,
                "names": names, "fwd": fwd}

    def imports(self):
        rva, _ = self.dd(1)
        out = []
        if not rva:
            return out
        o = self.r2o(rva)
        i = 0
        while True:
            ent = o + i * 20
            oft, tds, fc, nrva, fta = struct.unpack_from("<IIIII", self.b, ent)
            if nrva == 0 and oft == 0 and tds == 0:
                break
            p = self.r2o(nrva)
            dll = self.b[p:self.b.find(b"\0", p)].decode("latin1")
            thunk = self.r2o(oft or fta)
            funcs = []
            k = 0
            while True:
                v = struct.unpack_from("<Q" if self.is64 else "<I", self.b, thunk + k * (8 if self.is64 else 4))[0]
                if v == 0:
                    break
                if self.is64 and (v >> 63):
                    funcs.append(("ord", v & 0xFFFF))
                elif (not self.is64) and (v >> 31):
                    funcs.append(("ord", v & 0xFFFF))
                else:
                    po = self.r2o(v & 0x7FFFFFFF)
                    if po is None:
                        funcs.append(("bad", v))
                    else:
                        funcs.append(("name", self.b[po + 2:self.b.find(b"\0", po + 2)].decode("latin1")))
                k += 1
                if k > 5000:
                    break
            out.append((dll, funcs))
            i += 1
            if i > 200:
                break
        return out

    def ver_strings(self):
        """Crude VS_VERSION_INFO extraction: UTF-16 strings in .rsrc."""
        txt = self.b.decode("latin1")
        res = {}
        for key in ("FileVersion", "ProductVersion", "CompanyName", "ProductName",
                    "FileDescription", "InternalName", "OriginalFilename", "LegalCopyright"):
            m = re.search(re.escape(key) + "\x00" + r"(.{0,4}?)" + r"((?:[\x20-\x7e]\x00)+)", txt)
            if m:
                s = txt[m.start(2):m.start(2) + 300]
                out = []
                for j in range(0, len(s) - 1, 2):
                    ch = s[j]
                    if ch == "\x00" and s[j + 1] == "\x00":
                        break
                    out.append(ch)
                res[key] = "".join(out)
        return res

    def scan(self, patterns):
        hits = {}
        for pat in patterns:
            idx, n = [], 0
            start = 0
            while True:
                k = self.b.find(pat, start)
                if k < 0 or n >= 6:
                    break
                idx.append(k)
                start = k + 1
                n += 1
            if idx:
                hits[pat.decode("latin1", "replace")] = idx
        return hits

    def ascii_strings(self, minlen=6, limit=None):
        out = re.findall(rb"[\x20-\x7e]{%d,}" % minlen, self.b)
        out = [s.decode("latin1") for s in out]
        return out if limit is None else out[:limit]

    def utf16_strings(self, minlen=6):
        out = re.findall(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen, self.b)
        return [s.decode("utf-16-le") for s in out]

    def summary(self):
        return {
            "path": self.path, "size": len(self.b),
            "machine": hex(self.machine), "is64": self.is64,
            "image_base": hex(self.image_base),
            "sections": [s[0] for s in self.sections],
            "dll_chars": hex(self.dll_chars),
            "versions": self.ver_strings(),
        }


MAGICS = [b"FSSM", b"DXIL", b"DXBC", b"\xbc\xc0\xde", b"RDEF", b"ISG1", b"OSG1", b"SFI0",
          b"SGD2", b"rgxe", b"rgxa", b"RPF9", b"9FPR", b"RPF7", b"XBCH", b"XDAT"]


def main():
    paths = sys.argv[1:]
    out = {}
    for p in paths:
        try:
            pe = PE(p)
        except Exception as ex:
            out[p] = {"error": str(ex)}
            continue
        ex_ = pe.exports()
        out[p] = {
            "summary": pe.summary(),
            "n_exports": len(ex_["names"]),
            "export_base": ex_["base"],
            "export_names": [n for n, _, _ in ex_["names"]],
            "forwarders": ex_["fwd"],
            "imports": [(d, len(f)) for d, f in pe.imports()],
            "magics": pe.scan(MAGICS),
        }
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
