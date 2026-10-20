/* The Arduino entry: C++ constructors, setup(), then loop() forever. Runs in the "smcpad_con"
 * task that app_idle.c creates (the console's task in the bring-up firmware). setup() and loop()
 * come from the sketch, linked against the precompiled core object (fw/arduino). */
#include "system/includes.h"
#include "smcpad_usb_cfg.h"
#include "smcpad_rt.h"

#if SMCPAD_ARDUINO

extern void setup(void);
extern void loop(void);

/* from the core's linker script (fw/arduino: sdk_core.ld) */
typedef void (*ctor_t)(void);
extern ctor_t __init_array_start[] __attribute__((weak));
extern ctor_t __init_array_end[] __attribute__((weak));

void smcpad_arduino_run(void)
{
    for (ctor_t *c = __init_array_start; c && c < __init_array_end; c++) {
        (*c)();
    }
    setup();
    for (;;) {
        loop();
        smcpad_yield();
    }
}

#endif
