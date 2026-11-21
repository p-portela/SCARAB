// selftest_main.cpp -- runs the wrapper layer's own logic with no GPU and no
// device object, so the QI routing, refcounting, validation and unknown-slot
// logging can be verified on any machine (including under Wine on Linux).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

extern "C" int scarab_wrapper_selftest(void);

int main() {
    printf("d3d12xs wrapper self-test\n");
    int fails = scarab_wrapper_selftest();
    printf(fails == 0 ? "RESULT: PASS (0 failures)\n"
                      : "RESULT: FAIL\n");
    printf("see the .wrap.log next to this executable for the full trace\n");
    return fails;
}
