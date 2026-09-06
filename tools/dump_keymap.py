#!/usr/bin/env python3
"""Dump all keycodes L0-L3 via RAW HID (read-only, no writes).

Raw-wire verified (tools/via_raw_dump.py):
  k=0 REQ 04 00 00 00 -> RESP 22 04 ... = 0x2204 LSFT_T(A)
  k=1 REQ 04 00 00 01 -> RESP 00 05 ... = 0x0005 KC_B
So the keycode is resp[0:2] as BE16 (this firmware overwrites
msg[0]=hi, msg[1]=lo; no echo byte). Request: [0x04, layer, row, col].

Usage (one-shot, no args):
    python3 tools/dump_keymap.py
Override: VIAL_DEV=/dev/hidrawN python3 tools/dump_keymap.py
"""
import glob
import os
import re
import select
import struct
import sys

SCRIPT_ID = "DUMP_KEYMAP_ALL_v1-20260907"

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
    print(f"### SCRIPT_ID={SCRIPT_ID} ###")
    dev = os.environ.get("VIAL_DEV") or find_raw_hid()
    if not dev:
        print("RAW HID 1209:0001 not found. Override: VIAL_DEV=/dev/hidrawN")
        return 1
    print(f"[auto] using DEV={dev}")
    fd = os.open(dev, os.O_RDWR)
    for layer in range(4):
        print(f"--- L{layer} ---")
        for row in range(2):
            for col in range(4):
                os.write(fd, bytes([0x04, layer, row, col]) + bytes(28))
                r, _, _ = select.select([fd], [], [], 1.0)
                resp = os.read(fd, 32) if r else None
                if resp and len(resp) >= 2:
                    kc = (resp[0] << 8) | resp[1]
                    print(f"  row={row} col={col} (k={row * 4 + col}): {kc:#06x}")
                else:
                    print(f"  row={row} col={col} (k={row * 4 + col}): NO RESPONSE")
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
