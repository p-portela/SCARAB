<h1 align="center">SCARAB</h1>

<h4 align="center"><b>S</b>carlett <b>C</b>ompatibility <b>A</b>nd <b>R</b>untime <b>A</b>PI <b>B</b>ridge</h4>

<p align="center">A translation layer that runs a console (Scarlett) D3D12 title on PC.</p>

<p align="center">
  <a href="#status">Status</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#build">Build</a> ·
  <a href="#docs">Docs</a> ·
  <a href="#license">License</a>
</p>

SCARAB is not an emulator. Scarlett is x86-64, so the title's own code runs unchanged —
only the console OS and GPU-driver surface needs replacing. Console and PC D3D12 are close
enough that standard calls are forwarded to a PC runtime; only the console-only extensions
are implemented here. **Windows** is the primary target; **Linux** works through
vkd3d-proton.

## Status

| component | what it does | state |
|---|---|---|
| `runtime-bridge` | the console D3D12 runtime | built, verified byte-exact, runs under Wine |
| `extension-wrapper` | console-only D3D12 extensions | built and integrated |
| `shader-bridge` | console shader translation | extraction done; recompilation in progress |
| `memory-adapter` | console memory and heap handling | interface only |
| `present-adapter` | console presentation | interface only |

> **Nothing runs a title yet.** The title executable and the console system image are not
> part of this project; everything is built to advance without them. See
> [MILESTONES.md](docs/MILESTONES.md).

## How it works

- Standard D3D12 is forwarded to the PC runtime. The console-only `X` methods and
  command-list types are added by the optional wrapper, so the bridge still works without it.
- Console shaders ship as precompiled GPU code and are translated to DXIL, which both
  Windows D3D12 and vkd3d-proton accept.
- The PC backend is picked at runtime, not at build time:

| backend | platform |
|---|---|
| Microsoft D3D12 (system) | Windows |
| Microsoft Agility SDK | Windows |
| vkd3d-proton (D3D12 → Vulkan) | Linux |

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

Prerequisites, Wine setup and troubleshooting: [BUILDING.md](docs/BUILDING.md).

## Layout

```
components/runtime-bridge      d3d12_xs.dll            the console D3D12 runtime
components/extension-wrapper   d3d12_xs_wrap.dll       console-only extensions
components/shader-bridge       FSSM -> RDNA2 -> DXIL   shader translation
components/memory-adapter      console memory semantics
components/present-adapter     console presentation
tools/                         PE parser, export verifier, forwarding generator
tests/                         the verification gates
docs/                          architecture, findings, provenance
```

## Docs

- [MILESTONES.md](docs/MILESTONES.md) — version ladder and exit criteria
- [ROADMAP.md](docs/ROADMAP.md) — bytecode decision, phases, risks
- [ARCHITECTURE.md](docs/ARCHITECTURE.md) — the layered design
- [BUILDING.md](docs/BUILDING.md) — Windows and Linux, backends, troubleshooting
- [d3d12-xs-bridge.md](docs/d3d12-xs-bridge.md) — the runtime bridge in depth
- [d3d12-xs-wrapper.md](docs/d3d12-xs-wrapper.md) — the extension surface in depth
- [fssm-analysis.md](docs/fssm-analysis.md) — what the console shaders are
- [isa/](docs/isa/) — the console shader ISA

## License

GPLv3 — see [LICENSE](LICENSE). Copyright (C) 2026 Pedro Portela.

No vendor binaries or game content are redistributed here. Third-party names are used
descriptively; see [NOTICE.md](NOTICE.md).
