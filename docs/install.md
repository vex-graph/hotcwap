# The `MANIFEST(...)` Install Layout — hotcwap's "Manifest Binary Way"

`hot/manifest.h/.c` is the per-app **install-layout authority**. The manifest
is the on-disk install tree **plus** the `manifest.json` library catalog — the
plain-JSON file any runtime (the downloader) edits. There is no proprietary
policy seed to keep in sync; `MANIFEST.mf` (the old JSON seed) is retired.

This document describes the process: how the path resolves, what the catalog
declares, what the ladder holds, how first-run reflection works, and how the
two update experiences (hot swap while running, cold swap on next launch)
share the same tree.

---

## 1. The three roots

Every install resolves from one OS root via `ManifestPath_begin`:

| Root | macOS | Windows | Linux |
| :--- | :--- | :--- | :--- |
| `MANIFEST_MAIN_DISK` | `/` (Macintosh HD) | `C:\` | `/` |
| `MANIFEST_USER_HOME` | `/Users/<you>` | `C:\Users\<you>` | `~` |
| `MANIFEST_APP_DATA` | `~/Library/Application Support` | `%LOCALAPPDATA%` | `~/.local/share` (or `$XDG_DATA_HOME`) |

The platform-agnostic base is `#define APPLICATION_PATH`, one `_WIN32` /
`__APPLE__` / else branch in `hot/manifest.h`. Building with a custom
deployment? Override the macro at build time. Testing? Any tool that wants
an isolated tree sets **`$VEX_MANIFEST`** to a scratch base — the resolver
honors it first, so a test may mount a ladder in `/tmp` with zero install
risk (this is the seam the manifest smoke tests use).

## 2. `MANIFEST(...)` — one-time init

```c
bool MANIFEST(ManifestRoot kind, const char *org, const char *app);
```

```c
MANIFEST(MANIFEST_APP_DATA, "vexgraph", "semicolon");
// → macOS:   ~/Library/Application Support/vexgraph/semicolon/
// → Windows: %LOCALAPPDATA%\vexgraph\semicolon\
```

- The org segment defaults to `MANIFEST_ORG` = `vexgraph` when `org` is null
  or empty — the ecosystem nests every install under one org folder.
- **`MANIFEST(...)` is one-shot.** A second call returns `false`. The root
  locks for the whole process — two launchers can never mount the same
  ladder. On failure the mount clears and the app may retry; after success
  the root is frozen.
- Seeding: when `<root>/manifest.json` is absent, `MANIFEST(...)` writes the
  catalog `{name, version, org, libraries{}}` (name = app, default version
  `0.1.0`, org = resolved org). An existing catalog is parsed and kept —
  never overwritten.
- `MANIFEST_ROOT()` returns the locked buffer (`nullptr` before the init).

## 3. The catalog (`manifest.json`)

The **downloader owns the section arrays**. `MANIFEST_LIBRARY(...)` registers
the hosted library **keys** with empty lists; the downloader edits
`manifest.json` to grow a key's section list **before** staging any payload
that ships those sections, so `MANIFEST_UPDATE` only ever verifies against
declared sections (fail-closed).

```json
{
  "name": "semicolon",
  "version": "0.1.0",
  "org": "vexgraph",
  "libraries": {
    "vexspoke": ["io", "memory", "types"],
    "graphvex": ["buffer", "texture", "spv"]
  }
}
```

Dylib naming is automatic from the stem: section `"io"` ships as `io.dylib`
(Apple) / `io.dll` (Windows) / `libio.so` (Linux). `manifest.c` carries a
self-contained bounded JSON reader/writer — R1 stays inside its allowlist
(no `net/json` dependency; standalone hotcwap builds stay green).

Registering keys (must run after `MANIFEST(...)` — the `;;INTENTION` in
`manifest.c`):

```c
bool MANIFEST_LIBRARY(const char *first, ...);   // ends (const char*) 0
MANIFEST_LIBRARY("vexspoke", "graphvex", (const char*) 0);
```

Each key gets its per-library ladder subfolders in every slot, created here
(and again by `MANIFEST_ENSURE`). A second registration of the same key is a
no-op.

## 4. The ladder

`MANIFEST_ENSURE()` creates the full tree:

```
<root>/manifest.json        ← catalog (libraries → section stems)
<root>/bin/
│   ├── backward/           ← oldest retained set (rollback keeper)
│   │   └── <library>/      ← ONE generation set PER library
│   ├── previous/           ← prior generation (rollback), per library
│   ├── current/            ← the runnable set — what the launcher dlopens
│   │   ├── <library>/ ...
│   │   └── <library>.generation  ← per-library stamp (N+1 per promote)
│   └── new/                ← staged future set (downloads land here)
└── cache/                  ← cache system
```

Slot paths via `ManifestPath_ladderDir(slot, dest, cap, create)`; the
per-library path is `ManifestPath_libraryDir(slot, library, dest, cap,
create)` (validates the library name against a path-safe charset).
`ManifestPath_generationFile`, `ManifestPath_cacheDir` and
`ManifestPath_manifestJson` resolve the other three. There is no `hot/`
watch dir anymore — the `<library>.generation` stamp is the swap trigger
(the harnessed MODE-1 design from the pre-manifest era is retired).

## 5. First-run reflection (`MANIFEST_IS_FIRST_RUN` / `MANIFEST_REFLECT`)

First time the process opens (no `<current>/.install-mark`), the launcher
reflects each hosted library from its bundled payload:

```c
MANIFEST_REFLECT("vexspoke", "/path/to/bundled/vexspoke");
```

Per library: **seed the section list** in `manifest.json` from the payload's
top-level stems, **copy the payload** into `bin/current/<library>` (per-file
temp + atomic `rename`, `.name.stage` → `name`), and drop the **per-library
fingerprint** under `<current>/.install-mark/<library>`. Idempotent via that
fingerprint; on copy failure it returns `false` and the launcher retries
next launch. `MANIFEST_IS_FIRST_RUN()` flips from `true` to `false` once the
`.install-mark` directory exists.

The `.app` or `.exe` shell stays **frozen** — it is the loadable stub. The
behavior — all dylibs, `.spv` blobs, fonts, config — lives in the manifest
tree under `current/<library>/`. The OS binary never mutates on disk; the
behavior set does.

## 6. Two update experiences (the "hot cwap")

Both modes share the ladder and the same verification contract: every
top-level payload entry must be a **declared section** of the target library
(stem match by name, extension stripped). An undeclared entry refuses the
WHOLE update — never a partial stage. The older `HotStage_verify` (JSON
`hot.manifest` schema + `files.sha` FNV-1a) is retired.

### MODE 1 — HOT SWAP (app running)

Beyond the ladder, MODE 1 is `hot/hot.c`'s dual-poll handshake:
`stamp check → off-thread state save → dlopen + fail-closed verify →
restore-before-commit → trampoline swap → retire ring (4-poll grace)`. Each poll
compares `MANIFEST_GENERATION(<library>)` (reads
`bin/current/<library>.generation`) against the last-seen generation:

1. On a stamp move, the poll kicks a save worker (25ms cond-wait slices, the
   Bounded Wait Law) that snapshots the current images' module state via
   `Hot_save` and returns `loaded==0` — hot loops never pay serialization.
2. The next poll dlopens EVERY section of the new current set and verifies
   the whole library fail-closed (`dlopen` + `Hot_getExports` on each
   before any commit), then **restores** the saved blobs into the STAGED
   images BEFORE any commit — a section whose `Hot_restore` rejects its blob
   (or a snapshotted slot with no `Hot_restore`) rolls the WHOLE swap back
   (#8.5 Automated State Rollback): staged handles close, the old generation
   + saved state stay live, the stamp never advances, and the next poll
   re-attempts once the payload is fixed. Only when every restore passes does
   it atomically swap the trampoline table and retire the old handles into
   the grace ring.

Modules expose exports via the struct contract (`Hot_getExports`); the
retired JSON `Hot_manifest` ABI gate is gone — the ladder placement is the
gate. Zero restart, `current/` stays pinned during the swap.

### MODE 2 — COLD SWAP (app closed)

The downloader drops a staged set into `bin/new/<library>`, then the launcher
verifies and promotes:

```c
bool ok = MANIFEST_UPDATE("vexspoke", payloadDir);
// verify every payload entry is a DECLARED section of "vexspoke", then
// stage (replace) bin/new/vexspoke — verify only, never promotes.

bool ok = MANIFEST_PROMOTE();
// per-library slide for every library with a staged set:
//   current → previous → backward (oldest dropped)
//   new     → current
// Libraries with no staged set stay pinned.
```

- `MANIFEST_UPDATE` stages into `bin/new/<library>`, replacing any prior
  staged set; fail-closed per the Cold-Strict, Hot-Minimal Validation Law.
- `MANIFEST_PROMOTE` is **renames only** — atomic on the same filesystem,
  never a copy, never a partial bind. Each library slides independently;
  a failing library applies nothing.
- With nothing staged it is an idempotent no-op (returns `true`).
- If everything compiles and verifies, the *next launch loads the new set*.
  The `.app` shell you run again is the same stub; the `current/` set it
  reflect-loads is the new binary — "after we exit the application, there
  will now be .exe or .app file that is at the latest version" is exactly
  this: not by rewriting the OS binary (impossible — mmap pins it), but by
  sliding the layout the stub resolves.

## 7. The old policy file

`MANIFEST.mf` (repo root) and `spoke/MANIFEST.md` are retired. The
`consumers[]` allow-list that governed which projects may bind vexspoke now
reads as a **documented baseline** in `spoke/lifetime.h` — the authority for
what may board is the on-disk install ladder plus the `manifest.json` catalog
(vexspoke downloads → `bin/new` → `MANIFEST_UPDATE` → `MANIFEST_PROMOTE`),
never a JSON seed sitting next to the source.

## 8. Cold boundary

This is a cold boundary per the Cold-Strict, Hot-Minimal Validation Law:
`MANIFEST` / `LIBRARY` / `ENSURE` / `REFLECT` / `UPDATE` / `PROMOTE` validate
exhaustively (null, bounds, mount state, declared-section match, mkdir/rename
failures) and return `false` with zero partial state; the hot `Hot_poll` path
stays minimal. No function in this unit blocks unboundedly or allocates
outside stack buffers.
</content>
