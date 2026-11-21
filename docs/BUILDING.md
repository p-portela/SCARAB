# Building SCARAB

Two hosts are supported, and one CMake project serves both.

| host | status | toolchain | tests run |
|---|---|---|---|
| **Windows** | **primary target** | MSVC, clang-cl, or LLVM-mingw — native | natively |
| Linux | secondary | LLVM-mingw cross-compiling for Windows | under Wine |

The only platform-conditional logic in the build lives in
[`cmake/scarab-platform.cmake`](../cmake/scarab-platform.cmake); the components are
declarative and never ask which compiler they are on.

## Windows (primary)

Prerequisites:

- **Visual Studio 2022** with "Desktop development with C++" (or LLVM/clang-cl, or a
  native LLVM-mingw)
- **CMake** ≥ 3.20
- **Python 3** (any recent; the build finds it, so it does not have to be on `PATH` as
  `python3` — that is a Linux-ism)

```powershell
git clone <this repo> scarab; cd scarab
$env:SCARAB_ARTIFACTS = "$env:USERPROFILE\Documents\artifacts"   # where vendor/ lives
.\scripts\link-artifacts.ps1
.\scripts\build.ps1                     # configure + build
.\tests\run_tests.ps1                   # every gate
```

`link-artifacts.ps1` creates **directory junctions**, not symlinks, so it works without
Administrator rights or Developer Mode.

Without the scripts:

```powershell
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

## Linux (cross-compile, secondary)

You need a **Linux-hosted** cross toolchain. Note that the popular "winlibs" mingw
builds are Windows executables and cannot be used here without Wine — use LLVM-mingw:

```bash
mkdir -p ~/opt && cd ~/opt
curl -fLO https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64.tar.xz
mkdir -p llvm-mingw && tar -xf llvm-mingw-*.tar.xz -C llvm-mingw --strip-components=1

# portable Wine, for running the Windows test executables
mkdir -p ~/opt/wine-dl && cd ~/opt/wine-dl
curl -fLO https://github.com/Kron4ek/Wine-Builds/releases/download/11.22/wine-11.22-staging-amd64.tar.xz
mkdir -p ~/opt/wine && tar -xf wine-11.22-staging-amd64.tar.xz -C ~/opt/wine --strip-components=1
```

```bash
export SCARAB_ARTIFACTS=~/Documents/scarab-artifacts
./scripts/link-artifacts.sh
./tests/run_tests.sh
```

Wine needs a 64-bit-only prefix, and on a host with no 32-bit glibc you must move the
32-bit parts aside or Wine will try to exec `/lib/ld-linux.so.2`:

```bash
export WINEPREFIX=$HOME/.wine-scarab WINEARCH=win64
mv ~/opt/wine/lib/wine/i386-unix    ~/opt/wine/lib/wine/i386-unix.disabled
mv ~/opt/wine/lib/wine/i386-windows ~/opt/wine/lib/wine/i386-windows.disabled
```

## Backends: what the layer forwards *to*

Which PC runtime SCARAB forwards to is a runtime choice, not a build choice, because
every backend implements the same D3D12 entry points under the same names. The seam is
therefore "which `d3d12.dll` do we load".

| backend | platform | how it is selected |
|---|---|---|
| Microsoft D3D12 (system) | Windows | default |
| Microsoft Agility SDK | Windows | ship `D3D12Core.dll` next to the title; the loader picks it up. SCARAB always loads the *loader* `d3d12.dll`, never `D3D12Core.dll` — that one exports only `D3D12GetInterface` and `D3D12SDKVersion` |
| **vkd3d-proton** (D3D12 → Vulkan) | Linux | under Proton it resolves automatically; otherwise set `SCARAB_D3D12_DLL` to vkd3d's `d3d12.dll` |

Environment overrides:

| variable | meaning |
|---|---|
| `SCARAB_D3D12_DLL` | absolute path to the runtime to load. Wins over everything. |
| `SCARAB_BACKEND` | `auto` (default), `d3d12`, or `vkd3d`. Declares the expectation; a mismatch with what actually loaded is logged as a warning rather than silently tolerated. |

The bridge logs the resolved backend on load, so "which runtime am I actually on?" is
answerable from `d3d12_xs.bridge.log` alone:

```
backend: Microsoft d3d12 (system)
backend: C:\windows\system32\d3d12.dll
```

**DXVK is not a backend here.** DXVK covers D3D9/10/11; D3D12 on Vulkan is
vkd3d-proton's job. `d3d12_xs_wrap.dll` and the shader path are backend-independent.

## Cross-platform consequences worth knowing

### Where Wine is and is not needed

| activity | needs Wine on Linux? |
|---|---|
| configuring and building | **no** — LLVM-mingw cross-compiles with plain clang; Wine appears in the build only as the test-launcher variable |
| gates G1 (export contract) and G5 (tooling) | **no** — plain `python3` |
| gates G3 / G4 / G6 | **yes** — they are Windows PEs, and a PE cannot execute on Linux without a Windows loader. These tests are specifically *about* Windows ABI behaviour (COM vtables, PE loading), so this is structural |
| all five gates on Windows | **no** — they run natively. The primary target needs no Wine at all |
| producing DXIL for the shader path | **not for long** — currently G6 drives the *vendor's* `dxcompiler.dll` under Wine, but that is a convenience, not a requirement: Microsoft ships native Linux DXC (`dxc`, `libdxcompiler.so`, `libdxil.so`) and we should use it. See ROADMAP §6 |
| running a title on Linux | **yes, by definition** — the title is a Windows PE. It runs under Wine/Proton; SCARAB plugs into that process and neither adds nor removes the requirement |

So: the build is Wine-free, the primary (Windows) test path is Wine-free, and the one
place Wine is used as a *convenience* rather than a necessity — the shader compiler —
is scheduled to lose it.

- **Shaders are the main one.** vkd3d-proton consumes **DXIL**, not SPIR-V, so DXIL is
  the portable target for the shader bridge: it works on Microsoft's runtime *and* on
  vkd3d-proton. Emitting SPIR-V only would lock that work to the Linux path. See
  [`components/shader-bridge/README.md`](../components/shader-bridge/README.md).
- **The extension wrapper is entirely backend-neutral.** It only implements COM
  interfaces and forwards through vtables; it does not know or care which runtime is
  underneath.
- **Testing on Linux is not testing on Windows.** Wine's `d3d12.dll` is a partial
  implementation and reports no version resource. The gates that matter for correctness
  on Windows (G1 export contract, G2 layout) are platform-independent, but G4's device
  behaviour must eventually be re-run on the real target.

## What still needs the main machine

Nothing here has been run against a real GPU. The pieces to verify on the Windows box:

1. `.\tests\run_tests.ps1` — expect all four gates green, natively.
2. Check the backend log line names a real `d3d12.dll` **with a version resource**;
   Wine's does not have one, Microsoft's does. That is the first real signal.
3. `D3D12CreateDevice` against real hardware, then a console DMA command list (what G4
   does) and see whether the wrapper's log matches.
4. `SCARAB_BACKEND=vkd3d` on a Linux+Proton box as the cross-check.

## Troubleshooting

| symptom | cause |
|---|---|
| `G3/G4` fail with `Syntax error: word unexpected` | the test launcher was empty, so the shell tried to execute a PE. Set `WIN32`-independent detection correctly (`CMAKE_CROSSCOMPILING`), or pass `-DSCARAB_TEST_LAUNCHER=/path/to/wine` |
| generator step produces nothing / stale stubs are used | the generated files must be listed as `HEADER_FILE_ONLY` sources or CMake never runs the custom command |
| `undefined symbol: __mingw_uuidof<...>` | `__uuidof` / `IID_PPV_ARGS` do not work for MIDL interfaces under clang-mingw. Use the `IID_…` constants |
| `cmake` cannot find `python3` on Windows | the build resolves Python via `find_package(Python3)`; do not hard-code `python3` in a custom command |
