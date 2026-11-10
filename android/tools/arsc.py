#!/usr/bin/env python3
"""Tiny resources.arsc writer: one package (0x7f) with drawable resources only.

Enough to give the APK a launcher icon without aapt.
Usage: arsc.py out.arsc name=res/drawable/file.png [...]
Resource ids are 0x7f010000, 0x7f010001, ... in argument order.
"""
import struct
import sys


def string_pool(strings, utf8=True):
    offsets, data = [], b""
    for s in strings:
        offsets.append(len(data))
        if utf8:
            b = s.encode("utf-8")
            assert len(s) < 128 and len(b) < 128
            data += bytes([len(s), len(b)]) + b + b"\x00"
        else:
            e = s.encode("utf-16-le")
            data += struct.pack("<H", len(e) // 2) + e + b"\x00\x00"
    while len(data) % 4:
        data += b"\x00"
    hs = 28
    start = hs + 4 * len(strings)
    flags = 0x100 if utf8 else 0
    hdr = struct.pack("<HHIIIIII", 0x0001, hs, start + len(data), len(strings), 0, flags, start, 0)
    return hdr + b"".join(struct.pack("<I", o) for o in offsets) + data


def build(entries):
    names = [n for n, _ in entries]
    paths = [p for _, p in entries]
    values = string_pool(paths)
    types = string_pool(["drawable"])
    keys = string_pool(names)

    n = len(entries)
    spec = struct.pack("<HHIBBHI", 0x0202, 16, 16 + 4 * n, 1, 0, 0, n) + b"\x00\x00\x00\x00" * n

    config = struct.pack("<I", 64) + b"\x00" * 60
    type_hdr = 20 + len(config)
    entries_start = type_hdr + 4 * n
    ent = b""
    offs = []
    for i in range(n):
        offs.append(len(ent))
        ent += struct.pack("<HHI", 8, 0, i) + struct.pack("<HBBI", 8, 0, 0x03, i)
    typ = struct.pack("<HHIBBHII", 0x0201, type_hdr, entries_start + len(ent), 1, 0, 0, n, entries_start)
    typ += config + b"".join(struct.pack("<I", o) for o in offs) + ent

    pkg_hdr = 288
    name = "com.phonecam.app".encode("utf-16-le").ljust(256, b"\x00")
    body = types + keys + spec + typ
    pkg = struct.pack("<HHII", 0x0200, pkg_hdr, pkg_hdr + len(body), 0x7F) + name
    pkg += struct.pack("<IIIII", pkg_hdr, 1, pkg_hdr + len(types), n, 0)
    assert len(pkg) == pkg_hdr
    pkg += body

    content = values + pkg
    return struct.pack("<HHII", 0x0002, 12, 12 + len(content), 1) + content


if __name__ == "__main__":
    out = sys.argv[1]
    entries = [tuple(x.split("=", 1)) for x in sys.argv[2:]]
    with open(out, "wb") as f:
        f.write(build(entries))
