#!/usr/bin/env bash
# Shared launcher behind tools/ghidra/{setup,import,reexport,apply-names,run-script} (ADR 0007).
#
# Usage: tools/ghidra/launch.sh setup
#        tools/ghidra/launch.sh <driver command> [ARGS...]   (commands: tools/ghidra/driver.py)
#
# On first use it fetches and unpacks the pinned Ghidra + JDK (tools/mtport/ghidra_setup.py),
# then syncs the PyGhidra environment (uv group "ghidra", in build/ghidra/python-env), points
# Ghidra's settings, caches and temp files into build/ghidra/home and runs the driver.
#
# Environment: PYTHON=<interpreter> for the setup step (default python3, needs 3.10+);
# MAPLETHIEF_DOWNLOAD_DIR (default build/_downloads); UV_PYTHON picks the PyGhidra Python.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${PYTHON:-python3}"
if [ "$#" -eq 0 ]; then
    echo "usage: tools/ghidra/launch.sh setup | <driver command> [ARGS...]" >&2
    exit 2
fi
cmd="$1"
shift

if ! command -v uv >/dev/null 2>&1; then
    echo "error: uv not found; install it from https://docs.astral.sh/uv/" >&2
    exit 2
fi

setup_py() {
    PYTHONPATH="$REPO/tools" "$PY" -B -m mtport.ghidra_setup --repo "$REPO" "$@"
}

if [ "$cmd" = setup ] || ! setup_py check; then
    setup_py setup
fi

# XDG_CACHE_HOME (below) would also move uv's cache; keep uv's own.
UV_CACHE_DIR="${UV_CACHE_DIR:-$(uv cache dir)}"
export UV_CACHE_DIR

# KEY=VALUE lines from the setup module: plain paths, never shell syntax. Python on Windows
# ends them with CRLF, so drop the CR.
while IFS='=' read -r key value; do
    export "$key=${value%$'\r'}"
done < <(setup_py env)

uv sync --quiet --locked --group ghidra --project "$REPO"
if [ "$cmd" = setup ]; then
    exit 0
fi

# No tools/ on PYTHONPATH: tools/ghidra/ would shadow Ghidra's Java "ghidra" package
# (scripts/mtport_loader.py loads mtport instead).
unset PYTHONPATH
export PYTHONPYCACHEPREFIX="$REPO/build/pycache"
exec uv run --quiet --locked --no-sync --group ghidra --project "$REPO" \
    python "$REPO/tools/ghidra/driver.py" "$cmd" "$@"
