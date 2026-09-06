#!/usr/bin/env python3
"""Print EVERY key event from the 1209:0001 keyboard (all codes, not A/B only).

Use to map physical keys -> keycodes: press each physical key once,
left to right, and paste the output.
Usage (one-shot, no args):
    python3 tools/evkey_dump.py
Override: MT_DEV=/dev/input/eventN python3 tools/evkey_dump.py
"""
import os
import re
import select
import struct
import sys
import time

SCRIPT_ID = "EVKEY_DUMP_v1-20260907"

EV_KEY = 0x01
NAMES = {
    1: "ESC", 28: "ENTER", 29: "LCTRL", 42: "LSHIFT", 54: "RSHIFT",
    56: "LALT", 57: "SPACE", 15: "TAB", 14: "BKSP",
    16: "Q", 17: "W", 18: "E", 19: "R", 20: "T", 21: "Y", 22: "U",
    23: "I", 24: "O", 25: "P", 30: "A", 31: "S", 32: "D", 33: "F",
    34: "G", 35: "H", 36: "J", 37: "K", 38: "L", 44: "Z", 45: "X",
    46: "C", 47: "V", 48: "B", 49: "N", 50: "M",
}


def find_keyboard_event():
    try:
        txt = open("/proc/bus/input/devices").read()
    except OSError as e:
        print(f"cannot read /proc/bus/input/devices: {e}")
        return None
    for b in txt.strip().split("\n\n"):
        vm = re.search(r"Vendor=([0-9A-Fa-f]+)", b)
        pm = re.search(r"Product=([0-9A-Fa-f]+)", b)
        hm = re.search(r"Handlers=(.*)", b)
        if not vm or not pm or not hm:
            continue
        if (int(vm.group(1), 16), int(pm.group(1), 16)) == (0x1209, 0x0001):
            em = re.search(r"event(\d+)", hm.group(1))
            if em:
                return f"/dev/input/event{em.group(1)}"
    return None


def main():
    print(f"### SCRIPT_ID={SCRIPT_ID} ###")
    dev = os.environ.get("MT_DEV") or find_keyboard_event()
    if not dev:
        print("keyboard 1209:0001 not found. Override: MT_DEV=/dev/input/eventN")
        return 1
    print(f"[auto] using DEV={dev}")
    print("Press each physical key ONCE, left to right. Ctrl-C to quit.")
    fd = os.open(dev, os.O_RDONLY)
    fmt = "llHHi"
    size = struct.calcsize(fmt)
    t0 = None
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 60)
            if not r:
                print("(idle 60s, still waiting...)")
                continue
            data = os.read(fd, size)
            if len(data) < size:
                continue
            sec, usec, typ, code, val = struct.unpack(fmt, data)
            if typ != EV_KEY or val not in (0, 1):
                continue
            t = sec + usec / 1e6
            if t0 is None:
                t0 = t
            name = NAMES.get(code, f"CODE{code}")
            print(f"+{t - t0:8.3f}s  {name} {'down' if val else 'up'}")
    except KeyboardInterrupt:
        print("\ndone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
