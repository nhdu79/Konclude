# Incremental Reasoning over Many ABoxes (Powerlifted) — Status and Next Round

**Entry point for current work.** Powerlifted (`../powerlifted`) needs to
check many search states (ABoxes) against one fixed TBox. It does this
through the embedded C API
(`Source/Control/Interface/Embedded/konclude_embedded.h`). The plan follows
the Konclude author's advice in `TODO.md` (e-mail from Andreas Steigmiller).
How the interface works internally, and how to build, test and link it:
`docs/EMBEDDED_INTERFACE.md`.

## 1. What the author said (summary)

| Topic | Advice |
|---|---|
| Interface | Skip OWLlink XML. Build the same command objects that `COWLlinkQtXMLCommandParser` builds and pass them to `delegateCommand(...)` directly. |
| Option A: incremental Tell/Retract | Works today: one KB, and per state you retract the old facts, tell the new ones, then query. Catch: every revision is kept, so memory grows. |
| Option B: branching | Internally the KB is a tree of revisions, but OWLlink doesn't expose it. Reason over the TBox once, branch per ABox, query, then release the branch. Catch: release/cleanup is incomplete, so memory leaks and reasoning may slow down. |
| Correctness | The latest version has a completion-graph caching bug that can give wrong results ([konclude/Konclude#19](https://github.com/konclude/Konclude/issues/19)). |

## 2. Current state (Option A wired up)

| C API | Does |
|---|---|
| `konclude_create_reasoner` / `konclude_destroy_reasoner` | One commander per handle; always ≥2 worker threads |
| `konclude_load_ontology_file` | Load and install the TBox (+ any ABox); only once per handle |
| `konclude_tell_and_retract_assertions(classTell[], classRetract[], propTell[], propRetract[])` | One state change: create a revision, retract then tell `ClassAssertion` and `ObjectPropertyAssertion` facts, install it |
| `konclude_tell_and_retract_axioms(tell[], retract[])` | Same, class assertions only (kept for existing callers) |
| `konclude_execute_conjunctive_query(sparql)` + `konclude_query_result_*` | Single-BGP SPARQL SELECT against the installed head |
| `konclude_check_consistency` / `konclude_check_satisfiability` | Boolean queries |

Powerlifted uses them through the worker process
(`Tools/KoncludeWorker/`, whose client mirrors these calls) and
`KoncludeStateReasoner` (§5, task 6). Driver results (`Tools/EmbeddedDriver/rebuild.sh`):

| Driver | Result |
|---|---|
| `embedded_cq_driver`, `embedded_state_existing_abox_probe`, `embedded_scratch_revision_probe` | PASS |
| `embedded_state_driver` | PASS, 10/10 runs (since the §3 fix) |
| `embedded_state_benchmark 50 10 5 5` | 0 problems over 50 state switches and 250 queries (since the §3 fix) |
| `embedded_property_driver` | PASS, 10/10 runs: property tell/retract, reasoning via domain and `∃hasChild.Person`, invalid call rejected |

## 3. Fixed bug: answers went wrong after tell → query → tell → query

**Status: fixed (2026-10-06, commit `b413d14d`).** The
fix is collision-free individual IDs in `CConcreteOntologyUpdateBuilder`;
change and verification are in `docs/INCREMENTAL_WORK_LOG.md`, Task 1.
The description below is kept for reference.

The bug was in the reasoner core, not the embedded layer: the stock CLI shows
it too. Repro: `./Release/Konclude owllinkfile -w 2 -i Tests/<fixture> -o out.xml`.
The TBox is `SubClassOf(Father, Person)` and Q is `GetInstances(Person)`.

| Fixture | Requests | Expected | Got |
|---|---|---|---|
| `incremental-tell-query-tell-request.xml` | Tell `Father(john)`; Q; Tell `Person(mary)`; Q; Tell `Father(bob)`; Q | `{john}`, `{john,mary}`, `{john,mary,bob}` | `{john}`, `{mary}`, `{bob}` |
| `incremental-tell-tell-query-request.xml` | same three Tells, one Q at the end | `{john,mary,bob}` | correct |
| `incremental-retract-retell-request.xml` | Tell john; Q; Retract john + Tell mary; Q; Tell john again; Q; `GetInstances(Father)` | …, `{john,mary}`, `{john}` | …, `{mary}`, `{}` |

Once a query has run, later queries return only the individuals touched by
the newest revision. Through the embedded CQ path, which individual goes
missing also changes from run to run. Changing the thread count (`-w 1/2/4`)
or setting `Konclude.Ontology.Revision.IncrementalRebuild=false` doesn't
change anything.

**Root cause** (confirmed 2026-10-06 with instrumentation; details in
`docs/INCREMENTAL_WORK_LOG.md`, Task 1):

1. **Full rebuild per queried revision.** `Konclude.Ontology.Revision.IncrementalRebuild`
   defaults to `false`, so each queried revision is rebuilt from scratch:
   `CRequirementConfigPreprocessingThread::countActiveEntites()` resets its
   data boxes, including the ABox, to the empty basement ontology.
2. **Old individuals found by name.** `CConcreteOntologyUpdateBuilder::getIndividualForIndividualTerm()`
   still finds individuals from earlier revisions through the ontology's
   name → individual map, which the reset doesn't clear, and reuses their
   old IDs.
3. **New individuals numbered from 0.** `buildIndividualIDs()` numbers new
   individuals from `aBox->getIndividualCount()`, which is 0 after the reset.
4. **IDs collide.** Reused and new IDs collide, and so do reused IDs among
   themselves. Only one individual per colliding ID ends up in the ABox.
   The backend cache and realizer then correctly reflect that broken ABox.

An earlier analysis, made through the old embedded API, put the root cause
in the backend cache (`CBackendRepresentativeMemoryCache`, keyed by
`ontologyID`). That was a downstream effect. `IncrementalRebuild=true` is no
workaround: it segfaults on these fixtures.

**Fix:** option A in `docs/INCREMENTAL_WORK_LOG.md` (collision-free individual IDs).

**Already ruled out:**

- Turning off all 16 `…CacheRetrieval/Writing` / `RepresentativeBackendCache.*`
  config flags.
- A consistency check before or after the tells: it uses the same
  incremental, changed-only dispatch.
- Telling again into one installed revision: later tells never get built.
- Installing the query revision too.
- Any revision scheme at the Control layer: every install still gets a new
  ID.

## 4. Other known reasoner bugs that matter here

All are pre-existing and not fixed. They need their own tasks.

- **Realizer use-after-free and one-thread-per-ontology leak. Fixed**
  (2026-10-06, commit `89bfea6f`). The realizer, preprocessor, precomputator and
  classifier threads are now shared across ontologies; see
  `docs/INCREMENTAL_WORK_LOG.md`, Task 2. Their per-ontology items are still
  never freed (task 5).
- **Stale role-assertion destinations after a rebuild. Fixed** (task 3, `a873242e`).
  An individual found through the name map was used without a local copy
  when it was only a role-assertion destination, so the assertion linked to
  the previous revision's object. Property facts and inferences over them
  became random after state changes. See `docs/INCREMENTAL_WORK_LOG.md`, Task 3.
- **Answerer crash on a repeated property query. Fixed** (task 3, `160f90ae`).
  `COptimizedComplexExpressionAnsweringHandler::tryReuseVariableRolePropagationItem`
  dereferenced a missing item set (e.g. `?y` in `?x p ?y`) when reusing an
  item from an earlier query.
- **Revisions never freed. Mostly fixed** (task 5b). The embedded layer
  releases and deletes superseded revisions. Their precomputation saturation
  tasks stay (~150 kB/state), since later revisions share them through the
  TBox concepts. `CReleaseKnowledgeBaseCommand` still only removes the name
  mapping. `konclude_destroy_reasoner` frees a handle's threads and
  reasoning data (task 5c), but not what state changes leak.
- **Nondeterministic realisation of `PersonWithManySibling`** (roberts-family,
  `realisation -w 2`). Which individuals get this type changes from run to
  run, with or without the §3 fix. It is the only nondeterministic
  assertion out of 5,419. It may be related to cardinality reasoning
  (issue #19 is a cardinality completeness bug).
- **Disconnected queries answered only in part** (found in task 6). For a
  basic graph pattern with several connected components, e.g.
  `SELECT ?t ?p WHERE { ?t a :Team . ?p a :Player }`, each answer row binds
  only the first component's variables; the others have no cell, so
  `konclude_query_result_binding` returns `""` for them. The embedded copy
  (`CEmbeddedQueryManager.cpp`, the loop over
  `getVariableBindingsAnswersIterator()`) copies the rows as the answerer
  built them, so the cause is in the answerer. The CLI (`sparqlfile`,
  default config) is wrong too: it returns one-cell rows binding `?t` to
  the teams and the players. Pre-existing: upstream `a7a80bc2` gives the
  same CLI output. Workaround: query each component separately and take the
  product (Powerlifted's `KoncludeStateReasoner` does).
- **Max-cardinality entailments missed, or realization hangs** (found in
  task 6, also with the CLI `realisation -w 2`).
  - `Tests/max-cardinality-realisation.ofn` (the team ontology plus
    `hasCaptain(red, alice)`): `Team ⊑ ∃hasCaptain.Captain`,
    `Team ⊑ ≤1 hasCaptain.Player` and range `Player` entail
    `Captain(alice)`, but neither realisation nor a CQ returns it.
    Consistency checking is complete here: adding `Substitute(alice)` makes
    the ontology inconsistent.
  - `Tests/max-cardinality-realisation-hang.ofn` (the same without the min
    cardinality, union, disjointness and `hasMember` axioms): realisation and
    the first CQ never finish; consistency checking does.
  - Pre-existing: upstream `a7a80bc2` (built with the `if (concept)` typo
    fix of `056334ad`) misses `Captain(alice)` and hangs the same way.
  - Possibly related to the cardinality bug of issue #19.
- **CLI `sparqlfile` side issues.** These were found while trying to
  reproduce §3 via SPARQL.
  - `INSERT`/`DELETE` without a graph clause read `{` as the graph name
    (`CSPARQLKnowledgeBaseSplittingOperationParser.cpp:214`).
  - `LOAD … INTO <new-iri>` doesn't create the KB.
  - `COptimizedComplexExpressionAnsweringHandler::identifyNonInstantiation()`
    (`Reasoner/Answerer/…AnsweringHandler.cpp:2942`, config
    `Konclude.Answering.UnsatisfiableNonInstanceStatisticsChecking`) read
    zero occurrence statistics for fresh revisions, so it emptied results.
  - With that check off, realization hung.
  - With `-c Configs/querying-config.xml`, even a plain `LOAD` + `SELECT`
    hangs after "Scheduling processing of (realization) requirements"
    (upstream `a7a80bc2` too); the default config answers.

## 5. Next round — tasks in order

Each task is its own change (see CLAUDE.md "Task Scope").

1. **Fix §3. Done** (`b413d14d`).
   - Start from the three `Tests/incremental-*-request.xml` fixtures.
   - No upstream fix exists (checked 2026-10-06). Root cause found; the fix
     progress notes are in `docs/INCREMENTAL_WORK_LOG.md`.
   - Done when `embedded_state_driver` and `embedded_state_benchmark` pass on
     repeated runs.
2. **Fix the realizer use-after-free and the thread leak** (§4). **Done**
   (`89bfea6f`). The thread count is now flat; memory is still task 5.
3. **More fact types and queries.**
   - `ObjectPropertyAssertion`: **Done** (`konclude_tell_and_retract_assertions`).
     Along the way, two core bugs were fixed (§4).
   - Not done (optional): instance/type queries without SPARQL. Build what OWLlink
     `GetInstances`/`IsInstanceOf` builds
     (`COWLlinkQtXMLCommandParser::parseProcessQueryNode` →
     `CProcessQueryOWL2XMLNodeCommand`, handled in
     `COWLlinkProcessor::processCustomsEvents`).
4. **Branching (Option B). Deferred**: we use Option A for now. Kept as a
   possible experiment in §6.
5. **Memory cleanup.** Mostly done; see `docs/INCREMENTAL_WORK_LOG.md`, Task 5.
   - 5a (quadratic change history) and 5b (releasing and deleting
     superseded revisions and query layers) are done: ~255 kB per queried
     state (was ~1,300 and rising).
   - Left: the precomputation saturation tasks (~150 kB/state; freeing them
     needs a reachability analysis over saturation nodes shared through the
     TBox concepts).
   - 5c (Task 5c in the work log): `konclude_destroy_reasoner` now frees
     almost all of a handle (threads, task memory, caches). But
     each state change also leaks memory that survives destruction (~1.8 kB
     per fact in the state), so recreating the handle does not yet bound
     memory. Never keep two handles alive at once (deadlock).
   - 5d (Task 5d in the work log): why that leak survives destruction. All
     of it is already unreachable during the run, from three causes:
     pool-allocated objects with heap members, destructors that skip owned
     objects (query results are the largest), and a nulled temporary memory
     manager in the backend association handler. Fix options are listed
     there. Fixed so far: query results (B1), answerer concept items (B3),
     saturation jobs (B2) and backend association scratch (C),
     616 → 385 kB/state.
   - Bounding memory now: run Konclude in a worker process restarted every N
     changes (`Tools/KoncludeWorker/`, prototype; 175 MB max at N = 50 on the
     1000-change growth workload).
6. **Powerlifted wiring. Methods done, search not wired yet**; see
   `docs/INCREMENTAL_WORK_LOG.md`, Task 6.
   - `../powerlifted/src/search/konclude/state_reasoner.h`
     (`KoncludeStateReasoner`): turns a `DBState` into the diff to the facts
     Konclude holds and answers mko queries, through the worker process
     (5c). `konclude_test` (`dev/test-konclude.py`) runs it on the team task.
   - Missing, on the Powerlifted side: the translator must write the
     vocabulary (predicate → IRI), the ontology in OWL 2 XML or functional
     syntax, and each mko's query (one SPARQL SELECT per connected
     component); the search must call `set_state`/`entailed` when an mko's
     lowerbound and upperbound answers differ.
   - Consider caching answers on the Powerlifted side, since search revisits
     similar states.
   - Blocking correctness for some ontologies: the max-cardinality bug (§4).

## 6. Later experiment: branching (Option B)

Not planned now; worth trying if Option A's per-state cost or memory growth
becomes the bottleneck.

- **Idea.** Reason over the TBox and the shared facts once, then give each
  state its own branch on top of that base instead of mutating one head.
  Query the branch, then release it. This avoids retract diffs and the
  revision chain growing per state.
- **Entry point:** `Reasoner/Revision/CSPOntologyRevisionManager`.
  - `nameIDHash` maps a KB name to a revision path.
  - `revisionHash` maps a revision path to its head.
  - `onRevContainer` holds every installed revision.
  - Each `COntologyRevision` knows its predecessor and its depth.
- **Possible API:**
  - `konclude_branch_create(base)`: a new name/path whose head is a revision
    on top of `base`;
  - tell/retract/query on a branch;
  - `konclude_branch_release(branch)`.
- **Risks:**
  - Install merges when `nextRevDepth > revDepth+1` (install branch of
    `CSPOntologyRevisionManager.cpp`).
  - `CReleaseKnowledgeBaseCommand` only removes the name mapping.
  - Without task 5, released branches are never freed (the author's
    warning).
  - Branches would exercise the same rebuild paths fixed in tasks 1 and 3,
    so rerun their request files.
