#!/usr/bin/env python3
"""Drive the BPI-W2's LK console over serial, one command at a time.

LK and the audio core (ACPU) share one UART, so its output arrives
interleaved character by character:

    Real[bteoknd>i ng] 1295 chip

Sending commands on fixed sleeps does not survive that -- `usb start` takes
an unpredictable time to enumerate, and anything typed while LK is busy is
dropped. Every command here is therefore sent only once the `Realtek> `
prompt has come back and the line has gone quiet.

Environment:
    DEV        serial device                   (default /dev/ttyUSB0)
    LOG        where to write the raw capture  (default /out/lk.log)
    BOOTARGS   written to /chosen/bootargs before booting
    CAPTURE    seconds to keep reading after `boot k`   (default 120)
    SKIP_BOOT_A=1  do not run `boot a`
    ACPU_RESET=1   hold the audio core in reset before `boot k`
    BASE       directory on the USB device holding the boot files
"""

import os
import re
import sys
import time

DEV = os.environ.get("DEV", "/dev/ttyUSB0")
LOG = os.environ.get("LOG", "/out/lk.log")
BASE = os.environ.get("BASE", "bananapi/bpi-w2/linux")
BOOTARGS = os.environ.get("BOOTARGS", "")
CAPTURE = int(os.environ.get("CAPTURE", "120"))

PROMPT = b"Realtek> "

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
log = open(LOG, "wb", buffering=0)
buf = bytearray()


def _read():
    try:
        d = os.read(fd, 4096)
    except BlockingIOError:
        return b""
    if d:
        buf.extend(d)
        log.write(d)
    return d


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        if not _read():
            time.sleep(0.02)


def wait_for(pat, timeout, quiet=1.0):
    """Wait for `pat`, then for the line to stay quiet for `quiet` seconds."""
    end = time.time() + timeout
    start = len(buf)
    while time.time() < end:
        if not _read():
            time.sleep(0.02)
        if pat in bytes(buf[start:]):
            calm = time.time() + quiet
            while time.time() < calm:
                if _read():
                    calm = time.time() + quiet
                else:
                    time.sleep(0.02)
            return True
    return False


def send(cmd, timeout=30, check=None):
    # LK eats the first character of a line, so nudge it with a bare CR and
    # let the prompt come back before the real command goes out.
    os.write(fd, b"\r")
    wait_for(PROMPT, 5, quiet=0.4)
    mark = len(buf)
    os.write(fd, cmd.encode() + b"\r")
    ok = wait_for(PROMPT, timeout, quiet=1.5)
    out = bytes(buf[mark:])

    status = "ok" if ok else "TIMEOUT"
    if check == "fatload":
        # This LK reports a load as "Size: <n>, got: <n>" -- not u-boot's
        # "<n> bytes read". A short read leaves the two numbers different.
        m = re.search(rb"Size:\s*(\d+),\s*got:\s*(\d+)", out)
        if not m:
            status = "NO-SIZE-LINE"
        elif m.group(1) != m.group(2):
            status = "SHORT:%s/%s" % (m.group(2).decode(), m.group(1).decode())
        elif m.group(1) == b"0":
            status = "EMPTY"

    print("  %-56s %s" % (cmd, status), flush=True)
    return status == "ok"


print(">>> waiting for the LK prompt", flush=True)
os.write(fd, b"\r")
if not wait_for(PROMPT, 15, quiet=0.5):
    print("  no Realtek> prompt -- is the board sitting at LK?", flush=True)
    sys.exit(2)

# `usb start` regularly fails to enumerate on the first try ("Device not
# responding to set address"), so it is always issued twice.
steps = [
    ("usb start", 90, None),
    ("usb start", 90, None),
    ("usb storage", 30, None),
    ("fatload usb 0:1 0x02100000 %s/bpi-w2.dtb" % BASE, 60, "fatload"),
    ("fatload usb 0:1 0x0f900000 %s/bluecore.audio" % BASE, 120, "fatload"),
    ("fatload usb 0:1 0x03000000 %s/uImage" % BASE, 300, "fatload"),
]

for cmd, timeout, check in steps:
    if not send(cmd, timeout, check) and check:
        # Booting on a half-loaded image runs whatever happens to be in RAM
        # and takes the ACPU down with it. Stop instead.
        print(">>> %s did not land the whole file -- not running boot k" % cmd,
              flush=True)
        sys.exit(3)

send("fdt addr 0x02100000", 20)
send('fdt set /chosen bootargs "%s"' % BOOTARGS, 20)
# `boot a` hands the audio core its firmware and does not return a prompt
# promptly, so its TIMEOUT is expected and not fatal.
#
# SKIP_BOOT_A=1 leaves the audio core stopped. Nothing in PiKVM needs it, and
# it keeps running alongside Linux writing into its own ION heaps -- so this
# is the switch for deciding whether it is the one corrupting kernel memory.
if os.environ.get("SKIP_BOOT_A", "0") != "1":
    send("boot a", 30)
else:
    print("  %-56s %s" % ("(boot a skipped)", "SKIP_BOOT_A=1"), flush=True)


def read_word(addr):
    mark = len(buf)
    send("dw 0x%08x 1" % addr, 10)
    m = re.search(rb"(?:0x)?0*%x:\s*(?:0x)?([0-9a-fA-F]{8})" % addr, bytes(buf[mark:]))
    return int(m.group(1), 16) if m else None


# ACPU_RESET=1 holds the audio core in reset before Linux starts. On the eMMC
# path the boot firmware has already started it -- FSBL logs "md copy audio
# bin" and LK "Set ACPU share memory" whether or not `boot a` runs -- and it
# keeps running with a memory layout Linux knows nothing about. Soft reset 2
# is CRT + 0x4; bit 0 is RSTN_ACPU, active low.
if os.environ.get("ACPU_RESET", "0") == "1":
    SOFT_RESET2 = 0x98000004
    before = read_word(SOFT_RESET2)
    if before is None:
        print(">>> could not read SOFT_RESET2 -- not touching it", flush=True)
    else:
        send("mw 0x%08x 0x%08x" % (SOFT_RESET2, before & ~1), 10)
        after = read_word(SOFT_RESET2)
        print("  SOFT_RESET2 0x%08x -> 0x%08s (ACPU %s)" % (
            before, "%08x" % after if after is not None else "????????",
            "in reset" if after is not None and not (after & 1) else "NOT in reset"),
            flush=True)

print(">>> boot k", flush=True)
os.write(fd, b"\r")
wait_for(PROMPT, 5, quiet=0.4)
os.write(fd, b"boot k\r")
pump(CAPTURE)
print(">>> done", flush=True)
