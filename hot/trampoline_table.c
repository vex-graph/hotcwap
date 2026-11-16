#include "vex.h"
#include "hot/trampoline_table.h"
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
 *   u32 capacity : allocated row extent, bounded by i32 index representation.
 *   _Atomic u32 count : initialized rows, published after row initialization.
 *   u64 refusals : allocation/extent refusal count; reset by free/constructor.
 * SLOT RECORD: HotTrampoline
 *   _Atomic(any) ptr : current target; borrowed code address.
 *   _Atomic(any) fallback_ptr : previous target; NOT an ownership pin.
 *   char name[HOT_MANIFEST_MAX_NAME] : copied exact nonempty symbol identity.
 * FUNCTION REGISTRY:
 *   HotTrampolineTable_0() : empty constructor; chooser/zero in header.
 *   bool HotTrampolineTable_reserve(self, capacity) : fallible cold growth.
 *   i32 HotTrampolineTable_register(self, name) : exact identity, duplicate reuse.
 *   i32 HotTrampolineTable_find(self, name) : cold lookup, -1 absent/null.
 *   fn HotTrampolineTable_set(self, idx, ptr) : atomic replace and fallback.
 *   any HotTrampolineTable_get(self, idx) : target/fallback, nullptr invalid.
 *   u64 HotTrampolineTable_exhaustionCount(const self) : epoch refusals.
 *   fn HotTrampolineTable_free(self) : release backing/reset; nullptr no-op.
 *   bool HotTrampolineTable_toString(const self, dest, cap, outTruncated)
 *   bool HotTrampolineTable_toStringStruct(const self, dest, cap, outTruncated)
 * PRIVATE: refuse(self, quantity) reports once/counts always; project formats.
 * CONTRACT: owner/external serialization for metadata; exclude readers for growth
 * and destruction. Failure preserves all initialized rows/targets/count. No row
 * setter can forge ownership. Cold projections expose metadata, not code contents.
 * ============================================================================ */

;;GETTER
static inline HotTrampoline *HotTrampolineTable_rows(const HotTrampolineTable *self) { return (*self).rows; }
;;SETTER
static inline fn HotTrampolineTable_setRows(HotTrampolineTable *self, HotTrampoline *rows) { (*self).rows = rows; }
;;GETTER
static inline u32 HotTrampolineTable_capacity(const HotTrampolineTable *self) { return (*self).capacity; }
;;SETTER
static inline fn HotTrampolineTable_setCapacity(HotTrampolineTable *self, u32 capacity) { (*self).capacity = capacity; }
;;GETTER
static inline u32 HotTrampolineTable_countLoad(const HotTrampolineTable *self) { return atomic_load(&(*self).count); }
;;GETTER
static inline fn HotTrampolineTable_storeCount(HotTrampolineTable *self, u32 count) { atomic_store(&(*self).count, count); }
;;GETTER
static inline u64 HotTrampolineTable_refusals(const HotTrampolineTable *self) { return (*self).refusals; }
;;SETTER
static inline fn HotTrampolineTable_setRefusals(HotTrampolineTable *self, u64 refusals) { (*self).refusals = refusals; }
;;GETTER
static inline HotTrampoline *HotTrampolineTable_rowAtMut(const HotTrampolineTable *self, u32 i) { return &(*self).rows[i]; }
;;GETTER
static inline char *HotTrampoline_nameMut(HotTrampoline *row) { return (*row).name; }
;;GETTER
static inline any HotTrampoline_ptrLoad(const HotTrampoline *row) { return atomic_load(&(*row).ptr); }
;;GETTER
static inline fn HotTrampoline_initPtr(HotTrampoline *row, any ptr) { atomic_init(&(*row).ptr, ptr); }
;;GETTER
static inline fn HotTrampoline_storePtr(HotTrampoline *row, any ptr) { atomic_store(&(*row).ptr, ptr); }
;;GETTER
static inline any HotTrampoline_fallbackLoad(const HotTrampoline *row) { return atomic_load(&(*row).fallback_ptr); }
;;GETTER
static inline fn HotTrampoline_initFallback(HotTrampoline *row, any ptr) { atomic_init(&(*row).fallback_ptr, ptr); }
;;GETTER
static inline fn HotTrampoline_storeFallback(HotTrampoline *row, any ptr) { atomic_store(&(*row).fallback_ptr, ptr); }

HotTrampolineTable HotTrampolineTable_0() {
    return (HotTrampolineTable) {0};
}

static bool refuse(HotTrampolineTable *self, u32 quantity) {
    u64 prev = HotTrampolineTable_refusals(self);
    HotTrampolineTable_setRefusals(self, prev + 1);
    if (prev == 0)
        THROW("HotTrampolineTable exhausted: requested %u rows, backing %u, index bound %d", quantity, HotTrampolineTable_capacity(self), INT_MAX);
    return false;
}

bool HotTrampolineTable_reserve(HotTrampolineTable *self, u32 capacity) {
    if (self == nullptr)
        return false;
    u32 curCap = HotTrampolineTable_capacity(self);
    if (capacity <= curCap)
        return true;
    if (capacity > INT_MAX || SIZE_MAX / capacity < sizeof(HotTrampoline))
        return refuse(self, capacity);
    u32 extent = curCap ? curCap : HOT_TRAMPOLINE_CAPACITY_DEFAULT;
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
    u32 count = HotTrampolineTable_countLoad(self);
    for (u32 i = 0; i < count; ++i) {
        HotTrampoline *old = HotTrampolineTable_rowAtMut(self, i);
        HotTrampoline *row = &next[i];
        memcpy(HotTrampoline_nameMut(row), HotTrampoline_nameMut(old), sizeof(((HotTrampoline) {0}).name));
        HotTrampoline_initPtr(row, HotTrampoline_ptrLoad(old));
        HotTrampoline_initFallback(row, HotTrampoline_fallbackLoad(old));
    }
    free(HotTrampolineTable_rows(self));
    HotTrampolineTable_setRows(self, next);
    HotTrampolineTable_setCapacity(self, extent);
    return true;
}

i32 HotTrampolineTable_find(HotTrampolineTable *self, const char *name) {
    if (self == nullptr || name == nullptr)
        return -1;
    u32 count = HotTrampolineTable_countLoad(self);
    for (u32 i = 0; i < count; ++i) {
        HotTrampoline *row = HotTrampolineTable_rowAtMut(self, i);
        if (strcmp(HotTrampoline_nameMut(row), name) == 0)
            return (i32) i;
    }
    return -1;
}

i32 HotTrampolineTable_register(HotTrampolineTable *self, const char *name) {
    if (self == nullptr || name == nullptr || *name == '\0' ||
        strnlen(name, HOT_MANIFEST_MAX_NAME) == HOT_MANIFEST_MAX_NAME) {
        THROW("trampoline register: invalid symbol identity");
        return -1;
    }
    i32 found = HotTrampolineTable_find(self, name);
    if (found >= 0)
        return found;
    u32 count = HotTrampolineTable_countLoad(self);
    if (!HotTrampolineTable_reserve(self, count + 1))
        return -1;
    HotTrampoline *row = HotTrampolineTable_rowAtMut(self, count);
    strcpy(HotTrampoline_nameMut(row), name);
    HotTrampoline_initPtr(row, nullptr);
    HotTrampoline_initFallback(row, nullptr);
    HotTrampolineTable_storeCount(self, count + 1);
    return (i32) count;
}

fn HotTrampolineTable_set(HotTrampolineTable *self, i32 idx, any ptr) {
    if (self == nullptr || idx < 0 || (u32) idx >= HotTrampolineTable_countLoad(self))
        return;
    HotTrampoline *row = HotTrampolineTable_rowAtMut(self, (u32) idx);
    any old = HotTrampoline_ptrLoad(row);
    if (old != nullptr && old != ptr)
        HotTrampoline_storeFallback(row, old);
    HotTrampoline_storePtr(row, ptr);
}

any HotTrampolineTable_get(HotTrampolineTable *self, i32 idx) {
    if (self == nullptr || idx < 0 || (u32) idx >= HotTrampolineTable_countLoad(self))
        return nullptr;
    HotTrampoline *row = HotTrampolineTable_rowAtMut(self, (u32) idx);
    any ptr = HotTrampoline_ptrLoad(row);
    return ptr != nullptr ? ptr : HotTrampoline_fallbackLoad(row);
}

fn HotTrampolineTable_free(HotTrampolineTable *self) {
    if (self == nullptr)
        return;
    free(HotTrampolineTable_rows(self));
    *self = HotTrampolineTable_0();
}

u64 HotTrampolineTable_exhaustionCount(const HotTrampolineTable *self) {
    return self == nullptr ? 0 : HotTrampolineTable_refusals(self);
}

static bool project(const HotTrampolineTable *self, bool structure, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    i32 n = -1;
    if (dest != nullptr && cap > 0) {
        if (self == nullptr)
            n = snprintf(dest, cap, "nullptr");
        else if (structure)
            n = snprintf(dest, cap, "HotTrampolineTable{rows=%p,capacity=%u,count=%u,refusals=%llu}",
                         (any) HotTrampolineTable_rows(self), HotTrampolineTable_capacity(self),
                         HotTrampolineTable_countLoad(self), (u64) HotTrampolineTable_refusals(self));
        else
            n = snprintf(dest, cap, "symbols=%u capacity=%u",
                         HotTrampolineTable_countLoad(self), HotTrampolineTable_capacity(self));
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
