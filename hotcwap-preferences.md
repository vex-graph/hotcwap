# hotcwap — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal constitution: `gist.md` is the compressed law list; `PREFERENCES.md` is its rationale.

## 0. Constitution Link (supreme)
- [gist.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a#file-gist-md) — the compressed law list, mirrored in this workspace at `../../../metadata/gists/preferences/gist.md`. It is the only normative law text; `../../../metadata/gists/preferences/PREFERENCES.md` carries the rationale and is never ingested whole. Numbers are slice addresses inside one file, never law identity: cite by Title.
- All universal laws bind this repository. Read one on demand from the workspace root: `./tools/prefs law "<title>"`, `./tools/prefs section "<section>"`, `./tools/prefs repo <name>`.
- Exclusive `hotcwap` (R1 Kernel Host) preferences. R2 comprises Vexspoke computation/behavior and Relational Engine memory/storage/native C search. R1 owns lifetimes/residency and excludes active users before storage destruction. Canonical IO/NIO and default native Memory are RE-owned and borrowable here. No Rust allocator rewrite, automatic schema migration or live engine reload integration is implied.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the compressed constitution (`../../../metadata/gists/preferences/gist.md`); this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **One-Seam Canvas Window Architecture Law** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Present-On-Demand Law (composite ≠ render)** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Dynamic Module ABI Verification Law** | R1 Kernel Host | Mandatory for `hotcwap` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### One-Seam Canvas Window Architecture Law

#### Definition:
Exactly ONE on-screen Metal layer belongs to the host composite: seam canvas
(`CAMetalLayer`), beneath optional transparent `NSVisualEffectView` blur.
Vulkan boards (scene backdrop, content UI) are retained OFFSCREEN targets
composited into the seam image by the render repo; ordering is fixed by this
seam canvas, not by per-scene layers — no per-scene/widget `CALayer` or Vulkan
window presentation path.

#### The Why:
One on-screen Metal layer gives the window one frame cadence. Its drawable
tracks live window-content backing size, not a fixed monitor buffer; board
rectangles resolve against that size.

#### The Rule:
1. **Bottom Layer:** window blur substrate; UI widgets own no display layer.
2. **Top Layer:** sole on-screen seam `CAMetalLayer` (`geometryFlipped = YES`,
   TopLeft-pinned, native-pixel `drawableSize`); renderer composites retained
   board targets into it; hotcwap owns window and event routing.

---

### Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")

Freezing canvas extent or early-returning from layout during mouse drags
(`Window_isLiveResizing`) is **strictly abolished**; deferring until "settle"
freezes windows/animations and tears. Render continuously during every drag:

1. **Dynamic Extent:** `CAMetalLayer.drawableSize` tracks the live
   window-content backing bounds on every resize event; resize retained GPU
   targets without creating a Vulkan presentation object.
2. **Live Layout Recalculation = Real-Time Anchor Feel:** Recalculate layout and
   anchors against live bounds each drag frame. Pinned elements (e.g.,
   right-/bottom-anchored) update offsets and re-present, staying at their edges.
   One vsync (60/120Hz) is native granularity: CoreAnimation commits each frame,
   so no content is stale beyond one frame. The sub-frame gap is covered by the
   instantly-moved layer frame (Single-Transaction Live Coordination Law) and
   gravity-pinned last bitmap — never black, torn or stretched out of anchor.
3. **Unbroken Animation & Presentation:** Animation tickers, dirty propagation
   and command-buffer presentation continue at native display refresh
   (60/120Hz) throughout drags and moves.

---

### Present-On-Demand Law (composite ≠ render)

A surface presents only on change (motion, layout, text, hover, or a newly
published scene frame). Static content presents once and rests: the compositor
never re-presents clean content or re-invokes a scene render handler.

#### The Why (subsumes the former No Double-Render Law)
The former law forbade stamping one panel into two display targets. Scene RENDER
(world into retained offscreen target — `VkLayer`, own thread, own FPS) differs
from COMPOSITE (sample published targets + paint UI tree at vsync). Canvas only
samples published targets; double-render is structurally unreachable.

#### Present modes (per scene):
- `COMPOSITED` (default): scene owns retained flight render targets (offscreen
  images + acquire/render semaphores + fences) on its timeline; canvas samples
  the latest published frame at the anchor rect, interleaved with UI in tree
  z-order. One canvas total: no per-scene surfaces or DIRECT mode (Single-Seam
  Canvas Law).

#### Composite rules:
- Presenter wakes only for dirty tree, published layer frame or live-resize
  drag (moving edge is a ticket); idle means zero presents/GPU work.
- Any dirty tick immediately paints the FULL UI tree; damage rects are a later
  optimization, never the first move. Static canvas is neither reacquired nor
  presented.
- Scene anchors are resolved C rects; WindowServer-native anchoring
  (`presentsWithTransaction`, `autoresizingMask`) belongs only to the canvas layer.

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

;;INTENTION("R1 Kernel Host: native AppKit/Metal bridge; one seam canvas (single on-screen CAMetalLayer) hosting all Vulkan boards; dynamic dylib hotloading.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [hotcwap](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-hotcwap-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
