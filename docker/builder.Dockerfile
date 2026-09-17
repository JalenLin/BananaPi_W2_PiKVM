# BPI-W2 PiKVM - cross-compile environment for the BSP (kernel 4.9.119 + u-boot 2015.07)
#
# Deliberately pinned to Debian bullseye:
#   * OpenSSL 1.1.1 -- kernel 4.9's scripts/sign-file uses the 1.1 API and
#     will not compile against bookworm's OpenSSL 3
#   * GNU make 4.3 -- make 4.4 changed $(shell) behaviour, which breaks 4.9's Makefiles
#   * python2.7 is still packaged -- some u-boot 2015.07 scripts need it
FROM debian:bullseye-slim

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
        git \
        gzip \
        kmod \
        libncurses-dev \
        libssl-dev \
        lzop \
        make \
        python2 \
        rsync \
        u-boot-tools \
        unzip \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Some u-boot 2015.07 scripts hardcode `python`
RUN ln -sf /usr/bin/python2 /usr/local/bin/python

# Run as the caller's uid/gid inside the container so build outputs are not owned by root
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
