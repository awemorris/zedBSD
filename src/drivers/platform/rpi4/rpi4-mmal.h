/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VideoCore firmware's H.264 decoder (MMAL's ril.video_decode), driven
 * over VCHIQ.
 *
 * A decoder is fed whole access units and hands back decoded NV12 frames.
 * The client is synchronous and simple: a caller fills one of the decoder's
 * input staging areas, sends it (several units may be in flight, each in
 * an area of its own), and receives frames in the order they came into the
 * decoder's own bounce memory, which is cached (the client keeps it
 * coherent with the VPU), so the caller reads a frame quickly.  A received
 * frame's buffer is the caller's until it releases it.  Every bulk
 * transfer's memory stays the decoder's until the firmware has completed
 * it.
 */

#ifndef KERN_DRIVERS_RPI4_MMAL_H
#define KERN_DRIVERS_RPI4_MMAL_H

#include <stdbool.h>
#include <stdint.h>

/* The frame flags a receive reports (MMAL's buffer flags). */
#define DRV_RPI4_MMAL_FRAME_EOS		0x0001U
#define DRV_RPI4_MMAL_FRAME_CORRUPTED	0x0200U

/* A frame's time stamp when the firmware gave none, and the encoding of an NV12 frame. */
#define DRV_RPI4_MMAL_FRAME_NO_TIME	((int64_t)((uint64_t)1U << 63))
#define DRV_RPI4_MMAL_FRAME_NV12	0x3231564eU

/* The decodes whose frames a decoder may have targets for at once. */
#define DRV_RPI4_MMAL_TARGETS		8U

struct drv_rpi4_mmal_decoder;

/*
 * Where the frame of one decode goes instead of the decoder's bounce memory.
 *
 * physical is a contiguous run below 1 GiB of bytes bytes that the CPU
 * reaches only uncached (the decoder neither cleans nor invalidates it).
 * The frame goes there only when its layout is the target's: the luma rows
 * pitch bytes apart from the run's start and the chroma plane at chroma;
 * else it comes into the bounce memory as any frame.
 */
struct drv_rpi4_mmal_target {
	int64_t pts;
	uint64_t physical;
	uint32_t bytes;
	uint32_t pitch;
	uint32_t chroma;
};

/*
 * One decoded frame: whether it went to its target rather than the bounce
 * memory, the handle that releases it, its time stamp and flags, the bytes
 * received, and the layout of the planes in them.
 */
struct drv_rpi4_mmal_frame {
	bool direct;
	unsigned handle;
	int64_t pts;
	uint32_t flags;
	uint32_t length;
	uint32_t encoding;
	uint32_t width;
	uint32_t height;
	uint32_t crop_width;
	uint32_t crop_height;
	uint32_t planes;
	uint32_t plane_offset[2];
	uint32_t plane_pitch[2];
};

/*
 * Creates a decoder for H.264 pictures of up to width by height samples.
 *
 * input_bytes is the size of each input staging area, the largest access
 * unit the decoder takes.  frames is the number of output buffers the
 * decoder has at least (the frames the caller keeps and those decoding;
 * zero for the firmware's own number, at most eight).  Waits for VCHIQ's
 * connection.  Thread context only.
 */
int drv_rpi4_mmal_decoder_create(uint32_t width, uint32_t height, uint32_t input_bytes, uint32_t frames, struct drv_rpi4_mmal_decoder **result);

/*
 * Reports the input staging the next access unit goes into, waiting up to
 * timeout_ms until the firmware has finished with it (ETIMEDOUT; zero only
 * looks).
 */
int drv_rpi4_mmal_decoder_input(struct drv_rpi4_mmal_decoder *decoder, unsigned timeout_ms, uint8_t **staging, uint32_t *capacity);

/*
 * Sends the access unit written into the staging the last input call gave.
 *
 * The unit is split into the firmware's input buffers; the first carries
 * the start-of-frame flag, the last the end-of-frame flag, and every one
 * the time stamp.  With config, the staging holds parameter sets only and
 * goes as codec configuration instead (no frame flags).
 */
int drv_rpi4_mmal_decoder_send(struct drv_rpi4_mmal_decoder *decoder, uint32_t bytes, int64_t pts, int config);

/*
 * Sends an end of stream (an empty buffer): the firmware outputs every
 * picture it holds and an empty frame with the end-of-stream flag, and
 * forgets its references (T1-631), so it is for the stream's end only.
 * (An empty buffer with only the end-of-frame flag is not sent: the Pi 4
 * did not come back from the boot that first sent one, T1-633.)
 */
int drv_rpi4_mmal_decoder_end(struct drv_rpi4_mmal_decoder *decoder);

/*
 * Receives the oldest decoded frame not received yet, waiting up to
 * timeout_ms.
 *
 * pixels receives the frame's bytes in the decoder's cached bounce memory,
 * valid until the frame's handle is released or the decoder flushed.
 * ETIMEDOUT means no frame came in time (a timeout of zero only looks).
 */
int drv_rpi4_mmal_decoder_receive(struct drv_rpi4_mmal_decoder *decoder, unsigned timeout_ms, struct drv_rpi4_mmal_frame *frame, const uint8_t **pixels);

/*
 * Sends the frame of one decode to a target instead of the bounce memory.
 *
 * Called before the decode's unit is sent.  The target's run is the
 * caller's to keep until it removes the target.  EBUSY when the decoder has
 * DRV_RPI4_MMAL_TARGETS targets already.
 */
int drv_rpi4_mmal_decoder_target(struct drv_rpi4_mmal_decoder *decoder, const struct drv_rpi4_mmal_target *target);

/*
 * Removes the target of one decode.
 *
 * kept says that a frame's transfer into the target's run did not complete:
 * the VPU may still write the run, so the caller keeps it for good.
 * Removing a time stamp with no target does nothing.
 */
void drv_rpi4_mmal_decoder_untarget(struct drv_rpi4_mmal_decoder *decoder, int64_t pts, bool *kept);

/* Gives a received frame's buffer back to the firmware: handle is the frame's. */
int drv_rpi4_mmal_decoder_release(struct drv_rpi4_mmal_decoder *decoder, unsigned handle);

/*
 * Drops every picture and reference the decoder holds, as at a seek; the
 * frames the caller holds go back to the firmware, and their handles with
 * them.
 */
int drv_rpi4_mmal_decoder_flush(struct drv_rpi4_mmal_decoder *decoder);

/* Destroys a decoder; memory a bulk transfer still holds is kept, not freed. */
void drv_rpi4_mmal_decoder_destroy(struct drv_rpi4_mmal_decoder *decoder);

#endif
