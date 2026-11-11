#ifndef APP_COMMON_H
#define APP_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// app_common.h — what every version of the application image shares.
//
// Each app_vN.c is a SEPARATE SOURCE FILE with its own setup() and run(). They
// are not one file built twice: v1 and v2 genuinely differ, which is the point
// of the demo. Only the state record and its save/restore are shared, because
// the schema is the contract across generations — not the behaviour.
//
// The module contract, in the order the loader uses it:
//
//   Hot_restore(blob)          run BEFORE commit, inside the new image
//   setup(corePulse)           once per image, after restore
//   run(nowSeconds)            once per frame
//   teardown()                 before the image is retired
//   Hot_save(blob)             on the old image, off-thread, before staging
//
// The loader finds all of them through Hot_getExports(); it never assumes a
// name. `setup`/`run` are the convention WE chose, not a hotcwap rule.

typedef f32 (*CorePulseFn)(d64);

#define APP_MAGIC  0x41505020u   /* "APP " */
#define APP_SCHEMA 1u

#ifndef APP_REQUIRES_CORE_ABI
#define APP_REQUIRES_CORE_ABI 1u
#endif

typedef struct AppState {
    u32 magic;
    u32 schema;
    u64 frames;
    d64   accumulated;
} AppState;

static inline void AppState_init(AppState *state) {
    (*state).magic = APP_MAGIC;
    (*state).schema = APP_SCHEMA;
    (*state).frames = 0;
    (*state).accumulated = 0.0;
}

static inline bool AppState_save(const AppState *state, void *buf, bytes cap, bytes *outLen) {
    if (outLen)
        *outLen = 0;
    if (!buf || !outLen || cap < sizeof(AppState))
        return false;
    memcpy(buf, state, sizeof(AppState));
    *outLen = sizeof(AppState);
    return true;
}

static inline bool AppState_restore(AppState *state, const void *buf, bytes len) {
    if (!buf || len != sizeof(AppState))
        return false;
    AppState incoming;
    memcpy(&incoming, buf, sizeof(incoming));
    if (incoming.magic != APP_MAGIC || incoming.schema != APP_SCHEMA)
        return false;
    (*state) = incoming;
    return true;
}

// Row layout is a hard contract with hot/staged_section.c.
typedef struct HotModuleExport {
    const char *name;
    void *fn;
} HotModuleExport;

#endif
