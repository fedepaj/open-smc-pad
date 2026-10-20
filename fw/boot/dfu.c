/* SMC-PAD bootloader, DFU mode (runs from RAM, interrupts off, USB polled).
 *
 * One DFU alternate setting: the application's app area, i.e. what host/package_fw.sh builds
 * as work/flash/fw_area.bin -- the JieLi directory + app.bin + cfg_tool.bin, plaintext, laid
 * out for flash APP_DIR.  The bootloader does the SFC encryption itself (key = the chip id the
 * uboot passed in), so the same file goes on any pad.
 *
 * Safe against interruption: the first block (the directory) is checked and kept in RAM, its
 * sector is erased at once -- from then on the bootloader no longer sees a valid application
 * and stays in DFU mode on every boot -- and it is written last, at manifestation, only when
 * the whole area has arrived and its CRC (the directory's own data_crc) matches.
 *
 * Upload returns the area as it is in flash, decrypted, so `dfu-util -U` reads back exactly
 * the file that was downloaded.
 *
 * Alternate setting 1 ("flash") is read-only: the whole chip as raw physical bytes, the same
 * as a download-mode read -- a backup without the Pico.
 *
 * Alternate setting 2 ("bootloader") updates the bootloader itself, through the installer
 * (fw/boot/install.c): see boot_pkg_block() / boot_pkg_manifest().
 */
#include <string.h>
#include "boot.h"
#include "tusb.h"

#define BLOCK               CFG_TUD_DFU_XFER_BUFSIZE        /* one flash sector per block */
#define DFU_ALT_APP         0
#define DFU_ALT_FLASH       1
#define DFU_ALT_BOOT        2
#define BOOT_PKG_MAX        (STAGE_END - INSTALL_DIR)
#define FLASH_SIZE          0x80000u

#define USB_CON0            REG32(0x1E1800)
#define USB_IO_CON0         REG32(0x1E5100)
#define CLK_CON1            REG32(0x1E0010)                 /* JL_CLOCK->CLK_CON1 */
#define PLL_CON             REG32(0x1E0040)

/* Timer 0 as the time base, polled: 24 MHz oscillator, 1 ms period (SDK handshake_timer.c). */
#define T0_CON              REG32(0x1E0400)
#define T0_CNT              REG32(0x1E0404)
#define T0_PRD              REG32(0x1E0408)
#define T_EN                (1u << 0)
#define T_SRC_OSC           (1u << 3)
#define T_PCLR              (1u << 14)
#define T_PND               (1u << 15)

#define UNMOUNTED_GIVE_UP_MS    20000u      /* entered on request, host never configured us */
#define REBOOT_DELAY_MS         1500u       /* after a good manifestation, if nobody detaches */

static u32 chip_key;
static u32 app_ok;                          /* the flash holds a complete application */
static u32 now_ms;
static u32 reboot_at;                       /* 0 = no reboot scheduled */

/* download state */
static u8 dir_block[BLOCK] __attribute__((aligned(4)));    /* block 0, written last */
static u32 dir_len;
static u8 work[BLOCK] __attribute__((aligned(4)));
static u32 next_off;                        /* where the next block goes; ~0 = refused */
static u32 area_size, area_crc_want;
static u16 area_crc;
static int short_block_seen;
static u32 boot_next;                       /* alt 2: next offset into the package; ~0 = refused */

static u32 millis(void)
{
    if (T0_CON & T_PND) {
        T0_CON |= T_PCLR;
        now_ms++;
    }
    return now_ms;
}

uint32_t tusb_time_millis_api(void)
{
    return millis();
}

static int dir_ok(const u8 *d, u32 len)
{
    u32 size = rd32(d + DIR_SIZE);

    return len >= 0x120 && crc16_update(0, d + 2, 30) == (d[0] | d[1] << 8) &&
           rd32(d + DIR_OFFSET) == APP_ENTRY && size > 0x120 && size <= APP_AREA_MAX;
}

/* Encrypt `len` plaintext bytes (buf is clobbered) for the application sector at `off` and
 * write them there, verified. */
static int write_app_sector(u32 off, u8 *buf, u32 len)
{
    sfc_cipher(buf, len, APP_DIR + off, chip_key);
    return flash_write_sector(APP_DIR + off, buf, len);
}

static void fail(u32 code, uint8_t status)
{
    BOOT_DIAG->last_error = code;
    next_off = ~0u;
    tud_dfu_finish_flashing(status);
}

/* ------------------------------------------------------------------ TinyUSB DFU callbacks */

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
    (void)alt;
    return state == DFU_DNBUSY ? 60 : 20;   /* a sector erase + 16 pages; the manifestation */
}

/* Alt 2: a bootloader update package -- the image of INSTALL_DIR..STAGE_END, plaintext: the
 * installer's app area (encrypted here as it is written) and, from STAGE on, the staging area
 * (header + the new bootloader's app area, stored as is). Nothing outside that range is
 * touched until the manifestation has checked all of it. */
static void boot_pkg_block(uint16_t block_num, uint8_t const *data, uint16_t length)
{
    u32 off = (u32)block_num * BLOCK;

    if (block_num == 0) {
        boot_next = 0;
        app_ok = 0;                         /* a large application may reach into the slot */
    }
    if (boot_next != off || off + length > BOOT_PKG_MAX || (length & 31)) {
        boot_next = ~0u;
        fail(0x301, DFU_STATUS_ERR_ADDRESS);
        return;
    }
    memcpy(work, data, length);
    if (INSTALL_DIR + off < STAGE) {
        sfc_cipher(work, length, INSTALL_DIR + off, chip_key);
    }
    if (flash_write_sector(INSTALL_DIR + off, work, length)) {
        boot_next = ~0u;
        fail(0x302, DFU_STATUS_ERR_WRITE);
        return;
    }
    boot_next = off + length;
    BOOT_DIAG->blocks++;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

/* Everything checked, then sector 0 points at the installer and we reset into it: it puts the
 * new bootloader at 0x1000 (not touching the application) and points sector 0 back. */
static void boot_pkg_manifest(void)
{
    struct stage_header h;

    if (boot_next == ~0u || boot_next < STAGE - INSTALL_DIR) {
        fail(0x311, DFU_STATUS_ERR_VERIFY);
        return;
    }
    if (!area_check(INSTALL_DIR, SFC_CPU_BASE + 0x120, STAGE - INSTALL_DIR, 1, chip_key)) {
        fail(0x312, DFU_STATUS_ERR_VERIFY);         /* not an installer for the slot */
        return;
    }
    if (stage_check(&h) || h.erase_end != DEST_ERASE_UPDATE) {
        fail(0x313, DFU_STATUS_ERR_VERIFY);         /* no good bootloader in it, or a stock install */
        return;
    }
    if (set_boot_dir(INSTALL_DIR)) {
        fail(0x314, DFU_STATUS_ERR_WRITE);
        return;
    }
    BOOT_DIAG->events |= 16;
    reboot_at = millis() + 300;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length)
{
    BOOT_DIAG->events |= 4;
    if (alt == DFU_ALT_BOOT) {
        boot_pkg_block(block_num, data, length);
        return;
    }
    if (alt != DFU_ALT_APP) {
        fail(0x100, DFU_STATUS_ERR_WRITE);          /* the raw flash is read-only */
        return;
    }

    if (block_num == 0) {
        if (!dir_ok(data, length)) {
            fail(0x101, DFU_STATUS_ERR_TARGET);     /* not an app area for APP_DIR */
            return;
        }
        memcpy(dir_block, data, length);
        dir_len = length;
        area_size = rd32(data + DIR_SIZE);
        area_crc_want = data[DIR_DATA_CRC] | data[DIR_DATA_CRC + 1] << 8;
        area_crc = crc16_update(0, data + 0x20, (area_size < length ? area_size : length) - 0x20);
        short_block_seen = length < BLOCK;
        /* From here on the flash holds no valid application until the manifestation. */
        app_ok = 0;
        if (flash_erase_sector(APP_DIR)) {
            fail(0x102, DFU_STATUS_ERR_ERASE);
            return;
        }
        next_off = length;
        BOOT_DIAG->blocks = 1;
        tud_dfu_finish_flashing(DFU_STATUS_OK);
        return;
    }

    if (next_off == ~0u || short_block_seen || next_off != (u32)block_num * BLOCK) {
        fail(0x103, DFU_STATUS_ERR_ADDRESS);        /* out of order, or after a short block */
        return;
    }
    if (next_off + length > area_size) {
        fail(0x104, DFU_STATUS_ERR_ADDRESS);        /* longer than the directory says */
        return;
    }
    area_crc = crc16_update(area_crc, data, length);
    memcpy(work, data, length);
    if (write_app_sector(next_off, work, length)) {
        fail(0x105, DFU_STATUS_ERR_WRITE);
        return;
    }
    short_block_seen = length < BLOCK;
    next_off += length;
    BOOT_DIAG->blocks++;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

void tud_dfu_manifest_cb(uint8_t alt)
{
    if (alt == DFU_ALT_BOOT) {
        boot_pkg_manifest();
        return;
    }
    if (alt != DFU_ALT_APP) {
        fail(0x200, DFU_STATUS_ERR_WRITE);
        return;
    }
    if (next_off == ~0u || next_off < area_size || area_crc != area_crc_want) {
        fail(0x201, DFU_STATUS_ERR_VERIFY);         /* incomplete or corrupted: stay here */
        return;
    }
    memcpy(work, dir_block, dir_len);
    if (write_app_sector(0, work, dir_len)) {
        fail(0x202, DFU_STATUS_ERR_WRITE);
        return;
    }
    app_ok = 1;
    BOOT_DIAG->events |= 8;
    reboot_at = millis() + REBOOT_DELAY_MS;
    tud_dfu_finish_flashing(DFU_STATUS_OK);
}

uint16_t tud_dfu_upload_cb(uint8_t alt, uint16_t block_num, uint8_t *data, uint16_t length)
{
    u32 off = (u32)block_num * BLOCK, size;
    u8 head[32];

    if (alt == DFU_ALT_FLASH) {             /* the whole chip, raw, as download mode reads it */
        if (off >= FLASH_SIZE) {
            return 0;
        }
        if (length > FLASH_SIZE - off) {
            length = (uint16_t)(FLASH_SIZE - off);
        }
        return flash_read(off, data, length) ? 0 : length;
    }
    if (alt == DFU_ALT_BOOT) {              /* the version line, e.g. "smcpad-boot 0106 Oct 20 2026" */
        static const char hex[] = "0123456789abcdef";
        static const char tail[] = " " __DATE__ " " __TIME__ "\n";
        static const char head_s[] = "smcpad-boot ";
        u32 n = 0;

        if (block_num != 0) {
            return 0;
        }
        for (u32 i = 0; head_s[i] && n < length; i++) {
            data[n++] = (uint8_t)head_s[i];
        }
        for (int sh = 12; sh >= 0 && n < length; sh -= 4) {
            data[n++] = (uint8_t)hex[(BOOT_VERSION >> sh) & 0xF];
        }
        for (u32 i = 0; tail[i] && n < length; i++) {
            data[n++] = (uint8_t)tail[i];
        }
        return (uint16_t)n;
    }
    if (alt != DFU_ALT_APP || flash_read(APP_DIR, head, sizeof(head))) {
        return 0;
    }
    sfc_cipher(head, sizeof(head), APP_DIR, chip_key);
    size = rd32(head + DIR_SIZE);
    if (crc16_update(0, head + 2, 30) != (head[0] | head[1] << 8) || size > APP_AREA_MAX ||
        off >= size) {
        return 0;
    }
    if (length > size - off) {
        length = (uint16_t)(size - off);
    }
    if (flash_read(APP_DIR + off, data, length)) {
        return 0;
    }
    sfc_cipher(data, length, APP_DIR + off, chip_key);
    return length;
}

void tud_dfu_detach_cb(void)
{
    reboot_at = millis() + 50;
}

void tud_dfu_abort_cb(uint8_t alt)
{
    (void)alt;
    next_off = ~0u;
}

void tud_mount_cb(void)
{
    BOOT_DIAG->events |= 2;
}

/* ------------------------------------------------------------------------------- main */

/* The SHIFT button's LED blinks while the bootloader waits (docs/hardware.md, button LEDs: a
 * 3x4 multiplex, phases PA3/PC2/PC3 and segments PA5/PA2/PA0/PA1, all active low; SHIFT is
 * phase 0 = PA3, segment 2 = PA0). */
#define PORTA(o)            REG32(0x1E5000u + (o))
#define PORTC(o)            REG32(0x1E5080u + (o))
#define LED_A_PINS          ((1u << 3) | (1u << 5) | (1u << 2) | (1u << 0) | (1u << 1))
#define LED_C_PINS          ((1u << 2) | (1u << 3))

static void led_init(void)
{
    PORTA(0x10) &= ~LED_A_PINS;             /* no pulls */
    PORTA(0x14) &= ~LED_A_PINS;
    PORTC(0x10) &= ~LED_C_PINS;
    PORTC(0x14) &= ~LED_C_PINS;
    PORTA(0x00) |= LED_A_PINS;              /* everything off */
    PORTC(0x00) |= LED_C_PINS;
    PORTA(0x08) &= ~LED_A_PINS;
    PORTC(0x08) &= ~LED_C_PINS;
    PORTA(0x00) &= ~(1u << 0);              /* SHIFT's segment sinks; its phase blinks */
}

static void led_set(int on)
{
    if (on) {
        PORTA(0x00) &= ~(1u << 3);
    } else {
        PORTA(0x00) |= 1u << 3;
    }
}

static __attribute__((noreturn)) void reboot(void)
{
    /* Off the bus first, the way the application's `J` does it, so the host sees a detach. */
    USB_CON0 = 0;
    USB_IO_CON0 &= ~((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7));
    for (u32 t = millis(); millis() - t < 200; ) {
        wdt_kick();
    }
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}

void dfu_main(u32 key, u32 reason, u32 valid)
{
    volatile struct boot_diag *d = BOOT_DIAG;
    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };

    ROM_IRQ_DISABLE();
    wdt_kick();
    chip_key = key;
    app_ok = valid;
    next_off = ~0u;
    boot_next = ~0u;

    T0_CON = T_PCLR;
    T0_CNT = 0;
    T0_PRD = 24000;
    T0_CON = T_EN | T_SRC_OSC | T_PCLR;

    memset((void *)d, 0, sizeof(*d));
    d->magic = BOOT_DIAG_MAGIC | reason;
    d->clk_con1 = CLK_CON1;
    d->pll_con = PLL_CON;
    d->last_error = flash_begin();          /* the flash is data from now on */

    tusb_init(0, &init);
    d->usb_con0 = USB_CON0;
    led_init();

    for (u32 polls = 0;; polls++) {
        wdt_kick();
        dcd_int_handler(0);
        tud_task();
        u32 ms = millis();

        if ((polls & 0xFF) == 0) {
            d->ms = ms;
            d->polls = polls;
        }
        led_set((ms / 500) & 1);
        if (reboot_at && (int32_t)(ms - reboot_at) >= 0) {
            reboot();
        }
        /* Asked for by the application but nobody came: back to it. */
        if (app_ok && !tud_mounted() && ms > UNMOUNTED_GIVE_UP_MS) {
            reboot();
        }
    }
}
