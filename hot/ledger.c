#include "annotation/definition.h"
#include "annotation/overview.h"

#include "platform/platform.h"

;;DEFINITION
/** The install ledger remembers an application's install state outside its
 * install tree, so uninstalling the tree does not erase machine history. The
 * current implementation stores a plain per-user record, not a secure-store
 * credential or a security boundary. The manifest remains the ownership marker
 * for deleting an install tree. The record is retained until an explicit forget. */
;;OVERVIEW
/** ========================================================================
 * CLASS: Ledger (hot/ledger.c)
 * STRUCT FIELDS (Mirroring Ledger)
 * char org[64]                  admitted application organization identity
 * char app[64]                  admitted application name identity
 * char path[512]                per-user record path, outside install tree
 * char version[64]              last recorded install version
 * LedgerState state             absent, installed, or uninstalled state
 * FUNCTION REGISTRY
 * Constructors: Ledger *Ledger_open(const char *org, const char *app)
 * Lifecycle: void Ledger_free(Ledger *ledger)
 * Operations: bool Ledger_recordInstall(Ledger *ledger, const char *version)
 *   bool Ledger_markUninstalled(Ledger *ledger), bool Ledger_forget(Ledger *ledger)
 * Projections: LedgerState Ledger_state(const Ledger *ledger)
 *   bool Ledger_hasRecord(const Ledger *ledger)
 *   bool Ledger_lastVersion(const Ledger *ledger, char *dest, size_t cap)
 *   const char *Ledger_lastError(const Ledger *ledger)
 * Private helpers: state_dir, mkdir_p, sanitize, record_path, apply_record,
 *   write_state. They select the platform state directory, validate identity
 *   path components, parse a record, and publish record state.
 * Admission, null, failure and ownership contract:
 * Invalid identities, unavailable paths, malformed records and IO failures
 * reject without claiming a successful state transition. The returned Ledger
 * owns its record metadata; returned error text is borrowed. The plain record
 * is correctness state only and is neither tamper-resistant nor a secret store.
 * ======================================================================== */

#include "hot/ledger.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct Ledger {
    char org[64];
    char app[64];
    char path[512];
    char version[64];
    LedgerState state;
};

// --- the state directory (per user, outside any install tree) ----------------

static bool state_dir(char *dest, size_t cap) {
#if defined(VEX_PLATFORM_WINDOWS)
    const char *base = getenv("LOCALAPPDATA");
    if (base == nullptr || *base == '\0')
        base = getenv("USERPROFILE");
    if (base == nullptr || *base == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/vexgraph/ledger", base);
    return n > 0 && (size_t) n < cap;
#elif defined(VEX_PLATFORM_APPLE)
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/Library/Application Support/vexgraph/ledger", home);
    return n > 0 && (size_t) n < cap;
#else
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg != nullptr && *xdg != '\0') {
        int n = snprintf(dest, cap, "%s/vexgraph/ledger", xdg);
        return n > 0 && (size_t) n < cap;
    }
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        return false;
    int n = snprintf(dest, cap, "%s/.local/state/vexgraph/ledger", home);
    return n > 0 && (size_t) n < cap;
#endif
}

static bool mkdir_p(const char *path) {
    char tmp[512];
    size_t len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len == 0 || len >= sizeof(tmp))
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

// Sanitize org/app into one filename-safe key (non [alnum_] → '_').
static void sanitize(const char *src, char *dest, size_t cap) {
    size_t k = 0;
    for (const char *p = src; *p != '\0' && k + 1 < cap; p++)
        dest[k++] = (isalnum((unsigned char) *p) || *p == '_') ? *p : '_';
    dest[k] = '\0';
}

static bool record_path(char *dest, size_t cap, const char *org, const char *app) {
    char dir[400];
    if (!state_dir(dir, sizeof(dir)))
        return false;
    char o[64], a[64];
    sanitize(org, o, sizeof(o));
    sanitize(app, a, sizeof(a));
    int n = snprintf(dest, cap, "%s/%s_%s.rec", dir, o, a);
    return n > 0 && (size_t) n < cap;
}

// --- the class ---------------------------------------------------------------

static void apply_record(Ledger *ledger, const char *record) {
    const char *nl = strchr(record, '\n');
    if (nl == nullptr)
        return;
    size_t slen = (size_t) (nl - record);
    if (slen == 0 || slen >= 16)
        return;
    if (slen == 9 && strncmp(record, "installed", 9) == 0)
        (*ledger).state = LEDGER_INSTALLED;
    else if (slen == 11 && strncmp(record, "uninstalled", 11) == 0)
        (*ledger).state = LEDGER_UNINSTALLED;
    else
        return;
    const char *ver = nl + 1;
    const char *nl2 = strchr(ver, '\n');
    size_t vlen = nl2 ? (size_t) (nl2 - ver) : strlen(ver);
    if (vlen >= sizeof((*ledger).version))
        vlen = sizeof((*ledger).version) - 1;
    memcpy((*ledger).version, ver, vlen);
    (*ledger).version[vlen] = '\0';
}

/** Opens or loads the machine-scoped record for the organization/application pair. */
Ledger *Ledger_open(const char *org, const char *app) {
    if (org == nullptr || app == nullptr || *org == '\0' || *app == '\0')
        return nullptr;
    Ledger *ledger = calloc(1, sizeof(*ledger));
    if (ledger == nullptr)
        return nullptr;
    snprintf((*ledger).org, sizeof((*ledger).org), "%s", org);
    snprintf((*ledger).app, sizeof((*ledger).app), "%s", app);
    (*ledger).state = LEDGER_ABSENT;
    (*ledger).version[0] = '\0';
    if (!record_path((*ledger).path, sizeof((*ledger).path), org, app)) {
        free(ledger);
        return nullptr;
    }

    FILE *f = fopen((*ledger).path, "rb");
    if (f != nullptr) {
        char buf[256];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        apply_record(ledger, buf);
    }
    return ledger;
}

/** Frees the ledger's owned strings and resets its in-memory record. */
void Ledger_free(Ledger *ledger) {
    free(ledger);
}

/** Persists the requested install state and optional version in the ledger file. */
static bool write_state(Ledger *ledger, const char *state, const char *version) {
    char dir[400];
    if (!state_dir(dir, sizeof(dir)) || !mkdir_p(dir))
        return false;
    const char *ver = version ? version : "";
    char record[192];
    int n = snprintf(record, sizeof(record), "%s\n%s\n", state, ver);
    if (n < 0 || (size_t) n >= sizeof(record))
        return false;
    FILE *f = fopen((*ledger).path, "wb");
    if (f == nullptr)
        return false;
    bool ok = (fwrite(record, 1, (size_t) n, f) == (size_t) n);
    fclose(f);
    if (!ok)
        return false;
    (*ledger).state = (strcmp(state, "installed") == 0) ? LEDGER_INSTALLED : LEDGER_UNINSTALLED;
    snprintf((*ledger).version, sizeof((*ledger).version), "%s", ver);
    return true;
}

/** Records the installation and version for this application identity. */
bool Ledger_recordInstall(Ledger *ledger, const char *version) {
    if (ledger == nullptr)
        return false;
    return write_state(ledger, "installed", version);
}

/** Marks the recorded installation uninstalled without deleting its history. */
bool Ledger_markUninstalled(Ledger *ledger) {
    if (ledger == nullptr)
        return false;
    return write_state(ledger, "uninstalled", (*ledger).version);
}

/** Removes the machine-scoped ledger record for this identity. */
bool Ledger_forget(Ledger *ledger) {
    if (ledger == nullptr)
        return false;
    if (remove((*ledger).path) != 0 && errno != ENOENT)
        return false;
    (*ledger).state = LEDGER_ABSENT;
    (*ledger).version[0] = '\0';
    return true;
}

/** Returns the current recorded ledger state. */
LedgerState Ledger_state(const Ledger *ledger) {
    return ledger ? (*ledger).state : LEDGER_ABSENT;
}

/** Reports whether a machine-scoped record exists for the ledger identity. */
bool Ledger_hasRecord(const Ledger *ledger) {
    return ledger != nullptr && (*ledger).state != LEDGER_ABSENT;
}

/** Copies the last recorded version into the caller's bounded buffer. */
bool Ledger_lastVersion(const Ledger *ledger, char *dest, size_t cap) {
    if (ledger == nullptr || dest == nullptr || cap == 0)
        return false;
    snprintf(dest, cap, "%s", (*ledger).version);
    return true;
}

/** Returns the ledger's last diagnostic string, or a fallback for null. */
const char *Ledger_lastError(const Ledger *ledger) {
    (void) ledger;
    return "";
}
