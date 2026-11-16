// hotcwap/test/hot/save_phase_test.c — the HotSavePhase handshake _test.
//
// Owns the off-thread snapshot contract as ONE word: IDLE -> REQUESTED ->
// CAPTURED -> IDLE, with anything -> STOPPED at shutdown. The test drives the
// real worker (HotSaveSlot_workerMain) and the real transitions in
// hot/save_slot.c; it builds no manifest, loads no fixture dylib and touches no
// filesystem. The "module" row points at this executable's own image, which
// exports Hot_save, so dlsym resolves the same way it does in production.
//
// Build (only two translation units are needed):
//   clang -std=gnu23 -Wall -Wextra -Werror -Ivex -Ivex/hotcwap -Ivex/util \
//         -Ivex/vexspoke -Ivex/re test/hot/save_phase_test.c hot/save_slot.c \
//         -pthread -o save_phase_test

#include "vex.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hot/module.h"
#include "hot/internal.h"

static i32 g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

// The state blob a module hands to the loader. Exported so the loader's
// dlsym(handle, "Hot_save") finds it on this executable's image.
__attribute__((visibility("default")))
bool Hot_save(any buf, bytes cap, bytes *out) {
    static const char pattern[] = "save-phase";
    if (buf == nullptr || out == nullptr || cap < sizeof(pattern))
        return false;
    memcpy(buf, pattern, sizeof(pattern));
    *out = sizeof(pattern);
    return true;
}

;;GETTER
static inline time_t Timespec_tvSec(const struct timespec *ts) { return (*ts).tv_sec; }
;;GETTER
static inline long Timespec_tvNsec(const struct timespec *ts) { return (*ts).tv_nsec; }

static i64 elapsedMs(const struct timespec *from, const struct timespec *to) {
    return (Timespec_tvSec(to) - Timespec_tvSec(from)) * 1000L
         + (Timespec_tvNsec(to) - Timespec_tvNsec(from)) / 1000000L;
}

/** Bounded wait for a phase (the Bounded Wait Law); false on timeout. */
static bool waitPhase(HotModule *hot, HotSavePhase want, i64 timeoutMs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (HotSaveSlot_phase(hot) == want)
            return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsedMs(&start, &now) > timeoutMs)
            return false;
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, nullptr);
    }
}

;;GETTER
static inline pthread_mutex_t *HotModule_saveLockMut(HotModule *hot) { return &(*hot).saveLock; }
;;GETTER
static inline pthread_cond_t *HotModule_saveCondMut(HotModule *hot) { return &(*hot).saveCond; }
;;GETTER
static inline HotModuleInternal *HotModule_moduleAtMut(HotModule *hot, u32 i) { return &(*hot).modules[i]; }
;;SETTER
static inline fn HotModule_setModuleCount(HotModule *hot, u32 count) { (*hot).module_count = count; }
;;GETTER
static inline char *HotModuleInternal_nameMut(HotModuleInternal *mod) { return (*mod).name; }
;;SETTER
static inline fn HotModuleInternal_setHandle(HotModuleInternal *mod, any handle) { (*mod).handle = handle; }
;;SETTER
static inline fn HotModuleInternal_setLoaded(HotModuleInternal *mod, bool loaded) { (*mod).loaded = loaded; }

/** A zeroed HotModule resolved against this binary's exported Hot_save. */
static bool modulePrepare(HotModule *hot, any image) {
    memset(hot, 0, sizeof(*hot));
    if (pthread_mutex_init(HotModule_saveLockMut(hot), nullptr) != 0)
        return false;
    if (pthread_cond_init(HotModule_saveCondMut(hot), nullptr) != 0) {
        pthread_mutex_destroy(HotModule_saveLockMut(hot));
        return false;
    }
    if (dlsym(image, "Hot_save") == nullptr) {
        pthread_cond_destroy(HotModule_saveCondMut(hot));
        pthread_mutex_destroy(HotModule_saveLockMut(hot));
        return false;
    }
    HotModuleInternal *slot = HotModule_moduleAtMut(hot, 0);
    strncpy(HotModuleInternal_nameMut(slot), "probe", HOT_MANIFEST_MAX_NAME - 1);
    HotModuleInternal_setHandle(slot, image);
    HotModuleInternal_setLoaded(slot, true);
    HotModule_setModuleCount(hot, 1);
    return true;
}

static fn moduleRelease(HotModule *hot) {
    pthread_cond_destroy(HotModule_saveCondMut(hot));
    pthread_mutex_destroy(HotModule_saveLockMut(hot));
}

i32 main() {
    any image = dlopen(nullptr, RTLD_NOW);
    CHECK(image != nullptr);
    if (image == nullptr) {
        printf("save_phase_test: cannot open the main image\n");
        return 1;
    }

    HotModule hot;
    CHECK(modulePrepare(&hot, image));

    // A fresh loader is IDLE, and consuming an unpublished snapshot is refused.
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_IDLE);
    CHECK(!HotSaveSlot_consume(&hot));
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_IDLE);

    CHECK(pthread_create(&hot.saveThread, nullptr, HotSaveSlot_workerMain, &hot) == 0);

    // IDLE -> REQUESTED -> CAPTURED, published by the worker.
    CHECK(HotSaveSlot_begin(&hot));
    CHECK(waitPhase(&hot, HOT_SAVE_CAPTURED, 2000));
    CHECK(hot.saveCount == 1);
    CHECK(strcmp(hot.saveSlots[0].name, "probe") == 0);
    CHECK(hot.saveSlots[0].len == sizeof("save-phase"));
    CHECK(memcmp(hot.saveSlots[0].buf, "save-phase", sizeof("save-phase")) == 0);

    // CAPTURED -> IDLE exactly once; the published blob stays for the swap.
    CHECK(HotSaveSlot_consume(&hot));
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_IDLE);
    CHECK(!HotSaveSlot_consume(&hot));
    CHECK(hot.saveCount == 1);

    // A second handshake reuses the word: no stale CAPTURED, no stale blob count.
    CHECK(HotSaveSlot_begin(&hot));
    CHECK(waitPhase(&hot, HOT_SAVE_CAPTURED, 2000));
    CHECK(HotSaveSlot_consume(&hot));
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_IDLE);

    // Shutdown is the terminal transition; the worker joins and the word stays.
    HotSaveSlot_workerShutdown(&hot);
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_STOPPED);
    CHECK(!HotSaveSlot_begin(&hot));                 // never queue behind a dead worker
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_STOPPED);
    HotSaveSlot_workerShutdown(&hot);                // idempotent: no second join
    CHECK(HotSaveSlot_phase(&hot) == HOT_SAVE_STOPPED);
    moduleRelease(&hot);

    // Shutdown inside the REQUESTED window must still join bounded and land in
    // STOPPED; whether the snapshot won the race is not the contract.
    HotModule racing;
    CHECK(modulePrepare(&racing, image));
    CHECK(pthread_create(&racing.saveThread, nullptr, HotSaveSlot_workerMain, &racing) == 0);
    CHECK(HotSaveSlot_begin(&racing));
    HotSaveSlot_workerShutdown(&racing);
    CHECK(HotSaveSlot_phase(&racing) == HOT_SAVE_STOPPED);
    moduleRelease(&racing);

    // No save-capable row: begin refuses without moving the word.
    HotModule empty;
    CHECK(modulePrepare(&empty, image));
    empty.module_count = 0;
    CHECK(!HotSaveSlot_begin(&empty));
    CHECK(HotSaveSlot_phase(&empty) == HOT_SAVE_IDLE);
    moduleRelease(&empty);

    dlclose(image);
    if (g_failures == 0)
        printf("save_phase_test: all assertions held\n");
    return g_failures == 0 ? 0 : 1;
}
