/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd platform descriptions (display/xd-platform.c):
 * the choice of a row from the PCI id, the stepping from the revision,
 * the south display from the PCH id; the rows against the ids the driver
 * accepts today (intel/pci-ids.h); and the shape of every row (ports,
 * wells, PLLs, tables) and the lookups.
 */

#include "host-test.h"

#include "../../display/display-internal.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../display/xd.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/pci-ids.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Spells one PCI id of the tree's id lists as a value. */
#define I915_XD_TEST_ID(id) (id)

/*
 * The ids the driver accepts today as a display 12 GPU (Tiger Lake) and
 * as a display 13 GPU (the Alder Lake-P class), as device.c lists them.
 */
static const uint16_t i915_xd_test_accepted_12[] = { I915_PCI_TGL_IDS(I915_XD_TEST_ID) };
static const uint16_t i915_xd_test_accepted_13[] = {
	I915_PCI_ADLP_IDS(I915_XD_TEST_ID),
	I915_PCI_ADLN_IDS(I915_XD_TEST_ID),
	I915_PCI_RPLU_IDS(I915_XD_TEST_ID),
	I915_PCI_RPLP_IDS(I915_XD_TEST_ID)
};

static int i915_xd_test_listed(const uint16_t *list, unsigned count, uint16_t id);
static const struct i915_xd_platform *i915_xd_test_choose(uint16_t device, uint8_t revid, const char *name);
static void i915_xd_test_rows(void);
static void i915_xd_test_steps(void);
static void i915_xd_test_pch(void);
static void i915_xd_test_accepted(void);
static void i915_xd_test_shape(void);
static void i915_xd_test_lookups(void);
static void i915_xd_test_start(void);

/*
 * Runs every check of the platform descriptions and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_test_rows();
	i915_xd_test_steps();
	i915_xd_test_pch();
	i915_xd_test_accepted();
	i915_xd_test_shape();
	i915_xd_test_lookups();
	i915_xd_test_start();

	/* Reports the result. */
	failures = i915_host_report("host_xd_platform_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Tells whether an id is in a list. */
static int
i915_xd_test_listed(
	const uint16_t *list,
	unsigned count,
	uint16_t id)
{
	unsigned index;

	/* Compares the id with each of the list's. */
	for (index = 0U; index < count; index++) {
		if (list[index] == id)
			return 1;
	}

	/* Succeeded: the id is not listed. */
	return 0;
}

/* Chooses the row of a device with its own PCH assumed, and checks the row's name. */
static const struct i915_xd_platform *
i915_xd_test_choose(
	uint16_t device,
	uint8_t revid,
	const char *name)
{
	struct i915_xd_choice choice;
	int error;
	int named;
	int differs;

	/* Chooses without a PCH. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	named = 0;
	if (error == 0 && choice.platform != NULL) {
		/* The row's name must be the one asked for. */
		differs = strcmp(choice.platform->name, name);
		if (differs == 0)
			named = 1;
	}

	/* Checks the row's name. */
	i915_host_check(named != 0, name);

	/* Succeeded: reports the row, or NULL. */
	return choice.platform;
}

/* Checks the row chosen for a representative id of each of the eleven rows. */
static void
i915_xd_test_rows(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_choice choice;
	int error;

	/* Eleven rows, eleven representative ids. */
	i915_host_check(drv_i915_xd_platform_count() == 11U, "rows: eleven platform rows");
	platform = i915_xd_test_choose(0x9a60U, 1U, "Tiger Lake");
	i915_host_check(platform != NULL && platform->display_ver == 12U && platform->verified != 0, "rows: Tiger Lake is display 12 and verified");
	platform = i915_xd_test_choose(0x9a49U, 1U, "Tiger Lake UY");
	i915_host_check(platform != NULL && platform->swing->dp_high != platform->swing->dp_low, "rows: Tiger Lake UY has its own high swing table");
	platform = i915_xd_test_choose(0x4c8aU, 0U, "Rocket Lake");
	i915_host_check(platform != NULL && platform->verified == 0 && platform->pipes == 0x7U, "rows: Rocket Lake is unverified with three pipes");
	platform = i915_xd_test_choose(0x4680U, 0U, "Alder Lake-S");
	i915_host_check(platform != NULL && platform->verified == 0 && platform->tc_flow == I915_XD_TC_FLOW_NONE, "rows: Alder Lake-S is unverified without a Type-C flow");
	(void)i915_xd_test_choose(0xa780U, 4U, "Raptor Lake-S");
	platform = i915_xd_test_choose(0x46a6U, 0xcU, "Alder Lake-P");
	i915_host_check(platform != NULL && platform->display_ver == 13U && platform->cdclk_crawl != 0 && platform->dbuf_slices == 4U, "rows: Alder Lake-P is display 13 with crawl and four slices");
	(void)i915_xd_test_choose(0x46d1U, 0U, "Alder Lake-N");
	(void)i915_xd_test_choose(0xa7a0U, 4U, "Raptor Lake-P");
	platform = i915_xd_test_choose(0xa7a1U, 4U, "Raptor Lake-U");
	i915_host_check(platform != NULL && platform->ncdclk == 18U && platform->nvoltage == 4U && platform->voltage_steps[1] == 480000U, "rows: Raptor Lake-U has the 480 MHz steps");

	/* Ice Lake: display 11 with port F on most parts, one FIA, MG PHYs, a 2 us read latency. */
	platform = i915_xd_test_choose(0x8a52U, 7U, "Ice Lake");
	i915_host_check(platform != NULL && platform->display_ver == 11U && platform->verified == 0 && platform->nports == 6U &&
		platform->fia == I915_XD_FIA_SINGLE && platform->tccold == I915_XD_TCCOLD_AUX_WELL_EXIT && platform->wm_read_latency_us == 2U,
		"rows: Ice Lake is unverified display 11 with port F, one FIA and the TC cold exit");
	platform = i915_xd_test_choose(0x8a51U, 7U, "Ice Lake without port F");
	i915_host_check(platform != NULL && platform->nports == 5U && platform->ports[4].ddi == 4U, "rows: Ice Lake without port F stops at DDI E");

	/* A GPU that is not display 11 to 13 has no row (a Kaby Lake, a Meteor Lake). */
	error = drv_i915_xd_platform_choose(0x5917U, 0U, 0, 0U, &choice);
	i915_host_check(error == ENODEV && choice.platform == NULL, "rows: a Kaby Lake id has no row");
	error = drv_i915_xd_platform_choose(0x7d55U, 0U, 0, 0U, &choice);
	i915_host_check(error == ENODEV, "rows: a Meteor Lake id has no row");
}

/* Checks the steppings, including revisions between and past the rows. */
static void
i915_xd_test_steps(void)
{
	struct i915_xd_choice choice;

	/* Tiger Lake: revision 0 is B0, 1 is D0, past the table is a future stepping. */
	(void)drv_i915_xd_platform_choose(0x9a60U, 0U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_B0, "steps: Tiger Lake revision 0 is B0");
	(void)drv_i915_xd_platform_choose(0x9a60U, 1U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_D0, "steps: Tiger Lake revision 1 is D0");
	(void)drv_i915_xd_platform_choose(0x9a60U, 5U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_FUTURE, "steps: Tiger Lake revision 5 is a future stepping");

	/* Tiger Lake UY has its own table: revision 1 is C0. */
	(void)drv_i915_xd_platform_choose(0x9a49U, 1U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_C0, "steps: Tiger Lake UY revision 1 is C0");

	/* A revision in a gap takes the next row: Rocket Lake 2 is C0, Alder Lake-S 2 is B0. */
	(void)drv_i915_xd_platform_choose(0x4c8aU, 2U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_C0, "steps: Rocket Lake revision 2 takes the next row (C0)");
	(void)drv_i915_xd_platform_choose(0x4680U, 2U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_B0, "steps: Alder Lake-S revision 2 takes the next row (B0)");

	/* Alder Lake-P 0xc is D0, Alder Lake-N 0 is D0, Raptor Lake-P and -U 4 are E0. */
	(void)drv_i915_xd_platform_choose(0x46a6U, 0xcU, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_D0, "steps: Alder Lake-P revision 0xc is D0");
	(void)drv_i915_xd_platform_choose(0x46d0U, 0U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_D0, "steps: Alder Lake-N revision 0 is D0");
	(void)drv_i915_xd_platform_choose(0xa7a1U, 4U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_E0, "steps: Raptor Lake-U revision 4 is E0");
	(void)drv_i915_xd_platform_choose(0xa780U, 0U, 0, 0U, &choice);
	i915_host_check(choice.step == I915_XD_STEP_D0, "steps: Raptor Lake-S revision 0 takes the next row (D0)");
}

/* Checks the south display: read, assumed, and refused. */
static void
i915_xd_test_pch(void)
{
	struct i915_xd_choice choice;
	int error;

	/* The PCH kinds from the masked ids. */
	i915_host_check(drv_i915_xd_pch_kind(0xa0efU) == I915_PCH_TGP, "pch: 0xa0ef is a TGP");
	i915_host_check(drv_i915_xd_pch_kind(0x4381U) == I915_PCH_TGP, "pch: 0x4381 is a TGP");
	i915_host_check(drv_i915_xd_pch_kind(0x5182U) == I915_PCH_ADP, "pch: 0x5182 is an ADP");
	i915_host_check(drv_i915_xd_pch_kind(0x7a84U) == I915_PCH_ADP, "pch: 0x7a84 is an ADP");
	i915_host_check(drv_i915_xd_pch_kind(0x2918U) == I915_PCH_NONE, "pch: a virtual machine's bridge is not known");

	/* A PCH of the platform's kind is read, not assumed. */
	error = drv_i915_xd_platform_choose(0x46a6U, 0xcU, 1, 0x5182U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_ADP && choice.pch_assumed == 0, "pch: Alder Lake-P beside an ADP");
	error = drv_i915_xd_platform_choose(0x9a49U, 1U, 1, 0xa0efU, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_TGP && choice.pch_assumed == 0, "pch: Tiger Lake beside a TGP");
	error = drv_i915_xd_platform_choose(0x4c8aU, 0U, 1, 0x4381U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_TGP, "pch: Rocket Lake beside a TGP");

	/* No PCH, or a virtual bridge: the platform's own kind is assumed. */
	error = drv_i915_xd_platform_choose(0x46a6U, 0xcU, 1, 0x2918U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_ADP && choice.pch_assumed != 0, "pch: a virtual bridge assumes the platform's PCH");
	error = drv_i915_xd_platform_choose(0x9a60U, 1U, 0, 0U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_TGP && choice.pch_assumed != 0, "pch: no bridge assumes the platform's PCH");

	/* A known PCH of another kind is refused: Rocket Lake on a Comet Lake PCH, Tiger Lake beside an ADP. */
	error = drv_i915_xd_platform_choose(0x4c8aU, 0U, 1, 0x0284U, &choice);
	i915_host_check(error == EOPNOTSUPP && choice.platform == NULL, "pch: Rocket Lake beside a Comet Lake PCH is refused");
	error = drv_i915_xd_platform_choose(0x9a60U, 1U, 1, 0x5182U, &choice);
	i915_host_check(error == EOPNOTSUPP, "pch: Tiger Lake beside an ADP is refused");
}

/*
 * Checks the rows against the ids the driver accepts today: every
 * accepted id has a verified row of the same display version, every id
 * of a verified row is accepted, and no id of an unverified row (Rocket
 * Lake, Alder Lake-S, Raptor Lake-S) is accepted, so those GPUs still do
 * not start the GT.
 */
static void
i915_xd_test_accepted(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_choice choice;
	unsigned count;
	unsigned index;
	unsigned row;
	int all;
	int listed;
	int error;
	unsigned rows;

	/* Every display 12 id the driver accepts has a verified display 12 row. */
	all = 1;
	count = sizeof(i915_xd_test_accepted_12) / sizeof(i915_xd_test_accepted_12[0]);
	for (index = 0U; index < count; index++) {
		error = drv_i915_xd_platform_choose(i915_xd_test_accepted_12[index], 0U, 0, 0U, &choice);
		if (error != 0 ||
		    choice.platform->display_ver != 12U ||
		    choice.platform->verified == 0)
			all = 0;
	}

	/* Checks every accepted display 12 id has a verified display 12 row. */
	i915_host_check(all != 0, "accepted: every accepted display 12 id has a verified display 12 row");

	/* Every display 13 id the driver accepts has a verified display 13 row. */
	all = 1;
	count = sizeof(i915_xd_test_accepted_13) / sizeof(i915_xd_test_accepted_13[0]);
	for (index = 0U; index < count; index++) {
		error = drv_i915_xd_platform_choose(i915_xd_test_accepted_13[index], 0U, 0, 0U, &choice);
		if (error != 0 ||
		    choice.platform->display_ver != 13U ||
		    choice.platform->verified == 0)
			all = 0;
	}

	/* Checks every accepted display 13 id has a verified display 13 row. */
	i915_host_check(all != 0, "accepted: every accepted display 13 id has a verified display 13 row");

	/* A verified row lists only accepted ids; an unverified row lists none of them. */
	rows = drv_i915_xd_platform_count();
	for (row = 0U; row < rows; row++) {
		platform = drv_i915_xd_platform_row(row);
		all = 1;
		for (index = 0U; index < platform->npci_ids; index++) {
			listed = i915_xd_test_listed(i915_xd_test_accepted_12, sizeof(i915_xd_test_accepted_12) / sizeof(i915_xd_test_accepted_12[0]), platform->pci_ids[index]);
			if (listed == 0)
				listed = i915_xd_test_listed(i915_xd_test_accepted_13, sizeof(i915_xd_test_accepted_13) / sizeof(i915_xd_test_accepted_13[0]), platform->pci_ids[index]);
			if (platform->verified != 0 && listed == 0)
				all = 0;
			if (platform->verified == 0 && listed != 0)
				all = 0;
		}

		/* Checks the row's ids. */
		i915_host_check(all != 0, platform->name);
	}
}

/* Checks the shape of every row: ports, wells, PLLs and tables. */
static void
i915_xd_test_shape(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	unsigned row;
	unsigned index;
	unsigned dekel;
	unsigned mg;
	int sane;
	int well1;
	unsigned rows;

	/* Walks every row. */
	rows = drv_i915_xd_platform_count();
	for (row = 0U; row < rows; row++) {
		platform = drv_i915_xd_platform_row(row);
		sane = 1;

		/* Dekel and MG ports have a Type-C port and a north pin; combo ports have neither. */
		dekel = 0U;
		mg = 0U;
		for (index = 0U; index < platform->nports; index++) {
			port = &platform->ports[index];
			if (port->phy_kind == I915_XD_PHY_DEKEL) {
				dekel++;
				if (port->tc == 0U || port->north_pin != port->tc)
					sane = 0;
			} else if (port->phy_kind == I915_XD_PHY_MG) {
				mg++;
				if (port->tc == 0U || port->north_pin != port->tc)
					sane = 0;
			} else if (port->tc != 0U || port->north_pin != 0U) {
				sane = 0;
			}

			/* A port's AUX channel is its DDI. */
			if (port->aux != port->ddi)
				sane = 0;
		}

		/* A platform with Dekel or MG ports has a Type-C flow and that PHY's swing tables, and only those. */
		if (dekel != 0U &&
		    (platform->tc_flow == I915_XD_TC_FLOW_NONE ||
		     platform->swing->dkl_dp_low == NULL))
			sane = 0;
		if (dekel == 0U && platform->swing->dkl_dp_low != NULL)
			sane = 0;
		if (mg != 0U &&
		    (platform->tc_flow == I915_XD_TC_FLOW_NONE ||
		     platform->swing->mg_dp_low == NULL))
			sane = 0;
		if (mg == 0U && platform->swing->mg_dp_low != NULL)
			sane = 0;
		if (dekel == 0U &&
		    mg == 0U &&
		    platform->tc_flow != I915_XD_TC_FLOW_NONE)
			sane = 0;

		/* The first well is PW_1, owned by the DMC. */
		well1 = 0;
		if (platform->nwells != 0U &&
		    platform->wells[0].index == I915_XD_WELL_PW_1 &&
		    (platform->wells[0].flags & I915_XD_WELL_DMC_OWNED) != 0U)
			well1 = 1;
		if (well1 == 0)
			sane = 0;

		/* The tables are all there and of the platform's display version. */
		if (platform->ncdclk == 0U ||
		    platform->ndbuf == 0U ||
		    platform->nplls == 0U ||
		    platform->nsteps == 0U)
			sane = 0;
		if (platform->display_ver == 13U &&
		    (platform->wm_levels != 6U ||
		     platform->hw_sagv_wm == 0 ||
		     platform->dbuf_blocks != 4096U))
			sane = 0;
		if (platform->display_ver == 12U &&
		    (platform->wm_levels != 8U ||
		     platform->hw_sagv_wm != 0 ||
		     platform->dbuf_blocks != 2048U))
			sane = 0;
		if (platform->display_ver == 11U && (platform->wm_levels != 8U || platform->hw_sagv_wm != 0 || platform->dbuf_blocks != 2048U ||
		    platform->pipedmc_base != 0U || platform->swing->edp_hobl != NULL))
			sane = 0;

		/* Checks the row. */
		i915_host_check(sane != 0, platform->name);
	}
}

/* Checks the lookups against values read on the Latitude 5330 and in the tables. */
static void
i915_xd_test_lookups(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_dpll_dp_row *dpll;
	const struct i915_xd_buddy_mask *buddy;
	const struct i915_xd_port_desc *port;
	uint32_t cfgcr0;
	uint32_t cfgcr1;
	struct i915_xd_choice choice;

	/*
	 * HBR at a 38.4 MHz reference on display 13 (the 5330's eDP): the
	 * 19.2 MHz words with the fraction halved give CFGCR0 0x00e001a5 and
	 * CFGCR1 0x88, as the Linux dump of the machine shows.
	 */
	dpll = drv_i915_xd_dpll_dp(38400U, 270000U);
	(void)drv_i915_xd_platform_choose(0x46a6U, 0xcU, 0, 0U, &choice);
	cfgcr0 = 0U;
	cfgcr1 = 0U;
	if (dpll != NULL && choice.platform->frac_wa_38p4 != 0) {
		cfgcr0 = i915_xd_field_put(I915_XD_CFGCR0_DCO_FRACTION, dpll->dco_fraction / 2U) | i915_xd_field_put(I915_XD_CFGCR0_DCO_INTEGER, dpll->dco_integer);
		cfgcr1 = i915_xd_field_put(I915_XD_CFGCR1_QDIV_RATIO, dpll->qdiv_ratio) | i915_xd_field_put(I915_XD_CFGCR1_KDIV, dpll->kdiv) | i915_xd_field_put(I915_XD_CFGCR1_PDIV, dpll->pdiv);
		if (dpll->qdiv_mode != 0U)
			cfgcr1 |= I915_XD_CFGCR1_QDIV_MODE;
	}

	/* Checks HBR at 38.4 MHz gives CFGCR0 0x00e001a5. */
	i915_host_check(cfgcr0 == 0x00e001a5U, "lookups: HBR at 38.4 MHz gives CFGCR0 0x00e001a5");
	i915_host_check(cfgcr1 == 0x00000088U, "lookups: HBR gives CFGCR1 0x88");

	/* Rocket Lake does not halve the fraction; a rate the table lacks has no words. */
	(void)drv_i915_xd_platform_choose(0x4c8aU, 0U, 0, 0U, &choice);
	i915_host_check(choice.platform->frac_wa_38p4 == 0, "lookups: Rocket Lake has no 38.4 MHz fraction workaround");
	i915_host_check(drv_i915_xd_dpll_dp(19200U, 100000U) == NULL, "lookups: an unknown link rate has no words");
	i915_host_check(drv_i915_xd_dpll_dp(24000U, 810000U)->dco_integer == 0x151U, "lookups: HBR3 at 24 MHz");
	i915_host_check(drv_i915_xd_tbt_pll(38400U)->dco_integer == 0x54U && drv_i915_xd_tbt_pll(24000U)->dco_integer == 0x43U, "lookups: the TBT PLL words");

	/* The compensation references: a known voltage and process, and an unknown one. */
	i915_host_check(drv_i915_xd_procmon(0x06000000U)->dw9 == 0x9a00ab25U, "lookups: 1.05 V dot 1 references");
	i915_host_check(drv_i915_xd_procmon(0x1f000000U)->dw9 == 0x62ab67bbU, "lookups: an unknown combination takes the first row");

	/* The buddy masks: the usual table, the Alder Lake-S table, Rocket Lake A0 only, none. */
	(void)drv_i915_xd_platform_choose(0x9a60U, 1U, 0, 0U, &choice);
	buddy = drv_i915_xd_buddy_mask(choice.platform, choice.step, I915_XD_DRAM_LPDDR4, 2U);
	i915_host_check(buddy != NULL && buddy->page_mask == 0x1cU, "lookups: Tiger Lake LPDDR4 x2 buddy mask");
	(void)drv_i915_xd_platform_choose(0x4680U, 0xcU, 0, 0U, &choice);
	buddy = drv_i915_xd_buddy_mask(choice.platform, choice.step, I915_XD_DRAM_DDR4, 2U);
	i915_host_check(buddy != NULL && buddy->page_mask == 0x3U, "lookups: Alder Lake-S takes the workaround masks");
	(void)drv_i915_xd_platform_choose(0x4c8aU, 0U, 0, 0U, &choice);
	buddy = drv_i915_xd_buddy_mask(choice.platform, choice.step, I915_XD_DRAM_DDR4, 2U);
	i915_host_check(buddy != NULL && buddy->page_mask == 0x3U, "lookups: Rocket Lake A0 takes the workaround masks");
	(void)drv_i915_xd_platform_choose(0x4c8aU, 4U, 0, 0U, &choice);
	buddy = drv_i915_xd_buddy_mask(choice.platform, choice.step, I915_XD_DRAM_DDR4, 2U);
	i915_host_check(buddy != NULL && buddy->page_mask == 0x1fU, "lookups: Rocket Lake C0 takes the usual masks");
	buddy = drv_i915_xd_buddy_mask(choice.platform, choice.step, I915_XD_DRAM_DDR5, 4U);
	i915_host_check(buddy == NULL, "lookups: an unknown memory configuration has no mask");

	/* The ports: display 13's TC2 is on Dekel PHY G, Rocket Lake's TC1 on combo PHY C. */
	(void)drv_i915_xd_platform_choose(0x46a6U, 0xcU, 0, 0U, &choice);
	platform = choice.platform;
	port = drv_i915_xd_platform_port(platform, 4U);
	i915_host_check(port != NULL && port->phy_kind == I915_XD_PHY_DEKEL && port->phy == 6U && port->tc == 2U, "lookups: Alder Lake-P TC2");
	i915_host_check(drv_i915_xd_platform_port(platform, 2U) == NULL, "lookups: Alder Lake-P has no DDI C");
	(void)drv_i915_xd_platform_choose(0x4c8aU, 0U, 0, 0U, &choice);
	port = drv_i915_xd_platform_port(choice.platform, 3U);
	i915_host_check(port != NULL && port->phy_kind == I915_XD_PHY_COMBO && port->phy == 2U && port->south_pin_kind == I915_XD_SOUTH_PIN_TC, "lookups: Rocket Lake TC1 is combo PHY C on the TC1 pin");

	/* The display 13 power wells: PW_A powers pipe A only, PW_2 does not; AUX USBC1 may take 500 ms. */
	i915_host_check(platform->wells[3].powers == I915_XD_POWERS_PIPE(0U), "lookups: Alder Lake-P PW_A powers pipe A");
	i915_host_check((platform->wells[2].powers & I915_XD_POWERS_PIPE(0U)) == 0U && (platform->wells[2].powers & I915_XD_POWERS_AUX(3U)) != 0U, "lookups: Alder Lake-P PW_2 powers AUX USBC1 but not pipe A");
	i915_host_check(platform->wells[21].enable_timeout_ms == 500U, "lookups: Alder Lake-P AUX USBC1 may take 500 ms");

	/* The 5330 DBUF of one pipe A: joined, all four slices. */
	i915_host_check(platform->dbuf[0].pipes == 0x1U && platform->dbuf[0].joined != 0 && platform->dbuf[0].slices[0] == 0xfU, "lookups: one pipe A joins all four slices");
}

/* Checks the xd display's start: the description by product and revision, then the south display by the PCH. */
static void
i915_xd_test_start(void)
{
	static struct i915_xd xd;

	/* An Alder Lake-P: display 13, the platform's ADP south display assumed until the PCH is read. */
	drv_i915_xd_start(&xd, NULL, NULL, 0x46a6U, 0x0cU);
	i915_host_check(xd.chosen && xd.choice.platform != NULL && xd.choice.platform->display_ver == 13U, "start: Alder Lake-P is display 13");
	i915_host_check(xd.choice.pch_assumed && xd.choice.pch_kind == I915_PCH_ADP, "start: the south display is assumed before the PCH");

	/* Its ADP PCH is read. */
	drv_i915_xd_south(&xd, 0x46a6U, 0x0cU, 0x5180U);
	i915_host_check(!xd.choice.pch_assumed && xd.choice.pch_kind == I915_PCH_ADP, "start: the ADP PCH is read");

	/* A PCH of another kind keeps the description as it was. */
	drv_i915_xd_south(&xd, 0x46a6U, 0x0cU, 0xa080U);
	i915_host_check(xd.chosen && !xd.choice.pch_assumed && xd.choice.pch_kind == I915_PCH_ADP, "start: a TGP PCH does not change Alder Lake-P");

	/* No PCH found: the platform's own stays assumed. */
	drv_i915_xd_south(&xd, 0x46a6U, 0x0cU, 0U);
	i915_host_check(xd.choice.pch_assumed && xd.choice.pch_kind == I915_PCH_ADP, "start: no PCH keeps the assumed one");

	/* A Tiger Lake is display 12. */
	drv_i915_xd_start(&xd, NULL, NULL, 0x9a49U, 0x01U);
	i915_host_check(xd.chosen && xd.choice.platform->display_ver == 12U, "start: Tiger Lake is display 12");

	/* A Gen9 device (Kaby Lake GT2) and an unknown id have no description and do not use the xd display. */
	drv_i915_xd_start(&xd, NULL, NULL, 0x5917U, 0x07U);
	i915_host_check(!xd.chosen && xd.choice.platform == NULL, "start: Gen9 has no description");
	drv_i915_xd_south(&xd, 0x5917U, 0x07U, 0x9d80U);
	i915_host_check(!xd.chosen, "start: Gen9 stays without one after the PCH");
	drv_i915_xd_start(&xd, NULL, NULL, 0x1234U, 0x00U);
	i915_host_check(!xd.chosen, "start: an unknown id has no description");
}

