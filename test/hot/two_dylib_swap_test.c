// hotcwap/test/hot/two_dylib_swap_test.c — two real dylibs, one library.
//
// MODULE: TwoDylibSwapTest (zero structs) — the hotcwap Two-Dylib Swap Law,
// Manifest Resilience Law, and Runtime-Level Seam Law owner test for the loader.
//
// A HotModule binds one manifest LIBRARY key; the library's bin/current/<lib>
// holds one dylib per SECTION. PROVES the loader manages TWO dylibs at once:
//   A. A library with two module sections loads BOTH (loaded_count == 2).
//   B. A live swap (UPDATE + PROMOTE) re-adopts BOTH sections at generation 2,
//      so both dylibs are swapped together, not one at a time.
//
// The loader keeps the retired generation mapped HOT_RETIRED_GENERATIONS polls
// (the retire ring), so both the outgoing and incoming images are resident
// across the swap.
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

/** Copies a compiled module fixture to its staged loader destination. */
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

/** Creates missing parent directories for the two-module fixture. */
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

/** Removes the temporary two-module loader fixture tree. */
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

// Copy the built module into loadBase/payload/<stem>.so — one section per stem.
static bool stage_module(const char *loadBase, const char *stem) {
    char dir[512];
    int dl = snprintf(dir, sizeof(dir), "%s/payload", loadBase);
    if (dl < 0 || (bytes) dl >= sizeof(dir))
        return false;
    if (!mkdir_p(dir))
        return false;
    char dst[512];
    int n = snprintf(dst, sizeof(dst), "%s/%s.so", dir, stem);
    if (n < 0 || (bytes) n >= sizeof(dst))
        return false;
    return copy_file(HOT_BEHAVIOR_MODULE, dst);
}

/** Waits within the test deadline for the requested loader generation. */
static bool wait_for_generation(HotModule *hot, u32 target) {
    for (int i = 0; i < 80; i++) {
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
    printf("two_dylib_swap_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else
    char scratch[] = "/tmp/vexgraph_two_dylib_XXXXXX";
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

    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "two_dylib_swap_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0),
          "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    // --- Two sections: hot_behavior + companion -----------------------------
    CHECK(stage_module(base, "hot_behavior"), "stage hot_behavior failed");
    CHECK(stage_module(base, "hot_behavior_two"), "stage hot_behavior_two failed");
    char payloadDir[512];
    snprintf(payloadDir, sizeof(payloadDir), "%s/payload", base);
    CHECK(MANIFEST_REFLECT("hot_behavior", payloadDir), "MANIFEST_REFLECT() failed");

    // --- A. Both dylibs load ------------------------------------------------
    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        rmtree(base);
        return 1;
    }
    u32 loaded = 0;
    HotResult r = Hot_poll(hot, &loaded);
    CHECK(r == HOT_OK, "first Hot_poll failed: %s", Hot_getLastError(hot));
    CHECK(loaded == 2u, "expected 2 sections loaded, got %u", loaded);
    CHECK(Hot_getGeneration(hot) == 1u, "first load generation != 1");
    printf("  A. two sections loaded together (loaded=%u)\n", loaded);

    // --- B. Both swap together ----------------------------------------------
    CHECK(stage_module(base, "hot_behavior"), "re-stage hot_behavior failed");
    CHECK(stage_module(base, "hot_behavior_two"), "re-stage hot_behavior_two failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir), "MANIFEST_UPDATE() failed");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE() failed");

    r = Hot_poll(hot, &loaded);   // kick the snapshot worker
    CHECK(r == HOT_OK, "swap-kick Hot_poll failed: %s", Hot_getLastError(hot));
    CHECK(wait_for_generation(hot, 2u), "swap never landed (generation %llu)",
          (unsigned long long) Hot_getGeneration(hot));
    loaded = 0;
    HotFn pulse = Hot_getSymbol(hot, "hot_behavior_pulse");
    CHECK(pulse != nullptr, "hot_behavior_pulse unresolved after the two-dylib swap");
    printf("  B. both sections re-adopted at generation 2\n");

    Hot_shutdown(hot);
    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS two_dylib_swap_test (two real dylibs loaded and swapped together)\n");
    else
        fprintf(stderr, "FAIL two_dylib_swap_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
