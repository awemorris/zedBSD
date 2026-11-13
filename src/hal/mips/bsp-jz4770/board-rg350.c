/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Anbernic RG350 on the Ingenic JZ4770: the board's least part.
 *
 * The boot loader, UBIBoot, enters the kernel with an argument table that
 * lives only in its data cache lines; the HAL writes the caches back and
 * empties them before the board runs, so the board reads none of it and
 * the command line stays empty.  The board uses the low 256 MiB of RAM.
 * The display, the tick and the interrupt controller come in later
 * stages of the port; until then the console keeps its grid without a
 * display, the marks of the boot show on the power LED and the
 * backlight (early.S), and no tick arrives.
 *
 * The board keeps the processor's identification and configuration
 * registers as the boot found them, so that they can be shown once the
 * display shows.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../asm.h"
#include "../bsp.h"
#include "../cons.h"
#include "../cpu.h"
#include "jz4770.h"

/*
 * The RAM the board uses: the bank at physical address 0, which KSEG0
 * and KSEG1 reach.  The second bank, at 0x30000000, is left unused.
 *
 * Values referenced: OpenDingux linux jz-6.1
 * arch/mips/boot/dts/ingenic/rg350.dts and its includes (the memory node,
 * 0x00000000 and 0x30000000, 256 MiB each).
 */
#define BOARD_MEMORY_SIZE	0x10000000U

/*
 * Config's bit that says the next Config register exists.
 */
#define BOARD_CONFIG_MORE	0x80000000U

/*
 * The processor's registers as the boot found them: PRId and Config0 to
 * Config3, each Config past Config0 zero when the one before says it
 * does not exist.  Set once by mips_board_boot(); read-only from then on.
 * The floating-point unit's FIR is printed by the HAL's unit set-up.
 */
static uint32_t board_processor_id;
static uint32_t board_config[4];

/*
 * The handoff the kernel's entry reads: a single boot device, the first
 * partition of an MBR disk.  Filled by mips_board_kernel_handoff() just
 * before the kernel starts.
 */
static struct kern_boot_handoff board_handoff;

static uint32_t read_config3(void);

/*
 * Records the processor's registers; the loader's arguments are not
 * read.
 */
void
mips_board_boot(
	uint32_t argument_count,
	uint32_t argument_table,
	uint32_t environment_table,
	uint32_t low_memory_size)
{
	/* The arguments lived in the loader's cache lines, which are gone. */
	UNUSED_PARAMETER(argument_count);
	UNUSED_PARAMETER(argument_table);
	UNUSED_PARAMETER(environment_table);
	UNUSED_PARAMETER(low_memory_size);

	/* The identification and Config0. */
	board_processor_id = mips_read_processor_id();
	board_config[0] = mips_read_config();

	/* Config1 to Config3, each when the one before says it exists. */
	if ((board_config[0] & BOARD_CONFIG_MORE) == 0)
		return;
	board_config[1] = mips_read_config1();
	if ((board_config[1] & BOARD_CONFIG_MORE) == 0)
		return;
	board_config[2] = mips_read_config2();
	if ((board_config[2] & BOARD_CONFIG_MORE) == 0)
		return;
	board_config[3] = read_config3();
}

/*
 * Sets up the board's devices: none yet, the display comes later.
 */
void
mips_board_init(
	void)
{
	/* Nothing to set up; the console keeps its grid. */
}

/*
 * Writes one character to the board's console.
 */
void
mips_board_putc(
	int character)
{
	/* The console keeps the grid until the display shows. */
	mips_cons_putc(character);
}

/*
 * Prints what the board found.
 */
void
mips_board_describe(
	void)
{
	/* The memory, then the processor's registers as the boot found them. */
	hal_printf(MIPS_HAL_NAME " RG350 memory=%u KiB\n", BOARD_MEMORY_SIZE / 1024U);
	hal_printf(MIPS_HAL_NAME " RG350 prid=%08x config0=%08x config1=%08x\n",
		   board_processor_id,
		   board_config[0],
		   board_config[1]);
	hal_printf(MIPS_HAL_NAME " RG350 config2=%08x config3=%08x\n",
		   board_config[2],
		   board_config[3]);
}

/*
 * Reports whether the display shows the console.
 */
bool
mips_board_display_ready(
	void)
{
	/* No display yet. */
	return false;
}

/*
 * Names the display in the bring-up's summary.
 */
const char *
mips_board_display_name(
	void)
{
	/* Succeeded: the SoC's LCD controller. */
	return "LCDC";
}

/*
 * Arms the tick: the SoC's timer is not driven yet, so no tick arrives.
 * Reports a nominal rate for the counter, which stays at zero.
 */
uint32_t
mips_board_timer_init(
	uint32_t hz)
{
	/* No timer is set up; the counter's nominal rate is the requested tick's, nonzero as the HAL requires. */
	return hz;
}

/*
 * Reads the 64-bit counter, which does not run yet.
 */
uint64_t
mips_board_counter_read(
	void)
{
	/* Succeeded: no counter yet. */
	return 0;
}

/*
 * Reports the lines of Status.IM the board's interrupts come on: the
 * interrupt controller's.
 */
uint32_t
mips_board_interrupt_mask(
	void)
{
	/* Succeeded: the controller's line. */
	return MIPS_INTERRUPT_BOARD;
}

/*
 * Reports the bytes of RAM from physical address 0.
 */
uint32_t
mips_board_memory_size(
	void)
{
	/* Succeeded: the low bank. */
	return BOARD_MEMORY_SIZE;
}

/*
 * Reports the bytes of the RAM the board uses.
 */
uint32_t
mips_board_memory_total(
	void)
{
	/* Succeeded: the low bank alone. */
	return BOARD_MEMORY_SIZE;
}

/*
 * Reports no initial RAM disk: the loader loads none.
 */
void
mips_board_ramdisk(
	uint32_t *physical,
	uint32_t *size)
{
	/* No RAM disk. */
	*physical = 0;
	*size = 0;
}

/*
 * Prepares and reports the handoff the kernel's entry reads.
 */
const void *
mips_board_kernel_handoff(
	void)
{
	/* The common handoff: one boot device, the first MBR partition, its id 0x80. */
	hal_memset(&board_handoff, 0, sizeof(board_handoff));
	board_handoff.magic = KERN_HANDOFF_MAGIC;
	board_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	board_handoff.size = sizeof(board_handoff);
	board_handoff.device_count = 1;
	board_handoff.boot_bios_id = 0x80;
	board_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_MBR;
	board_handoff.boot_partition_index = 1;
	board_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;

	/* Succeeded: the handoff, which lives as long as the image. */
	return &board_handoff;
}

/*
 * Masks every line of the interrupt controller: not driven yet.
 */
void
mips_board_irq_init(
	void)
{
	/* Nothing is let in. */
}

/*
 * Refuses every line: the interrupt controller is not driven yet.
 */
int
mips_board_irq_register(
	unsigned line,
	bool level,
	void (*handler)(void *),
	void *argument)
{
	/* No line can be registered. */
	UNUSED_PARAMETER(line);
	UNUSED_PARAMETER(level);
	UNUSED_PARAMETER(handler);
	UNUSED_PARAMETER(argument);

	/* Failed: no controller. */
	return -1;
}

/*
 * Serves the board's interrupts: none is let in yet.
 */
bool
mips_board_interrupt(
	uint32_t pending)
{
	/* No line is the board's. */
	UNUSED_PARAMETER(pending);

	/* Succeeded: nothing served. */
	return false;
}

/*
 * Stops the board: its watchdog reset is not driven yet.
 */
void
hal_reset(
	void)
{
	/* Stops with interrupts off. */
	mips_halt_forever();
}

/*
 * Stops the board: it has no power control the HAL drives.
 */
void
hal_poweroff(
	void)
{
	/* Stops with interrupts off. */
	mips_halt_forever();
}

/*
 * Reports a named value of the boot handoff: the board holds none.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	/* No command line and no other value. */
	UNUSED_PARAMETER(name);

	/* Failed: the board holds no value of that name. */
	return NULL;
}

/* Reads Config3 (coprocessor 0 register 16, select 3). */
static uint32_t
read_config3(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $16, 3" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}
