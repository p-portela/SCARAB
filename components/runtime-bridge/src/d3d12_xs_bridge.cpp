// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ---------------------------------------------------------------------------
// d3d12_xs.dll  --  console (Scarlett) D3D12 runtime  ->  Windows D3D12 bridge
// ---------------------------------------------------------------------------
// Drop-in replacement for the console runtime d3d12_xs.dll
// (10.0.26100.6247, 116 exports), supplied by the user, never vendored.
//
// Strategy (Design A): do NOT reimplement the D3D12 object model. The console
// COM interfaces carry byte-identical GUIDs to the PC ones (verified: 28/28
// shared IIDs match, e.g. IID_ID3D12Device = 189819f1-1db6-4b57-be54-...),
// therefore the standard D3D12 surface can be *forwarded* to the PC runtime and
// only the console-only exports have to be emulated.
//
//     title (console build)
//        -> d3d12_xs.dll            [ this file ]
//             -> C:\Windows\System32\d3d12.dll   (PC runtime)
//                  -> PC vendor UMD -> PC kernel driver -> real GPU
//
// The genuine console stack (d3d12_xs.dll -> xgs12_pc_xs.dll "console User Mode
// Driver for Direct3D 12.XS / Offline Scarlett") is NOT used here: its bottom
// seam is DeviceIoControl to the *console* GPU kernel driver, which does not
// exist on PC.
//
// ABI NOTE on unknown-arity exports: on x64 (MS ABI) an implementation that
// ignores its arguments is safe to call no matter what the true arity is --
// arguments arrive in RCX/RDX/R8/R9 + stack and are simply never read. Every
// export whose real signature is not yet recovered is therefore written as a
// zero-argument stub that logs. That is deliberate, not sloppiness: it removes
// the risk of a wrong signature corrupting the stack.
//
// Build: see build.sh (mingw-w64, x86_64). Verify: tools/verify_exports.py
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winver.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <emmintrin.h>
#include <new>

// ---------------------------- logging --------------------------------------
static CRITICAL_SECTION g_log_cs;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static volatile LONG g_log_init = 0;

static void log_open() {
    if (InterlockedCompareExchange(&g_log_init, 1, 0) != 0) return;
    InitializeCriticalSection(&g_log_cs);
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&log_open, &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n > 4) {
        wchar_t* dot = wcsrchr(path, L'.');
        if (dot) *dot = 0;
    }
    lstrcatW(path, L".bridge.log");
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void blog(const char* fmt, ...) {
    log_open();
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    buf[n] = '\r'; buf[n + 1] = '\n'; buf[n + 2] = 0;
    EnterCriticalSection(&g_log_cs);
    DWORD w = 0;
    WriteFile(g_log, buf, (DWORD)n + 2, &w, nullptr);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------- backend resolution ---------------------------
// SCARAB does not hard-code *which* D3D12 runtime it forwards to. Every backend
// implements the same entry points under the same names, so "which backend"
// reduces to "which d3d12.dll do we load":
//
//   Windows (primary)  Microsoft's runtime. Either the system one in System32, or
//                      the app-local Agility SDK: shipping D3D12Core.dll next to
//                      the title is enough, because the loader d3d12.dll picks it
//                      up. We always load the *loader*, never D3D12Core.dll -
//                      that one exports only D3D12GetInterface and D3D12SDKVersion
//                      and cannot supply D3D12CreateDevice.
//   Linux (secondary)  vkd3d-proton (D3D12 -> Vulkan). It ships its own
//                      d3d12.dll, so the forwarding code below is unchanged.
//                      Under Proton the loader already resolves to it; under
//                      plain Wine point SCARAB_D3D12_DLL at vkd3d's d3d12.dll.
//
//   SCARAB_D3D12_DLL   explicit path to the runtime to load (wins over all)
//   SCARAB_BACKEND     auto | d3d12 | vkd3d   - declares the expectation, and a
//                      mismatch is reported loudly rather than silently tolerated
//
// Note: DXVK is not a candidate. DXVK covers D3D9/10/11; D3D12 on Vulkan is
// vkd3d-proton's job.

static const wchar_t* g_backend_kind = L"unknown";

static void read_file_version(const wchar_t* path, wchar_t* out, size_t cap) {
    if (cap) out[0] = 0;
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return;
    void* buf = malloc(size);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, size, buf)) {
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", (LPVOID*)&ffi, &len) && ffi) {
            swprintf(out, cap, L"%u.%u.%u.%u",
                     HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                     HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        }
    }
    free(buf);
}

// Is this process running under Wine/Proton? Detected by the Wine-specific
// ntdll export, not by path. Wine's builtin d3d12.dll sits at
// C:\windows\system32\d3d12.dll, so the path cannot tell it apart from
// Microsoft's runtime - and on Linux that builtin *is* the D3D12 -> Vulkan
// bridge. Getting this wrong mislabels the layer's own backend on the one host
// where vkd3d is the whole point.
static bool host_is_wine() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    cached = (nt && GetProcAddress(nt, "wine_get_version")) ? 1 : 0;
    return cached == 1;
}

// Which D3D12 runtime did we actually load? Reported as a hint - but the hint
// has to be right where it matters: under Wine/Proton the vkd3d-proton-derived
// implementation is the only one that exists, so it is what got loaded.
static const wchar_t* classify_backend(const wchar_t* path, const wchar_t* ver) {
    if (wcsstr(path, L"vkd3d")) return L"vkd3d-proton (D3D12 -> Vulkan)";
    if (host_is_wine())         return L"vkd3d-proton / wine builtin (D3D12 -> Vulkan)";
    if (wcsstr(path, L"System32") || wcsstr(path, L"system32")) {
        // Microsoft's runtime always carries a version resource; an unversioned
        // module under system32 is an override, i.e. something that is not MS.
        return (ver && ver[0]) ? L"Microsoft d3d12 (system)"
                               : L"unversioned d3d12 in system32 (override?)";
    }
    return L"custom / app-local";
}

static void log_backend(HMODULE mod) {
    wchar_t path[MAX_PATH] = L"(unresolved)";
    GetModuleFileNameW(mod, path, MAX_PATH);
    wchar_t ver[64];
    read_file_version(path, ver, 64);
    g_backend_kind = classify_backend(path, ver);
    blog("backend: %ls", g_backend_kind);
    blog("backend: %ls%s%ls", path, ver[0] ? L"  version " : L"", ver);

    wchar_t want[32] = L"";
    GetEnvironmentVariableW(L"SCARAB_BACKEND", want, 32);
    if (want[0] && _wcsicmp(want, L"auto") != 0) {
        const bool isvkd3d = (g_backend_kind[0] == L'v') ;
        const bool wantvkd3d = (_wcsicmp(want, L"vkd3d") == 0);
        if (wantvkd3d != isvkd3d)
            blog("backend: WARNING SCARAB_BACKEND=%ls but the loaded runtime looks like %ls",
                 want, g_backend_kind);
    }
}

// ---------------------- PC D3D12 forward table -----------------------------
// Resolved lazily from the real PC runtime. We deliberately go through
// System32\d3d12.dll by *name*: D3D12CreateDevice and friends are plain
// unmarshalled exports there and their signatures are stable and documented.
typedef HRESULT(WINAPI* PFN_D3D12CreateDevice)(void*, UINT /*D3D_FEATURE_LEVEL*/, const GUID*, void**);
typedef HRESULT(WINAPI* PFN_D3D12GetDebugInterface)(const GUID*, void**);
typedef HRESULT(WINAPI* PFN_D3D12SerializeRootSignature)(const void*, UINT, void**, void**);
typedef HRESULT(WINAPI* PFN_D3D12SerializeVersionedRootSignature)(const void*, void**, void**);
typedef HRESULT(WINAPI* PFN_D3D12CreateRootSignatureDeserializer)(const void*, SIZE_T, const GUID*, void**);
typedef HRESULT(WINAPI* PFN_D3D12CreateVersionedRootSignatureDeserializer)(const void*, SIZE_T, const GUID*, void**);
typedef HRESULT(WINAPI* PFN_D3D12EnableExperimentalFeatures)(UINT, const GUID*, const void*, UINT*);
typedef HRESULT(WINAPI* PFN_D3D12GetInterface)(const GUID*, const GUID*, void**);

static struct Fwd {
    HMODULE dll;
    PFN_D3D12CreateDevice CreateDevice;
    PFN_D3D12GetDebugInterface GetDebugInterface;
    PFN_D3D12SerializeRootSignature SerializeRootSignature;
    PFN_D3D12SerializeVersionedRootSignature SerializeVersionedRootSignature;
    PFN_D3D12CreateRootSignatureDeserializer CreateRootSignatureDeserializer;
    PFN_D3D12CreateVersionedRootSignatureDeserializer CreateVersionedRootSignatureDeserializer;
    PFN_D3D12EnableExperimentalFeatures EnableExperimentalFeatures;
    PFN_D3D12GetInterface GetInterface;

    bool ok() const { return CreateDevice && SerializeRootSignature; }

    void load() {
        if (dll) return;

        // 1. an explicit runtime wins, which is how vkd3d-proton is selected
        //    under plain Wine (Proton resolves it without help)
        wchar_t explicit_path[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"SCARAB_D3D12_DLL", explicit_path, MAX_PATH);
        HMODULE h = nullptr;
        if (explicit_path[0]) {
            h = LoadLibraryExW(explicit_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            blog("backend: SCARAB_D3D12_DLL set to %ls (%s)", explicit_path,
                 h ? "loaded" : "FAILED to load");
        }
        // 2. normal search first, so an app-local runtime or the host's own
        //    implementation (Wine/Proton) is honoured, then System32
        if (!h) h = LoadLibraryW(L"d3d12.dll");
        if (!h) h = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) { blog("FWD: cannot load a D3D12 runtime (err=%lu)", GetLastError()); return; }
        dll = h;
        log_backend(h);
        CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(h, "D3D12CreateDevice");
        GetDebugInterface = (PFN_D3D12GetDebugInterface)GetProcAddress(h, "D3D12GetDebugInterface");
        SerializeRootSignature = (PFN_D3D12SerializeRootSignature)GetProcAddress(h, "D3D12SerializeRootSignature");
        SerializeVersionedRootSignature = (PFN_D3D12SerializeVersionedRootSignature)GetProcAddress(h, "D3D12SerializeVersionedRootSignature");
        CreateRootSignatureDeserializer = (PFN_D3D12CreateRootSignatureDeserializer)GetProcAddress(h, "D3D12CreateRootSignatureDeserializer");
        CreateVersionedRootSignatureDeserializer = (PFN_D3D12CreateVersionedRootSignatureDeserializer)GetProcAddress(h, "D3D12CreateVersionedRootSignatureDeserializer");
        EnableExperimentalFeatures = (PFN_D3D12EnableExperimentalFeatures)GetProcAddress(h, "D3D12EnableExperimentalFeatures");
        GetInterface = (PFN_D3D12GetInterface)GetProcAddress(h, "D3D12GetInterface");
        blog("FWD: PC d3d12.dll loaded, %s", ok() ? "all entry points resolved" : "SOME ENTRY POINTS MISSING");
    }
} g_fwd;

static Fwd& fwd() { if (!g_fwd.dll) g_fwd.load(); return g_fwd; }

// ---------------------- optional console extension wrapper ------------------
// The wrapper layer (wrap/) owns the console extension surface: the X-suffixed
// methods the console appends to the PC interfaces, and the console-only
// command-list type. It is loaded dynamically and is optional: without it the
// bridge still serves the whole standard API and only the extension calls are
// lost. Keeping it out of the link also keeps this DLL's export contract exactly
// what the console runtime published.
typedef HRESULT (WINAPI* PFN_wrap_device)(void*, const GUID*, void**);

// Minimal stand-in for IUnknown, used only to call Release through the vtable
// slot. Never instantiated - this bridge is deliberately header-free.
struct MinimalUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID*, void**);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();
};

static PFN_wrap_device g_wrap = nullptr;
static volatile LONG g_wrap_state = 0;   // 0 untried, 1 tried

static PFN_wrap_device wrapfn() {
    if (InterlockedCompareExchange(&g_wrap_state, 1, 0) == 0) {
        HMODULE h = LoadLibraryExW(L"d3d12_xs_wrap.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!h) h = LoadLibraryW(L"d3d12_xs_wrap.dll");
        if (h) {
            g_wrap = (PFN_wrap_device)GetProcAddress(h, "scarab_wrap_device");
            blog("wrapper layer loaded: scarab_wrap_device=%p", (void*)g_wrap);
        } else {
            blog("wrapper layer absent (d3d12_xs_wrap.dll); devices go out unwrapped, "
                 "console extension calls will not be served");
        }
    }
    return g_wrap;
}

// Substitute the PC device with the wrapper, if the wrapper layer is present.
static void try_wrap_device(void** ppDevice, const GUID* riid) {
    if (!ppDevice || !*ppDevice) return;
    PFN_wrap_device f = wrapfn();
    if (!f) return;
    void* wrapped = nullptr;
    HRESULT hr = f(*ppDevice, riid, &wrapped);
    if (SUCCEEDED(hr) && wrapped) {
        ((MinimalUnknown*)*ppDevice)->Release();   // drop the reference the PC call gave us
        *ppDevice = wrapped;
        blog("device substituted with the wrapper layer");
    } else {
        blog("wrapper layer declined (0x%08lx); keeping the PC device", (unsigned long)hr);
    }
}

// ---------------------- local ID3D10Blob / ID3D10Blob impl ------------------
// IID_ID3D10Blob = {8ba5fb08-5195-40e2-ac58-0d989c3a0102}; the console runtime
// exports D3D10CreateBlob, and so does this bridge -- implemented locally so
// the bridge has no D3D10 dependency.
static const GUID kIID_ID3D10Blob =
    {0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};
static const GUID kIID_IUnknown =
    {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct LocalBlob {
    // vtable order: IUnknown(3) then GetBufferPointer, GetBufferSize
    virtual ~LocalBlob() {}
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(const GUID* riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (IsEqualGUID(*riid, kIID_ID3D10Blob) || IsEqualGUID(*riid, kIID_IUnknown)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    virtual ULONG STDMETHODCALLTYPE AddRef() { return (ULONG)InterlockedIncrement(&ref); }
    virtual ULONG STDMETHODCALLTYPE Release() {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) { free(data); delete this; }
        return (ULONG)r;
    }
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() { return data; }
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() { return size; }

    static LocalBlob* create(SIZE_T n) {
        LocalBlob* b = new (std::nothrow) LocalBlob();
        if (!b) return nullptr;
        b->data = malloc(n ? n : 1);
        if (!b->data) { delete b; return nullptr; }
        memset(b->data, 0, n);
        b->size = n;
        b->ref = 1;
        return b;
    }
    void* data; SIZE_T size; LONG ref;
};

// ===========================================================================
//  ORDINALS 5..11  --  standard D3D12 API: forward to the PC runtime
// ===========================================================================

extern "C" HRESULT WINAPI D3D12CreateDevice(void* pAdapter, UINT MinimumFeatureLevel,
                                            const GUID* riid, void** ppDevice) {
    Fwd& f = fwd();
    blog("D3D12CreateDevice(adapter=%p, fl=0x%x, riid=%p) -> forward", pAdapter, MinimumFeatureLevel, riid);
    if (!f.CreateDevice) return E_FAIL;
    HRESULT hr = f.CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr)) try_wrap_device(ppDevice, riid);
    return hr;
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(const GUID* riid, void** ppvDebug) {
    Fwd& f = fwd();
    blog("D3D12GetDebugInterface -> forward");
    if (!f.GetDebugInterface) return E_FAIL;
    return f.GetDebugInterface(riid, ppvDebug);
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(const void* pRootSignature, UINT Version,
                                                      void** ppBlob, void** ppErrorBlob) {
    Fwd& f = fwd();
    blog("D3D12SerializeRootSignature(ver=%u) -> forward", Version);
    if (!f.SerializeRootSignature) return E_FAIL;
    return f.SerializeRootSignature(pRootSignature, Version, ppBlob, ppErrorBlob);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void* pRootSignature,
                                                              void** ppBlob, void** ppErrorBlob) {
    Fwd& f = fwd();
    blog("D3D12SerializeVersionedRootSignature -> forward");
    if (!f.SerializeVersionedRootSignature) return E_FAIL;
    return f.SerializeVersionedRootSignature(pRootSignature, ppBlob, ppErrorBlob);
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void* pSrcData, SIZE_T SrcDataSizeInBytes,
                                                              const GUID* pRootSignatureDeserializerInterface,
                                                              void** ppRootSignatureDeserializer) {
    Fwd& f = fwd();
    blog("D3D12CreateRootSignatureDeserializer(%zu bytes) -> forward", SrcDataSizeInBytes);
    if (!f.CreateRootSignatureDeserializer) return E_FAIL;
    return f.CreateRootSignatureDeserializer(pSrcData, SrcDataSizeInBytes,
                                             pRootSignatureDeserializerInterface, ppRootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
        const void* pSrcData, SIZE_T SrcDataSizeInBytes,
        const GUID* pRootSignatureDeserializerInterface, void** ppRootSignatureDeserializer) {
    Fwd& f = fwd();
    blog("D3D12CreateVersionedRootSignatureDeserializer(%zu bytes) -> forward", SrcDataSizeInBytes);
    if (!f.CreateVersionedRootSignatureDeserializer) return E_FAIL;
    return f.CreateVersionedRootSignatureDeserializer(pSrcData, SrcDataSizeInBytes,
                                                      pRootSignatureDeserializerInterface,
                                                      ppRootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT NumFeatures,
                                                          const GUID* pIIDs, const void* pConfigurationStructs,
                                                          UINT* pConfigurationStructSizes) {
    Fwd& f = fwd();
    blog("D3D12EnableExperimentalFeatures(%u features) -> forward", NumFeatures);
    if (!f.EnableExperimentalFeatures) return E_FAIL;
    return f.EnableExperimentalFeatures(NumFeatures, pIIDs, pConfigurationStructs,
                                        pConfigurationStructSizes);
}

// ===========================================================================
//  ORDINAL 3  --  D3D10CreateBlob: implemented locally
// ===========================================================================
extern "C" HRESULT WINAPI D3D10CreateBlob(SIZE_T NumBytes, void** ppBuffer) {
    blog("D3D10CreateBlob(%zu) -> local blob", NumBytes);
    if (!ppBuffer) return E_POINTER;
    LocalBlob* b = LocalBlob::create(NumBytes);
    if (!b) { *ppBuffer = nullptr; return E_OUTOFMEMORY; }
    *ppBuffer = static_cast<void*>(b);
    return S_OK;
}

// ===========================================================================
//  ORDINAL 1/18  --  trivial console memory / cache primitives
// ===========================================================================
extern "C" void WINAPI D3D12ZeroMemoryX(void* pData, SIZE_T Size) {
    // console fast-zero helper; plain zeroing is semantically equivalent
    if (pData && Size) memset(pData, 0, Size);
}

extern "C" void WINAPI D3DFlushEntireCpuCache() {
    _mm_mfence();
    _mm_sfence();
    _mm_lfence();
}

// ===========================================================================
//  ORDINALS 14..17  --  console process/debug/driver state
// ===========================================================================
static volatile LONG g_debug_flags = 0;
static volatile LONG g_mem_profiling = 0;

extern "C" HRESULT WINAPI D3D12ConsGetProcessDebugFlags(UINT* pFlags) {
    if (!pFlags) return E_POINTER;
    *pFlags = (UINT)InterlockedCompareExchange(&g_debug_flags, 0, 0);
    blog("D3D12ConsGetProcessDebugFlags -> 0x%x", *pFlags);
    return S_OK;
}

extern "C" HRESULT WINAPI D3D12ConsSetProcessDebugFlags(UINT Flags) {
    blog("D3D12ConsSetProcessDebugFlags(0x%x) -> recorded, not honoured by PC runtime", Flags);
    InterlockedExchange(&g_debug_flags, (LONG)Flags);
    return S_OK;
}

extern "C" HRESULT WINAPI D3D12ConsEnableMemoryProfiling(WINBOOL Enable) {
    blog("D3D12ConsEnableMemoryProfiling(%d) -> recorded (no PC equivalent)", Enable);
    InterlockedExchange(&g_mem_profiling, Enable ? 1 : 0);
    return S_OK;
}

extern "C" HRESULT WINAPI D3D12ConsUnloadDriverModule() {
    blog("D3D12ConsUnloadDriverModule -> no-op (console UMD seam does not exist here)");
    return S_OK;
}

// ===========================================================================
//  ORDINAL 13  --  D3D12ConsCreateDevice
// ===========================================================================
// Real console SDK signature (D3D12CONS_CREATE_DEVICE_PARAMETERS): we forward the
// request to PC D3D12CreateDevice. The console memory model differs (unified
// memory, Flash/reserved heaps) so the creation parameters are logged, not
// honoured -- see docs/d3d12-xs-bridge.md, "known deltas".
extern "C" HRESULT WINAPI D3D12ConsCreateDevice(void* pAdapterFactory,
                                                const void* pCreationParameters,
                                                const GUID* riid, void** ppDevice) {
    blog("D3D12ConsCreateDevice(factory=%p, params=%p, riid=%p) -> forward as PC device",
         pAdapterFactory, pCreationParameters, riid);
    Fwd& f = fwd();
    if (!f.CreateDevice) return E_FAIL;
    // 0xB000 == D3D_FEATURE_LEVEL_11_0 ; console titles are SM6.5+ anyway, and
    // the PC runtime will refuse anything it cannot support.
    HRESULT hr = f.CreateDevice(nullptr, 0xB000u, riid, ppDevice);
    if (SUCCEEDED(hr)) try_wrap_device(ppDevice, riid);
    return hr;
}

// ===========================================================================
//  ORDINAL 4  --  D3D12ConfigureVirtualMemoryX
// ===========================================================================
// Console-only. On console the title tells the runtime how much physical/virtual
// address space it wants split between the GPU and system; on PC the OS driver
// owns that policy outright. Recorded for inspection, return S_OK so callers
// proceed.
static volatile LONG g_vmem_configured = 0;

extern "C" HRESULT WINAPI D3D12ConfigureVirtualMemoryX() {
    const UINT32* args = nullptr; (void)args;
    blog("D3D12ConfigureVirtualMemoryX -> recorded (PC driver owns VM policy)");
    InterlockedExchange(&g_vmem_configured, 1);
    return S_OK;
}

// ===========================================================================
//  ORDINAL 12  --  D3D12UploadCustomMicrocodeX  ** THE SHADER SEAM **
// ===========================================================================
// This is where the console runtime hands pre-compiled *GPU microcode* to the
// driver. On Scarlett that microcode is the FSSM container holding AMD RDNA2 /
// gfx10 machine code (see docs/fssm-analysis.md). The PC runtime and the PC
// vendor UMD accept DXIL, never microcode.
//
// Therefore this single export is the API-level face of the whole shader
// translation problem:
//
//     FSSM blob  ->  raw RDNA2 stream      (tools/fssm_to_rdna2.py, done)
//                ->  DXIL         (the upstream shader recompiler, in progress)
//                ->  PC PSO / shader blob  (this hook)
//
// Until that chain is complete the hook fails loudly rather than silently
// accepting microcode the PC runtime would reject anyway.
extern "C" HRESULT WINAPI D3D12UploadCustomMicrocodeX() {
    static volatile LONG warned = 0;
    if (InterlockedCompareExchange(&warned, 1, 0) == 0) {
        blog("D3D12UploadCustomMicrocodeX: console microcode upload is NOT translatable yet.");
        blog("  chain: FSSM -> RDNA2 -> DXIL. See the shader-target notes");
    }
    return E_NOTIMPL;  // deliberate: fail loudly, do not pretend
}

// ===========================================================================
//  ORDINALS 19..22  --  DXGIX presentation / vblank model
// ===========================================================================
// Console presents an *array* of swapchains in one call and drives vblank
// notifications from the display engine. DXGI on PC has neither, so these are
// emulated: presentation and vblank are recorded and acknowledged. A real
// implementation drives registered PC swapchains here (see docs).
extern "C" HRESULT WINAPI DXGIXPresentArray() {
    static volatile LONG n = 0;
    LONG c = InterlockedIncrement(&n);
    if (c <= 3 || (c % 600) == 0)
        blog("DXGIXPresentArray -> emulated (call #%ld); PC swapchain Present() not yet wired", c);
    return S_OK;
}

extern "C" HRESULT WINAPI DXGIXSetFrameNotification() {
    static volatile LONG warned = 0;
    if (InterlockedCompareExchange(&warned, 1, 0) == 0)
        blog("DXGIXSetFrameNotification -> recorded; PC has no frame-notification equivalent");
    return S_OK;
}

extern "C" HRESULT WINAPI DXGIXSetVLineNotification() {
    static volatile LONG warned = 0;
    if (InterlockedCompareExchange(&warned, 1, 0) == 0)
        blog("DXGIXSetVLineNotification -> recorded; PC vblank comes from D3DKMTWaitForVerticalBlankEvent");
    return S_OK;
}

extern "C" HRESULT WINAPI DXGIXGetFrameStatistics(void* pStats) {
    if (pStats) memset(pStats, 0, 64);
    return S_OK;
}

// ===========================================================================
//  ORDINAL 116  --  InternalDriverCallX   (the UMD escape hatch)
// ===========================================================================
// The genuine runtime uses this to reach driver-private entry points it does
// not expose as first-class API. The console UMD (xgs12_pc_xs.dll) exports
// matching UMDPresentArray / UMDInternalDriverCallX, i.e. this is the
// runtime<->UMD private channel. There is nothing on the PC side to forward it
// to: our contract is "keep the PC runtime clean", so it is a logged refusal.
extern "C" HRESULT WINAPI InternalDriverCallX() {
    static volatile LONG warned = 0;
    if (InterlockedCompareExchange(&warned, 1, 0) == 0)
        blog("InternalDriverCallX -> E_NOTIMPL. Private runtime<->UMD channel; "
             "no PC counterpart. Calls are counted only.");
    return E_NOTIMPL;
}

// ===========================================================================
//  ORDINAL 2  --  PixGetPixieEntryPoints  (PIX instrumentation)
// ===========================================================================
extern "C" HRESULT WINAPI PixGetPixieEntryPoints(void** ppEntryPoints) {
    blog("PixGetPixieEntryPoints -> no PIX on this path");
    if (ppEntryPoints) *ppEntryPoints = nullptr;
    return E_NOTIMPL;
}

// ===========================================================================
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        blog("=== d3d12_xs.dll bridge attached (pid=%lu) ===", GetCurrentProcessId());
        fwd();
    } else if (reason == DLL_PROCESS_DETACH) {
        blog("=== d3d12_xs.dll bridge detached ===");
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
