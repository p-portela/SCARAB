# Architecture

## 1. The design premise

Scarlett is x86-64, so a Scarlett title's own machine code runs on a PC unchanged.
What does not exist on a PC is the *environment*: the console OS APIs and the console
GPU driver stack. So this is a translation layer, not an emulator, and the entire
problem is the boundary between the title and the platform.

**Windows is the primary target**; Linux is a supported secondary host. That is not
symmetry for its own sake: on Windows the layer forwards to Microsoft's own D3D12
runtime, which is the reference implementation and what a title would face in the
intended scenario, while on Linux it forwards to vkd3d-proton. Both are worth having,
because a discrepancy between them is a strong signal about which layer is at fault.

That boundary has two halves, and they cost very different amounts:

- **D3D12 itself is cheap to bridge**, because the console and PC runtimes define the
  same COM interfaces. Verified: 28 of 28 shared `IID_*` values are byte-identical.
  Identical IID ⇒ identical vtable ⇒ the standard API can be forwarded. No object model
  to rebuild.
- **The console extensions are the real work**, because they have no PC counterpart by
  definition. These are the `X`-suffixed methods the console appends to PC interfaces,
  a console-only command-list type, console memory semantics, and console presentation.

Shaders are a third axis entirely: the console gives the driver pre-compiled RDNA2
microcode, and PC drivers accept DXIL or SPIR-V.

## 2. Layers

```
  L4  diagnostics        structured logs, QI audit, appended-slot log, UMD/IOCTL trace
      ------------------ seam: structured event log -------------------------------
  L3  shader pipeline    FSSM -> RDNA2 -> IR -> SPIR-V/DXIL
                         + root-signature -> binding synthesis (SRT/BDA)
                         + cooked-PSO (XBSC serialize/deserialize) translation
      ------------------ seam: D3D12UploadCustomMicrocodeX / PSO creation ---------
  L2  API translation    a) runtime bridge   : 116 free functions -> PC d3d12.dll
                         b) extension wrapper: X methods, console list types/IIDs
                         c) memory adapter   : heap flags, DCC/HTILE, tiling
                         d) present adapter  : DXGIX* -> DXGI swapchain/vblank
      ------------------ seam: the exported d3d12_xs.dll surface ------------------
  L1  OS translation     ERA/TitleOS APIs (SlimEra, XDL, consoleAudio2/XMA2)
                         NOT IN SCOPE - seam named so it can be added later
      ------------------ seam: Win32 + console SDK X* APIs --------------------------------
  L0  content            RPF9 containers, package layout, XVD. Read-only tooling.
```

**Why the seams are there.** L2a and L2b are reachable today: they build and run under
Wine, and G4 proves the console command-list path works end-to-end against a real PC
D3D12 runtime. L2c and L2d are known-scope work with the recovery evidence already in
hand. L3 is research with two viable paths. L0 and L1 buy information rather than
function, so they are deliberately not started.

### 2.3 Backends, and why the backend is not a compile-time choice

Every backend implements the same D3D12 entry points under the same names, so "which
platform" reduces to "which `d3d12.dll` do we load". That means the forwarding code in
L2a is byte-for-byte identical on Windows and Linux, and the difference is a runtime
lookup rather than an `#ifdef`:

| backend | platform | how it is reached |
|---|---|---|
| Microsoft D3D12 (system) | Windows | default lookup |
| Microsoft Agility SDK | Windows | an app-local `D3D12Core.dll`; we load the *loader*, never the core (the core exports only `D3D12GetInterface` and `D3D12SDKVersion`) |
| vkd3d-proton (D3D12 → Vulkan) | Linux | automatic under Proton, else `SCARAB_D3D12_DLL` |

`SCARAB_BACKEND` declares the expectation and a mismatch is logged loudly, and the
resolved backend (path + version resource) is written to the log on load — so "which
runtime am I actually on?" is answerable without a debugger.

Two consequences worth stating:

- **DXVK is not the Linux answer.** DXVK covers D3D9/10/11; D3D12 on Vulkan is
  vkd3d-proton. Getting this wrong would send the Linux path down a dead end.
- **L3's output target should be DXIL, not SPIR-V.** vkd3d-proton consumes DXIL, and so
  does Microsoft's runtime, so DXIL is the one target that serves both backends. Emitting
  SPIR-V (which the upstream shader recompiler already does) would be a Linux-only fast path, useful as a
  cross-check but not the primary.

## 3. Components

| component | artefact | responsibility | state |
|---|---|---|---|
| `runtime-bridge` | `d3d12_xs.dll` | the console runtime's 116 exports; forward the standard API, emulate the console-only free functions | built; contract byte-exact; runs under Wine |
| `extension-wrapper` | `d3d12_xs_wrap.dll` | appended `X` methods, console command-list types/IIDs, `CreateCommandList` interception, QI audit | built; self-test 19/19; integrated end-to-end |
| `shader-bridge` | (library + tooling) | FSSM → RDNA2 → **DXIL** (the target both backends accept); SPIR-V as a Linux-only cross-check; root-signature → bindings | extraction done for all 25,321 shaders; recompiler in progress |
| `memory-adapter` | (library) | console resource/heap flags, DCC and HTILE compression, tiling → PC heaps | seam only |
| `present-adapter` | (library) | `DXGIXPresentArray`, vblank/frame notifications → PC DXGI | seam only |

### 3.1 Why two DLLs instead of one

The bridge and the wrapper are separate components, and the bridge loads the wrapper
dynamically.

- The bridge's entire claim is that it is a **drop-in replacement** for the console
  runtime: same name, same 116 exports, same ordinals. That claim is verified by a test
  (G1). Anything that adds risk to that contract belongs in another module.
- The wrapper has an **off-by-design incompleteness**: the order of the appended console
  methods is not recovered. That uncertainty must not contaminate the artefact whose
  correctness is byte-checkable.
- Without the wrapper the bridge still serves the whole standard API; only extension
  calls are lost. That is a graceful degradation, and it is logged.

### 3.2 Why the bridge is header-free and the wrapper is not

The bridge declares its own ABI-exact prototypes and resolves the PC runtime through
`GetProcAddress`. It never owns an interface, so it needs no D3D12 headers and cannot
drift from the PC ABI at compile time.

The wrapper owns interfaces, so it **inherits the genuine ones** from the real Agility
SDK header and marks every forwarded method `override`. The compiler then proves the
PC-side vtable layout instead of us promising it, for all 53 device and 76 command-list
methods. The forwarding bodies are generated from the same header, so they cannot drift
either.

The rule: **code that owns an interface should inherit it; code that only forwards free
functions should not need the header at all.**

## 4. The honesty rule

Every call the title can make is graded, and the grade is visible in the log:

1. **real** — a correct PC equivalent exists and is used.
2. **recorded** — no PC equivalent exists; the call is stored, with its arguments, so a
   later pass can honour it (depth bias and sample mask are PSO fields on PC, so a
   PSO-patching pass is the natural consumer).
3. **refused** — no equivalent and no safe emulation yet: log loudly and return
   `E_NOTIMPL`. Never silently succeed, never guess.

This is not fastidiousness. In a translation layer a silent no-op is indistinguishable
from a translated call, which makes the behaviour of an unmodified title impossible to
reason about. A loud refusal turns "it renders wrong" into "this call is not
implemented", which is actionable.

The same rule drives the instrumentation of the one dangerous assumption: the appended
method **order** is unknown, so every appended slot logs its own name and its raw
arguments, and unknown slots log their index plus all four register arguments. A wrong
order then shows up as a logged name that does not match the call, rather than as silent
corruption.

## 5. Verification gates

| gate | command | pass condition |
|---|---|---|
| G1 export contract | `python3 tools/verify_exports.py <built> <reference>` | 116/116 names + ordinals, 93/93 GUID bytes identical |
| G2 wrapper vtable layout | the build itself | compiles: `override` proves 53 + 76 signatures against the real header |
| G3 wrapper logic | `wine wrapper_selftest.exe` | 0 failures across QI routing, refcounting, validation, unknown-slot logging |
| G4 integration | `wine integration_test.exe` | console DMA command list created through PC D3D12 and serves its console IID |
| G5 tooling | `python3 tests/test_tools.py` | PE parser, header generator, IID extraction, catalogue completeness |

`tests/run_tests.sh` runs all of them and exits non-zero on any failure.

## 6. Evidence base

Every architectural choice above rests on a measurement. The measurements, with where
they came from:

| # | finding | how it was established |
|---|---|---|
| E1 | console D3D12 interfaces are the PC ones | 28/28 shared `IID_*` byte-identical to the PC header |
| E2 | the console runtime exports 116 names, 93 of them GUID data | parsed from the genuine DLL |
| E3 | the console appends `X` methods to PC interfaces | UMD errors name them as `ID3D12Device::CreateCommandQueueX` |
| E4 | console-only command-list type and IID | UMD: *"An riid of `__uuidof(ID3D12ConsDmaCommandList)` is required for `D3D12CONS_COMMAND_LIST_TYPE_DMA`"* |
| E5 | 105 console method names, 106 `D3D12CONS_*` symbols | mined from the UMD's validation strings |
| E6 | the whole console graphics stack runs on Linux | all 7 DLLs load under Wine, 100% of exports resolve |
| E7 | FSSM is RDNA2/gfx10 machine code in an XBCH(zlib)/XDAT container | `s_endpgm` in 25,782/25,782 blobs, control 0/3000 |
| E8 | the vendor ships a shader disassembler | `xbsc_xs.dll` exports `XBSC_DisassembleRawShader` |
| E9 | VOP3 sub-encoding is base+offset, per the vendor's own text | `SQ_V_OP1_OFFSET + SQ_V_MOV_B32 …` |
| E10 | console takes shader bindings from the PSO root signature, not a shader header | `docs/shader-abi-notes.md` |
| E11 | memory-model deltas are structural | DCC/HTILE/indirect-buffer flags in the UMD; `xg_xs.dll` encode/decode |
| E12 | presentation is console-specific | `DXGIXPresentArray` and friends; queue `PresentX` with planes |
| E13 | two gates remain outside this project | title exe absent by design; `the system image (encrypted)` encrypted, retail ODK |
| E14 | the console shader container is the standard DXBC/DXIL container | same fourcc, same parts (`SFI0 ISG1 OSG1 PSV0 HASH DXIL`), same nested `DXIL` tag; at payload+24 the console has `XDAT`→zlib→FSSM where DXC has `BC C0 DE`→bitcode. Measured by comparing a console shader against a locally produced DXC output |
| E15 | we can produce real DXIL locally | `dxcompiler.dll` under Wine compiles HLSL to a container whose DXIL part holds LLVM bitcode — gate G6 |

## 7. Known risks

| risk | impact | mitigation |
|---|---|---|
| appended-method order unknown | wrong slot → wrong function | every slot logs name + arguments; unknown slots log index + 4 register args |
| console IIDs for `ID3D12ConsComputeCommandList*` unrecovered | QI for those fails | QI audit records them from a live title; 7 CANDIDATE UUIDs held but deliberately unused |
| memory-model mismatch (unified memory, DCC, tiling) | wrong or absent compression; possible crashes | `memory-adapter` seam; `xg_xs.dll` is the reference for swizzle/DCC semantics |
| shader translation incomplete | nothing draws | alignment hypothesis first (E9), vendor disassembler as the oracle (E8) |
| title exe and `the system image (encrypted)` closed | no end-to-end run | deliberately out of scope; the design advances without them |
| testing only under Wine | real Windows behaviour unverified | stated plainly; the test executables are plain Windows exes and run natively too |

## 8. Open questions

1. Does the wrapper need the D3D11 `X` surface (`ID3D11DeviceX`, `ID3D11DeviceContextX`,
   `ID3D11DmaEngineContextX`, …)? The console runtime exports those IIDs, so a title may
   use D3D11 on console. Unknown until the title executable exists.
2. Is `ID3D12ConsComputeCommandList` a distinct interface with its own IID, or the
   graphics list type with a different list type? The UMD's phrasing suggests a distinct IID.
3. Should `shader-bridge` depend on the upstream shader recompiler as a submodule or vendor its prebuilt
   recompiler library? A submodule is cleaner but couples the repositories.
4. Numeric values for the recovered `D3D12CONS_*` flags are unknown. They are left
   unassigned on purpose: a plausible-but-wrong value silently mistranslates, which is
   worse than an explicit gap.
