#include "vex.h"
#include "hot/module.h"

#include <string.h>

#include "hot/internal.h"

// hot/module_internal.c — the section row.
//
// A HotModuleInternal is the loader's memory of "which image is mapped right
// now for this section". Everything here is cold: the frame path only reads
// the fields the commit already published.

;;GETTER
static inline u32 HotModule_moduleCount(const HotModule *hot) { return (*hot).module_count; }
;;SETTER
static inline fn HotModule_setModuleCount(HotModule *hot, u32 count) { (*hot).module_count = count; }
;;GETTER
static inline HotModuleInternal *HotModule_moduleAtMut(HotModule *hot, u32 i) { return &(*hot).modules[i]; }
;;GETTER
static inline const char *HotModuleInternal_name(const HotModuleInternal *mod) { return (*mod).name; }
;;GETTER
static inline char *HotModuleInternal_nameMut(HotModuleInternal *mod) { return (*mod).name; }
;;SETTER
static inline fn HotModuleInternal_setHandle(HotModuleInternal *mod, any handle) { (*mod).handle = handle; }
;;SETTER
static inline fn HotModuleInternal_setLoaded(HotModuleInternal *mod, bool loaded) { (*mod).loaded = loaded; }

// Find a section slot by stem, or nullptr.
HotModuleInternal *HotModuleInternal_find(HotModule *hot, const char *name) {
    u32 count = HotModule_moduleCount(hot);
    for (u32 i = 0; i < count; i++) {
        HotModuleInternal *slot = HotModule_moduleAtMut(hot, i);
        if (strcmp(HotModuleInternal_name(slot), name) == 0)
            return slot;
    }
    return nullptr;
}

// Resolve the slot for `name`, creating one when the section is new.
//
// Returns nullptr when the registry is full. A new row starts unmapped: the
// staged image is adopted into it at commit, never before.
HotModuleInternal *HotModuleInternal_adopt(HotModule *hot, const char *name) {
    HotModuleInternal *mod = HotModuleInternal_find(hot, name);
    if (mod != nullptr)
        return mod;
    u32 count = HotModule_moduleCount(hot);
    if (count >= HOT_MAX_MODULES)
        return nullptr;
    HotModule_setModuleCount(hot, count + 1);
    mod = HotModule_moduleAtMut(hot, count);
    memset(mod, 0, sizeof(*mod));
    char *modName = HotModuleInternal_nameMut(mod);
    strncpy(modName, name, HOT_MANIFEST_MAX_NAME - 1);
    modName[HOT_MANIFEST_MAX_NAME - 1] = '\0';
    HotModuleInternal_setHandle(mod, nullptr);
    HotModuleInternal_setLoaded(mod, false);
    return mod;
}
