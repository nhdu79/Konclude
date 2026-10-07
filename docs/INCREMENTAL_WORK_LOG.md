# Work Log: Incremental Reasoning Tasks

Short notes per task from `docs/INCREMENTAL_REASONING.md` §5: what changed,
decisions made, results. Newest entries last.

## Conventions

- **Debug lines:** temporary `fprintf(stderr, "[KDBG] …")` lines are tagged
  `// KDBG-TEMP`. Find them with `grep -rn KDBG-TEMP Source` and remove them
  before committing.
- **CRLF files:** many `Source/Reasoner` files use CRLF line endings, so do
  scripted edits byte-wise and check `git diff --stat` afterwards.
- **CLI build/run:** `make -f MakefileNoRedland -j12`, then
  `./Release/Konclude owllinkfile -w 2 -i Tests/<file> -o out.xml`.
- **Embedded build/run:** `Tools/EmbeddedDriver/rebuild.sh`, then run the
  drivers from the repo root.
- **Comparing reasoner output:** Konclude's output isn't reproducible line by
  line. Compare classification as the subsumption closure, and realisation as
  the set of `ClassAssertion` axioms.

## Task 1: Wrong answers after tell → query → tell → query. Fixed (`b413d14d`)

**Root cause.**

1. **Full rebuild of each queried revision.** With `IncrementalRebuild=false`
   (the default), each queried revision is rebuilt from the empty basement:
   `CRequirementConfigPreprocessingThread::countActiveEntites()`, around
   line 329, calls `referenceDataBoxes(basement)`.
2. **Old individuals keep their old IDs.**
   `CConcreteOntologyUpdateBuilder::getIndividualForIndividualTerm()` still
   finds individuals from earlier revisions in the name → individual map,
   which isn't reset, and reuses their old IDs.
3. **New individuals start from the reset ABox.** `buildIndividualIDs()`
   numbers new individuals from `aBox->getIndividualCount()`, which is 0
   after the reset.
4. **IDs collide.** Reused and new IDs collide, and reused IDs collide with
   each other. Only one individual per ID survives in the ABox, and the
   backend cache and realizer just reflect that.

**Fix (option A).** In `Reasoner/Generator/CConcreteOntologyUpdateBuilder.{h,cpp}`:

- `mAssignedIndividualIdSet`: a reused ID that's already taken goes through
  the fresh-ID path instead.
- `mMaxKnownIndividualId`: the largest ID in the name map.
- Fresh IDs start at `max(ABox count, maxKnown+1, maxAssigned+1)`.
- Result: IDs stay stable across revisions and only grow.

**Decisions.**

| Decision | Why |
|---|---|
| Fix in the reasoner core | It reproduces with the stock `owllinkfile` CLI |
| No upstream fix to wait for | Issue #19 is unrelated; no commits upstream since May 2022 |
| Dropped the backend-cache fix | That cache only mirrored an ABox that was already wrong |
| Option A over B (no reuse in a full rebuild) and C (`IncrementalRebuild=true`) | A keeps IDs stable across revisions; C segfaults (exit 139) on the fixtures |

**Results.**

| Check | Result |
|---|---|
| `Tests/incremental-tell-query-tell-request.xml` | `{john}`, `{john,mary}`, `{john,mary,bob}` ✓ (was `{john}`, `{mary}`, `{bob}`) |
| `Tests/incremental-retract-retell-request.xml` | `{john}`, `{mary}`, `{john,mary}`, Father `{john}` ✓ |
| `Tests/incremental-tell-tell-query-request.xml` | unchanged, correct ✓ |
| `embedded_state_driver` | 10/10 runs PASS ✓ |
| `embedded_state_benchmark` | 0 problems in 5 short runs and in `50 10 5 5` (250 queries) ✓ |
| `embedded_cq_driver` and both probes | PASS ✓ |
| `classification` roberts-family / galen | subsumption closure identical (481 / 30,795 pairs) ✓ |
| `realisation` roberts-family | 5,418/5,419 identical; `PersonWithManySibling` is random with or without the fix |
| `sparqlfile -c Configs/querying-config.xml` | hangs in realization with or without the fix |

**Open.**

- Done: `KDBG-TEMP` lines removed; committed as `b413d14d` on branch
  `fix/incremental-reasoning`.
- `Tests/incremental-*-request.xml` aren't regression-checked
  automatically; run them by hand.

## Task 2: Realizer use-after-free and thread leak. Fixed (`89bfea6f`)

Files: `Reasoner/Realizer/CRealizerThread.cpp`;
`Reasoner/Kernel/Manager/C{Realization,Preprocessing,Precomputation}Manager.{h,cpp}`;
`Reasoner/Classifier/CConfigDependedSubsumptionClassifierFactory.{h,cpp}`.

**Use-after-free.**

- **Cause.** In `CRealizerThread::processCustomsEvents()` (the
  `CRealizeOntologyEvent` branch), when a requirement completes
  synchronously, `addProcessingRequirement()` deletes `callbackProcData`.
  `logRequirementProcessingStartStatistics()` then reads it.
- **How often.** The `KDBG` counter shows this path is taken on every
  realization (3 of 3 queries in the tell-query-tell request file), so the
  old code read freed memory on every query.
- **Fix** (`Reasoner/Realizer/CRealizerThread.cpp`):
  1. Hold one extra pending count while the requirements are added.
  2. Release it after logging the start statistics.
  3. If everything already completed, finish right there: log the finish
     statistics, run the callback, delete.
- **Side effect.** An empty requirement list now gets its callback; before,
  it never got one.

**Thread leak** (found to be wider than the realizer). Four managers each
created one OS thread per ontology (every queried revision *and* every
query layer) and never freed them:

| Manager | Thread classes |
|---|---|
| `Kernel/Manager/CRealizationManager` | `COptimizedRepresentativeKPSetOntologyRealizingThread`, `CIncrementalKPSetOntologyConceptRealizingThread` |
| `Kernel/Manager/CPreprocessingManager` | `CRequirementConfigPreprocessingThread` |
| `Kernel/Manager/CPrecomputationManager` | `CTotallyPrecomputationThread`, `CIncrementalPrecomputationThread` |
| `Classifier/CConfigDependedSubsumptionClassifierFactory` | the 4 class classifiers + 2 role classifiers |

**Fix.** Create one instance per thread class, lazily, and share it across
all ontologies. All of these threads already keep per-ontology items
(`mOntItemHash` / `ontItemHash` + processing lists).

**Decisions.**

| Decision | Why |
|---|---|
| Share threads instead of deleting them | The ontology's `CRealization` points at items owned by its realizer; incremental realization/classification reads the previous version's data. Deleting would leave dangling pointers. |
| Accept shared settings | Settings are re-read per item (realizer: per processing item; precomputation: when an ontology's item is created). On a shared thread the latest ontology's config wins, and all revisions of one KB share a config. |
| Extended task 2 beyond the realizer | Fixing only the realizer halved the threads (467 → 214) but they kept growing. It's the same leak in three more places. |

**Results.**

| Check | Result |
|---|---|
| Threads, `embedded_state_benchmark 50 10 10 3` | before: rising to 467; realizer shared: 214; all shared: **flat at 26** ✓ |
| Threads, `embedded_state_benchmark 200 20 5 5` (500 queries) | max 26 ✓, 0 problems |
| RSS | ~105 MB at the end of the 50-fact run (only the final value was recorded, not its growth). Items are never freed, so it is expected to grow; measuring is part of task 5 |
| `Tests/incremental-*-request.xml` | all correct ✓ |
| `embedded_state_driver` ×10, `embedded_cq_driver`, both probes | PASS ✓ |
| `embedded_cq_benchmark` (create/destroy cycles) | robot-grid 20/20, roberts 3/3, 0 failures ✓ |
| classification roberts / galen | closure identical (481 / 30,795 pairs) ✓ |
| realisation roberts | identical except the known-random `PersonWithManySibling` ✓ |
| `owllinkfile` classify+realize | identical up to the namespace-attribute order, which also varies between identical runs ✓ |

**Open.**

- Done: `KDBG-TEMP` lines removed (none left in `Source/`). Rebuilt and
  rechecked without them: the request files, `embedded_state_driver` ×5,
  `embedded_cq_driver`, and the threads benchmark (flat at 26) all pass.
  Committed as `89bfea6f` on `fix/incremental-reasoning`.
- Thread and ontology items are still never freed, so memory grows with
  each revision. That is task 5.

## Task 3: Object property assertions. Done

**API.**

- New `KoncludeObjectPropertyAssertion {subjectIRI, propertyIRI, objectIRI}`.
- New `konclude_tell_and_retract_assertions(handle, classTell, classRetract,
  propTell, propRetract)`: one call is one revision; retracts come before
  tells; any array may be `NULL` when its count is 0; an invalid argument
  changes nothing.
- `konclude_tell_and_retract_axioms` stays as the class-only wrapper, so
  Powerlifted's ABI is unchanged.
- C++ side: `AssertionChangeSet` in `CEmbeddedOntologyLoader.h`; the builder
  uses `getObjectProberty` (sic) + `getObjectPropertyAssertion(subject,
  object, property)`.

**Test.**

- New fixture `Tests/embedded-state-tbox-roles.owl.xml`:
  `ObjectPropertyDomain(hasChild, Person)`, `Parent ≡ ∃hasChild.Person`.
- New driver `Tools/EmbeddedDriver/embedded_property_driver.cpp`: 4 states
  plus an invalid call.

**Bugs found by the driver (both pre-existing, outside the embedded layer).**

1. **Crash on a repeated query `?x hasChild ?y`.**
   - Where:
     `COptimizedComplexExpressionAnsweringHandler::tryReuseVariableRolePropagationItem`
     (`Reasoner/Answerer/…AnsweringHandler.cpp`, about line 8503).
   - Cause: reusing a role-propagation item from an earlier query, it
     inserted into `varExpVarComItemHash->value(otherExp)`, which is null
     when `?y` has no item yet.
   - Fix: create the set if it's missing, as the callers at lines 5492–5495
     do.
2. **Property facts and inferences random after a state change.**
   - Symptom: in OWLlink and embedded runs, after any later tell or retract
     (even of an unrelated individual), `Parent` and sometimes the asserted
     `hasChild` pairs themselves came out wrong. Results varied run to run:
     8 of 10 driver runs failed. The build without the task-1 fix failed
     the same way.
   - Cause: in a rebuild from the basement,
     `CConcreteOntologyUpdateBuilder::setIndividualAssertionRoleFromObjectPropertyTerm()`
     looks up the destination with `forceLocalisation = false`. If the
     destination hasn't been built yet in this rebuild, the name-map hit
     returns the **previous revision's** `CIndividual` without creating this
     revision's copy, so the role assertion links to a stale object. Which
     comes first depends on hash order, hence the randomness.
   - Fix (`getIndividualForIndividualTerm`): an individual found by name
     always needs localisation. Term-hash lookups are unchanged.

**Decisions.**

| Decision | Why |
|---|---|
| New function instead of changing `konclude_tell_and_retract_axioms` | Keeps Powerlifted's ABI; one call is still one revision |
| `AssertionChangeSet` on the C++ side | Avoids passing 8 arguments through every layer |
| Fix both core bugs here | Property support is unusable without them; each gets its own commit |
| Skip SPARQL-free instance queries for now | Optional in the roadmap; SPARQL CQs already cover the need |

**Results.**

| Check | Result |
|---|---|
| `embedded_property_driver` ×10 | 10/10 PASS (was 2/10) ✓ |
| `Tests/incremental-property-tell-query-tell-request.xml` (new) | Parent `{john, mary}` both times, every run ✓ (was random) |
| `Tests/incremental-property-retract-request.xml` (new) | Parent `{john}` after the retract, every run ✓ |
| Repeated `?x hasChild ?y` in one state | no crash ✓ |
| Task-1 request files, `embedded_state_driver` ×10, `embedded_cq_driver`, probes, both benchmarks | unchanged, PASS ✓ |
| classification roberts / galen | closure identical (481 / 30,795 pairs) ✓ |
| realisation roberts | identical except the known-random `PersonWithManySibling` ✓ |

**Open.**

- Committed as three commits on `fix/incremental-reasoning`: builder
  localisation fix + property request files (`a873242e`), answerer crash
  fix (`160f90ae`), and the embedded API + driver + docs (the commit
  following these).
- SPARQL-free instance/type queries (optional).

## Task 4: Branching (Option B). Deferred

Decision (2026-10-06): stay with Option A (incremental tell/retract on one
KB) for now. Branching is kept as a possible later experiment in
`docs/INCREMENTAL_REASONING.md` §6. Next: task 5 (memory cleanup) and task
6 (Powerlifted wiring).

## Task 5: Memory cleanup. 5a done; 5b done except the saturation tasks (option A)

**Measurement.** New driver `Tools/EmbeddedDriver/embedded_memory_benchmark.cpp`
reads RSS and the thread count from `/proc/self/status` while it cycles
through states. Run: 500 states, 20 facts each, names drawn from a pool of
100.

| Mode | Before 5a | After 5a |
|---|---|---|
| tell + 2 queries (the real use) | 526 MB; 822 → 1,308 kB/state, **rising** | 332 MB; flat at ~660 kB/state |
| tell/retract only | 173 MB; 98 → 595 kB/state, rising | (not re-run) |
| queries only, one state | 23 MB; flat at ~40 kB/state | (not re-run) |

**5a: quadratic change history.**

- **Cause.** Each new revision copies its predecessor's build data
  (`COntologyBuildData::referenceBuildData`). Every container stays bounded
  except `mChangeAxiomList`, which grows ~32 entries per state: 9,545 after
  300 states. Appending detaches the Qt list, so every revision keeps a full
  copy of the whole history. Each basement rebuild also replayed the whole
  history for active-entity counting.
- **Fix** (`CRequirementConfigPreprocessingThread::countActiveEntites`,
  basement-reset branch): compact the change list to
  `{(axiom, added) : axiom ∈ tellAxiomSet}` and reset
  `lastChangeCountedAxiom`. After a reset every reader starts at 0 and needs
  only the net effect, so the results are the same.
- **Result.** The change list stays at 51–61 entries.
- **Limitation.** Revisions that are never queried aren't compacted (no
  preprocessing runs for them), so the tell-only mode still grows. Real use
  queries every state.
- **Verified.**
  - All 5 `Tests/incremental-*-request.xml` ×3.
  - `embedded_state_driver` / `embedded_property_driver` ×5 each.
  - `embedded_cq_driver`, probe, `embedded_state_benchmark`.
  - Classification closure identical (roberts, galen); realisation identical
    except the random `PersonWithManySibling`.

**5b findings: the remaining ~660 kB per queried state.**

1. **What's kept per state.** Each queried state creates 2 ontologies: the
   state revision, which goes through preprocessor, precomputator and
   realizer, and the query layer, which goes through the preprocessor only.
   Nothing in Konclude ever removes per-ontology items (no release path in
   any manager, thread or cache: `grep` for `remove`/`release` finds none).
2. **Latent hazard.** `CEmbeddedQueryManager::resetForNewState()` deletes
   query layers, but the shared preprocessor (`mOntItemHash`) and
   `CPreprocessingManager::mOntoPreprocessHash` keep entries keyed by the
   freed `CConcreteOntology*`. No address was reused over 500 states (1,000
   distinct pointers), because memory only grows. Once 5b frees memory,
   reuse becomes likely and a new ontology could hit a stale item, so these
   entries must be removed first.
3. **Recycling the handle doesn't help.** 10 × (create → load → 50 states →
   destroy): RSS grows +30 MB per cycle and doesn't drop at destroy, and +13
   threads stay alive per destroyed handle. `konclude_destroy_reasoner`
   frees almost nothing. (Fixed in task 5c.)

**5b options (decision needed).**

| Option | What | Pros | Cons |
|---|---|---|---|
| A. Release superseded revisions in one handle | A "release ontology" path through every holder: revision manager (`onRevContainer`, `revisionHash`), the 4 managers' hashes + shared threads' items (preprocess, precompute, classify, realize; on their own threads), backend cache + occurrence statistics (keyed by `ontologyID`), answering items, and the ontology's own memory. Also cut `getPreviousOntologyVersion` links | Memory bounded by the current state; matches Option A use | Large: touches every component; threading/ordering risks; the author warned cleanup is incomplete |
| B. Make `konclude_destroy_reasoner` free everything; Powerlifted recreates the handle every N states | Stop and delete threads/managers; implement the empty destructors | Coarse but conceptually simpler: no partial-liveness bookkeeping | Still many classes; recreating costs a TBox reload + first reasoning per N states |
| C. Run Konclude in a child process that Powerlifted restarts every N states | Powerlifted side only | The OS reclaims everything; no Konclude changes | IPC/serialization per call, losing the in-process benefit |

Estimate: at ~660 kB/state, 10k states ≈ 6.6 GB and 100k ≈ 66 GB.

**Decision (2026-10-06).** 5b uses option A: release superseded revisions
inside one handle. 5a was committed separately first.

**5b measurements.** Heap (`mallinfo2`) per state, states 101–300: tell/retract
−60 kB (the previous query layer gets deleted), **1st query +1,100 kB**, 2nd
query +27 kB. So almost everything retained comes from the first query of a
state: building, preprocessing, precomputing and realizing the new revision,
plus its query layer.

**5b holders of per-ontology state** (nothing ever removes these entries):

| Keyed by | Holder |
|---|---|
| `CConcreteOntology*` | `CRequirementPreparingData::mOntoReqDataHash`; `CPreprocessing/CPrecomputation/CRealizationManager` hashes; `CClassificationManager` (4 hashes); `CFirstQueuedAnsweringHandlerProvider::mOntoloyHandlerDataHash`; `mOntItemHash` + active/inactive/processing lists in each shared thread (`CPreprocessingThread`, `CPrecomputationThread`, `CRealizerThread`, `CSubsumptionClassifierThread`) |
| `ontologyID` | `CBackendRepresentativeMemoryCache` (`mOntologyIdentifierDataHash`, `mFixedOntologyIdentifierDataHash`), `COccurrenceStatisticsCacheData::mOntologyDataHash` |
| revision | `CSPOntologyRevisionManager`: `onRevContainer`, `revisionHash` |
| back-references | the next revision's `getPreviousOntologyVersion()` chain (walked by the update builder's triples-accessor search and the incremental searches); name-map entries for retracted individuals point at old revisions' `CIndividual` objects (read by task 1's `mMaxKnownIndividualId` loop) |

`~CConcreteOntology` frees its memory-pool context (data boxes, concepts,
individuals), build data, mappings and its consistence/classification/
precomputation/preprocessing/realization objects. So deleting a revision
frees most of its memory, once every external holder has dropped its entries.

**5b plan (stages; measure + rerun all checks after each):**

1. **Release path.**
   - Add `CReleaseOntologyCommand` (embedded layer → `CCommanderManagerThread`)
     → `CReasonerManager::releaseOntology(ontology, callback)` (non-pure,
     no-op default).
   - In `CReasonerManagerThread`, on its own thread, ask each manager to
     drop its entry and each shared thread to drop and delete its item (on
     that thread), then call back.
   - Apply it to the query layer that `resetForNewState()` already deletes.
     This removes the dangling-key hazard.
2. **Superseded state revisions.**
   - After state *k+1* is installed and its first query is done, release
     revision *k*'s holders, including the answering handler and the
     caches keyed by `ontologyID`.
   - Cut references: point *k+1*'s previous version to the basement, and
     replace name-map entries that point at freed individuals.
   - Then remove revision *k* from the revision manager and delete it.
3. **Kernel caches.** Check the satisfiability/unsatisfiability/completion-
   graph caches for pointers into released ontologies (the author's warning).
   An AddressSanitizer build of the library is the safety net for stages 2–3.

**Stage 1 done (not committed).** The release path exists and is used for
query layers.

- **New files.**
  - `Control/Command/Instructions/CReleaseOntologyCommand` (tag 2104).
  - `Reasoner/Kernel/Manager/Events/CReleaseOntologyEvent` (manager event
    2008).
  - `Reasoner/Preprocess/Events/CReleasePreprocessingOntologyEvent`
    (preprocessing event 2003).
  - All registered in `Konclude.pri`.
- **Flow.**
  1. `CEmbeddedQueryManager::resetForNewState()` sends `CReleaseOntologyCommand`
     and waits, then deletes the query layer.
  2. `CCommanderManagerThread` calls `CReasonerManager::releaseOntology`
     (no-op default; overridden in `CReasonerManagerThread`), using
     `COntologyPreparedCallbackEvent` to finish the command.
  3. On the manager thread, `CPreprocessingManager::releaseOntology` takes
     the hash entry and the preprocessor deletes its item on its own thread.
     An item that is still active or has running tests is kept, with a
     warning.
- **Verified.**
  - A temporary counter: 19 of 20 query layers released. Remaining items
    grow by 1 per state, i.e. the state revisions (stage 2).
  - Memory unchanged (~664 kB/state); query-layer preprocessing items are
    small. Stage 1's benefit is removing the dangling-pointer hazard.
  - Regression: all 5 request files, both state drivers ×5, probe, both
    benchmarks; classification closure identical (roberts, galen);
    realisation differs only in the random `PersonWithManySibling`.

**Stage 2a done (not committed).** Revisions older than the last queried one
now release their items in every shared thread; nothing is deleted yet.

- **New events:** `CReleasePrecomputation/Realization/ClassificationOntologyEvent`
  (event types 2006 / 2004 / 2011).
- **New overrides:** `releaseOntology` in `CPrecomputator`/`CRealizer`/
  `CSubsumptionClassifier` (no-op default) and in their threads; in the
  precomputation and realization managers; and in `CClassificationManager`
  (4 hashes).
- **Fan-out:** `CReasonerManagerThread` uses a local
  `CReleaseOntologyCallbackCounter` to wait for all components.
- **Scheduling:** `CEmbeddedOntologyLoader::releaseSupersededRevisions()` and
  `markCurrentStateQueried()`, which the query manager calls after a
  successful query or check.
- **Result:** frees only ~20 kB/state (664 → 645). The bulk is in the
  revisions themselves and the answering handlers (stages 2b/2c).
- **ASan build:** `MakefileEmbeddedAsan` → `ReleaseEmbeddedAsan/`
  (qmake `-after` overrides; ignored by git). Run drivers linked against it
  with `ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0`. The mismatch
  check has to be off because of a separate bug:
  `COccurrenceUnsatisfiableCacheEntry.cpp:61` deletes `new[]` memory with
  `delete` (at shutdown, in `COccurrenceUnsatisfiableCache::threadStopped`).
  With 2a, `embedded_state_driver`, `embedded_property_driver` and a
  30-state memory benchmark run without ASan errors.

**Next (stage 2b/2c):**
- **2b:** answering handler release (`CAnsweringManagerThread` →
  `CFirstQueuedAnsweringHandlerProvider` → per-handler data in each
  `CAnswererThread`).
- **2c:**
  - a persistent individual-ID high-water mark in `COntologyBuildData`;
  - name-map cleanup in `CMappingUpdaterPreProcess`;
  - point the next revision's previous version at the basement;
  - the revision manager drops and deletes the revision.
- Then measure and run everything under ASan.

**Stage 2b done (not committed).** Answering handlers of released revisions
are deleted.

- **API:** `CAnsweringManager::releaseOntology` (no-op default);
  `CAnsweringManagerThread` gets a `CReleaseAnsweringOntologyEvent` (event
  type 2009); `CAnsweringHandlerProvider::releaseOntology` deletes the idle
  handlers. Answerer threads need nothing: they drop their per-handler data
  when answering finishes.
- **Fan-out:** the reasoner manager includes the answering manager.
- **Bug exposed and fixed:** deleting a handler deletes its answering item's
  *testing ontology*, which shares the base ontology's `CConsistence`, so
  teardown freed it twice (ASan heap-use-after-free in `~CConcreteOntology`).
  `~COptimizedComplexExpressionOntologyAnsweringItem` now unsets the
  consistence first, as the realizing item already does for its helper
  ontologies.
- **Result:** ~570 kB/state (was 645). ASan clean on both state drivers and
  a 30-state memory benchmark. Normal build: drivers ×5, `embedded_cq_driver`,
  `embedded_cq_benchmark`, and `embedded_state_benchmark` all pass.

**Stage 2c done (not committed).** Superseded revisions are deleted.

- **Sequence** (`CEmbeddedOntologyLoader::releaseSupersededRevisions`): for
  every revision older than the last queried one:
  1. **Cut links in the next revision.**
     `COntologyIncrementalRevisionData::forgetOntology` points the previous
     version at the basement and clears the other previous-ontology
     pointers. `COntologyRevision::setPreviousOntologyRevision(nullptr)`.
     `CConfigurationBase::skipPreviousConfiguration` re-points the parent
     config past the deleted one and drops cached config references.
  2. **Release** reasoner data (`CReleaseOntologyCommand`, stages 1–2b).
  3. **Delete:** the new `CDeleteKnowledgeBaseRevisionCommand` (tag 2105)
     makes `CSPOntologyRevisionManager` remove the revision from
     `onRevContainer` and delete it. The current head is refused.
- **Prerequisites.**
  - `CMappingUpdaterPreProcess::removeForeignNameMappings`: after a build,
    the class/property/individual name maps keep only objects of the current
    revision.
  - `COntologyBuildData` max-assigned individual ID: task 1's fresh IDs no
    longer depend on old individuals staying in the name map.
- **ASan findings, fixed.**
  - Each revision's `COntologyConfigurationExtension` is the parent of the
    next one's.
  - The answering manager was configured with the config of the first
    queried revision; it now uses the basement ontology's config
    (`CReasonerManagerThread`, where `CAnsweringManagerThread` is created).
- **Verified.**
  - ASan: both state drivers and a 100-state memory benchmark, 0 errors.
  - Normal build: all 5 request files (CLI), drivers ×5, `embedded_cq_driver`,
    probe, both benchmarks, OWLlink classify+realize.
  - Classification closure identical (roberts, galen); realisation differs
    only in the random `PersonWithManySibling`.
- **Memory:** ~486 kB/state (500 states; was ~570 after 2b, 664 before 5b).

**What's left per state (heap profile, `Tools/HeapProfiler/`, 15 vs 30
states).** About 12 call sites each keep **one ~50 kB memory pool per state**
from `CNewAllocationMemoryPoolProvider::acquireMemoryPool`, never released:
12 × 48.8 kB ≈ **586 kB of heap per state** in total (one more site adds
~30 kB). That is more than the ~486 kB/state RSS growth because the profiler
counts requested heap bytes, while RSS only counts touched pages, and partly
used pools are only partly resident. Before 2c, `mallinfo` showed the same
gap (~1,068 kB heap vs ~664 kB RSS per state).
They are requested by:

- **precomputation jobs:** `CPrecomputationThread::processCalculationJob` —
  consistency, individual/concept saturation and occurrence-statistics
  saturation tasks, plus their task-schedule events
  (`CTaskEventCommunicator::postSendTaskScheduleEvent`) and
  `CSatisfiableCalculationTask` allocations;
- **classifier jobs**, and the role classifier's temporary role-classification
  ontology (`COptimizedKPSetRoleSubsumptionClassifierThread::createTemporaryRoleClassificationOntology`);
- **two `CThread::event` sites**, still to be identified.

**Stage 2d done.** ~255 kB/state (500 states; was ~486
after 2c).

- **Finding the holders.** The scheduler recycles released pools through a
  reserve (`CLimitedReserveMemoryPoolAllocationManager`), so the call stacks
  above show where a pool was first acquired, not who keeps it. With the
  reserve set to 0 (temporary diagnostic), the per-state holders are:

  | Holder | Heap/state | Result |
  |---|---|---|
  | Role classifier's temporary ontology (`createTemporaryRoleClassificationOntology`) | ~52 kB | freed |
  | `CBackendRepresentativeMemoryCache` data of the revision's `ontologyID` | ~98 kB | freed |
  | Logger: keeps every message (`Konclude.Logging.MaxLogMessageCount` = -1) | ~20 kB (≈100 kB RSS) | capped |
  | Precomputation saturation tasks (2 pools) + backend association allocation inside them | ~147 kB | **kept**, see below |
  | Builder name hashes, answerer item linkers | ~10 kB | not investigated |

- **Fixes.**
  1. *Virtual destructors.* `COntologyPreprocessingItem`,
     `COntologyRealizingItem` and `COntologyPrecomputationItem` had none, so
     the `delete item` calls of stages 1/2a skipped the derived destructors
     (undefined behavior; leaked e.g. each item's
     `CCalculationConfigurationExtension`). The realizing item's destructor
     now also guards against null temporary ontologies.
  2. *Role classifier.* `~COptimizedKPSetRoleOntologyClassificationItem`
     deletes the temporary ontology (after unsetting the shared consistence,
     like the realizing item) and its two temporary individuals.
  3. *Backend cache.* `CBackendRepresentativeMemoryCache::releaseOntologyData`
     (event `CReleaseOntologyDataCacheEvent`, type 2011) is part of the
     release fan-out in `CReasonerManagerThread`. On the cache thread,
     `releaseOntologyIdentifierData`:
     1. removes the identifier from the identifier hash and the fixed hash;
     2. sets each remaining data version's usage count to the number of
        reader slots that refer to it (the hash, fixed-hash and fixed-reader
        references are dropped: the fixed reader belongs to the realizing item
        released alongside);
     3. deletes unreferenced versions now; the others are deleted by
        `cleanUnusedSlots` when their slots go;
     4. deletes the identifier's shared ontology context once no slot refers
        to the identifier (`deleteUnreferredReleasedOntologyContexts`).

     Individual association contexts with separate memory management
     (individuals with >100 neighbours or >20 updates) are not freed.
  4. *Logger.* `CEmbeddedReasoner` configures the logger to keep at most
     1,000 messages; its observer copies the text it needs.
  5. *Race in the release callback* (stage 1 bug). `CCommanderManagerThread`
     cast the command to `CReleaseOntologyCommand` after finishing it as a
     `CPrepareKnowledgeBaseCommand`, when the waiting issuer may already have
     deleted it (ASan SEGV in `__dynamic_cast`). Both casts now happen first.

- **Why the saturation tasks stay.** The precomputation keeps its saturation
  tasks as models (`CSaturationTaskData::overtakeData()`). Freeing them with
  the revision is unsafe:
  1. Concept saturation data is shared across revisions through the TBox
     concepts: `CTotallyOntologyPrecomputationItem::getSaturationConceptDataItem`
     reuses the item that an earlier revision linked into the concept's
     `CConceptSaturationReferenceLinkingData`, and its node lives in that
     revision's task.
  2. Nodes then chain across tasks: `CPrecomputedSaturationSubsumerExtractor`
     follows `getSubstituteIndividualNode()` from a concept-linked node into a
     later revision's task.
  3. Tried and reverted: freeing all tasks (ASan use-after-free in the next
     revision's saturation), and keeping only the tasks with concept- or
     individual-linked nodes (use-after-free via the substitute chain). A safe
     release needs a reachability analysis over the saturation node graph, or
     per-revision concept saturation data.
  4. No consistency-model tasks are kept in this workload (consistency comes
     from the saturation); releasing those is untested and not done.

- **Verified.**
  - ASan: state/property drivers, 2 × 100-state memory benchmark, 0 errors.
  - Normal build: state/property drivers ×5, `embedded_cq_driver`, both
    probes, `embedded_state_benchmark`, `embedded_cq_benchmark 5 all`,
    500-state memory benchmark.
  - CLI vs a `HEAD` build: all 5 request files give identical responses
    (order ignored); classification closure identical (roberts, galen);
    realisation differs only in the random `PersonWithManySibling`.

- **Hazard: `embedded_cq_benchmark` at its default 1000 iterations.** Each
  iteration creates, loads and destroys a handle; `konclude_destroy_reasoner`
  frees almost nothing (finding 3 above), so roberts-family leaks ~420 MB per
  cycle. On a 22 GB machine it was OOM-killed, and systemd
  (`DefaultOOMPolicy=stop`) then stopped the whole tmux scope, including the
  shell. Run it with few iterations (e.g. `./embedded_cq_benchmark 5 all`)
  until handle destruction frees its data.

## Task 5c: Freeing a handle (`konclude_destroy_reasoner`). Done, except the per-state leak

**Goal.** Destroying a handle frees everything it created, so a caller can
bound memory by recreating the handle every N states.

**Measurement.** Two new drivers; both report RSS after `malloc_trim(0)`
(glibc's `mallinfo2` only covers the main arena, Konclude's threads allocate
from others).

- `embedded_handle_cycle_benchmark [small|roberts] [cycles] [statesPerCycle]`:
  create → load → use → destroy, repeated; RSS and threads after each destroy.
- `embedded_incremental_growth_benchmark [states] [queriesPerState]
  [reportEvery] [recreateEvery]`: one handle, each change tells 2 new facts
  and retracts 1 every second step (state grows by 1–2 facts), 2 queries per
  state; optionally destroys and recreates the handle (load + re-tell the
  state) every `recreateEvery` states. Ends with `malloc_stats()`.

**Teardown done so far** (ASan clean on handle cycles, small and roberts):

1. *Threads.* The answering, realization, precomputation, preprocessing and
   classification managers stop and delete their shared worker threads;
   `CReasonerManagerThread::threadStopped` deletes them in order (issuers →
   calculation manager → caches) and now also stops and deletes the
   computed-consequences, backend and occurrence-statistics caches. The
   commander stops itself and deletes the classification manager before the
   reasoner manager. Worker interfaces got virtual destructors.
2. *Calculation engine.* `CConcurrentTaskCalculationManager` deletes its
   environment; the environment stops all processor threads before deleting
   any; each processor unit frees its allocator (incl. its reserve of up to
   80,000 free pools), context, releaser, communicators, pool provider and
   task-handle algorithm; the scheduler also its task reserve queue.
3. *Kept tasks.* Tasks kept as models (not memory releaseable: consistency,
   saturation, classification) were never freed. Each unit now records them
   when it completes them (`mRetainedTaskList`) and releases them in its
   destructor. Safe only at teardown, when nothing reads them any more (see
   2d for why per revision it is not). ~235 MB per roberts handle.
4. *Backend cache.* The destructor frees all data versions, ontology
   contexts, individual association contexts, reader slots, and the pools of
   written data, which the cache keeps on purpose
   (`//mContext.releaseTemporaryMemoryPools(memoryPools)` in the write
   handler) and now collects in `mRetainedWriteMemoryPools`.
5. *Saturation successor maps.* `CLinkedRoleSaturationSuccessorHash`
   allocated `CLinkedRoleSaturationSuccessorData` without its process
   context, so its map's nodes were plain mallocs and leaked whenever the
   task memory was released (also during normal reasoning).
6. *Items.* Worker threads and the answering handler provider delete their
   remaining per-ontology items (and the provider its handler factory).

| Workload (kept per destroyed handle) | Before | Now |
|---|---|---|
| roberts, 1 query | ~410 MB, 13 threads | ~13 MB RSS (~6 MB live heap), 0 threads |
| small TBox, 10 states | ~8.8 MB, 13 threads | ~0.95 MB, 0 threads |

The ~6 MB left on roberts is spread over ~30 sites below 0.6 MB each, mostly
sub-structures of realizer/answering items whose destructors free only part
of what they build.

**Finding: the per-state cost is a leak that survives teardown.**
`embedded_incremental_growth_benchmark 1000 2 100 <N>`:

| state | facts | RSS (no recreate) | growth per state | time per state |
|---|---|---|---|---|
| 100 | 150 | 67 MB | 0.37 MB | 9 ms |
| 500 | 750 | 461 MB | 1.37 MB | 16 ms |
| 1000 | 1500 | 1.54 GB | 2.66 MB | 28 ms |

- Each change costs ~1.8 kB per fact in the current state, so memory grows
  quadratically with the number of changes on a growing state.
- Recreating does not bound it. Final RSS after 1000 states with recreation
  every 50 / 100 / 250 / 500 / never: 1.11 / 1.13 / 1.22 / 1.35 / 1.51 GB.
- After destroying the handle, `malloc_stats()` shows ~1.1 GB still **in
  use** (not just held), so it is a leak, not fragmentation. A fresh handle
  with all 1,500 facts takes ~10 MB.
- Live heap at exit (heap profiler): 9.9 MB after 100 states, 35.3 MB after
  200. The surviving allocations are many small ones per revision: interned
  IRI names (`CAbbreviatedIRIName::getIRIName`), query answer copies
  (`CVariableBindingsListAnswerResult::getCopy`), builder names
  (`CConcreteOntologyUpdateBuilder::updateName`), saturation job data,
  backend-cache indexing. Likely cause (not confirmed per site): mallocs (e.g.
  `QString`s) owned by objects that live in a revision's memory pools;
  freeing the pools never runs those objects' destructors.
- Recreating itself is cheap: destroy ~100 ms; create + load + re-tell 1,500
  facts + first query ~25 ms.

**Finding: two handles must not be alive at once.** Creating a handle before
destroying the old one deadlocked in the new handle's first query (both
handles share the commander registered in the process-wide config default,
see the caveat in `CEmbeddedReasoner`). Recreate as destroy → create → load →
re-tell.

**Worker process prototype** (`Tools/KoncludeWorker/`, see its README).
Konclude runs in a child process (`konclude_worker`, fork + exec, socket
protocol); `KoncludeWorkerClient` tracks the current state and restarts the
worker every N changes, re-telling the state. Process exit frees everything,
leaks included.

- Equivalence: 150 random class/property changes, restart every 7, worker
  killed twice: answers identical to in-process.
- Growth workload above, 1000 changes: max worker RSS 175 / 302 / 538 MB with
  a restart every 50 / 100 / 200 changes (1.54 GB without), total time 17.7 /
  18.8 / 19.1 s (20.6 s without).

**Verified.**
- ASan: handle cycles (small, roberts), both state drivers, 100-state memory
  benchmark, 150-state growth benchmark with recreation every 40: 0 errors.
- Normal build: state/property drivers ×3, `embedded_cq_driver`, both probes,
  `embedded_state_benchmark`, `embedded_cq_benchmark 5 all`, memory benchmark
  (255 kB/state, unchanged), handle cycles, worker equivalence test and growth
  benchmark.
- CLI vs a `HEAD` build: all 5 request files give identical responses;
  classification closure identical (roberts, galen); realisation differs only
  in the random `PersonWithManySibling`.

**Status.** The `embedded_cq_benchmark` hazard above is gone (it now keeps
~13 MB per cycle on roberts). Open: the per-revision mallocs (they are also
most of the in-handle growth); until then, bound memory with the worker.

## Task 6: Powerlifted wiring. Methods and test done; search not wired yet

**Goal.** Give Powerlifted's search the methods to keep Konclude's ABox equal
to a search state and to answer mko queries, through the worker process
(Task 5c), and run them in a test there.

**Powerlifted side** (`../powerlifted`, not committed by this task):

1. *Build.* `build.py --konclude` (CMake `USE_KONCLUDE`) no longer links
   `libKonclude`/Qt. It compiles `Tools/KoncludeWorker/konclude_worker_client.cpp`
   into a `konclude_client` library and bakes in the worker path
   (`KONCLUDE_WORKER_PATH`, CMake `KONCLUDE_WORKER`, default
   `<KONCLUDE_REPO_ROOT>/konclude_worker`). The in-process wrapper
   `src/search/konclude_reasoner.h` is gone; `--use-konclude` now only
   starts a worker (smoke test in `main.cc`).
2. *`KoncludeStateReasoner`* (`src/search/konclude/state_reasoner.{h,cc}`):
   - `KoncludeVocabulary`: task predicate → class/property IRI, plus an
     object-IRI prefix. Unlisted predicates (`type@object`, `QUERY<i>`,
     `=`, …) are never sent.
   - `facts_of(state)` (static facts included), `diff(from, to)`,
     `set_state(state)` / `apply_diff(diff)`: one worker state change with
     only the diff; none if nothing changed.
   - `answer(query)`: certain answers as object-index tuples;
     `entailed(query, candidates)`: the candidates (e.g. upperbound minus
     lowerbound answers) that are certain answers.
   - `KoncludeQuery` has one SPARQL SELECT per connected component; the
     answers are their product, because Konclude answers only the first
     component of a disconnected pattern (see
     `docs/INCREMENTAL_REASONING.md` §4). An empty binding is an error, not
     a dropped row.
3. *Test.* `konclude_test TASK.lifted ONTOLOGY.ofn [WORKER] [RESTART_EVERY]`,
   run by `dev/test-konclude.py`. It runs on the translated team task
   with `dev/ontologies/team.ofn`, a functional-syntax copy of `team.owl`;
   the embedded build reads no Turtle. It checks:
   - the initial state's mko answers;
   - the plan (recruit red bob) (recruit red carol), as a diff and as
     `set_state`;
   - goal entailment, then a jump back to the initial state (2 retracts);
   - the disconnected-pattern error;
   - worker restarts.

**Verified.**
- `dev/test-konclude.py`: PASS with a restart every 0 / 1 / 2 / 3 changes.
  `Captain(alice)` is reported as a known issue (max-cardinality bug, §4).
- Powerlifted `dev/run-tests.py --minimal`: all pass with and without
  `--konclude` (needs `.venv/bin` and VAL's `validate` on `PATH`).
- `search --use-konclude` on team prints "Konclude worker smoke test: OK".
  The search then aborts with the existing "Actions with negated
  preconditions not supported yet" (team's `(not (mko …))`), with or
  without `--use-konclude`.

**Found** (not fixed; all three also occur on upstream `a7a80bc2`, so they
predate this branch; see `docs/INCREMENTAL_REASONING.md` §4):
- disconnected queries are answered only in part;
- a max-cardinality entailment is missed (`Tests/max-cardinality-realisation.ofn`);
- realisation hangs (`Tests/max-cardinality-realisation-hang.ofn`).

The hang matters for the worker: the client has no timeout yet, so a hung
query blocks the search.

## Task 5d: Why the per-state leak survives handle destruction. Investigated; A, B1, B2 (jobs), B3 and C fixed

**Question.** Each state change costs ~1.8 kB per fact in the state, and
`konclude_destroy_reasoner` gives none of it back (Task 5c). Where does it go?

**Method.**
1. *What survives.* `embedded_incremental_growth_benchmark N 2 50 0` (one
   handle, N changes, destroyed at the end) under the live-heap profiler
   (`Tools/HeapProfiler/`, 28-frame stacks, all sites); diff N = 100 vs 200.
2. *Whether it is reachable.* The same benchmark linked against
   `ReleaseEmbeddedAsan` with LeakSanitizer (`ASAN_OPTIONS=detect_leaks=1:
   alloc_dealloc_mismatch=0:fast_unwind_on_malloc=0 LSAN_OPTIONS=max_leaks=0`),
   N = 60/120/100/200, grouped by owner with `Tools/HeapProfiler/lsan_owners.py`.
3. *Causality.* Patch one suspected owner, rerun under ASan, and check that
   its leaks disappear and that ASan reports no use-after-free or double
   free (done for B1 below, then reverted).

**Findings.**
- *All of it is unreachable.* At 200 states, LSan reports 40.3 MB leaked;
  the profiler sees 39.8 MB live at exit. No reachable cache grows. The
  memory is lost while the handle is running, not at teardown:
  - when 5b deletes a superseded revision;
  - when a task's memory is released;
  - when a query or a precomputation item is deleted.

  So no teardown code can find it; only fixing the owners (or process exit)
  frees it.
- *Why it is quadratic.* Each change rebuilds the whole state
  (`IncrementalRebuild=false`, Task 1), and every rebuild allocates per
  individual. From 100 to 200 states the main sites grow by ~22,750 objects,
  1–3× the sum of the state sizes (1.5·s individuals at state s).
- *Three causes* cover 25.2 of the 27.0 MB growth from 100 to 200 states
  (LSan):

| Cause | MB | Where |
|---|---|---|
| A. Pool-allocated objects own heap memory; freeing the pools runs no destructors | 5.5 | see A |
| B. An owner is deleted, but its destructor doesn't delete what it owns | 15.7 | see B1–B4 |
| C. Temporary manager set to `nullptr`, so `new` is used and nothing deletes | 4.1 | see C |

**A. Pool-allocated owners** (`CObjectAllocator`/`CObjectParameterizingAllocator`
into a context's pools; the pools are dropped in bulk, e.g.
`~CConcreteOntology` → `delete mOntConctext` →
`~CConcreteOntologyContextBase` deletes the pool provider):
- Names: `CConcreteOntologyUpdateBuilder::updateName` (`.cpp:2029`, called
  per individual at `:2252`) puts `CAbbreviatedIRIName`/`CIRIName` (with a
  `QString`) into the revision's pools. 2.0 MB.
- Backend cache:
  - `CBackendRepresentativeMemoryLabelCacheItemIndividualAssociationMapExtensionData`
    has two plain `QMap`s (`.h:88-89`) and is allocated in cache pools
    (`CBackendRepresentativeMemoryCache.cpp:3535, 4195`). 1.8 MB.
- Active-entity sets: `CABox::getActiveIndividualSet` (`CABox.cpp:126`, likewise
  for TBox/RBox) puts a `CBOXSET` = `CQtSet` (plain Qt, `OntologySettings.h:402`)
  into box pools; `~CABox` doesn't destroy it. Filled by
  `CMappingUpdaterPreProcess::collectActiveOntologyItemsFromActiveEntities`. 1.6 MB.

**B. Destructors that skip owned objects:**
- B1, query results, 9.7 MB:
  - The finishing handler hands the result to the query
    (`CAbstractStreamingComplexQueryFinishingHandler.cpp:233`,
    `setQueryResult`).
  - `~CComplexAnsweringQuery` (`CComplexAnsweringQuery.cpp:39`) deletes only
    `mQueryStats`.
  - Every query's rows, binding copies and IRI strings (`getIRIName`)
    therefore outlive `delete query` in `CEmbeddedQueryManager`.
  - *Confirmed:* adding `delete mQueryResult` there removed all B1 leaks at
    100 states (3.3 MB → 0, total 12.0 → 8.7 MB), with no ASan error and
    all answers correct.
- B2, saturation jobs and data items, 2.5 MB:
  - `CTotallyOntologyPrecomputationItem::setSaturationCalculationJob`
    (`.cpp:747`) overwrites `mApproxSatCalcJob` without deleting it, and the
    destructor (`:176`) never deletes it, so the per-individual constructs
    from `CApproximatedSaturationCalculationJobGenerator::extendApproximatedSaturationCalculationJobProcessing`
    leak.
  - The destructor also frees neither the free list nor
    `mSatIDIndividualItemsHash` of `CSaturationIndividualDataItem`s
    (`new` at `:811`).
- B3, answerer instance hashes, 3.3 MB:
  - `COptimizedComplexConceptInstanziatedIndividualItemHash`es are created
    with `new` (`COptimizedComplexExpressionAnsweringHandler.cpp:8221, 8276,
    10778, 11245`) and held by concept items.
  - `~COptimizedComplexConceptItem` (`COptimizedComplexConceptItem.cpp:114`)
    is empty.
- B4, small, 0.07 MB:
  - `~CConcreteOntologyQueryExtendedBuilder` deletes `mExpContainer` but
    not the query expressions in `mSparqlIndividualSelectExpList`.
  - `~COptimizedComplexExpressionAnsweringHandler` doesn't delete
    `mJoinComputer`/`mPropagationComputer` (`.cpp:177-195`).

**C. Nulled temporary manager** (4.1 MB):
- `CSaturationNodeBackendAssociationCacheHandler::tryAssociateNodesWithBackendCache`
  (`.cpp:1364`, upstream since `c2dc60e8`) sets
  `calcAlgContext->mUsedTempMemMan = nullptr`.
- Every `CObjectParameterizingAllocator<…>(getUsedTemporaryMemoryAllocationManager(), …)`
  after it falls back to `new` (`CObjectParameterizingAllocator.h:74`): the
  sets and hashes at `.cpp:285, 684, 733, 767, 835, 845, 1366-1386`. Nothing
  deletes them.
- Likely the author's intent: the write data must outlive the task's
  temporary memory, which `handleTask` releases (`releaseAllMemory`) before
  the next task, while the backend cache reads it later.

**Fix options** (one change each, per CLAUDE.md "Task Scope"):
1. B1: `delete mQueryResult` in `~CComplexAnsweringQuery`. **Done**, see
   "B1 fix" below.
2. B2/B3/B4: complete the destructors (B3 **done**, B2 jobs **done**; see below). For B2, check that no running
   calculation still uses the job when the item is replaced or deleted.
3. C: **done**, see "C fix" below. It turned out that none of these
   structures reaches the cache, so they only need to live for one call.
4. A: **done** for its three groups, each in its own way; see "A fix" below.

Fixing 1–4 would remove ~93% of the measured growth; the rest (1.8 MB) is
spread over classification/realization sites below 0.2 MB each.

**B1 fix: the query deletes its result.** `~CComplexAnsweringQuery` now
deletes `mQueryResult`. Ownership checks before the change:
- Every `setQueryResult` caller passes a new result, or the query's own
  streaming result. The finishing handler skips `setQueryResult` when the
  query already holds one (`CAbstractStreamingComplexQueryFinishingHandler.cpp:232`).
- Readers finish before the query is deleted:
  - the embedded layer copies the rows first;
  - SPARQL streaming serializes in `handleQueryResult`, when the command
    closes; the command, and with it the query, is deleted later
    (`CGenerateQueryCommand`).
  - JNI uses other query classes.
- Redland subqueries can share their parent's streaming result
  (`COptimizedComplexCompositionRedlandRasqalAnsweringHandler.cpp:306`).
  They are never deleted, so there is no double free. Noted in the
  destructor; not testable here (no Redland build).

| Measure | Before | After |
|---|---|---|
| Growth, `embedded_incremental_growth_benchmark 300 2 100 0` | 616 kB/state | 498 kB/state |
| RSS after destroying that handle | 140.6 MB | 105.4 MB |
| LSan leaks at 100 states (B1 part) | 12.0 MB (3.3 MB) | 8.7 MB (0) |

**Verified.**
- Drivers: `embedded_cq_driver`, both probes, state and property drivers
  (3× each), `embedded_state_benchmark 50 10 5 5` (0 problems),
  `embedded_cq_benchmark 5 all`, `embedded_handle_cycle_benchmark small 5 10`
  (0 threads kept, ~1.1 MB/cycle), `konclude_worker_equivalence_test`.
- ASan: state, property and CQ drivers and the 100-state growth benchmark:
  no use-after-free or double free, 0 failures.
- CLI vs. a pre-fix build:
  - `sparqlfile` (default config, disconnected-query file): identical
    answers.
  - The 5 `Tests/incremental-*-request.xml` files: identical answer sets;
    only the order of the namespace attributes differs, which varies
    between runs.

**B3 fix: the answering item frees its concept items.**
- *Root.* `COptimizedComplexExpressionOntologyAnsweringItem::getComplexConceptItem`
  creates every `COptimizedComplexConceptItem` with `new` and lists it in
  `mComplexConceptItemContainerSet`. Nothing ever deleted one, so deleting
  an answering handler (5b, or teardown) lost all its items.
- *Change.*
  - The answering item's destructor deletes them (`qDeleteAll`).
  - `~COptimizedComplexConceptItem` deletes `mKnownInstanceItems`, which is
    created once per item (`initializeKnownPossibleInstances`; the other
    creation sites are a debug function and the unused
    `searchKnownPossibleInstances`) and only borrowed elsewhere (e.g. the
    finishing handler, for one query).
- *Left out.* The item's other pointer fields (super/sub class sets, instance
  sets, iterator, variable-concept items) are handed between fields and
  partly deleted by the handler, so their ownership needs its own analysis.
  The realizer's `COptimizedKPSetIndividualInstantiatedItemMultiHash`
  (~0.3 MB per 100 states) has a different owner; it was wrongly grouped
  under B3 above.

| Measure | B1 only | B1 + B3 |
|---|---|---|
| Growth, `embedded_incremental_growth_benchmark 300 2 100 0` | 498 kB/state | 468 kB/state |
| RSS after destroying that handle | 105.4 MB | 96.7 MB |
| LSan leaks at 100 states (instance hashes + concept items) | 8.7 MB (1.2 MB) | 7.4 MB (0) |

**Verified** as for B1:
- the drivers, both benchmarks, handle cycles (0 threads, ~1.1 MB per
  cycle) and the worker equivalence test pass;
- ASan state, property and CQ drivers and the 100-state growth run show no
  memory errors and 0 failures;
- the CLI `sparqlfile` answers and the 5 incremental request files give
  the same answers as the pre-B1 build.

**B2 fix (jobs only): delete a saturation job once its calculation is done.**
- *Root.* Each saturation job (`CTotallyPrecomputationThread`, five
  `getApproximatedSaturationCalculationJob` sites) was stored on the
  precomputation item (`setSaturationCalculationJob`) and never deleted,
  with its per-individual constructs.
- *Why deleting is safe.*
  1. `CConcurrentTaskCalculationManager::calculateJob` builds the task
     from the job synchronously
     (`CSatisfiableCalculationTaskFromCalculationJobGenerator`).
  2. The task copies values from the constructs and keeps only data the job
     doesn't own: linkings, individuals, and the adapters, which
     `~CApproximatedSaturationCalculationJob` doesn't delete.
  3. Neither `getSaturationCalculationJob` (item or callback event) is ever
     called, and every saturation job is submitted through
     `CPrecomputationThread::processCalculationJob`, so it gets a
     completion callback.
- *Change.* The saturation-callback branch of
  `CPrecomputationThread::processCustomsEvents` deletes the job after
  `precomputationTested`. The item's pointer is marked as not owning. It is
  not deleted in the setter, because jobs of one item can run in parallel.
- *Left out: the `CSaturationIndividualDataItem`s* (~0.25 MB per 100 states).
  - An item is the individual's reference linking. The task's saturation
    nodes point to it and it points back
    (`setIndividualProcessNodeForIndividual`).
  - Saturation tasks are retained until teardown (5c), and their data is
    shared across revisions (5b).
  - So freeing the items when a precomputation item is deleted needs the
    same reachability analysis as the retained saturation tasks.

| Measure | B1 + B3 | + B2 jobs |
|---|---|---|
| Growth, `embedded_incremental_growth_benchmark 300 2 100 0` | 468 kB/state | 447 kB/state |
| RSS after destroying that handle | 96.7 MB | 89.0 MB |
| LSan leaks at 100 states (jobs + constructs) | 7.4 MB (0.66 MB) | 6.7 MB (0) |

**Verified.**
- Normal build:
  - the drivers, both benchmarks and the worker equivalence test pass;
  - handle cycles keep 0 threads (small ~1.0 MB, roberts ~13 MB per cycle,
    unchanged).
- ASan, no memory errors:
  - state, property and CQ drivers;
  - the 100-state growth run;
  - `embedded_state_benchmark 30 10 5 5`;
  - roberts handle cycles.
- CLI vs. the pre-B1 build:
  - roberts and galen classification: identical subsumption closures;
  - roberts realisation: identical assertions (except the known-random
    `PersonWithManySibling`);
  - `sparqlfile` and the 5 incremental request files: same answers.

**C fix: scratch pools for `tryAssociateNodesWithBackendCache`.**
- *What the cache reads.* The cache processes the write data
  asynchronously (`ThreadedUpdateSynchronization` defaults to true, so
  `CBackendRepresentativeMemoryCache::writeCachedData` posts an event).
  - The write data and its linkers are allocated in the message pools
    (`mMemAllocMan`, `CBackendAssociationCacheHandler::prepareCacheMessages`).
  - On commit, those pools go to the cache with the data.
  - Label data copies cache values; it doesn't keep the handler's
    structures.
- *What leaked.* Everything allocated through the used temporary manager in
  this call tree is scratch, used only until the call returns:
  - the member hashes and lists set up at its start;
  - the role sets and hashes in the `determine*`/`get*` helpers (in both
    handler files);
  - role and assertion linkers, nominal connection data,
    `CCacheValueTmpLabelReferenceData`.

  Their container nodes already lived in task memory
  (`getUsedTaskProcessorContext()` → the task-handle manager).
- *Change.* A local `CTaskHandleMemoryPoolAllocationManager scratchMemMan`
  replaces the `nullptr`:
  - it takes its pools from the task's temporary manager and returns them
    when it goes out of scope;
  - the used manager is reset to `nullptr` before the return, as before,
    for the rest of the task.
- *Poisoning experiment* (temporary, reverted). A subclass filled every
  scratch pool with `0xAB` before releasing it, so any later read would see
  garbage. With it:
  - all drivers, both benchmarks, roberts handle cycles, the growth run and
    the worker equivalence test passed;
  - CLI roberts realisation and roberts/galen classification were identical
    to the pre-fix build.

| Measure | B1 + B3 + B2 | + C |
|---|---|---|
| Growth, `embedded_incremental_growth_benchmark 300 2 100 0` | 447 kB/state | 385 kB/state |
| RSS after destroying that handle | 89.0 MB | 70.7 MB |
| Time per state at state 300 | 11.3 ms | 10.6 ms |
| roberts handle cycle, kept per cycle | ~13 MB | ~11.3 MB |
| LSan leaks at 100 states (association handler) | 6.7 MB (1.46 MB) | 5.2 MB (0) |

**Verified** as for B2 (normal build, ASan incl. roberts cycles and the
state benchmark, CLI vs. the pre-B1 build). Note on `sparqlfile`: it
streams results in parts, and which query's block a part lands in varies
between runs of the same binary. So compare the set of result rows over the
whole file (identical, 8 rows), not per block.

**A fix: heap members of pool-allocated objects.** Each of the three groups
has its own fix:
- *A1, names.* A rebuild gives every individual a new name object, whose
  abbreviated name was a new copy (`name.mid(...)`).
  - Change: `CConcreteOntologyUpdateBuilder::updateName` takes an optional
    source item. `getIndividualForIndividualTerm` passes `prevIndi`, the
    same individual in the previous revision.
  - If that item has an equal abbreviated name (same prefix string), the
    new name shares its `QString` data, so no allocation per state. The
    shared data stays alive, bounded by the number of distinct names.
- *A2, backend cache maps.* The two `QMap`s of
  `CBackendRepresentativeMemoryLabelCacheItemIndividualAssociationMapExtensionData`
  lived in ontology-context pools, released without destructors.
  - Why not pool-backed maps: the indexing tasks
    (`indexIndividualLabelAssociations`) build these objects with a
    stack-local context, so a pool-backed map would keep a dangling context
    for later updates.
  - Instead, `CBackendRepresentativeMemoryCacheOntologyContext` records the
    extension data allocated in it, under a mutex:
    - `getIndividualAssociationsExtensionData` registers each object;
    - the indexing tasks register theirs together with their pools.
  - `destroyOwnedExtensionData()` runs their destructors before the
    context's pools are released, both in
    `deleteUnreferredReleasedOntologyContexts` and at teardown.
  - That is safe because a context is only released once all data versions
    of its ontology are gone, and the label items holding the extension
    data live in the same context.
- *A3, active individual set.* `~CTBox` and `~CRBox` destroy their active
  sets, `~CABox` didn't. It now does (`COPADestroyAndRelease`).
  `referenceABox` never shares the pointer.

| Measure | before A | + A |
|---|---|---|
| Growth, `embedded_incremental_growth_benchmark 300 2 100 0` | 385 kB/state | 314 kB/state |
| RSS after destroying that handle | 70.7 MB | 50.4 MB |
| LSan leaks at 100 states | 5.2 MB | 3.4 MB |
| of which names / cache maps / active sets | 663 / 613 / 551 kB | 38 / 0 / 2 kB |
| handle cycles kept (small / roberts) | ~0.95 / ~11.3 MB | ~0.87 / ~10.9 MB |

**Verified** as for C:
- ASan: state, property and CQ drivers, the growth run, the state
  benchmark and roberts cycles show no memory errors.
- Normal build: drivers, benchmarks and the worker equivalence test pass;
  time per state is unchanged.
- CLI vs. the pre-B1 build:
  - roberts realisation: same assertions;
  - roberts/galen classification: same closures;
  - `sparqlfile`: same rows;
  - the 5 incremental request files: same answers.

**Where task 5d stands.** From the start of 5d to now:
- growth 616 → 314 kB/state;
- RSS after destroy 140.6 → 50.4 MB;
- LSan leaks at 200 states 40.3 → 6.7 MB.

What remains, all smaller:
- the saturation data items (B2, 0.25 MB per 100 states, need the
  reachability analysis);
- the realizer's KPSet multi-hash (~0.3 MB);
- the occurrence-statistics vectors (~0.24 MB);
- many classification/realization sites below 0.2 MB each.

The in-run growth that is not a leak (retained saturation tasks and cache
write pools, freed at teardown) is unchanged.

**Re-measured after all 5d fixes** (`embedded_incremental_growth_benchmark
1000 2 100 <N>`, `konclude_worker_growth_benchmark 1000 <N>`,
`embedded_memory_benchmark` defaults):

| Workload | Before 5d | After 5d |
|---|---|---|
| In-process, no recreation: RSS at 100 / 500 / 1000 states | 67 MB / 461 MB / 1.54 GB | 55 / 211 / 543 MB |
| In-process, recreate every 50: RSS at 1000 states | 1.11 GB | 167 MB |
| In-process, recreate every 250: RSS at 1000 states | 1.22 GB | 280 MB |
| RSS after the final destroy (1000 states, any N) | ~1.1 GB | ~120 MB |
| Worker max RSS, restart every 50 / 100 / 200 / never | 175 / 302 / 538 / 1540 MB | 84 / 121 / 191 / 544 MB |
| Worker total time (same four) | 17.7 / 18.8 / 19.1 / 20.6 s | 15.9 / 15.0 / 15.6 / 16.5 s |
| `embedded_memory_benchmark` (500 states, fixed size) | 255 kB/state | 199 kB/state |

- Recreating the handle now bounds memory to a large degree, which it did
  not before.
- About 90 MB of RSS above the start still stays after the last destroy.
  That is the remaining leak plus allocator retention: LSan finds 6.7 MB
  lost after 200 states.
- So the worker process remains the reliable bound.
