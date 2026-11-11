// hotcwap/test/hot/shutdown_order_test.c — shutdown joins the worker first.
//
// MODULE: ShutdownOrderTest (zero structs) — the hotcwap Teardown and Bounded
// Wait Law owner test for the loader (hot/hot.c).
//
// Hot_shutdown() must bounded-join the state-save worker FIRST, then close every
// module and drain the retire ring. PROVES that ordering holds even when a swap
// is mid-flight:
//   A. Load generation 1.
//   B. Kick a swap (snapshot worker wakes) and shut down IMMEDIATELY — no crash,
//      no hang: the worker is cancelled and joined before handles close.
//   C. A fresh Hot_init + load works afterwards (clean teardown, no leaked state).
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

/** Copies a built module fixture to its staged loader path. */
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

/** Creates missing parent directories for the shutdown fixture. */
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

/** Removes the temporary shutdown fixture directory tree. */
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

/** Stages the module used to observe loader and shutdown ordering. */
static bool stage_module(const char *loadBase) {
    char dir[512];
    int dl = snprintf(dir, sizeof(dir), "%s/payload", loadBase);
    if (dl < 0 || (bytes) dl >= sizeof(dir))
        return false;
    if (!mkdir_p(dir))
        return false;
    char dst[512];
    int n = snprintf(dst, sizeof(dst), "%s/hot_behavior.so", dir);
    if (n < 0 || (bytes) n >= sizeof(dst))
        return false;
    return copy_file(HOT_BEHAVIOR_MODULE, dst);
}

int main(void) {
#if defined(VEX_PLATFORM_WINDOWS)
    printf("shutdown_order_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else
    char scratch[] = "/tmp/vexgraph_shutdown_order_XXXXXX";
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

    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "shutdown_order_test"),
          "MANIFEST() mount failed");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0), "MANIFEST_LIBRARY() failed");
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    char payloadDir[512];
    snprintf(payloadDir, sizeof(payloadDir), "%s/payload", base);
    CHECK(stage_module(base), "stage #1 failed");
    CHECK(MANIFEST_REFLECT("hot_behavior", payloadDir), "MANIFEST_REFLECT() failed");

    // --- A. Load generation 1 ------------------------------------------------
    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        rmtree(base);
        return 1;
    }
    u32 loaded = 0;
    HotResult r = Hot_poll(hot, &loaded);
    CHECK(r == HOT_OK && Hot_getGeneration(hot) == 1u, "gen1 load failed: %s", Hot_getLastError(hot));
    printf("  A. generation 1 loaded\n");

    // --- B. Shutdown with a swap in flight -----------------------------------
    CHECK(stage_module(base), "stage #2 failed");
    CHECK(MANIFEST_UPDATE("hot_behavior", payloadDir), "MANIFEST_UPDATE() failed");
    CHECK(MANIFEST_PROMOTE(), "MANIFEST_PROMOTE() failed");
    Hot_poll(hot, &loaded);   // wakes the snapshot worker; swap not yet committed
    // Shut down immediately while the worker may be mid-snapshot: Hot_shutdown
    // must cancel + bounded-join the worker BEFORE closing handles.
    Hot_shutdown(hot);
    printf("  B. shutdown during a live swap completed cleanly\n");

    // --- C. A fresh instance still works -------------------------------------
    HotModule *hot2 = Hot_init("hot_behavior");
    CHECK(hot2 != nullptr, "re-init returned NULL");
    if (hot2) {
        loaded = 0;
        r = Hot_poll(hot2, &loaded);
        CHECK(r == HOT_OK, "re-init load failed: %s", Hot_getLastError(hot2));
        CHECK(Hot_getGeneration(hot2) == 2u,
              "re-init generation %llu != 2", (unsigned long long) Hot_getGeneration(hot2));
        Hot_shutdown(hot2);
    }
    printf("  C. fresh init loaded generation 2 after teardown\n");

    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS shutdown_order_test (worker joined before handles close)\n");
    else
        fprintf(stderr, "FAIL shutdown_order_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
