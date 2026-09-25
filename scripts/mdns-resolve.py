#!/usr/bin/env python3
"""Resolve a .local name with one multicast DNS query and print its IPv4.

For hosts without nss-mdns: the board answers mDNS for bpi-w2-pikvm.local
(systemd-resolved, see build-rootfs.sh), but plain getent will not ask.
Standard library only.

    scripts/mdns-resolve.py bpi-w2-pikvm.local [timeout-seconds]
"""
import socket
import struct
import sys
import time

MDNS = ("224.0.0.251", 5353)


def query(name):
    qname = b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split("."))
    # id 0, no flags, one question; QTYPE A, QCLASS IN with the unicast-response bit
    return struct.pack("!6H", 0, 0, 1, 0, 0, 0) + qname + b"\0" + struct.pack("!HH", 1, 0x8001)


def skip_name(msg, off):
    while True:
        n = msg[off]
        if n == 0:
            return off + 1
        if n & 0xC0 == 0xC0:
            return off + 2
        off += 1 + n


def read_name(msg, off):
    labels = []
    while True:
        n = msg[off]
        if n == 0:
            return ".".join(labels)
        if n & 0xC0 == 0xC0:
            off = ((n & 0x3F) << 8) | msg[off + 1]
            continue
        labels.append(msg[off + 1:off + 1 + n].decode(errors="replace"))
        off += 1 + n


def answers(msg):
    _, _, qd, an, ns, ar = struct.unpack("!6H", msg[:12])
    off = 12
    for _ in range(qd):
        off = skip_name(msg, off) + 4
    for _ in range(an + ns + ar):
        name = read_name(msg, off)
        off = skip_name(msg, off)
        rtype, _, _, rdlen = struct.unpack("!HHIH", msg[off:off + 10])
        off += 10
        if rtype == 1 and rdlen == 4:
            yield name, socket.inet_ntoa(msg[off:off + 4])
        off += rdlen


def main():
    name = sys.argv[1]
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    # Not every responder answers a query from an ephemeral port ("legacy
    # unicast"), so listen where real mDNS answers go -- port 5353 and the
    # group -- sharing the port with avahi if it is running.
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s.bind(("", MDNS[1]))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     socket.inet_aton(MDNS[0]) + socket.inet_aton("0.0.0.0"))
    except OSError:
        pass
    s.settimeout(0.5)
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.sendto(query(name), MDNS)
        try:
            while True:
                msg, _ = s.recvfrom(9000)
                for rname, addr in answers(msg):
                    if rname.lower().rstrip(".") == name.lower().rstrip("."):
                        print(addr)
                        return 0
        except socket.timeout:
            pass
        except (IndexError, struct.error):
            pass
    return 1


if __name__ == "__main__":
    sys.exit(main())
