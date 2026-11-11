#ifndef HOT_HOT_TRAMPOLINE_H
#define HOT_HOT_TRAMPOLINE_H

#include <stdatomic.h>
#include <stdint.h>

#include "manifest.h"

// hot/hot_trampoline.h — Per-instance atomic function-pointer table.
//
// Each HotModule owns one HotTrampolineTable, so two loader instances share
// the framework code but never collide: same symbol name in two apps resolves
// through two different tables. On reload the new dylib's address is stored
// into ptr while fallback_ptr records a prior address; it does NOT keep code
// alive. Caller exclusion/lifetime retention is mandatory. Cold swaps clear
// fallbacks before resuming after retiring the old images.

typedef struct HotTrampoline {
    _Atomic(void*) ptr;             // current generation target
    _Atomic(void*) fallback_ptr;    // prior generation (mid-swap cover)
    char name[HOT_MANIFEST_MAX_NAME]; // export symbol name
} HotTrampoline;

// Initial allocation only, not a rejection ceiling. Growth/registration/free
// require caller exclusion of all table readers; atomic target loads/stores do
// not make backing relocation concurrent-safe. Indices, not row pointers, escape.
#define HOT_TRAMPOLINE_CAPACITY_DEFAULT 16u

typedef struct HotTrampolineTable {
    HotTrampoline *rows;                     // cold-growable symbol backing
    u32 capacity;                       // allocated rows
    _Atomic u32 count;                  // used rows (cross-thread readers)
    u64 refusals;                       // allocation/extent refusals this epoch
} HotTrampolineTable;

HotTrampolineTable HotTrampolineTable_0(void);
#define HotTrampolineTable() HotTrampolineTable_0()
#define HotTrampolineTable_zero() HotTrampolineTable_0()
bool HotTrampolineTable_reserve(HotTrampolineTable *self, u32 capacity);
void HotTrampolineTable_free(HotTrampolineTable *self);
u64 HotTrampolineTable_exhaustionCount(const HotTrampolineTable *self);
bool HotTrampolineTable_toString(const HotTrampolineTable *self, char *dest, bytes cap, bool *outTruncated);
bool HotTrampolineTable_toStringStruct(const HotTrampolineTable *self, char *dest, bytes cap, bool *outTruncated);

// Register a row for a function. Returns the row index.
int HotTrampolineTable_register(HotTrampolineTable *self, const char *name);

// Current function pointer, falling back to the prior generation on mid-swap NULL.
void *HotTrampolineTable_get(HotTrampolineTable *self, int idx);

// Atomically swap a row's target, stashing the old pointer as fallback.
void HotTrampolineTable_set(HotTrampolineTable *self, int idx, void *ptr);

// Find a row by name. Returns -1 if not found.
int HotTrampolineTable_find(HotTrampolineTable *self, const char *name);

#endif
