/* MODULE: cold-update owner. Real two-section dylibs, exact SHA256 references,
 * synchronous state handoff, whole-set rejection, disk revert and recovery.
 * Caller excludes users for each update. No concurrent loader/sandbox claim. */
#include "hot.h"
#include "manifest.h"
#include "security/crypto.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void path(const char *base, const char *name, char *dest) {
    assert(snprintf(dest, 1024, "%s/%s", base, name) < 1024);
}

static void copy(const char *source, const char *dest) {
    FILE *in = fopen(source, "rb"), *out = fopen(dest, "wb");
    assert(in && out);
    unsigned char bytes[4096];
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), in)))
        assert(fwrite(bytes, 1, n, out) == n);
    assert(!ferror(in));
    assert(fclose(in) == 0 && fclose(out) == 0);
}

static void digest(const char *source, uint8_t *dest) {
    FILE *in = fopen(source, "rb");
    assert(in);
    CryptoSha256 hash;
    Crypto_sha256Init(&hash);
    unsigned char bytes[4096];
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), in)))
        Crypto_sha256Update(&hash, bytes, n);
    assert(!ferror(in) && fclose(in) == 0);
    Crypto_sha256Final(&hash, dest);
}

static void prepare(const char *base, const char *version, const char *payload, HotSectionDigest *specs) {
    for (size_t i = 0; i < 2; ++i) {
        HotSectionDigest *item = &specs[i];
        char dir[1024], source[1024], dest[1024];
        path(base, version, dir);
        path(dir, (*item).filename, source);
        path(payload, (*item).filename, dest);
        copy(source, dest);
        digest(source, (*item).sha256);
    }
}

static int value(HotModule *hot, const char *name) {
    HotFn fn = Hot_getSymbol(hot, name);
    assert(fn);
    return ((int (*)(void)) fn)();
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("SKIP: real dylib fixtures need a fixture driver that is not in the tree\n", stderr);
        return 77;
    }
    const char *base = argv[1];
    char payload[1024];
    path(base, "payload", payload);
    assert(mkdir(payload, 0700) == 0);
    assert(setenv("VEX_MANIFEST", base, 1) == 0);
    assert(setenv("HOME", base, 1) == 0);
    assert(MANIFEST(MANIFEST_APP_DATA, "vexgraph", "cold-update"));
    assert(MANIFEST_LIBRARY("agent", (const char*) nullptr));
    assert(MANIFEST_ENSURE());
    HotSectionDigest specs[2] = {{.filename = "one.so"}, {.filename = "two.so"}};
    prepare(base, "v1", payload, specs);
    assert(MANIFEST_REFLECT("agent", payload));
    assert(MANIFEST_SECTION_COUNT("agent") == 2);
    assert(MANIFEST_SECTION_COUNT(nullptr) == 0);
    assert(MANIFEST_ALLOWS("agent", "one.so"));
    assert(!MANIFEST_ALLOWS("agent", "rogue.so"));
    HotModule *hot = Hot_init("agent");
    assert(hot);
    assert(!Hot_requiresRecovery(hot) && !Hot_requiresRecovery(nullptr));
    assert(!MANIFEST_REQUIRES_RECOVERY());
    uint32_t loaded = 99;
    assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_OK && loaded == 2);
    assert(value(hot, "one_version") == 1 && value(hot, "two_version") == 1);
    ((void (*)(int)) Hot_getSymbol(hot, "one_set"))(77);
    ((void (*)(int)) Hot_getSymbol(hot, "two_set"))(88);
    uint64_t generation = Hot_getGeneration(hot);
    for (int i = 0; i < 20; ++i) {
        prepare(base, i % 2 ? "v1" : "v2", payload, specs);
        assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_OK && loaded == 2);
        assert(Hot_getGeneration(hot) == ++generation);
        assert(value(hot, "one_version") == (i % 2 ? 1 : 2));
        assert(value(hot, "two_version") == (i % 2 ? 1 : 2));
        assert(value(hot, "one_state") == 77 && value(hot, "two_state") == 88);
    }
    specs[1].sha256[0] ^= 1;
    assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_ERROR_ABI_MISMATCH && loaded == 0);
    assert(MANIFEST_GENERATION("agent") == generation);
    assert(Hot_updateCold(hot, payload, specs, 1, &loaded) != HOT_OK && loaded == 0);
    assert(Hot_updateCold(nullptr, payload, specs, 2, &loaded) != HOT_OK);
    prepare(base, "bad-abi", payload, specs);
    assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_ERROR_ABI_MISMATCH);
    assert(MANIFEST_GENERATION("agent") == generation && Hot_getGeneration(hot) == generation);
    assert(value(hot, "one_state") == 77 && value(hot, "two_state") == 88);
    prepare(base, "bad-restore", payload, specs);
    assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_ERROR_RESTORE_FAILED);
    assert(MANIFEST_GENERATION("agent") == generation && Hot_getGeneration(hot) == generation);
    assert(value(hot, "one_state") == 77 && value(hot, "two_state") == 88);
    prepare(base, "v2", payload, specs);
    assert(Hot_updateCold(hot, payload, specs, 2, &loaded) == HOT_OK);
    assert(Hot_getGeneration(hot) == ++generation);
    assert(value(hot, "one_state") == 77 && value(hot, "two_state") == 88);
    // Direct placement cannot bypass the loader allowlist on initial admission.
    char current[1024], rogue[1024], source[1024];
    assert(ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, "agent", current, sizeof(current), false));
    path(current, "rogue.so", rogue);
    path(payload, "one.so", source);
    copy(source, rogue);
    HotModule *probe = Hot_init("agent");
    assert(probe);
    assert(Hot_poll(probe, &loaded) == HOT_ERROR_ABI_MISMATCH && loaded == 0);
    assert(Hot_getGeneration(probe) == 0);
    Hot_shutdown(probe);
    assert(unlink(rogue) == 0);
    // Force a detected stamp publication failure; every completed move is undone.
    assert(MANIFEST_UPDATE("agent", payload));
    char stamp[1024], blocked[1040];
    assert(ManifestPath_generationFile("agent", stamp, sizeof(stamp)));
    assert(snprintf(blocked, sizeof(blocked), "%s.tmp", stamp) < (int) sizeof(blocked));
    assert(mkdir(blocked, 0700) == 0);
    assert(!MANIFEST_PROMOTE_LIBRARY("agent"));
    assert(MANIFEST_GENERATION("agent") == generation);
    assert(rmdir(blocked) == 0);
    assert(MANIFEST_PROMOTE_LIBRARY("agent"));
    assert(MANIFEST_REVERT_LIBRARY("agent", generation));
    assert(MANIFEST_GENERATION("agent") == generation);
    Hot_shutdown(hot);
    puts("cold_update_test: real two-section swaps, checksums, ABI, state, rollback and recovery passed");
}
