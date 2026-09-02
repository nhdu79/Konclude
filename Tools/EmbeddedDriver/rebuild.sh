#!/usr/bin/env bash
# Incrementally rebuilds libKonclude.so (KoncludeEmbedded.pro, debug symbols)
# and relinks every driver/probe in this directory against it. Safe to run
# repeatedly -- `make` only recompiles what changed, and each g++ invocation
# below is cheap. Run from the repo root.
#
# Picks up new files automatically: every Tools/EmbeddedDriver/*.cpp is built
# into a same-named binary at the repo root. No per-file registration needed.
#
# Regenerates MakefileEmbedded itself (via qmake) the first time, and again
# whenever KoncludeEmbedded.pro/Konclude.pri change -- no manual qmake step
# needed. Requires Qt 5 specifically: Qt 6 removed QLinkedList, which this
# codebase still uses throughout (see docs/FASTDOWNWARD_EMBEDDING.md). Picks
# a Qt5 qmake automatically -- set QMAKE to force a specific binary.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

find_qt5_qmake() {
    if [[ -n "${QMAKE:-}" ]]; then
        echo "$QMAKE"
        return 0
    fi
    local candidates=(
        qmake5
        qmake-qt5
        /opt/homebrew/opt/qt@5/bin/qmake
        /usr/local/opt/qt@5/bin/qmake
        /usr/lib/qt5/bin/qmake
        /usr/lib/x86_64-linux-gnu/qt5/bin/qmake
        qmake
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        command -v "$candidate" >/dev/null 2>&1 || continue
        if "$candidate" -v 2>/dev/null | grep -q "Using Qt version 5\."; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

nproc_portable() {
    nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4
}

QMAKE_BIN="$(find_qt5_qmake)" || {
    echo "[rebuild.sh] No Qt5 qmake found (Qt6 removed QLinkedList, which" >&2
    echo "[rebuild.sh] this codebase needs). Install Qt5 and/or set QMAKE=" >&2
    echo "[rebuild.sh] to point at its qmake, e.g.:" >&2
    echo "[rebuild.sh]   macOS:  brew install qt@5   (then QMAKE=/opt/homebrew/opt/qt@5/bin/qmake)" >&2
    echo "[rebuild.sh]   Debian/Ubuntu: apt install qtbase5-dev qt5-qmake" >&2
    exit 1
}

if [[ ! -f MakefileEmbedded || KoncludeEmbedded.pro -nt MakefileEmbedded || Konclude.pri -nt MakefileEmbedded ]]; then
    echo "[rebuild.sh] (re)generating MakefileEmbedded via $QMAKE_BIN"
    "$QMAKE_BIN" -o MakefileEmbedded "QMAKE_CXXFLAGS+=-g -O0" KoncludeEmbedded.pro
fi

make -f MakefileEmbedded -j"$(nproc_portable)"

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
