/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DDIs of the display 12 and 13 code (see xd-ddi.h).
 */

#include "xd-ddi.h"
#include "xd-aux.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-platform.h"
#include <kern/kcrt.h>

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd9.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The fixed transmit settings of a combo port's swing: the scaling mode
 * and termination of DW5, the compensation scalar of DW2; the rate above
 * which no lane takes the load generator.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_ddi_combo_vswing_program, icl_combo_phy_loadgen_select).
 */
#define I915_XD_COMBO_SCALING_MODE	0x2U
#define I915_XD_COMBO_RTERM		0x6U
#define I915_XD_COMBO_RCOMP_SCALAR	0x98U
#define I915_XD_COMBO_LOADGEN_KHZ	600000U

/*
 * The lanes a port powers down in DW10, by its lane count: one lane,
 * two lanes, and the same reversed.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy_regs.h (PWR_DOWN_LN_3_2_1,
 * PWR_DOWN_LN_3_2, PWR_DOWN_LN_2_1_0, PWR_DOWN_LN_1_0, PWR_UP_ALL_LANES).
 */
#define I915_XD_LANES_1			0xeU
#define I915_XD_LANES_2			0xcU
#define I915_XD_LANES_1_REVERSED	0x7U
#define I915_XD_LANES_2_REVERSED	0x3U

/*
 * How long a DDI buffer may take to go idle, and to leave idle (a Type-C
 * PHY's longer); how long the idle patterns may take.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_wait_ddi_buf_idle, intel_wait_ddi_buf_active,
 * intel_ddi_set_idle_link_train).
 */
#define I915_XD_DDI_IDLE_US		8U
#define I915_XD_DDI_ACTIVE_US		1000U
#define I915_XD_DDI_ACTIVE_TC_US	3000U
#define I915_XD_DDI_IDLE_PATTERNS_US	1000U

/* The bytes of COMP_DW1 that hold the process and voltage reference. */
#define I915_XD_COMP_DW1_PROCMON	0x00ff00ffU

/*
 * The rows of a display 9 DisplayPort or eDP table a DDI other than A
 * drives: the tenth row is DDI A's alone.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DDI buffer translations of DDI A and of the other DDIs).
 */
#define I915_XD9_DDI_ROWS_NOT_A		9U

/*
 * The waits of a display 9 DDI: the buffer leaves idle after more than
 * 518 us, goes idle within 8 us, and the idle patterns go out within
 * 1 ms.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DDI buffer's enable and disable times, the idle patterns).
 */
#define I915_XD9_DDI_ACTIVE_US		600U
#define I915_XD9_DDI_IDLE_US		8U
#define I915_XD9_DDI_IDLE_PATTERNS_US	1000U

/* The translation row an HDMI port drives: the tenth. */
#define I915_XD9_DDI_HDMI_ROW		9U

/* The boosts a translation row may ask for: off, or a balance leg of 1, 3 or 7. */
#define I915_XD9_BOOST_1		1U
#define I915_XD9_BOOST_3		3U
#define I915_XD9_BOOST_7		7U

/* How many times the sink's power state is written while it wakes, a millisecond apart. */
#define I915_XD9_WAKE_TRIES		3U
#define I915_XD9_WAKE_US		1000U

/* The DPCD revision from which a sink has a power state. */
#define I915_XD9_DPCD_REV_1_1		0x11U

/* The pattern field of a DPCD pattern byte. */
#define I915_XD9_PATTERN_FIELD		0x7U

/*
 * The enhanced framing bit of a sink's MAX_LANE_COUNT.
 *
 * Values referenced: VESA DisplayPort Standard 1.4 (DPCD MAX_LANE_COUNT,
 * ENHANCED_FRAME_CAP).
 */
#define I915_XD9_DPCD_ENHANCED_CAP	0x80U

static uint32_t i915_xd_combo_base(unsigned phy);
static int i915_xd_combo_has(uint8_t mask, unsigned phy);
static int i915_xd_combo_check(struct i915_xd_io *io, uint32_t offset, uint32_t mask, uint32_t value);
static void i915_xd_combo_init_one(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned phy);
static uint32_t i915_xd_combo_loadgen(const struct i915_xd_combo_levels *levels, unsigned lane);
static void i915_xd_combo_swing(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_combo_levels *levels, uint32_t base);
static unsigned i915_xd_ddi_wait_active(struct i915_xd_io *io, const struct i915_xd_ddi_link *link);
static void i915_xd9_dp_levels(void *context, const uint8_t set[4]);
static void i915_xd9_dp_pattern(void *context, uint8_t pattern);
static void i915_xd9_dp_prepare(void *context);
static void i915_xd9_dp_idle(void *context);
static void i915_xd9_dp_sleep(void *context, unsigned min_us, unsigned max_us);
static int i915_xd9_sink_read(void *context, uint32_t address, uint8_t *buffer, unsigned size);
static int i915_xd9_sink_write(void *context, uint32_t address, const uint8_t *buffer, unsigned size);
static int i915_xd9_sink_probe(void *context, uint32_t address);
static int i915_xd9_sink_caps(void *context, uint8_t *caps);
static void i915_xd9_dp_link(struct i915_xd9_dp_port *port, uint32_t rate_khz, unsigned lanes, struct i915_xd_dp_link *link);

/*
 * Tells whether a combo PHY reads as brought up: its IO compensation
 * powered (where the PHY has the word) and started; on display 12 on the
 * duty cycle corrector set to run once; the process and voltage reference
 * values of what the PHY reports; the reference current on a PHY that
 * owns the compensation resistor; its common lane allowed to power down.
 * Every word is read even after one that is wrong.  Returns 1 or 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_combo_phy_verify_state, icl_combo_phy_enabled,
 * icl_verify_procmon_ref_values, phy_is_master, has_phy_misc).
 */
int
drv_i915_xd_combo_phy_ready(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned phy)
{
	const struct i915_xd_procmon *procmon;
	uint32_t base;
	int ready;
	uint32_t value;
	int has;

	/* A PHY whose compensation is powered down or not started is not brought up. */
	base = i915_xd_combo_base(phy);
	has = i915_xd_combo_has(platform->phy_misc, phy);
	if (has) {
		/* The compensation's power down. */
		value = drv_i915_xd_io_read(io, I915_XD_PHY_MISC(phy));
		if ((value & I915_XD_PHY_MISC_IO_COMP_POWER_DOWN) != 0U)
			return 0;
	}

	/* The compensation started. */
	value = drv_i915_xd_io_read(io, I915_XD_PORT_COMP_DW(base, 0U));
	if ((value & I915_XD_COMP_DW0_INIT) == 0U)
		return 0;

	/* The duty cycle corrector of display 12 on. */
	ready = 1;
	if (platform->display_ver >= 12U) {
		ready &= i915_xd_combo_check(io, I915_XD_PORT_TX_DW_LN(base, 0U, 8U),
			I915_XD_TX_DW8_ODCC_CLOCK_SELECT | I915_XD_TX_DW8_ODCC_DIVIDER,
			I915_XD_TX_DW8_ODCC_CLOCK_SELECT | i915_xd_field_put(I915_XD_TX_DW8_ODCC_DIVIDER, I915_XD_TX_DW8_ODCC_DIVIDE_BY_2));
		ready &= i915_xd_combo_check(io, I915_XD_PORT_PCS_DW_LN(base, 0U, 1U), I915_XD_PCS_DW1_DCC_MODE,
			i915_xd_field_put(I915_XD_PCS_DW1_DCC_MODE, I915_XD_PCS_DW1_DCC_RUN_ONCE));
	}

	/* The reference values of the process and voltage the PHY reports. */
	procmon = drv_i915_xd_procmon(drv_i915_xd_io_read(io, I915_XD_PORT_COMP_DW(base, 3U)));
	ready &= i915_xd_combo_check(io, I915_XD_PORT_COMP_DW(base, 1U), I915_XD_COMP_DW1_PROCMON, procmon->dw1);
	ready &= i915_xd_combo_check(io, I915_XD_PORT_COMP_DW(base, 9U), 0xffffffffU, procmon->dw9);
	ready &= i915_xd_combo_check(io, I915_XD_PORT_COMP_DW(base, 10U), 0xffffffffU, procmon->dw10);

	/* The reference current of a PHY with the resistor, and the common lane's power down. */
	has = i915_xd_combo_has(platform->comp_masters, phy);
	if (has)
		ready &= i915_xd_combo_check(io, I915_XD_PORT_COMP_DW(base, 8U), I915_XD_COMP_DW8_IREFGEN, I915_XD_COMP_DW8_IREFGEN);
	ready &= i915_xd_combo_check(io, I915_XD_PORT_CL_DW(base, 5U), I915_XD_CL_DW5_POWER_DOWN_ENABLE, I915_XD_CL_DW5_POWER_DOWN_ENABLE);

	/* Succeeded: whether every word held. */
	return ready;
}

/*
 * Brings up every combo PHY of the platform that does not read as
 * brought up.  Returns the mask of the PHYs it wrote.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_combo_phys_init).
 */
unsigned
drv_i915_xd_combo_phy_init(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	unsigned written;
	unsigned phy;
	int has;
	int ready;

	/* Each combo PHY, first to last; one that is brought up is left alone. */
	written = 0U;
	for (phy = 0U; phy < 8U; phy++) {
		/* A PHY the platform does not have, or one already brought up, is passed over. */
		has = i915_xd_combo_has(platform->combo_phys, phy);
		if (!has)
			continue;
		ready = drv_i915_xd_combo_phy_ready(io, platform, phy);
		if (ready)
			continue;

		/* Brings the PHY up. */
		i915_xd_combo_init_one(io, platform, phy);
		written |= 1U << phy;
	}

	/* Succeeded: the PHYs written. */
	return written;
}

/*
 * Stops every combo PHY of the platform, last to first: the IO
 * compensation powered down where the PHY has the word, the compensation
 * stopped.  Returns 1 when PHY A did not read as brought up before (the
 * caller reports it; old firmware leaves it so on some platforms).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_combo_phys_uninit).
 */
unsigned
drv_i915_xd_combo_phy_uninit(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	unsigned phy;
	unsigned a_not_ready;
	int has;
	int ready;
	uint32_t base;

	/* Each combo PHY, last to first. */
	a_not_ready = 0U;
	for (phy = 8U; phy-- > 0U;) {
		/* A PHY the platform does not have is passed over. */
		has = i915_xd_combo_has(platform->combo_phys, phy);
		if (!has)
			continue;

		/* PHY A is expected to read as brought up. */
		if (phy == 0U) {
			ready = drv_i915_xd_combo_phy_ready(io, platform, phy);
			if (!ready)
				a_not_ready = 1U;
		}

		/* Powers the compensation down, then stops it. */
		has = i915_xd_combo_has(platform->phy_misc, phy);
		if (has)
			(void)drv_i915_xd_io_rmw(io, I915_XD_PHY_MISC(phy), 0U, I915_XD_PHY_MISC_IO_COMP_POWER_DOWN);
		base = i915_xd_combo_base(phy);
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_COMP_DW(base, 0U), I915_XD_COMP_DW0_INIT, 0U);
	}

	/* Succeeded: whether PHY A was down before. */
	return a_not_ready;
}

/*
 * Powers up the lanes a port's link uses on its combo PHY: one or two
 * lanes keep the others down (from the other end when the lanes are
 * reversed); four, or a count the PHY does not know, power up every lane.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (intel_combo_phy_power_up_lanes).
 */
void
drv_i915_xd_combo_lanes(
	struct i915_xd_io *io,
	unsigned phy,
	unsigned lanes,
	int reversed)
{
	uint32_t down;

	/* The lanes kept down. */
	down = 0U;
	if (lanes == 1U)
		down = reversed ? I915_XD_LANES_1_REVERSED : I915_XD_LANES_1;
	else if (lanes == 2U)
		down = reversed ? I915_XD_LANES_2_REVERSED : I915_XD_LANES_2;

	/* Writes the field. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(i915_xd_combo_base(phy), 10U), I915_XD_CL_DW10_LANE_POWER_DOWN,
		i915_xd_field_put(I915_XD_CL_DW10_LANE_POWER_DOWN, down));
}

/*
 * Chooses the swing table of a combo port: HDMI's; for eDP above 5.4 GHz
 * the eDP high rate table, with the VBT's HOBL setting the HOBL table
 * (on a platform that has one), with its low swing the eDP low table,
 * otherwise as DisplayPort; for
 * DisplayPort the table above 2.7 GHz or the one up to it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (tgl_get_combo_buf_trans, tgl_get_combo_buf_trans_edp,
 * tgl_get_combo_buf_trans_dp, adlp_get_combo_buf_trans,
 * adlp_get_combo_buf_trans_edp, adlp_get_combo_buf_trans_dp,
 * icl_get_combo_buf_trans_edp, use_edp_hobl, use_edp_low_vswing).
 */
const struct i915_xd_combo_swing_table *
drv_i915_xd_combo_swing_table(
	const struct i915_xd_platform *platform,
	unsigned kind,
	uint32_t rate_khz,
	unsigned vbt)
{
	const struct i915_xd_swing_set *set;

	/* HDMI has its own table. */
	set = platform->swing;
	if (kind == I915_XD_OUTPUT_HDMI)
		return set->hdmi;

	/* eDP by rate and the VBT. */
	if (kind == I915_XD_OUTPUT_EDP) {
		if (rate_khz > 540000U)
			return set->edp_hbr3;
		if ((vbt & I915_XD_SWING_HOBL) != 0U && set->edp_hobl != NULL)
			return set->edp_hobl;
		if ((vbt & I915_XD_SWING_LOW) != 0U)
			return set->edp_low_swing;
	}

	/* DisplayPort by rate. */
	if (rate_khz > 270000U)
		return set->dp_high;

	/* Succeeded: up to 2.7 GHz. */
	return set->dp_low;
}

/* Tells whether a swing table is the platform's eDP HOBL table. */
int
drv_i915_xd_combo_swing_is_hobl(
	const struct i915_xd_platform *platform,
	const struct i915_xd_combo_swing_table *table)
{
	/* Only that table. */
	if (table != platform->swing->edp_hobl)
		return 0;

	/* Succeeded: the HOBL table. */
	return 1;
}

/*
 * Finds the swing table row of a DisplayPort lane's level byte.  The rows
 * of a DisplayPort or eDP table follow the swing and pre-emphasis pairs a
 * sink may ask for, by swing and then by pre-emphasis, the two adding up
 * to 3 at most.  Returns 0 with the row, or EINVAL for a pair no table
 * has (row 0).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (index_to_dp_signal_levels).
 */
int
drv_i915_xd_combo_level_row(
	uint8_t level,
	unsigned *row)
{
	/*
	 * The first row of each swing level: four pre-emphasis levels at
	 * swing 0, three at swing 1, two at swing 2 and one at swing 3.
	 */
	static const uint8_t first_row[4] = { 0U, 4U, 7U, 9U };
	unsigned swing;
	unsigned preemph;

	/* The two levels of the byte. */
	swing = (unsigned)(level & 0x3U);
	preemph = (unsigned)((level >> 3) & 0x3U);
	*row = 0U;

	/* A pair beyond the table. */
	if (swing + preemph > 3U)
		return EINVAL;

	/* Succeeded: the pair's row. */
	*row = first_row[swing] + preemph;
	return 0;
}

/*
 * Returns the highest swing level a DisplayPort or eDP table of so many
 * rows reaches (a combo or a Dekel table): that of its last row (a table
 * of more rows than pairs is taken as every pair, one of none as the
 * first).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_dp_voltage_max).
 */
uint8_t
drv_i915_xd_dp_swing_max(
	unsigned rows)
{
	unsigned last;

	/* The last row, within the pairs. */
	last = 0U;
	if (rows > 0U)
		last = rows - 1U;
	if (last > 9U)
		last = 9U;

	/* The swing of the row: rows 9, 7 to 8, 4 to 6 and 0 to 3. */
	if (last >= 9U)
		return 3U;
	if (last >= 7U)
		return 2U;
	if (last >= 4U)
		return 1U;

	/* Succeeded: the lowest swing. */
	return 0U;
}

/*
 * Programs the signal levels of a combo port: the common keeper (on for
 * DisplayPort and eDP, off for HDMI), the load generator of each lane,
 * the SUS clock, the training enable cleared, the swing of each lane from
 * its table row, and the training enable set to latch it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_combo_phy_set_signal_levels).
 */
void
drv_i915_xd_combo_signal_levels(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_combo_levels *levels)
{
	uint32_t base;
	uint32_t value;
	unsigned lane;

	/* The common keeper, read from lane 0 and written to the group. */
	base = i915_xd_combo_base(levels->phy);
	value = drv_i915_xd_io_read(io, I915_XD_PORT_PCS_DW_LN(base, 0U, 1U));
	if (levels->kind == I915_XD_OUTPUT_HDMI)
		value &= ~I915_XD_PCS_DW1_COMMON_KEEPER;
	else
		value |= I915_XD_PCS_DW1_COMMON_KEEPER;
	drv_i915_xd_io_write(io, I915_XD_PORT_PCS_DW_GRP(base, 1U), value);

	/* The load generator of each lane (the group word would overwrite them). */
	for (lane = 0U; lane < 4U; lane++)
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_TX_DW_LN(base, lane, 4U), I915_XD_TX_DW4_LOADGEN, i915_xd_combo_loadgen(levels, lane));

	/* The SUS clock, then the training enable cleared. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, 5U), 0U, I915_XD_CL_DW5_SUS_CLOCK_CONFIG);
	value = drv_i915_xd_io_read(io, I915_XD_PORT_TX_DW_LN(base, 0U, 5U));
	drv_i915_xd_io_write(io, I915_XD_PORT_TX_DW_GRP(base, 5U), value & ~I915_XD_TX_DW5_TRAINING);

	/* The swing. */
	i915_xd_combo_swing(io, platform, levels, base);

	/* The training enable set, which latches the swing. */
	value = drv_i915_xd_io_read(io, I915_XD_PORT_TX_DW_LN(base, 0U, 5U));
	drv_i915_xd_io_write(io, I915_XD_PORT_TX_DW_GRP(base, 5U), value | I915_XD_TX_DW5_TRAINING);
}

/*
 * Reports the DDI buffer translation table a display 9 output uses: the
 * part's DisplayPort, eDP or HDMI table, cut to nine rows for DisplayPort
 * and eDP on a DDI other than A.  The rows it may use are in *count.
 * Returns NULL for a platform without display 9 tables or another kind of
 * output.
 */
const struct i915_xd9_buf_trans *
drv_i915_xd9_ddi_buf_table(
	const struct i915_xd_platform *platform,
	unsigned output,
	unsigned ddi,
	unsigned *count)
{
	const struct i915_xd9_buf_table *table;

	/* Nothing until the table is known. */
	*count = 0U;
	if (platform->ddi_buffers == NULL)
		return NULL;

	/* The table of the output's kind. */
	if (output == I915_XD9_OUTPUT_DP) {
		table = &platform->ddi_buffers->dp;
	} else if (output == I915_XD9_OUTPUT_EDP) {
		table = &platform->ddi_buffers->edp;
	} else if (output == I915_XD9_OUTPUT_HDMI) {
		table = &platform->ddi_buffers->hdmi;
	} else {
		return NULL;
	}

	/* Every row of an HDMI table, or a DDI A table. */
	*count = table->count;

	/* DisplayPort and eDP on another DDI stop at the ninth row. */
	if (output != I915_XD9_OUTPUT_HDMI &&
	    ddi != 0U &&
	    *count > I915_XD9_DDI_ROWS_NOT_A)
		*count = I915_XD9_DDI_ROWS_NOT_A;

	/* Succeeded: the table and the rows the output may use. */
	return table->rows;
}

/*
 * Reports the HDMI row a display 9 part's ports use when the VBT names
 * none.
 */
unsigned
drv_i915_xd9_ddi_hdmi_row(
	const struct i915_xd_platform *platform)
{
	/* A platform without display 9 tables has no HDMI row but the first. */
	if (platform->ddi_buffers == NULL)
		return 0U;

	/* Succeeded: the table's default row. */
	return platform->ddi_buffers->hdmi_default;
}

/*
 * Writes a display 9 DDI's buffer translation rows from a table.
 */
void
drv_i915_xd9_ddi_buffers(
	struct i915_xd_io *io,
	unsigned ddi,
	const struct i915_xd9_buf_trans *table,
	unsigned rows)
{
	unsigned row;

	/* Each row's two words. */
	for (row = 0U; row < rows; row++) {
		drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_TRANS(ddi, row), table[row].trans1);
		drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_TRANS(ddi, row) + 4U, table[row].trans2);
	}
}

/*
 * Drives a translation row on a display 9 DDI: the row's current boost,
 * then the row selected in the buffer's word, which is written and pushed
 * out.
 * A row past the table drives the last one.
 */
void
drv_i915_xd9_ddi_levels(
	struct i915_xd_io *io,
	unsigned ddi,
	const struct i915_xd9_buf_trans *table,
	unsigned rows,
	unsigned row,
	uint32_t *buf_word)
{
	/* The last row at most. */
	if (row >= rows)
		row = rows - 1U;

	/* The row's boost. */
	drv_i915_xd9_ddi_boost(io, ddi, table[row].iboost);

	/* The row in the buffer's word. */
	*buf_word &= ~I915_XD9_DDI_BUF_ROW_MASK;
	*buf_word |= (uint32_t)row << I915_XD9_DDI_BUF_ROW_SHIFT;
	drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_CTL(ddi), *buf_word);
	drv_i915_xd_io_posting_read(io, I915_XD9_DDI_BUF_CTL(ddi));
}

/*
 * Stops a display 9 DDI's buffer and its transport, and waits for a
 * buffer that was on to go idle.
 */
void
drv_i915_xd9_ddi_buffer_off(
	struct i915_xd_io *io,
	unsigned ddi)
{
	uint32_t buffer;
	int was_on;

	/* The buffer off. */
	was_on = 0;
	buffer = drv_i915_xd_io_read(io, I915_XD9_DDI_BUF_CTL(ddi));
	if ((buffer & I915_XD9_DDI_BUF_ENABLE) != 0U) {
		drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_CTL(ddi), buffer & ~I915_XD9_DDI_BUF_ENABLE);
		was_on = 1;
	}

	/* The transport off. */
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DP_TP_CTL(ddi), I915_XD9_DP_TP_ENABLE, 0U);
	drv_i915_xd_io_posting_read(io, I915_XD9_DP_TP_CTL(ddi));

	/* Succeeded: a buffer that was on goes idle. */
	if (was_on)
		(void)drv_i915_xd_io_poll(io, I915_XD9_DDI_BUF_CTL(ddi), I915_XD9_DDI_BUF_IDLE, I915_XD9_DDI_BUF_IDLE, I915_XD9_DDI_IDLE_US, NULL);
}

/*
 * Trains a display 9 DisplayPort link at a rate and lane count and leaves
 * it carrying the stream.
 *
 * The training is the xd code's (xd-dp.c), with the DDI as its source: the
 * transport in pattern 1 and the buffer started before the sink is set
 * up, the levels of the first lane on every lane through the buffer's
 * translation rows, the patterns in the transport, and the idle patterns
 * after it.  The port's word must hold the DDI's width and kept bits.
 * Returns 0, or EIO when the link did not train (the DDI is left
 * training, for the caller to stop).
 */
int
drv_i915_xd9_dp_train(
	struct i915_xd9_dp_port *port,
	uint32_t rate_khz,
	unsigned lanes)
{
	struct i915_xd_dp_link link;
	int error;

	/* The training. */
	i915_xd9_dp_link(port, rate_khz, lanes, &link);
	error = drv_i915_xd_dp_train(&link);
	port->set = link.set[0];
	if (error != 0)
		return EIO;

	/* Succeeded: the link leaves the training and carries the stream. */
	drv_i915_xd_dp_train_end(&link);
	return 0;
}

/*
 * Sets a display 9 sink's power state; D0 is written up to three times a
 * millisecond apart while the sink wakes.  A sink before DPCD 1.1 has
 * none.  Returns 0, or the error of the last write.
 */
int
drv_i915_xd9_dp_sink_power(
	struct i915_xd9_dp_port *port,
	uint8_t state)
{
	unsigned tries;
	int error;

	/* A sink before DPCD 1.1 has no power state. */
	if (port->caps[I915_XD_DPCD_REV] < I915_XD9_DPCD_REV_1_1)
		return 0;

	/* The write, again while the sink wakes. */
	error = EIO;
	for (tries = 0U; tries < I915_XD9_WAKE_TRIES; tries++) {
		error = drv_i915_xd_dpcd_write(&port->aux, port->io, I915_XD_DPCD_SET_POWER, &state, 1U);
		if (error == 0)
			break;
		(void)drv_i915_xd_io_delay(port->io, I915_XD9_WAKE_US);
	}

	/* A sink that never took it. */
	if (error != 0)
		return error;

	/* Succeeded: the sink is in the state. */
	return 0;
}

/*
 * Starts a display 9 HDMI port's buffer: the HDMI row of the table
 * written as the DDI's tenth translation row and its boost, the training
 * override of the DDI's transcoder chicken register on for a microsecond,
 * then the buffer on with its lane reversal kept, and the time it takes
 * to leave idle.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_enable_ddi_hdmi, Display WA #1143).
 */
void
drv_i915_xd9_ddi_hdmi_start(
	struct i915_xd_io *io,
	unsigned ddi,
	const struct i915_xd9_buf_trans *row)
{
	uint32_t chicken;
	uint32_t buffer;

	/* The row as the tenth, and its boost. */
	drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_TRANS(ddi, I915_XD9_DDI_HDMI_ROW), row->trans1);
	drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_TRANS(ddi, I915_XD9_DDI_HDMI_ROW) + 4U, row->trans2);
	drv_i915_xd9_ddi_boost(io, ddi, row->iboost);

	/* The training override on for a microsecond. */
	chicken = drv_i915_xd_io_rmw(io, I915_XD9_CHICKEN_TRANS(ddi - 1U), 0U, I915_XD9_TRAINING_OVERRIDE);
	drv_i915_xd_io_posting_read(io, I915_XD9_CHICKEN_TRANS(ddi - 1U));
	(void)drv_i915_xd_io_delay(io, 1U);
	drv_i915_xd_io_write(io, I915_XD9_CHICKEN_TRANS(ddi - 1U), chicken & ~I915_XD9_TRAINING_OVERRIDE);

	/* Succeeded: the buffer on, out of idle. */
	buffer = drv_i915_xd_io_read(io, I915_XD9_DDI_BUF_CTL(ddi));
	drv_i915_xd_io_write(io, I915_XD9_DDI_BUF_CTL(ddi), (buffer & I915_XD9_DDI_BUF_REVERSAL) | I915_XD9_DDI_BUF_ENABLE);
	(void)drv_i915_xd_io_delay(io, I915_XD9_DDI_ACTIVE_US);
}

/*
 * Sets a display 9 DDI's current boost: the balance leg off for a boost
 * of 0, or set to a boost of 1, 3 or 7; another value leaves the leg as
 * it is.
 */
void
drv_i915_xd9_ddi_boost(
	struct i915_xd_io *io,
	unsigned ddi,
	unsigned boost)
{
	/* No boost: the leg off. */
	if (boost == 0U) {
		(void)drv_i915_xd_io_rmw(io, I915_XD9_BMU_CR0, I915_XD9_BALANCE_LEG_MASK(ddi), I915_XD9_BALANCE_LEG_DISABLE(ddi));
		return;
	}

	/* A boost the leg does not take. */
	if (boost != I915_XD9_BOOST_1 && boost != I915_XD9_BOOST_3 && boost != I915_XD9_BOOST_7)
		return;

	/* Succeeded: the leg on with the boost. */
	(void)drv_i915_xd_io_rmw(io,
				 I915_XD9_BMU_CR0,
				 I915_XD9_BALANCE_LEG_MASK(ddi) | I915_XD9_BALANCE_LEG_DISABLE(ddi),
				 boost << I915_XD9_BALANCE_LEG_SHIFT(ddi));
}

/*
 * Reports the register block of a combo PHY.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy_regs.h (_ICL_COMBOPHY_A,
 * _ICL_COMBOPHY_B, _EHL_COMBOPHY_C, _RKL_COMBOPHY_D, _ADL_COMBOPHY_E).
 */
static uint32_t
i915_xd_combo_base(
	unsigned phy)
{
	static const uint32_t bases[] = {
		I915_XD_COMBO_PHY_A,
		I915_XD_COMBO_PHY_B,
		I915_XD_COMBO_PHY_C,
		I915_XD_COMBO_PHY_D,
		I915_XD_COMBO_PHY_E
	};

	/* A PHY past the last has PHY A's block (no platform names one). */
	if (phy >= sizeof(bases) / sizeof(bases[0]))
		return bases[0];

	/* Succeeded: the PHY's block. */
	return bases[phy];
}

/* Tells whether a PHY is in a mask of PHYs of the platform. */
static int
i915_xd_combo_has(
	uint8_t mask,
	unsigned phy)
{
	/* One bit per PHY. */
	if (phy >= 8U)
		return 0;

	/* A PHY whose bit is clear is not there. */
	if ((mask & (1U << phy)) == 0U)
		return 0;

	/* Succeeded: the PHY's bit is set. */
	return 1;
}

/* Tells whether a word's bits under a mask read as a value (1 or 0). */
static int
i915_xd_combo_check(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	uint32_t word;

	/* Reads the word. */
	word = drv_i915_xd_io_read(io, offset);

	/* A word whose bits differ does not match. */
	if ((word & mask) != value)
		return 0;

	/* Succeeded: the bits read as the value. */
	return 1;
}

/*
 * Brings up one combo PHY: the IO compensation powered (where the PHY has
 * the word); on display 12 on the duty cycle corrector (read from lane 0,
 * written to the group); the process and voltage reference values; the
 * reference current on a PHY with the resistor; the compensation started
 * and the common lane allowed to power down.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_combo_phys_init, icl_get_procmon_ref_values,
 * icl_set_procmon_ref_values).
 */
static void
i915_xd_combo_init_one(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned phy)
{
	const struct i915_xd_procmon *procmon;
	uint32_t base;
	uint32_t value;
	int has;

	/* Powers the IO compensation. */
	base = i915_xd_combo_base(phy);
	has = i915_xd_combo_has(platform->phy_misc, phy);
	if (has) {
		value = drv_i915_xd_io_read(io, I915_XD_PHY_MISC(phy));
		drv_i915_xd_io_write(io, I915_XD_PHY_MISC(phy), value & ~I915_XD_PHY_MISC_IO_COMP_POWER_DOWN);
	}

	/* The duty cycle corrector: the ODCC clock divided by 2, the DCC run once. */
	if (platform->display_ver >= 12U) {
		value = drv_i915_xd_io_read(io, I915_XD_PORT_TX_DW_LN(base, 0U, 8U));
		value &= ~I915_XD_TX_DW8_ODCC_DIVIDER;
		value |= I915_XD_TX_DW8_ODCC_CLOCK_SELECT | i915_xd_field_put(I915_XD_TX_DW8_ODCC_DIVIDER, I915_XD_TX_DW8_ODCC_DIVIDE_BY_2);
		drv_i915_xd_io_write(io, I915_XD_PORT_TX_DW_GRP(base, 8U), value);

		/* The duty cycle corrector runs once. */
		value = drv_i915_xd_io_read(io, I915_XD_PORT_PCS_DW_LN(base, 0U, 1U));
		value &= ~I915_XD_PCS_DW1_DCC_MODE;
		value |= i915_xd_field_put(I915_XD_PCS_DW1_DCC_MODE, I915_XD_PCS_DW1_DCC_RUN_ONCE);
		drv_i915_xd_io_write(io, I915_XD_PORT_PCS_DW_GRP(base, 1U), value);
	}

	/* The reference values of the process and voltage the PHY reports. */
	procmon = drv_i915_xd_procmon(drv_i915_xd_io_read(io, I915_XD_PORT_COMP_DW(base, 3U)));
	(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_COMP_DW(base, 1U), I915_XD_COMP_DW1_PROCMON, procmon->dw1);
	drv_i915_xd_io_write(io, I915_XD_PORT_COMP_DW(base, 9U), procmon->dw9);
	drv_i915_xd_io_write(io, I915_XD_PORT_COMP_DW(base, 10U), procmon->dw10);

	/* The reference current, the compensation started, the common lane's power down. */
	has = i915_xd_combo_has(platform->comp_masters, phy);
	if (has)
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_COMP_DW(base, 8U), 0U, I915_XD_COMP_DW8_IREFGEN);
	(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_COMP_DW(base, 0U), 0U, I915_XD_COMP_DW0_INIT);
	(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, 5U), 0U, I915_XD_CL_DW5_POWER_DOWN_ENABLE);
}

/*
 * Reports the load generator bit of a lane: none above 6 GHz; with four
 * lanes on lanes 1 to 3; with fewer on lanes 1 and 2.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_combo_phy_loadgen_select).
 */
static uint32_t
i915_xd_combo_loadgen(
	const struct i915_xd_combo_levels *levels,
	unsigned lane)
{
	/* No lane above 6 GHz. */
	if (levels->rate_khz > I915_XD_COMBO_LOADGEN_KHZ)
		return 0U;

	/* Four lanes: all but lane 0. */
	if (levels->lanes == 4U)
		return lane >= 1U ? I915_XD_TX_DW4_LOADGEN : 0U;

	/* Fewer lanes: lanes 1 and 2. */
	if (lane == 1U || lane == 2U)
		return I915_XD_TX_DW4_LOADGEN;

	/* Succeeded: no load generator on the other lanes. */
	return 0U;
}

/*
 * Writes the swing of a combo port: for eDP the 4K2K mode override (on
 * with the HOBL table); the scaling mode, termination and taps of DW5 for
 * the group; then each lane's swing select and compensation scalar (DW2),
 * cursor coefficients (DW4) and N scalar (DW7) from its table row.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_ddi_combo_vswing_program).
 */
static void
i915_xd_combo_swing(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_combo_levels *levels,
	uint32_t base)
{
	const struct i915_xd_combo_swing *row;
	uint32_t value;
	uint32_t override;
	unsigned lane;
	int hobl;

	/* eDP: the 4K2K mode override follows the HOBL table. */
	if (levels->kind == I915_XD_OUTPUT_EDP) {
		override = I915_XD_CL_DW10_EDP4K2K_OVERRIDE | I915_XD_CL_DW10_EDP4K2K_OPTIMIZED;
		hobl = drv_i915_xd_combo_swing_is_hobl(platform, levels->table);
		if (hobl)
			(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, 10U), override, override);
		else
			(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, 10U), override, 0U);
	}

	/* DW5 for the group: the scaling mode, the termination, tap 3 off. */
	value = drv_i915_xd_io_read(io, I915_XD_PORT_TX_DW_LN(base, 0U, 5U));
	value &= ~(I915_XD_TX_DW5_SCALING_MODE | I915_XD_TX_DW5_RTERM | I915_XD_TX_DW5_TAP2_DISABLE | I915_XD_TX_DW5_TAP3_DISABLE);
	value |= i915_xd_field_put(I915_XD_TX_DW5_SCALING_MODE, I915_XD_COMBO_SCALING_MODE);
	value |= i915_xd_field_put(I915_XD_TX_DW5_RTERM, I915_XD_COMBO_RTERM);
	value |= I915_XD_TX_DW5_TAP3_DISABLE;
	drv_i915_xd_io_write(io, I915_XD_PORT_TX_DW_GRP(base, 5U), value);

	/* DW2 of each lane: the swing select (its bit 3 apart) and the scalar. */
	for (lane = 0U; lane < 4U; lane++) {
		row = &levels->table->rows[levels->level[lane]];
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_TX_DW_LN(base, lane, 2U),
			I915_XD_TX_DW2_SWING_UPPER | I915_XD_TX_DW2_SWING_LOWER | I915_XD_TX_DW2_RCOMP_SCALAR,
			i915_xd_field_put(I915_XD_TX_DW2_SWING_UPPER, (uint32_t)row->swing_select >> 3) |
			i915_xd_field_put(I915_XD_TX_DW2_SWING_LOWER, row->swing_select) |
			i915_xd_field_put(I915_XD_TX_DW2_RCOMP_SCALAR, I915_XD_COMBO_RCOMP_SCALAR));
	}

	/* DW4 of each lane: the cursor coefficients. */
	for (lane = 0U; lane < 4U; lane++) {
		row = &levels->table->rows[levels->level[lane]];
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_TX_DW_LN(base, lane, 4U),
			I915_XD_TX_DW4_POST_CURSOR_1 | I915_XD_TX_DW4_POST_CURSOR_2 | I915_XD_TX_DW4_CURSOR_COEFF,
			i915_xd_field_put(I915_XD_TX_DW4_POST_CURSOR_1, row->post_cursor_1) |
			i915_xd_field_put(I915_XD_TX_DW4_POST_CURSOR_2, row->post_cursor_2) |
			i915_xd_field_put(I915_XD_TX_DW4_CURSOR_COEFF, row->cursor_coeff));
	}

	/* DW7 of each lane: the N scalar. */
	for (lane = 0U; lane < 4U; lane++) {
		row = &levels->table->rows[levels->level[lane]];
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_TX_DW_LN(base, lane, 7U), I915_XD_TX_DW7_N_SCALAR,
			i915_xd_field_put(I915_XD_TX_DW7_N_SCALAR, row->n_scalar));
	}
}

/*
 * Works out the DisplayPort word of a DDI buffer (without the enable):
 * the bits the port keeps, the translation select 0, the port width; on
 * a display 13 Type-C port the PHY link rate of the link.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (ddi_buf_phy_link_rate), and drivers/gpu/drm/i915/i915_reg.h
 * (DDI_PORT_WIDTH).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_init_dp_buf_reg).
 */
uint32_t
drv_i915_xd_ddi_dp_word(
	const struct i915_xd_platform *platform,
	const struct i915_xd_ddi_link *link)
{
	static const uint32_t rates[][2] = {
		{ 162000U, 0U }, { 216000U, 4U }, { 243000U, 5U }, { 270000U, 1U },
		{ 324000U, 6U }, { 432000U, 7U }, { 540000U, 2U }, { 810000U, 3U }
	};
	uint32_t word;
	uint32_t width;
	unsigned index;

	/* The kept bits and the width (three lanes are written as four). */
	width = link->lanes == 3U ? 4U : link->lanes - 1U;
	word = (link->kept & ~I915_XD_DDI_BUF_TC_OWNED) | i915_xd_field_put(I915_XD_DDI_BUF_WIDTH, width);

	/* A display 13 Type-C port carries the PHY link rate (a rate it does not know as 1.62 GHz's). */
	if (link->tc && platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		for (index = 0U; index < sizeof(rates) / sizeof(rates[0]); index++) {
			if (rates[index][0] == link->rate_khz)
				word |= i915_xd_field_put(I915_XD_DDI_BUF_PHY_LINK_RATE, rates[index][1]);
		}
	}

	/* Succeeded: the word. */
	return word;
}

/*
 * Starts a DisplayPort link: a link already running is stopped (the
 * buffer, then the transport, then the wait for idle); the transport is
 * enabled in training pattern 1 (single stream with its enhanced framing,
 * or multi-stream); the caller's step before the buffer runs (a Type-C
 * PHY's workaround); the buffer is enabled with the word (a Type-C port
 * keeping the PHY ownership the buffer holds) and the wait for it to
 * leave idle runs.  Returns the waits that ran out.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_wait_ddi_buf_idle, intel_wait_ddi_buf_active: 8 us, 1 ms, 3 ms
 * on a Type-C PHY).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_prepare_link_retrain, intel_wait_ddi_buf_idle,
 * intel_wait_ddi_buf_active).
 */
unsigned
drv_i915_xd_ddi_dp_start(
	struct i915_xd_io *io,
	const struct i915_xd_ddi_link *link,
	uint32_t word,
	void (*before_buffer)(void *context),
	void *context)
{
	uint32_t transport;
	uint32_t buffer;
	unsigned late;
	int was_on;
	int waited;

	/* A running link stops first: the buffer, the transport, the wait for idle. */
	late = 0U;
	transport = drv_i915_xd_io_read(io, I915_XD_DP_TP_CTL(link->trans));
	if ((transport & I915_XD_DP_TP_ENABLE) != 0U) {
		was_on = 0;
		buffer = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(link->ddi));
		if ((buffer & I915_XD_DDI_BUF_ENABLE) != 0U) {
			drv_i915_xd_io_write(io, I915_XD_DDI_BUF_CTL(link->ddi), buffer & ~I915_XD_DDI_BUF_ENABLE);
			was_on = 1;
		}

		/* The transport goes off; a buffer that was on is waited for until idle. */
		drv_i915_xd_io_write(io, I915_XD_DP_TP_CTL(link->trans), transport & ~I915_XD_DP_TP_ENABLE);
		drv_i915_xd_io_posting_read(io, I915_XD_DP_TP_CTL(link->trans));
		if (was_on) {
			waited = drv_i915_xd_io_poll(
				io,
				I915_XD_DDI_BUF_CTL(link->ddi),
				I915_XD_DDI_BUF_IDLE,
				I915_XD_DDI_BUF_IDLE,
				I915_XD_DDI_IDLE_US,
				NULL);
			if (waited != 0)
				late |= I915_XD_DDI_LATE_IDLE;
		}
	}

	/* The transport in training pattern 1. */
	transport = I915_XD_DP_TP_ENABLE | i915_xd_field_put(I915_XD_DP_TP_PATTERN, I915_XD_DP_TP_PATTERN_1);
	if (link->mst)
		transport |= I915_XD_DP_TP_MST;
	else if (link->enhanced)
		transport |= I915_XD_DP_TP_ENHANCED_FRAMING;
	drv_i915_xd_io_write(io, I915_XD_DP_TP_CTL(link->trans), transport);
	drv_i915_xd_io_posting_read(io, I915_XD_DP_TP_CTL(link->trans));

	/* The caller's step before the buffer. */
	if (before_buffer != NULL)
		before_buffer(context);

	/* The buffer; a Type-C port's PHY ownership is what the buffer holds, whatever the word says. */
	buffer = word | I915_XD_DDI_BUF_ENABLE;
	if (link->tc) {
		buffer &= ~I915_XD_DDI_BUF_TC_OWNED;
		buffer |= drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(link->ddi)) & I915_XD_DDI_BUF_TC_OWNED;
	}

	/* Enables the buffer. */
	drv_i915_xd_io_write(io, I915_XD_DDI_BUF_CTL(link->ddi), buffer);
	drv_i915_xd_io_posting_read(io, I915_XD_DDI_BUF_CTL(link->ddi));

	/* Waits for the buffer to leave idle. */
	late |= i915_xd_ddi_wait_active(io, link);

	/* Succeeded: the waits that ran out. */
	return late;
}

/*
 * Selects a training pattern in a transcoder's transport (1, 2, 3, 4,
 * idle or normal, as I915_XD_DP_TP_PATTERN_*).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_set_link_train).
 */
void
drv_i915_xd_ddi_pattern(
	struct i915_xd_io *io,
	unsigned trans,
	unsigned pattern)
{
	uint32_t transport;

	/* Replaces the pattern field. */
	transport = drv_i915_xd_io_read(io, I915_XD_DP_TP_CTL(trans));
	transport &= ~I915_XD_DP_TP_PATTERN;
	transport |= i915_xd_field_put(I915_XD_DP_TP_PATTERN, pattern);
	drv_i915_xd_io_write(io, I915_XD_DP_TP_CTL(trans), transport);
}

/*
 * Selects the idle pattern and waits for the idle patterns to be sent.
 * Returns I915_XD_DDI_LATE_IDLE_PATTERNS when they were not.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_set_idle_link_train: 1 ms).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_set_idle_link_train).
 */
unsigned
drv_i915_xd_ddi_idle_patterns(
	struct i915_xd_io *io,
	unsigned trans)
{
	int error;

	/* The idle pattern, then the wait. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_DP_TP_CTL(trans), I915_XD_DP_TP_PATTERN,
		i915_xd_field_put(I915_XD_DP_TP_PATTERN, I915_XD_DP_TP_PATTERN_IDLE));
	error = drv_i915_xd_io_poll(io, I915_XD_DP_TP_STATUS(trans), I915_XD_DP_TP_STATUS_IDLE_DONE, I915_XD_DP_TP_STATUS_IDLE_DONE,
		I915_XD_DDI_IDLE_PATTERNS_US, NULL);
	if (error != 0)
		return I915_XD_DDI_LATE_IDLE_PATTERNS;

	/* Succeeded: the patterns were sent. */
	return 0U;
}

/*
 * Starts an HDMI link's buffer: the kept bits and the enable (a display
 * 13 Type-C port, which drives HDMI only in legacy mode, with the PHY
 * ownership), then the wait for it to leave idle.  Returns the waits that
 * ran out.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_enable_ddi_hdmi).
 */
unsigned
drv_i915_xd_ddi_hdmi_start(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_ddi_link *link)
{
	uint32_t buffer;
	unsigned late;

	/* The word. */
	buffer = link->kept | I915_XD_DDI_BUF_ENABLE;
	if (link->tc && platform->tc_flow == I915_XD_TC_FLOW_TCSS)
		buffer |= I915_XD_DDI_BUF_TC_OWNED;
	drv_i915_xd_io_write(io, I915_XD_DDI_BUF_CTL(link->ddi), buffer);

	/* Waits for the buffer to leave idle. */
	late = i915_xd_ddi_wait_active(io, link);

	/* Succeeded: the buffer is on; the wait that ran out, if any. */
	return late;
}

/*
 * Stops a link's buffer: the buffer disabled when it is enabled; for
 * DisplayPort the transport disabled; FEC off when the link used it (with
 * a posting read); then the wait for the buffer to go idle, when it was
 * enabled.  Returns the waits that ran out.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (disable_ddi_buf, intel_ddi_disable_fec, intel_wait_ddi_buf_idle).
 */
unsigned
drv_i915_xd_ddi_stop(
	struct i915_xd_io *io,
	const struct i915_xd_ddi_link *link,
	int dp)
{
	uint32_t buffer;
	int was_on;
	int waited;

	/* The buffer. */
	was_on = 0;
	buffer = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(link->ddi));
	if ((buffer & I915_XD_DDI_BUF_ENABLE) != 0U) {
		drv_i915_xd_io_write(io, I915_XD_DDI_BUF_CTL(link->ddi), buffer & ~I915_XD_DDI_BUF_ENABLE);
		was_on = 1;
	}

	/* The transport and FEC. */
	if (dp)
		(void)drv_i915_xd_io_rmw(io, I915_XD_DP_TP_CTL(link->trans), I915_XD_DP_TP_ENABLE, 0U);
	if (link->fec) {
		(void)drv_i915_xd_io_rmw(io, I915_XD_DP_TP_CTL(link->trans), I915_XD_DP_TP_FEC, 0U);
		drv_i915_xd_io_posting_read(io, I915_XD_DP_TP_CTL(link->trans));
	}

	/* A buffer that was on is waited for until idle. */
	if (was_on) {
		waited = drv_i915_xd_io_poll(
			io,
			I915_XD_DDI_BUF_CTL(link->ddi),
			I915_XD_DDI_BUF_IDLE,
			I915_XD_DDI_BUF_IDLE,
			I915_XD_DDI_IDLE_US,
			NULL);
		if (waited != 0)
			return I915_XD_DDI_LATE_IDLE;
	}

	/* Succeeded: no wait ran out. */
	return 0U;
}

/* Waits for a DDI buffer to leave idle (longer on a Type-C PHY); returns I915_XD_DDI_LATE_ACTIVE when it did not. */
static unsigned
i915_xd_ddi_wait_active(
	struct i915_xd_io *io,
	const struct i915_xd_ddi_link *link)
{
	unsigned timeout_us;
	int error;

	/* The time a Type-C or a combo buffer is given. */
	timeout_us = link->tc ? I915_XD_DDI_ACTIVE_TC_US : I915_XD_DDI_ACTIVE_US;
	error = drv_i915_xd_io_poll(io, I915_XD_DDI_BUF_CTL(link->ddi), I915_XD_DDI_BUF_IDLE, 0U, timeout_us, NULL);
	if (error != 0)
		return I915_XD_DDI_LATE_ACTIVE;

	/* Succeeded: the buffer is active. */
	return 0U;
}

/* Fills the xd code's link of a display 9 port: its limits, the sink over its AUX channel, the DDI as its source. */
static void
i915_xd9_dp_link(
	struct i915_xd9_dp_port *port,
	uint32_t rate_khz,
	unsigned lanes,
	struct i915_xd_dp_link *link)
{
	kern_memset(link, 0, sizeof(*link));

	/* The link: the rate by its code, the lanes, enhanced framing when the sink takes it. */
	link->rate_khz = rate_khz;
	link->lanes = lanes;
	link->rate_index = -1;
	if ((port->caps[I915_XD_DPCD_MAX_LANES] & I915_XD9_DPCD_ENHANCED_CAP) != 0U)
		link->enhanced = 1;

	/* The source: the swing its table reaches, one level on every lane, pattern 3 but not 4, no repeaters. */
	link->swing_max = drv_i915_xd_dp_swing_max(port->rows);
	link->preemph_max = 3U;
	link->per_lane = 0;
	link->tps3 = 1;
	link->tps4 = 0;
	link->find_repeaters = 0;
	link->caps = port->caps;
	link->repeater_caps = port->repeater_caps;
	link->repeater_phy_caps = port->repeater_phy_caps;

	/* The sink over the DDI's AUX channel. */
	link->sink.read = i915_xd9_sink_read;
	link->sink.write = i915_xd9_sink_write;
	link->sink.probe = i915_xd9_sink_probe;
	link->sink.read_caps = i915_xd9_sink_caps;
	link->sink.context = port;

	/* Succeeded: the DDI as the source. */
	link->source.levels = i915_xd9_dp_levels;
	link->source.pattern = i915_xd9_dp_pattern;
	link->source.prepare = i915_xd9_dp_prepare;
	link->source.idle = i915_xd9_dp_idle;
	link->source.sleep = i915_xd9_dp_sleep;
	link->source.note = NULL;
	link->source.context = port;
}

/* Drives the first lane's levels on the DDI: the translation row of its swing and pre-emphasis. */
static void
i915_xd9_dp_levels(
	void *context,
	const uint8_t set[4])
{
	struct i915_xd9_dp_port *port;
	unsigned row;
	int error;

	/* The port the hook belongs to. */
	port = context;

	/* The row of the levels; one no table has is the first. */
	error = drv_i915_xd_combo_level_row(set[0], &row);
	if (error != 0)
		row = 0U;

	/* Succeeded: the row on the DDI. */
	drv_i915_xd9_ddi_levels(port->io, port->ddi, port->table, port->rows, row, &port->buf_word);
}

/* Selects a training pattern on the DDI's transport. */
static void
i915_xd9_dp_pattern(
	void *context,
	uint8_t pattern)
{
	struct i915_xd9_dp_port *port;
	uint32_t select;

	/* The port the hook belongs to. */
	port = context;

	/* The transport's field of the DPCD's pattern (an unknown one is pattern 1). */
	switch (pattern & I915_XD9_PATTERN_FIELD) {
	case I915_XD_DPCD_PATTERN_NONE:
		select = I915_XD9_DP_TP_PATTERN_NORMAL;
		break;
	case I915_XD_DPCD_PATTERN_2:
		select = I915_XD9_DP_TP_PATTERN_2;
		break;
	case I915_XD_DPCD_PATTERN_3:
		select = I915_XD9_DP_TP_PATTERN_3;
		break;
	default:
		select = I915_XD9_DP_TP_PATTERN_1;
		break;
	}

	/* Succeeded: the field, the rest kept. */
	(void)drv_i915_xd_io_rmw(port->io, I915_XD9_DP_TP_CTL(port->ddi), I915_XD9_DP_TP_PATTERN_MASK, select);
}

/*
 * Starts the DDI in training pattern 1: a transport still running stops
 * first with its buffer, then the transport in pattern 1 (with enhanced
 * framing when the sink takes it) and the buffer on, with the time it
 * takes to leave idle.
 */
static void
i915_xd9_dp_prepare(
	void *context)
{
	struct i915_xd9_dp_port *port;
	uint32_t transport;

	/* The port the hook belongs to. */
	port = context;

	/* A running transport stops first. */
	transport = drv_i915_xd_io_read(port->io, I915_XD9_DP_TP_CTL(port->ddi));
	if ((transport & I915_XD9_DP_TP_ENABLE) != 0U)
		drv_i915_xd9_ddi_buffer_off(port->io, port->ddi);

	/* The transport in pattern 1. */
	transport = I915_XD9_DP_TP_ENABLE | I915_XD9_DP_TP_PATTERN_1;
	if ((port->caps[I915_XD_DPCD_MAX_LANES] & I915_XD9_DPCD_ENHANCED_CAP) != 0U)
		transport |= I915_XD9_DP_TP_ENHANCED;
	drv_i915_xd_io_write(port->io, I915_XD9_DP_TP_CTL(port->ddi), transport);
	drv_i915_xd_io_posting_read(port->io, I915_XD9_DP_TP_CTL(port->ddi));

	/* Succeeded: the buffer on, out of idle. */
	port->buf_word |= I915_XD9_DDI_BUF_ENABLE;
	drv_i915_xd_io_write(port->io, I915_XD9_DDI_BUF_CTL(port->ddi), port->buf_word);
	drv_i915_xd_io_posting_read(port->io, I915_XD9_DDI_BUF_CTL(port->ddi));
	(void)drv_i915_xd_io_delay(port->io, I915_XD9_DDI_ACTIVE_US);
}

/* Sends the idle patterns after the training; DDI A does not wait for them. */
static void
i915_xd9_dp_idle(
	void *context)
{
	struct i915_xd9_dp_port *port;

	/* The port the hook belongs to. */
	port = context;

	/* The idle pattern. */
	(void)drv_i915_xd_io_rmw(port->io, I915_XD9_DP_TP_CTL(port->ddi), I915_XD9_DP_TP_PATTERN_MASK, I915_XD9_DP_TP_PATTERN_IDLE);
	if (port->ddi == 0U)
		return;

	/* Succeeded: the patterns are out. */
	(void)drv_i915_xd_io_poll(port->io, I915_XD9_DP_TP_STATUS(port->ddi), I915_XD9_DP_TP_STATUS_IDLE_DONE, I915_XD9_DP_TP_STATUS_IDLE_DONE, I915_XD9_DDI_IDLE_PATTERNS_US, NULL);
}

/* Lets the time between two steps of the training pass. */
static void
i915_xd9_dp_sleep(
	void *context,
	unsigned min_us,
	unsigned max_us)
{
	struct i915_xd9_dp_port *port;

	UNUSED_PARAMETER(max_us);

	/* The port the hook belongs to. */
	port = context;

	/* Succeeded: the shortest time asked. */
	(void)drv_i915_xd_io_delay(port->io, min_us);
}

/* Reads DPCD bytes for the training: the bytes read, or a negative error. */
static int
i915_xd9_sink_read(
	void *context,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd9_dp_port *port;
	int error;

	/* The port the hook belongs to. */
	port = context;

	/* The read. */
	error = drv_i915_xd_dpcd_read(&port->aux, port->io, address, buffer, size);
	if (error != 0)
		return -error;

	/* Succeeded: every byte. */
	return (int)size;
}

/* Writes DPCD bytes for the training: the bytes written, or a negative error. */
static int
i915_xd9_sink_write(
	void *context,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	struct i915_xd9_dp_port *port;
	int error;

	/* The port the hook belongs to. */
	port = context;

	/* The write. */
	error = drv_i915_xd_dpcd_write(&port->aux, port->io, address, buffer, size);
	if (error != 0)
		return -error;

	/* Succeeded: every byte. */
	return (int)size;
}

/* Wakes the sink with a read it throws away: 0, or a negative error. */
static int
i915_xd9_sink_probe(
	void *context,
	uint32_t address)
{
	uint8_t thrown;
	int moved;

	/* Succeeded or not, the outcome of a one-byte read. */
	moved = i915_xd9_sink_read(context, address, &thrown, 1U);
	if (moved < 0)
		return moved;

	/* Succeeded: the sink answered. */
	return 0;
}

/* Reads the sink's receiver capabilities: 0, or a negative error. */
static int
i915_xd9_sink_caps(
	void *context,
	uint8_t *caps)
{
	int moved;

	/* The capability bytes. */
	moved = i915_xd9_sink_read(context, I915_XD_DPCD_REV, caps, I915_XD_DPCD_CAPS_SIZE);
	if (moved < 0)
		return moved;

	/* Succeeded: the capabilities. */
	return 0;
}
