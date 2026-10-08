#!/usr/bin/env python3
# Point the audio firmware's printf at a memory ring and read it back
# (docs/09 §24). On the board, as root:
#   acpu-fwlog.py setup   -- ring in rpc_ringbuf, debug output on
#   acpu-fwlog.py dump    -- print what is in the ring
# The firmware (bluecore.audio 166265, the one build whose layout is known)
# keeps a pointer to the IPC block's printk_buffer at 0x8fc5db1c and the
# ring's KSEG1 address at 0x8fc5db20; printk_buffer is six little-endian
# words: buffer, size, length, start, start2, end.
import ctypes, mmap, os, sys
FW, IPC_PK = 0x0f900000, 0x0001f0e8
BUF, SIZE = 0x02000000, 0x2000
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
def region(phys, size):
    pg = phys & ~0xfff
    m = mmap.mmap(fd, (phys - pg + size + 0xfff) & ~0xfff, mmap.MAP_SHARED,
                  mmap.PROT_READ | mmap.PROT_WRITE, offset=pg)
    return m, phys - pg
def bswap(v): return int.from_bytes(v.to_bytes(4, "little"), "big")
fw, fo = region(FW, 0x400000)
fww = (ctypes.c_uint32 * (len(fw) // 4)).from_buffer(fw)
def fw_get(kseg0): return bswap(fww[(kseg0 - 0x8f900000) // 4])
def fw_set(kseg0, v): fww[(kseg0 - 0x8f900000) // 4] = bswap(v)
ipc, io = region(IPC_PK, 24)
ipw = (ctypes.c_uint32 * (len(ipc) // 4)).from_buffer(ipc)
buf, bo = region(BUF, SIZE)
bufw = (ctypes.c_uint32 * (len(buf) // 4)).from_buffer(buf)
if sys.argv[1] == "setup":
    for i in range(SIZE // 4): bufw[bo // 4 + i] = 0
    vals = [BUF, SIZE, 0, 0, 0, 0]
    for i, v in enumerate(vals): ipw[io // 4 + i] = v
    fw_set(0x8fc5db20, BUF | 0xa0000000)
    fw_set(0x8fc5db1c, 0xa0000000 | IPC_PK)
    flag = fw_get(0x8fc5daa4)
    fw_set((flag & 0x1fffffff) | 0x80000000, fw_get((flag & 0x1fffffff) | 0x80000000) | 1)
    print("ptr %08x buf %08x flag@%08x" % (fw_get(0x8fc5db1c), fw_get(0x8fc5db20), flag))
else:
    f = [ipw[io // 4 + i] for i in range(6)]
    print("fields", " ".join("%08x" % v for v in f), file=sys.stderr)
    end, ln = f[5], f[2]
    data = bytes(buf[bo:bo + SIZE])
    start = max(0, end - min(ln, SIZE))
    out = bytes(data[i % SIZE] for i in range(start, end))
    sys.stdout.write(out.decode("latin-1"))
