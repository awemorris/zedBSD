/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HDMI hotplug receive path on fake registers, as a part of the display
 * ktest.
 *
 * The Linux hotplug chain of the production code runs on fake
 * SHOTPLUG_CTL_DDI / SDEISR registers and a GMBUS sink model, with the
 * real work queue, timer queue, spinlock and mutex.  It runs on a display
 * of its own, so the device's hotplug interrupts, which enter the started
 * display's hotplug world, never reach the model.
 *
 *   HPD-DECODE   SDEIIR bit 17 and a long pulse on B: pin 5, the write-back
 *                clears the status, the work runs
 *   HPD-PLUG     live status set: HDMI-A-1 disconnected -> connected
 *                (CHANGED, epoch + 1, no retry)
 *   HPD-RETRY    an interrupt without a status change: UNCHANGED -> RETRY
 *                once after 1000 ms, then UNCHANGED
 *   HPD-EDID     the plug detection reads the sink's EDID over the GMBUS
 *                model (index read at 0x50, header and checksum)
 *   HPD-EDIDCHG  a different EDID behind the same live status: epoch + 1,
 *                CHANGED (connected -> connected)
 *   HPD-UNPLUG   live status clear: connected -> disconnected (CHANGED), no
 *                DDC access
 *   HPD-NODDC    live status set but no DDC answer (NAK, retried once):
 *                disconnected
 *   HPD-EDP      pin 4 (eDP, has hpd_pulse): the dig-port work (a step), no
 *                hotplug work for the pin
 *   HPD-STORM    6 long pulses within the period: MARK_DISABLED, irq_setup,
 *                the work switches the connector to polling; the re-enable
 *                work restores HPD
 *   HPD-GATE     after the stop an interrupt is dropped (no register access)
 */

#include "display-ktest.h"
#include "hpd-model.h"
#include "../execution/ktest.h"
#include "../../display/hotplug.h"
#include "../../display/hdmi.h"
#include <kern/kcrt.h>

#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stdint.h>

/* The SDEIIR / SDEISR bits of the DDI A and DDI B hotplug pins. */
#define I915_TEST_SDE_DDI_A		0x10000u
#define I915_TEST_SDE_DDI_B		0x20000u

/* SHOTPLUG_CTL_DDI: A and B enabled with no status, B with a long pulse, A with a long pulse. */
#define I915_TEST_SHOTPLUG_ENABLED	0x88u
#define I915_TEST_SHOTPLUG_B_LONG	0xa8u
#define I915_TEST_SHOTPLUG_A_LONG	0x8au

/* The display version and the VBT device types of the target's outputs (eDP, HDMI, DP on Type-C). */
#define I915_TEST_DISPLAY_VER		13
#define I915_TEST_DEVICE_TYPE_EDP	0x1806u
#define I915_TEST_DEVICE_TYPE_HDMI	0x60d2u
#define I915_TEST_DEVICE_TYPE_DP_TC	0x68c6u

/* The connector statuses (enum connector_status). */
#define I915_TEST_STATUS_CONNECTED	1
#define I915_TEST_STATUS_DISCONNECTED	2

/* The outcomes of one detection (enum intel_hotplug_state). */
#define I915_TEST_HOTPLUG_UNCHANGED	0
#define I915_TEST_HOTPLUG_CHANGED	1
#define I915_TEST_HOTPLUG_RETRY		2

/* The polling modes of a connector: HPD, and connect | disconnect polling. */
#define I915_TEST_POLL_HPD		1
#define I915_TEST_POLL_CONNECT_DISCONNECT 6

/* How many pulses at most make a storm (6 x 10 > the threshold of 50). */
#define I915_TEST_STORM_PULSES		6u

/* How often, and how many ticks apart, the bounded waits poll. */
#define I915_TEST_POLL_TRIES		150u
#define I915_TEST_POLL_TICKS		2u

/*
 * The display the model runs on.
 *
 * It is allocated at the start of the run and freed at its end; it holds
 * only the hotplug world, which no hardware interrupt reaches.
 */
static struct i915_display *model_display;

/*
 * The hotplug state of the Linux driver probe the model is given: the pin
 * tables and the register programming.  Cleared at the start of each run.
 */
static struct i915_hotplug model_hotplug;

/*
 * The encoders the output setup would have found: the target's eDP on A,
 * HDMI on B, DP on TC1 and TC2.  Cleared and filled at the start of each
 * run.
 */
static struct i915_display_nogem model_nogem;

/*
 * The fake hotplug and GMBUS registers and the DDC sink behind them.
 *
 * The test sets the live status and the pulses; the path reads, writes
 * back and counts through them.  Cleared at the start of each run.
 */
static struct i915_hpd_fake_regs model_fake;

/*
 * The completion the test sleeps on between steps.
 *
 * Nobody signals it; each wait simply lasts until its deadline.
 */
static struct i915_completion model_sleep;

/*
 * The EDID the DDC sink answers with.  It is rebuilt when the test changes
 * the sink.
 */
static uint8_t model_edid[128];

static void i915_make_edid(uint32_t serial);
static void i915_model_sleep(unsigned ticks);
static int i915_wait_records(unsigned count, unsigned ticks);
static void i915_set_encoder(unsigned index, int port, int phy, int is_tc, int init_hdmi, int init_dp, uint32_t device_type);
static int i915_model_create(void);
static void i915_model_destroy(void);
static int i915_start_tests(struct i915_ktest *ktest);
static void i915_plug_tests(struct i915_ktest *ktest, int hdmi);
static void i915_retry_tests(struct i915_ktest *ktest);
static void i915_edid_change_tests(struct i915_ktest *ktest);
static void i915_unplug_tests(struct i915_ktest *ktest, int hdmi);
static void i915_noddc_tests(struct i915_ktest *ktest);
static void i915_edp_tests(struct i915_ktest *ktest);
static void i915_storm_tests(struct i915_ktest *ktest, int hdmi);
static void i915_gate_tests(struct i915_ktest *ktest);

/*
 * Runs the hotplug receive path tests on fake registers.
 *
 * The model gets a display and a hotplug world of its own for the length of
 * the run.
 */
void
drv_i915_display_ktest_hpd(
	struct i915_ktest *ktest)
{
	int error;
	int hdmi;

	/* Gives the model a display of its own. */
	error = i915_model_create();
	if (error != 0) {
		drv_i915_ktest_skip(ktest, "hpd: the hotplug model tests", "no memory for the model's display");
		return;
	}

	/* Starts the model and finds its HDMI connector; without one nothing else can run. */
	hdmi = i915_start_tests(ktest);
	if (hdmi < 0) {
		drv_i915_hpd_stop(model_display);
		i915_model_destroy();
		return;
	}

	/* The plug, and the EDID read over the GMBUS model. */
	i915_plug_tests(ktest, hdmi);

	/* An interrupt without a change: the second detection pass. */
	i915_retry_tests(ktest);

	/* A different EDID behind the same live status. */
	i915_edid_change_tests(ktest);

	/* The unplug. */
	i915_unplug_tests(ktest, hdmi);

	/* Live status set, but no DDC answer. */
	i915_noddc_tests(ktest);

	/* The eDP pin: the dig-port path. */
	i915_edp_tests(ktest);

	/* The storm and the re-enable. */
	i915_storm_tests(ktest, hdmi);

	/* The stop closes the entry. */
	i915_gate_tests(ktest);

	/* Frees the model's display. */
	i915_model_destroy();
}

/* Builds a plain EDID 1.4 block: digital input, one detailed timing (1920x1080 148.5 MHz), valid checksum. */
static void
i915_make_edid(
	uint32_t serial)
{
	static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	unsigned i;
	unsigned sum;

	/* The fixed header. */
	kern_memset(model_edid, 0, sizeof(model_edid));
	kern_memcpy(model_edid, header, 8);

	/* The manufacturer "ZED" (0 11010 00101 00100) and the product 0x1234. */
	model_edid[8] = 0x68;
	model_edid[9] = 0xa4;
	model_edid[10] = 0x34;
	model_edid[11] = 0x12;

	/* The serial number, little-endian. */
	model_edid[12] = (uint8_t)serial;
	model_edid[13] = (uint8_t)(serial >> 8);
	model_edid[14] = (uint8_t)(serial >> 16);
	model_edid[15] = (uint8_t)(serial >> 24);

	/* EDID version 1.4, digital input. */
	model_edid[18] = 1;
	model_edid[19] = 4;
	model_edid[20] = 0x80;

	/* The first detailed timing: 14850 x 10 kHz, hactive 0x780, vactive 0x438. */
	model_edid[54] = 0x02;
	model_edid[55] = 0x3a;
	model_edid[56] = 0x80;
	model_edid[58] = 0x70;
	model_edid[59] = 0x38;
	model_edid[61] = 0x40;

	/* The checksum makes the block sum to zero. */
	sum = 0u;
	for (i = 0u; i < 127u; i++)
		sum += model_edid[i];
	model_edid[127] = (uint8_t)(256u - (sum & 0xffu));
}

/* Sleeps the test thread for the given number of ticks. */
static void
i915_model_sleep(
	unsigned ticks)
{
	uint64_t deadline;

	/* Waits on the completion nobody signals, until the deadline. */
	deadline = sched_ticks() + ticks;
	(void)drv_i915_wait_for_completion(&model_sleep, deadline);
}

/* Waits, for at most the given ticks, until count hotplug records exist; returns 1 when they do. */
static int
i915_wait_records(
	unsigned count,
	unsigned ticks)
{
	struct i915_hpd_summary summary;
	unsigned waited;

	/* Polls the record count until it is reached or the time is up. */
	waited = 0u;
	for (;;) {
		/* Enough records: the works have run. */
		drv_i915_hpd_summary(model_display, &summary);
		if (summary.hotplug_records >= count)
			return 1;

		/* Out of time: the works did not run. */
		if (waited >= ticks)
			return 0;

		/* Sleeps a little before looking again. */
		i915_model_sleep(I915_TEST_POLL_TICKS);
		waited += I915_TEST_POLL_TICKS;
	}
}

/* Describes one encoder of the model's output setup. */
static void
i915_set_encoder(
	unsigned index,
	int port,
	int phy,
	int is_tc,
	int init_hdmi,
	int init_dp,
	uint32_t device_type)
{
	/* Fills the fields the hotplug path reads. */
	model_nogem.encoders[index].port = port;
	model_nogem.encoders[index].phy = phy;
	model_nogem.encoders[index].is_tc = is_tc;
	model_nogem.encoders[index].init_hdmi = init_hdmi;
	model_nogem.encoders[index].init_dp = init_dp;
	model_nogem.encoders[index].device_type = device_type;
}

/* Allocates the model's display with its hotplug world; returns 0 or ENOMEM. */
static int
i915_model_create(void)
{
	int error;

	/* Allocates the display, zeroed. */
	model_display = kern_calloc(1U, sizeof(*model_display));
	if (model_display == NULL)
		return ENOMEM;

	/* Gives it a hotplug world. */
	error = drv_i915_hpd_world_create(model_display);
	if (error != 0) {
		kern_free(model_display);
		model_display = NULL;
		return error;
	}

	/* Succeeded: the model has a display of its own. */
	return 0;
}

/* Frees the model's hotplug world and display; the path must have been stopped. */
static void
i915_model_destroy(void)
{
	/* Frees the world, then the display. */
	drv_i915_hpd_world_destroy(model_display);
	kern_free(model_display);
	model_display = NULL;
}

/*
 * Prepares the model and starts the path on it; returns the index of the
 * HDMI connector, or -1 when there is none.
 */
static int
i915_start_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	const char *hdmi_name;
	const char *edp_name;
	const char *dp_name;
	int hdmi_named;
	int edp_named;
	int dp_named;
	int status;
	int error;
	int hdmi;
	int passed;

	/* Clears the model's state and prepares the sleep. */
	drv_i915_completion_init(&model_sleep, "hpd-ktest-sleep");
	kern_memset(&model_hotplug, 0, sizeof(model_hotplug));
	kern_memset(&model_nogem, 0, sizeof(model_nogem));
	kern_memset(&model_fake, 0, sizeof(model_fake));

	/* The target's outputs: eDP on A, HDMI on B, DP on TC1 and TC2. */
	i915_set_encoder(0u, 0, 0, 0, 0, 1, I915_TEST_DEVICE_TYPE_EDP);
	i915_set_encoder(1u, 1, 1, 0, 1, 0, I915_TEST_DEVICE_TYPE_HDMI);
	i915_set_encoder(2u, 3, 5, 1, 0, 1, I915_TEST_DEVICE_TYPE_DP_TC);
	i915_set_encoder(3u, 4, 6, 1, 0, 1, I915_TEST_DEVICE_TYPE_DP_TC);
	model_nogem.num_encoders = 4u;

	/* The pin tables: pin B on SDE bit 17, pin A on bit 16. */
	drv_i915_hpd_init_pins(&model_hotplug, I915_TEST_DISPLAY_VER, I915_PCH_ADP);
	passed = 0;
	if (model_hotplug.pch_hpd[I915_HPD_PORT_B] == I915_TEST_SDE_DDI_B && model_hotplug.pch_hpd[I915_HPD_PORT_A] == I915_TEST_SDE_DDI_A)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-PINS pch_hpd[B] = SDE bit 17, [A] = bit 16");

	/* eDP live and HDMI not, A and B enabled with no status, a sink with an EDID on the DDC. */
	model_fake.sdeisr = I915_TEST_SDE_DDI_A;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_ENABLED;
	i915_make_edid(0x1u);
	model_fake.ddc_edid = model_edid;
	model_fake.ddc_edid_len = sizeof(model_edid);
	model_fake.ddc_present = 1;

	/* Starts the path on the fake registers. */
	error = drv_i915_hpd_start(model_display, &model_hotplug, NULL, &model_nogem, NULL, NULL, I915_PCH_ADP, 1, &model_fake);
	drv_i915_ktest_check(ktest, error == 0, "hpd: HPD-START model backend");

	/* One connector per encoder; the HDMI one on DDI B. */
	drv_i915_hpd_summary(model_display, &summary);
	hdmi = summary.hdmi_connector;
	passed = 0;
	if (summary.num_connectors == 4u && hdmi == 1)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-OBJECTS 4 connectors, HDMI-A-1 on DDI B");

	/* The connectors carry drm's names. */
	passed = 0;
	if (hdmi >= 0) {
		hdmi_name = drv_i915_hpd_connector_name(model_display, (unsigned)hdmi);
		edp_name = drv_i915_hpd_connector_name(model_display, 0u);
		dp_name = drv_i915_hpd_connector_name(model_display, 2u);
		hdmi_named = kern_strcmp(hdmi_name, "HDMI-A-1");
		edp_named = kern_strcmp(edp_name, "eDP-1");
		dp_named = kern_strcmp(dp_name, "DP-1");
		if (hdmi_named == 0 &&
		    edp_named == 0 &&
		    dp_named == 0)
			passed = 1;
	}
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-NAMES eDP-1 HDMI-A-1 DP-1");

	/* Without an HDMI connector the rest of the run has nothing to watch. */
	if (hdmi < 0)
		return -1;

	/* The first detection finds the live status clear. */
	status = drv_i915_hpd_probe_connector(model_display, (unsigned)hdmi);
	drv_i915_hpd_summary(model_display, &summary);
	passed = 0;
	if (status == I915_TEST_STATUS_DISCONNECTED && summary.hdmi_epoch == 1ull)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-INIT first detection: disconnected (live status clear), epoch 1");

	/* Succeeded: reports the HDMI connector. */
	return hdmi;
}

/* Plugs the HDMI sink and checks the decode, the detection and the EDID read. */
static void
i915_plug_tests(
	struct i915_ktest *ktest,
	int hdmi)
{
	struct i915_hpd_summary summary;
	struct i915_hpd_edid_info edid_info;
	const struct i915_hpd_irq_record *irq;
	const struct i915_hpd_hotplug_record *record;
	const uint8_t *edid;
	uint32_t retry_bits;
	unsigned edid_size;
	int edid_differs;
	int ran;
	int passed;

	/* B goes live with a long pulse; the interrupt arrives. */
	model_fake.sdeisr = I915_TEST_SDE_DDI_A | I915_TEST_SDE_DDI_B;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);

	/* The handler wrote the read value back: the status cleared, the enables kept. */
	passed = 0;
	if (model_fake.rmw_writes == 1u &&
	    model_fake.last_rmw_write == I915_TEST_SHOTPLUG_B_LONG &&
	    model_fake.shotplug_ddi == I915_TEST_SHOTPLUG_ENABLED)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-DECODE the rmw wrote the read value back (status cleared, enables kept)");

	/* Pin 5 is an event for the hotplug work: HDMI has no hpd_pulse. */
	irq = drv_i915_hpd_irq_record(model_display, 0u);
	passed = 0;
	if (irq != NULL &&
	    irq->shotplug_ddi == I915_TEST_SHOTPLUG_B_LONG &&
	    (irq->event_bits_after & (1u << I915_HPD_PORT_B)) != 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-DECODE pin 5 in event_bits (no hpd_pulse on HDMI: the hotplug work handles it)");

	/* The hotplug work runs and finds the sink. */
	ran = i915_wait_records(1u, 100u);
	drv_i915_ktest_check(ktest, ran, "hpd: HPD-PLUG the hotplug work ran");
	record = drv_i915_hpd_hotplug_record(model_display, 0u);
	passed = 0;
	if (record != NULL &&
	    record->pin == I915_HPD_PORT_B &&
	    record->retries == 0 &&
	    record->old_status == I915_TEST_STATUS_DISCONNECTED &&
	    record->new_status == I915_TEST_STATUS_CONNECTED &&
	    record->state == I915_TEST_HOTPLUG_CHANGED &&
	    record->live == 1)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-PLUG disconnected -> connected, CHANGED, retry 0");

	/* A change arms no retry, and the epoch moved once. */
	i915_model_sleep(20u);
	drv_i915_hpd_summary(model_display, &summary);
	retry_bits = drv_i915_hpd_retry_bits(model_display);
	passed = 0;
	if (summary.to_connected == 1u &&
	    summary.hotplug_records == 1u &&
	    retry_bits == 0u &&
	    summary.hdmi_epoch == 2ull)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-PLUG no retry armed after a change, epoch 2");

	/* The base block read over GMBUS decodes as the sink's. */
	edid_size = 0u;
	edid = drv_i915_hpd_edid_bytes(model_display->hpd_world, (unsigned)hdmi, &edid_size);
	drv_i915_hpd_edid_info(model_display->hpd_world, &edid_info);
	passed = 0;
	if (edid_info.rc == 1 &&
	    edid_info.blocks == 1u &&
	    edid_info.digital == 1u &&
	    edid_info.mfg[0] == 'Z' &&
	    edid_info.mfg[1] == 'E' &&
	    edid_info.mfg[2] == 'D' &&
	    edid_info.product == 0x1234u &&
	    edid_info.hactive == 1920u &&
	    edid_info.vactive == 1080u &&
	    edid_info.pixel_clock_khz == 148500u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-EDID base block over GMBUS: ZED 0x1234, digital, DTD1 1920x1080 148.5 MHz");

	/* The stored bytes are the sink's, read without a NAK. */
	edid_differs = 1;
	if (edid != NULL && edid_size == 128u)
		edid_differs = kern_memcmp(edid, model_edid, 128);
	passed = 0;
	if (edid_differs == 0 &&
	    model_fake.gm_reads >= 32u &&
	    model_fake.gm_naks == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-EDID the 128 bytes equal the sink's (32+ GMBUS3 reads, no NAK)");
}

/* Raises an interrupt without a status change and checks the one retry. */
static void
i915_retry_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	const struct i915_hpd_hotplug_record *first;
	const struct i915_hpd_hotplug_record *second;
	unsigned base;
	int ran;
	int passed;

	/* Another long pulse on B, with nothing changed. */
	drv_i915_hpd_summary(model_display, &summary);
	base = summary.hotplug_records;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);

	/* Two detections: the event, and the retry after HPD_RETRY_DELAY. */
	ran = i915_wait_records(base + 2u, 300u);
	drv_i915_ktest_check(ktest, ran, "hpd: HPD-RETRY two detections (event + retry after HPD_RETRY_DELAY)");

	/* The first pass finds nothing changed and asks for a retry. */
	first = drv_i915_hpd_hotplug_record(model_display, base);
	passed = 0;
	if (first != NULL &&
	    first->retries == 0 &&
	    first->state == I915_TEST_HOTPLUG_RETRY)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-RETRY first pass UNCHANGED -> RETRY");

	/* The second pass runs about 1000 ms later and asks for nothing more. */
	second = drv_i915_hpd_hotplug_record(model_display, base + 1u);
	passed = 0;
	if (first != NULL &&
	    second != NULL &&
	    second->retries == 1 &&
	    second->state == I915_TEST_HOTPLUG_UNCHANGED &&
	    second->tick >= first->tick + 90u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-RETRY second pass ~1000 ms later, retry 1, UNCHANGED (no further retry)");

	/* Exactly one retry was armed. */
	i915_model_sleep(20u);
	drv_i915_hpd_summary(model_display, &summary);
	passed = 0;
	if (summary.hotplug_records == base + 2u && summary.retries_armed == 1u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-RETRY exactly one retry");
}

/* Swaps the sink's EDID behind the same live status and checks the change. */
static void
i915_edid_change_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	const struct i915_hpd_hotplug_record *record;
	unsigned base;
	int ran;
	int passed;

	/* A new serial number in the sink, then a long pulse on B. */
	drv_i915_hpd_summary(model_display, &summary);
	base = summary.hotplug_records;
	i915_make_edid(0x2u);
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);

	/* The detection sees the new EDID as a change. */
	ran = i915_wait_records(base + 1u, 100u);
	drv_i915_ktest_check(ktest, ran, "hpd: HPD-EDIDCHG the hotplug work ran");
	record = drv_i915_hpd_hotplug_record(model_display, base);
	passed = 0;
	if (record != NULL &&
	    record->old_status == I915_TEST_STATUS_CONNECTED &&
	    record->new_status == I915_TEST_STATUS_CONNECTED &&
	    record->state == I915_TEST_HOTPLUG_CHANGED)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-EDIDCHG connected -> connected, CHANGED (the EDID changed: epoch +1)");

	/* The epoch moved and no retry followed the change. */
	i915_model_sleep(20u);
	drv_i915_hpd_summary(model_display, &summary);
	passed = 0;
	if (summary.hdmi_epoch == 3ull && summary.hotplug_records == base + 1u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-EDIDCHG epoch 3, no retry after a change");
}

/* Clears B's live status and checks the disconnect without a DDC access. */
static void
i915_unplug_tests(
	struct i915_ktest *ktest,
	int hdmi)
{
	struct i915_hpd_summary summary;
	const struct i915_hpd_hotplug_record *record;
	unsigned base;
	unsigned reads_before;
	int status;
	int ran;
	int passed;

	/* B's live status goes away; a long pulse on B. */
	drv_i915_hpd_summary(model_display, &summary);
	base = summary.hotplug_records;
	reads_before = model_fake.gm_reads;
	model_fake.sdeisr = I915_TEST_SDE_DDI_A;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);

	/* The detection finds the sink gone. */
	ran = i915_wait_records(base + 1u, 100u);
	drv_i915_ktest_check(ktest, ran, "hpd: HPD-UNPLUG the hotplug work ran");
	record = drv_i915_hpd_hotplug_record(model_display, base);
	passed = 0;
	if (record != NULL &&
	    record->old_status == I915_TEST_STATUS_CONNECTED &&
	    record->new_status == I915_TEST_STATUS_DISCONNECTED &&
	    record->state == I915_TEST_HOTPLUG_CHANGED &&
	    record->live == 0)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-UNPLUG connected -> disconnected, CHANGED");

	/* The live status gate kept the detection off the DDC. */
	drv_i915_hpd_summary(model_display, &summary);
	status = drv_i915_hpd_connector_status(model_display, (unsigned)hdmi);
	passed = 0;
	if (summary.to_disconnected == 1u &&
	    status == I915_TEST_STATUS_DISCONNECTED &&
	    model_fake.gm_reads == reads_before)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-UNPLUG status disconnected, the DDC not touched (live status gate)");
}

/* Sets B live with no sink answering on the DDC and checks the dropped EDID. */
static void
i915_noddc_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	const struct i915_hpd_hotplug_record *record;
	unsigned base;
	int ran;
	int passed;

	/* B live, nobody on the DDC, a long pulse on B. */
	i915_model_sleep(20u);
	drv_i915_hpd_summary(model_display, &summary);
	base = summary.hotplug_records;
	model_fake.ddc_present = 0;
	model_fake.sdeisr = I915_TEST_SDE_DDI_A | I915_TEST_SDE_DDI_B;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);
	ran = i915_wait_records(base + 1u, 300u);
	drv_i915_ktest_check(ktest, ran, "hpd: HPD-NODDC the detection ran");

	/*
	 * The EDID read fails (NAK at 0x50, the first message retried once,
	 * then the bit-banging step): the stored EDID goes away
	 * (drm_edid_connector_update(NULL)), so the epoch moves although the
	 * status stays disconnected.
	 */
	record = drv_i915_hpd_hotplug_record(model_display, base);
	passed = 0;
	if (record != NULL &&
	    record->old_status == I915_TEST_STATUS_DISCONNECTED &&
	    record->new_status == I915_TEST_STATUS_DISCONNECTED &&
	    record->state == I915_TEST_HOTPLUG_CHANGED &&
	    record->live == 1 &&
	    model_fake.gm_naks == 2u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-NODDC NAK at 0x50 (retried once): disconnected, EDID dropped -> epoch +1, CHANGED");

	/* Puts the sink back and B's live status away. */
	model_fake.sdeisr = I915_TEST_SDE_DDI_A;
	model_fake.ddc_present = 1;
	i915_model_sleep(20u);
}

/* Pulses the eDP pin and checks that the dig-port work, not the hotplug work, takes it. */
static void
i915_edp_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	unsigned base;
	int passed;

	/* A long pulse on A. */
	i915_model_sleep(20u);
	drv_i915_hpd_summary(model_display, &summary);
	base = summary.hotplug_records;
	model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_A_LONG;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_A);
	i915_model_sleep(30u);

	/* The dig-port work ran the hpd_pulse step; no hotplug record was added. */
	drv_i915_hpd_summary(model_display, &summary);
	passed = 0;
	if (summary.digport_works >= 1u &&
	    summary.hpd_pulse_steps == 1u &&
	    summary.hotplug_records == base)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-EDP pin 4 goes to the dig-port work (hpd_pulse), not to the hotplug work");
}

/* Makes a storm on pin B and checks the switch to polling and the re-enable. */
static void
i915_storm_tests(
	struct i915_ktest *ktest,
	int hdmi)
{
	struct i915_hpd_summary summary;
	unsigned i;
	int pin_state;
	int polled;
	int flushed;
	int passed;

	/*
	 * Pulses until the pin leaves ENABLED (at most 6: 6 x 10 > 50) and no
	 * more after that, so no pulse meets a DISABLED pin.
	 */
	for (i = 0u; i < I915_TEST_STORM_PULSES; i++) {
		pin_state = drv_i915_hpd_pin_state(model_display, I915_HPD_PORT_B);
		if (pin_state != I915_HPD_ENABLED)
			break;
		model_fake.shotplug_ddi = I915_TEST_SHOTPLUG_B_LONG;
		drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);
	}

	/* The pin is no longer enabled, and irq_setup masked it. */
	pin_state = drv_i915_hpd_pin_state(model_display, I915_HPD_PORT_B);
	passed = 0;
	if (pin_state != I915_HPD_ENABLED && model_hotplug.state[I915_HPD_PORT_B] != I915_HPD_ENABLED)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-STORM pin 5 no longer enabled, irq_setup masked it");

	/* The hotplug work switches the pin to polling when it next runs (it may be busy with a retry). */
	for (i = 0u; i < I915_TEST_POLL_TRIES; i++) {
		pin_state = drv_i915_hpd_pin_state(model_display, I915_HPD_PORT_B);
		if (pin_state == I915_HPD_DISABLED)
			break;
		i915_model_sleep(I915_TEST_POLL_TICKS);
	}

	/* HDMI-A-1 now polls for connect and disconnect, and the pin is DISABLED. */
	drv_i915_hpd_summary(model_display, &summary);
	pin_state = drv_i915_hpd_pin_state(model_display, I915_HPD_PORT_B);
	polled = drv_i915_hpd_connector_polled(model_display, (unsigned)hdmi);
	passed = 0;
	if (pin_state == I915_HPD_DISABLED &&
	    polled == I915_TEST_POLL_CONNECT_DISCONNECT &&
	    summary.storms == 1u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-STORM the work switched HDMI-A-1 to polling (connect | disconnect), pin DISABLED");

	/* The same work arms the re-enable work just after the switch: flushes it once it is armed. */
	flushed = 0;
	for (i = 0u; i < I915_TEST_POLL_TRIES; i++) {
		flushed = drv_i915_test_hpd_flush_reenable(model_display);
		if (flushed == 1)
			break;
		i915_model_sleep(I915_TEST_POLL_TICKS);
	}
	drv_i915_ktest_check(ktest, flushed == 1, "hpd: HPD-STORM the armed re-enable work ran (flushed)");

	/* The re-enable restored HPD: the pin, the connector's polling mode and the register state. */
	drv_i915_hpd_summary(model_display, &summary);
	pin_state = drv_i915_hpd_pin_state(model_display, I915_HPD_PORT_B);
	polled = drv_i915_hpd_connector_polled(model_display, (unsigned)hdmi);
	passed = 0;
	if (pin_state == I915_HPD_ENABLED &&
	    polled == I915_TEST_POLL_HPD &&
	    model_hotplug.state[I915_HPD_PORT_B] == I915_HPD_ENABLED &&
	    summary.reenable_works == 1u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-STORM re-enabled: pin ENABLED, HPD polling mode, register state enabled");

	/* The storm's hotplug works may still be retrying: lets them finish before the stop. */
	i915_model_sleep(150u);
}

/* Stops the path and checks that a later interrupt is dropped without a register access. */
static void
i915_gate_tests(
	struct i915_ktest *ktest)
{
	struct i915_hpd_summary summary;
	unsigned writes_before;
	int passed;

	/* Stops the path, then raises one more interrupt. */
	drv_i915_hpd_stop(model_display);
	writes_before = model_fake.rmw_writes;
	drv_i915_test_hpd_model_irq(model_display, I915_TEST_SDE_DDI_B);

	/* The closed entry dropped it, and nothing in the run warned. */
	drv_i915_hpd_summary(model_display, &summary);
	passed = 0;
	if (summary.irq_dropped == 1u &&
	    model_fake.rmw_writes == writes_before &&
	    summary.warnings == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "hpd: HPD-GATE after stop the interrupt is dropped; no WARN in the whole run");
}
