#!/usr/bin/env python3
"""
Poll rtk_hdmirx's DDC status registers to see whether any I2C activity occurs.

  0x98037704  DDC2_I2C_SR1   bit7 cmderr / bit4 finish / bit3 timeout
  0x9803770C  DDC2_EDID_CR   bit0 edid_en
  0x98037720  DDC2_DDC_SIR   SRAM index (hardware advances it as the master reads)

Usage: ddc-watch.py [seconds]
"""
import mmap, os, struct, sys, time

BASE, PAGE = 0x98037700, 4096
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE & ~(PAGE-1))
off = BASE & (PAGE-1)
def rd(r): return struct.unpack("<I", m[off+r:off+r+4])[0]

seen = {}
t0 = time.time()
n = 0
while time.time() - t0 < dur:
    snap = (rd(0x04), rd(0x0C), rd(0x20), rd(0x1C))
    seen[snap] = seen.get(snap, 0) + 1
    n += 1
m.close(); os.close(fd)

print(f"  {n} polls, {len(seen)} distinct states seen")
for s, c in sorted(seen.items(), key=lambda x: -x[1])[:6]:
    print(f"    SR1=0x{s[0]:02X}  EDID_CR=0x{s[1]:02X}  SIR=0x{s[2]:02X}  EDID_IR=0x{s[3]:02X}   ({c}x)")
