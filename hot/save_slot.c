#include "hot/save_slot.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hot/internal.h"
#include "util/throw.h"

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

// The one handshake word. Every WRITE happens under saveLock; this read does
// not need it, because the phase is atomic and a torn value is impossible.
HotSavePhase HotSaveSlot_phase(const HotModule *hot) {
    return (HotSavePhase) atomic_load_explicit(&(*hot).savePhase, memory_order_acquire);
}

// Runs ON THE SAVE WORKER thread, under saveLock. Copies each loaded module's
// state blob via its Hot_save export into saveSlots.
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

// The worker loop: sleeps on saveCond (capped so every path reaches the exit
// check bounded), serves one REQUESTED handshake, exits on STOPPED. Both
// transitions are made here, under saveLock, so no other file can invent one.
void *HotSaveSlot_workerMain(void *arg) {
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
        HotSavePhase phase = (HotSavePhase) atomic_load_explicit(&(*hot).savePhase, memory_order_relaxed);
        if (phase == HOT_SAVE_STOPPED)
            break;
        if ((rc == 0 || rc == ETIMEDOUT) && phase == HOT_SAVE_REQUESTED) {
            snapshot_modules(hot);
            atomic_store_explicit(&(*hot).savePhase, HOT_SAVE_CAPTURED, memory_order_release);
        }
    }
    pthread_mutex_unlock(&(*hot).saveLock);
    return nullptr;
}

// Stop the worker FIRST (bounded join), so no thread touches module handles
// after shutdown begins. Teardown Order Law: worker resources die before the
// handles they read. Setting STOPPED also discards a REQUESTED handshake that
// has not been served yet, so shutdown never waits on a fresh snapshot.
void HotSaveSlot_workerShutdown(HotModule *hot) {
    pthread_mutex_lock(&(*hot).saveLock);
    if ((HotSavePhase) atomic_load_explicit(&(*hot).savePhase, memory_order_relaxed) == HOT_SAVE_STOPPED) {
        pthread_mutex_unlock(&(*hot).saveLock);
        return;
    }
    atomic_store_explicit(&(*hot).savePhase, HOT_SAVE_STOPPED, memory_order_release);
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    pthread_join((*hot).saveThread, nullptr);
}

// Ask the worker to snapshot the CURRENT generation's state (IDLE -> REQUESTED).
// True when any loaded module exports Hot_save; false when none do — the caller
// then swaps directly, because there is nothing to preserve.
bool HotSaveSlot_begin(HotModule *hot) {
    bool any = false;
    for (uint32_t i = 0; i < (*hot).module_count && !any; i++) {
        HotModuleInternal *mod = &(*hot).modules[i];
        if ((*mod).loaded && (*mod).handle && dlsym((*mod).handle, "Hot_save") != nullptr)
            any = true;
    }
    if (!any)
        return false;
    pthread_mutex_lock(&(*hot).saveLock);
    uint32_t expected = HOT_SAVE_IDLE;
    bool requested = atomic_compare_exchange_strong_explicit(&(*hot).savePhase, &expected,
        HOT_SAVE_REQUESTED, memory_order_acq_rel, memory_order_relaxed);
    if (requested) {
        (*hot).saveCount = 0;
        pthread_cond_signal(&(*hot).saveCond);
    } else {
        // Hot_poll resolves REQUESTED/CAPTURED/STOPPED before calling begin, so
        // this is unreachable in the loader; report it rather than queue a
        // second handshake behind a word that is already occupied.
        THROW("hot save: begin with phase %u in flight", (unsigned) expected);
    }
    pthread_mutex_unlock(&(*hot).saveLock);
    return requested;
}

// CAPTURED -> IDLE, by the frame thread immediately before the swap. The lock
// publishes the worker's blob writes to the caller's later unlocked reads.
bool HotSaveSlot_consume(HotModule *hot) {
    pthread_mutex_lock(&(*hot).saveLock);
    uint32_t expected = HOT_SAVE_CAPTURED;
    bool consumed = atomic_compare_exchange_strong_explicit(&(*hot).savePhase, &expected,
        HOT_SAVE_IDLE, memory_order_acq_rel, memory_order_relaxed);
    pthread_mutex_unlock(&(*hot).saveLock);
    return consumed;
}
