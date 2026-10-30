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
 *
 * The clock is the sound's, so the sound must never wait for the pictures
 * (BUG-300): while the ring of pictures is full, the reader keeps the
 * picture that did not fit, keeps reading, decodes and writes the sound, and
 * keeps the video packets in the file's order in a bounded queue, which the
 * video decoder takes as the ring makes room.
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
 * The video packets read ahead of their decoding at most, by count and by
 * bytes.  The sound's ring holds half a second and a file interleaves its
 * tracks in chunks of about half a second, so the queue holds about a
 * second of video in an ordinary file; the bounds keep an unusual file
 * (tracks not interleaved) from taking the memory, and then the reader
 * waits for the pictures as before.
 */
#define MEDIA_PACKETS		128U
#define MEDIA_PACKET_BYTES	(64U * 1024U * 1024U)

/* What media_picture and media_drain report when the ring of pictures is full and a picture is held back. */
#define MEDIA_FULL		2

/*
 * A video packet read ahead of its decoding: a copy of the container's
 * packet whose bytes the queue owns, or the end of the video track (which
 * drains the decoder).  sent says that the decoder has taken it; it stays
 * in the queue until the decoder's output after it is taken, as the
 * container's bytes stay valid until the next read.
 */
struct media_queued {
	struct media_packet packet;
	unsigned char *bytes;
	int end;
	int sent;
};

/*
 * What the media thread holds: the file, the two decoders and their
 * tracks, the time before which what decodes is passed over (after a
 * seek), the converted sound, and the video waiting for room in the ring
 * of pictures: the packets read ahead (a ring, in the file's order, with
 * their bytes in all), the picture decoded that did not fit, and whether
 * the end of the video track is queued.
 */
struct media_reader {
	struct vp_media *media;
	struct media_file *file;
	struct app_decoder *video;
	struct app_decoder *sound;
	unsigned video_track;
	unsigned sound_track;
	double skip_before;
	int sound_trimmed;
	int16_t samples[MEDIA_SOUND_FRAMES * 2];
	struct media_queued queue[MEDIA_PACKETS];
	unsigned queue_first;
	unsigned queue_count;
	size_t queue_bytes;
	struct app_frame *held;
	double held_time;
	int video_ended;
};

static void media_run(void *argument);
static int media_reader_open(struct media_reader *reader, const char *path);
static void media_reader_close(struct media_reader *reader);
static int app_decoder(struct media_reader *reader, unsigned kind, struct app_decoder **result, unsigned *track);
static int media_feed(struct media_reader *reader, struct app_decoder *decoder, const struct media_packet *packet);
static int media_drain(struct media_reader *reader, struct app_decoder *decoder);
static int media_picture(struct media_reader *reader, struct app_frame *picture, double time);
static int media_place(struct media_reader *reader);
static int media_video_pump(struct media_reader *reader);
static int media_enqueue(struct media_reader *reader, const struct media_packet *packet);
static void media_dequeue(struct media_reader *reader);
static void media_video_reset(struct media_reader *reader);
static int media_queue_full(const struct media_reader *reader);
static void media_wait_room(struct vp_media *media);
static int media_sound(struct media_reader *reader, double time);
static int media_seek(struct media_reader *reader);
static void media_fail(struct media_reader *reader, int error);
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

	/*
	 * The pictures decoded ahead: fewer with the presenter, whose pictures
	 * each hold an image of the decoder until they are freed.
	 */
	media->picture_limit = VP_PICTURES;
	if (VP_GPU_PRESENT)
		media->picture_limit = VP_GPU_PICTURES;
	(void)kl_mutex_init(&media->lock);
	(void)kl_cond_init(&media->wake);
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
	struct kl_thread_options options;
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
	media->failure = 0;
	media->late = 0U;
	error = media_reader_open(reader, path);
	if (error != 0) {
		media_reader_close(reader);
		free(reader);
		return error;
	}

	/* What is known of the file, paused at its start. */
	kl_mutex_lock(&media->lock);
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
	kl_mutex_unlock(&media->lock);

	/* The thread reads ahead from here: its name, what it runs and its priority. */
	memset(&options, 0, sizeof(options));
	options.name = "video-decode";
	options.run = media_run;
	options.argument = reader;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &media->thread);
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
		kl_mutex_lock(&media->lock);
		media->quit = 1;
		kl_cond_broadcast(&media->wake);
		kl_mutex_unlock(&media->lock);
		(void)kl_thread_join(media->thread);
		media->thread = NULL;
		media->thread_started = 0;
	}

	/* The sound stops and is dropped. */
	if (media->audio != NULL) {
		(void)vp_audio_stop(media->audio);
		(void)vp_audio_flush(media->audio);
	}

	/* The pictures and the state. */
	kl_mutex_lock(&media->lock);
	media_drop_pictures(media);
	media->state = VP_EMPTY;
	media->path[0] = '\0';
	media->eof = 0;
	kl_mutex_unlock(&media->lock);
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
	kl_mutex_lock(&media->lock);
	state = media->state;
	kl_mutex_unlock(&media->lock);
	if (state == VP_ENDED)
		vp_media_seek(media, 0.0);
	if (state == VP_EMPTY || state == VP_PLAYING || state == VP_FAILED)
		return;

	/* The clock goes on from where it stood. */
	kl_mutex_lock(&media->lock);
	media->clock_time = vp_media_clock(media);
	media->clock_frames = vp_audio_clock_position(media->audio);
	media->clock_us = media_now_us();
	media->state = VP_PLAYING;
	kl_cond_broadcast(&media->wake);
	kl_mutex_unlock(&media->lock);

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
	kl_mutex_lock(&media->lock);
	if (media->state != VP_PLAYING) {
		kl_mutex_unlock(&media->lock);
		return;
	}

	/* The clock stands where it is. */
	now = vp_media_clock(media);
	media->state = VP_PAUSED;
	media->clock_time = now;
	kl_mutex_unlock(&media->lock);

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
	kl_mutex_lock(&media->lock);
	media->seek_wanted = 1;
	media->seek_to = seconds;
	media->clock_time = seconds;
	media->clock_us = media_now_us();
	if (media->state == VP_ENDED)
		media->state = VP_PAUSED;
	media_drop_pictures(media);
	kl_cond_broadcast(&media->wake);
	kl_mutex_unlock(&media->lock);
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
struct app_frame *
vp_media_take(
	struct vp_media *media,
	double clock,
	double *time,
	double *next)
{
	struct app_frame *taken;
	unsigned slot;

	/* The latest due. */
	taken = NULL;
	*next = -1.0;
	kl_mutex_lock(&media->lock);
	while (media->picture_count > 0U) {
		/* Not due yet: the next time. */
		slot = media->picture_first;
		if (media->picture_times[slot] > clock) {
			*next = media->picture_times[slot];
			break;
		}

		/* Due: it replaces the one taken before it, which never reached presentation. */
		if (taken != NULL)
			media->late++;
		app_frame_free(&taken);
		taken = media->pictures[slot];
		*time = media->picture_times[slot];
		media->pictures[slot] = NULL;
		media->picture_first = (slot + 1U) % VP_PICTURES;
		media->picture_count--;
	}

	/* Room for the reader. */
	kl_cond_broadcast(&media->wake);
	kl_mutex_unlock(&media->lock);
	return taken;
}

/* The media thread: reads, decodes and queues until it is told to end. */
static void
media_run(
	void *argument)
{
	struct media_reader *reader;
	struct vp_media *media;
	struct media_packet packet;
	int drained;
	int status;
	int full;
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

		/* Wait in the failed state while leaving seek and stop requests available. */
		kl_mutex_lock(&media->lock);
		stop = 0;
		if (media->state == VP_FAILED)
			stop = 1;
		kl_mutex_unlock(&media->lock);
		if (stop) {
			media_sleep_ms(MEDIA_WAIT_MS * 5);
			continue;
		}

		/* The pictures decoded from the video packets read ahead, as far as the ring has room. */
		status = media_video_pump(reader);
		if (status < 0) {
			media_fail(reader, -status);
			continue;
		}

		/* A seek or the end interrupted the pictures. */
		if (status != 0)
			continue;

		/* With the ring and the queue of video packets both full, the reader waits for a picture to be taken. */
		full = media_queue_full(reader);
		if (full) {
			media_wait_room(media);
			continue;
		}

		/* The next packet; the end of the file drains the decoders and waits. */
		status = media_file_read(reader->file, &packet);
		if (status == ENODATA) {
			kl_mutex_lock(&media->lock);
			drained = media->eof;
			kl_mutex_unlock(&media->lock);
			if (!drained) {
				/* The end of the video track follows its last packet, and drains its decoder in turn. */
				if (!reader->video_ended) {
					status = media_enqueue(reader, NULL);
					if (status < 0) {
						media_fail(reader, -status);
						continue;
					}

					/* The end is queued once; a seek reads the track, and its end, again. */
					reader->video_ended = 1;
					continue;
				}

				/* The last pictures go to the ring as it makes room, before the sound's end. */
				if (reader->queue_count > 0U || reader->held != NULL) {
					media_wait_room(media);
					continue;
				}

				/* The sound decoder gives the sound it still holds. */
				status = 0;
				if (reader->sound != NULL)
					status = media_feed(reader, reader->sound, NULL);
				if (status < 0) {
					media_fail(reader, -status);
					continue;
				}

				/* An interrupted drain is retried after the pending seek or stop. */
				if (status != 0)
					continue;
			}

			/* The end is noted once, and the reader waits for a seek or the end of the thread. */
			kl_mutex_lock(&media->lock);
			if (!media->eof)
				vp_log("END reached dropped=%llu", (unsigned long long)media_file_dropped(reader->file));
			media->eof = 1;
			kl_mutex_unlock(&media->lock);
			media_sleep_ms(MEDIA_WAIT_MS * 5);
			continue;
		}

		/* A read error has no successful end-of-file or decoder-drain semantics. */
		if (status != 0) {
			media_fail(reader, status);
			continue;
		}

		/*
		 * A video packet joins the queue (decoded at the next turn, as the
		 * ring has room) and a sound packet is decoded and written now, so
		 * the sound goes on while the pictures wait.  A runtime decode
		 * failure pauses the file and is published to the window once.
		 */
		status = 0;
		if (packet.track == reader->video_track)
			status = media_enqueue(reader, &packet);
		else if (reader->sound != NULL && packet.track == reader->sound_track)
			status = media_feed(reader, reader->sound, &packet);
		if (status < 0)
			media_fail(reader, -status);

	}

	/* The reader goes with the thread. */
	media_reader_close(reader);
	free(reader);
}

/* Opens the file and its decoders; 0 or an errno value (media->codec_problem says a decoding problem). */
static int
media_reader_open(
	struct media_reader *reader,
	const char *path)
{
	const char *video_name;
	const char *video_backend;
	const char *sound_name;
	const char *sound_backend;
	const char *container_name;
	const struct media_track *video_info;
	uint64_t dropped;
	long long length;
	int64_t length_us;
	int status;

	/* The container. */
	status = media_file_open(path, &reader->file);
	if (status != 0)
		return status;

	/* The video, which a player needs. */
	status = app_decoder(reader, MEDIA_TRACK_VIDEO, &reader->video, &reader->video_track);
	if (status != 0) {
		reader->media->codec_problem = status;
		if (status == ENOMEM)
			reader->media->codec_problem = 0;
		return ENOTSUP;
	}

	/* The sound, when there is a track it decodes and a stream plays it. */
	if (reader->media->audio != NULL && reader->media->audio->created) {
		status = app_decoder(reader, MEDIA_TRACK_AUDIO, &reader->sound, &reader->sound_track);
		if (status != 0)
			reader->sound = NULL;
	}

	/* Succeeded: the log line the tests read. */
	length = -1;
	length_us = media_file_duration_us(reader->file);
	if (length_us > 0)
		length = (long long)(length_us / 1000);
	video_name = app_decoder_name(reader->video);
	video_backend = app_decoder_backend(reader->video);
	sound_name = app_decoder_name(reader->sound);
	sound_backend = app_decoder_backend(reader->sound);
	container_name = media_file_format_name(reader->file);
	video_info = media_file_track(reader->file, reader->video_track);
	if (video_info == NULL)
		return EINVAL;
	dropped = media_file_dropped(reader->file);
	vp_log("OPEN path=%s width=%u height=%u duration_ms=%lld video=%s/%s audio=%s/%s container=%s dropped=%llu",
	    path,
	    video_info->width,
	    video_info->height,
	    length,
	    video_name,
	    video_backend,
	    sound_name,
	    sound_backend,
	    container_name,
	    (unsigned long long)dropped);
	return 0;
}

/* Closes what the reader opened. */
static void
media_reader_close(
	struct media_reader *reader)
{
	/* The video waiting for the ring goes before its decoder. */
	media_video_reset(reader);

	/* Each part, when it was made. */
	app_decoder_close(reader->video);
	reader->video = NULL;
	app_decoder_close(reader->sound);
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
app_decoder(
	struct media_reader *reader,
	unsigned kind,
	struct app_decoder **result,
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
		status = app_decoder_open(found, result);
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
	struct app_decoder *decoder,
	const struct media_packet *packet)
{
	int status;
	int tries;

	/* Sent; a full decoder gives its pictures or sound first, then takes the packet. */
	for (tries = 0; tries < 2; tries++) {
		status = app_decoder_send(decoder, packet);
		if (status != EAGAIN)
			break;
		status = media_drain(reader, decoder);
		if (status != 0)
			return status;
	}

	/* A rejected packet must not disappear silently or be followed by uninitialized output. */
	if (status != 0)
		return -status;

	/* What comes out of it. */
	status = media_drain(reader, decoder);
	return status;
}

/*
 * Handles every picture or sound a decoder has ready.  Returns 0, MEDIA_FULL
 * when the ring of pictures is full (a picture held back), 1 when a seek or
 * the end interrupted, or a negative errno value.
 */
static int
media_drain(
	struct media_reader *reader,
	struct app_decoder *decoder)
{
	struct app_frame *picture;
	int64_t time_us;
	int received;
	int status;

	/* Each one. */
	for (;;) {
		received = app_decoder_receive(decoder, &time_us);
		if (received < 0)
			return received;
		if (received == 0)
			return 0;
		if (decoder == reader->video) {
			picture = app_decoder_picture(decoder);
			if (picture == NULL)
				return -ENOMEM;
			status = media_picture(reader, picture, (double)time_us / 1000000.0);
		} else {
			status = media_sound(reader, (double)time_us / 1000000.0);
		}

		/* A full ring, a seek or the end stops the drain. */
		if (status != 0)
			return status;
	}
}

/*
 * Queues a picture in the ring, or holds it back while the ring is full
 * (the reader's, until media_place puts it in).  Returns 0, MEDIA_FULL when
 * it is held back, or 1 when a seek or the end came (the held picture then
 * goes with the seek or the reader).
 */
static int
media_picture(
	struct media_reader *reader,
	struct app_frame *picture,
	double time)
{
	int status;

	/* A picture before the time sought is passed over. */
	if (time < reader->skip_before) {
		app_frame_free(&picture);
		return 0;
	}

	/* The reader holds it until it is in the ring. */
	reader->held = picture;
	reader->held_time = time;

	/* Into the ring, when it has room. */
	status = media_place(reader);
	if (status != 0)
		return status;

	/* Succeeded: the picture is the ring's. */
	return 0;
}

/*
 * Puts the picture held back into the ring when it has room.  Returns 0
 * (put in, or none held), MEDIA_FULL when the ring is still full, or 1 when
 * a seek or the end came.
 */
static int
media_place(
	struct media_reader *reader)
{
	struct vp_media *media;
	unsigned slot;

	/* Nothing held back. */
	if (reader->held == NULL)
		return 0;

	/* Into the next slot of the ring, unless a seek or the end came or the ring is still full. */
	media = reader->media;
	kl_mutex_lock(&media->lock);

	/* A seek or the end came: the seek or the reader frees the picture held back. */
	if (media->quit || media->seek_wanted) {
		kl_mutex_unlock(&media->lock);
		return 1;
	}

	/* The ring is still full: the picture stays held back. */
	if (media->picture_count >= media->picture_limit) {
		kl_mutex_unlock(&media->lock);
		return MEDIA_FULL;
	}

	/* The next slot takes it. */
	slot = (media->picture_first + media->picture_count) % VP_PICTURES;
	media->pictures[slot] = reader->held;
	media->picture_times[slot] = reader->held_time;
	media->picture_count++;

	kl_mutex_unlock(&media->lock);

	/* Succeeded: the ring owns the picture now. */
	reader->held = NULL;
	return 0;
}

/*
 * Decodes the video packets read ahead, in the file's order, as far as the
 * ring of pictures has room: the picture held back first, then what the
 * decoder has ready, then the next packet.  Returns 0 when the queue is
 * empty or the ring is full (a picture held back), 1 when a seek or the
 * end came, or a negative errno value for a decoding failure.
 */
static int
media_video_pump(
	struct media_reader *reader)
{
	struct media_queued *head;
	const struct media_packet *packet;
	int tries;
	int status;

	/* Until the queue is empty or the ring is full. */
	tries = 0;
	for (;;) {
		/* The picture held back goes first, so the pictures keep their order. */
		status = media_place(reader);
		if (status == MEDIA_FULL)
			return 0;
		if (status != 0)
			return status;

		/* What the decoder has ready, until the ring is full again. */
		status = media_drain(reader, reader->video);
		if (status == MEDIA_FULL)
			return 0;
		if (status != 0)
			return status;

		/* The packet sent last is done with once the output it gave is taken. */
		head = &reader->queue[reader->queue_first];
		if (reader->queue_count > 0U && head->sent)
			media_dequeue(reader);

		/* Nothing more is read ahead. */
		if (reader->queue_count == 0U)
			return 0;

		/* The next packet, or NULL at the end of the track, which drains the decoder. */
		head = &reader->queue[reader->queue_first];
		packet = &head->packet;
		if (head->end)
			packet = NULL;
		status = app_decoder_send(reader->video, packet);

		/* A full decoder gives its pictures first, then takes the packet, once. */
		if (status == EAGAIN) {
			tries++;
			if (tries >= 2)
				return -EAGAIN;
			continue;
		}

		/* A rejected packet must not disappear silently or be followed by uninitialized output. */
		if (status != 0)
			return -status;

		/* Taken: its output is drained at the next turn of the loop. */
		tries = 0;
		head->sent = 1;
	}
}

/*
 * Adds a copy of a video packet to the queue read ahead, or the end of the
 * track when packet is NULL.  The caller makes sure that there is a free
 * slot (media_queue_full).  Returns 0 or -ENOMEM.
 */
static int
media_enqueue(
	struct media_reader *reader,
	const struct media_packet *packet)
{
	struct media_queued *entry;
	unsigned slot;

	/* The next free slot, emptied. */
	slot = (reader->queue_first + reader->queue_count) % MEDIA_PACKETS;
	entry = &reader->queue[slot];
	memset(entry, 0, sizeof(*entry));

	/* The end of the track has no bytes. */
	if (packet == NULL) {
		entry->end = 1;
		reader->queue_count++;
		return 0;
	}

	/* The bytes, which the container keeps only until its next read. */
	entry->bytes = malloc(packet->size + 1U);
	if (entry->bytes == NULL)
		return -ENOMEM;
	if (packet->size > 0U)
		memcpy(entry->bytes, packet->data, packet->size);

	/* The packet points at the queue's copy. */
	entry->packet = *packet;
	entry->packet.data = entry->bytes;
	reader->queue_bytes += packet->size;
	reader->queue_count++;

	/* Succeeded: the packet waits for the decoder. */
	return 0;
}

/* Removes the oldest packet read ahead and frees its bytes. */
static void
media_dequeue(
	struct media_reader *reader)
{
	struct media_queued *entry;

	/* The oldest. */
	entry = &reader->queue[reader->queue_first];
	reader->queue_bytes -= entry->packet.size;
	free(entry->bytes);
	memset(entry, 0, sizeof(*entry));

	/* The next one becomes the oldest. */
	reader->queue_first = (reader->queue_first + 1U) % MEDIA_PACKETS;
	reader->queue_count--;
}

/* Drops the video read ahead and the picture held back (after a seek, or when the reader goes). */
static void
media_video_reset(
	struct media_reader *reader)
{
	/* Every packet read ahead. */
	while (reader->queue_count > 0U)
		media_dequeue(reader);
	reader->queue_first = 0U;
	reader->queue_bytes = 0U;

	/* The picture held back, and the end of the track, which is read again. */
	app_frame_free(&reader->held);
	reader->video_ended = 0;
}

/* Tells whether the queue of video packets read ahead has no room for another packet. */
static int
media_queue_full(
	const struct media_reader *reader)
{
	/* By count, then by bytes. */
	if (reader->queue_count >= MEDIA_PACKETS)
		return 1;
	if (reader->queue_bytes >= MEDIA_PACKET_BYTES)
		return 1;

	/* Room for one more. */
	return 0;
}

/* Waits a little for room in the ring of pictures, unless it has room or a seek or the end is asked for. */
static void
media_wait_room(
	struct vp_media *media)
{
	/* Woken by a picture taken or a request, or after the wait. */
	kl_mutex_lock(&media->lock);

	if (media->picture_count >= media->picture_limit &&
	    !media->quit &&
	    !media->seek_wanted)
		(void)kl_cond_wait_for(&media->wake, &media->lock, MEDIA_WAIT_MS);

	kl_mutex_unlock(&media->lock);
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
	if (time < reader->skip_before && reader->sound_trimmed == 0)
		return 0;

	/* Converted. */
	converted = app_decoder_sound(reader->sound, reader->samples, MEDIA_SOUND_FRAMES, media->audio->rate);
	if (converted == 0U)
		return 0;

	/* Written as room opens. */
	done = 0;
	while (done < converted) {
		stop = media_stopping(media);
		if (stop)
			return 1;
		kl_mutex_lock(&media->lock);
		stop = media->seek_wanted;
		kl_mutex_unlock(&media->lock);
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
	int error;

	/* A seek asked for. */
	media = reader->media;
	kl_mutex_lock(&media->lock);
	wanted = media->seek_wanted;
	seconds = media->seek_to;
	kl_mutex_unlock(&media->lock);
	if (!wanted)
		return 0;

	/* The file at the key frame before it, the decoders and the sound emptied. */
	error = media_file_seek(reader->file, (int64_t)(seconds * 1000000.0));
	if (error != 0) {
		kl_mutex_lock(&media->lock);
		media->seek_wanted = 0;
		kl_mutex_unlock(&media->lock);
		media_fail(reader, error);
		return 1;
	}

	/* Reset decoded output only after the container seek succeeded; the video read ahead and held back goes too. */
	media_video_reset(reader);
	app_decoder_flush(reader->video);
	if (reader->sound != NULL) {
		app_decoder_flush(reader->sound);
		reader->sound_trimmed = 0;
		error = app_decoder_trim(reader->sound, (int64_t)(seconds * 1000000.0));
		if (error == 0)
			reader->sound_trimmed = 1;
		(void)vp_audio_flush(media->audio);
	}

	/* What decodes before the time sought is passed over. */
	reader->skip_before = seconds;

	/* The clock anchored at the time sought; the pictures dropped again (some may have come meanwhile). */
	kl_mutex_lock(&media->lock);
	media_drop_pictures(media);
	media->clock_time = seconds;
	media->clock_frames = vp_audio_clock_position(media->audio);
	media->clock_us = media_now_us();
	media->eof = 0;
	media->failure = 0;
	if (media->state == VP_FAILED)
		media->state = VP_PAUSED;
	media->seek_wanted = 0;
	kl_mutex_unlock(&media->lock);
	vp_log("SEEK done to_ms=%lld", (long long)(seconds * 1000.0));

	/* Carried out. */
	return 1;
}

/* Publishes a terminal read or decode error; a successful seek may restart the reader. */
static void
media_fail(
	struct media_reader *reader,
	int error)
{
	struct vp_media *media;

	/* The window consumes the reason while the reader waits in the failed state. */
	media = reader->media;
	kl_mutex_lock(&media->lock);
	media->failure = error;
	media->state = VP_FAILED;
	kl_mutex_unlock(&media->lock);
	(void)vp_audio_stop(media->audio);
	vp_log("PLAYBACK failed error=%d", error);
}

/* Frees the pictures that wait (the lock held). */
static void
media_drop_pictures(
	struct vp_media *media)
{
	unsigned index;

	/* Each. */
	for (index = 0; index < VP_PICTURES; index++)
		app_frame_free(&media->pictures[index]);
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
	kl_mutex_lock(&media->lock);
	quit = media->quit;
	kl_mutex_unlock(&media->lock);
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
	kl_thread_sleep(ms);
}
