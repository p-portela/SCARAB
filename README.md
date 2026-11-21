# console-translation-layer

Translation layer that runs a **Scarlett (Scarlett) D3D12 title on PC** — bridging
console D3D12 to Windows D3D12. Not an emulator: Scarlett is x86-64, so there is no
instruction-set gap. Only the **OS and GPU-driver surface** needs shimming, and the
console and PC runtimes turn out to define *the same* COM interfaces.

## The insight this is built on

The console runtime and the PC runtime define the same D3D12 interfaces. I extracted
the 16-byte GUID behind every `IID_*` data export of the genuine console
`d3d12_xs.dll` and compared them with the PC Agility SDK headers:

| measurement | result |
|---|---|
| IIDs defined on both sides | 28 |
| byte-identical | **28** |
| different | **0** |

`IID_ID3D12Device` is `189819f1-1db6-4b57-be54-1821339b85f7` on both. Identical IID
means identical interface, identical vtable, identical ABI — so the standard API is
**forwarded, not reimplemented**. Only the console *extension* surface has to be built.

## Status

| component | what it does | state |
|---|---|---|
| `runtime-bridge` | `d3d12_xs.dll`: the console runtime's 116 exports; forwards the standard API to PC `d3d12.dll`, emulates the console-only free functions | **built, contract byte-exact, runs under Wine** |
| `extension-wrapper` | `d3d12_xs_wrap.dll`: the `X` methods the console appends to PC interfaces, console command-list types/IIDs, `CreateCommandList` interception, QI audit | **built, self-test 19/19, integrated end-to-end** |
| `shader-bridge` | FSSM unwrap → RDNA2 stream (done for all 25,321 shaders) → SPIR-V/DXIL via the upstream shader recompiler's recompiler; root-signature → binding synthesis | **in progress** |
| `memory-adapter` | console resource/heap flags, DCC/HTILE compression, tiling → PC heaps | **seam only** |
| `present-adapter` | `DXGIXPresentArray`, vblank/frame notifications → PC DXGI | **seam only** |

Verified end-to-end under Wine today: a console-only command-list type and a
console-only IID round-trip through bridge → wrapper → real PC D3D12, and the object
that comes back serves both the console IID and the standard PC interfaces.

## What this is not

- **No vendor binaries or game data, ever.** The repo builds against Microsoft's console
  console stack redistributables and analyses a game's extracted content. Neither is
  included; `scripts/link-artifacts.sh` symlinks them in from a directory you supply.
  See [docs/PROVENANCE.md](docs/PROVENANCE.md).
- **Not finished, and it will not run a title today.** Two gates are outside this
  project's control: the title executable (absent from the content rip by design) and
  the system OS image (`the system image (encrypted)`, encrypted with the retail ODK). Everything here is
  built so it can advance without them.
- **Not a guess.** Every claim in the docs is traceable to something that was measured
  on real binaries.

## Build

Linux host, no root, no Windows SDK. You need two things placed yourself:

```bash
# 1. LLVM-mingw: a *Linux-hosted* cross toolchain (note: the "winlibs" mingw builds
#    are Windows executables and cannot be used here without Wine)
mkdir -p ~/opt && cd ~/opt
curl -fLO https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64.tar.xz
mkdir -p llvm-mingw && tar -xf llvm-mingw-*.tar.xz -C llvm-mingw --strip-components=1

# 2. Portable Wine, for running the Windows test executables on Linux
mkdir -p ~/opt/wine && cd ~/opt/wine-dl
curl -fLO https://github.com/Kron4ek/Wine-Builds/releases/download/11.22/wine-11.22-staging-amd64.tar.xz
tar -xf wine-11.22-staging-amd64.tar.xz -C ~/opt/wine --strip-components=1
```

Then:

```bash
git clone <this repo> && cd console-translation-layer
export XTL_ARTIFACTS=~/Documents/scarab-artifacts      # where vendor/ and the corpora live
./scripts/link-artifacts.sh                # symlinks vendor/ and corpora/ (gitignored)
./scripts/build.sh                         # cross-compiles both DLLs + the test exes
./tests/run_tests.sh                       # runs every verification gate
```

Wine notes for this host: the prefix must be 64-bit only (`WINEARCH=win64`), and on a
box with no 32-bit glibc you must move `lib/wine/i386-{unix,windows}` aside so Wine
stops reaching for `/lib/ld-linux.so.2`.

## Tests

`tests/run_tests.sh` runs the gates described in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §5 and exits non-zero on any failure:

| gate | check |
|---|---|
| G1 | bridge exports == the console runtime: 116/116 names + ordinals, 93/93 GUID bytes |
| G2 | wrapper vtable layout — the compiler proves 53 + 76 signatures via `override` |
| G3 | wrapper self-test under Wine: QI routing, refcounting, validation, unknown-slot logging |
| G4 | integration: console DMA command list created through PC D3D12, serving its console IID |
| G5 | tooling unit tests: PE parser, header generator, IID extraction, catalogue completeness |

## Layout

```
components/runtime-bridge      d3d12_xs.dll            the 116-export bridge
components/extension-wrapper   d3d12_xs_wrap.dll       console extension surface
components/shader-bridge       FSSM -> RDNA2 -> IR, the upstream shader recompiler integration
components/memory-adapter      interfaces for console memory semantics
components/present-adapter     interfaces for DXGIX presentation
tools/                         PE parser, export verifier, forwarding generator
tests/                         the verification gates
docs/                          architecture, findings, provenance
```

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the layered design and why each seam
  is where it is
- [docs/d3d12-xs-bridge.md](docs/d3d12-xs-bridge.md) — the runtime bridge: full
  116-export disposition table, what is proven vs assumed
- [docs/d3d12-xs-wrapper.md](docs/d3d12-xs-wrapper.md) — the extension surface:
  catalogue, provenance of every console IID, the one dangerous assumption
- [docs/PROVENANCE.md](docs/PROVENANCE.md) — where every artefact comes from
- [docs/fssm-analysis.md](docs/fssm-analysis.md) — what the console shaders actually are
- [docs/isa/](docs/isa/) — the console shader ISA as recovered from the vendor's compiler

## Licence

MIT for the code in this repository; see [LICENSE](LICENSE). No third-party
binaries are redistributed.
