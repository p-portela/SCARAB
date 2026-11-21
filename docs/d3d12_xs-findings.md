# d3d12_xs.dll — the console D3D12 runtime, acquired

File: `$SCARAB_ARTIFACTS/d3d12_xs.dll`
SHA-256: `4792972d0647249f1706377d2d228523e9a37b6e3102b4a3500e1231f2e4b09b`
Size: 51,576 bytes. PE32+, x86-64, 6 sections. **Authenticode signed** (cert table 10,616 bytes).
PDB name embedded: `d3d12_xs.pdb`. No `.xbld` section.

## This closes blocked item 3

Item 3 was: *"`d3d12_xs.dll` is not obtainable — it lives in the SystemImage volume inside
`the system image (encrypted)`, which is encrypted."* It is no longer blocked: the console-vs-PC D3D12 binary
comparison can now run.

## Version resource — the most informative single fact

| field | value |
|-------|-------|
| FileVersion | `10.0.26100.6247 (WinBuild.160101.0800)` |
| ProductVersion | `10.0.26100.6247` |
| CompanyName | console |
| Product description | **`console D3D12.CONSOLE Runtime Library for Offline PC`** |

"**for Offline PC**" matters. This is the PC-runnable flavor of the console D3D12 runtime,
not a console-only build fingerprinted to one console. It also carries the string
`D3D12 ERROR: SSE4.2 CPU instruction set support is required to use D3D12.X for PC.`

The version is on the `10.0.26100.x` line — the same line as the PC `d3d12.dll` we already
hold (`10.0.26100.8328`), so both sides of the comparison sit in one servicing branch.

## It is a front-end over a user-mode driver, not the implementation

Its own error strings state the architecture:

- `D3D12 ERROR: Could not load user mode driver (%s)`
- `D3D12 ERROR: Failed to find required entry points in user mode driver (%s)`
- `D3D12 ERROR: Cannot load multiple user mode driver flavors at the same time.`
- `Make sure you release all devices and call D3D12ConsUnloadDriverModule prior to loading a new flavor.`

That is why 116 exports fit in 51 KB: this DLL loads a **UMD** at runtime. The heavy
implementation lives in the user-mode driver, not here. Its only non-CRT import is
`KERNEL32.dll` — including `DeviceIoControl`, `LoadLibraryExW`, `GetProcAddress`,
`OutputDebugStringW/A`, `IsDebuggerPresent`, and the `Rtl*` unwinding set.

Consequence: this DLL is not a drop-in for PC. What it *is* is the authoritative
specification of the surface a translation shim must implement.

## Export surface (116 named exports, base 1, ordinals 1–116)

Composition: ~94 `IID_*` GUID data exports, 7 standard D3D12 entry points shared with PC,
and 16 console-specific ones.

Shared with PC `d3d12.dll` (7): `D3D12CreateDevice`, `D3D12CreateRootSignatureDeserializer`,
`D3D12CreateVersionedRootSignatureDeserializer`, `D3D12EnableExperimentalFeatures`,
`D3D12GetDebugInterface`, `D3D12SerializeRootSignature`, `D3D12SerializeVersionedRootSignature`.

Console-only (16): `D3D10CreateBlob`, `D3D12ConfigureVirtualMemoryX`,
`D3D12UploadCustomMicrocodeX`, `D3D12ConsCreateDevice`, `D3D12ConsEnableMemoryProfiling`,
`D3D12ConsGetProcessDebugFlags`, `D3D12ConsSetProcessDebugFlags`,
`D3D12ConsUnloadDriverModule`, `D3D12ZeroMemoryX`, `D3DFlushEntireCpuCache`,
`DXGIXGetFrameStatistics`, `DXGIXPresentArray`, `DXGIXSetFrameNotification`,
`DXGIXSetVLineNotification`, `InternalDriverCallX`, `PixGetPixieEntryPoints`
(plus `UMDInternalDriverCallX` present as a string).

The `IID_*` set spans D3D11 (`ID3D11DeviceContextX`, `ID3D11DmaEngineContextX`,
`ID3D11PerformanceDeviceX`), D3D12 (`ID3D12ConsDmaCommandList`, through `ID3D12Device4`)
and DXGI. Exporting the interface GUIDs as data is a console trait; PC takes them from
the SDK headers. A translation layer needs that GUID table.

## Earlier inference now confirmed on real data

Prior claim, made from the import tables alone: *"console has no loader/core split."*
Confirmed. The console DLL has **no `D3D12GetInterface`** and no
`D3D12CoreCreateLayeredDevice` / `D3D12CoreRegisterLayers`. PC splits as
`d3d12.dll` (18 exports, with a `fothk` forwarder section) → `D3D12Core.dll`
(2 exports: `D3D12GetInterface`, `D3D12SDKVersion`). Console is one module that loads a UMD.

Also now measured rather than assumed: the two functions the shipped package DLLs import
from `d3d12_xs.dll` exist with identical names —
`consoleBvhBuilder.dll` → `D3D12SerializeVersionedRootSignature`,
`amd_fidelityfx_upscaler_console.dll` → `D3D12CreateRootSignatureDeserializer`.
Only 2 of the 116 exports are exercised by the shipped DLLs; the title exe will use more.

## What this changes for the plan

- **Item 3 (console-vs-PC D3D12 comparison): unblocked.** Export surface, GUID table and the
  UMD loading model are all readable from this binary.
- **The shim target is now bounded and enumerated**: 116 exports, of which 16 are
  console-specific (`D3D12Cons*`, `DXGIX*`, `InternalDriverCallX`, `D3D12*X`) and ~94 are
  GUID data. That replaces guesswork with a checklist.
- **New lead**: because this flavor is explicitly "for Offline PC", a PC-targeted copy of
  the same runtime plausibly ships in the *Windows* console SDK redistributable. Earlier we checked
  only the File tables of the public console SDK's small MSIs for `scarlett`/`d3d12_xs` strings and
  found nothing, but that search was narrowed to console tokens. Worth re-running against
  the exact filename `d3d12_xs` and the string `D3D12.X for PC`.
- **Still missing for boot**: `game_scarlett_master.exe` (absent from the rip) and the
  system images system DLLs (encrypted `the system image (encrypted)`). Unchanged by this.
