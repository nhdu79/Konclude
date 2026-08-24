#!/usr/bin/env bash
# Incrementally rebuilds libKonclude.so (KoncludeEmbedded.pro, debug symbols)
# and relinks every driver/probe in this directory against it. Safe to run
# repeatedly -- `make` only recompiles what changed, and each g++ invocation
# below is cheap. Run from the repo root.
#
# Picks up new files automatically: every Tools/EmbeddedDriver/*.cpp is built
# into a same-named binary at the repo root. No per-file registration needed.
#
# One-time prerequisite (only needed once, or again if KoncludeEmbedded.pro
# itself changes, or a source file is added/removed):
#   qmake -o MakefileEmbedded "QMAKE_CXXFLAGS+=-g -O0" KoncludeEmbedded.pro
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

make -f MakefileEmbedded -j"$(nproc)"

built=()
for src in Tools/EmbeddedDriver/*.cpp; do
    name="$(basename "$src" .cpp)"
    g++ -std=c++11 -g -O0 -I Source/Control/Interface/Embedded \
        "$src" \
        -L ReleaseEmbedded -lKonclude -Wl,-rpath,"$(pwd)/ReleaseEmbedded" \
        -o "$name"
    built+=("$name")
done

echo "[rebuild.sh] OK: libKonclude.so.1.0.0 + ${built[*]} up to date."
