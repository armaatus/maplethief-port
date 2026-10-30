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
#       (Metal), spirv-val on every SPIR-V module, and reflection for the binding checks.
#
# Pins: the base image by digest and the apt snapshot, as in tools/containers/linux/Dockerfile,
# and the DXC release by its SHA-256. Bump one pin per commit, and say in the commit why.

ARG UBUNTU_IMAGE=ubuntu:26.04@sha256:19fcd1bf268f02b60aca3bd30bd8a6d9a11759a430e32921833e65f16d87348a

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

ENV PATH=/opt/dxc/bin:$PATH \
    LD_LIBRARY_PATH=/opt/dxc/lib

RUN useradd --create-home --uid 1001 runner
USER runner
WORKDIR /src
CMD ["bash", "-c", "cat /etc/maplethief-shader-tools; dxc --version"]
