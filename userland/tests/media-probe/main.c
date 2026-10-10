/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native libmedia acceptance observes linear CPU pictures, never a vendor-specific GPU allocation or optional application codec. */
#include "userland/desktop/libmedia/media-private.h"
#include "userland/desktop/libmedia/picture.h"
#include "userland/base/common/sha256.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One packet can emit the bounded native reorder queue and at most one new picture. */
#define PROBE_RESULTS 32U

/* One receive result's presentation identity is retained until both simultaneous decoders have been compared. */
struct probe_result {
	int64_t time;
	char hash[65];
};

/* One invocation owns its file, native decoders, reference streams and audio RMS accumulator until cleanup. */
struct probe {
	struct media_file *file;
	struct media_decoder *decoder[2];
	FILE *expected[2];
	unsigned track;
	unsigned copies;
	int audio;
	struct probe_result results[2][PROBE_RESULTS];
	unsigned received[2];
	uint64_t frames[2];
	uint64_t sound_frames;
	uint64_t sound_sum[2];
	unsigned sound_count;
	uint64_t opened_us;
};

static unsigned probe_option(const char *argument);
static int probe_open(struct probe *probe, const char *path, const char *expected, int64_t seek);
static int probe_feed(struct probe *probe, unsigned decoder, const struct media_packet *packet);
static int probe_picture(struct probe *probe, unsigned decoder, int64_t time);
static int probe_hash(const struct media_picture *picture, char hash[65]);
static int probe_expect(FILE *expected, int64_t time, const char *hash);
static int probe_complete(struct probe *probe);
static void probe_sound(struct probe *probe, unsigned decoder);
static void probe_rms(struct probe *probe);
static uint64_t probe_now(void);

/*
 * Print native display hashes or normalized stereo RMS, optionally checking a second simultaneous decoder and seek.
 */
int
main(
	int argc,
	char **argv)
{
	struct probe probe;
	struct media_packet packet;
	const char *path;
	const char *expected;
	char *end;
	int64_t seek;
	double seconds;
	uint64_t started;
	unsigned index;
	unsigned decoder;
	unsigned result;
	unsigned option;
	int timed;
	int error;
	int read;
	int compared;

	/* All options are bounded to this file's native acceptance; no software fallback can obscure a device refusal. */
	memset(&probe, 0, sizeof(probe));
	probe.copies = 1U;
	path = NULL;
	expected = NULL;
	seek = 0;
	timed = 0;
	for (index = 1U; index < (unsigned)argc; index++) {
		option = probe_option(argv[index]);
		if (option == 1U) {
			probe.audio = 1;
		} else if (option == 2U) {
			probe.audio = 0;
		} else if (option == 3U) {
			probe.copies = 2U;
		} else if (option == 4U) {
			timed = 1;
		} else if (option == 5U) {
			expected = argv[index] + 9U;
		} else if (option == 6U) {
			seconds = strtod(argv[index] + 7U, &end);
			if (*end != '\0' || end == argv[index] + 7U)
				return 2;
			if (!(seconds >= 0.0 && seconds <= 2147483647.0))
				return 2;
			seek = (int64_t)(seconds * 1000000.0);
		} else if (argv[index][0] != '-' && path == NULL) {
			path = argv[index];
		} else {
			return 2;
		}
	}

	/* RMS is one stream's sample sequence; simultaneous-decoder comparison applies to video hashes. */
	if (
		path == NULL ||
		(probe.audio &&
		 (probe.copies != 1U || expected != NULL))) {
		fprintf(stderr, "usage: media-probe [--video-hash|--audio-rms] [--seek=SECONDS] [--expect=FILE] [--time] [--twice] FILE\n");
		return 2;
	}

	/* Open native sessions before beginning the timed packet loop. */
	started = probe_now();
	error = probe_open(&probe, path, expected, seek);
	probe.opened_us = probe_now() - started;
	while (error == 0) {
		read = media_file_read(probe.file, &packet);
		if (read != 0 && read != ENODATA) {
			error = read;
			break;
		}

		/* Only the selected track is fed; genuine end of data drains every instance. */
		if (read == 0 && packet.track != probe.track)
			continue;
		for (decoder = 0U; decoder < probe.copies; decoder++) {
			probe.received[decoder] = 0U;
			if (read == ENODATA)
				error = probe_feed(&probe, decoder, NULL);
			else
				error = probe_feed(&probe, decoder, &packet);
			if (error != 0)
				break;
		}

		/* Two simultaneous native sessions must publish the same picture identities for each accepted packet. */
		if (error == 0 && probe.copies == 2U) {
			if (probe.received[0] != probe.received[1])
				error = EIO;
			for (result = 0U; result < probe.received[0] && error == 0; result++) {
				compared = strcmp(probe.results[0][result].hash, probe.results[1][result].hash);
				if (compared != 0 || probe.results[0][result].time != probe.results[1][result].time)
					error = EIO;
			}
		}

		/* Every decoder has received its final drain, or a runtime error ended this bounded attempt. */
		if (read == ENODATA)
			break;
	}

	/* An expected stream must be consumed completely; matching a shortened prefix cannot prove pixel identity. */
	if (error == 0)
		error = probe_complete(&probe);

	/* Print any final partial RMS block and separate elapsed timing from a hardware pixel claim. */
	if (probe.sound_count != 0U)
		probe_rms(&probe);
	if (timed)
		fprintf(stderr, "media-probe: open_us=%llu elapsed_us=%llu\n", (unsigned long long)probe.opened_us, (unsigned long long)(probe_now() - started));
	fprintf(stderr, "media-probe: frames=%llu sound_frames=%llu copies=%u error=%d\n", (unsigned long long)probe.frames[0], (unsigned long long)probe.sound_frames, probe.copies, error);

	/* Both ordinary completion and partial open unwind through the same native owners. */
	for (decoder = 0U; decoder < 2U; decoder++) {
		media_decoder_close(probe.decoder[decoder]);
		if (probe.expected[decoder] != NULL)
			fclose(probe.expected[decoder]);
	}

	/* Close the input after decoder ownership has been released. */
	if (probe.file != NULL)
		media_file_close(probe.file);
	if (error != 0)
		return 1;
	return 0;
}

/* Verify actual output exists and no unconsumed expected picture remains after native EOF drain. */
static int
probe_complete(
	struct probe *probe)
{
	long long time;
	char hash[65];
	unsigned decoder;
	int fields;

	/* Empty output is a failed playback attempt rather than a successful diagnostic. */
	if (probe->audio) {
		if (probe->sound_frames == 0U)
			return ENODATA;
		return 0;
	}

	/* Every simultaneous decoder must publish pictures and exhaust its independently opened reference cursor. */
	for (decoder = 0U; decoder < probe->copies; decoder++) {
		if (probe->frames[decoder] == 0U)
			return ENODATA;
		if (probe->expected[decoder] == NULL)
			continue;
		fields = fscanf(probe->expected[decoder], "%lld %64s", &time, hash);
		if (fields != EOF)
			return EIO;
		fields = ferror(probe->expected[decoder]);
		if (fields != 0)
			return EIO;
	}

	/* Succeeded: all expected output was observed through ordinary native receive operations. */
	return 0;
}

/* Match one supported command-line option before the invocation applies its meaning. */
static unsigned
probe_option(
	const char *argument)
{
	/* Names and prefix sizes are immutable process-lifetime command-line syntax. */
	static const char *const names[] = {"--audio-rms", "--video-hash", "--twice", "--time", "--expect=", "--seek="};
	unsigned index;
	int compared;

	/* Exact switches precede the two switches which carry their value after an equals sign. */
	for (index = 0U; index < 6U; index++) {
		if (index == 4U)
			compared = strncmp(argument, names[index], 9U);
		else if (index == 5U)
			compared = strncmp(argument, names[index], 7U);
		else
			compared = strcmp(argument, names[index]);
		if (compared == 0)
			return index + 1U;
	}

	/* The caller separately accepts one positional input path or rejects an unknown option. */
	return 0U;
}

/* Open a native track and optional hash references, then establish a seek with AAC overlap preroll. */
static int
probe_open(
	struct probe *probe,
	const char *path,
	const char *expected,
	int64_t seek)
{
	const struct media_track *track;
	unsigned count;
	unsigned index;
	unsigned decoder;
	unsigned kind;
	int64_t start;
	int64_t preroll;
	int error;

	/* Find the first requested track without selecting a different codec after native admission fails. */
	error = media_file_open(path, &probe->file);
	if (error != 0)
		return error;
	kind = MEDIA_TRACK_VIDEO;
	if (probe->audio)
		kind = MEDIA_TRACK_AUDIO;
	count = media_file_track_count(probe->file);
	track = NULL;
	for (index = 0U; index < count; index++) {
		track = media_file_track(probe->file, index);
		if (track->kind == kind)
			break;
	}

	/* Refuse files without the requested track before opening any GPU or audio decoder. */
	if (index == count)
		return ENOENT;
	probe->track = index;
	for (decoder = 0U; decoder < probe->copies; decoder++) {
		error = media_decoder_open(track, &probe->decoder[decoder]);
		if (error != 0) {
			fprintf(stderr, "media-probe: admission problem=%d\n", error);
			return error;
		}

		/* Each comparison instance consumes an independent reference cursor. */
		if (expected != NULL) {
			probe->expected[decoder] = fopen(expected, "r");
			if (probe->expected[decoder] == NULL)
				return errno;
		}
	}

	/* Backend identity distinguishes physical standard decode from application software fallback. */
	fprintf(stderr, "media-probe: codec=%s backend=%s\n", media_decoder_name(probe->decoder[0]), media_decoder_backend(probe->decoder[0]));
	if (seek <= 0)
		return 0;
	preroll = 0;
	if (probe->audio)
		preroll = media_decoder_frame_us(probe->decoder[0]);
	start = seek - preroll;
	if (start < 0)
		start = 0;
	error = media_file_seek(probe->file, start);
	if (error != 0)
		return error;
	for (decoder = 0U; decoder < probe->copies; decoder++) {
		media_decoder_flush(probe->decoder[decoder]);
		if (probe->audio) {
			error = media_decoder_trim(probe->decoder[decoder], seek);
			if (error != 0)
				return error;
		}
	}

	/* The probe reports all decoded video preroll; the player's own target-time presentation discards it. */
	return 0;
}

/* Feed with ordinary EAGAIN semantics and consume all resulting native output. */
static int
probe_feed(
	struct probe *probe,
	unsigned decoder,
	const struct media_packet *packet)
{
	int64_t time;
	int error;
	int received;
	int attempts;

	/* One receive pass makes room before an unaccepted packet is retried. */
	attempts = 0;
	for (;;) {
		error = media_decoder_send(probe->decoder[decoder], packet);
		if (error != 0 && error != EAGAIN)
			return error;
		for (;;) {
			received = media_decoder_receive(probe->decoder[decoder], &time);
			if (received < 0)
				return -received;
			if (received == 0)
				break;
			if (probe->audio) {
				probe_sound(probe, decoder);
			} else {
				received = probe_picture(probe, decoder, time);
				if (received != 0)
					return received;
			}
		}

		/* Retry only an unaccepted packet, with a finite guard against a stalled decoder. */
		if (error == 0)
			return 0;
		attempts++;
		if (attempts >= 2)
			return EAGAIN;
	}
}

/* Hash one retained CPU picture through the library's private representation without adding a probe-only public API. */
static int
probe_picture(
	struct probe *probe,
	unsigned decoder,
	int64_t time)
{
	struct media_frame *frame;
	const struct media_picture *picture;
	struct probe_result *result;
	int error;

	/* Only the native Vulkan backend publishes this CPU picture representation. */
	if (probe->received[decoder] >= PROBE_RESULTS)
		return EOVERFLOW;
	frame = media_decoder_picture(probe->decoder[decoder]);
	if (frame == NULL)
		return ENOMEM;
	picture = frame->picture;
	result = &probe->results[decoder][probe->received[decoder]];
	result->time = time;
	error = probe_hash(picture, result->hash);
	media_frame_free(&frame);
	if (error != 0)
		return error;

	/* A seek may begin after earlier reference rows; exact time and pixels must still match its first actual output. */
	if (probe->expected[decoder] != NULL) {
		error = probe_expect(probe->expected[decoder], time, result->hash);
		if (error != 0)
			return error;
	}

	/* Only one copy is printed; the second participates in identity comparison. */
	if (decoder == 0U)
		printf("%lld %s\n", (long long)time, result->hash);
	probe->received[decoder]++;
	probe->frames[decoder]++;
	return 0;
}

/* Hash the visible luma window followed by the visible interleaved chroma window, as NV12 reference files do. */
static int
probe_hash(
	const struct media_picture *picture,
	char hash[65])
{
	struct command_sha256_context context;
	uint8_t digest[32];
	unsigned row;
	unsigned index;
	int error;

	/* Plane storage is linear CPU memory with its own pitch; no optimal GPU subresource layout is consulted. */
	command_sha256_init(&context);
	for (row = 0U; row < picture->height; row++) {
		error = command_sha256_update(&context, picture->luma + (size_t)row * picture->pitch, picture->width);
		if (error != 0)
			return error;
	}

	/* Both chroma samples are included for every visible two-pixel group. */
	for (row = 0U; row < (picture->height + 1U) / 2U; row++) {
		error = command_sha256_update(&context, picture->chroma + (size_t)row * picture->pitch, (picture->width + 1U) / 2U * 2U);
		if (error != 0)
			return error;
	}

	/* Publish an ordinary lowercase SHA-256 digest without retaining pixel storage. */
	command_sha256_final(&context, digest);
	for (index = 0U; index < 32U; index++)
		(void)snprintf(hash + index * 2U, 3U, "%02x", (unsigned)digest[index]);
	return 0;
}

/* Match a reference row by exact presentation time and visible NV12 digest. */
static int
probe_expect(
	FILE *expected,
	int64_t time,
	const char *hash)
{
	long long reference_time;
	char reference_hash[65];
	int fields;
	int compared;

	/* Skip only reference pictures preceding the first output after a seek. */
	for (;;) {
		fields = fscanf(expected, "%lld %64s", &reference_time, reference_hash);
		if (fields != 2)
			return EINVAL;
		if (reference_time >= time)
			break;
	}

	/* A mismatched time is a presentation error even when its pixels happen to match. */
	if (reference_time != time)
		return EIO;
	compared = strcmp(reference_hash, hash);
	if (compared != 0)
		return EIO;
	return 0;
}

/* Consume normalized stereo output using the same ordinary sound conversion API as Video Player and Music. */
static void
probe_sound(
	struct probe *probe,
	unsigned decoder)
{
	int16_t samples[2048];
	size_t frames;
	size_t index;
	int64_t left;
	int64_t right;

	/* Accumulate exactly 1024 source output frames per RMS row, carrying partial rows across AAC blocks. */
	for (;;) {
		frames = media_decoder_sound(probe->decoder[decoder], samples, 1024U, 48000U);
		if (frames == 0U)
			break;
		for (index = 0U; index < frames; index++) {
			left = samples[index * 2U];
			right = samples[index * 2U + 1U];
			probe->sound_sum[0] += (uint64_t)(left * left);
			probe->sound_sum[1] += (uint64_t)(right * right);
			probe->sound_count++;
			probe->sound_frames++;
			if (probe->sound_count == 1024U)
				probe_rms(probe);
		}
	}
}

/* Print one normalized stereo RMS block and reset only its accumulator. */
static void
probe_rms(
	struct probe *probe)
{
	double left;
	double right;

	/* Signed 16-bit conversion happens in libmedia; this diagnostic normalizes its final integer samples. */
	left = sqrt((double)probe->sound_sum[0] / probe->sound_count) / 32768.0;
	right = sqrt((double)probe->sound_sum[1] / probe->sound_count) / 32768.0;
	printf("RMS frame=%llu count=%u left=%.9f right=%.9f\n", (unsigned long long)(probe->sound_frames - probe->sound_count), probe->sound_count, left, right);
	probe->sound_sum[0] = 0U;
	probe->sound_sum[1] = 0U;
	probe->sound_count = 0U;
}

/* Read elapsed monotonic time without mixing playback timestamps with wall-clock time. */
static uint64_t
probe_now(void)
{
	struct timespec now;
	uint64_t microseconds;

	/* Timing failure affects diagnostics only and contributes a zero measurement. */
	memset(&now, 0, sizeof(now));
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	microseconds = (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
	return microseconds;
}
