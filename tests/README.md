# userspace-on-4.9 compatibility tests

Purpose: boot various userspaces on a 4.9 kernel under QEMU and see whether
the init system survives. The BPI-W2 can only use the BSP's 4.9.119 kernel,
and systemd has been steadily raising its kernel baseline, so this test is
how the distribution was chosen rather than guessed.

## Method

1. Build a vanilla linux-4.9.337 arm64 kernel with cgroup / namespace /
   seccomp options matched to the BSP's `rtd129x_bpi_defconfig`
2. Turn the userspace under test into an ext4 image and drop in a oneshot
   unit that prints `=====BOOT_TEST_MARKER_OK=====` along with the systemd
   and Python versions, then powers off
3. Boot it with `qemu-system-aarch64 -M virt` and see whether the marker
   appears

## Results as of 2026-09-04 (kernel 4.9.337)

| userspace | systemd | Python | Result |
|---|---|---|---|
| Arch Linux ARM aarch64 | 261.2 | — | **Fails**, PID 1 freezes |
| Debian 13 trixie | 257.13 | 3.13.5 | Boots, 0 failed units, unified cgroup v2 |
| Debian 12 bookworm | 252.39 | 3.11.2 | Boots, 0 failed units |

ALARM's failure output:

```
systemd[1]: Failed to determine whether /proc is a mount point: Invalid argument
systemd[1]: Failed to determine whether /sys is a mount point: Invalid argument
systemd[1]: Failed to determine whether /dev is a mount point: Invalid argument
[!!!!!!] Failed to mount early API filesystems.
systemd[1]: Freezing execution.
```

systemd 258 dropped cgroup v1 and raised its kernel requirement to 5.4;
257's "4.15 minimum" is a soft recommendation by comparison and works fine on
4.9 in practice.

## Caveat

These tests used vanilla 4.9.337 under QEMU virt, **not** the BSP's 4.9.119
on real hardware. See `../docs/02-decisions.md` D3 for the re-verification on
the actual board.
