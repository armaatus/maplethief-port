#!/usr/bin/env bash
# Verify original/MapleThief-1.5.ipa against original/SHA256SUMS and extract it
# to work/ipa/. Safe to re-run: the extraction is replaced, nothing else is touched.
#
# Usage: scripts/verify-original.sh [--no-extract]
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IPA="original/MapleThief-1.5.ipa"
SUMS="original/SHA256SUMS"
OUT="work/ipa"
BIN="$OUT/Payload/MapleThief.app/MapleThief"
EXPECTED_SIZE=17872861

cd "$REPO"

die() { echo "error: $*" >&2; exit 1; }

sha256() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  else die "need shasum or sha256sum"; fi
}

expected() { awk -v p="$1" '$2 == p { print $1 }' "$SUMS"; }

file_size() { wc -c <"$1" | tr -d ' '; }

check() {
  local path="$1" want got
  want="$(expected "$path")"
  [[ -n "$want" ]] || die "no entry for $path in $SUMS"
  got="$(sha256 "$path")"
  [[ "$got" == "$want" ]] || die "$path: SHA-256 mismatch (got $got, want $want)"
  echo "ok  $path"
}

command -v unzip >/dev/null 2>&1 || die "need unzip"
[[ -f "$SUMS" ]] || die "$SUMS missing"
[[ -f "$IPA" ]] || die "$IPA missing"

# A clone without Git LFS leaves a ~130-byte pointer file instead of the IPA.
if head -c 64 "$IPA" | grep -q "git-lfs.github.com/spec"; then
  die "$IPA is a Git LFS pointer; run 'git lfs install && git lfs pull' first"
fi

size="$(file_size "$IPA")"
[[ "$size" == "$EXPECTED_SIZE" ]] || die "$IPA: size $size, expected $EXPECTED_SIZE"
check "$IPA"

[[ "${1:-}" == "--no-extract" ]] && exit 0

rm -rf "$OUT"
mkdir -p "$OUT"
unzip -q "$IPA" -d "$OUT"
[[ -f "$BIN" ]] || die "extraction did not produce $BIN"
check "$BIN"

echo "extracted to $OUT/"
