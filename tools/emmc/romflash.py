#!/usr/bin/env python3
# Program the BPI-W2 eMMC bootloader through the RTD1296 ROM's serial
# download mode, as the BPI wiki describes for hypertrm:
#   hold ctrl+q at power-on until "d/g/r"; h + Y-modem hwsetting;
#   s98007058 / 01500000; d + Y-modem dvrboot.exe.bin; g.
# usage: romflash.py <hwsetting.bin> <dvrboot.exe.bin> [--wait-only]
import os, sys, time, termios, select

DEV = "/dev/ttyUSB0"
log = open("/io/romflash.log", "ab", buffering=0)

fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
termios.tcflush(fd, termios.TCIOFLUSH)

buf = bytearray()

def rd(t=0.05):
    r, _, _ = select.select([fd], [], [], t)
    if r:
        d = os.read(fd, 4096)
        if d:
            buf.extend(d); log.write(d)
            return d
    return b""

def wr(b):
    os.write(fd, b)

def expect(pat, timeout, kick=None, kick_every=0.02):
    end = time.time() + timeout
    start = len(buf)
    last = 0
    while time.time() < end:
        if kick is not None and time.time() - last >= kick_every:
            wr(kick); last = time.time()
        rd(0.01)
        if pat in bytes(buf[start:]):
            return True
    return False

def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc

SOH, STX, EOT, ACK, NAK, CAN = 1, 2, 4, 6, 0x15, 0x18

def getc(timeout):
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            d = os.read(fd, 1)
            if d:
                log.write(d)
                return d[0]
    return None

def block(seq, data, size):
    data = data.ljust(size, b"\x1a" if seq else b"\x00")
    c = crc16(data)
    return bytes([SOH if size == 128 else STX, seq & 0xff, 0xff - (seq & 0xff)]) + data + bytes([c >> 8, c & 0xff])

def send_block(pkt, what):
    for attempt in range(10):
        # the ROM sends 'C' without pause while it waits; drop those
        termios.tcflush(fd, termios.TCIFLUSH)
        wr(pkt)
        end = time.time() + 10
        c = None
        while time.time() < end:
            c = getc(end - time.time())
            if c in (ACK, NAK, CAN) or c is None:
                break
        if c == ACK:
            return True
        print("  %s: got %r, retry" % (what, c), flush=True)
        if c == CAN:
            return False
    return False

def ymodem(path):
    data = open(path, "rb").read()
    name = os.path.basename(path).encode()
    # The ROM first prints "download to 0x80006C30" and a Y-modem line, and
    # that address has a 'C' in it: let the text finish, then take a 'C'
    # that stands alone.
    if "--resume-ymodem" not in sys.argv:
        expect(b"Ymodem:", 10)
    while True:
        c = getc(60)
        if c is None:
            print("no 'C' from the receiver"); return False
        if c == ord("C"):
            break
    hdr = name + b"\0" + str(len(data)).encode() + b"\0"
    if not send_block(block(0, hdr, 128), "header"):
        return False
    seq = 1
    for off in range(0, len(data), 1024):
        if not send_block(block(seq, data[off:off + 1024], 1024), "block %d" % seq):
            return False
        seq += 1
        if seq % 64 == 0:
            print("  %d/%d KiB" % (off // 1024, len(data) // 1024), flush=True)
    for attempt in range(5):
        wr(bytes([EOT]))
        c = getc(10)
        if c == ACK:
            break
    getc(10)  # 'C' for the next file
    send_block(block(0, b"", 128), "end")
    print("  sent %s, %d bytes" % (name.decode(), len(data)), flush=True)
    return True

hw, dvr = sys.argv[1], sys.argv[2]
stage2 = "--stage2" in sys.argv
if "--resume-ymodem" in sys.argv:
    pass
elif stage2:
    # the ROM is already at its prompt from stage 1
    wr(b"\r")
    if not expect(b"d/g/r", 5):
        print("not at the d/g/r prompt"); sys.exit(1)
    while rd(0.3):
        pass
else:
  print("waiting for d/g/r (sending ctrl+q); reset or power-cycle the board now", flush=True)
  if not expect(b"d/g/r", 300, kick=b"\x11"):
    print("no d/g/r prompt"); sys.exit(1)
  time.sleep(0.5)
  while rd(0.2):
      pass
  print("ROM prompt reached", flush=True)
if "--probe" in sys.argv and not stage2:
    wr(b"\r"); time.sleep(1)
    while rd(0.3):
        pass
    print(bytes(buf[-400:]).decode(errors="replace"))
    sys.exit(0)

if not stage2:
    if "--resume-ymodem" not in sys.argv:
        wr(b"h")
    print("h: sending hwsetting", flush=True)
    if not ymodem(hw):
        sys.exit(2)
    expect(b"d/g/r", 10)
    time.sleep(0.5)
    wr(b"s"); time.sleep(0.5); wr(b"98007058"); time.sleep(0.5); wr(b"\r"); time.sleep(0.5)
    wr(b"01500000"); time.sleep(0.5); wr(b"\r"); time.sleep(1)
    while rd(0.3):
        pass
    print("set 98007058 = 01500000; stage 1 done", flush=True)
    print(bytes(buf[-600:]).decode(errors="replace"))
    sys.exit(0)

wr(b"d"); print("d: sending dvrboot", flush=True)
if not ymodem(dvr):
    sys.exit(3)
expect(b"d/g/r", 10)
time.sleep(0.5)
wr(b"g"); print("g: running the flash writer", flush=True)
# let it run and log what it says
end = time.time() + 300
while time.time() < end:
    d = rd(0.5)
    if b"Realtek>" in bytes(buf[-200:]) or b"BPI-W2>" in bytes(buf[-200:]):
        break
print("done; see romflash.log", flush=True)
