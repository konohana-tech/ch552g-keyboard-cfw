#!/usr/bin/env python3
"""MT debug poller: reads firmware debug state + evdev events simultaneously.

Polls 0xFE/0x0A (mt_packed/stamp/tt_now/cause/key_debounced) every ~5ms
while monitoring /dev/input/event* for key events. Logs both with
timestamps so we can see exactly what the firmware state is at each event.

Usage (one-shot, no args):
    python3 tools/mt_debug_poll.py
Override: VIAL_DEV=/dev/hidrawN python3 tools/mt_debug_poll.py
          MT_DEV=/dev/input/eventN python3 tools/mt_debug_poll.py
"""
import glob
import os
import re
import select
import struct
import sys
import time

SCRIPT_ID = "MT_DEBUG_POLL_v1-20260907"

EV_KEY = 0x01
KEY_NAMES = {
    1: "ESC", 14: "BKSP", 15: "TAB", 16: "Q", 17: "W", 18: "E",
    19: "R", 20: "T", 21: "Y", 22: "U", 23: "I", 24: "O", 25: "P",
    28: "ENTER", 29: "LCTRL", 30: "A", 31: "S", 32: "D", 33: "F",
    34: "G", 35: "H", 36: "J", 37: "K", 38: "L", 42: "LSHIFT",
    44: "Z", 45: "X", 46: "C", 47: "V", 48: "B", 49: "N", 50: "M",
    54: "RSHIFT", 56: "LALT", 57: "SPACE",
}

CAUSE_NAMES = {
    0: "PRESS",
    1: "TAP_INJECT",
    2: "ALREADY_HOLD",
    3: "STALE_RELEASE",
    4: "TERM_HOLD",
    5: "DEBOUNCE_GUARD",
}


def find_hidraw():
    for p in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        try:
            txt = open(p).read()
        except OSError:
            continue
        m = re.search(r"HID_ID=([0-9A-Fa-f]+):([0-9A-Fa-f]+):([0-9A-Fa-f]+)", txt)
        if not m:
            continue
        if (int(m.group(2), 16), int(m.group(3), 16)) == (0x1209, 0x0001):
            if os.path.isdir(os.path.join(os.path.dirname(p), "input")):
                continue
            node = "/dev/" + p.split("/")[4]
            try:
                fd = os.open(node, os.O_RDWR)
            except OSError:
                continue
            try:
                os.write(fd, bytes([0xFE, 0x0A]) + bytes(30))
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


def find_keyboard_event():
    try:
        txt = open("/proc/bus/input/devices").read()
    except OSError:
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


def read_debug(fd):
    os.write(fd, bytes([0xFE, 0x0A]) + bytes(30))
    r, _, _ = select.select([fd], [], [], 0.01)
    if not r:
        return None
    resp = os.read(fd, 32)
    if len(resp) < 6:
        return None
    return {
        "packed": resp[0],
        "stamp": resp[1],
        "tt_now": resp[2],
        "cause": resp[3],
        "dbg_pk": resp[4],
        "debounced": resp[5],
    }


def main():
    print(f"### SCRIPT_ID={SCRIPT_ID} ###")
    hdev = os.environ.get("VIAL_DEV") or find_hidraw()
    edev = os.environ.get("MT_DEV") or find_keyboard_event()
    if not hdev:
        print("RAW HID not found")
        return 1
    if not edev:
        print("Keyboard event not found")
        return 1
    print(f"HID={hdev} EVDEV={edev}")
    print("Press key1 (MT) with varying hold times. Ctrl-C to quit.")
    print(f"{'TIME':>8s}  {'SOURCE':>5s}  {'EVENT':<16s}  {'packed':>6s}  {'stamp':>5s}  {'tt':>5s}  {'cause':<16s}  {'deb':>6s}")
    hfd = os.open(hdev, os.O_RDWR)
    efd = os.open(edev, os.O_RDONLY | os.O_NONBLOCK)
    evfmt = "llHHi"
    evsize = struct.calcsize(evfmt)
    t0 = time.monotonic()
    last_debug = None
    POLL_INTERVAL = 0.005
    try:
        while True:
            r, _, _ = select.select([hfd, efd], [], [], POLL_INTERVAL)
            now = time.monotonic() - t0
            if efd in (r if isinstance(r, list) else [r] if r else []):
                try:
                    data = os.read(efd, evsize)
                except OSError:
                    data = None
                if data and len(data) >= evsize:
                    _, _, typ, code, val = struct.unpack(evfmt, data)
                    if typ == EV_KEY and val in (0, 1):
                        name = KEY_NAMES.get(code, f"CODE{code}")
                        d = read_debug(hfd)
                        cause_s = CAUSE_NAMES.get(d["cause"], f"?{d['cause']}") if d else "---"
                        ds = f"pk={d['packed']:02x} st={d['stamp']:02x} tt={d['tt_now']:02x} c={cause_s} deb={d['debounced']:02x}" if d else "no-response"
                        print(f"{now:8.3f}  EV  {name+' '+( 'down' if val else 'up'):16s}  {ds}")
                        last_debug = d
            else:
                d = read_debug(hfd)
                if d and d != last_debug:
                    cause_s = CAUSE_NAMES.get(d["cause"], f"?{d['cause']}")
                    print(f"{now:8.3f}  DBG pk={d['packed']:02x} st={d['stamp']:02x} tt={d['tt_now']:02x} c={cause_s:16s} deb={d['debounced']:02x}")
                    last_debug = d
    except KeyboardInterrupt:
        print("\ndone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
