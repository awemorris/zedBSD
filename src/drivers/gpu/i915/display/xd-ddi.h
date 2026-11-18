/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DDIs of the display 12 and 13 code (xd-ddi.c): the combo PHYs and
 * the DDI buffers.
 *
 * A combo PHY drives the lanes of one DDI.  It is brought up once, at the
 * display's start (and again after DC states that lose it): its IO
 * compensation is powered, its duty cycle corrector set to run once, its
 * process and voltage reference values written, the compensation started
 * (on the PHYs that own the compensation resistor the reference current
 * first) and its common lane allowed to power down.  A PHY whose state
 * already reads as brought up, as the firmware leaves PHY A, is not
 * written.  Before a suspend every PHY's compensation is stopped.
 *
 * A port powers up only the lanes its link uses.  Its signal levels come
 * from the platform's swing table of its kind (DisplayPort by rate, eDP by
 * rate and by the VBT's low swing or HOBL setting, HDMI): each lane takes
 * the row of the level the sink asked for, with the load generator of the
 * lane, and the change is latched by the training enable.
 *
 * The DDI buffer drives the link.  For DisplayPort its word carries the
 * lane count and lane reversal (and on a display 13 Type-C port the PHY
 * link rate); the link starts with the transport in training pattern 1
 * and the buffer enabled, then waits for the buffer to leave idle.  On a
 * Type-C port the PHY ownership bit belongs to the Type-C code: the
 * buffer keeps whatever it holds.  A link that is already running is
 * stopped first.  The stop clears the buffer, the transport and FEC, and
 * waits for the buffer to go idle.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_DDI_H
#define DRIVERS_GPU_I915_DISPLAY_XD_DDI_H

#include <stdint.h>

#include "xd-aux.h"
#include "xd-io.h"
#include "xd-platform.h"

/* The kinds of output a swing table serves. */
#define I915_XD_OUTPUT_DP		0U
#define I915_XD_OUTPUT_EDP		1U
#define I915_XD_OUTPUT_HDMI		2U

/* The VBT's eDP settings that choose a swing table: the low swing, and HOBL. */
#define I915_XD_SWING_LOW		0x1U
#define I915_XD_SWING_HOBL		0x2U

/*
 * The signal levels of one combo port: its PHY, its kind, the link rate
 * and lane count, the swing table, and the table row of each lane.
 */
struct i915_xd_combo_levels {
	unsigned phy;
	unsigned kind;
	uint32_t rate_khz;
	unsigned lanes;
	const struct i915_xd_combo_swing_table *table;
	uint8_t level[4];
};

/* The waits of the DDI buffer and the transport that ran out, as bits. */
#define I915_XD_DDI_LATE_IDLE		0x1U
#define I915_XD_DDI_LATE_ACTIVE		0x2U
#define I915_XD_DDI_LATE_IDLE_PATTERNS	0x4U

/*
 * A DDI link: the DDI, the transcoder whose transport it uses, whether
 * its PHY is a Type-C one, the lane count and link rate, the bits the
 * port keeps in its buffer word (the lane reversal, DDI A's four lanes),
 * and for DisplayPort the enhanced framing, multi-stream and FEC.
 */
struct i915_xd_ddi_link {
	unsigned ddi;
	unsigned trans;
	int tc;
	unsigned lanes;
	uint32_t rate_khz;
	uint32_t kept;
	int enhanced;
	int mst;
	int fec;
};

/* The kinds of display 9 output a DDI buffer table is chosen for. */
#define I915_XD9_OUTPUT_DP		1U
#define I915_XD9_OUTPUT_HDMI		2U
#define I915_XD9_OUTPUT_EDP		3U

/*
 * A display 9 DisplayPort port while its link trains and runs: the
 * registers and the DDI's AUX channel, the DDI and its translation table,
 * the buffer's word kept between the steps (its width and kept bits; the
 * row and the enable are added), the level the training left, and the
 * sink's receiver and repeater capabilities.  The caller owns it for as
 * long as the link is used.
 */
struct i915_xd9_dp_port {
	struct i915_xd_io *io;
	struct i915_xd_aux aux;
	unsigned ddi;
	const struct i915_xd9_buf_trans *table;
	unsigned rows;
	uint32_t buf_word;
	uint8_t set;
	uint8_t caps[15];
	uint8_t repeater_caps[8];
	uint8_t repeater_phy_caps[8][3];
};

void drv_i915_xd9_ddi_buffers(struct i915_xd_io *io, unsigned ddi, const struct i915_xd9_buf_trans *table, unsigned rows);
void drv_i915_xd9_ddi_levels(struct i915_xd_io *io, unsigned ddi, const struct i915_xd9_buf_trans *table, unsigned rows, unsigned row, uint32_t *buf_word);
void drv_i915_xd9_ddi_buffer_off(struct i915_xd_io *io, unsigned ddi);
void drv_i915_xd9_ddi_boost(struct i915_xd_io *io, unsigned ddi, unsigned boost);
void drv_i915_xd9_ddi_hdmi_start(struct i915_xd_io *io, unsigned ddi, const struct i915_xd9_buf_trans *row);
int drv_i915_xd9_dp_train(struct i915_xd9_dp_port *port, uint32_t rate_khz, unsigned lanes);
int drv_i915_xd9_dp_sink_power(struct i915_xd9_dp_port *port, uint8_t state);
const struct i915_xd9_buf_trans *drv_i915_xd9_ddi_buf_table(const struct i915_xd_platform *platform, unsigned output, unsigned ddi, unsigned *count);
unsigned drv_i915_xd9_ddi_hdmi_row(const struct i915_xd_platform *platform);
int drv_i915_xd_combo_phy_ready(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned phy);
unsigned drv_i915_xd_combo_phy_init(struct i915_xd_io *io, const struct i915_xd_platform *platform);
unsigned drv_i915_xd_combo_phy_uninit(struct i915_xd_io *io, const struct i915_xd_platform *platform);
void drv_i915_xd_combo_lanes(struct i915_xd_io *io, unsigned phy, unsigned lanes, int reversed);
const struct i915_xd_combo_swing_table *drv_i915_xd_combo_swing_table(const struct i915_xd_platform *platform, unsigned kind, uint32_t rate_khz, unsigned vbt);
int drv_i915_xd_combo_swing_is_hobl(const struct i915_xd_platform *platform, const struct i915_xd_combo_swing_table *table);
int drv_i915_xd_combo_level_row(uint8_t level, unsigned *row);
uint8_t drv_i915_xd_dp_swing_max(unsigned rows);
void drv_i915_xd_combo_signal_levels(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_combo_levels *levels);
uint32_t drv_i915_xd_ddi_dp_word(const struct i915_xd_platform *platform, const struct i915_xd_ddi_link *link);
unsigned drv_i915_xd_ddi_dp_start(struct i915_xd_io *io, const struct i915_xd_ddi_link *link, uint32_t word, void (*before_buffer)(void *context), void *context);
void drv_i915_xd_ddi_pattern(struct i915_xd_io *io, unsigned trans, unsigned pattern);
unsigned drv_i915_xd_ddi_idle_patterns(struct i915_xd_io *io, unsigned trans);
unsigned drv_i915_xd_ddi_hdmi_start(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_ddi_link *link);
unsigned drv_i915_xd_ddi_stop(struct i915_xd_io *io, const struct i915_xd_ddi_link *link, int dp);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_DDI_H */
