/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lanes (lane.h): resident threads that run the main thread's jobs and
 * hand them back through one list and one byte on the main thread's wake
 * pipe.  The main thread never waits for a lane while the event loop runs:
 * a submission and a taking each hold the lane's lock for a few list
 * operations, and a thread holds it only between its jobs.
 */

#include "lane.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void *lane_main(void *argument);
static void lane_finished_append(struct kwl_lane *lane, struct kwl_lane_job *job, unsigned *wake);
static void lane_cancel_key(struct kwl_lane *lane, uint64_t key, unsigned *wake);
static void lane_drop_list(struct kwl_lane_job *job);

/*
 * Starts a lane of count threads (1 to KWL_LANE_THREADS_MAX) under a name;
 * a lane of several threads names them name-1, name-2 and so on.  wake_main
 * is the write end of the main thread's wake pipe.  Returns 0, or the
 * errno value of making the lock, a signal or a thread; the lane is then
 * not running and needs no stop.
 */
int
kwl_lane_start(
	struct kwl_lane *lane,
	const char *name,
	unsigned count,
	int wake_main)
{
	char thread_name[KWL_THREAD_NAME];
	unsigned index;
	int error;

	/* A lane has at least one thread and at most the threads it can hold. */
	memset(lane, 0, sizeof(*lane));
	if (count == 0U || count > KWL_LANE_THREADS_MAX)
		return EINVAL;
	lane->wake_main = wake_main;

	/* The lock of the lists. */
	error = pthread_mutex_init(&lane->lock, NULL);
	if (error != 0)
		return error;

	/* The signal of a job given or of the stop. */
	error = pthread_cond_init(&lane->work, NULL);
	if (error != 0) {
		pthread_mutex_destroy(&lane->lock);
		return error;
	}

	/* The signal of a job ended, for the finish at the end of the session. */
	error = pthread_cond_init(&lane->idle, NULL);
	if (error != 0) {
		pthread_cond_destroy(&lane->work);
		pthread_mutex_destroy(&lane->lock);
		return error;
	}

	/* started makes a failed start below stop the threads already made. */
	lane->started = 1U;

	/* The threads, each waiting for the first job. */
	for (index = 0U; index < count; index++) {
		/* A lone thread takes the lane's name; a pool's are numbered from 1. */
		if (count == 1U) {
			(void)snprintf(thread_name, sizeof(thread_name), "%s", name);
		} else {
			(void)snprintf(thread_name, sizeof(thread_name), "%s-%u", name, index + 1U);
		}

		/* The thread; a failure stops the ones made so far. */
		error = kwl_thread_start(&lane->threads[index], thread_name, lane_main, lane);
		if (error != 0) {
			kwl_lane_stop(lane);
			return error;
		}

		/* The thread is the lane's to stop. */
		lane->count = index + 1U;
	}

	/* Succeeded: the lane runs the jobs it is given. */
	return 0;
}

/*
 * Gives a lane a job to run, with its operations and its key (0: none).
 * A job of a key replaces every job of the same key not started yet: those
 * come back canceled.  The job belongs to the lane until kwl_lane_take
 * hands it back.
 */
void
kwl_lane_submit(
	struct kwl_lane *lane,
	struct kwl_lane_job *job,
	const struct kwl_lane_ops *ops,
	uint64_t key)
{
	unsigned wake;

	/* The job as the lane keeps it. */
	job->next = NULL;
	job->ops = ops;
	job->key = key;
	job->canceled = 0U;

	/* The older jobs of its key leave, and it joins the end of the jobs not started. */
	wake = 0U;
	pthread_mutex_lock(&lane->lock);

	if (key != 0U)
		lane_cancel_key(lane, key, &wake);
	if (lane->pending_tail == NULL) {
		lane->pending = job;
	} else {
		lane->pending_tail->next = job;
	}

	/* The job is now the last of those not started, and the thread is woken. */
	lane->pending_tail = job;
	pthread_cond_signal(&lane->work);

	pthread_mutex_unlock(&lane->lock);

	/* The canceled jobs come back to the main thread at its next pass. */
	if (wake)
		kwl_thread_wake(lane->wake_main);
}

/*
 * Takes every job the lane finished or canceled since the last taking, on
 * the main thread: done for a job run, drop for a job canceled, in the
 * order they ended, outside the lane's lock.  Returns how many were taken.
 */
unsigned
kwl_lane_take(
	struct kwl_lane *lane,
	void *context)
{
	struct kwl_lane_job *job;
	struct kwl_lane_job *next;
	unsigned taken;

	/* A lane that is not running has nothing. */
	if (!lane->started)
		return 0U;

	/*
	 * The finished jobs leave the lane at once.  main_woken goes down
	 * under the same lock, so a job finished after this wakes the main
	 * thread again.
	 */
	pthread_mutex_lock(&lane->lock);

	job = lane->finished;
	lane->finished = NULL;
	lane->finished_tail = NULL;
	lane->main_woken = 0U;

	pthread_mutex_unlock(&lane->lock);

	/* Each job applied or freed, in its order. */
	taken = 0U;
	while (job != NULL) {
		next = job->next;
		job->next = NULL;
		if (job->canceled) {
			job->ops->drop(job);
		} else {
			job->ops->done(context, job);
		}

		/* Counts the job taken and goes on to the next. */
		taken++;
		job = next;
	}

	/* Reports the jobs taken. */
	return taken;
}

/*
 * Tells whether a lane has jobs not finished: given and not started, or
 * running now.  The answer is a moment's.
 */
unsigned
kwl_lane_busy(
	struct kwl_lane *lane)
{
	unsigned busy;

	/* A lane that is not running has none. */
	if (!lane->started)
		return 0U;

	/* The jobs waiting and the jobs running. */
	pthread_mutex_lock(&lane->lock);

	busy = 0U;
	if (lane->pending != NULL)
		busy = 1U;
	if (lane->running != 0U)
		busy = 1U;

	pthread_mutex_unlock(&lane->lock);

	/* Reports whether any job is not finished. */
	return busy;
}

/*
 * Waits until the lane has run every job given to it, without taking them:
 * any thread may wait so for another lane (the display thread for the
 * present thread's present), and the main thread only where waiting is the
 * point.
 */
void
kwl_lane_wait(
	struct kwl_lane *lane)
{
	/* A lane that is not running has nothing to run. */
	if (!lane->started)
		return;

	/* Every job given has ended. */
	pthread_mutex_lock(&lane->lock);

	while (lane->pending != NULL || lane->running != 0U)
		pthread_cond_wait(&lane->idle, &lane->lock);

	pthread_mutex_unlock(&lane->lock);
}

/*
 * Waits until the lane has run every job given to it, then takes them
 * (kwl_lane_take): only at the end of the session, after the event loop,
 * where waiting for a file's write is the point.
 */
void
kwl_lane_finish(
	struct kwl_lane *lane,
	void *context)
{
	/* A lane that is not running has nothing to finish. */
	if (!lane->started)
		return;

	/* Every job given has ended. */
	kwl_lane_wait(lane);

	/* What they came to. */
	(void)kwl_lane_take(lane, context);
}

/*
 * Stops a lane and waits for its threads: the jobs running are finished,
 * and every job not started or not taken is dropped (its drop frees it).
 */
void
kwl_lane_stop(
	struct kwl_lane *lane)
{
	unsigned index;

	/* A lane never started has nothing to stop. */
	if (!lane->started)
		return;

	/* The request, seen by each thread when it next looks for a job. */
	pthread_mutex_lock(&lane->lock);

	lane->stop = 1U;
	pthread_cond_broadcast(&lane->work);

	pthread_mutex_unlock(&lane->lock);

	/* The threads end; nothing touches the lane but this thread from here. */
	for (index = 0U; index < lane->count; index++)
		kwl_thread_join(&lane->threads[index]);
	lane->count = 0U;
	lane->started = 0U;

	/* The jobs not started and the jobs not taken. */
	lane_drop_list(lane->pending);
	lane_drop_list(lane->finished);
	lane->pending = NULL;
	lane->pending_tail = NULL;
	lane->finished = NULL;
	lane->finished_tail = NULL;

	/* The lock and the signals. */
	pthread_cond_destroy(&lane->idle);
	pthread_cond_destroy(&lane->work);
	pthread_mutex_destroy(&lane->lock);
}

/* Runs a lane's jobs on one of its threads until the stop. */
static void *
lane_main(
	void *argument)
{
	struct kwl_lane *lane;
	struct kwl_lane_job *job;
	unsigned wake;

	/* The lane's record. */
	lane = argument;

	/* Each job in the order given, until the stop. */
	for (;;) {
		/* The next job, or the stop; running counts the job from here. */
		pthread_mutex_lock(&lane->lock);

		while (lane->pending == NULL && !lane->stop)
			pthread_cond_wait(&lane->work, &lane->lock);
		job = NULL;
		if (!lane->stop) {
			job = lane->pending;
			lane->pending = job->next;
			if (lane->pending == NULL)
				lane->pending_tail = NULL;
			job->next = NULL;
			lane->running++;
		}

		pthread_mutex_unlock(&lane->lock);

		/* The stop ends the thread; the jobs left are the stopping thread's to drop. */
		if (job == NULL)
			break;

		/* The job's work, away from every lock of the lane. */
		job->ops->run(job);

		/* The job joins the finished ones; the main thread is woken unless it was already. */
		wake = 0U;
		pthread_mutex_lock(&lane->lock);

		lane->running--;
		lane_finished_append(lane, job, &wake);
		pthread_cond_broadcast(&lane->idle);

		pthread_mutex_unlock(&lane->lock);

		/* The main thread's poll returns. */
		if (wake)
			kwl_thread_wake(lane->wake_main);
	}

	/* The thread ends at the stop. */
	return NULL;
}

/*
 * Puts a job at the end of the finished ones, under the lane's lock, and
 * says through wake whether the main thread is to be woken (it was not
 * since it last took them).
 */
static void
lane_finished_append(
	struct kwl_lane *lane,
	struct kwl_lane_job *job,
	unsigned *wake)
{
	/* The job is the last finished. */
	job->next = NULL;
	if (lane->finished_tail == NULL) {
		lane->finished = job;
	} else {
		lane->finished_tail->next = job;
	}

	/* The job is now the last finished one. */
	lane->finished_tail = job;

	/* main_woken holds one byte on the pipe for every finished job until the main thread takes them. */
	if (!lane->main_woken) {
		lane->main_woken = 1U;
		*wake = 1U;
	}
}

/*
 * Moves every job of a key not started yet to the finished ones as
 * canceled, under the lane's lock.
 */
static void
lane_cancel_key(
	struct kwl_lane *lane,
	uint64_t key,
	unsigned *wake)
{
	struct kwl_lane_job *previous;
	struct kwl_lane_job *job;
	struct kwl_lane_job *next;

	/* Each job waiting, the ones of the key taken out of the list. */
	previous = NULL;
	job = lane->pending;
	while (job != NULL) {
		next = job->next;
		if (job->key != key) {
			previous = job;
			job = next;
			continue;
		}

		/* The job leaves the jobs not started. */
		if (previous == NULL) {
			lane->pending = next;
		} else {
			previous->next = next;
		}

		/* The tail moves back when the job was the last one. */
		if (lane->pending_tail == job)
			lane->pending_tail = previous;

		/* canceled tells the main thread to drop it, not to apply it. */
		job->canceled = 1U;
		lane_finished_append(lane, job, wake);
		job = next;
	}
}

/* Drops every job of a list (the lane stopped before they were run or taken). */
static void
lane_drop_list(
	struct kwl_lane_job *job)
{
	struct kwl_lane_job *next;

	/* Each job frees itself. */
	while (job != NULL) {
		next = job->next;
		job->next = NULL;
		job->ops->drop(job);
		job = next;
	}
}
