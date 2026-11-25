// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// The launcher's gate fixture: a PE with one app-local import and one TLS
// callback, so `scarab_launch` can prove, from a clean clone, that it maps,
// binds, runs the callbacks and reaches the entry point. The prints are the
// assertions test_loader.py reads.
#include <windows.h>
#include <stdio.h>

__declspec(dllimport) int fixture_dep_value(void);

static void NTAPI on_attach(PVOID module, DWORD reason, PVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) printf("FIXTURE_TLS\n");
}

#ifdef _MSC_VER
#pragma comment(linker, "/INCLUDE:_tls_used")
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK fixture_tls = on_attach;
#else
// GNU only emits the TLS directory when the CRT's _tls_used record is
// referenced; without this the .CRT$XLB pointer is never collected.
extern IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((used)) static void* fixture_keep_tls = (void*)&_tls_used;
PIMAGE_TLS_CALLBACK fixture_tls __attribute__((section(".CRT$XLB"))) = on_attach;
#endif

int main(void) {
    // Reaching this proves the loader bound the fixture's own import before
    // running it; reaching it through a stub would print 0.
    printf("FIXTURE_DEP=%d\n", fixture_dep_value());
    // The title asks for large pages without a fallback; the host may refuse
    // them. The launcher must still hand back memory.
    void* p = VirtualAlloc(NULL, 1 << 20, MEM_LARGE_PAGES | MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    printf("FIXTURE_LARGE_PAGES=%d\n", p != NULL);
    if (p) VirtualFree(p, 0, MEM_RELEASE);
    printf("FIXTURE_ENTRY\n");
    fflush(stdout);
    return 0;
}
