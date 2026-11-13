/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host test of libmedia's wake: "media-wake-test FILE" opens an engine
 * on a media file with a wake function and checks that the engine calls
 * it with its context from its own thread once it opened (the state is no
 * longer MEDIA_OPENING), from the caller's thread within
 * media_engine_play, and never after media_engine_close; then that an
 * engine opened without a wake (NULL) opens and closes as well.
 */

#include "userland/desktop/libmedia/media.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

/* How long the engine may take to open, in seconds. */
#define WAKE_SECONDS	10

/* What the wake function saw, under wake_lock. */
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_cond = PTHREAD_COND_INITIALIZER;
static int wakes;
static int wakes_off_thread;
static int wakes_on_thread;
static int wrong_context;

/* The test's thread, and the context given to the engine. */
static pthread_t test_thread;
static int wake_tag;

/* How many checks failed. */
static int failures;

static void check(int good, const char *what);
static void wake(void *context);
static int wake_count(void);
static int wait_opened(struct media_engine *engine, struct media_status *status);
static void pause_ms(long milliseconds);

int
main(
	int argc,
	char **argv)
{
	struct media_engine *engine;
	struct media_status status;
	int before;
	int error;

	/* One argument: the file. */
	if (argc != 2) {
		fprintf(stderr, "usage: media-wake-test FILE\n");
		return 2;
	}
	test_thread = pthread_self();

	/* Opened with a wake: it rings from the engine's thread once the file is open. */
	error = media_engine_open(argv[1], NULL, 0U, NULL, wake, &wake_tag, &engine);
	check(error == 0, "open with a wake");
	if (error != 0)
		return 1;
	error = wait_opened(engine, &status);
	check(error == 0, "opened in time");
	check(status.state == MEDIA_PAUSED || status.state == MEDIA_FAILED, "opened: paused or failed");
	pause_ms(100);
	pthread_mutex_lock(&wake_lock);
	check(wakes >= 1, "opened: the wake was called");
	check(wakes_off_thread >= 1, "opened: from the engine's thread");
	check(!wrong_context, "the wake's context");
	pthread_mutex_unlock(&wake_lock);

	/* Played: the wake from this thread, within media_engine_play. */
	if (status.state == MEDIA_PAUSED) {
		pthread_mutex_lock(&wake_lock);
		before = wakes_on_thread;
		pthread_mutex_unlock(&wake_lock);
		media_engine_play(engine);
		pthread_mutex_lock(&wake_lock);
		check(wakes_on_thread > before, "play: the wake from the caller's thread");
		pthread_mutex_unlock(&wake_lock);
		pause_ms(200);
		media_engine_status(engine, &status);
	}

	/* Closed: no wake after. */
	media_engine_close(engine);
	before = wake_count();
	pause_ms(200);
	check(wake_count() == before, "close: no wake after");

	/* Opened without a wake: it opens and closes all the same. */
	error = media_engine_open(argv[1], NULL, 0U, NULL, NULL, NULL, &engine);
	check(error == 0, "open without a wake");
	if (error == 0) {
		before = wake_count();
		error = wait_opened(engine, &status);
		check(error == 0, "no wake: opened in time");
		check(wake_count() == before, "no wake: nothing called");
		media_engine_close(engine);
	}

	/* The verdict. */
	if (failures != 0) {
		printf("media-wake-test: %d failed\n", failures);
		return 1;
	}
	printf("media-wake-test: PASS\n");
	return 0;
}

static void
check(
	int good,
	const char *what)
{
	if (good)
		return;
	printf("FAIL %s\n", what);
	failures++;
}

/* The engine's wake: counted by the thread it came from, and the waiting test wakes. */
static void
wake(
	void *context)
{
	pthread_mutex_lock(&wake_lock);
	wakes++;
	if (pthread_equal(pthread_self(), test_thread))
		wakes_on_thread++;
	else
		wakes_off_thread++;
	if (context != &wake_tag)
		wrong_context = 1;
	pthread_cond_broadcast(&wake_cond);
	pthread_mutex_unlock(&wake_lock);
}

/* How many times the wake was called. */
static int
wake_count(void)
{
	int count;

	pthread_mutex_lock(&wake_lock);
	count = wakes;
	pthread_mutex_unlock(&wake_lock);
	return count;
}

/*
 * Waits until the engine is no longer opening: after each wake (or every
 * 100 ms, for an engine without one) its state is read.  Returns 0, or
 * ETIMEDOUT after WAKE_SECONDS.
 */
static int
wait_opened(
	struct media_engine *engine,
	struct media_status *status)
{
	struct timespec deadline;
	time_t end;

	end = time(NULL) + WAKE_SECONDS;
	for (;;) {
		media_engine_status(engine, status);
		if (status->state != MEDIA_OPENING)
			return 0;
		if (time(NULL) > end)
			return ETIMEDOUT;
		pthread_mutex_lock(&wake_lock);
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
		(void)pthread_cond_timedwait(&wake_cond, &wake_lock, &deadline);
		pthread_mutex_unlock(&wake_lock);
	}
}

static void
pause_ms(
	long milliseconds)
{
	struct timespec time;

	time.tv_sec = milliseconds / 1000;
	time.tv_nsec = (milliseconds % 1000) * 1000000L;
	nanosleep(&time, NULL);
}
