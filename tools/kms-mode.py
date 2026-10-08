#!/usr/bin/env python3
# A minimal KMS client for the HDMI output (docs/09 §24). On the board:
#   kms-mode.py            -- list the connector's modes
#   kms-mode.py IDX SECS   -- set mode IDX with a test pattern, hold it SECS
import ctypes as C, fcntl, mmap, os, sys, time

def IOWR(nr, size): return (3 << 30) | (size << 16) | (ord('d') << 8) | nr

class Res(C.Structure):
    _fields_ = [("fb", C.c_uint64), ("crtc", C.c_uint64), ("conn", C.c_uint64), ("enc", C.c_uint64),
                ("nfb", C.c_uint32), ("ncrtc", C.c_uint32), ("nconn", C.c_uint32), ("nenc", C.c_uint32),
                ("minw", C.c_uint32), ("maxw", C.c_uint32), ("minh", C.c_uint32), ("maxh", C.c_uint32)]
class Mode(C.Structure):
    _fields_ = [("clock", C.c_uint32)] + [(n, C.c_uint16) for n in
                "hdisplay hsync_start hsync_end htotal hskew vdisplay vsync_start vsync_end vtotal vscan".split()] + \
               [("vrefresh", C.c_uint32), ("flags", C.c_uint32), ("type", C.c_uint32), ("name", C.c_char * 32)]
class Conn(C.Structure):
    _fields_ = [("enc", C.c_uint64), ("modes", C.c_uint64), ("props", C.c_uint64), ("pvals", C.c_uint64),
                ("nmodes", C.c_uint32), ("nprops", C.c_uint32), ("nenc", C.c_uint32), ("enc_id", C.c_uint32),
                ("id", C.c_uint32), ("type", C.c_uint32), ("type_id", C.c_uint32), ("connection", C.c_uint32),
                ("mmw", C.c_uint32), ("mmh", C.c_uint32), ("subpixel", C.c_uint32), ("pad", C.c_uint32)]
class Dumb(C.Structure):
    _fields_ = [("height", C.c_uint32), ("width", C.c_uint32), ("bpp", C.c_uint32), ("flags", C.c_uint32),
                ("handle", C.c_uint32), ("pitch", C.c_uint32), ("size", C.c_uint64)]
class MapDumb(C.Structure):
    _fields_ = [("handle", C.c_uint32), ("pad", C.c_uint32), ("offset", C.c_uint64)]
class FbCmd(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in "fb_id width height pitch bpp depth handle".split()]
class Crtc(C.Structure):
    _fields_ = [("conns", C.c_uint64)] + [(n, C.c_uint32) for n in
                "nconns crtc_id fb_id x y gamma_size mode_valid".split()] + [("mode", Mode)]

def io(fd, nr, s):
    fcntl.ioctl(fd, IOWR(nr, C.sizeof(s)), s, True)
    return s

fd = os.open("/dev/dri/card0", os.O_RDWR)
fcntl.ioctl(fd, (ord('d') << 8) | 0x1e)          # SET_MASTER
r = io(fd, 0xA0, Res())
crtcs = (C.c_uint32 * r.ncrtc)(); conns = (C.c_uint32 * r.nconn)()
r.crtc = C.addressof(crtcs); r.conn = C.addressof(conns); r.nfb = r.nenc = 0
io(fd, 0xA0, r)
c = io(fd, 0xA7, Conn(id=conns[0]))
modes = (Mode * c.nmodes)(); encs = (C.c_uint32 * max(c.nenc, 1))()
c2 = Conn(id=conns[0], modes=C.addressof(modes), nmodes=c.nmodes, enc=C.addressof(encs), nenc=c.nenc)
io(fd, 0xA7, c2)

if len(sys.argv) < 2:
    for i, m in enumerate(modes):
        print(i, m.name.decode(), m.vrefresh, m.clock, "i" if m.flags & 0x10 else "p",
              "pref" if m.type & 8 else "")
    sys.exit(0)

m = modes[int(sys.argv[1])]
W, H = m.hdisplay, m.vdisplay
d = io(fd, 0xB2, Dumb(width=W, height=H, bpp=32))
off = io(fd, 0xB3, MapDumb(handle=d.handle)).offset
buf = mmap.mmap(fd, d.size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)

# The pattern: 8 colour bars on the top half, a grey ramp below, a 4-pixel
# white border and a magenta cross through the centre.
bars = [0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000]
def px(v): return v.to_bytes(4, "little")
top = b"".join(px(bars[x * 8 // W]) for x in range(W))
bot = b"".join(px((x * 255 // (W - 1)) * 0x010101) for x in range(W))
white = px(0xffffff) * W
pitch = d.pitch
for y in range(H):
    row = top if y < H // 2 else bot
    if y < 4 or y >= H - 4 or abs(y - H // 2) < 2:
        row = white if y < 4 or y >= H - 4 else px(0xff00ff) * W
    else:
        row = bytearray(row)
        row[0:16] = px(0xffffff) * 4
        row[(W - 4) * 4:W * 4] = px(0xffffff) * 4
        row[(W // 2 - 2) * 4:(W // 2 + 2) * 4] = px(0xff00ff) * 4
        row = bytes(row)
    buf[y * pitch:y * pitch + W * 4] = row

fb = io(fd, 0xAE, FbCmd(width=W, height=H, pitch=pitch, bpp=32, depth=24, handle=d.handle))
cid = (C.c_uint32 * 1)(conns[0])
io(fd, 0xA2, Crtc(conns=C.addressof(cid), nconns=1, crtc_id=crtcs[0], fb_id=fb.fb_id, mode_valid=1, mode=m))
print("set", m.name.decode(), m.vrefresh, flush=True)
time.sleep(float(sys.argv[2]))
