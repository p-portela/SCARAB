# Scarlett (Scarlett) D3D12 shader ABI vs PS5 — notes for reusing the upstream shader recompiler

**Question.** We hold the target title (Scarlett) shader microcode in the publisher's `FSSM` container:
raw AMD RDNA2 / gfx10 GPU machine code (verified: `s_endpgm` in 25,782/25,782 blobs, see
`docs/fssm-analysis.md`). We want to reuse the upstream shader recompiler — a PS5 (gfx1013, RDNA2) **RDNA2 → SPIR-V**
recompiler whose input is `ShaderBinary { stage, codeAddress, code[], headerAddress, header[] }`
— as the back end. Its current "header" is the PS5 AGC `Shader` struct. **What replaces that
header on the console side, and how does an console D3D12 shader obtain its resource bindings at the
ISA level?**

**Method / confidence.** Claims carry a URL. Anything not confirmed from a primary source is marked
`[UNVERIFIED]`. Local source citations are to `the recompiler source tree` and this repo's
`docs/`. `web_extract` was unavailable; pages were fetched with `curl -sL -A 'Mozilla/5.0'` and
stripped, or read from `raw.githubusercontent.com`. The console SDK *shader-compiler* pages are
**NDA-gated** — their bodies could not be read; only their public titles/metadata are used. AMD's
`RDNA 2 ISA` PDF was downloaded and text-extracted locally (`pdftotext`).

---

## Q1. User-SGPR convention: what holds what, and the console equivalent of the "PS5 header"

### The hardware mechanism is the same on both consoles

Every GFX10/RDNA2 hardware stage has a set of **32-bit physical SPI *user data registers*** (16 or 32
per stage depending on graphics IP and stage). They are written from the command buffer and
"loaded into SGPRs when waves are launched via a subsequent dispatch or draw operation. This is the
way most arguments are passed from the application/runtime to a hardware shader."
— LLVM "User Guide for AMDGPU Backend", AMDPAL → **User Data** section
(<https://llvm.org/docs/AMDGPUUsage.html>, source at
<https://raw.githubusercontent.com/llvm/llvm-project/release/18.x/llvm/docs/AMDGPUUsage.rst> lines
14685–14690).

So the *mechanism* — driver writes SPI user-data registers, hardware copies them into SGPRs at wave
launch — is a GPU property, identical for PS5, Scarlett, and PC RDNA2. **The difference is who
decides the layout and where it is described.**

The per-stage SPI registers are visible in the ISA: `SPI_SHADER_USER_DATA_LO_GS` /
`SPI_SHADER_USER_DATA_HI_GS` / `_HS` supply the "GS/HS User Data Address" low/high dwords
(*RDNA 2 Instruction Set Architecture Reference Guide*,
<https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf>, §"GS/HS User Data
Address"). The compiler-side count is programmed in the PGM_RSRC2 register:
`compute_pgm_rsrc2.user_sgpr.user_sgpr_count` (AMDGPUUsage.rst line 4452) — the same field
the upstream shader recompiler reads as `userCount`.

### PS5 convention (from the upstream shader recompiler source)

On PS5 the shader **carries its own ABI in a header** — that header *is* `ShaderBinary.header`.
The struct is in `core/libs/SceShaders.hpp:68-89` (this is the PS5-side "shader header"):

```
struct Shader {                       // PS5 AGC shader header
  u32 file_header;                    // SHADER_FILE_HEADER_MAGIC = 0x34333231 ("1234")
  u32 version;                        // SHADER_VERSION = 0x18
  ShaderUserData*  user_data;         // -> SRT / direct-resource map  (the binding info)
  const volatile void* code;
  ShaderRegister*  cx_registers;      // (offset,value) context-register pairs
  ShaderRegister*  sh_registers;      // (offset,value) shader-register pairs  <-- incl. PGM_RSRC2
  ShaderSpecialRegs* specials;        // vgt_shader_stages_en, dispatch_modifier, user_data_range...
  ShaderSemantic*  input_semantics;   // vertex input layout
  ShaderSemantic*  output_semantics;
  u32 header_size, shader_size, embedded_constant_buffer_size_dqw, target;
  u32 num_input_semantics;
  u16 scratch_size_dw_per_thread, num_output_semantics, special_sizes_bytes;
  u8  type /*stage*/, num_cx_registers, num_sh_registers;
};
struct ShaderUserData {
  u16* direct_resource_offset;        // pointer table of "direct" resources
  ShaderSharp* sharp_resource_offset[4];  // V#/T#/S# descriptor tables (the SRT)
  u16 eud_size_dw; u16 srt_size_dw;
  u16 direct_resource_count; u16 sharp_resource_count[4];
};
```

The driver front-end derives the guest `GuestContext` from it
(`core/libs/prx/libSceAgcDriver/Execution/src/Driver/Shaders/ShaderRegistry.cpp`):

- **user-SGPR base + count**: `resources = state.shader[resourceRegister]` where `resourceRegister`
  is `COMPUTE_PGM_RSRC2 = 0x213` (CS), `SPI_SHADER_PGM_RSRC2 = 0x00b` (PS) / `0x08b` (VS/GS),
  `0x10b` (HS); then `userCount = (resources >> 1) & 0x1f` plus the extra bit-27 bit for non-CS.
  Vertex-family stages add 8 (the low 8 SGPRs are reserved), so `firstUser = 8` for vertex — line
  ~1375–1420. `userDataBaseRegister` is passed into `GuestContext` (Recompiler.hpp:305).
- **wave size**: CS → `specials.dispatch_modifier & 0x8000`; PS → `DecodePixelStageInfo().wave32`;
  vertex family → `LOC 0x2d5` bits. (ShaderRegistry.cpp:1385-1396.)
- **compute geometry/LDS**: `COMPUTE_NUM_THREAD_X/Y/Z`, `RSRC2>>15 & 0x1ff * 128` LDS dwords,
  scratch, granulated VGPRs (`Graphics/src/ShaderInputState.cpp:76-110`).
- **SRT / resource layout**: `DecodeVertexStageInfo(header, headerAddress, userData, ...)` reads
  `header.user_data` (`direct_resource_*`, `sharp_resource_*`, `srt_size_dw`, `eud_size_dw`)
  (`ShaderInputState.cpp:232+`).

**Crucially, the recompiler engine never dereferences `header`.** Grepping `header` across
`core/shader/recompiler/**` returns only `RequestSerializer.cpp:274-286` (serialize/deserialize),
`Deserialize.cpp:92` (prints its size), and unrelated CFG "loop header"/"table header" identifiers.
`RecompileImpl` (`Recompiler.cpp:1761`) and the translation/optimization passes never touch
`binary.header`. **Consequence: for an console shader the `header` span can be empty; everything the
AGC driver would have derived from it must instead be supplied directly in `GuestContext`
(`waveSize`, `userDataBaseRegister`, `userData`, `compute`/`pixel`/`vertex`, `memory`).**
(Full detail: `docs/the recompiler integration notes`.)

### console / D3D12 convention

On D3D12 there is **no per-shader ABI header**. The binding contract lives in the **root signature**,
which is owned by the application/PSO, *not* the shader:

- A root signature is "an arbitrarily arranged collection of descriptor tables (including their
  layout), root constants and root descriptors"; max 64 DWORDs; a descriptor table costs 1 DWORD, a
  root constant 1 DWORD, a root descriptor (a **64-bit GPU virtual address**) 2 DWORDs.
  — Microsoft Learn, *Root Signature Limits*
  (<https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits>) and
  *Root Signatures* (<https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures>).
- Descriptor tables point into a **shader-visible descriptor heap** of CBV/SRV/UAV and sampler
  descriptors — Microsoft Learn, *Descriptor Heaps*
  (<https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps>).
- How that reaches the shader's constant/user-data space: a root-table parameter is "an offset into
  the `ID3D12DescriptorHeap`… placed in push constants"; a root CBV/SRV/UAV descriptor is a raw
  64-bit pointer delivered as a **Buffer Device Address**.
  — Maister, *My personal hell of translating DXIL to SPIR-V* part 4
  (<https://themaister.net/blog/2022/04/11/my-personal-hell-of-translating-dxil-to-spir-v-part-4/>)
  and part 2 (<https://themaister.net/blog/page/2/>).
- On AMD hardware those push constants / root-signature entries are delivered through the **SPI
  user-data registers** (Q1 mechanism above). AMD's own graphics runtime model (AMDPAL) virtualises
  this as 128 "user data entries" mapped to physical user-data registers, with **user-data register
  0 = GlobalTable pointer, register 1 = PerShaderTable pointer**, plus a fixed vocabulary of driver
  entries: `SpillTable`, `BaseVertex`, `BaseInstance`, `DrawIndex`, `VertexBufferTable`,
  `UavExportTable`, `StreamOutTable`, `ViewId`, `PerShaderPerfData`, `NggCullingData`,
  `FetchShaderPtr` … — AMDGPUUsage.rst, AMDPAL *User Data* mapping table, lines 14711–14763.
  This is the AMD driver's PC/Vulkan model, **not** necessarily console's driver, but it demonstrates
  the exact hardware slot for "root signature data" and its layout vocabulary.

### Answer to Q1

- **Hardware**: identical on both — driver-written SPI user-data registers → user SGPRs.
- **PS5**: the *shader* carries the ABI. The AGC `Shader` header (via `header.user_data`,
  `sh_registers`/`PGM_RSRC2`, `specials`) tells the driver the user-SGPR base/count, wave size,
  LDS, and the SRT layout. the upstream shader recompiler consumes this only through the driver front-end, never in the
  engine.
- **console/D3D12**: the *pipeline* carries the ABI. There is **no per-shader header**; the layout is
  the root signature, and the descriptor locations are heap offsets / 64-bit BDA values handed to
  the shader as user SGPRs by the D3D12 driver. **What replaces the PS5 header is therefore two
  things**: (1) the **PSO's root signature + the D3D12 driver's user-data layout** (runtime state,
  not shader state), and (2) the **DXIL reflection chunks** (`SFI0/ISG1/OSG1/PSV0/HASH`) in the
  outer DXBC-shaped container, which describe the resource *interface* but not the register-level
  user-SGPR map.
- The exact user-SGPR numbering for XSX/XSS stage-by-stage is **not public** (the console SDK shader pages
  are NDA-gated). Anything precise about console's user-data slot assignment is `[UNVERIFIED]` and must
  be recovered from the FSSM binding table + runtime validation.

---

## Q2. Descriptor model: SRT (PS5) vs D3D12 descriptor heap / root signature (console)

Both models end in **the same AMD hardware descriptors** — the difference is *where the words live*
and *how the shader reaches them*.

### The AMD descriptor primitives (shared)

From the *RDNA 2 ISA* PDF §1.0 glossary:
- **V# (buffer resource descriptor)** — "a 128-bit value in SGPRs" (4 consecutive SGPRs), defines a
  buffer's base address/format/stride (§8.1, §"All VM operations use a buffer resource constant
  (V#) which is a 128-bit value in SGPRs").
- **T# (image/texture resource descriptor)** — "a 128-bit or 256-bit value in SGPRs" (4 or 8
  consecutive SGPRs) (§8.2, "All VM operations use an image resource constant (T#)…").
- **S# (sampler descriptor)** — "a 128-bit entity [in four consecutive SGPRs]" (§1.0).

### PS5 — the SRT (Shader Resource Table)

- The shader's **user data** points at a *Shader Resource Table*: the AGC header's `ShaderUserData`
  splits it into **"direct resources"** (`direct_resource_offset`, a pointer table) and **"sharp
  resources"** (`sharp_resource_offset[4]`, tables of V#/T#/S# descriptors = "sharps"), sized by
  `srt_size_dw`, plus an **EUD** (extended user data) region `eud_size_dw`
  (`core/libs/SceShaders.hpp:6-28`).
- The descriptor words live in **guest memory** described by the SRT; the shader does scalar
  loads (`s_load_dwordx4` etc.) to fetch V#/T#/S# into SGPRs before use — the ISA notes these
  constants are "typically fetched from memory using scalar memory reads prior to executing VM
  instructions" (RDNA2 ISA §8.1/§8.2).
- the upstream shader recompiler models the destination of this in its runtime ABI: `RuntimeAbi.hpp` has
  `Binding::FlattenedSrt`, `Binding::SamplerTable`, `Binding::ImageTableMap`, `Binding::ImageTable`,
  `Binding::BdaPagetable`, and per-image/sampler heaps (`ShaderData` with `userData[128]`, an
  `images[256]` array, a `samplers[64]` array, each with a 16-byte `descriptor` = a V#/T# word
  group). The SRT walk is done by `Optimization/src/SrtWalker.cpp` +
  `ResourceMaterializer.cpp`; the format is decoded in `RdnaDecoder/.../RdnaDescriptorFormat.hpp`
  (8-dword T# handling: `GcnImageWords(std::span<const uint32_t,8>)`).

### console / D3D12 — descriptor heaps + root signature

- Descriptors live in a **shader-visible descriptor heap**: "a collection of contiguous allocations
  of descriptors… Shader Resource Views (SRVs), Unordered Access Views (UAVs), Constant Buffer Views
  (CBVs), and Samplers" (Microsoft Learn, *Descriptor Heaps*,
  <https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps>).
- The shader reaches them via a **descriptor table**, which is a *descriptor-heap offset* delivered
  in the root signature/push-constant block, or via a **root descriptor** = a 64-bit GPU VA (BDA)
  (Root Signature Limits; Maister, above). Hardware-wise this is again a user SGPR holding either a
  heap-pointer/offset or a raw 64-bit address.
- The **descriptor words themselves are the same AMD SRDs**: a D3D12 CBV/SRV/UAV heap slot is sized
  to hold an image resource descriptor (an SRD up to 32 bytes) and a sampler slot a S# (16 bytes).
  The exact D3D12 heap increment sizes are driver-defined (`GetDescriptorHandleIncrementSize`)
  `[UNVERIFIED]` as specific byte values here, but they must be ≥ the hardware SRD sizes above.
- Modern D3D12 (Shader Model 6.6) additionally exposes **bindless** access via
  `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]`, where the shader indexes the heap directly
  by a 32-bit index — a third way to reach the same heap.
  `[UNVERIFIED — cited from the DirectX-Specs/SM6.6 material, not re-fetched here.]`

### Where the console descriptor words live in memory

In a **D3D12 descriptor heap** — a flat, GPU-visible array the driver allocates — not in a
publisher/PS5 SRT. The shader obtains either (a) a heap base + index (table), or (b) a raw 64-bit GPU
address (root descriptor / BDA). Either way it then does the same `s_load_dwordx4`-style scalar
fetch of a V#/T#/S# as on PS5. **Net: the ISA-level descriptor encoding is shared; the containment
model is not** (PS5 SRT-in-guest-memory vs D3D12 descriptor-heap-offset-in-root-signature).

---

## Q3. Is Scarlett microcode the same RDNA2/gfx10 encoding? What is "Scarlett" in gfx terms?

- **Scarlett GPU** is a custom AMD RDNA2 part; TechPowerUp names the XSX die **"Scarlett"**
  (XSS = **"Lockhart"**), RDNA2, 7 nm, DirectX 12 Ultimate (feature level 12_2), 52/20 CUs
  (<https://www.techpowerup.com/gpu-specs/console-series-x-gpu.c3482>,
  <https://www.techpowerup.com/gpu-specs/amd-lockhart.g970>). Console GPU codenames are
  `Scarlett` = Series X|S, `Durango` = console One.
- **PS5** GPU is `gfx1013` (`Oberon`) — the upstream shader recompiler's own progress page states "The PS5 uses gfx1013, but
  this public-source profile is not a complete console ISA or hardware verification"
  (<https://boykopovar.github.io/the upstream shader recompiler/progress.html>).
- **The machine code is the gfx10 family.** the upstream shader recompiler's decoder and our own the target title classifier dispatch
  on the first dword; the VOP3 discriminator is `0x35 << 26` (0xD4–0xD7) on **gfx10**, whereas gfx9
  uses `0x34` (0xD0–0xD3). the target title's FSSM code uses 0xD4/0xD5/0xD7 → gfx10 family
  (`docs/fssm-analysis.md`; skill reference `reverse-engineering/console-gdk-reversing`,
  `references/gpu-isa-vs-ir.md`). gfx10 families (`gfx1010`, `gfx1013`, `gfx1030`…) share one
  encoding table and one AMD machine-readable ISA XML (`amdgpu_isa_rdna2.xml`, mirrored in-repo as
  `tools/rdna2_isa_forms.txt`).
- **Platform-specific opcodes/extensions**: the *encoding* is the same for the whole gfx10 line, but
  gfx10.3 (RDNA2, "full RDNA2") adds instructions that a gfx1013-targeted decoder may not
  implement. Do **not** use `s_clause` / `s_inst_prefetch` as an RDNA2-only tell — the reference
  notes they appear in both gfx1013 and RDNA2. Which specific opcodes the target title uses that gfx1013
  lacks is `[UNVERIFIED]` and is a concrete test (disassemble with the upstream shader recompiler vs AMD RGA).
- **Exact gfx target string for the console is not public.** Conventionally the Scarlett GPU
  would be the gfx10.3 family (PC analogue `gfx1030` = Navi 21, RX 6800/6900,
  <https://github.com/ssubedir/RCOm-windows-gfx1030>), but no primary source states the console's
  gfx target → `[UNVERIFIED]`.
- Functional (not ISA) difference worth recording: **Scarlett exposes D3D12 mesh shaders** (part
  of "the Scarlett features" carried into DX12 Ultimate on PC,
  <https://arstechnica.com/gaming/2020/03/directx-12-ultimate-brings-console-series-x-features-to-pc-gaming/>),
  which is why the target title's library is mesh-shader-dominated. PS5's geometry engine is documented
  publicly as primitive-shader oriented, not D3D12-mesh — a reason a PS5-oriented front end will
  not already have an console mesh path.

---

## Q4. Do D3D12 shaders ship as DXIL? Is shipping raw RDNA2 ISA unusual?

- **Normal D3D12 shipping form is DXIL (SM6.x) in a DXBC-shaped container.** The console SDK compiles HLSL
  with **DXC** — "HLSL shader compiler (DXC) for the CONSOLE console SDK … Compile HLSL shaders for the CONSOLE console SDK
  with DXC" (<https://docs.console.com/tools/tools-console/shadercompiler>). PC RAGE titles confirm the
  convention: current-Gen RAGE title ships SM6 **DXIL** blobs each embedding an `RTS0` root-signature
  chunk (<https://github.com/crxhvrd/CodeWalkerProjects/blob/master/README.md>).
- On the console the driver lowers DXIL/DXBC to GCN/RDNA ISA. The console SDK's own (NDA-gated) shader
  pages advertise an **"console Iterative Compiler (XIC)"** and an **"NGG"** define
  (page keywords on
  <https://learn.microsoft.com/en-us/gaming/gdk/docs/tools/tools-console/shadercompiler/dxc-console-defines?view=gdk-2610>
  — body is NDA-gated), i.e. an offline/iterative native-ISA compilation path exists alongside DXC.
  The console SDK also documents **precompiling** D3D12 shaders off-device ("Advanced Shader Delivery …
  precompiles Direct3D 12 shaders and delivers them at download time … a State Object Database
  (SODB)", <https://docs.console.com/build/gdk-and-engines/guides/advanced-shader-delivery> — note this
  page frames ASD as a *Windows 11* D3D12 technology).
- **Therefore the target title shipping raw R-DNA2 ISA inside FSSM is unusual for D3D12** and implies the
  shaders were already lowered to GPU machine code before packaging (a driver/compiler-cache blob or
  a the publisher offline pipeline), not shipped as DXIL. Corroboration that the bytes are genuine
  compiler output: the FSSM code region is `s_code_end`-padded, matching AMD's rule that "the user
  must pad all shaders with 64 extra dwords (256 bytes) of data past the end of the shader… use the
  S_CODE_END instruction as padding" (RDNA2 ISA §2.5).
- **Implication for reuse:** there is no DXIL to translate — which is the *good* case against
  `docs/fssm-analysis.md`'s verdict (option 1, an ISA→SPIR-V translator, is buildable). It also
  means the D3D12 root-signature/reflection contract must be reconstructed from the outer DXIL
  reflection chunks plus FSSM's own binding table, since the program bytes carry no DXIL
  metadata. The title's runtime shader compiler (in the exe) remains an alternative source of that
  contract but is exe-gated.

---

## Q5. Mesh shaders: how do Scarlett mesh shaders receive payload / descriptor bindings?

- **Binding**: Mesh and amplification shaders use the **graphics root signature** (or dedicated mesh
  root-signature slots): "Mesh and Amplification shaders will use graphics root signature or their
  parameters. We will add shader visibility flags for Mesh and Amplification shader root signature
  slots." — Microsoft *DirectX-Specs*, `d3d/MeshShader.md`
  (<https://raw.githubusercontent.com/microsoft/DirectX-Specs/master/d3d/MeshShader.md>, lines
  ~105-107; also <https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html>).
- **Payload**: the amplification shader calls `DispatchMesh(ThreadGroupCountX, ThreadGroupCountY,
  ThreadGroupCountZ, MeshPayload)`; the mesh shader receives it as `in payload MeshPayloadStruct
  MeshPayload` via `GetMeshPayload()`. The payload is a group-shared blob, `<= 16k bytes`, "passed to
  all instances of Mesh shaders invoked by this call", "the same input values for the entire Mesh
  shader thread group", and **counts against groupshared limits** (28k mesh / 32k amplification).
  — MeshShader.md, *Mesh Payload* and *DispatchMesh - intrinsic* sections.
- **ISA-level (RDNA2/console)**: D3D12 mesh/amplification lowers onto the RDNA2 **NGG geometry
  engine**; the console SDK's shader-compiler defines page carries `NGG` as a keyword (see Q4 link),
  corroborating that console lowers geometry/mesh to NGG. The payload is passed through **LDS
  (groupshared)** and the mesh dispatch is a compute-like launch, while **user SGPRs carry the root
  signature entries exactly as for CS/VS** (Q1/Q2). The precise console user-data slot layout for mesh
  shaders is `[UNVERIFIED]`.
- **Contrast with the PS5 path in the upstream shader recompiler**: PS5's "Mesh" stage is already modelled
  (`ShaderStage::Mesh`, `GraphicsCompileContext.mesh`, `MeshConfiguration`, and mesh draw arguments
  delivered through the push-constant block: `MeshDrawPushOffsetBytes = 104`,
  `MeshDrawPushBytes = 24`, `MeshArgumentAddressDword`, see `Recompiler.hpp`), and mesh workgroup
  communication shows up at the ISA level as `s_barrier` + `s_sendmsg` — exactly what our FSSM
  classifier observed dominating the MS/MSNC/MSR/AMSNC blobs (`docs/fssm-analysis.md`). So the upstream shader recompiler
  *has* a mesh path, but it is sized for PS5's mesh draw-argument push constants, not D3D12's
  `DispatchMesh` payload/root-signature model.

---

## Q6. Public prior art for RDNA2/GCN ISA → SPIR-V (and related), with licenses

| Project | What it is | Reusable for us | License (verified via the repo's license file unless noted) |
|---|---|---|---|
| **the upstream shader recompiler** (Pedro's, local `the recompiler source tree`) | gfx1013 RDNA2 → SPIR-V recompiler; full decoder, CFG, SSA IR, SRT walker, SPIR-V emitter, AGC driver front-end | **The back end itself.** ISA decode, IR, resource materialization, SPIR-V emit are all directly reusable; only the front-end header/ABI is PS5-shaped | In-tree (project licence; not assessed here) |
| **shadPS4** (<https://github.com/shadps4-emu/shadPS4>) | PS4 emulator; **GCN (gfx6/gfx7) bytecode → IR → SPIR-V** pipeline (decode → CFG → IR → optimize → SPIR-V) — <https://deepwiki.com/shadps4-emu/shadPS4/3.4-shader-compilation-pipeline> | Sibling design of the same job (GCN→SPIR-V). Useful as a second opinion on IR/CFG and on non-RDNA GCN encodings; **not** RDNA2 | **GPL-2.0** (fetched `LICENSE`: "GNU GENERAL PUBLIC LICENSE Version 2") — copyleft, viral |
| **AMD Radeon GPU Analyzer (RGA)** (<https://github.com/GPUOpen-Tools/radeon_gpu_analyzer>) | Offline compiler + code-analysis tool for DXIL/DXBC, Vulkan SPIR-V, OpenGL, OpenCL → **RDNA/GCN ISA**, and ISA disassembly, independent of the installed GPU (<https://en.wikipedia.org/wiki/CodeXL>, repo README) | **Ground-truth disassembler** for validating our RDNA2 decoding, and an offline DXIL→ISA oracle | **MIT** (fetched `LICENSE.txt`: AMD copyright + MIT permission grant; repo also ships `EULA.txt`/`RGA_NOTICES.txt`) |
| **LLVM AMDGPU backend** (<https://llvm.org/docs/AMDGPUUsage.html>) | amdgcn code generation **and** a working amdgcn disassembler (`llvm-objdump -d --triple=amdgcn...`); the **AMDPAL User Data** documentation (Q1/A1) | The ABI model doc + an independent ISA disassembler for differential testing | **Apache-2.0 WITH LLVM-exception / UIUC-NCSA** (fetched `LICENSE.TXT`: "University of Illinois/NCSA Open Source License") |
| **Mesa** (RADV / ACO / NIR) (<https://docs.mesa3d.org/drivers/radv.html>) | RADV: SPIR-V→NIR→ACO→GCN/RDNA ISA; `ac_shader_args` documents the **Vulkan user-SGPR ABI** (descriptor sets, push constants) | Reference for the *Vulkan* user-SGPR layout (a third ABI to triangulate PS5 vs D3D12), and for descriptor→SRD handling | **MIT** (Mesa `COPYING`; the GitLab raw endpoint served an anti-bot page so this is cited from Mesa's published licensing `[UNVERIFIED exact file text]`) |
| **SPIRV-Cross** (<https://github.com/KhronosGroup/SPIRV-Cross>) | SPIR-V → GLSL/HLSL/MSL | Emit HLSL (DXC) or MSL from our SPIR-V if the target is not Vulkan | **Apache-2.0** (fetched `LICENSE`: "Apache License Version 2.0") |
| **SPIRV-Tools / spirv-dis** (Khronos) | SPIR-V binary ⇄ text, validation | Disassemble/validate the upstream shader recompiler's emitted SPIR-V (the upstream shader recompiler's build here has `SHADER_RECOMP_ENABLE_SPIRV_TOOLS=OFF`) | Apache-2.0 |
| **Xenia** (<https://github.com/xenia-project/xenia>) | console 360 (Xenos, *not* GCN) emulator with a shader translator and `docs/gpu.md` | Architectural precedent for translating a *console-specific* GPU ISA to a PC-targeted shader IR | **BSD-3-Clause** (fetched `LICENSE`: "Copyright (c) 2015, Ben Vanik. All rights reserved." + 3-clause text) |
| **XenosRecomp** (<https://github.com/hedge-dev/XenosRecomp>) | console 360 Xenos shader container → HLSL recompiler (powers Unleashed Recompiled) | Nearest precedent for **recompiling a console shader container into a PC shader**; not GCN | **MIT** (fetched `LICENSE.md`: "MIT License, Copyright (c) 2025 hedge-dev and contributors") |
| **vkd3d-proton** (<https://github.com/HansKristian-Work/vkd3d-proton>) | D3D12 → Vulkan; DXBC/DXIL → SPIR-V, and the canonical **root-signature/descriptor-heap → Vulkan descriptor** mapping | The reference for mapping D3D12 binding semantics (root sig, heaps, CBV/SRV/UAV/sampler) onto a Vulkan/SPIR-V model — i.e. the bridge we need between the console descriptor world and the upstream shader recompiler's `RuntimeAbi` | **LGPL-2.1** (fetched `LICENSE`: "GNU LESSER GENERAL PUBLIC LICENSE Version 2.1") — copyleft |
| **dxil-spirv** (<https://github.com/HansKristian-Work/dxil-spirv>) | DXIL → SPIR-V (used by vkd3d-proton) | Only if we later want a DXIL path; DXIL appears absent from the target title, so not on the critical path | `[UNVERIFIED]` (license file not found at the guessed path; commonly MIT) |
| **AMD machine-readable ISA XML** (`amdgpu_isa_rdna2.xml`), mirrored locally | Authoritative instruction-form tables | Already used by the upstream shader recompiler (`tools/rdna2_isa_forms.txt`); extend to full gfx10.3 forms | AMD-published (permissive; see GPUOpen) |
| **UMR (AMD User Mode Register debugger)** (`AMDGPU/umr`) | Dumps GPU registers/waves/microcode | Could capture live user-SGPR values to validate an console/PC ABI hypothesis | `[UNVERIFIED]` (license file not found at the guessed path) |

---

## What must be built to make a PS5-oriented recompiler consume an console shader

Concrete work items. Items 1–2 replace the PS5 "shader header"; the rest close the ABI/descriptor
gap. (1)–(4) are the core; this extends the PS5-side plan already in
`docs/the recompiler integration notes`.

1. **`FSSM` → `ShaderBinary` front end.**
   Parse the FSSM section table (`[0xc0] code_off`, `[0xc8] code_size`, `[0xe8] code_end`, 8-aligned;
   `docs/fssm-analysis.md`) into `code = words[code_off..code_end]`, keep the 16-byte sub-header,
   and capture the post-`code_end` **binding/metadata table** (the 20-byte records
   `[a][8][3][b+=8][1]`) as the shader-local binding manifest. Set `stage` from the name
   prefix (PS/MS/MSNC/MSR/CS/VS/GS/APS/AMS/…) — this replaces the AGC `header.type`.

2. **Build the console "shader header" replacement — a synthetic ABI record, not an AGC header.**
   the upstream shader recompiler ignores `ShaderBinary.header`, so define an `consoleShaderAbi` that fills the fields the AGC
   driver used to derive:
   - `GuestContext.waveSize` (32/64) — from the DXIL `SFI0`/`PSV0` chunks or inferred
     (`docs/shader-library.md` lists `Wave`-named shaders);
   - `GuestContext.userDataBaseRegister` + `GuestContext.userData` (the user-SGPR bank) — the
     **hardest item**, since D3D12's console user-data layout is not public;
   - `ShaderComputeStageInfo` / `ShaderPixelStageInfo` / `ShaderVertexStageInfo` /
     `GraphicsCompileContext.mesh` — synthesized from reflection + stage;
   - `GuestContext.memory` describing the SRT/descriptor-heap region.
   Leave `header`/`headerAddress` empty. **This is the direct answer to "what replaces the PS5
   shader header": a synthesized `consoleShaderAbi` derived from (a) the outer DXIL reflection chunks,
   (b) the FSSM binding table, and (c) validated assumptions about the D3D12 user-SGPR layout.**

3. **D3D12 → the upstream shader recompiler `RuntimeAbi` descriptor bridge.**
   Map D3D12 root-signature/descriptor-heap semantics onto the upstream shader recompiler's `Binding::{Buffers, Samplers,
   FlattenedSrt, ImageTable, ImageTableMap, SamplerTable, BdaPagetable}` model: turn D3D12
   CBV/SRV/UAV descriptors into V#/T# words and samplers into S# words, place them in the region
   `userData` points at, and set the image/sampler tables. Model descriptor tables as heap
   offset+base and root descriptors as 64-bit BDA (see Q2). Use **RGA** to confirm the V#/T#/S#
   encoding of a PC D3D12 descriptor is identical to what the console shader fetches.

4. **Mesh / amplification coverage (the dominant stage — 8,894 of 25,889 payloads).**
   Generalize `GraphicsCompileContext.mesh` / `MeshConfiguration` and the mesh draw-argument
   delivery away from PS5's push-constant layout (`MeshDrawPushOffsetBytes`, `MeshArgumentAddressDword`)
   to the D3D12 model: root signature + `DispatchMesh` payload passed through LDS and read via
   `GetMeshPayload()`. The ISA side (LDS payload, `s_barrier`/`s_sendmsg`) is already exercised by
   the upstream shader recompiler's mesh path; the *binding/argument* plumbing is what must change.

5. **Extend the gfx1013 decoder to full gfx10.3 / RDNA2 forms.**
   Scarlett is "full RDNA2"; the upstream shader recompiler targets gfx1013. Diff the local
   `tools/rdna2_isa_forms.txt` against gfx1013 forms, find opcodes present in RDNA2 but missing in
   the decoder, and add them. Decide the scope with a measured disassembly-gap report (item 8).

6. **Wire the DXIL reflection chunks into the interface contract.**
   `ISG1`/`OSG1`/`PSV0`/`SFI0` (already parseable — `scripts/parse_dxil_signatures.py` in the
   console-gdk-reversing skill) give input/output signatures, resource usage, and shader features.
   Use them to drive `ShaderVertexStageInfo.resourcesDst`, output mappings, and to cross-check the
   synthesized user-data/SRT layout. They do **not** give the register-level user-SGPR map — that
   still comes from item 2c.

7. **Wave-size / subgroup handling for the D3D12 model.**
   Mesh/CS/PS wave32 vs wave64 must be set consistently in `context.waveSize` and
   `target.subgroupSize`; note the upstream shader recompiler throws on `wave64 + sharedVgprBlocks != 0`.

8. **Differential disassembly harness (the cheapest decisive experiment).**
   Disassemble N sampled the target title blobs with **both** the upstream shader recompiler's `RdnaInstructionDecoder` and **AMD RGA**
   (or `llvm-objdump --triple=amdgcn`), and diff. Any opcode RGA decodes but the upstream shader recompiler cannot is a
   concrete decoder gap; a byte-for-byte agreement on a large sample is strong evidence the console ISA
   is within the shared gfx10 encoding. No title exe needed.

9. **Decide the output target.**
   SPIR-V (Vulkan) works today. If the target is D3D12-on-PC, emit DXIL via **DXC** instead (or go
   SPIR-V → HLSL via SPIRV-Cross → DXC). This changes the back end, not the front end.

10. **The two genuinely gated unknowns, stated once:**
    (a) the exact **console D3D12 user-SGPR base/count per stage** — public docs are NDA-gated, so it
    must be recovered from the FSSM binding table and validated at runtime; and (b) whether the
    **title exe's shader compiler** (rather than the FSSM binding table) is the authoritative source
    of the binding contract. Neither blocks items 1, 5, 8, 9, which need no leaked artifact.

---

### Sources (primary)

- LLVM AMDGPU backend — AMDPAL User Data, user-SGPR count: <https://llvm.org/docs/AMDGPUUsage.html>
  (rst: <https://raw.githubusercontent.com/llvm/llvm-project/release/18.x/llvm/docs/AMDGPUUsage.rst>)
- AMD *RDNA 2 ISA Reference Guide*: <https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf>
- Microsoft Learn — Root Signatures / Limits / Descriptor Heaps:
  <https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signatures>,
  <https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits>,
  <https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps>
- Microsoft *DirectX-Specs* Mesh Shader: <https://github.com/microsoft/DirectX-Specs/blob/master/d3d/MeshShader.md>
- console SDK / console SDK docs (public pages): <https://docs.console.com/tools/tools-console/shadercompiler>,
  <https://docs.console.com/build/gdk-and-engines/guides/advanced-shader-delivery>,
  <https://docs.console.com/build/gdk-and-engines/guides/directx-12-programming-guide>
  (shader-compiler sub-pages are NDA-gated: <https://learn.microsoft.com/en-us/gaming/gdk/docs/tools/tools-console/shadercompiler/dxc-console-defines?view=gdk-2610>)
- Maister — DXIL→SPIR-V (root table/root descriptor delivery):
  <https://themaister.net/blog/2022/04/11/my-personal-hell-of-translating-dxil-to-spir-v-part-4/>,
  <https://themaister.net/blog/page/2/>
- TechPowerUp GPU DB — Scarlett ("Scarlett"), Series S ("Lockhart");
  the upstream shader recompiler progress (PS5 = gfx1013): <https://boykopovar.github.io/the upstream shader recompiler/progress.html>
- Local the upstream shader recompiler sources: `core/libs/SceShaders.hpp`, `core/libs/prx/libSceAgc/Shader/src/CreateShader.cpp`,
  `core/libs/prx/libSceAgcDriver/Execution/src/Driver/Shaders/ShaderRegistry.cpp`,
  `core/shader/recompiler/{Recompiler.hpp,RuntimeAbi.hpp,Recompiler.cpp,RdnaDecoder/…}`.
- Local the target title docs: `docs/fssm-analysis.md`, `docs/shader-library.md`,
  `docs/the recompiler integration notes`, `docs/shader-container-prior-art.md`.
