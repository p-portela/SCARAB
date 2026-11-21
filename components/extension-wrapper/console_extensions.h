// ===========================================================================
//  console_extensions.h -- the recovered console (Scarlett) D3D12 extension ABI
// ===========================================================================
// The console extends D3D12 by *appending* methods to the PC interfaces and by
// adding console-only command-list types. The full recovered name set lives in
// docs/console-extension-catalogue.tsv (104 method names, 108 D3D12CONS_* enums),
// mined from the console UMD's own validation strings
// (vendor/x64_dx12/xgs12_pc_xs.dll, self-identified as
//  "[console user-mode driver for Direct3D 12, offline Scarlett build]").
//
// PROVENANCE - read this before trusting a slot:
//   * Method NAMES and the D3D12CONS_* symbol names: recovered, high confidence
//     (they appear verbatim in the UMD's validation messages).
//   * Which CLASS a method belongs to: recovered where the UMD message names it,
//     e.g. "ID3D12Device::CreateCommandQueueX cannot be called while the device
//     is suspended" ties CreateCommandQueueX to the device.
//   * METHOD ORDER within the appended block: **NOT recovered**. Assumed here,
//     and assumption #1 of the whole wrapper. Getting the order wrong is the one
//     failure mode that can corrupt state, so every appended slot logs its own
//     name and its raw arguments. If the order is wrong, the log shows a method
//     name that does not match the call the title made -- which turns the
//     assumption into a diagnosable symptom instead of silent corruption.
//   * SIGNATURES: inferred from the validation messages and from the closest PC
//     method. Marked per method.
//
// The trailing `XSlot_*` methods exist to absorb calls into appended slots whose
// identity we have not yet established: they log (slot index + register
// arguments) and return E_NOTIMPL, so an unknown console call is *recorded* rather
// than landing in an unrelated PC method.
// ===========================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// Console-only command-list type. Evidence, verbatim from the UMD:
//   "CreateCommandList: An riid of __uuidof(ID3D12ConsDmaCommandList) is
//    required for D3D12CONS_COMMAND_LIST_TYPE_DMA."
//   "@Flags must be set to D3D12CONS_COMMAND_LIST_FLAG_NONE for shader command
//    queues"
// ---------------------------------------------------------------------------
enum D3D12CONS_COMMAND_LIST_TYPE : UINT {
    D3D12CONS_COMMAND_LIST_TYPE_DMA = 6,   // appends to D3D12_COMMAND_LIST_TYPE (0..5)
};

enum D3D12CONS_COMMAND_LIST_FLAGS : UINT {
    D3D12CONS_COMMAND_LIST_FLAG_NONE            = 0x0,
    D3D12CONS_COMMAND_LIST_FLAG_ENABLE_IB_CHAINING = 0x1,
};

enum D3D12CONS_FLUSH_FLAGS : UINT {
    D3D12CONS_FLUSH_NONE                       = 0x0,
    D3D12CONS_FLUSH_IDLE                       = 0x1,
    D3D12CONS_FLUSH_RANGE_ALL                  = 0x2,
    D3D12CONS_FLUSH_TOP_SELECT_ENGINE_PFP      = 0x4,
    D3D12CONS_FLUSH_BOP_TEXTURE_L2_WRITEBACK   = 0x8,
};

// Evidence: "D3D12CONS_PRESENT_FLAG_*" set, including
// "ID3D12CommandQueue::PresentX - pPlaneParameters[%d].ResourceCount must
//  currently be less than or equal to 2."
enum D3D12CONS_PRESENT_FLAGS : UINT {
    D3D12CONS_PRESENT_FLAG_NONE                 = 0x0,
    D3D12CONS_PRESENT_FLAG_IMMEDIATE            = 0x1,
    D3D12CONS_PRESENT_FLAG_PERSIST              = 0x2,
    D3D12CONS_PRESENT_FLAG_UNIQUE_VIEW_RESOURCES = 0x4,
    D3D12CONS_PRESENT_PLANES_MAX                = 2,
};
#define D3D12CONS_PRESENT_VIEWS_MAX 8

// Evidence: "Cannot use D3D12CONS_PREFETCH_MEMORY_FLAG_USE_PFP on a compute
// command list."
enum D3D12CONS_PREFETCH_MEMORY_FLAGS : UINT {
    D3D12CONS_PREFETCH_MEMORY_FLAG_NONE    = 0x0,
    D3D12CONS_PREFETCH_MEMORY_FLAG_USE_PFP = 0x1,
};

// ===========================================================================
//  Device extension: appended after ID3D12Device4
// ===========================================================================
struct ID3D12DeviceX : public ID3D12Device4 {
    // --- named, class-attested by UMD messages -------------------------------
    // "ID3D12Device::CreateCommandQueueX cannot be called while the device is
    //  suspended." / "shader-based command queues only support
    //  D3D12_COMMAND_LIST_TYPE_COMPUTE."
    virtual HRESULT STDMETHODCALLTYPE CreateCommandQueueX(
        _In_ const void* pDesc, _In_ const void* pShaderQueueParams,
        REFIID riid, _COM_Outptr_ void** ppCommandQueue) = 0;

    // "ID3D12Device::GetFrameStatisticsX cannot be called while the device is
    //  suspended." -- stats getter; signature inferred.
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatisticsX(_Out_ void* pStatistics) = 0;

    // "ID3D12Device::ScheduleFrameEventX cannot be called while the device is
    //  suspended."
    virtual HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(_In_ UINT Id, _In_ UINT Flags) = 0;

    // "ID3D12Device::SetFrameIntervalX cannot be called while the device is
    //  suspended."
    virtual HRESULT STDMETHODCALLTYPE SetFrameIntervalX(_In_ UINT Interval, _In_ UINT Flags) = 0;

    // "ID3D12Device::SetDebugErrorFilterX: Invalid ErrorIdentifier 0x%X." /
    // "Invalid FilterFlags 0x%X."
    virtual HRESULT STDMETHODCALLTYPE SetDebugErrorFilterX(_In_ UINT ErrorIdentifier,
                                                           _In_ UINT FilterFlags) = 0;

    // --- absorbing slots for not-yet-placed device extensions -----------------
    // Recovered device-side names with no established slot: SetDebugFlagsX,
    // SetValidationFlagsX, CreateCommittedResourceX, CreatePlacedResourceX,
    // CreateComponentPlacedResourceX, CreateComputePipelineStateX,
    // CreateGraphicsPipelineStateX, CreateDerivedComputePipelineStateX,
    // CreateDerivedGraphicsPipelineStateX, Serialize/Deserialize*PipelineStateX,
    // Create*ShaderResourceViewX, CreatePlaced*ViewX, CreateSamplerX,
    // CreateMinMipShaderResourceViewX, GetSamplerBorderColorTableX,
    // GetComputeDescriptorX, CopyPageMappingsBatchX, RegisterPagePool2X,
    // UnregisterPagePoolX, EnableManualGraphicsTLBInvalidationX,
    // InvalidateGraphicsTLBX, FlushPipelineX, GetExecutionCommandSizeX,
    // SampleCountersX, ReportGpuHaltX, ReportGpuHangX, SetComputeShaderLimitsX,
    // SetGraphicsShaderLimitsX  (see docs/console-extension-catalogue.tsv)
    virtual HRESULT STDMETHODCALLTYPE XSlot_Device(UINT Slot, const void* a1, const void* a2,
                                                   const void* a3, const void* a4) = 0;
};

// ===========================================================================
//  Graphics command list extension: appended after ID3D12GraphicsCommandList6
// ===========================================================================
struct ID3D12GraphicsCommandListX : public ID3D12GraphicsCommandList6 {
    // "D3d12GraphicsCommandList::RSSetDepthBiasX can't be used with a closed
    //  command list" -- nearest PC method is RSSetDepthBias; signature inferred.
    virtual void STDMETHODCALLTYPE RSSetDepthBiasX(_In_ INT DepthBias,
                                                   _In_ FLOAT DepthBiasClamp,
                                                   _In_ FLOAT SlopeScaledDepthBias) = 0;

    // "ID3D12GraphicsCommandList(Compute) FillMemoryWith..." plus the
    // FillMemoryWith{32,64,128}BitValueX names from the catalogue.
    virtual void STDMETHODCALLTYPE FillMemoryWithPattern(_In_ ID3D12Resource* pResource,
                                                         _In_ UINT64 Offset,
                                                         _In_ UINT64 SizeBytes,
                                                         _In_ UINT Pattern) = 0;

    // "OMSetSampleMaskX is not supported on compute command lists."  On PC the
    // sample mask is baked into the PSO, so there is no direct forward.
    virtual void STDMETHODCALLTYPE OMSetSampleMaskX(_In_ UINT SampleMask) = 0;

    // "OMSetFrontAndBackStencilRef is not supported on compute command lists."
    // PC has a front reference only.
    virtual void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(_In_ UINT Front,
                                                               _In_ UINT Back) = 0;

    // --- absorbing slots ------------------------------------------------------
    // Recovered list-side names with no established slot: SetSampleMaskX,
    // RSSetMSAAParametersX, RSSetShadingRateX, OMSetDepthBoundsX,
    // SetHiStencilStateX, SetOcclusionQueryControlX, SetPerfectOcclusionQueriesX,
    // SetPixelShaderDepthForceZOrderX, SetDeferredPrimitiveBatchBinningX,
    // SetShaderScratchSpaceX, SetComputeShaderLaunchParametersX,
    // SetResourceCompressionStateX, ResourceBarrierX, CopyResourceX,
    // CopyTextureRegionX, CopyMemoryToMemoryX, Write32BitValue*X,
    // Write64BitValue*X, Wait32BitValueX, Wait64BitValueX, WaitFrameEventX,
    // SyncTopOfPipeX, SyncBottomOfPipeX, PrefetchMemoryX, PrefetchPipelineStateX,
    // FlushPipelineX, CloseBundleX, ExecuteBundleIndirectX, ExecuteIndirectBundleX,
    // DispatchX, DispatchMeshX, DispatchRaysX, DispatchRaysDimensionsX,
    // DrawIndexedX, WritePRTLODStatisticsX, ReadGDSX, WriteGDSX,
    // SetupOrderedAppendCounterX, AllocateFromCommandAllocatorX, KickoffX,
    // SetDebugFlagsX  (see docs/console-extension-catalogue.tsv)
    virtual HRESULT STDMETHODCALLTYPE XSlot_CommandList(UINT Slot, const void* a1, const void* a2,
                                                        const void* a3, const void* a4) = 0;
};

// ===========================================================================
//  DMA command list:  ID3D12ConsDmaCommandList
// ===========================================================================
// This one has a REAL exported IID (IID_ID3D12ConsDmaCommandList, exported by
// the console runtime), so the wrapper can answer QI for it without guessing.
// Evidence for its methods, verbatim from the UMD:
//   "ID3D12ConsDmaCommandList::FillRawBuffer - SizeBytes must be DWORD aligned."
//   "DMA engine cannot copy to a multisampled resource."
//   "DMA engine cannot copy from a multisampled resource."
//   "Destination resource cannot be NULL for subresource index copy type."
//   "Destination offset + NumBytes is out of bounds."
// and the catalogued names FillRawBuffer, CopyResource.
struct ID3D12ConsDmaCommandList : public ID3D12GraphicsCommandListX {
    virtual void STDMETHODCALLTYPE FillRawBuffer(_In_ ID3D12Resource* pResource,
                                                 _In_ UINT64 Offset,
                                                 _In_ UINT64 SizeBytes,
                                                 _In_ UINT Value) = 0;
    virtual void STDMETHODCALLTYPE CopyResource(_In_ ID3D12Resource* pDst,
                                                _In_ ID3D12Resource* pSrc) = 0;
};
