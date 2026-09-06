#!/usr/bin/env python3
# test_vial.py - Vial protocol round-trip test (32-byte RAW HID, CH552G)
import os
import sys
import struct
import time

SCRIPT_ID = "VIAL_PROTOCOL_TEST_v2-HID_ID-fix-20260906"

def _hid_vid_pid(uevent_text):
    """Extract (vid, pid) as ints from HID_ID=bus:vid:pid in uevent text.
    Matches zero-padded and short forms (e.g. 'HID_ID=0003:00001209:00000001')."""
    import re
    m = re.search(r"HID_ID=[0-9A-Fa-f]+:([0-9A-Fa-f]+):([0-9A-Fa-f]+)", uevent_text)
    if not m:
        return None
    return (int(m.group(1), 16), int(m.group(2), 16))

def find_raw_hid(vid="1209", pid="0001"):
    """Auto-detect the Raw HID (IF1) hidraw node for VID:PID 1209:0001.

    Heuristics on sysfs (mtime, highest number) are unreliable because stale
    nodes from previous plug cycles keep matching VID/PID entries. Instead we
    PROBE each candidate: open it, send a benign GET_KEYBOARD_ID (FE 00), and
    keep only the node that actually answers with protocol version 0x06. A dead
    node cannot answer, so it is never selected.
    Returns path or None."""
    import glob, os, select
    want_vid = int(vid, 16)
    want_pid = int(pid, 16)
    candidates = []
    for p in glob.glob("/sys/class/hidraw/hidraw*/device/uevent"):
        try:
            txt = open(p).read()
        except OSError:
            continue
        ids = _hid_vid_pid(txt)
        if ids and ids == (want_vid, want_pid):
            node = "/dev/" + p.split("/")[4]
            devdir = os.path.dirname(p)
            # skip keyboard (IF0): it has an 'input' child under its device
            if os.path.isdir(os.path.join(devdir, "input")):
                continue
            candidates.append(node)
    for node in candidates:
        try:
            fd = os.open(node, os.O_RDWR)
        except OSError:
            continue
        try:
            msg = bytes([0xFE, 0x00]) + bytes(30)
            try:
                os.write(fd, msg)
            except OSError:
                os.close(fd)
                continue
            r, _, _ = select.select([fd], [], [], 0.3)
            if not r:
                os.close(fd)
                continue
            try:
                resp = os.read(fd, 32)
            except OSError:
                os.close(fd)
                continue
            if resp and resp[0] == 0x06:
                os.close(fd)
                return node
        except Exception:
            pass
        try:
            os.close(fd)
        except Exception:
            pass
    return None

DEV = os.environ.get("VIAL_DEV") or find_raw_hid() or "/dev/hidraw8"
print(f"[auto] using DEV={DEV}", file=sys.stderr)

def send_recv(fd, msg, prepend_report_id=False):
    # msg: bytes (32). hidraw write = EP2 OUT (interrupt). No report ID (descriptor has none).
    # If prepend_report_id, mimic WebHID sendReport(0, data): the host prepends a
    # 0x00 Report ID byte -> 33-byte packet. Firmware's vial_handle_cmd is expected
    # to skip a leading 0x00 when the next byte is a Vial/VIA command.
    if prepend_report_id:
        msg = b"\x00" + msg
    assert len(msg) >= 32, f"msg too short: {len(msg)}"
    n = os.write(fd, msg)
    if n != len(msg):
        raise RuntimeError(f"os.write returned {n} (expected {len(msg)})")
    # read response (IN). First IN after OUT carries the answer.
    try:
        import select
        r, _, _ = select.select([fd], [], [], 1.0)
        if not r:
            return None
    except Exception:
        time.sleep(0.01)
    resp = os.read(fd, 32)
    return resp

def hexs(b):
    return b.hex(" ") if b else "(none)"

def main():
    print(f"### SCRIPT_ID={SCRIPT_ID} ###")
    print(f"=== dev={DEV} ===")
    fd = os.open(DEV, os.O_RDWR)
    try:
        # 0) GET_DEFINITION full fetch, mimicking vial.rocks exactly:
        #    vial.rocks: sendVial(GET_DEFINITION, [...LE32(block)]) where block = page number (0,1,2,...)
        #    each response is 32 bytes; concatenate; lzma.decompress; json.loads
        import lzma
        import re as _re, os as _os
        _here = _os.path.dirname(_os.path.abspath(__file__))
        _srcdir = _os.path.dirname(_here)  # repo root: <root>/src/...
        VIAL_DEFINITION_LEN = 0
        try:
            _m = _re.search(r"#define VIAL_DEFINITION_LEN (\d+)",
                            open(_os.path.join(_srcdir, "src", "vial_definition.h")).read())
            if _m:
                VIAL_DEFINITION_LEN = int(_m.group(1))
        except OSError:
            pass
        # get size first (GET_SIZE = FE 01)
        szmsg = bytes([0xFE, 0x01]) + bytes(30)
        szresp = send_recv(fd, szmsg)
        def_size = struct.unpack("<I", szresp[0:4])[0] if szresp else 0
        if not VIAL_DEFINITION_LEN:
            VIAL_DEFINITION_LEN = def_size
        print(f"\n[0] GET_SIZE -> definition_size = {def_size}")
        print(f"    VIAL_DEFINITION_LEN (build) = {VIAL_DEFINITION_LEN}")
        # fetch all pages
        payload = bytearray()
        block = 0
        while block * 32 < def_size:
            # LE32(block) => msg[2..5]; msg[0]=FE, msg[1]=02
            cmd = bytes([0xFE, 0x02]) + bytes([block & 0xFF, (block>>8)&0xFF, (block>>16)&0xFF, (block>>24)&0xFF]) + bytes(26)
            r = send_recv(fd, cmd)
            if not r:
                print(f"    [0] GET_DEF page {block}: NO RESPONSE (timeout)")
                break
            payload.extend(r)
            block += 1
        print(f"    [0] fetched {block} pages, {len(payload)} bytes total")
        # decompress
        try:
            raw = lzma.decompress(bytes(payload[:def_size]))
            import json
            p = json.loads(raw.decode("utf-8"))
            print(f"    [0] LZMA decompress OK: name={p.get('name')}, matrix={p.get('matrix')}")
        except Exception as e:
            print(f"    [0] LZMA decompress FAILED: {e}")
            print(f"    [0] first 16 bytes of payload: {payload[:16].hex(' ')}")

        # 1) GET_KEYBOARD_ID: FE 00 + 30 zero
        msg = bytes([0xFE, 0x00]) + bytes(30)
        resp = send_recv(fd, msg)
        print(f"\n[1] GET_KEYBOARD_ID (FE 00):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            ver = struct.unpack("<I", resp[0:4])[0]
            uid = resp[4:12]
            print(f"    protocol_version = {ver:#010x} (expect 0x00000006)")
            print(f"    keyboard_uid     = {uid.hex()} (expect prefix 12090001 + per-unit tail)")
            ok = (ver == 0x00000006 and uid[:4] == bytes([0x12,0x09,0x00,0x01]))
            print(f"    => {'OK' if ok else 'MISMATCH'}")

        # 2) GET_SIZE: FE 01 + 30 zero
        msg = bytes([0xFE, 0x01]) + bytes(30)
        resp = send_recv(fd, msg)
        print(f"\n[2] GET_SIZE (FE 01):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            sz = struct.unpack("<I", resp[0:4])[0]
            print(f"    definition_size = {sz} (expect {VIAL_DEFINITION_LEN} = VIAL_DEFINITION_LEN)")
            print(f"    => {'OK' if sz == VIAL_DEFINITION_LEN else 'MISMATCH'}")

        # 3) GET_DEF page 0: FE 02 [page_lo page_hi] + 28 zero
        msg = bytes([0xFE, 0x02, 0x00, 0x00]) + bytes(28)
        resp = send_recv(fd, msg)
        print(f"\n[3] GET_DEF page0 (FE 02 0000):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            # LZMA header: 5D 00 00 00 (FORMAT_ALONE magic)
            print(f"    lzma_magic = {resp[0:4].hex()} (expect 5d000000)")
            print(f"    => {'OK (LZMA stream starts)' if resp[0] == 0x5D else 'NOT LZMA'}")

        # 4) GET_LAYER_COUNT: 11 + 31 zero
        msg = bytes([0x11]) + bytes(31)
        resp = send_recv(fd, msg)
        print(f"\n[4] GET_LAYER_COUNT (11):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            layers = resp[1]
            print(f"    layers = {layers} (expect 4)")
            print(f"    => {'OK' if layers == 4 else 'MISMATCH'}")

        # 5) GET_KEYCODE layer0 key0: 04 00 00 + 29 zero
        msg = bytes([0x04, 0x00, 0x00]) + bytes(29)
        resp = send_recv(fd, msg)
        print(f"\n[5] GET_KEYCODE layer0 key0 (04 00 00):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            kc = struct.unpack(">H", resp[0:2])[0]  # BE on wire (cf. gotcha #28)
            print(f"    keycode = {kc:#06x} (expect 0x0029 = Esc)")
            print(f"    => {'OK' if kc == 0x29 else 'MISMATCH'}")

        # 5b) GET_PROTOCOL_VERSION (01) — GUI expects data[1:3] = 00 09 (BE, VIA ver 9)
        msg = bytes([0x01]) + bytes(31)
        resp = send_recv(fd, msg)
        print(f"\n[5b] GET_PROTOCOL_VERSION (01):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            ver = (resp[1] << 8) | resp[2]
            print(f"    via_version = {ver} (expect 9)")
            print(f"    => {'OK' if ver == 9 else 'MISMATCH'}")

        # 6) SET_KEYCODE layer0 key0 = 0x0004 ('a'), then GET back.
        # VIA SET_KEYCODE wire: [0x05, layer, row, col, kc_hi, kc_lo]
        lk = send_recv(fd, bytes([0xFE, 0x05]) + bytes(30))
        locked = (lk is None) or (lk[0] == 0)
        want = 0x0004 if not locked else 0x0029
        print(f"\n[6] SET_KEYCODE layer0 key0 = 0x0004 ('a') (locked={locked}, want={want:#06x})")
        msg = bytes([0x05, 0x00, 0x00, 0x00, 0x00, 0x04]) + bytes(26)
        resp = send_recv(fd, msg)
        msg = bytes([0x04, 0x00, 0x00]) + bytes(29)
        resp = send_recv(fd, msg)
        print(f"    GET after SET: {hexs(resp)}")
        if resp and len(resp) == 32:
            kc = struct.unpack(">H", resp[0:2])[0]
            print(f"    keycode now = {kc:#06x} (expect {want:#06x})")
            print(f"    => {'OK' if kc == want else 'MISMATCH'}")
        # restore key0 to default (0x0029 = Esc)
        if not locked:
            send_recv(fd, bytes([0x05, 0x00, 0x00, 0x00, 0x00, 0x29]) + bytes(26))

        # 7) GET_UNLOCK_STATUS (FE 05) — state depends on session (boots locked, but RAM persists)
        msg = bytes([0xFE, 0x05]) + bytes(30)
        resp = send_recv(fd, msg)
        print(f"\n[7] GET_UNLOCK_STATUS (FE 05):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            unlocked = resp[0]
            print(f"    unlocked = {unlocked}")
            print(f"    => OK (state reported)")

        # 8) UNLOCK_POLL (FE 07) — reports current unlock state
        msg = bytes([0xFE, 0x07]) + bytes(30)
        resp = send_recv(fd, msg)
        print(f"\n[8] UNLOCK_POLL (FE 07):")
        print(f"    resp: {hexs(resp)}")
        if resp and len(resp) == 32:
            poll = resp[0]
            print(f"    unlocked = {poll}")
            print(f"    => OK (state reported)")

    finally:
        os.close(fd)
    print("\n=== done ===")

if __name__ == "__main__":
    main()
