/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The descriptions of the display 9 hardware (Skylake, Kaby Lake,
 * Coffee Lake and Comet Lake) the xd display code reads: the PCI ids of
 * each part, the ports, the DDI buffer tables, and the platform rows that
 * tie them together.
 *
 * A display 9 part has four DDIs with their own buffers (A to D) and a
 * fifth (E) that shares DDI A's lanes; no combo or Type-C PHY, one
 * display buffer slice, and a south display of the Sunrise Point family
 * (Sunrise Point, Kaby Point) or of the Cannon Point family (Cannon
 * Point, Comet Point).  The parts differ in their DDI buffer tables by
 * power class (the H and S parts, the U parts, the Y parts), so each
 * class of each platform has a row of its own.
 *
 * The header holds data only and is included by display/xd-platform.c
 * alone.  Fields the rows leave empty (power wells, PLLs, CD clock steps,
 * combo swing tables) are those the xd code does not yet reach on
 * display 9.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD9_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD9_H

#include <stddef.h>
#include <stdint.h>

/*
 * The PCI device ids of each display 9 row, by platform and power class
 * (the U parts and the Y parts have DDI buffer tables of their own).
 *
 * Values referenced: this tree's src/drivers/gpu/i915/intel/pci-ids.h
 * (I915_PCI_SKL_IDS, I915_PCI_KBL_IDS, I915_PCI_CFL_IDS, I915_PCI_CML_IDS),
 * and Linux v6.8.12 drivers/gpu/drm/i915/intel_device_info.c (the ULT and
 * ULX id lists of Skylake to Comet Lake).
 */
static const uint16_t i915_xd9_ids_skl[] = {
	0x1902, 0x190a, 0x190b, 0x1912, 0x1917, 0x191a, 0x191b, 0x191d,
	0x192a, 0x192b, 0x192d, 0x1932, 0x193a, 0x193b, 0x193d,
};

static const uint16_t i915_xd9_ids_skl_u[] = {
	0x1906, 0x1913, 0x1916, 0x1921, 0x1923, 0x1926, 0x1927,
};

static const uint16_t i915_xd9_ids_skl_y[] = {
	0x190e, 0x1915, 0x191e,
};

static const uint16_t i915_xd9_ids_kbl[] = {
	0x5902, 0x5908, 0x590a, 0x590b, 0x5912, 0x5917, 0x591a, 0x591b,
	0x591d, 0x5923, 0x5927, 0x593b,
};

static const uint16_t i915_xd9_ids_kbl_u[] = {
	0x5906, 0x5913, 0x5916, 0x5921, 0x5926,
};

static const uint16_t i915_xd9_ids_kbl_y[] = {
	0x590e, 0x5915, 0x591c, 0x591e, 0x87c0,
};

static const uint16_t i915_xd9_ids_cfl[] = {
	0x3e90, 0x3e91, 0x3e92, 0x3e93, 0x3e94, 0x3e96, 0x3e98, 0x3e99,
	0x3e9a, 0x3e9b, 0x3e9c,
};

static const uint16_t i915_xd9_ids_cfl_u[] = {
	0x3ea0, 0x3ea1, 0x3ea2, 0x3ea3, 0x3ea4, 0x3ea5, 0x3ea6, 0x3ea7,
	0x3ea8, 0x3ea9,
};

static const uint16_t i915_xd9_ids_cfl_y[] = {
	0x87ca,
};

static const uint16_t i915_xd9_ids_cml[] = {
	0x9ba2, 0x9ba4, 0x9ba5, 0x9ba8, 0x9bc2, 0x9bc4, 0x9bc5, 0x9bc6,
	0x9bc8, 0x9be6, 0x9bf6,
};

static const uint16_t i915_xd9_ids_cml_u[] = {
	0x9b21, 0x9b41, 0x9baa, 0x9bac, 0x9bca, 0x9bcc,
};

/*
 * The ports of display 9: DDIs A to D with their own buffers and AUX
 * channels, and their hotplug pins in the south display (port A's too:
 * the Sunrise Point family carries it in its second hotplug register).
 * DDI E shares DDI A's lanes and has no AUX channel; it is listed and
 * not driven.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DDI_BUF_CTL, DDI_AUX_CTL, SHOTPLUG_CTL, SHOTPLUG_CTL2),
 * Intel Kaby Lake PRM Volume 12, Display Engine (the DDIs and their
 * lane sharing).
 */
static const struct i915_xd_port_desc i915_xd9_ports[] = {
	{ 0, I915_XD_PHY_DDI, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 1, I915_XD_PHY_DDI, 1, 0, 1, 0, I915_XD_SOUTH_PIN_DDI, 1 },
	{ 2, I915_XD_PHY_DDI, 2, 0, 2, 0, I915_XD_SOUTH_PIN_DDI, 2 },
	{ 3, I915_XD_PHY_DDI, 3, 0, 3, 0, I915_XD_SOUTH_PIN_DDI, 3 },
	{ 4, I915_XD_PHY_NONE, 4, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 4 },
};

/*
 * The DDI buffer translation tables of display 9, one row for each level
 * a sink may ask for: the two DDI_BUF_TRANS words and the current boost.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c (the tables named
 * with each below).
 */

/* Skylake H and S, DisplayPort (skl_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_skl_dp[] = {
	{ 0x00002016U, 0x000000a0U, 0x0U },
	{ 0x00005012U, 0x0000009bU, 0x0U },
	{ 0x00007011U, 0x00000088U, 0x0U },
	{ 0x80009010U, 0x000000c0U, 0x1U },
	{ 0x00002016U, 0x0000009bU, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000c0U, 0x1U },
	{ 0x00002016U, 0x000000dfU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x1U }
};

/* Skylake U, DisplayPort (skl_u_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_skl_u_dp[] = {
	{ 0x0000201bU, 0x000000a2U, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000cdU, 0x1U },
	{ 0x80009010U, 0x000000c0U, 0x1U },
	{ 0x0000201bU, 0x0000009dU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x1U },
	{ 0x80007011U, 0x000000c0U, 0x1U },
	{ 0x00002016U, 0x00000088U, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x1U }
};

/* Skylake Y, DisplayPort (skl_y_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_skl_y_dp[] = {
	{ 0x00000018U, 0x000000a2U, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000cdU, 0x3U },
	{ 0x80009010U, 0x000000c0U, 0x3U },
	{ 0x00000018U, 0x0000009dU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U },
	{ 0x80007011U, 0x000000c0U, 0x3U },
	{ 0x00000018U, 0x00000088U, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U }
};

/* Kaby Lake H and S, and Coffee Lake and Comet Lake, DisplayPort (kbl_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_kbl_dp[] = {
	{ 0x00002016U, 0x000000a0U, 0x0U },
	{ 0x00005012U, 0x0000009bU, 0x0U },
	{ 0x00007011U, 0x00000088U, 0x0U },
	{ 0x80009010U, 0x000000c0U, 0x1U },
	{ 0x00002016U, 0x0000009bU, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000c0U, 0x1U },
	{ 0x00002016U, 0x00000097U, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x1U }
};

/* Kaby Lake U, and Coffee Lake U and Comet Lake U, DisplayPort (kbl_u_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_kbl_u_dp[] = {
	{ 0x0000201bU, 0x000000a1U, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000cdU, 0x3U },
	{ 0x80009010U, 0x000000c0U, 0x3U },
	{ 0x0000201bU, 0x0000009dU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U },
	{ 0x80007011U, 0x000000c0U, 0x3U },
	{ 0x00002016U, 0x0000004fU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U }
};

/* Kaby Lake Y and Coffee Lake Y, DisplayPort (kbl_y_ddi_translations_dp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_kbl_y_dp[] = {
	{ 0x00001017U, 0x000000a1U, 0x0U },
	{ 0x00005012U, 0x00000088U, 0x0U },
	{ 0x80007011U, 0x000000cdU, 0x3U },
	{ 0x8000800fU, 0x000000c0U, 0x3U },
	{ 0x00001017U, 0x0000009dU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U },
	{ 0x80007011U, 0x000000c0U, 0x3U },
	{ 0x00001017U, 0x0000004cU, 0x0U },
	{ 0x80005012U, 0x000000c0U, 0x3U }
};

/* The H and S parts, eDP at low swing (skl_ddi_translations_edp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_edp[] = {
	{ 0x00000018U, 0x000000a8U, 0x0U },
	{ 0x00004013U, 0x000000a9U, 0x0U },
	{ 0x00007011U, 0x000000a2U, 0x0U },
	{ 0x00009010U, 0x0000009cU, 0x0U },
	{ 0x00000018U, 0x000000a9U, 0x0U },
	{ 0x00006013U, 0x000000a2U, 0x0U },
	{ 0x00007011U, 0x000000a6U, 0x0U },
	{ 0x00000018U, 0x000000abU, 0x0U },
	{ 0x00007013U, 0x0000009fU, 0x0U },
	{ 0x00000018U, 0x000000dfU, 0x0U }
};

/* The U parts, eDP at low swing (skl_u_ddi_translations_edp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_u_edp[] = {
	{ 0x00000018U, 0x000000a8U, 0x0U },
	{ 0x00004013U, 0x000000a9U, 0x0U },
	{ 0x00007011U, 0x000000a2U, 0x0U },
	{ 0x00009010U, 0x0000009cU, 0x0U },
	{ 0x00000018U, 0x000000a9U, 0x0U },
	{ 0x00006013U, 0x000000a2U, 0x0U },
	{ 0x00007011U, 0x000000a6U, 0x0U },
	{ 0x00002016U, 0x000000abU, 0x0U },
	{ 0x00005013U, 0x0000009fU, 0x0U },
	{ 0x00000018U, 0x000000dfU, 0x0U }
};

/* The Y parts, eDP at low swing (skl_y_ddi_translations_edp). */
static const struct i915_xd9_buf_trans i915_xd9_buf_y_edp[] = {
	{ 0x00000018U, 0x000000a8U, 0x0U },
	{ 0x00004013U, 0x000000abU, 0x0U },
	{ 0x00007011U, 0x000000a4U, 0x0U },
	{ 0x00009010U, 0x000000dfU, 0x0U },
	{ 0x00000018U, 0x000000aaU, 0x0U },
	{ 0x00006013U, 0x000000a4U, 0x0U },
	{ 0x00007011U, 0x0000009dU, 0x0U },
	{ 0x00000018U, 0x000000a0U, 0x0U },
	{ 0x00006012U, 0x000000dfU, 0x0U },
	{ 0x00000018U, 0x0000008aU, 0x0U }
};

/* The H, S and U parts, HDMI (skl_ddi_translations_hdmi). */
static const struct i915_xd9_buf_trans i915_xd9_buf_hdmi[] = {
	{ 0x00000018U, 0x000000acU, 0x0U },
	{ 0x00005012U, 0x0000009dU, 0x0U },
	{ 0x00007011U, 0x00000088U, 0x0U },
	{ 0x00000018U, 0x000000a1U, 0x0U },
	{ 0x00000018U, 0x00000098U, 0x0U },
	{ 0x00004013U, 0x00000088U, 0x0U },
	{ 0x80006012U, 0x000000cdU, 0x1U },
	{ 0x00000018U, 0x000000dfU, 0x0U },
	{ 0x80003015U, 0x000000cdU, 0x1U },
	{ 0x80003015U, 0x000000c0U, 0x1U },
	{ 0x80000018U, 0x000000c0U, 0x1U }
};

/* The Y parts, HDMI (skl_y_ddi_translations_hdmi). */
static const struct i915_xd9_buf_trans i915_xd9_buf_y_hdmi[] = {
	{ 0x00000018U, 0x000000a1U, 0x0U },
	{ 0x00005012U, 0x000000dfU, 0x0U },
	{ 0x80007011U, 0x000000cbU, 0x3U },
	{ 0x00000018U, 0x000000a4U, 0x0U },
	{ 0x00000018U, 0x0000009dU, 0x0U },
	{ 0x00004013U, 0x00000080U, 0x0U },
	{ 0x80006013U, 0x000000c0U, 0x3U },
	{ 0x00000018U, 0x0000008aU, 0x0U },
	{ 0x80003015U, 0x000000c0U, 0x3U },
	{ 0x80003015U, 0x000000c0U, 0x3U },
	{ 0x80000018U, 0x000000c0U, 0x3U }
};

/* Builds the table field of a set from one of the tables above. */
#define I915_XD9_TABLE(rows) { rows, sizeof(rows) / sizeof(rows[0]) }

/*
 * The DDI buffer tables of each platform and power class: Skylake's
 * DisplayPort tables are its own, Kaby Lake's serve Coffee Lake and
 * Comet Lake too; the eDP and HDMI tables follow the power class alone.
 * Every HDMI table's default row is its ninth.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c (the table each
 * platform and class takes, and the HDMI tables' default entry).
 */
static const struct i915_xd9_ddi_buffers i915_xd9_buffers_skl = {
	I915_XD9_TABLE(i915_xd9_buf_skl_dp), I915_XD9_TABLE(i915_xd9_buf_edp), I915_XD9_TABLE(i915_xd9_buf_hdmi), 8U
};

static const struct i915_xd9_ddi_buffers i915_xd9_buffers_skl_u = {
	I915_XD9_TABLE(i915_xd9_buf_skl_u_dp), I915_XD9_TABLE(i915_xd9_buf_u_edp), I915_XD9_TABLE(i915_xd9_buf_hdmi), 8U
};

static const struct i915_xd9_ddi_buffers i915_xd9_buffers_skl_y = {
	I915_XD9_TABLE(i915_xd9_buf_skl_y_dp), I915_XD9_TABLE(i915_xd9_buf_y_edp), I915_XD9_TABLE(i915_xd9_buf_y_hdmi), 8U
};

static const struct i915_xd9_ddi_buffers i915_xd9_buffers_kbl = {
	I915_XD9_TABLE(i915_xd9_buf_kbl_dp), I915_XD9_TABLE(i915_xd9_buf_edp), I915_XD9_TABLE(i915_xd9_buf_hdmi), 8U
};

static const struct i915_xd9_ddi_buffers i915_xd9_buffers_kbl_u = {
	I915_XD9_TABLE(i915_xd9_buf_kbl_u_dp), I915_XD9_TABLE(i915_xd9_buf_u_edp), I915_XD9_TABLE(i915_xd9_buf_hdmi), 8U
};

static const struct i915_xd9_ddi_buffers i915_xd9_buffers_kbl_y = {
	I915_XD9_TABLE(i915_xd9_buf_kbl_y_dp), I915_XD9_TABLE(i915_xd9_buf_y_edp), I915_XD9_TABLE(i915_xd9_buf_y_hdmi), 8U
};

#undef I915_XD9_TABLE

/*
 * The power wells of display 9, all in the driver's request register
 * (PWR_WELL_CTL2): the MISC IO well (0), the IO wells of DDI A and E (1)
 * and of DDIs B to D (2 to 4), power well 1 (14) and power well 2 (15).
 * Power well 1 waits for power gate 1's fuses and the DMC turns it off in
 * DC states; power well 2 waits for power gate 2's and powers pipes B and
 * C, the transcoders A to C, the AUX channels B to D, the VGA plane and
 * the audio.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PWR_WELL_CTL2, FUSE_STATUS), Intel Kaby Lake PRM Volume 12,
 * Display Engine (the power wells and what each powers).
 */
static const struct i915_xd_well_desc i915_xd9_wells[] = {
	{
		"MISC_IO",
		I915_XD_WELL_CONTROL_MAIN,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		0ULL
	},
	{
		"DDI_A_E_IO",
		I915_XD_WELL_CONTROL_MAIN,
		1U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U) | I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"DDI_B_IO",
		I915_XD_WELL_CONTROL_MAIN,
		2U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_C_IO",
		I915_XD_WELL_CONTROL_MAIN,
		3U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(2U)
	},
	{
		"DDI_D_IO",
		I915_XD_WELL_CONTROL_MAIN,
		4U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		14U,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"PW_2",
		I915_XD_WELL_CONTROL_MAIN,
		15U,
		2U,
		I915_XD_WELL_VGA,
		0x6U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(0U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(3U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO
	}
};

/*
 * The DPLLs of display 9: DPLL 0 (which also makes the CD clock, so the
 * firmware's stays), DPLL 1, and the WRPLLs DPLL 2 and 3, each selected
 * for a DDI by its number in DPLL_CTRL2, with its enable and its HDMI
 * configuration words (CFGCR2 follows CFGCR1).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DPLL_CTRL2, LCPLL1_CTL, LCPLL2_CTL, WRPLL_CTL, DPLL_CFGCR1).
 */
static const struct i915_xd_pll_desc i915_xd9_plls[] = {
	{ I915_XD_PLL_COMBO, 0U, 0U, I915_XD9_DPLL0_CTL, 0U, 0U },
	{ I915_XD_PLL_COMBO, 1U, 0U, I915_XD9_DPLL1_CTL, I915_XD9_DPLL1_CFGCR1, I915_XD9_DPLL1_CFGCR1 + 4U },
	{ I915_XD_PLL_COMBO, 2U, 0U, I915_XD9_DPLL2_CTL, I915_XD9_DPLL2_CFGCR1, I915_XD9_DPLL2_CFGCR1 + 4U },
	{ I915_XD_PLL_COMBO, 3U, 0U, I915_XD9_DPLL3_CTL, I915_XD9_DPLL3_CFGCR1, I915_XD9_DPLL3_CFGCR1 + 4U }
};

/*
 * Builds one display 9 row from the parts that differ between rows: the
 * name, the kind, the south display's kind, the PCI ids, the power class
 * and the DDI buffer tables.  Every other field is the same on every
 * display 9 part: three pipes, the ports, the power wells and the DPLLs above, one
 * display buffer slice of 892 blocks, eight watermark levels.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * three pipes, the display buffer's size, the eight watermark levels).
 */
#define I915_XD9_ROW(name, kind, pch, ids, power_class, buffers) {				\
		name,							\
		kind,							\
		9U,							\
		pch,							\
		0x7U,							\
		0,							\
		ids,							\
		sizeof(ids) / sizeof(ids[0]),				\
		NULL,							\
		0U,							\
		i915_xd9_ports,						\
		sizeof(i915_xd9_ports) / sizeof(i915_xd9_ports[0]),	\
		0x0U,							\
		0x0U,							\
		0x0U,							\
		0U,							\
		I915_XD_TC_FLOW_NONE,					\
		I915_XD_TCCOLD_NONE,					\
		i915_xd9_wells,						\
		sizeof(i915_xd9_wells) / sizeof(i915_xd9_wells[0]),	\
		i915_xd9_plls,						\
		sizeof(i915_xd9_plls) / sizeof(i915_xd9_plls[0]),	\
		I915_XD_PLL_MAP_DPLL_CTRL2,				\
		0,							\
		NULL,							\
		0U,							\
		NULL,							\
		0U,							\
		0,							\
		1U,							\
		892U,							\
		NULL,							\
		0U,							\
		NULL,							\
		0x0U,							\
		0x0U,							\
		0,							\
		8U,							\
		0U,							\
		0,							\
		NULL,							\
		0U,							\
		NULL,							\
		0U,							\
		NULL,							\
		I915_XD_FIA_SINGLE,					\
		NULL,							\
		NULL,							\
		power_class,						\
		buffers							\
	}

/*
 * The display 9 rows: each platform's H and S parts, U parts and Y
 * parts.  Skylake and Kaby Lake pair with the Sunrise Point family,
 * Coffee Lake and Comet Lake with the Cannon Point family.
 */
static const struct i915_xd_platform i915_xd9_platforms[] = {
	I915_XD9_ROW("Skylake", I915_XD_KIND_SKL, I915_PCH_SPT, i915_xd9_ids_skl, I915_XD_POWER_CLASS_H, &i915_xd9_buffers_skl),
	I915_XD9_ROW("Skylake U", I915_XD_KIND_SKL, I915_PCH_SPT, i915_xd9_ids_skl_u, I915_XD_POWER_CLASS_U, &i915_xd9_buffers_skl_u),
	I915_XD9_ROW("Skylake Y", I915_XD_KIND_SKL, I915_PCH_SPT, i915_xd9_ids_skl_y, I915_XD_POWER_CLASS_Y, &i915_xd9_buffers_skl_y),
	I915_XD9_ROW("Kaby Lake", I915_XD_KIND_KBL, I915_PCH_SPT, i915_xd9_ids_kbl, I915_XD_POWER_CLASS_H, &i915_xd9_buffers_kbl),
	I915_XD9_ROW("Kaby Lake U", I915_XD_KIND_KBL, I915_PCH_SPT, i915_xd9_ids_kbl_u, I915_XD_POWER_CLASS_U, &i915_xd9_buffers_kbl_u),
	I915_XD9_ROW("Kaby Lake Y", I915_XD_KIND_KBL, I915_PCH_SPT, i915_xd9_ids_kbl_y, I915_XD_POWER_CLASS_Y, &i915_xd9_buffers_kbl_y),
	I915_XD9_ROW("Coffee Lake", I915_XD_KIND_CFL, I915_PCH_CNP, i915_xd9_ids_cfl, I915_XD_POWER_CLASS_H, &i915_xd9_buffers_kbl),
	I915_XD9_ROW("Coffee Lake U", I915_XD_KIND_CFL, I915_PCH_CNP, i915_xd9_ids_cfl_u, I915_XD_POWER_CLASS_U, &i915_xd9_buffers_kbl_u),
	I915_XD9_ROW("Coffee Lake Y", I915_XD_KIND_CFL, I915_PCH_CNP, i915_xd9_ids_cfl_y, I915_XD_POWER_CLASS_Y, &i915_xd9_buffers_kbl_y),
	I915_XD9_ROW("Comet Lake", I915_XD_KIND_CML, I915_PCH_CNP, i915_xd9_ids_cml, I915_XD_POWER_CLASS_H, &i915_xd9_buffers_kbl),
	I915_XD9_ROW("Comet Lake U", I915_XD_KIND_CML, I915_PCH_CNP, i915_xd9_ids_cml_u, I915_XD_POWER_CLASS_U, &i915_xd9_buffers_kbl_u),
};

#undef I915_XD9_ROW

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD9_H */
