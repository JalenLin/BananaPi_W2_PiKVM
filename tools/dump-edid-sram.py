#!/usr/bin/env python3
"""
Read the EDID SRAM contents back out of rtk_hdmirx's DDC block.

  DDC base        0x98037700
  DDC2_DDC_SIR    +0x20  SRAM index
  DDC2_DDC_SAP    +0x24  SRAM access port

The driver's drvif_EDIDLoad() writes the EDID in through these same two
registers; this reads it back the same way to verify it.
"""
import mmap, os, struct

BASE = 0x98037700
SIR, SAP = 0x20, 0x24
PAGE = 4096

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED,
              mmap.PROT_READ | mmap.PROT_WRITE, offset=BASE & ~(PAGE - 1))
off = BASE & (PAGE - 1)

def wr(reg, val): m[off+reg:off+reg+4] = struct.pack("<I", val)
def rd(reg):      return struct.unpack("<I", m[off+reg:off+reg+4])[0]

edid = bytearray()
for i in range(256):
    wr(SIR, i)
    edid.append(rd(SAP) & 0xFF)

m.close(); os.close(fd)

print("=== First 32 bytes of EDID SRAM ===")
for r in range(0, 32, 16):
    print("  %02X: %s  |%s|" % (r, " ".join(f"{b:02x}" for b in edid[r:r+16]),
          "".join(chr(b) if 32 <= b < 127 else "." for b in edid[r:r+16])))
hdr = bytes(edid[0:8]) == bytes([0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00])
print(f"\n  EDID header : {'OK' if hdr else 'bad/blank'}")
print(f"  block0 sum  : 0x{sum(edid[0:128]) & 0xFF:02X}  -> {'OK' if sum(edid[0:128]) & 0xFF == 0 else 'MISMATCH'}")
print(f"  block1 sum  : 0x{sum(edid[128:256]) & 0xFF:02X}  -> {'OK' if sum(edid[128:256]) & 0xFF == 0 else 'MISMATCH'}")
nz = sum(1 for b in edid if b != 0)
print(f"  non-zero    : {nz}/256")
# Look for the monitor name
try:
    i = bytes(edid).index(b"\x00\x00\x00\xfc\x00")
    print(f"  monitor name: {bytes(edid[i+5:i+18]).decode('ascii', 'replace').strip()}")
except ValueError:
    pass
