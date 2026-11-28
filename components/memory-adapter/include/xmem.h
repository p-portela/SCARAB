// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ===========================================================================
//  xmem.h -- the XMem contract, as shipped in the GDK.
//
//  This is a sanitised transcription of the console XMem API surface, kept to
//  the bit layouts the console implements (xmem.h in the GDK, plus the default
//  implementation's decision tree in heap.c). Where the default implementation
//  is only documented by behaviour, the doc is cited; where a value is not
//  public, it is left out rather than invented.
//
//  House vocabulary note: the DLL's own names (XMemAlloc, XALLOC_ATTRIBUTES,
//  ...) are the public contract and are kept as-is; SCARAB-internal names
//  stay SCARAB_*/scarab_*.
// ===========================================================================

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- page protections (console allocation classes) ---------------------------
//
// These are the platform's own values, taken from <windows.h> (winnt.h), exactly
// as the GDK's xmem.h does -- the console does not redefine them. Defining them
// here with different numbers would silently disagree with the runtime.
//   PAGE_GRAPHICS_NOACCESS 0x0800   READONLY 0x1000   READWRITE 0x2000
//   PAGE_GRAPHICS_EXECUTE  0x4000   EXECUTE_READ 0x8000  EXECUTE_READWRITE 0x10000
//   PAGE_GRAPHICS_COHERENT 0x20000
// NOCACHE is guarded only because older Windows headers omit it; the GDK carries
// the same guard.
#ifndef PAGE_GRAPHICS_NOCACHE
#define PAGE_GRAPHICS_NOCACHE           0x40000
#endif

// --- allocation attributes -------------------------------------------------

#define XALLOC_PAGESIZE_4KB      0
#define XALLOC_PAGESIZE_64KB     1
#define XALLOC_PAGESIZE_2MB      2

#define XALLOC_MEMTYPE_HEAP_CACHEABLE                                 0x0
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE                          0x1
#define XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE_GPU_READONLY             0x2
#define XALLOC_MEMTYPE_GRAPHICS_CACHEABLE                             0x3
#define XALLOC_MEMTYPE_GRAPHICS_CACHEABLE_NONCOHERENT_GPU_READONLY    0x4
#define XALLOC_MEMTYPE_GRAPHICS_COMMAND_BUFFER_WRITECOMBINE           0x5
#define XALLOC_MEMTYPE_GRAPHICS_COMMAND_BUFFER_CACHEABLE              0x6
#define XALLOC_MEMTYPE_PHYSICAL_CACHEABLE                             0x7
#define XALLOC_MEMTYPE_PHYSICAL_WRITECOMBINE                          0x8
#define XALLOC_MEMTYPE_PHYSICAL_UNCACHED                              0x9
#define XALLOC_MEMTYPE_GRAPHICS_GPU_UNCACHED                          0xA
#define XALLOC_MEMTYPE_GRAPHICS_SHADER_CACHEABLE_NONCOHERENT          0xB
#define XALLOC_MEMTYPE_GRAPHICS_EXECUTABLE_WRITECOMBINE               0xC

#define XALLOC_ALIGNMENT_ANY            0x0
#define XALLOC_ALIGNMENT_4              0x2
#define XALLOC_ALIGNMENT_8              0x3
#define XALLOC_ALIGNMENT_16             0x4
#define XALLOC_ALIGNMENT_32             0x5
#define XALLOC_ALIGNMENT_64             0x6
#define XALLOC_ALIGNMENT_128            0x7
#define XALLOC_ALIGNMENT_256            0x8
#define XALLOC_ALIGNMENT_512            0x9
#define XALLOC_ALIGNMENT_1K             0xA
#define XALLOC_ALIGNMENT_2K             0xB
#define XALLOC_ALIGNMENT_4K             0xC
#define XALLOC_ALIGNMENT_8K             0xD
#define XALLOC_ALIGNMENT_16K            0xE
#define XALLOC_ALIGNMENT_32K            0xF
#define XALLOC_ALIGNMENT_64K            0x10
#define XALLOC_ALIGNMENT_128K           0x11
#define XALLOC_ALIGNMENT_256K           0x12
#define XALLOC_ALIGNMENT_512K           0x13

#define XALLOC_ALLOCATOR_ID_GAME_MIN           0
#define XALLOC_ALLOCATOR_ID_GAME_MAX           127
#define XALLOC_ALLOCATOR_ID_PLATFORM_RESERVED_MIN 128
#define XALLOC_ALLOCATOR_ID_D3D         128
#define XALLOC_ALLOCATOR_ID_AUDIO      129
#define XALLOC_ALLOCATOR_ID_VIDEO      130
#define XALLOC_ALLOCATOR_ID_PLATFORM_RESERVED_MAX 191
#define XALLOC_ALLOCATOR_ID_MIDDLEWARE_RESERVED_MIN 192
#define XALLOC_ALLOCATOR_ID_MIDDLEWARE_RESERVED_MAX 255

typedef union _XALLOC_ATTRIBUTES {
    uint64_t Attributes;
    struct {
        uint64_t ObjectType:14;
        uint64_t PageSize:2;
        uint64_t AllocatorId:8;
        uint64_t Alignment:5;
        uint64_t MemoryType:4;
        uint64_t Address32BitMapping:1;
        uint64_t reserved:30;
    } s;
} XALLOC_ATTRIBUTES, *PXALLOC_ATTRIBUTES;

static inline uint64_t make_xalloc_attributes(uint64_t allocator_id, uint64_t object_type,
                                             uint64_t memory_type, uint64_t page_size,
                                             uint64_t alignment, uint64_t address_32_bit_mapping) {
    XALLOC_ATTRIBUTES a;
    a.s.ObjectType = object_type;
    a.s.PageSize = page_size;
    a.s.AllocatorId = allocator_id;
    a.s.Alignment = alignment;
    a.s.MemoryType = memory_type;
    a.s.Address32BitMapping = address_32_bit_mapping;
    a.s.reserved = 0;
    return a.Attributes;
}

// --- memory-allocation flags (XMemVirtualAlloc XMemFlags) ----------------------

#define XMEM_CPU                     0x00000001ULL
#define XMEM_GRAPHICS                0x00000003ULL
#define XMEM_GRAPHICS_32BIT_ADDRESS  0x00000007ULL

#define XMEM_TOOL                    0x00000010ULL
#define XMEM_MAPPABLE                0x00000020ULL
#define XMEM_2MB_ALIGN               0x00000040ULL

#define XMEM_STANDARD_BANDWIDTH_REQUIRED     0x00000080ULL
#define XMEM_STANDARD_BANDWIDTH_PREFERRED    0x00000180ULL
#define XMEM_GPU_OPTIMAL_BANDWIDTH_REQUIRED  0x00000280ULL
#define XMEM_GPU_OPTIMAL_BANDWIDTH_PREFERRED 0x00000380ULL

#define XMEM_STD_BW_REQ     (XMEM_STANDARD_BANDWIDTH_REQUIRED)
#define XMEM_STD_BW_PREF    (XMEM_STANDARD_BANDWIDTH_PREFERRED)
#define XMEM_GPU_OPTBW_REQ  (XMEM_GPU_OPTIMAL_BANDWIDTH_REQUIRED)
#define XMEM_GPU_OPTBW_PREF (XMEM_GPU_OPTIMAL_BANDWIDTH_PREFERRED)

#define XMEM_40BIT_ADDRESS           0x00007000ULL
#define XMEM_41BIT_ADDRESS           0x00006000ULL
#define XMEM_42BIT_ADDRESS           0x00005000ULL
#define XMEM_43BIT_ADDRESS           0x00004000ULL
#define XMEM_44BIT_ADDRESS           0x00003000ULL
#define XMEM_45BIT_ADDRESS           0x00002000ULL
#define XMEM_46BIT_ADDRESS           0x00001000ULL

// --- working-set statistics ----------------------------------------------------

typedef struct _XMEM_WORKING_SET_STATISTICS {
    SIZE_T GameLimit;
    SIZE_T GameUsed;
    SIZE_T ToolsLimit;
    SIZE_T ToolsUsed;
    UINT32 StackUsed;
    UINT32 GameAllocatedPhysicalPageCount;
    SIZE_T GpuOptimalBandwidthLimit;
    SIZE_T GpuOptimalBandwidthUsed;
    SIZE_T ToolsGpuOptimalBandwidthLimit;
    SIZE_T ToolsGpuOptimalBandwidthUsed;
    UINT32 KernelPoolUsed;
    UINT32 KernelPoolLimit;
    UINT32 DirectStorageUsed;
    UINT32 FramePreserveBufferSize;
    SIZE_T GameUsedPeak;
    UINT32 GpuPageTableSize;
    UINT32 CpuPageTableSize;
    SIZE_T reserved1[2];
} XMEM_WORKING_SET_STATISTICS, *PXMEM_WORKING_SET_STATISTICS;

#define XMEM_WORKING_SET_TITLE       (0x20000001U)
#define XMEM_WORKING_SET_TOOLS       (0x20000002U)
#define XMEM_WORKING_SET_STACK       (0x20000004U)
#define XMEM_WORKING_SET_KERNEL_POOL (0x20000008U)
#define XMEM_WORKING_SET_CPU_PTE     (0x20000010U)
#define XMEM_WORKING_SET_ALL         (XMEM_WORKING_SET_TITLE       | \
                                      XMEM_WORKING_SET_TOOLS       | \
                                      XMEM_WORKING_SET_STACK       | \
                                      XMEM_WORKING_SET_KERNEL_POOL | \
                                      XMEM_WORKING_SET_CPU_PTE)

// --- virtual query -------------------------------------------------------------

typedef struct _XMEM_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    uint64_t XMemFlags;
    DWORD TagCode;
    CHAR TagFourCC[4];
} XMEM_BASIC_INFORMATION, *PXMEM_BASIC_INFORMATION;

// --- physical pages -------------------------------------------------------------

typedef struct _XMEM_PHYSICAL_MEMORY_LAYOUT {
    uint64_t LowestPageNumber;
    uint64_t LowestPageNumberGpuOptimalBandwidth;
} XMEM_PHYSICAL_MEMORY_LAYOUT, *PXMEM_PHYSICAL_MEMORY_LAYOUT;

// --- allocation hooks ------------------------------------------------------------

typedef void* (WINAPI *PXMEMALLOC_ROUTINE)(SIZE_T Size, uint64_t Attributes);
typedef void (WINAPI *PXMEMFREE_ROUTINE)(void* Address, uint64_t Attributes);

typedef struct _XMEM_ALLOCATION_HOOKS {
    PXMEMALLOC_ROUTINE AllocRoutine;
    PXMEMFREE_ROUTINE FreeRoutine;
} XMEM_ALLOCATION_HOOKS, *PXMEM_ALLOCATION_HOOKS;

// --- heap tracking --------------------------------------------------------------

typedef void (WINAPI *PHEAP_TRACK_ALLOC_ROUTINE)(HANDLE Heap, DWORD Flags, SIZE_T Size, PVOID Addr);
typedef void (WINAPI *PHEAP_TRACK_REALLOC_ROUTINE)(HANDLE Heap, DWORD Flags, SIZE_T Size, PVOID OldAddr, PVOID NewAddr);
typedef void (WINAPI *PHEAP_TRACK_FREE_ROUTINE)(HANDLE Heap, DWORD Flags, PVOID Addr);

typedef struct _XMEM_WIN32_HEAP_STATISTICS {
    SIZE_T SegmentCommitSize;
    SIZE_T LargeAllocCommitSize;
    SIZE_T reserved[6];
} XMEM_WIN32_HEAP_STATISTICS, *PXMEM_WIN32_HEAP_STATISTICS;

typedef struct _XMEM_HEAP_STATISTICS {
    ULONG dwSize;
    ULONG dwPageGroupSize;
    SIZE_T dwMaximumAllocatedSize;
    SIZE_T dwMinimumFreeDepth;
    ULONG dwAllocationCount;
    ULONG dwFreeCount;
} XMEM_HEAP_STATISTICS, *PXMEM_HEAP_STATISTICS;

#define XMEM_HEAP_DEFAULT            0x00000000ULL
#define XMEM_HEAP_TOOL               0x00000001ULL
#define XMEM_HEAP_INPLACE            0x00000002ULL
#define XMEM_HEAP_64K_PAGES          0x00000004ULL

// --- transfer types ---------------------------------------------------------------

typedef enum _XMEM_TRANSFER_TYPE {
    XMemTransferTitleToSystem,
    XMemTransferToolsToSystem,
    XMemTransferToolsToTitle
} XMEM_TRANSFER_TYPE, *PXMEM_TRANSFER_TYPE;

// --- options ------------------------------------------------------------------------

typedef enum _XMEM_OPTIONS {
    eXMEMOPT_VALIDATE_XMEMALLOC = 0x1,
    eXMEMOPT_XMEMALLOCDEFAULT_TAG_VIRTUALALLOC = 0x2,
    eXMEMOPT_RAISE_HEAPFREE_CALLBACK_ON_REALLOC = 0x3,
    eXMEMOPT_XMEMALLOCDEFAULT_DISABLEPAGESIZEDEMOTION = 0x4,
    eXMEMOPT_XMEMALLOCDEFAULT_TRAPUSEAFTERFREE = 0x5,
    eXMEMOPT_XMEMALLOCDEFAULT_USE64KPAGES = 0x6,
    eXMEMOPT_ALLOW_WRITEABLE_FILEMAPPINGS = 0x7,
    eXMEMOPT_SET_XMEM_TAG_TREE_SIZE = 0x8,
    eXMEMOPT_XMEM_WARN_WHEN_TAG_TREE_FULL = 0x9,
    eXMEMOPT_MAX = 0xFFFF
} XMEM_OPTIONS;

typedef struct _XMEM_OPTION {
    XMEM_OPTIONS Option;
    union {
        BOOL Bool;
        uint64_t Int;
        PVOID Pointer;
    } u;
} XMEM_OPTION, *PXMEM_OPTION;

// --- the entry points --------------------------------------------------------

// The allocation routines (the EXE normally overrides these with hooks).
void* WINAPI XMemAlloc(SIZE_T Size, uint64_t Attributes);
void* WINAPI XMemAllocDefault(SIZE_T Size, uint64_t Attributes);
void WINAPI XMemFree(void* Address, uint64_t Attributes);
void WINAPI XMemFreeDefault(void* Address, uint64_t Attributes);

ULONGLONG WINAPI XMemMakeTag(UINT32 Tag);

void WINAPI XMemSetOption(XMEM_OPTION option);

VOID WINAPI XMemSetAllocationHooks(PXMEMALLOC_ROUTINE AllocRoutine,
                                  PXMEMFREE_ROUTINE FreeRoutine);

void* WINAPI XMemVirtualAlloc(PVOID BaseAddress, SIZE_T Size, DWORD AllocationType,
                             uint64_t XMemFlags, DWORD PageProtection);

BOOL WINAPI XMemVirtualQuery(HANDLE Process, PVOID Address, PXMEM_BASIC_INFORMATION Buffer);

BOOL WINAPI XMemGetWorkingSetStatistics(DWORD WorkingSetFlags, PXMEM_WORKING_SET_STATISTICS Statistics);

BOOL WINAPI XMemIsToolsMemAvailable(void);

BOOL WINAPI XMemTransferMemory(XMEM_TRANSFER_TYPE TransferType, SIZE_T TransferSize);

BOOL WINAPI XMemAllocatePhysicalPages(uint64_t XMemFlags, ULONG_PTR* NumberOfPages,
                                      ULONG_PTR* PageArray);

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR NumberOfPages, const ULONG_PTR* PageArray);

PVOID WINAPI XMemMapPhysicalPages(PVOID VirtualAddress, ULONG_PTR NumberOfPages,
                                 const ULONG_PTR* PageArray);

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* VirtualAddressArray, ULONG_PTR NumberOfPages,
                                        const ULONG_PTR* PageArray);

BOOL WINAPI XMemGetPhysicalMemoryLayout(PXMEM_PHYSICAL_MEMORY_LAYOUT Layout);

HANDLE WINAPI XMemCreateWin32Heap(uint64_t HeapFlags, PVOID BaseAddress);
PVOID WINAPI XMemGetTitleHeap(void);

BOOL WINAPI XMemSetAllocationHysteresis(DWORD MemType, BOOL LargeHeap, SIZE_T NewSize);
SIZE_T WINAPI XMemGetAllocationHysteresis(DWORD MemType, BOOL LargeHeap);
BOOL WINAPI XMemPreallocateFreeSpace(DWORD MemType, BOOL LargeHeap);
BOOL WINAPI XMemGetAllocationStatistics(DWORD MemType, BOOL LargeHeap, PXMEM_HEAP_STATISTICS Statistics);

BOOL WINAPI XMemSetWin32HeapTrackingHooks(PHEAP_TRACK_ALLOC_ROUTINE AllocRoutine,
                                        PHEAP_TRACK_REALLOC_ROUTINE ReAllocRoutine,
                                        PHEAP_TRACK_FREE_ROUTINE FreeRoutine);

BOOL WINAPI XMemGetWin32HeapStatistics(HANDLE Heap, PXMEM_WIN32_HEAP_STATISTICS Statistics);

BOOL WINAPI XMemSetWin32HeapName(HANDLE Heap, PCSTR Buffer, SIZE_T SizeOfBuffer);
BOOL WINAPI XMemGetWin32HeapName(HANDLE Heap, PSTR Buffer, SIZE_T SizeOfBuffer, PSIZE_T ReturnLength);

#ifdef __cplusplus
}
#endif
