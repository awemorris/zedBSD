/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's diagnostics of the display 12 and 13 code (xd-diag.c).
 *
 * Two reads that need nothing but the register window and a platform's
 * description: the log of what the firmware left on before the driver
 * changes anything (the pipes whose wells are on, the ports' buffers, the
 * PLLs, the wells), and the stop of anything still scanned out when the
 * device goes away, which a pipe left running would otherwise keep doing
 * after the driver has let its memory go.  The start's record of the
 * display writes, which i915.debug=display prints, is the register
 * window's own (drv_i915_xd_io_log_records()).
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_DIAG_H
#define DRIVERS_GPU_I915_DISPLAY_XD_DIAG_H

#include "xd-io.h"
#include "xd-platform.h"

/* Logs the pipes, port buffers, PLLs and wells the firmware left on; writes nothing. */
void drv_i915_xd_diag_firmware_left(struct i915_xd_io *io, const struct i915_xd_platform *platform);

/* Stops every pipe and every DDI A or B buffer still on; returns how many were stopped. */
unsigned drv_i915_xd_diag_stop_scanout(struct i915_xd_io *io);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_DIAG_H */
