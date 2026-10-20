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

/* system */
void smcpad_enter_bootloader(void);         /* DFU mode of fw/boot; does not return */
void smcpad_reset(void);                    /* does not return */
void smcpad_boot_ok(void);                  /* tell the bootloader this boot works */

#ifdef __cplusplus
}
#endif

#endif
