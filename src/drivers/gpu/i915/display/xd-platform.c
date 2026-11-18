/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The platform descriptions of the display 12 and 13 code (see
 * xd-platform.h): the choice of the description for a device, and the
 * lookups in the tables that are not reached through a description.
 */

#include "display-internal.h"
#include "xd-platform.h"

#include "../intel/display-tables-xd.h"
#include "../intel/display-regs-xd9.h"
#include "../intel/display-tables-xd9.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The fields of COMP_DW3 the compensation references are chosen by. */
#define I915_XD_PROCMON_SELECT		(I915_XD_COMP_DW3_PROCESS | I915_XD_COMP_DW3_VOLTAGE)

/* The reference clock (kHz) whose DPLL tables differ from the 19.2 MHz ones. */
#define I915_XD_REF_24MHZ		24000U

static int i915_xd_platform_choose_in(const struct i915_xd_platform *rows, unsigned count, uint16_t device, uint8_t revid, int pch_present, uint16_t pch_device, struct i915_xd_choice *choice);
static const struct i915_xd_platform *i915_xd_platform_by_device(const struct i915_xd_platform *rows, unsigned count, uint16_t device);
static int i915_xd_platform_has_id(const struct i915_xd_platform *platform, uint16_t device);

/*
 * Chooses the description of a device and works out its stepping and
 * its south display.
 *
 * pch_present is zero when no PCH bridge was found.  A PCH the tables do
 * not know (a virtual machine's bridge, for one) is taken to be the
 * platform's own, and the choice says so.  Returns 0, ENODEV for a
 * device that is not a display 12 or 13 GPU, or EOPNOTSUPP for a known
 * GPU beside a south display the xd code does not drive (a Rocket Lake
 * on a Comet Lake PCH).
 */
int
drv_i915_xd_platform_choose(
	uint16_t device,
	uint8_t revid,
	int pch_present,
	uint16_t pch_device,
	struct i915_xd_choice *choice)
{
	int error;

	/* Looks among the rows of display 11 to 13. */
	error = i915_xd_platform_choose_in(
		i915_xd_platforms,
		sizeof(i915_xd_platforms) / sizeof(i915_xd_platforms[0]),
		device,
		revid,
		pch_present,
		pch_device,
		choice);
	if (error != 0)
		return error;

	/* Succeeded: the description, its stepping and its south display are chosen. */
	return 0;
}

/*
 * Chooses the description of a display 9 device (Skylake to Comet Lake)
 * in the same way, from the display 9 rows alone.
 *
 * Returns 0, ENODEV for a device that is not a display 9 GPU, or
 * EOPNOTSUPP for a display 9 GPU beside a south display of another
 * family than its platform's.
 */
int
drv_i915_xd_platform_choose_display9(
	uint16_t device,
	uint8_t revid,
	int pch_present,
	uint16_t pch_device,
	struct i915_xd_choice *choice)
{
	int error;

	/* Looks among the display 9 rows. */
	error = i915_xd_platform_choose_in(
		i915_xd9_platforms,
		sizeof(i915_xd9_platforms) / sizeof(i915_xd9_platforms[0]),
		device,
		revid,
		pch_present,
		pch_device,
		choice);
	if (error != 0)
		return error;

	/* Succeeded: the description and its south display are chosen. */
	return 0;
}

/*
 * Reports the display stepping of a PCI revision on a platform.
 *
 * A revision between two rows of the table takes the next row, and one
 * past the last row is a future stepping.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_step.c
 * (intel_step_init).
 */
uint8_t
drv_i915_xd_platform_step(
	const struct i915_xd_platform *platform,
	uint8_t revid)
{
	unsigned index;

	/* Finds the first row at or after the revision; the rows are in rising order. */
	for (index = 0U; index < platform->nsteps; index++) {
		if (platform->steps[index].revid >= revid)
			return platform->steps[index].display_step;
	}

	/* Succeeded: a revision past the table is a future stepping. */
	return I915_XD_STEP_FUTURE;
}

/*
 * Reports the kind of south display a PCH device id has (enum i915_pch),
 * or I915_PCH_NONE for an id the tables do not know.
 */
int
drv_i915_xd_pch_kind(
	uint16_t pch_device)
{
	uint16_t masked;
	unsigned index;

	/* The kind is read from the upper bits of the id. */
	masked = (uint16_t)(pch_device & I915_XD_PCH_ID_MASK);

	/* Looks the id up. */
	for (index = 0U; index < sizeof(i915_xd_pch_ids) / sizeof(i915_xd_pch_ids[0]); index++) {
		if (i915_xd_pch_ids[index].id == masked)
			return (int)i915_xd_pch_ids[index].kind;
	}

	/* Succeeded: the id is not one of the known PCHs. */
	return I915_PCH_NONE;
}

/*
 * Reports how many platform descriptions there are.
 */
unsigned
drv_i915_xd_platform_count(void)
{
	/* Reports the size of the table. */
	return sizeof(i915_xd_platforms) / sizeof(i915_xd_platforms[0]);
}

/*
 * Reports a platform description by its place in the table, or NULL
 * past its end.
 */
const struct i915_xd_platform *
drv_i915_xd_platform_row(
	unsigned index)
{
	/* A place past the table has no description. */
	if (index >= sizeof(i915_xd_platforms) / sizeof(i915_xd_platforms[0]))
		return NULL;

	/* Succeeded: reports the description. */
	return &i915_xd_platforms[index];
}

/*
 * Reports how many display 9 descriptions there are.
 */
unsigned
drv_i915_xd_platform_display9_count(void)
{
	/* Reports the size of the table. */
	return sizeof(i915_xd9_platforms) / sizeof(i915_xd9_platforms[0]);
}

/*
 * Reports a display 9 description by its place in the table, or NULL
 * past its end.
 */
const struct i915_xd_platform *
drv_i915_xd_platform_display9_row(
	unsigned index)
{
	/* A place past the table has no description. */
	if (index >= sizeof(i915_xd9_platforms) / sizeof(i915_xd9_platforms[0]))
		return NULL;

	/* Succeeded: reports the description. */
	return &i915_xd9_platforms[index];
}

/*
 * Reports a platform's port on a DDI, or NULL when the platform does not
 * use that DDI.
 */
const struct i915_xd_port_desc *
drv_i915_xd_platform_port(
	const struct i915_xd_platform *platform,
	unsigned ddi)
{
	unsigned index;

	/* Looks for the DDI among the platform's ports. */
	for (index = 0U; index < platform->nports; index++) {
		if (platform->ports[index].ddi == ddi)
			return &platform->ports[index];
	}

	/* Succeeded: the platform has no port on the DDI. */
	return NULL;
}

/*
 * Reports the compensation references of a combo PHY for the voltage and
 * process its COMP_DW3 word reports; a combination the table does not
 * know takes the first row.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_get_procmon_ref_values).
 */
const struct i915_xd_procmon *
drv_i915_xd_procmon(
	uint32_t comp_dw3)
{
	uint32_t selected;
	unsigned index;

	/* Keeps the voltage and process fields of the word. */
	selected = comp_dw3 & I915_XD_PROCMON_SELECT;

	/* Looks the combination up. */
	for (index = 0U; index < sizeof(i915_xd_procmon) / sizeof(i915_xd_procmon[0]); index++) {
		if (i915_xd_procmon[index].voltage_process == selected)
			return &i915_xd_procmon[index];
	}

	/* Succeeded: an unknown combination takes the first row. */
	return &i915_xd_procmon[0];
}

/*
 * Reports the combo DPLL words of a DisplayPort link rate at a reference
 * clock, or NULL for a rate the table does not have.  A 38.4 MHz
 * reference uses the 19.2 MHz words.
 */
const struct i915_xd_dpll_dp_row *
drv_i915_xd_dpll_dp(
	unsigned ref_khz,
	unsigned link_khz)
{
	const struct i915_xd_dpll_dp_row *rows;
	unsigned count;
	unsigned index;

	/* Picks the table of the reference clock. */
	rows = i915_xd_dpll_dp_19p2;
	count = sizeof(i915_xd_dpll_dp_19p2) / sizeof(i915_xd_dpll_dp_19p2[0]);
	if (ref_khz == I915_XD_REF_24MHZ) {
		rows = i915_xd_dpll_dp_24;
		count = sizeof(i915_xd_dpll_dp_24) / sizeof(i915_xd_dpll_dp_24[0]);
	}

	/* Looks the link rate up. */
	for (index = 0U; index < count; index++) {
		if (rows[index].link_khz == link_khz)
			return &rows[index];
	}

	/* Succeeded: the table has no such rate. */
	return NULL;
}

/*
 * Reports the DCO words of the Thunderbolt PLL at a reference clock (a
 * 24 MHz reference has its own; every other uses the 19.2 MHz ones).
 */
const struct i915_xd_dpll_dco *
drv_i915_xd_tbt_pll(
	unsigned ref_khz)
{
	/* A 24 MHz reference has its own words. */
	if (ref_khz == I915_XD_REF_24MHZ)
		return &i915_xd_tbt_pll_24;

	/* Succeeded: the 19.2 MHz words serve 19.2 and 38.4 MHz. */
	return &i915_xd_tbt_pll_19p2;
}

/*
 * Reports the bandwidth buddy page mask of a memory configuration, or
 * NULL when the platform's table has none (the buddy is then turned off).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c (tgl_bw_buddy_init).
 */
const struct i915_xd_buddy_mask *
drv_i915_xd_buddy_mask(
	const struct i915_xd_platform *platform,
	uint8_t step,
	unsigned dram_type,
	unsigned channels)
{
	const struct i915_xd_buddy_mask *rows;
	unsigned count;
	unsigned index;

	/* Picks the usual table, or the workaround one where the platform takes it. */
	rows = i915_xd_buddy_masks;
	count = sizeof(i915_xd_buddy_masks) / sizeof(i915_xd_buddy_masks[0]);
	if ((platform->wa & I915_XD_WA_BUDDY_1409767108) != 0U) {
		rows = i915_xd_buddy_masks_wa;
		count = sizeof(i915_xd_buddy_masks_wa) / sizeof(i915_xd_buddy_masks_wa[0]);
	} else if ((platform->wa & I915_XD_WA_BUDDY_1409767108_A0) != 0U && step < I915_XD_STEP_B0) {
		rows = i915_xd_buddy_masks_wa;
		count = sizeof(i915_xd_buddy_masks_wa) / sizeof(i915_xd_buddy_masks_wa[0]);
	}

	/* Looks the memory configuration up. */
	for (index = 0U; index < count; index++) {
		if (rows[index].dram_type != dram_type)
			continue;
		if (rows[index].channels != channels)
			continue;

		/* Succeeded: the row of the channels. */
		return &rows[index];
	}

	/* Succeeded: the table has no mask for the configuration. */
	return NULL;
}

/*
 * Reports the first display 9 description of a platform kind and power
 * class, or NULL when the kind has no part of that class.
 */
const struct i915_xd_platform *
drv_i915_xd_platform_display9_class(
	unsigned kind,
	unsigned power_class)
{
	unsigned index;

	/* Looks for the kind and the class among the rows. */
	for (index = 0U; index < sizeof(i915_xd9_platforms) / sizeof(i915_xd9_platforms[0]); index++) {
		if (i915_xd9_platforms[index].kind != kind)
			continue;
		if (i915_xd9_platforms[index].power_class != power_class)
			continue;

		/* Succeeded: the row of the class. */
		return &i915_xd9_platforms[index];
	}

	/* Succeeded: no part of the kind has the class. */
	return NULL;
}

/* Chooses a description among the rows of one table (see drv_i915_xd_platform_choose). */
static int
i915_xd_platform_choose_in(
	const struct i915_xd_platform *rows,
	unsigned count,
	uint16_t device,
	uint8_t revid,
	int pch_present,
	uint16_t pch_device,
	struct i915_xd_choice *choice)
{
	const struct i915_xd_platform *platform;
	int kind;

	/* Starts from no choice. */
	choice->platform = NULL;
	choice->step = I915_XD_STEP_FUTURE;
	choice->pch_kind = I915_PCH_NONE;
	choice->pch_assumed = 0;

	/* Finds the row of the device. */
	platform = i915_xd_platform_by_device(rows, count, device);
	if (platform == NULL)
		return ENODEV;

	/* Reads the kind of the PCH, when there is one. */
	kind = I915_PCH_NONE;
	if (pch_present != 0)
		kind = drv_i915_xd_pch_kind(pch_device);

	/* A known PCH of another kind than the platform's is refused. */
	if (kind != I915_PCH_NONE && kind != (int)platform->pch_kind)
		return EOPNOTSUPP;

	/* No PCH, or one the tables do not know: the platform's own is assumed. */
	if (kind == I915_PCH_NONE) {
		kind = (int)platform->pch_kind;
		choice->pch_assumed = 1;
	}

	/* Records the choice. */
	choice->platform = platform;
	choice->step = drv_i915_xd_platform_step(platform, revid);
	choice->pch_kind = kind;

	/* Succeeded: the description, its stepping and its south display are chosen. */
	return 0;
}

/* Finds the row of a table that lists a PCI device id. */
static const struct i915_xd_platform *
i915_xd_platform_by_device(
	const struct i915_xd_platform *rows,
	unsigned count,
	uint16_t device)
{
	unsigned index;
	int listed;

	/* Looks through every row's ids. */
	for (index = 0U; index < count; index++) {
		listed = i915_xd_platform_has_id(&rows[index], device);
		if (listed != 0)
			return &rows[index];
	}

	/* Succeeded: no row lists the device. */
	return NULL;
}

/* Tells whether a platform row lists a PCI device id. */
static int
i915_xd_platform_has_id(
	const struct i915_xd_platform *platform,
	uint16_t device)
{
	unsigned index;

	/* Compares the id with each of the row's. */
	for (index = 0U; index < platform->npci_ids; index++) {
		if (platform->pci_ids[index] == device)
			return 1;
	}

	/* Succeeded: the row does not list the device. */
	return 0;
}
