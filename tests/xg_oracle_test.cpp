// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pedro Portela
// xg_oracle_test.cpp -- compares our shim against the vendor xg_xs.dll.
//
// The vendor library is the only oracle this project has: scarab cannot
// verify the shim's semantics by reading the DLL's C (it is pseudocode), so
// it calls the real library and compares. Never guesses the answer twice.
//
// Usage:  xg_oracle_test.exe <path to vendor xg_xs.dll>
// Exit codes: 0 = match, 1 = mismatch, 77 = vendor DLL missing (skip)

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "memory_adapter.h"

typedef int32_t (WINAPI *PFN_Suggest)(const uint32_t*, uint32_t, uint32_t*);
typedef uint8_t  (WINAPI *PFN_Validate)(uint64_t, uint64_t, uint32_t);
// XGComputeOptimalSwizzleMode(param_1, param_2, param_3, param_4, param_5, param_6,
//                             param_7, param_8): eight positional args.
typedef uint32_t (WINAPI *PFN_Optimal)(int, int, uint32_t, uint32_t, uint64_t, int, uint32_t, uint32_t);
// XGComputeTileShape(param_1, param_2, param_3, param_4, param_5, param_6):
// six positional args; param_5 / param_6 are 3-uint outputs.
typedef int32_t (WINAPI *PFN_TileShape)(int, int, uint32_t, int, int*, int*);

static int fails = 0;
static void check(bool ok, const char* what, int32_t shim, int32_t vendor, uint64_t vendor_arg3) {
    printf("%s %s (shim=%d vendor=%d %llu)\n", ok ? "ok  " : "FAIL", what, shim, vendor, (unsigned long long)vendor_arg3);
    if (!ok) fails++;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "xg_xs.dll";
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        printf("SKIP: vendor xg_xs.dll not found at '%s'\n", path);
        return 77;
    }
    auto suggest = (PFN_Suggest)GetProcAddress(h, "XGSuggestSwizzleMode");
    auto validate = (PFN_Validate)GetProcAddress(h, "XGValidateDepthStencilSwizzleMode");
    auto optimal = (PFN_Optimal)GetProcAddress(h, "XGComputeOptimalSwizzleMode");
    auto tileshape = (PFN_TileShape)GetProcAddress(h, "XGComputeTileShape");
    if (!suggest || !validate || !optimal || !tileshape) {
        printf("SKIP: vendor xg_xs.dll lacks %s\n",
               !suggest ? "XGSuggestSwizzleMode" : (!validate ? "XGValidateDepthStencilSwizzleMode" : (!optimal ? "XGComputeOptimalSwizzleMode" : "XGComputeTileShape")));
        return 77;
    }

    // --- XGSuggestSwizzleMode: linear, render-target bit, surface, unknown
    // kind. The descriptor is uint32-indexed (desc[0] kind, desc[0xc] flags,
    // desc[4]/desc[6] w/h) -- see the XGSuggestSwizzleMode implementation.
    struct { uint8_t bytes[0x40]; } cases[4];
    memset(cases, 0, sizeof(cases));
    // case 0: linear kind
    *(uint32_t*)(cases[0].bytes + 0x00) = 1;
    // case 1: render-target-ish (bit 1 of the flags word at desc[0xc])
    *(uint32_t*)(cases[1].bytes + 0x00) = 3;
    *(uint32_t*)(cases[1].bytes + 0x30) = 2;
    // case 2: plain surface, small area
    *(uint32_t*)(cases[2].bytes + 0x00) = 3;
    *(uint32_t*)(cases[2].bytes + 0x10) = 16;
    *(uint32_t*)(cases[2].bytes + 0x18) = 16;
    // case 3: plain surface, big area
    *(uint32_t*)(cases[3].bytes + 0x00) = 3;
    *(uint32_t*)(cases[3].bytes + 0x10) = 1024;
    *(uint32_t*)(cases[3].bytes + 0x18) = 1024;

    for (int i = 0; i < 4; i++) {
        uint32_t shim_out = 0, vendor_out = 0xdeadbeef;
        int32_t shim_hr = XGSuggestSwizzleMode((const uint32_t*)cases[i].bytes, 0, &shim_out);
        int32_t vend_hr = suggest((const uint32_t*)cases[i].bytes, 0, &vendor_out);
        printf("case %d: shim hr=%d raw=%u vendor hr=%d raw=%u\n",
               i, shim_hr, shim_out, vend_hr, vendor_out);
        // The vendor is the oracle: values must be equal. If the vendor
        // returns E_FAIL, both must agree on that.
        check(shim_hr == vend_hr, "XGSuggestSwizzleMode HRESULT", shim_hr, vend_hr, (uint64_t)vendor_out);
        if (vend_hr == 0) {
            check(shim_out == vendor_out, "XGSuggestSwizzleMode mode", shim_out, vendor_out, 0);
        }
    }

    // --- XGValidateDepthStencilSwizzleMode: the 7 allowed modes + rejections
    static const uint32_t modes[] = { 4, 8, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0, 1, 2, 0x06, 0x16, 0x20 };
    for (uint32_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        uint32_t mode = modes[m];
        int32_t shim = scarab_validate_depth_stencil_swizzle_mode(mode) == 0 ? 1 : 0;
        uint8_t vendor = validate(0, 0, mode);
        check(shim == (vendor == 0 ? 1 : 0), "XGValidateDepthStencilSwizzleMode mode", shim, vendor ? 0 : 1, mode);
    }

    // --- XGComputeOptimalSwizzleMode: compare the shim's positional port against
    // the vendor on identical argument tuples (positional args: 1=layout, 2=shape,
    // 3=w, 4=h, 5=0, 6=0, 7=flags, 8=0). Bytes below are what the DLL reads.
    struct { int layout; uint32_t flags; uint32_t w; uint32_t h; } tuples[] = {
        { 1, 0x00, 16, 16 },
        { 2, 0x00, 16, 16 },
        { 3, 0x40, 16, 16 },
        { 3, 0x00, 16, 16 },
        { 4, 0x40, 16, 16 },
        { 4, 0x00, 16, 16 },
        { 4, 0x20, 16, 16 },
        { 4, 0x00, 1024, 1024 },
    };
    for (size_t i = 0; i < sizeof(tuples) / sizeof(tuples[0]); i++) {
        auto& t = tuples[i];
        uint32_t shim_out = 0xdeadbeef;
        int32_t shim_hr = scarab_optimal_swizzle_mode(t.layout, t.flags, t.w, t.h, 0u, &shim_out);
        uint32_t vendor = optimal(t.layout, 0, t.w, t.h, 0, 0, t.flags, 0);
        printf("case 4: shim hr=%d raw=%u vendor=%u\n", shim_hr, shim_out, vendor);
        check(shim_hr == 0 && shim_out == vendor, "XGComputeOptimalSwizzleMode", shim_out, vendor, 0);
    }

    // --- XGComputeTileShape: linear, surfaces, refused kinds. Positional
    // args (1,2,3,4,5,6): 1=kind, 2=descriptor (0 here), 3=tile size,
    // 4=elements per side, 5/6 = 3-uint outputs.
    struct { int p1; int p2; uint32_t p3; int p4; } tshape[] = {
        { 1, 0, 1, 1 },   // linear
        { 1, 0, 2, 1 },   // linear, refused tile size
        { 3, 0, 1, 1 },   // 1x1 surface, single element
        { 3, 0, 2, 2 },   // 2x1 surface, double elements
        { 3, 0, 4, 2 },   // 4x1 surface
        { 3, 0, 8, 4 },   // 8x2 surface, quad elements
        { 3, 0, 16, 8 },  // 16x4 surface, oct elements
        { 3, 0, 1, 4 },   // 2x0 surface, quad elements
        { 3, 0, 1, 8 },   // 1x0 surface, oct elements
        { 3, 0, 99, 1 },  // surface, refused tile size
        { 2, 0, 1, 1 },   // refused kind
        { 4, 0, 1, 1 },   // refused kind
    };
    for (size_t i = 0; i < sizeof(tshape) / sizeof(tshape[0]); i++) {
        auto& t = tshape[i];
        int shim_out5[3] = {0, 0, 0}, shim_out6[3] = {0, 0, 0};
        int vend_out5[3] = {0, 0, 0}, vend_out6[3] = {0, 0, 0};
        int32_t shim_hr = XGComputeTileShape(t.p1, t.p2, t.p3, t.p4, shim_out5, shim_out6);
        int32_t vend_hr = tileshape(t.p1, t.p2, t.p3, t.p4, vend_out5, vend_out6);
        printf("case 5: hr=%d/%d out5=(%d,%d,%d)/(%d,%d,%d) out6=(%d,%d,%d)/(%d,%d,%d)\n",
               shim_hr, vend_hr,
               shim_out5[0], shim_out5[1], shim_out5[2],
               vend_out5[0], vend_out5[1], vend_out5[2],
               shim_out6[0], shim_out6[1], shim_out6[2],
               vend_out6[0], vend_out6[1], vend_out6[2]);
        bool hr_ok = shim_hr == vend_hr;
        bool out_ok = shim_out5[0] == vend_out5[0] && shim_out5[1] == vend_out5[1] && shim_out5[2] == vend_out5[2] &&
                      shim_out6[0] == vend_out6[0] && shim_out6[1] == vend_out6[1] && shim_out6[2] == vend_out6[2];
        check(hr_ok && out_ok, "XGComputeTileShape", hr_ok ? 1 : 0, out_ok ? 1 : 0, 0);
    }

    printf(fails == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return fails;
}
