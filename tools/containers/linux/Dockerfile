# syntax=docker/dockerfile:1
#
# The Linux CI job, as a container (ADR 0023, issue #113): what GitHub's ubuntu-latest runner
# (Ubuntu 26.04 x86_64) gives the `build` and `lint-python` jobs of .github/workflows/ci.yml,
# pinned. scripts/test-linux builds and runs it; it is not meant to be used by hand.
#
# Pins (bump one per commit, and say in the commit why):
#   - the base image by its multi-arch index digest (linux/amd64 is the default, linux/arm64
#     the fast option on Apple silicon);
#   - every apt package through an Ubuntu archive snapshot (snapshot.ubuntu.com), so a rebuild
#     gets the same versions: GCC 15.2, clang, Ninja 1.13.2, Python 3.14, Node.js and the
#     SDL3 build dependencies of the README's list;
#   - CMake from Kitware's release archive (ubuntu-latest has a Kitware CMake, not Ubuntu's),
#     PowerShell from its release archive (ubuntu-latest has pwsh, and tests/test_bootstrap.py
#     runs the .ps1 scripts when it is found), uv from its image by digest, each hash-checked.

ARG UBUNTU_IMAGE=ubuntu:26.04@sha256:19fcd1bf268f02b60aca3bd30bd8a6d9a11759a430e32921833e65f16d87348a
ARG UV_IMAGE=ghcr.io/astral-sh/uv:0.11.7@sha256:240fb85ab0f263ef12f492d8476aa3a2e4e1e333f7d67fbdd923d00a506a516a

FROM ${UV_IMAGE} AS uv

FROM ${UBUNTU_IMAGE}

ARG TARGETARCH
ARG UBUNTU_SNAPSHOT=20261024T000000Z
ARG CMAKE_VERSION=4.4.4
ARG CMAKE_SHA256_AMD64=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
ARG CMAKE_SHA256_ARM64=a1b6cc63636a0e55c63257cf3315a8a5f129e42fade25db1afea4ff8ab06f25e
ARG PWSH_VERSION=7.6.7
ARG PWSH_SHA256_AMD64=6896f9e3a6b0a354bf470835a96da76d2f801cd50dcf047ad7c4563bf93bf431
ARG PWSH_SHA256_ARM64=ba465efdfb0984c1c5dc5ed9eb0b79a7e91a1b52c45f99377058c0659b84d3f1

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# apt from the snapshot only. The base image has no CA certificates yet, so the first update
# skips TLS peer verification; apt still checks the archive's signed InRelease files, which
# is what makes the packages trustworthy. Every later download verifies TLS.
# --build-arg UBUNTU_SNAPSHOT= (empty) uses the live archive instead, e.g. while the snapshot
# service is down; the versions then float with the archive.
RUN if [ -n "$UBUNTU_SNAPSHOT" ]; then \
        printf '%s\n' 'Types: deb' \
            "URIs: https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/" \
            'Suites: resolute resolute-updates resolute-security' \
            'Components: main universe' \
            'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
            > /etc/apt/sources.list.d/ubuntu.sources; \
    fi \
    && apt-get -o Acquire::https::Verify-Peer=false update \
    && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends \
        ca-certificates \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential clang ninja-build pkg-config python3 git curl xz-utils unzip zip file \
        nodejs less libicu-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev \
        libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev \
        libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/* \
    && echo "apt source: ${UBUNTU_SNAPSHOT:-live archive}" > /etc/maplethief-ci-image \
    && dpkg-query -W -f '${Package} ${Version}\n' >> /etc/maplethief-ci-image

# GNU tar 1.35 of Ubuntu 26.04 fails to create directories under Rosetta's x86_64 emulation
# ("Cannot mkdir: Function not implemented"), so archives are unpacked with Python's tarfile.
# (CMake's own `cmake -E tar`, used by FetchContent, works.)
COPY --chmod=755 <<'PY' /usr/local/bin/untar
#!/usr/bin/python3
"""untar ARCHIVE DIR: unpack a tar archive (any compression) into DIR; '-' reads stdin."""
import sys
import tarfile

src, dest = sys.argv[1:3]
tar = tarfile.open(fileobj=sys.stdin.buffer, mode="r|*") if src == "-" else tarfile.open(src)
with tar:
    tar.extractall(dest, filter="tar")
PY

RUN set -eu; \
    case "$TARGETARCH" in \
        amd64) cmake_arch=x86_64; cmake_sha=$CMAKE_SHA256_AMD64; pwsh_arch=x64; pwsh_sha=$PWSH_SHA256_AMD64 ;; \
        arm64) cmake_arch=aarch64; cmake_sha=$CMAKE_SHA256_ARM64; pwsh_arch=arm64; pwsh_sha=$PWSH_SHA256_ARM64 ;; \
        *) echo "unsupported architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz"; \
    echo "${cmake_sha}  /tmp/cmake.tar.gz" | sha256sum -c -; \
    untar /tmp/cmake.tar.gz /opt; \
    mv "/opt/cmake-${CMAKE_VERSION}-linux-${cmake_arch}" /opt/cmake; \
    ln -s /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/; \
    curl -fsSL -o /tmp/pwsh.tar.gz \
        "https://github.com/PowerShell/PowerShell/releases/download/v${PWSH_VERSION}/powershell-${PWSH_VERSION}-linux-${pwsh_arch}.tar.gz"; \
    echo "${pwsh_sha}  /tmp/pwsh.tar.gz" | sha256sum -c -; \
    untar /tmp/pwsh.tar.gz /opt/microsoft/powershell/7; \
    chmod +x /opt/microsoft/powershell/7/pwsh; \
    ln -s /opt/microsoft/powershell/7/pwsh /usr/local/bin/pwsh; \
    rm /tmp/cmake.tar.gz /tmp/pwsh.tar.gz

COPY --from=uv /uv /uvx /usr/local/bin/

# Like CI: warnings are errors (CMakeLists.txt), and the tests run as a user without root.
ENV CI=true \
    UV_CACHE_DIR=/cache/uv \
    POWERSHELL_TELEMETRY_OPTOUT=1
RUN useradd --create-home --uid 1001 runner && mkdir -p /cache/uv /src /out \
    && chown runner:runner /cache/uv /src /out
USER runner
WORKDIR /src

# Print the toolchain like CI's "Show toolchain versions" step, then run the given command.
CMD ["bash", "-c", "cmake --version | head -n 1; echo \"ninja $(ninja --version)\"; c++ --version | head -n 1; clang --version | head -n 1; python3 --version; uv --version; pwsh --version"]
