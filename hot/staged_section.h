#ifndef HOT_HOT_STAGED_SECTION_H
#define HOT_HOT_STAGED_SECTION_H

#include <stdint.h>

#include "hot/hot-limits.h"
#include "hot/module_internal.h"

// hot/staged_section.h — one section between dlopen and commit.
//
// Single Class Per File Law: HotStagedSection.
//
// Staging is deliberately separate from adoption. Verify-before-commit means
// an image can be mapped, inspected and rejected without ever touching the
// live table — so a bad payload costs a dlclose, not a rollback.
//
//   resolved slot  -> `mod` (created if this is a new section)
//   mapped image   -> `handle` (not yet adopted, not yet retired)
//   export table   -> `trampolines` + count (validated non-empty, no nulls)

typedef struct {
    HotModuleInternal *mod;           // resolved slot (created if new)
    char path[HOT_PATH_LEN];          // dylib path
    void *handle;                     // dlopened, not yet adopted
    const void *trampolines;          // Hot_getExports table
    u32 trampolineCount;         // entries
} HotStagedSection;

#endif
