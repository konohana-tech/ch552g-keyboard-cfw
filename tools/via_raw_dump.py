#!/usr/bin/env python3
"""Raw VIA GET_KEYCODE response hex dump — see exact wire bytes.
Usage: python3 tools/via_raw_dump.py [layer] [row] [col]
Defaults: layer=0 row=0 col=0
"""
import glob
import os
import re
import select
import struct
import sys

VID_PID = (0x1209, 0x0001)


def find_raw_hid():
    for p in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        try:
            txt = open(p).read()
        except OSError:
            continue
        m = re.search(r"HID_ID=([0-9A-Fa-f]+):([0-9A-Fa-f]+):([0-9A-Fa-f]+)", txt)
        if not m:
            continue
        if (int(m.group(2), 16), int(m.group(3), 16)) != VID_PID:
            continue
        if os.path.isdir(os.path.join(os.path.dirname(p), "input")):
            continue
        node = "/dev/" + p.split("/")[4]
        try:
            fd = os.open(node, os.O_RDWR)
        except OSError:
            continue
        try:
            os.write(fd, bytes([0xFE, 0x00]) + bytes(30))
            r, _, _ = select.select([fd], [], [], 0.3)
            if r and os.read(fd, 32)[0] == 0x06:
                os.close(fd)
                return node
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass
    return None


def main():
    dev = os.environ.get("VIAL_DEV") or find_raw_hid()
    if not dev:
        print("RAW HID not found")
        return 1
    print(f"DEV={dev}")
    layer = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    row = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    col = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    fd = os.open(dev, os.O_RDWR)
    req = bytes([0x04, layer, row, col]) + bytes(28)
    print(f"REQ: {req.hex(' ')}")
    os.write(fd, req)
    r, _, _ = select.select([fd], [], [], 1.0)
    if not r:
        print("NO RESPONSE")
        os.close(fd)
        return 1
    resp = os.read(fd, 32)
    os.close(fd)
    print(f"RESP ({len(resp)}B): {resp.hex(' ')}")
    print(f"resp[0] = 0x{resp[0]:02X}  (cmd echo?)")
    if len(resp) >= 3:
        print(f"resp[1] = 0x{resp[1]:02X}")
        print(f"resp[2] = 0x{resp[2]:02X}")
        print(f"resp[0:2] as BE16 = 0x{(resp[0] << 8 | resp[1]):04X}  (old script read)")
        print(f"resp[1:3] as BE16 = 0x{(resp[1] << 8 | resp[2]):04X}  (new script read)")
        print(f"resp[0:2] as LE16 = 0x{(resp[1] << 8 | resp[0]):04X}")
        print(f"resp[1:3] as LE16 = 0x{(resp[2] << 8 | resp[1]):04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
