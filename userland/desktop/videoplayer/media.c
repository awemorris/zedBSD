/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The media thread of the player: mediafile (WS122 p003) reads the file,
 * the decoding add-in (codec.c, p004) decodes its first video track and its
 * first audio track it has a decoder for.  The pictures wait in a small
 * ring for the window to take them at their time; the sound is converted
 * to 16-bit stereo at the stream's rate and written into the stream's ring,
 * which paces the reading.  A seek drops what is decoded and queued, and
 * starts the clock again at the time sought.
 */

#include "videoplayer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The longest a full ring of sound or pictures is waited on before looking at the requests again (ms). */
#define MEDIA_WAIT_MS		10

/* The sound converted at once at most (frames). */
#define MEDIA_SOUND_FRAMES	8192

/*
 * What the media thread holds: the file, the two decoders and their
 * tracks, the time before which what decodes is passed over (after a
 * seek), and the converted sound.
 */
struct media_reader {
	struct vp_media *media;
	struct media_file *file;
	struct media_decoder *video;
	struct media_decoder *sound;
	unsigned video_track;
	unsigned sound_track;
	double skip_before;
	int16_t samples[MEDIA_SOUND_FRAMES * 2];
};

static void *media_run(void *argument);
static int media_reader_open(struct media_reader *reader, const char *path);
static void media_reader_close(struct media_reader *reader);
static int media_decoder(struct media_reader *reader, unsigned kind, struct media_decoder **result, unsigned *track);
static int media_feed(struct media_reader *reader, struct media_decoder *decoder, const struct media_packet *packet);
static int media_drain(struct media_reader *reader, struct media_decoder *decoder);
static int media_picture(struct media_reader *reader, struct media_frame *picture, double time);
static int media_sound(struct media_reader *reader, double time);
static int media_seek(struct media_reader *reader);
static void media_drop_pictures(struct vp_media *media);
static int media_stopping(struct vp_media *media);
static uint64_t media_now_us(void);
static void media_sleep_ms(unsigned ms);

/*
 * Makes the media empty, with the sound it plays to (NULL or a stream that
 * is not made: no sound).
 */
void
vp_media_init(
	struct vp_media *media,
	struct vp_audio *audio)
{
	/* Nothing open. */
	memset(media, 0, sizeof(*media));
	media->audio = audio;
	(void)pthread_mutex_init(&media->lock, NULL);
	(void)pthread_cond_init(&media->wake, NULL);
}

/*
 * Opens a file and starts reading it, paused at its start.  Returns 0, or
 * an errno value (the file is not one mediafile reads, or has no video it
 * can decode; media->codec_problem then says why, MEDIA_PROBLEM_*).
 */
int
vp_media_open(
	struct vp_media *media,
	const char *path)
{
	struct media_reader *reader;
	int64_t length_us;
	int error;

	/* What was open goes; the sound is opened again when its service went or came (WS191). */
	vp_media_close(media);
	if (media->audio != NULL)
		vp_audio_renew(media->audio);

	/* The reader, opened here so that a file that cannot be played is told at once. */
	reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->media = media;
	media->codec_problem = 0;
	error = media_reader_open(reader, path);
	if (error != 0) {
		media_reader_close(reader);
		free(reader);
		return error;
	}

	/* What is known of the file, paused at its start. */
	(void)pthread_mutex_lock(&media->lock);
	(void)snprintf(media->path, sizeof(media->path), "%s", path);
	media->width = (int)media_file_track(reader->file, reader->video_track)->width;
	media->height = (int)media_file_track(reader->file, reader->video_track)->height;
	media->duration = 0.0;
	length_us = media_file_duration_us(reader->file);
	if (length_us > 0)
		media->duration = (double)length_us / 1000000.0;
	media->has_audio = reader->sound != NULL;
	media->state = VP_PAUSED;
	media->eof = 0;
	media->quit = 0;
	media->seek_wanted = 0;
	media->clock_time = 0.0;
	media->clock_frames = vp_audio_clock_position(media->audio);
	media->clock_us = media_now_us();
	(void)pthread_mutex_unlock(&media->lock);

	/* The thread reads ahead from here. */
	error = pthread_create(&media->thread, NULL, media_run, reader);
	if (error != 0) {
		media_reader_close(reader);
		free(reader);
		return error;
	}

	/* Succeeded: the thread owns the reader. */
	media->thread_started = 1;
	return 0;
}

/*
 * Ends the reading and forgets the file.
 */
void
vp_media_close(
	struct vp_media *media)
{
	/* The thread is asked to end, and waited for. */
	if (media->thread_started) {
		(void)pthread_mutex_lock(&media->lock);
		media->quit = 1;
		(void)pthread_cond_broadcast(&media->wake);
		(void)pthread_mutex_unlock(&media->lock);
		(void)pthread_join(media->thread, NULL);
		media->thread_started = 0;
	}

	/* The sound stops and is dropped. */
	if (media->audio != NULL) {
		(void)vp_audio_stop(media->audio);
		(void)vp_audio_flush(media->audio);
	}

	/* The pictures and the state. */
	(void)pthread_mutex_lock(&media->lock);
	media_drop_pictures(media);
	media->state = VP_EMPTY;
	media->path[0] = '\0';
	media->eof = 0;
	(void)pthread_mutex_unlock(&media->lock);
}

/*
 * Plays from where it is (from the start again after the end).
 */
void
vp_media_play(
	struct vp_media *media)
{
	unsigned state;

	/* From the start again after the end. */
	(void)pthread_mutex_lock(&media->lock);
	state = media->state;
	(void)pthread_mutex_unlock(&media->lock);
	if (state == VP_ENDED)
		vp_media_seek(media, 0.0);
	if (state == VP_EMPTY || state == VP_PLAYING)
		return;

	/* The clock goes on from where it stood. */
	(void)pthread_mutex_lock(&media->lock);
	media->clock_time = vp_media_clock(media);
	media->clock_frames = vp_audio_clock_position(media->audio);
	media->clock_us = media_now_us();
	media->state = VP_PLAYING;
	(void)pthread_cond_broadcast(&media->wake);
	(void)pthread_mutex_unlock(&media->lock);

	/* And the sound with it. */
	if (media->has_audio)
		(void)vp_audio_start(media->audio);
}

/*
 * Pauses where it is.
 */
void
vp_media_pause(
	struct vp_media *media)
{
	double now;

	/* Only while playing. */
	(void)pthread_mutex_lock(&media->lock);
	if (media->state != VP_PLAYING) {
		(void)pthread_mutex_unlock(&media->lock);
		return;
	}

	/* The clock stands where it is. */
	now = vp_media_clock(media);
	media->state = VP_PAUSED;
	media->clock_time = now;
	(void)pthread_mutex_unlock(&media->lock);

	/* And the sound with it. */
	if (media->has_audio)
		(void)vp_audio_stop(media->audio);
}

/*
 * Asks for a seek to a time (seconds, kept within the file).
 */
void
vp_media_seek(
	struct vp_media *media,
	double seconds)
{
	/* Within the file. */
	if (seconds < 0.0)
		seconds = 0.0;
	if (media->duration > 0.0 && seconds > media->duration)
		seconds = media->duration;

	/* The request; the clock stands at the time sought until the thread starts it again. */
	(void)pthread_mutex_lock(&media->lock);
	media->seek_wanted = 1;
	media->seek_to = seconds;
	media->clock_time = seconds;
	media->clock_us = media_now_us();
	if (media->state == VP_ENDED)
		media->state = VP_PAUSED;
	media_drop_pictures(media);
	(void)pthread_cond_broadcast(&media->wake);
	(void)pthread_mutex_unlock(&media->lock);
}

/*
 * Reports the clock (seconds): the sound's while playing with sound, the
 * monotonic clock's while playing without, standing still otherwise.
 * Called with or without the lock (it reads the anchors only).
 */
double
vp_media_clock(
	struct vp_media *media)
{
	uint64_t frames;
	uint64_t now;

	/* Standing still. */
	if (media->state != VP_PLAYING || media->seek_wanted)
		return media->clock_time;

	/* The sound read since the anchor. */
	if (media->has_audio && media->audio != NULL && media->audio->created) {
		frames = vp_audio_clock_position(media->audio);
		if (frames < media->clock_frames)
			return media->clock_time;
		return media->clock_time + (double)(frames - media->clock_frames) / (double)media->audio->rate;
	}

	/* The time since the anchor. */
	now = media_now_us();
	return media->clock_time + (double)(now - media->clock_us) / 1000000.0;
}

/*
 * Takes the latest picture whose time has come (the older ones are
 * dropped): its reference with its time, or NULL.  next is the time of the
 * picture after it, or -1 when none waits.
 */
struct media_frame *
vp_media_take(
	struct vp_media *media,
	double clock,
	double *time,
	double *next)
{
	struct media_frame *taken;
	unsigned slot;

	/* The latest due. */
	taken = NULL;
	*next = -1.0;
	(void)pthread_mutex_lock(&media->lock);
	while (media->picture_count > 0U) {
		/* Not due yet: the next time. */
		slot = media->picture_first;
		if (media->picture_times[slot] > clock) {
			*next = media->picture_times[slot];
			break;
		}

		/* Due: it replaces the one taken before it. */
		media_frame_free(&taken);
		taken = media->pictures[slot];
		*time = media->picture_times[slot];
		media->pictures[slot] = NULL;
		media->picture_first = (slot + 1U) % VP_PICTURES;
		media->picture_count--;
	}

	/* Room for the reader. */
	(void)pthread_cond_broadcast(&media->wake);
	(void)pthread_mutex_unlock(&media->lock);
	return taken;
}

/* The media thread: reads, decodes and queues until it is told to end. */
static void *
media_run(
	void *argument)
{
	struct media_reader *reader;
	struct vp_media *media;
	struct media_packet packet;
	int drained;
	int status;
	int stop;

	/* Until told to end. */
	reader = argument;
	media = reader->media;
	for (;;) {
		/* The end, or a seek. */
		stop = media_stopping(media);
		if (stop)
			break;
		status = media_seek(reader);
		if (status != 0)
			continue;

		/* The next packet; the end of the file drains the decoders and waits. */
		status = media_file_read(reader->file, &packet);
		if (status != 0) {
			(void)pthread_mutex_lock(&media->lock);
			drained = media->eof;
			(void)pthread_mutex_unlock(&media->lock);
			if (!drained) {
				(void)media_feed(reader, reader->video, NULL);
				if (reader->sound != NULL)
					(void)media_feed(reader, reader->sound, NULL);
			}

			/* The end is noted once, and the reader waits for a seek or the end of the thread. */
			(void)pthread_mutex_lock(&media->lock);
			if (!media->eof)
				vp_log("END reached dropped=%llu", (unsigned long long)media_file_dropped(reader->file));
			media->eof = 1;
			(void)pthread_mutex_unlock(&media->lock);
			media_sleep_ms(MEDIA_WAIT_MS * 5);
			continue;
		}

		/* Its decoder. */
		if (packet.track == reader->video_track)
			(void)media_feed(reader, reader->video, &packet);
		else if (reader->sound != NULL && packet.track == reader->sound_track)
			(void)media_feed(reader, reader->sound, &packet);
	}

	/* The reader goes with the thread. */
	media_reader_close(reader);
	free(reader);
	return NULL;
}

/* Opens the file and its decoders; 0 or an errno value (media->codec_problem says a decoding problem). */
static int
media_reader_open(
	struct media_reader *reader,
	const char *path)
{
	long long length;
	int64_t length_us;
	int status;

	/* The container. */
	status = media_file_open(path, &reader->file);
	if (status != 0)
		return status;

	/* The video, which a player needs. */
	status = media_decoder(reader, MEDIA_TRACK_VIDEO, &reader->video, &reader->video_track);
	if (status != 0) {
		reader->media->codec_problem = status;
		if (status == ENOMEM)
			reader->media->codec_problem = 0;
		return ENOTSUP;
	}

	/* The sound, when there is a track it decodes and a stream plays it. */
	if (reader->media->audio != NULL && reader->media->audio->created) {
		status = media_decoder(reader, MEDIA_TRACK_AUDIO, &reader->sound, &reader->sound_track);
		if (status != 0)
			reader->sound = NULL;
	}

	/* Succeeded: the log line the tests read. */
	length = -1;
	length_us = media_file_duration_us(reader->file);
	if (length_us > 0)
		length = (long long)(length_us / 1000);
	vp_log("OPEN path=%s width=%u height=%u duration_ms=%lld video=%s audio=%s container=%s dropped=%llu",
	    path, media_file_track(reader->file, reader->video_track)->width, media_file_track(reader->file, reader->video_track)->height, length,
	    media_decoder_name(reader->video), media_decoder_name(reader->sound), media_file_format_name(reader->file),
	    (unsigned long long)media_file_dropped(reader->file));
	return 0;
}

/* Closes what the reader opened. */
static void
media_reader_close(
	struct media_reader *reader)
{
	/* Each part, when it was made. */
	media_decoder_close(reader->video);
	reader->video = NULL;
	media_decoder_close(reader->sound);
	reader->sound = NULL;
	if (reader->file != NULL)
		media_file_close(reader->file);
	reader->file = NULL;
}

/*
 * Opens the decoder of the file's first track of a kind that has one.
 * Returns 0, the last MEDIA_PROBLEM_* problem, or ENOENT when there is no track
 * of the kind.
 */
static int
media_decoder(
	struct media_reader *reader,
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
	count = media_file_track_count(reader->file);
	for (index = 0; index < count; index++) {
		found = media_file_track(reader->file, index);
		if (found == NULL || found->kind != kind)
			continue;
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

/*
 * Gives a decoder a packet (NULL drains it) and handles everything that
 * comes out; nonzero when a seek or the end interrupted.
 */
static int
media_feed(
	struct media_reader *reader,
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
		status = media_drain(reader, decoder);
		if (status != 0)
			return status;
	}

	/* What comes out of it. */
	status = media_drain(reader, decoder);
	return status;
}

/* Handles every picture or sound a decoder has ready; nonzero when a seek or the end interrupted. */
static int
media_drain(
	struct media_reader *reader,
	struct media_decoder *decoder)
{
	struct media_frame *picture;
	int64_t time_us;
	int received;
	int status;

	/* Each one. */
	for (;;) {
		received = media_decoder_receive(decoder, &time_us);
		if (!received)
			return 0;
		if (decoder == reader->video) {
			picture = media_decoder_picture(decoder);
			if (picture == NULL)
				continue;
			status = media_picture(reader, picture, (double)time_us / 1000000.0);
		} else {
			status = media_sound(reader, (double)time_us / 1000000.0);
		}

		/* A seek or the end stops the drain. */
		if (status != 0)
			return status;
	}
}

/* Queues a picture (the reference is the ring's or freed), waiting for room; nonzero when a seek or the end came meanwhile. */
static int
media_picture(
	struct media_reader *reader,
	struct media_frame *picture,
	double time)
{
	struct vp_media *media;
	struct timespec until;
	unsigned slot;

	/* A picture before the time sought is passed over. */
	media = reader->media;
	if (time < reader->skip_before) {
		media_frame_free(&picture);
		return 0;
	}

	/* Room in the ring, unless a seek or the end comes first. */
	(void)pthread_mutex_lock(&media->lock);
	while (media->picture_count == VP_PICTURES && !media->quit && !media->seek_wanted) {
		(void)clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += MEDIA_WAIT_MS * 1000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}

		/* Woken by a picture taken or a request, or after the wait. */
		(void)pthread_cond_timedwait(&media->wake, &media->lock, &until);
	}

	/* Interrupted. */
	if (media->quit || media->seek_wanted) {
		(void)pthread_mutex_unlock(&media->lock);
		media_frame_free(&picture);
		return 1;
	}

	/* Queued. */
	slot = (media->picture_first + media->picture_count) % VP_PICTURES;
	media->pictures[slot] = picture;
	media->picture_times[slot] = time;
	media->picture_count++;
	(void)pthread_mutex_unlock(&media->lock);
	return 0;
}

/* Converts the sound received and writes it, waiting for room; nonzero when a seek or the end came meanwhile. */
static int
media_sound(
	struct media_reader *reader,
	double time)
{
	struct vp_media *media;
	size_t converted;
	size_t written;
	size_t done;
	int stop;

	/* Sound before the time sought is passed over. */
	media = reader->media;
	if (time < reader->skip_before)
		return 0;

	/* Converted. */
	converted = media_decoder_sound(reader->sound, reader->samples, MEDIA_SOUND_FRAMES, media->audio->rate);
	if (converted == 0U)
		return 0;

	/* Written as room opens. */
	done = 0;
	while (done < converted) {
		stop = media_stopping(media);
		if (stop)
			return 1;
		(void)pthread_mutex_lock(&media->lock);
		stop = media->seek_wanted;
		(void)pthread_mutex_unlock(&media->lock);
		if (stop)
			return 1;
		written = vp_audio_write(media->audio, reader->samples + done * 2U, converted - done);
		done += written;
		if (done < converted)
			media_sleep_ms(MEDIA_WAIT_MS);
	}

	/* Written. */
	return 0;
}

/*
 * Carries out a seek asked for: the file at the time sought, the decoders
 * and the sound emptied, the clock anchored there.  Returns 1 when one was
 * carried out.
 */
static int
media_seek(
	struct media_reader *reader)
{
	struct vp_media *media;
	double seconds;
	int wanted;

	/* A seek asked for. */
	media = reader->media;
	(void)pthread_mutex_lock(&media->lock);
	wanted = media->seek_wanted;
	seconds = media->seek_to;
	(void)pthread_mutex_unlock(&media->lock);
	if (!wanted)
		return 0;

	/* The file at the key frame before it, the decoders and the sound emptied. */
	(void)media_file_seek(reader->file, (int64_t)(seconds * 1000000.0));
	media_decoder_flush(reader->video);
	if (reader->sound != NULL) {
		media_decoder_flush(reader->sound);
		(void)vp_audio_flush(media->audio);
	}

	/* What decodes before the time sought is passed over. */
	reader->skip_before = seconds;

	/* The clock anchored at the time sought; the pictures dropped again (some may have come meanwhile). */
	(void)pthread_mutex_lock(&media->lock);
	media_drop_pictures(media);
	media->clock_time = seconds;
	media->clock_frames = vp_audio_clock_position(media->audio);
	media->clock_us = media_now_us();
	media->eof = 0;
	media->seek_wanted = 0;
	(void)pthread_mutex_unlock(&media->lock);
	vp_log("SEEK done to_ms=%lld", (long long)(seconds * 1000.0));

	/* Carried out. */
	return 1;
}

/* Frees the pictures that wait (the lock held). */
static void
media_drop_pictures(
	struct vp_media *media)
{
	unsigned index;

	/* Each. */
	for (index = 0; index < VP_PICTURES; index++)
		media_frame_free(&media->pictures[index]);
	media->picture_first = 0;
	media->picture_count = 0;
}

/* Tells whether the thread is to end. */
static int
media_stopping(
	struct vp_media *media)
{
	int quit;

	/* Read under the lock. */
	(void)pthread_mutex_lock(&media->lock);
	quit = media->quit;
	(void)pthread_mutex_unlock(&media->lock);
	return quit;
}

/* The monotonic clock in microseconds. */
static uint64_t
media_now_us(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Sleeps a number of milliseconds. */
static void
media_sleep_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The wait. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
