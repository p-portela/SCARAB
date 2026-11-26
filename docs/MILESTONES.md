# Milestones

Versions are **milestones, not checkpoints**. A version tag is placed only when every
exit criterion of that milestone is met and evidenced; work between milestones carries no
tag. (Earlier in the project the tag moved on each notable commit — v0.1.0 "initial
repository", v0.2.0 "rename", v0.3.0 "bytecode pinned". Those three are pre-milestone
development tags and do not map to the ladder below; see the note at the end.)

Rules for a version tag:

1. Every exit criterion is met, and each is backed by a run that can be repeated from a
   clean clone (`docs/BUILDING.md`), not by a claim.
2. The gates that cover the milestone's area are green on **both** hosts where they apply
   — Windows natively (primary) and Linux (secondary).
3. The tag is annotated, and the tag message names the criteria it closes.
4. Nothing in a milestone is skipped to reach the next one. A milestone that regresses an
   earlier one is not released.

Status vocabulary: **met** (evidenced), **partial**, **not met**.

---

## v0.1.0 — Structural

What it means: the project exists as a real, buildable, contract-checked layer, with the
architecture settled and the ABI proven. No title runs yet.

Exit criteria

| # | criterion | status |
|---|---|---|
| 1 | `ctest` green on Linux (secondary host) | **met** — 6/6 (G1 G3 G4 G5 G6 G7) |
| 2 | `ctest` green on Windows natively, MSVC path compiled | **not met** — the MSVC branch has never been compiled; no Windows host has run it |
| 3 | export contract byte-exact against the supplied runtime | **met** — G1: 116/116 names + ordinals, 93/93 GUIDs |
| 4 | wrapper vtable layout proven by the compiler, not asserted | **met** — G2 (53 + 76 signatures via `override`) |
| 5 | backend identity correct on both hosts (the Linux path really is the D3D12 → Vulkan bridge) | **met** — G7 both directions |
| 6 | the repo builds from a fresh clone with only the documented artifact dir | **met** — clean-clone run, 6/6 |
| 7 | no third-party binaries, no game content, no vendor export table committed | **met** — no vendor blob is tracked; the `.def` is generated |

Blocked only by #2, which needs a Windows machine.

## v0.2.0 — A title's device bring-up runs

What it means: the smallest real bring-up path — create device, create queue, create
swapchain, present — works *through our layer*, unmodified caller code, no unimplemented
slot reached.

Exit criteria

1. The minimal bring-up path from the vendor's public sample set runs against our
   `d3d12_xs.dll` and presents ≥ 60 frames.
2. The bridge log shows the full forward path and **zero** `E_NOTIMPL` from a slot the
   path actually calls.
3. Frame-pacing calls the bring-up path makes (`SetFrameIntervalX`, `ScheduleFrameEventX`,
   `WaitFrameEventX`, `PresentX`) are recorded rather than refused, with their real
   arguments visible in the log.
4. G4 extended: the console DMA command list is created *and* used to execute a no-op
   workload.
5. Same run on Linux through Wine/vkd3d-proton, classified correctly by G7.

## v0.3.0 — Extension surface pinned

What it means: the appended console block is no longer a guess.

Exit criteria

1. Appended-method **order** confirmed from a live title's QI audit (`scarab_wrapper_set_qi_audit(1)`),
   not assumed. This is the project's single most dangerous assumption.
2. All recovered methods placed on the right class, with signatures moved from
   *inferred* to *confirmed* against the vendor's own interface headers.
3. QI for every console IID either served from a proven GUID or explicitly refused, with
   the unknown-slot log used to close the remaining gaps.
4. Class-placement table (see `docs/d3d12-xs-wrapper.md`) complete for every method the
   bring-up path calls.

Depends on a live title; deliberately last among the "runs" milestones.

## v0.4.0 — Shader path end to end

What it means: console shader blobs become something a PC runtime accepts.

Exit criteria

1. The vendor disassembler is callable and used as an **oracle**: its output is ground
   truth for instruction boundaries.
2. Decode sweep over the full supplied corpus: 100% of streams decode with no alignment
   failure (today: 0 clean / 80 dirty on the sample).
3. FSSM → RDNA2 → **DXIL** end to end: a real blob in, a valid `DXBC`/`DXIL` container
   out, verified part-by-part (extend G6 from "emitter works" to "our IR round-trips").
4. `D3D12UploadCustomMicrocodeX` wired to that chain, with the console microcode path
   exercised by a test.

## v0.5.0 — Presentation and memory

What it means: a title can allocate, present, and schedule frames on PC hardware.

Exit criteria

1. Presentation equivalents (`PresentX` / plane parameters / frame notification)
   implemented against PC swapchain behaviour, with the delta documented.
2. Memory-adapter seam handles the console memory model: swizzle modes, compression
   flags, and virtual-memory configuration either honoured or explicitly refused.
3. A frame-scheduling policy exists (what our layer does with the console's frame model
   when the PC path has none) and is documented as a deliberate decision.
4. HDR/refresh-rate paths: at least one presentation variant other than 60 Hz default.

Status (2026-11-24): **partial** — criterion 2 is met for the part this repo can
honestly do. The memory-adapter (`components/memory-adapter/`) implements: the
console swizzle policy (`XGSuggestSwizzleMode`,
`XGComputeOptimalDepthStencilSwizzleMode`, `scarab_optimal_swizzle_mode`,
`scarab_validate_depth_stencil_swizzle_mode`), flag translation with honest
report-only semantics, and the xmem.dll shim for the exe's 12 imports. Every
one of these is verified against the vendor `xg_xs.dll` itself under Wine
(gate G9, `tests/xg_oracle_test.cpp`) — the oracle pins the descriptor mapping
and the swizzle-mode chain, which is what makes the shim honest. Criteria 1,
3 and 4 (presentation) are **not met**: the present-adapter is a seam and the
exe does not import the DXGIX exports the bridge documents.

## v0.5.1 — The texture layout surface is oracle-verified

What it means: every internal shape decision the title makes for textures is
verified against the console library, not just read off the decompiler.

Exit criteria

1. `XGComputeTileShape` ported for the fast paths (linear, 3-D-fast, and the
   refusals) and each decision compared against the vendor in the oracle
   (gate G9).
2. `XGSuggestSwizzleMode` and `XGComputeOptimalSwizzleMode` oracle-verified
   (already done, kept as the regression gate).
3. The table-dependent paths (`param_2 != 0`) documented as explicitly refused,
   not silently passed through.

Status: **met** — `XGComputeTileShape` is ported and oracle-verified against
the vendor (gate G9, all fast paths + the refusal kinds); the descriptor-
dependent path is documented as refused, not passed through.

## v0.6.0 — XGCreateTextureComputer

What it means: the texture factory and its layout object.

Exit criteria

1. The factory skeleton (heap-allocated object, `+0xc` usage marker, `+0x32dc`
   descriptor marker) layouted per the decompiled constructor
   (`FUN_1800080a0`).
2. The full `XG_RESOURCE_LAYOUT` struct (0x1B40) ported and oracle-verified —
   **blocked** on the descriptor/table semantics (see
   `docs/xg_xs-decompiled.md`): the 2D constructor's internal layout math
   depends on tables that cannot be recovered from the artifacts. No part of
   this milestone may be faked.
3. `XGComputeTileShape` table-dependent paths (`param_2 != 0`) ported and
   oracle-verified — **blocked** on the same tables.

Status: **not met** — documented as a blocked milestone, not faked.

## v0.7.0 — The title's entry point runs through the layer

What it means: the layer owns the title's process from its entry point into its own
System Init, so every remaining blocker is a named missing shim rather than a mystery.

Exit criteria

| # | criterion | status |
|---|---|---|
| 1 | The loader maps the title's own PE (rebased copy, imports zeroed, TLS callbacks cleared) and runs its entry point | **met** — G10: 68 modules, 587 imports, 493 bound, 94 stubbed (named); the CRT init, System Init and graphics DLL loads happen |
| 2 | The title's static TLS and thread creation see its own block | **met** — the loader points TLS index 0 at the title's block in the entry thread and in every thread it starts through `CreateThread` (an EXE loaded as a DLL would otherwise read the launcher's block) |
| 3 | The title's GDK bootstrap runs against our `xgameruntime` shim | **met** — `InitializeApiImplEx2(gdk=633600002712, os=65f400001edd, flags=2)` is called; families the shim does not implement are refused by GUID (XASYNC, XSYSTEM, XUSER, XGAME, XNETWORKING) |
| 4 | A fault is attributable to a named API | **met** — the loader prints a fault report before the host handles it (code, module+RVA, registers, stack scan); the first fatal one is `GameInput.dll` ordinal 1 → before the shim: Wine's stub `GameInputCreate` returned `E_NOTIMPL` and the title `js`ed into an `int3`. After the phantom-pad shim, the title runs past that trap and the next fatal is `XCRDAPI.dll!DStorageGetFactory` (see #6) |
| 5 | Windows-native gate | **not met** — same host gap as v0.1.0 #2 |
| 6 | Known-unimplemented surface enumerated with its failure mode | **partial** — GameInput shipped (one always-connected phantom pad; readings and raw devices refused loudly); XCRDAPI `DStorageGetFactory` and the content/storage pipeline (gameos.xvd, content RPF) are next fatal; GDK families refused in boot (the title's own runtime layer reports the missing dependencies); chad.dll, xapu.dll, AcpHal.dll, gameos.xvd unshipped |

Status: **partial** — Linux-host path evidenced; the Windows-native gate and the
GameInput/GDK surfaces are open. No tag is placed: the ladder's rule is that a tag
marks a milestone whose criteria are all met, and this one is not.

The evidence run: `scarab_launch --deps <memory-adapter> --deps <game-runtime> --deps <tests> <title>`
under Wine, with `T:` mapped to a scratch directory so the title's own `.crash.log`
and minidump are readable. The loader is an honest runner: what it binds, it prints;
what it refuses, it names.

## v1.0.0 — A retail title boots and renders

What it means: the layer's reason to exist, demonstrated.

Exit criteria

1. One retail console title reaches gameplay frames on **Windows** (primary target).
2. The same title reaches the same point on **Linux** via Wine/vkd3d-proton (secondary),
   with the differences documented.
3. Per-title bring-up notes exist for at least one title, enough that a second title is a
   repeatable process rather than a new investigation.
4. Every gate green on both hosts, from a clean clone.
5. Known-unimplemented surface enumerated with its failure mode documented, so nothing
   fails silently.

---

## Where the current tree sits

Between the pre-milestone tags and **v0.1.0**: six of seven criteria met. The outstanding
one is the Windows-native build (#2), which needs a Windows host — the same host the
sample set's bring-up work will need for v0.2.0. Separately, **v0.7.0** (the loader's
entry point) is in partial: the title runs to System Init with the known fatal surface
named (GameInput, GDK families). Nothing tagged is placed for it yet.

## Existing tags

`v0.1.0`, `v0.2.0`, `v0.3.0` were placed before this ladder existed and each marks a
commit, not a milestone. They are kept for history; the ladder above governs from here.
The first tag placed under these rules will be the next milestone whose criteria are met —
by the table above, that is **v0.1.0** (re-pointed) once the Windows-native build runs, and
the pre-milestone tags can then be retired or kept, but they must not be read as milestones.
