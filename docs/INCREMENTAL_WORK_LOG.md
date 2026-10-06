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
