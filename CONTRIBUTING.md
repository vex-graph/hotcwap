# Contributions & Engineering Manifesto (hotcwap)

This project is a strictly solo development process conducted in tight pair-programming partnership with an AI coding assistant.

It serves as an architectural manifesto for **R1 Host Supervision and OS Windowing**:
hot-reloading modules and managing lifetimes. R2 comprises Vexspoke CPU
computation/behavior and Relational Engine memory/storage/native C search. R1
keeps storage/code resident and excludes active users before destruction; current
Lifetime wiring is preserved; default native IO/NIO is now RE-owned and linked.
Native C semantics are preserved, not rewritten into Rust; engine reload is unproved.

---

## 1. The AI-First Architecture Manifesto & Boilerplate Defense

This codebase strictly enforces the verbose, explicit boilerplate required across the `vexgraph` ecosystem:
- Strict prohibition of arrow syntax (`p->field` is banned; only explicit `(*p).field` is permitted).
- Single Class Per File (the Java Law: one public `typedef struct` per `.h`/`.c` pair).
- Arity-overloaded explicit constructor dispatch macros (`Class_0()`, `Class_1()`).
- Complete, symmetric getters and setters for all struct fields.
- Strict dest-last parameter ordering `(a, b, dest)`.
- Two-layer member access cap (`(*layer1).layer2` maximum).
- Exhaustive `;;OVERVIEW` blueprints mirrored at the top of every implementation file.

### Why the Boilerplate Exists
This boilerplate is **not** an accident, nor is it a misunderstanding of idiomatic C. It is an intentional, machine-verifiable scaffold built specifically for **AI-Human Pair Systems Programming**:
1. **Machine Comprehension**: By eliminating `->` and restricting every file to a single class, an AI coding agent can hold the exact, un-aliased memory layout of any component in its context window without hallucinating field collisions.
2. **Explicit Indirection**: `(*ptr).field` makes every pointer dereference visible and accountable.
3. **AI-Maintained Rigor**: The AI agent authors, refactors, and validates the dense boilerplate, freeing human focus for high-level OS synchronization, dynamic module swapping, and memory safety.

---

## 2. Sanity Warning for External Contributors

> [!WARNING]
> **SANITY NOTICE FOR EXTERNAL CONTRIBUTORS**
> This repository is not designed for traditional C conveniences, casual hacking, or stylistic shortcuts. It is an unapologetic, machine-verifiable manifesto of AI-augmented systems architecture.
>
> **If you do not approve of this architecture or cannot find peace with this philosophy, consider leaving this repository for your own sanity.**
>
> We do not accept Pull Requests, issues, or unsolicited stylistic refactors attempting to re-introduce `->`, combine multiple classes into one file, or bypass explicit getters/setters. Upstream is maintained exclusively by the author and the AI agent.

---

## 3. Workspace Constitution & Repo-Local Preferences

All architectural rules and style invariants are governed by the central constitution:

- **[Compressed law list](../../metadata/gists/preferences/gist.md)** and [published preferences](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a); read only relevant laws with `./tools/prefs`.
- **[hotcwap-preferences.md](hotcwap-preferences.md)** (repo-local mirror binding hotcwap)

Under the Living Documentation Law, update affected contracts in the same cycle.
Universal changes are published to the existing Gist and byte-verified; repo-local
documentation is committed locally under the Git Workflow Law. Never auto-push.

---

## 4. `hotcwap` Architectural Invariants

| Invariant | Specification |
| :--- | :--- |
| **R1 Module Supervision** | Watches, verifies ABI, swaps and retires dynamic libraries without restarting the process. |
| **Two-Layer Split Architecture** | Bottom layer is the Vulkan swapchain (or transparent NSVisualEffectView blur); top layer is composited `CALayer`s backed by `IOSurface`. |
| **Zero Steady-State Allocation** | No `malloc`/`calloc` in window event loops, live resize, or module reload paths. |
| **Teardown Order Top-Down** | Detach before free: `Window_destroy` -> `Darling_shutdownCompositor` -> `Vk_shutdown` -> `Memory_freeAll`. Freeing the arena before detaching OS views causes zombie windows and leaks. |
| **Bounded Waits on Joined Threads** | Fences and worker joins must take explicit timeouts (e.g. 100ms) with a drop-degrade path; infinite waits (`UINT64_MAX`) on joins freeze teardown. |
