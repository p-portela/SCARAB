# runtime-bridge — `d3d12_xs.dll`

A drop-in replacement for the console D3D12 runtime's 116 exports.

The console and PC runtimes define the same COM interfaces (verified: 28/28 shared
`IID_*` values byte-identical), so the standard D3D12 API is **forwarded** to the PC
runtime via `GetProcAddress` on `d3d12.dll`, and only the console-only free functions are
emulated. See [../../docs/d3d12-xs-bridge.md](../../docs/d3d12-xs-bridge.md) for the full
116-export disposition table.

## Why this component is header-free

It never owns a COM interface: it forwards free functions and resolves the PC runtime at
run time. Declaring its own ABI-exact prototypes means it needs no D3D12 headers and
cannot drift from the PC ABI at compile time. The component that *does* own interfaces is
`../extension-wrapper`, and that one deliberately includes the real header.

## The export contract is not a promise, it is a test

```
python3 tools/verify_exports.py build/.../d3d12_xs.dll
```

must report 116/116 names, 116/116 ordinals and 93/93 GUID bytes against the genuine
console DLL. That is gate G1 in `tests/run_tests.sh`. If it fails, this is no longer a
drop-in replacement, whatever else works.

## Optional extension layer

At `D3D12CreateDevice` / `D3D12ConsCreateDevice` the bridge looks for
`d3d12_xs_wrap.dll` next to itself and substitutes the returned device with the wrapper.
Without that DLL the bridge still serves the entire standard API and only the console
extension calls are lost — a logged, graceful degradation. Keeping the wrapper out of
the link keeps this DLL's export contract exactly what the console runtime published.

## Build

Driven by the top-level CMake; `./scripts/build.sh` from the repository root.
