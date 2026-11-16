#include "vex.h"
#include "hot.h"
#include "manifest.h"
#include "hot/trampoline_table.h"
#include "hot/retire_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: HotModule
 * ============================================================================
 * Manifest-driven dynamic module hot-reloading manager. Binds to an on-disk
 * manifest library key (bin/current/<library>), detects generation advances
 * via the monotonic generation stamp, and manages non-blocking module swaps.
 *
 * Coordinates off-thread state capture (Hot_save) on a dedicated worker thread,
 * fail-closed symbol validation and dylib loading across library sections, atomic
 * trampoline swapping, and automated rollback if state restoration (Hot_restore)
 * fails. Old dylibs are retired into the per-instance HotRetireRing to allow
 * in-flight invocations to drain safely without crashes.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: HotModule (hot/hot.c)
 * ============================================================================
 * SUMMARY:
 *   The manifest-driven hotloader. A HotModule binds ONE library key of the
 *   mounted manifest (bin/current/<library>). Hot_poll compares
 *   MANIFEST_GENERATION(<library>) against its last-seen generation: on a move
 *   it snapshots the CURRENT generation's module state on an off-thread save
 *   worker (hot loops never pay the serialization), then on a later pass it
 *   dlopens every section of the new current set, verifies the WHOLE library
 *   fail-closed (dlopen + Hot_getExports on every section before any
 *   commit), rehydrates the saved blobs into the STAGED images BEFORE any
 *   commit, then atomically swaps the trampoline table and retires the old
 *   handles into the grace ring. A section whose Hot_restore rejects its blob
 *   rolls the whole swap back (Automated State Rollback): the old
 *   generation stays live, the generation never advances, and the next poll
 *   re-attempts once the payload is fixed.
 *   The rename slide (MANIFEST_PROMOTE) IS the swap — there is no clone step
 *   and no watch dir. Hot_poll() runs on _main thread only.
 *
 * STRUCT FIELDS (defined here; hot/hot.h keeps the type opaque):
 * ----------------------------------------------------------------------------
 *   char library[HOT_MANIFEST_MAX_NAME]; // manifest library key this loader watches
 *   u64 generation;                 // last COMMITTED bin/current/<lib> generation
 *   HotModuleInternal modules[HOT_MAX_MODULES]; // per-section slots (max 32)
 *   u32 module_count;               // used slots in modules[]
 *   char last_error[256];                // last diagnostic string
 *   HotTrampolineTable trampolines;      // per-instance symbols
 *   HotRetireRing retireRing;            // per-instance generational close
 *   pthread_t saveThread;                // off-thread state-save worker
 *   pthread_mutex_t saveLock;            // guards blobs + phase writes
 *   pthread_cond_t saveCond;             // worker sleep/wake
 *   atomic_uint savePhase;               // HotSavePhase: the one handshake word
 *   HotSaveSlot saveSlots[HOT_MAX_MODULES]; // snapshot blobs (worker writes, poll reads)
 *   bytes saveCount;                    // populated slots
 *   const HotSectionDigest *coldDigests; // borrowed trusted cold descriptor
 *   bytes coldCount;                    // descriptor extent; zero in legacy polls
 *   bool recoveryRequired;              // unresolved disk rollback; stop admission
 *
 * PRIVATE HELPERS (kept file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   HotModuleInternal (per-section slot — HotModule's slot type, no own API):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem (no extension)
 *     char path[HOT_PATH_LEN];           // source dylib path
 *     any handle;                      // dlopen handle (NULL = unloaded)
 *     bool loaded;                       // true once first load succeeds
 *
 *   HotSaveSlot (per-section state blob captured off-thread):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem the blob belongs to
 *     u8 buf[HOT_SAVE_SLOT_CAP];    // module Hot_save payload (4 KiB)
 *     bytes len;                        // valid Bytes in buf
 *
 *   HotStagedSection (verify-phase staging, transient inside perform_swap):
 *     HotModuleInternal *mod;   // resolved slot (created if new)
 *     char path[HOT_PATH_LEN];  // dylib path
 *     any handle;             // dlopened, not yet adopted
 *     const_any trampolines;  // Hot_getExports table
 *     u32 trampolineCount; // entries
 *
 *   Segregated (own files, see their overviews):
 *     HotTrampoline in hot/hot_trampoline.h and hot/hot_trampoline.c
 *     HotRetiredHandle in hot/hot_retire.h and hot/hot_retire.c
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Hot_init(library)                      : Allocate and bind to library key
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Hot_shutdown(hot)                       : Join save worker and drain modules
 *   - Hot_poll(hot, loaded_count)            : Two-phase generational reload poll
 *   - Hot_updateCold(hot, payloadDir, digests, count, loadedCount)
 *       Caller-excluded synchronous checksum/ABI/save/restore transaction;
 *       candidates discarded on failure, running generation unchanged.
 *   - Hot_shutdownModule(hot, name)         : Invoke module teardown export
 *   - Hot_saveModule(hot, name, b.sh, c, len)  : Capture module state blob
 *   - Hot_restoreModule(hot, name, buf, l)  : Rehydrate state into new image
 *   - Hot_migrateModule(hot, name, ...)     : Schema migration across versions
 *
 * Private Core Functions: (.c static)
 *   - HotModuleInternal_find(hot, name)                 : Lookup internal module slot
 *   - HotSaveSlot_begin(hot)                    : Wake save worker for off-thread capture
 *   - HotSaveSlot_phase(hot)                    : Read the one handshake word
 *   - HotSaveSlot_consume(hot)                  : CAPTURED -> IDLE before a swap
 *   - snapshot_modules(hot)                  : Worker body capturing state blobs
 *   - HotSaveSlot_workerMain(arg)                  : Dedicated save worker thread entry
 *   - perform_swap(hot)                      : Stage, verify, restore, and swap dylibs
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Hot_getSymbol(hot, name)              : Lookup function pointer in trampolines
 *   - Hot_getGeneration(hot)                : Last committed generation counter
 *   - Hot_requiresRecovery(hot)              : Unresolved rollback blocks cold retry
 *   - Hot_getLastError(hot)                    : Last diagnostic error message
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

;;INTENTION("State-save module exports are trusted bounded: Hot_save copies a "
            "fixed-size state blob (memcpy of statics) and is called on the save "
            "worker thread. The worker's join is therefore bounded to <25ms plus "
            "one module snapshot, satisfying the Bounded Wait Law. If a module "
            "ever exports a Hot_save that loops unboundedly, the violation is at "
            "the module boundary — modules are in-repo trusted code (hotcwap "
            "hot_behavior), never hostile payloads.")

;;INTENTION("The trampoline table is set per-section inside perform_swap's "
            "commit phase: a call landing mid-commit can observe a mixed "
            "generation (section A new, section B old) for the microseconds of "
            "the swap. Module call sites reach code only through trampolines at "
            "frame boundaries and old images stay mapped in the retire ring "
            "throughout, so this window is safe. Full-library atomicity is "
            "enforced on the VERIFY side (no section is adopted unless every "
            "section dlopens + exports Hot_getExports); the commit side "
            "is per-row assignment per the Conflict Triage Law.")

;;INTENTION("A library's declared section set is stable across its life (the "
            "manifest catalog is the contract; MANIFEST_UPDATE fails-closed "
            "against undeclared stems). A section that vanishes from a promoted "
            "current set keeps its old slot + handle mapped (never retired) so "
"stale trampolines never dangle — the unshared image lingers until "
             "Hot_shutdown. Removing a section is a library-uninstall event "
             "handled by a full restart, per the Conflict Triage Law.")

;;INTENTION("Restore-before-commit (#8.5 Automated State Rollback): a "
            "Hot_restore rejection — or a missing Hot_restore on a slot the "
            "previous generation snapshotted — fails the WHOLE swap: staged "
            "images close, old trampolines + state stay live, and the "
            "generation never advances. The ladder's current set keeps the "
            "bad payload (rejected again on every poll) until the deployer "
            "promotes a fixed set, so rollback self-heals. A module that "
            "intentionally drops Hot_save/Hot_restore must re-scope its "
            "state handoff first.")

// The loader's internal model lives one class per file under hot/:
//   hot/limits.h            named bounds
//   hot/module_internal.h   HotModuleInternal  — one loaded section row
//   hot/save_slot.h         HotSaveSlot        — one serialized module state
//   hot/staged_section.h    HotStagedSection   — one section between dlopen and commit
//   hot/module.h            HotModule          — the loader instance itself
#include "hot/module.h"
#include "hot/internal.h"
#include "hot/staged_section.h"

;;GETTER
static inline const char *Dirent_d_name(const struct dirent *ent) { return (*ent).d_name; }
;;GETTER
static inline const char *HotModule_library(const HotModule *hot) { return (*hot).library; }
;;GETTER
static inline char *HotModule_libraryMut(HotModule *hot) { return (*hot).library; }
;;GETTER
static inline u64 HotModule_generation(const HotModule *hot) { return (*hot).generation; }
;;SETTER
static inline fn HotModule_setGeneration(HotModule *hot, u64 gen) { (*hot).generation = gen; }
;;GETTER
static inline HotModuleInternal *HotModule_moduleAtMut(HotModule *hot, u32 i) { return &(*hot).modules[i]; }
;;GETTER
static inline u32 HotModule_moduleCount(const HotModule *hot) { return (*hot).module_count; }
;;SETTER
static inline fn HotModule_setModuleCount(HotModule *hot, u32 count) { (*hot).module_count = count; }
;;GETTER
static inline char *HotModule_lastErrorMut(HotModule *hot) { return (*hot).last_error; }
;;GETTER
static inline HotTrampolineTable *HotModule_trampolinesMut(HotModule *hot) { return &(*hot).trampolines; }
;;GETTER
static inline HotRetireRing *HotModule_retireRingMut(HotModule *hot) { return &(*hot).retireRing; }
;;GETTER
static inline pthread_t *HotModule_saveThreadMut(HotModule *hot) { return &(*hot).saveThread; }
;;GETTER
static inline pthread_mutex_t *HotModule_saveLockMut(HotModule *hot) { return &(*hot).saveLock; }
;;GETTER
static inline pthread_cond_t *HotModule_saveCondMut(HotModule *hot) { return &(*hot).saveCond; }
;;GETTER
static inline HotSaveSlot *HotModule_saveSlotAtMut(HotModule *hot, bytes i) { return &(*hot).saveSlots[i]; }
;;GETTER
static inline bytes HotModule_saveCount(const HotModule *hot) { return (*hot).saveCount; }
;;SETTER
static inline fn HotModule_setSaveCount(HotModule *hot, bytes count) { (*hot).saveCount = count; }
;;SETTER
static inline fn HotModule_setColdDigests(HotModule *hot, const HotSectionDigest *d) { (*hot).coldDigests = d; }
;;GETTER
static inline const HotSectionDigest *HotModule_coldDigestAt(const HotModule *hot, bytes i) {
    return &(*hot).coldDigests[i];
}
;;GETTER
static inline bytes HotModule_coldCount(const HotModule *hot) { return (*hot).coldCount; }
;;SETTER
static inline fn HotModule_setColdCount(HotModule *hot, bytes count) { (*hot).coldCount = count; }
;;GETTER
static inline bool HotModule_recoveryRequired(const HotModule *hot) { return (*hot).recoveryRequired; }
;;SETTER
static inline fn HotModule_setRecoveryRequired(HotModule *hot, bool req) { (*hot).recoveryRequired = req; }
;;GETTER
static inline const char *HotModuleInternal_name(const HotModuleInternal *mod) { return (*mod).name; }
;;GETTER
static inline const char *HotModuleInternal_path(const HotModuleInternal *mod) { return (*mod).path; }
;;GETTER
static inline char *HotModuleInternal_pathMut(HotModuleInternal *mod) { return (*mod).path; }
;;GETTER
static inline any HotModuleInternal_handle(const HotModuleInternal *mod) { return (*mod).handle; }
;;SETTER
static inline fn HotModuleInternal_setHandle(HotModuleInternal *mod, any h) { (*mod).handle = h; }
;;GETTER
static inline bool HotModuleInternal_loaded(const HotModuleInternal *mod) { return (*mod).loaded; }
;;SETTER
static inline fn HotModuleInternal_setLoaded(HotModuleInternal *mod, bool l) { (*mod).loaded = l; }
;;GETTER
static inline const char *HotSaveSlot_name(const HotSaveSlot *slot) { return (*slot).name; }
;;GETTER
static inline char *HotSaveSlot_nameMut(HotSaveSlot *slot) { return (*slot).name; }
;;GETTER
static inline u8 *HotSaveSlot_bufMut(HotSaveSlot *slot) { return (*slot).buf; }
;;GETTER
static inline bytes HotSaveSlot_len(const HotSaveSlot *slot) { return (*slot).len; }
;;SETTER
static inline fn HotSaveSlot_setLen(HotSaveSlot *slot, bytes len) { (*slot).len = len; }
;;GETTER
static inline HotModuleInternal *HotStagedSection_mod(const HotStagedSection *s) { return (*s).mod; }
;;GETTER
static inline const char *HotStagedSection_path(const HotStagedSection *s) { return (*s).path; }
;;GETTER
static inline any HotStagedSection_handle(const HotStagedSection *s) { return (*s).handle; }
;;GETTER
static inline u32 HotTrampolineTable_countLoad(const HotTrampolineTable *t) { return atomic_load(&(*t).count); }
;;GETTER
static inline HotTrampoline *HotTrampolineTable_rowAtMut(const HotTrampolineTable *t, u32 i) {
    return &(*t).rows[i];
}
;;GETTER
static inline const char *HotTrampoline_name(const HotTrampoline *r) { return (*r).name; }
;;GETTER
static inline fn HotTrampoline_clearFallback(HotTrampoline *r) { atomic_store(&(*r).fallback_ptr, nullptr); }
;;GETTER
static inline const char *HotSectionDigest_filename(const HotSectionDigest *d) { return (*d).filename; }
;;GETTER
static inline const u8 *HotSectionDigest_sha256(const HotSectionDigest *d) { return (*d).sha256; }

// CONSTRUCTORS — HotModule owns the module slots below; row mechanics live
// in hot_trampoline.c, generational close in hot_retire.c.

// Off-thread state-save worker (HOT_SAVE_WAIT_NS cond-wait cycle, cancel-aware).
// CONSTRUCTORS (PUBLIC & PRIVATE)
/** Creates a loader bound to a mounted manifest library and starts its save worker. */
HotModule *Hot_init(const char *library) {
    if (!library || *library == '\0')
        return nullptr;
    if (strlen(library) >= HOT_MANIFEST_MAX_NAME)
        return nullptr;
    if (!MANIFEST_ROOT())
        return nullptr; // manifest must be mounted before the loader binds

    HotModule *hot = (HotModule*) calloc(1, sizeof(HotModule));
    if (!hot)
        return nullptr;

    char *lib = HotModule_libraryMut(hot);
    strncpy(lib, library, HOT_MANIFEST_MAX_NAME - 1);
    lib[HOT_MANIFEST_MAX_NAME - 1] = '\0';

    pthread_mutex_init(HotModule_saveLockMut(hot), nullptr);
    pthread_cond_init(HotModule_saveCondMut(hot), nullptr);
    // calloc left savePhase at HOT_SAVE_IDLE (0); the worker publishes from there.
    if (pthread_create(HotModule_saveThreadMut(hot), nullptr, HotSaveSlot_workerMain, hot) != 0) {
        pthread_mutex_destroy(HotModule_saveLockMut(hot));
        pthread_cond_destroy(HotModule_saveCondMut(hot));
        free(hot);
        return nullptr;
    }
    return hot;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)

/** Stops the state-save worker, closes loaded modules, drains retired handles, and frees. */
fn Hot_shutdown(HotModule *hot) {
    if (!hot)
        return;

    HotSaveSlot_workerShutdown(hot);
    pthread_mutex_destroy(HotModule_saveLockMut(hot));
    pthread_cond_destroy(HotModule_saveCondMut(hot));

    u32 modCount = HotModule_moduleCount(hot);
    for (u32 i = 0; i < modCount; i++) {
        HotModuleInternal *mod = HotModule_moduleAtMut(hot, i);
        any handle = HotModuleInternal_handle(mod);
        if (handle) {
            typedef fn (*ShutdownFn)();
            ShutdownFn shutdown = dlsym(handle, "Hot_shutdown_module");
            if (shutdown)
                shutdown();
            dlclose(handle);
            HotModuleInternal_setHandle(mod, nullptr);
            HotModuleInternal_setLoaded(mod, false);
        }
    }

    // Drain retired handle ring
    HotRetireRing_drainAll(HotModule_retireRingMut(hot));

    HotTrampolineTable_free(HotModule_trampolinesMut(hot));

    free(hot);
}

// Load a whole manifest library generation and commit it. Phase A verifies
// EVERY section (dlopen + Hot_getExports present) before anything is
// touched — on any failure every staged handle is closed and the previous
// generation stays live, untouched. Phase B rehydrates the saved blobs into
// the STAGED images BEFORE any commit; a rejecting Hot_restore rolls the
// whole swap back (#8.5 Automated State Rollback). Phase C commits: retire
// old handles, adopt new, swap trampoline rows. Generation only advances on
// full success, so a failed swap re-enters the handshake on the next poll
// and self-heals once the payload is fixed.
/** Stages and validates every section, restores saved state, then commits the generation. */
static HotResult perform_verified_swap(HotModule *hot, const char *libDir, u32 *outLoaded) {
    HotTrampolineTable *table = HotModule_trampolinesMut(hot);
    HotRetireRing *ring = HotModule_retireRingMut(hot);
    u32 loaded = 0;

    DIR *dir = opendir(libDir);
    if (!dir) {
        snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                 "Cannot open manifest library dir: %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    // Phase A — verify (fail-closed: all-or-nothing). Each section is mapped
    // and validated by hot/staged_section.c; a rejection closes everything
    // staged so far and leaves the live generation untouched.
    HotStagedSection staged[HOT_MAX_MODULES];
    u32 stagedCount = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        // readdir storage dies at closedir; diagnostics on rejection must own
        // their filename before closing the iterator.
        char name[HOT_PATH_LEN];
        snprintf(name, sizeof(name), "%s", Dirent_d_name(ent));
        if (!HotStagedSection_isImage(name))
            continue;
        if (!MANIFEST_ALLOWS(HotModule_library(hot), name)) {
            closedir(dir);
            for (u32 i = 0; i < stagedCount; ++i)
                dlclose(staged[i].handle);
            snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error), "undeclared section: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }
        if (stagedCount >= HOT_MAX_MODULES) {
            closedir(dir);
            for (u32 i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                     "Too many sections (limit %d) in %s", HOT_MAX_MODULES, libDir);
            return HOT_ERROR_OUT_OF_MEMORY;
        }

        HotResult admitted = HotStagedSection_admit(hot, libDir, name, &staged[stagedCount]);
        if (admitted != HOT_OK) {
            closedir(dir);
            for (u32 i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            return admitted;
        }

        // Two sections resolving to the same identity make dispatch
        // order-dependent; reject the complete candidate before any restore
        // or target publication.
        bool duplicate = false;
        for (u32 i = 0; i < stagedCount; ++i)
            if (staged[i].mod == staged[stagedCount].mod)
                duplicate = true;
        if (duplicate) {
            closedir(dir);
            dlclose(staged[stagedCount].handle);
            for (u32 i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                     "duplicate section identity: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }

        stagedCount++;
    }
    closedir(dir);

    bytes coldCount = HotModule_coldCount(hot);
    if (coldCount) {
        // Duplicate public identities make dispatch order-dependent; reject the
        // complete candidate before any restore or target publication.
        for (u32 i = 0; i < stagedCount; ++i) {
            const struct { const char *name; any fn; } *left = staged[i].trampolines;
            for (u32 k = 0; k < staged[i].trampolineCount; ++k) {
                for (u32 j = 0; j <= i; ++j) {
                    const struct { const char *name; any fn; } *right = staged[j].trampolines;
                    u32 limit = j == i ? k : staged[j].trampolineCount;
                    for (u32 r = 0; r < limit; ++r) {
                        if (strcmp(left[k].name, right[r].name) == 0) {
                            for (u32 m = 0; m < stagedCount; ++m)
                                dlclose(staged[m].handle);
                            return HOT_ERROR_ABI_MISMATCH;
                        }
                    }
                }
            }
        }
    }

    if (stagedCount == 0) {
        snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                 "No loadable sections in %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    u64 newGen = MANIFEST_GENERATION(HotModule_library(hot));

    // Cold updates cannot strand an old export in a handle we will unload.
    // Removal is an ABI change requiring a resident host restart.
    if (coldCount) {
        u32 oldCount = HotTrampolineTable_countLoad(table);
        for (u32 r = 0; r < oldCount; ++r) {
            HotTrampoline *row = HotTrampolineTable_rowAtMut(table, r);
            const char *rname = HotTrampoline_name(row);
            bool present = false;
            for (u32 i = 0; i < stagedCount; ++i) {
                const struct { const char *name; any fn; } *exports = staged[i].trampolines;
                for (u32 k = 0; k < staged[i].trampolineCount; ++k) {
                    if (strcmp(rname, exports[k].name) == 0)
                        present = true;
                }
            }
            if (!present) {
                for (u32 i = 0; i < stagedCount; ++i)
                    dlclose(staged[i].handle);
                snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                         "cold update removed export: %s", rname);
                return HOT_ERROR_ABI_MISMATCH;
            }
        }
    }

    // Phase B — rehydrate the saved state into the STAGED (new) images BEFORE
    // any commit (#8.5 Automated State Rollback). A module that rejects the
    // previous generation's blob fails the whole swap: every staged handle
    // closes, the old generation (code + trampolines + state) stays live, and
    // the generation does NOT advance — the next poll re-enters the handshake
    // and re-attempts once the payload is fixed.
    bytes saveCount = HotModule_saveCount(hot);
    u64 curGen = HotModule_generation(hot);
    for (bytes i = 0; i < saveCount; i++) {
        HotSaveSlot *slot = HotModule_saveSlotAtMut(hot, i);
        const char *sname = HotSaveSlot_name(slot);
        HotStagedSection *match = nullptr;
        for (u32 k = 0; k < stagedCount; k++) {
            if (strcmp(HotModuleInternal_name(staged[k].mod), sname) == 0)
                match = &staged[k];
        }
        if (!match)
            continue; // section absent from the new set; its old slot stays mapped
        typedef bool (*RestoreFn)(const_any , bytes);
        RestoreFn restore = (RestoreFn) dlsym(HotStagedSection_handle(match), "Hot_restore");
        u8 *sbuf = HotSaveSlot_bufMut(slot);
        bytes slen = HotSaveSlot_len(slot);
        bool restored = (restore && restore(sbuf, slen));
        if (!restored && restore) {
            typedef bool (*MigrateFn)(const char *, const_any , bytes, any , bytes, bytes *);
            MigrateFn migrate = (MigrateFn) dlsym(HotStagedSection_handle(match), "Hot_migrate");
            if (migrate) {
                alignas(16) u8 migratedBuf[HOT_SAVE_SLOT_CAP];
                bytes migratedLen = 0;
                char oldGenStr[32];
                snprintf(oldGenStr, sizeof(oldGenStr), "%llu.0.0", (u64) curGen);
                if ((migrate(oldGenStr, sbuf, slen, migratedBuf, sizeof(migratedBuf), &migratedLen) ||
                     (!coldCount && migrate("1.0.0", sbuf, slen, migratedBuf, sizeof(migratedBuf), &migratedLen))) &&
                    migratedLen > 0 && migratedLen <= sizeof(migratedBuf)) {
                    restored = restore(migratedBuf, migratedLen);
                }
            }
        }
        if (!restored) {
            for (u32 k = 0; k < stagedCount; k++)
                dlclose(staged[k].handle); // never adopted — safe to close directly
            HotModule_setSaveCount(hot, 0);
            snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                     "Restore rejected by %s (%zu Bytes) — generation %llu stays live",
                     sname, slen, (u64) curGen);
            fprintf(stderr, "[hot] ROLLBACK: %s rejected restored state (%zu Bytes) — "
                            "keeping generation %llu live, generation NOT advanced\n",
                    sname, slen, (u64) curGen);
            return HOT_ERROR_RESTORE_FAILED;
        }
    }

    // Reserve every candidate symbol before any target changes. The caller must
    // exclude table readers during cold backing growth; published targets alone
    // do not make relocation safe. Repeated updates reuse existing names.
    u64 required = HotTrampolineTable_countLoad(table);
    for (u32 i = 0; i < stagedCount; ++i)
        required += staged[i].trampolineCount;
    if (required > INT_MAX || !HotTrampolineTable_reserve(table, (u32) required)) {
        for (u32 i = 0; i < stagedCount; ++i)
            dlclose(staged[i].handle);
        HotModule_setSaveCount(hot, 0);
        return HOT_ERROR_OUT_OF_MEMORY;
    }

    // Phase C — commit: retire old handles, adopt new, swap trampoline rows.
    // State already lives inside the fresh images (Phase B restored in place).
    for (u32 i = 0; i < stagedCount; i++) {
        HotStagedSection *s = &staged[i];
        HotModuleInternal *mod = HotStagedSection_mod(s);
        any oldHandle = HotModuleInternal_handle(mod);
        if (oldHandle)
            HotRetireRing_retire(ring, oldHandle);
        HotModuleInternal_setHandle(mod, HotStagedSection_handle(s));
        HotModuleInternal_setLoaded(mod, true);
        char *mpath = HotModuleInternal_pathMut(mod);
        strncpy(mpath, HotStagedSection_path(s), HOT_PATH_LEN - 1);
        mpath[HOT_PATH_LEN - 1] = '\0';

        const struct { const char *name; any fn; } *entries = staged[i].trampolines;
        u32 tcount = staged[i].trampolineCount;
        for (u32 k = 0; k < tcount; k++) {
            i32 tidx = HotTrampolineTable_find(table, entries[k].name);
            if (tidx < 0)
                tidx = HotTrampolineTable_register(table, entries[k].name);
            if (tidx >= 0)
                HotTrampolineTable_set(table, tidx, entries[k].fn);
        }
        loaded++;
        fprintf(stderr, "[hot] reloaded %s (generation %llu)\n",
                HotModuleInternal_name(mod), (u64) newGen);
    }
    HotModule_setSaveCount(hot, 0);

    HotModule_setGeneration(hot, newGen);

    if (outLoaded)
        (*outLoaded) = loaded;
    return HOT_OK;
}

// Cold release: copy and hash ALL private images before any dlopen constructor
// executes. Stable trusted parents and external serialization remain mandatory.
static HotResult perform_swap(HotModule *hot, const char *libDir, u32 *outLoaded) {
    bytes coldCount = HotModule_coldCount(hot);
    if (!coldCount)
        return perform_verified_swap(hot, libDir, outLoaded);
    char privateDir[HOT_PATH_LEN];
    i32 n = snprintf(privateDir, sizeof(privateDir), "%s/cache/load/%s/release-%u",
                     MANIFEST_ROOT(), HotModule_library(hot), HotStaging_nextSerial());
    if (n < 0 || (bytes) n >= sizeof(privateDir) || !HotStaging_mkdirs(privateDir))
        return HOT_ERROR_FILE_NOT_FOUND;
    HotResult result = HOT_ERROR_ABI_MISMATCH;
    bytes copied = 0;
    for (bytes i = 0; i < coldCount; ++i) {
        const HotSectionDigest *item = HotModule_coldDigestAt(hot, i);
        const char *fname = HotSectionDigest_filename(item);
        char source[HOT_PATH_LEN], dest[HOT_PATH_LEN];
        n = snprintf(source, sizeof(source), "%s/%s", libDir, fname);
        i32 d = snprintf(dest, sizeof(dest), "%s/%s", privateDir, fname);
        if (n < 0 || (bytes) n >= sizeof(source) || d < 0 || (bytes) d >= sizeof(dest))
            break;
        copied = i + 1; // cleanup also owns any partial failed copy
        if (!HotStaging_copy(source, dest) || !HotStaging_digestMatches(dest, HotSectionDigest_sha256(item)))
            break;
        if (i + 1 == coldCount)
            result = perform_verified_swap(hot, privateDir, outLoaded);
    }
    for (bytes i = 0; i < copied; ++i) {
        const HotSectionDigest *item = HotModule_coldDigestAt(hot, i);
        char dest[HOT_PATH_LEN];
        n = snprintf(dest, sizeof(dest), "%s/%s", privateDir, HotSectionDigest_filename(item));
        if (n >= 0 && (bytes) n < sizeof(dest))
            unlink(dest);
    }
    rmdir(privateDir);
    return result;
}

HotResult Hot_updateCold(HotModule *hot, const char *payloadDir,
                         const HotSectionDigest *digests, bytes count, u32 *loadedCount) {
    if (loadedCount != nullptr)
        *loadedCount = 0;
    if (hot != nullptr && HotModule_recoveryRequired(hot)) {
        THROW("cold update: unresolved disk rollback requires host recovery");
        return HOT_ERROR_RECOVERY_REQUIRED;
    }
    const char *lib = hot != nullptr ? HotModule_library(hot) : nullptr;
    if (hot == nullptr || payloadDir == nullptr || digests == nullptr || count == 0 ||
        count > HOT_MAX_MODULES || count != MANIFEST_SECTION_COUNT(lib) ||
        HotSaveSlot_phase(hot) != HOT_SAVE_IDLE) {
        THROW("cold update: invalid admission or a snapshot handshake is in flight");
        return HOT_ERROR_INIT_FAILED;
    }
    HotResult result = HOT_ERROR_ABI_MISMATCH;
    u32 originalModuleCount = HotModule_moduleCount(hot);
    DIR *dir = opendir(payloadDir);
    if (dir == nullptr) {
        THROW("cold update: cannot open payload");
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    HotModule_setColdDigests(hot, digests);
    HotModule_setColdCount(hot, count);
    bytes found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char *name = Dirent_d_name(entry);
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        const HotSectionDigest *digest = HotStaging_coldDigest(hot, name);
        char path[HOT_PATH_LEN];
        i32 n = snprintf(path, sizeof(path), "%s/%s", payloadDir, name);
        struct stat info;
        bytes len = strlen(name);
        bool dylib = (len > 6 && strcmp(name + len - 6, ".dylib") == 0) ||
                     (len > 3 && strcmp(name + len - 3, ".so") == 0);
        if (digest == nullptr || !dylib || !MANIFEST_ALLOWS(lib, name) ||
            n < 0 || (bytes) n >= sizeof(path) || lstat(path, &info) != 0 ||
            !S_ISREG(info.st_mode) || !HotStaging_digestMatches(path, HotSectionDigest_sha256(digest)))
            break;
        ++found;
    }
    bool complete = entry == nullptr && found == count;
    closedir(dir);
    // Duplicate descriptor names would otherwise disguise an incomplete set.
    for (bytes i = 0; complete && i < count; ++i) {
        const HotSectionDigest *item = &digests[i];
        const char *iname = HotSectionDigest_filename(item);
        if (iname == nullptr || !MANIFEST_ALLOWS(lib, iname)) {
            complete = false;
            break;
        }
        for (bytes k = 0; k < i; ++k) {
            const HotSectionDigest *prior = &digests[k];
            if (strcmp(HotSectionDigest_filename(prior), iname) == 0)
                complete = false;
        }
    }
    // The first cold release must retain every previously loaded section.
    for (u32 i = 0; complete && i < originalModuleCount; ++i) {
        HotModuleInternal *module = HotModule_moduleAtMut(hot, i);
        if (!HotModuleInternal_loaded(module))
            continue;
        const char *base = strrchr(HotModuleInternal_path(module), '/');
        if (base == nullptr || HotStaging_coldDigest(hot, base + 1) == nullptr)
            complete = false;
    }
    if (!complete)
        goto resolve;
    HotModule_setSaveCount(hot, 0);
    // Synchronous under caller exclusion: no worker reads concurrently with use.
    for (u32 i = 0; i < originalModuleCount; ++i) {
        HotModuleInternal *module = HotModule_moduleAtMut(hot, i);
        if (!HotModuleInternal_loaded(module))
            continue;
        typedef bool (*SaveFn)(any , bytes, bytes *);
        SaveFn save = (SaveFn) dlsym(HotModuleInternal_handle(module), "Hot_save");
        if (save == nullptr)
            continue; // stateless module, or all state belongs to resident host
        bytes scount = HotModule_saveCount(hot);
        HotSaveSlot *slot = HotModule_saveSlotAtMut(hot, scount);
        bytes length = 0;
        if (!save(HotSaveSlot_bufMut(slot), sizeof(((HotSaveSlot) {0}).buf), &length) ||
            length == 0 || length > sizeof(((HotSaveSlot) {0}).buf)) {
            result = HOT_ERROR_RESTORE_FAILED;
            goto resolve;
        }
        strcpy(HotSaveSlot_nameMut(slot), HotModuleInternal_name(module));
        HotSaveSlot_setLen(slot, length);
        HotModule_setSaveCount(hot, scount + 1);
    }
    if (!MANIFEST_UPDATE(lib, payloadDir)) {
        result = HOT_ERROR_FILE_NOT_FOUND;
        goto resolve;
    }
    u64 oldGeneration = MANIFEST_GENERATION(lib);
    if (!MANIFEST_PROMOTE_LIBRARY(lib)) {
        bool rec = MANIFEST_REQUIRES_RECOVERY();
        HotModule_setRecoveryRequired(hot, rec);
        result = rec ? HOT_ERROR_RECOVERY_REQUIRED : HOT_ERROR_INIT_FAILED;
        goto resolve;
    }
    char current[HOT_PATH_LEN];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, lib, current, sizeof(current), false))
        result = HOT_ERROR_FILE_NOT_FOUND;
    else
        result = perform_swap(hot, current, loadedCount);
    if (result != HOT_OK && !MANIFEST_REVERT_LIBRARY(lib, oldGeneration)) {
        snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                 "cold update disk rollback failed; recovery required");
        HotModule_setRecoveryRequired(hot, true);
        result = HOT_ERROR_RECOVERY_REQUIRED;
    }
    if (result == HOT_OK) {
        // ALL callers excluded: old code has no active borrower at this boundary.
        HotRetireRing_drainAll(HotModule_retireRingMut(hot));
        // Cold publication needs no old-address fallback; those images are gone.
        HotTrampolineTable *table = HotModule_trampolinesMut(hot);
        u32 rows = HotTrampolineTable_countLoad(table);
        for (u32 i = 0; i < rows; ++i) {
            HotTrampoline *row = HotTrampolineTable_rowAtMut(table, i);
            HotTrampoline_clearFallback(row);
        }
    }
resolve:
    HotModule_setColdDigests(hot, nullptr);
    HotModule_setColdCount(hot, 0);
    HotModule_setSaveCount(hot, 0);
    if (result != HOT_OK)
        HotModule_setModuleCount(hot, originalModuleCount);
    if (result != HOT_OK)
        THROW("cold update rejected: result %d; running generation %llu retained", result, (u64) HotModule_generation(hot));
    return result;
}

/** Advances retirement and handles initial load, pending snapshot, or generation reload. */
HotResult Hot_poll(HotModule *hot, u32 *loaded_count) {
    if (loaded_count)
        (*loaded_count) = 0;   // always define the out-count, even on rejection
    if (!hot)
        return HOT_ERROR_FILE_NOT_FOUND;

    HotRetireRing_advance(HotModule_retireRingMut(hot));

    const char *lib = HotModule_library(hot);
    char libDir[HOT_PATH_LEN];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, lib,
                                 libDir, sizeof(libDir), false)) {
        snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                 "manifest not mounted or unknown library '%s'", lib);
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    u64 stamp = MANIFEST_GENERATION(lib);

    // Complete an in-flight live swap once the off-thread snapshot landed.
    // The phase word is the whole handshake: REQUESTED waits, CAPTURED swaps,
    // STOPPED refuses to start new work.
    HotSavePhase save = HotSaveSlot_phase(hot);
    if (save == HOT_SAVE_REQUESTED)
        return HOT_OK; // worker still capturing the old generation
    if (save == HOT_SAVE_CAPTURED) {
        if (!HotSaveSlot_consume(hot))
            return HOT_OK; // consumed elsewhere; resolve on the next pass
        return perform_swap(hot, libDir, loaded_count);
    }
    if (save == HOT_SAVE_STOPPED) {
        snprintf(HotModule_lastErrorMut(hot), sizeof(((HotModule) {0}).last_error),
                 "poll after save worker stop");
        return HOT_ERROR_INIT_FAILED;
    }

    if (stamp == HotModule_generation(hot))
        return HOT_OK;

    if (HotModule_moduleCount(hot) == 0) {
        // First load — nothing to preserve, swap straight through.
        return perform_swap(hot, libDir, loaded_count);
    }

    // Live swap: preserve the CURRENT (old) generation's state off-thread,
    // swap on a later pass. Old handles stay mapped (retire ring) so the
    // worker always reads live state. begin() is the IDLE -> REQUESTED write.
    if (HotSaveSlot_begin(hot))
        return HOT_OK;
    return perform_swap(hot, libDir, loaded_count); // nothing save-capable
}

/** Invokes the named loaded module's optional shutdown export. */
fn Hot_shutdownModule(HotModule *hot, const char *module_name) {
    if (!hot || !module_name)
        return;
    HotModuleInternal *mod = HotModuleInternal_find(hot, module_name);
    any handle = mod ? HotModuleInternal_handle(mod) : nullptr;
    if (!mod || !HotModuleInternal_loaded(mod) || !handle)
        return;
    typedef fn (*ShutdownFn)();
    ShutdownFn shutdown = (ShutdownFn) dlsym(handle, "Hot_shutdown_module");
    if (shutdown)
        shutdown();
}

/** Calls the named loaded module's state-save export into the caller's buffer. */
bool Hot_saveModule(HotModule *hot, const char *module_name, any buf, bytes cap, bytes *outLen) {
    if (!hot || !module_name || !buf || !outLen)
        return false;
    HotModuleInternal *mod = HotModuleInternal_find(hot, module_name);
    any handle = mod ? HotModuleInternal_handle(mod) : nullptr;
    if (!mod || !HotModuleInternal_loaded(mod) || !handle)
        return false;
    typedef bool (*SaveFn)(any , bytes, bytes *);
    SaveFn save = (SaveFn) dlsym(handle, "Hot_save");
    if (!save)
        return false;
    return save(buf, cap, outLen);
}

/** Calls the named loaded module's state-restore export with the supplied bytes. */
bool Hot_restoreModule(HotModule *hot, const char *module_name, const_any buf, bytes len) {
    if (!hot || !module_name || !buf)
        return false;
    HotModuleInternal *mod = HotModuleInternal_find(hot, module_name);
    any handle = mod ? HotModuleInternal_handle(mod) : nullptr;
    if (!mod || !HotModuleInternal_loaded(mod) || !handle)
        return false;
    typedef bool (*RestoreFn)(const_any , bytes);
    RestoreFn restore = (RestoreFn) dlsym(handle, "Hot_restore");
    if (!restore)
        return false;
    return restore(buf, len);
}

/** Calls the named loaded module's migration export into the caller's destination. */
bool Hot_migrateModule(HotModule *hot, const char *module_name, const char *oldVersion,
                        const_any oldBuf, bytes oldLen, any newBuf, bytes newCap, bytes *outLen) {
    if (!hot || !module_name || !oldVersion || !oldBuf || !newBuf || !outLen)
        return false;
    HotModuleInternal *mod = HotModuleInternal_find(hot, module_name);
    any handle = mod ? HotModuleInternal_handle(mod) : nullptr;
    if (!mod || !HotModuleInternal_loaded(mod) || !handle)
        return false;
    typedef bool (*MigrateFn)(const char *, const_any , bytes, any , bytes, bytes *);
    MigrateFn migrate = (MigrateFn) dlsym(handle, "Hot_migrate");
    if (!migrate)
        return false;
    return migrate(oldVersion, oldBuf, oldLen, newBuf, newCap, outLen);
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Resolves a registered symbol through the current trampoline table. */
HotFn Hot_getSymbol(HotModule *hot, const char *name) {
    if (!hot || !name)
        return nullptr;

    HotTrampolineTable *table = HotModule_trampolinesMut(hot);
    i32 tidx = HotTrampolineTable_find(table, name);
    if (tidx < 0)
        return nullptr;

    return HotTrampolineTable_get(table, tidx);
}

;;GETTER
/** Returns the last successfully committed manifest generation. */
u64 Hot_getGeneration(const HotModule *hot) {
    return hot ? HotModule_generation(hot) : 0;
}

bool Hot_requiresRecovery(const HotModule *hot) {
    return hot != nullptr && HotModule_recoveryRequired(hot);
}

;;GETTER
/** Returns the loader's last diagnostic, or a fixed message for null. */
const char *Hot_getLastError(HotModule *hot) {
    if (!hot)
        return "NULL hot module";
    return HotModule_lastErrorMut(hot);
}
