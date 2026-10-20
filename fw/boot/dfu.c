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
 */
#include <string.h>
#include "boot.h"
#include "tusb.h"

#define BLOCK               CFG_TUD_DFU_XFER_BUFSIZE        /* one flash sector per block */
#define DFU_ALT_APP         0
#define DFU_ALT_FLASH       1
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

static u16 crc16_update(u16 crc, const u8 *p, u32 n)
{
    while (n--) {
        crc ^= (u16)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
        }
    }
    return crc;
}

static u32 rd32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* The SFC cipher (host/jlimage.py sfc_cipher): per 32-byte block of the mapped address space,
 * an LFSR keystream seeded with chip_key ^ (cpu_offset >> 2).  Symmetric. */
static void sfc_cipher(u8 *buf, u32 len, u32 flash_off)
{
    for (u32 i = 0; i < len; i += 32) {
        u32 key = chip_key ^ ((flash_off + i - SFC_FLASH_BASE) >> 2);
        u32 n = len - i < 32 ? len - i : 32;

        for (u32 j = 0; j < n; j++) {
            buf[i + j] ^= (u8)key;
            key = ((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
        }
    }
}

static int dir_ok(const u8 *d, u32 len)
{
    u32 size = rd32(d + DIR_SIZE);

    return len >= 0x120 && crc16_update(0, d + 2, 30) == (d[0] | d[1] << 8) &&
           rd32(d + DIR_OFFSET) == APP_ENTRY && size > 0x120 && size <= APP_AREA_MAX;
}

/* Erase the sector at `off` (relative to APP_DIR) and program `len` plaintext bytes there,
 * encrypted, then read back and compare.  `buf` is clobbered. */
static int write_sector(u32 off, u8 *buf, u32 len)
{
    u32 addr = APP_DIR + off;
    u8 check[64];

    sfc_cipher(buf, len, addr);
    if (flash_erase_sector(addr)) {
        return -1;
    }
    for (u32 a = 0; a < len; a += PAGE_SIZE) {
        u32 n = len - a < PAGE_SIZE ? len - a : PAGE_SIZE;

        if (flash_program(addr + a, buf + a, n)) {
            return -2;
        }
        wdt_kick();
    }
    for (u32 a = 0; a < len; a += sizeof(check)) {
        u32 n = len - a < sizeof(check) ? len - a : sizeof(check);

        if (flash_read(addr + a, check, n) || memcmp(check, buf + a, n)) {
            return -3;
        }
    }
    return 0;
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

void tud_dfu_download_cb(uint8_t alt, uint16_t block_num, uint8_t const *data, uint16_t length)
{
    BOOT_DIAG->events |= 4;
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
    if (write_sector(next_off, work, length)) {
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
    if (alt != DFU_ALT_APP) {
        fail(0x200, DFU_STATUS_ERR_WRITE);
        return;
    }
    if (next_off == ~0u || next_off < area_size || area_crc != area_crc_want) {
        fail(0x201, DFU_STATUS_ERR_VERIFY);         /* incomplete or corrupted: stay here */
        return;
    }
    memcpy(work, dir_block, dir_len);
    if (write_sector(0, work, dir_len)) {
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
    if (flash_read(APP_DIR, head, sizeof(head))) {
        return 0;
    }
    sfc_cipher(head, sizeof(head), APP_DIR);
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
    sfc_cipher(data, length, APP_DIR + off);
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

    for (u32 polls = 0;; polls++) {
        wdt_kick();
        dcd_int_handler(0);
        tud_task();
        u32 ms = millis();

        if ((polls & 0xFF) == 0) {
            d->ms = ms;
            d->polls = polls;
        }
        if (reboot_at && (int32_t)(ms - reboot_at) >= 0) {
            reboot();
        }
        /* Asked for by the application but nobody came: back to it. */
        if (app_ok && !tud_mounted() && ms > UNMOUNTED_GIVE_UP_MS) {
            reboot();
        }
    }
}
