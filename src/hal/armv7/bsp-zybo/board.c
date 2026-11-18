/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Digilent ZYBO (Zynq-7010) board, and the emulator's xilinx-zynq-a9
 * machine, which the same code runs on.
 *
 * The board is booted over JTAG: U-Boot SPL sets up the clocks, the DDR
 * and UART1, and the debugger puts the image at 0x00100000 and a device
 * tree at 0x01f00000 and enters with r2 naming it.  The emulator's ELF
 * loader passes nothing in r2, so a tree the host's script placed at the
 * same 0x01f00000 is taken instead.  The tree describes the memory, the
 * command line and the ramdisk; everything else is at the addresses the
 * SoC has them.
 *
 * The first MiB of RAM is never used (ARMV7_LOW_RAM_SKIP): in the JTAG
 * boot it is on-chip memory, a hole that aborts, and an alias of the high
 * on-chip memory where the second processor waits in the BootROM.  The
 * processor enters secure and the GIC is driven with its groups.  The
 * PL310 level 2 cache is left off; one a loader left on is cleaned and
 * turned off.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../clock.h"
#include "../defs.h"
#include "../space.h"
#include "zybo.h"

/*
 * Where the JTAG script and the emulator's script put the device tree,
 * and the tree's first word as it lies in memory, read as a
 * little-endian word.
 *
 * Values referenced: plan/tools/zybo/jtag-boot.sh (dtb_addr); the
 * Devicetree Specification, the flattened format's magic 0xd00dfeed.
 */
#define BOARD_FDT_PHYS		0x01f00000U
#define BOARD_FDT_MAGIC_LE	0xedfe0dd0U

/*
 * The lower 28 bits of a Z-7010's PS IDCODE; the upper 4 are the
 * silicon's revision.
 *
 * Values referenced: the board's value 0x13722093, read over JTAG
 * (plan/ws286/design.md section 4.8).
 */
#define BOARD_IDCODE_Z7010	0x03722093U
#define BOARD_IDCODE_PART_MASK	0x0fffffffU

/*
 * The PL310's control register, its enable, and the way operations that
 * clean and drop the whole cache, for a cache found on.
 *
 * Values referenced: Linux v6.12 arch/arm/include/asm/hardware/cache-l2x0.h
 * (L2X0_CTRL 0x100, L2X0_CACHE_SYNC 0x730, L2X0_CLEAN_INV_WAY 0x7FC);
 * the eight ways of the Zynq's 512 KiB PL310.
 */
#define BOARD_L2_CTRL		0x100U
#define BOARD_L2_CACHE_SYNC	0x730U
#define BOARD_L2_CLEAN_INV_WAY	0x7fcU
#define BOARD_L2_ENABLE		0x1U
#define BOARD_L2_ALL_WAYS	0xffU

/* How many times the PL310 is looked at before its way operation is given up on. */
#define BOARD_L2_POLLS		1000000U

/* The SLCR's soft reset of the whole PS, and the reboot status bits Linux clears first. */
#define BOARD_PSS_RST		0x1U
#define BOARD_REBOOT_KEEP	0xf0ffffffU

/* How long, in seconds, the reset is given before the board reports that it did not happen. */
#define BOARD_RESET_WAIT_SECONDS	1U

/*
 * The PS IDCODE read at boot, and what the board found of the level 2
 * cache: its control register as the loader left it.  Set once on the
 * boot processor and read for the boot log.
 */
static uint32_t board_idcode;
static uint32_t board_l2_control;

/*
 * Why the board's registers could not be mapped, or HAL_OK.  Set once on
 * the boot processor by bsp_early_init().
 */
static int board_map_error = HAL_ERR_UNSUPPORTED;

static int fdt_is_at(uintptr_t physical);
static void stop_l2(void);

/*
 * Makes the console reachable before the device tree is read: maps the
 * SLCR, the MPCore's region and UART1, and tells the board from the
 * emulator.
 */
void
bsp_early_init(
	void)
{
	int error;

	/* The SLCR and the MPCore's region, which tell the machine apart. */
	error = zybo_slcr_init();
	board_map_error = error;
	if (error != HAL_OK)
		return;
	board_idcode = zybo_slcr_read(ZYBO_SLCR_PSS_IDCODE);

	/* The console; a failure leaves it silent. */
	(void)zybo_uart_init();
}

/*
 * Names the device tree: the one r2 names, else the one placed at
 * 0x01f00000, else none.
 */
uintptr_t
bsp_find_fdt(
	uintptr_t loader_argument)
{
	int found;

	/* The JTAG boot names it in r2. */
	found = fdt_is_at(loader_argument);
	if (found)
		return loader_argument;

	/* The emulator's ELF loader passes nothing; its script puts the tree at the JTAG boot's address. */
	found = fdt_is_at(BOARD_FDT_PHYS);
	if (found) {
		hal_printf("ZYBO FDT r2=%08x is none; using the tree at %08x\n", (uint32_t)loader_argument, BOARD_FDT_PHYS);
		return BOARD_FDT_PHYS;
	}

	/* No tree anywhere: the boot stops on it. */
	return 0;
}

/*
 * Sets up what the board needs before the memory is managed: the level 2
 * cache is made sure to be off.
 */
void
bsp_init(
	const struct armv7_fdt_info *info)
{
	UNUSED_PARAMETER(info);

	/* Nothing is set up without the board's registers. */
	if (board_map_error != HAL_OK)
		HAL_FATAL("ZYBO registers cannot be mapped");

	/* The level 2 cache stays off. */
	stop_l2();
}

/*
 * Writes the machine, the processor's coherency setup and the timers'
 * clock to the boot log.
 */
void
bsp_describe(
	void)
{
	volatile uint8_t *mpcore;
	const char *machine;
	uint64_t periphclk;
	uint32_t part;
	uint32_t scu_control;
	uint32_t actlr;
	uint32_t mmfr3;
	int emulated;

	/* Which machine: the emulator, the Z-7010, or a part this does not know. */
	machine = "board";
	part = board_idcode & BOARD_IDCODE_PART_MASK;
	emulated = zybo_emulated();
	if (emulated) {
		machine = "qemu";
	} else if (part != BOARD_IDCODE_Z7010) {
		hal_printf("ZYBO IDCODE UNKNOWN 0x%08x\n", board_idcode);
	}

	/* The machine and its IDCODE, the line the tests look for. */
	hal_printf("ZYBO PLATFORM %s idcode=0x%08x\n", machine, board_idcode);

	/* The coherency the boot set up: ACTLR's SMP and FW, the SCU, the coherent walk, the L2. */
	mpcore = zybo_mpcore();
	scu_control = hal_mmio_read32(mpcore + ZYBO_MPCORE_SCU);
	actlr = armv7_read_actlr();
	mmfr3 = armv7_read_id_mmfr3();
	hal_printf("ZYBO CPU actlr=%08x scu_ctrl=%08x id_mmfr3=%08x l2_ctrl=%08x\n",
		   actlr,
		   scu_control,
		   mmfr3,
		   board_l2_control);

	/* The timers' clock. */
	periphclk = zybo_slcr_periphclk();
	hal_printf("ZYBO CLOCK periphclk=%llu Hz\n", periphclk);
}

/*
 * Writes one early-console character to UART1.
 */
void
bsp_cons_putc(
	int character)
{
	/* The board has no screen yet: the UART only. */
	zybo_uart_putc(character);
}

/*
 * Reads one character from UART1, or -1 when none has arrived.
 */
int
bsp_cons_getc(
	void)
{
	int character;

	/* Polls the receive FIFO. */
	character = zybo_uart_getc();

	/* Succeeded: the character, or -1. */
	return character;
}

/*
 * Reports the GIC's two blocks, at the MPCore's fixed offsets; the
 * processor is secure, so the groups are driven.
 */
int
bsp_gic_bases(
	const struct armv7_fdt_info *info,
	uint64_t *distributor,
	uint64_t *cpu_interface,
	int *secure)
{
	UNUSED_PARAMETER(info);

	/* Succeeded: the blocks in the MPCore's private region. */
	*distributor = (uint64_t)ZYBO_MPCORE_PHYS + ZYBO_MPCORE_GIC_DISTRIBUTOR;
	*cpu_interface = (uint64_t)ZYBO_MPCORE_PHYS + ZYBO_MPCORE_GIC_CPU;
	*secure = 1;
	return HAL_OK;
}

/*
 * Reports the RAM the board keeps from the allocator: none beyond the
 * first MiB, which the HAL already keeps out.
 */
unsigned
bsp_reserved_ranges(
	struct armv7_fdt_range *ranges,
	unsigned capacity)
{
	UNUSED_PARAMETER(ranges);
	UNUSED_PARAMETER(capacity);

	/* Succeeded: nothing more to keep. */
	return 0;
}

/*
 * Reports whether the machine is the emulator's.
 */
int
bsp_is_emulated(
	void)
{
	int emulated;

	/* The PS IDCODE read at boot says. */
	emulated = zybo_emulated();

	/* Succeeded: 1 for the emulator, 0 for the board. */
	return emulated;
}

/*
 * Resets the whole PS through the SLCR; returns, after saying so, when
 * the machine is still running a second later.
 *
 * With the boot-mode jumper on JTAG, the BootROM then waits for the
 * debugger again.
 *
 * Procedure referenced: Linux v6.12 arch/arm/mach-zynq/slcr.c,
 * zynq_slcr_system_restart (REBOOT_STATUS & 0xF0FFFFFF, then PS_RST_CTRL
 * = 1).
 */
void
bsp_reset(
	void)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint32_t reboot_status;
	bool read;

	/* Clears the bits that would keep a loader from loading the PL after the reset. */
	reboot_status = zybo_slcr_read(ZYBO_SLCR_REBOOT_STATUS);
	zybo_slcr_write(ZYBO_SLCR_REBOOT_STATUS, reboot_status & BOARD_REBOOT_KEEP);

	/* Resets the PS. */
	read = armv7_clock_read(&start, &frequency);
	zybo_slcr_write(ZYBO_SLCR_PSS_RST_CTRL, BOARD_PSS_RST);

	/* Waits a second by the counter for the reset to take the machine. */
	if (read) {
		do {
			(void)armv7_clock_read(&now, &frequency);
		} while (now - start < frequency * BOARD_RESET_WAIT_SECONDS);
	}

	/* The machine is still running: the caller halts it. */
	hal_puts("ZYBO RESET FAILED\n");
}

/*
 * Reports that the board cannot turn itself off; the caller halts it.
 */
void
bsp_poweroff(
	void)
{
	/* The board is powered from USB, with nothing the PS switches. */
	hal_puts("ZYBO POWER OFF unsupported; halting\n");
}

/*
 * Reports whether a device tree lies at a physical address of RAM the
 * boot map holds.
 */
static int
fdt_is_at(
	uintptr_t physical)
{
	const volatile uint32_t *first_word;
	uint64_t floor;
	uint64_t ceiling;

	/* Only RAM the boot map reaches, on a word boundary, can be read now. */
	floor = (uint64_t)ARMV7_PHYS_BASE + ARMV7_LOW_RAM_SKIP;
	ceiling = (uint64_t)ARMV7_PHYS_BASE + ARMV7_BOOT_MAP_BYTES;
	if ((uint64_t)physical < floor || (uint64_t)physical >= ceiling)
		return 0;
	if ((physical & 0x3U) != 0)
		return 0;

	/* A tree starts with its magic. */
	first_word = (const volatile uint32_t *)armv7_phys_to_direct((uint64_t)physical);
	if (*first_word != BOARD_FDT_MAGIC_LE)
		return 0;

	/* Succeeded: a tree is there. */
	return 1;
}

/*
 * Makes sure the PL310 level 2 cache is off: one the loader left on is
 * cleaned and dropped by way, then turned off.  Its control register as
 * found is kept for the boot log.
 */
static void
stop_l2(
	void)
{
	volatile uint8_t *l2;
	uint32_t ways;
	uint32_t polls;

	/* Reads whether the cache is on. */
	l2 = zybo_mpcore() + ZYBO_MPCORE_L2;
	board_l2_control = hal_mmio_read32(l2 + BOARD_L2_CTRL);
	if ((board_l2_control & BOARD_L2_ENABLE) == 0)
		return;

	/* Cleans and drops every way, and waits a bounded time for it. */
	hal_mmio_write32(l2 + BOARD_L2_CLEAN_INV_WAY, BOARD_L2_ALL_WAYS);
	for (polls = 0; polls < BOARD_L2_POLLS; polls++) {
		ways = hal_mmio_read32(l2 + BOARD_L2_CLEAN_INV_WAY);
		if ((ways & BOARD_L2_ALL_WAYS) == 0)
			break;
	}

	/* Drains the cache's buffers and turns it off. */
	hal_mmio_write32(l2 + BOARD_L2_CACHE_SYNC, 0);
	hal_mmio_write32(l2 + BOARD_L2_CTRL, 0);
	hal_io_mb();
	hal_puts("ZYBO L2 was on; cleaned and turned off\n");
}
