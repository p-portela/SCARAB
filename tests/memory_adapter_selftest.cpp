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
    fflush(stdout);
    if (!ok) fails++;
}

int main() {
    printf("memory-adapter self-test\n");
    fflush(stdout);

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

    // working set statistics: the real struct, with the usage we can account for.
    // Limits are deliberately left zero until the budget policy exists (T04), so
    // that is asserted too -- a nonzero limit here would be an invented number.
    XMEM_WORKING_SET_STATISTICS stats;
    memset(&stats, 0, sizeof(stats));
    check(XMemGetWorkingSetStatistics(XMEM_WORKING_SET_ALL, &stats) != 0,
          "XMemGetWorkingSetStatistics succeeds");
    check(stats.GameUsedPeak > 0, "peak title usage accounted");
    check(stats.GameLimit == 0, "limits stay zero until the budget policy exists");

    // physical pages: 64 KiB units over one lazy section, aliasable
    ULONG_PTR n = 2;
    ULONG_PTR pages[2] = {0, 0};
    check(XMemAllocatePhysicalPages(0, &n, pages) != 0, "XMemAllocatePhysicalPages succeeds");
    check(n == 2u && pages[0] != 0 && pages[1] == pages[0] + 1,
          "two contiguous 64 KiB pages");
    if (n == 2) {
        constexpr size_t SZ = 2 * 0x10000;
        void* a = XMemVirtualAlloc(NULL, SZ, MEM_RESERVE, 0, PAGE_NOACCESS);
        void* b = XMemVirtualAlloc(NULL, SZ, MEM_RESERVE, 0, PAGE_NOACCESS);
        check(a && b && a != b, "two 64 KiB reservations");
        if (a && b) {
            check(XMemMapPhysicalPages(a, 2, pages) == a, "map 2 pages at the first VA");
            check(XMemMapPhysicalPages(b, 2, pages) == b, "map the same pages at the second VA");
            memset(a, 0x5a, SZ);
            check(((uint8_t*)b)[0] == 0x5a && ((uint8_t*)b)[SZ - 1] == 0x5a,
                  "the two mappings alias the same pages");
            ((uint8_t*)a)[0x10000] = 0x11;
            check(((uint8_t*)a)[0] == 0x5a, "64 KiB is the page unit");
        }
        ULONG_PTR n2 = 2;
        ULONG_PTR pages2[2] = {0, 0};
        check(XMemAllocatePhysicalPages(0, &n2, pages2) != 0, "allocate 2 more pages");
        if (n2 == 2) {
            void* addrs[2] = {XMemVirtualAlloc(NULL, 0x10000, MEM_RESERVE, 0, PAGE_NOACCESS),
                              XMemVirtualAlloc(NULL, 0x10000, MEM_RESERVE, 0, PAGE_NOACCESS)};
            check(addrs[0] && addrs[1] &&
                  XMemMapPhysicalPagesScatter(addrs, 2, pages2) != 0,
                  "scatter maps one page at each address");
            check(XMemFreePhysicalPages(2, pages2) != 0, "free the scattered pages");
        }
        check(XMemFreePhysicalPages(2, pages) != 0, "free the aliased pages");
        ULONG_PTR n3 = 2;
        ULONG_PTR pages3[2] = {0, 0};
        check(XMemAllocatePhysicalPages(0, &n3, pages3) != 0 &&
              pages3[0] == pages[0] && pages3[1] == pages[1],
              "freed pages are reused");
        if (n3 == 2) XMemFreePhysicalPages(2, pages3);
    }

    // Regression: the LAST page number in the range must be mappable and must
    // alias. The section offset is (page - FIRST_PAGE_NUM) * 64 KiB; using the
    // raw page number put the final page one page past the end of the section,
    // so it silently fell back to a private commit and aliasing broke for
    // exactly one page -- the failure mode a 2-page test cannot see.
    {
        constexpr ULONG_PTR TOTAL = (16ull << 30) / 0x10000ull;  // one entry per 64 KiB page
        static ULONG_PTR all_pages[TOTAL];
        ULONG_PTR want = TOTAL;
        check(XMemAllocatePhysicalPages(0, &want, all_pages) != 0 && want == TOTAL,
              "allocate the whole 16 GiB page range");
        ULONG_PTR last = all_pages[TOTAL - 1];
        void* la = XMemVirtualAlloc(NULL, 0x10000, MEM_RESERVE, 0, PAGE_NOACCESS);
        void* lb = XMemVirtualAlloc(NULL, 0x10000, MEM_RESERVE, 0, PAGE_NOACCESS);
        if (la && lb) {
            check(XMemMapPhysicalPages(la, 1, &last) == la, "map the last page at A");
            check(XMemMapPhysicalPages(lb, 1, &last) == lb, "map the last page at B");
            *(volatile uint8_t*)la = 0x77;
            check(*(volatile uint8_t*)lb == 0x77, "the last page aliases");
        }
        check(XMemFreePhysicalPages(TOTAL, all_pages) != 0, "free the whole range");
    }

    // query on a live allocation (XMEM_BASIC_INFORMATION, not MEMORY_BASIC_
    // INFORMATION: the console struct appends XMemFlags/TagCode/TagFourCC)
    void* vq = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    XMEM_BASIC_INFORMATION mbi;
    memset(&mbi, 0, sizeof(mbi));
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
