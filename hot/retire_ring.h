#ifndef HOT_HOT_RETIRE_H
#define HOT_HOT_RETIRE_H

#include <stdint.h>

// hot/hot_retire.h — Per-instance generational dlclose ring.
//
// Each HotModule owns one HotRetireRing, so two loader instances retire on
// their own generations: old dylibs stay alive HOT_RETIRED_GENERATIONS polls
// so in-flight calls into the old generation finish before dlclose().

typedef struct HotRetiredHandle {
    void *handle;          // retired dylib awaiting close (NULL = free slot)
    uint32_t generation;   // poll generation when retired
} HotRetiredHandle;

#define HOT_RETIRED_MAX 16
#define HOT_RETIRED_GENERATIONS 4

typedef struct HotRetireRing {
    HotRetiredHandle slots[HOT_RETIRED_MAX]; // parked handles
    uint32_t generation;                     // current poll generation
} HotRetireRing;

// Park a handle in the ring, closing expired entries first.
void HotRetireRing_retire(HotRetireRing *self, void *handle);

// Advance one poll generation, closing entries older than the grace period.
void HotRetireRing_advance(HotRetireRing *self);

// Close and clear every parked handle (shutdown path).
void HotRetireRing_drainAll(HotRetireRing *self);

#endif
