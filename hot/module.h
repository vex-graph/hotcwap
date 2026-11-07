#ifndef HOT_HOT_MODULE_H
#define HOT_HOT_MODULE_H

#include <pthread.h>
#include <stdatomic.h>
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
// is the one supervised thread. `saveLock` guards the blobs and every phase
// WRITE; the phase itself is atomic so a diagnostic read needs no lock.

// The off-thread snapshot handshake, as one word:
//
//   IDLE ──begin──▶ REQUESTED ──worker publishes──▶ CAPTURED
//     ▲                                                │
//     └──────────── poll consumes (swap starts) ────────┘
//
//   any phase ──shutdown──▶ STOPPED  (terminal for the worker)
//
// Only `save_slot.c` writes a transition. The frame thread drives IDLE ->
// REQUESTED and CAPTURED -> IDLE; the worker drives REQUESTED -> CAPTURED;
// shutdown drives anything -> STOPPED. CAPTURED is exactly the old
// `swapPending` state, and STOPPED replaces the `saveCancel` bool.
typedef enum HotSavePhase {
    HOT_SAVE_IDLE = 0,   // nothing in flight
    HOT_SAVE_REQUESTED,  // frame asked; worker has not published yet
    HOT_SAVE_CAPTURED,   // blob published; the next poll swaps and clears
    HOT_SAVE_STOPPED,    // worker told to exit; its join is pending or done
    HOT_SAVE_PHASE_COUNT
} HotSavePhase;

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
    pthread_mutex_t saveLock;         // guards saveSlots/saveCount and phase writes
    pthread_cond_t saveCond;
    atomic_uint savePhase;            // HotSavePhase: the one handshake word

    HotSaveSlot saveSlots[HOT_MAX_MODULES];
    size_t saveCount;

    // --- cold (stop-the-world) payload admission ---
    const HotSectionDigest *coldDigests;
    size_t coldCount;
    bool recoveryRequired;
} HotModule;

#endif
