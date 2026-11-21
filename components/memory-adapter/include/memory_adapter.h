// ===========================================================================
//  memory_adapter.h -- console resource/heap semantics onto PC D3D12
// ===========================================================================
// STATUS: seam only. Nothing here is implemented yet; the header exists so the
// next work item has a home instead of a blank page, and so the *shape* of the
// problem is written down while the evidence is fresh.
//
// Why this is a real component and not a detail:
//
//  * Scarlett has unified memory and no VRAM residency concept. PC D3D12 has
//    heaps and residency. A title that assumes console allocator behaviour will
//    misbehave in ways no amount of API forwarding fixes.
//  * The console has GPU memory compression that PC does not expose: DCC (delta
//    color compression) and depth/stencil HTILE, with copy flags to force
//    decompression or resummarise HTILE. xg_xs.dll carries the encode/decode and
//    swizzle-mode logic (XGEncodeDCC, XGDecodeDCC, XGComputeOptimalSwizzleMode).
//  * The console has heap and resource flags with no PC equivalent.
//
// The flag values below are RECOVERED NAMES from the console UMD's validation
// strings (see docs/console-extension-catalogue.tsv). Their numeric values are NOT
// recovered - they are console enum members whose values we do not know. Do not
// assign numbers here without evidence: a wrong value silently mistranslates.
//
// Two honest strategies are available and neither is chosen yet:
//   (a) strip unsupported flags and let the PC runtime allocate uncompressed,
//       accepting the bandwidth/quality delta;
//   (b) emulate compression semantics where observable (e.g. honour
//       FORCE_DECOMPRESS by converting the copy into an uncompressed copy).

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Recovered console resource flags. Values unknown; names are evidence-backed.
typedef enum XtlconsoleResourceFlag {
    XTL_CONSOLE_RES_ALLOW_DCC                          = 0,
    XTL_CONSOLE_RES_FORCE_DCC_TEXTURE_COMPATIBILITY    = 0,
    XTL_CONSOLE_RES_FORCE_FMASK_TEXTURE_COMPATIBILITY  = 0,
    XTL_CONSOLE_RES_DENY_COLOR_COMPRESSION_DATA        = 0,
    XTL_CONSOLE_RES_DENY_DEPTH_COMPRESSION_DATA        = 0,
    XTL_CONSOLE_RES_DENY_DEPTH_COMPRESSION_EXPCLEAR    = 0,
    XTL_CONSOLE_RES_DENY_DEPTH_COMPRESSION_PLANES      = 0,
    XTL_CONSOLE_RES_DENY_DEPTH_STENCIL_HTILE           = 0,
    XTL_CONSOLE_RES_DENY_STENCIL_COMPRESSION_DATA      = 0,
    XTL_CONSOLE_RES_ALLOW_INDIRECT_BUFFER              = 0,
    XTL_CONSOLE_RES_ALLOW_AUTOMATIC_GAMEDVR_TONE_MAP   = 0,
} XtlconsoleResourceFlag;

// Recovered console heap flags.
typedef enum XtlconsoleHeapFlag {
    XTL_CONSOLE_HEAP_ALLOW_INDIRECT_BUFFERS = 0,
} XtlconsoleHeapFlag;

typedef struct XtlFlagTranslation {
    uint32_t pc_flags;        // flags safe to pass to the PC runtime
    uint32_t unhandled;       // console flags with no PC equivalent (bitmask of our own numbering)
    uint32_t compression_required; // non-zero if the resource cannot be uncompressed safely
} XtlFlagTranslation;

// Map console resource flags onto PC D3D12_RESOURCE_FLAGS. Whatever cannot be
// represented is reported in `unhandled` rather than dropped, so the caller can
// log it and the loss is visible.
int xtl_translate_resource_flags(uint32_t console_flags, XtlFlagTranslation* out);

// Map console heap flags onto PC D3D12_HEAP_FLAGS.
int xtl_translate_heap_flags(uint32_t console_flags, XtlFlagTranslation* out);

// Console surfaces are tiled; PC surfaces are usually linear. Report whether a
// translation is needed and, if so, which swizzle mode the console format implies.
int xtl_swizzle_mode_for_format(uint32_t dxgi_format, uint32_t* out_swizzle_mode);

#ifdef __cplusplus
}
#endif
