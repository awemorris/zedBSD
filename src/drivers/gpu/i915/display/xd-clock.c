/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display clocks of the display 12 and 13 code (see xd-clock.h).
 */

#include "xd-clock.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "xd-tc.h"

#include "../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include "../intel/display-regs-xd9.h"

/*
 * How long the CD clock PLL may take to lose its lock, to lock, and to
 * acknowledge a crawl.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (icl_cdclk_pll_disable, icl_cdclk_pll_enable, adlp_cdclk_pll_crawl).
 */
#define I915_XD_CDCLK_PLL_US		1000U

/*
 * How the firmware is asked whether a CD clock change may go ahead: each
 * request may take 500 us, the requests are repeated 10 us apart for
 * 3 ms, then for 50 ms more; the voltage level is told with one request
 * of 500 us.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (skl_pcode_request, __snb_pcode_rw, snb_pcode_write), and
 * drivers/gpu/drm/i915/display/intel_cdclk.c (bxt_set_cdclk: the base of
 * 3 ms).
 */
#define I915_XD_CDCLK_MAILBOX_US	500U
#define I915_XD_CDCLK_PREPARE_GAP_US	10U
#define I915_XD_CDCLK_PREPARE_US	3000U
#define I915_XD_CDCLK_PREPARE_MORE_US	50000U

/*
 * The highest CD clock of display 11 on: on a 24 MHz reference, and on
 * the others.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (intel_update_max_cdclk).
 */
#define I915_XD_CDCLK_MAX_24MHZ		648000U
#define I915_XD_CDCLK_MAX		652800U

/*
 * The bypass clock of display 11, which does not follow the reference.
 *
 * Value referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_get_cdclk).
 */
#define I915_XD_CDCLK_BYPASS_11_KHZ	50000U

/*
 * The 24 MHz reference, which has its own Thunderbolt PLL words; the
 * 38.4 MHz reference, which the port PLLs divide by 2; the range the
 * DCO of a combo DPLL must be in for an HDMI clock; and how long a port
 * PLL's power and lock may take to come and go.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_tbt_pll, icl_wrpll_ref_clock, icl_calc_wrpll,
 * icl_pll_power_enable, icl_pll_enable, icl_pll_disable).
 */
#define I915_XD_DPLL_REF_24		24000U
#define I915_XD_DPLL_REF_38P4		38400U
#define I915_XD_DPLL_DCO_MIN_KHZ	7998000U
#define I915_XD_DPLL_DCO_MAX_KHZ	10000000U
#define I915_XD_DPLL_US			1000U

/*
 * A Type-C PLL: the DCO of every DisplayPort link rate and the range of
 * an HDMI clock (without spread spectrum), the largest second divider,
 * the feedback pre-divider, the bits of the fraction, the DCO above which
 * the loop filter is stronger and its proportional and integral
 * coefficients below and above it, the SSC step count, and the constants
 * of the TDC target (2 / (3 ns * 8 * 50 * 1.1), in tenths, over the
 * reference in kHz) and of the feed-forward gain.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors: dco_min_freq, dco_max_freq of DisplayPort
 * and of HDMI without SSC, div2 from 10; icl_calc_mg_pll_state: m1div,
 * the 22 bits of m2div_frac, the 9 GHz of the coefficients, prop_coeff,
 * int_coeff, ssc_steplog, tdc_targetcnt, feedfwgain).
 */
#define I915_XD_TC_PLL_DP_DCO_KHZ	8100000U
#define I915_XD_TC_PLL_HDMI_DCO_MIN_KHZ	7992000U
#define I915_XD_TC_PLL_HDMI_DCO_MAX_KHZ	10000000U
#define I915_XD_TC_PLL_DSDIV_MAX	10U
#define I915_XD_TC_PLL_FBPREDIV		2U
#define I915_XD_TC_PLL_FRACTION_BITS	22U
#define I915_XD_TC_PLL_HIGH_DCO_KHZ	9000000U
#define I915_XD_TC_PLL_PROP_LOW		4U
#define I915_XD_TC_PLL_INTEG_LOW	8U
#define I915_XD_TC_PLL_PROP_HIGH	5U
#define I915_XD_TC_PLL_INTEG_HIGH	10U
#define I915_XD_TC_PLL_SSC_STEPS	4U
#define I915_XD_TC_PLL_TDC_NUMERATOR	2000000000ULL
#define I915_XD_TC_PLL_TDC_DENOMINATOR	132U
#define I915_XD_TC_PLL_GAIN_NUMERATOR	100000000U

/* The fields of each Type-C PLL register the PLL's words set; the rest of the register is kept. */
#define I915_XD_TC_PLL_HSCLK_FIELDS	(I915_XD_DKL_HSCLK_TLINEDRV | I915_XD_DKL_HSCLK_CORE_INPUTSEL | \
					 I915_XD_DKL_HSCLK_HSDIV | I915_XD_DKL_HSCLK_DSDIV)
#define I915_XD_TC_PLL_DIV0_FIELDS	(I915_XD_DKL_DIV0_INTEG_COEFF | I915_XD_DKL_DIV0_PROP_COEFF | \
					 I915_XD_DKL_DIV0_FBPREDIV | I915_XD_DKL_DIV0_FBDIV_INT)
#define I915_XD_TC_PLL_DIV1_FIELDS	(I915_XD_DKL_DIV1_IREF_TRIM | I915_XD_DKL_DIV1_TDC_TARGET)
#define I915_XD_TC_PLL_SSC_FIELDS	(I915_XD_DKL_SSC_IREF_NDIV | I915_XD_DKL_SSC_STEP_LEN | I915_XD_DKL_SSC_STEP_NUM | \
					 I915_XD_DKL_SSC_ENABLE)
#define I915_XD_TC_PLL_BIAS_FIELDS	(I915_XD_DKL_BIAS_FRAC_ENABLE | I915_XD_DKL_BIAS_FBDIV_FRAC)
#define I915_XD_TC_PLL_TDC_FIELDS	(I915_XD_DKL_TDC_SSC_STEP_SIZE | I915_XD_DKL_TDC_FEED_FWD_GAIN)

/*
 * The line driver clock select of a Type-C PLL with a second divider:
 * 1 in a Dekel PHY, 2 in an MG PHY.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors: tlinedrv).
 */
#define I915_XD_TC_PLL_LINEDRV_DKL	1U
#define I915_XD_TC_PLL_LINEDRV_MG	2U

/*
 * An MG PLL (Ice Lake): the feedback pre-divider it doubles when the
 * integer divider would not fit in 8 bits, the loop filter's gain, the
 * fractional lock's threshold, the SSC type, the TDC select, and the
 * fixed bias fields (the GB select, the initial DCO amplitude, the bias
 * bonus, the C trim and the reference RDAC).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_mg_pll_state: m1div of 4, MG_PLL_LF_GAINCTRL(1),
 * MG_PLL_FRAC_LOCK_LOCKTHRESH(10), MG_PLL_SSC_TYPE(2), MG_PLL_TDC_TDCSEL(3),
 * MG_PLL_BIAS_BIAS_GB_SEL(3), MG_PLL_BIAS_INIT_DCOAMP(0x3F),
 * MG_PLL_BIAS_BIAS_BONUS(10), MG_PLL_BIAS_CTRIM(12),
 * MG_PLL_BIAS_VREF_RDAC(4)).
 */
#define I915_XD_MG_PLL_FBPREDIV_HIGH	4U
#define I915_XD_MG_PLL_INT_MAX		255U
#define I915_XD_MG_PLL_LF_GAIN		1U
#define I915_XD_MG_PLL_LOCK_THRESHOLD	10U
#define I915_XD_MG_PLL_SSC_TYPE		2U
#define I915_XD_MG_PLL_TDC_SELECT	3U
#define I915_XD_MG_PLL_BIAS_GB		3U
#define I915_XD_MG_PLL_BIAS_DCOAMP	0x3fU
#define I915_XD_MG_PLL_BIAS_BONUS	10U
#define I915_XD_MG_PLL_BIAS_CTRIM	12U
#define I915_XD_MG_PLL_BIAS_RDAC	4U

/* The fields of the MG PLL's clock top words the words set; the rest of each register is kept. */
#define I915_XD_MG_PLL_HSCLK_FIELDS	I915_XD_TC_PLL_HSCLK_FIELDS

/*
 * The CD clock of display 9 for each frequency select of CDCLK_CTL, with
 * DPLL 0's VCO at 8100 MHz (the first row) and at 8640 MHz (the second).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (CDCLK_CTL, CD Frequency Select), Intel Kaby Lake PRM
 * Volume 12, Display Engine (the CD clock frequencies of each VCO).
 */
static const uint32_t i915_xd9_cdclk_khz[2][4] = {
	{ 450000U, 540000U, 337500U, 675000U },
	{ 432000U, 540000U, 308571U, 617143U }
};

/*
 * The DisplayPort port clocks DPLL_CTRL1's rate field gives, each with its
 * code (the link clock is half the port clock).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DPLL_CTRL1, Link Rate).
 */
static const struct i915_xd9_link_rate {
	uint32_t port_khz;
	uint32_t code;
} i915_xd9_link_rates[] = {
	{ 540000U, I915_XD9_LINK_RATE_2700 },
	{ 270000U, I915_XD9_LINK_RATE_1350 },
	{ 162000U, I915_XD9_LINK_RATE_810 },
	{ 324000U, I915_XD9_LINK_RATE_1620 },
	{ 216000U, I915_XD9_LINK_RATE_1080 },
	{ 432000U, I915_XD9_LINK_RATE_2160 }
};

/*
 * The DPLL's three central DCO frequencies in HDMI mode, in the order
 * the divider search tries them, with their CFGCR2 codes.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DPLL frequency calculation's central frequencies), Intel Kaby Lake PRM
 * Volume 2c, Command Reference: Registers (DPLL_CFGCR2, Central
 * Frequency).
 */
static const struct i915_xd9_wrpll_central {
	uint64_t hz;
	uint32_t code;
} i915_xd9_wrpll_centrals[3] = {
	{ 8400000000ULL, I915_XD9_CFGCR2_CENTRAL_8400 },
	{ 9000000000ULL, I915_XD9_CFGCR2_CENTRAL_9000 },
	{ 9600000000ULL, I915_XD9_CFGCR2_CENTRAL_9600 }
};

/*
 * The dividers between the DCO and the AFE clock the HDMI search tries,
 * the even ones first (an even divider is taken whenever one fits), each
 * split into the P, Q and K dividers that make it up, as their CFGCR2
 * codes.  A divider of 5 needs a P divider of 5, which has no code: the
 * search may pick it, and the clock is then refused.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DPLL frequency calculation's divider lists and their split into P0, P1
 * and P2), Intel Kaby Lake PRM Volume 2c, Command Reference: Registers
 * (DPLL_CFGCR2, Pdiv, Qdiv Ratio, Kdiv).
 */
static const struct i915_xd9_wrpll_divider {
	uint8_t divider;
	uint8_t even;
	uint8_t usable;
	uint8_t pdiv;
	uint8_t qdiv;
	uint8_t kdiv;
} i915_xd9_wrpll_dividers[] = {
	{ 4U, 1U, 1U, 1U, 1U, 1U },
	{ 6U, 1U, 1U, 1U, 1U, 2U },
	{ 8U, 1U, 1U, 1U, 2U, 1U },
	{ 10U, 1U, 1U, 1U, 1U, 0U },
	{ 12U, 1U, 1U, 1U, 3U, 1U },
	{ 14U, 1U, 1U, 4U, 1U, 1U },
	{ 16U, 1U, 1U, 1U, 4U, 1U },
	{ 18U, 1U, 1U, 2U, 3U, 1U },
	{ 20U, 1U, 1U, 1U, 5U, 1U },
	{ 24U, 1U, 1U, 1U, 6U, 1U },
	{ 28U, 1U, 1U, 1U, 7U, 1U },
	{ 30U, 1U, 1U, 2U, 5U, 1U },
	{ 32U, 1U, 1U, 1U, 8U, 1U },
	{ 36U, 1U, 1U, 1U, 9U, 1U },
	{ 40U, 1U, 1U, 1U, 10U, 1U },
	{ 42U, 1U, 1U, 2U, 7U, 1U },
	{ 44U, 1U, 1U, 1U, 11U, 1U },
	{ 48U, 1U, 1U, 1U, 12U, 1U },
	{ 52U, 1U, 1U, 1U, 13U, 1U },
	{ 54U, 1U, 1U, 2U, 9U, 1U },
	{ 56U, 1U, 1U, 1U, 14U, 1U },
	{ 60U, 1U, 1U, 1U, 15U, 1U },
	{ 64U, 1U, 1U, 1U, 16U, 1U },
	{ 66U, 1U, 1U, 2U, 11U, 1U },
	{ 68U, 1U, 1U, 1U, 17U, 1U },
	{ 70U, 1U, 1U, 4U, 5U, 1U },
	{ 72U, 1U, 1U, 1U, 18U, 1U },
	{ 76U, 1U, 1U, 1U, 19U, 1U },
	{ 78U, 1U, 1U, 2U, 13U, 1U },
	{ 80U, 1U, 1U, 1U, 20U, 1U },
	{ 84U, 1U, 1U, 1U, 21U, 1U },
	{ 88U, 1U, 1U, 1U, 22U, 1U },
	{ 90U, 1U, 1U, 2U, 15U, 1U },
	{ 92U, 1U, 1U, 1U, 23U, 1U },
	{ 96U, 1U, 1U, 1U, 24U, 1U },
	{ 98U, 1U, 1U, 4U, 7U, 1U },
	{ 3U, 0U, 1U, 2U, 1U, 3U },
	{ 5U, 0U, 0U, 0U, 1U, 3U },
	{ 7U, 0U, 1U, 4U, 1U, 3U },
	{ 9U, 0U, 1U, 2U, 1U, 2U },
	{ 15U, 0U, 1U, 2U, 1U, 0U },
	{ 21U, 0U, 1U, 4U, 1U, 2U },
	{ 35U, 0U, 1U, 4U, 1U, 0U }
};

/* The reference clock of display 9's DPLLs, in Hz. */
#define I915_XD9_DPLL_REF_HZ		24000000ULL

/*
 * How far the DCO may run from its central frequency, in ten-thousandths:
 * 1 % above it, 6 % below it.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DPLL frequency calculation's allowed deviations).
 */
#define I915_XD9_DCO_ABOVE		100U
#define I915_XD9_DCO_BELOW		600U

/* The AFE clock of an HDMI port is five times its pixel clock. */
#define I915_XD9_AFE_PER_PIXEL		5U

/*
 * How long a display 9 DPLL may take to lock, in microseconds.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DPLL enable sequence's lock time, with room).
 */
#define I915_XD9_DPLL_LOCK_US		5000U

static uint32_t i915_xd_clock_div_round(uint32_t a, uint32_t b);
static uint32_t i915_xd_cdclk_divider(uint32_t cdclk_khz, uint32_t vco_khz);
static uint32_t i915_xd_cdclk_freq_field(uint32_t cdclk_khz);
static int i915_xd_cdclk_prepare(struct i915_xd_io *io);
static void i915_xd_cdclk_pll_off(struct i915_xd_io *io, struct i915_xd_cdclk *state, struct i915_xd_cdclk_report *report);
static void i915_xd_cdclk_pll_on(struct i915_xd_io *io, struct i915_xd_cdclk *state, uint32_t vco_khz, struct i915_xd_cdclk_report *report);
static void i915_xd_cdclk_pll_retune(struct i915_xd_io *io, struct i915_xd_cdclk *state, uint32_t vco_khz, struct i915_xd_cdclk_report *report);
static void i915_xd_cdclk_program(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_cdclk *state, const struct i915_xd_cdclk *target, struct i915_xd_cdclk_report *report);
static void i915_xd_cdclk_report_clear(struct i915_xd_cdclk_report *report);
static void i915_xd_dpll_pack(const struct i915_xd_platform *platform, uint32_t ref_khz, uint32_t dco_integer, uint32_t dco_fraction, uint32_t qdiv_ratio, uint32_t qdiv_mode, uint32_t kdiv, uint32_t pdiv, int afc, struct i915_xd_dpll_words *words);
static void i915_xd_dpll_map_place(const struct i915_xd_platform *platform, unsigned phy, uint32_t *word, uint32_t *shift, uint32_t *off);
static uint32_t i915_xd_dpll_div0(const struct i915_xd_platform *platform, const struct i915_xd_pll_desc *pll);
static uint32_t i915_xd_dpll_p_code(uint32_t p);
static uint32_t i915_xd_dpll_p_value(uint32_t code);
static uint32_t i915_xd_dpll_k_code(uint32_t k);
static uint32_t i915_xd_dpll_k_value(uint32_t code);
static int i915_xd_tc_pll_iref(uint32_t ref_khz, uint32_t *ndiv, uint32_t *trim, uint32_t *pulse);
static int i915_xd_tc_pll_dividers(uint32_t port_khz, int hdmi, uint32_t *div1, uint32_t *div2, uint32_t *dco_khz);
static void i915_xd_tc_pll_clock_tops(int hdmi, uint32_t div1, uint32_t div2, uint32_t line_driver, uint32_t *refclkin, uint32_t *coreclk, uint32_t *hsclk);
static uint32_t i915_xd_tc_pll_hsdiv_code(uint32_t divider);
static uint32_t i915_xd_tc_pll_hsdiv_value(uint32_t code);
static void i915_xd_tc_pll_put(struct i915_xd_dkl *dkl, unsigned tc, uint32_t phy_offset, uint32_t fields, uint32_t value);
static int i915_xd9_wrpll_deviation(uint64_t dco_hz, uint64_t central_hz, uint64_t *deviation);

/*
 * Reads the CD clock the hardware runs: the reference from DSSM, the
 * PLL's rate when it is on and locked (its ratio times the reference),
 * the bypass clock (half the reference; 50 MHz on display 11), and the
 * CD clock (the PLL's rate
 * through the CD2X divider, or the bypass clock).  The voltage level is
 * not readable; the one the CD clock needs is assumed.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_get_cdclk, bxt_de_pll_readout, icl_readout_refclk).
 */
void
drv_i915_xd_cdclk_read(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_cdclk *state)
{
	uint32_t dssm;
	uint32_t pll;
	uint32_t control;
	uint32_t divider_x2;

	/* The reference clock; a code DSSM does not define reads as 24 MHz. */
	dssm = (drv_i915_xd_io_read(io, I915_XD_DSSM) & I915_XD_DSSM_REFCLK) >> 29;
	switch (dssm) {
	case I915_XD_DSSM_REFCLK_19P2:
		state->ref_khz = 19200U;
		break;
	case I915_XD_DSSM_REFCLK_38P4:
		state->ref_khz = 38400U;
		break;
	default:
		state->ref_khz = 24000U;
		break;
	}

	/* The PLL's rate, 0 when it is off or not locked. */
	pll = drv_i915_xd_io_read(io, I915_XD_CDCLK_PLL_ENABLE);
	state->vco_khz = 0U;
	if ((pll & I915_XD_CDCLK_PLL_ON) != 0U && (pll & I915_XD_CDCLK_PLL_LOCK) != 0U)
		state->vco_khz = (pll & I915_XD_CDCLK_PLL_RATIO) * state->ref_khz;

	/* The bypass clock is half the reference (50 MHz on display 11), and is the CD clock while the PLL is off. */
	state->bypass_khz = state->ref_khz / 2U;
	if (platform->display_ver < 12U)
		state->bypass_khz = I915_XD_CDCLK_BYPASS_11_KHZ;
	state->cdclk_khz = state->bypass_khz;

	/* The CD clock through the CD2X divider (given here doubled: 1, 1.5, 2, 4). */
	if (state->vco_khz != 0U) {
		control = drv_i915_xd_io_read(io, I915_XD_CDCLK_CTL);
		switch ((control & I915_XD_CDCLK_CD2X_DIV) >> 22) {
		case I915_XD_CDCLK_CD2X_DIV_1_5:
			divider_x2 = 3U;
			break;
		case I915_XD_CDCLK_CD2X_DIV_2:
			divider_x2 = 4U;
			break;
		case I915_XD_CDCLK_CD2X_DIV_4:
			divider_x2 = 8U;
			break;
		default:
			divider_x2 = 2U;
			break;
		}

		/* The CD clock the divider gives from the VCO. */
		state->cdclk_khz = i915_xd_clock_div_round(state->vco_khz, divider_x2);
	}

	/* The voltage level the CD clock needs. */
	state->voltage = drv_i915_xd_cdclk_voltage(platform, state->cdclk_khz);
}

/*
 * Reports the voltage level a CD clock needs: the first level whose
 * highest CD clock covers it, or the top level.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (tgl_calc_voltage_level, calc_voltage_level).
 */
uint8_t
drv_i915_xd_cdclk_voltage(
	const struct i915_xd_platform *platform,
	uint32_t cdclk_khz)
{
	unsigned level;

	/* Walks the levels from the lowest. */
	for (level = 0U; level < platform->nvoltage; level++) {
		if (cdclk_khz <= platform->voltage_steps[level])
			return (uint8_t)level;
	}

	/* A platform without levels has only level 0. */
	if (platform->nvoltage == 0U)
		return 0U;

	/* Succeeded: a CD clock above every level takes the top one. */
	return (uint8_t)(platform->nvoltage - 1U);
}

/*
 * Reports the lowest CD clock of the platform's table on a reference
 * that is at least a minimum, or 0 when none is.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_calc_cdclk).
 */
uint32_t
drv_i915_xd_cdclk_lowest(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	uint32_t min_khz)
{
	unsigned row;

	/* The table lists each reference's CD clocks from the lowest. */
	for (row = 0U; row < platform->ncdclk; row++) {
		if (platform->cdclk[row].ref_khz != ref_khz)
			continue;
		if (platform->cdclk[row].cdclk_khz < min_khz)
			continue;

		/* Succeeded: the first one fast enough. */
		return platform->cdclk[row].cdclk_khz;
	}

	/* No CD clock of the reference is fast enough. */
	return 0U;
}

/*
 * Reports the PLL rate a CD clock of the table needs on the state's
 * reference: 0 for the bypass clock, and 0 for a CD clock the table does
 * not have.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_calc_cdclk_pll_vco).
 */
uint32_t
drv_i915_xd_cdclk_vco(
	const struct i915_xd_platform *platform,
	const struct i915_xd_cdclk *state,
	uint32_t cdclk_khz)
{
	unsigned row;

	/* The bypass clock runs with the PLL off. */
	if (cdclk_khz == state->bypass_khz)
		return 0U;

	/* Looks the CD clock up for the reference. */
	for (row = 0U; row < platform->ncdclk; row++) {
		if (platform->cdclk[row].ref_khz != state->ref_khz)
			continue;
		if (platform->cdclk[row].cdclk_khz != cdclk_khz)
			continue;

		/* Succeeded: the ratio times the reference. */
		return state->ref_khz * platform->cdclk[row].ratio;
	}

	/* The table has no such CD clock. */
	return 0U;
}

/*
 * Tells whether a CD clock read from the hardware may be kept (1 or 0):
 * the PLL runs, the CD clock is not the bypass clock, it is one of the
 * table's with the PLL rate the table gives it, and the control word
 * holds its divider and decimal frequency (the pipe the last change
 * waited for is not compared).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_sanitize_cdclk).
 */
int
drv_i915_xd_cdclk_legal(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_cdclk *state)
{
	uint32_t control;
	uint32_t expected;
	uint32_t cdclk;
	uint32_t vco;

	/* A PLL that is off, or the bypass clock, is replaced. */
	if (state->vco_khz == 0U)
		return 0;
	if (state->cdclk_khz == state->bypass_khz)
		return 0;

	/* The CD clock must be one of the table's. */
	cdclk = drv_i915_xd_cdclk_lowest(platform, state->ref_khz, state->cdclk_khz);
	if (cdclk != state->cdclk_khz)
		return 0;

	/* The PLL must run at the rate the table gives it. */
	vco = drv_i915_xd_cdclk_vco(platform, state, cdclk);
	if (vco != state->vco_khz)
		return 0;

	/* The control word must hold its divider and decimal frequency, whatever pipe it waited for. */
	control = drv_i915_xd_io_read(io, I915_XD_CDCLK_CTL) & ~I915_XD_CDCLK_CD2X_PIPE_FIELD;
	expected = i915_xd_cdclk_freq_field(cdclk) | i915_xd_cdclk_divider(cdclk, vco);
	if (control != expected)
		return 0;

	/* Succeeded: the CD clock may be kept. */
	return 1;
}

/*
 * Changes the CD clock to a target: asks the firmware whether it may (a
 * refusal changes nothing), changes the PLL and writes the control word
 * (synchronized to no pipe), tells the firmware the voltage level (a
 * refusal leaves the hardware changed and the state as it was, the PLL's
 * rate excepted), then reads the CD clock back with the voltage level
 * asked for.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_set_cdclk).
 */
void
drv_i915_xd_cdclk_set(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_cdclk *state,
	const struct i915_xd_cdclk *target,
	struct i915_xd_cdclk_report *report)
{
	uint32_t data;
	int error;

	/* Asks the firmware whether the change may go ahead. */
	report->requested = *target;
	error = i915_xd_cdclk_prepare(io);
	report->prepare_error = error;
	if (error != 0)
		return;

	/* Changes the PLL and writes the control word. */
	i915_xd_cdclk_program(io, platform, state, target, report);

	/* Tells the firmware the voltage level the new CD clock needs. */
	data = target->voltage;
	error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_CDCLK_CONTROL, &data, NULL, I915_XD_CDCLK_MAILBOX_US);
	report->voltage_error = error;
	if (error != 0)
		return;

	/* Reads the CD clock back; the voltage level is the one asked for. */
	drv_i915_xd_cdclk_read(io, platform, state);
	state->voltage = target->voltage;
}

/*
 * Brings the CD clock up at the display's start: keeps the one the
 * firmware left when it may be kept, and otherwise sets the table's
 * lowest.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_cdclk_init_hw, bxt_sanitize_cdclk).
 */
void
drv_i915_xd_cdclk_init(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_cdclk *state,
	struct i915_xd_cdclk_report *report)
{
	struct i915_xd_cdclk target;
	int legal;

	/* Reads what the firmware left, and keeps it when it may be kept. */
	i915_xd_cdclk_report_clear(report);
	drv_i915_xd_cdclk_read(io, platform, state);
	report->before = *state;
	legal = drv_i915_xd_cdclk_legal(io, platform, state);
	if (legal) {
		report->kept = 1;
		return;
	}

	/* Forgets it: the PLL in an unknown state is turned off and on again. */
	report->replaced = 1;
	state->cdclk_khz = 0U;
	state->vco_khz = I915_XD_CDCLK_VCO_UNKNOWN;

	/* Sets the table's lowest CD clock. */
	target = *state;
	target.cdclk_khz = drv_i915_xd_cdclk_lowest(platform, state->ref_khz, 0U);
	target.vco_khz = drv_i915_xd_cdclk_vco(platform, state, target.cdclk_khz);
	target.voltage = drv_i915_xd_cdclk_voltage(platform, target.cdclk_khz);
	drv_i915_xd_cdclk_set(io, platform, state, &target, report);
}

/*
 * Takes the CD clock down before a suspend: the bypass clock with the
 * PLL off.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_cdclk_uninit_hw).
 */
void
drv_i915_xd_cdclk_uninit(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_cdclk *state,
	struct i915_xd_cdclk_report *report)
{
	struct i915_xd_cdclk target;

	/* The bypass clock with the PLL off, at the voltage level it needs. */
	i915_xd_cdclk_report_clear(report);
	target = *state;
	target.cdclk_khz = target.bypass_khz;
	target.vco_khz = 0U;
	target.voltage = drv_i915_xd_cdclk_voltage(platform, target.cdclk_khz);

	/* Sets it. */
	drv_i915_xd_cdclk_set(io, platform, state, &target, report);
}

/*
 * Reports the highest CD clock the display may use on the state's
 * reference.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (intel_update_max_cdclk).
 */
uint32_t
drv_i915_xd_cdclk_max(
	const struct i915_xd_cdclk *state)
{
	/* A 24 MHz reference reaches a little less. */
	if (state->ref_khz == 24000U)
		return I915_XD_CDCLK_MAX_24MHZ;

	/* Succeeded: the 19.2 and 38.4 MHz references' highest. */
	return I915_XD_CDCLK_MAX;
}

/*
 * Reports the CD clock of a display 9 part in kHz from LCPLL1_CTL,
 * DPLL_CTRL1 and CDCLK_CTL, or 0 when DPLL 0 does not run locked.
 *
 * DPLL 0 makes the CD clock.  Its VCO is 8640 MHz while it runs a link
 * at 1080 or 2160 MHz and 8100 MHz at every other rate, and CDCLK_CTL's
 * frequency select picks one of four clocks of that VCO.
 *
 * Procedure referenced: Intel Kaby Lake PRM Volume 12, Display Engine
 * (the CD clock and DPLL 0's VCO).
 */
uint32_t
drv_i915_xd9_cdclk_khz(
	uint32_t lcpll1_ctl,
	uint32_t dpll_ctrl1,
	uint32_t cdclk_ctl)
{
	uint32_t rate;
	uint32_t select;
	unsigned vco;

	/* DPLL 0 off makes no CD clock. */
	if ((lcpll1_ctl & I915_XD9_LCPLL_ENABLE) == 0U)
		return 0U;

	/* Nor does DPLL 0 before it locks. */
	if ((lcpll1_ctl & I915_XD9_LCPLL_LOCK) == 0U)
		return 0U;

	/* The VCO DPLL 0's link rate implies: the second row for 1080 and 2160 MHz. */
	rate = (dpll_ctrl1 >> I915_XD9_DPLL_CTRL1_RATE_SHIFT) & I915_XD9_DPLL_CTRL1_RATE_MASK;
	vco = 0U;
	if (rate == I915_XD9_LINK_RATE_1080) {
		vco = 1U;
	} else if (rate == I915_XD9_LINK_RATE_2160) {
		vco = 1U;
	}

	/* The frequency select. */
	select = (cdclk_ctl >> I915_XD9_CDCLK_SELECT_SHIFT) & I915_XD9_CDCLK_SELECT_MASK;

	/* Succeeded: the clock of the select on the VCO. */
	return i915_xd9_cdclk_khz[vco][select];
}

/*
 * Computes the DPLL words of a DisplayPort port clock on display 9: the
 * override and the link rate in the DPLL's field of DPLL_CTRL1, and no
 * configuration words.  Returns 0, or EINVAL for a port clock DPLL_CTRL1
 * has no rate for.
 */
int
drv_i915_xd9_dpll_dp_words(
	uint32_t port_khz,
	struct i915_xd9_dpll_words *words)
{
	unsigned index;

	/* No words until the clock is found. */
	words->ctrl1 = 0U;
	words->cfgcr1 = 0U;
	words->cfgcr2 = 0U;
	words->port_khz = 0U;

	/* Looks the port clock up among the rates. */
	for (index = 0U; index < sizeof(i915_xd9_link_rates) / sizeof(i915_xd9_link_rates[0]); index++) {
		if (i915_xd9_link_rates[index].port_khz != port_khz)
			continue;

		/* Succeeded: the override and the rate rule the DPLL. */
		words->ctrl1 = I915_XD9_DPLL_CTRL1_OVERRIDE | (i915_xd9_link_rates[index].code << I915_XD9_DPLL_CTRL1_RATE_SHIFT);
		words->port_khz = port_khz;
		return 0;
	}

	/* No rate gives the clock. */
	return EINVAL;
}

/*
 * Reports the DisplayPort port clock in kHz a DPLL_CTRL1 rate code gives,
 * or zero for a code that names none.
 */
uint32_t
drv_i915_xd9_dpll_rate_port_khz(
	uint32_t code)
{
	unsigned index;

	/* The code's row. */
	for (index = 0U; index < sizeof(i915_xd9_link_rates) / sizeof(i915_xd9_link_rates[0]); index++) {
		if (i915_xd9_link_rates[index].code == code)
			return i915_xd9_link_rates[index].port_khz;
	}

	/* Succeeded in learning that the code names no rate. */
	return 0U;
}

/*
 * Computes the DPLL words of an HDMI pixel clock on display 9.
 *
 * In HDMI mode the DPLL's DCO runs at a multiple of the AFE clock (five
 * times the pixel clock), and a divider made of the P, Q and K dividers
 * brings it down.  The divider chosen keeps the DCO nearest to one of the
 * three central frequencies, within 1 % above and 6 % below it; an even
 * divider is taken before an odd one, and among equal deviations the one
 * found first (by central frequency, then by divider).  The words are the
 * HDMI mode and the override in DPLL_CTRL1, the DCO as a multiple of the
 * 24 MHz reference in CFGCR1, and the dividers and the central frequency
 * in CFGCR2.  Returns 0, or EINVAL when no divider fits or the one chosen
 * cannot be made.
 *
 * Procedure referenced: Intel Kaby Lake PRM Volume 12, Display Engine
 * (the DPLL frequency calculation for HDMI).
 */
int
drv_i915_xd9_dpll_hdmi_words(
	uint32_t pixel_khz,
	struct i915_xd9_dpll_words *words)
{
	const struct i915_xd9_wrpll_divider *divider;
	const struct i915_xd9_wrpll_divider *best;
	const struct i915_xd9_wrpll_central *best_central;
	uint64_t afe_hz;
	uint64_t dco_hz;
	uint64_t deviation;
	uint64_t best_deviation;
	uint64_t dco_integer;
	uint64_t dco_millionths;
	uint64_t dco_fraction;
	unsigned pass;
	unsigned want_even;
	unsigned central;
	unsigned index;
	int fits;
	int exact;

	/* No words until a divider is chosen. */
	words->ctrl1 = 0U;
	words->cfgcr1 = 0U;
	words->cfgcr2 = 0U;
	words->port_khz = 0U;

	/* The AFE clock the DCO divides down to. */
	afe_hz = (uint64_t)pixel_khz * 1000U * I915_XD9_AFE_PER_PIXEL;

	/* The even dividers in a first pass, the odd ones in a second only when no even one fits. */
	best = NULL;
	best_central = NULL;
	best_deviation = 0U;
	exact = 0;
	for (pass = 0U; pass < 2U && best == NULL; pass++) {
		/* The pass's dividers: the even ones first. */
		want_even = 0U;
		if (pass == 0U)
			want_even = 1U;

		/* Each central frequency in turn, until a divider meets one exactly. */
		for (central = 0U; central < 3U && !exact; central++) {
			/* Each divider of the pass against the central frequency. */
			for (index = 0U; index < sizeof(i915_xd9_wrpll_dividers) / sizeof(i915_xd9_wrpll_dividers[0]); index++) {
				divider = &i915_xd9_wrpll_dividers[index];
				if (divider->even != want_even)
					continue;

				/* The DCO this divider needs, and how far it is from the centre. */
				dco_hz = (uint64_t)divider->divider * afe_hz;
				fits = i915_xd9_wrpll_deviation(dco_hz, i915_xd9_wrpll_centrals[central].hz, &deviation);
				if (!fits)
					continue;

				/* Only a smaller deviation than the best one replaces it. */
				if (best != NULL && deviation >= best_deviation)
					continue;
				best = divider;
				best_central = &i915_xd9_wrpll_centrals[central];
				best_deviation = deviation;

				/* A DCO on the centre cannot be bettered. */
				if (deviation == 0U) {
					exact = 1;
					break;
				}
			}
		}
	}

	/* No divider keeps the DCO in range. */
	if (best == NULL)
		return EINVAL;

	/* A divider whose P divider has no code cannot be made. */
	if (!best->usable)
		return EINVAL;

	/*
	 * The DCO as a multiple of the reference: its integer part, and its
	 * fraction in 1/0x8000, taken from the multiple in millionths.
	 */
	dco_hz = (uint64_t)best->divider * afe_hz;
	dco_integer = dco_hz / I915_XD9_DPLL_REF_HZ;
	dco_millionths = dco_hz / (I915_XD9_DPLL_REF_HZ / 1000000U) - dco_integer * 1000000U;
	dco_fraction = (dco_millionths * 0x8000U) / 1000000U;

	/* DPLL_CTRL1: the HDMI mode under the override. */
	words->ctrl1 = I915_XD9_DPLL_CTRL1_OVERRIDE | I915_XD9_DPLL_CTRL1_HDMI;

	/* CFGCR1: the frequency enable, the DCO's fraction and integer. */
	words->cfgcr1 = I915_XD9_CFGCR1_FREQ_ENABLE;
	words->cfgcr1 |= (uint32_t)dco_fraction << I915_XD9_CFGCR1_FRACTION_SHIFT;
	words->cfgcr1 |= (uint32_t)dco_integer;

	/* CFGCR2: the Q divider (its mode only when it divides), the K and P dividers, the central frequency. */
	words->cfgcr2 = (uint32_t)best->qdiv << I915_XD9_CFGCR2_QDIV_RATIO_SHIFT;
	if (best->qdiv != 1U)
		words->cfgcr2 |= I915_XD9_CFGCR2_QDIV_MODE;
	words->cfgcr2 |= (uint32_t)best->kdiv << I915_XD9_CFGCR2_KDIV_SHIFT;
	words->cfgcr2 |= (uint32_t)best->pdiv << I915_XD9_CFGCR2_PDIV_SHIFT;
	words->cfgcr2 |= best_central->code;

	/* Succeeded: the DPLL makes the pixel clock's TMDS clock. */
	words->port_khz = pixel_khz;
	return 0;
}

/*
 * Starts a display 9 DPLL with its words: its six bits of DPLL_CTRL1, its
 * configuration words (zero for DisplayPort), its enable, and the wait for
 * its lock.  DPLL 0 makes the CD clock and is never started here.
 * Returns 0, EINVAL for a DPLL the platform does not let the driver
 * start, or ETIMEDOUT (the DPLL is left enabled for the caller to stop).
 */
int
drv_i915_xd9_dpll_enable(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned id,
	const struct i915_xd9_dpll_words *words)
{
	const struct i915_xd_pll_desc *pll;
	uint32_t shift;
	int error;

	/* DPLLs 1 to 3 only. */
	if (id == 0U)
		return EINVAL;
	pll = drv_i915_xd_dpll_desc(platform, id);
	if (pll == NULL)
		return EINVAL;

	/* The DPLL's field of DPLL_CTRL1: HDMI mode, link rate and override. */
	shift = id * I915_XD9_DPLL_CTRL1_BITS;
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DPLL_CTRL1, 0x3fU << shift, words->ctrl1 << shift);
	drv_i915_xd_io_posting_read(io, I915_XD9_DPLL_CTRL1);

	/* The configuration words, pushed out. */
	drv_i915_xd_io_write(io, pll->cfgcr0, words->cfgcr1);
	drv_i915_xd_io_write(io, pll->cfgcr1, words->cfgcr2);
	drv_i915_xd_io_posting_read(io, pll->cfgcr0);
	drv_i915_xd_io_posting_read(io, pll->cfgcr1);

	/* The enable, and the lock. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD9_DPLL_ENABLE);
	error = drv_i915_xd_io_poll(io, I915_XD9_DPLL_STATUS, I915_XD9_DPLL_LOCK(id), I915_XD9_DPLL_LOCK(id), I915_XD9_DPLL_LOCK_US, NULL);
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: the DPLL runs. */
	return 0;
}

/*
 * Stops a display 9 DPLL other than DPLL 0.
 */
void
drv_i915_xd9_dpll_disable(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned id)
{
	const struct i915_xd_pll_desc *pll;

	/* DPLL 0 is never stopped here. */
	if (id == 0U)
		return;
	pll = drv_i915_xd_dpll_desc(platform, id);
	if (pll == NULL)
		return;

	/* Succeeded: the enable off, pushed out. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, I915_XD9_DPLL_ENABLE, 0U);
	drv_i915_xd_io_posting_read(io, pll->enable);
}

/*
 * Clocks a display 9 DDI from a DPLL (on: the DDI's clock on and the DPLL
 * selected under the override), or turns the DDI's clock off.
 */
void
drv_i915_xd9_ddi_clock(
	struct i915_xd_io *io,
	unsigned ddi,
	unsigned dpll,
	int on)
{
	uint32_t shift;
	uint32_t clear;
	uint32_t set;

	/* Off: the DDI's clock off bit. */
	if (!on) {
		(void)drv_i915_xd_io_rmw(io, I915_XD9_DPLL_CTRL2, 0U, I915_XD9_DPLL_CTRL2_CLOCK_OFF(ddi));
		return;
	}

	/* On: the off bit and the old select cleared, the DPLL selected with the override. */
	shift = ddi * I915_XD9_DPLL_CTRL2_BITS;
	clear = I915_XD9_DPLL_CTRL2_CLOCK_OFF(ddi) | (I915_XD9_DPLL_CTRL2_SELECT_MASK << (shift + I915_XD9_DPLL_CTRL2_SELECT_SHIFT));
	set = (dpll << (shift + I915_XD9_DPLL_CTRL2_SELECT_SHIFT)) | (I915_XD9_DPLL_CTRL2_OVERRIDE << shift);
	(void)drv_i915_xd_io_rmw(io, I915_XD9_DPLL_CTRL2, clear, set);
}

/* Divides, rounding to the closest. */
static uint32_t
i915_xd_clock_div_round(
	uint32_t a,
	uint32_t b)
{
	/* Half the divisor added before the truncating division. */
	return (a + b / 2U) / b;
}

/*
 * Reports the CD2X divider field of a CD clock on a PLL rate: 1.5, 2 or
 * 4 by their ratio, and 1 for any other (the bypass clock with the PLL
 * off among them).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (bxt_cdclk_cd2x_div_sel).
 */
static uint32_t
i915_xd_cdclk_divider(
	uint32_t cdclk_khz,
	uint32_t vco_khz)
{
	uint32_t ratio;

	/* A CD clock of 0 has no ratio. */
	if (cdclk_khz == 0U)
		return I915_XD_CDCLK_CD2X_DIV_1 << 22;

	/* Picks the divider by the ratio of the PLL rate to the CD clock. */
	ratio = i915_xd_clock_div_round(vco_khz, cdclk_khz);
	switch (ratio) {
	case 3U:
		return I915_XD_CDCLK_CD2X_DIV_1_5 << 22;
	case 4U:
		return I915_XD_CDCLK_CD2X_DIV_2 << 22;
	case 8U:
		return I915_XD_CDCLK_CD2X_DIV_4 << 22;
	default:
		break;
	}

	/* Succeeded: any other ratio divides by 1. */
	return I915_XD_CDCLK_CD2X_DIV_1 << 22;
}

/*
 * Reports the decimal frequency field of a CD clock: (the CD clock less
 * 1 MHz) in 0.5 MHz, rounded to the closest.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (skl_cdclk_decimal).
 */
static uint32_t
i915_xd_cdclk_freq_field(
	uint32_t cdclk_khz)
{
	/* A CD clock below 1 MHz has no decimal. */
	if (cdclk_khz < 1000U)
		return 0U;

	/* Succeeded: the field. */
	return i915_xd_clock_div_round(cdclk_khz - 1000U, 500U) & I915_XD_CDCLK_DECIMAL;
}

/*
 * Asks the firmware whether a CD clock change may go ahead: the request
 * is repeated until the firmware answers that it is ready, for 3 ms with
 * 10 us between the requests and then 50 ms more.  Returns 0, the last
 * request's mailbox error, or ETIMEDOUT when the firmware never answered
 * ready.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (skl_pcode_request, skl_pcode_try_request).
 */
static int
i915_xd_cdclk_prepare(
	struct i915_xd_io *io)
{
	uint32_t data;
	unsigned waited_us;
	int delayed;
	int error;

	/* Asks until the answer is ready or the time is spent. */
	waited_us = 0U;
	error = 0;
	for (;;) {
		/* One request; a busy mailbox, a refusal or an answer that is not ready asks again. */
		data = I915_XD_MAILBOX_CDCLK_PREPARE;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_CDCLK_CONTROL, &data, NULL, I915_XD_CDCLK_MAILBOX_US);
		if (error == 0 && (data & I915_XD_MAILBOX_CDCLK_READY) == I915_XD_MAILBOX_CDCLK_READY)
			return 0;

		/* Stops once the whole time is spent. */
		if (waited_us >= I915_XD_CDCLK_PREPARE_US + I915_XD_CDCLK_PREPARE_MORE_US)
			break;

		/* Waits before the next request; a time base that fails stops at once. */
		delayed = drv_i915_xd_io_delay(io, I915_XD_CDCLK_PREPARE_GAP_US);
		if (delayed != 0)
			return EIO;
		waited_us += I915_XD_CDCLK_PREPARE_GAP_US;
	}

	/* Reports the mailbox's error, or that the firmware never answered ready. */
	if (error != 0)
		return error;

	/* The firmware answered, but never that it was ready. */
	return ETIMEDOUT;
}

/*
 * Turns the CD clock PLL off and waits for its lock to go.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (icl_cdclk_pll_disable).
 */
static void
i915_xd_cdclk_pll_off(
	struct i915_xd_io *io,
	struct i915_xd_cdclk *state,
	struct i915_xd_cdclk_report *report)
{
	int error;

	/* Clears the enable and waits for the lock to drop. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_ON, 0U);
	error = drv_i915_xd_io_poll(io, I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_LOCK, 0U, I915_XD_CDCLK_PLL_US, NULL);
	if (error != 0)
		report->unlock_late = 1;

	/* The PLL is off. */
	state->vco_khz = 0U;
}

/*
 * Turns the CD clock PLL on at a rate: the ratio first, then the ratio
 * with the enable, and waits for the lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (icl_cdclk_pll_enable).
 */
static void
i915_xd_cdclk_pll_on(
	struct i915_xd_io *io,
	struct i915_xd_cdclk *state,
	uint32_t vco_khz,
	struct i915_xd_cdclk_report *report)
{
	uint32_t ratio;
	int error;

	/* The ratio, then the ratio with the enable. */
	ratio = i915_xd_clock_div_round(vco_khz, state->ref_khz) & I915_XD_CDCLK_PLL_RATIO;
	drv_i915_xd_io_write(io, I915_XD_CDCLK_PLL_ENABLE, ratio);
	drv_i915_xd_io_write(io, I915_XD_CDCLK_PLL_ENABLE, ratio | I915_XD_CDCLK_PLL_ON);

	/* Waits for the lock. */
	error = drv_i915_xd_io_poll(io, I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_LOCK, I915_XD_CDCLK_PLL_LOCK, I915_XD_CDCLK_PLL_US, NULL);
	if (error != 0)
		report->lock_late = 1;

	/* The PLL runs at the rate. */
	state->vco_khz = vco_khz;
}

/*
 * Moves the running CD clock PLL to another rate without turning it off:
 * the new ratio with the enable, then the change request, a wait for the
 * lock and the acknowledge, and the request withdrawn.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (adlp_cdclk_pll_crawl).
 */
static void
i915_xd_cdclk_pll_retune(
	struct i915_xd_io *io,
	struct i915_xd_cdclk *state,
	uint32_t vco_khz,
	struct i915_xd_cdclk_report *report)
{
	uint32_t value;
	int error;

	/* The new ratio with the PLL still on, then the change request. */
	value = (i915_xd_clock_div_round(vco_khz, state->ref_khz) & I915_XD_CDCLK_PLL_RATIO) | I915_XD_CDCLK_PLL_ON;
	drv_i915_xd_io_write(io, I915_XD_CDCLK_PLL_ENABLE, value);
	value |= I915_XD_CDCLK_PLL_FREQ_REQUEST;
	drv_i915_xd_io_write(io, I915_XD_CDCLK_PLL_ENABLE, value);

	/* Waits for the lock and the acknowledge. */
	error = drv_i915_xd_io_poll(io, I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_LOCK | I915_XD_CDCLK_PLL_FREQ_ACK,
		I915_XD_CDCLK_PLL_LOCK | I915_XD_CDCLK_PLL_FREQ_ACK, I915_XD_CDCLK_PLL_US, NULL);
	if (error != 0)
		report->crawl_late = 1;

	/* Withdraws the request; the PLL runs at the rate. */
	value &= ~I915_XD_CDCLK_PLL_FREQ_REQUEST;
	drv_i915_xd_io_write(io, I915_XD_CDCLK_PLL_ENABLE, value);
	state->vco_khz = vco_khz;
}

/*
 * Changes the PLL to the target's rate (a crawl between two running
 * rates where the platform can, otherwise off and on again) and writes
 * the control word: the divider, no pipe to wait for, and the decimal
 * frequency.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (_bxt_set_cdclk, icl_cdclk_pll_update).
 */
static void
i915_xd_cdclk_program(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_cdclk *state,
	const struct i915_xd_cdclk *target,
	struct i915_xd_cdclk_report *report)
{
	uint32_t control;
	int crawl;

	/* A crawl needs the platform's support and two running, known rates. */
	report->programmed = 1;
	crawl = 0;
	if (platform->cdclk_crawl &&
	    state->vco_khz != 0U &&
	    state->vco_khz != I915_XD_CDCLK_VCO_UNKNOWN &&
	    target->vco_khz != 0U)
		crawl = 1;

	/* Moves the PLL to the target's rate. */
	if (crawl) {
		if (state->vco_khz != target->vco_khz)
			i915_xd_cdclk_pll_retune(io, state, target->vco_khz, report);
	} else {
		/* A PLL at another rate goes off first; then it comes on at the target's, unless that is off. */
		if (state->vco_khz != 0U && state->vco_khz != target->vco_khz)
			i915_xd_cdclk_pll_off(io, state, report);
		if (state->vco_khz != target->vco_khz)
			i915_xd_cdclk_pll_on(io, state, target->vco_khz, report);
	}

	/* The control word: the divider, no pipe to wait for, the decimal frequency. */
	control = i915_xd_cdclk_divider(target->cdclk_khz, target->vco_khz);
	control |= I915_XD_CDCLK_CD2X_PIPE_NONE;
	control |= i915_xd_cdclk_freq_field(target->cdclk_khz);
	drv_i915_xd_io_write(io, I915_XD_CDCLK_CTL, control);
}

/* Starts a CD clock change's report with nothing in it. */
static void
i915_xd_cdclk_report_clear(
	struct i915_xd_cdclk_report *report)
{
	/* Nothing found, asked or seen yet. */
	report->before.ref_khz = 0U;
	report->before.vco_khz = 0U;
	report->before.cdclk_khz = 0U;
	report->before.bypass_khz = 0U;
	report->before.voltage = 0U;
	report->kept = 0;
	report->replaced = 0;
	report->requested = report->before;
	report->prepare_error = 0;
	report->voltage_error = 0;
	report->programmed = 0;
	report->unlock_late = 0;
	report->lock_late = 0;
	report->crawl_late = 0;
}

/*
 * Packs a DCO and dividers into the words of a combo DPLL: the fraction
 * halved where the platform needs it with a 38.4 MHz reference, the
 * normal crystal select of display 12 on (the 8400 MHz central frequency
 * before), and the AFC startup field when the VBT overrides it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_dpll_state, ehl_combo_pll_div_frac_wa_needed).
 */
static void
i915_xd_dpll_pack(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	uint32_t dco_integer,
	uint32_t dco_fraction,
	uint32_t qdiv_ratio,
	uint32_t qdiv_mode,
	uint32_t kdiv,
	uint32_t pdiv,
	int afc,
	struct i915_xd_dpll_words *words)
{
	/* With a 38.4 MHz reference some platforms take half the fraction, rounded. */
	if (platform->frac_wa_38p4 && ref_khz == I915_XD_DPLL_REF_38P4)
		dco_fraction = (dco_fraction + 1U) / 2U;

	/* The DCO. */
	words->cfgcr0 = i915_xd_field_put(I915_XD_CFGCR0_DCO_FRACTION, dco_fraction) |
			(dco_integer & I915_XD_CFGCR0_DCO_INTEGER);

	/* The dividers; display 12 on selects the normal crystal, which is a 0 field. */
	words->cfgcr1 = i915_xd_field_put(I915_XD_CFGCR1_QDIV_RATIO, qdiv_ratio) |
			i915_xd_field_put(I915_XD_CFGCR1_KDIV, kdiv) |
			i915_xd_field_put(I915_XD_CFGCR1_PDIV, pdiv);
	if (qdiv_mode != 0U)
		words->cfgcr1 |= I915_XD_CFGCR1_QDIV_MODE;
	if (platform->display_ver < 12U)
		words->cfgcr1 |= I915_XD_CFGCR1_CENTRAL_FREQ_8400;

	/* The VBT's AFC startup, when it has one. */
	words->div0 = 0U;
	words->div0_set = 0;
	if (afc >= 0) {
		words->div0 = i915_xd_field_put(I915_XD_DPLL_DIV0_AFC_STARTUP, (uint32_t)afc);
		words->div0_set = 1;
	}
}

/*
 * Works out the words of a combo DPLL for a DisplayPort link rate from
 * the table of the reference (a 38.4 MHz reference uses the 19.2 MHz
 * rows).  afc is the VBT's AFC startup override, or
 * I915_XD_DPLL_AFC_NONE.  Returns 0, or EINVAL for a rate the table does
 * not have.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_dp_combo_pll, icl_calc_dpll_state).
 */
int
drv_i915_xd_dpll_dp_words(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	uint32_t link_khz,
	int afc,
	struct i915_xd_dpll_words *words)
{
	const struct i915_xd_dpll_dp_row *row;

	/* The row of the link rate. */
	row = drv_i915_xd_dpll_dp(ref_khz, link_khz);
	if (row == NULL)
		return EINVAL;

	/* Packs it. */
	i915_xd_dpll_pack(platform, ref_khz, row->dco_integer, row->dco_fraction, row->qdiv_ratio, row->qdiv_mode,
		row->kdiv, row->pdiv, afc, words);

	/* Succeeded: the words are the caller's. */
	return 0;
}

/*
 * Works out the words of a combo DPLL for an HDMI TMDS clock.  The PLL's
 * output (five times the TMDS clock, the AFE clock) times one of the
 * dividers the hardware allows gives the DCO; of the dividers that put
 * the DCO between 7998 and 10000 MHz the one nearest the middle wins.
 * The divider is then split into P, Q and K, and the DCO is expressed in
 * the reference (19.2 MHz for a 38.4 MHz one) with a 15-bit fraction.
 * Returns 0, or EINVAL when no divider fits.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_wrpll: the dividers and the DCO range).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_wrpll, icl_wrpll_get_multipliers, icl_wrpll_params_populate,
 * icl_wrpll_ref_clock, icl_calc_dpll_state).
 */
int
drv_i915_xd_dpll_hdmi_words(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	uint32_t tmds_khz,
	int afc,
	struct i915_xd_dpll_words *words)
{
	static const uint8_t dividers[] = {
		2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40,
		42, 44, 48, 50, 52, 54, 56, 60, 64, 66, 68, 70, 72, 76, 78, 80,
		84, 88, 90, 92, 96, 98, 100, 102, 3, 5, 7, 9, 15, 21
	};
	uint32_t afe_khz;
	uint32_t dco_khz;
	uint32_t middle;
	uint32_t distance;
	uint32_t best_distance;
	uint32_t best_dco;
	uint32_t best_divider;
	uint32_t p;
	uint32_t q;
	uint32_t k;
	uint32_t wrpll_ref;
	uint64_t dco_fixed;
	unsigned index;

	/* The divider whose DCO is in range and nearest the middle. */
	afe_khz = tmds_khz * 5U;
	middle = (I915_XD_DPLL_DCO_MIN_KHZ + I915_XD_DPLL_DCO_MAX_KHZ) / 2U;
	best_distance = 0xffffffffU;
	best_dco = 0U;
	best_divider = 0U;
	for (index = 0U; index < sizeof(dividers) / sizeof(dividers[0]); index++) {
		dco_khz = afe_khz * dividers[index];
		if (dco_khz < I915_XD_DPLL_DCO_MIN_KHZ || dco_khz > I915_XD_DPLL_DCO_MAX_KHZ)
			continue;

		/* Keeps the divider whose DCO is nearest the middle. */
		distance = dco_khz >= middle ? dco_khz - middle : middle - dco_khz;
		if (distance < best_distance) {
			best_distance = distance;
			best_divider = dividers[index];
			best_dco = dco_khz;
		}
	}

	/* No divider fits. */
	if (best_divider == 0U)
		return EINVAL;

	/*
	 * Splits the divider: 2, 3, 5 and 7 alone; the other odd ones as
	 * three times P; the even ones as P times Q times K = 2.
	 */
	q = 1U;
	k = 1U;
	if (best_divider == 2U ||
	    best_divider == 3U ||
	    best_divider == 5U ||
	    best_divider == 7U) {
		p = best_divider;
	} else if ((best_divider & 1U) != 0U) {
		p = best_divider / 3U;
		k = 3U;
	} else if (best_divider % 4U == 0U) {
		p = 2U;
		q = best_divider / 4U;
		k = 2U;
	} else if (best_divider % 6U == 0U) {
		p = 3U;
		q = best_divider / 6U;
		k = 2U;
	} else if (best_divider % 5U == 0U) {
		p = 5U;
		q = best_divider / 10U;
		k = 2U;
	} else {
		p = 7U;
		q = best_divider / 14U;
		k = 2U;
	}

	/* The DCO as a 15-bit fixed point multiple of the reference the PLL sees. */
	wrpll_ref = ref_khz;
	if (wrpll_ref == I915_XD_DPLL_REF_38P4)
		wrpll_ref = ref_khz / 2U;
	if (wrpll_ref == 0U)
		return EINVAL;
	dco_fixed = ((uint64_t)best_dco << 15) / wrpll_ref;

	/* Packs it with the dividers in their register encoding. */
	i915_xd_dpll_pack(platform, ref_khz, (uint32_t)(dco_fixed >> 15), (uint32_t)(dco_fixed & 0x7fffU), q, q != 1U ? 1U : 0U,
		i915_xd_dpll_k_code(k), i915_xd_dpll_p_code(p), afc, words);

	/* Succeeded: the words are the caller's. */
	return 0;
}

/*
 * Works out the words of the Thunderbolt PLL, which runs at one rate:
 * its DCO from the table of the reference; its dividers are not used,
 * except on Ice Lake, whose row of the reference has them.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_tbt_pll, icl_calc_dpll_state).
 */
void
drv_i915_xd_dpll_tbt_words(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	int afc,
	struct i915_xd_dpll_words *words)
{
	const struct i915_xd_dpll_dp_row *row;
	const struct i915_xd_dpll_dco *dco;

	/* Ice Lake's PLL has its dividers too: the row of the reference. */
	if (platform->tbt_pll_19p2 != NULL) {
		row = platform->tbt_pll_19p2;
		if (ref_khz == I915_XD_DPLL_REF_24)
			row = platform->tbt_pll_24;
		i915_xd_dpll_pack(platform, ref_khz, row->dco_integer, row->dco_fraction, row->qdiv_ratio, row->qdiv_mode, row->kdiv,
			row->pdiv, afc, words);
		return;
	}

	/* Succeeded: the DCO of the reference, no dividers. */
	dco = drv_i915_xd_tbt_pll(ref_khz);
	i915_xd_dpll_pack(platform, ref_khz, dco->dco_integer, dco->dco_fraction, 0U, 0U, 0U, 0U, afc, words);
}

/*
 * Reports the link clock a combo DPLL's words give (the DCO over the
 * dividers and the AFE's 5), in kHz; 0 when a divider field holds no
 * divider.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_ddi_combo_pll_get_freq, __cnl_ddi_wrpll_get_freq).
 */
uint32_t
drv_i915_xd_dpll_port_khz(
	const struct i915_xd_platform *platform,
	uint32_t ref_khz,
	const struct i915_xd_dpll_words *words)
{
	uint32_t wrpll_ref;
	uint32_t fraction;
	uint32_t dco_khz;
	uint32_t p;
	uint32_t q;
	uint32_t k;

	/* The dividers: P and K from their codes, Q only in its mode. */
	p = i915_xd_dpll_p_value(i915_xd_field_get(I915_XD_CFGCR1_PDIV, words->cfgcr1));
	k = i915_xd_dpll_k_value(i915_xd_field_get(I915_XD_CFGCR1_KDIV, words->cfgcr1));
	q = 1U;
	if ((words->cfgcr1 & I915_XD_CFGCR1_QDIV_MODE) != 0U)
		q = i915_xd_field_get(I915_XD_CFGCR1_QDIV_RATIO, words->cfgcr1);
	if (p == 0U ||
	    q == 0U ||
	    k == 0U)
		return 0U;

	/* The DCO: the integer and the fraction (written halved where the platform needs it). */
	wrpll_ref = ref_khz;
	if (wrpll_ref == I915_XD_DPLL_REF_38P4)
		wrpll_ref = ref_khz / 2U;
	fraction = i915_xd_field_get(I915_XD_CFGCR0_DCO_FRACTION, words->cfgcr0);
	if (platform->frac_wa_38p4 && ref_khz == I915_XD_DPLL_REF_38P4)
		fraction *= 2U;
	dco_khz = (words->cfgcr0 & I915_XD_CFGCR0_DCO_INTEGER) * wrpll_ref;
	dco_khz += (fraction * wrpll_ref) / 0x8000U;

	/* Succeeded: the link clock. */
	return dco_khz / (p * q * k * 5U);
}

/*
 * Reports the PLL of a platform that a DDI selects with a value, or NULL.
 */
const struct i915_xd_pll_desc *
drv_i915_xd_dpll_desc(
	const struct i915_xd_platform *platform,
	unsigned select)
{
	unsigned index;

	/* Looks the select value up. */
	for (index = 0U; index < platform->nplls; index++) {
		if (platform->plls[index].select == select)
			return &platform->plls[index];
	}

	/* Succeeded: no PLL has it. */
	return NULL;
}

/*
 * Turns a combo DPLL or the Thunderbolt PLL on with its words: power,
 * the wait for the power, the configuration words and the AFC startup,
 * a posting read, the enable, the wait for the lock.  Returns the waits
 * that ran out (I915_XD_DPLL_LATE_*); a PLL without configuration words
 * (a Type-C PLL, whose words are in its PHY) is refused with every bit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_power_enable, icl_pll_enable: the waits of 1 ms).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (combo_pll_enable, tbt_pll_enable, icl_pll_power_enable, icl_dpll_write,
 * icl_pll_enable).
 */
unsigned
drv_i915_xd_dpll_enable(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_pll_desc *pll,
	const struct i915_xd_dpll_words *words)
{
	uint32_t div0;
	unsigned late;
	int error;

	/* Only a PLL with configuration words is written here. */
	if (pll->cfgcr0 == 0U)
		return I915_XD_DPLL_LATE_POWER_ON | I915_XD_DPLL_LATE_LOCK;

	/* Powers the PLL and waits for the power. */
	late = 0U;
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_POWER_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_POWER_STATE, I915_XD_PLL_POWER_STATE, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_POWER_ON;

	/* The words, the AFC startup where the platform has the word, then a posting read. */
	drv_i915_xd_io_write(io, pll->cfgcr0, words->cfgcr0);
	drv_i915_xd_io_write(io, pll->cfgcr1, words->cfgcr1);
	div0 = i915_xd_dpll_div0(platform, pll);
	if (words->div0_set && div0 != 0U)
		(void)drv_i915_xd_io_rmw(io, div0, I915_XD_DPLL_DIV0_AFC_STARTUP, words->div0);
	drv_i915_xd_io_posting_read(io, pll->cfgcr1);

	/* Enables it and waits for the lock. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_LOCK, I915_XD_PLL_LOCK, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_LOCK;

	/* Succeeded: the waits that ran out. */
	return late;
}

/*
 * Turns a port PLL of any kind off through its enable word: the enable,
 * the wait for the lock to go, the power, the wait for the power to go.
 * Returns the waits that ran out (I915_XD_DPLL_LATE_*).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_disable: the waits of 1 ms).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_disable, combo_pll_disable, tbt_pll_disable, mg_pll_disable).
 */
unsigned
drv_i915_xd_dpll_disable(
	struct i915_xd_io *io,
	const struct i915_xd_pll_desc *pll)
{
	unsigned late;
	int error;

	/* Clears the enable and waits for the lock to go. */
	late = 0U;
	(void)drv_i915_xd_io_rmw(io, pll->enable, I915_XD_PLL_ENABLE, 0U);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_LOCK, 0U, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_UNLOCK;

	/* Removes the power and waits for it to go. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, I915_XD_PLL_POWER_ENABLE, 0U);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_POWER_STATE, 0U, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_POWER_OFF;

	/* Succeeded: the waits that ran out. */
	return late;
}

/*
 * Reads a port PLL: returns 1 when it is enabled, with the configuration
 * words of a combo DPLL or the Thunderbolt PLL (and the AFC startup field
 * when afc is not I915_XD_DPLL_AFC_NONE and the platform has the word),
 * or 0 when it is off.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_get_hw_state, combo_pll_get_hw_state, tbt_pll_get_hw_state).
 */
int
drv_i915_xd_dpll_read(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_pll_desc *pll,
	int afc,
	struct i915_xd_dpll_words *words)
{
	uint32_t div0;
	uint32_t enable;

	/* A PLL that is not enabled has no state. */
	enable = drv_i915_xd_io_read(io, pll->enable);
	if ((enable & I915_XD_PLL_ENABLE) == 0U)
		return 0;

	/* The configuration words, when the PLL has them. */
	if (pll->cfgcr0 != 0U) {
		words->cfgcr0 = drv_i915_xd_io_read(io, pll->cfgcr0);
		words->cfgcr1 = drv_i915_xd_io_read(io, pll->cfgcr1);
		div0 = i915_xd_dpll_div0(platform, pll);
		if (afc != I915_XD_DPLL_AFC_NONE && div0 != 0U) {
			words->div0 = drv_i915_xd_io_read(io, div0) & I915_XD_DPLL_DIV0_AFC_STARTUP;
			words->div0_set = 1;
		}
	}

	/* Succeeded: the PLL is enabled. */
	return 1;
}

/*
 * Selects a combo DPLL for a combo PHY's DDI clock and ungates the clock,
 * in two writes (the select must be in place before the clock runs).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (_icl_ddi_enable_clock, icl_ddi_combo_enable_clock, rkl_ddi_enable_clock,
 * adls_ddi_enable_clock).
 */
void
drv_i915_xd_dpll_map(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned phy,
	unsigned select)
{
	uint32_t word;
	uint32_t shift;
	uint32_t off;

	/* The select, then the clock-off bit cleared. */
	i915_xd_dpll_map_place(platform, phy, &word, &shift, &off);
	(void)drv_i915_xd_io_rmw(io, word, I915_XD_DPCLKA_SEL_BITS << shift, (select & I915_XD_DPCLKA_SEL_BITS) << shift);
	(void)drv_i915_xd_io_rmw(io, word, off, 0U);
}

/*
 * Gates a combo PHY's DDI clock (the select stays).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (_icl_ddi_disable_clock, icl_ddi_combo_disable_clock).
 */
void
drv_i915_xd_dpll_unmap(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned phy)
{
	uint32_t word;
	uint32_t shift;
	uint32_t off;

	/* Sets the clock-off bit. */
	i915_xd_dpll_map_place(platform, phy, &word, &shift, &off);
	(void)drv_i915_xd_io_rmw(io, word, 0U, off);
}

/*
 * Reads a combo PHY's DDI clock: the DPLL it selects, and returns 1 when
 * the clock is ungated.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (_icl_ddi_get_pll, _icl_ddi_is_clock_enabled).
 */
int
drv_i915_xd_dpll_mapped(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned phy,
	unsigned *select)
{
	uint32_t word;
	uint32_t shift;
	uint32_t off;
	uint32_t value;

	/* Reads the word once for both. */
	i915_xd_dpll_map_place(platform, phy, &word, &shift, &off);
	value = drv_i915_xd_io_read(io, word);
	*select = (value >> shift) & I915_XD_DPCLKA_SEL_BITS;

	/* A clock whose off bit is set does not run. */
	if ((value & off) != 0U)
		return 0;

	/* Succeeded: the clock runs. */
	return 1;
}

/*
 * Works out the words of a Type-C port's PLL for a DisplayPort link rate
 * or an HDMI clock (port_khz) on a reference: the first high-speed and
 * second divider that put the DCO where the output needs it (8.1 GHz for
 * DisplayPort, the HDMI range otherwise), the clock top words, the
 * feedback divider with its fraction, the loop filter, the current
 * reference, the TDC target and the feed-forward gain.  Spread spectrum
 * is not used.  The AFC startup of DIV0 is the VBT's override, or not
 * written (I915_XD_DPLL_AFC_NONE).  Returns 0, or EINVAL when no divider
 * fits or the reference is not one the PHY takes.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors: the DCO of DisplayPort and the HDMI range,
 * the first dividers, the core and line driver selects;
 * icl_calc_mg_pll_state: the feedback pre-divider, the current reference
 * of each reference clock, the TDC target, the feed-forward gain, the loop
 * filter coefficients above and below 9 GHz, the SSC step count).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors, icl_calc_mg_pll_state).
 */
int
drv_i915_xd_tc_pll_words(
	uint32_t ref_khz,
	uint32_t port_khz,
	int hdmi,
	int afc,
	struct i915_xd_tc_pll_words *words)
{
	uint32_t div1;
	uint32_t div2;
	uint32_t dco_khz;
	uint32_t feedback_khz;
	uint32_t integer;
	uint32_t remainder;
	uint32_t fraction;
	uint32_t iref_ndiv;
	uint32_t iref_trim;
	uint32_t iref_pulse;
	uint32_t target;
	uint32_t gain;
	uint32_t proportional;
	uint32_t integral;
	int error;

	/* The current reference of the reference clock. */
	error = i915_xd_tc_pll_iref(ref_khz, &iref_ndiv, &iref_trim, &iref_pulse);
	if (error != 0)
		return error;

	/* The dividers that put the DCO in place. */
	error = i915_xd_tc_pll_dividers(port_khz, hdmi, &div1, &div2, &dco_khz);
	if (error != 0)
		return error;

	/* The feedback divider over twice the reference: an integer of 8 bits and a 22-bit fraction. */
	feedback_khz = ref_khz * I915_XD_TC_PLL_FBPREDIV;
	integer = dco_khz / feedback_khz;
	if (integer > I915_XD_DKL_DIV0_FBDIV_INT)
		return EINVAL;
	remainder = dco_khz % feedback_khz;
	fraction = (uint32_t)(((uint64_t)remainder << I915_XD_TC_PLL_FRACTION_BITS) / feedback_khz);

	/* The clock tops: the reference mux, the core divider, the line driver, input and dividers. */
	i915_xd_tc_pll_clock_tops(hdmi, div1, div2, I915_XD_TC_PLL_LINEDRV_DKL, &words->refclkin, &words->coreclk, &words->hsclk);

	/* The loop filter is stronger above 9 GHz. */
	if (dco_khz >= I915_XD_TC_PLL_HIGH_DCO_KHZ) {
		proportional = I915_XD_TC_PLL_PROP_HIGH;
		integral = I915_XD_TC_PLL_INTEG_HIGH;
	} else {
		proportional = I915_XD_TC_PLL_PROP_LOW;
		integral = I915_XD_TC_PLL_INTEG_LOW;
	}

	/* DIV0: the loop filter, the pre-divider, the integer, and the VBT's AFC startup when it has one. */
	words->div0 = i915_xd_field_put(I915_XD_DKL_DIV0_INTEG_COEFF, integral);
	words->div0 |= i915_xd_field_put(I915_XD_DKL_DIV0_PROP_COEFF, proportional);
	words->div0 |= i915_xd_field_put(I915_XD_DKL_DIV0_FBPREDIV, I915_XD_TC_PLL_FBPREDIV);
	words->div0 |= i915_xd_field_put(I915_XD_DKL_DIV0_FBDIV_INT, integer);
	words->afc_set = 0;
	if (afc != I915_XD_DPLL_AFC_NONE) {
		words->div0 |= i915_xd_field_put(I915_XD_DKL_PLL_DIV0_AFC_STARTUP, (uint32_t)afc);
		words->afc_set = 1;
	}

	/*
	 * DIV1: the current reference trim and the TDC target, 2 / (3 ns *
	 * 8 * 50 * 1.1) over the reference in MHz, rounded: kept in tenths
	 * so the rounding happens once, at the end.
	 */
	target = (uint32_t)(I915_XD_TC_PLL_TDC_NUMERATOR / ((uint64_t)I915_XD_TC_PLL_TDC_DENOMINATOR * ref_khz));
	target = (target + 5U) / 10U;
	words->div1 = i915_xd_field_put(I915_XD_DKL_DIV1_IREF_TRIM, iref_trim);
	words->div1 |= i915_xd_field_put(I915_XD_DKL_DIV1_TDC_TARGET, target);

	/* SSC: the reference divider and the step count; no spread. */
	words->ssc = i915_xd_field_put(I915_XD_DKL_SSC_IREF_NDIV, iref_ndiv);
	words->ssc |= i915_xd_field_put(I915_XD_DKL_SSC_STEP_NUM, I915_XD_TC_PLL_SSC_STEPS);

	/* BIAS: the fraction, enabled when it is not zero. */
	words->bias = i915_xd_field_put(I915_XD_DKL_BIAS_FBDIV_FRAC, fraction);
	if (fraction != 0U)
		words->bias |= I915_XD_DKL_BIAS_FRAC_ENABLE;

	/* TDC_COLDST_BIAS: the feed-forward gain, used only with a fraction. */
	gain = 0U;
	if (remainder != 0U)
		gain = I915_XD_TC_PLL_FBPREDIV * I915_XD_TC_PLL_GAIN_NUMERATOR / (dco_khz * 3U / 10U);
	words->tdc = i915_xd_field_put(I915_XD_DKL_TDC_FEED_FWD_GAIN, gain);

	/* Succeeded: the words are filled. */
	return 0;
}

/*
 * Reports the link clock a Type-C PLL's words give, in kHz: the feedback
 * divider (with its fraction when enabled) times the reference, over the
 * AFE's 5 and the two dividers.  0 when the high-speed divider code is
 * not one the PHY has.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_ddi_mg_pll_get_freq).
 */
uint32_t
drv_i915_xd_tc_pll_port_khz(
	uint32_t ref_khz,
	const struct i915_xd_tc_pll_words *words)
{
	uint64_t clock;
	uint32_t prediv;
	uint32_t integer;
	uint32_t fraction;
	uint32_t div1;
	uint32_t div2;

	/* The feedback divider. */
	prediv = i915_xd_field_get(I915_XD_DKL_DIV0_FBPREDIV, words->div0);
	integer = i915_xd_field_get(I915_XD_DKL_DIV0_FBDIV_INT, words->div0);
	fraction = 0U;
	if ((words->bias & I915_XD_DKL_BIAS_FRAC_ENABLE) != 0U)
		fraction = i915_xd_field_get(I915_XD_DKL_BIAS_FBDIV_FRAC, words->bias);

	/* The dividers; a second divider of 0 divides by 1. */
	div1 = i915_xd_tc_pll_hsdiv_value(i915_xd_field_get(I915_XD_DKL_HSCLK_HSDIV, words->hsclk));
	if (div1 == 0U)
		return 0U;
	div2 = i915_xd_field_get(I915_XD_DKL_HSCLK_DSDIV, words->hsclk);
	if (div2 == 0U)
		div2 = 1U;

	/* The DCO, the fraction's share added after its own shift to keep the rounding small. */
	clock = (uint64_t)prediv * integer * ref_khz;
	clock += ((uint64_t)prediv * fraction * ref_khz) >> I915_XD_TC_PLL_FRACTION_BITS;

	/* Succeeded: the link clock. */
	return (uint32_t)(clock / (5U * div1 * div2));
}

/*
 * Turns a Type-C port's PLL on with its words: the power of its enable
 * word and the wait for it, each word written into the PHY in the fields
 * the PLL uses (the rest of each register kept; DIV0's AFC startup only
 * when the words carry it), a posting read, the enable and the wait for
 * the lock.  Returns the waits that ran out (I915_XD_DPLL_LATE_*); a PLL
 * that is not a Type-C one is refused with every bit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_power_enable, icl_pll_enable: the waits of 1 ms), and
 * drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h (_DKL_REFCLKIN_CTL,
 * _DKL_CLKTOP2_CORECLKCTL1, _DKL_CLKTOP2_HSCLKCTL, _DKL_PLL_DIV0,
 * _DKL_PLL_DIV1, _DKL_PLL_SSC, _DKL_PLL_BIAS, _DKL_PLL_TDC_COLDST_BIAS).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (mg_pll_enable, dkl_pll_write, icl_pll_power_enable, icl_pll_enable).
 */
unsigned
drv_i915_xd_tc_pll_enable(
	struct i915_xd_io *io,
	struct i915_xd_dkl *dkl,
	const struct i915_xd_pll_desc *pll,
	const struct i915_xd_tc_pll_words *words)
{
	uint32_t div0_fields;
	unsigned tc;
	unsigned late;
	int error;

	/* Only a Type-C PLL has its words in a PHY. */
	if (pll->kind != I915_XD_PLL_TC || pll->tc == 0U)
		return I915_XD_DPLL_LATE_POWER_ON | I915_XD_DPLL_LATE_LOCK;
	tc = pll->tc - 1U;

	/* Powers the PLL and waits for the power. */
	late = 0U;
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_POWER_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_POWER_STATE, I915_XD_PLL_POWER_STATE, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_POWER_ON;

	/* The clock tops. */
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_REFCLKIN_CTL, I915_XD_DKL_REFCLKIN_OD_2_MUX, words->refclkin);
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_CLKTOP2_CORECLKCTL1, I915_XD_DKL_CORECLK_A_DIVRATIO, words->coreclk);
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_CLKTOP2_HSCLKCTL, I915_XD_TC_PLL_HSCLK_FIELDS, words->hsclk);

	/* DIV0 in one locked change, with the AFC startup only when the words carry it. */
	div0_fields = I915_XD_TC_PLL_DIV0_FIELDS;
	if (words->afc_set)
		div0_fields |= I915_XD_DKL_PLL_DIV0_AFC_STARTUP;
	drv_i915_xd_dkl_rmw(dkl, tc, I915_XD_DKL_PLL_DIV0, div0_fields, words->div0);

	/* The rest of the dividers, the spread, the fraction and the gain, then a posting read. */
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_PLL_DIV1, I915_XD_TC_PLL_DIV1_FIELDS, words->div1);
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_PLL_SSC, I915_XD_TC_PLL_SSC_FIELDS, words->ssc);
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_PLL_BIAS, I915_XD_TC_PLL_BIAS_FIELDS, words->bias);
	i915_xd_tc_pll_put(dkl, tc, I915_XD_DKL_PLL_TDC_COLDST_BIAS, I915_XD_TC_PLL_TDC_FIELDS, words->tdc);
	(void)drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_TDC_COLDST_BIAS);

	/* Enables it and waits for the lock. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_LOCK, I915_XD_PLL_LOCK, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_LOCK;

	/* Succeeded: the waits that ran out. */
	return late;
}

/*
 * Reads a Type-C port's PLL: returns 1 when it is enabled, with its words
 * (only the fields the PLL uses; DIV0's AFC startup only when afc is not
 * I915_XD_DPLL_AFC_NONE), or 0 when it is off or not a Type-C PLL.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (dkl_pll_get_hw_state).
 */
int
drv_i915_xd_tc_pll_read(
	struct i915_xd_io *io,
	struct i915_xd_dkl *dkl,
	const struct i915_xd_pll_desc *pll,
	int afc,
	struct i915_xd_tc_pll_words *words)
{
	uint32_t div0_fields;
	uint32_t enable;
	unsigned tc;

	/* Only a Type-C PLL has its words in a PHY. */
	if (pll->kind != I915_XD_PLL_TC || pll->tc == 0U)
		return 0;
	tc = pll->tc - 1U;

	/* A PLL that is not enabled has no state. */
	enable = drv_i915_xd_io_read(io, pll->enable);
	if ((enable & I915_XD_PLL_ENABLE) == 0U)
		return 0;

	/* The clock tops. */
	words->refclkin = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_REFCLKIN_CTL) & I915_XD_DKL_REFCLKIN_OD_2_MUX;
	words->coreclk = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_CLKTOP2_CORECLKCTL1) & I915_XD_DKL_CORECLK_A_DIVRATIO;
	words->hsclk = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_CLKTOP2_HSCLKCTL) & I915_XD_TC_PLL_HSCLK_FIELDS;

	/* The dividers, with the AFC startup when it is asked for. */
	div0_fields = I915_XD_TC_PLL_DIV0_FIELDS;
	words->afc_set = 0;
	if (afc != I915_XD_DPLL_AFC_NONE) {
		div0_fields |= I915_XD_DKL_PLL_DIV0_AFC_STARTUP;
		words->afc_set = 1;
	}

	/* Reads the PLL's divider words, keeping the fields the enable writes. */
	words->div0 = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_DIV0) & div0_fields;
	words->div1 = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_DIV1) & I915_XD_TC_PLL_DIV1_FIELDS;

	/* The spread, the fraction and the gain. */
	words->ssc = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_SSC) & I915_XD_TC_PLL_SSC_FIELDS;
	words->bias = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_BIAS) & I915_XD_TC_PLL_BIAS_FIELDS;
	words->tdc = drv_i915_xd_dkl_read(dkl, tc, I915_XD_DKL_PLL_TDC_COLDST_BIAS) & I915_XD_TC_PLL_TDC_FIELDS;

	/* Succeeded: the PLL is enabled. */
	return 1;
}

/*
 * Works out the words of an Ice Lake Type-C port's PLL in its MG PHY for
 * a DisplayPort link rate or an HDMI clock (port_khz) on a reference: the
 * dividers and the clock tops as a Dekel PHY's (with the MG line driver),
 * the feedback divider over twice the reference (four times when the
 * integer would not fit in 8 bits) with its 22-bit fraction, the loop
 * filter with the TDC target, the fractional lock with the feed-forward
 * gain, the SSC words without spread, the bias, and the TDC cold start;
 * on a 38.4 MHz reference only the cold start of the TDC word and none of
 * the bias are written.  Returns 0, or EINVAL when no divider fits or
 * the reference is not one the PHY takes.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_mg_pll_state: the MG words and their masks).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors, icl_calc_mg_pll_state).
 */
int
drv_i915_xd_mg_pll_words(
	uint32_t ref_khz,
	uint32_t port_khz,
	int hdmi,
	struct i915_xd_mg_pll_words *words)
{
	uint32_t div1;
	uint32_t div2;
	uint32_t dco_khz;
	uint32_t prediv;
	uint32_t feedback_khz;
	uint32_t integer;
	uint32_t remainder;
	uint32_t fraction;
	uint32_t iref_ndiv;
	uint32_t iref_trim;
	uint32_t iref_pulse;
	uint32_t target;
	uint32_t gain;
	uint32_t proportional;
	uint32_t integral;
	int error;

	/* The current reference of the reference clock. */
	error = i915_xd_tc_pll_iref(ref_khz, &iref_ndiv, &iref_trim, &iref_pulse);
	if (error != 0)
		return error;

	/* The dividers that put the DCO in place. */
	error = i915_xd_tc_pll_dividers(port_khz, hdmi, &div1, &div2, &dco_khz);
	if (error != 0)
		return error;

	/* The feedback divider: over twice the reference, or four times when 8 bits do not hold it. */
	prediv = I915_XD_TC_PLL_FBPREDIV;
	integer = dco_khz / (ref_khz * prediv);
	if (integer > I915_XD_MG_PLL_INT_MAX) {
		prediv = I915_XD_MG_PLL_FBPREDIV_HIGH;
		integer = dco_khz / (ref_khz * prediv);
	}

	/* Refuses a divider past what the PLL takes. */
	if (integer > I915_XD_MG_PLL_INT_MAX)
		return EINVAL;
	feedback_khz = ref_khz * prediv;
	remainder = dco_khz % feedback_khz;
	fraction = (uint32_t)(((uint64_t)remainder << I915_XD_TC_PLL_FRACTION_BITS) / feedback_khz);

	/* The clock tops: the reference mux, the core divider, the line driver, input and dividers. */
	i915_xd_tc_pll_clock_tops(hdmi, div1, div2, I915_XD_TC_PLL_LINEDRV_MG, &words->refclkin, &words->coreclk, &words->hsclk);

	/* The loop filter is stronger above 9 GHz. */
	if (dco_khz >= I915_XD_TC_PLL_HIGH_DCO_KHZ) {
		proportional = I915_XD_TC_PLL_PROP_HIGH;
		integral = I915_XD_TC_PLL_INTEG_HIGH;
	} else {
		proportional = I915_XD_TC_PLL_PROP_LOW;
		integral = I915_XD_TC_PLL_INTEG_LOW;
	}

	/* The TDC target and the feed-forward gain (used only with a fraction), as a Dekel PHY's. */
	target = (uint32_t)(I915_XD_TC_PLL_TDC_NUMERATOR / ((uint64_t)I915_XD_TC_PLL_TDC_DENOMINATOR * ref_khz));
	target = (target + 5U) / 10U;
	gain = 0U;
	if (remainder != 0U)
		gain = prediv * I915_XD_TC_PLL_GAIN_NUMERATOR / (dco_khz * 3U / 10U);

	/* DIV0: the integer and the fraction, enabled when there is one. */
	words->div0 = i915_xd_field_put(I915_XD_MG_DIV0_FBDIV_FRAC, fraction);
	words->div0 |= i915_xd_field_put(I915_XD_MG_DIV0_FBDIV_INT, integer);
	if (remainder != 0U)
		words->div0 |= I915_XD_MG_DIV0_FRAC_ENABLE;

	/* DIV1: the current reference divider, the dither divider of 2, an N divider of 1 and the pre-divider. */
	words->div1 = I915_XD_MG_DIV1_IREF_NDIV(iref_ndiv);
	words->div1 |= I915_XD_MG_DIV1_DITHER_DIV_2;
	words->div1 |= I915_XD_MG_DIV1_NDIV(1U);
	words->div1 |= i915_xd_field_put(I915_XD_MG_DIV1_FBPREDIV, prediv);

	/* The loop filter: the TDC target, the AFC counter of 512, the gain and the coefficients. */
	words->lf = I915_XD_MG_LF_TDC_TARGET(target);
	words->lf |= I915_XD_MG_LF_AFC_COUNT_512;
	words->lf |= I915_XD_MG_LF_GAIN(I915_XD_MG_PLL_LF_GAIN);
	words->lf |= I915_XD_MG_LF_INT_COEFF(integral);
	words->lf |= I915_XD_MG_LF_PROP_COEFF(proportional);

	/* The fractional lock: the lock criteria, the threshold, the dither, the gain and its calibration with a fraction. */
	words->frac_lock = I915_XD_MG_FRAC_LOCK_TRUELOCK_32;
	words->frac_lock |= I915_XD_MG_FRAC_LOCK_EARLYLOCK_32;
	words->frac_lock |= I915_XD_MG_FRAC_LOCK_THRESHOLD(I915_XD_MG_PLL_LOCK_THRESHOLD);
	words->frac_lock |= I915_XD_MG_FRAC_LOCK_DCO_DITHER;
	words->frac_lock |= I915_XD_MG_FRAC_LOCK_FEED_FWD_GAIN(gain);
	if (remainder != 0U)
		words->frac_lock |= I915_XD_MG_FRAC_LOCK_FEED_FWD_CAL;

	/* SSC: the type, the step count and the FLL; no spread, so no step length or size. */
	words->ssc = I915_XD_MG_SSC_TYPE(I915_XD_MG_PLL_SSC_TYPE);
	words->ssc |= I915_XD_MG_SSC_STEP_NUM(I915_XD_TC_PLL_SSC_STEPS);
	words->ssc |= I915_XD_MG_SSC_FLL_ENABLE;

	/* The TDC cold start bias. */
	words->tdc = I915_XD_MG_TDC_COLDSTART;
	words->tdc |= I915_XD_MG_TDC_IREFINT_ENABLE;
	words->tdc |= I915_XD_MG_TDC_REFBIAS_PULSE(iref_pulse);
	words->tdc |= I915_XD_MG_TDC_OVERCURRENT_CORR;
	words->tdc |= I915_XD_MG_TDC_SELECT(I915_XD_MG_PLL_TDC_SELECT);

	/* The bias. */
	words->bias = I915_XD_MG_BIAS_GB_SEL(I915_XD_MG_PLL_BIAS_GB);
	words->bias |= I915_XD_MG_BIAS_INIT_DCOAMP(I915_XD_MG_PLL_BIAS_DCOAMP);
	words->bias |= I915_XD_MG_BIAS_BONUS(I915_XD_MG_PLL_BIAS_BONUS);
	words->bias |= I915_XD_MG_BIAS_CAL_ENABLE;
	words->bias |= I915_XD_MG_BIAS_CTRIM(I915_XD_MG_PLL_BIAS_CTRIM);
	words->bias |= I915_XD_MG_BIAS_VREF_RDAC(I915_XD_MG_PLL_BIAS_RDAC);
	words->bias |= I915_XD_MG_BIAS_IREF_TRIM(iref_trim);

	/* A 38.4 MHz reference keeps the bias and all of the TDC word but its cold start. */
	words->bias_mask = 0xffffffffU;
	words->tdc_mask = 0xffffffffU;
	if (ref_khz == I915_XD_DPLL_REF_38P4) {
		words->bias_mask = 0U;
		words->tdc_mask = I915_XD_MG_TDC_COLDSTART;
	}

	/* Keeps the bias and the cold start bits the reference clock uses. */
	words->bias &= words->bias_mask;
	words->tdc &= words->tdc_mask;

	/* Succeeded: the words are filled. */
	return 0;
}

/*
 * Reports the link clock an MG PLL's words give, in kHz: the feedback
 * divider (with its fraction when enabled) times the reference, over the
 * AFE's 5 and the two dividers.  0 when the high-speed divider code is
 * not one the PHY has.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_ddi_mg_pll_get_freq).
 */
uint32_t
drv_i915_xd_mg_pll_port_khz(
	uint32_t ref_khz,
	const struct i915_xd_mg_pll_words *words)
{
	uint64_t clock;
	uint32_t prediv;
	uint32_t integer;
	uint32_t fraction;
	uint32_t div1;
	uint32_t div2;

	/* The feedback divider. */
	prediv = i915_xd_field_get(I915_XD_MG_DIV1_FBPREDIV, words->div1);
	integer = i915_xd_field_get(I915_XD_MG_DIV0_FBDIV_INT, words->div0);
	fraction = 0U;
	if ((words->div0 & I915_XD_MG_DIV0_FRAC_ENABLE) != 0U)
		fraction = i915_xd_field_get(I915_XD_MG_DIV0_FBDIV_FRAC, words->div0);

	/* The dividers; a second divider of 0 divides by 1. */
	div1 = i915_xd_tc_pll_hsdiv_value(i915_xd_field_get(I915_XD_DKL_HSCLK_HSDIV, words->hsclk));
	if (div1 == 0U)
		return 0U;
	div2 = i915_xd_field_get(I915_XD_DKL_HSCLK_DSDIV, words->hsclk);
	if (div2 == 0U)
		div2 = 1U;

	/* The DCO, the fraction's share added after its own shift to keep the rounding small. */
	clock = (uint64_t)prediv * integer * ref_khz;
	clock += ((uint64_t)prediv * fraction * ref_khz) >> I915_XD_TC_PLL_FRACTION_BITS;

	/* Succeeded: the link clock. */
	return (uint32_t)(clock / (5U * div1 * div2));
}

/*
 * Turns an Ice Lake Type-C port's PLL on with its words: the power of its
 * enable word and the wait for it, the clock tops written into the fields
 * the PLL uses, the dividers, loop filter, fractional lock and SSC words
 * written whole, the bias and TDC words through their masks, a posting
 * read, the enable and the wait for the lock.  The MG PHY's registers are
 * reached directly.  Returns the waits that ran out (I915_XD_DPLL_LATE_*);
 * a PLL that is not a Type-C one is refused with every bit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_pll_power_enable, icl_pll_enable: the waits of 1 ms).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (mg_pll_enable, icl_mg_pll_write, icl_pll_power_enable, icl_pll_enable).
 */
unsigned
drv_i915_xd_mg_pll_enable(
	struct i915_xd_io *io,
	const struct i915_xd_pll_desc *pll,
	const struct i915_xd_mg_pll_words *words)
{
	uint32_t base;
	unsigned late;
	int error;

	/* Only a Type-C PLL has its words in a PHY. */
	if (pll->kind != I915_XD_PLL_TC || pll->tc == 0U)
		return I915_XD_DPLL_LATE_POWER_ON | I915_XD_DPLL_LATE_LOCK;
	base = I915_XD_MG_PHY(pll->tc - 1U);

	/* Powers the PLL and waits for the power. */
	late = 0U;
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_POWER_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_POWER_STATE, I915_XD_PLL_POWER_STATE, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_POWER_ON;

	/* The clock tops, in the fields the PLL uses. */
	(void)drv_i915_xd_io_rmw(io, base + I915_XD_MG_REFCLKIN_CTL, I915_XD_DKL_REFCLKIN_OD_2_MUX, words->refclkin);
	(void)drv_i915_xd_io_rmw(io, base + I915_XD_MG_CLKTOP2_CORECLKCTL1, I915_XD_DKL_CORECLK_A_DIVRATIO, words->coreclk);
	(void)drv_i915_xd_io_rmw(io, base + I915_XD_MG_CLKTOP2_HSCLKCTL, I915_XD_MG_PLL_HSCLK_FIELDS, words->hsclk);

	/* The dividers, the loop filter, the fractional lock and the SSC, whole. */
	drv_i915_xd_io_write(io, base + I915_XD_MG_PLL_DIV0, words->div0);
	drv_i915_xd_io_write(io, base + I915_XD_MG_PLL_DIV1, words->div1);
	drv_i915_xd_io_write(io, base + I915_XD_MG_PLL_LF, words->lf);
	drv_i915_xd_io_write(io, base + I915_XD_MG_PLL_FRAC_LOCK, words->frac_lock);
	drv_i915_xd_io_write(io, base + I915_XD_MG_PLL_SSC, words->ssc);

	/* The bias and the TDC cold start through their masks, then a posting read. */
	(void)drv_i915_xd_io_rmw(io, base + I915_XD_MG_PLL_BIAS, words->bias_mask, words->bias);
	(void)drv_i915_xd_io_rmw(io, base + I915_XD_MG_PLL_TDC_COLDST_BIAS, words->tdc_mask, words->tdc);
	drv_i915_xd_io_posting_read(io, base + I915_XD_MG_PLL_TDC_COLDST_BIAS);

	/* Enables it and waits for the lock. */
	(void)drv_i915_xd_io_rmw(io, pll->enable, 0U, I915_XD_PLL_ENABLE);
	error = drv_i915_xd_io_poll(io, pll->enable, I915_XD_PLL_LOCK, I915_XD_PLL_LOCK, I915_XD_DPLL_US, NULL);
	if (error != 0)
		late |= I915_XD_DPLL_LATE_LOCK;

	/* Succeeded: the waits that ran out. */
	return late;
}

/*
 * Reads an Ice Lake Type-C port's PLL: returns 1 when it is enabled, with
 * its words (the clock tops in the fields the PLL uses, the bias and TDC
 * words through the masks of the reference), or 0 when it is off or not
 * a Type-C PLL.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (mg_pll_get_hw_state).
 */
int
drv_i915_xd_mg_pll_read(
	struct i915_xd_io *io,
	const struct i915_xd_pll_desc *pll,
	uint32_t ref_khz,
	struct i915_xd_mg_pll_words *words)
{
	uint32_t enable;
	uint32_t base;

	/* Only a Type-C PLL has its words in a PHY. */
	if (pll->kind != I915_XD_PLL_TC || pll->tc == 0U)
		return 0;
	base = I915_XD_MG_PHY(pll->tc - 1U);

	/* A PLL that is not enabled has no state. */
	enable = drv_i915_xd_io_read(io, pll->enable);
	if ((enable & I915_XD_PLL_ENABLE) == 0U)
		return 0;

	/* The clock tops. */
	words->refclkin = drv_i915_xd_io_read(io, base + I915_XD_MG_REFCLKIN_CTL) & I915_XD_DKL_REFCLKIN_OD_2_MUX;
	words->coreclk = drv_i915_xd_io_read(io, base + I915_XD_MG_CLKTOP2_CORECLKCTL1) & I915_XD_DKL_CORECLK_A_DIVRATIO;
	words->hsclk = drv_i915_xd_io_read(io, base + I915_XD_MG_CLKTOP2_HSCLKCTL) & I915_XD_MG_PLL_HSCLK_FIELDS;

	/* The whole words. */
	words->div0 = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_DIV0);
	words->div1 = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_DIV1);
	words->lf = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_LF);
	words->frac_lock = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_FRAC_LOCK);
	words->ssc = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_SSC);

	/* The bias and the TDC cold start through the masks of the reference. */
	words->bias_mask = 0xffffffffU;
	words->tdc_mask = 0xffffffffU;
	if (ref_khz == I915_XD_DPLL_REF_38P4) {
		words->bias_mask = 0U;
		words->tdc_mask = I915_XD_MG_TDC_COLDSTART;
	}

	/* Reads the bias and the cold start bits the reference clock uses. */
	words->bias = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_BIAS) & words->bias_mask;
	words->tdc = drv_i915_xd_io_read(io, base + I915_XD_MG_PLL_TDC_COLDST_BIAS) & words->tdc_mask;

	/* Succeeded: the PLL is enabled. */
	return 1;
}

/*
 * Reports a Type-C port's DDI clock select for the PLL that drives it: its
 * own TC PLL, or the Thunderbolt PLL's output of the link rate (none for
 * a rate the Thunderbolt PLL does not put out, or a combo DPLL).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_pll_to_ddi_clk_sel).
 */
uint32_t
drv_i915_xd_tc_clock_select(
	const struct i915_xd_pll_desc *pll,
	uint32_t rate_khz)
{
	/* The port's own PLL. */
	if (pll->kind == I915_XD_PLL_TC)
		return I915_XD_DDI_CLK_SEL_TC_PLL;

	/* A combo DPLL never drives a Type-C port. */
	if (pll->kind != I915_XD_PLL_TBT)
		return I915_XD_DDI_CLK_SEL_NONE;

	/* The Thunderbolt PLL's output of the rate. */
	switch (rate_khz) {
	case 162000U:
		return I915_XD_DDI_CLK_SEL_TBT_162;
	case 270000U:
		return I915_XD_DDI_CLK_SEL_TBT_270;
	case 540000U:
		return I915_XD_DDI_CLK_SEL_TBT_540;
	case 810000U:
		return I915_XD_DDI_CLK_SEL_TBT_810;
	default:
		return I915_XD_DDI_CLK_SEL_NONE;
	}
}

/*
 * Selects the clock of a Type-C port's DDI (a select of
 * drv_i915_xd_tc_clock_select()) and then ungates it, in two writes.  tc
 * counts the Type-C ports from 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_ddi_tc_enable_clock).
 */
void
drv_i915_xd_tc_clock_map(
	struct i915_xd_io *io,
	unsigned ddi,
	unsigned tc,
	uint32_t select)
{
	/* The select, then the gate opened. */
	drv_i915_xd_io_write(io, I915_XD_DDI_CLK_SEL(ddi), i915_xd_field_put(I915_XD_DDI_CLK_SEL_FIELD, select));
	(void)drv_i915_xd_io_rmw(io, I915_XD_DPCLKA_CFGCR0, I915_XD_DPCLKA_TC_OFF(tc), 0U);
}

/*
 * Gates the clock of a Type-C port's DDI and then deselects it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_ddi_tc_disable_clock).
 */
void
drv_i915_xd_tc_clock_unmap(
	struct i915_xd_io *io,
	unsigned ddi,
	unsigned tc)
{
	/* The gate closed, then no select. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_DPCLKA_CFGCR0, 0U, I915_XD_DPCLKA_TC_OFF(tc));
	drv_i915_xd_io_write(io, I915_XD_DDI_CLK_SEL(ddi), I915_XD_DDI_CLK_SEL_NONE);
}

/*
 * Tells whether a Type-C port's DDI has a clock: a select and an open
 * gate.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_ddi_tc_is_clock_enabled).
 */
int
drv_i915_xd_tc_clock_mapped(
	struct i915_xd_io *io,
	unsigned ddi,
	unsigned tc)
{
	uint32_t word;

	/* No select: no clock. */
	word = drv_i915_xd_io_read(io, I915_XD_DDI_CLK_SEL(ddi));
	if ((word & I915_XD_DDI_CLK_SEL_FIELD) == 0U)
		return 0;

	/* A closed gate: no clock. */
	word = drv_i915_xd_io_read(io, I915_XD_DPCLKA_CFGCR0);
	if ((word & I915_XD_DPCLKA_TC_OFF(tc)) != 0U)
		return 0;

	/* Succeeded: the clock runs. */
	return 1;
}

/*
 * Finds where a combo PHY's DPLL select and clock-off bit are in the
 * platform's map: the word, the select's shift and the clock-off bit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_DPCLKA_CFGCR0_DDI_CLK_OFF, RKL_DPCLKA_CFGCR0_DDI_CLK_OFF,
 * RKL_DPCLKA_CFGCR0_DDI_CLK_SEL_SHIFT, ADLS_DPCLKA_CFGCR).
 */
static void
i915_xd_dpll_map_place(
	const struct i915_xd_platform *platform,
	unsigned phy,
	uint32_t *word,
	uint32_t *shift,
	uint32_t *off)
{
	static const uint32_t offs[] = {
		I915_XD_DPCLKA_DDI_OFF_A,
		I915_XD_DPCLKA_DDI_OFF_B,
		I915_XD_DPCLKA_DDI_OFF_C,
		I915_XD_DPCLKA_DDI_OFF_D,
		I915_XD_DPCLKA_DDI_OFF_E
	};

	/* The one word of Tiger Lake and display 13 is the default. */
	*word = I915_XD_DPCLKA_CFGCR0;
	*shift = I915_XD_DPCLKA_SEL_SHIFT(phy);
	*off = phy < sizeof(offs) / sizeof(offs[0]) ? offs[phy] : 0U;

	/* Rocket Lake moves PHY D's select and every clock-off bit. */
	if (platform->pll_map == I915_XD_PLL_MAP_CFGCR0_RKL) {
		if (phy == 3U)
			*shift = I915_XD_DPCLKA_RKL_SEL_SHIFT_D;
		*off = I915_XD_DPCLKA_RKL_DDI_OFF(phy);
		return;
	}

	/* Alder Lake-S keeps PHY D and E in a second word. */
	if (platform->pll_map == I915_XD_PLL_MAP_CFGCR_ADLS) {
		*shift = I915_XD_DPCLKA_ADLS_SEL_SHIFT(phy);
		if (phy >= 3U)
			*word = I915_XD_DPCLKA_CFGCR1_ADLS;
	}
}

/*
 * Reports a PLL's DIV0 word, which holds the AFC startup: the combo DPLLs
 * and the Thunderbolt PLL of the platforms with one map word (Tiger Lake
 * and display 13) have one; 0 elsewhere (display 11 has none).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_dpll_write, icl_pll_get_hw_state).
 */
static uint32_t
i915_xd_dpll_div0(
	const struct i915_xd_platform *platform,
	const struct i915_xd_pll_desc *pll)
{
	/* Only display 12 on, the platforms of the one map word, and not a Type-C PLL. */
	if (platform->display_ver < 12U)
		return 0U;
	if (platform->pll_map != I915_XD_PLL_MAP_CFGCR0 || pll->kind == I915_XD_PLL_TC)
		return 0U;

	/* Succeeded: the PLL's DIV0. */
	return I915_XD_DPLL_DIV0(pll->select);
}

/*
 * The register codes of the P and K dividers, and back (an unknown code
 * reads as 0).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DPLL_CFGCR1_PDIV_2, DPLL_CFGCR1_PDIV_3, DPLL_CFGCR1_PDIV_5,
 * DPLL_CFGCR1_PDIV_7, DPLL_CFGCR1_KDIV_1, DPLL_CFGCR1_KDIV_2,
 * DPLL_CFGCR1_KDIV_3).
 */
static uint32_t
i915_xd_dpll_p_code(
	uint32_t p)
{
	/* 2, 3, 5, 7 as 1, 2, 4, 8. */
	switch (p) {
	case 2U:
		return 1U;
	case 3U:
		return 2U;
	case 5U:
		return 4U;
	case 7U:
		return 8U;
	default:
		return 0U;
	}
}

/* The P divider of a register code. */
static uint32_t
i915_xd_dpll_p_value(
	uint32_t code)
{
	/* 1, 2, 4, 8 as 2, 3, 5, 7. */
	switch (code) {
	case 1U:
		return 2U;
	case 2U:
		return 3U;
	case 4U:
		return 5U;
	case 8U:
		return 7U;
	default:
		return 0U;
	}
}

/* The register code of a K divider. */
static uint32_t
i915_xd_dpll_k_code(
	uint32_t k)
{
	/* 1, 2, 3 as 1, 2, 4. */
	switch (k) {
	case 1U:
		return 1U;
	case 2U:
		return 2U;
	case 3U:
		return 4U;
	default:
		return 0U;
	}
}

/* The K divider of a register code. */
static uint32_t
i915_xd_dpll_k_value(
	uint32_t code)
{
	/* 1, 2, 4 as 1, 2, 3. */
	switch (code) {
	case 1U:
		return 1U;
	case 2U:
		return 2U;
	case 4U:
		return 3U;
	default:
		return 0U;
	}
}

/*
 * Reports the current reference divider, trim and start pulse width
 * of a Type-C PLL for a reference clock.  Returns 0, or EINVAL for a
 * reference the PHY does not take.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_calc_mg_pll_state: iref_ndiv, iref_trim and iref_pulse_w of 19.2,
 * 24 and 38.4 MHz).
 */
static int
i915_xd_tc_pll_iref(
	uint32_t ref_khz,
	uint32_t *ndiv,
	uint32_t *trim,
	uint32_t *pulse)
{
	/* Each reference has its divider ratio, trim and start pulse width. */
	switch (ref_khz) {
	case 19200U:
		*ndiv = 1U;
		*trim = 28U;
		*pulse = 1U;
		break;
	case 24000U:
		*ndiv = 1U;
		*trim = 25U;
		*pulse = 2U;
		break;
	case 38400U:
		*ndiv = 2U;
		*trim = 28U;
		*pulse = 1U;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the reference's values. */
	return 0;
}

/*
 * Finds the high-speed and second dividers of a Type-C PLL: the first
 * high-speed divider (largest first) with the first second divider
 * (largest first) that puts the DCO, five times the link clock through
 * both, at 8.1 GHz for DisplayPort or in the HDMI range.  Returns 0 with
 * the dividers and the DCO, or EINVAL when none fits.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors: the first dividers 7, 5, 3, 2, the second
 * dividers 10 to 1, the DCO of DisplayPort and the HDMI range without
 * spread spectrum).
 */
static int
i915_xd_tc_pll_dividers(
	uint32_t port_khz,
	int hdmi,
	uint32_t *div1,
	uint32_t *div2,
	uint32_t *dco_khz)
{
	static const uint8_t first[] = { 7U, 5U, 3U, 2U };
	uint64_t dco;
	uint32_t low;
	uint32_t high;
	unsigned index;
	uint32_t second;

	/* The DCO DisplayPort needs, or the HDMI range. */
	low = I915_XD_TC_PLL_DP_DCO_KHZ;
	high = I915_XD_TC_PLL_DP_DCO_KHZ;
	if (hdmi) {
		low = I915_XD_TC_PLL_HDMI_DCO_MIN_KHZ;
		high = I915_XD_TC_PLL_HDMI_DCO_MAX_KHZ;
	}

	/* Tries each pair in order; the first that fits is taken. */
	for (index = 0U; index < sizeof(first) / sizeof(first[0]); index++) {
		/* The second dividers of this high-speed divider, largest first. */
		for (second = I915_XD_TC_PLL_DSDIV_MAX; second > 0U; second--) {
			/* The DCO of the pair. */
			dco = (uint64_t)first[index] * second * port_khz * 5U;
			if (dco < low || dco > high)
				continue;

			/* Succeeded: the pair and the DCO. */
			*div1 = first[index];
			*div2 = second;
			*dco_khz = (uint32_t)dco;
			return 0;
		}
	}

	/* No pair puts the DCO in place. */
	return EINVAL;
}

/*
 * Fills the clock top words of a Type-C PLL: the reference mux, the core
 * clock divider (10 for DisplayPort and 5 for HDMI with a second divider,
 * 5 without), and the high-speed clock control (the PHY's line driver
 * select with a second divider and 0 without, the core input of HDMI,
 * the two dividers).  The clock tops are the same words in a Dekel and
 * an MG PHY.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_mg_pll_find_divisors: a_divratio, tlinedrv, inputsel,
 * MG_REFCLKIN_CTL_OD_2_MUX(1)).
 */
static void
i915_xd_tc_pll_clock_tops(
	int hdmi,
	uint32_t div1,
	uint32_t div2,
	uint32_t line_driver,
	uint32_t *refclkin,
	uint32_t *coreclk,
	uint32_t *hsclk)
{
	uint32_t divratio;
	uint32_t linedrv;
	uint32_t inputsel;

	/* The core divider and the line driver depend on the second divider. */
	divratio = 5U;
	linedrv = 0U;
	if (div2 >= 2U) {
		divratio = hdmi ? 5U : 10U;
		linedrv = line_driver;
	}

	/* HDMI takes the other core input. */
	inputsel = hdmi ? 1U : 0U;

	/* The words. */
	*refclkin = i915_xd_field_put(I915_XD_DKL_REFCLKIN_OD_2_MUX, 1U);
	*coreclk = i915_xd_field_put(I915_XD_DKL_CORECLK_A_DIVRATIO, divratio);
	*hsclk = i915_xd_field_put(I915_XD_DKL_HSCLK_TLINEDRV, linedrv);
	*hsclk |= i915_xd_field_put(I915_XD_DKL_HSCLK_CORE_INPUTSEL, inputsel);
	*hsclk |= i915_xd_field_put(I915_XD_DKL_HSCLK_HSDIV, i915_xd_tc_pll_hsdiv_code(div1));
	*hsclk |= i915_xd_field_put(I915_XD_DKL_HSCLK_DSDIV, div2);
}

/* The high-speed divider code of a divider (2, 3, 5, 7); any other is taken as 2. */
static uint32_t
i915_xd_tc_pll_hsdiv_code(
	uint32_t divider)
{
	/* The code of each divider. */
	switch (divider) {
	case 3U:
		return I915_XD_DKL_HSCLK_HSDIV_3;
	case 5U:
		return I915_XD_DKL_HSCLK_HSDIV_5;
	case 7U:
		return I915_XD_DKL_HSCLK_HSDIV_7;
	default:
		return I915_XD_DKL_HSCLK_HSDIV_2;
	}
}

/* The divider of a high-speed divider code. */
static uint32_t
i915_xd_tc_pll_hsdiv_value(
	uint32_t code)
{
	/* The divider of each code. */
	switch (code) {
	case I915_XD_DKL_HSCLK_HSDIV_2:
		return 2U;
	case I915_XD_DKL_HSCLK_HSDIV_3:
		return 3U;
	case I915_XD_DKL_HSCLK_HSDIV_5:
		return 5U;
	case I915_XD_DKL_HSCLK_HSDIV_7:
		return 7U;
	default:
		return 0U;
	}
}

/*
 * Writes one word of a Type-C PLL into its PHY register: the register is
 * read, the PLL's fields replaced, the rest kept.
 */
static void
i915_xd_tc_pll_put(
	struct i915_xd_dkl *dkl,
	unsigned tc,
	uint32_t phy_offset,
	uint32_t fields,
	uint32_t value)
{
	uint32_t word;

	/* Reads the register, replaces the fields, writes it back. */
	word = drv_i915_xd_dkl_read(dkl, tc, phy_offset);
	word &= ~fields;
	word |= value;
	drv_i915_xd_dkl_write(dkl, tc, phy_offset, word);
}

/*
 * Tells whether a DCO frequency is within the allowed deviation of a
 * central frequency, and how far from it it is in ten-thousandths of it.
 */
static int
i915_xd9_wrpll_deviation(
	uint64_t dco_hz,
	uint64_t central_hz,
	uint64_t *deviation)
{
	/* Above the centre: up to 1 %. */
	if (dco_hz >= central_hz) {
		*deviation = (10000U * (dco_hz - central_hz)) / central_hz;
		if (*deviation >= I915_XD9_DCO_ABOVE)
			return 0;

		/* Succeeded: close enough above. */
		return 1;
	}

	/* Below the centre: up to 6 %. */
	*deviation = (10000U * (central_hz - dco_hz)) / central_hz;
	if (*deviation >= I915_XD9_DCO_BELOW)
		return 0;

	/* Succeeded: close enough below. */
	return 1;
}
