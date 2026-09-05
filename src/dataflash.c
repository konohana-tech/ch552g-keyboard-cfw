/* dataflash.c - CH552G DataFlash persistence
 * 128B DataFlash at 0xC000, accessed via ROM_ADDR/ROM_DATA/ROM_CTRL.
 * GLOBAL_CFG shares address 0xB1 with P3_MOD_OC on this HW, so save/restore.
 * Each logical byte is at even physical address (Addr<<1).
 */
#include <stdint.h>
#include <string.h>
#include "ch552.h"
#include "config.h"
#include "vial.h"
#include "dataflash.h"
#include "tap_dance.h"
#include "rgb.h"

static __data uint8_t dirty;
static __data uint16_t dirty_delay;

/* low-level byte read/write */
static uint8_t df_read_byte(uint8_t logical_addr) {
    ROM_ADDR_H = DATA_FLASH_ADDR >> 8;
    ROM_ADDR_L = (uint8_t)(logical_addr << 1);
    ROM_CTRL = ROM_CMD_READ;
    { uint8_t d = 20; while(d--); } /* x2 counts: same wall time @12MHz as 10 @6MHz */
    return ROM_DATA_L;
}

static void df_write_byte(uint8_t logical_addr, uint8_t data) {
    uint8_t saved = P3_MOD_OC;
    SAFE_MODE_ENTER();
    GLOBAL_CFG |= bDATA_WE;
    SAFE_MODE_EXIT();

    ROM_ADDR_H = DATA_FLASH_ADDR >> 8;
    ROM_ADDR_L = (uint8_t)(logical_addr << 1);
    ROM_DATA_L = data;
    if (ROM_STATUS & bROM_ADDR_OK) {
        ROM_CTRL = ROM_CMD_WRITE;
        { uint16_t d = 600; while(d--); } /* x2 counts: same wall time @12MHz */
    }

    SAFE_MODE_ENTER();
    GLOBAL_CFG &= (uint8_t)~bDATA_WE;
    SAFE_MODE_EXIT();
    P3_MOD_OC = saved;
}

static uint8_t calc_checksum(void) {
    uint8_t cs = 0;
    uint8_t i;
    for (i = 0; i < VIAL_LAYERS; i++) {
        uint8_t k;
        for (k = 0; k < NUM_KEYS; k++) {
            uint16_t kc = keymap[i][k];
            cs ^= (uint8_t)(kc >> 8);
            cs ^= (uint8_t)(kc & 0xFF);
        }
    }
    for (i = 0; i < VIAL_LAYERS; i++) {
        uint16_t a = encoder_map[i][0];
        uint16_t b = encoder_map[i][1];
        cs ^= (uint8_t)(a >> 8); cs ^= (uint8_t)(a & 0xFF);
        cs ^= (uint8_t)(b >> 8); cs ^= (uint8_t)(b & 0xFF);
    }
    for (i = 0; i < DATAFLASH_MACRO_LEN; i++)
        cs ^= macro_pool[i];
    for (i = 0; i < DATAFLASH_TAP_DANCE_COUNT; i++) {
        __xdata uint8_t *p = (__xdata uint8_t*)&td_entries[i];
        uint8_t j;
        for (j = 0; j < DATAFLASH_TAP_DANCE_LEN; j++) cs ^= p[j];
    }
    return cs;
}

void dataflash_init(void) {
    dirty = 0;
    dirty_delay = 0;
}

uint8_t dataflash_load(void) {
    uint8_t magic = df_read_byte(0);
    uint8_t ver = df_read_byte(1);
    uint8_t stored_cs = df_read_byte(2);
    if (magic != DATAFLASH_MAGIC || ver != DATAFLASH_VERSION) {
        return 0;
    }
    uint8_t i, k;
    uint8_t cs = 0;
    uint8_t offset = DATAFLASH_KEYMAP_OFFSET;
    for (i = 0; i < VIAL_LAYERS; i++) {
        for (k = 0; k < NUM_KEYS; k++) {
            uint8_t hi = df_read_byte(offset++);
            uint8_t lo = df_read_byte(offset++);
            uint16_t kc = ((uint16_t)hi << 8) | lo;
            keymap[i][k] = kc;
            cs ^= hi; cs ^= lo;
        }
    }
    for (i = 0; i < VIAL_LAYERS; i++) {
        uint8_t hi = df_read_byte(offset++);
        uint8_t lo = df_read_byte(offset++);
        uint16_t a = ((uint16_t)hi << 8) | lo;
        cs ^= hi; cs ^= lo;
        hi = df_read_byte(offset++);
        lo = df_read_byte(offset++);
        uint16_t b = ((uint16_t)hi << 8) | lo;
        cs ^= hi; cs ^= lo;
        encoder_map[i][0] = a;
        encoder_map[i][1] = b;
    }
    /* Macro pool at fixed 89..104 (16B); tap dance 105..124 (20B) */
    for (i = 0; i < DATAFLASH_MACRO_LEN; i++) {
        uint8_t b = df_read_byte((uint8_t)(DATAFLASH_MACRO_OFFSET + i));
        macro_pool[i] = b;
        cs ^= b;
    }
    /* Zero remaining macro_pool beyond persisted 16B (RAM-only, not checksummed) */
    for (i = DATAFLASH_MACRO_LEN; i < MACRO_POOL_LEN; i++) macro_pool[i]=0;
    /* Tap dance */
    for (i = 0; i < DATAFLASH_TAP_DANCE_COUNT; i++) {
        __xdata uint8_t *p = (__xdata uint8_t*)&td_entries[i];
        uint8_t j;
        for (j = 0; j < DATAFLASH_TAP_DANCE_LEN; j++) {
            uint8_t b = df_read_byte((uint8_t)(DATAFLASH_TAP_DANCE_OFFSET + i*DATAFLASH_TAP_DANCE_LEN + j));
            p[j]=b;
            cs ^= b;
        }
    }
    if (cs != stored_cs) {
        return 0;
    }
    /* LED region has its own checksum (byte 3); the keymap stays valid
     * even when LED bytes are unwritten (0xFF) or from an older image.
     * LED checksum is stored INVERTED (~xor): erased flash (all 0xFF)
     * can never validate, so a fresh/older image keeps RGB defaults. */
    {
        uint8_t j, lcs = 0;
        for (j = 0; j < DATAFLASH_LED_LEN; j++)
            lcs ^= df_read_byte((uint8_t)(DATAFLASH_LED_OFFSET + j));
        if ((uint8_t)~lcs == df_read_byte(DATAFLASH_LED_CSUM)) {
            rgb_brightness = df_read_byte(DATAFLASH_LED_OFFSET);
            rgb_effect = df_read_byte((uint8_t)(DATAFLASH_LED_OFFSET + 1));
            rgb_hue = df_read_byte((uint8_t)(DATAFLASH_LED_OFFSET + 2));
            rgb_sat = df_read_byte((uint8_t)(DATAFLASH_LED_OFFSET + 3));
        }
    }
    return 1;
}

void dataflash_save(void) {
    uint8_t cs = calc_checksum();
    uint8_t i, k;
    uint8_t offset;

    df_write_byte(0, DATAFLASH_MAGIC);
    df_write_byte(1, DATAFLASH_VERSION);
    df_write_byte(2, cs);
    df_write_byte(3, 0xFF);

    offset = DATAFLASH_KEYMAP_OFFSET;
    for (i = 0; i < VIAL_LAYERS; i++) {
        for (k = 0; k < NUM_KEYS; k++) {
            uint16_t kc = keymap[i][k];
            df_write_byte(offset++, (uint8_t)(kc >> 8));
            df_write_byte(offset++, (uint8_t)(kc & 0xFF));
        }
    }
    for (i = 0; i < VIAL_LAYERS; i++) {
        uint16_t a = encoder_map[i][0];
        uint16_t b = encoder_map[i][1];
        df_write_byte(offset++, (uint8_t)(a >> 8));
        df_write_byte(offset++, (uint8_t)(a & 0xFF));
        df_write_byte(offset++, (uint8_t)(b >> 8));
        df_write_byte(offset++, (uint8_t)(b & 0xFF));
    }
    for (i = 0; i < DATAFLASH_MACRO_LEN; i++)
        df_write_byte((uint8_t)(DATAFLASH_MACRO_OFFSET + i), macro_pool[i]);
    for (i = 0; i < DATAFLASH_TAP_DANCE_COUNT; i++) {
        __xdata uint8_t *p = (__xdata uint8_t*)&td_entries[i];
        uint8_t j;
        for (j = 0; j < DATAFLASH_TAP_DANCE_LEN; j++)
            df_write_byte((uint8_t)(DATAFLASH_TAP_DANCE_OFFSET + i*DATAFLASH_TAP_DANCE_LEN + j), p[j]);
    }
    /* LED region + inverted checksum (byte 3 was the 0xFF placeholder above) */
    df_write_byte(DATAFLASH_LED_OFFSET, rgb_brightness);
    df_write_byte((uint8_t)(DATAFLASH_LED_OFFSET + 1), rgb_effect);
    df_write_byte((uint8_t)(DATAFLASH_LED_OFFSET + 2), rgb_hue);
    df_write_byte((uint8_t)(DATAFLASH_LED_OFFSET + 3), rgb_sat);
    df_write_byte(DATAFLASH_LED_CSUM,
        (uint8_t)~(uint8_t)(rgb_brightness ^ rgb_effect ^ rgb_hue ^ rgb_sat));
    dirty = 0;
    dirty_delay = 0;
}

void dataflash_mark_dirty(void) {
    dirty = 1;
    dirty_delay = 1000; /* ~100ms @12MHz (was 500 @6MHz) */
}

void dataflash_poll(void) {
    if (dirty) {
        if (dirty_delay > 0) {
            dirty_delay--;
        } else {
            EA = 0;
            dataflash_save();
            EA = 1;
        }
    }
}

void dataflash_erase_all(void) {
    uint8_t i;
    EA = 0;
    for (i = 0; i < DATAFLASH_SIZE; i++) {
        df_write_byte(i, 0xFF);
    }
    EA = 1;
}
