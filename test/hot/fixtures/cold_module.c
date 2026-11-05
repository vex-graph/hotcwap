/* Stateless export table plus module-local state for cold transaction fixtures.
 * Candidate restore touches only candidate state; old state remains untouched. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
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
const uint64_t Hot_cold_abi = ABI;
static int state;
static int version(void) { return VERSION; }
static int get(void) { return state; }
static void set(int value) { state = value; }
bool Hot_save(void *dest, size_t cap, size_t *length) {
    if (!dest || !length || cap < sizeof(state))
        return false;
    memcpy(dest, &state, sizeof(state));
    *length = sizeof(state);
    return true;
}
bool Hot_restore(const void *source, size_t length) {
    if (!RESTORE_OK || !source || length != sizeof(state))
        return false;
    memcpy(&state, source, sizeof(state));
    return true;
}
const void *Hot_getExports(uint32_t *count) {
    static const struct { const char *name; void *fn; } exports[] = {
        {PREFIX "_version", (void*) version}, {PREFIX "_state", (void*) get}, {PREFIX "_set", (void*) set}
    };
    *count = 3;
    return exports;
}
