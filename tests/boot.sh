#!/bin/bash
SUITE=$1
SP=/tmp/claude-1000/-home-jalen-bpiw2-pikvm/11517dc8-8cdd-4252-ad6d-18310b9d2280/scratchpad/qemutest
docker run --rm --entrypoint bash -v "$SP:/t" debian:bookworm -c "
apt-get update -qq && apt-get install -qq -y qemu-system-arm >/dev/null 2>&1
cd /t
timeout 240 qemu-system-aarch64 -M virt -cpu cortex-a57 -smp 4 -m 2048 \
  -kernel linux-4.9.337/arch/arm64/boot/Image \
  -append 'root=/dev/vda rw console=ttyAMA0 rootwait' \
  -drive file=rootfs-$SUITE.img,if=none,id=hd0,format=raw \
  -device virtio-blk-device,drive=hd0 \
  -nographic -no-reboot 2>&1
" > $SP/boot-$SUITE.log 2>&1
echo "--- $SUITE ---"
if grep -aq BOOT_TEST_MARKER_OK $SP/boot-$SUITE.log; then
  sed -n '/BOOT_TEST_MARKER_OK/,/BOOT_TEST_DONE/p' $SP/boot-$SUITE.log | sed 's/\r//'
else
  echo "Boot failed:"
  grep -aE "systemd\[1\]|panic|Failed" $SP/boot-$SUITE.log | head -10
fi
