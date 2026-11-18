/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot-time probe of the VideoCore H.264 decoder (ws236 design §12),
 * built only by test kernels (CONFIG_DRIVER_RPI4_GPU_PROBE).
 *
 * It decodes two embedded streams with B frames, each four ways: as
 * encoded (the control, run a), with every SPS rewritten to ask for no
 * reordering (run b), and the same as b with a push after the first unit
 * (runs g, f; see enum probe_push) to find what makes the firmware give
 * out the first picture at once without losing its references.  It runs
 * when a write to /dev/mmal-probe asks, in a thread that write starts: the
 * boot path only publishes the device (T1-626: a frame's bulk stopped
 * while the 3D core started; T1-633 and T1-634: the Pi 4 did not come back
 * from boots with the probe's thread on the boot path).
 * Each output picture is identified by the checksum of its visible bytes
 * against the reference decode, and its place in decode order is compared
 * with the input that was last sent, which shows whether the firmware
 * outputs in decode order without delay.  The same is judged again by the
 * output's time stamp (its unit's decode index), which holds even for a
 * picture whose bytes differ; a picture whose luma alone matches points
 * at a chroma layout fault, and a map of band and strip checksums shows
 * where a picture differs.  Everything is written to the kernel log, one
 * line per output and one summary per run; a few frames are kept as they
 * came for /dev/mmal-probe (rpi4-mmal-dump.c).
 */

#include "drivers/platform/rpi4/rpi4-mmal-probe.h"
#include "drivers/platform/rpi4/rpi4-mmal-dump.h"
#include "drivers/platform/rpi4/rpi4-mmal.h"
#include "drivers/platform/rpi4/rpi4-h264.h"
#include "drivers/platform/rpi4/rpi4-vchiq.h"
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>
#include <uapi/time.h>

#include "drivers/platform/rpi4/rpi4-mmal-probe-data.inc"

/* How long the probe, once asked to run, waits for VCHIQ to connect. */
#define PROBE_CONNECT_MS		15000U


/* How long it waits for an output after each input, and at the end. */
#define PROBE_OUTPUT_MS			100U
#define PROBE_DRAIN_MS			500U

/* The largest access unit of the embedded streams, with room for a rewritten SPS. */
#define PROBE_INPUT_BYTES		(1024U * 1024U)

/* How long a staging area the firmware still reads is waited for. */
#define PROBE_INPUT_MS			1000U

/* The most frames a stream has. */
#define PROBE_MAX_FRAMES		32U

/* The encoding of an I420 frame (the decoder's default output), summed as NV12. */
#define PROBE_ENCODING_I420		0x30323449U

/* The FNV-1a 32 offset basis and prime. */
#define PROBE_FNV_BASIS			0x811c9dc5U
#define PROBE_FNV_PRIME			0x01000193U

/* An NV12 frame's encoding. */
#define PROBE_ENCODING_NV12		0x3231564eU

/*
 * A frame's map: the checksums of its bands (16 luma rows, 8 chroma rows)
 * and strips (64 bytes across, luma then chroma), the most words of one,
 * and the bytes of a strip.
 */
#define PROBE_MAP_WORDS			256U
#define PROBE_STRIP_BYTES		64U

/* Which NAL units of an access unit a copy takes: all, or the parameter sets. */
enum probe_nals {
	PROBE_NALS_ALL,
	PROBE_NALS_SETS
};

/* One embedded stream and its reference. */
struct probe_stream {
	const char *name;
	const char *file;
	const uint8_t *data;
	size_t bytes;
	uint32_t width;
	uint32_t height;
	const uint32_t *display_sums;
	const uint32_t *luma_sums;
	const uint32_t *maps;
	const uint8_t *decode_order;
	unsigned frames;
};

/*
 * What a run sends right after the first unit, to see what makes the
 * firmware give out the first picture (it holds it until more input
 * comes; an end of stream did, but lost the references, T1-631): nothing,
 * the unit's parameter sets again as codec configuration in a buffer of
 * their own, or an access unit delimiter in a buffer of its own.  (An
 * empty end-of-frame buffer is not tried again, T1-633.)
 */
enum probe_push {
	PROBE_PUSH_NONE,
	PROBE_PUSH_SETS,
	PROBE_PUSH_DELIMITER
};

/*
 * How one run feeds the decoder: as encoded (a), with every SPS rewritten
 * for no reordering (b), and the same as b with each push after the first
 * unit (g: parameter sets, f: delimiter); and how many of its frames go to
 * the dump.
 */
struct probe_mode {
	const char *name;
	int rewrite;
	enum probe_push push;
	unsigned dumps;
};

/*
 * The runs of each stream: the first four frames of 352's runs and of
 * 1080's run b, and the first of 1080's others, go to the dump.
 */
#define PROBE_MODES			4U
static const struct probe_mode probe_small_modes[PROBE_MODES] = {
	{ "a", 0, PROBE_PUSH_NONE, 2U },
	{ "b", 1, PROBE_PUSH_NONE, 2U },
	{ "g", 1, PROBE_PUSH_SETS, 3U },
	{ "f", 1, PROBE_PUSH_DELIMITER, 3U }
};
static const struct probe_mode probe_large_modes[PROBE_MODES] = {
	{ "a", 0, PROBE_PUSH_NONE, 1U },
	{ "b", 1, PROBE_PUSH_NONE, 1U },
	{ "g", 1, PROBE_PUSH_SETS, 1U },
	{ "f", 1, PROBE_PUSH_DELIMITER, 1U }
};

/* The access unit delimiter of push f (any slice types may follow). */
static const uint8_t probe_delimiter[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };

/*
 * What one run saw: the orders judged by the frames' checksums, and again
 * by their time stamps (each unit's decode index), which a frame keeps
 * even when its pixels differ from the reference; and the time stamps in
 * output order.
 */
struct probe_result {
	unsigned outputs;
	unsigned in_decode_order;
	unsigned in_display_order;
	unsigned unknown;
	unsigned luma_only;
	unsigned max_lag;
	int last_display;
	unsigned pts_in_decode_order;
	unsigned pts_in_display_order;
	unsigned pts_unknown;
	unsigned pts_max_lag;
	int pts_last_display;
	int pts[PROBE_MAX_FRAMES];
	unsigned dumps_left;
	int error;
};

static void probe_spawn(void);
static void probe_main(void *argument);
static void probe_run(const struct probe_stream *stream, const struct probe_mode *mode);
static int send_unit(struct drv_rpi4_mmal_decoder *decoder, const struct probe_stream *stream, size_t start, size_t end, const struct probe_mode *mode, int64_t pts);
static int push(struct drv_rpi4_mmal_decoder *decoder, const struct probe_stream *stream, size_t start, size_t end, const struct probe_mode *mode);
static int copy_nals(const struct probe_stream *stream, size_t start, size_t end, const struct probe_mode *mode, enum probe_nals which, uint8_t *staging, uint32_t capacity, size_t *used);
static void map_frame(const struct probe_stream *stream, const char *run, unsigned out, int reference, const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels);
static unsigned frame_map(const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels, uint32_t *words);
static uint32_t rows_sum(const uint8_t *plane, uint32_t pitch, uint32_t left, uint32_t width, uint32_t top, uint32_t rows);
static void dump_frame(const struct probe_stream *stream, const char *run, unsigned out, int pts_display, int display, const char *match, uint32_t sum, const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels);
static void collect(struct drv_rpi4_mmal_decoder *decoder, const struct probe_stream *stream, const char *run, unsigned sent, unsigned timeout_ms, struct probe_result *result);
static uint32_t frame_sum(const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels, uint32_t *luma);
static const char *order_name(unsigned outputs, unsigned in_decode_order, unsigned max_lag, unsigned in_display_order);
static uint64_t now_us(void);

/*
 * Publishes the probe's device; the probe runs when it is written to.
 *
 * Nothing else happens on the boot path: no thread, no wait.
 */
int
drv_rpi4_mmal_probe_start(void)
{
	int error;

	/* /dev/mmal-probe, whose first write calls probe_spawn. */
	error = drv_rpi4_mmal_dump_publish(probe_spawn);
	if (error != 0)
		return error;

	/* Succeeded: a write starts the probe. */
	return 0;
}

/* Starts the probe's thread (the first write to /dev/mmal-probe asks for it). */
static void
probe_spawn(void)
{
	struct thread *thread;
	int error;

	/* The probe runs once, in its own thread. */
	error = kthread_create(probe_main, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("rpi4-mmal-probe: no thread (%d)\n", error);
		return;
	}

	/* Runs it. */
	thread_start(thread);
}

/* Waits for VCHIQ, then runs each stream each way. */
static void
probe_main(
	void *argument)
{
	struct probe_stream small;
	struct probe_stream large;
	unsigned index;
	uint64_t deadline;
	uint64_t now;
	bool connected;

	/* Says that it runs, then waits for the firmware's connection. */
	(void)argument;
	kern_logf("rpi4-mmal-probe: started; waiting up to %u ms for VCHIQ\n", PROBE_CONNECT_MS);
	deadline = sched_ticks() + kern_ms_to_ticks(PROBE_CONNECT_MS);
	connected = drv_rpi4_vchiq_connected();
	now = sched_ticks();
	while (!connected && now < deadline) {
		kern_usleep_range(100000U, 110000U);
		connected = drv_rpi4_vchiq_connected();
		now = sched_ticks();
	}

	/* Gives up on a firmware that never connected. */
	if (!connected) {
		kern_logf("rpi4-mmal-probe: VCHIQ did not connect in %u ms\n", PROBE_CONNECT_MS);
		return;
	}

	/* Runs; the frames are kept in /dev/mmal-probe. */
	kern_logf("rpi4-mmal-probe: running\n");

	/* The two streams. */
	small.name = "352";
	small.file = "pb-high-352-pyramid.h264";
	small.data = probe_small_stream;
	small.bytes = sizeof(probe_small_stream);
	small.width = probe_small_width;
	small.height = probe_small_height;
	small.display_sums = probe_small_display_sums;
	small.luma_sums = probe_small_luma_sums;
	small.maps = probe_small_maps;
	small.decode_order = probe_small_decode_order;
	small.frames = sizeof(probe_small_decode_order);
	large.name = "1080";
	large.file = "pb-high-1080.h264";
	large.data = probe_large_stream;
	large.bytes = sizeof(probe_large_stream);
	large.width = probe_large_width;
	large.height = probe_large_height;
	large.display_sums = probe_large_display_sums;
	large.luma_sums = probe_large_luma_sums;
	large.maps = probe_large_maps;
	large.decode_order = probe_large_decode_order;
	large.frames = sizeof(probe_large_decode_order);

	/* Each stream three ways. */
	for (index = 0; index < PROBE_MODES; index++)
		probe_run(&small, &probe_small_modes[index]);
	for (index = 0; index < PROBE_MODES; index++)
		probe_run(&large, &probe_large_modes[index]);
	kern_logf("rpi4-mmal-probe: done\n");
}

/* Decodes one stream once and logs what came out. */
static void
probe_run(
	const struct probe_stream *stream,
	const struct probe_mode *mode)
{
	struct drv_rpi4_mmal_decoder *decoder;
	struct probe_result result;
	const char *run;
	char sequence[PROBE_MAX_FRAMES * 4U + 1U];
	size_t offset;
	size_t end;
	size_t used;
	unsigned sent;
	unsigned index;
	int error;

	/* The run's name and dumps. */
	run = mode->name;
	kern_memset(&result, 0, sizeof(result));
	result.last_display = -1;
	result.pts_last_display = -1;
	result.dumps_left = mode->dumps;

	/* A decoder of the stream's size. */
	error = drv_rpi4_mmal_decoder_create(stream->width, stream->height, PROBE_INPUT_BYTES, 0, &decoder);
	if (error != 0) {
		kern_logf("rpi4-mmal-probe: %s run=%s decoder not created (%d)\n", stream->name, run, error);
		return;
	}

	/* Sends each access unit and takes whatever comes out right after it. */
	offset = 0;
	sent = 0;
	while (sent < PROBE_MAX_FRAMES) {
		error = drv_rpi4_h264_access_unit(stream->data, stream->bytes, offset, &end);
		if (error != 0)
			break;
		error = send_unit(decoder, stream, offset, end, mode, (int64_t)sent);
		if (error != 0) {
			kern_logf("rpi4-mmal-probe: %s run=%s input %u not sent (%d)\n", stream->name, run, sent, error);
			result.error = error;
			break;
		}

		/* One more unit is in; the next starts where it ended. */
		sent++;
		offset = end;
		if (mode->push != PROBE_PUSH_NONE && sent == 1U) {
			error = push(decoder, stream, 0, end, mode);
			if (error != 0) {
				kern_logf("rpi4-mmal-probe: %s run=%s push not sent (%d)\n", stream->name, run, error);
				result.error = error;
				break;
			}
		}

		/* Takes what came out. */
		collect(decoder, stream, run, sent, PROBE_OUTPUT_MS, &result);
		if (result.error != 0)
			break;
	}

	/* Takes what is left (without an end of stream the last held pictures may stay). */
	if (result.error == 0)
		collect(decoder, stream, run, sent, PROBE_DRAIN_MS, &result);
	drv_rpi4_mmal_decoder_destroy(decoder);

	/* The time stamps in output order. */
	used = 0;
	sequence[0] = '\0';
	for (index = 0; index < result.outputs && index < PROBE_MAX_FRAMES; index++) {
		(void)kern_snprintf(sequence + used, sizeof(sequence) - used, "%s%d", index == 0 ? "" : ",", result.pts[index]);
		used += kern_strlen(sequence + used);
	}

	/*
	 * The run's order, by the checksums and by the time stamps: decode
	 * order without delay, display order, or neither.
	 */
	kern_logf("rpi4-mmal-probe: %s run=%s order=%s order_pts=%s inputs=%u outputs=%u missing=%u unknown=%u luma_only=%u lag=%u lag_pts=%u pts_unknown=%u error=%d pts_seq=%s\n",
		  stream->name, run,
		  order_name(result.outputs, result.in_decode_order, result.max_lag, result.in_display_order),
		  order_name(result.outputs, result.pts_in_decode_order, result.pts_max_lag, result.pts_in_display_order),
		  sent, result.outputs, stream->frames - result.outputs, result.unknown, result.luma_only,
		  result.max_lag, result.pts_max_lag, result.pts_unknown, result.error, sequence);
}

/* Names an order from the counts that agree with each. */
static const char *
order_name(
	unsigned outputs,
	unsigned in_decode_order,
	unsigned max_lag,
	unsigned in_display_order)
{
	/* Decode order without delay, display order, or neither. */
	if (outputs != 0 && in_decode_order == outputs && max_lag == 0)
		return "decode";
	if (outputs != 0 && in_display_order == outputs)
		return "display";

	/* Succeeded: neither. */
	return "other";
}

/*
 * Sends one access unit, rewriting its SPS when asked, with its decode
 * index as the time stamp.
 */
static int
send_unit(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct probe_stream *stream,
	size_t start,
	size_t end,
	const struct probe_mode *mode,
	int64_t pts)
{
	uint8_t *staging;
	uint32_t capacity;
	size_t used;
	int error;

	/* Every NAL unit of the access unit, as one frame. */
	error = drv_rpi4_mmal_decoder_input(decoder, PROBE_INPUT_MS, &staging, &capacity);
	if (error != 0)
		return error;
	error = copy_nals(stream, start, end, mode, PROBE_NALS_ALL, staging, capacity, &used);
	if (error != 0)
		return error;
	error = drv_rpi4_mmal_decoder_send(decoder, (uint32_t)used, pts, 0);
	if (error != 0)
		return error;

	/* Succeeded: the firmware has the unit. */
	return 0;
}

/*
 * Sends the run's push after the first unit ([start, end)): a delimiter
 * in a buffer of its own, or the unit's parameter sets again as codec
 * configuration.
 */
static int
push(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct probe_stream *stream,
	size_t start,
	size_t end,
	const struct probe_mode *mode)
{
	uint8_t *staging;
	uint32_t capacity;
	size_t used;
	int error;

	/* The delimiter or the sets, without a time stamp. */
	error = drv_rpi4_mmal_decoder_input(decoder, PROBE_INPUT_MS, &staging, &capacity);
	if (error != 0)
		return error;
	if (mode->push == PROBE_PUSH_DELIMITER) {
		if (capacity < sizeof(probe_delimiter))
			return ENOSPC;
		kern_memcpy(staging, probe_delimiter, sizeof(probe_delimiter));
		used = sizeof(probe_delimiter);
	} else {
		error = copy_nals(stream, start, end, mode, PROBE_NALS_SETS, staging, capacity, &used);
		if (error != 0)
			return error;
		if (used == 0)
			return 0;
	}

	/* Sends the push. */
	error = drv_rpi4_mmal_decoder_send(decoder, (uint32_t)used, DRV_RPI4_MMAL_FRAME_NO_TIME,
					   mode->push == PROBE_PUSH_SETS);
	if (error != 0)
		return error;

	/* Succeeded: the push is sent. */
	return 0;
}

/*
 * Copies the NAL units of [start, end) that which selects (all, or the
 * parameter sets) into staging, each with a four-byte start code; an SPS
 * is rewritten when the mode asks.  used receives the bytes written.
 */
static int
copy_nals(
	const struct probe_stream *stream,
	size_t start,
	size_t end,
	const struct probe_mode *mode,
	enum probe_nals which,
	uint8_t *staging,
	uint32_t capacity,
	size_t *used)
{
	size_t nal_start;
	size_t nal_end;
	size_t written;
	size_t cursor;
	unsigned type;
	bool set;
	int error;

	/* Each unit in turn. */
	*used = 0;
	cursor = start;
	for (;;) {
		error = drv_rpi4_h264_next_nal(stream->data, end, cursor, &nal_start, &nal_end);
		if (error != 0)
			break;
		cursor = nal_end;
		type = stream->data[nal_start] & 0x1fU;
		set = false;
		if (type == DRV_RPI4_H264_NAL_SPS || type == DRV_RPI4_H264_NAL_PPS)
			set = true;
		if (which == PROBE_NALS_SETS && !set)
			continue;
		if (mode->rewrite && type == DRV_RPI4_H264_NAL_SPS) {
			if (capacity - *used < DRV_RPI4_H264_SPS_MAX)
				return ENOSPC;
			error = drv_rpi4_h264_sps_no_reorder(stream->data + nal_start, nal_end - nal_start,
							     staging + *used, DRV_RPI4_H264_SPS_MAX, &written);
			if (error != 0)
				return error;
			*used += written;
			continue;
		}

		/* Any other unit as it is, after a four-byte start code. */
		if (capacity - *used < 4U + (nal_end - nal_start))
			return ENOSPC;
		staging[*used] = 0;
		staging[*used + 1U] = 0;
		staging[*used + 2U] = 0;
		staging[*used + 3U] = 1;
		kern_memcpy(staging + *used + 4U, stream->data + nal_start, nal_end - nal_start);
		*used += 4U + (nal_end - nal_start);
	}

	/* Succeeded: the units are in the staging. */
	return 0;
}

/*
 * Receives every frame that comes within timeout_ms, identifies it and
 * logs it.  sent is the number of inputs sent so far.
 */
static void
collect(
	struct drv_rpi4_mmal_decoder *decoder,
	const struct probe_stream *stream,
	const char *run,
	unsigned sent,
	unsigned timeout_ms,
	struct probe_result *result)
{
	struct drv_rpi4_mmal_frame frame;
	const uint8_t *pixels;
	uint8_t *copy;
	uint64_t started;
	uint64_t copy_us;
	uint32_t sum;
	uint32_t luma;
	unsigned index;
	unsigned lag;
	unsigned pts_lag;
	int display;
	int decode;
	int pts_display;
	int pts_decode;
	int reference;
	const char *match;
	int error;

	/* Takes frames until none comes in time. */
	for (;;) {
		error = drv_rpi4_mmal_decoder_receive(decoder, timeout_ms, &frame, &pixels);
		if (error == ETIMEDOUT)
			return;
		if (error != 0) {
			result->error = error;
			return;
		}

		/* An empty end of stream carries no picture; its buffer goes back. */
		if (frame.length == 0) {
			(void)drv_rpi4_mmal_decoder_release(decoder, frame.handle);
			continue;
		}

		/* Identifies the picture by its checksum, else by its luma's (a chroma layout fault). */
		sum = frame_sum(&frame, pixels, &luma);
		display = -1;
		match = "none";
		for (index = 0; index < stream->frames; index++) {
			if (stream->display_sums[index] == sum) {
				display = (int)index;
				match = "whole";
				break;
			}
		}

		/* Else the picture whose luma matches. */
		for (index = 0; index < stream->frames && display < 0; index++) {
			if (stream->luma_sums[index] == luma) {
				display = (int)index;
				match = "luma";
				result->luma_only++;
				break;
			}
		}

		/* Its place in decode order. */
		decode = -1;
		for (index = 0; index < stream->frames; index++) {
			if (display >= 0 && stream->decode_order[index] == (uint8_t)display) {
				decode = (int)index;
				break;
			}
		}

		/* The lag: inputs sent after the picture's own. */
		lag = 0;
		if (decode >= 0 && sent > (unsigned)decode + 1U)
			lag = sent - (unsigned)decode - 1U;
		if (lag > result->max_lag)
			result->max_lag = lag;

		/* Identifies the picture by its time stamp, the decode index of the unit it came from. */
		pts_decode = -1;
		pts_display = -1;
		if (frame.pts >= 0 && frame.pts < (int64_t)stream->frames) {
			pts_decode = (int)frame.pts;
			pts_display = (int)stream->decode_order[pts_decode];
		}

		/* The units sent after the picture's own, by its time stamp. */
		pts_lag = 0;
		if (pts_decode >= 0 && sent > (unsigned)pts_decode + 1U)
			pts_lag = sent - (unsigned)pts_decode - 1U;
		if (pts_lag > result->pts_max_lag)
			result->pts_max_lag = pts_lag;

		/* Counts the orders the output agrees with, by checksum and by time stamp. */
		if (decode < 0) {
			result->unknown++;
		} else {
			if (decode == (int)result->outputs)
				result->in_decode_order++;
			if (display == result->last_display + 1)
				result->in_display_order++;
			result->last_display = display;
		}

		/* The same by the time stamps. */
		if (pts_decode < 0) {
			result->pts_unknown++;
		} else {
			if (pts_decode == (int)result->outputs)
				result->pts_in_decode_order++;
			if (pts_display == result->pts_last_display + 1)
				result->pts_in_display_order++;
			result->pts_last_display = pts_display;
		}

		/* Keeps the time stamp for the run's sequence. */
		if (result->outputs < PROBE_MAX_FRAMES)
			result->pts[result->outputs] = pts_decode;
		result->outputs++;

		/* The time to copy a frame out of the uncached bounce (the large stream only). */
		copy_us = 0;
		if (frame.length > 1024U * 1024U) {
			copy = kern_malloc(frame.length);
			if (copy != NULL) {
				started = now_us();
				kern_memcpy(copy, pixels, frame.length);
				copy_us = now_us() - started;
				kern_free(copy);
			}
		}

		kern_logf("rpi4-mmal-probe: %s run=%s out=%u pts=%lld pts_display=%d display=%d decode=%d match=%s sent=%u lag=%u sum=%08x luma=%08x flags=%x len=%u fmt=%08x %ux%u crop %ux%u pitch %u/%u copy_us=%u\n",
			  stream->name, run, result->outputs - 1U, (long long)frame.pts, pts_display, display, decode, match, sent, lag,
			  (unsigned)sum, (unsigned)luma, (unsigned)frame.flags, (unsigned)frame.length, (unsigned)frame.encoding,
			  (unsigned)frame.width, (unsigned)frame.height, (unsigned)frame.crop_width, (unsigned)frame.crop_height,
			  (unsigned)frame.plane_pitch[0], (unsigned)frame.plane_pitch[1], (unsigned)copy_us);

		/* Where it differs from its reference (the matched picture, else its time stamp's), and the dump. */
		reference = display;
		if (reference < 0)
			reference = pts_display;
		map_frame(stream, run, result->outputs - 1U, reference, &frame, pixels);
		if (result->dumps_left > 0) {
			result->dumps_left--;
			dump_frame(stream, run, result->outputs - 1U, pts_display, display, match, sum, &frame, pixels);
		}

		/* The frame's buffer goes back to the firmware. */
		(void)drv_rpi4_mmal_decoder_release(decoder, frame.handle);
	}
}

/*
 * Computes FNV-1a 32 over a frame's visible NV12 bytes: the luma rows,
 * then the interleaved chroma rows.  luma receives the checksum of the
 * luma rows alone.
 */
static uint32_t
frame_sum(
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels,
	uint32_t *luma)
{
	uint32_t sum;
	uint32_t pitch_y;
	uint32_t pitch_uv;
	uint32_t offset_uv;
	uint32_t row;
	uint32_t column;
	uint32_t offset_v;
	const uint8_t *line;
	const uint8_t *line_v;

	/* The planes' layout, from the buffer, else the format's stride and height. */
	pitch_y = frame->plane_pitch[0];
	pitch_uv = frame->plane_pitch[1];
	offset_uv = frame->plane_offset[1];
	if (frame->planes < 2U || pitch_y == 0) {
		pitch_y = frame->width;
		pitch_uv = frame->width;
		offset_uv = frame->width * frame->height;
	}

	/* The luma rows. */
	sum = PROBE_FNV_BASIS;
	for (row = 0; row < frame->crop_height; row++) {
		line = pixels + frame->plane_offset[0] + (size_t)row * pitch_y;
		for (column = 0; column < frame->crop_width; column++) {
			sum ^= line[column];
			sum *= PROBE_FNV_PRIME;
		}
	}

	/* The luma's checksum alone. */
	*luma = sum;

	/* An I420 frame's chroma is two planes; it is summed interleaved, as NV12's. */
	if (frame->encoding == PROBE_ENCODING_I420) {
		pitch_uv = frame->width / 2U;
		offset_v = frame->width * frame->height + pitch_uv * (frame->height / 2U);
		for (row = 0; row < frame->crop_height / 2U; row++) {
			line = pixels + (size_t)frame->width * frame->height + (size_t)row * pitch_uv;
			line_v = pixels + offset_v + (size_t)row * pitch_uv;
			for (column = 0; column < frame->crop_width / 2U; column++) {
				sum ^= line[column];
				sum *= PROBE_FNV_PRIME;
				sum ^= line_v[column];
				sum *= PROBE_FNV_PRIME;
			}
		}

		/* Succeeded: the checksum. */
		return sum;
	}

	/* The chroma rows. */
	for (row = 0; row < frame->crop_height / 2U; row++) {
		line = pixels + offset_uv + (size_t)row * pitch_uv;
		for (column = 0; column < frame->crop_width; column++) {
			sum ^= line[column];
			sum *= PROBE_FNV_PRIME;
		}
	}

	/* Succeeded: the checksum. */
	return sum;
}

/*
 * Logs where a frame differs from the reference picture's map: a '.' for
 * each band and strip that matches, an 'x' for each that does not.
 */
static void
map_frame(
	const struct probe_stream *stream,
	const char *run,
	unsigned out,
	int reference,
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels)
{
	uint32_t words[PROBE_MAP_WORDS];
	char marks[PROBE_MAP_WORDS + 4U];
	const uint32_t *expected;
	unsigned count;
	unsigned bands_y;
	unsigned bands_c;
	unsigned strips;
	unsigned index;
	unsigned at;

	/* Only an NV12 frame of the stream's size with a known reference. */
	if (reference < 0 || (unsigned)reference >= stream->frames || pixels == NULL)
		return;
	if (frame->encoding != PROBE_ENCODING_NV12 || frame->crop_width != stream->width || frame->crop_height != stream->height)
		return;
	count = frame_map(frame, pixels, words);
	if (count == 0)
		return;
	expected = stream->maps + (size_t)reference * count;

	/* The marks: luma bands, chroma bands, luma strips, chroma strips, with a space between. */
	bands_y = (stream->height + 15U) / 16U;
	bands_c = (stream->height / 2U + 7U) / 8U;
	strips = (stream->width + PROBE_STRIP_BYTES - 1U) / PROBE_STRIP_BYTES;
	at = 0;
	for (index = 0; index < count; index++) {
		if (index == bands_y || index == bands_y + bands_c || index == bands_y + bands_c + strips)
			marks[at++] = ' ';
		marks[at++] = words[index] == expected[index] ? '.' : 'x';
	}

	/* Logs the marks. */
	marks[at] = '\0';
	kern_logf("rpi4-mmal-probe: %s run=%s out=%u map ref=%d Y|C|Ystrip|Cstrip %s\n", stream->name, run, out, reference, marks);
}

/*
 * Computes a frame's map (see PROBE_MAP_WORDS) as make-probe-data.py does
 * over the reference: each band's rows, then each strip's columns down
 * every row.  Returns the number of words, zero if it does not fit.
 */
static unsigned
frame_map(
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels,
	uint32_t *words)
{
	const uint8_t *luma;
	const uint8_t *chroma;
	uint32_t width;
	uint32_t height;
	uint32_t pitch_y;
	uint32_t pitch_uv;
	uint32_t top;
	uint32_t left;
	uint32_t rows;
	uint32_t across;
	unsigned count;

	/* The planes, as frame_sum finds them. */
	width = frame->crop_width;
	height = frame->crop_height;
	pitch_y = frame->plane_pitch[0];
	pitch_uv = frame->plane_pitch[1];
	luma = pixels + frame->plane_offset[0];
	chroma = pixels + frame->plane_offset[1];
	if (frame->planes < 2U || pitch_y == 0) {
		pitch_y = frame->width;
		pitch_uv = frame->width;
		luma = pixels;
		chroma = pixels + (size_t)frame->width * frame->height;
	}

	/* Refuses a frame whose map would not fit. */
	if ((height + 15U) / 16U + (height / 2U + 7U) / 8U + 2U * ((width + PROBE_STRIP_BYTES - 1U) / PROBE_STRIP_BYTES) > PROBE_MAP_WORDS)
		return 0;

	/* The luma bands. */
	count = 0;
	for (top = 0; top < height; top += 16U) {
		/* The last band may be shorter. */
		rows = 16U;
		if (height - top < rows)
			rows = height - top;
		words[count++] = rows_sum(luma, pitch_y, 0, width, top, rows);
	}

	/* The chroma bands. */
	for (top = 0; top < height / 2U; top += 8U) {
		/* The last band may be shorter. */
		rows = 8U;
		if (height / 2U - top < rows)
			rows = height / 2U - top;
		words[count++] = rows_sum(chroma, pitch_uv, 0, width, top, rows);
	}

	/* The luma strips. */
	for (left = 0; left < width; left += PROBE_STRIP_BYTES) {
		/* The last strip may be narrower. */
		across = PROBE_STRIP_BYTES;
		if (width - left < across)
			across = width - left;
		words[count++] = rows_sum(luma, pitch_y, left, across, 0, height);
	}

	/* The chroma strips. */
	for (left = 0; left < width; left += PROBE_STRIP_BYTES) {
		/* The last strip may be narrower. */
		across = PROBE_STRIP_BYTES;
		if (width - left < across)
			across = width - left;
		words[count++] = rows_sum(chroma, pitch_uv, left, across, 0, height / 2U);
	}

	/* Succeeded: the map's words. */
	return count;
}

/* Computes FNV-1a 32 over width bytes from column left of each of rows rows from row top. */
static uint32_t
rows_sum(
	const uint8_t *plane,
	uint32_t pitch,
	uint32_t left,
	uint32_t width,
	uint32_t top,
	uint32_t rows)
{
	const uint8_t *line;
	uint32_t sum;
	uint32_t row;
	uint32_t column;

	/* Row by row, left to right. */
	sum = PROBE_FNV_BASIS;
	for (row = 0; row < rows; row++) {
		line = plane + (size_t)(top + row) * pitch + left;
		for (column = 0; column < width; column++) {
			sum ^= line[column];
			sum *= PROBE_FNV_PRIME;
		}
	}

	/* Succeeded: the checksum. */
	return sum;
}

/*
 * Keeps a frame's bytes, exactly as the decoder gave them, with a header
 * that describes them, in /dev/mmal-probe (rpi4-mmal-dump.h).
 */
static void
dump_frame(
	const struct probe_stream *stream,
	const char *run,
	unsigned out,
	int pts_display,
	int display,
	const char *match,
	uint32_t sum,
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels)
{
	char header[DRV_RPI4_MMAL_DUMP_HEADER];
	int error;

	/* A frame that went straight to a target is not in the bounce memory. */
	if (pixels == NULL)
		return;
	(void)kern_snprintf(header, sizeof(header),
			    "zdump stream=%s file=%s run=%s out=%u pts=%lld pts_display=%d display=%d match=%s sum=%08x fmt=%08x width=%u height=%u crop_width=%u crop_height=%u planes=%u offset0=%u offset1=%u pitch0=%u pitch1=%u length=%u flags=%x",
			    stream->name, stream->file, run, out, (long long)frame->pts, pts_display, display, match, (unsigned)sum,
			    (unsigned)frame->encoding, (unsigned)frame->width, (unsigned)frame->height,
			    (unsigned)frame->crop_width, (unsigned)frame->crop_height, (unsigned)frame->planes,
			    (unsigned)frame->plane_offset[0], (unsigned)frame->plane_offset[1],
			    (unsigned)frame->plane_pitch[0], (unsigned)frame->plane_pitch[1],
			    (unsigned)frame->length, (unsigned)frame->flags);
	error = drv_rpi4_mmal_dump_add(header, pixels, frame->length);
	if (error != 0)
		kern_logf("rpi4-mmal-probe: %s run=%s out=%u not dumped (%d)\n", stream->name, run, out, error);
}

/* Reports the monotonic time in microseconds. */
static uint64_t
now_us(void)
{
	struct timespec now;
	int error;

	/* The monotonic clock, or zero when it cannot be read. */
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: microseconds since boot. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}
