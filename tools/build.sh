#!/usr/bin/env bash
# build.sh - SDCC build for CH552G macropad firmware
#
# Toolchain: SDCC 4.5.0 unpacked under ~/.local/opt/sdcc (no root needed).
# Output: build/ch552g-kbd.bin  - flash with:
#   wchisp flash build/ch552g-kbd.bin   (after entering ISP mode)
set -euo pipefail

SDCC_HOME="${SDCC_HOME-$HOME/.local/opt/sdcc}"
if [ -n "$SDCC_HOME" ]; then
  SDCC="$SDCC_HOME/usr/bin/sdcc"
  PACKIHX="$SDCC_HOME/usr/bin/packihx"
else
  SDCC="sdcc"
  PACKIHX="packihx"
fi

cd "$(dirname "$0")/.."
SRC=src
OUT=build
mkdir -p "$OUT"

if [ -n "${SDCC_HOME}" ]; then
  [ -x "$SDCC" ] || { echo "SDCC not found at $SDCC (set SDCC_HOME)"; exit 1; }
else
  command -v "$SDCC" >/dev/null || { echo "sdcc not found in PATH (apt-get install sdcc)"; exit 1; }
  command -v "$PACKIHX" >/dev/null || { echo "packihx not found in PATH (apt-get install sdcc)"; exit 1; }
fi

# -mmcs51            : 8051 core (E8051 is instruction-compatible)
# --model-small      : required for CH552G USB (Phase 1 verified)
# --no-xinit-opt     : disable xdata init optimization
# --opt-code-size    : optimize for code size
# --code-size 14336  : application flash is 14KiB (verified via wchisp info)
CFLAGS=(-mmcs51 --std=c99
        --model-small
        --no-xinit-opt
        --opt-code-size
        --code-size 14336)
# Diet measurement hook (issue #5 FLASH diet): EXTRA_CFLAGS env appends
# temporary optimizer flags without editing this file.
# shellcheck disable=SC2206
if [ -n "${EXTRA_CFLAGS:-}" ]; then CFLAGS+=($EXTRA_CFLAGS); fi

SRCS=(main usb vial dataflash tap_dance rgb)
RELS=()
for s in "${SRCS[@]}"; do
    echo "CC  $s.c"
    "$SDCC" "${CFLAGS[@]}" -c "$SRC/$s.c" -o "$OUT/$s.rel"
    RELS+=("$OUT/$s.rel")
done

echo "LINK"
"$SDCC" "${CFLAGS[@]}" "${RELS[@]}" -o "$OUT/firmware.ihx"
"$PACKIHX" "$OUT/firmware.ihx" > "$OUT/firmware.hex"

# Vector sanity: 0x0043 must be LJMP (<02> hi lo) into the ISR prologue,
# and the ISR must not be dead code.
python3 - "$OUT/firmware.hex" "$OUT/firmware.map" <<'PY'
import re, sys
data = {}
for l in open(sys.argv[1]):
    l = l.strip()
    if not l.startswith(':'): continue
    b = bytes.fromhex(l[1:]); n, addr, typ = b[0], (b[1]<<8)|b[2], b[3]
    if typ == 0:
        for i in range(n): data[addr+i] = b[4+i]
v = bytes(data.get(a,0) for a in range(0x43,0x46))
if v[0] != 0x02:
    sys.exit(f"ERROR: no LJMP at vector 0x0043 (got {v.hex(' ')})")
tgt = (v[1]<<8)|v[2]
pro = bytes(data.get(a,0) for a in range(tgt,tgt+6))
print(f"vector 0x0043 -> 0x{tgt:04X}; prologue {pro.hex(' ')} (expect push 0xC0..) OK"
      if pro[0]==0xC0 else f"WARNING: prologue at {tgt:04X}: {pro.hex(' ')}")
PY

# Intel hex -> raw bin for wchisp
python3 - "$OUT/firmware.hex" "$OUT/ch552g-kbd.bin" <<'PY'
import sys
hexas = [l.strip()[1:] for l in open(sys.argv[1]) if l.startswith(':')]
data = {}
maxaddr = 0
for h in hexas:
    b = bytes.fromhex(h)
    n, addr, typ = b[0], (b[1]<<8)|b[2], b[3]
    if typ == 0:
        for i in range(n):
            data[addr+i] = b[4+i]
        maxaddr = max(maxaddr, addr+n)
    elif typ == 1:
        break
out = bytes(data.get(a, 0) for a in range(maxaddr))
open(sys.argv[2], 'wb').write(out)
print(f"bin size: {len(out)} bytes (limit 14336)")
if len(out) > 14336:
    sys.exit("ERROR: image exceeds 14KiB application flash")
PY

echo "--- xdata placement sanity (SETUP window 0x00F0-0x00F7) ---"
python3 - "$OUT/firmware.map" <<'PY'
import re, sys
txt = open(sys.argv[1]).read()
seg = re.search(r'^\s*xseg.*?([0-9A-Fa-f]{4})', txt, re.M)
over = []
for m in re.finditer(r'^\s+([0-9A-Fa-f]{4})\s+\d+\s+\w+\s+(\S+)', txt, re.M):
    addr, name = int(m.group(1), 16), m.group(2)
    if 0x00F0 <= addr <= 0x00F7:
        over.append((hex(addr), name))
print("XSEG start:", seg.group(1) if seg else "not found")
print("vars in SETUP window:", over if over else "none")
sys.exit(1 if over else 0)
PY

echo "--- xdata scratch window 0x00F8-0x00FF (fx_base + tt_*/lt_* allowed) ---"
python3 - "$OUT/firmware.map" <<'PY'
import re, sys
txt = open(sys.argv[1]).read()
bad = []
for m in re.finditer(r'^\s+([0-9A-Fa-f]{4})\s+\d+\s+\w+\s+(\S+)', txt, re.M):
    addr, name = int(m.group(1), 16), m.group(2)
    allowed = ('fx_base' in name) or name.startswith('_tt_') or name.startswith('_lt_')
    if 0x00F8 <= addr <= 0x00FF and not allowed:
        bad.append((hex(addr), name))
print("vars in scratch window:", bad if bad else "only fx_base/tt_*/lt_*/none")
sys.exit(1 if bad else 0)
PY

# Manifest sync: keep firmware/manifest.json local-dev entry (size + sha256)
# in sync with the just-built image. Other releases are left untouched.
python3 - "$OUT/ch552g-kbd.bin" firmware/manifest.json <<'PY'
import hashlib, json, sys
bpath, mpath = sys.argv[1], sys.argv[2]
data = open(bpath, 'rb').read()
try:
    m = json.load(open(mpath))
except (OSError, ValueError):
    m = {"releases": []}
hit = [r for r in m.get("releases", []) if r.get("version") == "local-dev"]
e = hit[0] if hit else {"version": "local-dev", "file": "build/ch552g-kbd.bin"}
e.update({"file": "build/ch552g-kbd.bin", "size": len(data),
          "sha256": hashlib.sha256(data).hexdigest()})
if not hit:
    m.setdefault("releases", []).append(e)
json.dump(m, open(mpath, "w"), separators=(",", ": "))
open(mpath, "a").write("\n")
print(f"manifest: local-dev size={e['size']} sha256={e['sha256']}")
PY

echo "OK: $OUT/ch552g-kbd.bin"
