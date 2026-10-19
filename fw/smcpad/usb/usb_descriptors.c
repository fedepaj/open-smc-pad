/* USB descriptors for the TinyUSB build: one CDC ACM function, the bring-up console. */
#include "tusb.h"

#if SMCPAD_TINYUSB

/* pid.codes test VID/PID, for development only. */
#define USB_VID     0x1209
#define USB_PID     0x0001

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_TOTAL };

/* IN and OUT on different endpoint numbers: one number for both directions shares the
 * controller's FIFO and DMA area, and IN packets then went out of the OUT side's buffer. */
#define EP_CDC_NOTIF    0x81
#define EP_CDC_OUT      0x02
#define EP_CDC_IN       0x83
#define CONFIG_LEN      (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_config;
}

static const char *const strings[] = {
    NULL,
    "open_smc_pad",
    "SMC-PAD open fw",
    "0001",
    "SMC-PAD console",
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc[32];
    uint8_t n;

    (void)langid;
    if (index == 0) {
        desc[1] = 0x0409;
        n = 1;
    } else {
        const char *s;

        if (index >= sizeof(strings) / sizeof(strings[0])) {
            return NULL;
        }
        s = strings[index];
        for (n = 0; s[n] && n < 31; n++) {
            desc[1 + n] = (uint8_t)s[n];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc;
}

#endif
