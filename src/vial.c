/* vial.c - Vial RAW HID protocol for CH552G (subset, RAM keymap, DataFlash persist) */
#include <string.h>
#include <stdint.h>
#include "ch552.h"
#include "config.h"
#include "vial.h"
#include "vial_definition.h"
#include "dataflash.h"
#include "usb.h"
#include "tap_dance.h"
#include "rgb.h"

/* Keymap: layer x key -> 16-bit keycode. RAM only (persist is a later phase).
 * Placed at 0x0098: must NOT overlap EP2_buffer (__at 0x0016, size 130 -> ends 0x0097). */
__xdata __at (0x0098) uint16_t keymap[VIAL_LAYERS][NUM_KEYS];

/* Encoder: layer x {0=CW,1=CCW} -> keycode. Placed after keymap (0x0098 + 48 = 0x00D8). */
__xdata __at (0x00D8) uint16_t encoder_map[VIAL_LAYERS][2];

/* Macro pool: 3 NUL-separated slots, 36B at XRAM 0x0100 (RAM mirror;
 * DataFlash 89..104 holds 16B persistent copy). Zero = empty macros. */
__xdata __at (MACRO_POOL_XRAM) uint8_t macro_pool[MACRO_POOL_LEN];

__data uint8_t vial_unlocked;
__data uint8_t vial_unlock_in_progress;
__data uint8_t vial_unlock_counter;

/* LT-clock state lives in main.c (tt_now/lt_stamp/lt_packed drive the LT
 * tap window); no debug exposure in production. */

/* Per-unit UID (A-plan): tail derived from the factory chip ID in the
 * config area (WCH "unique ID"; addresses per WCH CH554.H Special Program
 * Space via SoCXin/CH552 mirror: ROM_CHIP_ID_HX 0x3FFA (low byte valid),
 * ROM_CHIP_ID_LO 0x3FFC (word), ROM_CHIP_ID_HI 0x3FFE (word)),
 * read at boot via MOVC (__code pointer). No DataFlash needed, so a
 * factory reset never changes the UID and all units share one .bin.
 * Vial UID = {0x12,0x09,0x00,0x01, r0..r3}, r folded from all 5 ID bytes.
 * Blank ID area (all-00/all-FF: unprogrammed or unreadable) falls back to
 * the legacy fixed tail in VIAL_KEYBOARD_UID (units then collide).
 * XRAM 0x0160: free (placements end at 0x015D rgb_sat; verified 2026-09-06). */
__xdata __at (0x0160) uint8_t vial_uid_ram[8];

static __code uint8_t dflt_uid[8] = VIAL_KEYBOARD_UID;

static void uid_init(void) {
    __code uint8_t *id = (__code uint8_t *)0x3FFA;
    uint8_t hx = id[0];
    uint8_t a = id[2], b = id[3], c = id[4], d = id[5];
    uint8_t r0, r1, r2, r3, k;
    for (k = 0; k < 4; k++) vial_uid_ram[k] = dflt_uid[k];
    if (((hx|a|b|c|d) == 0) || ((hx&a&b&c&d) == 0xFF)) {
        for (k = 0; k < 4; k++) vial_uid_ram[4+k] = dflt_uid[4+k];
    } else {
        r0 = hx ^ a; r1 = a ^ b; r2 = b ^ c; r3 = c ^ d;
        if (((r0|r1|r2|r3) == 0) || ((r0&r1&r2&r3) == 0xFF)) r3 ^= 0x5A;
        vial_uid_ram[4] = r0; vial_uid_ram[5] = r1;
        vial_uid_ram[6] = r2; vial_uid_ram[7] = r3;
    }
}

/* Factory defaults live in CODE flash; vial_init copies them via loops.
 * (Was: one unrolled __xdata 16-bit store per cell — each emits a full
 * DPTR setup. Table + loop emits the same bytes for far less CODE.) */
static __code uint16_t dflt_l0[NUM_KEYS] = { 0x29, 0x7700, 0x7701, 0, 0x7702, 0x5221, 0x28, 0x7F };
static __code uint16_t dflt_l1[NUM_KEYS] = { 0x52, 0x50, 0x4F, 0, 0x51, 0x4B, 0x4E, 0x00D3 };
static __code uint16_t dflt_enc[VIAL_LAYERS][2] = {
    { 0x00D9, 0x00DA }, { 0x004B, 0x004E }, { 0x0080, 0x0081 }, { 0x00D9, 0x00DA }
};
/* Macros: M0 Ctrl+C, M1 Ctrl+V, M2 Ctrl+Z (EXT QK_MODS tap, 5B each + NUL). */
static __code uint8_t dflt_macro[15] = {
    0x01, 0x05, 0x06, 0x01, 0x00,
    0x01, 0x05, 0x19, 0x01, 0x00,
    0x01, 0x05, 0x1D, 0x01, 0x00
};

void vial_init(void) {
    uint8_t k;
    vial_unlocked = 0;
    vial_unlock_in_progress = 0;
    vial_unlock_counter = VIAL_UNLOCK_COUNTER_MAX;
    /* L0 base: Esc / M0 copy / M1 paste / - / M2 undo / MO(1) / Enter / Mute(enc-sw) */
    /* L1 nav: Up / Left / Right / - / Down / PgUp / PgDn / MS_BTN3 middle(enc-sw) */
    /* L2/L3: all TRNS (base fallback). Encoders: L0 wheel; L1 PgUp/PgDn; L2 VolUp/VolDn; L3 wheel */
    for (k = 0; k < NUM_KEYS; k++) {
        keymap[0][k] = dflt_l0[k];
        keymap[1][k] = dflt_l1[k];
        keymap[2][k] = 0x0001;
        keymap[3][k] = 0x0001;
    }
    for (k = 0; k < VIAL_LAYERS; k++) {
        encoder_map[k][0] = dflt_enc[k][0];
        encoder_map[k][1] = dflt_enc[k][1];
    }
    for (k = 0; k < 15; k++) macro_pool[k] = dflt_macro[k];
    for (; k < MACRO_POOL_LEN; k++) macro_pool[k] = 0;
    uid_init(); /* per-unit UID must precede any Vial traffic */
    td_init();
    /* Try to load persisted keymap; if valid, it overwrites defaults */
    dataflash_init();
    dataflash_load();
}

/* Vial fat-case helpers: vial_handle_cmd's overlay frame (~50B) starves IRAM
 * (linker double-books __at absolutes when pressured -> frozen effects).
 * Split the three fattest cases into __reentrant (stack-frame) helpers with
 * the bodies moved VERBATIM (same params, same globals, same behavior). */

/* FE02 GET_DEFINITION: LE32 page -> 32B chunk (vial.rocks framing). */
static void vh_get_def(__xdata uint8_t *msg, uint8_t length) __reentrant {
    uint32_t block = ((uint32_t)msg[2]) | ((uint32_t)msg[3] << 8) | ((uint32_t)msg[4] << 16) | ((uint32_t)msg[5] << 24);
    uint32_t start = block * VIAL_RAW_EPSIZE;
    uint32_t end = start + VIAL_RAW_EPSIZE;
    memset(msg, 0, length);
    if (start >= VIAL_DEFINITION_LEN)
        return;  /* out of range: return zeros */
    if (end > VIAL_DEFINITION_LEN)
        end = VIAL_DEFINITION_LEN;
    memcpy(msg, &keyboard_definition[start], end - start);
}

/* VIA 0x12 GET_BUFFER: BE offset (WebHID) or padded-LE (hidapi) framing. */
static void vh_get_buf(__xdata uint8_t *msg, uint8_t length) __reentrant {
    uint16_t offset_be = ((uint16_t)msg[1] << 8) | msg[2];
    uint16_t sz_be = msg[3];
    uint16_t offset_le = ((uint16_t)msg[3] << 8) | msg[2];
    uint16_t sz_le = msg[4];
    uint16_t offset, sz;
    if (sz_be == 0 && sz_le != 0) { offset = offset_le; sz = sz_le; }
    else { offset = offset_be; sz = sz_be; }
    if (sz > VIAL_RAW_EPSIZE - 4) sz = VIAL_RAW_EPSIZE - 4;
    {
        uint8_t i, base;
        uint16_t total = VIAL_LAYERS * NUM_KEYS * 2;
        memset(msg, 0, length);
        /* offset>=total reads zeros; else base<total, base+i<92 (no wrap). */
        if (offset >= total) return;
        base = (uint8_t)offset;
        for (i = 0; i < sz; i++) {
            uint8_t addr = (uint8_t)(base + i);
            if (addr < total) {
                uint16_t entry = addr / 2;
                uint8_t hi = addr & 1;
                uint16_t kc = keymap[entry / NUM_KEYS][entry % NUM_KEYS];
                /* Vial expects big-endian on wire: high byte first */
                msg[4 + i] = hi ? (kc & 0xFF) : (kc >> 8);
            }
        }
    }
}

/* VIA 0x13 SET_BUFFER: LOCK-gated by caller. Same dual framing. */
static void vh_set_buf(__xdata uint8_t *msg, uint8_t length) __reentrant {
    uint16_t offset_be = ((uint16_t)msg[1] << 8) | msg[2];
    uint16_t sz_be = msg[3];
    uint16_t offset_le = ((uint16_t)msg[3] << 8) | msg[2];
    uint16_t sz_le = msg[4];
    uint16_t offset, sz;
    (void)length;
    if (sz_be == 0 && sz_le != 0) { offset = offset_le; sz = sz_le; }
    else { offset = offset_be; sz = sz_be; }
    if (sz > VIAL_RAW_EPSIZE - 4) sz = VIAL_RAW_EPSIZE - 4;
    {
        uint8_t i, base;
        uint16_t total = VIAL_LAYERS * NUM_KEYS * 2;
        uint8_t did_write = 0;
        /* offset>=total writes nothing; else base<total, base+i<92. */
        if (offset >= total) return;
        base = (uint8_t)offset;
        for (i = 0; i < sz; i++) {
            uint8_t addr = (uint8_t)(base + i);
            if (addr < total) {
                uint16_t entry = addr / 2;
                uint8_t hi = addr & 1;
                uint16_t kc = keymap[entry / NUM_KEYS][entry % NUM_KEYS];
                uint8_t b = msg[4 + (sz_be==0?1:0) + i];
                if (hi) kc = (kc & 0xFF00) | b;
                else    kc = (kc & 0x00FF) | ((uint16_t)b << 8);
                keymap[entry / NUM_KEYS][entry % NUM_KEYS] = kc;
                did_write = 1;
            }
        }
        if (did_write) dataflash_mark_dirty();
    }
}

/* VIA macro 0x0E GET_BUFFER: ">BHB" BE framing only (macro.py verified,
 * no hidapi pad). Data at msg[4:]. Chunks <=28B (BUFFER_FETCH_CHUNK). */
static void vh_macro_get(__xdata uint8_t *msg, uint8_t length) __reentrant {
    uint16_t offset = ((uint16_t)msg[1] << 8) | msg[2];
    uint8_t sz = msg[3];
    uint8_t i, base;
    if (sz > VIAL_RAW_EPSIZE - 4) sz = VIAL_RAW_EPSIZE - 4;
    memset(msg, 0, length);
    /* offset>=LEN reads all zeros; else base<LEN and base+i<64 (no wrap). */
    if (offset >= MACRO_POOL_LEN) return;
    base = (uint8_t)offset;
    for (i = 0; i < sz; i++) {
        uint8_t addr = (uint8_t)(base + i);
        if (addr < MACRO_POOL_LEN) msg[4 + i] = macro_pool[addr];
    }
}

/* VIA macro 0x0F SET_BUFFER: LOCK-gated by caller. Same framing. */
static void vh_macro_set(__xdata uint8_t *msg, uint8_t length) __reentrant {
    uint16_t offset = ((uint16_t)msg[1] << 8) | msg[2];
    uint8_t sz = msg[3];
    uint8_t i, base;
    uint8_t did_write = 0;
    (void)length;
    if (sz > VIAL_RAW_EPSIZE - 4) sz = VIAL_RAW_EPSIZE - 4;
    if (offset >= MACRO_POOL_LEN) return;
    base = (uint8_t)offset;
    for (i = 0; i < sz; i++) {
        uint8_t addr = (uint8_t)(base + i);
        if (addr < MACRO_POOL_LEN) {
            macro_pool[addr] = msg[4 + i];
            did_write = 1;
        }
    }
    if (did_write) dataflash_mark_dirty();
}

/* Fill response buffer with Vial command result. msg is the 32-byte raw packet. */
void vial_handle_cmd(__xdata uint8_t *msg, uint8_t length) {
    if (length != VIAL_RAW_EPSIZE)
        return;

    if (msg[0] == 0x00 && (msg[1] == 0xFE || msg[1] == 0xEE || msg[1] == 0x01 || msg[1] == 0x02 ||
                           msg[1] == 0x04 || msg[1] == 0x05 || msg[1] == 0x07 || msg[1] == 0x08 ||
                           msg[1] == 0x09 || msg[1] == 0x0B || msg[1] == 0x0C || msg[1] == 0x0D || msg[1] == 0x0E ||
                           msg[1] == 0x0F || msg[1] == 0x10 ||
                           msg[1] == 0x11 || msg[1] == 0x12 || msg[1] == 0x13 || msg[1] == 0x17)) {
        for (uint8_t i = 0; i < VIAL_RAW_EPSIZE - 1; i++)
            msg[i] = msg[i + 1];
        msg[VIAL_RAW_EPSIZE - 1] = 0;
        length = length - 1;
    }

    uint8_t prefix = msg[0];

    /* Vial extension: prefix 0xFE + subcommand in msg[1] */
    if (prefix == 0xFE) {
        switch (msg[1]) {
        case vial_get_keyboard_id: {
            memset(msg, 0, length);
            uint32_t ver = VIAL_PROTOCOL_VER;
            msg[0] = ver & 0xFF;
            msg[1] = (ver >> 8) & 0xFF;
            msg[2] = (ver >> 16) & 0xFF;
            msg[3] = (ver >> 24) & 0xFF;
            memcpy(&msg[4], vial_uid_ram, 8);
            break;
        }
        case vial_get_size: {
            uint32_t sz = VIAL_DEFINITION_LEN;
            msg[0] = sz & 0xFF;
            msg[1] = (sz >> 8) & 0xFF;
            msg[2] = (sz >> 16) & 0xFF;
            msg[3] = (sz >> 24) & 0xFF;
            break;
        }
        case vial_get_def:
            vh_get_def(msg, length);
            break;
        case vial_get_encoder: {
            uint8_t layer = msg[2];
            uint8_t idx = msg[3];  /* we only have 1 encoder: idx 0 */
            uint16_t cw = (layer < VIAL_LAYERS && idx == 0) ? encoder_map[layer][0] : 0;
            uint16_t ccw = (layer < VIAL_LAYERS && idx == 0) ? encoder_map[layer][1] : 0;
            msg[0] = (cw >> 8) & 0xFF;
            msg[1] = cw & 0xFF;
            msg[2] = (ccw >> 8) & 0xFF;
            msg[3] = ccw & 0xFF;
            break;
        }
        case vial_set_encoder: {
            if (!vial_unlocked) break;
            uint8_t layer = msg[2];
            uint8_t idx = msg[3];
            uint8_t dir = msg[4];
            uint16_t kc = ((uint16_t)msg[5] << 8) | msg[6];
            if (layer < VIAL_LAYERS && idx == 0 && dir < 2) {
                encoder_map[layer][dir] = kc;
                dataflash_mark_dirty();
            }
            break;
        }
        case vial_get_unlock_status: {
            /* Must match vial-qmk quantum/vial.c exactly:
             * byte0=unlocked, byte1=in_progress, bytes2+ = row/col pairs, rest 0xFF */
            memset(msg, 0xFF, length);
            msg[0] = vial_unlocked;
            msg[1] = vial_unlock_in_progress;
            /* 7-key combo: (0,0) KEY1, (0,1) KEY2, (0,2) KEY3, (1,0) KEY4, (1,1) KEY5, (1,2) KEY6, (1,3) ENC */
            msg[2] = 0; msg[3] = 0;
            msg[4] = 0; msg[5] = 1;
            msg[6] = 0; msg[7] = 2;
            msg[8] = 1; msg[9] = 0;
            msg[10] = 1; msg[11] = 1;
            msg[12] = 1; msg[13] = 2;
            msg[14] = 1; msg[15] = 3;
            break;
        }
        case vial_unlock_start: {
            vial_unlock_in_progress = 1;
            vial_unlock_counter = VIAL_UNLOCK_COUNTER_MAX;
            memset(msg, 0, length);
            msg[0] = vial_unlocked;
            msg[1] = vial_unlock_in_progress;
            msg[2] = vial_unlock_counter;
            break;
        }
        case vial_unlock_poll: {
            /* Check 7-key combo directly in ISR context (reading GPIO is safe). */
            {
                uint8_t p1 = P1;
                uint8_t p3 = P3;
                uint8_t all_pressed = 1;
                if (p1 & (1u<<1)) all_pressed = 0;
                if (p1 & (1u<<7)) all_pressed = 0;
                if (p1 & (1u<<6)) all_pressed = 0;
                if (p1 & (1u<<5)) all_pressed = 0;
                if (p1 & (1u<<4)) all_pressed = 0;
                if (p3 & (1u<<2)) all_pressed = 0;
                if (p3 & (1u<<3)) all_pressed = 0;
                if (vial_unlock_in_progress) {
                    if (all_pressed) {
                        if (vial_unlock_counter > 0) vial_unlock_counter--;
                        if (vial_unlock_counter == 0) {
                            vial_unlocked = 1;
                            vial_unlock_in_progress = 0;
                        }
                    } else {
                        vial_unlock_counter = VIAL_UNLOCK_COUNTER_MAX;
                    }
                }
            }
            memset(msg, 0, length);
            msg[0] = vial_unlocked;
            msg[1] = vial_unlock_in_progress;
            msg[2] = vial_unlock_counter;
            break;
        }
        case vial_lock: {
            vial_unlocked = 0;
            vial_unlock_in_progress = 0;
            vial_unlock_counter = VIAL_UNLOCK_COUNTER_MAX;
            memset(msg, 0, length);
            break;
        }
        case 0x09: { /* vial_qmk_settings_query — no QMK settings, return 0xFFFF to terminate loop */
            memset(msg, 0xFF, length);
            msg[0] = 0xFF;
            msg[1] = 0xFF;
            break;
        }
        case 0x0D: { /* vial_dynamic_entry_op */
            uint8_t op = msg[2];
            if (op == DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES) {
                memset(msg, 0, length);
                msg[0] = VIAL_TAP_DANCE_ENTRIES;
                msg[1] = 0; /* combo */
                msg[2] = 0; /* key override */
                msg[3] = 0; /* alt repeat */
                /* last byte feature bits =0 */
            } else if (op == DYNAMIC_VIAL_TAP_DANCE_GET) {
                uint8_t idx = msg[3];
                memset(msg, 0, length);
                if (idx < VIAL_TAP_DANCE_ENTRIES) {
                    msg[0] = 0;
                    memcpy(&msg[1], &td_entries[idx], sizeof(td_entry_t));
                } else {
                    msg[0] = 1;
                }
            } else if (op == DYNAMIC_VIAL_TAP_DANCE_SET) {
                uint8_t idx = msg[3];
                if (!vial_unlocked) break;
                if (idx < VIAL_TAP_DANCE_ENTRIES) {
                    memcpy(&td_entries[idx], &msg[4], sizeof(td_entry_t));
                    dataflash_mark_dirty();
                    memset(msg, 0, length);
                    msg[0] = 0;
                } else {
                    memset(msg, 0, length);
                    msg[0] = 1;
                }
            } else {
                memset(msg, 0, length);
            }
            break;
        }
        default:
            memset(msg, 0, length);
            break;
        }
        return;
    }

    /* SVAL board protocol (prefix 0xEE) */
    if (prefix == 0xEE) {
        switch (msg[1]) {
        case 0x01:
            memset(msg, 0, length);
            break;
        case 0x02:
            memset(msg, 0, length);
            break;
        case 0x10:
            memset(msg, 0, length);
            break;
        case 0x11:
            break;
        default:
            memset(msg, 0, length);
            break;
        }
        return;
    }

    /* VIA base commands (no prefix) */
    switch (prefix) {
    case CMD_VIA_GET_PROTOCOL_VERSION: {
        msg[0] = 0;
        msg[1] = 0x00;
        msg[2] = 0x09;
        break;
    }
    case CMD_VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT:
        msg[0] = 0;
        msg[1] = VIAL_LAYERS;
        break;
    /* VIA lighting: 16-color quantized (hue>>4), sat==0 forces white.
     * Not lock-gated: cosmetic only, same as before. */
    case CMD_VIA_LIGHTING_SET_VALUE: { /* 0x07: msg[1]=value_id */
        uint8_t id = msg[1];
        /* Direct var writes (no setter calls: flash budget, see rgb.h).
         * Unknown IDs latch nothing but still mark dirty (harmless). */
        if (id == QMK_RGBLIGHT_BRIGHTNESS) rgb_brightness = msg[2];
        else if (id == QMK_RGBLIGHT_EFFECT) rgb_effect = msg[2];
        else if (id == QMK_RGBLIGHT_COLOR) { rgb_hue = msg[2]; rgb_sat = msg[3]; }
        rgb_dirty = 1;
        dataflash_mark_dirty();
        break;
    }
    case CMD_VIA_LIGHTING_GET_VALUE: { /* 0x08 */
        uint8_t id = msg[1];
        msg[0] = CMD_VIA_LIGHTING_GET_VALUE;
        msg[1] = id;
        if (id == QMK_RGBLIGHT_BRIGHTNESS) msg[2] = rgb_brightness;
        else if (id == QMK_RGBLIGHT_EFFECT) msg[2] = rgb_effect;
        else if (id == QMK_RGBLIGHT_COLOR) { msg[2] = rgb_hue; msg[3] = rgb_sat; }
        else msg[2] = 0;
        break;
    }
    case CMD_VIA_LIGHTING_SAVE: /* 0x09: persist (deferred, same mechanism) */
        dataflash_mark_dirty();
        break;
    case CMD_VIA_BOOTLOADER_JUMP: { /* 0x0B: ISP jump. LOCK-gated like SETs. Never returns: no response is sent. */
        if (!vial_unlocked) break;
        usb_isp_jump();
        break;
    }
    case CMD_VIA_DYNAMIC_KEYMAP_GET_KEYCODE: {
        uint8_t layer = msg[1];
        uint8_t row = msg[2];
        uint8_t col = msg[3];
        uint8_t key = 0xFF;
        if (row < 2 && col < 4) key = row * 4 + col;
        uint16_t kc = 0;
        if (key != 0xFF && layer < VIAL_LAYERS)
            kc = keymap[layer][key];
        msg[0] = (kc >> 8) & 0xFF;
        msg[1] = kc & 0xFF;
        break;
    }
    case CMD_VIA_DYNAMIC_KEYMAP_SET_KEYCODE: {
        if (!vial_unlocked) break;
        uint8_t layer = msg[1];
        uint8_t row = msg[2];
        uint8_t col = msg[3];
        if (layer < VIAL_LAYERS && row < 2 && col < 4) {
            uint8_t key = row * 4 + col;
            uint16_t kc = ((uint16_t)msg[4] << 8) | msg[5];
            keymap[layer][key] = kc;
            dataflash_mark_dirty();
        }
        break;
    }
    case CMD_VIA_DYNAMIC_KEYMAP_GET_BUFFER:
        vh_get_buf(msg, length);
        break;
    case CMD_VIA_DYNAMIC_KEYMAP_SET_BUFFER:
        if (!vial_unlocked) break;
        vh_set_buf(msg, length);
        break;
    /* VIA macro (B-spec minimal: 2 slots, 36B pool, tap-only+hold player).
     * Framing verified against vial-gui protocol/macro.py. SET/RESET gated. */
    case CMD_VIA_MACRO_GET_COUNT:
        msg[0] = 0;
        msg[1] = MACRO_COUNT;
        break;
    case CMD_VIA_MACRO_GET_BUFFER_SIZE:
        msg[0] = 0;
        msg[1] = (MACRO_POOL_LEN >> 8) & 0xFF;
        msg[2] = MACRO_POOL_LEN & 0xFF;
        break;
    case CMD_VIA_MACRO_GET_BUFFER:
        vh_macro_get(msg, length);
        break;
    case CMD_VIA_MACRO_SET_BUFFER:
        if (!vial_unlocked) break;
        vh_macro_set(msg, length);
        break;
    case CMD_VIA_MACRO_RESET: {
        uint8_t r;
        if (!vial_unlocked) break;
        for (r = 0; r < MACRO_POOL_LEN; r++) macro_pool[r] = 0;
        dataflash_mark_dirty();
        break;
    }
    default:
        memset(msg, 0, length);
        break;
    }
}
