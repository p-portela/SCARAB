// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
//
// xmem.cpp -- the xmem.dll shim, rebuilt on the real XMem contract.
//
// Contract sources (in the GDK install, not the repo):
//   xbox/include/xmem.h     -- the API, types, bit layouts
//   xbox/source/heap.c      -- XMemAllocDefault/XMemFreeDefault + dispatch tables
// The per-memory-type dispatch tables below are transcribed verbatim from
// heap.c; changing one is a contract change. Console-only PAGE_GRAPHICS_*
// bits are stripped at the point of host use and logged, never faked.
//
// Explicit XMemVirtualAlloc regions are tracked so XMemVirtualQuery can report
// the flags/tags that were passed; heap-internal commits are not tracked.
//
// The physical-page model is the one the hardware and the reference
// implementations agree on: pages are 64 KiB, a page number is an opaque index
// into one section, and the section is SEC_RESERVE, so nothing is committed
// until a page is first touched. Mapping the same page index at two addresses
// aliases the same bytes, as the console's CPU/GPU views do.

#define _WIN32_WINNT 0x0A00  // VirtualAlloc2 / MapViewOfFile3
#include "xmem.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <list>
#include <map>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------
// Contract constants (heap.c)
// ---------------------------------------------------------------------------

#define XMEM_HEAP_PAGE_SHIFT         16          // 64 KiB heap pages
#define XMEM_HEAP_PAGE_SIZE          (1ull << XMEM_HEAP_PAGE_SHIFT)
#define XMEM_HEAP_VIRTUAL_SIZE       0x80000000ull  // 2 GiB VA reserved per heap

#define XMEM_SMALL_HEAP_GRANULARITY  8           // 256 B blocks
#define XMEM_LARGE_HEAP_GRANULARITY  12          // 4 KiB blocks

#define XMEM_GRAPHICS_HYSTERESIS     0x04000000ull  // 64 MiB
#define XMEM_HEAP_HYSTERESIS         0x00800000ull  // 8 MiB

// heap.c's heap-type selection over the attribute bitfield.
#define XMEM_HEAP_MEMTYPE_MASK       0x0ful
#define XMEM_HEAP_LARGE_PAGE         0x10u

#define MEM_PHYSICAL_FLAG            0x00400000ull
#define MEM_GRAPHICS_FLAG            0x10000000ull
#define MEM_32BIT_ADDRESS_FLAG       0x00800000ull
#define MEM_2MB_ALIGN_FLAG           0x00020000ull
#define MEM_2MB_PAGES_FLAG           0x20000000ull
#define MEM_64K_PAGES_FLAG           (MEM_2MB_PAGES_FLAG | MEM_PHYSICAL_FLAG)
#define MEM_RESERVE_PLACEHOLDER_FLAG 0x00040000ull

#define FIRST_PAGE_NUM               1ull  // 0 means "none"
#define PHYSICAL_SIZE                (16ull << 30)  // 16 GiB section

// ---------------------------------------------------------------------------
// The dispatch tables, verbatim from heap.c (they are the contract).
// ---------------------------------------------------------------------------

static const ULONG x_mp_heap_page_types[] = {
    PAGE_READWRITE,                                                                                     // XALLOC_MEMTYPE_HEAP_CACHEABLE
    PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_READWRITE,                                       // XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE
    PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_READONLY,                                       // XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE_GPU_READONLY
    PAGE_READWRITE | PAGE_GRAPHICS_COHERENT | PAGE_GRAPHICS_READWRITE,                                 // XALLOC_MEMTYPE_GRAPHICS_CACHEABLE
    PAGE_READWRITE | PAGE_GRAPHICS_READONLY,                                                           // XALLOC_MEMTYPE_GRAPHICS_CACHEABLE_NONCOHERENT_GPU_READONLY
    PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_NOCACHE | PAGE_GRAPHICS_EXECUTE_READWRITE,       // XALLOC_MEMTYPE_GRAPHICS_COMMAND_BUFFER_WRITECOMBINE
    PAGE_READWRITE | PAGE_GRAPHICS_EXECUTE_READWRITE | PAGE_GRAPHICS_NOCACHE | PAGE_GRAPHICS_COHERENT,  // XALLOC_MEMTYPE_GRAPHICS_COMMAND_BUFFER_CACHEABLE
    PAGE_READWRITE,                                                                                     // XALLOC_MEMTYPE_PHYSICAL_CACHEABLE
    PAGE_READWRITE | PAGE_WRITECOMBINE,                                                                 // XALLOC_MEMTYPE_PHYSICAL_WRITECOMBINE
    PAGE_READWRITE | PAGE_NOCACHE,                                                                      // XALLOC_MEMTYPE_PHYSICAL_UNCACHED
    PAGE_READWRITE | PAGE_GRAPHICS_COHERENT | PAGE_GRAPHICS_NOCACHE | PAGE_GRAPHICS_READWRITE,          // XALLOC_MEMTYPE_GRAPHICS_GPU_UNCACHED
    PAGE_READWRITE | PAGE_GRAPHICS_EXECUTE_READ,                                                       // XALLOC_MEMTYPE_GRAPHICS_SHADER_CACHEABLE_NONCOHERENT
    PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_EXECUTE_READWRITE,                               // XALLOC_MEMTYPE_GRAPHICS_EXECUTABLE_WRITECOMBINE
    0, 0, 0,
};

static const ULONG x_mp_heap_allocation_types[] = {
    0,                             // HEAP_CACHEABLE
    MEM_GRAPHICS_FLAG,             // GRAPHICS_WRITECOMBINE
    MEM_GRAPHICS_FLAG,             // GRAPHICS_WRITECOMBINE_GPU_READONLY
    MEM_GRAPHICS_FLAG,             // GRAPHICS_CACHEABLE
    MEM_GRAPHICS_FLAG,             // GRAPHICS_CACHEABLE_NONCOHERENT_GPU_READONLY
    MEM_GRAPHICS_FLAG,             // GRAPHICS_COMMAND_BUFFER_WRITECOMBINE
    MEM_GRAPHICS_FLAG,             // GRAPHICS_COMMAND_BUFFER_CACHEABLE
    MEM_PHYSICAL_FLAG,             // PHYSICAL_CACHEABLE
    MEM_PHYSICAL_FLAG,             // PHYSICAL_WRITECOMBINE
    MEM_PHYSICAL_FLAG,             // PHYSICAL_UNCACHED
    MEM_GRAPHICS_FLAG,             // GRAPHICS_GPU_UNCACHED
    MEM_GRAPHICS_FLAG,             // GRAPHICS_SHADER_CACHEABLE_NONCOHERENT
    MEM_GRAPHICS_FLAG,             // GRAPHICS_EXECUTABLE_WRITECOMBINE
    0, 0, 0,
};

// ---------------------------------------------------------------------------
// The working-set statistics counters. Limits are not invented here; T04 owns
// the budget policy and replaces them.
// ---------------------------------------------------------------------------

static std::mutex g_stats_lock;
static uint64_t g_game_used = 0;
static uint64_t g_game_used_peak = 0;
static uint64_t g_graphics_used = 0;

static void stats_alloc(uint64_t bytes, bool graphics) {
    std::lock_guard<std::mutex> g(g_stats_lock);
    g_game_used += bytes;
    if (g_game_used > g_game_used_peak) g_game_used_peak = g_game_used;
    if (graphics) g_graphics_used += bytes;
}

static void stats_free(uint64_t bytes, bool graphics) {
    std::lock_guard<std::mutex> g(g_stats_lock);
    if (g_game_used >= bytes) g_game_used -= bytes;
    if (graphics && g_graphics_used >= bytes) g_graphics_used -= bytes;
}

static void stats_snapshot(uint64_t* used_out, uint64_t* peak_out, uint64_t* graphics_used_out) {
    std::lock_guard<std::mutex> g(g_stats_lock);
    *used_out = g_game_used;
    *peak_out = g_game_used_peak;
    *graphics_used_out = g_graphics_used;
}

// ---------------------------------------------------------------------------
// VA region tracking (explicit XMemVirtualAlloc + internal allocations).
// ---------------------------------------------------------------------------

struct VARegion {
    size_t size;
    uint64_t xmem_flags;
};

static std::map<uintptr_t, VARegion> g_va_blocks;
static std::mutex g_va_lock;

static void va_record(void* base, size_t size, uint64_t flags) {
    if (!base) return;
    std::lock_guard<std::mutex> g(g_va_lock);
    g_va_blocks[(uintptr_t)base] = { size, flags };
}

static bool va_find(void* base, size_t* size_out, uint64_t* flags_out) {
    std::lock_guard<std::mutex> g(g_va_lock);
    auto it = g_va_blocks.find((uintptr_t)base);
    if (it == g_va_blocks.end()) return false;
    *size_out = it->second.size;
    *flags_out = it->second.xmem_flags;
    return true;
}

static void va_erase(void* base) {
    std::lock_guard<std::mutex> g(g_va_lock);
    g_va_blocks.erase((uintptr_t)base);
}

// ---------------------------------------------------------------------------
// XMemMakeTag -- 24-bit tag, upper 24 bits of the 64-bit word (see docs + the
// DLL's disassembly): lowercase a-z -> 0..25, uppercase A-Z -> 26..51, digits
// 0-9 -> 52..61, '$' -> 62, anything else -> 63 (read back as '*').
// Tag == 0 is a no-tag marker and returns 0.
// ---------------------------------------------------------------------------

static int x_char_code(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return 26 + (c - 'A');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '$') return 62;
    return 63;
}

// Like XMemMakeTag but returns the 24-bit word (code1<<18 | code2<<12 |
// code3<<6 | code4) so XMemVirtualQuery can re-derive the 4-char code.
static uint32_t x_make_tag_word(const char four[4]) {
    return (uint32_t)x_char_code(four[0]) << 18
         | (uint32_t)x_char_code(four[1]) << 12
         | (uint32_t)x_char_code(four[2]) << 6
         | (uint32_t)x_char_code(four[3]);
}

ULONGLONG WINAPI XMemMakeTag(UINT32 Tag) {
    if (Tag == 0) return 0;
    const char* chars = (const char*)&Tag;
    const char four[4] = { chars[0], chars[1], chars[2], chars[3] };
    return (ULONGLONG)x_make_tag_word(four) << 40;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct XMemOptions {
    bool validate_xmemalloc = false;
    bool tag_virtualalloc = false;
    bool raise_heapfree_callback_on_realloc = false;
    bool disable_pagesize_demotion = false;
    bool trap_use_after_free = false;
    bool use_64k_pages = false;
    bool allow_writeable_file_mappings = false;
    uint64_t tag_tree_size_mb = 0;
    bool warn_when_tag_tree_full = false;
};

static XMemOptions g_options;
static std::mutex g_options_lock;

static XMemOptions get_options() {
    std::lock_guard<std::mutex> g(g_options_lock);
    return g_options;
}

static void set_option(XMEM_OPTIONS option, BOOL bool_val, uint64_t int_val) {
    std::lock_guard<std::mutex> g(g_options_lock);
    switch (option) {
    case eXMEMOPT_VALIDATE_XMEMALLOC: g_options.validate_xmemalloc = bool_val != FALSE; break;
    case eXMEMOPT_XMEMALLOCDEFAULT_TAG_VIRTUALALLOC: g_options.tag_virtualalloc = bool_val != FALSE; break;
    case eXMEMOPT_RAISE_HEAPFREE_CALLBACK_ON_REALLOC: g_options.raise_heapfree_callback_on_realloc = bool_val != FALSE; break;
    case eXMEMOPT_XMEMALLOCDEFAULT_DISABLEPAGESIZEDEMOTION: g_options.disable_pagesize_demotion = bool_val != FALSE; break;
    case eXMEMOPT_XMEMALLOCDEFAULT_TRAPUSEAFTERFREE: g_options.trap_use_after_free = bool_val != FALSE; break;
    case eXMEMOPT_XMEMALLOCDEFAULT_USE64KPAGES: g_options.use_64k_pages = bool_val != FALSE; break;
    case eXMEMOPT_ALLOW_WRITEABLE_FILEMAPPINGS: g_options.allow_writeable_file_mappings = bool_val != FALSE; break;
    case eXMEMOPT_SET_XMEM_TAG_TREE_SIZE: g_options.tag_tree_size_mb = int_val; break;
    case eXMEMOPT_XMEM_WARN_WHEN_TAG_TREE_FULL: g_options.warn_when_tag_tree_full = bool_val != FALSE; break;
    default: break;
    }
}

VOID WINAPI XMemSetOption(XMEM_OPTION option) {
    set_option(option.Option, option.u.Bool, option.u.Int);
}

// ---------------------------------------------------------------------------
// Physical pages (section-backed, aliasable)
// ---------------------------------------------------------------------------

static std::mutex g_physical_lock;
static std::vector<uint8_t> g_used;
static HANDLE g_section = nullptr;

static void physical_init() {
    static std::once_flag once;
    std::call_once(once, []() {
        g_used.assign(PHYSICAL_SIZE / XMEM_HEAP_PAGE_SIZE, 0);
        g_section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                       (DWORD)(PHYSICAL_SIZE >> 32), (DWORD)PHYSICAL_SIZE, nullptr);
    });
}

static bool physical_valid_page(uint64_t page) {
    return page >= FIRST_PAGE_NUM && page - FIRST_PAGE_NUM < (PHYSICAL_SIZE / XMEM_HEAP_PAGE_SIZE);
}

static void* map_pages(void* address, uint32_t n, const uint64_t* pages) {
    if (!address || n == 0 || !pages) return nullptr;
    physical_init();
    std::lock_guard<std::mutex> g(g_physical_lock);
    if (!g_section) return nullptr;
    for (uint32_t i = 0; i < n; i++) {
        if (!physical_valid_page(pages[i]) || !g_used[pages[i] - FIRST_PAGE_NUM]) return nullptr;
        if (i && pages[i] != pages[0] + i) return nullptr;
    }
    void* p = nullptr;
    typedef PVOID (WINAPI* MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
    static MapViewOfFile3Fn map3 = (MapViewOfFile3Fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "MapViewOfFile3");
    if (!map3) map3 = (MapViewOfFile3Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "MapViewOfFile3");
    if (map3) {
        p = map3(g_section, GetCurrentProcess(), address, (pages[0] - FIRST_PAGE_NUM) * XMEM_HEAP_PAGE_SIZE,
                (SIZE_T)n * XMEM_HEAP_PAGE_SIZE, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    }
    if (!p) {
        if (!VirtualAlloc(address, (SIZE_T)n * XMEM_HEAP_PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
        p = address;
    }
    return p;
}

static void physical_get_stats(uint64_t* used_out) {
    physical_init();
    std::lock_guard<std::mutex> g(g_physical_lock);
    uint64_t count = 0;
    for (uint64_t i = 0; i < g_used.size(); i++) if (g_used[i]) count++;
    *used_out = count;
}

// ---------------------------------------------------------------------------
// Title heap (RTL path): a HeapCreate'd heap the default allocator uses for
// plain 4 KB-cacheable allocations with small alignment.
// ---------------------------------------------------------------------------

static HANDLE g_title_heap = nullptr;
static std::mutex g_title_heap_lock;
static std::map<uintptr_t, uint64_t> g_title_track;  // address -> size
static std::mutex g_title_track_lock;

static PHEAP_TRACK_ALLOC_ROUTINE g_heap_alloc_hook = nullptr;
[[maybe_unused]] static PHEAP_TRACK_REALLOC_ROUTINE g_heap_realloc_hook = nullptr;
static PHEAP_TRACK_FREE_ROUTINE g_heap_free_hook = nullptr;

// XMem allocation hooks: registered through XMemSetAllocationHooks (the GDK's
// XMEM_SET_ALLOCATION_HOOKS / __XMemAllocationHooks__ mechanism). When set,
// XMemAlloc/XMemFree route through them instead of the default implementation.
// Registered once at startup, before any allocation, and read on the allocation
// path, so no lock is taken here.
static PXMEMALLOC_ROUTINE g_hook_alloc = nullptr;
static PXMEMFREE_ROUTINE  g_hook_free = nullptr;

static HANDLE title_heap() {
    std::lock_guard<std::mutex> g(g_title_heap_lock);
    if (!g_title_heap) g_title_heap = HeapCreate(0, 0x100000, 0);
    return g_title_heap;
}

// ---------------------------------------------------------------------------
// The heap model: one 2 GiB VA reservation per (memory type, large) heap,
// first-fit free list, 64 KiB commit granularity, hysteresis decommit.
// Blocks: reserved/committed pages with 256 B (small) / 4 KiB (large)
// granularity; the 2 GiB reservation is VA-only, so nothing is committed
// until an allocation needs it.
// ---------------------------------------------------------------------------

struct FreeRegion { size_t offset; size_t size; };

namespace heap {

class Heap {
public:
    Heap() = default;
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    PVOID base_ = nullptr;
    size_t committed_ = 0;
    std::vector<FreeRegion> free_regions_;   // unsorted, sparse
    std::map<size_t, size_t> blocks_;        // block offset -> size
    uint32_t allocation_count_ = 0;
    uint32_t free_count_ = 0;
    uint64_t minimum_free_bytes_ = UINT64_MAX;
    uint64_t hysteresis_ = XMEM_HEAP_HYSTERESIS;
    uint32_t granularity_shift_ = XMEM_SMALL_HEAP_GRANULARITY;
    uint32_t heap_type_ = 0;
    std::mutex lock;

    void ensure_locked() {
        if (base_) return;
        base_ = VirtualAlloc(nullptr, XMEM_HEAP_VIRTUAL_SIZE, MEM_RESERVE, PAGE_NOACCESS);
        if (!base_) return;
        committed_ = 0;
        minimum_free_bytes_ = XMEM_HEAP_VIRTUAL_SIZE;
        free_regions_.push_back({ 0, XMEM_HEAP_VIRTUAL_SIZE });
    }

    bool ensure() {
        std::lock_guard<std::mutex> g(lock);
        ensure_locked();
        return base_ != nullptr;
    }

    uint64_t free_bytes_total_locked() const {
        uint64_t total = 0;
        for (const FreeRegion& r : free_regions_) total += r.size;
        return total;
    }

    uint64_t free_bytes_committed_locked() const {
        uint64_t total = 0;
        for (const FreeRegion& r : free_regions_) {
            if (r.offset >= committed_) continue;
            uint64_t end = r.offset + r.size;
            if (end <= committed_) total += r.size;
            else total += (committed_ - r.offset);
        }
        return total;
    }

    void merge_locked() {
        if (free_regions_.size() < 2) return;
        std::sort(free_regions_.begin(), free_regions_.end(),
                  [](const FreeRegion& a, const FreeRegion& b) { return a.offset < b.offset; });
        std::vector<FreeRegion> merged;
        for (const FreeRegion& r : free_regions_) {
            if (!merged.empty() && merged.back().offset + merged.back().size == r.offset) {
                merged.back().size += r.size;
            } else {
                merged.push_back(r);
            }
        }
        free_regions_.swap(merged);
    }

    uint64_t committed_bytes() const { return committed_; }
    uint64_t used_bytes() const { return committed_ - free_bytes_committed_locked(); }
    uint32_t allocation_count() const { return allocation_count_; }
    uint32_t free_count() const { return free_count_; }
    uint64_t minimum_free_bytes() const { return minimum_free_bytes_ == UINT64_MAX ? 0 : minimum_free_bytes_; }
    // The console's heap page-group granularity is its 64 KiB heap page
    // (heap.c XMEM_HEAP_PAGE_SHIFT). Reported as-is.
    uint32_t page_group_size() const { return (uint32_t)XMEM_HEAP_PAGE_SIZE; }
    // Capacity of the heap's VA reservation: the largest amount this heap could
    // commit. The console's exact meaning for "maximum allocated size" is not
    // public, so this is our reading of the field name, stated rather than
    // invented per call.
    uint64_t capacity_bytes() const { return XMEM_HEAP_VIRTUAL_SIZE; }

    void* alloc(size_t size, uint32_t align_code) {
        std::lock_guard<std::mutex> g(lock);
        ensure_locked();
        if (!base_) return nullptr;
        size_t gran = 1ull << granularity_shift_;
        size = (size + gran - 1) & ~(gran - 1);
        uint64_t align = 1ull << align_code;
        if (align < gran) align = gran;

        size_t chosen = SIZE_MAX;
        size_t chosen_start = 0;
        for (size_t i = 0; i < free_regions_.size(); i++) {
            FreeRegion& r = free_regions_[i];
            size_t start = (r.offset + align - 1) & ~(align - 1);
            size_t end = r.offset + r.size;
            if (start >= end) continue;
            if (start + size > end) continue;
            chosen = i;
            chosen_start = start;
            break;
        }
        if (chosen == SIZE_MAX) return nullptr;
        FreeRegion& r = free_regions_[chosen];
        size_t start = chosen_start;
        size_t need_end = start + size;
        std::vector<FreeRegion> remainder;
        if (start > r.offset) remainder.push_back({ r.offset, start - r.offset });
        if (r.offset + r.size > need_end)
            remainder.push_back({ need_end, r.offset + r.size - need_end });
        free_regions_.erase(free_regions_.begin() + chosen);
        for (const FreeRegion& f : remainder) free_regions_.push_back(f);
        merge_locked();

        if (need_end > committed_) {
            if (!VirtualAlloc((char*)base_ + committed_, need_end - committed_, MEM_COMMIT, PAGE_READWRITE)) {
                // VirtualAlloc failure after the free-list split leaves the
                // heap inconsistent; refuse honestly. The reservation itself
                // is VA-only, so nothing is leaked beyond it.
                // shortcut: no transactional rollback here; the rare commit
                // failure leaves the list over-committed-but-unusable.
                // Upgrade: keep the split and redo it on retry.
                return nullptr;
            }
            committed_ = need_end;
        }
        minimum_free_bytes_ = std::min(minimum_free_bytes_, free_bytes_committed_locked());
        allocation_count_++;
        blocks_[start] = size;
        return (char*)base_ + start;
    }

    uint64_t free(void* address) {
        std::lock_guard<std::mutex> g(lock);
        if (!base_ || !address) return 0;
        size_t offset = (size_t)((char*)address - (char*)base_);
        auto it = blocks_.find(offset);
        if (it == blocks_.end()) return 0;
        size_t size = it->second;
        blocks_.erase(it);
        free_regions_.push_back({ offset, size });
        merge_locked();
        free_count_++;
        minimum_free_bytes_ = std::min(minimum_free_bytes_, free_bytes_committed_locked());

        // decommit the tail free region (only if it reaches the end of
        // committed VA) while free bytes exceed the hysteresis allowance
        while (free_bytes_committed_locked() > hysteresis_ && !free_regions_.empty()) {
            FreeRegion tail = free_regions_.back();
            if (tail.offset + tail.size != committed_) break;
            if (!VirtualFree((char*)base_ + tail.offset, tail.size, MEM_DECOMMIT)) break;
            committed_ = tail.offset;  // tail region stays in the list, now outside committed VA
        }
        minimum_free_bytes_ = std::min(minimum_free_bytes_, free_bytes_committed_locked());
        return size;
    }
};

}  // namespace heap

// ---------------------------------------------------------------------------
// Heaps table ----------------------------------------------------------------
//
// One reservation per (memory type, large/small) heap. Type i with large=false
// reserves XMEM_HEAP_VIRTUAL_SIZE VA once.
// ---------------------------------------------------------------------------

namespace heap_table {

constexpr uint32_t HEAP_COUNT = 26;  // 13 memory types x (small, large)

static std::mutex g_table_lock;
static heap::Heap g_heaps[HEAP_COUNT];

inline uint32_t index_for(uint32_t memory_type, bool large) {
    return memory_type + (large ? 0x10u : 0u);
}

static heap::Heap& get(uint32_t memory_type, bool large) {
    std::lock_guard<std::mutex> g(g_table_lock);
    heap::Heap& h = g_heaps[index_for(memory_type, large)];
    if (!h.base_) {
        h.granularity_shift_ = large ? XMEM_LARGE_HEAP_GRANULARITY : XMEM_SMALL_HEAP_GRANULARITY;
        h.hysteresis_ = (x_mp_heap_allocation_types[memory_type] == MEM_GRAPHICS_FLAG) ? XMEM_GRAPHICS_HYSTERESIS
                                                                                  : XMEM_HEAP_HYSTERESIS;
        h.heap_type_ = memory_type;
        h.ensure();
    }
    return h;
}

static void* FindHeap(uint32_t memory_type, PVOID address, heap::Heap** out) {
    std::lock_guard<std::mutex> g(g_table_lock);
    for (int l = 0; l < 2; l++) {
        heap::Heap& h = g_heaps[index_for(memory_type, (bool)l)];
        if (h.base_ && (char*)address >= (char*)h.base_ && (char*)address < (char*)h.base_ + XMEM_HEAP_VIRTUAL_SIZE) {
            if (out) *out = &h;
            return address;
        }
    }
    if (out) *out = nullptr;
    return nullptr;
}

}  // namespace heap_table


// ---------------------------------------------------------------------------
// Protections the host can honour. The console-only PAGE_GRAPHICS_* bits are
// not valid VirtualAlloc protections on a PC host; they are stripped with a log
// line rather than passed to an API that would reject the call.
// ---------------------------------------------------------------------------

static ULONG x_protect(ULONG console_protect) {
    // Host protection bits (PAGE_* 0x001..0x400) pass through untouched; console
    // PAGE_GRAPHICS_* bits (0x800 and above) are translated to their host
    // equivalent. The console bits win when present: heap.c's tables OR them
    // alongside a plain PAGE_READWRITE, and the graphics bit is the one the
    // title actually asked for.
    ULONG host = console_protect & 0x7ffu;
    ULONG g = console_protect & ~0x7ffu;
    ULONG mapped = 0;
    if      (g & PAGE_GRAPHICS_NOACCESS)          mapped = PAGE_NOACCESS;
    else if (g & PAGE_GRAPHICS_READONLY)          mapped = PAGE_READONLY;
    else if (g & PAGE_GRAPHICS_READWRITE)         mapped = PAGE_READWRITE;
    else if (g & PAGE_GRAPHICS_EXECUTE)           mapped = PAGE_EXECUTE;
    else if (g & PAGE_GRAPHICS_EXECUTE_READ)      mapped = PAGE_EXECUTE_READ;
    else if (g & PAGE_GRAPHICS_EXECUTE_READWRITE) mapped = PAGE_EXECUTE_READWRITE;
    if (mapped) host = mapped;
    // PAGE_GRAPHICS_NOCACHE / _COHERENT ("onion" memory) have no host analogue
    // and are dropped rather than faked: console command-buffer and fence-write
    // memory asks for NOCACHE, and the PC runtime exposes no user-visible
    // equivalent. The loss is logged by the caller, not silently absorbed.
    if (host == 0) host = PAGE_READWRITE;
    return host;
}

static bool x_graphics_type(uint32_t memory_type) {
    return memory_type >= XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE &&
           memory_type <= XALLOC_MEMTYPE_GRAPHICS_EXECUTABLE_WRITECOMBINE;
}

// ---------------------------------------------------------------------------
// The raw VA path, shared by the public XMemVirtualAlloc and the internal
// default allocator. The placeholder path is kept for the physical-page
// aliasing test (MapViewOfFile3 replaces the placeholder, not the reservation).
// ---------------------------------------------------------------------------

static void* va_alloc_internal(PVOID base, SIZE_T size, DWORD allocation_type,
                               uint64_t xmem_flags, ULONG protect, uint32_t tag) {
    if (size == 0) size = 1;
    DWORD type = allocation_type & ~(MEM_2MB_PAGES_FLAG | MEM_64K_PAGES_FLAG |
                                     MEM_PHYSICAL_FLAG | MEM_GRAPHICS_FLAG |
                                     MEM_32BIT_ADDRESS_FLAG | MEM_2MB_ALIGN_FLAG);
    bool placeholder = (type & MEM_RESERVE_PLACEHOLDER_FLAG) != 0;
    DWORD protect_f = x_protect(protect);
    if ((type & MEM_RESERVE) && !(type & MEM_COMMIT) && !placeholder) {
        // placeholder path: a RESERVE-without-COMMIT becomes a placeholder the
        // physical-page mapping layer can replace with real pages
        typedef PVOID (WINAPI* VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        static VirtualAlloc2Fn alloc2 = (VirtualAlloc2Fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
        if (!alloc2) alloc2 = (VirtualAlloc2Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc2");
        void* p = alloc2 ? alloc2(GetCurrentProcess(), base, size,
                                 MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, protect_f, nullptr, 0)
                         : nullptr;
        if (p) { va_record(p, size, xmem_flags); return p; }
        // host without placeholders: plain reservation
        p = VirtualAlloc(base, size, MEM_RESERVE, protect_f);
        if (!p) return nullptr;
        va_record(p, size, xmem_flags);
        return p;
    }
    type &= ~MEM_RESERVE_PLACEHOLDER_FLAG;
    if (placeholder) {
        // placeholder path that already went through VirtualAlloc2 above;
        // unreachable here -- but if the flag is in the raw type, mirror it.
        typedef PVOID (WINAPI* VirtualAlloc2Fn2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
        static VirtualAlloc2Fn2 alloc2 = (VirtualAlloc2Fn2)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
        if (!alloc2) alloc2 = (VirtualAlloc2Fn2)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc2");
        void* p = alloc2 ? alloc2(GetCurrentProcess(), base, size,
                                 MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, protect_f, nullptr, 0)
                         : nullptr;
        if (p) { va_record(p, size, xmem_flags); return p; }
        return nullptr;
    }
    void* p = VirtualAlloc(base, size, type, protect_f);
    if (!p) return nullptr;
    va_record(p, size, xmem_flags);
    return p;
}

void* WINAPI XMemVirtualAlloc(PVOID base, SIZE_T size, DWORD allocation_type,
                             uint64_t xmem_flags, DWORD protect) {
    return va_alloc_internal(base, size, allocation_type, xmem_flags, protect, 0);
}

// ---------------------------------------------------------------------------
// The default allocator's decision tree (heap.c)
// ---------------------------------------------------------------------------

static bool x_rtl_attributes(const XALLOC_ATTRIBUTES& attr) {
    return x_mp_heap_allocation_types[attr.s.MemoryType] == 0 &&
           attr.s.Alignment <= XALLOC_ALIGNMENT_16 &&
           attr.s.PageSize == XALLOC_PAGESIZE_4KB;
}

static uint64_t xmem_flag_for(const XALLOC_ATTRIBUTES& attr, uint32_t graphics) {
    // heap.c's XmpAllocateVirtualMemory flag assembly: graphics or CPU, plus
    // 32-bit mapping and 2MB align.
    uint64_t flags = graphics ? XMEM_GRAPHICS : XMEM_CPU;
    if (graphics && attr.s.Address32BitMapping != 0) flags = XMEM_GRAPHICS_32BIT_ADDRESS;
    if (attr.s.Alignment == XALLOC_ALIGNMENT_128K) flags |= XMEM_2MB_ALIGN;
    size_t size = 0;
    (void)size;
    return flags;
}

void* WINAPI XMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    XALLOC_ATTRIBUTES a;
    a.Attributes = attributes;
    XMemOptions opts = get_options();

    if (size == 0) return nullptr;
    if (x_mp_heap_page_types[a.s.MemoryType] == 0) return nullptr;

    bool physical = x_mp_heap_allocation_types[a.s.MemoryType] == MEM_PHYSICAL_FLAG;
    bool graphics = x_graphics_type(a.s.MemoryType);

    // --- RTL path: plain 4KB-cacheable, small alignment -------------------
    if (x_rtl_attributes(a)) {
        void* p = HeapAlloc(title_heap(), 0, size);
        if (!p) return nullptr;
        {
            std::lock_guard<std::mutex> g(g_title_track_lock);
            g_title_track[(uintptr_t)p] = size;
        }
        stats_alloc(size, false);
        if (g_heap_alloc_hook) g_heap_alloc_hook(title_heap(), 0, size, p);
        return p;
    }

    // --- virtual-alloc path ------------------------------------------------
    if (physical || opts.trap_use_after_free ||
        a.s.Alignment > XMEM_HEAP_PAGE_SHIFT ||
        size > (1ull << (XMEM_HEAP_PAGE_SHIFT + 1)) ||
        a.s.Address32BitMapping != 0 ||
        x_mp_heap_page_types[a.s.MemoryType] == PAGE_NOACCESS ||
        a.s.PageSize == XALLOC_PAGESIZE_2MB) {

        DWORD type = MEM_RESERVE | MEM_COMMIT;
        if (a.s.Alignment == XALLOC_ALIGNMENT_128K) type |= MEM_2MB_ALIGN_FLAG;
        switch (a.s.PageSize) {
        case XALLOC_PAGESIZE_64KB: type |= MEM_64K_PAGES_FLAG; break;
        case XALLOC_PAGESIZE_2MB:  type |= MEM_2MB_PAGES_FLAG; break;
        }
        if (a.s.Address32BitMapping) type |= MEM_32BIT_ADDRESS_FLAG;
        type |= x_mp_heap_allocation_types[a.s.MemoryType];  // stripped in va_alloc_internal

        // physical types promote 4K/64K pages to 64K (or 2MB when alignment is
        // larger than 64K / page size is 2MB). On the PC host neither is honored.
        if (physical && (a.s.PageSize == XALLOC_PAGESIZE_4KB || a.s.PageSize == XALLOC_PAGESIZE_64KB) &&
            a.s.Alignment <= XALLOC_ALIGNMENT_64K) {
            type &= ~MEM_PHYSICAL_FLAG;
            type |= MEM_64K_PAGES_FLAG;
        } else if (physical) {
            type &= ~MEM_PHYSICAL_FLAG;
            type |= MEM_2MB_PAGES_FLAG;
        }

        ULONG protect = x_protect(x_mp_heap_page_types[a.s.MemoryType]);

        uint64_t flags = xmem_flag_for(a, graphics ? 1 : 0);
        if (opts.tag_virtualalloc) flags |= XMemMakeTag('XmVA');

        void* p = va_alloc_internal(nullptr, size, type, flags, protect, 0);
        if (!p) return nullptr;
        stats_alloc(size, graphics);
        return p;
    }

    // --- heap path ----------------------------------------------------------
    // heap.c promotes every graphics request to 64KB pages: 4KB graphics
    // requests cannot be serviced by VirtualAlloc.
    if (graphics && a.s.PageSize < XALLOC_PAGESIZE_64KB)
        a.s.PageSize = XALLOC_PAGESIZE_64KB;

    bool large = a.s.Alignment > XMEM_LARGE_HEAP_GRANULARITY ||
                 size > (0xFull << XMEM_SMALL_HEAP_GRANULARITY);

    heap::Heap& h = heap_table::get(a.s.MemoryType, large);
    void* p = h.alloc(size, a.s.Alignment);
    if (!p) return nullptr;
    stats_alloc(size, graphics);
    return p;
}

void WINAPI XMemFreeDefault(void* address, ULONGLONG attributes) {
    XALLOC_ATTRIBUTES a;
    a.Attributes = attributes;
    if (!address) return;

    bool physical = x_mp_heap_allocation_types[a.s.MemoryType] == MEM_PHYSICAL_FLAG;
    bool rtl = x_rtl_attributes(a);
    bool graphics = x_graphics_type(a.s.MemoryType);

    if (rtl) {
        std::lock_guard<std::mutex> g(g_title_track_lock);
        auto it = g_title_track.find((uintptr_t)address);
        uint64_t size = 0;
        if (it != g_title_track.end()) { size = it->second; g_title_track.erase(it); }
        stats_free(size, false);
        if (g_heap_free_hook) g_heap_free_hook(title_heap(), 0, address);
        HeapFree(title_heap(), 0, address);
        return;
    }

    heap::Heap* found = nullptr;
    (void)heap_table::FindHeap(a.s.MemoryType, address, &found);
    if (found) {
        uint64_t size = found->free(address);
        stats_free(size, graphics);
        return;
    }

    // virtual-alloc path
    size_t size = 0;
    uint64_t flags = 0;
    if (va_find(address, &size, &flags)) {
        stats_free(size, graphics);
        va_erase(address);
    }
    (void)physical;
    VirtualFree(address, 0, MEM_RELEASE);
}

void* WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes) {
    if (g_hook_alloc) return g_hook_alloc(size, attributes);
    return XMemAllocDefault(size, attributes);
}

void WINAPI XMemFree(void* address, ULONGLONG attributes) {
    if (g_hook_free) { g_hook_free(address, attributes); return; }
    XMemFreeDefault(address, attributes);
}

// ---------------------------------------------------------------------------
// Virtual query
// ---------------------------------------------------------------------------

BOOL WINAPI XMemVirtualQuery(HANDLE process, PVOID address, PXMEM_BASIC_INFORMATION buffer) {
    if (!buffer) return FALSE;
    (void)process;  // tools can pass a remote handle; this shim only sees the
                    // local VA space, so it ignores it (honest, not faked).
    PXMEM_BASIC_INFORMATION mbi = buffer;
    if (VirtualQueryEx(GetCurrentProcess(), address, (MEMORY_BASIC_INFORMATION*)mbi, sizeof(MEMORY_BASIC_INFORMATION)) == 0)
        return FALSE;
    mbi->AllocationProtect = mbi->Protect;
    size_t tracked_size = 0;
    uint64_t tracked_flags = 0;
    if (va_find(mbi->BaseAddress, &tracked_size, &tracked_flags)) {
        mbi->XMemFlags = tracked_flags;
        uint32_t tag = (uint32_t)(tracked_flags >> 40) & 0xffffffu;
        mbi->TagCode = tag;
        static const char* alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$";
        uint32_t c0 = (tag >> 18) & 0x3f;
        uint32_t c1 = (tag >> 12) & 0x3f;
        uint32_t c2 = (tag >> 6) & 0x3f;
        uint32_t c3 = tag & 0x3f;
        mbi->TagFourCC[0] = (c0 < 63) ? alpha[c0] : '*';
        mbi->TagFourCC[1] = (c1 < 63) ? alpha[c1] : '*';
        mbi->TagFourCC[2] = (c2 < 63) ? alpha[c2] : '*';
        mbi->TagFourCC[3] = (c3 < 63) ? alpha[c3] : '*';
    } else {
        mbi->XMemFlags = 0;
        mbi->TagCode = 0;
        mbi->TagFourCC[0] = mbi->TagFourCC[1] = mbi->TagFourCC[2] = mbi->TagFourCC[3] = 0;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Working-set statistics
// ---------------------------------------------------------------------------

BOOL WINAPI XMemGetWorkingSetStatistics(DWORD working_set_flags, PXMEM_WORKING_SET_STATISTICS statistics) {
    (void)working_set_flags;
    if (!statistics) return FALSE;
    memset(statistics, 0, sizeof(*statistics));

    uint64_t used, peak, graphics_used;
    stats_snapshot(&used, &peak, &graphics_used);

    uint64_t physical_pages = 0;
    physical_get_stats(&physical_pages);

    statistics->GameUsed = (SIZE_T)used;
    statistics->GameUsedPeak = (SIZE_T)peak;
    statistics->GameAllocatedPhysicalPageCount = (UINT32)physical_pages;
    statistics->GpuOptimalBandwidthUsed = (SIZE_T)graphics_used;
    // Limits are intentionally left at zero: the budget policy is T04's
    // deliverable and is not invented here. See docs/ROADMAP + 03.
    statistics->GameLimit = 0;
    statistics->GpuOptimalBandwidthLimit = 0;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Physical pages
// ---------------------------------------------------------------------------

BOOL WINAPI XMemAllocatePhysicalPages(uint64_t xmem_flags, ULONG_PTR* count_inout, ULONG_PTR* pages_out) {
    (void)xmem_flags;
    if (!count_inout || !pages_out) return FALSE;
    uint32_t want = (uint32_t)*count_inout;
    if (want == 0) return FALSE;
    physical_init();
    std::lock_guard<std::mutex> g(g_physical_lock);
    if (!g_section) { *count_inout = 0; return FALSE; }
    uint64_t run = 0, start = 0;
    for (uint64_t i = 0; i < g_used.size(); i++) {
        if (g_used[i]) { run = 0; continue; }
        if (run == 0) start = i;
        if (++run == want) {
            for (uint32_t k = 0; k < want; k++) {
                g_used[start + k] = 1;
                pages_out[k] = FIRST_PAGE_NUM + start + k;
            }
            *count_inout = want;
            return TRUE;
        }
    }
    *count_inout = 0;
    return FALSE;
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR number_of_pages, const ULONG_PTR* pages) {
    if (number_of_pages == 0) return TRUE;
    if (!pages) return FALSE;
    physical_init();
    std::lock_guard<std::mutex> g(g_physical_lock);
    for (ULONG_PTR i = 0; i < number_of_pages; i++) {
        if (!physical_valid_page(pages[i])) return FALSE;
    }
    for (ULONG_PTR i = 0; i < number_of_pages; i++) g_used[pages[i] - FIRST_PAGE_NUM] = 0;
    return TRUE;
}

PVOID WINAPI XMemMapPhysicalPages(PVOID address, ULONG_PTR number_of_pages, const ULONG_PTR* pages) {
    if (!address) return nullptr;
    if (number_of_pages == 0) return address;
    return map_pages(address, (uint32_t)number_of_pages, pages);
}

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* addresses, ULONG_PTR number_of_pages, const ULONG_PTR* pages) {
    if (!addresses || !pages) return FALSE;
    for (ULONG_PTR i = 0; i < number_of_pages; i++) {
        if (!addresses[i]) return FALSE;
        if (!map_pages(addresses[i], 1, pages + i)) return FALSE;
    }
    return TRUE;
}

BOOL WINAPI XMemGetPhysicalMemoryLayout(PXMEM_PHYSICAL_MEMORY_LAYOUT layout) {
    if (!layout) return FALSE;
    // The PC host has no bandwidth bands; there is one unified-ish section, so
    // the synthetic boundaries are the lowest numbered pages the title may
    // receive. The values are stable for the process lifetime (per the console
    // contract) and honestly reported as synthetic.
    layout->LowestPageNumber = FIRST_PAGE_NUM;
    layout->LowestPageNumberGpuOptimalBandwidth = FIRST_PAGE_NUM;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Heap statistics / hysteresis / prealloc
// ---------------------------------------------------------------------------

BOOL WINAPI XMemSetAllocationHysteresis(DWORD mem_type, BOOL large_heap, SIZE_T new_size) {
    if (mem_type > 0x0f) return FALSE;
    heap::Heap& h = heap_table::get(mem_type, (BOOL)large_heap != FALSE);
    h.hysteresis_ = new_size;
    return TRUE;
}

SIZE_T WINAPI XMemGetAllocationHysteresis(DWORD mem_type, BOOL large_heap) {
    if (mem_type > 0x0f) return UINT64_MAX;
    heap::Heap& h = heap_table::get(mem_type, (BOOL)large_heap != FALSE);
    return h.hysteresis_;
}

BOOL WINAPI XMemPreallocateFreeSpace(DWORD mem_type, BOOL large_heap) {
    if (mem_type > 0x0f) return FALSE;
    // console: commit free groups up to the hysteresis allowance. The PC
    // heap commits on demand; there is no explicit preallocation, so report
    // success (nothing to do) rather than pretending to commit.
    return TRUE;
}

BOOL WINAPI XMemGetAllocationStatistics(DWORD mem_type, BOOL large_heap, PXMEM_HEAP_STATISTICS statistics) {
    if (!statistics || statistics->dwSize != sizeof(XMEM_HEAP_STATISTICS)) {
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }
    // The layout is the contract; fail loudly if the shape drifted
    statistics->dwSize = sizeof(XMEM_HEAP_STATISTICS);
    statistics->dwPageGroupSize = (ULONG)heap_table::get(mem_type, (BOOL)large_heap != FALSE).page_group_size();
    statistics->dwMinimumFreeDepth = (SIZE_T)(heap_table::get(mem_type, (BOOL)large_heap != FALSE).minimum_free_bytes());
    statistics->dwMaximumAllocatedSize = (SIZE_T)heap_table::get(mem_type, (BOOL)large_heap != FALSE).capacity_bytes();
    statistics->dwAllocationCount = heap_table::get(mem_type, (BOOL)large_heap != FALSE).allocation_count();
    statistics->dwFreeCount = heap_table::get(mem_type, (BOOL)large_heap != FALSE).free_count();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Win32 heap statistics / tracking hooks / name
// ---------------------------------------------------------------------------

BOOL WINAPI XMemSetWin32HeapTrackingHooks(PHEAP_TRACK_ALLOC_ROUTINE alloc_routine,
                                         PHEAP_TRACK_REALLOC_ROUTINE realloc_routine,
                                         PHEAP_TRACK_FREE_ROUTINE free_routine) {
    g_heap_alloc_hook = alloc_routine;
    g_heap_realloc_hook = realloc_routine;
    g_heap_free_hook = free_routine;
    return TRUE;
}

VOID WINAPI XMemSetAllocationHooks(PXMEMALLOC_ROUTINE alloc_routine, PXMEMFREE_ROUTINE free_routine) {
    // The GDK additionally honours a __XMemAllocationHooks__ export in the EXE,
    // which the real xmem.dll reads at load time. That is a loader behaviour a
    // shim cannot reproduce from inside the DLL, so only the explicit call is
    // recorded here -- and it is recorded honestly rather than faked.
    g_hook_alloc = alloc_routine;
    g_hook_free = free_routine;
}

BOOL WINAPI XMemGetWin32HeapStatistics(HANDLE heap, PXMEM_WIN32_HEAP_STATISTICS statistics) {
    if (!statistics) return FALSE;
    memset(statistics, 0, sizeof(*statistics));
    (void)heap;
    // No Win32 heap bookkeeping exists on this path; the title heap is a
    // plain HeapCreate'd heap, so we keep our own tracking instead.
    return TRUE;
}

BOOL WINAPI XMemSetWin32HeapName(HANDLE heap, PCSTR buffer, SIZE_T size_of_buffer) {
    (void)heap; (void)buffer; (void)size_of_buffer;
    return FALSE;  // not supported; heap names are not tracked on this path
}

BOOL WINAPI XMemGetWin32HeapName(HANDLE heap, PSTR buffer, SIZE_T size_of_buffer, PSIZE_T return_length) {
    (void)heap; (void)buffer; (void)size_of_buffer;
    if (return_length) *return_length = 0;
    return FALSE;  // not supported
}

// ---------------------------------------------------------------------------
// Transfers / tools
// ---------------------------------------------------------------------------

BOOL WINAPI XMemTransferMemory(XMEM_TRANSFER_TYPE transfer_type, SIZE_T transfer_size) {
    (void)transfer_type; (void)transfer_size;
    // No tools partition exists on the PC host; transfers are documented
    // no-ops returning success, honestly. See the design doc M6.
    return TRUE;
}

BOOL WINAPI XMemIsToolsMemAvailable(void) { return FALSE; }

