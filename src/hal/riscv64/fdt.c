/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 HAL's reader of the flattened device tree.
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
	struct riscv64_fdt_info *info;
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
	{"sstc", RISCV64_FDT_EXT_SSTC},
	{"svpbmt", RISCV64_FDT_EXT_SVPBMT},
	{"zicbom", RISCV64_FDT_EXT_ZICBOM},
	{"zkr", RISCV64_FDT_EXT_ZKR}
};

/*
 * The base extensions a hart needs to run the system, by their letters: the
 * kernel is built for rv64imac and the userland for rv64gc, that is imafdc.
 *
 * Values referenced: the -march of platform/riscv64/vmunix.mk and of
 * platform/riscv64/userland.mk.
 */
static const char system_base_letters[] = "imafdc";

/*
 * The width a riscv,isa string must start with, and the letters its
 * single-letter g stands for.
 *
 * Values referenced: RISC-V unprivileged ISA (riscv/riscv-isa-manual), ISA
 * extension naming conventions: g abbreviates imafd with Zicsr and Zifencei.
 */
static const char isa_width_prefix[] = "rv64";
static const char isa_general_letters[] = "imafd";

/*
 * The letters that start a multi-letter name, which ends a riscv,isa
 * string's single letters when no underscore comes first; every base
 * extension the system needs comes before them in the canonical order.
 *
 * Values referenced: RISC-V unprivileged ISA (riscv/riscv-isa-manual), ISA
 * extension naming conventions, the canonical order of the names.
 */
static const char isa_multi_letter_starts[] = "hsxz";

static uint32_t read_be32(const uint8_t *bytes);
static uint64_t read_be64(const uint8_t *bytes);
static uint64_t read_cells(const uint8_t *bytes, uint32_t cells);
static void clear_info(struct riscv64_fdt_info *info);
static void clear_node(struct fdt_node *node);
static int string_is(const char *string, const char *wanted);
static int string_has_prefix(const char *string, const char *prefix);
static int bytes_are(const uint8_t *bytes, uint32_t length, const char *wanted);
static int list_has(const uint8_t *list, uint32_t length, const char *wanted);
static int read_reservations(const uint8_t *blob, uint32_t offset, uint32_t totalsize, struct riscv64_fdt_info *info);
static int walk_structure(struct fdt_walk *walk);
static int begin_node(struct fdt_walk *walk, const char *name);
static enum fdt_property property_kind(const char *name);
static void take_property(struct fdt_walk *walk, const char *name, const uint8_t *value, uint32_t length);
static enum fdt_role node_role(const struct fdt_node *node, const struct fdt_node *parent, unsigned depth);
static int node_is_cpu_intc(const struct fdt_node *node);
static int end_node(struct fdt_walk *walk);
static int add_range(struct riscv64_fdt_range *ranges, unsigned *count, unsigned capacity, uint64_t base, uint64_t size);
static int read_first_reg(const struct fdt_node *node, uint64_t *base, uint64_t *size);
static int add_reg_ranges(const struct fdt_node *node, struct riscv64_fdt_range *ranges, unsigned *count, unsigned capacity);
static uint32_t cpu_extensions(const struct fdt_node *node);
static uint32_t letter_bits(const char *letters);
static int cpu_runs_system(const struct fdt_node *node);
static uint64_t read_number(const uint8_t *value, uint32_t length);
static int keep_cpu(struct fdt_walk *walk, const struct fdt_node *node);
static int keep_plic(struct fdt_walk *walk, const struct fdt_node *node);
static void keep_uart(struct fdt_walk *walk, const struct fdt_node *node, unsigned kind);

/*
 * Reads a flattened device tree into the HAL's record of the machine.
 *
 * limit is the most bytes the caller lets the reader look at; the tree's
 * own size must fit inside it.  Returns RISCV64_FDT_OK, or why the tree
 * could not be read, in which case the record is incomplete.
 */
int
riscv64_fdt_parse(
	const void *blob,
	size_t limit,
	struct riscv64_fdt_info *info)
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
		return RISCV64_FDT_BAD_SIZE;

	/* Refuses memory that is not a tree. */
	magic = read_be32(bytes);
	if (magic != FDT_MAGIC)
		return RISCV64_FDT_BAD_MAGIC;

	/* Refuses a tree larger than the caller allows. */
	totalsize = read_be32(bytes + FDT_OFF_TOTALSIZE);
	if (totalsize < FDT_HEADER_SIZE || (size_t)totalsize > limit)
		return RISCV64_FDT_BAD_SIZE;

	/* Records how many bytes the tree takes. */
	info->totalsize = totalsize;

	/* Refuses a format older than the one with the block sizes. */
	version = read_be32(bytes + FDT_OFF_VERSION);
	if (version < FDT_MIN_VERSION)
		return RISCV64_FDT_BAD_VERSION;

	/* Locates the structure and strings blocks inside the tree. */
	walk.blob = bytes;
	walk.info = info;
	walk.depth = 0;
	walk.struct_offset = read_be32(bytes + FDT_OFF_STRUCT);
	struct_size = read_be32(bytes + FDT_OFF_STRUCT_SIZE);
	walk.strings_offset = read_be32(bytes + FDT_OFF_STRINGS);
	strings_size = read_be32(bytes + FDT_OFF_STRINGS_SIZE);

	/* Refuses a block that lies outside the tree. */
	if (walk.struct_offset > totalsize || struct_size > totalsize - walk.struct_offset)
		return RISCV64_FDT_BAD_STRUCTURE;
	if (walk.strings_offset > totalsize || strings_size > totalsize - walk.strings_offset)
		return RISCV64_FDT_BAD_STRUCTURE;

	/* Notes where each block ends. */
	walk.struct_end = walk.struct_offset + struct_size;
	walk.strings_end = walk.strings_offset + strings_size;

	/* Reads the reservation block, which comes before the structure. */
	reservation_offset = read_be32(bytes + FDT_OFF_RSVMAP);
	error = read_reservations(bytes, reservation_offset, totalsize, info);
	if (error != RISCV64_FDT_OK)
		return error;

	/* Walks the nodes. */
	error = walk_structure(&walk);
	if (error != RISCV64_FDT_OK)
		return error;

	/* Succeeded: the record describes the machine. */
	return RISCV64_FDT_OK;
}

/*
 * Names why a tree could not be read, for the boot log.
 */
const char *
riscv64_fdt_error(
	int error)
{
	/* Chooses the words for the error. */
	switch (error) {
	case RISCV64_FDT_OK:
		return "no error";
	case RISCV64_FDT_BAD_MAGIC:
		return "not a device tree";
	case RISCV64_FDT_BAD_SIZE:
		return "tree size out of bounds";
	case RISCV64_FDT_BAD_VERSION:
		return "tree format too old";
	case RISCV64_FDT_BAD_STRUCTURE:
		return "malformed structure block";
	case RISCV64_FDT_TOO_DEEP:
		return "nodes nested too deeply";
	case RISCV64_FDT_TOO_MANY:
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
riscv64_fdt_s_context(
	const struct riscv64_fdt_info *info,
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
		/* Another hart's record is not the one. */
		if (info->cpus[index].hart_id != hart_id)
			continue;

		/* A record without a controller is not either. */
		if (!info->cpus[index].has_intc)
			continue;

		/* Takes the controller's phandle. */
		phandle = info->cpus[index].intc_phandle;
		found = 1;
		break;
	}

	/* A hart the tree gives no controller has no context. */
	if (!found)
		return -1;

	/* Finds the context that delivers the hart's supervisor external interrupt. */
	found = 0;
	for (index = 0; index < info->plic_context_count; index++) {
		/* A context of another hart's controller is not the one. */
		if (info->plic_context_phandle[index] != phandle)
			continue;

		/* A context for another interrupt, the machine-mode one, is not either. */
		if (info->plic_context_irq[index] != FDT_INTC_S_EXTERNAL)
			continue;

		/* Takes the context. */
		found = 1;
		break;
	}

	/* The PLIC does not reach the hart in supervisor mode. */
	if (!found)
		return -1;

	/* Succeeded: reports the context. */
	*context = index;
	return 0;
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

	/* One cell is a 32-bit number; of two or more, the last two cells are the low 64 bits. */
	if (cells == 1U) {
		number = read_be32(bytes);
	} else {
		number = read_be64(bytes + (cells - 2U) * 4U);
	}

	/* Succeeded: the number. */
	return number;
}

/* Empties the record before a parse. */
static void
clear_info(
	struct riscv64_fdt_info *info)
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

	/* Succeeded: the strings are equal. */
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

	/* Succeeded: the value is the string. */
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
		/* Finds the terminator of the string that starts here. */
		end = start;
		while (end < length && list[end] != 0)
			end++;

		/* Stops at a list whose last string has no terminator. */
		if (end >= length)
			return 0;

		/* Reports the list as holding the string when this one is it. */
		equal = bytes_are(list + start, end + 1U - start, wanted);
		if (equal)
			return 1;

		/* Steps past the terminator to the next string. */
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
	struct riscv64_fdt_info *info)
{
	uint64_t base;
	uint64_t size;
	int error;

	/* Walks the pairs until the terminating one. */
	for (;;) {
		/* Refuses a block that runs past the tree. */
		if (offset > totalsize || totalsize - offset < 16U)
			return RISCV64_FDT_BAD_STRUCTURE;

		/* Reads the pair's address and size. */
		base = read_be64(blob + offset);
		size = read_be64(blob + offset + 8U);
		offset += 16U;

		/* The pair of zeros ends the block. */
		if (base == 0 && size == 0)
			break;

		/* Records the reservation. */
		error = add_range(info->reserved, &info->reserved_count, RISCV64_FDT_MAX_RESERVED, base, size);
		if (error != RISCV64_FDT_OK)
			return error;
	}

	/* Succeeded: every reservation is recorded. */
	return RISCV64_FDT_OK;
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
			return RISCV64_FDT_BAD_STRUCTURE;

		/* Reads the next token. */
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

			/* Refuses a name without a terminator inside the block. */
			if (end >= walk->struct_end)
				return RISCV64_FDT_BAD_STRUCTURE;

			/* Opens the node and steps past its padded name. */
			offset = (end + 1U + 3U) & ~3U;
			error = begin_node(walk, name);
			if (error != RISCV64_FDT_OK)
				return error;
			break;
		case FDT_END_NODE:
			/* Closes the node and records what it describes. */
			error = end_node(walk);
			if (error != RISCV64_FDT_OK)
				return error;
			break;
		case FDT_PROP:
			/* The value's length and the name's place in the strings block. */
			if (walk->struct_end - offset < 8U)
				return RISCV64_FDT_BAD_STRUCTURE;
			length = read_be32(blob + offset);
			name_offset = read_be32(blob + offset + 4U);
			offset += 8U;

			/* Refuses a value or a name outside its block. */
			if (length > walk->struct_end - offset)
				return RISCV64_FDT_BAD_STRUCTURE;
			if (name_offset >= walk->strings_end - walk->strings_offset)
				return RISCV64_FDT_BAD_STRUCTURE;

			/* A property outside any node is malformed. */
			if (walk->depth == 0)
				return RISCV64_FDT_BAD_STRUCTURE;

			/* Keeps the property and steps past its padded value. */
			name = (const char *)(blob + walk->strings_offset + name_offset);
			take_property(walk, name, blob + offset, length);
			offset = (offset + length + 3U) & ~3U;
			break;
		case FDT_NOP:
			/* A no-op token carries nothing. */
			break;
		case FDT_END:
			/* Every node must have ended. */
			if (walk->depth != 0)
				return RISCV64_FDT_BAD_STRUCTURE;

			/* Succeeded: the whole structure block is read. */
			return RISCV64_FDT_OK;
		default:
			/* Any other token is malformed. */
			return RISCV64_FDT_BAD_STRUCTURE;
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
		return RISCV64_FDT_TOO_DEEP;

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

	/* Pushes the node on the stack of open nodes. */
	walk->depth++;

	/* Succeeded: the node is open. */
	return RISCV64_FDT_OK;
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
		/* Reports the kind of the first kept name that matches. */
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
	struct riscv64_fdt_info *info;
	struct fdt_node *node;
	struct fdt_node *parent;
	enum fdt_role role;
	unsigned depth;
	int cpu_intc;
	int error;

	/* Refuses an end token without an open node. */
	if (walk->depth == 0)
		return RISCV64_FDT_BAD_STRUCTURE;

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
	error = RISCV64_FDT_OK;
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
		/* A hart: its id, its controller and its extensions. */
		error = keep_cpu(walk, node);
		break;
	case FDT_ROLE_MEMORY:
		/* The RAM ranges. */
		error = add_reg_ranges(node, info->memory, &info->memory_count, RISCV64_FDT_MAX_MEMORY);
		break;
	case FDT_ROLE_RESERVATION:
		/* Every reservation is kept from allocation; no-map ones also from the maps. */
		error = add_reg_ranges(node, info->reserved, &info->reserved_count, RISCV64_FDT_MAX_RESERVED);
		if (error == RISCV64_FDT_OK && node->no_map)
			error = add_reg_ranges(node, info->nomap, &info->nomap_count, RISCV64_FDT_MAX_RESERVED);
		break;
	case FDT_ROLE_PLIC:
		/* The interrupt controller and its contexts. */
		error = keep_plic(walk, node);
		break;
	case FDT_ROLE_UART_DW_APB:
		/* A DesignWare UART, a candidate for the console. */
		keep_uart(walk, node, RISCV64_FDT_UART_DW_APB);
		break;
	case FDT_ROLE_UART_NS16550A:
		/* A 16550 UART, a candidate for the console. */
		keep_uart(walk, node, RISCV64_FDT_UART_NS16550A);
		break;
	case FDT_ROLE_FWCFG:
		/* The firmware configuration interface's registers, when the tree gives them. */
		(void)read_first_reg(node, &info->fwcfg_base, &info->fwcfg_size);
		break;
	case FDT_ROLE_CCACHE:
		/* The cache controller's registers, when the tree gives them. */
		(void)read_first_reg(node, &info->ccache_base, &info->ccache_size);
		break;
	default:
		break;
	}

	/* Reports an entry the record has no room for. */
	if (error != RISCV64_FDT_OK)
		return error;

	/* Pops the node off the stack of open nodes. */
	walk->depth--;

	/* Succeeded: the node is closed. */
	return RISCV64_FDT_OK;
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
		/* The firmware's choices: the command line and the ramdisk. */
		matched = string_is(node->name, "chosen");
		if (matched)
			return FDT_ROLE_CHOSEN;

		/* The parent of the harts. */
		matched = string_is(node->name, "cpus");
		if (matched)
			return FDT_ROLE_CPUS;
	}

	/* The children of /cpus that are harts, and of /reserved-memory. */
	if (depth == 3U) {
		matched = string_is(parent->name, "cpus");
		if (matched) {
			/* A child of /cpus whose type is cpu is a hart. */
			matched = bytes_are(node->device_type, node->device_type_length, "cpu");
			if (matched)
				return FDT_ROLE_CPU;

			/* Any other child of /cpus, such as a cpu-map, is not read. */
			return FDT_ROLE_NONE;
		}

		/* A child of /reserved-memory is a reservation. */
		matched = string_is(parent->name, "reserved-memory");
		if (matched)
			return FDT_ROLE_RESERVATION;
	}

	/* A memory node, wherever the tree puts it. */
	matched = bytes_are(node->device_type, node->device_type_length, "memory");
	if (matched)
		return FDT_ROLE_MEMORY;

	/* The PLIC, under either of its two names. */
	matched = list_has(node->compatible, node->compatible_length, "sifive,plic-1.0.0");
	if (matched)
		return FDT_ROLE_PLIC;
	matched = list_has(node->compatible, node->compatible_length, "riscv,plic0");
	if (matched)
		return FDT_ROLE_PLIC;

	/* A DesignWare UART. */
	matched = list_has(node->compatible, node->compatible_length, "snps,dw-apb-uart");
	if (matched)
		return FDT_ROLE_UART_DW_APB;

	/* A 16550 UART. */
	matched = list_has(node->compatible, node->compatible_length, "ns16550a");
	if (matched)
		return FDT_ROLE_UART_NS16550A;

	/* The firmware configuration interface. */
	matched = list_has(node->compatible, node->compatible_length, "qemu,fw-cfg-mmio");
	if (matched)
		return FDT_ROLE_FWCFG;

	/* The cache controller. */
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

	/* Succeeded: the node is a hart's controller. */
	return 1;
}

/* Adds one range to a table of ranges; an empty one is skipped. */
static int
add_range(
	struct riscv64_fdt_range *ranges,
	unsigned *count,
	unsigned capacity,
	uint64_t base,
	uint64_t size)
{
	/* An empty range adds nothing. */
	if (size == 0)
		return RISCV64_FDT_OK;

	/* Refuses a table that is full. */
	if (*count >= capacity)
		return RISCV64_FDT_TOO_MANY;

	/* Appends the range and counts it. */
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;

	/* Succeeded: the range is the table's last. */
	return RISCV64_FDT_OK;
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

	/* Reads the entry's address and size. */
	*base = read_cells(node->reg, node->reg_address_cells);
	*size = read_cells(node->reg + node->reg_address_cells * 4U, node->reg_size_cells);

	/* Succeeded: the caller has the first entry. */
	return 0;
}

/* Adds every address and size of a node's reg to a table of ranges. */
static int
add_reg_ranges(
	const struct fdt_node *node,
	struct riscv64_fdt_range *ranges,
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
		return RISCV64_FDT_OK;

	/* Adds each whole entry. */
	for (offset = 0; offset + entry_length <= node->reg_length; offset += entry_length) {
		base = read_cells(node->reg + offset, node->reg_address_cells);
		size = read_cells(node->reg + offset + node->reg_address_cells * 4U, node->reg_size_cells);
		error = add_range(ranges, count, capacity, base, size);
		if (error != RISCV64_FDT_OK)
			return error;
	}

	/* Succeeded: every entry is in the table. */
	return RISCV64_FDT_OK;
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
		/* Looks for each recorded name in the list. */
		for (name = 0; name < sizeof(extension_names) / sizeof(extension_names[0]); name++) {
			present = list_has(node->isa_extensions, node->isa_extensions_length, extension_names[name].name);
			if (present)
				extensions |= extension_names[name].bit;
		}

		/* Succeeded: the extensions the list names. */
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
		/* Only an underscore starts a multi-letter name. */
		if (isa[index] != '_')
			continue;

		/* Compares the name with each recorded one. */
		for (name = 0; name < sizeof(extension_names) / sizeof(extension_names[0]); name++) {
			present = string_has_prefix(isa + index + 1U, extension_names[name].name);
			if (present)
				extensions |= extension_names[name].bit;
		}
	}

	/* Succeeded: the extensions the string names. */
	return extensions;
}

/* Gives each lowercase letter of a string its bit, a as bit 0. */
static uint32_t
letter_bits(
	const char *letters)
{
	uint32_t bits;
	size_t index;

	/* Sets the bit of each letter; anything but a lowercase letter has none. */
	bits = 0;
	for (index = 0; letters[index] != '\0'; index++) {
		if (letters[index] >= 'a' && letters[index] <= 'z')
			bits |= 1U << (uint32_t)(letters[index] - 'a');
	}

	/* Succeeded: the set of the string's letters. */
	return bits;
}

/*
 * Reports whether a hart has every base extension the system is built for.
 *
 * A tree of the current binding lists each base extension by its letter in
 * riscv,isa-extensions; an older one has only the riscv,isa string, whose
 * single letters follow the width up to the first multi-letter name.  A
 * monitor core such as the JH7110's S7 (rv64imac) lacks F and D.  A hart
 * whose tree says neither cannot be shown to have them.
 */
static int
cpu_runs_system(
	const struct fdt_node *node)
{
	const char *isa;
	uint32_t required;
	uint32_t general;
	uint32_t ends;
	uint32_t seen;
	uint32_t bit;
	uint32_t index;
	size_t wanted;
	char name[2];
	int present;

	/* The list of the current binding names each base extension on its own. */
	if (node->isa_extensions != NULL) {
		/* Looks for each wanted letter as a name of the list. */
		name[1] = '\0';
		for (wanted = 0; system_base_letters[wanted] != '\0'; wanted++) {
			name[0] = system_base_letters[wanted];
			present = list_has(node->isa_extensions, node->isa_extensions_length, name);
			if (!present)
				return 0;
		}

		/* Succeeded: the list names every base extension the system needs. */
		return 1;
	}

	/* A tree without the list: nothing can be said without a terminated string. */
	if (node->isa == NULL || node->isa_length == 0)
		return 0;
	if (node->isa[node->isa_length - 1U] != 0)
		return 0;

	/* A hart of another width cannot run the system. */
	isa = (const char *)node->isa;
	present = string_has_prefix(isa, isa_width_prefix);
	if (!present)
		return 0;

	/* Notes the single letters after the width, up to the first multi-letter name. */
	general = letter_bits(isa_general_letters);
	ends = letter_bits(isa_multi_letter_starts);
	seen = 0;
	for (index = sizeof(isa_width_prefix) - 1U; isa[index] != '\0'; index++) {
		/* A digit belongs to a version number and names nothing. */
		if (isa[index] >= '0' && isa[index] <= '9')
			continue;

		/* An underscore, or anything but a lowercase letter, ends the single letters. */
		if (isa[index] < 'a' || isa[index] > 'z')
			break;

		/* A multi-letter name without an underscore before it ends them too. */
		bit = 1U << (uint32_t)(isa[index] - 'a');
		if ((ends & bit) != 0)
			break;

		/* Notes the letter as an extension the hart has. */
		seen |= bit;

		/* A g has the letters it stands for as well. */
		if (isa[index] == 'g')
			seen |= general;
	}

	/* Refuses a hart without every base extension the system needs. */
	required = letter_bits(system_base_letters);
	if ((seen & required) != required)
		return 0;

	/* Succeeded: the string names every base extension the system needs. */
	return 1;
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

	/* Two cells are a 64-bit number, one cell a 32-bit number. */
	if (length >= 8U) {
		number = read_be64(value);
	} else {
		number = read_be32(value);
	}

	/* Succeeded: the number. */
	return number;
}

/*
 * Records one hart from its node under /cpus.
 */
static int
keep_cpu(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct riscv64_fdt_info *info;
	struct riscv64_fdt_cpu *cpu;
	uint64_t hart_id;
	uint32_t entry_length;
	int runs_system;
	int disabled;
	int okay;

	/* A hart without a whole reg has no id and is not recorded. */
	info = walk->info;
	entry_length = node->reg_address_cells * 4U;
	if (node->reg == NULL ||
	    entry_length == 0 ||
	    node->reg_length < entry_length)
		return RISCV64_FDT_OK;

	/* The hart id is the reg's address. */
	hart_id = read_cells(node->reg, node->reg_address_cells);

	/* Refuses a table that is full. */
	if (info->cpu_count >= RISCV64_FDT_MAX_CPUS)
		return RISCV64_FDT_TOO_MANY;

	/* A hart may give its own timebase when /cpus does not. */
	if (info->timebase_frequency == 0 && node->timebase != NULL)
		info->timebase_frequency = read_number(node->timebase, node->timebase_length);

	/* Records the hart's id, its controller and its extensions. */
	cpu = &info->cpus[info->cpu_count];
	cpu->hart_id = hart_id;
	cpu->intc_phandle = node->child_intc_phandle;
	cpu->has_intc = node->has_child_intc;
	cpu->extensions = cpu_extensions(node);

	/* Decides whether the tree disables the hart. */
	disabled = 0;
	if (node->status != NULL) {
		/* A status other than okay (or its old spelling ok) disables the hart. */
		okay = bytes_are(node->status, node->status_length, "okay");
		if (!okay)
			okay = bytes_are(node->status, node->status_length, "ok");
		if (!okay)
			disabled = 1;
	}

	/* Decides whether the hart has the base extensions the system is built for. */
	runs_system = cpu_runs_system(node);

	/*
	 * A hart is usable when it has an MMU, has those extensions and is not
	 * disabled; it is counted either way.  A firmware's tree may enable a
	 * monitor core and give it an mmu-type, as U-Boot's control tree does for
	 * the JH7110's S7, so the MMU alone does not make a hart usable.
	 */
	cpu->usable = 0;
	if (node->mmu_type != NULL &&
	    runs_system &&
	    !disabled)
		cpu->usable = 1;

	/* Counts the hart in the table. */
	info->cpu_count++;

	/* Succeeded: the hart is in the table. */
	return RISCV64_FDT_OK;
}

/*
 * Records the PLIC: its registers, its sources and its contexts.
 */
static int
keep_plic(
	struct fdt_walk *walk,
	const struct fdt_node *node)
{
	struct riscv64_fdt_info *info;
	uint32_t offset;
	unsigned count;

	/* The register block. */
	info = walk->info;
	(void)read_first_reg(node, &info->plic_base, &info->plic_size);
	info->plic_sources = node->sources;

	/* The contexts: pairs of a hart controller's phandle and its interrupt. */
	count = 0;
	if (node->interrupts_extended != NULL) {
		/* Reads each pair in the list's order, which numbers the contexts. */
		for (offset = 0; offset + 8U <= node->interrupts_extended_length; offset += 8U) {
			/* Refuses more contexts than the record keeps. */
			if (count >= RISCV64_FDT_MAX_CONTEXTS)
				return RISCV64_FDT_TOO_MANY;

			/* Records the context's controller and interrupt. */
			info->plic_context_phandle[count] = read_be32(node->interrupts_extended + offset);
			info->plic_context_irq[count] = read_be32(node->interrupts_extended + offset + 4U);
			count++;
		}
	}

	/* Records how many contexts were read. */
	info->plic_context_count = count;

	/* Succeeded: the PLIC is recorded. */
	return RISCV64_FDT_OK;
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
	struct riscv64_fdt_info *info;
	int error;

	/* Keeps only the first UART. */
	info = walk->info;
	if (info->uart_kind != RISCV64_FDT_UART_NONE)
		return;

	/* Its registers, their spacing and width, and its interrupt. */
	error = read_first_reg(node, &info->uart_base, &info->uart_size);
	if (error != 0)
		return;

	/* Records the UART's kind and its register layout. */
	info->uart_kind = kind;
	info->uart_reg_shift = node->reg_shift;
	info->uart_reg_io_width = node->reg_io_width;

	/* Records its interrupt, when the tree gives one. */
	if (node->interrupts != NULL && node->interrupts_length >= 4U)
		info->uart_irq = read_be32(node->interrupts);
}
