# SCARAB roadmap

> Living document. Every phase has an exit gate that is a command, not an opinion.

Version numbers are **milestones, not checkpoints** — see [MILESTONES.md](MILESTONES.md)
for the ladder and each version's exit criteria. No tag is placed for work that is not a
finished milestone.

## 0. The bytecode decision (settled, with evidence)

**Target: DXIL — LLVM bitcode, shader model 6.x — carried in a `DXBC` container.**
Produced by DXC from HLSL that we emit. Not SPIR-V, not DXBC bytecode, not
hand-written LLVM bitcode.

### Why, measured rather than assumed

A console shader and a DXC-produced PC shader are **the same container**:

```
console:  DXBC header, parts SFI0 ISG1 OSG1 PSV0 HASH DXIL PRIV
          DXIL part payload +8  -> nested tag "DXIL"
          DXIL part payload +24 -> "XDAT"  -> XBCH (zlib) -> FSSM microcode
DXC:      DXBC header, parts SFI0 ISG1 OSG1 PSV0 STAT ILDN HASH DXIL
          DXIL part payload +8  -> nested tag "DXIL"
          DXIL part payload +24 -> "BC C0 DE" -> LLVM bitcode
```

Same outer fourcc, same part names, same nested tag, **same offset** for the
payload. The console container is the standard DXBC container with the bitcode
replaced by microcode. Three consequences:

1. The container format is not a research problem. It is the published DXBC/DXIL
   container.
2. `ISG1`/`OSG1` (input/output signatures) and `PSV0` (pipeline state validation)
   in the console shaders are in the **standard** format — we already parse them
   (`docs/shader-library.md`, 25,889 rows, 0 parse failures). That gives us the
   expected interface for every shader, for free.
3. **This yields a GPU-free oracle.** Emit HLSL → compile with DXC → compare the
   produced `ISG1`/`OSG1` against the console shader's. If they match, the shader's
   interface is provably right without ever running the title. See §1 Phase 2.

### Why not SPIR-V

SPIR-V is the IR of a *different API* (Vulkan), consumed by Vulkan drivers — it sits
one layer **below** the boundary SCARAB works at. To use it we would have to
implement the entire D3D12 API on Vulkan ourselves, i.e. rebuild vkd3d-proton, and
throw away the API forwarding that currently works for free because the COM
interfaces are identical. Meanwhile DXIL is what *both* backends consume:

| backend | consumes | then |
|---|---|---|
| Microsoft D3D12 (Windows) | DXIL | vendor UMD compiles to hardware |
| vkd3d-proton (Linux) | DXIL | dxil-spirv compiles to SPIR-V → Vulkan |

SPIR-V stays useful as a **Linux-only cross-check** (the upstream shader recompiler already emits it), not
as the target.

### Why HLSL and not an LLVM bitcode backend

DXIL is LLVM 3.7 bitcode with `dx.*` intrinsics. Writing that backend means
generating 3.7-format bitcode by hand. Emitting **HLSL** and letting DXC produce the
bitcode is dramatically less work, is readable when it goes wrong, and DXC is
already in the artifact stack.

**Proven, on this host, under Wine** (`components/shader-bridge/src/dxc_emit_test.cpp`):
loads `dxcompiler.dll`, compiles HLSL to a 2,908-byte blob, parses the container, and
finds the nested `DXIL` tag with the bitcode magic at payload+24. Gate G6.

### Version policy (to be enforced in the emitter, not assumed)

| shader class | minimum target | reason |
|---|---|---|
| VS/PS/GS/HS/DS/CS | `*_6_0` | lowest that every modern backend accepts |
| mesh / amplification (8,894 shaders) | `ms_6_5` / `as_6_5` | mesh shaders are SM 6.5+; they are mandatory here |
| ray tracing / ray queries (1,017) | `lib_6_3`+ / `*_6_5` with rayquery | DXR tier; XDXR intrinsics are an open risk (§4) |

Emit the **lowest** version that covers the features actually used: it maximises
backend acceptance and there is no upside to claiming a higher SM.

## 1. Phases

### Phase 0 — Foundation (DONE)

Bridge, extension wrapper, build on Windows + Linux, CTest gates, artifact
provenance. Exit gate: `ctest` green, bridge export contract byte-exact.

### Phase 1 — Prove the shader pipeline end to end on ONE shader (next)

The goal is not coverage, it is to close the loop once, on the simplest shader in
the corpus, so that everything after it is repetition.

1. **Pick the target**: `VS_Passthrough_*` — no resources, no samplers, minimal ISA.
2. **RDNA2 → IR**: run the upstream shader recompiler's decoder front-end on its stream. Blocked today by the
   sweep; first work item is the alignment hypothesis (`docs/isa/shader-isa-notes.md`
   §3–4), then the vendor disassembler as an oracle (`xbsc_xs.dll`, loads under Wine).
3. **IR → HLSL**: emit a trivial, correct, ugly shader. Correctness over beauty.
4. **HLSL → DXIL**: DXC (already proven).
5. **Verify three ways, none of which needs a GPU**:
   - DXC's validator accepts the bitcode (`dxil.dll`);
   - the produced `ISG1`/`OSG1` **match the console container's** for that shader;
   - the container parses with the same part layout as the console one.

**Exit gate:** for one shader, a bitcode blob exists whose interface signature is
identical to the console shader's, validated by DXC. Add it as gate G7.

### Phase 2 — The interface oracle, then breadth by feature class

Build the oracle as tooling first (`tools/sigdiff.py`): given a console container and
a produced container, diff `ISG1`/`OSG1`/`SFI0` element-by-element and report. Then
work the corpus in ascending difficulty, each class gated by the oracle:

| class | count (measured) | what it adds |
|---|---|---|
| passthrough / blit VS+PS | small | nothing |
| textured, no cform | large | samplers, SRT bindings, `PSV0` |
| constant buffers / cbuffers | large | binding tables, root-signature layout |
| compute | large | UAVs, threadgroup dims, barriers |
| tessellation (HS/DS) | medium | patch constants, domain semantics |
| **mesh + amplification** | 8,894 | SM 6.5, payload/indirect args (see `D3D12CONS_CREATE_DEVICE_PARAMETERS` in the catalogue) |
| **ray tracing / XDXR** | 1,017 | DXR, BVH, `xdxrofflinebvh.dll`; XDXR intrinsics risk §4 |

**Exit gate:** the oracle passes for every shader in a class before moving to the
next; report coverage as a number, not a claim.

### Phase 3 — Memory and presentation

- `memory-adapter`: `D3D12CONS_RESOURCE_FLAG_*` / heap flags / DCC / HTILE / tiling.
  The values of those flags are still unknown — leave them unnumbered until there is
  evidence (`memory_adapter.h` says so).
- `present-adapter`: `DXGIXPresentArray`, vblank/frame notifications onto PC swapchains.

### Phase 4 — Bring-up, when the title executable exists

Blocked externally: the exe is absent from the rip by design and `the system image (encrypted)` is
encrypted with the retail ODK. Nothing in phases 1–3 waits on it.

## 2. Cross-cutting workstreams

| workstream | rule |
|---|---|
| **Windows first** | Windows is the primary target; Linux/vkd3d is a cross-check. A discrepancy between the two backends is a signal about which layer is at fault. |
| **Gates, not claims** | every phase exit is a command. G1–G6 exist; G7+ get added as each phase lands. |
| **Grading** | every call is real / recorded / refused, and the grade is logged. No silent successes. |
| **Provenance** | no vendor binaries or game data in the repo, ever. |
| **Instruments before answers** | the QI audit and appended-slot log exist to convert unknowns into data from a live title. |
| **Wine-free on Linux where it is not structural** | Wine is currently used in three places on Linux: running the PE test executables (G3/G4/G6) and driving the *vendor's* `dxcompiler.dll`. The first is structural - a Windows PE cannot execute on Linux without a Windows loader, and the tests are specifically about Windows ABI behaviour. The second is **not**: Microsoft ships native Linux DXC (`dxc`, `libdxcompiler.so`, `libdxil.so`), so the shader path should use that and drop Wine entirely. The build itself needs no Wine at all. |

## 3. Where the risk actually is

| risk | severity | mitigation |
|---|---|---|
| XDXR ray-tracing shaders may use console-only intrinsics with no DXIL equivalent | **high** — could make 1,017 shaders untranslatable as-is | classify their ISA against standard DXR intrinsics early, in Phase 2, not last |
| RDNA2 decoder gaps (VOP3/VOP2/SMEM/FLAT…) | medium | alignment hypothesis first, then the vendor disassembler as ground truth |
| Hand-emitted HLSL is correct but slow | low (correctness first) | revisit only after coverage |
| Mesh-shader payload/indirect-arg ABI differs from PC | medium | `D3D12CONS_CREATE_DEVICE_PARAMETERS` sizes are recorded in the catalogue |
| Console `SFI0` flags may encode console-only features | medium | compare against DXC-produced `SFI0` per shader via the oracle |
| Patching a console container by hand | avoided | let DXC build the container; use the console one only as the interface oracle |

## 4. Open questions that could change the plan

1. **Do XDXR shaders use console-only intrinsics?** If yes, ray tracing needs a
   different strategy (translate to standard DXR, or compute-based emulation). Answer
   in Phase 2 before investing in the RT class.
2. **Does the title hand shaders to `D3D12UploadCustomMicrocodeX` at runtime, or only
   cooked PSOs?** Affects whether the shader path is a batch pre-pass or an
   interception hot path.
3. **Is the console's `PSV0` populated with usable binding info**, or does the title
   supply bindings purely via root signature? Partially answered: `PSV0` is 1,168
   bytes on a real shader, so it is populated. Confirm against `PSV0` from DXC.
4. **the upstream shader recompiler dependency**: submodule vs vendored prebuilt recompiler library.
5. **When the exe appears**, re-plan Phase 4 around it; until then, no work item may
   depend on it.

## 5. Immediate next three items

1. `tools/sigdiff.py` — the interface oracle. Highest value: it makes every later
   shader claim checkable without a GPU.
2. Close the decoder alignment question (`docs/isa/shader-isa-notes.md` §4), or
   route around it using `xbsc_xs.dll`'s disassembler as the oracle.
3. Emit HLSL for one passthrough shader and take it through DXC + sigdiff (Phase 1).

## 6. Removing Wine from the Linux path (scoped, not yet done)

Wine is in SCARAB for two different reasons, and only one of them is legitimate.

| use | structural? | plan |
|---|---|---|
| running G3/G4/G6, which are Windows PEs | **yes** | a PE cannot execute on Linux without a Windows loader, and these tests are *about* Windows ABI behaviour (COM vtables, PE loading). Keep Wine here, or run `ctest` on Windows - which is the primary target and needs no Wine at all. |
| G6 driving the vendor's `dxcompiler.dll` to emit DXIL | **no** | switch to native Linux DXC. See below. |
| the build | n/a | already Wine-free: LLVM-mingw cross-compiles, and Wine appears only as the test launcher variable. |
| the runtime (a title, on Linux) | **yes, and not ours** | the title is a Windows PE, so it runs under Wine/Proton by definition. Our layer plugs into that process; it neither adds nor removes the requirement. |

### Native DXC

Microsoft publishes Linux builds of DXC — `dxc`, `libdxcompiler.so`, `libdxil.so` — in
the `microsoft/DirectXShaderCompiler` releases, in the `Microsoft.Direct3D.DXC` NuGet
package, and as a distro package (`dxc`). Steps:

1. Fetch and unpack the Linux DXC release as a build-time tool (not vendored).
2. Add `SCARAB_DXC_ROOT` to the platform module; resolve `libdxcompiler.so`/
   `libdxil.so` from it.
3. Add a native Linux target for the emitter test that `dlopen`s `libdxcompiler.so`
   and calls the same `DxcCreateInstance` entry point. The interface is identical, so
   only the loader differs (`LoadLibraryA` vs `dlopen`, `__cdecl` vs default).
4. Make G6 prefer the native path and fall back to Wine **only** if `SCARAB_DXC_ROOT`
   is unset, so the gate stops needing Wine the moment DXC is present.

Worth doing before Phase 2 rather than after: it is small, it removes a moving part
from the loop that Phase 2 will run thousands of times, and it gives us `libdxil.so`
as a real validator instead of DXC's `-Vd` escape hatch.

Note what does **not** change: `xbsc_xs.dll`, the vendor's console shader compiler and
disassembler, is a Windows DLL with no Linux equivalent. Using it as a decoder oracle
will need Wine or a Windows host — but that is an *analysis* tool run occasionally, not
a dependency of the pipeline.
