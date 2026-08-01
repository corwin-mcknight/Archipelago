ENTRY(_start)

/* One PT_LOAD per protection class, declared explicitly so the image always has the shape the
   kernel's loader requires: page-aligned segments, and no segment that is writable and executable.
   The ELF headers are deliberately left out of every segment -- there is nothing in them a user
   program needs mapped, and including them would put a fourth PT_LOAD at a low address. */
PHDRS {
    text   PT_LOAD FLAGS(5);  /* R+X */
    rodata PT_LOAD FLAGS(4);  /* R   */
    data   PT_LOAD FLAGS(6);  /* R+W */
}

SECTIONS {
    /* Matches the address space the kernel gives a task: code low, stack well above it. */
    . = 0x400000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    } :text

    . = ALIGN(0x1000);
    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);
    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    /* .bss packs directly after .data in the same segment rather than starting a page of its own:
       it carries no file bytes, so this segment's memsz exceeds its filesz and the loader has to
       cover the difference with zero-filled anonymous pages. Aligning it to a fresh page instead
       would leave it outside every PT_LOAD, and the first access would fault. */
    .bss : {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
