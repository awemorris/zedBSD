/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_cdclk.c),
 * which carries the following notice.
 *
 * Copyright © 2006-2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_dpll_mgr.c),
 * which carries the following notice.
 *
 * Copyright © 2006-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * The display clocks: CDCLK and the shared DPLLs (see clock.h).
 *
 * The CDCLK half ports intel_cdclk.c's bxt_* path for display versions 12
 * and 13: intel_init_cdclk_hooks() selects the platform table, and
 * intel_cdclk_init_hw() reads the pre-OS state out (the DE PLL and
 * CDCLK_CTL), sanitizes it, and reprograms it only when it is not a legal
 * frequency / VCO pair for the platform -- with the PCODE prepare / notify
 * handshake around the PLL and CDCLK_CTL writes.  Alder Lake-P can crawl
 * the PLL; neither platform squashes.  The modeset's CDCLK check
 * (bxt_modeset_calc_cdclk() reduced to the one crtc) runs on the Linux text
 * of intel_cdclk.c.
 *
 * The DPLL half is the Linux text of intel_dpll_mgr.c for the combo PLLs:
 * the DP table and HDMI WRPLL calculations, the enable / disable sequences,
 * the readout and sanitize, and the device's pool of two combo PLLs that
 * every screen's modeset object shares.
 */

#include "modeset-internal.h"
#include "takeover-internal.h"
#include "clock.h"
#include <kern/kcrt.h>

#include "../mmio.h"
#include "../power.h"
#include "../sync.h"

#include <kern/klog.h>

#include <uapi/errno.h>

/* The CDCLK reference clock select of SKL_DSSM (i915_reg.h). */
#define SKL_DSSM				0x51004u
#define ICL_DSSM_CDCLK_PLL_REFCLK_MASK		(7u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_24MHz		(0u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz	(1u << 29)
#define ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz	(2u << 29)

/* CDCLK_CTL and its fields (i915_reg.h). */
#define I915_CDCLK_CTL				0x46000u
#define BXT_CDCLK_CD2X_DIV_SEL_MASK		(3u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_1		(0u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_1_5		(1u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_2		(2u << 22)
#define BXT_CDCLK_CD2X_DIV_SEL_4		(3u << 22)
#define CDCLK_FREQ_DECIMAL_MASK			0x7ffu
#define TGL_CDCLK_CD2X_PIPE_NONE		(7u << 19)	/* == ICL_CDCLK_CD2X_PIPE_NONE */

/* The DE PLL (i915_reg.h). */
#define I915_BXT_DE_PLL_ENABLE			0x46070u
#define BXT_DE_PLL_PLL_ENABLE			(1u << 31)
#define BXT_DE_PLL_LOCK				(1u << 30)
#define BXT_DE_PLL_FREQ_REQ			(1u << 23)
#define BXT_DE_PLL_FREQ_REQ_ACK			(1u << 22)
#define ICL_CDCLK_PLL_RATIO_MASK		0xffu

/* The PCODE CDCLK mailbox (i915_reg.h). */
#define SKL_PCODE_CDCLK_CONTROL			0x7u
#define SKL_CDCLK_PREPARE_FOR_CHANGE		0x3u
#define SKL_CDCLK_READY_FOR_CHANGE		0x1u

/* The pipe of a CDCLK change made outside a modeset (no vblank sync). */
#define I915_CDCLK_INVALID_PIPE			(-1)

/*
 * One row of a platform's CDCLK table as the modeset text reads it
 * (intel_cdclk.c's struct intel_cdclk_vals).
 *
 * Instances are the rows of the constant tables below; the modeset
 * object's device view points at one of them.
 */
struct intel_cdclk_vals {
	u32 cdclk;
	u16 refclk;
	u16 waveform;
	u8 divider;	/* CD2X divider * 2 */
	u8 ratio;
};

/*
 * The divider settings of one WRPLL frequency (intel_dpll_mgr.c's struct
 * skl_wrpll_params).
 *
 * A calculation fills one on the stack and packs it into the PLL words.
 */
struct skl_wrpll_params {
	u32 dco_fraction;
	u32 dco_integer;
	u32 qdiv_ratio;
	u32 qdiv_mode;
	u32 kdiv;
	u32 pdiv;
	u32 central_freq;
};

/*
 * One DP link rate's combo PLL settings (intel_dpll_mgr.c's struct
 * icl_combo_pll_params): a row of the constant DP tables.
 */
struct icl_combo_pll_params {
	int clock;
	struct skl_wrpll_params wrpll;
};

/* The CDCLK and combo PLL tables (Linux-derived, constant). */
#include "../intel/clock.h"

static uint32_t i915_divrc(uint32_t a, uint32_t b);
static int i915_hweight16(uint16_t w);
static uint8_t i915_cd_calc_voltage_level(int cdclk, int n, const int *max_cdclk);
static uint8_t i915_cdclk_calc_voltage_level(struct i915_cdclk_dev *cd, int cdclk);
static uint32_t i915_skl_cdclk_decimal(int cdclk);
static uint32_t i915_bxt_cdclk_cd2x_div_sel(struct i915_cdclk_dev *cd, int cdclk, int vco);
static uint16_t i915_cdclk_squash_waveform(struct i915_cdclk_dev *cd, int cdclk);
static int i915_cdclk_pll_is_unknown(uint32_t vco);
static void i915_icl_readout_refclk(struct i915_cdclk_dev *cd, struct i915_cdclk_config *cfg);
static void i915_bxt_de_pll_readout(struct i915_cdclk_dev *cd, struct i915_cdclk_config *cfg);
static void i915_icl_cdclk_pll_disable(struct i915_cdclk_dev *cd);
static void i915_icl_cdclk_pll_enable(struct i915_cdclk_dev *cd, int vco);
static void i915_icl_cdclk_pll_update(struct i915_cdclk_dev *cd, int vco);
static void i915_adlp_cdclk_pll_crawl(struct i915_cdclk_dev *cd, int vco);
static void i915_bxt_program_cdclk(struct i915_cdclk_dev *cd, const struct i915_cdclk_config *cfg, int pipe);
static int i915_bxt_cdclk_is_legal(struct i915_cdclk_dev *cd);
static void i915_bxt_cdclk_init_hw(struct i915_cdclk_dev *cd);
static int i915_bxt_calc_cdclk(struct drm_i915_private *dev_priv, int min_cdclk);
static int i915_bxt_calc_cdclk_pll_vco(struct drm_i915_private *dev_priv, int cdclk);
static u8 i915_calc_voltage_level(int cdclk, int num_voltage_levels, const int voltage_level_max_cdclk[]);
static u8 i915_tgl_calc_voltage_level(int cdclk);
static int i915_pixel_rate_to_cdclk(const struct intel_crtc_state *crtc_state);
static int i915_planes_min_cdclk(struct i915_takeover_world *takeover, const struct intel_crtc_state *crtc_state);
static bool i915_ehl_combo_pll_div_frac_wa_needed(struct drm_i915_private *i915);
static int i915_icl_calc_dp_combo_pll(struct intel_crtc_state *crtc_state, struct skl_wrpll_params *pll_params);
static void i915_icl_calc_dpll_state(struct drm_i915_private *i915, const struct skl_wrpll_params *pll_params, struct intel_dpll_hw_state *pll_state);
static i915_reg_t i915_combo_pll_enable_reg(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_icl_pll_power_enable(struct drm_i915_private *i915, struct intel_shared_dpll *pll, i915_reg_t enable_reg);
static void i915_icl_dpll_write(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_icl_pll_enable(struct drm_i915_private *i915, struct intel_shared_dpll *pll, i915_reg_t enable_reg);
static void i915_adlp_cmtg_clock_gating_wa(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_combo_pll_enable(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_icl_pll_disable(struct drm_i915_private *i915, struct intel_shared_dpll *pll, i915_reg_t enable_reg);
static void i915_combo_pll_disable(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_intel_enable_shared_dpll(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_enable_shared_dpll_locked(struct drm_i915_private *i915, struct intel_crtc *crtc, struct intel_shared_dpll *pll, unsigned int pipe_mask);
static void i915_intel_disable_shared_dpll(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_disable_shared_dpll_locked(struct drm_i915_private *i915, struct intel_crtc *crtc, struct intel_shared_dpll *pll, unsigned int pipe_mask);
static int i915_icl_wrpll_ref_clock(struct drm_i915_private *i915);
static void i915_icl_wrpll_get_multipliers(int bestdiv, int *pdiv, int *qdiv, int *kdiv);
static void i915_icl_wrpll_params_populate(struct skl_wrpll_params *params, u32 dco_freq, u32 ref_freq, int pdiv, int qdiv, int kdiv);
static int i915_icl_calc_wrpll(struct intel_crtc_state *crtc_state, struct skl_wrpll_params *wrpll_params);
static unsigned long i915_dpll_mask_all(struct drm_i915_private *i915);
static struct intel_shared_dpll *i915_find_shared_dpll(struct i915_lcd_world *world, struct intel_atomic_state *state, const struct intel_crtc *crtc, const struct intel_dpll_hw_state *pll_state, unsigned long dpll_mask);
static void i915_reference_shared_dpll_crtc(const struct intel_crtc *crtc, const struct intel_shared_dpll *pll, struct intel_shared_dpll_state *shared_dpll_state);
static void i915_reference_shared_dpll(struct i915_lcd_world *world, struct intel_atomic_state *state, const struct intel_crtc *crtc, const struct intel_shared_dpll *pll, const struct intel_dpll_hw_state *pll_state);
static void i915_unreference_shared_dpll(struct i915_lcd_world *world, struct intel_atomic_state *state, const struct intel_crtc *crtc, const struct intel_shared_dpll *pll);
static int i915_icl_ddi_combo_pll_get_freq(struct drm_i915_private *i915, const struct intel_shared_dpll *pll, const struct intel_dpll_hw_state *pll_state);
static bool i915_combo_pll_get_hw_state(struct drm_i915_private *i915, struct intel_shared_dpll *pll, struct intel_dpll_hw_state *hw_state);
static bool i915_icl_pll_get_hw_state(struct drm_i915_private *i915, struct intel_shared_dpll *pll, struct intel_dpll_hw_state *hw_state, i915_reg_t enable_reg);
static void i915_icl_pll_read_config(struct drm_i915_private *i915, struct intel_shared_dpll *pll, struct intel_dpll_hw_state *hw_state);
static void i915_readout_dpll_hw_state(struct i915_takeover_world *takeover, struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_sanitize_dpll_state(struct drm_i915_private *i915, struct intel_shared_dpll *pll);
static void i915_lcd_dpll_pool_init(struct i915_lcd_world *world, struct i915_lcd_modeset *ms);

/*
 * The hooks of a combo PLL of these platforms: the reference's
 * combo_pll_funcs, with the readout half the readout uses.  Constant and
 * shared by every PLL of every device.
 */
static const struct intel_shared_dpll_funcs i915_combo_pll_funcs = {
	.get_hw_state = i915_combo_pll_get_hw_state,
	.get_freq = i915_icl_ddi_combo_pll_get_freq,
	.enable = i915_combo_pll_enable,
	.disable = i915_combo_pll_disable,
};

/*
 * Returns Alder Lake-P's CDCLK table in the display core's layout.
 */
const struct i915_cdclk_vals *
drv_i915_adlp_cdclk_table(void)
{
	/* Succeeded: the table is constant and shared. */
	return i915_adlp_cdclk_table;
}

/*
 * Returns the CDCLK table Tiger Lake uses (icl_cdclk_table[]) in the
 * display core's layout.
 */
const struct i915_cdclk_vals *
drv_i915_icl_cdclk_table(void)
{
	/* Succeeded: the table is constant and shared. */
	return i915_icl_cdclk_table;
}

/*
 * Maps an Alder Lake-P PCI revision id to its display stepping (the
 * entries of adlp_revids[] this driver needs).
 */
int
drv_i915_adlp_display_step(
	uint8_t revid)
{
	/* Looks the revision up. */
	switch (revid) {
	case 0x0:
		return I915_STEP_A0;
	case 0x4:
		return I915_STEP_B0;
	case 0x8:
		return I915_STEP_C0;
	case 0xC:
		return I915_STEP_D0;
	default:
		break;
	}

	/* A revision the table does not list has no known stepping. */
	return I915_STEP_NONE;
}

/*
 * Selects the platform's CDCLK table and hooks (intel_init_cdclk_hooks()).
 */
void
drv_i915_init_cdclk_hooks(
	struct i915_cdclk_dev *cd,
	int display_ver,
	int display_step,
	int is_alderlake_p)
{
	/* Records the display version the hooks serve. */
	cd->display_ver = display_ver;

	/*
	 * Display version 12 that is not Alder Lake-P is Tiger Lake:
	 * icl_cdclk_table.  Alder Lake-P takes the a-step table for [A0, B0)
	 * (Wa_22011320316; not this device, which takes adlp_cdclk_table
	 * too) and adlp_cdclk_table with tgl_cdclk_funcs for every later
	 * stepping.  RPL-U is a distinct SKU that is not matched.
	 */
	if (!is_alderlake_p && display_ver == 12) {
		cd->table = i915_icl_cdclk_table;
	} else if (is_alderlake_p && display_ver >= 12) {
		if (display_step >= I915_STEP_A0 && display_step < I915_STEP_B0) {
			/* adlp_a_step_cdclk_table would be this stepping's. */
			cd->table = i915_adlp_cdclk_table;
			cd->funcs = I915_CDCLK_FUNCS_TGL;
		} else {
			cd->table = i915_adlp_cdclk_table;
			cd->funcs = I915_CDCLK_FUNCS_TGL;
		}
	} else {
		cd->table = i915_adlp_cdclk_table;
		cd->funcs = I915_CDCLK_FUNCS_TGL;
	}

	/* The platform capabilities (xe_lpd_display: crawl yes, squash no). */
	cd->has_cdclk_crawl = 1;
	cd->has_cdclk_squash = 0;
}

/*
 * Reports the voltage level a CDCLK needs (tgl_calc_voltage_level()).
 */
uint8_t
drv_i915_tgl_calc_voltage_level(
	int cdclk)
{
	static const int tgl_max[] = { 312000, 326400, 556800, 652800 };
	uint8_t level;

	/* Finds the first level whose maximum covers the frequency. */
	level = i915_cd_calc_voltage_level(cdclk, 4, tgl_max);

	/* Succeeded: reports the level. */
	return level;
}

/*
 * Reports the lowest table CDCLK at or above a minimum for the current
 * reference clock (bxt_calc_cdclk()), or 0 when none is.
 */
int
drv_i915_bxt_calc_cdclk(
	struct i915_cdclk_dev *cd,
	int min_cdclk)
{
	const struct i915_cdclk_vals *table;
	int index;

	/* Looks for the first row of the reference clock that is fast enough. */
	table = cd->table;
	for (index = 0; table[index].refclk != 0u; index++) {
		if (table[index].refclk == cd->hw.ref && (int)table[index].cdclk >= min_cdclk)
			return (int)table[index].cdclk;
	}

	/* No row satisfies the minimum. */
	kern_logf("i915: cdclk: cannot satisfy min cdclk %d @ refclk %u\n",
		min_cdclk, cd->hw.ref);
	return 0;
}

/*
 * Reports the PLL VCO of a table CDCLK (bxt_calc_cdclk_pll_vco()): 0 for
 * the bypass frequency and for a frequency the table does not list.
 */
int
drv_i915_bxt_calc_cdclk_pll_vco(
	struct i915_cdclk_dev *cd,
	int cdclk)
{
	const struct i915_cdclk_vals *table;
	int index;

	/* The bypass frequency runs with the PLL off. */
	table = cd->table;
	if ((uint32_t)cdclk == cd->hw.bypass)
		return 0;

	/* Looks the frequency up for the reference clock. */
	for (index = 0; table[index].refclk != 0u; index++) {
		if (table[index].refclk == cd->hw.ref && (int)table[index].cdclk == cdclk)
			return (int)(cd->hw.ref * table[index].ratio);
	}

	/* The frequency is not valid for the reference clock. */
	kern_logf("i915: cdclk: cdclk %d not valid for refclk %u\n",
		cdclk, cd->hw.ref);
	return 0;
}

/*
 * Reads the CDCLK state out of the hardware (bxt_get_cdclk()).
 *
 * The voltage level cannot be read back; it is set to at least what the
 * frequency requires.
 */
void
drv_i915_bxt_get_cdclk(
	struct i915_cdclk_dev *cd,
	struct i915_cdclk_config *cfg)
{
	uint32_t divider;
	int div;

	/* Reads the reference clock and the PLL. */
	i915_bxt_de_pll_readout(cd, cfg);

	/* Display version 12+: the bypass frequency is half the reference. */
	cfg->bypass = cfg->ref / 2u;

	/* With the PLL off the CDCLK is the bypass frequency. */
	if (cfg->vco == 0u) {
		cfg->cdclk = cfg->bypass;
		cfg->voltage_level = i915_cdclk_calc_voltage_level(cd, (int)cfg->cdclk);
		return;
	}

	/* Decodes the CD2X divider. */
	divider = drv_i915_raw_read32(cd->m, I915_CDCLK_CTL) & BXT_CDCLK_CD2X_DIV_SEL_MASK;
	switch (divider) {
	case BXT_CDCLK_CD2X_DIV_SEL_1:
		div = 2;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_1_5:
		div = 3;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_2:
		div = 4;
		break;
	case BXT_CDCLK_CD2X_DIV_SEL_4:
		div = 8;
		break;
	default:
		/* MISSING_CASE: the reference returns early and leaves the frequency unset. */
		return;
	}

	/* No squash on these platforms: the frequency is the VCO over the divider. */
	cfg->cdclk = i915_divrc(cfg->vco, (uint32_t)div);

	/* Assumes at least the voltage level the frequency requires. */
	cfg->voltage_level = i915_cdclk_calc_voltage_level(cd, (int)cfg->cdclk);
}

/*
 * Reads the current CDCLK state into the device (intel_update_cdclk()).
 */
void
drv_i915_update_cdclk(
	struct i915_cdclk_dev *cd)
{
	/*
	 * intel_cdclk_get_cdclk() is bxt_get_cdclk() here; the GMBUSFREQ_VLV
	 * update is VLV / CHV only.
	 */
	drv_i915_bxt_get_cdclk(cd, &cd->hw);
}

/*
 * Programs a CDCLK state (bxt_set_cdclk()): the PCODE prepare, the PLL and
 * CDCLK_CTL writes, the PCODE voltage notify and the readout.
 *
 * A failed prepare writes nothing; a failed notify leaves the hardware
 * changed and the software state as it was.
 */
void
drv_i915_bxt_set_cdclk(
	struct i915_cdclk_dev *cd,
	const struct i915_cdclk_config *cfg)
{
	int cdclk;
	int pcode_result;

	/* Records the request. */
	cdclk = (int)cfg->cdclk;
	cd->diag_requested = *cfg;

	/*
	 * Informs the PCU of the change (display version 11+, not DG2):
	 * skl_pcode_request(CDCLK_CONTROL, PREPARE, READY, READY, 3).
	 */
	pcode_result = drv_i915_skl_pcode_request(cd->sb_lock, cd->m, SKL_PCODE_CDCLK_CONTROL,
		SKL_CDCLK_PREPARE_FOR_CHANGE, SKL_CDCLK_READY_FOR_CHANGE,
		SKL_CDCLK_READY_FOR_CHANGE, 3);
	cd->diag_prepare_status = pcode_result;
	if (pcode_result != 0) {
		kern_logf("i915: cdclk: PCU prepare failed (err %d, freq %d)\n",
			pcode_result, cdclk);
		return;
	}

	/*
	 * The crawl-and-squash midpoint needs both; without squash one
	 * programming step does it.
	 */
	i915_bxt_program_cdclk(cd, cfg, I915_CDCLK_INVALID_PIPE);

	/* Notifies the new voltage level (display version 11+, not DG2). */
	pcode_result = drv_i915_snb_pcode_write(cd->sb_lock, cd->m, SKL_PCODE_CDCLK_CONTROL,
		cfg->voltage_level);
	cd->diag_notify_status = pcode_result;
	if (pcode_result != 0) {
		/* The hardware already changed: the state update is not faked. */
		kern_logf("i915: cdclk: PCODE freq set failed (err %d, freq %d)\n",
			pcode_result, cdclk);
		return;
	}

	/* Reads the new state back. */
	drv_i915_update_cdclk(cd);

	/* The voltage level cannot be read back: it is what was requested. */
	cd->hw.voltage_level = cfg->voltage_level;
}

/*
 * Accepts a legal pre-OS CDCLK state or forces a full reprogram
 * (bxt_sanitize_cdclk()).
 *
 * A forced reprogram is recorded as cdclk 0 and VCO ~0.
 */
void
drv_i915_bxt_sanitize_cdclk(
	struct i915_cdclk_dev *cd)
{
	int legal;

	/* Reads what the firmware left. */
	drv_i915_update_cdclk(cd);
	cd->diag_observed_before = cd->hw;

	/* Keeps a legal state as it is. */
	legal = i915_bxt_cdclk_is_legal(cd);
	if (legal) {
		cd->diag_sanitized = cd->hw;
		return;
	}

	/* Forces the CDCLK programming and a full PLL disable and enable. */
	kern_logf("i915: cdclk: sanitizing cdclk programmed by pre-os\n");
	cd->hw.cdclk = 0;
	cd->hw.vco = ~0u;
	cd->diag_sanitized = cd->hw;
}

/*
 * Brings the CDCLK up (intel_cdclk_init_hw() -> bxt_cdclk_init_hw()).
 */
void
drv_i915_cdclk_init_hw(
	struct i915_cdclk_dev *cd)
{
	/* Display version 10+ (and BXT) takes the bxt path. */
	i915_bxt_cdclk_init_hw(cd);
}

/*
 * Reports the highest CDCLK of the platform (intel_update_max_cdclk(),
 * display version 11+ except JSL / EHL).
 */
uint32_t
drv_i915_max_cdclk_freq(
	const struct i915_cdclk_dev *cd)
{
	/* 648 MHz on a 24 MHz reference, 652.8 MHz otherwise. */
	if (cd->hw.ref == 24000u)
		return 648000u;

	/* Succeeded: the 19.2 / 38.4 MHz maximum. */
	return 652800u;
}

/*
 * Computes the CDCLK a crtc needs (intel_crtc_compute_min_cdclk()).
 *
 * The planes of the crtc are walked in the takeover registry, which the
 * Linux text reached through a file-scope accessor: they are found only
 * while the registry is live (NULL: no registry, no planes).
 */
int
drv_i915_crtc_compute_min_cdclk(
	struct i915_takeover_world *takeover,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	int min_cdclk;
	int planes_min;
	int display_ver;
	bool has_dp;
	bool has_dsi;

	/* Resolves the device of the crtc. */
	dev_priv = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);

	/* A crtc that is not enabled needs nothing. */
	if (!crtc_state->hw.enable)
		return 0;

	/* Starts from the pixel rate. */
	min_cdclk = i915_pixel_rate_to_cdclk(crtc_state);

	/* The pixel rate must not exceed 95% of the CDCLK with IPS on BDW. */
	if (IS_BROADWELL(dev_priv) && hsw_crtc_state_ips_capable(crtc_state))
		min_cdclk = DIV_ROUND_UP(min_cdclk * 100, 95);

	/*
	 * BSpec: "Do not use DisplayPort with CDCLK less than 432 MHz, audio
	 * enabled, port width x4, and link rate HBR2 (5.4 GHz), or else there
	 * may be audio corruption or screen corruption."  The GLK restriction
	 * is 316.8 MHz.
	 */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	has_dp = intel_crtc_has_dp_encoder(crtc_state);
	if (has_dp &&
	    crtc_state->has_audio &&
	    crtc_state->port_clock >= 540000 &&
	    crtc_state->lane_count == 4) {
		if (display_ver == 10) {
			/* Display WA #1145: glk */
			min_cdclk = max(316800, min_cdclk);
		} else if (display_ver == 9 || IS_BROADWELL(dev_priv)) {
			/* Display WA #1144: skl,bxt */
			min_cdclk = max(432000, min_cdclk);
		}
	}

	/*
	 * BSpec: "The CD clock frequency must be at least twice the frequency
	 * of the Azalia BCLK", and BCLK is 96 MHz by default.
	 */
	if (crtc_state->has_audio && display_ver >= 9)
		min_cdclk = max(2 * 96000, min_cdclk);

	/*
	 * "For DP audio configuration, cdclk frequency shall be set to meet the
	 * following requirements: 270 MHz link -> 320 MHz or higher, 162 MHz
	 * link -> 200 MHz or higher."
	 */
	if ((IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) &&
	    has_dp && crtc_state->has_audio)
		min_cdclk = max(crtc_state->port_clock, min_cdclk);

	/* On Valleyview some DSI panels lose (v|h)sync when the clock is lower than 320 MHz. */
	has_dsi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DSI);
	if (has_dsi && IS_VALLEYVIEW(dev_priv))
		min_cdclk = max(320000, min_cdclk);

	/*
	 * On Geminilake, once the CDCLK gets as low as 79.2 MHz the picture
	 * gets unstable, although the DSI PLL and DE PLL values are correct.
	 */
	if (has_dsi && IS_GEMINILAKE(dev_priv))
		min_cdclk = max(158400, min_cdclk);

	/* Accounts for the additional needs of the planes. */
	planes_min = i915_planes_min_cdclk(takeover, crtc_state);
	min_cdclk = max(planes_min, min_cdclk);

	/* DSC is not part of this path; reaching it is reported. */
	if (crtc_state->dsc.compression_enable)
		min_cdclk = max(min_cdclk, intel_vdsc_min_cdclk(crtc_state));

	/*
	 * HACK of the reference: on TGL / DG2 the lowest CDCLK computed from
	 * half the pixel rate can underrun, so the pixel rate itself is the
	 * minimum, clamped to the maximum CDCLK so 8K still works.
	 */
	if (I915_LCD_IS_TIGERLAKE(dev_priv) || IS_DG2(dev_priv)) {
		min_cdclk = max_t(int, min_cdclk,
				  min_t(int, crtc_state->pixel_rate,
					dev_priv->display.cdclk.max_cdclk_freq));
	}

	/* Succeeded: reports the crtc's CDCLK need. */
	return min_cdclk;
}

/*
 * Checks the modeset's CDCLK need against the state the normal
 * initialization left (bxt_modeset_calc_cdclk() for the one crtc, then
 * intel_cdclk_changed()).
 *
 * Nothing is programmed: when the required state equals the current one
 * the reference's pre / post plane updates leave the hardware alone, and
 * that is the only case the modeset accepts.  Returns 0, or I915_LCD_EINVAL
 * when the need cannot be met.
 */
int
drv_i915_lcd_ms_cdclk_check(
	struct i915_takeover_world *takeover,
	struct i915_lcd_modeset *ms)
{
	struct drm_i915_private *i915;
	const struct intel_crtc_state *cs;
	int min_cdclk;
	int cdclk;
	int vco;
	int level;
	int display_ver;
	int change_needed;

	/* Resolves the modeset object's device and crtc state. */
	i915 = &ms->i915;
	cs = &ms->crtc_state;

	/*
	 * intel_init_cdclk_hooks(): the table of this display -- Alder Lake-P
	 * past B0 uses adlp_cdclk_table, Tiger Lake icl_cdclk_table.  It must
	 * be the table the probe programmed the hardware from, or every mode
	 * looks like a CDCLK change.
	 */
	display_ver = drv_i915_lcd_display_ver();
	i915->display.cdclk.table = icl_cdclk_table;
	if (display_ver >= 13)
		i915->display.cdclk.table = adlp_cdclk_table;

	/* intel_compute_min_cdclk(): the crtc's need. */
	min_cdclk = drv_i915_crtc_compute_min_cdclk(takeover, cs);
	if (min_cdclk < 0)
		return -min_cdclk;

	/* And the bandwidth need (no forced minimum). */
	ms->cdclk.crtc_min = min_cdclk;
	ms->cdclk.bw_min = drv_i915_lcd_ms_bw_min_cdclk(ms);
	min_cdclk = max(ms->cdclk.bw_min, min_cdclk);
	if (min_cdclk > (int)i915->display.cdclk.max_cdclk_freq)
		return I915_LCD_EINVAL;

	/* bxt_modeset_calc_cdclk(): the frequency, its VCO and the voltage level. */
	cdclk = i915_bxt_calc_cdclk(i915, min_cdclk);
	vco = i915_bxt_calc_cdclk_pll_vco(i915, cdclk);
	level = max_t(int, cs->min_voltage_level, i915_tgl_calc_voltage_level(cdclk));
	if (cdclk == 0 || vco == 0)
		return I915_LCD_EINVAL;

	/* Records the required state. */
	ms->cdclk.min_cdclk = min_cdclk;
	ms->cdclk.cdclk = cdclk;
	ms->cdclk.vco = vco;
	ms->cdclk.voltage_level = level;

	/*
	 * intel_cdclk_changed(): intel_cdclk_needs_modeset() (the frequency,
	 * VCO or reference differ) or the voltage level differs.
	 */
	change_needed = 0;
	if (cdclk != (int)i915->display.cdclk.hw.cdclk)
		change_needed = 1;
	else if (vco != (int)i915->display.cdclk.hw.vco)
		change_needed = 1;
	else if (level != (int)i915->display.cdclk.hw.voltage_level)
		change_needed = 1;
	ms->cdclk.change_needed = change_needed;

	/* Succeeded: the need is recorded against the current state. */
	return 0;
}

/*
 * Enables a crtc's shared DPLL (intel_enable_shared_dpll()).
 *
 * The first pipe to use the PLL turns it on; a later one only joins its
 * active mask.
 */
void
drv_i915_enable_shared_dpll(
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *i915;
	struct intel_shared_dpll *pll;
	unsigned int pipe_mask;

	/* Resolves the crtc, its device, its PLL and its pipe bit. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915 = i915_lcd_to_i915(crtc->base.dev);
	pll = crtc_state->shared_dpll;
	pipe_mask = BIT(crtc->pipe);

	/* A crtc without a PLL is a warning of the Linux text. */
	if (pll == NULL) {
		drv_i915_lcd_error("WARN_ON(pll == NULL)\n");
		return;
	}

	/*
	 * Joins or turns on the PLL under the device's DPLL lock.  The Linux
	 * text reached the lock through the device the entry point had
	 * selected; the crtc's device is that device.
	 */
	I915_LCD_MUTEX_LOCK(i915, &i915->display.dpll.lock);

	i915_enable_shared_dpll_locked(i915, crtc, pll, pipe_mask);

	I915_LCD_MUTEX_UNLOCK(i915, &i915->display.dpll.lock);
}

/*
 * Disables a crtc's shared DPLL (intel_disable_shared_dpll()).
 *
 * The last pipe to leave the PLL turns it off.
 */
void
drv_i915_disable_shared_dpll(
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *i915;
	struct intel_shared_dpll *pll;
	unsigned int pipe_mask;

	/* Resolves the crtc, its device, its PLL and its pipe bit. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915 = i915_lcd_to_i915(crtc->base.dev);
	pll = crtc_state->shared_dpll;
	pipe_mask = BIT(crtc->pipe);

	/* PCH PLLs exist only on ILK+. */
	if (I915_LCD_DISPLAY_VER(i915) < 5)
		return;

	/* A crtc without a PLL has nothing to disable. */
	if (pll == NULL)
		return;

	/* Leaves or turns off the PLL under the device's DPLL lock. */
	I915_LCD_MUTEX_LOCK(i915, &i915->display.dpll.lock);

	i915_disable_shared_dpll_locked(i915, crtc, pll, pipe_mask);

	I915_LCD_MUTEX_UNLOCK(i915, &i915->display.dpll.lock);
}

/*
 * Finds the DPLL with an id in a device's pool (intel_get_shared_dpll_by_id()).
 */
struct intel_shared_dpll *
drv_i915_get_shared_dpll_by_id(
	struct drm_i915_private *i915,
	enum intel_dpll_id id)
{
	struct intel_shared_dpll *pll;
	int index;

	/* Looks the id up in the pool (for_each_shared_dpll). */
	for (index = 0; index < i915->display.dpll.num_shared_dpll; index++) {
		pll = &i915->display.dpll.shared_dplls[index];
		if (pll->info->id == id)
			return pll;
	}

	/* No PLL has the id. */
	I915_LCD_MISSING_CASE(id);
	return NULL;
}

/*
 * Drops a crtc's reference on a DPLL (intel_unreference_shared_dpll_crtc()).
 */
void
drv_i915_unreference_shared_dpll_crtc(
	const struct intel_crtc *crtc,
	const struct intel_shared_dpll *pll,
	struct intel_shared_dpll_state *shared_dpll_state)
{
	/* A crtc that holds no reference is a warning of the Linux text. */
	if ((shared_dpll_state->pipe_mask & BIT(crtc->pipe)) == 0)
		drv_i915_lcd_error("WARN_ON((shared_dpll_state->pipe_mask & BIT(crtc->pipe)) == 0)\n");

	/* The pipe mask names the crtcs that use the PLL. */
	shared_dpll_state->pipe_mask &= ~BIT(crtc->pipe);

	/* Notes the release (on the crtc's DRM device). */
	I915_LCD_DRM_DBG_KMS(crtc->base.dev, "[CRTC:%d:%s] releasing %s\n",
		crtc->base.base.id, crtc->base.name, pll->info->name);
}

/*
 * Calculates a DPLL's output frequency from a state (intel_dpll_get_freq()):
 * 0 when the PLL has no frequency hook.
 */
int
drv_i915_dpll_get_freq(
	struct drm_i915_private *i915,
	const struct intel_shared_dpll *pll,
	const struct intel_dpll_hw_state *pll_state)
{
	int freq;

	/* A PLL without the hook is a warning of the Linux text. */
	if (pll->info->funcs->get_freq == NULL) {
		drv_i915_lcd_error("WARN_ON(!pll->info->funcs->get_freq)\n");
		return 0;
	}

	/* Asks the PLL kind. */
	freq = pll->info->funcs->get_freq(i915, pll, pll_state);

	/* Succeeded: reports the frequency. */
	return freq;
}

/*
 * Reads a DPLL's hardware state (intel_dpll_get_hw_state()): true when
 * the PLL is enabled and the state was read.
 */
bool
drv_i915_dpll_get_hw_state(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	struct intel_dpll_hw_state *hw_state)
{
	bool enabled;

	/* Asks the PLL kind. */
	enabled = pll->info->funcs->get_hw_state(i915, pll, hw_state);

	/* Succeeded: reports whether the PLL is enabled. */
	return enabled;
}

/*
 * Reads every DPLL of the pool out (intel_dpll_readout_hw_state()).
 *
 * The crtcs whose readout uses a PLL are found in the takeover registry.
 */
void
drv_i915_n1_dpll_readout_hw_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	int index;

	/* Reads each PLL of the pool (for_each_shared_dpll). */
	for (index = 0; index < i915->display.dpll.num_shared_dpll; index++)
		i915_readout_dpll_hw_state(takeover, i915, &i915->display.dpll.shared_dplls[index]);
}

/*
 * Turns off every DPLL that is on but used by no pipe
 * (intel_dpll_sanitize_state()).
 */
void
drv_i915_dpll_sanitize_state(
	struct drm_i915_private *i915)
{
	int index;

	/* Sanitizes each PLL of the pool (for_each_shared_dpll). */
	for (index = 0; index < i915->display.dpll.num_shared_dpll; index++)
		i915_sanitize_dpll_state(i915, &i915->display.dpll.shared_dplls[index]);
}

/*
 * Computes the combo PLL words of an HDMI TMDS clock (icl_calc_wrpll() and
 * icl_calc_dpll_state()).
 *
 * port_clock is the TMDS clock in kHz and ref_nssc the reference clock.
 * Returns 0, or I915_LCD_EINVAL when no divider fits.
 */
int
drv_i915_icl_hdmi_wrpll(
	int port_clock,
	int ref_nssc,
	uint32_t *cfgcr0,
	uint32_t *cfgcr1,
	uint32_t *div0)
{
	struct drm_i915_private i915;
	struct intel_crtc crtc;
	struct intel_crtc_state cs;
	struct skl_wrpll_params params;
	struct intel_dpll_hw_state hw;
	int calc_result;

	/* Builds a device view with only the reference clock set. */
	kern_memset(&i915, 0, sizeof(i915));
	kern_memset(&crtc, 0, sizeof(crtc));
	kern_memset(&cs, 0, sizeof(cs));
	kern_memset(&params, 0, sizeof(params));
	kern_memset(&hw, 0, sizeof(hw));
	i915.display.dpll.ref_clks.nssc = ref_nssc;

	/* Links a crtc state of the clock to it. */
	crtc.base.dev = &i915.drm;
	cs.uapi.crtc = &crtc.base;
	cs.port_clock = port_clock;

	/* Chooses the dividers. */
	calc_result = i915_icl_calc_wrpll(&cs, &params);
	if (calc_result != 0)
		return calc_result;

	/* Packs them into the PLL words. */
	i915_icl_calc_dpll_state(&i915, &params, &hw);
	*cfgcr0 = hw.cfgcr0;
	*cfgcr1 = hw.cfgcr1;
	*div0 = hw.div0;

	/* Succeeded: the words are the caller's. */
	return 0;
}

/*
 * Computes the combo PLL words of a DP port clock (icl_calc_dp_combo_pll()
 * and icl_calc_dpll_state(), which halves the DCO fraction when the
 * reference clock is 38.4 MHz: Display WA #22010492432).
 *
 * Returns 0, or I915_LCD_EINVAL for a link rate the table does not list.
 */
int
drv_i915_icl_dp_combo_pll(
	int port_clock,
	int ref_nssc,
	uint32_t *cfgcr0,
	uint32_t *cfgcr1,
	uint32_t *div0)
{
	struct drm_i915_private i915;
	struct drm_crtc crtc;
	struct intel_crtc_state crtc_state;
	struct skl_wrpll_params params;
	struct intel_dpll_hw_state hw;
	int calc_result;

	/*
	 * Builds a device view with only the reference clock set.  The old
	 * text kept the view in a function static cleared at every call; an
	 * automatic one holds the same value and nothing survives the call.
	 */
	kern_memset(&i915, 0, sizeof(i915));
	kern_memset(&crtc_state, 0, sizeof(crtc_state));
	kern_memset(&params, 0, sizeof(params));
	kern_memset(&hw, 0, sizeof(hw));
	i915.display.dpll.ref_clks.nssc = ref_nssc;

	/* Links a crtc state of the clock to it. */
	crtc.dev = &i915.drm;
	crtc_state.uapi.crtc = &crtc;
	crtc_state.port_clock = port_clock;

	/* Looks the link rate up. */
	calc_result = i915_icl_calc_dp_combo_pll(&crtc_state, &params);
	if (calc_result != 0)
		return calc_result;

	/* Packs the settings into the PLL words. */
	i915_icl_calc_dpll_state(&i915, &params, &hw);
	*cfgcr0 = hw.cfgcr0;
	*cfgcr1 = hw.cfgcr1;
	*div0 = hw.div0;

	/* Succeeded: the words are the caller's. */
	return 0;
}

/*
 * Returns the pool's shared_dpll[] state of a world (the atomic state's
 * shared DPLL state).
 */
struct intel_shared_dpll_state *
drv_i915_lcd_shared_dpll_state(
	struct i915_lcd_world *world)
{
	/* Succeeded: the pool state lives in the world. */
	return world->i915_lcd_dpll_pool_state;
}

/*
 * Binds a device view to the world's pool of two combo PLLs, building the
 * pool on first use as intel_shared_dpll_init() leaves it.
 *
 * One pool serves every screen: two modeset objects that want the same
 * hardware state share one PLL object, and its active mask counts the
 * pipes that drive it, so one screen's stop never turns off a PLL the
 * other still uses.
 */
void
drv_i915_lcd_dpll_pool_bind(
	struct i915_lcd_world *world,
	struct drm_i915_private *i915)
{
	int index;

	/* Builds DPLL 0 and DPLL 1 once (the reference's adlp_plls[]: no power domain). */
	if (!world->i915_lcd_dpll_pool_inited) {
		for (index = 0; index < 2; index++) {
			kern_memset(&world->i915_lcd_dpll_pool[index], 0, sizeof(world->i915_lcd_dpll_pool[index]));
			kern_memset(&world->i915_lcd_dpll_pool_state[index], 0, sizeof(world->i915_lcd_dpll_pool_state[index]));

			/* Describes the PLL. */
			if (index == DPLL_ID_ICL_DPLL1) {
				world->i915_lcd_dpll_pool_info[index].name = "DPLL 1";
				world->i915_lcd_dpll_pool_info[index].id = DPLL_ID_ICL_DPLL1;
			} else {
				world->i915_lcd_dpll_pool_info[index].name = "DPLL 0";
				world->i915_lcd_dpll_pool_info[index].id = DPLL_ID_ICL_DPLL0;
			}

			/* Binds the combo PLL hooks, no power domain. */
			world->i915_lcd_dpll_pool_info[index].funcs = &i915_combo_pll_funcs;
			world->i915_lcd_dpll_pool_info[index].power_domain = 0;

			/* Links the PLL to its description and its place in the pool. */
			world->i915_lcd_dpll_pool[index].info = &world->i915_lcd_dpll_pool_info[index];
			world->i915_lcd_dpll_pool[index].index = (enum intel_dpll_id)index;
		}

		/* The pool is built. */
		world->i915_lcd_dpll_pool_inited = 1;
	}

	/* Points the device view at the pool. */
	i915->display.dpll.shared_dplls = world->i915_lcd_dpll_pool;
	i915->display.dpll.num_shared_dpll = 2;
}

/*
 * Gives a pipe's reference on every PLL of the pool back
 * (intel_release_shared_dplls()).
 */
void
drv_i915_lcd_ms_release_pipe(
	struct i915_lcd_world *world,
	enum pipe pipe)
{
	int index;

	/* Clears the pipe from each PLL's state and from its atomic state. */
	for (index = 0; index < 2; index++) {
		world->i915_lcd_dpll_pool_state[index].pipe_mask &= (u8)~BIT(pipe);
		world->i915_lcd_dpll_pool[index].state.pipe_mask &= (u8)~BIT(pipe);
	}
}

/*
 * Empties the world's pool, as intel_shared_dpll_init() leaves it when the
 * device is (re)created.
 */
void
drv_i915_lcd_dplls_reset(
	struct i915_lcd_world *world)
{
	int index;

	/* The pool is rebuilt on its next use. */
	world->i915_lcd_dpll_pool_inited = 0;

	/* Clears both PLLs and their atomic states. */
	for (index = 0; index < 2; index++) {
		kern_memset(&world->i915_lcd_dpll_pool[index], 0, sizeof(world->i915_lcd_dpll_pool[index]));
		kern_memset(&world->i915_lcd_dpll_pool_state[index], 0, sizeof(world->i915_lcd_dpll_pool_state[index]));
	}
}

/*
 * Chooses the PLL of the pool that carries a modeset's PLL state (the
 * allocation half of icl_get_combo_phy_dpll()).
 *
 * The state was computed by the caller.  Returns the DPLL id, or
 * -I915_LCD_EINVAL (a refusal, not an id) when both PLLs are taken by
 * other pipes.
 */
int
drv_i915_lcd_ms_alloc_pll(
	struct i915_lcd_world *world,
	struct i915_lcd_modeset *ms,
	const struct intel_dpll_hw_state *hw_state)
{
	struct intel_shared_dpll *pll;

	/* Binds the pool and gives back what this pipe held. */
	i915_lcd_dpll_pool_init(world, ms);
	drv_i915_lcd_ms_release_pipe(world, ms->crtc.pipe);

	/* Finds a PLL with the same state, or a free one. */
	pll = i915_find_shared_dpll(world, &ms->state, &ms->crtc, hw_state,
		BIT(DPLL_ID_ICL_DPLL0) | BIT(DPLL_ID_ICL_DPLL1));
	if (pll == NULL)
		return -I915_LCD_EINVAL;

	/* References it for the crtc; the object carries the new state. */
	i915_reference_shared_dpll(world, &ms->state, &ms->crtc, pll, hw_state);
	ms->crtc_state.shared_dpll = pll;
	pll->state = world->i915_lcd_dpll_pool_state[pll->index];

	/* Succeeded: reports the PLL's id. */
	return (int)pll->info->id;
}

/*
 * Gives a modeset's reference on its PLL back (intel_release_shared_dplls());
 * the object stays for the other pipe.
 */
void
drv_i915_lcd_ms_release_pll(
	struct i915_lcd_world *world,
	struct i915_lcd_modeset *ms)
{
	struct intel_shared_dpll *pll;

	/* A crtc without a PLL has nothing to give back. */
	pll = ms->crtc_state.shared_dpll;
	if (pll == NULL)
		return;

	/* Drops the crtc's reference and copies the pipe mask back to the object. */
	i915_unreference_shared_dpll(world, &ms->state, &ms->crtc, pll);
	pll->state.pipe_mask = world->i915_lcd_dpll_pool_state[pll->index].pipe_mask;
}

/*
 * Binds a modeset object's own PLL: the reference's adlp_plls[] entry for
 * DPLL 0 or DPLL 1 (no power domain), used by this crtc alone.
 */
void
drv_i915_lcd_ms_bind_pll(
	struct i915_lcd_modeset *ms,
	int dpll_id)
{
	/* Describes the PLL. */
	if (dpll_id == DPLL_ID_ICL_DPLL1) {
		ms->pll_info.name = "DPLL 1";
		ms->pll_info.id = DPLL_ID_ICL_DPLL1;
	} else {
		ms->pll_info.name = "DPLL 0";
		ms->pll_info.id = DPLL_ID_ICL_DPLL0;
	}

	/* Binds the combo PLL hooks, no power domain. */
	ms->pll_info.funcs = &i915_combo_pll_funcs;
	ms->pll_info.power_domain = 0;

	/* The crtc is the PLL's only user (what intel_reference_shared_dpll() leaves). */
	ms->pll.info = &ms->pll_info;
	ms->pll.state.pipe_mask = (u8)BIT(ms->crtc.pipe);
	ms->crtc_state.shared_dpll = &ms->pll;
}

/* Divides with rounding to the closest (DIV_ROUND_CLOSEST for non-negative operands). */
static uint32_t
i915_divrc(
	uint32_t a,
	uint32_t b)
{
	/* Adds half the divisor before the truncating division. */
	return (a + b / 2u) / b;
}

/* Counts the set bits of a 16-bit word. */
static int
i915_hweight16(
	uint16_t w)
{
	int count;

	/* Shifts the word out bit by bit. */
	count = 0;
	while (w != 0u) {
		count += (int)(w & 1u);
		w >>= 1;
	}

	/* Succeeded: reports the count. */
	return count;
}

/* Finds the first voltage level whose maximum CDCLK covers a frequency (calc_voltage_level()). */
static uint8_t
i915_cd_calc_voltage_level(
	int cdclk,
	int n,
	const int *max_cdclk)
{
	int index;

	/* Walks the levels from the lowest. */
	for (index = 0; index < n; index++) {
		if (cdclk <= max_cdclk[index])
			return (uint8_t)index;
	}

	/* MISSING_CASE: clamps to the top level. */
	return (uint8_t)(n - 1);
}

/* The voltage level a CDCLK needs with the platform's hooks (tgl_cdclk_funcs here). */
static uint8_t
i915_cdclk_calc_voltage_level(
	struct i915_cdclk_dev *cd,
	int cdclk)
{
	uint8_t level;

	UNUSED_PARAMETER(cd);

	/* The TGL hooks serve both platforms. */
	level = drv_i915_tgl_calc_voltage_level(cdclk);

	/* Succeeded: reports the level. */
	return level;
}

/* The decimal CDCLK field of CDCLK_CTL (skl_cdclk_decimal()). */
static uint32_t
i915_skl_cdclk_decimal(
	int cdclk)
{
	/* (cdclk - 1 MHz) in 0.5 MHz units, rounded to the closest. */
	return i915_divrc((uint32_t)(cdclk - 1000), 500u);
}

/* The CD2X divider select of a CDCLK and VCO (bxt_cdclk_cd2x_div_sel()). */
static uint32_t
i915_bxt_cdclk_cd2x_div_sel(
	struct i915_cdclk_dev *cd,
	int cdclk,
	int vco)
{
	uint32_t ratio;

	UNUSED_PARAMETER(cd);

	/* Selects by the VCO-to-CDCLK ratio. */
	ratio = i915_divrc((uint32_t)vco, (uint32_t)cdclk);
	switch (ratio) {
	case 3:
		return BXT_CDCLK_CD2X_DIV_SEL_1_5;
	case 4:
		return BXT_CDCLK_CD2X_DIV_SEL_2;
	case 8:
		return BXT_CDCLK_CD2X_DIV_SEL_4;
	default:
		break;
	}

	/*
	 * A ratio of 2, and any other: the reference warns unless the CDCLK is
	 * the bypass with the VCO off, and falls through to divide by 1.
	 */
	return BXT_CDCLK_CD2X_DIV_SEL_1;
}

/*
 * The squash waveform of a CDCLK (cdclk_squash_waveform()): 0 for the
 * bypass, 0xffff for a frequency the table does not list.
 */
static uint16_t
i915_cdclk_squash_waveform(
	struct i915_cdclk_dev *cd,
	int cdclk)
{
	const struct i915_cdclk_vals *table;
	int index;

	/* The bypass frequency has no waveform. */
	table = cd->table;
	if ((uint32_t)cdclk == cd->hw.bypass)
		return 0;

	/* Looks the frequency up for the reference clock (the rows here carry 0). */
	for (index = 0; table[index].refclk != 0u; index++) {
		if (table[index].refclk == cd->hw.ref && (int)table[index].cdclk == cdclk)
			return table[index].waveform;
	}

	/* The frequency is not in the table. */
	return 0xffffu;
}

/* Tells whether a VCO is the "unknown, force a full reprogram" value (1 or 0). */
static int
i915_cdclk_pll_is_unknown(
	uint32_t vco)
{
	/* ~0 is never a real frequency. */
	if (vco == ~0u)
		return 1;

	/* Succeeded: the VCO is known. */
	return 0;
}

/* Reads the CDCLK PLL reference clock (icl_readout_refclk()). */
static void
i915_icl_readout_refclk(
	struct i915_cdclk_dev *cd,
	struct i915_cdclk_config *cfg)
{
	uint32_t dssm;

	/* Decodes the reference clock select. */
	dssm = drv_i915_raw_read32(cd->m, SKL_DSSM) & ICL_DSSM_CDCLK_PLL_REFCLK_MASK;
	switch (dssm) {
	case ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz:
		cfg->ref = 19200;
		break;
	case ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz:
		cfg->ref = 38400;
		break;
	default:
		/* 24 MHz, and MISSING_CASE falls through to it (the reference). */
		cfg->ref = 24000;
		break;
	}
}

/* Reads the reference clock and the DE PLL's VCO (bxt_de_pll_readout()). */
static void
i915_bxt_de_pll_readout(
	struct i915_cdclk_dev *cd,
	struct i915_cdclk_config *cfg)
{
	uint32_t value;
	uint32_t ratio;

	/* Display version 11+ (not DG2): the reference clock select. */
	i915_icl_readout_refclk(cd, cfg);

	/* A PLL that is disabled or not locked has no VCO: 0 says so. */
	value = drv_i915_raw_read32(cd->m, I915_BXT_DE_PLL_ENABLE);
	if ((value & BXT_DE_PLL_PLL_ENABLE) == 0u || (value & BXT_DE_PLL_LOCK) == 0u) {
		cfg->vco = 0;
		return;
	}

	/* Display version 11+: the ratio is in the PLL enable register. */
	ratio = value & ICL_CDCLK_PLL_RATIO_MASK;
	cfg->vco = ratio * cfg->ref;
}

/* Turns the CDCLK PLL off (icl_cdclk_pll_disable()). */
static void
i915_icl_cdclk_pll_disable(
	struct i915_cdclk_dev *cd)
{
	uint32_t value;
	int wait_result;

	/* Clears the PLL enable. */
	value = drv_i915_raw_read32(cd->m, I915_BXT_DE_PLL_ENABLE);
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value & ~BXT_DE_PLL_PLL_ENABLE);

	/* Waits for the lock to drop. */
	wait_result = drv_i915_wait_reg(cd->m, I915_BXT_DE_PLL_ENABLE, BXT_DE_PLL_LOCK, 0u, 10u, 1u, NULL);
	if (wait_result != 0)
		kern_logf("i915: cdclk: timeout waiting for CDCLK PLL unlock\n");

	/* The PLL is off. */
	cd->hw.vco = 0;
}

/* Turns the CDCLK PLL on at a VCO (icl_cdclk_pll_enable()). */
static void
i915_icl_cdclk_pll_enable(
	struct i915_cdclk_dev *cd,
	int vco)
{
	uint32_t ratio;
	uint32_t value;
	int wait_result;

	/* Writes the ratio, then the ratio with the enable. */
	ratio = i915_divrc((uint32_t)vco, cd->hw.ref);
	value = ratio & ICL_CDCLK_PLL_RATIO_MASK;
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value);
	value |= BXT_DE_PLL_PLL_ENABLE;
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value);

	/* Waits for the lock. */
	wait_result = drv_i915_wait_reg(cd->m, I915_BXT_DE_PLL_ENABLE, BXT_DE_PLL_LOCK, BXT_DE_PLL_LOCK,
		10u, 1u, NULL);
	if (wait_result != 0)
		kern_logf("i915: cdclk: timeout waiting for CDCLK PLL lock\n");

	/* The PLL runs at the VCO. */
	cd->hw.vco = (uint32_t)vco;
}

/* Moves the CDCLK PLL to a VCO by a disable and an enable (icl_cdclk_pll_update()). */
static void
i915_icl_cdclk_pll_update(
	struct i915_cdclk_dev *cd,
	int vco)
{
	/* Turns a PLL running at another VCO off. */
	if (cd->hw.vco != 0u && cd->hw.vco != (uint32_t)vco)
		i915_icl_cdclk_pll_disable(cd);

	/* Turns the PLL on at the VCO when it is not there already. */
	if (cd->hw.vco != (uint32_t)vco)
		i915_icl_cdclk_pll_enable(cd, vco);
}

/* Moves the CDCLK PLL to a VCO without disabling it (adlp_cdclk_pll_crawl()). */
static void
i915_adlp_cdclk_pll_crawl(
	struct i915_cdclk_dev *cd,
	int vco)
{
	uint32_t ratio;
	uint32_t value;
	int wait_result;

	/* Writes the new ratio with the PLL still enabled. */
	ratio = i915_divrc((uint32_t)vco, cd->hw.ref);
	value = (ratio & ICL_CDCLK_PLL_RATIO_MASK) | BXT_DE_PLL_PLL_ENABLE;
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value);

	/* Submits the frequency change request. */
	value |= BXT_DE_PLL_FREQ_REQ;
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value);

	/* Waits for the lock and the acknowledge. */
	wait_result = drv_i915_wait_reg(cd->m, I915_BXT_DE_PLL_ENABLE,
		BXT_DE_PLL_LOCK | BXT_DE_PLL_FREQ_REQ_ACK,
		BXT_DE_PLL_LOCK | BXT_DE_PLL_FREQ_REQ_ACK, 10u, 1u, NULL);
	if (wait_result != 0)
		kern_logf("i915: cdclk: timeout waiting for FREQ change ack\n");

	/* Withdraws the request. */
	value &= ~BXT_DE_PLL_FREQ_REQ;
	drv_i915_raw_write32(cd->m, I915_BXT_DE_PLL_ENABLE, value);

	/* The PLL runs at the VCO. */
	cd->hw.vco = (uint32_t)vco;
}

/* Programs the PLL and CDCLK_CTL of a CDCLK state (_bxt_set_cdclk(): crawl yes, squash no). */
static void
i915_bxt_program_cdclk(
	struct i915_cdclk_dev *cd,
	const struct i915_cdclk_config *cfg,
	int pipe)
{
	int cdclk;
	int vco;
	int unsquashed_cdclk;
	int unknown;
	uint16_t waveform;
	uint32_t value;

	/* Records that the hardware sequence ran. */
	cdclk = (int)cfg->cdclk;
	vco = (int)cfg->vco;
	cd->diag_hw_sequence_reached = 1;

	/* Crawls between two running VCOs; otherwise disables and enables (display version 11+). */
	unknown = i915_cdclk_pll_is_unknown(cd->hw.vco);
	if (cd->has_cdclk_crawl && cd->hw.vco > 0u && vco > 0 && !unknown) {
		if (cd->hw.vco != (uint32_t)vco)
			i915_adlp_cdclk_pll_crawl(cd, vco);
	} else {
		i915_icl_cdclk_pll_update(cd, vco);
	}

	/* The frequency before squashing (no squash here: the waveform is 0 and counts 16 bits). */
	waveform = i915_cdclk_squash_waveform(cd, cdclk);
	if (waveform == 0u)
		waveform = 0xffffu;
	unsquashed_cdclk = (int)i915_divrc((uint32_t)cdclk * 16u, (uint32_t)i915_hweight16(waveform));

	/* The divider select; no dg2_cdclk_squash_program() without squash. */
	value = i915_bxt_cdclk_cd2x_div_sel(cd, unsquashed_cdclk, vco);

	/* Outside a modeset the change syncs to no pipe. */
	if (pipe == I915_CDCLK_INVALID_PIPE)
		value |= TGL_CDCLK_CD2X_PIPE_NONE;
	else
		value |= (uint32_t)pipe << 20;

	/* SSA precharge is GLK / BXT only; before display version 20 the decimal frequency. */
	value |= i915_skl_cdclk_decimal(cdclk) & CDCLK_FREQ_DECIMAL_MASK;

	/* Writes CDCLK_CTL; without a pipe there is no vblank to wait for. */
	drv_i915_raw_write32(cd->m, I915_CDCLK_CTL, value);
}

/*
 * Tells whether the CDCLK state read out is a legal one for the platform
 * (the checks of bxt_sanitize_cdclk()): 1 to keep it, 0 to reprogram.
 */
static int
i915_bxt_cdclk_is_legal(
	struct i915_cdclk_dev *cd)
{
	uint32_t cdctl;
	uint32_t expected;
	int cdclk;
	int clock;
	int vco;

	/* A PLL that is off, or a bypass frequency, is reprogrammed. */
	if (cd->hw.vco == 0u || cd->hw.cdclk == cd->hw.bypass)
		return 0;

	/*
	 * The DPLL is fine; CDCLK_CTL is checked (the firmware may leave a bad
	 * decimal or MBZ bits).  The pipe field is ignored: the firmware may
	 * have synced to a pipe or to none.
	 */
	cdctl = drv_i915_raw_read32(cd->m, I915_CDCLK_CTL);
	cdctl &= ~TGL_CDCLK_CD2X_PIPE_NONE;

	/* The frequency must be a table frequency. */
	cdclk = drv_i915_bxt_calc_cdclk(cd, (int)cd->hw.cdclk);
	if (cdclk != (int)cd->hw.cdclk)
		return 0;

	/* The VCO must be the frequency's. */
	vco = drv_i915_bxt_calc_cdclk_pll_vco(cd, cdclk);
	if (vco != (int)cd->hw.vco)
		return 0;

	/* CDCLK_CTL must hold the decimal and divider of the frequency (no squash, no SSA precharge). */
	expected = i915_skl_cdclk_decimal(cdclk);
	clock = (int)cd->hw.cdclk;
	expected |= i915_bxt_cdclk_cd2x_div_sel(cd, clock, (int)cd->hw.vco);
	if (cdctl != expected)
		return 0;

	/* Succeeded: nothing to sanitize. */
	return 1;
}

/* Sanitizes the CDCLK and reprograms it only when needed (bxt_cdclk_init_hw()). */
static void
i915_bxt_cdclk_init_hw(
	struct i915_cdclk_dev *cd)
{
	struct i915_cdclk_config cfg;

	/* Accepts or rejects what the firmware left. */
	drv_i915_bxt_sanitize_cdclk(cd);

	/* A state the sanitize accepted is kept as it is. */
	if (cd->hw.cdclk != 0u && cd->hw.vco != 0u) {
		cd->diag_no_change = 1;
		return;
	}

	/* FIXME of the reference: the initial CDCLK should come from the VBT; the minimum is used. */
	cfg = cd->hw;
	cfg.cdclk = (uint32_t)drv_i915_bxt_calc_cdclk(cd, 0);
	cfg.vco = (uint32_t)drv_i915_bxt_calc_cdclk_pll_vco(cd, (int)cfg.cdclk);
	cfg.voltage_level = i915_cdclk_calc_voltage_level(cd, (int)cfg.cdclk);

	/* Programs it. */
	drv_i915_bxt_set_cdclk(cd, &cfg);
}

/* The lowest table CDCLK of the device view at or above a minimum (bxt_calc_cdclk()), or 0. */
static int
i915_bxt_calc_cdclk(
	struct drm_i915_private *dev_priv,
	int min_cdclk)
{
	const struct intel_cdclk_vals *table;
	int index;

	/* Looks for the first row of the reference clock that is fast enough. */
	table = dev_priv->display.cdclk.table;
	for (index = 0; table[index].refclk != 0u; index++) {
		if (table[index].refclk == dev_priv->display.cdclk.hw.ref &&
		    table[index].cdclk >= (u32)min_cdclk)
			return (int)table[index].cdclk;
	}

	/* drm_WARN(1): no row satisfies the minimum. */
	drv_i915_lcd_error("Cannot satisfy minimum cdclk %d with refclk %u\n");
	return 0;
}

/* The PLL VCO of a table CDCLK of the device view (bxt_calc_cdclk_pll_vco()), or 0. */
static int
i915_bxt_calc_cdclk_pll_vco(
	struct drm_i915_private *dev_priv,
	int cdclk)
{
	const struct intel_cdclk_vals *table;
	int index;

	/* The bypass frequency runs with the PLL off. */
	table = dev_priv->display.cdclk.table;
	if ((unsigned int)cdclk == dev_priv->display.cdclk.hw.bypass)
		return 0;

	/* Looks the frequency up for the reference clock. */
	for (index = 0; table[index].refclk != 0u; index++) {
		if (table[index].refclk == dev_priv->display.cdclk.hw.ref &&
		    table[index].cdclk == (u32)cdclk)
			return (int)(dev_priv->display.cdclk.hw.ref * table[index].ratio);
	}

	/* drm_WARN(1): the frequency is not valid for the reference clock. */
	drv_i915_lcd_error("cdclk %d not valid for refclk %u\n");
	return 0;
}

/* The first voltage level whose maximum CDCLK covers a frequency (calc_voltage_level()). */
static u8
i915_calc_voltage_level(
	int cdclk,
	int num_voltage_levels,
	const int voltage_level_max_cdclk[])
{
	int voltage_level;

	/* Walks the levels from the lowest. */
	for (voltage_level = 0; voltage_level < num_voltage_levels; voltage_level++) {
		if (cdclk <= voltage_level_max_cdclk[voltage_level])
			return (u8)voltage_level;
	}

	/* No level covers it: clamps to the top one. */
	I915_LCD_MISSING_CASE(cdclk);
	return (u8)(num_voltage_levels - 1);
}

/* The voltage level a CDCLK needs on these platforms (tgl_calc_voltage_level()). */
static u8
i915_tgl_calc_voltage_level(
	int cdclk)
{
	static const int tgl_voltage_level_max_cdclk[] = {
		[0] = 312000,
		[1] = 326400,
		[2] = 556800,
		[3] = 652800,
	};
	u8 level;

	/* Finds the level. */
	level = i915_calc_voltage_level(cdclk,
					(int)ARRAY_SIZE(tgl_voltage_level_max_cdclk),
					tgl_voltage_level_max_cdclk);

	/* Succeeded: reports the level. */
	return level;
}

/* The CDCLK a pixel rate needs (intel_pixel_rate_to_cdclk()). */
static int
i915_pixel_rate_to_cdclk(
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	int pixel_rate;
	int display_ver;

	/* Resolves the device and the pixel rate. */
	dev_priv = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	pixel_rate = crtc_state->pixel_rate;
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/*
	 * The platform predicates of this environment answer for the device
	 * the probe found and do not read their argument.
	 */
	UNUSED_PARAMETER(dev_priv);

	/* Display version 10+ moves two pixels per CDCLK. */
	if (display_ver >= 10)
		return DIV_ROUND_UP(pixel_rate, 2);

	/* Version 9, BDW and HSW move one. */
	if (display_ver == 9 || IS_BROADWELL(dev_priv) || IS_HASWELL(dev_priv))
		return pixel_rate;

	/* CHV needs 95%, a double-wide pipe half of 90%, the rest 90%. */
	if (IS_CHERRYVIEW(dev_priv))
		return DIV_ROUND_UP(pixel_rate * 100, 95);
	if (crtc_state->double_wide)
		return DIV_ROUND_UP(pixel_rate * 100, 90 * 2);

	/* Succeeded: the 90% rule. */
	return DIV_ROUND_UP(pixel_rate * 100, 90);
}

/*
 * The highest CDCLK need of the crtc's planes (intel_planes_min_cdclk()).
 *
 * The Linux for_each_intel_plane_on_crtc() of this text walked the
 * takeover registry: the one primary plane of the crtc's pipe while the
 * registry is live, none otherwise.
 */
static int
i915_planes_min_cdclk(
	struct i915_takeover_world *takeover,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct intel_plane *plane;
	int min_cdclk;

	/* Resolves the crtc. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	min_cdclk = 0;

	/* Without a registry there is no plane to walk. */
	if (takeover == NULL)
		return min_cdclk;

	/* Takes the need of the registry's plane of the pipe, if any. */
	plane = drv_i915_n1_plane_at(takeover, (unsigned)crtc->pipe);
	if (plane != NULL)
		min_cdclk = max(crtc_state->min_cdclk[plane->id], min_cdclk);

	/* Succeeded: reports the planes' need. */
	return min_cdclk;
}

/*
 * Tells whether half the nominal DCO fraction is programmed (Display WA
 * #22010492432: ehl, tgl, adl-s, adl-p, on a 38.4 MHz reference).
 */
static bool
i915_ehl_combo_pll_div_frac_wa_needed(
	struct drm_i915_private *i915)
{
	bool platform;

	/* The platforms of the workaround. */
	platform = false;
	if (IS_ELKHARTLAKE(i915) && IS_DISPLAY_STEP(i915, STEP_B0, STEP_FOREVER))
		platform = true;
	else if (I915_LCD_IS_TIGERLAKE(i915))
		platform = true;
	else if (IS_ALDERLAKE_S(i915))
		platform = true;
	else if (I915_LCD_IS_ALDERLAKE_P(i915))
		platform = true;

	/* Only with a 38.4 MHz reference clock. */
	if (!platform)
		return false;
	if (i915->display.dpll.ref_clks.nssc != 38400)
		return false;

	/* Succeeded: the workaround applies. */
	return true;
}

/* Looks a DP link rate's combo PLL settings up (icl_calc_dp_combo_pll()): 0 or I915_LCD_EINVAL. */
static int
i915_icl_calc_dp_combo_pll(
	struct intel_crtc_state *crtc_state,
	struct skl_wrpll_params *pll_params)
{
	struct drm_i915_private *i915;
	const struct icl_combo_pll_params *params;
	int clock;
	int index;

	/* Picks the table of the reference clock (38.4 MHz uses the 19.2 MHz one). */
	i915 = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	params = icl_dp_combo_pll_19_2MHz_values;
	if (i915->display.dpll.ref_clks.nssc == 24000)
		params = icl_dp_combo_pll_24MHz_values;

	/* Looks the port clock up (both tables have the 24 MHz table's length). */
	clock = crtc_state->port_clock;
	for (index = 0; index < (int)ARRAY_SIZE(icl_dp_combo_pll_24MHz_values); index++) {
		if (clock == params[index].clock) {
			*pll_params = params[index].wrpll;
			return 0;
		}
	}

	/* The link rate is not in the table. */
	I915_LCD_MISSING_CASE(clock);
	return I915_LCD_EINVAL;
}

/* Packs divider settings into the PLL words (icl_calc_dpll_state()). */
static void
i915_icl_calc_dpll_state(
	struct drm_i915_private *i915,
	const struct skl_wrpll_params *pll_params,
	struct intel_dpll_hw_state *pll_state)
{
	u32 dco_fraction;
	bool halve;

	/* Display WA #22010492432: half the nominal DCO fraction. */
	dco_fraction = pll_params->dco_fraction;
	halve = i915_ehl_combo_pll_div_frac_wa_needed(i915);
	if (halve)
		dco_fraction = DIV_ROUND_CLOSEST(dco_fraction, 2);

	/* CFGCR0: the DCO fraction and integer. */
	pll_state->cfgcr0 = DPLL_CFGCR0_DCO_FRACTION(dco_fraction) |
			    pll_params->dco_integer;

	/* CFGCR1: the dividers. */
	pll_state->cfgcr1 = DPLL_CFGCR1_QDIV_RATIO(pll_params->qdiv_ratio) |
			    DPLL_CFGCR1_QDIV_MODE(pll_params->qdiv_mode) |
			    DPLL_CFGCR1_KDIV(pll_params->kdiv) |
			    DPLL_CFGCR1_PDIV(pll_params->pdiv);

	/* Display version 12+ selects the normal crystal; older ones the 8400 central frequency. */
	if (I915_LCD_DISPLAY_VER(i915) >= 12)
		pll_state->cfgcr1 |= TGL_DPLL_CFGCR1_CFSELOVRD_NORMAL_XTAL;
	else
		pll_state->cfgcr1 |= DPLL_CFGCR1_CENTRAL_FREQ_8400;

	/* The VBT's AFC startup override, when it has one. */
	if (i915->display.vbt.override_afc_startup)
		pll_state->div0 = TGL_DPLL0_DIV0_AFC_STARTUP(i915->display.vbt.override_afc_startup_val);
}

/* The enable register of a combo PLL (intel_combo_pll_enable_reg()). */
static i915_reg_t
i915_combo_pll_enable_reg(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	UNUSED_PARAMETER(i915);

	/* DG1 has its own enable registers. */
	if (IS_DG1(i915))
		return DG1_DPLL_ENABLE(pll->info->id);

	/* JSL / EHL's DPLL4 is enabled through MG_PLL_ENABLE(0). */
	if ((IS_JASPERLAKE(i915) || IS_ELKHARTLAKE(i915)) &&
	    (pll->info->id == DPLL_ID_EHL_DPLL4))
		return MG_PLL_ENABLE(0);

	/* Succeeded: every other combo PLL. */
	return ICL_DPLL_ENABLE(pll->info->id);
}

/* Powers a combo PLL on (icl_pll_power_enable()). */
static void
i915_icl_pll_power_enable(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	i915_reg_t enable_reg)
{
	int wait_result;

	/* Requests the PLL power. */
	(void)i915_lcd_intel_de_rmw(i915, enable_reg, 0, PLL_POWER_ENABLE);

	/* The spec says to "wait" but also that it should be immediate. */
	wait_result = i915_lcd_wait(i915, enable_reg, PLL_POWER_STATE, PLL_POWER_STATE, 1);
	if (wait_result != 0) {
		I915_LCD_DRM_ERR(&i915->drm, "PLL %d Power not enabled\n",
			pll->info->id);
	}
}

/* Writes a combo PLL's configuration (icl_dpll_write()). */
static void
i915_icl_dpll_write(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	struct intel_dpll_hw_state *hw_state;
	enum intel_dpll_id id;
	i915_reg_t cfgcr0_reg;
	i915_reg_t cfgcr1_reg;
	i915_reg_t div0_reg;
	bool div0_valid;

	/* Resolves the state and the PLL's id. */
	hw_state = &pll->state.hw_state;
	id = pll->info->id;
	div0_reg = INVALID_MMIO_REG;

	/* Picks the configuration registers of the platform. */
	if (IS_ALDERLAKE_S(i915)) {
		cfgcr0_reg = ADLS_DPLL_CFGCR0(id);
		cfgcr1_reg = ADLS_DPLL_CFGCR1(id);
	} else if (IS_DG1(i915)) {
		cfgcr0_reg = DG1_DPLL_CFGCR0(id);
		cfgcr1_reg = DG1_DPLL_CFGCR1(id);
	} else if (IS_ROCKETLAKE(i915)) {
		cfgcr0_reg = RKL_DPLL_CFGCR0(id);
		cfgcr1_reg = RKL_DPLL_CFGCR1(id);
	} else if (I915_LCD_DISPLAY_VER(i915) >= 12) {
		cfgcr0_reg = TGL_DPLL_CFGCR0(id);
		cfgcr1_reg = TGL_DPLL_CFGCR1(id);
		div0_reg = TGL_DPLL0_DIV0(id);
	} else {
		if ((IS_JASPERLAKE(i915) || IS_ELKHARTLAKE(i915)) &&
		    id == DPLL_ID_EHL_DPLL4) {
			cfgcr0_reg = ICL_DPLL_CFGCR0(4);
			cfgcr1_reg = ICL_DPLL_CFGCR1(4);
		} else {
			cfgcr0_reg = ICL_DPLL_CFGCR0(id);
			cfgcr1_reg = ICL_DPLL_CFGCR1(id);
		}
	}

	/* Writes the dividers. */
	i915_lcd_intel_de_write(i915, cfgcr0_reg, hw_state->cfgcr0);
	i915_lcd_intel_de_write(i915, cfgcr1_reg, hw_state->cfgcr1);

	/* An AFC override without a DIV0 register is a warning of the Linux text. */
	div0_valid = i915_mmio_reg_valid(div0_reg);
	if (i915->display.vbt.override_afc_startup && !div0_valid)
		drv_i915_lcd_error("WARN_ON(i915->display.vbt.override_afc_startup && !((div0_reg).reg != 0u))\n");

	/* Writes the AFC startup override. */
	if (i915->display.vbt.override_afc_startup && div0_valid) {
		(void)i915_lcd_intel_de_rmw(i915, div0_reg,
			TGL_DPLL0_DIV0_AFC_STARTUP_MASK, hw_state->div0);
	}

	/* Posts the writes. */
	i915_lcd_intel_de_posting_read(i915, cfgcr1_reg);
}

/* Enables a combo PLL and waits for its lock (icl_pll_enable()). */
static void
i915_icl_pll_enable(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	i915_reg_t enable_reg)
{
	int wait_result;

	/* Sets the enable. */
	(void)i915_lcd_intel_de_rmw(i915, enable_reg, 0, PLL_ENABLE);

	/* The timeout is actually 600 us. */
	wait_result = i915_lcd_wait(i915, enable_reg, PLL_LOCK, PLL_LOCK, 1);
	if (wait_result != 0)
		I915_LCD_DRM_ERR(&i915->drm, "PLL %d not locked\n", pll->info->id);
}

/*
 * Disables CMTG clock gating on Alder Lake-P A0 when DPLL0 is enabled
 * (adlp_cmtg_clock_gating_wa(): Wa_16011069516:adl-p[a0]).
 *
 * All CMTG registers are unreliable until CMTG clock gating is disabled,
 * so the default TRANS_CMTG_CHICKEN value is assumed and checked with a
 * double read.  It is applied here because TRANS_CMTG_CHICKEN is only
 * accessible while DPLL0 is enabled.
 */
static void
i915_adlp_cmtg_clock_gating_wa(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	u32 val;
	bool a_step;

	/* Only Alder Lake-P A-step with DPLL0. */
	a_step = false;
	if (I915_LCD_IS_ALDERLAKE_P(i915) && IS_DISPLAY_STEP(i915, STEP_A0, STEP_B0))
		a_step = true;
	if (!a_step || pll->info->id != DPLL_ID_ICL_DPLL0)
		return;

	/*
	 * Reads the register, then replaces it with DISABLE_DPT_CLK_GATING and
	 * checks the old value for unexpected flags.
	 */
	val = i915_lcd_intel_de_read(i915, TRANS_CMTG_CHICKEN);
	val = i915_lcd_intel_de_rmw(i915, TRANS_CMTG_CHICKEN, ~0, DISABLE_DPT_CLK_GATING);
	if ((val & ~DISABLE_DPT_CLK_GATING) != 0u) {
		drv_i915_lcd_error("WARN_ON(val & ~DISABLE_DPT_CLK_GATING)\n");
		I915_LCD_DRM_DBG_KMS(&i915->drm, "Unexpected flags in TRANS_CMTG_CHICKEN: %08x\n", val);
	}
}

/* Turns a combo PLL on (combo_pll_enable()). */
static void
i915_combo_pll_enable(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg;

	/* Powers the PLL. */
	enable_reg = i915_combo_pll_enable_reg(i915, pll);
	i915_icl_pll_power_enable(i915, pll, enable_reg);

	/* Writes its configuration. */
	i915_icl_dpll_write(i915, pll);

	/*
	 * The DVFS pre sequence would be here; the CDCLK paths already set the
	 * voltage.
	 */

	/* Enables it and waits for the lock. */
	i915_icl_pll_enable(i915, pll, enable_reg);

	/* Wa_16011069516:adl-p[a0]. */
	i915_adlp_cmtg_clock_gating_wa(i915, pll);

	/* The DVFS post sequence would be here (see above). */
}

/* Turns a combo PLL off (icl_pll_disable(); the first steps are done by intel_ddi_post_disable()). */
static void
i915_icl_pll_disable(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	i915_reg_t enable_reg)
{
	int wait_result;

	/* The DVFS pre sequence would be here; the CDCLK paths already set the voltage. */

	/* Clears the enable. */
	(void)i915_lcd_intel_de_rmw(i915, enable_reg, PLL_ENABLE, 0);

	/* The timeout is actually 1 us. */
	wait_result = i915_lcd_wait(i915, enable_reg, PLL_LOCK, 0u, 1);
	if (wait_result != 0)
		I915_LCD_DRM_ERR(&i915->drm, "PLL %d locked\n", pll->info->id);

	/* The DVFS post sequence would be here (see above). */

	/* Removes the PLL power. */
	(void)i915_lcd_intel_de_rmw(i915, enable_reg, PLL_POWER_ENABLE, 0);

	/* The spec says to "wait" but also that it should be immediate. */
	wait_result = i915_lcd_wait(i915, enable_reg, PLL_POWER_STATE, 0u, 1);
	if (wait_result != 0) {
		I915_LCD_DRM_ERR(&i915->drm, "PLL %d Power not disabled\n",
			pll->info->id);
	}
}

/* Turns a combo PLL off (combo_pll_disable()). */
static void
i915_combo_pll_disable(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	i915_reg_t enable_reg;

	/* Disables the PLL through its enable register. */
	enable_reg = i915_combo_pll_enable_reg(i915, pll);
	i915_icl_pll_disable(i915, pll, enable_reg);
}

/* Turns a shared DPLL on with its power domain (_intel_enable_shared_dpll()). */
static void
i915_intel_enable_shared_dpll(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	/* Takes the PLL's power domain, when it has one. */
	if (pll->info->power_domain)
		pll->wakeref = i915_lcd_intel_display_power_get(i915, pll->info->power_domain);

	/* Turns the PLL on. */
	pll->info->funcs->enable(i915, pll);
	pll->on = true;
}

/* The body of intel_enable_shared_dpll() under the DPLL lock. */
static void
i915_enable_shared_dpll_locked(
	struct drm_i915_private *i915,
	struct intel_crtc *crtc,
	struct intel_shared_dpll *pll,
	unsigned int pipe_mask)
{
	unsigned int old_mask;

	/* The pipes that drive the PLL before this one joins. */
	old_mask = pll->active_mask;

	/* A crtc not reserved on the PLL is a warning of the Linux text. */
	if (!(pll->state.pipe_mask & pipe_mask)) {
		drv_i915_lcd_error("WARN_ON(!(pll->state.pipe_mask & pipe_mask))\n");
		return;
	}

	/* A crtc already active on the PLL is a warning of the Linux text. */
	if (pll->active_mask & pipe_mask) {
		drv_i915_lcd_error("WARN_ON(pll->active_mask & pipe_mask)\n");
		return;
	}

	/* The active mask names the pipes that drive the PLL now. */
	pll->active_mask |= pipe_mask;

	/* Notes the enable. */
	I915_LCD_DRM_DBG_KMS(&i915->drm,
		"enable %s (active 0x%x, on? %d) for [CRTC:%d:%s]\n",
		pll->info->name, pll->active_mask, pll->on,
		crtc->base.base.id, crtc->base.name);

	/* Another pipe already runs the PLL: it must be on. */
	if (old_mask) {
		if (!pll->on)
			drv_i915_lcd_error("WARN_ON(!pll->on)\n");
		assert_shared_dpll_enabled(i915, pll);
		return;
	}

	/* The first pipe finds the PLL off. */
	if (pll->on)
		drv_i915_lcd_error("WARN_ON(pll->on)\n");

	/* Turns it on. */
	I915_LCD_DRM_DBG_KMS(&i915->drm, "enabling %s\n", pll->info->name);
	i915_intel_enable_shared_dpll(i915, pll);
}

/* Turns a shared DPLL off and gives its power domain back (_intel_disable_shared_dpll()). */
static void
i915_intel_disable_shared_dpll(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	/* Turns the PLL off. */
	pll->info->funcs->disable(i915, pll);
	pll->on = false;

	/* Gives the PLL's power domain back, when it has one. */
	if (pll->info->power_domain)
		i915_lcd_intel_display_power_put(i915, pll->info->power_domain, pll->wakeref);
}

/* The body of intel_disable_shared_dpll() under the DPLL lock. */
static void
i915_disable_shared_dpll_locked(
	struct drm_i915_private *i915,
	struct intel_crtc *crtc,
	struct intel_shared_dpll *pll,
	unsigned int pipe_mask)
{
	/* drm_WARN: the crtc does not drive the PLL. */
	if (!(pll->active_mask & pipe_mask)) {
		drv_i915_lcd_error("%s not used by [CRTC:%d:%s]\n");
		return;
	}

	/* Notes the disable. */
	I915_LCD_DRM_DBG_KMS(&i915->drm,
		"disable %s (active 0x%x, on? %d) for [CRTC:%d:%s]\n",
		pll->info->name, pll->active_mask, pll->on,
		crtc->base.base.id, crtc->base.name);

	/* The PLL of an active pipe must be on. */
	assert_shared_dpll_enabled(i915, pll);
	if (!pll->on)
		drv_i915_lcd_error("WARN_ON(!pll->on)\n");

	/* The active mask names the pipes that drive the PLL now; the last one turns it off. */
	pll->active_mask &= ~pipe_mask;
	if (pll->active_mask)
		return;

	/* Turns it off. */
	I915_LCD_DRM_DBG_KMS(&i915->drm, "disabling %s\n", pll->info->name);
	i915_intel_disable_shared_dpll(i915, pll);
}

/* The WRPLL reference clock (icl_wrpll_ref_clock(): 38.4 MHz is divided by 2 by the DPLL). */
static int
i915_icl_wrpll_ref_clock(
	struct drm_i915_private *i915)
{
	int ref_clock;

	/* For ICL+, a 38.4 MHz reference is used as 19.2 MHz. */
	ref_clock = i915->display.dpll.ref_clks.nssc;
	if (ref_clock == 38400)
		ref_clock = 19200;

	/* Succeeded: reports the reference clock. */
	return ref_clock;
}

/* Splits the best divider into P, Q and K (icl_wrpll_get_multipliers()). */
static void
i915_icl_wrpll_get_multipliers(
	int bestdiv,
	int *pdiv,
	int *qdiv,
	int *kdiv)
{
	/* Even dividers. */
	if (bestdiv % 2 == 0) {
		if (bestdiv == 2) {
			*pdiv = 2;
			*qdiv = 1;
			*kdiv = 1;
		} else if (bestdiv % 4 == 0) {
			*pdiv = 2;
			*qdiv = bestdiv / 4;
			*kdiv = 2;
		} else if (bestdiv % 6 == 0) {
			*pdiv = 3;
			*qdiv = bestdiv / 6;
			*kdiv = 2;
		} else if (bestdiv % 5 == 0) {
			*pdiv = 5;
			*qdiv = bestdiv / 10;
			*kdiv = 2;
		} else if (bestdiv % 14 == 0) {
			*pdiv = 7;
			*qdiv = bestdiv / 14;
			*kdiv = 2;
		}

		/* The even divider is split. */
		return;
	}

	/* Odd dividers: 3, 5 and 7 directly; 9, 15 and 21 as P x 3. */
	if (bestdiv == 3 || bestdiv == 5 || bestdiv == 7) {
		*pdiv = bestdiv;
		*qdiv = 1;
		*kdiv = 1;
	} else {
		*pdiv = bestdiv / 3;
		*qdiv = 1;
		*kdiv = 3;
	}
}

/* Encodes the dividers and the DCO of a WRPLL (icl_wrpll_params_populate()). */
static void
i915_icl_wrpll_params_populate(
	struct skl_wrpll_params *params,
	u32 dco_freq,
	u32 ref_freq,
	int pdiv,
	int qdiv,
	int kdiv)
{
	u32 dco;

	/* Encodes K. */
	switch (kdiv) {
	case 1:
		params->kdiv = 1;
		break;
	case 2:
		params->kdiv = 2;
		break;
	case 3:
		params->kdiv = 4;
		break;
	default:
		drv_i915_lcd_error("Incorrect KDiv\n");
		break;
	}

	/* Encodes P. */
	switch (pdiv) {
	case 2:
		params->pdiv = 1;
		break;
	case 3:
		params->pdiv = 2;
		break;
	case 5:
		params->pdiv = 4;
		break;
	case 7:
		params->pdiv = 8;
		break;
	default:
		drv_i915_lcd_error("Incorrect PDiv\n");
		break;
	}

	/* Q other than 1 needs K = 2. */
	if (kdiv != 2 && qdiv != 1)
		drv_i915_lcd_error("WARN_ON(kdiv != 2 && qdiv != 1)\n");

	/* Encodes Q. */
	params->qdiv_ratio = (u32)qdiv;
	params->qdiv_mode = 1;
	if (qdiv == 1)
		params->qdiv_mode = 0;

	/* The DCO in 15-bit fixed point of the reference. */
	dco = (u32)i915_div_u64((u64)dco_freq << 15, ref_freq);
	params->dco_integer = dco >> 15;
	params->dco_fraction = dco & 0x7fff;
}

/*
 * Chooses the WRPLL dividers of an HDMI clock (icl_calc_wrpll()): 0, or
 * I915_LCD_EINVAL when no divider puts the DCO in range.
 */
static int
i915_icl_calc_wrpll(
	struct intel_crtc_state *crtc_state,
	struct skl_wrpll_params *wrpll_params)
{
	static const int dividers[] = {  2,  4,  6,  8, 10, 12,  14,  16,
					 18, 20, 24, 28, 30, 32,  36,  40,
					 42, 44, 48, 50, 52, 54,  56,  60,
					 64, 66, 68, 70, 72, 76,  78,  80,
					 84, 88, 90, 92, 96, 98, 100, 102,
					  3,  5,  7,  9, 15, 21 };
	struct drm_i915_private *i915;
	int ref_clock;
	u32 afe_clock;
	u32 dco_min;
	u32 dco_max;
	u32 dco_mid;
	u32 dco;
	u32 best_dco;
	u32 dco_centrality;
	u32 best_dco_centrality;
	int d;
	int best_div;
	int pdiv;
	int qdiv;
	int kdiv;

	/* Resolves the reference and the AFE clock, and the DCO range. */
	i915 = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	ref_clock = i915_icl_wrpll_ref_clock(i915);
	afe_clock = (u32)crtc_state->port_clock * 5;
	dco_min = 7998000;
	dco_max = 10000000;
	dco_mid = (dco_min + dco_max) / 2;

	/* Starts with no divider (the centrality U32_MAX is the spec's 999999 MHz). */
	best_dco = 0;
	dco_centrality = 0;
	best_dco_centrality = U32_MAX;
	best_div = 0;
	pdiv = 0;
	qdiv = 0;
	kdiv = 0;

	/* Picks the divider whose DCO is in range and closest to the middle. */
	for (d = 0; d < (int)ARRAY_SIZE(dividers); d++) {
		dco = afe_clock * (u32)dividers[d];

		if (dco <= dco_max && dco >= dco_min) {
			/*
			 * XXX: abs() of the unsigned difference: the Linux text's
			 * abs() leaves an unsigned value as it is, so a DCO below the
			 * middle gives the wrapped difference, not the distance.  Kept
			 * as the old code computed it.
			 */
			dco_centrality = dco - dco_mid;

			if (dco_centrality < best_dco_centrality) {
				best_dco_centrality = dco_centrality;
				best_div = dividers[d];
				best_dco = dco;
			}
		}
	}

	/* No divider fits. */
	if (best_div == 0)
		return I915_LCD_EINVAL;

	/* Splits and encodes the divider. */
	i915_icl_wrpll_get_multipliers(best_div, &pdiv, &qdiv, &kdiv);
	i915_icl_wrpll_params_populate(wrpll_params, best_dco, (u32)ref_clock,
				       pdiv, qdiv, kdiv);

	/* Succeeded: the dividers are the caller's. */
	return 0;
}

/* The mask of every DPLL id in the pool (intel_dpll_mask_all()). */
static unsigned long
i915_dpll_mask_all(
	struct drm_i915_private *i915)
{
	struct intel_shared_dpll *pll;
	unsigned long dpll_mask;
	int index;

	/* Collects every PLL's id (for_each_shared_dpll). */
	dpll_mask = 0;
	for (index = 0; index < i915->display.dpll.num_shared_dpll; index++) {
		pll = &i915->display.dpll.shared_dplls[index];

		/* Two PLLs with one id are a warning of the Linux text. */
		if ((dpll_mask & BIT(pll->info->id)) != 0u)
			drv_i915_lcd_error("WARN_ON(dpll_mask & BIT(pll->info->id))\n");

		dpll_mask |= BIT(pll->info->id);
	}

	/* Succeeded: reports the ids. */
	return dpll_mask;
}

/*
 * Finds the PLL of a mask that already carries a state, or else a free one
 * (intel_find_shared_dpll()); NULL when every PLL is taken.
 */
static struct intel_shared_dpll *
i915_find_shared_dpll(
	struct i915_lcd_world *world,
	struct intel_atomic_state *state,
	const struct intel_crtc *crtc,
	const struct intel_dpll_hw_state *pll_state,
	unsigned long dpll_mask)
{
	struct drm_i915_private *i915;
	unsigned long dpll_mask_all;
	struct intel_shared_dpll_state *shared_dpll;
	struct intel_shared_dpll *unused_pll;
	struct intel_shared_dpll *pll;
	int id;
	int id_limit;
	int differs;

	UNUSED_PARAMETER(state);

	/* Resolves the device, its PLL ids and the atomic state's PLL states (the world's pool). */
	i915 = i915_lcd_to_i915(crtc->base.dev);
	dpll_mask_all = i915_dpll_mask_all(i915);
	shared_dpll = drv_i915_lcd_shared_dpll_state(world);
	unused_pll = NULL;

	/* Asking for a PLL the device does not have is a warning of the Linux text. */
	if ((dpll_mask & ~dpll_mask_all) != 0u)
		drv_i915_lcd_error("WARN_ON(dpll_mask & ~dpll_mask_all)\n");

	/* Walks the ids of the mask (for_each_set_bit up to fls(dpll_mask_all)). */
	id_limit = fls(dpll_mask_all);
	for (id = 0; id < id_limit; id++) {
		if ((dpll_mask & (1ul << id)) == 0u)
			continue;

		/* Resolves the PLL of the id. */
		pll = drv_i915_get_shared_dpll_by_id(i915, (enum intel_dpll_id)id);
		if (pll == NULL)
			continue;

		/* Only enabled timings are checked first; the first unused PLL is remembered. */
		if (shared_dpll[pll->index].pipe_mask == 0) {
			if (unused_pll == NULL)
				unused_pll = pll;
			continue;
		}

		/* A PLL with the same state is shared. */
		differs = kern_memcmp(pll_state,
				 &shared_dpll[pll->index].hw_state,
				 sizeof(*pll_state));
		if (differs == 0) {
			I915_LCD_DRM_DBG_KMS(&i915->drm,
				"[CRTC:%d:%s] sharing existing %s (pipe mask 0x%x, active 0x%x)\n",
				crtc->base.base.id, crtc->base.name,
				pll->info->name,
				shared_dpll[pll->index].pipe_mask,
				pll->active_mask);
			return pll;
		}
	}

	/* No matching timings: a free PLL, if there is one. */
	if (unused_pll != NULL) {
		I915_LCD_DRM_DBG_KMS(&i915->drm, "[CRTC:%d:%s] allocated %s\n",
			crtc->base.base.id, crtc->base.name,
			unused_pll->info->name);
		return unused_pll;
	}

	/* Every PLL is taken. */
	return NULL;
}

/* Takes a crtc's reference on a DPLL (intel_reference_shared_dpll_crtc()). */
static void
i915_reference_shared_dpll_crtc(
	const struct intel_crtc *crtc,
	const struct intel_shared_dpll *pll,
	struct intel_shared_dpll_state *shared_dpll_state)
{
	/* A crtc that already holds a reference is a warning of the Linux text. */
	if ((shared_dpll_state->pipe_mask & BIT(crtc->pipe)) != 0)
		drv_i915_lcd_error("WARN_ON((shared_dpll_state->pipe_mask & BIT(crtc->pipe)) != 0)\n");

	/* The pipe mask names the crtcs that use the PLL. */
	shared_dpll_state->pipe_mask |= BIT(crtc->pipe);

	/* Notes the reservation (on the crtc's DRM device). */
	I915_LCD_DRM_DBG_KMS(crtc->base.dev, "[CRTC:%d:%s] reserving %s\n",
		crtc->base.base.id, crtc->base.name, pll->info->name);
}

/* Takes a crtc's reference on a PLL in the atomic state (intel_reference_shared_dpll()). */
static void
i915_reference_shared_dpll(
	struct i915_lcd_world *world,
	struct intel_atomic_state *state,
	const struct intel_crtc *crtc,
	const struct intel_shared_dpll *pll,
	const struct intel_dpll_hw_state *pll_state)
{
	struct intel_shared_dpll_state *shared_dpll;

	UNUSED_PARAMETER(state);

	/* The atomic state's PLL states are the world's pool. */
	shared_dpll = drv_i915_lcd_shared_dpll_state(world);

	/* The first user sets the PLL's state. */
	if (shared_dpll[pll->index].pipe_mask == 0)
		shared_dpll[pll->index].hw_state = *pll_state;

	/* References it for the crtc. */
	i915_reference_shared_dpll_crtc(crtc, pll, &shared_dpll[pll->index]);
}

/* Drops a crtc's reference on a PLL in the atomic state (intel_unreference_shared_dpll()). */
static void
i915_unreference_shared_dpll(
	struct i915_lcd_world *world,
	struct intel_atomic_state *state,
	const struct intel_crtc *crtc,
	const struct intel_shared_dpll *pll)
{
	struct intel_shared_dpll_state *shared_dpll;

	UNUSED_PARAMETER(state);

	/* The atomic state's PLL states are the world's pool. */
	shared_dpll = drv_i915_lcd_shared_dpll_state(world);

	/* Drops the crtc's reference. */
	drv_i915_unreference_shared_dpll_crtc(crtc, pll, &shared_dpll[pll->index]);
}

/* The output frequency of a combo PLL state (icl_ddi_combo_pll_get_freq()), or 0. */
static int
i915_icl_ddi_combo_pll_get_freq(
	struct drm_i915_private *i915,
	const struct intel_shared_dpll *pll,
	const struct intel_dpll_hw_state *pll_state)
{
	int ref_clock;
	u32 dco_fraction;
	u32 p0;
	u32 p1;
	u32 p2;
	u32 dco_freq;
	bool halve;

	UNUSED_PARAMETER(pll);

	/* Takes the P and K fields, and Q when the Q divider is in use. */
	ref_clock = i915_icl_wrpll_ref_clock(i915);
	p0 = pll_state->cfgcr1 & DPLL_CFGCR1_PDIV_MASK;
	p2 = pll_state->cfgcr1 & DPLL_CFGCR1_KDIV_MASK;
	if (pll_state->cfgcr1 & DPLL_CFGCR1_QDIV_MODE(1)) {
		p1 = (pll_state->cfgcr1 & DPLL_CFGCR1_QDIV_RATIO_MASK) >>
			DPLL_CFGCR1_QDIV_RATIO_SHIFT;
	} else {
		p1 = 1;
	}

	/* Decodes P (an unknown field value is left as read). */
	switch (p0) {
	case DPLL_CFGCR1_PDIV_2:
		p0 = 2;
		break;
	case DPLL_CFGCR1_PDIV_3:
		p0 = 3;
		break;
	case DPLL_CFGCR1_PDIV_5:
		p0 = 5;
		break;
	case DPLL_CFGCR1_PDIV_7:
		p0 = 7;
		break;
	default:
		break;
	}

	/* Decodes K (an unknown field value is left as read). */
	switch (p2) {
	case DPLL_CFGCR1_KDIV_1:
		p2 = 1;
		break;
	case DPLL_CFGCR1_KDIV_2:
		p2 = 2;
		break;
	case DPLL_CFGCR1_KDIV_3:
		p2 = 3;
		break;
	default:
		break;
	}

	/* The DCO: integer times the reference, plus the 15-bit fraction. */
	dco_freq = (pll_state->cfgcr0 & DPLL_CFGCR0_DCO_INTEGER_MASK) *
		   (u32)ref_clock;
	dco_fraction = (pll_state->cfgcr0 & DPLL_CFGCR0_DCO_FRACTION_MASK) >>
		       DPLL_CFGCR0_DCO_FRACTION_SHIFT;

	/* Display WA #22010492432 halved the programmed fraction. */
	halve = i915_ehl_combo_pll_div_frac_wa_needed(i915);
	if (halve)
		dco_fraction *= 2;
	dco_freq += (dco_fraction * (u32)ref_clock) / 0x8000;

	/* A zero divider is a warning of the Linux text. */
	if (p0 == 0 || p1 == 0 || p2 == 0) {
		drv_i915_lcd_error("WARN_ON(p0 == 0 || p1 == 0 || p2 == 0)\n");
		return 0;
	}

	/* Succeeded: the DCO over the dividers and the 5 of the AFE. */
	return (int)(dco_freq / (p0 * p1 * p2 * 5));
}

/* Reads a combo PLL's hardware state (combo_pll_get_hw_state()). */
static bool
i915_combo_pll_get_hw_state(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	struct intel_dpll_hw_state *hw_state)
{
	i915_reg_t enable_reg;
	bool enabled;

	/* Reads the PLL through its enable register. */
	enable_reg = i915_combo_pll_enable_reg(i915, pll);
	enabled = i915_icl_pll_get_hw_state(i915, pll, hw_state, enable_reg);

	/* Succeeded: reports whether the PLL is enabled. */
	return enabled;
}

/*
 * Reads a combo PLL's configuration when it is enabled
 * (icl_pll_get_hw_state()); the display core is powered only if it already
 * is.
 */
static bool
i915_icl_pll_get_hw_state(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	struct intel_dpll_hw_state *hw_state,
	i915_reg_t enable_reg)
{
	intel_wakeref_t wakeref;
	bool enabled;
	u32 val;

	/* A readout never turns the display core on. */
	wakeref = drv_i915_n1_power_get_if_enabled(i915, POWER_DOMAIN_DISPLAY_CORE);
	if (!wakeref)
		return false;

	/* Reads the configuration only of an enabled PLL. */
	enabled = false;
	val = i915_lcd_intel_de_read(i915, enable_reg);
	if (val & PLL_ENABLE) {
		i915_icl_pll_read_config(i915, pll, hw_state);
		enabled = true;
	}

	/* Gives the display core back. */
	i915_lcd_intel_display_power_put(i915, POWER_DOMAIN_DISPLAY_CORE, wakeref);

	/* Succeeded: reports whether the PLL is enabled. */
	return enabled;
}

/* Reads an enabled combo PLL's configuration registers (the platform branches of icl_pll_get_hw_state()). */
static void
i915_icl_pll_read_config(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll,
	struct intel_dpll_hw_state *hw_state)
{
	enum intel_dpll_id id;

	/* Reads the dividers of the platform. */
	id = pll->info->id;
	if (IS_ALDERLAKE_S(i915)) {
		hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, ADLS_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, ADLS_DPLL_CFGCR1(id));
	} else if (IS_DG1(i915)) {
		hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, DG1_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, DG1_DPLL_CFGCR1(id));
	} else if (IS_ROCKETLAKE(i915)) {
		hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, RKL_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, RKL_DPLL_CFGCR1(id));
	} else if (I915_LCD_DISPLAY_VER(i915) >= 12) {
		hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, TGL_DPLL_CFGCR0(id));
		hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, TGL_DPLL_CFGCR1(id));
		if (i915->display.vbt.override_afc_startup) {
			hw_state->div0 = i915_lcd_intel_de_read(i915, TGL_DPLL0_DIV0(id));
			hw_state->div0 &= TGL_DPLL0_DIV0_AFC_STARTUP_MASK;
		}
	} else {
		if ((IS_JASPERLAKE(i915) || IS_ELKHARTLAKE(i915)) &&
		    id == DPLL_ID_EHL_DPLL4) {
			hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, ICL_DPLL_CFGCR0(4));
			hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, ICL_DPLL_CFGCR1(4));
		} else {
			hw_state->cfgcr0 = i915_lcd_intel_de_read(i915, ICL_DPLL_CFGCR0(id));
			hw_state->cfgcr1 = i915_lcd_intel_de_read(i915, ICL_DPLL_CFGCR1(id));
		}
	}
}

/*
 * Reads one PLL out and finds the crtcs that use it (readout_dpll_hw_state()).
 *
 * The Linux for_each_intel_crtc() of this text walked the takeover
 * registry.
 */
static void
i915_readout_dpll_hw_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	unsigned index;

	/* Reads the PLL; an enabled one takes its power domain. */
	pll->on = drv_i915_dpll_get_hw_state(i915, pll, &pll->state.hw_state);
	if (pll->on && pll->info->power_domain)
		pll->wakeref = i915_lcd_intel_display_power_get(i915, pll->info->power_domain);

	/* References the PLL for every active crtc of the registry that uses it. */
	pll->state.pipe_mask = 0;
	for (index = 0u; ; index++) {
		crtc = drv_i915_n1_crtc_at(takeover, index);
		if (crtc == NULL)
			break;

		/* Only an active crtc on this PLL uses it. */
		crtc_state = to_intel_crtc_state(crtc->base.state);
		if (crtc_state->hw.active && crtc_state->shared_dpll == pll)
			i915_reference_shared_dpll_crtc(crtc, pll, &pll->state);
	}

	/* The pipes that use the PLL are the pipes that drive it. */
	pll->active_mask = pll->state.pipe_mask;

	/* Notes the readout. */
	I915_LCD_DRM_DBG_KMS(&i915->drm,
		"%s hw state readout: pipe_mask 0x%x, on %i\n",
		pll->info->name, pll->state.pipe_mask, pll->on);
}

/* Turns off a PLL that is on but used by no pipe (sanitize_dpll_state()). */
static void
i915_sanitize_dpll_state(
	struct drm_i915_private *i915,
	struct intel_shared_dpll *pll)
{
	/* A PLL that is off needs nothing. */
	if (!pll->on)
		return;

	/* Wa_16011069516:adl-p[a0] for an enabled DPLL0. */
	i915_adlp_cmtg_clock_gating_wa(i915, pll);

	/* A PLL a pipe drives stays on. */
	if (pll->active_mask)
		return;

	/* Turns it off. */
	I915_LCD_DRM_DBG_KMS(&i915->drm,
		"%s enabled but not in use, disabling\n",
		pll->info->name);
	i915_intel_disable_shared_dpll(i915, pll);
}

/* Binds the modeset object's device view to the world's pool (built on first use). */
static void
i915_lcd_dpll_pool_init(
	struct i915_lcd_world *world,
	struct i915_lcd_modeset *ms)
{
	/* Binds the pool. */
	drv_i915_lcd_dpll_pool_bind(world, &ms->i915);
}
