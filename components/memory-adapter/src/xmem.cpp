// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
//
// xmem.cpp -- the xmem.dll shim.
//
// xmem.dll has no public ABI. Semantics implemented here follow the
// dll-surface.md shim spec, the exe's import table (12 imports) and the
// reference export enumeration (18 names). Where the console behaviour is not
// publicly documented, the comment says so rather than the code guessing.
//
// The physical-page model is the one the hardware and the reference
// implementations agree on: the GPU page is 64 KiB, a "physical page" is an
// opaque index into one section, and the section is SEC_RESERVE, so nothing is
// committed until a page is first touched. Mapping the same page index at two
// addresses aliases the same bytes, as the console's CPU/GPU views do.

#define _WIN32_WINNT 0x0A00  // VirtualAlloc2 / MapViewOfFile3
#include "xmem.h"
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <mutex>
#include <utility>
#include <vector>

#define PAGE_64K      0x10000ull
#define FIRST_PAGE_NUM 1ull // 0 means "none"; the numbering itself is opaque to the title

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

namespace {

// VirtualAlloc2 / MapViewOfFile3 live in kernelbase and are absent from
// mingw's kernel32 import library, so they are resolved at run time. A host
// without them gets the plain-reservation path below.
typedef PVOID (WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID (WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                         void*, ULONG);

template <typename Fn>
Fn resolve(const char* name) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    Fn fn = kernelbase ? (Fn)GetProcAddress(kernelbase, name) : nullptr;
    if (!fn) fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name);
    return fn;
}

VirtualAlloc2Fn virtual_alloc2() {
    static VirtualAlloc2Fn fn = resolve<VirtualAlloc2Fn>("VirtualAlloc2");
    return fn;
}

MapViewOfFile3Fn map_view_of_file3() {
    static MapViewOfFile3Fn fn = resolve<MapViewOfFile3Fn>("MapViewOfFile3");
    return fn;
}

}  // namespace

void* XMemVirtualAlloc(void* address, size_t size, uint32_t type, uint64_t attributes, uint32_t protect) {
    if (size == 0) size = 1;
    (void)attributes; // console attribute bits are undocumented
    // The console guarantees large pages, so the title asks for them and treats
    // failure as fatal. The memory contract (readable/writable pages at that
    // address) survives without them; only the granularity differs.
    // shortcut: large-page requests are served as normal pages. Upgrade: back
    // them with a large-page section when the host can commit one.
    type &= ~MEM_LARGE_PAGES;
    // MEM_PHYSICAL means "this is the UMA pool the GPU reads", not AWE; on the
    // host the pages are ordinary memory. Windows also rejects it with
    // MEM_COMMIT, so it only survives on the reserve path below, where the
    // placeholder already implies the same thing.
    if (type & MEM_COMMIT) type &= ~MEM_PHYSICAL;
    uint32_t protect_f = (protect & 0x1ffu) ? (protect & 0x1ffu) : PAGE_NOACCESS;
    if ((type & MEM_RESERVE) && !(type & MEM_COMMIT)) {
        // A placeholder, so XMemMapPhysicalPages can put real pages over it
        // (and so it can alias). Needs 64 KiB alignment, which the titles use.
        VirtualAlloc2Fn alloc2 = virtual_alloc2();
        void* p = alloc2 ? alloc2(GetCurrentProcess(), address, size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, protect_f, NULL, 0)
                         : NULL;
        if (p) return p;
        // Host without placeholders: a plain reservation, mapped privately.
    }
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
// One 16 GiB SEC_RESERVE section backs every page; a page number is an index
// into it in 64 KiB units. Nothing is committed until the page is first
// written, so a mapping is lazy by construction. The title only ever passes
// page numbers back to these functions, so the base of the numbering is ours
// to choose (1, so 0 can mean "none").

namespace {

constexpr uint64_t PHYSICAL_SIZE = 16ull << 30;
constexpr uint64_t PAGE_COUNT = PHYSICAL_SIZE / PAGE_64K;

struct State {
    HANDLE section = NULL;
    std::vector<uint8_t> used = std::vector<uint8_t>(PAGE_COUNT);
    std::vector<std::pair<void*, uint64_t>> views;  // mapped address -> first page
    std::mutex lock;
    State() {
        section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE,
                                     (DWORD)(PHYSICAL_SIZE >> 32), (DWORD)PHYSICAL_SIZE, NULL);
    }
};

State& state() {
    static State s;
    return s;
}

bool valid_page(uint64_t page) {
    return page >= FIRST_PAGE_NUM && page - FIRST_PAGE_NUM < PAGE_COUNT;
}

// Maps [address, address + n*64K) over the section pages. `n` pages that are
// not contiguous are refused: only the scatter form can express those.
void* map_pages(void* address, uint32_t n, const uint64_t* pages) {
    if (!address || n == 0 || !pages) return NULL;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (!s.section) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (!valid_page(pages[i]) || !s.used[pages[i] - FIRST_PAGE_NUM]) return NULL;
        if (i && pages[i] != pages[0] + i) return NULL;
    }
    void* p = NULL;
    MapViewOfFile3Fn map3 = map_view_of_file3();
    if (map3)
        p = map3(s.section, GetCurrentProcess(), address, pages[0] * PAGE_64K,
                 (SIZE_T)n * PAGE_64K, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    if (!p) {
        // Not a placeholder (the title reserved with plain VirtualAlloc): commit
        // privately. The pages are then private to this address, not aliased.
        if (!VirtualAlloc(address, (SIZE_T)n * PAGE_64K, MEM_COMMIT, PAGE_READWRITE)) return NULL;
        p = address;
    }
    s.views.push_back({p, pages[0]});
    return p;
}

}  // namespace

int XMemAllocatePhysicalPages(uint64_t flags, uint32_t* count_inout, uint64_t* pages_out) {
    (void)flags;
    if (!count_inout || !pages_out) return 0;
    uint32_t want = *count_inout;
    if (want == 0) return 0;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (!s.section) { *count_inout = 0; return 0; }
    uint64_t run = 0, start = 0;
    for (uint64_t i = 0; i < PAGE_COUNT; i++) {
        if (s.used[i]) { run = 0; continue; }
        if (run == 0) start = i;
        if (++run == want) {
            for (uint32_t k = 0; k < want; k++) {
                s.used[start + k] = 1;
                pages_out[k] = FIRST_PAGE_NUM + start + k;
            }
            *count_inout = want;
            return 1;
        }
    }
    *count_inout = 0;
    return 0;
}

int XMemFreePhysicalPages(uint32_t n, const uint64_t* pages) {
    if (n == 0) return 1;
    if (!pages) return 0;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!valid_page(pages[i])) return 0;
    }
    // A view that starts on a freed page goes with it; a view spanning freed
    // and live pages is left alone.
    // shortcut: partial-span views are not unmapped, and no reference count
    // exists; a title that frees a page still mapped elsewhere keeps the bytes.
    // Upgrade: refcount per page and split views.
    for (size_t i = s.views.size(); i-- > 0;) {
        for (uint32_t k = 0; k < n; k++) {
            if (s.views[i].second == pages[k]) {
                UnmapViewOfFile(s.views[i].first);
                s.views.erase(s.views.begin() + i);
                break;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) s.used[pages[i] - FIRST_PAGE_NUM] = 0;
    return 1;
}

void* XMemMapPhysicalPages(void* address, uint32_t n, const uint64_t* pages) {
    if (!address) return NULL;
    if (n == 0) return address; // an empty map is a no-op (kept per the doc header)
    return map_pages(address, n, pages);
}

int XMemMapPhysicalPagesScatter(void** addresses, uint32_t n, const uint64_t* pages) {
    if (!addresses || !pages) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!addresses[i]) return 0;
        if (!map_pages(addresses[i], 1, pages + i)) return 0;
    }
    return 1;
}

int XMemTransferMemory(uint32_t kind, size_t size) {
    (void)kind; (void)size;
    return 1; // no tools memory to move; transfers are permitted
}

int XMemIsToolsMemAvailable(void) { return 0; }

// ---------------------------------------------------------------------------
// Exported by the real xmem.dll but not imported by this title
// ---------------------------------------------------------------------------
//
// From the reference export enumeration (18 names). Their contracts are not
// known -- signature, return and side effects -- so each refuses: it returns 0
// and touches nothing. A title that calls one gets a defined failure instead of
// a guess written into its memory. Declared variadic on purpose: a wrong typed
// signature would read arguments the caller may not have passed.

#define XMEM_REFUSED(name) \
    extern "C" uint64_t name(...) { return 0; }

XMEM_REFUSED(XMemSetAllocationHooks)
XMEM_REFUSED(XMemGetAllocationStatistics)
XMEM_REFUSED(XMemGetAllocationHysteresis)
XMEM_REFUSED(XMemSetAllocationHysteresis)
XMEM_REFUSED(XMemMakeTag)
XMEM_REFUSED(XMemPreallocateFreeSpace)
XMEM_REFUSED(XMemGetWin32HeapStatistics)
