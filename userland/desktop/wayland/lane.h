/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A lane: resident threads of the compositor that run jobs the main thread
 * gives them, away from the event loop, and hand them back.
 *
 * A job's run is called on a thread of the lane and may wait (a device's
 * open, a file's write and fsync); its done is called on the main thread
 * from kwl_lane_take, which the event loop calls after its wake pipe, and
 * applies the result.  A lane of one thread runs its jobs in the order
 * given; a lane of more threads (a pool) runs as many at once.  A job with
 * a key replaces the job of the same key that has not started yet: the
 * replaced one comes back to the main thread canceled (its drop is called,
 * not its done), so only the latest of a kind is run.
 *
 * The lane writes one byte to the main thread's wake pipe when its
 * finished jobs go from none to some; the main thread takes all of them.
 */

#ifndef KWL_LANE_H
#define KWL_LANE_H

#include "threads.h"

#include <pthread.h>
#include <stdint.h>

/* The most threads one lane has. */
#define KWL_LANE_THREADS_MAX	8U

struct kwl_lane_job;

/*
 * What a job does: run on the lane's thread, done on the main thread with
 * the context kwl_lane_take is given (the job is the main thread's again
 * then, done frees it or keeps it), and drop on the main thread for a job
 * that is not run or not to be applied (replaced, or the lane stopped),
 * which frees it.
 */
struct kwl_lane_ops {
	void (*run)(struct kwl_lane_job *job);
	void (*done)(void *context, struct kwl_lane_job *job);
	void (*drop)(struct kwl_lane_job *job);
};

/*
 * One job, embedded first in a larger record of its own kind.  The lane
 * links it (next) from its submission to its taking; key is 0 for a job
 * that replaces none, and canceled says a job came back without being run
 * because a newer job of its key replaced it or the lane stopped.
 */
struct kwl_lane_job {
	struct kwl_lane_job *next;
	const struct kwl_lane_ops *ops;
	uint64_t key;
	unsigned canceled;
};

/*
 * A lane and what its threads share with the main thread.
 *
 * Under lock: the jobs given and not started (pending, in their order),
 * the jobs finished or canceled and not taken (finished, in the order they
 * ended), how many jobs the threads run now, the stop request, and whether
 * the main thread was woken for the finished ones and has not taken them
 * yet.  work wakes the threads; idle is signaled whenever a job ends, for
 * kwl_lane_finish.  wake_main is the write end of the main thread's wake
 * pipe.  The threads and the name are the starting thread's.
 */
struct kwl_lane {
	pthread_mutex_t lock;
	pthread_cond_t work;
	pthread_cond_t idle;
	struct kwl_lane_job *pending;
	struct kwl_lane_job *pending_tail;
	struct kwl_lane_job *finished;
	struct kwl_lane_job *finished_tail;
	unsigned running;
	unsigned stop;
	unsigned main_woken;
	int wake_main;
	unsigned count;
	struct kwl_thread threads[KWL_LANE_THREADS_MAX];
	unsigned started;
};

int kwl_lane_start(struct kwl_lane *lane, const char *name, unsigned count, int wake_main);
void kwl_lane_submit(struct kwl_lane *lane, struct kwl_lane_job *job, const struct kwl_lane_ops *ops, uint64_t key);
unsigned kwl_lane_take(struct kwl_lane *lane, void *context);
unsigned kwl_lane_busy(struct kwl_lane *lane);
void kwl_lane_wait(struct kwl_lane *lane);
void kwl_lane_finish(struct kwl_lane *lane, void *context);
void kwl_lane_stop(struct kwl_lane *lane);

#endif
