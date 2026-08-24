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

**Status: Phase 1 done, confirmed on macOS and Linux. Phase 2's facade is
now working end-to-end, not just compiling/linking** — the dispatch bug
that used to block every command past KB creation is fixed (see "Update:
the dispatch bug is fixed" under phase 2 below), and CQ answering has
shipped: `create` → `load_ontology_file` → `check_consistency` /
`check_satisfiability` / `execute_conjunctive_query` all complete and
return correct results, verified against trusted CLI output (314/314 rows
on `Tests/roberts-family-full-D.owl.xml` via
`Tools/EmbeddedDriver/embedded_cq_driver.cpp` — see
`docs/EMBEDDED_CQ_DRIVER.md`). The shipped CQ API is the plain
SPARQL-string shape (`konclude_execute_conjunctive_query` +
`konclude_query_result_*` reading straight off the reasoner handle), **not**
either of the two API shapes phase 2 originally proposed below — see the
status notes on those sections for what changed and why. The
snapshot-per-state design is the shipped path for FD's actual per-state ABox
updates — `konclude_state_begin`/`konclude_state_assert_class_fact` are
built and verified — but **not** as a never-installed scratch revision the
way this doc originally proposed: querying turned out to require installing
the state's revision once, lazily, on its first query (not once per CQ
call). See `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2 for
the current mechanism and why the never-install version doesn't work.
**Next steps and the millions-of-calls performance work are
now tracked in `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`, not here.**

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
- Also built and verified on **Linux** (Ubuntu 24.04, Qt 5.15.13 via apt
  `qtbase5-dev`/`qt5-qmake`, g++ 13.3.0):
  ```sh
  qmake -o MakefileEmbedded KoncludeEmbedded.pro
  make -f MakefileEmbedded -j$(nproc)
  ```
  Produced `ReleaseEmbedded/libKonclude.so.1.0.0` (~27 MB). Confirmed via
  `file` (ELF 64-bit shared object, not an executable), `nm -D ... | grep -w
  "T main"` (no match), and `nm -D ... | grep konclude_` (all six facade
  functions present as exported symbols). Same result as the macOS build —
  the embedded target is not platform-specific so far.
- Generated `compile_commands.json` for this build variant (qmake itself
  doesn't emit one): `compiledb -n -o compile_commands.json make -f
  MakefileEmbedded` (via the `compiledb` PyPI package, installed into a
  throwaway venv to avoid the system's PEP-668-managed Python). Verified
  with `clangd --check=Source/Control/Interface/Embedded/CEmbeddedReasoner.cpp`
  — parses with 0 errors. Worth regenerating after adding new files to this
  target (assert/retract, query builder, etc.) so editor tooling stays in
  sync.

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

- Added `Source/Control/Interface/Embedded/` with five files (a fourth,
  `CEmbeddedOWLlinkProcessor.h`/`.cpp`, was added later — see the dispatch
  bug fix below), wired into `Konclude.pri` (`HEADERS` and `SOURCES`) the
  same way the JNI files are:
  - `konclude_embedded.h` — the public `extern "C"` facade. Opaque
    `typedef void* KoncludeReasonerHandle`, no Qt/C++ types in any signature.
  - `CEmbeddedReasoner.h`/`.cpp` — wraps a per-instance `CCommanderManager`
    (via `CCommanderManagerThread`), guarded by `KONCLUDE_COMPILE_EMBEDDED_INTERFACE`
    exactly like the JNI files are guarded by their own flag.
  - `CEmbeddedInterfaceCAPI.cpp` — the thin `extern "C"` shim that downcasts
    the opaque handle and forwards to `CEmbeddedReasoner`.
- **Current actual exported function set** (superset of the original six —
  reflects the real `konclude_embedded.h` as of now, not the initial
  snapshot; see that header for full doc comments on each):
  ```c
  KoncludeReasonerHandle konclude_create_reasoner(void);
  void konclude_destroy_reasoner(KoncludeReasonerHandle handle);
  int konclude_load_ontology_file(KoncludeReasonerHandle handle, const char* filePath);
  int konclude_check_consistency(KoncludeReasonerHandle handle, int* outConsistent);
  int konclude_check_satisfiability(KoncludeReasonerHandle handle, const char* classIRI, int* outSatisfiable);
  int konclude_state_begin(KoncludeReasonerHandle handle);
  int konclude_state_assert_class_fact(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI);
  int konclude_execute_conjunctive_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery);
  int konclude_query_result_row_count(KoncludeReasonerHandle handle);
  int konclude_query_result_variable_count(KoncludeReasonerHandle handle);
  const char* konclude_query_result_variable_name(KoncludeReasonerHandle handle, int varIndex);
  const char* konclude_query_result_binding(KoncludeReasonerHandle handle, int row, int varIndex);
  const char* konclude_last_error(KoncludeReasonerHandle handle);
  ```
  Note the shape this actually took: there is **no separate query-result
  handle** — `konclude_query_result_*` all take the same
  `KoncludeReasonerHandle` and read state cached on `CEmbeddedReasoner`
  from the most recent `konclude_execute_conjunctive_query` call, valid
  until the next call on that handle. `konclude_execute_conjunctive_query`
  now requires `konclude_state_begin` to have been called first — it
  queries the current FD state's revision (see
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2), not a
  revision it mints for itself. This is simpler than either API
  shape proposed later in this doc (`KoncludeQueryResultHandle` with an
  explicit destroy call, or the atom-by-atom `KoncludeQueryPatternHandle`
  design) — see the status notes on those sections. There is also a
  twelfth function, `konclude_probe_scratch_revision_cycles`, added as a
  diagnostic for `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision
  4 — explicitly not part of the stable API surface, not listed above.
- Phase 3 (QCoreApplication-once, never `exec()`) and Phase 4 (`-w N>=2`
  hard-enforced) were folded directly into `CEmbeddedReasoner`'s constructor
  rather than deferred — see that file for the detail on *how* each is
  applied (a per-`CConfiguration` override for the processor count, mirroring
  the CLI's `-w N` flag exactly, since the naive approach of re-registering
  the config description silently created a second, unread one and
  reproduced the deadlock it was meant to prevent).
- Building `KoncludeEmbedded.pro` compiles this cleanly with zero errors and
  links into `libKonclude.so`/`.dylib`. Verified via `nm -D`/`nm -g`: every
  function above is present as an exported symbol — the facade is real,
  not just compiling in isolation, and (per the dispatch-bug fix and the CQ
  driver below) actually completes and returns correct results, not just
  links.

**Left to do for phase 2 (updated for actual current state):**

- **`konclude_assert_axiom` / `konclude_retract_axiom` — superseded, not
  merely undone.** The design revision immediately below ("Design
  revision: snapshot-per-state ABox") replaced persistent incremental
  assert/retract with per-state scratch revisions for FD's actual (tree-
  shaped search) use case, before this pair was ever built. They're not
  on the critical path anymore; still worth keeping in mind as a possible
  secondary API for embedding consumers that want one long-lived,
  persistently-mutated KB, but nothing currently plans to build them.
- No config-string input path exists — `konclude_create_reasoner` takes no
  parameters, so there's no analogue to `CConfigJNIReader`'s in-memory
  config yet. Still true, still low priority, still not blocking anything.
- **Done.** The smoke test this bullet asked for exists and passes:
  `Tools/EmbeddedDriver/embedded_cq_driver.cpp` loads
  `Tests/roberts-family-full-D.owl.xml` through the real facade, runs a CQ,
  and diffs all 314 result rows against trusted output
  (`cq-answers.xml`) — full PASS, reproducibly (see
  `docs/EMBEDDED_CQ_DRIVER.md`). `konclude_check_consistency`/
  `konclude_check_satisfiability`/`konclude_load_ontology_file` are also
  now confirmed working (see the dispatch-bug fix below) — this was not
  true when this bullet was originally written, since none of these
  commands could complete at all at the time.

**Design revision: snapshot-per-state ABox instead of persistent incremental
assert/retract (for the FD use case):**

- **Why the original assert/retract plan doesn't fit.** FD's search states
  form a **tree** (branch, then backtrack up several levels), not a linear
  sequence. A single long-lived ontology revision mutated by
  `konclude_assert_axiom`/`konclude_retract_axiom` calls would force the
  facade (or FD) to mirror the search tree's backtracking with matching
  retracts to keep the ABox in sync — exactly the bookkeeping burden this
  revision avoids. The better fit for "many tree-shaped states, each
  independently queried" is: keep one **persistent base ontology** (TBox +
  RBox, loaded and classified exactly once), and for each FD state, layer a
  **fresh, throwaway ABox snapshot** representing just that state's facts on
  top of it, run that state's CQs, then discard the snapshot — no retract
  bookkeeping needed because nothing persists across states.
- **A ready-made primitive for this already exists:**
  `CSPOntologyRevisionManager::createNewOntologyRevisionFromBasementOntology()`
  (`Source/Reasoner/Revision/CSPOntologyRevisionManager.cpp:97-133`) builds a
  fresh `CConcreteOntology` directly against a persistent, classified-once
  base ontology (`getBasementOntology()`, same file:73-95) — not by walking
  forward through a delta chain. It calls `referenceOntology()` →
  `referenceDataBoxes()` → `CABox::referenceABox`
  (`Source/Reasoner/Ontology/COntologyDataBoxes.cpp:60-73`,
  `Source/Reasoner/Ontology/CABox.cpp:80-91`), which is a lightweight
  reference/copy of the individual vector, not a TBox rebuild. This is
  effectively "TBox/RBox classified once and shared, ABox rebuilt per call."
- **Status: implemented and corrected — installing turned out to be
  required, not avoidable.** The per-state primitive is
  `createNewOntologyRevision()` (`CSPOntologyRevisionManager.cpp:457-520`),
  reached via `CCreateKnowledgeBaseRevisionUpdateCommand` — it builds a new
  `CConcreteOntology(currOnt, nextOntConfig)` referencing the currently
  installed revision (O(1) reference-sharing constructor). This doc
  originally claimed a state's revision could be Told and queried without
  ever installing it, leaving `onRevContainer` untouched entirely. That
  turned out to be wrong: nothing in this codebase queries a Told,
  never-installed revision — every real Tell path
  (`COWLlinkProcessor.cpp`'s SPARQL `UPDATE`/`INSERT DATA` handling) installs
  immediately after Telling, before any query touches the result, and
  forcing the materialization step (`OPSBUILD`) directly on a
  never-installed revision was tried and empirically returned zero query
  rows, including for content that predated the Tell. The shipped mechanism
  installs each state's revision lazily, once, on its first query — see
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2 for the full
  mechanism and why. `onRevContainer` does grow by one entry per FD state as
  a result (not per CQ call within a state) — a real, bounded cost this doc
  originally claimed didn't exist.
- **Resolved via source reading, empirical timing still blocked (see "What
  was verified" below):** `CTBox::referenceTBox`/`CRBox::referenceRBox` do
  share by pointer, not copy. `CConcreteOntology`'s reference constructor
  (`Source/Reasoner/Ontology/CConcreteOntology.cpp:68-103`) calls
  `referenceOntology()` (line 131-141), which calls
  `mDataBoxes->referenceDataBoxes()` (`COntologyDataBoxes.cpp:60-63`), which
  calls `CTBox::referenceTBox`/`CRBox::referenceRBox`/`CABox::referenceABox`
  directly. `CTBox::referenceTBox` (`CTBox.cpp:321-334`) does
  `concepts->referenceVector(tBox->concepts)` — and
  `CDynamicReferenceVectorBase::referenceVector`
  (`Source/Utilities/Container/CDynamicReferenceVector.cpp:147-160`) is
  O(1): `clear(); mReferenceVector = refVector; ... mRootBucket->initBucket(...)`
  — a pointer-aliasing wrapper around the *existing* bucket structure, not a
  per-element copy. This applies uniformly to TBox/RBox/ABox — confirms the
  "TBox/RBox classified once and shared, ABox rebuilt per call" model is
  structurally correct, independent of ontology size. `CConcreteOntology`'s
  constructor does still allocate a fixed, small number of new C++ objects
  per instance regardless (`COntologyTriplesData`, `CClassification`,
  `CPrecomputation`, `CPreprocessing`, `CRealization`,
  `CConcreteOntologyContextBase`, plus several `CObjectParameterizingAllocator`
  boxes) — O(1) per scratch ontology, not O(0), but not O(TBox size) either.
- **This supersedes `konclude_assert_axiom`/`konclude_retract_axiom` as the
  primary path for FD**, though that pair may still be worth keeping as a
  secondary API for embedding consumers that genuinely want persistent
  incremental mutation of one long-lived KB — it's just not the right fit
  for FD's tree-shaped per-state querying pattern.
- **Actual shipped API shape** (simpler than what was originally sketched
  here — no separate state handle; state lives on the reasoner handle
  itself, one active state at a time, matching FD's single-threaded search
  loop):
  ```c
  int konclude_state_begin(KoncludeReasonerHandle handle);
  int konclude_state_assert_class_fact(KoncludeReasonerHandle handle, const char* individualIRI, const char* classIRI);
  ```
  Object-property facts (`konclude_state_assert_object_property_fact` or
  similar) are the same mechanism, not yet built. There is no explicit
  `konclude_state_end` — the next `konclude_state_begin` call discards the
  previous state, and `konclude_destroy_reasoner` discards whatever state
  is current when the handle itself goes away.

**Update: the dispatch bug described below is fixed.** Root cause: the
raw `CPreconditionSynchronizer(mReasonerCommander)` pattern was missing a
`COWLlinkProcessor` in the dispatch path — `COWLlinkProcessor::processCustomsEvents`
turned out to be the *only* registered handler for several of the hanging
command types. Fix: added `CEmbeddedOWLlinkProcessor` (a minimal
`COWLlinkProcessor` subclass modeled on the JNI interface's
`CJNICommandProcessor`) and routed `loadOntologyFile()`/`checkConsistency()`/
`checkSatisfiability()` through it instead of `mPreconditionSynchronizer`
directly (see `docs/EMBEDDED_CQ_DRIVER.md`'s "Fixed bugs found via this
driver" section for the full writeup). Confirmed fixed two ways: (1) the
full CQ pipeline (`executeConjunctiveQuery()`, still dispatched through
`mPreconditionSynchronizer` directly, not through the new processor) now
completes and returns correct results end-to-end; (2) a dedicated probe,
`CEmbeddedReasoner::probeScratchRevisionCycles()` /
`konclude_probe_scratch_revision_cycles`, specifically re-tested the
never-install scratch-revision pattern this benchmark pass below was
trying to measure — 10/10 and 30/30 cycles completed cleanly under a
shell-level timeout, no hang (see
`docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 4). The
narrative below is left as-is for the historical record of how the bug was
found; treat every "blocked"/"not yet obtained" framing in it as
superseded by this note — empirical timing is what `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`
is now tracking as the next actual step.

**What was verified (standalone benchmark pass) — partial: source-level claims
confirmed, empirical timing blocked by a newly-discovered dispatch bug:**

- Added `Source/Control/Loader/CEmbeddedScratchOntologyBenchmarkLoader.h/.cpp`
  (wired into `Konclude.pri` and `CDefaultLoaderFactory`, following the
  existing internal-benchmark-loader convention CLAUDE.md already documents),
  intended to time N iterations of `CCreateKnowledgeBaseRevisionUpdateCommand`
  (build, never install) against a loaded/classified base ontology, per the
  open item above. Built cleanly against `KoncludeWithoutRedland.pro`.
- The TBox/RBox O(1) reference-sharing claim above **is confirmed by direct
  source reading** (constructor bodies, not just method signatures), so it
  stands as verified independent of runtime timing.
- **A real, previously-undiscovered bug blocked the runtime timing/leak
  measurement itself.** Any command dispatched via `CPreconditionSynchronizer`
  constructed directly from a raw `CCommanderManager`/`CCommanderManagerThread`
  — exactly the pattern `CEmbeddedReasoner` uses for `mPreconditionSynchronizer`
  (`Source/Control/Interface/Embedded/CEmbeddedReasoner.cpp:168`) — succeeds
  for the *first* command (`CCreateKnowledgeBaseCommand`) but **hangs
  indefinitely on every command after it** (`CIsConsistentQueryCommand`,
  `CLoadKnowledgeBaseOWLAutoOntologyCommand`, and
  `CCreateKnowledgeBaseRevisionUpdateCommand` were all reproduced hanging).
  Reproduced two independent ways: a hand-rolled setup mirroring
  `CEmbeddedReasoner`'s constructor verbatim, and a setup chained after
  `-DefaultReasonerLoader` reading the manager back via
  `CConfigManagerReader::readCommanderManagerConfig()` (the same technique
  `CCLIBatchProcessingLoader::init()` uses, `CCLIBatchProcessingLoader.cpp:54-56`).
  The already-documented `-w 1` startup deadlock was independently ruled out
  as the cause each time (confirmed via log output reaching "Reasoner
  initialized with 2 processing unit(s)." before the hang).
  **Contrast:** the identical command types (`CIsConsistentQueryCommand`
  et al.) complete correctly, fast, every time, via the CLI's actual proven
  path (`./Konclude consistency -w 2 -i ...`), which dispatches through
  `COWLlinkProcessor::delegateCommand()`
  (`Source/Control/Interface/OWLlink/COWLlinkProcessor.cpp:69-72`:
  `postEvent(new CRealizeCommandEvent(command)); return this;`) rather than
  forwarding straight to the manager. `CPreconditionSynchronizer` itself is a
  self-contained `CThread` with its own event queue and precondition-retry
  callback wiring (`Source/Control/Command/CPreconditionSynchronizer.cpp:31-44`)
  — reading it did not reveal an obvious dependency on `COWLlinkProcessor`
  specifically, so the exact mechanism by which wrapping a
  `COWLlinkProcessor` (vs. the raw manager) changes outcome was **not
  root-caused** in the time available; this needs a debugger (unavailable in
  this environment — no `gdb`) or further tracing, not more guessing.
- **Practical implication, worth having found now rather than during FD
  integration:** `CEmbeddedReasoner`'s `checkConsistency()`/
  `checkSatisfiability()`/`loadOntologyFile()` — all built on exactly the
  `CPreconditionSynchronizer(mReasonerCommander)` pattern shown to hang here
  — have almost certainly never actually completed when run, despite
  compiling and linking cleanly (which is all phase 2's "what was actually
  done" verified). This upgrades the existing "No smoke test yet confirming
  the facade works end-to-end" gap (noted under "Left to do for phase 2"
  above) from "untested" to "likely broken, reproducibly, and now understood
  well enough to point at the fix location" — the embedded facade's
  precondition-synchronizer wiring needs to be changed to route through
  something with `COWLlinkProcessor`-equivalent dispatch behavior (or the
  actual mechanism needs to be found and ported) before any further facade
  work is built on top of it.
- **Pragmatic workaround used to still get *some* signal:** the benchmark
  loader's timing loop was pivoted to skip ontology-file loading and the
  warmup consistency check entirely and would run directly against the
  trivial empty "ontology basement" `CCreateKnowledgeBaseCommand` already
  reliably produces (confirmed working in every run) — since the reference-
  sharing mechanism being measured doesn't depend on TBox content size. This
  still did not fully unblock the loop, since even the warmup consistency
  check on the empty basement hits the same hang. **No iteration-count
  timing or RSS-growth numbers were obtained.** The loader is left in the
  repo in this known-blocked state (see the `\note` in its header) as a
  reproduction case, not deleted — re-run with:
  ```sh
  qmake -o Makefile KoncludeWithoutRedland.pro && make -j$(nproc)
  ./Release/Konclude -DefaultReasonerLoader "+Konclude.Calculation.ProcessorCount=2" -EmbeddedScratchOntologyBenchmarkLoader
  ```
- **Net effect on confidence (updated):** the snapshot-per-state design's
  TBox/RBox O(1) reference-sharing is on solid source-level footing, and the
  dispatch bug that blocked turning it into working facade code is now
  fixed. The design has since shipped and been corrected on one point: ABox
  revisions do get installed (once per state, lazily) — see
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2. What's still
  open is empirical *timing* under that corrected design — that's the actual
  next step, tracked in `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`.

**Conjunctive query (CQ) answering — design findings, then actually
implemented, but not as either shape proposed below:**

**Status update.** CQ answering shipped as
`konclude_execute_conjunctive_query(handle, sparqlSelectQuery)` +
`konclude_query_result_*(handle, ...)` — the plain SPARQL-string design
from the findings immediately below, confirmed correct end-to-end
(314/314 rows, `docs/EMBEDDED_CQ_DRIVER.md`). It does **not** use the
`KoncludeQueryResultHandle`-with-explicit-destroy shape this section
originally proposed (see the corrected code block below), and the
atom-by-atom `KoncludeQueryPatternHandle` design in the following section
("CQ design revision: bypass SPARQL text entirely") was **not** what got
built — see that section's own status note for why and what's still open
about that choice.

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
- **Resolved, with a correction.** `executeConjunctiveQuery()` calls
  `CCreateKnowledgeBaseRevisionUpdateCommand` on every invocation to get a
  fresh, never-installed revision layered on top of the *current state* —
  but the state itself (`mCurrentStateRevision`, set up by
  `beginNewState()`/`konclude_state_assert_class_fact`) does get installed,
  once, lazily, on the first query. This was found by actually calling
  `executeConjunctiveQuery()` repeatedly against one live state with facts
  asserted into it: querying a Told, never-installed revision directly
  returned zero rows, including for content that predated the Tell, until
  installing was added. See `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s
  Decision 2 for the full mechanism.
- **Result extraction API** (confirmed, in `Reasoner/Query/`): three-level
  pull-iterator structure — `CVariableBindingsAnswersResult` (result set:
  `getVariableNames()`, `getResultCount()`,
  `getVariableBindingsAnswersIterator()`) → `CVariableBindingsAnswersResultIterator`
  (row-by-row) → `CVariableBindingsResultIterator` (cell-by-cell within a
  row) → `CVariableBindingStringResult::getBindingString()` (each bound
  value as a `QString`, convertible to `const char*` the same way
  `CEmbeddedReasoner::getLastErrorCStr()` already handles `mLastError`).
- **Actual shipped C API shape** (this bullet originally proposed a
  `KoncludeQueryResultHandle`-based design with an explicit destroy call —
  what was actually built is simpler than that):
  ```c
  int konclude_execute_conjunctive_query(KoncludeReasonerHandle handle, const char* sparqlSelectQuery);
  int konclude_query_result_row_count(KoncludeReasonerHandle handle);
  int konclude_query_result_variable_count(KoncludeReasonerHandle handle);
  const char* konclude_query_result_variable_name(KoncludeReasonerHandle handle, int varIndex);
  const char* konclude_query_result_binding(KoncludeReasonerHandle handle, int row, int varIndex);
  ```
  No separate result handle, no explicit destroy — results are extracted
  eagerly into plain `QStringList`/`QList<QStringList>` members on
  `CEmbeddedReasoner` right after `CCalculateQueryCommand` completes (see
  `docs/EMBEDDED_EXECUTE_CONJUNCTIVE_QUERY.md` step 7), so the
  `CQuery`/`CQueryResult` object graph doesn't need to stay alive for the
  caller to read results back, and the accessors just index into those
  members. Valid until the next `konclude_execute_conjunctive_query` call
  on the same handle. `generateQuerys()` returning a plural
  `QList<CQuery*>` was handled as predicted: exactly-one-query-in,
  exactly-one-query-out is enforced, anything else is an error (see
  `CEmbeddedReasoner.cpp`).

**CQ design revision: bypass SPARQL text entirely (for the FD use case) —
proposed here, not what was actually built:**

**Status update.** This section argued for superseding the SPARQL-string
API with an atom-by-atom `KoncludeQueryPatternHandle` design. In practice,
the SPARQL-string API (`konclude_execute_conjunctive_query`) is what got
implemented and shipped — the atom-based design below was never built.
Whether SPARQL-text parsing actually costs enough at FD's scale to justify
building this is now an open, *unmeasured* question, not a settled
decision either way — tracked as Decision 3 in
`docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`, which also notes that
every timing taken so far bundles text-parsing together with reasoning
cost in one number, so the parsing component in isolation has never
actually been measured. The technical findings below (about
`CSPARQLQueryBuilder`, atom construction, and `CComplexAnsweringQuery`'s
per-instance ontology binding) remain valid regardless of which API ships
and are still the right reference if this gets revisited.

- **The SPARQL-string plan above was considered superseded for FD** (this
  reasoning turned out not to determine what got built — see the status
  note just above), not because it's wrong, but because a cheaper, more
  direct path exists that also fits FD's call pattern (the same handful of
  CQ *shapes* re-executed against many states) better: build the query
  once as a typed object, skip SPARQL text and parsing altogether, and
  re-execute the same compiled pattern per state. The SPARQL-string API
  may still be worth keeping as a secondary, human/debug-friendly entry
  point later, but it is no longer the primary design for FD's hot path.
- **Confirmed: `CSPARQLSimpleQueryParser` is itself just a thin front-end
  over a clean, string-free C++ builder.** `processParts()`
  (`Source/Parser/CSPARQLSimpleQueryParser.cpp:121-291`) turns WHERE-clause
  triples into `QList<CAxiomExpression*>` via `getTripleOWLAxioms()`
  (inherited from `CSPARQLSimpleBuildingParser`,
  `Source/Parser/CSPARQLSimpleBuildingParser.h:68-77`), then hands that list
  straight to `mQueryBuilder->getSPARQLBasicGraphPatternIndividualSelectQuery(
  axiomList, disVarList, orderingList, filteringList, distinctModifier,
  limit, offset)` (`CSPARQLSimpleQueryParser.cpp:273`; `...AskQuery` at
  line 286).
- **That builder interface — `CSPARQLQueryBuilder`
  (`Source/Parser/CSPARQLQueryBuilder.h:69-118`) — takes no strings at all**:
  every method takes `QList<CAxiomExpression*>` + `QList<CExpressionVariable*>`
  + plain primitives (`limit`, `offset`, `bool distinct`). This is the
  parser's *output* layer, fully decoupled from tokenizing/parsing — exactly
  the boundary a C API should target.
- **Atom construction already has a non-SPARQL precedent.** The per-atom term
  builders (`getIndividualTermExpression`/`getClassTermExpression`/
  `getObjectPropertyTermExpression`, `CSPARQLSimpleBuildingParser.h:73-77`,
  each optionally minting/reusing a `CExpressionVariable*`) belong to
  `COntologyBuilder` — the same builder family the doc already cites via
  `CJNIAxiomExpressionVisitingLoader` for direct (non-parser) axiom
  construction. Building CQ atoms this way is the same technique already
  planned for `konclude_state_assert_*`, not a new dependency.
- **Reconfirmed:** `CJNIQueryProcessor`
  (`Source/Control/Interface/JNI/CJNIQueryProcessor.h:74-104`) remains
  single-atom-only (one class/property/individual name string per call, no
  join) — still not usable prior art for multi-atom CQs, matching the
  original finding above.
- **Resolved — no, definitively, via type signatures (not behavior that
  needed a runtime test):** `CComplexAnsweringQuery`
  (`Source/Reasoner/Query/CComplexAnsweringQuery.h:61-99`), the actual CQ
  query class, stores its target ontologies as plain member pointers —
  `CConcreteOntology* mOntology; CConcreteOntology* mExpressionsOntology;`
  (line 93-94) — set **only** by the constructor
  (`CComplexAnsweringQuery(CConcreteOntology* baseOntology,
  CConcreteOntology* expressionOntology, ...)`, line 65). The class exposes
  `getBaseOntology()`/`getExrpessionOntology()` but **no setter for either
  field** — the full public method list (line 63-86) has nothing that could
  rebind them. A query object is therefore structurally, permanently bound
  to the specific `CConcreteOntology*` pair it was built against; there is
  no API surface through which to point an existing one at a different
  scratch ontology. **"Build the CQ pattern once, run it per state" as
  originally proposed does not work as designed** — a fresh query object
  must be built per state, same as the atom-construction cost, every time.
  This doesn't force SPARQL text back into the design (atom construction is
  still the right approach per the findings above), it just means the
  "compiled pattern reused across many states" framing below is wrong and
  the API shape needs `konclude_query_pattern_execute` to *build* against
  the target state's ontology, not reuse a prior build.
  (Which concrete class implements `CSPARQLQueryBuilder` — likely
  `CConcreteOntologyQueryExtendedBuilder` under `Source/Reasoner/Generator/`
  — remains unconfirmed in detail; lower priority now that the reuse
  question above is settled.)
- **Proposed (not yet implemented) C API shape**, superseding the
  SPARQL-string version above for FD's use case:
  ```c
  typedef void* KoncludeQueryPatternHandle;    // a compiled, unbound CQ pattern
  typedef void* KoncludeQueryVarHandle;        // a CExpressionVariable-backed handle
  typedef void* KoncludeQueryResultHandle;

  KoncludeQueryPatternHandle konclude_query_pattern_create(KoncludeReasonerHandle handle);
  KoncludeQueryVarHandle konclude_query_pattern_variable(KoncludeQueryPatternHandle p, const char* varName);
  void konclude_query_pattern_add_class_atom(KoncludeQueryPatternHandle p, const char* classIRI, KoncludeQueryVarHandle var);
  void konclude_query_pattern_add_object_property_atom(KoncludeQueryPatternHandle p, const char* propertyIRI, KoncludeQueryVarHandle subjectVar, KoncludeQueryVarHandle objectVar);
  void konclude_query_pattern_project(KoncludeQueryPatternHandle p, KoncludeQueryVarHandle var);   // SELECT ?var
  void konclude_query_pattern_finalize(KoncludeQueryPatternHandle p);
  void konclude_query_pattern_destroy(KoncludeQueryPatternHandle p);

  // executed against a per-state scratch ontology (KoncludeStateHandle, see above).
  // NOTE: per the finding above, this must build a fresh CComplexAnsweringQuery
  // against `state`'s ontology on every call -- KoncludeQueryPatternHandle p is
  // reusable as an atom-list *specification* (avoids re-parsing/re-stringifying
  // atoms/variables), but the underlying CQuery object itself cannot be reused
  // across ontology instances and must be rebuilt from that spec each call.
  KoncludeQueryResultHandle konclude_query_pattern_execute(KoncludeStateHandle state, KoncludeQueryPatternHandle p);
  int konclude_query_result_row_count(KoncludeQueryResultHandle r);
  int konclude_query_result_variable_count(KoncludeQueryResultHandle r);
  const char* konclude_query_result_variable_name(KoncludeQueryResultHandle r, int varIndex);
  const char* konclude_query_result_binding(KoncludeQueryResultHandle r, int row, int varIndex);
  void konclude_query_result_destroy(KoncludeQueryResultHandle r);
  ```

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

See also `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md` for a later-phase
deep dive on the snapshot-per-state design's revision-chain memory/CPU
behavior at millions-of-calls scale, and for the status of the
`CPreconditionSynchronizer` dispatch-hang bug referenced throughout this
doc (likely fixed as a side effect of separate embedded-interface work, not
yet re-confirmed for the scratch-revision pattern specifically).

## Open risks to track

- In-process linking removes crash/hang isolation between FD and Konclude —
  any future tableau-kernel bug (of which the `-w 1` deadlock is one known
  example) now takes down the whole planner process.
- Statically-linked LGPLv3 obligations don't apply here (shared linking was
  chosen), but confirm the shared library is genuinely loaded dynamically at
  runtime (not folded in by a `--whole-archive`-style static step) if
  license compliance is ever audited.
- **Resolved.** The dispatch hang that used to block any command past KB
  creation on `CPreconditionSynchronizer` wrapping a raw
  `CCommanderManager`/`CCommanderManagerThread` is fixed: root cause was a
  missing `COWLlinkProcessor` in the dispatch path (several command types,
  including `CLoadKnowledgeBaseOWLAutoOntologyCommand` and
  `CIsConsistentQueryCommand`, are *only* handled inside
  `COWLlinkProcessor::processCustomsEvents`). Fixed by adding
  `CEmbeddedOWLlinkProcessor` and routing `loadOntologyFile()`/
  `checkConsistency()`/`checkSatisfiability()` through it. Confirmed fixed
  both directly (`checkConsistency`/`checkSatisfiability`/`loadOntologyFile`
  all now complete) and for the specific never-install
  `CCreateKnowledgeBaseRevisionUpdateCommand` pattern the snapshot-per-state
  design depends on (`probeScratchRevisionCycles`, 10/10 and 30/30 cycles,
  no hang — see "Update: the dispatch bug is fixed" under phase 2 above and
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md` Decision 4). No longer
  blocking any further facade work.
- `CSPOntologyRevisionManager` never frees revisions installed via
  `CInstallKnowledgeBaseRevisionUpdateCommand` (`onRevContainer` only cleared
  in the manager's own destructor, `CSPOntologyRevisionManager.cpp:53,127,323`)
  — **this does apply to the snapshot-per-state design**, one entry per FD
  state (not per CQ call): installing turned out to be required for a
  state's asserted facts to be query-visible at all, not avoidable by
  construction as originally claimed here — see
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2. The
  per-*query* revisions layered on top of an installed state are still
  never installed and still just `delete`d once a state's queries are done.
  Whether `onRevContainer` growing by one entry per FD state over a long
  search run is a problem in practice is unmeasured — tracked as the next
  benchmark step in the companion doc.
- One of the two performance assumptions the snapshot-per-state design was
  built on is now **confirmed by source reading** (not yet by runtime
  timing, which is blocked by the dispatch bug above): `CTBox`/`CRBox`
  reference-sharing is genuinely O(1) (pointer-aliasing, not per-element
  copy) — see "Resolved via source reading" under phase 2 above. The other
  (CQ pattern reuse across ontology instances) is **resolved as false** —
  query objects cannot be rebound to a different ontology after
  construction, confirmed via `CComplexAnsweringQuery`'s header (no setters)
  — see the CQ design revision section above (this also killed a *different*
  reuse idea proposed later, reusing a parsed `CQuery*` across ontology
  revisions via `CGetQueryDependentKnowledgeBaseRevisionUpdatesCommand` —
  see Decision 3 in `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`). Net
  effect: the design still avoids SPARQL text and incremental-retract
  bookkeeping in principle (though what actually shipped uses SPARQL text
  after all — see the CQ status notes above), and per-state ontology
  creation is confirmed cheap and no longer dispatch-blocked, but per-state
  query *construction* remains a known, unavoidable per-call cost. Actual
  timing is the next step, tracked in
  `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`, not here.
