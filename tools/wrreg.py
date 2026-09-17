#!/usr/bin/env python3
"""Write a single SoC register. Usage: wrreg.py <addr> <value>"""
import mmap, os, struct, sys
addr, val = int(sys.argv[1], 0), int(sys.argv[2], 0)
PAGE = 4096
base, off = addr & ~(PAGE-1), addr & (PAGE-1)
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ|mmap.PROT_WRITE, offset=base)
old = struct.unpack("<I", m[off:off+4])[0]
m[off:off+4] = struct.pack("<I", val)
new = struct.unpack("<I", m[off:off+4])[0]
print(f"0x{addr:08X}: 0x{old:08X} -> 0x{new:08X}")
m.close(); os.close(fd)
