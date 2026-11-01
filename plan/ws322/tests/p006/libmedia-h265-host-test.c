/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of libmedia's H.265 Vulkan Video back end
 * (userland/desktop/libmedia/vkvideo-h265.c) over a stand-in context
 * (the media_vkvideo_runtime_* calls it makes, written here): one Annex B
 * bitstream (a file, or the standard input with "-") is given as one
 * packet, then drained, and every picture received is compared, in order,
 * with the output order of libmedia's own reader and DPB run alone (as
 * plan/ws324/tests/conformance/conform.c runs them).  With --packets each
 * access unit is a packet of its own (the first from the start of the
 * stream), as a container gives them.  The stand-in checks
 * each decode: its slot is one of the context's and no picture in flight
 * or held uses it; each reference names a slot that holds the picture of
 * the order count the record gives; a picture is received only after its
 * decode completed.  The context keeps one picture in flight, as a device
 * that reads pictures back does, and has the slots the shape asks for; it
 * refuses a shape past 4096x2304 (the stream then ends in an error, as on
 * a device that refuses it), and the back end refuses a profile other
 * than Main 8-bit 4:2:0 (ENOTSUP).
 *
 * Last line: "libmedia-h265: <decoded> decoded, <received> received of <expected>, <checks> checks, <failed> failed".
 *
 *   libmedia-h265-host-test STREAM|-
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userland/desktop/libmedia/h265-dpb.h"
#include "userland/desktop/libmedia/h265-stream.h"
#include "userland/desktop/libmedia/media-private.h"
#include "userland/desktop/libmedia/picture.h"
#include "userland/desktop/libmedia/vkvideo-runtime.h"

/* The largest bitstream read, and the most pictures followed. */
#define TEST_MAX_INPUT		(256U * 1024U * 1024U)
#define TEST_PICTURES		100000U

static unsigned long checks;
static unsigned long failures;

static void
check(
	int ok,
	const char *what)
{
	checks++;
	if (ok)
		return;
	failures++;
	if (failures <= 30U)
		printf("FAIL %s\n", what);
}

/*
 * The stand-in context: its slots, the order count each slot's picture
 * has (INT32_MIN for none), the one picture in flight (its slot and
 * ticket, 0 for none), the tickets given, and which pool picture got which
 * order count and whether its decode completed.
 */
struct vkvideo_runtime {
	uint32_t slots;
	int32_t slot_poc[MEDIA_GPU_SLOTS];
	uint32_t flying_ticket;
	uint32_t flying_slot;
	uint32_t tickets;
};

static struct vkvideo_runtime stand_in;
static int stand_in_open;
static int refused;
static unsigned long decoded;
static struct media_picture *picture_of[TEST_PICTURES];
static int32_t poc_of[TEST_PICTURES];
static int done_of[TEST_PICTURES];
static uint32_t ticket_of[TEST_PICTURES];

/* The picture record of a pool picture (its last decode), or -1. */
static long
record_of(
	const struct media_picture *picture)
{
	long index;

	for (index = (long)decoded - 1; index >= 0; index--) {
		if (picture_of[index] == picture)
			return index;
	}
	return -1;
}

int
media_vkvideo_runtime_open_h265(
	const struct vkvideo_shape *shape,
	unsigned reorder,
	struct vkvideo_runtime **runtime)
{
	unsigned slot;

	/* A shape past the stand-in device's 4096x2304 is refused, as a device refuses it. */
	(void)reorder;
	check(shape->references >= 1U && shape->references <= 16U, "a shape of 1 to 16 references");
	if (shape->width > 4096U || shape->height > 2304U) {
		refused = 1;
		return MEDIA_PROBLEM_PROFILE;
	}
	memset(&stand_in, 0, sizeof(stand_in));
	stand_in.slots = shape->references + 2U;
	for (slot = 0; slot < MEDIA_GPU_SLOTS; slot++)
		stand_in.slot_poc[slot] = INT32_MIN;
	stand_in_open++;
	*runtime = &stand_in;
	return 0;
}

int media_vkvideo_runtime_gpu(const struct vkvideo_runtime *runtime) { (void)runtime; return 0; }
uint32_t media_vkvideo_runtime_slots(const struct vkvideo_runtime *runtime) { return runtime->slots; }
uint32_t media_vkvideo_runtime_flights(const struct vkvideo_runtime *runtime) { (void)runtime; return 1U; }
int media_vkvideo_runtime_skips_failed(const struct vkvideo_runtime *runtime) { (void)runtime; return 0; }
void media_vkvideo_runtime_reset(struct vkvideo_runtime *runtime) { (void)runtime; }

int
media_vkvideo_runtime_parameters_h265(
	struct vkvideo_runtime *runtime,
	const VkVideoDecodeH265SessionParametersAddInfoKHR *add)
{
	(void)runtime;
	check(add->stdSPSCount >= 1U && add->stdPPSCount >= 1U, "parameters with an SPS and a PPS");
	return 0;
}

/* Completes the picture in flight. */
static void
complete(
	struct vkvideo_runtime *runtime)
{
	unsigned long index;

	if (runtime->flying_ticket == 0U)
		return;
	for (index = 0; index < decoded; index++) {
		if (ticket_of[index] == runtime->flying_ticket)
			done_of[index] = 1;
	}
	runtime->flying_ticket = 0U;
}

int media_vkvideo_runtime_reserve(struct vkvideo_runtime *runtime) { complete(runtime); return 0; }
void media_vkvideo_runtime_discard(struct vkvideo_runtime *runtime) { complete(runtime); }
void media_vkvideo_runtime_close(struct vkvideo_runtime *runtime) { if (runtime != NULL) complete(runtime); }

uint32_t
media_vkvideo_runtime_busy(
	const struct vkvideo_runtime *runtime)
{
	if (runtime->flying_ticket == 0U)
		return 0U;
	return 1U << runtime->flying_slot;
}

int
media_vkvideo_runtime_finish(
	struct vkvideo_runtime *runtime,
	uint32_t ticket)
{
	if (ticket == runtime->flying_ticket)
		complete(runtime);
	return 0;
}

int
media_vkvideo_runtime_submit_h265(
	struct vkvideo_runtime *runtime,
	const struct vkvideo_job *job,
	struct media_picture *output,
	uint32_t *ticket)
{
	const StdVideoDecodeH265PictureInfo *info;
	const StdVideoDecodeH265ReferenceInfo *references;
	uint32_t index;
	int32_t slot;

	/* The slot: the context's, not in flight. */
	complete(runtime);
	info = job->picture_info;
	references = job->reference_info;
	check(job->setup >= 0 && (uint32_t)job->setup < runtime->slots, "the setup slot is the context's");
	check(job->slice_count >= 1U && job->width != 0U && job->height != 0U, "the job's slices and size");

	/* Each reference holds the picture its record names; the setup slot is not one of them. */
	for (index = 0; index < job->reference_count; index++) {
		slot = job->references[index];
		check(slot >= 0 && (uint32_t)slot < runtime->slots && runtime->slot_poc[slot] == references[index].PicOrderCntVal,
		      "a reference slot holds its picture");
		check(slot != job->setup, "the setup slot is not a reference");
	}

	/* The picture: its slot's, in flight. */
	if (decoded < TEST_PICTURES) {
		picture_of[decoded] = output;
		poc_of[decoded] = info->PicOrderCntVal;
		done_of[decoded] = 0;
		runtime->tickets++;
		ticket_of[decoded] = runtime->tickets;
		decoded++;
	}
	runtime->slot_poc[job->setup] = info->PicOrderCntVal;
	runtime->flying_ticket = runtime->tickets;
	runtime->flying_slot = (uint32_t)job->setup;
	*ticket = runtime->tickets;
	return 0;
}

/* The decoders' GPU pictures, which this back end's pool does not ask for. */
unsigned media_decoder_gpu_hold(void) { return 0; }

/* Reads a whole file, or the standard input for "-". */
static int
read_input(
	const char *path,
	uint8_t **data,
	size_t *size)
{
	FILE *file;
	uint8_t *buffer;

	file = stdin;
	if (strcmp(path, "-") != 0)
		file = fopen(path, "rb");
	if (file == NULL)
		return errno;
	buffer = malloc(TEST_MAX_INPUT);
	if (buffer == NULL)
		return ENOMEM;
	*size = fread(buffer, 1U, TEST_MAX_INPUT, file);
	if (file != stdin)
		fclose(file);
	*data = buffer;
	return 0;
}

/* The output order of the reader and the DPB alone: the order counts, in order. */
static unsigned long
expected_order(
	const uint8_t *data,
	size_t size,
	int32_t *order,
	size_t *units,
	unsigned long *unit_count)
{
	struct h265_stream *stream;
	struct h265_picture *picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	unsigned long count;
	unsigned index;
	unsigned outputs;
	int error;

	stream = calloc(1U, sizeof(*stream));
	picture = calloc(1U, sizeof(*picture));
	h265_stream_init(stream);
	h265_stream_input(stream, data, size);
	h265_dpb_init(&dpb, 17U);
	count = 0;
	for (;;) {
		error = h265_stream_next(stream, picture);
		if (error == ENOENT)
			break;
		if (error != 0)
			continue;
		if (*unit_count < TEST_PICTURES) {
			units[*unit_count] = picture->access_unit;
			(*unit_count)++;
		}
		error = h265_dpb_begin(&dpb, &stream->store.sps[picture->sps_id], &picture->rps,
				       picture->irap != 0 && picture->no_rasl_output != 0, picture->no_output_of_prior_pics, &plan);
		for (index = 0; index < plan.output_count && (error == 0 || error == EINVAL); index++) {
			if (count < TEST_PICTURES)
				order[count++] = plan.output[index].poc;
			h265_dpb_release(&dpb, plan.output[index].slot);
		}
		if (error != 0)
			continue;
		if (h265_dpb_take_slot(&dpb, &plan) != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}
		if (h265_dpb_end(&dpb, &plan, picture->poc, picture->pic_output, picture->index, output, &outputs) != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}
		for (index = 0; index < outputs; index++) {
			if (count < TEST_PICTURES)
				order[count++] = output[index].poc;
			h265_dpb_release(&dpb, output[index].slot);
		}
	}
	if (h265_dpb_flush(&dpb, output, &outputs) == 0) {
		for (index = 0; index < outputs; index++) {
			if (count < TEST_PICTURES)
				order[count++] = output[index].poc;
		}
	}
	h265_stream_destroy(stream);
	free(picture);
	free(stream);
	return count;
}

int
main(
	int argc,
	char **argv)
{
	static int32_t order[TEST_PICTURES];
	static size_t units[TEST_PICTURES];
	unsigned long unit_count;
	unsigned long sent;
	size_t start;
	size_t end;
	int packets;
	struct media_track track;
	struct media_packet packet;
	struct media_picture *got;
	unsigned long expected;
	unsigned long received;
	int64_t time;
	void *state;
	uint8_t *data;
	size_t size;
	long record;
	int error;

	packets = argc == 3 && strcmp(argv[1], "--packets") == 0;
	if (argc != 2 && !packets) {
		fprintf(stderr, "usage: libmedia-h265-host-test [--packets] STREAM|-\n");
		return 2;
	}
	error = read_input(argv[argc - 1], &data, &size);
	if (error != 0)
		return 2;
	unit_count = 0;
	expected = expected_order(data, size, order, units, &unit_count);

	/* The back end on the stream: one packet, then the drain. */
	memset(&track, 0, sizeof(track));
	track.codec = MEDIA_CODEC_HEVC;
	error = media_vkvideo_h265_ops.open(&track, &state);
	check(error == 0, "the back end opened the track");
	if (error != 0) {
		printf("libmedia-h265: open failed (%d)\n", error);
		return 1;
	}
	memset(&packet, 0, sizeof(packet));
	packet.keyframe = 1;
	packet.data = data;
	packet.size = size;
	sent = 1;
	if (packets && unit_count != 0) {
		end = units[0];
		if (unit_count > 1)
			end = units[1];
		packet.size = unit_count > 1 ? end : size;
	} else {
		unit_count = 1;
	}
	error = media_vkvideo_h265_ops.send(state, &packet);
	check(error == 0, "the packet was taken");
	received = 0;
	for (;;) {
		error = media_vkvideo_h265_ops.receive(state, &time);
		if (error == 0 && sent < unit_count) {
			/* The next access unit as a packet, from its start to the next one's (the first packet from the start). */
			start = units[sent];
			end = sent + 1 < unit_count ? units[sent + 1] : size;
			packet.data = data + start;
			packet.size = end - start;
			error = media_vkvideo_h265_ops.send(state, &packet);
			check(error == 0, "an access unit was taken");
			sent++;
			continue;
		}
		if (error == 0) {
			error = media_vkvideo_h265_ops.send(state, NULL);
			if (error == EAGAIN)
				continue;
			check(error == 0, "the drain started");
			error = media_vkvideo_h265_ops.receive(state, &time);
			if (error == 0)
				break;
		}
		if (error == -ENOTSUP)
			refused = 1;
		if (error < 0 && refused)
			break;
		check(error > 0, "a picture was received");
		if (error <= 0)
			break;

		/* The picture: its decode completed, its order count the next one expected. */
		got = media_vkvideo_h265_ops.picture(state);
		record = record_of(got);
		check(record >= 0 && done_of[record], "a received picture's decode completed");
		check(received < expected && record >= 0 && poc_of[record] == order[received], "the pictures come in output order");
		if (got != NULL)
			media_picture_unref(got);
		received++;
	}
	check(refused || received == expected, "every picture output was received");
	media_vkvideo_h265_ops.close(state);
	printf("libmedia-h265: %lu decoded, %lu received of %lu%s, %lu checks, %lu failed\n", decoded, received, expected,
	       refused ? " (refused by the stand-in device)" : "", checks, failures);
	free(data);
	return failures != 0 || expected == 0;
}
