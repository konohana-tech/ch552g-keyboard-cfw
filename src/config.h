// config.h - CH552G keyboard config
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include "ch552.h"

// USB device descriptor
#define USB_VENDOR_ID       0x1209
#define USB_PRODUCT_ID      0x0001
#define USB_DEVICE_VERSION  0x0100

// USB configuration descriptor
#define USB_MAX_POWER_MA    500  /* 6x WS2812B full-white 360mA + MCU */

// USB descriptor strings
// Serial MUST contain "vial:" prefix for Vial GUI discovery.
// Changed to "vial:f64c2b3c" to match VIAL_SERIAL_NUMBER_MAGIC checked by
// src/main/python/util.py:find_vial_devices (so desktop "no devices" is fixed).
#define MANUFACTURER_STR    'k','o','n','o','h','a','n','a'
#define PRODUCT_STR         'C','H','5','5','2','G','-','K','B','D'
#define SERIAL_STR          'v','i','a','l',':','f','6','4','c','2','b','3','c'

// Key count (matrix: 2 rows x 4 cols = 8)
#define NUM_KEYS 8

// RGB: WS2812B x6 chain on P3.4 (B-spec: fixed-white solid/off only)
#define RGB_NUM_LEDS 6
#define RGB_DEFAULT_BRIGHTNESS 76   /* 30% */
#define RGB_DEFAULT_EFFECT 1        /* 1=solid, else off */

// Vial configuration
#define VIAL_LAYERS         4
// 8-byte keyboard UID base (fixed prefix + fallback tail).
// At boot vial_uid_ram[0..3] takes bytes [0..3] always; tail [4..7] is
// replaced by the per-unit value folded from the factory chip ID
// (0x3FFA/0x3FFC/0x3FFE, see vial.c uid_init). [4..7] below is only used
// when the chip ID area reads blank (then units collide: needs checking).
#define VIAL_KEYBOARD_UID   { 0x12, 0x09, 0x00, 0x01, 0xAB, 0xCD, 0xEF, 0x00 }
#define VIAL_PROTOCOL_VERSION  0x00000006

/* Security model: Vial GUI requires the board to be "unlocked" before allowing
 * keymap/macro edits.
 *  - Initial state is LOCKED on every boot.
 *  - Unlock combo: hold all 7 inputs simultaneously (KEY1..KEY6 + encoder switch).
 *    Once unlocked it stays unlocked until vial_lock (FE08) or power cycle.
 *  - While locked, Vial SET commands (FE04, 05, 13) are ignored; GET commands
 *    still respond so the GUI can show the locked overlay. */

#endif
