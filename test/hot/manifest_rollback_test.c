// _tests/hotcwap/manifest_rollback_test.c — #8.5 Automated State Rollback.
//
// MODULE — test harness (zero structs), the Test Segregation Law's umbrella
// harness for hotcwap's restore-before-commit rollback (hot/hot.c).
//
// EXERCISES THE REAL LADDER AGAINST A FOREIGN-GENERATION MODULE:
//   1. Build a scratch $VEX_MANIFEST and reflect generation 1 = the good
//      hot_behavior build (canonical HOT_BEHAVIOR_SCHEMA_MAGIC).
//   2. First load, then mutate module state via hot_behavior_set_phase_bias
//      and record a determinist pulse baseline (the value proves which image
//      is live).
//   3. Promote generation 2 = the BAD build (foreign magic, compiled with
//      HOT_BEHAVIOR_SCHEMA_MAGIC=0xBADC0DE5). Its Hot_restore REJECTS the
//      canonical state blob — the exact #8.5 failure: restored state fails
//      validation.
//   4. Hot_poll must ROLL THE SWAP BACK: return HOT_ERROR_RESTORE_FAILED,
//      keep generation 1 live, keep the old code + phaseBias live, and NOT
//      advance the stamp. A re-poll re-attempts and rejects again.
//   5. Promote generation 3 = a good build again; the loader heals, adopts
//      it, and phaseBias survives (state preserved end-to-end).
//
// Run with `./build/_tests/.../manifest_rollback_test` after the umbrella
// build (target wired by projects/hotcwap/CMakeLists.txt: good module path in
// HOT_BEHAVIOR_MODULE, bad module path in HOT_BEHAVIOR_BAD_MODULE). Scratch
// state lives in a mkdtemp dir and is best-effort removed on exit. POSIX-only
// (dlopen-based loader contract); on Windows the harness skips with exit 0.

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
#ifndef HOT_BEHAVIOR_BAD_MODULE
#define HOT_BEHAVIOR_BAD_MODULE "hot_behavior.so"
#endif

#define PHASE_BIAS 1.5f

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

/** Copies a prepared module fixture into the rollback test's staging path. */
static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t buf[4096];
    size_t n = 0;
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

/** Creates missing parent directories for the rollback fixtures. */
static bool mkdir_p(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return true;
    if (errno != ENOENT)
        return false;
    char tmp[512];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len >= sizeof(tmp))
        return false;
    for (size_t i = 1; i < len; i++) {
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

/** Removes the temporary rollback fixture directory tree. */
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
        if (clen < 0 || (size_t) clen >= sizeof(child)) {
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

// Stage one built module into loadBase/payload as <basename> — the loader
// contract scans .dylib/.so entries under bin/current/<library>.
static bool stage_module(const char *loadBase, const char *modulePath) {
    char path[512];
    int plen = snprintf(path, sizeof(path), "%s/payload", loadBase);
    if (plen < 0 || (size_t) plen >= sizeof(path))
        return false;
    if (!mkdir_p(path))
        return false;
    const char *base = strrchr(modulePath, '/');
    base = base ? base + 1 : modulePath;
    char dst[512];
    int dlen = snprintf(dst, sizeof(dst), "%s/%s", path, base);
    if (dlen < 0 || (size_t) dlen >= sizeof(dst))
        return false;
    return copy_file(modulePath, dst);
}

// Poll (bounded sleeps) until a non-OK HotResult surfaces — the rollback
// error after a kick + swap. Returns HOT_OK when nothing settled in time.
static HotResult poll_until_error(HotModule *hot) {
    struct timespec d = { 0, 20 * 1000 * 1000 };
    HotResult last = HOT_OK;
    for (int i = 0; i < 64; i++) {
        if (i > 0)
            nanosleep(&d, 0);
        uint32_t loaded = 0;
        last = Hot_poll(hot, &loaded);
        if (last != HOT_OK)
            return last;
    }
    return last;
}

// Poll (bounded sleeps) until a generation lands; false when nothing settled.
static bool poll_until_generation(HotModule *hot, uint64_t target) {
    struct timespec d = { 0, 20 * 1000 * 1000 };
    for (int i = 0; i < 64; i++) {
        if (i > 0)
            nanosleep(&d, 0);
        uint32_t loaded = 0;
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
    printf("manifest_rollback_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else

    // --- 1. Scratch manifest base (test seam override) ----------------------
    char scratch[] = "/tmp/vexgraph_rollback_XXXXXX";
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

    // --- 2. Catalog + ladder + reflect generation 1 (good module) -----------
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "manifest_rollback_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0),
          "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");
    CHECK(stage_module(loadBase, HOT_BEHAVIOR_MODULE), "stage good module failed");
    char payloadDir[512];
    int pd = snprintf(payloadDir, sizeof(payloadDir), "%s/payload", loadBase);
    CHECK(pd >= 0 && (size_t) pd < sizeof(payloadDir), "payloadDir truncation");
    CHECK(MANIFEST_REFLECT("hot_behavior", payloadDir),
          "MANIFEST_REFLECT() failed");
    CHECK(MANIFEST_GENERATION("hot_behavior") == 1u,
          "expected generation 1 after reflect, got %llu",
          (unsigned long long) MANIFEST_GENERATION("hot_behavior"));

    // --- 3. First load (0 → 1) + live module state --------------------------
    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        unsetenv("VEX_MANIFEST");
        rmtree(loadBase);
        return 1;
    }
    uint32_t loaded = 0;
    HotResult r = Hot_poll(hot, &loaded);
    CHECK(r == HOT_OK, "first Hot_poll failed: %s", Hot_getLastError(hot));
    CHECK(Hot_getGeneration(hot) == 1u, "first load generation != 1");
    HotFn setFn = Hot_getSymbol(hot, "hot_behavior_set_phase_bias");
    HotFn getFn = Hot_getSymbol(hot, "hot_behavior_get_phase_bias");
    CHECK(setFn != nullptr, "hot_behavior_set_phase_bias unresolved (gen 1)");
    CHECK(getFn != nullptr, "hot_behavior_get_phase_bias unresolved (gen 1)");
    if (setFn)
        ((void (*)(float)) setFn)(PHASE_BIAS);
    float bias0 = getFn ? ((float (*)(void)) getFn)() : 0.0f;
    CHECK(bias0 == PHASE_BIAS, "phaseBias not applied in gen 1 (got %f)", bias0);
    HotFn pulseFn = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulseFn != nullptr, "hot_behavior_pulse unresolved (gen 1)");
    float pulse0 = pulseFn ? ((float (*)(double)) pulseFn)(1.0) : 0.0f;

    // --- 4. Promote generation 2 = the BAD (foreign-magic) build ------------
    CHECK(stage_module(loadBase, HOT_BEHAVIOR_BAD_MODULE), "stage bad module failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir),
          "MANIFEST_UPDATE(bad) failed");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE(bad) failed");
    CHECK(MANIFEST_GENERATION("hot_behavior") == 2u,
          "expected generation 2 after promote, got %llu",
          (unsigned long long) MANIFEST_GENERATION("hot_behavior"));

    // --- 5. The swap MUST roll back (restore rejected, pre-commit) ----------
    r = poll_until_error(hot);
    CHECK(r == HOT_ERROR_RESTORE_FAILED,
          "expected HOT_ERROR_RESTORE_FAILED on bad generation, got %d (%s)",
          (int) r, Hot_getLastError(hot));
    CHECK(strstr(Hot_getLastError(hot), "Restore rejected") != nullptr,
          "last_error should name the rejected restore: %s", Hot_getLastError(hot));
    CHECK(Hot_getGeneration(hot) == 1u,
          "generation advanced past a rejected restore (now %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    pulseFn = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulseFn != nullptr, "pulse symbol lost after rollback (old image dead?)");
    if (pulseFn) {
        float p = ((float (*)(double)) pulseFn)(1.0);
        CHECK(p == pulse0, "generation-1 pulse changed after rollback (%f != %f)",
              p, pulse0);
    }
    getFn = Hot_getSymbol(hot, "hot_behavior_get_phase_bias");
    float biasBack = getFn ? ((float (*)(void)) getFn)() : 0.0f;
    CHECK(biasBack == PHASE_BIAS,
          "generation-1 phaseBias lost after rollback (got %f, %s)",
          biasBack, Hot_getLastError(hot));
    printf("  rollback landed: generation stays 1, old code + phaseBias live\n");

    // --- 6. Re-attempt self-heal: still rejected, still generation 1 --------
    r = poll_until_error(hot);
    CHECK(r == HOT_ERROR_RESTORE_FAILED,
          "re-attempt should reject the bad set again, got %d (%s)",
          (int) r, Hot_getLastError(hot));
    CHECK(Hot_getGeneration(hot) == 1u,
          "re-attempt advanced generation (now %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    printf("  re-attempt rejected again — generation never advances on a bad set\n");

    // --- 7. Heal: promote generation 3 = a good build again -----------------
    CHECK(stage_module(loadBase, HOT_BEHAVIOR_MODULE), "stage good #2 failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir),
          "MANIFEST_UPDATE(good #2) failed");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE(good #2) failed");
    CHECK(MANIFEST_GENERATION("hot_behavior") == 3u,
          "expected generation 3 after promote, got %llu",
          (unsigned long long) MANIFEST_GENERATION("hot_behavior"));
    CHECK(poll_until_generation(hot, 3u), "self-heal swap never landed (gen %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    CHECK(Hot_getGeneration(hot) == 3u, "post-heal generation != 3");
    getFn = Hot_getSymbol(hot, "hot_behavior_get_phase_bias");
    float biasHealed = getFn ? ((float (*)(void)) getFn)() : 0.0f;
    CHECK(biasHealed == PHASE_BIAS,
          "phaseBias did not survive the heal swap (got %f)", biasHealed);
    pulseFn = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulseFn != nullptr, "pulse symbol lost after heal");
    if (pulseFn) {
        float p = ((float (*)(double)) pulseFn)(1.0);
        CHECK(p == pulse0, "pulse changed after heal (%f != %f)", p, pulse0);
    }
    printf("  healed to generation 3, phaseBias %f preserved end-to-end\n",
           (double) biasHealed);

    Hot_shutdown(hot);
    unsetenv("VEX_MANIFEST");
    rmtree(loadBase);

    if (s_failures == 0)
        printf("PASS manifest_rollback_test (bad set rejected, gen 1 preserved, self-healed)\n");
    else
        fprintf(stderr, "FAIL manifest_rollback_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
