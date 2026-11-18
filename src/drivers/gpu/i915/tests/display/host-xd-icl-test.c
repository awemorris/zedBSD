/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display 11 (Ice Lake) parts of the xd display, on
 * the register model (xd-fake-regs.c): the platform rows; the MG PLL's
 * words against values Linux's own calculation gives, its enable (the
 * order of the writes, the masks of a 38.4 MHz reference) and its read;
 * the MG PHY's DisplayPort mode and transmit swing; the single FIA, what
 * keeps a port out of TC cold and the connection of a legacy and a
 * DisplayPort alternate mode port; the AUX wells (the TC cold exit of a
 * legacy connector, the combo PHY's AUX lane and display WA #1178, the
 * Thunderbolt AUX well of a port); the eDP transcoder's DDI function,
 * setup and clock select, the port's DisplayPort transport; the CD
 * clock's bypass, the Thunderbolt PLL's words and the data box credits;
 * and the load of a version 1 DMC package.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-dbuf.h"
#include "../../display/xd-ddi.h"
#include "../../display/xd-dmc.h"
#include "../../display/xd-io.h"
#include "../../display/xd-pipe.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-power.h"
#include "../../display/xd-tc.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* An Ice Lake with port F (stepping D0), and one without. */
#define I915_XD_ICL_TEST_DEVICE		0x8a52U
#define I915_XD_ICL_TEST_DEVICE_NO_F	0x8a51U
#define I915_XD_ICL_TEST_REVID		0x07U

/* The most writes one check reads back. */
#define I915_XD_ICL_TEST_WRITES		128U

/* The size of a made-up DMC package and of the owner's storage. */
#define I915_XD_ICL_TEST_FILE		4096U
#define I915_XD_ICL_TEST_STORE		1024U

/* The register model's PLL enables of the four MG PLLs. */
#define I915_XD_ICL_TEST_MG_ENABLES	4U

/*
 * The MG PLL words Linux v6.8.12's own calculation (icl_calc_mg_pll_state
 * on display 11, run on the host) gives: DisplayPort RBR on a 38.4 MHz
 * reference, and HDMI's 148.5 MHz on a 24 MHz reference.  Each: the
 * reference input, the clock tops, DIV0, DIV1, the loop filter, the
 * fractional lock, the SSC, the bias and its mask, the TDC cold start and
 * its mask.
 */
static const struct i915_xd_mg_pll_words i915_xd_icl_test_rbr_38p4 = {
	0x100U, 0xa00U, 0xa200U, 0x5e000069U, 0x21012U, 0x27110804U, 0x55552U, 0x8001200U, 0x0U, 0x0U, 0x10000U, 0x10000U
};

static const struct i915_xd_mg_pll_words i915_xd_icl_test_hdmi_24 = {
	0x100U, 0x500U, 0x19400U, 0x680000b9U, 0x11012U, 0x3f110804U, 0x5554aU, 0x8001200U, 0xff0a8c99U, 0xffffffffU, 0x8050007U,
	0xffffffffU
};

/*
 * The owner's wells as a connection used them: the sets got and put, and
 * how many times.
 */
static struct {
	uint64_t got;
	uint64_t put;
	unsigned gets;
	unsigned puts;
} i915_xd_icl_test_power;

/*
 * The answers of the owner of the wells: whether the Type-C port has a
 * legacy connector, whether a DDI drives an eDP panel, and how many
 * times the Dekel health hook was called.
 */
static struct {
	int legacy;
	unsigned edp_ddi;
	unsigned health_calls;
} i915_xd_icl_test_owner;

/* The firmware's mailbox: how many TC cold exits it was asked for. */
static unsigned i915_xd_icl_test_exits;

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_icl_test_io;

/* A made-up DMC package and the owner's storage of its programs. */
static uint8_t i915_xd_icl_test_file[I915_XD_ICL_TEST_FILE];
static uint8_t i915_xd_icl_test_store[I915_XD_ICL_TEST_STORE];

static const struct i915_xd_platform *i915_xd_icl_test_setup(uint16_t device);
static unsigned i915_xd_icl_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static int i915_xd_icl_test_index(const uint32_t *offsets, unsigned count, uint32_t offset);
static int i915_xd_icl_test_words_equal(const struct i915_xd_mg_pll_words *a, const struct i915_xd_mg_pll_words *b);
static void i915_xd_icl_test_put32(uint8_t *bytes, size_t offset, uint32_t value);
static int i915_xd_icl_test_get(void *context, uint64_t functions);
static void i915_xd_icl_test_put(void *context, uint64_t functions);
static int i915_xd_icl_test_legacy(void *context, unsigned tc_port);
static int i915_xd_icl_test_edp(void *context, unsigned ddi);
static int i915_xd_icl_test_health(void *context, unsigned tc_port);
static void i915_xd_icl_test_mailbox(uint32_t offset, uint32_t value, void *context);
static void i915_xd_icl_test_rows(void);
static void i915_xd_icl_test_mg_pll(void);
static void i915_xd_icl_test_mg_phy(void);
static void i915_xd_icl_test_tc(void);
static void i915_xd_icl_test_wells(void);
static void i915_xd_icl_test_trans(void);
static void i915_xd_icl_test_clocks(void);
static void i915_xd_icl_test_dmc(void);

/*
 * Runs every check of the display 11 parts and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_icl_test_rows();
	i915_xd_icl_test_mg_pll();
	i915_xd_icl_test_mg_phy();
	i915_xd_icl_test_tc();
	i915_xd_icl_test_wells();
	i915_xd_icl_test_trans();
	i915_xd_icl_test_clocks();
	i915_xd_icl_test_dmc();

	/* Reports the result. */
	failures = i915_host_report("host_xd_icl_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model whose MG PLL power and lock follow their requests
 * and whose AUX wells' states follow theirs, and returns the description
 * of a device.
 */
static const struct i915_xd_platform *
i915_xd_icl_test_setup(
	uint16_t device)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	unsigned index;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_icl_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_icl_test_io, &hooks);

	/* The MG PLL and combo DPLL enables: the power state follows the request, the lock the enable. */
	for (index = 0U; index < I915_XD_ICL_TEST_MG_ENABLES; index++) {
		i915_xd_fake_rule_mirror(I915_XD_TC_PLL_ENABLE(index), I915_XD_PLL_POWER_ENABLE, I915_XD_TC_PLL_ENABLE(index), -1, 0U);
		i915_xd_fake_rule_mirror(I915_XD_TC_PLL_ENABLE(index), I915_XD_PLL_ENABLE, I915_XD_TC_PLL_ENABLE(index), -1, 0U);
	}

	/* The combo DPLL's enable likewise. */
	i915_xd_fake_rule_mirror(I915_XD_DPLL0_ENABLE, I915_XD_PLL_POWER_ENABLE, I915_XD_DPLL0_ENABLE, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_DPLL0_ENABLE, I915_XD_PLL_ENABLE, I915_XD_DPLL0_ENABLE, -1, 0U);

	/* The AUX wells' states follow their requests; the firmware answers the mailbox. */
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL_AUX, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL_AUX, -1, 0U);
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_icl_test_mailbox, NULL);
	i915_xd_icl_test_exits = 0U;

	/* The description. */
	error = drv_i915_xd_platform_choose(device, I915_XD_ICL_TEST_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Copies the writes the window recorded, oldest first; reports how many. */
static unsigned
i915_xd_icl_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_icl_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count; index++) {
		/* Only writes are kept. */
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
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

/* Reports where the first write to a register is among the writes, or -1. */
static int
i915_xd_icl_test_index(
	const uint32_t *offsets,
	unsigned count,
	uint32_t offset)
{
	unsigned index;

	/* The first write to it. */
	for (index = 0U; index < count; index++) {
		if (offsets[index] == offset)
			return (int)index;
	}

	/* The register was not written. */
	return -1;
}

/* Tells whether two sets of MG PLL words are the same. */
static int
i915_xd_icl_test_words_equal(
	const struct i915_xd_mg_pll_words *a,
	const struct i915_xd_mg_pll_words *b)
{
	/* Every word. */
	if (a->refclkin != b->refclkin ||
	    a->coreclk != b->coreclk ||
	    a->hsclk != b->hsclk)
		return 0;
	if (a->div0 != b->div0 ||
	    a->div1 != b->div1 ||
	    a->lf != b->lf ||
	    a->frac_lock != b->frac_lock)
		return 0;
	if (a->ssc != b->ssc ||
	    a->bias != b->bias ||
	    a->bias_mask != b->bias_mask)
		return 0;
	if (a->tdc != b->tdc || a->tdc_mask != b->tdc_mask)
		return 0;

	/* Succeeded: the same. */
	return 1;
}

/* Stores a little-endian dword. */
static void
i915_xd_icl_test_put32(
	uint8_t *bytes,
	size_t offset,
	uint32_t value)
{
	/* The lowest byte first. */
	bytes[offset] = (uint8_t)value;
	bytes[offset + 1U] = (uint8_t)(value >> 8);
	bytes[offset + 2U] = (uint8_t)(value >> 16);
	bytes[offset + 3U] = (uint8_t)(value >> 24);
}

/* Notes the owner's wells a connection got. */
static int
i915_xd_icl_test_get(
	void *context,
	uint64_t functions)
{
	(void)context;

	/* Notes the set. */
	i915_xd_icl_test_power.got |= functions;
	i915_xd_icl_test_power.gets++;

	/* Succeeded: the wells are up. */
	return 0;
}

/* Notes the owner's wells a connection gave back. */
static void
i915_xd_icl_test_put(
	void *context,
	uint64_t functions)
{
	(void)context;

	/* Notes the set. */
	i915_xd_icl_test_power.put |= functions;
	i915_xd_icl_test_power.puts++;
}

/* Answers whether a Type-C port has a legacy connector. */
static int
i915_xd_icl_test_legacy(
	void *context,
	unsigned tc_port)
{
	(void)context;
	(void)tc_port;

	/* Succeeded: the owner's answer. */
	return i915_xd_icl_test_owner.legacy;
}

/* Answers whether a DDI drives the eDP panel. */
static int
i915_xd_icl_test_edp(
	void *context,
	unsigned ddi)
{
	(void)context;

	/* The panel's DDI only. */
	if (ddi == i915_xd_icl_test_owner.edp_ddi)
		return 1;

	/* Succeeded: not the panel's. */
	return 0;
}

/* Notes a call of the Dekel PHY's health hook, which an MG PHY never needs. */
static int
i915_xd_icl_test_health(
	void *context,
	unsigned tc_port)
{
	(void)context;
	(void)tc_port;

	/* Notes the call. */
	i915_xd_icl_test_owner.health_calls++;

	/* Succeeded: healthy. */
	return 0;
}

/* Answers a mailbox command, counting the TC cold exits. */
static void
i915_xd_icl_test_mailbox(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	(void)offset;
	(void)context;

	/* Only a command. */
	if ((value & I915_XD_GTDRIVER_MAILBOX_BUSY) == 0U)
		return;

	/* Counts an exit and answers. */
	if ((value & 0xffU) == I915_XD_MAILBOX_EXIT_TCCOLD)
		i915_xd_icl_test_exits++;
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, 0U);
}

/*
 * The rows: the stepping, the ports (MG PHYs C..F on the Type-C ports,
 * port F only on the row that has it), the PLLs, the first and the last
 * well, and the Thunderbolt AUX wells' indexes.
 */
static void
i915_xd_icl_test_rows(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	struct i915_xd_choice choice;
	int well;
	int error;

	/* The row with port F, D0 from revision 7. */
	error = drv_i915_xd_platform_choose(I915_XD_ICL_TEST_DEVICE, I915_XD_ICL_TEST_REVID, 1, 0x3480U, &choice);
	platform = choice.platform;
	i915_host_check(error == 0 && platform != NULL && choice.step == I915_XD_STEP_D0 && choice.pch_kind == I915_PCH_ICP &&
		choice.pch_assumed == 0, "rows: an Ice Lake is display 11, D0, beside an ICP");
	if (platform == NULL)
		return;

	/* Port C is TC1 on MG PHY C; port F is TC4. */
	port = drv_i915_xd_platform_port(platform, 2U);
	i915_host_check(port != NULL && port->phy_kind == I915_XD_PHY_MG && port->phy == 2U && port->tc == 1U && port->aux == 2U &&
		port->south_pin_kind == I915_XD_SOUTH_PIN_TC && port->south_pin == 0U, "rows: DDI C is TC1 on MG PHY C");
	port = drv_i915_xd_platform_port(platform, 5U);
	i915_host_check(port != NULL && port->tc == 4U, "rows: DDI F is TC4");

	/* MG PLL 4 is enabled at 0x4603c, after the Thunderbolt PLL. */
	i915_host_check(platform->nplls == 7U && platform->plls[6].kind == I915_XD_PLL_TC && platform->plls[6].enable == 0x4603cU,
		"rows: MG PLL 4's enable is at 0x4603c");

	/* PW_1 first, PW_4 (pipe C) last; Thunderbolt AUX 1 at index 8 of the AUX control. */
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_AUX, 8U);
	i915_host_check(platform->nwells == 21U && platform->wells[0].index == I915_XD_WELL_PW_1 &&
		platform->wells[20].index == I915_XD_WELL_PW_4 && platform->wells[20].irq_pipes == 0x4U && well >= 0 &&
		(platform->wells[well].flags & I915_XD_WELL_TBT_AUX) != 0U, "rows: the wells from PW_1 to PW_4, Thunderbolt AUX 1 at 8");

	/* The row without port F stops at DDI E. */
	error = drv_i915_xd_platform_choose(I915_XD_ICL_TEST_DEVICE_NO_F, I915_XD_ICL_TEST_REVID, 0, 0U, &choice);
	port = NULL;
	if (error == 0)
		port = drv_i915_xd_platform_port(choice.platform, 5U);
	i915_host_check(error == 0 && port == NULL, "rows: an Ice Lake without port F has no DDI F");
}

/*
 * The MG PLL: the words of two links as Linux computes them, the link
 * clocks they give, the enable's order and masks (on a 38.4 MHz
 * reference the bias is kept and the TDC word takes only its cold
 * start), the read back, and the refusal of the Thunderbolt PLL.
 */
static void
i915_xd_icl_test_mg_pll(void)
{
	uint32_t offsets[I915_XD_ICL_TEST_WRITES];
	uint32_t values[I915_XD_ICL_TEST_WRITES];
	const struct i915_xd_platform *platform;
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_mg_pll_words words;
	uint32_t base;
	unsigned count;
	unsigned late;
	int power;
	int refclk;
	int div0;
	int tdc;
	int enable;
	int error;

	/* The words of RBR on 38.4 MHz and of HDMI 148.5 MHz on 24 MHz. */
	error = drv_i915_xd_mg_pll_words(38400U, 162000U, 0, &words);
	i915_host_check(error == 0 && i915_xd_icl_test_words_equal(&words, &i915_xd_icl_test_rbr_38p4) != 0 &&
		drv_i915_xd_mg_pll_port_khz(38400U, &words) == 162000U, "mg pll: RBR on 38.4 MHz has Linux's words and gives 1.62 GHz");
	error = drv_i915_xd_mg_pll_words(24000U, 148500U, 1, &words);
	i915_host_check(error == 0 && i915_xd_icl_test_words_equal(&words, &i915_xd_icl_test_hdmi_24) != 0 &&
		drv_i915_xd_mg_pll_port_khz(24000U, &words) == 148500U, "mg pll: HDMI 148.5 MHz on 24 MHz has Linux's words");
	error = drv_i915_xd_mg_pll_words(25000U, 162000U, 0, &words);
	i915_host_check(error == EINVAL, "mg pll: a reference the PHY does not take is refused");

	/* TC2's MG PLL (select 4) on 38.4 MHz, its PHY's other bits set. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	pll = drv_i915_xd_dpll_desc(platform, 4U);
	base = I915_XD_MG_PHY(1U);
	i915_xd_fake_set(base + I915_XD_MG_REFCLKIN_CTL, 0x1U);
	i915_xd_fake_set(base + I915_XD_MG_PLL_BIAS, 0x12345678U);
	i915_xd_fake_set(base + I915_XD_MG_PLL_TDC_COLDST_BIAS, 0x80000001U);
	late = drv_i915_xd_mg_pll_enable(&i915_xd_icl_test_io, pll, &i915_xd_icl_test_rbr_38p4);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(pll != NULL && pll->enable == 0x46034U && late == 0U, "mg pll: MG PLL 2 at 0x46034 powers and locks in time");

	/* The order: the power, the clock tops, the dividers, the bias and TDC, then the enable. */
	power = i915_xd_icl_test_index(offsets, count, 0x46034U);
	refclk = i915_xd_icl_test_index(offsets, count, base + I915_XD_MG_REFCLKIN_CTL);
	div0 = i915_xd_icl_test_index(offsets, count, base + I915_XD_MG_PLL_DIV0);
	tdc = i915_xd_icl_test_index(offsets, count, base + I915_XD_MG_PLL_TDC_COLDST_BIAS);

	/* The enable is the second write of the enable word, after the power. */
	enable = -1;
	if (power >= 0)
		enable = i915_xd_icl_test_index(offsets + power + 1, count - (unsigned)power - 1U, 0x46034U);

	/* Checks the order. */
	i915_host_check(power == 0 && refclk == 1 && div0 == 4 && tdc == 10 && enable == 10,
		"mg pll: power, the three clock tops, DIV0 to SSC, bias, TDC, then the enable");

	/* The words in place: the clock tops keep the other bits, the bias is kept, the TDC takes its cold start. */
	i915_host_check(i915_xd_fake_get(base + I915_XD_MG_REFCLKIN_CTL) == 0x101U &&
		i915_xd_fake_get(base + I915_XD_MG_CLKTOP2_HSCLKCTL) == 0xa200U &&
		i915_xd_fake_get(base + I915_XD_MG_PLL_DIV0) == 0x5e000069U && i915_xd_fake_get(base + I915_XD_MG_PLL_LF) == 0x27110804U,
		"mg pll: the clock tops and the dividers are written");
	i915_host_check(i915_xd_fake_get(base + I915_XD_MG_PLL_BIAS) == 0x12345678U &&
		i915_xd_fake_get(base + I915_XD_MG_PLL_TDC_COLDST_BIAS) == 0x80010001U,
		"mg pll: on 38.4 MHz the bias is kept and the TDC word takes only its cold start");

	/* Read back with the reference's masks: the same words. */
	memset(&words, 0, sizeof(words));
	enable = drv_i915_xd_mg_pll_read(&i915_xd_icl_test_io, pll, 38400U, &words);
	words.refclkin &= I915_XD_DKL_REFCLKIN_OD_2_MUX;
	i915_host_check(enable == 1 && words.div0 == 0x5e000069U && words.lf == 0x27110804U && words.bias == 0U &&
		words.tdc == 0x10000U && words.refclkin == 0x100U, "mg pll: the read gives the words back");

	/* The Thunderbolt PLL is not an MG PLL. */
	pll = drv_i915_xd_dpll_desc(platform, 2U);
	late = drv_i915_xd_mg_pll_enable(&i915_xd_icl_test_io, pll, &i915_xd_icl_test_rbr_38p4);
	i915_host_check(late == (I915_XD_DPLL_LATE_POWER_ON | I915_XD_DPLL_LATE_LOCK), "mg pll: the Thunderbolt PLL is refused");
}

/*
 * The MG PHY: the DisplayPort mode of the lanes for a pin assignment
 * and a legacy connector, and the transmit swing words of a link at
 * HBR (low-rate lock on, no duty-cycle override) and at HBR2 (the
 * override on).
 */
static void
i915_xd_icl_test_mg_phy(void)
{
	static const uint8_t levels[4] = { 0U, 1U, 4U, 9U };
	const struct i915_xd_mg_swing_table *table;
	const struct i915_xd_platform *platform;
	uint32_t lane0;
	uint32_t lane1;
	int error;

	/* TC1's lanes, with other bits set: assignment C at two lanes doubles both lanes. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	lane0 = I915_XD_MG_PHY(0U) + I915_XD_MG_DP_MODE;
	lane1 = lane0 + I915_XD_MG_LANE_STRIDE;
	i915_xd_fake_set(lane0, 0x1U | I915_XD_DKL_DP_MODE_X1);
	i915_xd_fake_set(lane1, 0x2U);
	error = drv_i915_xd_tc_mg_dp_mode(&i915_xd_icl_test_io, 0U, I915_XD_TC_PIN_C, 2U);
	i915_host_check(error == 0 && i915_xd_fake_get(lane0) == (0x1U | I915_XD_DKL_DP_MODE_X2) &&
		i915_xd_fake_get(lane1) == (0x2U | I915_XD_DKL_DP_MODE_X2), "mg phy: assignment C at two lanes doubles both lanes at 0x1683a0");

	/* A legacy connector at one lane: lane 1's single transmitter. */
	error = drv_i915_xd_tc_mg_dp_mode(&i915_xd_icl_test_io, 0U, I915_XD_TC_PIN_LEGACY, 1U);
	i915_host_check(error == 0 && i915_xd_fake_get(lane0) == 0x1U && i915_xd_fake_get(lane1) == (0x2U | I915_XD_DKL_DP_MODE_X1),
		"mg phy: a legacy connector at one lane uses lane 1's single transmitter");

	/* HBR's table: the swing and drive words of each transmitter, the low-rate lock, no override. */
	i915_xd_fake_set(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_LINK_PARAMS, I915_XD_MG_TX_USE_FS32 | 0x1U);
	i915_xd_fake_set(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_DCC, I915_XD_MG_DCC_AMI_DIV_OVERRIDE | 0x10U);
	table = drv_i915_xd_tc_mg_swing_table(platform, 0, 270000U);
	drv_i915_xd_tc_mg_signal_levels(&i915_xd_icl_test_io, 2U, table, levels, 270000U);
	i915_host_check(table == platform->swing->mg_dp_low && i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_LINK_PARAMS) == 0x1U,
		"mg phy: HBR takes the low table and leaves FS32");
	i915_host_check(i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_DRVCTRL) == 0x18400000U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX2_DRVCTRL) == 0x1d400000U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_LANE_STRIDE + I915_XD_MG_TX1_DRVCTRL) == 0x21400000U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_LANE_STRIDE + I915_XD_MG_TX2_DRVCTRL) == 0x3f400000U,
		"mg phy: the drive words of levels 0, 1, 4 and 9 with the override");
	i915_host_check(i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_SWINGCTRL) == 0U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX2_SWINGCTRL) == 0x05U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_CLKHUB) == I915_XD_MG_CLKHUB_LOW_RATE_LKREN &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_DCC) == 0x10U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX2_PISO_READLOAD) == I915_XD_MG_TX_CALCINIT,
		"mg phy: level 1's swing word, and below 300 MHz the low-rate lock, no duty-cycle override, the PISO's calculation");

	/* HBR2: the high table, no low-rate lock, the override of 1. */
	table = drv_i915_xd_tc_mg_swing_table(platform, 0, 540000U);
	drv_i915_xd_tc_mg_signal_levels(&i915_xd_icl_test_io, 2U, table, levels, 540000U);
	i915_host_check(table == platform->swing->mg_dp_high && i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_CLKHUB) == 0U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_TX1_DCC) == 0x03000010U &&
		i915_xd_fake_get(I915_XD_MG_PHY(2U) + I915_XD_MG_LANE_STRIDE + I915_XD_MG_TX1_DRVCTRL) == 0x26400000U,
		"mg phy: HBR2 takes the high table, no low-rate lock, the duty-cycle override of 1");

	/* HDMI's table defaults to its last row. */
	table = drv_i915_xd_tc_mg_swing_table(platform, 1, 148500U);
	i915_host_check(table == platform->swing->mg_hdmi && table->hdmi_default == 9U && table->rows[9].deemph_11_6 == 0x36U &&
		table->rows[9].deemph_17_12 == 0x09U, "mg phy: HDMI's table defaults to its last row");
}

/*
 * The FIA flow of display 11: FIA 1 serves every port without being
 * asked; TC cold is kept off by a legacy connector's AUX well only; a
 * legacy port connects with its AUX well, a DisplayPort alternate mode
 * port with none.
 */
static void
i915_xd_icl_test_tc(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	struct i915_xd_tc_connection connection;
	struct i915_xd_tc_power power;
	struct i915_xd_tc_fia fia;
	uint64_t functions;
	unsigned reads;
	int error;

	/* FIA 1 holds TC3 at index 2, and is not read even when it says modular. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, I915_XD_DFLEXDPSP_MODULAR);
	reads = i915_xd_fake_reads();
	error = drv_i915_xd_tc_fia(&i915_xd_icl_test_io, platform, 2U, &fia);
	i915_host_check(error == 0 && fia.base == I915_XD_FIA1_BASE && fia.index == 2U && i915_xd_fake_reads() == reads,
		"tc: Ice Lake's FIA 1 holds TC3 at index 2 without being asked");

	/* TC cold: a legacy connector's AUX well, nothing in the other modes. */
	functions = drv_i915_xd_tc_cold_functions(platform, 3U, I915_XD_TC_MODE_LEGACY);
	i915_host_check(functions == I915_XD_POWERS_AUX(3U), "tc: a legacy connector keeps TC cold off with its AUX well");
	functions = drv_i915_xd_tc_cold_functions(platform, 3U, I915_XD_TC_MODE_DP_ALT);
	functions |= drv_i915_xd_tc_cold_functions(platform, 3U, I915_XD_TC_MODE_TBT);
	i915_host_check(functions == 0U, "tc: DisplayPort alternate and Thunderbolt modes need no well");

	/* A legacy port (TC2 on DDI D): ready, owned, four lanes, its AUX well held. */
	port = drv_i915_xd_platform_port(platform, 3U);
	power.get = i915_xd_icl_test_get;
	power.put = i915_xd_icl_test_put;
	power.context = NULL;
	memset(&i915_xd_icl_test_power, 0, sizeof(i915_xd_icl_test_power));
	i915_xd_fake_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPPMS, 0x2U);
	error = drv_i915_xd_tc_connect(&i915_xd_icl_test_io, platform, port, 3U, I915_XD_TC_MODE_LEGACY, 4U, &power, &connection);
	i915_host_check(error == 0 && connection.owned == 1 && connection.max_lanes == 4U && connection.blocked == I915_XD_POWERS_AUX(3U) &&
		i915_xd_icl_test_power.got == I915_XD_POWERS_AUX(3U) &&
		(i915_xd_fake_get(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPCSSS) & 0x2U) != 0U,
		"tc: a legacy port takes its AUX well, then the PHY in FIA 1");
	drv_i915_xd_tc_disconnect(&i915_xd_icl_test_io, platform, port, &power, &connection);
	i915_host_check(i915_xd_icl_test_power.put == I915_XD_POWERS_AUX(3U) &&
		(i915_xd_fake_get(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPCSSS) & 0x2U) == 0U, "tc: the disconnect gives the PHY and the well back");

	/* A DisplayPort alternate mode port with two lanes on assignment D: no well. */
	memset(&i915_xd_icl_test_power, 0, sizeof(i915_xd_icl_test_power));
	i915_xd_fake_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXDPSP, I915_XD_DFLEXDPSP_LIVE_TC(1U) | (0x3U << 8U));
	i915_xd_fake_set(I915_XD_FIA1_BASE + I915_XD_FIA_DFLEXPA1, I915_XD_TC_PIN_D << 4U);
	error = drv_i915_xd_tc_connect(&i915_xd_icl_test_io, platform, port, 3U, I915_XD_TC_MODE_DP_ALT, 2U, &power, &connection);
	i915_host_check(error == 0 && connection.blocked == 0U && i915_xd_icl_test_power.gets == 0U && connection.max_lanes == 2U,
		"tc: a DisplayPort alternate mode port needs no well");
}

/*
 * The AUX wells: a legacy Type-C connector's well asks the firmware to
 * leave TC cold, a DisplayPort alternate mode one does not, and neither
 * waits for a Dekel microcontroller; the Thunderbolt AUX well of TC2
 * routes TC2's channel; combo AUX A of the eDP panel turns its lane on
 * without the analog override, combo AUX B of another port with it; the
 * disable takes the lane off first.
 */
static void
i915_xd_icl_test_wells(void)
{
	uint32_t offsets[I915_XD_ICL_TEST_WRITES];
	uint32_t values[I915_XD_ICL_TEST_WRITES];
	const struct i915_xd_platform *platform;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_well_report report;
	unsigned count;
	int request;
	int lane;
	int well;
	int error;

	/* The owner's hooks: TC2 legacy, DDI A the panel. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.tc_health = i915_xd_icl_test_health;
	hooks.tc_legacy = i915_xd_icl_test_legacy;
	hooks.port_edp = i915_xd_icl_test_edp;
	memset(&i915_xd_icl_test_owner, 0, sizeof(i915_xd_icl_test_owner));
	i915_xd_icl_test_owner.legacy = 1;
	i915_xd_icl_test_owner.edp_ddi = 0U;

	/* AUX D (TC2's channel) of a legacy connector: one exit, the TBT IO cleared, no health wait. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DDI_AUX_CTL(3U), I915_XD_AUX_CTL_TBT_IO);
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_AUX, 3U);
	error = drv_i915_xd_power_well_enable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	i915_host_check(well >= 0 && error == 0 && report.state_late == 0 && i915_xd_icl_test_exits == 1U &&
		i915_xd_icl_test_owner.health_calls == 0U && (i915_xd_fake_get(I915_XD_DDI_AUX_CTL(3U)) & I915_XD_AUX_CTL_TBT_IO) == 0U &&
		(i915_xd_fake_get(I915_XD_PWR_WELL_CTL_AUX) & I915_XD_PWR_WELL_REQUEST(3U)) != 0U,
		"wells: a legacy TC2's AUX well asks for the TC cold exit once, without the Dekel health wait");

	/* The same well of a DisplayPort alternate mode port: no exit. */
	i915_xd_icl_test_owner.legacy = 0;
	(void)drv_i915_xd_power_well_disable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	error = drv_i915_xd_power_well_enable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	i915_host_check(error == 0 && i915_xd_icl_test_exits == 1U, "wells: a DisplayPort alternate mode port's AUX well asks for no exit");

	/* TC2's Thunderbolt AUX well (index 9) routes AUX D through the Thunderbolt IO. */
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_AUX, I915_XD_WELL_AUX_TBT_ICL(1U));
	error = drv_i915_xd_power_well_enable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	i915_host_check(well >= 0 && error == 0 && (i915_xd_fake_get(I915_XD_DDI_AUX_CTL(3U)) & I915_XD_AUX_CTL_TBT_IO) != 0U &&
		i915_xd_icl_test_exits == 1U, "wells: TC2's Thunderbolt AUX well routes AUX D through the Thunderbolt IO");

	/* Combo AUX A of the panel: the lane on after the request, no analog override. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_AUX, 0U);
	error = drv_i915_xd_power_well_enable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	request = i915_xd_icl_test_index(offsets, count, I915_XD_PWR_WELL_CTL_AUX);
	lane = i915_xd_icl_test_index(offsets, count, I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 12U));
	i915_host_check(error == 0 && request >= 0 && lane == request + 1 &&
		(i915_xd_fake_get(I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_A, 12U)) & I915_XD_CL_DW12_AUX_LANE_ENABLE) != 0U &&
		i915_xd_icl_test_index(offsets, count, I915_XD_AUX_ANAOVRD1(I915_XD_COMBO_PHY_A)) < 0,
		"wells: the panel's AUX A turns PHY A's AUX lane on after the request, without the analog override");

	/* Combo AUX B of another port: PHY B's lane and the analog override (display WA #1178). */
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_AUX, 1U);
	error = drv_i915_xd_power_well_enable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	i915_host_check(error == 0 && (i915_xd_fake_get(I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_B, 12U)) & I915_XD_CL_DW12_AUX_LANE_ENABLE) != 0U &&
		i915_xd_fake_get(0x6c398U) == (I915_XD_AUX_ANAOVRD1_ENABLE | I915_XD_AUX_ANAOVRD1_LDO_BYPASS),
		"wells: AUX B of another port turns PHY B's lane on and takes the analog override at 0x6c398");

	/* The disable takes the lane off before the request. */
	drv_i915_xd_io_init(&i915_xd_icl_test_io, NULL, NULL);
	i915_xd_fake_hooks(&i915_xd_icl_test_io.hooks);
	error = drv_i915_xd_power_well_disable(&i915_xd_icl_test_io, platform, (unsigned)well, &hooks, &report);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(error == 0 && count >= 2U && offsets[0] == I915_XD_PORT_CL_DW(I915_XD_COMBO_PHY_B, 12U) &&
		offsets[1] == I915_XD_PWR_WELL_CTL_AUX && (values[0] & I915_XD_CL_DW12_AUX_LANE_ENABLE) == 0U,
		"wells: the disable takes PHY B's AUX lane off, then the request");
}

/*
 * The transcoders of display 11: the eDP transcoder's DDI function word
 * on pipe A and pipe B, another transcoder's DDI select, the eDP
 * transcoder's setup (its block, its chicken word, no vblank delay), the
 * clock select in display 11's field and none for the eDP transcoder,
 * and the training pattern in the port's transport.
 */
static void
i915_xd_icl_test_trans(void)
{
	uint32_t offsets[I915_XD_ICL_TEST_WRITES];
	uint32_t values[I915_XD_ICL_TEST_WRITES];
	const struct i915_xd_platform *platform;
	struct i915_xd_trans_setup setup;
	struct i915_xd_trans_func func;
	uint32_t word;
	unsigned count;

	/* eDP on DDI A, DisplayPort SST, 8 bits, four lanes, positive syncs. */
	memset(&func, 0, sizeof(func));
	func.ddi = 0U;
	func.mode = I915_XD_FUNC_DP_SST;
	func.bpp = 24U;
	func.vsync_high = 1;
	func.hsync_high = 1;
	func.lanes = 4U;
	func.mst_master = I915_XD_NO_TRANSCODER;
	func.sync_master = I915_XD_NO_TRANSCODER;
	word = drv_i915_xd_pipe_func_word_11(I915_XD_TRANS_EDP, 0U, &func);
	i915_host_check(word == 0x82030006U, "trans: the eDP transcoder on pipe A: DDI A, SST, 8 bits, the input always on");
	word = drv_i915_xd_pipe_func_word_11(I915_XD_TRANS_EDP, 1U, &func);
	i915_host_check(word == 0x82035006U, "trans: the eDP transcoder on pipe B takes pipe B's switched input");

	/* HDMI on DDI B through transcoder B: the DDI index in bits 30..28. */
	func.ddi = 1U;
	func.mode = I915_XD_FUNC_HDMI;
	word = drv_i915_xd_pipe_func_word_11(1U, 1U, &func);
	i915_host_check(word == 0x90030000U, "trans: transcoder B on DDI B selects 1 in bits 30..28");

	/* The eDP transcoder's setup: its block at 0x6f000, its chicken word, no vblank delay. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	memset(&setup, 0, sizeof(setup));
	setup.trans = I915_XD_TRANS_EDP;
	setup.timing.hdisplay = 1920U;
	setup.timing.hblank_start = 1920U;
	setup.timing.hblank_end = 2200U;
	setup.timing.hsync_start = 2008U;
	setup.timing.hsync_end = 2052U;
	setup.timing.htotal = 2200U;
	setup.timing.vdisplay = 1080U;
	setup.timing.vblank_start = 1080U;
	setup.timing.vblank_end = 1125U;
	setup.timing.vsync_start = 1084U;
	setup.timing.vsync_end = 1089U;
	setup.timing.vtotal = 1125U;
	setup.pixel_multiplier = 1U;
	setup.frame_start_delay = 1U;
	drv_i915_xd_pipe_configure(&i915_xd_icl_test_io, platform, &setup);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(i915_xd_fake_get(0x6f000U) == 0x0897077fU && i915_xd_icl_test_index(offsets, count, 0x7f008U) >= 0 &&
		i915_xd_icl_test_index(offsets, count, 0x420ccU) >= 0 && (i915_xd_fake_get(0x420ccU) & I915_XD_CHICKEN_TRANS_VBLANK_DELAY) == 0U,
		"trans: the eDP transcoder's timing at 0x6f000, its configuration at 0x7f008, its chicken word at 0x420cc");

	/* The clock select: transcoder B on DDI B in display 11's field; none for the eDP transcoder. */
	drv_i915_xd_io_init(&i915_xd_icl_test_io, NULL, NULL);
	i915_xd_fake_hooks(&i915_xd_icl_test_io.hooks);
	drv_i915_xd_pipe_clock_on(&i915_xd_icl_test_io, platform, 1U, 1U);
	drv_i915_xd_pipe_clock_on(&i915_xd_icl_test_io, platform, I915_XD_TRANS_EDP, 0U);
	drv_i915_xd_pipe_clock_off(&i915_xd_icl_test_io, I915_XD_TRANS_EDP);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(count == 1U && offsets[0] == 0x46144U && values[0] == 0x40000000U,
		"trans: transcoder B's clock on DDI B is 2 in bits 31..29; the eDP transcoder has none");

	/* The training pattern 2 of DDI B's transport at 0x64140. */
	drv_i915_xd_ddi_pattern(&i915_xd_icl_test_io, I915_XD_DP_TP_PORT(1U), I915_XD_DP_TP_PATTERN_2);
	i915_host_check(i915_xd_fake_get(0x64140U) == 0x100U && I915_XD_DP_TP_STATUS(I915_XD_DP_TP_PORT(1U)) == 0x64144U,
		"trans: DDI B's transport is at 0x64140 and its status at 0x64144");
}

/*
 * The clocks of display 11: the CD clock's bypass of 50 MHz, the
 * Thunderbolt PLL's words with their dividers, a combo DPLL's words at
 * Ice Lake's places without a DIV0, and the data box word without
 * regulated back-to-back transactions.
 */
static void
i915_xd_icl_test_clocks(void)
{
	uint32_t offsets[I915_XD_ICL_TEST_WRITES];
	uint32_t values[I915_XD_ICL_TEST_WRITES];
	const struct i915_xd_platform *platform;
	struct i915_xd_dpll_words words;
	struct i915_xd_cdclk state;
	uint32_t word;
	unsigned count;
	unsigned late;

	/* The CD clock with its PLL off on a 38.4 MHz reference: the 50 MHz bypass. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DSSM, I915_XD_DSSM_REFCLK_38P4 << 29U);
	drv_i915_xd_cdclk_read(&i915_xd_icl_test_io, platform, &state);
	i915_host_check(state.ref_khz == 38400U && state.bypass_khz == 50000U && state.cdclk_khz == 50000U,
		"clocks: display 11's bypass clock is 50 MHz");

	/* The voltage levels of 312, 556.8 and 652.8 MHz. */
	i915_host_check(drv_i915_xd_cdclk_voltage(platform, 312000U) == 0U && drv_i915_xd_cdclk_voltage(platform, 326400U) == 1U &&
		drv_i915_xd_cdclk_voltage(platform, 652800U) == 2U, "clocks: Ice Lake's three voltage levels");

	/* The Thunderbolt PLL on 19.2 MHz: its DCO, K 1, P 5 and the 8.4 GHz central frequency. */
	drv_i915_xd_dpll_tbt_words(platform, 19200U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(words.cfgcr0 == 0x01c001a5U && words.cfgcr1 == 0x53U, "clocks: Ice Lake's Thunderbolt PLL on 19.2 MHz");
	drv_i915_xd_dpll_tbt_words(platform, 24000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(words.cfgcr0 == 0x01000151U && words.cfgcr1 == 0x53U, "clocks: Ice Lake's Thunderbolt PLL on 24 MHz");

	/* DPLL0 with the VBT's AFC startup: its words at 0x164000 and 0x164004, no DIV0 on display 11. */
	words.div0 = 0x04000000U;
	words.div0_set = 1;
	late = drv_i915_xd_dpll_enable(&i915_xd_icl_test_io, platform, drv_i915_xd_dpll_desc(platform, 0U), &words);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(late == 0U && i915_xd_fake_get(0x164000U) == 0x01000151U && i915_xd_fake_get(0x164004U) == 0x53U &&
		i915_xd_icl_test_index(offsets, count, I915_XD_DPLL_DIV0(0U)) < 0 && i915_xd_icl_test_index(offsets, count, 0x164284U) < 0,
		"clocks: Ice Lake's DPLL0 words go to 0x164000 and 0x164004, without a DIV0");

	/* The data box: A 2, BW 1, B 8, nothing regulated. */
	word = drv_i915_xd_dbuf_dbox_word(platform, 0);
	i915_host_check(word == 0x4802U, "clocks: Ice Lake's data box word is A 2, BW 1, B 8");
}

/*
 * A version 1 DMC package (one main program of 8 dwords and two
 * registers): parsed on Ice Lake with the program at 0x80000; its load
 * writes no event handler, the program, the registers and the DC state
 * debug masks.
 */
static void
i915_xd_icl_test_dmc(void)
{
	static struct i915_xd_dmc dmc;
	uint32_t offsets[I915_XD_ICL_TEST_WRITES];
	uint32_t values[I915_XD_ICL_TEST_WRITES];
	const struct i915_xd_platform *platform;
	struct i915_xd_dmc_store store;
	struct i915_xd_choice choice;
	uint8_t *package;
	uint8_t *header;
	unsigned count;
	unsigned word;
	size_t size;
	int error;

	/* The signature header, the version 1 package header with its 20 entries, one main entry. */
	memset(i915_xd_icl_test_file, 0, sizeof(i915_xd_icl_test_file));
	i915_xd_icl_test_put32(i915_xd_icl_test_file, 4U, 128U / 4U);
	i915_xd_icl_test_put32(i915_xd_icl_test_file, 88U, 0x00010009U);
	package = i915_xd_icl_test_file + 128U;
	package[0] = (uint8_t)((16U + 20U * 12U) / 4U);
	package[1] = 1U;
	i915_xd_icl_test_put32(package, 12U, 1U);
	package[16U + 1U] = 0U;
	package[16U + 2U] = '*';
	package[16U + 3U] = '*';
	i915_xd_icl_test_put32(package, 16U + 4U, 0U);

	/* The version 1 program header: its length in bytes, 8 dwords, two registers in the DMC block. */
	header = package + 16U + 20U * 12U;
	i915_xd_icl_test_put32(header, 0U, 0x40403e3eU);
	header[4] = 128U;
	header[5] = 1U;
	i915_xd_icl_test_put32(header, 12U, 8U);
	i915_xd_icl_test_put32(header, 20U, 2U);
	i915_xd_icl_test_put32(header, 24U, 0x8f074U);
	i915_xd_icl_test_put32(header, 28U, 0x8f004U);
	i915_xd_icl_test_put32(header, 56U, 0x11U);
	i915_xd_icl_test_put32(header, 60U, 0x22U);
	for (word = 0U; word < 8U; word++)
		i915_xd_icl_test_put32(header, 128U + 4U * word, 0xa000U + word);
	size = 128U + 16U + 20U * 12U + 128U + 32U;

	/* Parsed on Ice Lake. */
	platform = i915_xd_icl_test_setup(I915_XD_ICL_TEST_DEVICE);
	if (platform == NULL)
		return;
	error = drv_i915_xd_platform_choose(I915_XD_ICL_TEST_DEVICE, I915_XD_ICL_TEST_REVID, 0, 0U, &choice);
	store.bytes = i915_xd_icl_test_store;
	store.room = sizeof(i915_xd_icl_test_store);
	store.used = 0U;
	if (error == 0)
		error = drv_i915_xd_dmc_parse(&dmc, platform, choice.step, i915_xd_icl_test_file, size, &store);
	i915_host_check(error == 0 && dmc.package_version == 1U && dmc.program[0].header_version == 1U && dmc.program[0].start == 0x80000U &&
		dmc.program[0].dwords == 8U && dmc.program[0].registers == 2U, "dmc: a version 1 package's main program goes to 0x80000");

	/* The load: no event handler, the program, the registers, the debug masks. */
	drv_i915_xd_dmc_load(&dmc, &i915_xd_icl_test_io, platform);
	count = i915_xd_icl_test_writes(offsets, values, I915_XD_ICL_TEST_WRITES);
	i915_host_check(dmc.loaded == 1 && dmc.handler_writes == 0U && count == 8U + 2U + 1U && offsets[0] == 0x80000U &&
		values[7] == 0xa007U && offsets[8] == 0x8f074U && values[9] == 0x22U && offsets[10] == I915_XD_DC_STATE_DEBUG,
		"dmc: display 11's load writes the program, its registers and the debug masks, and no event handler");
}
