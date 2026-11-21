# the publisher RAGE shader container formats — public prior art, and is the target title's `FSSM` a descendant?

**Scope.** What is publicly documented about the shader *container* formats of RAGE-engine titles (pre-Gen8 RAGE title,
a racing title on the same engine, a western RAGE title, a linear-shooter RAGE title, Gen8 RAGE title, current-Gen RAGE title), the tools that read/write them, and
whether the the target title (Scarlett) `FSSM` container (version `0x101`) plausibly belongs to that family.

**Method.** Web search for discovery; pages fetched with `curl -sL -A 'Mozilla/5.0'`; GitHub trees/blobs read
from `api.github.com` and `raw.githubusercontent.com`; two repos downloaded as tarballs and grepped locally.
`web_extract` was unavailable, and a public modding forum is behind Cloudflare (search snippets only).

**Confidence legend.** Claims are followed by a source URL. Anything I could not confirm from a primary source
is marked **[UNVERIFIED]**. Format constants are quoted exactly as they appear in the cited source.

---

## Q1. Documented RAGE shader container formats

### 1.1 Summary table

| Game(s) | Container | Magic | Magic as bytes / hex | Versioning | Wrapped payload |
|---|---|---|---|---|---|
| pre-Gen8 RAGE title, a racing title on the same engine | `.fxc` ("rgxa") | `1635280754` | `"rgxa"` = `0x61786772` | magic letter only (`a`) | console 360 Xenos microcode **or** D3D9 SM3 tokens |
| a linear-shooter RAGE title | `.fxc` ("rgxe") | `1702389618` | `"rgxe"` = `0x65786772` | magic letter only (`e`) | D3D9 SM3 (PC) / Xenos (console) |
| Gen8 RAGE title | `.fxc` ("rgxe") | `1702389618` | `"rgxe"` = `0x65786772` | magic letter only | **DXBC** (SM4/SM5) |
| current-Gen RAGE title (Gen9/DX12) | `.awc` ("SGD2") | `"SGD2"` | `0x32444753` (LE of `"SGD2"`) | magic string `SGD2` = Shader Group Data v2 | **DXIL** (SM6.x) |
| RDR3 (RDR2 / RDR Online) | `.awc` / `fxdb` ("SGD1"/"SGDB") | `"SGD1"`/`"SGDB"` | — | v1 | DXBC/DXIL |
| pre-Gen8 RAGE title, a racing title on the same engine (shader *presets*) | `.sps` | none (plain text) | — | — | n/a — material→shader map |
| console 360 titles (IV/a racing title on the same engine/MP3/RDR) | embedded in `.fxc` fragment | flags word `0x102A11XX` | — | low byte = stage | Xenos GPU microcode + reflection |

Sources: Gen8 RAGE title `rgxe` = `1702389618` in CodeWalker
`CodeWalker.Core/GameFiles/FileTypes/FxcFile.cs`
(https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/FileTypes/FxcFile.cs) and its Python
port `src/core/fxc_parser.py`
(a public third-party shader-manager tool); pre-Gen8 RAGE title/a racing title on the same engine `rgxa` =
`1635280754` in `Program.cs` of RAGE-Shader-Editor
(https://github.com/ImpossibleEchoes/RAGE-Shader-Editor/blob/main/Program.cs); SGD2 in
`src/smcore/enhanced.py` and `src/awclib/models.py`
(a public third-party shader-manager tool); `rgxa` layout and the console 360
container in LibertyRecomp `docs/SHADER_PIPELINE.md`
(https://github.com/OZORDI/LibertyRecomp/blob/main/docs/SHADER_PIPELINE.md).

**Key structural fact: every documented RAGE shader container wraps *vendor* bytecode (Xenos microcode, D3D9 SM3
tokens, DXBC, DXIL). the publisher does not ship a title-specific shader bytecode.** The container supplies only
effect metadata (variables, constant buffers, techniques/passes) plus the opaque byte blob.

### 1.2 `rgxe` — Gen8 RAGE title / a linear-shooter RAGE title `.fxc` (fully documented)

Header, in order (byte-exact from `src/core/fxc_parser.py` `FxcFile.load`, mirroring CodeWalker's `FxcFile.cs`):

```
u32  magic                 == 1702389618 ("rgxe")
u32  VertexType            (vertex declaration type; CodeWalker enum VertexType)
u8   PresetParamCount
  { string Name; u8 Unused0; u32 Value }[PresetParamCount]
6 × ShaderGroup            (order: VS, PS, CS, DS, GS, HS)
u8   CBufferCount1
  { u32 Size; u16 Slots[6] (VS,PS,CS,DS,GS,HS); string Name }[CBufferCount1]
u8   VariableCount1
  { u8 Type; u8 Count; u8 Slot; u8 Group; string Name1; string Name2;
    u8 Offset; u8 Variant; u8 Unused0; u8 Unused1; u32 CBufferName;
    u8 ParamCount; {...}[ParamCount]; u8 ValueCount; {u32|f32}[ValueCount] }[VariableCount1]
u8   CBufferCount2 ; {...}
u8   VariableCount2 ; {...}      (textures/samplers)
u8   TechniqueCount
  { string Name; u8 PassCount; { u8 Indices[6]; u8 ParamCount; {u32,u32}[] }[PassCount] }[TechniqueCount]
```

`ShaderGroup` and `Shader` records:

```
ShaderGroup: u8 ShaderCount; string Name; u8 Unk1; u8 Unk2; u32 Unk3; Shader[ShaderCount-1]
Shader:      string Name; string[] Variables; u8 BufferCount; { string Name; u16 Slot }[BufferCount];
             u32 DataLength; byte[DataLength] ByteCode;   //  <- raw DXBC blob
             u8 VersionMajor; u8 VersionMinor             //  present only for groups 0(VS),1(PS),4(GS)
```

Strings are length-prefixed: `u8 len`, then `len-1` ASCII chars + NUL. `FxcVariable.Type` codes are enumerated in
`src/smcore/legacy.py` (`FXC_VAR_TYPES`: 2=float … 6=texture, 7=bool, 11=int, 15=buffer, 21=uav buffer, 22=uav
texture). Note that the `.fxc` stores a shader-model version **only** for VS/PS/GS; for CS/DS/HS the real stage +
model come from the blob's own `SHDR`/`SHEX`/`DXIL` token
(a public third-party shader-manager tool).

### 1.3 `rgxa` — pre-Gen8 RAGE title / a racing title on the same engine `.fxc`

Documented layout (LibertyRecomp):

```
Magic: "rgxa" (0x61786772)
u8    Vertex Fragment Count
        Vertex Fragments[]: u8 VariablesCount; Variables[]{type,index,name};
                            u16 ShaderSize; u16 ShaderSize2; bytecode[ShaderSize]
u8    Pixel Fragment Count
u8[5] Unknown
        Pixel Fragments[]  (same structure as vertex)
...   additional metadata (global/local variables, techniques)
```

(https://github.com/OZORDI/LibertyRecomp/blob/main/docs/SHADER_PIPELINE.md). The RAGE-Shader-Editor C# struct
`FXC_IV` matches this, adding the trailing per-fragment *name* strings that only exist in the IV/a racing title on the same engine variant:
`magic; u8 vertexFragmentCount; FRAGMENT[]; u8 pixelFragmentCount; u8 unk1; u32 unk2; FRAGMENT[]; u8
globalVariablesCount; VARIABLE[]; u8 localVariablesCount; VARIABLE[]; u8 techniquesCount; TECHNIQUE[]; then
names[]` (https://github.com/ImpossibleEchoes/RAGE-Shader-Editor/blob/main/Structures.cs).

`rgxa` and `rgxe` are **not** distinguished by a numeric version field — only by the fourth magic byte (`a` vs `e`).
`FRAGMENT.shaderSize2` exists in every RAGE title *except RDR* per the struct comment in `Structures.cs`.

### 1.4 SGD2 — current-Gen RAGE title shader libraries (`.awc`)

The `.awc` extension is shared with audio wave containers; the shader libraries are distinguished solely by the
`SGD2` magic. Documented record layout (a public third-party shader-manager tool):

```
"SGD2"
6 arrays in order VS, PS, GS, DS, HS, CS:
    u32 count
    { u8 name_len; char name[]; u8 wavesize; u32 size; byte blob[size];
      u64 sga_hash; byte root_sig_summary[144]; u32 block_size; byte register_block[block_size] }[count]
effects trailer (techniques/passes/samplers, indices into the six stage arrays)
```

The 144-byte "root-sig summary" corresponds to the `RTS0` root-signature chunk inside each blob; CodeWalker's fork
reports 37,992 shaders across 6 archives, each embedding an `RTS0` chunk, and states that re-signing an SM6
container is not possible in managed code
(https://github.com/crxhvrd/CodeWalkerProjects/blob/master/README.md).

### 1.5 SGD1 / SGDB — RDR3

The shadermanager source calls SGD1/SGDB "the sibling format (RDR3 fxdb)" and references `FxdbEffect.Read()` in
CodeX (a public third-party shader-manager tool). **[UNVERIFIED]** at
byte level — I found no public byte-layout documentation for SGD1/SGDB.

### 1.6 console 360 Xenos shader container (embedded in IV/a racing title on the same engine/MP3/RDR `.fxc`)

Documented by LibertyRecomp:

```
u32 Flags             (magic 0x102A11XX; low byte encodes stage)
u32 Virtual Size      (big-endian)
u32 Physical Size     (big-endian)
u32 Field C
u32 Constant Table Offset
u32 Definition Table Offset
u32 Shader Offset
u32 Field 1C          (must be 0)
u32 Field 20          (must be 0)
... shader microcode and metadata
```

(https://github.com/OZORDI/LibertyRecomp/blob/main/docs/SHADER_PIPELINE.md). Independently, hedge-dev's
XenosRecomp says the console 360 shader container "includes constant buffer reflection data, definitions,
interpolators, vertex declarations, instructions", reverse-engineered "just enough for use in Unleashed
Recompiled" (https://github.com/hedge-dev/XenosRecomp). Xenia's `docs/gpu.md` is cited as the reference for Xenos
GPU behaviour.

RAGE-Shader-Editor detects the console vs PC variant by the first four bytes of a fragment's bytecode:
PC `vs_3_0` = `00 03 FE FF` (`0xFFFE0300`), PC `ps_3_0` = `00 03 FF FF` (`0xFFFF0300`), and throws
`"Unknown Shader"` for the Xenon pair `10 2A 11 01` / `10 2A 11 00`
(https://github.com/ImpossibleEchoes/RAGE-Shader-Editor/blob/main/ShaderUtils.cs).

### 1.7 Standard Microsoft containers that RAGE re-uses

* **D3D9 SM3 token stream** (pre-Gen8 RAGE title/MP3 PC): version token `vs_3_0 = 0xFFFE0300`, `ps_3_0 = 0xFFFF0300`;
  the constant table token `CTAB = 0x42415443` (`"CTAB"`) is written by RAGE-Shader-Editor's rebuild path
  (`shader.CTAB = 1111577667` = `0x42415443`).
* **DXBC container** (Gen8 RAGE title, SM4/5): `"DXBC"` | 16-byte digest | `u32 1` | `u32 total size` |
  `u32 chunk count` | `u32 chunk offsets[]`; each chunk is `fourcc | u32 size | data`
  (a public third-party shader-manager tool). The same header shape is used by
  DXIL containers; stage/model come from the `SHEX`/`SHDR` chunk (DXBC) or the `DXIL` chunk (DXIL), via the
  version word `(ver >> 16)`=stage kind, `(ver >> 4) & 0xF`=major, `ver & 0xF`=minor. The fxc-converter tool
  locates blobs by scanning for `"DXBC"` and reads the blob length from **offset +24** of that header
  (a public third-party RAGE shader compiler (source not linked)).

---

## Q2. Does Gen8 RAGE title ship compiled shaders, or compile at runtime?

**It ships compiled shaders.** The entire modding toolchain operates on pre-compiled blobs inside game archives:

* Gen8 RAGE title: `.fxc` effect files under `update/update.rpf/common/shaders` (the batch "a public shader unpacker"
  and fxc-converter both target that path — a public third-party shader unpacker (source not linked),
  a public third-party RAGE shader compiler). Blobs inside are raw `DXBC`.
* current-Gen RAGE title: `.awc` / `SGD2` libraries, ~37,992 shaders, blobs are SM6 `DXIL` with embedded `RTS0` root
  signatures (https://github.com/crxhvrd/CodeWalkerProjects/blob/master/README.md).
* pre-Gen8 RAGE title: LibertyRecomp states shaders are "**pre-compiled at build time** using XenosRecomp and embedded
  directly into the binary", with "**no runtime fallback**" (https://github.com/OZORDI/LibertyRecomp/blob/main/docs/SHADER_PIPELINE.md).
* The only *text* shader-side assets are pre-Gen8 RAGE title's `.sps` shader **preset** files — plain-text material→FXC maps
  with default parameters, "NOT compiled bytecode", in `common/shaders/db/` (131 files, mapping to 68 unique FXC
  shaders) (same source).

**What is known about the RAGE shader compiler:** publicly, nothing authoritative. The only "shader compiler"
strings in RAGE tooling are wrappers around **Microsoft's** compiler — RAGE-Shader-Editor ships `dlls\fx.dll`
exporting `bytecode_to_asm`, `asm_to_bytecode`, `hlsl_to_bytecode`, `get_hlsl_shader_size`, i.e. it forwards to
D3DX/`fxc` (https://github.com/ImpossibleEchoes/RAGE-Shader-Editor/blob/main/ShaderUtils.cs); fxc-converter ships
`fxc.exe`, `dxc.exe`, `dxcompiler.dll`, `dxil.dll` and the third-party `cmd_Decompiler.exe`
(a public third-party RAGE shader compiler). Whether RAGE has an internal compiler that generates the shipped
variants at *build* time is **[UNVERIFIED]** — the shipping artefacts are compiled, and the variants are
pre-generated (e.g. Legacy ships `win32_40_final`, `win32_40_lq_final`, `win32_nvstereo_final`, each compiling the
same effects to different blobs — a public third-party shader-manager tool).

---

## Q3. Any public reference to `FSSM`, a `0x101`-versioned shader container, or `0xDEADBEEF` padding?

* **`FSSM` as a shader magic: none found.** Web search for `"FSSM" shader`, `"FSSM" magic number file signature`,
  `"FSSM" the target title shader`, `FSSM the publisher` returns zero relevant results (only unrelated acronyms: a Nigerian
  college, a Polish police federation, a sleep-medicine congress, an F-Secure process `fssm32`). GitHub code
  search for `"FSSM" shader` is gated behind sign-in, but the repository/issue/PR facets show **0 repositories, 0
  issues, 0 PRs, 0 discussions, 0 users** for that query
  (https://github.com/search?q=%22FSSM%22+shader&type=code). No RAGE shader repo I downloaded (a public shader-manager tool,
  RAGE-Shader-Editor) contains the string `FSSM`.
* **A `0x101` (257) version field on a shader container: none found.** Documented RAGE containers do not carry a
  numeric version at all — they version by magic byte (`rgxa`/`rgxe`) or by magic string (`SGD1`/`SGD2`/`SGDB`).
* **`0xDEADBEEF` as a sentinel in RAGE shader formats: none found.** Worth noting the *near-miss*: RAGE-Shader-Editor's
  SM3 comment-block rebuilder fills its alignment padding with byte `171` = **`0xAB`**, not `0xDEADBEEF`
  (`for (int a = 0; a < trash.Length; a++) trash[a] = 171;` —
  https://github.com/ImpossibleEchoes/RAGE-Shader-Editor/blob/main/ShaderUtils.cs). So a documented RAGE shader
  writer does use a fixed sentinel padding byte, but `0xAB`, not `0xDEADBEEF`. `0xDEADBEEF` itself is not a
  documented RAGE shader constant.

**Conclusion for Q3: `FSSM` / `0x101` / `0xDEADBEEF` in this combination has no public prior art.** Anything about
them is currently first-hand.

---

## Q4. What do the HLSL decompile/recompile tools actually parse?

They parse **standard Microsoft bytecode**, never the publisher bytecode. The title-specific work is only in the
*container*:

| Tool | Container parsed | Bytecode handled | Decompiler/compiler used |
|---|---|---|---|
| CodeWalker `FxcFile.cs` / `FxcParser.cs` | `rgxe` `.fxc` | DXBC blob passed through opaquely | — (viewer/editor) |
| a public shader-manager tool | `rgxe` `.fxc` + `SGD2` `.awc` | DXBC (SM5) / DXIL (SM6) | `fxc.exe`, `dxc.exe`, `cmd_Decompiler.exe` (HLSLDecompiler), `dxbc2dxil.exe` |
| a public fxc-converter tool | `.fxc` blob scanner | raw DXBC | `cmd_Decompiler.exe` |
| ImpossibleEchoes/RAGE-Shader-Editor | `rgxa`, `rgxe` | **D3D9 SM3 tokens** (PC) | `dlls\fx.dll` → D3DX disassemble/assemble/compile |
| hedge-dev/XenosRecomp | console 360 Xenos container | **Xenos microcode** → HLSL | own recompiler → DXC (DXIL/SPIR-V) |
| OZORDI/LibertyRecomp `rage_fxc_extractor` | `rgxa` `.fxc` | console 360 Xenos binaries | XenosRecomp |

The stage/model for Gen8 RAGE title blobs is read straight out of the Microsoft chunk (`SHEX`/`SHDR`/`DXIL`), and the
"decompiler artifacts" the shadermanager warns about are D3D decompiler artifacts, not RAGE ones
(a public third-party shader-manager tool). So: **the bytecode layer is DXBC/DXIL; the the publisher layer is the
wrapper and the effect metadata.**

---

## Q5. Verdict: is `FSSM` plausibly a descendant of a known RAGE shader format?

### Verdict

**Plausibly a *sibling* in the same design family, but not a documented descendant of any specific known RAGE
container — and the one structural detail we have actually argues *against* a direct lineage to `rgxe`/`SGD2`.**

Arguments **for** family membership (weak-to-moderate):

1. `FSSM` is a **4-byte ASCII magic**, which is exactly the RAGE idiom: `rgxa`, `rgxe`, `SGD2`, `SGDB`, and
   (outside shaders) `RSC7`, `RPF7`, `RBF0`. the publisher names containers with short ASCII tags far more often than
   with numeric magics.
2. A publisher-owned wrapper carrying per-shader metadata around an otherwise-standard Microsoft shader container
   is precisely what `rgxe` (around DXBC) and `SGD2` (around DXIL, with a 144-byte root-sig summary and a register
   block) already are. `FSSM` sitting adjacent to a DXBC/DXIL container is the same *pattern*.
3. `rgxe` already versions itself by a magic character and has no numeric version; the *introduction* of an
   explicit `0x101` version field is a natural evolution for a 2025-era format and does not contradict RAGE style.

Arguments **against** a direct `rgxe`/`SGD2` descent (the decisive one):

4. **The nesting is inverted.** Every documented RAGE container (`rgxe`, `rgxa`, `SGD2`) sits **outside** the
   Microsoft bytecode — the DXBC/DXIL blob is an opaque payload field *inside* the the publisher container. In the the target title
   description, `FSSM` is reported as being **inside** a DXBC-shaped container (whose chunk table lists
   `SFI0;ISG1;OSG1;PSV0;HASH;DXIL;PRIV` — note `DXIL` and `HASH`, i.e. a **DXIL** container, not legacy DXBC).
   A the publisher container nested *inside* Microsoft's container is a different composition from anything publicly
   documented. `FSSM` is therefore more likely a **per-shader private/reflection wrapper** (a `PRIV`-chunk-style
   payload, or a header inside the DXIL container) than a re-badged `rgxe`/`SGD2`.
5. Nothing in the documented metadata set matches: `rgxe`/`rgxa` carry variables/constant-buffers/techniques;
   `SGD2` carries stage arrays + effects trailer + `RTS0` summary. We have no evidence `FSSM` reuses any of those
   record layouts, and its `0x101` version field has no counterpart.

**Net:** treat `FSSM` as a **new, undocumented member of the RAGE ASCII-magic container family** whose *role*
(metadata wrapper around vendor bytecode) is familiar, but whose *layout and nesting* are not documented anywhere
public. The reusable prior art is the surrounding plumbing — DXIL container parsing (`dxbc.py`), blob scanning,
HLSL decompile via DXC — not a ready-made `FSSM` parser. Budget for first-hand reversing of the `FSSM` header
itself.

### Cheapest confirming experiment

**Do a byte-level structural diff of two `FSSM` headers, and check where `DXBC`/`DXIL` sits relative to `FSSM`.**
Concretely, on the the target title Scarlett shader library already in hand (no root, no installs needed):

1. In Python, `grep` the decompressed XBCH payloads for the byte strings `b"FSSM"` and `b"DXBC"` and record their
   offsets relative to the start of each blob. **If `DXBC` precedes `FSSM`, `FSSM` is inside the Microsoft
   container** (the inversion in argument 4 is confirmed → not a direct `rgxe`/`SGD2` descendant). If `FSSM`
   precedes `DXBC`, it is an outer wrapper like `rgxe`/`SGD2`.
2. Dump the 64 bytes at `FSSM`: confirm `u32 0x00000101` follows the magic, and check whether the next fields are
   counts (the RAGE `magic → u32 → u8 count` shape) or a fixed header. Then `xxd` the whole header region.
3. Diff two `FSSM` blobs of the **same stage** (should be identical up to the variable region) and one **VS vs one
   PS** (the differing byte identifies the stage field) — this alone recovers the header layout for free.
4. Cross-check the recovered field widths against the documented `rgxe` header above: if the u32 after the magic
   occupies the same slot as `rgxe`'s `VertexType` and a `u8` count follows, lineage is strongly supported; if the
   header is a flat fixed-size block, it is a new design.

This is minutes of work on existing bytes, needs nothing installed, and is decisive on the lineage question
regardless of which way it falls.

---

## Repos / tools worth cloning or reading further

| Repo / page | Why |
|---|---|
| https://github.com/dexyfex/CodeWalker (`CodeWalker.Core/GameFiles/FileTypes/FxcFile.cs`, `CodeWalker/GameFiles/FxcParser.cs`) | Canonical Gen8 RAGE title `rgxe` parser (`magic != 1702389618 → bail`) |
| a public third-party shader-manager tool (`src/core/fxc_parser.py`, `src/smcore/legacy.py`, `src/smcore/enhanced.py`, `src/smcore/dxbc.py`, `src/awclib/`) | Full `rgxe` + `SGD2` readers/writers, DXBC/DXIL container logic, blob hashing |
| https://github.com/crxhvrd/CodeWalkerProjects | CodeWalker fork that adds `SGD2` `.awc` shader-library read/write (37,992 shaders, `RTS0` handling) |
| https://github.com/ImpossibleEchoes/RAGE-Shader-Editor (`Structures.cs`, `ShaderUtils.cs`, `IV.cs`, `MP3.cs`) | `rgxa` + `rgxe` structs, SM3 token details, Xenon detection, `0xAB` padding |
| https://github.com/ImpossibleEchoes/rage-shader-editor-cpp | C++ rewrite of the above |
| https://github.com/OZORDI/LibertyRecomp (`docs/SHADER_PIPELINE.md`, `docs/SPS_SHADER_PRESETS.md`, `tools/rage_fxc_extractor/`) | `rgxa` layout, console 360 Xenos container layout, `.sps` preset format |
| https://github.com/hedge-dev/XenosRecomp | console 360 Xenos shader container → HLSL recompiler |
| a public third-party RAGE shader compiler | Minimal `.fxc` blob scanner/patcher (DXBC magic + size at +24) |
| https://github.com/xenia-project/xenia/blob/master/docs/gpu.md | Xenos GPU/shader container reference |
| a public third-party RAGE shader editor (forum thread not linked) | RAGE Shader Editor thread (IV/MP3, console shader tooling) — Cloudflare-gated |
| a public third-party write-up of the RAGE formats (not linked) | Gen8 RAGE title RAGE file-format reference (RPF/RSC/ytyp; has a ped-materials page that reads shipped shaders) — **no shader-container page found** |

## Open / unverified items

* `SGD1`/`SGDB` (RDR3 fxdb) byte layout — **[UNVERIFIED]**.
* Whether RAGE has an internal build-time shader compiler that generates the shipped variants — **[UNVERIFIED]**.
* Whether `FSSM` is inside or outside the DXIL container — see the experiment in Q5.
* Scarlett `XBCH` section semantics — no public documentation found; treat as **[UNVERIFIED]**.
