/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's firmware: the Open Firmware client interface, asked once,
 * before the HAL takes the machine, for the RAM, the time base's rate, the
 * display and the command line.  The answers make the kernel's handoff
 * and the named objects of hal_get_arch_handoff().
 *
 * Values referenced: IEEE Std 1275-1994 (Open Firmware), the client
 * interface's argument array and the finddevice, getprop, child, parent
 * and instance-to-package services; the PowerPC binding (the display's
 * width, height, linebytes, depth and address properties); the PCI bus
 * binding (the assigned-addresses and ranges encodings).
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../asm.h"
#include "bsp.h"

/* A missing node or property. */
#define FIRMWARE_FAILED		0xffffffffU

/* The most cells one call passes and returns. */
#define FIRMWARE_CELLS		8U

/* One call's argument array: the service's name, the counts, then the cells. */
struct firmware_arguments {
	uint32_t service;
	uint32_t argument_count;
	uint32_t return_count;
	uint32_t cells[FIRMWARE_CELLS];
};

/* What the firmware reported. */
static struct ppc64_board_info board;

/* The kernel's handoff and the command line, handed over as "boot.command-line". */
static struct kern_boot_handoff kernel_handoff;
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

static void firmware(const char *service, unsigned arguments, unsigned returns, const uint32_t *in, uint32_t *out);
static uint32_t finddevice(const char *path);
static uint32_t getprop(uint32_t node, const char *name, void *buffer, uint32_t length);
static uint32_t getprop_word(uint32_t node, const char *name);
static void read_display(void);
static uint64_t pci_memory_window(uint32_t node);
static int name_is(const char *name, const char *wanted);

/*
 * Asks the firmware what the HAL needs, and prepares the kernel's handoff.
 */
void
pseries_firmware_read(
	void)
{
	uint32_t node;
	uint32_t cells[4];
	uint32_t length;

	/* The first RAM bank of the memory node: two cells of address, two of size. */
	node = finddevice("/memory@0");
	if (node != FIRMWARE_FAILED) {
		length = getprop(node, "reg", cells, sizeof(cells));
		if (length != FIRMWARE_FAILED && length >= sizeof(cells)) {
			board.memory_base = ((uint64_t)cells[0] << 32) | cells[1];
			board.memory_size = ((uint64_t)cells[2] << 32) | cells[3];
		}
	}

	/* The time base's rate and the hashed table's size, from the first processor. */
	node = finddevice("/cpus");
	if (node != FIRMWARE_FAILED) {
		firmware("child", 1, 1, &node, cells);
		node = cells[0];
		if (node != FIRMWARE_FAILED && node != 0) {
			board.timebase_frequency = getprop_word(node, "timebase-frequency");
			length = getprop(node, "ibm,pft-size", cells, 2U * sizeof(uint32_t));
			if (length == 2U * sizeof(uint32_t))
				board.htab_shift = cells[1];
		}
	}

	/*
	 * The hypervisor keeps the hashed table and serves the idle wait; the
	 * kernel's windows use 64 KiB pages.
	 */
	board.under_hypervisor = 1;
	board.large_page_shift = 16;

	/* The display and the command line. */
	read_display();
	node = finddevice("/chosen");
	if (node != FIRMWARE_FAILED) {
		length = getprop(node, "bootargs", boot_command_line, sizeof(boot_command_line) - 1U);
		if (length == FIRMWARE_FAILED)
			length = 0;
		if (length >= sizeof(boot_command_line))
			length = sizeof(boot_command_line) - 1U;
		boot_command_line[length] = '\0';
	}

	/* The common handoff: one boot device, the first MBR partition, its id 0x80. */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = KERN_HANDOFF_MAGIC;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_count = 1;
	kernel_handoff.boot_bios_id = 0x80;
	kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_MBR;
	kernel_handoff.boot_partition_index = 1;
	kernel_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;
}

/*
 * Reports what the firmware said about the board.
 */
const struct ppc64_board_info *
ppc64_board(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &board;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const struct kern_boot_handoff *
pseries_kernel_handoff(
	void)
{
	/* Succeeded: the handoff filled at boot. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object: "boot.command-line" is
 * the firmware's command line, when it had one.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* A missing name names nothing. */
	if (name == NULL)
		return NULL;

	/* The command line, when the firmware passed one. */
	match = name_is(name, "boot.command-line");
	if (match && boot_command_line[0] != '\0')
		return boot_command_line;

	/* An unknown handoff. */
	return NULL;
}

/* Calls a client interface service. */
static void
firmware(
	const char *service,
	unsigned arguments,
	unsigned returns,
	const uint32_t *in,
	uint32_t *out)
{
	struct firmware_arguments call;
	unsigned index;

	/* The array: the name, the counts, the arguments, room for the returns. */
	hal_memset(&call, 0, sizeof(call));
	call.service = (uint32_t)(uintptr_t)service;
	call.argument_count = arguments;
	call.return_count = returns;
	for (index = 0; index < arguments; index++)
		call.cells[index] = in[index];

	/* The firmware answers in place. */
	ppc64_firmware_call(&call);
	for (index = 0; index < returns; index++)
		out[index] = call.cells[arguments + index];
}

/* Finds a node by its path or alias. */
static uint32_t
finddevice(
	const char *path)
{
	uint32_t in;
	uint32_t out;

	/* The node's handle, or the failure value. */
	in = (uint32_t)(uintptr_t)path;
	firmware("finddevice", 1, 1, &in, &out);
	return out;
}

/* Reads a property into a buffer; reports its size, or the failure value. */
static uint32_t
getprop(
	uint32_t node,
	const char *name,
	void *buffer,
	uint32_t length)
{
	uint32_t in[4];
	uint32_t out;

	/* The node, the name, the buffer and its length. */
	in[0] = node;
	in[1] = (uint32_t)(uintptr_t)name;
	in[2] = (uint32_t)(uintptr_t)buffer;
	in[3] = length;
	firmware("getprop", 4, 1, in, &out);
	return out;
}

/* Reads a one-cell property, zero when it is missing. */
static uint32_t
getprop_word(
	uint32_t node,
	const char *name)
{
	uint32_t value;
	uint32_t length;

	/* A property shorter than a cell counts as missing. */
	value = 0;
	length = getprop(node, name, &value, sizeof(value));
	if (length != sizeof(value))
		return 0;

	/* Succeeded: the cell. */
	return value;
}

/*
 * Reads the display's address and shape: the screen alias's node, or the
 * node behind the firmware's output.
 */
static void
read_display(
	void)
{
	uint32_t node;
	uint32_t chosen;
	uint32_t instance;
	uint32_t address[2];
	uint32_t length;

	/* The screen alias first. */
	node = finddevice("screen");
	if (node == FIRMWARE_FAILED || node == 0) {
		chosen = finddevice("/chosen");
		if (chosen == FIRMWARE_FAILED)
			return;
		instance = getprop_word(chosen, "stdout");
		if (instance == 0)
			return;
		firmware("instance-to-package", 1, 1, &instance, &node);
		if (node == FIRMWARE_FAILED || node == 0)
			return;
	}

	/*
	 * The properties the display's driver published; the address may take
	 * two cells, or be missing, when the device's first memory window is
	 * the framebuffer.
	 */
	length = getprop(node, "address", address, sizeof(address));
	if (length == sizeof(uint32_t))
		board.framebuffer_address = address[0];
	if (length == sizeof(address))
		board.framebuffer_address = ((uint64_t)address[0] << 32) | address[1];
	if (board.framebuffer_address == 0)
		board.framebuffer_address = pci_memory_window(node);
	board.framebuffer_width = getprop_word(node, "width");
	board.framebuffer_height = getprop_word(node, "height");
	board.framebuffer_linebytes = getprop_word(node, "linebytes");
	board.framebuffer_depth = getprop_word(node, "depth");
}

/*
 * Reports the processor's address of a PCI device's largest memory
 * window, the framebuffer of a display: its assigned address, through the
 * parent bridge's ranges.
 */
static uint64_t
pci_memory_window(
	uint32_t node)
{
	uint32_t assigned[5 * 6];
	uint32_t ranges[7 * 8];
	uint32_t length;
	uint32_t parent;
	uint32_t space;
	uint32_t kind;
	uint64_t bus;
	uint64_t child;
	uint64_t size;
	uint64_t largest;
	unsigned index;

	/* The largest memory entry of the assigned addresses: five cells each. */
	length = getprop(node, "assigned-addresses", assigned, sizeof(assigned));
	if (length == FIRMWARE_FAILED || length < 5U * sizeof(uint32_t))
		return 0;
	bus = 0;
	space = 0;
	largest = 0;
	for (index = 0; index + 5U <= length / sizeof(uint32_t); index += 5U) {
		kind = (assigned[index] >> 24) & 3U;
		size = ((uint64_t)assigned[index + 3U] << 32) | assigned[index + 4U];
		if ((kind == 2U || kind == 3U) && size > largest) {
			largest = size;
			space = kind;
			bus = ((uint64_t)assigned[index + 1U] << 32) | assigned[index + 2U];
		}
	}

	/* A device without a memory window has no framebuffer. */
	if (bus == 0)
		return 0;

	/* The bridge's ranges: three cells of bus address, two of processor address, two of size. */
	firmware("parent", 1, 1, &node, &parent);
	if (parent == FIRMWARE_FAILED || parent == 0)
		return 0;
	length = getprop(parent, "ranges", ranges, sizeof(ranges));
	if (length == FIRMWARE_FAILED)
		return 0;
	for (index = 0; index + 7U <= length / sizeof(uint32_t); index += 7U) {
		if (((ranges[index] >> 24) & 3U) != space)
			continue;
		child = ((uint64_t)ranges[index + 1U] << 32) | ranges[index + 2U];
		size = ((uint64_t)ranges[index + 5U] << 32) | ranges[index + 6U];
		if (bus >= child && bus - child < size)
			return (((uint64_t)ranges[index + 3U] << 32) | ranges[index + 4U]) + (bus - child);
	}

	/* No range holds it. */
	return 0;
}

/* Tells whether a handoff name is the one wanted. */
static int
name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Compares character by character, then requires the same end. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* The names are the same only when the name ends there too. */
	if (name[index] != '\0')
		return 0;
	return 1;
}
