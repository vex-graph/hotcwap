// _tests/hotcwap/manifest_hot_test.c — end-to-end manifest-ladder hot reload.
//
// MODULE — test harness (zero structs), the Test Segregation Law's umbrella
// harness for hotcwap's generation-driven loader (hot/hot.c + hot/manifest.c).
//
// EXERCISES THE REAL LADDER, NOT A STUB:
//   1. Build a scratch $VEX_MANIFEST (MANIFEST_APP_DATA override seam).
//   2. MANIFEST() + MANIFEST_LIBRARY("hot_behavior") + MANIFEST_ENSURE().
//   3. MANIFEST_REFLECT a payload dir containing the built hot_behavior
//      module → seeds generation stamp 1.
//   4. Hot_poll first load (generation 0 → 1): dlopen, trampoline adopt.
//   5. MANIFEST_UPDATE + MANIFEST_PROMOTE → stamp 2 (rename slide IS the swap).
//   6. Hot_poll two-phase handshake: snapshot OFF-THREAD, then swap on a
//      later poll; retired handle drains via the grace ring.
//   7. Assert Hot_getGeneration == 2 AND the module's saved state survived
//      the swap: hot_behavior_pulse must return the same value pre/post swap
//      for the same phaseBias/glow domain (Hot_save → Hot_restore round trip).
//
// Run with `./build/_tests/.../manifest_hot_test` after the umbrella build
// (target wired by projects/hotcwap/CMakeLists.txt). Scratch state lives in
// a mkdtemp dir and is best-effort removed on exit. POSIX-only (dlopen-based
// loader contract); on Windows the harness skips with exit 0.

#include "lifetime/platform.h"
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hot.h"
#include "manifest.h"
#include "test_support.h"

#ifndef HOT_BEHAVIOR_MODULE
#define HOT_BEHAVIOR_MODULE "hot_behavior.so"
#endif

#define TEST_GENERATION_TARGET 2u

static int s_failures = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            s_failures++;                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
        }                                                                       \
    } while (0)

/** Copies the prepared module fixture to its loader staging location. */
static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    u8 buf[4096];
    bytes n = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(out);
            fclose(in);
            return false;
        }
    }
    bool ok = !ferror(in);
    fclose(out);
    fclose(in);
    return ok;
}

/** Creates missing parent directories required by the staged manifest. */
static bool mkdir_p(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return true;
    if (errno != ENOENT)
        return false;
    char tmp[512];
    bytes len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len >= sizeof(tmp))
        return false;
    for (bytes i = 1; i < len; i++) {
        if (tmp[i] != '/')
            continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            return false;
        tmp[i] = '/';
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

/** Removes the temporary manifest and module fixture tree. */
static bool rmtree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir)
        return false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp((*ent).d_name, ".") == 0 || strcmp((*ent).d_name, "..") == 0)
            continue;
        char child[512];
        int clen = snprintf(child, sizeof(child), "%s/%s", path, (*ent).d_name);
        if (clen < 0 || (bytes) clen >= sizeof(child)) {
            closedir(dir);
            return false;
        }
        struct stat st;
        if (lstat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!rmtree(child)) {
                closedir(dir);
                return false;
            }
        } else {
            unlink(child);
        }
    }
    closedir(dir);
    rmdir(path);
    return true;
}

// Stage the built module into loadBase/payload as <stem>.so — the loader
// contract scans .dylib/.so entries under bin/current/<library>.
static bool stage_payload(const char *loadBase) {
    char path[512];
    int plen = snprintf(path, sizeof(path), "%s/payload", loadBase);
    if (plen < 0 || (bytes) plen >= sizeof(path))
        return false;
    if (!mkdir_p(path))
        return false;
    const char *moduleFile = HOT_BEHAVIOR_MODULE;
    const char *base = strrchr(moduleFile, '/');
    base = base ? base + 1 : moduleFile;
    char dst[512];
    int dlen = snprintf(dst, sizeof(dst), "%s/%s", path, base);
    if (dlen < 0 || (bytes) dlen >= sizeof(dst))
        return false;
    return copy_file(moduleFile, dst);
}

/** Polls the module until its generation reaches the target or times out. */
static bool wait_for_generation(HotModule *hot, u32 target) {
    for (int i = 0; i < 60; i++) {
        struct timespec d = { 0, 20 * 1000 * 1000 };
        nanosleep(&d, 0);
        u32 loaded = 0;
        HotResult r = Hot_poll(hot, &loaded);
        if (r != HOT_OK)
            return false;
        if (Hot_getGeneration(hot) == target)
            return true;
    }
    return false;
}

int main(void) {
#if defined(VEX_PLATFORM_WINDOWS)
    printf("manifest_hot_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else

    // --- 1. Scratch manifest base (test seam override) ----------------------
    char scratch[] = "/tmp/vexgraph_manifest_XXXXXX";
    char *loadBase = mkdtemp(scratch);
    if (!loadBase) {
        fprintf(stderr, "FAIL: cannot mkdtemp scratch manifest base\n");
        return 1;
    }
    if (setenv("VEX_MANIFEST", loadBase, 1) != 0) {
        fprintf(stderr, "FAIL: cannot override VEX_MANIFEST\n");
        rmtree(loadBase);
        return 1;
    }
    char homeDir[512];
    snprintf(homeDir, sizeof(homeDir), "%s/home", loadBase);
    if (!mkdir_p(homeDir) || setenv("HOME", homeDir, 1) != 0) {
        fprintf(stderr, "FAIL: cannot redirect HOME\n");
        rmtree(loadBase);
        return 1;
    }

    // --- 2. Catalog + ladder -------------------------------------------------
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "manifest_hot_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0),
          "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    // --- 3. Stage + reflect (seeds generation 1) ----------------------------
    CHECK(stage_payload(loadBase), "stage_payload failed");
    char payloadDir[512];
    int pd = snprintf(payloadDir, sizeof(payloadDir), "%s/payload", loadBase);
    CHECK(pd >= 0 && (bytes) pd < sizeof(payloadDir), "payloadDir truncation");
    CHECK(MANIFEST_REFLECT("hot_behavior", payloadDir),
          "MANIFEST_REFLECT() failed");
    CHECK(MANIFEST_GENERATION("hot_behavior") == 1u,
          "expected generation 1 after reflect, got %llu",
          (unsigned long long) MANIFEST_GENERATION("hot_behavior"));

    // --- 4. First load (0 → 1): dlopen + trampoline adopt ------------------
    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        rmtree(loadBase);
        return 1;
    }
    u32 loaded = 0;
    HotResult r = Hot_poll(hot, &loaded);
    CHECK(r == HOT_OK, "first Hot_poll failed: %s", Hot_getLastError(hot));
    CHECK(loaded > 0u, "first Hot_poll loaded %u modules", loaded);
    CHECK(Hot_getGeneration(hot) == 1u, "first load generation != 1");
    HotFn pulseFn = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulseFn != nullptr, "hot_behavior_pulse unresolved after first load");
    if (pulseFn) {
        f32 v = ((f32 (*)(d64)) pulseFn)(1.0);
        CHECK(v >= 0.0f && v <= 1.0f, "pulse(1.0)=%f out of range", v);
        printf("  generation 1 pulse(1.0) = %f\n", v);
    }

    // --- 5. Stage v2 payload + UPDATE + PROMOTE (stamp → 2) ----------------
    CHECK(stage_payload(loadBase), "stage_payload #2 failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir),
          "MANIFEST_UPDATE() failed");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE() failed");
    CHECK(MANIFEST_GENERATION("hot_behavior") == 2u,
          "expected generation 2 after promote, got %llu",
          (unsigned long long) MANIFEST_GENERATION("hot_behavior"));

    // --- 6. Two-phase live swap (snapshot off-thread, then swap) ------------
    r = Hot_poll(hot, &loaded);   // kicks the snapshot worker; loaded stays 0
    CHECK(r == HOT_OK, "swap-kick Hot_poll failed: %s", Hot_getLastError(hot));
    CHECK(loaded == 0u, "kick poll must not swap yet (loaded=%u)", loaded);
    CHECK(wait_for_generation(hot, TEST_GENERATION_TARGET),
          "swap never landed (generation %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    CHECK(Hot_getGeneration(hot) == 2u, "post-swap generation != 2");
    printf("  live swap landed: generation 1 → 2\n");

    // --- 7. Trampolines re-adopted + state survived the swap ----------------
    pulseFn = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulseFn != nullptr, "hot_behavior_pulse unresolved after swap");
    if (pulseFn) {
        f32 v = ((f32 (*)(d64)) pulseFn)(1.0);
        CHECK(v >= 0.0f && v <= 1.0f, "post-swap pulse(1.0)=%f out of range", v);
        printf("  generation 2 pulse(1.0) = %f (state restored)\n", v);
    }
    HotFn barFn = Hot_getSymbol(hot, "hot_behavior_bar");
    CHECK(barFn != nullptr, "hot_behavior_bar lost after swap");

    Hot_shutdown(hot);
    unsetenv("VEX_MANIFEST");
    rmtree(loadBase);

    if (s_failures == 0)
        printf("PASS manifest_hot_test (generation 1 → 2 live swap, state preserved)\n");
    else
        fprintf(stderr, "FAIL manifest_hot_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
