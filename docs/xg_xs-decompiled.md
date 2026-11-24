# `xg_xs.dll` decompiled — the texture layout library

`xg_xs.dll` (547,192 bytes, 27 exports, image base `0x180000000`) is the console's
texture layout library: it decides the swizzle mode and tile shape of a surface
(the title's own texture placement comes through it — the exe calls
`XGCreateTextureComputer` 4× and `XGSuggestSwizzleMode` 2×). This doc records
what the decompilation establishes, organized by export. It is a findings
document, not an implementation; where the decompiled C does not pin the
semantics, the ambiguity is named rather than guessed (this repo's house rule).

Everything below comes from `artifacts/vendor/x64_dx12/xg_xs.dll`, decompiled
with Ghidra headless at the export's own entry (no auto-analysis). Ghidra's
decompiled C is pseudocode, not proof; the values it returns are the console's
own constants, and the chain is what it is.

**Status:** `XGSuggestSwizzleMode`, `XGComputeOptimalDepthStencilSwizzleMode`,
`scarab_optimal_swizzle_mode` (the optimal-swizzle chain), and
`scarab_validate_depth_stencil_swizzle_mode` are implemented in
`components/memory-adapter/src/memory_adapter.cpp` and verified against the
vendor library in `tests/xg_oracle_test.cpp` (gate G9). The texture-factory,
DCC, tile-shape, and table-semantics items below are documented, not ported.

## Export list (27)

| # | export | VA | role |
|---|--------|----|------|
| 1 | XGComputeBufferLayout | 0x180002680 | layout math (buffer) |
| 2 | XGComputeOptimalDepthStencilSwizzleMode | 0x1800027a0 | depth/stencil swizzle |
| 3 | **XGComputeOptimalSwizzleMode** | 0x1800027c0 | swizzle policy (below) |
| 4 | XGComputeTexture1DLayout | 0x1800028a0 | layout math (1D) |
| 5 | XGComputeTexture2DLayout | 0x1800029b0 | 2D layout = factory + call (below) |
| 6 | XGComputeTexture3DLayout | 0x180002a30 | layout math (3D) |
| 7 | XGComputeTileShape | 0x18000bba0 | tile shape (below) |
| 8 | XGCreateBVHComputer2 | 0x1800062e0 | BVH computer factory |
| 9 | XGCreateBVHComputer | 0x180006360 | BVH computer factory |
| 10 | XGCreateTexture1DComputer | 0x18000abc0 | 1D computer factory |
| 11 | XGCreateTexture2DComputer | 0x18000acc0 | 2D computer factory |
| 12 | XGCreateTexture3DComputer | 0x18000adc0 | 3D computer factory |
| 13 | XGCreateTextureComputer | 0x18000aec0 | generic computer factory |
| 14 | XGCreateTextureComputerFromDescriptor | 0x18000aed0 | same, from raw descriptor |
| 15 | XGDecodeDCC | 0x18000c1d0 | DCC decode (below) |
| 16 | XGDecodeDCCSurface | 0x18000c4b0 | DCC surface decode |
| 17 | XGEnableExclusiveLocking | 0x18000b540 | exclusive lock |
| 18 | XGEncodeDCC | 0x18000c980 | DCC encode (below) |
| 19 | XGEnterExclusiveLock | 0x18000b5c0 | exclusive lock |
| 20 | XGInitializeLibrary | 0x18000b5f0 | init |
| 21 | XGLeaveExclusiveLock | 0x18000b630 | exclusive lock |
| 22 | XGSetHardwareVersion | 0x18000b650 | hardware version setting |
| 23 | XGShuffleTextureBufferForDirectStorage | 0x18000d3c0 | Direct Storage shuffle |
| 24 | **XGSuggestSwizzleMode** | 0x180002be0 | swizzle policy (below) |
| 25 | XGTerminateLibrary | 0x18000b680 | teardown |
| 26 | XGValidateDepthStencilSwizzleMode | 0x180002d60 | depth/stencil swizzle validation (below) |
| 27 | XGValidateDescriptor | 0x18000cfa0 | descriptor validation |

## XGComputeOptimalSwizzleMode (0x1800027c0)

```c
// param_1 texture kind; param_7 flags (bit 0x40 = depth/stencil);
// param_8 flags (bit 0x8000000 = raw-bits); param_7 & 0x20 = explicit 0x1b;
// param_7 & 0x80 = force the small-surface check; param_2 = shape descriptor
// (0x28-byte table entries, byte 0 = multiplier); param_3/4 = w/h.
uint32_t XGComputeOptimalSwizzleMode(int param_1, int param_2, uint32_t param_3,
                                     uint32_t param_4, uint64_t param_5,
                                     int param_6, uint32_t param_7, uint32_t param_8)
{
    if (param_1 == 1) return 0;
    if (param_7 & 0x40) return 0x18;          // 24 — depth/stencil 64KB
    if (param_1 != 2) {
        if (param_1 == 3) {
            if ((param_8 & 0x8000000) == 0) {
                if ((param_7 & 0x20) == 0) {
                    if ((param_7 & 0x80) == 0) return 5;   // 4KB
                    if (param_6 == 1 && table_nonzero(param_2) &&
                        (table_byte(param_2) * (uint64_t)param_3 *
                         (uint64_t)param_4 & 0xfffffffffffffff8) < 0x80001)
                        return 0x15;                          // 21
                }
                return 0x1b;                                // 27 — 64KB_R_X
            }
        } else {
            if (param_1 != 4) return 0;
            if ((param_8 & 0x8000000) == 0) {
                if ((param_7 & 0x20) != 0) return 0x1b;      // 27
                return 5;                                    // 4KB
            }
        }
        return 10;                                           // 2D 64KB_D
    }
    return 0;
}
```

The chain: `1` → 0 (linear); `2`, and anything unresolved → 0; `3`/`4` surface
kinds go through depth-flag → 24, else `param_7 & 0x20` → 27, else 5/0x15/27;
the `0x8000000` bit on `param_8` flips `3`/`4` surfaces to 10 (2D 64KB_D).

**Inferred, not proven**: the meaning of each positional argument is not pinned
by the decompiled C alone (the exe calls the factory, not this function,
directly). The implementation in `components/memory-adapter/src/memory_adapter.cpp`
ports this chain verbatim with the names documented there.

## XGSuggestSwizzleMode (0x180002be0)

Takes a **descriptor** (`int*`), a flag, and an out `int*` mode. Returns 0 on
success; `0x80070057` on failure (`desc` or `out` null, or the kind is none of
1/2/3/4):

```c
// desc[0] = texture kind (uint32: 1 = linear, 2/3/4 = surfaces);
// desc[0xc] = flags word (uint32, at byte offset 0x30):
//   bit 1 = render-target; bit 0x400000 && (bit 4 | bit 1): 0x11b wide surface
uVar1 = desc[0xc];
if (!((uVar1 & 1) && (uVar1 & 2)) && (what & 3 != 3)) {
    kind = *desc;
    if (kind == 1) { *out = 0x100; return 0; }      // linear
    if ((uVar1 & 2) == 0) {                         // not render-target
        if ((uVar1 & 0x400000) && ((uVar1 & 4) || (uVar1 & 1))) {
            *out = 0x11b; return 0;
        }
        if (kind == 2) uVar3 = 2;
        else if (kind == 3) uVar3 = 3;
        else if (kind != 4) return 0x80070057;
        else uVar3 = 4;
        iVar2 = func_0x00018000dbd0(uVar3, desc[8], desc[4], desc[6]);
        if (iVar2 < 0x20) *out = iVar2 + 0x100;      // raw modes 0x100+
        return 0;
    }
    *out = 0x118;
    return 0;
}
return 0x80070057;
```

The out values are the raw descriptor modes observed in the exe's caller:
`0x100` (linear), `0x102` (2D 64KB_D?), `0x118`, `0x11b`, and `0x100 + <mode>`
for the 3/4-kind surfaces. The exact `what` semantics are unverified — treat as
opaque.

The helper `func_0x00018000dbd0` is the same swizzle-policy body as above
(`param_1 == 2` → 0; `3`/`4`/`5` → 10/5/0x1b/0x15 by the same flags; plus
`param_10 != 0` → 5).

## XGValidateDepthStencilSwizzleMode (0x180002d60)

Fully recovered — the 7 depth/stencil modes the library accepts:

```c
switch (mode) {
case 4: case 8: case 0xc: case 0x10: case 0x14: case 0x18: case 0x1c:
    return 0;
default:
    return 0x80004005;
}
```

## XGComputeOptimalDepthStencilSwizzleMode (0x1800027a0)

Stack-only: writes constant `0x18` (24) — the depth/stencil 64KB mode.

## XGComputeTileShape (0x18000bba0)

Shape-only read of the fast path:

- type input (`param_1`): 1 = linear (`uVar5 = 0; param_3 = 1`), 2/3/4 —
  `param_1 == 2` → 0x80004001 (refused); `param_1 == 3` — surface fast path:
  returns 0x80004005 for tile sizes outside 1/2/4/8/16 (with a table lookup
  when the descriptor is set), else tile width/height by the branchy
  arithmetic above; anything else → 0x80070057.
- tile arithmetic: `uVar6`/`uVar8` (tile width / height-or-count) from tile
  size: 1→0x100/0x100, 2→0x100/0x80, 4→0x80/0x80, 8→0x80/0x40, 16→0x40/0x40;
  pixel-per-side 2/4/8 halves it.
- with a shape descriptor (`param_2 != 0`), the shape comes from two table
  levels (`UNK_18005539c` stride `0x3c` uints, then `UNK_18004e0dd` and friends
  stride `0x28`): `param_5[2]=1; param_5[1]=byte[e0e0]*iVar7;
  param_5[0]=(byte[e0df]/byte[e0de])*uVar6`.
- 3D/1D input (`param_1 != 3`): refused (0x80004001 / 0x80004005), with the
  linear fall-through for the 64 KiB case.

Not ported: table contents, the 3D/pixel-per-side semantics, and the
`param_6`-output semantics are unverified.

## XGComputeTexture2DLayout (0x1800029b0)

Exit path that wraps the factory:

```c
if (param_1 != 0 && param_2 != 0) {
    uStack_10 = 0;
    iVar2 = XGCreateTexture2DComputer(param_1, &uStack_10);
    uVar1 = uStack_10;
    if (iVar2 < 0) {
        (*(code *)PTR__guard_dispatch_icall_18004ff00)(uStack_10, param_2);
        (*(code *)PTR__guard_dispatch_icall_18004ff00)(uVar1);
        return iVar2;
    }
    return -0x7ff8ffa9;
}
```

`iVar2 < 0` appears to be a success code (the `-0x7ff8ffa9` result is the E_FAIL
branch). The descriptor-to-computer conversion, its object model, and the exact
layout struct are **not** recovered; the layer reports E_NOTIMPL for this path.

## DCC (XGEncodeDCC / XGDecodeDCC / XGDecodeDCCSurface)

Not ported. The decompiled C is dominated by stack-mangled variable plumbing
and opaque dispatch stubs (`FUN_18000be80`, `PTR__guard_dispatch_icall_*`). The
library returns E_FAIL through an error path (`*out = 0xfffffffffffffff8`
sentinel) rather than reporting success. The compression itself is GPU-native
on PC for equivalent formats; forcing a decompression here is the wrong
answer. Scarab reports E_NOTIMPL for the DCC path rather than guessing.

## The exe's texture path (call-site evidence)

`game_scarlett_master.exe` calls `XGCreateTextureComputer` 4× and
`XGSuggestSwizzleMode` 2× (console-call-sites-pdata.json, pdata-snapped):

- `XGCreateTextureComputer` at 0x140344235 (fn 0x1403441f0), 0x140979c75
  (0x140979c40), 0x1416f7141 (0x1416f7100), 0x145121a5e (0x145121980)
- `XGSuggestSwizzleMode` at 0x140c4b3ce (0x140c4b030), 0x144eccca2 (0x144ecc940)

From the 2D caller (objdump of the pdata-snapped function 0x140c4b030):

- the exe reads the descriptor at `rdx`: width/height as u16s at `+0x02` and
  `+0x04`; flag bytes at `+0x06`/`+0x0b`/`+0x0c`/`+0x0d` (`movzwl 0x2(%rdx)`,
  `movzwl 0x4(%rdx)`, `movzbl 0xb(%rdx)`, `mov 0xc(%rdx),%r8b`)
- it derives a 32-bit mode word from those bytes and writes it into the
  descriptor (`0x20(%rcx)` / `0x2c(%rcx)`, e.g. `0x100..0x102` / `0x11b`)
- the call `XGCreateTextureComputer` passes the descriptor + a pointer to an
  opaque computer object

**Inferred, not proven**: the exact meaning of the `0x06`/`0x0b`/`0x0c`/`0x0d`
bytes and which `0x20(%rcx)`/`0x2c(%rcx)` field is a swizzle mode or a tile
count is not pinned by a single call site — a second call site says the same
thing, which is not the same as one being native. The layer must not trust
these bytes as a spec; they are evidence that the descriptor layout is roughly
`+0x00 kind / +0x02 width / +0x04 height / +0x06-0x0d flags / +0x20-0x2c
mode`.

## Known unresolved

- Table entry semantics (`UNK_18004bbc0` / `UNK_18005539c` / `UNK_18004e0dd`)
  are only half-recovered. The DLL's own answer is the only oracle; the port
  in `components/memory-adapter/src/memory_adapter.cpp` deliberately takes the
  "not small" path and documents the `small_mult` parameter as 0.
- The computer-object object model (`AddRef`/`Release`/`Get*`) is not recovered.
- DCC encode/decode is deliberately not ported.
- `XGComputeTileShape` and friends are documented shape-only, not ported.
- The `XGCreateTextureComputer` descriptor itself is only partially read; the
  factory is not ported.
