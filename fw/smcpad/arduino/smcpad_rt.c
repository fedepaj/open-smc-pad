/* SMC-PAD runtime (smcpad_rt.h): time, yield, USB serial, the way into the bootloader. Shared by
 * the bring-up console and the Arduino core. */
#include "system/includes.h"
#include "asm/usb.h"
#include "asm/gpio.h"
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

/* smcpad_millis(): smcpad_hw.c (TIMER3) */

/* smcpad_micros(), smcpad_delay_us(): smcpad_hw.c (TIMER3) */

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

/* At least ms milliseconds, counted on micros (as AVR does): millis steps would end it up to
 * a millisecond early. */
void smcpad_delay(uint32_t ms)
{
    uint32_t start = smcpad_micros();

    while (ms > 0) {
        smcpad_yield();
        while (ms > 0 && smcpad_micros() - start >= 1000) {
            ms--;
            start += 1000;
        }
    }
}


/* ------------------------------------------------------------------ storage
 * The SDK's VM (flash 0x71000, 16 KB, outside the application area: kept across DFU updates),
 * user items 1-49 (syscfg_id.h). Not from interrupts. */
#include "system/syscfg_id.h"

#define STORE_KEYS  (CFG_USER_DEFINE_END - CFG_USER_DEFINE_BEGIN + 1)

int smcpad_store_read(int key, void *buf, int len)
{
    if (key < 0 || key >= STORE_KEYS || len <= 0 || len > 0xFFFF) {
        return -1;
    }
    return syscfg_read(CFG_USER_DEFINE_BEGIN + key, buf, len);
}

int smcpad_store_write(int key, const void *buf, int len)
{
    if (key < 0 || key >= STORE_KEYS || len <= 0 || len > 0xFFFF) {
        return -1;
    }
    return syscfg_write(CFG_USER_DEFINE_BEGIN + key, (void *)buf, len);
}

/* ------------------------------------------------------------------ GPIO */

void smcpad_gpio_mode(uint32_t gpio, uint32_t mode)
{
    gpio_set_pull_up(gpio, mode == SMCPAD_GPIO_INPUT_PULLUP);
    gpio_set_pull_down(gpio, mode == SMCPAD_GPIO_INPUT_PULLDOWN);
    if (mode == SMCPAD_GPIO_OUTPUT) {
        gpio_set_direction(gpio, 0);
        return;
    }
    gpio_set_direction(gpio, 1);
    gpio_set_die(gpio, mode != SMCPAD_GPIO_ANALOG);
}

void smcpad_gpio_write(uint32_t gpio, uint32_t value)
{
    gpio_set_output_value(gpio, value ? 1 : 0);
}

int smcpad_gpio_read(uint32_t gpio)
{
    return gpio_read(gpio);
}

/* ------------------------------------------------------------------ USB serial */
#if SMCPAD_TINYUSB

int smcpad_usbmidi_write(const uint8_t packet[4])
{
    tud_task();
    return tud_mounted() && tud_midi_packet_write(packet);
}

int smcpad_usbmidi_read(uint8_t packet[4])
{
    tud_task();
    return tud_midi_available() && tud_midi_packet_read(packet);
}

void smcpad_usbmidi_flush(void)
{
    tud_task();
}

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
