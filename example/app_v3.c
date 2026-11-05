// app_v3.c — a deliberately INCOMPATIBLE build.
//
// It declares that it needs core contract 99, which nothing publishes. The
// loader still swaps the image in — admission is fail-closed on the image's
// own integrity, not on policy — but HotStack refuses to let it run, so the
// application layer goes idle.
//
// This is the proof that "the app cannot change unless it matches the core".

#include <stdio.h>

#include "app_common.h"

static AppState s_app;
static CorePulseFn s_corePulse;

void setup(CorePulseFn pulse) {
    s_corePulse = pulse;
    if (s_app.magic != APP_MAGIC)
        AppState_init(&s_app);
    printf("[app v3] setup — THIS SHOULD NEVER RUN\n");
}

void run(double nowSeconds) {
    (void) nowSeconds;
    printf("[app v3] run — THIS SHOULD NEVER RUN\n");
}

void teardown(void) {
    s_corePulse = nullptr;
}

uint64_t Hot_requires_core_abi(void);

uint64_t Hot_requires_core_abi(void) {
    return 99u;
}

bool Hot_save(void *buf, size_t cap, size_t *outLen);
bool Hot_restore(const void *buf, size_t len);

bool Hot_save(void *buf, size_t cap, size_t *outLen) {
    return AppState_save(&s_app, buf, cap, outLen);
}

bool Hot_restore(const void *buf, size_t len) {
    return AppState_restore(&s_app, buf, len);
}

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
