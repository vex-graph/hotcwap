#include "hot/manifest.h"
#include "hot/ledger.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#include "platform/platform.h"

#if defined(VEX_PLATFORM_WINDOWS)
#  include <io.h>
#  include <windows.h>
#  define RENAME(a, b) (MoveFileExA((a), (b), MOVEFILE_REPLACE_EXISTING) != 0)
#  define UNLINK(a)    _unlink(a)
#else
#  include <unistd.h>
#  define RENAME(a, b) (rename((a), (b)) == 0)
#  define UNLINK(a)    unlink(a)
#endif

#include "annotation/definition.h"
#include "annotation/overview.h"
#include "annotation/getter.h"
#include "annotation/setter.h"
#include "annotation/intention.h"
#include "exception/throw.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: ManifestPath
 * ============================================================================
 * Authority for on-disk application installation layouts, catalog reflection, and
 * multi-generation hot-reload binary ladders. Establishes the filesystem structure
 * under platform-standard application-data paths, locking the installation root
 * and managing the manifest.json catalog.
 *
 * Implements the atomic generation slide across ladder directories (backward, previous,
 * current, new). The monotonic generation stamp (bin/current/<lib>.generation) drives
 * in-process dynamic module reloading, guaranteeing fail-closed updates and seamless
 * rollback capabilities without runtime file locking contention.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: ManifestPath (hot/manifest.c)
 * ============================================================================
 * SUMMARY:
 *   The MANIFEST(...) install-layout authority — the "manifest binary way".
 *   The manifest IS the on-disk install tree PLUS the manifest.json library
 *   catalog. MANIFEST(...) resolves <application-base>/<org>/<app> from the
 *   platform application-data base, locks it once, and seeds manifest.json
 *   ({name, version, org, libraries{}}) when absent. MANIFEST_LIBRARY(...)
 *   registers the hosted library KEYS (empty section lists — the downloader
 *   owns the arrays). The ladder holds ONE generation set PER LIBRARY and the
 *   verbs verify against the DECLARED section lists fail-closed: MANIFEST_UPDATE
 *   stages into bin/new/<library>, MANIFEST_PROMOTE slides each library's
 *   generations, MANIFEST_REFLECT seeds a library's sections on first-run.
 *
 * STRUCT FIELDS (Mirroring hot/manifest.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char  *buf;    // caller-owned destination buffer
 *   size_t cap;    // capacity of buf
 *   size_t len;    // current path length, excluding the trailing NUL
 *
 * PRIVATE HELPERS (file-local pure-data only, each with full fields):
 * ----------------------------------------------------------------------------
 *   static char   g_root[MANIFEST_BUF_CAP];   // the locked install root
 *   static bool   g_mounted;                  // MANIFEST() one-time guard
 *   static char   g_name[HOT_MANIFEST_MAX_NAME];    // catalog/name
 *   static char   g_version[HOT_MANIFEST_MAX_NAME]; // catalog/version
 *   static char   g_org[HOT_MANIFEST_MAX_NAME];     // catalog/org
 *   static ManifestLibrary g_libraries[HOT_MANIFEST_MAX_LIBRARIES]; // catalog rows
 *        char name[HOT_MANIFEST_MAX_NAME];                 // library key
 *        uint32_t sectionCount;                            // declared stems
 *        char sections[HOT_MANIFEST_MAX_SECTIONS][HOT_MANIFEST_MAX_NAME];
 *   static uint32_t g_libraryCount;   // catalog rows in use
 *   static JsonIn   { const char *p; const char *end; }    // bounded cursor
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - ManifestPath_0(dest, cap)               : Bind builder to buffer
 *   - Manifest_init(first, ...)               : ONE-TIME initializer (MANIFEST macro)
 *   - Manifest_uninstall(first, ...)          : Complete uninstall (UNINSTALL macro)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - MANIFEST_LIBRARY(first, ...)            : Register hosted library keys
 *   - MANIFEST_ENSURE()                       : Create complete directory ladder
 *   - MANIFEST_REFLECT(library, sourceDir)    : First-run payload reflection
 *   - MANIFEST_UPDATE(library, payloadDir)    : Verify and stage payload
 *   - MANIFEST_PROMOTE()                      : Atomic generation slide
 *   - MANIFEST_PROMOTE_LIBRARY(library)        : Cold one-library undoable slide
 *   - MANIFEST_REVERT_LIBRARY(library, generation) : Restore prior cold set
 *   - MANIFEST_ALLOWS(library, filename)       : Catalog check at load admission
 *   - MANIFEST_SECTION_COUNT(library)          : Mounted declared section count
 *   - MANIFEST_REQUIRES_RECOVERY(void)         : Unresolved cold disk undo latch
 *   - MANIFEST_CLEAN(ladder, library)         : Clear specific ladder directory
 *   - MANIFEST_SECTION_LIST(lib, out, cap)    : Retrieve declared section stems
 *   - ManifestPath_append(self, segment)      : Append path component
 *   - ManifestPath_str(self)                  : Finalize and return path string
 *
 * Private Core Functions: (.c static)
 *   - valid_name(name)                        : Validate path safety
 *   - dir_exists(path)                        : Query directory presence
 *   - file_exists(path)                       : Query file presence
 *   - dir_mkdir(path)                         : Single directory creation
 *   - mkdir_p(path)                           : Recursive directory creation
 *   - read_file(path, buf, cap, len)          : Read whole file to buffer
 *   - resolve_base(out, cap, kind)            : Resolve system application directory
 *   - copy_file(src, dst)                     : Byte-level file copy
 *   - copy_tree(srcDir, dstDir, depth)        : Recursive tree copy
 *   - remove_ladder_dir(dir)                  : Atomic directory removal
 *   - stem_of(file, out, cap)                 : Extract stem from filename
 *   - library_declares(lib, stem)             : Verify stem declaration
 *   - catalog_lookup(name)                    : Lookup library in catalog
 *   - catalog_ensure(name)                    : Add or find library in catalog
 *   - generation_read(path)                   : Parse generation stamp
 *   - generation_write(path, value)           : Atomic generation write
 *   - catalog_write_json()                    : Serialize catalog to JSON
 *   - catalog_seed()                          : Seed initial catalog structure
 *   - catalog_save()                          : Persist catalog to disk
 *   - catalog_load()                          : Parse catalog from disk
 *   - stage_has_content(dir)                  : Test if staging directory is non-empty
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - MANIFEST_ROOT()                         : Get locked install root path
 *   - MANIFEST_IS_FIRST_RUN()                 : Test if initial install
 *   - MANIFEST_GENERATION(library)            : Query current library generation
 *   - MANIFEST_LIBRARY_COUNT()                : Number of registered libraries
 *   - MANIFEST_LIBRARY_NAME(index)            : Library key by index
 *   - MANIFEST_SECTION_COUNT(library)         : Number of sections in library
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

#define MANIFEST_BUF_CAP 1024
#define MANIFEST_JSON_CAP 16384
#define MANIFEST_BIN "bin"
#define MANIFEST_CACHE "cache"
#define MANIFEST_JSON "manifest.json"
#define MANIFEST_GENERATION_EXT ".generation"
#define MANIFEST_VERSION_DEFAULT "0.1.0"
#define SLOT_NAMES { "backward", "previous", "current", "new" }
#define ROTATION_DIR  ".promote_rot"
#define JSON_DEPTH_MAX 32

typedef struct ManifestLibrary {
    char name[HOT_MANIFEST_MAX_NAME];
    uint32_t sectionCount;
    char sections[HOT_MANIFEST_MAX_SECTIONS][HOT_MANIFEST_MAX_NAME];
} ManifestLibrary;

typedef struct JsonIn {
    const char *p;
    const char *end;
} JsonIn;

#define MANIFEST_MAX_SEGMENTS 16

static char g_root[MANIFEST_BUF_CAP];
static bool g_mounted = false;
static bool g_recoveryRequired = false;
static char g_name[HOT_MANIFEST_MAX_NAME];
static char g_version[HOT_MANIFEST_MAX_NAME];
static char g_org[HOT_MANIFEST_MAX_NAME];
static ManifestLibrary g_libraries[HOT_MANIFEST_MAX_LIBRARIES];
static uint32_t g_libraryCount = 0;
static char g_segments[MANIFEST_MAX_SEGMENTS][HOT_MANIFEST_MAX_NAME];
static uint32_t g_segmentCount = 0;

// --- private helpers ---------------------------------------------------------

static bool valid_name(const char *name) {
    if (name == nullptr || *name == '\0')
        return false;
    size_t n = strlen(name);
    if (n >= HOT_MANIFEST_MAX_NAME)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) name[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.' || c == ' '))
            return false;
    }
    return true;
}

static bool dir_exists(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    struct stat st;
    memset(&st, 0, sizeof(st));
    return stat(path, &st) == 0 && S_ISDIR((unsigned int) st.st_mode);
}

static bool file_exists(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    struct stat st;
    memset(&st, 0, sizeof(st));
    return stat(path, &st) == 0 && S_ISREG((unsigned int) st.st_mode);
}

// Extract the first double-quoted string value for a JSON key, tolerant of
// whitespace around the colon. Bounded; the ownership guard only.
static bool catalog_field(const char *buf, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    const char *p = buf;
    while ((p = strchr(p, '"')) != nullptr) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char *q = strchr(p + 2 + klen, ':');
            if (q == nullptr)
                return false;
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q != '"')
                return false;
            q++;
            size_t n = 0;
            while (*q != '\0' && *q != '"' && n + 1 < cap)
                out[n++] = *q++;
            out[n] = '\0';
            return true;
        }
        p++;
    }
    return false;
}

// Destructive-target guard — the SAME rule gates MANIFEST() creation and
// UNINSTALL() deletion, so what cannot be created cannot be destroyed. A root
// is acceptable only when it is not a protected location (the filesystem root
// or HOME itself) and, if it already exists, is a manifest tree that declares
// THIS application — its own manifest.json with matching name AND org. That
// marker is written at mount and lives for the install's life; a stray or
// foreign manifest.json does not make a directory ours. The manifest never
// adopts a directory it did not create (~/Documents, /System, ...).
static bool manifest_root_guard(const char *root) {
    if (root == nullptr || *root == '\0')
        return false;
    const char *homeEnv = getenv("HOME");
    if (strcmp(root, "/") == 0)
        return false;
    if (homeEnv && *homeEnv != '\0' && strcmp(root, homeEnv) == 0)
        return false;
    if (!dir_exists(root))
        return true;   // fresh: we are about to create it

    char catalog[MANIFEST_BUF_CAP];
    snprintf(catalog, sizeof(catalog), "%s/" MANIFEST_JSON, root);
    FILE *f = fopen(catalog, "rb");
    if (f == nullptr)
        return false;
    char buf[MANIFEST_JSON_CAP];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char name[HOT_MANIFEST_MAX_NAME];
    char org[HOT_MANIFEST_MAX_NAME];
    if (!catalog_field(buf, "name", name, sizeof(name)))
        return false;
    if (!catalog_field(buf, "org", org, sizeof(org)))
        return false;
    return strcmp(name, g_name) == 0 && strcmp(org, g_org) == 0;
}

// Read a base-10 generation stamp. Returns 0 when the file is missing or
// unparsable (a fresh library is generation 0 → first seed writes 1).
static uint64_t generation_read(const char *path) {
    if (path == nullptr || *path == '\0')
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char buf[32];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    uint64_t v = 0;
    for (size_t i = 0; i < n && isdigit((unsigned char) buf[i]); i++)
        v = v * 10 + (uint64_t) (buf[i] - '0');
    return v;
}

// Write a base-10 generation stamp via temp-file + rename (atomic enough for
// the loader's read-one-integer contract — a torn read degrades to a stale
// generation, which self-heals on the next poll).
static bool generation_write(const char *path, uint64_t value) {
    if (path == nullptr || *path == '\0')
        return false;
    char tmp[MANIFEST_BUF_CAP];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    bool written = fprintf(f, "%llu\n", (unsigned long long) value) > 0;
    if (fclose(f) != 0)
        written = false;
    if (!written) {
        UNLINK(tmp);
        return false;
    }
    if (!RENAME(tmp, path)) {
        UNLINK(tmp);
        return false;
    }
    return true;
}

static bool dir_mkdir(const char *path) {
    if (dir_exists(path))
        return true;
#if defined(VEX_PLATFORM_WINDOWS)
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0755);
#endif
    if (rc != 0 && errno != EEXIST)
        return false;
    return dir_exists(path);
}

// mkdir -p: walk each '/' prefix and create it in order.
static bool mkdir_p(const char *path) {
    if (path == nullptr || *path == '\0')
        return false;
    char tmp[MANIFEST_BUF_CAP];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';
    size_t i = 1;
    for (; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            if (!dir_mkdir(tmp))
                return false;
            tmp[i] = '/';
        }
    }
    return dir_mkdir(tmp);
}

static bool read_file(const char *path, char *buf, size_t cap, size_t *lenOut) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = true;
    if (fseek(f, 0, SEEK_END) != 0) {
        ok = false;
    } else {
        long sz = ftell(f);
        if (sz < 0 || (size_t) sz > cap - 1) {
            ok = false;
        } else if (fseek(f, 0, SEEK_SET) != 0) {
            ok = false;
        } else {
            size_t n = fread(buf, 1, (size_t) sz, f);
            if (n != (size_t) sz)
                ok = false;
            else {
                buf[n] = '\0';
                if (lenOut)
                    *lenOut = n;
            }
        }
    }
    fclose(f);
    return ok;
}

static bool resolve_base(char *out, size_t cap, const char *root) {
    if (!root || *root == '\0')
        return false;

    uintptr_t val = (uintptr_t) root;
    if (val == 0)
        root = "/";
    else if (val == 1)
        root = "~";
    else if (val == 2)
        root = "appdata";

    if (strcmp(root, "/") == 0) {
#if defined(VEX_PLATFORM_WINDOWS)
        snprintf(out, cap, "%s", "C:");
#else
        snprintf(out, cap, "%s", "/");
#endif
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || *home == '\0')
        home = ".";
    if (strcmp(root, "~") == 0) {
        snprintf(out, cap, "%s", home);
        return true;
    }
    if (strcmp(root, "appdata") == 0 || strcmp(root, APPLICATION_DATA) == 0) {
        const char *override = getenv("VEX_MANIFEST");
        if (override && *override != '\0') {
            snprintf(out, cap, "%s", override);
            return true;
        }
#if defined(VEX_PLATFORM_WINDOWS)
        const char *local = getenv("LOCALAPPDATA");
        if (local && *local != '\0')
            snprintf(out, cap, "%s", local);
        else {
            const char *profile = getenv("USERPROFILE");
            snprintf(out, cap, "%s", (profile && *profile != '\0') ? profile : home);
        }
#elif defined(VEX_PLATFORM_APPLE)
        snprintf(out, cap, "%s/%s", home, APPLICATION_PATH);
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg != '\0')
            snprintf(out, cap, "%s", xdg);
        else
            snprintf(out, cap, "%s/%s", home, APPLICATION_PATH);
#endif
        return true;
    }
    snprintf(out, cap, "%s", root);
    return true;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t buf[4096];
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

static bool copy_tree(const char *srcDir, const char *dstDir, int depth) {
    if (depth > 8)
        return false;
    DIR *dir = opendir(srcDir);
    if (!dir)
        return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr && ok) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char src[MANIFEST_BUF_CAP];
        char tmp[MANIFEST_BUF_CAP];
        char dst[MANIFEST_BUF_CAP];
        snprintf(src, sizeof(src), "%s/%s", srcDir, name);
        snprintf(dst, sizeof(dst), "%s/%s", dstDir, name);
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (lstat(src, &st) != 0) {
            ok = false;
            break;
        }
        if (S_ISDIR((unsigned int) st.st_mode)) {
            if (!dir_mkdir(dst)) {
                ok = false;
                break;
            }
            if (!copy_tree(src, dst, depth + 1)) {
                ok = false;
                break;
            }
        } else if (S_ISREG((unsigned int) st.st_mode)) {
            snprintf(tmp, sizeof(tmp), "%s/.%s.stage", dstDir, name);
            if (!copy_file(src, tmp)) {
                ok = false;
                break;
            }
            if (!RENAME(tmp, dst)) {
                UNLINK(tmp);
                ok = false;
                break;
            }
        }
    }
    closedir(dir);
    return ok;
}

// Recursive rm — rename-based: rename dir to a hidden sibling then sweep
// everything under it. No system(), bounded per-entry unlinks.
static void remove_ladder_dir(const char *dir) {
    if (!dir_exists(dir))
        return;
    char tomb[MANIFEST_BUF_CAP];
    snprintf(tomb, sizeof(tomb), "%s" ROTATION_DIR, dir);
    RENAME(dir, tomb);
    if (!dir_exists(tomb))
        return;
    DIR *d = opendir(tomb);
    if (!d)
        return;
    struct dirent *ent;
    char entry[MANIFEST_BUF_CAP];
    while ((ent = readdir(d)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        snprintf(entry, sizeof(entry), "%s/%s", tomb, name);
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (lstat(entry, &st) == 0 && S_ISDIR((unsigned int) st.st_mode))
            remove_ladder_dir(entry);
        else
            UNLINK(entry);
    }
    closedir(d);
    rmdir(tomb);
}

// Dylib stem of a payload entry name: "io.dylib"→"io", "foo.dll"→"foo",
// "libbar.so"→"bar", a bare folder name stays as-is.
static bool stem_of(const char *file, char *out, size_t cap) {
    if (file == nullptr || *file == '\0' || cap == 0)
        return false;
    size_t n = strlen(file);
    if (n >= HOT_MANIFEST_MAX_NAME)
        return false;
    char buf[HOT_MANIFEST_MAX_NAME];
    snprintf(buf, sizeof(buf), "%s", file);
    if (n > 6 && strcmp(buf + n - 6, ".dylib") == 0)
        n -= 6;
    else if (n > 4 && strcmp(buf + n - 4, ".dll") == 0)
        n -= 4;
    else if (n > 3 && strcmp(buf + n - 3, ".so") == 0) {
        n -= 3;
        if (n > 3 && strncmp(buf, "lib", 3) == 0)
            memmove(buf, buf + 3, n - 3 + 1);
    }
    if (n == 0)
        return false;
    buf[n] = '\0';
    if (strlen(buf) >= cap)
        return false;
    snprintf(out, cap, "%s", buf);
    return true;
}

static bool library_declares(const ManifestLibrary *lib, const char *stem) {
    for (uint32_t i = 0; i < (*lib).sectionCount; i++)
        if (strcmp((*lib).sections[i], stem) == 0)
            return true;
    return false;
}

static ManifestLibrary *catalog_lookup(const char *name) {
    for (uint32_t i = 0; i < g_libraryCount; i++)
        if (strcmp(g_libraries[i].name, name) == 0)
            return &g_libraries[i];
    return nullptr;
}

static ManifestLibrary *catalog_ensure(const char *name) {
    ManifestLibrary *lib = catalog_lookup(name);
    if (lib)
        return lib;
    if (g_libraryCount >= HOT_MANIFEST_MAX_LIBRARIES)
        return nullptr;
    ManifestLibrary *added = &g_libraries[g_libraryCount];
    snprintf((*added).name, sizeof((*added).name), "%s", name);
    (*added).sectionCount = 0;
    g_libraryCount++;
    return added;
}

// --- minimal bounded JSON reader (self-contained, fail-closed) --------------

static bool json_ws(JsonIn *self) {
    while ((*self).p < (*self).end) {
        char c = *(*self).p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            (*self).p++;
        else
            break;
    }
    return (*self).p < (*self).end;
}

static bool json_eat(JsonIn *self, char expect) {
    if (!json_ws(self))
        return false;
    if (*(*self).p == expect) {
        (*self).p++;
        return true;
    }
    return false;
}

static bool json_peek(JsonIn *self, char *c) {
    if (!json_ws(self))
        return false;
    *c = *(*self).p;
    return true;
}

static bool json_string(JsonIn *self, char *out, size_t cap) {
    if (!json_ws(self) || *(*self).p != '"')
        return false;
    (*self).p++;
    size_t n = 0;
    while ((*self).p < (*self).end && *(*self).p != '"') {
        char c = *(*self).p;
        if (c == '\\') {
            (*self).p++;
            if ((*self).p >= (*self).end)
                return false;
            c = *(*self).p;
        }
        if (n + 1 >= cap)
            return false;
        out[n++] = c;
        (*self).p++;
    }
    if ((*self).p >= (*self).end)
        return false;
    (*self).p++;
    out[n] = '\0';
    return true;
}

static bool json_skip_string(JsonIn *self) {
    if (!json_ws(self) || *(*self).p != '"')
        return false;
    (*self).p++;
    while ((*self).p < (*self).end) {
        char c = *(*self).p;
        if (c == '\\') {
            (*self).p += 2;
            continue;
        }
        if (c == '"') {
            (*self).p++;
            return true;
        }
        (*self).p++;
    }
    return false;
}

static bool json_skip(JsonIn *self, int depth) {
    char c;
    if (!json_peek(self, &c))
        return false;
    if (c == '"')
        return json_skip_string(self);
    if (c == '{') {
        if (depth > JSON_DEPTH_MAX)
            return false;
        (*self).p++;
        if (json_eat(self, '}'))
            return true;
        do {
            if (!json_skip_string(self))
                return false;
            if (!json_eat(self, ':'))
                return false;
            if (!json_skip(self, depth + 1))
                return false;
        } while (json_eat(self, ','));
        return json_eat(self, '}');
    }
    if (c == '[') {
        if (depth > JSON_DEPTH_MAX)
            return false;
        (*self).p++;
        if (json_eat(self, ']'))
            return true;
        do {
            if (!json_skip(self, depth + 1))
                return false;
        } while (json_eat(self, ','));
        return json_eat(self, ']');
    }
    while ((*self).p < (*self).end) {
        char ch = *(*self).p;
        if (ch == ',' || ch == ']' || ch == '}')
            return true;
        (*self).p++;
    }
    return false;
}

static bool catalog_parse_libraries(JsonIn *self) {
    if (!json_eat(self, '{'))
        return false;
    char c;
    if (json_peek(self, &c) && c == '}') {
        (*self).p++;
        return true;
    }
    while (true) {
        char name[HOT_MANIFEST_MAX_NAME];
        if (!json_string(self, name, sizeof(name)))
            return false;
        if (!json_eat(self, ':'))
            return false;
        if (valid_name(name)) {
            ManifestLibrary *lib = catalog_ensure(name);
            if (lib == nullptr)
                return false;
            (*lib).sectionCount = 0;
            if (json_peek(self, &c) && c == '[') {
                (*self).p++;
                if (!(json_peek(self, &c) && c == ']')) {
                    do {
                        char sect[HOT_MANIFEST_MAX_NAME];
                        if (!json_string(self, sect, sizeof(sect)))
                            return false;
                        if ((*lib).sectionCount >= HOT_MANIFEST_MAX_SECTIONS)
                            return false;
                        snprintf((*lib).sections[(*lib).sectionCount],
                                 HOT_MANIFEST_MAX_NAME, "%s", sect);
                        (*lib).sectionCount++;
                    } while (json_eat(self, ','));
                    if (!json_eat(self, ']'))
                        return false;
                } else {
                    (*self).p++;
                }
            } else if (!json_skip(self, 0)) {
                return false;
            }
        } else if (!json_skip(self, 0)) {
            return false;
        }
        if (json_eat(self, '}'))
            return true;
        if (!json_eat(self, ','))
            return false;
    }
}

static bool catalog_apply(JsonIn *self) {
    if (!json_eat(self, '{'))
        return false;
    char c;
    while (true) {
        if (json_peek(self, &c) && c == '}') {
            (*self).p++;
            return true;
        }
        char key[HOT_MANIFEST_MAX_NAME];
        memset(key, 0, sizeof(key));
        if (!json_string(self, key, sizeof(key)))
            return false;
        if (!json_eat(self, ':'))
            return false;
        if (strcmp(key, "name") == 0 || strcmp(key, "version") == 0 ||
            strcmp(key, "org") == 0) {
            char val[HOT_MANIFEST_MAX_NAME];
            if (!json_string(self, val, sizeof(val)))
                return false;
            if (strcmp(key, "name") == 0)
                snprintf(g_name, sizeof(g_name), "%s", val);
            else if (strcmp(key, "version") == 0)
                snprintf(g_version, sizeof(g_version), "%s", val);
            else
                snprintf(g_org, sizeof(g_org), "%s", val);
        } else if (strcmp(key, "libraries") == 0) {
            if (!catalog_parse_libraries(self))
                return false;
        } else if (!json_skip(self, 0)) {
            return false;
        }
        if (json_eat(self, '}'))
            return true;
        if (!json_eat(self, ','))
            return false;
    }
}

// --- catalog persistence -------------------------------------------------------

static bool catalog_write_json(void) {
    char path[MANIFEST_BUF_CAP];
    if (!ManifestPath_manifestJson(path, sizeof(path)))
        return false;
    char json[MANIFEST_JSON_CAP];
    size_t used = 0;
    int n;

    n = snprintf(json, sizeof(json),
                 "{\n"
                 "  \"name\": \"%s\",\n"
                 "  \"version\": \"%s\",\n"
                 "  \"org\": \"%s\",\n"
                 "  \"libraries\": {\n",
                 g_name, g_version, g_org);
    if (n < 0 || (size_t) n >= sizeof(json))
        return false;
    used = (size_t) n;

    for (uint32_t i = 0; i < g_libraryCount; i++) {
        ManifestLibrary *lib = &g_libraries[i];
        n = snprintf(json + used, sizeof(json) - used,
                     "    \"%s\": [", (*lib).name);
        if (n < 0 || (size_t) n >= sizeof(json) - used)
            return false;
        used += (size_t) n;
        for (uint32_t k = 0; k < (*lib).sectionCount; k++) {
            const char *sep = k == 0 ? "\"" : ", \"";
            n = snprintf(json + used, sizeof(json) - used,
                         "%s%s\"", sep, (*lib).sections[k]);
            if (n < 0 || (size_t) n >= sizeof(json) - used)
                return false;
            used += (size_t) n;
        }
        n = snprintf(json + used, sizeof(json) - used, "]%s", i + 1 < g_libraryCount ? ",\n" : "\n");
        if (n < 0 || (size_t) n >= sizeof(json) - used)
            return false;
        used += (size_t) n;
    }
    n = snprintf(json + used, sizeof(json) - used, "  }\n}\n");
    if (n < 0 || (size_t) n >= sizeof(json) - used)
        return false;
    used += (size_t) n;

    char tmp[MANIFEST_BUF_CAP];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return false;
    size_t wr = fwrite(json, 1, used, f);
    bool ok = wr == used;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        UNLINK(tmp);
        return false;
    }
    return RENAME(tmp, path);
}

static bool catalog_save(void) {
    if (!g_mounted)
        return false;
    return catalog_write_json();
}

static bool catalog_seed(void) {
    if (!g_mounted)
        return false;
    char path[MANIFEST_BUF_CAP];
    if (!ManifestPath_manifestJson(path, sizeof(path)))
        return false;
    if (file_exists(path))
        return true;
    return catalog_save();
}

static bool catalog_load(void) {
    if (!g_mounted)
        return false;
    char path[MANIFEST_BUF_CAP];
    if (!ManifestPath_manifestJson(path, sizeof(path)))
        return false;
    if (!file_exists(path))
        return catalog_seed();
    char json[MANIFEST_JSON_CAP];
    size_t len = 0;
    if (!read_file(path, json, sizeof(json), &len))
        return false;
    g_libraryCount = 0;
    JsonIn in;
    in.p = json;
    in.end = json + len;
    return catalog_apply(&in);
}

static bool stage_has_content(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *ent;
    bool has = false;
    while ((ent = readdir(d)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        has = true;
        break;
    }
    closedir(d);
    return has;
}

// --- constructors ------------------------------------------------------------

/** Initializes a caller-buffer path builder with zero current length. */
ManifestPath ManifestPath_0(char *dest, size_t cap) {
    ManifestPath self;
    self.buf = dest;
    self.cap = cap;
    self.len = 0;
    if (dest && cap > 0)
        (*dest) = '\0';
    return self;
}

/** Mounts the install tree from a NULL-terminated segment list and loads its catalog. */
bool Manifest_init(const char *first, ...) {
    if (g_mounted || first == nullptr)
        return false;

    char segments[MANIFEST_MAX_SEGMENTS][HOT_MANIFEST_MAX_NAME];
    uint32_t segCount = 0;

    uintptr_t val = (uintptr_t) first;
    if (val == 0)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "/");
    else if (val == 1)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "~");
    else if (val == 2)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "appdata");
    else
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", first);

    va_list ap;
    va_start(ap, first);
    const char *arg = nullptr;
    while ((arg = va_arg(ap, const char *)) != nullptr) {
        if (segCount >= MANIFEST_MAX_SEGMENTS) {
            va_end(ap);
            return false;
        }
        if (!valid_name(arg)) {
            va_end(ap);
            return false;
        }
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", arg);
    }
    va_end(ap);

    if (segCount < 2)
        return false;

    // Resolve the path WITHOUT creating it, so the guard below can inspect the
    // real pre-existing state of the target directory.
    ManifestPath path = ManifestPath_0(g_root, sizeof(g_root));
    if (!ManifestPath_begin(&path, segments[0]))
        return false;

    for (uint32_t i = 1; i < segCount; i++) {
        if (!ManifestPath_push(&path, segments[i], false))
            return false;
    }

    snprintf(g_name, sizeof(g_name), "%s", segments[segCount - 1]);
    if (segCount >= 3)
        snprintf(g_org, sizeof(g_org), "%s", segments[segCount - 2]);
    else
        snprintf(g_org, sizeof(g_org), "%s", MANIFEST_ORG);
    snprintf(g_version, sizeof(g_version), "%s", MANIFEST_VERSION_DEFAULT);

    g_segmentCount = segCount;
    for (uint32_t i = 0; i < segCount; i++)
        snprintf(g_segments[i], sizeof(g_segments[i]), "%s", segments[i]);

    // Shared destructive-target guard (the SAME rule as UNINSTALL): never own a
    // protected location, and never adopt a pre-existing directory that is not
    // already a manifest tree. Checked BEFORE any directory is created, so the
    // manifest can never own ~/Documents, /System, or another valuable dir.
    if (!manifest_root_guard(g_root)) {
        fprintf(stderr, "hot: MANIFEST refused — '%s' is a protected or non-manifest location\n", g_root);
        g_mounted = false;
        g_segmentCount = 0;
        g_root[0] = '\0';
        return false;
    }

    // Guard passed — create the tree now.
    path = ManifestPath_0(g_root, sizeof(g_root));
    if (!ManifestPath_begin(&path, segments[0]))
        return false;
    for (uint32_t i = 1; i < segCount; i++) {
        if (!ManifestPath_push(&path, segments[i], true))
            return false;
    }

    g_mounted = true;
    g_libraryCount = 0;
    if (!catalog_seed()) {
        g_mounted = false;
        g_segmentCount = 0;
        return false;
    }
    if (!catalog_load()) {
        g_mounted = false;
        g_segmentCount = 0;
        return false;
    }
    return true;
}

/** Removes the mounted install tree only when its segments match the active mount. */
bool Manifest_uninstall(const char *first, ...) {
    if (!g_mounted || g_segmentCount == 0 || first == nullptr) {
        fprintf(stderr, "hot: UNINSTALL refused — no manifest currently mounted\n");
        return false;
    }

    char segments[MANIFEST_MAX_SEGMENTS][HOT_MANIFEST_MAX_NAME];
    uint32_t segCount = 0;

    uintptr_t val = (uintptr_t) first;
    if (val == 0)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "/");
    else if (val == 1)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "~");
    else if (val == 2)
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", "appdata");
    else
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", first);

    va_list ap;
    va_start(ap, first);
    const char *arg = nullptr;
    while ((arg = va_arg(ap, const char *)) != nullptr) {
        if (segCount >= MANIFEST_MAX_SEGMENTS) {
            va_end(ap);
            fprintf(stderr, "hot: UNINSTALL refused — segment count exceeds maximum\n");
            return false;
        }
        snprintf(segments[segCount++], HOT_MANIFEST_MAX_NAME, "%s", arg);
    }
    va_end(ap);

    if (segCount != g_segmentCount) {
        fprintf(stderr, "hot: UNINSTALL refused — segment count %u does not match manifest (%u)\n",
                segCount, g_segmentCount);
        return false;
    }

    for (uint32_t i = 0; i < segCount; i++) {
        if (strcmp(segments[i], g_segments[i]) != 0) {
            fprintf(stderr, "hot: UNINSTALL refused — segment '%s' does not match manifest '%s'\n",
                    segments[i], g_segments[i]);
            return false;
        }
    }

    // Shared destructive-target guard: UNINSTALL refuses exactly what MANIFEST()
    // would refuse to create (a protected location, or a directory that is no
    // longer a manifest tree). The two verbs share one rule.
    if (!manifest_root_guard(g_root)) {
        fprintf(stderr, "hot: UNINSTALL refused — '%s' is a protected or non-manifest location\n", g_root);
        return false;
    }

    remove_ladder_dir(g_root);

    // The machine remembers: the tree is gone but the record stays, so a later
    // run is not a fresh install.
    Ledger *ledger = Ledger_open(g_org, g_name);
    if (ledger != nullptr) {
        Ledger_markUninstalled(ledger);
        Ledger_free(ledger);
    }

    g_mounted = false;
    g_root[0] = '\0';
    g_name[0] = '\0';
    g_org[0] = '\0';
    g_version[0] = '\0';
    g_libraryCount = 0;
    g_segmentCount = 0;

    return true;
}

;;INTENTION("MANIFEST must be called before MANIFEST_LIBRARY — the root locks the mount ladder once so every registered key lands in one install tree")
/** Declares one or more library keys and creates their ladder directories. */
bool MANIFEST_LIBRARY(const char *first, ...) {
    if (!g_mounted || first == nullptr)
        return false;
    char path[MANIFEST_BUF_CAP];
    bool ok = true;
    const char *name = first;
    va_list ap;
    va_start(ap, first);
    while (name != nullptr) {
        if (!valid_name(name)) {
            ok = false;
            break;
        }
        if (catalog_lookup(name) == nullptr) {
            if (catalog_ensure(name) == nullptr) {
                ok = false;
                break;
            }
        }
        for (int slot = MANIFEST_LADDER_BACKWARD; slot <= MANIFEST_LADDER_NEW; slot++) {
            if (!ManifestPath_libraryDir((ManifestLadder) slot, name, path,
                                         sizeof(path), true)) {
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
        name = va_arg(ap, const char*);
    }
    va_end(ap);
    if (!ok)
        return false;
    return catalog_save();
}

// --- core functions ----------------------------------------------------------

/** Returns the mounted install root, or nullptr when no manifest is mounted. */
const char *MANIFEST_ROOT(void) {
    return g_mounted ? g_root : nullptr;
}

/** Ensures all ladder, library, and cache directories exist for the mount. */
bool MANIFEST_ENSURE(void) {
    if (!g_mounted)
        return false;
    char path[MANIFEST_BUF_CAP];
    static const char *slots[] = SLOT_NAMES;
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (!ManifestPath_ladderDir((ManifestLadder) i, path, sizeof(path), true))
            return false;
        for (uint32_t j = 0; j < g_libraryCount; j++)
            if (!ManifestPath_libraryDir((ManifestLadder) i,
                                         g_libraries[j].name, path,
                                         sizeof(path), true))
                return false;
    }
    if (!ManifestPath_cacheDir(path, sizeof(path)))
        return false;
    return dir_mkdir(path);
}

/** Seeds a library catalog and its current payload from the supplied source directory. */
bool MANIFEST_REFLECT(const char *library, const char *sourceDir) {
    if (!g_mounted || !valid_name(library) || sourceDir == nullptr)
        return false;
    if (!dir_exists(sourceDir))
        return false;

    char current[MANIFEST_BUF_CAP];
    char markDir[MANIFEST_BUF_CAP];
    char libMark[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, current,
                                sizeof(current), false))
        return false;
    snprintf(markDir, sizeof(markDir), "%s/%s", current, MANIFEST_MARK);
    snprintf(libMark, sizeof(libMark), "%s/%s", markDir, library);
    if (file_exists(libMark))
        return true; // idempotent — this library already reflected

    ManifestLibrary *lib = catalog_ensure(library);
    if (lib == nullptr)
        return false;

    // Seed sections from the bundled payload's top-level stems.
    (*lib).sectionCount = 0;
    DIR *dir = opendir(sourceDir);
    if (!dir)
        return false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char stem[HOT_MANIFEST_MAX_NAME];
        if (!stem_of(name, stem, sizeof(stem)))
            continue;
        if ((*lib).sectionCount >= HOT_MANIFEST_MAX_SECTIONS) {
            closedir(dir);
            return false;
        }
        if (!library_declares(lib, stem)) {
            snprintf((*lib).sections[(*lib).sectionCount],
                     HOT_MANIFEST_MAX_NAME, "%s", stem);
            (*lib).sectionCount++;
        }
    }
    closedir(dir);
    if (!catalog_save())
        return false;

    char currentLib[MANIFEST_BUF_CAP];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, library, currentLib,
                                 sizeof(currentLib), false))
        return false;
    if (!mkdir_p(currentLib))
        return false;
    if (!copy_tree(sourceDir, currentLib, 0))
        return false;

    // Seed the generation stamp (1) — the live re-loader's swap trigger.
    char genFile[MANIFEST_BUF_CAP];
    if (!ManifestPath_generationFile(library, genFile, sizeof(genFile)))
        return false;
    if (generation_read(genFile) == 0 && !generation_write(genFile, 1))
        return false;

    if (!dir_mkdir(markDir))
        return false;
    FILE *f = fopen(libMark, "wb");
    if (!f)
        return false;
    fputs("vexgraph install reflection\n", f);
    fclose(f);

    // The install moment: record it in the machine-scoped ledger (out of tree,
    // survives UNINSTALL).
    Ledger *ledger = Ledger_open(g_org, g_name);
    if (ledger != nullptr) {
        Ledger_recordInstall(ledger, g_version);
        Ledger_free(ledger);
    }
    return true;
}

/** Replaces the staged payload after validating its top-level sections against the catalog. */
bool MANIFEST_UPDATE(const char *library, const char *payloadDir) {
    if (!g_mounted || !valid_name(library) || payloadDir == nullptr)
        return false;
    ManifestLibrary *lib = catalog_lookup(library);
    if (lib == nullptr || (*lib).sectionCount == 0)
        return false; // undeclared library / no declared sections → closed

    // Fail-closed: every top-level payload entry must be a DECLARED section.
    DIR *dir = opendir(payloadDir);
    if (!dir)
        return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        const char *name = (*ent).d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char stem[HOT_MANIFEST_MAX_NAME];
        if (!stem_of(name, stem, sizeof(stem))) {
            ok = false;
            break;
        }
        if (!library_declares(lib, stem)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    if (!ok)
        return false;

    // Stage: replace any prior staged set in bin/new/<library>; never promotes.
    char staged[MANIFEST_BUF_CAP];
    if (!ManifestPath_libraryDir(MANIFEST_LADDER_NEW, library, staged,
                                 sizeof(staged), false))
        return false;
    remove_ladder_dir(staged);
    if (!mkdir_p(staged))
        return false;
    return copy_tree(payloadDir, staged, 0);
}

bool MANIFEST_ALLOWS(const char *library, const char *filename) {
    if (!g_mounted || !valid_name(library) || !valid_name(filename))
        return false;
    ManifestLibrary *lib = catalog_lookup(library);
    char stem[HOT_MANIFEST_MAX_NAME];
    return lib != nullptr && stem_of(filename, stem, sizeof(stem)) && library_declares(lib, stem);
}

bool MANIFEST_PROMOTE_LIBRARY(const char *library) {
    // Complete-set validation belongs to cold code admission before promotion.
    char next[MANIFEST_BUF_CAP], current[MANIFEST_BUF_CAP], previous[MANIFEST_BUF_CAP];
    char backup[MANIFEST_BUF_CAP], stamp[MANIFEST_BUF_CAP];
    if (g_recoveryRequired || !g_mounted || !valid_name(library) || catalog_lookup(library) == nullptr ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_NEW, library, next, sizeof(next), false) ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, library, current, sizeof(current), false) ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_PREVIOUS, library, previous, sizeof(previous), false) ||
        !ManifestPath_generationFile(library, stamp, sizeof(stamp)))
        return false;
    int n = snprintf(backup, sizeof(backup), "%s.cold-backup", previous);
    if (n < 0 || (size_t) n >= sizeof(backup) || dir_exists(backup) || !stage_has_content(next))
        return false;
    uint64_t generation = generation_read(stamp);
    if (generation == UINT64_MAX)
        return false;
    bool hadPrevious = dir_exists(previous);
    bool movedPrevious = false, movedCurrent = false, movedNext = false;
    if (hadPrevious) {
        if (!RENAME(previous, backup))
            goto reject;
        movedPrevious = true;
    }
    if (!RENAME(current, previous))
        goto reject;
    movedCurrent = true;
    if (!RENAME(next, current))
        goto reject;
    movedNext = true;
    if (!generation_write(stamp, generation + 1))
        goto reject;
    remove_ladder_dir(backup);
    return true;
reject:
    // Caller has stopped all consumers; never silently proceed after undo fails.
    bool undo = true;
    if (movedNext && !RENAME(current, next))
        undo = false;
    if (movedCurrent && !RENAME(previous, current))
        undo = false;
    if (movedPrevious && !RENAME(backup, previous))
        undo = false;
    g_recoveryRequired = !undo;
    THROW("manifest cold promotion failed; rollback %s", undo ? "resolved" : "FAILED; host recovery required");
    return false;
}

bool MANIFEST_REQUIRES_RECOVERY(void) {
    return g_recoveryRequired;
}

uint32_t MANIFEST_SECTION_COUNT(const char *library) {
    if (!g_mounted || !valid_name(library))
        return 0;
    ManifestLibrary *lib = catalog_lookup(library);
    return lib == nullptr ? 0 : (*lib).sectionCount;
}

bool MANIFEST_REVERT_LIBRARY(const char *library, uint64_t generation) {
    char next[MANIFEST_BUF_CAP], current[MANIFEST_BUF_CAP], previous[MANIFEST_BUF_CAP], stamp[MANIFEST_BUF_CAP];
    if (!g_mounted || !valid_name(library) || catalog_lookup(library) == nullptr ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_NEW, library, next, sizeof(next), false) ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, library, current, sizeof(current), false) ||
        !ManifestPath_libraryDir(MANIFEST_LADDER_PREVIOUS, library, previous, sizeof(previous), false) ||
        !ManifestPath_generationFile(library, stamp, sizeof(stamp)) || dir_exists(next))
        return false;
    if (!RENAME(current, next))
        return false;
    if (!RENAME(previous, current)) {
        if (!RENAME(next, current))
            g_recoveryRequired = true;
        return false;
    }
    if (!generation_write(stamp, generation)) {
        g_recoveryRequired = true;
        THROW("manifest revert: generation write failed; host recovery required");
        return false;
    }
    remove_ladder_dir(next);
    return true;
}

/** Slides populated staged library directories through the ladder and advances generations. */
bool MANIFEST_PROMOTE(void) {
    if (!g_mounted)
        return false;
    char newDir[MANIFEST_BUF_CAP];
    char backDir[MANIFEST_BUF_CAP];
    char prevDir[MANIFEST_BUF_CAP];
    char curDir[MANIFEST_BUF_CAP];

    for (uint32_t i = 0; i < g_libraryCount; i++) {
        const char *name = g_libraries[i].name;
        if (!ManifestPath_libraryDir(MANIFEST_LADDER_NEW, name, newDir,
                                     sizeof(newDir), false))
            return false;
        if (!dir_exists(newDir) || !stage_has_content(newDir))
            continue; // nothing staged for this library → stays pinned

        if (!ManifestPath_libraryDir(MANIFEST_LADDER_BACKWARD, name, backDir,
                                     sizeof(backDir), false) ||
            !ManifestPath_libraryDir(MANIFEST_LADDER_PREVIOUS, name, prevDir,
                                     sizeof(prevDir), false) ||
            !ManifestPath_libraryDir(MANIFEST_LADDER_CURRENT, name, curDir,
                                     sizeof(curDir), false))
            return false;

        remove_ladder_dir(backDir);
        if (dir_exists(prevDir) && !RENAME(prevDir, backDir))
            return false;
        if (dir_exists(curDir) && !RENAME(curDir, prevDir))
            return false;
        if (!RENAME(newDir, curDir))
            return false;

        // The rename slide IS the swap; bump the generation stamp so the live
        // re-loader (hot/hot.c Hot_poll) sees the move and reloads current/.
        char genFile[MANIFEST_BUF_CAP];
        if (!ManifestPath_generationFile(name, genFile, sizeof(genFile)))
            return false;
        if (!generation_write(genFile, generation_read(genFile) + 1))
            return false;
    }
    return true;
}

/** Determines first-run status using the persistent ledger and install marker. */
bool MANIFEST_IS_FIRST_RUN(void) {
    if (!g_mounted)
        return true; // fail-closed: no mounted manifest means never installed

    // The machine-scoped ledger outlives the tree: if this app was ever
    // installed here, a wiped tree is NOT a fresh install.
    Ledger *ledger = Ledger_open(g_org, g_name);
    if (ledger != nullptr) {
        bool known = Ledger_hasRecord(ledger);
        Ledger_free(ledger);
        if (known)
            return false;
    }

    char current[MANIFEST_BUF_CAP];
    char markDir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, current,
                                sizeof(current), false))
        return true;
    snprintf(markDir, sizeof(markDir), "%s/%s", current, MANIFEST_MARK);
    return !dir_exists(markDir);
}

/** Returns a mounted library's generation stamp, or zero when unavailable. */
uint64_t MANIFEST_GENERATION(const char *library) {
    if (!g_mounted || !valid_name(library))
        return 0;
    char genFile[MANIFEST_BUF_CAP];
    if (!ManifestPath_generationFile(library, genFile, sizeof(genFile)))
        return 0;
    return generation_read(genFile);
}

// --- path builders -----------------------------------------------------------

/** Sets the builder to a resolved root path without creating the root. */
bool ManifestPath_begin(ManifestPath *self, const char *root) {
    if (self == nullptr || (*self).buf == nullptr || (*self).cap == 0)
        return false;
    char base[MANIFEST_BUF_CAP];
    if (!resolve_base(base, sizeof(base), root))
        return false;
    snprintf((*self).buf, (*self).cap, "%s", base);
    (*self).len = strlen((*self).buf);
    return true;
}

/** Appends one path segment and optionally creates the resulting directory. */
bool ManifestPath_push(ManifestPath *self, const char *segment, bool create) {
    if (self == nullptr || segment == nullptr || (*self).buf == nullptr)
        return false;
    size_t seg_len = strlen(segment);
    size_t need = (*self).len + 1 + seg_len + 1;
    if (need > (*self).cap)
        return false;
    (*self).buf[(*self).len] = '/';
    memcpy((*self).buf + (*self).len + 1, segment, seg_len);
    (*self).len += 1 + seg_len;
    (*self).buf[(*self).len] = '\0';
    if (create && !mkdir_p((*self).buf))
        return false;
    return true;
}

/** Builds the selected mounted ladder directory into the caller's buffer. */
bool ManifestPath_ladderDir(ManifestLadder slot, char *dest, size_t cap, bool create) {
    if (dest == nullptr || cap == 0)
        return false;
    static const char *slot_names[] = SLOT_NAMES;
    if (slot < MANIFEST_LADDER_BACKWARD || slot > MANIFEST_LADDER_NEW)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + 3 + 1 + strlen(slot_names[slot]) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    if (!ManifestPath_push(&p, MANIFEST_BIN, create))
        return false;
    return ManifestPath_push(&p, slot_names[slot], create);
}

/** Builds a library directory under the selected ladder slot. */
bool ManifestPath_libraryDir(ManifestLadder slot, const char *library, char *dest, size_t cap, bool create) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    if (!valid_name(library))
        return false;
    char slotDir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(slot, slotDir, sizeof(slotDir), create))
        return false;
    size_t slot_len = strlen(slotDir);
    if (slot_len + 1 + strlen(library) + 1 > cap)
        return false;
    memcpy(dest, slotDir, slot_len);
    dest[slot_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = slot_len;
    return ManifestPath_push(&p, library, create);
}

/** Builds the generation-stamp file path for a mounted library. */
bool ManifestPath_generationFile(const char *library, char *dest, size_t cap) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    if (!valid_name(library))
        return false;
    // Sibling of the current/<lib> dir (bin/current/<lib>.generation): sits
    // OUTSIDE the rotated subfolders so the ladder rename slide never sweeps it.
    char slotDir[MANIFEST_BUF_CAP];
    if (!ManifestPath_ladderDir(MANIFEST_LADDER_CURRENT, slotDir, sizeof(slotDir), false))
        return false;
    size_t n = strlen(slotDir) + 1 + strlen(library) + strlen(MANIFEST_GENERATION_EXT) + 1;
    if (n > cap)
        return false;
    snprintf(dest, cap, "%s/%s%s", slotDir, library, MANIFEST_GENERATION_EXT);
    return true;
}

/** Builds the mounted install cache directory path. */
bool ManifestPath_cacheDir(char *dest, size_t cap) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + strlen(MANIFEST_CACHE) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    return ManifestPath_push(&p, MANIFEST_CACHE, false);
}

/** Builds the mounted install manifest JSON path. */
bool ManifestPath_manifestJson(char *dest, size_t cap) {
    if (dest == nullptr || cap == 0)
        return false;
    if (!g_mounted)
        return false;
    size_t root_len = strlen(g_root);
    if (root_len + 1 + strlen(MANIFEST_JSON) + 1 > cap)
        return false;
    memcpy(dest, g_root, root_len);
    dest[root_len] = '\0';
    ManifestPath p;
    p.buf = dest;
    p.cap = cap;
    p.len = root_len;
    return ManifestPath_push(&p, MANIFEST_JSON, false);
}

// --- getters -----------------------------------------------------------------

/** Returns the builder's destination path, or nullptr when unavailable. */
const char *ManifestPath_get(const ManifestPath *self) {
    return self == nullptr || (*self).buf == nullptr ? nullptr : (*self).buf;
}

/** Returns the builder's current path length, or zero for null self. */
size_t ManifestPath_len(const ManifestPath *self) {
    return self == nullptr ? 0 : (*self).len;
}
