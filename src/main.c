/* main.c - CH552G macropad: 6 keys + rotary encoder (inline debounce) - fix mapping */
#include <stdint.h>
#include "ch552.h"
#include "usb.h"
#include "vial.h"
#include "dataflash.h"
#include "tap_dance.h"
#include "rgb.h"

void USBInit(void);
void USBInterrupt(void);
void USB_ISR(void) __interrupt(INT_NO_USB) { USBInterrupt(); }

#define ENC_A_PIN  (1u << 0)
#define ENC_B_PIN  (1u << 1)
#define ENC_SW_PIN (1u << 3)

#define DEBOUNCE_THRESHOLD 4 /* ~0.8ms window @12MHz poll (was 2 @6MHz) */

/* QMK layer keycodes (quantum/keycodes.h, verified 2026-09-04):
 * TO=0x5200, MO=0x5220, TG=0x5260 (+layer 0-31). TT=0x52C0 ABOLISHED.
 * KC_NO=0x0000, KC_TRNS=0x0001. */
#define QK_MO_BASE 0x5220
#define QK_MO_MASK 0xFFE0
#define QK_TG_BASE 0x5260
#define QK_TO_BASE 0x5200
#define QK_LT_BASE 0x4000
#define QK_LT_MASK 0xF000
/* Mod-Tap (QMK QK_MOD_TAP 0x2000-0x3FFF, keycodes.h 0.0.9 verified 2026-09-06;
 * high bytes 0x20-0x2F = left-hand, 0x30-0x3F = right-hand via mods bit4.
 * Only left-hand is supported here; right-hand stays silent. No overlap
 * with macro 0x7700 / TD 0x5700 / LT 0x4000. */
#define QK_MT_BASE 0x2000
#define QK_MT_MASK 0xE000
#define QK_LAYER_MASK 0xFFE0
/* LT tap window ~=196ms (3x65.5ms Timer0; QMK 200ms, ex-TT_TERM_TICKS). */
#define LT_TERM_TICKS 3
#define KC_TRNS 0x0001

static uint8_t enc_override_keycode;
static uint8_t enc_override_timer;

/* Layer state: NO free IRAM (0x1F = fx_phase) and XRAM <0x100 fully mapped
 * (XSEG auto-place lands in SIE-owned EP0_buffer 0x00-0x09 — gotcha #9:
 * build 5c35a229 bricked keys+Vial this way). active (0-3) lives in
 * key_debounced bits 7-6 (debounce uses 0-5; init 0x3F clears 7-6).
 * 2 bits < VIAL_LAYERS(4): always valid, no clamp needed. */
#define AL_GET() ((uint8_t)((key_debounced >> 6) & 0x03))

/* Plain DATA (never __at in IRAM: the linker packs overlay over __at
 * absolutes -> double-booking. It put main's delay counter high byte AND
 * vial's loop index on 0x1F/0x30 (see skill gotcha). XDATA __at is fine
 * and stays (SIE/DMA + XSEG discipline). */
static uint8_t key_debounced;
static uint8_t debounce_cnt[6];

/* Layer-tap timebase: 1B xdata @0xFB (build.sh guard-allowlisted with
 * fx_base; SIE/DMA never touch >=0xFB). tt_now: free-running 65.5ms ticks
 * (tt_tick owns TF0 clear-on-read). Ex-TT tracker (tt_stamp/tt_packed @0xFC-0xFD)
 * freed by TT abolition. LT tap state stays @0xFE-0xFF. */
__xdata __at (0x00FB) uint8_t tt_now;

/* LT tap state: 2B xdata @0xFE-0xFF (guard-allowlisted lt_*; same window
 * discipline as TT). lt_stamp: press tick of in-progress LT tap.
 * lt_packed: key[2:0] (7=none). Tap release injects via enc_override_*
 * (shared with encoder; rare overlap documented). RAM-only. */
__xdata __at (0x00FE) uint8_t lt_stamp;
__xdata __at (0x00FF) uint8_t lt_packed;
/* LT tap inject window: override must outlive the 10ms EP1 host poll.
 * Loop ~=0.2-0.5ms/poll, so 50 polls ~= 10-25ms. */
#define LT_TAP_POLLS 50

/* Layer-tap timebase: tt_tick OWNS TF0 (clear-on-read every poll).
 * Poll (~2ms) << tick (65.5ms) so at most one overflow per poll. */
__xdata __at (0x00FC) uint8_t lt_prev_tf0; /* reserved (edge detect removed) */
__xdata __at (0x00FD) uint8_t tt_tick_cnt; /* reserved (was tt_now debug counter, now unwritten) */
static void tt_tick(void) __reentrant {
  if (TF0) {
    TF0 = 0;
    tt_now++;
  }
}

/* LT layer-tap edge handler (QMK: hold=layer, tap=key within TAPPING_TERM).
 * Press records (key,tick); tap-release injects tk via enc_override_*
 * (LT_TAP_POLLS); hold-release does nothing (level resolve already applied
 * the layer while held). Encoding LT(layer,kc) = 0x4000|layer<<8|kc
 * (verified range + action.c split; GUI readback cross-checks at test).
 * k = matrix index, bl = LT layer (<4), tk = tap keycode. Single tracker
 * (shared nothing with TT); holds >16s misfire as tap on release (8-bit
 * wrap, documented, negligible). */
static void lt_edge(uint8_t k, uint8_t press, uint8_t bl, uint8_t tk) __reentrant {
  uint8_t pk = lt_packed;
  if (bl >= VIAL_LAYERS) return;
  if (press) {
    lt_stamp = tt_now;
    lt_packed = (uint8_t)(k & 0x07);
  } else {
    if ((pk & 0x07) != k) return; /* stale/untracked release */
    if (tk && (uint8_t)(tt_now - lt_stamp) <= LT_TERM_TICKS) {
      enc_override_keycode = tk;
      enc_override_timer = LT_TAP_POLLS;
    }
    lt_packed = 0x07;
  }
}

/* Mod-Tap state (issue #1): single tracker mirroring LT (independent of LT).
 * mt_stamp/mt_packed: press tick + key[2:0] (0x07 = idle, bit7 = hold past
 * term). mt_mod/mt_tk: press-time HID mod bits + tap key (layer-change safe).
 * 0x015E-0x015F free (rgb ends 0x015D, UID starts 0x0160); 0x0168+ free. */
__xdata __at (0x015E) uint8_t mt_stamp;
__xdata __at (0x015F) uint8_t mt_packed;
__xdata __at (0x0168) uint8_t mt_mod;
__xdata __at (0x0169) uint8_t mt_tk;
/* QK_MODS press-time mod slots: parallel to slot_kc, ORed into report[0]
 * while held. 0x016A-0x016F free (UID ends 0x0167). */
__xdata __at (0x016A) uint8_t slot_mod[6];
/* scan_keycode side-channel: HID mod bits of the just-decoded QK_MODS key
 * (0 when the key carries no modifier). Read immediately by the caller. */
__xdata __at (0x0170) uint8_t dec_mod;

/* Mod-Tap edge handler (issue #1; QMK MT(MOD_*,kc), left-hand only).
 * Lean single tracker mirroring lt_edge: press records (key,tick,mod,tap);
 * tap-release injects tk via enc_override_* (LT_TAP_POLLS); hold-release does
 * nothing (hold mod rides the level path mt_hold_mod while held).
 * Encoding MT = 0x2000|mods<<8|kc (mb bit0-3 = mods, bit4 = right-hand flag;
 * same byte layout as the macro EXT path). Right-hand press is ignored here
 * (stays silent). k = matrix index. Overlapping MT presses retrack (same
 * class of limitation as LT's single tracker, documented). */
static void mt_edge(uint8_t k, uint8_t press, uint8_t mb, uint8_t tk) __reentrant {
  uint8_t pk = mt_packed;
  if (press) {
    if (mb & 0x10) return; /* right-hand MT unsupported */
    mt_stamp = tt_now;
    mt_packed = (uint8_t)(k & 0x07);
    mt_mod = (uint8_t)(mb & 0x0F); /* left mods ARE the HID mod byte */
    mt_tk = tk;
  } else {
    if ((pk & 0x07) != (k & 0x07)) return; /* stale/untracked release */
    if (!(pk & 0x80) && mt_tk && (uint8_t)(tt_now - mt_stamp) <= LT_TERM_TICKS) {
      enc_override_keycode = mt_tk;
      enc_override_timer = LT_TAP_POLLS;
    }
    mt_packed = 0x07;
  }
}

/* MT hold level: call every poll, OR the result into report[0]. Past the
 * shared tap term (== LT term, same tt_now timebase) with the key still held
 * the press-time mod bits are emitted. Else 0. */
static uint8_t mt_hold_mod(void) __reentrant {
  uint8_t pk = mt_packed;
  uint8_t k, bi;
  if (pk == 0x07) return 0; /* idle */
  k = (uint8_t)(pk & 0x07);
  bi = (k < 3) ? k : (uint8_t)(k - 1);
  if (key_debounced & (uint8_t)(1u << bi)) { mt_packed = 0x07; return 0; }
  if ((uint8_t)(tt_now - mt_stamp) > LT_TERM_TICKS) {
    mt_packed = (uint8_t)(k | 0x80);
    return mt_mod;
  }
  return (pk & 0x80) ? mt_mod : 0;
}

/* Macro player state: XRAM 0x0124+ (pool is 0x0100-0x0123; SIE/DMA only
 * touch <0x0100; IRAM/overlay are full so nothing new lives there).
 * play: 0=idle, else id+1. pos: pool offset. gap: polls to wait.
 * phase: 0=parse next, 1=tap held (countdown to release).
 * kc/mod: current tap output. hk0/hk1/hk2/hm0/hm1/hm2: depth-3 DOWN slots. */
__xdata __at (0x0124) uint8_t mc_play;
__xdata __at (0x0125) uint8_t mc_pos;
__xdata __at (0x0126) uint8_t mc_gap;
__xdata __at (0x0127) uint8_t mc_phase;
__xdata __at (0x0128) uint8_t mc_kc;
__xdata __at (0x0129) uint8_t mc_mod;
__xdata __at (0x012A) uint8_t mc_hk0;
__xdata __at (0x012B) uint8_t mc_hm0;
__xdata __at (0x012C) uint8_t mc_hk1;
__xdata __at (0x012D) uint8_t mc_hm1;
__xdata __at (0x012E) uint8_t mc_hk2;
__xdata __at (0x012F) uint8_t mc_hm2;
/* Press-time binding slots: matrix k + HID byte per report slot 2..7.
 * A held position keeps its press-time keycode across layer changes (QMK-like).
 * 0x0144..0x014F free (TD entries end 0x0143, TD state starts 0x0150). */
__xdata __at (0x0144) uint8_t slot_pos[6]; /* matrix k, 0xFF = free */
__xdata __at (0x014A) uint8_t slot_kc[6];  /* press-time HID byte */
__xdata __at (0x0158) uint8_t mc_dtick; /* delay ticks remaining (65.5ms ea) */
__xdata __at (0x0159) uint8_t mc_dlast; /* last consumed tt_now tick */
__xdata __at (0x015A) uint8_t ms_wheel_rel; /* mouse wheel zero-release countdown (polls) */
__xdata __at (0x015B) uint8_t ms_buttons; /* mouse buttons held via enc-sw (bit2 = middle) */
/* Tap timing in polls (~0.2-0.5ms/poll): press must outlive the 10ms EP1
 * host poll (cf. LT_TAP_POLLS=50 ~= 10-25ms). */
#define MACRO_HOLD_POLLS 60
#define MACRO_GAP_POLLS 60

/* (Re)start macro id (0-2). Clears holds/tap: retrigger restarts cleanly. */
static void mc_start(uint8_t id) __reentrant {
  uint8_t p = 0, n = id;
  mc_hk0 = 0; mc_hm0 = 0; mc_hk1 = 0; mc_hm1 = 0; mc_hk2 = 0; mc_hm2 = 0;
  mc_kc = 0; mc_mod = 0; mc_phase = 0; mc_gap = 0;
  mc_dlast = tt_now; mc_dtick = 0;
  while (n) { /* macroN starts past N NULs (cap at pool end) */
    while (p < MACRO_POOL_LEN && macro_pool[p]) p++;
    if (p >= MACRO_POOL_LEN) break;
    p++; n--;
  }
  mc_pos = p;
  mc_play = (p < MACRO_POOL_LEN) ? (uint8_t)(id + 1) : 0;
}

/* Punctuation 0x20-0x60 (US layout): entry = HID usage, +0x80 = +shift.
 * 0x00 = compute path (digits/upper) or unmapped. 65 entries. */
__code const uint8_t mc_punct[65] = {
    0x2C,0x9E,0xB4,0xA0,0xA1,0xA2,0xA4,0x34,0xA6,0xA7,0xA5,0xAE,0x36,0x2D,0x37,0x38,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xB3,0x33,0xB6,0x2E,0xB7,0xB8,0x9F,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x2F,0x31,0x30,0xA3,0xAD,0x35
};

/* ASCII -> HID usage (alnum computed; punct via table).
 * *mp = 0x02 if left-shift needed, else 0. Returns 0 if unmappable. */
static uint8_t mc_ascii(uint8_t c, uint8_t *mp) __reentrant {
  *mp = 0;
  if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
  if (c >= 'A' && c <= 'Z') { *mp = 0x02; return (uint8_t)(0x04 + (c - 'A')); }
  if (c >= '1' && c <= '9') return (uint8_t)(0x1E + (c - '1'));
  if (c == '0') return 0x27;
  if (c == '\n') return 0x28;
  if (c == '\t') return 0x2B;
  if (c >= 0x20 && c <= 0x60) {
    uint8_t e = mc_punct[(uint8_t)(c - 0x20)];
    if (e) { if (e & 0x80) *mp = 0x02; return (uint8_t)(e & 0x7F); }
  }
  return 0;
}

/* Place a decoded (key, mod) by op: 1=tap momentary, 2=hold, else release.
 * Hold slots depth 3. */
static void mc_apply(uint8_t key, uint8_t mod, uint8_t op) __reentrant {
  if (op == 1) {
    mc_kc = key; mc_mod = mod; mc_phase = 1; mc_gap = MACRO_HOLD_POLLS;
  } else if (op == 2) {
    if (!mc_hk0 && !mc_hm0) { mc_hk0 = key; mc_hm0 = mod; }
    else if (!mc_hk1 && !mc_hm1) { mc_hk1 = key; mc_hm1 = mod; }
    else if (!mc_hk2 && !mc_hm2) { mc_hk2 = key; mc_hm2 = mod; }
  } else {
    if (mod) {
      mc_hm0 &= (uint8_t)~mod; mc_hm1 &= (uint8_t)~mod; mc_hm2 &= (uint8_t)~mod;
    } else {
      if (mc_hk0 == key) mc_hk0 = 0;
      if (mc_hk1 == key) mc_hk1 = 0;
      if (mc_hk2 == key) mc_hk2 = 0;
    }
  }
}

/* HID usage byte -> mod bit (0) or key part. Modifiers are 0xE0-0xE7. */
static void mc_emit(uint8_t kc, uint8_t down) __reentrant {
  uint8_t mod = 0, key = kc;
  if (kc >= 0xE0 && kc <= 0xE7) { mod = (uint8_t)(1u << (kc - 0xE0)); key = 0; }
  mc_apply(key, mod, down);
}

/* Advance the player one poll. Call every poll; output is read from
 * mc_kc/mc_mod/mc_h* by the report merge below. Token formats (vial-gui
 * macro_action.py v2 + vial-qmk dynamic_keymap.c, verified): 0x00 end;
 * 0x01 op args (1=tap,2=down,3=up +1 kc byte; 4=delay +2 bytes;
 * 5/6/7 EXT 16-bit QK_MODS, LE wire + decode_keycode);
 * other bytes = literal text (mc_ascii + punct table). */
static void macro_poll(void) __reentrant {
  uint8_t b;
  if (!mc_play) return;
  if (mc_gap) { mc_gap--; return; }
  if (mc_dtick) { /* delay in progress: consume one Timer0 tick at a time.
                   * Snap dlast (wrap-safe); dataflash_save may skip a tick
                   * under EA=0, shortening long delays slightly. */
    if (tt_now != mc_dlast) { mc_dlast = tt_now; mc_dtick--; }
    return;
  }
  if (mc_phase) { /* tap held long enough: release, inter-key gap */
    mc_kc = 0; mc_mod = 0; mc_phase = 0; mc_gap = MACRO_GAP_POLLS; return;
  }
  if (mc_pos >= MACRO_POOL_LEN) { mc_play = 0; return; }
  b = macro_pool[mc_pos];
  if (!b) { mc_play = 0; mc_kc = 0; mc_mod = 0; return; }
  if (b == 1) {
    uint8_t op, kc;
    if (mc_pos + 2 >= MACRO_POOL_LEN) { mc_play = 0; return; }
    op = macro_pool[mc_pos + 1]; kc = macro_pool[mc_pos + 2];
    if (op == 4) { /* delay ms=(d1-1)+(d2-1)*255 (GUI packs +1 each).
                     * Tick-based (~65.5ms): ticks=(d2-1)*4+((d1-1)>>6),
                     * ~= ms/64 (2% fast), cap 255 (~16s), min 1.
                     * (d1==0 unreachable: GUI min 1.) */
      uint8_t d2, t;
      if (mc_pos + 3 >= MACRO_POOL_LEN) { mc_play = 0; return; }
      d2 = macro_pool[mc_pos + 3];
      mc_pos += 4;
      if (d2 >= 65) t = 255;
      else {
        t = (uint8_t)((d2 - 1) * 4 + ((kc - 1) >> 6));
        if (!t) t = 1;
      }
      mc_dtick = t; mc_dlast = tt_now;
      return;
    }
    if (op >= 5) { /* EXT 16-bit, LE wire (vial-qmk memcpy + decode_keycode).
                     * Byte-wise exact equivalent: high byte b1 is the mods
                     * byte (0xFF remap reads mods from b0, key 0); b1==0 or
                     * >=0x20 is out of QK_MODS range, same as the uint16
                     * <0x0100 / >=0x2000 test. No 16-bit assemble. */
      uint8_t b0, b1, key, mod, mb;
      if (mc_pos + 3 >= MACRO_POOL_LEN) { mc_play = 0; return; }
      b0 = macro_pool[mc_pos + 2]; b1 = macro_pool[mc_pos + 3];
      mc_pos += 4;
      if (b1 == 0 && b0 >= 0xA8 && b0 <= 0xAA) {
        mc_apply((uint8_t)(0x7F + (b0 - 0xA8)), 0, (uint8_t)(op - 4)); /* 5/6/7 -> tap/down/up */
        return;
      }
      if (b1 == 0xFF) { if (!b0 || b0 >= 0x20) return; mb = b0; key = 0; }
      else { if (!b1 || b1 >= 0x20) return; mb = b1; key = b0; }
      mod = 0; /* QMK mods: bit8-11 left, +0x1000 right */
      if (mb & 0x01) mod |= (mb & 0x10) ? 0x10 : 0x01;
      if (mb & 0x02) mod |= (mb & 0x10) ? 0x20 : 0x02;
      if (mb & 0x04) mod |= (mb & 0x10) ? 0x40 : 0x04;
      if (mb & 0x08) mod |= (mb & 0x10) ? 0x80 : 0x08;
      mc_apply(key, mod, (uint8_t)(op - 4)); /* 5/6/7 -> tap/down/up */
      return;
    }
    if (op > 3 || !kc) { mc_pos += 3; return; } /* malformed: skip */
    mc_emit(kc, op);
    mc_pos += 3; return;
  }
  { /* literal text */
    uint8_t mod = 0;
    uint8_t kc = mc_ascii(b, &mod);
    mc_pos++;
    if (!kc) return; /* unmappable char: skip silently, no gap */
    mc_kc = kc; mc_mod = mod; mc_phase = 1; mc_gap = MACRO_HOLD_POLLS;
  }
}
 /* Debounce + TG/TO toggle (moved out of main: overlay, gotcha #39).
 * Layer latch bits live in debounce_cnt[0..2] bit7 (counters only count 0..4,
 * so bit7 is free) — every counter reset MUST preserve it (&= 0x80) and the
 * threshold compare MUST mask (& 0x7F). Toggle fires once per debounced
 * PRESS (release never toggles) and follows the same effective-binding rule
 * as scan_keycode (active layer, TRNS->base), so upper-layer shadowing works
 * QMK-faithful. TG flips its bit; TO(n) clears all latch bits then sets n
 * (TO(0) = back to base). TG on the encoder switch is unsupported (no
 * debounce/edge infra there); TG(0) is ignored. Latch is RAM-only. */
static void debounce_update(void) __reentrant {
  uint8_t raw = 0;
  uint8_t p1 = P1, p3 = P3;
  uint8_t i;
  if (p1 & (1u<<1)) raw |= (1u<<0);
  if (p1 & (1u<<7)) raw |= (1u<<1);
  if (p1 & (1u<<6)) raw |= (1u<<2);
  if (p1 & (1u<<5)) raw |= (1u<<3);
  if (p1 & (1u<<4)) raw |= (1u<<4);
  if (p3 & (1u<<2)) raw |= (1u<<5);
  for (i = 0; i < 6; i++) {
    uint8_t mask = (uint8_t)(1u<<i);
    if ((raw & mask) == (key_debounced & mask)) {
      debounce_cnt[i] &= 0x80;
    } else {
      if ((debounce_cnt[i] & 0x7F) < DEBOUNCE_THRESHOLD) debounce_cnt[i]++;
      else {
        uint8_t press = (raw & mask) ? 0 : 1;
        key_debounced ^= mask;
        debounce_cnt[i] &= 0x80;
        { /* layer-action edge. PRESS sees pre-press AL (correct press-time
           * binding). RELEASE sees stale-held AL (resolve hasn't reacted to
           * this release yet), so TT/LT prefer the effective binding
           * (shadow press) with L0 fallback (base press). TG/TO fire on
           * PRESS only. */
          uint8_t k = (i < 3) ? i : (uint8_t)(i + 1);
          uint8_t alx = AL_GET();
          uint16_t bkc = alx ? keymap[alx][k] : keymap[0][k];
          if (bkc == KC_TRNS) bkc = keymap[0][k];
          /* TO(n) is press-only: gate the branch on press so a RELEASE seen
           * through a TO binding (e.g. LT on L0, TO on L1: release resolves
           * with the stale-held layer) falls through to the L0 fallback
           * below instead of being swallowed. */
          if (press && (bkc & QK_LAYER_MASK) == QK_TO_BASE) {
            /* TO(n): exclusive — clear all latch bits, then set n.
             * TO(0) clears back to base (QMK layer_move; resolve reads the
             * same latch bits, no change needed there). */
            uint8_t bl = (uint8_t)(bkc & 0x1F);
            if (press && bl < VIAL_LAYERS) {
              debounce_cnt[0] &= 0x7F;
              debounce_cnt[1] &= 0x7F;
              debounce_cnt[2] &= 0x7F;
              if (bl >= 1) debounce_cnt[bl - 1] |= 0x80;
            }
          } else if ((bkc & QK_LT_MASK) == QK_LT_BASE) {
            uint8_t ll = (uint8_t)((bkc >> 8) & 0x0F);
            uint8_t tk = (uint8_t)(bkc & 0xFF);
            lt_edge(k, press, ll, tk);
          } else if ((bkc & QK_MT_MASK) == QK_MT_BASE) {
            mt_edge(k, press, (uint8_t)(bkc >> 8), (uint8_t)(bkc & 0xFF));
          } else if (press && td_is_td_key(bkc)) {
            td_press(td_get_index(bkc));
          } else if (!press && td_is_td_key(bkc)) {
            td_release(td_get_index(bkc));
          } else if (press && (bkc & QK_MACRO_MASK) == QK_MACRO_BASE &&
                     (bkc & 0x7F) < MACRO_COUNT) {
            /* Macro key: fire once per debounced PRESS (release ignored).
             * scan_keycode already silences it (>0x00FF -> 0). */
            mc_start((uint8_t)(bkc & 0x7F));
          } else if (!press) {
            /* release with non-action effective binding: L0 press-time
             * fallback (AL is stale-held here, so a release seen through
             * an upper-layer TO/MO/plain binding still finds the L0 LT/TD). */
            uint16_t b0 = keymap[0][k];
            if (b0 != KC_TRNS) {
              if ((b0 & QK_LT_MASK) == QK_LT_BASE) {
                lt_edge(k, 0, (uint8_t)((b0 >> 8) & 0x0F), (uint8_t)(b0 & 0xFF));
              } else if ((b0 & QK_MT_MASK) == QK_MT_BASE) {
                mt_edge(k, 0, (uint8_t)(b0 >> 8), (uint8_t)(b0 & 0xFF));
              } else if (td_is_td_key(b0)) {
                td_release(td_get_index(b0));
              }
            }
          }
        }
      }
    }
  }
}

/* Layer helpers (MO/TG/TT/TO): split from main's poll loop (overlay-frugal,
 * gotcha #39). resolve reads P3 directly so it takes no params;
 * scan returns the emittable byte (0 = nothing) with TRNS->base inside. */
/* NOTE: __reentrant (stack frames, NOT overlay): IRAM overlay pool is full
 * (11B, fragmented by absolute placements) so plain helpers overflow OSEG
 * at link. Stack (SSEG from 0x7F, ~129B free) has ample room. Called only
 * from main's poll loop; ISR never calls them. */
static uint8_t layer_resolve(void) __reentrant {
  uint8_t al = 0;
  uint8_t j;
  uint16_t bkc;
  for (j = 0; j < 6; j++) {
    uint8_t k;
    if (key_debounced & (1u << j)) continue;
    k = (j < 3) ? j : (uint8_t)(j + 1);
    bkc = keymap[0][k];
    /* MO held = momentary layer (QMK press behavior). */
    if ((bkc & QK_MO_MASK) == QK_MO_BASE) {
      uint8_t bl = (uint8_t)(bkc & 0x1F);
      if (bl < VIAL_LAYERS && bl > al) al = bl;
    } else if ((bkc & QK_LT_MASK) == QK_LT_BASE) {
      /* LT held likewise (layer in bits 11:8). */
      uint8_t ll = (uint8_t)((bkc >> 8) & 0x0F);
      if (ll < VIAL_LAYERS && ll > al) al = ll;
    }
  }
  if (!(P3 & ENC_SW_PIN)) {
    bkc = keymap[AL_GET()][7];
    if (bkc == KC_TRNS) bkc = keymap[0][7];
    if ((bkc & QK_MO_MASK) == QK_MO_BASE) { /* switch MO handled by resolve */
      uint8_t bl = (uint8_t)(bkc & 0x1F);
      if (bl < VIAL_LAYERS && bl > al) al = bl;
    } else if ((bkc & QK_LT_MASK) == QK_LT_BASE) { /* switch LT: hold-only */
      uint8_t ll = (uint8_t)((bkc >> 8) & 0x0F);
      if (ll < VIAL_LAYERS && ll > al) al = ll;
    }
  }
  /* TG/TO latch (debounce_cnt[0..2] bit7 = layers 1..3; TO writes the same
   * bits exclusively): highest latched wins together with the MO max. */
  {
    uint8_t t = 3;
    while (1) {
      if (debounce_cnt[t - 1] & 0x80) { if (t > al) al = t; break; }
      if (t == 1) break;
      t--;
    }
  }
  /* Tap Dance hold layer (momentary while TD held). */
  if (td_hold_layer && td_hold_layer < VIAL_LAYERS && td_hold_layer > al) al = td_hold_layer;
  key_debounced = (uint8_t)((key_debounced & 0x3F) | (al << 6));
  return al;
}

/* Consumer audio compat (Vial Mute/VolUp/VolDown = 0xA8-0xAA): this FW has no
 * consumer interface, so translate to keyboard-page equivalents
 * (KB_MUTE 0x7F / KB_VOL_UP 0x80 / KB_VOL_DOWN 0x81). All other consumer /
 * system / mouse codes have no keyboard equivalent and stay unsupported. */
static uint8_t kb_compat(uint8_t kc) __reentrant {
  if(kc>=0xA8 && kc<=0xAA) return (uint8_t)(0x7F + (kc - 0xA8));
  return kc;
}

static uint8_t scan_keycode(uint8_t al, uint8_t k) __reentrant {
  uint16_t bkc;
  dec_mod = 0;
  /* L0 layer-action keys (MO/TO/LT) are consumed as switches: their own
   * position never emits (QMK press-time binding; upper-layer shadowing of
   * a base switch stays silent by design). >0x00FF filter covers the rest. */
  if (al) {
    uint16_t b0 = keymap[0][k];
    uint16_t g = (uint16_t)(b0 & QK_LAYER_MASK);
    if (g == QK_MO_BASE || g == QK_TO_BASE ||
        (b0 & QK_LT_MASK) == QK_LT_BASE ||
        td_is_td_key(b0))
      return 0;
  }
  bkc = keymap[al][k];
  if (bkc == KC_TRNS) bkc = keymap[0][k]; /* TRNS -> base */
  if (bkc == 0) return 0; /* NO */
  if (bkc <= 0x00FF) return kb_compat((uint8_t)bkc);
  { /* QK_MODS left (0x0100-0x0FFF): the high-byte low nibble IS the HID mod
     * byte (same mapping as the macro EXT path); right-hand (0x10) stays
     * unsupported. Tap part rides the press-time slots via dec_mod. */
    uint8_t b1 = (uint8_t)(bkc >> 8);
    if (b1 >= 0x01 && b1 <= 0x1F && !(b1 & 0x10)) {
      dec_mod = (uint8_t)(b1 & 0x0F);
      return kb_compat((uint8_t)(bkc & 0xFF));
    }
  }
  return 0; /* MT / action / TD (0x5700+) / macro (0x7700+) */
}

void main(void) {
  uint8_t report[8];
  uint8_t p1, p3;
  static uint8_t prev_report[8]; /* zero-cleared by CRT; compared before first send */
  uint8_t i, changed;
  uint8_t kc;

  SAFE_MOD = 0x55; SAFE_MOD = 0xAA;
  CLOCK_CFG = CLOCK_CFG & ~0x07 | 0x04;
  SAFE_MOD = 0x00;
  TMOD &= 0xF0; TMOD |= 0x01; /* Timer0 mode 1 (16-bit free-run) */
  TH0 = 0; TL0 = 0; TF0 = 0; TR0 = 1; /* overflow 65.5ms @Fsys/12=1MHz */
  { unsigned int d = 5000; while(d--); }

  P3_DIR_PU &= ~(1u << 6);

  P1_MOD_OC &= 0xF1;
  P1_DIR_PU &= 0xF1;
  P3_MOD_OC &= 0xF0;
  P3_MOD_OC |= (1u << 4); /* P3.4 push-pull output: WS2812B DIN */
  P34 = 0; /* DIN low from the start: chain held in reset during ramp */
  P3_DIR_PU &= 0xF0;
  P1_DIR_PU |= 0xBE;
  P3_DIR_PU |= (1u<<2);
  P3_MOD_OC &= ~(ENC_A_PIN | ENC_B_PIN | ENC_SW_PIN);
  P3_DIR_PU &= ~(ENC_A_PIN | ENC_B_PIN | ENC_SW_PIN);

  /* Key-only ISP entry (hold-then-plug only): if KEY1+KEY3+KEY4+KEY6
   * (P1.1+P1.6+P1.5, P3.2) are all held at boot, jump straight to the
   * bootloader WITHOUT enumerating. GPIO is input-configured above, so this
   * read is deterministic. usb_isp_jump()'s detach+wait is harmless here
   * (never attached) and keeps a single jump path. plug-then-hold is
   * intentionally NOT supported (no runtime check -> zero typing interference). */
  if (((P1 & 0x62) == 0) && ((P3 & 0x04) == 0)) usb_isp_jump(); /* never returns */
  /* Factory reset (hold-then-plug only): KEY1(P1.1)+KEY3(P1.6)+KEY5(P1.4)+ENC_SW(P3.3)
   * held at boot erases DataFlash, so vial_init falls back to defaults.
   * Distinct from the ISP combo above (checked first; all-held goes to ISP). */
  if (((P1 & 0x52) == 0) && ((P3 & 0x08) == 0)) dataflash_erase_all();

  enc_override_keycode = 0;
  enc_override_timer = 0;
  key_debounced = 0x3F;
  for(i=0;i<6;i++) debounce_cnt[i]=0;
  tt_now = 0; lt_prev_tf0 = 0; /* timebase idle (ex-TT trackers freed) */
  lt_stamp = 0; lt_packed = 0x07; /* LT tracker idle */
  mt_stamp = 0; mt_packed = 0x07; mt_mod = 0; mt_tk = 0; dec_mod = 0; /* MT/MODS idle */
  mc_play = 0; mc_pos = 0; mc_gap = 0; mc_phase = 0; /* macro player idle */
  ms_wheel_rel = 0; /* mouse wheel idle */
  ms_buttons = 0; /* mouse buttons released */
  mc_kc = 0; mc_mod = 0; mc_dlast = tt_now; mc_dtick = 0;
  mc_hk0 = 0; mc_hm0 = 0; mc_hk1 = 0; mc_hm1 = 0; mc_hk2 = 0; mc_hm2 = 0;
  for (i = 0; i < 6; i++) { slot_pos[i] = 0xFF; slot_kc[i] = 0; slot_mod[i] = 0; } /* press-time slots idle */

  rgb_set_defaults(); /* overwritten by DataFlash LED region if valid */
  vial_init();
  USBInit();

  for (;;) {
    p1 = P1;
    p3 = P3;

    if (!vial_unlocked) {
      uint8_t all_pressed = 1;
      if (p1 & (1u<<1)) all_pressed = 0;
      if (p1 & (1u<<7)) all_pressed = 0;
      if (p1 & (1u<<6)) all_pressed = 0;
      if (p1 & (1u<<5)) all_pressed = 0;
      if (p1 & (1u<<4)) all_pressed = 0;
      if (p3 & (1u<<2)) all_pressed = 0;
      if (p3 & (1u<<3)) all_pressed = 0;
      if (all_pressed) {
        vial_unlocked = 1;
        vial_unlock_in_progress = 0;
        vial_unlock_counter = 0;
      }
    }

    tt_tick(); /* TT timebase (TF0 edge, read-only) */
    debounce_update(); /* debounce + TG/TO toggle + TT edges (reentrant) */

    /* Layer resolve + press-time emit. Held slots keep their press-time
     * keycode across layer changes (single input per press); fresh presses
     * resolve against the current layer. Encoder switch stays level-based. */
    {
      uint8_t s;
      layer_resolve(); /* active -> key_debounced[7:6]; read back via AL_GET (no local) */
      report[0] = 0; report[1] = 0;
      for (s = 0; s < 6; s++) {
        uint8_t k = slot_pos[s];
        if (k == 0xFF) { report[2+s] = 0; continue; }
        uint8_t bi = (k < 3) ? k : (uint8_t)(k - 1);
        if (key_debounced & (uint8_t)(1u << bi)) { slot_pos[s] = 0xFF; slot_kc[s] = 0; slot_mod[s] = 0; report[2+s] = 0; }
        else { report[2+s] = slot_kc[s]; report[0] |= slot_mod[s]; }
      }
      for (i = 0; i < 6; i++) {
        uint8_t k;
        if (key_debounced & (1u << i)) continue;
        k = (i < 3) ? i : (uint8_t)(i + 1);
        for (s = 0; s < 6; s++) if (slot_pos[s] == k) break;
        if (s < 6) continue;
        kc = scan_keycode(AL_GET(), k);
        if (!kc && !dec_mod) continue;
        for (s = 0; s < 6; s++) if (slot_pos[s] == 0xFF) { slot_pos[s] = k; slot_kc[s] = kc; slot_mod[s] = dec_mod; report[2+s] = kc; report[0] |= dec_mod; break; }
      }
      report[0] |= mt_hold_mod(); /* MT hold level (issue #1; 0 unless holding past term) */
      if (!(p3 & ENC_SW_PIN)) {
        uint16_t swb = keymap[AL_GET()][7];
        if (swb == KC_TRNS) swb = keymap[0][7];
        /* QMK mouse buttons (MS_BTN1-5 0xD1-0xD5, bit = code-0xD1;
         * middle = MS_BTN3 0x00D3 -> bit2): level-held bitmap. */
        if (swb >= 0xD1 && swb <= 0xD5) {
          uint8_t want = (uint8_t)(1u << (swb - 0xD1));
          if (want != ms_buttons) { ms_buttons = want; usb_send_mouse(0, 0, want, (int8_t)0); }
        } else {
          kc = scan_keycode(AL_GET(), 7);
          if (dec_mod) report[0] |= dec_mod; /* enc-sw QK_MODS level (MT silent here: no edge infra) */
          if (kc) { for (i = 2; i < 8; i++) if (report[i] == 0) { report[i] = kc; break; } }
          if (ms_buttons) { ms_buttons = 0; usb_send_mouse(0, 0, 0, (int8_t)0); }
        }
      } else if (ms_buttons) { ms_buttons = 0; usb_send_mouse(0, 0, 0, (int8_t)0); }
    }

    if (enc_override_timer && enc_override_keycode) report[2] = enc_override_keycode;

    td_task();
    {
        uint8_t tdk = kb_compat(td_pending_key());
        /* TD hold/tap modifier (0xE0-0xE7): OR into report[0] so chords
         * (e.g. TD-hold LCtrl + 'u') work; report[2] is left intact.
         * Plain keys keep the existing report[2] override. */
        if(tdk >= 0xE0 && tdk <= 0xE7) report[0] |= (uint8_t)(1u << (tdk - 0xE0));
        else if(tdk) report[2] = tdk;
    }

    macro_poll(); /* advance macro player (stack frame, no overlay) */
    if (mc_play || mc_kc || mc_mod || mc_hk0 || mc_hm0 || mc_hk1 || mc_hm1 || mc_hk2 || mc_hm2) {
      /* Macro owns the keys section while active (QMK: macro takes over).
       * Tap/holds merge here; scan keys are suppressed for clean output. */
      report[0] |= (uint8_t)(mc_mod | mc_hm0 | mc_hm1 | mc_hm2);
      report[2] = mc_kc;
      report[3] = mc_hk0;
      report[4] = mc_hk1;
      report[5] = mc_hk2;
      report[6] = 0; report[7] = 0;
    }

    changed=0; for(i=0;i<8;i++) if(report[i]!=prev_report[i]){changed=1;break;}
    if(changed){for(i=0;i<8;i++) prev_report[i]=report[i]; usb_send_report(report);}

    if(enc_override_timer){enc_override_timer--; if(enc_override_timer==0) enc_override_keycode=0;}
    if(ms_wheel_rel){ ms_wheel_rel--; if(ms_wheel_rel==0) usb_send_mouse(0,0,ms_buttons,(int8_t)0); }

    {
      uint8_t a = (p3 & ENC_A_PIN) ? 1 : 0;
      uint8_t b = (p3 & ENC_B_PIN) ? 1 : 0;
      uint8_t curr = (a<<1)|b;
      static uint8_t last_state=0; static int8_t accum=0; static uint8_t init_done=0;
      if(!init_done){last_state=curr; init_done=1;}
      if(curr!=last_state){
        static const int8_t enc_table[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
        uint8_t idx=(last_state<<2)|curr; accum+=enc_table[idx]; last_state=curr;
        if(accum>=4 || accum<=-4){
          uint16_t em = encoder_map[AL_GET()][(accum>=4)?(uint8_t)0:(uint8_t)1];
          /* QMK mouse wheel (MS_WHLU 0x00D9 / MS_WHLD 0x00DA): mouse report
           * + zero-release via ms_wheel_rel. Else legacy keyboard override. */
          if(em==0x00D9){ usb_send_mouse(0,0,ms_buttons,(int8_t)1); ms_wheel_rel=10; }
          else if(em==0x00DA){ usb_send_mouse(0,0,ms_buttons,(int8_t)-1); ms_wheel_rel=10; }
          else { enc_override_keycode=kb_compat((uint8_t)em); enc_override_timer=10; }
          if(accum>=4) accum-=4; else accum+=4;
        }
      }
    }
    {volatile unsigned int d; for(d=0;d<220;d++);}
    dataflash_poll();
    rgb_poll();
  }
}