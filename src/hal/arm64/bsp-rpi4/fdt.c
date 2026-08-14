#include <hal/hal.h>
#include "fdt.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE 2U
#define FDT_PROP 3U
#define FDT_NOP 4U
#define FDT_END 9U
#define FDT_MAX_DEPTH 24
#define FDT_MAX_CELLS 32

enum { FDT_OK, FDT_E_HEADER, FDT_E_BOUNDS, FDT_E_STRUCTURE,
	FDT_E_DEPTH, FDT_E_CELLS, FDT_E_OVERFLOW, FDT_E_NOT_RPI4 };

struct fdt_node {
	uint8 parent_addr_cells;
	uint8 parent_size_cells;
	uint8 addr_cells;
	uint8 size_cells;
	uint8 is_memory;
	uint8 is_rpi4;
	uint8 is_uart;
	uint8 is_mailbox;
	uint8 is_gic;
	uint8 is_sdhci;
	uint32 reg[FDT_MAX_CELLS];
	uint32 reg_count;
	uint32 ranges[FDT_MAX_CELLS];
	uint32 ranges_count;
	uint32 interrupts[12];
	uint32 interrupt_count;
};

static uint32
read_be32(const uint8 *p)
{
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
	    ((uint32)p[2] << 8) | p[3];
}

static uint64
read_be64(const uint8 *p)
{
	return ((uint64)read_be32(p) << 32) | read_be32(p + 4);
}

static int
bounded_string(const uint8 *p, const uint8 *end, size_t *length)
{
	const uint8 *q = p;
	while (q < end && *q != 0) q++;
	if (q == end) return FDT_E_BOUNDS;
	*length = (size_t)(q - p);
	return FDT_OK;
}

static int
string_equal(const uint8 *s, size_t n, const char *literal)
{
	size_t i = 0;
	while (literal[i] != '\0') i++;
	if (i != n) return 0;
	for (i = 0; i < n; i++) if (s[i] != (uint8)literal[i]) return 0;
	return 1;
}

static int
compatible_has(const uint8 *p, size_t length, const char *wanted)
{
	const uint8 *end = p + length;
	while (p < end) {
		size_t n;
		if (bounded_string(p, end, &n) != FDT_OK) return 0;
		if (string_equal(p, n, wanted)) return 1;
		p += n + 1;
	}
	return 0;
}

static int
cells_to_u64(const uint32 *cells, unsigned count, uint64 *value)
{
	uint64 v = 0;
	unsigned i;
	if (count > 2) return FDT_E_CELLS;
	for (i = 0; i < count; i++) v = (v << 32) | cells[i];
	*value = v;
	return FDT_OK;
}

static int
translate(const struct fdt_node *parent, uint64 address, uint64 *translated)
{
	unsigned stride, offset;
	if (parent->ranges_count == 0) {
		*translated = address;
		return FDT_OK;
	}
	stride = parent->addr_cells + parent->parent_addr_cells +
	    parent->size_cells;
	if (stride == 0) return FDT_E_CELLS;
	for (offset = 0; offset + stride <= parent->ranges_count; offset += stride) {
		uint64 child, physical, size;
		int error = cells_to_u64(parent->ranges + offset,
		    parent->addr_cells, &child);
		if (error != FDT_OK) return error;
		error = cells_to_u64(parent->ranges + offset + parent->addr_cells,
		    parent->parent_addr_cells, &physical);
		if (error != FDT_OK) return error;
		error = cells_to_u64(parent->ranges + offset + parent->addr_cells +
		    parent->parent_addr_cells, parent->size_cells, &size);
		if (error != FDT_OK) return error;
		if (address < child || address - child >= size) continue;
		if (physical > ~(uint64)0 - (address - child)) return FDT_E_OVERFLOW;
		*translated = physical + address - child;
		return FDT_OK;
	}
	return FDT_E_BOUNDS;
}

static int
add_range(struct rpi4_fdt_range *ranges, unsigned *count, unsigned maximum,
	uint64 base, uint64 size)
{
	if (size == 0) return FDT_OK;
	if (base > ~(uint64)0 - size) return FDT_E_OVERFLOW;
	if (*count >= maximum) return FDT_E_BOUNDS;
	ranges[*count].base = base;
	ranges[*count].size = size;
	(*count)++;
	return FDT_OK;
}

static uint32
decode_irq(const uint32 *cells, unsigned count)
{
	if (count < 2) return 0;
	if (cells[0] == 0) return cells[1] + 32;
	if (cells[0] == 1) return cells[1] + 16;
	return cells[1];
}

static int
commit_node(struct fdt_node *node, const struct fdt_node *parent,
	struct rpi4_fdt_info *info)
{
	unsigned stride = node->parent_addr_cells + node->parent_size_cells;
	unsigned offset;
	int error;
	if (node->is_rpi4) info->compatible_rpi4 = 1;
	if (node->is_memory && stride != 0) {
		for (offset = 0; offset + stride <= node->reg_count; offset += stride) {
			uint64 base, size;
			error = cells_to_u64(node->reg + offset,
			    node->parent_addr_cells, &base);
			if (error != FDT_OK) return error;
			error = cells_to_u64(node->reg + offset + node->parent_addr_cells,
			    node->parent_size_cells, &size);
			if (error != FDT_OK) return error;
			error = add_range(info->memory, &info->memory_count,
			    RPI4_FDT_MAX_MEMORY, base, size);
			if (error != FDT_OK) return error;
		}
	}
	if ((node->is_uart || node->is_mailbox || node->is_gic || node->is_sdhci) &&
	    stride != 0 && node->reg_count >= stride) {
		uint64 base, size;
		error = cells_to_u64(node->reg, node->parent_addr_cells, &base);
		if (error != FDT_OK) return error;
		error = cells_to_u64(node->reg + node->parent_addr_cells,
		    node->parent_size_cells, &size);
		if (error != FDT_OK || size == 0) return FDT_E_CELLS;
		error = translate(parent, base, &base);
		if (error != FDT_OK) return error;
		if (node->is_uart && info->uart_base == 0) info->uart_base = base;
		if (node->is_mailbox && info->mailbox_base == 0) info->mailbox_base = base;
		if (node->is_sdhci && info->sdhci_base == 0) {
			info->sdhci_base = base;
			info->sdhci_irq = decode_irq(node->interrupts, node->interrupt_count);
		}
		if (node->is_gic && info->gic_dist_base == 0) {
			info->gic_dist_base = base;
			if (node->reg_count >= stride * 2) {
				error = cells_to_u64(node->reg + stride,
				    node->parent_addr_cells, &base);
				if (error != FDT_OK) return error;
				error = translate(parent, base, &base);
				if (error != FDT_OK) return error;
				info->gic_cpu_base = base;
			}
		}
	}
	return FDT_OK;
}

int
rpi4_fdt_parse(const void *blob, size_t available, struct rpi4_fdt_info *info)
{
	const uint8 *data = blob, *structure, *strings, *p, *end;
	uint32 total, off_struct, off_strings, off_reserve, size_struct, size_strings;
	struct fdt_node stack[FDT_MAX_DEPTH];
	int depth = -1, error = FDT_OK;
	unsigned i;

	if (blob == NULL || info == NULL || available < 40) return FDT_E_HEADER;
	if (read_be32(data) != FDT_MAGIC) return FDT_E_HEADER;
	total = read_be32(data + 4);
	off_struct = read_be32(data + 8); off_strings = read_be32(data + 12);
	off_reserve = read_be32(data + 16); size_strings = read_be32(data + 32);
	size_struct = read_be32(data + 36);
	if (total < 40 || total > available || total > 2U * 1024U * 1024U)
		return FDT_E_BOUNDS;
	if (off_struct > total || size_struct > total - off_struct ||
	    off_strings > total || size_strings > total - off_strings ||
	    off_reserve > total) return FDT_E_BOUNDS;
	hal_memset(info, 0, sizeof(*info));
	info->totalsize = total;
	for (i = off_reserve; i + 16 <= total; i += 16) {
		uint64 base = read_be64(data + i), size = read_be64(data + i + 8);
		if (base == 0 && size == 0) break;
		error = add_range(info->reserved, &info->reserved_count,
		    RPI4_FDT_MAX_RESERVED, base, size);
		if (error != FDT_OK) return error;
	}
	structure = data + off_struct; strings = data + off_strings;
	p = structure; end = structure + size_struct;
	while (p + 4 <= end) {
		uint32 token = read_be32(p); p += 4;
		if (token == FDT_BEGIN_NODE) {
			size_t n;
			if (++depth >= FDT_MAX_DEPTH) return FDT_E_DEPTH;
			error = bounded_string(p, end, &n);
			if (error != FDT_OK) return error;
			hal_memset(&stack[depth], 0, sizeof(stack[depth]));
			if (depth == 0) {
				stack[depth].parent_addr_cells = 2;
				stack[depth].parent_size_cells = 1;
				stack[depth].addr_cells = 2;
				stack[depth].size_cells = 1;
			} else {
				stack[depth].parent_addr_cells = stack[depth - 1].addr_cells;
				stack[depth].parent_size_cells = stack[depth - 1].size_cells;
				stack[depth].addr_cells = stack[depth - 1].addr_cells;
				stack[depth].size_cells = stack[depth - 1].size_cells;
			}
			p += (n + 1 + 3) & ~(size_t)3;
		} else if (token == FDT_PROP) {
			uint32 length, nameoff, count;
			const uint8 *value, *name;
			size_t name_length;
			struct fdt_node *node;
			if (depth < 0 || p + 8 > end) return FDT_E_STRUCTURE;
			length = read_be32(p); nameoff = read_be32(p + 4); p += 8;
			if (length > (uint32)(end - p) || nameoff >= size_strings)
				return FDT_E_BOUNDS;
			value = p; name = strings + nameoff;
			error = bounded_string(name, strings + size_strings, &name_length);
			if (error != FDT_OK) return error;
			node = &stack[depth];
			if (string_equal(name, name_length, "#address-cells") && length == 4)
				node->addr_cells = (uint8)read_be32(value);
			else if (string_equal(name, name_length, "#size-cells") && length == 4)
				node->size_cells = (uint8)read_be32(value);
			else if (string_equal(name, name_length, "device_type") &&
			    length >= 7 && string_equal(value, 6, "memory")) node->is_memory = 1;
			else if (string_equal(name, name_length, "compatible")) {
				node->is_rpi4 |= compatible_has(value, length, "raspberrypi,4-model-b");
				node->is_rpi4 |= compatible_has(value, length, "brcm,bcm2711");
				node->is_uart |= compatible_has(value, length, "arm,pl011");
				node->is_mailbox |= compatible_has(value, length, "brcm,bcm2835-mbox");
				node->is_gic |= compatible_has(value, length, "arm,gic-400");
				node->is_sdhci |= compatible_has(value, length, "brcm,bcm2711-emmc2");
			} else if (string_equal(name, name_length, "reg") ||
			    string_equal(name, name_length, "ranges") ||
			    string_equal(name, name_length, "interrupts")) {
				uint32 *out; uint32 *out_count; unsigned maximum;
				if ((length & 3U) != 0) return FDT_E_CELLS;
				count = length / 4;
				if (string_equal(name, name_length, "reg")) { out = node->reg; out_count = &node->reg_count; maximum = FDT_MAX_CELLS; }
				else if (string_equal(name, name_length, "ranges")) { out = node->ranges; out_count = &node->ranges_count; maximum = FDT_MAX_CELLS; }
				else { out = node->interrupts; out_count = &node->interrupt_count; maximum = 12; }
				if (count > maximum) count = maximum;
				for (i = 0; i < count; i++) out[i] = read_be32(value + i * 4);
				*out_count = count;
			}
			p += (length + 3U) & ~3U;
		} else if (token == FDT_END_NODE) {
			if (depth < 0) return FDT_E_STRUCTURE;
			if (stack[depth].is_rpi4) info->compatible_rpi4 = 1;
			if (depth > 0) {
				error = commit_node(&stack[depth], &stack[depth - 1], info);
				if (error != FDT_OK) return error;
			}
			depth--;
		} else if (token == FDT_NOP) {
			continue;
		} else if (token == FDT_END) {
			if (depth != -1) return FDT_E_STRUCTURE;
			return info->compatible_rpi4 ? FDT_OK : FDT_E_NOT_RPI4;
		} else return FDT_E_STRUCTURE;
	}
	return FDT_E_STRUCTURE;
}

const char *
rpi4_fdt_error(int error)
{
	static const char *const names[] = { "ok", "header", "bounds", "structure",
	    "depth", "cells", "overflow", "not-rpi4" };
	return error >= 0 && (unsigned)error < sizeof(names) / sizeof(names[0]) ?
	    names[error] : "unknown";
}
