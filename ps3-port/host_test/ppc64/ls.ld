ENTRY(_start)
SECTIONS {
 . = 0x10000000;
 .text : { *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .opd : { *(.opd) }
 .toc : { *(.toc) }
 .got : { *(.got) *(.got.plt) }
 . = ALIGN(4096);
 .data : { *(.data .data.*) *(.sdata*) }
 .bss : { *(.sbss*) *(.bss .bss.*) *(COMMON) }
}
