#include <stdint.h>
#include "config.h"

#define MIN(a,b) ((a>b) ? b : a)
#define EP_BUF_SIZE(x) (x+2)  /* CH552 endpoint buffer = maxpacket + 2-byte SIE header */

#define EP0_SIZE 8
#define EP1_SIZE 9  /* Report ID (1B) + keyboard 8B; mouse uses ID + 4B in same EP1 */
#define EP2_SIZE 64  /* CH552 EP2 HW buffer is 64B fixed (OUT[64]+IN[64]); Vial protocol uses 32B chunks */

#define EP0_ADDR 0
#define EP0_BUF_SIZE EP_BUF_SIZE(EP0_SIZE)
#define EP1_ADDR (EP0_ADDR + EP0_BUF_SIZE)
#define EP1_BUF_SIZE 12 /* 9+2=11, +1 pad so EP2_ADDR stays even for SIE DMA */
#define EP2_ADDR (EP1_ADDR + EP1_BUF_SIZE)
#define EP2_BUF_SIZE (EP2_SIZE * 2 + 2)  /* CH552 EP2 bidirectional: OUT[32] + IN[32] + SIE header margin */

#define USB_GET_STATUS          0x00
#define USB_CLEAR_FEATURE       0x01
#define USB_SET_FEATURE         0x03
#define USB_SET_ADDRESS         0x05
#define USB_GET_DESCRIPTOR      0x06
#define USB_SET_DESCRIPTOR      0x07
#define USB_GET_CONFIGURATION   0x08
#define USB_SET_CONFIGURATION   0x09

#define USB_REQ_TYP_MASK        0x60
#define USB_REQ_TYP_STANDARD    0x00
#define USB_REQ_TYP_CLASS       0x20

#define USB_DESCR_TYP_DEVICE    0x01
#define USB_DESCR_TYP_CONFIG    0x02
#define USB_DESCR_TYP_STRING    0x03
#define USB_DESCR_TYP_INTERF    0x04
#define USB_DESCR_TYP_ENDP      0x05
#define USB_DESCR_TYP_HID       0x21
#define USB_DESCR_TYP_HID_REPORT 0x22

#define USB_ENDP_TYPE_INTER     0x03
#define USB_ENDP_ADDR_EP1_IN    0x81

typedef struct _USB_SETUP_REQ {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint8_t  wValueL;
    uint8_t  wValueH;
    uint8_t  wIndexL;
    uint8_t  wIndexH;
    uint8_t  wLengthL;
    uint8_t  wLengthH;
} USB_SETUP_REQ, *PUSB_SETUP_REQ;

typedef struct _USB_DEV_DESCR {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} USB_DEV_DESCR;

// Flat HID keyboard config descriptor (34 bytes total)
typedef struct _USB_CFG_DESCR_HID {
    // Config descriptor (9B)
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  MaxPower;
    // Interface descriptor (9B)
    uint8_t  itf_bLength;
    uint8_t  itf_bDescriptorType;
    uint8_t  itf_bInterfaceNumber;
    uint8_t  itf_bAlternateSetting;
    uint8_t  itf_bNumEndpoints;
    uint8_t  itf_bInterfaceClass;
    uint8_t  itf_bInterfaceSubClass;
    uint8_t  itf_bInterfaceProtocol;
    uint8_t  itf_iInterface;
    // HID descriptor (9B)
    uint8_t  hid_bLength;
    uint8_t  hid_bDescriptorType;
    uint16_t hid_bcdHID;
    uint8_t  hid_bCountryCode;
    uint8_t  hid_bNumDescriptors;
    uint8_t  hid_bDescriptorType2;
    uint16_t hid_wDescriptorLength;
    // Endpoint descriptor (7B)
    uint8_t  ep_bLength;
    uint8_t  ep_bDescriptorType;
    uint8_t  ep_bEndpointAddress;
    uint8_t  ep_bmAttributes;
    uint16_t ep_wMaxPacketSize;
    uint8_t  ep_bInterval;
} USB_CFG_DESCR_HID;

__xdata __at (EP0_ADDR) uint8_t EP0_buffer[EP0_BUF_SIZE];
__xdata __at (EP1_ADDR) uint8_t EP1_buffer[EP1_BUF_SIZE];
__xdata __at (EP2_ADDR) uint8_t EP2_buffer[EP2_BUF_SIZE];

#define setupBuf ((PUSB_SETUP_REQ)EP0_buffer)

extern __code USB_DEV_DESCR DevDescr;
extern __code uint8_t CfgDescr[];
extern __code uint16_t ManufDescr[];
extern __code uint16_t ProdDescr[];
extern __code uint16_t SerDescr[];
extern volatile uint8_t  SetupReq;
extern volatile uint16_t usb_tx_len;

#define USB_STR_DESCR_i1 (uint8_t*)ManufDescr
#define USB_STR_DESCR_i2 (uint8_t*)ProdDescr
#define USB_STR_DESCR_i3 (uint8_t*)SerDescr

void USBInit(void);
void USBInterrupt(void);
void usb_send_report(uint8_t report[8]);
void usb_isp_jump(void); /* VIA 0x0B: detach USB, wait, LJMP bootloader (never returns) */
void usb_send_mouse(int8_t dx, int8_t dy, uint8_t buttons, int8_t wheel);
