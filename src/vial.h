/* vial.h - Vial RAW HID protocol for CH552G (subset, RAM keymap) */
#ifndef VIAL_H
#define VIAL_H

#include <stdint.h>
#include "config.h"

/* Vial protocol constants (from vial-qmk/quantum/vial.h) */
#define VIAL_RAW_EPSIZE    32
#define VIAL_PROTOCOL_VER  0x00000006
#define VIAL_UNLOCK_COUNTER_MAX 10

/* id_vial subcommands (msg[1] when msg[0]==0xFE) */
enum {
    vial_get_keyboard_id = 0x00,
    vial_get_size        = 0x01,
    vial_get_def         = 0x02,
    vial_get_encoder     = 0x03,
    vial_set_encoder     = 0x04,
    vial_get_unlock_status = 0x05,
    vial_unlock_start    = 0x06,
    vial_unlock_poll     = 0x07,
    vial_lock            = 0x08,
};

/* VIA base commands (msg[0], no 0xFE prefix) */
#define CMD_VIA_GET_PROTOCOL_VERSION  0x01
#define CMD_VIA_GET_KEYBOARD_VALUE    0x02
#define CMD_VIA_SET_KEYBOARD_VALUE    0x03
#define CMD_VIA_DYNAMIC_KEYMAP_GET_KEYCODE 0x04
#define CMD_VIA_DYNAMIC_KEYMAP_SET_KEYCODE 0x05
#define CMD_VIA_DYNAMIC_KEYMAP_GET_BUFFER   0x12
#define CMD_VIA_DYNAMIC_KEYMAP_SET_BUFFER   0x13
#define CMD_VIA_DYNAMIC_KEYMAP_GET_LAYER_COUNT 0x11
#define CMD_VIA_BOOTLOADER_JUMP 0x0B
/* VIA macro commands (vial-gui protocol/macro.py framing, verified):
 * 0x0C count->msg[1]; 0x0D size BE16->msg[1:2]; 0x0E/0x0F BE offset+size,
 * data at msg[4:] (">BHB", no hidapi pad). */
#define CMD_VIA_MACRO_GET_COUNT       0x0C
#define CMD_VIA_MACRO_GET_BUFFER_SIZE 0x0D
#define CMD_VIA_MACRO_GET_BUFFER      0x0E
#define CMD_VIA_MACRO_SET_BUFFER      0x0F
#define CMD_VIA_MACRO_RESET           0x10

/* VIA lighting (16-color quantized: brightness + effect + hue/sat).
 * Sat==0 forces white; hue maps via hue>>4 to 16 wheel colors. */
#define CMD_VIA_LIGHTING_SET_VALUE 0x07
#define CMD_VIA_LIGHTING_GET_VALUE 0x08
#define CMD_VIA_LIGHTING_SAVE      0x09
#define QMK_RGBLIGHT_BRIGHTNESS   0x80
#define QMK_RGBLIGHT_EFFECT       0x81
#define QMK_RGBLIGHT_COLOR        0x83

/* Vial dynamic entries (tap dance) */
#define CMD_VIAL_DYNAMIC_ENTRY_OP 0x0D
#define DYNAMIC_VIAL_GET_NUMBER_OF_ENTRIES 0x00
#define DYNAMIC_VIAL_TAP_DANCE_GET 0x01
#define DYNAMIC_VIAL_TAP_DANCE_SET 0x02
#define VIAL_TAP_DANCE_ENTRIES 2

/* QMK tap dance keycodes */
#define QK_TAP_DANCE 0x5700
#define QK_TAP_DANCE_MAX 0x57FF

/* Keymap storage: 4*8*2=64B at 0x0098, encoder 4*2*2=16B at 0x00D8 (after keymap) */
#define VIAL_KEYMAP_ADDR 0x0098
extern __xdata __at (VIAL_KEYMAP_ADDR) uint16_t keymap[VIAL_LAYERS][NUM_KEYS];

#define VIAL_ENCODER_MAP_ADDR 0x00D8
extern __xdata __at (VIAL_ENCODER_MAP_ADDR) uint16_t encoder_map[VIAL_LAYERS][2];

/* Macro pool: 3 slots (QK_MACRO_0/1/2), NUL-separated, 36B total shared.
 * XRAM 0x0100+ (SIE/DMA touch only <0x0100; 1KB XRAM, rest free).
 * DataFlash mirror at 89..124 (see dataflash.h). */
#define MACRO_COUNT 3
#define MACRO_POOL_LEN 36
#define MACRO_POOL_XRAM 0x0100
#define DATAFLASH_MACRO_OFFSET 89
extern __xdata __at (MACRO_POOL_XRAM) uint8_t macro_pool[MACRO_POOL_LEN];

/* QMK macro keycodes (quantum/keycodes.h, verified): QK_MACRO 0x7700-0x777F.
 * Slots used: 0x7700 (M0), 0x7701 (M1), 0x7702 (M2). */
#define QK_MACRO_BASE 0x7700
#define QK_MACRO_MASK 0xFF80

/* Vial lock state */
extern __data uint8_t vial_unlocked;
extern __data uint8_t vial_unlock_in_progress;
extern __data uint8_t vial_unlock_counter;

void vial_init(void);
void vial_handle_cmd(__xdata uint8_t *msg, uint8_t length);

#endif
