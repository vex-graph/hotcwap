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

void setup(CorePulseFn pulse) {
    s_corePulse = pulse;
    if (s_app.magic != APP_MAGIC)      // a restored image keeps what restore wrote
        AppState_init(&s_app);
    printf("[app v1] hello, this is version 1\n");
}

void run(double nowSeconds) {
    s_app.frames += 1;

    if ((s_app.frames % 20u) == 1u) {
        int sum = 1 + 1;
        printf("[app v1] frame %4llu: 1 + 1 = %d   (pulse=%.2f)\n",
               (unsigned long long) s_app.frames, sum,
               s_corePulse ? (double) s_corePulse(nowSeconds) : 0.0);
    }
}

void teardown(void) {
    s_corePulse = nullptr;   // forget the borrow, keep the state
}

// --- the names the loader looks up by dlsym -------------------------------

uint64_t Hot_requires_core_abi(void);
bool Hot_save(void *buf, size_t cap, size_t *outLen);
bool Hot_restore(const void *buf, size_t len);

uint64_t Hot_requires_core_abi(void) {
    return APP_REQUIRES_CORE_ABI;
}

bool Hot_save(void *buf, size_t cap, size_t *outLen) {
    return AppState_save(&s_app, buf, cap, outLen);
}

bool Hot_restore(const void *buf, size_t len) {
    return AppState_restore(&s_app, buf, len);
}

// --- the table the loader adopts ------------------------------------------

static const HotModuleExport s_exports[] = {
    { "setup",                 (void*) setup },
    { "run",                   (void*) run },
    { "teardown",              (void*) teardown },
    { "Hot_requires_core_abi", (void*) Hot_requires_core_abi },
    { "Hot_save",              (void*) Hot_save },
    { "Hot_restore",           (void*) Hot_restore },
};

const void *Hot_getExports(uint32_t *outCount);

const void *Hot_getExports(uint32_t *outCount) {
    if (outCount)
        *outCount = (uint32_t) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}
