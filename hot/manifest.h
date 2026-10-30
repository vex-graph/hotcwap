#ifndef HOT_MANIFEST_H
#define HOT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// hot/manifest.h — the MANIFEST(...) install-layout + manifest.json catalog authority.
//
// The "manifest binary way": the manifest IS the on-disk install tree PLUS
// the manifest.json library catalog. MANIFEST(...) resolves the per-app
// install root from the platform application-data base and locks it;
// MANIFEST_LIBRARY(...) registers the hosted library keys; the ladder holds
// one generation set PER LIBRARY; manifest.json declares each library's
// allowed section list (dylib stems) that downloads must satisfy.
//
// SCREAMING_CASE surface, hotcwap-style:
//   MANIFEST(MANIFEST_APP_DATA, "vexgraph", "semicolon") // ONE-TIME init
//   MANIFEST_LIBRARY("vexspoke", "graphvex", ..., 0)     // register library keys
//   MANIFEST_UPDATE("graphvex", payloadDir)              // stage bin/new/graphvex/
//   MANIFEST_PROMOTE()                                   // per-library slide
//   MANIFEST_ENSURE()  MANIFEST_REFLECT(...)  MANIFEST_IS_FIRST_RUN()
//
// Per-app layout under APPLICATION_DATA + org + <app>:
//   manifest.json         library catalog {name, version, org, libraries{}}
//   bin/backward/<lib>    oldest retained set (rollback), per library
//   bin/previous/<lib>    prior generation (rollback), per library
//   bin/current/<lib>     the runnable set — what the launcher dlopens
//   bin/current/<lib>.generation  per-library generation stamp (N+1 per promote)
//   bin/new/<lib>         staged future set (downloads land here)
//   cache/                cache system
//
// The <lib>.generation stamp is THE hot-swap trigger: MANIFEST_REFLECT seeds
// it (1) on first install and MANIFEST_PROMOTE bumps it for every promoted
// library. The loader (hot/hot.c) compares its last-seen generation against
// the stamp and reloads bin/current/<lib> when it moves — the rename slide IS
// the swap, so no watch-dir and no clone step exist (the retired MODE-1
// hot/ watch dir is gone; see the SPIR-V Shader Deployment-era install docs
// history for the old design).
//
// manifest.json is the plain JSON catalog any runtime may edit (it is the
// downloader's entry point, not R1's own state). Example:
//
//   {
//     "name": "semicolon",
//     "version": "0.1.0",
//     "org": "vexgraph",
//     "libraries": {
//       "vexspoke": ["io", "memory", "types"],
//       "graphvex": ["buffer", "texture", "spv"]
//     }
//   }
//
// The DOWNLOADER (vexspoke or any runtime) owns the section lists: it edits
// manifest.json to add a section ("video") BEFORE staging a payload that
// carries it, so MANIFEST_UPDATE only ever verifies against declared sections
// (fail-closed). MANIFEST_REFLECT seeds sections on first-run from the
// bundled payload. Dylib naming is automatic from the stem: "io" ships as
// io.dylib / io.dll / libio.so by platform.
//
// Two experiences share this tree: MODE 1 (app RUNNING) swaps dylibs live
// via Hot_poll against the bin/current/<lib>.generation stamp (MANIFEST_PROMOTE
// bumps it; the loader reloads bin/current/<lib> when it moves); MODE 2
// (app CLOSED) promotes new/ → current/ with renames so the next launch is
// the new binary. See docs/install.md for the full process.

// --- Hotloading & catalog constants (consumed by hot/ trampolines and this file) ----

#define HOT_MANIFEST_MAX_NAME 64
#define HOT_MANIFEST_MAX_EXPORTS 128
#define HOT_MANIFEST_MAX_LIBRARIES 32
#define HOT_MANIFEST_MAX_SECTIONS 64

// --- Path roots & base defines ----------------------------------------------

// Per-platform application-install base, overridable at build time
// (custom deployments) or by $VEX_MANIFEST at runtime (test seam).
#include "platform/platform.h"

#if defined(VEX_PLATFORM_WINDOWS)
#  define APPLICATION_PATH "%LOCALAPPDATA%"
#elif defined(VEX_PLATFORM_APPLE)
#  define APPLICATION_PATH "Library/Application Support"
#else
#  define APPLICATION_PATH ".local/share"
#endif

// The vexgraph-ecosystem-gitrepo org folder under the application-data base (default org in
// MANIFEST() calls; resource roots still nest underneath it).
#define MANIFEST_ORG "vexgraph"

// Install fingerprint file dropped in bin/current after first-run reflection.
#define MANIFEST_MARK ".install-mark"

// Standard root aliases for MANIFEST()
#define MANIFEST_MAIN_DISK   "/"
#define MANIFEST_USER_HOME   "~"
#define MANIFEST_APP_DATA    "appdata"
#define APPLICATION_DATA     "appdata"

// Ladder slots, oldest → newest staging. VERBS promote NEW → CURRENT;
// CURRENT → PREVIOUS → BACKWARD keeps the rollback sets. Each slot carries
// one subfolder per registered library.
typedef enum ManifestLadder {
    MANIFEST_LADDER_BACKWARD = 0,
    MANIFEST_LADDER_PREVIOUS,
    MANIFEST_LADDER_CURRENT,
    MANIFEST_LADDER_NEW,
} ManifestLadder;

// The class: a path being built under a manifest root. A thin builder that
// owns a caller buffer pointer + current length, never the memory.
typedef struct ManifestPath {
    char *buf;    // caller-owned destination buffer
    size_t cap;   // capacity of buf
    size_t len;   // current path length, excluding the trailing NUL
} ManifestPath;

// Constructors:
//   ManifestPath(dest, cap)           — bind a builder to a caller buffer.
//   MANIFEST(root, seg1, ...)         — one-shot init + create dirs (below).

ManifestPath ManifestPath_0(char *dest, size_t cap);

// --- CORE FUNCTIONS ----------------------------------------------------------

// Varargs string-based initializer and uninstaller implementations.
bool Manifest_init(const char *first, ...);
bool Manifest_uninstall(const char *first, ...);

// ONE-TIME initializer. Accepts path segments as varargs:
//   MANIFEST(APPLICATION_DATA, "vexgraph", "test suite")
//   MANIFEST("~", "downloads", "whatever", "whateveragain")
#define MANIFEST(...) Manifest_init(__VA_ARGS__ __VA_OPT__(,) (const char*) 0)

// Complete uninstall: wipes the whole installed tree.
// Requires passing the EXACT same varargs path segments as MANIFEST:
//   UNINSTALL(APPLICATION_DATA, "vexgraph", "test suite")
// If any segment differs from the mounted manifest, it fails closed (zero files deleted).
#define UNINSTALL(...) Manifest_uninstall(__VA_ARGS__ __VA_OPT__(,) (const char*) 0)

// Register the hosted library KEYS in manifest.json (creating the file's
// libraries{} on first call). Varargs, must end with (const char*) 0:
//
//   MANIFEST_LIBRARY("vexspoke", "graphvex", (const char*) 0)
//
// Each key is added with an empty section list — the downloader owns the
// section arrays (edits manifest.json to grow them). Creates the per-library
// ladder subfolders in every slot. MUST be called after MANIFEST() (the
// ;;INTENTION in manifest.c). Returns false if never mounted / unregistered.
bool MANIFEST_LIBRARY(const char *first, ...);

// Return the locked root (<application-base>/<org>/<app>). Buffer valid until
// the next ManifestPath call. Returns nullptr when MANIFEST() never ran.
const char *MANIFEST_ROOT(void);

// Create the whole per-app ladder (bin/{backward,previous,current,new} plus
// one subfolder per registered library, cache). Idempotent. Returns
// true when every dir exists.
bool MANIFEST_ENSURE(void);

// First-run reflection of one library: seed its section list in manifest.json
// from the bundled payload at sourceDir, copy the payload into
// bin/current/<library>, seed the <library>.generation stamp (1), and drop
// the install fingerprint. Idempotent via the fingerprint in MANIFEST_MARK;
// returns false on any copy failure.
bool MANIFEST_REFLECT(const char *library, const char *sourceDir);

// Update staging: verify every top-level entry of payloadDir is a DECLARED
// section of <library> in manifest.json (dylib stems match by name, extension
// stripped), then stage the payload into bin/new/<library> (replacing any
// prior staged set). Fail-closed per the Cold-Strict, Hot-Minimal Validation
// Law: an undeclared payload section refuses the WHOLE update. Verify only —
// never promotes. Returns true when a promotion is safe.
bool MANIFEST_UPDATE(const char *library, const char *payloadDir);

// MODE-2 finale: per-library generation slide. For every library with staged
// content in bin/new/<library>, slide (renames, same filesystem, atomic):
//   current → previous → backward   (rollback sets slide, oldest dropped)
//   new     → current
// then bump that library's bin/current/<lib>.generation stamp (N+1) — the
// live re-loader sees the move on its next poll and swaps in-process.
// Libraries with no staged set stay pinned. Returns false if any rename fails
// (nothing is half-applied for the failing library).
bool MANIFEST_PROMOTE(void);

// First-run detection: true when the tree was never installed (no
// bin/current/<MANIFEST_MARK>). The launcher must MANIFEST_REFLECT before run.
bool MANIFEST_IS_FIRST_RUN(void);

// Read one library's current generation stamp — bin/current/<lib>.generation.
// Returns 0 when never mounted, the library key is invalid, or the stamp is
// missing/unparsable (a never-installed library reads 0). The hot re-loader
// (hot/hot.c) compares this against its last-seen generation to trigger a swap.
uint64_t MANIFEST_GENERATION(const char *library);

// Cold loader admission checks the mounted catalog, never trusts placement.
bool MANIFEST_ALLOWS(const char *library, const char *filename);
uint32_t MANIFEST_SECTION_COUNT(const char *library);
// Transactional, one-library promotion. On detected rename/stamp failure, undo
// completed moves; rollback failure is reported as false. Stable trusted tree,
// external serialization; crash/power-loss recovery is not provided by this API.
bool MANIFEST_PROMOTE_LIBRARY(const char *library);
// Restore the immediately previous set after a failed cold code admission.
bool MANIFEST_REVERT_LIBRARY(const char *library, uint64_t generation);
bool MANIFEST_REQUIRES_RECOVERY(void);

// --- PATH BUILDERS (dest-last) ----------------------------------------------

// Push a segment onto a bound builder. When `create` is true the segment's
// directory is created as it is appended ("will make a folder during that
// time"). Returns false on overflow / mkdir failure.
bool ManifestPath_push(ManifestPath *self, const char *segment, bool create);

// Resolve an OS root into a builder (no mkdir — the root always exists).
bool ManifestPath_begin(ManifestPath *self, const char *root);

// Ladder slot dir <locked root>/bin/<slot>. create=true also mkdirs it.
bool ManifestPath_ladderDir(ManifestLadder slot, char *dest, size_t cap, bool create);

// Per-library ladder dir <locked root>/bin/<slot>/<library>. create=true
// also mkdirs it. Returns false when the library name is invalid or the
// manifest never mounted.
bool ManifestPath_libraryDir(ManifestLadder slot, const char *library, char *dest, size_t cap, bool create);

// The per-library generation stamp <locked root>/bin/current/<lib>.generation
// — a plain base-10 integer (0 when never written). The live re-loader
// compares this against its last-seen generation to trigger a hot swap.
bool ManifestPath_generationFile(const char *library, char *dest, size_t cap);

// cache/ dir under the locked root.
bool ManifestPath_cacheDir(char *dest, size_t cap);

// The catalog file <locked root>/manifest.json (created on first save).
bool ManifestPath_manifestJson(char *dest, size_t cap);

// --- GETTERS -----------------------------------------------------------------

// Null-safe accessors (the Symmetric Getter/Setter Completeness Law).
const char *ManifestPath_get(const ManifestPath *self);
size_t ManifestPath_len(const ManifestPath *self);

#endif
