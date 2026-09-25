/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_modeset_setup.c),
 * which carries the following notice.
 *
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 *
 * Read out the current hardware modeset state, and sanitize it to the current
 * state.
 */

/*
 * The takeover of the display the firmware left running.
 *
 * Four parts, in the order of the display start:
 *
 *   the firmware display check (N0)  what the firmware left on the display,
 *                                    read before the first display write,
 *                                    and the decision taken from it;
 *   the VGA plane                    the VGA arbiter client and the legacy
 *                                    VGA plane the firmware may have left on;
 *   the output setup                 the front of the Linux
 *                                    intel_display_driver_probe_nogem(): the
 *                                    crtc, plane, DPLL and encoder records, and
 *                                    their readout and sanitize;
 *   the takeover registry (N1)       the Linux intel_modeset_setup_hw_state()
 *                                    and intel_crtc_disable_noatomic() run on
 *                                    a registry of the device's objects: the
 *                                    readout and sanitize of the firmware's
 *                                    display, then the takeover that stops it.
 *
 * The readout, sanitize and noatomic disable of the registry are rewritten
 * from Linux v6.8.12 drivers/gpu/drm/i915/display/intel_modeset_setup.c
 * (Copyright (C) 2022 Intel Corporation, MIT license): the order of every
 * register access, power reference and hook call is that text's.  The
 * callees of subsystems this path does not port are named steps of the
 * device's backend (takeover-internal.h), never a silent success.
 *
 * Errors are positive errno values.
 */

#include "internal.h"
#include "modeset-internal.h"
#include "watermark-internal.h"
#include "takeover-internal.h"
#include "takeover.h"
#include <kern/kcrt.h>

#include "clock.h"
#include "color.h"
#include "ddi.h"
#include "dmc.h"
#include "pipe.h"
#include "power.h"
#include "scanout.h"
#include "vblank.h"
#include "vbt-parse.h"
#include "watermark.h"

#include "../ggtt.h"
#include "../mmio.h"
#include "../power.h"
#include "../sync.h"
#include "../trace.h"

#include "bootloader/include/amd64-handoff.h"
#include "drivers/platform/pcat/graphics/backend.h"

#include <drivers/pci/pci.h>
#include <hal/hal.h>
#include <kern/device-io.h>

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/platform.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The trace stage of the display noirq bring-up.
 *
 * The VGA registration was recorded under this stage number before the
 * stages lost their enum; the value is kept so the trace reads the same.
 */
#define I915_TAKEOVER_TRACE_STAGE		4U

/* The PCODE mailbox commands of the watermark latencies and the SAGV block time. */
#define I915_GEN9_PCODE_READ_MEM_LATENCY	0x6U
#define I915_GEN9_MEM_LATENCY_LEVEL_0_4_MASK	0x000000ffU
#define I915_GEN9_MEM_LATENCY_LEVEL_1_5_MASK	0x0000ff00U
#define I915_GEN9_MEM_LATENCY_LEVEL_2_6_MASK	0x00ff0000U
#define I915_GEN9_MEM_LATENCY_LEVEL_3_7_MASK	0xff000000U
#define I915_GEN12_PCODE_READ_SAGV_BLOCK_TIME_US 0x23U

/* The panel power sequencer and the South Display Engine GMBUS bases. */
#define I915_PPS_BASE				0x61200U
#define I915_PCH_DISPLAY_BASE			0xc0000U
#define I915_GMBUS0_OFF				0x5100U
#define I915_GMBUS4_OFF				0x5110U
#define I915_GMBUS_RATE_100KHZ			0U

/* The registers of every arm of intel_display_wa_apply(), not only Alder Lake-P's. */
#define I915_ILK_DPFC_CHICKEN_FBC_A		0x43224U	/* ILK_DPFC_CHICKEN(INTEL_FBC_A) */
#define I915_DPFC_CHICKEN_COMP_DUMMY_PIXEL	(1U << 14)	/* glk+ */
#define I915_CLKREQ_POLICY			0x101038U
#define I915_CLKREQ_POLICY_MEM_UP_OVRD		(1U << 1)
#define I915_ICL_DELAY_PMRSP			(1U << 22)
#define I915_GEN9_CLKGATE_DIS_5			0x46540U
#define I915_DPCE_GATING_DIS			(1U << 17)
#define I915_GEN8_CHICKEN_DCPR_1		0x46430U
#define I915_DDI_CLOCK_REG_ACCESS		(1U << 7)

/* The VGA plane control (display 5 and later) and the standard VGA ports (video/vga.h). */
#define I915_CPU_VGACNTRL			0x41000U
#define I915_VGA_DISP_DISABLE			(1U << 31)
#define I915_VGA_SEQ_I				0x3C4U	/* sequencer index */
#define I915_VGA_SEQ_D				0x3C5U	/* sequencer data */
#define I915_VGA_SR01_SCREEN_OFF		0x20U	/* SR01 bit 5: screen off */
#define I915_VGA_MIS_R				0x3CCU	/* Misc Output read */
#define I915_VGA_MIS_W				0x3C2U	/* Misc Output write */

/* The GMCH control word of the host bridge and its VGA-disable bit. */
#define I915_SNB_GMCH_CTRL			0x50U	/* display 6 and later */
#define I915_INTEL_GMCH_CTRL			0x52U	/* older */
#define I915_INTEL_GMCH_VGA_DISABLE		(1U << 1)

/* The PCI VGA display class (base 0x03, subclass 0x00). */
#define I915_PCI_CLASS_VGA_MASK			0xffff00U
#define I915_PCI_CLASS_VGA			0x030000U

/* The ICL+ shared DPLL enable registers. */
#define I915_DPLL0_ENABLE			0x46010U
#define I915_DPLL1_ENABLE			0x46014U
#define I915_TBT_PLL_ENABLE			0x46020U
#define I915_MG_PLL1_ENABLE			0x46030U
#define I915_PLL_ENABLE_BIT			(1U << 31)

/* enum intel_dpll_id, ICL naming (values preserved). */
#define I915_DPLL_ID_ICL_DPLL0			0
#define I915_DPLL_ID_ICL_DPLL1			1
#define I915_DPLL_ID_ICL_TBTPLL			2
#define I915_DPLL_ID_ICL_MGPLL1			3
#define I915_DPLL_ID_ICL_MGPLL2			4
#define I915_DPLL_ID_ICL_MGPLL3			5
#define I915_DPLL_ID_ICL_MGPLL4			6

/* The GMBUS pin indices (intel_gmbus.h). */
#define I915_GMBUS_PIN_1_BXT			1U
#define I915_GMBUS_PIN_2_BXT			2U
#define I915_GMBUS_PIN_3_BXT			3U
#define I915_GMBUS_PIN_9_TC1_ICP		9U
#define I915_GMBUS_PIN_10_TC2_ICP		10U
#define I915_GMBUS_PIN_11_TC3_ICP		11U
#define I915_GMBUS_PIN_12_TC4_ICP		12U
#define I915_GMBUS_PIN_13_TC5_TGP		13U
#define I915_GMBUS_PIN_14_TC6_TGP		14U

/* The GPIO ordinals of the ICP pin table. */
#define I915_GPIOB				1U
#define I915_GPIOC				2U
#define I915_GPIOD				3U
#define I915_GPIOJ				9U
#define I915_GPIOK				10U
#define I915_GPIOL				11U
#define I915_GPIOM				12U
#define I915_GPION				13U
#define I915_GPIOO				14U

/*
 * The VBT DVO_PORT_* codes the port mapping uses.
 *
 * XXX: the codes from port G on differ from Linux's intel_vbt_defs.h
 * (Linux: DPG 15, HDMIG 16, DPH 17, HDMIH 18, DPI 19, HDMII 20).  A VBT
 * child on port G or later is misread.  The Alder Lake-P ports A, B and
 * TC1..TC4 do not reach them.  Kept as found; the fix is a separate step.
 */
#define I915_DVO_PORT_HDMIA			0U
#define I915_DVO_PORT_HDMIB			1U
#define I915_DVO_PORT_HDMIC			2U
#define I915_DVO_PORT_HDMID			3U
#define I915_DVO_PORT_HDMIE			12U
#define I915_DVO_PORT_HDMIF			14U
#define I915_DVO_PORT_HDMIG			15U
#define I915_DVO_PORT_HDMIH			16U
#define I915_DVO_PORT_HDMII			17U
#define I915_DVO_PORT_DPA			10U
#define I915_DVO_PORT_DPB			7U
#define I915_DVO_PORT_DPC			8U
#define I915_DVO_PORT_DPD			9U
#define I915_DVO_PORT_DPE			11U
#define I915_DVO_PORT_DPF			13U
#define I915_DVO_PORT_DPG			16U
#define I915_DVO_PORT_DPH			20U
#define I915_DVO_PORT_DPI			21U

/* The VBT child device types the output setup decides on. */
#define I915_DEVICE_TYPE_TMDS_DVI_SIGNALING	(1U << 4)
#define I915_DEVICE_TYPE_NOT_HDMI_OUTPUT	(1U << 11)
#define I915_DEVICE_TYPE_DISPLAYPORT_OUTPUT	(1U << 2)
#define I915_DEVICE_TYPE_MIPI_OUTPUT		(1U << 10)

/* The DDI buffer, the combo and Type-C clock selects. */
#define I915_DDI_BUF_CTL_A			0x64000U
#define I915_DDI_BUF_CTL_ENABLE			(1U << 31)
#define I915_ICL_DPCLKA_CFGCR0			0x164280U
#define I915_DDI_CLK_SEL_MASK			0xf0000000U
#define I915_DDI_CLK_SEL_NONE			0x00000000U
#define I915_PORT_CLK_SEL_A			0x46100U

/*
 * The first Type-C port whose TC_CLK_OFF bit restarts at bit 21: TC ports
 * 1..3 are bits 12..14, TC port 4 and later are bits 21 and up.
 */
#define I915_TC_PORT_4				3

/* The transcoder, pipe and plane registers the readout reads (stride 0x1000). */
#define I915_TRANSCONF_A			0x70008U
#define I915_TRANSCONF_ENABLE			(1U << 31)
#define I915_TRANS_DDI_FUNC_CTL_A		0x60400U
#define I915_TRANS_DDI_FUNC_ENABLE		(1U << 31)
#define I915_TRANS_DDI_EDP_INPUT_MASK		(7U << 12)
#define I915_TRANS_DDI_EDP_INPUT_A_ON		(0U << 12)
#define I915_TRANS_DDI_EDP_INPUT_A_ONOFF	(4U << 12)
#define I915_TRANS_DDI_EDP_INPUT_B_ONOFF	(5U << 12)
#define I915_TRANS_DDI_EDP_INPUT_C_ONOFF	(6U << 12)
#define I915_TRANS_DDI_EDP_INPUT_D_ONOFF	(7U << 12)
#define I915_TRANS_HTOTAL_A			0x60000U
#define I915_TRANS_VTOTAL_A			0x6000cU
#define I915_PIPESRC_A				0x6001cU
#define I915_PLANE_CTL_1_A			0x70180U
#define I915_PLANE_STRIDE_1_A			0x70188U
#define I915_PLANE_SIZE_1_A			0x70190U
#define I915_PLANE_SURF_1_A			0x7019cU
#define I915_PLANE_CTL_ENABLE			(1U << 31)
#define I915_PIPE_STRIDE			0x1000U

/* enum transcoder: A..D map 1:1 to the pipes, then EDP, DSI_0, DSI_1. */
#define I915_TRANSCODER_EDP			4
#define I915_TRANSCODER_DSI_0			5
#define I915_TRANSCODER_DSI_1			6

/* The pipe DMC, the FBC and the CMTG registers the sanitize writes. */
#define I915_PIPEDMC_CONTROL_A			0x45250U	/* stride 4 */
#define I915_PIPEDMC_ENABLE			(1U << 0)
#define I915_ILK_DPFC_CONTROL_A			0x43208U
#define I915_ILK_DPFC_CONTROL_B			0x43248U
#define I915_DPFC_CTL_EN			(1U << 31)
#define I915_TRANS_CMTG_CHICKEN			0x6fa90U

/*
 * The gating bit of the ADL-P A0 CMTG workaround.
 *
 * XXX: Linux v6.8 defines DISABLE_DPT_CLK_GATING as REG_BIT(1), and so does
 * the register table of the modeset environment; this bit 22 is what the
 * workaround has always written.  Kept as found; the fix is a separate step.
 */
#define I915_DISABLE_DPT_CLK_GATING		(1U << 22)

/* The registers N0 records on the pipe A power domain. */
#define I915_HSW_PWR_WELL_CTL2			0x45404U
#define I915_PP_STATUS				0xc7200U
#define I915_PP_CONTROL				0xc7204U
#define I915_BLC_PWM_CTL			0xc8250U
#define I915_BLC_PWM_DUTY			0xc8258U

/* The GPU's VT-d unit, reached through the MCHBAR mirror. */
#define I915_MCHBAR_MIRROR			0x140000U
#define I915_GFXVTBAR_LO			(I915_MCHBAR_MIRROR + 0x5400U)	/* bit 0 enable */
#define I915_VTD_VER				0x00U	/* version: non-zero on a real unit */
#define I915_VTD_GSTS				0x1cU	/* bit 31 TES translation enabled */
#define I915_VTD_PMEN				0x64U	/* bit 0 EPM (enable), bit 31 PRS (status) */
#define I915_VTD_WINDOW				0x1000U

/*
 * One row of the Alder Lake-P shared DPLL table (adlp_plls[]).
 *
 * The rows are copied into the DPLL records when the manager is created.
 */
struct i915_adlp_pll_desc {
	const char *name;
	int id;
	int funcs;
	uint32_t reg;
};

/*
 * One pin of the ICP GMBUS pin table (gmbus_pins_icp[]).
 *
 * The pin number is the index of the pin record it fills.
 */
struct i915_gmbus_pin_desc {
	unsigned pin;
	const char *name;
	unsigned gpio;
};

/*
 * One port of the VBT DVO port mapping: the DDI port, and the HDMI and DP
 * DVO codes that name it.
 */
struct i915_dvo_port_map {
	int port;
	uint8_t hdmi;
	uint8_t dp;
};

static struct drm_i915_private *i915_takeover_cur_i915(struct i915_takeover_world *takeover);
static struct i915_lcd_wm_ctx *i915_takeover_wm(struct i915_takeover_world *takeover);
static int i915_takeover_reg_trace(const struct i915_takeover_world *takeover);
static struct i915_takeover_world *i915_takeover_of_device(const struct drm_device *dev);
static bool i915_n1_plane_get_hw_state(struct intel_plane *plane, enum pipe *pipe);
static void i915_n1_build_device(struct i915_takeover_world *takeover, const struct i915_lcd_modeset_cfg *cfg, struct i915_lcd_emit *ops);
static void i915_n1_fill_report(struct i915_takeover_world *takeover, struct i915_n1_report *out);
static bool i915_crtc_needs_link_reset(struct i915_takeover_world *takeover, struct intel_crtc *crtc);
static u8 i915_get_bigjoiner_slave_pipes(struct i915_takeover_world *takeover, struct drm_i915_private *i915, u8 master_pipes_mask);
static void i915_get_portsync_pipes(struct i915_takeover_world *takeover, struct intel_crtc *crtc, u8 *master_pipe_mask, u8 *slave_pipes_mask);
static u8 i915_get_transcoder_pipes(struct i915_takeover_world *takeover, struct drm_i915_private *i915, u8 transcoder_mask);
static void i915_crtc_disable_noatomic_begin(struct i915_takeover_world *takeover, struct intel_crtc *crtc, struct drm_modeset_acquire_ctx *ctx);
static void i915_crtc_disable_noatomic_complete(struct i915_takeover_world *takeover, struct intel_crtc *crtc);
static void i915_crtc_disable_noatomic(struct i915_takeover_world *takeover, struct intel_crtc *crtc, struct drm_modeset_acquire_ctx *ctx);
static void i915_set_encoder_for_connector(struct intel_connector *connector, struct intel_encoder *encoder);
static void i915_reset_encoder_connector_state(struct i915_takeover_world *takeover, struct intel_encoder *encoder);
static void i915_reset_crtc_encoder_state(struct i915_takeover_world *takeover, struct intel_crtc *crtc);
static void i915_modeset_update_connector_atomic_state(struct i915_takeover_world *takeover, struct drm_i915_private *i915);
static void i915_crtc_copy_hw_to_uapi_state(struct intel_crtc_state *crtc_state);
static void i915_sanitize_plane_mapping(struct i915_takeover_world *takeover, struct drm_i915_private *i915);
static bool i915_crtc_has_encoders(struct i915_takeover_world *takeover, struct intel_crtc *crtc);
static struct intel_connector *i915_encoder_find_connector(struct i915_takeover_world *takeover, struct intel_encoder *encoder);
static void i915_sanitize_fifo_underrun_reporting(struct i915_takeover_world *takeover, const struct intel_crtc_state *crtc_state);
static bool i915_has_bogus_dpll_config(const struct intel_crtc_state *crtc_state);
static bool i915_sanitize_crtc(struct i915_takeover_world *takeover, struct intel_crtc *crtc, struct drm_modeset_acquire_ctx *ctx);
static void i915_sanitize_all_crtcs(struct i915_takeover_world *takeover, struct drm_i915_private *i915, struct drm_modeset_acquire_ctx *ctx);
static void i915_sanitize_encoder(struct i915_takeover_world *takeover, struct intel_encoder *encoder);
static void i915_readout_plane_state(struct i915_takeover_world *takeover, struct drm_i915_private *i915);
static void i915_modeset_readout_hw_state(struct i915_takeover_world *takeover, struct drm_i915_private *i915);
static void i915_get_encoder_power_domains(struct i915_takeover_world *takeover, struct drm_i915_private *i915);
static void i915_early_display_was(struct drm_i915_private *i915);
static void i915_modeset_setup_hw_state(struct i915_takeover_world *takeover, struct drm_i915_private *i915, struct drm_modeset_acquire_ctx *ctx);
static unsigned i915_popcount(unsigned value);
static void i915_nogem_write(struct i915_display_nogem *d, struct i915_mmio *m, uint32_t reg, uint32_t value);
static uint32_t i915_nogem_rmw(struct i915_display_nogem *d, struct i915_mmio *m, uint32_t reg, uint32_t clear, uint32_t set);
static uint32_t i915_sagv_block_time(int display_ver, struct mutex *sb_lock, struct i915_mmio *m);
static void i915_sagv_init(struct i915_display_nogem *d, int display_ver, struct mutex *sb_lock, struct i915_mmio *m, struct i915_bw_state *bw);
static int i915_gmbus_setup(struct i915_display_nogem *d, struct i915_mmio *m);
static uint32_t i915_icl_dpclka_ddi_clk_off(int phy);
static uint32_t i915_icl_dpclka_tc_clk_off(int tc_port);
static int i915_ddi_lanes_domain(int display_ver, int port);
static int i915_port_in_use(const struct i915_display_nogem *d, int port);
static void i915_ddi_skip(struct i915_display_nogem *d, int port, int reason);
static void i915_ddi_init(struct i915_display_nogem *d, int display_ver, unsigned port_mask, const struct i915_vbt_child *child);
static uint32_t i915_trans_reg(unsigned trans, uint32_t reg_a);
static int i915_trans_power_on(struct i915_power_domains *pd, struct i915_pw_ctx *pwc, unsigned trans);
static void i915_get_transcoder_timings(struct i915_crtc_state *cs, unsigned trans, struct i915_mmio *m);
static int i915_nogem_hsw_get_pipe_config(struct i915_display_nogem *d, int display_ver, struct i915_crtc *crtc, struct i915_mmio *m, struct i915_power_domains *pd, struct i915_pw_ctx *pwc);
static void i915_nogem_readout_plane_state(struct i915_display_nogem *d, struct i915_mmio *m, struct i915_power_domains *pd, struct i915_pw_ctx *pwc);
static void i915_nogem_early_display_was(struct i915_display_nogem *d, struct i915_mmio *m, int display_ver);
static void i915_nogem_fbc_sanitize(struct i915_display_nogem *d, struct i915_mmio *m, unsigned fbc_mask);
static void i915_nogem_sanitize_encoder_pll_mapping(struct i915_display_nogem *d, struct i915_encoder *e, struct i915_mmio *m);
static int i915_nogem_sanitize_crtc(struct i915_display_nogem *d, struct i915_crtc *crtc);
static void i915_nogem_adlp_cmtg_clock_gating_wa(struct i915_display_nogem *d, struct i915_mmio *m, int display_ver, int display_step, const struct i915_dpll *pll);
static void i915_nogem_power_domains_sanitize_state(struct i915_display_nogem *d, struct i915_power_domains *pd, struct i915_pw_ctx *pwc);
static int i915_vga_client_register(struct i915_vga_client *c);
static int i915_vga_get_legacy_io(struct i915_display *display);
static unsigned char i915_vga_in8(struct i915_display *display, unsigned short port);
static void i915_vga_out8(struct i915_display *display, unsigned short port, unsigned char value);
static void i915_vga_put_legacy_io(struct i915_display *display);
static int i915_cpu_hypervisor(void);
static void i915_read_opregion(struct i915_display *display, const struct i915_native_deps *d, struct i915_native_report *r);
static void i915_read_vtd(const struct i915_native_deps *d, struct i915_native_report *r);
static void i915_read_fb(const struct i915_native_deps *d, struct i915_native_report *r);
static void i915_read_display(const struct i915_native_deps *d, struct i915_native_report *r);
static const char *i915_opvbt_source_name(int src);
static const char *i915_opvbt_observed_name(int src);
static const char *i915_vbt_source_name(int parser_src);
static void i915_sha_prefix(const uint8_t *sha256, char *text);

/*
 * Allocates the takeover world of a display.
 *
 * The world holds the takeover registry, which was file-scope state and
 * therefore zero at load; the allocation is zeroed for the same start.
 */
int
drv_i915_takeover_world_create(
	struct i915_display *display)
{
	struct i915_takeover_world *takeover;

	/* Allocates the world zeroed: the registry starts empty and not live. */
	takeover = kern_calloc(1, sizeof(*takeover));
	if (takeover == NULL)
		return ENOMEM;

	/* Links the world to its display, whose register-trace switch the walks read. */
	takeover->display = display;

	/* Publishes the world to the display. */
	display->takeover_world = takeover;

	/* Succeeded: the display owns an empty takeover world. */
	return 0;
}

/*
 * Frees the takeover world of a display.
 *
 * Nothing may walk the registry afterwards; a display without a world is
 * left as it is.
 */
void
drv_i915_takeover_world_destroy(
	struct i915_display *display)
{
	/* A display whose world was never created has nothing to free. */
	if (display->takeover_world == NULL)
		return;

	/* Frees the world and forgets it. */
	kern_free(display->takeover_world);
	display->takeover_world = NULL;
}

/*
 * Reports the registry's crtc at an index (the walk over the device's crtcs).
 *
 * NULL past the last pipe and whenever the registry is not live.  While the
 * register trace is on, every step of a walk is printed: on a machine whose
 * only console is the screen that is how a readout is followed.
 */
struct intel_crtc *
drv_i915_n1_crtc_at(
	struct i915_takeover_world *takeover,
	unsigned idx)
{
	int trace;

	/* Prints the step of the walk while the register trace is on. */
	trace = i915_takeover_reg_trace(takeover);
	if (trace != 0) {
		kern_logf("i915: N1 walk crtc %u (plane0 hook 0x%llx)\n",
			  idx,
			  (unsigned long long)(uintptr_t)takeover->n1.plane[0].get_hw_state);
	}

	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Answers nothing past the last pipe. */
	if (idx >= (unsigned)I915_N1_PIPES)
		return NULL;

	/* Succeeded: reports the crtc of the pipe. */
	return &takeover->n1.crtc[idx];
}

/*
 * Reports the registry's crtc of a pipe (the readout's intel_crtc_for_pipe()).
 *
 * NULL for a pipe the registry does not hold and whenever it is not live.
 */
struct intel_crtc *
drv_i915_n1_crtc_for_pipe(
	struct i915_takeover_world *takeover,
	enum pipe pipe)
{
	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Answers nothing for a pipe outside the registry. */
	if ((unsigned)pipe >= (unsigned)I915_N1_PIPES)
		return NULL;

	/* Succeeded: reports the crtc of the pipe. */
	return &takeover->n1.crtc[pipe];
}

/*
 * Reports the registry's plane at an index (the walk over the device's planes).
 *
 * NULL past the last pipe and whenever the registry is not live; the step
 * is printed while the register trace is on.
 */
struct intel_plane *
drv_i915_n1_plane_at(
	struct i915_takeover_world *takeover,
	unsigned idx)
{
	int trace;

	/* Prints the step of the walk, with where the registry and its first plane hook are. */
	trace = i915_takeover_reg_trace(takeover);
	if (trace != 0) {
		kern_logf("i915: N1 walk plane %u | &n1=0x%llx &plane0=0x%llx hook=0x%llx live=%d\n",
			  idx,
			  (unsigned long long)(uintptr_t)&takeover->n1,
			  (unsigned long long)(uintptr_t)&takeover->n1.plane[0],
			  (unsigned long long)(uintptr_t)takeover->n1.plane[0].get_hw_state,
			  takeover->n1.live);
	}

	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Answers nothing past the last pipe. */
	if (idx >= (unsigned)I915_N1_PIPES)
		return NULL;

	/* Succeeded: reports the primary plane of the pipe. */
	return &takeover->n1.plane[idx];
}

/*
 * Reports the primary plane of pipe A (the one plane a plane-mask walk visits).
 *
 * NULL whenever the registry is not live.
 */
struct drm_plane *
drv_i915_n1_primary_plane(
	struct i915_takeover_world *takeover)
{
	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Succeeded: reports the primary plane pipe A's crtc names. */
	return takeover->n1.crtc[0].base.primary;
}

/*
 * Reports the registry's encoder at an index (the walk over the device's encoders).
 *
 * The registry holds one encoder, at index 0: the encoder of the screen the
 * DDI callers are bound to.  NULL for any other index and whenever the
 * registry is not live; the step is printed while the register trace is on.
 */
struct intel_encoder *
drv_i915_n1_encoder_at(
	struct i915_takeover_world *takeover,
	unsigned idx)
{
	int trace;

	/* Prints the step of the walk while the register trace is on. */
	trace = i915_takeover_reg_trace(takeover);
	if (trace != 0)
		kern_logf("i915: N1 walk encoder %u\n", idx);

	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Answers nothing past the one encoder. */
	if (idx != 0u)
		return NULL;

	/* Succeeded: reports the bound screen's encoder. */
	return takeover->n1.encoder;
}

/*
 * Reports the registry's connector at an index (the walk over the device's connectors).
 *
 * The registry holds one connector, at index 0.  NULL for any other index
 * and whenever the registry is not live; the step is printed while the
 * register trace is on.
 */
struct intel_connector *
drv_i915_n1_connector_at(
	struct i915_takeover_world *takeover,
	unsigned idx)
{
	int trace;

	/* Prints the step of the walk while the register trace is on. */
	trace = i915_takeover_reg_trace(takeover);
	if (trace != 0)
		kern_logf("i915: N1 walk connector %u\n", idx);

	/* Answers nothing while the registry is not built. */
	if (!takeover->n1.live)
		return NULL;

	/* Answers nothing past the one connector. */
	if (idx != 0u)
		return NULL;

	/* Succeeded: reports the bound screen's connector. */
	return takeover->n1.connector;
}

/*
 * Reports the state of a named crtc.
 *
 * While the registry is live the answer is the registry's state of that
 * crtc.  Outside a readout the registry is empty and the answer is the
 * state the watermark path is working on (NULL when there is none) -- the
 * meaning the watermark layer gave this accessor before the takeover
 * existed.
 */
struct intel_crtc_state *
drv_i915_n1_crtc_state(
	struct i915_takeover_world *takeover,
	struct i915_lcd_wm_ctx *wm,
	const struct intel_crtc *crtc)
{
	unsigned i;

	/* Looks for the crtc among the registry's while it is live. */
	if (takeover->n1.live) {
		for (i = 0u; i < (unsigned)I915_N1_PIPES; i++) {
			if (&takeover->n1.crtc[i] == crtc)
				return &takeover->n1.crtc_state[i];
		}
	}

	/* Without a watermark context there is no state to answer with. */
	if (wm == NULL)
		return NULL;

	/* Succeeded: reports the state the watermark path is working on. */
	return wm->crtc_state;
}

/*
 * Reports the crtc state of a crtc in the takeover's atomic state.
 *
 * The Linux intel_atomic_get_crtc_state() adds the crtc to the atomic
 * state; the disable that follows reads it back as the OLD state (the
 * takeover disables exactly what the readout found).  Outside a readout
 * this only answers with the state.
 */
struct intel_crtc_state *
drv_i915_n1_atomic_crtc_state(
	struct i915_takeover_world *takeover,
	struct i915_lcd_wm_ctx *wm,
	void *state,
	struct intel_crtc *crtc)
{
	struct intel_atomic_state *atomic_state;
	struct intel_crtc_state *crtc_state;

	/*
	 * The callers hand either the state or its base, which is its first
	 * member: the same address.
	 */
	atomic_state = state;

	/* Finds the state of the crtc. */
	crtc_state = drv_i915_n1_crtc_state(takeover, wm, crtc);

	/*
	 * During a readout the state becomes the disable's old state; a disable
	 * has no new state, as this path builds one elsewhere.
	 */
	if (takeover->n1.live && atomic_state != NULL) {
		atomic_state->old_crtc_state = crtc_state;
		atomic_state->crtc_state = NULL;
	}

	/* Succeeded: reports the crtc's state. */
	return crtc_state;
}

/*
 * Resets and reports the registry's throw-away atomic state.
 *
 * The takeover's noatomic disable builds one atomic state per crtc; this
 * path keeps a single one in the registry and resets it for each use.
 */
struct intel_atomic_state *
drv_i915_n1_atomic_state(
	struct i915_takeover_world *takeover,
	struct drm_device *dev)
{
	/* Resets the state for a new use on the named device. */
	takeover->n1.state.base.dev = dev;
	takeover->n1.state.crtc_state = NULL;
	takeover->n1.state.old_crtc_state = NULL;
	takeover->n1.state.acquire_ctx = NULL;
	takeover->n1.state.internal = false;

	/* The encoder walks of the state find the display's modeset world through it. */
	takeover->n1.state.world = takeover->display->lcd_world;

	/* Succeeded: reports the registry's state. */
	return &takeover->n1.state;
}

/*
 * Takes a display power reference only where the well is already on.
 *
 * A readout never turns a well on: 0 means the well is off (or the device
 * has no backend).  A model backend keeps no well state, so its wells
 * answer as the Linux "already on".
 *
 * XXX: the Linux text printed "is it already on?" here while the register
 * trace was on; the switch lives in the display, which this accessor does
 * not reach (it names only the device), so that trace line is not printed.
 */
intel_wakeref_t
drv_i915_n1_power_get_if_enabled(
	struct drm_i915_private *i915,
	enum intel_display_power_domain domain)
{
	intel_wakeref_t wakeref;

	/* A device without a backend holds no well. */
	if (i915 == NULL)
		return 0;
	if (i915->emit == NULL)
		return 0;

	/* A backend with well state takes the reference only where the well is on. */
	if (i915->emit->power_get_if_enabled != NULL) {
		wakeref = i915->emit->power_get_if_enabled(i915->emit->ctx, (int)domain);
		return wakeref;
	}

	/* A model backend without any power hook answers "already on". */
	if (i915->emit->power_get == NULL)
		return 1;

	/* A model backend takes the reference as an ordinary get. */
	wakeref = i915->emit->power_get(i915->emit->ctx, (int)domain);

	/* Succeeded: reports the model's cookie. */
	return wakeref;
}

/*
 * Adds a domain to a readout's set when its well is already on.
 *
 * The reference taken is recorded in the set's mask, which the takeover
 * gives back as a whole.
 */
void
drv_i915_n1_power_get_in_set_if_enabled(
	struct drm_i915_private *i915,
	struct intel_display_power_domain_set *set,
	enum intel_display_power_domain domain)
{
	intel_wakeref_t wakeref;

	/* A readout without a set records nothing. */
	if (set == NULL)
		return;

	/* Takes the reference only where the well is on. */
	wakeref = drv_i915_n1_power_get_if_enabled(i915, domain);
	if (wakeref == 0)
		return;

	/* Records the reference in the set. */
	i915_set_bit((unsigned)domain, set->mask.bits);
}

/*
 * Gives back every reference a readout's set holds.
 *
 * The set's own mask names the references; a missing device or set gives
 * nothing back.
 */
void
drv_i915_n1_power_put_all_in_set(
	struct drm_i915_private *i915,
	struct intel_display_power_domain_set *set)
{
	/* Without a device or a set there is nothing to give back. */
	if (i915 == NULL)
		return;
	if (set == NULL)
		return;

	/* Gives back the references the set's mask names. */
	drv_i915_display_power_put_mask_in_set(i915, set, &set->mask);
}

/*
 * Reads out the display the firmware left running, and sanitizes it.
 *
 * This is the Linux intel_modeset_setup_hw_state() on the takeover
 * registry: a device view built from the same configuration the modeset
 * object is built from, one crtc and primary plane per pipe, and the
 * encoder and connector of the screen the DDI callers are bound to.
 * Nothing of the firmware's picture is disturbed here; the takeover is a
 * separate step.  The registry stays live until drv_i915_n1_release().
 *
 * Returns 0, EINVAL for a missing argument, EBUSY when a readout is
 * already live, or ENXIO when no screen is bound (the readout has no
 * encoder to ask).  On success *out describes what the readout found.
 */
int
drv_i915_n1_readout(
	struct i915_display *display,
	const struct i915_lcd_modeset_cfg *cfg,
	struct i915_lcd_emit *ops,
	struct i915_n1_report *out)
{
	struct i915_takeover_world *takeover;
	struct i915_lcd_world *lcd;
	struct i915_n1_registry *n1;
	struct drm_i915_private *saved;

	/* Refuses a readout without its configuration, backend or report. */
	if (cfg == NULL)
		return EINVAL;
	if (ops == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;

	/* Resolves the registry and the modeset world the readout works in. */
	takeover = display->takeover_world;
	lcd = display->lcd_world;
	n1 = &takeover->n1;

	/* Refuses a second readout while one is live. */
	if (n1->live)
		return EBUSY;

	/* Builds the device view and the per-pipe objects. */
	i915_n1_build_device(takeover, cfg, ops);

	/* Takes the encoder and connector of the bound screen: without them the readout has nothing to ask. */
	n1->encoder = drv_i915_lcd_ms_bound_encoder(lcd);
	n1->connector = drv_i915_lcd_ms_bound_connector(lcd);
	if (n1->encoder == NULL)
		return ENXIO;
	if (n1->connector == NULL)
		return ENXIO;

	/* Attaches the connector to the registry's device, as the readout's only connector. */
	n1->connector->base.dev = &n1->i915.drm;
	n1->connector->base.state = &n1->conn_state;
	n1->connector->base.index = 0u;
	n1->connector->base.connector_type = DRM_MODE_CONNECTOR_eDP;
	n1->connector->encoder = n1->encoder;
	n1->connector->get_hw_state = drv_i915_ddi_connector_get_hw_state;

	/* Attaches the encoder to the registry's device, linked to no crtc yet. */
	n1->encoder->base.dev = &n1->i915.drm;
	n1->encoder->base.crtc = NULL;

	/* Binds the encoder's readout hooks (get_hw_state, get_config, sync_state, get_power_domains). */
	drv_i915_lcd_ms_bind_readout(n1->encoder);

	/* From here the walks answer with the registry's objects. */
	n1->live = 1;

	/*
	 * Runs the readout and the sanitize with the registry's device as the
	 * current one, and restores the device that was current before.
	 */
	saved = lcd->i915_lcd_cur_i915;
	lcd->i915_lcd_cur_i915 = &n1->i915;

	i915_modeset_setup_hw_state(takeover, &n1->i915, NULL);

	lcd->i915_lcd_cur_i915 = saved;

	/* Reports what the readout found. */
	i915_n1_fill_report(takeover, out);

	/* Succeeded: the registry is live and describes the firmware's display. */
	return 0;
}

/*
 * Takes over the display the readout found: stops every active crtc.
 *
 * Every crtc the readout found active is stopped through the Linux
 * intel_crtc_disable_noatomic().  After this the firmware's picture is gone
 * and the pipe is the driver's.  Returns 0 when every active crtc was
 * stopped, EINVAL without a live registry or a report, and EBUSY when a
 * crtc is still active (out->still_active says which).
 */
int
drv_i915_n1_takeover(
	struct i915_display *display,
	struct i915_n1_report *out)
{
	struct i915_takeover_world *takeover;
	struct i915_lcd_world *lcd;
	struct i915_n1_registry *n1;
	struct drm_i915_private *saved;
	unsigned i;

	/* Resolves the registry and the modeset world. */
	takeover = display->takeover_world;
	lcd = display->lcd_world;
	n1 = &takeover->n1;

	/* Refuses a takeover without a live readout or a report. */
	if (!n1->live)
		return EINVAL;
	if (out == NULL)
		return EINVAL;

	/* The registry's device is the current one while the crtcs are stopped. */
	saved = lcd->i915_lcd_cur_i915;
	lcd->i915_lcd_cur_i915 = &n1->i915;

	/* Stops every crtc the readout found active. */
	for (i = 0u; i < (unsigned)I915_N1_PIPES; i++) {
		if (!n1->crtc_state[i].hw.active)
			continue;

		/* Counts the takeover the report shows, then stops the crtc. */
		n1->takeovers++;
		i915_crtc_disable_noatomic(takeover, &n1->crtc[i], NULL);
	}

	/* Records the pipes still active afterwards (bit n = pipe n). */
	n1->still_active = 0u;
	for (i = 0u; i < (unsigned)I915_N1_PIPES; i++) {
		if (n1->crtc[i].active) {
			/* The crtc still counts itself active. */
			n1->still_active |= 1u << i;
		} else if (n1->crtc_state[i].hw.active) {
			/* The crtc's state still says the pipe runs. */
			n1->still_active |= 1u << i;
		}
	}

	lcd->i915_lcd_cur_i915 = saved;

	/* Reports what the takeover left. */
	i915_n1_fill_report(takeover, out);

	/* Reports a pipe the takeover did not stop. */
	if (n1->still_active != 0u)
		return EBUSY;

	/* Succeeded: every crtc the firmware left running is stopped. */
	return 0;
}

/*
 * Releases the takeover registry.
 *
 * The crtc and plane objects of the readout are not the objects of a
 * modeset, and nothing may walk them afterwards.  The encoder and the
 * connector belong to the modeset object and are handed back the way they
 * were found.
 */
void
drv_i915_n1_release(
	struct i915_display *display)
{
	struct i915_n1_registry *n1;

	/* Resolves the registry. */
	n1 = &display->takeover_world->n1;

	/* A registry that is not live has nothing to release. */
	if (!n1->live)
		return;

	/* Unlinks the encoder from the readout's crtc. */
	if (n1->encoder != NULL)
		n1->encoder->base.crtc = NULL;

	/* Unlinks the connector from the readout's state and encoder. */
	if (n1->connector != NULL) {
		n1->connector->base.state = NULL;
		n1->connector->base.encoder = NULL;
	}

	/* From here the walks answer nothing. */
	n1->live = 0;
}

/*
 * Runs the front of the display probe without GEM, up to the output setup.
 *
 * It covers intel_wm_init() through intel_vga_disable() of
 * intel_display_driver_probe_nogem().  The records in d are cleared first;
 * d is display->nogem and vga is display->vga_client in the driver.  Returns
 * 0, or the errno of the first step the reference propagates one from, with
 * d->fail_where naming that step.
 */
int
drv_i915_display_nogem_front(
	struct i915_display *display,
	struct i915_display_nogem *d,
	int display_ver,
	unsigned pipe_mask,
	struct i915_mmio *m,
	struct mutex *sb_lock,
	struct i915_cdclk_dev *cd,
	struct i915_bw_state *bw,
	struct i915_vga_client *vga)
{
	unsigned pipe;
	unsigned pipe_count;
	int is_adlp;
	int error;
	int vga_result;

	/* Alder Lake-P is the display version 13 this path serves. */
	is_adlp = 0;
	if (display_ver == 13)
		is_adlp = 1;

	/* Starts from empty records. */
	kern_memset(d, 0, sizeof(*d));

	/* intel_wm_init() -> skl_wm_init() for display 9 and later. */
	i915_sagv_init(d, display_ver, sb_lock, m, bw);
	drv_i915_skl_setup_wm_latency(d, display_ver, sb_lock, m, 0 /* dram_info.wm_lv_0_adjust_needed; P2 decoded it */);

	/*
	 * intel_panel_sanitize_ssc(): LVDS SSC handling.  ADL-P has no LVDS and
	 * no PCH SSC override, so there is nothing to sanitize here.
	 */

	/* intel_pps_setup(): not PCH_SPLIT/GLK/BXT and not VLV/CHV. */
	d->pps_mmio_base = I915_PPS_BASE;

	/* Sets the GMBUS pins up (intel_gmbus_setup()). */
	error = i915_gmbus_setup(d, m);
	if (error != 0) {
		d->fail_where = "intel_gmbus_setup";
		return error;
	}

	/* Reports how many pipes the platform has. */
	pipe_count = i915_popcount(pipe_mask);
	kern_logf("i915: P5 %u display pipe%s available.\n", pipe_count, pipe_count > 1U ? "s" : "");

	/* Creates the crtc of every pipe the platform has (for_each_pipe: intel_crtc_init()). */
	for (pipe = 0U; pipe < (unsigned)I915_NOGEM_MAX_PIPES; pipe++) {
		/* Skips a pipe the platform does not have. */
		if ((pipe_mask & (1U << pipe)) == 0U)
			continue;

		/* Creates the crtc and its planes. */
		error = drv_i915_crtc_init(d, display_ver, pipe);
		if (error != 0) {
			d->fail_where = "intel_crtc_init";
			return error;
		}

		/* Counts the created crtc. */
		d->num_crtcs++;
	}

	/*
	 * intel_plane_possible_crtcs_init(): each plane's possible_crtcs is the
	 * mask of its own CRTC.  The planes here are already per-CRTC records, so
	 * the relation is structural.
	 */

	/* Creates the shared DPLL manager. */
	drv_i915_shared_dpll_init(d, display_ver, is_adlp);

	/* intel_fdi_pll_freq_update(): IRONLAKE/SNB/IVB only -> returns. */
	/* intel_update_czclk(): VLV/CHV only -> returns. */

	/* intel_display_driver_init_hw(): reads the CDCLK the hardware runs at. */
	drv_i915_update_cdclk(cd);
	kern_logf("i915: P5 Current CDCLK: cdclk=%u vco=%u ref=%u voltage=%u\n", cd->hw.cdclk, cd->hw.vco, cd->hw.ref, (unsigned)cd->hw.voltage_level);

	/* The logical and actual CDCLK states now equal the hardware state. */
	d->cdclk_logical_set = 1;

	/* Applies the display workarounds of the platform. */
	drv_i915_display_wa_apply(d, m, display_ver, is_adlp);

	/* intel_dpll_update_ref_clks() -> icl_update_dpll_ref_clks(): no SSC reference. */
	if (d->dpll_mgr_present != 0)
		d->dpll_ref_nssc = cd->hw.ref;

	/*
	 * intel_hdcp_component_init(): needs the component framework and the GSC
	 * firmware path.  Neither exists here; recorded, not faked.
	 */
	d->hdcp_component_unimplemented = 1;

	/* Computes the maximum CDCLK when nothing set it (intel_update_max_cdclk()). */
	if (d->max_cdclk_freq == 0U)
		drv_i915_update_max_cdclk(d, display_ver, cd->hw.ref);

	/*
	 * intel_hti_init(): only when DISPLAY_INFO()->has_hti.  xe_lpd does NOT
	 * set has_hti (RKL and ADL-S do), so HDPORT_STATE is deliberately not read.
	 */
	d->hti_state_read = 0;

	/* Disables the VGA plane once at startup. */
	vga_result = drv_i915_vga_disable(display, vga, m);

	/* Records whether the plane was already off. */
	d->vga_already_disabled = 0;
	if (vga_result == 1)
		d->vga_already_disabled = 1;

	/* Records whether the plane was turned off now. */
	d->vga_disable_done = 0;
	if (vga_result == 0)
		d->vga_disable_done = 1;

	/* The front is complete. */
	d->inited = 1;

	/* Succeeded: the records are ready for the output setup. */
	return 0;
}

/*
 * Releases the crtc and DPLL records of the output setup.
 */
void
drv_i915_display_nogem_fini(
	struct i915_display_nogem *d)
{
	unsigned pipe;

	/* Marks every crtc record unused. */
	for (pipe = 0U; pipe < (unsigned)I915_NOGEM_MAX_PIPES; pipe++)
		d->crtcs[pipe].in_use = 0;

	/* Forgets the crtcs and the DPLL manager. */
	d->num_crtcs = 0U;
	d->num_dplls = 0U;
	d->dpll_mgr_present = 0;
	d->inited = 0;
}

/*
 * Adjusts the watermark latencies the PCODE reported.
 *
 * A level whose latency is 0 us disables itself and every level above it;
 * WaWmMemoryReadLatency adds the read latency to every valid level when
 * level 0 came back as 0 us; the 16 GB DIMM workaround adds 1 us to level 0.
 */
void
drv_i915_adjust_wm_latency(
	uint16_t wm[],
	int num_levels,
	int read_latency,
	int wm_lv_0_adjust_needed)
{
	int level;
	int above;

	/* Disables every level from the first one (above 0) with no latency. */
	for (level = 1; level < num_levels; level++) {
		/* A level with a latency stays. */
		if (wm[level] != 0U)
			continue;

		/* Clears every level above it and stops counting there. */
		for (above = level + 1; above < num_levels; above++)
			wm[above] = 0U;
		num_levels = level;
		break;
	}

	/*
	 * WaWmMemoryReadLatency: the punit does not account for the read latency,
	 * so add it to every valid level when level 0 came back as 0us.
	 */
	if (wm[0] == 0U) {
		for (level = 0; level < num_levels; level++)
			wm[level] = (uint16_t)(wm[level] + (uint16_t)read_latency);
	}

	/* WA Level-0 adjustment for 16GB DIMMs (SKL+). */
	if (wm_lv_0_adjust_needed != 0)
		wm[0] = (uint16_t)(wm[0] + 1U);
}

/*
 * Reads the watermark latencies from the PCODE (skl_setup_wm_latency()).
 *
 * A mailbox failure is logged and leaves the latencies unset.
 */
void
drv_i915_skl_setup_wm_latency(
	struct i915_display_nogem *d,
	int display_ver,
	struct mutex *sb_lock,
	struct i915_mmio *m,
	int wm_lv_0_adjust_needed)
{
	uint32_t val;
	uint32_t val1;
	uint32_t mult;
	int read_latency;
	int error;

	/* Display 12 and later add a read latency of 3 us, older ones 2 us. */
	read_latency = (display_ver >= 12) ? 3 : 2;

	/* IS_DG2 only doubles this. */
	mult = 1U;

	/* HAS_HW_SAGV_WM(ver >= 13 && !DGFX) -> 6 levels, otherwise 8. */
	d->wm_num_levels = (display_ver >= 13) ? 6U : 8U;

	/*
	 * Reads the first set of memory latencies [0:3] (data0 = 0).  The log
	 * shows the negative code the reference prints.
	 */
	val = 0U;
	val1 = 0U;
	error = drv_i915_pcode_read(sb_lock, m, I915_GEN9_PCODE_READ_MEM_LATENCY, &val, &val1);
	if (error != 0) {
		kern_logf("i915: SKL Mailbox read error = %d\n", -error);
		return;
	}

	/* Decodes levels 0..3, one byte each. */
	d->wm_skl_latency[0] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_0_4_MASK) >> 0) * mult);
	d->wm_skl_latency[1] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_1_5_MASK) >> 8) * mult);
	d->wm_skl_latency[2] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_2_6_MASK) >> 16) * mult);
	d->wm_skl_latency[3] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_3_7_MASK) >> 24) * mult);

	/* Reads the second set [4:7] (data0 = 1). */
	val = 1U;
	val1 = 0U;
	error = drv_i915_pcode_read(sb_lock, m, I915_GEN9_PCODE_READ_MEM_LATENCY, &val, &val1);
	if (error != 0) {
		kern_logf("i915: SKL Mailbox read error = %d\n", -error);
		return;
	}

	/* Decodes levels 4..7, one byte each. */
	d->wm_skl_latency[4] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_0_4_MASK) >> 0) * mult);
	d->wm_skl_latency[5] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_1_5_MASK) >> 8) * mult);
	d->wm_skl_latency[6] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_2_6_MASK) >> 16) * mult);
	d->wm_skl_latency[7] = (uint16_t)(((val & I915_GEN9_MEM_LATENCY_LEVEL_3_7_MASK) >> 24) * mult);

	/* Applies the level cut-off and the workarounds. */
	drv_i915_adjust_wm_latency(d->wm_skl_latency, (int)d->wm_num_levels, read_latency, wm_lv_0_adjust_needed);

	/* The latencies are now usable by the watermark code. */
	d->wm_latency_valid = 1;
}

/*
 * Creates the shared DPLL manager (intel_shared_dpll_init()).
 *
 * Only Alder Lake-P's adlp_pll_mgr is ported; display 14, DG2 and every
 * other platform leave the manager absent rather than guess a table.
 */
void
drv_i915_shared_dpll_init(
	struct i915_display_nogem *d,
	int display_ver,
	int is_alderlake_p)
{
	static const struct i915_adlp_pll_desc adlp_plls[] = {
		{ "DPLL 0",   I915_DPLL_ID_ICL_DPLL0,  I915_DPLL_FUNCS_COMBO, I915_DPLL0_ENABLE },
		{ "DPLL 1",   I915_DPLL_ID_ICL_DPLL1,  I915_DPLL_FUNCS_COMBO, I915_DPLL1_ENABLE },
		{ "TBT PLL",  I915_DPLL_ID_ICL_TBTPLL, I915_DPLL_FUNCS_TBT,   I915_TBT_PLL_ENABLE },
		{ "TC PLL 1", I915_DPLL_ID_ICL_MGPLL1, I915_DPLL_FUNCS_DKL,   I915_MG_PLL1_ENABLE + 0U },
		{ "TC PLL 2", I915_DPLL_ID_ICL_MGPLL2, I915_DPLL_FUNCS_DKL,   I915_MG_PLL1_ENABLE + 4U },
		{ "TC PLL 3", I915_DPLL_ID_ICL_MGPLL3, I915_DPLL_FUNCS_DKL,   I915_MG_PLL1_ENABLE + 8U },
		{ "TC PLL 4", I915_DPLL_ID_ICL_MGPLL4, I915_DPLL_FUNCS_DKL,   I915_MG_PLL1_ENABLE + 12U }
	};
	unsigned count;
	unsigned index;

	/* Initializes the lock of the DPLL records. */
	(void)mutex_init(&d->dpll_lock, LOCK_RANK_DEVICE, "i915-dpll");

	/*
	 * DISPLAY_VER >= 14 / DG2 have no shared DPLLs (port PLLs live in the PHY);
	 * ADL-P selects adlp_pll_mgr.  Anything else is out of scope for this port
	 * and leaves the manager absent rather than guessing a table.
	 */
	if (display_ver >= 14 || is_alderlake_p == 0) {
		d->dpll_mgr_present = 0;
		d->num_dplls = 0U;
		return;
	}

	/* Copies adlp_plls[] (DPLL0, DPLL1, TBT PLL, TC PLL 1..4) into the records. */
	count = sizeof(adlp_plls) / sizeof(adlp_plls[0]);
	for (index = 0U; index < count; index++) {
		/* Stops at the capacity of the records. */
		if (index >= (unsigned)I915_NOGEM_MAX_DPLLS)
			break;

		/* Fills one DPLL record from its table row. */
		d->dplls[index].name = adlp_plls[index].name;
		d->dplls[index].id = adlp_plls[index].id;
		d->dplls[index].funcs = adlp_plls[index].funcs;
		d->dplls[index].enable_reg = adlp_plls[index].reg;
		d->dplls[index].index = index;
	}

	/* Publishes the records and the manager. */
	d->num_dplls = index;
	d->dpll_mgr_present = 1;
}

/*
 * Creates the crtc record of one pipe with its planes (intel_crtc_init()).
 *
 * The pipe gets a primary plane, the sprites of its display version and a
 * cursor.  Returns 0, or EINVAL for a pipe beyond the records.
 */
int
drv_i915_crtc_init(
	struct i915_display_nogem *d,
	int display_ver,
	unsigned pipe)
{
	struct i915_crtc *crtc;
	unsigned count;
	unsigned sprite;
	unsigned num_sprites;

	/* Refuses a pipe beyond the records. */
	if (pipe >= (unsigned)I915_NOGEM_MAX_PIPES)
		return EINVAL;

	/* Starts the crtc from an empty record. */
	crtc = &d->crtcs[pipe];
	kern_memset(crtc, 0, sizeof(*crtc));
	crtc->pipe = pipe;

	/* num_scalers[pipe] = 2 for DISPLAY_VER >= 11. */
	crtc->num_scalers = (display_ver >= 11) ? 2U : 0U;

	/* Creates the primary plane: skl_universal_plane_create(PLANE_PRIMARY) for ver >= 9. */
	count = 0U;
	crtc->planes[count].id = 0;
	crtc->planes[count].type = I915_PLANE_PRIMARY;
	crtc->planes[count].pipe = pipe;
	crtc->planes[count].in_use = 1;
	crtc->plane_ids_mask |= 1U << (unsigned)crtc->planes[count].id;
	count++;

	/* intel_init_fifo_underrun_reporting(crtc, false) */
	crtc->fifo_underrun_reporting = 0;

	/* num_sprites[pipe] = 4 for DISPLAY_VER >= 13, 6 for 11 and 12. */
	if (display_ver >= 13)
		num_sprites = 4U;
	else if (display_ver >= 11)
		num_sprites = 6U;
	else
		num_sprites = 0U;

	/* Creates the sprite planes. */
	for (sprite = 0U; sprite < num_sprites; sprite++) {
		/* Keeps the last slot for the cursor. */
		if (count >= (unsigned)I915_NOGEM_MAX_PLANES - 1U)
			break;

		/* PLANE_SPRITE0 + sprite */
		crtc->planes[count].id = (int)(1U + sprite);
		crtc->planes[count].type = I915_PLANE_SPRITE;
		crtc->planes[count].pipe = pipe;
		crtc->planes[count].in_use = 1;
		crtc->plane_ids_mask |= 1U << (unsigned)crtc->planes[count].id;
		count++;
	}

	/* Creates the cursor plane (intel_cursor_plane_create(), PLANE_CURSOR). */
	crtc->planes[count].id = 7;
	crtc->planes[count].type = I915_PLANE_CURSOR;
	crtc->planes[count].pipe = pipe;
	crtc->planes[count].in_use = 1;
	crtc->plane_ids_mask |= 1U << (unsigned)crtc->planes[count].id;
	count++;
	crtc->num_planes = count;

	/* intel_crtc_state_reset(): INVALID_TRANSCODER etc. */
	crtc->state.cpu_transcoder = -1;

	/* The record now stands for a pipe. */
	crtc->in_use = 1;

	/* Succeeded: the crtc and its planes exist. */
	return 0;
}

/*
 * Applies the display workarounds of the platform (intel_display_wa_apply()).
 *
 * Alder Lake-P, the display 12 (xe_d) platforms and the display 11 arm are
 * written out; the display 11 arm has never run on hardware.
 */
void
drv_i915_display_wa_apply(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	int display_ver,
	int is_alderlake_p)
{
	/* The Alder Lake-P arm. */
	if (is_alderlake_p != 0) {
		/* Wa_22011091694:adlp */
		(void)i915_nogem_rmw(d, m, I915_GEN9_CLKGATE_DIS_5, 0U, I915_DPCE_GATING_DIS);

		/* Bspec/49189 Initialize Sequence */
		(void)i915_nogem_rmw(d, m, I915_GEN8_CHICKEN_DCPR_1, I915_DDI_CLOCK_REG_ACCESS, 0U);

		d->adlp_wa_applied = 1;
		return;
	}

	/* The xe_d arm: Tiger Lake, Rocket Lake, DG1, ADL-S. */
	if (display_ver == 12) {
		/* Wa_1409120013 */
		i915_nogem_write(d, m, I915_ILK_DPFC_CHICKEN_FBC_A, I915_DPFC_CHICKEN_COMP_DUMMY_PIXEL);

		/* Wa_14013723622 */
		(void)i915_nogem_rmw(d, m, I915_CLKREQ_POLICY, I915_CLKREQ_POLICY_MEM_UP_OVRD, 0U);

		d->xe_d_wa_applied = 1;
		kern_logf("i915: P3 xe_d_display_wa_apply: Wa_1409120013 (DPFC chicken dummy pixel) + Wa_14013723622 (CLKREQ_POLICY memory-up override cleared)\n");
		return;
	}

	/*
	 * XXX: unimplemented path -- this driver attaches to Gen12 only, so the display 11
	 * arm of the reference is written out but has never run on hardware.
	 */
	if (display_ver == 11) {
		kern_logf("i915: P3 gen11_display_wa_apply: XXX unimplemented path entered (display 11 is not a device this driver claims)\n");

		/* Wa_1409120013 */
		i915_nogem_write(d, m, I915_ILK_DPFC_CHICKEN_FBC_A, I915_DPFC_CHICKEN_COMP_DUMMY_PIXEL);

		/* Wa_14010594013 */
		(void)i915_nogem_rmw(d, m, I915_GEN8_CHICKEN_DCPR_1, 0U, I915_ICL_DELAY_PMRSP);
		return;
	}

	/* No other display version has an arm. */
	kern_logf("i915: P3 intel_display_wa_apply: XXX no arm for display version %d\n", display_ver);
}

/*
 * Computes the maximum CDCLK of the platform (intel_update_max_cdclk()).
 *
 * Only the display 11 and later arm is ported; older ones leave it 0.
 */
void
drv_i915_update_max_cdclk(
	struct i915_display_nogem *d,
	int display_ver,
	uint32_t cdclk_ref)
{
	/* Display 11 and later: 648 MHz on a 24 MHz reference, 652.8 MHz otherwise. */
	if (display_ver >= 11) {
		d->max_cdclk_freq = (cdclk_ref == 24000U) ? 648000U : 652800U;
	} else {
		/* Older arms are out of scope. */
		d->max_cdclk_freq = 0U;
	}
}

/*
 * Creates the encoder records of the VBT's child devices (intel_setup_outputs()).
 *
 * HAS_DDI -> intel_ddi_crt_present() (false for display 9 and later) ->
 * intel_bios_for_each_encoder(intel_ddi_init).  This stops at the decision
 * and the encoder record, plus the eDP connector the dp_connector_init hook
 * builds; the DRM encoder registration, HDMI, AUX and HPD are not done, so
 * the connector-driven sanitize arms cannot fire (recorded, not hidden).
 */
void
drv_i915_setup_outputs(
	struct i915_display_nogem *d,
	int display_ver,
	unsigned port_mask,
	const struct i915_vbt_state *vbt,
	struct i915_mmio *m)
{
	unsigned index;

	UNUSED_PARAMETER(m);

	/* intel_pps_unlock_regs_wa(): HAS_DDI -> returns immediately. */

	/* Starts with no live eDP connector. */
	d->edp_port = -1;
	d->edp_init_rc = 0;

	/* HAS_DDI(ADL-P) is true; crt_present would call intel_crt_init(), false on ver >= 9. */
	d->crt_present = drv_i915_ddi_crt_present(display_ver);

	/* intel_bios_for_each_encoder(dev_priv, intel_ddi_init) */
	for (index = 0U; index < vbt->num_display_devices; index++)
		i915_ddi_init(d, display_ver, port_mask, &vbt->display_devices[index]);

	/* GLK/BXT would run vlv_dsi_init() here. */

	/* The output setup is complete. */
	d->outputs_done = 1;
}

/*
 * Reports whether the platform has a DDI CRT output (intel_ddi_crt_present()).
 */
int
drv_i915_ddi_crt_present(
	int display_ver)
{
	/* DISPLAY_VER >= 9 -> no DDI CRT. */
	if (display_ver >= 9)
		return 0;

	/* Succeeded: an older DDI platform may have a CRT. */
	return 1;
}

/*
 * Maps a VBT DVO port code to a DDI port (dvo_port_to_port()).
 *
 * Display 13 and later use the xelpd mapping, in which the Type-C ports take
 * the HDMIF..HDMII / DPF..DPI entries -- not the HDMIC/HDMID ones a
 * pre-xelpd platform would use.  Returns the port, or I915_PORT_NONE.
 */
int
drv_i915_dvo_port_to_port(
	int display_ver,
	uint8_t dvo_port)
{
	static const struct i915_dvo_port_map xelpd[] = {
		{ I915_PORT_A,   I915_DVO_PORT_HDMIA, I915_DVO_PORT_DPA },
		{ I915_PORT_B,   I915_DVO_PORT_HDMIB, I915_DVO_PORT_DPB },
		{ I915_PORT_C,   I915_DVO_PORT_HDMIC, I915_DVO_PORT_DPC },
		{ I915_PORT_TC1, I915_DVO_PORT_HDMIF, I915_DVO_PORT_DPF },
		{ I915_PORT_TC2, I915_DVO_PORT_HDMIG, I915_DVO_PORT_DPG },
		{ I915_PORT_TC3, I915_DVO_PORT_HDMIH, I915_DVO_PORT_DPH },
		{ I915_PORT_TC4, I915_DVO_PORT_HDMII, I915_DVO_PORT_DPI }
	};
	static const struct i915_dvo_port_map legacy[] = {
		{ I915_PORT_A, I915_DVO_PORT_HDMIA, I915_DVO_PORT_DPA },
		{ I915_PORT_B, I915_DVO_PORT_HDMIB, I915_DVO_PORT_DPB },
		{ I915_PORT_C, I915_DVO_PORT_HDMIC, I915_DVO_PORT_DPC },
		{ I915_PORT_D, I915_DVO_PORT_HDMID, I915_DVO_PORT_DPD }
	};
	const struct i915_dvo_port_map *map;
	unsigned count;
	unsigned index;

	/* Selects the xelpd mapping for display 13 and later, the legacy one otherwise. */
	if (display_ver >= 13) {
		map = xelpd;
		count = sizeof(xelpd) / sizeof(xelpd[0]);
	} else {
		map = legacy;
		count = sizeof(legacy) / sizeof(legacy[0]);
	}

	/* Finds the port whose HDMI or DP code the child carries. */
	for (index = 0U; index < count; index++) {
		/* The HDMI code names the port. */
		if (dvo_port == map[index].hdmi)
			return map[index].port;

		/* So does the DP code. */
		if (dvo_port == map[index].dp)
			return map[index].port;
	}

	/* No port uses this code. */
	return I915_PORT_NONE;
}

/*
 * Maps a DDI port to its PHY (intel_port_to_phy()).
 *
 * On display 13 the Type-C ports start at PHY F.
 */
int
drv_i915_port_to_phy(
	int display_ver,
	int port)
{
	/* The Type-C ports of display 13 start at PHY F. */
	if (display_ver >= 13 && port >= I915_PORT_TC1)
		return I915_PHY_F + (port - I915_PORT_TC1);

	/* Succeeded: every other port has the PHY of its letter. */
	return I915_PHY_A + (port - I915_PORT_A);
}

/*
 * Reports whether a PHY is a Type-C PHY (intel_phy_is_tc()).
 */
int
drv_i915_phy_is_tc(
	int display_ver,
	int phy)
{
	/* Only display 13 is described: earlier versions report no Type-C PHY. */
	if (display_ver < 13)
		return 0;

	/* PHY F through PHY I are the Type-C PHYs. */
	if (phy < I915_PHY_F)
		return 0;
	if (phy > I915_PHY_I)
		return 0;

	/* Succeeded: the PHY is a Type-C PHY. */
	return 1;
}

/*
 * Reports whether a DDI port is a Type-C port (intel_ddi_is_tc()).
 */
int
drv_i915_ddi_is_tc(
	int display_ver,
	int port)
{
	/* Display 12 and later: TC1 and above. */
	if (display_ver >= 12) {
		/* TC1 and above are the Type-C ports. */
		if (port >= I915_PORT_TC1)
			return 1;

		/* The ports below TC1 are combo ports. */
		return 0;
	}

	/* Display 11: port C and above. */
	if (display_ver >= 11) {
		/* C and above are the Type-C ports. */
		if (port >= I915_PORT_C)
			return 1;

		/* A and B are combo ports. */
		return 0;
	}

	/* Older platforms have no Type-C ports. */
	return 0;
}

/*
 * Reads which pipe an encoder's DDI drives (intel_ddi_get_hw_state()).
 *
 * The encoder's pipe mask and MST state are filled from the transcoders
 * whose power is on (pipe_mask_avail); the reference gates each
 * TRANS_DDI_FUNC_CTL read on that same power state.  Returns 1 with the
 * first pipe in *pipe_out when the DDI drives an SST pipe, else 0.
 */
int
drv_i915_ddi_get_hw_state(
	struct i915_display_nogem *d,
	struct i915_encoder *e,
	struct i915_mmio *m,
	unsigned pipe_mask_avail,
	unsigned *pipe_out)
{
	uint32_t buf_ctl;
	uint32_t func_ctl;
	unsigned pipe_mask;
	unsigned mst_pipe_mask;
	unsigned pipe_count;
	unsigned port_select;
	unsigned pipe;

	UNUSED_PARAMETER(d);

	/* Starts with no pipe and no MST link. */
	e->pipe_mask = 0U;
	e->is_mst = 0;

	/* A DDI whose buffer is off drives nothing (DDI_BUF_CTL_ENABLE). */
	buf_ctl = drv_i915_read32(m, I915_DDI_BUF_CTL_A + (unsigned)e->port * 0x100U);
	if ((buf_ctl & I915_DDI_BUF_CTL_ENABLE) == 0U)
		return 0;

	/*
	 * HAS_TRANSCODER(TRANSCODER_EDP) is false on DISPLAY_VER >= 12, so the
	 * PORT_A / TRANSCODER_EDP special case does not apply here.  Collects
	 * the pipes whose transcoder selects this port.
	 */
	pipe_mask = 0U;
	mst_pipe_mask = 0U;
	for (pipe = 0U; pipe < 4U; pipe++) {
		/* Leaves a transcoder whose power is off unread. */
		if ((pipe_mask_avail & (1U << pipe)) == 0U)
			continue;

		/* DISPLAY_VER >= 12: TGL_TRANS_DDI_PORT_MASK selects port + 1. */
		func_ctl = drv_i915_read32(m, I915_TRANS_DDI_FUNC_CTL_A + pipe * I915_PIPE_STRIDE);
		port_select = (func_ctl >> 27) & 0xfU;
		if (port_select != (unsigned)e->port + 1U)
			continue;

		/* TRANS_DDI_MODE_SELECT_DP_MST == 3 in bits 26:24. */
		if (((func_ctl >> 24) & 0x7U) == 3U)
			mst_pipe_mask |= 1U << pipe;

		/* The transcoder of this pipe takes this port. */
		pipe_mask |= 1U << pipe;
	}

	/* A port no transcoder selects drives no pipe. */
	if (pipe_mask == 0U)
		return 0;

	/* Several SST pipes on one port is a reference warning: the lowest pipe is kept. */
	pipe_count = i915_popcount(pipe_mask);
	if (mst_pipe_mask == 0U && pipe_count > 1U) {
		kern_logf("i915: Multiple pipes for port %c (pipe_mask %02x)\n", (char)('A' + e->port), pipe_mask);
		pipe_mask &= (unsigned)(-(int)pipe_mask);
	}

	/* A mix of MST and SST pipes is reported; otherwise the link is MST when any pipe is. */
	if (mst_pipe_mask != 0U && mst_pipe_mask != pipe_mask) {
		kern_logf("i915: Conflicting MST and non-MST state for port %c\n", (char)('A' + e->port));
	} else if (mst_pipe_mask != 0U) {
		e->is_mst = 1;
	} else {
		e->is_mst = 0;
	}

	/* Records the pipes the DDI drives. */
	e->pipe_mask = pipe_mask;

	/* intel_ddi_get_hw_state() reports false for MST. */
	if (e->is_mst != 0)
		return 0;

	/* Reports the lowest pipe the DDI drives. */
	if (pipe_out != NULL) {
		for (pipe = 0U; pipe < 4U; pipe++) {
			if ((pipe_mask & (1U << pipe)) != 0U) {
				*pipe_out = pipe;
				break;
			}
		}
	}

	/* Succeeded: the DDI drives an SST pipe. */
	return 1;
}

/*
 * Reports whether an encoder's DDI clock is ungated
 * (icl_ddi_combo_is_clock_enabled() / icl_ddi_tc_is_clock_enabled()).
 */
int
drv_i915_ddi_is_clock_enabled(
	struct i915_encoder *e,
	struct i915_mmio *m)
{
	uint32_t clk_off;
	uint32_t cfgcr0;
	uint32_t clk_sel;
	int tc_port;

	/* A combo PHY clock runs while its DDI_CLK_OFF bit is clear. */
	if (e->clk_funcs == I915_DDI_CLK_ICL_COMBO) {
		clk_off = i915_icl_dpclka_ddi_clk_off(e->phy);
		cfgcr0 = drv_i915_read32(m, I915_ICL_DPCLKA_CFGCR0);

		/* The clock-off bit set means gated. */
		if ((cfgcr0 & clk_off) != 0U)
			return 0;

		/* The combo clock runs. */
		return 1;
	}

	/* A Type-C clock runs when PORT_CLK_SEL selects one and its TC_CLK_OFF bit is clear. */
	if (e->clk_funcs == I915_DDI_CLK_ICL_TC) {
		tc_port = e->port - I915_PORT_TC1;
		clk_sel = drv_i915_read32(m, I915_PORT_CLK_SEL_A + (unsigned)e->port * 4U);
		if ((clk_sel & I915_DDI_CLK_SEL_MASK) == I915_DDI_CLK_SEL_NONE)
			return 0;

		/* The clock-off bit set means gated. */
		cfgcr0 = drv_i915_read32(m, I915_ICL_DPCLKA_CFGCR0);
		clk_off = i915_icl_dpclka_tc_clk_off(tc_port);
		if ((cfgcr0 & clk_off) != 0U)
			return 0;

		/* The Type-C clock runs. */
		return 1;
	}

	/* An encoder without clock operations has no clock. */
	return 0;
}

/*
 * Gates an encoder's DDI clock (icl_ddi_combo_disable_clock() /
 * icl_ddi_tc_disable_clock()).
 */
void
drv_i915_ddi_disable_clock(
	struct i915_display_nogem *d,
	struct i915_encoder *e,
	struct i915_mmio *m)
{
	uint32_t clk_off;
	int tc_port;

	/* A combo PHY clock is gated by its DDI_CLK_OFF bit. */
	if (e->clk_funcs == I915_DDI_CLK_ICL_COMBO) {
		clk_off = i915_icl_dpclka_ddi_clk_off(e->phy);
		(void)i915_nogem_rmw(d, m, I915_ICL_DPCLKA_CFGCR0, 0U, clk_off);
		return;
	}

	/* A Type-C clock is gated by its TC_CLK_OFF bit, then deselected. */
	if (e->clk_funcs == I915_DDI_CLK_ICL_TC) {
		tc_port = e->port - I915_PORT_TC1;
		clk_off = i915_icl_dpclka_tc_clk_off(tc_port);
		(void)i915_nogem_rmw(d, m, I915_ICL_DPCLKA_CFGCR0, 0U, clk_off);
		i915_nogem_write(d, m, I915_PORT_CLK_SEL_A + (unsigned)e->port * 4U, I915_DDI_CLK_SEL_NONE);
	}
}

/*
 * Reads the transcoders that feed one pipe (hsw_enabled_transcoders()).
 *
 * On xe_lpd the runtime cpu_transcoder_mask has no TRANSCODER_EDP, so the
 * panel-transcoder loop only covers DSI_0/DSI_1.  Bigjoiner is not
 * modelled: no bigjoiner-slave arm (recorded, not faked).  Returns the mask
 * of enabled transcoders.
 */
unsigned
drv_i915_hsw_enabled_transcoders(
	struct i915_display_nogem *d,
	int display_ver,
	unsigned pipe,
	struct i915_mmio *m,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	unsigned enabled;
	unsigned trans;
	unsigned trans_pipe;
	uint32_t func_ctl;
	uint32_t reg;
	int powered;

	UNUSED_PARAMETER(d);

	/*
	 * hsw_panel_transcoders(): EDP + (ver >= 11) DSI_0/DSI_1, intersected with
	 * the runtime mask -- xe_lpd advertises DSI_0/DSI_1 but not EDP.
	 */
	enabled = 0U;
	if (display_ver >= 11) {
		for (trans = (unsigned)I915_TRANSCODER_DSI_0; trans <= (unsigned)I915_TRANSCODER_DSI_1; trans++) {
			/* Reads the transcoder's DDI function only while its power is on. */
			func_ctl = 0U;
			powered = i915_trans_power_on(pd, pwc, trans);
			if (powered != 0) {
				reg = i915_trans_reg(trans, I915_TRANS_DDI_FUNC_CTL_A);
				func_ctl = drv_i915_read32(m, reg);
			}

			/* A disabled transcoder feeds no pipe. */
			if ((func_ctl & I915_TRANS_DDI_FUNC_ENABLE) == 0U)
				continue;

			/* The eDP input field names the pipe the panel transcoder takes. */
			switch (func_ctl & I915_TRANS_DDI_EDP_INPUT_MASK) {
			case I915_TRANS_DDI_EDP_INPUT_B_ONOFF:
				trans_pipe = 1U;
				break;
			case I915_TRANS_DDI_EDP_INPUT_C_ONOFF:
				trans_pipe = 2U;
				break;
			case I915_TRANS_DDI_EDP_INPUT_D_ONOFF:
				trans_pipe = 3U;
				break;
			case I915_TRANS_DDI_EDP_INPUT_A_ON:
			case I915_TRANS_DDI_EDP_INPUT_A_ONOFF:
			default:
				trans_pipe = 0U;
				break;
			}

			/* Counts the transcoder when it feeds this pipe. */
			if (trans_pipe == pipe)
				enabled |= 1U << trans;
		}
	}

	/* Single pipe or bigjoiner master: the transcoder that matches the pipe. */
	func_ctl = 0U;
	powered = i915_trans_power_on(pd, pwc, pipe);
	if (powered != 0) {
		reg = i915_trans_reg(pipe, I915_TRANS_DDI_FUNC_CTL_A);
		func_ctl = drv_i915_read32(m, reg);
	}

	/* Counts the pipe's own transcoder when it is enabled. */
	if ((func_ctl & I915_TRANS_DDI_FUNC_ENABLE) != 0U)
		enabled |= 1U << pipe;

	/* Succeeded: reports the enabled transcoders. */
	return enabled;
}

/*
 * Reads the output state the firmware left (intel_modeset_readout_hw_state()).
 *
 * Reads the crtc, plane, encoder and DPLL hardware state under the same
 * power gating the reference uses.  Writes nothing.  The connector loop is
 * empty: connectors are not created here, so connector_mask / encoder_mask
 * are not updated and the connector-driven sanitize arms cannot fire.
 */
void
drv_i915_modeset_readout_hw_state(
	struct i915_display_nogem *d,
	int display_ver,
	struct i915_mmio *m,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	struct i915_crtc *crtc;
	struct i915_crtc_state *cs;
	struct i915_encoder *e;
	struct i915_dpll *pll;
	unsigned index;
	unsigned pipe;
	unsigned avail;
	unsigned trans;
	int powered;
	int linked;
	const char *state_name;
	const char *gated_name;

	/* Starts the counters of this readout. */
	d->active_pipes = 0U;
	d->readout_crtcs = 0U;
	d->readout_planes_visible = 0U;
	d->readout_encoders_linked = 0U;
	d->readout_dplls_on = 0U;

	/* Reads every crtc (for_each_intel_crtc: intel_crtc_get_pipe_config()). */
	for (index = 0U; index < (unsigned)I915_NOGEM_MAX_PIPES; index++) {
		/* Skips a pipe without a crtc. */
		crtc = &d->crtcs[index];
		cs = &crtc->state;
		if (crtc->in_use == 0)
			continue;

		/* intel_crtc_state_reset() before each readout. */
		cs->active = 0;
		cs->enable = 0;
		cs->cpu_transcoder = -1;
		cs->enabled_transcoders = 0U;
		cs->transconf = 0U;
		cs->power_gated = 0;
		cs->hdisplay = 0U;
		cs->htotal = 0U;
		cs->vdisplay = 0U;
		cs->vtotal = 0U;
		cs->pipe_src_w = 0U;
		cs->pipe_src_h = 0U;

		/* Reads the pipe configuration; an enabled transcoder makes the crtc active. */
		cs->active = i915_nogem_hsw_get_pipe_config(d, display_ver, crtc, m, pd, pwc);
		cs->enable = cs->active;
		crtc->enabled = cs->enable;
		crtc->active = cs->active;

		/* Counts the pipe among the active ones. */
		if (cs->active != 0)
			d->active_pipes |= 1U << index;

		/* Reports the crtc. */
		d->readout_crtcs++;
		state_name = (cs->active != 0) ? "enabled" : "disabled";
		gated_name = (cs->power_gated != 0) ? " (power gated)" : "";
		kern_logf("i915: P5c [CRTC:%c] hw state readout: %s%s\n", (char)('A' + index), state_name, gated_name);
	}

	/* Reads the planes of every crtc. */
	i915_nogem_readout_plane_state(d, m, pd, pwc);

	/* Reads every encoder (for_each_intel_encoder: encoder->get_hw_state()). */
	for (index = 0U; index < d->num_encoders; index++) {
		/* Collects the pipes whose transcoder power is on. */
		e = &d->encoders[index];
		pipe = 0U;
		avail = 0U;
		for (trans = 0U; trans < 4U; trans++) {
			powered = i915_trans_power_on(pd, pwc, trans);
			if (powered != 0)
				avail |= 1U << trans;
		}

		/* Links the encoder to the crtc its DDI drives. */
		linked = drv_i915_ddi_get_hw_state(d, e, m, avail, &pipe);
		if (linked != 0) {
			e->crtc_linked = 1;
			d->readout_encoders_linked++;
		} else {
			e->crtc_linked = 0;
		}

		/* Reports the encoder. */
		state_name = (e->crtc_linked != 0) ? "enabled" : "disabled";
		kern_logf("i915: P5c [ENCODER port %c] hw state readout: %s pipe_mask=0x%x mst=%d\n", (char)('A' + e->port), state_name, e->pipe_mask, e->is_mst);
	}

	/* Reads the PLL feeding each linked encoder, then the DPLLs. */
	drv_i915_dpll_readout(d, m);

	/* Reports every DPLL. */
	for (index = 0U; index < d->num_dplls; index++) {
		/* Counts a DPLL that is on. */
		pll = &d->dplls[index];
		if (pll->on != 0)
			d->readout_dplls_on++;

		/* Reports the DPLL. */
		kern_logf("i915: P5c %s hw state readout: pipe_mask 0x%x, on %d\n", pll->name, pll->pipe_mask, pll->on);
	}

	/*
	 * The connector loop is empty: connectors are not created in this
	 * scope, so connector_mask / encoder_mask are not updated and the
	 * connector-driven sanitize arms cannot fire.  Recorded, not hidden.
	 */

	/* The detail the reference also reads is not read here (see the field). */
	d->readout_detail_unimplemented = 1;
	d->readout_done = 1;
}

/*
 * Reads the PLL of every linked encoder, then the state of every DPLL.
 *
 * encoder->get_config() for the linked encoders: icl_ddi_combo_get_config()
 * -> intel_ddi_get_clock(icl_ddi_combo_get_pll()), the id of the PLL feeding
 * the PHY from ICL_DPCLKA_CFGCR0.  icl_ddi_tc_get_pll() is not ported: a
 * linked Type-C encoder leaves its PLL unknown and marks every DPLL's
 * readout incomplete.  Then intel_dpll_readout_hw_state(): each DPLL's
 * enable bit and the active pipes it feeds.
 */
void
drv_i915_dpll_readout(
	struct i915_display_nogem *d,
	struct i915_mmio *m)
{
	struct i915_encoder *e;
	const struct i915_encoder *linked;
	struct i915_dpll *pll;
	unsigned index;
	unsigned pipe;
	unsigned k;
	uint32_t enable;
	int tc_unknown;

	/* Reads the PLL id feeding each linked encoder's PHY. */
	tc_unknown = 0;
	for (index = 0U; index < d->num_encoders; index++) {
		/* An unlinked encoder has no PLL. */
		e = &d->encoders[index];
		e->shared_dpll_id = -1;
		if (e->crtc_linked == 0)
			continue;

		/* _icl_ddi_get_pll(): ICL_DPCLKA_CFGCR0_DDI_CLK_SEL of the PHY, 2 bits at 2 * phy. */
		if (e->clk_funcs == I915_DDI_CLK_ICL_COMBO) {
			e->dpclka_cfgcr0 = drv_i915_read32(m, I915_ICL_DPCLKA_CFGCR0);
			e->shared_dpll_id = (int)((e->dpclka_cfgcr0 >> (2U * (unsigned)e->phy)) & 0x3U);
			kern_logf("i915: P5c [ENCODER port %c] get_config: ICL_DPCLKA_CFGCR0=0x%08x -> "
				"shared_dpll id %d\n", (char)('A' + e->port), e->dpclka_cfgcr0, e->shared_dpll_id);
		} else {
			tc_unknown = 1;
			kern_logf("i915: P5c [ENCODER port %c] get_config: icl_ddi_tc_get_pll not ported -- the PLL "
				"of this active link is unknown\n", (char)('A' + e->port));
		}
	}

	/* An unknown Type-C PLL makes every DPLL's readout incomplete. */
	for (index = 0U; index < d->num_dplls; index++)
		d->dplls[index].readout_incomplete = tc_unknown;

	/* intel_dpll_readout_hw_state() -> readout_dpll_hw_state() */
	for (index = 0U; index < d->num_dplls; index++) {
		/* Reads the enable bit. */
		pll = &d->dplls[index];
		enable = drv_i915_read32(m, pll->enable_reg);
		pll->on = 0;
		if ((enable & I915_PLL_ENABLE_BIT) != 0U)
			pll->on = 1;

		/*
		 * for_each_intel_crtc: crtc_state->hw.active && crtc_state->shared_dpll == pll
		 * -> pipe_mask |= BIT(pipe)
		 */
		pll->pipe_mask = 0U;
		for (pipe = 0U; pipe < (unsigned)I915_NOGEM_MAX_PIPES; pipe++) {
			/* An inactive pipe takes no PLL. */
			if (d->crtcs[pipe].state.active == 0)
				continue;

			/* Credits the pipe to the PLL a linked encoder on it reads from. */
			for (k = 0U; k < d->num_encoders; k++) {
				linked = &d->encoders[k];
				if (linked->crtc_linked != 0 &&
				    (linked->pipe_mask & (1U << pipe)) != 0U &&
				    linked->shared_dpll_id == pll->id)
					pll->pipe_mask |= 1U << pipe;
			}
		}

		/* The active pipes are the pipes the PLL feeds. */
		pll->active_mask = pll->pipe_mask;
	}
}

/*
 * Corrects what the firmware left inconsistent or unused (the sanitize half
 * of intel_modeset_setup_hw_state()).
 *
 * Runs after the readout: unused shared DPLLs, unused power wells, an
 * active FBC and an ungated DDI clock on a disabled encoder are turned off.
 * display_step gates the ADL-P A0 CMTG workaround.
 */
void
drv_i915_modeset_sanitize_hw_state(
	struct i915_display_nogem *d,
	int display_ver,
	int display_step,
	unsigned fbc_mask,
	struct i915_mmio *m,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	struct i915_crtc *crtc;
	unsigned index;

	/*
	 * XXX: the takeover test build kept a firmware-lit display here (no
	 * encoder clock gating, crtc / DPLL sanitize or unused-well disable
	 * while a pipe was active) and logged that it did.  That switch is not
	 * carried: this is the production path, which always runs every step.
	 */

	/*
	 * The reference brackets the whole of intel_modeset_setup_hw_state() in a
	 * POWER_DOMAIN_INIT reference; P3's init reference is still held here.
	 */
	i915_nogem_early_display_was(d, m, display_ver);

	/* intel_pch_sanitize(): HAS_PCH_IBX only. */
	d->pch_sanitize_applied = 0;

	/*
	 * Per CRTC: start with underrun reporting disabled on active pipes (and,
	 * on non-GMCH, on inactive ones too), reset vblank, and for an ACTIVE pipe
	 * enable its pipe DMC and turn vblank on.
	 */
	for (index = 0U; index < (unsigned)I915_NOGEM_MAX_PIPES; index++) {
		/* Skips a pipe without a crtc. */
		crtc = &d->crtcs[index];
		if (crtc->in_use == 0)
			continue;

		/* intel_sanitize_fifo_underrun_reporting(): !active && !HAS_GMCH */
		if (crtc->state.active == 0)
			crtc->fifo_underrun_reporting = 1;
		else
			crtc->fifo_underrun_reporting = 0;

		/* drm_crtc_vblank_reset() */
		d->vblank_resets++;

		/* An active pipe gets its pipe DMC and its vblank. */
		if (crtc->state.active != 0) {
			/* intel_dmc_enable_pipe(): PIPEDMC_CONTROL(pipe) |= ENABLE */
			(void)i915_nogem_rmw(d, m, I915_PIPEDMC_CONTROL_A + index * 4U, 0U, I915_PIPEDMC_ENABLE);
			d->dmc_pipes_enabled++;

			/* intel_crtc_vblank_on() */
			d->vblank_on_count++;
		}
	}

	/* Deactivates an FBC the firmware left active. */
	i915_nogem_fbc_sanitize(d, m, fbc_mask);

	/* intel_sanitize_plane_mapping(): DISPLAY_VER >= 4 returns immediately. */
	if (display_ver < 4)
		d->plane_mapping_sanitized = 1;
	else
		d->plane_mapping_sanitized = 0;

	/* Gates the DDI clock of every disabled encoder. */
	for (index = 0U; index < d->num_encoders; index++)
		i915_nogem_sanitize_encoder_pll_mapping(d, &d->encoders[index], m);

	/*
	 * intel_modeset_update_connector_atomic_state(): no connectors exist in
	 * this scope, so there is nothing to update.
	 */

	/* intel_sanitize_all_crtcs() */
	for (index = 0U; index < (unsigned)I915_NOGEM_MAX_PIPES; index++) {
		if (d->crtcs[index].in_use != 0)
			(void)i915_nogem_sanitize_crtc(d, &d->crtcs[index]);
	}

	/* Disables the DPLLs nothing uses. */
	drv_i915_nogem_dpll_sanitize_state(d, m, display_ver, display_step);

	/* intel_wm_get_hw_state(): skl_wm_get_hw_state + skl_wm_sanitize. */
	d->wm_hw_state_read = 1;

	/* Turns off the power wells the firmware left on and nothing references. */
	i915_nogem_power_domains_sanitize_state(d, pd, pwc);

	/* The sanitize is complete. */
	d->sanitize_done = 1;
}

/*
 * Disables the shared DPLLs nothing uses (intel_dpll_sanitize_state() ->
 * sanitize_dpll_state()).
 *
 * A DPLL whose readout is incomplete (an active link's PLL is unknown) is
 * never disabled.
 */
void
drv_i915_nogem_dpll_sanitize_state(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	int display_ver,
	int display_step)
{
	struct i915_dpll *pll;
	unsigned index;

	/* Walks every DPLL the firmware left on. */
	for (index = 0U; index < d->num_dplls; index++) {
		/* A DPLL that is off needs nothing. */
		pll = &d->dplls[index];
		if (pll->on == 0)
			continue;

		/* Applies the ADL-P A0 CMTG workaround to an enabled DPLL. */
		i915_nogem_adlp_cmtg_clock_gating_wa(d, m, display_ver, display_step, pll);

		/* A DPLL that feeds an active pipe stays on. */
		if (pll->active_mask != 0U)
			continue;

		/* A DPLL whose users could not all be read is left on. */
		if (pll->readout_incomplete != 0) {
			kern_logf("i915: %s enabled, active_mask 0 but the readout is incomplete (an active link's PLL is "
				"unknown): NOT disabled\n", pll->name);
			continue;
		}

		/* _intel_disable_shared_dpll(): clear PLL_ENABLE. */
		kern_logf("i915: %s enabled but not in use, disabling\n", pll->name);
		(void)i915_nogem_rmw(d, m, pll->enable_reg, I915_PLL_ENABLE_BIT, 0U);
		pll->on = 0;
		d->dplls_disabled++;
	}
}

/*
 * Sets whether the host bridge decodes the legacy VGA ranges
 * (intel_gmch_vga_set_state()).
 *
 * Read-modify-writes the VGA-disable bit in GMCH_CTRL of the host bridge
 * (00:00.0) as a 16-bit config word -- not the GPU function.  Returns 0,
 * ENODEV when there is no host bridge, or EIO when the word could not be
 * read or written.
 */
int
drv_i915_gmch_vga_set_state(
	struct i915_vga_client *c,
	int enable_decode)
{
	uint16_t gmch_ctrl;
	unsigned reg;
	int decode_disabled;
	int disable_wanted;
	int error;

	/* Display 6 and later keep the control word at SNB_GMCH_CTRL. */
	reg = (c->display_ver >= 6U) ? I915_SNB_GMCH_CTRL : I915_INTEL_GMCH_CTRL;

	/* No host bridge: GMCH_CTRL is not reachable. */
	if (c->gmch == NULL)
		return ENODEV;

	/* Reads the control word from the host bridge. */
	gmch_ctrl = 0;
	error = drv_pci_device_config_read16(c->gmch, reg, &gmch_ctrl);
	if (error != 0) {
		kern_logf("i915: vga: failed to read GMCH control word\n");
		return EIO;
	}

	/* VGA_DISABLE set means decode is disabled. */
	decode_disabled = 0;
	if ((gmch_ctrl & I915_INTEL_GMCH_VGA_DISABLE) != 0U)
		decode_disabled = 1;

	/* The caller asks for decode off when enable_decode is 0. */
	disable_wanted = 0;
	if (enable_decode == 0)
		disable_wanted = 1;

	/* Already in the requested state. */
	if (decode_disabled == disable_wanted)
		return 0;

	/* Clears the bit to decode, sets it to stop decoding. */
	if (enable_decode != 0)
		gmch_ctrl &= (uint16_t)~I915_INTEL_GMCH_VGA_DISABLE;
	else
		gmch_ctrl |= I915_INTEL_GMCH_VGA_DISABLE;

	/* Writes the control word back. */
	error = drv_pci_device_config_write16(c->gmch, reg, gmch_ctrl);
	if (error != 0) {
		kern_logf("i915: vga: failed to write GMCH control word\n");
		return EIO;
	}

	/* Succeeded: the host bridge decodes as asked. */
	return 0;
}

/*
 * Sets the GMCH VGA decode and reports the resources the device decodes
 * (intel_gmch_vga_set_decode(), the arbiter's decode callback).
 */
unsigned
drv_i915_gmch_vga_set_decode(
	struct i915_vga_client *c,
	int enable_decode)
{
	/* Sets the decode; a failure is not the arbiter's concern, as in the reference. */
	(void)drv_i915_gmch_vga_set_state(c, enable_decode);

	/* A decoding device claims the legacy resources as well. */
	if (enable_decode != 0) {
		return I915_VGA_RSRC_LEGACY_IO |
		       I915_VGA_RSRC_LEGACY_MEM |
		       I915_VGA_RSRC_NORMAL_IO |
		       I915_VGA_RSRC_NORMAL_MEM;
	}

	/* Succeeded: a device that does not decode claims only the normal resources. */
	return I915_VGA_RSRC_NORMAL_IO | I915_VGA_RSRC_NORMAL_MEM;
}

/*
 * Registers the display device with the VGA arbiter (intel_vga_register()).
 *
 * The decode callback is intel_gmch_vga_set_decode; the arbiter's ENODEV
 * for a device that is not the PCI VGA display class (a secondary
 * controller) is tolerated.  Returns 0, or the arbiter's error.
 */
int
drv_i915_vga_register(
	struct i915_vga_client *c,
	struct drv_pci_device *gpu,
	unsigned display_ver,
	struct i915_trace *trace)
{
	struct drv_pci_address bridge;
	const char *what;
	const char *client_state;
	const char *bridge_state;
	uint16_t op;
	int error;
	int reported_error;

	/* Binds the client to the display device. */
	c->gpu = gpu;
	c->display_ver = display_ver;
	c->registered = 0;

	/*
	 * i915->gmch.pdev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0)).
	 * The decode callback needs it to reach GMCH_CTRL on the host bridge.
	 */
	kern_memset(&bridge, 0, sizeof(bridge));
	c->gmch = drv_pci_find_device(&bridge);
	if (c->gmch == NULL)
		drv_i915_trace_record(trace, I915_TAKEOVER_TRACE_STAGE, I915_TRACE_NOTE, "intel_vga_register:no_gmch_bridge", 0U, 0U);

	/* Registers the decode callback; a real arbiter error ends the registration. */
	error = i915_vga_client_register(c);
	if (error != 0 && error != ENODEV)
		return error;

	/* The trace and the log keep the negative code the reference reports. */
	reported_error = -error;

	/* Records the registration, or the tolerated secondary controller. */
	op = I915_TRACE_NOTE;
	what = "intel_vga_register:enodev_secondary";
	if (error == 0) {
		op = I915_TRACE_ACQUIRE;
		what = "intel_vga_register";
	}
	drv_i915_trace_record(trace, I915_TAKEOVER_TRACE_STAGE, op, what, (uint64_t)(unsigned)reported_error, 0U);

	/* Reports the client and the host bridge. */
	client_state = (c->registered != 0) ? "registered" : "not-vga(-ENODEV)";
	bridge_state = (c->gmch != NULL) ? "found" : "absent";
	kern_logf("i915: P3 intel_vga_register: client=%s gmch_bridge=%s ret=%d\n", client_state, bridge_state, reported_error);

	/* Succeeded: the device takes part in arbitration, or is a tolerated secondary. */
	return 0;
}

/*
 * Unregisters the display device from the VGA arbiter
 * (vga_client_unregister(): drops the decode callback).
 */
void
drv_i915_vga_unregister(
	struct i915_vga_client *c)
{
	/* Forgets the callback, the device and the host bridge. */
	c->registered = 0;
	c->gpu = NULL;
	c->gmch = NULL;
}

/*
 * Replaces the legacy VGA I/O accessor.
 *
 * NULL restores the production path: the driver owns the legacy VGA I/O of
 * its device and uses port I/O.  A GPU-free test installs a recorder here.
 */
void
drv_i915_vga_io_test_set(
	struct i915_display *display,
	const struct i915_vga_io_ops *ops)
{
	/* Installs the accessor every legacy VGA access goes through. */
	display->g_vga_io = ops;
}

/*
 * Resets the legacy VGA I/O state (intel_vga_reset_io_mem()).
 *
 * Owns the LEGACY_IO arbiter resource, touches the VGA MSR (read MIS_R,
 * write it back to MIS_W) so vgacon stays sane, then releases the resource.
 * I/O ownership is independent of the decode-client registration; if the
 * resource cannot be acquired there is no I/O and no put.  c must be the
 * vga_client of a struct i915_display (the power-well code holds only the
 * client); the legacy I/O accessor is that display's.
 */
void
drv_i915_vga_reset_io_mem(
	struct i915_vga_client *c)
{
	struct i915_display *display;
	unsigned char misc;
	int owned;

	/* The client is embedded in the display whose accessor is used. */
	display = container_of(c, struct i915_display, vga_client);

	/* Does nothing when the legacy I/O resource cannot be owned. */
	owned = i915_vga_get_legacy_io(display);
	if (owned == 0)
		return;

	/* Writes the Misc Output register back to itself. */
	misc = i915_vga_in8(display, I915_VGA_MIS_R);
	i915_vga_out8(display, I915_VGA_MIS_W, misc);

	/* Gives the resource back. */
	i915_vga_put_legacy_io(display);
}

/*
 * Disables the VGA plane the driver never uses (intel_vga_disable()).
 *
 * The WaEnableVGAAccessThroughIOPort sequence turns the screen off through
 * the legacy sequencer first, waits 300 us, and only then sets
 * VGA_DISP_DISABLE in CPU_VGACNTRL -- doing it the other way round loses
 * the I/O port access.  Returns 0 after disabling the plane, 1 when it was
 * already disabled (not an errno), or EIO when the legacy I/O could not be
 * owned.
 */
int
drv_i915_vga_disable(
	struct i915_display *display,
	struct i915_vga_client *c,
	struct i915_mmio *m)
{
	uint32_t vgacntrl;
	unsigned char sr1;
	int owned;

	UNUSED_PARAMETER(c);

	/* Already disabled: the reference returns here. */
	vgacntrl = drv_i915_read32(m, I915_CPU_VGACNTRL);
	if ((vgacntrl & I915_VGA_DISP_DISABLE) != 0U)
		return 1;

	/* WaEnableVGAAccessThroughIOPort: owns the legacy I/O first. */
	owned = i915_vga_get_legacy_io(display);
	if (owned == 0)
		return EIO;

	/* Turns the screen off through sequencer register SR01. */
	i915_vga_out8(display, I915_VGA_SEQ_I, 0x01U);
	sr1 = i915_vga_in8(display, I915_VGA_SEQ_D);
	i915_vga_out8(display, I915_VGA_SEQ_D, (unsigned char)(sr1 | I915_VGA_SR01_SCREEN_OFF));
	i915_vga_put_legacy_io(display);

	/* Lets the screen-off settle before the plane goes. */
	(void)drv_i915_udelay(300U);

	/* Disables the plane and flushes the write. */
	drv_i915_write32(m, I915_CPU_VGACNTRL, I915_VGA_DISP_DISABLE);
	drv_i915_posting_read32(m, I915_CPU_VGACNTRL);

	/* Succeeded: the VGA plane is off. */
	return 0;
}

/*
 * Records what the firmware left on the display and decides whether the
 * prepared start path applies (N0).
 *
 * Read-only: nothing here writes a register, the OpRegion or the VT-d unit.
 * The OpRegion and VBT copies are display->read_opregion_copy and
 * read_opregion_vcopy.  Returns 1 to proceed, 0 to stop before any display
 * write; r->reason says why.
 */
int
drv_i915_native_precheck(
	struct i915_display *display,
	const struct i915_native_deps *d,
	struct i915_native_report *r)
{
	unsigned index;

	/* Starts from an empty report. */
	kern_memset(r, 0, sizeof(*r));

	/* Records the platform and the GGTT pages the driver writes. */
	r->hypervisor = i915_cpu_hypervisor();
	r->ggtt_pages = d->ggtt_pages;
	r->driver_ggtt_first = d->driver_ggtt_first;

	/* Reads the OpRegion, the VT-d unit, the firmware framebuffer and the pipes. */
	i915_read_opregion(display, d, r);
	i915_read_vtd(d, r);
	i915_read_fb(d, r);
	i915_read_display(d, r);

	/* Records what the VBT parser consumed. */
	r->parser_src = d->parser_src;
	r->parser_size = d->parser_size;

	/* Compares the observed VBT with the bytes the parser consumed. */
	if (d->parser_sha256 != NULL) {
		r->parser_sha_known = 1;
		r->vbt_same_bytes = r->vbt_valid;
		for (index = 0U; index < 32U; index++) {
			r->parser_sha256[index] = d->parser_sha256[index];
			if (r->vbt_sha256[index] != d->parser_sha256[index])
				r->vbt_same_bytes = 0;
		}
	}

	/* Decides from the report alone. */
	drv_i915_native_decide(r);

	/* Succeeded: reports whether the start path applies. */
	return r->proceed;
}

/*
 * Decides from an N0 report whether the prepared start path applies.
 *
 * Every condition is recorded, so one native run names every wall it can
 * see; the first in decision order is the one that stops.  Reads only the
 * report.
 */
void
drv_i915_native_decide(
	struct i915_native_report *r)
{
	unsigned pipe;

	/* The driver writes GGTT pages [driver_ggtt_first, ggtt_pages): no firmware scanout may live there. */
	r->overlap = 0;
	if (r->fb_in_aperture != 0 && r->fb_ggtt_first + r->fb_ggtt_pages > r->driver_ggtt_first)
		r->overlap = 1;

	/* Nor may the surface of an active pipe. */
	for (pipe = 0U; pipe < 4U; pipe++) {
		if ((r->active_pipes & (1U << pipe)) != 0U && (r->pipe[pipe].plane_surf >> 12) + 1U > r->driver_ggtt_first)
			r->overlap = 1;
	}

	/* Records an active pipe. */
	r->conditions = 0U;
	if (r->active_pipes != 0U)
		r->conditions |= I915_N0_C_ACTIVE_PIPE;

	/* Records a pipe that did not read as register values. */
	for (pipe = 0U; pipe < 4U; pipe++) {
		if (r->pipe[pipe].cls == I915_N0_READ_ERROR)
			r->conditions |= I915_N0_C_PIPE_READ_ERROR;
	}

	/* Records a GGTT overlap. */
	if (r->overlap != 0)
		r->conditions |= I915_N0_C_GGTT_OVERLAP;

	/* Records the state of a VT-d unit the host does not own. */
	if (r->hypervisor == 0 && r->vtd_enabled != 0) {
		if (r->vtd_readable == 0) {
			r->conditions |= I915_N0_C_VTD_UNREADABLE;
		} else {
			/* GSTS.TES: translation enabled. */
			if ((r->vtd_gsts & 0x80000000U) != 0U)
				r->conditions |= I915_N0_C_VTD_TRANSLATION;

			/* PMEN.EPM / PRS: a protected memory region. */
			if ((r->vtd_pmen & 0x80000001U) != 0U)
				r->conditions |= I915_N0_C_VTD_PMR;

			/* GSTS.IRES: interrupt remapping. */
			if ((r->vtd_gsts & 0x02000000U) != 0U)
				r->conditions |= I915_N0_C_VTD_IR_ENABLED;
		}
	}

	/* Records an OpRegion. */
	if (r->asls != 0U)
		r->conditions |= I915_N0_C_OPREGION_REGISTER;

	/* Records an OpRegion VBT that is not the one the parser consumed. */
	if (r->vbt_valid != 0 && r->parser_sha_known != 0 && r->vbt_same_bytes == 0)
		r->conditions |= I915_N0_C_VBT_DIFFERS;

	/*
	 * Takes the first stop in decision order.
	 *
	 * XXX: the takeover test build did not stop on an active pipe (and
	 * then gave the reason that the takeover runs it); that switch is not
	 * carried, so in production an active pipe is the first stop.
	 */
	r->proceed = 0;
	r->primary_stop = 0U;
	if ((r->conditions & I915_N0_C_ACTIVE_PIPE) != 0U) {
		r->primary_stop = I915_N0_C_ACTIVE_PIPE;
		r->reason = "a pipe is active (firmware display): the takeover (N1: readout + crtc_disable_noatomic) is not ported";
	} else if ((r->conditions & I915_N0_C_PIPE_READ_ERROR) != 0U) {
		r->primary_stop = I915_N0_C_PIPE_READ_ERROR;
		r->reason = "a pipe's power state / registers did not read as register values: not shown inactive";
	} else if ((r->conditions & I915_N0_C_GGTT_OVERLAP) != 0U) {
		r->primary_stop = I915_N0_C_GGTT_OVERLAP;
		r->reason = "the firmware scanout overlaps the GGTT pages this driver writes";
	} else if ((r->conditions & I915_N0_C_VTD_UNREADABLE) != 0U) {
		r->primary_stop = I915_N0_C_VTD_UNREADABLE;
		r->reason = "the GPU's VT-d unit is enabled but its status is not readable";
	} else if ((r->conditions & (I915_N0_C_VTD_TRANSLATION | I915_N0_C_VTD_PMR)) != 0U) {
		r->primary_stop = r->conditions & (I915_N0_C_VTD_TRANSLATION | I915_N0_C_VTD_PMR);
		r->reason = "the GPU's VT-d unit translates / protects memory (firmware DMA protection) and zedBSD has no IOMMU driver";
	} else if (r->hypervisor != 0) {
		/* A guest sees the host's unit; the host owns it. */
		r->proceed = 1;
		r->reason = "start conditions match the prepared path (no active pipe, no overlap; VT-d: guest view, "
			"the host owns the unit)";
	} else {
		/* Native, with DMA untranslated. */
		r->proceed = 1;
		r->reason = "start conditions match the prepared path (no active pipe, no overlap, DMA untranslated)";
	}
}

/*
 * Logs the last N0 report again, as the final output of a run.
 *
 * Nothing is logged when N0 did not run.
 */
void
drv_i915_native_log_again(
	struct i915_display *display)
{
	/* N0 did not run: there is nothing to repeat. */
	if (display->n0_last == NULL)
		return;

	/* Repeats the summary. */
	kern_logf("i915: N0 ---- summary (repeated at the end of the run) ----\n");
	drv_i915_native_log(display, display->n0_last);
}

/*
 * Logs an N0 report and remembers it for drv_i915_native_log_again().
 */
void
drv_i915_native_log(
	struct i915_display *display,
	const struct i915_native_report *r)
{
	const struct i915_native_pipe *q;
	const char *class_name;
	char sha[17];
	char psha[17];
	unsigned pipe;

	/* Remembers the report for the summary at the end of the run. */
	display->n0_last = r;

	/* Spells the first 8 bytes of both hashes in hex. */
	i915_sha_prefix(r->vbt_sha256, sha);
	i915_sha_prefix(r->parser_sha256, psha);

	/* Logs the platform, the OpRegion and the VT-d unit. */
	kern_logf("i915: N0 platform: hypervisor=%d (CPUID.1:ECX[31]; recorded, not a driver mode)\n", r->hypervisor);
	kern_logf("i915: N0 opregion: ASLS=0x%08x mapped=%d sig=%d ver=%u.%u.%u size=%uKiB mboxes=0x%x | VBT via %s "
		"(rvda=0x%llx rvds=%u relative=%d inside=%d phys=0x%llx) mapped=%d size=%u valid=%d sha256=%s.. matches explicit pin=%d\n",
		r->asls, r->opregion_mapped, r->op.signature_ok, r->op.major, r->op.minor, r->op.revision, r->op.size_kib, r->op.mboxes,
		i915_opvbt_source_name(r->op.src),
		(unsigned long long)r->op.rvda, r->op.rvds, r->op.rvda_relative, r->op.rvda_inside, (unsigned long long)r->op.vbt_phys,
		r->vbt_mapped, r->vbt_size, r->vbt_valid, r->vbt_mapped ? sha : "-", r->vbt_matches_pin);
	kern_logf("i915: N0 vt-d (GPU unit): GFXVTBAR=0x%llx enabled=%d view=%s VER=0x%08x readable=%d GSTS=0x%08x "
		"(TES bit31) PMEN=0x%08x (EPM bit0 / PRS bit31)\n", (unsigned long long)r->gfxvtbar, r->vtd_enabled,
		r->hypervisor ? "guest" : "native", r->vtd_ver, r->vtd_readable, r->vtd_gsts, r->vtd_pmen);
	kern_logf("i915: N0 vt-d GSTS decoded: TES=%u RTPS=%u FLS=%u AFLS=%u WBFS=%u QIES=%u IRES=%u IRTPS=%u CFIS=%u | "
		"PMEN EPM=%u PRS=%u%s\n", (r->vtd_gsts >> 31) & 1U, (r->vtd_gsts >> 30) & 1U, (r->vtd_gsts >> 29) & 1U,
		(r->vtd_gsts >> 28) & 1U, (r->vtd_gsts >> 27) & 1U, (r->vtd_gsts >> 26) & 1U, (r->vtd_gsts >> 25) & 1U,
		(r->vtd_gsts >> 24) & 1U, (r->vtd_gsts >> 23) & 1U, r->vtd_pmen & 1U, (r->vtd_pmen >> 31) & 1U,
		r->vtd_readable ? "" : " (not register values)");

	/* Logs the VBT the OpRegion carries against the one the parser adopted. */
	kern_logf("i915: N0 VBT: observed %s %u bytes sha256=%s.. | adopted by the parser: %s %u bytes sha256=%s.. | "
		"same bytes=%d\n", i915_opvbt_observed_name(r->op.src), r->vbt_size, r->vbt_mapped ? sha : "-",
		i915_vbt_source_name(r->parser_src), r->parser_size, r->parser_sha_known ? psha : "-",
		r->vbt_same_bytes);

	/* Logs the firmware framebuffer and the GGTT pages. */
	kern_logf("i915: N0 firmware framebuffer: present=%d base=0x%llx size=0x%llx in_aperture=%d GGTT pages %u..%u | "
		"driver writes GGTT pages %u..%u overlap=%d\n", r->fb_present, (unsigned long long)r->fb_base,
		(unsigned long long)r->fb_size, r->fb_in_aperture, r->fb_ggtt_first, r->fb_ggtt_first + r->fb_ggtt_pages,
		r->driver_ggtt_first, r->ggtt_pages, r->overlap);

	/* Logs every pipe. */
	for (pipe = 0U; pipe < 4U; pipe++) {
		/* A pipe that could not be read names why. */
		q = &r->pipe[pipe];
		if (q->readable == 0) {
			if (q->cls == I915_N0_READ_ERROR)
				class_name = "READ_ERROR (the power state did not read as a register value; NOT counted inactive)";
			else
				class_name = "POWER_OFF";

			/* Reports the unreadable pipe. */
			kern_logf("i915: N0 pipe %c: %s -- not_readable (its wells' STATE is off: counted inactive, as the reference readout "
				"concludes; its registers were not read)\n", 'A' + (int)pipe, class_name);
			continue;
		}

		/* A readable pipe names its class and its registers. */
		if (q->cls == I915_N0_READABLE_ACTIVE)
			class_name = "READABLE_ACTIVE";
		else if (q->cls == I915_N0_READ_ERROR)
			class_name = "READ_ERROR (a pipe register read all-ones)";
		else
			class_name = "READABLE_INACTIVE";

		/* Reports the class and the registers. */
		kern_logf("i915: N0 pipe %c: %s\n", 'A' + (int)pipe, class_name);
		kern_logf("i915: N0 pipe %c: TRANSCONF=0x%08x TRANS_DDI_FUNC_CTL=0x%08x PIPESRC=0x%08x PLANE_CTL=0x%08x "
			"PLANE_SURF=0x%08x STRIDE=0x%08x SIZE=0x%08x\n", 'A' + (int)pipe, q->transconf, q->trans_ddi_func, q->pipesrc,
			q->plane_ctl, q->plane_surf, q->plane_stride, q->plane_size);
	}

	/* Logs the pipe A power domain, which was read only when pipe A was readable. */
	if (r->pipe[0].readable == 0) {
		kern_logf("i915: N0 pipe-A domain: not read (pipe A's wells are off)\n");
	} else {
		kern_logf("i915: N0 pipe-A domain: DPLL0_ENABLE=0x%08x DPLL1_ENABLE=0x%08x DPCLKA_CFGCR0=0x%08x (PHY A -> DPLL %u) "
			"DDI_BUF_CTL_A=0x%08x PP_STATUS=0x%08x PP_CONTROL=0x%08x BLC_PWM_CTL=0x%08x BLC_PWM_DUTY=0x%08x\n", r->pll_enable[0],
			r->pll_enable[1], r->dpclka_cfgcr0, r->dpclka_cfgcr0 & 3U, r->ddi_buf_ctl_a, r->pp_status, r->pp_control, r->blc_ctl,
			r->blc_duty);
	}

	/* Logs the decision and every condition observed. */
	kern_logf("i915: N0 decision: %s -- %s (active pipes 0x%x, unreadable 0x%x)\n", r->proceed ? "PROCEED" : "STOP before "
		"any display write", r->reason, r->active_pipes, r->unreadable_pipes);
	kern_logf("i915: N0 conditions: primary=0x%x observed=0x%x [%s%s%s%s%s%s%s%s%s]\n", r->primary_stop, r->conditions,
		(r->conditions & I915_N0_C_ACTIVE_PIPE) ? " ACTIVE_PIPE" : "",
		(r->conditions & I915_N0_C_PIPE_READ_ERROR) ? " PIPE_READ_ERROR" : "",
		(r->conditions & I915_N0_C_GGTT_OVERLAP) ? " GGTT_OVERLAP" : "",
		(r->conditions & I915_N0_C_VTD_UNREADABLE) ? " VTD_UNREADABLE" : "",
		(r->conditions & I915_N0_C_VTD_TRANSLATION) ? " VTD_TRANSLATION" : "",
		(r->conditions & I915_N0_C_VTD_PMR) ? " VTD_PMR" : "",
		(r->conditions & I915_N0_C_VTD_IR_ENABLED) ? " VTD_IR_ENABLED(check the MSI path)" : "",
		(r->conditions & I915_N0_C_OPREGION_REGISTER) ? " OPREGION_PRESENT(data only; runtime disabled)" : "",
		(r->conditions & I915_N0_C_VBT_DIFFERS) ? " VBT_DIFFERS" : "");
}

/*
 * Reads the OpRegion as data and copies the VBT it names (the P2
 * OpRegion acquisition, VBT_ONLY).
 *
 * The OpRegion is mapped read-only, its 8 KiB are copied to
 * display->i915_opregion_read_data_opcopy and the VBT to
 * i915_opregion_read_data_vbtcopy, which out->vbt points at when the VBT is
 * valid.  The runtime protocol is not joined (runtime = DISABLED).  Returns
 * 0 (also when there is no OpRegion), EIO when the OpRegion or its VBT could
 * not be mapped, or EINVAL when the OpRegion names no VBT that can be copied.
 */
int
drv_i915_opregion_read_data(
	struct i915_display *display,
	uint32_t asls,
	struct i915_opregion_data *out)
{
	uint8_t *opcopy;
	uint8_t *vbtcopy;
	void *op;
	void *vb;
	uint32_t length;
	unsigned index;
	int error;

	/* The copies live in the display. */
	opcopy = display->i915_opregion_read_data_opcopy;
	vbtcopy = display->i915_opregion_read_data_vbtcopy;

	/* Starts with the runtime protocol disabled, which it always is here. */
	kern_memset(out, 0, sizeof(*out));
	out->runtime_enabled = 0;
	out->runtime_reason = "ACPI_RUNTIME_UNAVAILABLE (no AML / ACPI event delivery in zedBSD): the OpRegion is used as data only";

	/* ASLS 0 means there is no OpRegion. */
	out->present = 0;
	if (asls != 0U)
		out->present = 1;
	if (out->present == 0)
		return 0;

	/* READ-ONLY view: this driver cannot write the shared mailboxes through it. */
	op = NULL;
	error = hal_space_map_device((hal_physaddr_t)asls, I915_OPREGION_SIZE, HAL_SPACE_READ, &op);
	if (error != HAL_OK)
		return EIO;
	if (op == NULL)
		return EIO;

	/* Copies the OpRegion and drops the mapping. */
	out->mapped = 1;
	for (index = 0U; index < I915_OPREGION_SIZE; index++)
		opcopy[index] = ((const volatile uint8_t *)op)[index];
	(void)hal_space_unmap_device(op, I915_OPREGION_SIZE);

	/* Finds where the OpRegion keeps the VBT. */
	error = drv_i915_opregion_locate_vbt(opcopy, I915_OPREGION_SIZE, asls, &out->op);
	if (error != 0)
		return EINVAL;

	/* Copies the VBT from RVDA, or from mailbox #4 inside the OpRegion copy. */
	if (out->op.src == I915_OPVBT_RVDA) {
		/* Refuses an RVDA VBT larger than the copy, or one inside the OpRegion. */
		length = out->op.rvds;
		if (length > I915_NATIVE_VBT_COPY_SIZE)
			return EINVAL;
		if (out->op.rvda_inside != 0)
			return EINVAL;

		/* Maps the RVDA VBT read-only. */
		vb = NULL;
		error = hal_space_map_device((hal_physaddr_t)out->op.vbt_phys, length, HAL_SPACE_READ, &vb);
		if (error != HAL_OK)
			return EIO;
		if (vb == NULL)
			return EIO;

		/* Copies it and drops the mapping. */
		for (index = 0U; index < length; index++)
			vbtcopy[index] = ((const volatile uint8_t *)vb)[index];
		(void)hal_space_unmap_device(vb, length);
		out->vbt_size = length;
	} else {
		/* Copies the mailbox #4 VBT out of the OpRegion copy. */
		out->vbt_size = out->op.vbt_max;
		for (index = 0U; index < out->vbt_size; index++)
			vbtcopy[index] = opcopy[out->op.vbt_offset + index];
	}

	/* Validates the VBT and hashes a valid one for the parser. */
	out->vbt_valid = drv_i915_vbt_validate(vbtcopy, out->vbt_size);
	if (out->vbt_valid != 0) {
		out->vbt = vbtcopy;
		drv_i915_sha256(vbtcopy, out->vbt_size, out->vbt_sha256);
	}

	/* Succeeded: the OpRegion data and its VBT are in out. */
	return 0;
}

/*
 * Logs the OpRegion data and the runtime mailboxes as found.
 */
void
drv_i915_opregion_log(
	const struct i915_opregion_data *d)
{
	const char *data_state;

	/* No OpRegion: nothing but its absence to report. */
	if (d->present == 0) {
		kern_logf("i915: P2 opregion: absent (ASLS=0): no OpRegion data, no runtime protocol\n");
		return;
	}

	/* The data is available when the OpRegion was mapped and carries its signature. */
	data_state = "UNAVAILABLE";
	if (d->mapped != 0 && d->op.signature_ok != 0)
		data_state = "AVAILABLE";

	/* Logs the data and the mailboxes; this driver writes none of them. */
	kern_logf("i915: P2 opregion: data=%s ver=%u.%u mboxes=0x%x VBT via %s size=%u valid=%d | runtime=DISABLED (%s)\n",
		data_state, d->op.major, d->op.minor, d->op.mboxes, i915_opvbt_source_name(d->op.src), d->vbt_size,
		d->vbt_valid, d->runtime_reason);
	kern_logf("i915: P2 opregion runtime mailboxes as found (observed only; this driver writes none of them): "
		"ACPI drdy=0x%x csts=0x%x cevt=0x%x chpd=0x%x clid=0x%x | ASLE ardy=0x%x aslc=0x%x tche=0x%x\n", d->op.acpi_drdy,
		d->op.acpi_csts, d->op.acpi_cevt, d->op.acpi_chpd, d->op.acpi_clid, d->op.asle_ardy, d->op.asle_aslc, d->op.asle_tche);
}

/*
 * Reports whether a pipe's power domains are on, for the N0 readout gate.
 *
 * The pipe_powered callback of struct i915_native_deps: ctx is the struct
 * i915_display (its power_domains and the n0_mmio N0 reads with).  It reads
 * the wells' real STATE, since before init_hw the driver neither synced nor
 * took over the firmware's requests.  Returns 1 when both the pipe and the
 * transcoder domains are on, else 0.
 */
int
drv_i915_n0_pipe_powered(
	void *ctx,
	unsigned pipe)
{
	struct i915_display *display;
	int pipe_on;
	int transcoder_on;

	/* The context is the display N0 runs on. */
	display = ctx;

	/* The pipe domain must be on. */
	pipe_on = drv_i915_power_domain_hw_state_on(&display->power_domains, (enum i915_power_domain)(I915_PW_DOMAIN_PIPE_A + pipe), display->n0_mmio);
	if (pipe_on == 0)
		return 0;

	/* So must the transcoder domain. */
	transcoder_on = drv_i915_power_domain_hw_state_on(&display->power_domains, (enum i915_power_domain)(I915_PW_DOMAIN_TRANSCODER_A + pipe), display->n0_mmio);
	if (transcoder_on == 0)
		return 0;

	/* Succeeded: the pipe's registers are readable. */
	return 1;
}

/*
 * Reads the firmware's primary plane of a pipe, as diagnostics for the
 * takeover.
 *
 * The reference's readout does not read the plane geometry ("FIXME read out
 * full plane state for all planes"), so PLANE_CTL, PLANE_STRIDE, PLANE_SIZE,
 * PLANE_OFFSET and PLANE_SURF are read here, before anything changes.
 */
void
drv_i915_n1_read_plane(
	const struct i915_lcd_kernel_deps *d,
	int pipe,
	uint32_t *ctl,
	uint32_t *stride,
	uint32_t *size,
	uint32_t *surf,
	uint32_t *offset)
{
	uint32_t base;

	/* The primary plane registers of the pipe. */
	base = I915_PLANE_CTL_1_A + I915_PIPE_STRIDE * (uint32_t)pipe;

	/* Reads them in the order the takeover always has. */
	*ctl = drv_i915_read32(d->mmio, base);
	*stride = drv_i915_read32(d->mmio, base + 0x08U);
	*size = drv_i915_read32(d->mmio, base + 0x10U);
	*offset = drv_i915_read32(d->mmio, base + 0x14U);
	*surf = drv_i915_read32(d->mmio, base + 0x1cU);
}

/*
 * Returns the console's own backing as the boot handoff describes it.
 *
 * Read only; NULL when the loader left no framebuffer.
 */
const struct zbl6_framebuffer *
drv_i915_n1_console_fb(void)
{
	const struct zbl6_framebuffer *fb;

	/* Looks the framebuffer up in the boot handoff. */
	fb = kern_boot_handoff("pcat.framebuffer");

	/* Succeeded: reports the framebuffer, or NULL. */
	return fb;
}

/*
 * Checks that the GGTT range the firmware plane reads carries the console's
 * pixels.
 *
 * Answered from the table, not from the surface register: the PTEs of the
 * first, second, middle and last page are read back and compared with the
 * backing the boot handoff reports.  When the console writes through the
 * aperture (in_aperture), the pixels are wherever those same PTEs point, so
 * the plane and the CPU agree by construction and 1 is reported.  Returns 1
 * when the pages match, else 0.
 */
int
drv_i915_n1_check_ggtt(
	const struct i915_lcd_kernel_deps *d,
	uint64_t fw_surf,
	unsigned pages,
	uint64_t fb_phys,
	int in_aperture)
{
	unsigned probe[4];
	unsigned first;
	unsigned count;
	unsigned index;
	uint64_t pte;
	uint64_t pa;
	uint64_t want;
	const char *verdict;
	int ok;

	/* The first GGTT page of the firmware surface. */
	first = (unsigned)(fw_surf >> 12);

	/* Always checks the first page. */
	count = 0U;
	probe[count] = 0U;
	count++;

	/* Checks the second page of a range longer than two pages. */
	if (pages > 2U) {
		probe[count] = 1U;
		count++;
	}

	/* Checks the middle page of a range longer than four pages. */
	if (pages > 4U) {
		probe[count] = pages / 2U;
		count++;
	}

	/* Checks the last page of a range longer than one page. */
	if (pages > 1U) {
		probe[count] = pages - 1U;
		count++;
	}

	/* Compares each picked PTE with the console page it should map. */
	ok = 1;
	for (index = 0U; index < count; index++) {
		/* Reads the PTE back and the page the console has there. */
		pte = drv_i915_gt_ggtt_read_pte(d->gm, first + probe[index]);
		pa = pte & ~(uint64_t)0xfff;
		want = fb_phys + (uint64_t)probe[index] * 4096U;

		/* Names the verdict: not present, the same page, or another page. */
		if ((pte & 1U) == 0U)
			verdict = "NOT PRESENT";
		else if (pa == want)
			verdict = "same";
		else
			verdict = "DIFFERS";
		kern_logf("i915: N1 ggtt[%u] (page %u) = 0x%016llx -> 0x%llx | console page 0x%llx | %s\n",
			probe[index], first + probe[index], (unsigned long long)pte, (unsigned long long)pa,
			(unsigned long long)want, verdict);

		/* A PTE that is absent or points elsewhere fails the check. */
		if ((pte & 1U) == 0U || pa != want)
			ok = 0;
	}

	/* The console writes through this very mapping: plane and CPU read/write the same pages. */
	if (in_aperture != 0) {
		kern_logf("i915: N1 ggtt: the console writes THROUGH the aperture, so the plane and the CPU "
			"use the same mapping by construction\n");
		return 1;
	}

	/* Succeeded: reports whether every picked page matched. */
	return ok;
}

/*
 * Mirrors the kernel console into the buffer the panel shows.
 *
 * The console keeps drawing into the firmware's framebuffer in memory; its
 * visible rectangle is copied from there into so's CPU view (R,G,B,X is
 * swapped to B,G,R,X when the console is RGBX) and so is published.
 * Nothing is written to the firmware's memory.  Returns 0 when the console
 * was mirrored and so published, or ENOENT when there is nothing to mirror
 * (so has no CPU view, or there is no console framebuffer); the caller
 * counts the mirrors and records the ENOENT.
 */
int
drv_i915_n1_mirror_console(
	struct i915_scanout *so)
{
	volatile uint32_t *pixels;
	volatile uint32_t *source;
	uint32_t *destination;
	uint32_t pixel;
	unsigned width;
	unsigned height;
	unsigned stride;
	unsigned rows;
	unsigned columns;
	unsigned row;
	unsigned column;
	int rgbx;
	int available;

	/* A scanout without a CPU view has nowhere to mirror into. */
	if (so->cpu == NULL)
		return ENOENT;

	/* Asks the console backend for its framebuffer. */
	pixels = NULL;
	width = 0U;
	height = 0U;
	stride = 0U;
	rgbx = 0;
	available = drv_pcat_graphics_backend_get_framebuffer(&pixels, &width, &height, &stride, &rgbx);
	if (available == 0)
		return ENOENT;
	if (pixels == NULL)
		return ENOENT;

	/* Mirrors the rectangle both buffers have. */
	rows = (height < so->height) ? height : so->height;
	columns = (width < so->width) ? width : so->width;
	for (row = 0U; row < rows; row++) {
		/* The same row in both buffers. */
		source = pixels + (size_t)row * stride;
		destination = so->cpu + (size_t)row * (so->pitch / 4U);

		/* Copies an XRGB row as it is, and swaps an RGBX row to XRGB. */
		if (rgbx == 0) {
			for (column = 0U; column < columns; column++)
				destination[column] = source[column];
		} else {
			for (column = 0U; column < columns; column++) {
				/* R,G,B,X -> B,G,R,X */
				pixel = source[column];
				destination[column] = (pixel & 0xff00ff00U) | ((pixel & 0x00ff0000U) >> 16) | ((pixel & 0x000000ffU) << 16);
			}
		}
	}

	/* Makes the copied pixels visible to the display engine. */
	drv_i915_scanout_publish(so);

	/* Succeeded: the console is on the panel's buffer. */
	return 0;
}

/* Reports the device the readout text treats as current (the modeset world's current device). */
static struct drm_i915_private *
i915_takeover_cur_i915(
	struct i915_takeover_world *takeover)
{
	/* The modeset world holds it; the readout entry points set it to the registry's device. */
	return takeover->display->lcd_world->i915_lcd_cur_i915;
}

/* Reports the watermark context the readout's DBUF and crtc-state questions answer from. */
static struct i915_lcd_wm_ctx *
i915_takeover_wm(
	struct i915_takeover_world *takeover)
{
	/* The watermark world holds the context of the modeset last computed. */
	return takeover->display->wm_world->i915_lcd_wm;
}

/* Reports whether the display's register-trace switch is on. */
static int
i915_takeover_reg_trace(
	const struct i915_takeover_world *takeover)
{
	/* The switch is a diagnostic of the display. */
	return takeover->display->i915_lcd_reg_trace;
}

/* Reports the takeover world a registry device belongs to. */
static struct i915_takeover_world *
i915_takeover_of_device(
	const struct drm_device *dev)
{
	struct drm_i915_private *i915;
	struct i915_n1_registry *n1;

	/* The DRM device is the registry's device, which the registry embeds. */
	i915 = i915_lcd_to_i915(dev);
	n1 = container_of(i915, struct i915_n1_registry, i915);

	/* The registry is the world's own. */
	return container_of(n1, struct i915_takeover_world, n1);
}

/* Reads a registry plane through the plane text's hook, printing the call while the trace is on. */
static bool
i915_n1_plane_get_hw_state(
	struct intel_plane *plane,
	enum pipe *pipe)
{
	struct i915_takeover_world *takeover;
	bool (*hook)(struct intel_plane *plane, enum pipe *pipe);
	bool visible;
	int trace;

	/*
	 * Finds the world of the plane for the trace switch: the call itself is
	 * what a console can see.
	 */
	takeover = i915_takeover_of_device(plane->base.dev);
	trace = i915_takeover_reg_trace(takeover);
	if (trace != 0)
		kern_logf("i915: N1 plane hook entered\n");

	/* Takes the plane text's readout hook. */
	hook = i915_lcd_plane_get_hw_state();
	if (trace != 0) {
		kern_logf("i915: N1 plane %d hook %p\n",
			  (int)plane->pipe,
			  (void *)hook);
	}

	/* Reads the plane. */
	visible = hook(plane, pipe);
	if (trace != 0) {
		kern_logf("i915: N1 plane %d visible=%d\n",
			  (int)plane->pipe,
			  (int)visible);
	}

	/* Succeeded: reports whether the plane scans out. */
	return visible;
}

/*
 * Builds the registry's device and objects from a modeset configuration.
 *
 * The readout reads the hardware, but it needs the platform's DBUF
 * geometry, the watermark latencies, the clocks and the PLL pool to
 * interpret what it reads: the fields are the ones the modeset object is
 * given from the same configuration.
 */
static void
i915_n1_build_device(
	struct i915_takeover_world *takeover,
	const struct i915_lcd_modeset_cfg *cfg,
	struct i915_lcd_emit *ops)
{
	struct i915_n1_registry *n1;
	struct drm_i915_private *i915;
	unsigned i;

	/* Clears the whole registry: nothing of an earlier readout survives. */
	n1 = &takeover->n1;
	i915 = &n1->i915;
	kern_memset(n1, 0, sizeof(*n1));

	/* The backend and the two display locks of the device. */
	i915->emit = ops;
	i915->display.dpll.lock.which = I915_LCD_LOCK_DPLL;
	i915->display.backlight.lock.which = I915_LCD_LOCK_BACKLIGHT;

	/* The VBT and DMC facts the readout reads. */
	i915->display.vbt.override_afc_startup = cfg->vbt_override_afc_startup != 0;
	i915->display.dmc.fw_mask = cfg->dmc_fw_mask;

	/* The watermark latencies and the SAGV block time the normal initialisation read. */
	kern_memcpy(i915->display.wm.skl_latency, cfg->wm_latency, sizeof(i915->display.wm.skl_latency));
	i915->display.wm.num_levels = cfg->wm_num_levels;
	i915->display.wm.ipc_enabled = cfg->wm_ipc_enabled != 0;
	i915->display.sagv.block_time_us = cfg->sagv_block_time_us;

	/* The platform's DBUF geometry, its DDI outputs and its four pipes. */
	i915->display.device_info.dbuf.size = cfg->dbuf_size;
	i915->display.device_info.dbuf.slice_mask = cfg->dbuf_slice_mask;
	i915->display.device_info.has_ddi = true;
	i915->display.runtime.pipe_mask = 0x0f;
	i915->display.runtime.rawclk_freq = cfg->rawclk_khz;

	/* The CDCLK state the normal initialisation left, and the platform limit. */
	i915->display.cdclk.hw.cdclk = cfg->cdclk_khz;
	i915->display.cdclk.hw.vco = cfg->cdclk_vco_khz;
	i915->display.cdclk.hw.ref = cfg->cdclk_ref_khz;
	i915->display.cdclk.hw.bypass = cfg->cdclk_bypass_khz;
	i915->display.cdclk.hw.voltage_level = cfg->cdclk_voltage_level;
	i915->display.cdclk.max_cdclk_freq = cfg->cdclk_max_khz;

	/* The display and colour hooks the rest of the modeset path lends the readout. */
	i915->display.funcs.display = drv_i915_lcd_ms_display_funcs();
	i915->display.funcs.color = drv_i915_lcd_ms_color_funcs();

	/* The device's vblank objects and the DBUF slices enabled now. */
	i915->drm.vblank = n1->vblank;
	i915->display.dbuf.enabled_slices = cfg->dbuf_enabled_slices;

	/*
	 * The global states the readout fills and the takeover clears.  In
	 * Linux each is the payload of a global state object; here the object
	 * IS the payload, so the pointer is the address of the payload.
	 */
	i915->display.dbuf.obj.state = (struct intel_global_state *)(void *)&n1->dbuf;
	i915->display.bw.obj.state = (struct intel_global_state *)(void *)&n1->bw;
	i915->display.cdclk.obj.state = (struct intel_global_state *)(void *)&n1->cdclk;
	i915->display.pmdemand.obj.state = (struct intel_global_state *)(void *)&n1->pmdemand;

	/* The device's PLL pool: the same objects every screen sees (intel_get_shared_dpll_by_id). */
	drv_i915_lcd_dpll_pool_bind(takeover->display->lcd_world, i915);

	/* Builds one crtc, its state and its primary plane per pipe. */
	for (i = 0u; i < (unsigned)I915_N1_PIPES; i++) {
		/* The crtc of the pipe, reading its frame counter through the modeset path's hooks. */
		n1->crtc[i].base.dev = &i915->drm;
		n1->crtc[i].base.name = "pipe";
		n1->crtc[i].base.base.id = (int)i;
		n1->crtc[i].pipe = (enum pipe)i;
		n1->crtc[i].base.state = &n1->crtc_state[i].uapi;
		n1->crtc[i].base.funcs = drv_i915_lcd_ms_crtc_funcs();
		n1->vblank[i].max_vblank_count = 0xffffffffu;
		n1->crtc_state[i].uapi.crtc = &n1->crtc[i].base;

		/* The primary plane of the pipe, read through the registry's traced hook. */
		n1->plane[i].base.dev = &i915->drm;
		n1->plane[i].base.name = "primary";
		n1->plane[i].base.base.id = 16 + (int)i;
		n1->plane[i].base.type = DRM_PLANE_TYPE_PRIMARY;
		n1->plane[i].id = PLANE_PRIMARY;
		n1->plane[i].pipe = (enum pipe)i;
		n1->plane[i].get_hw_state = i915_n1_plane_get_hw_state;
		n1->plane[i].base.state = (struct drm_plane_state *)(void *)&n1->plane_state[i];
		n1->plane_state[i].uapi.plane = &n1->plane[i].base;
		n1->crtc[i].base.primary = &n1->plane[i].base;
	}

	/* Reports the registry, with where it and its first plane hook are. */
	kern_logf("i915: N1 registry: %u built | &n1=0x%llx &plane0=0x%llx hook=0x%llx\n",
		  (unsigned)I915_N1_PIPES,
		  (unsigned long long)(uintptr_t)n1,
		  (unsigned long long)(uintptr_t)&n1->plane[0],
		  (unsigned long long)(uintptr_t)n1->plane[0].get_hw_state);
}

/* Describes the registry in a report: the first active pipe, its mode and what the takeover did. */
static void
i915_n1_fill_report(
	struct i915_takeover_world *takeover,
	struct i915_n1_report *out)
{
	const struct intel_crtc_state *crtc_state;
	struct i915_n1_registry *n1;
	unsigned i;

	/* Starts from an empty report: no pipe, transcoder, PLL or port yet. */
	n1 = &takeover->n1;
	kern_memset(out, 0, sizeof(*out));
	out->pipe = -1;
	out->cpu_transcoder = -1;
	out->dpll_id = -1;
	out->port = -1;

	/* Collects the active pipes, describing the first one. */
	for (i = 0u; i < (unsigned)I915_N1_PIPES; i++) {
		crtc_state = &n1->crtc_state[i];

		/* An inactive pipe is not reported. */
		if (!crtc_state->hw.active)
			continue;

		/* Every active pipe is in the mask. */
		out->active_pipes |= 1u << i;

		/* Only the first active pipe is described. */
		if (out->pipe >= 0)
			continue;

		/* The pipe, its transcoder and the PLL it uses. */
		out->pipe = (int)i;
		out->cpu_transcoder = crtc_state->cpu_transcoder;
		if (crtc_state->shared_dpll != NULL) {
			out->dpll_id = (int)crtc_state->shared_dpll->info->id;
		} else {
			out->dpll_id = -1;
		}

		/* The mode and the clocks the firmware runs. */
		out->mode_h = (unsigned)crtc_state->hw.adjusted_mode.hdisplay;
		out->mode_v = (unsigned)crtc_state->hw.adjusted_mode.vdisplay;
		out->clock_khz = (unsigned)crtc_state->hw.adjusted_mode.clock;
		out->port_clock_khz = (unsigned)crtc_state->port_clock;
		out->pipe_bpp = crtc_state->pipe_bpp;
		out->output_types = crtc_state->output_types;

		/* Whether the pipe's primary plane scans out, and the planes that do. */
		if (n1->plane_state[i].uapi.visible) {
			out->plane_visible = 1;
		} else {
			out->plane_visible = 0;
		}
		out->active_planes = crtc_state->active_planes;
	}

	/* The port of the bound encoder. */
	if (n1->encoder != NULL)
		out->port = (int)n1->encoder->port;

	/* Whether the readout linked the encoder to a crtc. */
	out->encoder_on_crtc = 0;
	if (n1->encoder != NULL) {
		if (n1->encoder->base.crtc != NULL)
			out->encoder_on_crtc = 1;
	}

	/* The connector's DPMS state, or -1 without a connector. */
	if (n1->connector != NULL) {
		out->connector_dpms = n1->connector->base.dpms;
	} else {
		out->connector_dpms = -1;
	}

	/* What the takeover did and whether the registry is live. */
	out->takeovers = n1->takeovers;
	out->still_active = n1->still_active;
	out->live = n1->live;
}

/* Tells whether a Type-C link of an encoder on the crtc must be reset first (intel_crtc_needs_link_reset()). */
static bool
i915_crtc_needs_link_reset(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc)
{
	struct drm_i915_private *cur_i915;
	struct intel_digital_port *dig_port;
	struct intel_encoder *encoder;
	unsigned index;
	bool needs_reset;

	/* The unported step is recorded on the current device. */
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* Asks the digital port of every encoder on the crtc. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		if (encoder->base.crtc != &crtc->base)
			continue;

		/* Only a digital port can carry a Type-C link. */
		dig_port = i915_lcd_enc_to_dig_port(encoder);
		if (dig_port == NULL)
			continue;

		/* A Type-C port may need its link reset. */
		needs_reset = I915_TAKEOVER_INTEL_TC_PORT_LINK_NEEDS_RESET(cur_i915, dig_port);
		if (needs_reset)
			return true;
	}

	/* No encoder of the crtc needs a link reset. */
	return false;
}

/* Collects the big-joiner slave pipes of the master pipes of a mask (get_bigjoiner_slave_pipes()). */
static u8
i915_get_bigjoiner_slave_pipes(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915,
	u8 master_pipes_mask)
{
	struct intel_crtc *master_crtc;
	unsigned index;
	u8 pipes;

	UNUSED_PARAMETER(i915);

	/* Adds the slaves each master crtc's state names (none: one pipe per stream here). */
	pipes = 0;
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, master_crtc, index, master_pipes_mask) {
		pipes |= intel_crtc_bigjoiner_slave_pipes(to_intel_crtc_state(master_crtc->base.state));
	}

	/* Succeeded: reports the slave pipes. */
	return pipes;
}

/*
 * Reports the port sync master and slave pipes linked to a crtc (get_portsync_pipes()).
 *
 * For big-joiner configurations only the big-joiner master pipes are
 * reported.
 */
static void
i915_get_portsync_pipes(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc,
	u8 *master_pipe_mask,
	u8 *slave_pipes_mask)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct intel_crtc_state *crtc_state;
	struct intel_crtc *master_crtc;
	struct intel_crtc_state *master_crtc_state;
	enum transcoder master_transcoder;
	bool sync_master;

	/* Resolves the crtc's device and state, and the device of the steps. */
	i915 = i915_lcd_to_i915(crtc->base.dev);
	crtc_state = to_intel_crtc_state(crtc->base.state);
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* A crtc outside port sync is its own master and has no slaves. */
	if (!is_trans_port_sync_mode(crtc_state)) {
		*master_pipe_mask = BIT(crtc->pipe);
		*slave_pipes_mask = 0;

		return;
	}

	/* The master transcoder is the crtc's own when it is the port sync master. */
	sync_master = I915_TAKEOVER_IS_TRANS_PORT_SYNC_MASTER(cur_i915, crtc_state);
	if (sync_master) {
		master_transcoder = crtc_state->cpu_transcoder;
	} else {
		master_transcoder = crtc_state->master_transcoder;
	}

	/* The pipes of the master transcoder: exactly one is expected. */
	*master_pipe_mask = i915_get_transcoder_pipes(takeover, i915, BIT(master_transcoder));
	if (!is_power_of_2(*master_pipe_mask))
		drv_i915_lcd_error("WARN_ON(!is_power_of_2(*master_pipe_mask))\n");

	/* The slaves are the pipes of the transcoders the master's state names. */
	master_crtc = I915_TAKEOVER_INTEL_CRTC_FOR_PIPE(takeover, i915, ffs(*master_pipe_mask) - 1);
	master_crtc_state = to_intel_crtc_state(master_crtc->base.state);
	*slave_pipes_mask = i915_get_transcoder_pipes(takeover, i915, master_crtc_state->sync_mode_slaves_mask);
}

/*
 * Collects the pipes using a transcoder of a mask (get_transcoder_pipes()).
 *
 * For big-joiner configurations only the big-joiner master is reported.
 */
static u8
i915_get_transcoder_pipes(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915,
	u8 transcoder_mask)
{
	struct intel_crtc *temp_crtc;
	struct intel_crtc_state *temp_crtc_state;
	unsigned index;
	u8 pipes;

	UNUSED_PARAMETER(i915);

	/* Walks every crtc of the registry. */
	pipes = 0;
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, temp_crtc, index) {
		temp_crtc_state = to_intel_crtc_state(temp_crtc->base.state);

		/* A crtc without a transcoder uses none of the mask. */
		if (temp_crtc_state->cpu_transcoder == INVALID_TRANSCODER)
			continue;

		/* A big-joiner slave is reported through its master. */
		if (intel_crtc_is_bigjoiner_slave(temp_crtc_state))
			continue;

		/* The crtc's pipe counts when its transcoder is in the mask. */
		if (transcoder_mask & BIT(temp_crtc_state->cpu_transcoder))
			pipes |= BIT(temp_crtc->pipe);
	}

	/* Succeeded: reports the pipes. */
	return pipes;
}

/*
 * Disables one active crtc through the display hooks (intel_crtc_disable_noatomic_begin()).
 *
 * The visible planes go first, then the crtc is disabled with the
 * registry's throw-away atomic state, and its reference on the shared DPLL
 * is dropped.
 */
static void
i915_crtc_disable_noatomic_begin(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct i915_lcd_wm_ctx *wm;
	struct intel_crtc_state *crtc_state;
	const struct intel_plane_state *plane_state;
	struct intel_crtc_state *temp_crtc_state;
	struct intel_atomic_state *state;
	struct intel_crtc *temp_crtc;
	struct intel_plane *plane;
	enum pipe pipe;
	unsigned index;
	int ret;

	/* Resolves the crtc's device, state and pipe, and the readout context. */
	i915 = i915_lcd_to_i915(crtc->base.dev);
	crtc_state = to_intel_crtc_state(crtc->base.state);
	pipe = crtc->pipe;
	cur_i915 = i915_takeover_cur_i915(takeover);
	wm = i915_takeover_wm(takeover);

	/* An inactive crtc has nothing to disable. */
	if (!crtc_state->hw.active)
		return;

	/* Disables every visible plane of the crtc first. */
	I915_TAKEOVER_FOR_EACH_INTEL_PLANE_ON_CRTC(takeover, crtc, plane) {
		plane_state = to_intel_plane_state(plane->base.state);
		if (plane_state->uapi.visible)
			drv_i915_plane_disable_noatomic(takeover, crtc, plane);
	}

	/* Takes the throw-away atomic state of the disable. */
	state = I915_TAKEOVER_DRM_ATOMIC_STATE_ALLOC(takeover, &i915->drm);
	if (state == NULL) {
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "failed to disable [CRTC:%d:%s], out of memory",
				     crtc->base.base.id,
				     crtc->base.name);
		return;
	}

	/* The state carries the lock context and is the driver's own. */
	state->acquire_ctx = ctx;
	state->internal = true;

	/*
	 * Adds the crtc and its big-joiner slaves to the state.  Everything is
	 * already locked, so -EDEADLK cannot happen.
	 */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, temp_crtc, index, BIT(pipe) | intel_crtc_bigjoiner_slave_pipes(crtc_state)) {
		/* Records the crtc's state as the old state of the disable. */
		temp_crtc_state = I915_TAKEOVER_INTEL_ATOMIC_GET_CRTC_STATE(takeover, wm, state, temp_crtc);

		/* Adds the crtc's connectors (a step of the DRM atomic core, not ported). */
		ret = I915_TAKEOVER_DRM_ATOMIC_ADD_AFFECTED_CONNECTORS(cur_i915, state, &temp_crtc->base);

		/*
		 * Warns when either failed.  No accessor of this path returns an
		 * error pointer (IS_ERR() is false), so only the connector step
		 * can fail.
		 */
		(void)temp_crtc_state;
		if (ret != 0)
			drv_i915_lcd_error("WARN_ON(IS_ERR(temp_crtc_state) || ret)\n");
	}

	/* Disables the crtc through the display hooks. */
	i915->display.funcs.display->crtc_disable(state, crtc);

	/* Notes the adjusted state. */
	I915_LCD_DRM_DBG_KMS(&i915->drm,
			     "[CRTC:%d:%s] hw state adjusted, was enabled, now disabled\n",
			     crtc->base.base.id,
			     crtc->base.name);

	/* The crtc no longer runs. */
	crtc->active = false;
	crtc->base.enabled = false;

	/* Drops the crtc's reference on its shared DPLL. */
	if (crtc_state->shared_dpll != NULL) {
		drv_i915_unreference_shared_dpll_crtc(crtc,
						      crtc_state->shared_dpll,
						      &crtc_state->shared_dpll->state);
	}
}

/*
 * Finishes the disable of a crtc (intel_crtc_disable_noatomic_complete()).
 *
 * The crtc's state is reset, its encoders and connectors are unlinked, its
 * power references are given back, and the device-wide states forget the
 * pipe.
 */
static void
i915_crtc_disable_noatomic_complete(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct intel_bw_state *bw_state;
	struct intel_cdclk_state *cdclk_state;
	struct intel_dbuf_state *dbuf_state;
	struct intel_pmdemand_state *pmdemand_state;
	struct intel_crtc_state *crtc_state;
	enum pipe pipe;

	/* Resolves the device, its global states, the crtc's state and pipe. */
	i915 = i915_lcd_to_i915(crtc->base.dev);
	cur_i915 = i915_takeover_cur_i915(takeover);
	bw_state = to_intel_bw_state(i915->display.bw.obj.state);
	cdclk_state = to_intel_cdclk_state(i915->display.cdclk.obj.state);
	dbuf_state = i915_takeover_intel_atomic_get_dbuf_state(i915_takeover_wm(takeover));
	pmdemand_state = to_intel_pmdemand_state(i915->display.pmdemand.obj.state);
	crtc_state = to_intel_crtc_state(crtc->base.state);
	pipe = crtc->pipe;

	/* Only the unported pm demand steps name the pm demand state; they do not read it. */
	(void)pmdemand_state;

	/* Frees and resets the crtc's state (the frees are steps: the state is the registry's storage). */
	I915_TAKEOVER___DRM_ATOMIC_HELPER_CRTC_DESTROY_STATE(cur_i915, &crtc_state->uapi);
	I915_TAKEOVER_INTEL_CRTC_FREE_HW_STATE(cur_i915, crtc_state);
	drv_i915_crtc_state_reset(crtc_state, crtc);

	/* Unlinks the crtc's encoders and their connectors. */
	i915_reset_crtc_encoder_state(takeover, crtc);

	/* FBC and the pre-Gen9 watermark update are not part of this path. */
	I915_TAKEOVER_INTEL_FBC_DISABLE(cur_i915, crtc);
	I915_TAKEOVER_INTEL_UPDATE_WATERMARKS(cur_i915, i915);

	/* Gives back the power references the crtc holds. */
	drv_i915_n1_power_put_all_in_set(cur_i915, &crtc->enabled_power_domains);

	/* The CDCLK state forgets the pipe. */
	cdclk_state->min_cdclk[pipe] = 0;
	cdclk_state->min_voltage_level[pipe] = 0;
	cdclk_state->active_pipes &= ~BIT(pipe);

	/* The DBUF state forgets the pipe. */
	dbuf_state->active_pipes &= ~BIT(pipe);

	/* The bandwidth state forgets the pipe. */
	bw_state->data_rate[pipe] = 0;
	bw_state->num_active_planes[pipe] = 0;

	/* The pm demand unit is not part of this path. */
	I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PORT_CLOCK(cur_i915, i915, pmdemand_state, pipe, 0);
}

/*
 * Disables a crtc and every crtc linked to it (intel_crtc_disable_noatomic()).
 *
 * The big-joiner slaves go first, then the port sync slaves, then the
 * masters; the states of all of them are completed afterwards.  MST is not
 * supported (as in Linux).
 */
static void
i915_crtc_disable_noatomic(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *i915;
	struct intel_crtc *temp_crtc;
	u8 portsync_master_mask;
	u8 portsync_slaves_mask;
	u8 bigjoiner_slaves_mask;
	unsigned index;
	bool overlap;

	/* Resolves the crtc's device. */
	i915 = i915_lcd_to_i915(crtc->base.dev);

	/* Collects the port sync masters and slaves, and the big-joiner slaves of all of them. */
	i915_get_portsync_pipes(takeover, crtc, &portsync_master_mask, &portsync_slaves_mask);
	bigjoiner_slaves_mask = i915_get_bigjoiner_slave_pipes(takeover,
							       i915,
							       portsync_master_mask | portsync_slaves_mask);

	/* Warns when the three sets overlap. */
	overlap = false;
	if ((portsync_master_mask & portsync_slaves_mask) != 0) {
		/* A master is also a slave. */
		overlap = true;
	} else if ((portsync_master_mask & bigjoiner_slaves_mask) != 0) {
		/* A master is also a big-joiner slave. */
		overlap = true;
	} else if ((portsync_slaves_mask & bigjoiner_slaves_mask) != 0) {
		/* A port sync slave is also a big-joiner slave. */
		overlap = true;
	}
	if (overlap) {
		drv_i915_lcd_error("WARN_ON(portsync_master_mask & portsync_slaves_mask || "
				   "portsync_master_mask & bigjoiner_slaves_mask || "
				   "portsync_slaves_mask & bigjoiner_slaves_mask)\n");
	}

	/* Disables the big-joiner slaves. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, temp_crtc, index, bigjoiner_slaves_mask) {
		i915_crtc_disable_noatomic_begin(takeover, temp_crtc, ctx);
	}

	/* Disables the port sync slaves. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, temp_crtc, index, portsync_slaves_mask) {
		i915_crtc_disable_noatomic_begin(takeover, temp_crtc, ctx);
	}

	/* Disables the port sync masters. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, temp_crtc, index, portsync_master_mask) {
		i915_crtc_disable_noatomic_begin(takeover, temp_crtc, ctx);
	}

	/* Completes the disable of every one of them. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, temp_crtc, index, bigjoiner_slaves_mask | portsync_slaves_mask | portsync_master_mask) {
		i915_crtc_disable_noatomic_complete(takeover, temp_crtc);
	}
}

/* Links a connector's state to an encoder and its crtc, or unlinks it (set_encoder_for_connector()). */
static void
i915_set_encoder_for_connector(
	struct intel_connector *connector,
	struct intel_encoder *encoder)
{
	struct drm_connector_state *conn_state;

	/* The state that is relinked. */
	conn_state = connector->base.state;

	/* A linked state held a connector reference (nothing to count in this path). */
	if (conn_state->crtc != NULL)
		I915_TAKEOVER_DRM_CONNECTOR_PUT(&connector->base);

	/* Links the encoder and its crtc, or clears both. */
	if (encoder != NULL) {
		conn_state->best_encoder = &encoder->base;
		conn_state->crtc = encoder->base.crtc;
		I915_TAKEOVER_DRM_CONNECTOR_GET(&connector->base);
	} else {
		conn_state->best_encoder = NULL;
		conn_state->crtc = NULL;
	}
}

/* Unlinks every connector attached to an encoder and turns it off (reset_encoder_connector_state()). */
static void
i915_reset_encoder_connector_state(
	struct i915_takeover_world *takeover,
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct intel_pmdemand_state *pmdemand_state;
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;

	/* Resolves the encoder's device and its pm demand state. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_takeover_cur_i915(takeover);
	pmdemand_state = to_intel_pmdemand_state(i915->display.pmdemand.obj.state);

	/* Only the unported pm demand steps name the pm demand state; they do not read it. */
	(void)pmdemand_state;

	/* Walks the connectors attached to the encoder. */
	i915_takeover_drm_connector_list_iter_begin(&conn_iter);
	I915_TAKEOVER_FOR_EACH_INTEL_CONNECTOR_ITER(takeover, connector, &conn_iter) {
		if (connector->base.encoder != &encoder->base)
			continue;

		/* Clears the encoder's bit in the pm demand active phys mask (not part of this path). */
		I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PHYS_MASK(cur_i915, i915, encoder, pmdemand_state, false);

		/* Unlinks the connector's state. */
		i915_set_encoder_for_connector(connector, NULL);

		/* The connector is off and attached to nothing. */
		connector->base.dpms = DRM_MODE_DPMS_OFF;
		connector->base.encoder = NULL;
	}
	i915_takeover_drm_connector_list_iter_end(&conn_iter);
}

/* Unlinks every encoder on a crtc and its connectors (reset_crtc_encoder_state()). */
static void
i915_reset_crtc_encoder_state(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc)
{
	struct intel_encoder *encoder;
	unsigned index;

	/* Resets each encoder on the crtc, then unlinks it from the crtc. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		if (encoder->base.crtc != &crtc->base)
			continue;

		/* Unlinks the encoder's connectors, then the encoder. */
		i915_reset_encoder_connector_state(takeover, encoder);
		encoder->base.crtc = NULL;
	}
}

/*
 * Brings every connector's atomic state in line with the readout
 * (intel_modeset_update_connector_atomic_state()).
 */
static void
i915_modeset_update_connector_atomic_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector_state *conn_state;
	struct intel_encoder *encoder;
	struct intel_crtc *crtc;
	const struct intel_crtc_state *crtc_state;
	int pipe_bpp;

	UNUSED_PARAMETER(i915);

	/* Walks every connector of the registry. */
	i915_takeover_drm_connector_list_iter_begin(&conn_iter);
	I915_TAKEOVER_FOR_EACH_INTEL_CONNECTOR_ITER(takeover, connector, &conn_iter) {
		conn_state = connector->base.state;
		encoder = to_intel_encoder(connector->base.encoder);

		/* Links the state to the encoder the readout found (or unlinks it). */
		i915_set_encoder_for_connector(connector, encoder);

		/* A linked connector takes its maximum bpc from the crtc's pipe bpp (24 when unknown). */
		if (encoder != NULL) {
			crtc = to_intel_crtc(encoder->base.crtc);
			crtc_state = to_intel_crtc_state(crtc->base.state);
			pipe_bpp = crtc_state->pipe_bpp;
			if (pipe_bpp == 0)
				pipe_bpp = 24;
			conn_state->max_bpc = pipe_bpp / 3;
		}
	}
	i915_takeover_drm_connector_list_iter_end(&conn_iter);
}

/* Copies the read-out hardware state into the uapi state (intel_crtc_copy_hw_to_uapi_state()). */
static void
i915_crtc_copy_hw_to_uapi_state(
	struct intel_crtc_state *crtc_state)
{
	int mode_error;

	/* A big-joiner slave has no uapi state of its own. */
	if (intel_crtc_is_bigjoiner_slave(crtc_state))
		return;

	/* The enable and active flags. */
	crtc_state->uapi.enable = crtc_state->hw.enable;
	crtc_state->uapi.active = crtc_state->hw.active;

	/* The mode (which cannot fail to copy in this path). */
	mode_error = i915_drm_atomic_set_mode_for_crtc(&crtc_state->uapi, &crtc_state->hw.mode);
	if (mode_error < 0)
		drv_i915_lcd_error("WARN_ON(drm_atomic_set_mode_for_crtc(&crtc_state->uapi, &crtc_state->hw.mode) < 0)\n");

	/* The adjusted mode and the scaling filter. */
	crtc_state->uapi.adjusted_mode = crtc_state->hw.adjusted_mode;
	crtc_state->uapi.scaling_filter = crtc_state->hw.scaling_filter;

	/* The LUTs, assuming a 1:1 mapping of the pre- and post-CSC LUTs. */
	crtc_state->hw.degamma_lut = crtc_state->pre_csc_lut;
	crtc_state->hw.gamma_lut = crtc_state->post_csc_lut;

	/* The uapi copies of the LUTs and the CTM. */
	crtc_state->uapi.degamma_lut = crtc_state->hw.degamma_lut;
	crtc_state->uapi.gamma_lut = crtc_state->hw.gamma_lut;
	crtc_state->uapi.ctm = crtc_state->hw.ctm;
}

/*
 * Disables a primary plane attached to the wrong pipe (intel_sanitize_plane_mapping()).
 *
 * Only display versions before 4 can attach a primary plane to another
 * pipe; this display returns at once.
 */
static void
i915_sanitize_plane_mapping(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	struct intel_crtc *crtc;
	struct intel_plane *plane;
	struct intel_crtc *plane_crtc;
	enum pipe pipe;
	unsigned index;
	int display_ver;
	bool visible;

	UNUSED_PARAMETER(i915);

	/* Display version 4 and later fix each primary plane to its pipe. */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (display_ver >= 4)
		return;

	/* Checks the primary plane of every crtc. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		plane = to_intel_plane(crtc->base.primary);

		/* A plane that is off is attached to nothing. */
		visible = plane->get_hw_state(plane, &pipe);
		if (!visible)
			continue;

		/* A plane on its own pipe is fine. */
		if (pipe == crtc->pipe)
			continue;

		/* Disables the plane on the pipe it is attached to. */
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[PLANE:%d:%s] attached to the wrong pipe, disabling plane\n",
				     plane->base.base.id,
				     plane->base.name);
		plane_crtc = I915_TAKEOVER_INTEL_CRTC_FOR_PIPE(takeover, i915, pipe);
		drv_i915_plane_disable_noatomic(takeover, plane_crtc, plane);
	}
}

/* Tells whether any encoder is linked to a crtc (intel_crtc_has_encoders()). */
static bool
i915_crtc_has_encoders(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc)
{
	struct intel_encoder *encoder;
	unsigned index;

	/* The first encoder on the crtc answers the question. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		if (encoder->base.crtc == &crtc->base)
			return true;
	}

	/* No encoder is linked to the crtc. */
	return false;
}

/* Finds the connector attached to an encoder (intel_encoder_find_connector()). */
static struct intel_connector *
i915_encoder_find_connector(
	struct i915_takeover_world *takeover,
	struct intel_encoder *encoder)
{
	struct drm_connector_list_iter conn_iter;
	struct intel_connector *connector;
	struct intel_connector *found_connector;

	/* Walks the connectors until one is attached to the encoder. */
	found_connector = NULL;
	i915_takeover_drm_connector_list_iter_begin(&conn_iter);
	I915_TAKEOVER_FOR_EACH_INTEL_CONNECTOR_ITER(takeover, connector, &conn_iter) {
		if (&encoder->base == connector->base.encoder) {
			found_connector = connector;
			break;
		}
	}
	i915_takeover_drm_connector_list_iter_end(&conn_iter);

	/* Reports the connector, or NULL when none is attached. */
	return found_connector;
}

/*
 * Starts the crtc's underrun reporting as Linux does (intel_sanitize_fifo_underrun_reporting()).
 *
 * Reporting starts disabled on active pipes to avoid races; platforms
 * without underrun control bits (GMCH) start disabled everywhere.  No
 * protection against concurrent access is required: at worst an underrun
 * happens, which also clears the flag.
 */
static void
i915_sanitize_fifo_underrun_reporting(
	struct i915_takeover_world *takeover,
	const struct intel_crtc_state *crtc_state)
{
	struct intel_crtc *crtc;
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;

	/* Resolves the crtc and its device. */
	crtc = to_intel_crtc(crtc_state->uapi.crtc);
	i915 = i915_lcd_to_i915(crtc->base.dev);
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* Only the unported step names the device; it does not read it. */
	(void)i915;

	/* The underrun bookkeeping is not part of this path (its interrupts are not enabled). */
	I915_TAKEOVER_INTEL_INIT_FIFO_UNDERRUN_REPORTING(cur_i915, i915, crtc, !crtc_state->hw.active && !HAS_GMCH(i915));
}

/*
 * Tells whether the BIOS left a bogus DPLL configuration (has_bogus_dpll_config()).
 *
 * Some Sandy Bridge BIOSes misprogram the hardware when a high-resolution
 * display is plugged in (P divider zero, bonkers timings); only that
 * platform is asked.
 */
static bool
i915_has_bogus_dpll_config(
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;

	/* Resolves the crtc's device. */
	i915 = i915_lcd_to_i915(crtc_state->uapi.crtc->dev);
	(void)i915;

	/* Only Sandy Bridge has this BIOS. */
	if (!IS_SANDYBRIDGE(i915))
		return false;

	/* Only an active pipe can be misprogrammed. */
	if (!crtc_state->hw.active)
		return false;

	/* Only a pipe on a shared DPLL can be misprogrammed. */
	if (crtc_state->shared_dpll == NULL)
		return false;

	/* The misprogrammed pipe has no port clock. */
	if (crtc_state->port_clock != 0)
		return false;

	/* Succeeded: the configuration is the bogus one. */
	return true;
}

/*
 * Sanitizes one crtc (intel_sanitize_crtc()).
 *
 * An active crtc keeps only its primary plane and loses any background
 * colour the BIOS set; an active crtc without encoders (or whose Type-C
 * link must be reset) is disabled.  Reports whether the crtc was forced off.
 */
static bool
i915_sanitize_crtc(
	struct i915_takeover_world *takeover,
	struct intel_crtc *crtc,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *cur_i915;
	struct intel_crtc_state *crtc_state;
	const struct intel_plane_state *plane_state;
	struct intel_plane *plane;
	bool needs_link_reset;
	bool has_encoders;

	/* Resolves the crtc's state and the device of the sleeps. */
	crtc_state = to_intel_crtc_state(crtc->base.state);
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* An active crtc keeps only its primary plane and loses the BIOS's background colour. */
	if (crtc_state->hw.active) {
		/* Disables everything but the primary plane. */
		I915_TAKEOVER_FOR_EACH_INTEL_PLANE_ON_CRTC(takeover, crtc, plane) {
			plane_state = to_intel_plane_state(plane->base.state);
			if (plane_state->uapi.visible && plane->base.type != DRM_PLANE_TYPE_PRIMARY)
				drv_i915_plane_disable_noatomic(takeover, crtc, plane);
		}

		/* Disables any background colour set by the BIOS. */
		drv_i915_color_commit_noarm(crtc_state);
		drv_i915_color_commit_arm(crtc_state);
	}

	/* An inactive crtc and a big-joiner slave are not forced off. */
	if (!crtc_state->hw.active)
		return false;
	if (intel_crtc_is_bigjoiner_slave(crtc_state))
		return false;

	/* Asks whether a Type-C link of the crtc must be reset. */
	needs_link_reset = i915_crtc_needs_link_reset(takeover, crtc);

	/*
	 * Keeps the output pipe as it is when it has active encoders and no
	 * link to reset.
	 */
	if (!needs_link_reset) {
		has_encoders = i915_crtc_has_encoders(takeover, crtc);
		if (has_encoders)
			return false;
	}

	/* Forces the crtc off. */
	i915_crtc_disable_noatomic(takeover, crtc, ctx);

	/*
	 * The HPD state of other active and disconnected Type-C ports may stay
	 * connected until this port is disabled and about 10 ms have passed;
	 * waits for that, so sanitizing the other crtcs sees the new HPD state.
	 */
	if (needs_link_reset)
		i915_lcd_msleep(cur_i915, 20);

	/* Succeeded: the crtc was forced off. */
	return true;
}

/*
 * Sanitizes every crtc until none is forced off any more (intel_sanitize_all_crtcs()).
 *
 * An active and disconnected Type-C port keeps the HPD live state of the
 * other Type-C ports from updating, so after a port is disabled the crtcs
 * on other Type-C ports are checked again.
 */
static void
i915_sanitize_all_crtcs(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *cur_i915;
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	u32 crtcs_forced_off;
	u32 old_mask;
	u32 crtc_mask;
	unsigned index;
	bool forced_off;

	UNUSED_PARAMETER(i915);

	/* The dump step is recorded on the current device. */
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* Repeats the pass until it forces no further crtc off. */
	crtcs_forced_off = 0;
	for (;;) {
		old_mask = crtcs_forced_off;

		/* Sanitizes every crtc not forced off yet. */
		I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
			crtc_mask = drm_crtc_mask(&crtc->base);
			if (crtcs_forced_off & crtc_mask)
				continue;

			/* Remembers a crtc the sanitize forced off. */
			forced_off = i915_sanitize_crtc(takeover, crtc, ctx);
			if (forced_off)
				crtcs_forced_off |= crtc_mask;
		}

		/* A pass that forced nothing off ends the repetition. */
		if (crtcs_forced_off == old_mask)
			break;
	}

	/* Dumps every crtc's state (a debugging step, not ported). */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		crtc_state = to_intel_crtc_state(crtc->base.state);
		(void)crtc_state;
		I915_TAKEOVER_INTEL_CRTC_STATE_DUMP(cur_i915, crtc_state, NULL, "setup_hw_state");
	}
}

/*
 * Sanitizes one encoder (intel_sanitize_encoder()).
 *
 * An encoder with active connectors but no active pipe (fallout of a
 * resume register restore, or of a readout bug) is disabled by hand and
 * its connector clamped to off; the firmware is told the encoder's final
 * state, and the DDI clock mapping is sanitized.
 */
static void
i915_sanitize_encoder(
	struct i915_takeover_world *takeover,
	struct intel_encoder *encoder)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	struct intel_connector *connector;
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	struct intel_pmdemand_state *pmdemand_state;
	void *best_encoder;
	bool has_active_crtc;
	bool bogus;
	int notify_enable;

	/* Resolves the encoder's device, its pm demand state and the crtc it reads from. */
	i915 = i915_lcd_to_i915(encoder->base.dev);
	cur_i915 = i915_takeover_cur_i915(takeover);
	crtc = to_intel_crtc(encoder->base.crtc);
	pmdemand_state = to_intel_pmdemand_state(i915->display.pmdemand.obj.state);

	/* The crtc's state, when the encoder is linked to a crtc. */
	crtc_state = NULL;
	if (crtc != NULL)
		crtc_state = to_intel_crtc_state(crtc->base.state);

	/* Only the unported pm demand steps name the pm demand state; they do not read it. */
	(void)pmdemand_state;

	/*
	 * Both the crtc link (the encoder is active and reads from a pipe) and
	 * the pipe itself being active count.
	 */
	has_active_crtc = false;
	if (crtc_state != NULL) {
		if (crtc_state->hw.active)
			has_active_crtc = true;
	}

	/* A pipe the BIOS misprogrammed does not count as active. */
	if (crtc_state != NULL) {
		bogus = i915_has_bogus_dpll_config(crtc_state);
		if (bogus) {
			I915_LCD_DRM_DBG_KMS(&i915->drm,
					     "BIOS has misprogrammed the hardware. Disabling pipe %c\n",
					     pipe_name(crtc->pipe));
			has_active_crtc = false;
		}
	}

	/* An encoder with a connector but no active pipe is disabled by hand. */
	connector = i915_encoder_find_connector(takeover, encoder);
	if (connector != NULL && !has_active_crtc) {
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[ENCODER:%d:%s] has active connectors but no active pipe!\n",
				     encoder->base.base.id,
				     encoder->base.name);

		/* Clears the encoder's bit in the pm demand active phys mask (not part of this path). */
		I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PHYS_MASK(cur_i915, i915, encoder, pmdemand_state, false);

		/*
		 * The connector is active but has no active pipe (fallout of the
		 * resume register restore): disables the encoder again.
		 */
		if (crtc_state != NULL) {
			I915_LCD_DRM_DBG_KMS(&i915->drm,
					     "[ENCODER:%d:%s] manually disabled\n",
					     encoder->base.base.id,
					     encoder->base.name);

			/* Points best_encoder at the encoder, in case the hooks consult it. */
			best_encoder = connector->base.state->best_encoder;
			connector->base.state->best_encoder = &encoder->base;

			/* Disables the encoder (with no atomic state, as in Linux). */
			if (encoder->disable != NULL)
				encoder->disable(NULL, encoder, crtc_state, connector->base.state);
			if (encoder->post_disable != NULL)
				encoder->post_disable(NULL, encoder, crtc_state, connector->base.state);

			/* Restores best_encoder. */
			connector->base.state->best_encoder = best_encoder;
		}
		encoder->base.crtc = NULL;

		/*
		 * The output, port and pipe states disagree, presumably through a
		 * readout bug: the connector is clamped to off as the safer
		 * default.
		 */
		connector->base.dpms = DRM_MODE_DPMS_OFF;
		connector->base.encoder = NULL;
	}

	/* Tells the firmware the encoder's sanitized state. */
	notify_enable = 0;
	if (connector != NULL) {
		if (has_active_crtc)
			notify_enable = 1;
	}
	intel_opregion_notify_encoder(encoder, notify_enable);

	/* Sanitizes the DDI clock mapping of the encoder. */
	if (HAS_DDI(i915))
		drv_i915_ddi_sanitize_encoder_pll_mapping(takeover, encoder);
}

/*
 * Reads out which plane is visible on which pipe (readout_plane_state()).
 *
 * Only the visibility is read (Linux: "FIXME read out full plane state for
 * all planes"); each crtc's plane bitmasks follow from it.
 */
static void
i915_readout_plane_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	struct intel_plane *plane;
	struct intel_plane_state *plane_state;
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	enum pipe pipe;
	unsigned index;
	bool visible;

	UNUSED_PARAMETER(i915);

	/* Reads every plane and records it visible or not on the crtc of its pipe. */
	I915_TAKEOVER_FOR_EACH_INTEL_PLANE(takeover, plane, index) {
		plane_state = to_intel_plane_state(plane->base.state);
		pipe = PIPE_A;

		/* Asks the plane whether it scans out, and from which pipe. */
		visible = plane->get_hw_state(plane, &pipe);

		/* Records the answer on the crtc of that pipe. */
		crtc = I915_TAKEOVER_INTEL_CRTC_FOR_PIPE(takeover, i915, pipe);
		crtc_state = to_intel_crtc_state(crtc->base.state);
		drv_i915_set_plane_visible(crtc_state, plane_state, visible);

		/* Notes the plane's readout. */
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[PLANE:%d:%s] hw state readout: %s, pipe %c\n",
				     plane->base.base.id,
				     plane->base.name,
				     str_enabled_disabled(visible),
				     pipe_name(pipe));
	}

	/* Derives each crtc's plane bitmasks from the visibility. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		crtc_state = to_intel_crtc_state(crtc->base.state);
		drv_i915_plane_fixup_bitmasks(takeover, crtc_state);
	}
}

/*
 * Reads out the hardware state of the whole display (intel_modeset_readout_hw_state()).
 *
 * The crtcs, the planes, the encoders, the DPLLs and the connectors are
 * read in that order; then each active crtc's derived state (the uapi
 * copy, the plane data rates and CDCLK needs, the bandwidth) is filled.
 */
static void
i915_modeset_readout_hw_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	struct drm_i915_private *cur_i915;
	struct intel_cdclk_state *cdclk_state;
	struct intel_dbuf_state *dbuf_state;
	struct intel_pmdemand_state *pmdemand_state;
	struct intel_bw_state *bw_state;
	struct intel_crtc_state *crtc_state;
	struct intel_crtc_state *slave_crtc_state;
	const struct intel_plane_state *plane_state;
	struct intel_crtc *crtc;
	struct intel_crtc *slave_crtc;
	struct intel_encoder *encoder;
	struct intel_connector *connector;
	struct intel_plane *plane;
	struct drm_connector_list_iter conn_iter;
	enum pipe pipe;
	unsigned index;
	unsigned slave_index;
	u8 active_pipes;
	int min_cdclk;
	int display_ver;
	bool enabled;
	bool half_rate;

	/* Resolves the device's global states and the device of the steps. */
	cur_i915 = i915_takeover_cur_i915(takeover);
	cdclk_state = to_intel_cdclk_state(i915->display.cdclk.obj.state);
	dbuf_state = i915_takeover_intel_atomic_get_dbuf_state(i915_takeover_wm(takeover));
	pmdemand_state = to_intel_pmdemand_state(i915->display.pmdemand.obj.state);
	active_pipes = 0;

	/* Only the unported pm demand steps name the pm demand state; they do not read it. */
	(void)pmdemand_state;

	/* Reads out every crtc from a freshly reset state. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		crtc_state = to_intel_crtc_state(crtc->base.state);

		/* Frees and resets the state (the frees are steps: the state is the registry's storage). */
		I915_TAKEOVER___DRM_ATOMIC_HELPER_CRTC_DESTROY_STATE(cur_i915, &crtc_state->uapi);
		I915_TAKEOVER_INTEL_CRTC_FREE_HW_STATE(cur_i915, crtc_state);
		drv_i915_crtc_state_reset(crtc_state, crtc);

		/*
		 * Reads the pipe's configuration from the hardware.  Whether the
		 * pipe is on is read from the state it fills (hw.active), not from
		 * the answer, as in Linux.
		 */
		(void)drv_i915_crtc_get_pipe_config(crtc_state);

		/* The crtc is enabled exactly when it is active. */
		crtc_state->hw.enable = crtc_state->hw.active;
		crtc->base.enabled = crtc_state->hw.enable;
		crtc->active = crtc_state->hw.active;

		/* Collects the active pipes. */
		if (crtc_state->hw.active)
			active_pipes |= BIT(crtc->pipe);

		/* Notes the crtc's readout. */
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[CRTC:%d:%s] hw state readout: %s\n",
				     crtc->base.base.id,
				     crtc->base.name,
				     str_enabled_disabled(crtc_state->hw.active));
	}

	/* The CDCLK and DBUF states learn the active pipes. */
	cdclk_state->active_pipes = active_pipes;
	dbuf_state->active_pipes = active_pipes;

	/* Reads out the planes. */
	i915_readout_plane_state(takeover, i915);

	/* Reads out every encoder and links it to the crtc of the pipe it reads from. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		crtc_state = NULL;
		pipe = 0;

		/* An enabled encoder is linked to its crtc and its configuration is read. */
		enabled = encoder->get_hw_state(encoder, &pipe);
		if (enabled) {
			crtc = I915_TAKEOVER_INTEL_CRTC_FOR_PIPE(takeover, i915, pipe);
			crtc_state = to_intel_crtc_state(crtc->base.state);

			/* Links the encoder and reads its part of the crtc's configuration. */
			encoder->base.crtc = &crtc->base;
			drv_i915_encoder_get_config(encoder, crtc_state);

			/* Reads the configuration into the big-joiner slave crtcs as well. */
			if (crtc_state->bigjoiner_pipes) {
				/* The encoder should be linked to the big-joiner master. */
				if (intel_crtc_is_bigjoiner_slave(crtc_state))
					drv_i915_lcd_error("WARN_ON(intel_crtc_is_bigjoiner_slave(crtc_state))\n");

				/* Reads the encoder's configuration into each slave. */
				I915_TAKEOVER_FOR_EACH_INTEL_CRTC_IN_PIPE_MASK(takeover, slave_crtc, slave_index, intel_crtc_bigjoiner_slave_pipes(crtc_state)) {
					slave_crtc_state = to_intel_crtc_state(slave_crtc->base.state);
					drv_i915_encoder_get_config(encoder, slave_crtc_state);
				}
			}

			/* Sets the encoder's bit in the pm demand active phys mask (not part of this path). */
			I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PHYS_MASK(cur_i915, i915, encoder, pmdemand_state, true);
		} else {
			/* Clears the encoder's bit in the pm demand active phys mask (not part of this path). */
			I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PHYS_MASK(cur_i915, i915, encoder, pmdemand_state, false);

			/* A disabled encoder is linked to no crtc. */
			encoder->base.crtc = NULL;
		}

		/* Lets the encoder align its own state with the readout. */
		if (encoder->sync_state != NULL)
			encoder->sync_state(encoder, crtc_state);

		/* Notes the encoder's readout. */
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[ENCODER:%d:%s] hw state readout: %s, pipe %c\n",
				     encoder->base.base.id,
				     encoder->base.name,
				     str_enabled_disabled(encoder->base.crtc),
				     pipe_name(pipe));
	}

	/* Reads out the shared DPLLs. */
	drv_i915_n1_dpll_readout_hw_state(takeover, i915);

	/* Reads out every connector and links it to its encoder. */
	i915_takeover_drm_connector_list_iter_begin(&conn_iter);
	I915_TAKEOVER_FOR_EACH_INTEL_CONNECTOR_ITER(takeover, connector, &conn_iter) {
		crtc_state = NULL;

		/* An enabled connector is on and attached to its encoder. */
		enabled = connector->get_hw_state(connector);
		if (enabled) {
			connector->base.dpms = DRM_MODE_DPMS_ON;

			/* Attaches the connector to its encoder. */
			encoder = i915_takeover_intel_attached_encoder(connector);
			connector->base.encoder = &encoder->base;

			/* Finds the crtc the encoder reads from, if any. */
			crtc = to_intel_crtc(encoder->base.crtc);
			if (crtc != NULL) {
				crtc_state = to_intel_crtc_state(crtc->base.state);
			} else {
				crtc_state = NULL;
			}

			/*
			 * The connector and encoder masks are filled during the
			 * readout, because anything calling .crtc_disable may rely
			 * on the connector mask being accurate.
			 */
			if (crtc_state != NULL && crtc_state->hw.active) {
				crtc_state->uapi.connector_mask |= drm_connector_mask(&connector->base);
				crtc_state->uapi.encoder_mask |= drm_encoder_mask(&encoder->base);
			}
		} else {
			/* A disabled connector is off and attached to nothing. */
			connector->base.dpms = DRM_MODE_DPMS_OFF;
			connector->base.encoder = NULL;
		}

		/* Lets the connector align its own state with the readout. */
		if (connector->sync_state != NULL)
			connector->sync_state(connector, crtc_state);

		/* Notes the connector's readout. */
		I915_LCD_DRM_DBG_KMS(&i915->drm,
				     "[CONNECTOR:%d:%s] hw state readout: %s\n",
				     connector->base.base.id,
				     connector->base.name,
				     str_enabled_disabled(connector->base.encoder));
	}
	i915_takeover_drm_connector_list_iter_end(&conn_iter);

	/* Fills the derived state of every crtc. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		bw_state = to_intel_bw_state(i915->display.bw.obj.state);
		crtc_state = to_intel_crtc_state(crtc->base.state);
		min_cdclk = 0;

		/*
		 * An active crtc needs a valid mode to keep the atomic core happy;
		 * the derived state is not complete, so the next commit is told
		 * to recalculate everything.
		 */
		if (crtc_state->hw.active) {
			crtc_state->inherited = true;
			drv_i915_crtc_update_active_timings(crtc_state, crtc_state->vrr.enable);
			i915_crtc_copy_hw_to_uapi_state(crtc_state);
		}

		/*
		 * Estimates the data rate and the CDCLK need of every visible
		 * plane: without the framebuffer yet, intel_plane_data_rate() and
		 * plane->min_cdclk() cannot be used.
		 */
		I915_TAKEOVER_FOR_EACH_INTEL_PLANE_ON_CRTC(takeover, crtc, plane) {
			plane_state = to_intel_plane_state(plane->base.state);

			/* A visible plane fetches four bytes per pixel. */
			if (plane_state->uapi.visible)
				crtc_state->data_rate[plane->id] = 4 * crtc_state->pixel_rate;

			/* A visible plane with a CDCLK hook needs the pixel rate, or half of it on a two-pixel pipe. */
			if (plane_state->uapi.visible && plane->min_cdclk != NULL) {
				half_rate = false;
				if (crtc_state->double_wide) {
					/* A double-wide pipe processes two pixels per clock. */
					half_rate = true;
				} else {
					/* Display version 10 and later process two pixels per clock. */
					display_ver = I915_LCD_DISPLAY_VER(i915);
					if (display_ver >= 10)
						half_rate = true;
				}

				/* Records the plane's CDCLK need. */
				if (half_rate) {
					crtc_state->min_cdclk[plane->id] = DIV_ROUND_UP(crtc_state->pixel_rate, 2);
				} else {
					crtc_state->min_cdclk[plane->id] = crtc_state->pixel_rate;
				}
			}

			/* Notes the plane's CDCLK need. */
			I915_LCD_DRM_DBG_KMS(&i915->drm,
					     "[PLANE:%d:%s] min_cdclk %d kHz\n",
					     plane->base.base.id,
					     plane->base.name,
					     crtc_state->min_cdclk[plane->id]);
		}

		/* An active crtc's minimum CDCLK (a negative answer counts as 0). */
		if (crtc_state->hw.active) {
			min_cdclk = drv_i915_crtc_compute_min_cdclk(takeover, crtc_state);
			if (min_cdclk < 0) {
				drv_i915_lcd_error("WARN_ON(min_cdclk < 0)\n");
				min_cdclk = 0;
			}
		}

		/* The CDCLK state learns the pipe's minimum CDCLK and voltage level. */
		cdclk_state->min_cdclk[crtc->pipe] = min_cdclk;
		cdclk_state->min_voltage_level[crtc->pipe] = crtc_state->min_voltage_level;

		/* The pm demand unit is not part of this path. */
		I915_TAKEOVER_INTEL_PMDEMAND_UPDATE_PORT_CLOCK(cur_i915, i915, pmdemand_state, pipe, crtc_state->port_clock);

		/* The bandwidth state learns the crtc's data rates. */
		drv_i915_bw_crtc_update(bw_state, crtc_state);
	}

	/* The pm demand parameters (decided: display 13 has no pm demand unit). */
	I915_TAKEOVER_INTEL_PMDEMAND_INIT_PMDEMAND_PARAMS(cur_i915, i915, pmdemand_state);
}

/* Lets every linked encoder take the power domains of its crtc (get_encoder_power_domains()). */
static void
i915_get_encoder_power_domains(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915)
{
	struct intel_encoder *encoder;
	struct intel_crtc_state *crtc_state;
	unsigned index;

	UNUSED_PARAMETER(i915);

	/* Asks every encoder with a power-domain hook and a crtc. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		if (encoder->get_power_domains == NULL)
			continue;

		/*
		 * MST-primary and inactive encoders have no crtc state and need
		 * no power domain references.
		 */
		if (encoder->base.crtc == NULL)
			continue;

		/* Takes the domains the encoder needs on its crtc. */
		crtc_state = to_intel_crtc_state(encoder->base.crtc->state);
		encoder->get_power_domains(encoder, crtc_state);
	}
}

/* Applies the display workarounds that precede the readout (intel_early_display_was()). */
static void
i915_early_display_was(
	struct drm_i915_private *i915)
{
	/* Display WA #1185 WaDisableDARBFClkGating:glk,icl,ehl,tgl (also Wa_14010480278). */
	if (I915_LCD_IS_DISPLAY_VER(i915, 10, 12))
		i915_lcd_intel_de_rmw(i915, GEN9_CLKGATE_DIS_0, 0, DARBF_GATING_DIS);

	/* WaRsPkgCStateDisplayPMReq:hsw -- the system hangs if this is not done before all planes are disabled. */
	if (IS_HASWELL(i915))
		i915_lcd_intel_de_rmw(i915, CHICKEN_PAR1_1, 0, FORCE_ARB_IDLE_PLANES);

	/* Display WA #1142:kbl,cfl,cml. */
	if (IS_KABYLAKE(i915) ||
	    IS_COFFEELAKE(i915) ||
	    IS_COMETLAKE(i915)) {
		i915_lcd_intel_de_rmw(i915, CHICKEN_PAR1_1, KBL_ARB_FILL_SPARE_22, KBL_ARB_FILL_SPARE_22);
		i915_lcd_intel_de_rmw(i915, CHICKEN_MISC_2, KBL_ARB_FILL_SPARE_13 | KBL_ARB_FILL_SPARE_14, KBL_ARB_FILL_SPARE_14);
	}
}

/*
 * Reads out the display and sanitizes it (intel_modeset_setup_hw_state()).
 *
 * Holds the INIT power reference for the whole run: the early workarounds,
 * the readout, the encoders' power domains, the per-crtc vblank and DMC
 * setup, the plane mapping, the encoder and crtc sanitize, the DPLL and
 * watermark readout, and the check that every crtc holds exactly the
 * domains it needs.
 */
static void
i915_modeset_setup_hw_state(
	struct i915_takeover_world *takeover,
	struct drm_i915_private *i915,
	struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_i915_private *cur_i915;
	struct intel_encoder *encoder;
	struct intel_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	struct intel_power_domain_mask put_domains;
	intel_wakeref_t wakeref;
	unsigned index;
	bool domains_empty;

	/* The steps are recorded on the current device. */
	cur_i915 = i915_takeover_cur_i915(takeover);

	/* Holds the INIT power reference for the whole readout. */
	wakeref = i915_lcd_intel_display_power_get(i915, POWER_DOMAIN_INIT);

	/* Applies the early workarounds and reads the hardware out. */
	i915_early_display_was(i915);
	i915_modeset_readout_hw_state(takeover, i915);

	/* The hardware state is read out; the sanitize starts with the encoders' power domains. */
	i915_get_encoder_power_domains(takeover, i915);

	/* The PCH sanitize (decided: HAS_PCH_IBX is false on this PCH). */
	I915_TAKEOVER_INTEL_PCH_SANITIZE(cur_i915, i915);

	/*
	 * Restores the vblank state of every crtc before the plane mapping is
	 * sanitized, which may wait for vblanks.
	 */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		crtc_state = to_intel_crtc_state(crtc->base.state);

		/* Starts the crtc's underrun reporting and resets its vblank state. */
		i915_sanitize_fifo_underrun_reporting(takeover, crtc_state);
		I915_TAKEOVER_DRM_CRTC_VBLANK_RESET(cur_i915, &crtc->base);

		/* An active crtc gets its DMC pipe program and its vblank. */
		if (crtc_state->hw.active) {
			drv_i915_dmc_enable_pipe(i915, crtc->pipe);
			intel_crtc_vblank_on(crtc_state);
		}
	}

	/* FBC is not part of this path. */
	I915_TAKEOVER_INTEL_FBC_SANITIZE(cur_i915, i915);

	/* Sanitizes the plane mapping. */
	i915_sanitize_plane_mapping(takeover, i915);

	/* Sanitizes every encoder. */
	I915_TAKEOVER_FOR_EACH_INTEL_ENCODER(takeover, encoder, index) {
		i915_sanitize_encoder(takeover, encoder);
	}

	/* The crtc sanitize needs the connectors' atomic state up to date first. */
	i915_modeset_update_connector_atomic_state(takeover, i915);

	/* Sanitizes every crtc. */
	i915_sanitize_all_crtcs(takeover, i915, ctx);

	/* Sanitizes the shared DPLLs. */
	drv_i915_dpll_sanitize_state(i915);

	/* Reads out the watermarks. */
	intel_wm_get_hw_state(i915);

	/* Checks that every crtc holds exactly the power domains it needs. */
	I915_TAKEOVER_FOR_EACH_INTEL_CRTC(takeover, crtc, index) {
		crtc_state = to_intel_crtc_state(crtc->base.state);

		/* Takes the domains the crtc needs and learns which it holds without needing. */
		drv_i915_modeset_get_crtc_power_domains(takeover->display->lcd_world, crtc_state, &put_domains);

		/* A crtc holding domains it does not need is warned about and gives them back. */
		domains_empty = bitmap_empty(put_domains.bits, POWER_DOMAIN_NUM);
		if (!domains_empty) {
			drv_i915_lcd_error("WARN_ON(!bitmap_empty(put_domains.bits, POWER_DOMAIN_NUM))\n");
			drv_i915_modeset_put_crtc_power_domains(crtc, &put_domains);
		}
	}

	/* Gives the INIT power reference back. */
	i915_lcd_intel_display_power_put(i915, POWER_DOMAIN_INIT, wakeref);

	/* The power-domain sanitize is not part of this path. */
	I915_TAKEOVER_INTEL_POWER_DOMAINS_SANITIZE_STATE(cur_i915, i915);
}

/* Counts the set bits of a pipe mask. */
static unsigned
i915_popcount(
	unsigned value)
{
	unsigned count;

	/* Adds one for every set bit. */
	count = 0U;
	while (value != 0U) {
		count += value & 1U;
		value >>= 1;
	}

	/* Succeeded: reports the number of set bits. */
	return count;
}

/* Writes a display register and counts the write in the records. */
static void
i915_nogem_write(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	uint32_t reg,
	uint32_t value)
{
	/* Writes the register and counts the write. */
	drv_i915_write32(m, reg, value);
	d->mmio_writes++;
}

/* Read-modify-writes a display register, writing only a changed value. */
static uint32_t
i915_nogem_rmw(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old;
	uint32_t value;

	/* Reads the register and computes the new value. */
	old = drv_i915_read32(m, reg);
	value = (old & ~clear) | set;

	/* Writes only a value that changed. */
	if (value != old)
		i915_nogem_write(d, m, reg, value);

	/* Succeeded: reports the value before the change. */
	return old;
}

/* Reads the SAGV block time (intel_sagv_block_time()). */
static uint32_t
i915_sagv_block_time(
	int display_ver,
	struct mutex *sb_lock,
	struct i915_mmio *m)
{
	uint32_t val;
	uint32_t val1;
	int error;

	/* DISPLAY_VER >= 12 reads it from the PCODE. */
	if (display_ver >= 12) {
		val = 0U;
		val1 = 0U;
		error = drv_i915_pcode_read(sb_lock, m, I915_GEN12_PCODE_READ_SAGV_BLOCK_TIME_US, &val, &val1);
		if (error != 0) {
			kern_logf("i915: Couldn't read SAGV block time!\n");
			return 0U;
		}

		/* The PCODE reported the block time. */
		return val;
	}

	/* Display 11 has a fixed block time. */
	if (display_ver == 11)
		return 10U;

	/* Succeeded: HAS_SAGV pre-icl. */
	return 30U;
}

/* Records the SAGV status and block time (intel_sagv_init()); on icl+ the bandwidth init settled the status. */
static void
i915_sagv_init(
	struct i915_display_nogem *d,
	int display_ver,
	struct mutex *sb_lock,
	struct i915_mmio *m,
	struct i915_bw_state *bw)
{
	const char *supported;

	/* HAS_SAGV: ver >= 9 && !IS_LP; a platform without SAGV does not control it. */
	if (display_ver < 9)
		bw->sagv_status = (int)I915_SAGV_NOT_CONTROLLED;

	/* DISPLAY_VER < 11 would probe with skl_sagv_disable() here. */

	/* The bandwidth init should have settled the status. */
	if (bw->sagv_status == (int)I915_SAGV_UNKNOWN)
		kern_logf("i915: WARN SAGV status still unknown at sagv_init\n");

	/* Records the status and the block time. */
	d->sagv_status = bw->sagv_status;
	d->sagv_block_time_us = i915_sagv_block_time(display_ver, sb_lock, m);

	/* Avoid overflow when adding with wm0 latency etc. */
	if (d->sagv_block_time_us > 0xffffU) {
		kern_logf("i915: Excessive SAGV block time %u, ignoring\n", d->sagv_block_time_us);
		d->sagv_block_time_us = 0U;
	}

	/* An SAGV the driver does not control blocks nothing. */
	if (bw->sagv_status == (int)I915_SAGV_NOT_CONTROLLED)
		d->sagv_block_time_us = 0U;

	/* Reports the SAGV. */
	supported = (bw->sagv_status != (int)I915_SAGV_NOT_CONTROLLED) ? "yes" : "no";
	kern_logf("i915: SAGV supported: %s, original SAGV block time: %u us\n", supported, d->sagv_block_time_us);
}

/* Sets the ICP GMBUS pins up and resets the controller (intel_gmbus_setup()). */
static int
i915_gmbus_setup(
	struct i915_display_nogem *d,
	struct i915_mmio *m)
{
	static const struct i915_gmbus_pin_desc icp[] = {
		{ I915_GMBUS_PIN_1_BXT,      "dpa", I915_GPIOB },
		{ I915_GMBUS_PIN_2_BXT,      "dpb", I915_GPIOC },
		{ I915_GMBUS_PIN_3_BXT,      "dpc", I915_GPIOD },
		{ I915_GMBUS_PIN_9_TC1_ICP,  "tc1", I915_GPIOJ },
		{ I915_GMBUS_PIN_10_TC2_ICP, "tc2", I915_GPIOK },
		{ I915_GMBUS_PIN_11_TC3_ICP, "tc3", I915_GPIOL },
		{ I915_GMBUS_PIN_12_TC4_ICP, "tc4", I915_GPIOM },
		{ I915_GMBUS_PIN_13_TC5_TGP, "tc5", I915_GPION },
		{ I915_GMBUS_PIN_14_TC6_TGP, "tc6", I915_GPIOO }
	};
	unsigned count;
	unsigned index;
	unsigned pin;

	/* !HAS_GMCH -> the South Display Engine offsets. */
	d->gmbus_mmio_base = I915_PCH_DISPLAY_BASE;

	/* Initializes the GMBUS lock and wait queue. */
	(void)mutex_init(&d->gmbus_lock, LOCK_RANK_DEVICE, "i915-gmbus");
	waitq_init(&d->gmbus_waitq, "i915-gmbus");

	/* gmbus_pins_icp[]: indices are the pin numbers, gaps stay absent. */
	count = sizeof(icp) / sizeof(icp[0]);
	for (index = 0U; index < count; index++) {
		/* Skips a pin beyond the pin records. */
		pin = icp[index].pin;
		if (pin >= (unsigned)I915_NOGEM_MAX_GMBUS)
			continue;

		/* Fills the pin record. */
		d->gmbus_pins[pin].name = icp[index].name;
		d->gmbus_pins[pin].gpio = icp[index].gpio;
		d->gmbus_pins[pin].reg0 = pin | I915_GMBUS_RATE_100KHZ;
		d->gmbus_pins[pin].present = 1;
		d->gmbus_pins_present++;
	}

	/*
	 * The reference now creates one i2c_adapter per pin.  There is no i2c core
	 * in this port, so the adapters are NOT created and that is recorded rather
	 * than reported as success.  Nothing in the readout/sanitize needs them.
	 */
	d->gmbus_adapters_unimplemented = 1;

	/* intel_gmbus_reset() */
	i915_nogem_write(d, m, d->gmbus_mmio_base + I915_GMBUS0_OFF, 0U);
	i915_nogem_write(d, m, d->gmbus_mmio_base + I915_GMBUS4_OFF, 0U);

	/* Succeeded: the pins are known and the controller is reset. */
	return 0;
}

/* Reports the DDI_CLK_OFF bit of a combo PHY: 1 << _PICK(phy, 10, 11, 24, 4, 5). */
static uint32_t
i915_icl_dpclka_ddi_clk_off(
	int phy)
{
	static const unsigned bits[5] = { 10U, 11U, 24U, 4U, 5U };

	/* A PHY beyond the table has no bit. */
	if (phy < 0 || phy > 4)
		return 0U;

	/* Succeeded: reports the PHY's bit. */
	return 1U << bits[phy];
}

/* Reports the TC_CLK_OFF bit of a Type-C port: TC ports 1..3 are bits 12..14, TC port 4 on restart at bit 21. */
static uint32_t
i915_icl_dpclka_tc_clk_off(
	int tc_port)
{
	/* The first three Type-C ports sit at bits 12..14. */
	if (tc_port < I915_TC_PORT_4)
		return 1U << (unsigned)(tc_port + 12);

	/* Succeeded: the later ports restart at bit 21. */
	return 1U << (unsigned)(tc_port - I915_TC_PORT_4 + 21);
}

/* Reports a port's DDI lanes power domain (intel_display_power_ddi_lanes_domain(), d13_port_domains). */
static int
i915_ddi_lanes_domain(
	int display_ver,
	int port)
{
	/* Display 13 has the combo ports A..C and the Type-C ports TC1..TC4. */
	if (display_ver >= 13) {
		/* The combo ports take DDI_LANES_A..C. */
		if (port >= I915_PORT_A && port <= I915_PORT_C)
			return (int)I915_PW_DOMAIN_PORT_DDI_LANES_A + (port - I915_PORT_A);

		/* The Type-C ports take DDI_LANES_TC1..TC4. */
		if (port >= I915_PORT_TC1 && port <= I915_PORT_TC4)
			return (int)I915_PW_DOMAIN_PORT_DDI_LANES_TC1 + (port - I915_PORT_TC1);
	}

	/* drm_WARN_ON path: fall back to LANES_A rather than an invalid domain. */
	return (int)I915_PW_DOMAIN_PORT_DDI_LANES_A;
}

/* Reports whether an earlier child already claimed a port. */
static int
i915_port_in_use(
	const struct i915_display_nogem *d,
	int port)
{
	unsigned index;

	/* Looks for an encoder on the port. */
	for (index = 0U; index < d->num_encoders; index++) {
		if (d->encoders[index].port == port)
			return 1;
	}

	/* No encoder uses the port. */
	return 0;
}

/* Records why intel_ddi_init() declined a child. */
static void
i915_ddi_skip(
	struct i915_display_nogem *d,
	int port,
	int reason)
{
	/* Keeps the reason while there is room for it. */
	if (d->num_ddi_skips < (unsigned)I915_NOGEM_MAX_ENCODERS) {
		d->ddi_skip_reason[d->num_ddi_skips] = reason;
		d->ddi_skip_port[d->num_ddi_skips] = port;
		d->num_ddi_skips++;
	}

	/* Counts every declined child. */
	d->ddi_skipped++;
}

/* Creates the encoder record of one VBT child (intel_ddi_init(), decision half), recording every early return. */
static void
i915_ddi_init(
	struct i915_display_nogem *d,
	int display_ver,
	unsigned port_mask,
	const struct i915_vbt_child *child)
{
	struct i915_encoder *e;
	int port;
	int phy;
	int in_use;
	int supports_dvi;
	int supports_hdmi;
	int init_hdmi;
	int init_dp;
	int connector_result;

	/* Counts the child. */
	d->ddi_init_calls++;

	/* Maps the child's DVO port to a DDI port. */
	port = drv_i915_dvo_port_to_port(display_ver, child->dvo_port);
	if (port == I915_PORT_NONE) {
		i915_ddi_skip(d, port, I915_DDI_SKIP_PORT_NONE);
		return;
	}

	/* port_strap_detected(): straps are not used on skl+. */
	if (display_ver < 9) {
		i915_ddi_skip(d, port, I915_DDI_SKIP_STRAP);
		return;
	}

	/* assert_port_valid(): the platform must advertise the port. */
	if ((port_mask & (1U << (unsigned)port)) == 0U) {
		kern_logf("i915: WARN Platform does not support port %c\n", (char)('A' + port));
		i915_ddi_skip(d, port, I915_DDI_SKIP_PORT_INVALID);
		return;
	}

	/* A port an earlier child claimed is not claimed again. */
	in_use = i915_port_in_use(d, port);
	if (in_use != 0) {
		kern_logf("i915: Port %c already claimed\n", (char)('A' + port));
		i915_ddi_skip(d, port, I915_DDI_SKIP_PORT_IN_USE);
		return;
	}

	/* intel_bios_encoder_supports_dsi() -> icl_dsi_init(), a separate path. */
	if ((child->device_type & I915_DEVICE_TYPE_MIPI_OUTPUT) != 0U) {
		i915_ddi_skip(d, port, I915_DDI_SKIP_DSI);
		return;
	}

	/* Maps the port to its PHY. */
	phy = drv_i915_port_to_phy(display_ver, port);

	/*
	 * intel_hti_uses_phy(): HTI can reserve PHYs.  xe_lpd has no has_hti, so
	 * hti.state stays 0 and no PHY is ever reserved here.
	 */
	if (d->hti_state != 0U) {
		i915_ddi_skip(d, port, I915_DDI_SKIP_HTI);
		return;
	}

	/* intel_bios_encoder_supports_dvi(): TMDS DVI signalling. */
	supports_dvi = 0;
	if ((child->device_type & I915_DEVICE_TYPE_TMDS_DVI_SIGNALING) != 0U)
		supports_dvi = 1;

	/* intel_bios_encoder_supports_hdmi(): DVI signalling that is not marked not-HDMI. */
	supports_hdmi = 0;
	if (supports_dvi != 0 && (child->device_type & I915_DEVICE_TYPE_NOT_HDMI_OUTPUT) == 0U)
		supports_hdmi = 1;

	/* HDMI is initialized for a DVI or an HDMI child. */
	init_hdmi = 0;
	if (supports_dvi != 0 || supports_hdmi != 0)
		init_hdmi = 1;

	/* intel_bios_encoder_supports_dp() */
	init_dp = 0;
	if ((child->device_type & I915_DEVICE_TYPE_DISPLAYPORT_OUTPUT) != 0U)
		init_dp = 1;

	/* intel_bios_encoder_is_lspcon(): HAS_LSPCON is DISPLAY_VER 9..10 only. */

	/* A child that is neither DVI, HDMI nor DP is respected as such. */
	if (init_dp == 0 && init_hdmi == 0) {
		kern_logf("i915: VBT says port %c is not DVI/HDMI/DP compatible, "
			"respect it\n", (char)('A' + port));
		i915_ddi_skip(d, port, I915_DDI_SKIP_NOT_DVI_HDMI_DP);
		return;
	}

	/* No record left for the encoder. */
	if (d->num_encoders >= (unsigned)I915_NOGEM_MAX_ENCODERS)
		return;

	/* Fills the encoder record. */
	e = &d->encoders[d->num_encoders];
	d->num_encoders++;
	e->port = port;
	e->phy = phy;
	e->is_tc = drv_i915_ddi_is_tc(display_ver, port);
	e->power_domain = i915_ddi_lanes_domain(display_ver, port);
	e->init_hdmi = init_hdmi;
	e->init_dp = init_dp;
	e->dvo_port = child->dvo_port;
	e->device_type = child->device_type;

	/*
	 * DISPLAY_VER >= 11 and not ADL-S / RKL / DG1 / JSL / EHL: the icl combo or
	 * TC clock ops, selected by intel_ddi_is_tc().
	 */
	e->clk_funcs = (e->is_tc != 0) ? I915_DDI_CLK_ICL_TC : I915_DDI_CLK_ICL_COMBO;
	e->in_use = 1;

	/*
	 * if (init_dp) intel_ddi_init_dp_connector(): for the eDP port this is where the
	 * reference powers the panel logic, reads DPCD / EDID and settles the PPS delays.
	 * A failure takes the reference's `goto err`: the encoder does not survive.
	 */
	if (init_dp != 0 && d->dp_connector_init != NULL) {
		connector_result = d->dp_connector_init(d->dp_connector_ctx, port);
		if (connector_result == 0) {
			d->edp_port = port;
		} else if (connector_result < 0) {
			d->edp_init_rc = connector_result;
			e->in_use = 0;
			d->num_encoders--;
			i915_ddi_skip(d, port, I915_DDI_SKIP_EDP_INIT_FAILED);
		}
	}
}

/* Reports a transcoder's copy of a transcoder A register; the DSI transcoders are not at a flat 0x1000 stride. */
static uint32_t
i915_trans_reg(
	unsigned trans,
	uint32_t reg_a)
{
	static const uint32_t offsets[7] = {
		0x00000U, 0x01000U, 0x02000U, 0x03000U,	/* A..D */
		0x00000U,				/* EDP: not on xe_lpd */
		0x0b000U, 0x0b800U			/* DSI_0, DSI_1 */
	};

	/* An unknown transcoder keeps the transcoder A register. */
	if (trans > 6U)
		return reg_a;

	/* Succeeded: reports the transcoder's register. */
	return reg_a + offsets[trans];
}

/* Reports whether a transcoder's power domain is on; the DSI transcoders have no dedicated pipe well here. */
static int
i915_trans_power_on(
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc,
	unsigned trans)
{
	int enabled;

	/* The DSI transcoders count as powered. */
	if (trans > 3U)
		return 1;

	/* Asks for the cached state of the transcoder domain. */
	enabled = drv_i915_display_power_is_enabled(pd, (enum i915_power_domain)(I915_PW_DOMAIN_TRANSCODER_A + trans), pwc);

	/* Succeeded: reports whether the domain is on. */
	return enabled;
}

/* Reads the active and total sizes of a transcoder (intel_get_transcoder_timings(), the parts the sanitize can use). */
static void
i915_get_transcoder_timings(
	struct i915_crtc_state *cs,
	unsigned trans,
	struct i915_mmio *m)
{
	uint32_t reg;
	uint32_t value;

	/* HACTIVE and HTOTAL. */
	reg = i915_trans_reg(trans, I915_TRANS_HTOTAL_A);
	value = drv_i915_read32(m, reg);
	cs->hdisplay = (value & 0xffffU) + 1U;
	cs->htotal = ((value >> 16) & 0xffffU) + 1U;

	/* VACTIVE and VTOTAL. */
	reg = i915_trans_reg(trans, I915_TRANS_VTOTAL_A);
	value = drv_i915_read32(m, reg);
	cs->vdisplay = (value & 0xffffU) + 1U;
	cs->vtotal = ((value >> 16) & 0xffffU) + 1U;
}

/* Reads a pipe's configuration, gated on the pipe power, then the transcoder state (hsw_get_pipe_config()). */
static int
i915_nogem_hsw_get_pipe_config(
	struct i915_display_nogem *d,
	int display_ver,
	struct i915_crtc *crtc,
	struct i915_mmio *m,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	struct i915_crtc_state *cs;
	unsigned enabled;
	unsigned trans;
	uint32_t transconf;
	uint32_t pipesrc;
	uint32_t reg;
	int powered;

	/* A pipe whose power is off is read as disabled. */
	cs = &crtc->state;
	powered = drv_i915_display_power_is_enabled(pd, (enum i915_power_domain)(I915_PW_DOMAIN_PIPE_A + crtc->pipe), pwc);
	if (powered == 0) {
		cs->power_gated = 1;
		return 0;
	}

	/* A pipe no transcoder feeds is disabled. */
	enabled = drv_i915_hsw_enabled_transcoders(d, display_ver, crtc->pipe, m, pd, pwc);
	cs->enabled_transcoders = enabled;
	if (enabled == 0U)
		return 0;

	/* With the exception of DSI there is only ever one; pick the first. */
	for (trans = 0U; trans < 7U; trans++) {
		if ((enabled & (1U << trans)) != 0U)
			break;
	}
	cs->cpu_transcoder = (int)trans;

	/* A transcoder whose power is off is not read. */
	powered = i915_trans_power_on(pd, pwc, trans);
	if (powered == 0)
		return 0;

	/*
	 * TRANSCONF is addressed by the PIPE offsets (0x70008 + pipe*0x1000), not
	 * the transcoder ones; a DSI transcoder drives its own pipe TRANSCONF.
	 */
	transconf = drv_i915_read32(m, I915_TRANSCONF_A + crtc->pipe * I915_PIPE_STRIDE);
	cs->transconf = transconf;
	if ((transconf & I915_TRANSCONF_ENABLE) == 0U)
		return 0;

	/* Reads the timings. */
	i915_get_transcoder_timings(cs, trans, m);

	/* Reads the pipe source size. */
	reg = i915_trans_reg(trans, I915_PIPESRC_A);
	pipesrc = drv_i915_read32(m, reg);
	cs->pipe_src_w = ((pipesrc >> 16) & 0xffffU) + 1U;
	cs->pipe_src_h = (pipesrc & 0xffffU) + 1U;

	/* Succeeded: the pipe is active. */
	return 1;
}

/* Reads which planes of every crtc are visible (readout_plane_state(): skl_plane_get_hw_state() per plane). */
static void
i915_nogem_readout_plane_state(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	struct i915_crtc *crtc;
	struct i915_plane *plane;
	unsigned pipe;
	unsigned index;
	uint32_t reg;
	uint32_t ctl;
	int powered;

	/* Walks every crtc. */
	for (pipe = 0U; pipe < (unsigned)I915_NOGEM_MAX_PIPES; pipe++) {
		/* Skips a pipe without a crtc. */
		crtc = &d->crtcs[pipe];
		if (crtc->in_use == 0)
			continue;

		/* The plane registers are read only while the pipe's power is on. */
		powered = drv_i915_display_power_is_enabled(pd, (enum i915_power_domain)(I915_PW_DOMAIN_PIPE_A + pipe), pwc);
		crtc->state.active_planes = 0U;

		/* Reads every plane of the crtc. */
		for (index = 0U; index < crtc->num_planes; index++) {
			/* A plane starts invisible; one on a pipe whose power is off stays so. */
			plane = &crtc->planes[index];
			plane->visible = 0;
			if (powered == 0)
				continue;

			/* CUR_CTL is a different register block. */
			if (plane->type == I915_PLANE_CURSOR)
				continue;

			/* PLANE_CTL's enable bit makes the plane visible. */
			reg = I915_PLANE_CTL_1_A + pipe * I915_PIPE_STRIDE + (unsigned)plane->id * 0x100U;
			ctl = drv_i915_read32(m, reg);
			if ((ctl & I915_PLANE_CTL_ENABLE) != 0U) {
				plane->visible = 1;
				crtc->state.active_planes |= 1U << (unsigned)plane->id;
				d->readout_planes_visible++;
			}
		}
	}
}

/* Applies the early display workarounds (intel_early_display_was()); WA #1185 is display 10..12 only, not ADL-P. */
static void
i915_nogem_early_display_was(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	int display_ver)
{
	UNUSED_PARAMETER(m);

	/* GEN9_CLKGATE_DIS_0 |= DARBF_GATING_DIS; the gate is kept so the boundary is real. */
	if (display_ver >= 10 && display_ver <= 12)
		d->early_display_was_applied = 1;
}

/* Deactivates an FBC the firmware left active (intel_fbc_sanitize()). */
static void
i915_nogem_fbc_sanitize(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	unsigned fbc_mask)
{
	unsigned id;
	uint32_t reg;
	uint32_t ctl;

	/*
	 * for_each_intel_fbc(): the runtime fbc_mask owned by the P3 display state
	 * (ADL-P has BIT(INTEL_FBC_A) only).
	 */
	for (id = 0U; id < 2U; id++) {
		/* Skips an FBC the platform does not have. */
		if ((fbc_mask & (1U << id)) == 0U)
			continue;

		/* ilk_fbc_is_active(): DPFC_CTL_EN. */
		reg = (id == 0U) ? I915_ILK_DPFC_CONTROL_A : I915_ILK_DPFC_CONTROL_B;
		ctl = drv_i915_read32(m, reg);
		if ((ctl & I915_DPFC_CTL_EN) == 0U)
			continue;

		/* ilk_fbc_deactivate(): clear DPFC_CTL_EN. */
		i915_nogem_write(d, m, reg, ctl & ~I915_DPFC_CTL_EN);
		d->fbc_deactivated++;
	}
}

/* Gates the DDI clock a disabled encoder left ungated (intel_ddi_sanitize_encoder_pll_mapping()). */
static void
i915_nogem_sanitize_encoder_pll_mapping(
	struct i915_display_nogem *d,
	struct i915_encoder *e,
	struct i915_mmio *m)
{
	int clock_enabled;

	/* A linked encoder needs its clock. */
	if (e->crtc_linked != 0)
		return;

	/* A gated clock needs nothing. */
	clock_enabled = drv_i915_ddi_is_clock_enabled(e, m);
	if (clock_enabled == 0)
		return;

	/* Gates the clock. */
	kern_logf("i915: [ENCODER port %c] is disabled with an ungated DDI "
		"clock, gate it\n", (char)('A' + e->port));
	drv_i915_ddi_disable_clock(d, e, m);
	d->encoder_clocks_gated++;
}

/* Checks that an active crtc has encoders (intel_sanitize_crtc()); disabling one without is not implemented. */
static int
i915_nogem_sanitize_crtc(
	struct i915_display_nogem *d,
	struct i915_crtc *crtc)
{
	const struct i915_encoder *e;
	unsigned index;
	int has_encoders;

	/* An inactive pipe needs nothing. */
	if (crtc->state.active == 0)
		return 0;

	/*
	 * An active pipe would first have every non-primary plane disabled and
	 * the BIOS background colour committed; both need the plane/colour commit
	 * paths that belong to a later stage.  Looks for a linked encoder on it.
	 */
	has_encoders = 0;
	for (index = 0U; index < d->num_encoders; index++) {
		e = &d->encoders[index];
		if (e->crtc_linked != 0 && (e->pipe_mask & (1U << crtc->pipe)) != 0U)
			has_encoders = 1;
	}

	/* An active pipe with encoders is consistent. */
	if (has_encoders != 0)
		return 0;

	/* An active pipe without encoders is reported and left as it is. */
	kern_logf("i915: WARN [CRTC:%c] is active with no encoders: "
		"intel_crtc_disable_noatomic() is NOT implemented, pipe left as-is\n",
		(char)('A' + crtc->pipe));
	d->crtc_disable_noatomic_unimplemented = 1;

	/* Succeeded: the crtc was examined. */
	return 0;
}

/* Sets the CMTG DPT clock gating for DPLL0 on ADL-P A0..B0 (adlp_cmtg_clock_gating_wa()). */
static void
i915_nogem_adlp_cmtg_clock_gating_wa(
	struct i915_display_nogem *d,
	struct i915_mmio *m,
	int display_ver,
	int display_step,
	const struct i915_dpll *pll)
{
	uint32_t old;

	/* Only ADL-P steppings A0 up to (not including) B0 need it. */
	if (display_ver != 13)
		return;
	if (display_step < (int)I915_STEP_A0)
		return;
	if (display_step >= (int)I915_STEP_B0)
		return;

	/* And only for DPLL0. */
	if (pll->id != I915_DPLL_ID_ICL_DPLL0)
		return;

	/* Wa_16011069516:adl-p[a0] -- a double read, then set the gating bit. */
	(void)drv_i915_read32(m, I915_TRANS_CMTG_CHICKEN);
	old = i915_nogem_rmw(d, m, I915_TRANS_CMTG_CHICKEN, 0xffffffffU, I915_DISABLE_DPT_CLK_GATING);

	/* Other flags already set are unexpected. */
	if ((old & ~I915_DISABLE_DPT_CLK_GATING) != 0U)
		kern_logf("i915: Unexpected flags in TRANS_CMTG_CHICKEN: %08x\n", old);

	/* The workaround ran. */
	d->cmtg_wa_applied = 1;
}

/* Turns off every power well the firmware left on that nothing references (intel_power_domains_sanitize_state()). */
static void
i915_nogem_power_domains_sanitize_state(
	struct i915_display_nogem *d,
	struct i915_power_domains *pd,
	struct i915_pw_ctx *pwc)
{
	struct i915_power_well *well;
	unsigned index;
	int enabled;

	/*
	 * Walks the wells in REVERSE.  Note this uses a FRESH is_enabled() read,
	 * unlike __intel_display_power_is_enabled()'s cached check.
	 */
	index = pd->num_power_wells;
	while (index > 0U) {
		/* An always-on well and a referenced well stay on. */
		index--;
		well = &pd->power_wells[index];
		if (well->always_on != 0)
			continue;
		if (well->refcount != 0U)
			continue;

		/* A well that is off needs nothing. */
		enabled = drv_i915_power_well_is_enabled(well, pwc);
		if (enabled == 0)
			continue;

		/* Disables the unused well. */
		kern_logf("i915: BIOS left unused %s power well enabled, "
			"disabling it\n", well->name);
		(void)drv_i915_power_well_disable(well, pwc);
		d->wells_disabled++;
	}
}

/* Registers the decode callback with the arbiter (vga_client_register()); only a PCI VGA-class device is a client. */
static int
i915_vga_client_register(
	struct i915_vga_client *c)
{
	uint32_t class_code;

	/* !PCI_DISPLAY_CLASS_VGA: not an arbiter client. */
	class_code = drv_pci_device_class(c->gpu);
	if ((class_code & I915_PCI_CLASS_VGA_MASK) != I915_PCI_CLASS_VGA)
		return ENODEV;

	/* The callback is installed. */
	c->registered = 1;

	/* Succeeded: the device is an arbiter client. */
	return 0;
}

/* Takes the legacy VGA I/O resource: 1 when owned, 0 when refused. */
static int
i915_vga_get_legacy_io(
	struct i915_display *display)
{
	const struct i915_vga_io_ops *ops;
	int owned;

	/* Production: the driver owns the legacy VGA I/O on this device, so the get succeeds. */
	ops = display->g_vga_io;
	if (ops == NULL)
		return 1;

	/* Asks the injected accessor. */
	owned = ops->get(ops->ctx, (int)I915_VGA_RSRC_LEGACY_IO);

	/* Succeeded: reports whether the resource was granted. */
	return owned;
}

/* Reads a legacy VGA port. */
static unsigned char
i915_vga_in8(
	struct i915_display *display,
	unsigned short port)
{
	const struct i915_vga_io_ops *ops;
	unsigned char value;

	/* Reads through the injected accessor, or the port itself. */
	ops = display->g_vga_io;
	if (ops != NULL)
		value = ops->in8(ops->ctx, port);
	else
		value = kern_io_in8(port);

	/* Succeeded: reports the port value. */
	return value;
}

/* Writes a legacy VGA port. */
static void
i915_vga_out8(
	struct i915_display *display,
	unsigned short port,
	unsigned char value)
{
	const struct i915_vga_io_ops *ops;

	/* Writes through the injected accessor, or the port itself. */
	ops = display->g_vga_io;
	if (ops != NULL)
		ops->out8(ops->ctx, port, value);
	else
		kern_io_out8(port, value);
}

/* Gives the legacy VGA I/O resource back; production holds nothing to give back. */
static void
i915_vga_put_legacy_io(
	struct i915_display *display)
{
	const struct i915_vga_io_ops *ops;

	/* Only an injected accessor has a resource to release. */
	ops = display->g_vga_io;
	if (ops != NULL)
		ops->put(ops->ctx, (int)I915_VGA_RSRC_LEGACY_IO);
}

/* Reports CPUID.1:ECX[31], the hypervisor bit. */
static int
i915_cpu_hypervisor(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* Asks CPUID leaf 1. */
	eax = 1U;
	__asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	(void)ebx;
	(void)edx;

	/* Succeeded: reports the hypervisor bit. */
	return (int)((ecx >> 31) & 1U);
}

/* Reads the OpRegion and the VBT it names into the N0 report, without writing either. */
static void
i915_read_opregion(
	struct i915_display *display,
	const struct i915_native_deps *d,
	struct i915_native_report *r)
{
	uint8_t *copy;
	uint8_t *vcopy;
	void *op;
	void *vb;
	uint32_t size;
	unsigned index;
	int error;

	/* The copies live in the display. */
	copy = display->read_opregion_copy;
	vcopy = display->read_opregion_vcopy;

	/* ASLS 0: there is no OpRegion. */
	r->asls = d->asls;
	if (d->asls == 0U)
		return;

	/* Maps the OpRegion read-only. */
	op = NULL;
	error = hal_space_map_device((hal_physaddr_t)d->asls, I915_OPREGION_SIZE, HAL_SPACE_READ, &op);
	if (error != HAL_OK)
		return;
	if (op == NULL)
		return;

	/* Copies it and drops the mapping. */
	r->opregion_mapped = 1;
	for (index = 0U; index < I915_OPREGION_SIZE; index++)
		copy[index] = ((const volatile uint8_t *)op)[index];
	(void)hal_space_unmap_device(op, I915_OPREGION_SIZE);

	/* Finds where the OpRegion keeps the VBT. */
	error = drv_i915_opregion_locate_vbt(copy, I915_OPREGION_SIZE, d->asls, &r->op);
	if (error != 0)
		return;

	/* Reads the VBT from RVDA, or from mailbox #4 inside the OpRegion copy. */
	if (r->op.src == I915_OPVBT_RVDA) {
		/* Refuses an RVDA VBT larger than the copy, or one inside the OpRegion. */
		size = r->op.rvds;
		if (size > I915_NATIVE_VBT_COPY_SIZE)
			return;
		if (r->op.rvda_inside != 0)
			return;

		/* Maps the RVDA VBT read-only. */
		vb = NULL;
		error = hal_space_map_device((hal_physaddr_t)r->op.vbt_phys, size, HAL_SPACE_READ, &vb);
		if (error != HAL_OK)
			return;
		if (vb == NULL)
			return;

		/* Copies it, drops the mapping, validates and hashes it. */
		r->vbt_mapped = 1;
		for (index = 0U; index < size; index++)
			vcopy[index] = ((const volatile uint8_t *)vb)[index];
		(void)hal_space_unmap_device(vb, size);
		r->vbt_size = size;
		r->vbt_valid = drv_i915_vbt_validate(vcopy, size);
		drv_i915_sha256(vcopy, size, r->vbt_sha256);
	} else {
		/* Validates and hashes the mailbox #4 VBT in place. */
		r->vbt_mapped = 1;
		r->vbt_size = r->op.vbt_max;
		r->vbt_valid = drv_i915_vbt_validate(copy + r->op.vbt_offset, r->op.vbt_max);
		drv_i915_sha256(copy + r->op.vbt_offset, r->op.vbt_max, r->vbt_sha256);
	}

	/* Compares a valid VBT with the explicit blob's pinned hash. */
	if (d->vbt_pin != NULL && r->vbt_valid != 0) {
		r->vbt_matches_pin = 1;
		for (index = 0U; index < 32U; index++) {
			if (r->vbt_sha256[index] != d->vbt_pin[index])
				r->vbt_matches_pin = 0;
		}
	}
}

/* Reads the GPU's VT-d unit through the MCHBAR mirror into the N0 report, without writing it. */
static void
i915_read_vtd(
	const struct i915_native_deps *d,
	struct i915_native_report *r)
{
	uint32_t low;
	uint32_t high;
	uint64_t base;
	void *unit;
	int error;

	/* Reads GFXVTBAR, low half first. */
	low = drv_i915_read32(d->mmio, I915_GFXVTBAR_LO);
	high = drv_i915_read32(d->mmio, I915_GFXVTBAR_LO + 4U);
	r->gfxvtbar = (uint64_t)low | (uint64_t)high << 32;

	/* A disabled unit, or one at address 0, is not read. */
	r->vtd_enabled = (int)(r->gfxvtbar & 1U);
	base = r->gfxvtbar & ~0xfffULL;
	if (r->vtd_enabled == 0)
		return;
	if (base == 0U)
		return;

	/* Maps the unit's registers read-only. */
	unit = NULL;
	error = hal_space_map_device((hal_physaddr_t)base, I915_VTD_WINDOW, HAL_SPACE_READ, &unit);
	if (error != HAL_OK)
		return;
	if (unit == NULL)
		return;

	/* Reads the version, the global status and the protected memory enable. */
	r->vtd_ver = *(const volatile uint32_t *)((const volatile uint8_t *)unit + I915_VTD_VER);
	r->vtd_gsts = *(const volatile uint32_t *)((const volatile uint8_t *)unit + I915_VTD_GSTS);
	r->vtd_pmen = *(const volatile uint32_t *)((const volatile uint8_t *)unit + I915_VTD_PMEN);
	(void)hal_space_unmap_device(unit, I915_VTD_WINDOW);

	/*
	 * A real unit reports its version (non-zero, not all-ones); zeros / all-ones
	 * = nothing decodes there (e.g. a guest's view of the host's address) --
	 * then GSTS / PMEN are not register values.
	 */
	r->vtd_readable = 0;
	if (r->vtd_ver != 0U && r->vtd_ver != 0xffffffffU)
		r->vtd_readable = 1;
}

/* Records the firmware framebuffer and the GGTT pages it occupies in the aperture. */
static void
i915_read_fb(
	const struct i915_native_deps *d,
	struct i915_native_report *r)
{
	const struct zbl6_framebuffer *fb;

	/* No framebuffer in the boot handoff: nothing to record. */
	fb = kern_boot_handoff("pcat.framebuffer");
	if (fb == NULL)
		return;
	if (fb->size == 0U)
		return;

	/* Records the framebuffer. */
	r->fb_present = 1;
	r->fb_base = fb->physical_base;
	r->fb_size = fb->size;

	/* A framebuffer inside the aperture occupies GGTT pages. */
	if (fb->physical_base >= d->gmadr_base && fb->physical_base + fb->size <= d->gmadr_base + d->gmadr_size) {
		r->fb_in_aperture = 1;
		r->fb_ggtt_first = (uint32_t)((fb->physical_base - d->gmadr_base) >> 12);
		r->fb_ggtt_pages = (uint32_t)((fb->size + 4095U) >> 12);
	}
}

/* Reads every pipe whose power is on, and the pipe A domain, into the N0 report. */
static void
i915_read_display(
	const struct i915_native_deps *d,
	struct i915_native_report *r)
{
	struct i915_mmio *m;
	struct i915_native_pipe *q;
	unsigned pipe;
	uint32_t offset;

	/* HSW_PWR_WELL_CTL2: the STATE bits the power decision reads; all-ones is not a register value. */
	m = d->mmio;
	r->pwr_well_ctl = drv_i915_read32(m, I915_HSW_PWR_WELL_CTL2);

	/* Classifies every pipe. */
	for (pipe = 0U; pipe < 4U; pipe++) {
		/* A power state that is not a register value is a read error. */
		q = &r->pipe[pipe];
		offset = I915_PIPE_STRIDE * pipe;
		if (r->pwr_well_ctl == 0xffffffffU) {
			q->cls = I915_N0_READ_ERROR;
			r->unreadable_pipes |= 1U << pipe;
			continue;
		}

		/* A pipe whose domains are off is not read. */
		q->readable = d->pipe_powered(d->ctx, pipe);
		if (q->readable == 0) {
			q->cls = I915_N0_POWER_OFF;
			r->unreadable_pipes |= 1U << pipe;
			continue;
		}

		/* Reads the transcoder, the pipe and the primary plane. */
		q->transconf = drv_i915_read32(m, I915_TRANSCONF_A + offset);
		q->trans_ddi_func = drv_i915_read32(m, I915_TRANS_DDI_FUNC_CTL_A + offset);
		q->pipesrc = drv_i915_read32(m, I915_PIPESRC_A + offset);
		q->plane_ctl = drv_i915_read32(m, I915_PLANE_CTL_1_A + offset);
		q->plane_stride = drv_i915_read32(m, I915_PLANE_STRIDE_1_A + offset);
		q->plane_size = drv_i915_read32(m, I915_PLANE_SIZE_1_A + offset);
		q->plane_surf = drv_i915_read32(m, I915_PLANE_SURF_1_A + offset);

		/* An all-ones register is a read error. */
		if (q->transconf == 0xffffffffU || q->plane_ctl == 0xffffffffU) {
			q->cls = I915_N0_READ_ERROR;
			continue;
		}

		/* An enabled transcoder or plane makes the pipe active. */
		q->cls = I915_N0_READABLE_INACTIVE;
		if ((q->transconf & 0x80000000U) != 0U || (q->plane_ctl & 0x80000000U) != 0U) {
			q->cls = I915_N0_READABLE_ACTIVE;
			r->active_pipes |= 1U << pipe;
		}
	}

	/* Pipe A's domain is on when these are meaningful (PW1 / DDI A): read only when pipe A is readable. */
	if (r->pipe[0].readable != 0) {
		r->pll_enable[0] = drv_i915_read32(m, I915_DPLL0_ENABLE);
		r->pll_enable[1] = drv_i915_read32(m, I915_DPLL1_ENABLE);
		r->ddi_buf_ctl_a = drv_i915_read32(m, I915_DDI_BUF_CTL_A);
		r->pp_status = drv_i915_read32(m, I915_PP_STATUS);
		r->pp_control = drv_i915_read32(m, I915_PP_CONTROL);
		r->blc_ctl = drv_i915_read32(m, I915_BLC_PWM_CTL);
		r->blc_duty = drv_i915_read32(m, I915_BLC_PWM_DUTY);
		r->dpclka_cfgcr0 = drv_i915_read32(m, I915_ICL_DPCLKA_CFGCR0);
	}
}

/* Names where the OpRegion keeps the VBT: RVDA, mailbox #4 or none. */
static const char *
i915_opvbt_source_name(
	int src)
{
	/* RVDA: a physical address the OpRegion names. */
	if (src == I915_OPVBT_RVDA)
		return "RVDA";

	/* Mailbox #4 inside the OpRegion. */
	if (src == I915_OPVBT_MAILBOX4)
		return "mailbox#4";

	/* No VBT in the OpRegion. */
	return "none";
}

/* Names the observed VBT's origin as the N0 VBT line spells it. */
static const char *
i915_opvbt_observed_name(
	int src)
{
	/* RVDA: a physical address the OpRegion names. */
	if (src == I915_OPVBT_RVDA)
		return "OPREGION(RVDA)";

	/* Mailbox #4 inside the OpRegion. */
	if (src == I915_OPVBT_MAILBOX4)
		return "OPREGION(mailbox#4)";

	/* No VBT in the OpRegion. */
	return "none";
}

/* Names the VBT source the parser consumed. */
static const char *
i915_vbt_source_name(
	int parser_src)
{
	/* The pinned explicit blob. */
	if (parser_src == I915_VBT_SRC_EXPLICIT_BLOB)
		return "EXPLICIT_BLOB";

	/* The OpRegion's VBT. */
	if (parser_src == I915_VBT_SRC_OPREGION)
		return "OPREGION";

	/* The PCI expansion ROM. */
	if (parser_src == I915_VBT_SRC_PCI_ROM)
		return "PCI_ROM";

	/* No VBT. */
	return "NONE";
}

/* Spells the first 8 bytes of a sha256 as 16 hex digits and a terminator in text[17]. */
static void
i915_sha_prefix(
	const uint8_t *sha256,
	char *text)
{
	static const char hex[] = "0123456789abcdef";
	unsigned index;

	/* Two digits per byte, high nibble first. */
	for (index = 0U; index < 8U; index++) {
		text[2U * index] = hex[sha256[index] >> 4];
		text[2U * index + 1U] = hex[sha256[index] & 15U];
	}

	/* Ends the string. */
	text[16] = '\0';
}
