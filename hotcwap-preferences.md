# hotcwap — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal Supreme Constitution: workspace-root preferences.md, published on Gist.

## 0. Constitution Link (supreme)
- [preferences.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a) — real, Git-ignored workspace-root file at ../../../preferences.md, not a tracked Vexspoke file or symlink.
- All universal laws in `../../../preferences.md` are mandatory and binding across the ecosystem.
- This document codifies **exclusive** preferences for `hotcwap` (R1 Kernel Host). R2 comprises Vexspoke computation/behavior and Relational Engine memory/storage/native C search. R1 owns lifetimes/residency and must exclude active users before storage destruction. Canonical IO/NIO and the default native Memory implementation are now RE-owned; R1 may borrow those contracts directly. No Rust allocator rewrite, automatic schema migration or live engine reload integration is implied.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the canonical `../../../preferences.md` Index; this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **One-Seam Canvas Window Architecture Law** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Present-On-Demand Law (composite ≠ render)** | R1 Kernel Host | Mandatory for `hotcwap` |
| **Dynamic Module ABI Verification Law** | R1 Kernel Host | Mandatory for `hotcwap` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### One-Seam Canvas Window Architecture Law

#### Definition:
The host window composite owns exactly ONE on-screen Metal layer — the seam
canvas (`CAMetalLayer`) — beneath an optional transparent `NSVisualEffectView`
blur substrate. All Vulkan-rendered boards (scene backdrop, content UI) are
retained OFFSCREEN targets composited into the seam image by the render repo
(the Window Compositing Layer Order Law + the Single-Seam Canvas Law); no
per-scene or per-widget `CALayer` and no Vulkan window presentation path.

#### The Why:
A single on-screen Metal layer keeps one frame cadence for the whole window.
Its drawable size follows the live window-content backing size, not a fixed
monitor-sized buffer. Board rectangles are resolved against that size.

#### The Rule:
1. **Bottom Layer:** window blur substrate; no UI widget owns a display layer.
2. **Top Layer:** the one seam `CAMetalLayer` (`geometryFlipped = YES`,
   TopLeft-pinned, native-pixel `drawableSize`); it is the only on-screen
   Metal layer in the window. The renderer composites retained board targets
   into its drawable; hotcwap continues to own window and event routing.

---

### Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")

Freezing the canvas extent and early-returning from layout during mouse drags (`Window_isLiveResizing`) is
**strictly abolished**. Deferring work to "settle" is an artificial cop-out
that produces frozen windows, dead animations, and visual tearing. During an
active window drag or live resize, the rendering pipeline operates
continuously:

1. **Dynamic Extent:** `CAMetalLayer.drawableSize` tracks the live
   window-content backing bounds on every resize event; retained GPU targets
   are resized without creating a Vulkan presentation object.
2. **Live Layout Recalculation = Real-Time Anchor Feel:** Container layout and
   anchor resolution run on live bounds every frame of the drag — pinned
   elements (e.g., right-anchored, bottom-anchored) recalculate their offsets
   dynamically and re-present, so they stay glued to their edges in real time.
   The resolution granularity is one vsync (60/120Hz), which IS the native
   contract: CoreAnimation itself commits per frame, so per-frame latency is
   indistinguishable from a native view. Nothing is ever stale beyond the
   latency of a single frame. The sub-frame gap between a resize event and
   the next frame is covered by the instantly-moved layer frame (the
   Single-Transaction Live Coordination Law) plus the gravity-pinned last
   bitmap — never black, never torn, never stretched-out-of-anchor.
3. **Unbroken Animation & Presentation:** Animation tickers,
   dirty-propagation, and command buffer presentation
    continue rendering and presenting at the
   display's native refresh rate (60/120Hz) throughout mouse drags and moves.

---

### Present-On-Demand Law (composite ≠ render)

A surface presents only on demand. Demand is change: motion, layout, text,
hover, or a scene publishing a new frame. Anything static presents once and
then rests on its last composite — the compositor never re-presents clean
content and never re-invokes a scene's render handler.

#### The Why (subsumes the former No Double-Render Law)
The old law forbade stamping one panel into two display targets. Present-on-demand
makes that impossible by construction: a scene RENDER (its world into its
retained offscreen target — `VkLayer`, own thread, own FPS) is distinct from
a COMPOSITE (sample published targets + paint the UI tree into the canvas at
vsync). The canvas painter only samples published targets — it cannot
re-render a scene. Double-render is structurally unreachable, not merely
forbidden.

#### Present modes (per scene):
- `COMPOSITED` (default): the scene owns retained flight render targets
  (offscreen images + acquire/render semaphores + fences) on its own
  timeline; the canvas samples the latest published frame at the anchor rect,
  in tree z-order interleaved with UI. One canvas total — no per-scene
   surfaces and no DIRECT mode (the Single-Seam Canvas Law).

#### Composite rules:
- The presenter wakes only on demand: a dirty tree, a published layer frame,
  or a live-resize drag (the moving edge is itself a ticket). An idle tree
  sleeps — zero presents, zero GPU work; power is the free win.
- UI paints the FULL tree on any dirty tick (immediate-on-demand; damage
  rects are a later optimization, never a first move). Static content rests:
  the canvas is neither re-acquired nor presented.
- Scene anchors are plain resolved C rects into the canvas; WindowServer-
  native anchoring (`presentsWithTransaction`, `autoresizingMask`) lives on
  the single canvas layer only.

---

### Dynamic Module ABI Verification Law

#### Definition:
Dynamic dylibs loaded during hot-reload passes must verify their ABI compatibility header before binding function trampolines. The loader inspects ABI version, class registry hashes, and exported symbol alignments. Retiring modules cycle through ABA-safe retirement rings.

#### The Why:
Live reloading without ABI validation causes memory misalignment and crashes when struct fields change between compilations. Validating before swapping guarantees zero runtime crashes during live iteration.

#### The Rule:
1. **Pre-Flight Verification:** ABI headers are checked before swapping function pointers.
2. **Graceful Rejection:** Incompatible dynamic modules fail cleanly, leaving the previous working version active.

---

## 3. Repo-Local Extensions (managed, per the Conflict Triage Law)

### Explicit Cold Update Boundary

`Hot_updateCold` requires caller-owned stop-the-world exclusion of all module
callers/callbacks and drained work. Its complete flat section set is bounded by
the existing module/snapshot ABI; trusted SHA256 references are supplied outside
model output. All private load copies verify before any constructor runs.
`Hot_cold_abi` gates export compatibility after trusted loading; checksums alone
are not ABI/schema proof. Save must be observational; restore/migrate mutate
candidate state only. Rejection retains live code/state and reverts current disk
generation; unresolved rollback blocks resumption and requires host recovery.
No crash journal, signatures, hostile-parent sandbox or arbitrary native-code
rollback is offered. Legacy automatic poll semantics remain distinct.

Trampoline rows grow on cold registration under reader exclusion, preserving
indices and reusing exact symbol identities. Atomics publish targets, not backing
relocation or image lifetime. Geometry/count/owned backing change only through
register/reserve/free, a scoped managed accessor exception under the Conflict
Triage Law and Single Class Per File Law (Java Law). Refusals count per epoch;
the first allocation/extent refusal reports its request/bound. Cold swaps forbid
section/export removal and clear old fallbacks after quiescent retirement.

;;INTENTION("R1 Kernel Host: native AppKit/Metal bridge; one seam canvas (single on-screen CAMetalLayer) hosting all Vulkan boards; dynamic dylib hotloading.")

---

## 4. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [hotcwap](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-hotcwap-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
