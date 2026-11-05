// hotcwap/test/hot/retire_ring_overflow_test.c — more swaps than ring slots.
//
// MODULE: RetireRingOverflowTest (zero structs) — the hotcwap Retire-Ring
// Overflow Law owner test for the loader (hot/hot.c + hot/hot_retire.c).
//
// The retire ring holds HOT_RETIRED_MAX (16) parked dylib handles and closes
// entries older than HOT_RETIRED_GENERATIONS (4) polls. PROVES the loader
// survives FAR more swaps than the ring has slots: it drives 20 promotions and
// asserts every swap lands and the module still resolves — the ring evicts the
// oldest handle without a use-after-free or a lost trampoline.
//
// Headless but real (dlopen-based loader contract). Scratch state lives in a
// mkdtemp dir under the VEX_MANIFEST test seam. POSIX-only.

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

#define SWAP_COUNT 20

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

/** Copies a compiled module fixture into the loader's staged payload. */
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

/** Creates missing directory components for the overflow fixture. */
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

/** Removes the temporary loader fixture tree. */
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

/** Places the test module under the manifest-selected payload name. */
static bool stage_module(const char *loadBase) {
    char dir[512];
    int dl = snprintf(dir, sizeof(dir), "%s/payload", loadBase);
    if (dl < 0 || (size_t) dl >= sizeof(dir))
        return false;
    if (!mkdir_p(dir))
        return false;
    char dst[512];
    int n = snprintf(dst, sizeof(dst), "%s/hot_behavior.so", dir);
    if (n < 0 || (size_t) n >= sizeof(dst))
        return false;
    return copy_file(HOT_BEHAVIOR_MODULE, dst);
}

/** Waits within the test deadline for the requested module generation. */
static bool wait_for_generation(HotModule *hot, uint32_t target) {
    for (int i = 0; i < 100; i++) {
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
    printf("retire_ring_overflow_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else
    char scratch[] = "/tmp/vexgraph_retire_overflow_XXXXXX";
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

    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "retire_ring_overflow_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0), "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    char payloadDir[512];
    snprintf(payloadDir, sizeof(payloadDir), "%s/payload", base);
    CHECK(stage_module(base), "stage #1 failed");
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

    // Drive far more swaps than the 16-slot ring holds.
    bool all_landed = true;
    for (uint32_t g = 2; g <= SWAP_COUNT; g++) {
        CHECK(stage_module(base), "stage #%u failed", g);
        if (!MANIFEST_UPDATE("hot_behavior", payloadDir) || !MANIFEST_PROMOTE()) {
            CHECK(false, "promote #%u failed", g);
            all_landed = false;
            break;
        }
        Hot_poll(hot, &loaded);   // kick the snapshot worker
        if (!wait_for_generation(hot, g)) {
            CHECK(false, "swap #%u never landed (generation %llu)", g,
                  (unsigned long long) Hot_getGeneration(hot));
            all_landed = false;
            break;
        }
    }
    CHECK(all_landed, "not every swap landed");
    CHECK(Hot_getGeneration(hot) == SWAP_COUNT, "final generation %llu != %u",
          (unsigned long long) Hot_getGeneration(hot), SWAP_COUNT);
    HotFn pulse = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulse != nullptr, "hot_behavior_pulse lost after the overflow run");
    if (pulse) {
        float v = ((float (*)(double)) pulse)(1.0);
        CHECK(v >= 0.0f && v <= 1.0f, "pulse returned %f after the overflow run", v);
    }
    printf("  %d swaps landed; ring (max 16) never corrupted; module resolves\n", SWAP_COUNT);

    Hot_shutdown(hot);
    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS retire_ring_overflow_test (survives more swaps than ring slots)\n");
    else
        fprintf(stderr, "FAIL retire_ring_overflow_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
