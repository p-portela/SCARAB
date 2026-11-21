// ===========================================================================
//  shader_bridge.h -- seam between the D3D12 bridge and the shader pipeline
// ===========================================================================
// On Scarlett the title hands pre-compiled *GPU microcode* to the driver through
// D3D12UploadCustomMicrocodeX. That microcode is the FSSM container holding AMD
// RDNA2 / gfx10 machine code. The PC runtime and the PC vendor UMD accept DXIL
// (or SPIR-V under Vulkan) and never microcode, so this is the API-level face of
// the whole shader translation problem:
//
//     FSSM blob  ->  raw RDNA2 stream      components/shader-bridge/fssm_to_rdna2.py   [done]
//                ->  DXIL                  the upstream shader recompiler's RDNA2 front-end + a DXIL emitter [in progress]
//                ->  PC shader blob        this seam                                 [this header]
//
// TARGET IS DXIL, NOT SPIR-V. vkd3d-proton consumes DXIL, and so does Microsoft's
// D3D12 runtime, so DXIL is the one output that serves both backends (Windows is
// the primary target, Linux via vkd3d-proton the secondary). the upstream shader recompiler already emits
// SPIR-V; that is useful as a Linux-only cross-check, but making it the primary
// target would lock the shader work to the Linux path.
//
// STATUS: extraction is done and the RDNA2 stream is available for all 25,321
// shaders. Recompilation is blocked on decoder coverage and on a missing
// validation layer. Until that closes, the bridge fails loudly rather than
// accepting microcode the PC runtime would reject anyway.
//
// The other half of this component is the binding ABI: console takes bindings from
// the PSO root signature, not from a per-shader header, so an SRT/BDA equivalent
// has to be synthesised for any shader that reads buffers or images. See
// docs/shader-abi-notes.md.

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum XtlShaderStage {
    SCARAB_SHADER_STAGE_UNKNOWN = 0,
    SCARAB_SHADER_STAGE_VS,
    SCARAB_SHADER_STAGE_PS,
    SCARAB_SHADER_STAGE_GS,
    SCARAB_SHADER_STAGE_HS,
    SCARAB_SHADER_STAGE_DS,
    SCARAB_SHADER_STAGE_CS,
    SCARAB_SHADER_STAGE_MS,   // mesh   (SM 6.5+; mandatory on Scarlett)
    SCARAB_SHADER_STAGE_AS,   // amplification
    SCARAB_SHADER_STAGE_RAY,  // XDXR
} XtlShaderStage;

typedef struct XtlShaderTranslateStats {
    uint32_t fssm_version;        // 0x101 observed across the corpus
    uint32_t code_offset;         // ISA begins at code_offset + 0x10
    uint32_t code_size;
    uint32_t instruction_count;
    uint32_t decoded;             // instructions the recompiler accepted
    uint32_t undecoded;           // instructions it rejected
    uint32_t spirv_bytes;
    uint32_t reserved;
} XtlShaderTranslateStats;

// Unwrap the console container and report what is inside, without translating.
// Cheap; used for triage over the whole corpus.
int scarab_shader_unwrap(const void* fssm, size_t fssm_size,
                      const void** out_rdna2, size_t* out_rdna2_size,
                      XtlShaderTranslateStats* out_stats);

// Translate console microcode into a PC shader blob.
// Returns 0 on success; non-zero on refusal, which is the expected outcome until
// the recompiler covers the whole corpus. On refusal, *out_diagnostic receives a
// human-readable reason (owned by the caller's allocator contract below).
int scarab_shader_translate(const void* fssm, size_t fssm_size,
                         XtlShaderStage stage, const void* root_signature, size_t root_signature_size,
                         void** out_pc_blob, size_t* out_pc_blob_size,
                         char* out_diagnostic, size_t diagnostic_capacity);

// Bindings for shaders that read buffers/images. console supplies these through the
// PSO root signature; a PC shader needs them baked in. Returns 0 on success.
int scarab_shader_synthesise_bindings(const void* root_signature, size_t root_signature_size,
                                   const void* pc_blob, size_t pc_blob_size,
                                   void** out_bound_blob, size_t* out_bound_blob_size);

#ifdef __cplusplus
}
#endif
