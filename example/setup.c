// setup.c — the ENTIRE hot-reload integration for this application.
//
// No loader state, no library structs, no ordering rules: hotcwap's HotStack
// owns all of that. This file only says what the application *is*:
//
//   core    the engine image        Hot_loadCore()
//   app     the application image   Hot_loadApplication()
//   plugin  an add-on image         Hot_loadPlugin()
//
// and then calls one function per frame.
//
// Run:
//   ./setup <core-v2-dir> <app-v2-dir> <app-v3-dir> <core-v1-dir> <app-v1-dir>

#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "hot.h"
#include "hot/stack.h"
#include "manifest.h"

// Hardcoded install root — every segment a literal, no HOME, no defaults.
#define EX_ROOT  "/"
#define EX_TICKS 200
#define EX_TICK_MS 10

#define T_CORE   60    // core v2 promoted alone
#define T_APP    90    // matching app v2 promoted
#define T_BADAPP 130   // app v3 wants a contract that does not exist

// The application's own entry points — the only types that are its policy.
typedef float (*CorePulseFn)(double);
typedef void  (*AppSetupFn)(CorePulseFn);
typedef void  (*AppRunFn)(double);
typedef void  (*AppTeardownFn)(void);
typedef void  (*PluginFrameFn)(double);

static volatile sig_atomic_t s_stop = 0;

static void onSignal(int sig) {
    (void) sig;
    s_stop = 1;
}

static double hostNow(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
}

static void sleepMs(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
}

int main(int argc, char **argv) {
    const char *coreV2 = argc > 1 ? argv[1] : nullptr;
    const char *appV2  = argc > 2 ? argv[2] : nullptr;
    const char *appV3  = argc > 3 ? argv[3] : nullptr;
    const char *coreV1 = argc > 4 ? argv[4] : "build/core/v1";
    const char *appV1  = argc > 5 ? argv[5] : "build/app/v1";

    signal(SIGINT, onSignal);

    // --- mount the ladder (install concerns, not loading concerns) ---------
    if (!MANIFEST(EX_ROOT, "Users", "vexgraph", "~v", "worktree", "vex",
              "hotcwap", "example", "install")) {
        fprintf(stderr, "[setup] MANIFEST() failed\n");
        return 1;
    }
    if (!MANIFEST_LIBRARY("core", "app", "plugin", (const char*) nullptr)) {
        fprintf(stderr, "[setup] MANIFEST_LIBRARY() failed\n");
        return 1;
    }
    if (!MANIFEST_ENSURE()) {
        fprintf(stderr, "[setup] MANIFEST_ENSURE() failed\n");
        return 1;
    }
    if (MANIFEST_GENERATION("core") == 0 && !MANIFEST_REFLECT("core", coreV1))
        return 1;
    if (MANIFEST_GENERATION("app") == 0 && !MANIFEST_REFLECT("app", appV1))
        return 1;
    if (MANIFEST_GENERATION("plugin") == 0 &&
        !MANIFEST_REFLECT("plugin", "build/plugin/v1"))
        return 1;

    // --- THE WHOLE INTEGRATION ---------------------------------------------
    HotStack *stack = HotStack();

    if (Hot_loadCore(stack) != HOT_OK) {
        fprintf(stderr, "[setup] core: %s\n", Hot_getStackError(stack));
        HotStack_free(stack);
        return 1;
    }
    printf("[setup] core loaded, contract %llu\n",
           (unsigned long long) Hot_getCoreContract(stack));

    if (Hot_loadApplication(stack) != HOT_OK)
        printf("[setup] app not running: %s\n", Hot_getStackError(stack));

    if (Hot_loadPlugin(stack, "plugin") != HOT_OK)
        printf("[setup] plugin not running: %s\n", Hot_getStackError(stack));
    else
        printf("[setup] plugin loaded\n");

    // Application-visible symbols. Re-fetched whenever the matching layer moves.
    CorePulseFn corePulse = (void*) Hot_getSymbol(Hot_getCore(stack), "Core_pulse");
    AppRunFn appRun = (void*) Hot_getSymbol(Hot_getApplication(stack), "run");
    PluginFrameFn pluginFrame = (void*) Hot_getSymbol(Hot_getPlugin(stack, 0), "Plugin_frame");
    bool appRunning = Hot_isApplicationCompatible(stack);

    if (appRunning && corePulse) {
        AppSetupFn setup = (AppSetupFn) (void*) Hot_getSymbol(Hot_getApplication(stack), "setup");
        if (setup)
            setup(corePulse);
    }
    printf("\n");

    double t0 = hostNow();
    bool didCore = false, didApp = false, didBad = false;

    for (int tick = 0; tick < EX_TICKS && !s_stop; tick++) {
        double now = hostNow() - t0;

        // The promotion schedule stands in for a downloader. One verb each.
        if (!didCore && coreV2 && tick == T_CORE) {
            printf("\n[setup] stage + promote core v2\n");
            if (MANIFEST_UPDATE("core", coreV2))
                MANIFEST_PROMOTE_LIBRARY("core");
            didCore = true;
        }
        if (!didApp && appV2 && tick == T_APP) {
            printf("\n[setup] stage + promote app v2\n");
            if (MANIFEST_UPDATE("app", appV2))
                MANIFEST_PROMOTE_LIBRARY("app");
            didApp = true;
        }
        if (!didBad && appV3 && tick == T_BADAPP) {
            printf("\n[setup] stage + promote app v3 (wants a contract that does not exist)\n");
            if (MANIFEST_UPDATE("app", appV3))
                MANIFEST_PROMOTE_LIBRARY("app");
            didBad = true;
        }

        // ONE CALL. Core, then app, then plugins — order and gate belong to
        // the library.
        HotStackChanged changed;
        Hot_pollAll(stack, &changed);

        if (changed.core || changed.app) {
            // Re-bind borrowed pointers. This is the only thing the
            // application must own: it knows its own contract with the engine.
            corePulse = (CorePulseFn) (void*) Hot_getSymbol(Hot_getCore(stack), "Core_pulse");
            appRun = (AppRunFn) (void*) Hot_getSymbol(Hot_getApplication(stack), "run");
            appRunning = Hot_isApplicationCompatible(stack);

            if (changed.core)
                printf("[setup] core moved -> contract %llu\n",
                       (unsigned long long) Hot_getCoreContract(stack));

            if (!appRunning) {
                printf("[setup] app idle: %s\n", Hot_getStackError(stack));
            } else if (corePulse) {
                AppSetupFn setup = (AppSetupFn) (void*) Hot_getSymbol(Hot_getApplication(stack), "setup");
                if (setup) {
                    printf("[setup] app re-attached to the new core\n");
                    setup(corePulse);
                }
            }
        }

        // --- per frame ---------------------------------------------------
        if (appRunning && appRun)
            appRun(now);
        else if (tick % 20 == 0)
            printf("[setup] frame %-4d core-only\n", tick);

        if (pluginFrame)
            pluginFrame(now);

        sleepMs(EX_TICK_MS);
    }

    HotStack_free(stack);
    printf("\n[setup] done\n");
    return 0;
}
