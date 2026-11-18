/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * OpenFirmware to zedBSD sun4u handoff builder.
 */

#include "handoff.h"
#include "ofw.h"

/* The largest property the builder reads. */
#define PROPERTY_BUFFER_SIZE 512U

/* The longest device path the builder reads from /aliases. */
#define DISPLAY_PATH_SIZE 256U

/*
 * The emulator's standard VGA: its PCI IDs, the offset of the byte order
 * register in its register window (BAR 2), and the register's values for
 * little-endian and big-endian pixels.
 *
 * Values referenced: QEMU v10.0.0 hw/display/vga-pci.c (PCI_VGA_QEXT_OFFSET,
 * PCI_VGA_QEXT_REG_BYTEORDER, PCI_VGA_QEXT_LITTLE_ENDIAN,
 * PCI_VGA_QEXT_BIG_ENDIAN) and include/hw/pci/pci_ids.h (the vendor 0x1234
 * and the device 0x1111).
 */
#define QEMU_VGA_VENDOR			0x1234UL
#define QEMU_VGA_DEVICE			0x1111UL
#define QEMU_VGA_QEXT_BYTEORDER		0x604U
#define QEMU_VGA_QEXT_LITTLE_ENDIAN	0x1e1e1e1eU
#define QEMU_VGA_QEXT_BIG_ENDIAN	0xbebebebeU

/*
 * The physical bypass address space identifier, little-endian, that reads
 * a device register without the caches.
 *
 * Values referenced: UltraSPARC IIi User's Manual (ASI_PHYS_BYPASS_EC_E_L,
 * 0x1d).
 */
#define ASI_PHYS_BYPASS_EC_E_L		0x1d

/* The 8042's command port lies four ports above its data port. */
#define I8042_COMMAND_DISTANCE		4U

/*
 * The walk of the device tree that fills one handoff.
 *
 * It lives for one sparcv9_handoff_build() call; each flag says that part
 * of the handoff has been found, so the first device of a kind wins.
 */
struct discovery {
	struct sun4u_boot_handoff *handoff;
	int have_cpu;
	int have_memory;
	int have_pci_io;
	int have_pci_memory;
	int have_serial;
	int have_ide;
	int have_display;
	int have_i8042;
};

static unsigned long be32(const unsigned char *bytes);
static unsigned long long cells64(const unsigned char *bytes, unsigned int cells);
static void clear_bytes(void *destination, unsigned long size);
static int same_string(const char *left, const char *right);
static int property_string(ofw_cell_t node, const char *name, char *buffer, unsigned long size);
static int property_u32(ofw_cell_t node, const char *name, unsigned long *value);
static int address_cells(ofw_cell_t node, const char *name, unsigned int fallback);
static int memory_ranges(ofw_cell_t node, const char *name, struct sun4u_memory_range *ranges, unsigned char *count);
static void discover_pci_ranges(ofw_cell_t node, struct discovery *state);
static void discover_serial(ofw_cell_t node, struct discovery *state);
static void discover_ide(ofw_cell_t node, struct discovery *state, unsigned long vendor, unsigned long device);
static void discover_display(ofw_cell_t node, struct discovery *state, unsigned long vendor, unsigned long device);
static void discover_i8042(ofw_cell_t node, struct discovery *state);
static int find_display(ofw_scell_t *node, ofw_scell_t *instance);
static int framebuffer_bar(ofw_cell_t node, unsigned long long pci_memory_base, unsigned long long *base, unsigned long long *size);
static int framebuffer_address(ofw_cell_t node, const struct discovery *state, unsigned long long *physical);
static unsigned long qemu_vga_format(const struct discovery *state);
static int set_rgb332_palette(ofw_cell_t instance);
static unsigned long phys_read32_le(unsigned long long address);
static void discover_framebuffer(struct discovery *state);
static void discover_cpu(ofw_cell_t node, struct discovery *state);
static void discover_memory(ofw_cell_t node, struct discovery *state);
static int walk_node(ofw_cell_t node, struct discovery *state, unsigned int depth);
static void copy_bootpath(char *destination, const char *source);

/*
 * Fills a handoff from OpenFirmware's device tree and the boot path.
 *
 * Returns 0, or -1 when the tree lacks a device the HAL needs.
 */
int
sparcv9_handoff_build(
	struct sun4u_boot_handoff *handoff,
	const char *bootpath)
{
	struct discovery state;
	ofw_scell_t root;
	int walked;

	/* Fills the common record: one SCSI-like boot device and its Sun partition. */
	clear_bytes(handoff, sizeof(*handoff));
	handoff->common.magic = KERN_HANDOFF_MAGIC;
	handoff->common.version = SUN4U_HANDOFF_COMMON_VERSION;
	handoff->common.size = sizeof(*handoff);
	handoff->common.device_count = 1;
	handoff->common.boot_bios_id = 0x80;
	handoff->common.boot_partition_scheme = KERN_PARTITION_SCHEME_SUN;
	handoff->common.boot_partition_index = 1;
	handoff->common.boot_partition_lba = 4096;

	/* Fills the sun4u extension's identity and the boot path. */
	handoff->extension_magic = SUN4U_HANDOFF_EXTENSION_MAGIC;
	handoff->extension_version = SUN4U_HANDOFF_EXTENSION_VERSION;
	handoff->extension_size = sizeof(*handoff) - sizeof(handoff->common);
	copy_bootpath(handoff->bootpath, bootpath);

	/* Starts the walk with nothing found. */
	state.handoff = handoff;
	state.have_cpu = 0;
	state.have_memory = 0;
	state.have_pci_io = 0;
	state.have_pci_memory = 0;
	state.have_serial = 0;
	state.have_ide = 0;
	state.have_display = 0;
	state.have_i8042 = 0;

	/* Refuses a firmware without a root node. */
	root = ofw_finddevice("/");
	if (root <= 0)
		return -1;

	/* Walks the whole tree; a tree too deep is refused. */
	walked = walk_node((ofw_cell_t)root, &state, 0);
	if (walked != 0)
		return -1;

	/* Refuses a tree that lacks a device the HAL needs; the display is optional. */
	if (!state.have_cpu ||
	    !state.have_memory ||
	    !state.have_pci_io ||
	    !state.have_serial ||
	    !state.have_ide)
		return -1;

	/* A display is usable only where the PCI memory space is known. */
	if (!state.have_pci_memory)
		handoff->display_vendor = 0;

	/* Takes the framebuffer the firmware drew on, when it can be handed over. */
	discover_framebuffer(&state);

	/* Succeeded: the handoff describes the machine. */
	return 0;
}

/* Reads a big-endian 32-bit cell. */
static unsigned long
be32(
	const unsigned char *bytes)
{
	/* Combines the four bytes, the most significant first. */
	return (unsigned long)bytes[0] << 24 |
		(unsigned long)bytes[1] << 16 |
		(unsigned long)bytes[2] << 8 | bytes[3];
}

/* Reads a value of a number of 32-bit cells, the most significant first. */
static unsigned long long
cells64(
	const unsigned char *bytes,
	unsigned int cells)
{
	unsigned long long value;

	/* Shifts each cell in. */
	value = 0;
	while (cells != 0) {
		value = value << 32 | be32(bytes);
		bytes += 4;
		cells--;
	}

	/* Reports the value. */
	return value;
}

/* Clears bytes without the C library. */
static void
clear_bytes(
	void *destination,
	unsigned long size)
{
	unsigned char *bytes;

	/* Clears the bytes one at a time. */
	bytes = destination;
	while (size != 0) {
		*bytes = 0;
		bytes++;
		size--;
	}
}

/* Reports whether two strings are equal. */
static int
same_string(
	const char *left,
	const char *right)
{
	/* Steps over the common start. */
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	/* The strings differ where the first one stopped. */
	if (*left != *right)
		return 0;

	/* Succeeded: the strings are equal. */
	return 1;
}

/* Reads a string property into a buffer and terminates it. */
static int
property_string(
	ofw_cell_t node,
	const char *name,
	char *buffer,
	unsigned long size)
{
	int length;

	/* Refuses a buffer too small for a character and the terminator. */
	if (size < 2)
		return -1;

	/* Refuses a missing or empty property, and one that fills the buffer. */
	length = ofw_getprop(node, name, buffer, size - 1U);
	if (length <= 0 || (unsigned long)length >= size)
		return -1;

	/* Terminates the string. */
	buffer[length] = '\0';

	/* Succeeded: the buffer holds the string. */
	return 0;
}

/* Reads a 32-bit integer property. */
static int
property_u32(
	ofw_cell_t node,
	const char *name,
	unsigned long *value)
{
	unsigned char bytes[4];
	int length;

	/* Refuses a property that is not one cell. */
	length = ofw_getprop(node, name, bytes, sizeof(bytes));
	if (length != 4)
		return -1;

	/* Decodes the cell. */
	*value = be32(bytes);

	/* Succeeded: the value is read. */
	return 0;
}

/* Reads a #address-cells or #size-cells property, or the fallback when it is missing or unusable. */
static int
address_cells(
	ofw_cell_t node,
	const char *name,
	unsigned int fallback)
{
	unsigned long value;
	int status;

	/* A missing property, zero cells, or more than four use the fallback. */
	status = property_u32(node, name, &value);
	if (status != 0 || value == 0 || value > 4)
		return (int)fallback;

	/* Reports the cell count. */
	return (int)value;
}

/* Reads a memory node's "reg" or "available" ranges. */
static int
memory_ranges(
	ofw_cell_t node,
	const char *name,
	struct sun4u_memory_range *ranges,
	unsigned char *count)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	const unsigned char *entry;
	ofw_scell_t parent;
	unsigned int address_count;
	unsigned int size_count;
	unsigned int stride;
	unsigned int entries;
	unsigned int index;
	int length;

	/* The parent's cell counts give the layout of each range. */
	parent = ofw_parent(node);
	if (parent <= 0)
		return -1;

	/* Reads the cell counts and the property. */
	address_count = (unsigned int)address_cells((ofw_cell_t)parent, "#address-cells", 2);
	size_count = (unsigned int)address_cells((ofw_cell_t)parent, "#size-cells", 2);
	stride = (address_count + size_count) * 4U;
	length = ofw_getprop(node, name, bytes, sizeof(bytes));
	if (length <= 0 || stride == 0 || (unsigned int)length % stride != 0)
		return -1;

	/* Refuses no ranges or more than the handoff holds. */
	entries = (unsigned int)length / stride;
	if (entries == 0 || entries > SUN4U_MAX_MEMORY_RANGES)
		return -1;

	/* Decodes each range, refusing an empty one. */
	for (index = 0; index < entries; index++) {
		entry = bytes + index * stride;
		ranges[index].base = cells64(entry, address_count);
		ranges[index].size = cells64(entry + address_count * 4U, size_count);
		if (ranges[index].size == 0)
			return -1;
	}

	/* Publishes the count. */
	*count = (unsigned char)entries;

	/* Succeeded: the ranges are read. */
	return 0;
}

/*
 * Finds the physical bases of the PCI I/O space and of the 32-bit memory
 * space from the first PCI node's "ranges".
 */
static void
discover_pci_ranges(
	ofw_cell_t node,
	struct discovery *state)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	const unsigned char *entry;
	ofw_scell_t parent;
	unsigned int child_address;
	unsigned int parent_address;
	unsigned int size_cells;
	unsigned int stride;
	unsigned int offset;
	unsigned long space;
	unsigned long long child;
	unsigned long long physical;
	int length;

	/* The first PCI node found gives the bases; the parent's cells are needed to read them. */
	parent = ofw_parent(node);
	if (parent <= 0 || state->have_pci_io)
		return;

	/* Reads the cell counts and the ranges. */
	child_address = (unsigned int)address_cells(node, "#address-cells", 3);
	size_cells = (unsigned int)address_cells(node, "#size-cells", 2);
	parent_address = (unsigned int)address_cells((ofw_cell_t)parent, "#address-cells", 2);
	stride = (child_address + parent_address + size_cells) * 4U;
	length = ofw_getprop(node, "ranges", bytes, sizeof(bytes));
	if (length <= 0 || stride == 0 || (unsigned int)length % stride != 0)
		return;

	/* Takes the first I/O space range and the first 32-bit memory space range. */
	for (offset = 0; offset < (unsigned int)length; offset += stride) {
		/* Only a range of the expected cell layout gives a base. */
		entry = bytes + offset;
		space = be32(entry) >> 24 & 3U;
		if (child_address < 3 || parent_address > 2)
			continue;

		/* A range mapped below its own bus address gives no base. */
		child = cells64(entry + 4U, child_address - 1U);
		physical = cells64(entry + child_address * 4U, parent_address);
		if (physical < child)
			continue;

		/* The base is where bus address zero of the space lies. */
		if (space == 1 && !state->have_pci_io) {
			state->handoff->pci_io_base = physical - child;
			state->have_pci_io = 1;
		} else if (space == 2 && !state->have_pci_memory) {
			state->handoff->pci_memory_base = physical - child;
			state->have_pci_memory = 1;
		}
	}
}

/* Finds the serial port's I/O offset from its "reg". */
static void
discover_serial(
	ofw_cell_t node,
	struct discovery *state)
{
	unsigned char bytes[32];
	ofw_scell_t parent;
	unsigned int address_count;
	int length;

	/* The first serial port found is the console. */
	if (state->have_serial)
		return;

	/* The parent's cells are needed to read the address. */
	parent = ofw_parent(node);
	if (parent <= 0)
		return;

	/* Reads the address, refusing a "reg" shorter than one. */
	address_count = (unsigned int)address_cells((ofw_cell_t)parent, "#address-cells", 2);
	length = ofw_getprop(node, "reg", bytes, sizeof(bytes));
	if (length < (int)(address_count * 4U) || address_count == 0)
		return;

	/* Records the port's offset. */
	state->handoff->serial_io_offset = (uint32_t)cells64(bytes, address_count);
	state->have_serial = 1;
}

/* Finds the CMD646 IDE controller's four I/O port ranges. */
static void
discover_ide(
	ofw_cell_t node,
	struct discovery *state,
	unsigned long vendor,
	unsigned long device)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	unsigned short ports[4];
	unsigned int found;
	unsigned int offset;
	unsigned long space;
	unsigned long address;
	int length;

	/* Only the first CMD646 is taken. */
	found = 0;
	if (state->have_ide || vendor != 0x1095 || device != 0x0646)
		return;

	/* Refuses assigned addresses that are not whole PCI address entries. */
	length = ofw_getprop(node, "assigned-addresses", bytes, sizeof(bytes));
	if (length <= 0 || (unsigned int)length % 20U != 0)
		return;

	/* Takes the first four I/O space ranges as the controller's ports. */
	for (offset = 0; offset < (unsigned int)length && found < 4; offset += 20U) {
		space = be32(bytes + offset) >> 24 & 3U;
		address = be32(bytes + offset + 8U);
		if (space == 1 && address <= 0xffffU) {
			ports[found] = (unsigned short)address;
			found++;
		}
	}

	/* A controller without all four ports is not used. */
	if (found != 4)
		return;

	/* Records the controller and its ports. */
	state->handoff->ide_vendor = (uint16_t)vendor;
	state->handoff->ide_device = (uint16_t)device;
	state->handoff->ide_primary_command = ports[0];
	state->handoff->ide_primary_control = ports[1];
	state->handoff->ide_secondary_command = ports[2];
	state->handoff->ide_secondary_control = ports[3];
	state->have_ide = 1;
}

/*
 * Finds the emulator's standard VGA: its linear framebuffer (BAR 0) and
 * its register window (BAR 2), as PCI memory addresses.
 *
 * Values referenced: QEMU v10.0.0 hw/display/vga-pci.c (the standard VGA's
 * PCI vendor 0x1234 and device 0x1111, the framebuffer in BAR 0 and the
 * register window in BAR 2).
 */
static void
discover_display(
	ofw_cell_t node,
	struct discovery *state,
	unsigned long vendor,
	unsigned long device)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	unsigned long framebuffer;
	unsigned long framebuffer_size;
	unsigned long registers;
	unsigned long high;
	unsigned long space;
	unsigned long bar;
	unsigned int offset;
	int found_framebuffer;
	int found_registers;
	int length;

	/* Only the first standard VGA is taken. */
	if (state->have_display || vendor != QEMU_VGA_VENDOR || device != QEMU_VGA_DEVICE)
		return;

	/* Refuses assigned addresses that are not whole PCI address entries. */
	length = ofw_getprop(node, "assigned-addresses", bytes, sizeof(bytes));
	if (length <= 0 || (unsigned int)length % 20U != 0)
		return;

	/* Takes the 32-bit memory ranges of BAR 0 and BAR 2. */
	found_framebuffer = 0;
	found_registers = 0;
	framebuffer = 0;
	framebuffer_size = 0;
	registers = 0;
	for (offset = 0; offset < (unsigned int)length; offset += 20U) {
		/* The first cell names the space and the configuration register of the BAR. */
		high = be32(bytes + offset);
		space = high >> 24 & 3U;
		bar = high & 0xffU;
		if (space != 2)
			continue;

		/* BAR 0, at configuration offset 0x10, is the framebuffer. */
		if (bar == 0x10) {
			framebuffer = be32(bytes + offset + 8U);
			framebuffer_size = be32(bytes + offset + 16U);
			found_framebuffer = 1;
		}

		/* BAR 2, at configuration offset 0x18, is the register window. */
		if (bar == 0x18) {
			registers = be32(bytes + offset + 8U);
			found_registers = 1;
		}
	}

	/* A display without both ranges is not used. */
	if (!found_framebuffer || !found_registers)
		return;

	/* Records the display. */
	state->handoff->display_vendor = (uint16_t)vendor;
	state->handoff->display_device = (uint16_t)device;
	state->handoff->display_framebuffer = (uint32_t)framebuffer;
	state->handoff->display_framebuffer_size = (uint32_t)framebuffer_size;
	state->handoff->display_registers = (uint32_t)registers;
	state->have_display = 1;
}

/*
 * Finds the 8042's data and command ports from its "reg": the first range
 * is the data port, and a second one the command port.
 */
static void
discover_i8042(
	ofw_cell_t node,
	struct discovery *state)
{
	unsigned char bytes[64];
	ofw_scell_t parent;
	unsigned int address_count;
	unsigned int size_count;
	unsigned int stride;
	unsigned long data;
	unsigned long command;
	int length;

	/* The first 8042 found is taken; the parent's cells are needed to read its ranges. */
	if (state->have_i8042)
		return;
	parent = ofw_parent(node);
	if (parent <= 0)
		return;

	/* Reads the ranges, refusing a "reg" shorter than one. */
	address_count = (unsigned int)address_cells((ofw_cell_t)parent, "#address-cells", 2);
	size_count = (unsigned int)address_cells((ofw_cell_t)parent, "#size-cells", 1);
	stride = (address_count + size_count) * 4U;
	length = ofw_getprop(node, "reg", bytes, sizeof(bytes));
	if (length < (int)stride)
		return;

	/* The low cell of the first address is the data port. */
	data = be32(bytes + (address_count - 1U) * 4U);
	command = data + I8042_COMMAND_DISTANCE;

	/* A second range gives the command port. */
	if (length >= (int)(stride * 2U))
		command = be32(bytes + stride + (address_count - 1U) * 4U);

	/* Ports beyond the 16-bit I/O space are not used. */
	if (data > 0xffffUL || command > 0xffffUL)
		return;

	/* Records the ports. */
	state->handoff->i8042_data = (uint16_t)data;
	state->handoff->i8042_command = (uint16_t)command;
	state->have_i8042 = 1;
}

/*
 * Finds the display OpenFirmware draws on: the console when it is a
 * display, or else the "screen" alias, which is opened so that it can be
 * set up and so that its palette can be set; the instance is not closed.
 *
 * Returns 0 with the node and an instance, or -1 when there is none.
 */
static int
find_display(
	ofw_scell_t *node,
	ofw_scell_t *instance)
{
	char type[32];
	char path[DISPLAY_PATH_SIZE];
	ofw_cell_t console;
	ofw_scell_t aliases;
	ofw_scell_t package;
	ofw_scell_t opened;
	int is_display;
	int status;

	/* The console's package is the display when it is of the display type. */
	console = ofw_stdout_handle();
	package = ofw_instance_to_package(console);
	type[0] = '\0';
	if (package > 0)
		(void)property_string((ofw_cell_t)package, "device_type", type, sizeof(type));
	is_display = same_string(type, "display");
	if (package > 0 && is_display) {
		*node = package;
		*instance = (ofw_scell_t)console;
		return 0;
	}

	/* Otherwise the "screen" alias names the display. */
	aliases = ofw_finddevice("/aliases");
	if (aliases <= 0)
		return -1;
	status = property_string((ofw_cell_t)aliases, "screen", path, sizeof(path));
	if (status != 0)
		return -1;

	/* The alias must name a display node. */
	package = ofw_finddevice(path);
	if (package <= 0)
		return -1;
	type[0] = '\0';
	(void)property_string((ofw_cell_t)package, "device_type", type, sizeof(type));
	is_display = same_string(type, "display");
	if (!is_display)
		return -1;

	/* Opens the display, which sets it up, and keeps it open. */
	opened = ofw_open(path);
	if (opened <= 0)
		return -1;

	/* Succeeded: the display and its instance. */
	*node = package;
	*instance = opened;
	return 0;
}

/*
 * Finds the largest memory range in a PCI display's "assigned-addresses",
 * as a physical address.
 *
 * Returns 0, or -1 when the node has no memory range.
 */
static int
framebuffer_bar(
	ofw_cell_t node,
	unsigned long long pci_memory_base,
	unsigned long long *base,
	unsigned long long *size)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	unsigned long long address;
	unsigned long long length_of_range;
	unsigned long space;
	unsigned int offset;
	int found;
	int length;

	/* Refuses assigned addresses that are not whole PCI address entries. */
	length = ofw_getprop(node, "assigned-addresses", bytes, sizeof(bytes));
	if (length <= 0 || (unsigned int)length % 20U != 0)
		return -1;

	/* Keeps the largest 32-bit or 64-bit memory range. */
	found = 0;
	for (offset = 0; offset < (unsigned int)length; offset += 20U) {
		/* Only memory space ranges hold a framebuffer. */
		space = be32(bytes + offset) >> 24 & 3U;
		if (space != 2 && space != 3)
			continue;

		/* Keeps the range when it is the largest so far. */
		address = cells64(bytes + offset + 4U, 2);
		length_of_range = cells64(bytes + offset + 12U, 2);
		if (found && length_of_range <= *size)
			continue;
		*base = pci_memory_base + address;
		*size = length_of_range;
		found = 1;
	}

	/* A node without a memory range has no framebuffer there. */
	if (!found)
		return -1;

	/* Succeeded: the largest memory range. */
	return 0;
}

/*
 * Finds the framebuffer's physical address: the firmware's "address"
 * translated by the MMU, checked against the display's memory range when it
 * is on PCI, or else the start of that memory range.
 *
 * Returns 0, or -1 when neither gives an address.
 */
static int
framebuffer_address(
	ofw_cell_t node,
	const struct discovery *state,
	unsigned long long *physical)
{
	unsigned long long bar_base;
	unsigned long long bar_size;
	unsigned long long translated;
	unsigned long virtual_address;
	int have_bar;
	int have_translated;
	int status;

	/* Reads the PCI memory range when the PCI memory space is known. */
	have_bar = 0;
	bar_base = 0;
	bar_size = 0;
	if (state->have_pci_memory) {
		status = framebuffer_bar(node, state->handoff->pci_memory_base, &bar_base, &bar_size);
		if (status == 0)
			have_bar = 1;
	}

	/* Translates the firmware's own mapping of the framebuffer. */
	have_translated = 0;
	translated = 0;
	status = property_u32(node, "address", &virtual_address);
	if (status == 0)
		status = ofw_translate(virtual_address, &translated);
	if (status == 0)
		have_translated = 1;

	/* A translation outside the display's memory range is not trusted. */
	if (have_translated &&
	    have_bar &&
	    (translated < bar_base || translated >= bar_base + bar_size))
		have_translated = 0;

	/* The translation comes first. */
	if (have_translated) {
		*physical = translated;
		return 0;
	}

	/* Without one, the memory range is the framebuffer. */
	if (!have_bar)
		return -1;
	*physical = bar_base;

	/* Succeeded: the start of the memory range. */
	return 0;
}

/*
 * Tells the byte order of the emulator's standard VGA from its byte order
 * register, which the loader reads and leaves as it is.
 */
static unsigned long
qemu_vga_format(
	const struct discovery *state)
{
	unsigned long long address;
	unsigned long order;

	/* Without the register window the pixels are taken as big-endian. */
	if (!state->have_display || !state->have_pci_memory)
		return KERN_BOOT_FRAMEBUFFER_XRGB8888;

	/* Reads the register. */
	address = state->handoff->pci_memory_base + state->handoff->display_registers + QEMU_VGA_QEXT_BYTEORDER;
	order = phys_read32_le(address);

	/* Little-endian pixels are blue first in memory. */
	if (order == QEMU_VGA_QEXT_LITTLE_ENDIAN)
		return KERN_BOOT_FRAMEBUFFER_BGRX8888;

	/* Big-endian pixels, and any other value, are the unused byte, then red, green and blue. */
	return KERN_BOOT_FRAMEBUFFER_XRGB8888;
}

/*
 * Sets the display's palette to the 3-3-2 ramp of KERN_BOOT_FRAMEBUFFER_RGB332
 * with its "color!" method ( r g b index -- ).
 *
 * Returns 0, or -1 when a call fails.
 */
static int
set_rgb332_palette(
	ofw_cell_t instance)
{
	ofw_cell_t arguments[4];
	unsigned int index;
	int status;

	/* Sets each of the 256 entries; arguments are top-of-stack first. */
	for (index = 0; index < 256U; index++) {
		arguments[0] = index;
		arguments[1] = (index & 3U) * 255U / 3U;
		arguments[2] = ((index >> 2) & 7U) * 255U / 7U;
		arguments[3] = ((index >> 5) & 7U) * 255U / 7U;
		status = ofw_call_method("color!", instance, 4, arguments, 0, NULL);
		if (status != 0)
			return -1;
	}

	/* Succeeded: the palette is the ramp. */
	return 0;
}

/* Reads a little-endian 32-bit device register at a physical address. */
static unsigned long
phys_read32_le(
	unsigned long long address)
{
	unsigned int value;

	/* Loads the word through the little-endian physical bypass. */
	__asm__ volatile("lduwa [%1] %2, %0" : "=r"(value) : "r"(address), "i"(ASI_PHYS_BYPASS_EC_E_L));

	/* Succeeded: the word. */
	return value;
}

/*
 * Fills the firmware framebuffer from the display node's "width",
 * "height", "depth" and "linebytes", leaving it zero when the display is
 * missing or of a kind the HAL cannot draw.
 */
static void
discover_framebuffer(
	struct discovery *state)
{
	unsigned long long physical;
	unsigned long width;
	unsigned long height;
	unsigned long depth;
	unsigned long pitch;
	unsigned long format;
	unsigned long vendor;
	unsigned long device;
	ofw_scell_t node;
	ofw_scell_t instance;
	int status;

	/* Finds the display and its size. */
	status = find_display(&node, &instance);
	if (status != 0)
		return;
	status = property_u32((ofw_cell_t)node, "width", &width);
	if (status != 0 || width == 0)
		return;
	status = property_u32((ofw_cell_t)node, "height", &height);
	if (status != 0 || height == 0)
		return;
	status = property_u32((ofw_cell_t)node, "depth", &depth);
	if (status != 0)
		return;

	/* A missing "linebytes" means rows of whole pixels. */
	status = property_u32((ofw_cell_t)node, "linebytes", &pitch);
	if (status != 0)
		pitch = width * ((depth + 7UL) / 8UL);

	/* A display not on PCI has no IDs. */
	vendor = 0;
	device = 0;
	(void)property_u32((ofw_cell_t)node, "vendor-id", &vendor);
	(void)property_u32((ofw_cell_t)node, "device-id", &device);

	/* Tells the format from the depth; others are not handed over. */
	if (depth == 8 && pitch >= width) {
		/* Eight bits index the 3-3-2 palette, which the loader sets. */
		status = set_rgb332_palette((ofw_cell_t)instance);
		if (status != 0)
			return;
		format = KERN_BOOT_FRAMEBUFFER_RGB332;
	} else if ((depth == 24 || depth == 32) && pitch >= width * 4UL) {
		/* Four bytes a pixel: big-endian, unless the emulator's VGA says otherwise. */
		format = KERN_BOOT_FRAMEBUFFER_XRGB8888;
		if (vendor == QEMU_VGA_VENDOR && device == QEMU_VGA_DEVICE)
			format = qemu_vga_format(state);
	} else {
		/* Packed 24-bit, 15-bit and 16-bit pixels are not handed over. */
		return;
	}

	/* Finds where the pixels are. */
	status = framebuffer_address((ofw_cell_t)node, state, &physical);
	if (status != 0)
		return;

	/* Records the framebuffer. */
	state->handoff->framebuffer_phys = physical;
	state->handoff->framebuffer_width = (uint32_t)width;
	state->handoff->framebuffer_height = (uint32_t)height;
	state->handoff->framebuffer_pitch = (uint32_t)pitch;
	state->handoff->framebuffer_format = (uint32_t)format;
	state->handoff->framebuffer_vendor = (uint16_t)(vendor & 0xffffUL);
	state->handoff->framebuffer_device = (uint16_t)(device & 0xffffUL);
}

/* Takes the tick frequency from the first CPU node with a clock. */
static void
discover_cpu(
	ofw_cell_t node,
	struct discovery *state)
{
	unsigned long clock;
	int status;

	/* A CPU node without a clock, or with a zero one, gives nothing. */
	status = property_u32(node, "clock-frequency", &clock);
	if (status != 0 || clock == 0)
		return;

	/* Records the frequency. */
	state->handoff->tick_frequency = clock;
	state->have_cpu = 1;
}

/* Takes the installed and the available memory from the first memory node that has both. */
static void
discover_memory(
	ofw_cell_t node,
	struct discovery *state)
{
	int status;

	/* Reads the installed memory. */
	status = memory_ranges(node, "reg", state->handoff->installed, &state->handoff->installed_count);
	if (status != 0)
		return;

	/* Reads the available memory. */
	status = memory_ranges(node, "available", state->handoff->available, &state->handoff->available_count);
	if (status != 0)
		return;

	/* The memory is found. */
	state->have_memory = 1;
}

/* Examines a node and walks its children, refusing a tree deeper than 32. */
static int
walk_node(
	ofw_cell_t node,
	struct discovery *state,
	unsigned int depth)
{
	char type[32];
	char name[32];
	unsigned long vendor;
	unsigned long device;
	ofw_scell_t child;
	int is_cpu;
	int is_memory;
	int is_pci;
	int is_serial;
	int is_i8042;
	int walked;

	/* A node without IDs matches no PCI device. */
	vendor = ~0UL;
	device = ~0UL;

	/* Refuses a tree too deep to walk. */
	if (depth > 32)
		return -1;

	/* Reads the properties that tell what the node is; a missing one stays empty. */
	type[0] = '\0';
	name[0] = '\0';
	(void)property_string(node, "device_type", type, sizeof(type));
	(void)property_string(node, "name", name, sizeof(name));
	(void)property_u32(node, "vendor-id", &vendor);
	(void)property_u32(node, "device-id", &device);

	/* The first CPU gives the tick frequency. */
	is_cpu = same_string(type, "cpu");
	if (is_cpu && !state->have_cpu)
		discover_cpu(node, state);

	/* The first memory node gives the memory ranges. */
	is_memory = same_string(type, "memory");
	if (is_memory && !state->have_memory)
		discover_memory(node, state);

	/* A PCI node may give the I/O space base. */
	is_pci = same_string(type, "pci");
	if (is_pci)
		discover_pci_ranges(node, state);

	/* The "su" node is the serial console. */
	is_serial = same_string(name, "su");
	if (is_serial)
		discover_serial(node, state);

	/* Any node may be the IDE controller or the display. */
	discover_ide(node, state, vendor, device);
	discover_display(node, state, vendor, device);

	/* The "8042" node is the keyboard and mouse controller. */
	is_i8042 = same_string(name, "8042");
	if (is_i8042)
		discover_i8042(node, state);

	/* Walks the children one sibling after another. */
	child = ofw_child(node);
	while (child > 0) {
		/* Refuses a subtree too deep. */
		walked = walk_node((ofw_cell_t)child, state, depth + 1U);
		if (walked != 0)
			return -1;

		child = ofw_peer((ofw_cell_t)child);
	}

	/* Succeeded: the subtree was walked. */
	return 0;
}

/* Copies the boot path, cut to the handoff's field and terminated. */
static void
copy_bootpath(
	char *destination,
	const char *source)
{
	unsigned int index;

	/* Copies up to the terminator or the field's end. */
	for (index = 0;
	     index + 1U < SUN4U_BOOTPATH_SIZE && source[index] != '\0';
	     index++)
		destination[index] = source[index];

	/* Terminates the path. */
	destination[index] = '\0';
}
