/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 loader's reading of /chosen's boot hart.
 *
 * The reader walks the tree's structure block once, from its first token to
 * its end token, and never leaves the block or the strings block the header
 * describes, so a damaged tree is refused rather than read past.
 */

#include "fdt-hartid.h"

/*
 * The flattened tree's header and the tokens of its structure block.
 *
 * Values referenced: Devicetree Specification v0.4, section 5.2 (the
 * header's magic, totalsize, off_dt_struct, off_dt_strings, version,
 * size_dt_strings and size_dt_struct words) and section 5.4.1 (the
 * FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP and FDT_END tokens).
 */
#define FDT_MAGIC			0xd00dfeedU
#define FDT_HEADER_BYTES		40U
#define FDT_HEADER_MAGIC		0U
#define FDT_HEADER_TOTALSIZE		4U
#define FDT_HEADER_OFF_STRUCT		8U
#define FDT_HEADER_OFF_STRINGS		12U
#define FDT_HEADER_VERSION		20U
#define FDT_HEADER_SIZE_STRINGS		32U
#define FDT_HEADER_SIZE_STRUCT		36U
#define FDT_VERSION_SIZED		17U
#define FDT_TOKEN_BEGIN_NODE		1U
#define FDT_TOKEN_END_NODE		2U
#define FDT_TOKEN_PROP			3U
#define FDT_TOKEN_NOP			4U
#define FDT_TOKEN_END			9U

/*
 * The node and property that name the boot hart, and the two lengths the
 * property may have.
 *
 * Values referenced: Linux v6.12 drivers/firmware/efi/libstub/riscv.c
 * (get_boot_hartid_from_fdt: the "/chosen" node, the "boot-hartid"
 * property, and its lengths of one 32-bit and one 64-bit cell).
 */
#define FDT_CHOSEN_NAME			"chosen"
#define FDT_HARTID_NAME			"boot-hartid"
#define FDT_HARTID_BYTES_32		4U
#define FDT_HARTID_BYTES_64		8U

/*
 * The depth of the root's children, where /chosen is, and of /chosen's own
 * properties, counted in begun and unended nodes.
 */
#define FDT_DEPTH_ROOT_CHILD		1U
#define FDT_DEPTH_CHOSEN_PROPERTY	2U

static uint32_t fdt_be32(const uint8_t *bytes);
static uint64_t fdt_be64(const uint8_t *bytes);
static int fdt_name_is(const uint8_t *bytes, uint32_t available, const char *name, uint32_t *length);
static uint64_t fdt_align4(uint64_t offset);

/*
 * Reads /chosen's boot-hartid from a flattened tree of size bytes.
 *
 * Procedure referenced: Devicetree Specification v0.4, section 5.4.2 (the
 * structure block is a run of tokens from the root's FDT_BEGIN_NODE to
 * FDT_END, each property's name an offset into the strings block).
 */
enum zbl_fdt_hartid_result
zbl_fdt_boot_hartid(
	const uint8_t *tree,
	uint32_t size,
	uint64_t *hart)
{
	const uint8_t *structure;
	const uint8_t *strings;
	uint32_t total;
	uint32_t struct_offset;
	uint32_t struct_size;
	uint32_t strings_offset;
	uint32_t strings_size;
	uint32_t magic;
	uint32_t version;
	uint32_t offset;
	uint32_t token;
	uint32_t depth;
	uint32_t name_length;
	uint32_t value_length;
	uint32_t name_offset;
	uint64_t value_end;
	uint64_t next;
	int in_chosen;
	int chosen_seen;
	int named;

	/* Refuses what is too short to hold a header. */
	if (tree == 0 || size < FDT_HEADER_BYTES)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Refuses what is not a tree. */
	magic = fdt_be32(tree + FDT_HEADER_MAGIC);
	if (magic != FDT_MAGIC)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Refuses a tree larger than the bytes given. */
	total = fdt_be32(tree + FDT_HEADER_TOTALSIZE);
	if (total < FDT_HEADER_BYTES || total > size)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Refuses a tree older than the version whose header sizes its blocks. */
	version = fdt_be32(tree + FDT_HEADER_VERSION);
	if (version < FDT_VERSION_SIZED)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Refuses a structure block that is misaligned or leaves the tree. */
	struct_offset = fdt_be32(tree + FDT_HEADER_OFF_STRUCT);
	struct_size = fdt_be32(tree + FDT_HEADER_SIZE_STRUCT);
	if ((struct_offset & 3U) != 0)
		return ZBL_FDT_HARTID_MALFORMED;
	if ((uint64_t)struct_offset + struct_size > total)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Refuses a strings block that leaves the tree. */
	strings_offset = fdt_be32(tree + FDT_HEADER_OFF_STRINGS);
	strings_size = fdt_be32(tree + FDT_HEADER_SIZE_STRINGS);
	if ((uint64_t)strings_offset + strings_size > total)
		return ZBL_FDT_HARTID_MALFORMED;

	/* Walks the structure block's tokens, keeping the depth and whether /chosen is open. */
	structure = tree + struct_offset;
	strings = tree + strings_offset;
	offset = 0;
	depth = 0;
	in_chosen = 0;
	chosen_seen = 0;
	for (;;) {
		/* Refuses a block that ends without its end token. */
		if (struct_size - offset < 4U)
			return ZBL_FDT_HARTID_MALFORMED;

		/* Takes the next token. */
		token = fdt_be32(structure + offset);
		offset += 4U;

		/* Follows the token's kind. */
		switch (token) {
		case FDT_TOKEN_BEGIN_NODE:
			/* Refuses a node whose name runs off the block. */
			named = fdt_name_is(structure + offset, struct_size - offset, FDT_CHOSEN_NAME, &name_length);
			if (named < 0)
				return ZBL_FDT_HARTID_MALFORMED;

			/* A child of the root named chosen is /chosen; its properties come next. */
			if (named > 0 && depth == FDT_DEPTH_ROOT_CHILD) {
				in_chosen = 1;
				chosen_seen = 1;
			}

			/* Steps past the name and its padding into the node. */
			depth++;
			next = fdt_align4((uint64_t)offset + name_length + 1U);
			if (next > struct_size)
				return ZBL_FDT_HARTID_MALFORMED;
			offset = (uint32_t)next;
			break;
		case FDT_TOKEN_END_NODE:
			/* Refuses an end with no node open. */
			if (depth == 0)
				return ZBL_FDT_HARTID_MALFORMED;

			/* Leaving the root's child closes /chosen if it was open. */
			depth--;
			if (depth == FDT_DEPTH_ROOT_CHILD)
				in_chosen = 0;
			break;
		case FDT_TOKEN_PROP:
			/* Refuses a property outside every node, or whose header runs off the block. */
			if (depth == 0)
				return ZBL_FDT_HARTID_MALFORMED;
			if (struct_size - offset < 8U)
				return ZBL_FDT_HARTID_MALFORMED;

			/* Refuses a value that runs off the block. */
			value_length = fdt_be32(structure + offset);
			name_offset = fdt_be32(structure + offset + 4U);
			value_end = (uint64_t)offset + 8U + value_length;
			if (value_end > struct_size)
				return ZBL_FDT_HARTID_MALFORMED;

			/* Reads boot-hartid when it is one of /chosen's own properties. */
			if (in_chosen && depth == FDT_DEPTH_CHOSEN_PROPERTY) {
				/* Refuses a name that lies outside the strings block. */
				if (name_offset >= strings_size)
					return ZBL_FDT_HARTID_MALFORMED;

				/* Compares the property's name, refusing one that runs off the strings block. */
				named = fdt_name_is(strings + name_offset, strings_size - name_offset, FDT_HARTID_NAME, &name_length);
				if (named < 0)
					return ZBL_FDT_HARTID_MALFORMED;

				/* Takes the id from a cell of either width, and refuses any other length. */
				if (named > 0) {
					if (value_length == FDT_HARTID_BYTES_32) {
						*hart = fdt_be32(structure + offset + 8U);
					} else if (value_length == FDT_HARTID_BYTES_64) {
						*hart = fdt_be64(structure + offset + 8U);
					} else {
						return ZBL_FDT_HARTID_BAD_LENGTH;
					}

					/* Succeeded: the id the firmware named. */
					return ZBL_FDT_HARTID_FOUND;
				}
			}

			/* Steps past the value and its padding. */
			next = fdt_align4(value_end);
			if (next > struct_size)
				return ZBL_FDT_HARTID_MALFORMED;
			offset = (uint32_t)next;
			break;
		case FDT_TOKEN_NOP:
			/* A placeholder carries nothing. */
			break;
		case FDT_TOKEN_END:
			/* Refuses an end inside an open node. */
			if (depth != 0)
				return ZBL_FDT_HARTID_MALFORMED;

			/* The whole tree was read without the property: says whether /chosen was there. */
			if (chosen_seen)
				return ZBL_FDT_HARTID_NO_PROPERTY;
			return ZBL_FDT_HARTID_NO_CHOSEN;
		default:
			/* Refuses a token the format does not have. */
			return ZBL_FDT_HARTID_MALFORMED;
		}
	}
}

/*
 * Names a reading's result for the console.
 *
 * The names are what the loader prints after "RV64 HART chosen".
 */
const char *
zbl_fdt_hartid_result_name(
	enum zbl_fdt_hartid_result result)
{
	/* Picks the result's name. */
	switch (result) {
	case ZBL_FDT_HARTID_FOUND:
		return "found";
	case ZBL_FDT_HARTID_NO_CHOSEN:
		return "no /chosen";
	case ZBL_FDT_HARTID_NO_PROPERTY:
		return "no boot-hartid";
	case ZBL_FDT_HARTID_BAD_LENGTH:
		return "boot-hartid of a bad length";
	case ZBL_FDT_HARTID_MALFORMED:
		return "malformed tree";
	}

	/* A value outside the enumeration has no name. */
	return "unknown";
}

/* Reads a big-endian 32-bit cell. */
static uint32_t
fdt_be32(
	const uint8_t *bytes)
{
	uint32_t number;

	/* Assembles the cell most significant byte first. */
	number = (uint32_t)bytes[0] << 24;
	number |= (uint32_t)bytes[1] << 16;
	number |= (uint32_t)bytes[2] << 8;
	number |= (uint32_t)bytes[3];

	/* Succeeded: the cell's value. */
	return number;
}

/* Reads a big-endian 64-bit value of two cells, which need not be 8-byte aligned. */
static uint64_t
fdt_be64(
	const uint8_t *bytes)
{
	uint64_t high;
	uint64_t low;

	/* Takes the high cell, then the low one. */
	high = fdt_be32(bytes);
	low = fdt_be32(bytes + 4U);

	/* Succeeded: the two cells joined. */
	return (high << 32) | low;
}

/*
 * Tells whether a NUL-terminated name within available bytes is the given
 * one, and reports its length without the NUL.
 *
 * Returns 1 for the name, 0 for another name, and -1 when no NUL lies
 * within the bytes.
 */
static int
fdt_name_is(
	const uint8_t *bytes,
	uint32_t available,
	const char *name,
	uint32_t *length)
{
	uint32_t index;
	int same;

	/* Measures the name up to its NUL, comparing it with the wanted one on the way. */
	same = 1;
	for (index = 0; index < available; index++) {
		/* The NUL ends the name. */
		if (bytes[index] == 0)
			break;

		/* A differing byte, or a name longer than the wanted one, is another name. */
		if (same && (uint8_t)name[index] != bytes[index])
			same = 0;
	}

	/* Refuses a name with no NUL within the bytes. */
	if (index == available)
		return -1;

	/*
	 * Succeeded: the name's length, and whether it is the wanted one, which
	 * it is when the wanted name ends where this one does.
	 */
	*length = index;
	if (same && name[index] == 0)
		return 1;
	return 0;
}

/* Rounds an offset up to the structure block's 4-byte alignment. */
static uint64_t
fdt_align4(
	uint64_t offset)
{
	/* The tokens are aligned to 4 bytes. */
	return (offset + 3U) & ~(uint64_t)3U;
}
