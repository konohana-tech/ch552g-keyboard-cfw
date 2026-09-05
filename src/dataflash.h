/* dataflash.h - CH552G DataFlash persistence for Vial keymap */
#ifndef DATAFLASH_H
#define DATAFLASH_H

#include <stdint.h>

#define DATAFLASH_MAGIC 0xA5
#define DATAFLASH_VERSION 0x08 /* v08: LED 84..87 (brightness,effect,hue,sat; 16-color) */

/* Layout in 128B DataFlash (logical bytes):
 * 0: magic (0xA5)
 * 1: version (0x08; older versions are rejected -> defaults)
 * 2: checksum (xor of bytes 4..124, i.e. keymap+encoder+macro+tap)
 * 3: LED checksum (~xor of 84..87; erased 0xFF never validates)
 * 4..67 : keymap[VIAL_LAYERS][NUM_KEYS] = 4*8*2 = 64 bytes (big-endian per entry)
 * 68..83: encoder_map[4][2] = 16 bytes (big-endian)
 * 84..87: LED (brightness, effect, hue, sat; 16-color quantized)
 * 88: reserved, 89..104: macro pool (16B, NUL-separated)
 * 105..124: tap dance 2 entries (20B, 10B each: on_tap, on_hold, on_double_tap, on_tap_hold, term)
 * 88, 125..127: reserved (0xFF)
 * Total used: 124 bytes within 128B limit.
 */
#define DATAFLASH_SIZE 128
#define DATAFLASH_KEYMAP_OFFSET 4
#define DATAFLASH_ENCODER_OFFSET 68
#define DATAFLASH_MACRO_OFFSET 89
#define DATAFLASH_MACRO_LEN 16
#define DATAFLASH_TAP_DANCE_OFFSET 105
#define DATAFLASH_TAP_DANCE_COUNT 2
#define DATAFLASH_TAP_DANCE_LEN 10

void dataflash_init(void);
uint8_t dataflash_load(void);   /* returns 1 if valid data loaded, 0 if defaults kept */
void dataflash_save(void);
void dataflash_mark_dirty(void);
void dataflash_poll(void);      /* call from main loop to handle deferred write */
void dataflash_erase_all(void); /* for testing */

#endif
