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
    u32 events;             /* bit 0 bus reset seen, bit 1 configured, bit 2 download, bit 3 manifest ok */
    u32 blocks;             /* download blocks written */
    u32 last_error;
};                          /* 40 bytes: 0x31AA0..0x31AC8 */
#define BOOT_DIAG           ((volatile struct boot_diag *)BOOT_DIAG_ADDR)

/* Why the DFU mode runs. */
#define DFU_REASON_MAGIC    1u
#define DFU_REASON_NO_APP   2u

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

void dfu_main(u32 chip_key, u32 reason, u32 app_valid);

/* flash.c: raw SPI NOR access with the SFC parked for good (the DFU mode runs from RAM). */
int flash_begin(void);
int flash_erase_sector(u32 addr);
int flash_program(u32 addr, const u8 *data, u32 len);      /* within one page */
int flash_read(u32 addr, u8 *buf, u32 len);
void wdt_kick(void);

#endif
