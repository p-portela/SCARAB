# Wrapper layer: the console D3D12 extension surface

Status: **builds, self-tests clean, and runs end-to-end under Wine.** See §5.

## 1. Why a wrapper is needed at all

The bridge (`bridge/`) forwards the console runtime's *free functions* to the PC
runtime. That covers the standard D3D12 API. It does not cover the extension
surface, for two reasons that are both visible in the console UMD's own error
strings:

1. The console **appends methods to the PC interfaces**. The UMD reports errors
   as `ID3D12Device::CreateCommandQueueX ...`, `ID3D12Device::SetFrameIntervalX
   ...`, `D3d12GraphicsCommandList::RSSetDepthBiasX ...` — the `X` methods sit on
   the same vtables as the PC methods, after them.
2. There is a **console-only command-list type**. Verbatim from the UMD:
   > `CreateCommandList: An riid of __uuidof(ID3D12ConsDmaCommandList) is
   > required for D3D12CONS_COMMAND_LIST_TYPE_DMA.`

A plain PC device handed back to the title cannot serve either. So the objects
the title receives must be wrappers: byte-identical in their PC part, extended in
their console part.

## 2. Design

```
title
  -> bridge/d3d12_xs.dll         116 exports, contract byte-exact vs the console runtime
       |  D3D12CreateDevice / D3D12ConsCreateDevice
       |     -> PC d3d12.dll -> real GPU
       |     -> loads d3d12_xs_wrap.dll (optional) and substitutes the device
       v
     wrap/d3d12_xs_wrap.dll
       DeviceWrapper          : ID3D12DeviceX          (ID3D12Device4 + console methods)
         CreateCommandList    : intercepts the console list type + console riid
       CommandListWrapper     : ID3D12ConsDmaCommandList
                                (ID3D12GraphicsCommandList6 + graphics + DMA extensions)
```

The two artefacts are separate components on purpose:

- **The bridge is header-free and has no link-time dependency on the wrapper.**
  It resolves `scarab_wrap_device` with `LoadLibrary`/`GetProcAddress`. Without
  the wrapper the bridge still serves the entire standard API and only extension
  calls are lost — and its export contract stays exactly what the console runtime
  published (still verified byte-exact, 116/116).
- **The wrapper deliberately depends on the real Agility SDK header**
  (`vendor/d3d12/include/d3d12.h`). That is what makes the compiler enforce the
  PC-side vtable layout. This is the opposite of the bridge's design choice, and
  the contrast is intentional: code that *owns* an interface should inherit it;
  code that only *forwards free functions* should not need the header at all.

### Why inheriting the real interfaces matters

The wrapper classes derive from the genuine interfaces and mark every forwarded
method `override`. The compiler therefore verifies all **53 device** and **76
command-list** signatures against the real ABI. There is no hand-written vtable
and no possibility of an off-by-one slot in the part that is known.

The forwarding bodies are *generated* from the real header by
`tools/gen_wrappers.py`, so they cannot drift either. The generator is not a
convenience: it is the mechanism that makes "implement 129 methods correctly" a
mechanical operation instead of 129 chances to make a mistake.

## 3. What is proven and what is assumed

| thing | status |
|---|---|
| PC-side vtable layout of the wrapper | **proven** — compiler-checked via `override` against the real header |
| Which console methods exist | **recovered** — 105 method names + 106 `D3D12CONS_*` symbols, mined from the UMD's validation strings |
| Which class a method belongs to | **partly recovered** — 10 methods are class-attributed by UMD messages that name the interface (`ID3D12Device::CreateCommandQueueX`, `ID3D12ConsDmaCommandList::FillRawBuffer`, `ID3D12CommandQueue::PresentX`, …), and 6 more are corroborated by call sites in the vendor's public sample set (below) |
| `IID_ID3D12ConsDmaCommandList` value | **proven** — exported by the console runtime at ordinal 100 |
| the other 9 console IIDs (`ID3D11DeviceX`, `ID3D11DeviceContextX`, `ID3D11DmaEngineContextX`, `ID3D11PerformanceDeviceX`, `ID3D11PerformanceContextX`, `ID3D11UserDefinedAnnotationX`, `ID3D11CounterSetX`, `ID3D11CounterSampleX`, `IGraphicsUnknown`) | **proven** — exported at known ordinals |
| IIDs of `ID3D12ConsComputeCommandList` / `…6(Compute)` / `…8(Compute)` / `ID3D12ConsCopyCommandList6` | **NOT recovered** — not exported by the runtime. 7 candidate v4 UUIDs sit 16-byte-aligned immediately after `IID_ID3D12Device4` in the UMD's `.rdata`; they are recorded in `iid_provenance.json` as CANDIDATE, not used |
| **the order of the appended console methods** | **ASSUMED** — assumption #1, see §4 |
| the signatures of the console methods | **inferred** from UMD messages and the nearest PC method, marked per method |
| the full catalogue | `wrap/docs/console-extension-catalogue.tsv` (105 methods, 106 enums) |

### Class placement, corroborated by real call sites

The console SDK publishes a sample set (the user supplies it; it is not committed
here). Scanning its source for the appended methods gives the *receiver* of each call,
which is independent of the UMD's error strings:

| method | receiver seen in samples | therefore |
|---|---|---|
| `PresentX` | the command queue (367 sites) | belongs to `ID3D12CommandQueue` |
| `SuspendX`, `ResumeX` | the command queue (122 each) | `ID3D12CommandQueue` |
| `SetFrameIntervalX` | the device (122) | `ID3D12Device` |
| `ScheduleFrameEventX` | the device (122) | `ID3D12Device` |
| `WaitFrameEventX` | the device (121) | `ID3D12Device` |

Same scan corroborates the free function's shape: `D3D12ConsCreateDevice(nullptr,
&params, riid, out)` with a `D3D12CONS_CREATE_DEVICE_PARAMETERS`, 123 sites. It also
shows what titles use that samples do not: `D3D12ConfigureVirtualMemoryX` and
`D3D12UploadCustomMicrocodeX` appear in **no** sample, so nothing external documents
them and our emulation remains the only implementation.

## 4. The one dangerous assumption, and how it is defused

Method *order* in the appended block is not recovered. If it is wrong, a call
lands in the wrong function — normally the one failure mode that corrupts state
silently.

Every appended slot therefore **logs its own name and raw arguments**. If the
order is wrong the log shows a method name that does not match the call the title
made, which is a diagnosable symptom rather than silent corruption. Unknown slots
(`XSlot_Device`, `XSlot_CommandList`) log slot index plus all four register
arguments and return `E_NOTIMPL`, so a console method we have not placed yet is
*recorded* instead of landing in an unrelated PC method.

The same principle applies to IIDs: unknown `QueryInterface` calls are logged
with their full GUID, and when the PC object can serve one the wrapper says so
explicitly, because that means interception is lost for that interface. That log
is exactly how the four missing console IIDs will be identified from the title.

## 5. Verification

### 5.1 wrapper logic, no GPU needed

`wrap/build/wrapper_selftest.exe` exercises QI routing, refcounting, validation
and unknown-slot logging with a null PC object:

```
$ wine wrapper_selftest.exe
wrapper self-test: 19 checks, 0 failures
RESULT: PASS (0 failures)
```

The trace shows correct routing: every PC device IID is served by the wrapper,
the device refuses the DMA *command-list* IID (`0x80004002` = `E_NOINTERFACE`),
and the DMA list serves it.

### 5.2 end-to-end through bridge + wrapper + real PC D3D12

`bridge/build/integration_test.exe` — console path, real runtime, run under Wine:

```
== integration test ==
ok   LoadLibrary(d3d12_xs.dll)
ok   D3D12CreateDevice -> 0x00000000 dev=0000002BD2D6ABB0
ok   device identity preserved across QI (wrapper served it)
ok   CreateCommandAllocator(COPY) -> 0x00000000
ok   CreateCommandList(type=CONSOLE_DMA(6), riid=IID_ID3D12ConsDmaCommandList) -> 0x00000000
ok   returned object serves IID_ID3D12ConsDmaCommandList -> 0000002BD2D6AC70
ok   same object also serves IID_ID3D12GraphicsCommandList -> 0000002BD2D6AC70
ok   command list GetDevice returned the wrapper device
== done ==
```

and the wrapper's log, showing the console semantics doing real work:

```
CreateCommandList(nodeMask=0, type=6 [CONSOLE DMA], alloc=..., riid=30bc77b9-8379) -> DMA wrapper
FillRawBuffer: SizeBytes 6 is not DWORD aligned -> rejected (matches UMD)
FillRawBuffer(res=..., off=0, size=8, value=0x0) -> E_NOTIMPL (needs compute-shader emulation)
OMSetFrontAndBackStencilRef(front=1, back=2) -> front forwarded, back recorded
```

So a console-only command-list type plus a console-only IID round-trips through
the whole stack, and the object that comes back serves both the console IID and
the standard PC interfaces.

### 5.3 the bug this test caught

The first run crashed with an execute-access violation. Cause: `scarab_wrap_device`
blind-cast the PC device to `ID3D12Device4`. A device created as `ID3D12Device`
may be exactly that, so treating it as `ID3D12Device4` puts methods past the end
of its vtable and any call to one jumps into whatever bytes follow.

Fixed by querying the highest version the PC device actually supports and
**refusing to wrap** below `ID3D12Device4` — a clean, logged failure instead of a
crash. The command-list path had the same hazard and avoids it correctly by
construction: it *requests* `IID_ID3D12GraphicsCommandList6`, so a success means
the object really does implement it.

## 6. Implemented console methods, and where each one honestly stands

| method | implementation |
|---|---|
| `OMSetFrontAndBackStencilRef(front, back)` | **real** — front forwarded to `OMSetStencilRef`, back recorded (PC has only a front reference) |
| `RSSetDepthBiasX` | recorded only — D3D12 moved depth bias into the PSO, so there is no command-list state to set |
| `OMSetSampleMaskX` | recorded only — PC sample mask is a PSO field |
| `FillRawBuffer` | **UMD rule enforced** (DWORD alignment), then `E_NOTIMPL` — no PC pattern-fill exists |
| `FillMemoryWithPattern` | `E_NOTIMPL` — needs a compute-shader or staging-buffer emulation |
| `CopyResource` (DMA) | **real** — falls through to the generated forward, which is correct for a DMA engine copy |
| `CreateCommandList` | **real** — maps the console DMA type onto the PC copy engine and returns the right wrapper |
| `GetFrameStatisticsX` | zeroed |
| `ScheduleFrameEventX`, `SetFrameIntervalX` | recorded |
| `SetDebugErrorFilterX` | mirrors the UMD's own validation, including the rejection |
| `CreateCommandQueueX` | `E_NOTIMPL` — PC has no shader-based command queue |
| `XSlot_Device`, `XSlot_CommandList` | log slot index + raw arguments, `E_NOTIMPL` |

Nothing here pretends to work. Methods with no PC equivalent fail loudly and say
why; methods with an equivalent are real.

## 7. Build

No Windows SDK, no root, Linux host:

```
~/opt/llvm-mingw                       # LLVM-mingw 20261117, clang 23.1.6
cd $SCARAB_ARTIFACTS/wrap   && bash build.sh
cd $SCARAB_ARTIFACTS/bridge && bash build.sh
python3 tools/verify_exports.py bridge/build/d3d12_xs.dll      # 116/116, still PASS
```

Build gotchas worth knowing:

- **`__uuidof` does not work** for these MIDL interfaces under llvm-mingw/clang
  (it resolves to an undefined `__mingw_uuidof<T>()` template). Use the `IID_…`
  constants — the wrapper and the integration test both do. This is also why the
  generator emits `IID_ID3D12GraphicsCommandList6` rather than `__uuidof(...)`.
- **`IID_PPV_ARGS` has the same problem.** Pass the IID explicitly.
- `d3d12.h` declares some methods **twice**, an MSVC-only form guarded by
  `#if defined(_MSC_VER) || !defined(_WIN32)` and a by-value form in the `#else`
  (23 occurrences). The generator resolves those guards the way the compiler
  would, otherwise it emits two conflicting declarations of the same method.
- One translation unit must define the IID GUIDs (`src/guid_defs.cpp`, `INITGUID`).

## 8. Extending it

The wrapper is set up so that learning more makes it more complete, mechanically:

1. Place a recovered method: add it to the right struct in
   `wrap/console_extensions.h`, regenerate, rebuild. The `override` check will
   reject a wrong signature.
2. Confirm an order: run a title with the QI audit on
   (`scarab_wrapper_set_qi_audit(1)`) and read the slot log. The indices the log
   reports *are* the order.
3. Confirm an IID: same log. Any console IID we do not know appears there.
4. Implement a method: give it real semantics and drop it from the log-only set.

The remaining unknown is dominated by two things that need the title exe:
the true order of the appended block, and the signatures of the ~95 console
methods not yet placed. Everything else is machinery that is already in place.
