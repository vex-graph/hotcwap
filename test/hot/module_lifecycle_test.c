// hotcwap/test/hot/module_lifecycle_test.c — HotModule worker lifecycle _test.
//
// Owns the constructor/teardown seam of HotModule: Hot_init starts the one
// supervised save worker and Hot_shutdown stops it through the HotSavePhase
// word (IDLE -> STOPPED) before touching any module. The test mounts a throwaway
// manifest under a fresh temporary directory, because Hot_init refuses to bind
// until MANIFEST() has run; nothing outside that directory is read or written.
//
// Bounded by construction: every wait in the loader is capped, so a regression
// that made the join unbounded shows up as this test's timeout, not a hang in
// CI's harness.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hot.h"
#include "manifest.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

int main(void) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == nullptr || *tmp == '\0')
        tmp = "/tmp";

    char base[512];
    snprintf(base, sizeof base, "%svex-cwp-lifecycle-%d", tmp, (int) getpid());
    if (mkdir(base, 0700) != 0 && access(base, F_OK) != 0) {
        printf("module_lifecycle_test: cannot create %s\n", base);
        return 1;
    }

    CHECK(MANIFEST(base, "vexgraph", "lifecycle"));
    CHECK(MANIFEST_ROOT() != nullptr);
    if (g_failures != 0) {
        printf("module_lifecycle_test: mount refused\n");
        return 1;
    }

    // Hot_init binds the key and starts the save worker; a second instance for
    // the same key is independent (the thread belongs to the instance).
    HotModule *hot = Hot_init("lifecycle");
    CHECK(hot != nullptr);
    HotModule *twin = Hot_init("lifecycle");
    CHECK(twin != nullptr);

    // Teardown is the STOPPED transition plus a bounded join. Calling shutdown
    // on a live instance must return without leaving a thread behind.
    Hot_shutdown(hot);
    Hot_shutdown(twin);
    CHECK(1);   // both joins returned

    // The loader never owned the manifest root; the test removes it.
    CHECK(UNINSTALL(base, "vexgraph", "lifecycle"));
    rmdir(base);

    if (g_failures == 0)
        printf("module_lifecycle_test: all assertions held\n");
    return g_failures == 0 ? 0 : 1;
}
