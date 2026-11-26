# syntax=docker/dockerfile:1
#
# The Android build (ADR 0150, ADR 0163, issue #356): the pinned SDK, NDK, JDK and Gradle of
# pins.env beside this file, installed by install-sdk.sh (the same script the Mac's
# scripts/android-sdk runs), plus the CMake our presets are tested with. scripts/build-android
# builds and runs it (it tags the image with a hash of this folder's files, like
# scripts/build-switch); it is not meant to be used by hand.
#
# linux/amd64 only: Google ships the NDK and the build tools for x86_64 Linux and not for arm64
# Linux (ADR 0150), so on Apple silicon it runs under emulation. The Mac's native SDK
# (build/_android-sdk/) is the fast path there.
#
# Pins (bump one per commit, and say in the commit why):
#   - the base image by its multi-arch index digest, and its apt packages through an Ubuntu
#     archive snapshot: the same as tools/containers/linux/Dockerfile;
#   - CMake from Kitware's release archive, hash-checked: the same 4.4.4;
#   - the Android packages, the JDK and Gradle: pins.env, each archive hash-checked.

ARG UBUNTU_IMAGE=ubuntu:26.04@sha256:19fcd1bf268f02b60aca3bd30bd8a6d9a11759a430e32921833e65f16d87348a

FROM ${UBUNTU_IMAGE}

ARG UBUNTU_SNAPSHOT=20261024T000000Z
ARG CMAKE_VERSION=4.4.4
ARG CMAKE_SHA256=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# apt from the snapshot only (tools/containers/linux/Dockerfile says why the first update skips
# TLS peer verification: the signed InRelease files are what make the packages trustworthy).
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
    && apt-get install -y --no-install-recommends ninja-build python3 git curl unzip zip \
    && rm -rf /var/lib/apt/lists/* \
    && echo "apt source: ${UBUNTU_SNAPSHOT:-live archive}" > /etc/maplethief-ci-image \
    && dpkg-query -W -f '${Package} ${Version}\n' >> /etc/maplethief-ci-image

# The SDK packages the build needs (no emulator, no images, no platform-tools: the container
# builds and runs nothing on a device).
COPY pins.env install-sdk.sh /opt/android-sdk-pins/
RUN /opt/android-sdk-pins/install-sdk.sh /opt/android-sdk linux \
        ndk platform build-tools jdk gradle

# GNU tar fails to create directories under Rosetta's x86_64 emulation ("Cannot mkdir: Function
# not implemented"), so archives (CMake's, the source export of scripts/build-android) are
# unpacked with Python's tarfile, as in tools/containers/linux/Dockerfile and install-sdk.sh.
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

# CMake.
RUN set -eu; \
    curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"; \
    echo "${CMAKE_SHA256}  /tmp/cmake.tar.gz" | sha256sum -c -; \
    untar /tmp/cmake.tar.gz /opt; \
    mv "/opt/cmake-${CMAKE_VERSION}-linux-x86_64" /opt/cmake; \
    ln -s /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/; \
    rm /tmp/cmake.tar.gz

# Like CI: warnings are errors (CMakeLists.txt), and the build runs as a user without root.
# cmake/AndroidToolchain.cmake finds the SDK through MAPLETHIEF_ANDROID_SDK.
ENV CI=true \
    MAPLETHIEF_ANDROID_SDK=/opt/android-sdk
RUN useradd --create-home --uid 1001 runner && mkdir -p /src /out && chown runner:runner /src /out
USER runner
WORKDIR /src

# Print the toolchain, then run the given command.
CMD ["bash", "-c", "cmake --version | head -n 1; echo \"ninja $(ninja --version)\"; python3 --version; cat /opt/android-sdk/ndk/*/source.properties | grep Revision"]
