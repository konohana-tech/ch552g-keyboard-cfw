/*
 * ch552.h — CH552G SFR definitions
 *
 * Register addresses verified against reference implementation that
 * runs successfully on CH552G hardware (CH552[0x5211], BTVER 02.50).
 * USB register addresses differ from early CH552 datasheet revisions.
 */
#ifndef CH552_H
#define CH552_H

#include <8051.h>

/* SDCC SBIT/SFR/SFR16 macros */
#ifndef SBIT
#define SBIT(name, addr, bit) __sbit __at((addr) + (bit)) name
#endif
#ifndef SFR
#define SFR(name, addr) __sfr __at(addr) name
#endif
#ifndef SFR16
#define SFR16(name, addr) __sfr __at(addr) name; __sfr __at((addr) + 1) name##_HIGH
#endif

/* ================================================================== */
/*  CH552-specific SFRs (not in standard 8051.h)                      */
/* ================================================================== */

/* --- GPIO extended ------------------------------------------------- */
/* Verified working on HW 0x5211: P1 at 0x91/0x92, P3 at 0xB1/0xB2 */
SFR(P1_MOD_OC,   0x91);
SFR(P1_DIR_PU,   0x92);
SFR(P3_MOD_OC,   0xB1);
SFR(P3_DIR_PU,   0xB2);
SFR(P4,          0xC0);
SFR(P4_MOD_OC,   0xC1);
SFR(P4_DIR_PU,   0xC2);
SFR(P5,          0xD0);
SFR(P5_MOD_OC,   0xD1);
SFR(P5_DIR_PU,   0xD2);

/* --- Clock --------------------------------------------------------- */
SFR(CLOCK_CFG,   0xB9);
SFR(SAFE_MOD,    0xA1); /* datasheet Table 5.1: safe mode ctrl (write only).
 * Was wrongly 0xA6 (that's CHIP_ID): clock switch never took effect,
 * Fsys stayed 6MHz from Phase 1. Fixed 2026-09-03: Fsys=12MHz now. */
/* Also define SAFE_MOD at datasheet address 0xA1 for DataFlash compatibility */
SFR(SAFE_MOD_DTS, 0xA1);

/* --- Global config / Flash ----------------------------------------- */
/* On this HW revision GLOBAL_CFG shares address 0xB1 with P3_MOD_OC */
#define GLOBAL_CFG      P3_MOD_OC
#define bDATA_WE        0x04
#define bCODE_WE        0x08
#define bBOOT_LOAD      0x20
#define bSW_RESET       0x10

SFR16(ROM_ADDR,  0x84);
SFR(ROM_ADDR_L,  0x84);
SFR(ROM_ADDR_H,  0x85);
SFR(ROM_CTRL,    0x86);
SFR16(ROM_DATA,  0x8E);
SFR(ROM_DATA_L,  0x8E);
SFR(ROM_DATA_H,  0x8F);
#define ROM_STATUS      ROM_CTRL
#define bROM_ADDR_OK    0x40
#define bROM_CMD_ERR    0x02
#define ROM_CMD_WRITE   0x9A
#define ROM_CMD_READ    0x8E
#define DATA_FLASH_ADDR 0xC000

/* --- Pin function -------------------------------------------------- */
SFR(PIN_FUNC,    0xC3);

/* --- Interrupt enable (extended) ----------------------------------- */
SFR(IE_EX,       0xE8);
SBIT(IE_USB, 0xE8, 2);

/* --- Timer 2 ------------------------------------------------------- */
SFR(T2MOD,       0xC9);
SFR(T2CON,       0xC8);

/* --- Timer0 (16-bit free-run boot timebase; default clock Fsys/12) --- */
SFR(TCON, 0x88);
SFR(TMOD, 0x89);
SFR(TL0,  0x8A);
SFR(TH0,  0x8C);
SBIT(TR0, 0x88, 4);
SBIT(TF0, 0x88, 5);
SFR(TH2,         0xCD);
SFR(TL2,         0xCC);

/* --- USB device controller ----------------------------------------- */
SFR(USB_CTRL,    0xE2);  /* USB control */
SFR(USB_DEV_AD,  0xE3);  /* USB device address */
SFR(UDEV_CTRL,   0xD1);  /* USB device port control */
SFR(USB_INT_EN,  0xE1);  /* USB interrupt enable */
SFR(USB_INT_FG,  0xD8);  /* USB interrupt flag (bit-addressable) */
SFR(USB_INT_ST,  0xD9);  /* USB interrupt status */
SFR(USB_MIS_ST,  0xDA);  /* USB miscellaneous status */
SFR(USB_RX_LEN,  0xDB);  /* USB reception length */

/* --- USB endpoint -------------------------------------------------- */
SFR(UEP4_1_MOD,  0xEA);  /* EP1, EP4 mode control */
SFR(UEP2_3_MOD,  0xEB);  /* EP2, EP3 mode control */
SFR16(UEP0_DMA,  0xEC);  /* EP0 DMA address (0xEC + 0xED) */
SFR16(UEP1_DMA,  0xEE);  /* EP1 DMA address (0xEE + 0xEF) */
SFR16(UEP2_DMA,  0xE4);  /* EP2 DMA address (0xE4 + 0xE5) */
SFR(UEP0_CTRL,   0xDC);  /* EP0 control */
SFR(UEP0_T_LEN,  0xDD);  /* EP0 TX length */
SFR(UEP1_CTRL,   0xD2);  /* EP1 control */
SFR(UEP1_T_LEN,  0xD3);  /* EP1 TX length */
SFR(UEP2_CTRL,   0xD4);  /* EP2 control */
SFR(UEP2_T_LEN,  0xD5);  /* EP2 TX length */

/* --- USB interrupt flags (bit-addressable at 0xD8) ----------------- */
SBIT(UIF_BUS_RST,  0xD8, 0);
SBIT(UIF_TRANSFER, 0xD8, 1);
SBIT(UIF_SUSPEND,  0xD8, 2);
SBIT(UIF_FIFO_OV,  0xD8, 4);

/* --- GPIO port bits ----------------------------------------------- */
SBIT(P10, 0x90, 0); SBIT(P11, 0x90, 1); SBIT(P12, 0x90, 2);
SBIT(P13, 0x90, 3); SBIT(P14, 0x90, 4); SBIT(P15, 0x90, 5);
SBIT(P16, 0x90, 6); SBIT(P17, 0x90, 7);

SBIT(P30, 0xB0, 0); SBIT(P31, 0xB0, 1); SBIT(P32, 0xB0, 2);
SBIT(P33, 0xB0, 3); SBIT(P34, 0xB0, 4); SBIT(P35, 0xB0, 5);
SBIT(P36, 0xB0, 6); SBIT(P37, 0xB0, 7);

SBIT(P40, 0xC0, 0); SBIT(P41, 0xC0, 1); SBIT(P42, 0xC0, 2);
SBIT(P43, 0xC0, 3); SBIT(P44, 0xC0, 4); SBIT(P45, 0xC0, 5);
SBIT(P46, 0xC0, 6); SBIT(P47, 0xC0, 7);

SBIT(P54, 0xD0, 4); SBIT(P55, 0xD0, 5); SBIT(P56, 0xD0, 6);
SBIT(P57, 0xD0, 7);

/* ================================================================== */
/*  Bit definitions — USB register bits                                */
/* ================================================================== */

/* USB_CTRL */
#define bUC_DEV_PU_EN   0x20
#define bUC_DMA_EN      0x01
#define bUC_INT_BUSY    0x08
#define bUC_RESET_SIE   0x04
#define bUC_CLR_ALL     0x02

/* UDEV_CTRL */
#define bUD_PD_DIS      0x80
#define bUD_PORT_EN     0x01

/* UEP4_1_MOD */
#define bUEP1_RX_EN     0x80
#define bUEP1_TX_EN     0x40
#define bUEP0_BUF_MOD   0x01

/* UEP2_3_MOD */
#define bUEP2_RX_EN     0x08
#define bUEP2_TX_EN     0x04
#define bUEP2_BUF_MOD   0x01

/* UEPn_CTRL */
#define bUEP_R_TOG      0x80
#define bUEP_T_TOG      0x40
#define bUEP_AUTO_TOG   0x10   /* bit 4: 1=auto toggle, 0=manual. NOT 0x40! */
#define MASK_UEP_R_RES  0x0C
#define UEP_R_RES_ACK   0x00
#define UEP_R_RES_NAK   0x08
#define UEP_R_RES_STALL 0x0C
#define MASK_UEP_T_RES  0x03
#define UEP_T_RES_ACK   0x00
#define UEP_T_RES_NAK   0x02
#define UEP_T_RES_STALL 0x03
#define U_TOG_OK        0x20
#define bUDA_GP_BIT     0x80

/* USB_INT_EN */
#define bUIE_SUSPEND    0x04
#define bUIE_TRANSFER   0x02
#define bUIE_BUS_RST    0x01

/* USB_INT_ST */
#define MASK_UIS_TOKEN  0x30
#define UIS_TOKEN_OUT   0x00
#define UIS_TOKEN_SOF   0x10
#define UIS_TOKEN_IN    0x20
#define UIS_TOKEN_SETUP 0x30
#define MASK_UIS_ENDP   0x0F

/* USB_MIS_ST */
#define bUMS_SUSPEND    0x04

/* Clock */
#define MASK_SYS_CK_SEL 0x07

/* Interrupt number */
#define INT_NO_USB      8

/* Helper: enter/exit safe mode on both addresses for compatibility */
#define SAFE_MODE_ENTER() do { SAFE_MOD = 0x55; SAFE_MOD = 0xAA; SAFE_MOD_DTS = 0x55; SAFE_MOD_DTS = 0xAA; } while(0)
#define SAFE_MODE_EXIT()  do { SAFE_MOD = 0x00; SAFE_MOD_DTS = 0x00; } while(0)

#endif /* CH552_H */
