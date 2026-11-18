/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd combo port PLLs (display/xd-clock.c) on the
 * register model (xd-fake-regs.c), whose PLL power and lock follow their
 * requests: the words of the DisplayPort link rates (the 5330's DPLL 0),
 * of HDMI clocks and of the Thunderbolt PLL; the link clock the words
 * give; the writes of a PLL's enable and disable, the AFC startup, the
 * waits that run out; the read of a PLL; and the DDI clock map of Tiger
 * Lake and display 13, Rocket Lake and Alder Lake-S.
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

/* The GPUs of the Latitude 5330 (Alder Lake-P), a Tiger Lake laptop, Rocket Lake and Alder Lake-S. */
#define I915_XD_DPLL_TEST_ADLP		0x46a6U
#define I915_XD_DPLL_TEST_ADLP_REVID	0x0cU
#define I915_XD_DPLL_TEST_TGL		0x9a49U
#define I915_XD_DPLL_TEST_TGL_REVID	0x01U
#define I915_XD_DPLL_TEST_RKL		0x4c8aU
#define I915_XD_DPLL_TEST_ADLS		0x4680U

/*
 * The 5330's DPLL 0 for its eDP at 2.7 GHz on a 38.4 MHz reference, as
 * Linux left it (its debugfs i915_shared_dplls_info), and the
 * Thunderbolt PLL's DCO word of the same machine (off, as the firmware
 * leaves it, read from the machine's registers).
 */
#define I915_XD_DPLL_TEST_HBR_CFGCR0	0x00e001a5U
#define I915_XD_DPLL_TEST_HBR_CFGCR1	0x00000088U
#define I915_XD_DPLL_TEST_TBT_CFGCR0	0x00600054U

/* The most writes one check reads back. */
#define I915_XD_DPLL_TEST_WRITES	16U

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_dpll_test_io;

static const struct i915_xd_platform *i915_xd_dpll_test_setup(uint16_t device, uint8_t revid, int locks);
static unsigned i915_xd_dpll_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static int i915_xd_dpll_test_same(const uint32_t *offsets, const uint32_t *values, unsigned count, const uint32_t *want_offsets, const uint32_t *want_values, unsigned want);
static void i915_xd_dpll_test_words(void);
static void i915_xd_dpll_test_hdmi(void);
static void i915_xd_dpll_test_switch(void);
static void i915_xd_dpll_test_read(void);
static void i915_xd_dpll_test_map(void);

/*
 * Runs every check of the xd port PLLs and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_dpll_test_words();
	i915_xd_dpll_test_hdmi();
	i915_xd_dpll_test_switch();
	i915_xd_dpll_test_read();
	i915_xd_dpll_test_map();

	/* Reports the result. */
	failures = i915_host_report("host_xd_dpll_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model whose combo PLLs follow their power and (when
 * locks is nonzero) lock to their enable; returns the description of a
 * device.
 */
static const struct i915_xd_platform *
i915_xd_dpll_test_setup(
	uint16_t device,
	uint8_t revid,
	int locks)
{
	static const uint32_t enables[] = {
		I915_XD_DPLL0_ENABLE, I915_XD_DPLL1_ENABLE, I915_XD_DPLL2_ENABLE_ADLS, I915_XD_TBT_PLL_ENABLE
	};
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	unsigned index;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_dpll_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_dpll_test_io, &hooks);

	/* The power state follows the power request, the lock the enable. */
	for (index = 0U; index < sizeof(enables) / sizeof(enables[0]); index++) {
		i915_xd_fake_rule_mirror(enables[index], I915_XD_PLL_POWER_ENABLE, enables[index], -1, 0U);
		if (locks)
			i915_xd_fake_rule_mirror(enables[index], I915_XD_PLL_ENABLE, enables[index], -1, 0U);
	}

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Copies the writes the window recorded, oldest first; reports how many. */
static unsigned
i915_xd_dpll_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_dpll_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count && kept < max; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* Tells whether recorded writes are exactly the wanted ones, in order. */
static int
i915_xd_dpll_test_same(
	const uint32_t *offsets,
	const uint32_t *values,
	unsigned count,
	const uint32_t *want_offsets,
	const uint32_t *want_values,
	unsigned want)
{
	unsigned index;

	/* The same number of writes, each the same. */
	if (count != want)
		return 0;
	for (index = 0U; index < count; index++) {
		if (offsets[index] != want_offsets[index] || values[index] != want_values[index])
			return 0;
	}

	/* Succeeded: the same writes. */
	return 1;
}

/* Checks the words of the DisplayPort link rates and of the Thunderbolt PLL, and their link clocks. */
static void
i915_xd_dpll_test_words(void)
{
	static const uint32_t rates[] = { 162000U, 216000U, 270000U, 324000U, 432000U, 540000U, 648000U, 810000U };
	static const uint32_t refs[] = { 19200U, 24000U, 38400U };
	const struct i915_xd_platform *adlp;
	const struct i915_xd_platform *tgl;
	const struct i915_xd_platform *rkl;
	struct i915_xd_dpll_words words;
	unsigned rate;
	unsigned ref;
	int all;
	int error;
	uint32_t port_khz;

	/* The Alder Lake-P and Tiger Lake descriptions. */
	adlp = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	tgl = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_TGL, I915_XD_DPLL_TEST_TGL_REVID, 1);
	rkl = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_RKL, 0U, 1);
	if (adlp == NULL ||
	    tgl == NULL ||
	    rkl == NULL) {
		i915_host_check(0, "dpll: the descriptions are found");
		return;
	}

	/* The 5330's eDP at 2.7 GHz: the fraction halved on the 38.4 MHz reference. */
	error = drv_i915_xd_dpll_dp_words(adlp, 38400U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && words.cfgcr0 == I915_XD_DPLL_TEST_HBR_CFGCR0 && words.cfgcr1 == I915_XD_DPLL_TEST_HBR_CFGCR1 &&
			words.div0 == 0U && words.div0_set == 0,
			"dpll: HBR on 38.4 MHz is the 5330's DPLL 0 (0x00e001a5 / 0x88), no AFC startup");
	error = drv_i915_xd_dpll_dp_words(tgl, 38400U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && words.cfgcr0 == I915_XD_DPLL_TEST_HBR_CFGCR0 && words.cfgcr1 == I915_XD_DPLL_TEST_HBR_CFGCR1,
			"dpll: Tiger Lake halves the fraction on 38.4 MHz too");
	error = drv_i915_xd_dpll_dp_words(rkl, 38400U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && words.cfgcr0 == 0x01c001a5U && words.cfgcr1 == I915_XD_DPLL_TEST_HBR_CFGCR1,
			"dpll: Rocket Lake keeps the whole fraction");
	error = drv_i915_xd_dpll_dp_words(adlp, 24000U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && words.cfgcr0 == 0x01000151U && words.cfgcr1 == I915_XD_DPLL_TEST_HBR_CFGCR1,
			"dpll: HBR on 24 MHz takes the 24 MHz row, whole");
	error = drv_i915_xd_dpll_dp_words(adlp, 38400U, 270000U, 5, &words);
	i915_host_check(error == 0 && words.div0 == 0x0a000000U && words.div0_set == 1,
			"dpll: the VBT's AFC startup 5 is the DIV0 field 0x0a000000");
	error = drv_i915_xd_dpll_dp_words(adlp, 38400U, 300000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == EINVAL, "dpll: a rate without a row is refused");

	/* Every row gives back its link rate on every reference. */
	all = 1;
	for (ref = 0U; ref < sizeof(refs) / sizeof(refs[0]); ref++) {
		for (rate = 0U; rate < sizeof(rates) / sizeof(rates[0]); rate++) {
			error = drv_i915_xd_dpll_dp_words(adlp, refs[ref], rates[rate], I915_XD_DPLL_AFC_NONE, &words);
			if (error != 0) {
				all = 0;
				continue;
			}

			/* The words give back the rate. */
			port_khz = drv_i915_xd_dpll_port_khz(adlp, refs[ref], &words);
			if (port_khz != rates[rate])
				all = 0;
		}
	}

	/* Checks every DisplayPort row gives back its link rate (19.2, 24, 38.4 MHz). */
	i915_host_check(all, "dpll: every DisplayPort row gives back its link rate (19.2, 24, 38.4 MHz)");
	words.cfgcr0 = I915_XD_DPLL_TEST_HBR_CFGCR0;
	words.cfgcr1 = 0U;
	i915_host_check(drv_i915_xd_dpll_port_khz(adlp, 38400U, &words) == 0U, "dpll: words without dividers give no link clock");

	/* The Thunderbolt PLL. */
	drv_i915_xd_dpll_tbt_words(adlp, 38400U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(words.cfgcr0 == I915_XD_DPLL_TEST_TBT_CFGCR0 && words.cfgcr1 == 0U,
			"dpll: the Thunderbolt PLL's DCO on 38.4 MHz is the 5330's 0x00600054, no dividers");
}

/* Checks the HDMI words: the divider nearest the middle of the DCO range, and the link clock back. */
static void
i915_xd_dpll_test_hdmi(void)
{
	static const uint32_t clocks[] = { 25200U, 27000U, 54000U, 74250U, 108000U, 148500U, 241500U, 297000U, 594000U };
	const struct i915_xd_platform *adlp;
	struct i915_xd_dpll_words words;
	uint32_t port;
	unsigned index;
	int close;
	int error;

	/* The Alder Lake-P description. */
	adlp = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	if (adlp == NULL)
		return;

	/* 148.5 MHz: divider 12 (8910 MHz): P 2, Q 3, K 2; the fraction 0x800 halved. */
	error = drv_i915_xd_dpll_hdmi_words(adlp, 38400U, 148500U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && words.cfgcr0 == 0x001001d0U && words.cfgcr1 == 0x00000e84U,
			"dpll: HDMI 148.5 MHz on 38.4 MHz: 0x001001d0 / 0x00000e84 (divider 12)");

	/* 27 MHz: the DCO of 66 (8910 MHz) is nearer the middle than that of 68 (9180 MHz). */
	error = drv_i915_xd_dpll_hdmi_words(adlp, 38400U, 27000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == 0 && i915_xd_field_get(I915_XD_CFGCR1_PDIV, words.cfgcr1) == 2U &&
			i915_xd_field_get(I915_XD_CFGCR1_QDIV_RATIO, words.cfgcr1) == 11U,
			"dpll: HDMI 27 MHz takes divider 66 (P 3, Q 11, K 2), the DCO nearest the middle");

	/* Every clock gives itself back within a kHz. */
	close = 1;
	for (index = 0U; index < sizeof(clocks) / sizeof(clocks[0]); index++) {
		error = drv_i915_xd_dpll_hdmi_words(adlp, 38400U, clocks[index], I915_XD_DPLL_AFC_NONE, &words);
		port = drv_i915_xd_dpll_port_khz(adlp, 38400U, &words);
		if (error != 0 ||
		    port + 1U < clocks[index] ||
		    port > clocks[index] + 1U)
			close = 0;
	}

	/* Checks the HDMI clocks from 25.2 to 594 MHz give themselves back. */
	i915_host_check(close, "dpll: the HDMI clocks from 25.2 to 594 MHz give themselves back");

	/* A clock no divider fits. */
	error = drv_i915_xd_dpll_hdmi_words(adlp, 38400U, 1000U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(error == EINVAL, "dpll: a clock no divider fits is refused");
}

/* Checks the writes of an enable and a disable, the AFC startup, and the waits that run out. */
static void
i915_xd_dpll_test_switch(void)
{
	static const uint32_t on_offsets[] = { I915_XD_DPLL0_ENABLE, 0x164284U, 0x164288U, I915_XD_DPLL0_ENABLE };
	static const uint32_t on_values[] = { 0x08000000U, I915_XD_DPLL_TEST_HBR_CFGCR0, I915_XD_DPLL_TEST_HBR_CFGCR1, 0x8c000000U };
	static const uint32_t off_offsets[] = { I915_XD_DPLL0_ENABLE, I915_XD_DPLL0_ENABLE };
	static const uint32_t off_values[] = { 0x4c000000U, 0x04000000U };
	static const uint32_t afc_offsets[] = { I915_XD_DPLL1_ENABLE, 0x16428cU, 0x164290U, 0x164c00U, I915_XD_DPLL1_ENABLE };
	static const uint32_t afc_values[] = { 0x08000000U, I915_XD_DPLL_TEST_HBR_CFGCR0, I915_XD_DPLL_TEST_HBR_CFGCR1, 0x0a000000U, 0x8c000000U };
	static const uint32_t rkl_offsets[] = { I915_XD_DPLL2_ENABLE_ADLS, 0x164294U, 0x164298U, I915_XD_DPLL2_ENABLE_ADLS };
	static const uint32_t rkl_values[] = { 0x08000000U, 0x01c001a5U, I915_XD_DPLL_TEST_HBR_CFGCR1, 0x8c000000U };
	const struct i915_xd_platform *platform;
	struct i915_xd_dpll_words words;
	uint32_t offsets[I915_XD_DPLL_TEST_WRITES];
	uint32_t values[I915_XD_DPLL_TEST_WRITES];
	unsigned count;
	unsigned late;

	/* The 5330's DPLL 0: power, the words, the enable; lock and power came. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	if (platform == NULL)
		return;
	(void)drv_i915_xd_dpll_dp_words(platform, 38400U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	late = drv_i915_xd_dpll_enable(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 0U), &words);
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	i915_host_check(late == 0U && i915_xd_fake_get(I915_XD_DPLL0_ENABLE) == 0xcc000000U &&
			i915_xd_dpll_test_same(offsets, values, count, on_offsets, on_values, 4U),
			"dpll: enable writes power, CFGCR0, CFGCR1, enable (no SSC, no DIV0); the PLL locks (0xcc000000)");

	/* The disable: the enable goes, then the power. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	i915_xd_fake_set(I915_XD_DPLL0_ENABLE, 0xcc000000U);
	late = drv_i915_xd_dpll_disable(&i915_xd_dpll_test_io, drv_i915_xd_dpll_desc(platform, 0U));
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	i915_host_check(late == 0U && i915_xd_fake_get(I915_XD_DPLL0_ENABLE) == 0U &&
			i915_xd_dpll_test_same(offsets, values, count, off_offsets, off_values, 2U),
			"dpll: disable clears the enable, then the power; lock and power go");

	/* The VBT's AFC startup goes into DPLL 1's DIV0 before the enable. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	(void)drv_i915_xd_dpll_dp_words(platform, 38400U, 270000U, 5, &words);
	late = drv_i915_xd_dpll_enable(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 1U), &words);
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	i915_host_check(late == 0U && i915_xd_dpll_test_same(offsets, values, count, afc_offsets, afc_values, 5U),
			"dpll: the AFC startup is written into DPLL 1's DIV0 between the words and the enable");

	/* Rocket Lake's third PLL uses the words at 0x164294 and has no DIV0. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_RKL, 0U, 1);
	if (platform == NULL)
		return;
	(void)drv_i915_xd_dpll_dp_words(platform, 38400U, 270000U, 5, &words);
	late = drv_i915_xd_dpll_enable(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 2U), &words);
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	i915_host_check(late == 0U && i915_xd_dpll_test_same(offsets, values, count, rkl_offsets, rkl_values, 4U),
			"dpll: Rocket Lake's DPLL 4 (select 2) writes 0x164294/0x164298 and no DIV0");

	/* A PLL that does not lock: the lock's wait runs out; one that does not unlock either. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 0);
	(void)drv_i915_xd_dpll_dp_words(platform, 38400U, 270000U, I915_XD_DPLL_AFC_NONE, &words);
	late = drv_i915_xd_dpll_enable(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 0U), &words);
	i915_host_check(late == I915_XD_DPLL_LATE_LOCK, "dpll: a PLL that does not lock is reported late on the lock only");
	i915_xd_fake_set(I915_XD_DPLL0_ENABLE, 0xcc000000U);
	late = drv_i915_xd_dpll_disable(&i915_xd_dpll_test_io, drv_i915_xd_dpll_desc(platform, 0U));
	i915_host_check(late == I915_XD_DPLL_LATE_UNLOCK, "dpll: a lock that does not go is reported late on the unlock only");

	/* A Type-C PLL's words are in its PHY: the enable refuses it and writes nothing. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	late = drv_i915_xd_dpll_enable(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 3U), &words);
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	i915_host_check(late != 0U && count == 0U, "dpll: a Type-C PLL is not written by the combo enable");
	i915_host_check(drv_i915_xd_dpll_desc(platform, 6U) != NULL && drv_i915_xd_dpll_desc(platform, 7U) == NULL,
			"dpll: Alder Lake-P has selects 0 to 6 (TC PLL 1 to 4 last)");

	/* The enable words of Alder Lake-P's PLLs: DPLL 0 and 1, the TBT PLL, TC PLL 1 to 4. */
	i915_host_check(drv_i915_xd_dpll_desc(platform, 0U)->enable == 0x46010U &&
			drv_i915_xd_dpll_desc(platform, 1U)->enable == 0x46014U &&
			drv_i915_xd_dpll_desc(platform, 2U)->enable == 0x46020U &&
			drv_i915_xd_dpll_desc(platform, 2U)->kind == I915_XD_PLL_TBT &&
			drv_i915_xd_dpll_desc(platform, 3U)->enable == 0x46038U &&
			drv_i915_xd_dpll_desc(platform, 4U)->enable == 0x46040U &&
			drv_i915_xd_dpll_desc(platform, 5U)->enable == 0x46048U &&
			drv_i915_xd_dpll_desc(platform, 6U)->enable == 0x46050U &&
			drv_i915_xd_dpll_desc(platform, 6U)->kind == I915_XD_PLL_TC && drv_i915_xd_dpll_desc(platform, 6U)->tc == 4U,
			"dpll: Alder Lake-P's enables: DPLL 0x46010/0x46014, TBT 0x46020, TC1..4 0x46038/0x46040/0x46048/0x46050");
}

/* Checks the read of a PLL: the 5330's DPLL 0, a PLL that is off, the AFC startup only with the override. */
static void
i915_xd_dpll_test_read(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_dpll_words words;
	int enabled;

	/* The Alder Lake-P description. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DPLL0_ENABLE, 0xcc000000U);
	i915_xd_fake_set(0x164284U, I915_XD_DPLL_TEST_HBR_CFGCR0);
	i915_xd_fake_set(0x164288U, I915_XD_DPLL_TEST_HBR_CFGCR1);
	i915_xd_fake_set(0x164b00U, 0x1a000000U);

	/* The 5330's DPLL 0 reads as Linux's state: enabled, its words, 2.7 GHz. */
	memset(&words, 0, sizeof(words));
	enabled = drv_i915_xd_dpll_read(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 0U), I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(enabled == 1 && words.cfgcr0 == I915_XD_DPLL_TEST_HBR_CFGCR0 && words.cfgcr1 == I915_XD_DPLL_TEST_HBR_CFGCR1 &&
			words.div0 == 0U && drv_i915_xd_dpll_port_khz(platform, 38400U, &words) == 270000U,
			"dpll: the 5330's DPLL 0 reads as Linux's state (enabled, 0x00e001a5 / 0x88, div0 0), 2.7 GHz");

	/* With the override the AFC field of DIV0 is read. */
	enabled = drv_i915_xd_dpll_read(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 0U), 0, &words);
	i915_host_check(enabled == 1 && words.div0 == 0x0a000000U && words.div0_set == 1, "dpll: with the override only the AFC field of DIV0 is read");

	/* DPLL 1, off. */
	enabled = drv_i915_xd_dpll_read(&i915_xd_dpll_test_io, platform, drv_i915_xd_dpll_desc(platform, 1U), I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(enabled == 0, "dpll: DPLL 1, off, reads as off");
}

/* Checks the DDI clock map of the three layouts. */
static void
i915_xd_dpll_test_map(void)
{
	static const uint32_t map_offsets[] = { I915_XD_DPCLKA_CFGCR0, I915_XD_DPCLKA_CFGCR0 };
	static const uint32_t map_values[] = { 0x00000c00U, 0x00000800U };
	const struct i915_xd_platform *platform;
	uint32_t offsets[I915_XD_DPLL_TEST_WRITES];
	uint32_t values[I915_XD_DPLL_TEST_WRITES];
	unsigned count;
	unsigned select;
	int running;

	/* The 5330's PHY A on DPLL 0: the select, then the clock-off bit cleared, in two writes. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLP, I915_XD_DPLL_TEST_ADLP_REVID, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DPCLKA_CFGCR0, 0x00000c00U);
	drv_i915_xd_dpll_map(&i915_xd_dpll_test_io, platform, 0U, 0U);
	count = i915_xd_dpll_test_writes(offsets, values, I915_XD_DPLL_TEST_WRITES);
	running = drv_i915_xd_dpll_mapped(&i915_xd_dpll_test_io, platform, 0U, &select);
	i915_host_check(i915_xd_dpll_test_same(offsets, values, count, map_offsets, map_values, 2U) && running == 1 && select == 0U,
			"dpll: PHY A takes DPLL 0 and its clock runs: the select, then the ungate (0x00000c00, 0x00000800)");

	/* PHY B on DPLL 1, then gated again; the select stays. */
	drv_i915_xd_dpll_map(&i915_xd_dpll_test_io, platform, 1U, 1U);
	running = drv_i915_xd_dpll_mapped(&i915_xd_dpll_test_io, platform, 1U, &select);
	i915_host_check(i915_xd_fake_get(I915_XD_DPCLKA_CFGCR0) == 0x00000004U && running == 1 && select == 1U,
			"dpll: PHY B takes DPLL 1 at bits 2-3 and its clock runs");
	drv_i915_xd_dpll_unmap(&i915_xd_dpll_test_io, platform, 1U);
	running = drv_i915_xd_dpll_mapped(&i915_xd_dpll_test_io, platform, 1U, &select);
	i915_host_check(i915_xd_fake_get(I915_XD_DPCLKA_CFGCR0) == 0x00000804U && running == 0 && select == 1U,
			"dpll: PHY B gated: its clock-off bit set, the select kept");

	/* Rocket Lake's PHY D: the select at bit 27, the clock-off bit at 13. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_RKL, 0U, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DPCLKA_CFGCR0, 0x00002000U);
	drv_i915_xd_dpll_map(&i915_xd_dpll_test_io, platform, 3U, 2U);
	i915_host_check(i915_xd_fake_get(I915_XD_DPCLKA_CFGCR0) == 0x10000000U, "dpll: Rocket Lake's PHY D takes DPLL 4 at bit 27, ungated at bit 13");

	/* Alder Lake-S's PHY E: the second word, select at bits 2-3, clock-off at bit 5. */
	platform = i915_xd_dpll_test_setup(I915_XD_DPLL_TEST_ADLS, 0U, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DPCLKA_CFGCR1_ADLS, 0x00000030U);
	drv_i915_xd_dpll_map(&i915_xd_dpll_test_io, platform, 4U, 3U);
	running = drv_i915_xd_dpll_mapped(&i915_xd_dpll_test_io, platform, 4U, &select);
	i915_host_check(i915_xd_fake_get(I915_XD_DPCLKA_CFGCR1_ADLS) == 0x0000001cU && i915_xd_fake_get(I915_XD_DPCLKA_CFGCR0) == 0U &&
			running == 1 && select == 3U,
			"dpll: Alder Lake-S's PHY E takes DPLL 3 in the second word (bits 2-3), ungated at bit 5");
}
