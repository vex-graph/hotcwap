#ifndef HOT_LEDGER_H
#define HOT_LEDGER_H

#include <stdbool.h>
#include <stddef.h>

// hot/ledger.h — the machine-scoped install ledger (R1 host).
//
// Records, OUTSIDE the install tree, that an application (org/app) has been
// installed on this machine — and survives UNINSTALL. Backed by a plain
// per-user record file outside the tree (Application Support, LocalAppData, or
// XDG_STATE_HOME / ~/.local/state, depending on platform).
//
// This is a CORRECTNESS record, not a secure store or security boundary: a local user with the
// machine can always clear the store. Its promise is "the machine remembers
// this app was here", not "the machine cannot forget". The in-tree
// manifest.json stays the ownership marker for safe deletion; this ledger is
// the machine's out-of-tree memory of the install.

typedef struct Ledger Ledger;

typedef enum LedgerState {
    LEDGER_ABSENT = 0,   // no record: never installed on this machine
    LEDGER_INSTALLED,    // record present, tree expected on disk
    LEDGER_UNINSTALLED,  // was installed; tree removed, record kept
} LedgerState;

// Bind to one application identity. Reads any existing record. NULL on bad
// args or allocation failure.
Ledger *Ledger_open(const char *org, const char *app);
void Ledger_free(Ledger *ledger);

// Create or update the record as installed at `version` (the install moment).
bool Ledger_recordInstall(Ledger *ledger, const char *version);

// Flip the record to uninstalled — the tree is gone but the machine remembers.
bool Ledger_markUninstalled(Ledger *ledger);

// Hard purge of the record (an explicit "forget this machine"). Never called
// by UNINSTALL; exists for reset/testing.
bool Ledger_forget(Ledger *ledger);

// Current state (LEDGER_ABSENT when there is no record).
LedgerState Ledger_state(const Ledger *ledger);

// True when any record exists (installed OR uninstalled).
bool Ledger_hasRecord(const Ledger *ledger);

// Last recorded version ("" when none). Dest-last.
bool Ledger_lastVersion(const Ledger *ledger, char *dest, size_t cap);

const char *Ledger_lastError(const Ledger *ledger);

#endif
