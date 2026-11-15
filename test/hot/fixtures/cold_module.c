/* Stateless export table plus module-local state for cold transaction fixtures.
 * Candidate restore touches only candidate state; old state remains untouched. */

#include "vex.h"
#include <string.h>
#ifndef VERSION
#define VERSION 1
#endif
#ifndef ABI
#define ABI 1
#endif
#ifndef RESTORE_OK
#define RESTORE_OK 1
#endif
#ifndef PREFIX
#define PREFIX "one"
#endif
const u64 Hot_cold_abi = ABI;
static int state;
static int version() { return VERSION; }
static int get() { return state; }
static fn set(int value) { state = value; }
bool Hot_save(any dest, bytes cap, bytes *length) {
    if (!dest || !length || cap < sizeof(state))
        return false;
    memcpy(dest, &state, sizeof(state));
    *length = sizeof(state);
    return true;
}
bool Hot_restore(const_any source, bytes length) {
    if (!RESTORE_OK || !source || length != sizeof(state))
        return false;
    memcpy(&state, source, sizeof(state));
    return true;
}
const_any Hot_getExports(u32 *count) {
    static const struct { const char *name; any fn; } exports[] = {
        {PREFIX "_version", (any) version}, {PREFIX "_state", (any) get}, {PREFIX "_set", (any) set}
    };
    *count = 3;
    return exports;
}
