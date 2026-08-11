# Embedding Konclude In-Process in Fast Downward

Design notes for linking Konclude directly into a Fast Downward (FD) planner
process as a shared library, so FD's extended query answering (precondition
and effect checks against a DL ontology) can call Konclude via plain
function calls instead of a subprocess/network protocol.

## Context and decision

- FD's precondition/effect checks may call into DL reasoning **millions of
  times** during search, so per-call transport overhead (HTTP + OWLlink XML
  parsing, or spawning `./Konclude` per call) is not viable at that volume.
- Decision made: link Konclude **in-process** as a **shared library**
  (`.so`/`.dylib`), not statically, and not as a separate
  subprocess/daemon reached over IPC.
- Trade-off accepted knowingly: in-process linking gives near-zero call
  overhead (a normal function call, same address space) but removes OS-level
  process isolation — a crash or hang inside Konclude's reasoning kernel
  becomes a crash or hang of FD's entire search process. There is already a
  known example of this risk: a genuine deadlock exists in Konclude's
  reasoner startup synchronization when configured with exactly 1 processing
  thread (`-w 1`, also the default) — reproduced via `CCLIBatchProcessingLoader::load()`
  and `CReasonerManagerThread::threadStarted()` deadlocking on
  `QSemaphore::acquire()`. The embedding must hard-enforce `-w N` with `N >= 2`
  internally so an FD caller can't reintroduce this bug.

## Pipeline

Each phase should produce something independently testable before moving to
the next — failures in this integration tend to be crashes/memory corruption
rather than clean exceptions, so validating incrementally matters more here
than in typical Python-side work.

### 1. Prove "builds as a library, no `main()`" first, with zero new API code

- Study `KoncludeLIB.pro` and `Source/Control/Interface/JNI/` — this is the
  existing proof that Konclude can build without `Source/mainLoader.cpp`'s
  `main()` (used today for the JNI/Java embedding path).
- Add a new qmake build flag parallel to `KONCLUDE_COMPILE_JNI_INTERFACE`
  (e.g. `KONCLUDE_COMPILE_EMBEDDED_INTERFACE`) that builds everything except
  `mainLoader.cpp` as a shared library (`-shared`), with **no new API yet**.
  Just confirm it links cleanly into `libKonclude.so` / `libKonclude.dylib`.
- This isolates "build system + two-`main()`-functions problem" risk before
  any interface design work begins.

### 2. Design the boundary as a C API, not a C++ API

- For a *shared* library specifically (as opposed to static linking), C++
  ABI (name mangling, STL container layouts, exception propagation across
  the boundary) is not guaranteed stable across compiler versions/toolchains.
  If Konclude and FD are ever built with different compilers, a C++-typed
  API can silently corrupt memory instead of failing cleanly.
- Expose an `extern "C"` facade instead: an opaque handle
  (`typedef void* KoncludeReasonerHandle`) plus functions taking/returning
  only primitives (`const char*`, `int`, `bool`) — e.g.
  `konclude_create_reasoner`, `konclude_destroy_reasoner`,
  `konclude_load_ontology_file`, `konclude_assert_axiom` /
  `konclude_retract_axiom`, `konclude_check_satisfiability`,
  `konclude_last_error`. This is the same technique SQLite and most
  FFI-safe C++ libraries use for their public surface.
- As a side benefit, a plain C API is also callable from Python later via
  `ctypes`/`cffi` if that's ever useful.
- Put this facade in a new `Source/Control/Interface/Embedded/` directory,
  internally wrapping `Source/Control/Command/CCommanderManager`.

### 3. Resolve the `QCoreApplication` / event-loop ownership question

- Read the `CReasonerManagerThread` / `Concurrent` / `Scheduler` init path to
  determine whether Konclude's internals require a **running** Qt event loop
  (`exec()`), or merely require a `QCoreApplication` instance to exist (a lot
  of Qt internals only check `qApp != nullptr`).
- This is a genuine open question requiring source investigation, not an
  assumption to design around ahead of time.
- Implement `konclude_global_init()` / `konclude_global_shutdown()` in the
  facade to own whichever answer this turns out to be — either constructing
  a dummy `QCoreApplication` once per process, or running a dedicated
  background thread that pumps the Qt event loop if one is genuinely
  required.

### 4. Bake the threading workaround into the facade

- `konclude_create_reasoner` should hard-enforce `-w N` with `N >= 2`
  internally — never expose a caller-settable thread count that could
  reintroduce the single-thread startup deadlock.
- Test specifically for lifecycle patterns Konclude has never been exercised
  under before: repeated create → use → destroy **within one process**
  (the CLI/server code paths have only ever been "start once, run once,
  exit").

### 5. Symbol hygiene for the shared library

- Compile with hidden visibility by default (`-fvisibility=hidden`) and
  explicitly export only the `extern "C"` facade functions. This prevents
  Konclude's ~2500 internal symbols (and any Qt symbols) from leaking into
  FD's link namespace and colliding if FD links its own Qt or other
  overlapping libraries.
- Ship exactly one public header (e.g. `konclude_embedded.h`) containing
  only the facade declarations — no Qt headers, no internal `Source/`
  headers reachable from it.

### 6. FD-side integration shim

- FD links against `libKonclude.so` / `.dylib` and includes only the plain C
  header — no Qt dependency is introduced into FD's own build.
- Wrap the raw C handle in a small RAII C++ class on FD's side (constructor
  calls `konclude_create_reasoner`, destructor calls
  `konclude_destroy_reasoner`) so FD code never manages the handle manually.
- Sort out runtime library discovery (`rpath`, `DYLD_LIBRARY_PATH` /
  `LD_LIBRARY_PATH`, or co-locating the `.so`/`.dylib` next to FD's binary)
  deliberately, rather than discovering it at deploy time.

### 7. Correctness gate before touching FD's search loop

- Write a standalone smoke-test harness (independent of FD) that loads an
  ontology through the new facade, asserts/retracts some ABox facts, runs
  satisfiability/consistency checks, and diffs the results against the
  already-trusted CLI output for the same operations on the same ontology
  (e.g. `Tests/roberts-family-full-D.owl.xml`). This is the regression check
  that the new embedded path is *correct*, not just fast.
- Stress-test it: thousands of mutate+query cycles in a tight loop, watching
  for crashes and for memory growth. Konclude's pooled allocators
  (`Source/Context/`) have never been exercised under "reuse across many
  operations inside one long-lived instance" before — this usage pattern is
  new for the codebase, not something the existing CLI/server paths cover.

### 8. Wire into FD for real

- Only after phase 7 is clean, plug the RAII wrapper into FD's actual
  precondition/effect evaluation code.
- Add a caching/memoization layer at this boundary (keyed on relevant
  ABox delta + query), keyed at the FD side. Eliminating IPC overhead does
  not eliminate the cost of the tableau reasoning itself, and planning
  search revisits overlapping states heavily enough that this is likely a
  bigger lever than the embedding work itself.

## Open risks to track

- Phase 3's Qt event-loop question is unresolved and blocks the design of
  `konclude_global_init()`.
- In-process linking removes crash/hang isolation between FD and Konclude —
  any future tableau-kernel bug (of which the `-w 1` deadlock is one known
  example) now takes down the whole planner process.
- Statically-linked LGPLv3 obligations don't apply here (shared linking was
  chosen), but confirm the shared library is genuinely loaded dynamically at
  runtime (not folded in by a `--whole-archive`-style static step) if
  license compliance is ever audited.
