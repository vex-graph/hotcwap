#include "hot/trampoline_table.h"
#include "util/annotation.h"
#include "util/throw.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

;;DEFINITION
;;INTENTION("per the Conflict Triage Law + Single Class Per File Law (Java Law): owned backing/count/geometry mutate only via reserve/register/free under reader exclusion; raw setters would forge ownership")
/* Named dispatch entries survive any number of target replacements. Cold reserve
 * f64s backing without a guessed symbol ceiling; callers exclude all readers
 * during registration, relocation and free. Atomic targets support publication,
 * not module lifetime pinning. A name is identity: reject truncation, reuse an
 * existing row, and publish count only after initialization. */
;;OVERVIEW
/* ============================================================================
 * CLASS: HotTrampolineTable (hot/hot_trampoline.c)
 * STRUCT FIELDS (Mirroring hot/hot_trampoline.h), in declaration order:
 *   HotTrampoline *rows : owned growable backing; no escaped row pointers.
 *   u32 capacity : allocated row extent, bounded by int index representation.
 *   _Atomic u32 count : initialized rows, published after row initialization.
 *   u64 refusals : allocation/extent refusal count; reset by free/constructor.
 * SLOT RECORD: HotTrampoline
 *   _Atomic(void*) ptr : current target; borrowed code address.
 *   _Atomic(void*) fallback_ptr : previous target; NOT an ownership pin.
 *   char name[HOT_MANIFEST_MAX_NAME] : copied exact nonempty symbol identity.
 * FUNCTION REGISTRY:
 *   HotTrampolineTable_0(void) : empty constructor; chooser/zero in header.
 *   bool HotTrampolineTable_reserve(self, capacity) : fallible cold growth.
 *   int HotTrampolineTable_register(self, name) : exact identity, duplicate reuse.
 *   int HotTrampolineTable_find(self, name) : cold lookup, -1 absent/null.
 *   void HotTrampolineTable_set(self, idx, ptr) : atomic replace and fallback.
 *   void *HotTrampolineTable_get(self, idx) : target/fallback, nullptr invalid.
 *   u64 HotTrampolineTable_exhaustionCount(const self) : epoch refusals.
 *   void HotTrampolineTable_free(self) : release backing/reset; nullptr no-op.
 *   bool HotTrampolineTable_toString(const self, dest, cap, outTruncated)
 *   bool HotTrampolineTable_toStringStruct(const self, dest, cap, outTruncated)
 * PRIVATE: refuse(self, quantity) reports once/counts always; project formats.
 * CONTRACT: owner/external serialization for metadata; exclude readers for growth
 * and destruction. Failure preserves all initialized rows/targets/count. No row
 * setter can forge ownership. Cold projections expose metadata, not code contents.
 * ============================================================================ */

HotTrampolineTable HotTrampolineTable_0(void) {
    return (HotTrampolineTable) {0};
}

static bool refuse(HotTrampolineTable *self, u32 quantity) {
    if ((*self).refusals++ == 0)
        THROW("HotTrampolineTable exhausted: requested %u rows, backing %u, index bound %d", quantity, (*self).capacity, INT_MAX);
    return false;
}

bool HotTrampolineTable_reserve(HotTrampolineTable *self, u32 capacity) {
    if (self == nullptr)
        return false;
    if (capacity <= (*self).capacity)
        return true;
    if (capacity > INT_MAX || SIZE_MAX / capacity < sizeof(HotTrampoline))
        return refuse(self, capacity);
    u32 extent = (*self).capacity ? (*self).capacity : HOT_TRAMPOLINE_CAPACITY_DEFAULT;
    while (extent < capacity) {
        if (extent > (u32) INT_MAX / 2) {
            extent = capacity;
            break;
        }
        extent *= 2;
    }
    HotTrampoline *next = calloc(extent, sizeof(*next));
    if (next == nullptr)
        return refuse(self, capacity);
    u32 count = atomic_load(&(*self).count);
    for (u32 i = 0; i < count; ++i) {
        HotTrampoline *old = &(*self).rows[i];
        HotTrampoline *row = &next[i];
        memcpy((*row).name, (*old).name, sizeof((*row).name));
        atomic_init(&(*row).ptr, atomic_load(&(*old).ptr));
        atomic_init(&(*row).fallback_ptr, atomic_load(&(*old).fallback_ptr));
    }
    free((*self).rows);
    (*self).rows = next;
    (*self).capacity = extent;
    return true;
}

int HotTrampolineTable_find(HotTrampolineTable *self, const char *name) {
    if (self == nullptr || name == nullptr)
        return -1;
    u32 count = atomic_load(&(*self).count);
    for (u32 i = 0; i < count; ++i) {
        HotTrampoline *row = &(*self).rows[i];
        if (strcmp((*row).name, name) == 0)
            return (int) i;
    }
    return -1;
}

int HotTrampolineTable_register(HotTrampolineTable *self, const char *name) {
    if (self == nullptr || name == nullptr || *name == '\0' ||
        strnlen(name, HOT_MANIFEST_MAX_NAME) == HOT_MANIFEST_MAX_NAME) {
        THROW("trampoline register: invalid symbol identity");
        return -1;
    }
    int found = HotTrampolineTable_find(self, name);
    if (found >= 0)
        return found;
    u32 count = atomic_load(&(*self).count);
    if (!HotTrampolineTable_reserve(self, count + 1))
        return -1;
    HotTrampoline *row = &(*self).rows[count];
    strcpy((*row).name, name);
    atomic_init(&(*row).ptr, nullptr);
    atomic_init(&(*row).fallback_ptr, nullptr);
    atomic_store(&(*self).count, count + 1);
    return (int) count;
}

void HotTrampolineTable_set(HotTrampolineTable *self, int idx, void *ptr) {
    if (self == nullptr || idx < 0 || (u32) idx >= atomic_load(&(*self).count))
        return;
    HotTrampoline *row = &(*self).rows[idx];
    void *old = atomic_load(&(*row).ptr);
    if (old != nullptr && old != ptr)
        atomic_store(&(*row).fallback_ptr, old);
    atomic_store(&(*row).ptr, ptr);
}

void *HotTrampolineTable_get(HotTrampolineTable *self, int idx) {
    if (self == nullptr || idx < 0 || (u32) idx >= atomic_load(&(*self).count))
        return nullptr;
    HotTrampoline *row = &(*self).rows[idx];
    void *ptr = atomic_load(&(*row).ptr);
    return ptr != nullptr ? ptr : atomic_load(&(*row).fallback_ptr);
}

void HotTrampolineTable_free(HotTrampolineTable *self) {
    if (self == nullptr)
        return;
    free((*self).rows);
    *self = HotTrampolineTable_0();
}

u64 HotTrampolineTable_exhaustionCount(const HotTrampolineTable *self) {
    return self == nullptr ? 0 : (*self).refusals;
}

static bool project(const HotTrampolineTable *self, bool structure, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    int n = -1;
    if (dest != nullptr && cap > 0) {
        if (self == nullptr)
            n = snprintf(dest, cap, "nullptr");
        else if (structure)
            n = snprintf(dest, cap, "HotTrampolineTable{rows=%p,capacity=%u,count=%u,refusals=%llu}",
                         (void*) (*self).rows, (*self).capacity, atomic_load(&(*self).count), (unsigned long long) (*self).refusals);
        else
            n = snprintf(dest, cap, "symbols=%u capacity=%u", atomic_load(&(*self).count), (*self).capacity);
    }
    if (n < 0 || (bytes) n >= cap) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        THROW("trampoline projection: missing/truncated destination");
        return false;
    }
    return true;
}

bool HotTrampolineTable_toString(const HotTrampolineTable *self, char *dest, bytes cap, bool *outTruncated) {
    return project(self, false, dest, cap, outTruncated);
}

bool HotTrampolineTable_toStringStruct(const HotTrampolineTable *self, char *dest, bytes cap, bool *outTruncated) {
    return project(self, true, dest, cap, outTruncated);
}
