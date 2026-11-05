#ifndef HOT_HOT_LIMITS_H
#define HOT_HOT_LIMITS_H

// hot/limits.h — the loader's named bounds.
//
// These are DEFAULTS, not ceilings (the No Hardcoding Law): the trampoline
// table and the deferred mailbox grow cold. The two fixed arrays below
// (module slots, save slots) are sized by a section count, which the manifest
// catalog already caps at HOT_MANIFEST_MAX_SECTIONS.

#include <stddef.h>

#define HOT_MAX_MODULES     32
#define HOT_PATH_LEN        512
#define HOT_SAVE_SLOT_CAP   4096

// 25ms cond-wait slice for the state-save worker (the Bounded Wait Law).
#define HOT_SAVE_WAIT_NS    (25 * 1000 * 1000)

#endif
