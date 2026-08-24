#!/usr/bin/env bash
# Watches the embedded interface + driver source for changes and reruns
# rebuild.sh automatically. Meant to sit in its own terminal/tmux pane while
# you edit in nvim -- no editor-side config needed.
#
# Uses `entr` if it's installed (cleaner, event-based); otherwise falls back
# to a portable mtime-polling loop that needs nothing extra installed.
#   Install entr for the better experience:  sudo apt install entr
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

WATCH_DIRS=(
    Source/Control/Interface/Embedded
    Tools/EmbeddedDriver
)
WATCH_FILES=()
while IFS= read -r -d '' f; do
    WATCH_FILES+=("$f")
done < <(find "${WATCH_DIRS[@]}" \( -name '*.cpp' -o -name '*.h' \) -print0)

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [watch.sh] $*"
}

if command -v entr >/dev/null 2>&1; then
    log "using entr, watching ${#WATCH_FILES[@]} files"
    printf '%s\n' "${WATCH_FILES[@]}" | entr -c bash -c '
        echo "[$(date "+%Y-%m-%d %H:%M:%S")] [watch.sh] change detected, rebuilding..."
        ./Tools/EmbeddedDriver/rebuild.sh
        echo "[$(date "+%Y-%m-%d %H:%M:%S")] [watch.sh] done."
    '
else
    log "entr not found, falling back to polling every 1s"
    log "(sudo apt install entr for a lighter-weight watcher)"
    declare -A last_mtime
    while true; do
        changed=0
        for f in "${WATCH_FILES[@]}"; do
            mtime=$(stat -c %Y "$f" 2>/dev/null || echo 0)
            if [[ "${last_mtime[$f]:-}" != "" && "${last_mtime[$f]}" != "$mtime" ]]; then
                changed=1
            fi
            last_mtime[$f]=$mtime
        done
        if [[ $changed -eq 1 ]]; then
            clear
            log "change detected, rebuilding..."
            ./Tools/EmbeddedDriver/rebuild.sh || true
            log "done."
        fi
        sleep 1
    done
fi
