#!/usr/bin/env python3
"""Read SoC registers through /dev/mem (read-only). Usage: rdreg.py <addr> [count]"""
import mmap, os, struct, sys

addr = int(sys.argv[1], 0)
count = int(sys.argv[2], 0) if len(sys.argv) > 2 else 1
PAGE = 4096
base = addr & ~(PAGE - 1)
off = addr - base
span = ((off + count * 4 + PAGE - 1) // PAGE) * PAGE

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, span, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
for i in range(count):
    v = struct.unpack("<I", m[off + i*4: off + i*4 + 4])[0]
    print(f"0x{addr + i*4:08X}: 0x{v:08X}")
m.close(); os.close(fd)
