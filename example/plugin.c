// plugin.c — an INDEPENDENT image (R5–R6 add-on).
//
// A plugin declares no core contract and is never gated. It loads on its own
// ladder, swaps on its own generation, and its absence never blocks the core
// or the application.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PLUGIN_MAGIC  0x504c5547u   // "PLUG"
#define PLUGIN_SCHEMA 1u

typedef struct PluginState {
    u32 magic;
    u32 schema;
    u64 ticks;
} PluginState;

static PluginState s_plugin = { PLUGIN_MAGIC, PLUGIN_SCHEMA, 0 };

void Plugin_frame(d64 nowSeconds);
bool Hot_save(void *buf, bytes cap, bytes *outLen);
bool Hot_restore(const void *buf, bytes len);

void Plugin_frame(d64 nowSeconds) {
    s_plugin.ticks += 1;
    if ((s_plugin.ticks % 40u) == 1u)
        printf("[plugin] tick %llu  t=%.2f\n",
               (unsigned long long) s_plugin.ticks, nowSeconds);
}

bool Hot_save(void *buf, bytes cap, bytes *outLen) {
    if (outLen)
        *outLen = 0;
    if (!buf || !outLen || cap < sizeof(PluginState))
        return false;
    memcpy(buf, &s_plugin, sizeof(PluginState));
    *outLen = sizeof(PluginState);
    return true;
}

bool Hot_restore(const void *buf, bytes len) {
    if (!buf || len != sizeof(PluginState))
        return false;
    PluginState in;
    memcpy(&in, buf, sizeof(in));
    if (in.magic != PLUGIN_MAGIC || in.schema != PLUGIN_SCHEMA)
        return false;
    s_plugin = in;
    return true;
}

typedef struct HotModuleExport {
    const char *name;
    void *fn;
} HotModuleExport;

static const HotModuleExport s_exports[] = {
    { "Plugin_frame", (void*) Plugin_frame },
    { "Hot_save",     (void*) Hot_save },
    { "Hot_restore",  (void*) Hot_restore },
};

const void *Hot_getExports(u32 *outCount);
const void *Hot_getExports(u32 *outCount) {
    if (outCount)
        *outCount = (u32) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}
