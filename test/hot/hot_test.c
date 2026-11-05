// hotcwap/test/hot/hot_test.c — the HotModule class _test (cold-seam contract).
//
// Owns the loader's degradation contract (the Cold-Strict, Hot-Minimal
// Validation Law): every entry point on a NULL / malformed handle returns the
// documented safe value instead of crashing. The full swap lifecycle
// (two_dylib_swap, manifest_hot, wrong_binary, retire_ring_overflow,
// shutdown_order) is proven by the ledger seams; this owns the guards they
// assume.
//
// Headless, deterministic, no dylibs, no manifest.

#include <stdio.h>
#include <string.h>

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
    // Hot_init rejects NULL, empty, and over-long library keys.
    CHECK(Hot_init(nullptr) == nullptr);
    CHECK(Hot_init("") == nullptr);
    char big[HOT_MANIFEST_MAX_NAME + 8];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK(Hot_init(big) == nullptr);

    // Hot_shutdown on NULL is a no-op.
    Hot_shutdown(nullptr);
    CHECK(1);

    // Hot_poll on NULL reports the cold-seam error and zeroes the count.
    uint32_t n = 99;
    CHECK(Hot_poll(nullptr, &n) == HOT_ERROR_FILE_NOT_FOUND);
    CHECK(n == 0);
    CHECK(Hot_poll(nullptr, nullptr) == HOT_ERROR_FILE_NOT_FOUND);

    // Symbol lookup on NULL is NULL.
    CHECK(Hot_getSymbol(nullptr, "Hot_getExports") == nullptr);

    // Generation on NULL is 0.
    CHECK(Hot_getGeneration(nullptr) == 0);

    // Last error on NULL is a stable, non-null string.
    const char *err = Hot_getLastError(nullptr);
    CHECK(err != nullptr && err[0] != '\0');

    // Module state ops on NULL are safely refused.
    char buf[32];
    size_t len = 0;
    CHECK(!Hot_saveModule(nullptr, "m", buf, sizeof buf, &len));
    CHECK(!Hot_restoreModule(nullptr, "m", buf, sizeof buf));
    CHECK(!Hot_migrateModule(nullptr, "m", "1.0.0", buf, sizeof buf, buf, sizeof buf, &len));
    Hot_shutdownModule(nullptr, "m");
    CHECK(1);

    if (g_failures == 0) {
        printf("hot_test: all assertions held\n");
        return 0;
    }
    printf("hot_test: %d FAILURES\n", g_failures);
    return 1;
}
