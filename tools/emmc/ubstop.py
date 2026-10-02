# Catch the eMMC u-boot (bootdelay=0) by sending Esc until its prompt shows,
# then run the given commands and log everything.
import os, sys, time, termios, select
fd = os.open("/dev/ttyUSB0", os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
log = open("/io/ubstop.log", "ab", buffering=0)
buf = bytearray()
def rd(t):
    r, _, _ = select.select([fd], [], [], t)
    if r:
        d = os.read(fd, 4096); buf.extend(d); log.write(d); return d
    return b""
end = time.time() + 400
seen = False
while time.time() < end:
    os.write(fd, b"\x1b")
    rd(0.05)
    tail = bytes(buf[-300:])
    if b"Realtek>" in tail or b"BPI-W2>" in tail or b"> " == tail[-2:]:
        seen = True
        break
if not seen:
    print("no u-boot prompt"); sys.exit(1)
time.sleep(1); rd(0.5)
# The Esc spam can leave the line editor holding a partial escape sequence
# that eats the next character: start on a fresh, empty line
os.write(fd, b"\r"); time.sleep(0.5); rd(0.5)
for cmd in sys.argv[1:]:
    w = 4
    if cmd.startswith("@"):
        w, cmd = cmd[1:].split(":", 1); w = float(w)
    os.write(fd, cmd.encode() + b"\r")
    t = time.time()
    while time.time() - t < w:
        rd(0.2)
print(bytes(buf[-12000:]).decode(errors="replace"))
