/* Time, pins and the other Common.h functions, on the runtime API (smcpad_rt.h). */
#include "Arduino.h"

extern "C" {

unsigned long millis(void) { return smcpad_millis(); }
unsigned long micros(void) { return smcpad_micros(); }
void delay(unsigned long ms) { smcpad_delay(ms); }
void delayMicroseconds(unsigned int us) { smcpad_delay_us(us); }
void yield(void) { smcpad_yield(); }

static uint32_t to_gpio(pin_size_t pin)
{
    return pin < NUM_DIGITAL_PINS ? smcpad_pin_to_gpio[pin] : 0xFF;
}

void pinMode(pin_size_t pin, PinMode mode)
{
    uint32_t g = to_gpio(pin);

    if (g == 0xFF) {
        return;
    }
    switch (mode) {
    case OUTPUT: smcpad_gpio_mode(g, SMCPAD_GPIO_OUTPUT); break;
    case INPUT_PULLUP: smcpad_gpio_mode(g, SMCPAD_GPIO_INPUT_PULLUP); break;
    case INPUT_PULLDOWN: smcpad_gpio_mode(g, SMCPAD_GPIO_INPUT_PULLDOWN); break;
    default: smcpad_gpio_mode(g, SMCPAD_GPIO_INPUT); break;
    }
}

void digitalWrite(pin_size_t pin, PinStatus status)
{
    uint32_t g = to_gpio(pin);

    if (g != 0xFF) {
        smcpad_gpio_write(g, status == HIGH);
    }
}

PinStatus digitalRead(pin_size_t pin)
{
    uint32_t g = to_gpio(pin);

    return g != 0xFF && smcpad_gpio_read(g) ? HIGH : LOW;
}

/* not wired yet: the pads have their own library (SMCPad) */
int analogRead(pin_size_t) { return 0; }
void analogReference(uint8_t) {}
void analogWrite(pin_size_t pin, int value) { digitalWrite(pin, value >= 128 ? HIGH : LOW); }

void attachInterrupt(pin_size_t, voidFuncPtr, PinStatus) {}
void attachInterruptParam(pin_size_t, voidFuncPtrParam, PinStatus, void *) {}
void detachInterrupt(pin_size_t) {}

void init(void) {}
void initVariant(void) {}

}

/* WMath: random numbers (xorshift32; randomSeed(0) is ignored, as on AVR) */
static uint32_t random_state = 2463534242u;

void randomSeed(unsigned long seed)
{
    if (seed != 0) {
        random_state = seed;
    }
}

static uint32_t random_next(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

long random(long howbig)
{
    return howbig <= 0 ? 0 : (long)(random_next() % (uint32_t)howbig);
}

long random(long howsmall, long howbig)
{
    return howsmall >= howbig ? howsmall : howsmall + random(howbig - howsmall);
}
