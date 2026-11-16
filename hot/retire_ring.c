#include "vex.h"
#include "hot/retire_ring.h"

#include <dlfcn.h>


;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: HotRetireRing
 * ============================================================================
 * Generational retirement ring for dynamically unloaded shared libraries (dylibs).
 * Prevents segmentation faults from in-flight function execution by parking old
 * module handles across a multi-generation grace period before invoking dlclose.
 *
 * memory consists of a fixed-capacity ring of 16 retired handle slots paired with
 * generation stamps. Handles are automatically evicted and closed once their age
 * exceeds HOT_RETIRED_GENERATIONS or when the ring is explicitly drained at teardown.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: HotRetireRing (hot/hot_retire.c)
 * ============================================================================
 * SUMMARY:
 *   One generational dlclose ring per HotModule instance. Old dylibs park
 *   here for HOT_RETIRED_GENERATIONS polls so in-flight calls drain before
 *   close. Full ring evicts the oldest entry; shutdown drains via drainAll.
 *
 * STRUCT FIELDS (Mirroring hot/hot_retire.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   HotRetiredHandle slots[HOT_RETIRED_MAX]; // parked handles (max 16)
 *   u32 generation;                     // current poll generation
 *
 * SLOT RECORD (public, behaviorless, owned by this ring):
 * ----------------------------------------------------------------------------
 *   HotRetiredHandle (one parked dylib):
 *     any handle;                          // retired dylib (NULL = free slot)
 *     u32 generation;                   // poll generation when retired
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - (none)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - HotRetireRing_retire(self, handle)     : Park handle, expire old entries
 *   - HotRetireRing_advance(self)            : Next generation, close expired
 *   - HotRetireRing_drainAll(self)           : Close and clear all (shutdown)
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - (none)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

;;GETTER
static inline HotRetiredHandle *HotRetireRing_slotAtMut(HotRetireRing *self, bytes i) { return &(*self).slots[i]; }
;;GETTER
static inline u32 HotRetireRing_generation(const HotRetireRing *self) { return (*self).generation; }
;;SETTER
static inline fn HotRetireRing_setGeneration(HotRetireRing *self, u32 gen) { (*self).generation = gen; }
;;GETTER
static inline any HotRetiredHandle_handle(const HotRetiredHandle *slot) { return (*slot).handle; }
;;SETTER
static inline fn HotRetiredHandle_setHandle(HotRetiredHandle *slot, any handle) { (*slot).handle = handle; }
;;GETTER
static inline u32 HotRetiredHandle_generation(const HotRetiredHandle *slot) { return (*slot).generation; }
;;SETTER
static inline fn HotRetiredHandle_setGeneration(HotRetiredHandle *slot, u32 gen) { (*slot).generation = gen; }

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Adds a loaded handle to the retire ring so delayed callers can finish. */
fn HotRetireRing_retire(HotRetireRing *self, any handle) {
    if (!self || !handle) return;

    u32 currentGen = HotRetireRing_generation(self);
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = HotRetireRing_slotAtMut(self, i);
        any slotHandle = HotRetiredHandle_handle(slot);
        if (slotHandle && (currentGen - HotRetiredHandle_generation(slot) >= HOT_RETIRED_GENERATIONS)) {
            dlclose(slotHandle);
            HotRetiredHandle_setHandle(slot, nullptr);
        }
    }

    bytes free_idx = HOT_RETIRED_MAX;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = HotRetireRing_slotAtMut(self, i);
        if (!HotRetiredHandle_handle(slot)) {
            free_idx = i;
            break;
        }
    }

    if (free_idx == HOT_RETIRED_MAX) {
        bytes oldest_idx = 0;
        u32 oldest_gen = UINT32_MAX;
        for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
            HotRetiredHandle *slot = HotRetireRing_slotAtMut(self, i);
            u32 slotGen = HotRetiredHandle_generation(slot);
            if (slotGen < oldest_gen) {
                oldest_gen = slotGen;
                oldest_idx = i;
            }
        }
        HotRetiredHandle *victim = HotRetireRing_slotAtMut(self, oldest_idx);
        dlclose(HotRetiredHandle_handle(victim));
        free_idx = oldest_idx;
    }

    HotRetiredHandle *dest = HotRetireRing_slotAtMut(self, free_idx);
    HotRetiredHandle_setHandle(dest, handle);
    HotRetiredHandle_setGeneration(dest, currentGen);
}

/** Advances the grace period and closes handles whose callers have quiesced. */
fn HotRetireRing_advance(HotRetireRing *self) {
    if (!self) return;
    u32 currentGen = HotRetireRing_generation(self) + 1;
    HotRetireRing_setGeneration(self, currentGen);
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = HotRetireRing_slotAtMut(self, i);
        any slotHandle = HotRetiredHandle_handle(slot);
        if (slotHandle && (currentGen - HotRetiredHandle_generation(slot) >= HOT_RETIRED_GENERATIONS)) {
            dlclose(slotHandle);
            HotRetiredHandle_setHandle(slot, nullptr);
        }
    }
}

/** Closes every handle still retained by the ring. */
fn HotRetireRing_drainAll(HotRetireRing *self) {
    if (!self) return;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = HotRetireRing_slotAtMut(self, i);
        any slotHandle = HotRetiredHandle_handle(slot);
        if (slotHandle) {
            dlclose(slotHandle);
            HotRetiredHandle_setHandle(slot, nullptr);
        }
    }
}
