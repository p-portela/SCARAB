// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
//
// xmem.cpp -- the xmem.dll shim.
//
// xmem.dll has no public ABI. Semantics implemented here follow the
// dll-surface.md shim spec (4 documented exports) and the exe's import table
// (12 imports). Where the console behaviour is not publicly documented, the
// comment says so rather than the code guessing.

#include "xmem.h"
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define PAGE_64K      0x10000ull
#define FRAMES_PER_PAGE 16u // 64 KiB / 4 KiB
#define FIRST_PAGE_NUM 0x10000ull // 0 means "none"

// Title-memory budget (advisory; the console does not publish how titles size
// their memory -- the layer's default).
#define TITLE_BUDGET_BYTES (8ull << 30) // 8 GiB

// ---------------------------------------------------------------------------
// XMemAlloc / XMemAllocDefault / XMemFree / XMemFreeDefault
// ---------------------------------------------------------------------------

// Alignment for a given XALLOC_ATTRIBUTES: log2 at bits 24..28.
static size_t xmem_alignment(uint64_t attributes) {
    uint32_t log2 = (uint32_t)((attributes >> 24) & 0x1f);
    if (log2 < 2u) log2 = 2u;
    return (size_t)1 << log2;
}

void* XMemAllocDefault(size_t size, uint64_t attributes) {
    if (size == 0) size = 1;
    size_t align = xmem_alignment(attributes);
    size_t total = (size + align - 1) & ~(align - 1);
    void* p = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return NULL;
    memset(p, 0, total);
    return p;
}

void* XMemAlloc(size_t size, uint64_t attributes) {
    return XMemAllocDefault(size, attributes);
}

void XMemFreeDefault(void* address, uint64_t attributes) {
    (void)attributes;
    if (address) VirtualFree(address, 0, MEM_RELEASE);
}

void XMemFree(void* address, uint64_t attributes) {
    XMemFreeDefault(address, attributes);
}

// ---------------------------------------------------------------------------
// XMemVirtualAlloc
// ---------------------------------------------------------------------------

void* XMemVirtualAlloc(void* address, size_t size, uint32_t type, uint64_t attributes, uint32_t protect) {
    if (size == 0) size = 1;
    (void)attributes; // console attribute bits are undocumented
    uint32_t protect_f = (protect & 0x1ffu) ? (protect & 0x1ffu) : PAGE_NOACCESS;
    return VirtualAlloc(address, size, type, protect_f);
}

// ---------------------------------------------------------------------------
// XMemVirtualQuery
// ---------------------------------------------------------------------------

int XMemVirtualQuery(void* process, const void* address, void* info) {
    if (!info) return 0;
    MEMORY_BASIC_INFORMATION* mbi = (MEMORY_BASIC_INFORMATION*)info;
    if (VirtualQueryEx((HANDLE)process, address, mbi, sizeof(MEMORY_BASIC_INFORMATION)) == 0) return 0;
    // The console's extra fields (XMem flags/tag) are unknown -- left zero.
    return 1;
}

// ---------------------------------------------------------------------------
// XMemGetWorkingSetStatistics
// ---------------------------------------------------------------------------

int XMemGetWorkingSetStatistics(uint32_t flags, uint8_t* stats) {
    (void)flags;
    if (!stats) return 0;
    memset(stats, 0, 0x50);
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        *(uint64_t*)(stats + 0)    = (uint64_t)TITLE_BUDGET_BYTES;
        *(uint64_t*)(stats + 8)    = (uint64_t)pmc.PagefileUsage;
        *(uint64_t*)(stats + 0x28) = (uint64_t)TITLE_BUDGET_BYTES;
        *(uint64_t*)(stats + 0x30) = (uint64_t)pmc.PagefileUsage;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Physical pages
// ---------------------------------------------------------------------------
//
// Honest shim: pages are numbered in 4 KiB frames (64 KiB page = 16 frames,
// 0 means "none"). XMemVirtualAlloc makes the address space; XMemMapPhysicalPages
// commits it. The console's lazy-commit / sparse semantics are NOT emulated --
// see docs/dll-surface.md (open item).

int XMemAllocatePhysicalPages(uint64_t flags, uint32_t* count_inout, uint64_t* pages_out) {
    (void)flags;
    if (!count_inout || !pages_out) return 0;
    uint32_t n = *count_inout;
    if (n == 0 || n > 1024) { *count_inout = 0; return 0; }
    for (uint32_t i = 0; i < n; i++) {
        pages_out[i] = FIRST_PAGE_NUM + (uint64_t)i * FRAMES_PER_PAGE;
    }
    *count_inout = n;
    return 1;
}

int XMemFreePhysicalPages(uint32_t n, const uint64_t* pages) {
    (void)n; (void)pages;
    return 1; // nothing is released here; the caller owns the address space
}

void* XMemMapPhysicalPages(void* address, uint32_t n, const uint64_t* pages) {
    (void)pages;
    if (!address) return NULL;
    if (n == 0) return address; // an empty map is a no-op (kept per the doc header)
    if (VirtualAlloc(address, (size_t)n * PAGE_64K, MEM_COMMIT, PAGE_READWRITE) == NULL) return NULL;
    return address;
}

int XMemMapPhysicalPagesScatter(void** addresses, uint32_t n, const uint64_t* pages) {
    (void)pages;
    if (!addresses) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!addresses[i]) return 0;
        if (VirtualAlloc(addresses[i], PAGE_64K, MEM_COMMIT, PAGE_READWRITE) == NULL) return 0;
    }
    return 1;
}

int XMemTransferMemory(uint32_t kind, size_t size) {
    (void)kind; (void)size;
    return 1; // no tools memory to move; transfers are permitted
}

int XMemIsToolsMemAvailable(void) { return 0; }
