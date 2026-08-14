/* OpenFirmware to zedBSD sun4u handoff builder. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "handoff.h"
#include "ofw.h"

#define PROPERTY_BUFFER_SIZE 512U

struct discovery {
	struct zedbsd_sun4u_handoff *handoff;
	int have_cpu;
	int have_memory;
	int have_pci_io;
	int have_serial;
	int have_ide;
};

static unsigned long
be32(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] << 24 |
	    (unsigned long)bytes[1] << 16 |
	    (unsigned long)bytes[2] << 8 | bytes[3];
}

static unsigned long long
cells64(const unsigned char *bytes, unsigned int cells)
{
	unsigned long long value = 0;

	while (cells-- != 0)
		value = value << 32 | be32(bytes), bytes += 4;
	return value;
}

static void
clear_bytes(void *destination, unsigned long size)
{
	unsigned char *bytes = destination;

	while (size-- != 0)
		*bytes++ = 0;
}

static int
same_string(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right)
		left++, right++;
	return *left == *right;
}

static int
property_string(ofw_cell_t node, const char *name, char *buffer,
	    unsigned long size)
{
	int length;

	if (size < 2)
		return -1;
	length = ofw_getprop(node, name, buffer, size - 1U);
	if (length <= 0 || (unsigned long)length >= size)
		return -1;
	buffer[length] = '\0';
	return 0;
}

static int
property_u32(ofw_cell_t node, const char *name, unsigned long *value)
{
	unsigned char bytes[4];

	if (ofw_getprop(node, name, bytes, sizeof(bytes)) != 4)
		return -1;
	*value = be32(bytes);
	return 0;
}

static int
address_cells(ofw_cell_t node, const char *name, unsigned int fallback)
{
	unsigned long value;

	if (property_u32(node, name, &value) != 0 || value == 0 || value > 4)
		return (int)fallback;
	return (int)value;
}

static int
memory_ranges(ofw_cell_t node, const char *name,
	    struct zedbsd_sun4u_memory_range *ranges, unsigned char *count)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	ofw_scell_t parent = ofw_parent(node);
	unsigned int address_count;
	unsigned int size_count;
	unsigned int stride;
	unsigned int entries;
	unsigned int index;
	int length;

	if (parent <= 0)
		return -1;
	address_count = (unsigned int)address_cells((ofw_cell_t)parent,
	    "#address-cells", 2);
	size_count = (unsigned int)address_cells((ofw_cell_t)parent,
	    "#size-cells", 2);
	stride = (address_count + size_count) * 4U;
	length = ofw_getprop(node, name, bytes, sizeof(bytes));
	if (length <= 0 || stride == 0 || (unsigned int)length % stride != 0)
		return -1;
	entries = (unsigned int)length / stride;
	if (entries == 0 || entries > ZEDBSD_SUN4U_MAX_MEMORY_RANGES)
		return -1;
	for (index = 0; index < entries; index++) {
		const unsigned char *entry = bytes + index * stride;

		ranges[index].base = cells64(entry, address_count);
		ranges[index].size = cells64(entry + address_count * 4U,
		    size_count);
		if (ranges[index].size == 0)
			return -1;
	}
	*count = (unsigned char)entries;
	return 0;
}

static void
discover_pci_ranges(ofw_cell_t node, struct discovery *state)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	ofw_scell_t parent = ofw_parent(node);
	unsigned int child_address;
	unsigned int parent_address;
	unsigned int size_cells;
	unsigned int stride;
	unsigned int offset;
	int length;

	if (parent <= 0 || state->have_pci_io)
		return;
	child_address = (unsigned int)address_cells(node, "#address-cells", 3);
	size_cells = (unsigned int)address_cells(node, "#size-cells", 2);
	parent_address = (unsigned int)address_cells((ofw_cell_t)parent,
	    "#address-cells", 2);
	stride = (child_address + parent_address + size_cells) * 4U;
	length = ofw_getprop(node, "ranges", bytes, sizeof(bytes));
	if (length <= 0 || stride == 0 || (unsigned int)length % stride != 0)
		return;
	for (offset = 0; offset < (unsigned int)length; offset += stride) {
		const unsigned char *entry = bytes + offset;
		unsigned long space = be32(entry) >> 24 & 3U;
		unsigned long long child;
		unsigned long long physical;

		if (space != 1 || child_address < 3 || parent_address > 2)
			continue;
		child = cells64(entry + 4U, child_address - 1U);
		physical = cells64(entry + child_address * 4U,
		    parent_address);
		if (physical < child)
			continue;
		state->handoff->pci_io_base = physical - child;
		state->have_pci_io = 1;
		return;
	}
}

static void
discover_serial(ofw_cell_t node, struct discovery *state)
{
	unsigned char bytes[32];
	ofw_scell_t parent;
	unsigned int address_count;
	int length;

	if (state->have_serial)
		return;
	parent = ofw_parent(node);
	if (parent <= 0)
		return;
	address_count = (unsigned int)address_cells((ofw_cell_t)parent,
	    "#address-cells", 2);
	length = ofw_getprop(node, "reg", bytes, sizeof(bytes));
	if (length < (int)(address_count * 4U) || address_count == 0)
		return;
	state->handoff->serial_io_offset = (uint32_t)cells64(bytes,
	    address_count);
	state->have_serial = 1;
}

static void
discover_ide(ofw_cell_t node, struct discovery *state,
	    unsigned long vendor, unsigned long device)
{
	unsigned char bytes[PROPERTY_BUFFER_SIZE];
	unsigned short ports[4];
	unsigned int found = 0;
	unsigned int offset;
	int length;

	if (state->have_ide || vendor != 0x1095 || device != 0x0646)
		return;
	length = ofw_getprop(node, "assigned-addresses", bytes, sizeof(bytes));
	if (length <= 0 || (unsigned int)length % 20U != 0)
		return;
	for (offset = 0; offset < (unsigned int)length && found < 4;
	    offset += 20U) {
		unsigned long space = be32(bytes + offset) >> 24 & 3U;
		unsigned long address = be32(bytes + offset + 8U);

		if (space == 1 && address <= 0xffffU)
			ports[found++] = (unsigned short)address;
	}
	if (found != 4)
		return;
	state->handoff->ide_vendor = (uint16_t)vendor;
	state->handoff->ide_device = (uint16_t)device;
	state->handoff->ide_primary_command = ports[0];
	state->handoff->ide_primary_control = ports[1];
	state->handoff->ide_secondary_command = ports[2];
	state->handoff->ide_secondary_control = ports[3];
	state->have_ide = 1;
}

static int
walk_node(ofw_cell_t node, struct discovery *state, unsigned int depth)
{
	char type[32];
	char name[32];
	unsigned long vendor = ~0UL;
	unsigned long device = ~0UL;
	ofw_scell_t child;

	if (depth > 32)
		return -1;
	type[0] = '\0';
	name[0] = '\0';
	(void)property_string(node, "device_type", type, sizeof(type));
	(void)property_string(node, "name", name, sizeof(name));
	(void)property_u32(node, "vendor-id", &vendor);
	(void)property_u32(node, "device-id", &device);
	if (same_string(type, "cpu") && !state->have_cpu) {
		unsigned long clock;

		if (property_u32(node, "clock-frequency", &clock) == 0 &&
		    clock != 0) {
			state->handoff->tick_frequency = clock;
			state->have_cpu = 1;
		}
	}
	if (same_string(type, "memory") && !state->have_memory &&
	    memory_ranges(node, "reg", state->handoff->installed,
	    &state->handoff->installed_count) == 0 &&
	    memory_ranges(node, "available", state->handoff->available,
	    &state->handoff->available_count) == 0)
		state->have_memory = 1;
	if (same_string(type, "pci"))
		discover_pci_ranges(node, state);
	if (same_string(name, "su"))
		discover_serial(node, state);
	discover_ide(node, state, vendor, device);
	child = ofw_child(node);
	while (child > 0) {
		if (walk_node((ofw_cell_t)child, state, depth + 1U) != 0)
			return -1;
		child = ofw_peer((ofw_cell_t)child);
	}
	return 0;
}

static void
copy_bootpath(char *destination, const char *source)
{
	unsigned int index;

	for (index = 0; index + 1U < ZEDBSD_SUN4U_BOOTPATH_SIZE &&
	    source[index] != '\0'; index++)
		destination[index] = source[index];
	destination[index] = '\0';
}

int
sparcv9_handoff_build(struct zedbsd_sun4u_handoff *handoff,
	    const char *bootpath)
{
	struct discovery state;
	ofw_scell_t root;

	clear_bytes(handoff, sizeof(*handoff));
	handoff->common.magic = ZEDBSD_HANDOFF_MAGIC;
	handoff->common.version = ZEDBSD_HANDOFF_VERSION_SUN4U;
	handoff->common.size = sizeof(*handoff);
	handoff->common.device_count = 1;
	handoff->common.boot_bios_id = 0x80;
	handoff->common.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_SUN;
	handoff->common.boot_partition_index = 1;
	handoff->common.boot_partition_lba = 4096;
	handoff->extension_magic = ZEDBSD_SUN4U_HANDOFF_MAGIC;
	handoff->extension_version = ZEDBSD_SUN4U_HANDOFF_VERSION;
	handoff->extension_size = sizeof(*handoff) - sizeof(handoff->common);
	copy_bootpath(handoff->bootpath, bootpath);
	state.handoff = handoff;
	state.have_cpu = 0;
	state.have_memory = 0;
	state.have_pci_io = 0;
	state.have_serial = 0;
	state.have_ide = 0;
	root = ofw_finddevice("/");
	if (root <= 0 || walk_node((ofw_cell_t)root, &state, 0) != 0)
		return -1;
	if (!state.have_cpu || !state.have_memory || !state.have_pci_io ||
	    !state.have_serial || !state.have_ide)
		return -1;
	return 0;
}
