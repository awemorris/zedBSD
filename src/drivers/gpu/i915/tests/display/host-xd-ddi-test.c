/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd DDIs' combo PHYs (display/xd-ddi.c) on the
 * register model (xd-fake-regs.c), whose group words reach the four
 * lanes: the check of a PHY the firmware brought up (the 5330's PHY A as
 * Linux left it, read from the machine's registers at 0x160000),
 * the writes of a bring-up and of the stop, the lanes a port powers, the
 * swing table of each kind of output, the signal levels of the 5330's
 * eDP against the same dump; and the DDI buffers: the DisplayPort word,
 * the start of a link (and of one already running), the training
 * patterns, the idle patterns, HDMI's start, the stop, a Type-C port's
 * PHY ownership kept, and the waits that run out.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-ddi.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The GPUs of the Latitude 5330 (Alder Lake-P), a Tiger Lake laptop, Rocket Lake and Alder Lake-S. */
#define I915_XD_DDI_TEST_ADLP		0x46a6U
#define I915_XD_DDI_TEST_ADLP_REVID	0x0cU
#define I915_XD_DDI_TEST_TGL		0x9a49U
#define I915_XD_DDI_TEST_TGL_REVID	0x01U
#define I915_XD_DDI_TEST_RKL		0x4c8aU
#define I915_XD_DDI_TEST_ADLS		0x4680U

/* The most writes one check reads back. */
#define I915_XD_DDI_TEST_WRITES		64U

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_ddi_test_io;

static const struct i915_xd_platform *i915_xd_ddi_test_setup(uint16_t device, uint8_t revid);
static void i915_xd_ddi_test_group(uint32_t offset, uint32_t value, void *context);
static unsigned i915_xd_ddi_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static void i915_xd_ddi_test_5330_phy_a(void);
static void i915_xd_ddi_test_ready(void);
static void i915_xd_ddi_test_init(void);
static void i915_xd_ddi_test_uninit(void);
static void i915_xd_ddi_test_lanes(void);
static void i915_xd_ddi_test_tables(void);
static void i915_xd_ddi_test_levels(void);
static void i915_xd_ddi_test_buffer_rule(uint32_t offset, uint32_t value, void *context);
static void i915_xd_ddi_test_hook(void *context);
static void i915_xd_ddi_test_buffers(void);

/*
 * Runs every check of the xd combo PHYs and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_ddi_test_ready();
	i915_xd_ddi_test_init();
	i915_xd_ddi_test_uninit();
	i915_xd_ddi_test_lanes();
	i915_xd_ddi_test_tables();
	i915_xd_ddi_test_levels();
	i915_xd_ddi_test_buffers();

	/* Reports the result. */
	failures = i915_host_report("host_xd_ddi_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model whose group words of combo PHYs A and B reach
 * their four lanes; returns the description of a device.
 */
static const struct i915_xd_platform *
i915_xd_ddi_test_setup(
	uint16_t device,
	uint8_t revid)
{
	static const uint32_t bases[] = { I915_XD_COMBO_PHY_A, I915_XD_COMBO_PHY_B };
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	unsigned base;
	unsigned dw;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_ddi_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_ddi_test_io, &hooks);

	/* The group words of the PCS and the transmitters reach the lanes. */
	for (base = 0U; base < sizeof(bases) / sizeof(bases[0]); base++) {
		for (dw = 0U; dw < 10U; dw++) {
			i915_xd_fake_rule_call(I915_XD_PORT_PCS_DW_GRP(bases[base], dw), i915_xd_ddi_test_group, NULL);
			i915_xd_fake_rule_call(I915_XD_PORT_TX_DW_GRP(bases[base], dw), i915_xd_ddi_test_group, NULL);
		}
	}

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Copies a group word to the same word of the four lanes. */
static void
i915_xd_ddi_test_group(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	uint32_t base;
	uint32_t within;
	unsigned lane;

	UNUSED_PARAMETER(context);

	/* The PHY's block and the word in it. */
	base = offset & ~0xfffU;
	if (base == (I915_XD_COMBO_PHY_B & ~0xfffU))
		base = I915_XD_COMBO_PHY_B;
	within = offset - base;

	/* PCS group words at 0x600, transmitter group words at 0x680. */
	for (lane = 0U; lane < 4U; lane++) {
		if (within < 0x680U)
			i915_xd_fake_set(I915_XD_PORT_PCS_DW_LN(base, lane, (within - 0x600U) / 4U), value);
		else
			i915_xd_fake_set(I915_XD_PORT_TX_DW_LN(base, lane, (within - 0x680U) / 4U), value);
	}
}

/* Copies the writes the window recorded, oldest first; reports how many. */
static unsigned
i915_xd_ddi_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_ddi_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count && kept < max; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* Loads the 5330's PHY A as Linux left it (the words the check reads). */
static void
i915_xd_ddi_test_5330_phy_a(void)
{
	unsigned lane;

	/* The compensation, its reference values and the common lane. */
	i915_xd_fake_set(I915_XD_PHY_MISC(0U), 0U);
	i915_xd_fake_set(0x162014U, 0x1204045bU);
	i915_xd_fake_set(0x162100U, 0x80005f28U);
	i915_xd_fake_set(0x162104U, 0x81000400U);
	i915_xd_fake_set(0x16210cU, 0xc0607623U);
	i915_xd_fake_set(0x162120U, 0x010d0280U);
	i915_xd_fake_set(0x162124U, 0x62ab67bbU);
	i915_xd_fake_set(0x162128U, 0x51914f96U);

	/* The duty cycle corrector of each lane. */
	for (lane = 0U; lane < 4U; lane++) {
		i915_xd_fake_set(I915_XD_PORT_PCS_DW_LN(I915_XD_COMBO_PHY_A, lane, 1U), 0x1c000004U);
		i915_xd_fake_set(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 8U), 0xb003593cU);
	}
}

/* Checks the read of whether a PHY is brought up. */
static void
i915_xd_ddi_test_ready(void)
{
	const struct i915_xd_platform *platform;

	/* The 5330's PHY A reads as brought up: the start does not touch it. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "ddi: the description is found");
		return;
	}

	/* PHY A as the 5330's firmware leaves it reads as brought up. */
	i915_xd_ddi_test_5330_phy_a();
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 0U) == 1,
			"ddi: the 5330's PHY A (Linux's words) reads as brought up");

	/* Each word that is wrong makes it not brought up. */
	i915_xd_fake_set(I915_XD_PHY_MISC(0U), I915_XD_PHY_MISC_IO_COMP_POWER_DOWN);
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 0U) == 0, "ddi: a powered-down compensation is not brought up");
	i915_xd_fake_set(I915_XD_PHY_MISC(0U), 0U);
	i915_xd_fake_set(0x162124U, 0x62ab67baU);
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 0U) == 0, "ddi: a wrong reference value is not brought up");
	i915_xd_fake_set(0x162124U, 0x62ab67bbU);
	i915_xd_fake_set(0x162120U, 0x000d0280U);
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 0U) == 0, "ddi: the master PHY without its reference current is not brought up");
	i915_xd_fake_set(0x162120U, 0x010d0280U);
	i915_xd_fake_set(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, 0U, 8U), 0x9003593cU);
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 0U) == 0, "ddi: the ODCC divider not by 2 is not brought up");
}

/* Checks the writes of the start: PHY B from cold, PHY A left alone. */
static void
i915_xd_ddi_test_init(void)
{
	static const uint32_t want_offsets[] = {
		0x64c04U, 0x06c6a0U, 0x06c604U, 0x06c104U, 0x06c124U, 0x06c128U, 0x06c100U, 0x06c014U
	};
	static const uint32_t want_values[] = {
		0x00000000U, 0xa0000000U, 0x00000000U, 0x00000000U, 0x86e172c7U, 0x77ca5eabU, 0x80000000U, 0x00000010U
	};
	const struct i915_xd_platform *platform;
	uint32_t offsets[I915_XD_DDI_TEST_WRITES];
	uint32_t values[I915_XD_DDI_TEST_WRITES];
	unsigned written;
	unsigned count;
	unsigned index;
	int same;

	/* PHY A as Linux left it; PHY B cold, reporting 0.95 V dot 0, its compensation powered down. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_ddi_test_5330_phy_a();
	i915_xd_fake_set(I915_XD_PHY_MISC(1U), I915_XD_PHY_MISC_IO_COMP_POWER_DOWN);
	i915_xd_fake_set(I915_XD_PORT_COMP_DW(I915_XD_COMBO_PHY_B, 3U), 0x01000000U);

	/* PHY B only: the compensation powered, ODCC, DCC, the reference values, the start; no reference current (a slave). */
	written = drv_i915_xd_combo_phy_init(&i915_xd_ddi_test_io, platform);
	count = i915_xd_ddi_test_writes(offsets, values, I915_XD_DDI_TEST_WRITES);
	same = count == sizeof(want_offsets) / sizeof(want_offsets[0]);
	for (index = 0U; same && index < count; index++) {
		if (offsets[index] != want_offsets[index] || values[index] != want_values[index])
			same = 0;
	}

	/* Checks the start writes PHY B only, in Linux's order (PHY_MISC, ODCC, DCC, procmon, COMP_INIT, CL_DW5). */
	i915_host_check(written == 0x2U && same, "ddi: the start writes PHY B only, in Linux's order (PHY_MISC, ODCC, DCC, procmon, COMP_INIT, CL_DW5)");

	/* Brought up, PHY B reads as such, and a second start writes nothing. */
	i915_host_check(drv_i915_xd_combo_phy_ready(&i915_xd_ddi_test_io, platform, 1U) == 1, "ddi: PHY B reads as brought up after the start");
	written = drv_i915_xd_combo_phy_init(&i915_xd_ddi_test_io, platform);
	i915_host_check(written == 0U, "ddi: a second start writes nothing");

	/* Rocket Lake brings up four PHYs, A and C with the reference current. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_RKL, 0U);
	if (platform == NULL)
		return;
	written = drv_i915_xd_combo_phy_init(&i915_xd_ddi_test_io, platform);
	i915_host_check(written == 0xfU &&
			(i915_xd_fake_get(I915_XD_PORT_COMP_DW(I915_XD_COMBO_PHY_C, 8U)) & I915_XD_COMP_DW8_IREFGEN) != 0U &&
			(i915_xd_fake_get(I915_XD_PORT_COMP_DW(I915_XD_COMBO_PHY_D, 8U)) & I915_XD_COMP_DW8_IREFGEN) == 0U &&
			(i915_xd_fake_get(I915_XD_PORT_COMP_DW(I915_XD_COMBO_PHY_D, 0U)) & I915_XD_COMP_DW0_INIT) != 0U,
			"ddi: Rocket Lake brings up A to D, with the reference current on C and not on D");

	/* Alder Lake-S: five PHYs, PHY_MISC only on A. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLS, 0U);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_PHY_MISC(1U), I915_XD_PHY_MISC_IO_COMP_POWER_DOWN);
	written = drv_i915_xd_combo_phy_init(&i915_xd_ddi_test_io, platform);
	i915_host_check(written == 0x1fU && i915_xd_fake_get(I915_XD_PHY_MISC(1U)) == I915_XD_PHY_MISC_IO_COMP_POWER_DOWN,
			"ddi: Alder Lake-S brings up A to E and touches PHY_MISC of A only");
}

/* Checks the stop: last PHY first, the compensation powered down, then stopped. */
static void
i915_xd_ddi_test_uninit(void)
{
	static const uint32_t want_offsets[] = { 0x64c04U, 0x06c100U, 0x64c00U, 0x162100U };
	const struct i915_xd_platform *platform;
	uint32_t offsets[I915_XD_DDI_TEST_WRITES];
	uint32_t values[I915_XD_DDI_TEST_WRITES];
	unsigned count;
	unsigned a_down;
	int same;

	/* Both PHYs up: B first, then A; PHY A was up. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_ddi_test_5330_phy_a();
	a_down = drv_i915_xd_combo_phy_uninit(&i915_xd_ddi_test_io, platform);
	count = i915_xd_ddi_test_writes(offsets, values, I915_XD_DDI_TEST_WRITES);
	same = count == 4U && offsets[0] == want_offsets[0] && offsets[1] == want_offsets[1] && offsets[2] == want_offsets[2] &&
	       offsets[3] == want_offsets[3] && values[2] == I915_XD_PHY_MISC_IO_COMP_POWER_DOWN && values[3] == 0x00005f28U;
	i915_host_check(a_down == 0U && same, "ddi: the stop goes B then A: PHY_MISC powered down, COMP_INIT cleared");

	/* A PHY A that was not up is reported. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	a_down = drv_i915_xd_combo_phy_uninit(&i915_xd_ddi_test_io, platform);
	i915_host_check(a_down == 1U, "ddi: a PHY A that was not up before the stop is reported");
}

/* Checks the lanes a port powers. */
static void
i915_xd_ddi_test_lanes(void)
{
	const struct i915_xd_platform *platform;
	uint32_t word;

	/* The Alder Lake-P description. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	word = I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 10U);
	i915_xd_fake_set(word, 0x00030000U);

	/* The 5330's eDP: two lanes, lanes 3 and 2 down (0x000300c0, Linux's dump). */
	drv_i915_xd_combo_lanes(&i915_xd_ddi_test_io, 0U, 2U, 0);
	i915_host_check(i915_xd_fake_get(word) == 0x000300c0U, "ddi: two lanes keep lanes 3 and 2 down (the 5330's 0x000300c0)");
	drv_i915_xd_combo_lanes(&i915_xd_ddi_test_io, 0U, 1U, 1);
	i915_host_check(i915_xd_fake_get(word) == 0x00030070U, "ddi: one reversed lane keeps lanes 2 to 0 down");
	drv_i915_xd_combo_lanes(&i915_xd_ddi_test_io, 0U, 4U, 0);
	i915_host_check(i915_xd_fake_get(word) == 0x00030000U, "ddi: four lanes power every lane");
	drv_i915_xd_combo_lanes(&i915_xd_ddi_test_io, 0U, 3U, 0);
	i915_host_check(i915_xd_fake_get(word) == 0x00030000U, "ddi: three lanes, which a combo PHY does not know, power every lane");
}

/* Checks the swing table of each kind of output. */
static void
i915_xd_ddi_test_tables(void)
{
	const struct i915_xd_platform *adlp;
	const struct i915_xd_platform *tgl;
	const struct i915_xd_combo_swing_table *table;

	/* The Alder Lake-P and Tiger Lake descriptions. */
	adlp = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	tgl = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_TGL, I915_XD_DDI_TEST_TGL_REVID);
	if (adlp == NULL || tgl == NULL)
		return;

	/* Alder Lake-P. */
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_EDP, 270000U, 0U);
	i915_host_check(table == adlp->swing->dp_low && table->count == 10U && table->rows[4].n_scalar == 0x4cU,
			"ddi: the 5330's eDP at 2.7 GHz without HOBL or low swing takes the DP table up to HBR (row 4, swing 1: N scalar 0x4c)");
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_EDP, 270000U, I915_XD_SWING_HOBL | I915_XD_SWING_LOW);
	i915_host_check(table == adlp->swing->edp_hobl && drv_i915_xd_combo_swing_is_hobl(adlp, table),
			"ddi: HOBL wins over the low swing");
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_EDP, 540000U, I915_XD_SWING_LOW);
	i915_host_check(table == adlp->swing->edp_low_swing, "ddi: the low swing up to 5.4 GHz");
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_EDP, 810000U, I915_XD_SWING_HOBL);
	i915_host_check(table == adlp->swing->edp_hbr3, "ddi: eDP above 5.4 GHz takes the HBR3 table, whatever the VBT says");
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_DP, 540000U, I915_XD_SWING_HOBL);
	i915_host_check(table == adlp->swing->dp_high && !drv_i915_xd_combo_swing_is_hobl(adlp, table),
			"ddi: DisplayPort above 2.7 GHz takes the high table; the VBT's eDP settings do not apply");
	table = drv_i915_xd_combo_swing_table(adlp, I915_XD_OUTPUT_HDMI, 165000U, 0U);
	i915_host_check(table == adlp->swing->hdmi && table->hdmi_default == table->count - 1U,
			"ddi: HDMI takes its table, whose default is the last row");

	/* Tiger Lake. */
	table = drv_i915_xd_combo_swing_table(tgl, I915_XD_OUTPUT_DP, 270000U, 0U);
	i915_host_check(table == tgl->swing->dp_low && table != adlp->swing->dp_low, "ddi: Tiger Lake has its own DisplayPort table");
}

/* Checks the signal levels of the 5330's eDP: the words of Linux's dump. */
static void
i915_xd_ddi_test_levels(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_combo_levels levels;
	unsigned lane;
	int words;
	uint32_t dw2;
	uint32_t dw4;
	uint32_t dw5;
	uint32_t dw7;
	uint32_t want4;

	/* The Alder Lake-P description. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL)
		return;

	/* eDP, 2.7 GHz, two lanes, row 4 (swing 1, no pre-emphasis) on every lane; the 4K2K override set from before. */
	memset(&levels, 0, sizeof(levels));
	levels.phy = 0U;
	levels.kind = I915_XD_OUTPUT_EDP;
	levels.rate_khz = 270000U;
	levels.lanes = 2U;
	levels.table = drv_i915_xd_combo_swing_table(platform, I915_XD_OUTPUT_EDP, 270000U, 0U);
	memset(levels.level, 4, sizeof(levels.level));
	for (lane = 0U; lane < 4U; lane++) {
		i915_xd_fake_set(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 4U), 0x00280000U);
		i915_xd_fake_set(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 5U), 0x00210000U);
	}

	/* The common lane's word, then the levels. */
	i915_xd_fake_set(I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 10U), 0x000300ccU);
	drv_i915_xd_combo_signal_levels(&i915_xd_ddi_test_io, platform, &levels);

	/*
	 * The other bits of DW4 and DW5 as the hardware held them; DW2 0x9098,
	 * DW7 0x4c000000 on every lane; DW4 0x0028003f with the load generator
	 * on lanes 1 and 2; DW5 0xa0290030.
	 */
	words = 1;
	for (lane = 0U; lane < 4U; lane++) {
		/* The lane's swing words. */
		dw2 = i915_xd_fake_get(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 2U));
		dw7 = i915_xd_fake_get(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 7U));
		dw4 = i915_xd_fake_get(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 4U));
		dw5 = i915_xd_fake_get(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, lane, 5U));

		/* Lanes 1 and 2 have the load generator on. */
		want4 = 0x0028003fU;
		if (lane == 1U || lane == 2U)
			want4 = 0x8028003fU;

		/* Each word must read as the 5330's eDP levels. */
		if (dw2 != 0x00009098U)
			words = 0;
		if (dw7 != 0x4c000000U)
			words = 0;
		if (dw4 != want4)
			words = 0;
		if (dw5 != 0xa0290030U)
			words = 0;
	}

	/* Checks the 5330's eDP levels are Linux's words: DW2 0x9098, DW4 0x0028003f (load generator on lanes 1-2), DW5 0xa0290030, DW7 0x4c000000. */
	i915_host_check(words, "ddi: the 5330's eDP levels are Linux's words: DW2 0x9098, DW4 0x0028003f (load generator on lanes 1-2), DW5 0xa0290030, DW7 0x4c000000");
	i915_host_check(i915_xd_fake_get(I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 10U)) == 0x000300c0U &&
			(i915_xd_fake_get(I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 5U)) & I915_XD_CL_DW5_SUS_CLOCK_CONFIG) == I915_XD_CL_DW5_SUS_CLOCK_CONFIG &&
			(i915_xd_fake_get(I915_XD_PORT_PCS_DW_LN(I915_XD_COMBO_PHY_A, 0U, 1U)) & I915_XD_PCS_DW1_COMMON_KEEPER) != 0U,
			"ddi: no HOBL clears the 4K2K override (0x000300c0); the SUS clock and the common keeper are set");

	/* HDMI clears the common keeper; four lanes at 8.1 GHz take no load generator. */
	levels.kind = I915_XD_OUTPUT_HDMI;
	levels.table = drv_i915_xd_combo_swing_table(platform, I915_XD_OUTPUT_HDMI, 594000U, 0U);
	levels.rate_khz = 810000U;
	levels.lanes = 4U;
	drv_i915_xd_combo_signal_levels(&i915_xd_ddi_test_io, platform, &levels);
	i915_host_check((i915_xd_fake_get(I915_XD_PORT_PCS_DW_LN(I915_XD_COMBO_PHY_A, 0U, 1U)) & I915_XD_PCS_DW1_COMMON_KEEPER) == 0U &&
			(i915_xd_fake_get(I915_XD_PORT_TX_DW_LN(I915_XD_COMBO_PHY_A, 1U, 4U)) & I915_XD_TX_DW4_LOADGEN) == 0U,
			"ddi: HDMI clears the common keeper; above 6 GHz no lane takes the load generator");
}

/* The writes counted when the start's step before the buffer ran. */
static unsigned i915_xd_ddi_test_hook_writes;

/* Makes a DDI buffer idle while it is disabled, and active while it is enabled (unless context says it stays idle). */
static void
i915_xd_ddi_test_buffer_rule(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	/* The idle bit follows the enable. */
	if ((value & I915_XD_DDI_BUF_ENABLE) != 0U && context == NULL)
		i915_xd_fake_set(offset, value & ~I915_XD_DDI_BUF_IDLE);
	else
		i915_xd_fake_set(offset, value | I915_XD_DDI_BUF_IDLE);
}

/* Notes how many writes were made when the start's step before the buffer runs. */
static void
i915_xd_ddi_test_hook(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* The count of writes so far. */
	i915_xd_ddi_test_hook_writes = i915_xd_fake_writes();
}

/* Checks the DDI buffers. */
static void
i915_xd_ddi_test_buffers(void)
{
	static const uint32_t start_offsets[] = { 0x60540U, 0x64000U };
	static const uint32_t start_values[] = { 0x80040000U, 0x80000002U };
	static const uint32_t restart_offsets[] = { 0x64000U, 0x60540U, 0x60540U, 0x64000U };
	static const uint32_t restart_values[] = { 0x00000002U, 0x00040300U, 0x80040000U, 0x80000002U };
	static int stays_idle;
	const struct i915_xd_platform *platform;
	struct i915_xd_ddi_link link;
	uint32_t offsets[I915_XD_DDI_TEST_WRITES];
	uint32_t values[I915_XD_DDI_TEST_WRITES];
	uint32_t word;
	unsigned count;
	unsigned late;
	unsigned index;
	int same;

	/* The 5330's eDP: DDI A, transcoder A, two lanes at 2.7 GHz, enhanced framing. */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(0U), i915_xd_ddi_test_buffer_rule, NULL);
	i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(4U), i915_xd_ddi_test_buffer_rule, NULL);
	memset(&link, 0, sizeof(link));
	link.ddi = 0U;
	link.trans = 0U;
	link.lanes = 2U;
	link.rate_khz = 270000U;
	link.enhanced = 1;
	word = drv_i915_xd_ddi_dp_word(platform, &link);
	i915_host_check(word == 0x00000002U, "ddi: the 5330's eDP word is two lanes, translation 0 (Linux's 0x80000002 without the enable)");

	/* The start: the transport in pattern 1 with enhanced framing, the step before the buffer, the buffer. */
	i915_xd_fake_set(I915_XD_DDI_BUF_CTL(0U), I915_XD_DDI_BUF_IDLE);
	i915_xd_ddi_test_hook_writes = 0U;
	late = drv_i915_xd_ddi_dp_start(&i915_xd_ddi_test_io, &link, word, i915_xd_ddi_test_hook, NULL);
	count = i915_xd_ddi_test_writes(offsets, values, I915_XD_DDI_TEST_WRITES);
	same = count == 2U && offsets[0] == start_offsets[0] && values[0] == start_values[0] &&
	       offsets[1] == start_offsets[1] && values[1] == start_values[1];
	i915_host_check(late == 0U && same && i915_xd_ddi_test_hook_writes == 1U,
			"ddi: the start writes DP_TP_CTL 0x80040000, runs the step, then DDI_BUF_CTL 0x80000002 and waits for it to leave idle");

	/* A running link stops first: the buffer, the transport, the wait for idle; then it starts again. */
	i915_xd_fake_set(I915_XD_DP_TP_CTL(0U), 0x80040300U);
	late = drv_i915_xd_ddi_dp_start(&i915_xd_ddi_test_io, &link, word, NULL, NULL);
	count = i915_xd_ddi_test_writes(offsets, values, I915_XD_DDI_TEST_WRITES);
	same = count == 6U;
	for (index = 0U; same && index < 4U; index++) {
		if (offsets[2U + index] != restart_offsets[index] || values[2U + index] != restart_values[index])
			same = 0;
	}

	/* Checks a running link stops (buffer off, transport off, idle) before it starts again. */
	i915_host_check(late == 0U && same, "ddi: a running link stops (buffer off, transport off, idle) before it starts again");

	/* The patterns. */
	drv_i915_xd_ddi_pattern(&i915_xd_ddi_test_io, 0U, I915_XD_DP_TP_PATTERN_2);
	i915_host_check(i915_xd_fake_get(I915_XD_DP_TP_CTL(0U)) == 0x80040100U, "ddi: pattern 2 replaces the field");
	drv_i915_xd_ddi_pattern(&i915_xd_ddi_test_io, 0U, I915_XD_DP_TP_PATTERN_NORMAL);
	i915_host_check(i915_xd_fake_get(I915_XD_DP_TP_CTL(0U)) == 0x80040300U, "ddi: normal replaces the field");
	i915_xd_fake_set(I915_XD_DP_TP_STATUS(0U), I915_XD_DP_TP_STATUS_IDLE_DONE);
	late = drv_i915_xd_ddi_idle_patterns(&i915_xd_ddi_test_io, 0U);
	i915_host_check(late == 0U && i915_xd_fake_get(I915_XD_DP_TP_CTL(0U)) == 0x80040200U, "ddi: the idle pattern, and its patterns were sent");
	i915_xd_fake_set(I915_XD_DP_TP_STATUS(0U), 0U);
	late = drv_i915_xd_ddi_idle_patterns(&i915_xd_ddi_test_io, 0U);
	i915_host_check(late == I915_XD_DDI_LATE_IDLE_PATTERNS, "ddi: idle patterns that are not sent are reported");

	/* The stop with FEC: buffer off, transport off, FEC off, idle. */
	link.fec = 1;
	i915_xd_fake_set(I915_XD_DP_TP_CTL(0U), 0xc0040300U);
	late = drv_i915_xd_ddi_stop(&i915_xd_ddi_test_io, &link, 1);
	i915_host_check(late == 0U && i915_xd_fake_get(I915_XD_DP_TP_CTL(0U)) == 0x00040300U &&
			(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U)) & (I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_IDLE)) == I915_XD_DDI_BUF_IDLE,
			"ddi: the stop clears the buffer, the transport and FEC, and the buffer goes idle");

	/*
	 * A Type-C port (DDI 4 = TC2 on Alder Lake-P, the 5330's USB-C DP):
	 * four lanes at 5.4 GHz, the reversal kept, the ownership the Type-C
	 * code took kept.
	 */
	memset(&link, 0, sizeof(link));
	link.ddi = 4U;
	link.trans = 1U;
	link.tc = 1;
	link.lanes = 4U;
	link.rate_khz = 540000U;
	link.kept = I915_XD_DDI_BUF_REVERSAL | I915_XD_DDI_BUF_TC_OWNED;
	word = drv_i915_xd_ddi_dp_word(platform, &link);
	i915_host_check(word == (I915_XD_DDI_BUF_REVERSAL | 0x00200006U), "ddi: a display 13 Type-C word carries the link rate (5.4 GHz = 2), four lanes, the reversal, not the ownership");
	i915_xd_fake_set(I915_XD_DDI_BUF_CTL(4U), I915_XD_DDI_BUF_TC_OWNED | I915_XD_DDI_BUF_IDLE);
	late = drv_i915_xd_ddi_dp_start(&i915_xd_ddi_test_io, &link, word, NULL, NULL);
	i915_host_check(late == 0U && (i915_xd_fake_get(I915_XD_DDI_BUF_CTL(4U)) & I915_XD_DDI_BUF_TC_OWNED) != 0U,
			"ddi: the start keeps the Type-C PHY ownership bit 6 the Type-C code set");
	i915_xd_fake_set(I915_XD_DDI_BUF_CTL(4U), I915_XD_DDI_BUF_IDLE);
	late = drv_i915_xd_ddi_dp_start(&i915_xd_ddi_test_io, &link, word | I915_XD_DDI_BUF_TC_OWNED, NULL, NULL);
	i915_host_check(late == 0U && (i915_xd_fake_get(I915_XD_DDI_BUF_CTL(4U)) & I915_XD_DDI_BUF_TC_OWNED) == 0U,
			"ddi: ... and does not take it when the Type-C code did not, even with it in the word");

	/* HDMI: the kept bits and the enable; a display 13 Type-C port takes the ownership. */
	late = drv_i915_xd_ddi_hdmi_start(&i915_xd_ddi_test_io, platform, &link);
	i915_host_check(late == 0U && (i915_xd_fake_get(I915_XD_DDI_BUF_CTL(4U)) & ~I915_XD_DDI_BUF_IDLE) ==
			(I915_XD_DDI_BUF_ENABLE | I915_XD_DDI_BUF_REVERSAL | I915_XD_DDI_BUF_TC_OWNED),
			"ddi: HDMI's buffer is the kept bits and the enable, with the ownership on a Type-C port");

	/* A buffer that stays idle: the wait runs out (3 ms on Type-C). */
	platform = i915_xd_ddi_test_setup(I915_XD_DDI_TEST_ADLP, I915_XD_DDI_TEST_ADLP_REVID);
	i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(4U), i915_xd_ddi_test_buffer_rule, &stays_idle);
	late = drv_i915_xd_ddi_dp_start(&i915_xd_ddi_test_io, &link, word, NULL, NULL);
	i915_host_check(late == I915_XD_DDI_LATE_ACTIVE && i915_xd_fake_now() >= 3000U,
			"ddi: a buffer that stays idle is reported after the Type-C port's 3 ms");
}
