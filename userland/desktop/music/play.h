/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The player of a song (play.c, ws120-p009), apart from music.h so that
 * the view and its host tests do not need Video Player's sound and
 * decoding: they are compiled into Music from userland/desktop/videoplayer
 * (audio.c, codec.c, bitstream.c) with mediafile.
 */

#ifndef MUSIC_PLAY_H
#define MUSIC_PLAY_H

#include "music.h"

#include "userland/desktop/videoplayer/videoplayer.h"

#include <pthread.h>

/*
 * The player: the thread, the sound (libkeiland's stream), and under the lock
 * the state (MU_*), whether the song was played to its end (taken by
 * mu_player_state) and how far the end's draining went (0 not begun, 1
 * the decoder drained, 2 told), the song's length (seconds), the
 * position's anchor (the time at the stream's position heard), a seek asked
 * for, the end of the thread, why the last song could not be played
 * (VP_CODEC_*, 0 for another reason or none), and why the song stopped
 * while it played (MU_FAIL_*, taken by mu_player_failure).
 */
struct mu_player {
	pthread_t thread;
	int thread_started;
	struct vp_audio audio;

	pthread_mutex_t lock;
	pthread_cond_t wake;
	unsigned state;
	int ended;
	int draining;
	double duration;
	double clock_time;
	uint64_t clock_frames;
	int seek_wanted;
	double seek_to;
	int quit;
	int problem;
	int failure;
};

/* Why a song stopped while it played (ws177-p021): its sound did not decode, or its file could not be read. */
#define MU_FAIL_DECODE		1
#define MU_FAIL_READ		2

int mu_player_init(struct mu_player *player);
void mu_player_release(struct mu_player *player);
int mu_player_open(struct mu_player *player, const char *path);
void mu_player_close(struct mu_player *player);
void mu_player_play(struct mu_player *player);
void mu_player_pause(struct mu_player *player);
void mu_player_seek(struct mu_player *player, double seconds);
double mu_player_position(struct mu_player *player);
unsigned mu_player_state(struct mu_player *player, int *ended);
int mu_player_failure(struct mu_player *player);

#endif
