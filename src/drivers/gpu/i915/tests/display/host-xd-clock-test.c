/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd CD clock (display/xd-clock.c) on the register
 * model (xd-fake-regs.c), whose CD clock PLL locks when it is enabled and
 * acknowledges a crawl, and whose firmware mailbox answers the CD clock
 * requests: the read of the clock the firmware left, the check that keeps
 * it, the start that replaces it, a crawl on Alder Lake-P and a relock on
 * Tiger Lake, the stop before a suspend, the firmware's refusals, and the
 * voltage levels and the highest clock.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The GPUs of the Latitude 5330 (Alder Lake-P) and of a Tiger Lake laptop, with their revisions. */
#define I915_XD_CLOCK_TEST_ADLP		0x46a6U
#define I915_XD_CLOCK_TEST_ADLP_REVID	0x0cU
#define I915_XD_CLOCK_TEST_TGL		0x9a49U
#define I915_XD_CLOCK_TEST_TGL_REVID	0x01U

/*
 * The CD clock the 5330's firmware leaves: a 38.4 MHz reference, the PLL
 * at ratio 10 (384 MHz), divided by 1 to 192 MHz with no pipe to wait
 * for; and the decimal field of 192 MHz.
 */
#define I915_XD_CLOCK_TEST_DSSM		0x40000000U
#define I915_XD_CLOCK_TEST_PLL		0xc000000aU
#define I915_XD_CLOCK_TEST_CTL		0x0038017eU

/* The most writes one check reads back. */
#define I915_XD_CLOCK_TEST_WRITES	64U

/*
 * What the model's firmware answers: whether it is ready for a change,
 * and whether it refuses the voltage level; and how many requests it saw.
 */
struct i915_xd_clock_test_firmware {
	int ready;
	int refuse_voltage;
	unsigned prepares;
	unsigned voltages;
	uint32_t last_voltage;
};

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_clock_test_io;

/* The model's firmware. */
static struct i915_xd_clock_test_firmware i915_xd_clock_test_firmware;

static const struct i915_xd_platform *i915_xd_clock_test_setup(uint16_t device, uint8_t revid);
static void i915_xd_clock_test_mailbox(uint32_t offset, uint32_t value, void *context);
static unsigned i915_xd_clock_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static void i915_xd_clock_test_read(void);
static void i915_xd_clock_test_init(void);
static void i915_xd_clock_test_changes(void);
static void i915_xd_clock_test_refusals(void);
static void i915_xd_clock_test_levels(void);

/*
 * Runs every check of the xd CD clock and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_clock_test_read();
	i915_xd_clock_test_init();
	i915_xd_clock_test_changes();
	i915_xd_clock_test_refusals();
	i915_xd_clock_test_levels();

	/* Reports the result. */
	failures = i915_host_report("host_xd_clock_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model with the firmware's CD clock, a PLL that locks
 * when enabled and acknowledges a crawl request, and a ready firmware;
 * returns the description of a device.
 */
static const struct i915_xd_platform *
i915_xd_clock_test_setup(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_clock_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_clock_test_io, &hooks);

	/* The firmware's CD clock; the PLL's lock follows its enable, the acknowledge its request. */
	i915_xd_fake_set(I915_XD_DSSM, I915_XD_CLOCK_TEST_DSSM);
	i915_xd_fake_set(I915_XD_CDCLK_PLL_ENABLE, I915_XD_CLOCK_TEST_PLL);
	i915_xd_fake_set(I915_XD_CDCLK_CTL, I915_XD_CLOCK_TEST_CTL);
	i915_xd_fake_rule_mirror(I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_ON, I915_XD_CDCLK_PLL_ENABLE, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_CDCLK_PLL_ENABLE, I915_XD_CDCLK_PLL_FREQ_REQUEST, I915_XD_CDCLK_PLL_ENABLE, -1, 0U);

	/* A ready firmware that accepts the voltage level. */
	memset(&i915_xd_clock_test_firmware, 0, sizeof(i915_xd_clock_test_firmware));
	i915_xd_clock_test_firmware.ready = 1;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_clock_test_mailbox, &i915_xd_clock_test_firmware);

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/*
 * Answers a CD clock request of the mailbox: a prepare with ready or not,
 * a voltage level accepted or refused; the busy bit and the command go,
 * the status stays.
 */
static void
i915_xd_clock_test_mailbox(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_clock_test_firmware *firmware;
	uint32_t data;
	uint32_t status;

	UNUSED_PARAMETER(offset);

	/* Only a command handed over is answered. */
	firmware = context;
	if ((value & I915_XD_GTDRIVER_MAILBOX_BUSY) == 0U)
		return;

	/* The CD clock command: a prepare, or a voltage level. */
	status = I915_XD_GTDRIVER_STATUS_OK;
	data = i915_xd_fake_get(I915_XD_GTDRIVER_DATA0);
	if ((value & 0xffU) == I915_XD_MAILBOX_CDCLK_CONTROL) {
		if (data == I915_XD_MAILBOX_CDCLK_PREPARE) {
			firmware->prepares++;
			i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, 0U);
			if (firmware->ready)
				i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, I915_XD_MAILBOX_CDCLK_READY);
		} else {
			firmware->voltages++;
			firmware->last_voltage = data;
			if (firmware->refuse_voltage)
				status = I915_XD_GTDRIVER_STATUS_REJECTED;
		}
	}

	/* The answer: no busy bit, the status. */
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, status);
}

/* Copies the CD clock writes the window recorded (not the mailbox's), oldest first; reports how many. */
static unsigned
i915_xd_clock_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes outside the mailbox. */
	count = drv_i915_xd_io_records(&i915_xd_clock_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count; index++) {
		/* Only writes, and not the mailbox's, are kept. */
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		if (records[index].offset >= I915_XD_GTDRIVER_MAILBOX && records[index].offset <= I915_XD_GTDRIVER_DATA1)
			continue;
		if (kept >= max)
			break;

		/* Keeps the write. */
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* Checks the read of the CD clock and the check that keeps it. */
static void
i915_xd_clock_test_read(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_cdclk state;
	int legal;

	/* The 5330's firmware clock reads as 192 MHz on a 384 MHz PLL, voltage level 0, and may be kept. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	legal = drv_i915_xd_cdclk_legal(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(state.ref_khz == 38400U && state.vco_khz == 384000U && state.cdclk_khz == 192000U &&
			state.bypass_khz == 19200U && state.voltage == 0U && legal == 1,
			"clock: the 5330's firmware clock reads as 192 MHz and may be kept");

	/* The pipe the last change waited for does not matter. */
	i915_xd_fake_set(I915_XD_CDCLK_CTL, (I915_XD_CLOCK_TEST_CTL & ~I915_XD_CDCLK_CD2X_PIPE_FIELD) | I915_XD_CDCLK_CD2X_PIPE(1U));
	legal = drv_i915_xd_cdclk_legal(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(legal == 1, "clock: a change that waited for pipe B is still kept");

	/* A wrong decimal field is not kept. */
	i915_xd_fake_set(I915_XD_CDCLK_CTL, I915_XD_CLOCK_TEST_CTL + 1U);
	legal = drv_i915_xd_cdclk_legal(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(legal == 0, "clock: a wrong decimal frequency is replaced");

	/* A divider of 1.5 reads as 179.2 MHz on a 537.6 MHz PLL. */
	i915_xd_fake_set(I915_XD_CDCLK_PLL_ENABLE, 0xc000000eU);
	i915_xd_fake_set(I915_XD_CDCLK_CTL, 0x00780164U);
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	legal = drv_i915_xd_cdclk_legal(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(state.vco_khz == 537600U && state.cdclk_khz == 179200U && legal == 1,
			"clock: the 1.5 divider reads as 179.2 MHz, a table clock");

	/* A PLL that is on but not locked counts as off: the bypass clock, not kept. */
	i915_xd_fake_set(I915_XD_CDCLK_PLL_ENABLE, 0x8000000aU);
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	legal = drv_i915_xd_cdclk_legal(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(state.vco_khz == 0U && state.cdclk_khz == 19200U && legal == 0,
			"clock: an unlocked PLL reads as the bypass clock and is replaced");

	/* A 24 MHz reference. */
	i915_xd_fake_set(I915_XD_DSSM, 0U);
	i915_xd_fake_set(I915_XD_CDCLK_PLL_ENABLE, 0xc0000010U);
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	i915_host_check(state.ref_khz == 24000U && state.vco_khz == 384000U && state.bypass_khz == 12000U,
			"clock: a 24 MHz reference");
}

/* Checks the start: the firmware's clock kept, or replaced by the table's lowest. */
static void
i915_xd_clock_test_init(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_cdclk state;
	struct i915_xd_cdclk_report report;
	uint32_t offsets[I915_XD_CLOCK_TEST_WRITES];
	uint32_t values[I915_XD_CLOCK_TEST_WRITES];
	unsigned count;

	/* A clock that may be kept writes nothing and asks the firmware nothing. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	drv_i915_xd_cdclk_init(&i915_xd_clock_test_io, platform, &state, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(report.kept == 1 && report.replaced == 0 && count == 0U && i915_xd_clock_test_firmware.prepares == 0U &&
			state.cdclk_khz == 192000U,
			"clock: the start keeps the firmware's 192 MHz");

	/* One that may not be kept: the PLL off and on at 537.6 MHz, the control word for 179.2 MHz. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	i915_xd_fake_set(I915_XD_CDCLK_CTL, I915_XD_CLOCK_TEST_CTL + 1U);
	drv_i915_xd_cdclk_init(&i915_xd_clock_test_io, platform, &state, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(report.replaced == 1 && report.before.cdclk_khz == 192000U && count == 4U &&
			offsets[0] == I915_XD_CDCLK_PLL_ENABLE && values[0] == 0x4000000aU &&
			offsets[1] == I915_XD_CDCLK_PLL_ENABLE && values[1] == 0x0000000eU &&
			offsets[2] == I915_XD_CDCLK_PLL_ENABLE && values[2] == 0x8000000eU &&
			offsets[3] == I915_XD_CDCLK_CTL && values[3] == 0x00780164U,
			"clock: the start replaces a bad clock by relocking the PLL at the table's lowest");
	i915_host_check(i915_xd_clock_test_firmware.prepares == 1U && i915_xd_clock_test_firmware.voltages == 1U &&
			i915_xd_clock_test_firmware.last_voltage == 0U &&
			state.cdclk_khz == 179200U && state.vco_khz == 537600U && report.programmed == 1,
			"clock: the firmware is asked before and told the voltage level after");
}

/* Checks a crawl on Alder Lake-P, a relock on Tiger Lake, and the stop before a suspend. */
static void
i915_xd_clock_test_changes(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_cdclk state;
	struct i915_xd_cdclk target;
	struct i915_xd_cdclk_report report;
	uint32_t offsets[I915_XD_CLOCK_TEST_WRITES];
	uint32_t values[I915_XD_CLOCK_TEST_WRITES];
	unsigned count;

	/* Alder Lake-P crawls from 384 to 614.4 MHz for 307.2 MHz. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	target = state;
	target.cdclk_khz = 307200U;
	target.vco_khz = 614400U;
	target.voltage = 0U;
	memset(&report, 0, sizeof(report));
	drv_i915_xd_cdclk_set(&i915_xd_clock_test_io, platform, &state, &target, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(count == 4U &&
			values[0] == 0x80000010U && values[1] == 0x80800010U && values[2] == 0x80000010U &&
			offsets[3] == I915_XD_CDCLK_CTL && values[3] == 0x00380264U &&
			state.cdclk_khz == 307200U && report.crawl_late == 0,
			"clock: Alder Lake-P crawls the running PLL to the new ratio");

	/* Tiger Lake relocks for the same change. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_TGL, I915_XD_CLOCK_TEST_TGL_REVID);
	if (platform == NULL)
		return;
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	memset(&report, 0, sizeof(report));
	drv_i915_xd_cdclk_set(&i915_xd_clock_test_io, platform, &state, &target, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(count == 4U &&
			values[0] == 0x4000000aU && values[1] == 0x00000010U && values[2] == 0x80000010U &&
			values[3] == 0x00380264U && state.vco_khz == 614400U,
			"clock: Tiger Lake turns the PLL off and on at the new ratio");

	/* The stop before a suspend: the PLL off, the bypass clock's control word. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	drv_i915_xd_cdclk_uninit(&i915_xd_clock_test_io, platform, &state, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(count == 2U && values[0] == 0x4000000aU && offsets[1] == I915_XD_CDCLK_CTL && values[1] == 0x00380024U &&
			state.vco_khz == 0U && state.cdclk_khz == 19200U,
			"clock: the stop turns the PLL off for the bypass clock");
}

/* Checks the firmware's refusals. */
static void
i915_xd_clock_test_refusals(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_cdclk state;
	struct i915_xd_cdclk target;
	struct i915_xd_cdclk_report report;
	uint32_t offsets[I915_XD_CLOCK_TEST_WRITES];
	uint32_t values[I915_XD_CLOCK_TEST_WRITES];
	unsigned count;

	/* A firmware that never answers ready: asked again and again for 53 ms, nothing written. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_clock_test_firmware.ready = 0;
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	target = state;
	target.cdclk_khz = 307200U;
	target.vco_khz = 614400U;
	memset(&report, 0, sizeof(report));
	drv_i915_xd_cdclk_set(&i915_xd_clock_test_io, platform, &state, &target, &report);
	count = i915_xd_clock_test_writes(offsets, values, I915_XD_CLOCK_TEST_WRITES);
	i915_host_check(report.prepare_error == ETIMEDOUT && report.programmed == 0 && count == 0U &&
			i915_xd_clock_test_firmware.prepares == 5301U && state.cdclk_khz == 192000U,
			"clock: a firmware never ready is asked for 53 ms and nothing changes");

	/* A refused voltage level leaves the hardware changed and the clock as it was, the PLL's rate excepted. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_ADLP, I915_XD_CLOCK_TEST_ADLP_REVID);
	i915_xd_clock_test_firmware.refuse_voltage = 1;
	drv_i915_xd_cdclk_read(&i915_xd_clock_test_io, platform, &state);
	memset(&report, 0, sizeof(report));
	drv_i915_xd_cdclk_set(&i915_xd_clock_test_io, platform, &state, &target, &report);
	i915_host_check(report.voltage_error != 0 && report.programmed == 1 && state.cdclk_khz == 192000U && state.vco_khz == 614400U &&
			i915_xd_fake_get(I915_XD_CDCLK_CTL) == 0x00380264U,
			"clock: a refused voltage level leaves the hardware changed and the clock unread");
}

/* Checks the voltage levels and the highest clock. */
static void
i915_xd_clock_test_levels(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_cdclk state;

	/* Tiger Lake's levels: up to 312, 326.4, 556.8, 652.8 MHz, and above the top level. */
	platform = i915_xd_clock_test_setup(I915_XD_CLOCK_TEST_TGL, I915_XD_CLOCK_TEST_TGL_REVID);
	if (platform == NULL)
		return;
	i915_host_check(drv_i915_xd_cdclk_voltage(platform, 312000U) == 0U && drv_i915_xd_cdclk_voltage(platform, 326400U) == 1U &&
			drv_i915_xd_cdclk_voltage(platform, 556800U) == 2U && drv_i915_xd_cdclk_voltage(platform, 652800U) == 3U &&
			drv_i915_xd_cdclk_voltage(platform, 700000U) == 3U,
			"clock: the voltage levels of display 12");

	/* The table's lowest on a reference, and the PLL rate of a table clock. */
	state.ref_khz = 38400U;
	state.bypass_khz = 19200U;
	i915_host_check(drv_i915_xd_cdclk_lowest(platform, 38400U, 300000U) == 307200U && drv_i915_xd_cdclk_lowest(platform, 38400U, 700000U) == 0U &&
			drv_i915_xd_cdclk_vco(platform, &state, 326400U) == 1305600U && drv_i915_xd_cdclk_vco(platform, &state, 19200U) == 0U &&
			drv_i915_xd_cdclk_vco(platform, &state, 200000U) == 0U,
			"clock: the table's lowest clock and a clock's PLL rate");

	/* The highest clock: 648 MHz on 24 MHz, 652.8 MHz otherwise. */
	i915_host_check(drv_i915_xd_cdclk_max(&state) == 652800U, "clock: the highest clock on 38.4 MHz");
	state.ref_khz = 24000U;
	i915_host_check(drv_i915_xd_cdclk_max(&state) == 648000U, "clock: the highest clock on 24 MHz");
}
