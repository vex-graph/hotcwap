# hotcwap — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal constitution: `gist.md` is the compressed law list; `PREFERENCES.md` is its rationale.

## 0. Constitution Link (supreme)
- [gist.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a#file-gist-md) — the compressed law list, mirrored in this workspace at `../../metadata/gists/preferences/gist.md`. It is the only normative law text; `../../../metadata/gists/preferences/PREFERENCES.md` carries the rationale and is never ingested whole. Numbers are slice addresses inside one file, never law identity: cite by Title.
- All universal laws bind this repository. Read one on demand from the workspace root: `./tools/prefs law "<title>"`, `./tools/prefs section "<section>"`, `./tools/prefs repo <name>`.
- Exclusive `hotcwap` (R1 Dynamic Module Reloader / ABI Airlock) preferences. R0 comprises Lifetime (Kernel Host: OS windowing, process, event loop, capabilities). R1 comprises Relational Engine (permanent memory bank, storage, cache, I/O) and Hotcwap (optional live reload substrate). R1 stays resident for process lifetime. Hotcwap manages safe dylib loading, trampoline routing, and retirement rings without modifying host memory layouts.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the compressed constitution (`../../metadata/gists/preferences/gist.md`); this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **Dynamic Module ABI Verification Law** | R1 Dynamic Airlock | Mandatory for `hotcwap` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### Dynamic Module ABI Verification Law

#### Definition:
Before binding trampolines, hot-reloaded dylibs verify their ABI compatibility
header: ABI version, class registry hashes and exported symbol alignments.
Retiring modules cycle through ABA-safe retirement rings.

#### The Why:
Without ABI validation, changed struct fields can misalign memory and crash.
Validate before swapping to prevent runtime crashes during live iteration.

#### The Rule:
1. **Pre-Flight Verification:** Check ABI headers before swapping function pointers.
2. **Graceful Rejection:** Reject incompatible modules cleanly; keep the previous working version active.

---

## 3. Repo-Local Extensions (managed, per the Conflict Triage Law)

### Explicit Cold Update Boundary

`Hot_updateCold` requires caller-owned stop-the-world exclusion of all module
callers/callbacks and drained work. Its complete flat section set is bounded by
module/snapshot ABI; trusted SHA256 references come from outside model output.
Verify all private load copies before constructors. After trusted load,
`Hot_cold_abi` gates export compatibility (checksums alone prove no ABI/schema).
Save is observational; restore/migrate mutate candidate state only. Rejection
retains live code/state and reverts disk generation; unresolved rollback blocks
resumption pending host recovery. No crash journal, signatures, hostile-parent
sandbox or arbitrary native-code rollback; legacy automatic poll semantics differ.

Trampoline rows grow on cold registration under reader exclusion, preserving
indices and reusing exact symbol identities. Atomics publish targets, not backing
relocation or image lifetime. Geometry/count/owned backing change only through
register/reserve/free: a scoped managed accessor exception under the Conflict
Triage Law and Single Class Per File Law (Java Law). Count refusals per epoch;
first allocation/extent refusal reports request/bound. Cold swaps forbid
section/export removal and clear old fallbacks after quiescent retirement.

;;INTENTION("R1 Dynamic Module Reloader: dynamic dylib hotloading, ABI verification, ABA-safe retirement rings, and trampoline dispatch.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [hotcwap](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-hotcwap-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
