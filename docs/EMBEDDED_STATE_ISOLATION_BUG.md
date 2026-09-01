# Bug: Reusing an Individual Name Across States Drops New Facts About It

**Status: reported, not fixed.** Read `CLAUDE.md`'s Task Scope convention
if picking this up — this is `Reasoner/Revision/` + `Reasoner/Kernel/Cache/`
+ `Reasoner/Realizer/` behavior, unrelated to (and predating) the
embedded-interface work it was found through. It is very likely a latent
bug affecting any long-lived caller that tells new facts about a
previously-queried individual across multiple revisions of the same
knowledge base (OWLlink/SPARQL server "Tell more axioms, then query
again" flows included), not something embedded-API-specific.

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
