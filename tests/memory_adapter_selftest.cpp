// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// memory_adapter_selftest.cpp -- runs the flag translation, swizzle policy and
// xmem shim with no GPU and no console, so the layer's own semantics can be
// verified on any machine (including under Wine on Linux).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory_adapter.h"
#include "xmem.h"

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

int main() {
    printf("memory-adapter self-test\n");

    // --- flag translation: honest report ---
    XtlFlagTranslation t;
    check(scarab_translate_resource_flags(0x401u, &t) == 0, "resource translation succeeds");
    check(t.pc_flags == 0, "resource translation reports nothing provable");
    check(t.unhandled != 0, "resource translation reports unmapped flags");
    check(scarab_translate_heap_flags(0x1u, &t) == 0, "heap translation succeeds");

    // --- swizzle policy: the three provably correct branches ---
    uint32_t mode;
    check(scarab_optimal_swizzle_mode(1, 0, 16, 16, 0, &mode) == 0 && mode == 0,
          "layout 1 -> linear");
    check(scarab_optimal_swizzle_mode(4, 0x40u, 16, 16, 0, &mode) == 0 && mode == 0x18u,
          "depth bit 0x40 -> 0x18");
    check(scarab_optimal_swizzle_mode(4, 0u, 1024, 1024, 0, &mode) == 0 && mode == 5u,
          "plain surface -> 4KB_S");

    // --- depth/stencil validation: the 7 allowed modes ---
    static const uint32_t good[] = {4, 8, 0x0c, 0x10, 0x14, 0x18, 0x1c};
    for (uint32_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        check(scarab_validate_depth_stencil_swizzle_mode(good[i]) == 0, "mode valid");
    }
    static const uint32_t bad[] = {0, 1, 0x02, 0x06, 0x16, 0x20};
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(scarab_validate_depth_stencil_swizzle_mode(bad[i]) != 0, "mode rejected");
    }

    // --- xmem shim: alloc/free round-trip ---
    void* p = XMemAlloc(4096, 0x00400000u); // alignment bit 4 => 16 bytes
    check(p != NULL, "XMemAlloc returns memory");
    if (p) {
        memset((uint8_t*)p, 0xab, 4096);
        void* q = XMemAlloc(1024, 0x00000000u);
        check(q != NULL && q != p, "XMemAlloc distinguishes allocations");
        XMemFree(p, 0x00400000u);
        XMemFree(q, 0x00000000u);
    }

    // working set statistics: non-null, budget written
    uint8_t stats[0x50];
    check(XMemGetWorkingSetStatistics(0, stats) != 0, "XMemGetWorkingSetStatistics succeeds");
    check(*(uint64_t*)(stats + 0) > 0, "budget written");

    // physical pages: numbers written, count in/out
    uint32_t n = 4;
    uint64_t pages[4] = {0, 0, 0, 0};
    check(XMemAllocatePhysicalPages(0, &n, pages) != 0, "XMemAllocatePhysicalPages succeeds");
    check(n == 4u, "count in/out");
    bool all_nonzero = true;
    for (uint32_t i = 0; i < 4; i++) all_nonzero = all_nonzero && pages[i] != 0;
    check(all_nonzero, "page numbers written");

    // physical pages map: reserve then commit
    constexpr size_t SZ = 4 * 0x10000;
    void* base = VirtualAlloc(NULL, SZ, MEM_RESERVE, PAGE_NOACCESS);
    check(base != NULL, "reserve for pages");
    if (base) {
        check(XMemMapPhysicalPages(base, 2, nullptr) != NULL, "XMemMapPhysicalPages commits");
        void* addrs[2] = { base, (uint8_t*)base + 0x10000 };
        check(XMemMapPhysicalPagesScatter(addrs, 2, nullptr) != 0, "scatter commits");
        VirtualFree(base, 0, MEM_RELEASE);
    }

    // query on a live allocation
    void* vq = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION mbi;
    check(vq && XMemVirtualQuery(GetCurrentProcess(), vq, &mbi) != 0 &&
          mbi.Protect == PAGE_READWRITE, "XMemVirtualQuery on live memory");
    if (vq) VirtualFree(vq, 0, MEM_RELEASE);

    // XGSuggestSwizzleMode: linear kind -> 0x100; render-target bit -> 0x118;
    // surface kinds -> 0x100+5; unknown kind -> failure. The descriptor is
    // uint32-indexed (see the implementation).
    uint8_t desc_bytes[0x40] = {0};
    *(uint32_t*)(desc_bytes + 0x00) = 1;
    uint32_t raw = 0;
    check(XGSuggestSwizzleMode((const uint32_t*)desc_bytes, 0, &raw) == 0 && raw == 0x100u,
          "XGSuggestSwizzleMode linear -> 0x100");

    uint8_t desc_rt[0x40] = {0};
    *(uint32_t*)(desc_rt + 0x00) = 3;
    *(uint32_t*)(desc_rt + 0x30) = 2; // render-target-ish bit
    check(XGSuggestSwizzleMode((const uint32_t*)desc_rt, 0, &raw) == 0 && raw == 0x118u,
          "XGSuggestSwizzleMode RT bit -> 0x118");

    uint8_t desc_surf[0x40] = {0};
    *(uint32_t*)(desc_surf + 0x00) = 3;
    check(XGSuggestSwizzleMode((const uint32_t*)desc_surf, 0, &raw) == 0 && raw == 0x100u + 5u,
          "XGSuggestSwizzleMode surface -> 4KB_S");

    uint8_t desc_bad[0x40] = {0};
    *(uint32_t*)(desc_bad + 0x00) = 0; // unknown kind
    check(XGSuggestSwizzleMode((const uint32_t*)desc_bad, 0, &raw) != 0,
          "XGSuggestSwizzleMode unknown kind -> failure");

    // XGComputeOptimalDepthStencilSwizzleMode -> 0x18
    check(XGComputeOptimalDepthStencilSwizzleMode(&raw) == 0 && raw == 0x18u,
          "XGComputeOptimalDepthStencilSwizzleMode -> 0x18");

    // XGComputeTileShape: linear, fast surface path, refusals
    int t5[3] = {0, 0, 0}, t6[3] = {0, 0, 0};
    check(XGComputeTileShape(1, 0, 1, 1, t5, t6) == 0 &&
          t6[0] == 0x10000 && t6[1] == 1 && t6[2] == 1,
          "XGComputeTileShape linear -> 64 KiB tile");

    check(XGComputeTileShape(3, 0, 4, 2, t5, t6) == 0 &&
          t6[0] == 64 && t6[1] == 128 && t6[2] == 1,
          "XGComputeTileShape surface 4x2 -> 64/128");

    check(XGComputeTileShape(3, 0, 1, 4, t5, t6) == 0 &&
          t6[0] == 128 && t6[1] == 128 && t6[2] == 1,
          "XGComputeTileShape surface 1x4 -> 128/128");

    check(XGComputeTileShape(1, 0, 2, 1, t5, t6) != 0,
          "XGComputeTileShape linear refused tile size");

    check(XGComputeTileShape(3, 0, 99, 1, t5, t6) != 0,
          "XGComputeTileShape surface refused tile size");

    check(XGComputeTileShape(2, 0, 1, 1, t5, t6) != 0,
          "XGComputeTileShape refused kind");

    check(XGComputeTileShape(3, 1, 1, 1, t5, t6) != 0,
          "XGComputeTileShape descriptor path refused honestly");

    // tools memory: honest negative
    check(XMemIsToolsMemAvailable() == 0, "XMemIsToolsMemAvailable is honestly negative");

    printf(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return fails;
}
