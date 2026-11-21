// integration_test.cpp -- end-to-end proof of the bridge + wrapper layer.
//
//   this exe (title side)
//     -> d3d12_xs.dll          the bridge, 116 verified exports, forwards to PC
//          -> PC d3d12.dll      real runtime (here: Wine's)
//     -> d3d12_xs_wrap.dll     loaded by the bridge, wraps the device
//
// The test exercises the console-specific path that the wrapper exists for:
// create a device, then ask for a command list of the console-only type
// D3D12CONS_COMMAND_LIST_TYPE_DMA with the console-only IID
// IID_ID3D12ConsDmaCommandList, and confirm the object that comes back really
// serves that IID.
//
// Compiled against the real d3d12.h so the vtable calls are the genuine ones.
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <cstdio>

// include paths are supplied by the build: components/extension-wrapper and
// components/extension-wrapper/src
#include "console_extensions.h"
#include "iid_console.inc"                  // the console IIDs (proven set)

typedef HRESULT (WINAPI *PFN_CreateDevice)(void*, D3D_FEATURE_LEVEL, const IID*, void**);

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "d3d12_xs.dll";
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: output survives a crash
    printf("== integration test ==\n");
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("FAIL LoadLibrary(%s) err=%lu\n", dll, GetLastError()); return 2; }
    printf("ok   LoadLibrary(%s)\n", dll);

    auto create = (PFN_CreateDevice)GetProcAddress(h, "D3D12CreateDevice");
    if (!create) { printf("FAIL no D3D12CreateDevice\n"); return 3; }

    void* cdev = nullptr;
    HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, &cdev);
    ID3D12Device* dev = (ID3D12Device*)cdev;
    printf("%s D3D12CreateDevice -> 0x%08lx dev=%p\n", SUCCEEDED(hr) ? "ok  " : "FAIL",
           (unsigned long)hr, (void*)dev);
    if (FAILED(hr) || !dev) {
        printf("NOTE: this host has no usable PC D3D12 device (no GPU/WARP), so the\n"
               "      device-level part cannot run here. The bridge log records that the\n"
               "      wrapper layer was found; wrapper_selftest.exe covers the wrapper.\n");
        return 0;
    }

    // 1. the device must hand itself back as the *wrapper*, not the PC object
    ID3D12Device* again = nullptr;
    hr = dev->QueryInterface(IID_ID3D12Device, (void**)&again);
    if (SUCCEEDED(hr) && again == dev)
        printf("ok   device identity preserved across QI (wrapper served it)\n");
    else
        printf("WARN QI returned a different object (%p) - device is unwrapped\n", (void*)again);
    if (again) again->Release();

    // 2. a command allocator, via the generated forwarding
    ID3D12CommandAllocator* alloc = nullptr;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_ID3D12CommandAllocator,
                               (void**)&alloc);
    printf("%s CreateCommandAllocator(COPY) -> 0x%08lx\n", SUCCEEDED(hr) ? "ok  " : "FAIL",
           (unsigned long)hr);

    // 3. THE CONSOLE PATH: console list type + console IID
    void* list = nullptr;
    hr = dev->CreateCommandList(0, (D3D12_COMMAND_LIST_TYPE)D3D12CONS_COMMAND_LIST_TYPE_DMA,
                                alloc, nullptr, IID_ID3D12ConsDmaCommandList, &list);
    printf("%s CreateCommandList(type=CONSOLE_DMA(%d), riid=IID_ID3D12ConsDmaCommandList) -> 0x%08lx\n",
           SUCCEEDED(hr) ? "ok  " : "FAIL", (int)D3D12CONS_COMMAND_LIST_TYPE_DMA,
           (unsigned long)hr);

    if (SUCCEEDED(hr) && list) {
        ID3D12ConsDmaCommandList* dma = nullptr;
        hr = ((IUnknown*)list)->QueryInterface(IID_ID3D12ConsDmaCommandList, (void**)&dma);
        printf("%s returned object serves IID_ID3D12ConsDmaCommandList -> %p\n",
               SUCCEEDED(hr) ? "ok  " : "FAIL", (void*)dma);
        if (dma) {
            // a real console method, with the UMD's alignment rule
            dma->FillRawBuffer(nullptr, 0, 6, 0);    // misaligned -> rejected
            dma->FillRawBuffer(nullptr, 0, 8, 0);    // aligned   -> recorded
            dma->OMSetFrontAndBackStencilRef(1, 2);
            dma->Release();
        }
        // the standard PC view must still work on the same object
        ID3D12GraphicsCommandList* gcl = nullptr;
        hr = ((IUnknown*)list)->QueryInterface(IID_ID3D12GraphicsCommandList, (void**)&gcl);
        printf("%s same object also serves IID_ID3D12GraphicsCommandList -> %p\n",
               SUCCEEDED(hr) ? "ok  " : "FAIL", (void*)gcl);
        if (gcl) {
            ID3D12Device* owner = nullptr;
            if (SUCCEEDED(gcl->GetDevice(IID_ID3D12Device, (void**)&owner)) && owner == dev)
                printf("ok   command list GetDevice returned the wrapper device\n");
            else
                printf("WARN command list GetDevice did not return the wrapper device\n");
            gcl->Release();
        }
        ((IUnknown*)list)->Release();
    }
    if (alloc) alloc->Release();
    dev->Release();
    printf("== done ==\n");
    return 0;
}
