# hotcwap example — setup.c is the whole integration

hotcwap is a **library**. An application adds hot-reload by calling three
functions and one per-frame function. There are no loader structs, no ordering
rules, and no version-probe code in the application.

```
./run.sh
```

---

## 1. The application's entire setup file

```c
#include "hot.h"
#include "hot/stack.h"

HotStack *stack = HotStack();              // defaults: "core" + "app"

Hot_loadCore(stack);                       // the engine image, first
Hot_loadApplication(stack);                // the application image, gated
Hot_loadPlugin(stack, "plugin");           // an independent add-on
```

and per frame:

```c
Hot_pollAll(stack, &changed);              // core → app → plugins
```

That is `setup.c`. The ordering, the version gate, and what happens when a
layer moves all live in `hot/stack.c`.

---

## 2. The three concerns

| call | what it loads | gate |
| :--- | :--- | :--- |
| `Hot_loadCore` | the engine image (R2–R4) | none — it publishes the contract |
| `Hot_loadApplication` | the application image | **must accept the core contract** |
| `Hot_loadPlugin` | an independent image (R5–R6) | none — never blocks anything |

The contract is two exported numbers:

```
core image   exports  uint64_t Hot_contract_abi(void);        "I am contract N"
app image    exports  uint64_t Hot_requires_core_abi(void);   "I need contract M"
```

`M != N` → the app does not run. It stays loaded but idle, and comes back on
its own the moment a matching image is promoted. Nothing is uninstalled and
nothing is lost.

---

## 3. What you see when you run it

```
[setup] core loaded, contract 1
[setup] plugin loaded
[app v1] attached to core contract pointer 0x1006d05f0
[app] frame    1  t=0.00  core.pulse=0.00  (wants core abi 1)
[plugin] tick 1  t=0.00

[setup] stage + promote core v2
[setup] core moved -> contract 2
[setup] app idle: app wants core contract 1, core publishes 2
[plugin] tick 81  t=1.33                     ← plugin is unaffected

[setup] stage + promote app v2
[setup] app re-attached to the new core
[app v2] attached to core contract pointer 0x10092c5f0
[app] frame   81  t=2.05  core.pulse=8.20  (wants core abi 2)

[setup] stage + promote app v3 (wants a contract that does not exist)
[setup] app idle: app wants core contract 99, core publishes 2
[plugin] tick 161  t=3.03                    ← still unaffected
```

Four things to notice:

1. **`setup.c` never decided any of this.** It promoted images on a timer and
   called `Hot_pollAll`. The gate, the order, and the idle state came from the
   library.
2. **The core went first.** `Hot_pollAll` polls core before app, every frame.
3. **The app came back by itself** when a compatible image was promoted — and
   its frame counter continued at 81, because its state crossed the swap.
4. **The plugin never noticed.** No contract, no gate, no interruption.

---

## 4. Files

| file | role |
| :--- | :--- |
| `setup.c` | the whole integration: 3 loads + 1 poll per frame |
| `core.c` | engine image; publishes `Hot_contract_abi` |
| `app_v1.c` | app version 1 — its own `setup()`/`run()`, wants contract 1 |
| `app_v2.c` | app version 2 — **different source, different output**, wants 2 |
| `app_v3.c` | app version 3 — wants contract 99, must never run |
| `app_common.h` | the state record + save/restore the versions share |
| `plugin.c` | independent image; declares nothing |
| `build.sh` | builds the host and six images |
| `run.sh` | fresh tree, then the promotion schedule |

`app_v1.c` and `app_v2.c` are two files, not one file compiled twice. v1 prints
a pulse; v2 prints a bar plus a running peak. Same state record, same contract,
genuinely different code.

Build output:

```
build/setup          the frozen process (links hotcwap)
build/core/v1|v2     contract 1 and 2
build/app/v1|v2      wanting contract 1 and 2
build/app/v3         wanting contract 99 — the refusal proof
build/plugin/v1      independent
```

---

## 5. The ladder on disk

`setup.c` hardcodes the mount:

```c
MANIFEST("/", "Users", "vexgraph", "~v", "worktree", "vex",
         "hotcwap", "example", "install")
```

```
install/
  manifest.json
  manifest/current/images/core/core.dylib    ← what the loader reads
  manifest/current/images/core.generation    ← the swap trigger
  manifest/current/images/app/app.dylib
  manifest/current/images/app.generation
  manifest/current/images/plugin/plugin.dylib
  manifest/new/images/<lib>/                 ← a download waits here
  manifest/<slot>/readme.md                  ← per-slot changelog
```

`MANIFEST_UPDATE(lib, dir)` stages into `new/`, `MANIFEST_PROMOTE_LIBRARY(lib)`
slides it to `current/` and bumps that library's generation. `Hot_pollAll`
notices the stamp on its next pass.

---

## 6. The swap underneath, step by step

`Hot_poll()` is one line of real work: compare the on-disk generation against
the one this `HotModule` last loaded. Equal → return. Different → swap.

When they differ, `perform_verified_swap` runs five steps **in this order**, and
the order is the safety:

```
1. snapshot   worker thread calls the OLD image's Hot_save(blob)
              → off-thread, the frame loop never blocks

2. stage      copy the dylib to cache/load/<lib>/<serial>/ then dlopen it
              → unique path, because dyld caches by path; the same path
                would hand back the stale image

3. verify     require Hot_getExports to exist, be non-empty, and carry no
              null rows
              → any failure closes ALL staged images, old code stays live

4. restore    call the NEW image's Hot_restore(blob) BEFORE commit
              → reject = rollback. generation never advances, old code and
                old state are still live

5. commit     retire the old handle into the retire ring, adopt the new
              handle, point the symbol rows at the new functions
```

Old images stay mapped for `HOT_RETIRED_GENERATIONS` (4) polls, so a call that
was already in flight finishes inside still-valid code. **That grace window is
also what keeps Objective-C images safe** — you never `dlclose` something the
runtime may still point at.

---

## 7. What the application still owns

Only what it is uniquely qualified to know:

- **its own symbols** — `Hot_getSymbol(...)` is re-fetched whenever a layer
  moves, because a pointer into a retired image is meaningless;
- **re-binding the app to the core** — `App_attach(corePulse)` after a core
  move, since only the application knows its own attach contract;
- **when to promote** — this demo uses a timer; a real one calls the same two
  verbs from a downloader.

Everything else — load order, the contract gate, idle/return behaviour,
teardown order — belongs to `hot/stack.c`.

---

## 8. Experiments that teach the most

**a. Break the state schema.** Change `APP_SCHEMA` in `app.c`, rebuild, run.
`Hot_restore` rejects the previous blob → the swap rolls back → the old
generation keeps running. That is the rollback gate.

**b. Watch the two-poll swap.** Add a print in `setup.c` right after
`Hot_pollAll` showing `changed`. A state-carrying swap kicks the snapshot on one
frame and commits on the next, because `Hot_save` runs on a worker thread.

**c. Drop the gate.** Delete `Hot_requires_core_abi` from `app.c`'s export
table. The stack treats the app as ungated and it runs on any contract.

**d. Delete `install/` and run again.** It re-seeds — but not because
`MANIFEST_IS_FIRST_RUN()` says so. hotcwap keeps an install **ledger** outside
the tree (`~/Library/Application Support/vexgraph/ledger`) per the Install
Ledger Law. `generation == 0` is what triggers the re-seed.

**e. Watch `cache/load/<lib>/`.** It is empty between swaps on purpose: the
loader copies the dylib to a numbered path, `dlopen`s it, then unlinks the
copy. The unique path defeats the dyld path cache; the cleanup keeps `cache/`
from growing.

**f. Read `manifest/current/readme.md`.** Every stage and promote appended a
block — library, phase, generation, timestamp. The ladder's own changelog.

---

## 9. What is deliberately missing

- **No per-repo images.** The engine ships as one image (vexspoke + graphvex +
  darling) per the Two Load Units Law; this demo has a single `core` section.
- **`throwable.c` is not linked.** The loader never calls it, and it drags
  `ChunkedList` + `Exception` + `Window` into a process that needs none.
- **No cold update path.** `Hot_updateCold` (stop-the-world, SHA-256 verified)
  is not exercised. Only the live poll path is.
- **No trampoline reliance.** `Hot_getSymbol` re-fetches after every move; the
  swap happens at a frame boundary, so nothing is mid-call.
