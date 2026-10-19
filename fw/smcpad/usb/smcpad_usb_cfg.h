/* Which USB stack the firmware uses: 1 = TinyUSB with our BR25 driver (usb/dcd_br25.c),
 * 0 = the JieLi SDK's. fw/build.sh can override it (SMCPAD_TINYUSB=0|1). */
#ifndef SMCPAD_USB_CFG_H
#define SMCPAD_USB_CFG_H
#include "smcpad_usb_build.h"               /* written by fw/build.sh */
#ifndef SMCPAD_TINYUSB
#define SMCPAD_TINYUSB 0
#endif
#endif
