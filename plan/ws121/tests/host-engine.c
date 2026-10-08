/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws121-p002: the host test of libmedia's engine (userland/desktop/libmedia/engine.c)
 * with the container reader and the decoding add-in against the host's FFmpeg (dlopen).
 *
 *     host-engine VIDEO SONG
 *
 * VIDEO (WS122's sample.mp4: MPEG-4 Part 2 320x240 and AAC, 20 s) is
 * opened through a source whose reader is pread, the engine's wake is
 * polled until it is open, it plays a second (pictures taken as the
 * caller would), seeks to 5 s, pauses; SONG (AAC alone) plays silent by
 * the monotonic clock to its end (no audiod on the host).  Prints "PASS
 * name" or "FAIL name ..." for each check; exits with 1 when one failed.
 */

#include "userland/desktop/libmedia/media.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The checks that failed. */
static int failures;

int main(int argc, char **argv);
static void check(const char *name, int passed, const char *detail);
static int wait_state(struct media_engine *engine, unsigned state, int seconds, struct media_status *status);
static int play_for(struct media_engine *engine, int milliseconds, uint32_t *pixels, unsigned *pictures);
static int read_at(void *context, uint64_t offset, void *data, size_t size);
static void log_line(void *context, const char *line);

/* Plays the two files and checks what the engine does. */
int
main(
	int argc,
	char **argv)
{
	static uint32_t pixels[160 * 120];
	struct media_engine *engine;
	struct media_status status;
	struct media_status later;
	struct media_source source;
	struct stat file_status;
	unsigned pictures;
	char detail[160];
	int reached;
	int fd;
	int error;

	/* The files. */
	if (argc != 3) {
		fprintf(stderr, "usage: host-engine VIDEO SONG\n");
		return 2;
	}

	/* The library's lines, printed. */
	media_set_log(log_line, NULL);

	/* The video through a source. */
	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		return 2;
	error = fstat(fd, &file_status);
	if (error != 0)
		return 2;
	source.read_at = read_at;
	source.size = (uint64_t)file_status.st_size;
	source.context = &fd;
	error = media_engine_open(NULL, &source, MEDIA_SOUND, &engine);
	check("open", error == 0, "media_engine_open");
	if (error != 0)
		return 1;

	/* Open, paused at its start: its size and length. */
	reached = wait_state(engine, MEDIA_PAUSED, 10, &status);
	(void)snprintf(detail, sizeof(detail), "state=%u error=%d %dx%d video=%d duration=%.2f", status.state, status.error,
	    status.width, status.height, status.has_video, status.duration);
	check("opened", reached && status.width == 320 && status.height == 240 && status.has_video && status.duration > 19.0, detail);

	/* The first picture at once, before playing. */
	pictures = 0;
	(void)play_for(engine, 200, pixels, &pictures);
	(void)snprintf(detail, sizeof(detail), "pictures=%u pixel=%08x", pictures, (unsigned)pixels[60 * 160 + 80]);
	check("first-picture", pictures == 1U && pixels[60 * 160 + 80] != 0U, detail);

	/* A second of playing: pictures come, the clock moves about a second. */
	media_engine_play(engine);
	pictures = 0;
	(void)play_for(engine, 1000, pixels, &pictures);
	media_engine_status(engine, &status);
	(void)snprintf(detail, sizeof(detail), "pictures=%u position=%.3f state=%u", pictures, status.position, status.state);
	check("play", status.state == MEDIA_PLAYING && pictures >= 15U && status.position > 0.8 && status.position < 1.5, detail);

	/* A seek to 5 s: the clock stands there until the thread goes on, then moves from there. */
	media_engine_seek(engine, 5.0);
	pictures = 0;
	(void)play_for(engine, 500, pixels, &pictures);
	media_engine_status(engine, &status);
	(void)snprintf(detail, sizeof(detail), "pictures=%u position=%.3f", pictures, status.position);
	check("seek", pictures >= 5U && status.position >= 5.0 && status.position < 6.0, detail);

	/* Paused: the clock stands. */
	media_engine_pause(engine);
	media_engine_status(engine, &status);
	(void)play_for(engine, 300, pixels, &pictures);
	media_engine_status(engine, &later);
	(void)snprintf(detail, sizeof(detail), "%.3f then %.3f", status.position, later.position);
	check("pause", later.state == MEDIA_PAUSED && later.position == status.position, detail);

	/* The video's engine goes. */
	media_engine_close(engine);
	(void)close(fd);

	/* The song by its path, silent: it plays by the monotonic clock to its end. */
	error = media_engine_open(argv[2], NULL, MEDIA_SOUND, &engine);
	check("song-open", error == 0, "media_engine_open");
	if (error != 0)
		return 1;
	reached = wait_state(engine, MEDIA_PAUSED, 10, &status);
	(void)snprintf(detail, sizeof(detail), "state=%u error=%d problem=%d video=%d audio=%d duration=%.2f", status.state, status.error,
	    status.problem, status.has_video, status.has_audio, status.duration);
	check("song-opened", reached && !status.has_video && status.duration > 1.5 && status.duration < 2.5, detail);
	media_engine_play(engine);
	reached = wait_state(engine, MEDIA_ENDED, 5, &status);
	(void)snprintf(detail, sizeof(detail), "state=%u position=%.3f", status.state, status.position);
	check("song-ended", reached && status.position > 1.5, detail);
	media_engine_close(engine);

	/* A file that is not media fails. */
	error = media_engine_open("/etc/hostname", NULL, 0U, &engine);
	reached = error == 0 && wait_state(engine, MEDIA_FAILED, 5, &status);
	check("not-media", reached && status.error != 0, "MEDIA_FAILED");
	if (error == 0)
		media_engine_close(engine);

	/* The end. */
	if (failures != 0) {
		printf("host-engine: FAIL %d\n", failures);
		return 1;
	}

	/* Every check passed. */
	printf("host-engine: PASS\n");
	return 0;
}

/* Reports one check. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS or FAIL with what was seen. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* A failure. */
	printf("FAIL %s: %s\n", name, detail);
	failures++;
}

/* Polls the engine's wake until it reaches a state, for at most a time; 1 when it did. */
static int
wait_state(
	struct media_engine *engine,
	unsigned state,
	int seconds,
	struct media_status *status)
{
	struct pollfd poller;
	int rounds;

	/* Each wake, or a tenth of a second. */
	for (rounds = 0; rounds < seconds * 10; rounds++) {
		media_engine_status(engine, status);
		if (status->state == state)
			return 1;
		poller.fd = media_engine_wake_fd(engine);
		poller.events = POLLIN;
		(void)poll(&poller, 1, 100);
	}

	/* The last look. */
	media_engine_status(engine, status);
	return status->state == state;
}

/* Takes the pictures as a caller would for a while, counting those drawn. */
static int
play_for(
	struct media_engine *engine,
	int milliseconds,
	uint32_t *pixels,
	unsigned *pictures)
{
	struct pollfd poller;
	struct media_status status;
	double next;
	int drawn;
	int waited;
	int wait;

	/* Until the time is up. */
	waited = 0;
	while (waited < milliseconds) {
		drawn = media_engine_picture(engine, pixels, 160, 160, 120, &next);
		if (drawn)
			(*pictures)++;
		wait = 10;
		if (next > 0.0 && next < 0.01)
			wait = (int)(next * 1000.0) + 1;
		poller.fd = media_engine_wake_fd(engine);
		poller.events = POLLIN;
		(void)poll(&poller, 1, wait);
		media_engine_status(engine, &status);
		waited += wait;
	}

	/* The time is up. */
	return 0;
}

/* The source's reader: pread of all the bytes. */
static int
read_at(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	ssize_t got;

	/* One read. */
	got = pread(*(int *)context, data, size, (off_t)offset);
	if (got != (ssize_t)size)
		return EIO;
	return 0;
}

/* The library's log lines, printed. */
static void
log_line(
	void *context,
	const char *line)
{
	/* Printed as they come. */
	(void)context;
	printf("LOG %s\n", line);
}
