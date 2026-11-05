#ifndef HOT_HOT_MODULE_H
#define HOT_HOT_MODULE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "hot/hot-limits.h"
#include "hot/module_internal.h"
#include "hot/save_slot.h"
#include "hot/retire_ring.h"
#include "hot/section_digest.h"
#include "hot/trampoline_table.h"
#include "manifest.h"

// hot/module.h — the loader instance for ONE manifest library key.
//
// Single Class Per File Law: HotModule.
//
// `hot.h` declares this type opaque; this header completes it and is internal
// to hotcwap. Anything outside the loader sees only a HotModule* and the
// functions in hot.h.
//
// What one instance owns:
//   - `modules[]`   the loaded section rows (one dylib each)
//   - `generation`  the ladder stamp it last loaded; the swap trigger compares
//                   MANIFEST_GENERATION(library) against this
//   - `trampolines` per-instance symbol table, so two loaders never collide on
//                   the same export name
//   - `retireRing`  old handles kept mapped for HOT_RETIRED_GENERATIONS polls
//   - the save worker and its slots, so state is captured off the frame thread
//
// Threading contract: Hot_poll runs on the frame thread only. The save worker
// is the one supervised thread; `saveLock` guards the flags marked "under
// saveLock". `swapPending` is frame-thread-only by design.

typedef struct HotModule {
    char library[HOT_MANIFEST_MAX_NAME];
    uint64_t generation;

    HotModuleInternal modules[HOT_MAX_MODULES];
    uint32_t module_count;

    char last_error[256];

    HotTrampolineTable trampolines;   // per-instance symbols (no cross-app collision)
    HotRetireRing retireRing;         // per-instance generational close

    // --- off-thread state-save worker ---
    pthread_t saveThread;
    pthread_mutex_t saveLock;
    pthread_cond_t saveCond;
    bool saveWorkerLive;
    bool saveCancel;                  // under saveLock
    bool saveRequested;               // under saveLock
    bool saveDone;                    // under saveLock
    bool swapPending;                 // frame-thread only

    HotSaveSlot saveSlots[HOT_MAX_MODULES];
    size_t saveCount;

    // --- cold (stop-the-world) payload admission ---
    const HotSectionDigest *coldDigests;
    size_t coldCount;
    bool recoveryRequired;
} HotModule;

#endif
