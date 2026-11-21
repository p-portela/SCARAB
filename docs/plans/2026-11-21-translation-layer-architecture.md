# console → PC D3D12 Translation Layer: Architecture & Repo Plan

> **For Hermes:** plan first, then execute the repo creation in the same session.
> Implementation tasks are at the end, in order.

**Goal:** Turn the prototype code in `~/Documents/scarab-artifacts/{bridge,wrap,tools,docs}` into a
proper, buildable, testable repository that translates a Scarlett (Scarlett) D3D12
title onto PC D3D12 — and that keeps growing as more of the console surface is recovered.

**Architecture:** Layered translation. The load-bearing insight is that the console
and PC runtimes define *the same COM interfaces* (verified: 28/28 shared IIDs
byte-identical), so the standard API is **forwarded**, not reimplemented, and only the
*extension* surface is built. Shaders are a separate axis: the console feeds compiled
RDNA2 microcode (FSSM) to the driver, which must be translated to SPIR-V/DXIL.

**Tech stack:** C++17 (components, cross-built with LLVM-mingw for
x86_64-w64-windows-gnu), Python 3.13 (analysis tooling), CMake (build), Wine (test
execution on Linux), `vendor/d3d12/include/d3d12.h` (Agility SDK header, authoritative
ABI), the upstream shader recompiler's RDNA2→SPIR-V recompiler (upstream dependency).

---

## 1. Evidence base (what the architecture is built on)

These are findings from the current work, each with a verification we can point at.

| # | Finding | Verification |
|---|---|---|
| E1 | Console D3D12 = PC D3D12 COM interfaces | 28/28 shared `IID_*` byte-identical to the PC header; `IID_ID3D12Device` = `189819f1-…` |
| E2 | Console runtime shipped 116 exports: 23 functions, 93 GUID data | `tools/verify_exports.py` against the genuine DLL |
| E3 | The console appends methods to PC interfaces (`X` methods) | UMD errors read `ID3D12Device::CreateCommandQueueX …`, `D3d12GraphicsCommandList::RSSetDepthBiasX …` |
| E4 | Console-only command-list type + IID | UMD: *"CreateCommandList: An riid of `__uuidof(ID3D12ConsDmaCommandList)` is required for `D3D12CONS_COMMAND_LIST_TYPE_DMA`"*; `IID_ID3D12ConsDmaCommandList` exported at ordinal 100 |
| E5 | Full extension catalogue: 105 methods, 106 `D3D12CONS_*` symbols | mined from the UMD's validation strings → `catalogue.tsv` |
| E6 | The whole console graphics stack runs on Linux | all 7 DLLs load under Wine, 100% of exports resolve |
| E7 | FSSM = RDNA2/gfx10 machine code in an XBCH(zlib)/XDAT container, inside a DXIL-shaped container | `s_endpgm` only in 25,782/25,782 blobs, control 0/3000; `XBSC_*`/`FSSM` magics present in the console shader compiler, absent from PC DXC |
| E8 | The vendor ships a shader **disassembler** | `xbsc_xs.dll` exports `XBSC_DisassembleRawShader`, `XBSC_AssembleShader`, 43 exports total, loads under Wine |
| E9 | VOP3 sub-encoding is base+offset, per the vendor's own text | `SQ_V_OP1_OFFSET + SQ_V_MOV_B32 …`; the upstream shader recompiler implements 0x100/0x180 consistently ⇒ the sweep failures are stream-alignment, not table errors |
| E10 | console takes shader bindings from the PSO root signature, not a per-shader header | `shader-abi-notes.md`; the upstream shader recompiler's recompiler never reads the PS5 shader header |
| E11 | Memory-model deltas are structural: DCC, HTILE resummarize, indirect-buffer heap flag, unified memory | UMD validation strings; `xg_xs.dll` (DCC encode/decode, swizzle modes) |
| E12 | Presentation is console-specific | `DXGIXPresentArray`, `DXGIXSetFrameNotification`, `DXGIXSetVLineNotification`, `DXGIXGetFrameStatistics`; queue `PresentX` with planes |
| E13 | Two hard gates remain | title exe absent from the rip by design; `the system image (encrypted)` encrypted, ODK slot 0 (retail) |

## 2. Architecture

Five layers, each with an explicit seam so work can proceed without the title exe.

```
  L4  diagnostics        logs, QI audit, appended-slot log, IOCTL/UMD trace
      ------------------- seam: structured event log ------------------------
  L3  shader pipeline    FSSM -> RDNA2 -> IR -> SPIR-V/DXIL
                         + root-signature -> binding-table synthesis (SRT/BDA)
                         + cooked-PSO (XBSC serialize/deserialize) translation
      ------------------- seam: D3D12UploadCustomMicrocodeX / PSO creation ---
  L2  API translation    a) runtime bridge   : 116 free functions -> PC d3d12.dll
                         b) extension wrapper: X methods, console list types/IIDs
                         c) memory adapter   : heap flags, DCC/HTILE, tiling (XG)
                         d) present adapter  : DXGIX* -> DXGI swapchain/vblank
      ------------------- seam: the exported d3d12_xs.dll surface ------------
  L1  OS translation     ERA/TitleOS APIs (SlimEra, XDL, consoleAudio2/XMA2)
                         NOT IN SCOPE YET - seam named so it can be added
      ------------------- seam: Win32 + console SDK X* APIs --------------------------
  L0  content            RPF9 containers, package layout, XVD
                         tooling only, read-only
```

Why this split: L2a/L2b are *within reach today* (they run under Wine and are proven
end-to-end), L2c/L2d are known-scope work items with recovery evidence already in hand,
L3 is research with two viable paths, and L0/L1 buy information rather than function.

### 2.1 Component responsibilities

| component | responsibility | state |
|---|---|---|
| `runtime-bridge` (`d3d12_xs.dll`) | the 116 exports; forwards the standard API to PC d3d12.dll; emulates console-only free functions | **built, contract verified, executed under Wine** |
| `extension-wrapper` (`d3d12_xs_wrap.dll`) | appended `X` methods, console command-list types/IIDs, `CreateCommandList` interception, QI audit | **built, self-test 19/19, integrated end-to-end under Wine** |
| `shader-bridge` | FSSM unwrap → RDNA2 stream (done) → SPIR-V/DXIL (via the upstream shader recompiler); root-signature→bindings | partially done (`fssm_to_rdna2.py` extracts 25,321 streams; recompiler links) |
| `memory-adapter` | `D3D12CONS_RESOURCE_FLAG_*` / heap flags / DCC / tiling semantics onto PC heaps and `xg_xs` equivalents | **not started**, interface to be defined now |
| `present-adapter` | `DXGIXPresentArray` / vblank / frame events onto PC swapchains | **not started**, interface to be defined now |
| `console-tooling` | run the vendor's own toolchain under Wine: disassemble FSSM, compile DXIL, build BVH | loadable, call signatures unrecovered |

### 2.2 The design rule that keeps this honest

Everything the title can call is graded, and the grade is visible in the log:

1. **real** — a correct PC equivalent exists and is used
2. **recorded** — no PC equivalent; the call is stored so a later pass can honour it
   (e.g. depth bias, sample mask → PSO patching)
3. **refused** — no equivalent and no safe emulation yet: log loudly, return
   `E_NOTIMPL`. Never silently succeed, never guess

## 3. Repo layout

```
scarab/
  CMakeLists.txt                  top-level; drives both components
  cmake/toolchain-mingw.cmake     Linux -> x86_64-w64-windows-gnu (LLVM-mingw)
  README.md                       what this is, how to build, how to test
  LICENSE                         GPL-3.0-only for our code
  .gitignore                      vendor/ corpora/ artifacts are NEVER committed
  components/
    runtime-bridge/               (was artifacts/bridge)   d3d12_xs.dll
    extension-wrapper/            (was artifacts/wrap)     d3d12_xs_wrap.dll
    shader-bridge/                FSSM -> RDNA2 -> IR; the recompiler integration
    memory-adapter/               interface + stubs (next real work)
    present-adapter/              interface + stubs
  tools/                          analysis tooling (Python 3)
    peinfo.py                     PE/export/import/magic parser
    verify_exports.py             export-contract verification (CI gate)
    gen_wrappers.py               generates ABI-exact forwarding from the real header
    fssm_lib.py fssm_parse.py fssm_to_rdna2.py
    extract_shaders.py
    peload.c peload.exe           loads a PE under Wine, resolves exports
  tests/
    run_tests.sh                  builds, runs every test, exits non-zero on failure
    test_export_contract.py       bridge exports == console runtime, byte-exact
    test_wrapper_selftest.sh      wrapper logic, no GPU
    test_integration.sh           bridge+wrap+PC D3D12 end-to-end under Wine
    test_tools.py                 tool unit tests (parser, GUID extraction, generator)
  docs/
    ARCHITECTURE.md               this plan's §2, kept current
    d3d12-xs-bridge.md            runtime bridge detail
    d3d12-xs-wrapper.md           extension wrapper detail
    console-extension-catalogue.tsv  105 methods + 106 symbols, with evidence
    isa/                          ISA mnemonics + spec text + notes
    ... (existing analysis docs move here)
  scripts/
    link-artifacts.sh             symlink vendor/ and corpora/ from $SCARAB_ARTIFACTS
    build.sh                      configure + build via CMake
    test.sh                       wrapper for tests/run_tests.sh
```

Repo at `~/Documents/scarab`. Game artifacts stay where they are
(`~/Documents/scarab-artifacts`); the repo never contains them.

## 4. Decisions and their reasons

1. **Two DLLs, not one.** `runtime-bridge` and `extension-wrapper` are separable
   components; the bridge loads the wrapper dynamically. Reason: the bridge's export
   contract must stay exactly what the console runtime published (that is its whole
   claim to being a drop-in), and the wrapper has an off-by-design incompleteness
   (unknown appended order) that must not contaminate it.
2. **The bridge stays header-free; the wrapper deliberately uses the real header.**
   Reason: code that *owns* an interface should inherit it so the compiler enforces
   layout; code that only forwards free functions needs no headers at all.
3. **No vendor binaries or game data in the repo, ever.** Reason: they are Microsoft's
   and the publisher's. `scripts/link-artifacts.sh` instead.
4. **CMake with a mingw toolchain file**, keeping thin `scripts/build.sh`. Reason:
   standard, lets tests be CTest targets, and the toolchain file documents the exact
   cross-build. The prototype build scripts remain for reference.
5. **Grading over stubbing** (§2.2). Reason: a silent no-op here is indistinguishable
   from a translated call, which makes debugging a title impossible.
6. **CMake over plain scripts**: the repo will grow components; the cost is one
   toolchain file now versus ad-hoc scripts forever.

## 5. Verification strategy

Every claim in the repo must be reproducible by a command. The gates:

| gate | command | pass condition |
|---|---|---|
| G1 export contract | `python3 tools/verify_exports.py` | bridge = 116/116 names+ordinals, 93/93 GUID bytes |
| G2 wrapper layout | wrapper build | compiles: `override` proves 53+76 signatures |
| G3 wrapper logic | `wine wrapper_selftest.exe` | 0 failures |
| G4 integration | `wine integration_test.exe` | console DMA list created and serves its console IID |
| G5 tools | `pytest tests/test_tools.py` | parser/generator unit tests pass |
| G6 clean build | `scripts/build.sh` from a fresh clone + linked artifacts | builds with no manual steps |

G1–G4 already pass today; the repo work must keep them passing across the move.

## 6. Implementation tasks

Bite-sized, in order. Each ends with a commit.

### Task 1: scaffold the repo
- Create `~/Documents/scarab` with `git init`, `.gitignore`
  (`vendor/`, `corpora/`, `build/`, `*.log`, `*.exe`, `*.dll`, `__pycache__/`),
  `LICENSE` (GPL-3.0-only), directory skeleton.
- Verify: `git status` shows only `.gitignore`/`LICENSE` as untracked.

### Task 2: move the components
- `git mv` semantics by hand: `artifacts/bridge` → `components/runtime-bridge`,
  `artifacts/wrap` → `components/extension-wrapper`, `artifacts/tools` → `tools/`,
  `artifacts/docs/*` → `docs/`.
- Drop build outputs (`build/`, `*.exe`, `*.log`, `*.wrap.log`, `*.a`).
- Verify: tree matches §3.

### Task 3: de-hardcode paths
- `tools/gen_wrappers.py`: header path from `$SCARAB_VENDOR` or `vendor/`.
- `tools/verify_exports.py`: reference DLL from `$SCARAB_VENDOR` or `vendor/`.
- `tools/fssm_*.py`, `extract_shaders.py`: corpus paths from `$SCARAB_CORPORA`.
- `scripts/link-artifacts.sh` creates the symlinks.
- Verify: `scripts/link-artifacts.sh && python3 tools/verify_exports.py` → PASS.

### Task 4: CMake
- `cmake/toolchain-mingw.cmake` (Windows system name, LLVM-mingw compilers).
- Top-level `CMakeLists.txt` + per-component `CMakeLists.txt`.
- Port the two `.def` files as-is; port generated `.inc` step as a custom command.
- Verify: `scripts/build.sh` produces both DLLs; G1 still PASS.

### Task 5: tests as the gate
- `tests/run_tests.sh` running G1–G5, exiting non-zero on any failure.
- Verify: run it; all green; deliberately break G1 (truncate the .def) and confirm it fails.

### Task 6: documentation
- `README.md`: what it is, the E1 insight, build, test, status matrix, caveats.
- `docs/ARCHITECTURE.md` from §2; `CONTRIBUTING.md` short.
- Verify: a reader can go from clone to green tests using only README.

### Task 7: define the unstarted seams
- `components/memory-adapter/` and `present-adapter/`: header-only interfaces with the
  method lists taken from the catalogue, documented, throwing `E_NOTIMPL`. These exist
  so the next work item has a home rather than a blank page.
- Verify: they compile into the wrapper build as an optional unit.

### Task 8: first commit + tag
- `git add`, commit "feat: initial translation layer repository", tag `v0.1.0`.
- No remote, no push (ask first).

## 7. Risks, tradeoffs, open questions

| risk | impact | mitigation |
|---|---|---|
| Appended-method **order** is unknown | wrong slot → wrong function | every slot logs name+args; unknown slots log index + 4 register args |
| Console IIDs for `ID3D12ConsComputeCommandList*` unrecovered | QI for those fails | QI audit log records them from a live title; 7 CANDIDATE UUIDs held but unused |
| Memory model (unified memory, DCC, tiling) | rendering correct but wrong/absent compression, or crashes | `memory-adapter` seam; `xg_xs.dll` is the reference for swizzle/DCC semantics |
| Shader translation incomplete (decoder gaps, no validation layer) | nothing draws | alignment hypothesis first (E9), then vendor disassembler as oracle (E8) |
| Title exe and `the system image (encrypted)` gates | no end-to-end run, ever, until they open | deliberately out of scope; everything is built so it can advance without them |
| Legal | shipping MS/the publisher binaries | nothing bundled; note xvdtool's own policy on leaked material |
| Wine-only testing | real Windows behaviour unverified | state it plainly in README; keep tests runnable on Windows too |

**Open questions**
1. Does the wrapper need to cover the D3D11 `X` surface (`ID3D11DeviceX`, …)? The console
   runtime exports those IIDs, so a title may use D3D11 on console. Unknown until the exe.
2. Is `ID3D12ConsComputeCommandList` a distinct interface with its own IID, or an alias
   of the graphics list with a different list type? UMD phrasing suggests a distinct IID.
3. Should `shader-bridge` depend on the upstream shader recompiler as a git submodule or vendor its `.a`?
   Submodule is cleaner but couples the repos; decide when the decoder sweep is clean.
4. CMake vs keeping the shell scripts: CMake chosen; revisit if it obstructs the
   Wine test loop (per-run cost).
