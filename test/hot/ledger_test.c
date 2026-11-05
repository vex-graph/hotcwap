// hotcwap/test/hot/ledger_test.c — the machine-scoped install ledger.
//
// MODULE: LedgerTest (zero structs) — the hotcwap Install Ledger law owner test
// for hot/ledger.c (the per-user state-file backend).
//
// PROVES: the machine remembers an install OUTSIDE the tree, and the memory
// survives an uninstall.
//   A. Absent — a never-installed identity has no record.
//   B. Record install — the identity reads back as installed at its version.
//   C. Mark uninstalled — the record is kept (state UNINSTALLED, hasRecord
//      still true), so a wiped tree is NOT a fresh install.
//   D. Forget — an explicit purge removes the record entirely.
//
// SAFE TO RUN: HOME is redirected into a mkdtemp scratch dir, so the ledger
// file never touches the real per-user state directory. POSIX-only.

#include "lifetime/platform.h"
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include "test_support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ledger.h"

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

/** Removes a ledger test directory and reports whether cleanup succeeded. */
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

int main(void) {
#if defined(VEX_PLATFORM_WINDOWS)
    printf("ledger_test: SKIP (POSIX state-file contract)\n");
    return B_TEST_SKIP;
#else
    char homeT[] = "/tmp/vexgraph_ledger_home_XXXXXX";
    char *home = mkdtemp(homeT);
    if (!home) {
        fprintf(stderr, "FAIL: cannot mkdtemp scratch HOME\n");
        return 1;
    }
    if (setenv("HOME", home, 1) != 0) {
        fprintf(stderr, "FAIL: cannot redirect HOME\n");
        rmtree(home);
        return 1;
    }

    char app[64];
    snprintf(app, sizeof(app), "ledger_test_%d", (int) getpid());
    const char *org = "vexgraph.test";

    // A. Absent.
    Ledger *ledger = Ledger_open(org, app);
    CHECK(ledger != nullptr, "Ledger_open returned NULL");
    if (!ledger) {
        rmtree(home);
        return 1;
    }
    CHECK(Ledger_state(ledger) == LEDGER_ABSENT, "fresh identity is not ABSENT");
    CHECK(!Ledger_hasRecord(ledger), "fresh identity reports a record");
    printf("  A. fresh identity is ABSENT\n");

    // B. Record install.
    if (!Ledger_recordInstall(ledger, "9.9.9")) {
        printf("ledger_test: SKIP (state dir not writable)\n");
        Ledger_free(ledger);
        rmtree(home);
        return B_TEST_SKIP;
    }
    Ledger_free(ledger);
    ledger = Ledger_open(org, app);
    CHECK(Ledger_state(ledger) == LEDGER_INSTALLED, "record did not read back as INSTALLED");
    char version[32];
    Ledger_lastVersion(ledger, version, sizeof(version));
    CHECK(strcmp(version, "9.9.9") == 0, "version read back as '%s'", version);
    printf("  B. recorded install reads back (version %s)\n", version);

    // C. Mark uninstalled — record survives.
    CHECK(Ledger_markUninstalled(ledger), "mark uninstalled failed");
    Ledger_free(ledger);
    ledger = Ledger_open(org, app);
    CHECK(Ledger_state(ledger) == LEDGER_UNINSTALLED, "record did not read back as UNINSTALLED");
    CHECK(Ledger_hasRecord(ledger), "uninstalled record was erased (fresh install again)");
    printf("  C. uninstall kept the record (machine still remembers)\n");

    // D. Forget purges.
    CHECK(Ledger_forget(ledger), "forget failed");
    Ledger_free(ledger);
    ledger = Ledger_open(org, app);
    CHECK(Ledger_state(ledger) == LEDGER_ABSENT, "forget did not purge the record");
    CHECK(!Ledger_hasRecord(ledger), "forget left a record");
    Ledger_free(ledger);
    printf("  D. forget purged the record\n");

    unsetenv("HOME");
    rmtree(home);

    if (s_failures == 0)
        printf("PASS ledger_test (out-of-tree record survives uninstall)\n");
    else
        fprintf(stderr, "FAIL ledger_test: %d check(s) failed\n", s_failures);
    return s_failures == 0 ? 0 : 1;
#endif
}
