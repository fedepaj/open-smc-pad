/* TinyUSB configuration for the SMC-PAD (JieLi BR25, full speed, no RTOS integration). */
#ifndef SMCPAD_TUSB_CONFIG_H
#define SMCPAD_TUSB_CONFIG_H

#include "smcpad_usb_cfg.h"

#define CFG_TUSB_MCU                OPT_MCU_NONE
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define SMCPAD_DCD_BR25             1
#define TUP_DCD_ENDPOINT_MAX        5           /* EP0..EP4 */

#define CFG_TUD_ENABLED             SMCPAD_TINYUSB
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_RX_BUFSIZE      1024
#define CFG_TUD_CDC_TX_BUFSIZE      1024
#define CFG_TUD_CDC_EP_BUFSIZE      64

#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                1
#define CFG_TUD_MIDI_RX_BUFSIZE     256
#define CFG_TUD_MIDI_TX_BUFSIZE     256
#define CFG_TUD_MIDI_EP_BUFSIZE     64
#define CFG_TUD_VENDOR              0
#define CFG_TUD_DFU                 0

#endif
