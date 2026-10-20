/* SMC-PAD installer (fw/boot/install.c): app area at flash 0x41000, linked at 0x1E00120.
 *
 * The uboot maps a slot's directory at CPU (entry & ~0xFFFF) (sfc_mode_boot: SFC BASE_ADR =
 * dir + 0x1000000 - (entry & 0xFFFF0000)), so code in a slot runs at 0x1E00120 like the
 * stock firmware. 0x41000 because 0x41000 - 0x1000 is a multiple of 0x40000: the SFC cipher
 * key then comes out the same whether the hardware keys it on the CPU or the flash address.
 * Otherwise the same scheme as boot.ld: .start runs in place and moves the rest to RAM
 * 0x12000, which runs with the SFC parked and ends in a chip reset.
 */
MEMORY
{
    code (rx)  : ORIGIN = 0x1E00120, LENGTH = 0x2000 - 0x120
    ram  (rwx) : ORIGIN = 0x12000,   LENGTH = 0x1E000
}

ENTRY(installer_entry)

SECTIONS
{
    .start :
    {
        KEEP(*(.start.entry))
        ASSERT(installer_entry == ORIGIN(code), "installer_entry must be the entry point");
        KEEP(*(.start))
        . = ALIGN(4);
        start_end = .;
    } > code

    .ram :
    {
        ram_begin = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
        ram_end = .;
    } > ram AT > code
    ram_load = LOADADDR(.ram);

    .bss (NOLOAD) :
    {
        bss_begin = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        bss_end = .;
    } > ram

    stack_top = ORIGIN(ram) + LENGTH(ram);

    /* boot.bin is .start then .ram, back to back, and must fit in front of the application */
    ASSERT(ram_load == start_end, ".ram is not stored right after .start")
    ASSERT(start_end + (ram_end - ram_begin) <= 0x1E02000 - 0x400, "installer too big (staging follows 0x2000 after it)")
}
