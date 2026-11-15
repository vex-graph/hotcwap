// app_v1.c — APPLICATION IMAGE, VERSION 1.
//
// Deliberately trivial so the swap is unmistakable: v1 says hello and adds
// 1 + 1. Version 2 is a different file that says a different hello and
// multiplies pi by pi.
//
// The `Hot_` names at the bottom are not decoration: dlsym() needs a literal
// name to look for, so the loader asks for exactly those. Everything the host
// actually calls (setup/run/teardown) is published through Hot_getExports().

#include <stdio.h>

#include "app_common.h"

// This image was built against engine contract 1.
#define APP_REQUIRES_CORE_ABI 1u

static AppState s_app;
static CorePulseFn s_corePulse;   // borrowed engine function, never owned

fn setup(CorePulseFn pulse) {
    s_corePulse = pulse;
    if (s_app.magic != APP_MAGIC)      // a restored image keeps what restore wrote
        AppState_init(&s_app);
    printf("[app v1] hello, this is version 1\n");
}

fn run(d64 nowSeconds) {
    s_app.frames += 1;

    if ((s_app.frames % 20u) == 1u) {
        int sum = 1 + 1;
        printf("[app v1] frame %4llu: 1 + 1 = %d   (pulse=%.2f)\n",
               (unsigned long long) s_app.frames, sum,
               s_corePulse ? (d64) s_corePulse(nowSeconds) : 0.0);
    }
}

fn teardown() {
    s_corePulse = nullptr;   // forget the borrow, keep the state
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

// --- the table the loader adopts ------------------------------------------

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
