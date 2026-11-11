// hotcwap/test/hot/loader_trust_test.c — the loader's trust boundary.
//
// MODULE: LoaderTrustTest (zero structs) — the hotcwap Loader Trust Boundary
// Law owner test for the manifest path gate + the loader's confined scan.
//
// PROVES: the loader only ever loads from the library's own ladder directory,
// and the manifest path gate rejects names that would escape the root.
//   A. Path/name gate — traversal and separator segments/keys are refused.
//   B. Confined scan — an empty library loads NOTHING and fails closed; the
//      loader does not wander outside bin/current/<library>.
//   C. Dangling section — a section that cannot be read is refused fail-closed.
//
// (The loader copies each section to a unique load path before dlopen, so a
// symlink is dereferenced at copy time: a dangling link is refused here.)
//
// SAFE TO RUN: HOME and VEX_MANIFEST are redirected into mkdtemp scratch dirs.
// POSIX-only.

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

/** Creates missing parent directories for the loader trust fixtures. */
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

/** Removes the temporary loader trust fixture tree. */
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

int main(void) {
#if defined(VEX_PLATFORM_WINDOWS)
    printf("loader_trust_test: SKIP (POSIX dlopen contract)\n");
    return B_TEST_SKIP;
#else
    char scratch[] = "/tmp/vexgraph_loader_trust_XXXXXX";
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

    // --- A. Path/name gate ---------------------------------------------------
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", ".."),
          "MANIFEST() accepted a '..' segment");
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", "a/b.sh"),
          "MANIFEST() accepted a separator segment");
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "loader_trust_test"),
          "MANIFEST() rejected a valid mount");
    CHECK(!MANIFEST_LIBRARY("../evil", (const char*) 0),
          "MANIFEST_LIBRARY() accepted a traversal key");
    CHECK(!MANIFEST_LIBRARY("a/b.sh", (const char*) 0),
          "MANIFEST_LIBRARY() accepted a separator key");
    CHECK(MANIFEST_LIBRARY("hot_behavior", (const char*) 0),
          "MANIFEST_LIBRARY() rejected a valid key");
    char gate[512];
    CHECK(!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, "..", gate, sizeof(gate), false),
          "ManifestPath_libraryDir() accepted '..'");
    CHECK(!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, "a/b.sh", gate, sizeof(gate), false),
          "ManifestPath_libraryDir() accepted a separator");
    printf("  A. path/name gate rejected traversal and separators\n");

    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() failed");

    // --- B. Confined scan: an empty library loads nothing, fails closed ------
    char emptyPayload[512];
    snprintf(emptyPayload, sizeof(emptyPayload), "%s/empty", base);
    CHECK(mkdir_p(emptyPayload), "could not make the empty payload dir");
    CHECK(MANIFEST_REFLECT("hot_behavior", emptyPayload), "MANIFEST_REFLECT() failed (empty)");

    HotModule *hot = Hot_init("hot_behavior");
    CHECK(hot != nullptr, "Hot_init() returned NULL");
    if (!hot) {
        rmtree(base);
        return 1;
    }
    u32 loaded = 0;
    HotResult r = HOT_OK;
    bool saw_error = false;
    for (int i = 0; i < 10 && !saw_error; i++) {
        struct timespec d = { 0, 20 * 1000 * 1000 };
        nanosleep(&d, 0);
        r = Hot_poll(hot, &loaded);
        if (r != HOT_OK)
            saw_error = true;
    }
    CHECK(saw_error, "loader did not fail closed on an empty library");
    CHECK(loaded == 0u, "loader reported %u loads from an empty library", loaded);
    printf("  B. empty library failed closed (no loadable sections)\n");
    Hot_shutdown(hot);

    // --- C. A dangling section is refused ------------------------------------
    char libDir[512];
    CHECK(ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, "hot_behavior",
                                  libDir, sizeof(libDir), false),
          "could not resolve the library dir");
    char linkPath[512];
    snprintf(linkPath, sizeof(linkPath), "%s/hot_behavior.so", libDir);
    CHECK(symlink("/nonexistent/vexgraph/module.so", linkPath) == 0,
          "could not create the dangling symlink");

    HotModule *hot2 = Hot_init("hot_behavior");
    CHECK(hot2 != nullptr, "Hot_init() #2 returned NULL");
    if (hot2) {
        saw_error = false;
        for (int i = 0; i < 10 && !saw_error; i++) {
            struct timespec d = { 0, 20 * 1000 * 1000 };
            nanosleep(&d, 0);
            r = Hot_poll(hot2, &loaded);
            if (r != HOT_OK)
                saw_error = true;
        }
        CHECK(saw_error, "loader accepted a dangling (unreadable) section");
        Hot_shutdown(hot2);
    }
    printf("  C. dangling section refused fail-closed\n");

    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS loader_trust_test (path gate holds; loader scan is confined)\n");
    else
        fprintf(stderr, "FAIL loader_trust_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
