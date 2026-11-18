/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display clocks of the display 12 and 13 code (xd-clock.c): the CD
 * clock and the combo port PLLs.
 *
 * The CD clock runs the display engine.  It comes from a PLL on the
 * reference clock (its ratio sets the PLL's rate) through a CD2X divider,
 * or, with the PLL off, it is the bypass clock, half the reference.  The
 * platform's table lists the CD clocks with their ratios; each CD clock
 * needs a voltage level, which the firmware is told after the change, as
 * it is asked before it whether the change may go ahead.
 *
 * At the display's start a CD clock the firmware left that is one of the
 * table's is kept; any other, a PLL that is off, or a bypass clock is
 * replaced by the table's lowest.  The platforms that can change the
 * PLL's ratio while it runs (display 13) crawl to a new one; the others,
 * and any change from a PLL that is off or in an unknown state, turn the
 * PLL off and on again.
 *
 * The port PLLs give the DDIs their link clocks.  A combo DPLL is
 * programmed with a DCO (an integer and a 15-bit fraction of the
 * reference) and three dividers: for a DisplayPort link rate from the
 * table, for an HDMI clock by choosing the dividers that put the DCO
 * nearest the middle of its range.  With a 38.4 MHz reference the PLL
 * divides the reference by 2, so the 19.2 MHz words serve, and some
 * platforms take the fraction halved.  The PLL is powered, written,
 * enabled and waited on through its enable word; a DDI takes its clock
 * from a combo DPLL through the platform's DPCLKA select, which is
 * written before the DDI's clock is ungated, in a separate write.
 *
 * A Type-C port's PLL is in its Dekel PHY.  Its feedback divider (an
 * integer and a 22-bit fraction of twice the reference) puts its DCO at
 * 8.1 GHz for every DisplayPort link rate, or in a range for an HDMI
 * clock; a high-speed divider (2, 3, 5 or 7) and a second divider bring
 * the DCO down to the link clock.  Its words are written through the
 * PHY's window (xd-tc.c) between the power and the enable of the PLL's
 * enable word, which stays in the display's registers.  A Type-C port's
 * DDI selects its own TC PLL, or one of the Thunderbolt PLL's link rates,
 * in its own clock select, and its clock is ungated in DPCLKA_CFGCR0.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_CLOCK_H
#define DRIVERS_GPU_I915_DISPLAY_XD_CLOCK_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"
#include "xd-tc.h"

/* A PLL rate that is not known, which forces the PLL off and on again. */
#define I915_XD_CDCLK_VCO_UNKNOWN	0xffffffffU

/*
 * One state of the CD clock, in kHz: the reference, the PLL's rate (0
 * when it is off), the CD clock (0 when it must be set again), the bypass
 * clock, and the voltage level the CD clock needs.
 */
struct i915_xd_cdclk {
	uint32_t ref_khz;
	uint32_t vco_khz;
	uint32_t cdclk_khz;
	uint32_t bypass_khz;
	uint8_t voltage;
};

/*
 * What a change of the CD clock met, for the owner's log and
 * diagnostics: the state found before the start's check and whether the
 * start kept it or had to replace it, the state asked for, the
 * firmware's answers before (prepare) and after (voltage) the change,
 * whether the PLL and the control word were written, and the waits for
 * the PLL that ran out (its lock going, its lock coming, the crawl's
 * acknowledge).
 */
struct i915_xd_cdclk_report {
	struct i915_xd_cdclk before;
	int kept;
	int replaced;
	struct i915_xd_cdclk requested;
	int prepare_error;
	int voltage_error;
	int programmed;
	int unlock_late;
	int lock_late;
	int crawl_late;
};

/* A DPLL write that has no VBT override of the AFC startup. */
#define I915_XD_DPLL_AFC_NONE		(-1)

/*
 * The waits of a port PLL that ran out, as bits: its power coming, its
 * lock coming, its lock going, its power going.
 */
#define I915_XD_DPLL_LATE_POWER_ON	0x1U
#define I915_XD_DPLL_LATE_LOCK		0x2U
#define I915_XD_DPLL_LATE_UNLOCK	0x4U
#define I915_XD_DPLL_LATE_POWER_OFF	0x8U

/*
 * The words a combo DPLL or the Thunderbolt PLL is programmed with: the
 * DCO (CFGCR0), the dividers (CFGCR1), and the AFC startup field of DIV0
 * with whether it is written at all (only with the VBT's override).
 */
struct i915_xd_dpll_words {
	uint32_t cfgcr0;
	uint32_t cfgcr1;
	uint32_t div0;
	int div0_set;
};

/*
 * The words of a Type-C port's PLL in its Dekel PHY, one per register,
 * and whether the AFC startup field of DIV0 is written at all (only
 * with the VBT's override).
 */
struct i915_xd_tc_pll_words {
	uint32_t refclkin;
	uint32_t coreclk;
	uint32_t hsclk;
	uint32_t div0;
	uint32_t div1;
	uint32_t ssc;
	uint32_t bias;
	uint32_t tdc;
	int afc_set;
};

/*
 * The words of an Ice Lake Type-C port's PLL in its MG PHY, one per
 * register, with the masks of the bias and the TDC cold start words
 * (only some of their fields are written on a 38.4 MHz reference).
 */
struct i915_xd_mg_pll_words {
	uint32_t refclkin;
	uint32_t coreclk;
	uint32_t hsclk;
	uint32_t div0;
	uint32_t div1;
	uint32_t lf;
	uint32_t frac_lock;
	uint32_t ssc;
	uint32_t bias;
	uint32_t bias_mask;
	uint32_t tdc;
	uint32_t tdc_mask;
};

/*
 * The DPLL words of a display 9 output: the DPLL's six-bit field of
 * DPLL_CTRL1 (the caller shifts it to the DPLL), and in HDMI mode the
 * DPLL's two configuration words; with the port clock they give, in kHz
 * (the TMDS clock for HDMI).
 */
struct i915_xd9_dpll_words {
	uint32_t ctrl1;
	uint32_t cfgcr1;
	uint32_t cfgcr2;
	uint32_t port_khz;
};

uint32_t drv_i915_xd9_cdclk_khz(uint32_t lcpll1_ctl, uint32_t dpll_ctrl1, uint32_t cdclk_ctl);
int drv_i915_xd9_dpll_dp_words(uint32_t port_khz, struct i915_xd9_dpll_words *words);
uint32_t drv_i915_xd9_dpll_rate_port_khz(uint32_t code);
int drv_i915_xd9_dpll_hdmi_words(uint32_t pixel_khz, struct i915_xd9_dpll_words *words);
int drv_i915_xd9_dpll_enable(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned id, const struct i915_xd9_dpll_words *words);
void drv_i915_xd9_dpll_disable(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned id);
void drv_i915_xd9_ddi_clock(struct i915_xd_io *io, unsigned ddi, unsigned dpll, int on);
void drv_i915_xd_cdclk_read(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_cdclk *state);
uint8_t drv_i915_xd_cdclk_voltage(const struct i915_xd_platform *platform, uint32_t cdclk_khz);
uint32_t drv_i915_xd_cdclk_lowest(const struct i915_xd_platform *platform, uint32_t ref_khz, uint32_t min_khz);
uint32_t drv_i915_xd_cdclk_vco(const struct i915_xd_platform *platform, const struct i915_xd_cdclk *state, uint32_t cdclk_khz);
int drv_i915_xd_cdclk_legal(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_cdclk *state);
void drv_i915_xd_cdclk_set(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_cdclk *state, const struct i915_xd_cdclk *target, struct i915_xd_cdclk_report *report);
void drv_i915_xd_cdclk_init(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_cdclk *state, struct i915_xd_cdclk_report *report);
void drv_i915_xd_cdclk_uninit(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_cdclk *state, struct i915_xd_cdclk_report *report);
uint32_t drv_i915_xd_cdclk_max(const struct i915_xd_cdclk *state);
int drv_i915_xd_dpll_dp_words(const struct i915_xd_platform *platform, uint32_t ref_khz, uint32_t link_khz, int afc, struct i915_xd_dpll_words *words);
int drv_i915_xd_dpll_hdmi_words(const struct i915_xd_platform *platform, uint32_t ref_khz, uint32_t tmds_khz, int afc, struct i915_xd_dpll_words *words);
void drv_i915_xd_dpll_tbt_words(const struct i915_xd_platform *platform, uint32_t ref_khz, int afc, struct i915_xd_dpll_words *words);
uint32_t drv_i915_xd_dpll_port_khz(const struct i915_xd_platform *platform, uint32_t ref_khz, const struct i915_xd_dpll_words *words);
const struct i915_xd_pll_desc *drv_i915_xd_dpll_desc(const struct i915_xd_platform *platform, unsigned select);
unsigned drv_i915_xd_dpll_enable(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_pll_desc *pll, const struct i915_xd_dpll_words *words);
unsigned drv_i915_xd_dpll_disable(struct i915_xd_io *io, const struct i915_xd_pll_desc *pll);
int drv_i915_xd_dpll_read(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_pll_desc *pll, int afc, struct i915_xd_dpll_words *words);
void drv_i915_xd_dpll_map(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned phy, unsigned select);
void drv_i915_xd_dpll_unmap(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned phy);
int drv_i915_xd_dpll_mapped(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned phy, unsigned *select);
int drv_i915_xd_tc_pll_words(uint32_t ref_khz, uint32_t port_khz, int hdmi, int afc, struct i915_xd_tc_pll_words *words);
uint32_t drv_i915_xd_tc_pll_port_khz(uint32_t ref_khz, const struct i915_xd_tc_pll_words *words);
unsigned drv_i915_xd_tc_pll_enable(struct i915_xd_io *io, struct i915_xd_dkl *dkl, const struct i915_xd_pll_desc *pll, const struct i915_xd_tc_pll_words *words);
int drv_i915_xd_tc_pll_read(struct i915_xd_io *io, struct i915_xd_dkl *dkl, const struct i915_xd_pll_desc *pll, int afc, struct i915_xd_tc_pll_words *words);
int drv_i915_xd_mg_pll_words(uint32_t ref_khz, uint32_t port_khz, int hdmi, struct i915_xd_mg_pll_words *words);
uint32_t drv_i915_xd_mg_pll_port_khz(uint32_t ref_khz, const struct i915_xd_mg_pll_words *words);
unsigned drv_i915_xd_mg_pll_enable(struct i915_xd_io *io, const struct i915_xd_pll_desc *pll, const struct i915_xd_mg_pll_words *words);
int drv_i915_xd_mg_pll_read(struct i915_xd_io *io, const struct i915_xd_pll_desc *pll, uint32_t ref_khz, struct i915_xd_mg_pll_words *words);
uint32_t drv_i915_xd_tc_clock_select(const struct i915_xd_pll_desc *pll, uint32_t rate_khz);
void drv_i915_xd_tc_clock_map(struct i915_xd_io *io, unsigned ddi, unsigned tc, uint32_t select);
void drv_i915_xd_tc_clock_unmap(struct i915_xd_io *io, unsigned ddi, unsigned tc);
int drv_i915_xd_tc_clock_mapped(struct i915_xd_io *io, unsigned ddi, unsigned tc);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_CLOCK_H */
