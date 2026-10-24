#ifndef HOT_KERNEL_KERNEL_H
#define HOT_KERNEL_KERNEL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "spoke/lifetime.h"
#include "application.h"
#include "console.h"
#include "process.h"
#include "commandline.h"

// kernel/kernel.h — R1 Host Supervisor: storage + dispatch, never an executor.
//
// The Kernel is an object that STORES registries and SENDS work. It holds the
// master arena, the transient scratch arena, and the four per-kind registries
// (processes / applications / consoles / commandlines). It owns NO loop, NO tick,
// and NO worker thread — Kernel_run is a thin reference
// forward that hands each registered kind to its own run function:
// Process_run() for one-shot invokables, Console_run() for session pumps,
// CommandLine for terminal applications, and graphvex's GfxLoop registration
// for windowed Applications. The Kernel never implements the work itself, so
// hot-reloading a module swaps the running code without touching the supervisor.
//
// The frame loop, event pump (Thread 0), presentation, and per-frame handler
// invocation live in graphvex's GfxLoop (the Vertical Integration Law / the Window Decoupling Law); the Kernel
// merely hands the Application over.
//
// Lifecycle (vk_test order):
//   kernel -> application -> Kernel_addApplication ->
//   window -> Application_addWindow -> Kernel_run(kernel) ->
//   Kernel_removeApplication -> Application_free -> Kernel_free.
//
// Kernel_run is the SOLE blocking entry: it dispatches every registered kind
// through that kind's own run method. Kernel_free refuses (false + stderr
// warn) while any kind is still registered — remove them first per the Teardown Order Law.)
//
// --- The Terminal-Run Contract (Kernel_runAll arming) ---
// Kernel_runAll is TERMINAL on the calling thread: every registration must
// happen BEFORE the first Kernel_runAll call — that is the sole supported
// path. Once armed (running == true, runThreadId == arming thread):
//   - The arming (Thread 0) thread CANNOT register anything more: the
//     Kernel_add* entry points refuse with false + one stderr warn. The run
//     loop applies ONLY what was registered before arming.
//   - ANY OTHER thread may still register: the add posts into the deferred-add
//     mailbox (mutex-guarded, growable, cold path), and the run loop drains it
//     on its next pass — so a hot-reloaded module or a worker can still spawn
//     a new Application / Window mid-run, on its own schedule, delivered on
//     Thread 0 by the pass itself.
// Kernel_runAll completes when EVERY registered kind is done: processes after
// their one-shot invoke, consoles when their session joins (Console_isRunning
// flips false), applications when all windows report shouldClose per the
// Application_isFinished completion predicate. Kernel_stop() ends the run
// immediately. See the ;;INTENTION in kernel.c for the full reasoning (the
// Conflict Triage Law: thread safety wins — the registries are NOT copied to
// the mailbox, the mailbox feeds the same registries, on Thread 0).

#define KERNEL_MAX_APPS     8
#define KERNEL_MAX_PROCS    8
#define KERNEL_MAX_CONSOLES 8
#define KERNEL_MAX_COMMANDLINES 8
#define KERNEL_ARENA_DEFAULT (64 * 1024 * 1024)
#define KERNEL_TRANSIENT_DEFAULT (64 * 1024 * 1024)

// Process exit codes returned by the Kernel_run dispatch entry.
#define KERNEL_EXIT_OK 0
#define KERNEL_EXIT_NO_APPS -1
// Kernel's legacy int return surface maps Process admission failure to -2.
// Use Process_run directly to distinguish admission status from callback codes.
#define KERNEL_EXIT_PROCESS_FAILED -2

// Kernel lifecycle phases (the stop/end/free split):
//   READY    — no run live; registration and end-hook firing allowed.
//   RUNNING  — a Kernel_runAll reactor is live on the arming thread.
//   DRAINING — a stop landed; the arming thread owns teardown: it stops the
//              kinds, joins every run worker, drains the mailbox, fires the
//              end hooks, then returns to READY. No registration and no
//              end-hook firing from any other thread while DRAINING.
typedef enum KernelPhase {
    KERNEL_PHASE_READY = 0,
    KERNEL_PHASE_RUNNING,
    KERNEL_PHASE_DRAINING
} KernelPhase;

typedef struct Kernel Kernel;

// Deferred-add mailbox slot (KERNEL-KINDS-DEFERRED PRIVATE HELPER sub-record —
// the Single Class Per File Law slot-record doctrine: behaviorless row owned by
// Kernel, wired entirely in kernel.c, drained on Thread 0 by the run loop's
// next pass after Kernel_run* is armed). Holds exactly one pending
// registration posted off the arming thread per the Terminal-Run Contract.
typedef enum KernelDeferredKind {
    KERNEL_DEFERRED_NONE = 0,
    KERNEL_DEFERRED_APPLICATION,
    KERNEL_DEFERRED_PROCESS,
    KERNEL_DEFERRED_CONSOLE,
    KERNEL_DEFERRED_COMMANDLINE,
} KernelDeferredKind;

typedef struct KernelDeferred {
    KernelDeferredKind kind;  // which registry the pending add targets
    void *ptr;                // Application* / Process* / Console* (as void*)
} KernelDeferred;

// Run function signature: runs on its own supervised worker thread during Kernel_runAll
typedef void (*KernelRunFn)(void *userdata);

// End function signature: invoked when Kernel_runAll completes or on Kernel_stop
typedef void (*KernelEndFn)(Kernel *self, void *userdata);

typedef struct KernelRunSlot {
    KernelRunFn fn;
    void *userdata;
    pthread_t thread;
    _Atomic bool threadLaunched;
    _Atomic bool done;
} KernelRunSlot;

typedef struct KernelEndSlot {
    KernelEndFn fn;
    void *userdata;
} KernelEndSlot;

struct Kernel {
    Lifetime lifetime;                     // Lifetime memory substrate (master + transient arenas)
    void *arena;                           // opaque master arena (provider-attested, never dereferenced)
    void *transientArena;                  // opaque scratch arena (reset, never freed mid-run)
    uint64_t arenaType;                    // provider-reported id, nonzero = attested
    uint64_t transientArenaType;           // provider-reported id, nonzero = attested
    Application *applications[KERNEL_MAX_APPS];  // windowed apps (opaque to engines)
    uint32_t applicationCount;                   // used slots in applications[]
    // The GfxLoop runner seat (the Vertical Integration Law: R1 owns fn-tables,
    // never links R3). A caller hands the demand-driven Application loop here
    // BEFORE Kernel_runApplication; a null seat falls back to the parked
    // Application_run loop, so hotcwap compiles and links standalone with zero
    // graphvex symbols (no weak externs, no Mach-O undefined-symbol traps).
    int (*gfxAppRun)(void *context, bool (*continueFn)(void*), bool (*pollFn)(void));
    Process     *processes[KERNEL_MAX_PROCS];    // one-shot invokables
    uint32_t processCount;                       // used slots in processes[]
    Console     *consoles[KERNEL_MAX_CONSOLES];  // session pumps
    uint32_t consoleCount;                       // used slots in consoles[]
    CommandLine *commandlines[KERNEL_MAX_COMMANDLINES]; // terminal applications
    uint32_t commandlineCount;                          // used slots in commandlines[]

    // --- Dynamic run-function and end-function registries ---
    KernelRunSlot *runSlots;                     // growable supervised run worker slots
    uint32_t runCount;
    uint32_t runCap;
    KernelEndSlot *endSlots;                     // growable lifecycle completion hooks
    uint32_t endCount;
    uint32_t endCap;
    _Atomic bool endHooksFired;                  // fire-once guard for end functions
    atomic_uint phase;                           // KernelPhase: READY/RUNNING/DRAINING

    // --- Terminal-run guard (wired by the Kernel_run* arming code) ---
    atomic_bool running;                         // true only while a Kernel_run* is live
    atomic_uintptr_t runThreadId;                // arming (Thread 0) id; 0 = never armed

    // --- Deferred-add mailbox (the KERNEL-KINDS-DEFERRED slot above) ---
    pthread_mutex_t addLock;                     // guards the three fields below
    KernelDeferred *deferred;                    // growable pending-add slots (cold path)
    size_t deferredCount;                        // queued entries
    size_t deferredCap;                          // allocated slots
};

// --- Overloaded constructors ---
//
//   Kernel()                           -> defaults (64MB master + 64MB transient)
//   Kernel(arenaBytes, transientBytes) -> sized arenas
//
// Arenas arrive through the spoke table as opaque handles (never
// dereferenced, never named — kernel/ includes no vexspoke headers, so
// hotcwap lives on its own). Handles carry the provider-reported type id;
// nonzero means attested, null/zero fails boot loudly (fail-closed).
// The manifest-id comparison upgrades the authority in the refresh phase.
// The Kernel struct itself is calloc-owned in Phase 1 (mirrors
// Application_0); migration to arena-owned Kernel is tracked via
// ;;INTENTION in kernel.c per the Conflict Triage Law.
Kernel *Kernel_0(void);
Kernel *Kernel_1(size_t arenaBytes);
Kernel *Kernel_2(size_t arenaBytes, size_t transientBytes);

#define KERNEL_CHOOSER(_0, _1, _2, NAME, ...) NAME

#define Kernel(...) KERNEL_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Kernel_2, Kernel_1, Kernel_0 \
)(__VA_ARGS__)

// Free supervision AFTER all kinds and windows are gone.
// Guarded (the Teardown Order Law): returns false plus an stderr warn while any process,
// application, or console is still registered — remove them first, then
// retry. Frees the transient arena, then the master arena LAST, then the
// Kernel struct. Never call while a loop/worker is still active (the Bounded Wait Law).
bool Kernel_free(Kernel *self);

// Legacy shim over Kernel_free: kept so existing callers link without edits.
// Warns-and-leaks (returns void) when the registry is non-empty instead of
// force-clearing it — prefer Kernel_free and check the result.
void Kernel_destroy(Kernel *self);

// --- Supervisor state ---
// Ends a live Kernel_runAll run: transitions RUNNING -> DRAINING, stops every
// registered app, cancels every console session (SIGTERM + cancel flag), and
// clears running so the reactor exits. Teardown ownership stays with the
// arming thread: it joins the run workers, drains the mailbox, fires the end
// hooks, and only then returns to READY. Called while READY (no run live) it
// fires the end hooks directly (fire-once). Called while DRAINING it is a
// no-op — the arming thread already owns teardown. Safe from any thread.
void Kernel_stop(Kernel *self);

// Current lifecycle phase (KernelPhase). Snapshot semantics: DRAINING may
// already have returned to READY by the time the caller acts on it.
KernelPhase Kernel_getPhase(const Kernel *self);

// True while a Kernel_run* entry is currently live on some thread (armed and
// not yet disarmed). False before the first run and after Kernel_stop/run
// exit. The Terminal-Run Contract: while true, the arming thread's add calls
// are refused; other threads' adds land in the deferred mailbox.
bool Kernel_isRunning(const Kernel *self);

// --- Work dispatch (SOLE blocking entry, arity-overloaded) ---
//
// The Kernel forwards work, it never does it. Each overload relays to the
// registered kind's own run function — the Kernel owns zero loops:
//   Kernel_run(kernel)                       -> run ALL registered kinds
//   Kernel_run(kernel, Process *)            -> Kernel_runProcess (one-shot)
//   Kernel_run(kernel, Console *)            -> Kernel_runConsole (session start)
//   Kernel_run(kernel, Application *)        -> Kernel_runApplication (GfxLoop attach)
//
// The 1-arity Kernel_run(kernel) is the completion reactor (the user model):
//
//     Kernel *k  = Kernel();
//     Console *c = Console("/bin/bash");
//     Application *a = Application("vex");
//     Kernel_addConsole(k, c);
//     Kernel_addApplication(k, a);
//     Kernel_run(k);             // runs until ALL kinds are done
//
// It runs every registered Process to completion once, starts every Console
// session pump, starts every Application and shows its windows, then
// SUPERVISES until every kind reports done (see Application_isFinished /
// Console_isRunning): drain deferred adds -> Window_pollEvents -> bounded
// Console_poll slices -> ask app closed-state at a 250ms cadence -> park.
// Kernel_stop() ends the run; Kernel_run returns the first non-zero Process
// exit seen, or KERNEL_EXIT_OK. It returns KERNEL_EXIT_NO_APPS when nothing
// is registered. The 2-arity forms stay blocking/non-supervising per kind.
int  Kernel_runAll(Kernel *self);
int  Kernel_runProcess(Kernel *self, Process *p);
bool Kernel_runConsole(Kernel *self, Console *c);
int  Kernel_runApplication(Kernel *self, Application *a);
// Blocking terminal-application loop for one CommandLine (the 2-arity form).
int  Kernel_runCommandLine(Kernel *self, CommandLine *c);

#define KERNEL_RUN_CHOOSER(_0, _1, _2, NAME, ...) NAME

#define KERNEL_RUN_DISPATCH(self, kind)                    \
    _Generic((kind),                                       \
        Process     *: Kernel_runProcess,                  \
        Console     *: Kernel_runConsole,                  \
        Application *: Kernel_runApplication,              \
        CommandLine *: Kernel_runCommandLine               \
    )(self, kind)

#define Kernel_run(...) KERNEL_RUN_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    KERNEL_RUN_DISPATCH, Kernel_runAll \
)(__VA_ARGS__)

// --- Application registry (multi-app, N apps x M windows per process) ---
// Register a live application. False on NULL, duplicate, or full registry.
bool Kernel_addApplication(Kernel *self, Application *app);
// Unregister an application (swap-remove, order not preserved). False if absent.
// Registered windows stay owned by the Application (detach-only, OS-owned).
bool Kernel_removeApplication(Kernel *self, Application *app);
// Application at index, or NULL when out of range.
Application *Kernel_getApplication(const Kernel *self, uint32_t index);

// --- GfxLoop runner seat (registered, never linked) ---
// Install the demand-driven Application loop (graphvex's frame scheduler) into
// the seat. Null-safe; passing NULL clears the seat (fallback parked loop).
void Kernel_setGfxAppRunner(Kernel *self, int (*fn)(void *context, bool (*continueFn)(void*), bool (*pollFn)(void)));
// The installed runner, or NULL when unset.
int (*Kernel_getGfxAppRunner(const Kernel *self))(void *context, bool (*continueFn)(void*), bool (*pollFn)(void));
// Number of registered applications.
uint32_t Kernel_getApplicationCount(const Kernel *self);
// Copy registry into out[] (up to cap), returns entries written.
uint32_t Kernel_getApplications(const Kernel *self, Application **out, uint32_t cap);

// --- Process registry (one-shot invokables) ---
bool    Kernel_addProcess(Kernel *self, Process *p);
bool    Kernel_removeProcess(Kernel *self, Process *p);
Process *Kernel_getProcess(const Kernel *self, uint32_t index);
uint32_t Kernel_getProcessCount(const Kernel *self);
uint32_t Kernel_getProcesses(const Kernel *self, Process **out, uint32_t cap);

// --- Console registry (session pumps) ---
bool    Kernel_addConsole(Kernel *self, Console *c);
bool    Kernel_removeConsole(Kernel *self, Console *c);
Console *Kernel_getConsole(const Kernel *self, uint32_t index);
uint32_t Kernel_getConsoleCount(const Kernel *self);
uint32_t Kernel_getConsoles(const Kernel *self, Console **out, uint32_t cap);

// --- CommandLine registry (terminal applications) ---
bool         Kernel_addCommandLine(Kernel *self, CommandLine *c);
bool         Kernel_removeCommandLine(Kernel *self, CommandLine *c);
CommandLine *Kernel_getCommandLine(const Kernel *self, uint32_t index);
uint32_t     Kernel_getCommandLineCount(const Kernel *self);
uint32_t     Kernel_getCommandLines(const Kernel *self, CommandLine **out, uint32_t cap);

// --- Run functions (async background execution) & End functions (lifecycle completion) ---
bool     Kernel_addRunFunction(Kernel *self, KernelRunFn fn, void *userdata);
bool     Kernel_addEndFunction(Kernel *self, KernelEndFn fn, void *userdata);
uint32_t Kernel_getRunFunctionCount(const Kernel *self);
uint32_t Kernel_getEndFunctionCount(const Kernel *self);

// --- Arena access (the Symmetric Getter/Setter Completeness Law symmetric getters) ---
void *Kernel_getArena(const Kernel *self);
void *Kernel_getTransientArena(const Kernel *self);
Lifetime *Kernel_getLifetime(Kernel *self);
const Lifetime *Kernel_lifetime(const Kernel *self);

#endif
