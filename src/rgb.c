/* rgb.c - WS2812B x6 chain on P3.4 (16-color quantized solid/off).
 * Hue 0-255 maps via hue>>4 to 16 wheel colors; sat==0 forces white
 * (QMK convention). No 16-bit math anywhere in the render path:
 * brightness scaling is (base*bri)>>8 via MUL AB, so no __mulint lib.
 *
 * Full-white 6 LEDs = 360mA (60mA/LED) + MCU ~20mA stays under the USB
 * 500mA descriptor, so the old 32-bit current-limiter is dead weight here.
 *
 * NOTE: one asm instruction PER LINE inside __asm blocks.
 * sdas8051 treats ';' as comment start, so `push ACC; push PSW' would
 * assemble ONLY the first push (broken save/restore -> caller regs clobbered
 * -> tx loop wedges inside EA=0 -> total death). Never compact.
 *
 * Hardware (schematic-verified): P3.4 -> R11 1k -> D1.DIN -> ... -> D6.DOUT
 * (open end). All VDD=VBUS 5V, MCU VCC=5V so VOH satisfies VIH=0.7*VDD.
 * LED type: WS2812B-compatible (exact part unknown), all 2020 footprint.
 *
 * Timing: sysclk 12MHz (CLOCK_CFG=0x04, datasheet Table 8.2.2), 1T E8051 core.
 * WS2812B bit (wagiminator constraints): T0H <500ns / T1H >625ns / TCT >1150ns.
 * Bit ops are 2 cycles (SETB/CLR/MOV bit), so HIGH is never split by branch:
 * `mov P3.4,C` ends '0' early. Cycle model (wagiminator CH552-USB-Knob neo.c,
 * proven on CH552+SDCC): RLC=1, SETB bit=2, MOV bit,C=2, CLR bit=2,
 * DJNZ taken=4 @12MHz (83ns).
 *   T0H (C=0): SETB+MOV = 4c ~333ns (must be <500ns)
 *   T1H (C=1): SETB+MOV+4NOP = 8c ~667ns (must be >625ns)
 *   TCT both: 15c ~1250ns (must be >1150ns)
 */
#include <stdint.h>
#include "ch552.h"
#include "config.h"
#include "rgb.h"

__xdata __at (0x00E8) uint8_t rgb_brightness;
__xdata __at (0x00E9) uint8_t rgb_effect;
__xdata __at (0x00EA) uint8_t rgb_dirty;
__xdata __at (0x00EB) uint8_t rgb_boot_n;
__xdata __at (0x015C) uint8_t rgb_hue;
__xdata __at (0x015D) uint8_t rgb_sat;

/* TX staging (GRB of the single zone) */
__xdata __at (0x00ED) static uint8_t tx_g;
__xdata __at (0x00EE) static uint8_t tx_r;
__xdata __at (0x00EF) static uint8_t tx_b;

void rgb_set_defaults(void) {
    rgb_brightness = RGB_DEFAULT_BRIGHTNESS;
    rgb_effect = RGB_DEFAULT_EFFECT;
    rgb_hue = 0;
    rgb_sat = 0; /* default white (matches the verified B-spec behavior) */
    rgb_dirty = 0;
    /* Dark until the chain settles after VBUS/POR ramp: first TX after
     * ~255 polls (~50-130ms + the boot delay + USB enumeration time).
     * The Vial SET handler latches state + marks dirty; the scheduled
     * first TX renders the latest. TX itself runs with EA=0, so a
     * partial frame cannot latch. */
    rgb_boot_n = 255;
}

/* Single-LED TX from staging. Same timing as documented above.
 * Touched regs are R2-R5 (+A/PSW/DPTR): only those are saved.
 * (Older version saved R0-R7; trimmed after asm audit: R0/R1/R6/R7
 * are never touched here.) */
void rgb_tx_one(void) __naked {
__asm
    push ACC
    push PSW
    push DPL
    push DPH
    push ar2
    push ar3
    push ar4
    push ar5
    mov DPTR,#_tx_g
    movx A,@DPTR
    mov R2,A
    inc DPTR
    movx A,@DPTR
    mov R3,A
    inc DPTR
    movx A,@DPTR
    mov R4,A
    mov A,R2
    mov R5,#8
rgb1_bit_g:
    rlc A
    setb _P34
    mov _P34, C
    nop
    nop
    nop
    nop
    clr _P34
    djnz R5,rgb1_bit_g
    mov A,R3
    mov R5,#8
rgb1_bit_r:
    rlc A
    setb _P34
    mov _P34, C
    nop
    nop
    nop
    nop
    clr _P34
    djnz R5,rgb1_bit_r
    mov A,R4
    mov R5,#8
rgb1_bit_b:
    rlc A
    setb _P34
    mov _P34, C
    nop
    nop
    nop
    nop
    clr _P34
    djnz R5,rgb1_bit_b
    pop ar5
    pop ar4
    pop ar3
    pop ar2
    pop DPH
    pop DPL
    pop PSW
    pop ACC
    ret
__endasm;
}

/* Uniform-zone transmit loop. Own frame (1B). */
static void fx_uniform_tx(void) {
    uint8_t i;
    for (i = 0; i < RGB_NUM_LEDS; i++) rgb_tx_one();
}

/* 16-color wheel table (RGB order), full saturation. Hue 0-255 maps via
 * idx = hue >> 4 (no division). 48B in code flash (+24B vs 8-color).
 * Entries sampled at h = n*16 with tools/gen_hue_table.py wheel(). */
static __code uint8_t hue16[16][3] = {
    {255, 0, 0},     /* 0: red (h=0) */
    {255, 96, 0},    /* 1: orange-red (h=16) */
    {255, 192, 0},   /* 2: orange (h=32) */
    {225, 255, 0},   /* 3: amber (h=48) */
    {129, 255, 0},   /* 4: yellow-green (h=64) */
    {33, 255, 0},    /* 5: lime (h=80) */
    {0, 255, 60},    /* 6: green (h=96) */
    {0, 255, 156},   /* 7: mint (h=112) */
    {0, 255, 252},   /* 8: cyan (h=128) */
    {0, 165, 255},   /* 9: sky (h=144) */
    {0, 69, 255},    /* 10: azure (h=160) */
    {24, 0, 255},    /* 11: blue (h=176) */
    {120, 0, 255},   /* 12: violet (h=192) */
    {216, 0, 255},   /* 13: purple (h=208) */
    {255, 0, 201},   /* 14: magenta (h=224) */
    {255, 0, 105},   /* 15: rose (h=240) */
};

/* Render the color frame into staging. Naked asm: the C equivalent pulls
 * __mulint (3x 16-bit mult) + gptrget addressing. Hand-rolled with MUL AB
 * (8-bit, high byte kept) it is ~70B total and no lib.
 * Reads globals, writes tx_g/r/b. Saves everything it touches
 * (MUL clobbers OV/CY in PSW, hence PSW too). */
static void rgb_render_color(void) __naked {
__asm
    push ACC
    push PSW
    push DPL
    push DPH
    push ar5
    push ar6
    push ar7
    mov DPTR,#_rgb_hue
    movx A,@DPTR
    swap A
    anl A,#0x0F
    mov B,#0x03
    mul AB
    add A,#<_hue16
    mov R6,A
    mov A,#>_hue16
    addc A,B
    mov R7,A
    mov DPTR,#_rgb_brightness
    movx A,@DPTR
    mov R5,A
    mov DPL,R6
    mov DPH,R7
    clr A
    movc A,@A+DPTR
    mov B,A
    mov A,R5
    mul AB
    mov A,B
    mov DPTR,#_tx_r
    movx @DPTR,A
    mov DPL,R6
    mov DPH,R7
    inc DPTR
    mov R6,DPL
    mov R7,DPH
    clr A
    movc A,@A+DPTR
    mov B,A
    mov A,R5
    mul AB
    mov A,B
    mov DPTR,#_tx_g
    movx @DPTR,A
    mov DPL,R6
    mov DPH,R7
    inc DPTR
    clr A
    movc A,@A+DPTR
    mov B,A
    mov A,R5
    mul AB
    mov A,B
    mov DPTR,#_tx_b
    movx @DPTR,A
    pop ar7
    pop ar6
    pop ar5
    pop DPH
    pop DPL
    pop PSW
    pop ACC
    ret
__endasm;
}

void rgb_poll(void) {
    uint8_t b;
    if (rgb_boot_n) {
        if (--rgb_boot_n == 0) rgb_dirty = 1;
    }
    if (!rgb_dirty)
        return;
    rgb_dirty = 0;
    if (rgb_effect == 1) {
        if (rgb_sat == 0) {
            b = rgb_brightness; /* single fetch */
            tx_g = b; tx_r = b; tx_b = b;
        } else {
            rgb_render_color();
        }
    } else { /* off: everything except solid -> dark frame */
        tx_g = 0; tx_r = 0; tx_b = 0;
    }
    EA = 0;
    fx_uniform_tx();
    EA = 1;
    { volatile uint16_t d; for (d = 0; d < 1500; d++); } /* ~500us reset, EA=1 */
}
