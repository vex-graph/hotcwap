#include "hot/retire_ring.h"

#include <dlfcn.h>
#include <stddef.h>

#include "util/annotation.h"

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
 *     void *handle;                          // retired dylib (NULL = free slot)
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

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Adds a loaded handle to the retire ring so delayed callers can finish. */
void HotRetireRing_retire(HotRetireRing *self, void *handle) {
    if (!self || !handle) return;

    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = &(*self).slots[i];
        if ((*slot).handle && ((*self).generation - (*slot).generation >= HOT_RETIRED_GENERATIONS)) {
            dlclose((*slot).handle);
            (*slot).handle = nullptr;
        }
    }

    bytes free_idx = HOT_RETIRED_MAX;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = &(*self).slots[i];
        if (!(*slot).handle) {
            free_idx = i;
            break;
        }
    }

    if (free_idx == HOT_RETIRED_MAX) {
        bytes oldest_idx = 0;
        u32 oldest_gen = UINT32_MAX;
        for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
            HotRetiredHandle *slot = &(*self).slots[i];
            if ((*slot).generation < oldest_gen) {
                oldest_gen = (*slot).generation;
                oldest_idx = i;
            }
        }
        HotRetiredHandle *victim = &(*self).slots[oldest_idx];
        dlclose((*victim).handle);
        free_idx = oldest_idx;
    }

    HotRetiredHandle *dest = &(*self).slots[free_idx];
    (*dest).handle = handle;
    (*dest).generation = (*self).generation;
}

/** Advances the grace period and closes handles whose callers have quiesced. */
void HotRetireRing_advance(HotRetireRing *self) {
    if (!self) return;
    (*self).generation++;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = &(*self).slots[i];
        if ((*slot).handle && ((*self).generation - (*slot).generation >= HOT_RETIRED_GENERATIONS)) {
            dlclose((*slot).handle);
            (*slot).handle = nullptr;
        }
    }
}

/** Closes every handle still retained by the ring. */
void HotRetireRing_drainAll(HotRetireRing *self) {
    if (!self) return;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        HotRetiredHandle *slot = &(*self).slots[i];
        if ((*slot).handle) {
            dlclose((*slot).handle);
            (*slot).handle = nullptr;
        }
    }
}
