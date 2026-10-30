/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5's device tree, read by the HAL at boot: the board, the
 * RAM and the reserved ranges (the reservation block's and
 * /reserved-memory's), the CPUs, the command line, the UART and the GIC
 * the HAL uses, and the framebuffer the bootloader left scanning out.
 *
 * A node whose status is neither "okay" nor "ok" is a block the board
 * does not wire up, or memory the firmware does not keep: it records
 * nothing.
 *
 * The parser walks the flattened tree once, keeps a stack of the open
 * nodes, and records what a node says when the node closes.
 */

#include <hal/hal.h>
#include "../bsp-rpi-common/fdt.h"

/* The blob's magic number and the structure block's tokens. */
#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_NOP 4U
#define FDT_END 9U

/* The deepest tree, and the most cells of one property, the parser keeps. */
#define FDT_MAX_DEPTH 24
#define FDT_MAX_CELLS 32
#define FDT_MAX_INTERRUPT_CELLS 12

/* The header's size, and the largest blob the parser accepts. */
#define FDT_HEADER_BYTES 40U
#define FDT_MAX_BLOB_BYTES (2U * 1024U * 1024U)

/* The parse's results; rpi_fdt_error() names them in this order. */
enum {
	FDT_OK,
	FDT_E_HEADER,
	FDT_E_BOUNDS,
	FDT_E_STRUCTURE,
	FDT_E_DEPTH,
	FDT_E_CELLS,
	FDT_E_OVERFLOW,
	FDT_E_NOT_BOARD
};

/*
 * The simple-framebuffer formats the console can paint: four bytes a
 * pixel, with blue or red in the low byte.
 *
 * Values referenced: Linux v7.3
 * Documentation/devicetree/bindings/display/simple-framebuffer.yaml (the
 * format strings).
 */
#define FDT_FB_FORMAT_NONE 0U
#define FDT_FB_FORMAT_BLUE_LOW 1U
#define FDT_FB_FORMAT_RED_LOW 2U

/* Set beside the format when its top byte is an alpha ("a8..."), not padding ("x8..."). */
#define FDT_FB_FORMAT_ALPHA 4U

/*
 * One open node of the tree: the cell counts it inherits and sets, what
 * kind of node its properties have shown it to be, and the cells of the
 * properties the HAL reads.  It lives on the parse's stack from its begin
 * token to its end token, where what it says is recorded.
 */
struct fdt_node {
	uint8_t parent_addr_cells;
	uint8_t parent_size_cells;
	uint8_t addr_cells;
	uint8_t size_cells;
	uint8_t is_memory;
	uint8_t is_board;
	uint8_t is_uart;
	uint8_t is_gic;
	uint8_t is_framebuffer;
	uint8_t is_chosen;
	uint8_t is_cpu;
	uint8_t spin_table;
	uint8_t psci;

	/* Nonzero when the node's status names it off. */
	uint8_t is_disabled;

	/* A simple-framebuffer's pixel format (FDT_FB_FORMAT_*). */
	uint8_t fb_format;

	/*
	 * The /reserved-memory node, and one of its children, whose reg is
	 * memory the firmware keeps (the secure monitor, the bootloader's
	 * configuration).
	 */
	uint8_t is_reserved_memory;
	uint8_t is_reserved_child;

	/* Nonzero when a /reserved-memory child carries no-map. */
	uint8_t is_no_map;

	uint64_t release_addr;
	uint32_t reg[FDT_MAX_CELLS];
	uint32_t reg_count;
	uint32_t ranges[FDT_MAX_CELLS];
	uint32_t ranges_count;
	uint32_t interrupts[FDT_MAX_INTERRUPT_CELLS];
	uint32_t interrupt_count;

	/*
	 * A memory node's linux,usable-memory: the part of its RAM the
	 * system may use, which wins over reg when it is given.
	 */
	uint32_t usable[FDT_MAX_CELLS];
	uint32_t usable_count;

	/* A simple-framebuffer's size in pixels and its row length in bytes. */
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_stride;
};

/*
 * Where the parse of the structure block stands: the blob, the strings,
 * the nodes open from the root down, and what has been found so far.
 */
struct fdt_parse {
	const uint8_t *data;
	const uint8_t *strings;
	uint32_t size_strings;
	const uint8_t *cursor;
	const uint8_t *end;
	int depth;
	struct fdt_node stack[FDT_MAX_DEPTH];
	struct rpi_fdt_info *info;
};

static uint32_t read_be32(const uint8_t *p);
static uint64_t read_be64(const uint8_t *p);
static int bounded_string(const uint8_t *p, const uint8_t *end, size_t *length);
static int string_equal(const uint8_t *s, size_t n, const char *literal);
static int compatible_has(const uint8_t *p, size_t length, const char *wanted);
static int cells_to_u64(const uint32_t *cells, unsigned count, uint64_t *value);
static int translate(const struct fdt_node *parent, uint64_t address, uint64_t *translated);
static int add_range(struct rpi_fdt_range *ranges, unsigned *count, unsigned maximum, uint64_t base, uint64_t size);
static uint32_t decode_irq(const uint32_t *cells, unsigned count);
static int commit_node(struct fdt_node *node, const struct fdt_node *parent, struct rpi_fdt_info *info);
static int commit_memory(const struct fdt_node *node, struct rpi_fdt_info *info);
static int commit_framebuffer(const struct fdt_node *node, const struct fdt_node *parent, struct rpi_fdt_info *info);
static int commit_device(const struct fdt_node *node, const struct fdt_node *parent, struct rpi_fdt_info *info);
static int commit_cpu(const struct fdt_node *node, struct rpi_fdt_info *info);
static int commit_reserved(const struct fdt_node *node, struct rpi_fdt_info *info);
static int read_reserve_map(const uint8_t *data, uint32_t total, uint32_t off_reserve, struct rpi_fdt_info *info);
static int begin_node(struct fdt_parse *parse);
static int read_property(struct fdt_parse *parse);
static void record_property(struct fdt_node *node, const uint8_t *name, size_t name_length, const uint8_t *value, uint32_t length);
static int record_cells(struct fdt_node *node, const uint8_t *name, size_t name_length, const uint8_t *value, uint32_t length);
static int end_node(struct fdt_parse *parse);

/*
 * Parses the firmware's flattened device tree into what the HAL needs.
 *
 * Every offset and length in the blob is checked against the bytes the
 * caller says are readable before it is used.
 */
int
rpi_fdt_parse(
	const void *blob,
	size_t available,
	struct rpi_fdt_info *info)
{
	struct fdt_parse parse;
	const uint8_t *data;
	uint32_t magic;
	uint32_t total;
	uint32_t off_struct;
	uint32_t off_strings;
	uint32_t off_reserve;
	uint32_t size_struct;
	uint32_t size_strings;
	uint32_t token;
	int error;

	/* Requires a blob, a record to fill, and a whole header. */
	if (blob == NULL ||
	    info == NULL ||
	    available < FDT_HEADER_BYTES)
		return FDT_E_HEADER;

	/* Requires the magic number. */
	data = blob;
	magic = read_be32(data);
	if (magic != FDT_MAGIC)
		return FDT_E_HEADER;

	/* Reads the header's sizes and offsets. */
	total = read_be32(data + 4);
	off_struct = read_be32(data + 8);
	off_strings = read_be32(data + 12);
	off_reserve = read_be32(data + 16);
	size_strings = read_be32(data + 32);
	size_struct = read_be32(data + 36);

	/* Refuses a blob larger than what is readable or than 2 MiB. */
	if (total < FDT_HEADER_BYTES ||
	    total > available ||
	    total > FDT_MAX_BLOB_BYTES)
		return FDT_E_BOUNDS;

	/* Refuses blocks that do not lie inside the blob. */
	if (off_struct > total || size_struct > total - off_struct)
		return FDT_E_BOUNDS;
	if (off_strings > total || size_strings > total - off_strings)
		return FDT_E_BOUNDS;
	if (off_reserve > total)
		return FDT_E_BOUNDS;

	/* Starts the answer and reads the reserved ranges. */
	hal_memset(info, 0, sizeof(*info));
	info->totalsize = total;
	error = read_reserve_map(data, total, off_reserve, info);
	if (error != FDT_OK)
		return error;

	/* Walks the structure block token by token. */
	parse.data = data;
	parse.strings = data + off_strings;
	parse.size_strings = size_strings;
	parse.cursor = data + off_struct;
	parse.end = parse.cursor + size_struct;
	parse.depth = -1;
	parse.info = info;
	while (parse.cursor + 4 <= parse.end) {
		token = read_be32(parse.cursor);
		parse.cursor += 4;

		/* Dispatches on the token. */
		switch (token) {
		case FDT_BEGIN_NODE:
			error = begin_node(&parse);
			break;
		case FDT_PROP:
			error = read_property(&parse);
			break;
		case FDT_END_NODE:
			error = end_node(&parse);
			break;
		case FDT_NOP:
			error = FDT_OK;
			break;
		case FDT_END:
			/* The end must close every node. */
			if (parse.depth != -1)
				return FDT_E_STRUCTURE;

			/* The tree must be an ODROID-C5's. */
			if (!info->compatible_board)
				return FDT_E_NOT_BOARD;

			/* Succeeded: the record is filled. */
			return FDT_OK;
		default:
			error = FDT_E_STRUCTURE;
			break;
		}

		/* Stops at the first malformed token. */
		if (error != FDT_OK)
			return error;
	}

	/* Failed: the structure block ended without its end token. */
	return FDT_E_STRUCTURE;
}

/*
 * Names a parse error for the boot log.
 */
const char *
rpi_fdt_error(
	int error)
{
	static const char *const names[] = {
		"ok",
		"header",
		"bounds",
		"structure",
		"depth",
		"cells",
		"overflow",
		"not-odroid-c5"
	};

	/* A number outside the table has no name. */
	if (error < 0 || (unsigned)error >= sizeof(names) / sizeof(names[0]))
		return "unknown";

	/* Succeeded: the error's name. */
	return names[error];
}

/* Reads a big-endian 32-bit word. */
static uint32_t
read_be32(
	const uint8_t *p)
{
	uint32_t value;

	/* The most significant byte comes first. */
	value = (uint32_t)p[0] << 24;
	value |= (uint32_t)p[1] << 16;
	value |= (uint32_t)p[2] << 8;
	value |= p[3];

	/* Succeeded: the word. */
	return value;
}

/* Reads a big-endian 64-bit word. */
static uint64_t
read_be64(
	const uint8_t *p)
{
	uint64_t high;
	uint64_t low;

	/* The high half comes first. */
	high = read_be32(p);
	low = read_be32(p + 4);

	/* Succeeded: the word. */
	return (high << 32) | low;
}

/* Measures a string that must end before the given end. */
static int
bounded_string(
	const uint8_t *p,
	const uint8_t *end,
	size_t *length)
{
	const uint8_t *q;

	/* Looks for the terminator inside the bounds. */
	q = p;
	while (q < end && *q != 0)
		q++;

	/* A string that runs off the end is malformed. */
	if (q == end)
		return FDT_E_BOUNDS;

	/* Succeeded: the length without the terminator. */
	*length = (size_t)(q - p);
	return FDT_OK;
}

/* Tells whether n bytes are exactly a literal string. */
static int
string_equal(
	const uint8_t *s,
	size_t n,
	const char *literal)
{
	size_t index;

	/* Measures the literal. */
	index = 0;
	while (literal[index] != '\0')
		index++;

	/* Strings of different lengths differ. */
	if (index != n)
		return 0;

	/* Compares byte by byte. */
	for (index = 0; index < n; index++) {
		if (s[index] != (uint8_t)literal[index])
			return 0;
	}

	/* Succeeded: the strings are equal. */
	return 1;
}

/* Tells whether a compatible list (strings one after another) names a value. */
static int
compatible_has(
	const uint8_t *p,
	size_t length,
	const char *wanted)
{
	const uint8_t *end;
	size_t n;
	int error;
	int equal;

	/* Looks at each string of the list in turn. */
	end = p + length;
	while (p < end) {
		/* A string that runs off the list ends the search. */
		error = bounded_string(p, end, &n);
		if (error != FDT_OK)
			return 0;

		/* The wanted value is found. */
		equal = string_equal(p, n, wanted);
		if (equal)
			return 1;

		/* Steps over the string and its terminator. */
		p += n + 1;
	}

	/* The list does not name the value. */
	return 0;
}

/* Joins one or two cells into a 64-bit value. */
static int
cells_to_u64(
	const uint32_t *cells,
	unsigned count,
	uint64_t *value)
{
	uint64_t joined;
	unsigned index;

	/* More than two cells do not fit. */
	if (count > 2)
		return FDT_E_CELLS;

	/* The most significant cell comes first. */
	joined = 0;
	for (index = 0; index < count; index++)
		joined = (joined << 32) | cells[index];

	/* Succeeded: the value. */
	*value = joined;
	return FDT_OK;
}

/* Translates a child address to the parent's through the parent's ranges. */
static int
translate(
	const struct fdt_node *parent,
	uint64_t address,
	uint64_t *translated)
{
	uint64_t child;
	uint64_t physical;
	uint64_t size;
	unsigned stride;
	unsigned offset;
	int error;

	/* A parent without ranges shares its children's addresses. */
	if (parent->ranges_count == 0) {
		*translated = address;
		return FDT_OK;
	}

	/* A range is a child address, a parent address and a size. */
	stride = parent->addr_cells + parent->parent_addr_cells + parent->size_cells;
	if (stride == 0)
		return FDT_E_CELLS;

	/* Looks for the range that holds the address. */
	for (offset = 0; offset + stride <= parent->ranges_count; offset += stride) {
		/* The range's start in the child's addresses. */
		error = cells_to_u64(parent->ranges + offset, parent->addr_cells, &child);
		if (error != FDT_OK)
			return error;

		/* Its start in the parent's addresses. */
		error = cells_to_u64(parent->ranges + offset + parent->addr_cells,
				     parent->parent_addr_cells,
				     &physical);
		if (error != FDT_OK)
			return error;

		/* Its size. */
		error = cells_to_u64(parent->ranges + offset + parent->addr_cells + parent->parent_addr_cells,
				     parent->size_cells,
				     &size);
		if (error != FDT_OK)
			return error;

		/* A range that does not hold the address is skipped. */
		if (address < child || address - child >= size)
			continue;

		/* Refuses a translation past the top of the address space. */
		if (physical > ~(uint64_t)0 - (address - child))
			return FDT_E_OVERFLOW;

		/* Succeeded: the address in the parent's terms. */
		*translated = physical + address - child;
		return FDT_OK;
	}

	/* Failed: no range holds the address. */
	return FDT_E_BOUNDS;
}

/* Appends a range to a table; an empty range adds nothing. */
static int
add_range(
	struct rpi_fdt_range *ranges,
	unsigned *count,
	unsigned maximum,
	uint64_t base,
	uint64_t size)
{
	/* An empty range has nothing to record. */
	if (size == 0)
		return FDT_OK;

	/* Refuses a range past the top of the address space, or a full table. */
	if (base > ~(uint64_t)0 - size)
		return FDT_E_OVERFLOW;
	if (*count >= maximum)
		return FDT_E_BOUNDS;

	/* Records the range at the end of the table. */
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;

	/* Succeeded: the range is recorded. */
	return FDT_OK;
}

/*
 * Turns a GIC interrupt specifier into the GIC's interrupt number: a
 * shared line (type 0) counts from 32 and a private one (type 1) from 16.
 * Zero when the specifier is too short.
 */
static uint32_t
decode_irq(
	const uint32_t *cells,
	unsigned count)
{
	/* A specifier needs the type and the number. */
	if (count < 2)
		return 0;

	/* A shared line. */
	if (cells[0] == 0)
		return cells[1] + 32;

	/* A private line. */
	if (cells[0] == 1)
		return cells[1] + 16;

	/* Succeeded: any other type is taken as the number itself. */
	return cells[1];
}

/* Records what a closed node says, once all its properties are read. */
static int
commit_node(
	struct fdt_node *node,
	const struct fdt_node *parent,
	struct rpi_fdt_info *info)
{
	int error;

	/* The root (or any node) naming the board marks the tree an ODROID-C5's. */
	if (node->is_board)
		info->compatible_board = 1;

	/* A block the board leaves off, or memory nobody keeps, says nothing more. */
	if (node->is_disabled)
		return FDT_OK;

	/* A CPU of /cpus. */
	if (node->is_cpu) {
		error = commit_cpu(node, info);
		if (error != FDT_OK)
			return error;
	}

	/* A memory node's ranges. */
	if (node->is_memory) {
		error = commit_memory(node, info);
		if (error != FDT_OK)
			return error;
	}

	/* Memory the firmware keeps for itself. */
	if (node->is_reserved_child) {
		error = commit_reserved(node, info);
		if (error != FDT_OK)
			return error;
	}

	/* The bootloader's framebuffer. */
	if (node->is_framebuffer) {
		error = commit_framebuffer(node, parent, info);
		if (error != FDT_OK)
			return error;
	}

	/* A peripheral the HAL uses. */
	error = commit_device(node, parent, info);
	if (error != FDT_OK)
		return error;

	/* Succeeded: the node is recorded. */
	return FDT_OK;
}

/* Records every range of a memory node's reg. */
static int
commit_memory(
	const struct fdt_node *node,
	struct rpi_fdt_info *info)
{
	const uint32_t *cells;
	uint32_t cell_count;
	unsigned stride;
	unsigned offset;
	uint64_t base;
	uint64_t size;
	int error;

	/* A node whose parent gives no cells has no ranges to read. */
	stride = node->parent_addr_cells + node->parent_size_cells;
	if (stride == 0)
		return FDT_OK;

	/*
	 * The usable part, when the tree gives one, is what the system may
	 * use (the vendor's tree limits its RAM that way); reg otherwise.
	 */
	cells = node->reg;
	cell_count = node->reg_count;
	if (node->usable_count != 0) {
		cells = node->usable;
		cell_count = node->usable_count;
	}

	/* Reads each base and size pair. */
	for (offset = 0; offset + stride <= cell_count; offset += stride) {
		/* The base of the range. */
		error = cells_to_u64(cells + offset, node->parent_addr_cells, &base);
		if (error != FDT_OK)
			return error;

		/* Its size. */
		error = cells_to_u64(cells + offset + node->parent_addr_cells,
				     node->parent_size_cells,
				     &size);
		if (error != FDT_OK)
			return error;

		/* Gives the range to the allocator. */
		error = add_range(info->memory, &info->memory_count, RPI_FDT_MAX_MEMORY, base, size);
		if (error != FDT_OK)
			return error;
	}

	/* Succeeded: every range is recorded. */
	return FDT_OK;
}

/*
 * Records the base, and the interrupt where the HAL needs it, of the
 * first enabled UART and GIC the tree lists.
 */
static int
commit_device(
	const struct fdt_node *node,
	const struct fdt_node *parent,
	struct rpi_fdt_info *info)
{
	unsigned stride;
	uint64_t base;
	uint64_t size;
	int wanted;
	int error;

	/* Only the peripherals the HAL maps itself. */
	wanted = 0;
	if (node->is_uart) {
		wanted = 1;
	} else if (node->is_gic) {
		wanted = 1;
	}
	if (!wanted)
		return FDT_OK;

	/* Requires one whole base and size in reg. */
	stride = node->parent_addr_cells + node->parent_size_cells;
	if (stride == 0 || node->reg_count < stride)
		return FDT_OK;

	/* Reads the first range's base. */
	error = cells_to_u64(node->reg, node->parent_addr_cells, &base);
	if (error != FDT_OK)
		return error;

	/* Reads its size, which must not be empty. */
	error = cells_to_u64(node->reg + node->parent_addr_cells, node->parent_size_cells, &size);
	if (error != FDT_OK)
		return FDT_E_CELLS;
	if (size == 0)
		return FDT_E_CELLS;

	/* Translates the base to a CPU address. */
	error = translate(parent, base, &base);
	if (error != FDT_OK)
		return error;

	/* The first UART, with its interrupt. */
	if (node->is_uart && info->uart_base == 0) {
		info->uart_base = base;
		info->uart_irq = decode_irq(node->interrupts, node->interrupt_count);
	}

	/* The first GIC: the distributor, then the CPU interface from the second range. */
	if (node->is_gic && info->gic_dist_base == 0) {
		info->gic_dist_base = base;

		/* The second range is the CPU interface, in the parent's terms too. */
		if (node->reg_count >= stride * 2U) {
			error = cells_to_u64(node->reg + stride, node->parent_addr_cells, &base);
			if (error != FDT_OK)
				return error;
			error = translate(parent, base, &base);
			if (error != FDT_OK)
				return error;
			info->gic_cpu_base = base;
		}
	}

	/* Succeeded: the peripheral is recorded. */
	return FDT_OK;
}

/*
 * Records the first simple-framebuffer the tree names: the buffer the
 * bootloader left the display scanning out, with its size, row length
 * and pixel order.  One in a format the console cannot paint, or without
 * a whole description, is left out.
 */
static int
commit_framebuffer(
	const struct fdt_node *node,
	const struct fdt_node *parent,
	struct rpi_fdt_info *info)
{
	unsigned stride;
	uint64_t base;
	uint64_t size;
	int error;

	/* Only the first one, and only one the console can paint. */
	if (info->framebuffer_size != 0)
		return FDT_OK;
	if ((node->fb_format & ~FDT_FB_FORMAT_ALPHA) == FDT_FB_FORMAT_NONE)
		return FDT_OK;
	if (node->fb_width == 0 || node->fb_height == 0 || node->fb_stride == 0)
		return FDT_OK;

	/* Requires one whole base and size in reg. */
	stride = node->parent_addr_cells + node->parent_size_cells;
	if (stride == 0 || node->reg_count < stride)
		return FDT_OK;

	/* Reads the buffer's base. */
	error = cells_to_u64(node->reg, node->parent_addr_cells, &base);
	if (error != FDT_OK)
		return error;

	/* Reads its size. */
	error = cells_to_u64(node->reg + node->parent_addr_cells, node->parent_size_cells, &size);
	if (error != FDT_OK)
		return error;

	/* Translates the base to a CPU address. */
	error = translate(parent, base, &base);
	if (error != FDT_OK)
		return error;

	/* Keeps the description; the buffer is memory, so it is also kept from the allocator later. */
	info->framebuffer_base = base;
	info->framebuffer_size = size;
	info->framebuffer_width = node->fb_width;
	info->framebuffer_height = node->fb_height;
	info->framebuffer_stride = node->fb_stride;
	info->framebuffer_red_low = 0;
	if ((node->fb_format & FDT_FB_FORMAT_RED_LOW) != 0)
		info->framebuffer_red_low = 1;
	info->framebuffer_alpha = 0;
	if ((node->fb_format & FDT_FB_FORMAT_ALPHA) != 0)
		info->framebuffer_alpha = 1;

	/* Succeeded: the framebuffer is recorded. */
	return FDT_OK;
}

/*
 * Records one cpu node: its affinity from reg, and how the firmware holds it.
 *
 * A tree with more CPUs than the table holds keeps the first ones; the
 * HAL brings up no more than four anyway.
 */
static int
commit_cpu(
	const struct fdt_node *node,
	struct rpi_fdt_info *info)
{
	struct rpi_fdt_cpu *cpu;
	uint64_t mpidr;
	int error;

	/* A cpu node without an address cannot be named to the interrupt controller. */
	if (node->parent_addr_cells == 0 || node->reg_count < node->parent_addr_cells)
		return FDT_E_CELLS;

	/* The reg cells are the CPU's affinity. */
	error = cells_to_u64(node->reg, node->parent_addr_cells, &mpidr);
	if (error != FDT_OK)
		return error;

	/* CPUs past the table are left out. */
	if (info->cpu_count >= RPI_FDT_MAX_CPUS)
		return FDT_OK;

	/* Appends the CPU. */
	cpu = &info->cpus[info->cpu_count];
	cpu->mpidr = mpidr;
	cpu->release_addr = node->release_addr;
	cpu->spin_table = node->spin_table;
	cpu->psci = node->psci;
	info->cpu_count++;

	/* Succeeded: the CPU is recorded. */
	return FDT_OK;
}

/*
 * Records every range of a /reserved-memory child's reg as reserved.
 *
 * A child without reg (a pool the firmware lets the system place) adds
 * nothing.
 */
static int
commit_reserved(
	const struct fdt_node *node,
	struct rpi_fdt_info *info)
{
	unsigned stride;
	unsigned offset;
	uint64_t base;
	uint64_t size;
	int error;

	/* A node whose parent gives no cells has no ranges to read. */
	stride = node->parent_addr_cells + node->parent_size_cells;
	if (stride == 0)
		return FDT_OK;

	/* Reads each base and size pair. */
	for (offset = 0; offset + stride <= node->reg_count; offset += stride) {
		/* The base of the range. */
		error = cells_to_u64(node->reg + offset, node->parent_addr_cells, &base);
		if (error != FDT_OK)
			return error;

		/* Its size. */
		error = cells_to_u64(node->reg + offset + node->parent_addr_cells,
				     node->parent_size_cells,
				     &size);
		if (error != FDT_OK)
			return error;

		/* Keeps the range out of the allocator. */
		error = add_range(info->reserved, &info->reserved_count, RPI_FDT_MAX_RESERVED, base, size);
		if (error != FDT_OK)
			return error;

		/* And out of the direct map, when the tree says it may not be mapped. */
		if (node->is_no_map) {
			error = add_range(info->no_map, &info->no_map_count, RPI_FDT_MAX_NO_MAP, base, size);
			if (error != FDT_OK)
				return error;
		}
	}

	/* Succeeded: every range is recorded. */
	return FDT_OK;
}

/* Reads the memory reservation block, up to its empty entry. */
static int
read_reserve_map(
	const uint8_t *data,
	uint32_t total,
	uint32_t off_reserve,
	struct rpi_fdt_info *info)
{
	uint32_t offset;
	uint64_t base;
	uint64_t size;
	int error;

	/* Reads each 16-byte base and size until the terminating zero pair. */
	for (offset = off_reserve; offset + 16U <= total; offset += 16U) {
		base = read_be64(data + offset);
		size = read_be64(data + offset + 8);

		/* The zero pair ends the block. */
		if (base == 0 && size == 0)
			break;

		/* Keeps the range out of the allocator. */
		error = add_range(info->reserved, &info->reserved_count, RPI_FDT_MAX_RESERVED, base, size);
		if (error != FDT_OK)
			return error;
	}

	/* Succeeded: every reservation is recorded. */
	return FDT_OK;
}

/*
 * Opens a node: it inherits its parent's cell counts until its own
 * properties say otherwise.
 */
static int
begin_node(
	struct fdt_parse *parse)
{
	struct fdt_node *node;
	struct fdt_node *parent;
	size_t length;
	int chosen;
	int reserved;
	int error;

	/* Refuses a tree deeper than the stack. */
	parse->depth++;
	if (parse->depth >= FDT_MAX_DEPTH)
		return FDT_E_DEPTH;

	/* Reads the node's name, which must end inside the block. */
	error = bounded_string(parse->cursor, parse->end, &length);
	if (error != FDT_OK)
		return error;

	/* Starts the node empty. */
	node = &parse->stack[parse->depth];
	hal_memset(node, 0, sizeof(*node));

	/* The root's parent has two address cells and one size cell; any other node inherits its parent's. */
	if (parse->depth == 0) {
		node->parent_addr_cells = 2;
		node->parent_size_cells = 1;
		node->addr_cells = 2;
		node->size_cells = 1;
	} else {
		parent = &parse->stack[parse->depth - 1];
		node->parent_addr_cells = parent->addr_cells;
		node->parent_size_cells = parent->size_cells;
		node->addr_cells = parent->addr_cells;
		node->size_cells = parent->size_cells;
	}

	/* The /chosen node carries the firmware's command line. */
	chosen = string_equal(parse->cursor, length, "chosen");
	if (parse->depth == 1 && chosen)
		node->is_chosen = 1;

	/* The /reserved-memory node. */
	if (parse->depth == 1) {
		reserved = string_equal(parse->cursor, length, "reserved-memory");
		node->is_reserved_memory = (uint8_t)reserved;
	}

	/* Each child of /reserved-memory. */
	if (parse->depth == 2 && parse->stack[1].is_reserved_memory)
		node->is_reserved_child = 1;

	/* Steps over the name and its padding. */
	parse->cursor += (length + 1 + 3) & ~(size_t)3;

	/* Succeeded: the node is open. */
	return FDT_OK;
}

/* Reads one property of the open node and records what it says. */
static int
read_property(
	struct fdt_parse *parse)
{
	struct fdt_node *node;
	const uint8_t *value;
	const uint8_t *name;
	size_t name_length;
	uint32_t length;
	uint32_t nameoff;
	int bootargs;
	int error;

	/* Requires an open node and a whole property header. */
	if (parse->depth < 0 || parse->cursor + 8 > parse->end)
		return FDT_E_STRUCTURE;
	length = read_be32(parse->cursor);
	nameoff = read_be32(parse->cursor + 4);
	parse->cursor += 8;

	/* Requires a value inside the block and a name inside the strings. */
	if (length > (uint32_t)(parse->end - parse->cursor) || nameoff >= parse->size_strings)
		return FDT_E_BOUNDS;
	value = parse->cursor;
	name = parse->strings + nameoff;
	error = bounded_string(name, parse->strings + parse->size_strings, &name_length);
	if (error != FDT_OK)
		return error;
	node = &parse->stack[parse->depth];

	/* Notes where the firmware's command line is. */
	bootargs = string_equal(name, name_length, "bootargs");
	if (node->is_chosen && bootargs) {
		parse->info->bootargs_offset = (uint32_t)(value - parse->data);
		parse->info->bootargs_length = length;
	}

	/* Records what the node's other properties say about it. */
	record_property(node, name, name_length, value, length);
	error = record_cells(node, name, name_length, value, length);
	if (error != FDT_OK)
		return error;

	/* Steps over the value and its padding. */
	parse->cursor += (length + 3U) & ~3U;

	/* Succeeded: the property is read. */
	return FDT_OK;
}

/* Records a property that names what kind of node this is. */
static void
record_property(
	struct fdt_node *node,
	const uint8_t *name,
	size_t name_length,
	const uint8_t *value,
	uint32_t length)
{
	int matches;
	int is_memory;
	int is_cpu;
	int is_okay;
	int is_ok;
	int blue_low;
	int red_low;
	int alpha_blue_low;
	int alpha_red_low;

	/* The node's own number of address cells, for its children. */
	matches = string_equal(name, name_length, "#address-cells");
	if (matches && length == 4) {
		node->addr_cells = (uint8_t)read_be32(value);
		return;
	}

	/* The node's own number of size cells, for its children. */
	matches = string_equal(name, name_length, "#size-cells");
	if (matches && length == 4) {
		node->size_cells = (uint8_t)read_be32(value);
		return;
	}

	/* A memory or cpu node by its device_type. */
	matches = string_equal(name, name_length, "device_type");
	if (matches) {
		/* "memory" with its terminator. */
		is_memory = 0;
		if (length >= 7)
			is_memory = string_equal(value, 6, "memory");
		if (is_memory) {
			node->is_memory = 1;
			return;
		}

		/* "cpu" with its terminator. */
		is_cpu = 0;
		if (length >= 4)
			is_cpu = string_equal(value, 3, "cpu");
		if (is_cpu) {
			node->is_cpu = 1;
			return;
		}
	}

	/* A CPU held in the firmware's spin table, or started through PSCI. */
	matches = string_equal(name, name_length, "enable-method");
	if (matches) {
		node->spin_table = (uint8_t)compatible_has(value, length, "spin-table");
		node->psci = (uint8_t)compatible_has(value, length, "psci");
		return;
	}

	/* Where a spin-table CPU waits, in one or two cells. */
	matches = string_equal(name, name_length, "cpu-release-addr");
	if (matches && length == 8) {
		node->release_addr = read_be64(value);
		return;
	}
	if (matches && length == 4) {
		node->release_addr = read_be32(value);
		return;
	}

	/* A reserved range the system may not map. */
	matches = string_equal(name, name_length, "no-map");
	if (matches) {
		node->is_no_map = 1;
		return;
	}

	/* A node is on unless its status says otherwise. */
	matches = string_equal(name, name_length, "status");
	if (matches) {
		is_okay = compatible_has(value, length, "okay");
		is_ok = compatible_has(value, length, "ok");
		node->is_disabled = 1;
		if (is_okay || is_ok)
			node->is_disabled = 0;
		return;
	}

	/*
	 * A simple-framebuffer's format: four bytes a pixel, blue in the low
	 * byte ("a8r8g8b8", "x8r8g8b8") or red ("a8b8g8r8", "x8b8g8r8").
	 *
	 * Values referenced: Linux v7.3
	 * Documentation/devicetree/bindings/display/simple-framebuffer.yaml
	 * (the format strings).
	 */
	matches = string_equal(name, name_length, "format");
	if (matches) {
		alpha_blue_low = compatible_has(value, length, "a8r8g8b8");
		blue_low = compatible_has(value, length, "x8r8g8b8");
		alpha_red_low = compatible_has(value, length, "a8b8g8r8");
		red_low = compatible_has(value, length, "x8b8g8r8");
		if (alpha_blue_low) {
			node->fb_format = FDT_FB_FORMAT_BLUE_LOW | FDT_FB_FORMAT_ALPHA;
		} else if (blue_low) {
			node->fb_format = FDT_FB_FORMAT_BLUE_LOW;
		} else if (alpha_red_low) {
			node->fb_format = FDT_FB_FORMAT_RED_LOW | FDT_FB_FORMAT_ALPHA;
		} else if (red_low) {
			node->fb_format = FDT_FB_FORMAT_RED_LOW;
		}
		return;
	}

	/* A simple-framebuffer's width, height and row length, one cell each. */
	matches = string_equal(name, name_length, "width");
	if (matches && length == 4) {
		node->fb_width = read_be32(value);
		return;
	}
	matches = string_equal(name, name_length, "height");
	if (matches && length == 4) {
		node->fb_height = read_be32(value);
		return;
	}
	matches = string_equal(name, name_length, "stride");
	if (matches && length == 4) {
		node->fb_stride = read_be32(value);
		return;
	}

	/*
	 * The board and the peripherals the HAL uses, by compatible string:
	 * the ODROID-C5 (Hardkernel's tree names it by Amlogic's reference
	 * board, an S7D), its UARTs, the GIC (a GIC-400, which the vendor's
	 * tree names by its older compatibles), and the bootloader's
	 * framebuffer.
	 *
	 * Values referenced: Hardkernel Linux odroids7d-6.12.y common_drivers
	 * arch/arm64/boot/dts/amlogic/s7d_s905x5m_odroidc5.dts and
	 * meson-s7d.dtsi (the compatible strings); Linux v7.3
	 * Documentation/devicetree/bindings/interrupt-controller/arm,gic.yaml.
	 */
	matches = string_equal(name, name_length, "compatible");
	if (matches) {
		node->is_board |= compatible_has(value, length, "hardkernel,odroid-c5");
		node->is_board |= compatible_has(value, length, "s7d_s905x5m_bm201");
		node->is_board |= compatible_has(value, length, "amlogic,s7d");
		node->is_uart |= compatible_has(value, length, "amlogic,meson-uart");
		node->is_uart |= compatible_has(value, length, "amlogic,meson-s4-uart");
		node->is_gic |= compatible_has(value, length, "arm,gic-400");
		node->is_gic |= compatible_has(value, length, "arm,cortex-a15-gic");
		node->is_framebuffer |= compatible_has(value, length, "simple-framebuffer");
	}
}

/* Copies reg, ranges or interrupts into the node's cells, up to their room. */
static int
record_cells(
	struct fdt_node *node,
	const uint8_t *name,
	size_t name_length,
	const uint8_t *value,
	uint32_t length)
{
	uint32_t *out;
	uint32_t *out_count;
	uint32_t count;
	unsigned maximum;
	unsigned index;
	int is_reg;
	int is_ranges;
	int is_interrupts;
	int is_usable;

	/* Picks where the cells go, or leaves any other property alone. */
	is_reg = string_equal(name, name_length, "reg");
	is_ranges = string_equal(name, name_length, "ranges");
	is_interrupts = string_equal(name, name_length, "interrupts");
	is_usable = string_equal(name, name_length, "linux,usable-memory");
	if (is_reg) {
		out = node->reg;
		out_count = &node->reg_count;
		maximum = FDT_MAX_CELLS;
	} else if (is_ranges) {
		out = node->ranges;
		out_count = &node->ranges_count;
		maximum = FDT_MAX_CELLS;
	} else if (is_interrupts) {
		out = node->interrupts;
		out_count = &node->interrupt_count;
		maximum = FDT_MAX_INTERRUPT_CELLS;
	} else if (is_usable) {
		out = node->usable;
		out_count = &node->usable_count;
		maximum = FDT_MAX_CELLS;
	} else {
		return FDT_OK;
	}

	/* Cells are whole 32-bit words. */
	if ((length & 3U) != 0)
		return FDT_E_CELLS;

	/* Copies as many cells as there is room for. */
	count = length / 4;
	if (count > maximum)
		count = maximum;
	for (index = 0; index < count; index++)
		out[index] = read_be32(value + index * 4);
	*out_count = count;

	/* Succeeded: the cells are recorded. */
	return FDT_OK;
}

/* Closes the open node and records what it said. */
static int
end_node(
	struct fdt_parse *parse)
{
	struct fdt_node *node;
	int error;

	/* Refuses an end with no node open. */
	if (parse->depth < 0)
		return FDT_E_STRUCTURE;

	/* The root names the board. */
	node = &parse->stack[parse->depth];
	if (node->is_board)
		parse->info->compatible_board = 1;

	/* Any other node is recorded against its parent. */
	if (parse->depth > 0) {
		error = commit_node(node, &parse->stack[parse->depth - 1], parse->info);
		if (error != FDT_OK)
			return error;
	}

	/* Pops the node. */
	parse->depth--;

	/* Succeeded: the node is closed. */
	return FDT_OK;
}
