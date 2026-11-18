/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd firmware tables and sink descriptions
 * (display/xd-vbt.c, display/xd-edid.c).
 *
 * The VBTs and OpRegions are made up here from the layout table
 * (intel/xd-vbt-layout.h, intel/xd-opregion-layout.h) with the values of
 * the target laptop (an Alder Lake-P: eDP 1920x1080 on DDI A, HDMI on B,
 * DisplayPort on TC1 and TC2) and of one board each of Tiger Lake,
 * Rocket Lake and Alder Lake-S; no firmware's own bytes are used.  The
 * checks are:
 *
 *   - the outputs and the panel each platform's VBT gives, against the
 *     values the platform's numbering tables say (DVO port, DDC pin and
 *     AUX channel codes);
 *   - the VBT stage (display/vbt-shell.c), which puts the xd values in the
 *     older display code's types;
 *   - the defaults of a board without a VBT;
 *   - the refusal of damaged VBTs (sizes, offsets, block overruns,
 *     inconsistent panel tables);
 *   - where an OpRegion keeps its VBT, against the firmware check's reader;
 *   - SHA-256 against the FIPS 180-2 examples;
 *   - the EDID decode of the laptop's panel and of a made-up HDMI sink.
 */

#include "host-test.h"

#include "dp-fixture-latitude5330.h"

#include "../../display/internal.h"
#include "../../display/output.h"
#include "../../display/vbt-parse.h"
#include "../../display/xd-edid.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-vbt.h"
#include "../../display/xd.h"
#include "../../intel/xd-opregion-layout.h"
#include "../../intel/xd-vbt-layout.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The room a made-up VBT and OpRegion get. */
#define TEST_VBT_BYTES		6144U
#define TEST_OPREGION_BYTES	I915_XD_OPREGION_BYTES

/* Where the BDB of a made-up VBT starts, and the size of its header. */
#define TEST_BDB_OFFSET		I915_XD_VBT_HEADER_BYTES

/* The layout of one made-up panel data entry: the timing table (with its six unaccounted bytes), the DVO timing, the PnP id. */
#define TEST_FP_BYTES		32U
#define TEST_ENTRY_BYTES	66U
#define TEST_DVO_AT		38U
#define TEST_PNP_AT		56U

/* The made-up blocks' sizes (the BDB versions used here read all of each). */
#define TEST_FEATURES_BYTES	7U
#define TEST_DRIVER_BYTES	19U
#define TEST_EDP_BYTES		812U
#define TEST_OPTIONS_BYTES	28U
#define TEST_BACKLIGHT_BYTES	305U
#define TEST_POWER_BYTES	58U
#define TEST_CHILD_BYTES	39U

/* One output of a made-up VBT. */
struct test_child {
	uint16_t handle;
	uint16_t type;
	uint8_t dvo;
	uint8_t aux;
	uint8_t ddc;
	uint8_t flags;
	uint8_t flags2;
	uint8_t hdmi_bits;
	uint8_t boost_levels;
	uint8_t dp_rate;
};

/* The panel a made-up VBT describes. */
struct test_panel {
	uint32_t clock_khz;
	uint16_t hactive, hfront, hsync, hblank;
	uint16_t vactive, vfront, vsync, vblank;
	uint16_t width_mm, height_mm;
	int hpos, vpos;
	uint16_t pwm_hz;
	uint16_t min_word;
	uint8_t bpp_code;
	uint32_t rate_khz;
	uint8_t lanes_code;
	uint8_t levels;
	uint16_t pps[5];
	uint8_t vswing_nibble;
};

/* One made-up VBT: the BDB version, the panel's row, the blocks it has, the outputs and the panel. */
struct test_vbt {
	const char *name;
	uint16_t version;
	uint8_t panel_type;
	int pointers;
	int generic_dtd;
	int pnp_row;
	unsigned nchildren;
	struct test_child children[4];
	struct test_panel panel;
};

/*
 * The handles and device types of the made-up outputs.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (DEVICE_HANDLE_LFP1, DEVICE_HANDLE_EFP1..EFP3, DEVICE_TYPE_eDP,
 * DEVICE_TYPE_HDMI, DEVICE_TYPE_DP, DEVICE_TYPE_DP_DUAL_MODE).
 */
#define TEST_LFP1		0x0008U
#define TEST_EFP1		0x0004U
#define TEST_EFP2		0x0040U
#define TEST_EFP3		0x0020U
#define TEST_TYPE_EDP		0x78c6U
#define TEST_TYPE_HDMI		0x60d2U
#define TEST_TYPE_DP		0x68c6U
#define TEST_TYPE_DP_DUAL	0x60d6U

/* The target laptop's panel: 1920x1080 at 60 Hz, PWM at 200 Hz, 6 bpc, eDP HBR x2. */
#define TEST_PANEL_5330 { \
	140800U, 1920U, 48U, 32U, 160U, 1080U, 3U, 5U, 32U, 290U, 170U, 1, 0, \
	200U, 0x0a0aU, 0U, 270000U, 1U, 0x00U, { 2100U, 10U, 2000U, 500U, 5000U }, 0x0U }

/*
 * The made-up VBTs: the target laptop (Alder Lake-P) with its pointers
 * block and generic DTD, the same laptop asking for the EDID's panel,
 * and one board of Tiger Lake (pointers made up, the panel data block's
 * timing), Rocket Lake and Alder Lake-S, each with the outputs its VBT
 * names by its own numbering.
 */
static const struct test_vbt test_vbts[] = {
	{
		"Alder Lake-P", 250U, 2U, 1, 1, -1, 4U,
		{
			{ TEST_LFP1, TEST_TYPE_EDP, I915_XD_DVO_DP_A, I915_XD_VBT_AUX_A, 0U, 0x40U, 0U, 0U, 0U, 3U },
			{ TEST_EFP1, TEST_TYPE_HDMI, I915_XD_DVO_HDMI_B, 0U, 2U, 0x08U, 0U, 0x88U, 0x21U, 0U },
			{ TEST_EFP2, TEST_TYPE_DP_DUAL, I915_XD_DVO_DP_F, I915_XD_VBT_AUX_F, 3U, 0xc2U, 0x01U, 0x07U, 0U, 4U },
			{ TEST_EFP3, TEST_TYPE_DP, I915_XD_DVO_DP_G, I915_XD_VBT_AUX_G, 4U, 0x10U, 0x03U, 0U, 0U, 0U }
		},
		TEST_PANEL_5330
	},
	{
		"Alder Lake-P EDID", 236U, I915_XD_VBT_PANEL_TYPE_MATCH_EDID, 1, 0, 5, 1U,
		{
			{ TEST_LFP1, TEST_TYPE_EDP, I915_XD_DVO_DP_A, I915_XD_VBT_AUX_A, 0U, 0U, 0U, 0U, 0U, 0U }
		},
		TEST_PANEL_5330
	},
	{
		"Tiger Lake", 240U, 3U, 0, 0, -1, 4U,
		{
			{ TEST_LFP1, TEST_TYPE_EDP, I915_XD_DVO_DP_A, I915_XD_VBT_AUX_A, 0U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP1, TEST_TYPE_HDMI, I915_XD_DVO_HDMI_B, 0U, 2U, 0U, 0U, 0x03U, 0U, 0U },
			{ TEST_EFP2, TEST_TYPE_DP, I915_XD_DVO_DP_D, I915_XD_VBT_AUX_D, 4U, 0U, 0x01U, 0U, 0U, 2U },
			{ TEST_EFP3, TEST_TYPE_DP_DUAL, I915_XD_DVO_DP_I, I915_XD_VBT_AUX_I, 9U, 0U, 0x02U, 0U, 0U, 0U }
		},
		{ 148500U, 1920U, 88U, 44U, 280U, 1080U, 4U, 5U, 45U, 344U, 194U, 1, 1,
		  1000U, 0x0010U, 1U, 540000U, 3U, 0x11U, { 2000U, 20U, 20U, 500U, 6000U }, 0x1U }
	},
	{
		"Rocket Lake", 235U, 0U, 1, 1, -1, 4U,
		{
			{ TEST_LFP1, TEST_TYPE_EDP, I915_XD_DVO_DP_A, I915_XD_VBT_AUX_A, 0U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP1, TEST_TYPE_DP, I915_XD_DVO_DP_B, I915_XD_VBT_AUX_B, 2U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP2, TEST_TYPE_HDMI, I915_XD_DVO_HDMI_C, I915_XD_VBT_AUX_C, 3U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP3, TEST_TYPE_DP, I915_XD_DVO_DP_D, I915_XD_VBT_AUX_D, 4U, 0U, 0U, 0U, 0U, 0U }
		},
		{ 241500U, 2560U, 48U, 32U, 160U, 1440U, 3U, 5U, 41U, 597U, 336U, 1, 0,
		  300U, 0x0800U, 2U, 810000U, 3U, 0x00U, { 2000U, 50U, 50U, 500U, 5000U }, 0x0U }
	},
	{
		"Alder Lake-S", 245U, 1U, 1, 1, -1, 3U,
		{
			{ TEST_LFP1, TEST_TYPE_EDP, I915_XD_DVO_DP_A, I915_XD_VBT_AUX_A, 0U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP1, TEST_TYPE_HDMI, I915_XD_DVO_HDMI_B, I915_XD_VBT_AUX_B, 2U, 0U, 0U, 0U, 0U, 0U },
			{ TEST_EFP2, TEST_TYPE_DP, I915_XD_DVO_DP_E, I915_XD_VBT_AUX_E, 5U, 0U, 0U, 0U, 0U, 0U }
		},
		{ 138500U, 1920U, 48U, 32U, 160U, 1080U, 3U, 5U, 31U, 344U, 194U, 1, 0,
		  20000U, 0x00ffU, 1U, 270000U, 1U, 0x00U, { 2000U, 10U, 10U, 500U, 5000U }, 0x0U }
	}
};

/*
 * What each made-up output is expected to be read as: the DDI, AUX
 * channel and GMBUS pin the platform's tables give for the VBT's codes.
 * The expected values are worked out by hand from the Linux tables named
 * at the xd tables (display-tables-xd.h).
 */
struct test_expect {
	uint8_t ddi;
	uint8_t aux;
	uint8_t pin;
};

static const struct test_expect test_expects[][4] = {
	{ { 0U, 0U, 0U }, { 1U, I915_XD_VBT_NONE, 2U }, { 3U, 3U, 9U }, { 4U, 4U, 10U } },
	{ { 0U, 0U, 0U } },
	{ { 0U, 0U, 0U }, { 1U, I915_XD_VBT_NONE, 2U }, { 3U, 3U, 9U }, { 8U, 8U, 14U } },
	{ { 0U, 0U, 0U }, { 1U, 1U, 2U }, { 3U, 3U, 9U }, { 4U, 4U, 10U } },
	{ { 0U, 0U, 0U }, { 3U, 3U, 9U }, { 6U, 6U, 12U } }
};

/* The platform rows the made-up VBTs are read for (by PCI id). */
static const uint16_t test_devices[] = { 0x46a6U, 0x46a6U, 0x9a60U, 0x4c8aU, 0x4680U };

/* Where the last made-up VBT's panel data block starts, counted from its BDB. */
static size_t test_lfp_data;

/*
 * The physical memory the OpRegion reader is given: the OpRegion at
 * TEST_ASLS and one more region (the raw VBT) at test_raw_address.  The
 * maps that were not given back are counted.
 */
#define TEST_ASLS		0x7a000000U
#define TEST_RAW_BYTES		0x2400U
static uint8_t test_memory_opregion[TEST_OPREGION_BYTES];
static uint8_t test_memory_raw[TEST_RAW_BYTES];
static uint64_t test_raw_address;
static int test_maps_open;

/* The display the VBT stage works on. */
static struct i915_display test_display;

static void test_put8(uint8_t *bytes, size_t offset, unsigned value);
static void test_put16(uint8_t *bytes, size_t offset, unsigned value);
static void test_put32(uint8_t *bytes, size_t offset, uint32_t value);
static void test_put64(uint8_t *bytes, size_t offset, uint64_t value);
static size_t test_block(uint8_t *bytes, size_t *at, unsigned id, size_t size);
static size_t test_build(const struct test_vbt *spec, uint8_t *bytes);
static void test_dvo_timing(uint8_t *timing, const struct test_panel *panel);
static const struct i915_xd_platform *test_platform(uint16_t device);
static void test_vbt_outputs(unsigned index);
static void test_vbt_panel(unsigned index);
static void test_defaults(void);
static void test_damaged(void);
static void test_opregion(void);
static void test_opregion_read(void);
static void test_stage_shell(void);
static int test_memory_map(void *context, uint64_t address, size_t size, const volatile uint8_t **bytes);
static void test_memory_unmap(void *context, const volatile uint8_t *bytes, size_t size);
static void test_sha256(void);
static void test_edid(void);

/*
 * Runs every check of the firmware tables and sink descriptions and
 * reports.
 */
int
main(void)
{
	unsigned index;
	int failures;

	/* Gives the display its VBT state. */
	i915_host_check(drv_i915_vbt_world_create(&test_display) == 0, "VBT state");

	/* Reads each made-up VBT. */
	for (index = 0U; index < sizeof(test_vbts) / sizeof(test_vbts[0]); index++) {
		test_vbt_outputs(index);
		test_vbt_panel(index);
	}

	/* The defaults, the damaged VBTs, the OpRegion, the digest and the EDIDs. */
	test_defaults();
	test_damaged();
	test_opregion();
	test_opregion_read();
	test_stage_shell();
	test_sha256();
	test_edid();

	/* Reports the result. */
	drv_i915_vbt_world_destroy(&test_display);
	failures = i915_host_report("host_xd_vbt_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Writes one byte. */
static void
test_put8(
	uint8_t *bytes,
	size_t offset,
	unsigned value)
{
	bytes[offset] = (uint8_t)value;
}

/* Writes a little-endian 16-bit word. */
static void
test_put16(
	uint8_t *bytes,
	size_t offset,
	unsigned value)
{
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
}

/* Writes a little-endian 32-bit word. */
static void
test_put32(
	uint8_t *bytes,
	size_t offset,
	uint32_t value)
{
	test_put16(bytes, offset, value & 0xffffU);
	test_put16(bytes, offset + 2U, value >> 16);
}

/* Writes a little-endian 64-bit word. */
static void
test_put64(
	uint8_t *bytes,
	size_t offset,
	uint64_t value)
{
	test_put32(bytes, offset, (uint32_t)value);
	test_put32(bytes, offset + 4U, (uint32_t)(value >> 32));
}

/* Appends a block header at *at and reports where the block's data starts (from the BDB's start). */
static size_t
test_block(
	uint8_t *bytes,
	size_t *at,
	unsigned id,
	size_t size)
{
	size_t data;

	/* The header, then room for the data. */
	test_put8(bytes, TEST_BDB_OFFSET + *at, id);
	test_put16(bytes, TEST_BDB_OFFSET + *at + 1U, (unsigned)size);
	data = *at + I915_XD_BLOCK_HEADER_BYTES;
	*at = data + size;

	/* Succeeded: the data's offset from the BDB. */
	return data;
}

/* Lays out a DVO timing for a panel. */
static void
test_dvo_timing(
	uint8_t *timing,
	const struct test_panel *panel)
{
	test_put16(timing, I915_XD_DVO_CLOCK, panel->clock_khz / 10U);
	timing[I915_XD_DVO_HACTIVE_LO] = (uint8_t)panel->hactive;
	timing[I915_XD_DVO_HBLANK_LO] = (uint8_t)panel->hblank;
	timing[I915_XD_DVO_H_HI] = (uint8_t)(((panel->hactive >> 8) << 4) | (panel->hblank >> 8));
	timing[I915_XD_DVO_VACTIVE_LO] = (uint8_t)panel->vactive;
	timing[I915_XD_DVO_VBLANK_LO] = (uint8_t)panel->vblank;
	timing[I915_XD_DVO_V_HI] = (uint8_t)(((panel->vactive >> 8) << 4) | (panel->vblank >> 8));
	timing[I915_XD_DVO_HSYNC_OFF_LO] = (uint8_t)panel->hfront;
	timing[I915_XD_DVO_HSYNC_WIDTH_LO] = (uint8_t)panel->hsync;
	timing[I915_XD_DVO_VSYNC_LO] = (uint8_t)(((panel->vfront & 0xfU) << 4) | (panel->vsync & 0xfU));
	timing[I915_XD_DVO_SYNC_HI] = (uint8_t)(((panel->hfront >> 8) << 6) | ((panel->hsync >> 8) << 4) | ((panel->vfront >> 4) << 2) | (panel->vsync >> 4));
	timing[I915_XD_DVO_HIMAGE_LO] = (uint8_t)panel->width_mm;
	timing[I915_XD_DVO_VIMAGE_LO] = (uint8_t)panel->height_mm;
	timing[I915_XD_DVO_IMAGE_HI] = (uint8_t)(((panel->width_mm >> 8) << 4) | (panel->height_mm >> 8));
	timing[I915_XD_DVO_FLAGS] = (uint8_t)(0x18U | (panel->hpos ? I915_XD_DVO_HSYNC_POSITIVE : 0U) | (panel->vpos ? I915_XD_DVO_VSYNC_POSITIVE : 0U));
}

/* Makes up a VBT from its description and reports its size. */
static size_t
test_build(
	const struct test_vbt *spec,
	uint8_t *bytes)
{
	const struct test_panel *panel;
	const struct test_child *child;
	uint8_t *b;
	size_t at;
	size_t block;
	size_t data;
	size_t entry;
	unsigned index;
	unsigned row;

	/* An empty VBT and its panel. */
	memset(bytes, 0, TEST_VBT_BYTES);
	panel = &spec->panel;

	/* The BDB header; the blocks follow it. */
	b = bytes + TEST_BDB_OFFSET;
	memcpy(b + I915_XD_BDB_SIGNATURE, "BIOS_DATA_BLOCK ", 16U);
	test_put16(b, I915_XD_BDB_VERSION, spec->version);
	test_put16(b, I915_XD_BDB_HEADER_SIZE, I915_XD_BDB_HEADER_BYTES);
	at = I915_XD_BDB_HEADER_BYTES;

	/* General features: spread spectrum at the alternate frequency, AFC code 1. */
	block = test_block(bytes, &at, I915_XD_BLOCK_GENERAL_FEATURES, TEST_FEATURES_BYTES);
	b[block + I915_XD_VBT_FEATURES_BITS2] = I915_XD_VBT_FEATURES_SSC | I915_XD_VBT_FEATURES_SSC_ALTERNATE;
	b[block + I915_XD_VBT_FEATURES_BITS7] = 1U;

	/* General definitions: the outputs. */
	block = test_block(bytes, &at, I915_XD_BLOCK_GENERAL_DEFINITIONS, I915_XD_VBT_DEFS_CHILDREN + 4U * TEST_CHILD_BYTES);
	b[block + I915_XD_VBT_DEFS_CHILD_SIZE] = TEST_CHILD_BYTES;
	for (index = 0U; index < spec->nchildren; index++) {
		child = &spec->children[index];
		data = block + I915_XD_VBT_DEFS_CHILDREN + index * TEST_CHILD_BYTES;
		test_put16(b, data + I915_XD_VBT_CHILD_HANDLE, child->handle);
		test_put16(b, data + I915_XD_VBT_CHILD_DEVICE_TYPE, child->type);
		b[data + I915_XD_VBT_CHILD_HDMI_BITS] = child->hdmi_bits;
		b[data + I915_XD_VBT_CHILD_DVO_PORT] = child->dvo;
		b[data + I915_XD_VBT_CHILD_DDC_PIN] = child->ddc;
		b[data + I915_XD_VBT_CHILD_FLAGS] = child->flags;
		b[data + I915_XD_VBT_CHILD_AUX_CHANNEL] = child->aux;
		b[data + I915_XD_VBT_CHILD_FLAGS2] = child->flags2;
		b[data + I915_XD_VBT_CHILD_IBOOST_LEVELS] = child->boost_levels;
		b[data + I915_XD_VBT_CHILD_DP_RATE_BITS] = child->dp_rate;
	}

	/* Driver features: an internal panel, DRRS on. */
	block = test_block(bytes, &at, I915_XD_BLOCK_DRIVER_FEATURES, TEST_DRIVER_BYTES);
	test_put16(b, block + I915_XD_VBT_DRIVER_EXT_BITS, 1U << 11);
	test_put16(b, block + I915_XD_VBT_DRIVER_FLAGS, I915_XD_VBT_DRIVER_DRRS);

	/* eDP: the panel's row of each table. */
	block = test_block(bytes, &at, I915_XD_BLOCK_EDP, TEST_EDP_BYTES);
	row = spec->panel_type;
	if (row >= I915_XD_VBT_PANEL_TYPES)
		row = (unsigned)spec->pnp_row;
	for (index = 0U; index < 5U; index++)
		test_put16(b, block + I915_XD_VBT_EDP_PPS + row * I915_XD_VBT_EDP_PPS_STRIDE + index * 2U, panel->pps[index]);
	test_put32(b, block + I915_XD_VBT_EDP_COLOR_DEPTH, (0xffffffffU & ~(3U << (row * 2U))) | ((uint32_t)panel->bpp_code << (row * 2U)));
	b[block + I915_XD_VBT_EDP_LINK + row * 2U] = (uint8_t)((panel->lanes_code << 4) | 1U);
	b[block + I915_XD_VBT_EDP_LINK + row * 2U + 1U] = panel->levels;
	test_put64(b, block + I915_XD_VBT_EDP_VSWING_PREEMPH, (0x1111111111111111ULL & ~(0xfULL << (row * 4U))) | ((uint64_t)panel->vswing_nibble << (row * 4U)));
	test_put16(b, block + I915_XD_VBT_EDP_LINK_RATE_WORDS + row * 2U, panel->rate_khz / 20U);
	test_put16(b, block + I915_XD_VBT_EDP_MAX_RATE_WORDS + row * 2U, 540000U / 20U);

	/* Panel options: the panel's row, dithering off, seamless switching for the row. */
	block = test_block(bytes, &at, I915_XD_BLOCK_LFP_OPTIONS, TEST_OPTIONS_BYTES);
	b[block + I915_XD_VBT_LFP_PANEL_TYPE] = spec->panel_type;
	b[block + I915_XD_VBT_LFP_PANEL_TYPE2] = 0U;
	test_put32(b, block + I915_XD_VBT_LFP_DRRS_BITS, 2U << (row * 2U));

	/* Panel data and its pointers: sixteen entries, the row's timing and the EDID's id. */
	if (spec->pointers != 0)
		(void)test_block(bytes, &at, I915_XD_BLOCK_LFP_DATA_POINTERS, 148U);
	data = test_block(bytes, &at, I915_XD_BLOCK_LFP_DATA, 16U * TEST_ENTRY_BYTES + 16U * I915_XD_VBT_PANEL_NAME_BYTES);
	test_lfp_data = data;
	for (index = 0U; index < 16U; index++) {
		entry = data + index * TEST_ENTRY_BYTES;
		test_put16(b, entry + TEST_DVO_AT - 2U, 0xffffU);
		memcpy(b + entry + TEST_PNP_AT, "\x11\x22\x33\x44\x00\x00\x00\x00\x01\x02", I915_XD_VBT_PNP_BYTES);
		if (index == row)
			test_dvo_timing(b + entry + TEST_DVO_AT, panel);
		if ((int)index == spec->pnp_row)
			memcpy(b + entry + TEST_PNP_AT, i915_dp_fixture_edid + 8U, I915_XD_VBT_PNP_BYTES);
	}

	/* The table pointers of a VBT that has them. */
	if (spec->pointers != 0) {
		block = data - I915_XD_BLOCK_HEADER_BYTES - 148U;
		b[block + I915_XD_VBT_PTRS_ENTRIES] = 3U;
		for (index = 0U; index < 16U; index++) {
			entry = block + I915_XD_VBT_PTRS_TABLES + index * I915_XD_VBT_PTRS_STRIDE;
			test_put16(b, entry + I915_XD_VBT_PTRS_FP, (unsigned)(data + index * TEST_ENTRY_BYTES));
			b[entry + I915_XD_VBT_PTRS_FP + 2U] = TEST_FP_BYTES;
			test_put16(b, entry + I915_XD_VBT_PTRS_DVO, (unsigned)(data + index * TEST_ENTRY_BYTES + TEST_DVO_AT));
			b[entry + I915_XD_VBT_PTRS_DVO + 2U] = I915_XD_VBT_DVO_TIMING_BYTES;
			test_put16(b, entry + I915_XD_VBT_PTRS_PNP, (unsigned)(data + index * TEST_ENTRY_BYTES + TEST_PNP_AT));
			b[entry + I915_XD_VBT_PTRS_PNP + 2U] = I915_XD_VBT_PNP_BYTES;
		}

		/* The panel names' pointer. */
		test_put16(b, block + I915_XD_VBT_PTRS_PANEL_NAME, (unsigned)(data + 16U * TEST_ENTRY_BYTES));
		b[block + I915_XD_VBT_PTRS_PANEL_NAME + 2U] = I915_XD_VBT_PANEL_NAME_BYTES;
	}

	/* Backlight: a PWM backlight with the row's frequency, controller 1, the 16-bit minimum. */
	block = test_block(bytes, &at, I915_XD_BLOCK_LFP_BACKLIGHT, TEST_BACKLIGHT_BYTES);
	b[block + I915_XD_VBT_BL_ENTRY_SIZE] = I915_XD_VBT_BL_ENTRY_BYTES;
	b[block + I915_XD_VBT_BL_ENTRIES + row * 6U] = I915_XD_VBT_BL_TYPE_PWM;
	test_put16(b, block + I915_XD_VBT_BL_ENTRIES + row * 6U + I915_XD_VBT_BL_PWM_HZ, panel->pwm_hz);
	b[block + I915_XD_VBT_BL_ENTRIES + row * 6U + I915_XD_VBT_BL_OLD_MIN] = 7U;
	b[block + I915_XD_VBT_BL_CONTROL + row] = 0x12U;
	test_put16(b, block + I915_XD_VBT_BL_LEVELS + row * 4U, 0xffffU);
	test_put16(b, block + I915_XD_VBT_BL_MIN_LEVELS + row * 4U, panel->min_word);
	b[block + I915_XD_VBT_BL_PRECISION + row] = 16U;

	/* Power conservation: switching and VRR on for the row, HOBL off. */
	block = test_block(bytes, &at, I915_XD_BLOCK_LFP_POWER, TEST_POWER_BYTES);
	test_put16(b, block + I915_XD_VBT_POWER_DRRS, 1U << row);
	test_put16(b, block + I915_XD_VBT_POWER_VRR, 0xffffU);

	/* Generic DTD: the row's timing. */
	if (spec->generic_dtd != 0) {
		block = test_block(bytes, &at, I915_XD_BLOCK_GENERIC_DTD, 2U + 16U * I915_XD_VBT_GDTD_ENTRY_BYTES);
		test_put16(b, block + I915_XD_VBT_GDTD_SIZE, I915_XD_VBT_GDTD_ENTRY_BYTES);
		entry = block + I915_XD_VBT_GDTD_ENTRIES + row * I915_XD_VBT_GDTD_ENTRY_BYTES;
		test_put32(b, entry + I915_XD_VBT_GDTD_CLOCK, panel->clock_khz);
		test_put16(b, entry + I915_XD_VBT_GDTD_HACTIVE, panel->hactive);
		test_put16(b, entry + I915_XD_VBT_GDTD_HBLANK, panel->hblank);
		test_put16(b, entry + I915_XD_VBT_GDTD_HFRONT, panel->hfront);
		test_put16(b, entry + I915_XD_VBT_GDTD_HSYNC, panel->hsync);
		test_put16(b, entry + I915_XD_VBT_GDTD_VACTIVE, panel->vactive);
		test_put16(b, entry + I915_XD_VBT_GDTD_VBLANK, panel->vblank);
		test_put16(b, entry + I915_XD_VBT_GDTD_VFRONT, panel->vfront);
		test_put16(b, entry + I915_XD_VBT_GDTD_VSYNC, panel->vsync);
		test_put16(b, entry + I915_XD_VBT_GDTD_WIDTH_MM, panel->width_mm);
		test_put16(b, entry + I915_XD_VBT_GDTD_HEIGHT_MM, panel->height_mm);
		b[entry + I915_XD_VBT_GDTD_FLAGS] = (uint8_t)((panel->hpos ? I915_XD_VBT_GDTD_HSYNC_POSITIVE : 0U) | (panel->vpos ? I915_XD_VBT_GDTD_VSYNC_POSITIVE : 0U));
	}

	/* The BDB size and the VBT header. */
	test_put16(b, I915_XD_BDB_SIZE, (unsigned)at);
	memcpy(bytes + I915_XD_VBT_SIGNATURE, "$VBT ALDERLAKE      ", 20U);
	test_put16(bytes, I915_XD_VBT_VERSION, 100U);
	test_put16(bytes, I915_XD_VBT_HEADER_SIZE, I915_XD_VBT_HEADER_BYTES);
	test_put16(bytes, I915_XD_VBT_VBT_SIZE, (unsigned)(TEST_BDB_OFFSET + at));
	test_put32(bytes, I915_XD_VBT_BDB_OFFSET, TEST_BDB_OFFSET);

	/* Succeeded: the VBT's size. */
	return TEST_BDB_OFFSET + at;
}

/* Reports the platform row of a PCI id. */
static const struct i915_xd_platform *
test_platform(
	uint16_t device)
{
	struct i915_xd_choice choice;

	/* Chooses the description of the device. */
	(void)drv_i915_xd_platform_choose(device, 0U, 0, 0U, &choice);
	return choice.platform;
}

/* Checks the outputs one made-up VBT gives. */
static void
test_vbt_outputs(
	unsigned index)
{
	static uint8_t bytes[TEST_VBT_BYTES];
	const struct test_vbt *spec;
	const struct i915_xd_vbt_child *child;
	struct i915_xd_ports ports;
	struct i915_xd_vbt vbt;
	char what[160];
	size_t size;
	unsigned c;
	int error;

	/* The VBT of the row, built. */
	spec = &test_vbts[index];
	size = test_build(spec, bytes);
	error = drv_i915_xd_vbt_check(bytes, size);
	snprintf(what, sizeof(what), "%s: the made-up VBT checks", spec->name);
	i915_host_check(error == 0, what);
	error = drv_i915_xd_vbt_parse(test_platform(test_devices[index]), bytes, size, &vbt);
	snprintf(what, sizeof(what), "%s: parse, version %u, %u outputs", spec->name, spec->version, spec->nchildren);
	i915_host_check(error == 0 && vbt.found == 1 && vbt.version == spec->version && vbt.nchildren == spec->nchildren, what);
	snprintf(what, sizeof(what), "%s: general features (alternate SSC, AFC override from 249) and driver features", spec->name);
	i915_host_check(vbt.ssc == 1 && vbt.ssc_khz == 100000U && vbt.internal_panel == 1 &&
	    vbt.afc_override == (spec->version >= 249U) && vbt.afc_value == 0U, what);

	/* Each output on its DDI, with its AUX channel and GMBUS pin in the platform's numbering. */
	for (c = 0U; c < spec->nchildren; c++) {
		child = drv_i915_xd_vbt_child(&vbt, test_expects[index][c].ddi);
		snprintf(what, sizeof(what), "%s: output %u on DDI %u, AUX %u, GMBUS pin %u", spec->name, c,
		    test_expects[index][c].ddi, test_expects[index][c].aux, test_expects[index][c].pin);
		i915_host_check(child == &vbt.children[c] && child->aux_ch == test_expects[index][c].aux &&
		    child->gmbus_pin == test_expects[index][c].pin, what);
	}

	/* The first output is the panel. */
	snprintf(what, sizeof(what), "%s: output 0 is eDP", spec->name);
	i915_host_check(vbt.children[0].edp == 1 && vbt.children[0].dp == 1 && vbt.children[0].hdmi == 0, what);

	/* The port table: every output becomes a port on its DDI, in order, the panel's first. */
	drv_i915_xd_ports(test_platform(test_devices[index]), &vbt, 0xffffffffU, &ports);
	snprintf(what, sizeof(what), "%s: the port table has one port per output, the panel's first", spec->name);
	i915_host_check(ports.n == spec->nchildren && ports.skipped == 0U && ports.port[0].edp == 1 && ports.port[0].dp == 1 &&
	    ports.port[0].ddi == test_expects[index][0].ddi && ports.port[0].aux == test_expects[index][0].aux, what);
	for (c = 0U; c < ports.n; c++) {
		snprintf(what, sizeof(what), "%s: port %u on DDI %u", spec->name, c, test_expects[index][c].ddi);
		i915_host_check(ports.port[c].ddi == test_expects[index][c].ddi, what);
	}

	/* A DDI the caller does not drive is skipped, and so is a second output on one DDI. */
	drv_i915_xd_ports(test_platform(test_devices[index]), &vbt, ~(1U << test_expects[index][0].ddi), &ports);
	snprintf(what, sizeof(what), "%s: a DDI outside the mask is skipped", spec->name);
	i915_host_check(ports.n == spec->nchildren - 1U && ports.skipped == 1U && ports.port[0].edp == 0, what);
	if (spec->nchildren >= 2U) {
		vbt.children[1].ddi = vbt.children[0].ddi;
		drv_i915_xd_ports(test_platform(test_devices[index]), &vbt, 0xffffffffU, &ports);
		snprintf(what, sizeof(what), "%s: a second output on one DDI is skipped", spec->name);
		i915_host_check(ports.n == spec->nchildren - 1U && ports.skipped == 1U, what);
	}

	/* Without a description there are no ports. */
	drv_i915_xd_ports(NULL, &vbt, 0xffffffffU, &ports);
	snprintf(what, sizeof(what), "%s: a device without a description has no ports", spec->name);
	i915_host_check(ports.n == 0U, what);
}

/* Checks the panel one made-up VBT gives. */
static void
test_vbt_panel(
	unsigned index)
{
	static uint8_t bytes[TEST_VBT_BYTES];
	const struct test_vbt *spec;
	const struct test_panel *p;
	struct i915_xd_vbt vbt;
	struct i915_xd_vbt_panel panel;
	char what[160];
	unsigned row;
	size_t size;
	int error;
	unsigned want_bpp;

	/* The VBT of the row and its panel. */
	spec = &test_vbts[index];
	p = &spec->panel;
	size = test_build(spec, bytes);
	(void)drv_i915_xd_vbt_parse(test_platform(test_devices[index]), bytes, size, &vbt);
	error = drv_i915_xd_vbt_panel(&vbt, &vbt.children[0], i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), &panel);
	row = spec->panel_type;
	if (row >= 16U)
		row = (unsigned)spec->pnp_row;

	/* The row and the rule that chose it. */
	snprintf(what, sizeof(what), "%s: panel row %u", spec->name, row);
	i915_host_check(error == 0 && panel.panel_type == row &&
	    panel.type_source == (spec->panel_type >= 16U ? I915_XD_PANEL_TYPE_PNP : I915_XD_PANEL_TYPE_VBT), what);

	/* The mode, from the generic DTD or the panel data block. */
	snprintf(what, sizeof(what), "%s: mode %ux%u %u kHz, syncs and image", spec->name, p->hactive, p->vactive, p->clock_khz);
	i915_host_check(panel.has_mode == 1 && panel.mode.clock_khz == (int)p->clock_khz &&
	    panel.mode.hdisplay == p->hactive && panel.mode.hsync_start == p->hactive + p->hfront &&
	    panel.mode.hsync_end == p->hactive + p->hfront + p->hsync && panel.mode.htotal == p->hactive + p->hblank &&
	    panel.mode.vdisplay == p->vactive && panel.mode.vsync_start == p->vactive + p->vfront &&
	    panel.mode.vsync_end == p->vactive + p->vfront + p->vsync && panel.mode.vtotal == p->vactive + p->vblank &&
	    panel.mode.width_mm == p->width_mm && panel.mode.height_mm == p->height_mm &&
	    panel.mode.hsync_positive == p->hpos && panel.mode.vsync_positive == p->vpos, what);

	/* The backlight. */
	snprintf(what, sizeof(what), "%s: PWM backlight %u Hz, controller 1, minimum %u", spec->name, p->pwm_hz, p->min_word / 255U);
	i915_host_check(panel.backlight == 1 && panel.backlight_hz == p->pwm_hz && panel.backlight_type == 2U &&
	    panel.backlight_controller == 1U && panel.backlight_active_low == 0 &&
	    panel.backlight_min == (uint8_t)(p->min_word / 255U), what);

	/* The bits per pixel the VBT's code names: 18, 24 or 30. */
	want_bpp = 30U;
	if (p->bpp_code == 0U) {
		want_bpp = 18U;
	} else if (p->bpp_code == 1U) {
		want_bpp = 24U;
	}

	/* The eDP link and the power sequence. */
	snprintf(what, sizeof(what), "%s: eDP %u kHz, lanes code %u, swing levels 0x%02x, power sequence", spec->name, p->rate_khz, p->lanes_code, p->levels);
	i915_host_check(panel.edp_rate_khz == p->rate_khz && panel.edp_lanes == (p->lanes_code == 3U ? 4U : p->lanes_code + 1U) &&
	    panel.edp_preemphasis == (p->levels & 0xfU) && panel.edp_vswing == (p->levels >> 4) &&
	    panel.low_vswing == (p->vswing_nibble == 0U) && panel.edp_max_rate_khz == (spec->version >= 244U ? 540000U : 0U) &&
	    panel.bpp == want_bpp &&
	    panel.t1_t3 == p->pps[0] && panel.t8 == p->pps[1] && panel.t9 == p->pps[2] && panel.t10 == p->pps[3] &&
	    panel.t11_t12 == p->pps[4], what);

	/* The options: no dithering, seamless switching, VRR. */
	snprintf(what, sizeof(what), "%s: no dither, seamless DRRS, VRR, no HOBL", spec->name);
	i915_host_check(panel.dither == 0 && panel.drrs == I915_XD_DRRS_SEAMLESS && panel.vrr == 1 && panel.hobl == 0, what);
}

/* Checks the outputs of a board without a VBT on each platform. */
static void
test_defaults(void)
{
	static const struct {
		uint16_t device;
		unsigned count;
		uint8_t ddis[6];
	} expect[] = {
		{ 0x46a6U, 3U, { 0U, 1U, 2U } },
		{ 0x9a60U, 3U, { 0U, 1U, 2U } },
		{ 0x4c8aU, 6U, { 0U, 1U, 3U, 4U, I915_XD_VBT_NONE, I915_XD_VBT_NONE } },
		{ 0x4680U, 6U, { 0U, 3U, 4U, 5U, 6U, I915_XD_VBT_NONE } }
	};
	struct i915_xd_vbt vbt;
	char what[160];
	unsigned row;
	unsigned c;
	int same;

	/* Each device's defaults. */
	for (row = 0U; row < sizeof(expect) / sizeof(expect[0]); row++) {
		(void)drv_i915_xd_vbt_parse(test_platform(expect[row].device), NULL, 0U, &vbt);
		same = vbt.found == 0 && vbt.version == 155U && vbt.nchildren == expect[row].count;
		for (c = 0U; same && c < vbt.nchildren; c++) {
			if (vbt.children[c].ddi != expect[row].ddis[c])
				same = 0;
		}

		/* Checks the defaults of the device. */
		snprintf(what, sizeof(what), "defaults of 0x%04x: %u outputs on the expected DDIs", expect[row].device, expect[row].count);
		i915_host_check(same, what);
	}

	/* Alder Lake-P: eDP on A, DisplayPort and HDMI on B and C. */
	(void)drv_i915_xd_vbt_parse(test_platform(0x46a6U), NULL, 0U, &vbt);
	i915_host_check(vbt.children[0].edp == 1 && vbt.children[1].hdmi == 1 && vbt.children[1].dp == 1,
	    "defaults of Alder Lake-P: eDP on A, DP and HDMI on B and C");
}

/* Checks the refusal of damaged VBTs and the parts of a VBT a damage hides. */
static void
test_damaged(void)
{
	static uint8_t bytes[TEST_VBT_BYTES];
	static uint8_t copy[TEST_VBT_BYTES];
	struct i915_xd_vbt vbt;
	struct i915_xd_vbt_panel panel;
	const struct i915_xd_platform *platform;
	size_t size;
	size_t at;
	int error;

	/* The Alder Lake-P description and the first VBT. */
	platform = test_platform(0x46a6U);
	size = test_build(&test_vbts[0], bytes);

	/* A short buffer, a wrong signature, a VBT larger than its buffer. */
	i915_host_check(drv_i915_xd_vbt_check(bytes, I915_XD_VBT_HEADER_BYTES - 1U) == EINVAL, "damaged: a short header is refused");
	memcpy(copy, bytes, size);
	copy[1] = 'X';
	i915_host_check(drv_i915_xd_vbt_check(copy, size) == EINVAL, "damaged: a wrong signature is refused, as by the old parser");
	i915_host_check(drv_i915_xd_vbt_check(bytes, size - 1U) == EINVAL, "damaged: a VBT longer than its buffer is refused, as by the old parser");

	/* A BDB that starts or ends outside the VBT. */
	memcpy(copy, bytes, size);
	test_put32(copy, I915_XD_VBT_BDB_OFFSET, (uint32_t)size - 10U);
	i915_host_check(drv_i915_xd_vbt_check(copy, size) == EINVAL, "damaged: a BDB header past the VBT is refused");
	memcpy(copy, bytes, size);
	test_put32(copy, I915_XD_VBT_BDB_OFFSET, 0xfffffff0U);
	i915_host_check(drv_i915_xd_vbt_check(copy, size) == EINVAL, "damaged: a BDB offset that wraps is refused");
	memcpy(copy, bytes, size);
	test_put16(copy, TEST_BDB_OFFSET + I915_XD_BDB_SIZE, (unsigned)size);
	i915_host_check(drv_i915_xd_vbt_check(copy, size) == EINVAL, "damaged: a BDB running past the VBT is refused");

	/* A refused VBT gives the defaults and EINVAL. */
	error = drv_i915_xd_vbt_parse(platform, copy, size, &vbt);
	i915_host_check(error == EINVAL && vbt.found == 0 && vbt.nchildren == 3U, "damaged: a refused VBT gives the defaults");

	/* A block that runs past the BDB hides itself and the blocks after it (the general definitions here). */
	memcpy(copy, bytes, size);
	at = TEST_BDB_OFFSET + I915_XD_BDB_HEADER_BYTES;
	test_put16(copy, at + 1U, 0xfff0U);
	error = drv_i915_xd_vbt_parse(platform, copy, size, &vbt);
	i915_host_check(error == 0 && vbt.found == 1 && vbt.nchildren == 0U && vbt.ssc_khz == 120000U, "damaged: a block past the BDB hides it and the later blocks");

	/* Children smaller than the oldest layout give no outputs. */
	memcpy(copy, bytes, size);
	at = TEST_BDB_OFFSET + I915_XD_BDB_HEADER_BYTES + 3U + TEST_FEATURES_BYTES + 3U;
	copy[at + I915_XD_VBT_DEFS_CHILD_SIZE] = 32U;
	error = drv_i915_xd_vbt_parse(platform, copy, size, &vbt);
	i915_host_check(error == 0 && vbt.nchildren == 0U, "damaged: children of 32 bytes give no outputs");

	/* Panel tables without their terminator give no panel data mode (the generic DTD is the version's mode, so use an older VBT). */
	size = test_build(&test_vbts[1], bytes);
	memcpy(copy, bytes, size);
	(void)drv_i915_xd_vbt_parse(platform, copy, size, &vbt);
	(void)drv_i915_xd_vbt_panel(&vbt, &vbt.children[0], NULL, 0U, &panel);
	i915_host_check(panel.has_mode == 1 && panel.type_source == I915_XD_PANEL_TYPE_FIRST, "damaged: without the EDID the panel is the first row, with its mode");
	copy[TEST_BDB_OFFSET + test_lfp_data + 7U * TEST_ENTRY_BYTES + TEST_DVO_AT - 2U] = 0U;
	(void)drv_i915_xd_vbt_parse(platform, copy, size, &vbt);
	(void)drv_i915_xd_vbt_panel(&vbt, &vbt.children[0], i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), &panel);
	i915_host_check(panel.has_mode == 0 && panel.type_source == I915_XD_PANEL_TYPE_FIRST,
	    "damaged: panel tables without a terminator give no mode and no EDID match");

	/* A missing output has no panel. */
	i915_host_check(drv_i915_xd_vbt_panel(&vbt, NULL, NULL, 0U, &panel) == EINVAL, "damaged: no output, no panel");
}

/* Checks where an OpRegion keeps its VBT, against today's reader. */
static void
test_opregion(void)
{
	static uint8_t opregion[TEST_OPREGION_BYTES];
	static const struct {
		const char *what;
		uint8_t major;
		uint8_t minor;
		uint32_t mailboxes;
		uint64_t rvda;
		uint32_t rvds;
		uint64_t raw;
		uint32_t mailbox_size;
	} cases[] = {
		{ "2.0, mailbox 4 to the end", 2U, 0U, 0x07U, 0U, 0U, 0U, 0x1c00U },
		{ "2.0, ASLE extension: mailbox 4 to it", 2U, 0U, 0x17U, 0U, 0U, 0U, 0x1800U },
		{ "2.0 raw VBT at a physical address", 2U, 0U, 0x07U, 0x7f000000ULL, 0x2000U, 0x7f000000ULL, 0x1c00U },
		{ "2.1 raw VBT after the OpRegion", 2U, 1U, 0x07U, 0x2000U, 0x2400U, 0x7a002000ULL, 0x1c00U },
		{ "3.0 raw VBT after the OpRegion", 3U, 0U, 0x27U, 0x2000U, 0x2400U, 0x7a002000ULL, 0x1c00U },
		{ "2.1 without the ASLE mailbox: no raw VBT", 2U, 1U, 0x03U, 0x2000U, 0x2400U, 0U, 0x1c00U },
		{ "1.0: no raw VBT", 1U, 0U, 0x07U, 0x2000U, 0x2400U, 0U, 0x1c00U }
	};
	struct i915_xd_opregion_vbt where;
	struct i915_opregion_info old;
	char what[160];
	unsigned row;
	int error;
	int same;

	/* Each OpRegion case. */
	for (row = 0U; row < sizeof(cases) / sizeof(cases[0]); row++) {
		memset(opregion, 0, sizeof(opregion));
		memcpy(opregion, "IntelGraphicsMem", 16U);
		opregion[I915_XD_OPREGION_MAJOR] = cases[row].major;
		opregion[I915_XD_OPREGION_MINOR] = cases[row].minor;
		test_put32(opregion, I915_XD_OPREGION_MAILBOXES, cases[row].mailboxes);
		test_put64(opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDA, cases[row].rvda);
		test_put32(opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDS, cases[row].rvds);
		error = drv_i915_xd_opregion_find_vbt(opregion, sizeof(opregion), 0x7a000000ULL, &where);
		(void)drv_i915_opregion_locate_vbt(opregion, sizeof(opregion), 0x7a000000ULL, &old);
		same = error == 0 && where.raw_address == cases[row].raw && where.mailbox_offset == 0x400U &&
		    where.mailbox_size == cases[row].mailbox_size;
		if (cases[row].raw != 0U) {
			if (old.src != I915_OPVBT_RVDA ||
			    old.vbt_phys != where.raw_address ||
			    old.rvds != where.raw_size)
				same = 0;
		} else {
			if (old.src != I915_OPVBT_MAILBOX4 ||
			    old.vbt_offset != where.mailbox_offset ||
			    old.vbt_max != where.mailbox_size)
				same = 0;
		}

		/* Checks the row against today's reader. */
		snprintf(what, sizeof(what), "OpRegion %s, as today's reader", cases[row].what);
		i915_host_check(same, what);
	}

	/* A short copy and a wrong signature are refused. */
	i915_host_check(drv_i915_xd_opregion_find_vbt(opregion, sizeof(opregion) - 1U, 0U, &where) == EINVAL, "OpRegion: a short copy is refused");
	opregion[0] = 'i';
	i915_host_check(drv_i915_xd_opregion_find_vbt(opregion, sizeof(opregion), 0U, &where) == EINVAL, "OpRegion: a wrong signature is refused");
}

/* Checks SHA-256 against the examples of FIPS 180-2 (one block, two blocks, and the empty string). */
static void
test_sha256(void)
{
	static const uint8_t abc[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	static const uint8_t two[32] = {
		0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
		0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
	};
	static const uint8_t empty[32] = {
		0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
		0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
	};
	static const char message[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	uint8_t digest[32];
	uint8_t old_digest[32];

	/* The SHA-256 of a known string. */
	drv_i915_xd_sha256((const uint8_t *)"abc", 3U, digest);
	i915_host_check(memcmp(digest, abc, 32U) == 0, "SHA-256 of \"abc\"");
	drv_i915_xd_sha256((const uint8_t *)message, sizeof(message) - 1U, digest);
	i915_host_check(memcmp(digest, two, 32U) == 0, "SHA-256 of the two-block example");
	drv_i915_xd_sha256((const uint8_t *)"", 0U, digest);
	i915_host_check(memcmp(digest, empty, 32U) == 0, "SHA-256 of the empty string");
	drv_i915_xd_sha256(i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), digest);
	drv_i915_sha256(i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), old_digest);
	i915_host_check(memcmp(digest, old_digest, 32U) == 0, "SHA-256 of the panel EDID, as today's digest");
}

/* Checks the EDID decode: the laptop's panel, a made-up HDMI sink, and damaged blocks. */
static void
test_edid(void)
{
	static uint8_t hdmi[256];
	static uint8_t base[128];
	struct i915_display_mode first;
	unsigned index;
	struct i915_xd_edid edid;
	struct i915_display_mode mode;
	unsigned at;
	unsigned sum;
	int error;

	/* The laptop's panel: AUO, digital at 6 bpc, 29x17 cm, its first timing as the output code decodes it. */
	error = drv_i915_xd_edid_decode(i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), &edid);
	(void)drv_i915_output_edid_timing(i915_dp_fixture_edid + 54U, &mode);
	i915_host_check(error == 0 && edid.blocks == 1U && edid.extensions == 0U && strcmp(edid.manufacturer, "AUO") == 0 &&
	    edid.product == 0x2b99U && edid.digital == 1 && edid.bpc == 6U && edid.width_mm == 290U && edid.height_mm == 170U,
	    "EDID of the panel: AUO 0x2b99, digital 6 bpc, 290x170 mm");
	i915_host_check(edid.has_preferred == 1 && edid.preferred.clock_khz == 140800 && edid.preferred.hdisplay == 1920U &&
	    edid.preferred.vdisplay == 1080U && edid.preferred.htotal == mode.htotal && edid.preferred.vtotal == mode.vtotal &&
	    edid.preferred.edid_bpc == 6 && memcmp(edid.vendor, i915_dp_fixture_edid + 8U, 10U) == 0,
	    "EDID of the panel: preferred 1920x1080 at 140.8 MHz, the output code's timing");

	/* The first timing descriptor: the panel's is descriptor 0; a display descriptor before it is passed over. */
	error = drv_i915_xd_edid_first_timing(i915_dp_fixture_edid, &first, &index);
	i915_host_check(error == 0 && index == 0U && first.clock_khz == 140800 && first.hdisplay == 1920U &&
	    first.vdisplay == 1080U && first.width_mm == 293U && first.height_mm == 165U && first.descriptor_index == 0U,
	    "first timing of the panel: descriptor 0, 1920x1080 at 140.8 MHz, 293x165 mm (the timing's own size)");
	memcpy(base, i915_dp_fixture_edid, 128U);
	memmove(base + 54U + 18U, i915_dp_fixture_edid + 54U, 18U);
	memset(base + 54U, 0, 18U);
	base[54U + 3U] = 0xfcU;
	error = drv_i915_xd_edid_first_timing(base, &first, &index);
	i915_host_check(error == 0 && index == 1U && first.hdisplay == 1920U && first.descriptor_index == 1U,
	    "first timing: a display descriptor before it is passed over");
	memset(base + 54U, 0, 72U);
	error = drv_i915_xd_edid_first_timing(base, &first, &index);
	i915_host_check(error == EINVAL, "first timing: none in a block of display descriptors");

	/* A made-up HDMI sink: the panel's base block with one CTA extension. */
	memcpy(hdmi, i915_dp_fixture_edid, 128U);
	hdmi[126] = 1U;
	hdmi[127] = 0U;
	for (sum = 0U, at = 0U; at < 127U; at++)
		sum += hdmi[at];
	hdmi[127] = (uint8_t)(0x100U - (sum & 0xffU));
	hdmi[128] = 0x02U;
	hdmi[129] = 0x03U;
	hdmi[131] = 0x70U;
	at = 132U;
	hdmi[at++] = (uint8_t)((2U << 5) | 3U);
	hdmi[at++] = 0x90U;
	hdmi[at++] = 0x04U;
	hdmi[at++] = 0x03U;
	hdmi[at++] = (uint8_t)((3U << 5) | 7U);
	hdmi[at++] = 0x03U;
	hdmi[at++] = 0x0cU;
	hdmi[at++] = 0x00U;
	hdmi[at++] = 0x10U;
	hdmi[at++] = 0x00U;
	hdmi[at++] = 0x00U;
	hdmi[at++] = 68U;
	hdmi[130] = (uint8_t)(at - 128U);
	for (sum = 0U, at = 128U; at < 255U; at++)
		sum += hdmi[at];
	hdmi[255] = (uint8_t)(0x100U - (sum & 0xffU));
	error = drv_i915_xd_edid_decode(hdmi, sizeof(hdmi), &edid);
	i915_host_check(error == 0 && edid.blocks == 2U && edid.cta == 1 && edid.hdmi == 1 && edid.hdmi_max_tmds_khz == 340000U &&
	    edid.audio == 1 && edid.ycbcr444 == 1 && edid.ycbcr422 == 1 && edid.nvics == 3U && edid.vics[0] == 16U &&
	    edid.vics[1] == 4U && edid.vics[2] == 3U && edid.native_vic == 16U,
	    "EDID of an HDMI sink: CTA, HDMI at 340 MHz, audio, formats 16 (native), 4, 3");

	/* A damaged extension ends the decode; a damaged base block is refused; a missing extension is not read. */
	hdmi[200] ^= 1U;
	error = drv_i915_xd_edid_decode(hdmi, sizeof(hdmi), &edid);
	i915_host_check(error == 0 && edid.blocks == 1U && edid.cta == 0 && edid.hdmi == 0, "EDID: a damaged extension is not read");
	error = drv_i915_xd_edid_decode(hdmi, 128U, &edid);
	i915_host_check(error == 0 && edid.blocks == 1U && edid.extensions == 1U, "EDID: an extension past the bytes is not read");
	hdmi[10] ^= 1U;
	i915_host_check(drv_i915_xd_edid_decode(hdmi, sizeof(hdmi), &edid) == EINVAL, "EDID: a damaged base block is refused");
	i915_host_check(drv_i915_xd_edid_decode(hdmi, 127U, &edid) == EINVAL, "EDID: a short base block is refused");
}

/* Hands out the test's physical memory: the OpRegion or the raw VBT region, whole ranges only. */
static int
test_memory_map(
	void *context,
	uint64_t address,
	size_t size,
	const volatile uint8_t **bytes)
{
	(void)context;

	/* The OpRegion. */
	if (address == TEST_ASLS && size <= sizeof(test_memory_opregion)) {
		*bytes = test_memory_opregion;
		test_maps_open++;
		return 0;
	}

	/* The raw VBT region, or a range inside it. */
	if (test_raw_address != 0U &&
	    address >= test_raw_address &&
	    address - test_raw_address + size <= sizeof(test_memory_raw)) {
		*bytes = test_memory_raw + (address - test_raw_address);
		test_maps_open++;
		return 0;
	}

	/* Anything else cannot be mapped. */
	return EIO;
}

/* Counts a mapping given back. */
static void
test_memory_unmap(
	void *context,
	const volatile uint8_t *bytes,
	size_t size)
{
	(void)context;
	(void)bytes;
	(void)size;
	test_maps_open--;
}

/* Checks the OpRegion reader: the raw VBT, the fall back to mailbox 4, and each refusal. */
static void
test_opregion_read(void)
{
	static uint8_t vbt[TEST_VBT_BYTES];
	static uint8_t opregion_copy[TEST_OPREGION_BYTES];
	static uint8_t vbt_copy[TEST_RAW_BYTES];
	struct i915_xd_opregion_access access;
	struct i915_xd_opregion_read read;
	uint8_t digest[32];
	size_t size;
	int error;

	/* The test's memory access. */
	access.map = test_memory_map;
	access.unmap = test_memory_unmap;
	access.context = NULL;
	size = test_build(&test_vbts[1], vbt);
	i915_host_check(size <= 0x1800U, "OpRegion read: the made-up VBT fits mailbox 4");

	/* A 2.1 OpRegion whose raw VBT follows it, as on the Latitude 5330. */
	memset(test_memory_opregion, 0, sizeof(test_memory_opregion));
	memcpy(test_memory_opregion, "IntelGraphicsMem", 16U);
	test_memory_opregion[I915_XD_OPREGION_MAJOR] = 2U;
	test_memory_opregion[I915_XD_OPREGION_MINOR] = 1U;
	test_put32(test_memory_opregion, I915_XD_OPREGION_MAILBOXES, 0x1dU);
	test_put64(test_memory_opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDA, 0x2000U);
	test_put32(test_memory_opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDS, (uint32_t)size);
	memset(test_memory_raw, 0, sizeof(test_memory_raw));
	memcpy(test_memory_raw, vbt, size);
	test_raw_address = TEST_ASLS + 0x2000U;
	test_maps_open = 0;
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	drv_i915_xd_sha256(vbt, size, digest);
	i915_host_check(error == 0 && read.present && read.mapped && read.found, "OpRegion read: the OpRegion is read");
	i915_host_check(read.source == I915_XD_VBT_FROM_RAW && read.raw_status == I915_XD_OPREGION_RAW_USED, "OpRegion read: the raw VBT is taken");
	i915_host_check(read.vbt_size == size && memcmp(vbt_copy, vbt, size) == 0, "OpRegion read: the raw VBT is copied whole");
	i915_host_check(read.vbt_valid && memcmp(read.vbt_sha256, digest, 32U) == 0, "OpRegion read: the raw VBT's digest");
	i915_host_check(test_maps_open == 0, "OpRegion read: every mapping is given back");

	/* A raw VBT that does not check: mailbox 4 is taken instead. */
	memset(test_memory_raw, 0xa5, sizeof(test_memory_raw));
	memcpy(test_memory_opregion + I915_XD_OPREGION_VBT, vbt, size);
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == 0 && read.source == I915_XD_VBT_FROM_MAILBOX, "OpRegion read: a raw VBT that does not check falls back to mailbox 4");
	i915_host_check(read.raw_status == I915_XD_OPREGION_RAW_UNCHECKED && read.vbt_size == 0x1800U, "OpRegion read: mailbox 4 up to the ASLE extension");

	/* A raw VBT that cannot be mapped. */
	test_raw_address = 0U;
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == 0 && read.raw_status == I915_XD_OPREGION_RAW_UNMAPPED && read.source == I915_XD_VBT_FROM_MAILBOX, "OpRegion read: an unmappable raw VBT falls back");

	/* A raw VBT inside the OpRegion's own 8 KiB is not taken. */
	test_put64(test_memory_opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDA, 0x1000U);
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == 0 && read.raw_status == I915_XD_OPREGION_RAW_INSIDE, "OpRegion read: a raw VBT inside the OpRegion is refused");

	/* A raw VBT larger than the copy is not taken. */
	test_put64(test_memory_opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDA, 0x2000U);
	test_put32(test_memory_opregion, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDS, sizeof(vbt_copy) + 1U);
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == 0 && read.raw_status == I915_XD_OPREGION_RAW_TOO_LARGE, "OpRegion read: a raw VBT larger than the copy is refused");

	/* Neither place holds a VBT. */
	memset(test_memory_opregion + I915_XD_OPREGION_VBT, 0, 0x100U);
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == ENOENT && !read.vbt_valid, "OpRegion read: no VBT anywhere");

	/* No OpRegion, an OpRegion that cannot be mapped, a wrong signature. */
	error = drv_i915_xd_opregion_read(&access, 0U, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == 0 && !read.present, "OpRegion read: ASLS 0 is no OpRegion");
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS + 0x10000U, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == EIO && read.present && !read.mapped, "OpRegion read: an unmappable OpRegion");
	test_memory_opregion[0] = 'i';
	error = drv_i915_xd_opregion_read(&access, TEST_ASLS, opregion_copy, vbt_copy, sizeof(vbt_copy), &read);
	i915_host_check(error == EINVAL && read.mapped && !read.found, "OpRegion read: a wrong signature");
	i915_host_check(test_maps_open == 0, "OpRegion read: every mapping is given back after the refusals");
}

/*
 * Checks the VBT stage of the display start: the older code's types carry
 * the xd parse's values, as the shell converts them.
 */
static void
test_stage_shell(void)
{
	static uint8_t bytes[TEST_VBT_BYTES];
	static struct i915_trace trace;
	const struct i915_vbt_encoder *encoder;
	const struct i915_xd_vbt_child *child;
	const struct i915_vbt *parsed;
	struct i915_xd_vbt_panel xd_panel;
	struct i915_vbt_panel panel;
	struct i915_xd_vbt vbt;
	char what[160];
	unsigned index;
	unsigned c;
	size_t size;
	int same;

	/* Each VBT parsed by both parsers. */
	drv_i915_trace_init(&trace);
	for (index = 0U; index < 2U; index++) {
		/* The stage on an Alder Lake-P display whose OpRegion holds the VBT. */
		size = test_build(&test_vbts[index], bytes);
		drv_i915_xd_start(&test_display.xd, NULL, NULL, 0x46a6U, 0x0cU);
		drv_i915_bios_set_opregion_vbt(&test_display, bytes, size);
		i915_host_check(drv_i915_bios_init_ex(&test_display, &test_display.vbt_state, NULL, 1, 0, &trace) == 0, "stage: the VBT stage runs");
		parsed = &test_display.vbt_state.parsed;
		i915_host_check(test_display.vbt_state.vbt_found && test_display.vbt_state.source == I915_VBT_SRC_OPREGION, "stage: the OpRegion's VBT is read");

		/* Every output carries the xd parse's values. */
		(void)drv_i915_xd_vbt_parse(test_platform(0x46a6U), bytes, size, &vbt);
		same = parsed->n_encoders == vbt.nchildren && parsed->bdb_version == vbt.version;
		for (c = 0U; same && c < vbt.nchildren; c++) {
			child = &vbt.children[c];
			encoder = drv_i915_vbt_encoder_for_port(parsed, child->ddi);
			if (encoder == NULL) {
				same = 0;
				continue;
			}

			/* The encoder must carry the child's facts. */
			if (encoder->device_type != child->device_type || encoder->dvo_port != child->dvo_port ||
			    encoder->ddc_pin != child->gmbus_pin || encoder->supports_dp != child->dp ||
			    encoder->supports_edp != child->edp || encoder->supports_hdmi != child->hdmi ||
			    encoder->supports_typec_usb != child->type_c || encoder->dp_max_lane_count != child->dp_max_lanes ||
			    encoder->dp_max_link_rate != (int)child->dp_max_rate_khz || encoder->hdmi_level_shift != child->hdmi_level_shift)
				same = 0;
			if (child->aux_ch == I915_XD_VBT_NONE && encoder->aux_ch != -1)
				same = 0;
			if (child->aux_ch != I915_XD_VBT_NONE && encoder->aux_ch != (int)child->aux_ch)
				same = 0;
		}

		/* Checks the stage's outputs. */
		snprintf(what, sizeof(what), "stage: %s outputs are the xd parse's", test_vbts[index].name);
		i915_host_check(same, what);
		i915_host_check(drv_i915_vbt_encoder_for_port(parsed, 0) != NULL && drv_i915_vbt_encoder_for_port(parsed, 0)->supports_edp == 1,
		    "stage: eDP on DDI A");
		i915_host_check(drv_i915_vbt_encoder_for_port(parsed, 2) == NULL || index == 0U, "stage: no output on a port the VBT does not name");

		/* The panel too, with the pre-emphasis three bits up as the older code keeps it. */
		(void)drv_i915_xd_vbt_panel(&vbt, &vbt.children[0], i915_dp_fixture_edid, sizeof(i915_dp_fixture_edid), &xd_panel);
		memset(&panel, 0, sizeof(panel));
		i915_host_check(drv_i915_vbt_init_panel(&test_display.vbt_state.parsed, 0, i915_dp_fixture_edid, &panel) == 0, "stage: the panel");
		snprintf(what, sizeof(what), "stage: %s panel is the xd parse's", test_vbts[index].name);
		i915_host_check(panel.initialized && panel.panel_type == (int)xd_panel.panel_type && panel.bpp == xd_panel.bpp &&
		    panel.edp_preemphasis == ((int)xd_panel.edp_preemphasis << 3) && panel.t1_t3 == xd_panel.t1_t3 &&
		    panel.t11_t12 == xd_panel.t11_t12 && panel.bl_pwm_freq_hz == xd_panel.backlight_hz &&
		    panel.override_afc_startup == vbt.afc_override && panel.hdisplay == xd_panel.mode.hdisplay &&
		    panel.vtotal == xd_panel.mode.vtotal, what);
		i915_host_check(drv_i915_vbt_init_panel(&test_display.vbt_state.parsed, 9, NULL, &panel) == ENODEV, "stage: no panel on a port without an output");
		drv_i915_bios_driver_remove(&test_display.vbt_state);
		i915_host_check(drv_i915_vbt_encoder_for_port(parsed, 0) == NULL, "stage: nothing after the stage is forgotten");
	}

	/* A device without an xd description gets no outputs. */
	drv_i915_xd_start(&test_display.xd, NULL, NULL, 0x5917U, 0x07U);
	(void)drv_i915_bios_init_ex(&test_display, &test_display.vbt_state, NULL, 1, 0, &trace);
	i915_host_check(test_display.vbt_state.parsed.n_encoders == 0U, "stage: Gen9 gets no outputs from the VBT");
}
