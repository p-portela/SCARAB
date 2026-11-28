// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// ===========================================================================
//  scarab_launch -- map a console title's own PE, bind its imports to this
//  layer, and run its entry point.
// ===========================================================================
// The console image is ordinary x86-64 PE32+: the machine code needs no
// translation. What it cannot find on a PC are its dependencies, so the job
// is the Windows loader's, not a hand-written mapper's:
//
//   1. patch a copy of the image so the OS maps it but runs nothing -- DLL
//      flag set, entry point cleared, import directory pointed at its empty
//      terminator, TLS callbacks detached (and the relocation that would
//      resurrect the 0 neutralised);
//   2. LoadLibraryExW that copy: the OS relocates it, allocates static TLS and
//      registers its unwind tables;
//   3. bind every import ourselves -- our shims and the host first, a named
//      stub last, so an unresolved symbol is reported, never silently zeroed;
//   4. run the TLS callbacks, then the entry point, on a thread with the stack
//      size the image asked for.
//
// `--list` stops after step 3 and only reports, so an image can be inspected
// without running a byte of its code.
//
// The title's code reads its TLS block as
// `mov r, gs:[0x58]`, assuming index 0, true for an exe and false for the
// DLL copy. Index 0 is pointed at the title's block in the entry thread and in
// every thread started through `CreateThread`, which covers the title's own
// threads. shortcut: threads the title starts any other way, and the launcher's
// own block at index 0, still read the wrong block. Upgrade: rewrite those
// loads to per-index thunks (VIctory does; 53,246 hits in this title's code).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

struct Section {
    DWORD rva, vsize, raw, rawsize;
};

struct Import {
    DWORD iat_rva;
    WORD ordinal;
    bool by_ordinal;
    size_t module;
    std::string name;
};

struct Image {
    std::vector<BYTE> bytes;
    std::vector<Section> sections;
    std::vector<Import> imports;
    std::vector<std::string> modules;
    ULONGLONG preferred_base = 0;
    DWORD entry_rva = 0;
    ULONGLONG stack_reserve = 0;
    DWORD import_rva = 0;
    DWORD import_count = 0;
    DWORD tls_rva = 0;
    ULONGLONG tls_callbacks = 0;
    ULONGLONG tls_index = 0;
    DWORD image_size = 0;
};

int usage() {
    fputs("usage: scarab_launch <image.exe> [--list] [--deps <dir>]...\n"
          "  --list       parse and bind, but run nothing\n"
          "  --deps <dir> extra directory to resolve import modules from\n", stderr);
    return 1;
}

bool read_file(const wchar_t* path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) != 0 && size.QuadPart <= 0x7fffffff;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) != 0 &&
             got == out.size();
    }
    CloseHandle(h);
    return ok;
}

const BYTE* rva_ptr(const std::vector<BYTE>& f, const std::vector<Section>& s, DWORD rva) {
    for (const Section& sec : s) {
        if (rva >= sec.rva && rva < sec.rva + sec.vsize) {
            DWORD off = sec.raw + (rva - sec.rva);
            return off < f.size() ? f.data() + off : nullptr;
        }
    }
    return nullptr;
}

// Parses the original bytes. Everything the loader needs later is copied out,
// because the patched copy no longer describes the image's real entry, imports
// or TLS callbacks.
bool parse(Image& img) {
    if (img.bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    size_t nt_off = static_cast<size_t>(dos->e_lfanew);
    if (nt_off + sizeof(IMAGE_NT_HEADERS64) > img.bytes.size()) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.bytes.data() + nt_off);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    const IMAGE_OPTIONAL_HEADER64& opt = nt->OptionalHeader;
    img.preferred_base = opt.ImageBase;
    img.entry_rva = opt.AddressOfEntryPoint;
    img.stack_reserve = opt.SizeOfStackReserve;
    img.image_size = opt.SizeOfImage;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        img.sections.push_back({sec->VirtualAddress, sec->Misc.VirtualSize,
                                sec->PointerToRawData, sec->SizeOfRawData});
    }

    DWORD import_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    img.import_rva = import_rva;
    if (import_rva) {
        auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            rva_ptr(img.bytes, img.sections, import_rva));
        for (; desc && desc->Name; desc++, img.import_count++) {
            auto* dll = reinterpret_cast<const char*>(rva_ptr(img.bytes, img.sections, desc->Name));
            if (!dll) return false;
            img.modules.emplace_back(dll);
            DWORD thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            for (DWORD i = 0;; i++) {
                auto* thunk = reinterpret_cast<const ULONGLONG*>(
                    rva_ptr(img.bytes, img.sections, thunk_rva + i * 8));
                if (!thunk || !*thunk) break;
                Import im{};
                im.iat_rva = desc->FirstThunk + i * 8;
                im.module = img.modules.size() - 1;
                if (*thunk & 0x8000000000000000ull) {
                    im.by_ordinal = true;
                    im.ordinal = static_cast<WORD>(*thunk & 0xffff);
                } else {
                    auto* hint = reinterpret_cast<const WORD*>(
                        rva_ptr(img.bytes, img.sections, static_cast<DWORD>(*thunk)));
                    if (!hint) return false;
                    im.name.assign(reinterpret_cast<const char*>(hint + 1));
                }
                img.imports.push_back(std::move(im));
            }
        }
    }

    DWORD tls_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
    if (tls_rva) {
        auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
            rva_ptr(img.bytes, img.sections, tls_rva));
        if (tls) {
            img.tls_rva = tls_rva;
            img.tls_callbacks = tls->AddressOfCallBacks;
            img.tls_index = tls->AddressOfIndex;
        }
    }
    return true;
}

// The patch victory's loader describes: the OS maps the copy and runs nothing.
bool patch(Image& img) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(img.bytes.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(img.bytes.data() + dos->e_lfanew);
    IMAGE_OPTIONAL_HEADER64& opt = nt->OptionalHeader;

    nt->FileHeader.Characteristics |= IMAGE_FILE_DLL;
    opt.AddressOfEntryPoint = 0;
    opt.CheckSum = 0;
    opt.DllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_FORCE_INTEGRITY;

    // Point the import directory at its own empty terminator: the loader sees
    // zero imports, and we bind the original slots from `img.imports`.
    if (img.import_rva) {
        DWORD terminator = img.import_rva + img.import_count * sizeof(IMAGE_IMPORT_DESCRIPTOR);
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = terminator;
        opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress = 0;
    opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size = 0;

    if (img.tls_rva) {
        auto* tls = const_cast<IMAGE_TLS_DIRECTORY64*>(
            reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(
                rva_ptr(img.bytes, img.sections, img.tls_rva)));
        if (tls) tls->AddressOfCallBacks = 0;

        // Zeroing the field is not enough: its DIR64 relocation would turn the
        // 0 into the rebase delta, and the loader would read callbacks from
        // there. Make that one entry a no-op.
        DWORD reloc_rva = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        DWORD reloc_size = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        DWORD field_rva = img.tls_rva + 24;  // AddressOfCallbacks in TLS_DIRECTORY64
        for (DWORD at = 0; at + 8 <= reloc_size;) {
            auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                rva_ptr(img.bytes, img.sections, reloc_rva + at));
            if (!block || block->SizeOfBlock < 8) break;
            auto* entry = reinterpret_cast<WORD*>(const_cast<IMAGE_BASE_RELOCATION*>(block) + 1);
            DWORD n = (block->SizeOfBlock - 8) / 2;
            for (DWORD i = 0; i < n; i++) {
                if ((entry[i] >> 12) == IMAGE_REL_BASED_DIR64 &&
                    block->VirtualAddress + (entry[i] & 0xfff) == field_rva)
                    entry[i] = IMAGE_REL_BASED_ABSOLUTE;
            }
            at += block->SizeOfBlock;
        }
    }
    return true;
}

bool write_temp(const std::wstring& name, const std::vector<BYTE>& bytes, std::wstring& out) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return false;
    out = dir;
    out += L"scarab-";
    out += std::to_wstring(GetCurrentProcessId());
    out += L"-";
    out += name;
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) != 0 &&
              wrote == bytes.size();
    CloseHandle(h);
    return ok;
}

// One stub for every unresolved import: it ignores its arguments (x64 passes
// them in registers) and returns 0. Each stub is its own small trampoline that
// names itself before returning, so a title that dies on a missing function
// says which one instead of just dying.
unsigned g_stub_calls = 0;

extern "C" void stub_called(const char* name) {
    if (g_stub_calls < 200) printf("scarab_launch: stub called: %s\n", name);
    else if (g_stub_calls == 200) printf("scarab_launch: ... further stub calls not shown\n");
    g_stub_calls++;
    fflush(stdout);
}

// x64 trampoline: save the argument registers, name the symbol, return 0.
// `mov r11, imm64` keeps the name out of the argument registers; five pushes
// leave rsp 16-byte aligned for the call.
// The stub pool lives at file scope so it can be write-protected once binding is
// done. Stubs are only created while imports are being bound; nothing writes them
// afterwards, so a permanent RWX region is an unnecessary target.
static BYTE* g_stub_pool = nullptr;
static size_t g_stub_used = 0;
static bool g_stub_pool_sealed = false;
constexpr size_t STUB_SLOT = 48;
constexpr size_t STUB_POOL = 64 * 1024;

FARPROC make_stub(const std::string& name) {
    if (!g_stub_pool)
        g_stub_pool = static_cast<BYTE*>(
            VirtualAlloc(nullptr, STUB_POOL, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stub_pool || g_stub_used + STUB_SLOT > STUB_POOL) return nullptr;
    // A late stub (created after sealing) reopens the pool just for the write and
    // re-seals it below, so the read+execute guarantee is never quietly dropped.
    DWORD reopened = 0;
    if (g_stub_pool_sealed)
        VirtualProtect(g_stub_pool, STUB_POOL, PAGE_EXECUTE_READWRITE, &reopened);
    char* copy = static_cast<char*>(malloc(name.size() + 1));
    if (!copy) return nullptr;
    memcpy(copy, name.c_str(), name.size() + 1);

    BYTE* code = g_stub_pool + g_stub_used;
    g_stub_used += STUB_SLOT;
    size_t i = 0;
    auto emit = [&](std::initializer_list<BYTE> bytes) {
        for (BYTE b : bytes) code[i++] = b;
    };
    auto emit64 = [&](ULONGLONG v) {
        for (int k = 0; k < 8; k++) code[i++] = static_cast<BYTE>(v >> (k * 8));
    };
    emit({0x49, 0xBB});
    emit64(reinterpret_cast<ULONGLONG>(copy));  // mov r11, name
    emit({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51});  // push rax rcx rdx r8 r9
    emit({0x4C, 0x89, 0xD9});                    // mov rcx, r11
    emit({0x48, 0xB8});
    emit64(reinterpret_cast<ULONGLONG>(&stub_called));  // mov rax, fn
    emit({0xFF, 0xD0});                          // call rax
    emit({0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r9 r8 rdx rcx rax
    emit({0x31, 0xC0, 0xC3});                    // xor eax, eax; ret
    FlushInstructionCache(GetCurrentProcess(), code, i);
    if (g_stub_pool_sealed) {
        DWORD ignored = 0;
        VirtualProtect(g_stub_pool, STUB_POOL, PAGE_EXECUTE_READ, &ignored);
    }
    return reinterpret_cast<FARPROC>(code);
}

// Binding writes the stubs and nothing writes them afterwards, so drop the pool to
// read+execute. A permanent RWX region turns any stray write anywhere in the title
// into code execution; there is no reason to hand that over.
void seal_stub_pool() {
    if (!g_stub_pool || g_stub_pool_sealed) return;
    DWORD old = 0;
    if (!VirtualProtect(g_stub_pool, STUB_POOL, PAGE_EXECUTE_READ, &old)) {
        fputs("scarab_launch: could not write-protect the import stub pool\n", stderr);
        return;
    }
    g_stub_pool_sealed = true;
    printf("scarab_launch: import stub pool sealed RX (%zu of %zu bytes used)\n",
           g_stub_used, STUB_POOL);
    fflush(stdout);
}

// The console guarantees large pages and exposes a physical-memory pool, so the
// title asks for both without a fallback and treats NULL as fatal. On the host
// those are ordinary pages: only the granularity and the pool name differ, and
// Windows rejects MEM_PHYSICAL with MEM_COMMIT outright.
// shortcut: large/physical requests are served as normal pages. Upgrade: back
// them with a large-page section when the host supports one.
void* WINAPI host_VirtualAlloc(void* address, size_t size, uint32_t type, uint32_t protect) {
    type &= ~MEM_LARGE_PAGES;
    if (type & MEM_COMMIT) type &= ~MEM_PHYSICAL;
    return VirtualAlloc(address, size, type, protect);
}

// The title's code reads its static TLS through gs:[0x58][0], assuming its own
// block is TLS index 0 -- true for an EXE (it is the first module loaded), not
// for an EXE loaded as a DLL: it got index 1 and index 0 is the launcher's.
// Point index 0 at the title's block in every thread the title starts.
// shortcut: only CreateThread is wrapped, and the launcher's own block at index
// 0 is replaced (it prints and returns, which needs nothing from it). Upgrade:
// rewrite the gs:[0x58] loads to use the title's index, the way VIctory does.
std::atomic<DWORD> g_title_tls_index{0};

void point_tls0_at_title() {
    auto** arr = reinterpret_cast<void**>(__readgsqword(0x58));
    DWORD idx = g_title_tls_index.load();
    if (arr && idx && idx < 4096) arr[0] = arr[idx];
}

using CreateThreadFn = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID,
                                      DWORD, LPDWORD);
CreateThreadFn real_CreateThread() {
    static CreateThreadFn fn = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<CreateThreadFn>(GetProcAddress(k, "CreateThread")) : nullptr;
    }();
    return fn;
}

struct ThreadStart {
    LPTHREAD_START_ROUTINE fn;
    LPVOID arg;
};

DWORD WINAPI tls_start(LPVOID param) {
    auto* s = static_cast<ThreadStart*>(param);
    LPTHREAD_START_ROUTINE fn = s->fn;
    LPVOID arg = s->arg;
    delete s;
    point_tls0_at_title();
    return fn(arg);
}

HANDLE WINAPI host_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn,
                                LPVOID arg, DWORD flags, LPDWORD id) {
    if (!fn || !g_title_tls_index.load()) return real_CreateThread()(sa, stack, fn, arg, flags, id);
    auto* s = new ThreadStart{fn, arg};
    if (!s) return nullptr;
    HANDLE h = real_CreateThread()(sa, stack, tls_start, s, flags, id);
    if (!h) delete s;
    return h;
}

// The title asks for xgameruntime.dll with LOAD_LIBRARY_SEARCH_SYSTEM32 only:
// it wants the platform's GDK runtime, not one that happens to sit next to it.
// In this process the scarab shim *is* that runtime, so a load of a name we
// ship is answered from the --deps dirs instead of the host's System32.
std::vector<std::wstring> g_shim_dirs;

using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExWFn real_LoadLibraryExW() {
    static LoadLibraryExWFn fn = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<LoadLibraryExWFn>(GetProcAddress(k, "LoadLibraryExW"))
                 : nullptr;
    }();
    return fn;
}

HMODULE WINAPI host_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (name) {
        std::wstring base(name);
        size_t slash = base.find_last_of(L"\\/");
        if (slash != std::wstring::npos) base = base.substr(slash + 1);
        for (const std::wstring& dir : g_shim_dirs) {
            std::wstring full = dir + L"\\" + base;
            if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            HMODULE hm = real_LoadLibraryExW()(full.c_str(), nullptr, 0);
            if (hm) {
                printf("scarab_launch: override %ls -> %ls\n", name, full.c_str());
                fflush(stdout);
                return hm;
            }
            printf("scarab_launch: %ls failed to load: error %lu\n", full.c_str(), GetLastError());
            fflush(stdout);
        }
    }
    return real_LoadLibraryExW()(name, file, flags);
}

FARPROC override_for(const std::string& module, const std::string& name) {
    std::string m = module;
    for (char& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    // The title imports both through their api-sets, so match those families too.
    bool memory_api = m == "kernel32.dll" || m == "kernelbase.dll" ||
                      m.compare(0, 23, "api-ms-win-core-memory-") == 0;
    bool loader_api = m == "kernel32.dll" || m == "kernelbase.dll" ||
                      m.compare(0, 30, "api-ms-win-core-libraryloader-") == 0;
    if (name == "VirtualAlloc" && memory_api)
        return reinterpret_cast<FARPROC>(&host_VirtualAlloc);
    bool thread_api = m == "kernel32.dll" || m == "kernelbase.dll" ||
                      m.compare(0, 38, "api-ms-win-core-processthreads-") == 0;
    if (name == "CreateThread" && thread_api)
        return reinterpret_cast<FARPROC>(&host_CreateThread);
    if (name == "LoadLibraryExW" && loader_api)
        return reinterpret_cast<FARPROC>(&host_LoadLibraryExW);
    return nullptr;
}

int bind(const Image& img, ULONG_PTR base, const std::vector<std::wstring>& deps) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    for (const std::wstring& d : deps) {
        // AddDllDirectory refuses a relative path; the caller's is relative to
        // its own cwd, which is the only meaning it can have here.
        wchar_t full[MAX_PATH];
        DWORD n = GetFullPathNameW(d.c_str(), MAX_PATH, full, nullptr);
        if (!n || n >= MAX_PATH || !AddDllDirectory(full))
            printf("scarab_launch: --deps %ls: unusable (error %lu)\n", d.c_str(), GetLastError());
        else
            g_shim_dirs.emplace_back(full);
    }

    std::vector<HMODULE> modules;
    for (const std::string& name : img.modules) {
        std::wstring wide(name.begin(), name.end());
        HMODULE hm = LoadLibraryExW(wide.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (!hm)
            printf("scarab_launch: module %s: not loadable (error %lu), its imports become stubs\n",
                   name.c_str(), GetLastError());
        modules.push_back(hm);
    }

    unsigned bound = 0, missing = 0;
    std::vector<std::string> missing_names;
    for (const Import& im : img.imports) {
        FARPROC fn = nullptr;
        HMODULE hm = modules[im.module];
        if (!im.by_ordinal) fn = override_for(img.modules[im.module], im.name);
        if (!fn && hm) {
            if (im.by_ordinal)
                fn = GetProcAddress(hm, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(im.ordinal)));
            else
                fn = GetProcAddress(hm, im.name.c_str());
        }
        if (fn) {
            bound++;
            if (!im.by_ordinal && override_for(img.modules[im.module], im.name))
                printf("scarab_launch: override %s!%s\n", img.modules[im.module].c_str(),
                       im.name.c_str());
        } else {
            std::string label = img.modules[im.module] + "!" +
                                (im.by_ordinal ? "#" + std::to_string(im.ordinal) : im.name);
            fn = make_stub(label);
            missing++;
            missing_names.push_back(std::move(label));
        }
        auto* dst = reinterpret_cast<ULONGLONG*>(base + im.iat_rva);
        DWORD old = 0;
        VirtualProtect(dst, sizeof(*dst), PAGE_READWRITE, &old);
        *dst = reinterpret_cast<ULONGLONG>(fn);
        VirtualProtect(dst, sizeof(*dst), old, &old);
    }
    printf("scarab_launch: imports %zu: %u bound, %u stubbed\n", img.imports.size(), bound, missing);
    size_t show = missing_names.size() < 30 ? missing_names.size() : 30;
    for (size_t i = 0; i < show; i++) printf("scarab_launch:   stub %s\n", missing_names[i].c_str());
    if (missing_names.size() > show)
        printf("scarab_launch:   ... and %zu more\n", missing_names.size() - show);
    return 0;
}

struct Run {
    ULONG_PTR base;
    DWORD entry_rva;
    ULONGLONG callbacks_va;
    ULONGLONG preferred_base;
    ULONGLONG tls_index_va;
    ULONG result;
};

// A trap address alone does not name its caller, and the title's own report
// says "System Init" and nothing else. Print the fault and a stack scan of code
// pointers (each as a title RVA or module+RVA) before the host and the title
// get their chance to handle it.
ULONG_PTR g_base = 0;
DWORD g_size = 0;

const char* module_of(ULONG_PTR a, ULONG_PTR* off) {
    if (a >= g_base && a < g_base + g_size) {
        *off = a - g_base;
        return "<title>";
    }
    thread_local char name[MAX_PATH];
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    const char* found = nullptr;
    if (Module32FirstW(snap, &me)) {
        do {
            if (a >= reinterpret_cast<ULONG_PTR>(me.modBaseAddr) &&
                a < reinterpret_cast<ULONG_PTR>(me.modBaseAddr) + me.modBaseSize) {
                *off = a - reinterpret_cast<ULONG_PTR>(me.modBaseAddr);
                WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, name, MAX_PATH, nullptr, nullptr);
                found = name;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

LONG WINAPI crash_report(EXCEPTION_POINTERS* ep) {
    // OutputDebugString and thread naming raise exceptions by design.
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 || code == 0x406D1388) return EXCEPTION_CONTINUE_SEARCH;
    auto* c = ep->ContextRecord;
    ULONG_PTR off = 0;
    const char* at = module_of(c->Rip, &off);
    printf("scarab_launch: fault %08lx at %p (%s+0x%llx) rsp=%p rcx=%p rdx=%p r8=%p r9=%p\n",
           static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
           reinterpret_cast<void*>(c->Rip), at ? at : "?", static_cast<unsigned long long>(off),
           reinterpret_cast<void*>(c->Rsp), reinterpret_cast<void*>(c->Rcx),
           reinterpret_cast<void*>(c->Rdx), reinterpret_cast<void*>(c->R8),
           reinterpret_cast<void*>(c->R9));
    auto* sp = reinterpret_cast<ULONG_PTR*>(c->Rsp);
    for (int i = 0; i < 64; i++) {
        ULONG_PTR v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), &sp[i], &v, sizeof v, nullptr)) break;
        if (!v) continue;
        ULONG_PTR o = 0;
        const char* m = module_of(v, &o);
        if (m) printf("scarab_launch:   stack[%d] %s+0x%llx\n", i, m,
                      static_cast<unsigned long long>(o));
    }
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI run_thread(void* param) {
    auto* r = static_cast<Run*>(param);
    fflush(stdout); // the bind report must survive a trap in the title
    seal_stub_pool(); // binding is finished, so the stubs are final
    if (r->tls_index_va) {
        auto* idx = reinterpret_cast<DWORD*>(r->base + (r->tls_index_va - r->preferred_base));
        g_title_tls_index.store(*idx);
        point_tls0_at_title();
    }
    if (r->callbacks_va) {
        auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(
            r->base + (r->callbacks_va - r->preferred_base));
        for (; *cb; cb++) (*cb)(reinterpret_cast<PVOID>(r->base), DLL_PROCESS_ATTACH, nullptr);
        printf("scarab_launch: TLS callbacks ran\n");
    }
    using Entry = ULONG(WINAPI*)(PVOID);
    auto entry = reinterpret_cast<Entry>(r->base + r->entry_rva);
    r->result = entry(nullptr);
    return 0;
}

}  // namespace

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) return usage();
    std::wstring path;
    bool list = false;
    std::vector<std::wstring> deps;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--list")) {
            list = true;
        } else if (!wcscmp(argv[i], L"--deps") && i + 1 < argc) {
            deps.emplace_back(argv[++i]);
        } else if (path.empty() && argv[i][0] != L'-') {
            path = argv[i];
        } else {
            return usage();
        }
    }
    if (path.empty()) return usage();

    Image img;
    if (!read_file(path.c_str(), img.bytes)) {
        fwprintf(stderr, L"scarab_launch: cannot read %s\n", path.c_str());
        return 2;
    }
    if (!parse(img)) {
        fwprintf(stderr, L"scarab_launch: %s is not an x86-64 PE32+ image\n", path.c_str());
        return 2;
    }
    printf("scarab_launch: image %ls entry=0x%lx stack=0x%llx modules=%zu imports=%zu tls=%s\n",
           path.c_str(), img.entry_rva, img.stack_reserve, img.modules.size(), img.imports.size(),
           img.tls_callbacks ? "yes" : "no");

    if (!patch(img)) return 2;
    std::wstring base_name(path);
    size_t slash = base_name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) base_name = base_name.substr(slash + 1);
    std::wstring temp;
    if (!write_temp(base_name, img.bytes, temp)) {
        fputs("scarab_launch: cannot write the patched copy\n", stderr);
        return 2;
    }
    HMODULE module = LoadLibraryExW(temp.c_str(), nullptr, 0);
    if (!module) {
        fwprintf(stderr, L"scarab_launch: LoadLibraryExW(%s) failed: %lu\n", temp.c_str(),
                 GetLastError());
        return 3;
    }
    printf("scarab_launch: mapped at %p (preferred %p)\n", static_cast<void*>(module),
           reinterpret_cast<void*>(img.preferred_base));

    g_base = reinterpret_cast<ULONG_PTR>(module);
    g_size = img.image_size;
    bind(img, reinterpret_cast<ULONG_PTR>(module), deps);

    if (list) {
        puts("scarab_launch: --list, not running the entry point");
        return 0;
    }
    if (!img.entry_rva) {
        fputs("scarab_launch: image has no entry point\n", stderr);
        return 4;
    }

    AddVectoredExceptionHandler(1, crash_report);
    Run run{};
    run.base = reinterpret_cast<ULONG_PTR>(module);
    run.entry_rva = img.entry_rva;
    run.callbacks_va = img.tls_callbacks;
    run.tls_index_va = img.tls_index;
    run.preferred_base = img.preferred_base;
    SIZE_T stack = static_cast<SIZE_T>(img.stack_reserve < (1u << 20) ? (1u << 20) : img.stack_reserve);
    HANDLE thread = CreateThread(nullptr, stack, run_thread, &run, 0, nullptr);
    if (!thread) {
        fputs("scarab_launch: CreateThread failed\n", stderr);
        return 5;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    printf("scarab_launch: entry returned %lu\n", run.result);
    return static_cast<int>(run.result);
}
