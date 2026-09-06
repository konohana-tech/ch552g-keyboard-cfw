#!/usr/bin/env python3
"""MT nest/roll timing analyzer (issue #5) — stdlib only, no args.

v3: pollution-proof episode segmentation.
  - Anchor on FIRST press of A/B/SHIFT (B-first sequences supported).
  - End after MT-up AND B-up both seen + 150ms tail.
  - MASHED verdict when a key is pressed >1 time in one episode
    (rapid mashing made v2 verdicts meaningless).
  - Captures CTRL/ALT/GUI too: a non-L0 layer fingerprint (e.g. TO latch)
    shows up instead of silently corrupting verdicts.

Runs on the Linux machine where the 1209:0001 keyboard is attached.
Usage (one-shot, no args):
    python3 tools/mt_timing_analyze.py
Override: MT_DEV=/dev/input/eventN python3 tools/mt_timing_analyze.py
"""
import os
import re
import select
import struct
import sys
import time

SCRIPT_ID = "MT_TIMING_ANALYZE_v3-20260907"

EV_KEY = 0x01
KEY_A = 30
KEY_B = 48
KEY_LEFTSHIFT = 42
KEY_LEFTCTRL = 29
KEY_LEFTALT = 56
KEY_LEFTMETA = 125
KEY_RIGHTSHIFT = 54
MT_CODES = (KEY_A, KEY_LEFTSHIFT)
MOD_CODES = (KEY_LEFTCTRL, KEY_LEFTALT, KEY_LEFTMETA, KEY_RIGHTSHIFT)
TRACKED = (KEY_A, KEY_B, KEY_LEFTSHIFT) + MOD_CODES
TAIL_AFTER_BOTH_UP_S = 0.15
EPISODE_TIMEOUT_S = 3.0
TERM_WARN_MS = 250.0
MIN_SEPARATION_MS = 15.0


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


def drain(fd):
    fmt = "llHHi"
    size = struct.calcsize(fmt)
    while True:
        r, _, _ = select.select([fd], [], [], 0)
        if not r:
            return
        try:
            os.read(fd, size)
        except OSError:
            return


def wait_key(fd, timeout_s):
    fmt = "llHHi"
    size = struct.calcsize(fmt)
    r, _, _ = select.select([fd], [], [], timeout_s)
    if not r:
        return None
    try:
        data = os.read(fd, size)
    except OSError:
        return None
    if len(data) < size:
        return None
    sec, usec, typ, code, val = struct.unpack(fmt, data)
    if typ != EV_KEY:
        return (sec + usec / 1e6, None, None)
    return (sec + usec / 1e6, code, val)


def collect_episode(fd):
    evs = []
    # idle: wait for first press of any tracked key
    while True:
        ev = wait_key(fd, 60)
        if ev is None:
            return None
        t, c, v = ev
        if c in TRACKED and v == 1:
            evs.append(ev)
            break
    mt_down = t
    b_down_seen = (c == KEY_B)
    mt_up_seen = False
    b_up_seen = False
    if c == KEY_B:
        first = "B"
    else:
        first = "MT"
    end = time.monotonic() + EPISODE_TIMEOUT_S
    tail_until = None
    while time.monotonic() < end:
        if mt_up_seen and b_up_seen:
            if tail_until is None:
                tail_until = time.monotonic() + TAIL_AFTER_BOTH_UP_S
            if time.monotonic() >= tail_until:
                break
        ev2 = wait_key(fd, max(0.01, end - time.monotonic()))
        if ev2 is None:
            continue
        t2, c2, v2 = ev2
        if c2 is None:
            continue
        if c2 not in TRACKED:
            continue
        evs.append(ev2)
        if c2 in MT_CODES and v2 == 0:
            mt_up_seen = True
        if c2 == KEY_B and v2 == 0:
            b_up_seen = True
        if c2 == KEY_B and v2 == 1:
            b_down_seen = True
    return {"evs": evs, "first": first, "mt_down": mt_down}


def analyze(ep):
    evs = ep["evs"]
    downs = {}
    ups = {}
    for t, c, v in evs:
        if v == 1:
            downs.setdefault(c, []).append(t)
        else:
            ups.setdefault(c, []).append(t)
    mods_seen = [c for c in MOD_CODES if c in downs]
    b_down = min(downs[KEY_B]) if KEY_B in downs else None
    b_up = max(ups[KEY_B]) if KEY_B in ups else None
    m_downs = [t for c in MT_CODES for t in downs.get(c, [])]
    m_ups = [t for c in MT_CODES for t in ups.get(c, [])]
    m_down = min(m_downs) if m_downs else None
    m_up = max(m_ups) if m_ups else None
    shift_seen = KEY_LEFTSHIFT in downs
    a_seen = KEY_A in downs
    mashed = (len(downs.get(KEY_B, [])) > 1 or len(m_downs) > 1)
    order = None
    if b_up is not None and m_up is not None:
        if b_down is not None and m_down is not None and b_down < m_down:
            order = "B-first-roll" if m_up < b_up else "B-first-nested"
        else:
            order = "ABBA(nested)" if b_up < m_up else "ABAB(roll)"
    ms = lambda a, b: (b - a) * 1000.0 if a is not None and b is not None else None
    return {
        "b_down": b_down, "b_up": b_up, "m_down": m_down, "m_up": m_up,
        "order": order, "shift_seen": shift_seen, "a_seen": a_seen,
        "mashed": mashed, "mods_seen": mods_seen, "first": ep["first"],
        "b_n": len(downs.get(KEY_B, [])), "m_n": len(m_downs),
        "m_span_ms": ms(m_down, m_up),
        "release_gap_ms": ms(min(b_up, m_up), max(b_up, m_up)) if b_up and m_up else None,
    }


def verdict(a):
    if a["mods_seen"]:
        return "LAYER? (CTRL/ALT/GUI seen — not L0? re-plug & retry)"
    if (a["b_n"] == 1 and a["m_n"] == 1 and not a["shift_seen"]
            and not a["mods_seen"] and a["b_up"] is not None
            and a["m_down"] is not None and a["b_up"] < a["m_down"]):
        return ("AMBIGUOUS(P4): B-then-MT-tap (firmware correct) vs "
                "MT-first-nested guard failure (firmware BUG). "
                "If fingers did MT-first, the guard failed. "
                "Redo ONE clean MT-first nest: MTdown, B tap once, MTup, hands off.")
    if a["mashed"]:
        return "MASHED (a key pressed >1x — retry ONE clean tap)"
    if a["m_down"] is None or a["m_up"] is None:
        return "NG(incomplete MT episode — retry)"
    if a["b_down"] is None or a["b_up"] is None:
        return ("NO-B-ON-HOST (host saw no B: fast B tap can fall inside "
                "MT tap-override window ~10-25ms and never reach USB — "
                "firmware-side swallow, not a light press. Retry with B "
                "held slightly longer, past the A output)")
    if a["m_span_ms"] and a["m_span_ms"] > TERM_WARN_MS:
        return f"NG(MT span {a['m_span_ms']:.0f}ms > term — retry faster, aim <200ms)"
    if a["release_gap_ms"] is not None and a["release_gap_ms"] < MIN_SEPARATION_MS:
        return (f"AMBIGUOUS: release gap {a['release_gap_ms']:.0f}ms < {MIN_SEPARATION_MS:.0f}ms "
                "(USB poll 10ms — retry with clearer separation)")
    if a["order"] is None:
        return "NG(order unclear — retry)"
    if "nested" in a["order"]:
        return "OK(hold: B expected)" if a["shift_seen"] else "NG(nested but no Shift — hold missing)"
    return "OK(tap: ab expected)" if (a["a_seen"] and not a["shift_seen"]) else "NG(roll but Shift seen — hold misfire)"


def main():
    print(f"### SCRIPT_ID={SCRIPT_ID} ###")
    dev = os.environ.get("MT_DEV") or find_keyboard_event()
    if not dev:
        print("keyboard 1209:0001 not found. Override: MT_DEV=/dev/input/eventN")
        return 1
    print(f"[auto] using DEV={dev} (override MT_DEV=... to change)")
    try:
        fd = os.open(dev, os.O_RDONLY)
    except OSError as e:
        print(f"cannot open {dev}: {e} (try sudo or input group)")
        return 1
    drain(fd)
    print("Ready. ONE clean trial at a time, pause 0.5s between trials.")
    print("NOTE: MT press is silent until decision; firstEv = first VISIBLE event only.")
    print("Ctrl-C to quit.")
    names = {KEY_A: "A", KEY_B: "B", KEY_LEFTSHIFT: "SHIFT",
             KEY_LEFTCTRL: "CTRL", KEY_LEFTALT: "ALT", KEY_LEFTMETA: "GUI",
             KEY_RIGHTSHIFT: "RSHIFT"}
    n = 0
    try:
        while True:
            ep = collect_episode(fd)
            if ep is None:
                print("(idle 60s, still waiting...)")
                continue
            n += 1
            a = analyze(ep)
            print(f"--- trial {n} (firstEv={a['first']}) ---")
            prev = None
            for t, c, v in ep["evs"]:
                if c in names:
                    if prev is None:
                        print(f"  {names[c]} {'down' if v else 'up'}")
                    else:
                        print(f"  {names[c]} {'down' if v else 'up'} (+{(t - prev) * 1000:.0f}ms)")
                    prev = t
            if a["m_span_ms"] is not None:
                print(f"  MT span {a['m_span_ms']:.0f}ms, "
                      f"release gap {(a['release_gap_ms'] or 0):.0f}ms -> {a['order']}")
            print(f"  output: Shift={'yes' if a['shift_seen'] else 'no'}, "
                  f"A={'yes' if a['a_seen'] else 'no'} -> {verdict(a)}")
    except KeyboardInterrupt:
        print(f"\ndone ({n} trials).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
