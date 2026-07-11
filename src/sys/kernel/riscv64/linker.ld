/* Tell the linker that we want a riscv64 ELF64 output file */
OUTPUT_FORMAT(elf64-littleriscv)
OUTPUT_ARCH(riscv)

/* We want the symbol _start to be our entry point */
ENTRY(_start)

/* Define the program headers we want so the bootloader gives us the right */
/* MMU permissions */
PHDRS
{
    limine_requests PT_LOAD;
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

SECTIONS
{
    . = 0xffffffff80000000;

    /* Define a section to contain the Limine requests and assign it to its own PHDR */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    .text ALIGN(CONSTANT(MAXPAGESIZE)) : {
        *(.text .text.*)
    } :text

    /* init_array and fini_array are used by clang to call constructors and destructors */
    /* for global objects. */
    .init_array : {
        __init_array_start = .;
        *(.init_array)
        __init_array_end = .;
    } :text

    /* medany emits gp-relative .srodata/.sdata/.sbss; fold them into their large-section peers. */
    .rodata ALIGN(CONSTANT(MAXPAGESIZE)) : {
        *(.rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(8);
        __start__ktests = .;
        KEEP(*(.ktests))
        . = ALIGN(8);
        __stop__ktests = .;
        . = ALIGN(8);
        __start__kshell_cmds = .;
        KEEP(*(.kshell_cmds))
        . = ALIGN(8);
        __stop__kshell_cmds = .;
    } :rodata

    .data ALIGN(CONSTANT(MAXPAGESIZE)) : {
        *(.data .data.* .sdata .sdata.* .ldata)
    } :data

    .bss ALIGN(CONSTANT(MAXPAGESIZE)) : {
        *(COMMON)
        *(.bss .bss.* .sbss .sbss.* .lbss .lbss.*)
        . += 0x1000;
        . = ALIGN(CONSTANT(MAXPAGESIZE));

        _initial_heap_start = .;
        . += 0x10000;
        _initial_heap_end = .;
    } :data

    /* Discard .note.* and .eh_frame since they may cause issues on some hosts. */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
