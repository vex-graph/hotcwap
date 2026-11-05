// hotcwap/test/hot/uninstall_guard_test.c — the destructive-target guard.
//
// MODULE: UninstallGuardTest (zero structs) — the hotcwap Manifest Resilience
// Law + Adversarial and Hostile-Input Proof Law owner test for the manifest's
// most dangerous verb: UNINSTALL (a recursive delete of a derived path).
//
// PROVES: MANIFEST() and UNINSTALL() share ONE rule — the manifest never owns
// a protected location, and never adopts a directory it did not create. What
// cannot be created cannot be destroyed.
//   A. Segment mismatch — UNINSTALL whose segments differ from the mount is
//      refused; the mounted tree survives. An exact uninstall still works.
//   B. Valuable HOME dir (non-empty) — MANIFEST("~","Documents") must refuse
//      to adopt it. The decoy's canary file survives.
//   B2. Valuable HOME dir (empty) — an empty, pre-existing dir is still not
//      ours to own; MANIFEST("~","Music") is refused.
//   C. Non-empty appdata dir with no manifest.json — refused, canary survives.
//   D. A bare filesystem root — MANIFEST(MANIFEST_MAIN_DISK) alone is refused.
//   E. The shared UNINSTALL guard — a mounted tree whose manifest.json was
//      removed is no longer a manifest tree, so UNINSTALL refuses it.
//
// SAFE TO RUN: HOME and VEX_MANIFEST are redirected into mkdtemp scratch dirs,
// so no real user or system directory is ever a candidate. POSIX-only.

#include "lifetime/platform.h"
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/** Creates the parent directories needed by the uninstall fixtures. */
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

/** Removes the temporary uninstall fixture directory tree. */
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

/** Writes the fixture string and reports whether the complete write succeeded. */
static bool write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t n = strlen(data);
    bool ok = (fwrite(data, 1, n, f) == n);
    fclose(f);
    return ok;
}

/** Reports whether the supplied fixture path currently exists. */
static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void) {
#if defined(VEX_PLATFORM_WINDOWS)
    printf("uninstall_guard_test: SKIP (POSIX path contract)\n");
    return B_TEST_SKIP;
#else
    char homeT[] = "/tmp/vexgraph_guard_home_XXXXXX";
    char appT[]  = "/tmp/vexgraph_guard_app_XXXXXX";
    char *home = mkdtemp(homeT);
    char *app  = mkdtemp(appT);
    if (!home || !app) {
        fprintf(stderr, "FAIL: cannot mkdtemp scratch bases\n");
        return 1;
    }
    if (setenv("HOME", home, 1) != 0 || setenv("VEX_MANIFEST", app, 1) != 0) {
        fprintf(stderr, "FAIL: cannot redirect HOME / VEX_MANIFEST\n");
        return 1;
    }

    // --- A. Segment mismatch is refused; the tree survives -------------------
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "guard_a"),
          "MANIFEST() failed to mount guard_a");
    CHECK(!UNINSTALL(MANIFEST_APP_DATA, "vexgraph", "guard_b"),
          "UNINSTALL() accepted segments that do not match the mount");
    CHECK(MANIFEST_ROOT() != nullptr,
          "a refused UNINSTALL unmounted the manifest");
    char aDir[512];
    snprintf(aDir, sizeof(aDir), "%s/vexgraph/guard_a", app);
    CHECK(path_exists(aDir), "refused UNINSTALL deleted the mounted tree");
    CHECK(UNINSTALL(MANIFEST_APP_DATA, "vexgraph", "guard_a"),
          "UNINSTALL() refused an exact-segment uninstall");
    CHECK(!path_exists(aDir), "exact UNINSTALL did not remove the tree");
    printf("  A. mismatch refused, exact uninstall removed the tree\n");

    // --- B. Valuable HOME dir (non-empty) ------------------------------------
    char bDir[512];
    snprintf(bDir, sizeof(bDir), "%s/Documents", home);
    char bCanary[512];
    snprintf(bCanary, sizeof(bCanary), "%s/canary.txt", bDir);
    CHECK(mkdir_p(bDir), "could not pre-create HOME/Documents");
    CHECK(write_file(bCanary, "precious"), "could not write the canary");

    CHECK(!MANIFEST("~", "Documents"),
          "MANIFEST() adopted a pre-existing non-manifest HOME dir");
    CHECK(path_exists(bCanary),
          "the decoy HOME/Documents was destroyed (canary gone)");
    printf("  B. HOME/Documents adoption refused, canary survived\n");

    // --- B2. Valuable HOME dir (empty) ---------------------------------------
    char b2Dir[512];
    snprintf(b2Dir, sizeof(b2Dir), "%s/Music", home);
    CHECK(mkdir_p(b2Dir), "could not pre-create empty HOME/Music");
    CHECK(!MANIFEST("~", "Music"),
          "MANIFEST() adopted an empty pre-existing HOME dir");
    CHECK(path_exists(b2Dir), "the decoy HOME/Music was removed");
    printf("  B2. empty HOME/Music adoption refused\n");

    // --- C. Non-empty appdata dir with no manifest.json ----------------------
    char cDir[512];
    snprintf(cDir, sizeof(cDir), "%s/vexgraph/guard_c", app);
    char cCanary[512];
    snprintf(cCanary, sizeof(cCanary), "%s/canary.txt", cDir);
    CHECK(mkdir_p(cDir), "could not pre-create appdata guard_c");
    CHECK(write_file(cCanary, "precious"), "could not write the appdata canary");

    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", "guard_c"),
          "MANIFEST() adopted a non-empty app dir with no manifest.json");
    CHECK(path_exists(cCanary), "the decoy app dir was destroyed (canary gone)");
    printf("  C. appdata adoption refused, canary survived\n");

    // --- D. A bare filesystem root is refused --------------------------------
    CHECK(!MANIFEST(MANIFEST_MAIN_DISK),
          "MANIFEST() accepted a bare MAIN_DISK root");
    printf("  D. bare root refused\n");

    // --- F. A foreign manifest.json does not authorize ownership -------------
    char fDir[512];
    snprintf(fDir, sizeof(fDir), "%s/vexgraph/guard_f", app);
    char fCatalog[512];
    snprintf(fCatalog, sizeof(fCatalog), "%s/manifest.json", fDir);
    CHECK(mkdir_p(fDir), "could not pre-create guard_f");
    CHECK(write_file(fCatalog,
            "{\n  \"name\": \"SomeOtherApp\",\n  \"version\": \"1.0\",\n"
            "  \"org\": \"someone-else\",\n  \"libraries\": {}\n}\n"),
          "could not write the foreign manifest.json");
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", "guard_f"),
          "MANIFEST() adopted a dir whose manifest.json belongs to another app");
    CHECK(path_exists(fCatalog), "the foreign manifest.json was destroyed");
    printf("  F. foreign manifest.json rejected (identity mismatch)\n");

    // --- E. The shared UNINSTALL guard ---------------------------------------
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "guard_e"),
          "MANIFEST() failed to mount guard_e");
    char eDir[512];
    snprintf(eDir, sizeof(eDir), "%s/vexgraph/guard_e", app);
    char eCatalog[512];
    snprintf(eCatalog, sizeof(eCatalog), "%s/manifest.json", eDir);
    CHECK(unlink(eCatalog) == 0, "could not remove the manifest.json");
    CHECK(!UNINSTALL(MANIFEST_APP_DATA, "vexgraph", "guard_e"),
          "UNINSTALL() destroyed a directory that is no longer a manifest tree");
    CHECK(path_exists(eDir), "the non-manifest tree was deleted");
    printf("  E. UNINSTALL refused a tree that lost its manifest.json\n");

    unsetenv("VEX_MANIFEST");
    rmtree(home);
    rmtree(app);

    if (s_failures == 0)
        printf("PASS uninstall_guard_test (MANIFEST and UNINSTALL share one guard)\n");
    else
        fprintf(stderr, "FAIL uninstall_guard_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
