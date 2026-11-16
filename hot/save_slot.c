#include "vex.h"
#include "hot/save_slot.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hot/internal.h"

// hot/save_slot.c — off-thread state capture.
//
// The frame thread must never pay the serialization cost, so Hot_save() runs
// on this one supervised worker. The two details that make it safe:
//
//   * the images it reads are the CURRENT (old-generation) handles, and those
//     stay mapped in the retire ring until the swap commits — so the worker is
//     never reading code that has already been closed;
//   * every wait is capped at HOT_SAVE_WAIT_NS, so cancel and shutdown are
//     bounded (the Bounded Wait Law).
//
// The handshake is one word (HotSavePhase), and this file writes every
// transition: begin IDLE -> REQUESTED, the worker REQUESTED -> CAPTURED,
// consume CAPTURED -> IDLE, shutdown anything -> STOPPED.

;;GETTER
static inline HotSavePhase HotSaveSlot_phaseLoad(const HotModule *hot, memory_order order) {
    return (HotSavePhase) atomic_load_explicit(&(*hot).savePhase, order);
}
;;GETTER
static inline fn HotSaveSlot_storePhase(HotModule *hot, HotSavePhase phase, memory_order order) {
    atomic_store_explicit(&(*hot).savePhase, phase, order);
}
;;GETTER
static inline bool HotSaveSlot_casPhase(HotModule *hot, u32 *expected, HotSavePhase desired) {
    return atomic_compare_exchange_strong_explicit(&(*hot).savePhase, expected, desired,
                                                  memory_order_acq_rel, memory_order_relaxed);
}
;;GETTER
static inline u32 HotModule_saveCount(const HotModule *hot) { return (*hot).saveCount; }
;;SETTER
static inline fn HotModule_setSaveCount(HotModule *hot, u32 count) { (*hot).saveCount = count; }
;;GETTER
static inline u32 HotModule_moduleCount(const HotModule *hot) { return (*hot).module_count; }
;;GETTER
static inline HotModuleInternal *HotModule_moduleAtMut(HotModule *hot, u32 i) { return &(*hot).modules[i]; }
;;GETTER
static inline HotSaveSlot *HotModule_saveSlotAtMut(HotModule *hot, u32 i) { return &(*hot).saveSlots[i]; }
;;GETTER
static inline pthread_mutex_t *HotModule_saveLockMut(HotModule *hot) { return &(*hot).saveLock; }
;;GETTER
static inline pthread_cond_t *HotModule_saveCondMut(HotModule *hot) { return &(*hot).saveCond; }
;;GETTER
static inline pthread_t HotModule_saveThread(const HotModule *hot) { return (*hot).saveThread; }
;;GETTER
static inline bool HotModuleInternal_loaded(const HotModuleInternal *mod) { return (*mod).loaded; }
;;GETTER
static inline any HotModuleInternal_handle(const HotModuleInternal *mod) { return (*mod).handle; }
;;GETTER
static inline const char *HotModuleInternal_name(const HotModuleInternal *mod) { return (*mod).name; }
;;GETTER
static inline u8 *HotSaveSlot_bufMut(HotSaveSlot *slot) { return (*slot).buf; }
;;GETTER
static inline char *HotSaveSlot_nameMut(HotSaveSlot *slot) { return (*slot).name; }
;;SETTER
static inline fn HotSaveSlot_setLen(HotSaveSlot *slot, bytes len) { (*slot).len = len; }

// The one handshake word. Every WRITE happens under saveLock; this read does
// not need it, because the phase is atomic and a torn value is impossible.
HotSavePhase HotSaveSlot_phase(const HotModule *hot) {
    return HotSaveSlot_phaseLoad(hot, memory_order_acquire);
}

// Runs ON THE SAVE WORKER thread, under saveLock. Copies each loaded module's
// state blob via its Hot_save export into saveSlots.
static fn snapshot_modules(HotModule *hot) {
    HotModule_setSaveCount(hot, 0);
    u32 modCount = HotModule_moduleCount(hot);
    for (u32 i = 0; i < modCount && HotModule_saveCount(hot) < HOT_MAX_MODULES; i++) {
        HotModuleInternal *mod = HotModule_moduleAtMut(hot, i);
        any handle = HotModuleInternal_handle(mod);
        if (!HotModuleInternal_loaded(mod) || !handle)
            continue;
        typedef bool (*SaveFn)(any , bytes, bytes *);
        SaveFn save = (SaveFn) dlsym(handle, "Hot_save");
        if (!save)
            continue;
        u32 scount = HotModule_saveCount(hot);
        HotSaveSlot *slot = HotModule_saveSlotAtMut(hot, scount);
        bytes n = 0;
        if (save(HotSaveSlot_bufMut(slot), sizeof(((HotSaveSlot) {0}).buf), &n) &&
            n > 0 && n <= sizeof(((HotSaveSlot) {0}).buf)) {
            char *sname = HotSaveSlot_nameMut(slot);
            strncpy(sname, HotModuleInternal_name(mod), HOT_MANIFEST_MAX_NAME - 1);
            sname[HOT_MANIFEST_MAX_NAME - 1] = '\0';
            HotSaveSlot_setLen(slot, n);
            HotModule_setSaveCount(hot, scount + 1);
        }
    }
}

// The worker loop: sleeps on saveCond (capped so every path reaches the exit
// check bounded), serves one REQUESTED handshake, exits on STOPPED. Both
// transitions are made here, under saveLock, so no other file can invent one.
any HotSaveSlot_workerMain(any arg) {
    HotModule *hot = (HotModule*) arg;
    pthread_mutex_t *lock = HotModule_saveLockMut(hot);
    pthread_cond_t *cond = HotModule_saveCondMut(hot);
    pthread_mutex_lock(lock);
    for (;;) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += HOT_SAVE_WAIT_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        i32 rc = pthread_cond_timedwait(cond, lock, &deadline);
        HotSavePhase phase = HotSaveSlot_phaseLoad(hot, memory_order_relaxed);
        if (phase == HOT_SAVE_STOPPED)
            break;
        if ((rc == 0 || rc == ETIMEDOUT) && phase == HOT_SAVE_REQUESTED) {
            snapshot_modules(hot);
            HotSaveSlot_storePhase(hot, HOT_SAVE_CAPTURED, memory_order_release);
        }
    }
    pthread_mutex_unlock(lock);
    return nullptr;
}

// Stop the worker FIRST (bounded join), so no thread touches module handles
// after shutdown begins. Teardown Order Law: worker resources die before the
// handles they read. Setting STOPPED also discards a REQUESTED handshake that
// has not been served yet, so shutdown never waits on a fresh snapshot.
fn HotSaveSlot_workerShutdown(HotModule *hot) {
    pthread_mutex_t *lock = HotModule_saveLockMut(hot);
    pthread_mutex_lock(lock);
    if (HotSaveSlot_phaseLoad(hot, memory_order_relaxed) == HOT_SAVE_STOPPED) {
        pthread_mutex_unlock(lock);
        return;
    }
    HotSaveSlot_storePhase(hot, HOT_SAVE_STOPPED, memory_order_release);
    pthread_cond_signal(HotModule_saveCondMut(hot));
    pthread_mutex_unlock(lock);
    pthread_join(HotModule_saveThread(hot), nullptr);
}

// Ask the worker to snapshot the CURRENT generation's state (IDLE -> REQUESTED).
// True when any loaded module exports Hot_save; false when none do — the caller
// then swaps directly, because there is nothing to preserve.
bool HotSaveSlot_begin(HotModule *hot) {
    bool any = false;
    u32 modCount = HotModule_moduleCount(hot);
    for (u32 i = 0; i < modCount && !any; i++) {
        HotModuleInternal *mod = HotModule_moduleAtMut(hot, i);
        any handle = HotModuleInternal_handle(mod);
        if (HotModuleInternal_loaded(mod) && handle && dlsym(handle, "Hot_save") != nullptr)
            any = true;
    }
    if (!any)
        return false;
    pthread_mutex_t *lock = HotModule_saveLockMut(hot);
    pthread_mutex_lock(lock);
    u32 expected = HOT_SAVE_IDLE;
    bool requested = HotSaveSlot_casPhase(hot, &expected, HOT_SAVE_REQUESTED);
    if (requested) {
        HotModule_setSaveCount(hot, 0);
        pthread_cond_signal(HotModule_saveCondMut(hot));
    } else {
        // Hot_poll resolves REQUESTED/CAPTURED/STOPPED before calling begin, so
        // this is unreachable in the loader; report it rather than queue a
        // second handshake behind a word that is already occupied.
        THROW("hot save: begin with phase %u in flight", (u32) expected);
    }
    pthread_mutex_unlock(lock);
    return requested;
}

// CAPTURED -> IDLE, by the frame thread immediately before the swap. The lock
// publishes the worker's blob writes to the caller's later unlocked reads.
bool HotSaveSlot_consume(HotModule *hot) {
    pthread_mutex_t *lock = HotModule_saveLockMut(hot);
    pthread_mutex_lock(lock);
    u32 expected = HOT_SAVE_CAPTURED;
    bool consumed = HotSaveSlot_casPhase(hot, &expected, HOT_SAVE_IDLE);
    pthread_mutex_unlock(lock);
    return consumed;
}
