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

/* system */
void smcpad_enter_bootloader(void);         /* DFU mode of fw/boot; does not return */
void smcpad_reset(void);                    /* does not return */
void smcpad_boot_ok(void);                  /* tell the bootloader this boot works */

#ifdef __cplusplus
}
#endif

#endif
