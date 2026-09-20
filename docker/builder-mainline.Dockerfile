# BPI-W2 PiKVM - cross-compile environment for the mainline/LTS kernel line
#
# Separate from docker/builder.Dockerfile on purpose. That one is pinned to
# bullseye because kernel 4.9 needs OpenSSL 1.1 and make 4.3, and it builds
# with the BSP's bundled gcc-linaro 7.3.1. None of that applies here:
#
#   * a 6.x kernel wants a newer GCC than the BSP ships, so the toolchain
#     comes from the distro instead of from vendor/bpi-w2-bsp/toolchains/
#   * OpenSSL 3 and make 4.4 are fine for 6.x
#   * python3 (not python2) is what the build scripts want
#
# u-boot still comes from the BSP and is still built by the bullseye image.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        bc \
        bison \
        build-essential \
        ca-certificates \
        cpio \
        device-tree-compiler \
        file \
        flex \
        gawk \
        gcc-aarch64-linux-gnu \
        git \
        gzip \
        kmod \
        libelf-dev \
        libncurses-dev \
        libssl-dev \
        lz4 \
        lzop \
        make \
        pahole \
        python3 \
        rsync \
        u-boot-tools \
        xz-utils \
        zstd \
    && rm -rf /var/lib/apt/lists/*

# Shared with the bullseye builder: run as the caller's uid/gid so build
# outputs are not left owned by root.
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
