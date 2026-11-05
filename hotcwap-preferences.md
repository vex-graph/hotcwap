# hotcwap — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal constitution: `gist.md` is the compressed law list; `PREFERENCES.md` is its rationale.

## 0. Constitution Link (supreme)
- [gist.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a#file-gist-md) — the compressed law list, mirrored in this workspace at `../../metadata/gists/preferences/gist.md`. It is the only normative law text; `../../../metadata/gists/preferences/PREFERENCES.md` carries the rationale and is never ingested whole. Numbers are slice addresses inside one file, never law identity: cite by Title.
- All universal laws bind this repository. Read one on demand from the workspace root: `./tools/prefs law "<title>"`, `./tools/prefs section "<section>"`, `./tools/prefs repo <name>`.
- Exclusive `hotcwap` (R1 Dynamic Module Reloader / ABI Airlock) preferences. R0 comprises Lifetime (Kernel Host: OS windowing, process, event loop, clock, capabilities) **plus Relational Engine** (the ecosystem's single allocator, storage, cache, I/O) — both statically linked into the host and never swapped. Hotcwap lives above R0 as the optional reload substrate: it loads and swaps the one R2–R4 engine image (`vexspoke` + `graphvex` + `darling-framework`) and each R5–R6 superapp image, never R0. Hotcwap manages safe dylib loading and ABA-safe retirement rings without modifying host memory layouts.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the compressed constitution (`../../metadata/gists/preferences/gist.md`); this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **Dynamic Module ABI Verification Law** | R1 Dynamic Airlock | Mandatory for `hotcwap` |
| **Two Load Units Law (R0 static, R2-R4 one image, apps separate)** | R1 Dynamic Airlock | Mandatory for `hotcwap` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### Two Load Units Law (R0 static, R2-R4 one image, apps separate)

#### Definition:
Three tiers, **two load units**. Tier 1 — `lifetime` + `relational-engine` (R0) — is
statically linked into the host executable and is never a dylib: it exports its
symbols, and nothing ever swaps it. Tier 2 is **one engine image** holding
`vexspoke` + `graphvex` + `darling-framework` (R2–R4), loaded with
`-undefined dynamic_lookup` against the host's exported symbols. Tier 3 is **one
dylib per superapp** (R5–R6, e.g. `semicolon`), also `-undefined dynamic_lookup`,
swapped independently of the engine image. All loads are `RTLD_NOW|RTLD_LOCAL`;
a path-unique copy per generation defeats the dyld path cache.

#### The Why:
A dylib loaded once and never unloaded is static linking with an extra failure
mode: if tier 1 failed to `dlopen`, the host would have no allocator and could
not even `THROW`, and every `Memory_alloc` would pay an extra indirection on the
hottest path. R2–R4 interlock — darling calls graphvex calls vexspoke — so
splitting them per repo would recreate cross-layer version skew inside the swap
layer; one image is one ABI gate and one swap.

#### The Rule:
1. **R0 is a link unit, not a load unit.** `lifetime` and `relational-engine` are
   compiled into the host executable. A swap request naming R0 is a hard refusal,
   never an unload. R0 changes only by versioned cold release plus restart.
2. **One engine image.** `vexspoke`, `graphvex` and `darling-framework` ship as a
   single dylib with a single contract version. The host rejects a version
   mismatch before committing any swap.
3. **Apps are separate dylibs.** Each superapp is its own image, versioned against
   the engine image's contract, and swappable without touching the engine.
4. **Symbols resolve downward only.** Engine and app images resolve R0 through
   `-undefined dynamic_lookup`; the host exports (`-rdynamic` on Linux, default on
   macOS). No dylib statically links R0 — that is the shadow-owner defect.
5. **Cross-image state is host-owned.** Values that must survive a swap live in
   host-allocated flat state blocks; a swapped image re-resolves handles on
   attach. No pointer from a retired image is ever dereferenced.
6. **Never `dlclose` an image carrying Objective-C metadata** in a live session;
   the retire ring keeps old images mapped through the grace window instead.

---

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
