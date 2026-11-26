// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// xcrdapi: the console's DirectStorage the title must create a factory for.
//
// The title imports DStorageGetFactory (XCRDAPI.dll) and asserts the factory
// pointer is non-NULL before anything else. The real pipeline -- an encrypted
// gameos.xvd volume, zlib/BCPack decoding, RPF pack traversal -- is not
// present in this tree. What this shim does, and only this:
//
//   * create the factory object and answer QueryInterface for the riid the
//     title asked for (the one this shim supports);
//   * refuse every other method loudly and by name (E_NOTIMPL).
//
// A title that proceeds past the factory is failing on content open; that is
// the honest failure mode, and this shim never pretends otherwise.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct Factory {
    void** vtable;
};

// The factory is created once and lives for the process; neither the riid
// capture nor the QI answer is allowed to become a lie.
static GUID g_factory_iid{0, 0, 0, {0}};

void log(const char* what) {
    printf("xcrdapi: %s\n", what);
    fflush(stdout);
}

bool guid_eq(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           memcmp(a.Data4, b.Data4, 8) == 0;
}

HRESULT factory_qi(Factory* self, const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    if (!guid_eq(g_factory_iid, *riid)) {
        log("IDStorageFactoryX::QueryInterface (unknown IID)");
        return E_NOINTERFACE;
    }
    *out = self;
    return S_OK;
}

ULONG factory_add_ref(Factory*) { return 1; }
ULONG factory_release(Factory*) { return 1; }

HRESULT factory_refused(const char* what) {
    log(what);
    return E_NOTIMPL;
}

HRESULT create_queue(Factory*, const void*, const GUID*, void**) {
    return factory_refused("IDStorageFactoryX::CreateQueue");
}
HRESULT open_file(Factory*, const void*, const GUID*, void**) {
    return factory_refused("IDStorageFactoryX::OpenFile");
}
HRESULT create_status_array(Factory*, uint32_t, const void*, const GUID*, void**) {
    return factory_refused("IDStorageFactoryX::CreateStatusArray");
}
// Configuration/advisory calls: the console accepts these and stores them.
// There is no staging buffer or queue in this shim, so store and succeed, and
// log the call -- never pretend an allocation happened.
HRESULT set_cpu_affinity(Factory*, uint32_t mask) {
    printf("xcrdapi: SetCpuAffinity(%u) [recorded]\n", mask);
    fflush(stdout);
    return S_OK;
}
HRESULT set_debug_flags(Factory*, uint32_t flags) {
    printf("xcrdapi: SetDebugFlags(%u) [recorded]\n", flags);
    fflush(stdout);
    return S_OK;
}
HRESULT set_staging_buffer_size(Factory*, uint32_t bytes) {
    printf("xcrdapi: SetStagingBufferSize(%u) [recorded]\n", bytes);
    fflush(stdout);
    return S_OK;
}
HRESULT unknown_slot(Factory*) {
    return factory_refused("IDStorageFactoryX: unknown slot");
}

static const void* FACTORY_VTBL[] = {
    (const void*)&factory_qi,
    (const void*)&factory_add_ref,
    (const void*)&factory_release,
    (const void*)&create_queue,
    (const void*)&open_file,
    (const void*)&create_status_array,
    (const void*)&set_cpu_affinity,
    (const void*)&set_debug_flags,
    (const void*)&set_staging_buffer_size,
    (const void*)&unknown_slot,  // 9: unknown -- refused loudly
    (const void*)&create_queue,   // 10: the X1 variant, same honest refusal
    (const void*)&unknown_slot,
    (const void*)&unknown_slot,
    (const void*)&unknown_slot,
    (const void*)&unknown_slot,
    (const void*)&unknown_slot,
};

Factory& phantom_factory() {
    static Factory f{ const_cast<void**>(FACTORY_VTBL) };
    return f;
}

}  // namespace

// HRESULT DStorageGetFactory(REFIID riid, void** factory)
extern "C" HRESULT DStorageGetFactory(const GUID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_INVALIDARG;
    if (g_factory_iid.Data1 == 0 && g_factory_iid.Data2 == 0 && g_factory_iid.Data3 == 0)
        g_factory_iid = *riid;
    return factory_qi(&phantom_factory(), riid, out);
}
