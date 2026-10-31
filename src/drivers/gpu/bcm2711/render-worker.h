/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One private ordered execution queue retains callbacks independently of native DMA. */
#ifndef KERN_DRIVERS_GPU_BCM2711_RENDER_WORKER_H
#define KERN_DRIVERS_GPU_BCM2711_RENDER_WORKER_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/waitq.h>

/* Decoder commands own the request slots; producer markers own a separate pool. */
#define BCM2711_RENDER_REQUESTS 16U
#define BCM2711_RENDER_MARKERS 96U

/*
 * Backpressure: a producer marker is reserved only while its session and the
 * engine have room for one more execution of the largest submission.  Each
 * open session may always hold the minimum, whatever the others hold, so the
 * marker pool covers the minimum of every possible session.
 */
#define BCM2711_RENDER_SESSIONS 32U
#define BCM2711_RENDER_MINIMUM 2U
#define BCM2711_RENDER_MINIMUM_BYTES (16U * 1024U * 1024U)
#define BCM2711_RENDER_SESSION_EXECUTIONS 12U
#define BCM2711_RENDER_SESSION_BYTES (48U * 1024U * 1024U)
#define BCM2711_RENDER_EXECUTIONS 64U
#define BCM2711_RENDER_BYTES (UINT64_C(192) * 1024U * 1024U)
#define BCM2711_RENDER_SUBMISSION_BYTES (8U * 1024U * 1024U)

/* A stream that submits without markers is refused past twice the session's limits. */
#define BCM2711_RENDER_HARD_EXECUTIONS (2U * BCM2711_RENDER_SESSION_EXECUTIONS)
#define BCM2711_RENDER_HARD_BYTES (2U * BCM2711_RENDER_SESSION_BYTES)

struct thread;
struct drv_gpu_completion;
struct drv_gpu_ops;
struct bcm2711_render_device;
struct bcm2711_render_session;

/* Execution and disposal run under the controller mutex; callbacks run without it. */
typedef int (*bcm2711_render_execute)(struct bcm2711_render_device *, struct bcm2711_render_session *, void *, bool *);
typedef int (*bcm2711_render_dispose)(struct bcm2711_render_device *, void *, bool);

struct bcm2711_render_execution;

/*
 * One step of an execution (at most one native pass) runs under the controller
 * mutex and reports whether the execution has finished; an error ends it.  The
 * end publishes the outcome (zero, the step's error, ECANCELED for a stopped
 * session or EIO for a faulted engine), releases the execution's owners and
 * frees it, also under the mutex, and returns a release failure.  The executor
 * turns a step's error or a release failure into a global fault; a canceled
 * execution affects only its session.
 */
typedef int (*bcm2711_render_execution_step)(struct bcm2711_render_device *, struct bcm2711_render_execution *, bool *);
typedef int (*bcm2711_render_execution_end)(struct bcm2711_render_device *, struct bcm2711_render_execution *, int);

/*
 * Tells whether an execution whose actions all ran may end now: zero when it
 * may, EINPROGRESS while work it started outside the engine (a video decode's
 * frame) has not come, another error when looking failed.  The flag lets it
 * wait a little for that work.  It runs under the controller mutex.
 */
typedef int (*bcm2711_render_execution_settle)(struct bcm2711_render_device *, struct bcm2711_render_execution *, bool);

/*
 * One accepted QueueSubmit's work in its session's execution queue.  The
 * native IRQ guard protects the links; the executor alone runs and ends it.
 *
 * An execution with a settle leaves the queue once its actions have run, so
 * the session's next execution runs, and waits in the session's settling
 * list until the settle lets it end.
 */
struct bcm2711_render_execution {
	struct bcm2711_render_execution *next;
	struct bcm2711_render_session *session;
	bcm2711_render_execution_step step;
	bcm2711_render_execution_end end;
	/* Asked after the last step when set (see bcm2711_render_execution_settle). */
	bcm2711_render_execution_settle settle;
	/* The queue domain of the submission, whose reserved marker completes it; zero for none. */
	uint32_t timeline;
	/* The execution's number in its session, from 1 in submission order; set when it is queued. */
	uint64_t ordinal;
	/* The staging bytes the execution's prepared work may hold, for the session's limit. */
	uint64_t bytes;
	/* The execution uses a run the display holds (a frame, a present copy, the cursor): it runs first. */
	bool display;
	/*
	 * Set by a failed step whose failure is the engine's (a launched pass,
	 * translations that could not be published, uncertain retirement); a step
	 * that failed while preparing, before anything launched, clears it and
	 * only this execution fails.
	 */
	bool engine;
};

/* The native IRQ guard protects every slot field while its prepared payload remains owned. */
struct bcm2711_render_request {
	struct bcm2711_render_request *next;
	struct bcm2711_render_session *session;
	struct drv_gpu_completion *completion;
	bcm2711_render_execute execute;
	bcm2711_render_dispose dispose;
	void *payload;
	/* A supervised callback retains its old queue domain through final callback retirement. */
	uint32_t timeline;
	uint32_t state;
	int canceled;
	bool supervised;
	/* Diagnostic tick count when the request entered the FIFO. */
	uint64_t queued_ticks;
	/* A parked domain completion is released once its session has executed this many executions. */
	uint64_t required;
	/*
	 * The ordinal of the execution a marker completes, found when that
	 * submission was queued; zero when it is not known.  A settling
	 * execution holds its own marker, or, not known, every marker after it.
	 */
	uint64_t ordinal;
	struct bcm2711_render_request *parked_next;
};

/* The native IRQ guard protects the FIFO, slots, pending counts and uncertain flag. */
struct bcm2711_render_worker {
	struct bcm2711_render_request requests[BCM2711_RENDER_REQUESTS];
	struct bcm2711_render_request markers[BCM2711_RENDER_MARKERS];
	/*
	 * Backpressure counts: open sessions, the sum over sessions of the
	 * markers each holds up to the minimum, and the unfinished executions
	 * and their bytes of all sessions.
	 */
	uint32_t sessions;
	uint32_t covered;
	uint32_t executions;
	uint64_t execution_bytes;
	struct bcm2711_render_request *head;
	struct bcm2711_render_request *tail;
	struct thread *thread;
	struct wait_queue available;
	struct wait_queue retired;
	bool uncertain;
	/*
	 * The executor: sessions with queued executions in round-robin order,
	 * the domain completions parked until their sessions' executions finish, its thread
	 * and its wake-up.  The native IRQ guard protects the lists.
	 */
	struct bcm2711_render_session *ready_head;
	struct bcm2711_render_session *ready_tail;
	/* The last step ran a display execution: the next turn goes to another session if one is ready. */
	bool display_last;
	/*
	 * Set by the executor before each step, under the native IRQ guard:
	 * another ready session than the one stepped has an execution queued.
	 * A step that would wait for the decoder (or a settling execution) then
	 * only looks, so the executor goes on with the others' work.
	 */
	bool contended;
	struct bcm2711_render_request *parked;
	struct thread *executor;
	struct wait_queue executable;
	/*
	 * The executor's window: threads waiting to map (counted while they wait
	 * and map) and how many have finished, and the wake-up of both the
	 * mappers waiting for the window to close and the executor waiting for
	 * the mappers that came before it.
	 */
	uint32_t pte_waiters;
	uint64_t pte_done;
	struct wait_queue window_queue;
	/* Failure lines the kernel log has taken, bounded so a failing client cannot flood it; the controller mutex protects it. */
	uint32_t failure_lines;
	/*
	 * Diagnostics of the executor's hold of the controller mutex: when it
	 * last gave the mutex up inside a step, and the ticks it went without
	 * it, which a step's hold excludes.  Only the executor touches them.
	 */
	uint64_t released_at;
	uint64_t released_ticks;
};

void bcm2711_render_jobs_bind(struct drv_gpu_ops *operations);
void bcm2711_render_worker_init(struct bcm2711_render_device *controller);
int bcm2711_render_worker_start(struct bcm2711_render_device *controller);
int bcm2711_render_worker_submit(struct bcm2711_render_session *session, bcm2711_render_execute execute, bcm2711_render_dispose dispose, void *payload, uint32_t timeline, struct drv_gpu_completion *completion);
int bcm2711_render_worker_step(struct bcm2711_render_device *controller);
void bcm2711_render_worker_stop(struct bcm2711_render_session *session, int error);
int bcm2711_render_worker_stopped(struct bcm2711_render_session *session);
void bcm2711_render_worker_drain(struct bcm2711_render_session *session);
void bcm2711_render_worker_fault(struct bcm2711_render_device *controller, int error);

/* Tells whether one more failure line fits the log's bound; the caller holds the controller mutex. */
bool bcm2711_render_failure_line(struct bcm2711_render_device *controller);

/*
 * Reports why native admission is closed to a session, or NULL while it is
 * open: the session stopping, the engine uncertain after a fault, the
 * engine not ready, or a fault.  The caller holds the native IRQ guard.
 */
const char *bcm2711_render_refusal_reason(const struct bcm2711_render_device *controller, const struct bcm2711_render_session *session);

/*
 * Logs a refusal with EIO at site for reason, at most one line a second
 * (a submit's EIO had no line); the caller holds no spinlock.
 */
void bcm2711_render_refusal_line(const char *site, const char *reason);
int bcm2711_render_worker_recovered(struct bcm2711_render_device *controller);

/* Counts an opened or closed session for the marker minimum; EBUSY when no more may open.  The closer reports the capacity change outside the mutex. */
int bcm2711_render_worker_session_open(struct bcm2711_render_device *controller);
void bcm2711_render_worker_session_close(struct bcm2711_render_device *controller);

/* Tells whether a live request still names the session's domain; the caller holds the native IRQ guard. */
bool bcm2711_render_worker_timeline_busy(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, uint32_t timeline);

/* Refuses an execution past the hard limits (a stream submitting without markers); the caller holds the controller mutex. */
int bcm2711_render_execution_admit(struct bcm2711_render_session *session, uint64_t bytes);

/*
 * The executor's window around a pass's native jobs.  Open is called by the
 * executor holding the controller mutex and returns without it; close takes
 * the mutex again, then unmaps the views released meanwhile.
 */
void bcm2711_render_window_open(struct bcm2711_render_device *controller);
int bcm2711_render_window_close(struct bcm2711_render_device *controller);

/* Takes the controller mutex outside the window, for an ioctl that maps; unlock gives it back. */
void bcm2711_render_map_lock(struct bcm2711_render_device *controller);
void bcm2711_render_map_unlock(struct bcm2711_render_device *controller);

/* Queues an accepted execution behind its session's earlier ones; the caller holds the controller mutex. */
void bcm2711_render_execution_submit(struct bcm2711_render_session *session, struct bcm2711_render_execution *execution);

/* Runs one step of the next session's execution; EAGAIN when no execution waits. */
int bcm2711_render_executor_step(struct bcm2711_render_device *controller);

#endif
