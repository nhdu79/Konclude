#!/usr/bin/env bash
# Builds the Konclude worker prototype (see README.md here). Run from the repo
# root after Tools/EmbeddedDriver/rebuild.sh, which builds
# ReleaseEmbedded/libKonclude.so.
#
#   konclude_worker                    serves one handle over a socket (links libKonclude)
#   konclude_worker_equivalence_test   in-process vs. worker answers (links both)
#   konclude_worker_growth_benchmark   memory/time through the worker (client only)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

src=Tools/KoncludeWorker
inc=(-I Source/Control/Interface/Embedded -I "$src")
lib=(-L ReleaseEmbedded -lKonclude -Wl,-rpath,"$(pwd)/ReleaseEmbedded")

g++ -std=c++11 -O2 -g "${inc[@]}" "$src/konclude_worker.cpp" "${lib[@]}" -o konclude_worker
g++ -std=c++11 -O2 -g "${inc[@]}" "$src/konclude_worker_equivalence_test.cpp" \
    "$src/konclude_worker_client.cpp" "${lib[@]}" -o konclude_worker_equivalence_test
# the client side needs neither libKonclude nor Qt
g++ -std=c++11 -O2 -g "${inc[@]}" "$src/konclude_worker_growth_benchmark.cpp" \
    "$src/konclude_worker_client.cpp" -o konclude_worker_growth_benchmark

echo "[build.sh] OK: konclude_worker konclude_worker_equivalence_test konclude_worker_growth_benchmark"
