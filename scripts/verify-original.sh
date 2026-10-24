#!/usr/bin/env bash
# Verify original/MapleThief-1.5.ipa against original/SHA256SUMS and extract it
# to work/ipa/. Safe to re-run: the extraction is replaced, nothing else is touched.
#
# Usage: scripts/verify-original.sh [--no-extract]
#
#   --no-extract  only verify the IPA, write nothing
#
# The first stages of scripts/bootstrap, which does the checks in tools/mtport/bootstrap.py
# (so needs Python 3.10+); scripts/bootstrap also splits the slices into work/slices/.
set -euo pipefail

case "$#:${1:-}" in
  0:) stop=--no-split ;;
  1:--no-extract) stop=--verify-only ;;
  *) echo "usage: scripts/verify-original.sh [--no-extract]" >&2; exit 2 ;;
esac

exec "$(dirname "${BASH_SOURCE[0]}")/bootstrap" "$stop"
