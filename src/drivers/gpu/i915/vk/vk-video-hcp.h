/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HCP command builder of an H.265 decode.
 *
 * vk-video.c resolves a decode -- its parameter sets, its picture, its
 * slice segments read from the bitstream, its references and the GPU
 * addresses of everything the decoder reads and writes -- into a struct
 * i915_video_hcp_decode, and the builder writes the Gen12 HCP commands of
 * that one picture into a batch.  The builder reads nothing else: it
 * checks nothing the checks before it decided and touches no object, so it
 * can be run and compared on the host.
 */

#ifndef DRIVERS_GPU_I915_VK_VK_VIDEO_HCP_H
#define DRIVERS_GPU_I915_VK_VK_VIDEO_HCP_H

#include "vk-batch.h"
#include "vk-video-h265.h"

#include <stdint.h>

/* The reference pictures the decoder addresses, and the session's line buffers. */
#define I915_VIDEO_HCP_REFERENCES	8U
#define I915_VIDEO_HCP_LINE_BUFFERS	9U

/* The most slice segments of a picture. */
#define I915_VIDEO_HCP_MAX_SLICES	256U

/*
 * The most dwords one decode writes: the commands before the slices (with
 * twenty quantizer matrices and the tile state, 610), the closing flushes
 * (10) and at most 136 a slice segment for 256 of them.
 */
#define I915_VIDEO_HCP_MAX_DWORDS	(620U + 256U * 136U)

/*
 * One reference picture the decoder addresses (its place in the decoder's
 * eight): the GPU addresses of its picture and of its slot's motion
 * vectors.
 */
struct i915_video_hcp_reference {
	uint64_t picture;
	uint64_t motion;
};

/*
 * One picture to decode, resolved: what the HCP commands of the picture are
 * built from.  It lives in the session's scratch; the sets it points to
 * are the parameters object's, the slices the session's.
 */
struct i915_video_hcp_decode {
	/* The picture's sets, its order count and its scaling matrices. */
	const struct i915_video_h265_sps *sps;
	const struct i915_video_h265_pps *pps;
	int32_t poc;
	struct i915_video_h265_matrices matrices;

	/* The output picture: its NV12 image's address (the Y plane), pitch and the rows of the Y plane. */
	uint64_t destination;
	uint32_t pitch;
	uint32_t chroma_rows;

	/* The session's line buffers and the motion vector buffer the picture writes. */
	uint64_t line_buffers[I915_VIDEO_HCP_LINE_BUFFERS];
	uint64_t motion_write;

	/*
	 * The references: the decoder's eight places (an unused one is the
	 * first used one, or the output), and for each DPB slot its place
	 * (I915_VIDEO_H265_NO_SLOT for none), its order count and whether it
	 * is long-term.
	 */
	uint32_t reference_count;
	struct i915_video_hcp_reference references[I915_VIDEO_HCP_REFERENCES];
	uint8_t place[I915_VIDEO_H265_SLOTS];
	struct i915_video_h265_slot slots[I915_VIDEO_H265_SLOTS];

	/*
	 * The bitstream: the 4 KiB page the decode's range starts in, the
	 * end of the buffer's bound range, and the bytes from the page to the
	 * range's start.
	 */
	uint64_t bitstream_base;
	uint64_t bitstream_end;
	uint32_t skew;

	/*
	 * The slice segments: each one's header as read, and where its NAL
	 * unit begins (after its start code) and ends, as byte offsets from
	 * the range's start.
	 */
	uint32_t slice_count;
	const struct i915_video_h265_slice *slices;
	const uint32_t *slice_starts;
	const uint32_t *slice_ends;

	/* The MOCS value of every buffer the commands name. */
	uint32_t mocs;
};

/*
 * Reports the bytes of a session's line buffer (index 0 to 8, in the order
 * of HCP_PIPE_BUF_ADDR_STATE) and of one picture's motion vectors for the
 * session's largest picture.
 */
uint64_t drv_i915_video_hcp_line_bytes(uint32_t index, uint32_t width, uint32_t height);
uint64_t drv_i915_video_hcp_motion_bytes(uint32_t width, uint32_t height);

void drv_i915_video_hcp_build(struct i915_gfx_batch *batch, const struct i915_video_hcp_decode *decode);

#endif
