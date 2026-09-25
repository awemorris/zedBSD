/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_ddi.c),
 * which carries the following notice.
 *
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eugeni Dodonov <eugeni.dodonov@intel.com>
 */

/*
 * The DDI port of the one-screen modeset path (see ddi.h).
 *
 * The functions follow the Linux 6.8.12 intel_ddi.c text for a combo-PHY
 * DDI on display version 12 and later: the transcoder function words, the
 * DP and HDMI enable and disable of the port (the encoder hooks the crtc
 * enable and disable reach through the drv_i915_encoders_*() walks), the
 * port clock and the power references the port holds, the voltage swing
 * programming of the combo PHY, the DP link-training hooks of the port,
 * and the readout of a port the firmware left running.
 *
 * The encoder hooks have the Linux signatures, which carry no world.  The
 * Linux text reached two things through file-scope pointers: the device
 * of the current entry point (for its sleeps, waits, locks, panel
 * operations, DPCD accesses and named steps) and the modeset object the
 * DDI hooks are bound to.  Every encoder these hooks are bound to is the
 * encoder of a modeset object, so both are found from the encoder: its
 * modeset object's world records the entry point's device
 * (i915_lcd_cur_i915) and the bound object (ddi_ms).  Where the Linux
 * text used the device of the object itself (the register accesses and
 * the power references), that device is used.
 *
 * Many macros of the environment answer for the supported configuration
 * without evaluating the device or object the Linux text hands them (the
 * platform predicates, the warnings, the guards of unported callees).  A
 * local that only such macros name is declared __maybe_unused; the Linux
 * text is kept as it was.
 */

#include "internal.h"
#include "modeset-internal.h"
#include "takeover-internal.h"
#include "../intel/trans.h"
#include "../intel/dp.h"
#include "ddi.h"
#include "dp.h"
#include "hdmi-mode.h"
#include "pipe.h"
#include <kern/kcrt.h>

/*
 * ==== Types ====
 */

/*
 * One encoder hook of the enable and disable sequences, as struct
 * intel_encoder carries it (pre_pll_enable, pre_enable, enable, disable,
 * post_disable, post_pll_disable).
 */
typedef void (*i915_ddi_encoder_hook_t)(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);

/*
 * ==== File-scope constants ====
 */

/*
 * The DP voltage swing and pre-emphasis pairs, in the order of the entries
 * of a buffer-translation table (the Linux index_to_dp_signal_levels[]).
 *
 * A signal level the sink requests is translated to the table entry of the
 * same index; the number of entries of the port's table bounds the swing
 * the port reports as its maximum.  Constant for the life of the kernel.
 */
static const u8 index_to_dp_signal_levels[] = {
	[0] = DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_0,
	[1] = DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_1,
	[2] = DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_2,
	[3] = DP_TRAIN_VOLTAGE_SWING_LEVEL_0 | DP_TRAIN_PRE_EMPH_LEVEL_3,
	[4] = DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_0,
	[5] = DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_1,
	[6] = DP_TRAIN_VOLTAGE_SWING_LEVEL_1 | DP_TRAIN_PRE_EMPH_LEVEL_2,
	[7] = DP_TRAIN_VOLTAGE_SWING_LEVEL_2 | DP_TRAIN_PRE_EMPH_LEVEL_0,
	[8] = DP_TRAIN_VOLTAGE_SWING_LEVEL_2 | DP_TRAIN_PRE_EMPH_LEVEL_1,
	[9] = DP_TRAIN_VOLTAGE_SWING_LEVEL_3 | DP_TRAIN_PRE_EMPH_LEVEL_0,
};

/*
 * ==== Forward declarations ====
 */

static struct i915_lcd_modeset *i915_ddi_modeset(const struct intel_encoder *encoder);
static struct drm_i915_private *i915_ddi_cur_i915(const struct intel_encoder *encoder);
static struct i915_lcd_modeset *i915_encoders_bound(struct intel_atomic_state *state);
static void i915_encoders_call(struct i915_lcd_modeset *ms, i915_ddi_encoder_hook_t hook, struct intel_atomic_state *state, const struct intel_crtc_state *crtc_state);
static u32 i915_ddi_buf_phy_link_rate(int port_clock);
static void i915_ddi_init_dp_buf_reg(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_set_dp_msa(const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static u32 i915_bdw_trans_port_sync_master_select(enum transcoder master_transcoder);
static u32 i915_ddi_transcoder_func_reg_val_get(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_enable_transcoder_func(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_config_transcoder_func(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_tgl_ddi_pre_enable_dp(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_ddi_pre_enable_dp(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_ddi_pre_enable(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_enable_ddi_dp(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_enable_ddi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_ddi_pre_pll_enable(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static u8 i915_ddi_dp_voltage_max(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static u8 i915_ddi_dp_preemph_max(struct intel_dp *intel_dp);
static void i915_ddi_enable_clock(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_disable_clock(struct intel_encoder *encoder);
static void i915_icl_ddi_enable_clock_reg(struct drm_i915_private *cur_i915, struct drm_i915_private *i915, i915_reg_t reg, u32 clk_sel_mask, u32 clk_sel, u32 clk_off);
static void i915_icl_ddi_disable_clock_reg(struct drm_i915_private *cur_i915, struct drm_i915_private *i915, i915_reg_t reg, u32 clk_off);
static void i915_icl_ddi_combo_enable_clock(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_icl_ddi_combo_disable_clock(struct intel_encoder *encoder);
static enum intel_display_power_domain i915_ddi_main_link_aux_domain(struct intel_digital_port *dig_port, const struct intel_crtc_state *crtc_state);
static void i915_main_link_aux_power_domain_get(struct intel_digital_port *dig_port, const struct intel_crtc_state *crtc_state);
static void i915_main_link_aux_power_domain_put(struct intel_digital_port *dig_port, const struct intel_crtc_state *crtc_state);
static void i915_ddi_enable_transcoder_clock(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_disable_transcoder_clock(const struct intel_crtc_state *crtc_state);
static int i915_ddi_dp_level(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, int lane);
static int i915_ddi_level(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, int lane);
static u32 i915_icl_combo_phy_loadgen_select(const struct intel_crtc_state *crtc_state, int lane);
static void i915_icl_ddi_combo_vswing_program(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_icl_combo_phy_set_signal_levels(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static int i915_translate_signal_level(struct intel_dp *intel_dp, u8 signal_levels);
static void i915_ddi_power_up_lanes(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_mso_configure(const struct intel_crtc_state *crtc_state);
static enum transcoder i915_tgl_dp_tp_transcoder(const struct intel_crtc_state *crtc_state);
static i915_reg_t i915_dp_tp_ctl_reg(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static i915_reg_t i915_dp_tp_status_reg(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_wait_ddi_buf_idle(struct drm_i915_private *cur_i915, struct drm_i915_private *dev_priv, enum port port);
static void i915_wait_ddi_buf_active(struct drm_i915_private *cur_i915, struct drm_i915_private *dev_priv, enum port port);
static void i915_ddi_prepare_link_retrain(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static void i915_ddi_set_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, u8 dp_train_pat);
static void i915_ddi_set_idle_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static void i915_ddi_disable_fec(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_disable_ddi_buf_ctl(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_disable_ddi_buf(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_disable_transcoder_func(struct drm_i915_private *cur_i915, const struct intel_crtc_state *crtc_state);
static void i915_dp_sink_set_msa_timing_par_ignore_state(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, bool enable);
static void i915_disable_ddi_dp(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_disable_ddi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_ddi_post_disable_dp(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_ddi_post_disable(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_ddi_post_pll_disable(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static int i915_icl_ddi_min_voltage_level(const struct intel_crtc_state *crtc_state);
static int i915_jsl_ddi_min_voltage_level(const struct intel_crtc_state *crtc_state);
static int i915_tgl_ddi_min_voltage_level(const struct intel_crtc_state *crtc_state);
static int i915_ddi_hdmi_level(struct intel_encoder *encoder, const struct intel_ddi_buf_trans *trans);
static void i915_ddi_pre_enable_hdmi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_enable_ddi_hdmi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state, const struct drm_connector_state *conn_state);
static void i915_disable_ddi_hdmi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_ddi_post_disable_hdmi(struct intel_atomic_state *state, struct intel_encoder *encoder, const struct intel_crtc_state *old_crtc_state, const struct drm_connector_state *old_conn_state);
static void i915_ddi_get_encoder_pipes(struct intel_encoder *encoder, u8 *pipe_mask, bool *is_dp_mst);
static void i915_ddi_read_encoder_pipes(struct intel_encoder *encoder, u8 *pipe_mask, bool *is_dp_mst);
static bool i915_ddi_get_hw_state(struct intel_encoder *encoder, enum pipe *pipe);
static void i915_ddi_read_func_ctl(struct intel_encoder *encoder, struct intel_crtc_state *pipe_config);
static void i915_ddi_dotclock_get(struct intel_crtc_state *pipe_config);
static void i915_ddi_get_config(struct intel_encoder *encoder, struct intel_crtc_state *pipe_config);
static void i915_ddi_get_clock(struct intel_encoder *encoder, struct intel_crtc_state *crtc_state, struct intel_shared_dpll *pll);
static struct intel_shared_dpll *i915_icl_ddi_get_pll_reg(struct drm_i915_private *i915, i915_reg_t reg, u32 clk_sel_mask, u32 clk_sel_shift);
static struct intel_shared_dpll *i915_icl_ddi_combo_get_pll(struct intel_encoder *encoder);
static void i915_icl_ddi_combo_get_config(struct intel_encoder *encoder, struct intel_crtc_state *crtc_state) __maybe_unused;
static void i915_ddi_sync_state(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
static void i915_ddi_get_power_domains(struct intel_encoder *encoder, struct intel_crtc_state *crtc_state);
static bool i915_icl_ddi_is_clock_enabled_reg(struct drm_i915_private *i915, i915_reg_t reg, u32 clk_off);
static bool i915_icl_ddi_combo_is_clock_enabled(struct intel_encoder *encoder);
static bool i915_ddi_connector_read_hw_state(struct intel_connector *intel_connector, struct intel_encoder *encoder);

/*
 * ==== Public functions ====
 */

/*
 * Selects the chicken register of a transcoder.
 *
 * The Linux hsw_chicken_trans_reg(): display version 14 moved the
 * transcoder chicken registers.
 */
i915_reg_t
drv_i915_hsw_chicken_trans_reg(
	struct drm_i915_private *i915,
	enum transcoder cpu_transcoder)
{
	int display_ver;

	UNUSED_PARAMETER(i915);

	/* The display version of the device the probe found. */
	display_ver = I915_LCD_DISPLAY_VER(i915);

	/* Display version 14 and later have the chicken registers elsewhere. */
	if (display_ver >= 14)
		return MTL_CHICKEN_TRANS(cpu_transcoder);

	/* Succeeded: reports the register of the earlier versions. */
	return CHICKEN_TRANS(cpu_transcoder);
}

/*
 * Computes the lowest voltage level the port clock of a crtc state needs.
 *
 * The Linux intel_ddi_compute_min_voltage_level(): the result is stored in
 * the state (min_voltage_level); a version without a rule leaves it alone.
 */
void
drv_i915_ddi_compute_min_voltage_level(
	struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	int display_ver;

	/* Finds the device of the crtc and its display version. */
	dev_priv = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* Takes the rule of the platform's display version. */
	if (display_ver >= 14) {
		crtc_state->min_voltage_level = i915_icl_ddi_min_voltage_level(crtc_state);
	} else if (display_ver >= 12) {
		crtc_state->min_voltage_level = i915_tgl_ddi_min_voltage_level(crtc_state);
	} else if (IS_JASPERLAKE(dev_priv) || IS_ELKHARTLAKE(dev_priv)) {
		crtc_state->min_voltage_level = i915_jsl_ddi_min_voltage_level(crtc_state);
	} else if (display_ver >= 11) {
		crtc_state->min_voltage_level = i915_icl_ddi_min_voltage_level(crtc_state);
	}
}

/*
 * Tells whether a connector's port drives it in the mode its type needs.
 *
 * The Linux intel_ddi_connector_get_hw_state(): the port's encoder must be
 * active on a pipe, and the transcoder of that pipe must be in HDMI / DVI
 * mode for an HDMI connector, SST mode for a DP or eDP connector, or FDI
 * mode for a VGA connector.  The port's power domain is only read while it
 * is already on.
 */
bool
drv_i915_ddi_connector_get_hw_state(
	struct intel_connector *intel_connector)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	struct intel_encoder *encoder;
	intel_wakeref_t wakeref;
	bool connected;

	/* Finds the device and the encoder the connector is attached to. */
	dev = intel_connector->base.dev;
	dev_priv = i915_lcd_to_i915(dev);
	encoder = i915_takeover_intel_attached_encoder(intel_connector);
	cur_i915 = i915_ddi_cur_i915(encoder);

	/* Reads nothing while the port's power well is off. */
	wakeref = I915_TAKEOVER_INTEL_DISPLAY_POWER_GET_IF_ENABLED(cur_i915, encoder->power_domain);
	if (!wakeref)
		return false;

	/* Reads the mode of the transcoder the port drives. */
	connected = i915_ddi_connector_read_hw_state(intel_connector, encoder);

	/* Gives the port's power reference back. */
	i915_lcd_intel_display_power_put(dev_priv, encoder->power_domain, wakeref);

	/* Succeeded: reports whether the connector is driven. */
	return connected;
}

/*
 * Gates the DDI clock of a port no crtc uses.
 *
 * The Linux intel_ddi_sanitize_encoder_pll_mapping(): a port the firmware
 * left without a crtc but with its DDI clock ungated has the clock gated
 * again.  The takeover registry is the device's encoder list, walked only
 * for a DSI encoder.
 */
void
drv_i915_ddi_sanitize_encoder_pll_mapping(
	struct i915_takeover_world *takeover,
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915 __maybe_unused;
	struct drm_i915_private *cur_i915;
	struct intel_encoder *other_encoder;
	unsigned encoder_index;
	u32 port_mask;
	bool ddi_clk_needed;
	u8 pipe_mask;
	bool is_mst;
	bool warned;
	bool clock_enabled;

	/* Finds the device of the encoder and the device of the entry point. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);

	/*
	 * In case of DP MST, we sanitize the primary encoder only, not the
	 * virtual ones.
	 */
	if (encoder->type == INTEL_OUTPUT_DP_MST)
		return;

	/* A DP port without a crtc is asked which pipes it drives. */
	if (!encoder->base.crtc && intel_encoder_is_dp(encoder)) {
		i915_ddi_get_encoder_pipes(encoder, &pipe_mask, &is_mst);

		/*
		 * In the unlikely case that BIOS enables DP in MST mode, just
		 * warn since our MST HW readout is incomplete.
		 */
		warned = I915_LCD_DRM_WARN_ON(&i915->drm, is_mst);
		if (warned)
			return;
	}

	/* The port's own bit, and whether a crtc still needs its clock. */
	port_mask = BIT(encoder->port);
	ddi_clk_needed = encoder->base.crtc;

	/* A DSI encoder spans several ports and keeps its clocks gated. */
	if (encoder->type == INTEL_OUTPUT_DSI) {
		port_mask = I915_TAKEOVER_INTEL_DSI_ENCODER_PORTS(cur_i915, encoder);

		/*
		 * Sanity check that we haven't incorrectly registered another
		 * encoder using any of the ports of this DSI encoder.
		 */
		I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, other_encoder, encoder_index) {
			if (other_encoder == encoder)
				continue;

			warned = I915_LCD_DRM_WARN_ON(&i915->drm,
						      port_mask & BIT(other_encoder->port));
			if (warned)
				return;
		}

		/*
		 * For DSI we keep the ddi clocks gated
		 * except during enable/disable sequence.
		 */
		ddi_clk_needed = false;
	}

	/* A clock a crtc needs, or a port that cannot tell, is left alone. */
	if (ddi_clk_needed || !encoder->is_clock_enabled)
		return;

	/* Asks the port whether its clock is ungated. */
	clock_enabled = encoder->is_clock_enabled(encoder);
	if (!clock_enabled)
		return;

	/* Reports the port whose clock is gated now. */
	I915_TAKEOVER_DRM_NOTICE(&i915->drm,
				 "[ENCODER:%d:%s] is disabled/in DSI mode with an ungated DDI clock, gate it\n",
				 encoder->base.base.id,
				 encoder->base.name);

	/* Gates the port's clock. */
	encoder->disable_clock(encoder);
}

/*
 * Records the DDI words of a DP output without a modeset object.
 *
 * Builds the device, crtc state, encoder and digital port the Linux DDI
 * text reads and runs it on the given backend (normally a word recorder):
 * intel_ddi_init_dp_buf_reg() yields the DDI_BUF_CTL value (reported in
 * *ddi_buf_ctl_value, not written), intel_ddi_set_dp_msa() writes
 * TRANS_MSA_MISC and intel_ddi_enable_transcoder_func() writes
 * TRANS_DDI_FUNC_CTL2 and TRANS_DDI_FUNC_CTL.  saved_port_bits is the
 * caller's: intel_ddi_init() takes it from the DDI_BUF_CTL readout masked
 * with DDI_BUF_PORT_REVERSAL plus the VBT's lane-reversal flag.  The
 * device and the crtc state are the world's recorder objects, cleared at
 * each call.  Always succeeds (0).
 */
int
drv_i915_ddi_emit(
	struct i915_lcd_world *world,
	struct i915_lcd_emit *emit,
	const struct drm_display_mode *mode,
	int port,
	int pipe,
	int cpu_transcoder,
	int port_clock,
	int lanes,
	int pipe_bpp,
	u32 saved_port_bits,
	u32 *ddi_buf_ctl_value)
{
	struct drm_i915_private *i915;
	struct intel_crtc_state *crtc_state;
	struct intel_crtc crtc;
	struct intel_digital_port dig_port;
	struct drm_connector_state conn_state;

	/* Clears the recorder's device and crtc state and the local objects. */
	i915 = &world->i915_ddi_emit_i915;
	crtc_state = &world->i915_ddi_emit_crtc_state;
	kern_memset(i915, 0, sizeof(*i915));
	kern_memset(&crtc, 0, sizeof(crtc));
	kern_memset(crtc_state, 0, sizeof(*crtc_state));
	kern_memset(&dig_port, 0, sizeof(dig_port));
	kern_memset(&conn_state, 0, sizeof(conn_state));

	/* Binds the device to the backend and the crtc to the device. */
	i915->emit = emit;
	crtc.base.dev = &i915->drm;
	crtc.pipe = (enum pipe)pipe;

	/* Fills the crtc state of an eDP modeset of the given mode and link. */
	crtc_state->uapi.crtc = &crtc.base;
	crtc_state->uapi.mode_changed = true;
	crtc_state->cpu_transcoder = cpu_transcoder;
	crtc_state->master_transcoder = INVALID_TRANSCODER;
	crtc_state->mst_master_transcoder = INVALID_TRANSCODER;
	crtc_state->hw.adjusted_mode = *mode;
	crtc_state->output_types = BIT(INTEL_OUTPUT_EDP);
	crtc_state->output_format = INTEL_OUTPUT_FORMAT_RGB;
	crtc_state->port_clock = port_clock;
	crtc_state->lane_count = lanes;
	crtc_state->pipe_bpp = pipe_bpp;

	/* Fills the port the words describe. */
	dig_port.base.base.dev = &i915->drm;
	dig_port.base.port = (enum port)port;
	dig_port.saved_port_bits = saved_port_bits;
	conn_state.colorspace = MODE_COLORIMETRY_DEFAULT;

	/* Computes the DDI_BUF_CTL value and hands it to the caller. */
	i915_ddi_init_dp_buf_reg(&dig_port.base, crtc_state);
	*ddi_buf_ctl_value = dig_port.dp.DP;

	/* Writes TRANS_MSA_MISC. */
	i915_ddi_set_dp_msa(crtc_state, &conn_state);

	/* Writes TRANS_DDI_FUNC_CTL2 and TRANS_DDI_FUNC_CTL. */
	i915_ddi_enable_transcoder_func(&dig_port.base, crtc_state);

	/* Succeeded: the words are recorded. */
	return 0;
}

/*
 * Reports the VBT's HDMI level shift of the port the DDI hooks are bound to.
 *
 * The Linux intel_bios_hdmi_level_shift(): the VBT child's value as the
 * parser read it.  0 is a valid index; only a VBT without the field, or no
 * bound modeset object, gives -1, and intel_ddi_hdmi_level() then falls
 * back to the buffer-translation table's own default entry.
 */
int
drv_i915_lcd_hdmi_level_shift(
	struct i915_lcd_world *world)
{
	/* No modeset object is bound. */
	if (world->ddi_ms == NULL)
		return -1;

	/* Succeeded: reports the bound object's value. */
	return world->ddi_ms->hdmi_level_shift;
}

/*
 * Reports the port the DDI hooks are bound to, for diagnostics.
 *
 * -1 when no modeset object is bound.
 */
int
drv_i915_lcd_ms_bound_port(
	struct i915_lcd_world *world)
{
	/* No modeset object is bound. */
	if (world->ddi_ms == NULL)
		return -1;

	/* Succeeded: reports the bound object's port. */
	return (int)world->ddi_ms->dig_port.base.port;
}

/*
 * Binds the readout hooks of an encoder.
 *
 * intel_ddi_init() binds these in Linux; they are static in this file, so
 * the takeover readout asks for them here.  Without them the readout of an
 * active encoder would call a NULL get_config.
 */
void
drv_i915_lcd_ms_bind_readout(
	struct intel_encoder *encoder)
{
	/* Binds the hooks the readout reaches through the encoder. */
	encoder->get_hw_state = i915_ddi_get_hw_state;
	encoder->get_config = i915_ddi_get_config;
	encoder->sync_state = i915_ddi_sync_state;
	encoder->get_power_domains = i915_ddi_get_power_domains;
}

/*
 * Reports the encoder of the modeset object the DDI hooks are bound to.
 *
 * NULL when none is bound; the takeover readout walks this encoder.
 */
struct intel_encoder *
drv_i915_lcd_ms_bound_encoder(
	struct i915_lcd_world *world)
{
	/* No modeset object is bound. */
	if (world->ddi_ms == NULL)
		return NULL;

	/* Succeeded: reports the bound object's encoder. */
	return &world->ddi_ms->dig_port.base;
}

/*
 * Reports the connector of the modeset object the DDI hooks are bound to.
 *
 * NULL when none is bound; the takeover readout walks this connector.
 */
struct intel_connector *
drv_i915_lcd_ms_bound_connector(
	struct i915_lcd_world *world)
{
	/* No modeset object is bound. */
	if (world->ddi_ms == NULL)
		return NULL;

	/* Succeeded: reports the bound object's connector. */
	return &world->ddi_ms->connector;
}

/*
 * Binds the one encoder of a modeset object to the DDI hooks.
 *
 * The hooks are bound as intel_ddi_init() and intel_ddi_init_dp_connector()
 * bind them for a combo-PHY DDI on display version 12 and later (the
 * `encoder->... =` and `dig_port->dp.... =` assignments of intel_ddi.c; the
 * buffer translations as intel_ddi_buf_trans_init() binds them).  The
 * object becomes the bound object of its world, and its encoder the only
 * encoder the modeset text walks.  The object's world is the one prepare
 * recorded in it.
 */
void
drv_i915_lcd_ms_bind_encoder(
	struct i915_lcd_modeset *ms)
{
	struct i915_lcd_world *world;
	struct intel_encoder *encoder;
	struct intel_dp *intel_dp;

	/* Finds the world the object belongs to, its encoder and its DP half. */
	world = ms->world;
	encoder = &ms->dig_port.base;
	intel_dp = &ms->dig_port.dp;

	/*
	 * The object is the one the DDI hooks and the level shift answer for
	 * from now on, and its commit state names the world, so that the
	 * encoder walks of its crtc enable and disable find the object.
	 */
	world->ddi_ms = ms;
	ms->state.world = world;

	/*
	 * intel_ddi_init(): encoder->power_domain =
	 * intel_display_power_ddi_lanes_domain() = POWER_DOMAIN_PORT_DDI_LANES_A
	 * + port.  The encoder is the first and only one of the device, and the
	 * walk over the encoders of a mask visits it.
	 */
	encoder->power_domain = POWER_DOMAIN_PORT_DDI_LANES_A + (int)encoder->port;
	encoder->base.index = 0;
	world->i915_lcd_only_encoder = &encoder->base;

	/* Binds the enable and disable hooks of the crtc sequences. */
	encoder->enable = i915_enable_ddi;
	encoder->pre_pll_enable = i915_ddi_pre_pll_enable;
	encoder->pre_enable = i915_ddi_pre_enable;
	encoder->disable = i915_disable_ddi;
	encoder->post_disable = i915_ddi_post_disable;
	encoder->post_pll_disable = i915_ddi_post_pll_disable;

	/* Binds the combo PHY's clock and signal-level hooks. */
	encoder->enable_clock = i915_icl_ddi_combo_enable_clock;
	encoder->is_clock_enabled = i915_icl_ddi_combo_is_clock_enabled;
	encoder->disable_clock = i915_icl_ddi_combo_disable_clock;
	encoder->set_signal_levels = i915_icl_combo_phy_set_signal_levels;

	/* Binds the buffer-translation table of the port. */
	drv_i915_lcd_ms_bind_buf_trans(encoder);

	/* Binds the link-training hooks of the DP half. */
	intel_dp->prepare_link_retrain = i915_ddi_prepare_link_retrain;
	intel_dp->set_link_train = i915_ddi_set_link_train;
	intel_dp->set_idle_link_train = i915_ddi_set_idle_link_train;
	intel_dp->voltage_max = i915_ddi_dp_voltage_max;
	intel_dp->preemph_max = i915_ddi_dp_preemph_max;
}

/*
 * Runs the pre-PLL-enable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_pre_pll_enable() walks the atomic state's
 * connectors; here there is exactly one encoder, the bound object's, and
 * it sees the new crtc state.  The world is the one the atomic state
 * names; the crtc is not read.
 */
void
drv_i915_encoders_pre_pll_enable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the new state. */
	i915_encoders_call(ms, ms->dig_port.base.pre_pll_enable, state, state->crtc_state);
}

/*
 * Runs the pre-enable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_pre_enable(), on the one encoder with the new
 * crtc state, in the world the atomic state names.  The crtc is not read.
 */
void
drv_i915_encoders_pre_enable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the new state. */
	i915_encoders_call(ms, ms->dig_port.base.pre_enable, state, state->crtc_state);
}

/*
 * Runs the enable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_enable(), on the one encoder with the new crtc
 * state, in the world the atomic state names.  The crtc is not read.
 */
void
drv_i915_encoders_enable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the new state. */
	i915_encoders_call(ms, ms->dig_port.base.enable, state, state->crtc_state);
}

/*
 * Runs the disable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_disable(), on the one encoder with the old crtc
 * state, in the world the atomic state names.  The crtc is not read.
 */
void
drv_i915_encoders_disable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the old state. */
	i915_encoders_call(ms, ms->dig_port.base.disable, state, state->old_crtc_state);
}

/*
 * Runs the post-disable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_post_disable(), on the one encoder with the old
 * crtc state, in the world the atomic state names.  The crtc is not read.
 */
void
drv_i915_encoders_post_disable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the old state. */
	i915_encoders_call(ms, ms->dig_port.base.post_disable, state, state->old_crtc_state);
}

/*
 * Runs the post-PLL-disable hook of the encoder on the crtc.
 *
 * The Linux intel_encoders_post_pll_disable(), on the one encoder with the
 * old crtc state.  The crtc is not read.
 */
void
drv_i915_encoders_post_pll_disable(
	struct intel_atomic_state *state,
	struct intel_crtc *crtc)
{
	struct i915_lcd_modeset *ms;

	UNUSED_PARAMETER(crtc);

	/* Finds the object the commit's world has bound; none is reported. */
	ms = i915_encoders_bound(state);
	if (ms == NULL)
		return;

	/* Runs the bound encoder's hook on the old state. */
	i915_encoders_call(ms, ms->dig_port.base.post_pll_disable, state, state->old_crtc_state);
}

/*
 * ==== Static functions ====
 */

/* Finds the modeset object whose encoder this is (every hooked encoder is one). */
static struct i915_lcd_modeset *
i915_ddi_modeset(
	const struct intel_encoder *encoder)
{
	struct i915_lcd_modeset *ms;

	/* The encoder is the base of the object's digital port. */
	ms = container_of(encoder, struct i915_lcd_modeset, dig_port.base);

	/* Succeeded: reports the object. */
	return ms;
}

/*
 * Finds the device of the current entry point: the device the Linux text
 * reached through its file-scope pointer, as the encoder's world records it.
 */
static struct drm_i915_private *
i915_ddi_cur_i915(
	const struct intel_encoder *encoder)
{
	struct i915_lcd_modeset *ms;

	/* The encoder's modeset object leads to its world. */
	ms = i915_ddi_modeset(encoder);

	/* Succeeded: reports the device the entry point recorded. */
	return ms->world->i915_lcd_cur_i915;
}

/*
 * Finds the modeset object the DDI hooks are bound to in the world a
 * commit's atomic state names.  A state that names no world is reported
 * as an error (NULL): the Linux text reached the bound object through a
 * file-scope pointer, which a state without its world cannot replace.
 */
static struct i915_lcd_modeset *
i915_encoders_bound(
	struct intel_atomic_state *state)
{
	/* A state its builder did not tie to a world cannot be walked. */
	if (state->world == NULL) {
		drv_i915_lcd_error("encoder walk: the atomic state names no modeset world\n");
		return NULL;
	}

	/* Succeeded: reports the world's bound object. */
	return state->world->ddi_ms;
}

/* Runs one hook of the bound encoder, if it is bound, on a crtc state. */
static void
i915_encoders_call(
	struct i915_lcd_modeset *ms,
	i915_ddi_encoder_hook_t hook,
	struct intel_atomic_state *state,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_encoder *encoder;

	/* The one encoder of the modeset. */
	encoder = &ms->dig_port.base;

	/* An unbound hook does nothing. */
	if (!hook)
		return;

	/* Runs the hook with the connector state of the object. */
	hook(state, encoder, crtc_state, &ms->conn_state);
}

/* Converts a DP port clock into the DDI_BUF_CTL PHY link rate field (the Linux ddi_buf_phy_link_rate()). */
static u32
i915_ddi_buf_phy_link_rate(
	int port_clock)
{
	/* Each supported link rate has a field value of its own. */
	switch (port_clock) {
	case 162000:
		return DDI_BUF_PHY_LINK_RATE(0);
	case 216000:
		return DDI_BUF_PHY_LINK_RATE(4);
	case 243000:
		return DDI_BUF_PHY_LINK_RATE(5);
	case 270000:
		return DDI_BUF_PHY_LINK_RATE(1);
	case 324000:
		return DDI_BUF_PHY_LINK_RATE(6);
	case 432000:
		return DDI_BUF_PHY_LINK_RATE(7);
	case 540000:
		return DDI_BUF_PHY_LINK_RATE(2);
	case 810000:
		return DDI_BUF_PHY_LINK_RATE(3);
	default:
		I915_LCD_MISSING_CASE(port_clock);
		return DDI_BUF_PHY_LINK_RATE(0);
	}
}

/* Computes the DDI_BUF_CTL value of a DP port into intel_dp->DP (the Linux intel_ddi_init_dp_buf_reg()). */
static void
i915_ddi_init_dp_buf_reg(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	struct intel_dp *intel_dp;
	struct intel_digital_port *dig_port;
	enum phy phy;
	bool is_uhbr;
	bool is_tc;
	bool in_tbt_alt_mode;
	int display_ver;
	bool is_alderlake_p;

	/* Finds the device, the DP half, the port and the PHY of the encoder. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	intel_dp = enc_to_intel_dp(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* DDI_BUF_CTL_ENABLE will be set by intel_ddi_prepare_link_retrain() later */
	intel_dp->DP = dig_port->saved_port_bits |
		DDI_PORT_WIDTH(crtc_state->lane_count) |
		DDI_BUF_TRANS_SELECT(0);

	/* Display version 14 and later select the port data width by the channel coding. */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (display_ver >= 14) {
		is_uhbr = drv_i915_dp_is_uhbr(crtc_state);
		if (is_uhbr) {
			intel_dp->DP |= DDI_BUF_PORT_DATA_40BIT;
		} else {
			intel_dp->DP |= DDI_BUF_PORT_DATA_10BIT;
		}
	}

	/* Only an Alder Lake-P Type-C port adds more; any other port's value is complete. */
	is_alderlake_p = I915_LCD_IS_ALDERLAKE_P(i915);
	if (!is_alderlake_p)
		return;

	/* Asks whether the port's PHY is Type-C. */
	is_tc = drv_i915_lcd_intel_phy_is_tc(i915, phy);
	if (!is_tc)
		return;

	/* A Type-C port carries the PHY link rate, and the PHY ownership outside Thunderbolt-alt mode. */
	intel_dp->DP |= i915_ddi_buf_phy_link_rate(crtc_state->port_clock);
	in_tbt_alt_mode = i915_lcd_intel_tc_port_in_tbt_alt_mode(dig_port);
	if (!in_tbt_alt_mode)
		intel_dp->DP |= DDI_BUF_CTL_TC_PHY_OWNERSHIP;
}

/* Writes the MSA MISC word of a DP transcoder (the Linux intel_ddi_set_dp_msa()). */
static void
i915_ddi_set_dp_msa(
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	u32 temp;
	bool has_dp_encoder;
	bool needs_vsc_sdp;

	/* Finds the crtc, its device and its transcoder. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	cpu_transcoder = crtc_state->cpu_transcoder;

	/* Only a DP transcoder has an MSA. */
	has_dp_encoder = intel_crtc_has_dp_encoder(crtc_state);
	if (!has_dp_encoder)
		return;

	/* A DSI transcoder here is a warning. */
	(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, transcoder_is_dsi(cpu_transcoder));

	/* The MSA always carries a synchronous clock. */
	temp = DP_MSA_MISC_SYNC_CLOCK;

	/* The colour depth of the pipe. */
	switch (crtc_state->pipe_bpp) {
	case 18:
		temp |= DP_MSA_MISC_6_BPC;
		break;
	case 24:
		temp |= DP_MSA_MISC_8_BPC;
		break;
	case 30:
		temp |= DP_MSA_MISC_10_BPC;
		break;
	case 36:
		temp |= DP_MSA_MISC_12_BPC;
		break;
	default:
		I915_LCD_MISSING_CASE(crtc_state->pipe_bpp);
		break;
	}

	/* nonsense combination */
	(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, crtc_state->limited_color_range &&
				   crtc_state->output_format != INTEL_OUTPUT_FORMAT_RGB);

	/* Limited range RGB is signalled as CEA RGB. */
	if (crtc_state->limited_color_range)
		temp |= DP_MSA_MISC_COLOR_CEA_RGB;

	/*
	 * As per DP 1.2 spec section 2.3.4.3 while sending
	 * YCBCR 444 signals we should program MSA MISC1/0 fields with
	 * colorspace information.
	 */
	if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR444)
		temp |= DP_MSA_MISC_COLOR_YCBCR_444_BT709;

	/*
	 * As per DP 1.4a spec section 2.2.4.3 [MSA Field for Indication
	 * of Color Encoding Format and Content Color Gamut] while sending
	 * YCBCR 420, HDR BT.2020 signals we should program MSA MISC1 fields
	 * which indicate VSC SDP for the Pixel Encoding/Colorimetry Format.
	 */
	needs_vsc_sdp = drv_i915_dp_needs_vsc_sdp(crtc_state, conn_state);
	if (needs_vsc_sdp)
		temp |= DP_MSA_MISC_COLOR_VSC_SDP;

	/* Writes the word. */
	i915_lcd_intel_de_write(dev_priv, TRANS_MSA_MISC(cpu_transcoder), temp);
}

/* Converts a port-sync master transcoder into its select field value (the Linux bdw_trans_port_sync_master_select()). */
static u32
i915_bdw_trans_port_sync_master_select(
	enum transcoder master_transcoder)
{
	/* The eDP transcoder selects 0. */
	if (master_transcoder == TRANSCODER_EDP)
		return 0;

	/* Succeeded: transcoders A.. select 1.. */
	return master_transcoder + 1;
}

/*
 * Computes the TRANS_DDI_FUNC_CTL value of a crtc state (the Linux
 * intel_ddi_transcoder_func_reg_val_get(); only for the enable and config
 * of the transcoder function).
 */
static u32
i915_ddi_transcoder_func_reg_val_get(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv __maybe_unused;
	enum pipe pipe;
	enum transcoder cpu_transcoder;
	enum port port;
	enum transcoder master;
	u8 master_select;
	u32 temp;
	int display_ver;
	bool has_hdmi;
	bool has_analog;
	bool has_dp_mst;
	bool is_uhbr;

	/* Finds the crtc, its device, pipe and transcoder, and the port. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	pipe = crtc->pipe;
	cpu_transcoder = crtc_state->cpu_transcoder;
	port = encoder->port;
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* Enable TRANS_DDI_FUNC_CTL for the pipe to work in HDMI mode */
	temp = TRANS_DDI_FUNC_ENABLE;
	if (display_ver >= 12) {
		temp |= TGL_TRANS_DDI_SELECT_PORT(port);
	} else {
		temp |= TRANS_DDI_SELECT_PORT(port);
	}

	/* The colour depth of the pipe; an unknown depth is taken as 6 bpc. */
	switch (crtc_state->pipe_bpp) {
	default:
		I915_LCD_MISSING_CASE(crtc_state->pipe_bpp);
		fallthrough;
	case 18:
		temp |= TRANS_DDI_BPC_6;
		break;
	case 24:
		temp |= TRANS_DDI_BPC_8;
		break;
	case 30:
		temp |= TRANS_DDI_BPC_10;
		break;
	case 36:
		temp |= TRANS_DDI_BPC_12;
		break;
	}

	/* The sync polarities of the mode. */
	if (crtc_state->hw.adjusted_mode.flags & DRM_MODE_FLAG_PVSYNC)
		temp |= TRANS_DDI_PVSYNC;
	if (crtc_state->hw.adjusted_mode.flags & DRM_MODE_FLAG_PHSYNC)
		temp |= TRANS_DDI_PHSYNC;

	/* The eDP transcoder selects the pipe that feeds it. */
	if (cpu_transcoder == TRANSCODER_EDP) {
		switch (pipe) {
		default:
			I915_LCD_MISSING_CASE(pipe);
			fallthrough;
		case PIPE_A:
			/* On Haswell, can only use the always-on power well for
			 * eDP when not using the panel fitter, and when not
			 * using motion blur mitigation (which we don't
			 * support). */
			if (crtc_state->pch_pfit.force_thru) {
				temp |= TRANS_DDI_EDP_INPUT_A_ONOFF;
			} else {
				temp |= TRANS_DDI_EDP_INPUT_A_ON;
			}
			break;
		case PIPE_B:
			temp |= TRANS_DDI_EDP_INPUT_B_ONOFF;
			break;
		case PIPE_C:
			temp |= TRANS_DDI_EDP_INPUT_C_ONOFF;
			break;
		}
	}

	/* Which kinds of output the crtc state drives. */
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	has_analog = false;
	has_dp_mst = false;
	if (!has_hdmi) {
		has_analog = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_ANALOG);
		if (!has_analog)
			has_dp_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);
	}

	/* The mode of the transcoder function, by the kind of output. */
	if (has_hdmi) {
		/* HDMI, or DVI for a sink without HDMI, with its scrambling and clock ratio. */
		if (crtc_state->has_hdmi_sink) {
			temp |= TRANS_DDI_MODE_SELECT_HDMI;
		} else {
			temp |= TRANS_DDI_MODE_SELECT_DVI;
		}

		if (crtc_state->hdmi_scrambling)
			temp |= TRANS_DDI_HDMI_SCRAMBLING;
		if (crtc_state->hdmi_high_tmds_clock_ratio)
			temp |= TRANS_DDI_HIGH_TMDS_CHAR_RATE;
		if (display_ver >= 14)
			temp |= TRANS_DDI_PORT_WIDTH(crtc_state->lane_count);
	} else if (has_analog) {
		/* FDI, with its lane count. */
		temp |= TRANS_DDI_MODE_SELECT_FDI_OR_128B132B;
		temp |= (crtc_state->fdi_lanes - 1) << 1;
	} else if (has_dp_mst) {
		/* DP MST, or 128b/132b at a UHBR rate, with its lanes and master. */
		is_uhbr = drv_i915_dp_is_uhbr(crtc_state);
		if (is_uhbr) {
			temp |= TRANS_DDI_MODE_SELECT_FDI_OR_128B132B;
		} else {
			temp |= TRANS_DDI_MODE_SELECT_DP_MST;
		}
		temp |= DDI_PORT_WIDTH(crtc_state->lane_count);

		if (display_ver >= 12) {
			master = crtc_state->mst_master_transcoder;
			(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm,
						   master == INVALID_TRANSCODER);
			temp |= TRANS_DDI_MST_TRANSPORT_SELECT(master);
		}
	} else {
		/* DP SST with its lanes. */
		temp |= TRANS_DDI_MODE_SELECT_DP_SST;
		temp |= DDI_PORT_WIDTH(crtc_state->lane_count);
	}

	/* Display versions 8 to 10 select the port-sync master here. */
	if (I915_LCD_IS_DISPLAY_VER(dev_priv, 8, 10) &&
	    crtc_state->master_transcoder != INVALID_TRANSCODER) {
		master_select = i915_bdw_trans_port_sync_master_select(crtc_state->master_transcoder);
		temp |= TRANS_DDI_PORT_SYNC_ENABLE |
			TRANS_DDI_PORT_SYNC_MASTER_SELECT(master_select);
	}

	/* Succeeded: reports the word. */
	return temp;
}

/* Writes and enables the transcoder function of a crtc state (the Linux intel_ddi_enable_transcoder_func()). */
static void
i915_ddi_enable_transcoder_func(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	enum transcoder master_transcoder;
	u8 master_select;
	u32 ctl2;
	u32 ctl;
	int display_ver;

	/* Finds the crtc, its device and its transcoder. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	cpu_transcoder = crtc_state->cpu_transcoder;

	/* Display version 11 and later write the port-sync word first. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 11) {
		master_transcoder = crtc_state->master_transcoder;
		ctl2 = 0;

		if (master_transcoder != INVALID_TRANSCODER) {
			master_select = i915_bdw_trans_port_sync_master_select(master_transcoder);
			ctl2 |= PORT_SYNC_MODE_ENABLE |
				PORT_SYNC_MODE_MASTER_SELECT(master_select);
		}

		i915_lcd_intel_de_write(dev_priv,
					TRANS_DDI_FUNC_CTL2(cpu_transcoder),
					ctl2);
	}

	/* Writes the transcoder function with its enable bit. */
	ctl = i915_ddi_transcoder_func_reg_val_get(encoder, crtc_state);
	i915_lcd_intel_de_write(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder), ctl);
}

/*
 * Writes the transcoder function of a crtc state without enabling it (the
 * Linux intel_ddi_config_transcoder_func()).
 */
static void
i915_ddi_config_transcoder_func(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	u32 ctl;

	/* Finds the crtc, its device and its transcoder. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	cpu_transcoder = crtc_state->cpu_transcoder;

	/* Writes the word with the enable bit cleared. */
	ctl = i915_ddi_transcoder_func_reg_val_get(encoder, crtc_state);
	ctl &= ~TRANS_DDI_FUNC_ENABLE;
	i915_lcd_intel_de_write(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder), ctl);
}

/*
 * Enables and trains a DP port on display version 12 and 13 (the Linux
 * tgl_ddi_pre_enable_dp(), which follows the bspec's numbered steps).
 */
static void
i915_tgl_ddi_pre_enable_dp(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct intel_dp *intel_dp;
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	struct intel_digital_port *dig_port;
	bool is_mst;
	bool in_tbt_alt_mode;

	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(conn_state);

	/* Finds the DP half, the devices, the port and whether the stream is MST. */
	intel_dp = enc_to_intel_dp(encoder);
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	is_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);

	/* Records the link rate and lane count of the link to train. */
	drv_i915_dp_set_link_params(intel_dp,
				    crtc_state->port_clock,
				    crtc_state->lane_count);

	/*
	 * We only configure what the register value will be here.  Actual
	 * enabling happens during link training farther down.
	 */
	i915_ddi_init_dp_buf_reg(encoder, crtc_state);

	/*
	 * 1. Enable Power Wells
	 *
	 * This was handled at the beginning of intel_atomic_commit_tail(),
	 * before we called down into this function.
	 */

	/* 2. Enable Panel Power if PPS is required */
	i915_lcd_intel_pps_on(cur_i915, intel_dp);

	/*
	 * 3. For non-TBT Type-C ports, set FIA lane count
	 * (DFLEXDPSP.DPX4TXLATC)
	 *
	 * This was done before tgl_ddi_pre_enable_dp by
	 * hsw_crtc_enable()->intel_encoders_pre_pll_enable().
	 */

	/*
	 * 4. Enable the port PLL.
	 *
	 * The PLL enabling itself was already done before this function by
	 * hsw_crtc_enable()->intel_enable_shared_dpll().  We need only
	 * configure the PLL to port mapping here.
	 */
	i915_ddi_enable_clock(encoder, crtc_state);

	/* 5. If IO power is controlled through PWR_WELL_CTL, Enable IO Power */
	in_tbt_alt_mode = i915_lcd_intel_tc_port_in_tbt_alt_mode(dig_port);
	if (!in_tbt_alt_mode) {
		(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, dig_port->ddi_io_wakeref);
		dig_port->ddi_io_wakeref = i915_lcd_intel_display_power_get(dev_priv,
									    dig_port->ddi_io_power_domain);
	}

	/* 6. Program DP_MODE */
	icl_program_mg_dp_mode(dig_port, crtc_state);

	/*
	 * 7. The rest of the below are substeps under the bspec's "Enable and
	 * Train Display Port" step.  Note that steps that are specific to
	 * MST will be handled by intel_mst_pre_enable_dp() before/after it
	 * calls into this function.  Also intel_mst_pre_enable_dp() only calls
	 * us when active_mst_links==0, so any steps designated for "single
	 * stream or multi-stream master transcoder" can just be performed
	 * unconditionally here.
	 */

	/*
	 * 7.a Configure Transcoder Clock Select to direct the Port clock to the
	 * Transcoder.
	 */
	i915_ddi_enable_transcoder_clock(encoder, crtc_state);

	/* A DP 2.0 transcoder has its own DP2 control word. */
	if (HAS_DP20(dev_priv))
		intel_ddi_config_transcoder_dp2(encoder, crtc_state);

	/*
	 * 7.b Configure TRANS_DDI_FUNC_CTL DDI Select, DDI Mode Select & MST
	 * Transport Select
	 */
	i915_ddi_config_transcoder_func(encoder, crtc_state);

	/*
	 * 7.c Configure & enable DP_TP_CTL with link training pattern 1
	 * selected
	 *
	 * This will be handled by the intel_dp_start_link_train() farther
	 * down this function.
	 */

	/* 7.e Configure voltage swing and related IO settings */
	encoder->set_signal_levels(encoder, crtc_state);

	/*
	 * 7.f Combo PHY: Configure PORT_CL_DW10 Static Power Down to power up
	 * the used lanes of the DDI.
	 */
	i915_ddi_power_up_lanes(encoder, crtc_state);

	/*
	 * 7.g Program CoG/MSO configuration bits in DSS_CTL1 if selected.
	 */
	i915_ddi_mso_configure(crtc_state);

	/* Wakes the sink of a single stream. */
	if (!is_mst)
		drv_i915_dp_set_power(intel_dp, DP_SET_POWER_D0);

	/* Configures a protocol converter behind the port. */
	intel_dp_configure_protocol_converter(intel_dp, crtc_state);

	/* Enables the sink's decompression for a single stream. */
	if (!is_mst) {
		intel_dp_sink_enable_decompression(state,
						   I915_LCD_TO_INTEL_CONNECTOR(conn_state->connector),
						   crtc_state);
	}

	/*
	 * DDI FEC: "anticipates enabling FEC encoding sets the FEC_READY bit
	 * in the FEC_CONFIGURATION register to 1 before initiating link
	 * training
	 */
	intel_dp_sink_set_fec_ready(intel_dp, crtc_state, true);

	/* Checks the FRL training of a PCON and configures its DSC. */
	intel_dp_check_frl_training(intel_dp);
	intel_dp_pcon_dsc_configure(intel_dp, crtc_state);

	/*
	 * 7.i Follow DisplayPort specification training sequence (see notes for
	 *     failure handling)
	 * 7.j If DisplayPort multi-stream - Set DP_TP_CTL link training to Idle
	 *     Pattern, wait for 5 idle patterns (DP_TP_STATUS Min_Idles_Sent)
	 *     (timeout after 800 us)
	 */
	drv_i915_dp_start_link_train(intel_dp, crtc_state);

	/* 7.k Set DP_TP_CTL link training to Normal */
	if (!is_trans_port_sync_mode(crtc_state))
		drv_i915_dp_stop_link_train(intel_dp, crtc_state);

	/* 7.l Configure and enable FEC if needed */
	intel_ddi_enable_fec(encoder, crtc_state);

	/* Writes the sink's DSC picture parameters for a single stream. */
	if (!is_mst)
		intel_dsc_dp_pps_write(encoder, crtc_state);
}

/* Enables a DP port by the display version's sequence, then its MSA (the Linux intel_ddi_pre_enable_dp()). */
static void
i915_ddi_pre_enable_dp(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	struct drm_i915_private *cur_i915;
	struct intel_dp *intel_dp __maybe_unused;
	int display_ver;
	bool has_dp_mst;

	/* Finds the devices and the DP half of the encoder. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	intel_dp = enc_to_intel_dp(encoder);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* A DP 2.0 source sends the SDP CRC and enables the sink's panel replay. */
	if (HAS_DP20(dev_priv)) {
		intel_dp_128b132b_sdp_crc16(enc_to_intel_dp(encoder),
					    crtc_state);
		if (crtc_state->has_panel_replay) {
			(void)I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915,
							  &intel_dp->aux,
							  I915_LCD_PANEL_REPLAY_CONFIG,
							  I915_LCD_DP_PANEL_REPLAY_ENABLE);
		}
	}

	/* Runs the enable sequence of the display version. */
	if (display_ver >= 14) {
		mtl_ddi_pre_enable_dp(state, encoder, crtc_state, conn_state);
	} else if (display_ver >= 12) {
		i915_tgl_ddi_pre_enable_dp(state, encoder, crtc_state, conn_state);
	} else {
		hsw_ddi_pre_enable_dp(state, encoder, crtc_state, conn_state);
	}

	/* MST will call a setting of MSA after an allocating of Virtual Channel
	 * from MST encoder pre_enable callback.
	 */
	has_dp_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);
	if (!has_dp_mst)
		i915_ddi_set_dp_msa(crtc_state, conn_state);
}

/* Runs the pre-enable of an HDMI or a DP port (the Linux intel_ddi_pre_enable(), the pre_enable hook). */
static void
i915_ddi_pre_enable(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	enum pipe pipe __maybe_unused;
	bool has_hdmi;

	/* Finds the crtc, its device and its pipe. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	pipe = crtc->pipe;

	/*
	 * When called from DP MST code:
	 * - conn_state will be NULL
	 * - encoder will be the main encoder (ie. mst->primary)
	 * - the main connector associated with this port
	 *   won't be active or linked to a crtc
	 * - crtc_state will be the state of the first stream to
	 *   be activated on this port, and it may not be the same
	 *   stream that will be deactivated last, but each stream
	 *   should have a state that is identical when it comes to
	 *   the DP link parameteres
	 */

	/* A PCH encoder here is a warning. */
	(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, crtc_state->has_pch_encoder);

	/* Where Linux arms the pipe's underrun reporting. */
	intel_set_cpu_fifo_underrun_reporting(dev_priv, pipe, true);

	/* Runs the HDMI or the DP enable of the port. */
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		i915_ddi_pre_enable_hdmi(state, encoder, crtc_state,
					 conn_state);
	} else {
		dig_port = i915_lcd_enc_to_dig_port(encoder);

		i915_ddi_pre_enable_dp(state, encoder, crtc_state,
				       conn_state);

		/* FIXME precompute everything properly */
		/* FIXME how do we turn infoframes off again? */
		if (dig_port->lspcon.active && intel_dp_has_hdmi_sink(&dig_port->dp)) {
			dig_port->set_infoframes(encoder,
						 crtc_state->has_infoframe,
						 crtc_state, conn_state);
		}
	}
}

/* Runs the DP half of the port enable after the transcoder (the Linux intel_enable_ddi_dp()). */
static void
i915_enable_ddi_dp(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	struct intel_dp *intel_dp;
	struct intel_digital_port *dig_port;
	enum port port;
	int display_ver;

	UNUSED_PARAMETER(state);

	/* Finds the device, the DP half, the port object and its number. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	intel_dp = enc_to_intel_dp(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	port = encoder->port;

	/* Port A before display version 9 stops the link training only here. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (port == PORT_A && display_ver < 9)
		drv_i915_dp_stop_link_train(intel_dp, crtc_state);

	/* Turns the privacy screen and the eDP backlight on. */
	drm_connector_update_privacy_screen(conn_state);
	drv_i915_edp_backlight_on(crtc_state, conn_state);

	/* Enables the DP infoframes unless an LSPCON drives an HDMI sink without them. */
	if (!dig_port->lspcon.active || intel_dp_has_hdmi_sink(&dig_port->dp))
		drv_i915_dp_set_infoframes(encoder, true, crtc_state, conn_state);

	/* Stops the link training of the port-sync slaves. */
	trans_port_sync_stop_link_train(state, encoder, crtc_state);
}

/* Enables the transcoder and the port output (the Linux intel_enable_ddi(), the enable hook). */
static void
i915_enable_ddi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	bool has_hdmi;

	/* A PCH encoder here is a warning. */
	(void)I915_LCD_DRM_WARN_ON(state->base.dev, crtc_state->has_pch_encoder);

	/* The master pipe enables the transcoder function. */
	if (!intel_crtc_is_bigjoiner_slave(crtc_state))
		i915_ddi_enable_transcoder_func(encoder, crtc_state);

	/* Enable/Disable DP2.0 SDP split config before transcoder */
	intel_audio_sdp_split_update(crtc_state);

	/* Enables the transcoder. */
	drv_i915_enable_transcoder(crtc_state);

	/* Waits for the FEC status and turns the crtc's vblank on. */
	intel_ddi_wait_for_fec_status(encoder, crtc_state, true);
	intel_crtc_vblank_on(crtc_state);

	/* Runs the HDMI or the DP half of the enable. */
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		i915_enable_ddi_hdmi(state, encoder, crtc_state, conn_state);
	} else {
		i915_enable_ddi_dp(state, encoder, crtc_state, conn_state);
	}

	/* Enables HDCP when content protection asks for it. */
	intel_hdcp_enable(state, encoder, crtc_state, conn_state);
}

/*
 * Takes the port's AUX power before the PLL is enabled (the Linux
 * intel_ddi_pre_pll_enable(), the pre_pll_enable hook).
 */
static void
i915_ddi_pre_pll_enable(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	struct intel_crtc *master_crtc __maybe_unused;
	enum phy phy;
	bool is_tc_port;
	bool in_tbt_alt_mode;

	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(conn_state);

	/* Finds the device, the port object and whether its PHY is Type-C. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, encoder->port);
	is_tc_port = drv_i915_lcd_intel_phy_is_tc(dev_priv, phy);

	/* A Type-C port takes its link and selects its active PLL. */
	if (is_tc_port) {
		master_crtc = to_intel_crtc(crtc_state->uapi.crtc);

		intel_tc_port_get_link(dig_port, crtc_state->lane_count);
		intel_ddi_update_active_dpll(state, encoder, master_crtc);
	}

	/* Takes the main link's AUX power. */
	i915_main_link_aux_power_domain_get(dig_port, crtc_state);

	/* Whether the port is in Thunderbolt-alt mode (only asked for a Type-C port). */
	in_tbt_alt_mode = true;
	if (is_tc_port)
		in_tbt_alt_mode = i915_lcd_intel_tc_port_in_tbt_alt_mode(dig_port);

	/*
	 * Program the lane count for static/dynamic connections on
	 * Type-C ports.  Skip this step for TBT.
	 */
	if (is_tc_port && !in_tbt_alt_mode) {
		intel_tc_port_set_fia_lane_count(dig_port, crtc_state->lane_count);
	} else if (IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv)) {
		bxt_ddi_phy_set_lane_optim_mask(encoder,
						crtc_state->lane_lat_optim_mask);
	}
}

/* Reports the highest voltage swing the port's buffer table reaches (the Linux intel_ddi_dp_voltage_max()). */
static u8
i915_ddi_dp_voltage_max(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_encoder *encoder;
	struct drm_i915_private *dev_priv __maybe_unused;
	int n_entries;
	bool warned;

	/* Finds the port's encoder and its device. */
	encoder = &i915_lcd_dp_to_dig_port(intel_dp)->base;
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* Asks the port for the number of entries of its buffer table. */
	encoder->get_buf_trans(encoder, crtc_state, &n_entries);

	/* A table without an entry is taken as one entry. */
	warned = I915_LCD_DRM_WARN_ON(&dev_priv->drm, n_entries < 1);
	if (warned)
		n_entries = 1;

	/*
	 * A table longer than the signal-level list is taken as the list's
	 * length.  The warning text is the Linux drm_WARN_ON()'s; the comparison
	 * casts the list length so that it is not an unsigned comparison.
	 */
	warned = I915_LCD_DRM_WARN(&dev_priv->drm,
				   n_entries > (int)ARRAY_SIZE(index_to_dp_signal_levels),
				   "WARN_ON(n_entries > ARRAY_SIZE(index_to_dp_signal_levels))\n");
	if (warned)
		n_entries = ARRAY_SIZE(index_to_dp_signal_levels);

	/* Succeeded: reports the swing of the last entry. */
	return index_to_dp_signal_levels[n_entries - 1] &
		DP_TRAIN_VOLTAGE_SWING_MASK;
}

/*
 * Reports the highest pre-emphasis of the port (the Linux
 * intel_ddi_dp_preemph_max()).  We assume that the full set of
 * pre-emphasis values can be used on all DDI platforms.
 */
static u8
i915_ddi_dp_preemph_max(
	struct intel_dp *intel_dp)
{
	UNUSED_PARAMETER(intel_dp);

	/* Succeeded: every DDI platform reaches level 3. */
	return DP_TRAIN_PRE_EMPH_LEVEL_3;
}

/* Maps the port to its PLL and ungates its clock, if the port can (the Linux intel_ddi_enable_clock()). */
static void
i915_ddi_enable_clock(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	/* A port without the hook has nothing to enable. */
	if (!encoder->enable_clock)
		return;

	/* Enables the clock through the port's hook. */
	encoder->enable_clock(encoder, crtc_state);
}

/* Gates the port's clock, if the port can (the Linux intel_ddi_disable_clock()). */
static void
i915_ddi_disable_clock(
	struct intel_encoder *encoder)
{
	/* A port without the hook has nothing to disable. */
	if (!encoder->disable_clock)
		return;

	/* Disables the clock through the port's hook. */
	encoder->disable_clock(encoder);
}

/*
 * Selects the port's PLL and ungates its clock in a clock register, under
 * the DPLL lock (the Linux _icl_ddi_enable_clock(); the lock is taken on
 * the entry point's device).
 */
static void
i915_icl_ddi_enable_clock_reg(
	struct drm_i915_private *cur_i915,
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clk_sel_mask,
	u32 clk_sel,
	u32 clk_off)
{
	/* Selects the PLL, then ungates the clock. */
	I915_LCD_MUTEX_LOCK(cur_i915, &i915->display.dpll.lock);

	(void)i915_lcd_intel_de_rmw(i915, reg, clk_sel_mask, clk_sel);

	/*
	 * "This step and the step before must be
	 *  done with separate register writes."
	 */
	(void)i915_lcd_intel_de_rmw(i915, reg, clk_off, 0);

	I915_LCD_MUTEX_UNLOCK(cur_i915, &i915->display.dpll.lock);
}

/*
 * Gates the port's clock in a clock register, under the DPLL lock (the
 * Linux _icl_ddi_disable_clock(); the lock is taken on the entry point's
 * device).
 */
static void
i915_icl_ddi_disable_clock_reg(
	struct drm_i915_private *cur_i915,
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clk_off)
{
	/* Gates the clock. */
	I915_LCD_MUTEX_LOCK(cur_i915, &i915->display.dpll.lock);

	(void)i915_lcd_intel_de_rmw(i915, reg, 0, clk_off);

	I915_LCD_MUTEX_UNLOCK(cur_i915, &i915->display.dpll.lock);
}

/* Maps a combo PHY port to the crtc's PLL and ungates its clock (the Linux icl_ddi_combo_enable_clock(), the enable_clock hook). */
static void
i915_icl_ddi_combo_enable_clock(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	const struct intel_shared_dpll *pll;
	enum phy phy;
	bool warned;

	/* Finds the devices, the crtc's PLL and the port's PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	pll = crtc_state->shared_dpll;
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* A crtc state without a PLL is a warning, and nothing is mapped. */
	warned = I915_LCD_DRM_WARN_ON(&i915->drm, !pll);
	if (warned)
		return;

	/* Selects the PLL for the PHY and ungates the PHY's DDI clock. */
	i915_icl_ddi_enable_clock_reg(cur_i915,
				      i915,
				      ICL_DPCLKA_CFGCR0,
				      ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy),
				      ICL_DPCLKA_CFGCR0_DDI_CLK_SEL(pll->info->id, phy),
				      ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy));
}

/* Gates the DDI clock of a combo PHY port (the Linux icl_ddi_combo_disable_clock(), the disable_clock hook). */
static void
i915_icl_ddi_combo_disable_clock(
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	enum phy phy;

	/* Finds the devices and the port's PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* Gates the PHY's DDI clock. */
	i915_icl_ddi_disable_clock_reg(cur_i915,
				       i915,
				       ICL_DPCLKA_CFGCR0,
				       ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy));
}

/* Selects the AUX power domain the port's main link needs (the Linux intel_ddi_main_link_aux_domain()). */
static enum intel_display_power_domain
i915_ddi_main_link_aux_domain(
	struct intel_digital_port *dig_port,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	enum phy phy;
	bool has_dp_encoder;
	bool is_tc;
	enum intel_display_power_domain domain;
	int display_ver;

	/* Finds the device and the port's PHY. */
	i915 = i915_lcd_to_i915(dig_port->base.base.dev);
	phy = drv_i915_lcd_intel_port_to_phy(i915, dig_port->base.port);

	/*
	 * ICL+ HW requires corresponding AUX IOs to be powered up for PSR with
	 * DC states enabled at the same time, while for driver initiated AUX
	 * transfers we need the same AUX IOs to be powered but with DC states
	 * disabled. Accordingly use the AUX_IO_<port> power domain here which
	 * leaves DC states enabled.
	 *
	 * Before MTL TypeC PHYs (in all TypeC modes and both DP/HDMI) also require
	 * AUX IO to be enabled, but all these require DC_OFF to be enabled as
	 * well, so we can acquire a wider AUX_<port> power domain reference
	 * instead of a specific AUX_IO_<port> reference without powering up any
	 * extra wells.
	 */
	if (intel_psr_needs_aux_io_power(&dig_port->base, crtc_state))
		return intel_display_power_aux_io_domain(i915, dig_port->aux_ch);

	/* Before display version 14 a DP stream or a Type-C PHY takes the AUX domain. */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (display_ver < 14) {
		has_dp_encoder = intel_crtc_has_dp_encoder(crtc_state);
		is_tc = false;
		if (!has_dp_encoder)
			is_tc = drv_i915_lcd_intel_phy_is_tc(i915, phy);

		if (has_dp_encoder || is_tc) {
			domain = drv_i915_aux_power_domain(dig_port);
			return domain;
		}
	}

	/* Succeeded: the main link needs no AUX power. */
	return POWER_DOMAIN_INVALID;
}

/* Takes the AUX power reference of the port's main link (the Linux main_link_aux_power_domain_get()). */
static void
i915_main_link_aux_power_domain_get(
	struct intel_digital_port *dig_port,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	enum intel_display_power_domain domain;

	/* Finds the device and the domain the main link needs. */
	i915 = i915_lcd_to_i915(dig_port->base.base.dev);
	domain = i915_ddi_main_link_aux_domain(dig_port, crtc_state);

	/* A reference still held here is a warning. */
	(void)I915_LCD_DRM_WARN_ON(&i915->drm, dig_port->aux_wakeref);

	/* A main link that needs no AUX power takes nothing. */
	if (domain == POWER_DOMAIN_INVALID)
		return;

	/* Takes the reference the post-PLL disable gives back. */
	dig_port->aux_wakeref = i915_lcd_intel_display_power_get(i915, domain);
}

/* Gives the AUX power reference of the port's main link back (the Linux main_link_aux_power_domain_put()). */
static void
i915_main_link_aux_power_domain_put(
	struct intel_digital_port *dig_port,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	enum intel_display_power_domain domain;
	intel_wakeref_t wf;

	/* Finds the device and the domain the main link needed. */
	i915 = i915_lcd_to_i915(dig_port->base.base.dev);
	domain = i915_ddi_main_link_aux_domain(dig_port, crtc_state);

	/* Takes the reference out of the port; none was held. */
	wf = fetch_and_zero(&dig_port->aux_wakeref);
	if (!wf)
		return;

	/* Gives the reference back. */
	i915_lcd_intel_display_power_put(i915, domain, wf);
}

/* Directs the port clock to the crtc's transcoder (the Linux intel_ddi_enable_transcoder_clock()). */
static void
i915_ddi_enable_transcoder_clock(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	enum phy phy;
	int display_ver;
	u32 val;

	/* Finds the crtc, its device, its transcoder and the port's PHY. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	cpu_transcoder = crtc_state->cpu_transcoder;
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, encoder->port);

	/* The eDP transcoder has no clock select. */
	if (cpu_transcoder == TRANSCODER_EDP)
		return;

	/* The select value names the PHY from display version 13, the port before. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 13) {
		val = TGL_TRANS_CLK_SEL_PORT(phy);
	} else if (display_ver >= 12) {
		val = TGL_TRANS_CLK_SEL_PORT(encoder->port);
	} else {
		val = TRANS_CLK_SEL_PORT(encoder->port);
	}

	/* Writes the clock select. */
	i915_lcd_intel_de_write(dev_priv, TRANS_CLK_SEL(cpu_transcoder), val);
}

/* Directs no clock to the crtc's transcoder (the Linux intel_ddi_disable_transcoder_clock()). */
static void
i915_ddi_disable_transcoder_clock(
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	u32 val;
	int display_ver;

	/* Finds the crtc's device and its transcoder. */
	dev_priv = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	cpu_transcoder = crtc_state->cpu_transcoder;

	/* The eDP transcoder has no clock select. */
	if (cpu_transcoder == TRANSCODER_EDP)
		return;

	/* The disabled value of the display version. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 12) {
		val = TGL_TRANS_CLK_SEL_DISABLED;
	} else {
		val = TRANS_CLK_SEL_DISABLED;
	}

	/* Writes the clock select. */
	i915_lcd_intel_de_write(dev_priv, TRANS_CLK_SEL(cpu_transcoder), val);
}

/* Converts a lane's DP train set into a buffer-translation index (the Linux intel_ddi_dp_level()). */
static int
i915_ddi_dp_level(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	int lane)
{
	u8 train_set;
	u8 signal_levels;
	bool is_uhbr;
	int level;

	/* The lane's train set. */
	train_set = intel_dp->train_set[lane];

	/* A UHBR link trains with a TX FFE preset, which is the index itself. */
	is_uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (is_uhbr)
		return train_set & DP_TX_FFE_PRESET_VALUE_MASK;

	/* An 8b/10b link's swing and pre-emphasis pair is looked up. */
	signal_levels = train_set & (DP_TRAIN_VOLTAGE_SWING_MASK |
				     DP_TRAIN_PRE_EMPHASIS_MASK);
	level = i915_translate_signal_level(intel_dp, signal_levels);

	/* Succeeded: reports the index of the pair. */
	return level;
}

/* Selects the buffer-translation entry of one lane (the Linux intel_ddi_level()). */
static int
i915_ddi_level(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	int lane)
{
	struct drm_i915_private *i915 __maybe_unused;
	const struct intel_ddi_buf_trans *trans;
	int level;
	int n_entries;
	bool warned;
	bool has_hdmi;

	/* Finds the device and the port's buffer table. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	trans = encoder->get_buf_trans(encoder, crtc_state, &n_entries);

	/* A port without a table is a warning, and the first entry is used. */
	warned = drm_WARN_ON_ONCE(&i915->drm, !trans);
	if (warned)
		return 0;

	/* HDMI uses the VBT's level; DP the level the sink asked for. */
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		level = i915_ddi_hdmi_level(encoder, trans);
	} else {
		level = i915_ddi_dp_level(enc_to_intel_dp(encoder), crtc_state,
					  lane);
	}

	/* A level past the table is a warning, and the last entry is used. */
	warned = drm_WARN_ON_ONCE(&i915->drm, level >= n_entries);
	if (warned)
		level = n_entries - 1;

	/* Succeeded: reports the entry. */
	return level;
}

/* Selects the loadgen bit of one lane (the Linux icl_combo_phy_loadgen_select()). */
static u32
i915_icl_combo_phy_loadgen_select(
	const struct intel_crtc_state *crtc_state,
	int lane)
{
	/* Above 6 GHz no lane selects loadgen. */
	if (crtc_state->port_clock > 600000)
		return 0;

	/* With four lanes, lanes 1 to 3 select it. */
	if (crtc_state->lane_count == 4) {
		if (lane >= 1)
			return LOADGEN_SELECT;

		return 0;
	}

	/* With one or two lanes, lanes 1 and 2 select it. */
	if (lane == 1 || lane == 2)
		return LOADGEN_SELECT;

	/* Succeeded: the other lanes do not. */
	return 0;
}

/* Programs the voltage swing of every lane of a combo PHY (the Linux icl_ddi_combo_vswing_program()). */
static void
i915_icl_ddi_combo_vswing_program(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	const struct intel_ddi_buf_trans *trans;
	struct intel_dp *intel_dp;
	enum phy phy;
	int n_entries;
	int ln;
	int level;
	u32 val;
	bool warned;
	bool has_edp;

	/* Finds the device, the port's buffer table and its PHY. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, encoder->port);
	trans = encoder->get_buf_trans(encoder, crtc_state, &n_entries);

	/* A port without a table is a warning, and nothing is programmed. */
	warned = drm_WARN_ON_ONCE(&dev_priv->drm, !trans);
	if (warned)
		return;

	/* An eDP port overrides the 4K2K mode when its table is the HOBL one. */
	has_edp = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP);
	if (has_edp) {
		intel_dp = enc_to_intel_dp(encoder);

		val = EDP4K2K_MODE_OVRD_EN | EDP4K2K_MODE_OVRD_OPTIMIZED;
		intel_dp->hobl_active = drv_i915_is_hobl_buf_trans(trans);
		if (intel_dp->hobl_active) {
			(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_CL_DW10(phy), val, val);
		} else {
			(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_CL_DW10(phy), val, 0);
		}
	}

	/* Set PORT_TX_DW5 */
	val = i915_lcd_intel_de_read(dev_priv, ICL_PORT_TX_DW5_LN(0, phy));
	val &= ~(SCALING_MODE_SEL_MASK | RTERM_SELECT_MASK |
		 TAP2_DISABLE | TAP3_DISABLE);
	val |= SCALING_MODE_SEL(0x2);
	val |= RTERM_SELECT(0x6);
	val |= TAP3_DISABLE;
	i915_lcd_intel_de_write(dev_priv, ICL_PORT_TX_DW5_GRP(phy), val);

	/* Program PORT_TX_DW2 */
	for (ln = 0; ln < 4; ln++) {
		level = i915_ddi_level(encoder, crtc_state, ln);

		(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_TX_DW2_LN(ln, phy),
					    SWING_SEL_UPPER_MASK | SWING_SEL_LOWER_MASK | RCOMP_SCALAR_MASK,
					    SWING_SEL_UPPER(trans->entries[level].icl.dw2_swing_sel) |
					    SWING_SEL_LOWER(trans->entries[level].icl.dw2_swing_sel) |
					    RCOMP_SCALAR(0x98));
	}

	/* Program PORT_TX_DW4 */
	/* We cannot write to GRP. It would overwrite individual loadgen. */
	for (ln = 0; ln < 4; ln++) {
		level = i915_ddi_level(encoder, crtc_state, ln);

		(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_TX_DW4_LN(ln, phy),
					    POST_CURSOR_1_MASK | POST_CURSOR_2_MASK | CURSOR_COEFF_MASK,
					    POST_CURSOR_1(trans->entries[level].icl.dw4_post_cursor_1) |
					    POST_CURSOR_2(trans->entries[level].icl.dw4_post_cursor_2) |
					    CURSOR_COEFF(trans->entries[level].icl.dw4_cursor_coeff));
	}

	/* Program PORT_TX_DW7 */
	for (ln = 0; ln < 4; ln++) {
		level = i915_ddi_level(encoder, crtc_state, ln);

		(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_TX_DW7_LN(ln, phy),
					    N_SCALAR_MASK,
					    N_SCALAR(trans->entries[level].icl.dw7_n_scalar));
	}
}

/*
 * Programs the signal levels of a combo PHY port (the Linux
 * icl_combo_phy_set_signal_levels(), the set_signal_levels hook).
 */
static void
i915_icl_combo_phy_set_signal_levels(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	enum phy phy;
	u32 val;
	u32 loadgen;
	int ln;
	bool has_hdmi;

	/* Finds the device and the port's PHY. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, encoder->port);

	/*
	 * 1. If port type is eDP or DP,
	 * set PORT_PCS_DW1 cmnkeeper_enable to 1b,
	 * else clear to 0b.
	 */
	val = i915_lcd_intel_de_read(dev_priv, ICL_PORT_PCS_DW1_LN(0, phy));
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		val &= ~COMMON_KEEPER_EN;
	} else {
		val |= COMMON_KEEPER_EN;
	}
	i915_lcd_intel_de_write(dev_priv, ICL_PORT_PCS_DW1_GRP(phy), val);

	/* 2. Program loadgen select */
	/*
	 * Program PORT_TX_DW4 depending on Bit rate and used lanes
	 * <= 6 GHz and 4 lanes (LN0=0, LN1=1, LN2=1, LN3=1)
	 * <= 6 GHz and 1,2 lanes (LN0=0, LN1=1, LN2=1, LN3=0)
	 * > 6 GHz (LN0=0, LN1=0, LN2=0, LN3=0)
	 */
	for (ln = 0; ln < 4; ln++) {
		loadgen = i915_icl_combo_phy_loadgen_select(crtc_state, ln);
		(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_TX_DW4_LN(ln, phy),
					    LOADGEN_SELECT,
					    loadgen);
	}

	/* 3. Set PORT_CL_DW5 SUS Clock Config to 11b */
	(void)i915_lcd_intel_de_rmw(dev_priv, ICL_PORT_CL_DW5(phy),
				    0, SUS_CLOCK_CONFIG);

	/* 4. Clear training enable to change swing values */
	val = i915_lcd_intel_de_read(dev_priv, ICL_PORT_TX_DW5_LN(0, phy));
	val &= ~TX_TRAINING_EN;
	i915_lcd_intel_de_write(dev_priv, ICL_PORT_TX_DW5_GRP(phy), val);

	/* 5. Program swing and de-emphasis */
	i915_icl_ddi_combo_vswing_program(encoder, crtc_state);

	/* 6. Set training enable to trigger update */
	val = i915_lcd_intel_de_read(dev_priv, ICL_PORT_TX_DW5_LN(0, phy));
	val |= TX_TRAINING_EN;
	i915_lcd_intel_de_write(dev_priv, ICL_PORT_TX_DW5_GRP(phy), val);
}

/* Finds the index of a DP swing and pre-emphasis pair (the Linux translate_signal_level()). */
static int
i915_translate_signal_level(
	struct intel_dp *intel_dp,
	u8 signal_levels)
{
	struct drm_i915_private *i915 __maybe_unused;
	int i;

	/* Finds the device of the port. */
	i915 = i915_lcd_dp_to_i915(intel_dp);

	/* Looks the pair up in the signal-level list. */
	for (i = 0; i < (int)ARRAY_SIZE(index_to_dp_signal_levels); i++) {
		if (index_to_dp_signal_levels[i] == signal_levels)
			return i;
	}

	/* A pair the list does not have is a warning, and index 0 is used. */
	(void)I915_LCD_DRM_WARN(&i915->drm, 1,
				"Unsupported voltage swing/pre-emphasis level: 0x%x\n",
				signal_levels);

	/* Succeeded: reports the fallback index. */
	return 0;
}

/* Powers up the lanes the crtc state uses on a combo PHY (the Linux intel_ddi_power_up_lanes()). */
static void
i915_ddi_power_up_lanes(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	struct intel_digital_port *dig_port;
	enum phy phy;
	bool is_combo;
	bool lane_reversal;

	/* Finds the device, the port object and its PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* Only a combo PHY has the static power-down field. */
	is_combo = drv_i915_phy_is_combo(i915, phy);
	if (!is_combo)
		return;

	/* Powers up the used lanes, reversed when the port's lanes are. */
	lane_reversal = dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL;
	drv_i915_combo_phy_power_up_lanes(i915, phy, false,
					  crtc_state->lane_count,
					  lane_reversal);
}

/* Programs the eDP multi-SST splitter of the pipe (the Linux intel_ddi_mso_configure()). */
static void
i915_ddi_mso_configure(
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *i915;
	enum pipe pipe;
	u32 dss1;
	int display_ver;

	/* Finds the crtc, its device and its pipe. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915 = i915_lcd_to_i915(crtc->base.dev);
	pipe = crtc->pipe;
	dss1 = 0;

	/* The splitter exists from display version 12 (the Linux HAS_MSO()). */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (!(display_ver >= 12))
		return;

	/* An enabled splitter selects its segments and overlap. */
	if (crtc_state->splitter.enable) {
		dss1 |= SPLITTER_ENABLE;
		dss1 |= OVERLAP_PIXELS(crtc_state->splitter.pixel_overlap);
		if (crtc_state->splitter.link_count == 2) {
			dss1 |= SPLITTER_CONFIGURATION_2_SEGMENT;
		} else {
			dss1 |= SPLITTER_CONFIGURATION_4_SEGMENT;
		}
	}

	/* Writes the splitter fields of the pipe. */
	(void)i915_lcd_intel_de_rmw(i915, ICL_PIPE_DSS_CTL1(pipe),
				    SPLITTER_ENABLE | SPLITTER_CONFIGURATION_MASK |
				    OVERLAP_PIXELS_MASK, dss1);
}

/* Selects the transcoder whose DP_TP registers carry the link (the Linux tgl_dp_tp_transcoder()). */
static enum transcoder
i915_tgl_dp_tp_transcoder(
	const struct intel_crtc_state *crtc_state)
{
	bool has_dp_mst;

	/* An MST stream uses the master transcoder's. */
	has_dp_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);
	if (has_dp_mst)
		return crtc_state->mst_master_transcoder;

	/* Succeeded: an SST stream uses its own transcoder's. */
	return crtc_state->cpu_transcoder;
}

/* Selects the DP_TP_CTL register of the link (the Linux dp_tp_ctl_reg()). */
static i915_reg_t
i915_dp_tp_ctl_reg(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	enum transcoder tp_transcoder;
	int display_ver;

	/* Finds the device. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* Display version 12 and later have it per transcoder. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 12) {
		tp_transcoder = i915_tgl_dp_tp_transcoder(crtc_state);
		return TGL_DP_TP_CTL(tp_transcoder);
	}

	/* Succeeded: the earlier versions have it per port. */
	return DP_TP_CTL(encoder->port);
}

/* Selects the DP_TP_STATUS register of the link (the Linux dp_tp_status_reg()). */
static i915_reg_t
i915_dp_tp_status_reg(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	enum transcoder tp_transcoder;
	int display_ver;

	/* Finds the device. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* Display version 12 and later have it per transcoder. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 12) {
		tp_transcoder = i915_tgl_dp_tp_transcoder(crtc_state);
		return TGL_DP_TP_STATUS(tp_transcoder);
	}

	/* Succeeded: the earlier versions have it per port. */
	return DP_TP_STATUS(encoder->port);
}

/*
 * Waits for a port's DDI buffer to become idle (the Linux
 * intel_wait_ddi_buf_idle(); the waits run on the entry point's device).
 */
static void
i915_wait_ddi_buf_idle(
	struct drm_i915_private *cur_i915,
	struct drm_i915_private *dev_priv,
	enum port port)
{
	int wait_result;

	/* Broxton only waits a fixed time. */
	if (IS_BROXTON(dev_priv)) {
		i915_lcd_udelay(cur_i915, 16);
		return;
	}

	/* Polls the idle bit for 8 us. */
	wait_result = I915_LCD_WAIT_FOR_US(cur_i915,
					   (i915_lcd_intel_de_read(dev_priv, DDI_BUF_CTL(port)) &
					    DDI_BUF_IS_IDLE),
					   8);
	if (wait_result) {
		I915_LCD_DRM_ERR(&dev_priv->drm, "Timeout waiting for DDI BUF %c to get idle\n",
				 port_name(port));
	}
}

/*
 * Waits for a port's DDI buffer to become active (the Linux
 * intel_wait_ddi_buf_active(); the waits run on the entry point's device).
 */
static void
i915_wait_ddi_buf_active(
	struct drm_i915_private *cur_i915,
	struct drm_i915_private *dev_priv,
	enum port port)
{
	enum phy phy;
	int timeout_us;
	int ret;
	int display_ver;
	bool is_tc;

	/* Finds the port's PHY and the display version. */
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, port);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* Wait > 518 usecs for DDI_BUF_CTL to be non idle */
	if (display_ver < 10) {
		i915_lcd_usleep_range(cur_i915, 518, 1000);
		return;
	}

	/* The time the buffer is given, by platform and PHY. */
	if (display_ver >= 14) {
		timeout_us = 10000;
	} else if (IS_DG2(dev_priv)) {
		timeout_us = 1200;
	} else if (display_ver >= 12) {
		is_tc = drv_i915_lcd_intel_phy_is_tc(dev_priv, phy);
		if (is_tc) {
			timeout_us = 3000;
		} else {
			timeout_us = 1000;
		}
	} else {
		timeout_us = 500;
	}

	/* Polls the version's idle bit until it clears. */
	if (display_ver >= 14) {
		ret = I915_LCD_WAIT_FOR_POLL(cur_i915,
					     !(i915_lcd_intel_de_read(dev_priv, XELPDP_PORT_BUF_CTL1(port)) & XELPDP_PORT_BUF_PHY_IDLE),
					     timeout_us, 10, 10);
	} else {
		ret = I915_LCD_WAIT_FOR_POLL(cur_i915,
					     !(i915_lcd_intel_de_read(dev_priv, DDI_BUF_CTL(port)) & DDI_BUF_IS_IDLE),
					     timeout_us, 10, 10);
	}

	/* A buffer that stayed idle is reported. */
	if (ret) {
		I915_LCD_DRM_ERR(&dev_priv->drm, "Timeout waiting for DDI BUF %c to get active\n",
				 port_name(port));
	}
}

/*
 * Restarts the link in training pattern 1 and enables the DDI buffer (the
 * Linux intel_ddi_prepare_link_retrain(), the prepare_link_retrain hook).
 */
static void
i915_ddi_prepare_link_retrain(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_digital_port *dig_port;
	struct intel_encoder *encoder;
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	enum port port;
	u32 dp_tp_ctl;
	u32 ddi_buf_ctl;
	bool wait;
	bool has_dp_mst;
	bool in_dp_alt_mode;
	bool in_legacy_mode;
	bool is_alderlake_p;

	/* Finds the port object, its encoder, the devices and the port number. */
	dig_port = i915_lcd_dp_to_dig_port(intel_dp);
	encoder = &dig_port->base;
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	port = encoder->port;
	wait = false;

	/* A link already running is stopped first: its DDI buffer and DP_TP_CTL are disabled. */
	dp_tp_ctl = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state));
	if (dp_tp_ctl & DP_TP_CTL_ENABLE) {
		ddi_buf_ctl = i915_lcd_intel_de_read(dev_priv, DDI_BUF_CTL(port));
		if (ddi_buf_ctl & DDI_BUF_CTL_ENABLE) {
			i915_lcd_intel_de_write(dev_priv, DDI_BUF_CTL(port),
						ddi_buf_ctl & ~DDI_BUF_CTL_ENABLE);
			wait = true;
		}

		dp_tp_ctl &= ~DP_TP_CTL_ENABLE;
		i915_lcd_intel_de_write(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state), dp_tp_ctl);
		i915_lcd_intel_de_posting_read(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state));

		if (wait)
			i915_wait_ddi_buf_idle(cur_i915, dev_priv, port);
	}

	/* Enables DP_TP_CTL in training pattern 1, in the stream's mode. */
	dp_tp_ctl = DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_PAT1;
	has_dp_mst = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST);
	if (has_dp_mst) {
		dp_tp_ctl |= DP_TP_CTL_MODE_MST;
	} else {
		dp_tp_ctl |= DP_TP_CTL_MODE_SST;
		if (crtc_state->enhanced_framing)
			dp_tp_ctl |= DP_TP_CTL_ENHANCED_FRAME_ENABLE;
	}
	i915_lcd_intel_de_write(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state), dp_tp_ctl);
	i915_lcd_intel_de_posting_read(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state));

	/* An Alder Lake-P Type-C port in DP-alt or legacy mode needs the TBT-to-DP workaround. */
	is_alderlake_p = I915_LCD_IS_ALDERLAKE_P(dev_priv);
	if (is_alderlake_p) {
		in_dp_alt_mode = intel_tc_port_in_dp_alt_mode(dig_port);
		in_legacy_mode = intel_tc_port_in_legacy_mode(dig_port);
		if (in_dp_alt_mode || in_legacy_mode)
			I915_LCD_ADLP_TBT_TO_DP_ALT_SWITCH_WA(cur_i915, encoder);
	}

	/* Enables the DDI buffer with the value computed at the pre-enable. */
	intel_dp->DP |= DDI_BUF_CTL_ENABLE;
	i915_lcd_intel_de_write(dev_priv, DDI_BUF_CTL(port), intel_dp->DP);
	i915_lcd_intel_de_posting_read(dev_priv, DDI_BUF_CTL(port));

	/* Waits for the buffer to become active. */
	i915_wait_ddi_buf_active(cur_i915, dev_priv, port);
}

/* Selects a training pattern in DP_TP_CTL (the Linux intel_ddi_set_link_train(), the set_link_train hook). */
static void
i915_ddi_set_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	u8 dp_train_pat)
{
	struct intel_encoder *encoder;
	struct drm_i915_private *dev_priv;
	u32 temp;
	u8 pattern;

	/* Finds the port's encoder and its device. */
	encoder = &i915_lcd_dp_to_dig_port(intel_dp)->base;
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* Reads DP_TP_CTL and clears its pattern field. */
	temp = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state));
	temp &= ~DP_TP_CTL_LINK_TRAIN_MASK;

	/* Sets the field of the requested pattern (without its scrambling bit). */
	pattern = intel_dp_training_pattern_symbol(dp_train_pat);
	switch (pattern) {
	case DP_TRAINING_PATTERN_DISABLE:
		temp |= DP_TP_CTL_LINK_TRAIN_NORMAL;
		break;
	case DP_TRAINING_PATTERN_1:
		temp |= DP_TP_CTL_LINK_TRAIN_PAT1;
		break;
	case DP_TRAINING_PATTERN_2:
		temp |= DP_TP_CTL_LINK_TRAIN_PAT2;
		break;
	case DP_TRAINING_PATTERN_3:
		temp |= DP_TP_CTL_LINK_TRAIN_PAT3;
		break;
	case DP_TRAINING_PATTERN_4:
		temp |= DP_TP_CTL_LINK_TRAIN_PAT4;
		break;
	}

	/* Writes DP_TP_CTL. */
	i915_lcd_intel_de_write(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state), temp);
}

/*
 * Selects the idle pattern and waits for the idle patterns to be sent (the
 * Linux intel_ddi_set_idle_link_train(), the set_idle_link_train hook).
 */
static void
i915_ddi_set_idle_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_encoder *encoder;
	struct drm_i915_private *dev_priv;
	enum port port;
	int wait_result;
	int display_ver;

	/* Finds the port's encoder, its device and its number. */
	encoder = &i915_lcd_dp_to_dig_port(intel_dp)->base;
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	port = encoder->port;

	/* Selects the idle pattern. */
	(void)i915_lcd_intel_de_rmw(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state),
				    DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_IDLE);

	/*
	 * Until TGL on PORT_A we can have only eDP in SST mode. There the only
	 * reason we need to set idle transmission mode is to work around a HW
	 * issue where we enable the pipe while not in idle link-training mode.
	 * In this case there is requirement to wait for a minimum number of
	 * idle patterns to be sent.
	 */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (port == PORT_A && display_ver < 12)
		return;

	/* Waits for the idle patterns to be sent. */
	wait_result = intel_de_wait_for_set(dev_priv,
					    i915_dp_tp_status_reg(encoder, crtc_state),
					    DP_TP_STATUS_IDLE_DONE, 1);
	if (wait_result) {
		I915_LCD_DRM_ERR(&dev_priv->drm,
				 "Timed out waiting for DP idle patterns\n");
	}
}

/* Disables FEC in DP_TP_CTL when the stream used it (the Linux intel_ddi_disable_fec()). */
static void
i915_ddi_disable_fec(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;

	/* Finds the device. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* A stream without FEC has nothing to disable. */
	if (!crtc_state->fec_enable)
		return;

	/* Clears the FEC enable and flushes the write. */
	(void)i915_lcd_intel_de_rmw(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state),
				    DP_TP_CTL_FEC_ENABLE, 0);
	i915_lcd_intel_de_posting_read(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state));
}

/*
 * Disables the DDI buffer and the link's DP_TP_CTL before display version
 * 14 (the Linux disable_ddi_buf()).
 */
static void
i915_disable_ddi_buf_ctl(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	enum port port;
	bool wait;
	bool has_dp_encoder;
	u32 val;

	/* Finds the devices and the port number. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	port = encoder->port;
	wait = false;

	/* Disables an enabled DDI buffer; its idle state is waited for below. */
	val = i915_lcd_intel_de_read(dev_priv, DDI_BUF_CTL(port));
	if (val & DDI_BUF_CTL_ENABLE) {
		val &= ~DDI_BUF_CTL_ENABLE;
		i915_lcd_intel_de_write(dev_priv, DDI_BUF_CTL(port), val);
		wait = true;
	}

	/* A DP stream also disables DP_TP_CTL. */
	has_dp_encoder = intel_crtc_has_dp_encoder(crtc_state);
	if (has_dp_encoder) {
		(void)i915_lcd_intel_de_rmw(dev_priv, i915_dp_tp_ctl_reg(encoder, crtc_state),
					    DP_TP_CTL_ENABLE, 0);
	}

	/* Disables FEC. */
	i915_ddi_disable_fec(encoder, crtc_state);

	/* Waits for the buffer that was disabled to become idle. */
	if (wait)
		i915_wait_ddi_buf_idle(cur_i915, dev_priv, port);
}

/* Disables the DDI buffer of the display version (the Linux intel_disable_ddi_buf()). */
static void
i915_disable_ddi_buf(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	struct drm_i915_private *cur_i915;
	int display_ver;

	/* Finds the devices. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);

	/* Runs the disable of the display version. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 14) {
		I915_LCD_MTL_DISABLE_DDI_BUF(cur_i915, encoder, crtc_state);

		/* 3.f Disable DP_TP_CTL FEC Enable if it is needed */
		i915_ddi_disable_fec(encoder, crtc_state);
	} else {
		i915_disable_ddi_buf_ctl(encoder, crtc_state);
	}

	/* Waits for FEC to report disabled. */
	intel_ddi_wait_for_fec_status(encoder, crtc_state, false);
}

/*
 * Disables the transcoder function (the Linux
 * intel_ddi_disable_transcoder_func(); the quirk's sleep runs on the entry
 * point's device).
 */
static void
i915_ddi_disable_transcoder_func(
	struct drm_i915_private *cur_i915,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *dev_priv;
	enum transcoder cpu_transcoder;
	u32 ctl;
	int display_ver;
	bool mst_master;
	bool has_hdmi;

	/* Finds the crtc, its device and its transcoder. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	dev_priv = i915_lcd_to_i915(crtc->base.dev);
	cpu_transcoder = crtc_state->cpu_transcoder;
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* Display version 11 and later clear the port-sync word. */
	if (display_ver >= 11) {
		i915_lcd_intel_de_write(dev_priv,
					TRANS_DDI_FUNC_CTL2(cpu_transcoder), 0);
	}

	/* Reads the function; HDCP signalling still on is a warning. */
	ctl = i915_lcd_intel_de_read(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder));

	(void)I915_LCD_DRM_WARN_ON(crtc->base.dev, ctl & TRANS_DDI_HDCP_SIGNALLING);

	/* Clears the enable bit. */
	ctl &= ~TRANS_DDI_FUNC_ENABLE;

	/* Display versions 8 to 10 clear the port-sync fields here. */
	if (I915_LCD_IS_DISPLAY_VER(dev_priv, 8, 10)) {
		ctl &= ~(TRANS_DDI_PORT_SYNC_ENABLE |
			 TRANS_DDI_PORT_SYNC_MASTER_SELECT_MASK);
	}

	/* Deselects the port and the mode, except on an MST master transcoder. */
	if (display_ver >= 12) {
		mst_master = intel_dp_mst_is_master_trans(crtc_state);
		if (!mst_master) {
			ctl &= ~(TGL_TRANS_DDI_PORT_MASK |
				 TRANS_DDI_MODE_SELECT_MASK);
		}
	} else {
		ctl &= ~(TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK);
	}

	/* Writes the function. */
	i915_lcd_intel_de_write(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder), ctl);

	/* A quirked HDMI sink is given 100 ms with the DDI disabled. */
	if (!I915_LCD_INTEL_HAS_QUIRK(dev_priv, QUIRK_INCREASE_DDI_DISABLED_TIME))
		return;

	/* The quirk applies to an HDMI stream only. */
	has_hdmi = intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI);
	if (!has_hdmi)
		return;

	/* Reports the quirk. */
	I915_LCD_DRM_DBG_KMS(&dev_priv->drm,
			     "Quirk Increase DDI disabled time\n");

	/* Quirk time at 100ms for reliable operation */
	i915_lcd_msleep(cur_i915, 100);
}

/*
 * Sets or clears the sink's MSA_TIMING_PAR_IGNORE for a VRR stream (the
 * Linux intel_dp_sink_set_msa_timing_par_ignore_state(); the DPCD write
 * runs on the entry point's device).
 */
static void
i915_dp_sink_set_msa_timing_par_ignore_state(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	bool enable)
{
	struct drm_i915_private *i915 __maybe_unused;
	struct drm_i915_private *cur_i915;
	long written;
	u8 value;

	/* Finds the port's device and the entry point's device. */
	i915 = i915_lcd_dp_to_i915(intel_dp);
	cur_i915 = i915_ddi_cur_i915(&i915_lcd_dp_to_dig_port(intel_dp)->base);

	/* Only a VRR stream asks the sink to ignore the MSA timing. */
	if (!crtc_state->vrr.enable)
		return;

	/* Writes the sink's control byte; a failed write is only logged. */
	value = 0;
	if (enable)
		value = DP_MSA_TIMING_PAR_IGNORE_EN;
	written = I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915, &intel_dp->aux, DP_DOWNSPREAD_CTRL, value);
	if (written <= 0) {
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "Failed to %s MSA_TIMING_PAR_IGNORE in the sink\n",
				     i915_str_enable_disable(enable));
	}
}

/* Runs the DP half of the port disable before the transcoder stops (the Linux intel_disable_ddi_dp()). */
static void
i915_disable_ddi_dp(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct intel_dp *intel_dp;
	struct intel_connector *connector __maybe_unused;

	UNUSED_PARAMETER(state);

	/* Finds the DP half and the connector. */
	intel_dp = enc_to_intel_dp(encoder);
	connector = I915_LCD_TO_INTEL_CONNECTOR(old_conn_state->connector);

	/* The link is no longer trained. */
	intel_dp->link_trained = false;

	/* Disables PSR and the eDP backlight. */
	intel_psr_disable(intel_dp, old_crtc_state);
	drv_i915_edp_backlight_off(old_conn_state);

	/* Disable the decompression in DP Sink */
	intel_dp_sink_disable_decompression(state,
					    connector, old_crtc_state);

	/* Disable Ignore_MSA bit in DP Sink */
	i915_dp_sink_set_msa_timing_par_ignore_state(intel_dp, old_crtc_state,
						     false);
}

/* Runs the port disable before the transcoder stops (the Linux intel_disable_ddi(), the disable hook). */
static void
i915_disable_ddi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	bool has_hdmi;

	/* Cancels the Type-C link reset work and disables HDCP. */
	intel_tc_port_link_cancel_reset_work(i915_lcd_enc_to_dig_port(encoder));
	intel_hdcp_disable(I915_LCD_TO_INTEL_CONNECTOR(old_conn_state->connector));

	/* Runs the HDMI or the DP half. */
	has_hdmi = intel_crtc_has_type(old_crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		i915_disable_ddi_hdmi(state, encoder, old_crtc_state,
				      old_conn_state);
	} else {
		i915_disable_ddi_dp(state, encoder, old_crtc_state,
				    old_conn_state);
	}
}

/* Runs the DP half of the port disable after the transcoder stopped (the Linux intel_ddi_post_disable_dp()). */
static void
i915_ddi_post_disable_dp(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	struct intel_digital_port *dig_port;
	struct intel_dp *intel_dp;
	enum transcoder cpu_transcoder;
	intel_wakeref_t wakeref;
	int display_ver;
	bool is_mst;

	UNUSED_PARAMETER(state);

	/* Finds the devices, the port object, its DP half and whether the stream is MST. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	intel_dp = &dig_port->dp;
	is_mst = intel_crtc_has_type(old_crtc_state,
				     INTEL_OUTPUT_DP_MST);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* A single stream disables its DP infoframes. */
	if (!is_mst) {
		drv_i915_dp_set_infoframes(encoder, false,
					   old_crtc_state, old_conn_state);
	}

	/*
	 * Power down sink before disabling the port, otherwise we end
	 * up getting interrupts from the sink on detecting link loss.
	 */
	drv_i915_dp_set_power(intel_dp, DP_SET_POWER_D3);

	/* Deselects the port of an MST transcoder, or the clock of an older SST one. */
	if (display_ver >= 12) {
		if (is_mst) {
			cpu_transcoder = old_crtc_state->cpu_transcoder;

			(void)i915_lcd_intel_de_rmw(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder),
						    TGL_TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK,
						    0);
		}
	} else {
		if (!is_mst)
			i915_ddi_disable_transcoder_clock(old_crtc_state);
	}

	/* Disables the DDI buffer and the sink's FEC readiness. */
	i915_disable_ddi_buf(encoder, old_crtc_state);

	intel_dp_sink_set_fec_ready(intel_dp, old_crtc_state, false);

	/*
	 * From TGL spec: "If single stream or multi-stream master transcoder:
	 * Configure Transcoder Clock select to direct no clock to the
	 * transcoder"
	 */
	if (display_ver >= 12)
		i915_ddi_disable_transcoder_clock(old_crtc_state);

	/* Forces the panel's VDD and turns the panel's power off. */
	i915_lcd_intel_pps_vdd_on(cur_i915, intel_dp);
	i915_lcd_intel_pps_off(cur_i915, intel_dp);

	/* Gives the port's I/O power reference back. */
	wakeref = fetch_and_zero(&dig_port->ddi_io_wakeref);

	if (wakeref) {
		i915_lcd_intel_display_power_put(dev_priv,
						 dig_port->ddi_io_power_domain,
						 wakeref);
	}

	/* Gates the port clock. */
	i915_ddi_disable_clock(encoder);

	/* De-select Thunderbolt */
	if (display_ver >= 14) {
		(void)i915_lcd_intel_de_rmw(dev_priv, XELPDP_PORT_BUF_CTL1(encoder->port),
					    XELPDP_PORT_BUF_IO_SELECT_TBT, 0);
	}
}

/*
 * Stops the transcoder and runs the port disable after it (the Linux
 * intel_ddi_post_disable(), the post_disable hook).
 */
static void
i915_ddi_post_disable(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	struct drm_i915_private *cur_i915;
	struct i915_lcd_world *world;
	bool has_dp_mst;
	bool has_hdmi;
	int display_ver;

	/* Finds the devices and the world of the encoder's modeset object. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	world = i915_ddi_modeset(encoder)->world;

	/* A single stream turns the vblank off and stops the transcoder and its function. */
	has_dp_mst = intel_crtc_has_type(old_crtc_state, INTEL_OUTPUT_DP_MST);
	if (!has_dp_mst) {
		I915_LCD_INTEL_CRTC_VBLANK_OFF(world, old_crtc_state);

		drv_i915_disable_transcoder(old_crtc_state);

		i915_ddi_disable_transcoder_func(cur_i915, old_crtc_state);

		intel_dsc_disable(old_crtc_state);

		/* Display version 9 and later disable the scaler, earlier ones the panel fitter. */
		display_ver = I915_LCD_DISPLAY_VER(dev_priv);
		if (display_ver >= 9) {
			skl_scaler_disable(old_crtc_state);
		} else {
			I915_LCD_ILK_PFIT_DISABLE(cur_i915, old_crtc_state);
		}
	}

	/*
	 * XXX: Linux walks the crtcs of the big-joiner slave pipes here
	 * (for_each_intel_crtc_in_pipe_mask over the takeover registry) and
	 * turns off each slave's vblank, DSC and scaler.  The mask is
	 * intel_crtc_bigjoiner_slave_pipes(), which is the literal 0 in this
	 * environment, so the walk never ran its body; the registry is not
	 * reachable from this hook, so the walk is not written out.  The guard
	 * reports a nonzero mask should the answer ever change.
	 */
	I915_LCD_GUARD(intel_crtc_bigjoiner_slave_pipes(old_crtc_state) == 0,
		       "the big-joiner slave pipes of intel_ddi_post_disable");

	/*
	 * When called from DP MST code:
	 * - old_conn_state will be NULL
	 * - encoder will be the main encoder (ie. mst->primary)
	 * - the main connector associated with this port
	 *   won't be active or linked to a crtc
	 * - old_crtc_state will be the state of the last stream to
	 *   be deactivated on this port, and it may not be the same
	 *   stream that was activated last, but each stream
	 *   should have a state that is identical when it comes to
	 *   the DP link parameteres
	 */

	/* Runs the HDMI or the DP half. */
	has_hdmi = intel_crtc_has_type(old_crtc_state, INTEL_OUTPUT_HDMI);
	if (has_hdmi) {
		i915_ddi_post_disable_hdmi(state, encoder, old_crtc_state,
					   old_conn_state);
	} else {
		i915_ddi_post_disable_dp(state, encoder, old_crtc_state,
					 old_conn_state);
	}
}

/*
 * Gives the main link's AUX power back after the PLL is disabled (the
 * Linux intel_ddi_post_pll_disable(), the post_pll_disable hook).
 */
static void
i915_ddi_post_pll_disable(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct intel_digital_port *dig_port;
	enum phy phy;
	bool is_tc_port;

	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(old_conn_state);

	/* Finds the devices, the port object and whether its PHY is Type-C. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);
	is_tc_port = drv_i915_lcd_intel_phy_is_tc(i915, phy);

	/* Gives the AUX power reference back. */
	i915_main_link_aux_power_domain_put(dig_port, old_crtc_state);

	/* A Type-C port gives its link reference back. */
	if (is_tc_port)
		I915_LCD_INTEL_TC_PORT_PUT_LINK(cur_i915, dig_port);
}

/* The minimum voltage level of Ice Lake and display 14 (the Linux icl_ddi_min_voltage_level()). */
static int
i915_icl_ddi_min_voltage_level(
	const struct intel_crtc_state *crtc_state)
{
	/* A port clock above 594 MHz needs level 1. */
	if (crtc_state->port_clock > 594000)
		return 1;

	/* Succeeded: lower clocks need no minimum. */
	return 0;
}

/* The minimum voltage level of Jasper Lake and Elkhart Lake (the Linux jsl_ddi_min_voltage_level()). */
static int
i915_jsl_ddi_min_voltage_level(
	const struct intel_crtc_state *crtc_state)
{
	/* A port clock above 594 MHz needs level 3. */
	if (crtc_state->port_clock > 594000)
		return 3;

	/* Succeeded: lower clocks need no minimum. */
	return 0;
}

/* The minimum voltage level of display 12 and 13 (the Linux tgl_ddi_min_voltage_level()). */
static int
i915_tgl_ddi_min_voltage_level(
	const struct intel_crtc_state *crtc_state)
{
	/* A port clock above 594 MHz needs level 2. */
	if (crtc_state->port_clock > 594000)
		return 2;

	/* Succeeded: lower clocks need no minimum. */
	return 0;
}

/*
 * Selects the buffer-translation entry of an HDMI port (the Linux
 * intel_ddi_hdmi_level()): the VBT's level shift, or the table's default.
 */
static int
i915_ddi_hdmi_level(
	struct intel_encoder *encoder,
	const struct intel_ddi_buf_trans *trans)
{
	struct i915_lcd_world *world;
	int level;

	/* Finds the world of the encoder's modeset object. */
	world = i915_ddi_modeset(encoder)->world;

	/* The VBT's value for the port the DDI hooks are bound to. */
	level = I915_LCD_INTEL_BIOS_HDMI_LEVEL_SHIFT(world, encoder->devdata);
	if (level < 0)
		level = trans->hdmi_default_entry;

	/* Succeeded: reports the entry. */
	return level;
}

/* Enables the clock, I/O power and infoframes of an HDMI port (the Linux intel_ddi_pre_enable_hdmi()). */
static void
i915_ddi_pre_enable_hdmi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct intel_digital_port *dig_port;
	struct intel_hdmi *intel_hdmi;
	struct drm_i915_private *dev_priv;

	UNUSED_PARAMETER(state);

	/* Finds the port object, its HDMI half and its device. */
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	intel_hdmi = &dig_port->hdmi;
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/* Switches a DP dual-mode adaptor to TMDS and maps the port to its PLL. */
	drv_i915_dp_dual_mode_set_tmds_output(intel_hdmi, true);
	i915_ddi_enable_clock(encoder, crtc_state);

	/* Takes the port's I/O power reference. */
	(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, dig_port->ddi_io_wakeref);
	dig_port->ddi_io_wakeref = i915_lcd_intel_display_power_get(dev_priv,
								    dig_port->ddi_io_power_domain);

	/* Programs the DP mode of a Type-C PHY and the transcoder's clock. */
	icl_program_mg_dp_mode(dig_port, crtc_state);

	i915_ddi_enable_transcoder_clock(encoder, crtc_state);

	/* Writes the infoframes the stream carries. */
	dig_port->set_infoframes(encoder,
				 crtc_state->has_infoframe,
				 crtc_state, conn_state);
}

/*
 * Sets up the sink's scrambling, the signal levels and the lanes of an
 * HDMI port, then enables its DDI buffer (the Linux intel_enable_ddi_hdmi()).
 */
static void
i915_enable_ddi_hdmi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	struct intel_digital_port *dig_port;
	struct drm_connector *connector;
	enum port port;
	enum phy phy;
	u32 buf_ctl;
	u8 lane_count __maybe_unused;
	u32 port_buf;
	i915_reg_t reg;
	u32 val;
	int display_ver;
	bool scrambling_set;
	bool is_tc;
	bool is_alderlake_p;

	UNUSED_PARAMETER(state);

	/*
	 * Finds the devices, the port object, its connector, port and PHY.
	 *
	 * XXX: the connector state's connector is the modeset object's struct
	 * intel_connector, read here through struct drm_connector as the Linux
	 * text does; the two layouts differ in this environment, so the scdc
	 * capabilities and the name read by the scrambling are not those of
	 * the intel_connector's display_info.  Kept as it was.
	 */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	connector = conn_state->connector;
	port = encoder->port;
	phy = drv_i915_lcd_intel_port_to_phy(dev_priv, port);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	is_alderlake_p = I915_LCD_IS_ALDERLAKE_P(dev_priv);

	/* Sets the sink's scrambling and TMDS clock ratio; a failure is only logged. */
	scrambling_set = drv_i915_hdmi_handle_sink_scrambling(encoder, connector,
							       crtc_state->hdmi_high_tmds_clock_ratio,
							       crtc_state->hdmi_scrambling);
	if (!scrambling_set) {
		I915_LCD_DRM_DBG_KMS(&dev_priv->drm,
				     "[CONNECTOR:%d:%s] Failed to configure sink scrambling/TMDS bit clock ratio\n",
				     connector->base.id, connector->name);
	}

	/* A platform with the buffer-translation select prepares the HDMI buffers. */
	if (has_buf_trans_select(dev_priv))
		hsw_prepare_hdmi_ddi_buffers(encoder, crtc_state);

	/* e. Enable D2D Link for C10/C20 Phy */
	if (display_ver >= 14)
		mtl_ddi_enable_d2d(encoder);

	/* Programs the signal levels. */
	encoder->set_signal_levels(encoder, crtc_state);

	/* Display WA #1143: skl,kbl,cfl */
	if (display_ver == 9 && !IS_BROXTON(dev_priv)) {
		/*
		 * For some reason these chicken bits have been
		 * stuffed into a transcoder register, event though
		 * the bits affect a specific DDI port rather than
		 * a specific transcoder.
		 */
		reg = gen9_chicken_trans_reg_by_port(dev_priv, port);

		val = i915_lcd_intel_de_read(dev_priv, reg);

		if (port == PORT_E) {
			val |= DDIE_TRAINING_OVERRIDE_ENABLE |
				DDIE_TRAINING_OVERRIDE_VALUE;
		} else {
			val |= DDI_TRAINING_OVERRIDE_ENABLE |
				DDI_TRAINING_OVERRIDE_VALUE;
		}

		i915_lcd_intel_de_write(dev_priv, reg, val);
		i915_lcd_intel_de_posting_read(dev_priv, reg);

		i915_lcd_udelay(cur_i915, 1);

		if (port == PORT_E) {
			val &= ~(DDIE_TRAINING_OVERRIDE_ENABLE |
				 DDIE_TRAINING_OVERRIDE_VALUE);
		} else {
			val &= ~(DDI_TRAINING_OVERRIDE_ENABLE |
				 DDI_TRAINING_OVERRIDE_VALUE);
		}

		i915_lcd_intel_de_write(dev_priv, reg, val);
	}

	/* Powers up the used lanes. */
	i915_ddi_power_up_lanes(encoder, crtc_state);

	/* In HDMI/DVI mode, the port width, and swing/emphasis values
	 * are ignored so nothing special needs to be done besides
	 * enabling the port.
	 *
	 * On ADL_P the PHY link rate and lane count must be programmed but
	 * these are both 0 for HDMI.
	 *
	 * But MTL onwards HDMI2.1 is supported and in TMDS mode this
	 * is filled with lane count, already set in the crtc_state.
	 * The same is required to be filled in PORT_BUF_CTL for C10/20 Phy.
	 */
	buf_ctl = dig_port->saved_port_bits | DDI_BUF_CTL_ENABLE;
	if (display_ver >= 14) {
		lane_count = mtl_get_port_width(crtc_state->lane_count);
		port_buf = 0;

		port_buf |= XELPDP_PORT_WIDTH(lane_count);

		if (dig_port->saved_port_bits & DDI_BUF_PORT_REVERSAL)
			port_buf |= XELPDP_PORT_REVERSAL;

		(void)i915_lcd_intel_de_rmw(dev_priv, XELPDP_PORT_BUF_CTL1(port),
					    XELPDP_PORT_WIDTH_MASK | XELPDP_PORT_REVERSAL, port_buf);

		buf_ctl |= DDI_PORT_WIDTH(crtc_state->lane_count);

		if (display_ver >= 20)
			buf_ctl |= XE2LPD_DDI_BUF_D2D_LINK_ENABLE;
	} else if (is_alderlake_p) {
		is_tc = drv_i915_lcd_intel_phy_is_tc(dev_priv, phy);
		if (is_tc) {
			(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, !intel_tc_port_in_legacy_mode(dig_port));
			buf_ctl |= DDI_BUF_CTL_TC_PHY_OWNERSHIP;
		}
	}

	/* Enables the DDI buffer and waits for it to become active. */
	i915_lcd_intel_de_write(dev_priv, DDI_BUF_CTL(port), buf_ctl);

	i915_wait_ddi_buf_active(cur_i915, dev_priv, port);
}

/* Resets the sink's scrambling and TMDS clock ratio (the Linux intel_disable_ddi_hdmi()). */
static void
i915_disable_ddi_hdmi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *i915 __maybe_unused;
	struct drm_connector *connector;
	bool scrambling_reset;

	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(old_crtc_state);

	/* Finds the device and the connector (read as in the enable, see there). */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	connector = old_conn_state->connector;

	/* Resets the sink; a failure is only logged. */
	scrambling_reset = drv_i915_hdmi_handle_sink_scrambling(encoder, connector,
								 false, false);
	if (!scrambling_reset) {
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[CONNECTOR:%d:%s] Failed to reset sink scrambling/TMDS bit clock ratio\n",
				     connector->base.id, connector->name);
	}
}

/*
 * Disables the infoframes, the DDI buffer, the clock and the I/O power of
 * an HDMI port after the transcoder stopped (the Linux
 * intel_ddi_post_disable_hdmi()).
 */
static void
i915_ddi_post_disable_hdmi(
	struct intel_atomic_state *state,
	struct intel_encoder *encoder,
	const struct intel_crtc_state *old_crtc_state,
	const struct drm_connector_state *old_conn_state)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	struct intel_hdmi *intel_hdmi;
	intel_wakeref_t wakeref;
	int display_ver;

	UNUSED_PARAMETER(state);

	/* Finds the device, the port object and its HDMI half. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	intel_hdmi = &dig_port->hdmi;
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* Disables the infoframes. */
	dig_port->set_infoframes(encoder, false,
				 old_crtc_state, old_conn_state);

	/* Before display version 12 the transcoder clock goes first. */
	if (display_ver < 12)
		i915_ddi_disable_transcoder_clock(old_crtc_state);

	/* Disables the DDI buffer. */
	i915_disable_ddi_buf(encoder, old_crtc_state);

	/* From display version 12 the transcoder clock goes after the buffer. */
	if (display_ver >= 12)
		i915_ddi_disable_transcoder_clock(old_crtc_state);

	/* Gives the port's I/O power reference back. */
	wakeref = fetch_and_zero(&dig_port->ddi_io_wakeref);
	if (wakeref) {
		i915_lcd_intel_display_power_put(dev_priv,
						 dig_port->ddi_io_power_domain,
						 wakeref);
	}

	/* Gates the port clock and switches a DP dual-mode adaptor off TMDS. */
	i915_ddi_disable_clock(encoder);

	drv_i915_dp_dual_mode_set_tmds_output(intel_hdmi, false);
}

/*
 * Reads which pipes a port drives and whether as MST (the Linux
 * intel_ddi_get_encoder_pipes()).  The port's power domain is only read
 * while it is already on; the reference is taken on the entry point's
 * device.
 */
static void
i915_ddi_get_encoder_pipes(
	struct intel_encoder *encoder,
	u8 *pipe_mask,
	bool *is_dp_mst)
{
	struct drm_device *dev;
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	enum port port __maybe_unused;
	intel_wakeref_t wakeref;
	u32 tmp;

	/* Finds the devices and the port number. */
	dev = encoder->base.dev;
	dev_priv = i915_lcd_to_i915(dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	port = encoder->port;

	/* No pipe until one is found. */
	*pipe_mask = 0;
	*is_dp_mst = false;

	/* Reads nothing while the port's power well is off. */
	wakeref = I915_TAKEOVER_INTEL_DISPLAY_POWER_GET_IF_ENABLED(cur_i915,
								    encoder->power_domain);
	if (!wakeref)
		return;

	/* Reads the pipes from the transcoder functions. */
	i915_ddi_read_encoder_pipes(encoder, pipe_mask, is_dp_mst);

	/* A Broxton PHY that drives a pipe must have its lanes powered. */
	if (*pipe_mask && (IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv))) {
		tmp = i915_lcd_intel_de_read(dev_priv, BXT_PHY_CTL(port));
		if ((tmp & (BXT_PHY_CMNLANE_POWERDOWN_ACK |
			    BXT_PHY_LANE_POWERDOWN_ACK |
			    BXT_PHY_LANE_ENABLED)) != BXT_PHY_LANE_ENABLED) {
			I915_LCD_DRM_ERR(&dev_priv->drm,
					 "[ENCODER:%d:%s] enabled but PHY powered down? (PHY_CTL %08x)\n",
					 encoder->base.base.id, encoder->base.name, tmp);
		}
	}

	/* Gives the port's power reference back. */
	i915_lcd_intel_display_power_put(dev_priv, encoder->power_domain, wakeref);
}

/*
 * Reads the pipes of an enabled port while its power is held: the eDP
 * transcoder's input, or every powered transcoder that selects the port
 * (the middle of the Linux intel_ddi_get_encoder_pipes(), up to its `out`
 * label).
 */
static void
i915_ddi_read_encoder_pipes(
	struct intel_encoder *encoder,
	u8 *pipe_mask,
	bool *is_dp_mst)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	enum port port;
	enum pipe p;
	enum transcoder cpu_transcoder;
	unsigned int port_mask;
	unsigned int ddi_select;
	intel_wakeref_t trans_wakeref;
	u32 tmp;
	u8 mst_pipe_mask;
	int display_ver;

	/* Finds the devices, the port number and the display version. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	port = encoder->port;
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);

	/* A disabled DDI buffer drives no pipe. */
	tmp = i915_lcd_intel_de_read(dev_priv, DDI_BUF_CTL(port));
	if (!(tmp & DDI_BUF_CTL_ENABLE))
		return;

	/* Port A on a platform with the eDP transcoder: its input select names the pipe. */
	if (HAS_TRANSCODER(dev_priv, TRANSCODER_EDP) && port == PORT_A) {
		tmp = i915_lcd_intel_de_read(dev_priv,
					     TRANS_DDI_FUNC_CTL(TRANSCODER_EDP));

		switch (tmp & TRANS_DDI_EDP_INPUT_MASK) {
		default:
			I915_LCD_MISSING_CASE(tmp & TRANS_DDI_EDP_INPUT_MASK);
			fallthrough;
		case TRANS_DDI_EDP_INPUT_A_ON:
		case TRANS_DDI_EDP_INPUT_A_ONOFF:
			*pipe_mask = BIT(PIPE_A);
			break;
		case TRANS_DDI_EDP_INPUT_B_ONOFF:
			*pipe_mask = BIT(PIPE_B);
			break;
		case TRANS_DDI_EDP_INPUT_C_ONOFF:
			*pipe_mask = BIT(PIPE_C);
			break;
		}

		return;
	}

	/*
	 * Walks the pipes of the device (the Linux for_each_pipe(): the pipes
	 * of the runtime pipe mask), asking each powered transcoder whether it
	 * selects this port.
	 */
	mst_pipe_mask = 0;
	for (p = 0; p < I915_MAX_PIPES; p++) {
		if (!(I915_LCD_DISPLAY_RUNTIME_INFO(dev_priv)->pipe_mask & BIT(p)))
			continue;

		cpu_transcoder = (enum transcoder)p;

		/* A transcoder whose power well is off is not read. */
		trans_wakeref = I915_TAKEOVER_INTEL_DISPLAY_POWER_GET_IF_ENABLED(cur_i915,
										  POWER_DOMAIN_TRANSCODER(cpu_transcoder));
		if (!trans_wakeref)
			continue;

		/* The port select field and this port's value in it. */
		if (display_ver >= 12) {
			port_mask = TGL_TRANS_DDI_PORT_MASK;
			ddi_select = TGL_TRANS_DDI_SELECT_PORT(port);
		} else {
			port_mask = TRANS_DDI_PORT_MASK;
			ddi_select = TRANS_DDI_SELECT_PORT(port);
		}

		/* Reads the transcoder function and gives its power back. */
		tmp = i915_lcd_intel_de_read(dev_priv,
					     TRANS_DDI_FUNC_CTL(cpu_transcoder));
		i915_lcd_intel_display_power_put(dev_priv, POWER_DOMAIN_TRANSCODER(cpu_transcoder),
						 trans_wakeref);

		/* A transcoder that selects another port is not this port's. */
		if ((tmp & port_mask) != ddi_select)
			continue;

		/* An MST or 128b/132b transcoder marks its pipe as MST. */
		if ((tmp & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_DP_MST ||
		    (HAS_DP20(dev_priv) &&
		     (tmp & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_FDI_OR_128B132B))
			mst_pipe_mask |= BIT(p);

		*pipe_mask |= BIT(p);
	}

	/* A port that drives no pipe is logged. */
	if (!*pipe_mask) {
		I915_LCD_DRM_DBG_KMS(&dev_priv->drm,
				     "No pipe for [ENCODER:%d:%s] found\n",
				     encoder->base.base.id, encoder->base.name);
	}

	/* An SST port on several pipes keeps the first one. */
	if (!mst_pipe_mask && hweight8(*pipe_mask) > 1) {
		I915_LCD_DRM_DBG_KMS(&dev_priv->drm,
				     "Multiple pipes for [ENCODER:%d:%s] (pipe_mask %02x)\n",
				     encoder->base.base.id, encoder->base.name,
				     *pipe_mask);
		*pipe_mask = BIT(ffs(*pipe_mask) - 1);
	}

	/* MST pipes that are not all of the port's pipes conflict; otherwise report the mode. */
	if (mst_pipe_mask && mst_pipe_mask != *pipe_mask) {
		I915_LCD_DRM_DBG_KMS(&dev_priv->drm,
				     "Conflicting MST and non-MST state for [ENCODER:%d:%s] (pipe_mask %02x mst_pipe_mask %02x)\n",
				     encoder->base.base.id, encoder->base.name,
				     *pipe_mask, mst_pipe_mask);
	} else {
		*is_dp_mst = mst_pipe_mask;
	}
}

/*
 * Reports the pipe an SST port drives (the Linux intel_ddi_get_hw_state(),
 * the get_hw_state readout hook): false for an MST port or a port on no
 * pipe.
 */
static bool
i915_ddi_get_hw_state(
	struct intel_encoder *encoder,
	enum pipe *pipe)
{
	u8 pipe_mask;
	bool is_mst;

	/* Reads the port's pipes. */
	i915_ddi_get_encoder_pipes(encoder, &pipe_mask, &is_mst);

	/* An MST port, or one on no pipe, is not active as an SST port. */
	if (is_mst || !pipe_mask)
		return false;

	/* The lowest pipe of the mask. */
	*pipe = ffs(pipe_mask) - 1;

	/* Succeeded: the port is active on that pipe. */
	return true;
}

/* Reads the transcoder function of an active port into a crtc state (the Linux intel_ddi_read_func_ctl()). */
static void
i915_ddi_read_func_ctl(
	struct intel_encoder *encoder,
	struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv;
	struct drm_i915_private *cur_i915;
	struct intel_crtc *crtc;
	struct intel_digital_port *dig_port;
	enum transcoder cpu_transcoder;
	u32 temp;
	u32 flags;
	u32 dp_tp_ctl;
	int display_ver;

	/* Finds the devices, the crtc, the port object and the transcoder. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	crtc = to_intel_crtc(pipe_config->uapi.crtc);
	cpu_transcoder = pipe_config->cpu_transcoder;
	dig_port = i915_lcd_enc_to_dig_port(encoder);
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	flags = 0;

	/* The sync polarities. */
	temp = i915_lcd_intel_de_read(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder));
	if (temp & TRANS_DDI_PHSYNC) {
		flags |= DRM_MODE_FLAG_PHSYNC;
	} else {
		flags |= DRM_MODE_FLAG_NHSYNC;
	}
	if (temp & TRANS_DDI_PVSYNC) {
		flags |= DRM_MODE_FLAG_PVSYNC;
	} else {
		flags |= DRM_MODE_FLAG_NVSYNC;
	}

	pipe_config->hw.adjusted_mode.flags |= flags;

	/* The colour depth of the pipe. */
	switch (temp & TRANS_DDI_BPC_MASK) {
	case TRANS_DDI_BPC_6:
		pipe_config->pipe_bpp = 18;
		break;
	case TRANS_DDI_BPC_8:
		pipe_config->pipe_bpp = 24;
		break;
	case TRANS_DDI_BPC_10:
		pipe_config->pipe_bpp = 30;
		break;
	case TRANS_DDI_BPC_12:
		pipe_config->pipe_bpp = 36;
		break;
	default:
		break;
	}

	/* The output kind of the transcoder's mode, its lanes, link M/N and infoframes. */
	switch (temp & TRANS_DDI_MODE_SELECT_MASK) {
	case TRANS_DDI_MODE_SELECT_HDMI:
		pipe_config->has_hdmi_sink = true;

		pipe_config->infoframes.enable |=
			I915_TAKEOVER_INTEL_HDMI_INFOFRAMES_ENABLED(cur_i915, encoder, pipe_config);

		if (pipe_config->infoframes.enable)
			pipe_config->has_infoframe = true;

		if (temp & TRANS_DDI_HDMI_SCRAMBLING)
			pipe_config->hdmi_scrambling = true;
		if (temp & TRANS_DDI_HIGH_TMDS_CHAR_RATE)
			pipe_config->hdmi_high_tmds_clock_ratio = true;
		fallthrough;
	case TRANS_DDI_MODE_SELECT_DVI:
		pipe_config->output_types |= BIT(INTEL_OUTPUT_HDMI);
		if (display_ver >= 14) {
			pipe_config->lane_count =
				((temp & DDI_PORT_WIDTH_MASK) >> DDI_PORT_WIDTH_SHIFT) + 1;
		} else {
			pipe_config->lane_count = 4;
		}
		break;
	case TRANS_DDI_MODE_SELECT_DP_SST:
		if (encoder->type == INTEL_OUTPUT_EDP) {
			pipe_config->output_types |= BIT(INTEL_OUTPUT_EDP);
		} else {
			pipe_config->output_types |= BIT(INTEL_OUTPUT_DP);
		}
		pipe_config->lane_count =
			((temp & DDI_PORT_WIDTH_MASK) >> DDI_PORT_WIDTH_SHIFT) + 1;

		drv_i915_cpu_transcoder_get_m1_n1(crtc, cpu_transcoder,
						  &pipe_config->dp_m_n);
		drv_i915_cpu_transcoder_get_m2_n2(crtc, cpu_transcoder,
						  &pipe_config->dp_m2_n2);

		dp_tp_ctl = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, pipe_config));
		pipe_config->enhanced_framing = dp_tp_ctl & DP_TP_CTL_ENHANCED_FRAME_ENABLE;

		if (display_ver >= 11) {
			dp_tp_ctl = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, pipe_config));
			pipe_config->fec_enable = dp_tp_ctl & DP_TP_CTL_FEC_ENABLE;
		}

		if (dig_port->lspcon.active && intel_dp_has_hdmi_sink(&dig_port->dp)) {
			pipe_config->infoframes.enable |=
				I915_TAKEOVER_INTEL_LSPCON_INFOFRAMES_ENABLED(cur_i915, encoder, pipe_config);
		} else {
			pipe_config->infoframes.enable |=
				I915_TAKEOVER_INTEL_HDMI_INFOFRAMES_ENABLED(cur_i915, encoder, pipe_config);
		}
		break;
	case TRANS_DDI_MODE_SELECT_FDI_OR_128B132B:
		if (!HAS_DP20(dev_priv)) {
			/* FDI */
			pipe_config->output_types |= BIT(INTEL_OUTPUT_ANALOG);
			dp_tp_ctl = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, pipe_config));
			pipe_config->enhanced_framing = dp_tp_ctl & DP_TP_CTL_ENHANCED_FRAME_ENABLE;
			break;
		}
		fallthrough; /* 128b/132b */
	case TRANS_DDI_MODE_SELECT_DP_MST:
		pipe_config->output_types |= BIT(INTEL_OUTPUT_DP_MST);
		pipe_config->lane_count =
			((temp & DDI_PORT_WIDTH_MASK) >> DDI_PORT_WIDTH_SHIFT) + 1;

		if (display_ver >= 12) {
			pipe_config->mst_master_transcoder =
				REG_FIELD_GET(TRANS_DDI_MST_TRANSPORT_SELECT_MASK, temp);
		}

		drv_i915_cpu_transcoder_get_m1_n1(crtc, cpu_transcoder,
						  &pipe_config->dp_m_n);

		if (display_ver >= 11) {
			dp_tp_ctl = i915_lcd_intel_de_read(dev_priv, i915_dp_tp_ctl_reg(encoder, pipe_config));
			pipe_config->fec_enable = dp_tp_ctl & DP_TP_CTL_FEC_ENABLE;
		}

		pipe_config->infoframes.enable |=
			I915_TAKEOVER_INTEL_HDMI_INFOFRAMES_ENABLED(cur_i915, encoder, pipe_config);
		break;
	default:
		break;
	}
}

/* Reads the pixel clock of the pipe into the crtc state (the Linux ddi_dotclock_get()). */
static void
i915_ddi_dotclock_get(
	struct intel_crtc_state *pipe_config)
{
	int dotclock;

	/* CRT dotclock is determined via other means */
	if (pipe_config->has_pch_encoder)
		return;

	/* The pixel clock the link M/N and the port clock give. */
	dotclock = drv_i915_crtc_dotclock(pipe_config);
	pipe_config->hw.adjusted_mode.crtc_clock = dotclock;
}

/*
 * Reads the configuration of an active port into a crtc state (the Linux
 * intel_ddi_get_config(), the get_config readout hook).  The subsystems
 * this path does not read back are named steps on the entry point's
 * device.
 */
static void
i915_ddi_get_config(
	struct intel_encoder *encoder,
	struct intel_crtc_state *pipe_config)
{
	struct drm_i915_private *dev_priv __maybe_unused;
	struct drm_i915_private *cur_i915;
	enum transcoder cpu_transcoder;
	bool warned;
	int display_ver;

	/* Finds the devices and the transcoder. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	cpu_transcoder = pipe_config->cpu_transcoder;

	/* XXX: DSI transcoder paranoia */
	warned = I915_LCD_DRM_WARN_ON(&dev_priv->drm, transcoder_is_dsi(cpu_transcoder));
	if (warned)
		return;

	/* Reads the transcoder function and the splitter. */
	i915_ddi_read_func_ctl(encoder, pipe_config);

	I915_TAKEOVER_INTEL_DDI_MSO_GET_CONFIG(cur_i915, encoder, pipe_config);

	/* Reads whether audio is enabled. */
	pipe_config->has_audio =
		I915_TAKEOVER_INTEL_DDI_IS_AUDIO_ENABLED(cur_i915, dev_priv, cpu_transcoder);

	/* An eDP port corrects the VBT's bpp with the read-out one. */
	if (encoder->type == INTEL_OUTPUT_EDP)
		I915_TAKEOVER_INTEL_EDP_FIXUP_VBT_BPP(cur_i915, encoder, pipe_config->pipe_bpp);

	/* Reads the pixel clock. */
	i915_ddi_dotclock_get(pipe_config);

	/* A Broxton PHY reads its lane latency mask. */
	if (IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv)) {
		pipe_config->lane_lat_optim_mask =
			bxt_ddi_phy_get_lane_lat_optim_mask(encoder);
	}

	/* Computes the voltage level the read-out port clock needs. */
	drv_i915_ddi_compute_min_voltage_level(pipe_config);

	/* Reads the infoframes the firmware is sending. */
	I915_TAKEOVER_INTEL_HDMI_READ_GCP_INFOFRAME(cur_i915, encoder, pipe_config);

	I915_TAKEOVER_INTEL_READ_INFOFRAME(cur_i915, encoder, pipe_config,
					   HDMI_INFOFRAME_TYPE_AVI,
					   &pipe_config->infoframes.avi);
	I915_TAKEOVER_INTEL_READ_INFOFRAME(cur_i915, encoder, pipe_config,
					   HDMI_INFOFRAME_TYPE_SPD,
					   &pipe_config->infoframes.spd);
	I915_TAKEOVER_INTEL_READ_INFOFRAME(cur_i915, encoder, pipe_config,
					   HDMI_INFOFRAME_TYPE_VENDOR,
					   &pipe_config->infoframes.hdmi);
	I915_TAKEOVER_INTEL_READ_INFOFRAME(cur_i915, encoder, pipe_config,
					   HDMI_INFOFRAME_TYPE_DRM,
					   &pipe_config->infoframes.drm);

	/* Display version 8 and later read the port-sync configuration. */
	display_ver = I915_LCD_DISPLAY_VER(dev_priv);
	if (display_ver >= 8)
		I915_TAKEOVER_BDW_GET_TRANS_PORT_SYNC_CONFIG(cur_i915, pipe_config);

	/* Reads the DP SDPs, PSR and the audio codec. */
	I915_TAKEOVER_INTEL_READ_DP_SDP(cur_i915, encoder, pipe_config, HDMI_PACKET_TYPE_GAMUT_METADATA);
	I915_TAKEOVER_INTEL_READ_DP_SDP(cur_i915, encoder, pipe_config, DP_SDP_VSC);

	I915_TAKEOVER_INTEL_PSR_GET_CONFIG(cur_i915, encoder, pipe_config);

	I915_TAKEOVER_INTEL_AUDIO_CODEC_GET_CONFIG(cur_i915, encoder, pipe_config);
}

/*
 * Reads the state and the frequency of the port's PLL into a crtc state
 * (the Linux intel_ddi_get_clock()).
 */
static void
i915_ddi_get_clock(
	struct intel_encoder *encoder,
	struct intel_crtc_state *crtc_state,
	struct intel_shared_dpll *pll)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	enum icl_port_dpll_id port_dpll_id;
	struct icl_port_dpll *port_dpll;
	bool pll_active;
	bool warned;

	/* Finds the devices and the crtc state's default port PLL slot. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	port_dpll_id = ICL_PORT_DPLL_DEFAULT;
	port_dpll = &crtc_state->icl_port_dplls[port_dpll_id];

	/* A port without a PLL is a warning, and nothing is read. */
	warned = I915_LCD_DRM_WARN_ON(&i915->drm, !pll);
	if (warned)
		return;

	/* Reads the PLL's state; an inactive PLL is a warning. */
	port_dpll->pll = pll;
	pll_active = drv_i915_dpll_get_hw_state(i915, pll, &port_dpll->hw_state);
	(void)I915_LCD_DRM_WARN_ON(&i915->drm, !pll_active);

	/* Makes the slot the active one. */
	I915_TAKEOVER_ICL_SET_ACTIVE_PORT_DPLL(cur_i915, crtc_state, port_dpll_id);

	/* Computes the port clock of the crtc's PLL. */
	crtc_state->port_clock = drv_i915_dpll_get_freq(i915, crtc_state->shared_dpll,
							&crtc_state->dpll_hw_state);
}

/* Finds the PLL a clock register selects for a port (the Linux _icl_ddi_get_pll()). */
static struct intel_shared_dpll *
i915_icl_ddi_get_pll_reg(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clk_sel_mask,
	u32 clk_sel_shift)
{
	enum intel_dpll_id id;
	struct intel_shared_dpll *pll;

	/* Reads the PLL select field. */
	id = (i915_lcd_intel_de_read(i915, reg) & clk_sel_mask) >> clk_sel_shift;

	/* Finds the device's PLL of that id. */
	pll = drv_i915_get_shared_dpll_by_id(i915, id);

	/* Succeeded: reports the PLL (NULL when the id is not in the pool). */
	return pll;
}

/* Finds the PLL a combo PHY port selects (the Linux icl_ddi_combo_get_pll()). */
static struct intel_shared_dpll *
i915_icl_ddi_combo_get_pll(
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915;
	struct intel_shared_dpll *pll;
	enum phy phy;

	/* Finds the device and the port's PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* Reads the PHY's select field of the DDI clock register. */
	pll = i915_icl_ddi_get_pll_reg(i915, ICL_DPCLKA_CFGCR0,
				       ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_MASK(phy),
				       ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_SHIFT(phy));

	/* Succeeded: reports the PLL. */
	return pll;
}

/*
 * Reads the PLL and the configuration of a combo PHY port (the Linux
 * icl_ddi_combo_get_config()).
 *
 * XXX: never bound.  The Linux intel_ddi_init() binds this as the
 * get_config hook of a combo PHY; the readout binding here binds
 * intel_ddi_get_config() instead (the PLL is read out with the device's
 * DPLL pool), so this and the two PLL lookups above it are not reached.
 * Kept as the old tree had them.
 */
static void
i915_icl_ddi_combo_get_config(
	struct intel_encoder *encoder,
	struct intel_crtc_state *crtc_state)
{
	struct intel_shared_dpll *pll;

	/* Reads the port's PLL, then the rest of the configuration. */
	pll = i915_icl_ddi_combo_get_pll(encoder);
	i915_ddi_get_clock(encoder, crtc_state, pll);
	i915_ddi_get_config(encoder, crtc_state);
}

/*
 * Brings the port's software state in line with a read-out crtc state (the
 * Linux intel_ddi_sync_state(), the sync_state readout hook); both halves
 * are named steps on the entry point's device.
 */
static void
i915_ddi_sync_state(
	struct intel_encoder *encoder,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	enum phy phy;
	bool is_tc;
	bool has_dp_encoder;

	/* Finds the devices and the port's PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_ddi_cur_i915(encoder);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* A Type-C port sanitizes its mode. */
	is_tc = drv_i915_lcd_intel_phy_is_tc(i915, phy);
	if (is_tc) {
		I915_TAKEOVER_INTEL_TC_PORT_SANITIZE_MODE(cur_i915, i915_lcd_enc_to_dig_port(encoder),
							  crtc_state);
	}

	/* A DP stream re-reads the sink. */
	if (!crtc_state)
		return;

	has_dp_encoder = intel_crtc_has_dp_encoder(crtc_state);
	if (has_dp_encoder)
		I915_TAKEOVER_INTEL_DP_SYNC_STATE(cur_i915, encoder, crtc_state);
}

/*
 * Takes the power references a read-out active port holds (the Linux
 * intel_ddi_get_power_domains(), the get_power_domains readout hook).
 */
static void
i915_ddi_get_power_domains(
	struct intel_encoder *encoder,
	struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	bool warned;
	bool in_tbt_alt_mode;

	/* Finds the device. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);

	/*
	 * TODO: Add support for MST encoders. Atm, the following should never
	 * happen since fake-MST encoders don't set their get_power_domains()
	 * hook.
	 */
	warned = I915_LCD_DRM_WARN_ON(&dev_priv->drm,
				      intel_crtc_has_type(crtc_state, INTEL_OUTPUT_DP_MST));
	if (warned)
		return;

	/* Finds the port object. */
	dig_port = i915_lcd_enc_to_dig_port(encoder);

	/* Takes the port's I/O power outside Thunderbolt-alt mode. */
	in_tbt_alt_mode = i915_lcd_intel_tc_port_in_tbt_alt_mode(dig_port);
	if (!in_tbt_alt_mode) {
		(void)I915_LCD_DRM_WARN_ON(&dev_priv->drm, dig_port->ddi_io_wakeref);
		dig_port->ddi_io_wakeref = i915_lcd_intel_display_power_get(dev_priv,
									    dig_port->ddi_io_power_domain);
	}

	/* Takes the main link's AUX power. */
	i915_main_link_aux_power_domain_get(dig_port, crtc_state);
}

/* Tells whether a clock register leaves a port's clock ungated (the Linux _icl_ddi_is_clock_enabled()). */
static bool
i915_icl_ddi_is_clock_enabled_reg(
	struct drm_i915_private *i915,
	i915_reg_t reg,
	u32 clk_off)
{
	u32 value;

	/* Reads the clock register. */
	value = i915_lcd_intel_de_read(i915, reg);

	/* A set clock-off bit gates the clock. */
	if (value & clk_off)
		return false;

	/* Succeeded: the clock is ungated. */
	return true;
}

/*
 * Tells whether a combo PHY port's DDI clock is ungated (the Linux
 * icl_ddi_combo_is_clock_enabled(), the is_clock_enabled hook).
 */
static bool
i915_icl_ddi_combo_is_clock_enabled(
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915;
	enum phy phy;
	bool enabled;

	/* Finds the device and the port's PHY. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	phy = drv_i915_lcd_intel_port_to_phy(i915, encoder->port);

	/* Reads the PHY's clock-off bit. */
	enabled = i915_icl_ddi_is_clock_enabled_reg(i915, ICL_DPCLKA_CFGCR0,
						    ICL_DPCLKA_CFGCR0_DDI_CLK_OFF(phy));

	/* Succeeded: reports whether the clock is ungated. */
	return enabled;
}

/*
 * Reads whether the port drives the connector in its type's mode, while
 * the port's power is held (the middle of the Linux
 * intel_ddi_connector_get_hw_state(), up to its `out` label).
 */
static bool
i915_ddi_connector_read_hw_state(
	struct intel_connector *intel_connector,
	struct intel_encoder *encoder)
{
	struct drm_i915_private *dev_priv;
	enum port port;
	enum transcoder cpu_transcoder;
	enum pipe pipe;
	int type;
	u32 tmp;
	bool active;

	/* Finds the device, the connector's type and the port number. */
	dev_priv = i915_lcd_to_i915(intel_connector->base.dev);
	type = intel_connector->base.connector_type;
	port = encoder->port;
	pipe = 0;

	/* A port that drives no pipe does not drive the connector. */
	active = encoder->get_hw_state(encoder, &pipe);
	if (!active)
		return false;

	/* Port A with the eDP transcoder uses it; any other port the pipe's transcoder. */
	if (HAS_TRANSCODER(dev_priv, TRANSCODER_EDP) && port == PORT_A) {
		cpu_transcoder = TRANSCODER_EDP;
	} else {
		cpu_transcoder = (enum transcoder) pipe;
	}

	/* Reads the mode of the transcoder function. */
	tmp = i915_lcd_intel_de_read(dev_priv, TRANS_DDI_FUNC_CTL(cpu_transcoder));

	/* The mode must be the one the connector's type needs. */
	switch (tmp & TRANS_DDI_MODE_SELECT_MASK) {
	case TRANS_DDI_MODE_SELECT_HDMI:
	case TRANS_DDI_MODE_SELECT_DVI:
		if (type == DRM_MODE_CONNECTOR_HDMIA)
			return true;
		return false;

	case TRANS_DDI_MODE_SELECT_DP_SST:
		if (type == DRM_MODE_CONNECTOR_eDP)
			return true;
		if (type == DRM_MODE_CONNECTOR_DisplayPort)
			return true;
		return false;

	case TRANS_DDI_MODE_SELECT_DP_MST:
		/* if the transcoder is in MST state then
		 * connector isn't connected */
		return false;

	case TRANS_DDI_MODE_SELECT_FDI_OR_128B132B:
		if (HAS_DP20(dev_priv)) {
			/* 128b/132b */
			return false;
		}

		/* FDI */
		if (type == DRM_MODE_CONNECTOR_VGA)
			return true;
		return false;

	default:
		break;
	}

	/* Succeeded: any other mode drives no connector. */
	return false;
}
