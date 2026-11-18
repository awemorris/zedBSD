/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs one transfer (a clear or a copy of a rectangle) on a session's
 * transfer context: its programs and command are built, kicked and
 * waited for.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_TRANSFER_JOB_H
#define KERN_DRIVERS_GPU_POWERVR_TRANSFER_JOB_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/transfer-program.h"

/*
 * One transfer: a clear of the destination's rectangle to a colour
 * (four float or integer bit patterns), a copy into it from a source
 * view whose corner the rectangle's corner reads, or a blit into it from
 * a source rectangle, scaled, mirrored along a flipped axis, and filtered
 * linearly when linear is set.  A copy with merge (a view of the
 * destination itself, of a packed depth and stencil) writes only the
 * bits of merge_mask, from the source's word shifted left by
 * merge_shift, and keeps the destination's other bits.  A copy of
 * extract_shift other than 0 writes each source word shifted right by
 * it (one aspect out of a packed depth and stencil).  A copy of samples
 * 4 is between two images of four samples a pixel, each sample from the
 * source's same sample.
 */
struct powervr_transfer_request {
	enum powervr_transfer_kind kind;
	struct powervr_render_surface destination;
	struct powervr_render_area area;
	uint32_t colour[4];
	const struct powervr_view_shape *source;
	uint32_t source_x;
	uint32_t source_y;
	uint32_t source_z;
	struct powervr_render_area source_area;
	bool flip_x;
	bool flip_y;
	bool linear;
	const struct powervr_view_shape *merge;
	uint32_t merge_shift;
	uint32_t merge_mask;
	uint32_t extract_shift;
	uint32_t samples;
};

/*
 * Runs a transfer, one row at a time when the destination's rows need
 * it; ETIMEDOUT when it did not finish in time, the core then may still
 * run it and its memory stays held.
 */
int powervr_transfer_run(struct powervr_render *render, struct powervr_session *session, const struct powervr_transfer_request *request);

/*
 * Runs a transfer of a command buffer: ordered after the session's work
 * by the firmware and left running (the session's last work is then the
 * transfer).  A transfer that needs several kicks waits for all but the
 * last.
 */
int powervr_transfer_run_async(struct powervr_render *render, struct powervr_session *session, const struct powervr_transfer_request *request);

/* Frees or keeps the memory of a session's transfers the core finished. */
void powervr_transfers_reap(struct powervr_render *render, struct powervr_session *session);

#endif
