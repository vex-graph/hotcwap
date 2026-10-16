#ifndef HOT_HOT_H
#define HOT_HOT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// hot/hot.h — Hotloading system for vex.
//
// THE MANIFEST IS THE MANIFESTATION: a HotModule is bound to one manifest
// LIBRARY key (the bin/current/<library> ladder slot), not to a watched
// directory. The swap trigger is the per-library generation stamp
// (bin/current/<library>.generation): MANIFEST_PROMOTE bumps it for every
// promoted library, and Hot_poll reloads bin/current/<library> when the
// stamp moves. The rename slide IS the swap — no clone step, no watch dir.
//
// Hot_poll() is a two-phase handshake on MANIFEST_UPDATE/PROMOTE:
//   1. snapshot the CURRENT generation's module state OFF-THREAD (a worker
//      thread), so hot loops never pay the serialization cost, then
//   2. on a later pass, dlopen every section from bin/current/<library>,
//      verify the whole library (fail-closed), rehydrate the saved blobs
//      into the STAGED images BEFORE any commit, then atomically swap the
//      trampoline table and retire the old handles into the grace ring.
//      A section whose Hot_restore rejects its blob rolls the whole swap
//      back (#8.5 Automated State Rollback): staged handles close, the old
//      generation stays live, and the generation never advances — the next
//      poll re-attempts once the payload is fixed.
//
// Old handles stay mapped HOT_RETIRED_GENERATIONS polls (the retire ring)
// so in-flight calls into the old generation drain before dlclose.
//
// The window/AppKit side is owned by the OS, not the engine. When a dylib
// is reloaded, the NSWindow/NSView/CAMetalLayer persist — only the Vulkan
// offscreen GPU objects and Metal drawable resources are recreated.
//
// Hot_poll() runs on _main thread only; the state-save worker is this class's
// one supervised thread (joined bounded per the Bounded Wait Law).

typedef enum {
    HOT_OK = 0,
    HOT_ERROR_FILE_NOT_FOUND,
    HOT_ERROR_DLOPEN_FAILED,
    HOT_ERROR_DLSYM_FAILED,
    HOT_ERROR_ABI_MISMATCH,
    HOT_ERROR_VERSION_MISMATCH,
    HOT_ERROR_INIT_FAILED,
    HOT_ERROR_RESTORE_FAILED,
    HOT_ERROR_OUT_OF_MEMORY,
    HOT_ERROR_RECOVERY_REQUIRED,
} HotResult;

// Module handle — opaque
typedef struct HotModule HotModule;

// SLOT RECORD owned by HotModule's cold admission; descriptors are supplied by
// a trusted local builder/release source, not by model output. Exact filenames.
typedef struct HotSectionDigest {
    const char *filename;
    uint8_t sha256[32];
} HotSectionDigest;

// INTENTIONAL(vex): this cold-update ABI number describes the module export
// contract, not a manifest generation or an vexgraph-ecosystem-gitrepo class identity.
#define HOT_COLD_ABI_VERSION 1ULL

// Explicit stop-the-world transaction. Caller stops admission and excludes ALL
// module callers/callbacks until return, including pending external sessions.
// Complete flat dylib payload only; no section removals. Verify copies before
// dlopen, require exported uint64_t Hot_cold_abi, snapshot synchronously, restore
// candidates before commit. Failure preserves running code/state and attempts
// disk revert. No downloader/signature verifier, hostile-directory sandbox,
// crash recovery or arbitrary-code rollback. Constructors/exports are trusted.
HotResult Hot_updateCold(HotModule *hot, const char *payloadDir,
                         const HotSectionDigest *digests, size_t count, uint32_t *loadedCount);

// Bind the hotloader to one manifest library key (e.g. "hot_behavior").
// Requires the manifest to be mounted first (MANIFEST() must have run; the
// library may be reflected or not yet installed). Returns NULL on failure.
HotModule *Hot_init(const char *library);

// Shutdown the hotloader: bounded-join the save worker FIRST (its cond wait
// is capped), then close every module, drain the retire ring, free.
void HotShutdown(HotModule *hot);

// Poll for updates. Call once per frame from the _main loop (_main thread
// only). Reloads bin/current/<library> when the generation stamp moves;
// returns HOT_OK if nothing changed, HOT_OK + loaded_count > 0 when a swap
// landed. A swap that must preserve state spans two polls: this call kicks
// the off-thread snapshot and returns; the NEXT call swaps once the worker
// has published it. A new image whose Hot_restore rejects the saved blob
// rolls the swap back and returns HOT_ERROR_RESTORE_FAILED (generation
// unchanged, old code + state still live).
HotResult Hot_poll(HotModule *hot, uint32_t *loaded_count);

// Get a function pointer by name (trampoline table lookup, stable until the
// next swap of its module).
typedef void (*HotFn)(void);
HotFn Hot_get_symbol(HotModule *hot, const char *name);

// Last-seen generation of bin/current/<library> (0 before any successful
// load/swap). Getters: after a swap Hot_poll advances this; it equals
// MANIFEST_GENERATION(<library>) when the loader and ladder are in sync.
uint64_t Hot_get_generation(const HotModule *hot);
// A disk rollback failure is unresolved: do not resume admission/retry in-process.
bool Hot_requiresRecovery(const HotModule *hot);

// Get the last error string (for diagnostics).
const char *Hot_last_error(HotModule *hot);

// Phase-1 lifecycle: graceful per-module teardown + state handoff.
// Shutdown calls the module's Hot_shutdown_module (if exported) on the
// currently loaded handle. Save/Restore move an opaque state blob across
// a swap: Hot_poll saves from the old handle before dlopen and restores
// into the new handle after load. Modules without Hot_save / Hot_restore
// simply skip the handoff. Returns false when the module is unknown,
// unloaded, or the symbol is missing / buffer too small.
void Hot_shutdown_module(HotModule *hot, const char *module_name);
bool Hot_save_module(HotModule *hot, const char *module_name, void *buf, size_t cap, size_t *outLen);
bool Hot_restore_module(HotModule *hot, const char *module_name, const void *buf, size_t len);

// Phase-2 migration: translate a state blob saved by oldVersion into the
// current module's schema. Calls the module's Hot_migrate export; false
// when the module is unknown or exports no migrator.
bool Hot_migrate_module(HotModule *hot, const char *module_name, const char *oldVersion,
                        const void *oldBuf, size_t oldLen, void *newBuf, size_t newCap, size_t *outLen);

#endif
