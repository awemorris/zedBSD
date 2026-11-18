/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One output of a screen of the display 12 and 13 code, lit and darkened
 * step by step (xd-output.c).
 *
 * A screen's light and dark (xd-screen.c) run their steps in a fixed
 * order; this is what each step does for an output whose description the
 * caller filled from its plan: the power the output uses, the display
 * buffer's slices and the bus's joining, the panel's backlight, the
 * pipe's DMC, the AUX power of the link, the port's PLL, the port and its
 * link (the panel's power, the DDI clock, the transcoder's function, the
 * signal levels, the sink woken and the link trained), the pipe (its
 * source, its word, the transcoder's timing and the colours), and the
 * output (the transcoder started, the backlight on).  The dark undoes
 * them in reverse: the backlight off, the transcoder stopped and its
 * function cleared, the sink put to sleep, the port stopped and its
 * clock gated, the panel's power off, the PLL, the AUX power, the pipe's
 * DMC, and the power and buffer given back last.
 *
 * Everything outside the registers is the caller's, through the
 * environment: the power references (by what the output uses them for),
 * the panel's power sequencer, the sink's DPCD, the display buffer's
 * slices, the locks, the vertical blank events of the pipe and the notes
 * of the run.  The output keeps what it holds (the references, the PLL,
 * the link's state, the backlight's levels), so a dark gives back exactly
 * what a light took, and a light that failed half way is darkened the
 * same way.
 *
 * Three kinds of output are driven: the built-in panel (eDP), an HDMI
 * or DVI sink on a combo port, and an external DisplayPort sink on a
 * Type-C port.  A Type-C port's PHY (its DisplayPort mode, its signal
 * levels, its core reset) and its own PLL are driven here through the
 * Dekel PHY's window, or on display 11 (Ice Lake) in the MG PHY's own
 * registers; what the port's connection holds (its link to the
 * PHY, the mode it is in, its lanes and pin assignment, and the
 * Thunderbolt PLL the Type-C ports share) is the caller's, through the
 * environment's Type-C hooks.
 *
 * Display 11 orders a DisplayPort port as its own generation does: the
 * transcoder's function is not written before the training and its
 * clock follows the trained link, the transport is the port's own (not
 * the transcoder's), the eDP transcoder takes its pipe in its function
 * word and has no clock select, and a stop takes the clock off the
 * transcoder before the DDI buffer stops.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_OUTPUT_H
#define DRIVERS_GPU_I915_DISPLAY_XD_OUTPUT_H

#include <stdint.h>

#include "xd-clock.h"
#include "xd-ddi.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-panel.h"
#include "xd-pipe.h"
#include "xd-platform.h"
#include "xd-tc.h"

/*
 * What an output takes power for: the exit from DC states, the display
 * core, the pipe, the transcoder, the port's lanes, the port's IO and its
 * AUX channel.  The caller names the wells that hold each.
 */
#define I915_XD_OUTPUT_POWER_DC_OFF	0U
#define I915_XD_OUTPUT_POWER_CORE	1U
#define I915_XD_OUTPUT_POWER_PIPE	2U
#define I915_XD_OUTPUT_POWER_TRANS	3U
#define I915_XD_OUTPUT_POWER_LANES	4U
#define I915_XD_OUTPUT_POWER_DDI_IO	5U
#define I915_XD_OUTPUT_POWER_AUX	6U
#define I915_XD_OUTPUT_POWER_USES	7U

/*
 * The steps of the panel's power sequencer the output asks for: the
 * panel's power on and off, its logic supply (VDD) held and let go, and
 * the backlight's enable in the sequencer.
 */
#define I915_XD_OUTPUT_PANEL_ON		0U
#define I915_XD_OUTPUT_PANEL_OFF	1U
#define I915_XD_OUTPUT_PANEL_VDD_ON	2U
#define I915_XD_OUTPUT_PANEL_VDD_OFF	3U
#define I915_XD_OUTPUT_PANEL_BACKLIGHT_ON	4U
#define I915_XD_OUTPUT_PANEL_BACKLIGHT_OFF	5U

/*
 * The points of a light and a dark the caller may look at the hardware:
 * the start (DC states held off, nothing written), the pipe's underruns
 * armed, and the end (before DC states are let back).
 */
#define I915_XD_OUTPUT_SEE_BEGIN	0U
#define I915_XD_OUTPUT_SEE_UNDERRUN	1U
#define I915_XD_OUTPUT_SEE_END		2U

/* The modes a Type-C port is held in: none, DisplayPort alternate mode, legacy (a fixed connector), Thunderbolt. */
#define I915_XD_OUTPUT_TC_NONE		0U
#define I915_XD_OUTPUT_TC_DP_ALT	1U
#define I915_XD_OUTPUT_TC_LEGACY	2U
#define I915_XD_OUTPUT_TC_TBT		3U

/* The locks the output takes around a shared resource: the PLLs' and the backlight's. */
#define I915_XD_OUTPUT_LOCK_PLL		0U
#define I915_XD_OUTPUT_LOCK_BACKLIGHT	1U

/*
 * The caller's parts of a Type-C port: what its connection holds.
 *
 * get_link takes the port's link (and with the first link the PHY) for
 * a link of so many lanes, put_link gives it back; mode reports the
 * mode the port is held in (I915_XD_OUTPUT_TC_*); lanes sets the lanes
 * the PHY's flexible IO gives the display; pin reports the DisplayPort
 * pin assignment the connection recorded (I915_XD_TC_PIN_*); and
 * tbt_pll turns the Thunderbolt PLL the Type-C ports share on or off
 * for the output's pipe (0 or an error).  Each takes the environment's
 * context.
 */
struct i915_xd_output_tc {
	void (*get_link)(void *context, unsigned lanes);
	void (*put_link)(void *context);
	unsigned (*mode)(void *context);
	void (*lanes)(void *context, unsigned lanes);
	unsigned (*pin)(void *context);
	int (*tbt_pll)(void *context, int on);
};

/*
 * What the caller lets the output reach.
 *
 * power_get takes a reference for one use of the output (the index is
 * the pipe, the transcoder, the DDI or the AUX channel the use names)
 * and returns a nonzero cookie, or 0 when it could not; power_put gives
 * it back, and power_put_later after a delay in milliseconds.  panel
 * runs one step of the power sequencer (0, or an error).  sink reaches
 * the sink's DPCD over the port's AUX channel.  dbuf_slices asks for
 * exactly these display buffer slices (bit n for slice n + 1).  lock
 * takes (taken nonzero) or drops one of the output's locks.  vblank_off
 * settles the pipe's pending vertical blank event before its transcoder
 * stops.  link_failed hears that the link did not train (the caller may
 * ask for a lower link).  converter sets up a branch's protocol
 * converter after the sink woke (NULL: none).  tc holds the Type-C
 * port's hooks (NULL for a combo port), and dkl is the Dekel PHYs'
 * window a Type-C port's PHY and PLL are reached through.  observe marks a point of the
 * run.  error and debug take the run's notes; error counts against the
 * run.
 */
struct i915_xd_output_env {
	struct i915_xd_io *io;
	const struct i915_xd_platform *platform;
	int (*power_get)(void *context, unsigned use, unsigned index);
	void (*power_put)(void *context, unsigned use, unsigned index, int cookie);
	void (*power_put_later)(void *context, unsigned use, unsigned index, int cookie, int delay_ms);
	int (*panel)(void *context, unsigned step);
	struct i915_xd_dp_sink sink;
	void (*dbuf_slices)(void *context, uint8_t slices);
	void (*lock)(void *context, unsigned lock, int taken);
	void (*vblank_off)(void *context);
	void (*link_failed)(void *context);
	void (*converter)(void *context);
	const struct i915_xd_output_tc *tc;
	struct i915_xd_dkl *dkl;
	void (*observe)(void *context, unsigned point);
	void (*error)(void *context, const char *text);
	void (*debug)(void *context, const char *text);
	void *context;
};

/*
 * The display buffer around one light or dark: the slices on, whether
 * the bus is joined and the pipes that run, before and after it.
 */
struct i915_xd_output_dbuf {
	uint8_t slices_before;
	uint8_t slices_after;
	uint8_t joined_before;
	uint8_t joined_after;
	uint8_t pipes_before;
	uint8_t pipes_after;
};

/*
 * The panel's backlight: what the VBT says of it (whether it has one,
 * its controller, its PWM frequency (0: the default) and lowest
 * brightness of 255), the raw clock the PWM counts, how many controllers
 * the south display has and whether the second shares the first's pins,
 * and whether the duty is inverted for this panel; then what the setup
 * and the run keep: the PWM as found, the range of levels (the PWM's
 * period and the lowest duty), the level, and whether the backlight is
 * on.
 */
struct i915_xd_output_backlight {
	int vbt_present;
	unsigned vbt_controller;
	unsigned vbt_hz;
	unsigned vbt_min;
	unsigned rawclk_khz;
	unsigned controllers;
	int muxed;
	int inverted;
	struct i915_xd_backlight pwm;
	int present;
	uint32_t min;
	uint32_t max;
	uint32_t level;
	int enabled;
};

/*
 * One output: what the caller described before the light, and what the
 * light and the dark keep.
 *
 * The description: the output's kind (I915_XD_SCREEN_*), its DDI and the
 * PHY behind it, its AUX channel, the pipe and the transcoder; the
 * transcoder's timing and sync polarities, the pipe's source size, the bits per pixel,
 * dither, a limited colour range, the frame start delay and the pixel
 * multiplier; the link (rate, lanes, enhanced framing, M/N, the eDP rate
 * table's index or -1, the DDI bits the port keeps), the VBT's choice of
 * swing table (I915_XD_SWING_*), the HDMI swing level the VBT names (-1:
 * the table's own), whether the port is Type-C (and which, 0 for the
 * first); the port's PLL and its words (a combo port's), and a Type-C
 * port's own PLL and its words (a Dekel PHY's, or on display 11 an MG
 * PHY's); the
 * colour words, the line time word; the DMC programs loaded (bit n for
 * DMC n); the sink's receiver capabilities and the display buffer's
 * change.
 *
 * The run: whether the transcoder runs, the PLL is on (and its pipes),
 * the references held (a bit per use, with their cookies), the DDI
 * buffer's word, the link's levels and whether it trained, whether the
 * swing table was the high-efficiency one (and failed with it), the
 * mode a Type-C port is held in, and whether the port's link to it is
 * held.
 */
struct i915_xd_output {
	unsigned kind;
	unsigned ddi;
	unsigned phy;
	unsigned aux;
	unsigned pipe;
	unsigned trans;
	struct i915_xd_timing timing;
	int hsync_high;
	int vsync_high;
	uint32_t src_width;
	uint32_t src_height;
	unsigned bpp;
	int dither;
	int limited_range;
	unsigned frame_start_delay;
	unsigned pixel_multiplier;
	uint32_t rate_khz;
	unsigned lanes;
	int enhanced;
	struct i915_xd_dp_m_n m_n;
	int rate_index;
	uint32_t kept;
	unsigned swing_vbt;
	int hdmi_level;
	int tc;
	unsigned tc_port;
	unsigned pll;
	struct i915_xd_dpll_words pll_words;
	unsigned tc_pll;
	struct i915_xd_tc_pll_words tc_pll_words;
	struct i915_xd_mg_pll_words mg_pll_words;
	uint32_t gamma_mode;
	uint32_t csc_mode;
	uint32_t linetime;
	uint32_t dmc_programs;
	uint8_t caps[15];
	uint8_t repeater_caps[8];
	uint8_t repeater_phy_caps[8][3];
	struct i915_xd_output_dbuf dbuf;
	struct i915_xd_output_backlight backlight;
	int lit;
	int pll_on;
	uint8_t pll_pipes;
	unsigned held;
	int cookie[I915_XD_OUTPUT_POWER_USES];
	unsigned put_later;
	uint32_t buf_word;
	uint8_t set[4];
	int trained;
	int hobl_active;
	int hobl_failed;
	unsigned tc_mode;
	int tc_linked;
};

int drv_i915_xd_output_dc_hold(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_power(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_dark_power(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_dbuf(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_backlight_setup(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_dmc(struct i915_xd_output *out, const struct i915_xd_output_env *env, int on);
int drv_i915_xd_output_link_power(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_pll(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_port(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_pipe(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_start(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_settle(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_stop(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_port_off(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_pll_off(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_link_power_off(struct i915_xd_output *out, const struct i915_xd_output_env *env);
int drv_i915_xd_output_dark_settle(struct i915_xd_output *out, const struct i915_xd_output_env *env);
void drv_i915_xd_output_dc_allow(struct i915_xd_output *out, const struct i915_xd_output_env *env);
void drv_i915_xd_output_backlight_on(struct i915_xd_output *out, const struct i915_xd_output_env *env);
void drv_i915_xd_output_backlight_off(struct i915_xd_output *out, const struct i915_xd_output_env *env);
void drv_i915_xd_output_brightness(struct i915_xd_output *out, const struct i915_xd_output_env *env, uint32_t user_level, uint32_t user_max);
void drv_i915_xd_output_brightness_acpi(struct i915_xd_output *out, const struct i915_xd_output_env *env, uint32_t user_level, uint32_t user_max);
uint32_t drv_i915_xd_output_user_level(const struct i915_xd_output *out, uint32_t user_max);

/*
 * One display 9 external output to light: whether it is HDMI, its DDI,
 * its pipe (whose transcoder has the same number) and DPLL with the
 * DPLL's words, the mode, a DisplayPort link's rate, lanes and M/N, an
 * HDMI port's translation row, and the plane's first surface (a GGTT
 * offset), its stride in 64-byte units and whether its pixels are R, G,
 * B, X.  The caller keeps it for as long as the output is lit.
 */
struct i915_xd9_output {
	int hdmi;
	unsigned ddi;
	unsigned pipe;
	unsigned dpll;
	struct i915_xd9_dpll_words dpll_words;
	struct i915_display_mode mode;
	uint32_t rate_khz;
	unsigned lanes;
	struct i915_xd_dp_m_n m_n;
	const struct i915_xd9_buf_trans *hdmi_row;
	uint32_t surf;
	uint32_t stride_units;
	int rgbx;
};

int drv_i915_xd9_output_on(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd9_output *out, struct i915_xd9_dp_port *port);
void drv_i915_xd9_output_off(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd9_output *out, struct i915_xd9_dp_port *port);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_OUTPUT_H */
