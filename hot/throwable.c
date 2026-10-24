#include "hot/throwable.h"
#include "window/window.h"
#include "struct/chunked_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Throwable
 * ============================================================================
 * Process-level panic, exception handling, and emergency teardown subsystem.
 * Manages dynamically registered termination callbacks through unconstrained
 * ChunkedList storage conforming to the Emergency Teardown Law, guaranteeing
 * windows and OS handles are cleanly destroyed before fatal process termination.
 *
 * Implements FATAL_THROW for fail-closed fatal error termination with formatted
 * diagnostic output and LIFO teardown execution, alongside TRY for non-fatal
 * diagnostic warnings and graceful execution recovery.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Throwable (hot/throwable.c)
 * ============================================================================
 * SUMMARY:
 *   Process-level panic, exception reporting, and emergency teardown registry.
 *   Provides LIFO teardown dispatch across arbitrary registered destruction hooks
 *   (including Window_destroyAll) prior to process exit on FATAL_THROW assertions.
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   (none — module-level static teardown list)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   - Throwable_ensureDefaultTeardown(void)   : registers default Window_destroyAll hook
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - (none)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Throwable_registerTeardown(fn)          : register teardown callback
 *   - Throwable_unregisterTeardown(fn)        : unregister teardown callback
 *   - Throwable_runTeardown(void)             : execute all teardowns in LIFO order
 *   - Throwable_throw(cat, site, file, line, msg, fmt, ...) : fatal exception exit
 *   - Throwable_tryCatch(cond, cat, site, file, line, msg, fmt, ...) : non-fatal check
 *
 * Private Core Functions: (.c static)
 *   - Throwable_ensureDefaultTeardown(void)   : lazy-init default teardown hooks
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - (none)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// R1 Host wrapper around R2 ChunkedList for teardown hooks.
// Encapsulates R2 ChunkedList storage behind R1's Throwable_* API.
// Conforms strictly to Preference 47 (No Artificial Limits) and the Vertical Integration Law:
// R1 owns the lifetime and policy, using R2's never-moved chunk storage underneath.
static ChunkedList *s_teardownList = nullptr;
static bool s_defaultTeardownRegistered = false;

// CONSTRUCTORS (PUBLIC & PRIVATE)

// CORE FUNCTIONS (PUBLIC & PRIVATE)

static void Throwable_ensureDefaultTeardown(void) {
    if (!s_defaultTeardownRegistered) {
        s_defaultTeardownRegistered = true;
        Throwable_registerTeardown(Window_destroyAll);
    }
}

/** Registers a teardown callback for the process-level throwable shutdown path. */
void Throwable_registerTeardown(ThrowableTeardownFn fn) {
    if (fn == nullptr) return;

    if (s_teardownList == nullptr) {
        s_teardownList = ChunkedList_3(ID_BIT64, sizeof(ThrowableTeardownFn), VEX_CHUNKED_BYTES_DEFAULT);
        if (s_teardownList == nullptr) return;
    }

    // Deduplicate: check if already registered
    uint32_t count = ChunkedList_size(s_teardownList);
    for (uint32_t i = 0; i < count; i++) {
        ThrowableTeardownFn *slot = (ThrowableTeardownFn*) ChunkedList_slot(s_teardownList, i);
        if (slot != nullptr && *slot == fn) {
            return; // Already registered
        }
    }

    // Allocate next never-moved slot in ChunkedList
    ThrowableTeardownFn *slot = (ThrowableTeardownFn*) ChunkedList_addSlot(s_teardownList);
    if (slot != nullptr) {
        *slot = fn;
    }
}

/** Removes a previously registered teardown callback. */
void Throwable_unregisterTeardown(ThrowableTeardownFn fn) {
    if (fn == nullptr || s_teardownList == nullptr) return;

    uint32_t count = ChunkedList_size(s_teardownList);
    for (uint32_t i = 0; i < count; i++) {
        ThrowableTeardownFn *slot = (ThrowableTeardownFn*) ChunkedList_slot(s_teardownList, i);
        if (slot != nullptr && *slot == fn) {
            *slot = nullptr; // Tombstone without disturbing other slots
            return;
        }
    }
}

/** Runs the registered teardown callbacks once in their registered order. */
void Throwable_runTeardown(void) {
    Throwable_ensureDefaultTeardown();
    if (s_teardownList != nullptr) {
        uint32_t count = ChunkedList_size(s_teardownList);
        // Execute teardown callbacks in LIFO order (last registered first)
        for (uint32_t i = count; i > 0; i--) {
            ThrowableTeardownFn *slot = (ThrowableTeardownFn*) ChunkedList_slot(s_teardownList, i - 1);
            if (slot != nullptr && *slot != nullptr) {
                ThrowableTeardownFn fn = *slot;
                *slot = nullptr; // Prevent double invocation
                fn();
            }
        }
        ChunkedList_free(s_teardownList);
        s_teardownList = nullptr;
    }
    s_defaultTeardownRegistered = false;
}

_Noreturn void Throwable_throw(ExceptionCategory category,
                              const char *site,
                              const char *file,
                              int line,
                              const char *msg,
                              const char *details_fmt,
                              ...) {
    Exception ex = Exception(category, site, file, line, "%s", msg ? msg : "");

    if (details_fmt != nullptr && details_fmt[0] != '\0') {
        va_list args;
        va_start(args, details_fmt);
        Exception_setDetailsV(&ex, details_fmt, args);
        va_end(args);
    }

    // Print full Java-style diagnostic banner
    Exception_print(&ex);
    Exception_free(&ex);

    // Emergency Teardown Law: cleanly close all windows before dying
    Throwable_runTeardown();

    fflush(stdout);
    fflush(stderr);
    exit(1);
}

bool Throwable_tryCatch(bool condition,
                        ExceptionCategory category,
                        const char *site,
                        const char *file,
                        int line,
                        const char *msg,
                        const char *details_fmt,
                        ...) {
    if (condition) {
        return true;
    }

    Exception ex = Exception(category, site, file, line, "%s", msg ? msg : "");

    if (details_fmt != nullptr && details_fmt[0] != '\0') {
        va_list args;
        va_start(args, details_fmt);
        Exception_setDetailsV(&ex, details_fmt, args);
        va_end(args);
    }

    fprintf(stderr, "\n[CAUGHT RUNTIME EXCEPTION] at %s (%s:%d): %s\n",
            site ? site : "unknown",
            file ? file : "unknown",
            line,
            msg ? msg : "");
    fprintf(stderr, "  Category: %s\n", Exception_categoryName(category));
    if (ex.details != nullptr && ex.details[0] != '\0') {
        fprintf(stderr, "  Details:  %s\n", ex.details);
    }
    fprintf(stderr, "  -> Recovery: Grace given, execution continuing.\n\n");
    fflush(stderr);

    Exception_free(&ex);
    return false;
}

// SETTERS (PUBLIC & PRIVATE)

// GETTERS (PUBLIC & PRIVATE)
