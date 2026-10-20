/*
 * SMC-PAD runtime: the C API the Arduino core (and any firmware on top of the precompiled SDK)
 * calls. Implemented in fw/smcpad/arduino/ and linked into the core object; every symbol
 * declared here is kept global through fw/smcpad/arduino/api.symbols.
 */
#ifndef SMCPAD_RT_H
#define SMCPAD_RT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* time */
uint32_t smcpad_millis(void);
uint32_t smcpad_micros(void);
void smcpad_delay(uint32_t ms);
void smcpad_delay_us(uint32_t us);

/* Services USB, the 1200-baud touch and the bootloader's crash-loop counter. loop() is
 * followed by it; long loops in a sketch should call it (smcpad_delay() does). */
void smcpad_yield(void);

/* USB CDC serial (TinyUSB) */
int smcpad_serial_connected(void);
uint32_t smcpad_serial_write(const uint8_t *buf, uint32_t len);
int smcpad_serial_available(void);
int smcpad_serial_read(void);
int smcpad_serial_peek(void);
void smcpad_serial_flush(void);

/* USB-MIDI (TinyUSB): 4-byte USB-MIDI event packets (cable/CIN, then 3 MIDI bytes) */
int smcpad_usbmidi_write(const uint8_t packet[4]);     /* 1 when queued */
int smcpad_usbmidi_read(uint8_t packet[4]);            /* 1 when one was waiting */
void smcpad_usbmidi_flush(void);

/* persistent storage: 49 keys (0..48), each a blob of up to a few KB, in the SDK's VM (16 KB
 * of flash in all, kept across firmware updates). Return the length read / written, <= 0 on
 * error or when the key was never written. Writing erases flash: not in a tight loop. */
int smcpad_store_read(int key, void *buf, int len);
int smcpad_store_write(int key, const void *buf, int len);

/* GPIO, by JieLi number: port * 16 + bit (PA0 = 0, PB0 = 16, PC0 = 32) */
enum {
    SMCPAD_GPIO_INPUT = 0,
    SMCPAD_GPIO_OUTPUT = 1,
    SMCPAD_GPIO_INPUT_PULLUP = 2,
    SMCPAD_GPIO_INPUT_PULLDOWN = 3,
    SMCPAD_GPIO_ANALOG = 4,
};
void smcpad_gpio_mode(uint32_t gpio, uint32_t mode);
void smcpad_gpio_write(uint32_t gpio, uint32_t value);
int smcpad_gpio_read(uint32_t gpio);

/* ------------------------------------------------------------------ the SMC-PAD hardware
 * (smcpad_hw.c; started before setup()). Numbering:
 *   pads     0..15, 0 = bottom-left, row by row upwards (MPC style: note 36 + pad)
 *   buttons  0..9 in panel order: bluetooth, pad bank, knob bank, left, right, play, pause,
 *            rec, shift, note repeat; their LEDs the same
 *   encoders 0..7 = knobs 1..8 */
#define SMCPAD_NUM_PADS     16
#define SMCPAD_NUM_BUTTONS  10
#define SMCPAD_NUM_ENCODERS 8

enum {
    SMCPAD_EV_PAD_ON = 1,           /* value: velocity 1..127 */
    SMCPAD_EV_PAD_OFF,
    SMCPAD_EV_BUTTON_DOWN,
    SMCPAD_EV_BUTTON_UP,
    SMCPAD_EV_ENCODER,              /* value: +1 / -1 */
};

typedef struct {
    uint8_t type;
    uint8_t index;
    int16_t value;
} smcpad_event_t;

void smcpad_hw_begin(void);
int smcpad_event_get(smcpad_event_t *ev);       /* 1 when one was waiting */
uint16_t smcpad_pad_raw(int pad);               /* 0..1023, 0 at rest */
uint32_t smcpad_pad_rounds(void);               /* ADC passes over all 16 pads so far */
int smcpad_button(int button);                  /* 1 = held */
int32_t smcpad_encoder(int encoder);            /* steps, signed, since start */
uint32_t smcpad_matrix_raw(void);               /* bit (column * 2 + sense) */
void smcpad_led_set(int led, int on);           /* button LEDs */
void smcpad_rgb_set(int pad, uint32_t rgb);     /* 0xRRGGBB; shown by smcpad_rgb_show() */
void smcpad_rgb_show(void);

/* ------------------------------------------------------------------ BLE peripheral
 * (smcpad_ble.c): a GATT server, one central at a time. The ATT database is the caller's, in
 * BTstack's compiled format (entries: size, flags, handle, type (2 or 16 bytes), value; a
 * zero size ends it); its dynamic attributes are served by the callbacks, which run in the
 * BT stack's task: keep them short. CCCs (0x2902, right after their value) are kept here. */
enum {
    SMCPAD_BLE_CONNECTED = 1,       /* handle: the connection */
    SMCPAD_BLE_DISCONNECTED,
    SMCPAD_BLE_SUBSCRIBE,           /* handle: the value; value: the CCC (1 notify, 2 indicate) */
};

typedef struct {
    /* buf NULL: return the value's length; else copy from offset, return the bytes copied */
    uint16_t (*read)(uint16_t handle, uint16_t offset, uint8_t *buf, uint16_t size);
    void (*write)(uint16_t handle, uint16_t offset, const uint8_t *data, uint16_t len);
    void (*event)(int event, uint16_t handle, uint16_t value);
} smcpad_ble_callbacks_t;

void smcpad_ble_begin(const smcpad_ble_callbacks_t *cb);   /* starts the stack; see ready */
int smcpad_ble_ready(void);
void smcpad_ble_set_name(const char *name);
void smcpad_ble_set_profile(const uint8_t *db, uint16_t len);  /* db must stay valid */
void smcpad_ble_set_adv(const uint8_t *adv, uint8_t adv_len, const uint8_t *rsp, uint8_t rsp_len,
                        uint16_t interval, int connectable);    /* AD structures; 0.625 ms */
void smcpad_ble_advertise(int on);                  /* also again after each disconnection */
uint16_t smcpad_ble_connection(void);               /* 0 when none */
uint16_t smcpad_ble_mtu(void);
void smcpad_ble_address(uint8_t addr[6]);           /* little-endian, as on air */
void smcpad_ble_central_address(uint8_t addr[6]);
int smcpad_ble_rssi(void);                          /* 127 when not connected */
int smcpad_ble_notify(uint16_t handle, const uint8_t *data, uint16_t len);   /* 1 when sent */
int smcpad_ble_subscribed(uint16_t handle);         /* the value's CCC */
void smcpad_ble_disconnect(void);

/* system */
void smcpad_enter_bootloader(void);         /* DFU mode of fw/boot; does not return */
void smcpad_reset(void);                    /* does not return */
void smcpad_boot_ok(void);                  /* tell the bootloader this boot works */

#ifdef __cplusplus
}
#endif

#endif
