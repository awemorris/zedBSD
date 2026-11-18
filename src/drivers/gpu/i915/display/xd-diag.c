/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's diagnostics of the display 12 and 13 code (see xd-diag.h).
 */

#include "xd-diag.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "xd-power.h"

#include "../intel/display-regs-xd.h"

#include <kern/klog.h>

#include <stddef.h>
#include <stdint.h>

/*
 * What the scan-out stop looks at, whatever the platform: the four pipes
 * every display 9 to 13 controller has, and the buffers of DDI A and B,
 * the ports a firmware lights a panel or a monitor on.
 */
#define I915_XD_DIAG_STOP_PIPES		4U
#define I915_XD_DIAG_STOP_DDIS		2U

/* How long the stop waits for a transcoder it turned off to report itself off. */
#define I915_XD_DIAG_STOP_WAIT_US	200000U

static int i915_xd_diag_pipe_powered(struct i915_xd_io *io, const struct i915_xd_platform *platform, unsigned pipe);

/*
 * Logs what the firmware left on, before the driver changes anything.
 *
 * Each pipe whose wells are on gets its transcoder, its DDI function and
 * its primary plane read; a pipe whose wells are off is only named,
 * because its registers would read nothing meaningful.  Then the buffer
 * of every port of the platform, the enable of every PLL, and the wells
 * that are on.  A device without a description is not read.
 */
void
drv_i915_xd_diag_firmware_left(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_pll_desc *pll;
	unsigned index;
	unsigned wells_on;
	uint32_t trans_conf;
	uint32_t ddi_function;
	uint32_t plane_ctl;
	uint32_t value;
	int powered;

	/* A device without a description has no pipes, ports or wells to name. */
	if (platform == NULL) {
		kern_logf("i915: display: firmware left: no description, nothing read\n");
		return;
	}

	/* Reads the transcoder, DDI function and primary plane of every pipe whose wells are on. */
	for (index = 0U; index < platform->pipes; index++) {
		/* A pipe whose wells are off is only named. */
		powered = i915_xd_diag_pipe_powered(io, platform, index);
		if (!powered) {
			kern_logf("i915: display: firmware left pipe %c: power off, not read\n", (char)('A' + index));
			continue;
		}

		/* Reads and names the pipe's words. */
		trans_conf = drv_i915_xd_io_read(io, I915_XD_TRANS_CONF(index));
		ddi_function = drv_i915_xd_io_read(io, I915_XD_TRANS_DDI_FUNC_CTL(index));
		plane_ctl = drv_i915_xd_io_read(io, I915_XD_PLANE_CTL(index));
		kern_logf("i915: display: firmware left pipe %c: TRANS_CONF=0x%08x TRANS_DDI_FUNC_CTL=0x%08x PLANE_CTL=0x%08x\n",
		    (char)('A' + index),
		    trans_conf,
		    ddi_function,
		    plane_ctl);
	}

	/* Reads and names the buffer control of every port. */
	for (index = 0U; index < platform->nports; index++) {
		port = &platform->ports[index];
		value = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(port->ddi));
		kern_logf("i915: display: firmware left DDI %c: DDI_BUF_CTL=0x%08x\n", (char)('A' + port->ddi), value);
	}

	/* Reads and names the enable of every PLL that has one. */
	for (index = 0U; index < platform->nplls; index++) {
		pll = &platform->plls[index];

		/* A PLL whose words live in its PHY has no enable register here. */
		if (pll->enable == 0U)
			continue;

		/* Logs the PLL's enable word. */
		value = drv_i915_xd_io_read(io, pll->enable);
		kern_logf("i915: display: firmware left PLL %u (0x%05x): ENABLE=0x%08x\n", index, pll->enable, value);
	}

	/* Counts and names the wells that are on. */
	wells_on = 0U;
	for (index = 0U; index < platform->nwells; index++) {
		powered = drv_i915_xd_power_well_powered(io, platform, index);
		if (powered) {
			wells_on++;
			kern_logf("i915: display: firmware left well %s on\n", platform->wells[index].name);
		}
	}

	/* Names the well totals. */
	kern_logf("i915: display: firmware left wells: total=%u on=%u\n", platform->nwells, wells_on);
}

/*
 * Stops every pipe and every DDI A or B buffer that is still on.
 *
 * This is the last resort at the end of the device, not the driver's
 * own stop: a pipe left running keeps reading memory after the driver has
 * let it go, and on a test host that passes the GPU to a guest it hangs
 * the IOMMU's unmap when the guest ends.  A pipe's plane goes off first,
 * armed through its surface, then its transcoder, whose state is waited
 * for a bounded time.  Every step is logged.  Returns how many pipes and
 * buffers were stopped.
 */
unsigned
drv_i915_xd_diag_stop_scanout(
	struct i915_xd_io *io)
{
	unsigned stopped;
	unsigned pipe;
	unsigned ddi;
	uint32_t trans_conf;
	uint32_t plane_ctl;
	uint32_t state;
	uint32_t buffer;
	int waited;

	/* Nothing stopped yet. */
	stopped = 0U;

	/* Stops every pipe whose transcoder or primary plane is still on. */
	for (pipe = 0U; pipe < I915_XD_DIAG_STOP_PIPES; pipe++) {
		/* Reads the pipe's transcoder and primary plane. */
		trans_conf = drv_i915_xd_io_read(io, I915_XD_TRANS_CONF(pipe));
		plane_ctl = drv_i915_xd_io_read(io, I915_XD_PLANE_CTL(pipe));

		/* A pipe with both off needs nothing. */
		if ((trans_conf & I915_XD_TRANS_CONF_ENABLE) == 0U && (plane_ctl & I915_XD_PLANE_CTL_ENABLE) == 0U)
			continue;

		/* Names the pipe found on. */
		kern_logf("i915: stop: pipe %c is still on (TRANS_CONF=0x%08x PLANE_CTL=0x%08x): stopping it so that nothing is scanned out when the driver is gone\n",
		    (char)('A' + pipe),
		    trans_conf,
		    plane_ctl);

		/* Turns the plane off, arms that through its surface, and turns the transcoder off. */
		drv_i915_xd_io_write(io, I915_XD_PLANE_CTL(pipe), plane_ctl & ~I915_XD_PLANE_CTL_ENABLE);
		drv_i915_xd_io_write(io, I915_XD_PLANE_SURF(pipe), 0U);
		drv_i915_xd_io_write(io, I915_XD_TRANS_CONF(pipe), trans_conf & ~I915_XD_TRANS_CONF_ENABLE);

		/* Waits a bounded time for the transcoder to report itself off. */
		state = 0U;
		waited = drv_i915_xd_io_poll(io, I915_XD_TRANS_CONF(pipe), I915_XD_TRANS_CONF_STATE, 0U, I915_XD_DIAG_STOP_WAIT_US, &state);

		/* Names what the pipe reads after the wait. */
		plane_ctl = drv_i915_xd_io_read(io, I915_XD_PLANE_CTL(pipe));
		kern_logf("i915: stop: pipe %c after the wait (%d): TRANS_CONF=0x%08x PLANE_CTL=0x%08x\n",
		    (char)('A' + pipe),
		    waited,
		    state,
		    plane_ctl);
		stopped++;
	}

	/* Disables the DDI A and B buffers that are still enabled. */
	for (ddi = 0U; ddi < I915_XD_DIAG_STOP_DDIS; ddi++) {
		/* Reads the buffer's control. */
		buffer = drv_i915_xd_io_read(io, I915_XD_DDI_BUF_CTL(ddi));

		/* A disabled buffer needs nothing. */
		if ((buffer & I915_XD_DDI_BUF_ENABLE) == 0U)
			continue;

		/* Disables the buffer and names it. */
		drv_i915_xd_io_write(io, I915_XD_DDI_BUF_CTL(ddi), buffer & ~I915_XD_DDI_BUF_ENABLE);
		kern_logf("i915: stop: DDI %c buffer was enabled (0x%08x): disabled\n", (char)('A' + ddi), buffer);
		stopped++;
	}

	/* Succeeded: reports how many pipes and buffers were stopped. */
	return stopped;
}

/* Tells whether every well of a pipe and of its transcoder is on. */
static int
i915_xd_diag_pipe_powered(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned pipe)
{
	uint64_t wells;
	unsigned well;
	int powered;

	/* The wells of the pipe and of its transcoder. */
	wells = drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_PIPE(pipe) | I915_XD_POWERS_TRANS(pipe));

	/* Each one of them must be on. */
	for (well = 0U; well < platform->nwells; well++) {
		/* A well that does not power the pipe is not asked. */
		if ((wells & (1ULL << well)) == 0ULL)
			continue;

		/* An off well leaves the pipe's registers unreadable. */
		powered = drv_i915_xd_power_well_powered(io, platform, well);
		if (!powered)
			return 0;
	}

	/* Succeeded: the pipe's registers read. */
	return 1;
}
