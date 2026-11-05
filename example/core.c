// core.c — THE CORE IMAGE (R2–R4: vexspoke + graphvex + darling).
//
// This is the lower layer. It is swapped on its own ladder (`bin/current/core`)
// and it publishes a CONTRACT ABI: a number that says "which core surface am
// I?" The app layer must match it or it is not allowed to run.
//
// Build:
//   -DCORE_CONTRACT_ABI=1   the v1 engine surface
//   -DCORE_CONTRACT_ABI=2   the v2 engine surface (pulse math changed)
//
// Bumping CORE_CONTRACT_ABI is the promise "an app built for the old surface
// will no longer be attached". That is the whole point of the gate.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CORE_CONTRACT_ABI
#define CORE_CONTRACT_ABI 1
#endif

// ---------------------------------------------------------------------------
// CORE STATE — carried across a core swap by Hot_save / Hot_restore.
// ---------------------------------------------------------------------------

#define CORE_MAGIC  0x434f5245u   // "CORE"
#define CORE_SCHEMA 1u

typedef struct CoreState {
    uint32_t magic;
    uint32_t schema;
    uint64_t pulses;      // how many times the app asked for a pulse
} CoreState;

static CoreState s_core = { CORE_MAGIC, CORE_SCHEMA, 0 };

// ---------------------------------------------------------------------------
// THE SURFACE THE APP CALLS.
//
// The app never links against this dylib. The host resolves Core_pulse and
// hands the pointer to the app at attach time — so when core swaps, the host
// re-resolves and re-attaches, and the app never holds a stale address.
// ---------------------------------------------------------------------------

float Core_pulse(double nowSeconds);
uint64_t Core_calls(void);

float Core_pulse(double nowSeconds) {
    s_core.pulses += 1;

#if CORE_CONTRACT_ABI >= 2
    return (float) (nowSeconds * 4.0);   // v2: twice the rate
#else
    return (float) (nowSeconds * 2.0);   // v1
#endif
}

uint64_t Core_calls(void) {
    return s_core.pulses;
}

// ---------------------------------------------------------------------------
// CONTRACT ANNOUNCEMENT — the number the app gate compares against.
// ---------------------------------------------------------------------------

uint64_t Hot_contract_abi(void);
uint64_t Hot_contract_abi(void) {
    return CORE_CONTRACT_ABI;
}

// ---------------------------------------------------------------------------
// STATE HANDOFF
// ---------------------------------------------------------------------------

bool Hot_save(void *buf, size_t cap, size_t *outLen);
bool Hot_restore(const void *buf, size_t len);

bool Hot_save(void *buf, size_t cap, size_t *outLen) {
    if (outLen)
        *outLen = 0;
    if (!buf || !outLen || cap < sizeof(CoreState))
        return false;
    memcpy(buf, &s_core, sizeof(CoreState));
    *outLen = sizeof(CoreState);
    return true;
}

bool Hot_restore(const void *buf, size_t len) {
    if (!buf || len != sizeof(CoreState))
        return false;
    CoreState in;
    memcpy(&in, buf, sizeof(in));
    if (in.magic != CORE_MAGIC || in.schema != CORE_SCHEMA)
        return false;
    s_core = in;
    return true;
}

// ---------------------------------------------------------------------------
// EXPORT TABLE
// ---------------------------------------------------------------------------

typedef struct HotModuleExport {
    const char *name;
    void *fn;
} HotModuleExport;

static const HotModuleExport s_exports[] = {
    { "Core_pulse",         (void*) Core_pulse },
    { "Core_calls",         (void*) Core_calls },
    { "Hot_contract_abi",   (void*) Hot_contract_abi },
    { "Hot_save",           (void*) Hot_save },
    { "Hot_restore",        (void*) Hot_restore },
};

const void *Hot_getExports(uint32_t *outCount);
const void *Hot_getExports(uint32_t *outCount) {
    if (outCount)
        *outCount = (uint32_t) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}
