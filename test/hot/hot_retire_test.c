// hotcwap/test/hot/hot_retire_test.c — the HotRetireRing _test.
//
// Proves the generational dlclose grace ring:
//   - NULL self and NULL handle are no-ops (never dlclose garbage).
//   - retire parks a handle at the current generation.
//   - advance within HOT_RETIRED_GENERATIONS keeps it parked.
//   - advance past the grace window closes it and frees the slot.
//   - drainAll closes anything still parked.
//
// The 16-slot overflow/eviction path is proven end-to-end by
// retire_ring_overflow_test (real swaps); this owns the grace bookkeeping.
// A real, already-loaded system library is used with RTLD_NODELETE so dlclose
// never unloads it (safe to close repeatedly).

#include <dlfcn.h>
#include <stddef.h>
#include "test_support.h"
#include <stdio.h>

#include "hot/retire_ring.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/** Counts occupied entries retained in the retirement ring. */
static int parkedCount(const HotRetireRing *ring) {
    int n = 0;
    for (bytes i = 0; i < HOT_RETIRED_MAX; i++) {
        const HotRetiredHandle *slot = &(*ring).slots[i];
        if ((*slot).handle)
            n++;
    }
    return n;
}

int main(void) {
    // NULL-self safety: every entry point is a no-op.
    HotRetireRing_retire(nullptr, (void*) 0x1);
    HotRetireRing_advance(nullptr);
    HotRetireRing_drainAll(nullptr);
    CHECK(1);

    static HotRetireRing ring; // zero-initialized

    // NULL handle parks nothing.
    HotRetireRing_retire(&ring, nullptr);
    CHECK(parkedCount(&ring) == 0);

    void *h = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW | RTLD_NODELETE);
    if (!h) {
        printf("hot_retire_test: SKIP (no dlopen handle available)\n");
        return B_TEST_SKIP;
    }

    HotRetireRing_retire(&ring, h);
    CHECK(parkedCount(&ring) == 1);
    CHECK(ring.generation == 0);

    // Within the grace window the handle stays parked.
    HotRetireRing_advance(&ring);
    HotRetireRing_advance(&ring);
    HotRetireRing_advance(&ring);
    CHECK(parkedCount(&ring) == 1);

    // Past HOT_RETIRED_GENERATIONS it is closed and the slot freed.
    HotRetireRing_advance(&ring);
    CHECK(parkedCount(&ring) == 0);

    // drainAll closes anything still parked.
    HotRetireRing_retire(&ring, h);
    CHECK(parkedCount(&ring) == 1);
    HotRetireRing_drainAll(&ring);
    CHECK(parkedCount(&ring) == 0);

    if (g_failures == 0) {
        printf("hot_retire_test: all assertions held\n");
        return 0;
    }
    printf("hot_retire_test: %d FAILURES\n", g_failures);
    return 1;
}
