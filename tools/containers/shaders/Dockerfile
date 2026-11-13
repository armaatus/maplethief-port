# syntax=docker/dockerfile:1
#
# The shader compilers of scripts/build-shaders (#120, ADR 0060). mt_gfx's GPU backend writes its
# shaders in HLSL and commits them compiled for SDL3 GPU's three shader formats; this image holds
# the pinned tools that compile them. scripts/build-shaders builds and runs it; it is not meant to
# be used by hand. It is linux/amd64 only, because DXC's Linux release is x86_64 (on a Mac it runs
# under Rosetta in OrbStack).
#
#   DXC: Microsoft's DirectXShaderCompiler release. It includes libdxil.so, so the DXIL it writes
#       is signed. HLSL -> DXIL (D3D12) and HLSL -> SPIR-V (Vulkan).
#   SPIRV-Cross and SPIRV-Tools: Ubuntu's packages, from the archive snapshot. SPIR-V -> MSL source
#       (Metal), SPIR-V -> GLSL (for UAM), spirv-val on every SPIR-V module, and reflection for
#       the binding checks.
#   UAM: devkitPro's deko3d shader compiler (#242, ADR 0143), GLSL -> DKSH for SDL_GPU on deko3d
#       (the Nintendo Switch). The binary of the devkitA64 image that ADR 0138 pins, copied out
#       of it: devkitPro's package server refuses scripted downloads, and it is the same UAM the
#       Switch build's image carries. It needs only libc and libstdc++.
#
# Pins: the base image by digest and the apt snapshot, as in tools/containers/linux/Dockerfile,
# the DXC release by its SHA-256, and the devkitA64 image by ADR 0138's index digest (its
# linux/amd64 image here). Bump one pin per commit, and say in the commit why.

ARG UBUNTU_IMAGE=ubuntu:26.04@sha256:19fcd1bf268f02b60aca3bd30bd8a6d9a11759a430e32921833e65f16d87348a
ARG DEVKITA64_IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

FROM ${DEVKITA64_IMAGE} AS devkita64

FROM ${UBUNTU_IMAGE}

ARG UBUNTU_SNAPSHOT=20261024T000000Z
ARG DXC_TAG=v1.9.2609
ARG DXC_ARCHIVE=linux_dxc_2026_09_28.x86_x64.tar.gz
ARG DXC_SHA256=96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

# apt from the snapshot only. tools/containers/linux/Dockerfile explains the TLS bootstrap.
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
    && apt-get install -y --no-install-recommends curl python3 spirv-cross spirv-tools \
    && rm -rf /var/lib/apt/lists/*

# GNU tar can't create directories under Rosetta (tools/containers/linux/Dockerfile), so
# Python's tarfile unpacks the archive.
RUN set -eu; \
    curl -fsSL -o /tmp/dxc.tar.gz \
        "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_ARCHIVE}"; \
    echo "${DXC_SHA256}  /tmp/dxc.tar.gz" | sha256sum -c -; \
    mkdir -p /opt/dxc; \
    python3 -c 'import sys, tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="tar")' \
        /tmp/dxc.tar.gz /opt/dxc; \
    chmod +x /opt/dxc/bin/dxc; \
    rm /tmp/dxc.tar.gz; \
    printf '%s\n' "dxc ${DXC_TAG} (${DXC_ARCHIVE})" > /etc/maplethief-shader-tools; \
    dpkg-query -W -f '${Package} ${Version}\n' spirv-cross spirv-tools >> /etc/maplethief-shader-tools

# UAM 1.1.0 (ISC), checked by its SHA-256 and its version line.
ARG UAM_SHA256=5c3cdb84f000fd683c7440b7b2fbddc2cbb0d7d90d3bf8a2ae3908fe94a1c413
COPY --from=devkita64 /opt/devkitpro/tools/bin/uam /opt/uam/bin/uam
RUN set -eu; \
    echo "${UAM_SHA256}  /opt/uam/bin/uam" | sha256sum -c -; \
    /opt/uam/bin/uam --version | grep -q '^uam 1\.1\.0 '; \
    printf '%s\n' "$(/opt/uam/bin/uam --version | cut -d' ' -f1-2) (devkitpro/devkita64, ADR 0138)" \
        >> /etc/maplethief-shader-tools

ENV PATH=/opt/dxc/bin:/opt/uam/bin:$PATH \
    LD_LIBRARY_PATH=/opt/dxc/lib

RUN useradd --create-home --uid 1001 runner
USER runner
WORKDIR /src
CMD ["bash", "-c", "cat /etc/maplethief-shader-tools; dxc --version"]
