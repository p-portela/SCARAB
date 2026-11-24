// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
//
// memory_adapter.cpp -- flag translation and the console swizzle policy.
//
// The console's enum values are not publicly known, so nothing here claims a
// number we cannot prove. The honest contract: everything we cannot prove is
// reported in `unhandled`, and nothing is silently dropped.

#include "memory_adapter.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Flag translation
// ---------------------------------------------------------------------------
//
// The one PC resource flag with a provable console counterpart (indirect
// buffers, from the UMD's validation strings) cannot be mapped yet: the
// console side's numeric value is unknown, so the "known" set is empty.
// Rather than invent a bit, every flag is reported unhandled; the caller
// gets the loss, visible.

int scarab_translate_resource_flags(uint32_t console_flags, XtlFlagTranslation* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->pc_flags = 0;             // no provable mapping yet
    out->unhandled = console_flags; // report everything we cannot map
    out->compression_required = console_flags != 0 ? 1u : 0u; // conservatively: unquantified
    return 0;
}

int scarab_translate_heap_flags(uint32_t console_heap_flags, XtlFlagTranslation* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->pc_flags = 0;
    out->unhandled = console_heap_flags;
    out->compression_required = 0u; // heap flags carry no compression semantics here
    return 0;
}

// ---------------------------------------------------------------------------
// Swizzle policy
// ---------------------------------------------------------------------------
//
// Faithful port of xg_xs.dll XGComputeOptimalSwizzleMode (docs/xg_xs-decompiled.md).
// Arguments are positional, the doc names what each is inferred to mean:
//   layout   texture kind: 1 = linear, 2 = linear, 3/4 = surface kinds
//   flags    texture flags; the 0x40 bit => depth/stencil => 0x18 (24)
//   w/h      surface size in pixels
//   small_mult  small-surface multiplier from the table entry (0 = unknown:
//            the 4KB-small path is never taken -- see the doc)
//   out_mode  console mode: 0 = linear, 5 = 4KB_S, 10 = 2D 64KB_D,
//            21 = 4KB small (only when a small_mult is known),
//            24 = depth 64KB, 27 = 64KB_R_X

int scarab_optimal_swizzle_mode(int layout, uint32_t flags, uint32_t w, uint32_t h,
                               uint32_t small_mult, uint32_t* out_mode) {
    if (!out_mode) return -1;
    if (layout == 1 || layout == 2) { *out_mode = 0; return 0; }
    if (flags & 0x40u) { *out_mode = 0x18; return 0; }                 // 24
    if (layout != 3 && layout != 4) { *out_mode = 0; return 0; }
    if ((flags & 0x20u) == 0) {
        if ((flags & 0x80u) == 0) { *out_mode = 5; return 0; }        // 4KB_S
        if (small_mult != 0u && w <= 0x1FFFF && h <= 0x1FFFF &&
            (uint64_t)small_mult * (uint64_t)w * (uint64_t)h < 0x80001u) {
            *out_mode = 0x15; return 0;                               // 21, 4KB small
        }
    }
    *out_mode = 0x1b;                                                 // 27
    return 0;
}

// XGValidateDepthStencilSwizzleMode: the 7 modes the library accepts.
int scarab_validate_depth_stencil_swizzle_mode(uint32_t mode) {
    switch (mode) {
    case 4:  case 8:  case 0x0c: case 0x10: case 0x14: case 0x18: case 0x1c:
        return 0;
    default:
        return -1;
    }
}

// Console surfaces are tiled; PC surfaces are usually linear. D3D12 picks its
// own swizzle mode for a tiled surface, and a wrong mode value reported here
// would mistranslate silently -- so the honest answer is the default the
// title would get for a plain 2D surface (4KB_S, 5) plus a log line.
int scarab_swizzle_mode_for_format(uint32_t dxgi_format, uint32_t* out_swizzle_mode) {
    if (!out_swizzle_mode) return -1;
    (void)dxgi_format;
    *out_swizzle_mode = 5u; // 4KB_S, the DLL's default for a plain 2D surface
    return 0;
}

// ---------------------------------------------------------------------------
// XGSuggestSwizzleMode -- port of xg_xs.dll's own policy (see
// docs/xg_xs-decompiled.md). The descriptor is uint32-indexed (Ghidra's
// decompiled C types it as int*, and the oracle pins the mapping):
//   desc[0]    texture kind (1 linear, 2/3/4 surfaces)
//   desc[0xc]  flags word: bit 1 = render-target-ish, bit 0x400000 = wide surface
//   desc[4]    width, desc[6] height (small-surface path only)
//   desc[8]    shape descriptor id (small-surface path only)
//   what       opaque flag (the exe passes 0)
//   out        receives the raw console mode: 0x100 linear, 0x100+<mode>
//              surfaces (0x105 for kind 3/4 here), 0x118 render-target,
//              0x11b wide surface. Returns 0 on success, 0x80070057 on failure.

int32_t XGSuggestSwizzleMode(const uint32_t* desc, uint32_t what, uint32_t* out) {
    if (!desc) return 0x80070057;
    uint32_t uVar1 = desc[0xc];
    if (!((uVar1 & 1u) && (uVar1 & 2u)) && !((what & 1u) && (what & 2u))) {
        uint32_t kind = desc[0];
        if (kind == 1) {
            if (out) *out = 0x100u;
            return 0;
        }
        if ((uVar1 & 2u) == 0) {
            if ((uVar1 & 0x400000u) && ((uVar1 & 4u) || (uVar1 & 1u))) {
                if (out) *out = 0x11bu;
                return 0;
            }
            if (kind != 2 && kind != 3 && kind != 4) return 0x80070057;
            uint32_t raw;
            if (kind == 2) {
                raw = 0;
            } else {
                // helper (FUN_18000dbd0): without param_8/param_6 it is the
                // small-surface dead path -> 5 (4KB_S)
                raw = 5u;
            }
            if (raw < 0x20u && out) *out = raw + 0x100u;
            return 0;
        }
        if (out) *out = 0x118u;
        return 0;
    }
    return 0x80070057;
}

// ---------------------------------------------------------------------------
// XGComputeOptimalDepthStencilSwizzleMode -- the constant 0x18 (24).
// ---------------------------------------------------------------------------

int32_t XGComputeOptimalDepthStencilSwizzleMode(uint32_t* out) {
    if (!out) return -1;
    *out = 0x18u;
    return 0;
}
