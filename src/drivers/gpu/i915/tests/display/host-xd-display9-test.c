/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display 9 descriptions of the xd display code
 * (display/xd-platform.c, intel/display-tables-xd9.h): every Gen9 id the
 * driver accepts (intel/pci-ids.h) finds exactly one display 9 row and no
 * row of display 11 to 13; the row's power class is the one the Gen9 mode
 * set gives the id; the south display's family is checked; and every row
 * has the display 9 shape (three pipes, DDIs A to D on their own buffers,
 * DDI E not driven, one display buffer slice).
 */

#include "host-test.h"

#include "../../display/display-internal.h"
#include "../../display/xd-platform.h"
#include "../../device-info.h"
#include "../../intel/pci-ids.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Spells one PCI id of the tree's id lists as a value. */
#define I915_XD9_TEST_ID(id) (id)

/* The Gen9 ids the driver accepts today, by platform, as device.c lists them. */
static const uint16_t i915_xd9_test_skl[] = { I915_PCI_SKL_IDS(I915_XD9_TEST_ID) };
static const uint16_t i915_xd9_test_kbl[] = { I915_PCI_KBL_IDS(I915_XD9_TEST_ID) };
static const uint16_t i915_xd9_test_cfl[] = { I915_PCI_CFL_IDS(I915_XD9_TEST_ID) };
static const uint16_t i915_xd9_test_cml[] = { I915_PCI_CML_IDS(I915_XD9_TEST_ID) };

/* The power classes a row's name and its class field name: H and S, U, and Y. */
#define I915_XD9_TEST_CLASS_H		0
#define I915_XD9_TEST_CLASS_U		1
#define I915_XD9_TEST_CLASS_Y		2

static void i915_xd9_test_ids(const uint16_t *ids, unsigned count, unsigned kind, const char *name);
static void i915_xd9_test_pch(void);
static void i915_xd9_test_shape(void);
static unsigned i915_xd9_test_rows_listing(uint16_t id);
static int i915_xd9_test_sku_of(const char *row_name);
static int i915_xd9_test_class_sku(unsigned power_class);

/*
 * Runs every check of the display 9 descriptions and reports.
 */
int
main(void)
{
	int status;

	/* The ids of each platform, the south display, the rows' shape. */
	i915_xd9_test_ids(i915_xd9_test_skl, sizeof(i915_xd9_test_skl) / sizeof(i915_xd9_test_skl[0]), I915_XD_KIND_SKL, "skl");
	i915_xd9_test_ids(i915_xd9_test_kbl, sizeof(i915_xd9_test_kbl) / sizeof(i915_xd9_test_kbl[0]), I915_XD_KIND_KBL, "kbl");
	i915_xd9_test_ids(i915_xd9_test_cfl, sizeof(i915_xd9_test_cfl) / sizeof(i915_xd9_test_cfl[0]), I915_XD_KIND_CFL, "cfl");
	i915_xd9_test_ids(i915_xd9_test_cml, sizeof(i915_xd9_test_cml) / sizeof(i915_xd9_test_cml[0]), I915_XD_KIND_CML, "cml");
	i915_xd9_test_pch();
	i915_xd9_test_shape();

	/* The tally. */
	status = i915_host_report("xd_display9_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the display 9 rows describe the parts the driver accepts. */
	return 0;
}

/* Checks the ids of one platform: one display 9 row each, of the platform's kind and the id's power class. */
static void
i915_xd9_test_ids(
	const uint16_t *ids,
	unsigned count,
	unsigned kind,
	const char *name)
{
	struct i915_xd_choice choice;
	unsigned index;
	unsigned rows;
	unsigned wrong_rows;
	unsigned wrong_kind;
	unsigned wrong_class;
	unsigned newer;
	int error;
	int by_name;
	int by_class;

	/* Each id of the platform. */
	wrong_rows = 0U;
	wrong_kind = 0U;
	wrong_class = 0U;
	newer = 0U;
	for (index = 0U; index < count; index++) {
		/* Exactly one display 9 row lists the id. */
		rows = i915_xd9_test_rows_listing(ids[index]);
		if (rows != 1U)
			wrong_rows++;

		/* No row of display 11 to 13 takes it. */
		error = drv_i915_xd_platform_choose(ids[index], 0U, 0, 0U, &choice);
		if (error != ENODEV)
			newer++;

		/* The display 9 choice is the platform's, in the id's power class. */
		error = drv_i915_xd_platform_choose_display9(ids[index], 0U, 0, 0U, &choice);
		if (error != 0 || choice.platform == NULL) {
			wrong_kind++;
			continue;
		}
		if (choice.platform->kind != kind)
			wrong_kind++;
		by_name = i915_xd9_test_sku_of(choice.platform->name);
		by_class = i915_xd9_test_class_sku(choice.platform->power_class);
		if (by_name != by_class) {
			fprintf(stderr, "%s: 0x%04x is in \"%s\", whose class is %d\n", name, ids[index], choice.platform->name, by_class);
			wrong_class++;
		}
	}

	/* The outcome of the platform. */
	i915_host_check(wrong_rows == 0U, name);
	i915_host_check(newer == 0U, "no display 11 to 13 row takes a Gen9 id");
	i915_host_check(wrong_kind == 0U, "the display 9 choice is the platform's");
	i915_host_check(wrong_class == 0U, "the row's power class is the mode set's");
}

/* Checks the south display: the platform's family assumed, kept, or a family of another kind refused. */
static void
i915_xd9_test_pch(void)
{
	struct i915_xd_choice choice;
	int error;

	/* Kaby Lake R (the SV7) without a PCH bridge: Sunrise Point assumed. */
	error = drv_i915_xd_platform_choose_display9(0x5917U, 0U, 0, 0U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_SPT && choice.pch_assumed == 1, "kbl: Sunrise Point assumed");

	/* Kaby Lake beside a Sunrise Point LP bridge: read, not assumed. */
	error = drv_i915_xd_platform_choose_display9(0x5917U, 0U, 1, 0x9d4eU, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_SPT && choice.pch_assumed == 0, "kbl: Sunrise Point read");

	/* Kaby Lake beside a Cannon Point bridge: refused. */
	error = drv_i915_xd_platform_choose_display9(0x5917U, 0U, 1, 0xa305U, &choice);
	i915_host_check(error == EOPNOTSUPP, "kbl: Cannon Point refused");

	/* Comet Lake U beside a Comet Point LP bridge: Cannon Point's family. */
	error = drv_i915_xd_platform_choose_display9(0x9b41U, 0U, 1, 0x0284U, &choice);
	i915_host_check(error == 0 && choice.pch_kind == I915_PCH_CNP && choice.pch_assumed == 0, "cml: Comet Point read");

	/* A display 12 id is not display 9. */
	error = drv_i915_xd_platform_choose_display9(0x9a49U, 0U, 0, 0U, &choice);
	i915_host_check(error == ENODEV, "tgl: not a display 9 id");
}

/* Checks the shape every display 9 row has. */
static void
i915_xd9_test_shape(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	unsigned count;
	unsigned row;
	unsigned ddi;
	unsigned bad;

	/* Eleven rows: three classes of Skylake and Kaby Lake and Coffee Lake, two of Comet Lake. */
	count = drv_i915_xd_platform_display9_count();
	i915_host_check(count == 11U, "rows: eleven display 9 rows");
	i915_host_check(drv_i915_xd_platform_display9_row(count) == NULL, "rows: none past the end");

	/* Each row. */
	bad = 0U;
	for (row = 0U; row < count; row++) {
		platform = drv_i915_xd_platform_display9_row(row);
		if (platform->display_ver != 9U || platform->pipes != 0x7U || platform->nports != 5U)
			bad++;
		if (platform->dbuf_slices != 1U || platform->dbuf_blocks != 892U || platform->wm_levels != 8U)
			bad++;
		if (platform->tc_flow != I915_XD_TC_FLOW_NONE || platform->pll_map != I915_XD_PLL_MAP_DPLL_CTRL2)
			bad++;
		if (platform->nwells != 7U || platform->nplls != 4U)
			bad++;
		if (platform->ddi_buffers == NULL)
			bad++;

		/* DDIs A to D on their own buffers with their AUX channel; DDI E not driven. */
		for (ddi = 0U; ddi < 5U; ddi++) {
			port = drv_i915_xd_platform_port(platform, ddi);
			if (port == NULL) {
				bad++;
				continue;
			}
			if (ddi < 4U && (port->phy_kind != I915_XD_PHY_DDI || port->aux != ddi))
				bad++;
			if (ddi == 4U && port->phy_kind != I915_XD_PHY_NONE)
				bad++;
		}
	}

	/* The outcome of the rows. */
	i915_host_check(bad == 0U, "rows: the display 9 shape");
}

/* Reports how many display 9 rows list an id. */
static unsigned
i915_xd9_test_rows_listing(
	uint16_t id)
{
	const struct i915_xd_platform *platform;
	unsigned row;
	unsigned index;
	unsigned found;

	/* Each row's ids. */
	found = 0U;
	for (row = 0U; row < drv_i915_xd_platform_display9_count(); row++) {
		platform = drv_i915_xd_platform_display9_row(row);
		for (index = 0U; index < platform->npci_ids; index++) {
			if (platform->pci_ids[index] == id)
				found++;
		}
	}

	/* Succeeded: the count. */
	return found;
}

/* Reports the power class a row's name stands for: a U row, a Y row, or the H and S parts. */
static int
i915_xd9_test_sku_of(
	const char *row_name)
{
	size_t length;

	/* The class is the name's last word. */
	length = strlen(row_name);
	if (length > 2U && strcmp(row_name + length - 2U, " U") == 0)
		return I915_XD9_TEST_CLASS_U;
	if (length > 2U && strcmp(row_name + length - 2U, " Y") == 0)
		return I915_XD9_TEST_CLASS_Y;

	/* Succeeded: the H and S parts. */
	return I915_XD9_TEST_CLASS_H;
}

/* Reports the mode set's power class of a row's power class. */
static int
i915_xd9_test_class_sku(
	unsigned power_class)
{
	/* The U parts and the Y parts. */
	if (power_class == I915_XD_POWER_CLASS_U)
		return I915_XD9_TEST_CLASS_U;
	if (power_class == I915_XD_POWER_CLASS_Y)
		return I915_XD9_TEST_CLASS_Y;

	/* Succeeded: the H and S parts. */
	return I915_XD9_TEST_CLASS_H;
}
