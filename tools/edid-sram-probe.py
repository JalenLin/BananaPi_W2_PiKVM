#!/usr/bin/env python3
"""Cross-check the EDID SRAM with different read strategies to tell whether an
offset happens on the write side or the read side."""
import mmap, os, struct

BASE, SIR, SAP, PAGE = 0x98037700, 0x20, 0x24, 4096
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
              offset=BASE & ~(PAGE - 1))
off = BASE & (PAGE - 1)
def wr(r, v): m[off+r:off+r+4] = struct.pack("<I", v)
def rd(r):    return struct.unpack("<I", m[off+r:off+r+4])[0]

# Method A: set the index before every byte
a = bytearray()
for i in range(16):
    wr(SIR, i); a.append(rd(SAP) & 0xFF)

# Method B: set the index once and read continuously (tests auto-increment)
wr(SIR, 0)
b = bytearray(rd(SAP) & 0xFF for _ in range(16))

# Method C: index minus one
c = bytearray()
for i in range(16):
    wr(SIR, (i - 1) & 0xFF); c.append(rd(SAP) & 0xFF)

m.close(); os.close(fd)
want = "00 ff ff ff ff ff ff 00 4a 8b 95 12 00 00 00 00"
print(f"  expected         : {want}")
print(f"  A index each time: {' '.join(f'{x:02x}' for x in a)}")
print(f"  B index once     : {' '.join(f'{x:02x}' for x in b)}")
print(f"  C index minus 1  : {' '.join(f'{x:02x}' for x in c)}")
