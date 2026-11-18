/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's description of the display for the display 12 and 13
 * code (xd-vbt.c): where the ACPI OpRegion keeps the Video BIOS Table
 * (VBT), and what the VBT says.
 *
 * The VBT lists the outputs the board wires (one child device each: its
 * DDI, its AUX channel and DDC pin, what it can carry, its link limits)
 * and describes the built-in panel (its timing, power sequence delays,
 * backlight and eDP link).  The parse reads the bytes in place and
 * copies the values out; it keeps no pointer into them except for the
 * panel pass, which reads them again, so the bytes must stay mapped
 * until the panel is parsed.  A VBT the code cannot use is refused as a
 * whole, and the display then runs on the defaults a missing VBT gets
 * (an eDP panel on DDI A, DisplayPort and DVI on the other non Type-C
 * DDIs).
 *
 * Nothing here touches the hardware; the mapping of the OpRegion and of
 * the VBT is the caller's.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_VBT_H
#define DRIVERS_GPU_I915_DISPLAY_XD_VBT_H

#include <stddef.h>
#include <stdint.h>

#include "display-internal.h"
#include "xd-platform.h"

/* The most child devices a parse keeps; a VBT lists far fewer. */
#define I915_XD_VBT_MAX_CHILDREN	16U

/* A child's DDI, AUX channel or GMBUS pin that the VBT does not give or the platform does not have. */
#define I915_XD_VBT_NONE		0xffU

/* Where an OpRegion keeps its VBT: nowhere, in mailbox 4, or at the raw VBT address. */
#define I915_XD_VBT_FROM_NONE	0U
#define I915_XD_VBT_FROM_MAILBOX	1U
#define I915_XD_VBT_FROM_RAW	2U

/* Which rule chose the panel type: the VBT's own, the EDID's PnP id, or the first panel. */
#define I915_XD_PANEL_TYPE_VBT		1U
#define I915_XD_PANEL_TYPE_PNP		2U
#define I915_XD_PANEL_TYPE_FIRST	3U

/* The refresh rate switching the panel allows: none, at a mode set, or seamless. */
#define I915_XD_DRRS_NONE		0U
#define I915_XD_DRRS_STATIC		1U
#define I915_XD_DRRS_SEAMLESS		2U

/*
 * Where an OpRegion keeps its VBT.
 *
 * The raw VBT (OpRegion 2.0 on, when the ASLE mailbox names one) is
 * tried first: the caller maps raw_address for raw_size bytes and checks
 * it.  When there is no raw VBT, or it does not check, the VBT is in
 * mailbox 4: mailbox_size bytes at mailbox_offset inside the OpRegion.
 */
struct i915_xd_opregion_vbt {
	/* The OpRegion version and the mailboxes it has. */
	uint8_t major;
	uint8_t minor;
	uint32_t mailboxes;

	/* The physical address and size of the raw VBT; 0 when the OpRegion names none. */
	uint64_t raw_address;
	uint32_t raw_size;

	/* Mailbox 4: its offset in the OpRegion and the room the VBT may use. */
	uint32_t mailbox_offset;
	uint32_t mailbox_size;
};

/*
 * One output the VBT lists (a child device).
 *
 * ddi is I915_XD_VBT_NONE for a child whose DVO port the platform does
 * not have; such a child is kept so the count matches the VBT, but no
 * port finds it.
 */
struct i915_xd_vbt_child {
	/* Where the child is: its DDI, its handle and its DVO port code. */
	uint8_t ddi;
	uint16_t handle;
	uint8_t dvo_port;

	/* What it carries: the device type bits and the outputs they make. */
	uint16_t device_type;
	int dp;
	int edp;
	int hdmi;
	int dvi;
	int type_c;
	int tbt;

	/*
	 * Its AUX channel (I915_XD_VBT_NONE) and GMBUS pin (0) when the VBT
	 * gives none the platform has.
	 */
	uint8_t aux_ch;
	uint8_t gmbus_pin;

	/* The DDC pin code as the VBT wrote it (0 for none). */
	uint8_t ddc_code;

	/* The wiring flags. */
	int lane_reversal;
	int hpd_invert;

	/*
	 * The link limits: the largest DisplayPort link rate (kHz, 0: the
	 * platform's), the most lanes (0: no limit), the largest TMDS clock
	 * (kHz, 0: the platform's).
	 */
	uint32_t dp_max_rate_khz;
	uint8_t dp_max_lanes;
	uint32_t hdmi_max_tmds_khz;

	/* The current boosts of DisplayPort and HDMI (0: none), and the HDMI swing row (-1: none). */
	uint8_t dp_boost;
	uint8_t hdmi_boost;
	int hdmi_level_shift;
};

/*
 * What one VBT says about the board, or the defaults when there is no
 * VBT.
 *
 * The parse fills it once; it holds no state of its own afterwards.
 * bytes and size still name the VBT for the panel pass.
 */
struct i915_xd_vbt {
	/* The platform the VBT was read for (its port and pin numbering). */
	const struct i915_xd_platform *platform;

	/* Nonzero when a VBT was parsed; zero when the defaults stand in for it. */
	int found;

	/* The VBT, and where its BDB starts and how long it is; NULL and 0 for the defaults. */
	const uint8_t *bytes;
	size_t size;
	uint32_t bdb_offset;
	uint32_t bdb_size;

	/* The BDB version (155 for the defaults, as if a minimal VBT had been found). */
	uint16_t version;

	/* The general features: panel clock spread spectrum, its frequency, the display clock mode. */
	int ssc;
	uint32_t ssc_khz;
	int display_clock_mode;

	/* The AFC startup override (BDB 249 on): whether there is one and its value. */
	int afc_override;
	uint8_t afc_value;

	/* Whether the board has an internal panel per the driver features. */
	int internal_panel;

	/* The outputs. */
	unsigned nchildren;
	struct i915_xd_vbt_child children[I915_XD_VBT_MAX_CHILDREN];
};

/*
 * What the VBT says about one built-in panel.
 *
 * The delays are in the VBT's 100 us units; a zero delay is one the VBT
 * leaves to the driver's defaults.
 */
struct i915_xd_vbt_panel {
	/* The panel's row in the VBT tables, and the rule that chose it. */
	unsigned panel_type;
	unsigned type_source;

	/* Whether to dither, and the refresh rate switching and VRR the panel allows. */
	int dither;
	unsigned drrs;
	int vrr;

	/* The VBT's own mode for the panel, when it has one. */
	int has_mode;
	struct i915_display_mode mode;

	/* The backlight: present (PWM), its type and controller, polarity, frequency and minimum. */
	int backlight;
	uint8_t backlight_type;
	uint8_t backlight_controller;
	int backlight_active_low;
	uint16_t backlight_hz;
	uint8_t backlight_min;
	uint8_t backlight_precision;

	/* The eDP link: bits per pixel (0: not given), rate (kHz), lanes, pre-emphasis and swing levels. */
	uint8_t bpp;
	uint32_t edp_rate_khz;
	uint8_t edp_lanes;
	uint8_t edp_preemphasis;
	uint8_t edp_vswing;

	/* The swing table to use: low swing, and the high-efficiency (HOBL) setting. */
	int low_vswing;
	int hobl;

	/* The largest eDP link rate (kHz, 0: no limit). */
	uint32_t edp_max_rate_khz;

	/* The power sequence delays: T1+T3, T8, T9, T10 and T11+T12. */
	uint16_t t1_t3;
	uint16_t t8;
	uint16_t t9;
	uint16_t t10;
	uint16_t t11_t12;
};

/* Why the raw VBT an OpRegion names was not used. */
#define I915_XD_OPREGION_RAW_USED	0U
#define I915_XD_OPREGION_RAW_ABSENT	1U
#define I915_XD_OPREGION_RAW_INSIDE	2U
#define I915_XD_OPREGION_RAW_TOO_LARGE	3U
#define I915_XD_OPREGION_RAW_UNMAPPED	4U
#define I915_XD_OPREGION_RAW_UNCHECKED	5U

/*
 * How the OpRegion reader reaches physical memory: map makes size bytes at
 * a physical address readable (0, or an errno), unmap gives them back.
 * The kernel maps device memory read-only; a host test hands out buffers.
 */
struct i915_xd_opregion_access {
	int (*map)(void *context, uint64_t address, size_t size, const volatile uint8_t **bytes);
	void (*unmap)(void *context, const volatile uint8_t *bytes, size_t size);
	void *context;
};

/*
 * What a read of the OpRegion found: whether there is one (ASLS is not
 * zero) and it could be mapped, where it keeps its VBT, where the VBT
 * was taken from (the raw VBT, mailbox 4, or nowhere), why the raw VBT
 * was not taken, and the VBT's size, check and SHA-256.
 */
struct i915_xd_opregion_read {
	int present;
	int mapped;
	int found;
	struct i915_xd_opregion_vbt where;
	unsigned source;
	unsigned raw_status;
	uint32_t vbt_size;
	int vbt_valid;
	uint8_t vbt_sha256[32];
};

int drv_i915_xd_opregion_read(const struct i915_xd_opregion_access *access, uint32_t asls, uint8_t *opregion_copy, uint8_t *vbt_copy, size_t vbt_room, struct i915_xd_opregion_read *read);
int drv_i915_xd_opregion_find_vbt(const uint8_t *opregion, size_t size, uint64_t address, struct i915_xd_opregion_vbt *where);
int drv_i915_xd_vbt_check(const uint8_t *bytes, size_t size);
int drv_i915_xd_vbt_parse(const struct i915_xd_platform *platform, const uint8_t *bytes, size_t size, struct i915_xd_vbt *vbt);
const struct i915_xd_vbt_child *drv_i915_xd_vbt_child(const struct i915_xd_vbt *vbt, unsigned ddi);
int drv_i915_xd_vbt_panel(const struct i915_xd_vbt *vbt, const struct i915_xd_vbt_child *child, const uint8_t *edid, size_t edid_size, struct i915_xd_vbt_panel *panel);
void drv_i915_xd_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_VBT_H */
