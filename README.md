# SCARAB

**S**carlett **C**ompatibility **A**nd **R**untime **A**PI **B**ridge — runs an console
Series (Scarlett) D3D12 title on PC, bridging console D3D12 to Windows D3D12.

Not an emulator. Scarlett is x86-64, so there is no instruction-set gap; only the OS
and GPU-driver surface needs shimming. And the console and PC runtimes turn out to define
*the same* COM interfaces, which is what makes this tractable.

Primary target is **Windows**; Linux works too and is a first-class build path.

## The insight this is built on

I extracted the 16-byte GUID behind every `IID_*` data export of the genuine console
`d3d12_xs.dll` and compared them against the PC Agility SDK headers:

| measurement | result |
|---|---|
| IIDs defined on both sides | 28 |
| byte-identical | **28** |
| different | **0** |

`IID_ID3D12Device` is `189819f1-1db6-4b57-be54-1821339b85f7` on both. Identical IID means
identical interface, identical vtable, identical ABI — so the standard D3D12 API is
**forwarded, not reimplemented**. Only the console *extension* surface has to be built.

## Status

| component | what it does | state |
|---|---|---|
| `runtime-bridge` | `d3d12_xs.dll`: the console runtime's 116 exports; forwards the standard API to a PC D3D12 runtime, emulates the console-only free functions | **built, contract byte-exact, runs under Wine** |
| `extension-wrapper` | `d3d12_xs_wrap.dll`: the `X` methods the console appends to PC interfaces, console command-list types/IIDs, `CreateCommandList` interception, QI audit | **built, self-test 19/19, integrated end-to-end** |
| `shader-bridge` | FSSM unwrap → RDNA2 stream (done for all 25,321 shaders) → DXIL via the upstream shader recompiler front-end; root-signature → binding synthesis | **in progress** |
| `memory-adapter` | console resource/heap flags, DCC/HTILE compression, tiling → PC heaps | **seam only** |
| `present-adapter` | `DXGIXPresentArray`, vblank/frame notifications → PC DXGI | **seam only** |

Verified today under Wine: a console-only command-list type and a console-only IID
round-trip through bridge → wrapper → a real PC D3D12 runtime, and the object that comes
back serves both the console IID and the standard PC interfaces.

## Backends

Which PC runtime SCARAB forwards to is a runtime choice, not a build choice — every
backend implements the same entry points under the same names, so the seam is simply
"which `d3d12.dll` do we load".

| backend | platform | selection |
|---|---|---|
| Microsoft D3D12 (system) | Windows | default |
| Microsoft Agility SDK | Windows | ship `D3D12Core.dll` next to the title |
| **vkd3d-proton** (D3D12 → Vulkan) | Linux | automatic under Proton, or set `SCARAB_D3D12_DLL` |

`SCARAB_BACKEND=auto|d3d12|vkd3d` declares the expectation and warns on a mismatch. The
resolved backend is logged on load. **DXVK is not a backend here** — DXVK is D3D9/10/11;
D3D12 on Vulkan is vkd3d-proton.

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

Full prerequisites, Wine setup, backend selection and a troubleshooting table:
[docs/BUILDING.md](docs/BUILDING.md).

## What this is not

- **No vendor binaries or game data, ever.** The repo builds against Microsoft's console
  console stack redistributables and analyses a game's extracted content. Neither is
  included; the link scripts connect them from a directory you supply.
- **Not finished, and it will not run a title today.** Two gates are outside this
  project's control: the title executable (absent from the content rip by design) and the
  system OS image (`the system image (encrypted)`, encrypted with the retail ODK). Everything is built so it
  can advance without them.
- **Not a guess.** Every claim in the docs is traceable to something measured on real
  binaries.

## Tests

The gates are registered with CTest, so the command is the same everywhere
(`ctest --test-dir build --output-on-failure`); `tests/run_tests.{sh,ps1}` just build
first. On Windows they run natively, on Linux through Wine.

| gate | check |
|---|---|
| G1 | bridge exports == the console runtime: 116/116 names + ordinals, 93/93 GUID bytes |
| G2 | wrapper vtable layout — the compiler proves 53 + 76 signatures via `override` |
| G3 | wrapper self-test: QI routing, refcounting, validation, unknown-slot logging |
| G4 | integration: console DMA command list created through PC D3D12, serving its console IID |
| G5 | tooling unit tests: PE parser, header generator, IID extraction, catalogue completeness |
| G6 | shader emitter: HLSL → DXC → real DXIL, container verified part-by-part (skips if the vendor DXC is absent) |
| G7 | backend identity: on Linux the resolved runtime is vkd3d (D3D12 → Vulkan), not mislabelled as Microsoft's, with no `SCARAB_BACKEND` mismatch warning |

## Shader target: DXIL

The console shader container and a DXC-produced PC shader are **the same container** —
same `DXBC` fourcc, same part layout, same nested `DXIL` tag, and at the same payload
offset (+24) the console puts `XDAT`→zlib→FSSM microcode where the PC puts LLVM
bitcode. So the target is **DXIL (LLVM bitcode, SM 6.x) in a DXBC container**, which is
what both backends consume: Microsoft's D3D12 runtime on Windows, and vkd3d-proton on
Linux. Rationale and version policy: [docs/ROADMAP.md](docs/ROADMAP.md) §0.

## Layout

```
components/runtime-bridge      d3d12_xs.dll            the 116-export bridge
components/extension-wrapper   d3d12_xs_wrap.dll       console extension surface
components/shader-bridge       FSSM -> RDNA2 -> DXIL, the upstream shader recompiler integration
components/memory-adapter      interfaces for console memory semantics
components/present-adapter     interfaces for DXGIX presentation
tools/                         PE parser, export verifier, forwarding generator
tests/                         the CTest gates
docs/                          architecture, findings, provenance
```

## Documentation

- [docs/MILESTONES.md](docs/MILESTONES.md) — the version ladder: what each version means
  and the exit criteria that must be evidenced before it is tagged
- [docs/ROADMAP.md](docs/ROADMAP.md) — the bytecode decision, phases, gates and risks
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the layered design, and why each seam is
  where it is
- [docs/BUILDING.md](docs/BUILDING.md) — Windows and Linux, backends, troubleshooting
- [docs/d3d12-xs-bridge.md](docs/d3d12-xs-bridge.md) — the runtime bridge: full 116-export
  disposition table, what is proven vs assumed
- [docs/d3d12-xs-wrapper.md](docs/d3d12-xs-wrapper.md) — the extension surface: catalogue,
  provenance of every console IID, the one dangerous assumption
- [docs/fssm-analysis.md](docs/fssm-analysis.md) — what the console shaders actually are
- [docs/isa/](docs/isa/) — the console shader ISA as recovered from the vendor's compiler

## Licence

GNU General Public License v3.0 — see [LICENSE](LICENSE). Copyright (C) 2026 Pedro
Portela. Every first-party file is `GPL-3.0-only` (SPDX headers); the one exception is
`components/shader-bridge/third_party/dxc/dxcapi.h`, which keeps Microsoft's own
University of Illinois Open Source licence.

No third-party binaries and no game content are redistributed by this repository.
Third-party names are used descriptively, see [NOTICE.md](NOTICE.md).
