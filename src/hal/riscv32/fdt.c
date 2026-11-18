/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 HAL's reader of the flattened device tree.
 *
 * The firmware describes the machine in the tree: the harts and their
 * timebase, memory and its reservations, the interrupt controller and the
 * console.  The reader walks the structure block once, keeping for each
 * open node the properties it may need, and decides what a node is when
 * the node ends, because a node's properties may come in any order and a
 * hart's interrupt controller is a child that ends before its parent.
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
 * The interrupt of a hart's controller that a PLIC context raises for
 * supervisor mode.
 *
 * Values referenced: RISC-V privileged architecture (riscv/riscv-isa-manual),
 * interrupt 9, the supervisor external interrupt, as the riscv,cpu-intc
 * binding numbers it.
 */
#define FDT_INTC_S_EXTERNAL	9U

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
	const uint8_t *status;
	uint32_t status_length;
	const uint8_t *mmu_type;
	const uint8_t *isa_extensions;
	uint32_t isa_extensions_length;
	const uint8_t *isa;
	uint32_t isa_length;
	const uint8_t *interrupts_extended;
	uint32_t interrupts_extended_length;
	const uint8_t *interrupts;
	uint32_t interrupts_length;
	const uint8_t *timebase;
	uint32_t timebase_length;
	const uint8_t *bootargs;
	uint32_t bootargs_length;
	const uint8_t *initrd_start;
	uint32_t initrd_start_length;
	const uint8_t *initrd_end;
	uint32_t initrd_end_length;
	uint32_t phandle;
	int has_phandle;
	uint32_t sources;
	uint32_t reg_shift;
	uint32_t reg_io_width;
	int no_map;
	int interrupt_controller;

	/* The phandle of a hart controller found among this node's children. */
	uint32_t child_intc_phandle;
	int has_child_intc;
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
	struct riscv32_fdt_info *info;
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
	FDT_PROPERTY_STATUS,
	FDT_PROPERTY_MMU_TYPE,
	FDT_PROPERTY_ISA_EXTENSIONS,
	FDT_PROPERTY_ISA,
	FDT_PROPERTY_INTERRUPTS_EXTENDED,
	FDT_PROPERTY_INTERRUPTS,
	FDT_PROPERTY_TIMEBASE,
	FDT_PROPERTY_BOOTARGS,
	FDT_PROPERTY_INITRD_START,
	FDT_PROPERTY_INITRD_END,
	FDT_PROPERTY_PHANDLE,
	FDT_PROPERTY_SOURCES,
	FDT_PROPERTY_REG_SHIFT,
	FDT_PROPERTY_REG_IO_WIDTH,
	FDT_PROPERTY_NO_MAP,
	FDT_PROPERTY_INTERRUPT_CONTROLLER
};

/*
 * What a closing node describes, as far as the HAL is concerned.
 */
enum fdt_role {
	FDT_ROLE_NONE = 0,
	FDT_ROLE_CHOSEN,
	FDT_ROLE_CPUS,
	FDT_ROLE_CPU,
	FDT_ROLE_MEMORY,
	FDT_ROLE_RESERVATION,
	FDT_ROLE_PLIC,
	FDT_ROLE_UART_DW_APB,
	FDT_ROLE_UART_NS16550A,
	FDT_ROLE_FWCFG,
	FDT_ROLE_CCACHE
};

/*
 * One property name and the kind the reader gives it.
 */
struct fdt_property_name {
	const char *name;
	enum fdt_property kind;
};

/*
 * One extension name and the bit the record gives it.
 */
struct fdt_extension_name {
	const char *name;
	uint32_t bit;
};

/*
 * The property names of the devicetree bindings the reader looks for.
 *
 * The names are the bindings' own words (Devicetree Specification and the
 * RISC-V, SiFive, Synopsys and QEMU bindings), searched for as they are.
 */
static const struct fdt_property_name property_names[] = {
	{"#address-cells", FDT_PROPERTY_ADDRESS_CELLS},
	{"#size-cells", FDT_PROPERTY_SIZE_CELLS},
	{"reg", FDT_PROPERTY_REG},
	{"compatible", FDT_PROPERTY_COMPATIBLE},
	{"device_type", FDT_PROPERTY_DEVICE_TYPE},
	{"status", FDT_PROPERTY_STATUS},
	{"mmu-type", FDT_PROPERTY_MMU_TYPE},
	{"riscv,isa-extensions", FDT_PROPERTY_ISA_EXTENSIONS},
	{"riscv,isa", FDT_PROPERTY_ISA},
	{"interrupts-extended", FDT_PROPERTY_INTERRUPTS_EXTENDED},
	{"interrupts", FDT_PROPERTY_INTERRUPTS},
	{"timebase-frequency", FDT_PROPERTY_TIMEBASE},
	{"bootargs", FDT_PROPERTY_BOOTARGS},
	{"linux,initrd-start", FDT_PROPERTY_INITRD_START},
	{"linux,initrd-end", FDT_PROPERTY_INITRD_END},
	{"phandle", FDT_PROPERTY_PHANDLE},
	{"riscv,ndev", FDT_PROPERTY_SOURCES},
	{"reg-shift", FDT_PROPERTY_REG_SHIFT},
	{"reg-io-width", FDT_PROPERTY_REG_IO_WIDTH},
	{"no-map", FDT_PROPERTY_NO_MAP},
	{"interrupt-controller", FDT_PROPERTY_INTERRUPT_CONTROLLER}
};

/*
 * The extensions the record notes for each hart, by their names in the
 * RISC-V ISA naming convention.
 */
static const struct fdt_extension_name extension_names[] = {
	{"sstc", RISCV32_FDT_EXT_SSTC},
	{"svpbmt", RISCV32_FDT_EXT_SVPBMT},
	{"zicbom", RISCV32_FDT_EXT_ZICBOM},
	{"zkr", RISCV32_FDT_EXT_ZKR}
};

static uint32_t read_be32(const uint8_t *bytes);
static uint64_t read_be64(const uint8_t *bytes);
static uint64_t read_cells(const uint8_t *bytes, uint32_t cells);
static void clear_info(struct riscv32_fdt_info *info);
static void clear_node(struct fdt_node *node);
static int string_is(const char *string, const char *wanted);
static int string_has_prefix(const char *string, const char *prefix);
static int bytes_are(const uint8_t *bytes, uint32_t length, const char *wanted);
static int list_has(const uint8_t *list, uint32_t length, const char *wanted);
static int read_reservations(const uint8_t *blob, uint32_t offset, uint32_t totalsize, struct riscv32_fdt_info *info);
static int walk_structure(struct fdt_walk *walk);
static int begin_node(struct fdt_walk *walk, const char *name);
static enum fdt_property property_kind(const char *name);
static void take_property(struct fdt_walk *walk, const char *name, const uint8_t *value, uint32_t length);
static enum fdt_role node_role(const struct fdt_node *node, const struct fdt_node *parent, unsigned depth);
static int node_is_cpu_intc(const struct fdt_node *node);
static int end_node(struct fdt_walk *walk);
static int add_range(struct riscv32_fdt_range *ranges, unsigned *count, unsigned capacity, uint64_t base, uint64_t size);
static int read_first_reg(const struct fdt_node *node, uint64_t *base, uint64_t *size);
static int add_reg_ranges(const struct fdt_node *node, struct riscv32_fdt_range *ranges, unsigned *count, unsigned capacity);
static uint32_t cpu_extensions(const struct fdt_node *node);
static uint64_t read_number(const uint8_t *value, uint32_t length);
static int keep_cpu(struct fdt_walk *walk, const struct fdt_node *node);
static int keep_plic(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_uart(struct fdt_walk *walk, const struct fdt_node *node, unsigned kind);

/*
 * Reads a flattened device tree into the HAL's record of the machine.
 *
 * limit is the most bytes the caller lets the reader look at; the tree's
 * own size must fit inside it.  Returns RISCV32_FDT_OK, or why the tree
 * could not be read, in which case the record is incomplete.
 */
int
riscv32_fdt_parse(
	const void *blob,
	size_t limit,
	struct riscv32_fdt_info *info)
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
		return RISCV32_FDT_BAD_SIZE;

	/* Refuses memory that is not a tree. */
	magic = read_be32(bytes);
	if (magic != FDT_MAGIC)
		return RISCV32_FDT_BAD_MAGIC;

	/* Refuses a tree larger than the caller allows. */
	totalsize = read_be32(bytes + FDT_OFF_TOTALSIZE);
	if (totalsize < FDT_HEADER_SIZE || (size_t)totalsize > limit)
		return RISCV32_FDT_BAD_SIZE;
	info->totalsize = totalsize;

	/* Refuses a format older than the one with the block sizes. */
	version = read_be32(bytes + FDT_OFF_VERSION);
	if (version < FDT_MIN_VERSION)
		return RISCV32_FDT_BAD_VERSION;

	/* Locates the structure and strings blocks inside the tree. */
	walk.blob = bytes;
	walk.info = info;
	walk.depth = 0;
	walk.struct_offset = read_be32(bytes + FDT_OFF_STRUCT);
	struct_size = read_be32(bytes + FDT_OFF_STRUCT_SIZE);
	walk.strings_offset = read_be32(bytes + FDT_OFF_STRINGS);
	strings_size = read_be32(bytes + FDT_OFF_STRINGS_SIZE);
	if (walk.struct_offset > totalsize || struct_size > totalsize - walk.struct_offset)
		return RISCV32_FDT_BAD_STRUCTURE;
	if (walk.strings_offset > totalsize || strings_size > totalsize - walk.strings_offset)
		return RISCV32_FDT_BAD_STRUCTURE;
	walk.struct_end = walk.struct_offset + struct_size;
	walk.strings_end = walk.strings_offset + strings_size;

	/* Reads the reservation block, which comes before the structure. */
	reservation_offset = read_be32(bytes + FDT_OFF_RSVMAP);
	error = read_reservations(bytes, reservation_offset, totalsize, info);
	if (error != RISCV32_FDT_OK)
		return error;

	/* Walks the nodes. */
	error = walk_structure(&walk);
	if (error != RISCV32_FDT_OK)
		return error;

	/* Succeeded: the record describes the machine. */
	return RISCV32_FDT_OK;
}

/*
 * Names why a tree could not be read, for the boot log.
 */
const char *
riscv32_fdt_error(
	int error)
{
	/* Chooses the words for the error. */
	switch (error) {
	case RISCV32_FDT_OK:
		return "no error";
	case RISCV32_FDT_BAD_MAGIC:
		return "not a device tree";
	case RISCV32_FDT_BAD_SIZE:
		return "tree size out of bounds";
	case RISCV32_FDT_BAD_VERSION:
		return "tree format too old";
	case RISCV32_FDT_BAD_STRUCTURE:
		return "malformed structure block";
	case RISCV32_FDT_TOO_DEEP:
		return "nodes nested too deeply";
	case RISCV32_FDT_TOO_MANY:
		return "more entries than the HAL keeps";
	default:
		break;
	}

	/* An error this reader does not produce. */
	return "unknown error";
}

/*
 * Finds the PLIC context through which a hart takes supervisor external
 * interrupts.
 *
 * The context numbers are the order of the PLIC's interrupts-extended
 * list; the entry whose phandle is the hart's controller and whose
 * interrupt is the supervisor external one is the context.  Reports 0 and
 * the context, or -1 when the hart has none.
 */
int
riscv32_fdt_s_context(
	const struct riscv32_fdt_info *info,
	uint64_t hart_id,
	unsigned *context)
{
	uint32_t phandle;
	unsigned index;
	int found;

	/* Starts with no controller found. */
	phandle = 0;
	found = 0;

	/* Finds the hart's controller. */
	for (index = 0; index < info->cpu_count; index++) {
		if (info->cpus[index].hart_id != hart_id)
			continue;
		if (!info->cpus[index].has_intc)
			continue;
		phandle = info->cpus[index].intc_phandle;
		found = 1;
		break;
	}

	/* A hart the tree gives no controller has no context. */
	if (!found)
		return -1;

	/* Finds the context that delivers the hart's supervisor external interrupt. */
	for (index = 0; index < info->plic_context_count; index++) {
		if (info->plic_context_phandle[index] != phandle)
			continue;
		if (info->plic_context_irq[index] != FDT_INTC_S_EXTERNAL)
			continue;
		*context = index;
		return 0;
	}

	/* The PLIC does not reach the hart in supervisor mode. */
	return -1;
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
	/* One cell is a 32-bit number. */
	if (cells == 1U)
		return read_be32(bytes);

	/* No cells is the number zero. */
	if (cells == 0U)
		return 0;

	/* Two or more: the last two cells are the low 64 bits. */
	return read_be64(bytes + (cells - 2U) * 4U);
}

/* Empties the record before a parse. */
static void
clear_info(
	struct riscv32_fdt_info *info)
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

/* Reports whether a terminated string begins with a prefix. */
static int
string_has_prefix(
	const char *string,
	const char *prefix)
{
	size_t index;

	/* Compares up to the prefix's terminator. */
	for (index = 0; prefix[index] != '\0'; index++) {
		if (string[index] != prefix[index])
			return 0;
	}

	/* Succeeded: the string starts with the prefix. */
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
	struct riscv32_fdt_info *info)
{
	uint64_t base;
	uint64_t size;
	int error;

	/* Walks the pairs until the terminating one. */
	for (;;) {
		/* Refuses a block that runs past the tree. */
		if (offset > totalsize || totalsize - offset < 16U)
			return RISCV32_FDT_BAD_STRUCTURE;
		base = read_be64(blob + offset);
		size = read_be64(blob + offset + 8U);
		offset += 16U;

		/* The pair of zeros ends the block. */
		if (base == 0 && size == 0)
			break;
		error = add_range(info->reserved, &info->reserved_count, RISCV32_FDT_MAX_RESERVED, base, size);
		if (error != RISCV32_FDT_OK)
			return error;
	}

	/* Succeeded: every reservation is recorded. */
	return RISCV32_FDT_OK;
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
			return RISCV32_FDT_BAD_STRUCTURE;
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
				return RISCV32_FDT_BAD_STRUCTURE;
			offset = (end + 1U + 3U) & ~3U;
			error = begin_node(walk, name);
			if (error != RISCV32_FDT_OK)
				return error;
			break;
		case FDT_END_NODE:
			error = end_node(walk);
			if (error != RISCV32_FDT_OK)
				return error;
			break;
		case FDT_PROP:
			/* The value's length and the name's place in the strings block. */
			if (walk->struct_end - offset < 8U)
				return RISCV32_FDT_BAD_STRUCTURE;
			length = read_be32(blob + offset);
			name_offset = read_be32(blob + offset + 4U);
			offset += 8U;
			if (length > walk->struct_end - offset)
				return RISCV32_FDT_BAD_STRUCTURE;
			if (name_offset >= walk->strings_end - walk->strings_offset)
				return RISCV32_FDT_BAD_STRUCTURE;

			/* A property outside any node is malformed. */
			if (walk->depth == 0)
				return RISCV32_FDT_BAD_STRUCTURE;
			name = (const char *)(blob + walk->strings_offset + name_offset);
			take_property(walk, name, blob + offset, length);
			offset = (offset + length + 3U) & ~3U;
			break;
		case FDT_NOP:
			break;
		case FDT_END:
			/* Every node must have ended. */
			if (walk->depth != 0)
				return RISCV32_FDT_BAD_STRUCTURE;
			return RISCV32_FDT_OK;
		default:
			return RISCV32_FDT_BAD_STRUCTURE;
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
		return RISCV32_FDT_TOO_DEEP;

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
	return RISCV32_FDT_OK;
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
	case FDT_PROPERTY_STATUS:
		node->status = value;
		node->status_length = length;
		break;
	case FDT_PROPERTY_MMU_TYPE:
		node->mmu_type = value;
		break;
	case FDT_PROPERTY_ISA_EXTENSIONS:
		node->isa_extensions = value;
		node->isa_extensions_length = length;
		break;
	case FDT_PROPERTY_ISA:
		node->isa = value;
		node->isa_length = length;
		break;
	case FDT_PROPERTY_INTERRUPTS_EXTENDED:
		node->interrupts_extended = value;
		node->interrupts_extended_length = length;
		break;
	case FDT_PROPERTY_INTERRUPTS:
		node->interrupts = value;
		node->interrupts_length = length;
		break;
	case FDT_PROPERTY_TIMEBASE:
		node->timebase = value;
		node->timebase_length = length;
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
	case FDT_PROPERTY_PHANDLE:
		/* A phandle names the node for the properties that point at it. */
		if (length >= 4U) {
			node->phandle = number;
			node->has_phandle = 1;
		}

		break;
	case FDT_PROPERTY_SOURCES:
		node->sources = number;
		break;
	case FDT_PROPERTY_REG_SHIFT:
		node->reg_shift = number;
		break;
	case FDT_PROPERTY_REG_IO_WIDTH:
		node->reg_io_width = number;
		break;
	case FDT_PROPERTY_NO_MAP:
		node->no_map = 1;
		break;
	case FDT_PROPERTY_INTERRUPT_CONTROLLER:
		node->interrupt_controller = 1;
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
	struct riscv32_fdt_info *info;
	struct fdt_node *node;
	struct fdt_node *parent;
	enum fdt_role role;
	unsigned depth;
	int cpu_intc;
	int error;

	/* Refuses an end token without an open node. */
	if (walk->depth == 0)
		return RISCV32_FDT_BAD_STRUCTURE;

	/* Names the node, its parent (none for the root) and the record. */
	depth = walk->depth;
	node = &walk->nodes[depth - 1U];
	parent = NULL;
	if (depth >= 2U)
		parent = &walk->nodes[depth - 2U];
	info = walk->info;

	/* A hart's interrupt controller tells its parent, the hart, its phandle. */
	cpu_intc = node_is_cpu_intc(node);
	if (parent != NULL && cpu_intc) {
		parent->child_intc_phandle = node->phandle;
		parent->has_child_intc = 1;
	}

	/* Records what the node describes. */
	role = node_role(node, parent, depth);
	error = RISCV32_FDT_OK;
	switch (role) {
	case FDT_ROLE_CHOSEN:
		/* The command line the firmware passes, as a place in the tree. */
		if (node->bootargs != NULL && node->bootargs_length > 0) {
			info->bootargs_offset = (uint32_t)(node->bootargs - walk->blob);
			info->bootargs_length = node->bootargs_length;
		}

		/* The initial ramdisk the firmware loaded, when there is one. */
		if (node->initrd_start != NULL && node->initrd_end != NULL) {
			info->initrd_start = read_number(node->initrd_start, node->initrd_start_length);
			info->initrd_end = read_number(node->initrd_end, node->initrd_end_length);
		}

		break;
	case FDT_ROLE_CPUS:
		/* The timebase is usually given for all the harts at once. */
		if (node->timebase != NULL)
			info->timebase_frequency = read_number(node->timebase, node->timebase_length);
		break;
	case FDT_ROLE_CPU:
		error = keep_cpu(walk, node);
		break;
	case FDT_ROLE_MEMORY:
		error = add_reg_ranges(node, info->memory, &info->memory_count, RISCV32_FDT_MAX_MEMORY);
		break;
	case FDT_ROLE_RESERVATION:
		/* Every reservation is kept from allocation; no-map ones also from the maps. */
		error = add_reg_ranges(node, info->reserved, &info->reserved_count, RISCV32_FDT_MAX_RESERVED);
		if (error == RISCV32_FDT_OK && node->no_map)
			error = add_reg_ranges(node, info->nomap, &info->nomap_count, RISCV32_FDT_MAX_RESERVED);
		break;
	case FDT_ROLE_PLIC:
		error = keep_plic(walk, node);
		break;
	case FDT_ROLE_UART_DW_APB:
		keep_uart(walk, node, RISCV32_FDT_UART_DW_APB);
		break;
	case FDT_ROLE_UART_NS16550A:
		keep_uart(walk, node, RISCV32_FDT_UART_NS16550A);
		break;
	case FDT_ROLE_FWCFG:
		(void)read_first_reg(node, &info->fwcfg_base, &info->fwcfg_size);
		break;
	case FDT_ROLE_CCACHE:
		(void)read_first_reg(node, &info->ccache_base, &info->ccache_size);
		break;
	default:
		break;
	}

	/* Reports an entry the record has no room for. */
	if (error != RISCV32_FDT_OK)
		return error;

	/* Succeeded: the node is closed. */
	walk->depth--;
	return RISCV32_FDT_OK;
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
	int matched;

	/* The nodes directly under the root that the HAL reads by name. */
	if (depth == 2U) {
		matched = string_is(node->name, "chosen");
		if (matched)
			return FDT_ROLE_CHOSEN;
		matched = string_is(node->name, "cpus");
		if (matched)
			return FDT_ROLE_CPUS;
	}

	/* The children of /cpus that are harts, and of /reserved-memory. */
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
	matched = list_has(node->compatible, node->compatible_length, "sifive,plic-1.0.0");
	if (matched)
		return FDT_ROLE_PLIC;
	matched = list_has(node->compatible, node->compatible_length, "riscv,plic0");
	if (matched)
		return FDT_ROLE_PLIC;
	matched = list_has(node->compatible, node->compatible_length, "snps,dw-apb-uart");
	if (matched)
		return FDT_ROLE_UART_DW_APB;
	matched = list_has(node->compatible, node->compatible_length, "ns16550a");
	if (matched)
		return FDT_ROLE_UART_NS16550A;
	matched = list_has(node->compatible, node->compatible_length, "qemu,fw-cfg-mmio");
	if (matched)
		return FDT_ROLE_FWCFG;
	matched = list_has(node->compatible, node->compatible_length, "sifive,ccache0");
	if (matched)
		return FDT_ROLE_CCACHE;

	/* Nothing the HAL reads. */
	return FDT_ROLE_NONE;
}

/* Reports whether a node is a hart's local interrupt controller. */
static int
node_is_cpu_intc(
	const struct fdt_node *node)
{
	int compatible;

	/* It is an interrupt controller with a phandle the PLIC can name. */
	if (!node->interrupt_controller)
		return 0;
	if (!node->has_phandle)
		return 0;

	/* And it says it is a hart's controller. */
	compatible = list_has(node->compatible, node->compatible_length, "riscv,cpu-intc");
	if (!compatible)
		return 0;
	return 1;
}

/* Adds one range to a table of ranges; an empty one is skipped. */
static int
add_range(
	struct riscv32_fdt_range *ranges,
	unsigned *count,
	unsigned capacity,
	uint64_t base,
	uint64_t size)
{
	/* An empty range adds nothing. */
	if (size == 0)
		return RISCV32_FDT_OK;

	/* Refuses a table that is full. */
	if (*count >= capacity)
		return RISCV32_FDT_TOO_MANY;

	/* Succeeded: the range is the table's last. */
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;
	return RISCV32_FDT_OK;
}

/* Reads the first address and size of a node's reg. */
static int
read_first_reg(
	const struct fdt_node *node,
	uint64_t *base,
	uint64_t *size)
{
	uint32_t entry_length;

	/* A node without a whole entry has no address. */
	entry_length = (node->reg_address_cells + node->reg_size_cells) * 4U;
	if (node->reg == NULL ||
	    entry_length == 0 ||
	    node->reg_length < entry_length)
		return -1;

	/* Succeeded: the entry's address and size. */
	*base = read_cells(node->reg, node->reg_address_cells);
	*size = read_cells(node->reg + node->reg_address_cells * 4U, node->reg_size_cells);
	return 0;
}

/* Adds every address and size of a node's reg to a table of ranges. */
static int
add_reg_ranges(
	const struct fdt_node *node,
	struct riscv32_fdt_range *ranges,
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
		return RISCV32_FDT_OK;

	/* Adds each whole entry. */
	for (offset = 0; offset + entry_length <= node->reg_length; offset += entry_length) {
		base = read_cells(node->reg + offset, node->reg_address_cells);
		size = read_cells(node->reg + offset + node->reg_address_cells * 4U, node->reg_size_cells);
		error = add_range(ranges, count, capacity, base, size);
		if (error != RISCV32_FDT_OK)
			return error;
	}

	/* Succeeded: every entry is in the table. */
	return RISCV32_FDT_OK;
}

/*
 * Reads which of the recorded extensions a hart has.
 *
 * A tree of the current binding lists them in riscv,isa-extensions; an
 * older one has only the riscv,isa string, whose multi-letter extensions
 * follow underscores.
 */
static uint32_t
cpu_extensions(
	const struct fdt_node *node)
{
	const char *isa;
	uint32_t extensions;
	uint32_t index;
	size_t name;
	int present;

	/* The list of the current binding. */
	extensions = 0;
	if (node->isa_extensions != NULL) {
		for (name = 0; name < sizeof(extension_names) / sizeof(extension_names[0]); name++) {
			present = list_has(node->isa_extensions, node->isa_extensions_length, extension_names[name].name);
			if (present)
				extensions |= extension_names[name].bit;
		}

		/* Succeeded: the extensions found. */
		return extensions;
	}

	/* A tree without the list: nothing can be said without a terminated string. */
	if (node->isa == NULL || node->isa_length == 0)
		return 0;
	if (node->isa[node->isa_length - 1U] != 0)
		return 0;

	/* Looks at each underscore-led name of the string. */
	isa = (const char *)node->isa;
	for (index = 0; isa[index] != '\0'; index++) {
		if (isa[index] != '_')
			continue;
		for (name = 0; name < sizeof(extension_names) / sizeof(extension_names[0]); name++) {
			present = string_has_prefix(isa + index + 1U, extension_names[name].name);
			if (present)
				extensions |= extension_names[name].bit;
		}
	}

	/* Succeeded: the extensions the string names. */
	return extensions;
}

/* Reads a property of one or two cells as a number. */
static uint64_t
read_number(
	const uint8_t *value,
	uint32_t length)
{
	/* Two cells are a 64-bit number. */
	if (length >= 8U)
		return read_be64(value);

	/* One cell is a 32-bit number. */
	if (length >= 4U)
		return read_be32(value);

	/* A shorter value is no number. */
	return 0;
}

/*
 * Records one hart from its node under /cpus.
 */
static int
keep_cpu(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct riscv32_fdt_info *info;
	struct riscv32_fdt_cpu *cpu;
	uint64_t hart_id;
	uint32_t entry_length;
	int disabled;
	int okay;

	/* A hart without a whole reg has no id and is not recorded. */
	info = walk->info;
	entry_length = node->reg_address_cells * 4U;
	if (node->reg == NULL ||
	    entry_length == 0 ||
	    node->reg_length < entry_length)
		return RISCV32_FDT_OK;
	hart_id = read_cells(node->reg, node->reg_address_cells);

	/* Refuses a table that is full. */
	if (info->cpu_count >= RISCV32_FDT_MAX_CPUS)
		return RISCV32_FDT_TOO_MANY;

	/* A hart may give its own timebase when /cpus does not. */
	if (info->timebase_frequency == 0 && node->timebase != NULL)
		info->timebase_frequency = read_number(node->timebase, node->timebase_length);

	/* Records the hart, usable when it translates addresses and is not disabled. */
	cpu = &info->cpus[info->cpu_count];
	cpu->hart_id = hart_id;
	cpu->intc_phandle = node->child_intc_phandle;
	cpu->has_intc = node->has_child_intc;
	cpu->extensions = cpu_extensions(node);
	disabled = 0;
	if (node->status != NULL) {
		/* A status other than okay (or its old spelling ok) disables the hart. */
		okay = bytes_are(node->status, node->status_length, "okay");
		if (!okay)
			okay = bytes_are(node->status, node->status_length, "ok");
		if (!okay)
			disabled = 1;
	}

	/* A hart is started only when it has an MMU and is not disabled. */
	cpu->usable = 0;
	if (node->mmu_type != NULL && !disabled)
		cpu->usable = 1;
	info->cpu_count++;

	/* Succeeded: the hart is in the table. */
	return RISCV32_FDT_OK;
}

/*
 * Records the PLIC: its registers, its sources and its contexts.
 */
static int
keep_plic(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct riscv32_fdt_info *info;
	uint32_t offset;
	unsigned count;

	/* The register block. */
	info = walk->info;
	(void)read_first_reg(node, &info->plic_base, &info->plic_size);
	info->plic_sources = node->sources;

	/* The contexts: pairs of a hart controller's phandle and its interrupt. */
	count = 0;
	if (node->interrupts_extended != NULL) {
		for (offset = 0; offset + 8U <= node->interrupts_extended_length; offset += 8U) {
			/* Refuses more contexts than the record keeps. */
			if (count >= RISCV32_FDT_MAX_CONTEXTS)
				return RISCV32_FDT_TOO_MANY;
			info->plic_context_phandle[count] = read_be32(node->interrupts_extended + offset);
			info->plic_context_irq[count] = read_be32(node->interrupts_extended + offset + 4U);
			count++;
		}
	}

	/* Records how many contexts were found. */
	info->plic_context_count = count;

	/* Succeeded: the PLIC is recorded. */
	return RISCV32_FDT_OK;
}

/*
 * Records the first UART the tree lists, the one the firmware's console
 * uses on the machines this port runs on.
 */
static void
keep_uart(
	struct fdt_walk *walk,
	const struct fdt_node *node,
	unsigned kind)
{
	struct riscv32_fdt_info *info;
	int error;

	/* Keeps only the first UART. */
	info = walk->info;
	if (info->uart_kind != RISCV32_FDT_UART_NONE)
		return;

	/* Its registers, their spacing and width, and its interrupt. */
	error = read_first_reg(node, &info->uart_base, &info->uart_size);
	if (error != 0)
		return;
	info->uart_kind = kind;
	info->uart_reg_shift = node->reg_shift;
	info->uart_reg_io_width = node->reg_io_width;
	if (node->interrupts != NULL && node->interrupts_length >= 4U)
		info->uart_irq = read_be32(node->interrupts);
}
