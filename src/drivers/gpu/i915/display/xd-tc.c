/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Type-C PHYs of the display 11 to 13 code (see xd-tc.h): the
 * Dekel PHY windows, the lane programming of a Type-C port in a Dekel or
 * an MG PHY, and the connection of a Type-C port.
 */

#include "display-internal.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "xd-tc.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd-south.h"

#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* How long one poll of the microcontroller's health waits. */
#define I915_XD_DKL_POLL_US		10U

/*
 * The load generator selects of the two transmitters of each lane on the
 * platforms that select them: HDMI drives lane 0 with 0 and 2 and lane 1
 * with 3 and 3; DisplayPort uses 0 everywhere.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (tgl_dkl_phy_set_signal_levels: the DKL_TX_DPCNTL2_CFG_LOADGENSELECT_TX1
 * and DKL_TX_DPCNTL2_CFG_LOADGENSELECT_TX2 values).
 */
#define I915_XD_DKL_HDMI_LOADGEN_LN0_TX1	0U
#define I915_XD_DKL_HDMI_LOADGEN_LN0_TX2	2U
#define I915_XD_DKL_HDMI_LOADGEN_LN1_TX1	3U
#define I915_XD_DKL_HDMI_LOADGEN_LN1_TX2	3U

/* The rate above which a DisplayPort link takes the high-rate swing table. */
#define I915_XD_DKL_SWING_LOW_MAX_KHZ	270000U

/*
 * The link clocks an MG PHY's lanes follow: below 300 MHz the clock hub
 * keeps the low-rate lock enable, above 500 MHz the duty-cycle
 * correction takes the AMI clock divider override of 1.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_mg_phy_set_signal_levels: 300000, 500000,
 * CFG_AMI_CK_DIV_OVERRIDE_VAL(1)).
 */
#define I915_XD_MG_LOW_RATE_KHZ		300000U
#define I915_XD_MG_DCC_OVERRIDE_KHZ	500000U
#define I915_XD_MG_DCC_DIVIDER		1U

static void i915_xd_dkl_lock(struct i915_xd_dkl *dkl);
static void i915_xd_dkl_unlock(struct i915_xd_dkl *dkl);
static uint32_t i915_xd_dkl_select(struct i915_xd_dkl *dkl, unsigned tc, uint32_t phy_offset);
static uint32_t i915_xd_tc_swing_word(const struct i915_xd_dkl_swing_table *table, uint8_t level);
static uint32_t i915_xd_tc_loadgen(int hdmi, unsigned lane);
static int i915_xd_tc_connect_tcss(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, unsigned aux, unsigned mode, unsigned lanes, const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);
static int i915_xd_tc_connect_fia(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, unsigned aux, unsigned mode, unsigned lanes, const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);
static int i915_xd_tc_block(const struct i915_xd_platform *platform, unsigned aux, unsigned mode, const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);
static void i915_xd_tc_unblock(const struct i915_xd_tc_power *power, struct i915_xd_tc_connection *connection);
static int i915_xd_tc_dp_modes(unsigned pin_assignment, unsigned lanes, uint32_t *mode0_out, uint32_t *mode1_out);
static const struct i915_xd_mg_swing *i915_xd_tc_mg_row(const struct i915_xd_mg_swing_table *table, uint8_t level);
static int i915_xd_tc_check(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port, unsigned mode, unsigned lanes, struct i915_xd_tc_connection *connection);

/*
 * Prepares the Dekel PHY windows of a display: the register window they
 * are reached through and the lock the ports share.
 */
void
drv_i915_xd_dkl_init(
	struct i915_xd_dkl *dkl,
	struct i915_xd_io *io)
{
	/* The window and the lock of the bank index registers. */
	dkl->io = io;
	spin_init(&dkl->lock, LOCK_RANK_DEVICE, "i915 xd dkl");
	dkl->hooks.read = NULL;
	dkl->hooks.write = NULL;
	dkl->hooks.rmw = NULL;
	dkl->hooks.context = NULL;

	/* The windows may be used from here on. */
	dkl->ready = 1;
}

/*
 * Gives the PHY accesses another route (a host test's model, or the
 * older modeset's recorded writes).  The hooks' owner serializes them.
 */
void
drv_i915_xd_dkl_set_hooks(
	struct i915_xd_dkl *dkl,
	const struct i915_xd_dkl_hooks *hooks)
{
	/* Takes the route. */
	dkl->hooks = *hooks;
}

/*
 * Works out where a PHY register of a Type-C port is reached: returns the
 * MMIO register in the port's window, and stores the bank index register
 * and the whole value to write to it first.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h
 * (_DKL_PHY1_BASE, _DKL_PHY2_BASE, _DKL_BANK_SHIFT, _HIP_INDEX_REG0,
 * _HIP_INDEX_REG1, _HIP_INDEX_SHIFT).
 */
uint32_t
drv_i915_xd_dkl_window(
	unsigned tc,
	uint32_t phy_offset,
	uint32_t *index_reg,
	uint32_t *index_value)
{
	uint32_t bank;

	/* The bank goes into the port's byte of its index register. */
	bank = phy_offset >> I915_XD_DKL_BANK_SHIFT;
	*index_reg = I915_XD_HIP_INDEX_REG(tc);
	*index_value = bank << I915_XD_HIP_INDEX_SHIFT(tc);

	/* Succeeded: the register in the port's window. */
	return I915_XD_DKL_WINDOW(tc) + (phy_offset & I915_XD_DKL_OFFSET_IN_BANK);
}

/*
 * Reads a register of a Type-C port's Dekel PHY (phy_offset is the PHY's
 * own offset, bank and register).  A struct that is not ready reads
 * all-ones.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy.c
 * (dkl_phy_set_hip_idx, intel_dkl_phy_read).
 */
uint32_t
drv_i915_xd_dkl_read(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	uint32_t phy_offset)
{
	uint32_t offset;
	uint32_t value;

	/* Another route reads it whole. */
	if (dkl->hooks.read != NULL) {
		value = dkl->hooks.read(dkl->hooks.context, tc, phy_offset);
		return value;
	}

	/* Windows that are not ready have no registers. */
	if (!dkl->ready)
		return 0xffffffffU;

	/* Selects the bank and reads the register under the window lock. */
	i915_xd_dkl_lock(dkl);

	offset = i915_xd_dkl_select(dkl, tc, phy_offset);
	value = drv_i915_xd_io_read(dkl->io, offset);

	i915_xd_dkl_unlock(dkl);

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a register of a Type-C port's Dekel PHY.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy.c
 * (intel_dkl_phy_write).
 */
void
drv_i915_xd_dkl_write(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	uint32_t phy_offset,
	uint32_t value)
{
	uint32_t offset;

	/* Another route writes it whole. */
	if (dkl->hooks.write != NULL) {
		dkl->hooks.write(dkl->hooks.context, tc, phy_offset, value);
		return;
	}

	/* Windows that are not ready have no registers. */
	if (!dkl->ready)
		return;

	/* Selects the bank and writes the register under the window lock. */
	i915_xd_dkl_lock(dkl);

	offset = i915_xd_dkl_select(dkl, tc, phy_offset);
	drv_i915_xd_io_write(dkl->io, offset, value);

	i915_xd_dkl_unlock(dkl);
}

/*
 * Clears and sets bits of a register of a Type-C port's Dekel PHY, the
 * read and the write under one hold of the window lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy.c
 * (intel_dkl_phy_rmw).
 */
void
drv_i915_xd_dkl_rmw(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	uint32_t phy_offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t offset;

	/* Another route changes it whole. */
	if (dkl->hooks.rmw != NULL) {
		dkl->hooks.rmw(dkl->hooks.context, tc, phy_offset, clear, set);
		return;
	}

	/* Windows that are not ready have no registers. */
	if (!dkl->ready)
		return;

	/* Selects the bank and changes the register under the window lock. */
	i915_xd_dkl_lock(dkl);

	offset = i915_xd_dkl_select(dkl, tc, phy_offset);
	(void)drv_i915_xd_io_rmw(dkl->io, offset, clear, set);

	i915_xd_dkl_unlock(dkl);
}

/*
 * Waits for the microcontroller of a Type-C port's PHY to report itself
 * healthy, as it must after the port's AUX well came up.  Returns 0,
 * ETIMEDOUT when it did not within timeout_us, or EIO when the time base
 * failed.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h
 * (_DKL_CMN_UC_DW27, DKL_CMN_UC_DW27_UC_HEALTH).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (icl_tc_phy_aux_power_well_enable).
 */
int
drv_i915_xd_tc_wait_health(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	unsigned timeout_us)
{
	uint32_t status;
	unsigned waited;
	int error;

	/* Reads the health bit until it is set or the time is up; the last read decides. */
	waited = 0U;
	for (;;) {
		/* The microcontroller reports itself healthy. */
		status = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_CMN_UC_DW27);
		if ((status & I915_XD_DKL_UC_HEALTH) != 0U)
			break;

		/* The time is up. */
		if (waited >= timeout_us)
			return ETIMEDOUT;

		/* Lets a little time pass before the next read. */
		error = drv_i915_xd_io_delay(dkl->io, I915_XD_DKL_POLL_US);
		if (error != 0)
			return EIO;
		waited += I915_XD_DKL_POLL_US;
	}

	/* Succeeded: the microcontroller is healthy. */
	return 0;
}

/*
 * Sets the DisplayPort mode of the two lanes of a Type-C port for its
 * pin assignment and the lanes the link uses: one or two transmitters
 * of each lane.  A Thunderbolt port is not set here (the caller skips
 * it).  Returns 0, or EINVAL for a pin assignment the PHY has no mode
 * for (the lanes are then written with no mode).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_DP_MODE_CFG_DP_X1_MODE, MG_DP_MODE_CFG_DP_X2_MODE), and
 * drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h (_DKL_DP_MODE_LN0).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_program_mg_dp_mode).
 */
int
drv_i915_xd_tc_dp_mode(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	unsigned pin_assignment,
	unsigned lanes)
{
	uint32_t lane0;
	uint32_t lane1;
	uint32_t modes;
	uint32_t mode0;
	uint32_t mode1;
	int error;

	/* Reads both lanes' mode words without their modes. */
	modes = I915_XD_DKL_DP_MODE_X1 | I915_XD_DKL_DP_MODE_X2;
	lane0 = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_DP_MODE);
	lane1 = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_DP_MODE + I915_XD_DKL_LANE_STRIDE);
	lane0 &= ~modes;
	lane1 &= ~modes;

	/* Chooses the modes the pin assignment and the lane count call for. */
	error = i915_xd_tc_dp_modes(pin_assignment, lanes, &mode0, &mode1);

	/* Writes both lanes' modes. */
	drv_i915_xd_dkl_write(dkl, tc, I915_XD_DKL_DP_MODE, lane0 | mode0);
	drv_i915_xd_dkl_write(dkl, tc, I915_XD_DKL_DP_MODE + I915_XD_DKL_LANE_STRIDE, lane1 | mode1);

	/* Reports a pin assignment without a mode. */
	if (error != 0)
		return error;

	/* Succeeded: the lanes are in the link's mode. */
	return 0;
}

/*
 * Sets the DisplayPort mode of the two lanes of an Ice Lake Type-C port
 * in its MG PHY, as drv_i915_xd_tc_dp_mode() does in a Dekel PHY: the
 * same modes, in the lanes' registers reached directly.  Returns 0, or
 * EINVAL for a pin assignment the PHY has no mode for (the lanes are
 * then written with no mode).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_DP_MODE, MG_DP_MODE_CFG_DP_X1_MODE, MG_DP_MODE_CFG_DP_X2_MODE).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_program_mg_dp_mode).
 */
int
drv_i915_xd_tc_mg_dp_mode(
	struct i915_xd_io *io,
	unsigned tc,
	unsigned pin_assignment,
	unsigned lanes)
{
	uint32_t lane0_reg;
	uint32_t lane1_reg;
	uint32_t lane0;
	uint32_t lane1;
	uint32_t modes;
	uint32_t mode0;
	uint32_t mode1;
	int error;

	/* Reads both lanes' mode words without their modes. */
	modes = I915_XD_DKL_DP_MODE_X1 | I915_XD_DKL_DP_MODE_X2;
	lane0_reg = I915_XD_MG_PHY(tc) + I915_XD_MG_DP_MODE;
	lane1_reg = lane0_reg + I915_XD_MG_LANE_STRIDE;
	lane0 = drv_i915_xd_io_read(io, lane0_reg);
	lane1 = drv_i915_xd_io_read(io, lane1_reg);
	lane0 &= ~modes;
	lane1 &= ~modes;

	/* Chooses the modes the pin assignment and the lane count call for. */
	error = i915_xd_tc_dp_modes(pin_assignment, lanes, &mode0, &mode1);

	/* Writes both lanes' modes. */
	drv_i915_xd_io_write(io, lane0_reg, lane0 | mode0);
	drv_i915_xd_io_write(io, lane1_reg, lane1 | mode1);

	/* Reports a pin assignment without a mode. */
	if (error != 0)
		return error;

	/* Succeeded: the lanes are in the link's mode. */
	return 0;
}

/*
 * Chooses the Dekel swing table of a Type-C port: HDMI's, or DisplayPort's
 * up to 2.7 GHz or above it.  NULL on a platform without Dekel ports.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (tgl_get_dkl_buf_trans, adlp_get_dkl_buf_trans).
 */
const struct i915_xd_dkl_swing_table *
drv_i915_xd_tc_swing_table(
	const struct i915_xd_platform *platform,
	int hdmi,
	uint32_t rate_khz)
{
	const struct i915_xd_swing_set *set;

	/* HDMI has its own table. */
	set = platform->swing;
	if (hdmi)
		return set->dkl_hdmi;

	/* DisplayPort above 2.7 GHz. */
	if (rate_khz > I915_XD_DKL_SWING_LOW_MAX_KHZ)
		return set->dkl_dp_high;

	/* Succeeded: DisplayPort up to 2.7 GHz. */
	return set->dkl_dp_low;
}

/*
 * Programs the transmit swing of the four transmitters of a Type-C port:
 * each PHY lane leaves its suspend, its first transmitter takes the row
 * of the even link lane's level and its second the odd one's, the lane
 * leaves the 20-bit DisplayPort mode, and on a platform that selects them
 * it takes the load generators of HDMI or DisplayPort.  A level past the
 * table takes the table's last row.  A Thunderbolt port is not
 * programmed here (the caller skips it).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h
 * (_DKL_TX_PMD_LANE_SUS_LN0, _DKL_TX_DPCNTL0_LN0, _DKL_TX_DPCNTL1_LN0,
 * _DKL_TX_DPCNTL2_LN0, DKL_TX_DP20BITMODE).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (tgl_dkl_phy_set_signal_levels).
 */
void
drv_i915_xd_tc_signal_levels(
	struct i915_xd_dkl *dkl,
	const struct i915_xd_platform *platform,
	unsigned tc,
	const struct i915_xd_dkl_swing_table *table,
	const uint8_t levels[4],
	int hdmi)
{
	uint32_t fields;
	uint32_t lane_offset;
	uint32_t word;
	unsigned lane;

	/* The fields of the swing word a row sets. */
	fields = I915_XD_DKL_TX_PRESHOOT | I915_XD_DKL_TX_DE_EMPHASIS | I915_XD_DKL_TX_VSWING;

	/* Programs each PHY lane. */
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		/* Takes the lane out of its suspend. */
		lane_offset = lane * I915_XD_DKL_LANE_STRIDE;
		drv_i915_xd_dkl_write(dkl, tc, I915_XD_DKL_TX_PMD_LANE_SUS + lane_offset, 0U);

		/* The first transmitter carries the even link lane. */
		word = i915_xd_tc_swing_word(table, levels[2U * lane]);
		drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_TX_DPCNTL0 + lane_offset, fields, word);

		/* The second transmitter carries the odd link lane. */
		word = i915_xd_tc_swing_word(table, levels[2U * lane + 1U]);
		drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_TX_DPCNTL1 + lane_offset, fields, word);

		/* The lane leaves the 20-bit DisplayPort mode. */
		drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_TX_DPCNTL2 + lane_offset, I915_XD_DKL_TX_DP20BITMODE, 0U);

		/* A platform without the load generator select is done with the lane. */
		if ((platform->wa & I915_XD_WA_DKL_LOADGEN) == 0U)
			continue;

		/* The load generators of the output. */
		word = i915_xd_tc_loadgen(hdmi, lane);
		drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_TX_DPCNTL2 + lane_offset, I915_XD_DKL_TX_LOADGEN_TX1 | I915_XD_DKL_TX_LOADGEN_TX2,
			word);
	}
}

/*
 * Chooses the MG swing table of an Ice Lake Type-C port: HDMI's, or
 * DisplayPort's up to 2.7 GHz or above it.  NULL on a platform without
 * MG ports.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (icl_get_mg_buf_trans, icl_get_mg_buf_trans_dp).
 */
const struct i915_xd_mg_swing_table *
drv_i915_xd_tc_mg_swing_table(
	const struct i915_xd_platform *platform,
	int hdmi,
	uint32_t rate_khz)
{
	const struct i915_xd_swing_set *set;

	/* HDMI has its own table. */
	set = platform->swing;
	if (hdmi)
		return set->mg_hdmi;

	/* DisplayPort above 2.7 GHz. */
	if (rate_khz > I915_XD_DKL_SWING_LOW_MAX_KHZ)
		return set->mg_dp_high;

	/* Succeeded: DisplayPort up to 2.7 GHz. */
	return set->mg_dp_low;
}

/*
 * Programs the transmit swing of the four transmitters of an Ice Lake
 * Type-C port in its MG PHY, for a link clock (port_khz: the link rate,
 * or HDMI's clock): each transmitter leaves its FS32 use, takes the
 * de-emphasis override of its link lane's level (the first transmitter
 * of a PHY lane the even link lane, the second the odd one) in its swing
 * and drive controls, its clock hub's low-rate lock below 300 MHz, the
 * duty-cycle override above 500 MHz, and the calculation init of its
 * PISO.  A level past the table takes the table's last row.  A
 * Thunderbolt port is not programmed here (the caller skips it).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_TX1_LINK_PARAMS, MG_TX2_LINK_PARAMS, CRI_USE_FS32, MG_TX1_SWINGCTRL,
 * MG_TX2_SWINGCTRL, CRI_TXDEEMPH_OVERRIDE_17_12, MG_TX1_DRVCTRL,
 * MG_TX2_DRVCTRL, CRI_TXDEEMPH_OVERRIDE_11_6, CRI_TXDEEMPH_OVERRIDE_5_0,
 * CRI_TXDEEMPH_OVERRIDE_EN, MG_CLKHUB, CFG_LOW_RATE_LKREN_EN,
 * MG_TX1_DCC, MG_TX2_DCC, CFG_AMI_CK_DIV_OVERRIDE_VAL,
 * CFG_AMI_CK_DIV_OVERRIDE_EN, MG_TX1_PISO_READLOAD,
 * MG_TX2_PISO_READLOAD, CRI_CALCINIT).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_mg_phy_set_signal_levels).
 */
void
drv_i915_xd_tc_mg_signal_levels(
	struct i915_xd_io *io,
	unsigned tc,
	const struct i915_xd_mg_swing_table *table,
	const uint8_t levels[4],
	uint32_t port_khz)
{
	const struct i915_xd_mg_swing *row;
	uint32_t lane_base;
	uint32_t drive_fields;
	uint32_t word;
	uint32_t lock;
	uint32_t duty;
	unsigned lane;

	/* Each transmitter leaves its FS32 use. */
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX1_LINK_PARAMS, I915_XD_MG_TX_USE_FS32, 0U);
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX2_LINK_PARAMS, I915_XD_MG_TX_USE_FS32, 0U);
	}

	/* The swing controls take bits 17..12 of each link lane's de-emphasis. */
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		row = i915_xd_tc_mg_row(table, levels[2U * lane]);
		word = i915_xd_field_put(I915_XD_MG_TX_DEEMPH_17_12, row->deemph_17_12);
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX1_SWINGCTRL, I915_XD_MG_TX_DEEMPH_17_12, word);
		row = i915_xd_tc_mg_row(table, levels[2U * lane + 1U]);
		word = i915_xd_field_put(I915_XD_MG_TX_DEEMPH_17_12, row->deemph_17_12);
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX2_SWINGCTRL, I915_XD_MG_TX_DEEMPH_17_12, word);
	}

	/* The drive controls take bits 11..6 and 5..0 and turn the override on. */
	drive_fields = I915_XD_MG_TX_DEEMPH_11_6 | I915_XD_MG_TX_DEEMPH_5_0;
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		row = i915_xd_tc_mg_row(table, levels[2U * lane]);
		word = i915_xd_field_put(I915_XD_MG_TX_DEEMPH_11_6, row->deemph_11_6);
		word |= i915_xd_field_put(I915_XD_MG_TX_DEEMPH_5_0, row->deemph_5_0);
		word |= I915_XD_MG_TX_DEEMPH_OVERRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX1_DRVCTRL, drive_fields, word);
		row = i915_xd_tc_mg_row(table, levels[2U * lane + 1U]);
		word = i915_xd_field_put(I915_XD_MG_TX_DEEMPH_11_6, row->deemph_11_6);
		word |= i915_xd_field_put(I915_XD_MG_TX_DEEMPH_5_0, row->deemph_5_0);
		word |= I915_XD_MG_TX_DEEMPH_OVERRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX2_DRVCTRL, drive_fields, word);
	}

	/* The clock hub keeps the low-rate lock enable below 300 MHz. */
	lock = 0U;
	if (port_khz < I915_XD_MG_LOW_RATE_KHZ)
		lock = I915_XD_MG_CLKHUB_LOW_RATE_LKREN;
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_CLKHUB, I915_XD_MG_CLKHUB_LOW_RATE_LKREN, lock);
	}

	/* The duty-cycle correction takes the divider override above 500 MHz. */
	duty = 0U;
	if (port_khz > I915_XD_MG_DCC_OVERRIDE_KHZ)
		duty = i915_xd_field_put(I915_XD_MG_DCC_AMI_DIV, I915_XD_MG_DCC_DIVIDER) | I915_XD_MG_DCC_AMI_DIV_OVERRIDE;
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX1_DCC, I915_XD_MG_DCC_AMI_DIV | I915_XD_MG_DCC_AMI_DIV_OVERRIDE, duty);
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX2_DCC, I915_XD_MG_DCC_AMI_DIV | I915_XD_MG_DCC_AMI_DIV_OVERRIDE, duty);
	}

	/* Each transmitter's PISO starts its calculation. */
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++) {
		lane_base = I915_XD_MG_PHY(tc) + lane * I915_XD_MG_LANE_STRIDE;
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX1_PISO_READLOAD, 0U, I915_XD_MG_TX_CALCINIT);
		(void)drv_i915_xd_io_rmw(io, lane_base + I915_XD_MG_TX2_PISO_READLOAD, 0U, I915_XD_MG_TX_CALCINIT);
	}
}

/*
 * Releases the soft reset of the PCS of both lanes of a Type-C port,
 * which a port switched from Thunderbolt to DisplayPort alternate or
 * legacy mode needs on display 13 before its buffer is enabled.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h
 * (_DKL_PCS_DW5_LN0, DKL_PCS_DW5_CORE_SOFTRESET).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (adlp_tbt_to_dp_alt_switch_wa).
 */
void
drv_i915_xd_tc_release_soft_reset(
	struct i915_xd_dkl *dkl,
	unsigned tc)
{
	unsigned lane;

	/* Clears the bit in each lane. */
	for (lane = 0U; lane < I915_XD_DKL_LANES; lane++)
		drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_PCS_DW5 + lane * I915_XD_DKL_LANE_STRIDE, I915_XD_DKL_PCS_DW5_CORE_SOFTRESET, 0U);
}

/*
 * Finds where a Type-C port's FIA words are.  Display 13's FIA is
 * modular (two ports a FIA); Tiger Lake's is when FIA 1 says so (read
 * with TC cold blocked), otherwise FIA 1 holds every port; Ice Lake's
 * FIA 1 holds every port and is not asked.  Returns 0, or EIO when FIA 1
 * reads all-ones (the PHY is in TC cold).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (FIA1_BASE, FIA2_BASE, FIA3_BASE), and drivers/gpu/drm/i915/i915_reg.h
 * (MODULAR_FIA_MASK).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (tc_phy_load_fia_params, icl_tc_phy_init, tgl_tc_phy_init,
 * adlp_tc_phy_init).
 */
int
drv_i915_xd_tc_fia(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned tc,
	struct i915_xd_tc_fia *fia)
{
	static const uint32_t bases[] = { I915_XD_FIA1_BASE, I915_XD_FIA2_BASE, I915_XD_FIA3_BASE };
	uint32_t word;
	int modular;

	/* Display 13's FIA is modular; Tiger Lake's says whether it is; Ice Lake has FIA 1 alone. */
	modular = 1;
	if (platform->tc_flow == I915_XD_TC_FLOW_FIA && platform->fia == I915_XD_FIA_SINGLE) {
		modular = 0;
	} else if (platform->tc_flow == I915_XD_TC_FLOW_FIA) {
		word = drv_i915_xd_io_read(io, I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP);
		if (word == 0xffffffffU)
			return EIO;
		modular = 0;
		if ((word & I915_XD_DFLEXDPSP_MODULAR) != 0U)
			modular = 1;
	}

	/* A modular FIA serves two ports, the single one all of them. */
	if (modular) {
		fia->base = bases[(tc / 2U) % 3U];
		fia->index = tc % 2U;
	} else {
		fia->base = I915_XD_FIA1_BASE;
		fia->index = tc;
	}

	/* Succeeded: the port's FIA. */
	return 0;
}

/*
 * Reads what the sink of a Type-C port put it in (I915_XD_TC_LIVE_*): on
 * display 13 the Type-C and Thunderbolt bits of the hotplug interrupt
 * word, on Tiger Lake the FIA's live state (nothing while it reads
 * all-ones, in TC cold: the caller blocks TC cold); on both the south
 * display's Type-C pin for a legacy connector.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_hpd_live_status, adlp_tc_phy_hpd_live_status).
 */
unsigned
drv_i915_xd_tc_live(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	const struct i915_xd_tc_fia *fia)
{
	uint32_t word;
	unsigned live;
	unsigned tc;

	/* The north side: the hotplug interrupt word, or the FIA. */
	live = 0U;
	tc = port->tc - 1U;
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		word = drv_i915_xd_io_read(io, I915_XD_DE_HPD_ISR);
		if ((word & I915_XD_HPD_INT_TC(tc)) != 0U)
			live |= I915_XD_TC_LIVE_DP_ALT;
		if ((word & I915_XD_HPD_INT_TBT(tc)) != 0U)
			live |= I915_XD_TC_LIVE_TBT;
	} else {
		word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXDPSP);
		if (word == 0xffffffffU)
			return 0U;
		if ((word & I915_XD_DFLEXDPSP_LIVE_TBT(fia->index)) != 0U)
			live |= I915_XD_TC_LIVE_TBT;
		if ((word & I915_XD_DFLEXDPSP_LIVE_TC(fia->index)) != 0U)
			live |= I915_XD_TC_LIVE_DP_ALT;
	}

	/* The south side: a legacy connector on the port's Type-C pin. */
	if (port->south_pin_kind == I915_XD_SOUTH_PIN_TC) {
		word = drv_i915_xd_io_read(io, I915_XD_SDE_ISR);
		if ((word & I915_XD_SDE_TC_HPD(port->south_pin)) != 0U)
			live |= I915_XD_TC_LIVE_LEGACY;
	}

	/* Succeeded: the live state. */
	return live;
}

/*
 * Tells whether a Type-C port's PHY is ready for the display to take it:
 * the TCSS status on display 13, the FIA's PHY mode status on Tiger Lake;
 * a word that reads all-ones (TC cold) is not ready.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TCSS_DDI_STATUS_READY, DP_PHY_MODE_STATUS_COMPLETED).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_is_ready, adlp_tc_phy_is_ready).
 */
int
drv_i915_xd_tc_ready(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned tc,
	const struct i915_xd_tc_fia *fia)
{
	uint32_t word;

	/* Display 13: the TCSS status. */
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		word = drv_i915_xd_io_read(io, I915_XD_TCSS_DDI_STATUS(tc));
		if (word == 0xffffffffU)
			return 0;
		if ((word & I915_XD_TCSS_READY) == 0U)
			return 0;

		/* Succeeded: ready. */
		return 1;
	}

	/* Tiger Lake: the FIA's PHY mode status. */
	word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXDPPMS);
	if (word == 0xffffffffU)
		return 0;
	if ((word & I915_XD_DFLEXDPPMS_READY(fia->index)) == 0U)
		return 0;

	/* Succeeded: ready. */
	return 1;
}

/*
 * Takes (take nonzero) or gives back a Type-C port's PHY: on display 13
 * the ownership bit of the port's DDI buffer word (with the port's lane
 * well up), on Tiger Lake the FIA's not-safe bit.  Returns 0, or EIO when
 * the FIA reads all-ones (TC cold).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DDI_BUF_CTL_TC_PHY_OWNERSHIP, DP_PHY_MODE_STATUS_NOT_SAFE).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_take_ownership, adlp_tc_phy_take_ownership).
 */
int
drv_i915_xd_tc_own(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	const struct i915_xd_tc_fia *fia,
	int take)
{
	uint32_t bit;
	uint32_t word;

	/* Display 13: the DDI buffer's ownership bit. */
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		bit = 0U;
		if (take)
			bit = I915_XD_DDI_BUF_TC_OWNED;
		(void)drv_i915_xd_io_rmw(io, I915_XD_DDI_BUF_CTL(port->ddi), I915_XD_DDI_BUF_TC_OWNED, bit);

		/* Succeeded: the bit is written. */
		return 0;
	}

	/* Tiger Lake: the FIA's not-safe bit, unless the FIA is in TC cold. */
	word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXDPCSSS);
	if (word == 0xffffffffU)
		return EIO;
	word &= ~I915_XD_DFLEXDPCSSS_OWNED(fia->index);
	if (take)
		word |= I915_XD_DFLEXDPCSSS_OWNED(fia->index);
	drv_i915_xd_io_write(io, fia->base + I915_XD_FIA_DFLEXDPCSSS, word);

	/* Succeeded: the bit is written. */
	return 0;
}

/*
 * Tells whether the display owns a Type-C port's PHY (a FIA in TC cold
 * owns nothing).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_is_owned, adlp_tc_phy_is_owned).
 */
int
drv_i915_xd_tc_owned(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	const struct i915_xd_tc_fia *fia)
{
	uint32_t word;

	/* Display 13: the DDI buffer's ownership bit. */
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		word = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(port->ddi));
		if ((word & I915_XD_DDI_BUF_TC_OWNED) == 0U)
			return 0;

		/* Succeeded: owned. */
		return 1;
	}

	/* Tiger Lake: the FIA's not-safe bit. */
	word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXDPCSSS);
	if (word == 0xffffffffU)
		return 0;
	if ((word & I915_XD_DFLEXDPCSSS_OWNED(fia->index)) == 0U)
		return 0;

	/* Succeeded: owned. */
	return 1;
}

/*
 * Reads a Type-C port's pin assignment from its FIA (0 for a legacy
 * connector, 1..6 for A..F).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_get_pin_assignment_mask).
 */
unsigned
drv_i915_xd_tc_pin(
	struct i915_xd_io *io,
	const struct i915_xd_tc_fia *fia)
{
	uint32_t word;

	/* The port's nibble. */
	word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXPA1);

	/* Succeeded: the assignment. */
	return i915_xd_field_get(I915_XD_DFLEXPA1_PIN(fia->index), word);
}

/*
 * Reads how many lanes the sink of a Type-C port in DisplayPort
 * alternate mode gave the display: one lane of any, two of the lower or
 * upper pair, all four; an assignment it does not know counts as one.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_get_lane_mask, intel_tc_port_get_max_lane_count).
 */
unsigned
drv_i915_xd_tc_max_lanes(
	struct i915_xd_io *io,
	const struct i915_xd_tc_fia *fia)
{
	uint32_t word;
	uint32_t lanes;

	/* The port's lane assignment. */
	word = drv_i915_xd_io_read(io, fia->base + I915_XD_FIA_DFLEXDPSP);
	lanes = i915_xd_field_get(I915_XD_DFLEXDPSP_LANES(fia->index), word);

	/* Counts it. */
	switch (lanes) {
	case 0x3U:
	case 0xcU:
		return 2U;
	case 0xfU:
		return 4U;
	default:
		return 1U;
	}
}

/*
 * Tells a Type-C port's FIA which main link lanes the display uses: one
 * lane (lane 3 when the lanes are reversed, lane 0 otherwise), two (the
 * upper or the lower pair), or four.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (intel_tc_port_set_fia_lane_count).
 */
void
drv_i915_xd_tc_set_lanes(
	struct i915_xd_io *io,
	const struct i915_xd_tc_fia *fia,
	unsigned lanes,
	int reversed)
{
	uint32_t field;
	uint32_t value;

	/* The lanes of the count and direction. */
	switch (lanes) {
	case 1U:
		value = reversed ? I915_XD_DFLEXDPMLE1_ML3 : I915_XD_DFLEXDPMLE1_ML0;
		break;
	case 2U:
		value = reversed ? I915_XD_DFLEXDPMLE1_ML3_2 : I915_XD_DFLEXDPMLE1_ML1_0;
		break;
	default:
		value = I915_XD_DFLEXDPMLE1_ML3_0;
		break;
	}

	/* The port's nibble. */
	field = I915_XD_DFLEXDPMLE1_LANES(fia->index);
	(void)drv_i915_xd_io_rmw(io, fia->base + I915_XD_FIA_DFLEXDPMLE1, field, i915_xd_field_put(field, value));
}

/*
 * Reports what keeps a Type-C port out of TC cold in a mode, as the power
 * functions (I915_XD_POWERS_*) the owner brings up: Tiger Lake's TC cold
 * well in every mode; on display 13 the port's AUX well (aux is its AUX
 * channel) except in Thunderbolt mode, which needs none; on Ice Lake the
 * port's AUX well for a legacy connector only (the well then asks the
 * firmware to leave TC cold), nothing in the other modes.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_cold_off_domain, tgl_tc_phy_cold_off_domain,
 * adlp_tc_phy_cold_off_domain).
 */
uint64_t
drv_i915_xd_tc_cold_functions(
	const struct i915_xd_platform *platform,
	unsigned aux,
	unsigned mode)
{
	/* Tiger Lake: the firmware's TC cold block. */
	if (platform->tccold == I915_XD_TCCOLD_MAILBOX_WELL)
		return I915_XD_POWERS_TC_COLD_OFF;

	/* Ice Lake: a legacy connector's AUX well, nothing in the other modes. */
	if (platform->tccold == I915_XD_TCCOLD_AUX_WELL_EXIT) {
		if (mode != I915_XD_TC_MODE_LEGACY)
			return 0U;

		/* Succeeded: a legacy connector's AUX well. */
		return I915_XD_POWERS_AUX(aux);
	}

	/* A platform without TC cold. */
	if (platform->tccold != I915_XD_TCCOLD_AUX_WELL)
		return 0U;

	/* Display 13 in Thunderbolt mode needs no well. */
	if (mode == I915_XD_TC_MODE_TBT)
		return 0U;

	/* Succeeded: the port's AUX well. */
	return I915_XD_POWERS_AUX(aux);
}

/*
 * Connects a Type-C port in a mode for a link of some lanes, with the
 * platform's flow, and fills the connection.
 *
 * The TCSS flow (display 13): the port's lane well, the ownership, the
 * PHY's ready, TC cold blocked by the port's AUX well, the lanes checked,
 * the lane well given back.  The FIA flow (Tiger Lake): TC cold blocked,
 * the port's FIA, the PHY's ready and then the ownership, the lanes
 * checked.  Thunderbolt mode only blocks TC cold.  A legacy connector is
 * used even when the PHY is not ready or not taken (it is logged).  The
 * lanes check: a legacy connector has four; in DisplayPort
 * alternate mode the sink must still be there and give enough lanes.  A
 * step that fails gives back what the earlier ones took.  Returns 0,
 * EBUSY (not ready or not taken), ENODEV (the sink left), ENOSPC (too
 * few lanes), EINVAL (a port that is not Type-C, or no mode), EIO (TC
 * cold that did not end), or the owner's error.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_connect, adlp_tc_phy_connect, tc_phy_verify_legacy_or_dp_alt_mode,
 * tc_cold_block).
 */
int
drv_i915_xd_tc_connect(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	unsigned aux,
	unsigned mode,
	unsigned lanes,
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	int error;

	/* Nothing held yet. */
	connection->mode = I915_XD_TC_MODE_NONE;
	connection->blocked = 0U;
	connection->owned = 0;
	connection->max_lanes = 0U;
	connection->pin = 0U;
	connection->fia.base = 0U;
	connection->fia.index = 0U;

	/* Only a Type-C port connects, in a mode. */
	if (port->tc == 0U || platform->tc_flow == I915_XD_TC_FLOW_NONE)
		return EINVAL;
	if (mode == I915_XD_TC_MODE_NONE)
		return EINVAL;

	/* The platform's flow. */
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		error = i915_xd_tc_connect_tcss(io, platform, port, aux, mode, lanes, power, connection);
	} else {
		error = i915_xd_tc_connect_fia(io, platform, port, aux, mode, lanes, power, connection);
	}

	/* Reports a connection that failed. */
	if (error != 0)
		return error;

	/* Notes the mode. */
	connection->mode = mode;

	/* Succeeded: connected in the mode. */
	return 0;
}

/*
 * Disconnects a Type-C port: on display 13 under the port's lane well,
 * TC cold allowed again and the PHY given back; on Tiger Lake the PHY
 * given back and TC cold allowed again.  A port in Thunderbolt mode only
 * allows TC cold again.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c
 * (icl_tc_phy_disconnect, adlp_tc_phy_disconnect).
 */
void
drv_i915_xd_tc_disconnect(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	uint64_t lane_well;
	int lane_error;

	/* A port that is not connected has nothing to give back. */
	if (connection->mode == I915_XD_TC_MODE_NONE)
		return;

	/* Display 13: under the lane well, TC cold first, then the PHY. */
	if (platform->tc_flow == I915_XD_TC_FLOW_TCSS) {
		lane_well = I915_XD_POWERS_LANES(port->ddi);
		lane_error = power->get(power->context, lane_well);
		i915_xd_tc_unblock(power, connection);
		if (connection->owned)
			(void)drv_i915_xd_tc_own(io, platform, port, &connection->fia, 0);
		if (lane_error == 0)
			power->put(power->context, lane_well);
	} else {
		/* Tiger Lake: the PHY first, then TC cold. */
		if (connection->owned)
			(void)drv_i915_xd_tc_own(io, platform, port, &connection->fia, 0);
		i915_xd_tc_unblock(power, connection);
	}

	/* Nothing held. */
	connection->owned = 0;
	connection->mode = I915_XD_TC_MODE_NONE;
}

/* Takes the window lock, telling a host test so. */
static void
i915_xd_dkl_lock(
	struct i915_xd_dkl *dkl)
{
	/* Notes the lock, then takes it. */
	drv_i915_xd_io_lock_note(dkl->io, I915_XD_LOCK_DKL, 1);
	spin_lock(&dkl->lock);
}

/* Releases the window lock. */
static void
i915_xd_dkl_unlock(
	struct i915_xd_dkl *dkl)
{
	/* Releases the lock, then notes it. */
	spin_unlock(&dkl->lock);
	drv_i915_xd_io_lock_note(dkl->io, I915_XD_LOCK_DKL, 0);
}

/*
 * Selects the bank of a PHY offset in the port's index register and
 * reports the window register that reaches it.  Runs under the window
 * lock.
 */
static uint32_t
i915_xd_dkl_select(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	uint32_t phy_offset)
{
	uint32_t index_reg;
	uint32_t index_value;
	uint32_t window;

	/* Writes the bank into the port's byte of its index register. */
	window = drv_i915_xd_dkl_window(tc, phy_offset, &index_reg, &index_value);
	drv_i915_xd_io_write(dkl->io, index_reg, index_value);

	/* Succeeded: the register in the port's window. */
	return window;
}

/* Builds the swing word of one table row; a level past the table takes its last row. */
static uint32_t
i915_xd_tc_swing_word(
	const struct i915_xd_dkl_swing_table *table,
	uint8_t level)
{
	const struct i915_xd_dkl_swing *row;
	uint32_t word;
	unsigned index;

	/* The row of the level, kept inside the table. */
	index = level;
	if (index >= table->count)
		index = table->count - 1U;
	row = &table->rows[index];

	/* Places the preshoot, de-emphasis and swing control fields. */
	word = i915_xd_field_put(I915_XD_DKL_TX_PRESHOOT, row->preshoot);
	word |= i915_xd_field_put(I915_XD_DKL_TX_DE_EMPHASIS, row->de_emphasis);
	word |= i915_xd_field_put(I915_XD_DKL_TX_VSWING, row->vswing);

	/* Succeeded: the row's swing word. */
	return word;
}

/* Builds the load generator selects of a PHY lane for HDMI or DisplayPort. */
static uint32_t
i915_xd_tc_loadgen(
	int hdmi,
	unsigned lane)
{
	uint32_t word;

	/* DisplayPort selects 0 for both transmitters. */
	if (!hdmi)
		return 0U;

	/* HDMI: lane 0 and lane 1 have their own selects. */
	if (lane == 0U) {
		word = i915_xd_field_put(I915_XD_DKL_TX_LOADGEN_TX1, I915_XD_DKL_HDMI_LOADGEN_LN0_TX1);
		word |= i915_xd_field_put(I915_XD_DKL_TX_LOADGEN_TX2, I915_XD_DKL_HDMI_LOADGEN_LN0_TX2);
	} else {
		word = i915_xd_field_put(I915_XD_DKL_TX_LOADGEN_TX1, I915_XD_DKL_HDMI_LOADGEN_LN1_TX1);
		word |= i915_xd_field_put(I915_XD_DKL_TX_LOADGEN_TX2, I915_XD_DKL_HDMI_LOADGEN_LN1_TX2);
	}

	/* Succeeded: the selects of the lane. */
	return word;
}

/* The TCSS flow of display 13's connect (see drv_i915_xd_tc_connect()). */
static int
i915_xd_tc_connect_tcss(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	unsigned aux,
	unsigned mode,
	unsigned lanes,
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	uint64_t lane_well;
	int ready;
	int error;

	/* The FIA is modular: no read. */
	error = drv_i915_xd_tc_fia(io, platform, port->tc - 1U, &connection->fia);
	if (error != 0)
		return error;

	/* Thunderbolt mode only blocks TC cold. */
	if (mode == I915_XD_TC_MODE_TBT) {
		error = i915_xd_tc_block(platform, aux, mode, power, connection);
		if (error != 0)
			return error;

		/* Succeeded: blocked. */
		return 0;
	}

	/* The port's lane well, under which the ownership bit holds. */
	lane_well = I915_XD_POWERS_LANES(port->ddi);
	error = power->get(power->context, lane_well);
	if (error != 0)
		return error;

	/* Takes the PHY. */
	(void)drv_i915_xd_tc_own(io, platform, port, &connection->fia, 1);
	connection->owned = 1;

	/* The PHY must be ready (a legacy connector goes on). */
	ready = drv_i915_xd_tc_ready(io, platform, port->tc - 1U, &connection->fia);
	if (!ready && mode != I915_XD_TC_MODE_LEGACY)
		error = EBUSY;

	/* Blocks TC cold. */
	if (error == 0)
		error = i915_xd_tc_block(platform, aux, mode, power, connection);

	/* The lanes and the sink; TC cold is allowed again when they fail. */
	if (error == 0) {
		error = i915_xd_tc_check(io, platform, port, mode, lanes, connection);
		if (error != 0)
			i915_xd_tc_unblock(power, connection);
	}

	/* A step that failed gives the PHY back. */
	if (error != 0) {
		(void)drv_i915_xd_tc_own(io, platform, port, &connection->fia, 0);
		connection->owned = 0;
	}

	/* The lane well is given back either way. */
	power->put(power->context, lane_well);
	if (error != 0)
		return error;

	/* Succeeded: TC cold blocked and the PHY held. */
	return 0;
}

/* The FIA flow of Tiger Lake's connect (see drv_i915_xd_tc_connect()). */
static int
i915_xd_tc_connect_fia(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	unsigned aux,
	unsigned mode,
	unsigned lanes,
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	int ready;
	int error;

	/* Blocks TC cold. */
	error = i915_xd_tc_block(platform, aux, mode, power, connection);
	if (error != 0)
		return error;

	/* The port's FIA, read out of TC cold. */
	error = drv_i915_xd_tc_fia(io, platform, port->tc - 1U, &connection->fia);

	/* Thunderbolt mode is done. */
	if (error == 0 && mode == I915_XD_TC_MODE_TBT)
		return 0;

	/* The PHY must be ready and taken (a legacy connector goes on). */
	if (error == 0) {
		ready = drv_i915_xd_tc_ready(io, platform, port->tc - 1U, &connection->fia);
		if (ready) {
			error = drv_i915_xd_tc_own(io, platform, port, &connection->fia, 1);
			if (error == 0)
				connection->owned = 1;
		}

		/* A port the PHY was not given to stays busy, unless its connector is legacy. */
		error = 0;
		if (!connection->owned && mode != I915_XD_TC_MODE_LEGACY)
			error = EBUSY;
	}

	/* The lanes and the sink. */
	if (error == 0)
		error = i915_xd_tc_check(io, platform, port, mode, lanes, connection);

	/* A step that failed gives back the PHY and TC cold. */
	if (error != 0) {
		if (connection->owned)
			(void)drv_i915_xd_tc_own(io, platform, port, &connection->fia, 0);
		connection->owned = 0;
		i915_xd_tc_unblock(power, connection);
		return error;
	}

	/* Succeeded: TC cold blocked and the PHY held. */
	return 0;
}

/* Brings up what keeps a port out of TC cold in its mode, and notes it in the connection. */
static int
i915_xd_tc_block(
	const struct i915_xd_platform *platform,
	unsigned aux,
	unsigned mode,
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	uint64_t functions;
	int error;

	/* A mode that needs nothing. */
	functions = drv_i915_xd_tc_cold_functions(platform, aux, mode);
	if (functions == 0U)
		return 0;

	/* The owner's wells. */
	error = power->get(power->context, functions);
	if (error != 0)
		return error;

	/* Notes what is held. */
	connection->blocked = functions;

	/* Succeeded: held. */
	return 0;
}

/* Gives back what kept a port out of TC cold. */
static void
i915_xd_tc_unblock(
	const struct i915_xd_tc_power *power,
	struct i915_xd_tc_connection *connection)
{
	/* Nothing held. */
	if (connection->blocked == 0U)
		return;

	/* The owner's wells. */
	power->put(power->context, connection->blocked);
	connection->blocked = 0U;
}

/*
 * Checks the lanes of a connection: a legacy connector has four; in
 * DisplayPort alternate mode the sink must still be there and give at
 * least the lanes the link needs.  Notes the lanes and the pin
 * assignment.  Returns 0, ENODEV or ENOSPC.
 */
static int
i915_xd_tc_check(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port,
	unsigned mode,
	unsigned lanes,
	struct i915_xd_tc_connection *connection)
{
	unsigned live;

	/* The pin assignment. */
	connection->pin = drv_i915_xd_tc_pin(io, &connection->fia);

	/* A legacy connector has all four lanes. */
	if (mode == I915_XD_TC_MODE_LEGACY) {
		connection->max_lanes = 4U;
		return 0;
	}

	/* The sink may have left meanwhile. */
	live = drv_i915_xd_tc_live(io, platform, port, &connection->fia);
	if ((live & I915_XD_TC_LIVE_DP_ALT) == 0U)
		return ENODEV;

	/* The lanes it gives. */
	connection->max_lanes = drv_i915_xd_tc_max_lanes(io, &connection->fia);
	if (connection->max_lanes < lanes)
		return ENOSPC;

	/* Succeeded: enough lanes. */
	return 0;
}

/*
 * Chooses the DisplayPort modes of a Type-C PHY's two lanes for a pin
 * assignment and the lanes a link uses: one or two transmitters of each
 * lane.  Returns 0, or EINVAL (with no modes) for a pin assignment the
 * PHY has no mode for.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_program_mg_dp_mode).
 */
static int
i915_xd_tc_dp_modes(
	unsigned pin_assignment,
	unsigned lanes,
	uint32_t *mode0_out,
	uint32_t *mode1_out)
{
	uint32_t mode0;
	uint32_t mode1;
	int error;

	/* The modes of each pin assignment. */
	mode0 = 0U;
	mode1 = 0U;
	error = 0;
	switch (pin_assignment) {
	case I915_XD_TC_PIN_LEGACY:
		/* A legacy port: one lane is lane 1's single transmitter, more are both lanes doubled. */
		if (lanes == 1U) {
			mode1 = I915_XD_DKL_DP_MODE_X1;
		} else {
			mode0 = I915_XD_DKL_DP_MODE_X2;
			mode1 = I915_XD_DKL_DP_MODE_X2;
		}

		break;
	case I915_XD_TC_PIN_A:
		/* Assignment A doubles both lanes only for four lanes. */
		if (lanes == 4U) {
			mode0 = I915_XD_DKL_DP_MODE_X2;
			mode1 = I915_XD_DKL_DP_MODE_X2;
		}

		break;
	case I915_XD_TC_PIN_B:
		/* Assignment B doubles both lanes only for two lanes. */
		if (lanes == 2U) {
			mode0 = I915_XD_DKL_DP_MODE_X2;
			mode1 = I915_XD_DKL_DP_MODE_X2;
		}

		break;
	case I915_XD_TC_PIN_C:
	case I915_XD_TC_PIN_D:
	case I915_XD_TC_PIN_E:
	case I915_XD_TC_PIN_F:
		/* Assignments C to F: one lane is both lanes single, more are both doubled. */
		if (lanes == 1U) {
			mode0 = I915_XD_DKL_DP_MODE_X1;
			mode1 = I915_XD_DKL_DP_MODE_X1;
		} else {
			mode0 = I915_XD_DKL_DP_MODE_X2;
			mode1 = I915_XD_DKL_DP_MODE_X2;
		}

		break;
	default:
		/* The PHY has no mode for it. */
		error = EINVAL;
		break;
	}

	/* Hands the modes back. */
	*mode0_out = mode0;
	*mode1_out = mode1;

	/* Reports a pin assignment without a mode. */
	if (error != 0)
		return error;

	/* Succeeded: the modes. */
	return 0;
}

/* The row of an MG swing table for a level; a level past the table takes its last row. */
static const struct i915_xd_mg_swing *
i915_xd_tc_mg_row(
	const struct i915_xd_mg_swing_table *table,
	uint8_t level)
{
	unsigned index;

	/* The row of the level, kept inside the table. */
	index = level;
	if (index >= table->count)
		index = table->count - 1U;

	/* Succeeded: the row. */
	return &table->rows[index];
}
