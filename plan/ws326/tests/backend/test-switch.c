/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the decoder layer (decoder.c): its table of back ends is
 * three scripted stand-ins (media_vkvideo_ops, media_h264sw_ops and
 * media_aac_ops are defined below), and the test drives how a refusal of
 * the stream is handled: at open, at send before any packet was taken (the
 * same packet goes to the next back end), at send after (the old back end is
 * drained, packets are dropped until a key frame) and at the stream's end
 * before a key frame.
 */

#include "check.h"
#include "media-private.h"

#define UNUSED_PARAMETER(name) ((void)(name))

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many stand-in back ends there are: the order of decoder.c's table. */
#define FAKES 3U

/*
 * One stand-in back end's script and record.  open_status: what open
 * answers.  refuse_at: the number of the packet (counting from 1) that send
 * refuses with refuse_status, 0 for never.  opened, closed, sent, drained and
 * last_time: what happened to it.  pictures: the pictures it holds for
 * receive; each picture is the packet's time when it was sent.
 */
struct fake {
	const char *name;
	int open_status;
	unsigned refuse_at;
	int refuse_status;
	unsigned opened;
	unsigned closed;
	unsigned sent;
	unsigned drained;
	int64_t last_time;
	int last_key;
	int64_t pictures[16];
	unsigned count;
	unsigned handed;
	unsigned flushes;
};

static struct fake fakes[FAKES];
static unsigned live_pictures;
static char log_text[1024];

/* The library's log: the last lines are kept for the test to look at. */
void
media_log(
	const char *format,
	...)
{
	va_list args;
	char line[256];
	size_t used;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	used = strlen(log_text);
	if (used + strlen(line) + 2U < sizeof(log_text)) {
		strcat(log_text, line);
		strcat(log_text, "\n");
	}
}

static struct fake *
fake_of(
	const void *state)
{
	return (struct fake *)state;
}

static int
fake_load(void)
{
	return 0;
}

static const char *
fake_reason(void)
{
	return "";
}

static const char *
fake_name(
	const void *state)
{
	UNUSED_PARAMETER(state);
	return "h264";
}

static int
fake_send(
	void *state,
	const struct media_packet *packet)
{
	struct fake *fake;

	fake = fake_of(state);

	/* The end of the stream: every packet sent becomes a picture already, so there is nothing more to give. */
	if (packet == NULL) {
		fake->drained++;
		return 0;
	}

	/* The script's refusal. */
	if (fake->refuse_at != 0U && fake->sent + 1U == fake->refuse_at)
		return fake->refuse_status;
	fake->sent++;
	fake->last_time = packet->pts_us;
	fake->last_key = packet->keyframe;
	if (fake->count < 16U)
		fake->pictures[fake->count++] = packet->pts_us;
	return 0;
}

static int
fake_receive(
	void *state,
	int64_t *time_us)
{
	struct fake *fake;
	unsigned index;

	fake = fake_of(state);
	if (fake->count == 0U)
		return 0;
	*time_us = fake->pictures[0];
	for (index = 1U; index < fake->count; index++)
		fake->pictures[index - 1U] = fake->pictures[index];
	fake->count--;
	fake->handed = 1U;
	return 1;
}

static void *
fake_picture(
	void *state)
{
	UNUSED_PARAMETER(state);
	live_pictures++;
	return malloc(1U);
}

static size_t
fake_sound(
	void *state,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	UNUSED_PARAMETER(state);
	UNUSED_PARAMETER(samples);
	UNUSED_PARAMETER(capacity);
	UNUSED_PARAMETER(rate);
	return 0U;
}

static void
fake_flush(
	void *state)
{
	struct fake *fake;

	fake = fake_of(state);
	fake->flushes++;
	fake->count = 0U;
}

static void
fake_close(
	void *state)
{
	fake_of(state)->closed++;
}

static void
fake_picture_free(
	void *picture)
{
	live_pictures--;
	free(picture);
}

static void
fake_picture_size(
	const void *picture,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(picture);
	*width = 1;
	*height = 1;
}

static int
fake_open(
	const struct media_track *track,
	void **state,
	unsigned index)
{
	struct fake *fake;

	UNUSED_PARAMETER(track);
	fake = &fakes[index];
	*state = NULL;
	if (fake->open_status != 0)
		return fake->open_status;
	fake->opened++;
	*state = fake;
	return 0;
}

static int fake_open0(const struct media_track *track, void **state) { return fake_open(track, state, 0U); }
static int fake_open1(const struct media_track *track, void **state) { return fake_open(track, state, 1U); }
static int fake_open2(const struct media_track *track, void **state) { return fake_open(track, state, 2U); }

/* The three stand-ins, in the table's order; their names tell them apart. */
const struct media_decoder_ops media_vkvideo_ops = {
	"fake0", fake_load, fake_reason, fake_open0, fake_name, fake_send, fake_receive, fake_picture, fake_sound,
	fake_flush, fake_close, fake_picture_free, fake_picture_size, NULL, NULL, NULL, NULL, NULL, NULL
};
const struct media_decoder_ops media_h264sw_ops = {
	"fake1", fake_load, fake_reason, fake_open1, fake_name, fake_send, fake_receive, fake_picture, fake_sound,
	fake_flush, fake_close, fake_picture_free, fake_picture_size, NULL, NULL, NULL, NULL, NULL, NULL
};
const struct media_decoder_ops media_aac_ops = {
	"fake2", fake_load, fake_reason, fake_open2, fake_name, fake_send, fake_receive, fake_picture, fake_sound,
	fake_flush, fake_close, fake_picture_free, fake_picture_size, NULL, NULL, NULL, NULL, NULL, NULL
};

/* Resets the scripts and the log. */
static void
setup(void)
{
	unsigned index;

	memset(fakes, 0, sizeof(fakes));
	for (index = 0U; index < FAKES; index++)
		fakes[index].open_status = MEDIA_PROBLEM_FORMAT;
	log_text[0] = '\0';
}

/* Makes a track with private data to copy. */
static void
make_track(
	struct media_track *track,
	const unsigned char *private_data)
{
	memset(track, 0, sizeof(*track));
	track->codec = MEDIA_CODEC_H264;
	track->private_data = private_data;
	track->private_size = 4U;
}

/* Sends a packet of a time and a key flag. */
static int
send_packet(
	struct media_decoder *decoder,
	int64_t time_us,
	int key)
{
	struct media_packet packet;
	unsigned char byte;
	int status;

	byte = 0U;
	memset(&packet, 0, sizeof(packet));
	packet.pts_us = time_us;
	packet.keyframe = key;
	packet.data = &byte;
	packet.size = 1U;
	status = media_decoder_send(decoder, &packet);
	return status;
}

/* The order of opening and what open answers. */
static void
test_open(void)
{
	struct media_track track;
	struct media_decoder *decoder;
	unsigned char private_data[4];
	int status;

	memset(private_data, 7, sizeof(private_data));
	make_track(&track, private_data);

	/* No back end takes the codec. */
	setup();
	status = media_decoder_open(&track, &decoder);
	CHECK(status == MEDIA_PROBLEM_FORMAT);
	CHECK(decoder == NULL);

	/* The first back end that opens takes it. */
	setup();
	fakes[0].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake0") == 0);
	media_decoder_close(decoder);
	CHECK(fakes[0].closed == 1U);

	/* A refusal at open (device, profile, busy) goes on to the next back end. */
	setup();
	fakes[0].open_status = MEDIA_PROBLEM_DEVICE;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake1") == 0);
	media_decoder_close(decoder);
	setup();
	fakes[0].open_status = MEDIA_PROBLEM_BUSY;
	fakes[1].open_status = MEDIA_PROBLEM_PROFILE;
	fakes[2].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake2") == 0);
	media_decoder_close(decoder);

	/* When none opens, the first problem is returned. */
	setup();
	fakes[0].open_status = MEDIA_PROBLEM_DEVICE;
	fakes[1].open_status = MEDIA_PROBLEM_PROFILE;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == MEDIA_PROBLEM_DEVICE);
	CHECK(decoder == NULL);
	setup();
	fakes[0].open_status = MEDIA_PROBLEM_FORMAT;
	fakes[1].open_status = MEDIA_PROBLEM_PROFILE;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == MEDIA_PROBLEM_PROFILE);

	/* An allocation failure or another error ends the search. */
	setup();
	fakes[0].open_status = ENOMEM;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == ENOMEM);
	setup();
	fakes[0].open_status = EINVAL;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == EINVAL);
	CHECK(fakes[1].opened == 0U);
}

/* A refusal before any packet was taken sends the same packet to the next back end. */
static void
test_refusal_first(void)
{
	struct media_track track;
	struct media_decoder *decoder;
	unsigned char private_data[4];
	int status;

	memset(private_data, 7, sizeof(private_data));
	make_track(&track, private_data);
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 1U;
	fakes[0].refuse_status = ENOTSUP;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	status = send_packet(decoder, 40, 0);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake1") == 0);
	CHECK(fakes[0].closed == 1U);
	CHECK(fakes[0].drained == 0U);
	CHECK(fakes[1].sent == 1U);
	CHECK(fakes[1].last_time == 40);
	CHECK(fakes[1].last_key == 0);
	media_decoder_close(decoder);
	CHECK(fakes[1].closed == 1U);

	/* The reasons ENODEV and EBUSY switch as well; an ordinary error does not. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 1U;
	fakes[0].refuse_status = ENODEV;
	fakes[1].open_status = MEDIA_PROBLEM_PROFILE;
	fakes[2].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	status = send_packet(decoder, 1, 1);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake2") == 0);
	media_decoder_close(decoder);
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 1U;
	fakes[0].refuse_status = EINVAL;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	status = send_packet(decoder, 1, 1);
	CHECK(status == EINVAL);
	CHECK(strcmp(media_decoder_backend(decoder), "fake0") == 0);
	media_decoder_close(decoder);

	/* When no back end follows, the refusal stands and the decoder stays as it was. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 1U;
	fakes[0].refuse_status = ENOTSUP;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	status = send_packet(decoder, 1, 1);
	CHECK(status == ENOTSUP);
	CHECK(strcmp(media_decoder_backend(decoder), "fake0") == 0);
	CHECK(fakes[0].closed == 0U);
	media_decoder_close(decoder);

	/* The next one refuses too: the one after it takes the packet. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 1U;
	fakes[0].refuse_status = ENOTSUP;
	fakes[1].open_status = 0;
	fakes[1].refuse_at = 1U;
	fakes[1].refuse_status = EBUSY;
	fakes[2].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	status = send_packet(decoder, 5, 0);
	CHECK(status == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake2") == 0);
	CHECK(fakes[0].closed == 1U);
	CHECK(fakes[1].closed == 1U);
	CHECK(fakes[2].sent == 1U);
	media_decoder_close(decoder);
}

/* A refusal after packets were taken: drain, drop to a key frame, start there. */
static void
test_refusal_late(void)
{
	struct media_track track;
	struct media_decoder *decoder;
	struct media_frame *frame;
	unsigned char private_data[4];
	int64_t time_us;
	int status;

	memset(private_data, 7, sizeof(private_data));
	make_track(&track, private_data);
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 4U;
	fakes[0].refuse_status = EBUSY;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(send_packet(decoder, 20, 0) == 0);
	CHECK(send_packet(decoder, 30, 0) == 0);

	/* The fourth is refused: it is dropped (not a key frame), the old back end drained and closed. */
	status = send_packet(decoder, 40, 0);
	CHECK(status == 0);
	CHECK(fakes[0].drained == 1U);
	CHECK(fakes[0].closed == 1U);
	CHECK(fakes[1].opened == 1U);
	CHECK(fakes[1].sent == 0U);
	CHECK(strcmp(media_decoder_backend(decoder), "fake1") == 0);

	/* More non-key packets are dropped. */
	CHECK(send_packet(decoder, 50, 0) == 0);
	CHECK(fakes[1].sent == 0U);

	/* A key frame goes to the new back end. */
	CHECK(send_packet(decoder, 60, 1) == 0);
	CHECK(fakes[1].sent == 1U);
	CHECK(fakes[1].last_time == 60);
	CHECK(fakes[1].last_key == 1);
	CHECK(strstr(log_text, "after 2 packets dropped") != NULL);

	/* The drained pictures come out first, each with its own time, then the new back end's. */
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 10);
	frame = media_decoder_picture(decoder);
	CHECK(frame != NULL);
	CHECK(media_decoder_picture(decoder) == NULL);
	media_frame_free(&frame);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 20);

	/* A picture received and not taken is let go by the next receive. */
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 30);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 60);
	frame = media_decoder_picture(decoder);
	CHECK(frame != NULL);
	media_frame_free(&frame);
	CHECK(media_decoder_receive(decoder, &time_us) == 0);
	media_decoder_close(decoder);
	CHECK(live_pictures == 0U);

	/* A refused packet that is a key frame goes straight to the new back end. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 2U;
	fakes[0].refuse_status = ENOTSUP;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(send_packet(decoder, 20, 1) == 0);
	CHECK(fakes[0].drained == 1U);
	CHECK(fakes[1].sent == 1U);
	CHECK(fakes[1].last_time == 20);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 10);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 20);
	media_decoder_close(decoder);

	/* The stream ends before a key frame arrives: the end is the refusal's error, and the drained pictures remain receivable. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 3U;
	fakes[0].refuse_status = ENODEV;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(send_packet(decoder, 20, 0) == 0);
	CHECK(send_packet(decoder, 30, 0) == 0);
	CHECK(send_packet(decoder, 40, 0) == 0);
	status = media_decoder_send(decoder, NULL);
	CHECK(status == ENODEV);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 10);
	media_decoder_close(decoder);
	CHECK(live_pictures == 0U);

	/* The stream ends normally: the drain goes to the back end in use. */
	setup();
	fakes[0].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(media_decoder_send(decoder, NULL) == 0);
	CHECK(fakes[0].drained == 1U);
	media_decoder_close(decoder);

	/* A seek forgets the drained pictures and the wait for a key frame. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 3U;
	fakes[0].refuse_status = EBUSY;
	fakes[1].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(send_packet(decoder, 20, 0) == 0);
	CHECK(send_packet(decoder, 30, 0) == 0);
	media_decoder_flush(decoder);
	CHECK(fakes[1].flushes == 1U);
	CHECK(media_decoder_receive(decoder, &time_us) == 0);
	CHECK(send_packet(decoder, 70, 0) == 0);
	CHECK(fakes[1].sent == 1U);
	media_decoder_close(decoder);

	/* The second back end refuses too, after taking packets: the third takes over at a key frame. */
	setup();
	fakes[0].open_status = 0;
	fakes[0].refuse_at = 2U;
	fakes[0].refuse_status = EBUSY;
	fakes[1].open_status = 0;
	fakes[1].refuse_at = 2U;
	fakes[1].refuse_status = ENOTSUP;
	fakes[2].open_status = 0;
	status = media_decoder_open(&track, &decoder);
	CHECK(status == 0);
	CHECK(send_packet(decoder, 10, 1) == 0);
	CHECK(send_packet(decoder, 20, 1) == 0);
	CHECK(send_packet(decoder, 30, 1) == 0);
	CHECK(strcmp(media_decoder_backend(decoder), "fake2") == 0);
	CHECK(fakes[2].sent == 1U);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 10);
	CHECK(media_decoder_receive(decoder, &time_us) == 1);
	CHECK(time_us == 20);
	media_decoder_close(decoder);
	CHECK(live_pictures == 0U);
}

/* Runs the tests. */
int
main(void)
{
	test_open();
	test_refusal_first();
	test_refusal_late();
	return check_finish("switch");
}
