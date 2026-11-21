# Contributing

## Before you change anything

Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §4 — the honesty rule. It is the one
convention in this repo that has no exceptions:

> Every call is either **real** (a correct PC equivalent exists and is used),
> **recorded** (no equivalent; the call and its arguments are stored so a later pass can
> honour it), or **refused** (log loudly, return `E_NOTIMPL`). Never silently succeed,
> never guess.

A silent no-op in a translation layer is indistinguishable from a translated call. If
you are tempted to return `S_OK` from something you have not implemented, don't.

The same applies to unknowns in code and docs: an explicit gap is worth more than a
plausible value. `components/memory-adapter/include/memory_adapter.h` deliberately
leaves the recovered console flags unnumbered for exactly this reason.

## Ground rules

1. **Never commit vendor binaries or game content.** `.gitignore` covers `vendor/` and
   `corpora/`; if you add a new artefact, add it there too and note it in
   [docs/PROVENANCE.md](docs/PROVENANCE.md).
2. **Never weaken a gate to make a change pass.** If G1 (export contract) fails, the
   bridge is not a drop-in replacement any more; fix the code, not the test.
3. **Evidence or it didn't happen.** A claim about the console surface needs a
   measurement behind it — a parsed export table, a string from a shipped binary, a
   reproducible command. Put the evidence in the commit message or the doc, not just the
   conclusion.
4. **Mark the grade.** When you implement one of the console methods, move it from
   recorded/refused to real in the same commit that makes it real, and update the table
   in `docs/d3d12-xs-wrapper.md` §6.

## Layout and ownership

| path | what belongs there |
|---|---|
| `components/runtime-bridge/` | the 116 exports. Header-free by design — do not add a D3D12 include. |
| `components/extension-wrapper/` | interfaces this project *owns*. Inherit the real ones; let the compiler check the layout. |
| `components/shader-bridge/` | FSSM/RDNA2 work and the the upstream shader recompiler integration |
| `components/{memory,present}-adapter/` | the unstarted seams |
| `tools/` | analysis tooling. Anything a `docs/` claim depends on belongs here. |
| `tests/` | the gates |

### The forwarding generator

`components/extension-wrapper/src/gen_{device,clist}.inc` are **generated** from
`vendor/d3d12/include/d3d12.h` by `tools/gen_wrappers.py` at build time. Never edit them
by hand and never commit them.

If you add an interface for the wrapper to own, put it in `console_extensions.h` and
make sure the generator covers it. The `override` keyword is the safety net: a wrong
signature will not compile.

Pitfalls already paid for, in case you hit them again:

- `__uuidof` and `IID_PPV_ARGS` **do not work** for these MIDL interfaces under
  llvm-mingw/clang (they resolve to an undefined `__mingw_uuidof<T>()`). Use the declared
  `IID_…` constants.
- `d3d12.h` declares 23 methods twice behind
  `#if defined(_MSC_VER) || !defined(_WIN32)`. The generator resolves those guards the
  way the compiler would; a generator change must keep
  `tests/test_tools.py::test_gen_wrappers_resolves_msvc_only_guards` green.
- Adding a vtable slot whose position is not confirmed is the one change that can corrupt
  state silently. If you cannot establish the order, use an `XSlot_*` logging slot.

## Workflow

```bash
./scripts/link-artifacts.sh     # once
./scripts/build.sh
./tests/run_tests.sh            # all gates must pass
```

Commit style: `type: summary`, where type is one of `feat`, `fix`, `docs`, `test`,
`build`, `chore`. Say what was measured, not just what changed:

```
fix: stop blind-casting the PC device to ID3D12Device4

A device created as ID3D12Device may be exactly that; treating it as
ID3D12Device4 puts methods past the end of its vtable, and the first call
to one jumped into garbage (execute-access violation under Wine).
Now queries the highest supported version and refuses to wrap below 4.
```

## Licence

This project is GPL-3.0-only (see `LICENSE`). By contributing you agree your
contribution is licensed under the same terms, and that you have the right to submit it.
Do not paste code from a source whose licence is incompatible with the GPL, and do not
commit third-party binaries or game content (see `docs/PROVENANCE.md`).

## Reporting a finding

If you recover something — a method's true order, an IID, a signature — record it where
it belongs rather than leaving it in a chat log:

| finding | where it goes |
|---|---|
| an IID | `components/extension-wrapper/src/iid_console.inc` if proven, `iid_provenance.json` if not |
| a method's class or order | `components/extension-wrapper/console_extensions.h` |
| a new name or symbol | regenerate `components/extension-wrapper/docs/console-extension-catalogue.tsv` |
| a shader-format fact | `docs/fssm-analysis.md` |
| an artefact's origin or hash | `docs/PROVENANCE.md` |
