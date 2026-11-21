# extension-wrapper — `d3d12_xs_wrap.dll`

Owns the console (Scarlett) D3D12 **extension** surface: the `X`-suffixed methods the
console appends to the PC interfaces, and the console-only command-list type and IID.

See [../../docs/d3d12-xs-wrapper.md](../../docs/d3d12-xs-wrapper.md) for the design, the
provenance of every console IID, the one dangerous assumption and how it is instrumented.

## Why it inherits the real interfaces

The classes derive from the genuine Agility SDK interfaces and mark every forwarded
method `override`, so the compiler proves the PC-side vtable layout — all 53 device and
76 command-list methods — instead of us promising it. The forwarding bodies themselves are
**generated** from the same header by `tools/gen_wrappers.py`, so they cannot drift
either.

> If you find a stale copy of `src/gen_device.inc` or `src/gen_clist.inc` anywhere, that is
> a bug: they are generated at build time into the build directory and are gitignored.
> A stale copy silently reintroduces exactly the drift the generator prevents.

## Files

| file | role |
|---|---|
| `console_extensions.h` | the recovered console interface surface, with provenance notes per method |
| `src/wrap.cpp` | the wrapper classes, `CreateCommandList` interception, QI routing, grading (§4 of ARCHITECTURE) |
| `src/iid_console.inc` | console IIDs **proven** to be exported by the runtime |
| `iid_provenance.json` | every IID this component knows about, graded: PROVEN / RECOVERED / CANDIDATE |
| `docs/console-extension-catalogue.tsv` | 105 recovered method names + 106 `D3D12CONS_*` symbols |
| `src/guid_defs.cpp` | `INITGUID` storage for the PC IIDs |

## The rule when editing this component

Never add a vtable slot whose position is unconfirmed. If you cannot establish the order,
use an `XSlot_*` logging slot instead — it records the index and all four register
arguments and returns `E_NOTIMPL`, which turns an unknown call into evidence rather than
silent corruption.
