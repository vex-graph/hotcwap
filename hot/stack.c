#include "vex.h"
#include "hot/stack.h"

#include <stdio.h>
#include <stdlib.h>



#include "lang/primitive.h"

// hot/stack.c — the loading order, owned by the library.
//
// The application says WHAT it is (core, app, plugins). This file decides the
// order, the contract gate, and what happens when a layer moves. That keeps
// every consumer's setup from inventing the same three rules.

struct HotStack {
    char coreLibrary[HOT_MANIFEST_MAX_NAME];
    char appLibrary[HOT_MANIFEST_MAX_NAME];

    HotModule *core;
    HotModule *app;

    HotModule *plugins[HOT_STACK_MAX_PLUGINS];
    char pluginNames[HOT_STACK_MAX_PLUGINS][HOT_MANIFEST_MAX_NAME];
    u32 pluginCount;

    u64 coreContract;   // 0 = core never bound a contract
    bool appCompatible;
    char lastError[256];
};

;;GETTER
static inline const char *HotStack_coreLibrary(const HotStack *stack) { return (*stack).coreLibrary; }
;;GETTER
static inline char *HotStack_coreLibraryMut(HotStack *stack) { return (*stack).coreLibrary; }
;;GETTER
static inline const char *HotStack_appLibrary(const HotStack *stack) { return (*stack).appLibrary; }
;;GETTER
static inline char *HotStack_appLibraryMut(HotStack *stack) { return (*stack).appLibrary; }
;;GETTER
static inline HotModule *HotStack_core(const HotStack *stack) { return (*stack).core; }
;;SETTER
static inline fn HotStack_setCore(HotStack *stack, HotModule *core) { (*stack).core = core; }
;;GETTER
static inline HotModule *HotStack_app(const HotStack *stack) { return (*stack).app; }
;;SETTER
static inline fn HotStack_setApp(HotStack *stack, HotModule *app) { (*stack).app = app; }
;;GETTER
static inline HotModule *HotStack_pluginAt(const HotStack *stack, u32 i) { return (*stack).plugins[i]; }
;;SETTER
static inline fn HotStack_setPluginAt(HotStack *stack, u32 i, HotModule *p) { (*stack).plugins[i] = p; }
;;GETTER
static inline char *HotStack_pluginNameAtMut(HotStack *stack, u32 i) { return (*stack).pluginNames[i]; }
;;GETTER
static inline u32 HotStack_pluginCount(const HotStack *stack) { return (*stack).pluginCount; }
;;SETTER
static inline fn HotStack_setPluginCount(HotStack *stack, u32 count) { (*stack).pluginCount = count; }
;;GETTER
static inline u64 HotStack_coreContract(const HotStack *stack) { return (*stack).coreContract; }
;;SETTER
static inline fn HotStack_setCoreContract(HotStack *stack, u64 contract) { (*stack).coreContract = contract; }
;;GETTER
static inline bool HotStack_appCompatible(const HotStack *stack) { return (*stack).appCompatible; }
;;SETTER
static inline fn HotStack_setAppCompatible(HotStack *stack, bool ok) { (*stack).appCompatible = ok; }
;;GETTER
static inline const char *HotStack_lastError(const HotStack *stack) { return (*stack).lastError; }
;;GETTER
static inline char *HotStack_lastErrorMut(HotStack *stack) { return (*stack).lastError; }

// The two conventions the gate rests on. A module without them is simply
// ungated rather than rejected: core optional, app optional.
typedef u64 (*AbiFn)();

static u64 readExportAbi(HotModule *module, const char *symbol) {
    const AbiFn fn = (any) Hot_getSymbol(module, symbol);
    return fn ? fn() : 0;
}

static fn stackSetError(HotStack *stack, const char *text) {
    snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError), "%s", text);
}

// --- construction ---------------------------------------------------------

HotStack *HotStack_2(const char *coreLibrary, const char *appLibrary) {
    HotStack *stack = (HotStack*) calloc(1, sizeof(HotStack));
    if (!stack)
        return nullptr;

    snprintf(HotStack_coreLibraryMut(stack), HOT_MANIFEST_MAX_NAME, "%s",
             coreLibrary && *coreLibrary ? coreLibrary : HOT_STACK_CORE_LIBRARY);
    snprintf(HotStack_appLibraryMut(stack), HOT_MANIFEST_MAX_NAME, "%s",
             appLibrary && *appLibrary ? appLibrary : HOT_STACK_APP_LIBRARY);
    HotStack_setAppCompatible(stack, true);
    return stack;
}

HotStack *HotStack_1() {
    return HotStack_2(HOT_STACK_CORE_LIBRARY, HOT_STACK_APP_LIBRARY);
}

fn HotStack_free(HotStack *stack) {
    if (!stack)
        return;
    u32 pcount = HotStack_pluginCount(stack);
    for (u32 i = 0; i < pcount; i++) {
        HotModule *p = HotStack_pluginAt(stack, i);
        if (p)
            Hot_shutdown(p);
    }
    if (HotStack_app(stack))
        Hot_shutdown(HotStack_app(stack));
    if (HotStack_core(stack))
        Hot_shutdown(HotStack_core(stack));
    free(stack);
}

// --- core -----------------------------------------------------------------

HotResult Hot_loadCore(HotStack *stack) {
    if (!stack)
        return HOT_ERROR_INIT_FAILED;

    HotModule *core = Hot_init(HotStack_coreLibrary(stack));
    HotStack_setCore(stack, core);
    if (!core) {
        stackSetError(stack, "Hot_init(core) failed — is the manifest mounted?");
        return HOT_ERROR_INIT_FAILED;
    }

    u32 loaded = 0;
    HotResult result = Hot_poll(core, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError(core);
        snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                 "core first load failed: %s", why ? why : "no sections");
        Hot_shutdown(core);
        HotStack_setCore(stack, nullptr);
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    HotStack_setCoreContract(stack, readExportAbi(core, "Hot_contract_abi"));
    return HOT_OK;
}

// --- application ----------------------------------------------------------

// True when the loaded app image accepts the loaded core contract. A stack
// with no core, or a core publishing no contract, is ungated: there is nothing
// to compare against, and refusing would block apps that never opted in.
static bool appAcceptsContract(HotStack *stack) {
    HotModule *app = HotStack_app(stack);
    u64 contract = HotStack_coreContract(stack);
    if (!app || contract == 0)
        return true;
    u64 wants = readExportAbi(app, "Hot_requires_core_abi");
    if (wants == 0)
        return true;   // app declares no requirement
    return wants == contract;
}

HotResult Hot_loadApplication(HotStack *stack) {
    if (!stack)
        return HOT_ERROR_INIT_FAILED;

    HotModule *app = Hot_init(HotStack_appLibrary(stack));
    HotStack_setApp(stack, app);
    if (!app) {
        stackSetError(stack, "Hot_init(app) failed — is the manifest mounted?");
        return HOT_ERROR_INIT_FAILED;
    }

    u32 loaded = 0;
    HotResult result = Hot_poll(app, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError(app);
        snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                 "app first load failed: %s", why ? why : "no sections");
        Hot_shutdown(app);
        HotStack_setApp(stack, nullptr);
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    if (!appAcceptsContract(stack)) {
        HotStack_setAppCompatible(stack, false);
        snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                 "app wants core contract %llu, core publishes %llu",
                 (u64) readExportAbi(app, "Hot_requires_core_abi"),
                 (u64) HotStack_coreContract(stack));
        // Held, not torn down: a matching app promoted later re-gates cleanly
        // on the next poll without a rebind cost.
        return HOT_ERROR_ABI_MISMATCH;
    }

    HotStack_setAppCompatible(stack, true);
    return HOT_OK;
}

// --- plugins --------------------------------------------------------------

HotResult Hot_loadPlugin(HotStack *stack, const char *library) {
    if (!stack || !library || *library == '\0')
        return HOT_ERROR_INIT_FAILED;
    u32 slot = HotStack_pluginCount(stack);
    if (slot >= HOT_STACK_MAX_PLUGINS) {
        stackSetError(stack, "plugin slots exhausted");
        return HOT_ERROR_OUT_OF_MEMORY;
    }

    HotModule *plugin = Hot_init(library);
    if (!plugin) {
        snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                 "Hot_init(plugin '%s') failed", library);
        return HOT_ERROR_INIT_FAILED;
    }

    u32 loaded = 0;
    HotResult result = Hot_poll(plugin, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError(plugin);
        snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                 "plugin '%s' first load failed: %s", library, why ? why : "no sections");
        Hot_shutdown(plugin);
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    HotStack_setPluginCount(stack, slot + 1);
    HotStack_setPluginAt(stack, slot, plugin);
    snprintf(HotStack_pluginNameAtMut(stack, slot), HOT_MANIFEST_MAX_NAME, "%s", library);
    return HOT_OK;
}

// --- frame ----------------------------------------------------------------

HotResult Hot_pollAll(HotStack *stack, HotStackChanged *changed) {
    if (!stack)
        return HOT_ERROR_INIT_FAILED;

    HotStackChanged moved = { false, false, false };
    HotResult result = HOT_OK;

    // CORE FIRST. Nothing above it can be validated until the contract is
    // current.
    HotModule *core = HotStack_core(stack);
    if (core) {
        u32 loaded = 0;
        HotResult coreResult = Hot_poll(core, &loaded);
        if (coreResult != HOT_OK)
            result = coreResult;
        if (loaded > 0) {
            moved.core = true;
            u64 contract = readExportAbi(core, "Hot_contract_abi");
            if (contract != HotStack_coreContract(stack)) {
                HotStack_setCoreContract(stack, contract);
                // The loaded app image was admitted against the previous
                // contract. Re-gate it: an incompatible one goes idle until
                // the ladder serves a matching image.
                if (!appAcceptsContract(stack)) {
                    HotStack_setAppCompatible(stack, false);
                    snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                             "app wants core contract %llu, core publishes %llu",
                             (u64) readExportAbi(HotStack_app(stack), "Hot_requires_core_abi"),
                             (u64) HotStack_coreContract(stack));
                } else {
                    HotStack_setAppCompatible(stack, true);
                    HotStack_lastErrorMut(stack)[0] = '\0';
                }
            }
        }
    }

    // APP SECOND. The image is always polled — the loader's own admission is
    // fail-closed, so an incompatible image does not need to be refused a
    // load, it must simply not RUN. Gate after the commit, never before: the
    // requirement lives inside the image being promoted.
    HotModule *app = HotStack_app(stack);
    if (app) {
        u32 loaded = 0;
        HotResult appResult = Hot_poll(app, &loaded);
        if (appResult != HOT_OK)
            result = appResult;
        if (loaded > 0) {
            moved.app = true;
            if (!appAcceptsContract(stack)) {
                HotStack_setAppCompatible(stack, false);
                snprintf(HotStack_lastErrorMut(stack), sizeof(((HotStack) {0}).lastError),
                         "app wants core contract %llu, core publishes %llu",
                         (u64) readExportAbi(app, "Hot_requires_core_abi"),
                         (u64) HotStack_coreContract(stack));
            } else {
                HotStack_setAppCompatible(stack, true);
                HotStack_lastErrorMut(stack)[0] = '\0';
            }
        }
    }

    // PLUGINS LAST and independently: a plugin never blocks core or app.
    u32 pcount = HotStack_pluginCount(stack);
    for (u32 i = 0; i < pcount; i++) {
        HotModule *p = HotStack_pluginAt(stack, i);
        if (!p)
            continue;
        u32 loaded = 0;
        if (Hot_poll(p, &loaded) != HOT_OK)
            continue;
        if (loaded > 0)
            moved.plugins = true;
    }

    if (changed)
        (*changed) = moved;
    return result;
}

// --- getters --------------------------------------------------------------

HotModule *Hot_getCore(const HotStack *stack) {
    return stack ? HotStack_core(stack) : nullptr;
}

HotModule *Hot_getApplication(const HotStack *stack) {
    return stack ? HotStack_app(stack) : nullptr;
}

HotModule *Hot_getPlugin(const HotStack *stack, u32 index) {
    if (!stack || index >= HotStack_pluginCount(stack))
        return nullptr;
    return HotStack_pluginAt(stack, index);
}

u32 Hot_getPluginCount(const HotStack *stack) {
    return stack ? HotStack_pluginCount(stack) : 0;
}

u64 Hot_getCoreContract(const HotStack *stack) {
    return stack ? HotStack_coreContract(stack) : 0;
}

bool Hot_isApplicationCompatible(const HotStack *stack) {
    return stack ? HotStack_appCompatible(stack) : false;
}

const char *Hot_getStackError(const HotStack *stack) {
    return stack ? HotStack_lastError(stack) : nullptr;
}
