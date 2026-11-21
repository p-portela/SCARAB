// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ===========================================================================
//  present_adapter.h -- DXGIX presentation onto PC DXGI
// ===========================================================================
// STATUS: seam only. The console runtime's four presentation exports are
// currently emulated inside the bridge (recorded and acknowledged). This header
// is where the real implementation will live once there is a swapchain registry
// to drive.
//
// What differs from PC, from the console runtime's own export list and the UMD's
// validation strings:
//
//  * DXGIXPresentArray presents an *array* of swapchains in one call - there is
//    no PC equivalent. A real implementation keeps a registry of the PC
//    swapchains it created and presents each in turn.
//  * DXGIXSetFrameNotification / DXGIXSetVLineNotification: the console drives
//    frame and vblank notifications from the display engine. On PC the closest
//    primitive is D3DKMTWaitForVerticalBlankEvent.
//  * DXGIXGetFrameStatistics: console frame statistics have no PC counterpart;
//    the bridge currently zeroes the structure rather than inventing numbers.
//  * ID3D12CommandQueue::PresentX carries presentation planes
//    (pPlaneParameters[].ResourceCount <= 2) and flags, including
//    PRESENT_FLAG_PERSIST and PRESENT_FLAG_UNIQUE_VIEW_RESOURCES. None of these
//    exist in IDXGISwapChain::Present.
//
// The rule for this component is the project-wide one: never report success for
// something that did not happen. A vblank notification that will never fire must
// be logged as unimplemented, not swallowed.

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Recovered console present flags (names evidence-backed, values unknown).
typedef enum XtlconsolePresentFlag {
    SCARAB_CONS_PRESENT_NONE                 = 0,
    SCARAB_CONS_PRESENT_IMMEDIATE            = 0,
    SCARAB_CONS_PRESENT_PERSIST              = 0,
    SCARAB_CONS_PRESENT_UNIQUE_VIEW_RESOURCES= 0,
} XtlconsolePresentFlag;

#define SCARAB_CONS_PRESENT_PLANES_MAX 2
#define SCARAB_CONS_PRESENT_VIEWS_MAX  8

typedef struct XtlFrameStatistics {
    uint64_t frame_count;         // console semantics; PC supplies what it can
    uint64_t missed_frames;
    uint64_t reserved[6];
} XtlFrameStatistics;

// Register a PC swapchain so that a later DXGIXPresentArray can drive it.
int scarab_present_register_swapchain(void* pc_swapchain, uint32_t id);
int scarab_present_unregister_swapchain(uint32_t id);

// Emulate DXGIXPresentArray over the registered swapchains.
int scarab_present_array(const uint32_t* ids, uint32_t count, uint32_t flags);

// vblank / frame notifications. Returns non-zero while unimplemented so callers
// can distinguish "armed" from "acknowledged but inert".
int scarab_present_set_vline_notification(uint32_t scanline, void* event_handle);
int scarab_present_set_frame_notification(uint64_t frame_index, void* event_handle);
int scarab_present_get_frame_statistics(XtlFrameStatistics* out);

#ifdef __cplusplus
}
#endif
