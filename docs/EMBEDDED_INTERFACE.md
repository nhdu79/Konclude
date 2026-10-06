# Embedded Interface (`libKonclude.so`)

How the in-process C API works, why it is built this way, and how to build,
test, debug and link it. For the current status, known bugs and next tasks,
see `docs/INCREMENTAL_REASONING.md`.

Code: `Source/Control/Interface/Embedded/`. Build file: `KoncludeEmbedded.pro`.

## 1. Design decisions

| Decision | Why | Where |
|---|---|---|
| Shared library, no `main()` | Same ~2500 files as the CLI (`include(Konclude.pri)`). `KONCLUDE_COMPILE_EMBEDDED_INTERFACE` turns off `main()` in `Source/mainLoader.cpp` and turns on `Control/Interface/Embedded/`. | `KoncludeEmbedded.pro`, `Source/mainLoader.cpp` |
| Plain `extern "C"` API, opaque `void*` handle | C++ ABI (name mangling, STL layout, exceptions) is not stable across toolchains. Callers need no Qt. | `konclude_embedded.h`, `CEmbeddedInterfaceCAPI.cpp` |
| One `QCoreApplication` per process, `exec()` never called | Qt needs the instance. Commands complete through Konclude's own blocking primitives (`CCommandExecutedBlocker`), not the Qt event loop. Same approach as the JNI bridge. | `CEmbeddedReasoner::ensureQCoreApplication()` |
| ≥2 worker threads, always | `-w 1` deadlocks at startup (`CReasonerManagerThread::threadStarted()` on `QSemaphore::acquire()`). Set per `CConfiguration` via `createAndSetConfig("Konclude.Calculation.ProcessorCount")`, as the CLI's `-w N` does. Re-registering the config description creates a second description that nothing reads. | `CEmbeddedReasoner()` |
| CQ settings forced on | `ComplexQueryingSupport=true`, `RepresentativePropagation=false`, `SignatureMirroringBlocking=false`, the same values `CSPARQLFileComandLinePreparationTranslator` sets for `sparqlfile`. | `CEmbeddedReasoner()` |
| Results copied out eagerly | Results are copied into Qt containers right after the query, so the `CQuery` graph can be deleted. Accessors index into the copies, which stay valid until the next call. | `CEmbeddedQueryManager` |

Not yet done: hidden symbol visibility (`-fvisibility=hidden` + exporting
only `konclude_*`), and a config-string input (there is no counterpart to
the JNI `CConfigJNIReader`).

## 2. Object structure and command routing

```
konclude_* (CEmbeddedInterfaceCAPI.cpp)
  └─ CEmbeddedReasoner        config, commander, routing, error capture (CAbstractLogObserver)
       ├─ CEmbeddedOntologyLoader   load ontology; tell/retract → new installed revision
       └─ CEmbeddedQueryManager     consistency / satisfiability / SPARQL CQ
```

Every command is delegated and then waited on with
`CCommandExecutedBlocker::waitExecutedCommand(cmd)`. Two routes exist:

1. **`CPreconditionSynchronizer`** (wraps `CCommanderManagerThread`): KB
   creation, revision create/install, query preprocessing and calculation.
   The synchronizer retries a command until its preconditions hold, such as
   the reasoner still being initialized. Delegating straight to the
   commander hangs.
2. **`CEmbeddedOWLlinkProcessor`** (a minimal `COWLlinkProcessor`, modeled
   on `JNI/CJNICommandProcessor`): commands that only
   `COWLlinkProcessor::processCustomsEvents` handles, such as
   `CLoadKnowledgeBaseOWLAutoOntologyCommand`, `CIsConsistentQueryCommand`,
   `CProcessClassNameSatisfiableQueryCommand` and
   `CPrepareKnowledgeBaseForRevisionCommand`. Sent anywhere else, they are
   never processed and the wait never returns.

Rule for new calls: find what `COWLlinkQtXMLCommandParser` builds for the
matching OWLlink request, then follow how `COWLlinkProcessor` dispatches it.

## 3. Revisions: what a state change and a query do

1. **Load** (`loadOntologyFile`): `CCreateKnowledgeBaseCommand`, then
   `CLoadKnowledgeBaseOWLAutoOntologyCommand`. Only once per handle. Errors
   show up only as log messages (`konclude_last_error`).
2. **State change** (`tellAndRetractAssertions`):
   1. `CCreateKnowledgeBaseRevisionUpdateCommand`: new `COntologyRevision`
      on top of the head. `new CConcreteOntology(currOnt, …)` shares the
      TBox/RBox/ABox vectors by pointer
      (`CDynamicReferenceVectorBase::referenceVector`), so this is O(1) in
      ontology size.
   2. `CConcreteOntologyUpdateCollectorBuilder`: retracts, then tells, for
      `ClassAssertion` and `ObjectPropertyAssertion` facts (an
      `AssertionChangeSet`).
      `retractOntologyAxiom` only removes axioms already in the inherited
      tell set.
   3. `CInstallKnowledgeBaseRevisionUpdateCommand`: becomes the head. Is
      stored in `CSPOntologyRevisionManager::onRevContainer` and never freed.

   Installing is required. A told revision that was never installed was
   tried: queries on it returned zero rows.
3. **Query** (`executeConjunctiveQuery`):
   1. **Query layer.** Created lazily on top of the head
      (`CCreateKnowledgeBaseRevisionUpdateCommand`), never installed. Reused
      until the next state change (`resetForNewState()` deletes it). A layer
      is needed: building the query directly against the head parses fine,
      but every term resolves to `nullptr`
      (`CConcreteOntologyQueryBasicBuilder` returns `nullptr` on a miss), so
      the result has zero rows.
   2. **Parse.** Uses `CConcreteOntologyUpdateSeparateHashingCollectorBuilder`,
      `CConcreteOntologyQueryExtendedBuilder(baseOnt, onto, …)` and
      `CSPARQLSimpleQueryParser::parseQueryText()`. This is the parser
      without Rasqal: one SELECT with one basic graph pattern. Don't use
      `parseQueryTextList()`: it expects already-split tokens, so with a
      whole query string it builds nothing.
   3. **`CPreprocessKnowledgeBaseRequirementsForQueryCommand`.** Builds the
      triples index. Without it the calculation silently returns zero rows.
   4. **`CCalculateQueryCommand`.** Runs the reasoning.
   5. **Copy results.** Walk the rows and cells as
      `CSPARQLXMLAnswerSerializer` does. The cell iterator is reused across
      rows. Then delete the `CQuery`.

A `CQuery` (`CComplexAnsweringQuery`) is tied to the two ontologies it was
built against: there is no setter for them. So it can't be reused across
states, and every query is built again. A SPARQL-free option would build
the query through `CSPARQLQueryBuilder` (it takes `CAxiomExpression*` lists
and variables, no strings). That is only worth doing if profiling shows
text parsing matters (not measured yet).

## 4. Build, test, debug

```sh
Tools/EmbeddedDriver/rebuild.sh      # from repo root
```

What the script does:

1. Finds a **Qt 5** `qmake`. Qt 6 dropped `QLinkedList`, which the code
   uses. Override it with `QMAKE=…`.
2. (Re)generates `MakefileEmbedded` when `KoncludeEmbedded.pro` or
   `Konclude.pri` changed. It uses `-g -O0`.
3. Builds `ReleaseEmbedded/libKonclude.so.1.0.0`, whose SONAME is
   `libKonclude.so.1`.
4. Links each `Tools/EmbeddedDriver/*.cpp` into a same-named binary at the
   repo root. Each binary gets an rpath pointing at the library.

Drivers: run them from the repo root; exit code 0 means pass.

| Driver | Checks |
|---|---|
| `embedded_cq_driver` | 314 `(x,y)` rows for `Father(?x), hasChild(?x,?y)` on `Tests/roberts-family-full-D.owl.xml`, the same as `cq-answers.xml` from the CLI `sparqlfile` |
| `embedded_state_driver` | tell/retract diffs across 4 states on `Tests/embedded-state-tbox-only.owl.xml` |
| `embedded_property_driver` | property and class tell/retract across 4 states on `Tests/embedded-state-tbox-roles.owl.xml`, plus a rejected invalid call |
| `embedded_state_benchmark [maxFacts step repeats queriesPerState]` | switches between states A and B, timing tell/retract and queries, and checking each answer set |
| `embedded_cq_benchmark [iterations] [roberts-family\|robot-grid\|all]` | full create → load → query → destroy cycles. Measured earlier: roberts-family about 4.6 s/cycle (almost all query), robot-grid about 39 ms/cycle |
| `embedded_state_existing_abox_probe`, `embedded_scratch_revision_probe` | a tell on top of an existing ABox; create/delete cycles of never-installed revisions (check for hangs; run under `timeout`) |

Debug build notes:

- Use `QMAKE_CXXFLAGS_RELEASE=-g -O0`, not `QMAKE_CXXFLAGS+=…`. The release
  `-O2` comes later on the command line and wins. Check with
  `grep '^CXXFLAGS' MakefileEmbedded`.
- Run `make -f MakefileEmbedded clean` after changing flags. qmake Makefiles
  don't rebuild when only flags change.
- The library is linked at build time, so breakpoints in
  `Control/Interface/Embedded/*.cpp` bind as soon as the program starts. With
  gdb through DAP, set `-gdb-set breakpoint pending on`.
- Library `compile_commands.json` for clangd:
  `compiledb -n -o compile_commands.json make -f MakefileEmbedded`.

## 5. Linking into Powerlifted

Powerlifted (`../powerlifted`) builds with CMake and links the library as a
prebuilt imported target. The setup below is already in place.

1. **Build the library** (§4).
2. **Declare it in `src/search/CMakeLists.txt`:**
   ```cmake
   set(KONCLUDE_REPO_ROOT "/home/zinzin2312/repos/Konclude" CACHE PATH "Konclude checkout built with KoncludeEmbedded.pro")
   add_library(Konclude SHARED IMPORTED)
   set_target_properties(Konclude PROPERTIES
       IMPORTED_LOCATION "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded/libKonclude.so.1.0.0"
       INTERFACE_INCLUDE_DIRECTORIES "${KONCLUDE_REPO_ROOT}/Source/Control/Interface/Embedded")
   target_link_libraries(search PRIVATE project_options Konclude)
   set_target_properties(search PROPERTIES
       BUILD_RPATH "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded"
       INSTALL_RPATH "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded")
   ```
   This links the checkout directly, without copying it. Vendor a copy
   under `src/search/third_party/konclude/` once the API is stable.
3. **Wrapper and switches.** The RAII wrapper is
   `src/search/konclude_reasoner.h`. Linking is opt-in with
   `build.py --konclude` (CMake `USE_KONCLUDE`); using it needs
   `--use-konclude`.
4. **Check the link:** `ldd builds/release/search/search | grep -i konclude`
   must point into `ReleaseEmbedded/`.

Troubleshooting:

- `libKonclude.so.1 => not found`: the rpath is missing, or the CMake cache
  holds a stale `KONCLUDE_REPO_ROOT`. Delete `builds/<type>` and rebuild.
- Undefined `konclude_*` symbols: Konclude isn't linked into that target.
  `datalog_test` needs it too if it uses the reasoner.
- At runtime the Qt5 `.so` files (`libQt5Core`, `Xml`, `Network`,
  `Concurrent`) must be installed on the machine that runs `search`.
- macOS: `KoncludeEmbedded.pro` sets `-install_name @rpath/`. Without it,
  dyld ignores the consumer's rpath.
