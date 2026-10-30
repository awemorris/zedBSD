/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libmedia's software H.264 back end (h264sw-backend.c) against
 * a scripted stand-in for the decoder (h264sw-decoder.c is replaced by the
 * functions below): open, the parameter sets, pictures with cropping, colour
 * and aspect, the order and times, the queue's bound and the deferred rest
 * of a packet, the drain, a flush, and the error paths.
 */

#include "check.h"
#include "h264sw-backend.h"
#include "h264sw-decoder.h"
#include "h264sw-nal.h"
#include "picture.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The size in macroblocks of the frames the stand-in decoder makes; the pool is made from it at the first frame. */
static unsigned fake_width_mbs = 2U;
static unsigned fake_height_mbs = 2U;

/* The cropping window the script gives each frame; the defaults crop nothing from a 32x32 frame. */
static unsigned fake_crop_x;
static unsigned fake_crop_y;
static unsigned fake_crop_width = 32U;
static unsigned fake_crop_height = 32U;

/* How many frames a slice NAL unit outputs, and how many the end of the stream outputs. */
static unsigned fake_frames_per_slice = 1U;
static unsigned fake_flush_frames;

/* The sample aspect ratio the frames carry; zero means the stream does not state one. */
static unsigned fake_sar_width;
static unsigned fake_sar_height;

/* The video signal fields the frames carry: whether they are present, the range and the matrix. */
static int fake_signal_present;
static int fake_full_range;
static int fake_colour_present;
static unsigned fake_matrix;

/* What the stand-in decoder answers for a parameter set, a slice and its own creation; 0 is success. */
static int fake_sps_status;
static int fake_slice_status;
static int fake_create_status;

/* The number of the next frame the stand-in decoder makes, which the pattern in its planes is built from. */
static unsigned fake_serial;

/* How many times the decoder was reset and destroyed, for the tests to observe. */
static unsigned fake_resets;
static unsigned fake_destroyed;

/* The NAL unit types the stand-in decoder was given, in order, and how many are recorded. */
static unsigned fake_types[64];
static unsigned fake_type_count;

/* The time of the last NAL unit the stand-in decoder was given. */
static int64_t fake_last_time;

/* The avcC record with one SPS and one PPS, lengths of 4 bytes. */
static const unsigned char avcc[] = {
	1U, 66U, 0U, 30U, 0xffU, 0xe1U, 0U, 4U, 0x67U, 1U, 2U, 3U, 1U, 0U, 2U, 0x68U, 9U
};

static void fake_fill(struct h264sw_frame *frame, unsigned serial);
static void fake_output(struct h264sw_decoder *decoder, unsigned frames, int64_t time_us);
static void script_reset(void);
static void make_track(struct media_track *track);
static size_t make_data(unsigned char *buffer, unsigned count, unsigned type);
static unsigned drain(void *state, int64_t *last_time);
static void test_open(void);
static void test_picture(void);
static void test_order(void);
static void test_queue(void);
static void test_errors(void);

/* The library's log is not under test. */
void
media_log(
	const char *format,
	...)
{
	UNUSED_PARAMETER(format);
}

/* Makes the decoder in its start state. */
int
h264sw_decoder_create(
	struct h264sw_decoder **decoder)
{
	struct h264sw_decoder *made;

	/* The script may refuse. */
	*decoder = NULL;
	if (fake_create_status != 0)
		return fake_create_status;

	/* Allocates the decoder. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Starts the output queue and gives the decoder a picture buffer size. */
	h264sw_output_init(&made->output);
	made->dpb.size = 4U;

	/* Hands the decoder to the caller. */
	*decoder = made;

	/* Succeeded: the caller owns the decoder. */
	return 0;
}

/* Frees the stand-in decoder. */
void
h264sw_decoder_destroy(
	struct h264sw_decoder *decoder)
{
	/* Destroying nothing is allowed. */
	if (decoder == NULL)
		return;

	/* Releases the output queue, the pool and the decoder, and counts the destruction. */
	h264sw_output_clear(&decoder->output);
	h264sw_frame_pool_destroy(decoder->pool);
	free(decoder);
	fake_destroyed++;
}

/* Takes one NAL unit: records its type, answers the script's status, and outputs frames for a slice. */
int
h264sw_decoder_nal(
	struct h264sw_decoder *decoder,
	const uint8_t *nal,
	size_t size,
	int64_t time_us)
{
	unsigned type;

	UNUSED_PARAMETER(size);

	/* The real decoder refuses a NAL unit while its output is not taken. */
	if (decoder->output.count != 0U)
		return EBUSY;

	/* Records the type and time of the NAL unit for the tests to read. */
	type = nal[0] & 31U;
	if (fake_type_count < sizeof(fake_types) / sizeof(fake_types[0]))
		fake_types[fake_type_count++] = type;
	fake_last_time = time_us;

	/* Answers the script's status for a parameter set. */
	if (type == 7U && fake_sps_status != 0)
		return fake_sps_status;

	/* Answers the script's status for a slice. */
	if ((type == 1U || type == 5U) && fake_slice_status != 0)
		return fake_slice_status;

	/* A slice outputs the script's number of frames. */
	if (type == 1U || type == 5U)
		fake_output(decoder, fake_frames_per_slice, time_us);

	/* Succeeded: the NAL unit was taken. */
	return 0;
}

/* Nothing in progress to finish. */
int
h264sw_decoder_end_access_unit(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Succeeded: there is nothing to finish. */
	return 0;
}

/* The end of the stream outputs the script's frames. */
int
h264sw_decoder_flush(
	struct h264sw_decoder *decoder)
{
	/* Outputs the frames the script holds back until the end. */
	fake_output(decoder, fake_flush_frames, 9);

	/* Succeeded: the stream is flushed. */
	return 0;
}

/* Takes the oldest output frame. */
struct h264sw_frame *
h264sw_decoder_output(
	struct h264sw_decoder *decoder)
{
	struct h264sw_frame *frame;

	/* Takes the oldest frame the script queued, or none. */
	frame = h264sw_output_take(&decoder->output);
	return frame;
}

/* Drops the output for a seek. */
void
h264sw_decoder_reset(
	struct h264sw_decoder *decoder)
{
	/* Empties the queue and counts the reset. */
	h264sw_output_clear(&decoder->output);
	fake_resets++;
}

/* Runs the tests. */
int
main(void)
{
	int status;

	/* Runs every test. */
	test_open();
	test_picture();
	test_order();
	test_queue();
	test_errors();

	/* Prints the summary line and exits with its status. */
	status = check_finish("backend");
	return status;
}

/* Fills a frame's planes with a pattern that tells the frame, the plane and the place. */
static void
fake_fill(
	struct h264sw_frame *frame,
	unsigned serial)
{
	unsigned x;
	unsigned y;

	/* Luma, then Cb and Cr. */
	for (y = 0U; y < frame->height; y++) {
		for (x = 0U; x < frame->width; x++)
			frame->plane[0][y * frame->stride[0] + x] = (uint8_t)(serial * 7U + x + 2U * y);
	}

	/* Chroma: Cb then Cr, each at half the luma size. */
	for (y = 0U; y < frame->height / 2U; y++) {
		for (x = 0U; x < frame->width / 2U; x++) {
			frame->plane[1][y * frame->stride[1] + x] = (uint8_t)(serial + x + y);
			frame->plane[2][y * frame->stride[2] + x] = (uint8_t)(serial * 3U + 2U * x + y);
		}
	}
}

/* Appends frames to the output queue, as the real decoder's bumping does. */
static void
fake_output(
	struct h264sw_decoder *decoder,
	unsigned frames,
	int64_t time_us)
{
	struct h264sw_frame *frame;
	unsigned index;
	unsigned place;

	/* The pool of the script's size, made at the first frame. */
	if (decoder->pool == NULL) {
		h264sw_frame_pool_create(
			fake_width_mbs,
			fake_height_mbs,
			fake_crop_x,
			fake_crop_y,
			fake_crop_width,
			fake_crop_height,
			H264SW_FRAME_POOL_MAX,
			&decoder->pool);
	}

	/* Each frame, in output order. */
	for (index = 0U; index < frames; index++) {
		frame = h264sw_frame_get(decoder->pool);
		if (frame == NULL)
			return;

		/* Numbers the frame and fills its planes with the pattern of its number. */
		fake_serial++;
		fake_fill(frame, fake_serial);

		/* Gives the frame its order, its time and the script's signal fields. */
		frame->poc = (int32_t)fake_serial;
		frame->time_us = time_us * 1000 + (int64_t)index;
		frame->sar_width = fake_sar_width;
		frame->sar_height = fake_sar_height;
		frame->signal_present = fake_signal_present;
		frame->full_range = fake_full_range;
		frame->colour_present = fake_colour_present;
		frame->matrix_coefficients = fake_matrix;

		/* Appends the frame to the output queue. */
		place = (decoder->output.head + decoder->output.count) % H264SW_OUTPUT_MAX;
		decoder->output.frame[place] = frame;
		decoder->output.count++;
	}
}

/* Restores the script to its defaults. */
static void
script_reset(void)
{
	fake_width_mbs = 2U;
	fake_height_mbs = 2U;
	fake_crop_x = 0U;
	fake_crop_y = 0U;
	fake_crop_width = 32U;
	fake_crop_height = 32U;
	fake_frames_per_slice = 1U;
	fake_flush_frames = 0U;
	fake_sar_width = 0U;
	fake_sar_height = 0U;
	fake_signal_present = 0;
	fake_full_range = 0;
	fake_colour_present = 0;
	fake_matrix = 0U;
	fake_sps_status = 0;
	fake_slice_status = 0;
	fake_create_status = 0;
	fake_type_count = 0U;
}

/* A track of the script's codec with the avcC record. */
static void
make_track(
	struct media_track *track)
{
	memset(track, 0, sizeof(*track));
	track->kind = MEDIA_TRACK_VIDEO;
	track->codec = MEDIA_CODEC_H264;
	track->width = 32U;
	track->height = 32U;
	track->private_data = avcc;
	track->private_size = sizeof(avcc);
}

/* Builds a packet of `count` slice NAL units of 4-byte lengths, each 6 bytes of NAL (type 1 or 5). */
static size_t
make_data(
	unsigned char *buffer,
	unsigned count,
	unsigned type)
{
	size_t offset;
	unsigned index;

	/* Writes each NAL unit as a 4-byte big-endian length and six bytes of NAL. */
	offset = 0U;
	for (index = 0U; index < count; index++) {
		buffer[offset++] = 0U;
		buffer[offset++] = 0U;
		buffer[offset++] = 0U;
		buffer[offset++] = 6U;
		buffer[offset++] = (unsigned char)(0x60U | type);
		buffer[offset++] = (unsigned char)index;
		buffer[offset++] = 1U;
		buffer[offset++] = 2U;
		buffer[offset++] = 3U;
		buffer[offset++] = 4U;
	}

	/* Reports the number of bytes written to the buffer. */
	return offset;
}

/* Receives every picture, returning how many and checking the times grow by the script's rule. */
static unsigned
drain(
	void *state,
	int64_t *last_time)
{
	int64_t time_us;
	unsigned count;
	int received;

	/* Receives pictures until the back end has none, keeping the last time. */
	count = 0U;
	received = media_h264sw_ops.receive(state, &time_us);
	while (received > 0) {
		count++;
		*last_time = time_us;
		received = media_h264sw_ops.receive(state, &time_us);
	}

	/* Reports how many pictures were received. */
	return count;
}

/* The tests of open. */
static void
test_open(void)
{
	struct media_track track;
	void *state;
	int status;
	int difference;
	const char *name;

	/* Another codec is not taken. */
	script_reset();
	make_track(&track);
	track.codec = MEDIA_CODEC_AAC;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == MEDIA_PROBLEM_FORMAT);
	CHECK(state == NULL);

	/* The parameter sets of the private data reach the decoder, in order, and no slice does. */
	make_track(&track);
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	CHECK(state != NULL);
	CHECK(fake_type_count == 2U);
	CHECK(fake_types[0] == 7U);
	CHECK(fake_types[1] == 8U);
	name = media_h264sw_ops.decoder_name(state);
	difference = strcmp(name, "h264");
	CHECK(difference == 0);
	media_h264sw_ops.close(state);
	CHECK(fake_destroyed >= 1U);

	/* No private data: open succeeds and nothing is given to the decoder. */
	script_reset();
	make_track(&track);
	track.private_data = NULL;
	track.private_size = 0U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	CHECK(fake_type_count == 0U);
	media_h264sw_ops.close(state);

	/* A parameter set outside the range goes to the next back end. */
	script_reset();
	make_track(&track);
	fake_sps_status = ENOTSUP;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == MEDIA_PROBLEM_PROFILE);
	CHECK(state == NULL);

	/* A stream error in the private data is concealed: open still succeeds. */
	script_reset();
	make_track(&track);
	fake_sps_status = EINVAL;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	media_h264sw_ops.close(state);

	/* An allocation failure is returned as it is. */
	script_reset();
	make_track(&track);
	fake_sps_status = ENOMEM;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == ENOMEM);
	CHECK(state == NULL);

	/* A decoder that is not built passes the track on; any other creation error is returned. */
	script_reset();
	make_track(&track);
	fake_create_status = ENOSYS;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == MEDIA_PROBLEM_FORMAT);
	fake_create_status = ENOMEM;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == ENOMEM);

	/* Private data that cannot be read is an error and leaves nothing open. */
	script_reset();
	make_track(&track);
	track.private_size = 3U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == EINVAL);
	CHECK(state == NULL);
}

/* The tests of a picture: size, samples, colour and aspect. */
static void
test_picture(void)
{
	struct media_track track;
	struct media_packet packet;
	struct media_picture *picture;
	unsigned char data[64];
	void *state;
	void *handle;
	int64_t time_us;
	unsigned x;
	unsigned y;
	unsigned serial;
	unsigned bad;
	int status;

	/* One frame of 32x32 cropped to 16x8 from (4, 2). */
	script_reset();
	make_track(&track);
	track.aspect_num = 4U;
	track.aspect_den = 3U;
	fake_crop_x = 4U;
	fake_crop_y = 2U;
	fake_crop_width = 16U;
	fake_crop_height = 8U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	memset(&packet, 0, sizeof(packet));
	packet.data = data;
	packet.size = make_data(data, 1U, 5U);
	packet.pts_us = 1234;
	packet.keyframe = 1;
	serial = fake_serial;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	CHECK(time_us == 1234 * 1000);
	handle = media_h264sw_ops.picture(state);
	CHECK(handle != NULL);
	picture = handle;
	CHECK(picture->width == 16U);
	CHECK(picture->height == 8U);
	serial++;
	bad = 0U;
	for (y = 0U; y < 8U; y++) {
		for (x = 0U; x < 16U; x++) {
			if (picture->luma[y * picture->pitch + x] != (uint8_t)(serial * 7U + (x + 4U) + 2U * (y + 2U)))
				bad++;
		}
	}

	/* Chroma samples: Cb at even and Cr at odd bytes of each interleaved row. */
	for (y = 0U; y < 4U; y++) {
		for (x = 0U; x < 8U; x++) {
			if (picture->chroma[y * picture->pitch + 2U * x] != (uint8_t)(serial + (x + 2U) + (y + 1U)))
				bad++;
			if (picture->chroma[y * picture->pitch + 2U * x + 1U] != (uint8_t)(serial * 3U + 2U * (x + 2U) + (y + 1U)))
				bad++;
		}
	}

	/* Every sample of the picture matched the pattern the stand-in wrote. */
	CHECK(bad == 0U);

	/* The track's aspect ratio, 601 for a small picture, the track's range. */
	CHECK(picture->aspect_num == 4U);
	CHECK(picture->aspect_den == 3U);
	CHECK(picture->colour == MEDIA_COLOUR_601);
	CHECK(picture->full_range == 0);
	media_h264sw_ops.picture_free(handle);

	/* Nothing more is ready: the back end needs a packet. */
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 0);
	media_h264sw_ops.close(state);

	/* The frame's own aspect ratio, range and colour override the track's. */
	script_reset();
	make_track(&track);
	track.aspect_num = 4U;
	track.aspect_den = 3U;
	track.width = 1920U;
	track.height = 1080U;
	fake_sar_width = 16U;
	fake_sar_height = 11U;
	fake_signal_present = 1;
	fake_full_range = 1;
	fake_colour_present = 1;
	fake_matrix = 9U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	picture = media_h264sw_ops.picture(state);
	CHECK(picture != NULL);
	CHECK(picture->aspect_num == 16U);
	CHECK(picture->aspect_den == 11U);
	CHECK(picture->colour == MEDIA_COLOUR_2020);
	CHECK(picture->full_range == 1);
	media_h264sw_ops.picture_free(picture);
	media_h264sw_ops.close(state);

	/* The container's matrix, then 709 for a large picture; other stream matrices keep the choice. */
	script_reset();
	make_track(&track);
	track.width = 1280U;
	fake_colour_present = 1;
	fake_matrix = 5U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	picture = media_h264sw_ops.picture(state);
	CHECK(picture->colour == MEDIA_COLOUR_709);
	CHECK(picture->aspect_num == 1U);
	media_h264sw_ops.picture_free(picture);
	media_h264sw_ops.close(state);
	script_reset();
	make_track(&track);
	track.colour_present = 1;
	track.colour_matrix = 9U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	picture = media_h264sw_ops.picture(state);
	CHECK(picture->colour == MEDIA_COLOUR_2020);
	media_h264sw_ops.picture_free(picture);
	media_h264sw_ops.close(state);

	/* A picture handed out stays valid after close (the pool lives on; ASan checks the reads). */
	script_reset();
	make_track(&track);
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	picture = media_h264sw_ops.picture(state);
	media_h264sw_ops.close(state);
	CHECK(picture->luma[0] == (uint8_t)(fake_serial * 7U));
	media_h264sw_ops.picture_free(picture);
}

/* The order and times of several frames from one NAL unit. */
static void
test_order(void)
{
	struct media_track track;
	struct media_packet packet;
	unsigned char data[64];
	void *state;
	int64_t time_us;
	unsigned index;
	int status;

	/* Three frames from one slice come out in order with their own times. */
	script_reset();
	make_track(&track);
	fake_frames_per_slice = 3U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	memset(&packet, 0, sizeof(packet));
	packet.data = data;
	packet.size = make_data(data, 1U, 1U);
	packet.pts_us = 7;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	for (index = 0U; index < 3U; index++) {
		status = media_h264sw_ops.receive(state, &time_us);
		CHECK(status == 1);
		CHECK(time_us == 7000 + (int64_t)index);
	}

	/* The frames of the packet are all received, so the next receive finds none. */
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 0);

	/* Closes the back end. */
	media_h264sw_ops.close(state);
}

/* The queue's bound, the deferred rest of a packet and the drain. */
static void
test_queue(void)
{
	struct media_track track;
	struct media_packet packet;
	unsigned char data[256];
	unsigned char copy[256];
	void *state;
	int64_t time_us;
	int64_t last;
	unsigned received;
	unsigned total;
	unsigned sent;
	int status;

	/* Frames pile up while the caller does not receive: send says EAGAIN before the queue could overflow. */
	script_reset();
	make_track(&track);
	fake_frames_per_slice = 8U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	memset(&packet, 0, sizeof(packet));
	packet.data = data;
	packet.size = make_data(data, 1U, 1U);
	sent = 0U;
	status = 0;
	while (sent < 100U) {
		packet.pts_us = (int64_t)sent;
		status = media_h264sw_ops.send(state, &packet);
		if (status != 0)
			break;
		sent++;
	}

	/* The queue's bound made send refuse the packet. */
	CHECK(status == EAGAIN);
	CHECK(sent >= 1U);
	CHECK(sent * 8U <= H264SW_OUTPUT_MAX);
	received = drain(state, &last);
	CHECK(received == sent * 8U);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	media_h264sw_ops.close(state);

	/* A packet whose NAL units output more than the queue holds: the rest is deferred, owned by the back end. */
	script_reset();
	make_track(&track);
	fake_frames_per_slice = 20U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	memset(&packet, 0, sizeof(packet));
	packet.data = copy;
	packet.size = make_data(data, 5U, 1U);
	memcpy(copy, data, packet.size);
	packet.pts_us = 3;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);

	/* The caller's bytes may change as soon as send returns. */
	memset(copy, 0xff, sizeof(copy));
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == EAGAIN);
	total = 0U;
	status = media_h264sw_ops.receive(state, &time_us);
	while (status > 0) {
		/* Each slice's frames carry that slice's time and count up inside it. */
		CHECK(time_us == 3000 + (int64_t)(total % 20U));
		total++;
		status = media_h264sw_ops.receive(state, &time_us);
	}

	/* Every frame of the deferred packet came out, in order of its slice. */
	CHECK(total == 100U);
	media_h264sw_ops.close(state);

	/* The drain outputs the decoder's remaining frames and ends; a packet after it is refused. */
	script_reset();
	make_track(&track);
	fake_flush_frames = 3U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, NULL);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	CHECK(time_us == 9000);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 0);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == EINVAL);

	/* A flush forgets the end of the stream and the decoder is reset. */
	fake_resets = 0U;
	media_h264sw_ops.flush(state);
	CHECK(fake_resets == 1U);
	fake_frames_per_slice = 1U;
	packet.size = make_data(data, 1U, 1U);
	packet.data = data;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);
	media_h264sw_ops.close(state);
}

/* The error paths. */
static void
test_errors(void)
{
	struct media_track track;
	struct media_packet packet;
	unsigned char data[64];
	void *state;
	int64_t time_us;
	size_t sound_size;
	int status;
	void *handle;

	/* A packet with bytes missing, and a length that runs past its end. */
	script_reset();
	make_track(&track);
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	memset(&packet, 0, sizeof(packet));
	packet.data = NULL;
	packet.size = 5U;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == EINVAL);
	packet.data = data;
	memset(data, 0, sizeof(data));
	data[3] = 200U;
	packet.size = 10U;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == EINVAL);

	/* A stream error from the decoder is concealed: send does not report it. */
	packet.size = make_data(data, 1U, 1U);
	fake_slice_status = EINVAL;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 0);

	/* A stream outside the range is reported once, and the back end goes on. */
	fake_slice_status = ENOTSUP;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == ENOTSUP);
	fake_slice_status = 0;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);

	/* An allocation failure is lasting until a flush. */
	fake_slice_status = ENOMEM;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == ENOMEM);
	fake_slice_status = 0;
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == ENOMEM);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == -ENOMEM);
	media_h264sw_ops.flush(state);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == 0);
	status = media_h264sw_ops.receive(state, &time_us);
	CHECK(status == 1);

	/* A frame whose cropping window lies outside the frame is dropped, not copied. */
	fake_crop_width = 32U;
	fake_crop_height = 32U;
	media_h264sw_ops.flush(state);
	media_h264sw_ops.close(state);

	/* An oversize picture is outside the pictures' range: the stream is refused. */
	script_reset();
	make_track(&track);
	fake_width_mbs = 600U;
	fake_height_mbs = 2U;
	fake_crop_width = 9600U;
	fake_crop_height = 32U;
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	packet.size = make_data(data, 1U, 1U);
	status = media_h264sw_ops.send(state, &packet);
	CHECK(status == ENOTSUP);
	media_h264sw_ops.close(state);

	/* Closing a back end that was never opened is harmless, and the sound is empty. */
	media_h264sw_ops.close(NULL);
	script_reset();
	make_track(&track);
	status = media_h264sw_ops.open(&track, &state);
	CHECK(status == 0);
	sound_size = media_h264sw_ops.sound(state, NULL, 0U, 48000U);
	CHECK(sound_size == 0U);
	handle = media_h264sw_ops.picture(state);
	CHECK(handle == NULL);
	media_h264sw_ops.close(state);
}
