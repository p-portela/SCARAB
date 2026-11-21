# console D3D12 → Windows D3D12 bridge (`d3d12_xs.dll`)

Status: **bridge builds and its export contract is verified byte-exact.** Nothing
here has been executed on Windows yet — see "What is proven vs assumed".

## 1. The answer to "can it be done"

Yes, and the deciding measurement came out in favour of the cheap design.

The console runtime and the PC runtime define **the same COM interfaces**. I
extracted the 16-byte GUID behind every `IID_*` data export of the genuine
console `d3d12_xs.dll` and compared them with the `DEFINE_GUID` values in the
PC D3D12 headers we hold:

| measurement | result |
|---|---|
| IIDs defined on both sides | 28 |
| hex-identical | **28** |
| different | **0** |

`IID_ID3D12Device` is `189819f1-1db6-4b57-be54-1821339b85f7` on both. Same for
`ID3D12CommandQueue`, `ID3D12DescriptorHeap`, `ID3D12Device1`–`4`,
`ID3D12CommandAllocator`, `ID3D12PipelineState`, `ID3D12RootSignature`,
`ID3D12CommandSignature`, `ID3D12DeviceChild`, `ID3D12CommandList`, `ID3D10Blob`.

Identical IID ⇒ identical interface ⇒ identical vtable layout ⇒ **the standard
D3D12 surface can be forwarded, not reimplemented.** That is the whole reason
this bridge is a few hundred lines of C++ instead of a DXVK-scale project.

## 2. Architecture

```
title (Scarlett build, game_scarlett_master.exe)
  |
  +-- d3d12_xs.dll ................ this bridge (drop-in replacement)
  |     |
  |     +-- standard D3D12 exports ---> C:\Windows\System32\d3d12.dll (PC runtime)
  |     |                                   |
  |     |                                   +--> PC vendor UMD --> PC kernel driver --> real GPU
  |     |
  |     +-- console-only exports -----> emulated locally (see §4)
  |     |
  |     +-- IID_* / DXGI_DEBUG_* -----> data exports, byte-identical GUID table
  |
  X   the genuine console stack is NOT used:
      d3d12_xs.dll -> xgs12_pc_xs.dll -> DeviceIoControl -> *console* GPU kernel driver
```

Why the genuine pair is not reused: see §3. The console UMD's bottom seam is
`DeviceIoControl` to the **console** GPU kernel-mode driver, which does not exist
on a PC. Everything above that seam is portable; the seam itself is not.

## 3. The console stack, now fully identified

a retail PC game PC build ships the console graphics stack redist in `[redacted]/`, and
it turns out to contain **the entire console graphics stack**. This was not
obvious from file names — the identification comes from the binaries' own
strings and export tables.

| file | size | what it actually is | evidence |
|---|---|---|---|
| `d3d12_xs.dll` | 51,576 | console D3D12 **runtime** front-end | product string `console D3D12.CONSOLE Runtime Library for Offline PC`; exports 116, of which 93 are IID data |
| `xgs12_pc_xs.dll` | 3,852,664 | **the console UMD** — "console User Mode Driver for Direct3D 12.XS / [VALIDATED] Offline Scarlett-%s" | own string; exports `OpenAdapter12`, `UMDCreateDevice12`, `UMDCreateBlob`, `UMDCreateRootSignatureDeserializer`, `UMDCreateVersionedRootSignatureDeserializer`, `UMDGetDebugInterface`, `UMDSerialize{Versioned,}RootSignature`, `UMDPresentArray`, `UMDInternalDriverCallX`, `UMDSetNullGpuMode`, `D3D11on12_*` |
| `xg_xs.dll` | 547,192 | console Graphics library: tiling/swizzle/DCC/BVH | exports `XGComputeOptimalSwizzleMode`, `XGEncodeDCC`, `XGDecodeDCC`, `XGCreateBVHComputer`, `XGSetHardwareVersion`, … |
| `xbsc_xs.dll` | 16,802,360 | **console shader compiler** (compile *and* disassemble) | exports `SCCompileMultiShadersconsole`, `XBSC_AssembleShader`, `XBSC_DisassembleRawShader`, `XBSC_DisassembleHwPipeline`, `XBSC_CompileRootSignature`, `XBSC_SerializePipelineCompileOutput`, …; embeds the FSSM/XBCH/XDAT magics |
| `dxcompiler_xs.dll` | 18,596,392 | DXC + **XDXR** (raytracing) compiler | exports `CreateXdxrCompiler`, `CreateXdxrCompiler2`, `DxcCreateInstance[2]` |
| `dxil.dll` | 1,503,304 | DXIL validator | exports `DxcCreateInstance`, `DxcCreateInstance2` |
| `xdxrofflinebvh.dll` | 29,709,344 | offline XDXR BVH builder | 119 exports; matches `consoleBvhBuilder.dll` in the game package and the `pack/*_bvh4` files |
| `D3D12_0/D3D12Core.dll` | 5,025,592 | PC D3D12 core (Agility SDK), **newer build than ours** (4,874,024) | same export set `D3D12GetInterface`, `D3D12SDKVersion` |
| `D3D12_0/d3d12SDKLayers.dll` | 4,965,688 | D3D12 debug/validation layer | — |
| `D3D12_0/D3D12StateObjectCompiler.{dll,exe}` | 2,749,792 / 2,481,504 | state-object / DXR compiler host | — |
| `xaudio2_9redist.dll` | 997,400 | XAudio2.9 redist | closes an open item — see §6 |

Proof that `d3d12_xs.dll` and `xgs12_pc_xs.dll` are a matched pair: the runtime's
own string table names exactly the UMD entry points it looks up —
`UMDCreateBlob`, `UMDCreateDevice12`, `UMDCreateRootSignatureDeserializer`,
`UMDCreateVersionedRootSignatureDeserializer`, `UMDInternalDriverCallX`,
`UMDPresentArray`, `UMDSerializeRootSignature`,
`UMDSerializeVersionedRootSignature` — and those are precisely `xgs12_pc_xs.dll`'s
exports. This also finally answers "what is the UMD": the vendor's own string
calls it *console User Mode Driver for Direct3D 12.XS*.

Note the IID table tells us the console D3D12 extension surface is **free
functions plus command-list interfaces**, not a new device interface: there is no
`IID_ID3D12DeviceX`. Console-only interfaces present are
`IID_ID3D12ConsDmaCommandList`, `IID_ID3D11DeviceX`, `IID_ID3D11DeviceContextX`,
`IID_ID3D11DmaEngineContextX`, `IID_ID3D11PerformanceDeviceX`,
`IID_ID3D11PerformanceContextX`, `IID_ID3D11UserDefinedAnnotationX`,
`IID_ID3D11CounterSetX`, `IID_ID3D11CounterSampleX`, `IID_IGraphicsUnknown`.

## 4. Export disposition — all 116

Generated table: `bridge/exports_console.csv` (ordinal, name, kind).
Mechanical contract: `bridge/d3d12_xs.def`.

| ordinals | group | disposition |
|---|---|---|
| 5,6,7,8,9,10,11 | `D3D12CreateDevice`, `D3D12Create{Versioned,}RootSignatureDeserializer`, `D3D12EnableExperimentalFeatures`, `D3D12GetDebugInterface`, `D3D12Serialize{,Versioned}RootSignature` | **forward** to PC `d3d12.dll` by name |
| 3 | `D3D10CreateBlob` | **implemented locally** (own `ID3D10Blob`: `{8ba5fb08-…}`) — no D3D10 dependency |
| 1, 18 | `D3D12ZeroMemoryX`, `D3DFlushEntireCpuCache` | trivial: `memset`, `mfence/sfence/lfence` |
| 14,15,16,17 | `D3D12Cons{EnableMemoryProfiling,Get,Set}ProcessDebugFlags`, `D3D12ConsUnloadDriverModule` | **recorded**, no PC equivalent |
| 13 | `D3D12ConsCreateDevice` | forward as a PC device; console creation parameters logged, not honoured |
| 4 | `D3D12ConfigureVirtualMemoryX` | recorded; PC driver owns VM policy |
| **12** | **`D3D12UploadCustomMicrocodeX`** | **the shader seam** — fails loudly (`E_NOTIMPL`) until FSSM→SPIR-V/DXIL exists |
| 19,20,21,22 | `DXGIX{GetFrameStatistics,PresentArray,SetFrameNotification,SetVLineNotification}` | emulated; PC has no present-array / vblank-notification model |
| 2 | `PixGetPixieEntryPoints` | `E_NOTIMPL` |
| 116 | `InternalDriverCallX` | `E_NOTIMPL`, counted. Private runtime↔UMD channel; nothing to forward to |
| 23–27 | `DXGI_DEBUG_*` | **data** — GUIDs |
| 28–115 | 88 × `IID_*` | **data** — GUIDs, byte-identical to the genuine runtime |
| 114/115 boundary | `IID_IDXGISwapChain1`, `IID_IGraphicsUnknown` | last data slots before `InternalDriverCallX` @116 |

**ABI note on unknown-arity exports.** Several console-only exports have
signatures that are not publicly documented (`DXGIXPresentArray`,
`InternalDriverCallX`, `D3D12ConfigureVirtualMemoryX`, …). On x64 (Microsoft
ABI) a callee that ignores its arguments is safe to call whatever the true arity
is: arguments arrive in `RCX/RDX/R8/R9` + stack and are simply never read. Those
exports are therefore implemented as zero-argument stubs that log. That is
deliberate — it removes the risk that a guessed signature corrupts the stack, and
it is strictly better than not exporting the symbol at all (which would fail
`GetProcAddress` at load).

## 5. Verification — what is proven vs assumed

Proven, by tool output:

```
$ python3 tools/verify_exports.py bridge/build/d3d12_xs.dll
built     : bridge/build/d3d12_xs.dll  (116 exports, base 1)
reference : vendor/x64_dx12/d3d12_xs.dll  (116 exports, base 1)
[1] names      : 116/116 present
[2] ordinals   : 116/116 match
[3] count      : built=116 reference=116
[4] GUID data  : 93/93 byte-identical
[5] forwarders : 0
RESULT: PASS - export contract is byte-exact (116 exports, 93 GUIDs)
```

Also proven: the 28/28 IID identity measurement (§1); the console stack
identification (§3); `d3d12_xs.dll`'s UMD entry-point string list matching
`xgs12_pc_xs.dll`'s exports.

**Assumed, not proven** — be explicit about this:

- That forwarding works against the *real* PC runtime. The bridge has been
  executed (see §5.1) and it did resolve and call into `d3d12.dll` successfully —
  but that was Wine's `d3d12.dll`, not Windows'. Wine has no GPU here, so no
  device was actually created. The standard-path reasoning is sound (identical
  IIDs) and the call path is exercised; real-hardware behaviour is not.
- The true signatures of the unknown-arity exports. The stubs are *safe* (see the
  ABI note) but they may not be *correct*; correcting them needs the title exe.
- Memory-model behaviour. The console has unified memory and no VRAM residency
  concept; PC D3D12 has heaps and residency. A title that assumes allocator
  behaviour the PC runtime does not provide will misbehave in ways this bridge
  cannot paper over.

## 5.1 Executed: the bridge and the console stack under Wine

Portable Wine (`~/opt/wine`, Kron4ek 11.22-staging-amd64) runs on this box, so
the artefacts were loaded for real. Harness: `tools/peload.c` → `tools/peload.exe`
(built with LLVM-mingw; it enumerates the export directory *from inside
Windows-land* via `GetProcAddress`, so it independently cross-checks
`tools/peinfo.py`).

```
### bridge/build/d3d12_xs.dll        LOADLIBRARY_OK   EXPORTS 116   RESOLVED 116/116
### vendor/.../d3d12_xs.dll (genuine) LOADLIBRARY_OK   EXPORTS 116   RESOLVED 116/116
### vendor/.../xgs12_pc_xs.dll       LOADLIBRARY_OK   EXPORTS  21   RESOLVED  21/21
### vendor/.../xbsc_xs.dll           LOADLIBRARY_OK   EXPORTS  43   RESOLVED  43/43
### vendor/.../dxcompiler_xs.dll     LOADLIBRARY_OK   EXPORTS   4   RESOLVED   4/4
### vendor/.../xg_xs.dll             LOADLIBRARY_OK   EXPORTS  27   RESOLVED  27/27
### vendor/.../xdxrofflinebvh.dll    LOADLIBRARY_OK   EXPORTS 119   RESOLVED 119/119
### vendor/.../D3D12_0/D3D12Core.dll LOADLIBRARY_OK   EXPORTS   2   RESOLVED   2/2
### vendor/.../D3D12_0/d3d12SDKLayers.dll  LOADLIBRARY_FAILED err=126
```

Our bridge's `DllMain` ran and its forward path executed — its own log:

```
=== d3d12_xs.dll bridge attached (pid=32) ===
FWD: PC d3d12.dll loaded, all entry points resolved
=== d3d12_xs.dll bridge detached ===
```

The GUID data exports were read out of the loaded image and match the genuine
runtime byte-for-byte:

```
IID_ID3D12Device       f1199818 b61d 574b be541821339b85f7   -> 189819f1-1db6-4b57-be54-1821339b85f7
IID_IGraphicsUnknown   63eaeeac a9e0 1c4a bbec71b2f485f758
```

That last one matters: `IID_IGraphicsUnknown` is one of the console-only
interfaces, so the console IID table is being reproduced exactly, not
approximated.

**The wider result: the entire console graphics toolchain runs on Linux.**
`xbsc_xs.dll` (the console shader compiler *and disassembler*) resolves all 43
exports under Wine, including `XBSC_DisassembleRawShader`,
`XBSC_AssembleShader` and `SCCompileMultiShadersconsole`. The 25,321 extracted
RDNA2 streams therefore do not have to be decoded by inference — the vendor's own
disassembler is available locally. Calling it needs the export signatures, which
are not public; the next step is recovering them (Ghidra on the 43 stubs) and
then running the disassembler over the corpus.

Wine setup notes, so this is reproducible: the prefix must be 64-bit only
(`WINEARCH=win64`) and this box has no 32-bit glibc, so `lib/wine/i386-{unix,windows}`
were moved aside to stop Wine reaching for `/lib/ld-linux.so.2`. A sibling
dependency such as `XBSC_XS.dll` must be resolvable by name — `xgs12_pc_xs.dll`
imports it in upper case from a case-sensitive filesystem, hence the
`XBSC_XS.dll` copy alongside it.

## 6. Open items closed and opened by this work

Closed:

- **What is the UMD** — the console GPU user-mode driver; we now hold it
  (`xgs12_pc_xs.dll`, self-identified).
- **`XAudio2_9.dll` ordinal #1** imported by `Bink2Scarlett.dll` is
  **`XAudio2Create`** — `xaudio2_9redist.dll` exports it at ordinal 1.
- Orphaned internet references to `xg_xs.dll` (e.g. the claim that it alone
  carries a `.xbld` section) are now checkable locally: `xg_xs.dll` and
  `xgs12_pc_xs.dll` both have `.xbld`, the console runtime `d3d12_xs.dll` does
  not. `.xbld` marks the console SDK CI pipeline, and its absence in the runtime is
  consistent with that build being serviced from the OS branch.

Opened:

- `xgs12_pc_xs.dll` embeds the **XDXR internal shader paths**
  (`D:\os\obj\amd64fre\console\drivers\gpu\raytracing\xdxr\shaders\csh\objfre\amd64\*`)
  — `Box16Rehydration`, `Box32Rehydration`, `CopyAndCompactBVH`,
  `DeserializeBVH`, `FixupPointers`, `LeafRehydration`, `DecompressHeader`,
  `GetBVHCompactedSize`, `PrefixSum{64..4096}`, `RadixAlloc`, `RadixCount`,
  `ExecuteIndirectConvertArgs`. Those match the 1,017 raytracing shaders measured
  in `shaders.rpf`. The `.shpdb` extension is the console's shader-program
  database.
- Console command lists are console-specific types: `ID3D12ConsComputeCommandList`
  (and variants 6 and 8), `ID3D12ConsCopyCommandList6`, `ID3D12ConsDmaCommandList`
  (with `FillRawBuffer`), plus
  `D3D12ConsRehydrateRaytracingAccelerationStructure`. A title that QIs for these
  against a forwarded PC device gets `E_NOINTERFACE`. This is the main structural
  gap and the next real piece of work: a wrapper layer that owns those
  interfaces and forwards their contents to a PC command list.

## 7. Build

No Windows SDK, no root, Linux host:

```
~/opt/llvm-mingw           # LLVM-mingw 20261117, Linux-hosted, clang 23.1.6
cd $SCARAB_ARTIFACTS/bridge && bash build.sh
python3 ../tools/verify_exports.py build/d3d12_xs.dll
```

The bridge is header-free by design: it declares ABI-exact prototypes and
resolves the PC runtime through `GetProcAddress`, so it needs no D3D12 headers
and cannot drift from the PC ABI at compile time.

## 8. Next steps, in order

1. ~~Wrapper layer for the console command-list / DMA interfaces~~ — **done**, see
   `docs/d3d12-xs-wrapper.md`. `wrap/d3d12_xs_wrap.dll` owns the extension surface
   and is substituted into the device at `D3D12CreateDevice`. Verified end-to-end
   under Wine: a console DMA command list round-trips and serves both
   `IID_ID3D12ConsDmaCommandList` and `IID_ID3D12GraphicsCommandList`.
2. Place the remaining ~95 recovered console methods and confirm the appended
   block's real order — needs the QI/slot log from a running title.
3. Point `D3D12UploadCustomMicrocodeX` at the real FSSM→RDNA2→SPIR-V chain once
   the the upstream shader recompiler decoder sweep is clean.
4. Recover the true signatures of the unknown-arity exports from the title exe —
   blocked on the exe, which is absent from the rip by design.
5. Execute the bridge on a Windows host with a title that exercises it.
