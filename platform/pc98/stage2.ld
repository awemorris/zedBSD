/*
 * vmunix is a two-segment ELF (see scripts/patch-stage2.py for the
 * subset contract Stage 1 relies on).  The low segment is the loader
 * closure: everything that may execute once a kernel load has started
 * overwriting memory at and above 1 MiB.  The high segment carries the
 * Noct VM, BeUI, and soft-float support, and is dead memory from that
 * point on, so a kernel whose PHYSICAL_START is exactly 1 MiB may land
 * on top of it.
 */
OUTPUT_FORMAT(elf32-i386)
ENTRY(text_start)
PHDRS
{
	low  PT_LOAD ;
	high PT_LOAD ;
}
SECTIONS
{
	. = 0x80020000;
	__low_start = .;
	.header : { KEEP(*(.header)) } :low
	.text.low : {
		*(.text.entry)
		*/hal/i386/*.o(.text*) */hal/i386/bsp-pc98/*.o(.text*)
		*/kern/*.o(EXCLUDE_FILE (*/kern/net/*.o) .text*)
		*/kern/pc98/*.o(.text*)
		*main.o(.text*) *stage2-m9-test.o(.text*)
		*console.o(.text*) *env.o(.text*)
		*fs.o(.text*) *namespace.o(.text*)
		*fat.o(.text*) *fat-lfn.o(.text*) *fat16.o(.text*) *image.o(.text*)
		*disk.o(.text*) *partition.o(.text*)
		*pc98-ide.o(.text*)
		*kbd-pc98.o(.text*) *kbd-pc98-map.o(.text*)
		*display.o(.text*)
		*heap.o(.text*) *string.o(.text*) *ctype.o(.text*)
		*int64.o(.text*) *strto.o(.text*) *format.o(.text*)
		*stdio.o(.text*) *stdio-fs.o(.text*)
	} :low
	.rodata.low : {
		*/hal/i386/*.o(.rodata*) */hal/i386/bsp-pc98/*.o(.rodata*)
		*/kern/*.o(EXCLUDE_FILE (*/kern/net/*.o) .rodata*)
		*/kern/pc98/*.o(.rodata*)
		*main.o(.rodata*) *stage2-m9-test.o(.rodata*)
		*console.o(.rodata*) *env.o(.rodata*)
		*fs.o(.rodata*) *namespace.o(.rodata*)
		*fat.o(.rodata*) *fat-lfn.o(.rodata*) *fat16.o(.rodata*) *image.o(.rodata*)
		*disk.o(.rodata*) *partition.o(.rodata*)
		*pc98-ide.o(.rodata*)
		*kbd-pc98.o(.rodata*) *kbd-pc98-map.o(.rodata*)
		*display.o(.rodata*)
		*heap.o(.rodata*) *string.o(.rodata*) *ctype.o(.rodata*)
		*int64.o(.rodata*) *strto.o(.rodata*) *format.o(.rodata*)
		*stdio.o(.rodata*) *stdio-fs.o(.rodata*)
	} :low
	.data.low : {
		*/hal/i386/*.o(.data*) */hal/i386/bsp-pc98/*.o(.data*)
		*/kern/*.o(EXCLUDE_FILE (*/kern/net/*.o) .data*)
		*/kern/pc98/*.o(.data*)
		*main.o(.data*) *stage2-m9-test.o(.data*)
		*console.o(.data*) *env.o(.data*)
		*fs.o(.data*) *namespace.o(.data*)
		*fat.o(.data*) *fat-lfn.o(.data*) *fat16.o(.data*) *image.o(.data*)
		*disk.o(.data*) *partition.o(.data*)
		*pc98-ide.o(.data*)
		*kbd-pc98.o(.data*) *kbd-pc98-map.o(.data*)
		*display.o(.data*)
		*heap.o(.data*) *string.o(.data*) *ctype.o(.data*)
		*int64.o(.data*) *strto.o(.data*) *format.o(.data*)
		*stdio.o(.data*) *stdio-fs.o(.data*)
	} :low
	__low_image_end = .;
	__low_bss_start = .;
	.bss.low (NOLOAD) : SUBALIGN(16) {
		/* locore owns page tables whose physical addresses must be 4K aligned.
		 * Align their placement explicitly without raising PT_LOAD p_align,
		 * because Stage 1's contract fixes the file payload at offset 512. */
		. = ALIGN(4096);
		*/hal/i386/locore.o(.bss* COMMON)
		*/hal/i386/*.o(.bss* COMMON)
		*/hal/i386/bsp-pc98/*.o(.bss* COMMON)
		*/kern/*.o(EXCLUDE_FILE (*/kern/net/*.o) .bss* COMMON)
		*/kern/pc98/*.o(.bss* COMMON)
		*main.o(.bss* COMMON) *stage2-m9-test.o(.bss* COMMON)
		*console.o(.bss* COMMON) *env.o(.bss* COMMON)
		*fs.o(.bss* COMMON) *namespace.o(.bss* COMMON)
		*fat.o(.bss* COMMON) *fat-lfn.o(.bss* COMMON) *fat16.o(.bss* COMMON)
		*image.o(.bss* COMMON)
		*disk.o(.bss* COMMON) *partition.o(.bss* COMMON)
		*pc98-ide.o(.bss* COMMON)
		*kbd-pc98.o(.bss* COMMON) *kbd-pc98-map.o(.bss* COMMON)
		*display.o(.bss* COMMON)
		*heap.o(.bss* COMMON) *string.o(.bss* COMMON)
		*ctype.o(.bss* COMMON) *int64.o(.bss* COMMON)
		*strto.o(.bss* COMMON) *format.o(.bss* COMMON)
		*stdio.o(.bss* COMMON) *stdio-fs.o(.bss* COMMON)
	} :low
	__low_bss_end = .;
	__low_end = .;

	. = 0x80100000;
	__high_start = .;
	.text.high : { *(.text*) } :high
	.rodata.high : { *(.rodata*) } :high
	.data.high : { *(.data*) } :high
	__high_image_end = .;
	__high_bss_start = .;
	.bss.high (NOLOAD) : { *(.vfs_bss) *(.net_bss) *(.bss*) *(COMMON) } :high
	.kernel_heap (NOLOAD) : ALIGN(4096) {
		__kernel_heap_start = .;
		KEEP(*(.kernel_heap))
		__kernel_heap_end = .;
	} :high
	__high_bss_end = .;
	__high_end = .;

	ASSERT(__low_end <= 0x80080000,
	       "vmunix low segment overlaps boot parameters (0x80000)")
	ASSERT(__high_end <= 0x80F00000,
	       "vmunix high segment reaches the PC-98 15MB hole")
	/* The retained PC-98 display backend shares the HAL's Unicode table;
	 * the Noct VM's duplicate table now lives only in NOCT.ELF. */
	PROVIDE(noct_jisx0208_to_ucs = hal_pc98_jisx0208_to_ucs);
}
