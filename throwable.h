#ifndef HOT_THROWABLE_H
#define HOT_THROWABLE_H

#include <stdbool.h>
#include <stddef.h>
#include "exception/exception.h"

// Teardown hook invoked when FATAL_THROW terminates the process.
typedef void (*ThrowableTeardownFn)(void);

// Register a teardown callback (e.g. Window_destroyAll) to execute before exit.
// Dynamically scales 0..N with zero hardcoded capacity ceiling.
void Throwable_registerTeardown(ThrowableTeardownFn fn);

// Unregister a previously registered teardown callback.
void Throwable_unregisterTeardown(ThrowableTeardownFn fn);

// Run all registered teardowns in LIFO order and free dynamic storage.
void Throwable_runTeardown(void);

// Internal throw execution: builds Exception, prints banner, runs teardown, exits.
_Noreturn void Throwable_throw(ExceptionCategory category,
                              const char *site,
                              const char *file,
                              int line,
                              const char *msg,
                              const char *details_fmt,
                              ...);

// Internal try/catch check: if condition is false, logs graceful warning and returns false.
bool Throwable_tryCatch(bool condition,
                        ExceptionCategory category,
                        const char *site,
                        const char *file,
                        int line,
                        const char *msg,
                        const char *details_fmt,
                        ...);

// Fatal termination; distinct from vexspoke's recoverable THROW.
#define FATAL_THROW(msg, site, ...) \
    Throwable_throw(EXCEPTION_RUNTIME, (site), __FILE__, __LINE__, (msg), "" __VA_OPT__(__VA_ARGS__))

#define FATAL_THROW_EX(category, msg, site, ...) \
    Throwable_throw((category), (site), __FILE__, __LINE__, (msg), "" __VA_OPT__(__VA_ARGS__))

// TRY macro: tests expression condition; on failure logs caught exception and returns false.
#define TRY(expr, msg, site, ...) \
    Throwable_tryCatch((bool)(expr), EXCEPTION_RUNTIME, (site), __FILE__, __LINE__, (msg), "" __VA_OPT__(__VA_ARGS__))

#define TRY_EX(expr, category, msg, site, ...) \
    Throwable_tryCatch((bool)(expr), (category), (site), __FILE__, __LINE__, (msg), "" __VA_OPT__(__VA_ARGS__))

#endif // HOT_THROWABLE_H
