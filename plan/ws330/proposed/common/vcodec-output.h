/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output side of libmedia's software video decoders, the part both
 * codecs share: the queue of frames the decoded picture buffer has
 * output ("bumped", H.264 C.4.5.3, H.265 C.5.2.4) and not yet handed to
 * the caller, and the reordering of presentation times for a container
 * that gives times in decoding order.
 */

#ifndef LIBMEDIA_VCODEC_OUTPUT_H
#define LIBMEDIA_VCODEC_OUTPUT_H

#include "vcodec.h"
#include "vcodec-frame.h"

/*
 * The most frames one picture's steps can output: every frame of a full
 * buffer of 16, the current one, and a margin for frames that pass
 * through a full buffer (H.264 gap frames, H.265 generated pictures).
 */
#define VCODEC_OUTPUT_MAX 34U

/*
 * The frames output by the bumping process, oldest first, waiting for
 * the decoder to hand them out.  Each holds one frame reference.
 */
struct vcodec_output_queue {
	struct vcodec_frame *frame[VCODEC_OUTPUT_MAX];
	unsigned head;
	unsigned count;
};

/* Empties a queue without references (a new queue). */
void vcodec_output_init(struct vcodec_output_queue *queue);

/*
 * Appends a frame, adding a reference.  Returns 0, or ENOBUFS when the
 * queue holds VCODEC_OUTPUT_MAX frames (a decoder's mistake: it takes
 * the output before it decodes on).
 */
int vcodec_output_push(struct vcodec_output_queue *queue, struct vcodec_frame *frame);

/* Takes the oldest frame with the queue's reference (the caller unrefs it), or NULL when empty. */
struct vcodec_frame *vcodec_output_pop(struct vcodec_output_queue *queue);

/* Drops every frame's reference and empties the queue. */
void vcodec_output_clear(struct vcodec_output_queue *queue);

/*
 * Moves presentation times into output order for a container that gave
 * them in decoding order.
 *
 * taken is the frame being handed out; waiting[0..count) the frames that
 * will be output after it (the rest of the queue and the frames of the
 * decoded picture buffer still needed for output).  taken gets the
 * smallest time_us among itself and waiting; the frame that had that
 * time gets taken's time, so the set of times is kept.  NULL entries of
 * waiting are skipped.
 */
void vcodec_output_retime(struct vcodec_frame *taken, struct vcodec_frame *const *waiting, unsigned count);

#endif
