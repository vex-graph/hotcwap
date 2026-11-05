#include "hot/stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    uint32_t pluginCount;

    uint64_t coreContract;   // 0 = core never bound a contract
    bool appCompatible;
    char lastError[256];
};

// The two conventions the gate rests on. A module without them is simply
// ungated rather than rejected: core optional, app optional.
typedef uint64_t (*AbiFn)(void);

static uint64_t readExportAbi(HotModule *module, const char *symbol) {
    AbiFn fn = (AbiFn) (void*) Hot_getSymbol(module, symbol);
    return fn ? fn() : 0;
}

static void stackSetError(HotStack *stack, const char *text) {
    snprintf((*stack).lastError, sizeof((*stack).lastError), "%s", text);
}

// --- construction ---------------------------------------------------------

HotStack *HotStack_2(const char *coreLibrary, const char *appLibrary) {
    HotStack *stack = (HotStack*) calloc(1, sizeof(HotStack));
    if (!stack)
        return nullptr;

    snprintf((*stack).coreLibrary, HOT_MANIFEST_MAX_NAME, "%s",
             coreLibrary && *coreLibrary ? coreLibrary : HOT_STACK_CORE_LIBRARY);
    snprintf((*stack).appLibrary, HOT_MANIFEST_MAX_NAME, "%s",
             appLibrary && *appLibrary ? appLibrary : HOT_STACK_APP_LIBRARY);
    (*stack).appCompatible = true;
    return stack;
}

HotStack *HotStack_0(void) {
    return HotStack_2(HOT_STACK_CORE_LIBRARY, HOT_STACK_APP_LIBRARY);
}

void HotStack_free(HotStack *stack) {
    if (!stack)
        return;
    for (uint32_t i = 0; i < (*stack).pluginCount; i++)
        if ((*stack).plugins[i])
            Hot_shutdown((*stack).plugins[i]);
    if ((*stack).app)
        Hot_shutdown((*stack).app);
    if ((*stack).core)
        Hot_shutdown((*stack).core);
    free(stack);
}

// --- core -----------------------------------------------------------------

HotResult Hot_loadCore(HotStack *stack) {
    if (!stack)
        return HOT_ERROR_INIT_FAILED;

    (*stack).core = Hot_init((*stack).coreLibrary);
    if (!(*stack).core) {
        stackSetError(stack, "Hot_init(core) failed — is the manifest mounted?");
        return HOT_ERROR_INIT_FAILED;
    }

    uint32_t loaded = 0;
    HotResult result = Hot_poll((*stack).core, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError((*stack).core);
        snprintf((*stack).lastError, sizeof((*stack).lastError),
                 "core first load failed: %s", why ? why : "no sections");
        Hot_shutdown((*stack).core);
        (*stack).core = nullptr;
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    (*stack).coreContract = readExportAbi((*stack).core, "Hot_contract_abi");
    return HOT_OK;
}

// --- application ----------------------------------------------------------

// True when the loaded app image accepts the loaded core contract. A stack
// with no core, or a core publishing no contract, is ungated: there is nothing
// to compare against, and refusing would block apps that never opted in.
static bool appAcceptsContract(HotStack *stack) {
    if (!(*stack).app || (*stack).coreContract == 0)
        return true;
    uint64_t wants = readExportAbi((*stack).app, "Hot_requires_core_abi");
    if (wants == 0)
        return true;   // app declares no requirement
    return wants == (*stack).coreContract;
}

HotResult Hot_loadApplication(HotStack *stack) {
    if (!stack)
        return HOT_ERROR_INIT_FAILED;

    (*stack).app = Hot_init((*stack).appLibrary);
    if (!(*stack).app) {
        stackSetError(stack, "Hot_init(app) failed — is the manifest mounted?");
        return HOT_ERROR_INIT_FAILED;
    }

    uint32_t loaded = 0;
    HotResult result = Hot_poll((*stack).app, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError((*stack).app);
        snprintf((*stack).lastError, sizeof((*stack).lastError),
                 "app first load failed: %s", why ? why : "no sections");
        Hot_shutdown((*stack).app);
        (*stack).app = nullptr;
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    if (!appAcceptsContract(stack)) {
        (*stack).appCompatible = false;
        snprintf((*stack).lastError, sizeof((*stack).lastError),
                 "app wants core contract %llu, core publishes %llu",
                 (unsigned long long) readExportAbi((*stack).app, "Hot_requires_core_abi"),
                 (unsigned long long) (*stack).coreContract);
        // Held, not torn down: a matching app promoted later re-gates cleanly
        // on the next poll without a rebind cost.
        return HOT_ERROR_ABI_MISMATCH;
    }

    (*stack).appCompatible = true;
    return HOT_OK;
}

// --- plugins --------------------------------------------------------------

HotResult Hot_loadPlugin(HotStack *stack, const char *library) {
    if (!stack || !library || *library == '\0')
        return HOT_ERROR_INIT_FAILED;
    if ((*stack).pluginCount >= HOT_STACK_MAX_PLUGINS) {
        stackSetError(stack, "plugin slots exhausted");
        return HOT_ERROR_OUT_OF_MEMORY;
    }

    HotModule *plugin = Hot_init(library);
    if (!plugin) {
        snprintf((*stack).lastError, sizeof((*stack).lastError),
                 "Hot_init(plugin '%s') failed", library);
        return HOT_ERROR_INIT_FAILED;
    }

    uint32_t loaded = 0;
    HotResult result = Hot_poll(plugin, &loaded);
    if (result != HOT_OK || loaded == 0) {
        const char *why = Hot_getLastError(plugin);
        snprintf((*stack).lastError, sizeof((*stack).lastError),
                 "plugin '%s' first load failed: %s", library, why ? why : "no sections");
        Hot_shutdown(plugin);
        return result == HOT_OK ? HOT_ERROR_FILE_NOT_FOUND : result;
    }

    uint32_t slot = (*stack).pluginCount++;
    (*stack).plugins[slot] = plugin;
    snprintf((*stack).pluginNames[slot], HOT_MANIFEST_MAX_NAME, "%s", library);
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
    if ((*stack).core) {
        uint32_t loaded = 0;
        HotResult coreResult = Hot_poll((*stack).core, &loaded);
        if (coreResult != HOT_OK)
            result = coreResult;
        if (loaded > 0) {
            moved.core = true;
            uint64_t contract = readExportAbi((*stack).core, "Hot_contract_abi");
            if (contract != (*stack).coreContract) {
                (*stack).coreContract = contract;
                // The loaded app image was admitted against the previous
                // contract. Re-gate it: an incompatible one goes idle until
                // the ladder serves a matching image.
                if (!appAcceptsContract(stack)) {
                    (*stack).appCompatible = false;
                    snprintf((*stack).lastError, sizeof((*stack).lastError),
                             "app wants core contract %llu, core publishes %llu",
                             (unsigned long long) readExportAbi((*stack).app, "Hot_requires_core_abi"),
                             (unsigned long long) (*stack).coreContract);
                } else {
                    (*stack).appCompatible = true;
                    (*stack).lastError[0] = '\0';
                }
            }
        }
    }

    // APP SECOND. The image is always polled — the loader's own admission is
    // fail-closed, so an incompatible image does not need to be refused a
    // load, it must simply not RUN. Gate after the commit, never before: the
    // requirement lives inside the image being promoted.
    if ((*stack).app) {
        uint32_t loaded = 0;
        HotResult appResult = Hot_poll((*stack).app, &loaded);
        if (appResult != HOT_OK)
            result = appResult;
        if (loaded > 0) {
            moved.app = true;
            if (!appAcceptsContract(stack)) {
                (*stack).appCompatible = false;
                snprintf((*stack).lastError, sizeof((*stack).lastError),
                         "app wants core contract %llu, core publishes %llu",
                         (unsigned long long) readExportAbi((*stack).app, "Hot_requires_core_abi"),
                         (unsigned long long) (*stack).coreContract);
            } else {
                (*stack).appCompatible = true;
                (*stack).lastError[0] = '\0';
            }
        }
    }

    // PLUGINS LAST and independently: a plugin never blocks core or app.
    for (uint32_t i = 0; i < (*stack).pluginCount; i++) {
        if (!(*stack).plugins[i])
            continue;
        uint32_t loaded = 0;
        if (Hot_poll((*stack).plugins[i], &loaded) != HOT_OK)
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
    return stack ? (*stack).core : nullptr;
}

HotModule *Hot_getApplication(const HotStack *stack) {
    return stack ? (*stack).app : nullptr;
}

HotModule *Hot_getPlugin(const HotStack *stack, uint32_t index) {
    if (!stack || index >= (*stack).pluginCount)
        return nullptr;
    return (*stack).plugins[index];
}

uint32_t Hot_getPluginCount(const HotStack *stack) {
    return stack ? (*stack).pluginCount : 0;
}

uint64_t Hot_getCoreContract(const HotStack *stack) {
    return stack ? (*stack).coreContract : 0;
}

bool Hot_isApplicationCompatible(const HotStack *stack) {
    return stack ? (*stack).appCompatible : false;
}

const char *Hot_getStackError(const HotStack *stack) {
    return stack ? (*stack).lastError : nullptr;
}
