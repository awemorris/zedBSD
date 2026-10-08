/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p002: what the two halves of the host test of the sound's
 * playback streams share (host-audio-server.c, host-audio-client.c).
 */

#ifndef WS191_HOST_AUDIO_H
#define WS191_HOST_AUDIO_H

#include <pthread.h>
#include <stdint.h>

/* The stand-in backend's counts and the test's commands, under lock. */
struct host_audio_world {
	pthread_mutex_t lock;
	unsigned opens;
	unsigned closes;
	unsigned controls;
	unsigned protocol_errors;
	unsigned fail_next_open;	/* a KL_BACKEND_AUDIO_ERROR_* for the next open's FAILED */
	int refuse_next_control;	/* an errno value the next control returns */
	uint64_t play_frames;		/* frames every running stream plays at the next pass */
	unsigned underrun;		/* a count every stream reports at the next pass */
	unsigned drained;		/* every draining stream completes at the next pass */
	unsigned lose;			/* every stream is lost at the next pass */
	unsigned close_on_create;	/* the next create_stream's connection is closed (the compositor went) */
	unsigned bad_version;		/* the next ring says another version */
	unsigned stop;
};

extern struct host_audio_world host_audio;

void host_audio_server_start(const char *path);
void host_audio_server_stop(void);

#endif
