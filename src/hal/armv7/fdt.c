/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 HAL's reader of the flattened device tree.
 *
 * The loader describes the machine in the tree: the processors, memory and
 * its reservations, the GIC, the architected timer, the console UART, the
 * emulator's configuration interface and the firmware call conduit.  The
 * reader walks the structure block once, keeping for each open node the
 * properties it may need, and decides what a node is when the node ends,
 * because a node's properties may come in any order.
 *
 * It uses nothing of the kernel or of the C library, so it runs before
 * either exists and is tried on the host as it is.
 */

#include <hal/types.h>
#include "fdt.h"

/*
 * The header fields and the structure tokens.
 *
 * Values referenced: Devicetree Specification (devicetree.org), the
 * flattened format: the header's magic and field offsets, the tokens
 * FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP and FDT_END, and the
 * version that introduced the strings size.
 */
#define FDT_MAGIC		0xd00dfeedU
#define FDT_HEADER_SIZE		40U
#define FDT_OFF_TOTALSIZE	4U
#define FDT_OFF_STRUCT		8U
#define FDT_OFF_STRINGS		12U
#define FDT_OFF_RSVMAP		16U
#define FDT_OFF_VERSION		20U
#define FDT_OFF_STRINGS_SIZE	32U
#define FDT_OFF_STRUCT_SIZE	36U
#define FDT_MIN_VERSION		17U
#define FDT_BEGIN_NODE		1U
#define FDT_END_NODE		2U
#define FDT_PROP		3U
#define FDT_NOP			4U
#define FDT_END			9U

/*
 * The GIC's interrupt specifier: three cells, the kind (0 a shared
 * interrupt, 1 a private one), the number within the kind and the flags;
 * a shared interrupt's GIC number is its number plus 32, a private
 * one's its number plus 16.
 *
 * Values referenced: Linux v6.12
 * Documentation/devicetree/bindings/interrupt-controller/arm,gic.yaml
 * (the three cells and the kinds), and the GIC architecture's numbering of
 * private interrupts from 16 and shared ones from 32.
 */
#define FDT_GIC_CELLS		3U
#define FDT_GIC_KIND_SPI	0U
#define FDT_GIC_KIND_PPI	1U
#define FDT_GIC_SPI_BASE	32U
#define FDT_GIC_PPI_BASE	16U

/*
 * The architected timer's interrupts in the order the binding lists
 * them: the secure physical timer, then the non-secure physical timer.
 *
 * Values referenced: Linux v6.12
 * Documentation/devicetree/bindings/timer/arm,arch_timer.yaml.
 */
#define FDT_TIMER_SECURE_INDEX		0U
#define FDT_TIMER_NONSECURE_INDEX	1U

/* The deepest nesting of nodes the reader follows. */
#define FDT_MAX_DEPTH		16U

/*
 * The cell counts a node's children are read with when the node gives
 * none, as the specification sets them.
 */
#define FDT_DEFAULT_ADDRESS_CELLS	2U
#define FDT_DEFAULT_SIZE_CELLS		1U

/*
 * The properties of one open node that the reader may need when it ends.
 *
 * A property is kept as its value's place in the tree and its length; a
 * missing one has a NULL place.  The record lives on the reader's stack of
 * open nodes from the node's begin token to its end token.
 */
struct fdt_node {
	const char *name;

	/* The cells this node's own reg is read with, the parent's. */
	uint32_t reg_address_cells;
	uint32_t reg_size_cells;

	/* The cells this node's children are read with. */
	uint32_t address_cells;
	uint32_t size_cells;

	const uint8_t *reg;
	uint32_t reg_length;
	const uint8_t *compatible;
	uint32_t compatible_length;
	const uint8_t *device_type;
	uint32_t device_type_length;
	const uint8_t *interrupts;
	uint32_t interrupts_length;
	const uint8_t *bootargs;
	uint32_t bootargs_length;
	const uint8_t *initrd_start;
	uint32_t initrd_start_length;
	const uint8_t *initrd_end;
	uint32_t initrd_end_length;
	const uint8_t *method;
	uint32_t method_length;
	uint32_t clock_frequency;
};

/*
 * The state of one walk of the tree: where its blocks are, and the stack
 * of open nodes.  It lives on the caller's stack for one parse.
 */
struct fdt_walk {
	const uint8_t *blob;
	uint32_t struct_offset;
	uint32_t struct_end;
	uint32_t strings_offset;
	uint32_t strings_end;
	unsigned depth;
	struct fdt_node nodes[FDT_MAX_DEPTH];
	struct armv7_fdt_info *info;
};

/*
 * The properties the reader keeps, and the names they have in a tree.
 */
enum fdt_property {
	FDT_PROPERTY_OTHER = 0,
	FDT_PROPERTY_ADDRESS_CELLS,
	FDT_PROPERTY_SIZE_CELLS,
	FDT_PROPERTY_REG,
	FDT_PROPERTY_COMPATIBLE,
	FDT_PROPERTY_DEVICE_TYPE,
	FDT_PROPERTY_INTERRUPTS,
	FDT_PROPERTY_BOOTARGS,
	FDT_PROPERTY_INITRD_START,
	FDT_PROPERTY_INITRD_END,
	FDT_PROPERTY_METHOD,
	FDT_PROPERTY_CLOCK_FREQUENCY
};

/*
 * What a closing node describes, as far as the HAL is concerned.
 */
enum fdt_role {
	FDT_ROLE_NONE = 0,
	FDT_ROLE_CHOSEN,
	FDT_ROLE_CPU,
	FDT_ROLE_MEMORY,
	FDT_ROLE_RESERVATION,
	FDT_ROLE_GIC,
	FDT_ROLE_TIMER,
	FDT_ROLE_UART_PL011,
	FDT_ROLE_FWCFG,
	FDT_ROLE_PSCI
};

/*
 * One property name and the kind the reader gives it.
 */
struct fdt_property_name {
	const char *name;
	enum fdt_property kind;
};

/*
 * One compatible string and the role a node with it has.
 */
struct fdt_compatible_role {
	const char *compatible;
	enum fdt_role role;
};

/*
 * The property names of the devicetree bindings the reader looks for.
 *
 * The names are the bindings' own words (Devicetree Specification and the
 * ARM, PSCI and QEMU bindings), searched for as they are.
 */
static const struct fdt_property_name property_names[] = {
	{"#address-cells", FDT_PROPERTY_ADDRESS_CELLS},
	{"#size-cells", FDT_PROPERTY_SIZE_CELLS},
	{"reg", FDT_PROPERTY_REG},
	{"compatible", FDT_PROPERTY_COMPATIBLE},
	{"device_type", FDT_PROPERTY_DEVICE_TYPE},
	{"interrupts", FDT_PROPERTY_INTERRUPTS},
	{"bootargs", FDT_PROPERTY_BOOTARGS},
	{"linux,initrd-start", FDT_PROPERTY_INITRD_START},
	{"linux,initrd-end", FDT_PROPERTY_INITRD_END},
	{"method", FDT_PROPERTY_METHOD},
	{"clock-frequency", FDT_PROPERTY_CLOCK_FREQUENCY}
};

/*
 * The devices the reader records, by the compatible strings of their
 * bindings: the GICs of the Cortex-A15 (the emulator's virt board), the
 * Cortex-A9 (the Zynq-7000) and their relatives, the architected timer,
 * the PL011 UART, the emulator's configuration interface and PSCI.
 */
static const struct fdt_compatible_role compatible_roles[] = {
	{"arm,cortex-a15-gic", FDT_ROLE_GIC},
	{"arm,cortex-a9-gic", FDT_ROLE_GIC},
	{"arm,cortex-a7-gic", FDT_ROLE_GIC},
	{"arm,gic-400", FDT_ROLE_GIC},
	{"arm,pl390", FDT_ROLE_GIC},
	{"arm,armv7-timer", FDT_ROLE_TIMER},
	{"arm,pl011", FDT_ROLE_UART_PL011},
	{"qemu,fw-cfg-mmio", FDT_ROLE_FWCFG},
	{"arm,psci-1.0", FDT_ROLE_PSCI},
	{"arm,psci-0.2", FDT_ROLE_PSCI},
	{"arm,psci", FDT_ROLE_PSCI}
};

static uint32_t read_be32(const uint8_t *bytes);
static uint64_t read_be64(const uint8_t *bytes);
static uint64_t read_cells(const uint8_t *bytes, uint32_t cells);
static void clear_info(struct armv7_fdt_info *info);
static void clear_node(struct fdt_node *node);
static int string_is(const char *string, const char *wanted);
static int bytes_are(const uint8_t *bytes, uint32_t length, const char *wanted);
static int list_has(const uint8_t *list, uint32_t length, const char *wanted);
static int read_reservations(const uint8_t *blob, uint32_t offset, uint32_t totalsize, struct armv7_fdt_info *info);
static int walk_structure(struct fdt_walk *walk);
static int begin_node(struct fdt_walk *walk, const char *name);
static enum fdt_property property_kind(const char *name);
static void take_property(struct fdt_walk *walk, const char *name, const uint8_t *value, uint32_t length);
static enum fdt_role node_role(const struct fdt_node *node, const struct fdt_node *parent, unsigned depth);
static int end_node(struct fdt_walk *walk);
static int add_range(struct armv7_fdt_range *ranges, unsigned *count, unsigned capacity, uint64_t base, uint64_t size);
static int read_reg(const struct fdt_node *node, unsigned index, struct armv7_fdt_range *range);
static int add_reg_ranges(const struct fdt_node *node, struct armv7_fdt_range *ranges, unsigned *count, unsigned capacity);
static uint64_t read_number(const uint8_t *value, uint32_t length);
static int read_interrupt(const struct fdt_node *node, unsigned index, uint32_t *number);
static int keep_cpu(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_gic(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_timer(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_uart(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_psci(struct fdt_walk *walk, const struct fdt_node *node);

/*
 * Reads a flattened device tree into the HAL's record of the machine.
 *
 * limit is the most bytes the caller lets the reader look at; the tree's
 * own size must fit inside it.  Returns ARMV7_FDT_OK, or why the tree
 * could not be read, in which case the record is incomplete.
 */
int
armv7_fdt_parse(
	const void *blob,
	size_t limit,
	struct armv7_fdt_info *info)
{
	struct fdt_walk walk;
	const uint8_t *bytes;
	uint32_t magic;
	uint32_t totalsize;
	uint32_t version;
	uint32_t struct_size;
	uint32_t strings_size;
	uint32_t reservation_offset;
	int error;

	/* Starts from an empty record. */
	bytes = blob;
	clear_info(info);

	/* Refuses a buffer too small for the header. */
	if (limit < FDT_HEADER_SIZE)
		return ARMV7_FDT_BAD_SIZE;

	/* Refuses memory that is not a tree. */
	magic = read_be32(bytes);
	if (magic != FDT_MAGIC)
		return ARMV7_FDT_BAD_MAGIC;

	/* Refuses a tree larger than the caller allows. */
	totalsize = read_be32(bytes + FDT_OFF_TOTALSIZE);
	if (totalsize < FDT_HEADER_SIZE || (size_t)totalsize > limit)
		return ARMV7_FDT_BAD_SIZE;
	info->totalsize = totalsize;

	/* Refuses a format older than the one with the block sizes. */
	version = read_be32(bytes + FDT_OFF_VERSION);
	if (version < FDT_MIN_VERSION)
		return ARMV7_FDT_BAD_VERSION;

	/* Locates the structure and strings blocks inside the tree. */
	walk.blob = bytes;
	walk.info = info;
	walk.depth = 0;
	walk.struct_offset = read_be32(bytes + FDT_OFF_STRUCT);
	struct_size = read_be32(bytes + FDT_OFF_STRUCT_SIZE);
	walk.strings_offset = read_be32(bytes + FDT_OFF_STRINGS);
	strings_size = read_be32(bytes + FDT_OFF_STRINGS_SIZE);

	/* Refuses a structure or a strings block that runs past the blob. */
	if (walk.struct_offset > totalsize || struct_size > totalsize - walk.struct_offset)
		return ARMV7_FDT_BAD_STRUCTURE;
	if (walk.strings_offset > totalsize || strings_size > totalsize - walk.strings_offset)
		return ARMV7_FDT_BAD_STRUCTURE;
	walk.struct_end = walk.struct_offset + struct_size;
	walk.strings_end = walk.strings_offset + strings_size;

	/* Reads the reservation block, which comes before the structure. */
	reservation_offset = read_be32(bytes + FDT_OFF_RSVMAP);
	error = read_reservations(bytes, reservation_offset, totalsize, info);
	if (error != ARMV7_FDT_OK)
		return error;

	/* Walks the nodes. */
	error = walk_structure(&walk);
	if (error != ARMV7_FDT_OK)
		return error;

	/* Succeeded: the record describes the machine. */
	return ARMV7_FDT_OK;
}

/*
 * Names why a tree could not be read, for the boot log.
 */
const char *
armv7_fdt_error(
	int error)
{
	/* Chooses the words for the error. */
	switch (error) {
	case ARMV7_FDT_OK:
		return "no error";
	case ARMV7_FDT_BAD_MAGIC:
		return "not a device tree";
	case ARMV7_FDT_BAD_SIZE:
		return "tree size out of bounds";
	case ARMV7_FDT_BAD_VERSION:
		return "tree format too old";
	case ARMV7_FDT_BAD_STRUCTURE:
		return "malformed structure block";
	case ARMV7_FDT_TOO_DEEP:
		return "nodes nested too deeply";
	case ARMV7_FDT_TOO_MANY:
		return "more entries than the HAL keeps";
	default:
		break;
	}

	/* An error this reader does not produce. */
	return "unknown error";
}

/* Reads a big-endian 32-bit value at any alignment. */
static uint32_t
read_be32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Assembles the value from the most significant byte down. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= (uint32_t)bytes[3];

	/* Succeeded: the value in host order. */
	return value;
}

/* Reads a big-endian 64-bit value at any alignment. */
static uint64_t
read_be64(
	const uint8_t *bytes)
{
	uint64_t high;
	uint64_t low;

	/* Assembles the two halves. */
	high = read_be32(bytes);
	low = read_be32(bytes + 4);

	/* Succeeded: the value in host order. */
	return (high << 32) | low;
}

/* Reads a number of one or two cells; more cells keep the low two. */
static uint64_t
read_cells(
	const uint8_t *bytes,
	uint32_t cells)
{
	uint64_t number;

	/* No cells is the number zero. */
	if (cells == 0U)
		return 0;

	/* One cell is a 32-bit number; two or more end with the low 64 bits. */
	if (cells == 1U)
		number = read_be32(bytes);
	else
		number = read_be64(bytes + (cells - 2U) * 4U);

	/* Succeeded: the number the cells hold. */
	return number;
}

/* Empties the record before a parse. */
static void
clear_info(
	struct armv7_fdt_info *info)
{
	uint8_t *bytes;
	size_t index;

	/* Zeroes every byte; zero means absent for every field. */
	bytes = (uint8_t *)info;
	for (index = 0; index < sizeof(*info); index++)
		bytes[index] = 0;
}

/* Empties the record of one open node. */
static void
clear_node(
	struct fdt_node *node)
{
	uint8_t *bytes;
	size_t index;

	/* Zeroes every byte; zero means absent for every field. */
	bytes = (uint8_t *)node;
	for (index = 0; index < sizeof(*node); index++)
		bytes[index] = 0;
}

/* Reports whether two terminated strings are equal. */
static int
string_is(
	const char *string,
	const char *wanted)
{
	size_t index;

	/* Compares up to the wanted string's terminator. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (string[index] != wanted[index])
			return 0;
	}

	/* Equal only when the string ends there too. */
	if (string[index] != '\0')
		return 0;
	return 1;
}

/* Reports whether a property value is exactly one string. */
static int
bytes_are(
	const uint8_t *bytes,
	uint32_t length,
	const char *wanted)
{
	uint32_t index;

	/* An absent property is no string. */
	if (bytes == NULL)
		return 0;

	/* Compares the string's bytes while the value lasts. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (index >= length)
			return 0;
		if (bytes[index] != (uint8_t)wanted[index])
			return 0;
	}

	/* The value must end with the string's terminator. */
	if (index >= length)
		return 0;
	if (bytes[index] != 0)
		return 0;
	return 1;
}

/* Reports whether a string-list property holds one string. */
static int
list_has(
	const uint8_t *list,
	uint32_t length,
	const char *wanted)
{
	uint32_t start;
	uint32_t end;
	int equal;

	/* An absent property holds nothing. */
	if (list == NULL)
		return 0;

	/* Compares each terminated string of the list. */
	start = 0;
	while (start < length) {
		end = start;
		while (end < length && list[end] != 0)
			end++;

		/* Stops at a list whose last string has no terminator. */
		if (end >= length)
			return 0;
		equal = bytes_are(list + start, end + 1U - start, wanted);
		if (equal)
			return 1;
		start = end + 1U;
	}

	/* No string of the list is the one wanted. */
	return 0;
}

/*
 * Reads the reservation block: pairs of 64-bit address and size, ending
 * with a pair of zeros.
 */
static int
read_reservations(
	const uint8_t *blob,
	uint32_t offset,
	uint32_t totalsize,
	struct armv7_fdt_info *info)
{
	uint64_t base;
	uint64_t size;
	int error;

	/* Walks the pairs until the terminating one. */
	for (;;) {
		/* Refuses a block that runs past the tree. */
		if (offset > totalsize || totalsize - offset < 16U)
			return ARMV7_FDT_BAD_STRUCTURE;
		base = read_be64(blob + offset);
		size = read_be64(blob + offset + 8U);
		offset += 16U;

		/* The pair of zeros ends the block. */
		if (base == 0 && size == 0)
			break;
		error = add_range(info->reserved, &info->reserved_count, ARMV7_FDT_MAX_RESERVED, base, size);
		if (error != ARMV7_FDT_OK)
			return error;
	}

	/* Succeeded: every reservation is recorded. */
	return ARMV7_FDT_OK;
}

/*
 * Walks the structure block token by token.
 */
static int
walk_structure(
	struct fdt_walk *walk)
{
	const uint8_t *blob;
	const char *name;
	uint32_t offset;
	uint32_t token;
	uint32_t length;
	uint32_t name_offset;
	uint32_t end;
	int error;

	/* Reads tokens until the end token. */
	blob = walk->blob;
	offset = walk->struct_offset;
	for (;;) {
		/* Refuses a block that ends without the end token. */
		if (offset > walk->struct_end || walk->struct_end - offset < 4U)
			return ARMV7_FDT_BAD_STRUCTURE;
		token = read_be32(blob + offset);
		offset += 4U;

		/* Acts on each kind of token. */
		switch (token) {
		case FDT_BEGIN_NODE:
			/* The node's name follows, terminated and padded to four bytes. */
			name = (const char *)(blob + offset);
			end = offset;
			while (end < walk->struct_end && blob[end] != 0)
				end++;
			if (end >= walk->struct_end)
				return ARMV7_FDT_BAD_STRUCTURE;
			offset = (end + 1U + 3U) & ~3U;
			error = begin_node(walk, name);
			if (error != ARMV7_FDT_OK)
				return error;
			break;
		case FDT_END_NODE:
			error = end_node(walk);
			if (error != ARMV7_FDT_OK)
				return error;
			break;
		case FDT_PROP:
			/* The value's length and the name's place in the strings block. */
			if (walk->struct_end - offset < 8U)
				return ARMV7_FDT_BAD_STRUCTURE;
			length = read_be32(blob + offset);
			name_offset = read_be32(blob + offset + 4U);
			offset += 8U;
			if (length > walk->struct_end - offset)
				return ARMV7_FDT_BAD_STRUCTURE;
			if (name_offset >= walk->strings_end - walk->strings_offset)
				return ARMV7_FDT_BAD_STRUCTURE;

			/* A property outside any node is malformed. */
			if (walk->depth == 0)
				return ARMV7_FDT_BAD_STRUCTURE;
			name = (const char *)(blob + walk->strings_offset + name_offset);
			take_property(walk, name, blob + offset, length);
			offset = (offset + length + 3U) & ~3U;
			break;
		case FDT_NOP:
			break;
		case FDT_END:
			/* Every node must have ended. */
			if (walk->depth != 0)
				return ARMV7_FDT_BAD_STRUCTURE;
			return ARMV7_FDT_OK;
		default:
			return ARMV7_FDT_BAD_STRUCTURE;
		}
	}
}

/*
 * Opens a node: its record starts empty and its reg is read with the
 * cells its parent gives.
 */
static int
begin_node(
	struct fdt_walk *walk,
	const char *name)
{
	struct fdt_node *node;
	struct fdt_node *parent;

	/* Refuses nesting deeper than the stack. */
	if (walk->depth >= FDT_MAX_DEPTH)
		return ARMV7_FDT_TOO_DEEP;

	/* Starts the node's record with the defaults for its children. */
	node = &walk->nodes[walk->depth];
	clear_node(node);
	node->name = name;
	node->address_cells = FDT_DEFAULT_ADDRESS_CELLS;
	node->size_cells = FDT_DEFAULT_SIZE_CELLS;

	/* The node's own reg is read with its parent's cells; the root has none. */
	node->reg_address_cells = FDT_DEFAULT_ADDRESS_CELLS;
	node->reg_size_cells = FDT_DEFAULT_SIZE_CELLS;
	if (walk->depth > 0) {
		parent = &walk->nodes[walk->depth - 1U];
		node->reg_address_cells = parent->address_cells;
		node->reg_size_cells = parent->size_cells;
	}

	/* Succeeded: the node is open. */
	walk->depth++;
	return ARMV7_FDT_OK;
}

/* Reports which kept property a name is, or that it is none of them. */
static enum fdt_property
property_kind(
	const char *name)
{
	size_t index;
	int equal;

	/* Compares the name with each kept one. */
	for (index = 0; index < sizeof(property_names) / sizeof(property_names[0]); index++) {
		equal = string_is(name, property_names[index].name);
		if (equal)
			return property_names[index].kind;
	}

	/* A property the reader does not keep. */
	return FDT_PROPERTY_OTHER;
}

/*
 * Keeps one property of the open node when the reader may need it.
 *
 * The cell counts are read at once, since they change how the children
 * are read; the other properties are kept as their places.
 */
static void
take_property(
	struct fdt_walk *walk,
	const char *name,
	const uint8_t *value,
	uint32_t length)
{
	struct fdt_node *node;
	enum fdt_property kind;
	uint32_t number;

	/* The property belongs to the innermost open node. */
	node = &walk->nodes[walk->depth - 1U];
	kind = property_kind(name);

	/* A one-cell property's number, for the kinds that are numbers. */
	number = 0;
	if (length >= 4U)
		number = read_be32(value);

	/* Keeps the property where the classification reads it. */
	switch (kind) {
	case FDT_PROPERTY_ADDRESS_CELLS:
		node->address_cells = number;
		break;
	case FDT_PROPERTY_SIZE_CELLS:
		node->size_cells = number;
		break;
	case FDT_PROPERTY_REG:
		node->reg = value;
		node->reg_length = length;
		break;
	case FDT_PROPERTY_COMPATIBLE:
		node->compatible = value;
		node->compatible_length = length;
		break;
	case FDT_PROPERTY_DEVICE_TYPE:
		node->device_type = value;
		node->device_type_length = length;
		break;
	case FDT_PROPERTY_INTERRUPTS:
		node->interrupts = value;
		node->interrupts_length = length;
		break;
	case FDT_PROPERTY_BOOTARGS:
		node->bootargs = value;
		node->bootargs_length = length;
		break;
	case FDT_PROPERTY_INITRD_START:
		node->initrd_start = value;
		node->initrd_start_length = length;
		break;
	case FDT_PROPERTY_INITRD_END:
		node->initrd_end = value;
		node->initrd_end_length = length;
		break;
	case FDT_PROPERTY_METHOD:
		node->method = value;
		node->method_length = length;
		break;
	case FDT_PROPERTY_CLOCK_FREQUENCY:
		node->clock_frequency = number;
		break;
	default:
		break;
	}
}

/*
 * Closes a node and records what it describes.
 */
static int
end_node(
	struct fdt_walk *walk)
{
	struct armv7_fdt_info *info;
	struct fdt_node *node;
	struct fdt_node *parent;
	enum fdt_role role;
	unsigned depth;
	int error;

	/* Refuses an end token without an open node. */
	if (walk->depth == 0)
		return ARMV7_FDT_BAD_STRUCTURE;

	/* Names the node, its parent (none for the root) and the record. */
	depth = walk->depth;
	node = &walk->nodes[depth - 1U];
	parent = NULL;
	if (depth >= 2U)
		parent = &walk->nodes[depth - 2U];
	info = walk->info;

	/* Records what the node describes. */
	role = node_role(node, parent, depth);
	error = ARMV7_FDT_OK;
	switch (role) {
	case FDT_ROLE_CHOSEN:
		/* The command line the loader passes, as a place in the tree. */
		if (node->bootargs != NULL && node->bootargs_length > 0) {
			info->bootargs_offset = (uint32_t)(node->bootargs - walk->blob);
			info->bootargs_length = node->bootargs_length;
		}

		/* The initial ramdisk the loader placed, when there is one. */
		if (node->initrd_start != NULL && node->initrd_end != NULL) {
			info->initrd_start = read_number(node->initrd_start, node->initrd_start_length);
			info->initrd_end = read_number(node->initrd_end, node->initrd_end_length);
		}

		break;
	case FDT_ROLE_CPU:
		error = keep_cpu(walk, node);
		break;
	case FDT_ROLE_MEMORY:
		error = add_reg_ranges(node, info->memory, &info->memory_count, ARMV7_FDT_MAX_MEMORY);
		break;
	case FDT_ROLE_RESERVATION:
		error = add_reg_ranges(node, info->reserved, &info->reserved_count, ARMV7_FDT_MAX_RESERVED);
		break;
	case FDT_ROLE_GIC:
		keep_gic(walk, node);
		break;
	case FDT_ROLE_TIMER:
		keep_timer(walk, node);
		break;
	case FDT_ROLE_UART_PL011:
		keep_uart(walk, node);
		break;
	case FDT_ROLE_FWCFG:
		(void)read_reg(node, 0, &info->fwcfg);
		break;
	case FDT_ROLE_PSCI:
		keep_psci(walk, node);
		break;
	default:
		break;
	}

	/* Reports an entry the record has no room for. */
	if (error != ARMV7_FDT_OK)
		return error;

	/* Succeeded: the node is closed. */
	walk->depth--;
	return ARMV7_FDT_OK;
}

/*
 * Decides what a closing node describes, from where it is and what it
 * says it is.
 */
static enum fdt_role
node_role(
	const struct fdt_node *node,
	const struct fdt_node *parent,
	unsigned depth)
{
	size_t index;
	int matched;

	/* The node directly under the root that the HAL reads by name. */
	if (depth == 2U) {
		matched = string_is(node->name, "chosen");
		if (matched)
			return FDT_ROLE_CHOSEN;
	}

	/* The children of /cpus that are processors, and of /reserved-memory. */
	if (depth == 3U) {
		matched = string_is(parent->name, "cpus");
		if (matched) {
			matched = bytes_are(node->device_type, node->device_type_length, "cpu");
			if (matched)
				return FDT_ROLE_CPU;
			return FDT_ROLE_NONE;
		}

		/* Only the children of the reservation node are reservations. */
		matched = string_is(parent->name, "reserved-memory");
		if (matched)
			return FDT_ROLE_RESERVATION;
	}

	/* A memory node, wherever the tree puts it. */
	matched = bytes_are(node->device_type, node->device_type_length, "memory");
	if (matched)
		return FDT_ROLE_MEMORY;

	/* The devices, by their compatible strings. */
	for (index = 0; index < sizeof(compatible_roles) / sizeof(compatible_roles[0]); index++) {
		matched = list_has(node->compatible, node->compatible_length, compatible_roles[index].compatible);
		if (matched)
			return compatible_roles[index].role;
	}

	/* Nothing the HAL reads. */
	return FDT_ROLE_NONE;
}

/* Adds one range to a table of ranges; an empty one is skipped. */
static int
add_range(
	struct armv7_fdt_range *ranges,
	unsigned *count,
	unsigned capacity,
	uint64_t base,
	uint64_t size)
{
	/* An empty range adds nothing. */
	if (size == 0)
		return ARMV7_FDT_OK;

	/* Refuses a table that is full. */
	if (*count >= capacity)
		return ARMV7_FDT_TOO_MANY;

	/* Succeeded: the range is the table's last. */
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;
	return ARMV7_FDT_OK;
}

/* Reads one address and size of a node's reg, by its place in the list. */
static int
read_reg(
	const struct fdt_node *node,
	unsigned index,
	struct armv7_fdt_range *range)
{
	uint32_t entry_length;
	uint32_t offset;

	/* A node without that whole entry has no such address. */
	entry_length = (node->reg_address_cells + node->reg_size_cells) * 4U;
	offset = entry_length * index;
	if (node->reg == NULL ||
	    entry_length == 0 ||
	    node->reg_length < offset + entry_length)
		return -1;

	/* Succeeded: the entry's address and size. */
	range->base = read_cells(node->reg + offset, node->reg_address_cells);
	range->size = read_cells(node->reg + offset + node->reg_address_cells * 4U, node->reg_size_cells);
	return 0;
}

/* Adds every address and size of a node's reg to a table of ranges. */
static int
add_reg_ranges(
	const struct fdt_node *node,
	struct armv7_fdt_range *ranges,
	unsigned *count,
	unsigned capacity)
{
	uint32_t entry_length;
	uint32_t offset;
	uint64_t base;
	uint64_t size;
	int error;

	/* A node without whole entries adds nothing. */
	entry_length = (node->reg_address_cells + node->reg_size_cells) * 4U;
	if (node->reg == NULL || entry_length == 0)
		return ARMV7_FDT_OK;

	/* Adds each whole entry. */
	for (offset = 0; offset + entry_length <= node->reg_length; offset += entry_length) {
		base = read_cells(node->reg + offset, node->reg_address_cells);
		size = read_cells(node->reg + offset + node->reg_address_cells * 4U, node->reg_size_cells);
		error = add_range(ranges, count, capacity, base, size);
		if (error != ARMV7_FDT_OK)
			return error;
	}

	/* Succeeded: every entry is in the table. */
	return ARMV7_FDT_OK;
}

/* Reads a property of one or two cells as a number. */
static uint64_t
read_number(
	const uint8_t *value,
	uint32_t length)
{
	uint64_t number;

	/* A value shorter than a cell is no number. */
	if (length < 4U)
		return 0;

	/* Two cells are a 64-bit number, one a 32-bit number. */
	if (length >= 8U)
		number = read_be64(value);
	else
		number = read_be32(value);

	/* Succeeded: the value's number. */
	return number;
}

/*
 * Reads one GIC interrupt specifier of a node's interrupts and reports its
 * GIC number.  Reports -1 for a missing entry or a kind the GIC numbers
 * otherwise.
 */
static int
read_interrupt(
	const struct fdt_node *node,
	unsigned index,
	uint32_t *number)
{
	uint32_t offset;
	uint32_t kind;
	uint32_t within;

	/* The entry must be whole. */
	offset = index * FDT_GIC_CELLS * 4U;
	if (node->interrupts == NULL || node->interrupts_length < offset + FDT_GIC_CELLS * 4U)
		return -1;
	kind = read_be32(node->interrupts + offset);
	within = read_be32(node->interrupts + offset + 4U);

	/* A shared interrupt is numbered from 32. */
	if (kind == FDT_GIC_KIND_SPI) {
		*number = within + FDT_GIC_SPI_BASE;
		return 0;
	}

	/* A private interrupt is numbered from 16. */
	if (kind == FDT_GIC_KIND_PPI) {
		*number = within + FDT_GIC_PPI_BASE;
		return 0;
	}

	/* No other kind is a GIC number. */
	return -1;
}

/*
 * Records one processor from its node under /cpus: its reg is the
 * affinity part of its MPIDR.
 */
static int
keep_cpu(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct armv7_fdt_info *info;
	uint32_t entry_length;

	/* A processor without a whole reg has no affinity and is not recorded. */
	info = walk->info;
	entry_length = node->reg_address_cells * 4U;
	if (node->reg == NULL ||
	    entry_length == 0 ||
	    node->reg_length < entry_length)
		return ARMV7_FDT_OK;

	/* Refuses a table that is full. */
	if (info->cpu_count >= ARMV7_FDT_MAX_CPUS)
		return ARMV7_FDT_TOO_MANY;

	/* Succeeded: the processor is in the table. */
	info->cpu_mpidr[info->cpu_count] = (uint32_t)read_cells(node->reg, node->reg_address_cells);
	info->cpu_count++;
	return ARMV7_FDT_OK;
}

/*
 * Records the GIC: the distributor is the first reg entry and the CPU
 * interface the second.  Only the first GIC of the tree is kept.
 */
static void
keep_gic(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct armv7_fdt_info *info;
	int error;

	/* Keeps only the first GIC. */
	info = walk->info;
	if (info->gic_distributor.size != 0)
		return;

	/* Reads the distributor's block. */
	error = read_reg(node, 0, &info->gic_distributor);
	if (error != 0)
		return;

	/* Reads the CPU interface's block; without it the distributor is forgotten too. */
	error = read_reg(node, 1, &info->gic_cpu_interface);
	if (error != 0) {
		info->gic_distributor.base = 0;
		info->gic_distributor.size = 0;
	}
}

/*
 * Records the architected timer: the private interrupts of its two
 * physical timers and the frequency, when the tree states one.
 */
static void
keep_timer(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct armv7_fdt_info *info;
	int error;

	/* The secure physical timer is the first interrupt; without it the node is no use. */
	info = walk->info;
	error = read_interrupt(node, FDT_TIMER_SECURE_INDEX, &info->timer_secure_irq);
	if (error != 0)
		return;

	/* The non-secure physical timer is the second. */
	error = read_interrupt(node, FDT_TIMER_NONSECURE_INDEX, &info->timer_nonsecure_irq);
	if (error != 0)
		return;

	/* Succeeded: the timer is recorded, with its frequency or zero. */
	info->timer_frequency = node->clock_frequency;
	info->has_timer = 1;
}

/*
 * Records the first PL011 the tree lists, the console of the emulator's
 * virt board.
 */
static void
keep_uart(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct armv7_fdt_info *info;
	int error;

	/* Keeps only the first UART. */
	info = walk->info;
	if (info->uart.size != 0)
		return;

	/* Its registers, and its interrupt when the tree gives one. */
	error = read_reg(node, 0, &info->uart);
	if (error != 0)
		return;
	(void)read_interrupt(node, 0, &info->uart_irq);
}

/*
 * Records the conduit of the firmware calls, which the node's method
 * names.
 */
static void
keep_psci(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct armv7_fdt_info *info;
	int hvc;
	int smc;

	/* The hypervisor call. */
	info = walk->info;
	hvc = bytes_are(node->method, node->method_length, "hvc");
	if (hvc) {
		info->psci_method = ARMV7_FDT_PSCI_HVC;
		return;
	}

	/* The secure monitor call. */
	smc = bytes_are(node->method, node->method_length, "smc");
	if (smc)
		info->psci_method = ARMV7_FDT_PSCI_SMC;
}
