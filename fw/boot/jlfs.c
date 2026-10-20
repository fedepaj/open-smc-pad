/* What the bootloader (dfu.c) and the installer (install.c) share, all running from RAM with
 * the SFC parked (flash.c): JieLi CRCs and ciphers, verified sector writes, sector 0's boot
 * pointer, and the check of an installer + staging area.  Formats: docs/flash-layout.md,
 * docs/bootloader.md. */
#include <string.h>
#include "boot.h"

static u8 sector[SECTOR_SIZE] __attribute__((aligned(4)));
static u8 check[PAGE_SIZE] __attribute__((aligned(4)));

/* JieLi CRC16: CCITT polynomial 0x1021, init as given, not reflected. */
u16 crc16_update(u16 crc, const u8 *p, u32 n)
{
    while (n--) {
        crc ^= (u16)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
        }
    }
    return crc;
}

u32 crc32_update(u32 crc, const u8 *p, u32 len)
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

u32 rd32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* JieLi ENC stream cipher (host/jlimage.py enc_cipher), in place, symmetric. */
void enc_cipher(u8 *buf, u32 len, u32 key)
{
    for (u32 i = 0; i < len; i++) {
        buf[i] ^= (u8)key;
        key = ((key << 1) ^ ((key & 0x8000) ? 0x1021 : 0)) & 0xFFFF;
    }
}

/* The SFC cipher for data meant for flash_off (host/jlimage.py sfc_cipher): per 32-byte block,
 * ENC keyed with chip_key ^ ((flash_off - 0x1000) >> 2). Symmetric. */
void sfc_cipher(u8 *buf, u32 len, u32 flash_off, u32 chip_key)
{
    for (u32 i = 0; i < len; i += 32) {
        enc_cipher(buf + i, len - i < 32 ? len - i : 32,
                   chip_key ^ ((flash_off + i - SFC_FLASH_BASE) >> 2));
    }
}

int flash_crc32(u32 addr, u32 len, u32 *crc)
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

/* Erase the sector at addr, program len bytes of data (RAM), read back and compare; three
 * attempts. */
int flash_write_sector(u32 addr, const u8 *data, u32 len)
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

/* An app area at flash `base`, stored encrypted (encrypted != 0, decrypted with chip_key as
 * the SFC would) or plaintext: directory CRC, entry point, size limit, whole-area CRC.
 * Returns the area size, 0 if it does not hold up. */
u32 area_check(u32 base, u32 entry, u32 max_size, int encrypted, u32 chip_key)
{
    u8 head[32];
    u32 size;
    u16 crc = 0;

    if (flash_read(base, head, sizeof(head))) {
        return 0;
    }
    if (encrypted) {
        sfc_cipher(head, sizeof(head), base, chip_key);
    }
    size = rd32(head + DIR_SIZE);
    if (crc16_update(0, head + 2, 30) != (head[0] | head[1] << 8) ||
        rd32(head + DIR_OFFSET) != entry || size <= 0x120 || size > max_size) {
        return 0;
    }
    for (u32 a = 0x20; a < size; a += PAGE_SIZE) {
        u32 n = size - a < PAGE_SIZE ? size - a : PAGE_SIZE;

        if (flash_read(base + a, check, n)) {
            return 0;
        }
        if (encrypted) {
            /* page offsets are 32-byte aligned past the first 0x20: the cipher blocks line up */
            sfc_cipher(check, n, base + a, chip_key);
        }
        crc = crc16_update(crc, check, n);
        wdt_kick();
    }
    return crc == (head[DIR_DATA_CRC] | head[DIR_DATA_CRC + 1] << 8) ? size : 0;
}

/* The staging area: header CRC, sane fields, payload CRC32, and the payload being a
 * bootloader area for DEST. Fills *h; 0 when all of it holds. */
int stage_check(struct stage_header *h)
{
    u32 crc;

    if (flash_read(STAGE, (u8 *)h, sizeof(*h)) || h->magic != STAGE_MAGIC || h->version != 1 ||
        crc32_update(0, (const u8 *)h, 28) != h->hdr_crc32 || h->dest != DEST ||
        (h->erase_end != DEST_ERASE_STOCK && h->erase_end != DEST_ERASE_UPDATE) ||
        h->payload_len == 0 || h->payload_len > DEST_MAX ||
        STAGE + h->payload_off + h->payload_len > STAGE_END) {
        return -1;
    }
    if (flash_crc32(STAGE + h->payload_off, h->payload_len, &crc) || crc != h->payload_crc32) {
        return -2;
    }
    if (!area_check(STAGE + h->payload_off, SFC_CPU_BASE + 0x120, h->payload_len, 0, 0)) {
        return -3;
    }
    return 0;
}

/* Sector 0's file list: 32-byte entries, each ENC-ciphered with key 0xFFFF on its own and
 * protected by a CRC16 over its last 30 bytes. Returns the offset of app_dir_head, or -1. */
static int find_app_dir_head(const u8 *s0)
{
    for (u32 off = 0x20; off < 0x200; off += 32) {
        u8 e[32];

        memcpy(e, s0 + off, 32);
        enc_cipher(e, 32, 0xFFFF);
        if (crc16_update(0, e + 2, 30) != (e[0] | e[1] << 8)) {
            break;                          /* past the list */
        }
        if (!memcmp(e + 16, "app_dir_head", 13)) {
            return (int)off;
        }
    }
    return -1;
}

/* Point sector 0's app_dir_head at `dir`: what the uboot boots from the next reset on. The
 * one write in the whole scheme that a power cut can make fatal (tens of ms). */
int set_boot_dir(u32 dir)
{
    int off;
    u8 e[32];
    u16 crc;

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
    crc = crc16_update(0, e + 2, 30);
    e[0] = (u8)crc;
    e[1] = (u8)(crc >> 8);
    enc_cipher(e, 32, 0xFFFF);
    memcpy(sector + off, e, 32);
    return flash_write_sector(0, sector, SECTOR_SIZE) ? -3 : 0;
}
