/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the hotplug (display/xd-hpd.c) on the register model:
 * the pins of the ports, the arming of the north and south hotplug
 * registers (the 5330's ports, and ports A and B alone with the device's
 * interrupts on and off), the acknowledgment and the length of the
 * pulses, their sorting into pulses and detection events, a storm's mask
 * and its rearming, the detection of an HDMI monitor over GMBUS and of a
 * Type-C DisplayPort sink through the hooks, the retries, and the
 * pulses a connector takes.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/display-internal.h"
#include "../../display/xd-hpd.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The Alder Lake-P of the 5330 and its revision, a Tiger Lake and a Rocket Lake. */
#define I915_XD_HPD_TEST_ADLP		0x46a6U
#define I915_XD_HPD_TEST_ADLP_REV	0x0cU
#define I915_XD_HPD_TEST_TGL		0x9a49U
#define I915_XD_HPD_TEST_RKL		0x4c8aU

/* The north display's hotplug mask, and the address of an EDID on the DDC. */
#define I915_XD_HPD_TEST_DE_IMR		(I915_XD_DE_HPD_ISR + I915_XD_DE_IMR_OFFSET)
#define I915_XD_HPD_TEST_EDID		0x50U

/*
 * What the hooks of a run saw and answer: the power references held, the
 * live state and the probe's answer of the Type-C ports, and the calls.
 */
struct i915_xd_hpd_test {
	struct i915_xd_io io;
	struct i915_xd_hpd hpd;
	struct i915_xd_hpd_env env;
	struct i915_xd_fake_gmbus target;
	int power_held[2];
	int power_refused;
	int tc_live;
	int dp_status;
	int dp_same;
	unsigned dp_probes;
	unsigned dp_resets;
	unsigned links;
	uint8_t dp_edid[128];
};

/* The state of the run of the checks; each group starts it again. */
static struct i915_xd_hpd_test i915_xd_hpd_test_run;

static void i915_xd_hpd_test_setup(uint16_t device, uint8_t revid, int south);
static void i915_xd_hpd_test_edid(uint8_t *edid, uint8_t serial);
static int i915_xd_hpd_test_power(void *ctx, unsigned what, int on);
static int i915_xd_hpd_test_tc_live(void *ctx, unsigned ddi);
static int i915_xd_hpd_test_dp_probe(void *ctx, unsigned ddi, uint8_t *edid, size_t size, unsigned *bytes);
static int i915_xd_hpd_test_dp_pulse(void *ctx, unsigned ddi);
static void i915_xd_hpd_test_dp_reset(void *ctx, unsigned ddi);
static void i915_xd_hpd_test_link(void *ctx, unsigned ddi, int hdmi);
static void i915_xd_hpd_test_5330(void);
static void i915_xd_hpd_test_pins(void);
static void i915_xd_hpd_test_arm(void);
static void i915_xd_hpd_test_arm_two(void);
static void i915_xd_hpd_test_irqs(void);
static void i915_xd_hpd_test_storm(void);
static void i915_xd_hpd_test_hdmi(void);
static void i915_xd_hpd_test_dp(void);
static void i915_xd_hpd_test_pulses(void);

int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_hpd_test_pins();
	i915_xd_hpd_test_arm();
	i915_xd_hpd_test_arm_two();
	i915_xd_hpd_test_irqs();
	i915_xd_hpd_test_storm();
	i915_xd_hpd_test_hdmi();
	i915_xd_hpd_test_dp();
	i915_xd_hpd_test_pulses();

	/* Reports the result. */
	failures = i915_host_report("host_xd_hpd_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model, a window on it, the hotplug of a device's
 * description with a south display of the kind given, and the hooks.
 */
static void
i915_xd_hpd_test_setup(
	uint16_t device,
	uint8_t revid,
	int south)
{
	struct i915_xd_hpd_test *run;
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	run = &i915_xd_hpd_test_run;
	memset(run, 0, sizeof(*run));
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&run->io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&run->io, &hooks);

	/* The hotplug of the device's description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	i915_host_check(error == 0, "setup: the device has a description");
	drv_i915_xd_hpd_init(&run->hpd, choice.platform, south);

	/* The hooks. */
	run->env.ctx = run;
	run->env.power = i915_xd_hpd_test_power;
	run->env.tc_live = i915_xd_hpd_test_tc_live;
	run->env.dp_probe = i915_xd_hpd_test_dp_probe;
	run->env.dp_pulse = i915_xd_hpd_test_dp_pulse;
	run->env.dp_reset = i915_xd_hpd_test_dp_reset;
	run->env.link = i915_xd_hpd_test_link;
}

/* Fills a digital EDID base block with a valid header and checksum, told apart by its serial byte. */
static void
i915_xd_hpd_test_edid(
	uint8_t *edid,
	uint8_t serial)
{
	static const uint8_t header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	unsigned index;
	unsigned sum;

	/* The header, a digital input, the serial, no extension. */
	memset(edid, 0, 128U);
	memcpy(edid, header, sizeof(header));
	edid[12] = serial;
	edid[18] = 1U;
	edid[19] = 4U;
	edid[20] = 0x80U;

	/* The checksum makes the block sum to zero. */
	sum = 0U;
	for (index = 0U; index < 127U; index++)
		sum += edid[index];

	/* The checksum makes the block's bytes sum to 0. */
	edid[127] = (uint8_t)(256U - sum % 256U);
}

/* Takes or gives back a power reference, refused when the run says so (a hook). */
static int
i915_xd_hpd_test_power(
	void *ctx,
	unsigned what,
	int on)
{
	struct i915_xd_hpd_test *run;

	/* The run the hook belongs to. */
	run = ctx;

	/* A refusal holds nothing. */
	if (on && run->power_refused)
		return 1;

	/* Counts the reference. */
	if (on) {
		run->power_held[what]++;
	} else {
		run->power_held[what]--;
	}

	/* Succeeded: the reference is counted. */
	return 0;
}

/* Answers a Type-C port's live state from the run (a hook). */
static int
i915_xd_hpd_test_tc_live(
	void *ctx,
	unsigned ddi)
{
	struct i915_xd_hpd_test *run;

	UNUSED_PARAMETER(ddi);
	run = ctx;

	/* Succeeded: the run's live state. */
	return run->tc_live;
}

/* Answers a sink's probe from the run, with its EDID (a hook). */
static int
i915_xd_hpd_test_dp_probe(
	void *ctx,
	unsigned ddi,
	uint8_t *edid,
	size_t size,
	unsigned *bytes)
{
	struct i915_xd_hpd_test *run;

	UNUSED_PARAMETER(ddi);
	run = ctx;
	run->dp_probes++;

	/* The EDID of a connected sink. */
	*bytes = 0U;
	if (run->dp_status == I915_XD_HPD_CONNECTED && size >= sizeof(run->dp_edid)) {
		memcpy(edid, run->dp_edid, sizeof(run->dp_edid));
		*bytes = sizeof(run->dp_edid);
	}

	/* Succeeded: the run's state. */
	return run->dp_status;
}

/* Answers a short pulse's check from the run (a hook). */
static int
i915_xd_hpd_test_dp_pulse(
	void *ctx,
	unsigned ddi)
{
	struct i915_xd_hpd_test *run;

	UNUSED_PARAMETER(ddi);
	run = ctx;

	/* Succeeded: whether the sink is as it was. */
	return run->dp_same;
}

/* Counts a long pulse's reset of a sink's limits (a hook). */
static void
i915_xd_hpd_test_dp_reset(
	void *ctx,
	unsigned ddi)
{
	struct i915_xd_hpd_test *run;

	UNUSED_PARAMETER(ddi);
	run = ctx;
	run->dp_resets++;
}

/* Counts a look at a port's link (a hook). */
static void
i915_xd_hpd_test_link(
	void *ctx,
	unsigned ddi,
	int hdmi)
{
	struct i915_xd_hpd_test *run;

	UNUSED_PARAMETER(ddi);
	UNUSED_PARAMETER(hdmi);
	run = ctx;
	run->links++;
}

/* Adds the 5330's connectors: the panel on A, HDMI on B, DisplayPort on TC1 to TC4 (DDI 3 to 6). */
static void
i915_xd_hpd_test_5330(void)
{
	struct i915_xd_hpd_test *run;
	unsigned ddi;
	int index;

	/* The run of the test. */
	run = &i915_xd_hpd_test_run;

	/* The panel and HDMI. */
	index = drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_EDP, 0U);
	i915_host_check(index == 0 && strcmp(run->hpd.connector[0].name, "eDP-1") == 0, "5330: the panel is eDP-1");
	index = drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_HDMI, 1U);
	i915_host_check(index == 1 && strcmp(run->hpd.connector[1].name, "HDMI-A-1") == 0 && run->hpd.connector[1].ddc_pin == 2U,
	    "5330: HDMI-A-1 on DDI B, its DDC on GMBUS pin 2");

	/* The four Type-C ports. */
	for (ddi = 3U; ddi <= 6U; ddi++)
		(void)drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_DP, ddi);

	/* Checks the connectors of the 5330's ports. */
	i915_host_check(run->hpd.count == 6U && strcmp(run->hpd.connector[5].name, "DP-4") == 0 && run->hpd.connector[2].tc == 1U,
	    "5330: DP-1 to DP-4 on TC1 to TC4");
}

/* Checks the pins of the ports of Alder Lake-P, Tiger Lake and Rocket Lake. */
static void
i915_xd_hpd_test_pins(void)
{
	struct i915_xd_hpd_test *run;
	const struct i915_xd_platform *platform;

	/* The run of the test. */
	run = &i915_xd_hpd_test_run;

	/* Alder Lake-P: A and B on the DDI pins, DDI 3 to 6 on TC1 to TC4, DDI 2 has none. */
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	platform = run->hpd.platform;
	i915_host_check(drv_i915_xd_hpd_pin_of(platform, 0U) == I915_XD_HPD_PIN_DDI(0U) &&
	    drv_i915_xd_hpd_pin_of(platform, 1U) == I915_XD_HPD_PIN_DDI(1U) &&
	    drv_i915_xd_hpd_pin_of(platform, 3U) == I915_XD_HPD_PIN_TC(0U) &&
	    drv_i915_xd_hpd_pin_of(platform, 6U) == I915_XD_HPD_PIN_TC(3U) &&
	    drv_i915_xd_hpd_pin_of(platform, 2U) == I915_XD_HPD_PIN_NONE,
	    "pins: Alder Lake-P A, B on the DDI pins, TC1..TC4 on the Type-C pins, no DDI C");

	/* The bits: A's south bit 16, TC1's south bit 24 and its north bits 16 and 0. */
	i915_host_check(run->hpd.pin[I915_XD_HPD_PIN_DDI(0U)].south == 0x00010000U &&
	    run->hpd.pin[I915_XD_HPD_PIN_TC(0U)].south == 0x01000000U &&
	    run->hpd.pin[I915_XD_HPD_PIN_TC(0U)].north == 0x00010001U &&
	    run->hpd.pin[I915_XD_HPD_PIN_DDI(0U)].north == 0U,
	    "pins: the north and south bits of A and TC1");

	/* Tiger Lake: DDI 8 is TC6. */
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_TGL, 0U, I915_PCH_TGP);
	i915_host_check(drv_i915_xd_hpd_pin_of(run->hpd.platform, 8U) == I915_XD_HPD_PIN_TC(5U), "pins: Tiger Lake DDI 8 on TC6");

	/* Rocket Lake: its combo DDI 3 is on the Type-C pin TC1. */
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_RKL, 0U, I915_PCH_TGP);
	i915_host_check(drv_i915_xd_hpd_pin_of(run->hpd.platform, 3U) == I915_XD_HPD_PIN_TC(0U), "pins: Rocket Lake DDI 3 on TC1");

	/* A south display before ICP has no south bits. */
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_CNP);
	i915_host_check(run->hpd.pin[I915_XD_HPD_PIN_DDI(0U)].south == 0U && run->hpd.pin[I915_XD_HPD_PIN_TC(0U)].south == 0U,
	    "pins: no south bits before ICP");
}

/* Checks the arming of the 5330's pins over registers the firmware left all enabled and masked. */
static void
i915_xd_hpd_test_arm(void)
{
	struct i915_xd_hpd_test *run;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();

	/* The firmware's registers. */
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_DDI, 0x8888U);
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_TC, 0x00888888U);
	i915_xd_fake_set(I915_XD_TC_HOTPLUG_CTL, 0x00888888U);
	i915_xd_fake_set(I915_XD_TBT_HOTPLUG_CTL, 0x00888888U);
	i915_xd_fake_set(I915_XD_SDE_IMR, 0xffffffffU);
	i915_xd_fake_set(I915_XD_HPD_TEST_DE_IMR, 0xffffffffU);

	/* Arms with the device's interrupts on. */
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 1);

	/* A, B and TC1..TC4 enabled, the others cleared; their interrupts unmasked; the filter of ADP. */
	i915_host_check(i915_xd_fake_get(I915_XD_SHOTPLUG_CTL_DDI) == 0x88U, "arm 5330: SHOTPLUG_CTL_DDI enables A and B");
	i915_host_check(i915_xd_fake_get(I915_XD_SHOTPLUG_CTL_TC) == 0x8888U, "arm 5330: SHOTPLUG_CTL_TC enables TC1..TC4");
	i915_host_check(i915_xd_fake_get(I915_XD_TC_HOTPLUG_CTL) == 0x8888U && i915_xd_fake_get(I915_XD_TBT_HOTPLUG_CTL) == 0x8888U,
	    "arm 5330: the Type-C and Thunderbolt controls enable TC1..TC4");
	i915_host_check(i915_xd_fake_get(I915_XD_SDE_IMR) == 0xf0fcffffU, "arm 5330: SDEIMR unmasks A, B and TC1..TC4");
	i915_host_check(i915_xd_fake_get(I915_XD_HPD_TEST_DE_IMR) == 0xfff0fff0U, "arm 5330: the north mask unmasks TC1..TC4 and TBT1..TBT4");
	i915_host_check(i915_xd_fake_get(I915_XD_SHPD_FILTER_CNT) == I915_XD_SHPD_FILTER_250US, "arm 5330: the 250 us filter of ADP");
	i915_host_check(run->hpd.words.south_used == 0x0f030000U && run->hpd.words.north_used == 0x000f000fU && run->hpd.words.sde_imr_written == 1,
	    "arm 5330: the used bits");
}

/*
 * Checks the arming of ports A and B alone: the south display's A and B
 * enabled, the Type-C controls cleared, the north mask untouched (nothing
 * of it is used), the filter written; again with the device's interrupts
 * off, when SDEIMR is not written; and Tiger Lake's filter.
 */
static void
i915_xd_hpd_test_arm_two(void)
{
	struct i915_xd_hpd_test *run;
	unsigned writes;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	(void)drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_EDP, 0U);
	(void)drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_HDMI, 1U);

	/* The firmware's registers. */
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_DDI, 0x8888U);
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_TC, 0x00888888U);
	i915_xd_fake_set(I915_XD_TC_HOTPLUG_CTL, 0x00888888U);
	i915_xd_fake_set(I915_XD_TBT_HOTPLUG_CTL, 0x00888888U);
	i915_xd_fake_set(I915_XD_SDE_IMR, 0xffffffffU);
	i915_xd_fake_set(I915_XD_HPD_TEST_DE_IMR, 0xffffffffU);

	/* Arms with the device's interrupts on. */
	writes = i915_xd_fake_writes();
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 1);
	i915_host_check(i915_xd_fake_get(I915_XD_SHOTPLUG_CTL_DDI) == 0x88U && i915_xd_fake_get(I915_XD_SHOTPLUG_CTL_TC) == 0U,
	    "arm A/B: SHOTPLUG_CTL_DDI enables A and B, SHOTPLUG_CTL_TC cleared");
	i915_host_check(i915_xd_fake_get(I915_XD_TC_HOTPLUG_CTL) == 0U && i915_xd_fake_get(I915_XD_TBT_HOTPLUG_CTL) == 0U,
	    "arm A/B: the Type-C and Thunderbolt controls cleared");
	i915_host_check(i915_xd_fake_get(I915_XD_SDE_IMR) == 0xfffcffffU && run->hpd.words.south_used == 0x30000U && run->hpd.words.south_enabled == 0x30000U,
	    "arm A/B: SDEIMR unmasks A and B");
	i915_host_check(i915_xd_fake_get(I915_XD_HPD_TEST_DE_IMR) == 0xffffffffU && run->hpd.words.north_used == 0U,
	    "arm A/B: the north mask is left as it was");
	i915_host_check(i915_xd_fake_get(I915_XD_SHPD_FILTER_CNT) == I915_XD_SHPD_FILTER_250US && i915_xd_fake_writes() > writes,
	    "arm A/B: the filter of ADP is written");

	/* Again before the device takes interrupts: SDEIMR is left as it is. */
	i915_xd_fake_set(I915_XD_SDE_IMR, 0xffffffffU);
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 0);
	i915_host_check(i915_xd_fake_get(I915_XD_SDE_IMR) == 0xffffffffU && run->hpd.words.sde_imr_written == 0,
	    "arm A/B: SDEIMR is not written while the device takes no interrupts");

	/* Tiger Lake's south display (TGP) takes the adjusted 500 us filter. */
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_TGL, 0U, I915_PCH_TGP);
	(void)drv_i915_xd_hpd_add(&run->hpd, I915_XD_HPD_EDP, 0U);
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 1);
	i915_host_check(i915_xd_fake_get(I915_XD_SHPD_FILTER_CNT) == I915_XD_SHPD_FILTER_500US_ADJUSTED, "arm: TGP's adjusted 500 us filter");
}

/* Checks the acknowledgment of the south and north pulses, their lengths, and their sorting. */
static void
i915_xd_hpd_test_irqs(void)
{
	struct i915_xd_hpd_test *run;
	uint32_t pins;
	uint32_t longs;
	unsigned queue;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 1);

	/* A long pulse on B: acknowledged by writing back the control, long. */
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_DDI, 0x88U | I915_XD_SHOTPLUG_LONG(1U));
	drv_i915_xd_hpd_south_irq(&run->hpd, &run->io, I915_XD_SDE_DDI_HPD(1U), &pins, &longs);
	i915_host_check(pins == (1U << I915_XD_HPD_PIN_DDI(1U)) && longs == pins, "irq: a long pulse on B");
	i915_host_check(i915_xd_fake_previous() == (0x88U | I915_XD_SHOTPLUG_LONG(1U)), "irq: B's pulse acknowledged by writing back what was read");

	/* It is HDMI's: a detection event. */
	queue = drv_i915_xd_hpd_sort(&run->hpd, &run->io, pins, longs, 5000U);
	i915_host_check(queue == I915_XD_HPD_QUEUE_DETECT && run->hpd.events == pins && run->hpd.long_ports == 0U,
	    "irq: HDMI's pulse is a detection event");

	/* A short pulse on TC2 in the south display. */
	run->hpd.events = 0U;
	i915_xd_fake_set(I915_XD_SHOTPLUG_CTL_TC, 0x8888U | I915_XD_SHOTPLUG_SHORT(1U));
	drv_i915_xd_hpd_south_irq(&run->hpd, &run->io, I915_XD_SDE_TC_HPD(1U), &pins, &longs);
	i915_host_check(pins == (1U << I915_XD_HPD_PIN_TC(1U)) && longs == 0U, "irq: a short pulse on TC2");

	/* It is DP-2's: a short pulse for DDI 4's connector, no event. */
	queue = drv_i915_xd_hpd_sort(&run->hpd, &run->io, pins, longs, 5000U);
	i915_host_check(queue == I915_XD_HPD_QUEUE_PULSES && run->hpd.short_ports == (1U << 4U) && run->hpd.events == 0U,
	    "irq: DP-2's short pulse is a pulse of DDI 4");

	/* A long pulse on TC1 in the north display (its Type-C bit), and one on TC3's Thunderbolt bit. */
	run->hpd.short_ports = 0U;
	i915_xd_fake_set(I915_XD_TC_HOTPLUG_CTL, 0x8888U | I915_XD_HOTPLUG_CTL_LONG(0U));
	i915_xd_fake_set(I915_XD_TBT_HOTPLUG_CTL, 0x8888U | I915_XD_HOTPLUG_CTL_SHORT(2U));
	drv_i915_xd_hpd_north_irq(&run->hpd, &run->io, I915_XD_HPD_INT_TC(0U) | I915_XD_HPD_INT_TBT(2U), &pins, &longs);
	i915_host_check(pins == ((1U << I915_XD_HPD_PIN_TC(0U)) | (1U << I915_XD_HPD_PIN_TC(2U))) && longs == (1U << I915_XD_HPD_PIN_TC(0U)),
	    "irq: north pulses on TC1 (long) and TC3 (short)");

	/* Sorts the pulses into long and short ports. */
	queue = drv_i915_xd_hpd_sort(&run->hpd, &run->io, pins, longs, 5000U);
	i915_host_check(queue == I915_XD_HPD_QUEUE_PULSES && run->hpd.long_ports == (1U << 3U) && run->hpd.short_ports == (1U << 5U),
	    "irq: DP-1's long pulse and DP-3's short one");

	/* A north interrupt with no pin is counted. */
	drv_i915_xd_hpd_north_irq(&run->hpd, &run->io, 0x80000000U, &pins, &longs);
	i915_host_check(pins == 0U && run->hpd.unexpected == 1U, "irq: a north interrupt with no pin is counted");

	/* The GMBUS controller's interrupt is counted. */
	drv_i915_xd_hpd_south_irq(&run->hpd, &run->io, I915_XD_SDE_GMBUS, &pins, &longs);
	i915_host_check(pins == 0U && run->hpd.gmbus_irqs == 1U, "irq: the GMBUS interrupt is counted");
}

/* Checks a storm: six long events in a second mask HDMI's pin, which comes back armed. */
static void
i915_xd_hpd_test_storm(void)
{
	struct i915_xd_hpd_test *run;
	uint32_t pin;
	unsigned queue;
	unsigned index;
	int masked;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();
	i915_xd_fake_set(I915_XD_SDE_IMR, 0xffffffffU);
	drv_i915_xd_hpd_arm(&run->hpd, &run->io, 1);

	/* Five long pulses count 50: no storm yet. */
	pin = 1U << I915_XD_HPD_PIN_DDI(1U);
	queue = 0U;
	for (index = 0U; index < 5U; index++)
		queue |= drv_i915_xd_hpd_sort(&run->hpd, &run->io, pin, pin, 10000U + index);

	/* Checks the pulses below the limit. */
	i915_host_check((queue & I915_XD_HPD_STORM) == 0U && run->hpd.pin[I915_XD_HPD_PIN_DDI(1U)].count == 50, "storm: 50 within the limit");

	/* The sixth in the same second is a storm: the pin is marked and masked, its event dropped. */
	queue = drv_i915_xd_hpd_sort(&run->hpd, &run->io, pin, pin, 10500U);
	i915_host_check((queue & I915_XD_HPD_STORM) != 0U && run->hpd.pin[I915_XD_HPD_PIN_DDI(1U)].state == I915_XD_HPD_PIN_MARKED,
	    "storm: the sixth long pulse marks the pin");
	i915_host_check((run->hpd.events & pin) == 0U && (i915_xd_fake_get(I915_XD_SDE_IMR) & I915_XD_SDE_DDI_HPD(1U)) != 0U,
	    "storm: B's event is dropped and B is masked");

	/* A marked pin takes no more events. */
	run->hpd.events = 0U;
	queue = drv_i915_xd_hpd_sort(&run->hpd, &run->io, pin, pin, 10600U);
	i915_host_check(queue == 0U && run->hpd.events == 0U, "storm: a marked pin takes no event");

	/* The detection masks it for good; a pulse after the period on another pin starts a new count. */
	masked = drv_i915_xd_hpd_storms(&run->hpd);
	i915_host_check(masked == 1 && run->hpd.pin[I915_XD_HPD_PIN_DDI(1U)].state == I915_XD_HPD_PIN_DISABLED, "storm: the pin is masked");

	/* Two minutes later it is armed again. */
	drv_i915_xd_hpd_rearm(&run->hpd, &run->io);
	i915_host_check(run->hpd.pin[I915_XD_HPD_PIN_DDI(1U)].state == I915_XD_HPD_PIN_ENABLED && (i915_xd_fake_get(I915_XD_SDE_IMR) & I915_XD_SDE_DDI_HPD(1U)) == 0U,
	    "storm: rearmed, B is unmasked");

	/* Short pulses never storm. */
	pin = 1U << I915_XD_HPD_PIN_TC(0U);
	queue = 0U;
	for (index = 0U; index < 80U; index++)
		queue |= drv_i915_xd_hpd_sort(&run->hpd, &run->io, pin, 0U, 20000U);

	/* Checks the short pulses. */
	i915_host_check((queue & I915_XD_HPD_STORM) == 0U, "storm: short pulses are not counted");
}

/* Checks an HDMI monitor's detection: live and a digital EDID over GMBUS, then unplugged; the retries. */
static void
i915_xd_hpd_test_hdmi(void)
{
	struct i915_xd_hpd_test *run;
	int status;
	int state;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();

	/* A monitor on B's DDC. */
	i915_xd_hpd_test_edid(run->target.memory, 0x11U);
	i915_xd_fake_gmbus_attach(&run->target, I915_XD_HPD_TEST_EDID);

	/* Nothing plugged in: disconnected without a GMBUS cycle. */
	i915_xd_fake_set(I915_XD_SDE_ISR, 0U);
	status = drv_i915_xd_hpd_probe(&run->hpd, &run->io, &run->env, 1U);
	i915_host_check(status == I915_XD_HPD_DISCONNECTED && run->target.cycles == 0U && run->hpd.connector[1].edid_size == 0U,
	    "hdmi: nothing plugged in is disconnected, no EDID read");
	i915_host_check(run->power_held[0] == 0 && run->power_held[1] == 0, "hdmi: the power references are given back");

	/* Plugged in: connected with its EDID; the epoch moved. */
	i915_xd_fake_set(I915_XD_SDE_ISR, I915_XD_SDE_DDI_HPD(1U));
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 1U, 1);
	i915_host_check(state == I915_XD_HPD_CHANGED && run->hpd.connector[1].status == I915_XD_HPD_CONNECTED && run->hpd.connector[1].edid_size == 128U &&
	    run->hpd.connector[1].edid[12] == 0x11U && run->hpd.connector[1].live == 1,
	    "hdmi: plugged in, connected with its EDID over GMBUS");
	i915_host_check(run->links == 1U && run->power_held[0] == 0 && run->power_held[1] == 0, "hdmi: the link is looked at, the power given back");

	/* The same again: one retry, then unchanged. */
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 1U, 1);
	i915_host_check(state == I915_XD_HPD_RETRY, "hdmi: nothing new on an event: a retry");
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 1U, 0);
	i915_host_check(state == I915_XD_HPD_UNCHANGED && run->hpd.connector[1].retries == 1U, "hdmi: nothing new on the retry: unchanged");

	/* Another monitor plugged in its place: changed (the EDID). */
	i915_xd_hpd_test_edid(run->target.memory, 0x22U);
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 1U, 1);
	i915_host_check(state == I915_XD_HPD_CHANGED && run->hpd.connector[1].status == I915_XD_HPD_CONNECTED && run->hpd.connector[1].edid[12] == 0x22U,
	    "hdmi: another monitor is a change");

	/* Unplugged: disconnected, the EDID forgotten. */
	i915_xd_fake_set(I915_XD_SDE_ISR, 0U);
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 1U, 1);
	i915_host_check(state == I915_XD_HPD_CHANGED && run->hpd.connector[1].status == I915_XD_HPD_DISCONNECTED && run->hpd.connector[1].edid_size == 0U,
	    "hdmi: unplugged is a change to disconnected");

	/* A live port whose monitor does not answer: disconnected. */
	i915_xd_fake_set(I915_XD_SDE_ISR, I915_XD_SDE_DDI_HPD(1U));
	i915_xd_fake_gmbus_script(&run->target, I915_XD_FAKE_GMBUS_NAK, 4U);
	status = drv_i915_xd_hpd_probe(&run->hpd, &run->io, &run->env, 1U);
	i915_host_check(status == I915_XD_HPD_DISCONNECTED, "hdmi: a monitor that does not answer is disconnected");

	/* A refused power reference: disconnected without a look. */
	run->power_refused = 1;
	status = drv_i915_xd_hpd_probe(&run->hpd, &run->io, &run->env, 1U);
	i915_host_check(status == I915_XD_HPD_DISCONNECTED && run->power_held[1] == 0, "hdmi: a refused power reference reads nothing");
}

/* Checks a Type-C DisplayPort sink's detection through the hooks, and its five retries. */
static void
i915_xd_hpd_test_dp(void)
{
	struct i915_xd_hpd_test *run;
	unsigned index;
	int state;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();

	/* Nothing on TC2: disconnected without a probe. */
	run->tc_live = 0;
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 3U, 1);
	i915_host_check(state == I915_XD_HPD_CHANGED && run->hpd.connector[3].status == I915_XD_HPD_DISCONNECTED && run->dp_probes == 0U,
	    "dp: nothing on TC2 is disconnected");

	/* A monitor: connected with the probe's EDID. */
	run->tc_live = 1;
	run->dp_status = I915_XD_HPD_CONNECTED;
	i915_xd_hpd_test_edid(run->dp_edid, 0x33U);
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 3U, 1);
	i915_host_check(state == I915_XD_HPD_CHANGED && run->hpd.connector[3].status == I915_XD_HPD_CONNECTED && run->hpd.connector[3].edid_size == 128U &&
	    run->dp_probes == 1U,
	    "dp: a monitor on TC2 is connected with its EDID");

	/* Nothing new: five retries on a Type-C port, then unchanged. */
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 3U, 1);
	for (index = 0U; index < 4U && state == I915_XD_HPD_RETRY; index++)
		state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 3U, 0);

	/* Checks the retries of a Type-C port. */
	i915_host_check(state == I915_XD_HPD_RETRY && run->hpd.connector[3].retries == 4U, "dp: a Type-C port retries up to five times");
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 3U, 0);
	i915_host_check(state == I915_XD_HPD_UNCHANGED, "dp: unchanged after the fifth retry");

	/* The panel is never detected. */
	state = drv_i915_xd_hpd_detect(&run->hpd, &run->io, &run->env, 0U, 1);
	i915_host_check(run->hpd.connector[0].status == I915_XD_HPD_UNKNOWN, "dp: the panel stays unknown");

	/* A DisplayPort port that is not Type-C stays unknown (no hook is asked). */
	run->env.tc_live = NULL;
	state = drv_i915_xd_hpd_probe(&run->hpd, &run->io, &run->env, 2U);
	i915_host_check(state == I915_XD_HPD_UNKNOWN, "dp: without the Type-C hooks a DisplayPort connector stays unknown");
}

/* Checks the pulses the connectors take: the panel's ignored, a long one left to the detection, a short one checked. */
static void
i915_xd_hpd_test_pulses(void)
{
	struct i915_xd_hpd_test *run;
	int taken;

	/* The run of the test on the 5330's model. */
	run = &i915_xd_hpd_test_run;
	i915_xd_hpd_test_setup(I915_XD_HPD_TEST_ADLP, I915_XD_HPD_TEST_ADLP_REV, I915_PCH_ADP);
	i915_xd_hpd_test_5330();

	/* The panel's pulse is taken and ignored. */
	taken = drv_i915_xd_hpd_pulse(&run->hpd, &run->env, 0U, 1);
	i915_host_check(taken == 1 && run->dp_resets == 0U, "pulse: the panel's pulse is ignored");

	/* A long one resets the sink's limits and is left to the detection. */
	taken = drv_i915_xd_hpd_pulse(&run->hpd, &run->env, 2U, 1);
	i915_host_check(taken == 0 && run->dp_resets == 1U, "pulse: a long pulse is left to the detection");

	/* A short one of a sink that is as it was is taken; one that changed is left to the detection. */
	run->dp_same = 1;
	taken = drv_i915_xd_hpd_pulse(&run->hpd, &run->env, 2U, 0);
	i915_host_check(taken == 1, "pulse: a short pulse of an unchanged sink is taken");
	run->dp_same = 0;
	taken = drv_i915_xd_hpd_pulse(&run->hpd, &run->env, 2U, 0);
	i915_host_check(taken == 0, "pulse: a short pulse of a changed sink is left to the detection");
}
