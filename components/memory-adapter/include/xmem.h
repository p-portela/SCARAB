// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ===========================================================================
//  xmem.h -- the xmem.dll shim
// ===========================================================================
//  xmem.dll has no known public version (docs/dll-surface.md keeps its own
//  spec: a shim over VirtualAlloc/HeapAlloc). The title (artifacts/docs/
//  master-exe-surface.md) imports 12 functions from it. Every signature here
//  is an honest guess from the call sites and the spec -- the console's real
//  ABI is not publicly known, so anything not in the spec is logged by the
//  caller rather than trusted.

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- documented spec (dll-surface.md) -----------------------------------
// XMemAlloc(size, attributes): zeroed heap allocation, 0 on failure.
// Attributes: alignment as log2 (bits 24..28), memory type at bit 29,
// 32-bit address flag at bit 33.
void* XMemAlloc(size_t size, uint64_t attributes);
void* XMemAllocDefault(size_t size, uint64_t attributes);
void  XMemFree(void* address, uint64_t attributes);
void  XMemFreeDefault(void* address, uint64_t attributes);

// XMemGetWorkingSetStatistics(flags, stats): stats is a 0x50-ish buffer;
// filled with the title's limit/used at +0/+8 and 0 elsewhere. Returns 1.
int   XMemGetWorkingSetStatistics(uint32_t flags, uint8_t* stats);

// XMemVirtualQuery(process, address, info): host VirtualQueryEx plus a
// zeroed XMem flags/tag region; returns 1 on success.
int   XMemVirtualQuery(void* process, const void* address, void* info);

// --- exe surface (master-exe-surface.md, 12 imports) --------------------
// XMemVirtualAlloc(address, size, type, attributes, protect):
// type is VirtualAlloc-ish (RESERVE/COMMIT/LARGE_PAGES(0x2000_0000));
// attributes like XMemAlloc. Returns the host address, NULL on failure.
// Address requirements (2^40 etc.) are NOT honoured by a shim -- log that.
void* XMemVirtualAlloc(void* address, size_t size, uint32_t type, uint64_t attributes, uint32_t protect);

// XMemAllocatePhysicalPages(flags, count_inout, pages_out):
// count in = pages wanted; out = count given; pages written into pages_out.
// Page numbers count 4 KiB frames (a 64 KiB page is a multiple of 16); 0 means
// "none". Returns 1 on success, 0 otherwise (count = 0).
int   XMemAllocatePhysicalPages(uint64_t flags, uint32_t* count_inout, uint64_t* pages_out);

// XMemFreePhysicalPages(n, pages): releases the given pages. Returns 1.
int   XMemFreePhysicalPages(uint32_t n, const uint64_t* pages);

// XMemMapPhysicalPages(address, n, pages): commits the reservation at
// address for the given pages. Returns the address (NULL = failure).
// Passing n=0 is a no-op and returns the address.
void* XMemMapPhysicalPages(void* address, uint32_t n, const uint64_t* pages);

// XMemMapPhysicalPagesScatter(addresses, n, pages): commits each page at the
// address given (scatter). Returns 1 if every page landed, 0 otherwise.
int   XMemMapPhysicalPagesScatter(void** addresses, uint32_t n, const uint64_t* pages);

// XMemTransferMemory(kind, size): no tools memory exists; transfers of title
// memory are permitted up to the budget. Returns 1.
int   XMemTransferMemory(uint32_t kind, size_t size);

// XMemIsToolsMemAvailable(): 0 -- a retail console has no tools memory.
int   XMemIsToolsMemAvailable(void);

#ifdef __cplusplus
}
#endif
