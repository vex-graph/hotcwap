# hotcwap — Repo-Local Living Preferences

## Constitution Link

gist.md (`../../../metadata/gists/preferences/gist.md`) is the compressed law list; all universal laws bind this repository. Read one on demand: `./tools/prefs law "<title>"`. Universal laws are inherited, never restated.

Tier: tool (Dynamic Module Reloader / ABI Airlock). Owns: dynamic dylib loading, ABI verification, ABA-safe retirement rings, trampoline dispatch. Borrows: `lifetime` (`lifetime` + `re`) statically linked into host.

## Exclusive Laws

| Law Title | Scope | Enforcement |
| **Two Load Units Law** | Dynamic Airlock | Mandatory for `hotcwap` |
| **Dynamic Module ABI Verification Law** | Dynamic Airlock | Mandatory for `hotcwap` |

### Two Load Units Law

* Load unit 1 — `lifetime` (`lifetime` + `re`) — is statically linked into the host executable and is never a dylib.
* Load unit 2 is one engine image holding `vexspoke` + `graphvex` + `darling-framework`, loaded with `-undefined dynamic_lookup` against the host's exported symbols.
* Load unit 3 is one dylib per `app`, also `-undefined dynamic_lookup`, swapped independently of the engine image.
* All loads are `RTLD_NOW|RTLD_LOCAL`; a path-unique copy per generation defeats the dyld path cache.
* `lifetime` is a link unit, not a load unit; a swap request naming `lifetime` is a hard refusal.
* One engine image: `vexspoke`, `graphvex` and `darling-framework` ship as a single dylib with a single contract version.
* Apps are separate dylibs: each app is its own image, versioned against the engine image's contract, swappable without touching the engine.
* Symbols resolve downward only: engine and app images resolve `lifetime` through `-undefined dynamic_lookup`; no dylib statically links `lifetime`.
* Cross-image state is host-owned: values that must survive a swap live in host-allocated flat state blocks; a swapped image re-resolves handles on attach; no pointer from a retired image is ever dereferenced.
* Never `dlclose` an image carrying Objective-C metadata in a live session; the retire ring keeps old images mapped through the grace window.

### Dynamic Module ABI Verification Law

* Before binding trampolines, verify ABI headers: ABI version, class registry hashes, exported symbol alignments.
* Retiring modules cycle through ABA-safe retirement rings.
* Reject incompatible modules cleanly; keep the previous working version active.

## Repo-Local Extensions

* None — no managed exceptions to universal laws.

## Readiness Cross-Reference

* Feature readiness: hotcwap readiness gist.
* Open blockers: ecosystem blockers gist.
* Proof lawbook: `tests/test-preferences.md` — `./tools/prefs test "<title>"`.
* Evidence ledger: `metadata/gists/test-checklist.md` — `python3 tools/test_checklist.py check`.
