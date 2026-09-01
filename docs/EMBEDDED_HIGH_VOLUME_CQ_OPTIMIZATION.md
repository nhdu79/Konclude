# High-Volume CQ Answering: Data-Path Optimization Plan

Companion to `docs/FASTDOWNWARD_EMBEDDING.md` (the master design doc for
embedding Konclude into Fast Downward) and
`docs/EMBEDDED_EXECUTE_CONJUNCTIVE_QUERY.md` (the CQ pipeline's internals).
This doc covers a narrower, later-stage question that came up once the base
embedded CQ pipeline was working end to end: FD's precondition/effect checks
call conjunctive-query answering **millions of times** during search, so
per-call *transport*/*construction* overhead — everything other than the
tableau reasoning itself — has to be optimized separately from correctness.

**Read this alongside `docs/FASTDOWNWARD_EMBEDDING.md` — do not treat it as
a replacement.** That doc already designed and partially validated the
architecture this doc ends up recommending (see Decision 2 below); this doc
exists because that design was made in a different phase of the project and
its consequences for revision-chain memory/CPU growth had not been traced
through in detail. Two places below explicitly correct assumptions made
earlier in *this* session's conversation, not in the older doc.

## Context and goal

- FD's precondition/effect checks each correspond to one successor-state
  generation, answered as one or more conjunctive queries against a DL
  ontology.
- `Tools/EmbeddedDriver/embedded_cq_benchmark.cpp`'s two scenarios establish
  that for CQ answering *comparable to or easier than* `robot-grid`
  (large TBox via `SubClassOf` chains, tiny ABox — one individual), the
  actual reasoning cost is small: ~29 ms of a ~39 ms cycle, per
  `docs/EMBEDDED_CQ_DRIVER.md`'s "Measured numbers" section. FD's queries
  are expected to be in this range or easier.
- **Explicit scope decision: the tableau/query-calculation step itself is
  not a target of this optimization.** What's being optimized is everything
  *around* it — reasoner lifecycle, ontology construction, query
  construction — collectively "the data path," because at millions of
  calls, fixed per-call overhead that's individually cheap becomes the
  dominant cost even when each individual reasoning step is easy.

## Decision 1: one persistent reasoner handle, not one per call

- **Problem.** `konclude_create_reasoner()` / `konclude_destroy_reasoner()`
  spin up and tear down a full `CCommanderManagerThread` (hard-enforced
  `>=2` processing threads, `konclude_embedded.h`'s documented
  workaround for the `-w 1` startup deadlock) on every call. This is fixed
  overhead — thread creation/teardown, config loading — independent of how
  easy the query is, paid once per FD call instead of once per FD run.
- **Decision.** FD keeps a single `KoncludeReasonerHandle` alive for the
  entire search process instead of one per query.
- **Why this is already supported.** `konclude_embedded.h`'s own
  thread-safety comment documents "sequential use of separate handles, one
  at a time" as supported; using *one* handle sequentially for millions of
  calls is the same usage pattern, just without the destroy/recreate in
  between. No new API surface needed for this part.
- **Residual risk, not yet exercised.** Every existing use of the embedded
  interface (this session's driver/benchmark) creates a handle, does a
  bounded amount of work, and destroys it — nobody has run it as a
  long-lived process handling millions of sequential calls. `Source/Context/`'s
  pooled allocators have never been exercised under sustained reuse inside
  one long-lived instance before (this exact caveat is already called out
  in `docs/FASTDOWNWARD_EMBEDDING.md` phase 7). Worth a dedicated soak test,
  not just a correctness check.

## Decision 2: don't reparse the whole ontology file per call

**Current design (implemented and verified): classified-once base ontology
+ one revision per FD state, installed lazily once per state — not once
per CQ call, and not never-installed.** This corrects the design this doc
previously recommended (assert into a scratch revision and never install
it at all). The never-install version turned out not to work once real
ABox facts are involved; see "Why installing turns out to be required"
below for what was actually found.

- **Mechanism, as implemented in `CEmbeddedReasoner`.** Load and classify
  the TBox+RBox exactly once, at startup, as a persistent base ontology.
  `beginNewState()` builds one `COntologyRevision` via
  `CCreateKnowledgeBaseRevisionUpdateCommand`, wrapping the *current* base
  ontology. `assertClassFact()` (and, once built, its property-fact
  sibling) Tells that state's ABox facts directly through axiom-builder
  calls — `CConcreteOntologyUpdateCollectorBuilder`, the exact class every
  file-based loader in this codebase already uses to Tell axioms, not
  OWL/XML or SPARQL-UPDATE text. The **first** `executeConjunctiveQuery()`
  call for that state installs this revision via
  `CInstallKnowledgeBaseRevisionUpdateCommand` (lazily, once), then builds
  and runs the query against a **second**, fresh, never-installed revision
  layered on top of the now-installed state — mirroring
  `COWLlinkProcessor.cpp`'s real SPARQL Tell/Query handling exactly (Tell →
  Install → then each subsequent query creates and discards its own
  short-lived revision on top). Every later query against the same state
  reuses the already-installed base and only pays the cheap per-query
  revision cost, not another install. Moving to the next state
  (`beginNewState()` again) discards the previous one: a plain `delete` if
  it was never installed (no query ever ran against it), or just dropping
  the pointer if it was — `CSPOntologyRevisionManager`'s `onRevContainer`
  owns it from that point on.
- **Why installing turns out to be required, not optional.** Every Tell
  path that exists anywhere in this codebase installs the Told revision
  immediately, before anything ever queries it — see
  `COWLlinkProcessor.cpp`'s SPARQL `UPDATE`/`INSERT DATA` handling
  (`CCreateKnowledgeBaseRevisionUpdateCommand` → Tell →
  `CInstallKnowledgeBaseRevisionUpdateCommand`, immediately, before any
  query touches the result). There is no precedent anywhere in Konclude
  for querying a Told, never-installed revision directly. This was
  confirmed empirically, not just by reading the proven code path: forcing
  `OPSBUILD` (the processing step that materializes Told axioms into the
  internal ABox/TBox node structures the tableau engine and query
  answering actually read) directly on a never-installed revision looked
  plausible on paper — `CPrepareKnowledgeBaseCommand` with an explicit
  `OPSBUILD` requirement, dispatched the same way the existing
  triples-indexing precondition already is — but returned **zero** query
  rows, including for ABox content that predated the Tell and had queried
  correctly moments earlier with no Tell involved at all. Installing
  first, then querying via a fresh layered revision, fixed it immediately
  and reproducibly.
- **The cost this reintroduces, and why it's still much better than
  installing every call.** Installing means `onRevContainer` now retains
  one revision per **FD state** (freed only in the manager's own
  destructor) and pays the `referenceBuildData()` O(N) copy once per
  state, not zero times as the original never-install design assumed. The
  difference from the rejected "install every CQ call" alternative is
  purely frequency: FD's flow is one ABox → potentially many CQs per
  state, so paying this cost once per state — not once per query, the
  actual millions-of-calls hot path — keeps growth tied to *state* count,
  not *query* count. This is a real, load-bearing cost now, not eliminated
  by construction the way the original plan claimed; whether per-state
  growth over a long FD run is acceptable is unmeasured and belongs in the
  benchmark step below.
- **TBox/RBox structural sharing is unaffected by any of this and still
  holds.** `CTBox::referenceTBox`/`CRBox::referenceRBox` share the TBox/RBox
  by pointer, not copy, confirmed via
  `CDynamicReferenceVectorBase::referenceVector`
  (`Utilities/Container/CDynamicReferenceVector.cpp:147-160`) being O(1)
  pointer-aliasing. Only the *ABox* revision gets installed per state; the
  classified TBox/RBox stays shared and untouched.
- **Alternative considered and rejected: install every CQ call**, not just
  once per state — effectively what `sparqlserver`/`owllinkserver` mode
  does across independent HTTP requests with no state grouping. Rejected
  because at FD's millions-of-calls scale, installing per call instead of
  per state means `onRevContainer` grows with call count instead of state
  count, and the `referenceBuildData()` copy cost compounds the same way —
  O(N) per call, O(N²) total across *N* calls — for no benefit once
  queries are already naturally grouped by state via `beginNewState()`.

## Decision 3: query construction — correcting an assumption made earlier this session

- **What was proposed earlier in this conversation.** Reuse a single parsed
  `CQuery*` across many calls via `CGetQueryDependentKnowledgeBaseRevisionUpdatesCommand`
  / `CInstallQueryDependentKnowledgeBaseRevisionUpdatesCommand`
  (`Control/Command/Instructions/`, consumed at
  `COWLlinkProcessor.cpp:936-975`), to skip re-parsing SPARQL text on every
  call.
- **This doesn't work, and `docs/FASTDOWNWARD_EMBEDDING.md` already found
  out why**, in its "CQ design revision: bypass SPARQL text entirely"
  section (lines 487-578). `CComplexAnsweringQuery`
  (`Reasoner/Query/CComplexAnsweringQuery.h:61-99`), the actual CQ query
  class, stores its target ontologies (`mOntology`, `mExpressionsOntology`)
  as plain member pointers set **only** by the constructor — no setter
  exists anywhere in its public interface. A query object is permanently,
  structurally bound to the specific `CConcreteOntology*` pair it was built
  against. There is no way to point an existing `CQuery*` at Decision 2's
  scratch ontology for a different call. **Correction: this invalidates the
  query-reuse idea proposed earlier this session — a fresh `CQuery*` must
  be built every call.** Decision 2's benefit is about avoiding *ontology*
  reparse; query construction is always a separate per-call cost on top of
  it.
- **What's still worth optimizing on the query side: SPARQL *text* parsing
  specifically**, independent of the fact that a fresh `CQuery` object is
  unavoidable either way. `docs/FASTDOWNWARD_EMBEDDING.md` already sketched
  an atom-by-atom construction API (`konclude_query_pattern_*`, lines
  551-578 there) that builds `CQuery` objects directly via the same
  axiom/expression-builder primitives (`COntologyBuilder` family) used
  elsewhere, bypassing `CSPARQLSimpleQueryParser`'s tokenizer entirely.
- **Open reconciliation, not resolved by this doc: what actually shipped
  this session contradicts that plan.** `konclude_execute_conjunctive_query()`
  (implemented and verified this session — see
  `docs/EMBEDDED_EXECUTE_CONJUNCTIVE_QUERY.md`) takes a raw SPARQL string
  and reparses it via `CSPARQLSimpleQueryParser::parseQueryText()` on every
  call — precisely the cost the older doc's atom-based design was meant to
  avoid. Whether that reparse cost actually matters at FD's scale is
  **unmeasured**: every timing so far
  (`docs/EMBEDDED_CQ_DRIVER.md`'s benchmark) bundles SPARQL-text parsing
  together with triples-indexing and tableau calculation in one `query`
  phase number, so the text-parsing component has never been isolated.
  This needs to be measured (see below) before deciding whether to build
  the atom-based API or keep the shipped SPARQL-string one.

## Decision 4: status of the dispatch-hang bug this design was originally blocked on

- `docs/FASTDOWNWARD_EMBEDDING.md`'s own status line records that its
  snapshot-per-state design (Decision 2 above) was fully worked out at the
  source level but **empirically untested**, blocked by a real bug: any
  command dispatched via `CPreconditionSynchronizer` wrapping a raw
  `CCommanderManager`/`CCommanderManagerThread` — exactly
  `CEmbeddedReasoner`'s original dispatch pattern — hung indefinitely on
  every command past initial KB creation (reproduced for
  `CIsConsistentQueryCommand`, `CLoadKnowledgeBaseOWLAutoOntologyCommand`,
  and `CCreateKnowledgeBaseRevisionUpdateCommand` specifically — the exact
  command Decision 2's scratch-revision mechanism depends on).
- **This session's earlier, separately-motivated debugging work likely
  already fixed it.** The permanent hang in `loadOntologyFile()` was traced
  to a missing `COWLlinkProcessor` in the dispatch path and fixed by adding
  `CEmbeddedOWLlinkProcessor` (see `docs/EMBEDDED_CQ_DRIVER.md`'s "Fixed
  bugs found via this driver" section) — the same symptom, on overlapping
  command types, as the bug this older doc describes.
- **Confirmed for the never-install pattern specifically.** Added
  `CEmbeddedReasoner::probeScratchRevisionCycles()`
  (`Source/Control/Interface/Embedded/CEmbeddedReasoner.cpp`) and its C API
  wrapper `konclude_probe_scratch_revision_cycles`
  (`konclude_embedded.h`/`CEmbeddedInterfaceCAPI.cpp`), exercised by
  `Tools/EmbeddedDriver/embedded_scratch_revision_probe.cpp`: repeatedly
  builds a scratch `COntologyRevision` via
  `CCreateKnowledgeBaseRevisionUpdateCommand` through
  `mPreconditionSynchronizer`, never installs it, and `delete`s it, with a
  per-iteration stderr progress line so a hang would show exactly how far
  it got when killed by a shell-level timeout. Run under `timeout 30`/
  `timeout 60`: **10/10 cycles completed against `Tests/robot22.owl.xml`,
  30/30 against `Tests/roberts-family-full-D.owl.xml`, both in well under
  the timeout, no hang.** This does not assert any ABox facts onto the
  scratch revision or install it — it isolates the dispatch question
  Decision 4 was actually about (does this command sequence hang), not
  Decision 2's separately-discovered install requirement, and it is a
  correctness/hang check, not a timing measurement — the next step below
  still needs to establish actual per-state/per-call cost under the
  corrected (install-once-per-state) design.

## Decision 5: the query-layer revision is minted once per state (not once
per query), which reduces but does not fix a pre-existing realizer
use-after-free

**Current design, as implemented in `CEmbeddedReasoner`.** The first
`executeConjunctiveQuery()` call for a given state lazily installs
`mCurrentStateRevision` (Decision 2), then creates a second revision,
`mCurrentQueryRevision`, layered on top of it and used to build/answer the
query. Unlike the state revision, `mCurrentQueryRevision` is **created once
and reused for every subsequent query against the same state** — it is only
replaced (a plain `delete`, since it's never installed) when
`beginNewState()` starts the next state, or by the destructor. This mirrors
Konclude's own proven `COWLlinkProcessor::processCustomsEvents`
(`CParseProcessSPARQLTextCommand` branch,
`Source/Control/Interface/OWLlink/COWLlinkProcessor.cpp:1082-1119`), where
`lastGetCurrKBRevC` — the fresh revision a batch of SPARQL queries is built
against — is likewise created once per batch and reused across every
consecutive `SPARQL_QUERY` operation, only reset when an update intervenes.

- **Why some layer is still required, and why it doesn't need to be new
  every call.** This was checked empirically, prompted by a direct question
  about whether Decision 2's design was more layering than necessary.
  Querying `mCurrentStateRevision`'s ontology directly, with no extra layer
  at all, does *not* work: pre-existing ABox content (present before
  `beginNewState()`/install) resolves fine that way, but facts asserted via
  `assertClassFact()` in the same session silently return zero rows — so a
  layer is required for newly-Told content to become query-visible,
  confirming Decision 2's core finding. But that layer does not need to be
  *new on every call* — reusing one per state is sufficient and correct.
  All existing correctness regressions pass under this design
  (`embedded_state_driver`, `embedded_cq_driver` 314/314,
  `embedded_state_existing_abox_probe`, `embedded_scratch_revision_probe`).

**The realizer use-after-free this design change mitigates (but does not
fix).** Building `Tools/EmbeddedDriver/embedded_state_benchmark.cpp`
(per-state-size scaling benchmark, step 3 below) originally surfaced a
crash — before the once-per-state reuse above was implemented, when a fresh
query-layer revision (and therefore a fresh `CConcreteOntology*`) was minted
on *every* query. The crash is a real, pre-existing bug in Konclude's
realizer thread, unrelated to any `CEmbeddedReasoner` code, exposed by the
embedded state API's usage pattern (many CQs answered against the same
already-installed ontology, in one process, many times in a row). Nothing
in Konclude's normal batch CLI usage (one ontology, one realize pass per
process) ever exercises this path enough to hit it.

- **Root cause, confirmed under AddressSanitizer** (heap-use-after-free, not
  a data race — free and read both happen on the same thread):
  `Source/Reasoner/Realizer/CRealizerThread.cpp:189-196`
  (`processCustomsEvents`, `CRealizeOntologyEvent` branch) allocates a
  `COntologyRealizingDynamicRequirmentCallbackData* callbackProcData`, loops
  over the requirement list calling `addOntologyRealizingRequirements(item,
  requ, procData)` for each, and only *after* that loop calls
  `item->logRequirementProcessingStartStatistics(callbackProcData->getStatistics())`
  at line 196. But `addOntologyRealizingRequirements` →
  `COptimizedRepresentativeKPSetOntologyRealizingItem::addProcessingRequirement`
  (`Source/Reasoner/Realizer/COptimizedRepresentativeKPSetOntologyRealizingItem.cpp:1003-1014`)
  deletes that same `callbackProcData` (as `callbackData`, line 1011) as
  soon as a requirement finishes synchronously *and* is the last one
  outstanding. Line 196 then dereferences the freed object unconditionally.
  This delete-then-read happens on **every single cycle**, not just the one
  that finally crashes — confirmed by temporary logging. Reading
  already-freed memory is undefined behavior that usually "gets away with
  it" until something else's allocator activity reclaims that address,
  which is where the second bug below comes in.
- **A compounding, related bug: `CRealizerThread` instances (and their OS
  threads) are never freed.** `CRealizationManager::getRealizer()`
  (`Source/Reasoner/Kernel/Manager/CRealizationManager.cpp:45-72`) caches
  realizers in `mOntoRealizerHash`/`mRealizerSet`, keyed by
  `CConcreteOntology*`. `CRealizationManager::~CRealizationManager()` is
  empty — nothing in it, or anywhere else found so far, ever deletes
  entries out of `mRealizerSet`. Each leaked `CRealizerThread`
  (`COptimizedRepresentativeKPSetOntologyRealizingThread`, its own dedicated
  OS thread via `CThread::startThread`) gets its own private glibc malloc
  arena; once enough of them accumulate, one of their still-live arenas
  collides with another's freed-but-dangling chunk from the use-after-free
  above, turning silent UB into a visible failure. Confirmed directly
  against `Tools/EmbeddedDriver/embedded_state_benchmark.cpp`
  (`./embedded_state_benchmark 50 10 10 5`, 250 total query calls, run
  twice at each setting to rule out noise): corruption onsets consistently
  around state 9-11 regardless of glibc arena configuration, but which
  *failure signature* appears depends on it. Default arena behavior
  produces a loud, catchable failure — from onset on, every affected
  state's first query call returns `false` outright
  ("Query did not produce a variable-bindings result"), 41/250 calls
  affected, consistent across repeated runs. Forcing a single shared arena
  (`MALLOC_ARENA_MAX=1`) does **not** eliminate the corruption — it
  changes its shape to a silent one: ~190/250 calls affected, almost all
  returning success with a stale row count frozen at the very first
  state's answer (`rowCount=10`, regardless of the current state's actual
  fact count — indicative of a stale/cached query result being returned
  instead of a fresh one), plus a couple of failures with a distinct
  message never seen without the arena override
  (`"Stop processing 'Calculate-Query Command'."`). Single-arena mode is
  therefore not a usable mitigation: it trades a detectable hard failure
  for a larger volume of silent wrong answers.
- **Why minting the query-layer revision once per state (not once per
  query) matters here.** Before this fix, every single query missed the
  `CConcreteOntology*`-keyed realizer cache (a fresh revision meant a fresh
  ontology pointer every call) and leaked a new `CRealizerThread`. Reusing
  one query-layer revision per state means the cache now hits on the second
  and later queries within a state — only one realizer thread leaks per
  *state*, not per *query*. This does not fix either bug above; it only
  changes what "an accumulated leak large enough to trigger the collision"
  costs in terms of useful work. Measured empirically: a repro doing
  `beginNewState()` → assert → 15 queries, repeated across states, ran
  **120 correct queries (8 states × 15) before failing at state 8** — the
  same "~8 accumulated leaks" boundary observed with per-query minting, now
  counted in states rather than raw queries. For FD's actual usage shape
  (comparatively few states, many CQs answered against each one), this is
  roughly a `queriesPerState`-fold improvement in safe throughput before
  hitting these bugs.
- **Confirmed: the leak survives even a full reasoner-handle teardown, not
  just query-to-query.** Traced the actual shutdown path to check whether
  destroying and recreating the whole `KoncludeReasonerHandle` (not just
  reusing one query-layer revision per state) would reclaim the leaked
  realizer threads. `CThread::~CThread()`
  (`Source/Concurrent/CThread.cpp:48-50`) does correctly `quit()`+`wait()`
  the underlying OS thread *if it runs* — the base shutdown mechanics are
  fine. And `CReasonerManagerThread::threadStopped()`
  (`Source/Reasoner/Kernel/Manager/CReasonerManagerThread.cpp:277-287`),
  which runs as part of tearing down a reasoner instance/handle, does call
  `delete mRealizationManager;` (line 287). But `~CRealizationManager()`
  (`CRealizationManager.cpp:42-43`) is empty, and `mOntoRealizerHash`/
  `mRealizerSet` (`CRealizationManager.h:88-89`) are `QHash`/`QSet` *of
  pointers* — destroying them tears down the hash/set structure, not the
  pointed-to `CRealizer` objects. `mRealizerSet` is referenced in exactly
  two places in the whole codebase: insertion on creation
  (`CRealizationManager.cpp:66`) and iteration for progress reporting
  (`CRealizationManager.cpp:80`) — never for cleanup. So the leaked
  `CRealizerThread` objects, and their live OS threads, survive **even a
  complete `konclude_destroy_reasoner()` teardown** — they are leaked for
  the life of the *process*, not the life of the reasoner instance.
  Recycling the reasoner handle periodically, considered as a possible
  workaround, does **not** bound this leak.
- `CReasonerManagerThread.cpp:599-612` shows that
  `OPREALIZER` is scheduled whenever a query needs materialization (note:
  `CPreprocessKnowledgeBaseRequirementsForQueryCommand` itself is *not* the
  trigger — its own requirement list only ever demands `OPSTRIPLESINDEXING`,
  `CPreprocessKnowledgeBaseRequirementsForQueryCommand.cpp:44-47`; realization
  is decided later, from the query object's own shape):
  1. `CEmbeddedReasoner::executeConjunctiveQuery()` builds the `CQuery*` from
     the SPARQL text, then delegates a `CCalculateQueryCommand` and blocks on
     it (`CEmbeddedReasoner.cpp:641-643`).
  2. `CCommanderManagerThread` recognizes the command
     (`CCommanderManagerThread.cpp:190-194`) and hands it to the reasoner
     manager, which posts a `CCalcQueryEvent` onto `CReasonerManagerThread`'s
     own event queue (`CReasonerManagerThread.cpp:123`/`130`).
  3. The event loop receives it (`CReasonerManagerThread.cpp:1211-1212`) and
     calls `prepareQueryReasoning(CCalcQueryEvent*)` (`.cpp:690-725`), which
     calls `getRequirementsForQuery(query)` at line 696.
  4. `getRequirementsForQuery()` (`.cpp:333-461`) `dynamic_cast`s the query
     against several types; an `rdf:type` BGP atom makes it both a
     `CRealizationPremisingQuery` and a `CComplexVariablesAnsweringQuery`
     (the branches at `.cpp:403-461`), and since
     `isConceptRealisationRequired()`/`isDynamicRealisationRequired()` are
     true for that shape, a concept-realization requirement is appended and
     expanded via `mRequirementExpander->getUnsatisfiedRequirementsExpanded()`
     — this is the actual decision point that pulls realization into the
     pipeline, not any command's own static requirement list.
  5. Back in `prepareQueryReasoning`, that requirement is bucketed by
     processor type into `COntologyRequirementPreparingData::mRealizerReqList`
     (`COntologyRequirementPreparingData.cpp:71`/`85`), then
     `continueRequirementProcessing(reqPrepData, ontology)` is called
     (`.cpp:719`).
  6. `continueRequirementProcessing()`'s realizer branch
     (`.cpp:599-612`, quoted above) fires because `mRealizerReqList` is
     non-empty: it calls `mRealizationManager->getRealizer(ontology,config)`
     (the cache lookup/creation described above) and then
     `realizer->realize(ontology,config,ontReqPrepData->mRealizerReqList,reqProcCallbackEvent)`.
  7. `CRealizerThread::realize()` (`CRealizerThread.cpp:47-50`) does not do
     the work inline — it posts a `CRealizeOntologyEvent` onto the realizer
     thread's own queue and returns immediately, so the caller's stack frame
     is long gone by the time the next step runs.
  8. On the realizer thread, `processCustomsEvents()`'s
     `CRealizeOntologyEvent` branch (`.cpp:166-206`) is exactly the
     use-after-free traced above: `callbackProcData` allocated at line 189,
     freed inside the `addOntologyRealizingRequirements` loop
     (lines 191-195, via `addProcessingRequirement`), then read at line 196.

  So the query's own shape (an `rdf:type` BGP atom) — decided in step 4,
  three hops before the realizer is even looked up — is what makes every
  `executeConjunctiveQuery()` call of this shape walk straight into the bug;
  there is no branch point after that where the embedding layer could
  intervene without changing which triples get answered.
- **Status: reported, not fixed.** Per this repo's Task Scope convention
  (`CLAUDE.md`), bugs found while working on something else are reported
  here with file/line references and the failure scenario rather than fixed
  inline — both are pre-existing `Reasoner/Realizer/`+
  `Reasoner/Kernel/Manager/` code, not anything introduced by the
  `CEmbeddedReasoner` work this session, and warrant their own dedicated
  fix/verification pass rather than a drive-by patch.
- **Practical impact:** still blocks `embedded_state_benchmark.cpp` (and any
  other workload answering more than a handful of *states'* worth of CQs in
  one process, exact count environment-dependent since it hinges on glibc
  arena behavior) from producing trustworthy numbers at scale, just at a
  higher threshold than before this fix — the benchmark driver
  (`Tools/EmbeddedDriver/embedded_state_benchmark.cpp`) exists in the repo
  but is **not yet usable end-to-end** for this reason. Even setting the
  use-after-free aside, the unbounded per-state thread leak makes any
  long-running FD-style workload (many states) unfit for production as-is.
- **Incidental fix found while validating this design:**
  `Tools/EmbeddedDriver/embedded_cq_benchmark.cpp` was discovered to be
  failing 100% of its cycles, unrelated to the above — it predates the
  `konclude_state_*` API and was never updated to call
  `konclude_state_begin()` before querying, unlike its sibling
  `embedded_cq_driver.cpp`, which was. Fixed to match (a state with no
  asserted facts, so it queries the loaded ABox as-is); both benchmark
  scenarios now pass 0 failures again.

### Workaround plan (mitigates exposure; does not fix either bug)

Neither bug can be worked around from the embedding layer alone — the UAF
is unconditional (fires every cycle, per the AddressSanitizer finding
above) and the leak is unconditional and unreclaimable short of killing the
OS process (per the teardown trace above). What *is* achievable without
touching `Reasoner/Realizer/`/`Reasoner/Kernel/Manager/` is bounding and
monitoring exposure to both:

1. **Tuning glibc arena behavior (`mallopt(M_ARENA_MAX, 1)` /
   `MALLOC_ARENA_MAX=1`) does not help — dropped as an option.** Tested
   directly against `Tools/EmbeddedDriver/embedded_state_benchmark.cpp`
   (see above): forcing a single shared arena doesn't eliminate the
   corruption, it changes its signature from a loud, catchable failure
   (default arenas: 41/250 calls return `false` outright once the leak
   accumulates past ~9 states) into a much larger volume of silent wrong
   answers (single arena: ~190/250 calls return success with a stale row
   count frozen at an earlier state's answer). A caller can detect and
   react to the first failure mode; it cannot detect the second without an
   independent correctness check. Not recommended.
2. **Reasoner-handle recycling does not help** (see the teardown trace
   above) — dropped as an option. The only mechanism that reliably reclaims
   the leaked `CRealizerThread` OS threads is killing the OS process itself,
   since process teardown stops all threads unconditionally regardless of
   what Konclude's own destructors do or don't run.
3. **Run the embedded reasoner in a subprocess FD can restart periodically**,
   roughly every *K* states, instead of assuming one process for an entire
   search. This is heavier to integrate than an in-process handle swap (FD's
   side needs a restart protocol and a way to resume search state across the
   restart), but per point 2 it's the only mechanism that actually reclaims
   the leaked threads.
4. **Pick *K* from a resource budget, not the empirically-observed ~9-state
   onset** — that threshold is itself an artifact of allocator/heap layout
   (see above) and isn't a safety bound, nor can it be pushed out by arena
   tuning (point 1). Each leaked realizer is one live pthread (default
   ~8 MB reserved stack) plus one glibc malloc arena; size *K* conservatively
   against `ulimit -u` (max threads per user) and available virtual memory,
   with real headroom.
5. **Not yet built, but the natural next step if this plan is adopted**:
   expose a counter from `CEmbeddedReasoner` for how many distinct
   query-layer revisions it has minted this process (it already knows this —
   once per `beginNewState()`-triggered first query, i.e. exactly once per
   leaked realizer thread) via the C API, so the FD-side driver can restart
   proactively based on the actual accumulated count instead of a fixed
   schedule guessed in advance.
6. **Optional, supplementary**: lower the default thread stack size for
   realizer threads (if reachable via `QThread::setStackSize()` before
   `CThread::startThread()`), to raise how many leaked threads fit in
   virtual memory before *K* is reached. Not required for correctness, just
   headroom.

## Bug: reusing an individual name across states silently drops new facts about it, once the earlier state was installed

**Moved to `docs/EMBEDDED_STATE_ISOLATION_BUG.md`** — the investigation
(hash construction/destruction, the axiom-tracking layer, the
`CExpressionDataBoxMapping` layer, and the reasoner-kernel model-caching
layer, plus a minimal repro and next steps) grew long enough across
several rounds to warrant its own file. Status remains **reported, not
fixed**; this is still the single most important open issue for FD's
actual use case (see that doc's "Practical impact" section) since it hits
on essentially every state transition once individual names are reused,
unlike the `CRealizerThread` leak below which needs ~9+ states to matter.

## Recommended plan of record

1. ~~Verify Decision 4's open item first.~~ **Done.**
2. ~~Build the `konclude_state_*` C API surface.~~ **Done, for class
   assertions.** `konclude_state_begin` / `konclude_state_assert_class_fact`
   are implemented in `CEmbeddedReasoner`/`konclude_embedded.h` and verified
   correct (`Tools/EmbeddedDriver/embedded_state_driver.cpp`,
   `embedded_state_existing_abox_probe.cpp`; trusted-answer regression
   `embedded_cq_driver.cpp` still passes 314/314 against
   `Tests/roberts-family-full-D.owl.xml`). `konclude_state_assert_object_property_fact`
   (or similarly named) is the same mechanism, not yet built.
3. **Benchmark the corrected design — much less blocked after the
   once-per-state query-revision reuse, but still not clean.** The
   benchmark driver (`Tools/EmbeddedDriver/embedded_state_benchmark.cpp`) is
   written — for each fact count in a step-10 sweep, on one persistent
   handle: `beginNewState()` → assert that many facts → run one CQ
   (triggering the lazy install) → run several more CQs against the same
   state → repeat — timing each phase separately. Decision 5's
   once-per-state reuse means it can now run far more cycles than it could
   under the original once-per-query design before hitting the realizer
   bugs, but a sweep with enough states will still eventually corrupt, so
   results should be treated as provisional until those bugs are actually
   fixed. Once fixed, this is what answers the two open quantitative
   questions: does per-state cost stay flat across *N* states, and what is
   `referenceBuildData`'s actual install-time cost for a TBox around
   `robot-grid`'s size.
4. **Split the per-query numbers**, once step 3 has real data: "fresh
   per-query revision construction," "query construction (SPARQL-text
   parsing)," and "reasoning," instead of one bundled `query` phase number.
   This is what answers Decision 3's open question (does SPARQL-text
   parsing matter enough to justify an atom-based query API).
5. **Only after 3-4 produce real numbers**, decide whether to build the
   atom-based query API (`konclude_query_pattern_*`, sketched in
   `docs/FASTDOWNWARD_EMBEDDING.md`) or keep the shipped SPARQL-string
   `konclude_execute_conjunctive_query`, based on whether step 4 shows
   text-parsing as a meaningful fraction of per-call cost.

## Non-goals / explicitly out of scope

- Tableau/query-calculation performance itself — explicitly excluded per
  this session's direction ("the querying part is not optimizable").
- Concurrent, simultaneously-alive handles — still marked unverified/unsafe
  in `konclude_embedded.h`'s thread-safety comment; this plan assumes one
  handle used sequentially, matching FD's single-threaded search loop.
- Building the reactive-compaction mitigation
  (`CConcreteOntologyMergingRebuildingBuilder`, currently only invoked
  reactively for concurrent-write conflict resolution) for `onRevContainer`
  growth — not moot anymore the way it was under the original never-install
  plan (install now happens once per FD state), but still not undertaken
  until the benchmark above shows per-state growth is actually a problem at
  FD's expected state count, rather than assumed.
