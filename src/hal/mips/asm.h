/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS HAL's processor definitions, shared by its C and its assembly:
 * the coprocessor 0 registers it uses and their bits, the fixed segments
 * of the address space, and the width of a register and of a pointer,
 * which is all the 32-bit and the 64-bit processor differ in here.
 */

#ifndef KERN_HAL_MIPS_ASM_H
#define KERN_HAL_MIPS_ASM_H

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/*
 * The width of the processor.
 *
 * The 64-bit kernel is built for the n64 ABI with its symbols in the
 * 32-bit compatibility segments (the compiler's sym32 feature), so that an
 * address of the image is still a sign-extended 32-bit value: the same
 * lui and addiu reach it, and a pointer is added to with the doubleword
 * instructions.  MIPS_HAL_NAME begins the lines the HAL prints.
 *
 * The 64-bit kernel runs with Status.KX and UX set: the kernel reaches
 * xkphys, and the user's 64-bit segment takes the 64-bit TLB refill.
 * REG_S and REG_L store and load a whole register, and MFC0_WIDE and
 * MTC0_WIDE move the coprocessor 0 registers that hold an address
 * (EntryHi, EntryLo, BadVAddr, EPC) at their full width.
 *
 * Values referenced: MIPS64 Architecture for Programmers Volume III, the
 * Privileged Resource Architecture (Status: UX bit 5, KX bit 7; the xkphys segment
 * at 0x8000000000000000 with the cache attribute in bits 59-61, 2
 * uncached and 3 cacheable noncoherent).
 */
#if defined(HAL_ARCH_MIPS64)
#define MIPS_HAL_NAME			"MIPS64"
#define MIPS_REGISTER_BYTES		8
#define REG_S				sd
#define REG_L				ld
#define PTR_ADDU			daddu
#define PTR_ADDIU			daddiu
#define PTR_SRL				dsrl
#define MFC0_WIDE			dmfc0
#define MTC0_WIDE			dmtc0
#define MIPS_WIDE_MFC0_STRING		"dmfc0"
#define MIPS_WIDE_MTC0_STRING		"dmtc0"
#define MIPS_STATUS_KERNEL_ADDRESSING	0x000000a0
#define MIPS_XKPHYS_CACHED		0x9800000000000000
#define MIPS_XKPHYS_UNCACHED		0x9000000000000000
#else
#define MIPS_HAL_NAME			"MIPS32"
#define MIPS_REGISTER_BYTES		4
#define REG_S				sw
#define REG_L				lw
#define PTR_ADDU			addu
#define PTR_ADDIU			addiu
#define PTR_SRL				srl
#define MFC0_WIDE			mfc0
#define MTC0_WIDE			mtc0
#define MIPS_WIDE_MFC0_STRING		"mfc0"
#define MIPS_WIDE_MTC0_STRING		"mtc0"
#define MIPS_STATUS_KERNEL_ADDRESSING	0x00000000
#endif

/*
 * Loads the address of a symbol of the image: its high half and then the
 * sign-extended low half, which both widths take the same way, since the
 * image is in KSEG0 (on the 64-bit processor CKSEG0).
 */
#define LOAD_ADDRESS(target, symbol) \
	lui target, %hi(symbol); \
	addiu target, target, %lo(symbol)

/*
 * The coprocessor 0 registers, as register number and select.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III, the
 * Privileged Resource Architecture (the coprocessor 0 register numbers
 * and selects of Index, EntryLo0, EntryLo1, PageMask, Wired, BadVAddr,
 * Count, EntryHi, Compare, Status, Cause, EPC, PRId, EBase, Config,
 * Config1 and Config2).
 */
#define MIPS_CP0_INDEX			$0
#define MIPS_CP0_ENTRY_LO0		$2
#define MIPS_CP0_ENTRY_LO1		$3
#define MIPS_CP0_PAGE_MASK		$5
#define MIPS_CP0_WIRED			$6
#define MIPS_CP0_BAD_ADDRESS		$8
#define MIPS_CP0_COUNT			$9
#define MIPS_CP0_ENTRY_HI		$10
#define MIPS_CP0_COMPARE		$11
#define MIPS_CP0_STATUS			$12
#define MIPS_CP0_CAUSE			$13
#define MIPS_CP0_EXCEPTION_PC		$14
#define MIPS_CP0_PROCESSOR_ID		$15
#define MIPS_CP0_PROCESSOR_ID_SELECT	0
#define MIPS_CP0_EXCEPTION_BASE		$15
#define MIPS_CP0_EXCEPTION_BASE_SELECT	1
#define MIPS_CP0_CONFIG			$16
#define MIPS_CP0_CONFIG_SELECT		0
#define MIPS_CP0_CONFIG1_SELECT		1
#define MIPS_CP0_CONFIG2_SELECT		2

/*
 * The Ingenic XBurst cores' page control register (register 5, select
 * 4) and the value that turns their own page mode off, so that the TLB
 * behaves as the architecture's.  Written only on a processor whose
 * company field of PRId is Ingenic's.
 *
 * Values referenced: Linux master arch/mips/kernel/cpu-probe.c
 * (cpu_probe_ingenic: PRID_COMP_INGENIC_D1 0xd1 among Ingenic's company
 * fields, the page control register at register 5 select 4, the value
 * 0xa9000000); arch/mips/include/asm/cpu.h (the company field, PRId bits
 * 16-23).
 */
#define MIPS_CP0_PAGE_CTRL		$5
#define MIPS_CP0_PAGE_CTRL_SELECT	4
#define MIPS_PRID_COMPANY_SHIFT		16
#define MIPS_PRID_COMPANY_MASK		0xff
#define MIPS_PRID_COMPANY_INGENIC_D1	0xd1
#define MIPS_INGENIC_PAGE_CTRL_VTLB	0xa9000000

/*
 * The bits of Status.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Status: IE bit 0, EXL bit 1, ERL bit 2, KSU bits 3-4, IM bits 8-15,
 * BEV bit 22, FR bit 26, CU0 bit 28, CU1 bit 29).
 */
#define MIPS_STATUS_INTERRUPTS		0x00000001
#define MIPS_STATUS_EXCEPTION_LEVEL	0x00000002
#define MIPS_STATUS_ERROR_LEVEL		0x00000004
#define MIPS_STATUS_MODE_MASK		0x00000018
#define MIPS_STATUS_MODE_USER		0x00000010
#define MIPS_STATUS_INTERRUPT_MASK	0x0000ff00
#define MIPS_STATUS_BOOT_VECTORS	0x00400000
#define MIPS_STATUS_FLOAT_64		0x04000000
#define MIPS_STATUS_COPROCESSOR_0	0x10000000
#define MIPS_STATUS_COPROCESSOR_1	0x20000000

/*
 * The fields of Cause and Config the HAL reads.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Cause: ExcCode bits 2-6, CE bits 28-29, BD bit 31; Config: K0 bits
 * 0-2, the value 2 uncached and 3 cacheable noncoherent).
 */
#define MIPS_CAUSE_CODE_SHIFT		2
#define MIPS_CAUSE_CODE_MASK		0x1f
#define MIPS_CAUSE_BRANCH_DELAY		0x80000000
#define MIPS_CAUSE_PENDING_MASK		0x0000ff00
#define MIPS_CAUSE_COPROCESSOR_SHIFT	28
#define MIPS_CAUSE_COPROCESSOR_MASK	0x3

/*
 * The interrupt lines of Cause.IP and Status.IM the malta board wires:
 * the PIIX4's 8259 pair on hardware line 0 (IP2) and the timer of
 * coprocessor 0 on hardware line 5 (IP7).
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III (IP2
 * bit 10, IP7 bit 15); QEMU v10.0.11 target/mips/cpu.c (the IntCtl reset
 * value 0xe0000000, IPTI 7) and hw/mips/malta.c (the 8259 output on the
 * processor's interrupt 2).
 */
#define MIPS_INTERRUPT_BOARD		0x00000400
#define MIPS_INTERRUPT_TIMER		0x00008000

/*
 * The exception codes of Cause.ExcCode the trap handling tells apart.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III (Int
 * 0, Mod 1, TLBL 2, TLBS 3, AdEL 4, AdES 5, Sys 8, Bp 9, RI 10, CpU 11,
 * Ov 12, Tr 13, FPE 15).
 */
#define MIPS_EXCEPTION_INTERRUPT	0
#define MIPS_EXCEPTION_TLB_MODIFIED	1
#define MIPS_EXCEPTION_TLB_LOAD		2
#define MIPS_EXCEPTION_TLB_STORE	3
#define MIPS_EXCEPTION_ADDRESS_LOAD	4
#define MIPS_EXCEPTION_ADDRESS_STORE	5
#define MIPS_EXCEPTION_SYSTEM_CALL	8
#define MIPS_EXCEPTION_BREAKPOINT	9
#define MIPS_EXCEPTION_RESERVED		10
#define MIPS_EXCEPTION_COPROCESSOR	11
#define MIPS_EXCEPTION_OVERFLOW		12
#define MIPS_EXCEPTION_TRAP		13
#define MIPS_EXCEPTION_FLOAT		15

/*
 * The Cause field of the floating-point control and status register,
 * whose bits raise an exception as soon as a write sets one together
 * with its Enable bit.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume I (FCSR:
 * Cause bits 12-17, Enables bits 7-11).
 */
#define MIPS_FCSR_CAUSE_MASK		0x0003f000
#define MIPS_FCSR_ENABLE_SHIFT		5

/* The offsets of FCSR and FIR in struct hal_fpregs, after the 32 registers. */
#define MIPS_FPREGS_FCSR		256
#define MIPS_FPREGS_FIR			260
#define MIPS_CONFIG_K0_MASK		0x00000007
#define MIPS_CONFIG_K0_CACHED		3

/*
 * The fields of Config1 and of the floating-point implementation
 * register (FIR, control register 0) the HAL reads: the TLB size less
 * one, the presence of the floating-point unit, and its 64-bit
 * registers.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Config1: MMU Size bits 25-30, FP bit 0); Volume I (FIR: F64 bit 22).
 */
#define MIPS_CONFIG1_TLB_SHIFT		25
#define MIPS_CONFIG1_TLB_MASK		0x3f
#define MIPS_CONFIG1_FLOAT		0x00000001
#define MIPS_FIR_FLOAT_64		0x00400000

/*
 * The TLB's fields: Index's probe failure bit, the 8-bit address space
 * identifier of EntryHi and its 8 KiB pair of pages (VPN2), and the
 * bits of EntryLo: global, valid, dirty (writable), the cache
 * attribute and the page frame number from bit 6.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III
 * (Index P bit 31; EntryHi ASID bits 0-7, VPN2 from bit 13 (its mask is
 * with the page tables below); EntryLo G bit 0, V bit 1, D bit 2, C bits
 * 3-5, PFN from bit 6).
 */
#define MIPS_INDEX_PROBE_FAILED		0x80000000
#define MIPS_ENTRY_HI_ASID_MASK		0xff
#define MIPS_ENTRY_LO_GLOBAL		0x01
#define MIPS_ENTRY_LO_VALID		0x02
#define MIPS_ENTRY_LO_DIRTY		0x04
#define MIPS_ENTRY_LO_CACHE_SHIFT	3

/*
 * The page tables the TLB refill walks: 4 KiB pages and tables of one
 * page each.  The 32-bit processor has two levels: a root of 1024 words
 * indexed by the address's top ten bits, each the KSEG0 address of a
 * leaf table of 1024 entries or zero.  The 64-bit processor has three
 * levels of 512 doublewords (9 + 9 + 9 bits above the page offset, 512
 * GiB of user space): a root, middle tables and leaf tables, each
 * nonzero entry the xkphys address of the table below.
 *
 * A leaf entry is the page's physical address from bit 12, the six
 * EntryLo bits shifted up by MIPS_PTE_HARDWARE_SHIFT (bit 6 global, bit
 * 7 valid, bit 8 dirty, bits 9-11 the cache attribute) and below them
 * the software's own bits.  The refill shifts an entry right by six:
 * the frame number lands where EntryLo has it, and the software's bits
 * fall out.
 *
 * Values referenced: MIPS64 Architecture for Programmers Volume III
 * (EntryLo PFN from bit 6; EntryHi VPN2 bits 13 to SEGBITS-1, which is 42
 * on QEMU's MIPS64R2-generic, ws272-p001); QEMU v10.0.11
 * target/mips/cpu-defs.c.inc (PABITS 36).
 */
#define MIPS_PAGE_SIZE			4096
#define MIPS_PAGE_SHIFT			12
#if defined(HAL_ARCH_MIPS64)
#define MIPS_TABLE_LEVELS		3
#define MIPS_TABLE_INDEX_BITS		9
#define MIPS_TABLE_ENTRIES		512
#define MIPS_TABLE_ENTRY_SHIFT		3
#define MIPS_ROOT_SHIFT			30
#define MIPS_MIDDLE_SHIFT		21
#define MIPS_PTE_FRAME_MASK		0x0000000ffffff000
#define MIPS_ENTRY_HI_VPN2_MASK		0x000003ffffffe000
#else
#define MIPS_TABLE_LEVELS		2
#define MIPS_TABLE_INDEX_BITS		10
#define MIPS_TABLE_ENTRIES		1024
#define MIPS_TABLE_ENTRY_SHIFT		2
#define MIPS_ROOT_SHIFT			22
#define MIPS_PTE_FRAME_MASK		0xfffff000
#define MIPS_ENTRY_HI_VPN2_MASK		0xffffe000
#endif
#define MIPS_PTE_HARDWARE_SHIFT		6
#define MIPS_PTE_PRESENT		0x001
#define MIPS_PTE_ACCESSED		0x002
#define MIPS_PTE_WRITABLE		0x004
#define MIPS_PTE_DIRTY			0x008
#define MIPS_PTE_EXEC			0x010
#define MIPS_PTE_READABLE		0x020
#define MIPS_PTE_SOFTWARE_MASK		0x03f
#define MIPS_PTE_VALID			0x080
#define MIPS_PTE_WRITE_ENABLE		0x100
#define MIPS_PTE_CACHE_SHIFT		9

/*
 * The user segment, the first page never mapped: kuseg below 2 GiB on the
 * 32-bit processor, whose root entries are the low half of the root (the
 * high half stays zero, so that a refill for the kernel's mapped segments
 * finds nothing); on the 64-bit processor the low 512 GiB of xuseg, all
 * the three levels reach (MIPS_USER_SHIFT is its width in bits; the
 * 64-bit refill sends a miss above it to the general exception).
 */
#define MIPS_USER_MINIMUM		0x00001000
#if defined(HAL_ARCH_MIPS64)
#define MIPS_USER_SHIFT			39
#define MIPS_USER_LIMIT			0x0000008000000000
#else
#define MIPS_USER_LIMIT			0x80000000
#endif

/*
 * The fixed segments of the 32-bit address space: KSEG0 maps the low
 * 512 MiB of physical space cached and KSEG1 the same uncached, both
 * without the TLB.  The 64-bit processor has them as CKSEG0 and CKSEG1,
 * at the same addresses sign-extended; the values here are the 32-bit
 * ones, which mips_segment_pointer() of io.h extends.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III (the
 * kseg0 and kseg1 segments).
 */
#define MIPS_KSEG0_BASE			0x80000000
#define MIPS_KSEG1_BASE			0xa0000000
#define MIPS_KSEG_SIZE			0x20000000

/*
 * The offsets from EBase of the exception vectors when Status.BEV is clear:
 * the TLB refill, the 64-bit TLB refill, the cache error, the general
 * exception and the interrupt's own vector, which an interrupt takes
 * while Cause.IV is set.
 *
 * Values referenced: MIPS32 Architecture for Programmers Volume III (the
 * exception vector offsets 0x000, 0x080, 0x100 and 0x180, and 0x200 for
 * an interrupt with Cause.IV set and IntCtl.VS zero).
 */
#define MIPS_VECTOR_REFILL		0x000
#define MIPS_VECTOR_REFILL64		0x080
#define MIPS_VECTOR_CACHE_ERROR		0x100
#define MIPS_VECTOR_GENERAL		0x180
#define MIPS_VECTOR_INTERRUPT		0x200

/*
 * The trap frame, as offsets from its start: the 32 general registers,
 * hi, lo, and the exception's EPC, Status, Cause and BadVAddr, each a
 * whole register.  The 32-bit trap entry places it above the 16-byte
 * argument home of the C handler it calls, 168 bytes in all; the n64
 * ABI has no home, and the 64-bit frame is 304 bytes.  Both keep the
 * stack aligned as the ABI asks (8 and 16 bytes).
 */
#define MIPS_FRAME_REGISTER_BYTES	MIPS_REGISTER_BYTES
#define MIPS_FRAME_HI			(32 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_LO			(33 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_EXCEPTION_PC		(34 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_STATUS		(35 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_CAUSE		(36 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_BAD_ADDRESS		(37 * MIPS_REGISTER_BYTES)
#define MIPS_FRAME_SIZE			(38 * MIPS_REGISTER_BYTES)
#define MIPS_TRAP_STACK_BYTES		(MIPS_ARGUMENT_HOME_SIZE + MIPS_FRAME_SIZE)

/*
 * The registers mips_task_dispatch() keeps on a stack it leaves: s0 to
 * s7, fp and ra, and two that keep the stack aligned (on the 32-bit
 * processor with a free home slot).
 */
#define MIPS_DISPATCH_WORDS		12
#define MIPS_DISPATCH_FP		8
#define MIPS_DISPATCH_RA		9

/*
 * The stages of the boot mips_board_early_mark() shows on a board that
 * has no console yet: locore.S entered, C entered, and the board's
 * display about to be set up.
 */
#define MIPS_EARLY_MARK_START		1
#define MIPS_EARLY_MARK_C		2
#define MIPS_EARLY_MARK_DISPLAY		3

/* The bytes of the boot stack, which ends with the image's bss. */
#define MIPS_BOOT_STACK_SIZE		16384

/* The bytes of the stack the early exception report runs on. */
#define MIPS_TRAP_STACK_SIZE		4096

/*
 * The bytes an o32 caller reserves below its outgoing arguments for the
 * callee to keep a0 to a3 in.  An n64 caller reserves none.
 */
#if defined(HAL_ARCH_MIPS64)
#define MIPS_ARGUMENT_HOME_SIZE		0
#else
#define MIPS_ARGUMENT_HOME_SIZE		16
#endif

#endif
