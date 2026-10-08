/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia (ws121-p002): the playing of a media file the desktop's
 * programs share -- first the browser's <video> and <audio>.  It holds the
 * container reader (userland/desktop/mediafile), Video Player's decoding
 * add-in (FFmpeg's libavcodec opened with dlopen, without its headers),
 * and an engine that plays one file: a thread reads and decodes ahead (the
 * file's bytes come from a path or from the caller's source, read on that
 * thread) and keeps a few pictures; the caller takes the picture whose
 * time has come and scales it into its own pixels.  No sound is played
 * (WS191: the library may not reach Keiland's sound streams; its users
 * get a sound output of their own later): a file plays by the monotonic
 * clock, and one without pictures (a song) plays silent.
 *
 * The engine never calls back: it makes a descriptor readable when its
 * state changed or a picture came (media_engine_wake_fd), for the caller's
 * poll; the caller then reads the state (media_engine_status), which also
 * empties the descriptor.  Everything but the reading of the bytes runs
 * on the caller's thread or the engine's own.
 *
 * The library's internal API, for the desktop's programs; not a part of
 * any SDK.
 */

#ifndef LIBMEDIA_MEDIA_H
#define LIBMEDIA_MEDIA_H

#include "userland/desktop/mediafile/mediafile.h"

#include <stddef.h>
#include <stdint.h>

/* An engine's states. */
#define MEDIA_OPENING		0U	/* the file is being opened on the engine's thread */
#define MEDIA_PAUSED		1U	/* open, standing still */
#define MEDIA_PLAYING		2U
#define MEDIA_ENDED		3U	/* played to its end */
#define MEDIA_FAILED		4U	/* could not be opened or played (status.error, status.problem) */

/* What an engine is asked to play (media_engine_open). */
#define MEDIA_SOUND		1U	/* its sound is wanted (none is played for now, WS191) */

/*
 * An engine's state as the caller reads it: the state (MEDIA_*), why it
 * failed (an errno value) and whether the decoding add-in was the reason
 * (VP_CODEC_*, 0 for none), the picture's size (0 without pictures),
 * whether there are pictures and sound, the length and the position
 * (seconds), and the number of the latest picture taken (it grows by one
 * each time media_engine_picture takes a newer one).
 */
struct media_status {
	unsigned state;
	int error;
	int problem;
	int width;
	int height;
	int has_video;
	int has_audio;
	double duration;
	double position;
	uint64_t picture;
};

struct media_engine;

/* The log: a line of the library's (without its end) goes to the caller's function, or nowhere. */
void media_set_log(void (*log)(void *context, const char *line), void *context);

/* The engine (engine.c). */
int media_engine_open(const char *path, const struct mf_source *source, unsigned flags, struct media_engine **engine);
int media_engine_wake_fd(const struct media_engine *engine);
void media_engine_status(struct media_engine *engine, struct media_status *status);
void media_engine_play(struct media_engine *engine);
void media_engine_pause(struct media_engine *engine);
void media_engine_seek(struct media_engine *engine, double seconds);
int media_engine_picture(struct media_engine *engine, uint32_t *pixels, size_t stride, int width, int height, double *next);
int media_engine_redraw(struct media_engine *engine, uint32_t *pixels, size_t stride, int width, int height);
void media_engine_close(struct media_engine *engine);

#endif
