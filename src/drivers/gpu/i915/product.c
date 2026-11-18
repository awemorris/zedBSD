/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the graphics devices the driver knows, looked up by PCI
 * device id.
 *
 * Each row is one platform: its id list, its graphics and display versions,
 * the engines it is built with and which rules of the shared workaround
 * lists it takes.  The platforms of one graphics version share their code
 * paths; what tells them apart is written in the row, not tested by name.
 */

#include "product.h"
#include "device-info.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/pci-ids.h"

/* One id of a list, as the PCI id macros apply it. */
#define I915_PRODUCT_ID(id)	(id)

/* The engines of a Gen12 platform with two video decode engines: RCS0, BCS0, VCS0, VCS2 and VECS0. */
#define I915_PRODUCT_ENGINES_TWO_VIDEO	((1U << I915_RCS0) | (1U << I915_BCS0) | (1U << I915_VCS0) | (1U << I915_VCS2) | (1U << I915_VECS0))

/* The engines of a platform with one video decode engine in use: RCS0, BCS0, VCS0 and VECS0. */
#define I915_PRODUCT_ENGINES_ONE_VIDEO	((1U << I915_RCS0) | (1U << I915_BCS0) | (1U << I915_VCS0) | (1U << I915_VECS0))

/*
 * One platform of the table.
 *
 * The rows are constant; a lookup copies the matching row into the
 * caller's i915_product_kind.
 */
struct i915_product_row {
	const uint16_t *ids;
	unsigned id_count;
	unsigned graphics_ver;
	unsigned display_ver;
	int platform;
	uint32_t engine_mask;
	uint32_t platform_wa;
	const char *name;
};

/* The Ice Lake ids. */
static const uint16_t i915_product_icelake[] = { I915_PCI_ICL_IDS(I915_PRODUCT_ID) };

/* The Tiger Lake ids. */
static const uint16_t i915_product_tigerlake[] = { I915_PCI_TGL_IDS(I915_PRODUCT_ID) };

/* The Rocket Lake ids. */
static const uint16_t i915_product_rocketlake[] = { I915_PCI_RKL_IDS(I915_PRODUCT_ID) };

/* The Alder Lake-S ids. */
static const uint16_t i915_product_alderlake_s[] = { I915_PCI_ADLS_IDS(I915_PRODUCT_ID) };

/* The Raptor Lake-S ids, whose GT is Alder Lake-S's. */
static const uint16_t i915_product_raptorlake_s[] = { I915_PCI_RPLS_IDS(I915_PRODUCT_ID) };

/* The Alder Lake-P and -N and Raptor Lake-P and -U ids, which share the XE_LPD display. */
static const uint16_t i915_product_alderlake_p[] = {
	I915_PCI_ADLP_IDS(I915_PRODUCT_ID),
	I915_PCI_ADLN_IDS(I915_PRODUCT_ID),
	I915_PCI_RPLU_IDS(I915_PRODUCT_ID),
	I915_PCI_RPLP_IDS(I915_PRODUCT_ID)
};

/* The Skylake ids. */
static const uint16_t i915_product_skylake[] = { I915_PCI_SKL_IDS(I915_PRODUCT_ID) };

/* The Kaby Lake ids. */
static const uint16_t i915_product_kabylake[] = { I915_PCI_KBL_IDS(I915_PRODUCT_ID) };

/* The Coffee Lake ids. */
static const uint16_t i915_product_coffeelake[] = { I915_PCI_CFL_IDS(I915_PRODUCT_ID) };

/* The Comet Lake ids. */
static const uint16_t i915_product_cometlake[] = { I915_PCI_CML_IDS(I915_PRODUCT_ID) };

/*
 * The platforms, looked up in order; an id is in one list only.
 *
 * Ice Lake is graphics version 11 with its own lists.  Rocket Lake has no
 * second video decode engine.  The render engine's
 * PSMI rule (Wa_1607297627) is taken by Tiger Lake, Rocket Lake and the
 * Alder Lake-P class, not by Alder Lake-S and Raptor Lake-S.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_pci.c
 * (icl_info, tgl_info, rkl_info, adl_s_info, adl_p_info:
 * platform_engine_mask) and
 * drivers/gpu/drm/i915/gt/intel_workarounds.c (rcs_engine_wa_init: the
 * platforms of Wa_1607297627).
 */
static const struct i915_product_row i915_product_rows[] = {
	{
		i915_product_icelake,
		sizeof(i915_product_icelake) / sizeof(i915_product_icelake[0]),
		11U,
		11U,
		I915_PLATFORM_GEN11,
		I915_PRODUCT_ENGINES_TWO_VIDEO,
		0U,
		"Ice Lake"
	},
	{
		i915_product_tigerlake,
		sizeof(i915_product_tigerlake) / sizeof(i915_product_tigerlake[0]),
		12U,
		12U,
		I915_PLATFORM_GEN12,
		I915_PRODUCT_ENGINES_TWO_VIDEO,
		I915_PLATFORM_WA_RENDER_PSMI,
		"Tiger Lake"
	},
	{
		i915_product_rocketlake,
		sizeof(i915_product_rocketlake) / sizeof(i915_product_rocketlake[0]),
		12U,
		12U,
		I915_PLATFORM_GEN12,
		I915_PRODUCT_ENGINES_ONE_VIDEO,
		I915_PLATFORM_WA_RENDER_PSMI,
		"Rocket Lake"
	},
	{
		i915_product_alderlake_s,
		sizeof(i915_product_alderlake_s) / sizeof(i915_product_alderlake_s[0]),
		12U,
		12U,
		I915_PLATFORM_GEN12,
		I915_PRODUCT_ENGINES_TWO_VIDEO,
		0U,
		"Alder Lake-S"
	},
	{
		i915_product_raptorlake_s,
		sizeof(i915_product_raptorlake_s) / sizeof(i915_product_raptorlake_s[0]),
		12U,
		12U,
		I915_PLATFORM_GEN12,
		I915_PRODUCT_ENGINES_TWO_VIDEO,
		0U,
		"Raptor Lake-S"
	},
	{
		i915_product_alderlake_p,
		sizeof(i915_product_alderlake_p) / sizeof(i915_product_alderlake_p[0]),
		12U,
		13U,
		I915_PLATFORM_GEN12,
		I915_PRODUCT_ENGINES_TWO_VIDEO,
		I915_PLATFORM_WA_RENDER_PSMI,
		"Alder Lake-P class (XE_LPD)"
	},
	{
		i915_product_skylake,
		sizeof(i915_product_skylake) / sizeof(i915_product_skylake[0]),
		9U,
		9U,
		I915_PLATFORM_SKYLAKE,
		I915_PRODUCT_ENGINES_ONE_VIDEO,
		0U,
		"Skylake (Gen9, display kept as the firmware left it)"
	},
	{
		i915_product_kabylake,
		sizeof(i915_product_kabylake) / sizeof(i915_product_kabylake[0]),
		9U,
		9U,
		I915_PLATFORM_KABYLAKE,
		I915_PRODUCT_ENGINES_ONE_VIDEO,
		0U,
		"Kaby Lake (Gen9, display kept as the firmware left it)"
	},
	{
		i915_product_coffeelake,
		sizeof(i915_product_coffeelake) / sizeof(i915_product_coffeelake[0]),
		9U,
		9U,
		I915_PLATFORM_COFFEELAKE,
		I915_PRODUCT_ENGINES_ONE_VIDEO,
		0U,
		"Coffee Lake (Gen9.5, display kept as the firmware left it)"
	},
	{
		i915_product_cometlake,
		sizeof(i915_product_cometlake) / sizeof(i915_product_cometlake[0]),
		9U,
		9U,
		I915_PLATFORM_COMETLAKE,
		I915_PRODUCT_ENGINES_ONE_VIDEO,
		0U,
		"Comet Lake (Gen9.5, display kept as the firmware left it)"
	}
};

static int i915_product_listed(uint16_t product, const struct i915_product_row *row);

/*
 * Tells the versions, platform, engines and workaround rules of a PCI
 * device id.
 *
 * The table is explicit.  Returns 0, or ENODEV for an id in no row, which
 * the device start refuses rather than guessing its registers.
 */
int
drv_i915_product_kind(
	uint16_t product,
	struct i915_product_kind *kind)
{
	const struct i915_product_row *row;
	unsigned index;
	int listed;

	/* Finds the platform whose list holds the id. */
	for (index = 0U; index < sizeof(i915_product_rows) / sizeof(i915_product_rows[0]); index++) {
		row = &i915_product_rows[index];
		listed = i915_product_listed(product, row);
		if (listed == 0)
			continue;

		/* Copies what the row says about the platform. */
		kind->graphics_ver = row->graphics_ver;
		kind->display_ver = row->display_ver;
		kind->platform = row->platform;
		kind->engine_mask = row->engine_mask;
		kind->platform_wa = row->platform_wa;
		kind->name = row->name;

		/* Succeeded: the id is a known platform. */
		return 0;
	}

	/* An id in no list has no known registers. */
	return ENODEV;
}

/* Reports nonzero when a PCI device id is in a row's list. */
static int
i915_product_listed(
	uint16_t product,
	const struct i915_product_row *row)
{
	unsigned index;

	/* Looks the id up in the list. */
	for (index = 0U; index < row->id_count; index++) {
		if (row->ids[index] == product)
			return 1;
	}

	/* Not in the list. */
	return 0;
}
