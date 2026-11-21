// peload.exe -- load a PE library under Wine and resolve every export.
//
// Independent cross-check of tools/peinfo.py: this enumerates the export
// directory from inside Windows-land via GetProcAddress, so if it agrees with
// the Python parser, both are right.
//
//   peload.exe <path-to-dll>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fputs("usage: peload.exe <dll>\n", stdout); return 2; }
    const wchar_t* path = argv[1];

    // make the DLL's own directory searchable so its sibling dependencies
    // (e.g. XBSC_XS.dll, xg_xs.dll for xgs12_pc_xs.dll) resolve
    {
        wchar_t dir[MAX_PATH];
        wcsncpy(dir, path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        wchar_t* s1 = wcsrchr(dir, L'\\');
        wchar_t* s2 = wcsrchr(dir, L'/');
        wchar_t* s = (s1 > s2) ? s1 : s2;
        if (s) { *s = 0; SetDllDirectoryW(dir); }
    }
    HMODULE h = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        printf("LOADLIBRARY_FAILED err=%lu\n", GetLastError());
        return 3;
    }
    printf("LOADLIBRARY_OK hmod=%p\n", (void*)h);

    wchar_t full[MAX_PATH];
    if (GetModuleFileNameW(h, full, MAX_PATH))
        wprintf(L"PATH %ls\n", full);

    // walk the export directory ourselves
    BYTE* base = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!rva) { fputs("NO_EXPORT_DIRECTORY\n", stdout); return 0; }
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + rva);

    DWORD* names = (DWORD*)(base + ed->AddressOfNames);
    WORD* ords = (WORD*)(base + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + ed->AddressOfFunctions);

    printf("EXPORTS base=%u count=%u named=%u\n", ed->Base, ed->NumberOfFunctions, ed->NumberOfNames);

    int resolved = 0, failed = 0;
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        DWORD ordinal = ed->Base + ords[i];
        FARPROC p = GetProcAddress(h, nm);
        if (p) {
            resolved++;
        } else {
            failed++;
            printf("UNRESOLVED %s (ordinal %lu)\n", nm, (unsigned long)ordinal);
        }
    }
    printf("RESOLVED %d/%d  failed=%d\n", resolved, (int)ed->NumberOfNames, failed);

    // pinned exports from the contract, printed with ordinals for tool comparison
    const char* pin[] = {
        "D3D12ZeroMemoryX", "PixGetPixieEntryPoints", "D3D10CreateBlob",
        "D3D12ConfigureVirtualMemoryX", "D3D12CreateDevice",
        "D3D12UploadCustomMicrocodeX", "D3D12ConsCreateDevice",
        "DXGIXPresentArray", "InternalDriverCallX",
        "IID_ID3D12Device", "IID_IGraphicsUnknown", "DXGI_DEBUG_ALL",
        "XBSC_DisassembleRawShader", "XBSC_AssembleShader", "SCCompileMultiShadersconsole",
        "OpenAdapter12", "UMDCreateDevice12", "XAudio2Create", "DxcCreateInstance",
        NULL
    };
    for (int i = 0; pin[i]; i++) {
        FARPROC p = GetProcAddress(h, pin[i]);
        if (p) {
            // report the ordinal + bytes for data exports so the table can be checked
            printf("PIN %s ptr=%p", pin[i], (void*)p);
            if (pin[i][0] == 'I' || pin[i][0] == 'D' && pin[i][1] == 'X') {
                const unsigned char* g = (const unsigned char*)p;
                printf(" data:");
                for (int k = 0; k < 16; k++) printf("%02x", g[k]);
            }
            putchar('\n');
        }
    }
    return 0;
}
