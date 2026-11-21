# DLL surface spec — translation-layer target list

Scope: the 16 DLLs in chunk 1002. This is the complete set the layout declares, and all 16 are present.
Purpose: define exactly what a translation layer must satisfy, and what can be replaced outright.

Method: PE import/export table parse (local tooling), plus REA/Ghidra decompilation where noted.
Image base for all package DLLs: `0x180000000`.

## 1. console-specific surface — what must be translated

Only four items in the package are console-specific. The whole runtime shim surface is **7 functions
and 1 unresolved ordinal**:

| module | imports required | count | translation |
|---|---|---|---|
| `xmem.dll` | `XMemAlloc`, `XMemFree`, `XMemGetWorkingSetStatistics`, `XMemVirtualQuery` | 4 | shim over `VirtualAlloc`/`HeapAlloc` + `GetProcessMemoryInfo` |
| `d3d12_xs.dll` | `D3D12CreateRootSignatureDeserializer`, `D3D12SerializeVersionedRootSignature` | 2 | alias/forward to PC `d3d12.dll` (same export names) |
| `XAudio2_9.dll` | `ord#1` (unnamed ordinal, from `Bink2Scarlett.dll`) | 1 | **unresolved** — needs XAudio2.9 export table |
| `consoleBvhBuilder.dll` | — (this *is* an console component) | — | **drop on PC**; use native D3D12 DXR on `ID3D12GraphicsCommandList4` |

Consumers of the console-only modules:
- `xmem.dll` ← `consoleBvhBuilder.dll`, `Bink2Scarlett.dll`, `amd_fidelityfx_upscaler_console.dll`
- `d3d12_xs.dll` ← `consoleBvhBuilder.dll`, `amd_fidelityfx_upscaler_console.dll`
- `KERNEL32.dll` ← exactly one file in the set (not all of them)

## 2. Everything else is standard Windows or replaceable

| module | what it is | action |
|---|---|---|
| `msvcp140*`, `vcruntime140*`, `concrt140`, `vccorlib140`, `vcomp140` | VC++ redistributables | none — identical on PC |
| `libHttpClient.console SDK.dll` | MS libHttpClient (console SDK flavour) | replace with libHttpClient's Windows build |
| `XCurl.dll` | Microsoft's curl fork | replace with libcurl (exports are `curl_*`) |
| `Bink2Scarlett.dll` | RAD Bink 2 video for Scarlett | replace with Bink 2 Windows build (licensed) |
| `amd_fidelityfx_loader/upscaler_console.dll` | FidelityFX loader + FSR4-int8 | replace with desktop FidelityFX SDK (see §4) |
| `amd_fidelityfx_*` (framegen) | referenced by the loader, **not shipped in this build** | optional; absence is handled |

## 3. consoleBvhBuilder.dll — XDXR (decompiled, REA)

- Exports the C++ class `consoleBvhBuilder` (12 exports), including
  `BuildRaytracingAccelerationStructure`, `...Batched2X`, `RehydrateRaytracingAccelerationStructure*`,
  `CopyRaytracingAccelerationStructure`, `Set64BitMortonCodes`, `GetRaytracingAccelerationStructurePrebuildInfo`.
- Depends on XDXR-only types: `D3D12CONS_XDXR_STREAM_DESC`,
  `D3D12CONS_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2`.
- Written against standard PC interface revisions: `ID3D12Device11`, `ID3D12GraphicsCommandList4/6/8`.
- Pass/shader inventory recovered from strings: MortonCodesCalculator, RadixSort* (PrefixSum64→4096),
  BitonicSort, BuildKDOP, BuildApexPointMap, BVH4Builder, Construct/Refit, TreeletReorder, WritePartitions,
  Load{Instances,Primitives,Triangles,ProceduralGeometry}, and the rehydration/decompression set.
- Limit string: "This version of XDXR limits the maximum BVH size to 2GB."
- `Set64BitMortonCodes(bool)` decompiles to a single store (`DAT_1800d7b4c = param_2`); that global is
  read by the prebuild-info sizing routine — the setter is live, not vestigial.
- Error path is a C++ `_CxxThrowException` with HRESULT `0x80070057`, preceded by `OutputDebugStringW`
  ("D3D12 XDXR Error: …"). Not a raw `int3` trap.

## 4. FidelityFX loader — runtime binding (decompiled, REA)

`ffxCreateContext` dispatches on effect-type bits 16–23 of the first dword of the create params
(`*param_2 & 0xff0000`): `0x10000` → upscaler, `0x20000` → framegeneration. `FUN_180001000` resolves
the provider: `GetModuleHandleA(name)` → `LoadLibraryA(name)` → `GetProcAddress` for
`ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`. Provider module
names are the DLL filenames themselves.

⇒ **There is no static link to the FFX modules; substitution by filename is a working seam.**
The upscaler is FSR4-int8 with AMD-cooked PSOs (`ACO ('ACCC')`, `ffxProvider_FSR4_Int8`,
`Fsr4Int8UpscalerModel`) built from `amdinternal` — so the seam is proven but whether a public PC FSR4
build exposes an equivalent int8 model is **unverified**.

## 5. PC-side reference artifacts acquired (for comparison)

- `vendor/d3d12/x64/D3D12Core.dll` — D3D12 Agility SDK **1.620.2**, 4,874,024 bytes,
  sha256 `70a6d47d335b4a15180544c01a35b84474522a369dc582d85fe057313a3aeaa0`.
- `vendor/d3d12/x64/D3D12Core.pdb` — 8,163,328 bytes (**symbolicated PC side**).
- `vendor/d3d12/include/` — `d3d12.h`, `d3d12.idl`, `d3d12shader.h`, `d3dcommon.h`, `d3dx12_core.h`.

Note: `D3D12Core.dll` exports only **2** names — `D3D12GetInterface` (rva `0x14fa0`) and `D3D12SDKVersion`.
The familiar entry points (`D3D12CreateDevice`, `D3D12SerializeVersionedRootSignature`, …) live in the OS
`d3d12.dll`, which the NuGet redist does **not** ship. console has no such split: `d3d12_xs.dll` is both
loader and core. ⇒ The correct PC counterpart for `d3d12_xs.dll` is the **pair** (`d3d12.dll` + `D3D12Core.dll`),
and comparison must be interface/vtable-oriented, not export-oriented.

## 6. Open items

- `XAudio2_9.dll` `ord#1` — unresolved; needs that DLL's export table.
- console-side `d3d12_xs.dll` — **not obtainable from current artifacts** (see `package-spec.md` §4).
- Whether the public console SDK bundles Scarlett headers: the public console SDK zip ships an installer bundle whose
  `SetupScripts/` include `ExtractconsoleOneDKs.ps1` / `InstallconsoleOneDKs.ps1`, i.e. console DK payload is
  present in hashed `.cab` files. **Unverified** whether Scarlett (not just console One) headers are included,
  and whether extracting them is licensed. Worth checking before any manual struct RE.
