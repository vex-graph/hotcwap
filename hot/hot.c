#include "hot/hot.h"
#include "hot/manifest.h"
#include "hot/hot_trampoline.h"
#include "hot/hot_retire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include "security/crypto.h"
#include "exception/throw.h"

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/intention.h"

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
 *   fail-closed (dlopen + VkModuleGetTrampolines on every section before any
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
 *   uint64_t generation;                 // last COMMITTED bin/current/<lib> generation
 *   HotModuleInternal modules[HOT_MAX_MODULES]; // per-section slots (max 32)
 *   uint32_t module_count;               // used slots in modules[]
 *   char last_error[256];                // last diagnostic string
 *   HotTrampolineTable trampolines;      // per-instance symbols
 *   HotRetireRing retireRing;            // per-instance generational close
 *   pthread_t saveThread;                // off-thread state-save worker
 *   pthread_mutex_t saveLock;            // guards saveRequested/saveDone/saveSlots
 *   pthread_cond_t saveCond;             // worker sleep/wake
 *   bool saveWorkerLive;                 // worker running
 *   bool saveCancel;                     // shutdown flag (under saveLock)
 *   bool saveRequested;                  // snapshot pending (under saveLock)
 *   bool saveDone;                       // snapshot published (under saveLock)
 *   bool swapPending;                    // snapshot kicked; swap on next poll
 *   HotSaveSlot saveSlots[HOT_MAX_MODULES]; // snapshot blobs (worker writes, poll reads)
 *   size_t saveCount;                    // populated slots
 *   const HotSectionDigest *coldDigests; // borrowed trusted cold descriptor
 *   size_t coldCount;                    // descriptor extent; zero in legacy polls
 *   bool recoveryRequired;              // unresolved disk rollback; stop admission
 *
 * PRIVATE HELPERS (kept file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   HotModuleInternal (per-section slot — HotModule's slot type, no own API):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem (no extension)
 *     char path[HOT_PATH_LEN];           // source dylib path
 *     void *handle;                      // dlopen handle (NULL = unloaded)
 *     bool loaded;                       // true once first load succeeds
 *
 *   HotSaveSlot (per-section state blob captured off-thread):
 *     char name[HOT_MANIFEST_MAX_NAME];  // section stem the blob belongs to
 *     uint8_t buf[HOT_SAVE_SLOT_CAP];    // module Hot_save payload (4 KiB)
 *     size_t len;                        // valid Bytes in buf
 *
 *   HotStagedSection (verify-phase staging, transient inside perform_swap):
 *     HotModuleInternal *mod;   // resolved slot (created if new)
 *     char path[HOT_PATH_LEN];  // dylib path
 *     void *handle;             // dlopened, not yet adopted
 *     const void *trampolines;  // VkModuleGetTrampolines table
 *     uint32_t trampolineCount; // entries
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
 *   - HotShutdown(hot)                       : Join save worker and drain modules
 *   - Hot_poll(hot, loaded_count)            : Two-phase generational reload poll
 *   - Hot_updateCold(hot, payloadDir, digests, count, loadedCount)
 *       Caller-excluded synchronous checksum/ABI/save/restore transaction;
 *       candidates discarded on failure, running generation unchanged.
 *   - Hot_shutdown_module(hot, name)         : Invoke module teardown export
 *   - Hot_save_module(hot, name, b, c, len)  : Capture module state blob
 *   - Hot_restore_module(hot, name, buf, l)  : Rehydrate state into new image
 *   - Hot_migrate_module(hot, name, ...)     : Schema migration across versions
 *
 * Private Core Functions: (.c static)
 *   - find_module(hot, name)                 : Lookup internal module slot
 *   - snapshot_begin(hot)                    : Wake save worker for off-thread capture
 *   - snapshot_modules(hot)                  : Worker body capturing state blobs
 *   - save_worker_main(arg)                  : Dedicated save worker thread entry
 *   - perform_swap(hot)                      : Stage, verify, restore, and swap dylibs
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Hot_get_symbol(hot, name)              : Lookup function pointer in trampolines
 *   - Hot_get_generation(hot)                : Last committed generation counter
 *   - Hot_requiresRecovery(hot)              : Unresolved rollback blocks cold retry
 *   - Hot_last_error(hot)                    : Last diagnostic error message
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
            "section dlopens + exports VkModuleGetTrampolines); the commit side "
            "is per-row assignment per the Conflict Triage Law.")

;;INTENTION("A library's declared section set is stable across its life (the "
            "manifest catalog is the contract; MANIFEST_UPDATE fails-closed "
            "against undeclared stems). A section that vanishes from a promoted "
            "current set keeps its old slot + handle mapped (never retired) so "
"stale trampolines never dangle — the unshared image lingers until "
             "HotShutdown. Removing a section is a library-uninstall event "
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

#define HOT_MAX_MODULES 32
#define HOT_PATH_LEN 512
#define HOT_SAVE_SLOT_CAP 4096
#define HOT_SAVE_WAIT_NS (25 * 1000 * 1000) // 25ms cond-wait slice (the Bounded Wait Law)

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    char path[HOT_PATH_LEN];
    void *handle;                    // dlopen handle
    bool loaded;                     // is currently loaded
} HotModuleInternal;

typedef struct {
    char name[HOT_MANIFEST_MAX_NAME];
    alignas(16) uint8_t buf[HOT_SAVE_SLOT_CAP];
    size_t len;
} HotSaveSlot;

typedef struct {
    HotModuleInternal *mod;          // resolved slot (created if new)
    char path[HOT_PATH_LEN];         // dylib path
    void *handle;                    // dlopened, not yet adopted
    const void *trampolines;         // VkModuleGetTrampolines table
    uint32_t trampolineCount;        // entries
} HotStagedSection;

typedef struct HotModule {
    char library[HOT_MANIFEST_MAX_NAME];
    uint64_t generation;
    HotModuleInternal modules[HOT_MAX_MODULES];
    uint32_t module_count;
    char last_error[256];
    HotTrampolineTable trampolines;  // per-instance symbols (no cross-app collision)
    HotRetireRing retireRing;        // per-instance generational close
    // Off-thread state-save worker
    pthread_t saveThread;
    pthread_mutex_t saveLock;
    pthread_cond_t saveCond;
    bool saveWorkerLive;
    bool saveCancel;                 // under saveLock
    bool saveRequested;              // under saveLock
    bool saveDone;                   // under saveLock
    bool swapPending;                // _main-thread only
    HotSaveSlot saveSlots[HOT_MAX_MODULES];
    size_t saveCount;
    const HotSectionDigest *coldDigests;
    size_t coldCount;
    bool recoveryRequired;
} HotModule;

// CONSTRUCTORS — HotModule owns the module slots below; row mechanics live
// in hot_trampoline.c, generational close in hot_retire.c.

// Find a module slot by section stem.
static HotModuleInternal *find_module(HotModule *hot, const char *name) {
    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        if (strcmp((*hot).modules[i].name, name) == 0)
            return &(*hot).modules[i];
    }
    return nullptr;
}

// Off-thread state-save worker (HOT_SAVE_WAIT_NS cond-wait cycle, cancel-aware).
static void *save_worker_main(void *arg);

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

    strncpy((*hot).library, library, HOT_MANIFEST_MAX_NAME - 1);
    (*hot).library[HOT_MANIFEST_MAX_NAME - 1] = '\0';

    pthread_mutex_init(&(*hot).saveLock, nullptr);
    pthread_cond_init(&(*hot).saveCond, nullptr);
    (*hot).saveWorkerLive = (pthread_create(&(*hot).saveThread, nullptr,
                                            save_worker_main, hot) == 0);
    if (!(*hot).saveWorkerLive) {
        pthread_mutex_destroy(&(*hot).saveLock);
        pthread_cond_destroy(&(*hot).saveCond);
        free(hot);
        return nullptr;
    }
    return hot;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)

// Stop the save worker FIRST (bounded join — its cond wait is capped at
// HOT_SAVE_WAIT_NS), so no thread touches module handles after shutdown
// begins. Then close modules + drain the retire ring, per the Teardown
// Order Law (worker resources die before the handles they read).
static void save_worker_shutdown(HotModule *hot) {
    if (!(*hot).saveWorkerLive)
        return;
    pthread_mutex_lock(&(*hot).saveLock);
    (*hot).saveCancel = true;
    (*hot).saveRequested = false;
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    pthread_join((*hot).saveThread, nullptr);
    (*hot).saveWorkerLive = false;
}

/** Stops the state-save worker, closes loaded modules, drains retired handles, and frees. */
void HotShutdown(HotModule *hot) {
    if (!hot)
        return;

    save_worker_shutdown(hot);
    pthread_mutex_destroy(&(*hot).saveLock);
    pthread_cond_destroy(&(*hot).saveCond);

    for (uint32_t i = 0; i < (*hot).module_count; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if ((*mod).handle) {
            typedef void (*ShutdownFn)(void);
            ShutdownFn shutdown = (ShutdownFn) dlsym((*mod).handle, "Hot_shutdown_module");
            if (shutdown)
                shutdown();
            dlclose((*mod).handle);
            (*mod).handle = nullptr;
            (*mod).loaded = false;
        }
    }

    // Drain retired handle ring
    HotRetireRing_drainAll(&(*hot).retireRing);

    HotTrampolineTable_free(&(*hot).trampolines);

    free(hot);
}

// Runs ON THE SAVE WORKER thread. Copies each loaded module's state blob via
// its Hot_save export into saveSlots, then publishes saveDone. The module
// images being snapshotted are the CURRENT (old-generation) handles — they
// stay mapped in the retire ring until the swap commits, so the worker always
// reads live state.
static void snapshot_modules(HotModule *hot) {
    (*hot).saveCount = 0;
    for (uint32_t i = 0; i < (*hot).module_count && (*hot).saveCount < HOT_MAX_MODULES; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if (!(*mod).loaded || !(*mod).handle)
            continue;
        typedef bool (*SaveFn)(void *, size_t, size_t *);
        SaveFn save = (SaveFn) dlsym((*mod).handle, "Hot_save");
        if (!save)
            continue;
        HotSaveSlot *slot = &(*hot).saveSlots[(*hot).saveCount];
        size_t n = 0;
        if (save((*slot).buf, sizeof((*slot).buf), &n) && n > 0 && n <= sizeof((*slot).buf)) {
            strncpy((*slot).name, (*mod).name, HOT_MANIFEST_MAX_NAME - 1);
            (*slot).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            (*slot).len = n;
            (*hot).saveCount++;
        }
    }
}

// The worker loop: sleeps on saveCond (capped at HOT_SAVE_WAIT_NS so every
// path reaches the exit check bounded — the Bounded Wait Law), snapshots on
// request, exits on cancel.
static void *save_worker_main(void *arg) {
    HotModule *hot = (HotModule*) arg;
    pthread_mutex_lock(&(*hot).saveLock);
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += HOT_SAVE_WAIT_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&(*hot).saveCond, &(*hot).saveLock, &deadline);
        if ((*hot).saveCancel)
            break;
        if (rc == 0 || rc == ETIMEDOUT) {
            if ((*hot).saveRequested) {
                (*hot).saveRequested = false;
                snapshot_modules(hot);
                (*hot).saveDone = true;
            }
        }
    }
    pthread_mutex_unlock(&(*hot).saveLock);
    return nullptr;
}

// Ask the worker to snapshot the CURRENT generation's state. Returns true
// when any loaded module exports Hot_save (a snapshot was requested); false
// when none do (the caller swaps directly — nothing to preserve).
static bool snapshot_begin(HotModule *hot) {
    bool any = false;
    for (uint32_t i = 0; i < (*hot).module_count && !any; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if ((*mod).loaded && (*mod).handle && dlsym((*mod).handle, "Hot_save") != nullptr)
            any = true;
    }
    if (!any)
        return false;
    pthread_mutex_lock(&(*hot).saveLock);
    (*hot).saveRequested = true;
    (*hot).saveDone = false;
    (*hot).saveCount = 0;
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    return true;
}

// Load a whole manifest library generation and commit it. Phase A verifies
// EVERY section (dlopen + VkModuleGetTrampolines present) before anything is
// touched — on any failure every staged handle is closed and the previous
// generation stays live, untouched. Phase B rehydrates the saved blobs into
// the STAGED images BEFORE any commit; a rejecting Hot_restore rolls the
// whole swap back (#8.5 Automated State Rollback). Phase C commits: retire
// old handles, adopt new, swap trampoline rows. Generation only advances on
// full success, so a failed swap re-enters the handshake on the next poll
// and self-heals once the payload is fixed.
static uint32_t s_loadSerial = 0;

static bool hot_mkdir_p(const char *path) {
    char tmp[HOT_PATH_LEN];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp))
        return false;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        tmp[i] = '/';
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

static bool hot_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    unsigned char buf[8192];
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(out);
    fclose(in);
    return ok;
}

// Hash the private load copy, not a previously checked pathname. Stable trusted
// parents still required; this is not protection against same-user mutation.
static bool digest_matches(const char *path, const uint8_t expected[32]) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
        return false;
    CryptoSha256 hash;
    Crypto_sha256Init(&hash);
    uint8_t buffer[4096], actual[32];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
        Crypto_sha256Update(&hash, buffer, n);
    bool ok = !ferror(file);
    if (fclose(file) != 0)
        ok = false;
    Crypto_sha256Final(&hash, actual);
    return ok && memcmp(actual, expected, sizeof(actual)) == 0;
}

static const HotSectionDigest *cold_digest(HotModule *hot, const char *name) {
    for (size_t i = 0; i < (*hot).coldCount; ++i) {
        const HotSectionDigest *digest = &(*hot).coldDigests[i];
        if ((*digest).filename != nullptr && strcmp((*digest).filename, name) == 0)
            return digest;
    }
    return nullptr;
}

/** Stages and validates every section, restores saved state, then commits the generation. */
static HotResult perform_verified_swap(HotModule *hot, const char *libDir, uint32_t *outLoaded) {
    HotTrampolineTable *table = &(*hot).trampolines;
    HotRetireRing *ring = &(*hot).retireRing;
    uint32_t loaded = 0;

    DIR *dir = opendir(libDir);
    if (!dir) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "Cannot open manifest library dir: %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    // Phase A — verify (fail-closed: all-or-nothing).
    HotStagedSection staged[HOT_MAX_MODULES];
    uint32_t stagedCount = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        // readdir storage dies at closedir; diagnostics on rejection must own
        // their filename before closing the iterator.
        char name[HOT_PATH_LEN];
        snprintf(name, sizeof(name), "%s", (*ent).d_name);
        size_t nlen = strlen(name);
        bool is_dylib = (nlen > 6 && strcmp(name + nlen - 6, ".dylib") == 0) ||
                        (nlen > 3 && strcmp(name + nlen - 3, ".so") == 0);
        if (!is_dylib)
            continue;
        if (!MANIFEST_ALLOWS((*hot).library, name)) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; ++i)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error), "undeclared section: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }
        if (stagedCount >= HOT_MAX_MODULES) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Too many sections (limit %d) in %s", HOT_MAX_MODULES, libDir);
            return HOT_ERROR_OUT_OF_MEMORY;
        }

        char path[HOT_PATH_LEN];
        int plen = snprintf(path, sizeof(path), "%s/%s", libDir, name);
        if (plen < 0 || (size_t) plen >= sizeof(path)) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Section path '%s/%s' truncated", libDir, name);
            return HOT_ERROR_FILE_NOT_FOUND;
        }

        // dyld caches a loaded image by path, so a replaced file at the SAME
        // path is NOT re-read — the swap would silently keep the old code.
        // Load from a per-swap unique copy (Generation-Unique Load Path) so
        // every generation is a genuinely fresh image.
        char loadDir[HOT_PATH_LEN];
        char loadPath[HOT_PATH_LEN];
        if ((*hot).coldCount) {
            snprintf(loadDir, sizeof(loadDir), "%s", libDir);
            snprintf(loadPath, sizeof(loadPath), "%s", path);
        } else {
            snprintf(loadDir, sizeof(loadDir), "%s/cache/load/%s/%u",
                     MANIFEST_ROOT(), (*hot).library, ++s_loadSerial);
            snprintf(loadPath, sizeof(loadPath), "%s/%s", loadDir, name);
        }
        if (!(*hot).coldCount && (!hot_mkdir_p(loadDir) || !hot_copy_file(path, loadPath))) {
            unlink(loadPath);
            rmdir(loadDir);
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "cannot stage a unique load copy for %s", name);
            return HOT_ERROR_FILE_NOT_FOUND;
        }

        const HotSectionDigest *digest = cold_digest(hot, name);
        if ((*hot).coldCount && (digest == nullptr || !digest_matches(loadPath, (*digest).sha256))) {
            unlink(loadPath);
            rmdir(loadDir);
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; ++i)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error), "checksum mismatch: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }
        void *handle = dlopen(loadPath, RTLD_NOW | RTLD_LOCAL);
        // dlopen retains the mapped image; this unique private copy is transport
        // only, not a resource root. Modules use manifest/host resource paths.
        // Reclaim it on success and rejection rather than accumulating releases.
        unlink(loadPath);
        rmdir(loadDir);
        if (!handle) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "dlopen(%s) failed: %s", loadPath, dlerror());
            return HOT_ERROR_DLOPEN_FAILED;
        }

        typedef const void *(*TrampolinesFn)(uint32_t*);
        const uint64_t *abi = (*hot).coldCount ? dlsym(handle, "Hot_cold_abi") : nullptr;
        if ((*hot).coldCount && (abi == nullptr || *abi != HOT_COLD_ABI_VERSION)) {
            closedir(dir);
            dlclose(handle);
            for (uint32_t i = 0; i < stagedCount; ++i)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error), "cold ABI mismatch: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }
        TrampolinesFn get_trampolines = (TrampolinesFn) dlsym(handle, "VkModuleGetTrampolines");
        if (!get_trampolines) {
            closedir(dir);
            dlclose(handle);
            for (uint32_t i = 0; i < stagedCount; i++)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "dlsym(VkModuleGetTrampolines) on %s failed: %s", path, dlerror());
            return HOT_ERROR_DLSYM_FAILED;
        }

        // Section stem = file name without extension, plus the manifest
        // catalog's stem rule ("libbar.so" → "bar") so loader slots and
        // declared sections agree on the same identity.
        char mod_name[HOT_MANIFEST_MAX_NAME];
        strncpy(mod_name, name, HOT_MANIFEST_MAX_NAME - 1);
        mod_name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
        char *dot = strrchr(mod_name, '.');
        if (dot)
            (*dot) = '\0';
        if (strcmp(name + nlen - 3, ".so") == 0 && nlen > 6 &&
            strncmp(mod_name, "lib", 3) == 0)
            memmove(mod_name, mod_name + 3, strlen(mod_name + 3) + 1);

        HotModuleInternal *mod = find_module(hot, mod_name);
        if (!mod) {
            if ((*hot).module_count >= HOT_MAX_MODULES) {
                closedir(dir);
                dlclose(handle);
                for (uint32_t i = 0; i < stagedCount; i++)
                    dlclose(staged[i].handle);
                snprintf((*hot).last_error, sizeof((*hot).last_error),
                         "Too many modules (limit %d reached)", HOT_MAX_MODULES);
                return HOT_ERROR_OUT_OF_MEMORY;
            }
            mod = &(*hot).modules[(*hot).module_count++];
            strncpy((*mod).name, mod_name, HOT_MANIFEST_MAX_NAME - 1);
            (*mod).name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            (*mod).handle = nullptr;
            (*mod).loaded = false;
        }

        HotStagedSection *s = &staged[stagedCount++];
        (*s).mod = mod;
        strncpy((*s).path, path, HOT_PATH_LEN - 1);
        (*s).path[HOT_PATH_LEN - 1] = '\0';
        (*s).handle = handle;
        (*s).trampolines = get_trampolines(&(*s).trampolineCount);
        const struct { const char *name; void *fn; } *exports = (*s).trampolines;
        bool valid = exports != nullptr && (*s).trampolineCount > 0;
        for (uint32_t i = 0; valid && i + 1 < stagedCount; ++i)
            valid = staged[i].mod != mod;
        for (uint32_t k = 0; valid && k < (*s).trampolineCount; ++k)
            valid = exports[k].name != nullptr && exports[k].fn != nullptr &&
                    exports[k].name[0] != '\0' &&
                    strnlen(exports[k].name, HOT_MANIFEST_MAX_NAME) < HOT_MANIFEST_MAX_NAME;
        if (!valid) {
            closedir(dir);
            for (uint32_t i = 0; i < stagedCount; ++i)
                dlclose(staged[i].handle);
            snprintf((*hot).last_error, sizeof((*hot).last_error), "invalid export table: %s", name);
            return HOT_ERROR_ABI_MISMATCH;
        }
    }
    closedir(dir);

    if ((*hot).coldCount) {
        // Duplicate public identities make dispatch order-dependent; reject the
        // complete candidate before any restore or target publication.
        for (uint32_t i = 0; i < stagedCount; ++i) {
            const struct { const char *name; void *fn; } *left = staged[i].trampolines;
            for (uint32_t k = 0; k < staged[i].trampolineCount; ++k) {
                for (uint32_t j = 0; j <= i; ++j) {
                    const struct { const char *name; void *fn; } *right = staged[j].trampolines;
                    uint32_t limit = j == i ? k : staged[j].trampolineCount;
                    for (uint32_t r = 0; r < limit; ++r) {
                        if (strcmp(left[k].name, right[r].name) == 0) {
                            for (uint32_t m = 0; m < stagedCount; ++m)
                                dlclose(staged[m].handle);
                            return HOT_ERROR_ABI_MISMATCH;
                        }
                    }
                }
            }
        }
    }

    if (stagedCount == 0) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "No loadable sections in %s", libDir);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    uint64_t newGen = MANIFEST_GENERATION((*hot).library);

    // Cold updates cannot strand an old export in a handle we will unload.
    // Removal is an ABI change requiring a resident host restart.
    if ((*hot).coldCount) {
        uint32_t oldCount = atomic_load(&(*table).count);
        for (uint32_t r = 0; r < oldCount; ++r) {
            HotTrampoline *row = &(*table).rows[r];
            bool present = false;
            for (uint32_t i = 0; i < stagedCount; ++i) {
                const struct { const char *name; void *fn; } *exports = staged[i].trampolines;
                for (uint32_t k = 0; k < staged[i].trampolineCount; ++k) {
                    if (strcmp((*row).name, exports[k].name) == 0)
                        present = true;
                }
            }
            if (!present) {
                for (uint32_t i = 0; i < stagedCount; ++i)
                    dlclose(staged[i].handle);
                snprintf((*hot).last_error, sizeof((*hot).last_error), "cold update removed export: %s", (*row).name);
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
    for (size_t i = 0; i < (*hot).saveCount; i++) {
        HotSaveSlot *slot = &(*hot).saveSlots[i];
        HotStagedSection *match = nullptr;
        for (uint32_t k = 0; k < stagedCount; k++) {
            if (strcmp((*staged[k].mod).name, (*slot).name) == 0)
                match = &staged[k];
        }
        if (!match)
            continue; // section absent from the new set; its old slot stays mapped
        typedef bool (*RestoreFn)(const void *, size_t);
        RestoreFn restore = (RestoreFn) dlsym((*match).handle, "Hot_restore");
        bool restored = (restore && restore((*slot).buf, (*slot).len));
        if (!restored && restore) {
            typedef bool (*MigrateFn)(const char *, const void *, size_t, void *, size_t, size_t *);
            MigrateFn migrate = (MigrateFn) dlsym((*match).handle, "Hot_migrate");
            if (migrate) {
                alignas(16) uint8_t migratedBuf[HOT_SAVE_SLOT_CAP];
                size_t migratedLen = 0;
                char oldGenStr[32];
                snprintf(oldGenStr, sizeof(oldGenStr), "%llu.0.0", (unsigned long long) (*hot).generation);
                if ((migrate(oldGenStr, (*slot).buf, (*slot).len, migratedBuf, sizeof(migratedBuf), &migratedLen) ||
                     (!(*hot).coldCount && migrate("1.0.0", (*slot).buf, (*slot).len, migratedBuf, sizeof(migratedBuf), &migratedLen))) &&
                    migratedLen > 0 && migratedLen <= sizeof(migratedBuf)) {
                    restored = restore(migratedBuf, migratedLen);
                }
            }
        }
        if (!restored) {
            for (uint32_t k = 0; k < stagedCount; k++)
                dlclose(staged[k].handle); // never adopted — safe to close directly
            (*hot).saveCount = 0;
            snprintf((*hot).last_error, sizeof((*hot).last_error),
                     "Restore rejected by %s (%zu Bytes) — generation %llu stays live",
                     (*slot).name, (*slot).len, (unsigned long long) (*hot).generation);
            fprintf(stderr, "[hot] ROLLBACK: %s rejected restored state (%zu Bytes) — "
                            "keeping generation %llu live, generation NOT advanced\n",
                    (*slot).name, (*slot).len, (unsigned long long) (*hot).generation);
            return HOT_ERROR_RESTORE_FAILED;
        }
    }

    // Reserve every candidate symbol before any target changes. The caller must
    // exclude table readers during cold backing growth; published targets alone
    // do not make relocation safe. Repeated updates reuse existing names.
    uint64_t required = atomic_load(&(*table).count);
    for (uint32_t i = 0; i < stagedCount; ++i)
        required += staged[i].trampolineCount;
    if (required > INT_MAX || !HotTrampolineTable_reserve(table, (uint32_t) required)) {
        for (uint32_t i = 0; i < stagedCount; ++i)
            dlclose(staged[i].handle);
        (*hot).saveCount = 0;
        return HOT_ERROR_OUT_OF_MEMORY;
    }

    // Phase C — commit: retire old handles, adopt new, swap trampoline rows.
    // State already lives inside the fresh images (Phase B restored in place).
    for (uint32_t i = 0; i < stagedCount; i++) {
        HotStagedSection *s = &staged[i];
        HotModuleInternal *mod = (*s).mod;
        if ((*mod).handle)
            HotRetireRing_retire(ring, (*mod).handle);
        (*mod).handle = (*s).handle;
        (*mod).loaded = true;
        strncpy((*mod).path, (*s).path, HOT_PATH_LEN - 1);
        (*mod).path[HOT_PATH_LEN - 1] = '\0';

        const struct { const char *name; void *fn; } *entries = (*s).trampolines;
        for (uint32_t k = 0; k < (*s).trampolineCount; k++) {
            int tidx = HotTrampolineTable_find(table, entries[k].name);
            if (tidx < 0)
                tidx = HotTrampolineTable_register(table, entries[k].name);
            if (tidx >= 0)
                HotTrampolineTable_set(table, tidx, entries[k].fn);
        }
        loaded++;
        fprintf(stderr, "[hot] reloaded %s (generation %llu)\n",
                (*mod).name, (unsigned long long) newGen);
    }
    (*hot).saveCount = 0;

    (*hot).generation = newGen;

    if (outLoaded)
        (*outLoaded) = loaded;
    return HOT_OK;
}

// Cold release: copy and hash ALL private images before any dlopen constructor
// executes. Stable trusted parents and external serialization remain mandatory.
static HotResult perform_swap(HotModule *hot, const char *libDir, uint32_t *outLoaded) {
    if (!(*hot).coldCount)
        return perform_verified_swap(hot, libDir, outLoaded);
    char privateDir[HOT_PATH_LEN];
    int n = snprintf(privateDir, sizeof(privateDir), "%s/cache/load/%s/release-%u",
                     MANIFEST_ROOT(), (*hot).library, ++s_loadSerial);
    if (n < 0 || (size_t) n >= sizeof(privateDir) || !hot_mkdir_p(privateDir))
        return HOT_ERROR_FILE_NOT_FOUND;
    HotResult result = HOT_ERROR_ABI_MISMATCH;
    size_t copied = 0;
    for (size_t i = 0; i < (*hot).coldCount; ++i) {
        const HotSectionDigest *item = &(*hot).coldDigests[i];
        char source[HOT_PATH_LEN], dest[HOT_PATH_LEN];
        n = snprintf(source, sizeof(source), "%s/%s", libDir, (*item).filename);
        int d = snprintf(dest, sizeof(dest), "%s/%s", privateDir, (*item).filename);
        if (n < 0 || (size_t) n >= sizeof(source) || d < 0 || (size_t) d >= sizeof(dest))
            break;
        copied = i + 1; // cleanup also owns any partial failed copy
        if (!hot_copy_file(source, dest) || !digest_matches(dest, (*item).sha256))
            break;
        if (i + 1 == (*hot).coldCount)
            result = perform_verified_swap(hot, privateDir, outLoaded);
    }
    for (size_t i = 0; i < copied; ++i) {
        const HotSectionDigest *item = &(*hot).coldDigests[i];
        char dest[HOT_PATH_LEN];
        n = snprintf(dest, sizeof(dest), "%s/%s", privateDir, (*item).filename);
        if (n >= 0 && (size_t) n < sizeof(dest))
            unlink(dest);
    }
    rmdir(privateDir);
    return result;
}

HotResult Hot_updateCold(HotModule *hot, const char *payloadDir,
                         const HotSectionDigest *digests, size_t count, uint32_t *loadedCount) {
    if (loadedCount != nullptr)
        *loadedCount = 0;
    if (hot != nullptr && (*hot).recoveryRequired) {
        THROW("cold update: unresolved disk rollback requires host recovery");
        return HOT_ERROR_RECOVERY_REQUIRED;
    }
    if (hot == nullptr || payloadDir == nullptr || digests == nullptr || count == 0 ||
        count > HOT_MAX_MODULES || count != MANIFEST_SECTION_COUNT((*hot).library) || (*hot).swapPending) {
        THROW("cold update: invalid admission or legacy snapshot pending");
        return HOT_ERROR_INIT_FAILED;
    }
    HotResult result = HOT_ERROR_ABI_MISMATCH;
    uint32_t originalModuleCount = (*hot).module_count;
    DIR *dir = opendir(payloadDir);
    if (dir == nullptr) {
        THROW("cold update: cannot open payload");
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    (*hot).coldDigests = digests;
    (*hot).coldCount = count;
    size_t found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char *name = (*entry).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        const HotSectionDigest *digest = cold_digest(hot, name);
        char path[HOT_PATH_LEN];
        int n = snprintf(path, sizeof(path), "%s/%s", payloadDir, name);
        struct stat info;
        size_t len = strlen(name);
        bool dylib = (len > 6 && strcmp(name + len - 6, ".dylib") == 0) ||
                     (len > 3 && strcmp(name + len - 3, ".so") == 0);
        if (digest == nullptr || !dylib || !MANIFEST_ALLOWS((*hot).library, name) ||
            n < 0 || (size_t) n >= sizeof(path) || lstat(path, &info) != 0 ||
            !S_ISREG(info.st_mode) || !digest_matches(path, (*digest).sha256))
            break;
        ++found;
    }
    bool complete = entry == nullptr && found == count;
    closedir(dir);
    // Duplicate descriptor names would otherwise disguise an incomplete set.
    for (size_t i = 0; complete && i < count; ++i) {
        const HotSectionDigest *item = &digests[i];
        if ((*item).filename == nullptr || !MANIFEST_ALLOWS((*hot).library, (*item).filename)) {
            complete = false;
            break;
        }
        for (size_t k = 0; k < i; ++k) {
            const HotSectionDigest *prior = &digests[k];
            if (strcmp((*prior).filename, (*item).filename) == 0)
                complete = false;
        }
    }
    // The first cold release must retain every previously loaded section.
    for (uint32_t i = 0; complete && i < (*hot).module_count; ++i) {
        HotModuleInternal *module = &(*hot).modules[i];
        if (!(*module).loaded)
            continue;
        const char *base = strrchr((*module).path, '/');
        if (base == nullptr || cold_digest(hot, base + 1) == nullptr)
            complete = false;
    }
    if (!complete)
        goto resolve;
    (*hot).saveCount = 0;
    // Synchronous under caller exclusion: no worker reads concurrently with use.
    for (uint32_t i = 0; i < (*hot).module_count; ++i) {
        HotModuleInternal *module = &(*hot).modules[i];
        if (!(*module).loaded)
            continue;
        typedef bool (*SaveFn)(void *, size_t, size_t *);
        SaveFn save = (SaveFn) dlsym((*module).handle, "Hot_save");
        if (save == nullptr)
            continue; // stateless module, or all state belongs to resident host
        HotSaveSlot *slot = &(*hot).saveSlots[(*hot).saveCount];
        size_t length = 0;
        if (!save((*slot).buf, sizeof((*slot).buf), &length) || length == 0 || length > sizeof((*slot).buf)) {
            result = HOT_ERROR_RESTORE_FAILED;
            goto resolve;
        }
        strcpy((*slot).name, (*module).name);
        (*slot).len = length;
        ++(*hot).saveCount;
    }
    if (!MANIFEST_UPDATE((*hot).library, payloadDir)) {
        result = HOT_ERROR_FILE_NOT_FOUND;
        goto resolve;
    }
    uint64_t oldGeneration = MANIFEST_GENERATION((*hot).library);
    if (!MANIFEST_PROMOTE_LIBRARY((*hot).library)) {
        (*hot).recoveryRequired = MANIFEST_REQUIRES_RECOVERY();
        result = (*hot).recoveryRequired ? HOT_ERROR_RECOVERY_REQUIRED : HOT_ERROR_INIT_FAILED;
        goto resolve;
    }
    char current[HOT_PATH_LEN];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, (*hot).library, current, sizeof(current), false))
        result = HOT_ERROR_FILE_NOT_FOUND;
    else
        result = perform_swap(hot, current, loadedCount);
    if (result != HOT_OK && !MANIFEST_REVERT_LIBRARY((*hot).library, oldGeneration)) {
        snprintf((*hot).last_error, sizeof((*hot).last_error), "cold update disk rollback failed; recovery required");
        (*hot).recoveryRequired = true;
        result = HOT_ERROR_RECOVERY_REQUIRED;
    }
    if (result == HOT_OK) {
        // ALL callers excluded: old code has no active borrower at this boundary.
        HotRetireRing_drainAll(&(*hot).retireRing);
        // Cold publication needs no old-address fallback; those images are gone.
        HotTrampolineTable *table = &(*hot).trampolines;
        uint32_t rows = atomic_load(&(*table).count);
        for (uint32_t i = 0; i < rows; ++i) {
            HotTrampoline *row = &(*table).rows[i];
            atomic_store(&(*row).fallback_ptr, nullptr);
        }
    }
resolve:
    (*hot).coldDigests = nullptr;
    (*hot).coldCount = 0;
    (*hot).saveCount = 0;
    if (result != HOT_OK)
        (*hot).module_count = originalModuleCount;
    if (result != HOT_OK)
        THROW("cold update rejected: result %d; running generation %llu retained", result, (unsigned long long) (*hot).generation);
    return result;
}

/** Advances retirement and handles initial load, pending snapshot, or generation reload. */
HotResult Hot_poll(HotModule *hot, uint32_t *loaded_count) {
    if (loaded_count)
        (*loaded_count) = 0;   // always define the out-count, even on rejection
    if (!hot)
        return HOT_ERROR_FILE_NOT_FOUND;

    HotRetireRing_advance(&(*hot).retireRing);

    char libDir[HOT_PATH_LEN];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, (*hot).library,
                                 libDir, sizeof(libDir), false)) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "manifest not mounted or unknown library '%s'", (*hot).library);
        return HOT_ERROR_FILE_NOT_FOUND;
    }
    uint64_t stamp = MANIFEST_GENERATION((*hot).library);

    // Complete an in-flight live swap once the off-thread snapshot landed.
    if ((*hot).swapPending) {
        pthread_mutex_lock(&(*hot).saveLock);
        bool done = (*hot).saveDone;
        pthread_mutex_unlock(&(*hot).saveLock);
        if (done) {
            (*hot).swapPending = false;
            return perform_swap(hot, libDir, loaded_count);
        }
        return HOT_OK; // worker still snapshooting the old generation
    }

    if (stamp == (*hot).generation)
        return HOT_OK;

    if ((*hot).module_count == 0) {
        // First load — nothing to preserve, swap straight through.
        return perform_swap(hot, libDir, loaded_count);
    }

    // Live swap: preserve the CURRENT (old) generation's state off-thread,
    // swap on a later pass. Old handles stay mapped (retire ring) so the
    // worker always reads live state.
    if (snapshot_begin(hot)) {
        (*hot).swapPending = true;
        return HOT_OK;
    }
    return perform_swap(hot, libDir, loaded_count); // nothing save-capable
}

/** Invokes the named loaded module's optional shutdown export. */
void Hot_shutdown_module(HotModule *hot, const char *module_name) {
    if (!hot || !module_name)
        return;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return;
    typedef void (*ShutdownFn)(void);
    ShutdownFn shutdown = (ShutdownFn) dlsym((*mod).handle, "Hot_shutdown_module");
    if (shutdown)
        shutdown();
}

/** Calls the named loaded module's state-save export into the caller's buffer. */
bool Hot_save_module(HotModule *hot, const char *module_name, void *buf, size_t cap, size_t *outLen) {
    if (!hot || !module_name || !buf || !outLen)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*SaveFn)(void *, size_t, size_t *);
    SaveFn save = (SaveFn) dlsym((*mod).handle, "Hot_save");
    if (!save)
        return false;
    return save(buf, cap, outLen);
}

/** Calls the named loaded module's state-restore export with the supplied bytes. */
bool Hot_restore_module(HotModule *hot, const char *module_name, const void *buf, size_t len) {
    if (!hot || !module_name || !buf)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*RestoreFn)(const void *, size_t);
    RestoreFn restore = (RestoreFn) dlsym((*mod).handle, "Hot_restore");
    if (!restore)
        return false;
    return restore(buf, len);
}

/** Calls the named loaded module's migration export into the caller's destination. */
bool Hot_migrate_module(HotModule *hot, const char *module_name, const char *oldVersion,
                        const void *oldBuf, size_t oldLen, void *newBuf, size_t newCap, size_t *outLen) {
    if (!hot || !module_name || !oldVersion || !oldBuf || !newBuf || !outLen)
        return false;
    HotModuleInternal *mod = find_module(hot, module_name);
    if (!mod || !(*mod).loaded || !(*mod).handle)
        return false;
    typedef bool (*MigrateFn)(const char *, const void *, size_t, void *, size_t, size_t *);
    MigrateFn migrate = (MigrateFn) dlsym((*mod).handle, "Hot_migrate");
    if (!migrate)
        return false;
    return migrate(oldVersion, oldBuf, oldLen, newBuf, newCap, outLen);
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Resolves a registered symbol through the current trampoline table. */
HotFn Hot_get_symbol(HotModule *hot, const char *name) {
    if (!hot || !name)
        return nullptr;

    HotTrampolineTable *table = &(*hot).trampolines;
    int tidx = HotTrampolineTable_find(table, name);
    if (tidx < 0)
        return nullptr;

    return (HotFn) HotTrampolineTable_get(table, tidx);
}

;;GETTER
/** Returns the last successfully committed manifest generation. */
uint64_t Hot_get_generation(const HotModule *hot) {
    return hot ? (*hot).generation : 0;
}

bool Hot_requiresRecovery(const HotModule *hot) {
    return hot != nullptr && (*hot).recoveryRequired;
}

;;GETTER
/** Returns the loader's last diagnostic, or a fixed message for null. */
const char *Hot_last_error(HotModule *hot) {
    if (!hot)
        return "NULL hot module";
    return (*hot).last_error;
}
