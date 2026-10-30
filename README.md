# hotcwap. hot-c-wap. R1 Host Supervisor — thin nano-VM.

## Current State

hotcwap is the **R1 host supervisor** — the process that boots first and tears
down last, owning the OS window and the dynamic-module reloader. It is a real
runtime with a working macOS backend, not a finished multi-platform host.

- **Implemented:** the `Kernel` (master + transient arenas; process / application
  / console / commandline registries), the four process kinds, the hot-reload path (`hot/*`:
  `dlopen`/`dlsym` trampolines, the retire ring, the `manifest.json` install
  ladder), and the window abstraction with the macOS AppKit backend
  (`window/window_cocoa.m`) proven on this host.
- **Recorded proof is partial.** Only a minority of hotcwap files carry executed
  evidence in `../../metadata/gists/test-checklist.md`. Owner tests exist under `../../../tests/hotcwap`
  (loader, kernel, window families), but most rows are still **unrecorded** — a
  green `b build` plus the presence of owner tests is not the same as recorded
  per-file evidence.
- **Explicit cold update slice:** `Hot_updateCold` accepts a complete flat library
  plus trusted SHA256 references, enforces the mounted catalog at load admission,
  checks all private images before loading, validates `Hot_cold_abi`, saves
  synchronously and restores into candidate images before publishing targets.
  Detected rejection reverts disk current/generation and retains running state;
  unresolved rollback requires host recovery. Dispatch rows grow cold and reuse
  symbol identities; there is no arbitrary update-count ceiling. Owner runner:
  `python3 tests/hotcwap/hot/cold_update_test.py` (real two-section repeated swaps).
  Caller exclusion is mandatory, not a new concurrent loader/lifetime guarantee.
- **Draft / unproven:** the Linux X11, Wayland and Win32 window backends
  (`window_linux.c`, `window_wayland.c`, `window_win32.c`) are drafts that need
  their own host to compile and are explicitly unproven on macOS. Live Hot-loader
  integration with Relational Engine storage, and rollback-tested schema
  upgrades, remain future proof; `spoke/lifetime` wiring is preserved, not
  automatic. No C/Rust atomic-layout compatibility is assumed.
- **Platforms:** proven on Apple Silicon macOS 14+ only. Linux and Windows host
  backends are unproven here.

Zero-downtime dynamic module hot-reloading, persistent OS windowing, and process supervision.

A play on the term **hot swap** — `hotcwap` is an infrastructure runtime designed to reload compiled C23 dynamic libraries in real-time without restarting the process, losing application state, or destroying the native operating system window.

In conventional game architectures, window management and simulation loops are tightly tangled. If a module crashes, reloads, or reconfigures, the window flickers, the graphics device is destroyed, and the event pump restarts. 

`hotcwap` enforces a strict architectural division: **a window belongs to the operating system and Thread 0, while simulation and graphics logic belong to reloadable modules.** By holding the window handle stable in `hotcwap`, modules and shaders can be swapped seamlessly on the fly.

---

## Key Architecture & Strengths

* **OS Window Decoupling**: Thread 0 hosts the native platform window (`AppKit` / Cocoa on macOS; X11/Wayland on Linux). The display link, event pump, and surface layer persist indefinitely across module reloads.
* **Decoupled Window Backend**: A window is a dumb surface + callback bridge (`window/window_cocoa.m`) — pure AppKit, zero Vulkan/Metal. It answers `Window_*` calls from graphvex and R5 apps through the per-window `WindowEvent` lifecycle registry; the GPU-era composite/attach surface is retained as `;;INTENTION` stubs until the darling compositor migrates onto the bridge (the Window Decoupling Law).
* **Microsecond Dynamic Reloader**: Swaps loaded `.dylib` function pointer dispatch tables with zero frame interruption. `Hot_poll` (`hot/hot.c`) compares the manifest's per-library generation stamp (`bin/current/<library>.generation`) against its last-seen generation; on a move it snapshots the running module state on an off-thread save worker (the Bounded Wait Law), then on the next poll dlopens + fail-closed verifies the whole new set (`VkModuleGetTrampolines` on every section), rehydrates the saved state into the STAGED images BEFORE any commit, then atomically swaps trampolines. A new image whose `Hot_restore` rejects its saved state rolls the whole swap back (automated state rollback) — the old generation stays live and the stamp never advances, so the next poll self-heals once a fixed set is promoted. The install-ladder authority + `manifest.json` catalog (`hot/manifest.h/.c`) gates what lands in `bin/current/<library>` via the `MANIFEST_UPDATE`/`MANIFEST_PROMOTE` verbs — the loader trusts the ladder placement. Vulkan module loading lives in graphvex (`src/vulkan/vk_loader.c`) — hotcwap holds no Vulkan code.
* **Low-Latency Event Pump**: Decoupled polling for keyboard, mouse, and touch in bounded 25ms slices (the Bounded Wait Law), mirrored into the vexspoke input rings per-window.

---

## Workspace Integration & How to Use It

`hotcwap` sits at **R1 host** in the supervisor order (the Vertical Integration Law). It boots first as Kernel Host, owns the master + transient arenas and the Application registry, and tears down last — depending only on `vexspoke` shapes + `graphvex` GPU types, never on `darling`/`api-haven`/engines. The full ecosystem map lives in the workspace root `../../README.md` and the ecosystem wiki, not here.

### R2 storage boundary

R2 has two cooperating owners: Vexspoke provides CPU computation, math,
algorithms, synchronization and behavior; Relational Engine owns memory/storage,
stable row chunks, variable bindings and native C search over Rust-owned spans.
**R1 owns their lifetimes/residency:** stop admission and active users before
storage destruction, and keep code/storage resident across consumer reloads.
Native IO/NIO and the default allocator implementation now come from Relational
Engine with compatible C semantics. Broader collection migration remains staged;
`spoke/lifetime` wiring is preserved, not automatic schema migration or live Hot
loader integration. Never assume C/Rust atomic-layout compatibility. GPU shaders and
dispatch remain Graphvex R3.

### Kernel lifecycle example

```c
Kernel *k = Kernel();              // 1. kernel: master + transient arenas
Application *app = Application("vex vk probe"); // 2. application
Kernel_addApplication(k, app);     // 3. adding an application in the kernel
Window *w = Window();              // 4. window (OS-owned, never hot-updated)
Application_addWindow(app, w);     // 5. adding a window to the application
Application_run(app);              // 6. run the application (Phase 1: blocking, single-app)
Application_free(app);             // 7a. end the application (detach-only)
Kernel_destroy(k);                 // 7b. end the kernel (stops apps, arenas LAST)
```

### Build

```sh
./tools/b build hotcwap # from the Vexgraph workspace root
```

### Standalone autonomy
The Standalone Autonomy Law requires runtime dependency closure through
[b](https://github.com/vex-graph/b), with real R2/R3 contracts. Workspace editor
configuration is not a standalone runtime build.

---

## What's in this repo

* **`kernel/kernel.h/.c`** — R1 Host Supervisor (thin nano-VM): `Kernel {arena, transientArena, applications[KERNEL_MAX_APPS]}`. Boots first, tears down last. Holds opaque Application handles + callbacks, never engine headers.
* **`kernel`** — The four process kinds (the Process taxonomy): `process` (one-shot invocable), `application` (executable identity + window registry + hot-module slot), `console` (tty/session pump), `commandline` (owned terminal-application host: the single resize surface, bounded input, one present flush, a swappable app seam — `commandline_ncurses.c` is the one ncurses backend).
* **`spoke/lifetime.h/.c`** — R1→R2 memory substrate contract for vexspoke (and vexspoke only): the `Lifetime` struct (master + transient arenas, opaque handles, type-attested). The single seam including vexspoke memory headers — `kernel` names no vexspoke internal type.
* **`window/window.h`** — Platform-agnostic window abstraction: creation, sizing, fullscreen toggles, input event dispatch, and title management.
* **`window/window_cocoa.m`** — Native macOS AppKit backend (pure AppKit, zero Vulkan/Metal): window lifecycle, event pump, chrome, traffic-light API, per-window `WindowEvent` registry.
* **`window/window_linux.c`** — Linux X11 fallback backend (kept for hosts without Wayland dev libraries).
* **`window/window_wayland.c`** — Lean Linux Wayland draft backend (1:1 cocoa mirror; opt-in via `-DHOTCWAP_USE_WAYLAND=ON`, auto-falls back to X11 when `wayland-client`/`xkbcommon` are absent — `;;DRAFT`, needs a Linux host to compile).
* **`window/window_win32.c`** — Lean Windows Win32 draft backend (1:1 cocoa mirror; the production candidate, with `window.c` kept as the legacy safe-default stub via `-DHOTCWAP_WIN32_LEGACY=ON` — `;;DRAFT`, needs a Windows host to compile).
* **`hot/hot.h/.c`** — Dynamic module reloader: `dlopen`/`dlsym` lifecycle wrappers and runtime state preservation.
* **`hot/manifest.h/.c`** — The `MANIFEST(...)` install-layout authority (the "manifest binary way"): the install tree plus the `manifest.json` library catalog the downloader edits. `MANIFEST(kind, org, app)` resolves `<application-data>/<org>/<app>` once; `MANIFEST_LIBRARY(...)` registers library KEYS; `MANIFEST_UPDATE(library, payloadDir)` fail-closes undeclared payload sections and stages `bin/new/<library>`; `MANIFEST_PROMOTE()` slides each library's generations — the MODE-2 cold-swap ladder beneath the MODE-1 `Hot_poll` hot swap. See `docs/install.md`.

---

## Requirements

* C23 compiler (Clang with `-std=gnu23`).
* macOS (AppKit, Cocoa) or Linux (X11).
* The workspace build system, `b` (bundled at `../b`).

## Scope and Limitations

Cold updates are trusted/stable-tree, caller-excluded transactions, not a
downloader, signature verifier, hostile-directory sandbox or power-loss journal.
They retain every declared section/export and use the existing 4 KiB snapshot
slots. Module save/restore/constructors must obey the trusted candidate-only
contract. Legacy `Hot_poll`/global promotion retains its separate poll-aged
retirement limitations and is not upgraded to cold safety by this API.
ASan/UBSan host and UBSan module proof does not establish Apple fully-ASan dylib
unload/reload, zero leaks, complete failure injection or other-platform support.

hotcwap is the R1 layer only. It deliberately does not do the following:

- **It owns no renderer.** Vulkan lives in graphvex; the window is a dumb
  surface + callback bridge — `window/window_cocoa.m` is pure AppKit, zero
  Vulkan/Metal. The composite/attach surface is retained as `;;INTENTION` stubs
  until the darling compositor migrates onto the bridge.
- **It includes no `darling`/`api-haven`/engine headers** — only vexspoke shapes
  and graphvex GPU types.
- **It claims no multi-platform proof.** macOS is proven; the Linux X11 backend
  is a fallback, and the Wayland and Win32 backends are drafts pending their own
  hosts.
- **Standalone runtime dependency closure requires its own build proof.**
