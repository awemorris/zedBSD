/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display the firmware left running, read before the driver's first
 * display write (see xd-takeover.h).
 */

#include "xd-takeover.h"
#include "xd-clock.h"
#include "xd-ddi.h"
#include "xd-dmc.h"
#include "xd-io.h"
#include "xd-pipe.h"
#include "xd-platform.h"
#include "xd-power.h"
#include "display-internal.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd-south.h"
#include "../intel/display-regs-xd9.h"

#include <kern/klog.h>
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The GPU's VT-d unit base (GFXVTBAR) as the MCHBAR mirror in the
 * display's register space shows it: bit 0 enables the unit, the page
 * above it is the unit's registers.
 *
 * Values referenced: Intel 12th Generation Intel Core Processors
 * Datasheet, Volume 2 (MCHBAR GFXVTBAR).
 */
#define I915_XD_FW_GFXVTBAR		0x145400U
#define I915_XD_FW_GFXVTBAR_ENABLE	0x1ULL
#define I915_XD_FW_GFXVTBAR_BASE	(~0xfffULL)

/*
 * The VT-d unit's global status bits the decision reads: translation
 * enabled, and interrupt remapping enabled; and the protected memory
 * enable and its status.
 *
 * Values referenced: Intel Virtualization Technology for Directed I/O
 * Architecture Specification (GSTS_REG TES and IRES, PMEN_REG EPM and
 * PRS).
 */
#define I915_XD_FW_VTD_TRANSLATES	0x80000000U
#define I915_XD_FW_VTD_REMAPS		0x02000000U
#define I915_XD_FW_VTD_PROTECTS		0x80000001U

/* A register read that is all ones: nothing decoded the access. */
#define I915_XD_FW_ALL_ONES		0xffffffffU

/* Where the DDI select and the mode sit in the transcoder's DDI function word. */
#define I915_XD_FW_DDI_SELECT_SHIFT	27U
#define I915_XD_FW_DDI_MODE_SHIFT	24U

/*
 * The legacy VGA screen-off settle time before the VGA plane is
 * disabled.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vga.c
 * (intel_vga_disable).
 */
#define I915_XD_FW_VGA_SETTLE_US	300U

/* Where the PLL select sits in a DDI clock select. */
#define I915_XD_FW_CLK_SEL_SHIFT	28U

/* How long a plane's disable waits for the next vertical blank, in 1 ms steps. */
#define I915_XD_FW_VBLANK_MS		100U

/* The size of a GGTT page. */
#define I915_XD_FW_PAGE_SHIFT		12U
#define I915_XD_FW_PAGE_SIZE		4096U

/*
 * The display 9 readout's fields: the DDI select's width in the DDI
 * function, the refresh range a measured timing is believed in (20 Hz to
 * 250 Hz), and the check's stride and height fields of the first plane.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (TRANS_DDI_FUNC_CTL, PLANE_STRIDE, PLANE_SIZE).
 */
#define I915_XD9_FW_DDI_MASK		0x7U
#define I915_XD9_FW_REFRESH_MIN		20000U
#define I915_XD9_FW_REFRESH_MAX		250000U
#define I915_XD9_FW_STRIDE_MASK		0x7ffU
#define I915_XD9_FW_HEIGHT_MASK		0xfffU

static unsigned i915_xd_fw_transcoder(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe);
static int i915_xd_fw_function_ddi(const struct i915_xd_platform *platform, uint32_t function, unsigned *ddi);
static int i915_xd_fw_pipe_powered(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe, unsigned trans);
static void i915_xd_fw_read_pipe(struct i915_xd_io *io, unsigned pipe, unsigned trans, struct i915_xd_firmware *firmware);
static void i915_xd_fw_read_pipe_a_domain(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_firmware *firmware);
static void i915_xd_fw_read_fb(const struct i915_xd_fw_env *env, struct i915_xd_firmware *firmware);
static void i915_xd_fw_read_vtd(struct i915_xd_io *io, const struct i915_xd_fw_env *env, struct i915_xd_firmware *firmware);
static void i915_xd_fw_vtd_walls(struct i915_xd_firmware *firmware);
static void i915_xd_fw_choose_stop(struct i915_xd_firmware *firmware);
static void i915_xd9_fw_read_transcoder(struct i915_xd_io *io, struct i915_xd9_fw_registers *registers, unsigned transcoder);
static void i915_xd9_fw_read_pipe(struct i915_xd_io *io, struct i915_xd9_fw_registers *registers, unsigned pipe);
static int i915_xd9_fw_decode_plane(const struct i915_xd9_fw_registers *registers, struct i915_xd9_fw_readout *readout);
static void i915_xd9_fw_decode_refresh(const struct i915_xd9_fw_registers *registers, struct i915_xd9_fw_readout *readout);
static int i915_xd9_fw_transcoder_pipe(const struct i915_xd9_fw_registers *registers, unsigned transcoder, unsigned *pipe);
static const char *i915_xd9_fw_transcoder_name(unsigned transcoder);
static void i915_xd9_fw_check_plane(struct i915_xd_io *io, unsigned pipe, uint64_t driver_start, struct i915_xd9_fw_check *check);
static unsigned i915_xd_fw_planes_off(struct i915_xd_io *io, unsigned pipe);
static const struct i915_xd_pll_desc *i915_xd_fw_pll_of(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port);
static int i915_xd_fw_pll_shared(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_firmware *firmware, unsigned pipe, const struct i915_xd_pll_desc *pll);
static void i915_xd_fw_clock_off(struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_port_desc *port);
static void i915_xd_fw_hook(void (*hook)(void *context, unsigned pipe), void *context, unsigned pipe);

/*
 * Reads what the firmware left on the display into a record, writing
 * nothing, and decides from it (drv_i915_xd_firmware_decide).  opregion
 * is the OpRegion's address (ASLS, 0 for none), which is only recorded.
 * A device without a description is recorded as one: the decision stops.
 * Returns 0, or ENODEV for a device without a description.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_modeset_setup.c
 * (intel_modeset_readout_hw_state, which reads a pipe only when its power
 * domain is on), intel_display.c (hsw_enabled_transcoders and
 * hsw_get_transcoder_state: display 11's eDP transcoder feeds the pipe its
 * input names, and its power domain is not in a switched well).
 */
int
drv_i915_xd_firmware_read(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_fw_env *env,
	uint32_t opregion,
	struct i915_xd_firmware *firmware)
{
	unsigned pipe;
	unsigned pipes;
	unsigned trans;
	int powered;

	/* Starts from an empty record with what the environment says. */
	kern_memset(firmware, 0, sizeof(*firmware));
	firmware->output_ddi = -1;
	firmware->hypervisor = env->hypervisor;
	firmware->ggtt_pages = env->ggtt_pages;
	firmware->driver_first_page = env->driver_first_page;
	firmware->opregion = opregion;

	/* The framebuffer and the VT-d unit are read whatever the display is. */
	i915_xd_fw_read_fb(env, firmware);
	i915_xd_fw_read_vtd(io, env, firmware);

	/* A device without a description: nothing on the display is known. */
	if (platform == NULL) {
		firmware->walls |= I915_XD_FW_WALL_UNDESCRIBED;
		drv_i915_xd_firmware_decide(firmware);
		return ENODEV;
	}

	/* The main well control: its states gate every pipe; all ones is not a register value. */
	firmware->well_control = drv_i915_xd_io_read(io, I915_XD_PWR_WELL_CTL);

	/* Reads every pipe whose wells are on, and marks the rest. */
	pipes = platform->pipes;
	if (pipes > I915_XD_FW_PIPES)
		pipes = I915_XD_FW_PIPES;
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		/* A pipe the platform does not have reads as off. */
		if (pipe >= pipes) {
			firmware->pipe[pipe].state = I915_XD_FW_PIPE_OFF;
			firmware->unread_pipes |= 1U << pipe;
			continue;
		}

		/* A well state that is not a register value is a garbled pipe. */
		if (firmware->well_control == I915_XD_FW_ALL_ONES) {
			firmware->pipe[pipe].state = I915_XD_FW_PIPE_GARBLED;
			firmware->unread_pipes |= 1U << pipe;
			continue;
		}

		/* A pipe whose wells (and its transcoder's) are off is not read. */
		trans = i915_xd_fw_transcoder(io, platform, pipe);
		powered = i915_xd_fw_pipe_powered(io, platform, pipe, trans);
		if (!powered) {
			firmware->pipe[pipe].state = I915_XD_FW_PIPE_OFF;
			firmware->unread_pipes |= 1U << pipe;
			continue;
		}

		/* Reads the pipe and its transcoder. */
		i915_xd_fw_read_pipe(io, pipe, trans, firmware);
	}

	/* Pipe A's domain carries the panel's parts: read only when pipe A reads. */
	if (firmware->pipe[0].state == I915_XD_FW_PIPE_DARK || firmware->pipe[0].state == I915_XD_FW_PIPE_LIT)
		i915_xd_fw_read_pipe_a_domain(io, platform, firmware);

	/* The VGA plane, whose well is the always-on one. */
	firmware->vga_control = drv_i915_xd_io_read(io, I915_XD_VGA_CONTROL);

	/* Decides from the record. */
	drv_i915_xd_firmware_decide(firmware);

	/* Succeeded: the record and its decision. */
	return 0;
}

/*
 * Decides from a record alone whether the driver may use the display.
 *
 * Every wall is recorded, so one start names every wall it can see; the
 * first in order is the one that stops: a device without a description,
 * a pipe that did not read, a firmware scanout in the GGTT pages the
 * driver writes, a VT-d unit that does not read, one that translates or
 * protects memory.  A lit pipe is no stop: the driver takes the
 * firmware's output over before its first write.
 */
void
drv_i915_xd_firmware_decide(
	struct i915_xd_firmware *firmware)
{
	uint32_t last_page;
	unsigned pipe;

	/* Keeps only the wall the readout alone knows, and the OpRegion's record. */
	firmware->walls &= I915_XD_FW_WALL_UNDESCRIBED;
	if (firmware->opregion != 0U)
		firmware->walls |= I915_XD_FW_WALL_OPREGION;

	/* A lit pipe, and a pipe that did not read. */
	firmware->lit_pipes = 0U;
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		if (firmware->pipe[pipe].state == I915_XD_FW_PIPE_LIT)
			firmware->lit_pipes |= 1U << pipe;
		if (firmware->pipe[pipe].state == I915_XD_FW_PIPE_GARBLED)
			firmware->walls |= I915_XD_FW_WALL_GARBLED;
	}

	/* Any lit pipe is recorded as a wall. */
	if (firmware->lit_pipes != 0U)
		firmware->walls |= I915_XD_FW_WALL_LIT;

	/* The framebuffer may not reach the pages the driver writes. */
	if (firmware->fb_in_aperture && firmware->fb_first_page + firmware->fb_pages > firmware->driver_first_page)
		firmware->walls |= I915_XD_FW_WALL_OVERLAP;

	/* Nor may the surface of a lit pipe. */
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		if ((firmware->lit_pipes & (1U << pipe)) == 0U)
			continue;

		/* A lit plane whose pages reach the driver's is an overlap. */
		last_page = firmware->pipe[pipe].plane_surface >> I915_XD_FW_PAGE_SHIFT;
		if (last_page + 1U > firmware->driver_first_page)
			firmware->walls |= I915_XD_FW_WALL_OVERLAP;
	}

	/* The VT-d unit of a machine that owns it. */
	i915_xd_fw_vtd_walls(firmware);

	/* Takes the first stop, or uses the display (taking a lit one over). */
	i915_xd_fw_choose_stop(firmware);
}

/*
 * Names the output the firmware lit, from the pipes the readout read and
 * the ports of the platform's description.
 *
 * The output is the lowest lit pipe whose transcoder drives a DDI (a
 * firmware that clones onto two outputs gives the lowest pipe).
 * DisplayPort SST on the panel's DDI (panel_ddi, -1 for none) is the
 * panel; HDMI or DVI on a combo port is the HDMI output; DisplayPort SST
 * on a Type-C port is the Type-C DisplayPort; DisplayPort SST on a combo
 * port that is not the panel's (the monitor of a desktop, whose Type-C
 * connectors may be combo ports) is the combo DisplayPort, which the
 * driver does not light; anything else (a DDI the description does not
 * have or drives with no PHY, DisplayPort MST, HDMI on a Type-C port) is
 * an output the driver does not light.
 */
void
drv_i915_xd_firmware_classify(
	const struct i915_xd_platform *platform,
	int panel_ddi,
	struct i915_xd_firmware *firmware)
{
	const struct i915_xd_port_desc *port;
	uint32_t function;
	unsigned ddi;
	unsigned mode;
	unsigned pipe;
	int drives;

	/* Nothing lit until a pipe says so; the panel's DDI is kept for the takeover. */
	firmware->panel_ddi = panel_ddi;
	firmware->output = I915_XD_FW_OUTPUT_NONE;
	firmware->output_pipe = 0U;
	firmware->output_ddi = -1;
	firmware->output_mode = 0U;

	/* The lowest lit pipe that drives a DDI. */
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		/* A pipe the firmware did not light. */
		if ((firmware->lit_pipes & (1U << pipe)) == 0U)
			continue;

		/* A DDI function that is off or drives no DDI is not an output. */
		function = firmware->pipe[pipe].ddi_function;
		if ((function & I915_XD_DDI_FUNC_ENABLE) == 0U)
			continue;
		drives = i915_xd_fw_function_ddi(platform, function, &ddi);
		if (!drives)
			continue;

		/* The output: its pipe, DDI and mode. */
		mode = (function & I915_XD_DDI_FUNC_MODE) >> I915_XD_FW_DDI_MODE_SHIFT;
		firmware->output_pipe = pipe;
		firmware->output_ddi = (int)ddi;
		firmware->output_mode = mode;
		firmware->output = I915_XD_FW_OUTPUT_OTHER;
		break;
	}

	/* No lit output, or no description to read the port in. */
	if (firmware->output == I915_XD_FW_OUTPUT_NONE)
		return;
	if (platform == NULL)
		return;

	/* A DDI the description does not have, or drives with no PHY, is not lit by the driver. */
	port = drv_i915_xd_platform_port(platform, (unsigned)firmware->output_ddi);
	if (port == NULL)
		return;
	if (port->phy_kind == I915_XD_PHY_NONE)
		return;

	/* The panel: DisplayPort SST on the panel's DDI. */
	if (firmware->output_ddi == panel_ddi && firmware->output_mode == I915_XD_DDI_FUNC_MODE_DP_SST) {
		firmware->output = I915_XD_FW_OUTPUT_PANEL;
		return;
	}

	/* HDMI or DVI on a combo port. */
	if (port->phy_kind == I915_XD_PHY_COMBO &&
	    (firmware->output_mode == I915_XD_DDI_FUNC_MODE_HDMI || firmware->output_mode == I915_XD_DDI_FUNC_MODE_DVI)) {
		firmware->output = I915_XD_FW_OUTPUT_HDMI;
		return;
	}

	/* DisplayPort SST on a Type-C port (a Dekel or an MG PHY). */
	if (port->tc != 0U && firmware->output_mode == I915_XD_DDI_FUNC_MODE_DP_SST) {
		firmware->output = I915_XD_FW_OUTPUT_DP_TC;
		return;
	}

	/* DisplayPort SST on a combo port that is not the panel's. */
	if (port->phy_kind == I915_XD_PHY_COMBO && firmware->output_mode == I915_XD_DDI_FUNC_MODE_DP_SST)
		firmware->output = I915_XD_FW_OUTPUT_DP_COMBO;
}

/*
 * Names a kind of firmware output for the log.
 */
const char *
drv_i915_xd_firmware_output_name(
	unsigned output)
{
	/* One name per kind. */
	switch (output) {
	case I915_XD_FW_OUTPUT_NONE:
		return "none";
	case I915_XD_FW_OUTPUT_PANEL:
		return "panel";
	case I915_XD_FW_OUTPUT_HDMI:
		return "HDMI";
	case I915_XD_FW_OUTPUT_DP_TC:
		return "Type-C DP";
	case I915_XD_FW_OUTPUT_DP_COMBO:
		return "combo DP";
	default:
		break;
	}

	/* Succeeded: an output the driver does not light. */
	return "other";
}

/*
 * Logs the record and its decision in two lines: the firmware's display
 * (the lit and unread pipes, the output, the framebuffer's pages, the
 * VT-d unit) and the decision with its walls.
 */
void
drv_i915_xd_firmware_log(
	const struct i915_xd_firmware *firmware)
{
	/* What the firmware left. */
	kern_logf("i915: xd-takeover: lit pipes 0x%x, unread 0x%x, output %s (pipe %c, DDI %d, mode %u), framebuffer %s pages %u..%u, VT-d %s (status 0x%08x, protect 0x%08x)\n",
	    firmware->lit_pipes,
	    firmware->unread_pipes,
	    drv_i915_xd_firmware_output_name(firmware->output),
	    'A' + (int)firmware->output_pipe,
	    firmware->output_ddi,
	    firmware->output_mode,
	    firmware->fb_in_aperture ? "in the aperture" : "outside the aperture",
	    firmware->fb_first_page,
	    firmware->fb_first_page + firmware->fb_pages,
	    firmware->vtd_enabled ? "on" : "off",
	    firmware->vtd_status,
	    firmware->vtd_protect);

	/* The decision. */
	kern_logf("i915: xd-takeover: %s, take over %d, walls 0x%x, stop 0x%x: %s\n",
	    firmware->use ? "use" : "keep the firmware's picture",
	    firmware->take_over,
	    firmware->walls,
	    firmware->stop,
	    firmware->reason);
}

/*
 * Stops one lit pipe the firmware left, in the order an output is
 * disabled (see xd-takeover.h), and marks it dark in the record.  The
 * transcoder that feeds the pipe (display 11's eDP transcoder when its
 * input is the pipe), its DDI, its mode and its PLL are read from the
 * hardware; the PLL is turned off unless another lit pipe still uses it.
 * Display 11 takes the transcoder's clock off before the DDI buffer and
 * stops the link on the port's own transport.  dmc may be NULL (no
 * pipe DMC is switched).  Returns 0, EINVAL for a pipe that is not lit or
 * drives no DDI, or ENODEV for a DDI the description does not have.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_modeset_setup.c
 * (intel_crtc_disable_noatomic_begin), intel_display.c (hsw_crtc_disable,
 * intel_disable_transcoder), intel_ddi.c (intel_disable_ddi,
 * intel_ddi_post_disable, intel_ddi_post_disable_dp,
 * intel_ddi_post_disable_hdmi, intel_ddi_disable_transcoder_clock,
 * icl_ddi_combo_disable_clock, icl_ddi_tc_disable_clock), intel_dp.c
 * (intel_dp_set_infoframes), intel_hdmi.c (hsw_set_infoframes),
 * skl_scaler.c (skl_scaler_disable), intel_dpll_mgr.c
 * (intel_disable_shared_dpll), intel_dmc.c (intel_dmc_disable_pipe).
 */
int
drv_i915_xd_takeover_stop(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_dmc *dmc,
	struct i915_xd_firmware *firmware,
	unsigned pipe,
	const struct i915_xd_takeover_hooks *hooks,
	struct i915_xd_takeover_report *report)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_ddi_link link;
	uint32_t function;
	uint32_t islands;
	uint32_t transport;
	unsigned trans;
	unsigned ddi;
	unsigned waits;
	unsigned late;
	int was_running;
	int drives;
	int shared;
	int panel;
	int dp;

	/* Nothing stopped yet. */
	kern_memset(report, 0, sizeof(*report));
	report->ddi = -1;
	report->pll = -1;

	/* A pipe that is not lit, or drives no DDI, has no output to stop. */
	if (pipe >= I915_XD_FW_PIPES)
		return EINVAL;
	if ((firmware->lit_pipes & (1U << pipe)) == 0U)
		return EINVAL;
	trans = i915_xd_fw_transcoder(io, platform, pipe);
	function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(trans));
	drives = i915_xd_fw_function_ddi(platform, function, &ddi);
	if (!drives)
		return EINVAL;

	/* The DDI's description, its mode, and whether it is the panel. */
	report->ddi = (int)ddi;
	report->mode = (function & I915_XD_DDI_FUNC_MODE) >> I915_XD_FW_DDI_MODE_SHIFT;
	port = drv_i915_xd_platform_port(platform, (unsigned)report->ddi);
	if (port == NULL)
		return ENODEV;
	dp = report->mode == I915_XD_DDI_FUNC_MODE_DP_SST;
	panel = dp && report->ddi == firmware->panel_ddi;

	/* The PLL the DDI runs on, read before its clock is gated. */
	pll = i915_xd_fw_pll_of(io, platform, port);
	drv_i915_xd_io_step(io, "xd-takeover stop");

	/* The planes, with a vertical blank for their disable to take effect. */
	late = i915_xd_fw_planes_off(io, pipe);

	/* The panel's backlight, and the HDMI sink's scrambling. */
	if (panel)
		i915_xd_fw_hook(hooks->backlight_off, hooks->context, pipe);
	if (!dp)
		i915_xd_fw_hook(hooks->sink_before, hooks->context, pipe);

	/* The transcoder and its DDI function. */
	waits = drv_i915_xd_pipe_stop(io, platform, trans, &was_running);
	if ((waits & I915_XD_PIPE_LATE_OFF) != 0U)
		late |= I915_XD_TAKEOVER_LATE_PIPE;
	(void)drv_i915_xd_pipe_func_disable(io, trans, 0);

	/* The scalers. */
	drv_i915_xd_pipe_scalers_off(io, pipe);

	/* The data islands (a DisplayPort link's picture parameters too). */
	islands = I915_XD_DIP_INFOFRAMES | I915_XD_DIP_VSC;
	if (dp)
		islands |= I915_XD_DIP_PPS;
	(void)drv_i915_xd_io_rmw(io, I915_XD_DIP_CTL(trans), islands, 0U);
	drv_i915_xd_io_posting_read(io, I915_XD_DIP_CTL(trans));

	/* A DisplayPort sink goes to sleep before the port stops. */
	if (dp)
		i915_xd_fw_hook(hooks->sink_after, hooks->context, pipe);

	/* Display 11 takes the clock off the transcoder before the buffer stops. */
	if (platform->display_ver < 12U)
		drv_i915_xd_pipe_clock_off(io, trans);

	/* The DDI buffer and the transport (display 11's is the port's), with FEC when the link used it. */
	kern_memset(&link, 0, sizeof(link));
	link.ddi = ddi;
	link.trans = trans;
	if (platform->display_ver < 12U)
		link.trans = I915_XD_DP_TP_PORT(ddi);
	link.tc = port->tc != 0U;
	transport = drv_i915_xd_io_read(io, I915_XD_DP_TP_CTL(link.trans));
	if (dp && (transport & I915_XD_DP_TP_FEC) != 0U)
		link.fec = 1;
	waits = drv_i915_xd_ddi_stop(io, &link, dp);
	if ((waits & I915_XD_DDI_LATE_IDLE) != 0U)
		late |= I915_XD_TAKEOVER_LATE_BUFFER;

	/* From display 12 the transcoder's clock goes after the buffer. */
	if (platform->display_ver >= 12U)
		drv_i915_xd_pipe_clock_off(io, trans);

	/* The panel's power, the port's power references, then its clock. */
	if (panel)
		i915_xd_fw_hook(hooks->panel_off, hooks->context, pipe);
	i915_xd_fw_hook(hooks->port_release, hooks->context, pipe);
	i915_xd_fw_clock_off(io, platform, port);

	/* The PLL, unless another lit pipe still runs on it. */
	firmware->lit_pipes &= ~(1U << pipe);
	firmware->pipe[pipe].state = I915_XD_FW_PIPE_DARK;
	if (pll != NULL) {
		shared = i915_xd_fw_pll_shared(io, platform, firmware, pipe, pll);
		if (!shared) {
			report->pll = (int)(pll - platform->plls);
			waits = drv_i915_xd_dpll_disable(io, pll);
			if (waits != 0U)
				late |= I915_XD_TAKEOVER_LATE_PLL;
		}
	}

	/* The link's references, and the pipe's DMC. */
	i915_xd_fw_hook(hooks->link_release, hooks->context, pipe);
	if (dmc != NULL)
		drv_i915_xd_dmc_pipe(dmc, io, pipe, 0);

	/* Succeeded: the pipe is dark. */
	report->late = late;
	return 0;
}

/*
 * Corrects what the firmware left on but unused, before the driver starts
 * using the display: a lit pipe gets its pipe DMC, the frame buffer
 * compressor is turned off (the driver does not use it), every port that
 * drives no lit pipe has its clock gated, and every PLL no lit pipe runs
 * on is turned off.  When the PLL of a lit pipe's port cannot be read
 * (a Type-C port's select is one this code does not know), no PLL is
 * turned off.  The lit pipes themselves are left as they are: the
 * takeover stops them.  dmc may be NULL.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_modeset_setup.c
 * (intel_modeset_setup_hw_state, intel_sanitize_crtc), intel_ddi.c
 * (intel_ddi_sanitize_encoder_pll_mapping), intel_dpll_mgr.c
 * (intel_dpll_sanitize_state), intel_fbc.c (intel_fbc_sanitize).
 */
void
drv_i915_xd_firmware_sanitize(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_dmc *dmc,
	const struct i915_xd_firmware *firmware,
	struct i915_xd_sanitize_report *report)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_dpll_words words;
	uint32_t control;
	uint32_t function;
	unsigned used_ddis;
	unsigned used_plls;
	unsigned select;
	unsigned trans;
	unsigned index;
	unsigned pipe;
	unsigned ddi;
	int drives;
	int unknown;
	int lit;
	int on;
	int has_dmc;

	/* Nothing changed yet. */
	kern_memset(report, 0, sizeof(*report));
	drv_i915_xd_io_step(io, "xd-takeover sanitize");

	/* Each lit pipe's DMC, its DDI, and the PLL that DDI runs on. */
	used_ddis = 0U;
	used_plls = 0U;
	unknown = 0;
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		/* A pipe that is not lit is passed over. */
		if ((firmware->lit_pipes & (1U << pipe)) == 0U)
			continue;

		/* The pipe's DMC program runs again when the DMC has one. */
		has_dmc = 0;
		if (dmc != NULL)
			has_dmc = drv_i915_xd_dmc_has(dmc, I915_XD_DMC_PIPE(pipe));
		if (has_dmc) {
			drv_i915_xd_dmc_pipe(dmc, io, pipe, 1);
			report->dmc_pipes |= 1U << pipe;
		}

		/* The DDI the pipe's transcoder drives. */
		trans = i915_xd_fw_transcoder(io, platform, pipe);
		function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(trans));
		drives = i915_xd_fw_function_ddi(platform, function, &ddi);
		if (!drives)
			continue;
		used_ddis |= 1U << ddi;
		port = drv_i915_xd_platform_port(platform, ddi);
		if (port == NULL) {
			unknown = 1;
			continue;
		}

		/* The PLL the port runs on; an unknown one keeps every PLL. */
		pll = i915_xd_fw_pll_of(io, platform, port);
		if (pll == NULL) {
			unknown = 1;
			continue;
		}

		/* Marks the PLL as used. */
		used_plls |= 1U << (unsigned)(pll - platform->plls);
	}

	/* The frame buffer compressor. */
	control = drv_i915_xd_io_read(io, I915_XD_DPFC_CONTROL_A);
	if ((control & I915_XD_DPFC_CONTROL_ENABLE) != 0U) {
		drv_i915_xd_io_write(io, I915_XD_DPFC_CONTROL_A, control & ~I915_XD_DPFC_CONTROL_ENABLE);
		report->fbc_off = 1;
	}

	/* The clocks of the ports no lit pipe drives. */
	for (index = 0U; index < platform->nports; index++) {
		port = &platform->ports[index];
		if ((used_ddis & (1U << port->ddi)) != 0U)
			continue;

		/* A port whose clock is gated already needs nothing. */
		if (port->phy_kind == I915_XD_PHY_COMBO) {
			on = drv_i915_xd_dpll_mapped(io, platform, port->phy, &select);
		} else {
			control = drv_i915_xd_io_read(io, I915_XD_DPCLKA_CFGCR0);
			on = (control & I915_XD_DPCLKA_TC_OFF((unsigned)port->tc - 1U)) == 0U;
		}

		/* A port whose clock is off is passed over. */
		if (!on)
			continue;

		/* Gates the clock of a port no lit pipe uses. */
		i915_xd_fw_clock_off(io, platform, port);
		report->clocks_gated |= 1U << port->ddi;
	}

	/* The PLLs no lit pipe runs on, unless a lit port's PLL is unknown. */
	if (unknown) {
		report->plls_kept = 1;
		return;
	}

	/* Turns off each PLL that is on and that no lit pipe uses. */
	for (index = 0U; index < platform->nplls; index++) {
		if ((used_plls & (1U << index)) != 0U)
			continue;

		/* A PLL that is off needs nothing. */
		pll = &platform->plls[index];
		lit = drv_i915_xd_dpll_read(io, platform, pll, I915_XD_DPLL_AFC_NONE, &words);
		if (!lit)
			continue;

		/* Turns the PLL off. */
		(void)drv_i915_xd_dpll_disable(io, pll);
		report->plls_off |= 1U << index;
	}
}

/*
 * Turns the VGA plane off: the screen off through the legacy sequencer
 * (screen_off, the caller's: 0, or nonzero when the legacy I/O could not
 * be owned), a settle time, then the VGA plane's disable.  Returns 0, 1
 * when the plane was off already, or EIO when the legacy I/O was refused
 * (the plane is left).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_vga.c
 * (intel_vga_disable).
 */
int
drv_i915_xd_vga_off(
	struct i915_xd_io *io,
	int (*screen_off)(void *context),
	void *context)
{
	uint32_t control;
	int error;

	/* A plane that is off already. */
	control = drv_i915_xd_io_read(io, I915_XD_VGA_CONTROL);
	if ((control & I915_XD_VGA_CONTROL_DISABLE) != 0U)
		return 1;

	/* The screen off through the sequencer first, or the I/O access is lost. */
	drv_i915_xd_io_step(io, "xd-takeover vga off");
	error = screen_off(context);
	if (error != 0)
		return EIO;
	(void)drv_i915_xd_io_delay(io, I915_XD_FW_VGA_SETTLE_US);

	/* The plane's disable, flushed. */
	drv_i915_xd_io_write(io, I915_XD_VGA_CONTROL, I915_XD_VGA_CONTROL_DISABLE);
	drv_i915_xd_io_posting_read(io, I915_XD_VGA_CONTROL);

	/* Succeeded: the VGA plane is off. */
	return 0;
}

/*
 * Reads the firmware's display registers, each only while its power well
 * is up.  Nothing is written.
 */
void
drv_i915_xd9_fw_read(
	struct i915_xd_io *io,
	struct i915_xd9_fw_registers *registers)
{
	unsigned transcoder;
	unsigned pipe;

	/* The power wells decide what else can be read. */
	registers->power = drv_i915_xd_io_read(io, I915_XD9_PWR_WELL_CTL2);
	if ((registers->power & I915_XD9_PW1_STATE) == 0U)
		return;

	/* The DC states, the CD clock, the DPLL controls and the compressor, which power well 1 keeps. */
	registers->dc_state = drv_i915_xd_io_read(io, I915_XD9_DC_STATE_EN);
	registers->cdclk = drv_i915_xd_io_read(io, I915_XD9_CDCLK_CTL);
	registers->dpll_ctrl1 = drv_i915_xd_io_read(io, I915_XD9_DPLL_CTRL1);
	registers->dpll_ctrl2 = drv_i915_xd_io_read(io, I915_XD9_DPLL_CTRL2);
	registers->fbc_ctl = drv_i915_xd_io_read(io, I915_XD9_FBC_CTL);

	/* The eDP transcoder and pipe A live in power well 1. */
	i915_xd9_fw_read_transcoder(io, registers, I915_XD9_TRANS_EDP);
	registers->psr_ctl = drv_i915_xd_io_read(io, I915_XD9_PSR_CTL_EDP);
	i915_xd9_fw_read_pipe(io, registers, 0U);

	/* Transcoders A to C and pipes B and C live in power well 2. */
	if ((registers->power & I915_XD9_PW2_STATE) == 0U)
		return;

	/* The transcoders A to C, behind power well 2. */
	for (transcoder = 0U; transcoder < I915_XD9_TRANS_EDP; transcoder++)
		i915_xd9_fw_read_transcoder(io, registers, transcoder);

	/* The pipes B and C, behind power well 2 too. */
	for (pipe = 1U; pipe < I915_XD9_FW_PIPES; pipe++)
		i915_xd9_fw_read_pipe(io, registers, pipe);
}

/*
 * Decodes the firmware's display registers and decides whether the flips
 * can be used.
 *
 * Returns 0 when they can, with every field of the readout filled; else
 * EINVAL with readout->reason naming the first condition that failed and
 * the fields found so far.
 */
int
drv_i915_xd9_fw_decode(
	const struct i915_xd9_fw_registers *registers,
	struct i915_xd9_fw_readout *readout)
{
	unsigned transcoder;
	unsigned pipe;
	unsigned lit_transcoder;
	int error;

	/* Nothing found yet; the refresh assumed until the timing gives it. */
	kern_memset(readout, 0, sizeof(*readout));
	readout->refresh_millihz = I915_XD9_FW_REFRESH_ASSUMED;

	/* Pipe A, the eDP transcoder and the first DDI live in power well 1; without it nothing is lit. */
	if ((registers->power & I915_XD9_PW1_STATE) == 0U) {
		readout->reason = "pw1-off";
		return EINVAL;
	}

	/* A DC state lets the DMC power the display down between frames, which this driver does not manage. */
	if ((registers->dc_state & I915_XD9_DC_STATE_MASK) != 0U) {
		readout->reason = "dc-state";
		return EINVAL;
	}

	/* Counts the pipes a running transcoder feeds and whose first plane is on, keeping the last one. */
	lit_transcoder = 0U;
	for (transcoder = 0U; transcoder < I915_XD9_FW_TRANSCODERS; transcoder++) {
		error = i915_xd9_fw_transcoder_pipe(registers, transcoder, &pipe);
		if (error != 0)
			continue;

		/* A pipe whose registers were not read, or whose first plane is off, shows nothing of the firmware's. */
		if (!registers->pipe_read[pipe])
			continue;
		if ((registers->plane_ctl[pipe] & I915_XD9_PLANE_CTL_ENABLE) == 0U)
			continue;

		/* A lit pipe: counted, and remembered with its transcoder. */
		readout->lit_pipes++;
		readout->pipe = pipe;
		lit_transcoder = transcoder;
	}

	/* The flips take over exactly the one output the firmware shows. */
	if (readout->lit_pipes == 0U) {
		readout->reason = "no-lit-pipe";
		return EINVAL;
	}

	/* Several lit pipes: which one is the panel is not known. */
	if (readout->lit_pipes > 1U) {
		readout->reason = "several-lit-pipes";
		return EINVAL;
	}

	/* The one lit pipe's transcoder. */
	readout->transcoder = lit_transcoder;

	/* The plane must scan out a surface the driver's buffers can stand in for. */
	error = i915_xd9_fw_decode_plane(registers, readout);
	if (error != 0)
		return error;

	/* A compressor caching the firmware's surface would need the invalidations this driver does not make. */
	if ((registers->fbc_ctl & I915_XD9_FBC_ENABLE) != 0U) {
		readout->reason = "fbc";
		return EINVAL;
	}

	/* A self-refreshing eDP panel would not see the flips without the PSR exits this driver does not make. */
	if (lit_transcoder == I915_XD9_TRANS_EDP && (registers->psr_ctl & I915_XD9_PSR_ENABLE) != 0U) {
		readout->reason = "psr";
		return EINVAL;
	}

	/* The refresh the display reports, measured from the timing when it can be. */
	i915_xd9_fw_decode_refresh(registers, readout);

	/* Succeeded: the flips can stand in for the firmware's surface. */
	return 0;
}

/* Logs the registers read and what the readout made of them. */
void
drv_i915_xd9_fw_log(
	const struct i915_xd9_fw_registers *registers,
	const struct i915_xd9_fw_readout *readout,
	const char *fb_match)
{
	unsigned pipe;
	unsigned transcoder;

	/* The raw registers, for the photograph of a failure. */
	kern_logf("i915: xd9-firmware: PWR_WELL_CTL2=0x%08x DC_STATE_EN=0x%08x CDCLK_CTL=0x%08x DPLL_CTRL1=0x%08x DPLL_CTRL2=0x%08x PSR_CTL_EDP=0x%08x DPFC_CONTROL=0x%08x\n",
	    registers->power,
	    registers->dc_state,
	    registers->cdclk,
	    registers->dpll_ctrl1,
	    registers->dpll_ctrl2,
	    registers->psr_ctl,
	    registers->fbc_ctl);

	/* Every transcoder that was read. */
	for (transcoder = 0U; transcoder < I915_XD9_FW_TRANSCODERS; transcoder++) {
		if (!registers->transcoder_read[transcoder])
			continue;

		/* The transcoder's registers. */
		kern_logf("i915: xd9-firmware: transcoder %s DDI_FUNC_CTL=0x%08x TRANSCONF=0x%08x HTOTAL=0x%08x VTOTAL=0x%08x LINK_M1=0x%08x LINK_N1=0x%08x\n",
		    i915_xd9_fw_transcoder_name(transcoder),
		    registers->ddi_func[transcoder],
		    registers->transconf[transcoder],
		    registers->htotal[transcoder],
		    registers->vtotal[transcoder],
		    registers->link_m[transcoder],
		    registers->link_n[transcoder]);
	}

	/* Every pipe that was read. */
	for (pipe = 0U; pipe < I915_XD9_FW_PIPES; pipe++) {
		if (!registers->pipe_read[pipe])
			continue;

		/* The pipe's registers. */
		kern_logf("i915: xd9-firmware: pipe %c PIPESRC=0x%08x PLANE_CTL=0x%08x STRIDE=0x%08x POS=0x%08x SIZE=0x%08x SURF=0x%08x OFFSET=0x%08x SURFLIVE=0x%08x PS=0x%08x/0x%08x\n",
		    'A' + (int)pipe,
		    registers->pipe_src[pipe],
		    registers->plane_ctl[pipe],
		    registers->plane_stride[pipe],
		    registers->plane_pos[pipe],
		    registers->plane_size[pipe],
		    registers->plane_surf[pipe],
		    registers->plane_offset[pipe],
		    registers->plane_surflive[pipe],
		    registers->scaler[pipe][0],
		    registers->scaler[pipe][1]);
	}

	/* What the readout found: a pipe lit by the firmware, or none. */
	if (readout->lit_pipes == 0U) {
		kern_logf("i915: xd9-firmware: readout: no pipe lit by the firmware\n");
		return;
	}

	/* What the decode made of them. */
	kern_logf("i915: xd9-firmware: readout pipe %c transcoder %s %ux%u stride %u %s surf 0x%08x refresh %u mHz (%s, pixel %u kHz) scaler %s fb=%s\n",
	    'A' + (int)readout->pipe,
	    i915_xd9_fw_transcoder_name(readout->transcoder),
	    readout->width,
	    readout->height,
	    readout->stride_bytes,
	    readout->rgbx ? "XBGR8888" : "XRGB8888",
	    readout->surf,
	    readout->refresh_millihz,
	    readout->refresh_measured ? "measured" : "assumed",
	    readout->pixel_khz,
	    readout->scaled ? "on" : "off",
	    fb_match);
}

/*
 * Reports the pixel clock in kHz of a DP link: link M times the port
 * clock over link N, rounded up, or zero when N is zero.
 */
uint32_t
drv_i915_xd9_dp_pixel_khz(
	uint32_t link_m,
	uint32_t link_n,
	uint32_t port_khz)
{
	uint64_t product;
	uint64_t pixel;

	/* A link without an N carries no pixels the driver can count. */
	if (link_n == 0U)
		return 0U;

	/* The data rate scaled by M over N, rounded up as the reference does. */
	product = (uint64_t)link_m * (uint64_t)port_khz;
	pixel = (product + (uint64_t)link_n - 1U) / (uint64_t)link_n;

	/* A clock beyond 32 bits is not a timing the plane runs. */
	if (pixel > 0xffffffffULL)
		return 0U;

	/* Succeeded: the pixel clock. */
	return (uint32_t)pixel;
}

/*
 * Reads whether a display 9 output the readout found still runs (after a
 * sleep): power well 1, its transcoder's state and its first plane's
 * enable, each reported as read.  Nothing is written.  Returns nonzero
 * when all three are on.
 */
int
drv_i915_xd9_fw_runs(
	struct i915_xd_io *io,
	const struct i915_xd9_fw_readout *readout,
	uint32_t *well_control,
	uint32_t *transconf,
	uint32_t *plane_ctl)
{
	/* The three words. */
	*well_control = drv_i915_xd_io_read(io, I915_XD9_PWR_WELL_CTL2);
	*transconf = drv_i915_xd_io_read(io, I915_XD9_TRANSCONF(readout->transcoder));
	*plane_ctl = drv_i915_xd_io_read(io, I915_XD9_PLANE_CTL(readout->pipe));

	/* Something the sleep turned off. */
	if ((*well_control & I915_XD9_PW1_STATE) == 0U || (*transconf & I915_XD9_TRANSCONF_STATE) == 0U)
		return 0;
	if ((*plane_ctl & I915_XD9_PLANE_CTL_ENABLE) == 0U)
		return 0;

	/* Succeeded: the output runs. */
	return 1;
}

/*
 * Checks what the firmware scans out on a display 9 device before the GT
 * starts, and reads its GPU's VT-d unit: every powered pipe's first plane
 * is read and logged, and a picture that reaches into the GGTT pages the
 * driver writes is recorded.  Nothing is written.  Returns 0, EIO when a
 * VT-d unit the machine owns would translate or block the GT's DMA (or
 * cannot be read), or EBUSY when a picture reaches the driver's pages.
 */
int
drv_i915_xd9_fw_check(
	struct i915_xd_io *io,
	const struct i915_xd_fw_env *env,
	struct i915_xd9_fw_check *check)
{
	uint64_t driver_start;
	uint32_t wanted;
	unsigned pipe;

	/* The power wells, and where the driver's GGTT pages begin. */
	kern_memset(check, 0, sizeof(*check));
	check->well_control = drv_i915_xd_io_read(io, I915_XD9_PWR_WELL_CTL2);
	driver_start = (uint64_t)env->driver_first_page << I915_XD_FW_PAGE_SHIFT;
	kern_logf("i915: xd9-firmware: PWR_WELL_CTL2=0x%08x driver GGTT from 0x%llx\n", check->well_control, (unsigned long long)driver_start);

	/* The eDP transcoder a laptop's firmware drives its panel with, in power well 1. */
	if ((check->well_control & I915_XD9_PW1_STATE) != 0U) {
		check->edp_transconf = drv_i915_xd_io_read(io, I915_XD9_TRANSCONF(I915_XD9_TRANS_EDP));
		kern_logf("i915: xd9-firmware: TRANSCONF_EDP=0x%08x\n", check->edp_transconf);
	}

	/* The VT-d unit and its walls. */
	check->vtd.hypervisor = env->hypervisor;
	i915_xd_fw_read_vtd(io, env, &check->vtd);
	i915_xd_fw_vtd_walls(&check->vtd);
	kern_logf("i915: xd9-firmware: GFXVTBAR=0x%llx enabled=%d %s readable=%d GSTS=0x%08x PMEN=0x%08x\n",
	    (unsigned long long)check->vtd.vtd_bar,
	    check->vtd.vtd_enabled,
	    check->vtd.hypervisor ? "guest" : "native",
	    check->vtd.vtd_readable,
	    check->vtd.vtd_status,
	    check->vtd.vtd_protect);

	/* Every pipe whose wells are up: A needs power well 1, B and C power well 2 as well. */
	for (pipe = 0U; pipe < I915_XD9_FW_PIPES; pipe++) {
		wanted = I915_XD9_PW1_STATE;
		if (pipe != 0U)
			wanted |= I915_XD9_PW2_STATE;
		if ((check->well_control & wanted) != wanted)
			continue;
		i915_xd9_fw_check_plane(io, pipe, driver_start, check);
	}

	/* The firmware's picture stays on either way. */
	kern_logf("i915: xd9-firmware: the firmware's picture is kept; the GPU node has no display\n");

	/* A VT-d unit that would translate or block the GT's DMA. */
	if ((check->vtd.walls & (I915_XD_FW_WALL_VTD_HIDDEN | I915_XD_FW_WALL_VTD_TRANSLATES | I915_XD_FW_WALL_VTD_PROTECTS)) != 0U) {
		kern_logf("i915: xd9-firmware: the GPU's VT-d unit translates or protects memory and zedBSD has no IOMMU driver: the GT is not started\n");
		return EIO;
	}

	/* A picture the GT would overwrite. */
	if (check->overlap_pipes != 0U)
		return EBUSY;

	/* Succeeded: the pictures are clear of the driver's pages and nothing translates the GT's DMA. */
	return 0;
}

/*
 * Finds the transcoder that feeds a pipe: display 11's eDP transcoder
 * when it is on and its input is the pipe, otherwise the pipe's own.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_DDI_EDP_INPUT_A_ON, TRANS_DDI_EDP_INPUT_A_ONOFF,
 * TRANS_DDI_EDP_INPUT_B_ONOFF, TRANS_DDI_EDP_INPUT_C_ONOFF).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (hsw_enabled_transcoders: an input it does not know is pipe A's).
 */
static unsigned
i915_xd_fw_transcoder(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe)
{
	uint32_t function;
	unsigned input;
	unsigned fed;

	/* From display 12 a pipe's transcoder is its own. */
	if (platform->display_ver >= 12U)
		return pipe;

	/* Display 11's eDP transcoder, when its function is on. */
	function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(I915_XD_TRANS_EDP));
	if ((function & I915_XD_DDI_FUNC_ENABLE) == 0U)
		return pipe;

	/* The pipe its input names: B, C, or else A. */
	input = i915_xd_field_get(I915_XD_DDI_FUNC_EDP_INPUT, function);
	fed = 0U;
	if (input == I915_XD_DDI_FUNC_EDP_INPUT_B_ONOFF)
		fed = 1U;
	if (input == I915_XD_DDI_FUNC_EDP_INPUT_C_ONOFF)
		fed = 2U;
	if (fed != pipe)
		return pipe;

	/* Succeeded: the eDP transcoder feeds the pipe. */
	return I915_XD_TRANS_EDP;
}

/*
 * Takes the DDI a transcoder's DDI function drives out of its word.
 * Display 11 names the DDI itself (DDI A is 0), so only an enabled
 * function drives one; later displays name the DDI's index plus one, and
 * 0 is none.  Without a description the later field is read.  Returns 1
 * with the DDI's index, or 0 for none.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_DDI_PORT_MASK, TGL_TRANS_DDI_PORT_MASK).
 */
static int
i915_xd_fw_function_ddi(
	const struct i915_xd_platform *platform,
	uint32_t function,
	unsigned *ddi)
{
	unsigned select;

	/* Display 11: the DDI of an enabled function. */
	if (platform != NULL && platform->display_ver < 12U) {
		if ((function & I915_XD_DDI_FUNC_ENABLE) == 0U)
			return 0;
		*ddi = i915_xd_field_get(I915_XD_DDI_FUNC_SELECT_11, function);
		return 1;
	}

	/* Later displays: the index plus one, 0 for none. */
	select = (function & I915_XD_DDI_FUNC_SELECT) >> I915_XD_FW_DDI_SELECT_SHIFT;
	if (select == 0U)
		return 0;
	*ddi = select - 1U;

	/* Succeeded: the DDI. */
	return 1;
}

/*
 * Tells whether every well of a pipe and its transcoder is on.  Display
 * 11's eDP transcoder is in no switched well: only the pipe's are looked
 * at.
 */
static int
i915_xd_fw_pipe_powered(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe,
	unsigned trans)
{
	uint64_t powers;
	uint64_t wells;
	unsigned well;
	int powered;

	/* The wells of the pipe and of its transcoder. */
	powers = I915_XD_POWERS_PIPE(pipe);
	if (trans != I915_XD_TRANS_EDP)
		powers |= I915_XD_POWERS_TRANS(trans);
	wells = drv_i915_xd_power_wells_of(platform, powers);

	/* Each must be on. */
	for (well = 0U; well < platform->nwells; well++) {
		if ((wells & (1ULL << well)) == 0ULL)
			continue;

		/* A well that is not powered fails the check. */
		powered = drv_i915_xd_power_well_powered(io, platform, well);
		if (!powered)
			return 0;
	}

	/* Succeeded: the pipe's registers read. */
	return 1;
}

/* Reads one powered pipe: the transcoder that feeds it, its DDI function, the source size and the primary plane. */
static void
i915_xd_fw_read_pipe(
	struct i915_xd_io *io,
	unsigned pipe,
	unsigned trans,
	struct i915_xd_firmware *firmware)
{
	struct i915_xd_fw_pipe *record;

	/* The transcoder, its DDI function and the pipe's source size. */
	record = &firmware->pipe[pipe];
	record->trans_conf = drv_i915_xd_io_read(io, I915_XD_TRANS_CONF(trans));
	record->ddi_function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(trans));
	record->source_size = drv_i915_xd_io_read(io, I915_XD_PIPE_SRCSZ(pipe));

	/* The primary plane. */
	record->plane_ctl = drv_i915_xd_io_read(io, I915_XD_PLANE_CTL(pipe));
	record->plane_stride = drv_i915_xd_io_read(io, I915_XD_PLANE_STRIDE(pipe));
	record->plane_size = drv_i915_xd_io_read(io, I915_XD_PLANE_SIZE(pipe));
	record->plane_surface = drv_i915_xd_io_read(io, I915_XD_PLANE_SURF(pipe));

	/* A transcoder or plane word of all ones is not a register value. */
	if (record->trans_conf == I915_XD_FW_ALL_ONES || record->plane_ctl == I915_XD_FW_ALL_ONES) {
		record->state = I915_XD_FW_PIPE_GARBLED;
		return;
	}

	/* An enabled transcoder or plane makes the pipe lit. */
	record->state = I915_XD_FW_PIPE_DARK;
	if ((record->trans_conf & I915_XD_TRANS_CONF_ENABLE) != 0U || (record->plane_ctl & I915_XD_PLANE_CTL_ENABLE) != 0U)
		record->state = I915_XD_FW_PIPE_LIT;
}

/* Reads pipe A's domain: the first two combo PLLs, the DDI A buffer, the panel, its PWM and the clock map. */
static void
i915_xd_fw_read_pipe_a_domain(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_firmware *firmware)
{
	unsigned index;
	unsigned found;

	/* The enables of the first two combo PLLs. */
	found = 0U;
	for (index = 0U; index < platform->nplls; index++) {
		if (found == 2U)
			break;
		if (platform->plls[index].kind != I915_XD_PLL_COMBO)
			continue;

		/* Keeps the PLL's enable word. */
		firmware->pll_enable[found] = drv_i915_xd_io_read(io, platform->plls[index].enable);
		found++;
	}

	/* The DDI A buffer, the panel's power sequencer and its backlight PWM. */
	firmware->ddi_a_buffer = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(0U));
	firmware->panel_status = drv_i915_xd_io_read(io, I915_XD_PP_STATUS(0U));
	firmware->panel_control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(0U));
	firmware->pwm_control = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_CTL(0U));
	firmware->pwm_duty = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_DUTY(0U));

	/* The DDI clock map. */
	firmware->clock_map = drv_i915_xd_io_read(io, I915_XD_DPCLKA_CFGCR0);
}

/* Records the firmware's framebuffer and the GGTT pages it takes in the aperture. */
static void
i915_xd_fw_read_fb(
	const struct i915_xd_fw_env *env,
	struct i915_xd_firmware *firmware)
{
	uint64_t aperture_end;

	/* No framebuffer: nothing to record. */
	if (!env->fb_present)
		return;
	if (env->fb_size == 0U)
		return;

	/* The framebuffer. */
	firmware->fb_present = 1;
	firmware->fb_base = env->fb_base;
	firmware->fb_size = env->fb_size;

	/* One inside the aperture takes GGTT pages. */
	aperture_end = env->aperture_base + env->aperture_size;
	if (env->fb_base < env->aperture_base)
		return;
	if (env->fb_base + env->fb_size > aperture_end)
		return;
	firmware->fb_in_aperture = 1;
	firmware->fb_first_page = (uint32_t)((env->fb_base - env->aperture_base) >> I915_XD_FW_PAGE_SHIFT);
	firmware->fb_pages = (uint32_t)((env->fb_size + I915_XD_FW_PAGE_SIZE - 1U) >> I915_XD_FW_PAGE_SHIFT);
}

/* Reads the GPU's VT-d unit through the MCHBAR mirror and the environment's reader. */
static void
i915_xd_fw_read_vtd(
	struct i915_xd_io *io,
	const struct i915_xd_fw_env *env,
	struct i915_xd_firmware *firmware)
{
	uint32_t low;
	uint32_t high;
	uint64_t base;
	int error;

	/* The unit's base, low half first. */
	low = drv_i915_xd_io_read(io, I915_XD_FW_GFXVTBAR);
	high = drv_i915_xd_io_read(io, I915_XD_FW_GFXVTBAR + 4U);
	firmware->vtd_bar = (uint64_t)low | (uint64_t)high << 32;

	/* A disabled unit, or one at address 0, is not read. */
	firmware->vtd_enabled = (int)(firmware->vtd_bar & I915_XD_FW_GFXVTBAR_ENABLE);
	base = firmware->vtd_bar & I915_XD_FW_GFXVTBAR_BASE;
	if (!firmware->vtd_enabled)
		return;
	if (base == 0U)
		return;
	if (env->vtd_read == NULL)
		return;

	/* The version, the global status and the protected memory enable. */
	error = env->vtd_read(env->context, base, &firmware->vtd_version, &firmware->vtd_status, &firmware->vtd_protect);
	if (error != 0)
		return;

	/* A real unit reports a version that is neither zero nor all ones (a guest may see nothing there). */
	if (firmware->vtd_version != 0U && firmware->vtd_version != I915_XD_FW_ALL_ONES)
		firmware->vtd_readable = 1;
}

/* Records the walls of a VT-d unit the machine owns (a guest's unit is the host's). */
static void
i915_xd_fw_vtd_walls(
	struct i915_xd_firmware *firmware)
{
	/* A guest, or no unit: nothing to record. */
	if (firmware->hypervisor)
		return;
	if (!firmware->vtd_enabled)
		return;

	/* A unit that does not read. */
	if (!firmware->vtd_readable) {
		firmware->walls |= I915_XD_FW_WALL_VTD_HIDDEN;
		return;
	}

	/* Translation, a protected memory region, interrupt remapping. */
	if ((firmware->vtd_status & I915_XD_FW_VTD_TRANSLATES) != 0U)
		firmware->walls |= I915_XD_FW_WALL_VTD_TRANSLATES;
	if ((firmware->vtd_protect & I915_XD_FW_VTD_PROTECTS) != 0U)
		firmware->walls |= I915_XD_FW_WALL_VTD_PROTECTS;
	if ((firmware->vtd_status & I915_XD_FW_VTD_REMAPS) != 0U)
		firmware->walls |= I915_XD_FW_WALL_VTD_REMAPS;
}

/* Takes the first stop in order, or uses the display (and takes a lit one over). */
static void
i915_xd_fw_choose_stop(
	struct i915_xd_firmware *firmware)
{
	unsigned vtd_dma;

	/* The order of the stops. */
	firmware->use = 0;
	firmware->take_over = 0;
	firmware->stop = 0U;
	vtd_dma = firmware->walls & (I915_XD_FW_WALL_VTD_TRANSLATES | I915_XD_FW_WALL_VTD_PROTECTS);
	if ((firmware->walls & I915_XD_FW_WALL_UNDESCRIBED) != 0U) {
		firmware->stop = I915_XD_FW_WALL_UNDESCRIBED;
		firmware->reason = "the device has no description: the display is not known";
	} else if ((firmware->walls & I915_XD_FW_WALL_GARBLED) != 0U) {
		firmware->stop = I915_XD_FW_WALL_GARBLED;
		firmware->reason = "a pipe did not read as register values";
	} else if ((firmware->walls & I915_XD_FW_WALL_OVERLAP) != 0U) {
		firmware->stop = I915_XD_FW_WALL_OVERLAP;
		firmware->reason = "the firmware's scanout overlaps the GGTT pages the driver writes";
	} else if ((firmware->walls & I915_XD_FW_WALL_VTD_HIDDEN) != 0U) {
		firmware->stop = I915_XD_FW_WALL_VTD_HIDDEN;
		firmware->reason = "the GPU's VT-d unit is on but does not read";
	} else if (vtd_dma != 0U) {
		firmware->stop = vtd_dma;
		firmware->reason = "the GPU's VT-d unit translates or protects memory, and there is no IOMMU driver";
	} else if (firmware->hypervisor) {
		firmware->use = 1;
		firmware->reason = "no wall (a guest: the host owns the VT-d unit)";
	} else {
		firmware->use = 1;
		firmware->reason = "no wall (DMA is not translated)";
	}

	/* A lit display is taken over before the first write. */
	if (firmware->use && (firmware->walls & I915_XD_FW_WALL_LIT) != 0U) {
		firmware->take_over = 1;
		firmware->reason = "a pipe is lit: the driver takes the firmware's output over before its first write";
	}
}

/* Turns the pipe's first plane off when it is on, and waits for the vertical blank that applies it; returns the late bits. */
static unsigned
i915_xd_fw_planes_off(
	struct i915_xd_io *io,
	unsigned pipe)
{
	uint32_t control;
	uint32_t frame;
	uint32_t now;
	unsigned waited;

	/* A plane that is off already is left. */
	control = drv_i915_xd_io_read(io, I915_XD_PLANE_CTL(pipe));
	if ((control & I915_XD_PLANE_CTL_ENABLE) == 0U)
		return 0U;

	/* The plane off, armed. */
	frame = drv_i915_xd_pipe_frame(io, pipe);
	drv_i915_xd_plane_off_noarm(io, pipe);
	drv_i915_xd_plane_off_arm(io, pipe);

	/* The next vertical blank. */
	for (waited = 0U; waited < I915_XD_FW_VBLANK_MS; waited++) {
		now = drv_i915_xd_pipe_frame(io, pipe);
		if (now != frame)
			return 0U;

		/* Waits a millisecond before reading again. */
		(void)drv_i915_xd_io_delay(io, 1000U);
	}

	/* Succeeded: the plane is off, though no vertical blank was seen. */
	return I915_XD_TAKEOVER_LATE_VBLANK;
}

/* Finds the PLL a port's clock is selected from: by the DDI clock map of a combo PHY, by the DDI clock select of a Type-C one; NULL for none. */
static const struct i915_xd_pll_desc *
i915_xd_fw_pll_of(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port)
{
	unsigned select;
	unsigned index;
	unsigned kind;
	int ungated;
	const struct i915_xd_pll_desc *pll;

	/* A combo PHY: the select in the clock map, when its clock is ungated. */
	if (port->phy_kind == I915_XD_PHY_COMBO) {
		ungated = drv_i915_xd_dpll_mapped(io, platform, port->phy, &select);
		if (!ungated)
			return NULL;

		/* The PLL the clock select names. */
		pll = drv_i915_xd_dpll_desc(platform, select);

		/* Succeeded: the PLL, or NULL for a select the platform does not have. */
		return pll;
	}

	/* A Type-C PHY: the port's own PLL, the Thunderbolt PLL, or none. */
	select = (drv_i915_xd_io_read(io, I915_XD_DDI_CLK_SEL(port->ddi)) & I915_XD_DDI_CLK_SEL_FIELD) >> I915_XD_FW_CLK_SEL_SHIFT;
	if (select == I915_XD_DDI_CLK_SEL_TC_PLL) {
		kind = I915_XD_PLL_TC;
	} else if (select >= I915_XD_DDI_CLK_SEL_TBT_162) {
		kind = I915_XD_PLL_TBT;
	} else {
		return NULL;
	}

	/* The PLL of that kind (and of the port, for a Type-C PLL). */
	for (index = 0U; index < platform->nplls; index++) {
		if (platform->plls[index].kind != kind)
			continue;
		if (kind == I915_XD_PLL_TC && platform->plls[index].tc != port->tc)
			continue;

		/* Succeeded: the port's PLL. */
		return &platform->plls[index];
	}

	/* Succeeded: the description has no such PLL. */
	return NULL;
}

/* Tells whether a lit pipe other than this one still runs on a PLL. */
static int
i915_xd_fw_pll_shared(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_firmware *firmware,
	unsigned pipe,
	const struct i915_xd_pll_desc *pll)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_pll_desc *other;
	uint32_t function;
	unsigned trans;
	unsigned index;
	unsigned ddi;
	int drives;

	/* Each other lit pipe's DDI and its PLL. */
	for (index = 0U; index < I915_XD_FW_PIPES; index++) {
		if (index == pipe)
			continue;
		if ((firmware->lit_pipes & (1U << index)) == 0U)
			continue;

		/* The DDI the pipe's transcoder drives. */
		trans = i915_xd_fw_transcoder(io, platform, index);
		function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(trans));
		drives = i915_xd_fw_function_ddi(platform, function, &ddi);
		if (!drives)
			continue;
		port = drv_i915_xd_platform_port(platform, ddi);
		if (port == NULL)
			continue;
		other = i915_xd_fw_pll_of(io, platform, port);
		if (other == pll)
			return 1;
	}

	/* Succeeded: no other lit pipe uses it. */
	return 0;
}

/* Gates a port's DDI clock: the clock-off bit of a combo PHY; a Type-C port's bit and its clock select. */
static void
i915_xd_fw_clock_off(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_port_desc *port)
{
	/* A combo PHY's clock-off bit. */
	if (port->phy_kind == I915_XD_PHY_COMBO) {
		drv_i915_xd_dpll_unmap(io, platform, port->phy);
		return;
	}

	/* A Type-C port's clock-off bit, then no PLL selected. */
	drv_i915_xd_tc_clock_unmap(io, port->ddi, (unsigned)port->tc - 1U);
}

/* Calls one of the caller's hooks when it has one. */
static void
i915_xd_fw_hook(
	void (*hook)(void *context, unsigned pipe),
	void *context,
	unsigned pipe)
{
	/* A hook the caller does not have. */
	if (hook == NULL)
		return;

	/* The caller's part. */
	hook(context, pipe);
}

/* Reads one transcoder's DDI function, configuration, totals and DisplayPort link M and N. */
static void
i915_xd9_fw_read_transcoder(
	struct i915_xd_io *io,
	struct i915_xd9_fw_registers *registers,
	unsigned transcoder)
{
	/* The words, at the transcoder's place (the eDP transcoder's own). */
	registers->ddi_func[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANS_DDI_FUNC(transcoder));
	registers->transconf[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANSCONF(transcoder));
	registers->htotal[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANS_HTOTAL(transcoder));
	registers->vtotal[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANS_VTOTAL(transcoder));
	registers->link_m[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANS_LINK_M1(transcoder));
	registers->link_n[transcoder] = drv_i915_xd_io_read(io, I915_XD9_TRANS_LINK_N1(transcoder));

	/* Succeeded: the transcoder was read. */
	registers->transcoder_read[transcoder] = 1;
}

/* Reads one pipe's source size, first plane and scalers. */
static void
i915_xd9_fw_read_pipe(
	struct i915_xd_io *io,
	struct i915_xd9_fw_registers *registers,
	unsigned pipe)
{
	/* The pipe's source size. */
	registers->pipe_src[pipe] = drv_i915_xd_io_read(io, I915_XD9_PIPESRC(pipe));

	/* The first plane's control, layout and surfaces. */
	registers->plane_ctl[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_CTL(pipe));
	registers->plane_stride[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_STRIDE(pipe));
	registers->plane_pos[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_POS(pipe));
	registers->plane_size[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_SIZE(pipe));
	registers->plane_surf[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_SURF(pipe));
	registers->plane_offset[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_OFFSET(pipe));
	registers->plane_surflive[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_SURFLIVE(pipe));

	/* The pipe's two scalers. */
	registers->scaler[pipe][0] = drv_i915_xd_io_read(io, I915_XD9_PS_CTRL(pipe, 0U));
	registers->scaler[pipe][1] = drv_i915_xd_io_read(io, I915_XD9_PS_CTRL(pipe, 1U));

	/* Succeeded: the pipe was read. */
	registers->pipe_read[pipe] = 1;
}

/*
 * Decodes the lit pipe's first plane into the readout and checks that the
 * driver's buffers can stand in for its surface.
 *
 * Returns 0, or EINVAL with the reason.
 */
static int
i915_xd9_fw_decode_plane(
	const struct i915_xd9_fw_registers *registers,
	struct i915_xd9_fw_readout *readout)
{
	uint32_t control;
	uint32_t format;
	uint32_t tiling;
	uint32_t live;
	unsigned pipe;

	/* The plane of the lit pipe. */
	pipe = readout->pipe;
	control = registers->plane_ctl[pipe];

	/* The plane's extent, row bytes, byte order and surface, as found. */
	readout->width = (registers->plane_size[pipe] & I915_XD9_PLANE_SIZE_FIELD_MASK) + 1U;
	readout->height = ((registers->plane_size[pipe] >> I915_XD9_PLANE_SIZE_HEIGHT_SHIFT) & I915_XD9_PLANE_SIZE_FIELD_MASK) + 1U;
	readout->stride_bytes = (registers->plane_stride[pipe] & I915_XD9_PLANE_STRIDE_MASK) * I915_XD9_PLANE_STRIDE_UNIT;
	readout->rgbx = 0;
	if ((control & I915_XD9_PLANE_CTL_RGBX) != 0U)
		readout->rgbx = 1;
	readout->surf = registers->plane_surf[pipe] & I915_XD9_PLANE_SURF_ADDRESS;

	/* A scaler on the pipe stretches the plane to the panel; the flips keep it as the firmware set it. */
	if ((registers->scaler[pipe][0] & I915_XD9_PS_ENABLE) != 0U)
		readout->scaled = 1;
	if ((registers->scaler[pipe][1] & I915_XD9_PS_ENABLE) != 0U)
		readout->scaled = 1;

	/* The buffers are 32-bit RGB of either order. */
	format = (control >> I915_XD9_PLANE_CTL_FORMAT_SHIFT) & I915_XD9_PLANE_CTL_FORMAT_MASK;
	if (format != I915_XD9_PLANE_FORMAT_XRGB_8888) {
		readout->reason = "format";
		return EINVAL;
	}

	/* The buffers are linear. */
	tiling = (control >> I915_XD9_PLANE_CTL_TILED_SHIFT) & I915_XD9_PLANE_CTL_TILED_MASK;
	if (tiling != I915_XD9_PLANE_TILED_LINEAR) {
		readout->reason = "tiled";
		return EINVAL;
	}

	/* The frames are drawn upright. */
	if ((control & I915_XD9_PLANE_CTL_ROTATE_MASK) != 0U) {
		readout->reason = "rotated";
		return EINVAL;
	}

	/* An asynchronous flip would not wait for the vertical blank. */
	if ((control & I915_XD9_PLANE_CTL_ASYNC_FLIP) != 0U) {
		readout->reason = "async";
		return EINVAL;
	}

	/* The plane reads each buffer from its first pixel. */
	if (registers->plane_offset[pipe] != 0U) {
		readout->reason = "offset";
		return EINVAL;
	}

	/* The buffers take a size the scanout buffers can have, and the rows the plane reads fit its stride. */
	if (readout->width > I915_XD9_FW_MAX_SIDE || readout->height > I915_XD9_FW_MAX_SIDE) {
		readout->reason = "size";
		return EINVAL;
	}

	/* Rows at least as long as the plane is wide. */
	if (readout->stride_bytes < readout->width * 4U) {
		readout->reason = "stride";
		return EINVAL;
	}

	/* A linear surface of display 9 starts on a 256 KiB boundary. */
	if ((readout->surf % I915_XD9_LINEAR_ALIGNMENT) != 0U) {
		readout->reason = "surf-align";
		return EINVAL;
	}

	/* A flip of the firmware's own that has not latched would race the first of the driver's. */
	live = registers->plane_surflive[pipe] & I915_XD9_PLANE_SURF_ADDRESS;
	if (live != readout->surf) {
		readout->reason = "surf-pending";
		return EINVAL;
	}

	/* Succeeded: the plane can be pointed at the driver's buffers. */
	return 0;
}

/*
 * Measures the refresh of the lit transcoder from its DP timing: the pixel
 * clock from the link M and N and the DPLL's port clock, over the totals.
 *
 * A transcoder that is not DP, a DDI clock not chosen by DPLL_CTRL2, an
 * HDMI-mode DPLL or a refresh out of range leaves the assumed 60 Hz.
 */
static void
i915_xd9_fw_decode_refresh(
	const struct i915_xd9_fw_registers *registers,
	struct i915_xd9_fw_readout *readout)
{
	unsigned transcoder;
	uint32_t ddi_func;
	uint32_t mode;
	uint32_t port;
	uint32_t dpll;
	uint32_t rate;
	uint32_t port_khz;
	uint32_t htotal;
	uint32_t vtotal;
	uint64_t millihz;

	/* The transcoder and its DDI function. */
	transcoder = readout->transcoder;
	ddi_func = registers->ddi_func[transcoder];

	/* Only a DP transcoder's clock follows from its link M and N. */
	mode = (ddi_func >> I915_XD9_DDI_FUNC_MODE_SHIFT) & I915_XD9_DDI_FUNC_MODE_MASK;
	if (mode != I915_XD9_DDI_FUNC_MODE_DP_SST && mode != I915_XD9_DDI_FUNC_MODE_DP_MST)
		return;

	/* The DDI port's clock comes from the DPLL DPLL_CTRL2 selects, when it overrides the default. */
	port = (ddi_func >> I915_XD9_DDI_FUNC_DDI_SHIFT) & I915_XD9_FW_DDI_MASK;
	if ((registers->dpll_ctrl2 & (1U << (port * I915_XD9_DPLL_CTRL2_BITS))) == 0U)
		return;

	/* The DPLL the port takes its clock from. */
	dpll = (registers->dpll_ctrl2 >> (port * I915_XD9_DPLL_CTRL2_BITS + I915_XD9_DPLL_CTRL2_SELECT_SHIFT)) & I915_XD9_DPLL_CTRL2_SELECT_MASK;

	/* An HDMI-mode DPLL has no fixed link rate. */
	if ((registers->dpll_ctrl1 & (I915_XD9_DPLL_CTRL1_HDMI << (dpll * I915_XD9_DPLL_CTRL1_BITS))) != 0U)
		return;

	/* The DPLL's link rate gives the port clock. */
	rate = (registers->dpll_ctrl1 >> (dpll * I915_XD9_DPLL_CTRL1_BITS + I915_XD9_DPLL_CTRL1_RATE_SHIFT)) & I915_XD9_DPLL_CTRL1_RATE_MASK;
	port_khz = drv_i915_xd9_dpll_rate_port_khz(rate);
	if (port_khz == 0U)
		return;

	/* The port and the DPLL, for the external outputs (xd9-outputs.c). */
	readout->port = port;
	readout->dpll = dpll;

	/* The pixel clock the link carries. */
	readout->pixel_khz = drv_i915_xd9_dp_pixel_khz(registers->link_m[transcoder] & I915_XD9_LINK_MN_MASK, registers->link_n[transcoder] & I915_XD9_LINK_MN_MASK, port_khz);
	if (readout->pixel_khz == 0U)
		return;

	/* The frame's totals. */
	htotal = ((registers->htotal[transcoder] >> I915_XD9_TIMING_TOTAL_SHIFT) & I915_XD9_TIMING_FIELD_MASK) + 1U;
	readout->htotal = htotal;
	vtotal = ((registers->vtotal[transcoder] >> I915_XD9_TIMING_TOTAL_SHIFT) & I915_XD9_TIMING_FIELD_MASK) + 1U;

	/* The frames a second, in millihertz, rounded to the nearest. */
	millihz = ((uint64_t)readout->pixel_khz * 1000000U + ((uint64_t)htotal * vtotal) / 2U) / ((uint64_t)htotal * vtotal);

	/* A refresh no panel runs at means a register was misread. */
	if (millihz < I915_XD9_FW_REFRESH_MIN || millihz > I915_XD9_FW_REFRESH_MAX)
		return;

	/* Succeeded: the refresh is the timing's. */
	readout->refresh_millihz = (uint32_t)millihz;
	readout->refresh_measured = 1;
}

/*
 * Finds the pipe a running transcoder feeds.
 *
 * Returns 0 with the pipe, or ENOENT for a transcoder that was not read,
 * is off, or names no pipe.
 */
static int
i915_xd9_fw_transcoder_pipe(
	const struct i915_xd9_fw_registers *registers,
	unsigned transcoder,
	unsigned *pipe)
{
	uint32_t input;

	/* A transcoder not read, without a DDI function, or not running feeds nothing. */
	if (!registers->transcoder_read[transcoder])
		return ENOENT;
	if ((registers->ddi_func[transcoder] & I915_XD9_DDI_FUNC_ENABLE) == 0U)
		return ENOENT;
	if ((registers->transconf[transcoder] & I915_XD9_TRANSCONF_ENABLE) == 0U)
		return ENOENT;
	if ((registers->transconf[transcoder] & I915_XD9_TRANSCONF_STATE) == 0U)
		return ENOENT;

	/* Transcoders A to C feed their own pipes. */
	if (transcoder != I915_XD9_TRANS_EDP) {
		*pipe = transcoder;
		return 0;
	}

	/* The eDP transcoder feeds the pipe its input select names. */
	input = (registers->ddi_func[transcoder] >> I915_XD9_DDI_FUNC_EDP_INPUT_SHIFT) & I915_XD9_DDI_FUNC_EDP_INPUT_MASK;
	if (input == I915_XD9_DDI_FUNC_EDP_INPUT_A_ON || input == I915_XD9_DDI_FUNC_EDP_INPUT_A_ONOFF) {
		*pipe = 0U;
	} else if (input == I915_XD9_DDI_FUNC_EDP_INPUT_B_ONOFF) {
		*pipe = 1U;
	} else if (input == I915_XD9_DDI_FUNC_EDP_INPUT_C_ONOFF) {
		*pipe = 2U;
	} else {
		return ENOENT;
	}

	/* Succeeded: the pipe the transcoder feeds. */
	return 0;
}

/* Reports a transcoder's name for the log. */
static const char *
i915_xd9_fw_transcoder_name(
	unsigned transcoder)
{
	/* The eDP transcoder, then A to C by their index. */
	if (transcoder == I915_XD9_TRANS_EDP)
		return "EDP";
	if (transcoder == 0U)
		return "A";
	if (transcoder == 1U)
		return "B";

	/* Succeeded: the last one. */
	return "C";
}

/* Reads and logs a powered display 9 pipe's first plane, and records a picture that reaches the driver's pages. */
static void
i915_xd9_fw_check_plane(
	struct i915_xd_io *io,
	unsigned pipe,
	uint64_t driver_start,
	struct i915_xd9_fw_check *check)
{
	uint64_t start;
	uint64_t rows;

	/* The plane's enable, layout and surface. */
	check->pipe_read[pipe] = 1;
	check->plane_ctl[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_CTL(pipe));
	check->plane_stride[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_STRIDE(pipe));
	check->plane_size[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_SIZE(pipe));
	check->plane_surf[pipe] = drv_i915_xd_io_read(io, I915_XD9_PLANE_SURF(pipe));
	kern_logf("i915: xd9-firmware: pipe %u PLANE_CTL=0x%08x STRIDE=0x%08x SIZE=0x%08x SURF=0x%08x\n",
	    pipe,
	    check->plane_ctl[pipe],
	    check->plane_stride[pipe],
	    check->plane_size[pipe],
	    check->plane_surf[pipe]);

	/* A plane that is off scans nothing out. */
	if ((check->plane_ctl[pipe] & I915_XD9_PLANE_CTL_ENABLE) == 0U)
		return;

	/* The bytes the picture covers: its rows of its stride from its surface. */
	start = check->plane_surf[pipe] & I915_XD9_PLANE_SURF_ADDRESS;
	rows = ((check->plane_size[pipe] >> I915_XD9_PLANE_SIZE_HEIGHT_SHIFT) & I915_XD9_FW_HEIGHT_MASK) + 1U;
	check->plane_end[pipe] = start + (uint64_t)(check->plane_stride[pipe] & I915_XD9_FW_STRIDE_MASK) * I915_XD9_PLANE_STRIDE_UNIT * rows;

	/* A picture that clears the driver's pages. */
	if (check->plane_end[pipe] <= driver_start)
		return;

	/* Succeeded in learning that the picture ends inside the driver's pages. */
	check->overlap_pipes |= 1U << pipe;
	kern_logf("i915: xd9-firmware: pipe %u scans out 0x%llx..0x%llx, inside the driver's GGTT pages\n",
	    pipe,
	    (unsigned long long)start,
	    (unsigned long long)check->plane_end[pipe]);
}
