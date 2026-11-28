#!/usr/bin/env bash
# Install the Android toolchain's pinned packages (pins.env beside this file; ADR 0150, ADR 0163,
# issue #356) into an SDK root: the same script for the container (Dockerfile, linux) and the Mac
# (scripts/android-sdk, darwin), so the two can't drift apart.
#
# Usage: install-sdk.sh SDK_ROOT darwin|linux PACKAGE...
#   PACKAGE: ndk platform build-tools platform-tools emulator image image-ps16k jdk gradle
#
# Each archive is downloaded from its pinned URL, checked against its pinned SHA-256 (a mismatch
# stops the script and installs nothing), unpacked into a temporary folder beside its place and
# moved there. A package whose stamp (.maplethief-pin, the URL and the SHA-256) matches the pin is
# skipped, so a second run touches no network. sdkmanager is not used: it checks SHA-1 only and
# can't install an older emulator or image revision (ADR 0163). The archive is deleted after
# unpacking, unless ANDROID_DOWNLOAD_CACHE names a folder to keep it in (and to look in first).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PINS="$HERE/pins.env"

die() { echo "install-sdk: $*" >&2; exit 1; }

[ "$#" -ge 3 ] || die "usage: install-sdk.sh SDK_ROOT darwin|linux PACKAGE..."
root="$1"
host="$2"
shift 2
case "$host" in
    darwin) HOST=DARWIN ;;
    linux) HOST=LINUX ;;
    *) die "host must be darwin or linux, not $host" ;;
esac

# pins.env: plain KEY=value lines (CMake and Gradle parse it too), read without running it.
pin() { sed -n "s/^$1=//p" "$PINS"; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

mkdir -p "$root"
root="$(cd "$root" && pwd -P)"

install_one() {
    local name="$1" key
    key="$(printf '%s' "$name" | tr 'a-z-' 'A-Z_')"
    local path archive
    path="$(pin "ANDROID_${key}_PATH")"
    archive="$(pin "ANDROID_${key}_${HOST}")"
    [ -n "$path" ] || die "unknown package: $name"
    [ -n "$archive" ] || die "$name has no pin for $host"
    local url="${archive%% *}" sha="${archive##* }"
    local dest="$root/$path" stamp="$url $sha"
    if [ -f "$dest/.maplethief-pin" ] && [ "$(cat "$dest/.maplethief-pin")" = "$stamp" ]; then
        echo "install-sdk: $path is up to date"
        return
    fi

    local file="${url##*/}" tmp
    file="${file//%2B/+}"
    tmp="$(mktemp -d "$root/.install-$name.XXXXXX")"
    local zip="$tmp/$file"
    if [ -n "${ANDROID_DOWNLOAD_CACHE:-}" ] && [ -f "$ANDROID_DOWNLOAD_CACHE/$file" ]; then
        zip="$ANDROID_DOWNLOAD_CACHE/$file"
    else
        [ -z "${ANDROID_DOWNLOAD_CACHE:-}" ] || zip="$ANDROID_DOWNLOAD_CACHE/$file"
        [ -z "${ANDROID_DOWNLOAD_CACHE:-}" ] || mkdir -p "$ANDROID_DOWNLOAD_CACHE"
        echo "install-sdk: downloading $url"
        curl -fsSL --retry 3 -o "$zip.part" "$url"
        mv "$zip.part" "$zip"
    fi
    local got
    got="$(sha256_of "$zip")"
    if [ "$got" != "$sha" ]; then
        rm -rf "$tmp"
        die "$file: SHA-256 $got, but pins.env pins $sha"
    fi

    echo "install-sdk: unpacking $file into $path"
    mkdir -p "$tmp/x"
    case "$file" in
        *.zip) unzip -q "$zip" -d "$tmp/x" ;;
        # Python's tarfile: GNU tar fails to create directories under Rosetta's emulation
        # (tools/containers/linux/Dockerfile).
        *.tar.gz) python3 -c 'import sys, tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="tar")' "$zip" "$tmp/x" ;;
        *) die "$file: unknown archive type" ;;
    esac
    # Every archive holds one top-level folder (android-ndk-r28c/, arm64-v8a/, jdk-17.../, ...).
    local top=("$tmp/x"/*)
    [ "${#top[@]}" -eq 1 ] && [ -d "${top[0]}" ] || die "$file: expected one top-level folder"
    printf '%s\n' "$stamp" >"${top[0]}/.maplethief-pin"
    rm -rf "$dest"
    mkdir -p "$(dirname "$dest")"
    mv "${top[0]}" "$dest"
    rm -rf "$tmp"
}

for package in "$@"; do
    install_one "$package"
done
