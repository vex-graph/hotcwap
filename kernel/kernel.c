#include "kernel/kernel.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "annotation/intention.h"
#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "input/key.h"
#include "input/mouse.h"
#include "window/window.h"
#include "spoke/bespoke.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Kernel
 * ============================================================================
 * R1 Host Supervisor: storage and dispatch for executable components across the
 * vexgraph-ecosystem-gitrepo. Owns the master lifetime memory substrate, transient arenas, and the
 * three per-kind registries (processes, applications, consoles). The Kernel owns
 * no execution loops, render ticks, or worker threads; Kernel_run forwards each
 * registered kind to its own run method, allowing dynamic module swapping without
 * supervisor restarts.
 *
 * memory layout comprises master and transient arena handles, fixed-capacity
 * arrays for windowed applications, one-shot processes, and interactive consoles,
 * alongside a thread-safe deferred-add mailbox and supervised worker run slots.
 *
 * Operational invariants: the Terminal-Run Contract enforces that Thread 0 cannot
 * mutate registries once Kernel_runAll arms, redirecting off-thread additions to
 * the mutex-guarded deferred mailbox. Completion occurs reactively when all
 * registered children report finished status, followed by ordered teardown.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Kernel (kernel/kernel.c)
 * ============================================================================
 * SUMMARY:
 *   R1 Host Supervisor: STORAGE + DISPATCH, never an executor. Owns the master
 *   session arena, the transient scratch arena, and the three per-kind
 *   registries (processes / applications / consoles). Kernel_run is a thin
 *   reference forward that hands each registered kind to its own run function;
 *   the Kernel owns NO loop, NO tick, and NO worker thread. Frame scheduling,
 *   the event pump, and presentation live in graphvex's GfxLoop (the Vertical
 *   Integration Law / the Window Decoupling Law).
 *
 * STRUCT FIELDS (Mirroring kernel/kernel.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   Lifetime lifetime;                          // Lifetime memory substrate (master + transient arenas)
 *   void *arena;                                // opaque master arena (attested, never dereferenced)
 *   void *transientArena;                       // opaque scratch arena
 *   uint64_t arenaType;                         // provider id, nonzero = attested
 *   uint64_t transientArenaType;                // provider id, nonzero = attested
 *   Application *applications[KERNEL_MAX_APPS];  // windowed apps (opaque handles)
 *   uint32_t applicationCount;                    // used slots in applications[]
 *   Process     *processes[KERNEL_MAX_PROCS];     // one-shot invokables
 *   uint32_t processCount;                       // used slots in processes[]
 *   Console     *consoles[KERNEL_MAX_CONSOLES];  // session pumps
 *   uint32_t consoleCount;                       // used slots in consoles[]
 *   KernelRunSlot *runSlots;                     // growable supervised run worker slots
 *   uint32_t runCount;
 *   uint32_t runCap;
 *   KernelEndSlot *endSlots;                     // growable lifecycle completion hooks
 *   uint32_t endCount;
 *   uint32_t endCap;
 *   _Atomic bool endHooksFired;                  // fire-once guard for end functions
 *   atomic_uint phase;                           // KernelPhase: READY/RUNNING/DRAINING
 *   atomic_bool running;                          // terminal-run armed flag
 *   atomic_uintptr_t runThreadId;                 // arming (Thread 0) id; 0 = never armed
 *   pthread_mutex_t addLock;                      // guards the mailbox fields below
 *   KernelDeferred *deferred;                     // growable pending-add slots
 *   size_t deferredCount;                         // queued entries
 *   size_t deferredCap;                           // allocated slots
 *
 * PRIVATE HELPERS (kept file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   KernelDeferred    // deferred-add mailbox slot (kernel.h): memory married
 *                     // to Kernel, ALL behavior lives in this file
 *     KernelDeferredKind kind;   // which registry the pending add targets
 *     void *ptr;                 // Application* / Process* / Console* (as void*)
 *
 *   Static wiring (no stored state): kernelRunActive, kernelPostDeferred,
 *   kernelApplyDeferred, kernelDrainDeferred, kernelAdd{Application,Process,
 *   Console}Internal, kernelAllDone, kernelStartApplication.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Kernel()                                : Kernel_0()
 *   - Kernel(arenaBytes)                      : Kernel_1(arenaBytes)
 *   - Kernel(arenaBytes, transientBytes)      : Kernel_2(arenaBytes, transientBytes)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Kernel_destroy(self)                    : Legacy shim over free
 *   - Kernel_free(self)                       : Release supervision and arenas
 *   - Kernel_stop(self)                       : Transition to draining and stop children
 *   - Kernel_runAll(self)                     : Completion reactor supervising children
 *   - Kernel_runProcess(self, p)              : Dispatch to Process_run
 *   - Kernel_runConsole(self, c)              : Dispatch to Console_run
 *   - Kernel_runApplication(self, a)          : Dispatch to Application_run
 *   - Kernel_addApplication(self, app)        : Register windowed application
 *   - Kernel_removeApplication(self, app)     : Unregister windowed application
 *   - Kernel_addProcess(self, p)              : Register one-shot process
 *   - Kernel_removeProcess(self, p)           : Unregister one-shot process
 *   - Kernel_addConsole(self, c)              : Register interactive console
 *   - Kernel_removeConsole(self, c)           : Unregister interactive console
 *   - Kernel_addRunFunction(self, fn, user)   : Register background worker function
 *   - Kernel_addEndFunction(self, fn, user)   : Register completion lifecycle hook
 *
 * Private Core Functions: (.c static)
 *   - kernelRunActive(self)                   : Test if kernel is currently running or draining
 *   - kernelPostDeferred(self, kind, ptr)     : Post registration to off-thread mailbox
 *   - kernelDrainDeferred(self)               : Drain mailbox onto _main thread
 *   - kernelAddApplicationInternal(self, app) : Direct application registration
 *   - kernelAddProcessInternal(self, p)       : Direct process registration
 *   - kernelAddConsoleInternal(self, c)       : Direct console registration
 *   - kernelAllDone(self)                     : Evaluate if all children completed
 *   - kernelStartApplication(app)             : Launch application windows
 *   - kernelFireEndHooks(self)                : Execute lifecycle completion callbacks
 *   - kernelRunWorkerMain(arg)                : Worker thread dispatch wrapper
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Kernel_getPhase(self)                   : Current lifecycle phase
 *   - Kernel_isRunning(self)                  : Query if run is live
 *   - Kernel_getApplication(self, index)      : Retrieve application by index
 *   - Kernel_getApplicationCount(self)        : Count of registered applications
 *   - Kernel_getApplications(self, out, cap)  : Copy application handles
 *   - Kernel_getProcess(self, index)          : Retrieve process by index
 *   - Kernel_getProcessCount(self)            : Count of registered processes
 *   - Kernel_getProcesses(self, out, cap)     : Copy process handles
 *   - Kernel_getConsole(self, index)          : Retrieve console by index
 *   - Kernel_getConsoleCount(self)            : Count of registered consoles
 *   - Kernel_getConsoles(self, out, cap)      : Copy console handles
 *   - Kernel_getRunFunctionCount(self)        : Count of run functions
 *   - Kernel_getEndFunctionCount(self)        : Count of end functions
 *   - Kernel_getArena(self)                   : Master arena handle
 *   - Kernel_getTransientArena(self)          : Scratch arena handle
 *   - Kernel_getLifetime(self)                : Lifetime pointer
 *   - Kernel_lifetime(self)                   : Const lifetime pointer
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

;;INTENTION("Kernel struct is calloc-owned like Application_0; "
            "arenas are provider-owned opaque handles. Migrating the struct itself"
            " into arena storage happens once the registries move to doubling arena slabs per the Dynamic Scalability & Anti-Hardcoding Law — keeps teardown order provable today per the Conflict Triage Law.")

;;INTENTION("Kernel_runApplication starts the app, shows its windows, then"
            " blocks in Application_run's keep-alive parked loop until every "
            "window closes. graphvex's GfxLoop frame scheduler lands later and "
            "layers on top — registering windows into the loop, driving the "
            "event pump, present-on-demand, and telemetry. The Kernel never "
            "owns the loop either way; hotcwap's Application owns the window "
            "end.")

;;INTENTION("The Terminal-Run Contract (the Conflict Triage Law): Kernel_runAll"
            " ARMS the kernel — running=true + runThreadId set on the arming"
            " thread. After arming, Thread-0 registration is REFUSED with one"
            " stderr warn: the run loop applies only what it was given before"
            " run, so the arming thread cannot re-enter or mutate the registry"
            " it is currently iterating (the Tier-1 thread-safety half). Other"
            " threads may still register via the deferred-add mailbox (mutex-"
            " guarded, growable realloc — a COLD path, never a steady-state"
            " allocation), drained on Thread 0 by the run loop's next pass, so"
            " a hot-reloaded module / worker can spawn an Application mid-run"
            " on its own schedule. The registries are never copied to the"
            " mailbox — the mailbox FEEDS the same arrays on the running"
            " thread, keeping all registry mutation single-threaded.")


// CONSTRUCTORS
/** Creates a kernel with the named default persistent and transient arena sizes. */
Kernel *Kernel_0(void) {
    return Kernel_2(KERNEL_ARENA_DEFAULT, KERNEL_TRANSIENT_DEFAULT);
}

/** Creates a kernel with a caller-selected persistent arena size. */
Kernel *Kernel_1(size_t arenaBytes) {
    return Kernel_2(arenaBytes, KERNEL_TRANSIENT_DEFAULT);
}

/** Allocates the supervisor and its persistent/transient lifetime arenas. */
Kernel *Kernel_2(size_t arenaBytes, size_t transientBytes) {
    Kernel *self = (Kernel*) calloc(1, sizeof(Kernel));
    if (!self)
        return nullptr;
    // Create the Lifetime memory substrate directly via vexspoke's MemoryArena.
    Lifetime lt = Lifetime_create(arenaBytes, transientBytes);
    if (!Lifetime_isValid(&lt)) {
        Lifetime_destroy(&lt);
        free(self);
        return nullptr;
    }
    (*self).lifetime = lt;
    (*self).arena = lt.persistentArena;
    (*self).arenaType = lt.persistentType;
    (*self).transientArena = lt.transientArena;
    (*self).transientArenaType = lt.transientType;
    (*self).applicationCount = 0;
    (*self).processCount = 0;
    (*self).consoleCount = 0;
    (*self).commandlineCount = 0;
    (*self).runSlots = nullptr;
    (*self).runCount = 0;
    (*self).runCap = 0;
    (*self).endSlots = nullptr;
    (*self).endCount = 0;
    (*self).endCap = 0;
    atomic_store_explicit(&(*self).endHooksFired, false, memory_order_relaxed);
    atomic_store_explicit(&(*self).phase, KERNEL_PHASE_READY, memory_order_relaxed);
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) 0, memory_order_relaxed);
    pthread_mutex_init(&(*self).addLock, nullptr);
    (*self).deferred = nullptr;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;
    return self;
}

// Forward declare hook invoker
static void kernelFireEndHooks(Kernel *self);
static void *kernelRunWorkerMain(void *arg);

// CORE FUNCTIONS
/** Stops admission and frees an idle, empty kernel; refuses while busy or registered. */
bool Kernel_free(Kernel *self) {
    if (!self)
        return false;
    if (atomic_load_explicit(&(*self).phase, memory_order_acquire) != KERNEL_PHASE_READY) {
        fprintf(stderr, "kernel: refusing free while a run is live or draining — stop and let it quiesce first\n");
        return false;
    }
    if ((*self).applicationCount > 0 || (*self).processCount > 0 || (*self).consoleCount > 0
        || (*self).commandlineCount > 0) {
        fprintf(stderr,
                "kernel: %u app(s), %u process(es), %u console(s), %u commandline(s) still registered; remove before free\n",
                (*self).applicationCount, (*self).processCount, (*self).consoleCount,
                (*self).commandlineCount);
        return false;
    }
    // Fire end hooks BEFORE joining? No — AFTER: hooks release resources the
    // workers may still touch, so quiescence precedes release (the Teardown
    // Order Law). In READY phase no workers exist; Kernel_stop already fired
    // the hooks in that path and the fire-once guard keeps this a no-op.
    // Join any run worker threads
    for (uint32_t i = 0; i < (*self).runCount; i++) {
        KernelRunSlot *slot = &(*self).runSlots[i];
        if (atomic_load_explicit(&(*slot).threadLaunched, memory_order_relaxed)) {
            pthread_join((*slot).thread, nullptr);
            atomic_store_explicit(&(*slot).threadLaunched, false, memory_order_relaxed);
        }
    }
    kernelFireEndHooks(self);

    free((*self).runSlots);
    (*self).runSlots = nullptr;
    (*self).runCount = 0;
    (*self).runCap = 0;

    free((*self).endSlots);
    (*self).endSlots = nullptr;
    (*self).endCount = 0;
    (*self).endCap = 0;

    // No Vulkan teardown here: the device lifecycle (init/shutdown) is owned
    // entirely by graphvex and driven through its own registration path.
    // The Kernel never names Vk_* — see the Teardown Order Law: graphvex
    // tears its device down before the arenas free below, ordered by whoever
    // holds the GfxLoop registration, not by the supervisor.
    pthread_mutex_destroy(&(*self).addLock);
    free((*self).deferred);
    (*self).deferred = nullptr;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;

    for (uint32_t i = 0; i < KERNEL_MAX_APPS; i++)
        (*self).applications[i] = nullptr;
    (*self).applicationCount = 0;
    for (uint32_t i = 0; i < KERNEL_MAX_PROCS; i++)
        (*self).processes[i] = nullptr;
    (*self).processCount = 0;
    for (uint32_t i = 0; i < KERNEL_MAX_CONSOLES; i++)
        (*self).consoles[i] = nullptr;
    (*self).consoleCount = 0;
    for (uint32_t i = 0; i < KERNEL_MAX_COMMANDLINES; i++)
        (*self).commandlines[i] = nullptr;
    (*self).commandlineCount = 0;

    (*self).transientArena = nullptr;
    (*self).transientArenaType = 0;
    (*self).arena = nullptr;
    (*self).arenaType = 0;
    Lifetime_destroy(&(*self).lifetime);
    free(self);
    return true;
}

/** Compatibility destructor that forwards to Kernel_free. */
void Kernel_destroy(Kernel *self) {
    if (!self)
        return;
    (void) Kernel_free(self);
}

/** Requests draining and stops active applications and consoles. */
void Kernel_stop(Kernel *self) {
    if (!self)
        return;
    // LIVE RUN: claim teardown ownership once (RUNNING -> DRAINING). The
    // winner stops the kinds and clears running so the reactor exits; the
    // ARMING THREAD then joins the workers and fires the end hooks in its
    // epilogue — never here, because hooks may release resources the workers
    // still touch (the Teardown Order Law: quiescence precedes release).
    uint32_t expected = KERNEL_PHASE_RUNNING;
    if (atomic_compare_exchange_strong_explicit(&(*self).phase, &expected, KERNEL_PHASE_DRAINING,
                                                memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
        for (uint32_t i = 0; i < (*self).applicationCount; i++) {
            Application *app = (*self).applications[i];
            if (app)
                Application_stop(app);
        }
        for (uint32_t i = 0; i < (*self).consoleCount; i++) {
            Console *c = (*self).consoles[i];
            if (c)
                Console_cancel(c);
        }
        for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
            CommandLine *cl = (*self).commandlines[i];
            if (cl)
                CommandLine_stop(cl);
        }
        return;
    }
    // NO RUN LIVE (READY): nothing to quiesce — fire the end hooks directly
    // (the fire-once guard keeps repeated stops harmless).
    if (expected == KERNEL_PHASE_READY) {
        kernelFireEndHooks(self);
        return;
    }
    // DRAINING: the arming thread already owns teardown; nothing to do.
}

/** Returns the atomic lifecycle phase, or READY for a null kernel. */
KernelPhase Kernel_getPhase(const Kernel *self) {
    if (!self)
        return KERNEL_PHASE_READY;
    return (KernelPhase) atomic_load_explicit(&(*self).phase, memory_order_acquire);
}

/** Returns the atomic run flag, or false for a null kernel. */
bool Kernel_isRunning(const Kernel *self) {
    if (!self)
        return false;
    return atomic_load_explicit(&(*self).running, memory_order_relaxed);
}

/** Invokes registered end callbacks once, guarded against repeated teardown. */
static void kernelFireEndHooks(Kernel *self) {
    if (!self)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&(*self).endHooksFired, &expected, true, memory_order_relaxed, memory_order_relaxed))
        return;
    for (uint32_t i = 0; i < (*self).endCount; i++) {
        KernelEndSlot *slot = &(*self).endSlots[i];
        if ((*slot).fn)
            (*slot).fn(self, (*slot).userdata);
    }
}

/** Runs one registered worker callback and publishes its completion flag. */
static void *kernelRunWorkerMain(void *arg) {
    KernelRunSlot *slot = (KernelRunSlot*) arg;
    if (slot && (*slot).fn) {
        (*slot).fn((*slot).userdata);
    }
    if (slot) {
        atomic_store_explicit(&(*slot).done, true, memory_order_relaxed);
    }
    return nullptr;
}

// --- REGISTRY INTERNALS + DEFERRED MAILBOX -----------------------------------
// The three Kernel_add* entry points split into (a) the Terminal-Run guard
// and (b) apply-internal peers that touch ONLY the registry arrays. The
// guard lives at the public seam; the apply-internal peers are the sole
// writers of the arrays. During a live run only the arming thread mutates
// the registries (via drain), keeping iteration single-threaded per the
// Tier-1 thread-safety half of the ;;INTENTION above.

/** Reports whether the phase has left READY. */
static bool kernelRunActive(const Kernel *self) {
    return atomic_load_explicit(&(*self).phase, memory_order_acquire) != KERNEL_PHASE_READY;
}

// DRAINING is owned by the arming thread: no registration from any thread,
// and nothing may grow a registry/slot table a joining worker still points
// into. Callers already warn on refusal.
/** Reports whether the kernel is in its teardown-owned draining phase. */
static bool kernelDraining(const Kernel *self) {
    return atomic_load_explicit(&(*self).phase, memory_order_acquire) == KERNEL_PHASE_DRAINING;
}

// Forward: start one app manifest non-blocking (defined below with the run
// helpers; kernelApplyDeferred admits deferred adds and starts them like the
// initial pass would).
static void kernelStartApplication(Kernel *self, Application *a);

/** Adds an application to the single-threaded registry if unique and within capacity. */
static bool kernelAddApplicationInternal(Kernel *self, Application *app) {
    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        if ((*self).applications[i] == app)
            return false;
    if ((*self).applicationCount >= KERNEL_MAX_APPS)
        return false;
    (*self).applications[(*self).applicationCount++] = app;
    return true;
}

/** Adds a process to the single-threaded registry if unique and within capacity. */
static bool kernelAddProcessInternal(Kernel *self, Process *p) {
    for (uint32_t i = 0; i < (*self).processCount; i++)
        if ((*self).processes[i] == p)
            return false;
    if ((*self).processCount >= KERNEL_MAX_PROCS)
        return false;
    (*self).processes[(*self).processCount++] = p;
    return true;
}

/** Adds a console to the single-threaded registry if unique and within capacity. */
static bool kernelAddConsoleInternal(Kernel *self, Console *c) {
    for (uint32_t i = 0; i < (*self).consoleCount; i++)
        if ((*self).consoles[i] == c)
            return false;
    if ((*self).consoleCount >= KERNEL_MAX_CONSOLES)
        return false;
    (*self).consoles[(*self).consoleCount++] = c;
    return true;
}

/** Adds a terminal application to the registry if unique and within capacity. */
static bool kernelAddCommandLineInternal(Kernel *self, CommandLine *c) {
    for (uint32_t i = 0; i < (*self).commandlineCount; i++)
        if ((*self).commandlines[i] == c)
            return false;
    if ((*self).commandlineCount >= KERNEL_MAX_COMMANDLINES)
        return false;
    (*self).commandlines[(*self).commandlineCount++] = c;
    return true;
}

// Post a pending add from ANY thread (mutex-guarded, growable realloc — cold
// path, never steady-state). The drain on Thread 0 steals the whole batch in
// one lock and re-owns the arrays for the rest of the pass.
/** Queues a registration request in the mutex-protected deferred mailbox. */
static bool kernelPostDeferred(Kernel *self, KernelDeferredKind kind, void *ptr) {
    pthread_mutex_lock(&(*self).addLock);
    if ((*self).deferredCount == (*self).deferredCap) {
        size_t cap = (*self).deferredCap ? (*self).deferredCap * 2 : 4;
        KernelDeferred *nx = (KernelDeferred*) realloc((*self).deferred, cap * sizeof(KernelDeferred));
        if (!nx) {
            pthread_mutex_unlock(&(*self).addLock);
            fprintf(stderr, "kernel: deferred-add mailbox grow failed (Kernel_add* dropped)\n");
            return false;
        }
        (*self).deferred = nx;
        (*self).deferredCap = cap;
    }
    (*self).deferred[(*self).deferredCount].kind = kind;
    (*self).deferred[(*self).deferredCount].ptr = ptr;
    (*self).deferredCount++;
    pthread_mutex_unlock(&(*self).addLock);
    return true;
}

// Admit one pending add into the registry on the arming thread, then START
// it exactly like the initial pass would: processes invoke one-shot, consoles
// spawn their session, applications start + show their windows. A kind that
// fails the dup/full/guards stays unregistered and silent.
/** Applies one queued add on the run thread, registering and starting its kind. */
static void kernelApplyDeferred(Kernel *self, KernelDeferredKind kind, void *ptr) {
    switch (kind) {
        case KERNEL_DEFERRED_APPLICATION: {
            Application *a = (Application*) ptr;
            if (kernelAddApplicationInternal(self, a))
                kernelStartApplication(self, a);
            break;
        }
        case KERNEL_DEFERRED_PROCESS: {
            Process *p = (Process*) ptr;
            if (kernelAddProcessInternal(self, p)) {
                int exitStatus = 0;
                (void) Process_run(p, &exitStatus);
            }
            break;
        }
        case KERNEL_DEFERRED_CONSOLE: {
            Console *c = (Console*) ptr;
            if (kernelAddConsoleInternal(self, c))
                (void) Console_run(c);
            break;
        }
        case KERNEL_DEFERRED_COMMANDLINE: {
            CommandLine *cl = (CommandLine*) ptr;
            if (kernelAddCommandLineInternal(self, cl) && !CommandLine_isOpen(cl))
                (void) CommandLine_open(cl);
            break;
        }
        case KERNEL_DEFERRED_NONE:
        default:
            break;
    }
}

// Steal the whole pending batch under the lock, then apply each entry OUTSIDE
// the lock — the run loop keeps iterating its own registry, never the mailbox.
/** Takes and applies the current deferred batch outside the mailbox lock. */
static void kernelDrainDeferred(Kernel *self) {
    pthread_mutex_lock(&(*self).addLock);
    KernelDeferred *batch = (*self).deferred;
    size_t n = (*self).deferredCount;
    (*self).deferred = nullptr;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;
    pthread_mutex_unlock(&(*self).addLock);

    for (size_t i = 0; i < n; i++)
        kernelApplyDeferred(self, batch[i].kind, batch[i].ptr);

    if (batch)
        free(batch);
}

// Teardown counterpart of the drain: steal the pending batch and DISCARD it.
// The run ended (stop landed or all kinds done) — starting fresh work now
// would outlive the stop, and DRAINING refuses new posts anyway, so anything
// left was queued while RUNNING and never admitted. One warn per drop batch.
/** Discards pending additions after run completion and reports a dropped batch. */
static void kernelDropDeferred(Kernel *self) {
    pthread_mutex_lock(&(*self).addLock);
    KernelDeferred *batch = (*self).deferred;
    size_t n = (*self).deferredCount;
    (*self).deferred = nullptr;
    (*self).deferredCount = 0;
    (*self).deferredCap = 0;
    pthread_mutex_unlock(&(*self).addLock);

    if (n > 0)
        fprintf(stderr, "kernel: dropped %zu deferred add(s) — the run ended before admission\n", n);
    if (batch)
        free(batch);
}

// Every registered kind reports done? Processes are done by construction
// after the initial invoke pass (one-shot); consoles when their session
// joined (Console_isRunning false); apps per the Application_isFinished
// completion predicate (all windows closed or externally stopped).
/** Checks application, console, and worker completion to decide reactor exit. */
static bool kernelAllDone(const Kernel *self) {
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *a = (*self).applications[i];
        if (a && !Application_isFinished(a))
            return false;
    }
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        Console *c = (*self).consoles[i];
        if (c && Console_isRunning(c))
            return false;
    }
    for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
        CommandLine *cl = (*self).commandlines[i];
        if (cl && CommandLine_isOpen(cl) && !CommandLine_isCancelled(cl))
            return false;
    }
    for (uint32_t i = 0; i < (*self).runCount; i++) {
        const KernelRunSlot *slot = &(*self).runSlots[i];
        if (!atomic_load_explicit(&(*slot).done, memory_order_relaxed))
            return false;
    }
    return true;
}

// Start one app manifest NON-blocking: flip running, show every window. The
// reactor supervises it to completion; the blocking Kernel_runApplication
// path adds Application_run on top.
/** Begins an application and shows its registered windows without blocking. */
static void kernelStartApplication(Kernel *self, Application *a) {
    (void) self;
    if (!a)
        return;
    (void) Application_begin(a);
    uint32_t winCount = Application_getWindowCount(a);
    for (uint32_t i = 0; i < winCount; i++) {
        Window *w = Application_getWindow(a, i);
        if (w)
            Window_show(w);
    }
}

// --- WORK DISPATCH ----------------------------------------------------------
// The Kernel forwards work, it never does it: each registered kind is relayed
// to its own run function. The frame loop, event pump, presentation, and
// per-frame handler invocation live in graphvex's GfxLoop (the Vertical
// Integration Law / the Window Decoupling Law). Kernel_runAll is the ONE
// supervising entry: it ARMS (the Terminal-Run Contract), dispatches the
// initial registrations, then runs a completion reactor until every kind is
// done or Kernel_stop() lands.

/** Arms supervision, dispatches registered work, services it, then joins workers. */
int Kernel_runAll(Kernel *self) {
    if (!self)
        return KERNEL_EXIT_NO_APPS;
    if ((*self).applicationCount == 0 && (*self).processCount == 0 && (*self).consoleCount == 0
        && (*self).commandlineCount == 0 && (*self).runCount == 0)
        return KERNEL_EXIT_NO_APPS;

    // TERMINAL-RUN ARM: claim the run (READY -> RUNNING). From here the
    // arming (Thread 0) thread can no longer register — all Kernel_add* on it
    // are refused; other threads post into the deferred mailbox, drained on
    // the next pass below. A second concurrent runAll is refused (one warn).
    uint32_t readyPhase = KERNEL_PHASE_READY;
    if (!atomic_compare_exchange_strong_explicit(&(*self).phase, &readyPhase, KERNEL_PHASE_RUNNING,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        fprintf(stderr, "kernel: Kernel_run refused — a run is already live or draining\n");
        return KERNEL_EXIT_NO_APPS;
    }
    atomic_store_explicit(&(*self).running, true, memory_order_relaxed);
    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) pthread_self(), memory_order_relaxed);

    int rc = KERNEL_EXIT_OK;

    // One-shot invokables: run to completion on the arming thread.
    for (uint32_t i = 0; i < (*self).processCount; i++) {
        Process *p = (*self).processes[i];
        if (p) {
            int r = 0;
            if (Process_run(p, &r) != PROCESS_OK)
                r = KERNEL_EXIT_PROCESS_FAILED;
            if (r != 0 && rc == KERNEL_EXIT_OK)
                rc = r;
        }
    }

    // Launch supervised run worker threads
    for (uint32_t i = 0; i < (*self).runCount; i++) {
        KernelRunSlot *slot = &(*self).runSlots[i];
        atomic_store_explicit(&(*slot).done, false, memory_order_relaxed);
        if (pthread_create(&(*slot).thread, nullptr, kernelRunWorkerMain, slot) == 0) {
            atomic_store_explicit(&(*slot).threadLaunched, true, memory_order_relaxed);
        } else {
            atomic_store_explicit(&(*slot).done, true, memory_order_relaxed);
        }
    }

    // Session pumps + windowed apps: start without blocking — the reactor
    // below supervises them to completion.
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        Console *c = (*self).consoles[i];
        if (c)
            (void) Console_run(c);
    }
    // Terminal applications: acquire the device, then drive it each pass below.
    for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
        CommandLine *cl = (*self).commandlines[i];
        if (cl && !CommandLine_isOpen(cl))
            (void) CommandLine_open(cl);
    }
    for (uint32_t i = 0; i < (*self).applicationCount; i++)
        kernelStartApplication(self, (*self).applications[i]);

    bridgeBespoke();

    // COMPLETION REACTOR: drain deferred adds, pump OS events, supervise
    // bounded console slices, ask app closed-state at a ~250ms cadence. 5ms
    // per pass keeps teardown responsive (the Bounded Wait Law) without a
    // busy spin.
    const struct timespec park = {0, 5000000L};
    uint64_t pass = 0;
    while (atomic_load_explicit(&(*self).running, memory_order_relaxed) && !kernelAllDone(self)) {
        kernelDrainDeferred(self);

        Window_pollEvents();
        for (uint32_t i = 0; i < (*self).applicationCount; i++)
            Application_poll((*self).applications[i]);
        bridgeBespokeCheck();

        char buf[4096];
        for (uint32_t i = 0; i < (*self).consoleCount; i++) {
            Console *c = (*self).consoles[i];
            if (!c || !Console_isRunning(c))
                continue;
            size_t n = 0;
            if (Console_poll(c, buf, sizeof(buf), &n) && n > 0)
                (void) fwrite(buf, 1, n, stdout);
        }
        // Terminal applications: one non-blocking draw/resize/input pass each.
        for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
            CommandLine *cl = (*self).commandlines[i];
            if (cl && CommandLine_isOpen(cl) && CommandLine_hasApp(cl) && !CommandLine_isCancelled(cl))
                (void) CommandLine_tick(cl);
        }

        if ((pass % 50) == 0) {
            for (uint32_t i = 0; i < (*self).applicationCount; i++) {
                Application *a = (*self).applications[i];
                if (!a)
                    continue;
                Application_pollHot(a); // generation-driven hot swap, ~250ms
                if (Application_isFinished(a))
                    Application_stop(a);
            }
        }

        nanosleep(&park, nullptr);
        pass++;
    }

    // Reactor exit: the arming thread owns teardown. It stops every kind
    // still live, JOINS the run workers (quiescence first), then fires the
    // end functions — hooks release resources workers may still touch, so
    // they run only after the last join (the Teardown Order Law). While
    // DRAINING, Kernel_add* stays closed on every thread: no registration
    // can grow a registry a joining worker still points into. The final
    // drain is a best-effort straggler sweep; admitted adds run like the
    // initial pass would. Then the phase returns to READY.
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        Application *a = (*self).applications[i];
        if (a && Application_isRunning(a))
            Application_stop(a);
        Application_finish(a);
    }
    for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
        CommandLine *cl = (*self).commandlines[i];
        if (cl)
            CommandLine_close(cl);   // restore the terminal before any join (the Teardown Order Law)
    }
    for (uint32_t i = 0; i < (*self).runCount; i++) {
        KernelRunSlot *slot = &(*self).runSlots[i];
        if (atomic_load_explicit(&(*slot).threadLaunched, memory_order_relaxed)) {
            pthread_join((*slot).thread, nullptr);
            atomic_store_explicit(&(*slot).threadLaunched, false, memory_order_relaxed);
        }
    }
    kernelFireEndHooks(self);
    kernelDropDeferred(self);

    atomic_store_explicit(&(*self).runThreadId, (uintptr_t) 0, memory_order_relaxed);
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    atomic_store_explicit(&(*self).phase, KERNEL_PHASE_READY, memory_order_release);

    return rc;
}

/** Forwards one process invocation and returns its callback status or kernel error. */
int Kernel_runProcess(Kernel *self, Process *p) {
    (void) self;
    if (!p)
        return KERNEL_EXIT_NO_APPS;
    int exitStatus = 0;
    if (Process_run(p, &exitStatus) != PROCESS_OK)
        return KERNEL_EXIT_PROCESS_FAILED;
    return exitStatus;
}

/** Starts a console session through its configured IO seam. */
bool Kernel_runConsole(Kernel *self, Console *c) {
    (void) self;
    return c ? Console_run(c) : false;
}

// Blocking terminal-application entry: attaches nothing, just runs the registered
// CommandLine loop to completion on the calling thread. The 1-arity Kernel_run
// reactor drives the same CommandLine with CommandLine_tick instead.
/** Runs one CommandLine terminal-application loop; returns its exit code. */
int Kernel_runCommandLine(Kernel *self, CommandLine *c) {
    (void) self;
    if (!c)
        return KERNEL_EXIT_NO_APPS;
    if (!CommandLine_isOpen(c) && !CommandLine_open(c))
        return KERNEL_EXIT_PROCESS_FAILED;
    return CommandLine_run(c);
}

// The GfxLoop runner seat (the Vertical Integration Law: R1 owns fn-tables,
// never links R3 and never names graphvex). Kernel_setGfxAppRunner installs
// the demand-driven Application loop at boot; a null seat falls back to the
// parked Application_run loop, so hotcwap compiles and links standalone with
// zero graphvex symbols (no weak externs, no Mach-O undefined-symbol traps).

// R1's half of the GfxLoop lifecycle contract (the Vertical Integration Law:
// the Kernel never names Vk_* and graphvex never mirrors a Kernel/Application
// layout). Completion predicate + per-pass servicing: the loop keeps running
// until every window closes, and each ask doubles as the generation-driven
// hot-reload poll (cheap generation compare at frame cadence).
/** Services application reload/events and reports whether it remains unfinished. */
static bool kernelGfxAppContinues(void *context) {
    Application *a = (Application*) context;
    if (!a)
        return false;
    Application_pollHot(a);
    Application_poll(a);
    bridgeBespokeCheck();
    return !Application_isFinished(a);
}

// Thread-0 pump handed to graphvex's frame loop. A single native event is
// mirrored into vexspoke's input rings — dispatching those rings
// to registered listeners (the widget bridge) is a separate step. Without
// this pass, a window that pumps events still never delivers a single click
// or keystroke to any listener. Returning "more queued" lets the graphics loop
// paint the current scroll position before accepting another event. hotcwap
// owns the pump (the Window Decoupling Law); vexspoke owns ring dispatch (R2).
/** Polls one window event step, then dispatches queued key and mouse input. */
static bool kernelGfxPump(void) {
    bool queued = Window_pollEventStep();
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    return queued;
}

/** Starts one application through the installed graphics runner or parked loop. */
int Kernel_runApplication(Kernel *self, Application *a) {
    if (!self || !a)
        return KERNEL_EXIT_NO_APPS;
    kernelStartApplication(self, a);
    bridgeBespoke();

    // If a GfxLoop runner is installed in the seat, hand the application to
    // the demand-driven frame scheduler loop: completion + hot servicing stay
    // HERE (R1), the frame loop + Thread-0 event pump stay THERE (R3).
    // Otherwise, fall back to hotcwap's parked loop.
    if ((*self).gfxAppRun != nullptr) {
        int result = (*self).gfxAppRun(a, kernelGfxAppContinues, kernelGfxPump);
        Application_finish(a);
        return result;
    }

    bridgeBespoke();
    Application_run(a);
    return KERNEL_EXIT_OK;
}

// --- APPLICATION REGISTRY ---
// Registration is the PRE-arm contract: while Kernel_runAll is live, the
// arming thread's adds are refused (one warn) and other threads' adds post
// to the deferred mailbox, admitted on the running thread's next pass.
/** Registers now or defers the application add to the active run thread. */
bool Kernel_addApplication(Kernel *self, Application *app) {
    if (!self || !app)
        return false;
    if (kernelDraining(self)) {
        fprintf(stderr, "kernel: Kernel_addApplication refused while draining\n");
        return false;
    }
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addApplication refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_APPLICATION, app);
    }
    return kernelAddApplicationInternal(self, app);
}

/** Removes a registered application without destroying the borrowed object. */
bool Kernel_removeApplication(Kernel *self, Application *app) {
    if (!self || !app)
        return false;
    for (uint32_t i = 0; i < (*self).applicationCount; i++) {
        if ((*self).applications[i] == app) {
            (*self).applications[i] = (*self).applications[--(*self).applicationCount];
            (*self).applications[(*self).applicationCount] = nullptr;
            return true;
        }
    }
    return false;
}

// --- PROCESS REGISTRY ---
/** Registers now or defers the process add to the active run thread. */
bool Kernel_addProcess(Kernel *self, Process *p) {
    if (!self || !p)
        return false;
    if (kernelDraining(self)) {
        fprintf(stderr, "kernel: Kernel_addProcess refused while draining\n");
        return false;
    }
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addProcess refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_PROCESS, p);
    }
    return kernelAddProcessInternal(self, p);
}

/** Removes a registered process without freeing it. */
bool Kernel_removeProcess(Kernel *self, Process *p) {
    if (!self || !p)
        return false;
    for (uint32_t i = 0; i < (*self).processCount; i++) {
        if ((*self).processes[i] == p) {
            (*self).processes[i] = (*self).processes[--(*self).processCount];
            (*self).processes[(*self).processCount] = nullptr;
            return true;
        }
    }
    return false;
}

// --- CONSOLE REGISTRY ---
/** Registers now or defers the console add to the active run thread. */
bool Kernel_addConsole(Kernel *self, Console *c) {
    if (!self || !c)
        return false;
    if (kernelDraining(self)) {
        fprintf(stderr, "kernel: Kernel_addConsole refused while draining\n");
        return false;
    }
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addConsole refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_CONSOLE, c);
    }
    return kernelAddConsoleInternal(self, c);
}

/** Removes a registered console without destroying it. */
bool Kernel_removeConsole(Kernel *self, Console *c) {
    if (!self || !c)
        return false;
    for (uint32_t i = 0; i < (*self).consoleCount; i++) {
        if ((*self).consoles[i] == c) {
            (*self).consoles[i] = (*self).consoles[--(*self).consoleCount];
            (*self).consoles[(*self).consoleCount] = nullptr;
            return true;
        }
    }
    return false;
}

// --- COMMANDLINE REGISTRY ---
/** Registers now or defers the terminal-application add to the active run thread. */
bool Kernel_addCommandLine(Kernel *self, CommandLine *c) {
    if (!self || !c)
        return false;
    if (kernelDraining(self)) {
        fprintf(stderr, "kernel: Kernel_addCommandLine refused while draining\n");
        return false;
    }
    if (kernelRunActive(self)) {
        if ((uintptr_t) pthread_self() == atomic_load_explicit(&(*self).runThreadId, memory_order_relaxed)) {
            fprintf(stderr, "kernel: Kernel_addCommandLine refused on the arming thread while Kernel_run is live — register before Kernel_run(), or post from another thread\n");
            return false;
        }
        return kernelPostDeferred(self, KERNEL_DEFERRED_COMMANDLINE, c);
    }
    return kernelAddCommandLineInternal(self, c);
}

/** Removes a registered terminal application without destroying it. */
bool Kernel_removeCommandLine(Kernel *self, CommandLine *c) {
    if (!self || !c)
        return false;
    for (uint32_t i = 0; i < (*self).commandlineCount; i++) {
        if ((*self).commandlines[i] == c) {
            (*self).commandlines[i] = (*self).commandlines[--(*self).commandlineCount];
            (*self).commandlines[(*self).commandlineCount] = nullptr;
            return true;
        }
    }
    return false;
}

// --- RUN FUNCTIONS & END FUNCTIONS ---
/** Adds a worker callback to be launched and joined by the next run. */
bool Kernel_addRunFunction(Kernel *self, KernelRunFn fn, void *userdata) {
    if (!self || !fn)
        return false;
    if (kernelRunActive(self)) {
        fprintf(stderr, "kernel: Kernel_addRunFunction refused while a run is live or draining — worker slots must never grow under a joined thread\n");
        return false;
    }
    if ((*self).runCount >= (*self).runCap) {
        uint32_t newCap = (*self).runCap ? (*self).runCap * 2 : 4;
        KernelRunSlot *newSlots = (KernelRunSlot*) realloc((*self).runSlots, newCap * sizeof(KernelRunSlot));
        if (!newSlots)
            return false;
        (*self).runSlots = newSlots;
        (*self).runCap = newCap;
    }
    KernelRunSlot *slot = &(*self).runSlots[(*self).runCount++];
    (*slot).fn = fn;
    (*slot).userdata = userdata;
    atomic_store_explicit(&(*slot).threadLaunched, false, memory_order_relaxed);
    atomic_store_explicit(&(*slot).done, false, memory_order_relaxed);
    return true;
}

/** Adds a completion callback invoked once after supervised work quiesces. */
bool Kernel_addEndFunction(Kernel *self, KernelEndFn fn, void *userdata) {
    if (!self || !fn)
        return false;
    if (kernelRunActive(self)) {
        fprintf(stderr, "kernel: Kernel_addEndFunction refused while a run is live or draining — end slots must never grow under a firing loop\n");
        return false;
    }
    if ((*self).endCount >= (*self).endCap) {
        uint32_t newCap = (*self).endCap ? (*self).endCap * 2 : 4;
        KernelEndSlot *newSlots = (KernelEndSlot*) realloc((*self).endSlots, newCap * sizeof(KernelEndSlot));
        if (!newSlots)
            return false;
        (*self).endSlots = newSlots;
        (*self).endCap = newCap;
    }
    KernelEndSlot *slot = &(*self).endSlots[(*self).endCount++];
    (*slot).fn = fn;
    (*slot).userdata = userdata;
    return true;
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Returns the number of registered run callbacks. */
uint32_t Kernel_getRunFunctionCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).runCount;
}

;;GETTER
/** Returns the number of registered end callbacks. */
uint32_t Kernel_getEndFunctionCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).endCount;
}

;;GETTER
/** Returns the application handle at index, or nullptr if absent/out of range. */
Application *Kernel_getApplication(const Kernel *self, uint32_t index) {
    if (!self)
        return nullptr;
    if (index >= (*self).applicationCount)
        return nullptr;
    return (*self).applications[index];
}

;;GETTER
/** Returns the number of registered applications. */
uint32_t Kernel_getApplicationCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).applicationCount;
}

;;GETTER
/** Copies up to cap application handles to out and returns the number copied. */
uint32_t Kernel_getApplications(const Kernel *self, Application **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).applicationCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).applications[i];
    return n;
}

;;SETTER
/** Installs or clears the optional graphics application runner callback. */
void Kernel_setGfxAppRunner(Kernel *self, int (*fn)(void *context, bool (*continueFn)(void*), bool (*pollFn)(void))) {
    if (!self)
        return;
    (*self).gfxAppRun = fn;
}

;;GETTER
/** Returns the configured graphics runner, or nullptr for null self. */
int (*Kernel_getGfxAppRunner(const Kernel *self))(void *context, bool (*continueFn)(void*), bool (*pollFn)(void)) {
    if (!self)
        return nullptr;
    return (*self).gfxAppRun;
}

;;GETTER
/** Returns the process handle at index, or nullptr if absent/out of range. */
Process *Kernel_getProcess(const Kernel *self, uint32_t index) {
    if (!self)
        return nullptr;
    if (index >= (*self).processCount)
        return nullptr;
    return (*self).processes[index];
}

;;GETTER
/** Returns the number of registered processes. */
uint32_t Kernel_getProcessCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).processCount;
}

;;GETTER
/** Copies up to cap process handles to out and returns the number copied. */
uint32_t Kernel_getProcesses(const Kernel *self, Process **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).processCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).processes[i];
    return n;
}

;;GETTER
/** Returns the console handle at index, or nullptr if absent/out of range. */
Console *Kernel_getConsole(const Kernel *self, uint32_t index) {
    if (!self)
        return nullptr;
    if (index >= (*self).consoleCount)
        return nullptr;
    return (*self).consoles[index];
}

;;GETTER
/** Returns the number of registered consoles. */
uint32_t Kernel_getConsoleCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).consoleCount;
}

;;GETTER
/** Copies up to cap console handles to out and returns the number copied. */
uint32_t Kernel_getConsoles(const Kernel *self, Console **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).consoleCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).consoles[i];
    return n;
}

;;GETTER
/** Returns the CommandLine handle at index, or nullptr if absent/out of range. */
CommandLine *Kernel_getCommandLine(const Kernel *self, uint32_t index) {
    if (!self)
        return nullptr;
    if (index >= (*self).commandlineCount)
        return nullptr;
    return (*self).commandlines[index];
}

;;GETTER
/** Returns the number of registered terminal applications. */
uint32_t Kernel_getCommandLineCount(const Kernel *self) {
    if (!self)
        return 0;
    return (*self).commandlineCount;
}

;;GETTER
/** Copies up to cap CommandLine handles to out and returns the number copied. */
uint32_t Kernel_getCommandLines(const Kernel *self, CommandLine **out, uint32_t cap) {
    if (!self || !out || cap == 0)
        return 0;
    uint32_t n = (*self).commandlineCount;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).commandlines[i];
    return n;
}

;;GETTER
/** Returns the opaque master arena handle, or nullptr for null self. */
void *Kernel_getArena(const Kernel *self) {
    if (!self)
        return nullptr;
    return (*self).arena;
}

;;GETTER
/** Returns the opaque transient arena handle, or nullptr for null self. */
void *Kernel_getTransientArena(const Kernel *self) {
    if (!self)
        return nullptr;
    return (*self).transientArena;
}

;;GETTER
/** Returns the kernel's embedded Lifetime record. */
Lifetime *Kernel_getLifetime(Kernel *self) {
    return self ? &(*self).lifetime : nullptr;
}

;;GETTER
/** Returns a const pointer to the kernel's embedded Lifetime record. */
const Lifetime *Kernel_lifetime(const Kernel *self) {
    return self ? &(*self).lifetime : nullptr;
}
