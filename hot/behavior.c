#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Hot_behavior
 * ============================================================================
 * Dynamically reloadable calculation module implementing animated pulse math,
 * status bar dimensions, and asset paths. Operates strictly stateless on the GPU
 * (no Vulkan or UI panel handles cross the module boundary) to ensure hot swaps
 * can occur with zero dangling GPU command buffers or pointer corruption.
 *
 * Exposes versioned state save and restore hooks (schema v3 with ownership magic
 * HOT_BEHAVIOR_SCHEMA_MAGIC) to preserve animation continuity across swaps,
 * alongside backwards schema migration and automated rollback support.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Hot_behavior (hot/hot_behavior.c)
 * ============================================================================
 * SUMMARY:
 *   Phase-2 L2/L3 behavior subject: pure pulse/bar math + texture path.
 *
 *   Deliberately stateless-on-GPU: this module NEVER calls Vk_* or touches
 *   the Panel tree. Host handlers (_main/test_suite.c, _tests/hotcwap/) own
 *   cmdBuffer + Panel pointers and delegate only the math here. That keeps swap safe by
 *   construction — no code pointers cross the dylib boundary, no dangling
 *   renderHandler after dlclose.
 *
 *   State schema (versioned for L3 migration + rollback validation):
 *     v1 (1.0.0): [phaseBias f32][modeShadow i32] = 8 Bytes
 *     v2 (1.1.0): [phaseBias f32][modeShadow i32][glowStrength f32] = 12 Bytes
 *     v3 (1.2.0): [schemaMagic u32][phaseBias f32][modeShadow i32][glowStrength f32] = 16 Bytes
 *   Hot_save emits v3 blobs carrying HOT_BEHAVIOR_SCHEMA_MAGIC as an ownership
 *   tag. Hot_restore adopts a v3 blob only when its magic matches the current
 *   build (a foreign magic returns false), wraps legacy v1/v2 sizes into the
 *   current schema, and rejects unknown lengths. The loader (hot/hot.c) treats
 *   a restore rejection as Automated State Rollback: it keeps the previous
 *   generation live and never advances the stamp.
 *   Hot_migrate carries v1/v2 to v3 forward (glow defaults to 1.0).
 *
 * STRUCT FIELDS: none — procedural/stateless (operates on HotModule via Hot_* module exports)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Hot_init_module(void)                   : Module initialization export
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Hot_shutdown_module(void)               : Module teardown export
 *   - Hot_save(buf, cap, outLen)              : Serialize module state blob
 *   - Hot_restore(buf, len)                   : Rehydrate module state blob
 *   - Hot_migrate(oldVer, oldB, l, newB, c, outL) : Forward schema migration
 *   - Hot_manifest(void)                      : JSON descriptor string export
 *   - Hot_getExports(outCount)        : Exported trampoline table
 *   - hot_behavior_pulse(nowSeconds)          : Trigonometric pulse math
 *   - hot_behavior_bar(w, h, pulse, outH, outW) : Layout bar dimension math
 *   - hot_texture_path(void)                  : Relative texture asset path
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - hot_behavior_set_phase_bias(value)      : Set phase bias offset
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - hot_behavior_get_phase_bias(void)       : Query phase bias offset
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

#define HOT_BEHAVIOR_VERSION "1.2.0"
#define HOT_BEHAVIOR_V1_SIZE 8
#define HOT_BEHAVIOR_V2_SIZE 12
#define HOT_BEHAVIOR_V3_SIZE 16

#ifndef HOT_BEHAVIOR_SCHEMA_MAGIC
#define HOT_BEHAVIOR_SCHEMA_MAGIC 0x56455842u   // "VEXB" — v3 state-blob ownership tag
#endif

static f32 s_phaseBias = 0.0f;
static i32 s_modeShadow = 0;
static f32 s_glowStrength = 1.0f;

// CONSTRUCTORS (PUBLIC & PRIVATE)

/** Initializes the stateless module and reports success. */
bool Hot_init_module(void) {
    return true;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)

/** Performs module teardown; this module currently owns no external resources. */
void Hot_shutdown_module(void) {
    return;
}

/** Writes the current v3 state fields to the caller's buffer. */
bool Hot_save(void *buf, bytes cap, bytes *outLen) {
    if (!buf || !outLen)
        return false;
    if (cap < HOT_BEHAVIOR_V3_SIZE)
        return false;
    u8 *b = (u8*) buf;
    const u32 magic = HOT_BEHAVIOR_SCHEMA_MAGIC;
    memcpy(b, &magic, 4);
    memcpy(b + 4, &s_phaseBias, 4);
    memcpy(b + 8, &s_modeShadow, 4);
    memcpy(b + 12, &s_glowStrength, 4);
    (*outLen) = HOT_BEHAVIOR_V3_SIZE;
    return true;
}

/** Restores recognized v1/v2 state or a v3 blob with the expected schema magic. */
bool Hot_restore(const void *buf, bytes len) {
    if (!buf)
        return false;
    const u8 *b = (const u8*) buf;
    if (len == HOT_BEHAVIOR_V1_SIZE) {
        memcpy(&s_phaseBias, b, 4);
        memcpy(&s_modeShadow, b + 4, 4);
        s_glowStrength = 1.0f;
        return true;
    }
    if (len == HOT_BEHAVIOR_V2_SIZE) {
        memcpy(&s_phaseBias, b, 4);
        memcpy(&s_modeShadow, b + 4, 4);
        memcpy(&s_glowStrength, b + 8, 4);
        return true;
    }
    if (len != HOT_BEHAVIOR_V3_SIZE)
        return false;
    u32 magic = 0;
    memcpy(&magic, b, 4);
    if (magic != HOT_BEHAVIOR_SCHEMA_MAGIC)
        return false;
    memcpy(&s_phaseBias, b + 4, 4);
    memcpy(&s_modeShadow, b + 8, 4);
    memcpy(&s_glowStrength, b + 12, 4);
    return true;
}

/** Converts supported legacy state versions to v3, or copies a fitting unknown version. */
bool Hot_migrate(const char *oldVersion, const void *oldBuf, bytes oldLen,
                 void *newBuf, bytes newCap, bytes *outLen) {
    if (!oldVersion || !oldBuf || !newBuf || !outLen)
        return false;
    const u32 magic = HOT_BEHAVIOR_SCHEMA_MAGIC;
    if (strcmp(oldVersion, "1.0.0") == 0 && oldLen == HOT_BEHAVIOR_V1_SIZE) {
        if (newCap < HOT_BEHAVIOR_V3_SIZE)
            return false;
        memcpy(newBuf, &magic, 4);
        memcpy((u8*) newBuf + 4, oldBuf, HOT_BEHAVIOR_V1_SIZE);
        f32 glow = 1.0f;
        memcpy((u8*) newBuf + HOT_BEHAVIOR_V1_SIZE + 4, &glow, 4);
        (*outLen) = HOT_BEHAVIOR_V3_SIZE;
        return true;
    }
    if (strcmp(oldVersion, "1.1.0") == 0 && oldLen == HOT_BEHAVIOR_V2_SIZE) {
        if (newCap < HOT_BEHAVIOR_V3_SIZE)
            return false;
        memcpy(newBuf, &magic, 4);
        memcpy((u8*) newBuf + 4, oldBuf, oldLen);
        (*outLen) = HOT_BEHAVIOR_V3_SIZE;
        return true;
    }
    if (oldLen <= newCap) {
        memcpy(newBuf, oldBuf, oldLen);
        (*outLen) = oldLen;
        return true;
    }
    return false;
}

/** Returns the static JSON module descriptor. */
const char *Hot_manifest(void) {
    return "{\"name\": \"hot_behavior\", \"version\": \"" HOT_BEHAVIOR_VERSION "\", "
        "\"type_ids\": [{\"name\": \"BehaviorState\", \"value\": 2}], "
        "\"exports\": [\"hot_behavior_pulse\", \"hot_behavior_bar\", "
        "\"hot_texture_path\", \"hot_behavior_set_phase_bias\", "
        "\"hot_behavior_get_phase_bias\", \"Hot_init_module\", "
        "\"Hot_shutdown_module\", \"Hot_save\", \"Hot_restore\", \"Hot_migrate\", "
        "\"Hot_getExports\"], "
        "\"dependencies\": []}";
}

// LOADER ABI EXPORT — the trampoline table the manifest loader (hot/hot.c)
// adopts on dlopen. Row layout MUST match hot.c's contract:
//   { const char *name; void *fn; } entries[], count via Hot_getExports.
typedef struct HotModuleExport {
    const char *name;
    void *fn;
} HotModuleExport;

f32 hot_behavior_pulse(d64 nowSeconds);
void hot_behavior_bar(f32 w, f32 h, f32 pulse, f32 *outBarH, f32 *outBarW);
const char *hot_texture_path(void);
void hot_behavior_set_phase_bias(f32 value);
f32 hot_behavior_get_phase_bias(void);

static const HotModuleExport s_exports[] = {
    { "hot_behavior_pulse",    (void*) hot_behavior_pulse },
    { "hot_behavior_bar",      (void*) hot_behavior_bar },
    { "hot_texture_path",      (void*) hot_texture_path },
    { "hot_behavior_set_phase_bias", (void*) hot_behavior_set_phase_bias },
    { "hot_behavior_get_phase_bias", (void*) hot_behavior_get_phase_bias },
    { "Hot_init_module",       (void*) Hot_init_module },
    { "Hot_shutdown_module",   (void*) Hot_shutdown_module },
    { "Hot_save",              (void*) Hot_save },
    { "Hot_restore",           (void*) Hot_restore },
    { "Hot_migrate",           (void*) Hot_migrate },
};

/** Returns this module's static trampoline export table and optional row count. */
const void *Hot_getExports(u32 *outCount) {
    if (outCount)
        (*outCount) = (u32) (sizeof(s_exports) / sizeof(s_exports[0]));
    return s_exports;
}

/** Computes a sinusoidal pulse shifted by phase bias and scaled by glow strength. */
f32 hot_behavior_pulse(d64 nowSeconds) {
    d64 t = nowSeconds + (d64) s_phaseBias;
    f32 base = 0.5f + 0.5f * sinf((f32) (t * 6.28318530718));
    return base * s_glowStrength > 1.0f ? 1.0f : base * s_glowStrength;
}

/** Computes bar height and pulse-scaled width into whichever outputs are supplied. */
void hot_behavior_bar(f32 w, f32 h, f32 pulse, f32 *outBarH, f32 *outBarW) {
    f32 barH = h * 0.08f;
    f32 barW = w * pulse;
    if (outBarH)
        (*outBarH) = barH;
    if (outBarW)
        (*outBarW) = barW;
    (void) w;
}

/** Returns the asset-relative texture path used by the host. */
const char *hot_texture_path(void) {
    // Portable asset-relative path: the host resolves this against its asset
    // root (never an absolute developer-machine path — those break every
    // other checkout and leak local filesystem layout).
    return "assets/sunflower.png";
}

// SETTERS (PUBLIC & PRIVATE)

;;SETTER
/** Replaces the module's phase offset used by pulse calculations. */
void hot_behavior_set_phase_bias(f32 value) {
    s_phaseBias = value;
}

// GETTERS (PUBLIC & PRIVATE)

;;GETTER
/** Returns the module's current phase offset. */
f32 hot_behavior_get_phase_bias(void) {
    return s_phaseBias;
}
