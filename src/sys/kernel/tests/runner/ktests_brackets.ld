/* Host-tier linker fragment.
 *
 * The .ktests section name is not a valid C identifier, so lld does not synthesize the
 * __start_/__stop_ encapsulation symbols for it (the kernel linker script defines them by hand for
 * the same reason). Define them here and gather the section. INSERT AFTER augments the default link
 * script rather than replacing it, so the host runner still links as an ordinary executable. */
SECTIONS
{
    .ktests : {
        __start__ktests = .;
        KEEP(*(.ktests))
        __stop__ktests = .;
    }
} INSERT AFTER .rodata;
