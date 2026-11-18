/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC Mac's firmware: Apple's Open Firmware, or the
 * emulator's OpenBIOS, asked once through its client interface before the
 * HAL takes the machine, for the model, the RAM, the time base's rate,
 * the display and the command line.  The firmware runs in 32-bit mode,
 * so every address handed to it is below 4 GiB: the image, its stack and
 * the strings it passes all are.
 *
 * The RAM is the memory node's reg, laid out by the root's
 * #address-cells and #size-cells (one and one on the emulator, two and
 * one on the 970 machines); the HAL takes the run of banks that follow
 * each other from the first one's start.  The display is the screen
 * alias's node when that is a display, else the first display node of
 * the tree.  Its framebuffer is its address property, or else its
 * largest assigned memory window translated through the parent bridge's
 * ranges, whose parent address takes the cells the bridge's parent
 * names.
 *
 * Values referenced: IEEE Std 1275-1994 (Open Firmware), the client
 * interface's argument array and the finddevice, getprop, child, peer,
 * parent and instance-to-package services; the PowerPC binding (the
 * display's width, height, linebytes, depth and address properties,
 * /chosen's stdout and bootargs, a processor's timebase-frequency); the
 * PCI bus binding (the assigned-addresses and ranges encodings).
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "../asm.h"
#include "bsp.h"

/* A missing node or property. */
#define FIRMWARE_FAILED		0xffffffffU

/* The most cells one call passes and returns. */
#define FIRMWARE_CELLS		8U

/* The most cells of a memory node's reg the HAL reads: eight banks of three. */
#define MEMORY_CELLS		24U

/* The most nodes the search for a display visits. */
#define DISPLAY_SEARCH_LIMIT	512U

/* One call's argument array: the service's name, the counts, then the cells. */
struct firmware_arguments {
	uint32_t service;
	uint32_t argument_count;
	uint32_t return_count;
	uint32_t cells[FIRMWARE_CELLS];
};

/*
 * What the firmware reported, filled once at boot before the HAL takes
 * the machine and read-only afterwards.
 */
static struct ppc64_board_info board;

/*
 * The machine's model, the root's model property ("PowerMac7,3" on an
 * Early 2005 PowerMac G5), filled once at boot.
 */
static char board_model[64];

/*
 * The kernel's handoff and the command line, handed over as
 * "boot.command-line"; both are filled once at boot.
 */
static struct kern_boot_handoff kernel_handoff;
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

static void firmware(const char *service, unsigned arguments, unsigned returns, const uint32_t *in, uint32_t *out);
static uint32_t finddevice(const char *path);
static uint32_t getprop(uint32_t node, const char *name, void *buffer, uint32_t length);
static uint32_t getprop_word(uint32_t node, const char *name);
static uint32_t tree_step(const char *service, uint32_t node);
static void read_memory(void);
static void read_display(void);
static int is_display(uint32_t node);
static uint32_t find_display(void);
static uint64_t pci_memory_window(uint32_t node);
static uint64_t read_cells(const uint32_t *cells, uint32_t count);
static int name_is(const char *name, const char *wanted);

/*
 * Asks the firmware what the HAL needs, and prepares the kernel's handoff.
 */
void
macppc64_firmware_read(
	void)
{
	uint32_t node;
	uint32_t length;

	/* The model, which says which machine this is. */
	node = finddevice("/");
	if (node != FIRMWARE_FAILED) {
		length = getprop(node, "model", board_model, sizeof(board_model) - 1U);
		if (length == FIRMWARE_FAILED || length >= sizeof(board_model))
			length = 0;
		board_model[length] = '\0';
	}

	/* The RAM, then the time base's rate from the first processor. */
	read_memory();
	node = finddevice("/cpus");
	node = tree_step("child", node);
	if (node != FIRMWARE_FAILED && node != 0)
		board.timebase_frequency = getprop_word(node, "timebase-frequency");

	/*
	 * The processor runs the hashed table directly; the board's start
	 * gives its place.  The kernel's windows use the 970's 16 MiB page.
	 */
	board.under_hypervisor = 0;
	board.large_page_shift = 24;

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

	/* The common handoff: one boot device, the first partition, its id 0x80. */
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
 * Gives the board's record the hashed table's place, which the board's
 * start sets aside.
 */
void
macppc64_set_table(
	uint64_t base,
	uint32_t shift)
{
	/* The table's origin and the power of two of its size. */
	board.htab_base = base;
	board.htab_shift = shift;
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
 * Reports the machine's model, empty when the firmware gave none.
 */
const char *
macppc64_model(
	void)
{
	/* Succeeded: the model read at boot. */
	return board_model;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const struct kern_boot_handoff *
macppc64_kernel_handoff(
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

/* Moves through the tree: a node's first child, next peer or parent; zero when there is none. */
static uint32_t
tree_step(
	const char *service,
	uint32_t node)
{
	uint32_t next;

	/* A missing node has no neighbours. */
	if (node == FIRMWARE_FAILED || node == 0)
		return 0;

	/* The firmware's answer, with its failure value read as none. */
	firmware(service, 1, 1, &node, &next);
	if (next == FIRMWARE_FAILED)
		return 0;

	/* Succeeded: the neighbour, or zero. */
	return next;
}

/*
 * Reads the RAM: the run of banks that follow each other from the first
 * bank's start.  A bank after a gap is not used yet.
 */
static void
read_memory(
	void)
{
	uint32_t cells[MEMORY_CELLS];
	uint32_t node;
	uint32_t length;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t entry;
	uint32_t count;
	uint32_t index;
	uint32_t value;
	uint64_t start;
	uint64_t size;
	uint64_t end;
	bool grew;

	/* The root's cells: one of address and one of size unless it says otherwise. */
	node = finddevice("/");
	address_cells = 1;
	size_cells = 1;
	if (node != FIRMWARE_FAILED) {
		value = getprop_word(node, "#address-cells");
		if (value != 0)
			address_cells = value;
		value = getprop_word(node, "#size-cells");
		if (value != 0)
			size_cells = value;
	}

	/* An address or a size wider than two cells is not one the HAL reads. */
	if (address_cells > 2U || size_cells > 2U)
		return;
	entry = address_cells + size_cells;

	/* The banks. */
	node = finddevice("/memory");
	if (node == FIRMWARE_FAILED)
		return;
	length = getprop(node, "reg", cells, sizeof(cells));
	if (length == FIRMWARE_FAILED || length < entry * 4U)
		return;
	if (length > sizeof(cells))
		length = sizeof(cells);
	count = length / (entry * 4U);
	board.memory_base = read_cells(&cells[0], address_cells);
	end = board.memory_base;

	/* The run from the first bank: each bank that starts where the run ends lengthens it. */
	do {
		grew = false;
		for (index = 0; index < count; index++) {
			start = read_cells(&cells[index * entry], address_cells);
			size = read_cells(&cells[index * entry + address_cells], size_cells);
			if (size == 0 || start != end)
				continue;
			end += size;
			grew = true;
		}
	} while (grew);
	board.memory_size = end - board.memory_base;
}

/*
 * Reads the display's address and shape: the screen alias's node when it
 * is a display, else the first display node of the tree.
 */
static void
read_display(
	void)
{
	uint32_t node;
	uint32_t address[2];
	uint32_t length;
	int display;

	/* The screen alias first, then a search. */
	node = finddevice("screen");
	display = is_display(node);
	if (!display)
		node = find_display();
	if (node == 0)
		return;

	/*
	 * The properties the display's driver published; the address may take
	 * two cells, or be missing, when the device's largest memory window is
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

/* Tells whether a node's device type is a display. */
static int
is_display(
	uint32_t node)
{
	char type[16];
	uint32_t length;
	int match;

	/* A missing node is no display. */
	if (node == FIRMWARE_FAILED || node == 0)
		return 0;

	/* The device type, compared as a string. */
	hal_memset(type, 0, sizeof(type));
	length = getprop(node, "device_type", type, sizeof(type) - 1U);
	if (length == FIRMWARE_FAILED)
		return 0;
	match = name_is(type, "display");

	/* Succeeded: whether the type is the display's. */
	return match;
}

/*
 * Finds the first display node of the tree, depth first; zero when there
 * is none.
 */
static uint32_t
find_display(
	void)
{
	uint32_t node;
	uint32_t next;
	unsigned visited;
	int display;

	/* Walks the tree from the root's first child: down, else across, else up and across. */
	node = finddevice("/");
	node = tree_step("child", node);
	for (visited = 0; node != 0 && visited < DISPLAY_SEARCH_LIMIT; visited++) {
		display = is_display(node);
		if (display)
			return node;

		/* The first child, when there is one. */
		next = tree_step("child", node);
		if (next != 0) {
			node = next;
			continue;
		}

		/* Otherwise the next peer of this node or of the nearest ancestor that has one. */
		next = tree_step("peer", node);
		while (next == 0 && node != 0) {
			node = tree_step("parent", node);
			next = tree_step("peer", node);
		}
		node = next;
	}

	/* No display in the tree. */
	return 0;
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
	uint32_t grandparent;
	uint32_t parent_cells;
	uint32_t entry;
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

	/* The bridge, and the cells its parent gives an address. */
	parent = tree_step("parent", node);
	if (parent == 0)
		return 0;
	grandparent = tree_step("parent", parent);
	parent_cells = 1;
	if (grandparent != 0)
		parent_cells = getprop_word(grandparent, "#address-cells");
	if (parent_cells == 0 || parent_cells > 2U)
		return 0;

	/* The bridge's ranges: three cells of bus address, the parent's address, two of size. */
	entry = 3U + parent_cells + 2U;
	length = getprop(parent, "ranges", ranges, sizeof(ranges));
	if (length == FIRMWARE_FAILED)
		return 0;
	for (index = 0; index + entry <= length / sizeof(uint32_t); index += entry) {
		if (((ranges[index] >> 24) & 3U) != space)
			continue;
		child = ((uint64_t)ranges[index + 1U] << 32) | ranges[index + 2U];
		size = read_cells(&ranges[index + 3U + parent_cells], 2U);
		if (bus >= child && bus - child < size)
			return read_cells(&ranges[index + 3U], parent_cells) + (bus - child);
	}

	/* No range holds it. */
	return 0;
}

/* Reads a number of one or two cells, the first the most significant. */
static uint64_t
read_cells(
	const uint32_t *cells,
	uint32_t count)
{
	/* One cell is the whole number. */
	if (count == 1U)
		return cells[0];

	/* Two cells: the high one, then the low one. */
	return ((uint64_t)cells[0] << 32) | cells[1];
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
