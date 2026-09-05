#include <string.h>
#include "ch552.h"
#include "config.h"
#include "usb.h"
#include "vial.h"

volatile uint16_t usb_tx_len;
volatile uint8_t  SetupReq, UsbConfig;
__code uint8_t *p_usb_tx;

/* Forward declaration: defined later in this file, used by ISR */
void usb_send_raw(__xdata uint8_t *data, uint8_t len);

/* EP0 OUT data stage pending flag: SET_REPORT payload arrives on the EP0 OUT
   data stage, not on the interrupt OUT endpoint. Set in USB_EP0_SETUP. */
static uint8_t ep0_out_pending = 0;

// HID Keyboard+Mouse Report Descriptor (IF0: keyboard ID1 + mouse ID2)
__code uint8_t HIDReportDesc[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,        //   Usage Minimum (Left Control)
  0x29, 0xE7,        //   Usage Maximum (Right GUI)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Constant)
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01,        //   Usage Minimum (Num Lock)
  0x29, 0x05,        //   Usage Maximum (Kana)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x05,        //   Report Count (5)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x03,        //   Report Size (3)
  0x91, 0x01,        //   Output (Constant)
  0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0xFF,        //   Usage Maximum (255)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x06,        //   Report Count (6)
  0x81, 0x00,        //   Input (Data, Array)
  0xC0,              // End Collection (keyboard)

  // Mouse collection (Report ID 2: buttons + X + Y + wheel, same IF0/EP1)
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x85, 0x02,        //     Report ID (2)
  0x05, 0x09,        //     Usage Page (Buttons)
  0x19, 0x01,        //     Usage Minimum (1)
  0x29, 0x03,        //     Usage Maximum (3)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x03,        //     Report Count (3)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data,Var,Abs)
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x05,        //     Report Size (5)
  0x81, 0x01,        //     Input (Constant)
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x06,        //     Input (Data,Var,Rel)
  0xC0,              //   End Collection
  0xC0               // End Collection (mouse)
};

// RAW HID Report Descriptor (Vial) — 25 bytes (NO Report ID; NO padding)
// Usage Page 0xFF60 (Vendor Defined), 64-byte Input + 64-byte Output.
// NOTE: do NOT define a Report ID (0x85). Linux HID rejects report_id 0 as invalid
// ("report_id 0 is invalid" -> device fails to bind as hidraw). Vial desktop/libhidapi
// and WebHID send the 32-byte command directly (optionally with a leading 0x00 byte that
// vial_handle_cmd skips); no Report ID in the descriptor is the correct Vial layout.
__code uint8_t raw_report_desc[] = {
  0x06, 0x60, 0xFF,   // Usage Page (Vendor Defined 0xFF60)
  0x09, 0x61,         // Usage (Vendor Defined)
  0xA1, 0x01,         // Collection (Application)
  0x09, 0x62,         //   Usage (Vendor Defined)
  0x15, 0x00,         //   Logical Minimum (0)
  0x26, 0xFF, 0x00,   //   Logical Maximum (255)
  0x75, 0x08,         //   Report Size (8)
  0x95, 0x20,         //   Report Count (32) — Vial protocol uses 32-byte chunks
  0x81, 0x02,         //   Input (Data,Var,Abs)
  0x09, 0x63,         //   Usage (Vendor Defined)
  0x91, 0x02,         //   Output (Data,Var,Abs)
  0xC0                // End Collection
};
#define RAW_REPORT_DESC_LEN (sizeof(raw_report_desc))

// Device Descriptor
__code USB_DEV_DESCR DevDescr = {
  .bLength            = sizeof(DevDescr),
  .bDescriptorType    = USB_DESCR_TYP_DEVICE,
  .bcdUSB             = 0x0110,
  .bDeviceClass       = 0,
  .bDeviceSubClass    = 0,
  .bDeviceProtocol    = 0,
  .bMaxPacketSize0    = EP0_SIZE,
  .idVendor           = USB_VENDOR_ID,
  .idProduct          = USB_PRODUCT_ID,
  .bcdDevice          = USB_DEVICE_VERSION,
  .iManufacturer      = 1,
  .iProduct           = 2,
  .iSerialNumber      = 3,
  .bNumConfigurations = 1
};

// Configuration Descriptor: composite (keyboard+mouse IF0 + RAW HID IF1)
// Total: 9 + (9+9+7) + (9+9+7+7) = 66 bytes
// IF0 carries keyboard (Report ID 1) + mouse wheel (Report ID 2) multiplexed on EP1.
__code uint8_t CfgDescr[] = {
  // Configuration descriptor (9B)
  9, 0x02,            // bLength, CONFIG
  66, 0x00,           // wTotalLength = 66
  2,                  // bNumInterfaces = 2
  1,                  // bConfigurationValue
  0,                  // iConfiguration
  0xA0,               // bmAttributes: bus-powered + remote wakeup
  USB_MAX_POWER_MA / 2, // MaxPower

  // Interface 0: HID Keyboard, Report ID 1 (non-boot: reports carry IDs)
  9, 0x04,            // bLength, INTERFACE
  0,                  // bInterfaceNumber
  0,                  // bAlternateSetting
  1,                  // bNumEndpoints
  0x03,               // bInterfaceClass = HID
  0x00,               // bInterfaceSubClass = none
  0x00,               // bInterfaceProtocol = none
  0,                  // iInterface

  // HID descriptor for keyboard
  9, 0x21,            // bLength, HID
  0x11, 0x01,         // bcdHID 1.11
  0x00,               // bCountryCode
  1,                  // bNumDescriptors
  0x22,               // bDescriptorType = REPORT
  sizeof(HIDReportDesc) & 0xFF, (sizeof(HIDReportDesc) >> 8) & 0xFF,

  // Endpoint 1 IN (keyboard ID1 9B / mouse ID2 5B, multiplexed)
  7, 0x05,            // bLength, ENDPOINT
  0x81,               // bEndpointAddress = EP1 IN
  0x03,               // bmAttributes = interrupt
  EP1_SIZE, 0x00,     // wMaxPacketSize = 9
  10,                 // bInterval = 10ms

  // Interface 1: RAW HID (Vial), Vendor Defined
  9, 0x04,            // bLength, INTERFACE
  1,                  // bInterfaceNumber
  0,                  // bAlternateSetting
  2,                  // bNumEndpoints (IN + OUT)  <-- REQUIRED: host needs OUT to send commands
  0x03,               // bInterfaceClass = HID
  0x00,               // bInterfaceSubClass = none
  0x00,               // bInterfaceProtocol = none
  0,                  // iInterface

  // HID descriptor for RAW HID
  9, 0x21,            // bLength, HID
  0x11, 0x01,         // bcdHID 1.11
  0x00,               // bCountryCode
  1,                  // bNumDescriptors
  0x22,               // bDescriptorType = REPORT
  25, 0x00,           // wDescriptorLength = 25 (raw_report_desc)

  // Endpoint 2 IN (RAW HID, 64B)
  7, 0x05,            // bLength, ENDPOINT
  0x82,               // bEndpointAddress = EP2 IN
  0x03,               // bmAttributes = interrupt
  EP2_SIZE, 0x00,     // wMaxPacketSize = 64
  1,                  // bInterval = 1ms

  // Endpoint 2 OUT (RAW HID, 64B)  <-- previously MISSING: this is why no command was received
  7, 0x05,            // bLength, ENDPOINT
  0x02,               // bEndpointAddress = EP2 OUT
  0x03,               // bmAttributes = interrupt
  EP2_SIZE, 0x00,     // wMaxPacketSize = 64
  1,                  // bInterval = 1ms
};

// String Descriptors
__code uint16_t ManufDescr[] = {
  ((uint16_t)USB_DESCR_TYP_STRING << 8) | sizeof(ManufDescr), MANUFACTURER_STR };
__code uint16_t ProdDescr[] = {
  ((uint16_t)USB_DESCR_TYP_STRING << 8) | sizeof(ProdDescr), PRODUCT_STR };
__code uint16_t SerDescr[] = {
  ((uint16_t)USB_DESCR_TYP_STRING << 8) | sizeof(SerDescr), SERIAL_STR };

// HID report buffer
extern __xdata uint8_t kbd_report[8];

void vial_handle_cmd(__xdata uint8_t *rx, uint8_t len);  // Vial protocol handler (src/vial.c); called from USB ISR (EP2 OUT / EP0 SET_REPORT)

void USB_EP0_SETUP(void) {
  uint8_t len = 0;
  usb_tx_len = setupBuf->wLengthL;
  SetupReq = setupBuf->bRequest;

  switch(setupBuf->bRequestType & 0x60) {
  case 0x00: // Standard
    switch(SetupReq) {
    case USB_GET_DESCRIPTOR:
      switch(setupBuf->wValueH) {
      case USB_DESCR_TYP_DEVICE:
        p_usb_tx = (uint8_t*)&DevDescr;
        len = sizeof(DevDescr);
        break;
      case USB_DESCR_TYP_CONFIG:
        p_usb_tx = (uint8_t*)CfgDescr;
        len = sizeof(CfgDescr);
        break;
      case USB_DESCR_TYP_STRING:
        switch(setupBuf->wValueL) {
        case 1: p_usb_tx = USB_STR_DESCR_i1; break;
        case 2: p_usb_tx = USB_STR_DESCR_i2; break;
        case 3: p_usb_tx = USB_STR_DESCR_i3; break;
        default: p_usb_tx = USB_STR_DESCR_i1; break;
        }
        len = setupBuf->wLengthL;
        break;
      case USB_DESCR_TYP_HID_REPORT:
        // wIndexL = interface number
        if(setupBuf->wIndexL == 0) {
          // Interface 0: keyboard report descriptor
          p_usb_tx = (uint8_t*)HIDReportDesc;
          len = sizeof(HIDReportDesc);
        } else {
          // Interface 1: RAW HID (Vial) report descriptor
          p_usb_tx = (uint8_t*)raw_report_desc;
          len = RAW_REPORT_DESC_LEN;
        }
        break;
      }
      break;
    case USB_SET_ADDRESS:
      usb_tx_len = setupBuf->wValueL;
      break;
    case USB_GET_CONFIGURATION:
      EP0_buffer[0] = UsbConfig;
      UEP0_T_LEN = 1;
      break;
    case USB_SET_CONFIGURATION:
      UsbConfig = setupBuf->wValueL;
      break;
    case USB_GET_STATUS:
      EP0_buffer[0] = 0x00;
      EP0_buffer[1] = 0x00;
      UEP0_T_LEN = 2;
      break;
    }
    break;

  case 0x20: // Class (HID)
    switch(SetupReq) {
    case 0x09: // SET_REPORT — host sends Vial command as 64B OUT data stage
      ep0_out_pending = 1;
      UEP0_T_LEN  = 0;   // Arm EP0 to receive the OUT data stage
      UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
      break;
    case 0x0A: // SET_IDLE
      break;
    }
    break;
  }

  if(len) {
    if(usb_tx_len > len) usb_tx_len = len;
    len = MIN(usb_tx_len, EP0_SIZE);
    memcpy(EP0_buffer, p_usb_tx, len);
    usb_tx_len -= len;
    p_usb_tx += len;
  }

  UEP0_T_LEN = len;
  UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
}

void USB_EP0_IN(void) {
  uint8_t len;
  switch(SetupReq) {
  case USB_GET_DESCRIPTOR:
    len = MIN(usb_tx_len, EP0_SIZE);
    memcpy(EP0_buffer, p_usb_tx, len);
    usb_tx_len  -= len;
    p_usb_tx    += len;
    UEP0_T_LEN = len;
    UEP0_CTRL ^= bUEP_T_TOG;
    break;
  case USB_SET_ADDRESS:
    USB_DEV_AD = USB_DEV_AD & bUDA_GP_BIT | usb_tx_len;
    UEP0_CTRL  = UEP_R_RES_ACK | UEP_T_RES_NAK;
    break;
  case 0x09: // SET_REPORT status stage (IN, 0 bytes) — complete the control transfer
    UEP0_T_LEN = 0;
    UEP0_CTRL ^= bUEP_T_TOG;
    break;
  }
}

void USBInterrupt(void) {
  if(UIF_TRANSFER) {
    switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {
      case UIS_TOKEN_IN | 0:
        USB_EP0_IN();
        break;
      case UIS_TOKEN_SETUP | 0:
        USB_EP0_SETUP();
        break;
      case UIS_TOKEN_IN | 1:
        /* HID IN (keyboard ID1 9B / mouse ID2 5B multiplexed). The sender
           arms UEP1_T_LEN per report; the SIE clears it after each IN send,
           so just NAK here until the next report is armed. */
        UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
        break;
      case UIS_TOKEN_IN | 2:
        /* RAW HID EP2 IN: response was armed by the OUT handler (usb_send_raw
           set UEP2_T_LEN + TX ACK). After this IN is sent the SIE clears
           UEP2_T_LEN, so NAK further INs until the next OUT re-arms.
           Do NOT re-arm here: writing UEP2_T_LEN on every IN poll destabilizes
           the CH552 bidirectional endpoint and makes EP2 OUT NAK (ETIMEDOUT). */
        UEP2_CTRL = (UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
        break;

      case UIS_TOKEN_OUT | 2:
        /* RAW HID EP2 OUT: Vial command in first 32B of EP2_buffer (HW buf is 64B).
           vial_handle_cmd writes the 32B response into EP2_buffer[0..31];
           usb_send_raw copies it to the IN region [64..95] for the next IN. */
        vial_handle_cmd(EP2_buffer, 32);
        usb_send_raw(EP2_buffer, 32);
        break;

      case UIS_TOKEN_OUT | 0:
        /* EP0 OUT data stage (SET_REPORT payload). Host sends Vial command
           here when issuing a HID SET_REPORT on the RAW HID interface. */
        if (ep0_out_pending) {
            ep0_out_pending = 0;
            vial_handle_cmd(EP0_buffer, EP0_SIZE);
            usb_send_raw(EP0_buffer, EP0_SIZE);
        }
        UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
        break;
    }
    UIF_TRANSFER = 0;
  }

  if(UIF_BUS_RST) {
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK;
    UEP2_CTRL = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
    USB_DEV_AD   = 0x00;
    UsbConfig    = 0;
    UIF_SUSPEND  = 0;
    UIF_TRANSFER = 0;
    UIF_BUS_RST  = 0;
  }

  if (UIF_SUSPEND) {
    UIF_SUSPEND = 0;
    if ( !(USB_MIS_ST & bUMS_SUSPEND) ) USB_INT_FG = 0xFF;
  }
}

void USBInit(void) {
  USB_CTRL    |= bUC_RESET_SIE | bUC_CLR_ALL;
  USB_CTRL    &= ~bUC_CLR_ALL;

  USB_CTRL    = bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;
  UDEV_CTRL   = bUD_PD_DIS | bUD_PORT_EN;

  UEP0_DMA    = EP0_ADDR;
  UEP0_CTRL   = UEP_R_RES_ACK | UEP_T_RES_NAK;

  UEP1_DMA    = EP1_ADDR;
  UEP1_CTRL   = bUEP_AUTO_TOG | UEP_T_RES_NAK;
  UEP4_1_MOD  = bUEP1_TX_EN;

  UEP2_DMA    = EP2_ADDR;
  /* Match reference bidirectional EP2: AUTO_TOG + UEP_R_RES_ACK so the SIE
     ACKs host OUT transactions (required for UIS_TOKEN_OUT|2 to fire).
     Without UEP_R_RES_ACK the OUT is never completed -> handler never runs. */
  UEP2_CTRL   = bUEP_AUTO_TOG | UEP_T_RES_NAK | UEP_R_RES_ACK;
  UEP2_3_MOD  = bUEP2_TX_EN | bUEP2_RX_EN;  // RAW HID 64B bidirectional
  UEP2_T_LEN  = 0;

  USB_INT_EN |= bUIE_SUSPEND | bUIE_TRANSFER | bUIE_BUS_RST;
  USB_INT_FG |= 0x1F;
  IE_USB      = 1;
  EA          = 1;

  UEP0_T_LEN  = 0;
  EP1_buffer[0] = 1; /* idle keyboard report (ID 1 + 8 zero bytes) */
  EP1_buffer[1] = 0; EP1_buffer[2] = 0; EP1_buffer[3] = 0; EP1_buffer[4] = 0;
  EP1_buffer[5] = 0; EP1_buffer[6] = 0; EP1_buffer[7] = 0; EP1_buffer[8] = 0;
  UEP1_T_LEN  = EP1_SIZE;
  UEP2_T_LEN  = EP2_SIZE;
}

/* Keyboard report send (Report ID 1 + 8B). main.c keeps its 8B layout;
 * the ID prefix is added here. Call from main loop only. */
void usb_send_report(uint8_t report[8]) {
    uint8_t i;
    EP1_buffer[0] = 1;
    for (i = 0; i < 8; i++) EP1_buffer[1 + i] = report[i];
    UEP1_T_LEN = EP1_SIZE; /* 9 */
    UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
}

/* Mouse report send (Report ID 2 + buttons/X/Y/wheel = 5B).
 * Call from main loop only (shares EP1 with keyboard: last writer wins
 * per host poll; wheel detents are rare so collisions are negligible). */
void usb_send_mouse(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel) {
    EP1_buffer[0] = 2;
    EP1_buffer[1] = buttons;
    EP1_buffer[2] = (uint8_t)dx;
    EP1_buffer[3] = (uint8_t)dy;
    EP1_buffer[4] = (uint8_t)wheel;
    UEP1_T_LEN = 5;
    UEP1_CTRL = (UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
}

/* RAW HID report send (for Vial). Call from ISR context only.
 * CH552 EP2 HW buffer is 64B (OUT[0..63] + IN[64..127]).
 * Vial protocol uses 32B chunks: caller passes a 32B buffer; we copy it
 * into the IN region EP2_buffer[64..95]. */
void usb_send_raw(__xdata uint8_t *data, uint8_t len) {
    if (len > 32) len = 32;
    for (uint8_t i = 0; i < len; i++) {
        EP2_buffer[EP2_SIZE + i] = data[i];   // TX buffer is IN region at UEP2_DMA+64
    }
    UEP2_T_LEN = len;
    UEP2_CTRL = (UEP2_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;  // AUTO_TOG manages TX PID
}

/* ISP jump for VIA CMD_VIA_BOOTLOADER_JUMP (0x0B). Never returns.
 * Why this sequence (see skill references/isp-jump-investigation.md):
 * - bSW_RESET cannot enter ISP: software reset clears bBOOT_LOAD, so the
 *   bootloader at 0x3800 is skipped and the app restarts (datasheet GLOBAL_CFG).
 * - Bare LJMP 0x3800 fails: with USB still enumerated as the app (1209:0001),
 *   the host never sees a disconnect/re-enumerate as 4348:55e0 (ch55xduino #63).
 * So: kill interrupts, detach USB (D+ pullup off + port off) so the host
 * processes a disconnect, wait ~330ms on Timer0 (polled, no ISR needed),
 * then LJMP the bootloader entry. The bootloader re-enumerates as 4348:55e0.
 * ISR-safe: called from vial_handle_cmd (EP2 OUT / EP0 SET_REPORT context).
 * Overlay cost: 1 byte local. No response is sent (device re-enumerates). */
void usb_isp_jump(void) {
    uint8_t k;
    EA = 0;
    IE_USB = 0;
    USB_CTRL &= (uint8_t)~bUC_DEV_PU_EN;
    UDEV_CTRL &= (uint8_t)~bUD_PORT_EN;
    TMOD &= 0xF0; TMOD |= 0x01; /* Timer0 mode 1 (16-bit free-run) */
    TH0 = 0; TL0 = 0; TF0 = 0; TR0 = 1; /* 65.5ms/overflow @12MHz */
    for (k = 0; k < 5; k++) {
        while (!TF0)
            ;
        TF0 = 0;
    }
    TR0 = 0;
    __asm
        ljmp 0x3800
    __endasm;
    while (1)
        ;
}
