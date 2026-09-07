/* Private boot image. ELF is only a link intermediate; objcopy emits the bytes from this header
   through the end of .data. No ELF headers, tables, relocations, or symbol names reach boot. */
ENTRY(_start)
SECTIONS {
    . = 0x400000 - 64;
    .boot_header : {
        QUAD(0x54494e4948435241) /* ARCHINIT */
        QUAD(1) /* version */
        QUAD(INIT_MACHINE)
        QUAD(_start)
        QUAD(SIZEOF(.text))
        QUAD(SIZEOF(.rodata))
        QUAD(SIZEOF(.data))
        QUAD(SIZEOF(.bss))
    }
    .text : {
        *(.text .text.*)
        . = ALIGN(0x1000);
    }
    .rodata : {
        *(.rodata .rodata.*)
        . = ALIGN(0x1000);
    }
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
        BYTE(0)
        . = ALIGN(0x1000);
    }
    .bss (NOLOAD) : {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
    }
    ASSERT(. <= 0x800000, "init overlaps its boot stack")
    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.riscv.attributes)
    }
}
