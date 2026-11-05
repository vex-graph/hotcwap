#ifndef HOT_HOT_STACK_H
#define HOT_HOT_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "manifest.h"
#include "hot.h"

// hot/stack.h — the opinionated loading order.
//
// The loader (hot.h) knows how to load ONE library. A running application has
// a shape, and that shape is policy, not application code:
//
//   core   the engine image (R2–R4). Loads first. Publishes a contract number.
//   app    the application image. Refused unless it wants the loaded contract.
//   plugin an independent image (R5–R6 add-ons). No gate, any order.
//
// HotStack owns those HotModules and enforces the order, so an application's
// setup file is three calls and nothing else:
//
//   HotStack *stack = HotStack();          // defaults: "core" + "app"
//   Hot_loadCore(stack);
//   Hot_loadApplication(stack);
//   Hot_loadPlugin(stack, "gallery");
//
// and its frame loop is one:
//
//   Hot_pollAll(stack);                    // core, then app, then plugins
//
// The contract gate: the core image exports Hot_contract_abi(), the app image
// exports Hot_requires_core_abi(). When core swaps to a contract the running
// app does not want, the stack unloads the app and keeps polling — the app
// comes back on its own once a compatible one is promoted. Nothing here
// inspects a payload by path; the ladder stays the authority.

#define HOT_STACK_CORE_LIBRARY "core"
#define HOT_STACK_APP_LIBRARY  "app"
#define HOT_STACK_MAX_PLUGINS  8

typedef struct HotStack HotStack;

// Which layers moved during the last Hot_pollAll.
typedef struct HotStackChanged {
    bool core;
    bool app;
    bool plugins;
} HotStackChanged;

// Default library keys: "core" and "app".
HotStack *HotStack_0(void);
// Custom library keys for the engine and application images.
HotStack *HotStack_2(const char *coreLibrary, const char *appLibrary);

// Two arms: the chooser takes one placeholder for the empty-args case plus the
// two constructors, so NAME resolves to the right arm for any argument count.
#define HOT_STACK_CHOOSER(_0, _1, NAME, ...) NAME
#define HotStack(...) HOT_STACK_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    HotStack_2, HotStack_0 \
)(__VA_ARGS__)

// Shut down every held module (plugins, app, then core) and free the stack.
void HotStack_free(HotStack *stack);

// --- the three loading concerns -------------------------------------------

// Bind and first-load the core image. Records its published contract.
HotResult Hot_loadCore(HotStack *stack);

// Bind and first-load the application image, gated on the loaded core
// contract. HOT_ERROR_ABI_MISMATCH when the app wants a different contract.
HotResult Hot_loadApplication(HotStack *stack);

// Bind and first-load an independent image. No gate: plugins do not declare a
// core contract, and their absence never blocks the stack.
HotResult Hot_loadPlugin(HotStack *stack, const char *library);

// --- the frame call --------------------------------------------------------

// Poll core, then app, then every plugin. When core moves, its new contract is
// re-read and an incompatible app is unloaded so the next pass can retry.
// `changed` is optional.
HotResult Hot_pollAll(HotStack *stack, HotStackChanged *changed);

// --- getters (the Symmetric Getter/Setter Completeness Law) ----------------

HotModule *Hot_getCore(const HotStack *stack);
HotModule *Hot_getApplication(const HotStack *stack);
HotModule *Hot_getPlugin(const HotStack *stack, uint32_t index);
uint32_t Hot_getPluginCount(const HotStack *stack);
uint64_t Hot_getCoreContract(const HotStack *stack);
bool Hot_isApplicationCompatible(const HotStack *stack);
const char *Hot_getStackError(const HotStack *stack);

#endif
