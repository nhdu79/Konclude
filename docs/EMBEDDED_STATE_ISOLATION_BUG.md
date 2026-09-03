# Bug: Reusing an Individual Name Across States Drops New Facts About It

**Status: reported, not fixed, and confirmed NOT embedded-API-specific.**
Read `CLAUDE.md`'s Task Scope convention if picking this up — this is
`Reasoner/Revision/` + `Reasoner/Kernel/Cache/` + `Reasoner/Realizer/`
behavior, unrelated to (and predating) the embedded-interface work it was
found through. It is very likely a latent bug affecting any long-lived
caller that tells new facts about a previously-queried individual across
multiple revisions of the same knowledge base (OWLlink/SPARQL server "Tell
more axioms, then query again" flows included), not something
embedded-API-specific — no longer just a suspicion, see the "Confirmed
empirically" entry under "Attempted fixes (ruled out)" below, which
reproduces this bug through a driver that mirrors
`COWLlinkProcessor.cpp`'s real Tell/Install pattern call-for-call, not just
the embedded interface's simplified one.

## Symptom

Once a state has been installed (i.e. queried at least once), re-asserting
a fact about an individual **reused by name** from that installed state is
silently dropped from later query results, while a fact about a
**brand-new** individual name works correctly.

Minimal repro against `Tests/embedded-state-tbox-only.owl.xml`:

1. State 1: assert `ClassAssertion(a0, Father)`, then query → correctly
   returns `[a0]` (this also installs state 1).
2. State 2 (`beginNewState()`): assert `ClassAssertion(a0, Father)` (same
   individual, re-told) **and** `ClassAssertion(a1, Father)` (brand-new
   individual). Query returns **`[a1]` only** — `a0` is missing, despite
   `konclude_state_assert_class_fact` reporting success for it.
3. A 3rd state (`a0`+`a1`+`a2`, all re-told/new) confirms the pattern
   generalizes: query returns `[a2]` only.

So the trigger is specifically: *the individual's name was already used in
a state that got installed.* Names never seen before, or seen only in a
never-installed state, always work.

## Root cause

Confirmed step by step with gdb against a live two/three-state repro.

**1. Every `beginNewState()` mints a new, never-reused ontology ID.**
`CEmbeddedOntologyLoader::beginNewState()` → `CSPOntologyRevisionManager::
createNewOntologyRevision()` (`Source/Reasoner/Revision/
CSPOntologyRevisionManager.cpp:483`):

```cpp
qint64 ontologyID = nextOntologyID++;
```

`nextOntologyID` is a simple monotonic counter (`.cpp:33`), never reset to
or reconciled with a predecessor revision's ID. Confirmed via gdb: state 1
used `ontologyIdentifier=2`, state 2 used `ontologyIdentifier=4`.

**2. Asserting facts and building the ABox works correctly — this layer is
not the bug.** Entity interning gives a reused name (`a0`) the *same*
`CIndividualTermExpression*`/`CIndividual*` identity across states.
Axiom tracking correctly records the re-assertion as a change for the new
revision, and `setIndividualAssertionConceptFromClassTerm()` runs
identically for `a0` and `a1`. The ABox itself ends up correct — `a0` is
genuinely tagged `Father` in state 2's model. The divergence happens
downstream of this, in query answering's candidate-individual lookup.

**3. Query answering reads candidate individuals from a cache, not the
live ABox.** `executeConjunctiveQuery()` → `COptimizedComplexExpressionAnsweringHandler`
→ `COptimizedRepresentativeKPSetOntologyRealizingItem::getConceptInstancesIterator()`
(`Source/Reasoner/Realizer/COptimizedRepresentativeKPSetOntologyRealizingItem.cpp:2237-2253`).
This does not walk the ABox's individual list; it reads exclusively from
`CBackendRepresentativeMemoryCache`, populated once per query by
`COptimizedRepresentativeKPSetOntologyRealizingThread::
initializeRepresentativeConceptSetCacheLabelItems()` (`.cpp:417-523`).

**4. That cache is keyed by the never-reused ontology ID from step 1.**
`CBackendRepresentativeMemoryCache::prepareOntologyDataUpdate(ontologyIdentifier,
...)` (`Source/Reasoner/Kernel/Cache/CBackendRepresentativeMemoryCache.cpp:360-458`)
looks up `(*mOntologyIdentifierDataHash)[ontologyIdentifier]`. State 2's ID
(4) has never been seen, so `prevOntologyData` is null and a **brand-new,
empty** `indiIdAssoDataVector` is allocated for it — nothing is carried
over from state 1's array (keyed under `2`). A reuse path exists for
updating the *same* ID's slot (`.cpp:410-414`), but since every
`beginNewState()` mints a new ID, that path never fires across states.

**5. The new, empty array is only filled for individuals the tableau layer
decides need re-checking.** An individual only gets an entry written into
it when `CSaturationNodeBackendAssociationCacheHandler::
tryAssociateNodesWithBackendCache()` (`Source/Reasoner/Kernel/Algorithm/
CSaturationNodeBackendAssociationCacheHandler.cpp:1363`) processes its
node. Confirmed via gdb (breakpoint on this function, walking the
`CIndividualSaturationProcessNodeLinker*` argument): in every state tested,
this fires with **exactly one node** — only the individual with something
newly relevant to verify (`a1` in state 2, `a2` in state 3). `a0` has
nothing new from the tableau's point of view (already known
consistent/typed from state 1), so its node is never dispatched here and
it never gets an entry in state 2's cache array.

**6. The drop.** Query answering's candidate list (step 3) reads only from
that array. `a0` was never written into it, so it is invisible to every
query in state 2 — even though its ABox-level fact (`Father(a0)`) is
completely correct.

**In one sentence:** each `beginNewState()` gets a fresh, empty
candidate-individual cache keyed by an ID never linked to the previous
state's cache, and that cache is only populated for individuals the
tableau layer re-verifies (i.e. ones with genuinely new facts) — so an
individual reused from an earlier installed state is answering-invisible
unless something also forces its node through the tableau layer this
revision.

## Fix options (not attempted — architecture decision, not embedded-only)

In increasing order of invasiveness:

1. **Make `CSPOntologyRevisionManager::createNewOntologyRevision()` reuse
   the predecessor's `ontologyID`** instead of `nextOntologyID++`
   (`.cpp:483`, and the sibling call at `.cpp:413` for the persisted-reload
   path). This would make `prepareOntologyDataUpdate()`'s `prevOntologyData`
   lookup hit the predecessor's cache data automatically (the "slot update"
   path at `.cpp:410-414` already exists for exactly this). Likely the
   smallest, most targeted fix, but `getOntologyID()` is also read by
   `COccurrenceStatisticsCacheHandler`, `CIndividualNodeBackendCacheHandler`,
   `CTotallyPrecomputationThread`, `CIncrementalOntologyPrecomputationItem`,
   and the answering handler's own testing/materialization sub-ontologies —
   whether any of those rely on ontologyID uniqueness-per-revision for
   their own correctness needs auditing before this is safe.
2. **Make the satisfiability/saturation dispatcher also associate
   unchanged-but-still-relevant individuals** into a brand-new
   `ontologyIdentifier`'s cache namespace the first time that namespace is
   used, not only individuals with something newly asserted. More
   invasive (touches calculation-job generation), but keeps "one
   ontologyID per revision" semantics untouched.
3. **Give the KP-set realizer a fallback path** that consults
   `mOntology->getABox()->getIndividualVector()` directly (already
   available in `getConceptInstancesIterator`, `.cpp:2239`) for
   individuals absent from the backend cache, instead of trusting the
   backend cache as sole source of truth. Least invasive to the
   revision/cache layers, but changes query-answering semantics in a way
   that could mask a different bug if the backend cache is *supposed* to
   be authoritative for some other reason not yet understood.

No structural fix has been attempted — this needs a decision on which
tradeoff to take before touching `Reasoner/Revision/` or
`Reasoner/Kernel/Cache/` code shared well beyond the embedded interface.

## Attempted fixes (ruled out)

- **Config bisection** — forced `false` on all 16 config flags gating the
  optional satisfiability/expansion/representative-backend caches
  (`Konclude.Calculation.Optimization.*CacheRetrieval/Writing`,
  `Konclude.Cache.RepresentativeBackendCache.*`). **Bug reproduced
  identically with every one off** — this is unconditional incremental-
  revision bookkeeping, not anything togglable via config.
- **Consistency check right after `beginNewState()`** — called
  `checkConsistency()` immediately after `mOntologyLoader->beginNewState()`
  succeeds, on the theory this might force a from-scratch model
  computation touching every individual. **No change** — right after
  `beginNewState()` the new revision is identical to its predecessor, so
  the incremental consistency algorithm has nothing new to verify and
  takes the same "reuse predecessor's model" shortcut query answering
  does; `a0`'s node is still never touched.
- **Lazy, once-per-state consistency check run after facts are asserted**
  — added a flag to run `checkConsistency()` once, the first time a state
  is queried (i.e. after `assertClassFact()` calls), instead of
  immediately after `beginNewState()`. **No change** — confirmed via gdb
  that `tryAssociateNodesWithBackendCache()` still fires exactly once per
  state with exactly one individual node, identical to no consistency
  check at all. `CIsConsistentQueryCommand` uses the same incremental,
  changed-individuals-only dispatch as conjunctive query answering
  regardless of when it's invoked relative to assertions — there is no
  ordering of existing embedded-API calls that works around this from
  outside `Reasoner/Kernel/` + `Reasoner/Revision/`.
- **Retract + reassert on the SAME ontology revision, avoiding
  `beginNewState()` entirely** — added temporary `retractClassFact()`
  plumbing (`CEmbeddedOntologyLoader`/`CEmbeddedReasoner`/
  `konclude_state_retract_class_fact`, mirroring `assertClassFact()` but
  calling `retractOntologyAxiom()`) to test whether staying on one
  revision (so the ontologyID from "Root cause" step 1 never changes) lets
  a reused individual's fact update correctly without a fresh, empty
  cache namespace. Reverted after testing; not left in the tree. **Blocked
  by a separate, more basic limitation, found in the process: once a
  state's revision has been queried once (installed, and
  `CEmbeddedQueryManager::mCurrentQueryRevision` created for it), no
  further `assertClassFact()`/`retractClassFact()` call against that same
  revision is visible to any later query in that state — not just
  reused-name retractions, but a plain brand-new individual asserted
  after the first query is silently invisible too** (confirmed with a
  minimal isolation probe: `assertClassFact("a2", "Father")` called after
  the state's first query, followed by another query, still returns only
  the pre-existing individual — `a2` never appears, and the assert call
  itself reports success).
- **Follow-up: refresh the query-layer revision on every Tell/retract,
  mirroring `COWLlinkProcessor.cpp`'s `SPARQL_UPDATE_MODIFY` handling**
  (~line 1132-1150), which resets `lastGetCurrKBRevC`/`lastSparqlQC` to
  null whenever an update is processed, forcing the next query to build a
  fresh query-layer revision instead of reusing a stale one. Re-added the
  temporary `retractClassFact()` plumbing and additionally made
  `CEmbeddedReasoner::assertClassFact()`/`retractClassFact()` call
  `mQueryManager->resetForNewState()` (which only drops
  `mCurrentQueryRevision`, not the base state revision) after every
  successful Tell/retract — the direct embedded-API translation of that
  reset. Reverted after testing; not left in the tree. **No change** —
  same repro, same result (`[a0]` only, never `a1`/`a2`/the retraction).
  Added temporary `fprintf` instrumentation (also reverted) printing the
  base ontology's pointer, `ontologyID`, and
  `getABox()->getIndividualCount()` at every `assertClassFact()` and
  `executeConjunctiveQuery()` call to see why. Result, across
  assert(a0) → query → retract(a0) → query → reassert(a0)+assert(a1) →
  query, all against the **same** base ontology object (pointer and
  `ontologyID=2` constant throughout, confirming the retract/reassert
  plumbing really was hitting one unchanging revision, not creating new
  ones): individual count on that base ontology went `0` (right after
  telling `a0`, before any query) → `1` (by the second query) → **stuck
  at `1` for every query after that**, never advancing to `2` even after
  `a1` was told and even though the query-layer revision was, this time,
  freshly rebuilt (fresh pointer, fresh `ontologyID`) for every single
  query. **This relocates the real blocker one level below the query
  layer**: the *base* state revision's own `CConcreteOntology` — its
  TBox/RBox/ABox vectors, populated by the
  `resortAndInstallConceptsAndRolesAndIndividuals()` step from the
  "How axioms get built" walkthrough — only gets built once, apparently
  as a side effect of being used as someone else's "previous" revision
  during the *first* query's preprocessing
  (`CPreprocessKnowledgeBaseRequirementsForQueryCommand`). Once that has
  happened, the base ontology is evidently treated as "already built" and
  never re-triggers that build step for axioms told into it afterward —
  regardless of whether the layer on top of it is cached or freshly
  rebuilt each time, since a freshly-rebuilt layer still only ever reads
  whatever the base ontology's own vectors currently contain, and those
  never advance past the first build.
  **This means "keep only 2 revisions (previous + current), refresh the
  query layer at query time" cannot be made to work by refreshing the
  query layer alone** — the base revision itself has no supported,
  already-installed-then-Told-again-then-rebuilt lifecycle in this
  codebase. Consistent with this: `COWLlinkProcessor.cpp`'s own
  `SPARQL_UPDATE_MODIFY` branch never re-Tells into an already-installed
  ontology object either — it always constructs a **new**
  `CCreateKnowledgeBaseRevisionUpdateCommand` (a new revision, a new
  `ontologyID`) for every Modify batch and installs *that*, exactly
  mirroring `beginNewState()`'s own pattern. So the real OWLlink/SPARQL
  server flow this embedded interface was modeled on is, by this
  reasoning, *already* subject to the same reused-individual-invisible
  failure mode as "Root cause" describes for a `Tell`-then-`Modify` cycle
  that reuses an individual name across installs — not verified directly
  against the OWLlink server itself, but implied by using the identical
  `createNewOntologyRevision()`/fresh-`ontologyID` mechanism for every
  Modify. There is no revision-count-bounded, refresh-at-query-time
  scheme that avoids needing one of the three structural fix options
  above; the only way to avoid minting a fresh `ontologyID` per update
  batch is fix option 1 (reuse the predecessor's `ontologyID`) or the
  other two options, all of which touch `Reasoner/Revision/`/
  `Reasoner/Kernel/Cache/`/`Reasoner/Realizer/` directly.
- **Second-generation experiment: also install/adopt the QUERY revision
  itself as the chain head, and never reset "current" at a state boundary**
  — added `CEmbeddedPersistentChainedOntologyLoader`/
  `CEmbeddedPersistentChainedQueryManager` (own class/file pair, alongside
  not instead of `CEmbeddedChained*` above; wired into `CEmbeddedReasoner`
  and `konclude_embedded.h` as the `*_persistent_chained` C API functions;
  left in the tree, unlike the entry above's temporary plumbing) and a
  matching driver
  (`Tools/EmbeddedDriver/embedded_state_isolation_persistent_chained_probe.cpp`).
  Two changes from the first-generation chained pair: (1)
  `beginOrContinuePersistentChainedState()` never discards the current
  chain-head revision — a "new state" is a no-op once the chain has a head;
  (2) `executeConjunctiveQuery()` no longer builds a disposable,
  never-installed overlay reused across calls — every query call itself
  constructs a new revision layered on the current head, installs it
  immediately, queries it, and adopts it as the new chain head, exactly the
  same Create→Install→adopt-as-current mechanic `assertClassFactChained()`/
  `retractClassFactChained()` already use. **Result: identical failure,
  confirming this doesn't dodge "Root cause" either** — run against the same
  repro: state 1 (`a0`) → query `[a0]`; "state 2" (begin is a no-op; re-tell
  `a0` + tell `a1`) → query returns **`[a1]` only, `a0` dropped again**; a
  later no-op-begin "state 4" (tell `a0`+`a1`+`a2` on top of whatever was
  already installed) → query returns **`[a2]` only** — this time *both*
  `a0` and the just-retracted-then-untouched `a1` are dropped, not just the
  originally-reused name, consistent with "Root cause" step 5's "only
  individuals with something newly relevant this revision get an entry":
  installing a query's own revision doesn't make an unrelated individual's
  node newly relevant to the tableau, so it still never gets dispatched
  into that revision's backend-cache array. Confirms the bug is entirely
  about what the tableau/saturation layer chooses to (re-)verify per
  `ontologyID`, not about which Control-layer object initiates the install
  or when "state" boundaries nominally occur — no combination of
  install-immediately/install-the-query-revision/never-reset at the
  `Control::Interface::Embedded` layer can dodge this without one of the
  three structural fixes listed above.
- **Confirmed empirically: the real `COWLlinkProcessor.cpp` Tell/Install
  pattern IS subject to this bug too** (settling the "implied but not
  verified" hedge in the entry above). Added temporary
  `assertClassFactChained()`/`retractClassFactChained()` to
  `CEmbeddedOntologyLoader`/`CEmbeddedReasoner` plus
  `konclude_state_assert_class_fact_chained`/
  `konclude_state_retract_class_fact_chained` in the C API, and a driver
  (`Tools/EmbeddedDriver/embedded_state_isolation_chained_probe.cpp`) — all
  reverted after testing, not left in the tree. Unlike the "Follow-up"
  attempt above (which kept Telling into one static, once-installed base
  revision, only refreshing the query layer), this mirrored
  `SPARQL_UPDATE_MODIFY` call-for-call: **every** chained assert/retract
  created a brand-new revision layered on the current installed head (via
  `CCreateKnowledgeBaseRevisionUpdateCommand`), Told/retracted the single
  fact into it, and installed it immediately (via
  `CInstallKnowledgeBaseRevisionUpdateCommand`) — a genuine chain of
  installed revisions, each with its own fresh `ontologyID`, exactly as
  `COWLlinkProcessor.cpp`'s real flow does. Result, run against the exact
  repro from "Symptom" above (state 1: tell `a0`, query → `[a0]`; state 2:
  re-tell `a0` + tell `a1`, query): **`a0` is still dropped, query returns
  `[a1]` only — identical symptom, identical failure.** This is the
  expected outcome given "Root cause" above: every install still goes
  through `createNewOntologyRevision()`, which mints a fresh `ontologyID`
  unconditionally (`.cpp:483`) regardless of *how* often or via what
  Control-layer pattern installs happen — so no revision-chain-management
  scheme at `Control::Interface::Embedded` (or, by the same reasoning,
  `Control::Interface::OWLlink`) can dodge this without one of the three
  structural fixes above.
  **Two further findings from this run, both new:**
  1. **Per-call install reproduces the exact cost
     `docs/EMBEDDED_HIGH_VOLUME_CQ_OPTIMIZATION.md`'s Decision 2 explicitly
     rejected** ("install every call" — `onRevContainer` growth and
     `referenceBuildData()` cost compounding per call instead of per
     state), confirming that doc's rejection was correct independent of
     this bug.
  2. **Intermittent crash (SIGTRAP, exit 133), not present in the
     non-chained design, in roughly 1 of 3 standalone runs** — the same
     probe binary, unchanged, alternated between a clean completion
     (matching the result above) and a crash after only 1-2 query calls,
     across 3 consecutive runs on the same machine. Did not reproduce under
     `lldb` (ran to clean completion there every time), consistent with a
     genuine timing-dependent race rather than a deterministic bug —
     plausibly from the back-to-back create→Tell→install command
     round-trips each chained call adds relative to `assertClassFact()`'s
     single accumulate-only Tell. Not investigated further (out of scope
     for this isolation-bug test; would need its own dedicated look at
     `Control/Command` dispatch or `Reasoner/Revision` under rapid
     sequential installs).
- **Attempted: run the repro through the actual `sparqlfile` CLI (not a
  mirror of `COWLlinkProcessor.cpp`'s pattern via the embedded interface,
  the real interface itself) — inconclusive, blocked by two other bugs
  found along the way, both distinct from this one.** Built
  `KoncludeWithoutRedland.pro`'s CLI binary and wrote
  `Tools/EmbeddedDriver/sparql_isolation_probe.sparql` (left in the tree) to
  run: `INSERT GRAPH <kb> { a0 rdf:type Father }` → `SELECT` → `INSERT GRAPH
  <kb> { a0 rdf:type Father . a1 rdf:type Father }` → `SELECT`, all against
  one explicitly-named graph, via `./Release/Konclude sparqlfile -w 2 -s
  Tools/EmbeddedDriver/sparql_isolation_probe.sparql -o OUT -c
  Configs/querying-config.xml`. Confirmed via direct code reading (not
  needing to run anything) that this CLI path is mechanically identical to
  what the chained probe above already tested: `COWLlinkProcessor.cpp`'s
  `SPARQL_UPDATE_MODIFY` branch (~line 1133-1150) issues the exact same
  `CCreateKnowledgeBaseRevisionUpdateCommand` → Tell →
  `CInstallKnowledgeBaseRevisionUpdateCommand` sequence, landing in the same
  `CSPOntologyRevisionManager::createNewOntologyRevision()` with its
  unconditional `nextOntologyID++` (`.cpp:483`) — so the reused-individual
  drop this doc describes is expected to reproduce here too. Getting an
  actual *live* result to confirm/refute that turned out to require working
  around, then getting blocked by, two separate issues:
  1. **`CSPARQLKnowledgeBaseSplittingOperationParser`'s graph-name
     resolution is fragile for anything beyond simple LOAD-then-SELECT**
     (`Source/Parser/CSPARQLKnowledgeBaseSplittingOperationParser.cpp`).
     `INSERT`/`DELETE` resolve their target graph via a *direct* token
     reference (`getGraphName(...,directGraphReference=true)`, `.cpp:214`)
     instead of requiring a `FROM`/`INTO`/`GRAPH` keyword the way `SELECT`
     does — so a bare `INSERT { ... }` with no graph clause at all
     misreads the opening `{` itself as a candidate graph IRI, silently
     creating and populating a bogus knowledge base literally named `{`
     confirmed by the log: `Preprocessing ontology '{'`). Separately, `LOAD
     <file> INTO <iri>` does NOT auto-create `<iri>` if it doesn't already
     exist — only resolving to the built-in
     `"$LastCreatedKnowledgeBase..."` sentinel does that
     (`CSPOntologyRevisionManager::createNewOntologyRevision()`,
     `.cpp:462-467`) — a `LOAD ... INTO <not-yet-existing-iri>` hits
     `"KnowledgeBase '...' does not exist."` instead. Worked around by
     issuing an explicit `CREATE GRAPH <iri>` first, then naming that same
     `<iri>` explicitly on every subsequent `LOAD`/`INSERT`/`SELECT`. Minor,
     narrow parser gap — not investigated further, out of scope for this
     task (reported here per `CLAUDE.md`'s Task Scope convention).
  2. **A second, unrelated per-`ontologyID` cache — not the one this doc
     otherwise describes — makes CQ answering spuriously report *any*
     brand-new revision's query as unsatisfiable, confirmed by temporary
     debug instrumentation (reverted, not left in the tree) in
     `COptimizedComplexExpressionAnsweringHandler::identifyNonInstantiation()`
     (`Source/Reasoner/Answerer/COptimizedComplexExpressionAnsweringHandler.cpp:2942-3002`,
     called from `initializeQueryProcessing()` at `.cpp:1988-1994`, gated by
     config `Konclude.Answering.UnsatisfiableNonInstanceStatisticsChecking`,
     default `true`).** It looks up
     `COccurrenceStatisticsCacheReader::getAccummulatedConceptDataOccurrenceStatistics(ontologyId,
     conceptTag)` — `ontologyId` again being
     `mOntoAnsweringItem->getOntology()->getOntologyID()`, the same
     never-reused counter from "Root cause" step 1 — and if both its
     existential- and individual-instance counts are `<= 0`, treats the
     query as trivially unsatisfiable *before ever consulting the ABox*.
     Debug print confirmed this reads `existCount=0 indiCount=0` for
     **every** ontologyId tested, including the very first query against
     the very first freshly-installed revision (`ontologyId=2`, with `a0`
     genuinely told and installed, no reuse involved at all) — so this
     shortcut isn't specific to reused individuals, it empties every single
     query result through this CLI path regardless. Forcing the config flag
     false (temporarily, in-process, since a `-c` config file's `<Set>` for
     this same key had no observable effect — config-loading-order issue
     not run down) got past this shortcut but then the real answering path
     hung during realization under both `-w 2` and `-w 4` before completing
     — a third, distinct issue, not investigated further.

  **Net result: inconclusive at the live-CLI level** — never got a clean
  before/after query-result comparison through `sparqlfile` itself, so this
  entry does NOT independently confirm the reused-individual drop the way
  the chained-probe entry above does. The static code-path identity
  argument above stands on its own regardless (same commands, same
  `createNewOntologyRevision()` call, same unconditional fresh
  `ontologyID`), so the reused-individual bug is still expected to affect
  the real SPARQL/OWLlink server — just not re-confirmed by an end-to-end
  run this time. A follow-up attempt should expect to need to work around
  both issues above (or use `owllinkserver`/`sparqlserver` with a real
  client instead of the `sparqlfile` batch-splitting parser, which may not
  share issue 1) before a live before/after comparison is possible.
