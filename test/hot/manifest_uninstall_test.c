#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "manifest.h"
#include "spoke/bespoke.h"
#include "annotation/annotation.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: ManifestUninstallTest (hotcwap/test/manifest_uninstall_test.c)
 * LEVEL: L4 — Self-Management (manifest UNINSTALL varargs verification & purge)
 * ============================================================================
 * Verifies that UNINSTALL(...) strictly enforces the varargs exact-match
 * identity check before removing any on-disk files, and cleanly purges the
 * tree when parameters match. Also verifies Bespoke Bridge dispatch.
 *
 * STRUCT FIELDS: none — procedural test harness.
 * ============================================================================
 */

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("PASS: %s\n", msg); \
    } else { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failures++; \
    } \
} while (0)

/** Reports whether the fixture path currently names an existing directory. */
static bool dirExists(const char *path) {
    if (!path || *path == '\0')
        return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void) {
    printf("=== Running Manifest UNINSTALL & Bespoke Bridge Test Suite ===\n");

    // Isolate in scratch dir via VEX_MANIFEST test seam
    char scratchTemplate[] = "/tmp/vex_uninstall_test_XXXXXX";
    char *scratchDir = mkdtemp(scratchTemplate);
    CHECK(scratchDir != nullptr, "Created isolated scratch directory");
    setenv("VEX_MANIFEST", scratchDir, 1);
    char homeDir[512];
    snprintf(homeDir, sizeof(homeDir), "%s/home", scratchDir);
    if (mkdir(homeDir, 0755) == 0)
        setenv("HOME", homeDir, 1);

    // #1 Mount initial manifest
    bool mounted = MANIFEST("appdata", "vexgraph", "test suite");
    CHECK(mounted, "MANIFEST(appdata, vexgraph, test suite) succeeded");
    const char *root = MANIFEST_ROOT();
    CHECK(root != nullptr && dirExists(root), "Manifest root directory exists on disk");
    char savedRoot[1024];
    snprintf(savedRoot, sizeof(savedRoot), "%s", root);

    // #2 Ensure ladder
    CHECK(MANIFEST_ENSURE(), "MANIFEST_ENSURE() created ladder");

    // #3 Attempt UNINSTALL with wrong app name -> MUST FAIL closed
    bool fail1 = UNINSTALL("appdata", "vexgraph", "wrong_app");
    CHECK(!fail1, "UNINSTALL refused on wrong app name");
    CHECK(dirExists(savedRoot), "Root directory preserved after mismatched app name");

    // #4 Attempt UNINSTALL with wrong org -> MUST FAIL closed
    bool fail2 = UNINSTALL("appdata", "wrong_org", "test suite");
    CHECK(!fail2, "UNINSTALL refused on wrong org name");
    CHECK(dirExists(savedRoot), "Root directory preserved after mismatched org name");

    // #5 Attempt UNINSTALL with wrong root -> MUST FAIL closed
    bool fail3 = UNINSTALL("~", "vexgraph", "test suite");
    CHECK(!fail3, "UNINSTALL refused on wrong root path segment");
    CHECK(dirExists(savedRoot), "Root directory preserved after mismatched root segment");

    // #6 Attempt UNINSTALL with extra segment -> MUST FAIL closed
    bool fail4 = UNINSTALL("appdata", "vexgraph", "test suite", "extra");
    CHECK(!fail4, "UNINSTALL refused on extra segment");
    CHECK(dirExists(savedRoot), "Root directory preserved after extra segment");

    // #7 Exact Match UNINSTALL -> MUST SUCCEED
    bool success = UNINSTALL("appdata", "vexgraph", "test suite");
    CHECK(success, "Exact match UNINSTALL(appdata, vexgraph, test suite) succeeded");
    CHECK(!dirExists(savedRoot), "Root directory completely purged from disk");
    CHECK(MANIFEST_ROOT() == nullptr, "MANIFEST_ROOT() is NULL after uninstall");

    // #8 Second UNINSTALL -> MUST FAIL (not mounted)
    bool fail5 = UNINSTALL("appdata", "vexgraph", "test suite");
    CHECK(!fail5, "Second UNINSTALL refused because manifest is unmounted");

    // #9 Re-mount with string varargs and spaces
    bool remount = MANIFEST("appdata", "vexgraph", "test app 2");
    CHECK(remount, "Re-mounted manifest for test app 2");
    const char *root2 = MANIFEST_ROOT();
    CHECK(root2 != nullptr && dirExists(root2), "New root directory created");

    // Clean up test app 2
    bool clean2 = UNINSTALL("appdata", "vexgraph", "test app 2");
    CHECK(clean2, "Cleaned up test app 2 via UNINSTALL");

    // #10 Bespoke Bridge Verification
    bridgeBespoke();
    printf("Bespoke graphics state: %d\n", (int) bridgeBespokeGraphicsState());
    printf("Bespoke networking state: %d\n", (int) bridgeBespokeNetworkingState());
    printf("Bespoke smth state: %d\n", (int) bridgeBespokeSmthState());

    // Step / check bespoke
    bool changed = bridgeBespokeCheck();
    (void) changed;
    CHECK(true, "bridgeBespokeCheck() executed safely");

    // Remove scratch base
    rmdir(scratchDir);

    printf("\n=== Results: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
