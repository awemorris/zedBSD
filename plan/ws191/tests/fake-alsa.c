/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws191-p004: a stand-in of alsa-lib for the host test of the Linux pump
 * (host-pump.c): the calls audio-stream-linux.c finds with dlsym, over one
 * simulated PCM of a 1920-frame buffer and 480-frame periods.  Nothing
 * plays by itself: the test plays frames with fake_alsa_play.  A running
 * PCM that plays out what it holds goes to XRUN, as alsa-lib's does.
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_BUFFER	1920UL
#define FAKE_PERIOD	480UL
#define STATE_PREPARED	2
#define STATE_RUNNING	3
#define STATE_XRUN	4
#define STATE_PAUSED	6

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static int fake_state = STATE_PREPARED;
static unsigned long fake_queued;
static unsigned long fake_threshold = FAKE_BUFFER;
static unsigned long fake_written;
static unsigned long fake_played;
static int fake_fail_open;
static int fake_opened;
static int fake_dummy;

/* The test's controls. */
void fake_alsa_fail_open(int error) { pthread_mutex_lock(&fake_lock); fake_fail_open = error; pthread_mutex_unlock(&fake_lock); }
unsigned long fake_alsa_written(void) { unsigned long v; pthread_mutex_lock(&fake_lock); v = fake_written; pthread_mutex_unlock(&fake_lock); return v; }
unsigned long fake_alsa_queued(void) { unsigned long v; pthread_mutex_lock(&fake_lock); v = fake_queued; pthread_mutex_unlock(&fake_lock); return v; }
int fake_alsa_state(void) { int v; pthread_mutex_lock(&fake_lock); v = fake_state; pthread_mutex_unlock(&fake_lock); return v; }
int fake_alsa_opened(void) { int v; pthread_mutex_lock(&fake_lock); v = fake_opened; pthread_mutex_unlock(&fake_lock); return v; }

/* Plays up to frames of what the PCM holds, while running. */
void
fake_alsa_play(unsigned long frames)
{
	pthread_mutex_lock(&fake_lock);
	if (fake_state == STATE_RUNNING) {
		if (frames > fake_queued)
			frames = fake_queued;
		fake_queued -= frames;
		fake_played += frames;
		if (fake_queued == 0UL)
			fake_state = STATE_XRUN;
	}
	pthread_mutex_unlock(&fake_lock);
}

int
snd_pcm_open(void **pcm, const char *name, int stream, int mode)
{
	int error;

	(void)name; (void)stream; (void)mode;
	pthread_mutex_lock(&fake_lock);
	error = fake_fail_open;
	fake_fail_open = 0;
	if (error == 0) {
		fake_opened++;
		fake_state = STATE_PREPARED;
		fake_queued = 0UL;
	}
	pthread_mutex_unlock(&fake_lock);
	if (error != 0)
		return -error;
	*pcm = &fake_dummy;
	return 0;
}

int snd_pcm_set_params(void *pcm, int format, int access, unsigned channels, unsigned rate, int soft_resample, unsigned latency)
{ (void)pcm; (void)format; (void)access; (void)channels; (void)rate; (void)soft_resample; (void)latency; return 0; }

int snd_pcm_get_params(void *pcm, unsigned long *buffer_size, unsigned long *period_size)
{ (void)pcm; *buffer_size = FAKE_BUFFER; *period_size = FAKE_PERIOD; return 0; }

long
snd_pcm_writei(void *pcm, const void *buffer, unsigned long size)
{
	unsigned long room;

	(void)pcm; (void)buffer;
	pthread_mutex_lock(&fake_lock);
	if (fake_state == STATE_XRUN) {
		pthread_mutex_unlock(&fake_lock);
		return -EPIPE;
	}
	room = FAKE_BUFFER - fake_queued;
	if (size > room)
		size = room;
	fake_queued += size;
	fake_written += size;
	if (fake_state == STATE_PREPARED && fake_queued >= fake_threshold)
		fake_state = STATE_RUNNING;
	pthread_mutex_unlock(&fake_lock);
	if (size == 0UL)
		return -EAGAIN;
	return (long)size;
}

long
snd_pcm_avail_update(void *pcm)
{
	long avail;

	(void)pcm;
	pthread_mutex_lock(&fake_lock);
	avail = (long)(FAKE_BUFFER - fake_queued);
	if (fake_state == STATE_XRUN)
		avail = -EPIPE;
	pthread_mutex_unlock(&fake_lock);
	return avail;
}

int
snd_pcm_delay(void *pcm, long *delay)
{
	(void)pcm;
	pthread_mutex_lock(&fake_lock);
	*delay = (long)fake_queued;
	pthread_mutex_unlock(&fake_lock);
	return 0;
}

int
snd_pcm_pause(void *pcm, int enable)
{
	(void)pcm;
	pthread_mutex_lock(&fake_lock);
	fake_state = enable ? STATE_PAUSED : STATE_RUNNING;
	pthread_mutex_unlock(&fake_lock);
	return 0;
}

int snd_pcm_drop(void *pcm) { (void)pcm; pthread_mutex_lock(&fake_lock); fake_queued = 0UL; fake_state = 1; pthread_mutex_unlock(&fake_lock); return 0; }
int snd_pcm_prepare(void *pcm) { (void)pcm; pthread_mutex_lock(&fake_lock); fake_queued = 0UL; fake_state = STATE_PREPARED; pthread_mutex_unlock(&fake_lock); return 0; }

int
snd_pcm_start(void *pcm)
{
	(void)pcm;
	pthread_mutex_lock(&fake_lock);
	if (fake_state == STATE_PREPARED && fake_queued > 0UL)
		fake_state = STATE_RUNNING;
	pthread_mutex_unlock(&fake_lock);
	return 0;
}

int snd_pcm_state(void *pcm) { (void)pcm; return fake_alsa_state(); }
int snd_pcm_recover(void *pcm, int error, int silent) { (void)error; (void)silent; return snd_pcm_prepare(pcm); }
int snd_pcm_poll_descriptors_count(void *pcm) { (void)pcm; return 0; }
int snd_pcm_poll_descriptors(void *pcm, struct pollfd *descriptors, unsigned space) { (void)pcm; (void)descriptors; (void)space; return 0; }
int snd_pcm_close(void *pcm) { (void)pcm; pthread_mutex_lock(&fake_lock); fake_opened--; pthread_mutex_unlock(&fake_lock); return 0; }
int snd_pcm_sw_params_malloc(void **params) { *params = malloc(8); return *params == NULL ? -ENOMEM : 0; }
int snd_pcm_sw_params_current(void *pcm, void *params) { (void)pcm; (void)params; return 0; }
int snd_pcm_sw_params_set_start_threshold(void *pcm, void *params, unsigned long value) { (void)pcm; (void)params; pthread_mutex_lock(&fake_lock); fake_threshold = value; pthread_mutex_unlock(&fake_lock); return 0; }
int snd_pcm_sw_params(void *pcm, void *params) { (void)pcm; (void)params; return 0; }
void snd_pcm_sw_params_free(void *params) { free(params); }
