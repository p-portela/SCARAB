// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// xgameruntime: the GDK system runtime the title loads by name.
//
// The title loads `xgameruntime.dll` with LOAD_LIBRARY_SEARCH_SYSTEM32 and then
// resolves the GDK bootstrap through GetProcAddress: QueryApiImpl,
// InitializeApiImpl(Ex/Ex2), XErrorReport and UninitializeApiImpl. Every GDK
// API the title calls reaches this shim through that indirection, so the
// families it asks QueryApiImpl for -- not this file -- are the real surface.
//
// The bootstrap exports succeed: this shim is the implementation and it is
// loaded. QueryApiImpl answers a family it does not implement with
// REGDB_E_CLASSNOTREG and names the GUID it was asked for: the honest answer,
// and the one that says what to implement next.

#include <windows.h>
#include <cstdint>
#include <cstdio>

static void print_guid(const char* what, const GUID* g) {
    if (!g) return;
    printf("xgameruntime: %s {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n", what,
           static_cast<unsigned long>(g->Data1), g->Data2, g->Data3, g->Data4[0], g->Data4[1],
           g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    fflush(stdout);
}

extern "C" HRESULT InitializeApiImpl(uint64_t gdk, uint64_t os) {
    printf("xgameruntime: InitializeApiImpl(gdk=%llx os=%llx)\n", gdk, os);
    fflush(stdout);
    return S_OK;
}

extern "C" HRESULT InitializeApiImplEx(uint64_t gdk, uint64_t os, uint64_t flags) {
    printf("xgameruntime: InitializeApiImplEx(gdk=%llx os=%llx flags=%llx)\n", gdk, os, flags);
    fflush(stdout);
    return S_OK;
}

extern "C" HRESULT InitializeApiImplEx2(uint64_t gdk, uint64_t os, uint64_t flags, void* options) {
    printf("xgameruntime: InitializeApiImplEx2(gdk=%llx os=%llx flags=%llx options=%p)\n", gdk, os,
           flags, options);
    fflush(stdout);
    return S_OK;
}

extern "C" void UninitializeApiImpl(void) {}

// The GDK families the title queries (per its own QueryApiImpl calls and the
// xcl gdk_table GUIDs). Each family gets one object that answers QueryInterface
// for the family's main interface and refuses every method loudly: nothing it
// exposes is a fake, it does not claim behavior that is not implemented.
// shortcut: every slot refuses; the axes that matter (XAsync run/complete,
// XSystem queries) are the first to implement.
struct GdkObject {
    void** vtable;
    const char* name;
    GUID iid;
};

static bool guid_eq(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           a.Data4[0] == b.Data4[0] && a.Data4[1] == b.Data4[1] && a.Data4[2] == b.Data4[2] &&
           a.Data4[3] == b.Data4[3] && a.Data4[4] == b.Data4[4] && a.Data4[5] == b.Data4[5] &&
           a.Data4[6] == b.Data4[6] && a.Data4[7] == b.Data4[7];
}

// Every COM object must answer IID_IUnknown: middleware that validates by QI
// treats E_NOINTERFACE as "this object is not usable". The family's own
// interface and IUnknown both resolve to the same object.
static const GUID IID_IUNKNOWN_ = {0x00000000, 0x0000, 0x0000,
                                   {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static HRESULT gdk_qi(GdkObject* self, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    if (!guid_eq(self->iid, *riid) && !guid_eq(IID_IUNKNOWN_, *riid)) return E_NOINTERFACE;
    *out = self;
    return S_OK;
}

static ULONG gdk_addref(GdkObject*) { return 1; }
static ULONG gdk_release(GdkObject*) { return 1; }

static HRESULT gdk_refused(GdkObject* self) {
    printf("xgameruntime: %s: refused\n", self->name);
    fflush(stdout);
    return E_NOTIMPL;
}

static const void* GDK_VTBL[] = {
    (const void*)&gdk_qi,
    (const void*)&gdk_addref,
    (const void*)&gdk_release,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused, (const void*)&gdk_refused,
    (const void*)&gdk_refused, (const void*)&gdk_refused,
};
// Derived, never written down: an earlier revision's commit message claimed 40
// slots while the table held 41. Sizing it here keeps the number honest.
static constexpr size_t GDK_VTBL_LEN = sizeof(GDK_VTBL) / sizeof(GDK_VTBL[0]);

struct Family {
    const char* name;
    GUID clsid;
    GUID iid;
    GdkObject object;
};

static Family g_families[] = {
    {"XASYNC",
     {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}},
     {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}}, {}},
    {"XSYSTEM",
     {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}},
     {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}}, {}},
    {"XUSER",
     {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}},
     {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}}, {}},
    {"XGAME",
     {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}},
     {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}}, {}},
    {"XNETWORKING",
     {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}},
     {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}}, {}},
};

static void init_families() {
    printf("xgameruntime: %zu vtable slots per family object\n", GDK_VTBL_LEN);
    fflush(stdout);
    for (auto& f : g_families) {
        f.object.vtable = const_cast<void**>(GDK_VTBL);
        f.object.name = f.name;
        f.object.iid = f.iid;
    }
}

extern "C" HRESULT QueryApiImpl(const GUID* clsid, const GUID* iid, void** out) {
    if (out) *out = nullptr;
    if (!clsid || !iid) return E_INVALIDARG;
    init_families();
    for (auto& f : g_families) {
        // IUnknown resolves too: QueryApiImpl is "create the object and QI it",
        // so refusing IUnknown here would contradict gdk_qi answering it.
        if (guid_eq(f.clsid, *clsid) &&
            (guid_eq(f.iid, *iid) || guid_eq(IID_IUNKNOWN_, *iid))) {
            *out = &f.object;
            return S_OK;
        }
    }
    // A family we do not implement: refuse loudly and name it.
    print_guid("QueryApiImpl class", clsid);
    print_guid("QueryApiImpl interface", iid);
    return REGDB_E_CLASSNOTREG;
}

extern "C" void XErrorReport(HRESULT hr, const char* message) {
    printf("xgameruntime: XErrorReport %08lx: %s\n", static_cast<unsigned long>(hr),
           message ? message : "(null)");
    fflush(stdout);
}

extern "C" HRESULT XGameRuntimeInitialize(void) { return S_OK; }

// shortcut: the GDK signatures are not verified here; x64 passes arguments in
// registers, so refusing is safe and touches nothing. Upgrade: the exact
// XAsyncBlock contract, when a title needs these to succeed.
extern "C" HRESULT XGameUiShowPlayerProfileCardAsync(void) { return E_NOTIMPL; }
extern "C" HRESULT XGameUiShowTextEntryAsync(void) { return E_NOTIMPL; }
