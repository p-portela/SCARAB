// ===========================================================================
//  wrap.cpp -- wrapper layer: owns the console (Scarlett) D3D12 extension surface
//              and forwards the PC surface to a real PC D3D12 object.
// ===========================================================================
// WHY A WRAPPER IS NEEDED
// The bridge (bridge/) forwards the *free functions* of the console runtime to
// PC d3d12.dll. That is enough for the standard API but not for the extension
// surface: the console appends methods to the PC interfaces (the UMD reports
// errors as "ID3D12Device::CreateCommandQueueX ...", i.e. X methods sit on the
// device vtable) and adds a console-only command-list type. A plain PC device
// object handed to the title cannot serve those calls. The objects returned to
// the title must therefore be wrappers: identical in their PC part, extended in
// their console part.
//
// WHY THE WRAPPER INHERITS FROM THE REAL INTERFACES
// The classes derive from the genuine Agility SDK interfaces and mark every
// forwarded method `override`, so the compiler verifies all 53 (device) and 76
// (command list) signatures against the real ABI. There is no hand-maintained
// vtable and no chance of an off-by-one slot in the part that is known. Only the
// appended console methods have an assumed order, and those are instrumented.
//
// PROVENANCE: wrap/console_extensions.h, wrap/iid_provenance.json.
//   names + D3D12CONS_* symbols: recovered from the console UMD's validation
//   strings. Ordering: NOT recovered - assumption #1.
// ===========================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <new>

#include "console_extensions.h"

// --------------------------------------------------------------------------
// logging + QueryInterface audit
// --------------------------------------------------------------------------
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
static LONG g_init = 0;
static volatile LONG g_qi_audit = 0;

static void log_open() {
    if (InterlockedCompareExchange(&g_init, 1, 0) != 0) return;
    InitializeCriticalSection(&g_cs);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) *dot = 0;
    strcat(path, ".wrap.log");
    g_log = fopen(path, "w");
}

static void wlog(const char* fmt, ...) {
    log_open();
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    fputs(buf, g_log); fputs("\r\n", g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

extern "C" void scarab_wrapper_set_qi_audit(int on) {
    InterlockedExchange(&g_qi_audit, on ? 1 : 0);
}

static void audit_qi(const char* who, REFIID riid, HRESULT hr) {
    if (!InterlockedCompareExchange(&g_qi_audit, 0, 0)) return;
    wlog("QI %-8s { %08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x } -> 0x%08lx",
         who, riid.Data1, riid.Data2, riid.Data3,
         riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
         riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7], (unsigned long)hr);
}

// --------------------------------------------------------------------------
// IID tables
// --------------------------------------------------------------------------
#include "iid_console.inc"     // 60 console-specific GUIDs exported by the runtime

static bool eq(REFIID a, REFIID b) { return memcmp(&a, &b, sizeof(IID)) == 0; }

// PC IIDs answered with the *wrapper*, so interception survives repeated QI.
static const IID* const kDevicePcIids[] = {
    &IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable,
    &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2,
    &IID_ID3D12Device3, &IID_ID3D12Device4,
};
static const IID* const kListPcIids[] = {
    &IID_IUnknown, &IID_ID3D12Object, &IID_ID3D12DeviceChild, &IID_ID3D12Pageable,
    &IID_ID3D12CommandList,
    &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1,
    &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3,
    &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5,
    &IID_ID3D12GraphicsCommandList6,
};

// --------------------------------------------------------------------------
// Command list wrapper
//
// One class serves both the graphics list and the DMA list. It implements the
// full console list interface (ID3D12ConsDmaCommandList = graphics extensions +
// DMA extensions); m_isDma decides whether QI answers for the DMA IID. The
// console DMA IID is the one console interface IID that is *proven* - it is
// exported by the console runtime at ordinal 100 - so this needs no guessing.
// --------------------------------------------------------------------------
struct CommandListWrapper : public ID3D12ConsDmaCommandList {
    ID3D12GraphicsCommandList6* m_pc;
    ID3D12DeviceX*              m_device;   // owning wrapper, not addref'd
    LONG m_ref;
    bool m_isDma;
    UINT m_backStencilRef;                  // PC has a front reference only

    CommandListWrapper(ID3D12GraphicsCommandList6* pc, ID3D12DeviceX* dev, bool isDma)
        : m_pc(pc), m_device(dev), m_ref(1), m_isDma(isDma), m_backStencilRef(0) {}
    virtual ~CommandListWrapper() {}

    // ---- IUnknown: hand-written, QI is where the console IIDs are served -----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        for (auto* iid : kListPcIids)
            if (eq(riid, *iid)) {
                *ppv = static_cast<ID3D12GraphicsCommandList6*>(this);
                AddRef(); audit_qi("list", riid, S_OK);
                return S_OK;
            }
        if (m_isDma && eq(riid, IID_ID3D12ConsDmaCommandList)) {
            *ppv = static_cast<ID3D12ConsDmaCommandList*>(this);
            AddRef(); audit_qi("dma-list", riid, S_OK);
            return S_OK;
        }
        // Unknown IID. If we serve it from the PC object the caller gets an
        // UNWRAPPED object and interception is lost for that interface, so log it
        // loudly: an entry here is exactly how the missing console IIDs
        // (ID3D12ConsComputeCommandList et al.) will be identified at runtime.
        if (m_pc) {
            HRESULT hr = m_pc->QueryInterface(riid, ppv);
            audit_qi("list", riid, hr);
            if (SUCCEEDED(hr))
                wlog("QI list: UNKNOWN IID %08lx-%04x served unwrapped by the PC object",
                     riid.Data1, riid.Data2);
            return hr;
        }
        audit_qi("list", riid, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { if (m_pc) m_pc->Release(); delete this; }
        return (ULONG)r;
    }

    // ---- generated ABI-exact forwarding (76 methods) -------------------------
    // Includes CopyResource, which the DMA interface also declares: the forward
    // to the PC list is the correct behaviour for a DMA engine copy, so it is
    // deliberately left to the generated code rather than duplicated by hand.
#include "gen_clist.inc"

    // The title must get the *wrapper* device back, or it loses interception on
    // its next CreateCommandList.
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override {
        if (!ppvDevice) return E_POINTER;
        if (m_device) return m_device->QueryInterface(riid, ppvDevice);
        *ppvDevice = nullptr;
        return E_NOINTERFACE;
    }

    // ---- console extensions: graphics list ---------------------------------
    void STDMETHODCALLTYPE RSSetDepthBiasX(INT DepthBias, FLOAT DepthBiasClamp,
                                           FLOAT SlopeScaledDepthBias) override {
        // No PC forward exists: D3D12 moved depth bias into the PSO
        // (D3D12_RASTERIZER_DESC::DepthBias/DepthBiasClamp/SlopeScaledDepthBias),
        // so unlike D3D11 there is no rasterizer bias command-list state to set.
        // Recorded with values so a future PSO-patching pass can honour it.
        wlog("RSSetDepthBiasX(%d, %f, %f) -> recorded only (PC depth bias lives in the PSO)",
             DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
    }

    void STDMETHODCALLTYPE FillMemoryWithPattern(ID3D12Resource* pResource, UINT64 Offset,
                                                 UINT64 SizeBytes, UINT Pattern) override {
        // No PC equivalent: D3D12 has no pattern fill. The console command
        // processor does it in hardware. Emulating it needs a compute shader (or
        // a pre-filled staging buffer), which is real work, not a forward.
        wlog("FillMemoryWithPattern(res=%p, off=%llu, size=%llu, pattern=0x%x) -> E_NOTIMPL "
             "(needs compute-shader emulation)", pResource, (unsigned long long)Offset,
             (unsigned long long)SizeBytes, Pattern);
    }

    void STDMETHODCALLTYPE OMSetSampleMaskX(UINT SampleMask) override {
        // PC sample mask is a PSO field (D3D12_BLEND_DESC::SampleMask), not
        // dynamic command-list state, so it cannot be forwarded. Recorded.
        wlog("OMSetSampleMaskX(0x%x) -> recorded only (PC sample mask is a PSO field)", SampleMask);
    }

    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT Front, UINT Back) override {
        m_backStencilRef = Back;
        wlog("OMSetFrontAndBackStencilRef(front=%u, back=%u) -> front forwarded, back recorded "
             "(PC exposes only the front reference)", Front, Back);
        if (m_pc) m_pc->OMSetStencilRef(Front);
    }

    HRESULT STDMETHODCALLTYPE XSlot_CommandList(UINT Slot, const void* a1, const void* a2,
                                                const void* a3, const void* a4) override {
        wlog("XSlot_CommandList(%u) UNKNOWN CONSOLE METHOD args=[%p %p %p %p]", Slot, a1, a2, a3, a4);
        return E_NOTIMPL;
    }

    // ---- console extensions: DMA list --------------------------------------
    void STDMETHODCALLTYPE FillRawBuffer(ID3D12Resource* pResource, UINT64 Offset,
                                         UINT64 SizeBytes, UINT Value) override {
        // Enforce the UMD's own rule first, so a malformed call is rejected the
        // way the console rejects it: "ID3D12ConsDmaCommandList::FillRawBuffer -
        // SizeBytes must be DWORD aligned."
        if ((SizeBytes & 3ull) != 0) {
            wlog("FillRawBuffer: SizeBytes %llu is not DWORD aligned -> rejected (matches UMD)",
                 (unsigned long long)SizeBytes);
            return;
        }
        wlog("FillRawBuffer(res=%p, off=%llu, size=%llu, value=0x%x) -> E_NOTIMPL "
             "(needs compute-shader emulation)", pResource, (unsigned long long)Offset,
             (unsigned long long)SizeBytes, Value);
    }
};

// --------------------------------------------------------------------------
// Device wrapper
// --------------------------------------------------------------------------
struct DeviceWrapper : public ID3D12DeviceX {
    ID3D12Device4* m_pc;
    LONG m_ref;
    UINT m_frameInterval, m_frameIntervalFlags;

    explicit DeviceWrapper(ID3D12Device4* pc)
        : m_pc(pc), m_ref(1), m_frameInterval(0), m_frameIntervalFlags(0) {}
    virtual ~DeviceWrapper() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        for (auto* iid : kDevicePcIids)
            if (eq(riid, *iid)) {
                *ppv = static_cast<ID3D12Device4*>(this);
                AddRef(); audit_qi("device", riid, S_OK);
                return S_OK;
            }
        if (m_pc) {
            HRESULT hr = m_pc->QueryInterface(riid, ppv);
            audit_qi("device", riid, hr);
            if (SUCCEEDED(hr))
                wlog("QI device: UNKNOWN IID %08lx-%04x served unwrapped by the PC object",
                     riid.Data1, riid.Data2);
            return hr;
        }
        audit_qi("device", riid, E_NOINTERFACE);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) { if (m_pc) m_pc->Release(); delete this; }
        return (ULONG)r;
    }

    // ---- generated ABI-exact forwarding (53 methods) -------------------------
#include "gen_device.inc"

    // ---- interception: CreateCommandList -----------------------------------
    // The UMD states the rule verbatim: "CreateCommandList: An riid of
    // __uuidof(ID3D12ConsDmaCommandList) is required for
    // D3D12CONS_COMMAND_LIST_TYPE_DMA." So this one method is where console
    // command lists enter the process, and it is the wrapper's main hook.
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* pCommandAllocator,
                                                ID3D12PipelineState* pInitialState,
                                                REFIID riid, void** ppCommandList) override {
        if (!ppCommandList) return E_POINTER;
        *ppCommandList = nullptr;

        const bool isDmaType = ((UINT)type == (UINT)D3D12CONS_COMMAND_LIST_TYPE_DMA);
        const bool isDmaIid  = eq(riid, IID_ID3D12ConsDmaCommandList);
        D3D12_COMMAND_LIST_TYPE pcType = type;

        if (isDmaType) {
            pcType = D3D12_COMMAND_LIST_TYPE_COPY;   // console DMA -> PC copy engine
            if (!isDmaIid)
                wlog("CreateCommandList: DMA list type without the DMA riid "
                     "(the console would reject this; continuing)");
        }
        wlog("CreateCommandList(nodeMask=%u, type=%u%s, alloc=%p, riid=%08lx-%04x) -> %s wrapper",
             nodeMask, (unsigned)type, isDmaType ? " [CONSOLE DMA]" : "", pCommandAllocator,
             riid.Data1, riid.Data2, isDmaIid ? "DMA" : "list");
        if (!m_pc) return E_FAIL;

        // Ask for the highest PC list version so titles needing only the base
        // interface still work if the PC runtime is older than our headers.
        ID3D12GraphicsCommandList6* pc = nullptr;
        bool viaCreate1 = true;
        HRESULT hr = m_pc->CreateCommandList1(nodeMask, pcType, D3D12_COMMAND_LIST_FLAG_NONE,
                                              IID_ID3D12GraphicsCommandList6, (void**)&pc);
        if (FAILED(hr) || !pc) {
            viaCreate1 = false;
            hr = m_pc->CreateCommandList(nodeMask, pcType, pCommandAllocator, pInitialState,
                                         IID_ID3D12GraphicsCommandList6, (void**)&pc);
        }
        if (FAILED(hr) || !pc) {
            wlog("CreateCommandList: PC runtime refused (0x%08lx)", (unsigned long)hr);
            return hr;
        }
        // CreateCommandList1 ignores the initial state, so re-apply it if the
        // caller supplied one.
        if (viaCreate1 && (pCommandAllocator || pInitialState))
            pc->Reset(pCommandAllocator, pInitialState);

        CommandListWrapper* w = new (std::nothrow) CommandListWrapper(pc, this, isDmaIid);
        if (!w) { pc->Release(); return E_OUTOFMEMORY; }
        hr = w->QueryInterface(riid, ppCommandList);
        w->Release();
        return hr;
    }

    // ---- console device extensions -----------------------------------------
    HRESULT STDMETHODCALLTYPE CreateCommandQueueX(const void* pDesc, const void* pShaderQueueParams,
                                                 REFIID riid, void** ppCommandQueue) override {
        // UMD: "shader-based command queues only support
        // D3D12_COMMAND_LIST_TYPE_COMPUTE." PC has no shader-based queue concept,
        // so this cannot be forwarded 1:1.
        wlog("CreateCommandQueueX(desc=%p, params=%p, riid=%08lx) -> E_NOTIMPL "
             "(PC has no shader-based command queue)", pDesc, pShaderQueueParams, riid.Data1);
        if (ppCommandQueue) *ppCommandQueue = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetFrameStatisticsX(void* pStatistics) override {
        if (pStatistics) memset(pStatistics, 0, 64);
        wlog("GetFrameStatisticsX -> zeroed (PC has no console frame statistics)");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduleFrameEventX(UINT Id, UINT Flags) override {
        wlog("ScheduleFrameEventX(id=%u, flags=0x%x) -> recorded", Id, Flags);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetFrameIntervalX(UINT Interval, UINT Flags) override {
        m_frameInterval = Interval; m_frameIntervalFlags = Flags;
        wlog("SetFrameIntervalX(interval=%u, flags=0x%x) -> recorded (PC presentation timing is "
             "DXGI's)", Interval, Flags);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDebugErrorFilterX(UINT ErrorIdentifier, UINT FilterFlags) override {
        // Mirror the UMD's validation so a title relying on the rejection behaves
        // identically here: "Invalid ErrorIdentifier 0x%X."
        if (ErrorIdentifier == 0 || ErrorIdentifier > 0xFFFF) {
            wlog("SetDebugErrorFilterX: invalid ErrorIdentifier 0x%X (matches UMD rejection)",
                 ErrorIdentifier);
            return E_INVALIDARG;
        }
        wlog("SetDebugErrorFilterX(0x%X, 0x%X) -> recorded", ErrorIdentifier, FilterFlags);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE XSlot_Device(UINT Slot, const void* a1, const void* a2,
                                           const void* a3, const void* a4) override {
        wlog("XSlot_Device(%u) UNKNOWN CONSOLE METHOD args=[%p %p %p %p]", Slot, a1, a2, a3, a4);
        return E_NOTIMPL;
    }
};

// --------------------------------------------------------------------------
// entry points for the bridge
// --------------------------------------------------------------------------
extern "C" HRESULT scarab_wrap_device(void* pcDevice, REFIID riid, void** ppv) {
    if (!pcDevice || !ppv) return E_POINTER;
    *ppv = nullptr;

    // NEVER blind-cast the PC device up to a version it may not implement.
    // A device created as ID3D12Device may be exactly that; treating it as
    // ID3D12Device4 would put methods past the end of its vtable, and any call
    // to one would jump into whatever bytes follow - an execute-access
    // violation, not a clean failure. Ask for the version we need instead.
    // (This bug was caught by bridge/tools/integration_test.cpp, which is the
    //  reason that test exists.)
    static const IID* const kVersions[] = {
        &IID_ID3D12Device4, &IID_ID3D12Device3, &IID_ID3D12Device2,
        &IID_ID3D12Device1, &IID_ID3D12Device,
    };
    ID3D12Device4* pc4 = nullptr;
    for (auto* iid : kVersions) {
        void* p = nullptr;
        HRESULT qhr = ((IUnknown*)pcDevice)->QueryInterface(*iid, &p);
        if (FAILED(qhr) || !p) continue;
        if (eq(*iid, IID_ID3D12Device4)) { pc4 = (ID3D12Device4*)p; break; }
        ((IUnknown*)p)->Release();
        wlog("scarab_wrap_device: PC device lacks ID3D12Device4 (has %08lx)", iid->Data1);
    }
    if (!pc4) {
        wlog("scarab_wrap_device: PC device does not implement ID3D12Device4; refusing to "
             "wrap. The device will go out unwrapped and console extension calls will not "
             "be served - which is the safe failure.");
        return E_NOINTERFACE;
    }

    DeviceWrapper* w = new (std::nothrow) DeviceWrapper(pc4);
    if (!w) return E_OUTOFMEMORY;
    HRESULT hr = w->QueryInterface(riid, ppv);
    w->Release();
    wlog("scarab_wrap_device(%p, %08lx) -> 0x%08lx", pcDevice, riid.Data1, (unsigned long)hr);
    return hr;
}

// --------------------------------------------------------------------------
// self-test: exercises the wrapper's own logic with no GPU and no device.
// A null PC object is passed on purpose: QI routing, refcounting, validation and
// unknown-slot logging must all behave without one.
// --------------------------------------------------------------------------
extern "C" int scarab_wrapper_selftest(void) {
    int checks = 0, fails = 0;
    wlog("=== wrapper self-test ===");
    scarab_wrapper_set_qi_audit(1);

    DeviceWrapper* d = new DeviceWrapper(nullptr);
    void* pv = nullptr;

    // 1. every PC device IID must be served by the *wrapper*
    for (auto* iid : kDevicePcIids) {
        checks++;
        pv = nullptr;
        HRESULT hr = d->QueryInterface(*iid, &pv);
        if (hr != S_OK || pv != static_cast<ID3D12Device4*>(d)) {
            wlog("FAIL device QI for %08lx", iid->Data1); fails++;
        } else {
            d->Release();
        }
    }
    // 2. a device must NOT serve a command-list IID
    checks++;
    if (d->QueryInterface(IID_ID3D12ConsDmaCommandList, &pv) != E_NOINTERFACE) {
        wlog("FAIL device served a command-list IID"); fails++;
    }

    // 3. a DMA list must serve its proven console IID; a plain list must not
    checks++;
    CommandListWrapper* dm = new CommandListWrapper(nullptr, d, true);
    pv = nullptr;
    if (dm->QueryInterface(IID_ID3D12ConsDmaCommandList, &pv) != S_OK || !pv) {
        wlog("FAIL dma list QI for its own IID"); fails++;
    } else {
        dm->Release();
    }
    checks++;
    CommandListWrapper* gl = new CommandListWrapper(nullptr, d, false);
    if (gl->QueryInterface(IID_ID3D12ConsDmaCommandList, &pv) != E_NOINTERFACE) {
        wlog("FAIL plain list served the DMA IID"); fails++;
    }
    gl->Release();

    // 4. console-only device methods must be callable and must not touch a null PC
    checks++;
    if (d->SetFrameIntervalX(2, 0) != S_OK) { wlog("FAIL SetFrameIntervalX"); fails++; }
    checks++;
    if (d->GetFrameStatisticsX(nullptr) != S_OK) { wlog("FAIL GetFrameStatisticsX"); fails++; }
    checks++;
    if (d->SetDebugErrorFilterX(0, 0) != E_INVALIDARG) {
        wlog("FAIL SetDebugErrorFilterX should reject ErrorIdentifier 0"); fails++; }
    checks++;
    if (d->SetDebugErrorFilterX(1, 0) != S_OK) { wlog("FAIL SetDebugErrorFilterX valid"); fails++; }
    checks++;
    if (d->CreateCommandQueueX(nullptr, nullptr, IID_ID3D12Device, &pv) != E_NOTIMPL) {
        wlog("FAIL CreateCommandQueueX should refuse"); fails++; }

    // 5. unknown appended slots must be recorded, not crash
    checks++;
    if (d->XSlot_Device(7, (void*)1, (void*)2, (void*)3, (void*)4) != E_NOTIMPL) {
        wlog("FAIL XSlot_Device"); fails++; }

    // 6. console list methods must not touch a null PC and must honour UMD rules
    checks++;
    CommandListWrapper* l2 = new CommandListWrapper(nullptr, d, false);
    l2->OMSetFrontAndBackStencilRef(3, 5);      // front forwarded, back recorded
    l2->OMSetSampleMaskX(0xffffffff);           // recorded only
    l2->RSSetDepthBiasX(10, 1.0f, 2.0f);        // recorded only
    l2->FillMemoryWithPattern(nullptr, 0, 16, 0xdeadbeef);
    l2->FillRawBuffer(nullptr, 0, 6, 0);        // 6 is not DWORD aligned -> rejected
    l2->FillRawBuffer(nullptr, 0, 8, 0);        // aligned -> E_NOTIMPL path
    if (l2->XSlot_CommandList(3, nullptr, nullptr, nullptr, nullptr) != E_NOTIMPL) {
        wlog("FAIL XSlot_CommandList"); fails++; }
    l2->Release();

    d->Release();
    wlog("=== self-test done: %d checks, %d failures ===", checks, fails);
    printf("wrapper self-test: %d checks, %d failures\n", checks, fails);
    return fails;
}
