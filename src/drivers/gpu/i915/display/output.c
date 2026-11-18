/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The choice of the resident node's output by the GPU scanout rule.
 *
 * The resident node has one display, and it is the output the firmware
 * (GOP) was scanning out when the driver started: the driver starts no
 * scanout of its own choosing on any other output.  The firmware's
 * output is read from the transcoders it left lit, by the platform's
 * ports: DDI A in DP SST mode is the eDP panel; a combo PHY port in HDMI
 * or DVI mode is the HDMI display, which runs on pipe B in DVI mode (no
 * infoframes, no audio); another port in DP SST mode is the external
 * DisplayPort display.  The firmware's output on any other interface is
 * one this driver cannot light yet: the display is
 * then left as the firmware left it (display.c marks the node without a
 * display before the first display write).  Without a lit pipe (no
 * firmware picture at all) the built-in panel is the output.  display= no
 * longer chooses and is logged as ignored.
 *
 * The HDMI mode is the one display.mode=WxH[@R] names -- looked up in the
 * sink's EDID, then in the CEA modes every HDMI sink takes, then computed
 * with the CVT reduced-blanking formula -- or, without it, the preferred
 * detailed timing of the EDID, or CEA format 4 (1280x720) when no EDID was
 * read.  The choice is made once and does not follow a later hotplug.
 */

#include "output.h"
#include "dp-ext-kern.h"
#include "hpd.h"
#include "edp-device.h"
#include "tc-port.h"

#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>

#include <uapi/errno.h>

/* The connector status of a connected sink (enum connector_status). */
#define I915_OUTPUT_CONNECTED		1

/* TRANS_DDI_FUNC_CTL: the enable bit, the port select of display 12+ (port + 1, 0 for none) and the mode select. */
#define I915_OUTPUT_DDI_FUNC_ENABLE	0x80000000U
#define I915_OUTPUT_DDI_PORT_SHIFT	27U
#define I915_OUTPUT_DDI_PORT_MASK	0xfU
#define I915_OUTPUT_DDI_MODE_SHIFT	24U
#define I915_OUTPUT_DDI_MODE_MASK	7U

/* The transcoder's mode selects: HDMI, DVI, DP SST and DP MST. */
#define I915_OUTPUT_MODE_HDMI		0U
#define I915_OUTPUT_MODE_DVI		1U
#define I915_OUTPUT_MODE_DP_SST		2U
#define I915_OUTPUT_MODE_DP_MST		3U

/* The ports: A (the panel's DDI), and the first Type-C port's DDI, from which the log names ports TC1 on. */
#define I915_OUTPUT_PORT_A		0
#define I915_OUTPUT_PORT_TC1		3

/* The pipes the firmware's readout covers. */
#define I915_OUTPUT_PIPES		4U

/* The connector status of a sink that is not there (enum connector_status). */
#define I915_OUTPUT_DISCONNECTED	2

/* The reference clock the port PLLs are planned from when the CDCLK state has none (kHz, non-SSC). */
#define I915_OUTPUT_REF_KHZ		38400

/*
 * With display=hdmi the sink is asked again for this long before the panel
 * takes its place: on bare metal the probe runs a few seconds after power-on,
 * before a USB-powered LCD's controller answers the EDID read.
 */
#define I915_OUTPUT_HDMI_WAIT_MS	6000U
#define I915_OUTPUT_HDMI_RETRY_MS	250U

/* The highest TMDS clock this path drives: HDMI 1.4 without scrambling (kHz). */
#define I915_OUTPUT_MAX_CLOCK_KHZ	340000

/* The EDID block size, the first detailed descriptor and the descriptor size. */
#define I915_OUTPUT_EDID_BLOCK		128U
#define I915_OUTPUT_EDID_DTD_FIRST	54U
#define I915_OUTPUT_EDID_DTD_SIZE	18U
#define I915_OUTPUT_EDID_DTD_COUNT	4U

/* The EDID base block's screen size in centimetres, and the CEA-861 extension's tag. */
#define I915_OUTPUT_EDID_WIDTH_CM	21U
#define I915_OUTPUT_EDID_HEIGHT_CM	22U
#define I915_OUTPUT_EDID_CEA_TAG	0x02U

/* The detailed timing flags: interlaced, digital separate sync, and its two polarities. */
#define I915_OUTPUT_DTD_INTERLACED	0x80U
#define I915_OUTPUT_DTD_SYNC_MASK	0x18U
#define I915_OUTPUT_DTD_DIGITAL_SEP	0x18U
#define I915_OUTPUT_DTD_VSYNC_POS	0x04U
#define I915_OUTPUT_DTD_HSYNC_POS	0x02U

/* A refresh rate given with display.mode= matches a timing within this many hertz. */
#define I915_OUTPUT_REFRESH_SLACK_HZ	1U

/* The refresh rate display.mode= means without @R. */
#define I915_OUTPUT_DEFAULT_REFRESH_HZ	60U

/* CVT reduced blanking, version 1: the horizontal blank, sync and front porch (pixels). */
#define I915_OUTPUT_CVT_RB_H_BLANK	160U
#define I915_OUTPUT_CVT_RB_H_SYNC	32U
#define I915_OUTPUT_CVT_RB_H_FRONT	48U

/* CVT reduced blanking: the least vertical blank time (us), the front porch and the least back porch (lines). */
#define I915_OUTPUT_CVT_RB_MIN_VBLANK_US	460U
#define I915_OUTPUT_CVT_RB_V_FRONT	3U
#define I915_OUTPUT_CVT_RB_MIN_V_BACK	6U

/* CVT: the pixel clock step (kHz). */
#define I915_OUTPUT_CVT_CLOCK_STEP_KHZ	250U

/*
 * The CEA-861 modes a display.mode= without a match in the EDID takes
 * before the CVT formula: every HDMI sink accepts formats 1, 4 and 16.
 * The table is constant.
 */
static const struct i915_display_mode i915_output_cea_modes[] = {
	/* Format 1: 640x480 at 60 Hz, 25.175 MHz, negative syncs. */
	{
		.clock_khz = 25175,
		.hdisplay = 640,
		.hsync_start = 656,
		.hsync_end = 752,
		.htotal = 800,
		.vdisplay = 480,
		.vsync_start = 490,
		.vsync_end = 492,
		.vtotal = 525,
		.hsync_positive = 0,
		.vsync_positive = 0,
		.edid_bpc = 8,
	},

	/* Format 4: 1280x720 at 60 Hz, 74.25 MHz, positive syncs. */
	{
		.clock_khz = 74250,
		.hdisplay = 1280,
		.hsync_start = 1390,
		.hsync_end = 1430,
		.htotal = 1650,
		.vdisplay = 720,
		.vsync_start = 725,
		.vsync_end = 730,
		.vtotal = 750,
		.hsync_positive = 1,
		.vsync_positive = 1,
		.edid_bpc = 8,
	},

	/* Format 16: 1920x1080 at 60 Hz, 148.5 MHz, positive syncs. */
	{
		.clock_khz = 148500,
		.hdisplay = 1920,
		.hsync_start = 2008,
		.hsync_end = 2052,
		.htotal = 2200,
		.vdisplay = 1080,
		.vsync_start = 1084,
		.vsync_end = 1089,
		.vtotal = 1125,
		.hsync_positive = 1,
		.vsync_positive = 1,
		.edid_bpc = 8,
	},
};

/* The index of CEA format 4 in the table: the mode of a sink without an EDID. */
#define I915_OUTPUT_CEA4_INDEX		1U

/* The number of modes in the CEA table. */
#define I915_OUTPUT_CEA_COUNT		(sizeof(i915_output_cea_modes) / sizeof(i915_output_cea_modes[0]))

static int i915_output_hdmi(struct i915_display *display, const char **reason);
static int i915_output_hdmi_mode(struct i915_display *display, unsigned connector, struct i915_display_output *output, const char **reason);
static void i915_output_panel_connector(struct i915_display *display, struct i915_display_output *output);
static int i915_output_dp_ext(struct i915_display *display, unsigned connector, int port, struct i915_display_output *output, const char **reason);
static void i915_output_hdmi_wait(struct i915_display *display, const char *name);
static void i915_output_dp_tc_wait(struct i915_display *display, const char *name);
static void i915_output_choose(struct i915_display *display);
static void i915_output_inventory(struct i915_display *display);
static int i915_output_wanted_mode(uint32_t *width, uint32_t *height, uint32_t *refresh_hz);
static uint32_t i915_output_refresh_hz(const struct i915_display_mode *mode);
static int i915_output_mode_matches(const struct i915_display_mode *mode, uint32_t width, uint32_t height, uint32_t refresh_hz);
static int i915_output_edid_find(const uint8_t *edid, unsigned edid_size, uint32_t width, uint32_t height, uint32_t refresh_hz, struct i915_display_mode *mode);
static int i915_output_edid_block_find(const uint8_t *block, unsigned first, unsigned last, uint32_t width, uint32_t height, uint32_t refresh_hz, struct i915_display_mode *mode);
static void i915_output_edid_size(const uint8_t *edid, unsigned edid_size, struct i915_display_mode *mode);
static uint32_t i915_output_cvt_vsync(uint32_t width, uint32_t height);
static void i915_output_link_from_plan(const struct i915_xd_plan *plan, uint32_t ref_khz, struct i915_display_link *link);
static int i915_output_hdmi_port(const struct i915_display *display, int port);
static int i915_output_connector_of(struct i915_display *display, unsigned kind, int port);
static int i915_output_panel_mode(const struct i915_lcd_kernel_deps *d, uint32_t *width, uint32_t *height, uint32_t *refresh_millihz);
static int i915_output_panel_size_mm(const struct i915_lcd_kernel_deps *d, uint32_t *width_mm, uint32_t *height_mm);

/*
 * Reads the firmware's output from the record of the firmware's display,
 * by the platform's ports.
 *
 * Every lit pipe is recorded; the output is the first lit pipe whose
 * transcoder is enabled and drives a port (a firmware that clones onto two
 * outputs gives the lowest pipe, the panel's on these machines).  DDI A in
 * DP SST mode is the panel; a combo PHY port in HDMI or DVI mode is the
 * HDMI path; any other port of the platform in DP SST mode is the external
 * DisplayPort path; anything else (a port the platform does not drive,
 * DP MST, HDMI on a Type-C port) is not lit by the driver.
 */
void
drv_i915_gop_output_read(
	const struct i915_xd_platform *platform,
	const struct i915_xd_firmware *firmware,
	struct i915_gop_output *gop)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_fw_pipe *pipe;
	unsigned select;
	unsigned index;
	uint32_t function;

	/* Nothing lit until a pipe says so. */
	gop->kind = I915_GOP_NONE;
	gop->pipe = 0U;
	gop->port = -1;
	gop->mode = 0U;
	gop->pipes = 0U;

	/* Each lit pipe; the first that drives a port is the output. */
	for (index = 0U; index < I915_OUTPUT_PIPES; index++) {
		/* A pipe the firmware did not light. */
		pipe = &firmware->pipe[index];
		if (pipe->state != I915_XD_FW_PIPE_LIT)
			continue;
		gop->pipes |= 1U << index;

		/* A transcoder that is off or drives no port is not an output. */
		function = pipe->ddi_function;
		select = (function >> I915_OUTPUT_DDI_PORT_SHIFT) & I915_OUTPUT_DDI_PORT_MASK;
		if ((function & I915_OUTPUT_DDI_FUNC_ENABLE) == 0U || select == 0U)
			continue;
		if (gop->kind != I915_GOP_NONE)
			continue;

		/* The output: its pipe, port and mode, of a port the platform may not drive. */
		gop->pipe = index;
		gop->port = (int)select - 1;
		gop->mode = (function >> I915_OUTPUT_DDI_MODE_SHIFT) & I915_OUTPUT_DDI_MODE_MASK;
		gop->kind = I915_GOP_OTHER;
		port = NULL;
		if (platform != NULL)
			port = drv_i915_xd_platform_port(platform, (unsigned)gop->port);

		/* DDI A in DP SST mode is the panel. */
		if (gop->port == I915_OUTPUT_PORT_A && gop->mode == I915_OUTPUT_MODE_DP_SST) {
			gop->kind = I915_GOP_EDP;
			continue;
		}

		/* A port the platform does not drive is not lit. */
		if (port == NULL)
			continue;

		/* A combo PHY port in HDMI or DVI mode is the HDMI path. */
		if (port->phy_kind == I915_XD_PHY_COMBO &&
		    (gop->mode == I915_OUTPUT_MODE_HDMI ||
		     gop->mode == I915_OUTPUT_MODE_DVI))
			gop->kind = I915_GOP_HDMI;

		/* Any other port in DP SST mode is the external DisplayPort path. */
		if (gop->mode == I915_OUTPUT_MODE_DP_SST)
			gop->kind = I915_GOP_DP_TC;
	}
}

/*
 * Names the firmware's output for the log.
 */
void
drv_i915_gop_output_name(
	const struct i915_gop_output *gop,
	char *name,
	unsigned size)
{
	static const char *const modes[] = { "HDMI", "DVI", "DP SST", "DP MST", "FDI", "mode 5", "mode 6", "mode 7" };
	char port[8];

	/* No lit output. */
	if (gop->kind == I915_GOP_NONE) {
		kern_snprintf(name, size, "none");
		return;
	}

	/* The port: A, B, C, then the Type-C ports. */
	if (gop->port >= I915_OUTPUT_PORT_TC1)
		kern_snprintf(port, sizeof(port), "TC%d", gop->port - I915_OUTPUT_PORT_TC1 + 1);
	else
		kern_snprintf(port, sizeof(port), "%c", 'A' + gop->port);

	/* The mode, the port and the pipe. */
	kern_snprintf(name, size, "%s on DDI %s, pipe %c", modes[gop->mode & I915_OUTPUT_DDI_MODE_MASK], port, 'A' + (int)gop->pipe);
}

/*
 * Chooses the output of the resident node: the firmware's output.
 *
 * The panel when the firmware lit the panel, or lit nothing; the HDMI
 * display when the firmware lit it and its mode can be driven (the sink is
 * asked again for a while, since the firmware was driving it); otherwise
 * no output -- the driver does not light another output in its place.
 */
void
drv_i915_display_output_select(
	struct i915_display *display)
{
	/* The resident output, then every connector's state for the inventory. */
	i915_output_choose(display);
	i915_output_inventory(display);

	/* The firmware's output, which comes back when a moved output's last hold ends. */
	display->gop_output = display->output;
}

/* Chooses the resident output: the firmware's (see drv_i915_display_output_select). */
static void
i915_output_choose(
	struct i915_display *display)
{
	const struct kern_boot_parameters *parameters;
	const char *wanted;
	char name[48];
	uint32_t refresh;

	/* The panel (its connector, when the hotplug path knows it) until the firmware's output says otherwise. */
	drv_i915_display_output_panel(display, &display->output);
	drv_i915_gop_output_name(&display->gop, name, sizeof(name));

	/* display= no longer chooses the output. */
	parameters = kern_boot_parameters_current();
	wanted = kern_boot_parameters_value(parameters, KERN_BOOT_PARAMETER_DISPLAY);
	if (wanted != NULL)
		kern_logf("i915: display output: display=%s ignored: the driver lights the firmware's output only (GPU scanout rule)\n", wanted);

	/* No firmware picture at all: the built-in panel. */
	if (display->gop.kind == I915_GOP_NONE) {
		kern_logf("i915: display output: eDP panel (the firmware lit no output)\n");
		return;
	}

	/* The firmware's panel. */
	if (display->gop.kind == I915_GOP_EDP) {
		kern_logf("i915: display output: eDP panel, the firmware's output (%s, lit pipes 0x%x)\n", name, display->gop.pipes);
		return;
	}

	/*
	 * The firmware's DisplayPort display on a Type-C port:
	 * its sink is asked over the port's AUX channel before anything of the
	 * firmware's display is stopped; when it cannot be driven the node has
	 * no output and the firmware's picture stays (the GPU scanout rule).
	 */
	if (display->gop.kind == I915_GOP_DP_TC) {
		i915_output_dp_tc_wait(display, name);
		return;
	}

	/* An interface this driver cannot light: display.c left the display as the firmware did. */
	if (display->gop.kind != I915_GOP_HDMI) {
		display->output.none = 1;
		kern_logf("i915: display output: none: the firmware's output (%s) is not one this driver lights\n", name);
		return;
	}

	/* The firmware's HDMI display, asked again while its sink is not answering yet. */
	i915_output_hdmi_wait(display, name);
	if (display->output.kind != I915_OUTPUT_KIND_HDMI)
		return;

	/* The node shows the HDMI display from here on. */
	refresh = i915_output_refresh_hz(&display->output.mode);
	kern_logf("i915: display output: HDMI on DDI %c, pipe %c, DVI mode, the firmware's output (%s): %ux%u@%u Hz %d kHz (mode from %s) %ux%u mm\n",
	    (char)('A' + display->output.port),
	    (char)('A' + display->output.pipe),
	    name,
	    display->output.mode.hdisplay,
	    display->output.mode.vdisplay,
	    refresh,
	    display->output.mode.clock_khz,
	    display->output.mode_source,
	    display->output.mode.width_mm,
	    display->output.mode.height_mm);
}

/*
 * Reports the mode of the chosen output.
 *
 * The HDMI mode was fixed by the choice; the panel's mode is read from the
 * resident eDP's state.
 */
int
drv_i915_display_output_mode(
	struct i915_display *display,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	const struct i915_display_mode *m;
	uint64_t pixels;
	int error;

	/* The node has an output only once the resident dependencies exist, and when one was chosen. */
	if (display->rctx.lcd == NULL || display->output.none)
		return ENXIO;

	/* The panel: its own timing. */
	if (display->output.kind == I915_OUTPUT_KIND_PANEL) {
		error = i915_output_panel_mode(display->rctx.lcd, width, height, refresh_millihz);
		if (error != 0)
			return ENXIO;

		/* Succeeded: the panel's mode is reported. */
		return 0;
	}

	/* Another output's mode (fixed when it was chosen), and its refresh in millihertz. */
	m = &display->output.mode;
	pixels = (uint64_t)m->htotal * m->vtotal;
	if (pixels == 0U)
		return ENXIO;
	*width = m->hdisplay;
	*height = m->vdisplay;
	*refresh_millihz = (uint32_t)(((uint64_t)m->clock_khz * 1000000ULL + pixels / 2U) / pixels);

	/* Succeeded: the HDMI mode is reported. */
	return 0;
}

/*
 * Reports the physical size of the chosen output in millimetres.
 */
int
drv_i915_display_output_size_mm(
	struct i915_display *display,
	uint32_t *width_mm,
	uint32_t *height_mm)
{
	int error;

	/* No output, no size. */
	if (display->output.none)
		return ENODEV;

	/* The panel: the size its EDID reported. */
	if (display->output.kind == I915_OUTPUT_KIND_PANEL) {
		error = i915_output_panel_size_mm(display->rctx.lcd, width_mm, height_mm);
		if (error != 0)
			return ENODEV;

		/* Succeeded: the panel's size is reported. */
		return 0;
	}

	/* Another output has a size only when its EDID reported one. */
	if (display->output.mode.width_mm == 0U || display->output.mode.height_mm == 0U)
		return ENODEV;

	/* The size the EDID reported. */
	*width_mm = display->output.mode.width_mm;
	*height_mm = display->output.mode.height_mm;

	/* Succeeded: the HDMI display's size is reported. */
	return 0;
}

/*
 * Names the chosen output for the display query.
 */
const char *
drv_i915_display_output_name(
	const struct i915_display *display)
{
	/* An HDMI display, or an external DisplayPort display. */
	if (display->output.kind == I915_OUTPUT_KIND_HDMI)
		return "HDMI";
	if (display->output.kind == I915_OUTPUT_KIND_DP_EXT)
		return "DisplayPort";

	/* The built-in panel. */
	return "eDP panel";
}

/*
 * Describes a connector of the hotplug path for the display inventory as
 * the hotplug path took it, except the built-in panel, which is connected
 * with its own mode for as long as the node has it.
 *
 * The hotplug path never detects an eDP connector: its status stays
 * unknown and it reads no EDID for it.  Taken as it is, the panel would be
 * unplugged whenever another output is lit, and a
 * claim that brings the output back to the panel (the lid opened)
 * would be refused with ENXIO.  The panel's mode and size are
 * the ones the resident dependencies read at the start (its EDID or the
 * VBT); a node without them leaves the connector as the hotplug path took
 * it.  0, or ENOENT for a connector the path does not have.
 */
int
drv_i915_display_output_connector(
	struct i915_display *display,
	unsigned connector,
	struct i915_hpd_output *found)
{
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t width_mm;
	uint32_t height_mm;
	int error;

	/* The connector as the hotplug path last took it. */
	error = drv_i915_hpd_output(display, connector, found);
	if (error != 0)
		return error;

	/* Any other kind is as it was detected. */
	if (found->kind != I915_HPD_OUTPUT_EDP)
		return 0;

	/* A node without the panel's dependencies has no panel to show. */
	if (display->rctx.lcd == NULL)
		return 0;

	/* The panel's own mode; without one there is no panel to show. */
	error = i915_output_panel_mode(display->rctx.lcd, &width, &height, &refresh_millihz);
	if (error != 0)
		return 0;

	/* The panel is connected, with its mode, while the node has it. */
	found->connected = 1;
	found->width = width;
	found->height = height;
	found->refresh_millihz = refresh_millihz;

	/* Its size, when its EDID gave one. */
	error = i915_output_panel_size_mm(display->rctx.lcd, &width_mm, &height_mm);
	if (error == 0) {
		found->width_mm = width_mm;
		found->height_mm = height_mm;
	}

	/* Succeeded: the connector is described. */
	return 0;
}

/*
 * Prepares another connected output for the resident run (a
 * claim that moves the output while no lease is held): reads what the
 * connector is and fills output for it, without detecting it again or
 * writing the hardware.  Each kind of connector has its own preparation;
 * this is the one place that chooses among them.  Returns 0, ENXIO for a
 * connector that is gone or not connected, EOPNOTSUPP for a kind this
 * driver does not light (a DisplayPort display off the Type-C ports, an
 * HDMI port that is not a combo PHY port), or the preparation's error; reason says
 * why.
 */
int
drv_i915_display_output_prepare(
	struct i915_display *display,
	unsigned connector,
	struct i915_display_output *output,
	const char **reason)
{
	struct i915_hpd_output found;
	int error;

	/* The connector as the hotplug path last took it (the panel connected while the node has it). */
	kern_memset(output, 0, sizeof(*output));
	error = drv_i915_display_output_connector(display, connector, &found);
	if (error != 0) {
		*reason = "no such connector";
		return ENXIO;
	}

	/* A connector with nothing plugged in cannot be lit. */
	if (!found.connected) {
		*reason = "the connector is not connected";
		return ENXIO;
	}

	/* Each kind's preparation. */
	switch (found.kind) {
	case I915_HPD_OUTPUT_EDP:
		/* The built-in panel: the resident dependencies describe it. */
		output->kind = I915_OUTPUT_KIND_PANEL;
		output->has_connector = 1;
		output->connector = connector;
		break;
	case I915_HPD_OUTPUT_HDMI:
		/* An HDMI display on a combo PHY port, in DVI mode: its mode from the EDID the hotplug path read. */
		error = i915_output_hdmi_mode(display, connector, output, reason);
		if (error != 0)
			return error;
		break;
	case I915_HPD_OUTPUT_DP:
		/* An external DisplayPort display: only a Type-C port's is lit. */
		error = i915_output_dp_ext(display, connector, found.port, output, reason);
		if (error != 0)
			return error;
		break;
	default:
		*reason = "a connector of a kind this driver does not light";
		return EOPNOTSUPP;
	}

	/* Succeeded: output is ready for the resident run. */
	return 0;
}

/*
 * Makes an output the built-in panel, of its connector when the hotplug
 * path knows it (the start's default, and the panel a moved output falls
 * back to after a sleep).
 */
void
drv_i915_display_output_panel(
	struct i915_display *display,
	struct i915_display_output *output)
{
	/* The panel on its pipe, of no known connector yet. */
	kern_memset(output, 0, sizeof(*output));
	output->kind = I915_OUTPUT_KIND_PANEL;
	output->pipe = I915_OUTPUT_PANEL_PIPE;

	/* Succeeded: its connector, when there is one. */
	i915_output_panel_connector(display, output);
}

/*
 * Gives the pipe the resident run drives the output on: the one its
 * preparation chose (the panel's pipe A, pipe B for an HDMI or an
 * external DisplayPort display).
 */
unsigned
drv_i915_display_output_pipe(
	const struct i915_display_output *output)
{
	/* Succeeded: the output's pipe. */
	return output->pipe;
}

/*
 * Decodes one EDID detailed timing descriptor into a mode.
 *
 * The descriptor's layout is VESA E-EDID 1.4 section 3.10.2: the pixel
 * clock in 10 kHz units, then the active and blanking sizes, the sync
 * offsets and widths, the image size in millimetres and the flags.
 */
int
drv_i915_output_edid_timing(
	const uint8_t *d,
	struct i915_display_mode *mode)
{
	uint32_t clock;
	uint32_t hactive;
	uint32_t hblank;
	uint32_t vactive;
	uint32_t vblank;
	uint32_t hfront;
	uint32_t hsync;
	uint32_t vfront;
	uint32_t vsync;
	uint8_t flags;

	/* A descriptor with a zero clock is a display descriptor, not a timing. */
	clock = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
	if (clock == 0U)
		return EINVAL;

	/* An interlaced timing is not driven here. */
	flags = d[17];
	if ((flags & I915_OUTPUT_DTD_INTERLACED) != 0U)
		return EINVAL;

	/* The horizontal active and blanking pixels (upper four bits of each in byte 4). */
	hactive = (uint32_t)d[2] | (((uint32_t)d[4] >> 4) << 8);
	hblank = (uint32_t)d[3] | (((uint32_t)d[4] & 0x0fU) << 8);

	/* The vertical active and blanking lines (upper four bits of each in byte 7). */
	vactive = (uint32_t)d[5] | (((uint32_t)d[7] >> 4) << 8);
	vblank = (uint32_t)d[6] | (((uint32_t)d[7] & 0x0fU) << 8);

	/* The sync offsets and widths, their upper bits in byte 11. */
	hfront = (uint32_t)d[8] | ((((uint32_t)d[11] >> 6) & 3U) << 8);
	hsync = (uint32_t)d[9] | ((((uint32_t)d[11] >> 4) & 3U) << 8);
	vfront = ((uint32_t)d[10] >> 4) | ((((uint32_t)d[11] >> 2) & 3U) << 4);
	vsync = ((uint32_t)d[10] & 0x0fU) | (((uint32_t)d[11] & 3U) << 4);

	/* Refuses an empty picture. */
	if (hactive == 0U || vactive == 0U)
		return EINVAL;

	/* The timing. */
	kern_memset(mode, 0, sizeof(*mode));
	mode->clock_khz = (int)(clock * 10U);
	mode->hdisplay = (uint16_t)hactive;
	mode->hsync_start = (uint16_t)(hactive + hfront);
	mode->hsync_end = (uint16_t)(hactive + hfront + hsync);
	mode->htotal = (uint16_t)(hactive + hblank);
	mode->vdisplay = (uint16_t)vactive;
	mode->vsync_start = (uint16_t)(vactive + vfront);
	mode->vsync_end = (uint16_t)(vactive + vfront + vsync);
	mode->vtotal = (uint16_t)(vactive + vblank);
	mode->edid_bpc = 8;

	/* The image size, in millimetres (upper four bits of each in byte 14). */
	mode->width_mm = (uint16_t)((uint32_t)d[12] | (((uint32_t)d[14] >> 4) << 8));
	mode->height_mm = (uint16_t)((uint32_t)d[13] | (((uint32_t)d[14] & 0x0fU) << 8));

	/* Digital separate sync carries both polarities; any other kind is taken as negative. */
	if ((flags & I915_OUTPUT_DTD_SYNC_MASK) == I915_OUTPUT_DTD_DIGITAL_SEP) {
		if ((flags & I915_OUTPUT_DTD_HSYNC_POS) != 0U)
			mode->hsync_positive = 1;
		if ((flags & I915_OUTPUT_DTD_VSYNC_POS) != 0U)
			mode->vsync_positive = 1;
	}

	/* Succeeded: the descriptor is a progressive timing. */
	return 0;
}

/*
 * Computes the CVT reduced-blanking (version 1) timing of a mode.
 *
 * VESA CVT 1.2 section 5.4: a fixed horizontal blank of 160 pixels, a
 * vertical blank of at least 460 us, a vertical sync width that names the
 * aspect ratio, and a pixel clock in steps of 0.25 MHz; syncs are positive
 * horizontally and negative vertically.
 */
int
drv_i915_output_cvt_rb(
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz,
	struct i915_display_mode *mode)
{
	uint64_t frame_ps;
	uint64_t line_ps;
	uint64_t clock_khz;
	uint32_t vsync;
	uint32_t vblank;
	uint32_t min_vblank;
	uint32_t htotal;
	uint32_t vtotal;

	/* Refuses a size or rate the formula cannot serve. */
	if (width == 0U || height == 0U)
		return EINVAL;
	if (refresh_hz == 0U)
		return EINVAL;

	/* The frame time less the least vertical blank, over the active lines: the estimated line time (ps). */
	frame_ps = 1000000000000ULL / refresh_hz;
	if (frame_ps <= (uint64_t)I915_OUTPUT_CVT_RB_MIN_VBLANK_US * 1000000ULL)
		return EINVAL;

	/* A frame too short for its lines has no line time. */
	line_ps = (frame_ps - (uint64_t)I915_OUTPUT_CVT_RB_MIN_VBLANK_US * 1000000ULL) / height;
	if (line_ps == 0U)
		return EINVAL;

	/* The vertical blank: the lines 460 us takes plus one, at least front porch, sync and back porch. */
	vsync = i915_output_cvt_vsync(width, height);
	vblank = (uint32_t)((uint64_t)I915_OUTPUT_CVT_RB_MIN_VBLANK_US * 1000000ULL / line_ps) + 1U;
	min_vblank = I915_OUTPUT_CVT_RB_V_FRONT + vsync + I915_OUTPUT_CVT_RB_MIN_V_BACK;
	if (vblank < min_vblank)
		vblank = min_vblank;

	/* The totals, and the pixel clock rounded down to its 0.25 MHz step. */
	htotal = width + I915_OUTPUT_CVT_RB_H_BLANK;
	vtotal = height + vblank;
	clock_khz = (uint64_t)refresh_hz * htotal * vtotal / 1000U;
	clock_khz = clock_khz / I915_OUTPUT_CVT_CLOCK_STEP_KHZ * I915_OUTPUT_CVT_CLOCK_STEP_KHZ;

	/* Refuses a timing the 16-bit fields or the clock cannot hold. */
	if (htotal > 0xffffU || vtotal > 0xffffU)
		return EINVAL;
	if (clock_khz == 0U || clock_khz > 0x7fffffffULL)
		return EINVAL;

	/* The timing. */
	kern_memset(mode, 0, sizeof(*mode));
	mode->clock_khz = (int)clock_khz;
	mode->hdisplay = (uint16_t)width;
	mode->hsync_start = (uint16_t)(width + I915_OUTPUT_CVT_RB_H_FRONT);
	mode->hsync_end = (uint16_t)(width + I915_OUTPUT_CVT_RB_H_FRONT + I915_OUTPUT_CVT_RB_H_SYNC);
	mode->htotal = (uint16_t)htotal;
	mode->vdisplay = (uint16_t)height;
	mode->vsync_start = (uint16_t)(height + I915_OUTPUT_CVT_RB_V_FRONT);
	mode->vsync_end = (uint16_t)(height + I915_OUTPUT_CVT_RB_V_FRONT + vsync);
	mode->vtotal = (uint16_t)vtotal;
	mode->hsync_positive = 1;
	mode->vsync_positive = 0;
	mode->edid_bpc = 8;

	/* Succeeded: the timing is computed. */
	return 0;
}

/*
 * Picks the HDMI mode from display.mode= and the sink's EDID.
 *
 * A wanted size is looked up in the EDID's detailed timings, then in the
 * CEA table, then computed with CVT reduced blanking (at 60 Hz without
 * @R).  Without a wanted size the EDID's first detailed timing is taken,
 * and CEA format 4 when there is none.  The physical size comes from the
 * EDID when the mode has none of its own.
 */
int
drv_i915_output_pick_mode(
	uint32_t want_width,
	uint32_t want_height,
	uint32_t want_refresh_hz,
	const uint8_t *edid,
	unsigned edid_size,
	struct i915_display_mode *mode,
	const char **source)
{
	uint32_t refresh;
	unsigned i;
	int matches;
	int error;

	/* No wanted size: the sink's preferred timing, or CEA format 4. */
	if (want_width == 0U) {
		error = EINVAL;
		if (edid != NULL && edid_size >= I915_OUTPUT_EDID_BLOCK)
			error = drv_i915_output_edid_timing(edid + I915_OUTPUT_EDID_DTD_FIRST, mode);
		if (error == 0) {
			*source = "EDID";
		} else {
			*mode = i915_output_cea_modes[I915_OUTPUT_CEA4_INDEX];
			*source = "CEA 4 (no EDID timing)";
		}

		/* Succeeded: the preferred mode, with the EDID's size when it has none of its own. */
		i915_output_edid_size(edid, edid_size, mode);
		return 0;
	}

	/* The wanted size among the sink's own timings. */
	error = i915_output_edid_find(edid, edid_size, want_width, want_height, want_refresh_hz, mode);
	if (error == 0) {
		/* Succeeded: the sink's own timing of that size. */
		*source = "display.mode, EDID timing";
		i915_output_edid_size(edid, edid_size, mode);
		return 0;
	}

	/* The wanted size among the CEA modes every HDMI sink takes. */
	for (i = 0U; i < I915_OUTPUT_CEA_COUNT; i++) {
		matches = i915_output_mode_matches(&i915_output_cea_modes[i], want_width, want_height, want_refresh_hz);
		if (matches) {
			/* Succeeded: the CEA mode of that size, with the EDID's size. */
			*mode = i915_output_cea_modes[i];
			*source = "display.mode, CEA mode";
			i915_output_edid_size(edid, edid_size, mode);
			return 0;
		}
	}

	/* The CVT reduced-blanking timing is at 60 Hz when no rate was given. */
	refresh = want_refresh_hz;
	if (refresh == 0U)
		refresh = I915_OUTPUT_DEFAULT_REFRESH_HZ;

	/* Computes the CVT reduced-blanking timing. */
	error = drv_i915_output_cvt_rb(want_width, want_height, refresh, mode);
	if (error != 0)
		return error;

	/* The EDID's size, when the sink stated one. */
	*source = "display.mode, CVT reduced blanking";
	i915_output_edid_size(edid, edid_size, mode);

	/* Succeeded: the computed timing is the mode. */
	return 0;
}

/*
 * Plans the link and the combo PLL of an HDMI mode on a combo PHY port.
 *
 * The link runs four lanes at the mode's TMDS clock, which at 8 bits per
 * colour is its pixel clock.  Nothing is written to the hardware.
 * Returns 0, or EINVAL for a missing description, a mode without a clock
 * or a clock the PLL cannot make.
 */
int
drv_i915_output_link_hdmi(
	const struct i915_xd_platform *platform,
	unsigned ddi,
	const struct i915_display_mode *mode,
	uint32_t ref_khz,
	struct i915_display_link *link)
{
	struct i915_xd_plan_input input;
	struct i915_xd_plan plan;
	unsigned refused;

	/* Refuses a call without a description or a mode with a clock. */
	if (platform == NULL)
		return EINVAL;
	if (mode->clock_khz <= 0)
		return EINVAL;

	/* The screen's plan of the link and of the PLL. */
	kern_memset(&input, 0, sizeof(input));
	input.kind = I915_XD_SCREEN_HDMI;
	input.ddi = ddi;
	input.mode = mode;
	input.platform = platform;
	input.ref_khz = ref_khz;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);

	/* The link as the plan found it, refused or not. */
	i915_output_link_from_plan(&plan, ref_khz, link);
	if (refused != I915_XD_PLAN_OK)
		return EINVAL;

	/* Succeeded: the link and the PLL are planned. */
	return 0;
}

/*
 * Plans the link and the M/N of an external DisplayPort mode on a Type-C
 * port's DDI.
 *
 * The bits per colour start from the EDID's depth (8 when undefined),
 * capped at 8 and at a branch's downstream limit, and drop by 2 down to 6;
 * for each depth the rates the source and the sink share are tried from
 * the lowest, and for each rate 1, 2 and then 4 lanes: the first that
 * carries the mode is the link.  max_rate_khz and max_lanes are the
 * fallback's limits (0: none).  The Type-C port's PLL is the picture's
 * own, made from the rate when the screen is prepared.  Nothing is
 * written to the hardware.  Returns 0, EINVAL for a missing description, a
 * mode without a clock, or a sink without a common rate or lane, or ENOSPC
 * when no link carries the mode.
 */
int
drv_i915_output_link_dp(
	const struct i915_xd_platform *platform,
	unsigned ddi,
	const struct i915_display_mode *mode,
	const struct i915_xd_dp_probe *sink,
	int max_rate_khz,
	int max_lanes,
	uint32_t ref_khz,
	struct i915_display_link *link)
{
	uint32_t common[I915_XD_DP_RATES];
	struct i915_xd_plan_input input;
	struct i915_xd_plan plan;
	unsigned refused;
	unsigned index;

	/* Refuses a call without a description or a mode with a clock. */
	if (platform == NULL)
		return EINVAL;
	if (mode->clock_khz <= 0)
		return EINVAL;

	/* Refuses a sink that shares no rate or lane with the source. */
	if (sink->num_common_rates <= 0)
		return EINVAL;
	if (sink->max_lanes <= 0)
		return EINVAL;

	/* The rates both ends share, lowest first. */
	for (index = 0U; index < (unsigned)sink->num_common_rates && index < I915_XD_DP_RATES; index++)
		common[index] = (uint32_t)sink->common_rates[index];

	/* The sink and the limits the plan works within. */
	kern_memset(&input, 0, sizeof(input));
	input.kind = I915_XD_SCREEN_DP;
	input.ddi = ddi;
	input.mode = mode;
	input.sink.rates = common;
	input.sink.nrates = index;
	input.sink.max_lanes = (unsigned)sink->max_lanes;
	if (sink->max_bpc > 0)
		input.sink.max_bpc = (unsigned)sink->max_bpc;
	input.sink.max_dotclock_khz = (uint32_t)sink->max_dotclock_khz;
	input.sink.min_tmds_khz = (uint32_t)sink->min_tmds_clock_khz;
	input.sink.max_tmds_khz = (uint32_t)sink->max_tmds_clock_khz;
	if (max_rate_khz > 0)
		input.sink.rate_limit_khz = (uint32_t)max_rate_khz;
	if (max_lanes > 0)
		input.sink.lane_limit = (unsigned)max_lanes;
	input.platform = platform;
	input.ref_khz = ref_khz;

	/* The screen's plan of the link. */
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	if (refused != I915_XD_PLAN_OK)
		return ENOSPC;

	/* Succeeded: the link and its M/N are planned. */
	i915_output_link_from_plan(&plan, ref_khz, link);
	return 0;
}

/*
 * Makes the HDMI display the output when its sink is connected and its
 * mode can be driven; reason says what refused it.
 */
static int
i915_output_hdmi(
	struct i915_display *display,
	const char **reason)
{
	int connector;
	int status;
	int error;

	/* The hotplug path must run and know the HDMI connector of the firmware's port. */
	if (!display->hpd_started) {
		*reason = "the hotplug path did not start";
		return ENODEV;
	}

	/* The HDMI connector on the firmware's port. */
	connector = i915_output_connector_of(display, I915_HPD_OUTPUT_HDMI, display->gop.port);
	if (connector < 0) {
		*reason = "no HDMI connector on the firmware's port";
		return ENODEV;
	}

	/* Detects the sink once, the way the connector's first probe does; it reads the EDID too. */
	status = drv_i915_hpd_probe_connector(display, (unsigned)connector);
#ifdef I915_TEST_HDMI_ABSENT
	/*
	 * The panel fallback test: the probe ran, but its answer is
	 * taken as a disconnected sink, the state an unplugged cable leaves.
	 */
	kern_logf("i915: display output: I915_TEST_HDMI_ABSENT takes the HDMI sink as absent (probe said %d)\n", status);
	status = I915_OUTPUT_DISCONNECTED;
#endif
	if (status != I915_OUTPUT_CONNECTED) {
		*reason = "no HDMI sink is connected at boot";
		return EAGAIN;
	}

	/* The mode from the EDID the detection read. */
	error = i915_output_hdmi_mode(display, (unsigned)connector, &display->output, reason);
	if (error != 0)
		return error;

	/* Succeeded: the HDMI display can be lit. */
	return 0;
}

/*
 * Chooses an HDMI connector's mode from the EDID the hotplug path last read
 * of it (no detection here: the hotplug path keeps it current), computes
 * its link and PLL into output, and makes output that HDMI
 * display.  reason says what refused it.
 */
static int
i915_output_hdmi_mode(
	struct i915_display *display,
	unsigned connector,
	struct i915_display_output *output,
	const char **reason)
{
	struct i915_hpd_output found;
	struct i915_display_mode mode;
	const uint8_t *edid;
	unsigned edid_size;
	uint32_t want_width;
	uint32_t want_height;
	uint32_t want_refresh;
	int ref_khz;
	int error;

	/* The connector's port, which must be a combo PHY port of the platform. */
	error = drv_i915_hpd_output(display, connector, &found);
	if (error != 0) {
		*reason = "no such connector";
		return ENXIO;
	}

	/* Refuses an HDMI port that is not a combo PHY port. */
	error = i915_output_hdmi_port(display, found.port);
	if (error != 0) {
		*reason = "an HDMI port that is not a combo PHY port";
		return EOPNOTSUPP;
	}

	/* The EDID the detection read, if it read one. */
	edid_size = 0U;
	edid = drv_i915_hpd_edid_bytes(display, connector, &edid_size);
	if (edid == NULL)
		edid_size = 0U;

	/* The mode display.mode= asks for, if any. */
	error = i915_output_wanted_mode(&want_width, &want_height, &want_refresh);
	if (error != 0) {
		*reason = "display.mode is unreadable";
		return error;
	}

	/* The mode to drive. */
	error = drv_i915_output_pick_mode(want_width, want_height, want_refresh, edid, edid_size, &mode, &output->mode_source);
	if (error != 0) {
		*reason = "no timing serves display.mode";
		return error;
	}

	/* Refuses a clock that needs scrambling, which this path does not program. */
	if (mode.clock_khz > I915_OUTPUT_MAX_CLOCK_KHZ) {
		*reason = "the mode's clock is above 340 MHz";
		return EINVAL;
	}

	/* The display's reference clock, or the platform's when the CDCLK state has none. */
	ref_khz = (int)display->cdclk.hw.ref;
	if (ref_khz <= 0)
		ref_khz = I915_OUTPUT_REF_KHZ;

	/* Plans the link and the PLL of the mode; the output's mode and link are written whole. */
	error = drv_i915_output_link_hdmi(display->xd.choice.platform, (unsigned)found.port, &mode, (uint32_t)ref_khz, &output->link);
	output->mode = mode;
	if (error != 0) {
		*reason = "the PLL refused the mode's clock";
		return error;
	}

	/* Succeeded: the output is that HDMI display, on its port and the other pipe than the panel's. */
	output->kind = I915_OUTPUT_KIND_HDMI;
	output->has_connector = 1;
	output->connector = connector;
	output->port = found.port;
	output->pipe = I915_OUTPUT_OTHER_PIPE;
	return 0;
}

/*
 * Makes the firmware's HDMI display the output: its sink is asked again
 * for a while (on bare metal the first probe can come before a USB-powered
 * LCD's controller answers the EDID read).  When it cannot be
 * driven the node has no output: the panel does not take its place.
 */
static void
i915_output_hdmi_wait(
	struct i915_display *display,
	const char *name)
{
	const char *reason;
	unsigned waited_ms;
	int error;

	/* The first answer. */
	reason = NULL;
	error = i915_output_hdmi(display, &reason);

	/* A sink not answering yet is asked again. */
	waited_ms = 0U;
	while (error == EAGAIN && waited_ms < I915_OUTPUT_HDMI_WAIT_MS) {
		kern_usleep_range(I915_OUTPUT_HDMI_RETRY_MS * 1000U, I915_OUTPUT_HDMI_RETRY_MS * 1000U);
		waited_ms += I915_OUTPUT_HDMI_RETRY_MS;
		kern_memset(&display->output, 0, sizeof(display->output));
		error = i915_output_hdmi(display, &reason);
	}

	/* How long it waited, when it did. */
	if (waited_ms != 0U)
		kern_logf("i915: display output: waited %u ms for the HDMI sink (rc=%d)\n", waited_ms, error);

	/* Not drivable: no output. */
	if (error != 0) {
		kern_memset(&display->output, 0, sizeof(display->output));
		display->output.none = 1;
		kern_logf("i915: display output: none: the firmware's HDMI output (%s) cannot be driven (%s: rc=%d)\n", name, reason, error);
		return;
	}

	/* Succeeded: the HDMI display is the output (i915_output_hdmi_mode made it so). */
	return;
}

/*
 * Makes the firmware's DisplayPort display on a Type-C port the output:
 * the DP connector of the firmware's port is found, and its
 * sink is prepared as a claim prepares it (i915_output_dp_ext: probed over
 * the port's AUX channel, its mode and link computed, nothing written).  A
 * sink not answering yet is asked again for a while, as the HDMI one is.
 * The Type-C readout counted a link for the port the firmware drives, so
 * the probes do not give its PHY back.  When it cannot be driven the node
 * has no output: the firmware's picture is kept and the panel does not
 * take its place.
 */
static void
i915_output_dp_tc_wait(
	struct i915_display *display,
	const char *name)
{
	const char *reason;
	unsigned waited_ms;
	int connector;
	int error;

	/* The firmware's port's DP connector, which the hotplug path made. */
	connector = -1;
	if (display->hpd_started)
		connector = i915_output_connector_of(display, I915_HPD_OUTPUT_DP, display->gop.port);

	/* Without the connector there is nothing to prepare. */
	if (connector < 0) {
		kern_memset(&display->output, 0, sizeof(display->output));
		display->output.none = 1;
		kern_logf("i915: display output: none: the firmware's DisplayPort output (%s) has no DP connector; the firmware's picture is kept\n", name);
		return;
	}

	/* The first answer. */
	reason = NULL;
	kern_memset(&display->output, 0, sizeof(display->output));
	error = i915_output_dp_ext(display, (unsigned)connector, display->gop.port, &display->output, &reason);

	/* A sink not answering yet is asked again. */
	waited_ms = 0U;
	while (error == ENXIO && waited_ms < I915_OUTPUT_HDMI_WAIT_MS) {
		kern_usleep_range(I915_OUTPUT_HDMI_RETRY_MS * 1000U, I915_OUTPUT_HDMI_RETRY_MS * 1000U);
		waited_ms += I915_OUTPUT_HDMI_RETRY_MS;
		kern_memset(&display->output, 0, sizeof(display->output));
		error = i915_output_dp_ext(display, (unsigned)connector, display->gop.port, &display->output, &reason);
	}

	/* How long it waited, when it did. */
	if (waited_ms != 0U)
		kern_logf("i915: display output: waited %u ms for the DisplayPort sink (rc=%d)\n", waited_ms, error);

	/* Not drivable: no output, and the firmware's picture stays. */
	if (error != 0) {
		kern_memset(&display->output, 0, sizeof(display->output));
		display->output.none = 1;
		kern_logf("i915: display output: none: the firmware's DisplayPort output (%s) cannot be driven (%s: rc=%d); the firmware's picture is kept\n", name, reason, error);
		return;
	}

	/* Succeeded: the Type-C port's DisplayPort display is the output (i915_output_dp_ext made it so). */
	kern_logf("i915: display output: DP on the Type-C port, the firmware's output (%s)\n", name);
}

/*
 * Takes every connector's connection and preferred mode into the display
 * inventory once: an HDMI connector other than the resident
 * output's, and every DP connector, is detected first (it reads the sink's
 * EDID; a Type-C DP connector probes its sink over AUX and gives the
 * port's PHY back); the resident HDMI connector was detected
 * by its choice.  No scanout starts.
 */
static void
i915_output_inventory(
	struct i915_display *display)
{
	struct i915_hpd_summary summary;
	struct i915_hpd_output output;
	unsigned count;
	unsigned index;
	int status;
	int error;

	/* Nothing without the hotplug path. */
	if (!display->hpd_started)
		return;

	/* Each connector. */
	drv_i915_hpd_summary(display, &summary);
	count = drv_i915_hpd_output_count(display);
	for (index = 0U; index < count; index++) {
		/* An HDMI connector the resident choice did not detect is detected now. */
		error = drv_i915_hpd_output(display, index, &output);
		if (error != 0)
			continue;
		if (output.kind == I915_HPD_OUTPUT_HDMI && !(display->output.kind == I915_OUTPUT_KIND_HDMI && (int)index == summary.hdmi_connector)) {
			status = drv_i915_hpd_probe_connector(display, index);
			kern_logf("i915: display inventory: %s detected at the start (status %d), not lit\n", output.name, status);
		}

		/* A DP connector is detected now: a Type-C one probes its sink and releases the port. */
		if (output.kind == I915_HPD_OUTPUT_DP) {
			status = drv_i915_hpd_probe_connector(display, index);
			kern_logf("i915: display inventory: %s detected at the start (status %d), not lit\n", output.name, status);
		}

		/* Its state, the baseline of the first snapshot. */
		drv_i915_hpd_output_take(display, index);
	}
}

/* Reads display.mode=: 0 with the wanted size and rate, all 0 when it is not given, or EINVAL. */
static int
i915_output_wanted_mode(
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_hz)
{
	const struct kern_boot_parameters *parameters;
	const char *text;
	int error;

	/* Nothing is wanted without the parameter. */
	*width = 0U;
	*height = 0U;
	*refresh_hz = 0U;
	parameters = kern_boot_parameters_current();
	text = kern_boot_parameters_value(parameters, KERN_BOOT_PARAMETER_DISPLAY_MODE);
	if (text == NULL)
		return 0;

	/* The boot parser accepted the text already; it is read again here. */
	error = kern_boot_display_mode_parse(text, kern_strlen(text), width, height, refresh_hz);
	if (error != 0)
		return error;

	/* Succeeded: the wanted mode is read. */
	return 0;
}

/* Reports a mode's refresh rate, rounded to whole hertz. */
static uint32_t
i915_output_refresh_hz(
	const struct i915_display_mode *mode)
{
	uint64_t pixels;

	/* The pixels of one frame; an empty timing has no rate. */
	pixels = (uint64_t)mode->htotal * mode->vtotal;
	if (pixels == 0U)
		return 0U;

	/* The clock over the frame, rounded. */
	return (uint32_t)(((uint64_t)mode->clock_khz * 1000U + pixels / 2U) / pixels);
}

/* Reports whether a mode has the wanted size and, when one is wanted, its refresh rate. */
static int
i915_output_mode_matches(
	const struct i915_display_mode *mode,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz)
{
	uint32_t refresh;

	/* The size must be the same. */
	if (mode->hdisplay != width || mode->vdisplay != height)
		return 0;

	/* Any rate matches when none is wanted. */
	if (refresh_hz == 0U)
		return 1;

	/* The rate must be within a hertz of the wanted one. */
	refresh = i915_output_refresh_hz(mode);
	if (refresh + I915_OUTPUT_REFRESH_SLACK_HZ < refresh_hz)
		return 0;
	if (refresh > refresh_hz + I915_OUTPUT_REFRESH_SLACK_HZ)
		return 0;

	/* Reports a match. */
	return 1;
}

/*
 * Finds a detailed timing of the wanted size in the EDID: the base block's
 * four descriptors, then those of each CEA-861 extension.
 */
static int
i915_output_edid_find(
	const uint8_t *edid,
	unsigned edid_size,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz,
	struct i915_display_mode *mode)
{
	const uint8_t *block;
	unsigned offset;
	unsigned dtd_start;
	int error;

	/* No EDID has no timings. */
	if (edid == NULL || edid_size < I915_OUTPUT_EDID_BLOCK)
		return ENOENT;

	/* The base block's four descriptors. */
	error = i915_output_edid_block_find(edid, I915_OUTPUT_EDID_DTD_FIRST, I915_OUTPUT_EDID_DTD_FIRST + I915_OUTPUT_EDID_DTD_COUNT * I915_OUTPUT_EDID_DTD_SIZE, width, height, refresh_hz, mode);
	if (error == 0)
		return 0;

	/* The detailed timings of each CEA-861 extension, from the offset its byte 2 gives to the checksum. */
	for (offset = I915_OUTPUT_EDID_BLOCK; offset + I915_OUTPUT_EDID_BLOCK <= edid_size; offset += I915_OUTPUT_EDID_BLOCK) {
		/* Only a CEA-861 extension carries detailed timings here. */
		block = edid + offset;
		if (block[0] != I915_OUTPUT_EDID_CEA_TAG)
			continue;

		/* An extension whose offset points outside its block has none. */
		dtd_start = block[2];
		if (dtd_start < 4U || dtd_start >= I915_OUTPUT_EDID_BLOCK - 1U)
			continue;

		/* Its descriptors up to the checksum byte. */
		error = i915_output_edid_block_find(block, dtd_start, I915_OUTPUT_EDID_BLOCK - 1U, width, height, refresh_hz, mode);
		if (error == 0)
			return 0;
	}

	/* Reports that no timing of the EDID has the wanted size. */
	return ENOENT;
}

/* Finds a detailed timing of the wanted size among the descriptors of one EDID block from first up to last. */
static int
i915_output_edid_block_find(
	const uint8_t *block,
	unsigned first,
	unsigned last,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_hz,
	struct i915_display_mode *mode)
{
	struct i915_display_mode candidate;
	unsigned position;
	int matches;
	int error;

	/* Searches each whole descriptor of the range. */
	for (position = first; position + I915_OUTPUT_EDID_DTD_SIZE <= last; position += I915_OUTPUT_EDID_DTD_SIZE) {
		/* Decodes the descriptor; one that is no progressive timing is passed over. */
		error = drv_i915_output_edid_timing(block + position, &candidate);
		if (error != 0)
			continue;

		/* Takes the first timing of the wanted size. */
		matches = i915_output_mode_matches(&candidate, width, height, refresh_hz);
		if (matches) {
			*mode = candidate;
			return 0;
		}
	}

	/* Reports that no descriptor of the block has the wanted size. */
	return ENOENT;
}

/* Takes the EDID's screen size (centimetres, bytes 21 and 22) for a mode that has no image size of its own. */
static void
i915_output_edid_size(
	const uint8_t *edid,
	unsigned edid_size,
	struct i915_display_mode *mode)
{
	/* A mode with its own image size keeps it. */
	if (mode->width_mm != 0U && mode->height_mm != 0U)
		return;

	/* No EDID has no size. */
	if (edid == NULL || edid_size < I915_OUTPUT_EDID_BLOCK)
		return;

	/* The screen size, 0 when the sink does not state one. */
	mode->width_mm = (uint16_t)(edid[I915_OUTPUT_EDID_WIDTH_CM] * 10U);
	mode->height_mm = (uint16_t)(edid[I915_OUTPUT_EDID_HEIGHT_CM] * 10U);
}

/* Names the CVT vertical sync width of an aspect ratio: 4:3, 16:9, 16:10, 5:4 and 15:9 have their own. */
static uint32_t
i915_output_cvt_vsync(
	uint32_t width,
	uint32_t height)
{
	/* 4:3. */
	if (width * 3U == height * 4U)
		return 4U;

	/* 16:9. */
	if (width * 9U == height * 16U)
		return 5U;

	/* 16:10. */
	if (width * 10U == height * 16U)
		return 6U;

	/* 5:4 and 15:9. */
	if (width * 4U == height * 5U)
		return 7U;
	if (width * 9U == height * 15U)
		return 7U;

	/* Any other aspect ratio. */
	return 10U;
}

/*
 * Prepares an external DisplayPort display for the resident run:
 * a Type-C port's sink, probed again so the claim acts on
 * what is plugged in now, its mode (display.mode=, else the EDID's
 * preferred timing, else CEA format 4), and the link and TC PLL that
 * carry it under the port's link limits.  Nothing is written to the
 * hardware.  Returns 0 with output made that display, EOPNOTSUPP for a
 * port that is not a bound Type-C port, ENXIO when the sink is gone,
 * ENOSPC when no link carries the mode, or the error of a step; reason
 * says why.
 */
static int
i915_output_dp_ext(
	struct i915_display *display,
	unsigned connector,
	int port,
	struct i915_display_output *output,
	const char **reason)
{
	struct i915_display_mode mode;
	const struct i915_lcd_aux_emit *aux;
	enum i915_xd_dp_probe_status status;
	uint8_t edid[I915_XD_DP_PROBE_EDID_BLOCKS * I915_XD_DP_PROBE_EDID_BLOCK_SIZE];
	unsigned edid_bytes;
	uint32_t want_width;
	uint32_t want_height;
	uint32_t want_refresh;
	int tc_port;
	int max_rate;
	int max_lanes;
	int ref_khz;
	int error;

	/* Only a Type-C port's DisplayPort is lit (a combo PHY's is not). */
	tc_port = drv_i915_tc_port_of(display, port);
	if (tc_port < 0) {
		*reason = "a DisplayPort port that is not a Type-C port";
		return EOPNOTSUPP;
	}

	/* The port must be bound as an external DP port, with its own AUX channel. */
	aux = drv_i915_dp_ext_aux_emit(display, port);
	if (aux == NULL) {
		*reason = "the Type-C port is not bound as an external DP port";
		return EOPNOTSUPP;
	}

	/* Probes the sink again: the claim acts on what is plugged in now. */
	status = drv_i915_dp_ext_probe(display, port, edid, sizeof(edid), &edid_bytes);
	if (status != I915_XD_DP_PROBE_CONNECTED) {
		*reason = "the DisplayPort sink does not answer as connected";
		return ENXIO;
	}

	/* What the probe found, whole. */
	error = drv_i915_dp_ext_sink_copy(display, port, &output->sink);
	if (error != 0) {
		*reason = "the DisplayPort sink's probe could not be read";
		return ENXIO;
	}

	/* The mode display.mode= asks for, if any. */
	error = i915_output_wanted_mode(&want_width, &want_height, &want_refresh);
	if (error != 0) {
		*reason = "display.mode is unreadable";
		return error;
	}

	/* The mode to drive, from the sink's EDID. */
	error = drv_i915_output_pick_mode(want_width, want_height, want_refresh, edid, edid_bytes, &mode, &output->mode_source);
	if (error != 0) {
		*reason = "no timing serves display.mode";
		return error;
	}

	/* The port's link limits (lowered by a failed link training until the next long hot plug pulse). */
	error = drv_i915_dp_ext_link_limits(display, port, &max_rate, &max_lanes);
	if (error != 0) {
		*reason = "the Type-C port's link limits could not be read";
		return ENXIO;
	}

	/* The display's reference clock, or the platform's when the CDCLK state has none. */
	ref_khz = (int)display->cdclk.hw.ref;
	if (ref_khz <= 0)
		ref_khz = I915_OUTPUT_REF_KHZ;

	/* Plans the link and its M/N; the output's mode and link are written whole. */
	error = drv_i915_output_link_dp(display->xd.choice.platform, (unsigned)port, &mode, &output->sink, max_rate, max_lanes, (uint32_t)ref_khz, &output->link);
	output->mode = mode;
	if (error != 0) {
		*reason = "no DisplayPort link carries the mode";
		return error;
	}

	/* Logs the choice. */
	kern_logf("i915: resident display: DP on TC%d (DDI %c): %ux%u %d kHz (%s), link %u kHz x%u at %u bpp (limits %d kHz x%d)\n",
		  tc_port + 1,
		  (char)('A' + port),
		  mode.hdisplay,
		  mode.vdisplay,
		  mode.clock_khz,
		  output->mode_source,
		  output->link.rate_khz,
		  output->link.lanes,
		  output->link.bpp,
		  max_rate,
		  max_lanes);

	/* Succeeded: the output is that DisplayPort display. */
	output->kind = I915_OUTPUT_KIND_DP_EXT;
	output->has_connector = 1;
	output->connector = connector;
	output->tc_port = (unsigned)tc_port;
	output->port = port;
	output->pipe = I915_OUTPUT_OTHER_PIPE;
	return 0;
}

/* Names the panel's connector in output when the hotplug path has one (the first eDP connector). */
static void
i915_output_panel_connector(
	struct i915_display *display,
	struct i915_display_output *output)
{
	struct i915_hpd_output found;
	unsigned count;
	unsigned index;
	int error;

	/* Nothing without the hotplug path. */
	if (!display->hpd_started)
		return;

	/* The first eDP connector. */
	count = drv_i915_hpd_output_count(display);
	for (index = 0U; index < count; index++) {
		error = drv_i915_hpd_output(display, index, &found);
		if (error != 0 || found.kind != I915_HPD_OUTPUT_EDP)
			continue;
		output->has_connector = 1;
		output->connector = index;
		return;
	}
}

/* Copies a screen's plan of an output's link into the output's link, with the PLLs' reference clock. */
static void
i915_output_link_from_plan(
	const struct i915_xd_plan *plan,
	uint32_t ref_khz,
	struct i915_display_link *link)
{
	/* The link and its M/N. */
	kern_memset(link, 0, sizeof(*link));
	link->rate_khz = plan->rate_khz;
	link->lanes = plan->lanes;
	link->bpp = plan->bpp;
	link->tu = plan->m_n.tu;
	link->data_m = plan->m_n.data_m;
	link->data_n = plan->m_n.data_n;
	link->link_m = plan->m_n.link_m;
	link->link_n = plan->m_n.link_n;

	/* The reference clock and, on a combo PHY port, the DPLL's words. */
	link->ref_khz = ref_khz;
	if (plan->pll_set) {
		link->cfgcr0 = plan->pll.cfgcr0;
		link->cfgcr1 = plan->pll.cfgcr1;
		link->div0 = plan->pll.div0;
	}
}

/*
 * Reports the mode of the built-in panel: its size and refresh rate.
 *
 * The refresh rate is the pixel clock over the pixels of one frame, in
 * millihertz and rounded to nearest.  ENODEV when there is no panel or its
 * mode is empty.
 */
static int
i915_output_panel_mode(
	const struct i915_lcd_kernel_deps *d,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_millihz)
{
	const struct i915_display_mode *mode;
	uint64_t pixels;

	/* Refuses a node without the panel. */
	if (d == NULL)
		return ENODEV;
	if (d->edp == NULL)
		return ENODEV;

	/* The pixels of one frame, the blanks included. */
	mode = &d->edp->mode;
	pixels = (uint64_t)mode->htotal * mode->vtotal;

	/* Refuses a mode the panel's EDID or the VBT left empty. */
	if (mode->hdisplay == 0u)
		return ENODEV;
	if (mode->vdisplay == 0u)
		return ENODEV;
	if (pixels == 0u)
		return ENODEV;
	if (mode->clock_khz <= 0)
		return ENODEV;

	/* The visible size, and the frames a second in millihertz. */
	*width = mode->hdisplay;
	*height = mode->vdisplay;
	*refresh_millihz = (uint32_t)(((uint64_t)mode->clock_khz * 1000000ull + pixels / 2u) / pixels);

	/* Succeeded: the panel's mode is reported. */
	return 0;
}

/*
 * Reports the physical size of the built-in panel in millimetres; ENODEV
 * when there is no panel or its EDID gave no size.
 */
static int
i915_output_panel_size_mm(
	const struct i915_lcd_kernel_deps *d,
	uint32_t *width_mm,
	uint32_t *height_mm)
{
	/* Refuses a node without the panel, or a panel without a size. */
	if (d == NULL)
		return ENODEV;
	if (d->edp == NULL)
		return ENODEV;
	if (d->edp->mode.width_mm == 0u)
		return ENODEV;

	/* The size the panel's EDID reported. */
	*width_mm = d->edp->mode.width_mm;
	*height_mm = d->edp->mode.height_mm;

	/* Succeeded: the panel's size is reported. */
	return 0;
}

/*
 * Tells whether an HDMI display on a port can be lit: the port must be
 * one of the platform's combo PHY ports.  0, or EOPNOTSUPP.
 */
static int
i915_output_hdmi_port(
	const struct i915_display *display,
	int port)
{
	const struct i915_xd_port_desc *desc;

	/* A display without a description, or a port it does not drive. */
	if (display->xd.choice.platform == NULL || port < 0)
		return EOPNOTSUPP;

	/* The port's row of the platform's description. */
	desc = drv_i915_xd_platform_port(display->xd.choice.platform, (unsigned)port);
	if (desc == NULL)
		return EOPNOTSUPP;

	/* A Type-C port's HDMI is not lit. */
	if (desc->phy_kind != I915_XD_PHY_COMBO)
		return EOPNOTSUPP;

	/* Succeeded: the port can carry HDMI. */
	return 0;
}

/* Finds the hotplug path's connector of a kind on a port: its index, or -1. */
static int
i915_output_connector_of(
	struct i915_display *display,
	unsigned kind,
	int port)
{
	struct i915_hpd_output found;
	unsigned count;
	unsigned index;
	int error;

	/* Each connector of the hotplug path. */
	count = drv_i915_hpd_output_count(display);
	for (index = 0U; index < count; index++) {
		/* A connector of another kind or port. */
		error = drv_i915_hpd_output(display, index, &found);
		if (error != 0)
			continue;
		if (found.kind != kind || found.port != port)
			continue;

		/* Succeeded: the port's connector. */
		return (int)index;
	}

	/* No connector of that kind is on the port. */
	return -1;
}
