/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the Wave5 video engine (ws322-p004,
 * src/drivers/vpu/wave5-video.c) over a stand-in core: what goes into the
 * ring (the sets when new or at an IDR picture, then the slices), the
 * sequence and the pool (one more than the firmware asks), the frame
 * buffer picked and the display flags around it, and each result of the
 * firmware: decoded, skipped, refused (the bytes taken back), failed, not
 * answered (the stream broken, its pool kept); a new size at an I
 * picture, a reset and a picture before any I picture.
 *
 *   engine-host-test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/powervr.h"
#include "drivers/vpu/vpu-codec.h"
#include "drivers/vpu/wave5-video.h"
#include "drivers/vpu/wave5.h"

static int passes;
static int failures;

static void
check(
	int ok,
	const char *what)
{
	if (ok) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* The kernel's calls, the host's. */
void *kern_memset(void *to, int value, size_t bytes) { return memset(to, value, bytes); }
void *kern_memcpy(void *to, const void *from, size_t bytes) { return memcpy(to, from, bytes); }
int kern_memcmp(const void *a, const void *b, size_t bytes) { return memcmp(a, b, bytes); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }
void kern_logf(const char *format, ...) { (void)format; }
int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name) { (void)mutex; (void)rank; (void)name; return 0; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }

/* The stand-in core's state: what the engine did, and what the firmware answers next. */
static struct {
	const struct drv_powervr_video_ops *ops;
	void *engine;
	int opens;
	int closes;
	int close_error;
	unsigned pieces;
	size_t lengths[3];
	uint8_t first[3];
	int unreads;
	int sequences;
	uint32_t required;
	uint32_t width;
	uint32_t height;
	unsigned registered;
	uint32_t set;
	uint32_t clear;
	int decode_error;
	int32_t decoded;
	uint32_t success;
	int32_t order;
	int pools;
	uint32_t pool_count;
} core;

/* The one stand-in instance. */
static struct drv_wave5_stream *instance = (struct drv_wave5_stream *)&core;

int
drv_powervr_video_register(
	const struct drv_powervr_video_ops *ops,
	void *engine)
{
	core.ops = ops;
	core.engine = engine;
	return 0;
}

int
drv_wave5_open(
	struct drv_wave5 *owner,
	unsigned codec,
	size_t ring_size,
	struct drv_wave5_stream **stream)
{
	(void)owner;
	check(codec == DRV_WAVE5_H264 && ring_size >= 1024U * 1024U, "an H.264 instance with a ring");
	core.opens++;
	*stream = instance;
	return 0;
}

int
drv_wave5_close(
	struct drv_wave5_stream *stream)
{
	(void)stream;
	core.closes++;
	return core.close_error;
}

int
drv_wave5_append_pieces(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_piece *pieces,
	unsigned count)
{
	unsigned index;

	(void)stream;
	core.pieces = count;
	for (index = 0; index < count && index < 3; index++) {
		core.lengths[index] = pieces[index].length;
		core.first[index] = pieces[index].length > 4 ? pieces[index].bytes[4] : pieces[index].bytes[0];
	}
	return 0;
}

void
drv_wave5_unread(
	struct drv_wave5_stream *stream)
{
	(void)stream;
	core.unreads++;
}

int
drv_wave5_init_sequence(
	struct drv_wave5_stream *stream,
	struct drv_wave5_sequence *sequence)
{
	(void)stream;
	memset(sequence, 0, sizeof(*sequence));
	core.sequences++;
	sequence->success = 1;
	sequence->width = core.width;
	sequence->height = core.height;
	sequence->required = core.required;
	sequence->luma_depth = 8;
	sequence->chroma_depth = 8;
	return 0;
}

int
drv_wave5_register(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_picture *linear,
	unsigned count,
	uint32_t stride)
{
	(void)stream;
	check(linear[0].luma == 0x10000000U && linear[count - 1].chroma == 0x10000000U + (count - 1U) * 0x1000000U + 0x200000U,
	      "the pool's pictures are the frame buffers");
	check(stride == 2048U, "the pool's pitch is the stride");
	core.registered = count;
	return 0;
}

int
drv_wave5_display_flags(
	struct drv_wave5_stream *stream,
	uint32_t set,
	uint32_t clear,
	uint32_t *flags)
{
	(void)stream;
	(void)flags;
	core.set = set;
	core.clear = clear;
	return 0;
}

int
drv_wave5_decode(
	struct drv_wave5_stream *stream,
	struct drv_wave5_result *result)
{
	(void)stream;
	memset(result, 0, sizeof(*result));
	result->success = core.success;
	result->decoded = core.decoded;
	result->order = core.order;
	result->ticks = 1234;
	return core.decode_error;
}

/* The render node's pool: count pictures 16 MiB apart, chroma 2 MiB in, pitch 2048. */
static int
pool(
	void *owner,
	uint32_t count,
	uint32_t width,
	uint32_t height,
	uint32_t *pitch,
	struct drv_powervr_video_picture *pictures)
{
	uint32_t index;

	(void)owner;
	(void)width;
	(void)height;
	core.pools++;
	core.pool_count = count;
	for (index = 0; index < count; index++) {
		pictures[index].luma = 0x10000000U + index * 0x1000000U;
		pictures[index].chroma = pictures[index].luma + 0x200000U;
	}
	*pitch = 2048U;
	return 0;
}

/* A 1080p SPS and its PPS. */
static struct vpu_h264_sps sps;
static struct vpu_h264_pps pps;
static const uint8_t slices[] = { 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33 };

static int
decode(
	void *stream,
	uint32_t flags,
	int32_t setup,
	uint32_t references,
	uint32_t active,
	struct drv_powervr_video_result *result)
{
	struct drv_powervr_video_decode job;

	memset(&job, 0, sizeof(job));
	job.bitstream = slices;
	job.length = sizeof(slices);
	job.sps = &sps;
	job.pps = &pps;
	job.flags = flags;
	job.order = core.order;
	job.setup = setup;
	job.references = references;
	job.active = active;
	return core.ops->decode(stream, &job, result);
}

int
main(void)
{
	struct drv_powervr_video_open open;
	struct drv_powervr_video_result result;
	void *stream;
	int error;

	memset(&sps, 0, sizeof(sps));
	sps.flags = VPU_H264_SPS_FRAME_MBS_ONLY;
	sps.profile_idc = 100;
	sps.level = 11;
	sps.chroma_format_idc = 1;
	sps.max_num_ref_frames = 4;
	sps.pic_width_in_mbs_minus1 = 119;
	sps.pic_height_in_map_units_minus1 = 67;
	memset(&pps, 0, sizeof(pps));

	error = drv_wave5_video_register((struct drv_wave5 *)&core);
	check(error == 0 && core.ops != NULL, "registered");
	check(core.ops->operations(core.engine) == DRV_POWERVR_VIDEO_DECODE_H264, "H.264 decode offered");

	open.width = 1920;
	open.height = 1088;
	open.owner = NULL;
	open.pool = pool;
	error = core.ops->open(core.engine, DRV_POWERVR_VIDEO_DECODE_H264, &open, &stream);
	check(error == 0, "opened");

	/* A P picture first: an error, no instance. */
	error = decode(stream, 0, 0, 0, 0, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.opens == 0, "no instance before an I picture");

	/* The IDR picture: sets and slices, the sequence, a pool of five, frame buffer 0. */
	core.width = 1920;
	core.height = 1088;
	core.required = 4;
	core.success = 1;
	core.decoded = 0;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_IDR | DRV_POWERVR_VIDEO_PICTURE_INTRA, 0, 0, 1, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && result.index == 0, "IDR decoded into 0");
	check(core.opens == 1 && core.sequences == 1 && core.pools == 1 && core.pool_count == 5 && core.registered == 5, "sequence and a pool of five");
	check(core.pieces == 3 && core.first[0] == 0x67 && core.first[1] == 0x68 && core.lengths[2] == sizeof(slices), "SPS, PPS, slices");
	check(core.set == 0x1e && core.clear == 0x1, "the others flagged");
	check(result.width == 1920 && result.height == 1088 && result.ticks == 1234, "size and time");

	/* A P picture referencing slot 0, into slot 1: the sets are not sent again, frame buffer 1. */
	core.decoded = 1;
	error = decode(stream, 0, 1, 0x1, 0x3, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && result.index == 1, "P into 1");
	check(core.pieces == 1 && core.set == 0x1d && core.clear == 0x2, "only the slices, 1 picked");

	/* A refused frame buffer: the bytes go back, an error. */
	core.decoded = DRV_WAVE5_INDEX_NONE;
	error = decode(stream, 0, 2, 0x3, 0x7, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.unreads == 1, "refused, taken back");

	/* A skipped picture: complete, nothing to copy. */
	core.decoded = DRV_WAVE5_INDEX_SKIP;
	error = decode(stream, 0, -1, 0x3, 0x3, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && result.index == -1, "skipped");

	/* Another frame buffer than picked: an error. */
	core.decoded = 4;
	error = decode(stream, 0, 2, 0x3, 0x7, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR, "another frame buffer is an error");

	/* An IDR picture sends the sets again. */
	core.decoded = 2;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_IDR | DRV_POWERVR_VIDEO_PICTURE_INTRA, 0, 0, 0x1, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.pieces == 3, "IDR resends the sets");

	/* A new size at an I picture: the instance is closed and another opened. */
	sps.pic_width_in_mbs_minus1 = 79;
	sps.pic_height_in_map_units_minus1 = 44;
	core.width = 1280;
	core.height = 720;
	core.decoded = 0;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_IDR | DRV_POWERVR_VIDEO_PICTURE_INTRA, 0, 0, 0x1, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.opens == 2 && core.closes == 1 && core.pools == 2, "new size, new instance");

	/* A reset closes the instance; the next P picture is an error. */
	core.ops->reset(stream);
	check(core.closes == 2, "reset closes");
	error = decode(stream, 0, 1, 0x1, 0x3, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.opens == 2, "P after reset is an error");

	/* No answer: EIO, broken, the pool kept at the close. */
	core.decode_error = ETIMEDOUT;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_IDR | DRV_POWERVR_VIDEO_PICTURE_INTRA, 0, 0, 0x1, &result);
	check(error == EIO && result.status == DRV_POWERVR_VIDEO_ERROR, "no answer is EIO");
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_IDR | DRV_POWERVR_VIDEO_PICTURE_INTRA, 0, 0, 0x1, &result);
	check(error == EIO, "a broken stream decodes nothing");
	error = core.ops->close(stream);
	check(error == EIO, "a broken stream's pool is kept");

	/* Eight streams at most. */
	{
		void *streams[9];
		int index;
		for (index = 0; index < 8; index++)
			check(core.ops->open(core.engine, DRV_POWERVR_VIDEO_DECODE_H264, &open, &streams[index]) == 0, "a stream of eight");
		check(core.ops->open(core.engine, DRV_POWERVR_VIDEO_DECODE_H264, &open, &streams[8]) == EBUSY, "a ninth is EBUSY");
		for (index = 0; index < 8; index++)
			check(core.ops->close(streams[index]) == 0, "closed");
	}

	printf("engine: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
