#include "hot/staged_section.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hot/internal.h"
#include "manifest.h"
#include "security/crypto.h"

// hot/staged_section.c — map a candidate image and prove it is admissible.
//
// Everything here happens BEFORE commit. That is the point: a section can be
// copied, hashed, dlopen'd and inspected, and then thrown away for the cost of
// a dlclose. Nothing in the live table has moved yet, so a rejection needs no
// rollback.

static u32 s_loadSerial = 0;

u32 HotStaging_nextSerial(void) {
    return ++s_loadSerial;
}

// mkdir -p, bounded by HOT_PATH_LEN.
bool HotStaging_mkdirs(const char *path) {
    char tmp[HOT_PATH_LEN];
    bytes len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp))
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

bool HotStaging_copy(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    unsigned char buf[8192];
    bytes n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    fclose(out);
    fclose(in);
    return ok;
}

// Hash the private load copy, not a previously checked pathname. Stable trusted
// parents still required; this is not protection against same-user mutation.
bool HotStaging_digestMatches(const char *path, const u8 expected[32]) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr)
        return false;
    CryptoSha256 hash;
    Crypto_sha256Init(&hash);
    u8 buffer[4096], actual[32];
    bytes n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
        Crypto_sha256Update(&hash, buffer, n);
    bool ok = !ferror(file);
    if (fclose(file) != 0)
        ok = false;
    Crypto_sha256Final(&hash, actual);
    return ok && memcmp(actual, expected, sizeof(actual)) == 0;
}

const HotSectionDigest *HotStaging_coldDigest(HotModule *hot, const char *name) {
    for (bytes i = 0; i < (*hot).coldCount; ++i) {
        const HotSectionDigest *digest = &(*hot).coldDigests[i];
        if ((*digest).filename != nullptr && strcmp((*digest).filename, name) == 0)
            return digest;
    }
    return nullptr;
}

// True when `name` is an image the loader should map.
bool HotStagedSection_isImage(const char *name) {
    bytes nlen = strlen(name);
    return (nlen > 6 && strcmp(name + nlen - 6, ".dylib") == 0) ||
           (nlen > 3 && strcmp(name + nlen - 3, ".so") == 0);
}

// Map, verify and describe one section. On failure `hot->last_error` is set
// and any partial private copy or handle is already cleaned up, so the caller
// only has to close the rows it staged earlier.
HotResult HotStagedSection_admit(HotModule *hot, const char *libDir,
                                 const char *name, HotStagedSection *out) {
    char path[HOT_PATH_LEN];
    int plen = snprintf(path, sizeof(path), "%s/%s", libDir, name);
    if (plen < 0 || (bytes) plen >= sizeof(path)) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "Section path '%s/%s' truncated", libDir, name);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    // dyld caches a loaded image by path, so a replaced file at the SAME path
    // is NOT re-read — the swap would silently keep the old code. Load from a
    // per-swap unique copy (Generation-Unique Load Path) so every generation
    // is a genuinely fresh image.
    char loadDir[HOT_PATH_LEN];
    char loadPath[HOT_PATH_LEN];
    if ((*hot).coldCount) {
        snprintf(loadDir, sizeof(loadDir), "%s", libDir);
        snprintf(loadPath, sizeof(loadPath), "%s", path);
    } else {
        snprintf(loadDir, sizeof(loadDir), "%s/cache/load/%s/%u",
                 MANIFEST_ROOT(), (*hot).library, HotStaging_nextSerial());
        snprintf(loadPath, sizeof(loadPath), "%s/%s", loadDir, name);
    }
    if (!(*hot).coldCount && (!HotStaging_mkdirs(loadDir) || !HotStaging_copy(path, loadPath))) {
        unlink(loadPath);
        rmdir(loadDir);
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "cannot stage a unique load copy for %s", name);
        return HOT_ERROR_FILE_NOT_FOUND;
    }

    const HotSectionDigest *digest = HotStaging_coldDigest(hot, name);
    if ((*hot).coldCount && (digest == nullptr || !HotStaging_digestMatches(loadPath, (*digest).sha256))) {
        unlink(loadPath);
        rmdir(loadDir);
        snprintf((*hot).last_error, sizeof((*hot).last_error), "checksum mismatch: %s", name);
        return HOT_ERROR_ABI_MISMATCH;
    }

    void *handle = dlopen(loadPath, RTLD_NOW | RTLD_LOCAL);
    // dlopen retains the mapped image; this unique private copy is transport
    // only, not a resource root. Reclaim it on success and rejection rather
    // than accumulating releases.
    unlink(loadPath);
    rmdir(loadDir);
    if (!handle) {
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "dlopen(%s) failed: %s", loadPath, dlerror());
        return HOT_ERROR_DLOPEN_FAILED;
    }

    const u64 *abi = (*hot).coldCount ? dlsym(handle, "Hot_cold_abi") : nullptr;
    if ((*hot).coldCount && (abi == nullptr || *abi != HOT_COLD_ABI_VERSION)) {
        dlclose(handle);
        snprintf((*hot).last_error, sizeof((*hot).last_error), "cold ABI mismatch: %s", name);
        return HOT_ERROR_ABI_MISMATCH;
    }

    typedef const void *(*TrampolinesFn)(u32*);
    TrampolinesFn get_trampolines = (TrampolinesFn) dlsym(handle, "Hot_getExports");
    if (!get_trampolines) {
        dlclose(handle);
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "dlsym(Hot_getExports) on %s failed: %s", path, dlerror());
        return HOT_ERROR_DLSYM_FAILED;
    }

    // Section stem = file name without extension, plus the manifest catalog's
    // stem rule ("libbar.so" -> "bar"), so loader slots and declared sections
    // agree on the same identity.
    char mod_name[HOT_MANIFEST_MAX_NAME];
    strncpy(mod_name, name, HOT_MANIFEST_MAX_NAME - 1);
    mod_name[HOT_MANIFEST_MAX_NAME - 1] = '\0';
    char *dot = strrchr(mod_name, '.');
    if (dot)
        (*dot) = '\0';
    bytes nlen = strlen(name);
    if (nlen > 6 && strcmp(name + nlen - 3, ".so") == 0 && strncmp(mod_name, "lib", 3) == 0)
        memmove(mod_name, mod_name + 3, strlen(mod_name + 3) + 1);

    HotModuleInternal *mod = HotModuleInternal_adopt(hot, mod_name);
    if (!mod) {
        dlclose(handle);
        snprintf((*hot).last_error, sizeof((*hot).last_error),
                 "Too many modules (limit %d reached)", HOT_MAX_MODULES);
        return HOT_ERROR_OUT_OF_MEMORY;
    }

    (*out).mod = mod;
    strncpy((*out).path, path, HOT_PATH_LEN - 1);
    (*out).path[HOT_PATH_LEN - 1] = '\0';
    (*out).handle = handle;
    (*out).trampolines = get_trampolines(&(*out).trampolineCount);

    // Every exported row must name a real function. A section that exports an
    // empty or half-filled table is refused before anything is adopted.
    const struct { const char *name; void *fn; } *exports = (*out).trampolines;
    bool valid = exports != nullptr && (*out).trampolineCount > 0;
    for (u32 k = 0; valid && k < (*out).trampolineCount; ++k)
        valid = exports[k].name != nullptr && exports[k].fn != nullptr &&
                exports[k].name[0] != '\0' &&
                strnlen(exports[k].name, HOT_MANIFEST_MAX_NAME) < HOT_MANIFEST_MAX_NAME;
    if (!valid) {
        dlclose(handle);
        snprintf((*hot).last_error, sizeof((*hot).last_error), "invalid export table: %s", name);
        return HOT_ERROR_ABI_MISMATCH;
    }

    return HOT_OK;
}
