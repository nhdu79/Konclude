# Linking Konclude into Powerlifted

Stepwise, verified instructions for linking the Konclude embedded shared
library into [Powerlifted](https://github.com/abcorrea/powerlifted) (the
Fast-Downward-derived planner this repo's embedding work targets — see
`docs/FASTDOWNWARD_EMBEDDING.md` for the design rationale). Every command and
code snippet below was actually run against these two repos on this machine
and is copy-pasteable as-is:

- Konclude repo: `/home/zinzin2312/repos/Konclude`
- Powerlifted repo: `/home/zinzin2312/repos/powerlifted`

Powerlifted builds with **CMake** (`build.py` → `cmake` + `make` in
`builds/<debug|release>/search`, target executable `search`,
`src/search/CMakeLists.txt`), not qmake — Konclude's build system. The two
build systems don't need to know about each other: Konclude produces a
`.so`, Powerlifted's CMake links against it as a prebuilt (imported) library,
exactly the same way it would link any other third-party `.so`.

## 0. Why this shape

Per `docs/FASTDOWNWARD_EMBEDDING.md` §2/§5/§6, the embedded interface is
deliberately a plain `extern "C"` facade (`konclude_embedded.h`) with no Qt
or C++ types crossing the boundary, specifically so a consumer like
Powerlifted never needs Qt headers, Qt's C++ ABI, or qmake in its own build.
Powerlifted only needs:
1. One header: `Source/Control/Interface/Embedded/konclude_embedded.h`.
2. One shared library: `libKonclude.so.1.0.0` (produced by
   `KoncludeEmbedded.pro`).

## 1. Build `libKonclude.so` (Konclude repo)

```sh
cd /home/zinzin2312/repos/Konclude
qmake -o Makefile KoncludeEmbedded.pro
make -j$(nproc)
```

This compiles all of Konclude (~2500 files, no `main()` — see
`docs/FASTDOWNWARD_EMBEDDING.md` §1) and produces, verified on this machine:

```
ReleaseEmbedded/
  libKonclude.so -> libKonclude.so.1.0.0
  libKonclude.so.1 -> libKonclude.so.1.0.0
  libKonclude.so.1.0 -> libKonclude.so.1.0.0
  libKonclude.so.1.0.0        (27 MB, ELF 64-bit shared object)
```

`readelf -d ReleaseEmbedded/libKonclude.so.1.0.0 | grep SONAME` confirms the
runtime linker looks for it as **`libKonclude.so.1`** — that's the name that
matters for rpath/`ldd` resolution below, not the unversioned `.so` symlink.

**Runtime dependency note** (only matters once the `.so` is *loaded*, not at
Powerlifted's compile time): `ldd ReleaseEmbedded/libKonclude.so.1.0.0`
shows it dynamically links `libQt5Xml.so.5`, `libQt5Network.so.5`,
`libQt5Concurrent.so.5`, `libQt5Core.so.5`, plus their own transitive deps
(ICU, glib, krb5, etc.) — all resolved automatically here because Qt5 is
installed via `apt` (`qtbase5-dev`) at the standard multiarch path
(`/lib/x86_64-linux-gnu/`), which is already on the default dynamic linker
search path. **No Qt `-dev`/headers are needed on Powerlifted's side, but the
Qt5 runtime `.so`'s must be installed/discoverable on whatever machine runs
the final `search` binary.** If Powerlifted is ever deployed somewhere
without system Qt5, either install the `libqt5xml5`/`libqt5network5`/
`libqt5concurrent5`/`libqt5core5a` runtime packages there, or vendor those
`.so`'s alongside `libKonclude.so` and extend the rpath in step 3 to cover
them too.

Rebuild this any time embedded-interface source changes — Powerlifted's
CMake step doesn't rebuild Konclude, it just links against whatever's
already in `ReleaseEmbedded/`.

## 2. Vendoring decision: reference the checkout directly, don't copy

Since both repos live side-by-side on this machine and the embedded
interface is still under active development (`konclude_tell_and_retract_axioms`/
`konclude_execute_conjunctive_query` are currently stubs — see
`Source/Control/Interface/Embedded/konclude_embedded.h`), point Powerlifted's
CMake straight at the Konclude checkout's build output
(`/home/zinzin2312/repos/Konclude/ReleaseEmbedded` +
`/home/zinzin2312/repos/Konclude/Source/Control/Interface/Embedded`) rather
than copying the header/`.so` into Powerlifted's tree. Rebuilding Konclude
(step 1) then immediately shows up the next time Powerlifted links, with no
copy step to remember. Once the interface stabilizes and this stops being
co-developed daily, switch to vendoring a copy under
`src/search/third_party/konclude/{include,lib}` (mirroring the existing
`src/search/parallel_hashmap/` vendored-dependency pattern) for a
self-contained, reproducible Powerlifted checkout — not done here since it'd
go stale immediately at this stage.

## 3. Wire `src/search/CMakeLists.txt` (Powerlifted repo)

Add near the top of `/home/zinzin2312/repos/powerlifted/src/search/CMakeLists.txt`
(verified — configures, builds, links, and runs cleanly, including a clean-env
run with no `LD_LIBRARY_PATH` set):

```cmake
# --- Konclude embedded reasoner ---
set(KONCLUDE_REPO_ROOT "/home/zinzin2312/repos/Konclude" CACHE PATH
    "Path to a Konclude checkout built with KoncludeEmbedded.pro (see docs/EMBEDDED_LINKING_POWERLIFTED.md in that repo)")

add_library(Konclude SHARED IMPORTED)
set_target_properties(Konclude PROPERTIES
    IMPORTED_LOCATION "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded/libKonclude.so.1.0.0"
    INTERFACE_INCLUDE_DIRECTORIES "${KONCLUDE_REPO_ROOT}/Source/Control/Interface/Embedded"
)
```

Then, further down, extend the *existing* `target_link_libraries(search PRIVATE project_options)`
line (do not add a second, separate `target_link_libraries(search ...)` call
for this — appending to the existing one is just as correct, but keeping it
in one place matches the file's current style):

```cmake
target_link_libraries(search PRIVATE project_options Konclude)

# So the built `search` binary finds libKonclude.so.1 at runtime without
# LD_LIBRARY_PATH gymnastics (Qt5's own .so's resolve via the system's
# default linker search path already -- see step 1's runtime-dependency note).
set_target_properties(search PROPERTIES
    BUILD_RPATH "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded"
    INSTALL_RPATH "${KONCLUDE_REPO_ROOT}/ReleaseEmbedded"
)
```

`-DKONCLUDE_REPO_ROOT=...` on the `cmake` command line overrides the default
path for anyone with a differently-located Konclude checkout; `build.py`
doesn't currently expose a flag for extra `-D` options, so either edit the
default above or invoke `cmake` manually in `builds/release/search` with the
override the first time (CMake then caches it).

## 4. A small RAII wrapper (Powerlifted repo)

Add e.g. `src/search/konclude_reasoner.h`:

```cpp
#pragma once

#include "konclude_embedded.h"

#include <string>
#include <vector>

class KoncludeReasoner {
 public:
  KoncludeReasoner() : handle_(konclude_create_reasoner()) {}
  ~KoncludeReasoner() { konclude_destroy_reasoner(handle_); }
  KoncludeReasoner(const KoncludeReasoner&) = delete;
  KoncludeReasoner& operator=(const KoncludeReasoner&) = delete;

  bool ok() const { return handle_ != nullptr; }

  bool loadOntology(const std::string& path) {
    return konclude_load_ontology_file(handle_, path.c_str());
  }

  bool tellAndRetract(const std::vector<KoncludeClassAssertion>& tell,
                       const std::vector<KoncludeClassAssertion>& retract) {
    return konclude_tell_and_retract_axioms(
        handle_, tell.data(), static_cast<int>(tell.size()),
        retract.data(), static_cast<int>(retract.size()));
  }

  bool query(const std::string& sparql) {
    return konclude_execute_conjunctive_query(handle_, sparql.c_str());
  }

  const char* lastError() const { return konclude_last_error(handle_); }

 private:
  KoncludeReasonerHandle handle_;
};
```

Add `konclude_reasoner.h` (and a `.cc` if it grows non-inline code) to the
relevant `set(..._SOURCES ...)` list in `CMakeLists.txt` only if you split it
into a `.cc`; a header-only wrapper like above needs no `CMakeLists.txt`
source-list entry, just `#include "konclude_reasoner.h"` from whatever
Powerlifted file uses it (e.g. wherever precondition/effect checks against
the ontology will live).

## 5. Build Powerlifted

```sh
cd /home/zinzin2312/repos/powerlifted
./build.py            # release build -> builds/release/search/search
# or: ./build.py -d   # debug build   -> builds/debug/search/search
```

## 6. Verify the link actually resolved

```sh
ldd builds/release/search/search | grep -i konclude
# expect: libKonclude.so.1 => /home/zinzin2312/repos/Konclude/ReleaseEmbedded/libKonclude.so.1 (0x...)
```

If that line is missing or says "not found", see Troubleshooting below.
A quick functional check — call `konclude_create_reasoner()`/
`konclude_destroy_reasoner()` from wherever you first wire in
`KoncludeReasoner` (e.g. `main.cc`) and confirm the binary doesn't crash/hang
at startup — mirrors what `Tools/EmbeddedDriver/embedded_cq_driver.cpp` does
standalone in the Konclude repo.

## Troubleshooting

- **`libKonclude.so.1 => not found` in `ldd`**: the `BUILD_RPATH`/
  `INSTALL_RPATH` properties in step 3 weren't applied, or CMake's cache
  still has a stale `KONCLUDE_REPO_ROOT` from before a rename/move — delete
  `builds/release` (or `builds/debug`) and re-run `./build.py` to force a
  clean CMake configure.
- **`undefined reference to konclude_*` at link time**: `target_link_libraries`
  in step 3 wasn't added to the actual `search` target (Powerlifted also
  defines a `datalog_test` target — link `Konclude` into that too if it
  needs the reasoner).
- **`konclude_embedded.h: No such file or directory`**: the
  `INTERFACE_INCLUDE_DIRECTORIES` path doesn't match where the header
  actually lives, or `KONCLUDE_REPO_ROOT` is wrong for this checkout —
  confirm with `ls "$KONCLUDE_REPO_ROOT/Source/Control/Interface/Embedded/konclude_embedded.h"`.
- **Works here, fails on another machine**: that machine is missing the Qt5
  runtime `.so`'s (see step 1's runtime-dependency note) or has Konclude
  checked out somewhere other than `/home/zinzin2312/repos/Konclude` —
  reconfigure with `-DKONCLUDE_REPO_ROOT=<path>`.

## Current API surface caveat

`konclude_tell_and_retract_axioms` and `konclude_execute_conjunctive_query`
are currently stubs (validate arguments, return success/empty results, do
not run real reasoning yet — see their doc comments in
`Source/Control/Interface/Embedded/konclude_embedded.h`). Everything in
steps 1-6 above works end-to-end today; the reasoning results themselves
will start being real once those two are wired to
`CEmbeddedOntologyLoader`/`CEmbeddedQueryManager` (tracked in
`docs/FASTDOWNWARD_EMBEDDING.md`).
