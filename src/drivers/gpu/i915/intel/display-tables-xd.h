/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tables of the display 12 and 13 hardware the xd display code
 * (display/xd-*.c) programs from: the transmit swing of the combo and
 * Dekel PHYs, the combo DPLL words of the DisplayPort link rates, the
 * CD clock steps, the display buffer slices, the memory bus credits,
 * the bandwidth buddy masks, the compensation references, the power
 * wells and what each of them powers, the ports, the PLLs, the
 * steppings, the PCI and PCH ids, and the platform descriptions that
 * tie them together.
 *
 * The types of the tables are in display/xd-platform.h.  The header
 * holds data only and is included by display/xd-platform.c alone: every
 * table is reached through the platform description that xd-platform.c
 * chooses for the device, or through its lookups.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD_H

#include <stddef.h>
#include <stdint.h>

#include "display-regs-xd.h"
#include "xd-vbt-layout.h"
#include "../display/xd-platform.h"

/*
 * The combo PHY swing tables every platform shares: eDP above 5.4 GHz
 * (and DisplayPort above 2.7 GHz on Ice Lake), eDP with the VBT's low
 * swing, HDMI (its last row is the default level), and eDP with the
 * VBT's HOBL setting (one row repeated for every level).
 * Each row: swing select, N scalar, cursor coefficient, post cursor 1,
 * post cursor 2.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_icl_combo_phy_trans_dp_hbr2_edp_hbr3, _icl_combo_phy_trans_edp_hbr2,
 * _icl_combo_phy_trans_hdmi, _tgl_combo_phy_trans_edp_hbr2_hobl).
 */
static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_hbr3_common_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x71, 0x2f, 0x10, 0x00 },
	{ 0x6, 0x7f, 0x2b, 0x14, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x6c, 0x3c, 0x03, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_hbr3_common = {
	i915_xd_combo_swing_edp_hbr3_common_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_low_common_rows[] = {
	{ 0x0, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x8, 0x7f, 0x38, 0x07, 0x00 },
	{ 0x1, 0x7f, 0x33, 0x0c, 0x00 },
	{ 0x9, 0x7f, 0x31, 0x0e, 0x00 },
	{ 0x8, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x1, 0x7f, 0x38, 0x07, 0x00 },
	{ 0x9, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x1, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x9, 0x7f, 0x38, 0x07, 0x00 },
	{ 0x9, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_low_common = {
	i915_xd_combo_swing_edp_low_common_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_hdmi_rows[] = {
	{ 0xa, 0x60, 0x3f, 0x00, 0x00 },
	{ 0xb, 0x73, 0x36, 0x09, 0x00 },
	{ 0x6, 0x7f, 0x31, 0x0e, 0x00 },
	{ 0xb, 0x73, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x37, 0x08, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_hdmi = {
	i915_xd_combo_swing_hdmi_rows,
	7,
	6
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_hobl_rows[] = {
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_hobl = {
	i915_xd_combo_swing_edp_hobl_rows,
	9,
	0
};

/*
 * The Tiger Lake combo PHY DisplayPort swing tables: up to 2.7 GHz,
 * above it, and above it on the UY parts.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_tgl_combo_phy_trans_dp_hbr, _tgl_combo_phy_trans_dp_hbr2,
 * _tgl_uy_combo_phy_trans_dp_hbr2).
 */
static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_low_tgl_rows[] = {
	{ 0xa, 0x32, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x71, 0x2f, 0x10, 0x00 },
	{ 0x6, 0x7d, 0x2b, 0x14, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x6c, 0x3c, 0x03, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_low_tgl = {
	i915_xd_combo_swing_dp_low_tgl_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_high_tgl_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x63, 0x2f, 0x10, 0x00 },
	{ 0x6, 0x7f, 0x2b, 0x14, 0x00 },
	{ 0xa, 0x47, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x63, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x61, 0x3c, 0x03, 0x00 },
	{ 0x6, 0x7b, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_high_tgl = {
	i915_xd_combo_swing_dp_high_tgl_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_high_tgl_uy_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x36, 0x09, 0x00 },
	{ 0xc, 0x60, 0x32, 0x0d, 0x00 },
	{ 0xc, 0x7f, 0x2d, 0x12, 0x00 },
	{ 0xc, 0x47, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x6f, 0x36, 0x09, 0x00 },
	{ 0x6, 0x7d, 0x32, 0x0d, 0x00 },
	{ 0x6, 0x60, 0x3c, 0x03, 0x00 },
	{ 0x6, 0x7f, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_high_tgl_uy = {
	i915_xd_combo_swing_dp_high_tgl_uy_rows,
	10,
	0
};

/*
 * The Rocket Lake combo PHY DisplayPort swing tables: up to 2.7 GHz and
 * above it.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_rkl_combo_phy_trans_dp_hbr, _rkl_combo_phy_trans_dp_hbr2_hbr3).
 */
static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_low_rkl_rows[] = {
	{ 0xa, 0x2f, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x63, 0x2f, 0x10, 0x00 },
	{ 0x6, 0x7d, 0x2a, 0x15, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x6e, 0x3e, 0x01, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_low_rkl = {
	i915_xd_combo_swing_dp_low_rkl_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_high_rkl_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x50, 0x38, 0x07, 0x00 },
	{ 0xc, 0x61, 0x33, 0x0c, 0x00 },
	{ 0x6, 0x7f, 0x2e, 0x11, 0x00 },
	{ 0xa, 0x47, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x5f, 0x38, 0x07, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x5f, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7e, 0x36, 0x09, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_high_rkl = {
	i915_xd_combo_swing_dp_high_rkl_rows,
	10,
	0
};

/*
 * The Alder Lake-S combo PHY swing tables: DisplayPort above 2.7 GHz,
 * eDP with the VBT's low swing, and eDP above 5.4 GHz (DisplayPort up
 * to 2.7 GHz uses the Tiger Lake table).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_adls_combo_phy_trans_dp_hbr2_hbr3, _adls_combo_phy_trans_edp_hbr2,
 * _adls_combo_phy_trans_edp_hbr3).
 */
static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_high_adls_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x63, 0x31, 0x0e, 0x00 },
	{ 0x6, 0x7f, 0x2c, 0x13, 0x00 },
	{ 0xa, 0x47, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x63, 0x37, 0x08, 0x00 },
	{ 0x6, 0x73, 0x32, 0x0d, 0x00 },
	{ 0xc, 0x58, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_high_adls = {
	i915_xd_combo_swing_dp_high_adls_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_low_adls_rows[] = {
	{ 0x9, 0x73, 0x3d, 0x02, 0x00 },
	{ 0x9, 0x7a, 0x3c, 0x03, 0x00 },
	{ 0x9, 0x7f, 0x3b, 0x04, 0x00 },
	{ 0x4, 0x6c, 0x33, 0x0c, 0x00 },
	{ 0x2, 0x73, 0x3a, 0x05, 0x00 },
	{ 0x2, 0x7c, 0x38, 0x07, 0x00 },
	{ 0x4, 0x5a, 0x36, 0x09, 0x00 },
	{ 0x4, 0x57, 0x3d, 0x02, 0x00 },
	{ 0x4, 0x65, 0x38, 0x07, 0x00 },
	{ 0x4, 0x6c, 0x3a, 0x05, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_low_adls = {
	i915_xd_combo_swing_edp_low_adls_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_hbr3_adls_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x63, 0x31, 0x0e, 0x00 },
	{ 0x6, 0x7f, 0x2c, 0x13, 0x00 },
	{ 0xa, 0x47, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x63, 0x37, 0x08, 0x00 },
	{ 0x6, 0x73, 0x32, 0x0d, 0x00 },
	{ 0xc, 0x58, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_hbr3_adls = {
	i915_xd_combo_swing_edp_hbr3_adls_rows,
	10,
	0
};

/*
 * The display 13 combo PHY swing tables: DisplayPort up to 2.7 GHz and
 * above it, eDP with the VBT's low swing, and eDP above 5.4 GHz.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_adlp_combo_phy_trans_dp_hbr, _adlp_combo_phy_trans_dp_hbr2_hbr3,
 * _adlp_combo_phy_trans_edp_hbr2, _adlp_combo_phy_trans_dp_hbr2_edp_hbr3).
 */
static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_low_adlp_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x71, 0x31, 0x0e, 0x00 },
	{ 0x6, 0x7f, 0x2c, 0x13, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x2f, 0x10, 0x00 },
	{ 0xc, 0x7c, 0x3c, 0x03, 0x00 },
	{ 0x6, 0x7f, 0x35, 0x0a, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_low_adlp = {
	i915_xd_combo_swing_dp_low_adlp_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_dp_high_adlp_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x71, 0x30, 0x0f, 0x00 },
	{ 0x6, 0x7f, 0x2b, 0x14, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x30, 0x0f, 0x00 },
	{ 0xc, 0x63, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x38, 0x07, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_dp_high_adlp = {
	i915_xd_combo_swing_dp_high_adlp_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_low_adlp_rows[] = {
	{ 0x4, 0x50, 0x38, 0x07, 0x00 },
	{ 0x4, 0x58, 0x35, 0x0a, 0x00 },
	{ 0x4, 0x60, 0x34, 0x0b, 0x00 },
	{ 0x4, 0x6a, 0x32, 0x0d, 0x00 },
	{ 0x4, 0x5e, 0x38, 0x07, 0x00 },
	{ 0x4, 0x61, 0x36, 0x09, 0x00 },
	{ 0x4, 0x6b, 0x34, 0x0b, 0x00 },
	{ 0x4, 0x69, 0x39, 0x06, 0x00 },
	{ 0x4, 0x73, 0x37, 0x08, 0x00 },
	{ 0x4, 0x7a, 0x38, 0x07, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_low_adlp = {
	i915_xd_combo_swing_edp_low_adlp_rows,
	10,
	0
};

static const struct i915_xd_combo_swing i915_xd_combo_swing_edp_hbr3_adlp_rows[] = {
	{ 0xa, 0x35, 0x3f, 0x00, 0x00 },
	{ 0xa, 0x4f, 0x37, 0x08, 0x00 },
	{ 0xc, 0x71, 0x30, 0x0f, 0x00 },
	{ 0x6, 0x7f, 0x2b, 0x14, 0x00 },
	{ 0xa, 0x4c, 0x3f, 0x00, 0x00 },
	{ 0xc, 0x73, 0x34, 0x0b, 0x00 },
	{ 0x6, 0x7f, 0x30, 0x0f, 0x00 },
	{ 0xc, 0x63, 0x3f, 0x00, 0x00 },
	{ 0x6, 0x7f, 0x38, 0x07, 0x00 },
	{ 0x6, 0x7f, 0x3f, 0x00, 0x00 },
};

static const struct i915_xd_combo_swing_table i915_xd_combo_swing_edp_hbr3_adlp = {
	i915_xd_combo_swing_edp_hbr3_adlp_rows,
	10,
	0
};

/*
 * The Dekel PHY swing tables: Tiger Lake DisplayPort up to 2.7 GHz and
 * above it, HDMI on both platforms (its last row is the default level),
 * and display 13 DisplayPort up to 2.7 GHz and above it.
 * Each row: voltage swing control, de-emphasis, preshoot.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_tgl_dkl_phy_trans_dp_hbr, _tgl_dkl_phy_trans_dp_hbr2,
 * _tgl_dkl_phy_trans_hdmi, _adlp_dkl_phy_trans_dp_hbr,
 * _adlp_dkl_phy_trans_dp_hbr2_hbr3).
 */
static const struct i915_xd_dkl_swing i915_xd_dkl_swing_dp_low_tgl_rows[] = {
	{ 0x7, 0x00, 0x0 },
	{ 0x5, 0x05, 0x0 },
	{ 0x2, 0x0b, 0x0 },
	{ 0x0, 0x18, 0x0 },
	{ 0x5, 0x00, 0x0 },
	{ 0x2, 0x08, 0x0 },
	{ 0x0, 0x14, 0x0 },
	{ 0x2, 0x00, 0x0 },
	{ 0x0, 0x0b, 0x0 },
	{ 0x0, 0x00, 0x0 },
};

static const struct i915_xd_dkl_swing_table i915_xd_dkl_swing_dp_low_tgl = {
	i915_xd_dkl_swing_dp_low_tgl_rows,
	10,
	0
};

static const struct i915_xd_dkl_swing i915_xd_dkl_swing_dp_high_tgl_rows[] = {
	{ 0x7, 0x00, 0x0 },
	{ 0x5, 0x05, 0x0 },
	{ 0x2, 0x0b, 0x0 },
	{ 0x0, 0x19, 0x0 },
	{ 0x5, 0x00, 0x0 },
	{ 0x2, 0x08, 0x0 },
	{ 0x0, 0x14, 0x0 },
	{ 0x2, 0x00, 0x0 },
	{ 0x0, 0x0b, 0x0 },
	{ 0x0, 0x00, 0x0 },
};

static const struct i915_xd_dkl_swing_table i915_xd_dkl_swing_dp_high_tgl = {
	i915_xd_dkl_swing_dp_high_tgl_rows,
	10,
	0
};

static const struct i915_xd_dkl_swing i915_xd_dkl_swing_hdmi_rows[] = {
	{ 0x7, 0x00, 0x0 },
	{ 0x6, 0x00, 0x0 },
	{ 0x4, 0x00, 0x0 },
	{ 0x2, 0x00, 0x0 },
	{ 0x0, 0x00, 0x0 },
	{ 0x0, 0x05, 0x0 },
	{ 0x0, 0x06, 0x0 },
	{ 0x0, 0x07, 0x0 },
	{ 0x0, 0x08, 0x0 },
	{ 0x0, 0x0a, 0x0 },
};

static const struct i915_xd_dkl_swing_table i915_xd_dkl_swing_hdmi = {
	i915_xd_dkl_swing_hdmi_rows,
	10,
	9
};

static const struct i915_xd_dkl_swing i915_xd_dkl_swing_dp_low_adlp_rows[] = {
	{ 0x7, 0x01, 0x0 },
	{ 0x5, 0x06, 0x0 },
	{ 0x2, 0x0b, 0x0 },
	{ 0x0, 0x17, 0x0 },
	{ 0x5, 0x00, 0x0 },
	{ 0x2, 0x08, 0x0 },
	{ 0x0, 0x14, 0x0 },
	{ 0x2, 0x00, 0x0 },
	{ 0x0, 0x0b, 0x0 },
	{ 0x0, 0x00, 0x0 },
};

static const struct i915_xd_dkl_swing_table i915_xd_dkl_swing_dp_low_adlp = {
	i915_xd_dkl_swing_dp_low_adlp_rows,
	10,
	0
};

static const struct i915_xd_dkl_swing i915_xd_dkl_swing_dp_high_adlp_rows[] = {
	{ 0x7, 0x00, 0x0 },
	{ 0x5, 0x04, 0x0 },
	{ 0x2, 0x0a, 0x0 },
	{ 0x0, 0x18, 0x0 },
	{ 0x5, 0x00, 0x0 },
	{ 0x2, 0x06, 0x0 },
	{ 0x0, 0x14, 0x0 },
	{ 0x2, 0x00, 0x0 },
	{ 0x0, 0x09, 0x0 },
	{ 0x0, 0x00, 0x0 },
};

static const struct i915_xd_dkl_swing_table i915_xd_dkl_swing_dp_high_adlp = {
	i915_xd_dkl_swing_dp_high_adlp_rows,
	10,
	0
};

/*
 * The MG PHY swing tables of Ice Lake: DisplayPort up to 2.7 GHz and
 * above it, and HDMI (its last row is the default).  Each row:
 * de-emphasis override bits 17..12, 11..6 and 5..0.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (_icl_mg_phy_trans_rbr_hbr, _icl_mg_phy_trans_hbr2_hbr3,
 * _icl_mg_phy_trans_hdmi, icl_mg_phy_trans_hdmi).
 */
static const struct i915_xd_mg_swing i915_xd_mg_swing_dp_low_icl_rows[] = {
	{ 0x00, 0x18, 0x00 },
	{ 0x05, 0x1d, 0x00 },
	{ 0x0c, 0x24, 0x00 },
	{ 0x14, 0x2b, 0x00 },
	{ 0x00, 0x21, 0x00 },
	{ 0x08, 0x2b, 0x00 },
	{ 0x0f, 0x30, 0x00 },
	{ 0x03, 0x31, 0x00 },
	{ 0x0b, 0x34, 0x00 },
	{ 0x00, 0x3f, 0x00 },
};

static const struct i915_xd_mg_swing_table i915_xd_mg_swing_dp_low_icl = {
	i915_xd_mg_swing_dp_low_icl_rows,
	sizeof(i915_xd_mg_swing_dp_low_icl_rows) / sizeof(i915_xd_mg_swing_dp_low_icl_rows[0]),
	0U
};

static const struct i915_xd_mg_swing i915_xd_mg_swing_dp_high_icl_rows[] = {
	{ 0x00, 0x18, 0x00 },
	{ 0x05, 0x1d, 0x00 },
	{ 0x0c, 0x24, 0x00 },
	{ 0x14, 0x2b, 0x00 },
	{ 0x00, 0x26, 0x00 },
	{ 0x07, 0x2c, 0x00 },
	{ 0x0c, 0x33, 0x00 },
	{ 0x00, 0x2e, 0x00 },
	{ 0x09, 0x36, 0x00 },
	{ 0x00, 0x3f, 0x00 },
};

static const struct i915_xd_mg_swing_table i915_xd_mg_swing_dp_high_icl = {
	i915_xd_mg_swing_dp_high_icl_rows,
	sizeof(i915_xd_mg_swing_dp_high_icl_rows) / sizeof(i915_xd_mg_swing_dp_high_icl_rows[0]),
	0U
};

static const struct i915_xd_mg_swing i915_xd_mg_swing_hdmi_icl_rows[] = {
	{ 0x00, 0x1a, 0x00 },
	{ 0x00, 0x20, 0x00 },
	{ 0x00, 0x29, 0x00 },
	{ 0x00, 0x32, 0x00 },
	{ 0x00, 0x3f, 0x00 },
	{ 0x05, 0x3a, 0x00 },
	{ 0x06, 0x39, 0x00 },
	{ 0x07, 0x38, 0x00 },
	{ 0x08, 0x37, 0x00 },
	{ 0x09, 0x36, 0x00 },
};

static const struct i915_xd_mg_swing_table i915_xd_mg_swing_hdmi_icl = {
	i915_xd_mg_swing_hdmi_icl_rows,
	sizeof(i915_xd_mg_swing_hdmi_icl_rows) / sizeof(i915_xd_mg_swing_hdmi_icl_rows[0]),
	sizeof(i915_xd_mg_swing_hdmi_icl_rows) / sizeof(i915_xd_mg_swing_hdmi_icl_rows[0]) - 1U
};

/*
 * The swing tables of each platform, by the case they serve.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi_buf_trans.c
 * (tgl_get_combo_buf_trans, tgl_get_dkl_buf_trans, rkl_get_combo_buf_trans,
 * adls_get_combo_buf_trans, adlp_get_combo_buf_trans,
 * adlp_get_dkl_buf_trans, icl_get_combo_buf_trans,
 * icl_get_combo_buf_trans_edp, icl_get_combo_buf_trans_dp,
 * icl_get_mg_buf_trans, icl_get_mg_buf_trans_dp: Ice Lake's combo
 * DisplayPort takes one table at every rate, and its eDP no HOBL table).
 */
static const struct i915_xd_swing_set i915_xd_swing_tgl = {
	&i915_xd_combo_swing_dp_low_tgl,
	&i915_xd_combo_swing_dp_high_tgl,
	&i915_xd_combo_swing_edp_low_common,
	&i915_xd_combo_swing_edp_hobl,
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_hdmi,
	&i915_xd_dkl_swing_dp_low_tgl,
	&i915_xd_dkl_swing_dp_high_tgl,
	&i915_xd_dkl_swing_hdmi,
	NULL,
	NULL,
	NULL
};

static const struct i915_xd_swing_set i915_xd_swing_tgl_uy = {
	&i915_xd_combo_swing_dp_low_tgl,
	&i915_xd_combo_swing_dp_high_tgl_uy,
	&i915_xd_combo_swing_edp_low_common,
	&i915_xd_combo_swing_edp_hobl,
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_hdmi,
	&i915_xd_dkl_swing_dp_low_tgl,
	&i915_xd_dkl_swing_dp_high_tgl,
	&i915_xd_dkl_swing_hdmi,
	NULL,
	NULL,
	NULL
};

static const struct i915_xd_swing_set i915_xd_swing_rkl = {
	&i915_xd_combo_swing_dp_low_rkl,
	&i915_xd_combo_swing_dp_high_rkl,
	&i915_xd_combo_swing_edp_low_common,
	&i915_xd_combo_swing_edp_hobl,
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_hdmi,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static const struct i915_xd_swing_set i915_xd_swing_adls = {
	&i915_xd_combo_swing_dp_low_tgl,
	&i915_xd_combo_swing_dp_high_adls,
	&i915_xd_combo_swing_edp_low_adls,
	&i915_xd_combo_swing_edp_hobl,
	&i915_xd_combo_swing_edp_hbr3_adls,
	&i915_xd_combo_swing_hdmi,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static const struct i915_xd_swing_set i915_xd_swing_adlp = {
	&i915_xd_combo_swing_dp_low_adlp,
	&i915_xd_combo_swing_dp_high_adlp,
	&i915_xd_combo_swing_edp_low_adlp,
	&i915_xd_combo_swing_edp_hobl,
	&i915_xd_combo_swing_edp_hbr3_adlp,
	&i915_xd_combo_swing_hdmi,
	&i915_xd_dkl_swing_dp_low_adlp,
	&i915_xd_dkl_swing_dp_high_adlp,
	&i915_xd_dkl_swing_hdmi,
	NULL,
	NULL,
	NULL
};

static const struct i915_xd_swing_set i915_xd_swing_icl = {
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_edp_low_common,
	NULL,
	&i915_xd_combo_swing_edp_hbr3_common,
	&i915_xd_combo_swing_hdmi,
	NULL,
	NULL,
	NULL,
	&i915_xd_mg_swing_dp_low_icl,
	&i915_xd_mg_swing_dp_high_icl,
	&i915_xd_mg_swing_hdmi_icl
};

/*
 * The combo DPLL words of the DisplayPort link rates at a 19.2 MHz
 * reference (also used at 38.4 MHz, where the platforms with the
 * fraction workaround write the fraction halved) and at a 24 MHz
 * reference.  Each row: link rate (kHz), DCO integer, DCO fraction, Q
 * divider ratio and mode, K and P dividers in their register encoding.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_dp_combo_pll_19_2MHz_values, icl_dp_combo_pll_24MHz_values).
 */
static const struct i915_xd_dpll_dp_row i915_xd_dpll_dp_19p2[] = {
	{ 540000, 0x1a5, 0x7000, 0, 0, 1, 0x2 },
	{ 270000, 0x1a5, 0x7000, 0, 0, 2, 0x2 },
	{ 162000, 0x1a5, 0x7000, 0, 0, 2, 0x4 },
	{ 324000, 0x1a5, 0x7000, 0, 0, 1, 0x4 },
	{ 216000, 0x1c2, 0x0000, 2, 1, 2, 0x1 },
	{ 432000, 0x1c2, 0x0000, 0, 0, 2, 0x1 },
	{ 648000, 0x1fa, 0x2000, 0, 0, 1, 0x2 },
	{ 810000, 0x1a5, 0x7000, 0, 0, 1, 0x1 },
};

static const struct i915_xd_dpll_dp_row i915_xd_dpll_dp_24[] = {
	{ 540000, 0x151, 0x4000, 0, 0, 1, 0x2 },
	{ 270000, 0x151, 0x4000, 0, 0, 2, 0x2 },
	{ 162000, 0x151, 0x4000, 0, 0, 2, 0x4 },
	{ 324000, 0x151, 0x4000, 0, 0, 1, 0x4 },
	{ 216000, 0x168, 0x0000, 2, 1, 2, 0x1 },
	{ 432000, 0x168, 0x0000, 0, 0, 2, 0x1 },
	{ 648000, 0x195, 0x0000, 0, 0, 1, 0x2 },
	{ 810000, 0x151, 0x4000, 0, 0, 1, 0x1 },
};

/*
 * The DCO words of the Thunderbolt PLL on display 12 and 13 at a 19.2
 * or 38.4 MHz reference and at a 24 MHz reference (its dividers are not
 * used).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (tgl_tbt_pll_19_2MHz_values, tgl_tbt_pll_24MHz_values).
 */
static const struct i915_xd_dpll_dco i915_xd_tbt_pll_19p2 = { 0x54, 0x3000 };

static const struct i915_xd_dpll_dco i915_xd_tbt_pll_24 = { 0x43, 0x4000 };

/*
 * The words of Ice Lake's Thunderbolt PLL at a 19.2 or 38.4 MHz reference
 * and at a 24 MHz reference: unlike the later PLL its dividers are used.
 * Each: no link rate, DCO integer and fraction, Q divider ratio and mode,
 * and the K and P dividers in their register codes.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_tbt_pll_19_2MHz_values, icl_tbt_pll_24MHz_values, icl_calc_tbt_pll).
 */
static const struct i915_xd_dpll_dp_row i915_xd_tbt_pll_icl_19p2 = { 0, 0x1a5, 0x7000, 0, 0, 1, 0x4 };

static const struct i915_xd_dpll_dp_row i915_xd_tbt_pll_icl_24 = { 0, 0x151, 0x4000, 0, 0, 1, 0x4 };

/*
 * The CD clock steps: Tiger Lake and Alder Lake-S, Rocket Lake (the
 * same clocks with doubled ratios and dividers), display 13, and
 * Raptor Lake-U (display 13 with a 480 MHz step).  Each row: reference
 * (kHz), CD clock (kHz), PLL ratio, CD2X divider doubled.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (icl_cdclk_table, rkl_cdclk_table, adlp_cdclk_table, rplu_cdclk_table).
 */
static const struct i915_xd_cdclk_row i915_xd_cdclk_tgl[] = {
	{ 19200, 172800, 18, 2 },
	{ 19200, 192000, 20, 2 },
	{ 19200, 307200, 32, 2 },
	{ 19200, 326400, 68, 4 },
	{ 19200, 556800, 58, 2 },
	{ 19200, 652800, 68, 2 },
	{ 24000, 180000, 15, 2 },
	{ 24000, 192000, 16, 2 },
	{ 24000, 312000, 26, 2 },
	{ 24000, 324000, 54, 4 },
	{ 24000, 552000, 46, 2 },
	{ 24000, 648000, 54, 2 },
	{ 38400, 172800, 9, 2 },
	{ 38400, 192000, 10, 2 },
	{ 38400, 307200, 16, 2 },
	{ 38400, 326400, 34, 4 },
	{ 38400, 556800, 29, 2 },
	{ 38400, 652800, 34, 2 },
};

static const struct i915_xd_cdclk_row i915_xd_cdclk_rkl[] = {
	{ 19200, 172800, 36, 4 },
	{ 19200, 192000, 40, 4 },
	{ 19200, 307200, 64, 4 },
	{ 19200, 326400, 136, 8 },
	{ 19200, 556800, 116, 4 },
	{ 19200, 652800, 136, 4 },
	{ 24000, 180000, 30, 4 },
	{ 24000, 192000, 32, 4 },
	{ 24000, 312000, 52, 4 },
	{ 24000, 324000, 108, 8 },
	{ 24000, 552000, 92, 4 },
	{ 24000, 648000, 108, 4 },
	{ 38400, 172800, 18, 4 },
	{ 38400, 192000, 20, 4 },
	{ 38400, 307200, 32, 4 },
	{ 38400, 326400, 68, 8 },
	{ 38400, 556800, 58, 4 },
	{ 38400, 652800, 68, 4 },
};

static const struct i915_xd_cdclk_row i915_xd_cdclk_adlp[] = {
	{ 19200, 172800, 27, 3 },
	{ 19200, 192000, 20, 2 },
	{ 19200, 307200, 32, 2 },
	{ 19200, 556800, 58, 2 },
	{ 19200, 652800, 68, 2 },
	{ 24000, 176000, 22, 3 },
	{ 24000, 192000, 16, 2 },
	{ 24000, 312000, 26, 2 },
	{ 24000, 552000, 46, 2 },
	{ 24000, 648000, 54, 2 },
	{ 38400, 179200, 14, 3 },
	{ 38400, 192000, 10, 2 },
	{ 38400, 307200, 16, 2 },
	{ 38400, 556800, 29, 2 },
	{ 38400, 652800, 34, 2 },
};

static const struct i915_xd_cdclk_row i915_xd_cdclk_rplu[] = {
	{ 19200, 172800, 27, 3 },
	{ 19200, 192000, 20, 2 },
	{ 19200, 307200, 32, 2 },
	{ 19200, 480000, 50, 2 },
	{ 19200, 556800, 58, 2 },
	{ 19200, 652800, 68, 2 },
	{ 24000, 176000, 22, 3 },
	{ 24000, 192000, 16, 2 },
	{ 24000, 312000, 26, 2 },
	{ 24000, 480000, 40, 2 },
	{ 24000, 552000, 46, 2 },
	{ 24000, 648000, 54, 2 },
	{ 38400, 179200, 14, 3 },
	{ 38400, 192000, 10, 2 },
	{ 38400, 307200, 16, 2 },
	{ 38400, 480000, 25, 2 },
	{ 38400, 556800, 29, 2 },
	{ 38400, 652800, 34, 2 },
};

/*
 * The highest CD clock (kHz) of each voltage level, lowest level first:
 * display 12 and 13, Raptor Lake-U, and Ice Lake (three levels).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (tgl_calc_voltage_level, rplu_calc_voltage_level, icl_calc_voltage_level).
 */
static const uint32_t i915_xd_voltage_tgl[] = { 312000, 326400, 556800, 652800 };

static const uint32_t i915_xd_voltage_rplu[] = { 312000, 480000, 556800, 652800 };

static const uint32_t i915_xd_voltage_icl[] = { 312000, 556800, 652800 };

/*
 * The display buffer slices each set of active pipes gets: display 12
 * (two slices), display 13 (four slices; a single pipe A or B may have
 * the bus joined to it, and those rows come first) and Ice Lake (two
 * slices, one slice a pipe).  Each row: the pipes (bit per pipe),
 * joined, the slices (bit per slice) of pipes A..D.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (tgl_allowed_dbufs, adlp_allowed_dbufs, icl_allowed_dbufs).
 */
static const struct i915_xd_dbuf_rule i915_xd_dbuf_tgl[] = {
	{ 0x1, 0, { 0x3, 0x0, 0x0, 0x0 } },
	{ 0x2, 0, { 0x0, 0x3, 0x0, 0x0 } },
	{ 0x3, 0, { 0x2, 0x1, 0x0, 0x0 } },
	{ 0x4, 0, { 0x0, 0x0, 0x3, 0x0 } },
	{ 0x5, 0, { 0x1, 0x0, 0x2, 0x0 } },
	{ 0x6, 0, { 0x0, 0x1, 0x2, 0x0 } },
	{ 0x7, 0, { 0x1, 0x1, 0x2, 0x0 } },
	{ 0x8, 0, { 0x0, 0x0, 0x0, 0x3 } },
	{ 0x9, 0, { 0x1, 0x0, 0x0, 0x2 } },
	{ 0xa, 0, { 0x0, 0x1, 0x0, 0x2 } },
	{ 0xb, 0, { 0x1, 0x1, 0x0, 0x2 } },
	{ 0xc, 0, { 0x0, 0x0, 0x1, 0x2 } },
	{ 0xd, 0, { 0x1, 0x0, 0x2, 0x2 } },
	{ 0xe, 0, { 0x0, 0x1, 0x2, 0x2 } },
	{ 0xf, 0, { 0x1, 0x1, 0x2, 0x2 } },
};

static const struct i915_xd_dbuf_rule i915_xd_dbuf_adlp[] = {
	{ 0x1, 1, { 0xf, 0x0, 0x0, 0x0 } },
	{ 0x2, 1, { 0x0, 0xf, 0x0, 0x0 } },
	{ 0x1, 0, { 0x3, 0x0, 0x0, 0x0 } },
	{ 0x2, 0, { 0x0, 0xc, 0x0, 0x0 } },
	{ 0x3, 0, { 0x3, 0xc, 0x0, 0x0 } },
	{ 0x4, 0, { 0x0, 0x0, 0xc, 0x0 } },
	{ 0x5, 0, { 0x3, 0x0, 0xc, 0x0 } },
	{ 0x6, 0, { 0x0, 0xc, 0xc, 0x0 } },
	{ 0x7, 0, { 0x3, 0xc, 0xc, 0x0 } },
	{ 0x8, 0, { 0x0, 0x0, 0x0, 0x3 } },
	{ 0x9, 0, { 0x3, 0x0, 0x0, 0x3 } },
	{ 0xa, 0, { 0x0, 0xc, 0x0, 0x3 } },
	{ 0xb, 0, { 0x3, 0xc, 0x0, 0x3 } },
	{ 0xc, 0, { 0x0, 0x0, 0xc, 0x3 } },
	{ 0xd, 0, { 0x3, 0x0, 0xc, 0x3 } },
	{ 0xe, 0, { 0x0, 0xc, 0xc, 0x3 } },
	{ 0xf, 0, { 0x3, 0xc, 0xc, 0x3 } },
};

static const struct i915_xd_dbuf_rule i915_xd_dbuf_icl[] = {
	{ 0x1, 0, { 0x1, 0x0, 0x0, 0x0 } },
	{ 0x2, 0, { 0x0, 0x1, 0x0, 0x0 } },
	{ 0x3, 0, { 0x1, 0x2, 0x0, 0x0 } },
	{ 0x4, 0, { 0x0, 0x0, 0x2, 0x0 } },
	{ 0x5, 0, { 0x1, 0x0, 0x2, 0x0 } },
	{ 0x6, 0, { 0x0, 0x1, 0x2, 0x0 } },
	{ 0x7, 0, { 0x1, 0x1, 0x2, 0x0 } },
};

/*
 * The memory bus data box credits: display 12, display 13 with its
 * larger A credit when the bus is joined (Wa_22010947358), and Ice Lake
 * (no regulated back-to-back transactions: a maximum of 0).  Each:
 * A credit joined and not, BW credit, B credit, back-to-back maximum and
 * delay.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (intel_mbus_dbox_update).
 */
static const struct i915_xd_dbox_credit i915_xd_dbox_tgl = { 2, 2, 2, 12, 16, 1 };

static const struct i915_xd_dbox_credit i915_xd_dbox_adlp = { 6, 4, 2, 8, 16, 1 };

static const struct i915_xd_dbox_credit i915_xd_dbox_icl = { 2, 2, 1, 8, 0, 0 };

/*
 * The bandwidth buddy page masks by memory type and channel count: the
 * usual table, and the Wa_1409767108 one of Alder Lake-S (and of the
 * first Rocket Lake stepping).  A configuration in neither table turns
 * the buddy off.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power.c
 * (tgl_buddy_page_masks, wa_1409767108_buddy_page_masks).
 */
static const struct i915_xd_buddy_mask i915_xd_buddy_masks[] = {
	{ I915_XD_DRAM_DDR4, 1, 0x0f },
	{ I915_XD_DRAM_DDR5, 1, 0x0f },
	{ I915_XD_DRAM_LPDDR4, 2, 0x1c },
	{ I915_XD_DRAM_LPDDR5, 2, 0x1c },
	{ I915_XD_DRAM_DDR4, 2, 0x1f },
	{ I915_XD_DRAM_DDR5, 2, 0x1e },
	{ I915_XD_DRAM_LPDDR4, 4, 0x38 },
	{ I915_XD_DRAM_LPDDR5, 4, 0x38 },
};

static const struct i915_xd_buddy_mask i915_xd_buddy_masks_wa[] = {
	{ I915_XD_DRAM_LPDDR4, 1, 0x01 },
	{ I915_XD_DRAM_DDR4, 1, 0x01 },
	{ I915_XD_DRAM_DDR5, 1, 0x01 },
	{ I915_XD_DRAM_LPDDR5, 1, 0x01 },
	{ I915_XD_DRAM_LPDDR4, 2, 0x03 },
	{ I915_XD_DRAM_DDR4, 2, 0x03 },
	{ I915_XD_DRAM_DDR5, 2, 0x03 },
	{ I915_XD_DRAM_LPDDR5, 2, 0x03 },
};

/*
 * The compensation reference words of the combo PHYs by the voltage and
 * process COMP_DW3 reports (a value not in the table takes the first
 * row).  Each: the COMP_DW3 voltage and process bits, COMP_DW1 (its
 * bytes 0 and 2), COMP_DW9, COMP_DW10.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c
 * (icl_procmon_values, icl_get_procmon_ref_values), and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy_regs.h
 * (VOLTAGE_INFO_0_85V, VOLTAGE_INFO_0_95V, VOLTAGE_INFO_1_05V,
 * PROCESS_INFO_DOT_0, PROCESS_INFO_DOT_1).
 */
static const struct i915_xd_procmon i915_xd_procmon[] = {
	{ 0x00000000, 0x00000000, 0x62ab67bb, 0x51914f96 },
	{ 0x01000000, 0x00000000, 0x86e172c7, 0x77ca5eab },
	{ 0x05000000, 0x00000000, 0x93f87fe1, 0x8ae871c5 },
	{ 0x02000000, 0x00000000, 0x98fa82dd, 0x89e46dc1 },
	{ 0x06000000, 0x00440000, 0x9a00ab25, 0x8ae38ff1 },
};

/*
 * The display steppings by PCI revision of each platform (Raptor Lake-U
 * has the Raptor Lake-P one).  A revision between two rows takes the
 * next row; one past the last row is a future stepping.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_step.c
 * (icl_revids, tgl_revids, tgl_uy_revids, rkl_revids, adls_revids, adls_rpls_revids,
 * adlp_revids, adlp_n_revids, adlp_rplp_revids, intel_step_init).
 */
static const struct i915_xd_step_row i915_xd_steps_icl[] = {
	{ 0x07, I915_XD_STEP_D0 },
};

static const struct i915_xd_step_row i915_xd_steps_tgl[] = {
	{ 0x00, I915_XD_STEP_B0 },
	{ 0x01, I915_XD_STEP_D0 },
};

static const struct i915_xd_step_row i915_xd_steps_tgl_uy[] = {
	{ 0x00, I915_XD_STEP_A0 },
	{ 0x01, I915_XD_STEP_C0 },
	{ 0x02, I915_XD_STEP_C0 },
	{ 0x03, I915_XD_STEP_D0 },
};

static const struct i915_xd_step_row i915_xd_steps_rkl[] = {
	{ 0x00, I915_XD_STEP_A0 },
	{ 0x01, I915_XD_STEP_B0 },
	{ 0x04, I915_XD_STEP_C0 },
};

static const struct i915_xd_step_row i915_xd_steps_adls[] = {
	{ 0x00, I915_XD_STEP_A0 },
	{ 0x01, I915_XD_STEP_A2 },
	{ 0x04, I915_XD_STEP_B0 },
	{ 0x08, I915_XD_STEP_B0 },
	{ 0x0c, I915_XD_STEP_C0 },
};

static const struct i915_xd_step_row i915_xd_steps_rpls[] = {
	{ 0x04, I915_XD_STEP_D0 },
	{ 0x0c, I915_XD_STEP_C0 },
};

static const struct i915_xd_step_row i915_xd_steps_adlp[] = {
	{ 0x00, I915_XD_STEP_A0 },
	{ 0x04, I915_XD_STEP_B0 },
	{ 0x08, I915_XD_STEP_C0 },
	{ 0x0c, I915_XD_STEP_D0 },
};

static const struct i915_xd_step_row i915_xd_steps_adln[] = {
	{ 0x00, I915_XD_STEP_D0 },
};

static const struct i915_xd_step_row i915_xd_steps_rplp[] = {
	{ 0x04, I915_XD_STEP_E0 },
};

/*
 * The PCH device ids (the bridge id masked with 0xff80) of the south
 * displays beside display 12 and 13 GPUs, of the Comet Lake PCHs a
 * Rocket Lake may sit beside (whose south display the xd code does not
 * drive, so they are known only to be refused), and of the Sunrise Point,
 * Kaby Lake and Cannon Point PCHs beside display 9 GPUs, whose backlight
 * layout the PCH decides.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_pch.h
 * (INTEL_PCH_DEVICE_ID_MASK, INTEL_PCH_SPT_DEVICE_ID_TYPE,
 * INTEL_PCH_SPT_LP_DEVICE_ID_TYPE, INTEL_PCH_KBP_DEVICE_ID_TYPE,
 * INTEL_PCH_CNP_DEVICE_ID_TYPE, INTEL_PCH_CNP_LP_DEVICE_ID_TYPE,
 * INTEL_PCH_CMP_DEVICE_ID_TYPE,
 * INTEL_PCH_CMP2_DEVICE_ID_TYPE, INTEL_PCH_CMP_V_DEVICE_ID_TYPE,
 * INTEL_PCH_ICP_DEVICE_ID_TYPE,
 * INTEL_PCH_ICP2_DEVICE_ID_TYPE, INTEL_PCH_TGP_DEVICE_ID_TYPE,
 * INTEL_PCH_TGP2_DEVICE_ID_TYPE, INTEL_PCH_ADP_DEVICE_ID_TYPE to
 * INTEL_PCH_ADP4_DEVICE_ID_TYPE), and Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_pch.c
 * (intel_pch_type).
 */
#define I915_XD_PCH_ID_MASK		0xff80U

static const struct i915_xd_pch_id i915_xd_pch_ids[] = {
	{ 0xa100, I915_PCH_SPT },
	{ 0x9d00, I915_PCH_SPT },
	{ 0xa280, I915_PCH_SPT },
	{ 0xa300, I915_PCH_CNP },
	{ 0x9d80, I915_PCH_CNP },
	{ 0x0280, I915_PCH_CNP },
	{ 0x0680, I915_PCH_CNP },
	{ 0xa380, I915_PCH_SPT },
	{ 0x3480, I915_PCH_ICP },
	{ 0x3880, I915_PCH_ICP },
	{ 0xa080, I915_PCH_TGP },
	{ 0x4380, I915_PCH_TGP },
	{ 0x7a80, I915_PCH_ADP },
	{ 0x5180, I915_PCH_ADP },
	{ 0x7a00, I915_PCH_ADP },
	{ 0x5480, I915_PCH_ADP },
};

/*
 * The PCI device ids of each platform row.  Ice Lake's row with port F
 * has the port F ids, its other row the rest; Tiger Lake's UY row has
 * the GT2 ids; Raptor Lake-P's row leaves out the Raptor Lake-U ids,
 * which have a row of their own.  The Alder Lake-N row also has the two
 * later ids (0x46d3, 0x46d4) the driver already accepts.
 *
 * Values referenced: Linux v6.8.12 include/drm/i915_pciids.h
 * (INTEL_ICL_PORT_F_IDS, INTEL_ICL_11_IDS, INTEL_TGL_12_GT1_IDS, INTEL_TGL_12_GT2_IDS, INTEL_RKL_IDS,
 * INTEL_ADLS_IDS, INTEL_RPLS_IDS, INTEL_ADLP_IDS, INTEL_ADLN_IDS,
 * INTEL_RPLP_IDS, INTEL_RPLU_IDS), Linux v6.8.12
 * drivers/gpu/drm/i915/intel_device_info.c (subplatform_uy_ids,
 * subplatform_n_ids, subplatform_rpl_ids, subplatform_rplu_ids), and
 * this tree's src/drivers/gpu/i915/intel/pci-ids.h (INTEL_ADLN_IDS),
 * and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_device.c
 * (icl_display, intel_display_device_info_runtime_init: port F).
 */
static const uint16_t i915_xd_ids_icl[] = {
	0x8a50, 0x8a52, 0x8a53, 0x8a54, 0x8a56, 0x8a57, 0x8a58, 0x8a59,
	0x8a5a, 0x8a5b, 0x8a5c, 0x8a70, 0x8a71,
};

static const uint16_t i915_xd_ids_icl_no_f[] = {
	0x8a51, 0x8a5d,
};

static const uint16_t i915_xd_ids_tgl[] = {
	0x9a60, 0x9a68, 0x9a70,
};

static const uint16_t i915_xd_ids_tgl_uy[] = {
	0x9a40, 0x9a49, 0x9a59, 0x9a78, 0x9ac0, 0x9ac9,
	0x9ad9, 0x9af8,
};

static const uint16_t i915_xd_ids_rkl[] = {
	0x4c80, 0x4c8a, 0x4c8b, 0x4c8c, 0x4c90, 0x4c9a,
};

static const uint16_t i915_xd_ids_adls[] = {
	0x4680, 0x4682, 0x4688, 0x468a, 0x468b, 0x4690,
	0x4692, 0x4693,
};

static const uint16_t i915_xd_ids_rpls[] = {
	0xa780, 0xa781, 0xa782, 0xa783, 0xa788, 0xa789,
	0xa78a, 0xa78b,
};

static const uint16_t i915_xd_ids_adlp[] = {
	0x46a0, 0x46a1, 0x46a2, 0x46a3, 0x46a6, 0x46a8,
	0x46aa, 0x462a, 0x4626, 0x4628, 0x46b0, 0x46b1,
	0x46b2, 0x46b3, 0x46c0, 0x46c1, 0x46c2, 0x46c3,
};

static const uint16_t i915_xd_ids_adln[] = {
	0x46d0, 0x46d1, 0x46d2, 0x46d3, 0x46d4,
};

static const uint16_t i915_xd_ids_rplp[] = {
	0xa720, 0xa7a0, 0xa7a8, 0xa7aa, 0xa7ab,
};

static const uint16_t i915_xd_ids_rplu[] = {
	0xa721, 0xa7a1, 0xa7a9, 0xa7ac, 0xa7ad,
};

/*
 * The power wells of Tiger Lake and Alder Lake-S (the TC cold well only
 * on Tiger Lake): PW_1 (switched by the DMC in DC states), the DC state
 * well, PW_2 to PW_5 with what each powers (a well powers what the wells
 * below it in the chain power), the DDI IO wells, the AUX wells and the
 * Thunderbolt AUX wells.  The fuse gate of PW_n is n.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_map.c
 * (icl_power_wells_pw_1, tgl_power_wells_main, tgl_power_wells_tc_cold_off,
 * tgl_power_wells_aux, tgl_pwdoms_pw_2 to tgl_pwdoms_pw_5, tgl_pwdoms_dc_off,
 * tgl_pwdoms_tc_cold_off, icl_pwdoms_aux_a to icl_pwdoms_aux_c), and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_well.c (hsw_power_well_enable).
 */
static const struct i915_xd_well_desc i915_xd_wells_tgl[] = {
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_1,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"DC_off",
		I915_XD_WELL_CONTROL_DC_OFF,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U) |
		I915_XD_POWERS_AUX(0U) | I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) |
		I915_XD_POWERS_DC_OFF
	},
	{
		"PW_2",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_2,
		2U,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U)
	},
	{
		"PW_3",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_3,
		3U,
		I915_XD_WELL_VGA,
		0x2U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U)
	},
	{
		"DDI_IO_A",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U)
	},
	{
		"DDI_IO_B",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_IO_C",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(2U)
	},
	{
		"DDI_IO_TC1",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"DDI_IO_TC2",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"DDI_IO_TC3",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(5U)
	},
	{
		"DDI_IO_TC4",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(6U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(6U)
	},
	{
		"DDI_IO_TC5",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(7U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(7U)
	},
	{
		"DDI_IO_TC6",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(8U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(8U)
	},
	{
		"PW_4",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_4,
		4U,
		0U,
		0x4U,
		1U,
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U)
	},
	{
		"PW_5",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_5,
		5U,
		0U,
		0x8U,
		1U,
		I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U)
	},
	{
		"TC_cold_off",
		I915_XD_WELL_CONTROL_MAILBOX,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U) |
		I915_XD_POWERS_TC_COLD_OFF
	},
	{
		"AUX_A",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(0U) |
		I915_XD_POWERS_AUX(0U)
	},
	{
		"AUX_B",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(1U) |
		I915_XD_POWERS_AUX(1U)
	},
	{
		"AUX_C",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(2U) |
		I915_XD_POWERS_AUX(2U)
	},
	{
		"AUX_USBC1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(3U)
	},
	{
		"AUX_USBC2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(4U)
	},
	{
		"AUX_USBC3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(5U)
	},
	{
		"AUX_USBC4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(6U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(6U)
	},
	{
		"AUX_USBC5",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(7U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(7U)
	},
	{
		"AUX_USBC6",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(8U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(8U)
	},
	{
		"AUX_TBT1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(0U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(0U)
	},
	{
		"AUX_TBT2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(1U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(1U)
	},
	{
		"AUX_TBT3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(2U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(2U)
	},
	{
		"AUX_TBT4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(3U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"AUX_TBT5",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(4U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(4U)
	},
	{
		"AUX_TBT6",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(5U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(5U)
	},
};

static const struct i915_xd_well_desc i915_xd_wells_adls[] = {
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_1,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"DC_off",
		I915_XD_WELL_CONTROL_DC_OFF,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U) |
		I915_XD_POWERS_AUX(0U) | I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) |
		I915_XD_POWERS_DC_OFF
	},
	{
		"PW_2",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_2,
		2U,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U)
	},
	{
		"PW_3",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_3,
		3U,
		I915_XD_WELL_VGA,
		0x2U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) | I915_XD_POWERS_TBT_AUX(4U) | I915_XD_POWERS_TBT_AUX(5U)
	},
	{
		"DDI_IO_A",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U)
	},
	{
		"DDI_IO_B",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_IO_C",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(2U)
	},
	{
		"DDI_IO_TC1",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"DDI_IO_TC2",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"DDI_IO_TC3",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(5U)
	},
	{
		"DDI_IO_TC4",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(6U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(6U)
	},
	{
		"DDI_IO_TC5",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(7U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(7U)
	},
	{
		"DDI_IO_TC6",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(8U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(8U)
	},
	{
		"PW_4",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_4,
		4U,
		0U,
		0x4U,
		1U,
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U)
	},
	{
		"PW_5",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_5,
		5U,
		0U,
		0x8U,
		1U,
		I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U)
	},
	{
		"AUX_A",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(0U) |
		I915_XD_POWERS_AUX(0U)
	},
	{
		"AUX_B",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(1U) |
		I915_XD_POWERS_AUX(1U)
	},
	{
		"AUX_C",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(2U) |
		I915_XD_POWERS_AUX(2U)
	},
	{
		"AUX_USBC1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(3U)
	},
	{
		"AUX_USBC2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(4U)
	},
	{
		"AUX_USBC3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(5U)
	},
	{
		"AUX_USBC4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(6U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(6U)
	},
	{
		"AUX_USBC5",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(7U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(7U)
	},
	{
		"AUX_USBC6",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(8U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(8U)
	},
	{
		"AUX_TBT1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(0U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(0U)
	},
	{
		"AUX_TBT2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(1U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(1U)
	},
	{
		"AUX_TBT3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(2U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(2U)
	},
	{
		"AUX_TBT4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(3U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"AUX_TBT5",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(4U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(4U)
	},
	{
		"AUX_TBT6",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(5U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(5U)
	},
};

/*
 * The power wells of Rocket Lake: PW_1, the DC state well, PW_3 and PW_4
 * (there is no PW_2), the DDI IO wells of A, B, TC1 and TC2, and their AUX
 * wells.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_map.c
 * (icl_power_wells_pw_1, rkl_power_wells_main, rkl_power_wells_ddi_aux,
 * rkl_pwdoms_pw_3, rkl_pwdoms_pw_4, rkl_pwdoms_dc_off).
 */
static const struct i915_xd_well_desc i915_xd_wells_rkl[] = {
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_1,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"DC_off",
		I915_XD_WELL_CONTROL_DC_OFF,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) |
		I915_XD_POWERS_AUX(0U) | I915_XD_POWERS_AUX(1U) |
		I915_XD_POWERS_DC_OFF
	},
	{
		"PW_3",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_3,
		3U,
		I915_XD_WELL_VGA,
		0x2U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U)
	},
	{
		"PW_4",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_4,
		4U,
		0U,
		0x4U,
		1U,
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U)
	},
	{
		"DDI_IO_A",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U)
	},
	{
		"DDI_IO_B",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_IO_TC1",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"DDI_IO_TC2",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"AUX_A",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(0U) |
		I915_XD_POWERS_AUX(0U)
	},
	{
		"AUX_B",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(1U) |
		I915_XD_POWERS_AUX(1U)
	},
	{
		"AUX_USBC1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(3U)
	},
	{
		"AUX_USBC2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX(4U)
	},
};

/*
 * The power wells of display 13: PW_1, the DC state well, PW_2 (the
 * ports and the parent of PW_B, PW_C and PW_D), PW_A (pipe A, beside
 * PW_2 under PW_1), PW_B to PW_D, the DDI IO wells, the AUX wells (with
 * a fixed delay after enabling; the Type-C ones may take 500 ms,
 * Wa_14017248603) and the Thunderbolt AUX wells.  The fuse gate of a
 * well is its index plus one.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_map.c
 * (icl_power_wells_pw_1, xelpd_power_wells_dc_off, xelpd_power_wells_main,
 * xelpd_pwdoms_pw_a to xelpd_pwdoms_pw_d, xelpd_pwdoms_pw_2,
 * xelpd_pwdoms_dc_off, icl_pwdoms_aux_a to icl_pwdoms_aux_e), and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_well.c (hsw_power_well_enable).
 */
static const struct i915_xd_well_desc i915_xd_wells_adlp[] = {
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_1,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"DC_off",
		I915_XD_WELL_CONTROL_DC_OFF,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_LANES(2U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) | I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX_IO(2U) | I915_XD_POWERS_AUX_IO(7U) | I915_XD_POWERS_AUX_IO(8U) |
		I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) |
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX(0U) | I915_XD_POWERS_AUX(1U) |
		I915_XD_POWERS_DC_OFF
	},
	{
		"PW_2",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_2,
		2U,
		I915_XD_WELL_VGA,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U) |
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U) |
		I915_XD_POWERS_LANES(2U) | I915_XD_POWERS_LANES(7U) | I915_XD_POWERS_LANES(8U) | I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) | I915_XD_POWERS_LANES(6U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX_IO(2U) | I915_XD_POWERS_AUX_IO(7U) | I915_XD_POWERS_AUX_IO(8U) |
		I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(7U) | I915_XD_POWERS_AUX(8U) |
		I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) | I915_XD_POWERS_AUX(6U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"PW_A",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_A,
		6U,
		0U,
		0x1U,
		1U,
		I915_XD_POWERS_PIPE(0U)
	},
	{
		"PW_B",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_B,
		7U,
		0U,
		0x2U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U)
	},
	{
		"PW_C",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_C,
		8U,
		0U,
		0x4U,
		1U,
		I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(2U)
	},
	{
		"PW_D",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_D,
		9U,
		0U,
		0x8U,
		1U,
		I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_TRANS(3U)
	},
	{
		"DDI_IO_A",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U)
	},
	{
		"DDI_IO_B",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_IO_C",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(2U)
	},
	{
		"DDI_IO_D",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(7U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(7U)
	},
	{
		"DDI_IO_E",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(8U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(8U)
	},
	{
		"DDI_IO_TC1",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"DDI_IO_TC2",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"DDI_IO_TC3",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(5U)
	},
	{
		"DDI_IO_TC4",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(6U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(6U)
	},
	{
		"AUX_A",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(0U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(0U) |
		I915_XD_POWERS_AUX(0U)
	},
	{
		"AUX_B",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(1U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(1U) |
		I915_XD_POWERS_AUX(1U)
	},
	{
		"AUX_C",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(2U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(2U) |
		I915_XD_POWERS_AUX(2U)
	},
	{
		"AUX_D",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(7U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(7U) |
		I915_XD_POWERS_AUX(7U)
	},
	{
		"AUX_E",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(8U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(8U) |
		I915_XD_POWERS_AUX(8U)
	},
	{
		"AUX_USBC1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(3U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		500U,
		I915_XD_POWERS_AUX(3U)
	},
	{
		"AUX_USBC2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(4U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		500U,
		I915_XD_POWERS_AUX(4U)
	},
	{
		"AUX_USBC3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(5U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		500U,
		I915_XD_POWERS_AUX(5U)
	},
	{
		"AUX_USBC4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(6U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_FIXED_DELAY,
		0U,
		500U,
		I915_XD_POWERS_AUX(6U)
	},
	{
		"AUX_TBT1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(0U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(0U)
	},
	{
		"AUX_TBT2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(1U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(1U)
	},
	{
		"AUX_TBT3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(2U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(2U)
	},
	{
		"AUX_TBT4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT(3U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(3U)
	},
};

/*
 * The power wells of Ice Lake: PW_1 (switched by the DMC in DC states),
 * the DC state well, PW_2 and PW_3 (which hold pipe B, transcoders A to
 * C, the lanes of DDIs B to F, the VGA plane, the audio and the AUX
 * channels B to F), the DDI IO wells A to F, the AUX wells A to F, the
 * Thunderbolt AUX wells of the four Type-C ports, and PW_4 (pipe C).
 * The fuse gate of PW_n is n.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_map.c
 * (icl_power_wells_pw_1, icl_power_wells_main, icl_pwdoms_pw_2 to
 * icl_pwdoms_pw_4, icl_pwdoms_dc_off, glk_pwdoms_ddi_io_a to
 * glk_pwdoms_ddi_io_c, icl_pwdoms_ddi_io_d to icl_pwdoms_ddi_io_f,
 * icl_pwdoms_aux_a to icl_pwdoms_aux_f, icl_pwdoms_aux_tbt1 to
 * icl_pwdoms_aux_tbt4).
 */
static const struct i915_xd_well_desc i915_xd_wells_icl[] = {
	{
		"PW_1",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_1,
		1U,
		I915_XD_WELL_DMC_OWNED,
		0U,
		1U,
		0ULL
	},
	{
		"DC_off",
		I915_XD_WELL_CONTROL_DC_OFF,
		0U,
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(0U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_LANES(1U) | I915_XD_POWERS_LANES(2U) | I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX_IO(1U) | I915_XD_POWERS_AUX_IO(2U) | I915_XD_POWERS_AUX_IO(3U) | I915_XD_POWERS_AUX_IO(4U) | I915_XD_POWERS_AUX_IO(5U) |
		I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U) |
		I915_XD_POWERS_AUX(0U) |
		I915_XD_POWERS_DC_OFF
	},
	{
		"PW_2",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_2,
		2U,
		0U,
		0U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(0U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_LANES(1U) | I915_XD_POWERS_LANES(2U) | I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX_IO(1U) | I915_XD_POWERS_AUX_IO(2U) | I915_XD_POWERS_AUX_IO(3U) | I915_XD_POWERS_AUX_IO(4U) | I915_XD_POWERS_AUX_IO(5U) |
		I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"PW_3",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_3,
		3U,
		I915_XD_WELL_VGA,
		0x2U,
		1U,
		I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_TRANS(0U) | I915_XD_POWERS_TRANS(1U) | I915_XD_POWERS_TRANS(2U) |
		I915_XD_POWERS_LANES(1U) | I915_XD_POWERS_LANES(2U) | I915_XD_POWERS_LANES(3U) | I915_XD_POWERS_LANES(4U) | I915_XD_POWERS_LANES(5U) |
		I915_XD_POWERS_VGA |
		I915_XD_POWERS_AUDIO |
		I915_XD_POWERS_AUX_IO(1U) | I915_XD_POWERS_AUX_IO(2U) | I915_XD_POWERS_AUX_IO(3U) | I915_XD_POWERS_AUX_IO(4U) | I915_XD_POWERS_AUX_IO(5U) |
		I915_XD_POWERS_AUX(1U) | I915_XD_POWERS_AUX(2U) | I915_XD_POWERS_AUX(3U) | I915_XD_POWERS_AUX(4U) | I915_XD_POWERS_AUX(5U) |
		I915_XD_POWERS_TBT_AUX(0U) | I915_XD_POWERS_TBT_AUX(1U) | I915_XD_POWERS_TBT_AUX(2U) | I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"DDI_IO_A",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(0U)
	},
	{
		"DDI_IO_B",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(1U)
	},
	{
		"DDI_IO_C",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(2U)
	},
	{
		"DDI_IO_D",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(3U)
	},
	{
		"DDI_IO_E",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(4U)
	},
	{
		"DDI_IO_F",
		I915_XD_WELL_CONTROL_DDI,
		I915_XD_WELL_DDI(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_DDI_IO(5U)
	},
	{
		"AUX_A",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(0U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(0U) |
		I915_XD_POWERS_AUX(0U)
	},
	{
		"AUX_B",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(1U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(1U) |
		I915_XD_POWERS_AUX(1U)
	},
	{
		"AUX_C",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(2U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(2U) |
		I915_XD_POWERS_AUX(2U)
	},
	{
		"AUX_D",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(3U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(3U) |
		I915_XD_POWERS_AUX(3U)
	},
	{
		"AUX_E",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(4U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(4U) |
		I915_XD_POWERS_AUX(4U)
	},
	{
		"AUX_F",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX(5U),
		I915_XD_WELL_NO_FUSE,
		0U,
		0U,
		1U,
		I915_XD_POWERS_AUX_IO(5U) |
		I915_XD_POWERS_AUX(5U)
	},
	{
		"AUX_TBT1",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT_ICL(0U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(0U)
	},
	{
		"AUX_TBT2",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT_ICL(1U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(1U)
	},
	{
		"AUX_TBT3",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT_ICL(2U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(2U)
	},
	{
		"AUX_TBT4",
		I915_XD_WELL_CONTROL_AUX,
		I915_XD_WELL_AUX_TBT_ICL(3U),
		I915_XD_WELL_NO_FUSE,
		I915_XD_WELL_TBT_AUX,
		0U,
		1U,
		I915_XD_POWERS_TBT_AUX(3U)
	},
	{
		"PW_4",
		I915_XD_WELL_CONTROL_MAIN,
		I915_XD_WELL_PW_4,
		4U,
		0U,
		0x4U,
		1U,
		I915_XD_POWERS_PIPE(2U)
	}
};

/*
 * The ports of each platform: Tiger Lake's A and B on combo PHYs and
 * TC1..TC6 on Dekel PHYs D..I (its DDI C is not used); Rocket Lake's A,
 * B, TC1 and TC2 all on combo PHYs A..D; Alder Lake-S's A and TC1..TC4
 * on combo PHYs A..E; display 13's A and B on combo PHYs and TC1..TC4 on
 * Dekel PHYs F..I; Ice Lake's A and B on combo PHYs and C..F (TC1..TC4,
 * F only on some parts) on MG PHYs C..F.  A port's default AUX channel
 * is its DDI, and its hotplug pin follows its DDI (A, B on the DDI pins,
 * the others on the Type-C pins, which Rocket Lake with a TGP south
 * display uses too).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_device.c
 * (icl_display, tgl_display, rkl_display, adl_s_display, xe_lpd_display: port_mask,
 * intel_display_device_info_runtime_init: port F),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c (intel_port_to_phy,
 * intel_phy_is_combo, intel_phy_is_tc, intel_port_to_tc),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c (icl_hpd_pin, tgl_hpd_pin, rkl_hpd_pin,
 * xelpd_hpd_pin), and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp_aux.c (default_aux_ch).
 */
static const struct i915_xd_port_desc i915_xd_ports_icl[] = {
	{ 0, I915_XD_PHY_COMBO, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 1, I915_XD_PHY_COMBO, 1, 0, 1, 0, I915_XD_SOUTH_PIN_DDI, 1 },
	{ 2, I915_XD_PHY_MG, 2, 1, 2, 1, I915_XD_SOUTH_PIN_TC, 0 },
	{ 3, I915_XD_PHY_MG, 3, 2, 3, 2, I915_XD_SOUTH_PIN_TC, 1 },
	{ 4, I915_XD_PHY_MG, 4, 3, 4, 3, I915_XD_SOUTH_PIN_TC, 2 },
	{ 5, I915_XD_PHY_MG, 5, 4, 5, 4, I915_XD_SOUTH_PIN_TC, 3 },
};

static const struct i915_xd_port_desc i915_xd_ports_tgl[] = {
	{ 0, I915_XD_PHY_COMBO, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 1, I915_XD_PHY_COMBO, 1, 0, 1, 0, I915_XD_SOUTH_PIN_DDI, 1 },
	{ 3, I915_XD_PHY_DEKEL, 3, 1, 3, 1, I915_XD_SOUTH_PIN_TC, 0 },
	{ 4, I915_XD_PHY_DEKEL, 4, 2, 4, 2, I915_XD_SOUTH_PIN_TC, 1 },
	{ 5, I915_XD_PHY_DEKEL, 5, 3, 5, 3, I915_XD_SOUTH_PIN_TC, 2 },
	{ 6, I915_XD_PHY_DEKEL, 6, 4, 6, 4, I915_XD_SOUTH_PIN_TC, 3 },
	{ 7, I915_XD_PHY_DEKEL, 7, 5, 7, 5, I915_XD_SOUTH_PIN_TC, 4 },
	{ 8, I915_XD_PHY_DEKEL, 8, 6, 8, 6, I915_XD_SOUTH_PIN_TC, 5 },
};

static const struct i915_xd_port_desc i915_xd_ports_rkl[] = {
	{ 0, I915_XD_PHY_COMBO, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 1, I915_XD_PHY_COMBO, 1, 0, 1, 0, I915_XD_SOUTH_PIN_DDI, 1 },
	{ 3, I915_XD_PHY_COMBO, 2, 0, 3, 0, I915_XD_SOUTH_PIN_TC, 0 },
	{ 4, I915_XD_PHY_COMBO, 3, 0, 4, 0, I915_XD_SOUTH_PIN_TC, 1 },
};

static const struct i915_xd_port_desc i915_xd_ports_adls[] = {
	{ 0, I915_XD_PHY_COMBO, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 3, I915_XD_PHY_COMBO, 1, 0, 3, 0, I915_XD_SOUTH_PIN_TC, 0 },
	{ 4, I915_XD_PHY_COMBO, 2, 0, 4, 0, I915_XD_SOUTH_PIN_TC, 1 },
	{ 5, I915_XD_PHY_COMBO, 3, 0, 5, 0, I915_XD_SOUTH_PIN_TC, 2 },
	{ 6, I915_XD_PHY_COMBO, 4, 0, 6, 0, I915_XD_SOUTH_PIN_TC, 3 },
};

static const struct i915_xd_port_desc i915_xd_ports_adlp[] = {
	{ 0, I915_XD_PHY_COMBO, 0, 0, 0, 0, I915_XD_SOUTH_PIN_DDI, 0 },
	{ 1, I915_XD_PHY_COMBO, 1, 0, 1, 0, I915_XD_SOUTH_PIN_DDI, 1 },
	{ 3, I915_XD_PHY_DEKEL, 5, 1, 3, 1, I915_XD_SOUTH_PIN_TC, 0 },
	{ 4, I915_XD_PHY_DEKEL, 6, 2, 4, 2, I915_XD_SOUTH_PIN_TC, 1 },
	{ 5, I915_XD_PHY_DEKEL, 7, 3, 5, 3, I915_XD_SOUTH_PIN_TC, 2 },
	{ 6, I915_XD_PHY_DEKEL, 8, 4, 6, 4, I915_XD_SOUTH_PIN_TC, 3 },
};

/*
 * The PLLs of each platform: the combo DPLLs (Rocket Lake's third one
 * and Alder Lake-S's DPLL2 select as 2 and use the words at 0x164294;
 * Alder Lake-S's DPLL3 has its own), the Thunderbolt PLL and the Type-C
 * PLLs (display 13 moved their enables; Ice Lake's are the MG PLLs, whose
 * words are in the MG PHY; Ice Lake's DPLLs and Thunderbolt PLL have
 * their words at its own places).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c
 * (icl_plls, tgl_plls, rkl_plls, adls_plls, adlp_plls, intel_combo_pll_enable_reg,
 * intel_tc_pll_enable_reg, icl_dpll_write), and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.h (DPLL_ID_ICL_DPLL0,
 * DPLL_ID_ICL_DPLL1, DPLL_ID_EHL_DPLL4, DPLL_ID_ICL_TBTPLL,
 * DPLL_ID_ICL_MGPLL1, DPLL_ID_DG1_DPLL2, DPLL_ID_DG1_DPLL3).
 */
static const struct i915_xd_pll_desc i915_xd_plls_icl[] = {
	{ I915_XD_PLL_COMBO, 0, 0, I915_XD_DPLL0_ENABLE, I915_XD_DPLL_CFGCR0_ICL(0U), I915_XD_DPLL_CFGCR1_ICL(0U) },
	{ I915_XD_PLL_COMBO, 1, 0, I915_XD_DPLL1_ENABLE, I915_XD_DPLL_CFGCR0_ICL(1U), I915_XD_DPLL_CFGCR1_ICL(1U) },
	{ I915_XD_PLL_TBT, 2, 0, I915_XD_TBT_PLL_ENABLE, I915_XD_DPLL_CFGCR0_ICL(2U), I915_XD_DPLL_CFGCR1_ICL(2U) },
	{ I915_XD_PLL_TC, 3, 1, I915_XD_TC_PLL_ENABLE(0U), 0U, 0U },
	{ I915_XD_PLL_TC, 4, 2, I915_XD_TC_PLL_ENABLE(1U), 0U, 0U },
	{ I915_XD_PLL_TC, 5, 3, I915_XD_TC_PLL_ENABLE(2U), 0U, 0U },
	{ I915_XD_PLL_TC, 6, 4, I915_XD_TC_PLL_ENABLE(3U), 0U, 0U },
};

static const struct i915_xd_pll_desc i915_xd_plls_tgl[] = {
	{ I915_XD_PLL_COMBO, 0, 0, I915_XD_DPLL0_ENABLE, I915_XD_DPLL_CFGCR0(0U), I915_XD_DPLL_CFGCR1(0U) },
	{ I915_XD_PLL_COMBO, 1, 0, I915_XD_DPLL1_ENABLE, I915_XD_DPLL_CFGCR0(1U), I915_XD_DPLL_CFGCR1(1U) },
	{ I915_XD_PLL_TBT, 2, 0, I915_XD_TBT_PLL_ENABLE, I915_XD_TBT_PLL_CFGCR0, I915_XD_TBT_PLL_CFGCR1 },
	{ I915_XD_PLL_TC, 3, 1, I915_XD_TC_PLL_ENABLE(0U), 0U, 0U },
	{ I915_XD_PLL_TC, 4, 2, I915_XD_TC_PLL_ENABLE(1U), 0U, 0U },
	{ I915_XD_PLL_TC, 5, 3, I915_XD_TC_PLL_ENABLE(2U), 0U, 0U },
	{ I915_XD_PLL_TC, 6, 4, I915_XD_TC_PLL_ENABLE(3U), 0U, 0U },
	{ I915_XD_PLL_TC, 7, 5, I915_XD_TC_PLL_ENABLE(4U), 0U, 0U },
	{ I915_XD_PLL_TC, 8, 6, I915_XD_TC_PLL_ENABLE(5U), 0U, 0U },
};

static const struct i915_xd_pll_desc i915_xd_plls_rkl[] = {
	{ I915_XD_PLL_COMBO, 0, 0, I915_XD_DPLL0_ENABLE, I915_XD_DPLL_CFGCR0(0U), I915_XD_DPLL_CFGCR1(0U) },
	{ I915_XD_PLL_COMBO, 1, 0, I915_XD_DPLL1_ENABLE, I915_XD_DPLL_CFGCR0(1U), I915_XD_DPLL_CFGCR1(1U) },
	{ I915_XD_PLL_COMBO, 2, 0, I915_XD_DPLL2_ENABLE_ADLS, I915_XD_DPLL2_CFGCR0_ADLS, I915_XD_DPLL2_CFGCR1_ADLS },
};

static const struct i915_xd_pll_desc i915_xd_plls_adls[] = {
	{ I915_XD_PLL_COMBO, 0, 0, I915_XD_DPLL0_ENABLE, I915_XD_DPLL_CFGCR0(0U), I915_XD_DPLL_CFGCR1(0U) },
	{ I915_XD_PLL_COMBO, 1, 0, I915_XD_DPLL1_ENABLE, I915_XD_DPLL_CFGCR0(1U), I915_XD_DPLL_CFGCR1(1U) },
	{ I915_XD_PLL_COMBO, 2, 0, I915_XD_DPLL2_ENABLE_ADLS, I915_XD_DPLL2_CFGCR0_ADLS, I915_XD_DPLL2_CFGCR1_ADLS },
	{ I915_XD_PLL_COMBO, 3, 0, I915_XD_DPLL3_ENABLE_ADLS, I915_XD_DPLL3_CFGCR0_ADLS, I915_XD_DPLL3_CFGCR1_ADLS },
};

static const struct i915_xd_pll_desc i915_xd_plls_adlp[] = {
	{ I915_XD_PLL_COMBO, 0, 0, I915_XD_DPLL0_ENABLE, I915_XD_DPLL_CFGCR0(0U), I915_XD_DPLL_CFGCR1(0U) },
	{ I915_XD_PLL_COMBO, 1, 0, I915_XD_DPLL1_ENABLE, I915_XD_DPLL_CFGCR0(1U), I915_XD_DPLL_CFGCR1(1U) },
	{ I915_XD_PLL_TBT, 2, 0, I915_XD_TBT_PLL_ENABLE, I915_XD_TBT_PLL_CFGCR0, I915_XD_TBT_PLL_CFGCR1 },
	{ I915_XD_PLL_TC, 3, 1, I915_XD_TC_PLL_ENABLE_ADLP(0U), 0U, 0U },
	{ I915_XD_PLL_TC, 4, 2, I915_XD_TC_PLL_ENABLE_ADLP(1U), 0U, 0U },
	{ I915_XD_PLL_TC, 5, 3, I915_XD_TC_PLL_ENABLE_ADLP(2U), 0U, 0U },
	{ I915_XD_PLL_TC, 6, 4, I915_XD_TC_PLL_ENABLE_ADLP(3U), 0U, 0U },
};

/*
 * How each platform's VBT names its ports: the DVO port codes of each
 * DDI.  Tiger Lake numbers by DDI (A..I, the CRT code with DDI E);
 * Rocket Lake by combo PHY (C and D are TC1 and TC2); Alder Lake-S by
 * combo PHY (B..E are TC1..TC4); display 13 keeps D and E for DDIs it
 * does not have and names TC1..TC4 F..I.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (dvo_port_to_port: port_mapping, rkl_port_mapping, adls_port_mapping,
 * xelpd_port_mapping) and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_limits.h (enum port: PORT_TC1,
 * PORT_D_XELPD, PORT_E_XELPD).
 */
static const struct i915_xd_vbt_dvo_row i915_xd_vbt_dvo_tgl[] = {
	{ 0U, { I915_XD_DVO_HDMI_A, I915_XD_DVO_DP_A, I915_XD_DVO_NONE } },
	{ 1U, { I915_XD_DVO_HDMI_B, I915_XD_DVO_DP_B, I915_XD_DVO_NONE } },
	{ 2U, { I915_XD_DVO_HDMI_C, I915_XD_DVO_DP_C, I915_XD_DVO_NONE } },
	{ 3U, { I915_XD_DVO_HDMI_D, I915_XD_DVO_DP_D, I915_XD_DVO_NONE } },
	{ 4U, { I915_XD_DVO_HDMI_E, I915_XD_DVO_DP_E, I915_XD_DVO_CRT } },
	{ 5U, { I915_XD_DVO_HDMI_F, I915_XD_DVO_DP_F, I915_XD_DVO_NONE } },
	{ 6U, { I915_XD_DVO_HDMI_G, I915_XD_DVO_DP_G, I915_XD_DVO_NONE } },
	{ 7U, { I915_XD_DVO_HDMI_H, I915_XD_DVO_DP_H, I915_XD_DVO_NONE } },
	{ 8U, { I915_XD_DVO_HDMI_I, I915_XD_DVO_DP_I, I915_XD_DVO_NONE } },
};

static const struct i915_xd_vbt_dvo_row i915_xd_vbt_dvo_rkl[] = {
	{ 0U, { I915_XD_DVO_HDMI_A, I915_XD_DVO_DP_A, I915_XD_DVO_NONE } },
	{ 1U, { I915_XD_DVO_HDMI_B, I915_XD_DVO_DP_B, I915_XD_DVO_NONE } },
	{ 3U, { I915_XD_DVO_HDMI_C, I915_XD_DVO_DP_C, I915_XD_DVO_NONE } },
	{ 4U, { I915_XD_DVO_HDMI_D, I915_XD_DVO_DP_D, I915_XD_DVO_NONE } },
};

static const struct i915_xd_vbt_dvo_row i915_xd_vbt_dvo_adls[] = {
	{ 0U, { I915_XD_DVO_HDMI_A, I915_XD_DVO_DP_A, I915_XD_DVO_NONE } },
	{ 3U, { I915_XD_DVO_HDMI_B, I915_XD_DVO_DP_B, I915_XD_DVO_NONE } },
	{ 4U, { I915_XD_DVO_HDMI_C, I915_XD_DVO_DP_C, I915_XD_DVO_NONE } },
	{ 5U, { I915_XD_DVO_HDMI_D, I915_XD_DVO_DP_D, I915_XD_DVO_NONE } },
	{ 6U, { I915_XD_DVO_HDMI_E, I915_XD_DVO_DP_E, I915_XD_DVO_NONE } },
};

static const struct i915_xd_vbt_dvo_row i915_xd_vbt_dvo_adlp[] = {
	{ 0U, { I915_XD_DVO_HDMI_A, I915_XD_DVO_DP_A, I915_XD_DVO_NONE } },
	{ 1U, { I915_XD_DVO_HDMI_B, I915_XD_DVO_DP_B, I915_XD_DVO_NONE } },
	{ 2U, { I915_XD_DVO_HDMI_C, I915_XD_DVO_DP_C, I915_XD_DVO_NONE } },
	{ 7U, { I915_XD_DVO_HDMI_D, I915_XD_DVO_DP_D, I915_XD_DVO_NONE } },
	{ 8U, { I915_XD_DVO_HDMI_E, I915_XD_DVO_DP_E, I915_XD_DVO_NONE } },
	{ 3U, { I915_XD_DVO_HDMI_F, I915_XD_DVO_DP_F, I915_XD_DVO_NONE } },
	{ 4U, { I915_XD_DVO_HDMI_G, I915_XD_DVO_DP_G, I915_XD_DVO_NONE } },
	{ 5U, { I915_XD_DVO_HDMI_H, I915_XD_DVO_DP_H, I915_XD_DVO_NONE } },
	{ 6U, { I915_XD_DVO_HDMI_I, I915_XD_DVO_DP_I, I915_XD_DVO_NONE } },
};

/*
 * The VBT DDC pin code of each GMBUS pin (the index), 0 where the pin
 * has none: the ICP-class table of Tiger Lake (pins 1..3 for DDI A..C,
 * 9..14 for TC1..TC6), Rocket Lake beside a TGP, Alder Lake-S, and
 * display 13.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (icp_ddc_pin_map, rkl_pch_tgp_ddc_pin_map, adls_ddc_pin_map,
 * adlp_ddc_pin_map), Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (enum vbt_gmbus_ddi) and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.h (GMBUS_PIN_*).
 */
static const uint8_t i915_xd_vbt_ddc_tgl[] = { 0U, 1U, 2U, 3U, 0U, 0U, 0U, 0U, 0U, 4U, 5U, 6U, 7U, 8U, 9U };
static const uint8_t i915_xd_vbt_ddc_rkl[] = { 0U, 1U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 3U, 4U };
static const uint8_t i915_xd_vbt_ddc_adls[] = { 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 3U, 4U, 5U };
static const uint8_t i915_xd_vbt_ddc_adlp[] = { 0U, 1U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 3U, 4U, 5U, 6U };

/*
 * The VBT AUX channel code of each AUX channel (the index: A, B, C, then
 * USBC1..USBC6, which display 13 also numbers its D and E by), 0 where
 * the channel has none.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (direct_aux_ch_map, rkl_aux_ch_map, adls_aux_ch_map, adlp_aux_ch_map)
 * and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.h
 * (enum aux_ch).
 */
static const uint8_t i915_xd_vbt_aux_tgl[] = {
	I915_XD_VBT_AUX_A, I915_XD_VBT_AUX_B, I915_XD_VBT_AUX_C, I915_XD_VBT_AUX_D, I915_XD_VBT_AUX_E,
	I915_XD_VBT_AUX_F, I915_XD_VBT_AUX_G, I915_XD_VBT_AUX_H, I915_XD_VBT_AUX_I
};
static const uint8_t i915_xd_vbt_aux_rkl[] = {
	I915_XD_VBT_AUX_A, I915_XD_VBT_AUX_B, 0U, I915_XD_VBT_AUX_C, I915_XD_VBT_AUX_D
};
static const uint8_t i915_xd_vbt_aux_adls[] = {
	I915_XD_VBT_AUX_A, 0U, 0U, I915_XD_VBT_AUX_B, I915_XD_VBT_AUX_C, I915_XD_VBT_AUX_D, I915_XD_VBT_AUX_E
};
static const uint8_t i915_xd_vbt_aux_adlp[] = {
	I915_XD_VBT_AUX_A, I915_XD_VBT_AUX_B, I915_XD_VBT_AUX_C, I915_XD_VBT_AUX_F, I915_XD_VBT_AUX_G,
	I915_XD_VBT_AUX_H, I915_XD_VBT_AUX_I, I915_XD_VBT_AUX_D, I915_XD_VBT_AUX_E
};

/*
 * The VBT numbering of each platform, made of the tables above.
 */
static const struct i915_xd_vbt_maps i915_xd_vbt_maps_tgl = {
	i915_xd_vbt_dvo_tgl, sizeof(i915_xd_vbt_dvo_tgl) / sizeof(i915_xd_vbt_dvo_tgl[0]),
	i915_xd_vbt_ddc_tgl, sizeof(i915_xd_vbt_ddc_tgl) / sizeof(i915_xd_vbt_ddc_tgl[0]),
	i915_xd_vbt_aux_tgl, sizeof(i915_xd_vbt_aux_tgl) / sizeof(i915_xd_vbt_aux_tgl[0])
};

static const struct i915_xd_vbt_maps i915_xd_vbt_maps_rkl = {
	i915_xd_vbt_dvo_rkl, sizeof(i915_xd_vbt_dvo_rkl) / sizeof(i915_xd_vbt_dvo_rkl[0]),
	i915_xd_vbt_ddc_rkl, sizeof(i915_xd_vbt_ddc_rkl) / sizeof(i915_xd_vbt_ddc_rkl[0]),
	i915_xd_vbt_aux_rkl, sizeof(i915_xd_vbt_aux_rkl) / sizeof(i915_xd_vbt_aux_rkl[0])
};

static const struct i915_xd_vbt_maps i915_xd_vbt_maps_adls = {
	i915_xd_vbt_dvo_adls, sizeof(i915_xd_vbt_dvo_adls) / sizeof(i915_xd_vbt_dvo_adls[0]),
	i915_xd_vbt_ddc_adls, sizeof(i915_xd_vbt_ddc_adls) / sizeof(i915_xd_vbt_ddc_adls[0]),
	i915_xd_vbt_aux_adls, sizeof(i915_xd_vbt_aux_adls) / sizeof(i915_xd_vbt_aux_adls[0])
};

static const struct i915_xd_vbt_maps i915_xd_vbt_maps_adlp = {
	i915_xd_vbt_dvo_adlp, sizeof(i915_xd_vbt_dvo_adlp) / sizeof(i915_xd_vbt_dvo_adlp[0]),
	i915_xd_vbt_ddc_adlp, sizeof(i915_xd_vbt_ddc_adlp) / sizeof(i915_xd_vbt_ddc_adlp[0]),
	i915_xd_vbt_aux_adlp, sizeof(i915_xd_vbt_aux_adlp) / sizeof(i915_xd_vbt_aux_adlp[0])
};

/*
 * The platform descriptions: Tiger Lake (and its UY parts), Rocket Lake
 * beside a TGP south display, Alder Lake-S and Raptor Lake-S, Alder
 * Lake-P and its N and Raptor Lake-P and -U parts, and Ice Lake (with
 * and without port F), last so that the first row stays Tiger Lake's.  Only Tiger Lake and the display 13 rows were checked on a
 * machine (verified); the others are built from the tables alone.  Ice
 * Lake's ports C..F are Type-C ports on MG PHYs with one FIA; a legacy
 * one leaves TC cold through its AUX well and the firmware.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_device.c
 * (ICL_DISPLAY, XE_D_DISPLAY, XE_LPD_FEATURES, icl_display, tgl_display,
 * rkl_display, adl_s_display, xe_lpd_display: pipe_mask, abox_mask, dbuf,
 * has_hti, has_cdclk_crawl), Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_pch.c
 * (intel_pch_type: the ICP south display of Ice Lake),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (icl_combo_phy_aux_power_well_enable: the AUX lane and display WA #1178),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power.c (icl_mbus_init, gen12_dbuf_slices_config, tgl_bw_buddy_init),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dpll_mgr.c (ehl_combo_pll_div_frac_wa_needed),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_combo_phy.c (phy_is_master, has_phy_misc),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c (intel_init_cdclk_hooks),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c (skl_wm_init, adjust_wm_latency),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dmc.c (ICL_DMC_PATH, TGL_DMC_PATH, RKL_DMC_PATH,
 * ADLS_DMC_PATH, ADLP_DMC_PATH, disable_dmc_evt, pipedmc_clock_gating_wa),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dmc_regs.h (_TGL_PIPEDMC_REG_MMIO_BASE_A,
 * _ADLP_PIPEDMC_REG_MMIO_BASE_A), Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_wa.c
 * (xe_d_display_wa_apply, adlp_display_wa_apply), Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (tgl_dkl_phy_set_signal_levels: the load generator of Alder Lake-P),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c (intel_crtc_compute_min_cdclk: the
 * Tiger Lake minimum of the pixel rate),
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_power_well.c (hsw_power_well_enable), and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_tc.c (the TC cold handling of Ice Lake,
 * Tiger Lake and display 13, icl_tc_phy_init: Ice Lake's single FIA).
 */
static const struct i915_xd_platform i915_xd_platforms[] = {
	{
		"Tiger Lake",
		I915_XD_KIND_TGL,
		12U,
		I915_PCH_TGP,
		0xfU,
		1,
		i915_xd_ids_tgl,
		sizeof(i915_xd_ids_tgl) / sizeof(i915_xd_ids_tgl[0]),
		i915_xd_steps_tgl,
		sizeof(i915_xd_steps_tgl) / sizeof(i915_xd_steps_tgl[0]),
		i915_xd_ports_tgl,
		sizeof(i915_xd_ports_tgl) / sizeof(i915_xd_ports_tgl[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_FIA,
		I915_XD_TCCOLD_MAILBOX_WELL,
		i915_xd_wells_tgl,
		sizeof(i915_xd_wells_tgl) / sizeof(i915_xd_wells_tgl[0]),
		i915_xd_plls_tgl,
		sizeof(i915_xd_plls_tgl) / sizeof(i915_xd_plls_tgl[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_tgl,
		sizeof(i915_xd_dbuf_tgl) / sizeof(i915_xd_dbuf_tgl[0]),
		&i915_xd_dbox_tgl,
		0x7U,
		0x6U,
		1,
		8U,
		3U,
		0,
		"i915/tgl_dmc_ver2_12.bin",
		I915_XD_DMC_PIPE_BASE_TGL,
		&i915_xd_swing_tgl,
		I915_XD_WA_DMC_CLK_MSEC_VBLANK | I915_XD_WA_XE_D_CHICKENS | I915_XD_WA_CDCLK_PIXEL_RATE,
		&i915_xd_vbt_maps_tgl,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Tiger Lake UY",
		I915_XD_KIND_TGL,
		12U,
		I915_PCH_TGP,
		0xfU,
		1,
		i915_xd_ids_tgl_uy,
		sizeof(i915_xd_ids_tgl_uy) / sizeof(i915_xd_ids_tgl_uy[0]),
		i915_xd_steps_tgl_uy,
		sizeof(i915_xd_steps_tgl_uy) / sizeof(i915_xd_steps_tgl_uy[0]),
		i915_xd_ports_tgl,
		sizeof(i915_xd_ports_tgl) / sizeof(i915_xd_ports_tgl[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_FIA,
		I915_XD_TCCOLD_MAILBOX_WELL,
		i915_xd_wells_tgl,
		sizeof(i915_xd_wells_tgl) / sizeof(i915_xd_wells_tgl[0]),
		i915_xd_plls_tgl,
		sizeof(i915_xd_plls_tgl) / sizeof(i915_xd_plls_tgl[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_tgl,
		sizeof(i915_xd_dbuf_tgl) / sizeof(i915_xd_dbuf_tgl[0]),
		&i915_xd_dbox_tgl,
		0x7U,
		0x6U,
		1,
		8U,
		3U,
		0,
		"i915/tgl_dmc_ver2_12.bin",
		I915_XD_DMC_PIPE_BASE_TGL,
		&i915_xd_swing_tgl_uy,
		I915_XD_WA_DMC_CLK_MSEC_VBLANK | I915_XD_WA_XE_D_CHICKENS | I915_XD_WA_CDCLK_PIXEL_RATE,
		&i915_xd_vbt_maps_tgl,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Rocket Lake",
		I915_XD_KIND_RKL,
		12U,
		I915_PCH_TGP,
		0x7U,
		0,
		i915_xd_ids_rkl,
		sizeof(i915_xd_ids_rkl) / sizeof(i915_xd_ids_rkl[0]),
		i915_xd_steps_rkl,
		sizeof(i915_xd_steps_rkl) / sizeof(i915_xd_steps_rkl[0]),
		i915_xd_ports_rkl,
		sizeof(i915_xd_ports_rkl) / sizeof(i915_xd_ports_rkl[0]),
		0xfU,
		0x5U,
		0x3U,
		1U,
		I915_XD_TC_FLOW_NONE,
		I915_XD_TCCOLD_NONE,
		i915_xd_wells_rkl,
		sizeof(i915_xd_wells_rkl) / sizeof(i915_xd_wells_rkl[0]),
		i915_xd_plls_rkl,
		sizeof(i915_xd_plls_rkl) / sizeof(i915_xd_plls_rkl[0]),
		I915_XD_PLL_MAP_CFGCR0_RKL,
		0,
		i915_xd_cdclk_rkl,
		sizeof(i915_xd_cdclk_rkl) / sizeof(i915_xd_cdclk_rkl[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_tgl,
		sizeof(i915_xd_dbuf_tgl) / sizeof(i915_xd_dbuf_tgl[0]),
		&i915_xd_dbox_tgl,
		0x1U,
		0x1U,
		1,
		8U,
		3U,
		0,
		"i915/rkl_dmc_ver2_03.bin",
		I915_XD_DMC_PIPE_BASE_TGL,
		&i915_xd_swing_rkl,
		I915_XD_WA_BUDDY_1409767108_A0 | I915_XD_WA_XE_D_CHICKENS,
		&i915_xd_vbt_maps_rkl,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Alder Lake-S",
		I915_XD_KIND_ADLS,
		12U,
		I915_PCH_ADP,
		0xfU,
		0,
		i915_xd_ids_adls,
		sizeof(i915_xd_ids_adls) / sizeof(i915_xd_ids_adls[0]),
		i915_xd_steps_adls,
		sizeof(i915_xd_steps_adls) / sizeof(i915_xd_steps_adls[0]),
		i915_xd_ports_adls,
		sizeof(i915_xd_ports_adls) / sizeof(i915_xd_ports_adls[0]),
		0x1fU,
		0x9U,
		0x1U,
		1U,
		I915_XD_TC_FLOW_NONE,
		I915_XD_TCCOLD_NONE,
		i915_xd_wells_adls,
		sizeof(i915_xd_wells_adls) / sizeof(i915_xd_wells_adls[0]),
		i915_xd_plls_adls,
		sizeof(i915_xd_plls_adls) / sizeof(i915_xd_plls_adls[0]),
		I915_XD_PLL_MAP_CFGCR_ADLS,
		1,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_tgl,
		sizeof(i915_xd_dbuf_tgl) / sizeof(i915_xd_dbuf_tgl[0]),
		&i915_xd_dbox_tgl,
		0x7U,
		0x6U,
		1,
		8U,
		3U,
		0,
		"i915/adls_dmc_ver2_01.bin",
		I915_XD_DMC_PIPE_BASE_TGL,
		&i915_xd_swing_adls,
		I915_XD_WA_BUDDY_1409767108 | I915_XD_WA_DMC_VBLANK_A | I915_XD_WA_XE_D_CHICKENS,
		&i915_xd_vbt_maps_adls,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Raptor Lake-S",
		I915_XD_KIND_ADLS,
		12U,
		I915_PCH_ADP,
		0xfU,
		0,
		i915_xd_ids_rpls,
		sizeof(i915_xd_ids_rpls) / sizeof(i915_xd_ids_rpls[0]),
		i915_xd_steps_rpls,
		sizeof(i915_xd_steps_rpls) / sizeof(i915_xd_steps_rpls[0]),
		i915_xd_ports_adls,
		sizeof(i915_xd_ports_adls) / sizeof(i915_xd_ports_adls[0]),
		0x1fU,
		0x9U,
		0x1U,
		1U,
		I915_XD_TC_FLOW_NONE,
		I915_XD_TCCOLD_NONE,
		i915_xd_wells_adls,
		sizeof(i915_xd_wells_adls) / sizeof(i915_xd_wells_adls[0]),
		i915_xd_plls_adls,
		sizeof(i915_xd_plls_adls) / sizeof(i915_xd_plls_adls[0]),
		I915_XD_PLL_MAP_CFGCR_ADLS,
		1,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_tgl,
		sizeof(i915_xd_dbuf_tgl) / sizeof(i915_xd_dbuf_tgl[0]),
		&i915_xd_dbox_tgl,
		0x7U,
		0x6U,
		1,
		8U,
		3U,
		0,
		"i915/adls_dmc_ver2_01.bin",
		I915_XD_DMC_PIPE_BASE_TGL,
		&i915_xd_swing_adls,
		I915_XD_WA_BUDDY_1409767108 | I915_XD_WA_DMC_VBLANK_A | I915_XD_WA_XE_D_CHICKENS,
		&i915_xd_vbt_maps_adls,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Alder Lake-P",
		I915_XD_KIND_ADLP,
		13U,
		I915_PCH_ADP,
		0xfU,
		1,
		i915_xd_ids_adlp,
		sizeof(i915_xd_ids_adlp) / sizeof(i915_xd_ids_adlp[0]),
		i915_xd_steps_adlp,
		sizeof(i915_xd_steps_adlp) / sizeof(i915_xd_steps_adlp[0]),
		i915_xd_ports_adlp,
		sizeof(i915_xd_ports_adlp) / sizeof(i915_xd_ports_adlp[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_TCSS,
		I915_XD_TCCOLD_AUX_WELL,
		i915_xd_wells_adlp,
		sizeof(i915_xd_wells_adlp) / sizeof(i915_xd_wells_adlp[0]),
		i915_xd_plls_adlp,
		sizeof(i915_xd_plls_adlp) / sizeof(i915_xd_plls_adlp[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_adlp,
		sizeof(i915_xd_cdclk_adlp) / sizeof(i915_xd_cdclk_adlp[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		1,
		4U,
		4096U,
		i915_xd_dbuf_adlp,
		sizeof(i915_xd_dbuf_adlp) / sizeof(i915_xd_dbuf_adlp[0]),
		&i915_xd_dbox_adlp,
		0x0U,
		0x3U,
		0,
		6U,
		3U,
		1,
		"i915/adlp_dmc.bin",
		I915_XD_DMC_PIPE_BASE_ADLP,
		&i915_xd_swing_adlp,
		I915_XD_WA_PW1_FLR_SOURCE | I915_XD_WA_DPCE_UNGATE | I915_XD_WA_PIPEDMC_UNGATE | I915_XD_WA_ARB_PROG_SLOTS |
		I915_XD_WA_DKL_LOADGEN | I915_XD_WA_TC_RESET_AFTER_TBT,
		&i915_xd_vbt_maps_adlp,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Alder Lake-N",
		I915_XD_KIND_ADLP,
		13U,
		I915_PCH_ADP,
		0xfU,
		1,
		i915_xd_ids_adln,
		sizeof(i915_xd_ids_adln) / sizeof(i915_xd_ids_adln[0]),
		i915_xd_steps_adln,
		sizeof(i915_xd_steps_adln) / sizeof(i915_xd_steps_adln[0]),
		i915_xd_ports_adlp,
		sizeof(i915_xd_ports_adlp) / sizeof(i915_xd_ports_adlp[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_TCSS,
		I915_XD_TCCOLD_AUX_WELL,
		i915_xd_wells_adlp,
		sizeof(i915_xd_wells_adlp) / sizeof(i915_xd_wells_adlp[0]),
		i915_xd_plls_adlp,
		sizeof(i915_xd_plls_adlp) / sizeof(i915_xd_plls_adlp[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_adlp,
		sizeof(i915_xd_cdclk_adlp) / sizeof(i915_xd_cdclk_adlp[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		1,
		4U,
		4096U,
		i915_xd_dbuf_adlp,
		sizeof(i915_xd_dbuf_adlp) / sizeof(i915_xd_dbuf_adlp[0]),
		&i915_xd_dbox_adlp,
		0x0U,
		0x3U,
		0,
		6U,
		3U,
		1,
		"i915/adlp_dmc.bin",
		I915_XD_DMC_PIPE_BASE_ADLP,
		&i915_xd_swing_adlp,
		I915_XD_WA_PW1_FLR_SOURCE | I915_XD_WA_DPCE_UNGATE | I915_XD_WA_PIPEDMC_UNGATE | I915_XD_WA_ARB_PROG_SLOTS |
		I915_XD_WA_DKL_LOADGEN | I915_XD_WA_TC_RESET_AFTER_TBT,
		&i915_xd_vbt_maps_adlp,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Raptor Lake-P",
		I915_XD_KIND_ADLP,
		13U,
		I915_PCH_ADP,
		0xfU,
		1,
		i915_xd_ids_rplp,
		sizeof(i915_xd_ids_rplp) / sizeof(i915_xd_ids_rplp[0]),
		i915_xd_steps_rplp,
		sizeof(i915_xd_steps_rplp) / sizeof(i915_xd_steps_rplp[0]),
		i915_xd_ports_adlp,
		sizeof(i915_xd_ports_adlp) / sizeof(i915_xd_ports_adlp[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_TCSS,
		I915_XD_TCCOLD_AUX_WELL,
		i915_xd_wells_adlp,
		sizeof(i915_xd_wells_adlp) / sizeof(i915_xd_wells_adlp[0]),
		i915_xd_plls_adlp,
		sizeof(i915_xd_plls_adlp) / sizeof(i915_xd_plls_adlp[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_adlp,
		sizeof(i915_xd_cdclk_adlp) / sizeof(i915_xd_cdclk_adlp[0]),
		i915_xd_voltage_tgl,
		sizeof(i915_xd_voltage_tgl) / sizeof(i915_xd_voltage_tgl[0]),
		1,
		4U,
		4096U,
		i915_xd_dbuf_adlp,
		sizeof(i915_xd_dbuf_adlp) / sizeof(i915_xd_dbuf_adlp[0]),
		&i915_xd_dbox_adlp,
		0x0U,
		0x3U,
		0,
		6U,
		3U,
		1,
		"i915/adlp_dmc.bin",
		I915_XD_DMC_PIPE_BASE_ADLP,
		&i915_xd_swing_adlp,
		I915_XD_WA_PW1_FLR_SOURCE | I915_XD_WA_DPCE_UNGATE | I915_XD_WA_PIPEDMC_UNGATE | I915_XD_WA_ARB_PROG_SLOTS |
		I915_XD_WA_DKL_LOADGEN | I915_XD_WA_TC_RESET_AFTER_TBT,
		&i915_xd_vbt_maps_adlp,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Raptor Lake-U",
		I915_XD_KIND_ADLP,
		13U,
		I915_PCH_ADP,
		0xfU,
		1,
		i915_xd_ids_rplu,
		sizeof(i915_xd_ids_rplu) / sizeof(i915_xd_ids_rplu[0]),
		i915_xd_steps_rplp,
		sizeof(i915_xd_steps_rplp) / sizeof(i915_xd_steps_rplp[0]),
		i915_xd_ports_adlp,
		sizeof(i915_xd_ports_adlp) / sizeof(i915_xd_ports_adlp[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_TCSS,
		I915_XD_TCCOLD_AUX_WELL,
		i915_xd_wells_adlp,
		sizeof(i915_xd_wells_adlp) / sizeof(i915_xd_wells_adlp[0]),
		i915_xd_plls_adlp,
		sizeof(i915_xd_plls_adlp) / sizeof(i915_xd_plls_adlp[0]),
		I915_XD_PLL_MAP_CFGCR0,
		1,
		i915_xd_cdclk_rplu,
		sizeof(i915_xd_cdclk_rplu) / sizeof(i915_xd_cdclk_rplu[0]),
		i915_xd_voltage_rplu,
		sizeof(i915_xd_voltage_rplu) / sizeof(i915_xd_voltage_rplu[0]),
		1,
		4U,
		4096U,
		i915_xd_dbuf_adlp,
		sizeof(i915_xd_dbuf_adlp) / sizeof(i915_xd_dbuf_adlp[0]),
		&i915_xd_dbox_adlp,
		0x0U,
		0x3U,
		0,
		6U,
		3U,
		1,
		"i915/adlp_dmc.bin",
		I915_XD_DMC_PIPE_BASE_ADLP,
		&i915_xd_swing_adlp,
		I915_XD_WA_PW1_FLR_SOURCE | I915_XD_WA_DPCE_UNGATE | I915_XD_WA_PIPEDMC_UNGATE | I915_XD_WA_ARB_PROG_SLOTS |
		I915_XD_WA_DKL_LOADGEN | I915_XD_WA_TC_RESET_AFTER_TBT,
		&i915_xd_vbt_maps_adlp,
		I915_XD_FIA_READ_MODULAR,
		NULL,
		NULL,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Ice Lake",
		I915_XD_KIND_ICL,
		11U,
		I915_PCH_ICP,
		0x7U,
		0,
		i915_xd_ids_icl,
		sizeof(i915_xd_ids_icl) / sizeof(i915_xd_ids_icl[0]),
		i915_xd_steps_icl,
		sizeof(i915_xd_steps_icl) / sizeof(i915_xd_steps_icl[0]),
		i915_xd_ports_icl,
		sizeof(i915_xd_ports_icl) / sizeof(i915_xd_ports_icl[0]),
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_FIA,
		I915_XD_TCCOLD_AUX_WELL_EXIT,
		i915_xd_wells_icl,
		sizeof(i915_xd_wells_icl) / sizeof(i915_xd_wells_icl[0]),
		i915_xd_plls_icl,
		sizeof(i915_xd_plls_icl) / sizeof(i915_xd_plls_icl[0]),
		I915_XD_PLL_MAP_CFGCR0,
		0,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_icl,
		sizeof(i915_xd_voltage_icl) / sizeof(i915_xd_voltage_icl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_icl,
		sizeof(i915_xd_dbuf_icl) / sizeof(i915_xd_dbuf_icl[0]),
		&i915_xd_dbox_icl,
		0x1U,
		0x0U,
		0,
		8U,
		2U,
		0,
		"i915/icl_dmc_ver1_09.bin",
		0U,
		&i915_xd_swing_icl,
		I915_XD_WA_COMBO_AUX_LANE | I915_XD_WA_AUX_ANAOVRD_1178,
		&i915_xd_vbt_maps_tgl,
		I915_XD_FIA_SINGLE,
		&i915_xd_tbt_pll_icl_19p2,
		&i915_xd_tbt_pll_icl_24,
		I915_XD_POWER_CLASS_H,
		NULL
	},
	{
		"Ice Lake without port F",
		I915_XD_KIND_ICL,
		11U,
		I915_PCH_ICP,
		0x7U,
		0,
		i915_xd_ids_icl_no_f,
		sizeof(i915_xd_ids_icl_no_f) / sizeof(i915_xd_ids_icl_no_f[0]),
		i915_xd_steps_icl,
		sizeof(i915_xd_steps_icl) / sizeof(i915_xd_steps_icl[0]),
		i915_xd_ports_icl,
		sizeof(i915_xd_ports_icl) / sizeof(i915_xd_ports_icl[0]) - 1U,
		0x3U,
		0x1U,
		0x3U,
		0U,
		I915_XD_TC_FLOW_FIA,
		I915_XD_TCCOLD_AUX_WELL_EXIT,
		i915_xd_wells_icl,
		sizeof(i915_xd_wells_icl) / sizeof(i915_xd_wells_icl[0]),
		i915_xd_plls_icl,
		sizeof(i915_xd_plls_icl) / sizeof(i915_xd_plls_icl[0]),
		I915_XD_PLL_MAP_CFGCR0,
		0,
		i915_xd_cdclk_tgl,
		sizeof(i915_xd_cdclk_tgl) / sizeof(i915_xd_cdclk_tgl[0]),
		i915_xd_voltage_icl,
		sizeof(i915_xd_voltage_icl) / sizeof(i915_xd_voltage_icl[0]),
		0,
		2U,
		2048U,
		i915_xd_dbuf_icl,
		sizeof(i915_xd_dbuf_icl) / sizeof(i915_xd_dbuf_icl[0]),
		&i915_xd_dbox_icl,
		0x1U,
		0x0U,
		0,
		8U,
		2U,
		0,
		"i915/icl_dmc_ver1_09.bin",
		0U,
		&i915_xd_swing_icl,
		I915_XD_WA_COMBO_AUX_LANE | I915_XD_WA_AUX_ANAOVRD_1178,
		&i915_xd_vbt_maps_tgl,
		I915_XD_FIA_SINGLE,
		&i915_xd_tbt_pll_icl_19p2,
		&i915_xd_tbt_pll_icl_24,
		I915_XD_POWER_CLASS_H,
		NULL
	}
};

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_TABLES_XD_H */
