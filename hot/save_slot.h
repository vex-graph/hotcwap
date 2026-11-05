#ifndef HOT_HOT_SAVE_SLOT_H
#define HOT_HOT_SAVE_SLOT_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "hot/hot-limits.h"
#include "manifest.h"

// hot/save_slot.h — one module's serialized state, captured off-thread.
//
// Single Class Per File Law: HotSaveSlot.
//
// The save worker calls the OLD (still mapped) image's Hot_save() into `buf`
// before the new image is staged. The blob never crosses as a pointer: it is
// opaque bytes, and only the module's own Hot_restore() interprets them.
//
// `alignas(16)` on the buffer: the module may cast `buf` to a struct holding a
// 16-byte-aligned member, and the loader must not be the reason that faults.

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    alignas(16) uint8_t buf[HOT_SAVE_SLOT_CAP];
    size_t len;
} HotSaveSlot;

#endif
