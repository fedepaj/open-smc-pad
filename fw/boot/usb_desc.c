/* USB descriptors of the bootloader: one DFU interface (DFU 1.1, DFU mode), and the Microsoft OS
 * 2.0 descriptors that make Windows bind WinUSB to it by itself (dfu-util needs WinUSB there; no
 * Zadig). USB 2.1 (bcdUSB 0x0210) is what makes Windows ask for the BOS descriptor.
 * Note: any change here is a "new accessory" to macOS on Apple silicon, which asks the user to
 * allow it once (docs/bootloader.md): change these only on purpose. */
#include "tusb.h"
#include "boot.h"

#define USB_VID             0x1209      /* pid.codes */
#define USB_PID             0x0002      /* pid.codes test PID; the application is 0x0001 */

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,       /* 2.1: BOS descriptor (Microsoft OS 2.0) */
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

/* ------------------------------------------------------------------ Microsoft OS 2.0
 * A BOS platform capability tells Windows to fetch the descriptor set below with a vendor
 * request; the set says: bind WinUSB, and publish this device interface GUID (for libusb /
 * dfu-util). One interface, so no configuration / function subsets. */
#define MS_OS_20_VENDOR_CODE    0x01
#define MS_OS_20_DESC_LEN       0xA2
#define BOS_TOTAL_LEN           (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, MS_OS_20_VENDOR_CODE),
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return desc_bos;
}

#define W(c) (c), 0                 /* UTF-16LE */

static const uint8_t desc_ms_os_20[MS_OS_20_DESC_LEN] = {
    /* set header: length, type, Windows version (8.1+), total length */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    /* compatible ID: WINUSB */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) = our interface GUID */
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    W('D'), W('e'), W('v'), W('i'), W('c'), W('e'), W('I'), W('n'), W('t'), W('e'), W('r'),
    W('f'), W('a'), W('c'), W('e'), W('G'), W('U'), W('I'), W('D'), W('s'), W(0),
    U16_TO_U8S_LE(0x0050),
    W('{'), W('7'), W('d'), W('2'), W('7'), W('9'), W('a'), W('7'), W('1'), W('-'),
    W('4'), W('e'), W('2'), W('7'), W('-'), W('4'), W('b'), W('c'), W('b'), W('-'),
    W('9'), W('c'), W('a'), W('9'), W('-'), W('6'), W('a'), W('8'), W('c'), W('9'),
    W('3'), W('b'), W('5'), W('6'), W('6'), W('3'), W('0'), W('}'), W(0), W(0),
};

/* Windows: GET_MS_OS_20_DESCRIPTOR (our vendor code, wIndex 7) */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bRequest == MS_OS_20_VENDOR_CODE && request->wIndex == 7) {
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20));
    }
    return false;
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
