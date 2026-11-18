/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The external outputs of a display 9 display (see xd9-outputs.h).
 *
 * A DDI's sink is found by the south display's live hotplug state, then
 * its DPCD over AUX for a DisplayPort sink, else its EDID over the GMBUS
 * for an HDMI one.  Lighting an output moves the display buffer: the
 * pipes already lit get their new, smaller parts first, each at its
 * vertical blank, and only then is the new pipe's part written;
 * darkening frees the pipe's part first and grows the others after it,
 * in the opposite order.
 *
 * Not done: the CD clock is never changed (a mode above it is not
 * chosen), MST, and a panel on an external DDI.
 */

#include "xd9-outputs.h"
#include "output.h"
#include "xd-aux.h"
#include "xd-clock.h"
#include "xd-dp.h"
#include "xd-edid.h"
#include "xd-gmbus.h"
#include "xd-hpd.h"
#include "xd-pipe.h"
#include "xd-power.h"
#include <kern/kcrt.h>
#include <kern/klog.h>

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd9.h"

#include <uapi/errno.h>
#include <stddef.h>

/*
 * The memory read latency PCODE leaves out of display 9's levels when
 * level 0 reads 0, the latency IPC adds on Kaby Lake and later, and the
 * bits of one level in PCODE's words; all in microseconds.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * watermark latencies, and the memory read latency workaround).
 */
#define I915_XD9_READ_LATENCY_US	2U
#define I915_XD9_IPC_LATENCY_US		4U
#define I915_XD9_LATENCY_BITS		8U
#define I915_XD9_LATENCY_MASK		0xffU
#define I915_XD9_LATENCY_PER_WORD	4U

/* The pixel clock (kHz) an HDMI port drives at most (no HDMI 2.0 scrambling). */
#define I915_XD9_OUTPUT_HDMI_KHZ	300000U

/* The bytes a pixel takes on the link: 24 bits. */
#define I915_XD9_OUTPUT_LINK_BYTES	3U
#define I915_XD9_OUTPUT_BPP		24U

/* The lowest DisplayPort rate, which a sink that names none takes. */
#define I915_XD9_OUTPUT_RBR_KHZ		162000U

/* The most lanes a DDI has. */
#define I915_XD9_OUTPUT_LANES		4U

/* The DPLLs an output may take (DPLL 0 is the CD clock's). */
#define I915_XD9_OUTPUT_DPLL_FIRST	1U
#define I915_XD9_OUTPUT_DPLL_LAST	3U

/* The EDID's detailed timings: the first, their size and count; a CEA-861 extension's tag; the extension count's byte. */
#define I915_XD9_EDID_DTD_FIRST		54U
#define I915_XD9_EDID_DTD_BYTES		18U
#define I915_XD9_EDID_DTDS		4U
#define I915_XD9_EDID_CEA_TAG		0x02U
#define I915_XD9_EDID_EXTENSIONS	126U

/* The largest side the pipe drives. */
#define I915_XD9_MODE_MAX_SIDE		4096U

/* The safe mode when the sink gives none: VESA DMT 1024x768 at 60 Hz. */
#define I915_XD9_SAFE_CLOCK_KHZ		65000

/* The EDID's header. */
static const uint8_t i915_xd9_edid_header[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

static int i915_xd9_outputs_probe(struct i915_xd9_outputs *outputs, struct i915_xd9_ext *output);
static void i915_xd9_outputs_edid(struct i915_xd9_outputs *outputs, struct i915_xd9_ext *output);
static int i915_xd9_outputs_edid_blocks(struct i915_xd9_outputs *outputs, struct i915_xd9_ext *output, unsigned blocks);
static void i915_xd9_outputs_mode(struct i915_xd9_outputs *outputs, struct i915_xd9_ext *output);
static int i915_xd9_outputs_config(struct i915_xd9_outputs *outputs, struct i915_xd9_ext *output);
static int i915_xd9_outputs_plan(const struct i915_xd9_outputs *outputs, const struct i915_xd9_ext *with, struct i915_xd9_pipe_plan plans[I915_XD9_PIPES]);
static int i915_xd9_mode_fits(const struct i915_display_mode *mode, uint32_t max_pixel_khz);
static void i915_xd9_safe_mode(struct i915_display_mode *mode);

/*
 * Decodes the memory latencies PCODE answered (four 8-bit levels in each
 * word, level 0 lowest) and adjusts them for display 9: the levels from
 * the first that reads 0 above level 0 are off; when level 0 reads 0, the
 * memory read latency PCODE left out goes on every usable level; level 0
 * takes 1 us more since the DIMMs are not known; and IPC lengthens every
 * usable level.
 */
void
drv_i915_xd9_wm_latency(
	uint32_t low,
	uint32_t high,
	int ipc,
	uint16_t latency[I915_XD_WM_LEVELS])
{
	unsigned levels;
	unsigned level;
	uint32_t word;

	/* Each level from its word. */
	for (level = 0U; level < I915_XD_WM_LEVELS; level++) {
		word = low;
		if (level >= I915_XD9_LATENCY_PER_WORD)
			word = high;
		latency[level] = (uint16_t)((word >> ((level % I915_XD9_LATENCY_PER_WORD) * I915_XD9_LATENCY_BITS)) & I915_XD9_LATENCY_MASK);
	}

	/* The usable levels end at the first one above level 0 that reads 0. */
	levels = I915_XD_WM_LEVELS;
	for (level = 1U; level < I915_XD_WM_LEVELS; level++) {
		if (latency[level] == 0U) {
			levels = level;
			break;
		}
	}
	for (level = levels; level < I915_XD_WM_LEVELS; level++)
		latency[level] = 0U;

	/* Level 0 at 0: the memory read latency on every usable level. */
	if (latency[0] == 0U) {
		for (level = 0U; level < levels; level++)
			latency[level] = (uint16_t)(latency[level] + I915_XD9_READ_LATENCY_US);
	}

	/* Level 0 for DIMMs that are not known. */
	latency[0] = (uint16_t)(latency[0] + 1U);

	/* Without IPC, nothing more. */
	if (!ipc)
		return;

	/* Succeeded: IPC on every level that is used. */
	for (level = 0U; level < I915_XD_WM_LEVELS; level++) {
		if (latency[level] != 0U)
			latency[level] = (uint16_t)(latency[level] + I915_XD9_IPC_LATENCY_US);
	}
}

/*
 * Picks the mode an output shows from its EDID: the first detailed timing
 * the pipe drives within the pixel clock (the preferred one comes
 * first), from the base block and then a CEA-861 extension; without an
 * EDID or a timing that fits, 1024x768 at 60 Hz.  Returns 0, or EINVAL
 * when even that does not fit.
 */
int
drv_i915_xd9_pick_mode(
	const uint8_t *edid,
	unsigned edid_bytes,
	uint32_t max_pixel_khz,
	struct i915_display_mode *mode)
{
	struct i915_display_mode candidate;
	const uint8_t *extension;
	unsigned index;
	unsigned offset;
	unsigned first;
	int fits;
	int error;

	/* The base block's descriptors in order. */
	if (edid != NULL && edid_bytes >= I915_XD_EDID_BLOCK_BYTES) {
		for (index = 0U; index < I915_XD9_EDID_DTDS; index++) {
			error = drv_i915_output_edid_timing(edid + I915_XD9_EDID_DTD_FIRST + index * I915_XD9_EDID_DTD_BYTES, &candidate);
			if (error != 0)
				continue;
			fits = i915_xd9_mode_fits(&candidate, max_pixel_khz);
			if (fits) {
				*mode = candidate;
				return 0;
			}
		}
	}

	/* A CEA-861 extension's descriptors, from the offset its third byte names. */
	if (edid != NULL && edid_bytes >= 2U * I915_XD_EDID_BLOCK_BYTES && edid[I915_XD_EDID_BLOCK_BYTES] == I915_XD9_EDID_CEA_TAG) {
		extension = edid + I915_XD_EDID_BLOCK_BYTES;
		first = extension[2];
		for (offset = first; first >= 4U && offset + I915_XD9_EDID_DTD_BYTES <= I915_XD_EDID_BLOCK_BYTES - 1U; offset += I915_XD9_EDID_DTD_BYTES) {
			error = drv_i915_output_edid_timing(extension + offset, &candidate);
			if (error != 0)
				break;
			fits = i915_xd9_mode_fits(&candidate, max_pixel_khz);
			if (fits) {
				*mode = candidate;
				return 0;
			}
		}
	}

	/* The safe mode, when the pipe drives it. */
	i915_xd9_safe_mode(&candidate);
	fits = i915_xd9_mode_fits(&candidate, max_pixel_khz);
	if (!fits)
		return EINVAL;

	/* Succeeded: the safe mode. */
	*mode = candidate;
	return 0;
}

/*
 * Sets the outputs up beside the panel: each DDI, the DDIs the board has
 * but the panel's, and their hotplug sense on.
 */
void
drv_i915_xd9_outputs_init(
	struct i915_xd9_outputs *outputs,
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd9_panel *panel,
	uint32_t cdclk_khz,
	const uint16_t latency[I915_XD_WM_LEVELS])
{
	unsigned index;
	unsigned present;

	/* What the outputs share the display with. */
	kern_memset(outputs, 0, sizeof(*outputs));
	outputs->io = io;
	outputs->platform = platform;
	outputs->panel = *panel;
	outputs->cdclk_khz = cdclk_khz;
	kern_memcpy(outputs->latency, latency, sizeof(outputs->latency));

	/* Each external DDI. */
	for (index = 0U; index < I915_XD9_OUTPUTS; index++)
		outputs->output[index].ddi = index + I915_XD9_HPD_FIRST_DDI;

	/* Succeeded: the DDIs the board has, but the panel's, and their sense. */
	present = drv_i915_xd9_hpd_present(io);
	outputs->present = present & ~(1U << panel->ddi);
	drv_i915_xd9_hpd_enable(io);
}

/*
 * Reads which DDIs have a sink and probes those that changed.  Returns
 * the DDIs (bit per DDI) whose attachment changed; a lit output whose
 * sink went stays lit for the caller to darken.
 */
unsigned
drv_i915_xd9_outputs_poll(
	struct i915_xd9_outputs *outputs)
{
	struct i915_xd9_ext *output;
	unsigned connected;
	unsigned changed;
	unsigned bit;
	unsigned index;
	int error;

	/* The live state of the DDIs the board has. */
	connected = drv_i915_xd9_hpd_connected(outputs->io) & outputs->present;
	changed = connected ^ outputs->connected;

	/* Each DDI whose sink came or went. */
	for (index = 0U; index < I915_XD9_OUTPUTS; index++) {
		output = &outputs->output[index];
		bit = 1U << output->ddi;
		if ((changed & bit) == 0U)
			continue;

		/* A sink that went. */
		if ((connected & bit) == 0U) {
			output->connected = 0;
			output->kind = 0U;
			continue;
		}

		/* A sink that came and could not be probed counts as absent. */
		error = i915_xd9_outputs_probe(outputs, output);
		if (error != 0) {
			kern_logf("i915: xd9-outputs: DDI %c has a sink that could not be probed: %d\n", 'A' + (int)output->ddi, error);
			connected &= ~bit;
			changed &= ~bit;
			continue;
		}

		/* A new attachment. */
		outputs->generation++;
		output->generation = outputs->generation;
		output->connected = 1;
	}

	/* Succeeded: the DDIs that changed. */
	outputs->connected = connected;
	return changed;
}

/*
 * Lights an output on a free pipe and DPLL: the pipes already lit get
 * their new parts of the display buffer in ascending order, then the new
 * pipe's, then the output.  Returns 0, ENOENT for a DDI without a sink,
 * EBUSY for one lit already, ENOSPC when no pipe, DPLL or buffer space is
 * left, or the output's error (with the parts given back).
 */
int
drv_i915_xd9_outputs_light(
	struct i915_xd9_outputs *outputs,
	unsigned index,
	uint32_t surf,
	uint32_t stride_units,
	int rgbx)
{
	struct i915_xd9_pipe_plan plans[I915_XD9_PIPES];
	struct i915_xd9_pipe_plan empty;
	struct i915_xd9_ext *output;
	int error;

	/* A sink that is there, and not lit yet. */
	output = &outputs->output[index];
	if (!output->connected)
		return ENOENT;
	if (output->lit)
		return EBUSY;

	/* The pipe, the DPLL, the link and the clocks. */
	error = i915_xd9_outputs_config(outputs, output);
	if (error != 0)
		return error;
	output->out.surf = surf;
	output->out.stride_units = stride_units;
	output->out.rgbx = rgbx;

	/* The display buffer with the new pipe in it. */
	error = i915_xd9_outputs_plan(outputs, output, plans);
	if (error != 0)
		return error;

	/* The lit pipes shrink first, then the new pipe's part, then the output. */
	drv_i915_xd9_wm_arm_pipes(outputs->io, plans, I915_XD9_PIPES, 1, output->out.pipe);
	drv_i915_xd9_wm_write_pipe(outputs->io, output->out.pipe, &plans[output->out.pipe]);
	error = drv_i915_xd9_output_on(outputs->io, outputs->platform, &output->out, &output->dp);
	if (error == 0) {
		output->lit = 1;
		kern_logf("i915: xd9-outputs: DDI %c lit on pipe %c, DPLL%u, %ux%u at %u kHz\n",
		    'A' + (int)output->ddi,
		    'A' + (int)output->out.pipe,
		    output->out.dpll,
		    (unsigned)output->mode.hdisplay,
		    (unsigned)output->mode.vdisplay,
		    (unsigned)output->mode.clock_khz);
		return 0;
	}

	/* The output did not light: its part goes, and the others grow back. */
	kern_memset(&empty, 0, sizeof(empty));
	drv_i915_xd9_wm_write_pipe(outputs->io, output->out.pipe, &empty);
	(void)i915_xd9_outputs_plan(outputs, NULL, plans);
	drv_i915_xd9_wm_arm_pipes(outputs->io, plans, I915_XD9_PIPES, 0, output->out.pipe);
	kern_logf("i915: xd9-outputs: DDI %c could not be lit: %d\n", 'A' + (int)output->ddi, error);

	/* Succeeded in learning that the output stays dark. */
	return error;
}

/*
 * Darkens a lit output: the output off, its pipe's part freed (its plane
 * is off: the surface write arms it at once), then the other pipes grow
 * in descending order.
 */
void
drv_i915_xd9_outputs_darken(
	struct i915_xd9_outputs *outputs,
	unsigned index)
{
	struct i915_xd9_pipe_plan plans[I915_XD9_PIPES];
	struct i915_xd9_pipe_plan empty;
	struct i915_xd9_ext *output;
	unsigned pipe;

	/* Nothing to darken. */
	output = &outputs->output[index];
	if (!output->lit)
		return;

	/* The output off, and its pipe's part freed. */
	drv_i915_xd9_output_off(outputs->io, outputs->platform, &output->out, &output->dp);
	output->lit = 0;
	pipe = output->out.pipe;
	kern_memset(&empty, 0, sizeof(empty));
	drv_i915_xd9_wm_write_pipe(outputs->io, pipe, &empty);
	drv_i915_xd_io_write(outputs->io, I915_XD9_PLANE_SURF(pipe), 0U);

	/* The others grow into it. */
	(void)i915_xd9_outputs_plan(outputs, NULL, plans);
	drv_i915_xd9_wm_arm_pipes(outputs->io, plans, I915_XD9_PIPES, 0, pipe);

	/* Succeeded: the output is dark. */
	kern_logf("i915: xd9-outputs: DDI %c dark\n", 'A' + (int)output->ddi);
}

/* Points a lit output's plane at a surface, which takes effect at its next vertical blank. */
void
drv_i915_xd9_outputs_flip(
	struct i915_xd9_outputs *outputs,
	unsigned index,
	uint32_t surf)
{
	unsigned pipe;

	/* Succeeded: the surface, which arms the plane. */
	pipe = outputs->output[index].out.pipe;
	drv_i915_xd_io_write(outputs->io, I915_XD9_PLANE_SURF(pipe), surf);
	drv_i915_xd_io_posting_read(outputs->io, I915_XD9_PLANE_SURF(pipe));
}

/* Reports the surface a lit output's plane scans out now. */
uint32_t
drv_i915_xd9_outputs_live(
	struct i915_xd9_outputs *outputs,
	unsigned index)
{
	uint32_t live;

	/* The output's plane's live surface. */
	live = drv_i915_xd9_plane_surf_live(outputs->io, outputs->output[index].out.pipe);

	/* Succeeded: the surface. */
	return live;
}

/* Reports an output's mode's refresh in millihertz, or 0 without a mode. */
uint32_t
drv_i915_xd9_ext_refresh(
	const struct i915_xd9_ext *output)
{
	uint64_t frame;

	/* A mode with a frame. */
	frame = (uint64_t)output->mode.htotal * output->mode.vtotal;
	if (frame == 0U || output->mode.clock_khz <= 0)
		return 0U;

	/* Succeeded: pixels a second over pixels a frame, rounded. */
	return (uint32_t)(((uint64_t)output->mode.clock_khz * 1000000U + frame / 2U) / frame);
}

/*
 * Finds what a DDI's sink is: a DisplayPort sink answers its DPCD over
 * AUX; else an HDMI one, whose EDID comes over the GMBUS.  Power well 2
 * (the AUX channels and the GMBUS of DDIs B to D) goes up first and
 * stays up.  Returns 0, or the error of the well or the AUX channel.
 */
static int
i915_xd9_outputs_probe(
	struct i915_xd9_outputs *outputs,
	struct i915_xd9_ext *output)
{
	unsigned ddi;
	unsigned lanes;
	int error;

	/* Nothing from an attachment before. */
	ddi = output->ddi;
	kern_memset(output, 0, sizeof(*output));
	output->ddi = ddi;

	/* The AUX channels and the GMBUS. */
	error = drv_i915_xd9_power_pw2_on(outputs->io, outputs->platform);
	if (error != 0)
		return error;

	/* The DDI's AUX channel. */
	error = drv_i915_xd_aux_init(&output->dp.aux, ddi);
	if (error != 0)
		return error;
	output->dp.io = outputs->io;
	output->dp.ddi = ddi;

	/* A DisplayPort sink: its capabilities and link limits; else HDMI. */
	error = drv_i915_xd_dpcd_read(&output->dp.aux, outputs->io, I915_XD_DPCD_REV, output->dp.caps, sizeof(output->dp.caps));
	if (error == 0 && output->dp.caps[I915_XD_DPCD_REV] != 0U) {
		output->kind = I915_XD9_OUTPUT_DP;
		output->sink_max_khz = drv_i915_xd9_dp_sink_rate(output->dp.caps[I915_XD_DPCD_MAX_RATE]);
		if (output->sink_max_khz == 0U)
			output->sink_max_khz = I915_XD9_OUTPUT_RBR_KHZ;
		lanes = output->dp.caps[I915_XD_DPCD_MAX_LANES] & I915_XD_DPCD_LANES_MASK;
		if (lanes == 0U || lanes > I915_XD9_OUTPUT_LANES)
			lanes = I915_XD9_OUTPUT_LANES;
		output->sink_lanes = lanes;
	} else {
		output->kind = I915_XD9_OUTPUT_HDMI;
	}

	/* Its EDID, and the mode. */
	i915_xd9_outputs_edid(outputs, output);
	i915_xd9_outputs_mode(outputs, output);
	kern_logf("i915: xd9-outputs: DDI %c: %s sink, EDID %u bytes, %ux%u at %u kHz\n",
	    'A' + (int)ddi,
	    (output->kind == I915_XD9_OUTPUT_DP) ? "DP" : "HDMI",
	    output->edid_bytes,
	    (unsigned)output->mode.hdisplay,
	    (unsigned)output->mode.vdisplay,
	    (unsigned)output->mode.clock_khz);

	/* Succeeded: the sink is known. */
	return 0;
}

/*
 * Reads a sink's EDID: the base block, then the first extension when the
 * base names one.  A block that cannot be read, or a base without the
 * header, leaves no EDID.
 */
static void
i915_xd9_outputs_edid(
	struct i915_xd9_outputs *outputs,
	struct i915_xd9_ext *output)
{
	int differs;
	int error;

	/* The base block with its header. */
	error = i915_xd9_outputs_edid_blocks(outputs, output, 1U);
	if (error != 0)
		return;
	differs = kern_memcmp(output->edid, i915_xd9_edid_header, sizeof(i915_xd9_edid_header));
	if (differs != 0)
		return;
	output->edid_bytes = I915_XD_EDID_BLOCK_BYTES;

	/* The first extension, when the base names one. */
	if (output->edid[I915_XD9_EDID_EXTENSIONS] == 0U)
		return;
	error = i915_xd9_outputs_edid_blocks(outputs, output, 2U);
	if (error != 0)
		return;

	/* Succeeded: both blocks. */
	output->edid_bytes = 2U * I915_XD_EDID_BLOCK_BYTES;
}

/* Reads the first EDID blocks of a sink, over AUX or the GMBUS: 0, or the error. */
static int
i915_xd9_outputs_edid_blocks(
	struct i915_xd9_outputs *outputs,
	struct i915_xd9_ext *output,
	unsigned blocks)
{
	struct i915_xd_gmbus gmbus;
	int error;

	/* A DisplayPort sink over I2C-over-AUX. */
	if (output->kind == I915_XD9_OUTPUT_DP) {
		error = drv_i915_xd_aux_edid_blocks(&output->dp.aux, outputs->io, output->edid, blocks);
		if (error != 0)
			return error;
		return 0;
	}

	/* An HDMI sink over the GMBUS on the DDI's pin. */
	drv_i915_xd_gmbus_init(&gmbus);
	error = drv_i915_xd9_gmbus_edid_blocks(&gmbus, outputs->io, drv_i915_xd9_gmbus_pin(output->ddi), output->edid, blocks);
	if (error != 0)
		return error;

	/* Succeeded: the blocks are read. */
	return 0;
}

/*
 * Chooses the mode an output shows: the first timing of its EDID the CD
 * clock, the port and the link carry and the display window holds, else
 * 1024x768.
 */
static void
i915_xd9_outputs_mode(
	struct i915_xd9_outputs *outputs,
	struct i915_xd9_ext *output)
{
	const uint8_t *edid;
	uint32_t limit;
	uint32_t link;
	int error;

	/* The CD clock, and the port's or the link's limit. */
	limit = outputs->cdclk_khz;
	if (output->kind == I915_XD9_OUTPUT_HDMI && limit > I915_XD9_OUTPUT_HDMI_KHZ)
		limit = I915_XD9_OUTPUT_HDMI_KHZ;
	if (output->kind == I915_XD9_OUTPUT_DP) {
		link = output->sink_max_khz * output->sink_lanes / I915_XD9_OUTPUT_LINK_BYTES;
		if (limit > link)
			limit = link;
	}

	/* The EDID's first timing that fits. */
	edid = NULL;
	if (output->edid_bytes != 0U)
		edid = output->edid;
	error = drv_i915_xd9_pick_mode(edid, output->edid_bytes, limit, &output->mode);
	if (error != 0)
		kern_memset(&output->mode, 0, sizeof(output->mode));

	/* Larger than the display window holds: the safe mode. */
	if (output->mode.hdisplay <= I915_XD9_OUTPUT_MAX_WIDTH && output->mode.vdisplay <= I915_XD9_OUTPUT_MAX_HEIGHT)
		return;
	error = drv_i915_xd9_pick_mode(NULL, 0U, limit, &output->mode);
	if (error != 0)
		kern_memset(&output->mode, 0, sizeof(output->mode));
}

/*
 * Works out how an output is lit: a pipe neither the panel nor another
 * lit output uses (its transcoder has the same number), a DPLL from 1 to
 * 3 neither uses, the DisplayPort link or the HDMI clock with the DPLL's
 * words, and the DDI's translation rows.  Returns 0, or ENOSPC.
 */
static int
i915_xd9_outputs_config(
	struct i915_xd9_outputs *outputs,
	struct i915_xd9_ext *output)
{
	const struct i915_xd9_buf_trans *table;
	struct i915_xd_dp_choice choice;
	struct i915_xd9_output *out;
	unsigned pipes_used;
	unsigned dplls_used;
	unsigned index;
	unsigned pipe;
	unsigned dpll;
	unsigned count;
	unsigned row;
	int error;

	/* What the panel and the lit outputs use (DPLL 0 is the CD clock's). */
	pipes_used = 1U << outputs->panel.pipe;
	dplls_used = 1U | (1U << outputs->panel.dpll);
	for (index = 0U; index < I915_XD9_OUTPUTS; index++) {
		if (!outputs->output[index].lit)
			continue;
		pipes_used |= 1U << outputs->output[index].out.pipe;
		dplls_used |= 1U << outputs->output[index].out.dpll;
	}

	/* The first free pipe and DPLL. */
	for (pipe = 0U; pipe < I915_XD9_PIPES; pipe++) {
		if ((pipes_used & (1U << pipe)) == 0U)
			break;
	}
	for (dpll = I915_XD9_OUTPUT_DPLL_FIRST; dpll <= I915_XD9_OUTPUT_DPLL_LAST; dpll++) {
		if ((dplls_used & (1U << dpll)) == 0U)
			break;
	}

	/* None left, or no mode. */
	if (pipe == I915_XD9_PIPES || dpll > I915_XD9_OUTPUT_DPLL_LAST || output->mode.clock_khz <= 0)
		return ENOSPC;

	/* The pipe, the DPLL and the mode. */
	out = &output->out;
	kern_memset(out, 0, sizeof(*out));
	out->hdmi = output->kind == I915_XD9_OUTPUT_HDMI;
	out->ddi = output->ddi;
	out->pipe = pipe;
	out->dpll = dpll;
	out->mode = output->mode;

	/* HDMI: the WRPLL at the pixel clock, and the table's HDMI row. */
	if (out->hdmi) {
		error = drv_i915_xd9_dpll_hdmi_words((uint32_t)output->mode.clock_khz, &out->dpll_words);
		if (error != 0)
			return ENOSPC;
		table = drv_i915_xd9_ddi_buf_table(outputs->platform, I915_XD9_OUTPUT_HDMI, output->ddi, &count);
		if (table == NULL || count == 0U)
			return ENOSPC;
		row = drv_i915_xd9_ddi_hdmi_row(outputs->platform);
		if (row >= count)
			row = count - 1U;
		out->hdmi_row = &table[row];
		return 0;
	}

	/* DisplayPort: the slowest link that carries the mode, its M/N and its rate's DPLL words. */
	error = drv_i915_xd9_dp_link(output->sink_max_khz, output->sink_lanes, (uint32_t)output->mode.clock_khz, &choice, &out->m_n);
	if (error != 0)
		return ENOSPC;
	out->rate_khz = choice.rate_khz;
	out->lanes = choice.lanes;
	error = drv_i915_xd9_dpll_dp_words(choice.rate_khz, &out->dpll_words);
	if (error != 0)
		return ENOSPC;

	/* Succeeded: the DDI's translation rows. */
	output->dp.table = drv_i915_xd9_ddi_buf_table(outputs->platform, I915_XD9_OUTPUT_DP, output->ddi, &count);
	output->dp.rows = count;
	return 0;
}

/*
 * Plans the display buffer between the panel's pipe, the lit outputs'
 * and the one about to be lit (with, or NULL).  Returns 0, or ENOSPC when
 * a pipe's share does not take even level 0.
 */
static int
i915_xd9_outputs_plan(
	const struct i915_xd9_outputs *outputs,
	const struct i915_xd9_ext *with,
	struct i915_xd9_pipe_plan plans[I915_XD9_PIPES])
{
	const struct i915_xd9_ext *output;
	struct i915_xd9_pipe_plan *plan;
	unsigned index;
	int error;

	/* The panel. */
	kern_memset(plans, 0, I915_XD9_PIPES * sizeof(plans[0]));
	plan = &plans[outputs->panel.pipe];
	plan->active = 1;
	plan->hdisplay = outputs->panel.hdisplay;
	plan->htotal = outputs->panel.htotal;
	plan->pixel_khz = outputs->panel.pixel_khz;
	plan->plane_width = outputs->panel.hdisplay;

	/* Each output lit or being lit. */
	for (index = 0U; index < I915_XD9_OUTPUTS; index++) {
		output = &outputs->output[index];
		if (!output->lit && output != with)
			continue;
		plan = &plans[output->out.pipe];
		plan->active = 1;
		plan->hdisplay = output->mode.hdisplay;
		plan->htotal = output->mode.htotal;
		plan->pixel_khz = (uint32_t)output->mode.clock_khz;
		plan->plane_width = output->mode.hdisplay;
	}

	/* The xd code's plan; a pipe it did not reach keeps no part. */
	error = drv_i915_xd9_dbuf_plan(outputs->platform, outputs->latency, plans, I915_XD9_PIPES);
	if (error != 0)
		return error;

	/* Succeeded: every lit pipe has its part. */
	return 0;
}

/* Reports nonzero when a mode has a clock within the limit and a size the plane and the buffers take. */
static int
i915_xd9_mode_fits(
	const struct i915_display_mode *mode,
	uint32_t max_pixel_khz)
{
	/* A clock above the limit, or none. */
	if (mode->clock_khz <= 0 || (uint32_t)mode->clock_khz > max_pixel_khz)
		return 0;

	/* No size, or too large a one. */
	if (mode->hdisplay == 0U || mode->vdisplay == 0U)
		return 0;
	if (mode->hdisplay > I915_XD9_MODE_MAX_SIDE || mode->vdisplay > I915_XD9_MODE_MAX_SIDE)
		return 0;

	/* Succeeded: the pipe drives it. */
	return 1;
}

/*
 * Fills the safe mode: VESA DMT 1024x768 at 60 Hz with negative syncs.
 *
 * Values referenced: VESA Display Monitor Timing Standard (1024x768 at
 * 60 Hz).
 */
static void
i915_xd9_safe_mode(
	struct i915_display_mode *mode)
{
	/* Succeeded: the timing. */
	kern_memset(mode, 0, sizeof(*mode));
	mode->clock_khz = I915_XD9_SAFE_CLOCK_KHZ;
	mode->hdisplay = 1024U;
	mode->hsync_start = 1048U;
	mode->hsync_end = 1184U;
	mode->htotal = 1344U;
	mode->vdisplay = 768U;
	mode->vsync_start = 771U;
	mode->vsync_end = 777U;
	mode->vtotal = 806U;
	mode->edid_bpc = 8;
}
