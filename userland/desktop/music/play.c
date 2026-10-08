/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The player of a song (ws120-p009): a thread reads the file's sound track
 * with mediafile, decodes it with Video Player's add-in of libavcodec
 * (codec.c, opened with dlopen) and writes it, converted to 16-bit stereo
 * at the stream's rate, into the stream's ring (audio.c, libkeiland's sound
 * stream), which paces the reading.  The position is the sound's: what the
 * stream has played since an anchor.  At the end of the file the thread waits for the ring to be
 * played out and says the song ended, for the window to go to the next.
 * Sound that does not decode (PLAY_BAD_MAX packets in a row) or a file
 * that cannot be read any more stops the song and says why (ws177-p021).
 */

#include "play.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The longest a full ring is waited on before looking at the requests again (ms). */
#define PLAY_WAIT_MS		10

/* The sound converted at once at most (frames). */
#define PLAY_FRAMES		8192

/* The packets in a row whose sound does not decode before the song stops. */
#define PLAY_BAD_MAX		16

/*
 * What the thread holds: the player, the file, the decoder and its track,
 * the time before which what decodes is passed over (after a seek), and
 * the packets in a row that did not decode, and the converted sound.
 */
struct play_reader {
	struct mu_player *player;
	struct mf_file *file;
	struct vp_decoder *decoder;
	unsigned track;
	double skip_before;
	unsigned bad;
	int16_t samples[PLAY_FRAMES * 2];
};

static void *play_run(void *argument);
static int play_reader_open(struct play_reader *reader, const char *path);
static void play_reader_close(struct play_reader *reader);
static int play_feed(struct play_reader *reader, const struct mf_packet *packet);
static int play_sound(struct play_reader *reader, double time);
static int play_seek(struct play_reader *reader);
static void play_end(struct play_reader *reader);
static void play_fail(struct play_reader *reader, int failure);
static int play_stopping(struct mu_player *player);
static void play_sleep_ms(unsigned ms);

/*
 * Makes the player and its sound: opens its stream.  Returns 0, or an
 * errno value when there is no sound (the player cannot play then).
 */
int
mu_player_init(
	struct mu_player *player)
{
	int error;

	/* Nothing open. */
	memset(player, 0, sizeof(*player));
	(void)pthread_mutex_init(&player->lock, NULL);
	(void)pthread_cond_init(&player->wake, NULL);

	/* The sound. */
	error = vp_audio_open(&player->audio);
	return error;
}

/*
 * Ends what plays and frees the player.
 */
void
mu_player_release(
	struct mu_player *player)
{
	/* The song, then the sound. */
	mu_player_close(player);
	vp_audio_close(&player->audio);
	(void)pthread_cond_destroy(&player->wake);
	(void)pthread_mutex_destroy(&player->lock);
}

/*
 * Opens a song and plays it from its start.  Returns 0, or an errno
 * value: ENODEV without sound, ENOTSUP when it has no sound the add-in
 * decodes (player->problem then says why, VP_CODEC_*), or the file's.
 */
int
mu_player_open(
	struct mu_player *player,
	const char *path)
{
	struct play_reader *reader;
	int64_t length_us;
	int error;

	/* What played goes; the sound is opened again when its service went or came (WS191). */
	mu_player_close(player);
	vp_audio_renew(&player->audio);
	if (!player->audio.created)
		return ENODEV;

	/* The reader, opened here so that a song that cannot be played is told at once. */
	reader = calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	reader->player = player;
	player->problem = 0;
	player->failure = 0;
	error = play_reader_open(reader, path);
	if (error != 0) {
		play_reader_close(reader);
		free(reader);
		return error;
	}

	/* What is known of it, playing from its start. */
	(void)pthread_mutex_lock(&player->lock);
	player->duration = 0.0;
	length_us = mf_duration_us(reader->file);
	if (length_us > 0)
		player->duration = (double)length_us / 1000000.0;
	player->state = MU_PLAYING;
	player->ended = 0;
	player->draining = 0;
	player->quit = 0;
	player->seek_wanted = 0;
	player->clock_time = 0.0;
	player->clock_frames = vp_audio_clock_position(&player->audio);
	(void)pthread_mutex_unlock(&player->lock);

	/* The thread reads ahead from here. */
	error = pthread_create(&player->thread, NULL, play_run, reader);
	if (error != 0) {
		play_reader_close(reader);
		free(reader);
		player->state = MU_STOPPED;
		return error;
	}

	/* Succeeded: the thread owns the reader, and the sound goes. */
	player->thread_started = 1;
	(void)vp_audio_start(&player->audio);
	return 0;
}

/*
 * Ends the song: the thread, and the sound written.
 */
void
mu_player_close(
	struct mu_player *player)
{
	/* The thread is asked to end, and waited for. */
	if (player->thread_started) {
		(void)pthread_mutex_lock(&player->lock);
		player->quit = 1;
		(void)pthread_cond_broadcast(&player->wake);
		(void)pthread_mutex_unlock(&player->lock);
		(void)pthread_join(player->thread, NULL);
		player->thread_started = 0;
	}

	/* The sound stops and is dropped. */
	(void)vp_audio_stop(&player->audio);
	(void)vp_audio_flush(&player->audio);

	/* Nothing plays. */
	(void)pthread_mutex_lock(&player->lock);
	player->state = MU_STOPPED;
	player->ended = 0;
	(void)pthread_mutex_unlock(&player->lock);
}

/*
 * Plays from where the song is paused.
 */
void
mu_player_play(
	struct mu_player *player)
{
	/* Only a paused song. */
	(void)pthread_mutex_lock(&player->lock);
	if (player->state != MU_PAUSED) {
		(void)pthread_mutex_unlock(&player->lock);
		return;
	}

	/* The position goes on from where it stood. */
	player->clock_frames = vp_audio_clock_position(&player->audio);
	player->state = MU_PLAYING;
	(void)pthread_cond_broadcast(&player->wake);
	(void)pthread_mutex_unlock(&player->lock);

	/* And the sound with it. */
	(void)vp_audio_start(&player->audio);
}

/*
 * Pauses where the song is.
 */
void
mu_player_pause(
	struct mu_player *player)
{
	double now;

	/* Only a playing song. */
	(void)pthread_mutex_lock(&player->lock);
	if (player->state != MU_PLAYING) {
		(void)pthread_mutex_unlock(&player->lock);
		return;
	}

	/* The position stands where it is. */
	now = mu_player_position(player);
	player->state = MU_PAUSED;
	player->clock_time = now;
	(void)pthread_mutex_unlock(&player->lock);

	/* And the sound with it. */
	(void)vp_audio_stop(&player->audio);
}

/*
 * Asks for the song to go to a time (seconds, kept within it).
 */
void
mu_player_seek(
	struct mu_player *player,
	double seconds)
{
	/* Within the song. */
	if (seconds < 0.0)
		seconds = 0.0;
	if (player->duration > 0.0 && seconds > player->duration)
		seconds = player->duration;

	/* The request; the position stands at the time sought until the thread goes on from it. */
	(void)pthread_mutex_lock(&player->lock);
	if (player->state == MU_STOPPED) {
		(void)pthread_mutex_unlock(&player->lock);
		return;
	}

	/* Asked of the thread. */
	player->seek_wanted = 1;
	player->seek_to = seconds;
	player->clock_time = seconds;
	player->ended = 0;
	(void)pthread_cond_broadcast(&player->wake);
	(void)pthread_mutex_unlock(&player->lock);
}

/*
 * Reports the position (seconds): the sound read since the anchor while
 * playing, standing still otherwise.  Called with or without the lock (it
 * reads the anchors only).
 */
double
mu_player_position(
	struct mu_player *player)
{
	uint64_t frames;
	double position;

	/* Standing still. */
	if (player->state != MU_PLAYING || player->seek_wanted || player->audio.rate == 0U)
		return player->clock_time;

	/* The sound heard since the anchor, within the song. */
	frames = vp_audio_clock_position(&player->audio);
	if (frames < player->clock_frames)
		return player->clock_time;
	position = player->clock_time + (double)(frames - player->clock_frames) / (double)player->audio.rate;
	if (player->duration > 0.0 && position > player->duration)
		position = player->duration;
	return position;
}

/*
 * Reports the state (MU_*), and takes the song's end: 1 once when it was
 * played to its end since the last call.
 */
unsigned
mu_player_state(
	struct mu_player *player,
	int *ended)
{
	unsigned state;

	/* Read under the lock; the end is taken. */
	(void)pthread_mutex_lock(&player->lock);
	state = player->state;
	*ended = player->ended;
	player->ended = 0;
	(void)pthread_mutex_unlock(&player->lock);
	return state;
}

/*
 * Takes why the song stopped while it played (MU_FAIL_*), once; 0 for
 * nothing.
 */
int
mu_player_failure(
	struct mu_player *player)
{
	int failure;

	/* Read and taken under the lock. */
	(void)pthread_mutex_lock(&player->lock);
	failure = player->failure;
	player->failure = 0;
	(void)pthread_mutex_unlock(&player->lock);
	return failure;
}

/* The thread: reads, decodes and writes until it is told to end. */
static void *
play_run(
	void *argument)
{
	struct play_reader *reader;
	struct mu_player *player;
	struct mf_packet packet;
	int status;
	int stop;

	/* Until told to end. */
	reader = argument;
	player = reader->player;
	for (;;) {
		/* The end of the thread, or a seek. */
		stop = play_stopping(player);
		if (stop)
			break;
		status = play_seek(reader);
		if (status != 0)
			continue;

		/* The next packet; at the end of the file the song ends when its sound is played out. */
		status = mf_read(reader->file, &packet);
		if (status == ENODATA) {
			play_end(reader);
			continue;
		}

		/* A file that cannot be read any more stops the song. */
		if (status != 0) {
			mu_log("PLAY read error=%d", status);
			play_fail(reader, MU_FAIL_READ);
			continue;
		}

		/* The sound track's packets. */
		if (packet.track == reader->track)
			(void)play_feed(reader, &packet);
	}

	/* The reader goes with the thread. */
	play_reader_close(reader);
	free(reader);
	return NULL;
}

/* Opens the file and the decoder of its first sound track; 0 or an errno value (player->problem says a decoding problem). */
static int
play_reader_open(
	struct play_reader *reader,
	const char *path)
{
	const struct mf_track *track;
	unsigned count;
	unsigned index;
	int status;

	/* The container. */
	status = mf_open(path, &reader->file);
	if (status != 0)
		return status;

	/* The first sound track with a decoder. */
	status = ENOENT;
	count = mf_track_count(reader->file);
	for (index = 0; index < count; index++) {
		/* A sound track. */
		track = mf_track(reader->file, index);
		if (track == NULL || track->kind != MF_TRACK_AUDIO)
			continue;

		/* Its decoder; the add-in missing ends the search. */
		status = vp_decoder_open(track, &reader->decoder);
		if (status == 0) {
			reader->track = index;
			break;
		}

		/* Without the add-in no other track decodes either. */
		if (status == VP_CODEC_MISSING || status == VP_CODEC_VERSION)
			break;
	}

	/* None: why. */
	if (status != 0) {
		reader->player->problem = status;
		if (status == ENOENT || status == ENOMEM)
			reader->player->problem = 0;
		return ENOTSUP;
	}

	/* Succeeded: the log line the tests read. */
	mu_log("PLAY open codec=%s container=%s duration_ms=%lld", vp_decoder_name(reader->decoder),
	    mf_format_name(reader->file), (long long)(mf_duration_us(reader->file) / 1000));
	return 0;
}

/* Closes what the reader opened. */
static void
play_reader_close(
	struct play_reader *reader)
{
	/* The decoder and the file, when they were opened. */
	vp_decoder_close(reader->decoder);
	reader->decoder = NULL;
	if (reader->file != NULL)
		mf_close(reader->file);
	reader->file = NULL;
}

/*
 * Gives the decoder a packet (NULL drains it) and writes what comes out;
 * nonzero when a seek or the end interrupted.
 */
static int
play_feed(
	struct play_reader *reader,
	const struct mf_packet *packet)
{
	int64_t time_us;
	int received;
	int status;
	int tries;

	/* Sent; a full decoder gives its sound first, then takes the packet. */
	status = 0;
	for (tries = 0; tries < 2; tries++) {
		status = vp_decoder_send(reader->decoder, packet);
		if (status != EAGAIN)
			break;

		/* What it holds, written. */
		for (;;) {
			received = vp_decoder_receive(reader->decoder, &time_us);
			if (!received)
				break;
			status = play_sound(reader, (double)time_us / 1000000.0);
			if (status != 0)
				return status;
		}
	}

	/* A packet that does not decode; too many in a row stop the song. */
	if (status != 0 && status != EAGAIN && packet != NULL) {
		reader->bad++;
		if (reader->bad == 1U || reader->bad == PLAY_BAD_MAX)
			mu_log("PLAY decode error=%d bad=%u", status, reader->bad);
		if (reader->bad >= PLAY_BAD_MAX) {
			play_fail(reader, MU_FAIL_DECODE);
			return 1;
		}
	} else if (status == 0) {
		reader->bad = 0U;
	}

	/* What comes out of it. */
	for (;;) {
		received = vp_decoder_receive(reader->decoder, &time_us);
		if (!received)
			return 0;
		status = play_sound(reader, (double)time_us / 1000000.0);
		if (status != 0)
			return status;
	}
}

/* Converts the sound received and writes it, waiting for room; nonzero when a seek or the end came meanwhile. */
static int
play_sound(
	struct play_reader *reader,
	double time)
{
	struct mu_player *player;
	size_t converted;
	size_t written;
	size_t done;
	int stop;

	/* Sound before the time sought is passed over. */
	player = reader->player;
	if (time < reader->skip_before)
		return 0;

	/* Converted. */
	converted = vp_decoder_sound(reader->decoder, reader->samples, PLAY_FRAMES, player->audio.rate);
	if (converted == 0U)
		return 0;

	/* Written as room opens (a paused stream has none until it plays again). */
	done = 0;
	while (done < converted) {
		/* The end of the thread or a seek. */
		stop = play_stopping(player);
		if (stop)
			return 1;
		(void)pthread_mutex_lock(&player->lock);
		stop = player->seek_wanted;
		(void)pthread_mutex_unlock(&player->lock);
		if (stop)
			return 1;

		/* As much as fits. */
		written = vp_audio_write(&player->audio, reader->samples + done * 2U, converted - done);
		done += written;
		if (done < converted)
			play_sleep_ms(PLAY_WAIT_MS);
	}

	/* Written. */
	return 0;
}

/*
 * Carries out a seek asked for: the file at the time sought, the decoder
 * and the sound emptied, the position anchored there.  Returns 1 when one
 * was carried out.
 */
static int
play_seek(
	struct play_reader *reader)
{
	struct mu_player *player;
	double seconds;
	int wanted;

	/* A seek asked for. */
	player = reader->player;
	(void)pthread_mutex_lock(&player->lock);
	wanted = player->seek_wanted;
	seconds = player->seek_to;
	(void)pthread_mutex_unlock(&player->lock);
	if (!wanted)
		return 0;

	/* The file before it, the decoder and the sound emptied. */
	(void)mf_seek(reader->file, (int64_t)(seconds * 1000000.0));
	vp_decoder_flush(reader->decoder);
	(void)vp_audio_flush(&player->audio);

	/* What decodes before the time sought is passed over. */
	reader->skip_before = seconds;

	/* The position anchored at the time sought. */
	(void)pthread_mutex_lock(&player->lock);
	player->clock_time = seconds;
	player->clock_frames = vp_audio_clock_position(&player->audio);
	player->seek_wanted = 0;
	player->ended = 0;
	player->draining = 0;
	(void)pthread_mutex_unlock(&player->lock);
	mu_log("PLAY seek to_ms=%lld", (long long)(seconds * 1000.0));

	/* Carried out. */
	return 1;
}

/*
 * At the end of the file: drains the decoder once, then waits for the stream
 * to have taken all that was written, and says the song ended.
 */
static void
play_end(
	struct play_reader *reader)
{
	struct mu_player *player;
	uint64_t read;
	uint64_t written;

	/* The decoder's last sound, once. */
	player = reader->player;
	if (!player->draining) {
		player->draining = 1;
		(void)play_feed(reader, NULL);
	}

	/* Played out: the end, once; otherwise a little wait. */
	read = vp_audio_read_position(&player->audio);
	written = vp_audio_write_position(&player->audio);
	(void)pthread_mutex_lock(&player->lock);
	if (read >= written && player->state == MU_PLAYING && player->draining == 1) {
		player->draining = 2;
		player->ended = 1;
		player->clock_time = player->duration;
		player->state = MU_PAUSED;
		mu_log("PLAY ended");
	}

	/* The state is let go. */
	(void)pthread_mutex_unlock(&player->lock);

	/* Until the next look. */
	play_sleep_ms(PLAY_WAIT_MS * 5);
}

/*
 * Stops the song because it cannot go on (MU_FAIL_*): the reason is kept
 * for the window, which goes to the next song, and the thread ends.
 */
static void
play_fail(
	struct play_reader *reader,
	int failure)
{
	struct mu_player *player;

	/* Told once, stopped, and the thread asked to end. */
	player = reader->player;
	(void)pthread_mutex_lock(&player->lock);
	player->failure = failure;
	player->state = MU_STOPPED;
	player->quit = 1;
	(void)pthread_mutex_unlock(&player->lock);
	mu_log("PLAY failed reason=%d", failure);
}

/* Tells whether the thread is to end. */
static int
play_stopping(
	struct mu_player *player)
{
	int quit;

	/* Read under the lock. */
	(void)pthread_mutex_lock(&player->lock);
	quit = player->quit;
	(void)pthread_mutex_unlock(&player->lock);
	return quit;
}

/* Sleeps a number of milliseconds. */
static void
play_sleep_ms(
	unsigned ms)
{
	struct timespec wait;

	/* The wait. */
	wait.tv_sec = ms / 1000U;
	wait.tv_nsec = (long)(ms % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
