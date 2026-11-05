#ifndef HOT_HOT_INTERNAL_H
#define HOT_HOT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hot.h"
#include "hot/module.h"
#include "hot/staged_section.h"

// hot/internal.h — the loader's cross-file contracts.
//
// Each `Hot<Class>_<verb>` below is the private behavior of one class that
// lives in its own translation unit under hot/. They are not part of hot.h:
// nothing outside hotcwap links against them, and the Hot prefix keeps them
// from colliding with a module's exports.

// --- hot/module_internal.c — the section row -------------------------------
HotModuleInternal *HotModuleInternal_find(HotModule *hot, const char *name);
HotModuleInternal *HotModuleInternal_adopt(HotModule *hot, const char *name);

// --- hot/save_slot.c — off-thread state capture ----------------------------
void *HotSaveSlot_workerMain(void *arg);
void HotSaveSlot_workerShutdown(HotModule *hot);
bool HotSaveSlot_begin(HotModule *hot);

// --- hot/staged_section.c — map, verify, describe --------------------------
bool HotStagedSection_isImage(const char *name);
HotResult HotStagedSection_admit(HotModule *hot, const char *libDir,
                                 const char *name, HotStagedSection *out);

// Staging transport helpers (unique-path copy + digest).
bool HotStaging_mkdirs(const char *path);
bool HotStaging_copy(const char *src, const char *dst);
bool HotStaging_digestMatches(const char *path, const uint8_t expected[32]);
uint32_t HotStaging_nextSerial(void);
const HotSectionDigest *HotStaging_coldDigest(HotModule *hot, const char *name);

#endif
