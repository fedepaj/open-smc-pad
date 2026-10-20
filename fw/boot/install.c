/* SMC-PAD installer: replaces the stock firmware with our bootloader, on a pad that only has
 * the stock firmware -- no Pico, no download mode.
 *
 * host/install_from_stock.py, talking to the *stock* firmware over USB-MIDI (SINCO), writes
 * this installer as an app area at flash INSTALL_DIR, the bootloader's app area (plaintext)
 * into a staging area behind it, and points sector 0's app_dir_head at INSTALL_DIR.  At the
 * next power-up the uboot runs us instead of the stock firmware, which never runs again:
 *
 *   1. check the staging area: header CRC, payload CRC32, and the payload being an app area
 *      for 0x1000 whose own directory CRCs hold;
 *      bad -> point sector 0 back at 0x1000 and reset: the stock firmware, still intact,
 *      boots again and the host can retry.
 *   2. erase the stock firmware (0x1000..0x26000), write the bootloader there, encrypted with
 *      the chip key, and read it back;
 *      a failure or a power cut here just runs the installer again at the next boot.
 *   3. point sector 0's app_dir_head back at 0x1000 (erase + program + verify; the only window
 *      in which losing power needs the Pico), erase the installer and staging, reset.
 *
 * The bootloader then finds no application and comes up in DFU mode for host/dfu_flash.sh.
 *
 * Like the bootloader, the part in .start runs in place and makes nothing but the move to
 * RAM; everything else runs from RAM with the SFC parked (flash.c).
 */
#include <string.h>
#include "boot.h"

#define INSTALL_DIR         0x41000u            /* this installer's app area (see install.ld) */
#define STAGE               0x43000u            /* staging header; the payload follows */
#define STAGE_END           0x4D000u
#define STAGE_MAGIC         0x49434D53u         /* "SMCI" */
#define DEST                0x1000u             /* where the payload goes */
#define DEST_ERASE_END      0x26000u            /* the whole stock app + resource area */
#define DEST_MAX            (APP_DIR - DEST)    /* the bootloader's area must end before the app */

/* What the installer did, for the application's `I` (next to the bootloader's diagnostics). */
#define INSTALL_DIAG        ((volatile u32 *)0x31AC8u)
#define INSTALL_DIAG_MAGIC  0x1A57A000u         /* | step << 4 | result */

struct stage_header {
    u32 magic;
    u32 version;                /* 1 */
    u32 payload_off;            /* from STAGE */
    u32 payload_len;
    u32 payload_crc32;
    u32 dest;                   /* DEST */
    u32 erase_end;              /* DEST_ERASE_END */
    u32 hdr_crc32;              /* over the 28 bytes above */
};

#define START   __attribute__((section(".start"), noinline))

/* Breadcrumb in RAM that survives a reset and download mode: how far the last run got. */
#define CRUMB               (*(volatile u32 *)0x31AD0u)

extern u32 ram_load[], ram_begin[], ram_end[], bss_begin[], bss_end[], stack_top[];

void install_main(u32 chip_key);

__attribute__((section(".start"), naked, noreturn, noinline))
static void switch_stack(u32 chip_key, u32 sp_top)
{
    __asm__ volatile("sp = r1\n\t"
                     "ssp = r1\n\t"
                     "goto install_start");
}

__attribute__((section(".start"), used, noreturn, noinline))
void install_start(u32 chip_key)
{
    volatile u32 *src = ram_load, *dst = ram_begin;

    CRUMB = 0xC0DE0002;                     /* on our stack */
    while (dst < ram_end) {
        *dst++ = *src++;
    }
    for (dst = bss_begin; dst < bss_end; ) {
        *dst++ = 0;
    }
    CRUMB = 0xC0DE0003;                     /* copied */
    void (*volatile ram_main)(u32) = install_main;
    ram_main(chip_key);
    for (;;) {
    }
}

/* Entry point: r0 = BOOT_DEVICE_INFO * from the uboot; chip_id is at +12. */
__attribute__((section(".start.entry"), used, noreturn))
void installer_entry(u8 *info)
{
    CRUMB = 0xC0DE0001;                     /* the uboot ran us */
    switch_stack(info[12] | info[13] << 8, (u32)stack_top);
}

/* ------------------------------------------------------------------------- RAM part */

static u32 chip_key;
static u8 sector[SECTOR_SIZE] __attribute__((aligned(4)));
static u8 check[PAGE_SIZE] __attribute__((aligned(4)));

/* What happened, also in flash (DIAG_PAGE, inside the area the installer erases when it is
 * done), since a failed install has nothing to show it with: read it in download mode. */
#define DIAG_PAGE           0x4C000u
#define DIAG_PAGE_MAGIC     0x31474449u         /* "IDG1" */
static u32 diag_words[8];

static void diag(u32 step, u32 result)
{
    INSTALL_DIAG[0] = INSTALL_DIAG_MAGIC | step << 4 | (result & 0xF);
    diag_words[0] = DIAG_PAGE_MAGIC;
    diag_words[1] = step << 4 | (result & 0xF);
}

static void diag_save(void)
{
    flash_erase_sector(DIAG_PAGE);
    flash_program(DIAG_PAGE, (const u8 *)diag_words, sizeof(diag_words));
}

static u32 crc32_update(u32 crc, const u8 *p, u32 len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

static u16 crc16(const u8 *p, u32 n)
{
    u16 crc = 0;

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

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* JieLi ENC stream cipher (host/jlimage.py enc_cipher), in place, symmetric. */
static void enc_cipher(u8 *buf, u32 len, u32 key)
{
    for (u32 i = 0; i < len; i++) {
        buf[i] ^= (u8)key;
        key = ((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
    }
}

/* The SFC cipher for data meant for flash_off (host/jlimage.py sfc_cipher). */
static void sfc_cipher(u8 *buf, u32 len, u32 flash_off)
{
    for (u32 i = 0; i < len; i += 32) {
        enc_cipher(buf + i, len - i < 32 ? len - i : 32,
                   chip_key ^ ((flash_off + i - SFC_FLASH_BASE) >> 2));
    }
}

static int read_crc32(u32 addr, u32 len, u32 *crc)
{
    u32 c = 0;

    for (u32 a = 0; a < len; a += PAGE_SIZE) {
        u32 n = len - a < PAGE_SIZE ? len - a : PAGE_SIZE;

        if (flash_read(addr + a, check, n)) {
            return -1;
        }
        c = crc32_update(c, check, n);
        wdt_kick();
    }
    *crc = c;
    return 0;
}

/* Erase the sector at addr and program `sector` (len bytes) into it, then compare. */
static int write_sector(u32 addr, const u8 *data, u32 len)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        int bad = flash_erase_sector(addr) != 0;

        for (u32 a = 0; !bad && a < len; a += PAGE_SIZE) {
            u32 n = len - a < PAGE_SIZE ? len - a : PAGE_SIZE;

            bad = flash_program(addr + a, data + a, n) || flash_read(addr + a, check, n) ||
                  memcmp(check, data + a, n);
            wdt_kick();
        }
        if (!bad) {
            return 0;
        }
    }
    return -1;
}

/* Sector 0's file list: 32-byte entries, each ENC-ciphered with key 0xFFFF on its own and
 * protected by a CRC16 over its last 30 bytes. Returns the offset of app_dir_head, or -1. */
static int find_app_dir_head(const u8 *s0)
{
    for (u32 off = 0x20; off < 0x200; off += 32) {
        u8 e[32];

        memcpy(e, s0 + off, 32);
        enc_cipher(e, 32, 0xFFFF);
        if (crc16(e + 2, 30) != (e[0] | e[1] << 8)) {
            break;                          /* past the list */
        }
        if (!memcmp(e + 16, "app_dir_head", 13)) {
            return (int)off;
        }
    }
    return -1;
}

/* Point sector 0's app_dir_head at `dir`. */
static int set_boot_dir(u32 dir)
{
    int off;
    u8 e[32];

    if (flash_read(0, sector, SECTOR_SIZE)) {
        return -1;
    }
    off = find_app_dir_head(sector);
    if (off < 0) {
        return -2;
    }
    memcpy(e, sector + off, 32);
    enc_cipher(e, 32, 0xFFFF);
    if (rd32(e + 4) == dir) {
        return 0;                           /* already there */
    }
    wr32(e + 4, dir);
    e[0] = (u8)crc16(e + 2, 30);
    e[1] = (u8)(crc16(e + 2, 30) >> 8);
    enc_cipher(e, 32, 0xFFFF);
    memcpy(sector + off, e, 32);
    return write_sector(0, sector, SECTOR_SIZE) ? -3 : 0;
}

/* The payload must be an app area for DEST: directory CRC, entry point, whole-area CRC. */
static int payload_ok(const struct stage_header *h)
{
    u32 base = STAGE + h->payload_off, size;
    u16 crc = 0;

    if (flash_read(base, sector, 32)) {
        return 0;
    }
    size = rd32(sector + DIR_SIZE);
    if (crc16(sector + 2, 30) != (sector[0] | sector[1] << 8) ||
        rd32(sector + DIR_OFFSET) != SFC_CPU_BASE + 0x120 || size > h->payload_len) {
        return 0;
    }
    /* data_crc: CRC16 over everything after the head entry (crc16 continued per page) */
    for (u32 a = 0x20; a < size; a += PAGE_SIZE) {
        u32 n = size - a < PAGE_SIZE ? size - a : PAGE_SIZE;

        if (flash_read(base + a, check, n)) {
            return 0;
        }
        for (u32 i = 0; i < n; i++) {
            crc ^= (u16)check[i] << 8;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
            }
        }
    }
    return crc == (sector[DIR_DATA_CRC] | sector[DIR_DATA_CRC + 1] << 8);
}

static __attribute__((noreturn)) void reset(void)
{
    diag_save();
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}

void install_main(u32 key)
{
    struct stage_header h;
    u32 crc;

    CRUMB = 0xC0DE0004;                     /* in RAM */
    ROM_IRQ_DISABLE();
    wdt_kick();
    chip_key = key;
    diag(1, 0);
    if (flash_begin()) {
        diag(1, 1);
        reset();                            /* try again from the top */
    }
    /* The stock firmware leaves the flash block-protected: erases and writes would be ignored
     * without a word. Status registers before and after go in the diagnostics. */
    flash_read_sr(&diag_words[2], &diag_words[3]);
    diag_words[6] = (u32)flash_unprotect();
    flash_read_sr(&diag_words[4], &diag_words[5]);

    /* 1. the staging area */
    if (flash_read(STAGE, (u8 *)&h, sizeof(h)) || h.magic != STAGE_MAGIC || h.version != 1 ||
        crc32_update(0, (const u8 *)&h, 28) != h.hdr_crc32 || h.dest != DEST ||
        h.erase_end != DEST_ERASE_END || h.payload_len == 0 || h.payload_len > DEST_MAX ||
        STAGE + h.payload_off + h.payload_len > STAGE_END ||
        read_crc32(STAGE + h.payload_off, h.payload_len, &crc) || crc != h.payload_crc32 ||
        !payload_ok(&h)) {
        diag(1, 2);
        set_boot_dir(DEST);                 /* back to the stock firmware, untouched */
        reset();
    }

    /* 2. the stock firmware goes, the bootloader comes */
    diag(2, 0);
    for (u32 a = DEST; a < DEST_ERASE_END; a += SECTOR_SIZE) {
        if (flash_erase_sector(a)) {
            diag(2, 1);
            reset();
        }
        wdt_kick();
    }
    for (u32 off = 0; off < h.payload_len; off += SECTOR_SIZE) {
        u32 n = h.payload_len - off < SECTOR_SIZE ? h.payload_len - off : SECTOR_SIZE;

        if (flash_read(STAGE + h.payload_off + off, sector, n)) {
            diag(2, 2);
            reset();
        }
        sfc_cipher(sector, n, DEST + off);
        if (write_sector(DEST + off, sector, n)) {
            diag(2, 3);
            reset();
        }
    }

    /* 3. boot it from now on */
    diag(3, 0);
    if (set_boot_dir(DEST)) {
        diag(3, 1);
        reset();
    }
    for (u32 a = INSTALL_DIR; a < STAGE_END; a += SECTOR_SIZE) {
        flash_erase_sector(a);
        wdt_kick();
    }
    diag(3, 0xF);                           /* done */
    reset();
}
