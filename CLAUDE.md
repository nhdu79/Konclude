# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Konclude is a tableau-based reasoner for the Description Logic SROIQV(D) (SROIQ(D) + Nominal Schemas), covering almost all of OWL 2 DL. It is a C++/Qt application (`QCoreApplication`, no GUI) that exposes a command-line interface, an OWLlink server/file interface, and a SPARQL server/file interface.

## Build

Build system is qmake (Qt project files), not CMake.

```
qmake -o Makefile Konclude.pro     # full build, requires Redland RDF libs (for RDF/SPARQL-via-Rasqal support)
qmake -o Makefile KoncludeWithoutRedland.pro   # build without the Redland dependency
make
```

- Requires Qt 5.11+ (`xml network concurrent` modules) and a C++11 compiler.
- `Konclude.pro` links against the bundled Redland/Raptor2/Rasqal/libxml2 static libs under `External/librdf/<Platform>/<Arch>/lib/`. If these paths don't match your platform, either fix them in `Konclude.pro` or build `KoncludeWithoutRedland.pro` instead (this is what CI uses — see `.github/workflows/build-without-redland.yml`).
- `KoncludeLIB.pro` builds Konclude as a library with a JNI interface (`KONCLUDE_COMPILE_JNI_INTERFACE`, requires `JAVA_HOME`) instead of the standalone `main()` in `Source/mainLoader.cpp`.
- `KoncludeEmbedded.pro` builds Konclude as a shared library (`ReleaseEmbedded/libKonclude.so`) with a plain `extern "C"` facade (`KONCLUDE_COMPILE_EMBEDDED_INTERFACE`, no JNI/Java dependency) for in-process embedding — see `Source/Control/Interface/Embedded/` and the "Current focus" section below. `Tools/EmbeddedDriver/rebuild.sh` (run from repo root) regenerates `MakefileEmbedded` if needed, rebuilds the library, and relinks every `Tools/EmbeddedDriver/*.cpp` driver into a same-named binary at the repo root.
- A `gitbuild` pre-target runs `UnixGitBuildScript.sh` / `WinGitBuildScript.bat` to stamp the build with git revision info into `revision-git.h` before compiling — don't hand-edit `revision-git.h`.
- Output binary goes to `./Release/Konclude`. Wrapper scripts `Scripts/Konclude` / `Scripts/Konclude.sh` / `Scripts/Konclude.bat` invoke it.
- Docker builds are available via the separate `KoncludeDocker` repo (`./build_release.sh` there produces a statically linked binary); useful when local Qt/Redland setup is painful.

There is no CMake, no package.json, no separate lint step — treat qmake/make as the only build entry point.

## Running / manual verification

There is no unit test runner in the usual sense; verification is done by running the compiled binary against ontology/request fixtures in `Tests/`. For the embedded interface, the drivers built by `Tools/EmbeddedDriver/rebuild.sh` are the tests (`embedded_state_driver`, `embedded_cq_driver`, `embedded_state_benchmark`, …; exit code 0 = pass).

```
./Konclude classification -i Tests/roberts-family-full-D.owl.xml -o out.owl.xml
./Konclude satisfiability -i Tests/roberts-family-full-D.owl.xml -x http://www.co-ode.org/roberts/family-tree.owl#Aunt
./Konclude owllinkfile -i Tests/galen-classify-request.xml -o response.xml
./Konclude sparqlfile -s Tests/lubm-univ-bench-sparql-load-and-query-test.sparql -o Tests/query-answers.xml -c Configs/querying-config.xml
```

Commands: `classification`, `consistency`, `satisfiability` (needs `-x IRI`), `realisation`, `owllinkfile`, `owllinkserver` (`-p PORT`, default 8080), `sparqlfile`, `sparqlserver`. `-i` input file, `-o` output file, `-c` config file (see `Configs/default-config.xml`, `Configs/querying-config.xml`). `-h` prints full usage.

Konclude only natively parses OWL 2 XML and OWL 2 Functional Style ontologies; other syntaxes need converting first (e.g. via Protégé). RDF serializations only work if built with Redland integration.

`Source/Test/` and `Source/Control/Loader/CTest*Loader.h`, `CAnalyseReasonerLoader`, `CReasonerEvaluation*Loader` etc. contain internal stress/benchmark/regression harnesses (concurrent hash/memory testers, throughput testers, evaluation loaders) that are wired up as alternate CLI loader modes rather than a `make test` target — check `Source/Control/Loader/CDefaultLoaderFactory.cpp` and `CCommandLinePreparationTranslatorSelector` to see how a given loader is selected from CLI arguments before assuming a testing entry point exists.

## Architecture

Everything lives under `Source/`, organized by layer:

- **`Control/`** — application entry/orchestration.
  - `Control/Loader/` — one `CLoader` subclass per run mode (CLI batch processing per task, OWLlink file/server, SPARQL file/server, JNI, internal test/evaluation generators). `CDefaultLoaderFactory` + `CCommandLinePreparationTranslatorSelector` pick the loader from CLI args; this is the dispatch point to read first when tracing "what happens for command X".
  - `Control/Interface/` — protocol front-ends: `CommandLine/`, `OWLlink/`, `SPARQL/`, `JNI/`.
  - `Control/Command/` — the internal command model (`CCommand`, `CCommandBuilder`, `CCommanderManager`) that decouples interfaces from reasoning execution; commands carry preconditions/instructions/records and are dispatched to reasoner managers.
- **`Parser/`** — ontology/request parsing: OWL2 XML (`COWL2QtXMLOntologyParser`), KRSS, OWLlink/SPARQL request parsing, and axiom/ontology builders that turn parsed input into the internal ontology model.
- **`Reasoner/`** — the reasoning engine, the core of the codebase:
  - `Reasoner/Ontology/` — internal ontology/ABox model (`CABox`, IRI/entity representations).
  - `Reasoner/Preprocess/` — pre-tableau transformations (absorption, branch triggers, disjunct extraction, etc.).
  - `Reasoner/Kernel/` — the tableau algorithm itself: `Algorithm/`, `Calculation/`, `Process/` (expansion rules/linkers), `Strategy/` (processing-priority strategies), `Cache/` (satisfiability/unsatisfiability caching — the thing the README suggests disabling first if memory usage is too high), `Task/`, `Manager/` (per-mode reasoner managers, e.g. `CAnalyseReasonerManager`, `CExperimentalReasonerManager`).
  - `Reasoner/Consistence/` + `Reasoner/Consistiser/` — consistency checking orchestration on top of the kernel.
  - `Reasoner/Classification/` + `Reasoner/Classifier/` — class/property hierarchy computation.
  - `Reasoner/Taxonomy/` — hierarchy data structures produced by classification.
  - `Reasoner/Realization/` + `Reasoner/Realizer/` — instance realization (individual → most-specific-class typing).
  - `Reasoner/Query/` + `Reasoner/Answerer/` — conjunctive query / SPARQL query answering built on the tableau engine.
  - `Reasoner/Generator/` — builds internal "basement"/processing data structures from the ontology model for the kernel to consume.
  - `Reasoner/Revision/` — incremental ontology change/revision handling.
  - `Reasoner/Triples/` — Redland-backed RDF triple store integration (only relevant when built with Redland).
- **`Renderer/`** — serializes the internal ontology/answer model back out (OWL2 XML renderer, RDF renderer via Redland, expression render visitors).
- **`Concurrent/`** and **`Scheduler/`** — Konclude's own task/event scheduling and concurrency framework (event channels/handlers, memory-pool-aware task scheduling) that the reasoning kernel is built on; this is not plain Qt concurrency, it's a custom cooperative scheduler with its own memory pool distributors/releasers — expect kernel code to interact with it rather than spawning threads directly.
- **`Context/`** — custom memory-pool allocation contexts (`CMemoryPoolNewAllocationContext`, etc.) used pervasively by reasoning data structures for allocation performance.
- **`Config/`** — typed configuration system (config types, config data readers, `CConfiguration`) backing the `-c FILEPATH` config files, which are themselves OWLlink `Set`-command XML files (see `Configs/default-config.xml`).
- **`Network/`** — HTTP client/server plumbing (`Network/HTTP/`) used by the OWLlink/SPARQL servers.
- **`Logger/`** — the `CLogger` singleton + `LOG(LEVEL, domain, message, code)` macro used throughout instead of ad-hoc logging; multiple pluggable log observers (console, Qt debug, configurable).
- **`Utilities/`** — foundational data structures, including `Utilities/Container/` — **modified versions of Qt's `QMap`/`QHash`/`QList`** (`CQtManagedRestrictedModification*` files) that integrate with Konclude's own memory management. Use these container types rather than raw Qt containers in reasoning-hot-path code, consistent with existing surrounding code.

## Current focus: incremental reasoning for Powerlifted

The goal is to use Konclude from Powerlifted (`../powerlifted`, same parent folder) to reason over many search states (ABoxes) against one TBox, through `konclude_embedded.h`, run in a worker process (`Tools/KoncludeWorker/`; Powerlifted's `src/search/konclude/state_reasoner.h` uses it). **Read `docs/INCREMENTAL_REASONING.md` first.** It covers the Konclude author's guidance (e-mail in `TODO.md`), what the embedded API does today, the blocking reasoner-core bug, and the ordered task list for the next round. `docs/EMBEDDED_INTERFACE.md` covers how the interface works internally (command routing, revision layering, CQ pitfalls), plus how to build, debug and link it into Powerlifted.

- One C API call is one state change: `konclude_tell_and_retract_assertions(...)` (class and object-property facts; `konclude_tell_and_retract_axioms` is the class-only shorthand) creates a revision, tells/retracts, and installs it, as an OWLlink `<Tell>`/`<Retract>` does. Queries (`konclude_execute_conjunctive_query`) run against the installed head.
- Build native calls by creating the same command objects the OWLlink path creates (`COWLlinkQtXMLCommandParser` → `COWLlinkProcessor::processCustomsEvents`). Delegate them through `CPreconditionSynchronizer`; commands that only `COWLlinkProcessor` handles go through `CEmbeddedOWLlinkProcessor`. Don't hand-derive lower-level sequences.
- The reasoner-core bug "answers contain only the individuals the newest revision touched" (individual-ID collisions during full rebuilds) is fixed in `CConcreteOntologyUpdateBuilder` (commit `b413d14d`); `docs/INCREMENTAL_WORK_LOG.md` holds short progress notes per roadmap task; add to it as you work. Regression fixtures: `Tests/incremental-*-request.xml` (`./Release/Konclude owllinkfile -w 2 -i … -o …`). Temporary debug lines are tagged `// KDBG-TEMP` by convention; `grep -rn KDBG-TEMP Source` must be empty before committing.
- Memory: `konclude_destroy_reasoner` frees the handle's threads and reasoning data (task 5c), and most per-state leaks are fixed (task 5d: 1000 growing states peak at 543 MB instead of 1.54 GB). A small rest still survives destruction; only process exit frees it, so `Tools/KoncludeWorker/` runs Konclude in a worker process restarted every N changes. Never keep two handles alive at once (deadlock). Details: `docs/INCREMENTAL_WORK_LOG.md`, Tasks 5c and 5d.
- Keep the C API plain C and ABI-stable (the worker and Powerlifted's client build on it). Always use ≥2 worker threads (`-w 1` deadlocks).

## Task Scope

- If you notice a bug while working on something unrelated to it, do not fix it as part of that change. Report it instead (to the user, and/or as a note in the relevant `docs/*.md` file with file/line references and the failure scenario) and leave the fix for a dedicated follow-up task. This keeps unrelated diffs reviewable and avoids silently changing behavior the current task wasn't asked to touch.
- When documenting, favour conciseness to improve readability. When explaining a concept, always follow step-wise reasoning and include code entry points.

### Conventions

- Classes are prefixed `C` (e.g. `CLoader`, `CABox`, `CConfiguration`); this is consistent across the entire codebase.
- Namespaces mirror the `Source/` directory path (e.g. `Konclude::Control::Loader`, `Konclude::Reasoner::Kernel`).
- Logging goes through `LOG(LEVEL, "::Namespace::Path", message, code)` from `Logger/CLogger.h`, not `qDebug`/`std::cout`.
- License headers (LGPLv3, "Copyright (C) ... by the Konclude Developer Team") are present at the top of every source file — keep this pattern for new files.
- Because of the custom memory-pool/container integration described above, avoid introducing plain `std::` containers or raw Qt containers into performance-sensitive `Reasoner/Kernel/` and `Context/`-adjacent code without checking how neighboring code manages allocation.
