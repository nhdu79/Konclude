# Getting Started with Konclude

A quick-start guide for building, running, and finding your way around the Konclude source tree. For the full protocol/command reference (OWLlink command coverage, SPARQL support, changelog, licensing), see the main [README.md](../README.md).

## What is Konclude?

Konclude is a tableau-based reasoner for the Description Logic SROIQV(D) (i.e. SROIQ(D) + Nominal Schemas), covering almost all of OWL 2 DL. It's a C++/Qt command-line application that can also run as an OWLlink server or a SPARQL server.

## Building

Konclude uses **qmake**, not CMake. It targets **Qt 5.11+** — Qt 6 will not compile it (Qt 6 removed `QLinkedList`, which Konclude uses throughout).

```sh
qmake -o Makefile KoncludeWithoutRedland.pro   # no RDF/Redland dependency
make
```

If you have the bundled Redland/Raptor2/Rasqal libraries for your platform under `External/librdf/`, you can instead build `Konclude.pro` for full RDF-serialization and complex SPARQL (via Rasqal) support. Only Linux and Windows binaries are bundled — macOS users should stick with `KoncludeWithoutRedland.pro` unless they supply their own Redland build.

**On macOS with Homebrew**, the default `qt` formula is Qt 6. Install Qt 5 alongside it (it's keg-only, so it won't disturb your existing Qt 6):

```sh
brew install qt@5
/opt/homebrew/opt/qt@5/bin/qmake -o Makefile KoncludeWithoutRedland.pro
make -j$(sysctl -n hw.ncpu)
```

The build produces `Release/Konclude.app/Contents/MacOS/Konclude` on macOS (a `.app` bundle even though it's a console tool), or `Release/Konclude` / `Release/Konclude.exe` on Linux/Windows. The `Scripts/Konclude(.sh|.bat)` wrapper scripts point at that binary.

There's also `KoncludeEmbedded.pro`, which builds Konclude as a shared library (`libKonclude.so`/`.dylib`, no `main()`) exposing a plain C `extern "C"` facade for in-process embedding into other applications — see `docs/FASTDOWNWARD_EMBEDDING.md` for the design and `Source/Control/Interface/Embedded/` for the facade itself:

```sh
qmake -o MakefileEmbedded KoncludeEmbedded.pro
make -f MakefileEmbedded -j$(nproc)   # -j$(sysctl -n hw.ncpu) on macOS
```

### `compile_commands.json` for clangd

qmake doesn't emit a compilation database itself. Generate one with the [`compiledb`](https://github.com/nickdiego/compiledb) tool (`pip install compiledb`, ideally in a venv since Debian/Ubuntu's system Python is externally managed) against whichever `Makefile*` you care about:

```sh
compiledb -n -o compile_commands.json make -f MakefileEmbedded   # or plain `make` for the default Makefile
```

The `-n` flag does a `make -n` dry run, so this doesn't actually compile anything — safe to run anytime the source file list changes. Regenerate it after adding/removing files from whichever `.pro`/`.pri` target you're editing.

## Running

```sh
./Konclude -h    # full usage / flag reference
```

### Basic reasoning tasks

```sh
./Konclude classification -i ontology.owl.xml -o hierarchy.owl.xml
./Konclude consistency -i ontology.owl.xml
./Konclude satisfiability -i ontology.owl.xml -x http://example.org/onto#SomeClass
./Konclude realization -i ontology.owl.xml
```

Konclude natively reads **OWL 2 XML** or **OWL 2 Functional Style** ontologies. Convert other syntaxes (Turtle, Manchester, etc.) with [Protégé](http://protege.stanford.edu/) first, or build with Redland integration for experimental RDF parsing.

### OWLlink

```sh
./Konclude owllinkfile -i request.xml -o response.xml
./Konclude owllinkserver -p 8080
```

### SPARQL

```sh
./Konclude sparqlfile -s query.sparql -i ontology.owl.xml -o answers.xml
./Konclude sparqlserver -p 8080 -c Configs/querying-config.xml
```

### Common flags

| Flag | Meaning |
|---|---|
| `-i FILEPATH` | ontology / OWLlink request input file |
| `-o FILEPATH` | output / response file |
| `-s FILEPATH` | SPARQL request file |
| `-x IRI` | target entity IRI (satisfiability class, realization individual) |
| `-c FILEPATH` | config file (see `Configs/default-config.xml`) |
| `-w N` | number of processing threads (`AUTO` scales to core count) |
| `-p PORT` | listening port for `owllinkserver`/`sparqlserver` (default 8080) |
| `-v` | verbose loading/processing timings |

Try the fixtures in `Tests/` to sanity-check a build, e.g.:

```sh
./Konclude classification -i Tests/roberts-family-full-D.owl.xml -o out.owl.xml
```

## Project Structure

Everything lives under `Source/`, organized by layer, roughly following the request lifecycle from CLI/OWLlink/SPARQL input down to the tableau reasoning core and back out to a rendered response:

```
Source/
├── mainLoader.cpp        entry point (QCoreApplication, non-JNI builds)
├── Control/               orchestration
│   ├── Interface/          protocol front-ends: CommandLine, OWLlink, SPARQL, JNI
│   ├── Loader/              one CLoader subclass per run mode (classification,
│   │                        owllinkfile, sparqlserver, internal test/eval generators, ...);
│   │                        CDefaultLoaderFactory picks the loader from CLI args
│   └── Command/             internal command model (CCommand, CCommanderManager)
│                            that decouples interfaces from the reasoning engine
├── Parser/                 turns OWL2 XML / KRSS / OWLlink / SPARQL input into the
│                            internal ontology/axiom model
├── Reasoner/                the reasoning engine
│   ├── Ontology/             internal ontology/ABox model (CABox, entities, IRIs)
│   ├── Preprocess/            pre-tableau transforms (absorption, branch triggers, ...)
│   ├── Kernel/                 the tableau algorithm itself:
│   │                           Algorithm/, Calculation/, Process/ (expansion rules),
│   │                           Strategy/ (processing priority), Cache/ (sat/unsat caching),
│   │                           Task/, Manager/ (per-mode reasoner managers)
│   ├── Consistence/ + Consistiser/   consistency checking orchestration
│   ├── Classification/ + Classifier/  class/property hierarchy computation
│   ├── Taxonomy/               hierarchy data structures from classification
│   ├── Realization/ + Realizer/  individual → most-specific-class typing
│   ├── Query/ + Answerer/       conjunctive query / SPARQL query answering
│   ├── Generator/               builds kernel-ready structures from the ontology model
│   ├── Revision/                incremental ontology change handling
│   └── Triples/                  Redland-backed RDF triple store (Redland builds only)
├── Renderer/                serializes results back out (OWL2 XML, RDF via Redland)
├── Concurrent/ + Scheduler/  Konclude's own task/event scheduling framework that the
│                            reasoning kernel runs on (not plain Qt concurrency)
├── Context/                 custom memory-pool allocation contexts used by reasoning
│                            data structures for performance
├── Config/                  typed configuration system backing `-c FILEPATH` config files
├── Network/                 HTTP client/server plumbing for the OWLlink/SPARQL servers
├── Logger/                  CLogger singleton + LOG(LEVEL, domain, message, code) macro
├── Utilities/                foundational data structures, including Utilities/Container/ —
│                            modified QMap/QHash/QList integrated with Konclude's own
│                            memory management
└── Test/                    internal stress/benchmark/regression harnesses, wired up as
                             alternate CLI loader modes rather than a `make test` target
```

Other top-level directories:

- `Configs/` — example configuration files (`default-config.xml`, `querying-config.xml`).
- `Tests/` — sample ontologies and OWLlink/SPARQL request fixtures used for manual verification.
- `External/` — bundled third-party dependencies (Redland/librdf, hoard, vcpkg).
- `Scripts/` — thin wrapper scripts for launching the built binary.

### Conventions

- Classes are prefixed `C` (`CLoader`, `CABox`, `CConfiguration`, ...).
- Namespaces mirror the `Source/` path, e.g. `Konclude::Reasoner::Kernel`.
- Logging goes through the `LOG(LEVEL, "::Namespace::Path", message, code)` macro from `Logger/CLogger.h`, not `qDebug`/`std::cout`.
