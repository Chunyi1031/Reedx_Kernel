OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(KernelStart)

SECTIONS
{
    . = 0x100000;
    
    . = ALIGN(4K);
    .text : {
        *(.text.start)
        *(.text)
        *(.gnu.linkonce.t.*)
        . = ALIGN(16);
    }
    
    . = ALIGN(4K);
    .rodata : {
        *(.rodata)
    }
    
    . = ALIGN(4K);
    .data : {
        *(.data)
    }
    
    . = ALIGN(4K);
    .bss : {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;
    
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}