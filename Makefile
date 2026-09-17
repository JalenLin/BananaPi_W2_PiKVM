# BPI-W2 PiKVM - top-level build flow
#
# All compilation happens inside Docker containers (see
# docker/builder.Dockerfile); the host only needs docker, git and bash.

.PHONY: help builder sources kernel uboot rootfs image all clean-kernel distclean

help:
	@echo "make builder   - build the docker image used for compiling"
	@echo "make sources   - fetch the upstream sources and apply patches/"
	@echo "make kernel    - build kernel + dtb + modules"
	@echo "make uboot     - build u-boot"
	@echo "make rootfs    - build the Debian 13 arm64 rootfs"
	@echo "make image     - assemble the flashable SD image"
	@echo ""
	@echo "from scratch: make all"

builder:
	docker build -t bpiw2-pikvm/builder:bullseye -f docker/builder.Dockerfile docker/

sources:
	scripts/prepare-sources.sh

kernel:
	scripts/build-kernel.sh

uboot:
	scripts/in-docker.sh bash -c 'cd /work/vendor/bpi-w2-bsp && make u-boot'

rootfs:
	scripts/build-rootfs.sh

image:
	scripts/build-image.sh

all: builder sources kernel uboot rootfs image

clean-kernel:
	scripts/in-docker.sh bash -c 'cd /work/vendor/bpi-w2-bsp && make kernel-clean'

distclean:
	rm -rf vendor build
