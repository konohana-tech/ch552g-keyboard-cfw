/* rgb.h - WS2812B x6 driver (16-color quantized solid/off + Vial control) */
#ifndef RGB_H
#define RGB_H

#include <stdint.h>

/* DataFlash LED region (logical bytes; keymap/checksum layout untouched).
 * 16-color quantized: hue/sat persisted, mapped to 16 wheel colors at render.
 * Own inverted-xor checksum in byte 3, same scheme as before. */
#define DATAFLASH_LED_OFFSET 84 /* 84: brightness, 85: effect, 86: hue, 87: sat */
#define DATAFLASH_LED_CSUM   3  /* ~(b ^ e ^ h ^ s); erased 0xFF never validates */
#define DATAFLASH_LED_LEN    4

/* State: 0x00E8-0x00EF (encoder_map ends 0x00E8; USB SETUP window
 * starts 0x00F0) + hue/sat at 0x015C-0x015D (free XRAM past 0x015B).
 * TX staging reuses 0x00ED-0x00EF.
 * Declare address in BOTH definition and extern (SDCC overlaps auto-place). */
extern __xdata __at (0x00E8) uint8_t rgb_brightness; /* 0-255 white/color level */
extern __xdata __at (0x00E9) uint8_t rgb_effect;     /* 1=solid, else off */
extern __xdata __at (0x00EA) uint8_t rgb_dirty;      /* 1 = needs retransmit */
extern __xdata __at (0x00EB) uint8_t rgb_boot_n;     /* >0: boot delay polls to first TX */
/* Hue/sat live past the contiguous block (0x015C+ is free XRAM;
 * 0x00F0-0x00FF is the USB SETUP/scratch window, off limits). */
extern __xdata __at (0x015C) uint8_t rgb_hue;        /* 0-255, quantized to 16 colors */
extern __xdata __at (0x015D) uint8_t rgb_sat;        /* 0=white, else hue color */

void rgb_set_defaults(void);
void rgb_poll(void); /* main loop: boot-delayed on-demand transmit */
/* NOTE: no setter functions (flash budget): the single Vial SET handler
 * writes the latched vars directly + marks dirty. All runtime LED
 * mutations go through it; defaults/load write vars directly too. */
void rgb_tx_one(void); /* single LED from staging (sole TX path) */

#endif
