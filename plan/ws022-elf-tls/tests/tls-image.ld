ENTRY(_start)
PHDRS {
 text PT_LOAD FLAGS(5);
 data PT_LOAD FLAGS(6);
 tls PT_TLS FLAGS(4);
 stack PT_GNU_STACK FLAGS(6);
}
SECTIONS {
 . = 0x00400000;
 .text : ALIGN(4096) { *(.text .text.*) *(.rodata .rodata.*) } :text
 . = ALIGN(4096);
 .data : { *(.data .data.*) } :data
 .tdata : { *(.tdata .tdata.*) } :data :tls
 .tbss : { *(.tbss .tbss.*) } :data :tls
 . = ALIGN(4096);
 .bss : { *(.bss .bss.*) *(COMMON) } :data
 .stack 0 (NOLOAD) : { . += 0x100000; } :stack
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
