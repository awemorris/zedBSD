/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading the table of IP blocks an RDNA GPU keeps near the top of its
 * VRAM.
 *
 * The table names every hardware block of the chip with its version and
 * the register windows it answers in, and describes the shape of the
 * graphics engine.  The driver takes the chip's identity from it rather
 * than from the PCI device ID, and every register address from its
 * windows.  The bytes are checked (signatures, sums and bounds) before
 * anything is taken from them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * The table's top header: its signature, where its version words, its sum
 * and its length are, and the list of sub-tables with the size of one list
 * entry (offset, sum and length words).  The block list is entry 0, the
 * graphics shape entry 1.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/discovery.h (BINARY_SIGNATURE,
 * struct binary_header, struct table_info, enum table, IP_DISCOVERY, GC).
 */
#define TABLE_SIGNATURE			0x28211407U
#define TABLE_VERSION_MAJOR_AT		4U
#define TABLE_VERSION_MINOR_AT		6U
#define TABLE_SUM_AT			8U
#define TABLE_LENGTH_AT			10U
#define TABLE_LIST_AT			12U
#define TABLE_LIST_ENTRY_BYTES		8U
#define TABLE_LIST_ENTRY_SUM_AT		2U
#define TABLE_LIST_ENTRY_LENGTH_AT	4U
#define TABLE_LIST_BLOCKS		0U
#define TABLE_LIST_SHAPE		1U

/*
 * The block list's header: its signature, its layout version, its length,
 * the number of dies and the die entries (die number and offset), and the
 * flags byte a layout-4 list puts after the sixteen die entries, whose
 * bit 0 says the windows are 64-bit.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/discovery.h (DISCOVERY_TABLE_SIGNATURE,
 * struct ip_discovery_header, struct die_info, struct die_header).
 */
#define LIST_SIGNATURE			0x53445049U
#define LIST_LAYOUT_AT			4U
#define LIST_LENGTH_AT			6U
#define LIST_DIE_COUNT_AT		12U
#define LIST_DIE_ENTRIES_AT		14U
#define LIST_DIE_ENTRY_BYTES		4U
#define LIST_DIE_OFFSET_AT		2U
#define LIST_DIE_MAX			16U
#define LIST_FLAGS_AT			78U
#define LIST_FLAGS_WIDE_WINDOWS		0x01U
#define LIST_WIDE_LAYOUT		4U
#define DIE_BLOCK_COUNT_AT		2U
#define DIE_BLOCKS_AT			4U

/*
 * One block entry: the hardware ID, the instance, the number of windows,
 * the version bytes, and the windows after the 8-byte head.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/discovery.h (struct ip, struct ip_v3,
 * struct ip_v4).
 */
#define BLOCK_HARDWARE_AT		0U
#define BLOCK_INSTANCE_AT		2U
#define BLOCK_WINDOW_COUNT_AT		3U
#define BLOCK_MAJOR_AT			4U
#define BLOCK_MINOR_AT			5U
#define BLOCK_STEP_AT			6U
#define BLOCK_WINDOWS_AT		8U

/*
 * The graphics shape sub-table: its ID ("GC"), its length word, and the
 * dword fields of its first layout the driver reads: shader engines, the
 * two counts of compute-unit pairs per array, render back ends per engine,
 * level-2 cache channels, the wave width, waves per SIMD, scan converters
 * per engine, arrays per engine and packers per scan converter.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/discovery.h (GC_TABLE_ID,
 * struct gpu_info_header, struct gc_info_v1_0).
 */
#define SHAPE_ID			0x4347U
#define SHAPE_LENGTH_AT			8U
#define SHAPE_FIELDS_AT			12U
#define SHAPE_ENGINES			0U
#define SHAPE_PAIRS_FIRST		1U
#define SHAPE_PAIRS_SECOND		2U
#define SHAPE_BACKENDS			3U
#define SHAPE_CACHE_CHANNELS		4U
#define SHAPE_WAVE_LANES		11U
#define SHAPE_WAVES_PER_SIMD		12U
#define SHAPE_CONVERTERS		15U
#define SHAPE_ARRAYS			16U
#define SHAPE_PACKERS			17U
#define SHAPE_FIELD_COUNT		18U

/*
 * Which known block each hardware ID of the table names.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/soc15_hw_ip.h (GC_HWID, NBIF_HWID, MP0_HWID,
 * MP1_HWID, SDMA0_HWID, SDMA1_HWID, OSSSYS_HWID, HDP_HWID, MMHUB_HWID,
 * ATHUB_HWID, DMU_HWID, THM_HWID, SMUIO_HWID, UVD_HWID, DF_HWID,
 * UMC_HWID).
 */
static const struct block_hardware_id {
	uint16_t hardware;
	enum amdrdna_block block;
} block_hardware_ids[] = {
	{ 11U, AMDRDNA_BLOCK_GC },
	{ 108U, AMDRDNA_BLOCK_NBIO },
	{ 255U, AMDRDNA_BLOCK_PSP },
	{ 1U, AMDRDNA_BLOCK_SMU },
	{ 42U, AMDRDNA_BLOCK_SDMA0 },
	{ 43U, AMDRDNA_BLOCK_SDMA1 },
	{ 40U, AMDRDNA_BLOCK_IH },
	{ 41U, AMDRDNA_BLOCK_HDP },
	{ 34U, AMDRDNA_BLOCK_MMHUB },
	{ 35U, AMDRDNA_BLOCK_ATHUB },
	{ 271U, AMDRDNA_BLOCK_DCN },
	{ 3U, AMDRDNA_BLOCK_THM },
	{ 4U, AMDRDNA_BLOCK_SMUIO },
	{ 12U, AMDRDNA_BLOCK_VCN },
	{ 46U, AMDRDNA_BLOCK_DF },
	{ 150U, AMDRDNA_BLOCK_UMC }
};

/*
 * The names the log gives the known blocks, in the order of
 * enum amdrdna_block.
 */
static const char *const block_names[AMDRDNA_BLOCK_COUNT] = {
	"GC",
	"NBIO",
	"PSP",
	"SMU",
	"SDMA0",
	"SDMA1",
	"IH",
	"HDP",
	"MMHUB",
	"ATHUB",
	"DCN",
	"THM",
	"SMUIO",
	"VCN",
	"DF",
	"UMC"
};

static uint16_t byte_sum(const uint8_t *bytes, size_t length);
static int check_sub_table(const uint8_t *bytes, size_t length, unsigned entry, uint32_t *offset, uint32_t *declared);
static int read_blocks(const uint8_t *bytes, size_t length, uint32_t offset, struct amdrdna_block_table *table);
static int read_die(const uint8_t *bytes, size_t length, uint32_t at, bool wide, struct amdrdna_block_table *table);
static void read_shape(const uint8_t *bytes, uint32_t offset, struct amdrdna_shader_shape *shape);
static bool known_block(uint16_t hardware, enum amdrdna_block *block);

/*
 * Reads the GPU's table of IP blocks.
 *
 * Returns 0 with the table described, EINVAL for bytes that are not such a
 * table or whose sums or bounds are wrong, and ENOENT for a table that
 * lists no graphics block.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c (amdgpu_discovery_init,
 * amdgpu_discovery_reg_base_init, amdgpu_discovery_get_gfx_info).
 */
int
amdrdna_block_table_parse(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_block_table *table)
{
	uint32_t declared;
	uint32_t offset;
	uint32_t signature;
	uint32_t shape_length;
	uint16_t expected;
	uint16_t sum;
	uint16_t list_length;
	uint16_t shape_id;
	unsigned index;
	int error;

	/* Starts with nothing found. */
	table->layout_major = 0U;
	table->layout_minor = 0U;
	table->list_layout = 0U;
	table->listed = 0U;
	table->has_shape = false;
	for (index = 0U; index < AMDRDNA_BLOCK_COUNT; index++) {
		table->blocks[index].found = 0U;
		table->blocks[index].window_count = 0U;
	}

	/* A buffer too short for the top header and its list is no table. */
	if (length < TABLE_LIST_AT + 2U * TABLE_LIST_ENTRY_BYTES)
		return EINVAL;

	/* The signature says the bytes are a block table at all. */
	signature = amdrdna_le32(bytes, 0U);
	if (signature != TABLE_SIGNATURE)
		return EINVAL;

	/* The table's own length must fit in what was read. */
	declared = amdrdna_le16(bytes, TABLE_LENGTH_AT);
	if (declared > length)
		return EINVAL;
	if (declared <= TABLE_LENGTH_AT)
		return EINVAL;

	/* The sum of every byte after the sum word up to the table's length must match. */
	sum = byte_sum(bytes + TABLE_LENGTH_AT, declared - TABLE_LENGTH_AT);
	expected = amdrdna_le16(bytes, TABLE_SUM_AT);
	if (sum != expected)
		return EINVAL;

	/* Records the table's layout version. */
	table->layout_major = amdrdna_le16(bytes, TABLE_VERSION_MAJOR_AT);
	table->layout_minor = amdrdna_le16(bytes, TABLE_VERSION_MINOR_AT);

	/* Finds and checks the block list, which every table has. */
	error = check_sub_table(bytes, length, TABLE_LIST_BLOCKS, &offset, &declared);
	if (error != 0)
		return error;

	/* The block list's signature and its own length word. */
	signature = amdrdna_le32(bytes, offset);
	if (signature != LIST_SIGNATURE)
		return EINVAL;

	/* The list's own length word must agree with the top list. */
	list_length = amdrdna_le16(bytes, offset + LIST_LENGTH_AT);
	if (list_length != declared)
		return EINVAL;

	/* The list's header, with all its die entries and the flags byte, must fit in the list. */
	if (declared <= LIST_FLAGS_AT)
		return EINVAL;

	/* Walks the dies and their blocks. */
	error = read_blocks(bytes, length, offset, table);
	if (error != 0)
		return error;

	/* A table without the graphics block names no chip the driver can drive. */
	if (table->blocks[AMDRDNA_BLOCK_GC].found == 0U)
		return ENOENT;

	/* The graphics shape is optional; a damaged or short one is ignored rather than refused. */
	error = check_sub_table(bytes, length, TABLE_LIST_SHAPE, &offset, &declared);
	if (error != 0)
		return 0;
	if (declared < SHAPE_FIELDS_AT + SHAPE_FIELD_COUNT * 4U)
		return 0;

	/* Takes the shape only when its ID and its length word agree. */
	shape_id = amdrdna_le16(bytes, offset);
	shape_length = amdrdna_le32(bytes, offset + SHAPE_LENGTH_AT);
	if (shape_id != SHAPE_ID)
		return 0;
	if (shape_length != declared)
		return 0;

	/* Records the shape. */
	read_shape(bytes, offset, &table->shape);
	table->has_shape = true;

	/* Succeeded: the blocks the driver knows are described. */
	return 0;
}

/*
 * Names a known block for the log.
 */
const char *
amdrdna_block_name(
	enum amdrdna_block block)
{
	/* A value outside the enumeration has no name. */
	if ((unsigned)block >= AMDRDNA_BLOCK_COUNT)
		return "?";

	/* Succeeded: the block's short name. */
	return block_names[block];
}

/* Adds bytes into a 16-bit sum, the way the table's sums are made. */
static uint16_t
byte_sum(
	const uint8_t *bytes,
	size_t length)
{
	uint16_t sum;
	size_t index;

	/* Adds every byte, letting the sum wrap at 16 bits. */
	sum = 0U;
	for (index = 0U; index < length; index++)
		sum = (uint16_t)(sum + bytes[index]);

	/* The wrapped sum. */
	return sum;
}

/* Finds one sub-table from the top list and checks its bounds and its sum. */
static int
check_sub_table(
	const uint8_t *bytes,
	size_t length,
	unsigned entry,
	uint32_t *offset,
	uint32_t *declared)
{
	size_t at;
	uint16_t sum;
	uint16_t expected;

	/* The list entry of the sub-table. */
	at = TABLE_LIST_AT + (size_t)entry * TABLE_LIST_ENTRY_BYTES;
	*offset = amdrdna_le16(bytes, at);
	*declared = amdrdna_le16(bytes, at + TABLE_LIST_ENTRY_LENGTH_AT);

	/* An offset of zero says the table has no such sub-table. */
	if (*offset == 0U)
		return ENOENT;

	/* The sub-table must lie inside what was read. */
	if (*declared == 0U)
		return EINVAL;
	if ((size_t)*offset + *declared > length)
		return EINVAL;

	/* Its bytes must add up to the list's sum. */
	sum = byte_sum(bytes + *offset, *declared);
	expected = amdrdna_le16(bytes, at + TABLE_LIST_ENTRY_SUM_AT);
	if (sum != expected)
		return EINVAL;

	/* Succeeded: the sub-table lies at offset and is declared bytes long. */
	return 0;
}

/* Walks every die of the block list. */
static int
read_blocks(
	const uint8_t *bytes,
	size_t length,
	uint32_t offset,
	struct amdrdna_block_table *table)
{
	unsigned die_count;
	unsigned die;
	uint32_t die_at;
	bool wide;
	int error;

	/* The list's layout decides whether its windows are 64-bit. */
	table->list_layout = amdrdna_le16(bytes, offset + LIST_LAYOUT_AT);
	wide = false;
	if (table->list_layout >= LIST_WIDE_LAYOUT) {
		/* Only a layout-4 list carries the flags byte. */
		if ((bytes[offset + LIST_FLAGS_AT] & LIST_FLAGS_WIDE_WINDOWS) != 0U)
			wide = true;
	}

	/* A list names one die at least and at most as many as it has entries for. */
	die_count = amdrdna_le16(bytes, offset + LIST_DIE_COUNT_AT);
	if (die_count == 0U || die_count > LIST_DIE_MAX)
		return EINVAL;

	/* Reads the blocks of every die. */
	for (die = 0U; die < die_count; die++) {
		die_at = amdrdna_le16(bytes, offset + LIST_DIE_ENTRIES_AT + die * LIST_DIE_ENTRY_BYTES + LIST_DIE_OFFSET_AT);
		error = read_die(bytes, length, die_at, wide, table);
		if (error != 0)
			return error;
	}

	/* Succeeded: every die was walked. */
	return 0;
}

/* Reads the blocks of one die, keeping the first instance of each known block. */
static int
read_die(
	const uint8_t *bytes,
	size_t length,
	uint32_t at,
	bool wide,
	struct amdrdna_block_table *table)
{
	struct amdrdna_ip *ip;
	enum amdrdna_block block;
	uint16_t hardware;
	unsigned block_count;
	unsigned index;
	unsigned window;
	unsigned window_count;
	size_t entry;
	size_t window_bytes;
	bool known;

	/* The die header must lie inside what was read. */
	if ((size_t)at + DIE_BLOCKS_AT > length)
		return EINVAL;

	/* Each window is a dword, or two on a list with 64-bit windows. */
	window_bytes = 4U;
	if (wide)
		window_bytes = 8U;

	/* Walks the die's block entries. */
	block_count = amdrdna_le16(bytes, at + DIE_BLOCK_COUNT_AT);
	entry = (size_t)at + DIE_BLOCKS_AT;
	for (index = 0U; index < block_count; index++) {
		/* The entry's head must lie inside what was read. */
		if (entry + BLOCK_WINDOWS_AT > length)
			return EINVAL;

		/* So must its windows. */
		window_count = bytes[entry + BLOCK_WINDOW_COUNT_AT];
		if (entry + BLOCK_WINDOWS_AT + window_count * window_bytes > length)
			return EINVAL;

		/* Keeps the first instance of a block the driver knows. */
		hardware = amdrdna_le16(bytes, entry + BLOCK_HARDWARE_AT);
		known = known_block(hardware, &block);
		ip = &table->blocks[block];
		if (known &&
		    ip->found == 0U &&
		    bytes[entry + BLOCK_INSTANCE_AT] == 0U) {
			ip->found = 1U;
			ip->major = bytes[entry + BLOCK_MAJOR_AT];
			ip->minor = bytes[entry + BLOCK_MINOR_AT];
			ip->step = bytes[entry + BLOCK_STEP_AT];

			/* Keeps as many windows as the record holds; a dword index fits in the low word. */
			ip->window_count = (uint8_t)window_count;
			if (window_count > AMDRDNA_IP_WINDOWS)
				ip->window_count = AMDRDNA_IP_WINDOWS;
			for (window = 0U; window < ip->window_count; window++)
				ip->window[window] = amdrdna_le32(bytes, entry + BLOCK_WINDOWS_AT + window * window_bytes);
		}

		/* Counts the entry and steps over it. */
		table->listed++;
		entry += BLOCK_WINDOWS_AT + window_count * window_bytes;
	}

	/* Succeeded: the die's blocks are taken. */
	return 0;
}

/* Copies the fields of the graphics shape the driver uses. */
static void
read_shape(
	const uint8_t *bytes,
	uint32_t offset,
	struct amdrdna_shader_shape *shape)
{
	size_t fields;

	/* The fields start after the sub-table's header, one dword each. */
	fields = (size_t)offset + SHAPE_FIELDS_AT;

	/* The engines, and the arrays in each. */
	shape->engines = amdrdna_le32(bytes, fields + SHAPE_ENGINES * 4U);
	shape->arrays_per_engine = amdrdna_le32(bytes, fields + SHAPE_ARRAYS * 4U);

	/* The compute-unit pairs of an array come in two counts that add up. */
	shape->pairs_per_array = amdrdna_le32(bytes, fields + SHAPE_PAIRS_FIRST * 4U);
	shape->pairs_per_array += amdrdna_le32(bytes, fields + SHAPE_PAIRS_SECOND * 4U);

	/* The render back ends and the level-2 cache channels. */
	shape->backends_per_engine = amdrdna_le32(bytes, fields + SHAPE_BACKENDS * 4U);
	shape->cache_channels = amdrdna_le32(bytes, fields + SHAPE_CACHE_CHANNELS * 4U);

	/* The wave width and the waves a SIMD keeps. */
	shape->wave_lanes = amdrdna_le32(bytes, fields + SHAPE_WAVE_LANES * 4U);
	shape->waves_per_simd = amdrdna_le32(bytes, fields + SHAPE_WAVES_PER_SIMD * 4U);

	/* The scan converters of an engine, and the packers of a converter. */
	shape->converters_per_engine = amdrdna_le32(bytes, fields + SHAPE_CONVERTERS * 4U);
	shape->packers_per_converter = amdrdna_le32(bytes, fields + SHAPE_PACKERS * 4U);
}

/* Reports which known block a hardware ID names. */
static bool
known_block(
	uint16_t hardware,
	enum amdrdna_block *block)
{
	unsigned index;

	/* Looks the ID up; the first block stands in for an unknown one. */
	*block = AMDRDNA_BLOCK_GC;
	for (index = 0U; index < sizeof(block_hardware_ids) / sizeof(block_hardware_ids[0]); index++) {
		/* Stops at the ID's block. */
		if (block_hardware_ids[index].hardware == hardware) {
			*block = block_hardware_ids[index].block;
			return true;
		}
	}

	/* The table lists a block the driver does not use. */
	return false;
}
