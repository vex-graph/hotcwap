// app_v3.c — a deliberately INCOMPATIBLE build.
//
// It declares that it needs core contract 99, which nothing publishes. The
// loader still swaps the image in — admission is fail-closed on the image's
// own integrity, not on policy — but HotStack refuses to let it run, so the
// application layer goes idle.
//
// This is the proof that "the app cannot change unless it matches the core".

#include "vex.h"
#include <stdio.h>

#include "app_common.h"

static AppState s_app;
static CorePulseFn s_corePulse;

fn setup(CorePulseFn pulse) {
    s_corePulse = pulse;
    if (s_app.magic != APP_MAGIC)
        AppState_init(&s_app);
    printf("[app v3] setup — THIS SHOULD NEVER RUN\n");
}

fn run(d64 nowSeconds) {
    (discard) nowSeconds;
    printf("[app v3] run — THIS SHOULD NEVER RUN\n");
}

fn teardown() {
    s_corePulse = nullptr;
}

u64 Hot_requires_core_abi();

u64 Hot_requires_core_abi() {
    return 99u;
}

bool Hot_save(any buf, bytes cap, bytes *outLen);
bool Hot_restore(const_any buf, bytes len);

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
