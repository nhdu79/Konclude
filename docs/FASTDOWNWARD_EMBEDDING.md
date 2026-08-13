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

**Status: Phase 1 done. Phase 2 in progress (facade skeleton built and linking; incremental axiom assert/retract still missing; CQ answering designed via Rasqal-free SPARQL parsing, not yet implemented).**

### 1. Prove "builds as a library, no `main()`" first, with zero new API code — ✅ done

- Confirmed by reading the source: `KoncludeLIB.pro` builds the exact same
  ~2500 files as `Konclude.pro`/`KoncludeWithoutRedland.pro` (all three
  `include(Konclude.pri)`) — it's not a separate source tree. It just sets
  `TEMPLATE = lib` instead of an implicit app, and adds
  `DEFINES += KONCLUDE_COMPILE_JNI_INTERFACE`.
- That one define is the whole mechanism for excluding `main()`:
  `Source/mainLoader.cpp` wraps its entire body, including `int main(...)`,
  in `#ifndef KONCLUDE_COMPILE_JNI_INTERFACE`. When the flag is set, the
  file compiles to nothing. No separate lib-only source tree, no build-time
  file exclusion in the `.pro`/`.pri` — just a preprocessor guard on the one
  file that defines `main()`. Our new flag should follow this exact pattern.
- `Source/Control/Interface/JNI/` is the mirror image: every file in it is
  wrapped in `#ifdef KONCLUDE_COMPILE_JNI_INTERFACE`, so it compiles in only
  for the JNI/lib build, never for the CLI build.
- Add a new qmake build flag parallel to `KONCLUDE_COMPILE_JNI_INTERFACE`
  (e.g. `KONCLUDE_COMPILE_EMBEDDED_INTERFACE`) that builds everything except
  `mainLoader.cpp` as a shared library (`-shared`), with **no new API yet**.
  Just confirm it links cleanly into `libKonclude.so` / `libKonclude.dylib`.
- This isolates "build system + two-`main()`-functions problem" risk before
  any interface design work begins.

**What was actually done:**

- Widened the guard in `Source/mainLoader.cpp` from
  `#ifndef KONCLUDE_COMPILE_JNI_INTERFACE` to
  `#if !defined(KONCLUDE_COMPILE_JNI_INTERFACE) && !defined(KONCLUDE_COMPILE_EMBEDDED_INTERFACE)`
  (and updated the matching `#endif` comment). This is the only source
  change — everything in `Source/Control/Interface/JNI/` is left untouched;
  since `KONCLUDE_COMPILE_JNI_INTERFACE` is never defined for this build
  variant, those files' `#ifdef`-guarded bodies compile to empty
  translation units, so no `jni.h`/`JAVA_HOME` dependency is pulled in.
- Added `KoncludeEmbedded.pro` at the repo root, modeled on `KoncludeLIB.pro`
  minus the JNI/Java-specific lines (no `JAVAHOME`, no `$$JAVAHOME/include`).
  Key settings: `TEMPLATE = lib`, `CONFIG += shared`,
  `DEFINES += ... KONCLUDE_COMPILE_EMBEDDED_INTERFACE`,
  `DESTDIR = ./ReleaseEmbedded`, `OBJECTS_DIR += releaseembedded`. It
  `include(Konclude.pri)`s the same shared file list as every other `.pro`
  variant — no new source files yet.
- Built and verified on macOS (Qt 5.15.19 arm64, Homebrew):
  ```sh
  /opt/homebrew/opt/qt@5/bin/qmake -o MakefileEmbedded KoncludeEmbedded.pro
  make -f MakefileEmbedded -j$(sysctl -n hw.ncpu)
  ```
  Produced `ReleaseEmbedded/libKonclude.1.0.0.dylib` (~19.8 MB), confirmed via:
  - `file`: `Mach-O 64-bit dynamically linked shared library arm64` — a
    real shared library, not an executable.
  - `nm -g ... | grep -w "_main"`: no match — confirms the widened guard
    correctly excludes the CLI entry point.

**Issues found, not yet fixed:**

- `DESTDIR = ./ReleaseEmbedded` and `OBJECTS_DIR += releaseembedded` collide
  on macOS's default case-insensitive filesystem — both resolved to the same
  directory, so the `.dylib` and all ~2500 `.o` files ended up mixed
  together. Harmless for this smoke test; should be given genuinely
  distinct names (not just distinct case) before this is a build target
  anyone else relies on.
- `otool -D` shows the install name is just `libKonclude.1.dylib` (no path)
  — FD will need an `rpath` or the library copied next to its binary to
  find it at runtime. Expected, and already tracked as the phase 6 "runtime
  library discovery" item — noted here as confirmed, not newly discovered.

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
- The JNI interface already has working prior art for most of this — worth
  reading before designing from scratch, not just for inspiration:
  - `CJNIInstanceManager` is exactly the per-instance state object our
    opaque `KoncludeReasonerHandle` should wrap.
  - `CJNIAxiomExpressionVisitingLoader` builds axioms via a visitor/builder
    pattern directly against the internal model — proof that "mutate the
    ontology without going through the OWL2-XML parser" already exists and
    works, rather than something to invent.
  - `CJNIOntologyRevisionData` wraps `COntologyRevision`
    (`Reasoner/Revision/`) for incremental changes — the precedent for
    `konclude_assert_axiom`/`konclude_retract_axiom`.
  - `CConfigJNIReader` reads configuration from an in-memory string instead
    of a `-c FILEPATH` file — the precedent for configuring
    `konclude_create_reasoner` without requiring a config file on disk.

**What was actually done:**

- Added `Source/Control/Interface/Embedded/` with four files, wired into
  `Konclude.pri` (`HEADERS` and `SOURCES`) the same way the JNI files are:
  - `konclude_embedded.h` — the public `extern "C"` facade. Opaque
    `typedef void* KoncludeReasonerHandle`, no Qt/C++ types in any signature.
  - `CEmbeddedReasoner.h`/`.cpp` — wraps a per-instance `CCommanderManager`
    (via `CCommanderManagerThread`), guarded by `KONCLUDE_COMPILE_EMBEDDED_INTERFACE`
    exactly like the JNI files are guarded by their own flag.
  - `CEmbeddedInterfaceCAPI.cpp` — the thin `extern "C"` shim that downcasts
    the opaque handle and forwards to `CEmbeddedReasoner`.
- Implemented and working: `konclude_create_reasoner`,
  `konclude_destroy_reasoner`, `konclude_load_ontology_file`,
  `konclude_check_consistency`, `konclude_check_satisfiability`,
  `konclude_last_error`.
- Phase 3 (QCoreApplication-once, never `exec()`) and Phase 4 (`-w N>=2`
  hard-enforced) were folded directly into `CEmbeddedReasoner`'s constructor
  rather than deferred — see that file for the detail on *how* each is
  applied (a per-`CConfiguration` override for the processor count, mirroring
  the CLI's `-w N` flag exactly, since the naive approach of re-registering
  the config description silently created a second, unread one and
  reproduced the deadlock it was meant to prevent).
- Building `KoncludeEmbedded.pro` compiles this cleanly with zero errors and
  links into `libKonclude.dylib`. Verified via `nm -g`: all six functions
  above are present as exported symbols (`_konclude_create_reasoner`,
  `_konclude_check_satisfiability`, etc.) — the facade is real, not just
  compiling in isolation.

**Left to do for phase 2:**

- **`konclude_assert_axiom` / `konclude_retract_axiom` are not implemented
  yet.** This is the actual point of the exercise: FD cannot reload the
  whole ontology file per search state at millions-of-calls scale, so
  `konclude_load_ontology_file` alone (one-shot, once per instance) isn't
  sufficient for the real integration. The precedent this needs
  (`CJNIAxiomExpressionVisitingLoader` + `CJNIOntologyRevisionData` /
  `COntologyRevision`) is referenced in code comments but not yet used.
- No config-string input path exists — `konclude_create_reasoner` takes no
  parameters, so there's no analogue to `CConfigJNIReader`'s in-memory
  config yet. Lower priority than assert/retract; may be fine to defer past
  phase 2.
- No smoke test yet confirming the facade works end-to-end (load →
  consistency/satisfiability → answer matches trusted CLI output) — only
  that it compiles and the symbols link. An untracked `roberts-class.owl.xml`
  at the repo root looks like a manual test fixture from exploring this, but
  there's no test harness or driver code yet. Formal correctness gating is
  phase 7's job, but a minimal manual sanity check before adding more
  surface area (assert/retract) would de-risk building on an unverified
  foundation.

**Conjunctive query (CQ) answering — design findings (not yet implemented):**

- **JNI is not usable prior art here.** `CJNIQueryProcessor` only exposes
  fixed-shape, single-atom queries (`queryOntologyInstances`,
  `queryOntologySubClasses`, `queryOntologyTypes`, etc.), each built from a
  single class/individual/property name string. None of them construct a
  `CComplexAnsweringQuery` or touch the composition-tree join/propagation
  machinery that real multi-atom CQ answering uses. `CInstancesQuery` /
  `CIsInstanceOfQuery` (`Reasoner/Query/`) are similarly single-atom only —
  neither can express a 2-3 atom pattern like
  `Person(?x), hasParent(?x,?y), Doctor(?y)`.
- **Initial design assumption was wrong — corrected by follow-up research.**
  The first pass assumed CQs would need to be built atom-by-atom through a
  bespoke C API (`konclude_query_add_class_atom`/`add_property_atom`/...),
  reasoning that real SPARQL-text parsing requires Rasqal, which the
  embedded build deliberately excludes. That's false:
  `CSPARQLSimpleQueryParser` (`Parser/CSPARQLSimpleQueryParser.h/.cpp`)
  parses genuine SPARQL `SELECT` syntax with **zero Rasqal/Redland
  dependency** (no `KONCLUDE_REDLAND_INTEGRATION` guard anywhere in it), and
  it's already compiled unconditionally into every `.pro` variant via
  `Konclude.pri:413,3056` — including the current `KoncludeEmbedded.pro`
  build, with nothing new to add. So the embedded facade can accept a real
  SPARQL `SELECT` string directly, rather than needing a hand-rolled
  atom-building C API.
- **The non-Rasqal SPARQL execution sequence**, confirmed from a real
  non-Rasqal call site in `Control/Interface/OWLlink/COWLlinkProcessor.cpp:890-917`:
  ```cpp
  CConcreteOntologyUpdateSeparateHashingCollectorBuilder* builder =
      new CConcreteOntologyUpdateSeparateHashingCollectorBuilder(onto);
  CConcreteOntologyQueryExtendedBuilder* queryBuilderGen =
      new CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, ontConfig, builder);
  CSPARQLQueryParser* sparqlQueryParser =
      new CSPARQLSimpleQueryParser(queryBuilderGen, builder, onto);
  builder->initializeBuilding();
  sparqlQueryParser->parseQueryTextList(queryStringList);   // real SPARQL SELECT text
  builder->completeBuilding();
  queryList = queryBuilderGen->generateQuerys();            // QList<CQuery*>, ready for CCalculateQueryCommand
  ```
  From there, dispatch is identical to every other query already in
  `CEmbeddedReasoner`: wrap the resulting `CQuery*` in `CCalculateQueryCommand`
  (the same single command class used for satisfiability/consistency — there
  is no separate CQ-specific command; dispatch is polymorphic on the
  `CQuery*`'s own type), delegate through `mPreconditionSynchronizer`, block
  on `CCommandExecutedBlocker::waitExecutedCommand`.
- **`expressionOntology` is not a fresh/empty ontology — it's the knowledge
  base's own current `COntologyRevision`.** Confirmed at both non-Rasqal call
  sites (`COWLlinkProcessor.cpp:876-877,2162-2170`):
  `onto = ontRev->getOntology()` (the current revision) is passed as
  `expressionOntology`, and `ontRev->getPreviousOntologyRevision()->getOntology()`
  (the prior revision) is passed as `baseOntology`. No separate/empty
  ontology object is constructed anywhere in the codebase for this purpose —
  query-local declarations (like SPARQL variables) get registered directly
  onto the live ontology's current revision via the
  `CConcreteOntologyUpdateSeparateHashingCollectorBuilder` transaction
  (`initializeBuilding()` → parse → `completeBuilding()`), not onto a
  disposable side object. `CExpressionVariable` itself
  (`Parser/Expressions/CExpressionVariable.h:57`) is a trivial name-only leaf
  type with no separate registration call.
- **Open risk carried forward, not yet resolved:** because query parsing
  mutates the live ontology's revision chain (to register query-local
  variable declarations) rather than a disposable object, executing a CQ is
  not obviously side-effect-free on `CEmbeddedReasoner`'s knowledge base
  state. At FD's millions-of-calls scale, if each query execution leaves
  behind a new `COntologyRevision`, that's a per-call revision-chain growth
  problem analogous to (and possibly compounding with) the assert/retract
  axiom item above. Needs verification before this is built: does
  `completeBuilding()`/query execution clean up query-local declarations
  afterward, or does the revision chain grow unboundedly per query?
- **Result extraction API** (confirmed, in `Reasoner/Query/`): three-level
  pull-iterator structure — `CVariableBindingsAnswersResult` (result set:
  `getVariableNames()`, `getResultCount()`,
  `getVariableBindingsAnswersIterator()`) → `CVariableBindingsAnswersResultIterator`
  (row-by-row) → `CVariableBindingsResultIterator` (cell-by-cell within a
  row) → `CVariableBindingStringResult::getBindingString()` (each bound
  value as a `QString`, convertible to `const char*` the same way
  `CEmbeddedReasoner::getLastErrorCStr()` already handles `mLastError`).
- **Proposed (not yet implemented) C API shape**, given the corrected design:
  ```c
  typedef void* KoncludeQueryResultHandle;

  KoncludeQueryResultHandle konclude_execute_sparql_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery);
  int konclude_query_result_row_count(KoncludeQueryResultHandle r);
  int konclude_query_result_variable_count(KoncludeQueryResultHandle r);
  const char* konclude_query_result_variable_name(KoncludeQueryResultHandle r, int varIndex);
  const char* konclude_query_result_binding(KoncludeQueryResultHandle r, int row, int varIndex);
  void konclude_query_result_destroy(KoncludeQueryResultHandle r);
  ```
  Much smaller surface than the originally-proposed atom-by-atom API — one
  execute call plus a result accessor set. `generateQuerys()` returns a
  `QList<CQuery*>` (plural) for a batch of query texts; the single-string
  `konclude_execute_sparql_query` should assume/assert a one-query-in,
  one-query-out relationship and treat more-or-fewer than one as an error,
  rather than silently taking `.first()`.

### 3. `QCoreApplication` / event-loop ownership — resolved by the JNI precedent

- **Resolved**, not an open question anymore: `com_konclude_jnibridge_KoncludeReasonerBridge.cpp`
  (`Java_..._initKoncludeLibraryInstance`) already shows the answer.

  ```cpp
  if (QCoreApplication::instance() == nullptr) {
      qtApp = new QCoreApplication(qtAppargc, qtAppargv);
      qtAppCreated = true;
      //qtApp->exec();          // <- explicitly commented out
  }
  ...
  jniCommandLoader->load();
  jniCommandLoader->waitSynchronization();
  ```

  A `QCoreApplication` instance is constructed once (required for Qt
  internals to function at all) but **`exec()` is never called** — no event
  loop is pumped, ever. Instead, `waitSynchronization()` blocks the calling
  thread using Konclude's own `Concurrent`/`Scheduler` synchronization
  primitives directly (the same semaphore/wait-condition family behind the
  `-w 1` startup deadlock), not Qt's event loop dispatch.
- Implication for `konclude_global_init()`: construct a static
  `QCoreApplication` once per process (guarded the same way, checking
  `QCoreApplication::instance() == nullptr`) and never call `exec()`. No
  dedicated background thread pumping a Qt event loop is needed.
- Still worth a smoke test in phase 1/7: confirm nothing in the reasoning
  path Konclude actually uses (as opposed to what JNI happens to exercise)
  secretly depends on queued signal/slot delivery, which requires a running
  event loop to fire. The JNI precedent is strong evidence it doesn't, not
  a guarantee for every code path.

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

- In-process linking removes crash/hang isolation between FD and Konclude —
  any future tableau-kernel bug (of which the `-w 1` deadlock is one known
  example) now takes down the whole planner process.
- Statically-linked LGPLv3 obligations don't apply here (shared linking was
  chosen), but confirm the shared library is genuinely loaded dynamically at
  runtime (not folded in by a `--whole-archive`-style static step) if
  license compliance is ever audited.
