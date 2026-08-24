# `CEmbeddedReasoner::executeConjunctiveQuery()` — Step-by-Step

This documents what `Source/Control/Interface/Embedded/CEmbeddedReasoner.cpp`'s
`executeConjunctiveQuery()` (roughly lines 350-458) actually does, step by
step, as it stands after the bug fixes described in
`docs/EMBEDDED_CQ_DRIVER.md`'s "Fixed bugs found via this driver" section.
Read that doc first for *why* each of these steps exists — this doc is the
"what does the code do" companion once those bugs are already understood.

## 1. Reset cached results

```cpp
mLastQueryVariableNames.clear();
mLastQueryResultRows.clear();
```

Every call starts by wiping whatever the *previous* `executeConjunctiveQuery()`
call left behind — these two members are what `getLastQueryResult*()` reads
back through the C API, so stale data from a prior query must never leak
into a failed/different one.

## 2. Guard: ontology must already be loaded

Simple precondition check against `mOntologyLoaded` (set by
`loadOntologyFile()`). Fails fast with a clear error rather than sending a
command sequence into a reasoner with nothing to query.

## 3. Create a *new* revision layer first

```cpp
CCreateKnowledgeBaseRevisionUpdateCommand* createRevCommand = new CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName);
mPreconditionSynchronizer->delegateCommand(createRevCommand);
CCommandExecutedBlocker::waitExecutedCommand(createRevCommand);
COntologyRevision* ontRev = createRevCommand->getOntologyRevision();
...
CConcreteOntology* onto = ontRev->getOntology();
COntologyRevision* prevOntRev = ontRev->getPreviousOntologyRevision();
CConcreteOntology* baseOnt = prevOntRev ? prevOntRev->getOntology() : onto;
COntologyConfigurationExtension* ontConfig = ontRev->getOntologyConfiguration();
```

The least intuitive step, and the fix for the fourth bug found while getting
the embedded CQ driver to pass. Rather than reading back the ontology object
that's already current, this uses the plain `CCreateKnowledgeBaseRevisionUpdateCommand`
constructor to have `CSPOntologyRevisionManager` build a **brand-new
`CConcreteOntology`** that wraps the current one (`new CConcreteOntology(currOnt, nextOntConfig)`
internally — see `CSPOntologyRevisionManager::createNewOntologyRevision`).
This mirrors exactly what the proven CLI/OWLlink SPARQL path does before it
builds any query. `onto` is this new object; `baseOnt` is the *previous*
revision's ontology (the "before this query's implicit revision" snapshot)
— falls back to `onto` itself only if somehow there's no previous revision
at all. `ontConfig` carries per-revision settings the query builder needs.

Skipping this and building the query straight against the already-current
ontology was exactly what produced the zero-rows bug documented in
`docs/EMBEDDED_CQ_DRIVER.md`: the query still parsed fine (variable names
came out correct), but every class/property term silently resolved to
nothing, because `CConcreteOntologyQueryBasicBuilder`'s term lookup falls
back to `nullptr` on a miss instead of erroring.

## 4. Build the `CQuery*` from the SPARQL text

```cpp
CConcreteOntologyUpdateSeparateHashingCollectorBuilder* builder = new CConcreteOntologyUpdateSeparateHashingCollectorBuilder(onto);
CConcreteOntologyQueryExtendedBuilder* queryBuilderGen = new CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, ontConfig, builder);
CSPARQLSimpleQueryParser* sparqlQueryParser = new CSPARQLSimpleQueryParser(queryBuilderGen, builder, onto);
builder->initializeBuilding();
sparqlQueryParser->parseQueryText(sparqlSelectQuery);
builder->completeBuilding();
QList<CQuery*> queryList = queryBuilderGen->generateQuerys();
delete sparqlQueryParser;
delete queryBuilderGen;
delete builder;
```

Three collaborating objects, all local/temporary (deleted right after use —
only the resulting `CQuery*` objects survive):

- **`builder`** — an update-collector that `CConcreteOntologyQueryExtendedBuilder`
  writes into as it discovers entities the query references.
- **`queryBuilderGen`** — the actual query-model builder; ties everything to
  `baseOnt`/`onto`/`ontConfig`.
- **`sparqlQueryParser`** — a hand-rolled tokenizer/parser (not Rasqal —
  `KoncludeEmbedded.pro` never links it), driven via `parseQueryText()`, the
  singular-string method. Its sibling `parseQueryTextList()` expects
  *already-tokenized* input despite the name — passing it one untokenized
  blob silently builds nothing (the second bug fixed this session; see
  `docs/EMBEDDED_CQ_DRIVER.md`).

`initializeBuilding()`/`completeBuilding()` bracket the parse, flushing
whatever the collector accumulated. `generateQuerys()` (plural — the API
supports multiple queries per text blob) then materializes actual `CQuery*`
objects. Since this driver only supports one `SELECT`/one basic graph
pattern, anything other than exactly one result is treated as an error.

## 5. Force triples indexing before calculating

```cpp
CPreprocessKnowledgeBaseRequirementsForQueryCommand* prepQueryCommand = new CPreprocessKnowledgeBaseRequirementsForQueryCommand(onto);
mPreconditionSynchronizer->delegateCommand(prepQueryCommand);
CCommandExecutedBlocker::waitExecutedCommand(prepQueryCommand);
```

The first CQ-specific bug fix. `CCalculateQueryCommand`'s own handler just
forwards the query straight to the reasoner manager — it does **not**
independently ensure the ontology's triples index has been built. The
proven `checkSatisfiability` path runs this exact command first as an
explicit precondition; without it, calculation silently returns zero rows
rather than erroring.

## 6. Calculate

```cpp
CCalculateQueryCommand* calcQueryCommand = new CCalculateQueryCommand(query);
mPreconditionSynchronizer->delegateCommand(calcQueryCommand);
CCommandExecutedBlocker::waitExecutedCommand(calcQueryCommand);
```

The actual reasoning step — this is what invokes the tableau/query-answering
algorithm against `onto`'s completion and writes the answer set back onto
the `CQuery` object itself (`query->getQueryResult()`, used next).

## 7. Extract results into plain Qt containers

```cpp
CQueryResult* result = query->getQueryResult();
CVariableBindingsAnswersResult* varBindAnsRes = dynamic_cast<...>(result);
...
for (const QString& varName : varBindAnsRes->getVariableNames()) mLastQueryVariableNames.append(varName);

CVariableBindingsResultIterator* cellIt = nullptr;
CVariableBindingsAnswersResultIterator* rowIt = varBindAnsRes->getVariableBindingsAnswersIterator();
while (rowIt->hasNext()) {
    CVariableBindingsAnswerResult* row = rowIt->getNext();
    cellIt = row->getVariableBindingsIterator(cellIt);   // reuses cellIt across rows
    QStringList rowValues;
    while (cellIt->hasNext()) {
        CVariableBindingResult* cell = cellIt->getNext();
        rowValues.append(cell ? cell->getBindingString() : QString());
    }
    mLastQueryResultRows.append(rowValues);
}
delete rowIt;
delete cellIt;
```

Two nested iterators — rows, then cells within a row — walked exactly the
way `CSPARQLXMLAnswerSerializer.cpp` does it (the proven reference this was
modeled on). Note `cellIt` is deliberately **reused** across row iterations
(`row->getVariableBindingsIterator(cellIt)` passes the existing pointer back
in) rather than allocated fresh per row — an explicit reuse contract in that
API, hence only one `delete cellIt` at the end alongside `delete rowIt`.
Everything gets copied out into `mLastQueryVariableNames`/`mLastQueryResultRows`
(plain `QString`/`QStringList`) precisely so the `CQuery`/`CQueryResult`
object graph doesn't need to stay alive for the C API caller to read results
back afterward — `getLastQueryResultRowCount()` etc. just index into these
two members.

## Note on command object lifetime

This method never deletes the `CQuery*` or the
`CCreateKnowledgeBaseRevisionUpdateCommand`/`CPreprocessKnowledgeBaseRequirementsForQueryCommand`/`CCalculateQueryCommand`
command objects it constructs. That mirrors the same pattern already present
in `loadOntologyFile()`/`checkConsistency()` elsewhere in this file (no
explicit command cleanup there either) — presumably Konclude's
command/revision machinery owns and reclaims these internally once
processed, but this has not been independently verified. Worth keeping in
mind if this ever gets used in a tight, long-running loop rather than
short-lived driver processes — see `Tools/EmbeddedDriver/embedded_cq_benchmark.cpp`
for exactly such a loop, and check for unbounded memory growth over many
iterations if that's ever a concern.
