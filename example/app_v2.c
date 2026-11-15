// app_v2.c — APPLICATION IMAGE, VERSION 2.
//
// A different source file with different arithmetic: v2 says a different hello
// and multiplies pi by pi. That is the whole visible difference after the swap.

#include "vex.h"
#include <stdio.h>

// v2 was built against engine contract 2.
#define APP_REQUIRES_CORE_ABI 2u

#include "app_common.h"

#define PI 3.14159265358979323846

static AppState s_app;
static CorePulseFn s_corePulse;

fn setup(CorePulseFn pulse) {
    s_corePulse = pulse;
    if (s_app.magic != APP_MAGIC)
        AppState_init(&s_app);
    printf("[app v2] hello, this is version 2\n");
}

fn run(d64 nowSeconds) {
    s_app.frames += 1;

    if ((s_app.frames % 20u) == 1u) {
        d64 squared = PI * PI;
        printf("[app v2] frame %4llu: pi * pi = %.6f   (pulse=%.2f)\n",
               (u64) s_app.frames, squared,
               s_corePulse ? (d64) s_corePulse(nowSeconds) : 0.0);
    }
}

fn teardown() {
    s_corePulse = nullptr;
}

// --- the names the loader looks up by dlsym -------------------------------

u64 Hot_requires_core_abi();
bool Hot_save(any buf, bytes cap, bytes *outLen);
bool Hot_restore(const_any buf, bytes len);

u64 Hot_requires_core_abi() {
    return APP_REQUIRES_CORE_ABI;
}

bool Hot_save(any buf, bytes cap, bytes *outLen) {
    return AppState_save(&s_app, buf, cap, outLen);
}

bool Hot_restore(const_any buf, bytes len) {
    return AppState_restore(&s_app, buf, len);
}

static const HotModuleExport s_exports[] = {
    { "setup",                 (any) setup },
    { "run",                   (any) run },
    { "teardown",              (any) teardown },
    { "Hot_requires_core_abi", (any) Hot_requires_core_abi },
    { "Hot_save",              (any) Hot_save },
    { "Hot_restore",           (any) Hot_restore },
};

const_any Hot_getExports(u32 *outCount);
const_any Hot_getExports(u32 *outCount) {
    if (outCount)
        *outCount = (u32) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}
