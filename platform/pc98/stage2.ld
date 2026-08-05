/*
 * BOOT.SYS is a two-segment ELF (see scripts/patch-stage2.py for the
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
		*/kern/*.o(.text*) *exit-trampoline.o(.text*)
		*stage2.o(.text*) *stage2-m9-test.o(.text*)
		*console.o(.text*) *env.o(.text*)
		*fs.o(.text*) *namespace.o(.text*)
		*fat.o(.text*) *fat16.o(.text*) */core/image.o(.text*)
		*blkdev.o(.text*) */core/partition.o(.text*)
		*ide-pc98.o(.text*) *partition-pc98.o(.text*)
		*kbd-pc98.o(.text*) *kbd-pc98-map.o(.text*)
		*display-pc98.o(.text*)
		*heap.o(.text*) *string.o(.text*) *ctype.o(.text*)
		*int64.o(.text*) *strto.o(.text*) *format.o(.text*)
		*stdio.o(.text*) *stdio-fs.o(.text*)
	} :low
	.rodata.low : {
		*/hal/i386/*.o(.rodata*) */hal/i386/bsp-pc98/*.o(.rodata*)
		*/kern/*.o(.rodata*) *exit-trampoline.o(.rodata*)
		*stage2.o(.rodata*) *stage2-m9-test.o(.rodata*)
		*console.o(.rodata*) *env.o(.rodata*)
		*fs.o(.rodata*) *namespace.o(.rodata*)
		*fat.o(.rodata*) *fat16.o(.rodata*) */core/image.o(.rodata*)
		*blkdev.o(.rodata*) */core/partition.o(.rodata*)
		*ide-pc98.o(.rodata*) *partition-pc98.o(.rodata*)
		*kbd-pc98.o(.rodata*) *kbd-pc98-map.o(.rodata*)
		*display-pc98.o(.rodata*)
		*heap.o(.rodata*) *string.o(.rodata*) *ctype.o(.rodata*)
		*int64.o(.rodata*) *strto.o(.rodata*) *format.o(.rodata*)
		*stdio.o(.rodata*) *stdio-fs.o(.rodata*)
	} :low
	.data.low : {
		*/hal/i386/*.o(.data*) */hal/i386/bsp-pc98/*.o(.data*)
		*/kern/*.o(.data*) *exit-trampoline.o(.data*)
		*stage2.o(.data*) *stage2-m9-test.o(.data*)
		*console.o(.data*) *env.o(.data*)
		*fs.o(.data*) *namespace.o(.data*)
		*fat.o(.data*) *fat16.o(.data*) */core/image.o(.data*)
		*blkdev.o(.data*) */core/partition.o(.data*)
		*ide-pc98.o(.data*) *partition-pc98.o(.data*)
		*kbd-pc98.o(.data*) *kbd-pc98-map.o(.data*)
		*display-pc98.o(.data*)
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
		*/kern/*.o(.bss* COMMON) *exit-trampoline.o(.bss* COMMON)
		*stage2.o(.bss* COMMON) *stage2-m9-test.o(.bss* COMMON)
		*console.o(.bss* COMMON) *env.o(.bss* COMMON)
		*fs.o(.bss* COMMON) *namespace.o(.bss* COMMON)
		*fat.o(.bss* COMMON) *fat16.o(.bss* COMMON)
		*/core/image.o(.bss* COMMON)
		*blkdev.o(.bss* COMMON) */core/partition.o(.bss* COMMON)
		*ide-pc98.o(.bss* COMMON) *partition-pc98.o(.bss* COMMON)
		*kbd-pc98.o(.bss* COMMON) *kbd-pc98-map.o(.bss* COMMON)
		*display-pc98.o(.bss* COMMON)
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
	.bss.high (NOLOAD) : { *(.bss*) *(COMMON) } :high
	__high_bss_end = .;
	__high_end = .;

	ASSERT(__low_end <= 0x80080000,
	       "BOOT.SYS low segment overlaps boot parameters (0x80000)")
	ASSERT(__high_end <= 0x80F00000,
	       "BOOT.SYS high segment reaches the PC-98 15MB hole")
}
