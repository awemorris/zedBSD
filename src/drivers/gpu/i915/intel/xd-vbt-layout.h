/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the Video BIOS Table (VBT) as the xd display code
 * (display/xd-vbt.c) reads it: the VBT header, the BIOS data block (BDB)
 * header, the block headers, and the offsets and bit masks of the fields
 * of the blocks the display uses.  The firmware writes these tables; the
 * code never writes them.
 *
 * Every offset counts bytes from the start of the structure it names (a
 * block offset from the first byte after the block header, a child
 * device offset from the start of the child).  A value of more than one
 * byte is little endian.  A mask names bits of the byte (or word) at the
 * offset given with it.  Fields are read one by one with bounds checks;
 * there are no packed structures.
 */

#ifndef DRIVERS_GPU_I915_INTEL_XD_VBT_LAYOUT_H
#define DRIVERS_GPU_I915_INTEL_XD_VBT_LAYOUT_H

/*
 * The VBT header: the signature (its first four bytes are "$VBT"), the
 * size of the whole VBT, and where the BDB starts.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct vbt_header).
 */
#define I915_XD_VBT_SIGNATURE			0U
#define I915_XD_VBT_SIGNATURE_CHECKED		4U
#define I915_XD_VBT_VERSION			20U
#define I915_XD_VBT_HEADER_SIZE			22U
#define I915_XD_VBT_VBT_SIZE			24U
#define I915_XD_VBT_BDB_OFFSET			28U
#define I915_XD_VBT_HEADER_BYTES		48U

/*
 * The BDB header: its signature ("BIOS_DATA_BLOCK "), the BDB version
 * the field rules below depend on, the size of the header (the first
 * block follows it), and the size of the whole BDB.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_header).
 */
#define I915_XD_BDB_SIGNATURE			0U
#define I915_XD_BDB_VERSION			16U
#define I915_XD_BDB_HEADER_SIZE			18U
#define I915_XD_BDB_SIZE			20U
#define I915_XD_BDB_HEADER_BYTES		22U

/*
 * A block header: the block id and the 16-bit size of the block's data
 * that follows the three header bytes.  The MIPI sequence block from its
 * version 3 keeps its size in a 32-bit word after its version byte.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (_get_blocksize).
 */
#define I915_XD_BLOCK_ID			0U
#define I915_XD_BLOCK_SIZE			1U
#define I915_XD_BLOCK_HEADER_BYTES		3U
#define I915_XD_BLOCK_MIPI_VERSION		3U
#define I915_XD_BLOCK_MIPI_SIZE			4U
#define I915_XD_BLOCK_MIPI_LONG_VERSION		3U

/*
 * The ids of the blocks the display reads, and of the MIPI sequence
 * block whose size is kept differently.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (enum bdb_block_id).
 */
#define I915_XD_BLOCK_GENERAL_FEATURES		1U
#define I915_XD_BLOCK_GENERAL_DEFINITIONS	2U
#define I915_XD_BLOCK_DRIVER_FEATURES		12U
#define I915_XD_BLOCK_EDP			27U
#define I915_XD_BLOCK_LFP_OPTIONS		40U
#define I915_XD_BLOCK_LFP_DATA_POINTERS		41U
#define I915_XD_BLOCK_LFP_DATA			42U
#define I915_XD_BLOCK_LFP_BACKLIGHT		43U
#define I915_XD_BLOCK_LFP_POWER			44U
#define I915_XD_BLOCK_MIPI_SEQUENCE		53U
#define I915_XD_BLOCK_GENERIC_DTD		58U

/*
 * The general features block: spread spectrum on the panel clock and
 * its frequency choice, the display clock mode, and (BDB 249 on) the
 * AFC startup override.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_general_features: enable_ssc, ssc_freq, display_clock_mode,
 * afc_startup_config).
 */
#define I915_XD_VBT_FEATURES_BITS2		1U
#define I915_XD_VBT_FEATURES_SSC		0x02U
#define I915_XD_VBT_FEATURES_SSC_ALTERNATE	0x04U
#define I915_XD_VBT_FEATURES_CLOCK_MODE		0x40U
#define I915_XD_VBT_FEATURES_BITS7		6U
#define I915_XD_VBT_FEATURES_AFC_STARTUP	0x03U

/*
 * The general definitions block: the CRT DDC pin, the size of one child
 * device, and the child devices that follow.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_general_definitions).
 */
#define I915_XD_VBT_DEFS_CRT_DDC_PIN		0U
#define I915_XD_VBT_DEFS_CHILD_SIZE		4U
#define I915_XD_VBT_DEFS_CHILDREN		5U

/*
 * One child device: its handle, its device type, the HDMI level shift
 * and the HDMI data rate limit, its DVO port, its DDC pin, the lane
 * reversal, current boost, hotplug inversion and lane count limit flags,
 * its AUX channel, its Type-C and Thunderbolt flags, its current boost
 * levels and its DisplayPort rate limit; and the smallest child size the
 * display reads at all, and the size of all the fields it knows.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct child_device_config: handle, device_type,
 * hdmi_level_shifter_value, hdmi_max_data_rate, dvo_port, ddc_pin,
 * lane_reversal, iboost, hpd_invert, dp_max_lane_count, aux_channel,
 * dp_usb_type_c, tbt, dp_iboost_level, hdmi_iboost_level,
 * dp_max_link_rate; LEGACY_CHILD_DEVICE_CONFIG_SIZE).
 */
#define I915_XD_VBT_CHILD_HANDLE		0U
#define I915_XD_VBT_CHILD_DEVICE_TYPE		2U
#define I915_XD_VBT_CHILD_HDMI_BITS		7U
#define I915_XD_VBT_CHILD_HDMI_LEVEL_SHIFT	0x1fU
#define I915_XD_VBT_CHILD_HDMI_MAX_RATE		0xe0U
#define I915_XD_VBT_CHILD_DVO_PORT		16U
#define I915_XD_VBT_CHILD_DDC_PIN		19U
#define I915_XD_VBT_CHILD_FLAGS			23U
#define I915_XD_VBT_CHILD_LANE_REVERSAL		0x02U
#define I915_XD_VBT_CHILD_IBOOST		0x08U
#define I915_XD_VBT_CHILD_HPD_INVERT		0x10U
#define I915_XD_VBT_CHILD_DP_MAX_LANES		0xc0U
#define I915_XD_VBT_CHILD_AUX_CHANNEL		25U
#define I915_XD_VBT_CHILD_FLAGS2		33U
#define I915_XD_VBT_CHILD_TYPE_C		0x01U
#define I915_XD_VBT_CHILD_TBT			0x02U
#define I915_XD_VBT_CHILD_IBOOST_LEVELS		37U
#define I915_XD_VBT_CHILD_DP_IBOOST		0x0fU
#define I915_XD_VBT_CHILD_HDMI_IBOOST		0xf0U
#define I915_XD_VBT_CHILD_DP_RATE_BITS		38U
#define I915_XD_VBT_CHILD_DP_MAX_RATE		0x07U
#define I915_XD_VBT_CHILD_MIN_BYTES		33U
#define I915_XD_VBT_CHILD_KNOWN_BYTES		39U

/*
 * The handles of the two built-in panels (each picks its own panel type).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (DEVICE_HANDLE_LFP1, DEVICE_HANDLE_LFP2).
 */
#define I915_XD_VBT_HANDLE_LFP1			0x0008U
#define I915_XD_VBT_HANDLE_LFP2			0x0080U

/*
 * The bits of a child's device type: analog output, DisplayPort output,
 * TMDS (DVI) signalling, MIPI output, "not an HDMI output", and the
 * internal connector (a built-in panel).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (DEVICE_TYPE_ANALOG_OUTPUT, DEVICE_TYPE_DISPLAYPORT_OUTPUT,
 * DEVICE_TYPE_TMDS_DVI_SIGNALING, DEVICE_TYPE_MIPI_OUTPUT,
 * DEVICE_TYPE_NOT_HDMI_OUTPUT, DEVICE_TYPE_INTERNAL_CONNECTOR).
 */
#define I915_XD_VBT_TYPE_ANALOG			0x0001U
#define I915_XD_VBT_TYPE_DISPLAYPORT		0x0004U
#define I915_XD_VBT_TYPE_TMDS			0x0010U
#define I915_XD_VBT_TYPE_MIPI			0x0400U
#define I915_XD_VBT_TYPE_NOT_HDMI		0x0800U
#define I915_XD_VBT_TYPE_INTERNAL		0x1000U

/*
 * The DVO port codes a child names its port with: HDMI A..I, DP A..I and
 * the CRT; and NONE, which this code uses for "no code" in its tables.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (DVO_PORT_HDMIA..DVO_PORT_HDMII, DVO_PORT_DPA..DVO_PORT_DPI,
 * DVO_PORT_CRT).
 */
#define I915_XD_DVO_HDMI_A			0U
#define I915_XD_DVO_HDMI_B			1U
#define I915_XD_DVO_HDMI_C			2U
#define I915_XD_DVO_HDMI_D			3U
#define I915_XD_DVO_CRT				6U
#define I915_XD_DVO_DP_B			7U
#define I915_XD_DVO_DP_C			8U
#define I915_XD_DVO_DP_D			9U
#define I915_XD_DVO_DP_A			10U
#define I915_XD_DVO_DP_E			11U
#define I915_XD_DVO_HDMI_E			12U
#define I915_XD_DVO_DP_F			13U
#define I915_XD_DVO_HDMI_F			14U
#define I915_XD_DVO_DP_G			15U
#define I915_XD_DVO_HDMI_G			16U
#define I915_XD_DVO_DP_H			17U
#define I915_XD_DVO_HDMI_H			18U
#define I915_XD_DVO_DP_I			19U
#define I915_XD_DVO_HDMI_I			20U
#define I915_XD_DVO_NONE			0xffU

/*
 * The AUX channel codes a child names its AUX channel with.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (DP_AUX_A..DP_AUX_I).
 */
#define I915_XD_VBT_AUX_A			0x40U
#define I915_XD_VBT_AUX_B			0x10U
#define I915_XD_VBT_AUX_C			0x20U
#define I915_XD_VBT_AUX_D			0x30U
#define I915_XD_VBT_AUX_E			0x50U
#define I915_XD_VBT_AUX_F			0x60U
#define I915_XD_VBT_AUX_G			0x70U
#define I915_XD_VBT_AUX_H			0x80U
#define I915_XD_VBT_AUX_I			0x90U

/*
 * The HDMI data rate limit codes of a child (BDB 204 on, the last three
 * BDB 249 on), as the TMDS clock in kHz they stand for (0: the
 * platform's own limit).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (HDMI_MAX_DATA_RATE_PLATFORM, _297, _165, _594, _340, _300) and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_hdmi_max_tmds_clock).
 */
#define I915_XD_VBT_HDMI_RATE_PLATFORM		0U
#define I915_XD_VBT_HDMI_RATE_297		1U
#define I915_XD_VBT_HDMI_RATE_165		2U
#define I915_XD_VBT_HDMI_RATE_594		3U
#define I915_XD_VBT_HDMI_RATE_340		4U
#define I915_XD_VBT_HDMI_RATE_300		5U

/*
 * The current boost values a child's boost level codes 0..2 stand for.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (translate_iboost).
 */
#define I915_XD_VBT_IBOOST_CODES		3U
#define I915_XD_VBT_IBOOST_0			1U
#define I915_XD_VBT_IBOOST_1			3U
#define I915_XD_VBT_IBOOST_2			7U

/*
 * The DisplayPort rate limit codes of a child: from BDB 216 to 229 the
 * codes 0..3 are HBR3, HBR2, HBR, RBR; from BDB 230 the codes 0..7 are
 * none, RBR, HBR, HBR2, HBR3, UHBR10, UHBR13.5, UHBR20.  The rates are
 * the link symbol clocks in kHz.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (BDB_216_VBT_DP_MAX_LINK_RATE_*, BDB_230_VBT_DP_MAX_LINK_RATE_*) and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_bdb_216_dp_max_link_rate, parse_bdb_230_dp_max_link_rate).
 */
#define I915_XD_VBT_RATE_RBR_KHZ		162000U
#define I915_XD_VBT_RATE_HBR_KHZ		270000U
#define I915_XD_VBT_RATE_HBR2_KHZ		540000U
#define I915_XD_VBT_RATE_HBR3_KHZ		810000U
#define I915_XD_VBT_RATE_UHBR10_KHZ		1000000U
#define I915_XD_VBT_RATE_UHBR13P5_KHZ		1350000U
#define I915_XD_VBT_RATE_UHBR20_KHZ		2000000U

/*
 * The BDB versions from which a field is present or read differently.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (the version tests of parse_general_features, intel_bios_hdmi_level_shift,
 * intel_bios_encoder_supports_typec_usb, intel_bios_dp_boost_level,
 * intel_bios_hdmi_max_tmds_clock, intel_bios_encoder_supports_tbt,
 * intel_bios_dp_max_link_rate, intel_bios_dp_max_lane_count,
 * generate_lfp_data_ptrs, parse_lfp_backlight, parse_panel_driver_features,
 * parse_power_conservation_features, parse_edp, parse_generic_dtd) and
 * Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h (the
 * version notes of the fields).
 */
#define I915_XD_BDB_GENERATED_POINTERS		155U
#define I915_XD_BDB_HDMI_LEVEL_SHIFT		158U
#define I915_XD_BDB_EDP_LOW_VSWING		173U
#define I915_XD_BDB_BACKLIGHT_CONTROL		191U
#define I915_XD_BDB_TYPE_C			195U
#define I915_XD_BDB_IBOOST			196U
#define I915_XD_BDB_HDMI_MAX_RATE		204U
#define I915_XD_BDB_TBT				209U
#define I915_XD_BDB_DP_MAX_RATE			216U
#define I915_XD_BDB_EDP_RATE_WORD		224U
#define I915_XD_BDB_LFP_POWER_FLAGS		228U
#define I915_XD_BDB_GENERIC_DTD			229U
#define I915_XD_BDB_DP_MAX_RATE_V2		230U
#define I915_XD_BDB_HOBL			232U
#define I915_XD_BDB_VRR				233U
#define I915_XD_BDB_BRIGHTNESS_WORDS		234U
#define I915_XD_BDB_BRIGHTNESS_PRECISION	236U
#define I915_XD_BDB_DP_MAX_LANES		244U
#define I915_XD_BDB_EDP_MAX_RATE		244U
#define I915_XD_BDB_AFC_STARTUP			249U

/*
 * The driver features block: the panel configuration (an internal
 * panel or not) in the extended bits, and, before BDB 228, the DRRS,
 * PSR and DMRRS switches.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_driver_features: lvds_config, drrs_enabled, psr_enabled,
 * dmrrs_enabled; BDB_DRIVER_FEATURE_INT_LVDS).
 */
#define I915_XD_VBT_DRIVER_EXT_BITS		7U
#define I915_XD_VBT_DRIVER_LFP_CONFIG		0x1800U
#define I915_XD_VBT_DRIVER_LFP_INTERNAL		1U
#define I915_XD_VBT_DRIVER_FLAGS		17U
#define I915_XD_VBT_DRIVER_DRRS			0x0020U
#define I915_XD_VBT_DRIVER_PSR			0x0200U
#define I915_XD_VBT_DRIVER_DMRRS		0x1000U

/*
 * The eDP block, sixteen panel entries of each field: the power sequence
 * (five 16-bit delays in 100 us units: T1+T3, T8, T9, T10, T11+T12), the
 * colour depth (2 bits a panel), the fast link parameters (rate and lane
 * codes, pre-emphasis and swing codes), the swing table choice (4 bits a
 * panel, BDB 173 on), the link rate in 20 kHz units (BDB 224 on) and the
 * largest link rate (BDB 244 on).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_edp, struct edp_fast_link_params, EDP_18BPP, EDP_24BPP,
 * EDP_30BPP, EDP_RATE_*, EDP_LANE_*) and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_bios.h (struct edp_power_seq).
 */
#define I915_XD_VBT_EDP_PPS			0U
#define I915_XD_VBT_EDP_PPS_STRIDE		10U
#define I915_XD_VBT_EDP_PPS_T1_T3		0U
#define I915_XD_VBT_EDP_PPS_T8			2U
#define I915_XD_VBT_EDP_PPS_T9			4U
#define I915_XD_VBT_EDP_PPS_T10			6U
#define I915_XD_VBT_EDP_PPS_T11_T12		8U
#define I915_XD_VBT_EDP_COLOR_DEPTH		160U
#define I915_XD_VBT_EDP_LINK			164U
#define I915_XD_VBT_EDP_LINK_STRIDE		2U
#define I915_XD_VBT_EDP_LINK_RATE		0x0fU
#define I915_XD_VBT_EDP_LINK_LANES		0xf0U
#define I915_XD_VBT_EDP_LINK_PREEMPHASIS	0x0fU
#define I915_XD_VBT_EDP_LINK_VSWING		0xf0U
#define I915_XD_VBT_EDP_VSWING_PREEMPH		204U
#define I915_XD_VBT_EDP_LINK_RATE_WORDS		748U
#define I915_XD_VBT_EDP_MAX_RATE_WORDS		780U
#define I915_XD_VBT_EDP_BPP_18			0U
#define I915_XD_VBT_EDP_BPP_24			1U
#define I915_XD_VBT_EDP_BPP_30			2U
#define I915_XD_VBT_EDP_RATE_1P62		0U
#define I915_XD_VBT_EDP_RATE_2P7		1U
#define I915_XD_VBT_EDP_RATE_5P4		2U
#define I915_XD_VBT_EDP_LANES_1			0U
#define I915_XD_VBT_EDP_LANES_2			1U
#define I915_XD_VBT_EDP_LANES_4			3U
#define I915_XD_VBT_EDP_RATE_UNIT_KHZ		20U

/*
 * The panel options block: the panel type (of the first and the second
 * built-in panel; 0xff asks the driver to match the EDID), the dither
 * flag, and the DRRS mode bits (2 bits a panel; read only when the block
 * has at least 16 bytes).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_lvds_options: panel_type, panel_type2, pixel_dither,
 * dps_panel_type_bits) and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_bios.c (parse_panel_options,
 * vbt_get_panel_type).
 */
#define I915_XD_VBT_LFP_PANEL_TYPE		0U
#define I915_XD_VBT_LFP_PANEL_TYPE2		1U
#define I915_XD_VBT_LFP_CAPS			2U
#define I915_XD_VBT_LFP_DITHER			0x20U
#define I915_XD_VBT_LFP_DRRS_BITS		16U
#define I915_XD_VBT_LFP_DRRS_MIN_BLOCK		16U
#define I915_XD_VBT_PANEL_TYPE_MATCH_EDID	0xffU
#define I915_XD_VBT_PANEL_TYPES			16U

/*
 * The panel data pointers block: the number of tables an entry has
 * (always three), then for each of the sixteen panels the offset (from
 * the start of the BDB) and size of its timing table, its DVO timing and
 * its PnP id, and last the offset and size of the panel name table.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_lvds_lfp_data_ptrs, struct lvds_lfp_data_ptr,
 * struct lvds_lfp_data_ptr_table).
 */
#define I915_XD_VBT_PTRS_ENTRIES		0U
#define I915_XD_VBT_PTRS_TABLES			1U
#define I915_XD_VBT_PTRS_STRIDE			9U
#define I915_XD_VBT_PTRS_FP			0U
#define I915_XD_VBT_PTRS_DVO			3U
#define I915_XD_VBT_PTRS_PNP			6U
#define I915_XD_VBT_PTRS_OFFSET			0U
#define I915_XD_VBT_PTRS_TABLE_SIZE		2U
#define I915_XD_VBT_PTRS_PANEL_NAME		145U
#define I915_XD_VBT_PTRS_EXPECTED_ENTRIES	3U

/*
 * The sizes of the panel data tables: a DVO timing, a PnP id, a panel
 * name, the smallest timing table, the timing table size a pointer table
 * is made for when the VBT has none, the six bytes past the timing
 * table that real VBTs leave out of its size, and the terminator word at
 * the end of each timing table.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct lvds_dvo_timing, struct lvds_pnp_id, struct lvds_lfp_panel_name)
 * and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (validate_lfp_data_ptrs, generate_lfp_data_ptrs).
 */
#define I915_XD_VBT_DVO_TIMING_BYTES		18U
#define I915_XD_VBT_PNP_BYTES			10U
#define I915_XD_VBT_PANEL_NAME_BYTES		13U
#define I915_XD_VBT_FP_TIMING_MIN		32U
#define I915_XD_VBT_FP_TIMING_GENERATED		38U
#define I915_XD_VBT_FP_TIMING_HOLE		6U
#define I915_XD_VBT_FP_TERMINATOR		0xffffU

/*
 * A DVO timing (the layout of an EDID detailed timing): the clock in
 * 10 kHz units, the active and blank sizes with their upper nibbles, the
 * sync offsets and widths with their upper bits, the image size, and the
 * sync polarities.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct lvds_dvo_timing).
 */
#define I915_XD_DVO_CLOCK			0U
#define I915_XD_DVO_HACTIVE_LO			2U
#define I915_XD_DVO_HBLANK_LO			3U
#define I915_XD_DVO_H_HI			4U
#define I915_XD_DVO_HBLANK_HI			0x0fU
#define I915_XD_DVO_HACTIVE_HI			0xf0U
#define I915_XD_DVO_VACTIVE_LO			5U
#define I915_XD_DVO_VBLANK_LO			6U
#define I915_XD_DVO_V_HI			7U
#define I915_XD_DVO_VBLANK_HI			0x0fU
#define I915_XD_DVO_VACTIVE_HI			0xf0U
#define I915_XD_DVO_HSYNC_OFF_LO		8U
#define I915_XD_DVO_HSYNC_WIDTH_LO		9U
#define I915_XD_DVO_VSYNC_LO			10U
#define I915_XD_DVO_VSYNC_WIDTH_LO		0x0fU
#define I915_XD_DVO_VSYNC_OFF_LO		0xf0U
#define I915_XD_DVO_SYNC_HI			11U
#define I915_XD_DVO_VSYNC_WIDTH_HI		0x03U
#define I915_XD_DVO_VSYNC_OFF_HI		0x0cU
#define I915_XD_DVO_HSYNC_WIDTH_HI		0x30U
#define I915_XD_DVO_HSYNC_OFF_HI		0xc0U
#define I915_XD_DVO_HIMAGE_LO			12U
#define I915_XD_DVO_VIMAGE_LO			13U
#define I915_XD_DVO_IMAGE_HI			14U
#define I915_XD_DVO_VIMAGE_HI			0x0fU
#define I915_XD_DVO_HIMAGE_HI			0xf0U
#define I915_XD_DVO_FLAGS			17U
#define I915_XD_DVO_VSYNC_POSITIVE		0x20U
#define I915_XD_DVO_HSYNC_POSITIVE		0x40U

/*
 * The backlight block: the size of one entry (six bytes), sixteen
 * entries (the type, two of which means a PWM backlight, and its
 * polarity; the PWM frequency in Hz; the old minimum brightness), the old
 * brightness levels, the control methods (type and controller), and from
 * BDB 234 the brightness and minimum brightness words (four bytes a
 * panel) and from BDB 236 the precision bits.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_lfp_backlight_data, struct lfp_backlight_data_entry,
 * struct lfp_backlight_control_method, struct lfp_brightness_level,
 * BDB_BACKLIGHT_TYPE_PWM).
 */
#define I915_XD_VBT_BL_ENTRY_SIZE		0U
#define I915_XD_VBT_BL_ENTRIES			1U
#define I915_XD_VBT_BL_ENTRY_BYTES		6U
#define I915_XD_VBT_BL_FLAGS			0U
#define I915_XD_VBT_BL_TYPE			0x03U
#define I915_XD_VBT_BL_ACTIVE_LOW		0x04U
#define I915_XD_VBT_BL_PWM_HZ			1U
#define I915_XD_VBT_BL_OLD_MIN			3U
#define I915_XD_VBT_BL_TYPE_PWM			2U
#define I915_XD_VBT_BL_OLD_LEVELS		97U
#define I915_XD_VBT_BL_CONTROL			113U
#define I915_XD_VBT_BL_CONTROL_TYPE		0x0fU
#define I915_XD_VBT_BL_CONTROL_CONTROLLER	0xf0U
#define I915_XD_VBT_BL_LEVELS			129U
#define I915_XD_VBT_BL_MIN_LEVELS		193U
#define I915_XD_VBT_BL_LEVEL_STRIDE		4U
#define I915_XD_VBT_BL_PRECISION		257U

/*
 * The power conservation block (BDB 228 on): the DRRS and DMRRS
 * switches, HOBL (BDB 232 on) and VRR (BDB 233 on), one bit a panel.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_lfp_power: drrs, dmrrs, hobl, vrr_feature_enabled).
 */
#define I915_XD_VBT_POWER_DRRS			26U
#define I915_XD_VBT_POWER_DMRRS			32U
#define I915_XD_VBT_POWER_HOBL			54U
#define I915_XD_VBT_POWER_VRR			56U

/*
 * The generic DTD block (BDB 229 on): the size of one entry, then the
 * entries: the pixel clock in kHz, the active, blank, front porch and
 * sync sizes, the image size, and the sync polarities.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vbt_defs.h
 * (struct bdb_generic_dtd, struct generic_dtd_entry).
 */
#define I915_XD_VBT_GDTD_SIZE			0U
#define I915_XD_VBT_GDTD_ENTRIES		2U
#define I915_XD_VBT_GDTD_ENTRY_BYTES		28U
#define I915_XD_VBT_GDTD_CLOCK			0U
#define I915_XD_VBT_GDTD_HACTIVE		4U
#define I915_XD_VBT_GDTD_HBLANK			6U
#define I915_XD_VBT_GDTD_HFRONT			8U
#define I915_XD_VBT_GDTD_HSYNC			10U
#define I915_XD_VBT_GDTD_VACTIVE		12U
#define I915_XD_VBT_GDTD_VBLANK			14U
#define I915_XD_VBT_GDTD_VFRONT			16U
#define I915_XD_VBT_GDTD_VSYNC			18U
#define I915_XD_VBT_GDTD_WIDTH_MM		20U
#define I915_XD_VBT_GDTD_HEIGHT_MM		22U
#define I915_XD_VBT_GDTD_FLAGS			24U
#define I915_XD_VBT_GDTD_VSYNC_POSITIVE		0x40U
#define I915_XD_VBT_GDTD_HSYNC_POSITIVE		0x80U

/*
 * The panel clock spread spectrum frequencies (kHz) the general features
 * choose between on the platforms with a PCH.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_ssc_frequency).
 */
#define I915_XD_VBT_SSC_KHZ			120000U
#define I915_XD_VBT_SSC_ALTERNATE_KHZ		100000U

/*
 * The AFC startup values the general features override with: code 1
 * asks for 0, the others for 7.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_general_features: override_afc_startup_val).
 */
#define I915_XD_VBT_AFC_CODE_ZERO		1U
#define I915_XD_VBT_AFC_ZERO			0x0U
#define I915_XD_VBT_AFC_SEVEN			0x7U

#endif /* DRIVERS_GPU_I915_INTEL_XD_VBT_LAYOUT_H */
