/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the built-in panel's sink (display/xd-edp.c) and of
 * the EDID over the AUX channel's I2C (display/xd-aux.c, xd-edid.c), on
 * the register model (xd-fake-regs.c) with a panel's sink behind channel
 * A: the sequencer's choice and delays, VDD held across the accesses of
 * the taking, the power-up delay before the first access, VDD let go
 * five power cycles after its last use, VDD back on after the power
 * cycle and the power-up delay, the power references and the locks, the
 * panel's steps and their waits, the stop, the extended capabilities,
 * the refusals of the taking, and the EDID's reads over I2C (the sink's
 * and the device's requests to wait, a refusal, a damaged and an empty
 * EDID).
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-aux.h"
#include "../../display/xd-edid.h"
#include "../../display/xd-edp.h"
#include "../../display/xd-io.h"
#include "../../display/xd-panel.h"
#include "../../intel/display-dpcd-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The 5330's display: Alder Lake-P, PCI id 0x46a6 at revision 0x0c. */
#define I915_XD_EDP_TEST_ADLP		0x46a6U
#define I915_XD_EDP_TEST_ADLP_REVID	0x0cU

/* How long the model's panel takes to come on, in microseconds. */
#define I915_XD_EDP_TEST_POWER_UP_US	30000U

/*
 * The firmware's delays of the model, in the sequencer's 100 us units:
 * power up 210 ms, backlight 0.1 ms each way, power down 500 ms, and the
 * power cycle field 6 (600 ms).
 */
#define I915_XD_EDP_TEST_ON_DELAYS	((2100U << 16) | 1U)
#define I915_XD_EDP_TEST_OFF_DELAYS	((5000U << 16) | 1U)
#define I915_XD_EDP_TEST_CYCLE_FIELD	(6U << 4)

/*
 * What the test's environment saw: the references held for each use and
 * the ones taken, the locks held (bit per lock) and the takings in the
 * wrong order, the delayed VDD-off armed (and its delay) and the times it
 * was called off, and the notes.
 */
struct i915_xd_edp_test_env {
	int held[2];
	unsigned taken[2];
	unsigned later_puts;
	unsigned locks;
	unsigned wrong_order;
	int armed;
	unsigned armed_delay_ms;
	unsigned cancels;
	unsigned waited_cancels;
	unsigned notes;
	const char *last_note;
};

/* The window, the sink, the panel's sink and the environment the checks share; each group starts them afresh. */
static struct i915_xd_io i915_xd_edp_test_io;
static struct i915_xd_fake_aux i915_xd_edp_test_aux;
static struct i915_xd_edp i915_xd_edp_test_edp;
static struct i915_xd_edp_test_env i915_xd_edp_test_env;

static void i915_xd_edp_test_setup(void);
static void i915_xd_edp_test_edid(uint8_t *block);
static void i915_xd_edp_test_config(struct i915_xd_edp_config *config);
static void i915_xd_edp_test_status(uint32_t offset, uint32_t value, void *context);
static void i915_xd_edp_test_script(unsigned answer, unsigned param, unsigned count);
static unsigned i915_xd_edp_test_vdd_ons(void);
static uint64_t i915_xd_edp_test_now(void *context);
static int i915_xd_edp_test_power_get(void *context, unsigned use);
static void i915_xd_edp_test_power_put(void *context, unsigned use, int cookie);
static void i915_xd_edp_test_power_put_later(void *context, unsigned use, int cookie);
static void i915_xd_edp_test_lock(void *context, unsigned lock, int taken);
static void i915_xd_edp_test_vdd_later(void *context, unsigned delay_ms);
static void i915_xd_edp_test_vdd_cancel(void *context, int wait);
static void i915_xd_edp_test_error(void *context, const char *text);
static int i915_xd_edp_test_ddc(void *context, unsigned segment, unsigned offset, uint8_t *buffer, unsigned size);
static void i915_xd_edp_test_take(void);
static void i915_xd_edp_test_vdd_later_use(void);
static void i915_xd_edp_test_steps(void);
static void i915_xd_edp_test_caps(void);
static void i915_xd_edp_test_refusals(void);
static void i915_xd_edp_test_dpcd_faults(void);
static void i915_xd_edp_test_i2c(void);

/*
 * Runs every check of the panel's sink and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_edp_test_take();
	i915_xd_edp_test_vdd_later_use();
	i915_xd_edp_test_steps();
	i915_xd_edp_test_caps();
	i915_xd_edp_test_refusals();
	i915_xd_edp_test_dpcd_faults();
	i915_xd_edp_test_i2c();

	/* Reports the result. */
	failures = i915_host_report("host_xd_edp_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model: the panel's sink on channel A (DPCD 1.1, HBR, two
 * lanes, eDP display control capabilities, a one-block EDID), the
 * sequencer's firmware delays with the panel off and VDD off, the panel's
 * status following the power bit, and a fresh environment and sink.
 */
static void
i915_xd_edp_test_setup(void)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_edp_env env;
	const struct i915_xd_platform *platform;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_edp_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_edp_test_io, &hooks);

	/* The panel's sink: its capabilities, its eDP capabilities and its EDID. */
	memset(&i915_xd_edp_test_aux, 0, sizeof(i915_xd_edp_test_aux));
	i915_xd_edp_test_aux.dpcd[0x000] = 0x11U;
	i915_xd_edp_test_aux.dpcd[0x001] = 0x0aU;
	i915_xd_edp_test_aux.dpcd[0x002] = 0x82U;
	i915_xd_edp_test_aux.dpcd[0x700] = 0x03U;
	i915_xd_edp_test_aux.dpcd[0x701] = 0x01U;
	i915_xd_edp_test_aux.dpcd[0x702] = 0x00U;
	i915_xd_edp_test_edid(i915_xd_edp_test_aux.i2c);
	i915_xd_fake_aux_attach(&i915_xd_edp_test_aux, 0U);

	/* The firmware's delays; the panel follows the power bit. */
	i915_xd_fake_set(I915_XD_PP_ON_DELAYS(0U), I915_XD_EDP_TEST_ON_DELAYS);
	i915_xd_fake_set(I915_XD_PP_OFF_DELAYS(0U), I915_XD_EDP_TEST_OFF_DELAYS);
	i915_xd_fake_set(I915_XD_PP_CONTROL(0U), I915_XD_EDP_TEST_CYCLE_FIELD);
	i915_xd_fake_rule_call(I915_XD_PP_CONTROL(0U), i915_xd_edp_test_status, NULL);

	/* The 5330's description. */
	platform = NULL;
	error = drv_i915_xd_platform_choose(I915_XD_EDP_TEST_ADLP, I915_XD_EDP_TEST_ADLP_REVID, 0, 0U, &choice);
	if (error == 0)
		platform = choice.platform;

	/* A fresh environment and sink. */
	memset(&i915_xd_edp_test_env, 0, sizeof(i915_xd_edp_test_env));
	memset(&env, 0, sizeof(env));
	env.now_ms = i915_xd_edp_test_now;
	env.power_get = i915_xd_edp_test_power_get;
	env.power_put = i915_xd_edp_test_power_put;
	env.power_put_later = i915_xd_edp_test_power_put_later;
	env.lock = i915_xd_edp_test_lock;
	env.vdd_later = i915_xd_edp_test_vdd_later;
	env.vdd_cancel = i915_xd_edp_test_vdd_cancel;
	env.error = i915_xd_edp_test_error;
	env.context = &i915_xd_edp_test_env;
	drv_i915_xd_edp_init(&i915_xd_edp_test_edp, &i915_xd_edp_test_io, platform, &env);
}

/* Fills a one-block EDID: the header, a manufacturer and a product, no extension, the checksum. */
static void
i915_xd_edp_test_edid(
	uint8_t *block)
{
	static const uint8_t header[8] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };
	unsigned index;
	unsigned sum;

	/* The header, the ids and the version. */
	memset(block, 0, 128U);
	memcpy(block, header, sizeof(header));
	block[8] = 0x4cU;
	block[9] = 0x83U;
	block[10] = 0x5aU;
	block[11] = 0x41U;
	block[18] = 1U;
	block[19] = 4U;

	/* The checksum makes the bytes sum to 0. */
	sum = 0U;
	for (index = 0U; index < 127U; index++)
		sum += block[index];
	block[127] = (uint8_t)(0x100U - (sum & 0xffU));
}

/* Describes the 5330's panel: channel A, two sequencers with muxed pins, the VBT's controller 0, no VBT delays. */
static void
i915_xd_edp_test_config(
	struct i915_xd_edp_config *config)
{
	/* The panel's channel and sequencers. */
	memset(config, 0, sizeof(*config));
	config->aux_channel = 0U;
	config->sequencers = 2U;
	config->muxed = 1;
	config->controller = 0;
	config->long_power_cycle = 0;
}

/* Scripts the sink's next answers afresh (the answers of an earlier script are dropped). */
static void
i915_xd_edp_test_script(
	unsigned answer,
	unsigned param,
	unsigned count)
{
	/* An empty script, then the new one. */
	i915_xd_edp_test_aux.script_length = 0U;
	i915_xd_edp_test_aux.script_next = 0U;
	i915_xd_fake_aux_script(&i915_xd_edp_test_aux, answer, param, count);
}

/* Makes the model's panel follow the power bit: on 30 ms after it is set, off at once when it is cleared. */
static void
i915_xd_edp_test_status(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	uint32_t previous;

	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(context);

	/* The status follows the power bit when it changes. */
	previous = i915_xd_fake_previous();
	if ((value & I915_XD_PP_CONTROL_POWER_ON) != 0U && (previous & I915_XD_PP_CONTROL_POWER_ON) != 0U)
		return;
	if ((value & I915_XD_PP_CONTROL_POWER_ON) != 0U) {
		i915_xd_fake_later(I915_XD_EDP_TEST_POWER_UP_US, I915_XD_PP_STATUS(0U), 0xffffffffU,
				   I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_STATE_ON_IDLE);
	} else {
		i915_xd_fake_set(I915_XD_PP_STATUS(0U), 0U);
	}
}

/* Counts the writes of the window that switched VDD on (the hold set where it was clear). */
static unsigned
i915_xd_edp_test_vdd_ons(void)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned ons;
	int held;

	/* Follows the hold through the control word's writes. */
	count = drv_i915_xd_io_records(&i915_xd_edp_test_io, records, I915_XD_IO_RECORDS);
	ons = 0U;
	held = 0;
	for (index = 0U; index < count; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE || records[index].offset != I915_XD_PP_CONTROL(0U))
			continue;

		/* Counts each time VDD is forced on from off. */
		if ((records[index].value & I915_XD_PP_CONTROL_FORCE_VDD) != 0U && !held)
			ons++;
		held = 0;
		if ((records[index].value & I915_XD_PP_CONTROL_FORCE_VDD) != 0U)
			held = 1;
	}

	/* Succeeded: the times VDD came on. */
	return ons;
}

/* Reads the model's clock in milliseconds. */
static uint64_t
i915_xd_edp_test_now(
	void *context)
{
	(void)context;

	/* The model's microseconds. */
	return i915_xd_fake_now() / 1000U;
}

/* Takes a reference for a use: a fresh cookie. */
static int
i915_xd_edp_test_power_get(
	void *context,
	unsigned use)
{
	struct i915_xd_edp_test_env *env;

	/* One more held. */
	env = context;
	env->held[use]++;
	env->taken[use]++;

	/* Succeeded: the cookie. */
	return (int)env->taken[use];
}

/* Gives a reference back. */
static void
i915_xd_edp_test_power_put(
	void *context,
	unsigned use,
	int cookie)
{
	struct i915_xd_edp_test_env *env;

	UNUSED_PARAMETER(cookie);

	/* One fewer held. */
	env = context;
	env->held[use]--;
}

/* Lets a reference go later: the test lets it go at once and counts it. */
static void
i915_xd_edp_test_power_put_later(
	void *context,
	unsigned use,
	int cookie)
{
	struct i915_xd_edp_test_env *env;

	UNUSED_PARAMETER(cookie);

	/* One fewer held. */
	env = context;
	env->held[use]--;
	env->later_puts++;
}

/* Takes or drops a lock; the AUX channel's lock is never taken while the sequencer's is held. */
static void
i915_xd_edp_test_lock(
	void *context,
	unsigned lock,
	int taken)
{
	struct i915_xd_edp_test_env *env;

	/* The test's environment. */
	env = context;

	/* Drops it. */
	if (!taken) {
		env->locks &= ~(1U << lock);
		return;
	}

	/* The channel after the sequencer is the wrong order; a lock taken twice too. */
	if (lock == I915_XD_EDP_LOCK_AUX && (env->locks & (1U << I915_XD_EDP_LOCK_PPS)) != 0U)
		env->wrong_order++;
	if ((env->locks & (1U << lock)) != 0U)
		env->wrong_order++;
	env->locks |= 1U << lock;
}

/* Arms the delayed VDD-off. */
static void
i915_xd_edp_test_vdd_later(
	void *context,
	unsigned delay_ms)
{
	struct i915_xd_edp_test_env *env;

	/* Armed with its delay. */
	env = context;
	env->armed = 1;
	env->armed_delay_ms = delay_ms;
}

/* Disarms the delayed VDD-off. */
static void
i915_xd_edp_test_vdd_cancel(
	void *context,
	int wait)
{
	struct i915_xd_edp_test_env *env;

	/* Disarms the work and counts the cancel, and a cancel that waited. */
	env = context;
	env->armed = 0;
	env->cancels++;
	if (wait)
		env->waited_cancels++;
}

/* Counts a note of the sink. */
static void
i915_xd_edp_test_error(
	void *context,
	const char *text)
{
	struct i915_xd_edp_test_env *env;

	/* One more. */
	env = context;
	env->notes++;
	env->last_note = text;
}

/* Reads an EDID block over channel A's I2C, the channel held usable by the test. */
static int
i915_xd_edp_test_ddc(
	void *context,
	unsigned segment,
	unsigned offset,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_aux_i2c_message messages[2];
	struct i915_xd_aux *aux;
	uint8_t offset_byte;
	int error;

	UNUSED_PARAMETER(segment);

	/* The offset, then the bytes. */
	aux = context;
	offset_byte = (uint8_t)offset;
	messages[0].address = 0x50U;
	messages[0].read = 0;
	messages[0].bytes = &offset_byte;
	messages[0].length = 1U;
	messages[1].address = 0x50U;
	messages[1].read = 1;
	messages[1].bytes = buffer;
	messages[1].length = size;
	error = drv_i915_xd_aux_i2c(aux, &i915_xd_edp_test_io, messages, 2U);
	return error;
}

/*
 * Checks the taking of the 5330's panel: the VBT's sequencer, the
 * firmware's delays, the capabilities and the EDID, VDD on once and kept
 * across the accesses and after the taking (no VDD-off armed while the
 * delays are the early ones), the power-up delay before the first
 * access, and a second taking refused.
 */
static void
i915_xd_edp_test_take(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_edp *edp;
	int error;

	/* The taking. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == 0 && edp->live && edp->stage == I915_XD_EDP_STAGE_TAKEN, "edp: the taking succeeds up to the EDID");
	i915_host_check(edp->pps == 0U, "edp: sequencer 0 (the VBT's controller) is chosen");

	/* What it read. */
	i915_host_check(memcmp(edp->caps, i915_xd_edp_test_aux.dpcd, 15U) == 0 && edp->caps[1] == 0x0aU && edp->caps[2] == 0x82U,
			"edp: the receiver capabilities are the sink's 0x000..0x00e");
	i915_host_check(edp->edp_caps_read && memcmp(edp->edp_caps, &i915_xd_edp_test_aux.dpcd[0x700], 3U) == 0,
			"edp: the eDP display control capabilities are the sink's 0x700..0x702");
	i915_host_check(edp->edid_blocks == 1U && edp->edid_extensions == 0U && memcmp(edp->edid, i915_xd_edp_test_aux.i2c, 128U) == 0,
			"edp: the EDID is the sink's one block");

	/* The delays the firmware gave: power up 210 ms, the power cycle 600 ms. */
	i915_host_check(edp->waits.power_up_ms == 210U && edp->waits.power_cycle_ms == 600U, "edp: the power cycle field 6 is 600 ms, the power up 210 ms");

	/* VDD on once, kept on after the taking, nothing armed while the delays are early. */
	i915_host_check(i915_xd_edp_test_vdd_ons() == 1U &&
			(i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) != 0U &&
			edp->vdd_cookie != 0 && !i915_xd_edp_test_env.armed,
			"edp: VDD came on once and stays on, no VDD-off armed during the early delays");

	/* The panel's power-up delay before the sink was asked. */
	i915_host_check(i915_xd_fake_now() >= 210000U, "edp: the 210 ms power-up delay was waited before the first access");

	/* The references: VDD's AUX one held, the accesses' given back later, the core's given back; no lock left. */
	i915_host_check(i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 1 &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_CORE] == 0 &&
			i915_xd_edp_test_env.later_puts != 0U &&
			i915_xd_edp_test_env.locks == 0U && i915_xd_edp_test_env.wrong_order == 0U,
			"edp: VDD holds the AUX power, the accesses' references went later, no lock held, no wrong order");
	i915_host_check(i915_xd_edp_test_env.notes == 0U, "edp: no note during the taking");

	/* A second taking. */
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == EBUSY, "edp: a second taking is refused (EBUSY)");
}

/*
 * Checks VDD after the late step: armed five power cycles out (3 s),
 * let go when the delayed VDD-off runs, back on for a later access after
 * the power cycle and the power-up delay, and armed again 3 s after that
 * access; a VDD-off that finds VDD wanted keeps it; the stop calls the
 * delayed VDD-off off, lets VDD go and gives every reference back.
 */
static void
i915_xd_edp_test_vdd_later_use(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_pps_delays vbt;
	struct i915_xd_edp *edp;
	uint64_t off_at;
	uint8_t byte;
	int error;

	/* Taken and finished with no VBT delays. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	(void)drv_i915_xd_edp_start(edp, &config);
	memset(&vbt, 0, sizeof(vbt));
	drv_i915_xd_edp_late(edp, 0, &vbt);
	i915_host_check(i915_xd_edp_test_env.armed && i915_xd_edp_test_env.armed_delay_ms == 3000U,
			"edp: after the late step VDD goes in 3000 ms (5 x the 600 ms power cycle)");

	/* The delayed VDD-off runs: VDD off, its AUX power given back. */
	drv_i915_xd_edp_vdd_work(edp);
	off_at = i915_xd_fake_now();
	i915_host_check((i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) == 0U &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 0 && edp->vdd_cookie == 0,
			"edp: the delayed VDD-off lets VDD and its AUX power go");

	/* A later access turns VDD on by itself, after the power cycle and the power-up delay. */
	error = drv_i915_xd_edp_dpcd_read(edp, I915_XD_DPCD_REV, &byte, 1U);
	i915_host_check(error == 0 && byte == 0x11U && (i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) != 0U,
			"edp: a later read turns VDD on again by itself");
	i915_host_check(i915_xd_fake_now() >= off_at + 810000U, "edp: ... after the 600 ms power cycle and the 210 ms power-up delay");
	i915_host_check(i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 1 && edp->vdd_cookie != 0,
			"edp: the access's AUX reference went, VDD's is held");
	i915_host_check(i915_xd_edp_test_env.armed && i915_xd_edp_test_env.armed_delay_ms == 3000U,
			"edp: ... and VDD goes again 3 s after that access");

	/* A delayed VDD-off that finds VDD wanted again keeps it. */
	(void)drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_VDD_ON);
	drv_i915_xd_edp_vdd_work(edp);
	i915_host_check((i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) != 0U,
			"edp: a delayed VDD-off keeps a VDD that is wanted");

	/* The stop: called off and waited for, VDD off, everything given back. */
	drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_VDD_OFF);
	drv_i915_xd_edp_stop(edp);
	i915_host_check(!edp->live && i915_xd_edp_test_env.waited_cancels != 0U &&
			(i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) == 0U &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 0 &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_CORE] == 0 && i915_xd_edp_test_env.locks == 0U,
			"edp: the stop calls the VDD-off off, lets VDD go and gives every reference back");

	/* Nothing works on a sink let go. */
	error = drv_i915_xd_edp_dpcd_read(edp, I915_XD_DPCD_REV, &byte, 1U);
	i915_host_check(error == EINVAL, "edp: a read of a sink let go is refused");
	error = drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_ON);
	i915_host_check(error == EINVAL, "edp: a step of a sink let go is refused");
}

/*
 * Checks the screen's steps: VDD, the power on after the power cycle,
 * the backlight enable after its delay, the backlight off, the power off
 * with VDD and its AUX power, and a power on that waits out the power
 * cycle; an unknown step is refused.
 */
static void
i915_xd_edp_test_steps(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_pps_delays vbt;
	struct i915_xd_edp *edp;
	uint64_t at;
	uint32_t control;
	int error;

	/* Taken and finished, with the VBT's backlight-on delay of 50 ms. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	(void)drv_i915_xd_edp_start(edp, &config);
	memset(&vbt, 0, sizeof(vbt));
	vbt.backlight_on = 500U;
	drv_i915_xd_edp_late(edp, 0, &vbt);

	/* VDD, then the power on. */
	error = drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_VDD_ON);
	error |= drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_ON);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check(error == 0 && (control & I915_XD_PP_CONTROL_POWER_ON) != 0U &&
			(i915_xd_fake_get(I915_XD_PP_STATUS(0U)) & I915_XD_PP_STATUS_ON) != 0U && i915_xd_edp_test_env.notes == 0U,
			"edp: VDD and the power on light the panel");

	/* The backlight enable 50 ms after the power came on. */
	at = edp->power_on_ms;
	error = drv_i915_xd_edp_panel(edp, I915_XD_EDP_BACKLIGHT_ON);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check(error == 0 && (control & I915_XD_PP_CONTROL_BACKLIGHT) != 0U && i915_xd_fake_now() / 1000U >= at + 50U,
			"edp: the backlight enable waits the 50 ms backlight-on delay since the power came on");

	/* The backlight off. */
	error = drv_i915_xd_edp_panel(edp, I915_XD_EDP_BACKLIGHT_OFF);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check(error == 0 && (control & I915_XD_PP_CONTROL_BACKLIGHT) == 0U, "edp: the backlight enable is cleared");

	/* The power off takes VDD and its AUX power with it. */
	error = drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_OFF);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check(error == 0 &&
			(control & (I915_XD_PP_CONTROL_POWER_ON | I915_XD_PP_CONTROL_FORCE_VDD | I915_XD_PP_CONTROL_BACKLIGHT)) == 0U &&
			edp->vdd_cookie == 0 && !edp->vdd_wanted && i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 0 &&
			i915_xd_edp_test_env.notes == 0U,
			"edp: the power off takes VDD, the backlight enable and the AUX power with it");

	/* A power on right after waits out the 600 ms power cycle. */
	at = i915_xd_fake_now();
	(void)drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_VDD_ON);
	(void)drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_ON);
	i915_host_check(i915_xd_fake_now() >= at + 600000U, "edp: a power on right after the power off waits the 600 ms power cycle");

	/* A power on of a panel already on is noted and changes nothing. */
	(void)drv_i915_xd_edp_panel(edp, I915_XD_EDP_PANEL_ON);
	i915_host_check(i915_xd_edp_test_env.notes == 1U, "edp: a power on of a panel already on is noted");

	/* An unknown step. */
	error = drv_i915_xd_edp_panel(edp, 99U);
	i915_host_check(error == EINVAL, "edp: an unknown step is refused");
}

/*
 * Checks the extended receiver capabilities: they replace the
 * receiver's when the sink has them and their revision is not lower,
 * and are left when it is lower.
 */
static void
i915_xd_edp_test_caps(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_edp *edp;
	uint8_t caps[15];
	int error;

	/* A sink with extended capabilities of revision 1.4. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_aux.dpcd[0x00e] = I915_XD_DPCD_EXTENDED_PRESENT;
	i915_xd_edp_test_aux.dpcd[0x2200 % I915_XD_FAKE_AUX_DPCD_BYTES] = 0x14U;
	i915_xd_edp_test_aux.dpcd[(0x2200 + 1) % I915_XD_FAKE_AUX_DPCD_BYTES] = 0x14U;
	i915_xd_edp_test_config(&config);
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == 0 && edp->caps[0] == 0x14U && edp->caps[1] == 0x14U, "edp: the extended capabilities replace the receiver's");

	/* A lower extended revision is left. */
	i915_xd_edp_test_aux.dpcd[0x2200 % I915_XD_FAKE_AUX_DPCD_BYTES] = 0x10U;
	error = drv_i915_xd_edp_read_caps(edp, caps);
	i915_host_check(error == 0 && caps[0] == 0x11U && caps[1] == 0x0aU, "edp: a lower extended revision leaves the receiver's capabilities");

	/* A revision of 0 makes the capabilities useless. */
	i915_xd_edp_test_aux.dpcd[0x000] = 0U;
	error = drv_i915_xd_edp_read_caps(edp, caps);
	i915_host_check(error == EIO, "edp: a receiver revision of 0 is an error");
}

/*
 * Checks the refusals of the taking: a Type-C channel, a sink that does
 * not answer (VDD let go, every reference back, nothing live), an empty
 * EDID and a damaged one.
 */
static void
i915_xd_edp_test_refusals(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_edp *edp;
	int error;

	/* A Type-C channel. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	config.aux_channel = 3U;
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == EINVAL && !edp->live && i915_xd_edp_test_vdd_ons() == 0U,
			"edp: a Type-C channel is refused, nothing switched");

	/* A sink that never answers. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	i915_xd_fake_aux_attach(&i915_xd_edp_test_aux, 5U);
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error != 0 && !edp->live && edp->stage == I915_XD_EDP_STAGE_PPS &&
			(i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) == 0U &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 0 &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_CORE] == 0 && i915_xd_edp_test_env.locks == 0U,
			"edp: a sink that does not answer fails the taking, VDD off, every reference back");

	/* An empty EDID. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	memset(i915_xd_edp_test_aux.i2c, 0, sizeof(i915_xd_edp_test_aux.i2c));
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == ENXIO && !edp->live && edp->stage == I915_XD_EDP_STAGE_CAPS &&
			(i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) & I915_XD_PP_CONTROL_FORCE_VDD) == 0U,
			"edp: an all-zero EDID stops the taking (ENXIO), VDD off");

	/* A damaged EDID. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	i915_xd_edp_test_aux.i2c[127] ^= 0x01U;
	error = drv_i915_xd_edp_start(edp, &config);
	i915_host_check(error == EPROTO && !edp->live && i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 0,
			"edp: an EDID that never checks stops the taking (EPROTO), everything back");
}

/*
 * Checks the DPCD accesses of a taken sink: the sink's requests to wait
 * and a refusal are tried again; a refusal that never ends fails after
 * the tries, every reference back.
 */
static void
i915_xd_edp_test_dpcd_faults(void)
{
	struct i915_xd_edp_config config;
	struct i915_xd_edp *edp;
	unsigned before;
	uint8_t bytes[2];
	int error;

	/* A panel's sink taken from a fresh model. */
	i915_xd_edp_test_setup();
	edp = &i915_xd_edp_test_edp;
	i915_xd_edp_test_config(&config);
	(void)drv_i915_xd_edp_start(edp, &config);

	/* Three requests to wait, then the answer. */
	i915_xd_edp_test_script(I915_XD_FAKE_AUX_DEFER, 0U, 3U);
	error = drv_i915_xd_edp_dpcd_read(edp, 0x001U, bytes, 2U);
	i915_host_check(error == 0 && bytes[0] == 0x0aU && bytes[1] == 0x82U, "edp: three native DEFERs are tried again");

	/* One refusal, then the answer. */
	i915_xd_edp_test_script(I915_XD_FAKE_AUX_NACK, 0U, 1U);
	error = drv_i915_xd_edp_dpcd_write(edp, 0x600U, bytes, 1U);
	i915_host_check(error == 0 && i915_xd_edp_test_aux.dpcd[0x600] == 0x0aU, "edp: one native NACK is tried again");

	/* A refusal that never ends. */
	before = i915_xd_edp_test_aux.transactions;
	i915_xd_edp_test_script(I915_XD_FAKE_AUX_NACK, 0U, I915_XD_FAKE_AUX_SCRIPT);
	error = drv_i915_xd_edp_dpcd_read(edp, 0x001U, bytes, 1U);
	i915_host_check(error == EIO && i915_xd_edp_test_aux.transactions - before == 32U &&
			i915_xd_edp_test_env.held[I915_XD_EDP_POWER_AUX] == 1 && i915_xd_edp_test_env.locks == 0U,
			"edp: a NACK that never ends is EIO after 32 tries, only VDD's reference held");
}

/*
 * Checks the EDID over channel A's I2C: the device's and the sink's
 * requests to wait are tried again, a refused transfer is tried again,
 * and a block read over several messages comes out whole.
 */
static void
i915_xd_edp_test_i2c(void)
{
	struct i915_xd_edid_ddc ddc;
	struct i915_xd_aux aux;
	uint8_t edid[256];
	unsigned blocks;
	unsigned extensions;
	int error;

	/* The channel and its DDC. */
	i915_xd_edp_test_setup();
	(void)drv_i915_xd_aux_init(&aux, 0U);
	aux.stalled = NULL;
	ddc.read_block = i915_xd_edp_test_ddc;
	ddc.context = &aux;

	/* Five requests to wait from the device. */
	i915_xd_edp_test_script(I915_XD_FAKE_AUX_DEFER, 0U, 5U);
	error = drv_i915_xd_edid_read(&ddc, edid, 2U, &blocks, &extensions);
	i915_host_check(error == 0 && blocks == 1U && memcmp(edid, i915_xd_edp_test_aux.i2c, 128U) == 0,
			"edp: five I2C DEFERs during the EDID are tried again");

	/* One refusal: the block is read again. */
	i915_xd_edp_test_script(I915_XD_FAKE_AUX_NACK, 0U, 1U);
	error = drv_i915_xd_edid_read(&ddc, edid, 2U, &blocks, &extensions);
	i915_host_check(error == 0 && blocks == 1U && memcmp(edid, i915_xd_edp_test_aux.i2c, 128U) == 0,
			"edp: one I2C NACK: the block is read again");

	/* A channel with no sink behind it. */
	i915_xd_fake_aux_attach(&i915_xd_edp_test_aux, 5U);
	error = drv_i915_xd_edid_read(&ddc, edid, 2U, &blocks, &extensions);
	i915_host_check(error == EIO && blocks == 0U, "edp: a channel with no sink is EIO, no block");
}
