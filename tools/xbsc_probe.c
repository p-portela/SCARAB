// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// xbsc_probe.exe -- empirical signature probe for xbsc_xs.dll's XBSC_* API.
//
// The XBSC_* exports are a handle/text API: a call returns an opaque result
// handle, and the handle carries a result code plus an error string. So wrong
// arguments do not crash -- they hand back a message. That makes the error
// string the oracle for the call signature.
//
// One probe per process on purpose: a wrong guess that does fault kills only
// that probe, and runner.sh walks the matrix. See tools/xbsc_probe_notes.md for
// the Ghidra-derived shape of the call.
//
//   xbsc_probe.exe <xbsc_xs.dll> <blob.rdna2> <probe-id>
//
// Ghidra (see notes) says the entry is:
//   XBSC_Result* XBSC_DisassembleRawShader(u64, u64, u64* out, u64, XbscBuf* in)
// with XbscBuf = { void* data; u64 size; u64 version; } and `out` dereferenced
// once by the implementation, so it must not be NULL.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned int u32;

typedef struct { u64 data; u64 size; u64 version; } XbscBuf;

// Confirmed shape (see tools/xbsc_probe_notes.md): the code pointer and byte
// size are the first two arguments, the marker "out" pointer is third and must
// not be NULL, and the fourth is an optional {data,size,version} blob view.
typedef void* (__attribute__((cdecl)) *FnDisasm)(const void*, u64, u64*, XbscBuf*);
typedef u32   (__attribute__((cdecl)) *FnCode)(void*);
typedef char* (__attribute__((cdecl)) *FnMsg)(void*);
typedef int   (__attribute__((cdecl)) *FnGet)(void*, void**, void**);
typedef void  (__attribute__((cdecl)) *FnDestroy)(void*);

static unsigned char* g_code;      // blob, with 4 KB of zero guard after it
static u64 g_size;
static u64 g_out;                  // storage the implementation dereferences

static void report(const char* tag, void* h,
                   FnCode code, FnMsg msg, FnGet get_d, FnDestroy destroy) {
    u32 rc = code(h);
    const char* m = h ? msg(h) : msg(NULL);
    printf("%s handle=%p code=%u msg=\"%s\"\n", tag, h, rc, m ? m : "(null)");
    if (h && rc == 0) {
        void* text = NULL; void* len = NULL;
        int g = get_d(h, &text, &len);
        printf("%s getdisassembly=%d text=%p len=%llu\n", tag, g, text,
               (unsigned long long)(u64)len);
        if (g == 0 && text && (u64)len < 65536) {
            fwrite(text, 1, (size_t)(u64)len, stdout);
            putchar('\n');
        }
    }
    if (h) destroy(h);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { fputs("usage: xbsc_probe.exe <xbsc_xs.dll> <blob|manifest> <probe-id|batch>\n", stderr); return 2; }
    int probe = _wtoi(argv[3]);
    int batch = (wcscmp(argv[3], L"batch") == 0);
    g_size = 0;

    if (!batch) {
        if (wcscmp(argv[2], L"-") != 0) {
            FILE* f = _wfopen(argv[2], L"rb");
            if (!f) { fputs("blob open failed\n", stderr); return 2; }
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            g_code = (unsigned char*)calloc(1, (size_t)n + 4096);   // zero guard
            if (fread(g_code, 1, (size_t)n, f) != (size_t)n) { fputs("blob read failed\n", stderr); return 2; }
            fclose(f);
            g_size = (u64)n;
        } else {
            g_code = (unsigned char*)calloc(1, 4096);
        }
    }

    // make the DLL's own directory searchable for its siblings
    {
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, argv[1], MAX_PATH - 1); dir[MAX_PATH - 1] = 0;
        wchar_t* s1 = wcsrchr(dir, L'\\'); wchar_t* s2 = wcsrchr(dir, L'/');
        wchar_t* s = (s1 > s2) ? s1 : s2;
        if (s) { *s = 0; SetDllDirectoryW(dir); }
    }
    HMODULE h = LoadLibraryExW(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { printf("LOADLIBRARY_FAILED err=%lu\n", GetLastError()); return 3; }

    FnDisasm disasm = (FnDisasm)GetProcAddress(h, "XBSC_DisassembleRawShader");
    FnCode   code   = (FnCode)  GetProcAddress(h, "XBSC_GetResultCode");
    FnMsg    msg    = (FnMsg)   GetProcAddress(h, "XBSC_GetErrorMessage");
    FnGet    getd   = (FnGet)   GetProcAddress(h, "XBSC_GetDisassembly");
    FnDestroy destroy = (FnDestroy)GetProcAddress(h, "XBSC_DestroyResult");
    if (!disasm || !code || !msg || !getd || !destroy) {
        printf("RESOLVE_FAILED disasm=%p code=%p msg=%p get=%p destroy=%p\n",
               (void*)disasm, (void*)code, (void*)msg, (void*)getd, (void*)destroy);
        return 3;
    }
    printf("blob_size=%llu probe=%d\n", (unsigned long long)g_size, probe);

    // Batch mode: one DLL load, one record per manifest line. Records are
    // delimited so a consumer can parse them and so a crash names the file that
    // was in flight (stdout is flushed after every record).
    //   xbsc_probe.exe <dll> <manifest.txt> batch
    if (batch) {
        FILE* mf = _wfopen(argv[2], L"r");
        if (!mf) { fputs("manifest open failed\n", stderr); return 2; }
        size_t cap = 1u << 23;                    // 8 MB is far above the largest stream
        unsigned char* buf = (unsigned char*)calloc(1, cap);
        char line[4096];
        u64 counted = 0;
        while (fgets(line, sizeof line, mf)) {
            size_t n = strlen(line);
            while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
            if (!n) continue;
            wchar_t wpath[4096];
            MultiByteToWideChar(CP_UTF8, 0, line, -1, wpath, 4096);
            FILE* bf = _wfopen(wpath, L"rb");
            if (!bf) { printf("@@ %s rc=0 g=-2 len=0\n@@END\n", line); fflush(stdout); continue; }
            size_t sz = fread(buf, 1, cap, bf);
            fclose(bf);
            if (sz > cap) sz = cap;
            void* h = disasm(buf, (u64)sz, &g_out, NULL);
            u32 rc = h ? code(h) : 1;
            void* text = NULL; void* lenp = NULL;
            int g = (h && rc == 0) ? getd(h, &text, &lenp) : -1;
            u64 tl = (g == 0) ? (u64)lenp : 0;
            printf("@@ %s rc=%u g=%d len=%llu\n", line, rc, g, (unsigned long long)tl);
            if (g == 0 && text && tl && tl < (1u << 20)) fwrite(text, 1, (size_t)tl, stdout);
            fputs("\n@@END\n", stdout);
            fflush(stdout);
            if (h) destroy(h);
            counted++;
        }
        fclose(mf);
        free(buf);
        fprintf(stderr, "batch done: %llu blobs\n", (unsigned long long)counted);
        return 0;
    }

    u64* out = &g_out;

    switch (probe) {
    case 0: report("p0 all-null",     disasm(NULL, 0, NULL, NULL), code, msg, getd, destroy); break;
    case 1: report("p1 empty+out",    disasm(NULL, 0, out,  NULL), code, msg, getd, destroy); break;
    case 2: report("p2 WORKING CALL", disasm(g_code, g_size, out, NULL), code, msg, getd, destroy); break;
    case 3: report("p3 arg3=NULL",    disasm(g_code, g_size, NULL, NULL), code, msg, getd, destroy); break;
    case 4: report("p4 size=0",       disasm(g_code, 0, out, NULL), code, msg, getd, destroy); break;
    case 5: report("p5 size-4",       disasm(g_code, g_size > 4 ? g_size - 4 : 0, out, NULL), code, msg, getd, destroy); break;
    // arg4 must be NULL. A zeroed 0x40-byte object and a {0,0,6} triple both
    // fault (the implementation calls a field of it as a pointer), so no probe
    // passes a non-NULL arg4. See tools/xbsc_probe_notes.md.
    default: report("pX unknown probe", NULL, code, msg, getd, destroy); break;
    }
    return 0;
}
