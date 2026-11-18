/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's CD clock (see clock.h).
 *
 * The CD clock state the display core and the resident run read, kept
 * over the xd clock (xd-clock.c): its read, its start and stop with the
 * PCODE handshake around the PLL and CDCLK_CTL writes, a change of
 * frequency (a crawl on Alder Lake-P), and the highest step.
 */

#include "internal.h"
#include "clock.h"
#include "xd-clock.h"
#include "xd.h"
#include <kern/kcrt.h>

#include "../mmio.h"
#include "../power.h"
#include "../sync.h"

#include <kern/klog.h>

#include <uapi/errno.h>

static void i915_cdclk_to_xd(const struct i915_cdclk_config *cfg, struct i915_xd_cdclk *state);
static void i915_cdclk_from_xd(const struct i915_xd_cdclk *state, struct i915_cdclk_config *cfg);
static void i915_cdclk_report(struct i915_cdclk_dev *cd, const struct i915_xd_cdclk_report *report);

/*
 * Prepares the CDCLK state of a display over its xd description: the
 * display version, and whether the PLL may crawl to a new ratio.
 */
void
drv_i915_cdclk_prepare(
	struct i915_cdclk_dev *cd,
	struct i915_xd *xd)
{
	/* Records the xd state the CD clock is read and set through. */
	cd->xd = xd;

	/* A display without a description has no CD clock to bring up. */
	if (xd == NULL || !xd->chosen)
		return;

	/* The display version and the PLL crawl of the platform; none squashes. */
	cd->display_ver = xd->choice.platform->display_ver;
	cd->has_cdclk_crawl = xd->choice.platform->cdclk_crawl;
	cd->has_cdclk_squash = 0;
}

/*
 * Reads the CDCLK state out of the hardware; the voltage level is the one
 * the frequency needs.
 */
void
drv_i915_cdclk_read(
	struct i915_cdclk_dev *cd,
	struct i915_cdclk_config *cfg)
{
	struct i915_xd_cdclk state;

	/* Reads it through the xd clock. */
	drv_i915_xd_cdclk_read(&cd->xd->io, cd->xd->choice.platform, &state);
	i915_cdclk_from_xd(&state, cfg);
}

/*
 * Reads the current CDCLK state into the device.
 */
void
drv_i915_cdclk_refresh(
	struct i915_cdclk_dev *cd)
{
	/* The device's state is the hardware's. */
	drv_i915_cdclk_read(cd, &cd->hw);
}

/*
 * Brings the CDCLK up: keeps a legal state the firmware left, and
 * otherwise sets the table's lowest.
 */
void
drv_i915_cdclk_start(
	struct i915_cdclk_dev *cd)
{
	struct i915_xd_cdclk state;
	struct i915_xd_cdclk_report report;

	/* Brings it up through the xd clock and keeps what it met. */
	drv_i915_xd_cdclk_init(&cd->xd->io, cd->xd->choice.platform, &state, &report);
	i915_cdclk_from_xd(&state, &cd->hw);
	i915_cdclk_report(cd, &report);
}

/*
 * Takes the CDCLK down for a suspend: the bypass frequency with the PLL
 * off.  The resume's drv_i915_cdclk_start() finds the PLL off and sets the
 * CDCLK again.
 */
void
drv_i915_cdclk_stop(
	struct i915_cdclk_dev *cd)
{
	struct i915_xd_cdclk state;
	struct i915_xd_cdclk_report report;

	/* Takes it down through the xd clock and keeps what it met. */
	i915_cdclk_to_xd(&cd->hw, &state);
	drv_i915_xd_cdclk_uninit(&cd->xd->io, cd->xd->choice.platform, &state, &report);
	i915_cdclk_from_xd(&state, &cd->hw);
	i915_cdclk_report(cd, &report);
}

/*
 * Reports the highest CDCLK of the platform.
 */
uint32_t
drv_i915_cdclk_max(
	const struct i915_cdclk_dev *cd)
{
	struct i915_xd_cdclk state;
	uint32_t max;

	/* Asks the xd clock for the reference. */
	i915_cdclk_to_xd(&cd->hw, &state);
	max = drv_i915_xd_cdclk_max(&state);

	/* Succeeded: the highest CDCLK. */
	return max;
}

/* Copies a CDCLK state of the older code into the xd clock's form. */
static void
i915_cdclk_to_xd(
	const struct i915_cdclk_config *cfg,
	struct i915_xd_cdclk *state)
{
	/* Field by field, in kHz. */
	state->ref_khz = cfg->ref;
	state->vco_khz = cfg->vco;
	state->cdclk_khz = cfg->cdclk;
	state->bypass_khz = cfg->bypass;
	state->voltage = cfg->voltage_level;
}

/* Copies a CDCLK state of the xd clock into the older code's form. */
static void
i915_cdclk_from_xd(
	const struct i915_xd_cdclk *state,
	struct i915_cdclk_config *cfg)
{
	/* Field by field, in kHz. */
	cfg->ref = state->ref_khz;
	cfg->vco = state->vco_khz;
	cfg->cdclk = state->cdclk_khz;
	cfg->bypass = state->bypass_khz;
	cfg->voltage_level = state->voltage;
}

/*
 * Logs what a CDCLK change met and copies it into the device's
 * diagnostics: the state found and whether it was kept, the state asked
 * for, the firmware's answers, and whether the PLL and CDCLK_CTL were
 * written.
 */
static void
i915_cdclk_report(
	struct i915_cdclk_dev *cd,
	const struct i915_xd_cdclk_report *report)
{
	/* The state the start found, kept or replaced. */
	if (report->kept || report->replaced) {
		i915_cdclk_from_xd(&report->before, &cd->diag_observed_before);
		cd->diag_sanitized = cd->diag_observed_before;
		cd->diag_no_change = report->kept;
	}

	/* Logs a firmware state that was not one of the table's. */
	if (report->replaced) {
		kern_logf("i915: display: CD clock: the firmware's state is not one of the table's: replaced\n");
		cd->diag_sanitized.cdclk = 0u;
		cd->diag_sanitized.vco = I915_XD_CDCLK_VCO_UNKNOWN;
	}

	/* A start that kept the state asked for nothing. */
	if (report->kept)
		return;

	/* The state asked for and the firmware's answers. */
	i915_cdclk_from_xd(&report->requested, &cd->diag_requested);
	cd->diag_prepare_status = report->prepare_error;
	cd->diag_notify_status = report->voltage_error;
	cd->diag_hw_sequence_reached = report->programmed;
	if (report->prepare_error != 0)
		kern_logf("i915: display: CD clock: the firmware refused the change to %u kHz (%d)\n", report->requested.cdclk_khz, report->prepare_error);
	if (report->voltage_error != 0)
		kern_logf("i915: display: CD clock: the firmware refused the voltage of %u kHz (%d)\n", report->requested.cdclk_khz, report->voltage_error);

	/* The PLL waits that ran out. */
	if (report->unlock_late)
		kern_logf("i915: display: CD clock: the PLL did not unlock in time\n");
	if (report->lock_late)
		kern_logf("i915: display: CD clock: the PLL did not lock in time\n");
	if (report->crawl_late)
		kern_logf("i915: display: CD clock: the PLL did not acknowledge the crawl in time\n");
}
