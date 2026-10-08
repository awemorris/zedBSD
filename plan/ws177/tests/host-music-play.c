/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of Music's player when a song cannot go on (ws177-p021):
 * play.c is built with stand-ins for the container (a scripted run of
 * packets, then the end or a read error), the decoder (scripted packets
 * that do not decode) and the sound stream (it takes everything at once).
 * A song plays to its end; 16 packets in a row that do not decode stop it
 * (MU_FAIL_DECODE); 15 in a row, twice, do not; a read error stops it
 * (MU_FAIL_READ).
 *   host-music-play
 */

#include "play.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The script of a song: its packets, those that do not decode (first to last, -1 for none, twice), and what ends it. */
struct test_script {
	unsigned packets;
	int bad_first[2];
	int bad_last[2];
	int end;
};

/* The script played, and where the stand-ins are in it. */
static struct test_script test_song;
static unsigned test_next;
static int test_pending;
static int64_t test_time_us;
static uint64_t test_written;
static int test_failures;

static const struct media_track test_track = { .kind = MEDIA_TRACK_AUDIO, .codec_name = "aac", .sample_rate = 48000U, .channels = 2U };
static unsigned char test_byte;

static void test_check(const char *name, int passed);
static int test_wait(struct mu_player *player, int want_end, int want_failure);
static void test_pause_ms(unsigned ms);
static int test_bad(unsigned index);

/* The log of the program, on standard output. */
void
mu_log(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("MUSIC ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
}

/* The container's stand-ins. */
int
media_file_open(
	const char *path,
	struct media_file **file)
{
	(void)path;
	test_next = 0U;
	*file = (struct media_file *)&test_byte;
	return 0;
}

unsigned
media_file_track_count(
	const struct media_file *file)
{
	(void)file;
	return 1U;
}

const struct media_track *
media_file_track(
	const struct media_file *file,
	unsigned index)
{
	(void)file;
	(void)index;
	return &test_track;
}

int64_t
media_file_duration_us(
	const struct media_file *file)
{
	(void)file;
	return (int64_t)test_song.packets * 20000;
}

const char *
media_file_format_name(
	const struct media_file *file)
{
	(void)file;
	return "test";
}

int
media_file_read(
	struct media_file *file,
	struct media_packet *packet)
{
	(void)file;
	if (test_next >= test_song.packets)
		return test_song.end;
	memset(packet, 0, sizeof(*packet));
	packet->track = 0U;
	packet->pts_us = (int64_t)test_next * 20000;
	packet->data = &test_byte;
	packet->size = 1U;
	test_next++;
	return 0;
}

int
media_file_seek(
	struct media_file *file,
	int64_t time_us)
{
	(void)file;
	test_next = (unsigned)(time_us / 20000);
	return 0;
}

void
media_file_close(
	struct media_file *file)
{
	(void)file;
}

/* The decoder's stand-ins: a scripted packet does not decode; the others give a little sound each. */
int
media_decoder_open(
	const struct media_track *track,
	struct media_decoder **decoder)
{
	(void)track;
	*decoder = (struct media_decoder *)&test_byte;
	return 0;
}

const char *
media_decoder_name(
	const struct media_decoder *decoder)
{
	(void)decoder;
	return "test";
}

int
media_decoder_send(
	struct media_decoder *decoder,
	const struct media_packet *packet)
{
	(void)decoder;
	if (packet == NULL)
		return 0;
	if (test_bad((unsigned)(packet->pts_us / 20000)))
		return EINVAL;
	test_pending = 1;
	test_time_us = packet->pts_us;
	return 0;
}

int
media_decoder_receive(
	struct media_decoder *decoder,
	int64_t *time_us)
{
	(void)decoder;
	if (!test_pending)
		return 0;
	test_pending = 0;
	*time_us = test_time_us;
	return 1;
}

size_t
media_decoder_sound(
	struct media_decoder *decoder,
	int16_t *samples,
	size_t capacity,
	uint32_t rate)
{
	(void)decoder;
	(void)rate;
	if (capacity < 16U)
		return 0U;
	memset(samples, 0, 16U * 2U * sizeof(*samples));
	return 16U;
}

void
media_decoder_flush(
	struct media_decoder *decoder)
{
	(void)decoder;
	test_pending = 0;
}

void
media_decoder_close(
	struct media_decoder *decoder)
{
	(void)decoder;
}

/* The sound's stand-ins: everything written is played at once. */
int
vp_audio_open(
	struct vp_audio *audio)
{
	memset(audio, 0, sizeof(*audio));
	audio->rate = 48000U;
	audio->channels = 2U;
	audio->created = 1;
	return 0;
}

void
vp_audio_close(
	struct vp_audio *audio)
{
	audio->created = 0;
}

int
vp_audio_start(
	struct vp_audio *audio)
{
	audio->running = 1;
	return 0;
}

int
vp_audio_stop(
	struct vp_audio *audio)
{
	audio->running = 0;
	return 0;
}

int
vp_audio_flush(
	struct vp_audio *audio)
{
	(void)audio;
	return 0;
}

uint64_t
vp_audio_read_position(
	const struct vp_audio *audio)
{
	(void)audio;
	return __atomic_load_n(&test_written, __ATOMIC_ACQUIRE);
}

uint64_t
vp_audio_write_position(
	const struct vp_audio *audio)
{
	(void)audio;
	return __atomic_load_n(&test_written, __ATOMIC_ACQUIRE);
}

uint64_t
vp_audio_clock_position(
	const struct vp_audio *audio)
{
	(void)audio;
	return __atomic_load_n(&test_written, __ATOMIC_ACQUIRE);
}

void
vp_audio_renew(
	struct vp_audio *audio)
{
	if (!audio->created)
		(void)vp_audio_open(audio);
}

size_t
vp_audio_write(
	struct vp_audio *audio,
	const int16_t *samples,
	size_t frames)
{
	(void)audio;
	(void)samples;
	__atomic_add_fetch(&test_written, frames, __ATOMIC_ACQ_REL);
	return frames;
}

/* Plays the scripts. */
int
main(void)
{
	static struct mu_player player;
	int status;

	/* The player. */
	status = mu_player_init(&player);
	test_check("init", status == 0);

	/* A song that plays to its end. */
	memset(&test_song, 0, sizeof(test_song));
	test_song.packets = 40U;
	test_song.bad_first[0] = -1;
	test_song.bad_first[1] = -1;
	test_song.end = ENODATA;
	status = mu_player_open(&player, "good.m4a");
	test_check("good-open", status == 0);
	test_check("good-ended", test_wait(&player, 1, 0) == 1);

	/* Sixteen packets in a row that do not decode: stopped, MU_FAIL_DECODE. */
	test_song.bad_first[0] = 5;
	test_song.bad_last[0] = 20;
	status = mu_player_open(&player, "bad.m4a");
	test_check("decode-open", status == 0);
	test_check("decode-failed", test_wait(&player, 0, MU_FAIL_DECODE) == 1);
	test_check("decode-stopped", player.state == MU_STOPPED);
	test_check("decode-told-once", mu_player_failure(&player) == 0);

	/* Fifteen in a row, twice, with one between: no failure, the end. */
	test_song.bad_first[0] = 2;
	test_song.bad_last[0] = 16;
	test_song.bad_first[1] = 18;
	test_song.bad_last[1] = 32;
	status = mu_player_open(&player, "rough.m4a");
	test_check("rough-open", status == 0);
	test_check("rough-ended", test_wait(&player, 1, 0) == 1);

	/* A read error: stopped, MU_FAIL_READ. */
	test_song.bad_first[0] = -1;
	test_song.bad_first[1] = -1;
	test_song.packets = 10U;
	test_song.end = EIO;
	status = mu_player_open(&player, "unreadable.m4a");
	test_check("read-open", status == 0);
	test_check("read-failed", test_wait(&player, 0, MU_FAIL_READ) == 1);

	/* A failure is gone with the next song. */
	test_song.end = ENODATA;
	status = mu_player_open(&player, "again.m4a");
	test_check("again", status == 0 && mu_player_failure(&player) == 0);

	/* The end. */
	mu_player_release(&player);
	if (test_failures != 0) {
		printf("host-music-play: FAIL %d\n", test_failures);
		return 1;
	}
	printf("host-music-play: PASS\n");
	return 0;
}

/* Reports one check. */
static void
test_check(
	const char *name,
	int passed)
{
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}
	printf("FAIL %s\n", name);
	test_failures++;
}

/* Waits up to 2 s for the song's end (want_end) or a failure (want_failure, which must be the one); 1 when it came. */
static int
test_wait(
	struct mu_player *player,
	int want_end,
	int want_failure)
{
	unsigned waited;
	unsigned state;
	int ended;
	int failure;

	for (waited = 0U; waited < 2000U; waited += 10U) {
		state = mu_player_state(player, &ended);
		(void)state;
		if (want_end && ended)
			return 1;
		failure = mu_player_failure(player);
		if (failure != 0)
			return failure == want_failure;
		test_pause_ms(10U);
	}
	return 0;
}

/* Sleeps a number of milliseconds. */
static void
test_pause_ms(
	unsigned ms)
{
	struct timespec wait;

	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}

/* Tells whether a packet of the script does not decode. */
static int
test_bad(
	unsigned index)
{
	unsigned run;

	for (run = 0U; run < 2U; run++) {
		if (test_song.bad_first[run] >= 0 && (int)index >= test_song.bad_first[run] && (int)index <= test_song.bad_last[run])
			return 1;
	}
	return 0;
}
