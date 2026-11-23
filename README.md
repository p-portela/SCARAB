<h1 align="center">SCARAB</h1>

<h4 align="center"><b>S</b>carlett <b>C</b>ompatibility <b>A</b>nd <b>R</b>untime <b>A</b>PI <b>B</b>ridge</h4>

<p align="center">Bridges console (Scarlett) D3D12 to PC D3D12, so a console title's own x86-64 code runs on your PC.</p>

<p align="center">
  <a href="#status">Status</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#build">Build</a> ·
  <a href="#tests">Tests</a> ·
  <a href="#docs">Docs</a> ·
  <a href="#license">License</a>
</p>

Not an emulator. Scarlett is x86-64, so only the OS and GPU-driver surface needs shimming.
Console and PC D3D12 define *the same* COM interfaces (28/28 shared `IID_*` values
byte-identical), so the standard API is **forwarded, not reimplemented** — only the console
*extension* surface is built. **Windows** is the primary target; **Linux** works too, on
vkd3d-proton.

## Status

| component | role | state |
|---|---|---|
| `runtime-bridge` | `d3d12_xs.dll` — the console runtime's 116 exports | built; contract byte-exact; runs under Wine |
| `extension-wrapper` | `d3d12_xs_wrap.dll` — appended `X` methods, console command-list types/IIDs | built; self-test 19/19; integrated end-to-end |
| `shader-bridge` | FSSM → RDNA2 → DXIL; root-signature → bindings | extraction done (25,321 shaders); recompiler in progress |
| `memory-adapter` | console heap flags, DCC/HTILE, tiling → PC heaps | seam only |
| `present-adapter` | `DXGIXPresentArray`, vblank/frame events → PC DXGI | seam only |

Under Wine, a console-only command-list type and IID round-trip through bridge → wrapper →
PC D3D12, and the resulting object serves both the console IID and the standard interfaces.

> **It does not run a title yet.** The title executable and the encrypted system OS image
> are outside this project, so everything is built to advance without them — see
> [MILESTONES.md](docs/MILESTONES.md).

## How it works

Standard D3D12 is forwarded to a PC `d3d12.dll`; only console-only free functions are
emulated. Console `X` methods and command-list types live in the optional wrapper, so the
bridge stays a drop-in without it. Shaders go from console RDNA2 microcode to DXIL, with
bindings synthesised from the root signature.

The backend is a runtime choice, not a build choice — it is whichever `d3d12.dll` loads:

| backend | platform | selection |
|---|---|---|
| Microsoft D3D12 (system) | Windows | default |
| Microsoft Agility SDK | Windows | app-local `D3D12Core.dll` |
| vkd3d-proton (D3D12 → Vulkan) | Linux | automatic under Proton/Wine, or `SCARAB_D3D12_DLL` |

`SCARAB_BACKEND=auto|d3d12|vkd3d` declares the expectation and warns on a mismatch; the
resolved backend is logged on load. **DXVK is not the Linux answer** — it covers
D3D9/10/11; D3D12 on Vulkan is vkd3d-proton.

## Shader target: DXIL

A console shader and a DXC-produced PC shader are the same container: same `DXBC` fourcc,
same part layout, same nested `DXIL` tag. Where the PC shader holds LLVM bitcode, the
console holds zlib-compressed FSSM RDNA2 microcode. So the target is **DXIL in a DXBC
container**, consumed by both Microsoft's D3D12 runtime and vkd3d-proton
([rationale](docs/ROADMAP.md)). The vendor's `XBSC_DisassembleRawShader` loads under Wine
(43/43 exports) and serves as the decoder oracle ([notes](docs/shader-library.md)).

## Build

Windows (primary):

```powershell
$env:SCARAB_ARTIFACTS = "$env:USERPROFILE\Documents\artifacts"
.\scripts\link-artifacts.ps1
.\scripts\build.ps1
.\tests\run_tests.ps1
```

Linux (cross-compile with LLVM-mingw, tests under Wine):

```bash
export SCARAB_ARTIFACTS=~/Documents/scarab-artifacts
./scripts/link-artifacts.sh
./tests/run_tests.sh
```

Prerequisites, Wine setup, backend selection and troubleshooting:
[BUILDING.md](docs/BUILDING.md).

## Tests

Seven gates; all but G2 run through CTest:
`ctest --test-dir build --output-on-failure`.

| gate | check |
|---|---|
| G1 | Bridge exports == the console runtime: 116/116 names + ordinals, 93/93 GUID bytes |
| G2 | Wrapper vtable layout — 53 + 76 signatures proven by the compiler via `override` |
| G3 | Wrapper self-test: QI routing, refcounting, validation, unknown-slot logging |
| G4 | End-to-end: console DMA command list created through PC D3D12, serving its console IID |
| G5 | Tooling: PE parser, header generator, IID extraction, catalogue completeness |
| G6 | Shader emitter: HLSL → DXC → real DXIL, container verified part-by-part |
| G7 | Backend identity: Linux resolves to vkd3d, no false mismatch warning |

## Scope

- **No vendor binaries or game data, ever.** The console stack and extracted content are
  supplied by you; neither is in this repo.
- **Not a guess.** Every claim in the docs traces to something measured on real binaries.

## Layout

```
components/runtime-bridge      d3d12_xs.dll            the 116-export bridge
components/extension-wrapper   d3d12_xs_wrap.dll       console extension surface
components/shader-bridge       FSSM -> RDNA2 -> DXIL   shader translation
components/memory-adapter      console memory semantics
components/present-adapter     DXGIX presentation
tools/                         PE parser, export verifier, forwarding generator
tests/                         the CTest gates
docs/                          architecture, findings, provenance
```

## Docs

- [MILESTONES.md](docs/MILESTONES.md) — version ladder and exit criteria
- [ROADMAP.md](docs/ROADMAP.md) — bytecode decision, phases, gates, risks
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) — layers, and why each seam is where it is
- [BUILDING.md](docs/BUILDING.md) — Windows and Linux, backends, troubleshooting
- [d3d12-xs-bridge.md](docs/d3d12-xs-bridge.md) — 116-export disposition, proven vs assumed
- [d3d12-xs-wrapper.md](docs/d3d12-xs-wrapper.md) — extension catalogue, IID provenance
- [fssm-analysis.md](docs/fssm-analysis.md) — what the console shaders actually are
- [isa/](docs/isa/) — the console shader ISA as recovered from the vendor compiler

## License

GPLv3 — see [LICENSE](LICENSE). Copyright (C) 2026 Pedro Portela. First-party files are
`GPL-3.0-only` (SPDX headers); `components/shader-bridge/third_party/dxc/dxcapi.h` keeps
Microsoft's University of Illinois Open Source licence.

No third-party binaries or game content are redistributed here. Third-party names are
used descriptively; see [NOTICE.md](NOTICE.md).
