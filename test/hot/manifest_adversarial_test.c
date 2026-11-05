// hotcwap/test/hot/manifest_adversarial_test.c — hostile manifest battery.
//
// MODULE: ManifestAdversarialTest (zero structs) — the hotcwap Manifest
// Resilience Law + Adversarial and Hostile-Input Proof Law owner test for
// hot/manifest.c's catalog + path gate.
//
// PROVES: the catalog is created when absent and never trusted when hostile.
//   A. Path/name gate — "..", "a/b.sh", and "" as segments fail closed.
//   B. Corrupt manifest.json — MANIFEST() rejects it and NEVER overwrites the
//      existing file (fail-closed, no clobber).
//   C. Missing manifest.json — created on mount (create-files-if-empty).
//   D. Library key gate — an invalid key is rejected; a fresh library reads
//      generation 0 (never installed).
//
// Headless, dlopen-free. Scratch state lives in a mkdtemp dir under the
// VEX_MANIFEST test seam and is removed on exit. POSIX-only.

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "manifest.h"

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

/** Creates missing parent directories for adversarial manifest fixtures. */
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

/** Removes the temporary adversarial manifest fixture tree. */
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

/** Writes a NUL-terminated fixture string to the specified path. */
static bool write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    size_t n = strlen(data);
    bool ok = (fwrite(data, 1, n, f) == n);
    fclose(f);
    return ok;
}

// True when path's Bytes are exactly `data` (length + content). Proves a
// rejected mount did NOT clobber the existing hostile file.
static bool file_matches(const char *path, const char *data) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, data) == 0;
}

int main(void) {
    char scratch[] = "/tmp/vexgraph_manifest_adv_XXXXXX";
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

    // --- A0. Tilde gate ------------------------------------------------------
    // A tilde inside a NAME is a real POSIX directory ("~v") and must mount.
    // A BARE "~" is the home ROOT alias, so it is refused as a segment —
    // otherwise MANIFEST("~", ...) would be ambiguous.
    //
    // Both run in a forked child: a successful MANIFEST() mounts process-wide,
    // and the parent must still be unmounted for the sections below.
    for (int wantTildeName = 1; wantTildeName >= 0; wantTildeName--) {
        pid_t pid = fork();
        CHECK(pid >= 0, "fork failed");
        if (pid == 0) {
            bool mounted = wantTildeName
                ? MANIFEST(MANIFEST_APP_DATA, "vexgraph", "~v")
                : MANIFEST(MANIFEST_APP_DATA, "vexgraph", "~");
            _exit(mounted ? 0 : 1);
        }
        int status = 0;
        CHECK(waitpid(pid, &status, 0) == pid, "waitpid failed");
        bool mounted = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (wantTildeName)
            CHECK(mounted, "MANIFEST() rejected a '~v' segment");
        else
            CHECK(!mounted, "MANIFEST() accepted a bare '~' segment");
    }
    printf("  A0. tilde gate accepted '~v', rejected bare '~'\n");

    // --- A. Path/name gate --------------------------------------------------
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", ".."),
          "MANIFEST() accepted a '..' segment");
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", "a/b.sh"),
          "MANIFEST() accepted a segment with a path separator");
    CHECK(!MANIFEST(MANIFEST_APP_DATA, "vexgraph", ""),
          "MANIFEST() accepted an empty segment");
    printf("  A. path gate rejected '..', 'a/b.sh', ''\n");

    // --- B. Corrupt manifest.json: reject + never overwrite ------------------
    const char *hostile[] = { "not json at all", "{", "]", "{\"libraries\":" };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        char app[64];
        snprintf(app, sizeof(app), "adv_corrupt%zu", i);
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/vexgraph/%s", base, app);
        char file[512];
        snprintf(file, sizeof(file), "%s/manifest.json", dir);

        CHECK(mkdir_p(dir), "could not pre-create %s", dir);
        CHECK(write_file(file, hostile[i]), "could not pre-write hostile manifest");

        bool mounted = MANIFEST(MANIFEST_APP_DATA, "vexgraph", app);
        CHECK(!mounted, "MANIFEST() accepted corrupt manifest #%zu", i);
        CHECK(file_matches(file, hostile[i]),
              "MANIFEST() clobbered the existing hostile manifest #%zu", i);
    }
    printf("  B. corrupt manifests rejected, never overwritten (%zu cases)\n",
           sizeof(hostile) / sizeof(hostile[0]));

    // --- C. Missing manifest.json: created on mount -------------------------
    CHECK(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "adv_ok"),
          "MANIFEST() failed on a fresh (missing-manifest) tree");
    char okfile[512];
    snprintf(okfile, sizeof(okfile), "%s/vexgraph/adv_ok/manifest.json", base);
    struct stat st;
    CHECK(stat(okfile, &st) == 0 && S_ISREG((unsigned int) st.st_mode),
          "missing manifest.json was not created on mount");
    CHECK(MANIFEST_ROOT() != nullptr, "MANIFEST_ROOT() null after a good mount");
    printf("  C. missing manifest.json created on mount\n");

    // --- D. Library key gate + fresh generation -----------------------------
    CHECK(!MANIFEST_LIBRARY("..", (const char*) 0),
          "MANIFEST_LIBRARY() accepted an invalid key");
    CHECK(MANIFEST_LIBRARY("adv_lib", (const char*) 0),
          "MANIFEST_LIBRARY() rejected a valid key");
    CHECK(MANIFEST_GENERATION("adv_lib") == 0u,
          "a never-installed library read generation %llu",
          (unsigned long long) MANIFEST_GENERATION("adv_lib"));
    printf("  D. library key gate held; fresh library reads generation 0\n");

    unsetenv("VEX_MANIFEST");
    rmtree(base);

    if (s_failures == 0)
        printf("PASS manifest_adversarial_test (path gate, no-clobber, create-if-empty)\n");
    else
        fprintf(stderr, "FAIL manifest_adversarial_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
}
