# Conjunctive Query Answering: CLI/Server Entrypoint Pipeline

Traces the real, already-working code path Konclude uses today to answer a
conjunctive query (CQ) via `sparqlfile`/`sparqlserver`, from CLI invocation
to output file. This is the *dispatch/orchestration* pipeline — for the
internal composition-tree join/propagation engine that actually evaluates a
parsed query once built (`Reasoner/Answerer/`, `Reasoner/Query/`), see the
research notes referenced inline below rather than duplicating them here.

Traced for:
```sh
./Konclude sparqlfile -s query.sparql -o out.xml -c Configs/querying-config.xml
```

## 1. CLI dispatch → loader selection

`CSPARQLFileComandLinePreparationTranslator::canTranslate`
(`Control/Interface/CommandLine/CSPARQLFileComandLinePreparationTranslator.cpp:157-162`)
recognizes the `sparqlfile`/`sparql` command. Its constructor
(lines 32-40) hardcodes three reasoner config overrides unconditionally for
every `sparqlfile` run:

- `Konclude.Calculation.Querying.ComplexQueryingSupport=true`
- `RepresentativePropagation=false`
- `SignatureMirroringBlocking=false`

The latter two optimizations are disabled because, per the config file's own
comment, "these optimizations are currently not compatible with the query
answering techniques."

`translate()` (lines 99-153) consumes `-s <file>` (query input,
lines 136-144) and `-o`/`-r` (response/output file, lines 117-130), and
delegates `-c <config>` to the base-class translator.
`combineTranslatedArguments()` (lines 46-97) assembles the synthetic arg
list handed to the loader factory: `-DefaultReasonerLoader` (sets up the
reasoner), optionally `-OWLlinkBatchFileLoader` (if `-c` was given, to apply
the config file), and finally:

```
-SPARQLBatchFileLoader +=Konclude.SPARQL.CloseAfterProcessedRequest=true
  +=Konclude.SPARQL.BlockUntilProcessedRequest=true
  +=Konclude.SPARQL.RequestFile=<s-file> +=Konclude.SPARQL.ResponseFile=<o-file>
```

`CDefaultLoaderFactory` resolves `-SPARQLBatchFileLoader` to
`CSPARQLBatchFileLoader` (`Control/Loader/CSPARQLBatchFileLoader.cpp`).

## 2. `CSPARQLBatchFileLoader`: read file → parse → block until done → write file

`init()` (lines 49-101) reads `Konclude.SPARQL.RequestFile`/`ResponseFile`
off the config and sets `mBlockUntilProcessed = true` (from the arg list
above). `load()` (lines 105-111) calls `startProcessing()` then **blocks on
`mBlockingSem.acquire()`** until the whole request is done — the same
blocking-semaphore pattern used throughout this codebase (mirrors the JNI
and embedded-facade synchronous-wait patterns), not a callback API.

`initializeOWLlinkContent()` (lines 145-189) reads the entire `-s` file into
a `QString`, constructs a `CSPARQLRecordResultStreamingInterpreter` (the
result recorder/serializer driver — see §6) wired to `this` as the
`CSPARQLStreamingWriter` sink, and delegates a
`CParseProcessSPARQLTextCommand` carrying the raw SPARQL text
(lines 162-166), with `mSPARQLInterpreter` set as its command recorder.

`concludeOWLlinkContent()` (lines 283-298) releases `mBlockingSem`,
unblocking `load()`, and (since `CloseAfterProcessedRequest=true`) calls
`QCoreApplication::exit()` — this is why `mainLoader.cpp`'s `a.exec()` is
needed for the CLI build specifically: `sparqlfile` mode relies on the real
Qt event loop to actually terminate the process afterward, unlike the
JNI/embedded synchronous-wait pattern (see `docs/FASTDOWNWARD_EMBEDDING.md`
phase 3).

## 3. `CParseProcessSPARQLTextCommand` → per-BGP command chain

`CSPARQLBatchFileLoader` is itself a `COWLlinkProcessor` subclass, so its
raw SPARQL text gets processed by `COWLlinkProcessor`'s command-dispatch
`if`/`else` chain. The `CParseProcessSPARQLTextCommand` branch
(`Control/Interface/OWLlink/COWLlinkProcessor.cpp:1061-1152+`) does the real
SPARQL-level work:

1. `CSPARQLKnowledgeBaseSplittingOperationParser` splits the text by target
   graph/knowledge-base and by operation type — `SPARQL_QUERY` vs.
   `SPARQL_UPDATE_MANAGE` vs. `SPARQL_UPDATE_MODIFY` (lines 1071-1093).
2. For each query operation, it builds a **precondition-chained command
   sequence**, not one flat call:
   `CCreateKnowledgeBaseRevisionUpdateCommand` (get/create the KB revision to
   query against) → `CParseSPARQLQueryCommand` (parse text into `CQuery*`
   objects, §4) → `CGetQueryDependentKnowledgeBaseRevisionUpdatesCommand` →
   `CCalculateQueriesCommand` (execute them) (lines 1096-1118). Each
   command's constructor takes the previous one as a dependency, so
   `CCommanderManager`'s own precondition system — not manual blocking —
   sequences the steps.

## 4. `CParseSPARQLQueryCommand` — the Rasqal-vs-non-Rasqal fork

`Control/Interface/OWLlink/COWLlinkProcessor.cpp:868-929`:

```cpp
CConcreteOntology* onto    = ontRev->getOntology();                               // :876 -- current revision = expressionOntology
CConcreteOntology* baseOnt = ontRev->getPreviousOntologyRevision()->getOntology(); // :877 -- previous revision = baseOntology
bool confRedlandRasqalSPARQLQueryProcessing =
    CConfigDataReader::readConfigBoolean(ontConfig, "Konclude.Answering.RedlandRasqalSPARQLQueryProcessing", true); // :886
#ifdef KONCLUDE_REDLAND_INTEGRATION
if (confRedlandRasqalSPARQLQueryProcessing) {
    CSPARQLRedlandRasqalQueryParser* sparqlQueryParser = new CSPARQLRedlandRasqalQueryParser(baseOnt, onto, ontConfig); // :893
    sparqlQueryParser->parseQueryTextList(queryStringList, queryName);
    ...
} else {
#endif
    CConcreteOntologyUpdateSeparateHashingCollectorBuilder* builder = new CConcreteOntologyUpdateSeparateHashingCollectorBuilder(onto); // :902
    CConcreteOntologyQueryExtendedBuilder* queryBuilderGen = new CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, ontConfig, builder); // :903
    CSPARQLQueryParser* sparqlQueryParser = new CSPARQLSimpleQueryParser(queryBuilderGen, builder, onto); // :904
    builder->initializeBuilding();
    sparqlQueryParser->parseQueryTextList(queryStringList);
    builder->completeBuilding();
    queryList = queryBuilderGen->generateQuerys();  // :910 -- QList<CQuery*>, ready for CCalculateQueryCommand
#ifdef KONCLUDE_REDLAND_INTEGRATION
}
#endif
```

This is a **two-level gate**, not purely config or purely build-time:
`KONCLUDE_REDLAND_INTEGRATION` (build-time, absent from
`KoncludeEmbedded.pro`) controls whether the Rasqal path exists in the
binary at all; `Konclude.Answering.RedlandRasqalSPARQLQueryProcessing`
(config, defaults **true**) only picks between the two paths when both are
compiled in. In `KoncludeEmbedded.pro`, that config key has no effect — you
always get `CSPARQLSimpleQueryParser`.

`expressionOntology` (`onto`) is not a fresh/empty ontology — it's the
knowledge base's own *current* `COntologyRevision`; `baseOntology` is simply
the *previous* revision in that same chain. No separate/empty ontology
object is constructed anywhere for this purpose.

### Concrete scope limit — straight from the config file's own comment

`Configs/querying-config.xml:68-72`:

> "Specifies whether Redland Rasqal is used for processing SPARQL queries.
> If enabled, BGPs of the query are precomputed by Konclude's query
> answering engine and their results are then appropriately incorporated.
> **If disabled (or if the Redland libraries are not integrated), only
> simple SPARQL queries with just one BGP are supported.**"

A single BGP *is* exactly a multi-atom conjunctive query — a set of triple
patterns joined by shared variables. So `CSPARQLSimpleQueryParser` (and
therefore any Rasqal-free embedded build) fully supports the "several atoms
joined by shared variables" case. What it cannot do is combine **multiple
BGPs** into one query — `OPTIONAL`, `UNION`, nested sub-`SELECT`s all
require the Rasqal path.

## 5. `CCalculateQueriesCommand` → `CCalculateQueryCommand` (fan-out)

`COWLlinkProcessor.cpp:1045-1055`:

```cpp
} else if (dynamic_cast<CCalculateQueriesCommand*>(command)) {
    CCalculateQueriesCommand *calcQueriesC = (CCalculateQueriesCommand*)command;
    for (CQuery* query : calcQueriesC->getQueryList()) {
        if (query) {
            CCalculateQueryCommand* calcQueryC = new CCalculateQueryCommand(query, calcQueriesC);
            preSynchronizer->delegateCommand(calcQueryC);
        }
    }
```

`CCalculateQueryCommand` is the single shared execution primitive for every
kind of query in the codebase — simple fixed-shape queries (instance
checks, satisfiability) and multi-atom CQs alike; dispatch is polymorphic on
the `CQuery*`'s own concrete type. This is the same command class
`Source/Control/Interface/Embedded/CEmbeddedReasoner.cpp` already uses for
satisfiability/consistency checks.

## 6. Result extraction and SPARQL-XML rendering

`CSPARQLRecordResultStreamingInterpreter::delegateCommand`
(`Control/Interface/SPARQL/CSPARQLRecordResultStreamingInterpreter.cpp:53-74`)
intercepts every `CCalculateQueryCommand` as it's delegated and attaches a
`CSPARQLResultStreamingData` per command, keyed by a sequence number (for
correct output ordering when queries execute concurrently). When the
command's `CClosureProcessCommandRecord` fires (finished), `recordData()`
(lines 103-157) pulls `query->getQueryResult()` and calls
`seqData->handleQueryResult(query, queryResult)`.

`CSPARQLResultStreamingData::handleQueryResult`
(`CSPARQLResultStreamingData.cpp:192-217`):

```cpp
} else if (dynamic_cast<CVariableBindingsAnswersResult*>(queryResult)) {
    CVariableBindingsAnswersResult* varBindAnsRes = (CVariableBindingsAnswersResult*)queryResult;
    QStringList varList;
    for (auto varExp : varBindAnsRes->getVariableNames()) { varList.append(varExp); }
    mSerializer->addResultSerialization(varList, varBindAnsRes);   // :206
```

`mSerializer` is a
`CSPARQLXMLAnswerStreamingPrecompiledByteArraySerializer`
(`Control/Interface/SPARQL/CSPARQLXMLAnswerStreamingPrecompiledByteArraySerializer.h:71`)
— the concrete SPARQL Query Results XML Format writer, which walks the
`CVariableBindingsAnswersResult` / `CVariableBindingsAnswersResultIterator` /
`CVariableBindingsResultIterator` chain to produce
`<result><binding name="...">...` XML. Its output buffers flow through
`CSPARQLStreamingWriter::writeStreamData` →
`CSPARQLBatchFileLoader::writeStreamDataToFile`
(`CSPARQLBatchFileLoader.cpp:205-279`), which wraps them in the
`<sparql xmlns="http://www.w3.org/2005/sparql-results#">...</sparql>`
envelope (header/footer built in the constructor, lines 36-37) and writes
them to the `-o` file in chunks.

## 7. Config file's concrete role (`Configs/querying-config.xml`)

- **`Konclude.Answering.RedlandRasqalSPARQLQueryProcessing`** — Rasqal on/off
  switch (§4); only has effect when `KONCLUDE_REDLAND_INTEGRATION` is
  compiled in.
- **`Konclude.Answering.MinimalMappingsComputationSize`** — initial batch
  size for incremental answer computation; `-1` disables streaming entirely
  and computes all mappings up front. This is the config-facing knob on top
  of the internal geometric-batch-growth mechanism in
  `CAbstractStreamingComplexQueryFinishingHandler` (starts at 10 bindings,
  ×10 growth per round, capped at 100M).
- **`ConcurrentJoinComputation` / `ConcurrentAnswerGeneration`** — parallelize
  joins and serialization, with `ConcurrentComputationThreadPoolSize`
  (`-1` = use all cores).
- **`InterpretNonAnswerIndividualVariablesAsAnonymousVariables`** —
  existential-variable semantics for non-projected variables.

## 8. Server mode (`sparqlserver`)

`CSPARQLHttpConnectionHandlerProcessor : public COWLlinkProcessor, public
CQtHttpPooledConnectionHandler, public CSPARQLStreamingWriter`
(`Control/Interface/SPARQL/CSPARQLHttpConnectionHandlerProcessor.h:81`) is a
`COWLlinkProcessor` subclass exactly like `CSPARQLBatchFileLoader` — the
entire §3-6 command chain (`CParseProcessSPARQLTextCommand` →
`CParseSPARQLQueryCommand` → `CCalculateQueriesCommand` →
`CCalculateQueryCommand` → serializer) is identical between `sparqlfile` and
`sparqlserver`. Only the `CSPARQLStreamingWriter` sink differs (HTTP response
stream instead of a file), and each pooled HTTP connection gets its own
handler instance ("Pooled" in the class name) rather than sharing one global
processor.

## Relevance to the embedded facade design

`docs/FASTDOWNWARD_EMBEDDING.md`'s Phase 2 CQ design (`konclude_execute_sparql_query`)
targets exactly the single-BGP case this pipeline's `CSPARQLSimpleQueryParser`
path (§4) already supports without any Rasqal dependency — the scope match
is not incidental, it's why that design is viable at all in
`KoncludeEmbedded.pro`. The `expressionOntology`-is-the-live-revision finding
in §4 is the same open risk already tracked there (does executing a query
grow the KB's revision chain unboundedly at millions-of-calls scale) — not
re-resolved by this pipeline trace, still open.
