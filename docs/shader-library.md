# the target title (Scarlett) shader library — shaders.rpf

Source: `shaders.rpf` from `the retail title build`, 411,695,600 bytes
(sha256 prefix `0bc1634594e51ca8e7ef5f2bc6f5b32b`). Extracted with
`tools/extract_shaders.py` into `shaders/` + `shaders/index.csv`.

## Headline

The shader library is fully extractable — **7,010 distinct shader names**,
25,889 distinct payloads, 245.2 MB uncompressed — **but the payloads are not
portable shader bytecode.** They are the publisher's own `FSSM` state records, not
DXIL and not DXBC program code. There is no GPU program anywhere in the file.

This corrects an earlier working assumption that the shaders were SM6 DXIL and
therefore PC-loadable without recompilation. That was wrong; see "Correction".

## RPF9 container

Magic on disk `9FPR` → u32 LE `RPF9`. 64-byte header (RPF7's was 16):

| off  | field |
|------|-------|
| 0x00 | magic `RPF9` |
| 0x04 | u32 f1, 0x08 u32 f2, 0x0C u32 f3 (sizes/counts, semantics TBD) |
| 0x10 | u64 per-archive id (12/12 distinct across archives) |
| 0x18 | u64 constant `0x00047a00` / `0x00000150` (all archives) |
| 0x20 | u16 flag=1, u16 type∈{3,5,6,7}, u32 count |
| 0x28 | u32 key-id, u32 version `0x00090f01`/`0x00090f02` |
| 0x30 | 16 zero bytes |
| 0x40 | AES-ECB-encrypted TOC |

ECB evidence: bytes 0x40–0x5F are byte-identical across all twelve archives
sampled; from 0x60 they diverge. Deterministic block encryption over a fixed
leading plaintext — AES-ECB (RPF7's documented mode) or CBC with fixed IV.
The 0x28 field takes only four distinct values across twelve archives and
groups related packs, so it reads as a key selector.

## Shader record framing (plaintext, inside the file data)

```
[u8 len+1][name ASCII][u8 0x00][u8 0x32]
[u32 compressedSize][u32 0x000DDCE7][u32 compressedSize][u32 uncompressedSize]
[raw deflate stream, no zlib header]
```

- name prefix byte = strlen + 1 (51 for a 50-char name, 35 for a 34-char name).
- the `0x32` after the name is **constant** (50), not the name length.
- `0x000DDCE7` is constant across all 26,503 records.
- the deflate stream begins exactly **18 bytes after the name's end**; verified
  on every record (0 failures across 26,503).

## What the deflate stream holds

A DXBC-shaped container with a uniform chunk set on all 25,889 payloads:

```
SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV
```

- `ISG1`/`OSG1`/`PSV0`/`SFI0` are DXIL-style **reflection** chunks (input/output
  signatures, pipeline-state validation, shader feature info). These are
  standard and readable — e.g. `SV_VertexID`, `SV_Position`, `TEXCOORD`.
- `HASH` = u32 flags + 16-byte shader hash (PSO cache key).
- `DXIL` — **not** LLVM bitcode. No `BC C0 DE` magic occurs anywhere in the
  411 MB file (0 hits in 4,000 sampled shaders). The chunk contains two
  sub-sections: `XDAT` (tables, incl. a 16-byte hash) and `XBCH`.
- `XBCH` header `magic u32 version=3, u32 compressedSize, u32 0, u32
  uncompressedSize, u32 0`, then a **zlib** stream (`78 9c`).
- That zlib stream decompresses (2988/2988 sampled) to magic **`FSSM`**,
  version `0x101`.

## FSSM — what it actually is

A 6,722-byte record for the first shader:

- entropy: **corpus-wide median 5.72 bits/byte, max 6.70, never above 7.0**; 6,179 of
  25,782 blobs have regions >6.0. (The single VS quoted above sits at ~1.7–2.2 because it
  is a trivial passthrough — it is not representative. An earlier draft of this doc
  generalised from it and was wrong.)
- nonzero fraction median 0.47, max 0.91.
- header: `FSSM`, `0x101`, size, then sentinel-filled slots; 26 or 36 `0xDEADBEEF` dwords
  in the large majority of blobs.
- tail is a table of repeating 16- or 20-byte records — `[u32 a][u32 8][u32 3][u32 b][u32 1]`
  with `b` stepping by 8 — i.e. a binding/slot table.
- ends with the length-prefixed shader name.

Body structure (from the largest blob, `CS_ScreenProbes_ActivateProbes_9083c094_Wrapped`,
493,467 bytes): only **2,063 unique dwords out of 123,366 (1.7%)**; the 2-byte unit `69d5`
occurs 19,707 times; byte0 clusters on 0x11/0x12/0x13. That is a highly repetitive
instruction/record stream.

Read: **corrected — FSSM is AMD RDNA2 / gfx10 GPU machine code (the Scarlett GPU ISA),
not bytecode/IR.** An earlier draft of this doc read the low entropy and dword repetition
as an instruction/IR stream; that was wrong. `s_endpgm` (`0xBF810000`) is present at 4-byte
alignment in 25,782/25,782 blobs (random-data control: 0/3000), and the code region ends in
repeated `0xBF9F0000` (`s_code_end`) padding. Whole-blob entropy is low because the blob is
mostly metadata plus filler, not because the content is bytecode. Full write-up:
`docs/fssm-analysis.md`.

## Shader inventory

Stages (by name prefix), distinct payloads:

| prefix | count | meaning |
|--------|-------|---------|
| PS     | 10,197 | pixel |
| MS     | 4,928  | mesh |
| MSNC   | 3,338  | mesh (no-cull) |
| CS     | 2,543  | compute |
| VS     | 1,664  | vertex |
| MSR    | 388    | mesh (R?) |
| ptfx   | 202    | particle FX |
| GS     | 195    | geometry |
| TL     | 136    | tile/tessellation? |
| APS    | 90     | amplification |
| DPS    | 64     | deferred pixel |
| AMSNC  | 51     | amplification/mesh |

- 1,017 shader names contain `Raytracing`; 1,002 payloads contain the string.
- 8,894 payloads are mesh/amplification (MS/MSNC/MSR/AMS/APS).
- 49 contain `gs_`, 17 `Wave` (wave intrinsics), 1 `hlsl`.
- Payload sizes: min 568, median 5,604, p90 22,128, max 107,872 bytes.
- 1,415 names have more than one payload (up to 275 for
  `CS_RaytracingResolve_f21e9178_Wrapped`), differing slightly in size — i.e.
  per-permutation instances that the name does not distinguish.
- ~840 distinct sampler-binding names appear (TrilinearClamp, DiffuseSampler,
  Anisotropic4XWrap, vehglassCrackTextureSampler, ImposterAtlasPointSampler,
  TerrainFoamSampler, ProjDecalTrailTexArraySampler, …).

## Consequences for the translation layer

- **Feature floor is high**: mesh shaders (SM 6.5+) and ray tracing are
  mandatory, not optional. Any PC target needs DXR-class ray queries and mesh
  shader support — the known weak spot for vkd3d-proton.
- **Shader translation is a real work item**, not a freebie. FSSM has to be
  reversed, or the runtime shader compiler recovered, before anything renders.
- **Boot is unaffected by all of this.** Boot = loader + ERA/TitleOS shims +
  package mount. Shaders gate rendering, which is several milestones later.
- The reflection chunks (ISG1/OSG1/PSV0/SFI0/HASH) and the sampler/binding
  tables are the useful portable output: they give the shader interface and
  resource-binding model without needing the program.

## Correction

An earlier claim in this session — that the shaders are SM6 DXIL and therefore
usable on PC with no recompilation — was wrong. The `DXIL` fourcc is present in
the container, which is what made it look right, but the chunk holds
`XDAT`/`XBCH`→`FSSM`, and no LLVM bitcode exists anywhere in the file. Recorded
here rather than silently dropped.
