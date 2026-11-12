# SPDX-License-Identifier: Apache-2.0
#
# Development environment for Passerelle.
#
# Reproduces everything the CI needs, plus the tools the benches use:
#   - the C11 toolchain (gcc and clang, clang-tidy, cppcheck),
#   - CMake and CTest,
#   - QEMU and virtiofsd for the guest scenarios (tests/qemu),
#   - Python 3 with pytest for the host tools (host/),
#   - cpio, zstd and curl to build the guest initramfs.
#
# It does NOT contain the QNX SDP: it is licensed and needs an account, so it
# is mounted from the host when present (see docs/development.md).
#
# Build and use:
#   docker build -t passerelle-dev .
#   docker run --rm -it -v "$PWD:/src" passerelle-dev
#
# Or with devcontainers:
#   code .            # picks up .devcontainer/devcontainer.json
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Toolchain, build system, QEMU, and the guest-building utilities.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        cppcheck \
        cpio \
        curl \
        git \
        gcovr \
        gzip \
        lcov \
        ninja-build \
        python3 \
        python3-pip \
        python3-pytest \
        qemu-system-x86 \
        qemu-utils \
        shellcheck \
        zstd \
    && rm -rf /var/lib/apt/lists/*

# The host tools are plain Python; install them so `qshare`, `qrun`, `fsproof`
# and `pbench` are on the PATH inside the container.
COPY host/ /opt/passerelle-host/
RUN python3 -m pip install --break-system-packages --no-cache-dir /opt/passerelle-host \
    && rm -rf /opt/passerelle-host

# Where the QNX SDP is expected to be mounted (read-only): the image does not
# ship it, it is installed from the licensed download.
VOLUME ["/opt/qnx800"]

WORKDIR /src
CMD ["bash"]
