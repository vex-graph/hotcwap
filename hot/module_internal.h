#ifndef HOT_HOT_MODULE_INTERNAL_H
#define HOT_HOT_MODULE_INTERNAL_H

#include <stdbool.h>

#include "hot/hot-limits.h"
#include "manifest.h"

// hot/module_internal.h — one loaded section row.
//
// Single Class Per File Law: HotModuleInternal.
//
// A SECTION is one dylib inside a library: `manifest/current/images/<lib>/app.dylib`
// is the section "app". A library may hold several. This row is the loader's
// memory of "which image is mapped right now for this section".
//
// `handle` is the dlopen handle; it stays mapped through the retire ring grace
// window after a swap, so an in-flight call always lands in live code. That is
// also why an Objective-C-bearing image is never dlclose'd mid-session.

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];  // section stem, e.g. "app"
    char path[HOT_PATH_LEN];           // the unique-path copy that was dlopen'd
    void *handle;                      // dlopen handle
    bool loaded;                       // currently loaded
} HotModuleInternal;

#endif
