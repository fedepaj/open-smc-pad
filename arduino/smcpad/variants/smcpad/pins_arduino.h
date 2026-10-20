/* SMC-PAD: the raw GPIOs as Arduino pins (the pads, LEDs, keys and encoders have their own
 * library; see docs/hardware.md for what each pin does on the board). */
#pragma once
#include <stdint.h>

#define NUM_DIGITAL_PINS    20
#define NUM_ANALOG_INPUTS   0

/* Arduino pin -> JieLi GPIO (port * 16 + bit) */
static const uint8_t smcpad_pin_to_gpio[NUM_DIGITAL_PINS] = {
    0, 1, 2, 3, 4, 5, 6,                /*  0..6  PA0..PA6 */
    16, 17, 18, 19, 20, 21, 22, 23,     /*  7..14 PB0..PB7 */
    33, 34, 35, 36, 37,                 /* 15..19 PC1..PC5 */
};

#define PIN_PA0 0
#define PIN_PB0 7
#define PIN_PC1 15
