/* SMC-PAD runtime (smcpad_rt.h): time, yield, USB serial, the way into the bootloader. Shared by
 * the bring-up console and the Arduino core. */
#include "system/includes.h"
#include "asm/usb.h"
#include "smcpad_usb_cfg.h"
#include "smcpad_rt.h"
#include "tusb.h"

extern void clr_wdt(void);
extern unsigned long jiffies_half_msec(void);

/* ------------------------------------------------------------------ bootloader contract
 * fw/boot/boot.h: the DFU request word at UPDATA_BEG, and the crash-loop counter the
 * bootloader keeps (count, count ^ check) -- cleared here once the application has run for a
 * few seconds. */
#define BOOT_MAGIC_ADDR     0x31AE0u
#define BOOT_MAGIC_DFU      0xB007DF00u
#define BOOT_COUNT_ADDR     0x31AD8u
#define BOOT_OK_AFTER_MS    3000u

void smcpad_boot_ok(void)
{
    ((volatile u32 *)BOOT_COUNT_ADDR)[0] = 0;
    ((volatile u32 *)BOOT_COUNT_ADDR)[1] = 0;
}

void smcpad_reset(void)
{
    local_irq_disable();
    JL_USB->CON0 = 0;
    JL_USB_IO->CON0 &= ~(BIT(DPPU) | BIT(DMPU) | BIT(DPPD) | BIT(DMPD));
    for (volatile u32 i = 0; i < 3000000; i++) {
    }
    cpu_reset();
    for (;;) {
    }
}

/* Off the bus first (PHY off, pulls off), so the host sees us go before the bootloader comes. */
void smcpad_enter_bootloader(void)
{
#if SMCPAD_APP_DIR != 0x1000
    *(volatile u32 *)BOOT_MAGIC_ADDR = BOOT_MAGIC_DFU;
#endif
    smcpad_reset();
}

/* The 1200-baud touch, seen by smcpad_tusb.c's line-state callback through
 * smcpad_dbg_read() and smcpad_yield(). */
extern volatile int smcpad_usb_touch_1200;

void smcpad_on_touch_1200(void)
{
    smcpad_enter_bootloader();
}

/* ------------------------------------------------------------------ time */

uint32_t smcpad_millis(void)
{
    return jiffies_half_msec() >> 1;
}

uint32_t smcpad_micros(void)
{
    return jiffies_half_msec() * 500u;      /* 0.5 ms resolution until a µs timer is wired */
}

void smcpad_yield(void)
{
    static u8 boot_confirmed;

#if SMCPAD_TINYUSB
    tud_task();
    if (smcpad_usb_touch_1200) {
        smcpad_on_touch_1200();
    }
#endif
    if (!boot_confirmed && smcpad_millis() > BOOT_OK_AFTER_MS) {
        smcpad_boot_ok();
        boot_confirmed = 1;
    }
    clr_wdt();
}

void smcpad_delay(uint32_t ms)
{
    uint32_t start = smcpad_millis();

    while (smcpad_millis() - start < ms) {
        smcpad_yield();
    }
}

void smcpad_delay_us(uint32_t us)
{
    /* busy loop, calibrated for the 24 MHz system clock (coarse) */
    for (volatile uint32_t i = 0; i < us * 4; i++) {
    }
}

/* ------------------------------------------------------------------ USB serial */
#if SMCPAD_TINYUSB

int smcpad_serial_connected(void)
{
    return tud_cdc_connected();
}

/* Blocks while the host is reading; drops what is left after ~100 ms without progress or
 * when no terminal has the port open, so a sketch never hangs on a closed port. */
uint32_t smcpad_serial_write(const uint8_t *buf, uint32_t len)
{
    uint32_t done = 0, idle_since = smcpad_millis();

    tud_task();
    if (!tud_cdc_connected()) {
        return len;
    }
    while (done < len && smcpad_millis() - idle_since < 100) {
        uint32_t n = tud_cdc_write(buf + done, len - done);

        if (n) {
            done += n;
            idle_since = smcpad_millis();
        }
        tud_cdc_write_flush();
        smcpad_yield();
    }
    return len;
}

int smcpad_serial_available(void)
{
    tud_task();
    return (int)tud_cdc_available();
}

int smcpad_serial_read(void)
{
    uint8_t c;

    tud_task();
    return tud_cdc_read(&c, 1) ? c : -1;
}

int smcpad_serial_peek(void)
{
    uint8_t c;

    tud_task();
    return tud_cdc_peek(&c) ? c : -1;
}

void smcpad_serial_flush(void)
{
    tud_cdc_write_flush();
    tud_task();
}

#endif
