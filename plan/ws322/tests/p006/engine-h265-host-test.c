/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the Wave5 video engine's H.265 decode (ws322-p006,
 * src/drivers/vpu/wave5-video.c) over a stand-in core: both decodes
 * offered; an H.265 stream opens an H.265 instance at its first IRAP
 * picture and not before; the VPS, SPS and PPS go in front of an IRAP
 * picture, a changed PPS alone in front of the next picture, nothing in
 * front of a picture whose sets were sent; a new size at an IRAP picture
 * starts another instance, before one it is an error; a picture without
 * its sets, or with a set the writer refuses, is an error; an H.264 stream
 * still sends its SPS and PPS to an H.264 instance.
 *
 *   engine-h265-host-test
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
	unsigned codec;
	int closes;
	unsigned pieces;
	uint8_t first[4];
	int sequences;
	int decodes;
	uint32_t width;
	uint32_t height;
	int32_t decoded;
	int32_t order;
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
	(void)ring_size;
	core.opens++;
	core.codec = codec;
	*stream = instance;
	return 0;
}

int drv_wave5_close(struct drv_wave5_stream *stream) { (void)stream; core.closes++; return 0; }
int drv_wave5_update_stream(struct drv_wave5_stream *stream) { (void)stream; return 0; }
int drv_wave5_lost(const struct drv_wave5_stream *stream) { (void)stream; return 0; }
int drv_wave5_flush(struct drv_wave5_stream *stream) { (void)stream; return 0; }
int drv_wave5_recover(struct drv_wave5 *owner) { (void)owner; return 0; }
void drv_wave5_unread(struct drv_wave5_stream *stream) { (void)stream; }

/* Remembers the pieces of a picture: how many, and the byte after each one's start code (its NAL header). */
int
drv_wave5_append_pieces(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_piece *pieces,
	unsigned count)
{
	unsigned index;

	(void)stream;
	core.pieces = count;
	memset(core.first, 0, sizeof(core.first));
	for (index = 0; index < count && index < 4U; index++)
		core.first[index] = pieces[index].length > 4U ? pieces[index].bytes[4] : 0U;
	return 0;
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
	sequence->required = 6;
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
	(void)linear;
	(void)count;
	(void)stride;
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
	(void)set;
	(void)clear;
	(void)flags;
	return 0;
}

int
drv_wave5_decode(
	struct drv_wave5_stream *stream,
	struct drv_wave5_result *result)
{
	(void)stream;
	memset(result, 0, sizeof(*result));
	core.decodes++;
	result->success = 1;
	result->decoded = core.decoded;
	result->order = core.order;
	return 0;
}

void drv_wave5_positions(const struct drv_wave5_stream *stream, uint32_t *read, uint32_t *write) { (void)stream; *read = 0; *write = 0; }
void drv_wave5_interrupts(struct drv_wave5 *owner, unsigned *taken, unsigned *polled, uint32_t *reasons) { (void)owner; *taken = 0; *polled = 0; *reasons = 0; }
uint32_t drv_wave5_wraps(const struct drv_wave5_stream *stream) { (void)stream; return 0; }
uint32_t drv_wave5_last_reason(struct drv_wave5 *owner) { (void)owner; return 0; }

/* The render node's pool: count pictures 16 MiB apart, pitch 2048. */
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
	for (index = 0; index < count; index++) {
		pictures[index].luma = 0x10000000U + index * 0x1000000U;
		pictures[index].chroma = pictures[index].luma + 0x200000U;
	}
	*pitch = 2048U;
	return 0;
}

/* A 1080p Main sequence: its VPS, SPS and PPS, and an H.264 one for the other stream. */
static struct vpu_h265_vps vps;
static struct vpu_h265_sps sps;
static struct vpu_h265_pps pps;
static struct vpu_h264_sps h264_sps;
static struct vpu_h264_pps h264_pps;
static const uint8_t slices[] = { 0, 0, 0, 1, 0x26, 0x01, 0xaf, 0x00, 0x33 };

/* Decodes one H.265 picture with the sets given (NULL for none). */
static int
decode(
	void *stream,
	uint32_t flags,
	const struct vpu_h265_vps *with_vps,
	struct drv_powervr_video_result *result)
{
	struct drv_powervr_video_decode job;

	memset(&job, 0, sizeof(job));
	job.bitstream = slices;
	job.length = sizeof(slices);
	job.vps = with_vps;
	job.h265_sps = &sps;
	job.h265_pps = &pps;
	job.flags = flags;
	job.order = core.order;
	job.setup = 0;
	return core.ops->decode(stream, &job, result);
}

int
main(void)
{
	struct drv_powervr_video_open open;
	struct drv_powervr_video_result result;
	struct drv_powervr_video_decode job;
	void *stream;
	void *other;
	int error;

	/* The sets: Main, level 4.1, 1920x1080 in 64x64 blocks. */
	memset(&vps, 0, sizeof(vps));
	vps.has_ptl = 1;
	vps.ptl.profile_idc = 1;
	vps.ptl.level = 6;
	vps.has_ordering = 1;
	vps.ordering.buffering_minus1[0] = 4;
	vps.ordering.reorder[0] = 2;
	memset(&sps, 0, sizeof(sps));
	sps.has_ptl = 1;
	sps.ptl = vps.ptl;
	sps.has_ordering = 1;
	sps.ordering = vps.ordering;
	sps.chroma_format_idc = 1;
	sps.width = 1920;
	sps.height = 1080;
	sps.log2_max_poc_lsb_minus4 = 4;
	sps.log2_diff_cb = 3;
	sps.log2_diff_tb = 3;
	sps.flags = VPU_H265_SPS_SAO | VPU_H265_SPS_TEMPORAL_MVP;
	memset(&pps, 0, sizeof(pps));
	pps.flags = VPU_H265_PPS_FILTER_SLICES;

	/* Both decodes are offered. */
	error = drv_wave5_video_register((struct drv_wave5 *)&core);
	check(error == 0 && core.ops != NULL, "registered");
	check(core.ops->operations(core.engine) == (DRV_POWERVR_VIDEO_DECODE_H264 | DRV_POWERVR_VIDEO_DECODE_H265), "both decodes offered");
	open.width = 1920;
	open.height = 1088;
	open.owner = NULL;
	open.pool = pool;
	error = core.ops->open(core.engine, DRV_POWERVR_VIDEO_DECODE_H265, &open, &stream);
	check(error == 0, "an H.265 stream opened");
	check(core.ops->open(core.engine, DRV_POWERVR_VIDEO_ENCODE_H265, &open, &other) == EINVAL, "no encode stream");
	core.width = 1920;
	core.height = 1080;

	/* A picture that is not IRAP first: an error, no instance. */
	error = decode(stream, 0, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.opens == 0, "no instance before an IRAP picture");

	/* The IRAP picture: an H.265 instance, the three sets then the slices, the sequence, the picture. */
	core.decoded = 2;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_INTRA | DRV_POWERVR_VIDEO_PICTURE_IDR, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && result.index == 2, "the IRAP picture decoded");
	check(core.opens == 1 && core.codec == DRV_WAVE5_H265, "an H.265 instance");
	check(core.pieces == 4U && core.first[0] == 0x40U && core.first[1] == 0x42U && core.first[2] == 0x44U && core.first[3] == 0x26U,
	      "VPS, SPS, PPS, then the slices");
	check(core.sequences == 1 && result.width == 1920U && result.height == 1080U, "the sequence set up");

	/* The next picture with the same sets: the slices alone. */
	core.decoded = 3;
	error = decode(stream, 0, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.pieces == 1U, "no set again");

	/* A changed PPS: it alone in front. */
	pps.init_qp_minus26 = 3;
	core.decoded = 4;
	error = decode(stream, 0, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.pieces == 2U && core.first[0] == 0x44U, "the changed PPS alone");

	/* The next IRAP picture (a CRA): every set again, the same instance. */
	core.decoded = 0;
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_INTRA, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.pieces == 4U && core.opens == 1, "every set at an IRAP picture");

	/* A new size before an IRAP picture is an error; at one, another instance. */
	sps.width = 1280;
	sps.height = 720;
	core.width = 1280;
	core.height = 720;
	core.decodes = 0;
	error = decode(stream, 0, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.decodes == 0, "a new size before an IRAP picture is an error");
	error = decode(stream, DRV_POWERVR_VIDEO_PICTURE_INTRA, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.opens == 2 && core.closes == 1 && core.sequences == 2,
	      "a new size at an IRAP picture opens another instance");

	/* A picture without its VPS, and one with a set the writer refuses: errors, nothing decoded. */
	core.decodes = 0;
	error = decode(stream, 0, NULL, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.decodes == 0, "no VPS is an error");
	sps.id = 20;
	error = decode(stream, 0, &vps, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_ERROR && core.decodes == 0, "an SPS the writer refuses is an error");
	sps.id = 0;
	check(core.ops->close(stream) == 0, "the H.265 stream closed");

	/* An H.264 stream: an H.264 instance with its SPS and PPS. */
	memset(&h264_sps, 0, sizeof(h264_sps));
	h264_sps.flags = VPU_H264_SPS_FRAME_MBS_ONLY;
	h264_sps.profile_idc = 100;
	h264_sps.level = 11;
	h264_sps.chroma_format_idc = 1;
	h264_sps.max_num_ref_frames = 4;
	h264_sps.pic_width_in_mbs_minus1 = 79;
	h264_sps.pic_height_in_map_units_minus1 = 44;
	memset(&h264_pps, 0, sizeof(h264_pps));
	error = core.ops->open(core.engine, DRV_POWERVR_VIDEO_DECODE_H264, &open, &other);
	check(error == 0, "an H.264 stream opened");
	memset(&job, 0, sizeof(job));
	job.bitstream = slices;
	job.length = sizeof(slices);
	job.sps = &h264_sps;
	job.pps = &h264_pps;
	job.flags = DRV_POWERVR_VIDEO_PICTURE_INTRA | DRV_POWERVR_VIDEO_PICTURE_IDR;
	job.setup = 0;
	core.decoded = 1;
	error = core.ops->decode(other, &job, &result);
	check(error == 0 && result.status == DRV_POWERVR_VIDEO_COMPLETE && core.codec == DRV_WAVE5_H264, "an H.264 instance");
	check(core.pieces == 3U && core.first[0] == 0x67U && core.first[1] == 0x68U, "SPS, PPS, then the slices");
	check(core.ops->close(other) == 0, "the H.264 stream closed");

	printf("engine-h265: %d passed, %d failed\n", passes, failures);
	return failures != 0;
}
