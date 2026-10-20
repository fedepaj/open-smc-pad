/* SMC-PAD bootloader: layout and the contract with the application. */
#ifndef SMCPAD_BOOT_H
#define SMCPAD_BOOT_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define REG32(a)            (*(volatile u32 *)(a))

/* Flash layout (offsets). The SFC maps flash SFC_FLASH_BASE at CPU SFC_CPU_BASE. */
#define SFC_FLASH_BASE      0x1000u
#define SFC_CPU_BASE        0x1E00000u
#define APP_DIR             0x9000u                         /* application's app area */
#define APP_AREA_END        0x71000u                        /* = the VM start both directories declare */
#define APP_AREA_MAX        (APP_AREA_END - APP_DIR)
#define APP_ENTRY           (SFC_CPU_BASE + APP_DIR - SFC_FLASH_BASE + 0x120)
#define SECTOR_SIZE         0x1000u
#define PAGE_SIZE           0x100u

/* Survives a chip reset (the SDK's UPDATA_BEG, +0..7 are kept by ROM and uboot). The
 * application writes BOOT_MAGIC_DFU there and resets to stay in the bootloader. */
#define BOOT_MAGIC_ADDR     0x31AE0u
#define BOOT_MAGIC_DFU      0xB007DF00u

/* What the bootloader leaves for the application to show (console `I`): RAM the application
 * does not initialise, under UPDATA_BEG, past its own post-mortem records. */
#define BOOT_DIAG_ADDR      0x31AA0u
#define BOOT_DIAG_MAGIC     0xB0D1A600u
struct boot_diag {
    u32 magic;              /* BOOT_DIAG_MAGIC | reason the DFU mode was entered */
    u32 ms;                 /* milliseconds spent in DFU mode, last update */
    u32 polls;              /* main loop iterations */
    u32 usb_con0;
    u32 clk_con1;
    u32 pll_con;
    u32 events;             /* bit 1 configured, bit 2 download, bit 3 app manifest ok, bit 4 bootloader update armed */
    u32 blocks;             /* download blocks written */
    u32 last_error;
};                          /* 40 bytes: 0x31AA0..0x31AC8 */
#define BOOT_DIAG           ((volatile struct boot_diag *)BOOT_DIAG_ADDR)

/* Crash-loop counter: the bootloader counts boots here, the application clears it once it has
 * run for a few seconds (fw/smcpad: smcpad_boot_ok()). Valid only with its check word, so the
 * random RAM of a power-up reads as 0. */
#define BOOT_COUNT          ((volatile u32 *)0x31AD8u)  /* [0] count, [1] count ^ check */
#define BOOT_COUNT_CHECK    0x5A5AB007u
#define BOOT_CRASH_LIMIT    3u          /* the 4th boot in a row that was never confirmed: DFU */

/* Held at power-up: stay in the bootloader. Columns of the key matrix, sense line 1 (PC1):
 * 0 shift, 1 note repeat, 2 bluetooth, 3 pad bank, 4 knob bank, 5 left, 6 right, 7 play,
 * 8 rec, 9 pause (docs/hardware.md). */
#define DFU_KEY_COL_A       0u          /* shift */
#define DFU_KEY_COL_B       7u          /* play */

/* Why the DFU mode runs. */
#define DFU_REASON_MAGIC    1u          /* asked for by the application (B, 1200-baud touch) */
#define DFU_REASON_NO_APP   2u          /* no valid application directory */
#define DFU_REASON_KEYS     3u          /* shift + play held at power-up */
#define DFU_REASON_CRASH    4u          /* the application kept resetting before confirming */
#define DFU_REASON_BAD_CRC  5u          /* the application area does not match its CRC */

/* Mask ROM (addresses from the SDK's sdk.ld). */
#define ROM_CHIP_RESET      ((void (*)(u32, u32))0x117fccu)
#define ROM_IRQ_DISABLE     ((void (*)(void))0x117d44u)
#define ROM_P33_OR_1BYTE    ((void (*)(u32, u32))0x117d54u)
#define ROM_SFC_RESUME      ((void (*)(u32))0x117fb0u)

/* JLFS2 directory entry (32 bytes). */
#define DIR_HDR_CRC         0
#define DIR_DATA_CRC        2
#define DIR_OFFSET          4
#define DIR_SIZE            8

/* Bootloader version. NOT in the USB descriptors: macOS (Apple silicon, "allow accessories to
 * connect") treats a device whose descriptors changed as a new accessory and leaves it
 * unconfigured until the user approves it -- bcdDevice 0x0104 kept the updated bootloader
 * invisible to dfu-util. The descriptors stay the same forever (bcdDevice 0x0100); the
 * version is read through DFU alt 2: dfu-util -a 2 -U <file>. */
#define BOOT_VERSION        0x0107

/* Installer slot + staging area (fw/boot/install.c), used to install the bootloader from the
 * stock firmware and to update it from the bootloader itself (DFU alt 2). The uboot maps a
 * slot's directory at CPU 0x1E00000 (install.ld); 0x41000 keeps the SFC key independent of
 * CPU- vs flash-address keying. */
#define INSTALL_DIR         0x41000u
#define STAGE               0x43000u            /* staging header; the payload follows */
#define STAGE_END           0x4D000u
#define STAGE_MAGIC         0x49434D53u         /* "SMCI" */
#define DEST                0x1000u             /* where the payload (a bootloader area) goes */
#define DEST_ERASE_STOCK    0x26000u            /* from the stock firmware: all of it goes */
#define DEST_ERASE_UPDATE   APP_DIR             /* bootloader update: the application stays */
#define DEST_MAX            (APP_DIR - DEST)

struct stage_header {
    u32 magic;
    u32 version;                /* 1 */
    u32 payload_off;            /* from STAGE */
    u32 payload_len;
    u32 payload_crc32;
    u32 dest;                   /* DEST */
    u32 erase_end;              /* DEST_ERASE_STOCK or DEST_ERASE_UPDATE */
    u32 hdr_crc32;              /* over the 28 bytes above */
};

void dfu_main(u32 chip_key, u32 reason, u32 app_valid);

/* jlfs.c (RAM, SFC parked) */
u16 crc16_update(u16 crc, const u8 *p, u32 n);
u32 crc32_update(u32 crc, const u8 *p, u32 len);
u32 rd32(const u8 *p);
void wr32(u8 *p, u32 v);
void enc_cipher(u8 *buf, u32 len, u32 key);
void sfc_cipher(u8 *buf, u32 len, u32 flash_off, u32 chip_key);
int flash_crc32(u32 addr, u32 len, u32 *crc);
int flash_write_sector(u32 addr, const u8 *data, u32 len);
u32 area_check(u32 base, u32 entry, u32 max_size, int encrypted, u32 chip_key);
int stage_check(struct stage_header *h);
int set_boot_dir(u32 dir);

/* flash.c: raw SPI NOR access with the SFC parked for good (the DFU mode runs from RAM). */
int flash_begin(void);
int flash_erase_sector(u32 addr);
int flash_program(u32 addr, const u8 *data, u32 len);      /* within one page */
int flash_read(u32 addr, u8 *buf, u32 len);
int flash_read_sr(u32 *sr1, u32 *sr2);
int flash_unprotect(void);
void wdt_kick(void);

#endif
