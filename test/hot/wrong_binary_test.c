// hotcwap/test/hot/wrong_binary_test.c — a wrong dylib is rejected.
//
// MODULE: WrongBinaryTest (zero structs) — the hotcwap Stale and Wrong Binary
// Law owner test for the loader (hot/hot.c).
//
// PROVES: a promoted section that is not a loadable module is refused
// fail-closed, the old generation stays live, and a later good promote heals.
//   A. Load a good generation 1.
//   B. Promote a GARBAGE section (a non-dylib named hot_behavior.so): the
//      loader surface an error, the generation does NOT advance, and the old
//      code still resolves and runs.
//   C. Heal: promote a good generation 3; the loader swaps cleanly.
//
// Headless but real (dlopen-based loader contract). Scratch state lives in a
// mkdtemp dir under the VEX_MANIFEST test seam. POSIX-only.

#include "lifetime/platform.h"
#include <dirent.h>
#include <dlfcn.h>
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

/** Copies a module fixture to the loader's staged payload path. */
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

/** Creates missing parent directories for wrong-binary test fixtures. */
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

/** Removes the temporary wrong-binary fixture tree. */
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

/** Builds the bounded path to the loader's module payload directory. */
static bool payload_dir(char *dest, size_t cap, const char *loadBase) {
    int n = snprintf(dest, cap, "%s/payload", loadBase);
    return n > 0 && (size_t) n < cap;
}

/** Stages the known-good module fixture for recovery assertions. */
static bool stage_good(const char *loadBase) {
    char dir[512];
    if (!payload_dir(dir, sizeof(dir), loadBase))
        return false;
    if (!mkdir_p(dir))
        return false;
    char dst[512];
    int n = snprintf(dst, sizeof(dst), "%s/hot_behavior.so", dir);
    if (n < 0 || (size_t) n >= sizeof(dst))
        return false;
    return copy_file(HOT_BEHAVIOR_MODULE, dst);
}

// A file that is NOT a loadable module, staged under the declared stem.
static bool stage_garbage(const char *loadBase) {
    char dir[512];
    if (!payload_dir(dir, sizeof(dir), loadBase))
        return false;
    if (!mkdir_p(dir))
        return false;
    char dst[512];
    int n = snprintf(dst, sizeof(dst), "%s/hot_behavior.so", dir);
    if (n < 0 || (size_t) n >= sizeof(dst))
        return false;
    FILE *f = fopen(dst, "wb");
    if (!f)
        return false;
    fputs("this is not a mach-o module\n", f);
    fclose(f);
    return true;
}

/** Polls the loader until the expected generation is active or times out. */
static bool wait_for_generation(HotModule *hot, uint32_t target) {
    for (int i = 0; i < 80; i++) {
        struct timespec d = { 0, 20 * 1000 * 1000 };
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
    printf("wrong_binary_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else
    char scratch[] = "/tmp/vexgraph_wrong_binary_XXXXXX";
    char *base = mkdtemp(scratch);
    if (!base) {
        fprintf(stderr, "FAIL: cannot mkdtemp scratch manifest base\n");
        return 1;
    }
    if (setenv("VEX_MANIFEST", base, 1) != 0) {
        fprintf(stderr, "FAIL: cannot override VEX_MANIFEST\n");
        rmtree(base);
        return 1;
    }
    char homeDir[512];
    snprintf(homeDir, sizeof(homeDir), "%s/home", base);
    if (!mkdir_p(homeDir) || setenv("HOME", homeDir, 1) != 0) {
        fprintf(stderr, "FAIL: cannot redirect HOME\n");
        rmtree(base);
        return 1;
    }

    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "wrong_binary_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0), "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    char payloadDir[512];
    CHECK(payload_dir(payloadDir, sizeof(payloadDir), base), "payloadDir failed");

    // --- A. Good generation 1 ------------------------------------------------
    CHECK(stage_good(base), "stage good #1 failed");
    CHECK(MANIFEST_REFLECT("hot_behavior", payloadDir), "MANIFEST_REFLECT() failed");
    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        rmtree(base);
        return 1;
    }
    uint32_t loaded = 0;
    HotResult r = Hot_poll(hot, &loaded);
    CHECK(r == HOT_OK && Hot_getGeneration(hot) == 1u, "gen1 load failed: %s", Hot_getLastError(hot));
    printf("  A. good generation 1 loaded\n");

    // --- B. Garbage section is refused; gen 1 stays live ---------------------
    CHECK(stage_garbage(base), "stage garbage failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir), "MANIFEST_UPDATE() rejected the staged section");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE() failed");

    bool saw_error = false;
    for (int i = 0; i < 12 && !saw_error; i++) {
        struct timespec d = { 0, 20 * 1000 * 1000 };
        nanosleep(&d, 0);
        loaded = 0;
        r = Hot_poll(hot, &loaded);
        if (r != HOT_OK)
            saw_error = true;
        if (Hot_getGeneration(hot) != 1u)
            break;
    }
    CHECK(saw_error, "loader did not surface an error for a garbage section");
    CHECK(Hot_getGeneration(hot) == 1u,
          "generation advanced on a bad binary (now %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    HotFn pulse = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulse != nullptr, "old code lost after a refused swap");
    if (pulse) {
        float v = ((float (*)(double)) pulse)(1.0);
        CHECK(v >= 0.0f && v <= 1.0f, "old code returned %f after a refused swap", v);
    }
    printf("  B. garbage section refused; generation stayed 1, old code live\n");

    // --- C. Heal with a good promote ----------------------------------------
    CHECK(stage_good(base), "stage good #3 failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir), "MANIFEST_UPDATE() failed on heal");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE() failed on heal");
    r = Hot_poll(hot, &loaded);   // kick
    if (r != HOT_OK) {
        // a failed kick is acceptable; wait_for_generation retries
    }
    uint32_t target = (uint32_t) MANIFEST_GENERATION("hot_behavior");
    CHECK(wait_for_generation(hot, target), "heal swap never landed (generation %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    CHECK(Hot_getGeneration(hot) == target, "post-heal generation mismatch");
    printf("  C. healed at generation %u\n", target);

    Hot_shutdown(hot);
    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS wrong_binary_test (bad binary refused, rolled back, healed)\n");
    else
        fprintf(stderr, "FAIL wrong_binary_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
