// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ===========================================================================
//  memory_adapter.h -- console resource/heap semantics onto PC D3D12
// ===========================================================================
//  Flag translation + swizzle policy, implemented 2026-11-24. The console's
//  enum values are not publicly known, so every flag translation is by
//  index -- a wrong number would lie; an unknown one cannot. See
//  docs/xg_xs-decompiled.md for the evidence this rests on.

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Recovered console resource flags. Values unknown; names are evidence-backed.
typedef enum XtlconsoleResourceFlag {
    SCARAB_CONS_RES_ALLOW_DCC                          = 0,
    SCARAB_CONS_RES_FORCE_DCC_TEXTURE_COMPATIBILITY    = 0,
    SCARAB_CONS_RES_FORCE_FMASK_TEXTURE_COMPATIBILITY  = 0,
    SCARAB_CONS_RES_DENY_COLOR_COMPRESSION_DATA        = 0,
    SCARAB_CONS_RES_DENY_DEPTH_COMPRESSION_DATA        = 0,
    SCARAB_CONS_RES_DENY_DEPTH_COMPRESSION_EXPCLEAR    = 0,
    SCARAB_CONS_RES_DENY_DEPTH_COMPRESSION_PLANES      = 0,
    SCARAB_CONS_RES_DENY_DEPTH_STENCIL_HTILE           = 0,
    SCARAB_CONS_RES_DENY_STENCIL_COMPRESSION_DATA      = 0,
    SCARAB_CONS_RES_ALLOW_INDIRECT_BUFFER              = 0,
    SCARAB_CONS_RES_ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP   = 0,
} XtlconsoleResourceFlag;

// Recovered console heap flags.
typedef enum XtlconsoleHeapFlag {
    SCARAB_CONS_HEAP_ALLOW_INDIRECT_BUFFERS = 0,
} XtlconsoleHeapFlag;

typedef struct XtlFlagTranslation {
    uint32_t pc_flags;        // flags safe to pass to the PC runtime (bitmask of our numbering)
    uint32_t unhandled;       // console flags with no PC equivalent (bitmask of our own numbering)
    uint32_t compression_required; // non-zero if the resource cannot be uncompressed safely
} XtlFlagTranslation;

// Map console resource flags onto PC flags. Whatever cannot be represented is
// reported in `unhandled` rather than dropped, so the caller can log it and
// the loss is visible. Returns 0 on success, -1 on a null out.
int scarab_translate_resource_flags(uint32_t console_flags, XtlFlagTranslation* out);

// Map console heap flags onto PC flags. Same contract.
int scarab_translate_heap_flags(uint32_t console_heap_flags, XtlFlagTranslation* out);

// The console library's swizzle-mode choice for a 2D surface. This is a
// faithful port of xg_xs.dll XGComputeOptimalSwizzleMode - every argument is
// passed by position and nothing here is a proven fact (docs/xg_xs-decompiled.md):
//   layout   texture kind: 1 = linear, 2 = linear, 3/4 = surface kinds
//   flags    texture flags; the 0x40 bit => depth/stencil => 24
//   w/h      width, height in pixels
//   small_mult  byte multiplier of the small-surface check (0 means unknown:
//            the 4KB-small path is not taken)
//   out_mode  receives the console mode (0 linear, 5 = 4KB_S, 10 = 2D 64KB_D,
//            21 = 4KB small, 24 = depth 64KB, 27 = 64KB_R_X)
// Returns 0 on success, -1 on an out-of-range input.
int scarab_optimal_swizzle_mode(int layout, uint32_t flags, uint32_t w, uint32_t h,
                               uint32_t small_mult, uint32_t* out_mode);

// xg_xs.dll XGValidateDepthStencilSwizzleMode: the 7 depth/stencil modes the
// library accepts (4, 8, 0xc, 0x10, 0x14, 0x18, 0x1c). Returns 0 (S_OK) if
// valid, -1 otherwise.
int scarab_validate_depth_stencil_swizzle_mode(uint32_t mode);

// Console surfaces are tiled; PC surfaces are usually linear. Report whether a
// translation is needed and, if so, which swizzle mode the console format
// implies. The returned mode is the console's choice (advisory, see above);
// the PC runtime picks its own, so the honest use is log + use PC defaults.
// Returns 0 on success, -1 on a format it cannot map.
int scarab_swizzle_mode_for_format(uint32_t dxgi_format, uint32_t* out_swizzle_mode);


// ---------------------------------------------------------------------------
// XGSuggestSwizzleMode -- port of xg_xs.dll's own policy (see
// docs/xg_xs-decompiled.md). The descriptor is uint32-indexed (the oracle
// pins the mapping):
//   desc[0]    texture kind: 1 = linear, 2/3/4 = surfaces
//   desc[0xc]  flags word: bit 1 = render-target-ish, bit 0x400000 = wide surface
//   desc[4] / desc[6]  width / height (small-surface path only)
//   desc[8]    shape descriptor id (small-surface path only)
//   what       opaque flag (the exe passes 0)
// and returns the raw console mode through `out`: 0x100 for linear,
// 0x100+<mode> for surfaces (0x105 for the raw modes reachable from a
// caller), 0x118 for the render-target bit, 0x11b for the wide surface.
// `out` is written exactly when the raw mode is < 0x20 (as in the DLL).
// Returns 0 on success, 0x80070057 on failure (null/unknown kind).
int32_t XGSuggestSwizzleMode(const uint32_t* desc, uint32_t what, uint32_t* out);

// xg_xs.dll XGComputeOptimalDepthStencilSwizzleMode: the constant depth/stencil
// 64KB mode. Returns 0 (S_OK) and stores 0x18 at *out.
int32_t XGComputeOptimalDepthStencilSwizzleMode(uint32_t* out);

#ifdef __cplusplus
}
#endif
