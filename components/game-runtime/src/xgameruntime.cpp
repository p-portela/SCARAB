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

// shortcut: no GDK family is implemented yet, so every query refuses by name.
// Upgrade: return a family's COM object from the xcl/VIctory GUID tables when a
// title needs one, and keep refusing the rest.
extern "C" HRESULT QueryApiImpl(const GUID* clsid, const GUID* iid, void** out) {
    if (out) *out = nullptr;
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
