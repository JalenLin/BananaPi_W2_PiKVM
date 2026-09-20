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
        libfdt-dev \
        libncurses-dev \
        libssl-dev \
        lz4 \
        lzop \
        make \
        pahole \
        pkg-config \
        python3 \
        python3-dev \
        python3-pip \
        rsync \
        swig \
        u-boot-tools \
        xz-utils \
        zstd \
        yamllint \
    && rm -rf /var/lib/apt/lists/*

# dtschema, for `CHECK_DTBS=1 make kernel-mainline`. Not packaged in trixie,
# and its pylibfdt dependency is built from source -- hence swig, libfdt-dev,
# python3-dev and pkg-config above.
RUN pip3 install --break-system-packages --no-cache-dir --root-user-action=ignore \
        dtschema

# Shared with the bullseye builder: run as the caller's uid/gid so build
# outputs are not left owned by root.
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
