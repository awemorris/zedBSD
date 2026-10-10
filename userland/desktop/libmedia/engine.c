/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The engine (ws121-p002; media.h): one media file played by a thread of
 * its own.  The thread opens the file (through the caller's source when
 * there is one), opens a decoder for its first video track, then reads and
 * decodes ahead: the pictures wait in a small ring for the caller to take
 * them at their time.  The engine plays no sound (WS191, design D11: the
 * library is linked into libbrowser, which may not reach Keiland's sound
 * streams; its users get a sound output of their own later), so a file of
 * sound alone plays silent, and the clock is the monotonic clock.  A seek
 * drops what is decoded and queued, and starts the clock again at the time
 * sought.
 *
 * Built like Video Player's player (userland/desktop/videoplayer/media.c),
 * with the opening moved to the thread and the pictures made optional.
 */

#include "media.h"
#include "media-private.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How many decoded pictures wait at most. */
#define ENGINE_PICTURES		4U

/* The longest a full ring of sound or pictures is waited on before looking at the requests again (ms). */
#define ENGINE_WAIT_MS		10

/* The largest picture an engine shows (pixels a side). */
#define ENGINE_SIDE_MAX		4096

/*
 * An engine.
 *
 * Set at the open and then read only: the path or the source, the flags,
 * and the descriptor pair of the wake (the thread writes, the caller
 * polls the other end).
 *
 * The thread's own: the file, the decoders and their tracks (no sound's
 * is opened), and the time before which what decodes is passed over
 * (after a seek).
 *
 * The caller's own: the picture shown, its scaler, and the size it was
 * last scaled to.
 *
 * Under the lock: the state and why it failed, what is known of the file,
 * the pictures waiting (a ring of references with their times), the end
 * of the file reached, the clock's anchors, the requests (a seek, the end
 * of the thread), and the number of the latest picture taken.
 */
struct media_engine {
	pthread_t thread;
	int thread_started;
	char *path;
	struct media_source source;
	unsigned flags;
	int wake[2];

	struct media_file *file;
	struct media_decoder *video;
	struct media_decoder *sound;
	unsigned video_track;
	unsigned sound_track;
	double skip_before;

	struct media_frame *shown;
	struct media_scaler *scaler;
	int scaled_width;
	int scaled_height;

	pthread_mutex_t lock;
	pthread_cond_t ready;
	unsigned state;
	int error;
	int problem;
	int width;
	int height;
	double duration;
	int has_audio;
	struct media_frame *pictures[ENGINE_PICTURES];
	double picture_times[ENGINE_PICTURES];
	unsigned picture_first;
	unsigned picture_count;
	int eof;
	double clock_time;
	uint64_t clock_us;
	int seek_wanted;
	double seek_to;
	int quit;
	uint64_t taken;
};

/* The library's log: the caller's function and its context (media_set_log). */
static void (*engine_log_function)(void *context, const char *line);
static void *engine_log_context;

static void *engine_run(void *argument);
static int engine_open_file(struct media_engine *engine);
static int engine_decoder(struct media_engine *engine, unsigned kind, struct media_decoder **result, unsigned *track);
static int engine_feed(struct media_engine *engine, struct media_decoder *decoder, const struct media_packet *packet);
static int engine_drain(struct media_engine *engine, struct media_decoder *decoder);
static int engine_picture(struct media_engine *engine, struct media_frame *picture, double time);
static int engine_seek(struct media_engine *engine);
static void engine_end(struct media_engine *engine);
static void engine_fail(struct media_engine *engine, int error);
static double engine_clock(struct media_engine *engine);
static void engine_drop_pictures(struct media_engine *engine);
static int engine_stopping(struct media_engine *engine);
static void engine_wake(struct media_engine *engine);
static uint64_t engine_now_us(void);
static void engine_sleep_ms(unsigned ms);

/*
 * Sets where the library's log lines go (NULL: nowhere).
 */
void
media_set_log(
	void (*log)(void *context, const char *line),
	void *context)
{
	/* The function and its context. */
	engine_log_function = log;
	engine_log_context = context;
}

/*
 * Writes a log line of the library's (the engine's, the decoders' and the
 * readers') to the caller's function.
 */
void
media_log(
	const char *format,
	...)
{
	va_list arguments;
	char line[512];

	/* Nowhere to go. */
	if (engine_log_function == NULL)
		return;

	/* The line, to the caller. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	engine_log_function(engine_log_context, line);
}

/*
 * Starts playing a file, paused at its start: from its path, or (path
 * NULL) from the caller's source, whose reader the engine's thread calls.
 * Returns 0 with the engine, which is opening (MEDIA_OPENING) until the
 * wake says otherwise, or an errno value.  flags: MEDIA_SOUND.
 */
int
media_engine_open(
	const char *path,
	const struct media_source *source,
	unsigned flags,
	struct media_engine **engine)
{
	struct media_engine *made;
	int status;
	int error;

	/* The engine, nothing open yet. */
	*engine = NULL;
	if (path == NULL && (source == NULL || source->read_at == NULL))
		return EINVAL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->wake[0] = -1;
	made->wake[1] = -1;
	made->flags = flags;
	made->state = MEDIA_OPENING;
	(void)pthread_mutex_init(&made->lock, NULL);
	(void)pthread_cond_init(&made->ready, NULL);

	/* The path, or the source. */
	if (path != NULL) {
		made->path = strdup(path);
		if (made->path == NULL) {
			media_engine_close(made);
			return ENOMEM;
		}
	} else {
		made->source = *source;
	}

	/* The wake: a pipe whose ends do not block. */
	status = pipe(made->wake);
	if (status != 0) {
		error = errno;
		made->wake[0] = -1;
		made->wake[1] = -1;
		media_engine_close(made);
		return error;
	}

	/* Neither end blocks, nor goes to a program the caller starts. */
	(void)fcntl(made->wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(made->wake[1], F_SETFL, O_NONBLOCK);
	(void)fcntl(made->wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(made->wake[1], F_SETFD, FD_CLOEXEC);

	/* The thread, which opens the file. */
	error = pthread_create(&made->thread, NULL, engine_run, made);
	if (error != 0) {
		media_engine_close(made);
		return error;
	}

	/* Succeeded: opening. */
	made->thread_started = 1;
	*engine = made;
	return 0;
}

/*
 * Reports the descriptor that is readable when the engine's state changed
 * or a picture came (it is emptied by media_engine_status).
 */
int
media_engine_wake_fd(
	const struct media_engine *engine)
{
	/* The pipe's reading end. */
	return engine->wake[0];
}

/*
 * Reads the engine's state, and empties its wake.
 */
void
media_engine_status(
	struct media_engine *engine,
	struct media_status *status)
{
	char bytes[64];
	ssize_t got;

	/* The wake emptied. */
	for (;;) {
		got = read(engine->wake[0], bytes, sizeof(bytes));
		if (got <= 0)
			break;
	}

	/* The state, under the lock. */
	memset(status, 0, sizeof(*status));
	(void)pthread_mutex_lock(&engine->lock);
	status->state = engine->state;
	status->error = engine->error;
	status->problem = engine->problem;
	status->width = engine->width;
	status->height = engine->height;
	status->has_video = engine->video != NULL;
	status->has_audio = engine->has_audio;
	status->duration = engine->duration;
	status->position = engine_clock(engine);
	status->picture = engine->taken;
	(void)pthread_mutex_unlock(&engine->lock);
}

/*
 * Plays from where it is (from the start again after the end).
 */
void
media_engine_play(
	struct media_engine *engine)
{
	unsigned state;

	/* Only an open file. */
	(void)pthread_mutex_lock(&engine->lock);
	state = engine->state;
	(void)pthread_mutex_unlock(&engine->lock);
	if (state == MEDIA_ENDED)
		media_engine_seek(engine, 0.0);
	if (state != MEDIA_PAUSED && state != MEDIA_ENDED)
		return;

	/* The clock goes on from where it stood. */
	(void)pthread_mutex_lock(&engine->lock);
	engine->clock_time = engine_clock(engine);
	engine->clock_us = engine_now_us();
	engine->state = MEDIA_PLAYING;
	(void)pthread_cond_broadcast(&engine->ready);
	(void)pthread_mutex_unlock(&engine->lock);
	media_log("MEDIA play position_ms=%lld", (long long)(engine->clock_time * 1000.0));
	engine_wake(engine);
}

/*
 * Pauses where it is.
 */
void
media_engine_pause(
	struct media_engine *engine)
{
	double now;

	/* Only while playing. */
	(void)pthread_mutex_lock(&engine->lock);
	if (engine->state != MEDIA_PLAYING) {
		(void)pthread_mutex_unlock(&engine->lock);
		return;
	}

	/* The clock stands where it is. */
	now = engine_clock(engine);
	engine->state = MEDIA_PAUSED;
	engine->clock_time = now;
	(void)pthread_mutex_unlock(&engine->lock);
	media_log("MEDIA pause position_ms=%lld", (long long)(now * 1000.0));
	engine_wake(engine);
}

/*
 * Asks for a seek to a time (seconds, kept within the file).
 */
void
media_engine_seek(
	struct media_engine *engine,
	double seconds)
{
	/* Within the file. */
	if (seconds < 0.0)
		seconds = 0.0;

	/* The request; the clock stands at the time sought until the thread starts it again. */
	(void)pthread_mutex_lock(&engine->lock);
	if (engine->duration > 0.0 && seconds > engine->duration)
		seconds = engine->duration;
	engine->seek_wanted = 1;
	engine->seek_to = seconds;
	engine->clock_time = seconds;
	engine->clock_us = engine_now_us();
	if (engine->state == MEDIA_ENDED)
		engine->state = MEDIA_PAUSED;
	engine_drop_pictures(engine);
	(void)pthread_cond_broadcast(&engine->ready);
	(void)pthread_mutex_unlock(&engine->lock);
}

/*
 * Takes the latest picture whose time has come and scales it into the
 * caller's pixels (0xAARRGGBB, opaque; stride pixels from a row to the
 * next) of a size; the picture taken
 * before is scaled again when the size changed.  Returns 1 when the
 * pixels were drawn, 0 when they still hold the latest picture (or there
 * is none).  next is the time in seconds until the next picture is due,
 * or -1 when none waits.
 */
int
media_engine_picture(
	struct media_engine *engine,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height,
	double *next)
{
	struct media_frame *taken;
	double clock;
	unsigned slot;
	int status;
	int drawn;

	/* The latest due: it replaces the ones before it. */
	taken = NULL;
	*next = -1.0;
	(void)pthread_mutex_lock(&engine->lock);
	clock = engine_clock(engine);
	while (engine->picture_count > 0U) {
		/* Not due yet: when the next is. */
		slot = engine->picture_first;
		if (engine->picture_times[slot] > clock && !(engine->shown == NULL && taken == NULL)) {
			*next = engine->picture_times[slot] - clock;
			break;
		}

		/* Due (or the first of all, shown at once). */
		media_frame_free(&taken);
		taken = engine->pictures[slot];
		engine->pictures[slot] = NULL;
		engine->picture_first = (slot + 1U) % ENGINE_PICTURES;
		engine->picture_count--;
	}

	/* A new one is counted; the thread may decode on. */
	if (taken != NULL)
		engine->taken++;
	(void)pthread_cond_broadcast(&engine->ready);
	(void)pthread_mutex_unlock(&engine->lock);

	/* A new picture is kept for drawing again. */
	if (taken != NULL) {
		media_frame_free(&engine->shown);
		engine->shown = taken;
	}

	/* Nothing new and the same size: the pixels hold it already. */
	if (engine->shown == NULL || width <= 0 || height <= 0)
		return 0;
	if (taken == NULL && width == engine->scaled_width && height == engine->scaled_height)
		return 0;

	/* Scaled into the pixels. */
	status = media_frame_scale(engine->shown, &engine->scaler, pixels, stride * sizeof(pixels[0]), width, height);
	drawn = 0;
	if (status == 0) {
		engine->scaled_width = width;
		engine->scaled_height = height;
		drawn = 1;
	}

	/* Whether the pixels were drawn. */
	return drawn;
}

/*
 * Scales the latest picture taken into the caller's pixels again (after
 * the caller drew over them).  Returns 1 when drawn, 0 when there is no
 * picture yet.
 */
int
media_engine_redraw(
	struct media_engine *engine,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	int status;

	/* No picture yet. */
	if (engine->shown == NULL || width <= 0 || height <= 0)
		return 0;

	/* Scaled into the pixels. */
	status = media_frame_scale(engine->shown, &engine->scaler, pixels, stride * sizeof(pixels[0]), width, height);
	if (status != 0)
		return 0;
	engine->scaled_width = width;
	engine->scaled_height = height;
	return 1;
}

/*
 * Ends the engine and frees it: the thread is told to end and waited for.
 */
void
media_engine_close(
	struct media_engine *engine)
{
	/* Nothing to close. */
	if (engine == NULL)
		return;

	/* The thread. */
	if (engine->thread_started) {
		(void)pthread_mutex_lock(&engine->lock);
		engine->quit = 1;
		(void)pthread_cond_broadcast(&engine->ready);
		(void)pthread_mutex_unlock(&engine->lock);
		(void)pthread_join(engine->thread, NULL);
	}

	/* What the caller held, and the pictures waiting. */
	media_frame_free(&engine->shown);
	media_scaler_free(engine->scaler);
	engine_drop_pictures(engine);

	/* The wake, the lock, the path, the engine. */
	if (engine->wake[0] >= 0)
		(void)close(engine->wake[0]);
	if (engine->wake[1] >= 0)
		(void)close(engine->wake[1]);
	(void)pthread_cond_destroy(&engine->ready);
	(void)pthread_mutex_destroy(&engine->lock);
	free(engine->path);
	free(engine);
}

/* The thread: opens the file, then reads, decodes and queues until it is told to end. */
static void *
engine_run(
	void *argument)
{
	struct media_engine *engine;
	struct media_packet packet;
	int sought;
	int status;
	int read;
	int stop;
	int fed;

	/* The file and its decoder. */
	engine = argument;
	status = engine_open_file(engine);
	(void)pthread_mutex_lock(&engine->lock);
	engine->error = status;
	engine->state = MEDIA_PAUSED;
	if (status != 0)
		engine->state = MEDIA_FAILED;
	engine->clock_us = engine_now_us();
	(void)pthread_mutex_unlock(&engine->lock);
	engine_wake(engine);

	/* Each round until told to end (a file that failed waits for that). */
	for (;;) {
		/* The end, or a seek. */
		stop = engine_stopping(engine);
		if (stop)
			break;
		if (status != 0) {
			engine_sleep_ms(ENGINE_WAIT_MS * 5);
			continue;
		}

		/* A seek asked for. */
		sought = engine_seek(engine);
		if (sought)
			continue;

		/* A decode failure waits for a seek without treating an interrupted feed as failure. */
		(void)pthread_mutex_lock(&engine->lock);
		stop = 0;
		if (engine->state == MEDIA_FAILED)
			stop = 1;
		(void)pthread_mutex_unlock(&engine->lock);
		if (stop) {
			engine_sleep_ms(ENGINE_WAIT_MS * 5);
			continue;
		}

		/* The next packet; the end of the file drains the decoders and waits. */
		read = media_file_read(engine->file, &packet);
		if (read == ENODATA) {
			engine_end(engine);
			continue;
		}

		/* Publish a read failure without successful end-of-stream semantics. */
		if (read != 0) {
			engine_fail(engine, read);
			continue;
		}

		/* Its decoder. */
		fed = 0;
		if (engine->video != NULL && packet.track == engine->video_track)
			fed = engine_feed(engine, engine->video, &packet);
		else if (engine->sound != NULL && packet.track == engine->sound_track)
			fed = engine_feed(engine, engine->sound, &packet);
		if (fed < 0)
			engine_fail(engine, -fed);
	}

	/* What the thread opened goes with it. */
	media_decoder_close(engine->video);
	engine->video = NULL;
	media_decoder_close(engine->sound);
	engine->sound = NULL;
	if (engine->file != NULL)
		media_file_close(engine->file);
	engine->file = NULL;
	return NULL;
}

/*
 * Opens the file and the decoder of its first video track.  A file needs
 * pictures or sound the add-in decodes (the sound is not played).  Returns 0, or an errno value (engine->problem
 * says a decoding problem, MEDIA_PROBLEM_*).
 */
static int
engine_open_file(
	struct media_engine *engine)
{
	const struct media_track *track;
	int64_t length_us;
	struct media_decoder *probe;
	unsigned probe_track;
	int video_status;
	int sound_status;
	int silent;
	int status;

	/* The container. */
	if (engine->path != NULL)
		status = media_file_open(engine->path, &engine->file);
	else
		status = media_file_open_source(&engine->source, &engine->file);
	if (status != 0) {
		media_log("MEDIA open error=%d", status);
		return status;
	}

	/* The pictures, when there are some. */
	video_status = engine_decoder(engine, MEDIA_TRACK_VIDEO, &engine->video, &engine->video_track);

	/*
	 * No sound is played (WS191, design D11), MEDIA_SOUND or not: a file of
	 * sound alone still plays, silent, by the monotonic clock; its sound is
	 * only checked to be one the add-in decodes.
	 */
	sound_status = ENOENT;
	silent = 0;
	if (engine->video == NULL && engine->sound == NULL) {
		sound_status = engine_decoder(engine, MEDIA_TRACK_AUDIO, &probe, &probe_track);
		if (sound_status == 0) {
			media_decoder_close(probe);
			silent = 1;
		}
	}

	/* Neither: why. */
	if (engine->video == NULL && engine->sound == NULL && !silent) {
		engine->problem = video_status;
		if (video_status == ENOENT)
			engine->problem = sound_status;
		if (engine->problem == ENOENT || engine->problem == ENOMEM || engine->problem == ENODEV)
			engine->problem = 0;
		media_log("MEDIA open error=%d problem=%d", ENOTSUP, engine->problem);
		return ENOTSUP;
	}

	/* What is known of the file. */
	(void)pthread_mutex_lock(&engine->lock);
	if (engine->video != NULL) {
		track = media_file_track(engine->file, engine->video_track);
		engine->width = (int)track->width;
		engine->height = (int)track->height;
		if (engine->width > ENGINE_SIDE_MAX || engine->height > ENGINE_SIDE_MAX) {
			(void)pthread_mutex_unlock(&engine->lock);
			return EFBIG;
		}
	}

	/* Its length, and whether it sounds. */
	length_us = media_file_duration_us(engine->file);
	if (length_us > 0)
		engine->duration = (double)length_us / 1000000.0;
	engine->has_audio = engine->sound != NULL;
	(void)pthread_mutex_unlock(&engine->lock);

	/* Succeeded: the log line the tests read. */
	media_log("MEDIA open width=%d height=%d duration_ms=%lld video=%s audio=%s container=%s", engine->width, engine->height,
	    (long long)(length_us / 1000), media_decoder_name(engine->video), media_decoder_name(engine->sound), media_file_format_name(engine->file));
	return 0;
}

/*
 * Opens the decoder of the file's first track of a kind that has one.
 * Returns 0, the last MEDIA_PROBLEM_* problem, or ENOENT when there is no track
 * of the kind.
 */
static int
engine_decoder(
	struct media_engine *engine,
	unsigned kind,
	struct media_decoder **result,
	unsigned *track)
{
	const struct media_track *found;
	unsigned count;
	unsigned index;
	int problem;
	int status;

	/* Each track of the kind, in the file's order. */
	problem = ENOENT;
	count = media_file_track_count(engine->file);
	for (index = 0; index < count; index++) {
		/* A track of the kind. */
		found = media_file_track(engine->file, index);
		if (found == NULL || found->kind != kind)
			continue;

		/* Its decoder. */
		status = media_decoder_open(found, result);
		if (status == 0) {
			*track = index;
			return 0;
		}

		/* The add-in missing ends the search; another codec may still have a decoder. */
		problem = status;
		if (status == MEDIA_PROBLEM_MISSING || status == MEDIA_PROBLEM_VERSION)
			break;
	}

	/* None. */
	return problem;
}

/* Gives a decoder a packet (NULL drains it) and handles what comes out; nonzero when a seek or the end interrupted. */
static int
engine_feed(
	struct media_engine *engine,
	struct media_decoder *decoder,
	const struct media_packet *packet)
{
	int status;
	int tries;

	/* Sent; a full decoder gives its pictures or sound first, then takes the packet. */
	for (tries = 0; tries < 2; tries++) {
		status = media_decoder_send(decoder, packet);
		if (status != EAGAIN)
			break;
		status = engine_drain(engine, decoder);
		if (status != 0)
			return status;
	}

	/* Preserve a failed send as a negative runtime error for the reading thread. */
	if (status != 0)
		return -status;

	/* What comes out of it. */
	status = engine_drain(engine, decoder);
	return status;
}

/* Handles every picture or sound a decoder has ready; nonzero when a seek or the end interrupted. */
static int
engine_drain(
	struct media_engine *engine,
	struct media_decoder *decoder)
{
	struct media_frame *picture;
	int64_t time_us;
	int received;
	int status;

	/* Each one. */
	for (;;) {
		received = media_decoder_receive(decoder, &time_us);
		if (received < 0)
			return received;
		if (received == 0)
			return 0;
		if (decoder != engine->video)
			continue;

		/* A picture, queued at its time. */
		picture = media_decoder_picture(decoder);
		if (picture == NULL)
			return -ENOMEM;
		status = engine_picture(engine, picture, (double)time_us / 1000000.0);

		/* A seek or the end stops the drain. */
		if (status != 0)
			return status;
	}
}

/* Queues a picture (the reference is the ring's or freed), waiting for room; nonzero when a seek or the end came meanwhile. */
static int
engine_picture(
	struct media_engine *engine,
	struct media_frame *picture,
	double time)
{
	struct timespec until;
	unsigned slot;

	/* A picture before the time sought is passed over. */
	if (time < engine->skip_before) {
		media_frame_free(&picture);
		return 0;
	}

	/* Room in the ring, unless a seek or the end comes first. */
	(void)pthread_mutex_lock(&engine->lock);
	while (engine->picture_count == ENGINE_PICTURES && !engine->quit && !engine->seek_wanted) {
		(void)clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += ENGINE_WAIT_MS * 1000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}

		/* Woken by a picture taken or a request, or after the wait. */
		(void)pthread_cond_timedwait(&engine->ready, &engine->lock, &until);
	}

	/* Interrupted. */
	if (engine->quit || engine->seek_wanted) {
		(void)pthread_mutex_unlock(&engine->lock);
		media_frame_free(&picture);
		return 1;
	}

	/* Queued; the caller hears of it. */
	slot = (engine->picture_first + engine->picture_count) % ENGINE_PICTURES;
	engine->pictures[slot] = picture;
	engine->picture_times[slot] = time;
	engine->picture_count++;
	(void)pthread_mutex_unlock(&engine->lock);
	engine_wake(engine);
	return 0;
}

/*
 * Carries out a seek asked for: the file at the time sought, the decoders
 * and the sound emptied, the clock anchored there.  Returns 1 when one
 * was carried out.
 */
static int
engine_seek(
	struct media_engine *engine)
{
	double seconds;
	int wanted;
	int error;

	/* A seek asked for. */
	(void)pthread_mutex_lock(&engine->lock);
	wanted = engine->seek_wanted;
	seconds = engine->seek_to;
	(void)pthread_mutex_unlock(&engine->lock);
	if (!wanted)
		return 0;

	/* The file at the key frame before it, the decoders and the sound emptied. */
	error = media_file_seek(engine->file, (int64_t)(seconds * 1000000.0));
	if (error != 0) {
		(void)pthread_mutex_lock(&engine->lock);
		engine->seek_wanted = 0;
		(void)pthread_mutex_unlock(&engine->lock);
		engine_fail(engine, error);
		return 1;
	}

	/* Flush native decoders only after the container seek succeeded. */
	if (engine->video != NULL)
		media_decoder_flush(engine->video);
	if (engine->sound != NULL)
		media_decoder_flush(engine->sound);

	/* What decodes before the time sought is passed over. */
	engine->skip_before = seconds;

	/* The clock anchored at the time sought; the pictures dropped again (some may have come meanwhile). */
	(void)pthread_mutex_lock(&engine->lock);
	engine_drop_pictures(engine);
	engine->clock_time = seconds;
	engine->clock_us = engine_now_us();
	engine->eof = 0;
	engine->error = 0;
	if (engine->state == MEDIA_FAILED)
		engine->state = MEDIA_PAUSED;
	engine->seek_wanted = 0;
	(void)pthread_mutex_unlock(&engine->lock);
	media_log("MEDIA seek to_ms=%lld", (long long)(seconds * 1000.0));
	engine_wake(engine);

	/* Carried out. */
	return 1;
}

/*
 * At the end of the file: drains the decoders once, then, while playing,
 * says the file ended when its last picture was taken and its sound
 * played out (without sound, when the clock reached its length).
 */
static void
engine_end(
	struct media_engine *engine)
{
	double clock;
	int drained;
	int ended;
	int status;

	/* The decoders' last pictures and sound, once. */
	(void)pthread_mutex_lock(&engine->lock);
	drained = engine->eof;
	(void)pthread_mutex_unlock(&engine->lock);
	if (!drained) {
		status = 0;
		if (engine->video != NULL)
			status = engine_feed(engine, engine->video, NULL);
		if (status == 0 && engine->sound != NULL)
			status = engine_feed(engine, engine->sound, NULL);
		if (status < 0)
			engine_fail(engine, -status);
		if (status != 0)
			return;
		(void)pthread_mutex_lock(&engine->lock);
		engine->eof = 1;
		(void)pthread_mutex_unlock(&engine->lock);
	}

	/* Played out: no picture waits, and the clock reached the length. */
	ended = 0;
	(void)pthread_mutex_lock(&engine->lock);
	clock = engine_clock(engine);
	if (engine->state == MEDIA_PLAYING && engine->picture_count == 0U &&
	    (engine->duration <= 0.0 || clock >= engine->duration - 0.05)) {
		engine->clock_time = clock;
		engine->state = MEDIA_ENDED;
		ended = 1;
	}

	/* The state is let go. */
	(void)pthread_mutex_unlock(&engine->lock);

	/* The end told once. */
	if (ended) {
		media_log("MEDIA ended position_ms=%lld dropped=%llu", (long long)(clock * 1000.0),
		    (unsigned long long)media_file_dropped(engine->file));
		engine_wake(engine);
	}

	/* A little wait before the next look. */
	engine_sleep_ms(ENGINE_WAIT_MS * 5);
}

/* Publishes a failed read or decode and leaves the reader available for a later seek. */
static void
engine_fail(
	struct media_engine *engine,
	int error)
{
	/* Runtime errors use errno; decoder admission separately records MEDIA_PROBLEM_* values. */
	(void)pthread_mutex_lock(&engine->lock);
	engine->error = error;
	engine->problem = 0;
	engine->state = MEDIA_FAILED;
	(void)pthread_mutex_unlock(&engine->lock);
	engine_wake(engine);
}

/*
 * Reports the clock (seconds): the monotonic clock's while playing,
 * standing still otherwise.
 * Called with the lock held (or by the thread that alone writes the
 * anchors).
 */
static double
engine_clock(
	struct media_engine *engine)
{
	uint64_t now;
	double clock;

	/* Standing still. */
	if (engine->state != MEDIA_PLAYING || engine->seek_wanted)
		return engine->clock_time;

	/* The time since the anchor. */
	now = engine_now_us();
	clock = engine->clock_time + (double)(now - engine->clock_us) / 1000000.0;

	/* Not past the end. */
	if (engine->duration > 0.0 && clock > engine->duration)
		clock = engine->duration;
	return clock;
}

/* Frees the pictures that wait (the lock held, or the thread gone). */
static void
engine_drop_pictures(
	struct media_engine *engine)
{
	unsigned index;

	/* Each. */
	for (index = 0; index < ENGINE_PICTURES; index++)
		media_frame_free(&engine->pictures[index]);
	engine->picture_first = 0;
	engine->picture_count = 0;
}

/* Tells whether the thread is to end. */
static int
engine_stopping(
	struct media_engine *engine)
{
	int quit;

	/* Read under the lock. */
	(void)pthread_mutex_lock(&engine->lock);
	quit = engine->quit;
	(void)pthread_mutex_unlock(&engine->lock);
	return quit;
}

/* Makes the wake readable (a full pipe is readable already). */
static void
engine_wake(
	struct media_engine *engine)
{
	ssize_t wrote;
	char byte;

	/* One byte. */
	byte = 1;
	wrote = write(engine->wake[1], &byte, 1U);
	(void)wrote;
}

/* The monotonic clock in microseconds. */
static uint64_t
engine_now_us(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Sleeps a number of milliseconds. */
static void
engine_sleep_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The wait. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
