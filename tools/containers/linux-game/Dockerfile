# syntax=docker/dockerfile:1
#
# The portable Linux build (#84, ADR 0065; #349, ADR 0193): scripts/linux-game builds the game
# folder and the player downloads in this image, not in the CI image of tools/containers/linux/,
# because an executable needs the glibc it was linked against or newer. The CI image's Ubuntu
# 26.04 has glibc 2.43, whose libm gave asinf, acosf, atan2f, log10f and sqrtf new symbol
# versions, so its build runs only on glibc 2.43 and later. Ubuntu 22.04 LTS has glibc 2.35:
# what this image links against is the newest glibc the downloads need (platform.linux_portable
# checks it with MAPLETHIEF_LINUX_GLIBC_FLOOR=2.35), so they run on Ubuntu 22.04, Debian 12 and
# every current SteamOS. It is not meant to be used by hand.
#
# Pins (bump one per commit, and say in the commit why), as in tools/containers/linux/:
#   - the base image by its multi-arch index digest (linux/amd64 is the default);
#   - every apt package through an Ubuntu archive snapshot: Ninja, Python 3.10, the SDL3 build
#     dependencies of the README's list, and the packers of the AppImage (mksquashfs) and the
#     Flatpak bundle (flatpak build-export, build-bundle), plus Xvfb for scripts/linux-game's
#     checks;
#   - GCC 14 from the Ubuntu Toolchain PPA through Launchpad's PPA snapshot service, at the
#     same timestamp, signed by the PPA's key committed next to this file
#     (ubuntu-toolchain-r.asc, C8EC952E2A0E1FBDC5090F6A2C277A0A352154E5). Ubuntu 22.04's own
#     GCC 12 is older than ADR 0002's minimum;
#   - CMake from Kitware's release archive, the same version and hashes as the CI image.
# No clang, uv, PowerShell or Node.js: this image builds MapleThief and runs
# mtport.linux_game (standard library only), it does not run the test suites.

ARG UBUNTU_IMAGE=ubuntu:22.04@sha256:b000e69a50ff940390233025e7a337fb8469b509e2334fa4d018ac30f3e18cdb

FROM ${UBUNTU_IMAGE}

ARG TARGETARCH
ARG UBUNTU_SNAPSHOT=20261125T000000Z
ARG CMAKE_VERSION=4.4.4
ARG CMAKE_SHA256_AMD64=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
ARG CMAKE_SHA256_ARM64=a1b6cc63636a0e55c63257cf3315a8a5f129e42fade25db1afea4ff8ab06f25e
# The AppImage's type-2 runtime (ADR 0193): AppImage/type2-runtime's release 20251108, static,
# with FUSE 3 and squashfs zlib/zstd/xz built in. mtport.linux_game appimage appends the
# squashfs to it; mtport.release accepts no other runtime in an AppImage (APPIMAGE_RUNTIMES).
ARG APPIMAGE_RUNTIME_RELEASE=20251108
ARG APPIMAGE_RUNTIME_SHA256_AMD64=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
ARG APPIMAGE_RUNTIME_SHA256_ARM64=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

COPY ubuntu-toolchain-r.asc /usr/share/keyrings/ubuntu-toolchain-r.asc

# apt from the snapshot only (see tools/containers/linux/Dockerfile for the TLS bootstrap);
# --build-arg UBUNTU_SNAPSHOT= (empty) uses the live archives instead. Ubuntu 22.04 still has
# the one-line sources.list.
RUN if [ -n "$UBUNTU_SNAPSHOT" ]; then \
        archive="https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/"; \
        ppa="https://snapshot.ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/${UBUNTU_SNAPSHOT}/"; \
    else \
        archive="http://archive.ubuntu.com/ubuntu/"; \
        ppa="https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/"; \
    fi \
    && printf '%s\n' \
        "deb $archive jammy main universe" \
        "deb $archive jammy-updates main universe" \
        "deb $archive jammy-security main universe" > /etc/apt/sources.list \
    && apt-get -o Acquire::https::Verify-Peer=false update \
    && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends \
        ca-certificates \
    && echo "deb [signed-by=/usr/share/keyrings/ubuntu-toolchain-r.asc] $ppa jammy main" \
        > /etc/apt/sources.list.d/ubuntu-toolchain-r.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 ninja-build pkg-config python3 curl \
        squashfs-tools flatpak xvfb xauth \
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
    rm /tmp/cmake.tar.gz; \
    case "$TARGETARCH" in \
        amd64) rt_arch=x86_64; rt_sha=$APPIMAGE_RUNTIME_SHA256_AMD64 ;; \
        arm64) rt_arch=aarch64; rt_sha=$APPIMAGE_RUNTIME_SHA256_ARM64 ;; \
    esac; \
    mkdir -p /opt/appimage; \
    curl -fsSL -o /opt/appimage/runtime \
        "https://github.com/AppImage/type2-runtime/releases/download/${APPIMAGE_RUNTIME_RELEASE}/runtime-${rt_arch}"; \
    echo "${rt_sha}  /opt/appimage/runtime" | sha256sum -c -

ENV CC=gcc-14 \
    CXX=g++-14
RUN useradd --create-home --uid 1001 runner && mkdir -p /src /out && chown runner:runner /src /out
USER runner
WORKDIR /src

CMD ["bash", "-c", "cmake --version | head -n 1; echo \"ninja $(ninja --version)\"; c++ --version | head -n 1; g++-14 --version | head -n 1; python3 --version; ldd --version | head -n 1; mksquashfs -version | head -n 1; flatpak --version; sha256sum /opt/appimage/runtime"]
