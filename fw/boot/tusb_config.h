/* TinyUSB configuration of the bootloader: DFU only, polled, EP0 only. */
#ifndef SMCPAD_BOOT_TUSB_CONFIG_H
#define SMCPAD_BOOT_TUSB_CONFIG_H

#define CFG_TUSB_MCU                OPT_MCU_NONE
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define SMCPAD_DCD_BR25             1
#define TUP_DCD_ENDPOINT_MAX        2

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_DFU                 1
#define CFG_TUD_DFU_XFER_BUFSIZE    4096        /* one flash sector per DFU block */

#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#endif
