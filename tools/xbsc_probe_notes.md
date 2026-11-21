# `XBSC_DisassembleRawShader` — recovered signature, and the probe that confirms it

Status: **confirmed by execution**, not inferred. Tools: `tools/xbsc_probe.c` +
`tools/xbsc_probe.sh` (probe), `tools/ghidra/XbscDecompile.java` (Ghidra headless
decompiler for the export cluster).

## The call

```c
typedef struct { unsigned long long data, size, version; } XbscBuf;   // 4th arg, pass NULL

void* XBSC_DisassembleRawShader(const void* code,      // arg1: RDNA2 words
                                unsigned long long size, // arg2: size in BYTES
                                unsigned long long* marker, // arg3: must not be NULL
                                XbscBuf* view);        // arg4: optional, NULL = default

unsigned int XBSC_GetResultCode(void* result);              // 0 = ok, 1 = failure
char*        XBSC_GetErrorMessage(void* result);            // NULL handle -> "unknown error"
int          XBSC_GetDisassembly(void* result, void** text, void** len);
void         XBSC_DestroyResult(void* result);
```

Call as `XBSC_DisassembleRawShader(code, size_bytes, &marker, NULL)`; on a non-NULL
return with `GetResultCode()==0`, `XBSC_GetDisassembly(result, &text, &len)` gives the
text. `GetDisassembly` only produces text when the result's kind field (`+0x18`) is
`0xe` for a raw shader — that check is inside the DLL.

## How the shape was established

Ghidra (`XbscDecompile.java`, 12.1.4 + JDK 21, `-noanalysis`, targeted addresses —
see the addresses pinned below) decompiles the exports down to the real worker
`FUN_1800a8210(code, size, marker, …)`, which builds a `{data,size,version}` view via
`FUN_18000d4d0`/`FUN_18000d470` and calls the disassembler. The `XBSC_*` exports are
thin: `XBSC_GetResultCode` is `h ? *(u32*)(h+8) : 1`, and a NULL handle is
failure-by-default.

Then the probe measured it. The decisive checks, all in `xbsc_probe.sh`:

- **Arity 4, not 5.** Passing a non-NULL 4th arg faults inside `FUN_18000d470` at
  `movq 8(%rcx), %r8` with `rcx` = the value passed as arg 4 — the implementation
  dereferences arg 4 as `{p0,p1,p2}`. The "5th stack argument" seen earlier in the
  export stub is a red herring.
- **`size` is bytes.** A 8-byte blob of `0xBF810000, 0xBF9F0000` yields exactly
  `s_endpgm` + `s_code_end` — nothing more, nothing less.
- **arg3 must be non-NULL.** `disasm(code, size, NULL, NULL)` returns a NULL handle
  with code 1; `disasm(code, size, &marker, NULL)` returns a valid one.
- **arg4 must be NULL.** A hand-made `{data,size,version}` triple, a zeroed 0x40-byte
  object, and `{0,0,6}` all fault: the implementation calls a field of the object as a
  pointer (`rip=0`, or a jump to a heap address, with the triple's 3rd qword in `rcx`).
  So arg4 is an interface pointer that must come from another XBSC factory, and NULL
  selects the DLL's own default. The raw-shader path needs nothing else — arg1/arg2
  carry the code.
- **The output is real.** The first stream the probe decoded has exactly one
  `s_endpgm` at word 19 and 40 `s_code_end` words after it; the vendor disassembler
  emits exactly that, wrapped in `shader main / asic(CI) / type(LS) / … / end`.

## Ghidra addresses used (RVA + image base 0x180000000)

| export / helper | VA |
|---|---|
| `XBSC_DisassembleRawShader` (stub) | `0x18000a4c0` |
| its implementation | `0x18000a4e0` |
| `XBSC_GetDisassembly` / `GetErrorMessage` / `GetResultCode` / `DestroyResult` | `0xa6d0` / `0xa3f0` / `0xa3d0` / `0xa3b0` |
| view builder / dispatcher | `0x18000d470`, `0x18014aa70`, `0x18000ba28` |
| disassembler worker | `0x1800a8210` |

`XBSC_GetErrorMessage(NULL)` returns the static string `"unknown error"`; a real
failure is expected to come from the result object's own vtable instead.
