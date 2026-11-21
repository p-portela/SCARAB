// dxc_emit_test.cpp -- can SCARAB produce DXIL locally?
//
// The shader bridge needs an emitter. Writing a DXIL backend means generating
// LLVM 3.7 bitcode with dx.* intrinsics, which is a serious undertaking. The far
// cheaper route is to emit *HLSL* from our IR and let DXC do it -- and DXC is
// already sitting in the vendor stack (dxcompiler.dll).
//
// This program proves that half of the pipeline works, here, today: it loads
// dxcompiler.dll, compiles a trivial shader, and inspects the result for a real
// DXIL container. If this passes, the remaining shader work is only
// RDNA2 -> HLSL, and nothing else.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>   // dxcapi.h needs IUnknown; WIN32_LEAN_AND_MEAN does not pull it in
#include <cstdio>
#include <cstring>

#include "dxcapi.h"
#include "dxc_iids.h"  // clang-mingw ignores __declspec(uuid), so __uuidof is unusable

typedef HRESULT(WINAPI* PFN_DxcCreateInstance)(REFCLSID, REFIID, LPVOID*);

static bool contains(const unsigned char* p, size_t n, const char* needle, size_t nl) {
    if (n < nl) return false;
    for (size_t i = 0; i + nl <= n; i++)
        if (memcmp(p + i, needle, nl) == 0) return true;
    return false;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dll = (argc > 1) ? argv[1] : "dxcompiler.dll";
    const char* out = (argc > 2) ? argv[2] : "test.dxil";

    static const char kHlsl[] =
        "float4 main(float4 pos : SV_Position) : SV_Target { return pos * 0.5f; }\n";

    printf("== DXC DXIL emitter test ==\n");
    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        // 77 = SKIP (CTest SKIP_RETURN_CODE): dxcompiler.dll is a vendor artifact,
        // so its absence must not fail the suite on a machine without it.
        printf("SKIP cannot load %s (err=%lu) - vendor artifact not linked\n", dll, GetLastError());
        return 77;
    }
    printf("ok   loaded %s\n", dll);

    auto create = (PFN_DxcCreateInstance)GetProcAddress(h, "DxcCreateInstance");
    if (!create) { printf("FAIL no DxcCreateInstance\n"); return 3; }
    printf("ok   DxcCreateInstance resolved\n");

    // --- utils: wrap the source text in a DxcBuffer -------------------------
    IDxcUtils* utils = nullptr;
    HRESULT hr = create(CLSID_DxcUtils, IID_IDxcUtils, (LPVOID*)&utils);
    if (FAILED(hr) || !utils) { printf("FAIL DxcCreateInstance(Utils) 0x%08lx\n", (unsigned long)hr); return 4; }
    IDxcBlobEncoding* src = nullptr;
    hr = utils->CreateBlobFromPinned(kHlsl, (UINT32)strlen(kHlsl), DXC_CP_UTF8, &src);
    if (FAILED(hr) || !src) { printf("FAIL CreateBlobFromPinned 0x%08lx\n", (unsigned long)hr); return 5; }
    printf("ok   source blob created\n");

    // --- compiler ----------------------------------------------------------
    IDxcCompiler3* compiler = nullptr;
    hr = create(CLSID_DxcCompiler, IID_IDxcCompiler3, (LPVOID*)&compiler);
    if (FAILED(hr) || !compiler) { printf("FAIL DxcCreateInstance(Compiler) 0x%08lx\n", (unsigned long)hr); return 6; }
    printf("ok   IDxcCompiler3 obtained\n");

    const wchar_t* args[] = {
        L"-T", L"ps_6_0",
        L"-E", L"main",
        L"-Vd",            // skip validation: avoids needing dxil.dll
        L"-Zi",            // debug info: prove we get real DXIL, not a stub
    };
    DxcBuffer buf = {};
    buf.Ptr = src->GetBufferPointer();
    buf.Size = src->GetBufferSize();
    buf.Encoding = DXC_CP_UTF8;

    IDxcResult* result = nullptr;
    hr = compiler->Compile(&buf, args, _countof(args), nullptr,
                           IID_IDxcResult, (void**)&result);
    if (FAILED(hr) || !result) { printf("FAIL Compile 0x%08lx\n", (unsigned long)hr); return 7; }

    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    printf("%s compile status 0x%08lx\n", SUCCEEDED(status) ? "ok  " : "FAIL", (unsigned long)status);
    if (FAILED(status)) {
        IDxcBlobEncoding* err = nullptr;
        if (SUCCEEDED(result->GetErrorBuffer(&err)) && err) {
            const char* msg = (const char*)err->GetBufferPointer();
            printf("      dxc says: %.*s\n", (int)err->GetBufferSize(), msg ? msg : "(null)");
        }
        return 8;
    }

    IDxcBlob* blob = nullptr;
    hr = result->GetResult(&blob);
    if (FAILED(hr) || !blob) { printf("FAIL GetResult 0x%08lx\n", (unsigned long)hr); return 9; }

    const unsigned char* p = (const unsigned char*)blob->GetBufferPointer();
    size_t n = blob->GetBufferSize();
    printf("ok   DXIL blob: %zu bytes\n", n);

    // Verify the CONTAINER, not just a magic anywhere in the blob. This matters:
    // the outer fourcc is "DXBC", which itself ends in "BC", so a naive search for
    // "BC\xC0\xDE" matches the fourcc plus the first two bytes of the hash. That
    // false positive is exactly how an earlier check wrongly concluded the game's
    // corpus contained bitcode. Parse the parts instead.
    auto u32 = [&](size_t o) { return (unsigned)p[o] | ((unsigned)p[o+1] << 8)
                                    | ((unsigned)p[o+2] << 16) | ((unsigned)p[o+3] << 24); };
    bool okFourcc = (n > 32 && memcmp(p, "DXBC", 4) == 0);
    unsigned nparts = okFourcc ? u32(28) : 0;
    printf("%s outer container fourcc 'DXBC' (%u parts)\n", okFourcc ? "ok  " : "FAIL", nparts);

    int dxilPart = -1;
    for (unsigned i = 0; i < nparts && 32 + i * 4 + 8 < n; i++) {
        size_t off = u32(32 + i * 4);
        if (off + 8 > n) break;
        printf("     part %-4.4s size=%u\n", (const char*)(p + off), u32(off + 4));
        if (memcmp(p + off, "DXIL", 4) == 0) dxilPart = (int)off;
    }

    bool nested = false, bitcode = false;
    if (dxilPart >= 0) {
        const unsigned char* q = p + dxilPart + 8;   // part payload
        nested  = (memcmp(q + 8, "DXIL", 4) == 0);   // nested container tag
        // The bitcode payload begins at +24 in the nested container. This offset
        // is also where the CONSOLE container puts its XDAT microcode blob -- same
        // part, same offset, different payload. That equivalence is the whole
        // basis of the shader bridge's container strategy.
        bitcode = (memcmp(q + 24, "BC\xC0\xDE", 4) == 0);
    }
    printf("%s nested 'DXIL' tag in the DXIL part\n", nested ? "ok  " : "FAIL");
    printf("%s LLVM bitcode magic 'BC C0 DE' at payload+24\n", bitcode ? "ok  " : "FAIL");

    FILE* f = fopen(out, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); printf("ok   wrote %s\n", out); }

    blob->Release(); result->Release(); compiler->Release();
    src->Release(); utils->Release();
    const bool pass = okFourcc && nested && bitcode;
    printf(pass ? "== RESULT: PASS (real DXIL produced locally) ==\n"
               : "== RESULT: FAIL ==\n");
    return pass ? 0 : 1;
}
