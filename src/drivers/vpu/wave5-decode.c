/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wave5 decoder's instances.
 *
 * An instance owns a work area the firmware keeps its state in and a
 * bitstream ring the host puts each picture into, from the ring's start,
 * with an explicit end.  The first picture sets the sequence up, which
 * tells the size and how many frame buffers the firmware needs; the host
 * then registers that many pairs, the linear buffers first and the
 * compressed ones, which the instance allocates, after them, eight per
 * command.  Each picture is then decoded by one command whose completion
 * comes as an interrupt, and its result is read with the result query.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/vpu/wave5-private.h"

/*
 * The firmware's numbers for the decoders.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.h
 * (W_HEVC_DEC 0x00, W_AVC_DEC 0x02).
 */
#define STANDARD_H265		0x00U
#define STANDARD_H264		0x02U

/*
 * The work area of a Wave511 decoding instance.
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuconfig.h
 * (WAVE521DEC_WORKBUF_SIZE 1784 KiB).
 */
#define WORK_SIZE		(1784U * 1024U)

/*
 * The arguments of the instance creation: the codec core the instance
 * runs on, the work area, the bitstream ring and its byte order, the
 * secondary memory, the extended address and the command queue's depth
 * less one.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_ADDR_WORK_BASE 0x114 to W5_CMD_BS_PARAM 0x124, W5_CMD_ADDR_SEC_AXI
 * 0x130, W5_CMD_SEC_AXI_SIZE 0x134, W5_CMD_EXT_ADDR 0x138,
 * W5_CMD_NUM_CQ_DEPTH_M1 0x13c, W5_CMD_DEC_VCORE_INFO 0x194), wave5.h
 * (BITSTREAM_ENDIANNESS_BIG_ENDIAN 0xf) and wave5-vpuconfig.h
 * (WAVE521_COMMAND_QUEUE_DEPTH 2).
 */
#define CREATE_WORK_BASE	0x0114U
#define CREATE_WORK_SIZE	0x0118U
#define CREATE_RING_BASE	0x011cU
#define CREATE_RING_SIZE	0x0120U
#define CREATE_RING_ORDER	0x0124U
#define CREATE_SRAM_BASE	0x0130U
#define CREATE_SRAM_SIZE	0x0134U
#define CREATE_EXTENDED		0x0138U
#define CREATE_QUEUE_DEPTH	0x013cU
#define CREATE_CODEC_CORE	0x0194U
#define RING_BYTES_IN_ORDER	0xfU
#define QUEUE_DEPTH		2U

/*
 * The arguments of the sequence and picture commands: where the ring's
 * bytes start and end, how they end (explicitly, at the write position),
 * the user data the host wants, the temporal layers, the sequence changes
 * the firmware reports, the forced output latency, and the secondary
 * memory's use.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_BS_RD_PTR 0x118, W5_BS_WR_PTR 0x11c, W5_BS_OPTION 0x120,
 * W5_CMD_SEQ_CHANGE_ENABLE_FLAG 0x128, W5_CMD_DEC_USER_MASK 0x12c,
 * W5_CMD_DEC_TEMPORAL_ID_PLUS1 0x130, W5_CMD_DEC_FORCE_FB_LATENCY_PLUS1
 * 0x134, W5_USE_SEC_AXI 0x150), wave5.h (BSOPTION_ENABLE_EXPLICIT_END) and
 * wave5-vpuapi.h (INIT_SEQ_NORMAL 1, DEC_PIC_NORMAL 0).
 */
#define STREAM_READ		0x0118U
#define STREAM_WRITE		0x011cU
#define STREAM_OPTIONS		0x0120U
#define PICTURE_CHANGES		0x0128U
#define PICTURE_USER_DATA	0x012cU
#define PICTURE_LAYERS		0x0130U
#define PICTURE_LATENCY		0x0134U
#define PICTURE_SRAM_USE	0x0150U
#define STREAM_EXPLICIT_END	0x1U
#define SEQUENCE_NORMAL		1U
#define PICTURE_NORMAL		0U

/*
 * The sequence changes the firmware is let report: for H.264 the size,
 * the bit depth, the buffer count, the aspect ratio, the video signal and
 * the timing; for H.265 the profile, the size, the bit depth and the
 * buffer count.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.h
 * (SEQ_CHANGE_ENABLE_* bits 5, 16, 18, 19, 21, 23, 29 and the two
 * SEQ_CHANGE_ENABLE_ALL_* sets).
 */
#define CHANGES_H264		((1U << 16) | (1U << 18) | (1U << 19) | (1U << 21) | (1U << 23) | (1U << 29))
#define CHANGES_H265		((1U << 5) | (1U << 16) | (1U << 18) | (1U << 19))

/*
 * The result query's arguments and answers: where user data goes (none
 * here) and its byte order; the read position, the sequence's profile,
 * the bit depths, the frame buffers needed, the sequence change, the
 * size, the crop, the picture's type and order, the decoded and the
 * displayed index, the display flags, the core's ticks, the warnings,
 * the errors and the verdict; the task buffer's sizes.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_CMD_DEC_ADDR_REPORT_BASE 0x114 to W5_CMD_DEC_REPORT_PARAM 0x11c,
 * W5_RET_DEC_SEQ_PARAM 0x120 to W5_RET_DEC_DECODING_SUCCESS 0x1dc,
 * W5_RET_VLC_BUF_SIZE 0x1b0, W5_RET_PARAM_BUF_SIZE 0x1b4,
 * W5_RET_QUERY_DEC_BS_RD_PTR 0x11c) and wave5.h
 * (REPORT_PARAM_ENDIANNESS_BIG_ENDIAN 0xf).
 */
#define REPORT_BASE		0x0114U
#define REPORT_SIZE		0x0118U
#define REPORT_ORDER		0x011cU
#define RESULT_SEQUENCE		0x0120U
#define RESULT_DEPTHS		0x0124U
#define RESULT_REQUIRED		0x0138U
#define RESULT_CHANGES		0x0144U
#define RESULT_SIZE		0x014cU
#define RESULT_CROP_TOP_BOTTOM	0x0150U
#define RESULT_CROP_LEFT_RIGHT	0x0154U
#define RESULT_TYPE		0x0160U
#define RESULT_ORDER		0x0164U
#define RESULT_DECODED		0x0170U
#define RESULT_DISPLAY		0x0174U
#define RESULT_FLAGS		0x017cU
#define RESULT_VLC_SIZE		0x01b0U
#define RESULT_PARAMETER_SIZE	0x01b4U
#define RESULT_HOST_TICK	0x01b8U
#define RESULT_END_TICK		0x01d0U
#define RESULT_WARNINGS		0x01d4U
#define RESULT_ERRORS		0x01d8U
#define RESULT_VERDICT		0x01dcU
#define POSITION_ANSWER		0x011cU
#define POSITION_SET		0x011cU
#define REPORT_IN_ORDER		0xfU

/*
 * The core's ticks are this many of its cycles.
 *
 * Value referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_vpu_build_up_dec_param: cycle_per_tick 256).
 */
#define CYCLES_PER_TICK		256U

/*
 * The frame buffer registration: the task buffer, the pictures' size, the
 * common layout (bus master write-back for linear buffers, pixel order,
 * interleaved chroma, the stride), how many buffers this command names
 * and whether it is the first and the last of the series, and per buffer
 * the planes (16 bytes apart: luma, chroma, then for linear buffers the
 * second chroma plane, for compressed ones the luma and chroma offset
 * tables) and the motion vectors (4 bytes apart).
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_SFB_OPTION 0x104, W5_COMMON_PIC_INFO 0x118, W5_PIC_SIZE 0x11c,
 * W5_SET_FB_NUM 0x120, W5_ADDR_LUMA_BASE0 0x134, W5_ADDR_CB_BASE0 0x138,
 * W5_ADDR_CR_BASE0 and W5_ADDR_FBC_Y_OFFSET0 0x13c, W5_ADDR_FBC_C_OFFSET0
 * 0x140, W5_ADDR_MV_COL0 0x1b4, W5_CMD_SET_FB_ADDR_TASK_BUF 0x1d4,
 * W5_CMD_SET_FB_TASK_BUF_SIZE 0x1d8) and wave5-hw.c
 * (wave5_vpu_dec_register_framebuffer: the common layout's bits 28, 23
 * and 16 and the series bits 3 and 4).
 */
#define BUFFERS_OPTION		0x0104U
#define BUFFERS_LAYOUT		0x0118U
#define BUFFERS_SIZE		0x011cU
#define BUFFERS_RANGE		0x0120U
#define BUFFERS_LUMA		0x0134U
#define BUFFERS_CHROMA		0x0138U
#define BUFFERS_SECOND		0x013cU
#define BUFFERS_CHROMA_TABLE	0x0140U
#define BUFFERS_MOTION		0x01b4U
#define BUFFERS_TASK_BASE	0x01d4U
#define BUFFERS_TASK_SIZE	0x01d8U
#define BUFFERS_STEP		16U
#define BUFFERS_MOTION_STEP	4U
#define BUFFERS_PER_COMMAND	8U
#define LAYOUT_WRITE_BACK	(1U << 28)
#define LAYOUT_PIXEL_ORDER	(1U << 23)
#define LAYOUT_INTERLEAVED	(1U << 16)
#define SERIES_FIRST		(1U << 3)
#define SERIES_LAST		(1U << 4)

/*
 * The update of one frame buffer pair: the option, both strides (the
 * linear one in the upper half), the index (as a range of one, the form
 * the registration names its buffers in), the linear planes, the motion
 * vectors, the compressed planes and their offset tables.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (SET_FB_OPTION 0x01 "update framebuffer", W5_CMD_SET_FB_STRIDE 0x118,
 * W5_CMD_SET_FB_INDEX 0x120, W5_ADDR_LUMA_BASE 0x134 to
 * W5_ADDR_FBC_C_OFFSET 0x150).
 */
#define UPDATE_OPTION		0x1U
#define UPDATE_STRIDES		0x0118U
#define UPDATE_INDEX		0x0120U
#define UPDATE_LUMA		0x0134U
#define UPDATE_CHROMA		0x0138U
#define UPDATE_SECOND		0x013cU
#define UPDATE_MOTION		0x0140U
#define UPDATE_PACKED_LUMA	0x0144U
#define UPDATE_PACKED_CHROMA	0x0148U
#define UPDATE_LUMA_TABLE	0x014cU
#define UPDATE_CHROMA_TABLE	0x0150U

/*
 * The display flag query's arguments.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-regdefine.h
 * (W5_CMD_DEC_SET_DISP_IDC 0x118, W5_CMD_DEC_CLR_DISP_IDC 0x11c).
 */
#define DISPLAY_SET		0x0118U
#define DISPLAY_CLEAR		0x011cU

/*
 * The task buffer is made one and a half times the size the vendor's
 * driver gives it (two VLC buffers and a parameter buffer per queued
 * command), in thousandths.  With the vendor's size the Wave511's
 * firmware stopped answering a picture command once about 2 MB of a 1080p
 * stream had been decoded, whatever the ring's size and the buffer's
 * alignment; with a quarter more every stream ran to its end (as seen on
 * the VisionFive 2), and half more leaves a margin.
 */
#define TASK_PERMILLE		1500U

/* How many times a destruction the firmware is not ready for is retried. */
#define DESTROY_TRIES		10U

static int take_compressed(struct drv_wave5_stream *stream, unsigned count);
static void give_compressed(struct drv_wave5_stream *stream);
static int send_buffers(struct drv_wave5_stream *stream, const struct drv_wave5_picture *linear, unsigned count, int compressed);
static uint32_t align_up(uint32_t value, uint32_t alignment);
static void read_sequence(struct drv_wave5_stream *stream, struct drv_wave5_sequence *sequence);

/*
 * Opens an instance that decodes one codec.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_vpu_build_up_dec_param: the arguments and the creation command).
 */
int
drv_wave5_open(
	struct drv_wave5 *core,
	unsigned codec,
	size_t ring_size,
	struct drv_wave5_stream **stream)
{
	struct drv_wave5_stream *opened;
	unsigned id;
	int error;

	/* Refuses a codec the core does not decode and a ring of the wrong size. */
	if (codec != DRV_WAVE5_H264 && codec != DRV_WAVE5_H265)
		return EINVAL;
	if (ring_size == 0 || ring_size % 1024U != 0 || ring_size > 0x10000000U)
		return EINVAL;

	/* Allocates the instance's state. */
	opened = kern_calloc(1, sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;
	opened->core = core;
	opened->codec = codec;
	opened->ring_size = ring_size;
	opened->standard = STANDARD_H264;
	if (codec == DRV_WAVE5_H265)
		opened->standard = STANDARD_H265;

	/* Takes the work area and the ring. */
	error = drv_wave5_core_take(core, WORK_SIZE, &opened->work);
	if (error != 0) {
		kern_free(opened);
		return error;
	}

	error = drv_wave5_core_take(core, ring_size, &opened->ring);
	if (error != 0) {
		drv_wave5_core_give(&opened->work);
		kern_free(opened);
		return error;
	}

	/* Takes a free instance number and creates the instance. */
	mutex_lock(&core->command_lock);

	error = ENOSPC;
	for (id = 0; id < VPU_MAX_INSTANCES; id++) {
		if ((core->instances & (1U << id)) == 0)
			break;
	}
	if (id < VPU_MAX_INSTANCES) {
		opened->id = id;
		drv_wave5_core_write(core, CREATE_CODEC_CORE, 1);
		drv_wave5_core_write(core, CREATE_WORK_BASE, drv_wave5_core_address(&opened->work));
		drv_wave5_core_write(core, CREATE_WORK_SIZE, WORK_SIZE);
		drv_wave5_core_write(core, CREATE_SRAM_BASE, core->place.sram_address);
		drv_wave5_core_write(core, CREATE_SRAM_SIZE, core->place.sram_size);
		drv_wave5_core_write(core, CREATE_RING_BASE, drv_wave5_core_address(&opened->ring));
		drv_wave5_core_write(core, CREATE_RING_SIZE, (uint32_t)ring_size);
		drv_wave5_core_write(core, CREATE_RING_ORDER, RING_BYTES_IN_ORDER);
		drv_wave5_core_write(core, CREATE_EXTENDED, 0);
		drv_wave5_core_write(core, CREATE_QUEUE_DEPTH, QUEUE_DEPTH - 1U);
		opened->generation = core->generation;
		error = drv_wave5_core_command(core, opened, VPU_RUN_CREATE, 1, NULL);
		if (error == 0)
			core->instances |= 1U << id;
	}

	mutex_unlock(&core->command_lock);

	/* Gives the memory back when no instance was created. */
	if (error != 0) {
		drv_wave5_core_give(&opened->ring);
		drv_wave5_core_give(&opened->work);
		kern_free(opened);
		return error;
	}

	/* Succeeded: the instance reads from the ring's start. */
	opened->read = drv_wave5_core_address(&opened->ring);
	opened->write = opened->read;
	*stream = opened;
	return 0;
}

/*
 * Destroys an instance and frees its memory.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.c
 * (wave5_vpu_dec_close: the destruction retried while the firmware says
 * the instance still runs).
 */
int
drv_wave5_close(
	struct drv_wave5_stream *stream)
{
	struct drv_wave5 *core;
	unsigned tries;
	uint32_t reason;
	int lost;
	int error;

	/* An instance a recovery lost is no longer the firmware's: its memory goes at once. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	lost = stream->generation != core->generation;

	mutex_unlock(&core->command_lock);
	if (lost) {
		drv_wave5_stream_free(stream);
		return 0;
	}

	/* Destroys the instance, retrying while the firmware still runs it. */
	error = EIO;
	for (tries = 0; tries < DESTROY_TRIES; tries++) {
		mutex_lock(&core->command_lock);

		reason = 0;
		error = drv_wave5_core_command(core, stream, VPU_RUN_DESTROY, 1, &reason);
		if (error == 0)
			core->instances &= ~(1U << stream->id);
		if (error == ESTALE)
			lost = 1;

		mutex_unlock(&core->command_lock);

		/* Stops unless the firmware asked for another try. */
		if (error != EBUSY)
			break;
		kern_usleep_range(1000, 2000);
	}

	/* A recovery between the tries lost the instance: its memory goes. */
	if (lost) {
		drv_wave5_stream_free(stream);
		return 0;
	}

	/* Keeps the memory of an instance the firmware may still use until the core is detached. */
	if (error != 0) {
		mutex_lock(&core->command_lock);

		stream->next_orphan = core->orphans;
		core->orphans = stream;

		mutex_unlock(&core->command_lock);
		return EIO;
	}

	/* Succeeded: the instance is gone and its memory free. */
	drv_wave5_stream_free(stream);
	return 0;
}

/*
 * Reports whether a recovery lost the instance.
 */
int
drv_wave5_lost(
	const struct drv_wave5_stream *stream)
{
	struct drv_wave5 *core;
	int lost;

	/* The core's generation moves on in a recovery, under the command lock. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	lost = stream->generation != core->generation;

	mutex_unlock(&core->command_lock);

	/* Succeeded: whether the firmware still knows the instance. */
	return lost;
}

/*
 * Has the firmware drop the instance's pictures and unread bytes.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.c
 * (wave5_vpu_flush_instance: the flush command repeated while the firmware
 * still runs the instance) and wave5-vpu-dec.c (the read position asked
 * after the flush becomes both ring positions).
 */
int
drv_wave5_flush(
	struct drv_wave5_stream *stream)
{
	struct drv_wave5 *core;
	uint32_t position;
	uint32_t start;
	unsigned tries;
	int error;

	/* Flushes the instance, retrying while the firmware still runs it. */
	core = stream->core;
	error = EIO;
	for (tries = 0; tries < DESTROY_TRIES; tries++) {
		mutex_lock(&core->command_lock);

		error = drv_wave5_core_command(core, stream, VPU_RUN_FLUSH, 1, NULL);

		mutex_unlock(&core->command_lock);

		/* Stops unless the firmware asked for another try. */
		if (error != EBUSY)
			break;
		kern_usleep_range(1000, 2000);
	}
	if (error != 0)
		return error;

	/* The firmware reads on from where it stopped: the next bytes go there. */
	mutex_lock(&core->command_lock);

	core->flushes++;
	error = drv_wave5_core_query(core, stream, VPU_ASK_READ_POSITION);
	position = 0;
	if (error == 0)
		position = drv_wave5_core_read(core, POSITION_ANSWER);

	mutex_unlock(&core->command_lock);
	if (error != 0)
		return error;

	/* A position outside the ring leaves the positions as they were. */
	start = drv_wave5_core_address(&stream->ring);
	if (position >= start && position < start + (uint32_t)stream->ring_size) {
		stream->read = position;
		stream->write = position;
	}

	/* Succeeded: the instance waits for a picture to start at. */
	return 0;
}

/*
 * Puts one picture's bytes at the start of the ring.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_dec_set_rd_ptr: the set read position query).
 */
int
drv_wave5_load(
	struct drv_wave5_stream *stream,
	const uint8_t *bytes,
	size_t length,
	unsigned rewind)
{
	struct drv_wave5 *core;
	uint32_t start;
	int error;

	/* Refuses bytes the ring cannot hold. */
	if (length == 0 || length > stream->ring_size)
		return EFBIG;

	/* Copies the bytes to the ring's start, before any command names them. */
	kern_memcpy(stream->ring.view, bytes, length);
	kern_io_write_barrier();

	/* The next command reads from the start to the end of the bytes. */
	start = drv_wave5_core_address(&stream->ring);
	stream->read = start;
	stream->write = start + (uint32_t)length;

	/* The register rewind leaves it to the read position the next command carries. */
	if (rewind == DRV_WAVE5_REWIND_REGISTER)
		return 0;

	/* Tells the firmware to read from the ring's start again. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, POSITION_SET, start);
	error = drv_wave5_core_query(core, stream, VPU_ASK_SET_POSITION);

	mutex_unlock(&core->command_lock);

	/* Reports a refused rewind. */
	if (error != 0)
		return error;

	/* Succeeded: the next command reads the new bytes. */
	return 0;
}

/*
 * Puts one picture's bytes right after the bytes the last command read.
 *
 * The core reads its ring as a ring: bytes that run past its end go on
 * at its start.  One picture takes at most the ring less 1 KiB, so its
 * end never meets the start of the bytes the core reads.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpu-dec.c
 * (write_to_ringbuffer: the part past the ring's end written at its start).
 */
int
drv_wave5_append(
	struct drv_wave5_stream *stream,
	const uint8_t *bytes,
	size_t length)
{
	struct drv_wave5_piece piece;
	int error;

	/* The bytes as one piece. */
	piece.bytes = bytes;
	piece.length = length;
	error = drv_wave5_append_pieces(stream, &piece, 1);
	if (error != 0)
		return error;

	/* Succeeded: the next command reads the new bytes. */
	return 0;
}

/*
 * Puts one picture's bytes, given in pieces, right after the bytes the
 * last command read, wrapping at the ring's end.
 */
int
drv_wave5_append_pieces(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_piece *pieces,
	unsigned count)
{
	uint32_t start;
	size_t length;
	size_t offset;
	size_t at;
	size_t first;
	unsigned index;
	int wrapped;

	/* The picture's bytes, all pieces together. */
	length = 0;
	for (index = 0; index < count; index++) {
		if (pieces[index].length > stream->ring_size)
			return EFBIG;
		length += pieces[index].length;
	}

	/* Refuses bytes the ring cannot hold beside the core's reading. */
	start = drv_wave5_core_address(&stream->ring);
	if (length == 0 || length > stream->ring_size - 1024U)
		return EFBIG;

	/* Where the last bytes ended, at the ring's start once they reached its end. */
	offset = stream->write - start;
	if (offset >= stream->ring_size)
		offset = 0;

	/* Copies each piece up to the ring's end, then the rest from its start, before any command names them. */
	at = offset;
	wrapped = 0;
	for (index = 0; index < count; index++) {
		first = pieces[index].length;
		if (first > stream->ring_size - at)
			first = stream->ring_size - at;
		kern_memcpy((uint8_t *)stream->ring.view + at, pieces[index].bytes, first);
		at += first;
		if (at == stream->ring_size)
			at = 0;
		if (first < pieces[index].length) {
			kern_memcpy(stream->ring.view, pieces[index].bytes + first, pieces[index].length - first);
			at = pieces[index].length - first;
			wrapped = 1;
		}
	}
	kern_io_write_barrier();

	/* The next command reads from the last end to the new one, which wraps to the start past the ring's end. */
	stream->read = start + (uint32_t)offset;
	stream->write = start + (uint32_t)at;
	if (wrapped)
		stream->wraps++;

	/* Succeeded: the next command reads the new bytes. */
	return 0;
}

/*
 * Tells the core where the ring's bytes now end.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.c
 * (wave5_vpu_dec_update_bitstream_buffer, after write_to_ringbuffer) and
 * wave5-hw.c (wave5_vpu_dec_set_bitstream_flag: the options, the write
 * position, then the bitstream update command, W5_UPDATE_BS 0x8000 in
 * wave5-regdefine.h).
 */
int
drv_wave5_update_stream(
	struct drv_wave5_stream *stream)
{
	struct drv_wave5 *core;
	uint32_t reason;
	int error;

	/* The explicit end at the write position, then the command. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, STREAM_OPTIONS, STREAM_EXPLICIT_END);
	drv_wave5_core_write(core, STREAM_WRITE, stream->write);
	reason = 0;
	error = drv_wave5_core_command(core, stream, VPU_RUN_UPDATE_STREAM, 1, &reason);

	mutex_unlock(&core->command_lock);

	/* Reports an update the firmware refused. */
	if (error != 0)
		return error;

	/* Succeeded: the core knows the bytes' end. */
	return 0;
}

/*
 * Takes back the bytes the last append put in the ring, which the core
 * did not read.
 */
void
drv_wave5_unread(
	struct drv_wave5_stream *stream)
{
	/* The next bytes go where the last ones did. */
	stream->write = stream->read;
}

/*
 * Reports where the last append put the picture's bytes.
 */
void
drv_wave5_positions(
	const struct drv_wave5_stream *stream,
	uint32_t *read,
	uint32_t *write)
{
	/* The bytes the next command reads. */
	*read = stream->read;
	*write = stream->write;
}

/*
 * Reports how many pictures' bytes went past the ring's end.
 */
uint32_t
drv_wave5_wraps(
	const struct drv_wave5_stream *stream)
{
	/* Succeeded: the count. */
	return stream->wraps;
}

/*
 * Reports where the ring's bytes start.
 */
void
drv_wave5_ring(
	struct drv_wave5_stream *stream,
	uint8_t **view,
	uint64_t *address)
{
	/* The ring's start, as the host and the core see it. */
	*view = stream->ring.view;
	*address = stream->ring.run.paddr;
}

/*
 * Sets the instance's sequence up from the loaded picture.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_vpu_dec_init_seq, wave5_vpu_dec_get_seq_info,
 * wave5_get_dec_seq_result: the read and write positions, the explicit
 * end, the sequence command; after its interrupt the result query and its
 * answers).
 */
int
drv_wave5_init_sequence(
	struct drv_wave5_stream *stream,
	struct drv_wave5_sequence *sequence)
{
	struct drv_wave5 *core;
	uint32_t reason;
	int error;

	/* Issues the sequence command on the loaded bytes. */
	core = stream->core;
	kern_memset(sequence, 0, sizeof(*sequence));
	mutex_lock(&core->command_lock);

	drv_wave5_core_forget(core, stream);
	drv_wave5_core_write(core, STREAM_READ, stream->read);
	drv_wave5_core_write(core, STREAM_WRITE, stream->write);
	drv_wave5_core_write(core, STREAM_OPTIONS, STREAM_EXPLICIT_END);
	drv_wave5_core_write(core, VPU_OPTION, SEQUENCE_NORMAL);
	drv_wave5_core_write(core, PICTURE_USER_DATA, 0);
	reason = 0;
	error = drv_wave5_core_command(core, stream, VPU_RUN_SEQUENCE, 1, &reason);

	mutex_unlock(&core->command_lock);

	/* Reports a sequence command the firmware refused. */
	if (error != 0) {
		sequence->error = reason;
		return error;
	}

	/* Waits for the sequence's completion. */
	error = drv_wave5_core_wait(core, stream, 1, VPU_DECODE_MS);
	if (error != 0)
		return error;

	/* Asks for the result and takes the sequence's description. */
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, REPORT_BASE, 0);
	drv_wave5_core_write(core, REPORT_SIZE, 0);
	drv_wave5_core_write(core, REPORT_ORDER, REPORT_IN_ORDER);
	error = drv_wave5_core_query(core, stream, VPU_ASK_RESULT);
	if (error == 0) {
		read_sequence(stream, sequence);
		error = drv_wave5_core_query(core, stream, VPU_ASK_READ_POSITION);
		if (error == 0)
			sequence->read_position = drv_wave5_core_read(core, POSITION_ANSWER);
	}

	mutex_unlock(&core->command_lock);

	/* Reports a refused query. */
	if (error != 0)
		return error;

	/* Reports a sequence the firmware could not set up. */
	if (sequence->success != 1U)
		return EIO;

	/* Succeeded: the sequence is set up. */
	stream->sequence = *sequence;
	stream->sequenced = 1;
	return 0;
}

/*
 * Registers frame buffer pairs.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.c
 * (wave5_vpu_dec_register_frame_buffer_ex: the linear buffers before the
 * compressed ones, the stride a multiple of 8 not below the width) and
 * wave5-vpu-dec.c (wave5_prepare_fb: the compressed pictures' size).
 */
int
drv_wave5_register(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_picture *linear,
	unsigned count,
	uint32_t stride)
{
	int error;

	/* Refuses a registration before the sequence or with too many pairs. */
	if (!stream->sequenced)
		return EINVAL;
	if (count == 0 || count > DRV_WAVE5_MAX_BUFFERS)
		return EINVAL;

	/* Refuses a stride the core cannot write. */
	if (stride % 8U != 0 || stride < stream->sequence.width)
		return EINVAL;

	/* Refuses a second registration: a sequence registers once. */
	if (stream->count != 0)
		return EBUSY;

	/* Takes the compressed side of every pair and the task buffer. */
	stream->linear_stride = stride;
	error = take_compressed(stream, count);
	if (error != 0)
		return error;

	/* Registers the linear buffers, then the compressed ones. */
	error = send_buffers(stream, linear, count, 0);
	if (error == 0)
		error = send_buffers(stream, NULL, count, 1);
	if (error != 0) {
		give_compressed(stream);
		return error;
	}

	/* Succeeded: the pairs are registered. */
	stream->count = count;
	return 0;
}

/*
 * Points one registered pair's linear buffer at another picture.
 */
int
drv_wave5_replace(
	struct drv_wave5_stream *stream,
	unsigned index,
	const struct drv_wave5_picture *linear)
{
	struct wave5_compressed *pair;
	struct drv_wave5 *core;
	uint32_t pixels;
	uint32_t reason;
	uint32_t second;
	int error;

	/* Refuses a pair that is not registered. */
	if (index >= stream->count)
		return EINVAL;

	/* Names the pair, its new linear planes and its unchanged compressed side, then updates it. */
	core = stream->core;
	pair = &stream->compressed[index];
	pixels = drv_wave5_core_address(&pair->pixels);
	second = (uint32_t)linear->chroma + stream->linear_stride * stream->sequence.height / 4U;
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, BUFFERS_OPTION, UPDATE_OPTION);
	drv_wave5_core_write(core, UPDATE_STRIDES, (stream->linear_stride << 16) | stream->compressed_stride);
	drv_wave5_core_write(core, UPDATE_INDEX, (index << 8) | index);
	drv_wave5_core_write(core, UPDATE_LUMA, (uint32_t)linear->luma);
	drv_wave5_core_write(core, UPDATE_CHROMA, (uint32_t)linear->chroma);
	drv_wave5_core_write(core, UPDATE_SECOND, second);
	drv_wave5_core_write(core, UPDATE_MOTION, drv_wave5_core_address(&pair->motion));
	drv_wave5_core_write(core, UPDATE_PACKED_LUMA, pixels);
	drv_wave5_core_write(core, UPDATE_PACKED_CHROMA, pixels + stream->compressed_luma);
	drv_wave5_core_write(core, UPDATE_LUMA_TABLE, drv_wave5_core_address(&pair->luma_table));
	drv_wave5_core_write(core, UPDATE_CHROMA_TABLE, drv_wave5_core_address(&pair->chroma_table));
	reason = 0;
	error = drv_wave5_core_command(core, stream, VPU_RUN_BUFFERS, 1, &reason);

	mutex_unlock(&core->command_lock);

	/* Reports an update the firmware refused. */
	if (error != 0)
		return error;

	/* Succeeded: the pair's linear buffer is the new picture. */
	return 0;
}

/*
 * Sets and clears frame buffers' display flags.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_dec_clr_disp_flag, wave5_dec_set_disp_flag).
 */
int
drv_wave5_display_flags(
	struct drv_wave5_stream *stream,
	uint32_t set,
	uint32_t clear,
	uint32_t *flags)
{
	struct drv_wave5 *core;
	int error;

	/* Names the flags to set and to clear, then asks the query. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, DISPLAY_SET, set);
	drv_wave5_core_write(core, DISPLAY_CLEAR, clear);
	error = drv_wave5_core_query(core, stream, VPU_ASK_DISPLAY);
	if (error == 0 && flags != NULL)
		*flags = drv_wave5_core_read(core, RESULT_FLAGS);

	mutex_unlock(&core->command_lock);

	/* Reports a refused query. */
	if (error != 0)
		return error;

	/* Succeeded: the flags are as asked. */
	return 0;
}

/*
 * Decodes the loaded picture and waits for its result.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c (wave5_vpu_decode,
 * wave5_vpu_dec_get_result: the positions, the explicit end, no secondary
 * memory, the sequence changes, the forced latency that gives a picture
 * out in decode order, the picture command; after its interrupt the
 * result query and its answers).
 */
int
drv_wave5_decode(
	struct drv_wave5_stream *stream,
	struct drv_wave5_result *result)
{
	struct drv_wave5 *core;
	uint64_t started;
	uint64_t ended;
	uint32_t changes;
	uint32_t reason;
	uint32_t host_tick;
	uint32_t end_tick;
	int waited;
	int error;

	/* Refuses a decode before frame buffers are registered. */
	kern_memset(result, 0, sizeof(*result));
	result->decoded = DRV_WAVE5_INDEX_NONE;
	result->display = DRV_WAVE5_INDEX_NONE;
	if (stream->count == 0)
		return EINVAL;

	/* The sequence changes the codec reports. */
	changes = CHANGES_H264;
	if (stream->codec == DRV_WAVE5_H265)
		changes = CHANGES_H265;

	/* Issues the picture command on the loaded bytes. */
	core = stream->core;
	started = clock_ticks();
	mutex_lock(&core->command_lock);

	drv_wave5_core_forget(core, stream);
	drv_wave5_core_write(core, STREAM_READ, stream->read);
	drv_wave5_core_write(core, STREAM_WRITE, stream->write);
	drv_wave5_core_write(core, STREAM_OPTIONS, STREAM_EXPLICIT_END);
	drv_wave5_core_write(core, PICTURE_SRAM_USE, 0);
	drv_wave5_core_write(core, PICTURE_USER_DATA, 0);
	drv_wave5_core_write(core, VPU_OPTION, PICTURE_NORMAL);
	drv_wave5_core_write(core, PICTURE_LAYERS, 0);
	drv_wave5_core_write(core, PICTURE_CHANGES, changes);
	drv_wave5_core_write(core, PICTURE_LATENCY, 1);
	reason = 0;
	error = drv_wave5_core_command(core, stream, VPU_RUN_PICTURE, 1, &reason);
	result->queue[0] = drv_wave5_core_read(core, VPU_QUEUE_STATUS);

	mutex_unlock(&core->command_lock);

	/* Reports a picture command the firmware refused. */
	if (error != 0) {
		result->error = reason;
		return error;
	}

	/*
	 * Waits for the picture's completion.  A wait that ends without it
	 * still asks for the result, which tells what the firmware did.
	 */
	waited = drv_wave5_core_wait(core, stream, 0, VPU_DECODE_MS);

	/* Asks for the result and takes it. */
	mutex_lock(&core->command_lock);

	drv_wave5_core_write(core, REPORT_BASE, 0);
	drv_wave5_core_write(core, REPORT_SIZE, 0);
	drv_wave5_core_write(core, REPORT_ORDER, REPORT_IN_ORDER);
	error = drv_wave5_core_query(core, stream, VPU_ASK_RESULT);
	if (error == 0) {
		result->success = drv_wave5_core_read(core, RESULT_VERDICT);
		result->error = drv_wave5_core_read(core, RESULT_ERRORS);
		result->warning = drv_wave5_core_read(core, RESULT_WARNINGS);
		result->type = drv_wave5_core_read(core, RESULT_TYPE);
		result->decoded = (int32_t)drv_wave5_core_read(core, RESULT_DECODED);
		result->display = (int32_t)drv_wave5_core_read(core, RESULT_DISPLAY);
		result->order = (int32_t)drv_wave5_core_read(core, RESULT_ORDER);
		result->notification = drv_wave5_core_read(core, RESULT_CHANGES);
		result->display_flags = drv_wave5_core_read(core, RESULT_FLAGS);
		result->width = drv_wave5_core_read(core, RESULT_SIZE) >> 16;
		result->height = drv_wave5_core_read(core, RESULT_SIZE) & 0xffffU;
		host_tick = drv_wave5_core_read(core, RESULT_HOST_TICK);
		end_tick = drv_wave5_core_read(core, RESULT_END_TICK);
		result->ticks = (uint64_t)(end_tick - host_tick) * CYCLES_PER_TICK;
		result->queue[1] = drv_wave5_core_read(core, VPU_QUEUE_STATUS);
		error = drv_wave5_core_query(core, stream, VPU_ASK_READ_POSITION);
		if (error == 0)
			result->read_position = drv_wave5_core_read(core, POSITION_ANSWER);
	}

	/* A picture that completed ends a run of recoveries. */
	if (waited == 0 && error == 0)
		core->strikes = 0;

	mutex_unlock(&core->command_lock);

	/* The host's time from the command to the result. */
	ended = clock_ticks();
	result->wall = kern_ticks_to_us(ended - started);

	/* Reports a picture that did not complete, then a refused query. */
	if (waited != 0)
		return waited;
	if (error != 0)
		return error;

	/* Succeeded: the result is the firmware's. */
	return 0;
}

/*
 * Asks the core where it is reading in the ring now.
 */
int
drv_wave5_read_position(
	struct drv_wave5_stream *stream,
	uint32_t *position)
{
	struct drv_wave5 *core;
	int error;

	/* Asks the read position query. */
	core = stream->core;
	mutex_lock(&core->command_lock);

	error = drv_wave5_core_query(core, stream, VPU_ASK_READ_POSITION);
	if (error == 0)
		*position = drv_wave5_core_read(core, POSITION_ANSWER);

	mutex_unlock(&core->command_lock);

	/* Reports a refused query. */
	if (error != 0)
		return error;

	/* Succeeded: the position is the firmware's. */
	return 0;
}

/*
 * Frees an instance's memory.
 */
void
drv_wave5_stream_free(
	struct drv_wave5_stream *stream)
{
	/* Frees the frame buffers' compressed side, the task buffer, the ring and the work area. */
	give_compressed(stream);
	drv_wave5_core_give(&stream->ring);
	drv_wave5_core_give(&stream->work);
	kern_free(stream);
}

/*
 * Takes the compressed side of count pairs and the task buffer for the
 * sequence's size.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpuapi.h
 * (WAVE5_DEC_AVC_BUF_SIZE, WAVE5_DEC_HEVC_BUF_SIZE,
 * WAVE5_FBC_LUMA_TABLE_SIZE, WAVE5_FBC_CHROMA_TABLE_SIZE, BUFFER_MARGIN
 * 4096), wave5-vpu-dec.c (wave5_prepare_fb: the picture's stride the
 * width, its rows the height aligned to 32, its chroma half the stride
 * aligned to 16), wave5-hw.c (wave5_vpu_dec_register_framebuffer: each
 * table and the motion vectors aligned to 16, then to the margin, plus a
 * margin; the task buffer two VLC buffers and a parameter buffer per
 * queued command) and wave5-vpuconfig.h (VLC_BUF_NUM 2).
 */
static int
take_compressed(
	struct drv_wave5_stream *stream,
	unsigned count)
{
	struct wave5_compressed *pair;
	uint32_t width;
	uint32_t height;
	uint32_t rows;
	uint32_t chroma;
	uint32_t motion;
	uint32_t luma_table;
	uint32_t chroma_table;
	uint32_t share;
	uint32_t alignment;
	uint32_t task;
	unsigned index;
	int error;

	/* Works out the sizes from the sequence's picture. */
	width = stream->sequence.width;
	height = stream->sequence.height;
	rows = align_up(height, 32U);
	stream->compressed_stride = width;
	stream->compressed_luma = width * rows;
	chroma = align_up(width / 2U, 16U) * rows;
	if (stream->codec == DRV_WAVE5_H264) {
		motion = ((align_up(width, 256U) / 16U) * (align_up(height, 16U) / 16U) + 16U) * 80U;
	} else {
		motion = ((width + 63U) / 64U) * ((height + 63U) / 64U) * 256U + 64U;
	}
	motion = align_up(align_up(motion, 16U), 4096U) + 4096U;
	luma_table = align_up(height, 64U) * align_up(width, 256U) / 32U;
	luma_table = align_up(align_up(luma_table, 16U), 4096U) + 4096U;
	chroma_table = align_up(height, 64U) * align_up(width / 2U, 256U) / 32U;
	chroma_table = align_up(align_up(chroma_table, 16U), 4096U) + 4096U;
	mutex_lock(&stream->core->command_lock);

	share = stream->core->task_permille;
	alignment = stream->core->task_alignment;

	mutex_unlock(&stream->core->command_lock);
	task = stream->vlc_size * 2U + stream->parameter_size * QUEUE_DEPTH;
	if (share == 0)
		share = TASK_PERMILLE;
	task = (uint32_t)((uint64_t)task * share / 1000U);

	/* Takes the task buffer the firmware works through the queued commands in. */
	error = drv_wave5_core_take_aligned(stream->core, task, alignment, &stream->task);
	if (error != 0)
		return error;

	/* Takes each pair's picture, tables and motion vectors. */
	for (index = 0; index < count; index++) {
		pair = &stream->compressed[index];
		error = drv_wave5_core_take(stream->core, stream->compressed_luma + chroma, &pair->pixels);
		if (error == 0)
			error = drv_wave5_core_take(stream->core, luma_table, &pair->luma_table);
		if (error == 0)
			error = drv_wave5_core_take(stream->core, chroma_table, &pair->chroma_table);
		if (error == 0)
			error = drv_wave5_core_take(stream->core, motion, &pair->motion);
		if (error != 0) {
			give_compressed(stream);
			return error;
		}
	}

	/* The sizes, for the log. */
	kern_logf("wave5: instance %u: vlc %u parameter %u task %u at %08x, luma %u chroma %u motion %u tables %u %u\n",
		  stream->id,
		  (unsigned)stream->vlc_size,
		  (unsigned)stream->parameter_size,
		  (unsigned)task,
		  (unsigned)drv_wave5_core_address(&stream->task),
		  (unsigned)stream->compressed_luma,
		  (unsigned)chroma,
		  (unsigned)motion,
		  (unsigned)luma_table,
		  (unsigned)chroma_table);

	/* Succeeded: every pair has its compressed side. */
	return 0;
}

/* Gives the compressed side of every pair and the task buffer back. */
static void
give_compressed(
	struct drv_wave5_stream *stream)
{
	struct wave5_compressed *pair;
	unsigned index;

	/* Each pair's memory; one never taken is empty and left alone. */
	for (index = 0; index < DRV_WAVE5_MAX_BUFFERS; index++) {
		pair = &stream->compressed[index];
		drv_wave5_core_give(&pair->motion);
		drv_wave5_core_give(&pair->chroma_table);
		drv_wave5_core_give(&pair->luma_table);
		drv_wave5_core_give(&pair->pixels);
	}

	/* The task buffer. */
	drv_wave5_core_give(&stream->task);
}

/*
 * Registers the linear or the compressed side of count pairs, eight per
 * command, and checks the firmware took the series.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_vpu_dec_register_framebuffer).
 */
static int
send_buffers(
	struct drv_wave5_stream *stream,
	const struct drv_wave5_picture *linear,
	unsigned count,
	int compressed)
{
	struct wave5_compressed *pair;
	struct drv_wave5 *core;
	uint32_t layout;
	uint32_t series;
	uint32_t pixels;
	uint32_t taken;
	unsigned first;
	unsigned last;
	unsigned slot;
	unsigned index;
	int error;

	/* The layout: compressed pictures in planes, linear ones written back with interleaved chroma. */
	core = stream->core;
	if (compressed) {
		layout = LAYOUT_PIXEL_ORDER | stream->compressed_stride;
	} else {
		layout = LAYOUT_WRITE_BACK | LAYOUT_PIXEL_ORDER | LAYOUT_INTERLEAVED | stream->linear_stride;
	}

	/* Names the task buffer, the size and the layout. */
	error = 0;
	mutex_lock(&core->command_lock);

	if (compressed) {
		drv_wave5_core_write(core, BUFFERS_TASK_BASE, drv_wave5_core_address(&stream->task));
		drv_wave5_core_write(core, BUFFERS_TASK_SIZE, (uint32_t)stream->task.run.size);
	}
	drv_wave5_core_write(core, BUFFERS_SIZE, (stream->sequence.width << 16) | stream->sequence.height);
	drv_wave5_core_write(core, BUFFERS_LAYOUT, layout);

	/* Sends the buffers eight at a time. */
	for (first = 0; first < count && error == 0; first += BUFFERS_PER_COMMAND) {
		/* Marks the series' first and last command and names this command's buffers. */
		last = first + BUFFERS_PER_COMMAND - 1U;
		if (last >= count)
			last = count - 1U;
		series = 0;
		if (first == 0)
			series |= SERIES_FIRST;
		if (last == count - 1U)
			series |= SERIES_LAST;
		drv_wave5_core_write(core, BUFFERS_OPTION, series);
		drv_wave5_core_write(core, BUFFERS_RANGE, (first << 8) | last);

		/* Names each buffer's planes. */
		for (index = first; index <= last; index++) {
			slot = index - first;
			if (compressed) {
				pair = &stream->compressed[index];
				pixels = drv_wave5_core_address(&pair->pixels);
				drv_wave5_core_write(core, BUFFERS_LUMA + slot * BUFFERS_STEP, pixels);
				drv_wave5_core_write(core, BUFFERS_CHROMA + slot * BUFFERS_STEP, pixels + stream->compressed_luma);
				drv_wave5_core_write(core, BUFFERS_SECOND + slot * BUFFERS_STEP, drv_wave5_core_address(&pair->luma_table));
				drv_wave5_core_write(core, BUFFERS_CHROMA_TABLE + slot * BUFFERS_STEP, drv_wave5_core_address(&pair->chroma_table));
				drv_wave5_core_write(core, BUFFERS_MOTION + slot * BUFFERS_MOTION_STEP, drv_wave5_core_address(&pair->motion));
			} else {
				drv_wave5_core_write(core, BUFFERS_LUMA + slot * BUFFERS_STEP, (uint32_t)linear[index].luma);
				drv_wave5_core_write(core, BUFFERS_CHROMA + slot * BUFFERS_STEP, (uint32_t)linear[index].chroma);
				drv_wave5_core_write(core,
						     BUFFERS_SECOND + slot * BUFFERS_STEP,
						     (uint32_t)linear[index].chroma + stream->linear_stride * stream->sequence.height / 4U);
				drv_wave5_core_write(core, BUFFERS_CHROMA_TABLE + slot * BUFFERS_STEP, 0);
				drv_wave5_core_write(core, BUFFERS_MOTION + slot * BUFFERS_MOTION_STEP, 0);
			}
		}

		/* Sends this command's buffers; the series is checked at its end. */
		error = drv_wave5_core_command(core, stream, VPU_RUN_BUFFERS, 0, NULL);
	}

	/* Checks the firmware took the series. */
	if (error == 0) {
		taken = drv_wave5_core_read(core, VPU_SUCCESS);
		if (taken == 0)
			error = EIO;
	}

	mutex_unlock(&core->command_lock);

	/* Reports a series the firmware did not take. */
	if (error != 0)
		return error;

	/* Succeeded: the side is registered. */
	return 0;
}

/* Rounds a value up to a power-of-two alignment. */
static uint32_t
align_up(
	uint32_t value,
	uint32_t alignment)
{
	/* The next multiple of the alignment. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}

/*
 * Takes a sequence's description from the result query's answers.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-hw.c
 * (wave5_get_dec_seq_result: the size's width in the upper half, the
 * crops' first edge in the upper half, the bit depths in bits 0 to 3 and
 * 4 to 7, the profile in bits 24 to 30).
 */
static void
read_sequence(
	struct drv_wave5_stream *stream,
	struct drv_wave5_sequence *sequence)
{
	struct drv_wave5 *core;
	uint32_t value;

	/* The verdict and its reason. */
	core = stream->core;
	sequence->success = drv_wave5_core_read(core, RESULT_VERDICT);
	sequence->error = drv_wave5_core_read(core, RESULT_ERRORS);

	/*
	 * The coded size and the frame buffers needed.  An H.265 picture's
	 * width is a multiple of 8 only; its frame buffers are as wide as the
	 * next multiple of 16 (the stride and size the core is given; H.264's
	 * width is a multiple of 16 already).  StarFive's driver rounds the
	 * frame buffers' width up the same way (wave5-vpu-dec.c,
	 * wave5_update_pix_fmt, to 32; 16 is what the H.264 pictures here
	 * already show the core takes).
	 */
	value = drv_wave5_core_read(core, RESULT_SIZE);
	sequence->width = value >> 16;
	if (stream->codec == DRV_WAVE5_H265)
		sequence->width = align_up(sequence->width, 16U);
	sequence->height = value & 0xffffU;
	sequence->required = drv_wave5_core_read(core, RESULT_REQUIRED);

	/* The crop. */
	value = drv_wave5_core_read(core, RESULT_CROP_LEFT_RIGHT);
	sequence->crop_left = value >> 16;
	sequence->crop_right = value & 0xffffU;
	value = drv_wave5_core_read(core, RESULT_CROP_TOP_BOTTOM);
	sequence->crop_top = value >> 16;
	sequence->crop_bottom = value & 0xffffU;

	/* The bit depths and the profile. */
	value = drv_wave5_core_read(core, RESULT_DEPTHS);
	sequence->luma_depth = value & 0xfU;
	sequence->chroma_depth = (value >> 4) & 0xfU;
	value = drv_wave5_core_read(core, RESULT_SEQUENCE);
	sequence->profile = (value >> 24) & 0x7fU;

	/* The task buffer's parts, which the registration sizes. */
	stream->vlc_size = drv_wave5_core_read(core, RESULT_VLC_SIZE);
	stream->parameter_size = drv_wave5_core_read(core, RESULT_PARAMETER_SIZE);
}
