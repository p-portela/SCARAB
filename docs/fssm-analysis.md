# FSSM analysis — the target title (Scarlett) shader container

Verdict: **the FSSM body is AMD RDNA2 / gfx10 GPU machine code — the Scarlett GPU
ISA.** It is not DXIL, not DXBC program code, and not a the publisher intermediate
representation. Confidence: very high on "machine code"; the residual uncertainty is only
which exact RDNA2 variant.

This overturns the earlier "bytecode/IR instruction stream" reading in
`docs/shader-library.md`. That reading came from whole-blob entropy and dword-repetition
statistics, which conflated three different regions (metadata, code, and `s_code_end`
filler). See "Where the earlier reading went wrong".

## Evidence verified directly (OBSERVED)

Measured on the full corpus of 25,782 parseable FSSM blobs.

| check | result | control (random data, matched sizes) |
|-------|--------|--------------------------------------|
| `s_endpgm` (`0xBF810000`) present at 4-byte alignment | **25,782 / 25,782 (100%)** | 0 / 3000 |
| `s_code_end` (`0xBF9F0000`) present at 4-byte alignment | **25,782 / 25,782 (100%)** | 0 / 3000 |
| `s_nop` (`0xBF800000`) | 22,383 / 25,782 (86.8%) | 0 / 3000 |
| `s_waitcnt` (`0xBF8C0000`) | 8,935 / 25,782 (34.7%) | 0 / 3000 |
| `[0x08] == filelen - 16` | 25,782 / 25,782 (100%) | — |
| `[0x10] == 6` | 25,782 / 25,782 (100%) | — |
| `code_end == code_off + code_size` | 25,765 / 25,782 (99.9%) | — |

The random-data control is the discriminator: `0xBF810000` is a 4-byte exact match, so in
random data the hit rate is ~6e-6 per blob. Finding it in **every** blob at 4-byte
alignment is not coincidence.

The code region also visibly ends in repeated `s_code_end` padding — the tail of
`CPS_VehicleLiveryOffDetailMapsOffDeferred_NoDeform_34f92402_Wrapped` reads
`00009fbf` × 8, i.e. `0xBF9F0000` repeated to the section end. Textbook AMD shader
code-end filler.

Verbatim header + section table from that blob (len 18,710):

```
[0x00] 'FSSM'  [0x04] 0x101  [0x08] 18694 (== len-16)  [0x10] 6
[0xc0] code_off = 3832   [0xc8] code_size = 9664   [0xe8] code_end = 13496
```

## Evidence reported by the analysis subagent, not independently re-verified here (INFERRED)

- A full instruction walk using AMD-ISA encoding rules (from the local the upstream shader recompiler
  `RdnaDecoder`) produces `s_endpgm`, `s_nop`, `s_code_end`, `s_waitcnt`,
  `s_cbranch_execz`, `v_mov_b32`, `v_cndmask_b32`, `exp`, `smem`, `mubuf` streams.
- Branch targets resolve in-range **12,786 / 12,789 (100%)**; a bytecode or hash stream
  cannot produce self-consistent relative control flow at that rate.
- Instruction mix tracks shader stage: `v_interp_*` interpolation ops in PS/APS/ptfx
  (9,815 in PS); `s_barrier` + `s_sendmsg` in MS/MSNC/AMSNC/MSR; `s_barrier` with no
  interpolation in CS.
- The trivial 6,722-byte VS and the 493,467-byte CS decode with identical grammar — a real
  ISA shows the same encoding regardless of size.

These are consistent with the directly-measured facts above but should be re-checked
before being quoted as established.

## Correction to the subagent's report

The subagent stated `code_off` is 16-byte aligned (400/400). **That is wrong.** Across the
corpus `code_off mod 16` is `0` for 11,403 blobs and `8` for 14,379 — i.e. `code_off` is
**8-aligned**, not 16. Use 8-byte alignment when walking the section table.

## Container structure (OBSERVED)

```
FSSM container
  0x00  'FSSM'
  0x04  u32 0x101            version
  0x08  u64 filelen - 16
  0x10  u64 6
  0x18… 8-byte slots, 0xdeadbeefdeadbeef = unset
  0x78  pair (2, N)
  0x80  u64 0x44 / 0x45
  0xc0  u64 code_off    (8-aligned)
  0xc8  u64 code_size
  0xe8  u64 code_end    (== code_off + code_size)
  0xf0  u64 metadata pointer, or DEAD sentinel
code_off + 0x00..0x10  16-byte sub-header, (0,0,0,0) or (8,1,0,0)
code_off + 0x10        raw AMD ISA instructions
                       ... padded with 0xBF9F0000 (s_code_end) up to code_end
code_end → EOF         binding/metadata tables, 20-byte records [a][8][3][b+=8][1]
                       then name record [0xffffffff][0][u16 len][name]
```

Outer nesting, unchanged: RPF9 → shader record → raw deflate → DXBC-shaped container
(`SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV`) → `DXIL` chunk → `XDAT` + `XBCH` →
zlib (`78 9c`) → `FSSM`.

## Where the earlier reading went wrong

Three separate measurements, all individually correct, combined into a wrong conclusion:

1. Whole-blob entropy median 5.72 was read as "too low for machine code". In fact the blob
   is mostly metadata plus `s_code_end` filler, which drags the figure down. The code
   region alone is 55–86% unique, not the 1.7% the whole-blob dword count suggested.
2. Dword repetition (`69d5` × 19,707 in the big CS) was read as an opcode alphabet. It is
   repetitive because small shaders share instruction patterns, not because it is bytecode.
3. The `DXIL` fourcc on the outer chunk made "DXIL" look confirmed. It is a chunk *name*;
   the payload is ISA.

Lesson worth keeping: measure the region, not the file. Whole-file statistics on a
multi-region container will mislead.

## Consequence for the translation layer

- The shipped shaders are **console GPU machine code**. There is no DXIL and no
  re-containerising shortcut.
- A PC rendering path therefore needs one of:
  1. an **RDNA2 ISA → DXIL shader translator**, or
  2. the game's own runtime shader compiler, which is almost certainly in the title exe.
- Option 1 is buildable and **testable offline**, against the 25,889-shader corpus we
  already hold, with the reflection metadata (`ISG1`/`OSG1`/`PSV0`/`SFI0`, plus the
  `interface.csv` manifest) supplying the interface contract. So shaders are not strictly
  exe-gated — but this is a substantial component, not a formatting problem.
- Mitigating fact: RDNA2 is the same ISA family as PC RDNA2 GPUs, so the ISA itself is not
  exotic. It still has to be translated, because D3D12 consumes DXIL, not raw ISA.
- Known precedent to study: shader-ISA translation is what PS3/X360-era translation layers
  did, and AMD's own RGA plus the upstream shader recompiler `RdnaDecoder` already give partial disassembly.

## Counter-evidence (strongest against the verdict)

- The walker uses documented base instruction lengths and does **not** apply +1-dword
  literal operands, so ~3/12,789 branch targets miss — consistent with drift where a
  literal is parsed as an opcode. Exact disassembly needs the literal-operand rules from
  the upstream shader recompiler decoders, not just the classifier.
- Corpus entropy really is low compared with textbook machine code; the explanation given
  here (filler domination) is plausible and partly demonstrated, not fully proven.
- Everything above establishes "AMD ISA". The step to "RDNA2 specifically" rests on the
  Scarlett GPU being RDNA2 plus the observed opcode families.

## Next step

Feed the literal-operand rules from the upstream recompiler's decoder into the walker to get a full, clean
disassembly of one small shader and one large CS, then assess how tractable an
ISA → DXIL translation actually is on a real sample.

## Artifacts

- `tools/fssm_lib.py` — `.dxbc` → `DXIL`/`XDAT`/`XBCH` → zlib → `FSSM`
- `tools/rdna2_classify.py` — RDNA2 family classifier + mnemonics
- `tools/fssm_parse.py` — FSSM parser skeleton (header, section table, instruction walk,
  branch validation, name record)
- `shaders/interface.csv` — per-shader signatures (see `docs/shader-library.md`)
