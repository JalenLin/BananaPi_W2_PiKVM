# Run commands at an already-waiting u-boot prompt and print the output
import os, sys, time, termios, select
fd = os.open("/dev/ttyUSB0", os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
a[4] = a[5] = termios.B115200
a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)
out = bytearray()
wait = float(os.environ.get("WAIT", "3"))
for cmd in [""] + sys.argv[1:]:
    os.write(fd, cmd.encode() + b"\r")
    t = time.time()
    while time.time() - t < wait:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            out += os.read(fd, 4096)
sys.stdout.write(out.decode(errors="replace"))
