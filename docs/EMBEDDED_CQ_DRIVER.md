# Embedded Conjunctive-Query Driver

`Tools/EmbeddedDriver/embedded_cq_driver.cpp` is a standalone smoke test for
the embedded C API's conjunctive-query support
(`konclude_execute_conjunctive_query` and friends, see
`Source/Control/Interface/Embedded/konclude_embedded.h`). It is not part of
the qmake build — it's a small, separately-compiled C++ program that only
talks to Konclude through the plain C API, no Qt types crossing that
boundary.

It loads `Tests/roberts-family-full-D.owl.xml` through the embedded
interface, runs the same query as `Tests/roberts-family-simple-cq-test.sparql`
(`Father(?x), hasChild(?x,?y)`), and diffs the `(x,y)` pairs it gets back
against the trusted CLI-computed answer set in `cq-answers.xml` (produced via
`./Konclude sparqlfile -s Tests/roberts-family-simple-cq-test.sparql -i
Tests/roberts-family-full-D.owl.xml -o cq-answers.xml`).

## Build

### Step 1 — build the embedded shared library

```sh
qmake -o MakefileEmbedded KoncludeEmbedded.pro
make -f MakefileEmbedded -j$(nproc)
```

- `KoncludeEmbedded.pro` is one of several qmake project files in the repo
  (alongside `Konclude.pro` for the CLI binary, `KoncludeLIB.pro` for the JNI
  build, etc.). It sets `TEMPLATE = lib` + `CONFIG += shared` and defines
  `KONCLUDE_COMPILE_EMBEDDED_INTERFACE`, which does two things at compile
  time: it excludes `Source/mainLoader.cpp`'s `main()` (so there's no CLI
  entry point competing for `main`), and it compiles in
  `Source/Control/Interface/Embedded/` (the C API facade), which is normally
  `#ifdef`-guarded out of every other build variant.
- `qmake -o MakefileEmbedded KoncludeEmbedded.pro` reads that `.pro` file and
  generates a plain `Makefile` named `MakefileEmbedded` (the `-o` names the
  output file explicitly so it doesn't clobber a `Makefile` from a different
  variant you might also have built, e.g. the plain CLI one).
- `make -f MakefileEmbedded -j$(nproc)` compiles all ~2500 translation units
  and links them into `ReleaseEmbedded/libKonclude.so` (plus versioned
  symlinks `libKonclude.so.1`, `libKonclude.so.1.0`, pointing at
  `libKonclude.so.1.0.0`). `-j$(nproc)` parallelizes across all CPU cores.
  `-f MakefileEmbedded` is needed because plain `make` would look for a file
  literally named `Makefile`, not `MakefileEmbedded`.
- Only needs to be redone when C++ source changes — not before every driver
  run.

### Step 2 — compile the driver against that library

```sh
g++ -std=c++11 -I Source/Control/Interface/Embedded \
    Tools/EmbeddedDriver/embedded_cq_driver.cpp \
    -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
    -o embedded_cq_driver
```

- `-I Source/Control/Interface/Embedded` adds that directory to the include
  search path so `#include "konclude_embedded.h"` in the driver resolves.
- `-L ReleaseEmbedded -lKonclude` tells the linker where to find the library
  (`-L`) and to link against `libKonclude.so` (`-l` + the `Konclude` part of
  the filename, dropping the `lib`/`.so`).
- `-Wl,-rpath,ReleaseEmbedded` passes `-rpath,ReleaseEmbedded` through to the
  linker, baking the library's directory into the driver binary as a runtime
  search path. Without this, running `./embedded_cq_driver` fails with an
  "error while loading shared libraries" because `.so` files in a relative,
  non-system directory aren't found by default.
- `-o embedded_cq_driver` names the output binary.

Both steps are one-time (until you edit either side); after that just re-run
the binary.

## Debug build (for gdb / lldb / nvim-dap)

The steps above build both the library and the driver under qmake's
`CONFIG += release` optimization settings, which strip usable debug info.
To step through code (either the driver or `Source/Control/Interface/Embedded/`
itself) in a debugger, rebuild both with `-g -O0` instead:

```sh
# Library: regenerate MakefileEmbedded with debug flags, then clean + rebuild
qmake -o MakefileEmbedded "QMAKE_CXXFLAGS_RELEASE=-g -O0" KoncludeEmbedded.pro
make -f MakefileEmbedded clean
make -f MakefileEmbedded -j$(nproc)

# Driver: compile+link with matching flags
g++ -std=c++11 -g -O0 -I Source/Control/Interface/Embedded \
    Tools/EmbeddedDriver/embedded_cq_driver.cpp \
    -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
    -o embedded_cq_driver
```

- **Use `QMAKE_CXXFLAGS_RELEASE=-g -O0`, not `QMAKE_CXXFLAGS+=-g -O0`.**
  qmake's `CONFIG += release` (set in `KoncludeEmbedded.pro`) injects its own
  `-O2` via the `QMAKE_CXXFLAGS_RELEASE` mkspec variable, appended *after*
  `QMAKE_CXXFLAGS` on the actual compile command line. GCC honors the last
  `-O` flag it sees, so `QMAKE_CXXFLAGS+=-g -O0` silently produces
  `g++ ... -g -O0 -O2 ...` — still optimized, just with debug info bolted on
  (learned the hard way: the first attempt at this passed `file`/`readelf`
  checks for having `.debug_info`, but variables were still getting optimized
  away when actually breakpointed). Overriding `QMAKE_CXXFLAGS_RELEASE`
  directly replaces qmake's `-O2` instead of losing an ordering fight with it.
  Verify with `grep '^CXXFLAGS' MakefileEmbedded` before rebuilding — it
  should show `-O0` and no `-O2`.
- **`make -f MakefileEmbedded clean` is required**, not optional, before the
  rebuild — qmake-generated Makefiles don't track compiler-flag changes as a
  reason to recompile an object file whose source hasn't changed, so without
  `clean` you'd silently keep the stale `-O2` (or plain release) objects from
  a prior build.
- This is a full rebuild of all ~2500 translation units at `-O0`, so expect
  it to take noticeably longer than the optimized build, and produce a much
  larger `.so` (roughly 350MB optimized-with-symbols vs. 500MB+ at true
  `-O0` in one measurement).
- Sanity-check the result before trusting a debugger against it:
  ```sh
  file ReleaseEmbedded/libKonclude.so.1.0.0        # should say "not stripped"
  readelf -S ReleaseEmbedded/libKonclude.so.1.0.0 | grep debug_info   # should print a row
  ldd embedded_cq_driver | grep Konclude           # confirms it resolves to ReleaseEmbedded/, not a stale copy elsewhere
  ```
- To go back to a fast/optimized build afterward, regenerate once more
  without the override (`qmake -o MakefileEmbedded KoncludeEmbedded.pro`),
  `make -f MakefileEmbedded clean`, rebuild.

### Setting breakpoints inside the library

`libKonclude.so` is a direct link-time dependency (`-lKonclude`), so the
dynamic linker maps it into the process before `main()` runs — there's no
"library not loaded yet" timing concern like with a `dlopen`'d plugin.
Breakpoints set in `Source/Control/Interface/Embedded/*.cpp` should bind
("verify") as soon as the debug session starts. If using gdb/`cppdbg`
non-interactively (e.g. via a DAP adapter), it can help to force pending
breakpoints explicitly rather than rely on the interactive prompt:
```
-gdb-set breakpoint pending on
```
`codelldb`/lldb-based setups don't need this — they treat breakpoints
against not-yet-resolved modules as pending by default.

## Run

```sh
./embedded_cq_driver [Tests/roberts-family-full-D.owl.xml cq-answers.xml]
```

Arguments are optional -- with none, the driver defaults to that exact
fixture pair, so it also runs correctly when launched from a debugger with
no `args` configured (a launch config omitting `args` was the cause of an
earlier "session exits immediately" symptom while debugging this). Pass
both arguments together to override; passing exactly one is a usage error.

Prints the row/variable counts it got back, then either:
- `PASS: embedded conjunctive-query results match the trusted CLI (sparqlfile) output exactly.`
- `FAIL: ...` plus up to 10 example rows present in only one of the two result sets.

Exit code is `0` on pass, `1` on failure/mismatch, `2` on bad arguments.

## Fixed bugs found via this driver

The driver initially could not complete a run at all. Getting it to a clean
`PASS` (314/314 rows matching the trusted CLI answer set) surfaced four
distinct, previously-unnoticed bugs in the embedded interface -- consistent
with `docs/FASTDOWNWARD_EMBEDDING.md`'s note that the embedded facade had
never been smoke-tested end-to-end before this driver was written.

1. **Missing command delegater for load/consistency/satisfiability
   (permanent hang).** `konclude_load_ontology_file` (and
   `konclude_check_consistency`/`konclude_check_satisfiability`) hung
   indefinitely. `CLoadKnowledgeBaseOWLAutoOntologyCommand`,
   `CIsConsistentQueryCommand`, and `CProcessClassNameSatisfiableQueryCommand`
   are only ever handled inside `COWLlinkProcessor::processCustomsEvents`, but
   `CEmbeddedReasoner`'s constructor never instantiated any
   `COWLlinkProcessor`-derived delegater at all -- those commands were
   dispatched but never processed, so
   `CCommandExecutedBlocker::waitExecutedCommand()` blocked forever. Verified
   pre-existing (not caused by the conjunctive-query work) via `git stash`
   against the last-committed baseline, rebuild, and reproducing identically
   there. Fixed by adding `CEmbeddedOWLlinkProcessor`
   (`Source/Control/Interface/Embedded/CEmbeddedOWLlinkProcessor.h/.cpp`), a
   minimal `COWLlinkProcessor` subclass modeled on the proven
   `Control/Interface/JNI/CJNICommandProcessor`, and routing those three
   commands through it instead of `mPreconditionSynchronizer`.
2. **`parseQueryTextList` vs `parseQueryText` mixup.**
   `CEmbeddedReasoner::executeConjunctiveQuery()` called
   `parseQueryTextList(QStringList() << sparqlSelectQuery)`, but that method
   expects an ALREADY-TOKENIZED list of parts (its loop appends each entry as
   one token, no further splitting) -- not a list of full query strings
   despite the name. Passing one untokenized blob meant nothing ever matched
   a keyword, so zero queries were built. Fixed by calling the singular
   `parseQueryText(sparqlSelectQuery)`, which does its own tokenization.
3. **Missing triples-indexing precondition.** Even with a correctly parsed
   query, `CCalculateQueryCommand` returned zero result rows.
   `CCalculateQueryCommand`'s handler (`CCommanderManagerThread::processCustomsEvents`)
   forwards the query straight to the reasoner manager without ensuring
   triples indexing has run; the proven `checkSatisfiability` path explicitly
   runs `CPreprocessKnowledgeBaseRequirementsForQueryCommand` first as a
   precondition. Fixed by adding that command (dispatched via
   `mPreconditionSynchronizer`, waited on) before `CCalculateQueryCommand`.
4. **Query built against the wrong ontology object.** Still zero rows after
   fix 3. The real CLI/OWLlink SPARQL path (`CParseProcessSPARQLTextCommand`'s
   handling, ultimately `COWLlinkProcessor`'s `CParseSPARQLQueryCommand`
   branch) creates a **new** `CCreateKnowledgeBaseRevisionUpdateCommand`
   revision *before* building the query, and builds against *that* fresh
   `CConcreteOntology` object -- not the already-current one. Building
   against the already-current ontology directly parses fine (variable names
   come out correct) but resolves zero matches, because term-to-concept
   lookups in `CConcreteOntologyQueryBasicBuilder` fall back to `nullptr` on
   any miss rather than erroring. Fixed by reordering
   `executeConjunctiveQuery()` to create the new revision first (plain
   `CCreateKnowledgeBaseRevisionUpdateCommand(mKnowledgeBaseName)`, no preset)
   and build the query against its `getOntology()`, rather than fetching the
   current revision via `CGetCurrentKnowledgeBaseRevisionCommand` first.

## Benchmark driver (`embedded_cq_benchmark`)

`Tools/EmbeddedDriver/embedded_cq_benchmark.cpp` is the performance-focused
sibling of `embedded_cq_driver.cpp` above — same embedded C API, same build
prerequisites (`libKonclude.so` from `KoncludeEmbedded.pro`), but instead of
a single pass/fail check it times N independent, fully repeated cycles of
**create reasoner → load ABox → answer a conjunctive query → destroy
reasoner**, for one or more named scenarios.

### Why a full cycle, not just the query

Each iteration deliberately does the whole thing from scratch, not just
re-runs the query against an already-loaded reasoner:

- **"Load the ABox again"**: `konclude_load_ontology_file` may only be
  called once per handle (see its doc comment in `konclude_embedded.h`), so
  repeating the load means a brand-new `konclude_create_reasoner()` /
  `konclude_destroy_reasoner()` pair every iteration, not a second load call
  reusing one instance.
- **"Full construction as a new [query]"**: `konclude_execute_conjunctive_query()`
  always parses its query-text argument and builds a fresh `CQuery` from
  scratch internally (see `CEmbeddedReasoner::executeConjunctiveQuery()` and
  `docs/EMBEDDED_EXECUTE_CONJUNCTIVE_QUERY.md`) — passing the identical query
  string every iteration already satisfies this without any extra code in
  the driver.

### Build

Same library, one extra `g++` invocation for this second binary:

```sh
g++ -std=c++11 -O2 -I Source/Control/Interface/Embedded \
    Tools/EmbeddedDriver/embedded_cq_benchmark.cpp \
    -L ReleaseEmbedded -lKonclude -Wl,-rpath,ReleaseEmbedded \
    -o embedded_cq_benchmark
```

(`-O2`, not `-g -O0` — this is a timing measurement, so it should reflect a
normal optimized build, not the debug build described above for stepping
through code.)

### Run

```sh
./embedded_cq_benchmark [iterations] [roberts-family|robot-grid|all]
```

Defaults: `iterations=1000`, scenario=`all` (runs every scenario in
sequence, back to back, each with its own stats block).

### Scenarios

| Scenario | Ontology | Query | Expected rows |
|---|---|---|---|
| `roberts-family` | `Tests/roberts-family-full-D.owl.xml` | `Father(?x), hasChild(?x,?y)` | 314 |
| `robot-grid` | `Tests/robot22.owl.xml` | `LeftOf15(?x), BelowOf15(?x)` | 1 (`robot`) |

`robot-grid` exercises a very different shape of problem than
`roberts-family`: a large TBox (22×22 grid of `LeftOf`/`RightOf`/`AboveOf`/`BelowOf`
classes chained by `SubClassOf`, e.g. `LeftOf10 ⊑ LeftOf11 ⊑ … ⊑ LeftOf21`)
against a tiny ABox (a single asserted individual, `robot`, typed
`RightOf1`, `LeftOf10`, `AboveOf0`, `BelowOf10`). The query only matches
because subsumption reasoning over the `LeftOf`/`BelowOf` chains entails
`LeftOf15(robot)` and `BelowOf15(robot)` from the asserted `LeftOf10`/`BelowOf10`
facts — a useful contrast against `roberts-family`'s small-TBox/large-ABox
shape.

Each scenario's expected row count is checked every single iteration (not
just the first) — a cycle whose row count doesn't match is counted as a
mismatch and included in the pass/fail exit code, so a regression that only
shows up intermittently (e.g. a flaky ordering issue) won't be silently
averaged away.

### Output

For each scenario: per-iteration progress lines (every 5% of the run), then
a summary with per-phase timing broken out (`create`, `load`, `query`,
`destroy`, `total/cycle`), each as total/mean/min/max in milliseconds.
Exit code is `0` only if every scenario run had zero failures and zero
row-count mismatches across all iterations.

### Measured numbers (this ontology/query pair, this machine)

From 10-iteration runs during development (see conversation history for raw
output) — indicative only, not a committed performance baseline:

- **`roberts-family`**: ~4.6 s/cycle, almost entirely in the `query` phase
  (~4.58 s of it); `load` is comparatively cheap (~20 ms).
- **`robot-grid`**: ~39 ms/cycle total, `query` phase ~29 ms, `load` ~7 ms —
  roughly two orders of magnitude faster per cycle than `roberts-family`,
  consistent with its tiny ABox (one individual) versus `roberts-family`'s
  much larger one.

At `roberts-family`'s per-cycle cost, the full default 1000 iterations for
that scenario alone takes on the order of an hour; `robot-grid`'s 1000
iterations takes well under a minute. Pass an explicit `iterations` argument
and/or a single scenario name to `robot-grid` for quick iteration during
development, and reserve `all`/large iteration counts for runs where the
long wall-clock time is expected and acceptable.
