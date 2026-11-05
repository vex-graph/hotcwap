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

// Runs ON THE SAVE WORKER thread. Copies each loaded module's state blob via
// its Hot_save export into saveSlots, then publishes saveDone.
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
// check bounded), snapshots on request, exits on cancel.
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

// Stop the worker FIRST (bounded join), so no thread touches module handles
// after shutdown begins. Teardown Order Law: worker resources die before the
// handles they read.
void HotSaveSlot_workerShutdown(HotModule *hot) {
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

// Ask the worker to snapshot the CURRENT generation's state. True when any
// loaded module exports Hot_save (a snapshot was requested); false when none
// do — the caller then swaps directly, because there is nothing to preserve.
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
    (*hot).saveRequested = true;
    (*hot).saveDone = false;
    (*hot).saveCount = 0;
    pthread_cond_signal(&(*hot).saveCond);
    pthread_mutex_unlock(&(*hot).saveLock);
    return true;
}
