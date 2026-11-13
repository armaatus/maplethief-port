# syntax=docker/dockerfile:1
#
# The Nintendo Switch build (ADR 0138, issue #235): devkitA64 with libnx, the toolchain of the
# homebrew NRO, plus the CMake our presets are tested with. scripts/build-switch builds and runs
# it (it tags the image with a hash of this file, like scripts/test-linux); it is not meant to be
# used by hand.
#
# Pins (bump one per commit, and say in the commit why):
#   - devkitPro's image by its multi-arch index digest (ADR 0138): GCC 15.2.0, libnx 4.12.0,
#     newlib 4.6.0.20260123, switch-cmake 1.5.2, switch-tools 1.13.1 (elf2nro, nacptool,
#     nxlink) on Debian 12. linux/amd64 for CI and Windows, linux/arm64 natively on Apple
#     silicon; both hold the same package versions. Ninja 1.11.1 and Python 3.11.2 (for the
#     build's generators: the icon, doctest's listing, the byte-compile targets) come with it,
#     so nothing is installed from Debian's archive and a rebuild gets the same layers;
#   - CMake from Kitware's release archive, hash-checked: the same 4.4.4 as the Linux CI
#     container (tools/containers/linux/Dockerfile). The image's own CMake 3.31.6 stays in
#     /usr/bin and is not used.

ARG DEVKITA64_IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

FROM ${DEVKITA64_IMAGE}

ARG TARGETARCH
ARG CMAKE_VERSION=4.4.4
ARG CMAKE_SHA256_AMD64=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
ARG CMAKE_SHA256_ARM64=a1b6cc63636a0e55c63257cf3315a8a5f129e42fade25db1afea4ff8ab06f25e

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN set -eu; \
    case "$TARGETARCH" in \
        amd64) cmake_arch=x86_64; cmake_sha=$CMAKE_SHA256_AMD64 ;; \
        arm64) cmake_arch=aarch64; cmake_sha=$CMAKE_SHA256_ARM64 ;; \
        *) echo "unsupported architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/cmake.tar.gz \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz"; \
    echo "${cmake_sha}  /tmp/cmake.tar.gz" | sha256sum -c -; \
    mkdir -p /opt/cmake; \
    tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1 --no-same-owner; \
    rm /tmp/cmake.tar.gz; \
    for tool in cmake ctest cpack; do ln -s "/opt/cmake/bin/$tool" "/usr/local/bin/$tool"; done; \
    for tool in ninja python3 elf2nro nacptool nxlink; do command -v "$tool" >/dev/null; done

# Like CI: warnings are errors (CMakeLists.txt), and the build runs as a user without root.
ENV CI=true
RUN useradd --create-home --uid 1001 runner && mkdir -p /src /out && chown runner:runner /src /out
USER runner
WORKDIR /src

# Print the toolchain, then run the given command.
CMD ["bash", "-c", "cmake --version | head -n 1; echo \"ninja $(ninja --version)\"; \"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc\" --version | head -n 1; python3 --version"]
