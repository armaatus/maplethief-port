# syntax=docker/dockerfile:1
#
# The portable Linux game build (#84, ADR 0065): scripts/linux-game builds the game folder in
# this image, not in the CI image of tools/containers/linux/, because an executable needs the
# glibc it was linked against or newer. The CI image's Ubuntu 26.04 has glibc 2.43, whose libm
# gave asinf, acosf, atan2f, log10f and sqrtf new symbol versions, so its build runs only on
# glibc 2.43 and later. Ubuntu 24.04 LTS has glibc 2.39: what this image links against is the
# newest glibc the game folder needs (platform.linux_portable checks it with
# MAPLETHIEF_LINUX_GLIBC_FLOOR=2.39). It is not meant to be used by hand.
#
# Pins (bump one per commit, and say in the commit why), as in tools/containers/linux/:
#   - the base image by its multi-arch index digest (linux/amd64 is the default);
#   - every apt package through an Ubuntu archive snapshot: GCC 14 (g++-14 from universe; the
#     default GCC 13 is ADR 0002's minimum, 14 is closer to CI's 15), Ninja, Python 3.12 and
#     the SDL3 build dependencies of the README's list;
#   - CMake from Kitware's release archive, the same version and hashes as the CI image.
# No clang, uv, PowerShell or Node.js: this image builds MapleThief and runs
# mtport.linux_game (standard library only), it does not run the test suites.

ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852

FROM ${UBUNTU_IMAGE}

ARG TARGETARCH
ARG UBUNTU_SNAPSHOT=20261024T000000Z
ARG CMAKE_VERSION=4.4.4
ARG CMAKE_SHA256_AMD64=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
ARG CMAKE_SHA256_ARM64=a1b6cc63636a0e55c63257cf3315a8a5f129e42fade25db1afea4ff8ab06f25e

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# apt from the snapshot only (see tools/containers/linux/Dockerfile for the TLS bootstrap);
# --build-arg UBUNTU_SNAPSHOT= (empty) uses the live archive instead.
RUN if [ -n "$UBUNTU_SNAPSHOT" ]; then \
        rm -f /etc/apt/sources.list.d/ubuntu.sources; \
        printf '%s\n' 'Types: deb' \
            "URIs: https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/" \
            'Suites: noble noble-updates noble-security' \
            'Components: main universe' \
            'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
            > /etc/apt/sources.list.d/ubuntu-snapshot.sources; \
    fi \
    && apt-get -o Acquire::https::Verify-Peer=false update \
    && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends \
        ca-certificates \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 ninja-build pkg-config python3 curl \
        libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev \
        libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev \
        libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdbus-1-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/* \
    && echo "apt source: ${UBUNTU_SNAPSHOT:-live archive}" > /etc/maplethief-game-image \
    && dpkg-query -W -f '${Package} ${Version}\n' >> /etc/maplethief-game-image

# GNU tar 1.35 fails to create directories under Rosetta's x86_64 emulation, so archives are
# unpacked with Python's tarfile (as in the CI image).
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
        amd64) cmake_arch=x86_64; cmake_sha=$CMAKE_SHA256_AMD64 ;; \
        arm64) cmake_arch=aarch64; cmake_sha=$CMAKE_SHA256_ARM64 ;; \
        *) echo "unsupported architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz"; \
    echo "${cmake_sha}  /tmp/cmake.tar.gz" | sha256sum -c -; \
    untar /tmp/cmake.tar.gz /opt; \
    mv "/opt/cmake-${CMAKE_VERSION}-linux-${cmake_arch}" /opt/cmake; \
    ln -s /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/; \
    rm /tmp/cmake.tar.gz

ENV CC=gcc-14 \
    CXX=g++-14
RUN useradd --create-home --uid 1001 runner && mkdir -p /src /out && chown runner:runner /src /out
USER runner
WORKDIR /src

CMD ["bash", "-c", "cmake --version | head -n 1; echo \"ninja $(ninja --version)\"; c++ --version | head -n 1; g++-14 --version | head -n 1; python3 --version; ldd --version | head -n 1"]
