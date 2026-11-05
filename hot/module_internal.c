#include "hot/module.h"

#include <string.h>

#include "hot/internal.h"

// hot/module_internal.c — the section row.
//
// A HotModuleInternal is the loader's memory of "which image is mapped right
// now for this section". Everything here is cold: the frame path only reads
// the fields the commit already published.

// Find a section slot by stem, or nullptr.
HotModuleInternal *HotModuleInternal_find(HotModule *hot, const char *name) {
    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        if (strcmp((*hot).modules[i].name, name) == 0)
            return &(*hot).modules[i];
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
    if ((*hot).module_count >= HOT_MAX_MODULES)
        return nullptr;
    mod = &(*hot).modules[(*hot).module_count++];
    memset(mod, 0, sizeof(*mod));
    strncpy((*mod).name, name, HOT_MANIFEST_MAX_NAME - 1);
    (*mod).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
    (*mod).handle = nullptr;
    (*mod).loaded = false;
    return mod;
}
