/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The start and the stop of the display core of the display 12 and 13
 * code (see xd-core.h).
 */

#include "xd-core.h"
#include "xd-dbuf.h"
#include "xd-ddi.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "xd-power.h"
#include "display-internal.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The tracker service of a display 12 DBUF slice, and the memory bus
 * ABOX credits of display 12 (both pools 16, B 1, BW 1).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (gen12_dbuf_slices_config, icl_mbus_init).
 */
#define I915_XD_CORE_TRACKER_SERVICE	8U
#define I915_XD_CORE_ABOX_POOL		16U
#define I915_XD_CORE_ABOX_B		1U
#define I915_XD_CORE_ABOX_BW		1U

/*
 * The TLB request timer display 12 sets in each bandwidth buddy.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c (tgl_bw_buddy_init).
 */
#define I915_XD_CORE_BUDDY_TLB_TIMER	0x8U

static void i915_xd_core_tracker(struct i915_xd_io *io, const struct i915_xd_platform *platform);
static void i915_xd_core_change(struct i915_xd_io *io, uint32_t offset, uint32_t clear, uint32_t set);
static void i915_xd_core_dbuf(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_core_hooks *hooks);
static void i915_xd_core_abox(struct i915_xd_io *io, const struct i915_xd_platform *platform);
static void i915_xd_core_buddy(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_core_input *input);

/*
 * Starts the display core in its order (see xd-core.h).  *stop tells
 * where a fault stopped it (I915_XD_CORE_DONE when it did not) and
 * *phys the combo PHYs it brought up.  Returns 0, or EIO for a fault of
 * the register access.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (icl_display_core_init, intel_pch_reset_handshake, gen9_dbuf_enable).
 */
int
drv_i915_xd_core_start(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_power *power,
	const struct i915_xd_core_input *input,
	const struct i915_xd_core_hooks *hooks,
	unsigned *stop,
	unsigned *phys)
{
	int error;
	int faulted;

	/* The DC states the firmware left. */
	*stop = I915_XD_CORE_DONE;
	*phys = 0U;
	drv_i915_xd_io_step(io, "xd-core start");
	if (power != NULL)
		drv_i915_xd_power_dc_clear(power, io);

	/* The DPMG unit's clock gate off on a TGP or ADP south display. */
	if (input->pch_kind == I915_PCH_TGP || input->pch_kind == I915_PCH_ADP)
		drv_i915_xd_io_rmw(io, I915_XD_SOUTH_DSPCLK_GATE_D, 0U, I915_XD_SOUTH_DSPCLK_DPMG_UNGATE);

	/* The PCH reset handshake. */
	drv_i915_xd_io_rmw(io, I915_XD_NDE_RSTWRN_OPT, I915_XD_NDE_RSTWRN_PCH_HANDSHAKE, I915_XD_NDE_RSTWRN_PCH_HANDSHAKE);

	/* The combo PHYs. */
	*phys = drv_i915_xd_combo_phy_init(io, platform);
	faulted = hooks->faulted(hooks->context);
	if (faulted) {
		*stop = I915_XD_CORE_STOP_PHYS;
		return EIO;
	}

	/* Power well 1. */
	error = hooks->well1_on(hooks->context);
	if (error != 0) {
		*stop = I915_XD_CORE_STOP_WELL1;
		return EIO;
	}

	/* The CD clock. */
	hooks->cdclk_on(hooks->context);
	faulted = hooks->faulted(hooks->context);
	if (faulted) {
		*stop = I915_XD_CORE_STOP_CDCLK;
		return EIO;
	}

	/* The DBUF slices' tracker service (display 12), the DBUF, the ABOX credits and the buddy. */
	i915_xd_core_tracker(io, platform);
	i915_xd_core_dbuf(io, platform, hooks);
	i915_xd_core_abox(io, platform);
	i915_xd_core_buddy(io, platform, input);

	/* The DC power request chicken bits of display 12 and 13 (Ice Lake takes none). */
	if (platform->display_ver >= 12U) {
		drv_i915_xd_io_rmw(io, I915_XD_CHICKEN_DCPR_2, 0U,
			I915_XD_DCPR_2_CLEAR_MEMSTAT_DIS | I915_XD_DCPR_2_SEND_RESP_IMM |
			I915_XD_DCPR_2_MASK_LPMODE | I915_XD_DCPR_2_MASK_MAXLATENCY_MEMUP);
	}

	/* Display 13's fatal error mask. */
	if (platform->display_ver == 13U)
		drv_i915_xd_io_write(io, I915_XD_DISPLAY_ERR_FATAL_MASK, 0xffffffffU);

	/* Succeeded: the display core is up. */
	return 0;
}

/*
 * Stops the display core: the DC states left (*report tells how), the
 * DBUF off, the CD clock down, power well 1 off, the combo PHYs down
 * (*phy_a_down nonzero when PHY A was not up).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c (icl_display_core_uninit).
 */
void
drv_i915_xd_core_stop(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_power *power,
	const struct i915_xd_core_hooks *hooks,
	struct i915_xd_dc_report *report,
	unsigned *phy_a_down)
{
	/* The DC states. */
	drv_i915_xd_io_step(io, "xd-core stop");
	if (power != NULL)
		drv_i915_xd_power_dc_leave(power, io, report);

	/* The DBUF, the CD clock and power well 1. */
	hooks->dbuf(hooks->context, 0U);
	hooks->cdclk_off(hooks->context);
	hooks->well1_off(hooks->context);

	/* The combo PHYs. */
	*phy_a_down = drv_i915_xd_combo_phy_uninit(io, platform);
}

/*
 * Applies the display's start workarounds, which the display's start
 * writes after it read the CD clock: on display 13 the DPCE clock gate
 * off and the DDI clock register access chicken cleared; on display 12
 * the compressor's dummy pixel chicken and the clock request policy's
 * memory-up override cleared; on display 11 the dummy pixel chicken and
 * the power response delay.  A register that already holds its value is
 * not written again.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_wa.c
 * (intel_display_wa_apply, adlp_display_wa_apply, xe_d_display_wa_apply,
 * gen11_display_wa_apply).
 */
void
drv_i915_xd_display_wa(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	/* Display 13: the DPCE clock gate off, the DDI clock register access chicken cleared. */
	if (platform->display_ver >= 13U) {
		i915_xd_core_change(io, I915_XD_CLKGATE_DIS_5, 0U, I915_XD_CLKGATE_DIS_5_DPCE);
		i915_xd_core_change(io, I915_XD_CHICKEN_DCPR_1, I915_XD_DCPR_1_DDI_CLOCK_REG_ACCESS, 0U);
		return;
	}

	/* Display 12: the dummy pixel chicken written, the memory-up override cleared. */
	if (platform->display_ver == 12U) {
		drv_i915_xd_io_write(io, I915_XD_DPFC_CHICKEN_A, I915_XD_DPFC_CHICKEN_DUMMY_PIXEL);
		i915_xd_core_change(io, I915_XD_CLKREQ_POLICY, I915_XD_CLKREQ_POLICY_MEM_UP, 0U);
		return;
	}

	/* Display 11: the dummy pixel chicken written, the power response delayed. */
	if (platform->display_ver == 11U) {
		drv_i915_xd_io_write(io, I915_XD_DPFC_CHICKEN_A, I915_XD_DPFC_CHICKEN_DUMMY_PIXEL);
		i915_xd_core_change(io, I915_XD_CHICKEN_DCPR_1, 0U, I915_XD_DCPR_1_DELAY_PMRSP);
	}
}

/* Gives every DBUF slice of display 12 the tracker service of 8 (display 13 keeps the firmware's). */
static void
i915_xd_core_tracker(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	static const uint32_t controls[4] = {
		I915_XD_DBUF_CTL_S1, I915_XD_DBUF_CTL_S2, I915_XD_DBUF_CTL_S3, I915_XD_DBUF_CTL_S4
	};
	unsigned slice;
	uint32_t service;

	/* Only where the platform sets the service. */
	if (!platform->tracker_state_service)
		return;

	/* Each slice. */
	service = i915_xd_field_put(I915_XD_DBUF_TRACKER_STATE_SERVICE, I915_XD_CORE_TRACKER_SERVICE);
	for (slice = 0U; slice < platform->dbuf_slices && slice < 4U; slice++)
		drv_i915_xd_io_rmw(io, controls[slice], I915_XD_DBUF_TRACKER_STATE_SERVICE, service);
}

/* Powers the DBUF slices the firmware left on, and at least slice 1. */
static void
i915_xd_core_dbuf(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_core_hooks *hooks)
{
	static const uint32_t controls[4] = {
		I915_XD_DBUF_CTL_S1, I915_XD_DBUF_CTL_S2, I915_XD_DBUF_CTL_S3, I915_XD_DBUF_CTL_S4
	};
	uint32_t state;
	uint8_t slices;
	unsigned slice;

	/* The slices whose power is on, and slice 1. */
	slices = 0x1U;
	for (slice = 0U; slice < platform->dbuf_slices && slice < 4U; slice++) {
		state = drv_i915_xd_io_read(io, controls[slice]);
		if ((state & I915_XD_DBUF_POWER_STATE) != 0U)
			slices |= (uint8_t)(1U << slice);
	}

	/* The caller powers them. */
	hooks->dbuf(hooks->context, slices);
}

/* Gives the memory bus ABOXes of the platform their credits (display 12; ABOX0 included). */
static void
i915_xd_core_abox(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	static const uint32_t controls[3] = {
		I915_XD_MBUS_ABOX0_CTL, I915_XD_MBUS_ABOX1_CTL, I915_XD_MBUS_ABOX2_CTL
	};
	uint32_t mask;
	uint32_t word;
	unsigned abox;

	/* The fields and their credits. */
	mask = I915_XD_MBUS_ABOX_BT_CREDIT_POOL1 | I915_XD_MBUS_ABOX_BT_CREDIT_POOL2 |
	       I915_XD_MBUS_ABOX_B_CREDIT | I915_XD_MBUS_ABOX_BW_CREDIT;
	word = i915_xd_field_put(I915_XD_MBUS_ABOX_BT_CREDIT_POOL1, I915_XD_CORE_ABOX_POOL) |
	       i915_xd_field_put(I915_XD_MBUS_ABOX_BT_CREDIT_POOL2, I915_XD_CORE_ABOX_POOL) |
	       i915_xd_field_put(I915_XD_MBUS_ABOX_B_CREDIT, I915_XD_CORE_ABOX_B) |
	       i915_xd_field_put(I915_XD_MBUS_ABOX_BW_CREDIT, I915_XD_CORE_ABOX_BW);

	/* Each ABOX of the platform's mask (none on display 13). */
	for (abox = 0U; abox < 3U; abox++) {
		if ((platform->abox_credit_mask & (1U << abox)) == 0U)
			continue;
		drv_i915_xd_io_rmw(io, controls[abox], mask, word);
	}
}

/*
 * Programs the bandwidth buddies of the platform: the page mask of the
 * memory type and channels (display 12 also the TLB request timer), or
 * the buddy off for a memory the tables do not know.
 */
static void
i915_xd_core_buddy(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_core_input *input)
{
	const struct i915_xd_buddy_mask *row;
	unsigned buddy;

	/* The page mask of the memory. */
	row = drv_i915_xd_buddy_mask(platform, input->step, input->dram_type, input->dram_channels);

	/* Each buddy of the platform's mask. */
	for (buddy = 0U; buddy < 3U; buddy++) {
		if ((platform->abox_buddy_mask & (1U << buddy)) == 0U)
			continue;
		if (row == NULL) {
			drv_i915_xd_io_write(io, I915_XD_BW_BUDDY_CTL(buddy), I915_XD_BW_BUDDY_DISABLE);
			continue;
		}

		/* Writes the buddy's page mask, and on display 12 its request timer. */
		drv_i915_xd_io_write(io, I915_XD_BW_BUDDY_PAGE_MASK(buddy), row->page_mask);
		if (platform->display_ver == 12U) {
			drv_i915_xd_io_rmw(io, I915_XD_BW_BUDDY_CTL(buddy), I915_XD_BW_BUDDY_TLB_REQ_TIMER,
				i915_xd_field_put(I915_XD_BW_BUDDY_TLB_REQ_TIMER, I915_XD_CORE_BUDDY_TLB_TIMER));
		}
	}
}

/* Clears and sets bits of a register, writing it only when its value changes. */
static void
i915_xd_core_change(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old;
	uint32_t value;

	/* The register now and with the change. */
	old = drv_i915_xd_io_read(io, offset);
	value = (old & ~clear) | set;

	/* A register that holds the value already is not written. */
	if (value == old)
		return;

	/* Writes the new value. */
	drv_i915_xd_io_write(io, offset, value);
}
