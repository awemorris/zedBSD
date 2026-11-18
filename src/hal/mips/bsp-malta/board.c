/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board: what the emulator's loader passes, and the display.
 *
 * The loader runs a small bootloader of its own instead of the board's
 * firmware.  It enters the kernel with the number of entries of an
 * argument table, the table, an environment table of name and value
 * strings and the size of low memory, all in the RAM below the image.
 * The board copies the strings it uses at once, so that the memory they
 * are in can be given out later.  The second argument is the command
 * line; when an initial RAM disk was loaded its place and size come first
 * on it, and the board takes them off.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../asm.h"
#include "../bsp.h"
#include "../cons.h"
#include "../cpu.h"
#include "../io.h"
#include "framebuffer.h"

/*
 * The longest string of the loader's tables, with its terminator.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (ENVP_ENTRY_SIZE, the
 * 256 bytes of each entry).
 */
#define BOARD_STRING_SIZE	256U

/* The most environment entries read: the loader writes at most sixteen strings. */
#define BOARD_ENVIRONMENT_MAX	16U

/*
 * The largest low memory the loader reports: the RAM mapped at physical
 * address 0, at most 256 MiB.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (mips_malta_init, the
 * low RAM alias of at most 256 MiB).
 */
#define BOARD_LOW_MEMORY_MAX	0x10000000U

/*
 * The most RAM the board has, which the environment's ememsize gives.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (mips_malta_init, at
 * most 2 GiB of RAM; the environment's ememsize, the whole RAM).
 */
#define BOARD_MEMORY_MAX	0x80000000U

/*
 * The board's software reset: the FPGA's SOFTRES register, which resets
 * the board when the reset value is written to it.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (malta_fpga_write,
 * SOFTRES at FPGA offset 0x500 and the value 0x42; the FPGA at physical
 * 0x1f000000).
 */
#define BOARD_SOFT_RESET	0x1f000500U
#define BOARD_SOFT_RESET_VALUE	0x42U

/*
 * The words the loader puts before the command line for an initial RAM
 * disk.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (load_kernel, the
 * "rd_start=0x... rd_size=... " prefix).
 */
#define BOARD_RAMDISK_START	"rd_start="
#define BOARD_RAMDISK_SIZE	"rd_size="

/*
 * The command line as the kernel receives it, without the RAM disk's
 * words, and whether there is one.
 *
 * Filled once by mips_board_boot() before anything else runs; read-only
 * from then on.
 */
static char board_command_line[BOARD_STRING_SIZE];
static int board_has_command_line;

/*
 * The initial RAM disk's KSEG0 address and size in bytes from the command
 * line, both zero when none was loaded.  Set once by mips_board_boot().
 */
static uint32_t board_ramdisk_start;
static uint32_t board_ramdisk_size;

/*
 * The bytes of RAM from physical address 0: the environment's memsize, or
 * the loader's fourth argument without it.  Set once by mips_board_boot().
 */
static uint32_t board_low_memory;

/*
 * The bytes of the whole RAM: the environment's ememsize, or low memory
 * without it.  Never less than low memory.  Set once by mips_board_boot().
 */
static uint32_t board_total_memory;

/*
 * The handoff the kernel's entry reads: a single boot device, the first
 * partition of an MBR disk, the way the kernel finds its root on the
 * machines whose loader passes no device table.  Filled by
 * mips_board_kernel_handoff() just before the kernel starts.
 */
static struct kern_boot_handoff board_handoff;

/*
 * Nonzero once the display shows.  Set once by mips_board_init().
 */
static int board_display_on;

/*
 * The standard VGA as the console draws on it: 80 by 30 cells of the
 * 8x16 font.
 */
static const struct mips_cons_display board_display = {
	.columns = MALTA_CONSOLE_COLUMNS,
	.rows = MALTA_CONSOLE_ROWS,
	.cell = malta_framebuffer_cell,
	.cursor = malta_framebuffer_cursor,
};

static const char *table_string(uint32_t address);
static void copy_string(char *destination, const char *source);
static int strings_equal(const char *left, const char *right);
static int starts_with(const char *string, const char *prefix);
static const char *parse_number(const char *string, uint32_t *value);
static const char *skip_spaces(const char *string);
static void take_command_line(const char *line);

/*
 * Copies what the board uses from the loader's tables.
 *
 * The tables hold KSEG0 addresses of strings in low memory; an address
 * outside it is not read.
 */
void
mips_board_boot(
	uint32_t argument_count,
	uint32_t argument_table,
	uint32_t environment_table,
	uint32_t low_memory_size)
{
	const uint32_t *arguments;
	const uint32_t *environment;
	const char *name;
	const char *value;
	const char *line;
	const char *end;
	uint32_t memory;
	unsigned index;
	int matched;

	/* Low memory as the loader passed it, which bounds every address read below. */
	board_low_memory = low_memory_size;
	if (board_low_memory > BOARD_LOW_MEMORY_MAX)
		board_low_memory = BOARD_LOW_MEMORY_MAX;

	/* The command line is the second argument, when the loader gave one. */
	arguments = (const uint32_t *)table_string(argument_table);
	if (arguments != NULL && argument_count >= 2) {
		line = table_string(arguments[1]);
		if (line != NULL)
			take_command_line(line);
	}


	/* Without an environment, low memory is the loader's fourth argument and all of RAM. */
	board_total_memory = board_low_memory;
	environment = (const uint32_t *)table_string(environment_table);
	if (environment == NULL)
		return;

	/* Reads the environment's pairs of name and value up to its empty name. */
	for (index = 0; index + 1U < BOARD_ENVIRONMENT_MAX; index += 2U) {
		name = table_string(environment[index]);
		if (name == NULL)
			break;
		value = table_string(environment[index + 1U]);
		if (value == NULL)
			break;

		/* Only the sizes of low memory and of all RAM, in decimal, are used. */
		end = parse_number(value, &memory);
		if (end == NULL || memory == 0)
			continue;
		matched = strings_equal(name, "memsize");
		if (matched && memory <= BOARD_LOW_MEMORY_MAX)
			board_low_memory = memory;
		matched = strings_equal(name, "ememsize");
		if (matched && memory <= BOARD_MEMORY_MAX)
			board_total_memory = memory;
	}

	/* All of RAM holds low memory at least. */
	if (board_total_memory < board_low_memory)
		board_total_memory = board_low_memory;
}

/*
 * Sets up the display and draws on it what the console has printed.
 */
void
mips_board_init(
	void)
{
	int error;

	/* Sets the standard VGA up; without it the console only keeps its grid. */
	error = malta_framebuffer_init();
	if (error != HAL_OK)
		return;

	/* Shows the lines printed before the display was set up. */
	board_display_on = 1;
	mips_cons_attach(&board_display);
}

/*
 * Writes one character to the board's console.
 */
void
mips_board_putc(
	int character)
{
	/* The console keeps the grid before the display shows and draws it after. */
	mips_cons_putc(character);
}

/*
 * Prints what the board found.
 */
void
mips_board_describe(
	void)
{
	/* The memory and the command line the loader passed. */
	hal_printf(MIPS_HAL_NAME " MALTA memory=%u KiB total=%u KiB ramdisk=%08x+%u\n",
		   board_low_memory / 1024U,
		   board_total_memory / 1024U,
		   board_ramdisk_start,
		   board_ramdisk_size);
	if (board_has_command_line)
		hal_printf(MIPS_HAL_NAME " MALTA command line: %s\n", board_command_line);

	/* The display, when it shows. */
	if (board_display_on)
		malta_framebuffer_describe();
}

/*
 * Reports whether the display shows the console.
 */
bool
mips_board_display_ready(
	void)
{
	/* The display was set up by mips_board_init(). */
	if (board_display_on)
		return true;

	/* No display: the console stays on the serial line. */
	return false;
}

/*
 * Names the display in the bring-up's summary.
 */
const char *
mips_board_display_name(
	void)
{
	/* Succeeded: the emulator's standard VGA. */
	return "VGA";
}

/*
 * Shows nothing of an exception taken before the trap handling exists:
 * the console reports it, and the emulator's screen shows the console.
 */
void
mips_board_early_fault(
	uint32_t code)
{
	UNUSED_PARAMETER(code);
}

/*
 * Reports the bytes of RAM from physical address 0.
 */
uint32_t
mips_board_memory_size(
	void)
{
	/* Succeeded: what the loader reported, within the low RAM alias. */
	return board_low_memory;
}

/*
 * Reports the bytes of the whole RAM, low memory and what is past it.
 */
uint32_t
mips_board_memory_total(
	void)
{
	/* Succeeded: what the loader reported, at most the board's largest RAM. */
	return board_total_memory;
}

/*
 * Reports the physical place and size of the initial RAM disk, both
 * zero when the loader loaded none.
 */
void
mips_board_ramdisk(
	uint32_t *physical,
	uint32_t *size)
{
	/* No RAM disk, or one whose KSEG0 address is not in the segment. */
	*physical = 0;
	*size = 0;
	if (board_ramdisk_size == 0 || board_ramdisk_start < MIPS_KSEG0_BASE)
		return;

	/* Succeeded: the RAM disk's KSEG0 address less the segment's base. */
	*physical = board_ramdisk_start - MIPS_KSEG0_BASE;
	*size = board_ramdisk_size;
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
 * Resets the board through the FPGA's software reset.
 */
void
hal_reset(
	void)
{
	/* Writes the reset value; the board restarts at once. */
	mips_io_write32(BOARD_SOFT_RESET, BOARD_SOFT_RESET_VALUE);

	/* Stops, should the write not have reset the board. */
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
 * Reports a named value of the boot handoff.
 *
 * The board holds boot.command-line, the command line without the
 * initial RAM disk's words; it is NULL when the loader gave none.  Any
 * other name is NULL.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int matched;

	/* Refuses a missing name. */
	if (name == NULL)
		return NULL;

	/* The command line, when there is one. */
	matched = strings_equal(name, "boot.command-line");
	if (matched) {
		if (!board_has_command_line)
			return NULL;

		/* Succeeded: the board's copy of the command line. */
		return board_command_line;
	}

	/* Failed: the board holds no value of that name. */
	return NULL;
}

/*
 * Reports a KSEG0 address of the loader's tables as a pointer, or NULL
 * when it is not in low memory.
 */
static const char *
table_string(
	uint32_t address)
{
	/* Below KSEG0, or past low memory, is not the loader's. */
	if (address < MIPS_KSEG0_BASE)
		return NULL;
	if (address - MIPS_KSEG0_BASE >= board_low_memory)
		return NULL;

	/* Succeeded: the address, which KSEG0 (on 64 bits CKSEG0) maps as it is. */
	return (const char *)mips_segment_pointer(address);
}

/* Copies a string of the loader's into a buffer of BOARD_STRING_SIZE bytes. */
static void
copy_string(
	char *destination,
	const char *source)
{
	unsigned index;

	/* Copies up to the terminator or the buffer's last byte, then terminates. */
	for (index = 0; index + 1U < BOARD_STRING_SIZE && source[index] != '\0'; index++)
		destination[index] = source[index];
	destination[index] = '\0';
}

/* Reports whether two strings are the same. */
static int
strings_equal(
	const char *left,
	const char *right)
{
	unsigned index;

	/* Compares byte by byte up to the first terminator. */
	for (index = 0; left[index] == right[index]; index++) {
		if (left[index] == '\0')
			return 1;
	}

	/* The strings differ at index. */
	return 0;
}

/* Reports whether a string begins with a prefix. */
static int
starts_with(
	const char *string,
	const char *prefix)
{
	unsigned index;

	/* Compares the prefix's bytes. */
	for (index = 0; prefix[index] != '\0'; index++) {
		if (string[index] != prefix[index])
			return 0;
	}

	/* Every byte of the prefix matched. */
	return 1;
}

/*
 * Reads a decimal number, or a hexadecimal one after 0x, and reports
 * where it ends, or NULL when no digit is there.
 */
static const char *
parse_number(
	const char *string,
	uint32_t *value)
{
	uint32_t base;
	uint32_t number;
	uint32_t digit;
	int digits;

	/* A 0x prefix makes the number hexadecimal. */
	base = 10;
	if (string[0] == '0' && (string[1] == 'x' || string[1] == 'X')) {
		base = 16;
		string += 2;
	}

	/* Collects the digits of the base. */
	number = 0;
	digits = 0;
	for (;;) {
		if (*string >= '0' && *string <= '9') {
			digit = (uint32_t)(*string - '0');
		} else if (base == 16 && *string >= 'a' && *string <= 'f') {
			digit = (uint32_t)(*string - 'a') + 10U;
		} else if (base == 16 && *string >= 'A' && *string <= 'F') {
			digit = (uint32_t)(*string - 'A') + 10U;
		} else {
			break;
		}

		/* Appends the digit to the number. */
		number = number * base + digit;
		digits++;
		string++;
	}

	/* Refuses a number without a digit. */
	if (digits == 0)
		return NULL;

	/* Succeeded: the number and the byte after it. */
	*value = number;
	return string;
}

/* Reports the first byte of a string that is not a space. */
static const char *
skip_spaces(
	const char *string)
{
	/* Steps over the spaces. */
	while (*string == ' ')
		string++;

	/* Succeeded: the first other byte. */
	return string;
}

/*
 * Keeps the command line, without the initial RAM disk's words in front
 * of it.
 */
static void
take_command_line(
	const char *line)
{
	const char *rest;
	uint32_t number;
	int matched;

	/* The RAM disk's place, in hexadecimal. */
	line = skip_spaces(line);
	matched = starts_with(line, BOARD_RAMDISK_START);
	if (matched) {
		rest = parse_number(line + sizeof(BOARD_RAMDISK_START) - 1U, &number);
		if (rest != NULL) {
			board_ramdisk_start = number;
			line = skip_spaces(rest);
		}
	}

	/* The RAM disk's size, in decimal. */
	matched = starts_with(line, BOARD_RAMDISK_SIZE);
	if (matched) {
		rest = parse_number(line + sizeof(BOARD_RAMDISK_SIZE) - 1U, &number);
		if (rest != NULL) {
			board_ramdisk_size = number;
			line = skip_spaces(rest);
		}
	}

	/* An empty remainder is no command line. */
	if (*line == '\0')
		return;

	/* Keeps the remainder. */
	copy_string(board_command_line, line);
	board_has_command_line = 1;
}
