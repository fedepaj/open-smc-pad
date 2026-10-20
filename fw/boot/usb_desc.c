/* USB descriptors of the bootloader: one DFU interface (DFU 1.1, DFU mode). */
#include "tusb.h"
#include "boot.h"

#define USB_VID             0x1209      /* pid.codes */
#define USB_PID             0x0002      /* pid.codes test PID; the application is 0x0001 */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,       /* fixed: see BOOT_VERSION in boot.h */
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

#define DFU_ALT_COUNT       3
#define DFU_ATTRS           (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_CAN_UPLOAD | DFU_ATTR_MANIFESTATION_TOLERANT)
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(DFU_ALT_COUNT))

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* interface 0, alternates from string 4, detach timeout 1000 ms */
    TUD_DFU_DESCRIPTOR(0, DFU_ALT_COUNT, 4, DFU_ATTRS, 1000, CFG_TUD_DFU_XFER_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static const char *const strings[] = {
    NULL,                   /* 0: language, handled below */
    "open_smc_pad",
    "SMC-PAD bootloader",
    "0001",
    "app",                  /* DFU alt 0: the application's app area */
    "flash",                /* DFU alt 1: the whole flash, raw, read-only */
    "bootloader",           /* DFU alt 2: bootloader update package (installer + staging) */
};

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    uint8_t n;

    (void)langid;
    if (index == 0) {
        desc_str[1] = 0x0409;
        n = 1;
    } else {
        const char *s;

        if (index >= sizeof(strings) / sizeof(strings[0])) {
            return NULL;
        }
        s = strings[index];
        for (n = 0; s[n] && n < 31; n++) {
            desc_str[1 + n] = (uint8_t)s[n];
        }
    }
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
