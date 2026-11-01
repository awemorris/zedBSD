/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wave5 decoder as the PowerVR render node's video engine (see
 * wave5-video.h and drv_powervr_video_ops in powervr.h).
 *
 * One engine stream is one Vulkan video session and, once its first I
 * picture comes, one instance of the firmware.  Each picture goes into
 * the instance's bitstream ring after the one before (the firmware reads
 * the ring on from where it stopped and cannot be moved back), behind
 * the picture's parameter sets whenever they differ from the ones sent
 * last or the picture is an IDR picture.  The first picture sets the
 * sequence up, which says how many frame buffers the firmware needs; the
 * render node then makes that many pool pictures plus one, and they are
 * registered as the frame buffers' linear halves, once.
 *
 * The firmware keeps its references in its frame buffers and must not
 * decode into one the application's DPB slots still hold (vpu-codec.h's
 * slots tie them).  The display flags of the live slots' frame buffers
 * are set and the others cleared, so the firmware decodes into one of
 * those it does not reference itself (it may keep one more than the
 * slots, and waits for, rather than refuses, a frame buffer it holds);
 * it gives each picture out as soon as it is decoded.  The render node
 * copies the pool picture into the application's image.
 *
 * A picture that cannot be decoded is an error and the stream goes on: a
 * refused one is taken back out of the ring; after a refused sequence or
 * a sequence change the instance is closed and the next I picture opens
 * another.  A reset flushes the instance, which keeps its sequence and
 * frame buffers, and the next picture must be an I picture.  A core that
 * stops answering is recovered (wave5.h): its picture is an error, every
 * stream's instance is lost and each stream opens another at its next I
 * picture.  A core the recoveries gave up on decodes nothing more; one
 * that could not even be stopped leaves its streams broken, their pools
 * kept for the core may still write them.  The calls of every stream are
 * serialized by the engine's lock, since the firmware's commands of one
 * picture belong together.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#if WAVE5_VIDEO_TEST
#include <kern/cdev.h>
#include <kern/file.h>
#endif

#include "drivers/gpu/powervr/powervr.h"
#include "drivers/vpu/vpu-codec.h"
#include "drivers/vpu/wave5-video.h"
#include "drivers/vpu/wave5.h"

/*
 * The bytes of a stream's bitstream ring, a multiple of 1 KiB: one
 * picture of 1080p is tens of kilobytes, an I picture of 4K a megabyte
 * or two.  A measurement build may make it smaller to see the ring wrap
 * sooner (ZEDBSD_TEST_CPPFLAGS += -DWAVE5_VIDEO_RING_BYTES=...).
 */
#ifndef WAVE5_VIDEO_RING_BYTES
#define WAVE5_VIDEO_RING_BYTES		(4U * 1024U * 1024U)
#endif

/* The most streams of the engine, one firmware instance each. */
#define VIDEO_STREAMS_MAX		8U

/* The bit depth of the luma and chroma the pool pictures hold. */
#define VIDEO_DEPTH			8U

/* The decodes a stream remembers for the log of one that went wrong: frame buffers flagged, decoded, flags after, order count. */
#define VIDEO_HISTORY			16U

/* The pixels across a macroblock. */
#define VIDEO_MB			16U

/* The log lines of pictures in error the engine writes at most, and of read positions. */
#define VIDEO_LOG_LINES			32U
#define VIDEO_PLACE_LINES		4U

/*
 * A measurement build publishes /dev/wave5-test (ZEDBSD_TEST_CPPFLAGS +=
 * -DWAVE5_VIDEO_TEST=1): a read gives the engine's counts and the core's
 * health, a write of "task <thousandths> [<KiB alignment>]" sizes and
 * aligns the task buffers of the next sequences, "lose <count>" has
 * picture completions go unseen and "clear" starts the counts afresh.
 */
#ifndef WAVE5_VIDEO_TEST
#define WAVE5_VIDEO_TEST		0
#endif
#define TEST_DEVICE_NUMBER		0x001b0000U
#define TEST_DEVICE_NAME		"wave5-test"
#define TEST_TEXT			512U

/*
 * What the engine did, for the measurements: the decodes that completed,
 * the host's time for them (their sum and the longest, in microseconds)
 * and the core's (its cycles), the pictures in error, the instances
 * opened, flushed by a reset and closed (for any reason).
 */
struct video_counts {
	uint32_t decodes;
	uint64_t wall;
	uint64_t longest;
	uint64_t ticks;
	uint32_t errors;
	uint32_t opened;
	uint32_t flushed;
	uint32_t closed;
};

/*
 * The engine: its core, its lock (every stream's calls, see the file's
 * comment), the count of open streams, whether the core was given up on,
 * and the counts, all of which the lock guards.
 */
struct video_engine {
	struct drv_wave5 *core;
	struct mutex lock;
	uint32_t streams;
	int stopped;
	struct video_counts counts;
};

/*
 * One stream: its engine and what it was opened with; its instance (NULL
 * until an I picture opens one, and after a reset); whether the
 * instance's sequence is set up, its coded size and the pool registered
 * for it; the slots' ties to the frame buffers; the parameter sets sent
 * last; the scratch a set is written in; whether a flushed instance
 * waits for an I picture; whether the core could not be stopped (broken);
 * and the counts of pictures and errors.
 */
struct video_stream {
	struct video_engine *engine;
	struct drv_powervr_video_open open;
	struct drv_wave5_stream *instance;
	int sequenced;
	int flushed;
	uint32_t width;
	uint32_t height;
	uint32_t count;
	uint32_t pitch;
	struct drv_powervr_video_picture pool[DRV_WAVE5_MAX_BUFFERS];
	struct vpu_slots slots;
	uint8_t sent_sps[VPU_H264_SET_MAX];
	size_t sent_sps_bytes;
	uint8_t sent_pps[VPU_H264_SET_MAX];
	size_t sent_pps_bytes;
	uint8_t sps_unit[VPU_H264_SET_MAX];
	uint8_t pps_unit[VPU_H264_SET_MAX];
	int broken;
	uint32_t pictures;
	uint32_t errors;
	uint32_t history[VIDEO_HISTORY][4];
};

/* The one engine, made by drv_wave5_video_register() and kept for the system's life. */
static struct video_engine *video_engine;

/* The log lines written so far (a count only, a race costs a line at most). */
static uint32_t video_log_lines;

/* The lines of read positions the core reported elsewhere than the bytes' end (a count only). */
static uint32_t video_place_lines;

static uint32_t engine_operations(void *engine);
static int engine_open(void *engine, uint32_t operation, const struct drv_powervr_video_open *open, void **stream);
static int engine_decode(void *stream, const struct drv_powervr_video_decode *job, struct drv_powervr_video_result *result);
static void engine_keep(void *stream, uint32_t active);
static void engine_reset(void *stream);
static int engine_close(void *stream);
static int decode_locked(struct video_stream *video, const struct drv_powervr_video_decode *job, struct drv_powervr_video_result *result);
static int put_picture(struct video_stream *video, const struct drv_powervr_video_decode *job);
static int start_sequence(struct video_stream *video);
static int set_differs(const uint8_t *unit, size_t bytes, const uint8_t *sent, size_t sent_bytes);
static void close_instance(struct video_stream *video);
static int recover(struct video_stream *video, int error);
#if WAVE5_VIDEO_TEST
static ssize_t test_read(struct file *file, void *buffer, size_t size);
static ssize_t test_write(struct file *file, const void *buffer, size_t size);

/* The test device: a read gives the counts, a write sets a test's knob. */
static const struct cdev_ops test_ops = {
	.read = test_read,
	.write = test_write,
};
#endif
static void note(const struct video_stream *video, const char *what, int value);
static void describe(const struct video_stream *video, const struct drv_wave5_result *decoded, uint32_t index, int32_t setup);

/* The engine's operations, as the render node calls them. */
static const struct drv_powervr_video_ops video_ops = {
	engine_operations,
	engine_open,
	engine_decode,
	engine_keep,
	engine_reset,
	engine_close
};

/*
 * Registers a booted core with the render node as its video engine.
 */
int
drv_wave5_video_register(
	struct drv_wave5 *core)
{
	struct video_engine *engine;
	int error;

	/* Only one engine. */
	if (video_engine != NULL)
		return EBUSY;

	/* The engine and its lock. */
	engine = kern_calloc(1, sizeof(*engine));
	if (engine == NULL)
		return ENOMEM;
	engine->core = core;
	error = mutex_init(&engine->lock, LOCK_RANK_DEVICE, "wave5 video");
	if (error != 0) {
		kern_free(engine);
		return error;
	}

	/* The render node's sessions opened from now on offer H.264 decode. */
	error = drv_powervr_video_register(&video_ops, engine);
	if (error != 0) {
		kern_free(engine);
		return error;
	}

	/* Succeeded: the engine stays for the system's life. */
	video_engine = engine;
#if WAVE5_VIDEO_TEST
	error = cdev_register(TEST_DEVICE_NAME, (dev_t)TEST_DEVICE_NUMBER, &test_ops, NULL);
	if (error != 0)
		kern_logf("wave5: video: /dev/%s not published (%d)\n", TEST_DEVICE_NAME, error);
#endif
	return 0;
}

/* Reports the operations of the engine: H.264 decode. */
static uint32_t
engine_operations(
	void *engine)
{
	/* Succeeded: the one operation. */
	(void)engine;
	return DRV_POWERVR_VIDEO_DECODE_H264;
}

/* Opens a stream of H.264 decode; its instance comes with its first I picture. */
static int
engine_open(
	void *engine,
	uint32_t operation,
	const struct drv_powervr_video_open *open,
	void **stream)
{
	struct video_engine *owner;
	struct video_stream *video;
	int refused;

	/* Only H.264 decode, with a pool to decode into. */
	owner = engine;
	if (operation != DRV_POWERVR_VIDEO_DECODE_H264 || open->pool == NULL)
		return EINVAL;

	/* The stream's state. */
	video = kern_calloc(1, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	video->engine = owner;
	video->open = *open;
	vpu_slots_reset(&video->slots, 0);

	/* A free place among the firmware's instances, on a core not given up on. */
	mutex_lock(&owner->lock);

	refused = 0;
	if (owner->stopped) {
		refused = EIO;
	} else if (owner->streams >= VIDEO_STREAMS_MAX) {
		refused = EBUSY;
	} else {
		owner->streams++;
	}

	mutex_unlock(&owner->lock);

	/* Refuses a stream past the instances the firmware holds, or on a stopped core. */
	if (refused != 0) {
		kern_free(video);
		return refused;
	}

	/* Succeeded: the stream is open. */
	*stream = video;
	return 0;
}

/* Decodes one picture, under the engine's lock. */
static int
engine_decode(
	void *stream,
	const struct drv_powervr_video_decode *job,
	struct drv_powervr_video_result *result)
{
	struct video_stream *video;
	int error;

	/* No picture yet. */
	video = stream;
	kern_memset(result, 0, sizeof(*result));
	result->status = DRV_POWERVR_VIDEO_ERROR;
	result->index = -1;

	/* The picture's decode. */
	mutex_lock(&video->engine->lock);

	error = decode_locked(video, job, result);
	video->pictures++;
	if (result->status != DRV_POWERVR_VIDEO_COMPLETE) {
		video->errors++;
		video->engine->counts.errors++;
	} else {
		video->engine->counts.decodes++;
		video->engine->counts.wall += result->wall;
		video->engine->counts.ticks += result->ticks;
		if (result->wall > video->engine->counts.longest)
			video->engine->counts.longest = result->wall;
	}

	mutex_unlock(&video->engine->lock);

	/* A core that stopped answering. */
	if (error != 0)
		return error;

	/* Succeeded: the result says how the picture went. */
	return 0;
}

/* Forgets the ties of the slots a coding scope does not keep. */
static void
engine_keep(
	void *stream,
	uint32_t active)
{
	struct video_stream *video;

	/* The slots kept, under the engine's lock. */
	video = stream;
	mutex_lock(&video->engine->lock);

	vpu_slots_keep(&video->slots, active);

	mutex_unlock(&video->engine->lock);
}

/*
 * Forgets the stream's pictures: the instance is flushed, keeping its
 * sequence and frame buffers, and waits for an I picture; one the
 * firmware does not flush goes, and the next I picture opens another.
 */
static void
engine_reset(
	void *stream)
{
	struct video_stream *video;
	int error;

	/* The instance and the ties, under the engine's lock. */
	video = stream;
	mutex_lock(&video->engine->lock);

	if (video->instance != NULL && drv_wave5_lost(video->instance))
		close_instance(video);
	if (video->instance != NULL && video->sequenced) {
		error = drv_wave5_flush(video->instance);
		if (error == 0) {
			video->flushed = 1;
			video->sent_sps_bytes = 0;
			video->sent_pps_bytes = 0;
			vpu_slots_reset(&video->slots, video->count);
			video->engine->counts.flushed++;
		} else if (error == ETIMEDOUT) {
			(void)recover(video, error);
			close_instance(video);
		} else {
			note(video, "the instance was not flushed", error);
			close_instance(video);
		}
	} else if (video->instance != NULL) {
		close_instance(video);
	}

	mutex_unlock(&video->engine->lock);
}

/* Ends a stream; EIO when the core may still write its pool. */
static int
engine_close(
	void *stream)
{
	struct video_stream *video;
	struct video_engine *owner;
	int broken;

	/* The instance, then the stream's place among the instances. */
	video = stream;
	owner = video->engine;
	mutex_lock(&owner->lock);

	close_instance(video);
	broken = video->broken;
	owner->streams--;

	mutex_unlock(&owner->lock);

	/* The counts, for the log. */
	if (video->pictures != 0)
		kern_logf("wave5: video: a stream ends: %u pictures, %u in error\n", (unsigned)video->pictures, (unsigned)video->errors);
	kern_free(video);

	/* The core may still write the pool of a stream it stopped answering. */
	if (broken)
		return EIO;

	/* Succeeded: the pool may go. */
	return 0;
}

/*
 * Decodes one picture with the engine's lock held: the ring, the sequence,
 * the frame buffer, the display flags, the decode and its result.  EIO
 * when the core stopped answering.
 */
static int
decode_locked(
	struct video_stream *video,
	const struct drv_powervr_video_decode *job,
	struct drv_powervr_video_result *result)
{
	struct drv_wave5_result decoded;
	uint32_t coded_width;
	uint32_t coded_height;
	uint32_t index;
	uint32_t mask;
	uint32_t taken;
	uint32_t read;
	uint32_t write;
	int intra;
	int error;

	/* A stream the core stopped answering decodes nothing more, nor one on a core given up on. */
	if (video->broken || video->engine->stopped)
		return EIO;

	/* An instance a recovery lost goes; the next I picture opens another. */
	if (video->instance != NULL && drv_wave5_lost(video->instance))
		close_instance(video);

	/* Whether the picture has only I slices: an instance starts at one. */
	intra = 0;
	if ((job->flags & DRV_POWERVR_VIDEO_PICTURE_INTRA) != 0)
		intra = 1;

	/* A new coded size at an I picture starts another instance; before one it is an error. */
	coded_width = (job->sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
	coded_height = (job->sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
	if ((job->sps->flags & VPU_H264_SPS_FRAME_MBS_ONLY) == 0)
		coded_height *= 2U;
	if (video->sequenced && (coded_width != video->width || coded_height != video->height)) {
		if (!intra)
			return 0;
		close_instance(video);
	}

	/* An instance starts at an I picture, and a flushed one goes on at one. */
	if (video->instance == NULL && !intra)
		return 0;
	if (video->flushed && !intra)
		return 0;
	video->flushed = 0;

	/* An instance for the stream. */
	if (video->instance == NULL) {
		error = drv_wave5_open(video->engine->core, DRV_WAVE5_H264, WAVE5_VIDEO_RING_BYTES, &video->instance);
		if (error != 0) {
			note(video, "no instance", error);
			return 0;
		}
		video->sequenced = 0;
		video->sent_sps_bytes = 0;
		video->sent_pps_bytes = 0;
		video->engine->counts.opened++;
	}

	/* The picture's parameter sets and slices in the ring. */
	error = put_picture(video, job);
	if (error == ETIMEDOUT)
		return recover(video, error);
	if (error != 0) {
		note(video, "not put in the ring", error);
		return 0;
	}

	/* The sequence from the first picture: its frame buffers and the pool. */
	if (!video->sequenced) {
		error = start_sequence(video);
		if (error == ETIMEDOUT)
			return recover(video, error);
		if (error != 0) {
			note(video, "no sequence", error);
			close_instance(video);
			return 0;
		}
	}

	/* A frame buffer no live slot holds must be there; none means the picture goes back out of the ring. */
	error = vpu_slots_pick(&video->slots, job->references, job->active, job->setup, &index);
	if (error != 0) {
		drv_wave5_unread(video->instance);
		note(video, "no free frame buffer", error);
		return 0;
	}

	/*
	 * The live slots' frame buffers are flagged and the others not: the
	 * firmware decodes into one of those it does not reference itself
	 * (it may keep one the slots no longer hold, and waits for a picked
	 * one it still holds rather than refusing it).
	 */
	mask = (uint32_t)((1ULL << video->count) - 1U);
	taken = vpu_slots_taken(&video->slots, job->references, job->active, job->setup);
	error = drv_wave5_display_flags(video->instance, taken, mask & ~taken, NULL);
	if (error == ETIMEDOUT)
		return recover(video, error);
	if (error != 0) {
		drv_wave5_unread(video->instance);
		note(video, "display flags refused", error);
		return 0;
	}

	/* The decode, remembered for the log. */
	error = drv_wave5_decode(video->instance, &decoded);
	result->ticks = decoded.ticks;
	result->wall = decoded.wall;
	video->history[video->pictures % VIDEO_HISTORY][0] = taken;
	video->history[video->pictures % VIDEO_HISTORY][1] = (uint32_t)decoded.decoded;
	video->history[video->pictures % VIDEO_HISTORY][2] = decoded.display_flags;
	video->history[video->pictures % VIDEO_HISTORY][3] = (uint32_t)decoded.order | (job->references << 16) | (job->active << 24);
	if (error == ETIMEDOUT) {
		note(video, "the core did not answer", error);
		describe(video, &decoded, index, job->setup);
		return recover(video, error);
	}

	/* A command the firmware refused reads nothing. */
	if (error != 0) {
		drv_wave5_unread(video->instance);
		note(video, "decode refused", error);
		return 0;
	}

	/* A picture the stream says to skip is complete, with nothing to copy. */
	if (decoded.decoded == DRV_WAVE5_INDEX_SKIP) {
		result->status = DRV_POWERVR_VIDEO_COMPLETE;
		return 0;
	}

	/* A picture the firmware refused (its frame buffer still referenced) left the ring unread. */
	if (decoded.decoded == DRV_WAVE5_INDEX_NONE) {
		drv_wave5_unread(video->instance);
		note(video, "frame buffer refused", (int)index);
		return 0;
	}

	/* A failed decode, or one into a frame buffer a live slot holds. */
	if (decoded.success == 0 || decoded.decoded < 0 || decoded.decoded >= (int32_t)video->count ||
	    (taken & (1U << (uint32_t)decoded.decoded)) != 0) {
		note(video, "decode failed", decoded.decoded);
		describe(video, &decoded, index, job->setup);
		return 0;
	}

	/* The core should have read the picture's bytes to their end. */
	drv_wave5_positions(video->instance, &read, &write);
	if (decoded.read_position != write && video_place_lines < VIDEO_PLACE_LINES) {
		video_place_lines++;
		kern_logf("wave5: video: picture %u: the core read to %08x, the bytes end at %08x\n",
			  (unsigned)video->pictures,
			  (unsigned)decoded.read_position,
			  (unsigned)write);
	}

	/* The firmware's and the application's order counts should agree. */
	if (decoded.order != job->order)
		note(video, "order count differs", decoded.order);

	/* The setup slot holds the frame buffer from now on. */
	index = (uint32_t)decoded.decoded;
	vpu_slots_bind(&video->slots, job->setup, index);

	/* Succeeded: the picture is in the pool's picture. */
	result->status = DRV_POWERVR_VIDEO_COMPLETE;
	result->index = (int32_t)index;
	result->width = video->width;
	result->height = video->height;
	return 0;
}

/*
 * Puts a picture in the ring: its SPS and PPS when they differ from the
 * ones sent last or the picture is an IDR picture, then its slices.
 */
static int
put_picture(
	struct video_stream *video,
	const struct drv_powervr_video_decode *job)
{
	struct drv_wave5_piece pieces[3];
	size_t sps_bytes;
	size_t pps_bytes;
	unsigned count;
	int idr;
	int sps_new;
	int pps_new;
	int error;

	/* The sets as NAL units. */
	error = vpu_h264_write_sps(job->sps, 1, video->sps_unit, sizeof(video->sps_unit), &sps_bytes);
	if (error != 0)
		return error;
	error = vpu_h264_write_pps(job->pps, job->sps, video->pps_unit, sizeof(video->pps_unit), &pps_bytes);
	if (error != 0)
		return error;

	/* Each set goes in when it is new or the picture is an IDR picture. */
	idr = 0;
	if ((job->flags & DRV_POWERVR_VIDEO_PICTURE_IDR) != 0)
		idr = 1;
	sps_new = set_differs(video->sps_unit, sps_bytes, video->sent_sps, video->sent_sps_bytes);
	pps_new = set_differs(video->pps_unit, pps_bytes, video->sent_pps, video->sent_pps_bytes);

	/* The pieces: the sets, then the slices. */
	count = 0;
	if (sps_new || idr) {
		pieces[count].bytes = video->sps_unit;
		pieces[count].length = sps_bytes;
		count++;
	}
	if (pps_new || sps_new || idr) {
		pieces[count].bytes = video->pps_unit;
		pieces[count].length = pps_bytes;
		count++;
	}
	pieces[count].bytes = job->bitstream;
	pieces[count].length = job->length;
	count++;

	/* Into the ring after the last picture, and the core told where the bytes end. */
	error = drv_wave5_append_pieces(video->instance, pieces, count);
	if (error != 0)
		return error;
	error = drv_wave5_update_stream(video->instance);
	if (error != 0) {
		drv_wave5_unread(video->instance);
		return error;
	}

	/* The sets sent last. */
	kern_memcpy(video->sent_sps, video->sps_unit, sps_bytes);
	video->sent_sps_bytes = sps_bytes;
	kern_memcpy(video->sent_pps, video->pps_unit, pps_bytes);
	video->sent_pps_bytes = pps_bytes;

	/* Succeeded: the picture is in the ring. */
	return 0;
}

/*
 * Sets the instance's sequence up from the picture in the ring, has the
 * render node make the pool for it (one picture more than the firmware
 * asks for) and registers the pool.  EINVAL for a sequence the session
 * cannot hold.
 */
static int
start_sequence(
	struct video_stream *video)
{
	struct drv_wave5_picture linear[DRV_WAVE5_MAX_BUFFERS];
	struct drv_wave5_sequence sequence;
	uint32_t count;
	uint32_t pitch;
	uint32_t index;
	int error;

	/* The sequence the firmware found. */
	error = drv_wave5_init_sequence(video->instance, &sequence);
	if (error != 0)
		return error;

	/* 8-bit pictures no larger than the session's. */
	if (sequence.luma_depth != VIDEO_DEPTH || sequence.chroma_depth != VIDEO_DEPTH)
		return EINVAL;
	if (sequence.width > video->open.width || sequence.height > video->open.height)
		return EINVAL;

	/* One frame buffer more than the firmware asks for, within what it registers. */
	count = sequence.required + 1U;
	if (count > DRV_WAVE5_MAX_BUFFERS)
		return EINVAL;

	/* The pool for the coded size, then its pictures as the frame buffers' linear halves. */
	error = video->open.pool(video->open.owner, count, sequence.width, sequence.height, &pitch, video->pool);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		linear[index].luma = video->pool[index].luma;
		linear[index].chroma = video->pool[index].chroma;
	}
	error = drv_wave5_register(video->instance, linear, count, pitch);
	if (error != 0)
		return error;

	/* Succeeded: the sequence is set up and no slot holds a frame buffer. */
	video->sequenced = 1;
	video->width = sequence.width;
	video->height = sequence.height;
	video->count = count;
	video->pitch = pitch;
	vpu_slots_reset(&video->slots, count);
	return 0;
}

/* Reports whether a written set differs from the one sent last (none sent differs). */
static int
set_differs(
	const uint8_t *unit,
	size_t bytes,
	const uint8_t *sent,
	size_t sent_bytes)
{
	int difference;

	/* Another length differs. */
	if (bytes != sent_bytes)
		return 1;

	/* The same length differs by its bytes. */
	difference = kern_memcmp(unit, sent, bytes);
	if (difference != 0)
		return 1;

	/* The same set. */
	return 0;
}

/* Closes the stream's instance, if any; the next I picture opens another. */
static void
close_instance(
	struct video_stream *video)
{
	int error;

	/* No instance, nothing to close. */
	if (video->instance == NULL)
		return;

	/* The firmware lets the instance go, or keeps its memory until the core goes. */
	error = drv_wave5_close(video->instance);
	if (error != 0)
		note(video, "the instance was not destroyed", error);

	/* The stream has no instance, no sequence and no ties. */
	video->instance = NULL;
	video->sequenced = 0;
	video->flushed = 0;
	vpu_slots_reset(&video->slots, 0);
	video->engine->counts.closed++;
}

/*
 * Brings the core back after work that timed out, with the engine's lock
 * held: the picture is an error and every stream's instance is lost (the
 * next I picture opens another).  EIO for a core given up on, whose
 * streams decode nothing more; a core that could not be stopped leaves
 * the stream broken too, its pool kept.
 */
static int
recover(
	struct video_stream *video,
	int error)
{
	int recovered;

	/* Only a timeout is recovered from. */
	if (error != ETIMEDOUT)
		return 0;

	/* The core again, or given up on. */
	recovered = drv_wave5_recover(video->engine->core);
	if (recovered == 0) {
		note(video, "the core was recovered", 0);
		return 0;
	}
	video->engine->stopped = 1;
	if (recovered == EIO)
		video->broken = 1;
	note(video, "the core was given up on", recovered);
	return EIO;
}

/* Logs why a picture went wrong, the first few times. */
static void
note(
	const struct video_stream *video,
	const char *what,
	int value)
{
	/* Only the first lines. */
	if (video_log_lines >= VIDEO_LOG_LINES)
		return;
	video_log_lines++;

	/* The reason, the value and the picture's number. */
	kern_logf("wave5: video: picture %u: %s (%d)\n", (unsigned)video->pictures, what, value);
}

/* Logs what the firmware answered for a picture that went wrong, the first few times. */
static void
describe(
	const struct video_stream *video,
	const struct drv_wave5_result *decoded,
	uint32_t index,
	int32_t setup)
{
	const uint32_t *entry;
	uint32_t flags;
	uint32_t back;
	unsigned taken;
	unsigned polled;
	uint32_t reasons;
	uint32_t read;
	uint32_t write;

	/* Only the first lines. */
	if (video_log_lines >= VIDEO_LOG_LINES)
		return;
	video_log_lines++;

	/* The firmware's answer, the interrupts, and where the picture's bytes are. */
	drv_wave5_interrupts(video->engine->core, &taken, &polled, &reasons);
	drv_wave5_positions(video->instance, &read, &write);
	kern_logf("wave5: video: picture %u: pick %u ok %u fail %08x warn %08x dec %d disp %d order %d flags %08x queue %08x,%08x"
		  " irq %u polled %u reasons %08x bytes %08x-%08x read %08x wraps %u reason %08x\n",
		  (unsigned)video->pictures,
		  (unsigned)index,
		  (unsigned)decoded->success,
		  (unsigned)decoded->error,
		  (unsigned)decoded->warning,
		  (int)decoded->decoded,
		  (int)decoded->display,
		  (int)decoded->order,
		  (unsigned)decoded->display_flags,
		  (unsigned)decoded->queue[0],
		  (unsigned)decoded->queue[1],
		  taken,
		  polled,
		  (unsigned)reasons,
		  (unsigned)read,
		  (unsigned)write,
		  (unsigned)decoded->read_position,
		  (unsigned)drv_wave5_wraps(video->instance),
		  (unsigned)drv_wave5_last_reason(video->engine->core));

	/* The flags the firmware holds now, and the decodes before. */
	flags = 0;
	(void)drv_wave5_display_flags(video->instance, 0, 0, &flags);
	kern_logf("wave5: video: picture %u: flags now %08x, setup %d\n", (unsigned)video->pictures, (unsigned)flags, (int)setup);
	for (back = VIDEO_HISTORY; back > 0; back--) {
		if (video->pictures < back)
			continue;
		entry = video->history[(video->pictures - back) % VIDEO_HISTORY];
		kern_logf("wave5: video:  picture %u: taken %02x dec %d flags %02x order %d refs %02x active %02x\n",
			  (unsigned)(video->pictures - back),
			  (unsigned)entry[0],
			  (int)entry[1],
			  (unsigned)entry[2],
			  (int)(int16_t)(entry[3] & 0xffffU),
			  (unsigned)((entry[3] >> 16) & 0xffU),
			  (unsigned)(entry[3] >> 24));
	}
}

#if WAVE5_VIDEO_TEST
/* Gives the engine's counts and the core's health from the start; nothing at a later offset. */
static ssize_t
test_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_wave5_health health;
	struct video_counts counts;
	struct video_engine *engine;
	char text[TEST_TEXT];
	size_t length;

	/* The text once, from the start. */
	engine = video_engine;
	if (engine == NULL || file->f_offset != 0)
		return 0;

	/* The counts under the engine's lock, the health under the core's. */
	mutex_lock(&engine->lock);

	counts = engine->counts;

	mutex_unlock(&engine->lock);
	drv_wave5_health(engine->core, &health);

	/* One line. */
	kern_snprintf(text,
		      sizeof(text),
		      "decodes %u errors %u wall %llu longest %llu ticks %llu opened %u flushed %u closed %u"
		      " recoveries %u lost %u strikes %u stopped %d flushes %u\n",
		      (unsigned)counts.decodes,
		      (unsigned)counts.errors,
		      (unsigned long long)counts.wall,
		      (unsigned long long)counts.longest,
		      (unsigned long long)counts.ticks,
		      (unsigned)counts.opened,
		      (unsigned)counts.flushed,
		      (unsigned)counts.closed,
		      (unsigned)health.recoveries,
		      (unsigned)health.lost,
		      (unsigned)health.strikes,
		      health.stopped,
		      (unsigned)health.flushes);
	length = kern_strlen(text);
	if (length > size)
		length = size;
	kern_memcpy(buffer, text, length);
	file->f_offset = (off_t)length;

	/* Succeeded: the line. */
	return (ssize_t)length;
}

/* Takes one of "task <thousandths> [<KiB alignment>]", "lose <count>" and "clear". */
static ssize_t
test_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct video_engine *engine;
	const char *text;
	uint32_t alignment;
	uint32_t value;
	size_t at;

	/* A word, then a number. */
	(void)file;
	engine = video_engine;
	if (engine == NULL)
		return -ENODEV;
	text = buffer;
	at = 0;
	while (at < size && text[at] != ' ' && text[at] != '\n')
		at++;
	value = 0;
	while (at < size && text[at] == ' ')
		at++;
	while (at < size && text[at] >= '0' && text[at] <= '9') {
		value = value * 10U + (uint32_t)(text[at] - '0');
		at++;
	}

	/* The knob the word names. */
	if (size >= 5U && kern_memcmp(text, "task ", 5U) == 0) {
		alignment = 0;
		while (at < size && text[at] == ' ')
			at++;
		while (at < size && text[at] >= '0' && text[at] <= '9') {
			alignment = alignment * 10U + (uint32_t)(text[at] - '0');
			at++;
		}
		drv_wave5_set_task_share(engine->core, value, alignment * 1024U);
	} else if (size >= 5U && kern_memcmp(text, "lose ", 5U) == 0) {
		drv_wave5_lose_completions(engine->core, value);
	} else if (size >= 5U && kern_memcmp(text, "clear", 5U) == 0) {
		mutex_lock(&engine->lock);

		kern_memset(&engine->counts, 0, sizeof(engine->counts));

		mutex_unlock(&engine->lock);
	} else {
		return -EINVAL;
	}

	/* Succeeded: the whole write taken. */
	return (ssize_t)size;
}
#endif
