# BPI-W2 PiKVM - top-level build flow
#
# All compilation happens inside Docker containers (see
# docker/builder.Dockerfile); the host only needs docker, git and bash.

.PHONY: help builder sources kernel uboot rootfs image all clean-kernel distclean \
        builder-mainline sources-mainline kernel-mainline image-mainline \
        clean-kernel-mainline

help:
	@echo "make builder   - build the docker image used for compiling"
	@echo "make sources   - fetch the upstream sources and apply patches/"
	@echo "make kernel    - build kernel + dtb + modules"
	@echo "make uboot     - build u-boot"
	@echo "make rootfs    - build the Debian 13 arm64 rootfs"
	@echo "make image     - assemble the flashable SD image"
	@echo ""
	@echo "from scratch: make all"
	@echo ""
	@echo "mainline/LTS kernel line (see docs/09-mainline-bringup.md):"
	@echo "make builder-mainline  - build the trixie compile container"
	@echo "make sources-mainline  - fetch the mainline kernel next to the BSP"
	@echo "make kernel-mainline   - build Image + dtbs + modules"
	@echo "make image-mainline    - assemble an SD image with that kernel"

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

# --- mainline/LTS kernel line -------------------------------------------
#
# A second kernel tree alongside the BSP, not a replacement for it: u-boot,
# the audio firmware and the vendor initramfs still come from vendor/bpi-w2-bsp.

builder-mainline:
	docker build -t bpiw2-pikvm/builder-mainline:trixie -f docker/builder-mainline.Dockerfile docker/

sources-mainline:
	WITH_MAINLINE=1 scripts/prepare-sources.sh

kernel-mainline:
	scripts/build-kernel-mainline.sh

image-mainline:
	KERNEL_FLAVOUR=mainline scripts/build-image.sh

clean-kernel-mainline:
	BUILDER_IMAGE=bpiw2-pikvm/builder-mainline:trixie scripts/in-docker.sh \
	    bash -c 'cd /work/vendor/linux-mainline && make ARCH=arm64 mrproper'

clean-kernel:
	scripts/in-docker.sh bash -c 'cd /work/vendor/bpi-w2-bsp && make kernel-clean'

distclean:
	rm -rf vendor build
