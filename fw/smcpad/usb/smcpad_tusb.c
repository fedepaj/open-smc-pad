/* Glue between TinyUSB and the JieLi SDK environment: interrupt, time base, start. */
#include "system/includes.h"
#include "asm/hwi.h"
#include "tusb.h"

#if SMCPAD_TINYUSB

extern unsigned long jiffies_msec(void);

uint32_t tusb_time_millis_api(void)
{
    return (uint32_t)jiffies_msec();
}

___interrupt
static void usb_isr_tinyusb(void)
{
    tud_int_handler(0);
}

void smcpad_tusb_start(void)
{
    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };

    /* The handler first: tusb_init() pulls D+ up and enables the interrupt, and the host's
     * bus reset can come at once -- into an empty vector otherwise. */
    request_irq(IRQ_USB_CTRL_IDX, 3, usb_isr_tinyusb, 0);
    bit_clr_ie(IRQ_USB_CTRL_IDX);
    tusb_init(0, &init);
}

/* "1200 baud touch", as Arduino boards do it: the host sets 1200 baud and drops DTR. It
 * travels over EP0 only, so it still works when the data endpoints do not -- the way to
 * get a broken build into download mode without a hand on the switch (host/flash_fw.sh). */
volatile int smcpad_usb_touch_1200;

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    cdc_line_coding_t coding;

    (void)itf;
    (void)rts;
    tud_cdc_get_line_coding(&coding);
    if (!dtr && coding.bit_rate == 1200) {
        smcpad_usb_touch_1200 = 1;
    }
}

void smcpad_tusb_task(void)
{
    tud_task();
}

#endif

