/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render thread (ws213-p004): the two steps of the pull, the frame's
 * recording, submission, presentation and fence apart from the main thread
 * (render-thread.h).
 *
 * The thread never holds its lock while it calls an operation (Vulkan),
 * waits on anything but its own signal, or writes to the wake pipe
 * (design 4.6).
 */

#include "render-thread.h"

#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void *render_main(void *argument);
static enum kwl_render_step render_idle(struct kwl_render_thread *render);
static enum kwl_render_step render_ahead(struct kwl_render_thread *render);
static uint64_t render_now_us(const struct kwl_render_thread *render);
static enum kwl_render_step render_answer(struct kwl_render_thread *render);
static enum kwl_render_step render_reap(struct kwl_render_thread *render, uint64_t timeout_ns);
static void render_tasks_submit(struct kwl_render_thread *render, struct kwl_render_message *tasks);
static enum kwl_render_step render_tasks_reap(struct kwl_render_thread *render, uint64_t timeout_ns);
static void render_messages_free(struct kwl_render_message *messages);
static void render_post(struct kwl_render_thread *render, struct kwl_render_message *message);
static void render_deadline(struct timespec *deadline, unsigned wait_ms);
static void render_wake(int descriptor);

/*
 * Makes the render thread's state: its lock and signals (timed by the
 * monotonic clock), no want, nothing held.
 */
int
kwl_render_init(
	struct kwl_render_thread *render,
	const struct kwl_render_ops *ops,
	void *context,
	int wake_main)
{
	pthread_condattr_t attributes;
	int error;

	/* Nothing wanted, held or queued. */
	memset(render, 0, sizeof(*render));
	render->ops = *ops;
	render->context = context;
	render->wake_main = wake_main;

	/* The lock. */
	error = pthread_mutex_init(&render->lock, NULL);
	if (error != 0)
		return error;

	/* The signals' clock. */
	error = pthread_condattr_init(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(&render->lock);
		return error;
	}

	/* The main thread's changes, which the thread waits for. */
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error == 0)
		error = pthread_cond_init(&render->changed, &attributes);
	if (error != 0) {
		pthread_condattr_destroy(&attributes);
		pthread_mutex_destroy(&render->lock);
		return error;
	}

	/* The thread's answers and messages to the main thread. */
	error = pthread_cond_init(&render->answered, &attributes);
	pthread_condattr_destroy(&attributes);
	if (error != 0) {
		pthread_cond_destroy(&render->changed);
		pthread_mutex_destroy(&render->lock);
		return error;
	}

	/* Succeeded: the thread can be started. */
	return 0;
}

/*
 * Frees what the stopped render thread leaves: messages not taken, tasks
 * never seen complete (a thread never started), and its lock and signals.
 */
void
kwl_render_destroy(
	struct kwl_render_thread *render)
{
	/* The messages not taken, and the tasks' records (their command buffers and fences are the main thread's). */
	render_messages_free(render->messages);
	render->messages = NULL;
	render->messages_tail = NULL;
	render_messages_free(render->tasks);
	render->tasks = NULL;
	render->tasks_tail = NULL;
	render_messages_free(render->task_flights);
	render->task_flights = NULL;
	render->task_flights_tail = NULL;

	/* A park's message never posted, a completion never posted, and those of jobs never seen complete. */
	free(render->park_message);
	render->park_message = NULL;
	free(render->completion);
	render->completion = NULL;
	while (render->flight_count > 0U) {
		render->flight_count--;
		free(render->flights[render->flight_count]);
		render->flights[render->flight_count] = NULL;
	}

	/* The signals, then the lock. */
	pthread_cond_destroy(&render->answered);
	pthread_cond_destroy(&render->changed);
	pthread_mutex_destroy(&render->lock);
}

/*
 * Starts the render thread.
 */
int
kwl_render_start(
	struct kwl_render_thread *render)
{
	sigset_t signals_all;
	sigset_t signals_before;
	int error;

	/* The thread steps until it is asked to stop. */
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&render->thread, NULL, render_main, render);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
	if (error != 0)
		return error;

	/* Succeeded: stopped by kwl_render_stop. */
	render->started = 1U;
	return 0;
}

/*
 * Stops the render thread after the frame in flight, and waits for it.
 */
void
kwl_render_stop(
	struct kwl_render_thread *render)
{
	/* A thread never started has nothing in flight. */
	if (!render->started)
		return;

	/* The request, which wakes the thread. */
	pthread_mutex_lock(&render->lock);

	render->stop = 1U;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);

	pthread_mutex_unlock(&render->lock);

	/* The thread ends. */
	(void)pthread_join(render->thread, NULL);
	render->started = 0U;
}

/*
 * Runs one step of the render thread: submits the tasks given and tells
 * those completed, looks for a want and acquires its images, takes an
 * answer to its ready, or waits a while for the frame or the task in
 * flight.  It never waits for the main thread: it reports that it is
 * blocked instead.  The thread calls it in a loop; a host test calls it
 * step by step.
 */
enum kwl_render_step
kwl_render_step(
	struct kwl_render_thread *render)
{
	struct kwl_render_message *notice;
	struct kwl_render_message *tasks;
	enum kwl_render_step step;
	unsigned listed;
	unsigned tasks_left;

	/* The tasks given up to now, taken together with the look at the changes. */
	pthread_mutex_lock(&render->lock);

	render->seen_sequence = render->sequence;
	tasks = render->tasks;
	render->tasks = NULL;
	render->tasks_tail = NULL;

	pthread_mutex_unlock(&render->lock);

	/* Submitted before any list given after them; those completed are told without waiting. */
	render_tasks_submit(render, tasks);
	if (render->task_flights != NULL) {
		step = render_tasks_reap(render, 0U);
		if (step == KWL_RENDER_STEP_WORKED)
			return step;
	}

	/*
	 * The requests first, except while a frame is in flight or its list
	 * has been given: it completes before a stop or a park (design 4.8),
	 * so a park never leaves a given list to run after it.
	 */
	pthread_mutex_lock(&render->lock);

	listed = 0U;
	if (render->state == KWL_RENDER_STATE_ANSWER &&
	    render->answer == KWL_RENDER_ANSWER_LIST &&
	    render->answer_number == render->ready_number)
		listed = 1U;
	tasks_left = 0U;
	if (render->tasks != NULL || render->task_flights != NULL)
		tasks_left = 1U;
	if (render->flight_count == 0U && !listed) {
		/* A stop ends the thread once every task given has completed. */
		if (render->stop && !tasks_left) {
			pthread_mutex_unlock(&render->lock);
			return KWL_RENDER_STEP_STOP;
		}

		/* Asked to stop with a task in flight: its fence for a moment, then the next look. */
		if (render->stop) {
			pthread_mutex_unlock(&render->lock);
			(void)render_tasks_reap(render, KWL_RENDER_POLL_NS);
			return KWL_RENDER_STEP_WORKED;
		}

		/*
		 * A park is answered once every task given before it has
		 * completed (drained: nothing the thread submitted still runs),
		 * meanwhile the oldest task's fence for a moment.
		 */
		if (render->park && !render->parked && tasks_left) {
			pthread_mutex_unlock(&render->lock);
			(void)render_tasks_reap(render, KWL_RENDER_POLL_NS);
			return KWL_RENDER_STEP_WORKED;
		}

		/* A park is answered once; the thread calls no Vulkan but its tasks' until it ends. */
		if (render->park) {
			if (!render->parked) {
				render->parked = 1U;
				pthread_cond_broadcast(&render->answered);
			}

			/* A park asked without waiting is told by its message, after the messages before it. */
			notice = render->park_message;
			render->park_message = NULL;
			pthread_mutex_unlock(&render->lock);
			if (notice != NULL)
				render_post(render, notice);

			/* Parked with a task in flight: its fence for a moment, then the next look. */
			if (render->task_flights != NULL) {
				(void)render_tasks_reap(render, KWL_RENDER_POLL_NS);
				return KWL_RENDER_STEP_WORKED;
			}

			/* Parked until the main thread ends the park. */
			return KWL_RENDER_STEP_BLOCKED;
		}

		/* No park, or it is over. */
		render->parked = 0U;
	}

	pthread_mutex_unlock(&render->lock);

	/* A job in flight that completed is told first, without waiting. */
	if (render->flight_count > 0U) {
		step = render_reap(render, 0U);
		if (step == KWL_RENDER_STEP_WORKED)
			return step;
	}

	/* By where the thread is in the pull; a want waits while as many jobs as there are slots are in flight. */
	step = KWL_RENDER_STEP_BLOCKED;
	switch (render->state) {
	case KWL_RENDER_STATE_IDLE:
		if (render->flight_count < KWL_RENDER_FLIGHTS)
			step = render_idle(render);
		break;
	case KWL_RENDER_STATE_ANSWER:
		step = render_answer(render);
		break;
	default:
		break;
	}

	/*
	 * Blocked with a job in flight: the oldest's fence for a moment, so
	 * that the main thread's next want or answer is looked at soon too;
	 * with only a task in flight, the oldest task's fence so.
	 */
	if (step == KWL_RENDER_STEP_BLOCKED && render->flight_count > 0U) {
		(void)render_reap(render, KWL_RENDER_POLL_NS);
		step = KWL_RENDER_STEP_WORKED;
	} else if (step == KWL_RENDER_STEP_BLOCKED && render->task_flights != NULL) {
		(void)render_tasks_reap(render, KWL_RENDER_POLL_NS);
		step = KWL_RENDER_STEP_WORKED;
	}

	/* Reports what the step came to. */
	return step;
}

/*
 * Says what the main thread may draw (design 4.1): the kind, and for a
 * frame the heads it needs.  The number moves only when the kind changes
 * to a frame or a layer (from another kind, or from none after the want
 * went down); a new list of heads keeps it.  Gives the want's number.
 */
void
kwl_render_set_want(
	struct kwl_render_thread *render,
	enum kwl_render_kind kind,
	const unsigned *heads,
	unsigned head_count,
	uint64_t *number)
{
	unsigned index;

	/* The want, and the thread woken for it. */
	if (head_count > KWL_RENDER_HEADS)
		head_count = KWL_RENDER_HEADS;
	pthread_mutex_lock(&render->lock);

	if (kind != KWL_RENDER_NONE && kind != render->want.kind)
		render->want.number++;
	render->want.kind = kind;
	render->want.head_count = head_count;
	for (index = 0; index < head_count; index++)
		render->want.heads[index] = heads[index];
	render->sequence++;
	pthread_cond_broadcast(&render->changed);
	*number = render->want.number;

	pthread_mutex_unlock(&render->lock);
}

/*
 * Answers a ready: the list for the job given, or a cancel (the images
 * stay held).  A list takes the want down now, if it is still the job's
 * (compare and clear), together with the main thread's own view of it: a
 * want raised again after this is a new want with a new number, even
 * before the thread has taken the list (place20's stall, where the thread
 * took the want raised again down with the list).
 */
void
kwl_render_answer(
	struct kwl_render_thread *render,
	uint64_t number,
	enum kwl_render_answer answer,
	const struct kwl_render_job *job)
{
	/* The answer, and the thread woken for it. */
	pthread_mutex_lock(&render->lock);

	render->answer = answer;
	render->answer_number = number;
	if (job != NULL)
		render->job = *job;
	if (answer == KWL_RENDER_ANSWER_LIST && render->want.number == number)
		render->want.kind = KWL_RENDER_NONE;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);

	pthread_mutex_unlock(&render->lock);
}

/*
 * Takes every message the render thread has queued, in order, for the main
 * thread to apply and free.  The main thread empties its wake pipe before,
 * so a message queued after this wakes it again.
 */
struct kwl_render_message *
kwl_render_take(
	struct kwl_render_thread *render)
{
	struct kwl_render_message *messages;

	/* The whole queue at once. */
	pthread_mutex_lock(&render->lock);

	messages = render->messages;
	render->messages = NULL;
	render->messages_tail = NULL;
	render->main_woken = 0U;

	pthread_mutex_unlock(&render->lock);

	/* Succeeded: the messages, oldest first, or NULL. */
	return messages;
}

/*
 * Waits at most wait_ms for a message of the render thread (the main
 * thread waiting for the frame in flight).  Returns 0 when one is queued,
 * ETIMEDOUT otherwise.
 */
int
kwl_render_wait_message(
	struct kwl_render_thread *render,
	unsigned wait_ms)
{
	struct timespec deadline;
	int error;

	/* Until a message, or the deadline. */
	render_deadline(&deadline, wait_ms);
	error = 0;
	pthread_mutex_lock(&render->lock);

	while (render->messages == NULL && error == 0)
		error = pthread_cond_timedwait(&render->answered, &render->lock, &deadline);
	if (render->messages != NULL)
		error = 0;

	pthread_mutex_unlock(&render->lock);

	/* None came in time. */
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: a message is queued. */
	return 0;
}

/*
 * Parks the render thread for the main thread's change of the swapchains:
 * waits at most wait_ms for the thread to answer, which it does after the
 * frame in flight and the tasks given before, at a point where it calls
 * no Vulkan.  Returns 0 when
 * parked, ETIMEDOUT otherwise.
 */
int
kwl_render_park(
	struct kwl_render_thread *render,
	unsigned wait_ms)
{
	struct timespec deadline;
	int error;

	/* The request, and the thread's answer. */
	render_deadline(&deadline, wait_ms);
	error = 0;
	pthread_mutex_lock(&render->lock);

	render->park = 1U;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);
	while (!render->parked && error == 0)
		error = pthread_cond_timedwait(&render->answered, &render->lock, &deadline);
	if (render->parked)
		error = 0;

	pthread_mutex_unlock(&render->lock);

	/* The thread did not answer in time. */
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: parked until kwl_render_unpark. */
	return 0;
}

/*
 * Parks the render thread without waiting (the display thread's changes,
 * design 4.3): the thread parks after the frame in flight and the tasks
 * given before, and posts the message given (the main thread's, kind
 * KWL_RENDER_PARKED) back in its queue, after the messages before it.  A
 * thread parked already posts it at its next look.
 */
void
kwl_render_park_request(
	struct kwl_render_thread *render,
	struct kwl_render_message *message)
{
	/* The request and its message, and the thread woken for it. */
	message->kind = KWL_RENDER_PARKED;
	message->next = NULL;
	pthread_mutex_lock(&render->lock);

	free(render->park_message);
	render->park_message = message;
	render->park = 1U;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);

	pthread_mutex_unlock(&render->lock);
}

/*
 * Ends a park.
 */
void
kwl_render_unpark(
	struct kwl_render_thread *render)
{
	/* The thread steps again from its next look. */
	pthread_mutex_lock(&render->lock);

	render->park = 0U;
	free(render->park_message);
	render->park_message = NULL;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);

	pthread_mutex_unlock(&render->lock);
}

/*
 * Forgets images the render thread holds whose swapchains the main thread
 * destroyed while it was parked: the output's (output nonzero) and the
 * heads' of the slots set in heads_mask.
 */
void
kwl_render_forget(
	struct kwl_render_thread *render,
	unsigned output,
	unsigned heads_mask)
{
	unsigned slot;

	/* The held images, the thread parked. */
	pthread_mutex_lock(&render->lock);

	if (output) {
		render->held.output = 0U;
		render->ahead = 0U;
	}
	for (slot = 0; slot < KWL_RENDER_HEADS; slot++) {
		if ((heads_mask & (1U << slot)) != 0U)
			render->held.heads[slot] = 0U;
	}

	pthread_mutex_unlock(&render->lock);
}

/*
 * Gives the render thread a task: the message the main thread made with
 * its number, its recorded command buffer and its fence.  The thread
 * submits it before any list given after it and posts the same message
 * back once the fence is signalled; until then the main thread keeps the
 * command buffer, the fence and what the commands name.
 */
void
kwl_render_task(
	struct kwl_render_thread *render,
	struct kwl_render_message *task)
{
	/* A completion, once it is posted back. */
	task->kind = KWL_RENDER_TASK_DONE;
	task->next = NULL;
	task->result = VK_SUCCESS;

	/* After the tasks given before it, and the thread woken for it. */
	pthread_mutex_lock(&render->lock);

	if (render->tasks_tail == NULL) {
		render->tasks = task;
	} else {
		render->tasks_tail->next = task;
	}

	/* The task is now the last one given, and the thread is told of the change. */
	render->tasks_tail = task;
	render->sequence++;
	pthread_cond_broadcast(&render->changed);

	pthread_mutex_unlock(&render->lock);
}

/*
 * Tells whether the render thread is recording, submitting or presenting a
 * job now: while it is, the main thread uses neither the frames' command
 * pool nor the queue.  Returns 1 while it is.
 */
int
kwl_render_running(
	struct kwl_render_thread *render)
{
	unsigned running;

	/* The flag, at one moment. */
	pthread_mutex_lock(&render->lock);

	running = render->running;

	pthread_mutex_unlock(&render->lock);

	/* Succeeded: whether a job runs. */
	if (running)
		return 1;
	return 0;
}

/*
 * Gives what the render thread shares with the main thread now, for the
 * main thread's line when a want goes unanswered.
 */
void
kwl_render_status(
	struct kwl_render_thread *render,
	struct kwl_render_status *status)
{
	struct kwl_render_message *message;

	/* The shared state, at one moment. */
	memset(status, 0, sizeof(*status));
	pthread_mutex_lock(&render->lock);

	status->want_kind = render->want.kind;
	status->want_number = render->want.number;
	status->answer = render->answer;
	status->answer_number = render->answer_number;
	status->park = render->park;
	status->parked = render->parked;
	status->busy = render->busy;
	status->held_output = render->held.output;
	status->last_ready = render->last_ready;
	status->last_completed = render->last_completed;

	/* The messages the main thread has not taken yet. */
	for (message = render->messages; message != NULL; message = message->next)
		status->messages++;

	pthread_mutex_unlock(&render->lock);
}

/* Runs the render thread's steps, waiting between them for the main thread when blocked, until it is asked to stop. */
static void *
render_main(
	void *argument)
{
	struct kwl_render_thread *render;
	enum kwl_render_step step;

	/* Step after step. */
	render = argument;
	for (;;) {
		step = kwl_render_step(render);
		if (step == KWL_RENDER_STEP_STOP)
			break;
		if (step == KWL_RENDER_STEP_WORKED)
			continue;

		/*
		 * Blocked: the next change the main thread makes (every change
		 * signals; a job or a task in flight is never blocked, its fence
		 * is waited for in the steps).  An image taken ahead that was not
		 * free is taken with the next frame's want instead.
		 */
		pthread_mutex_lock(&render->lock);

		while (render->sequence == render->seen_sequence)
			(void)pthread_cond_wait(&render->changed, &render->lock);

		pthread_mutex_unlock(&render->lock);
	}

	/* The thread ends. */
	return NULL;
}

/*
 * Looks for a want and acquires what it needs: for a frame, the output's
 * image unless one is held (at most KWL_RENDER_ACQUIRE_NS at a time, the
 * requests looked at in between) and each wanted head's not held (without
 * waiting).  Then the ready goes out with everything held.  A failed
 * acquire of the output takes the want down, if it is still the same one
 * (compare and clear, design 4.1), and is told to the main thread.
 */
static enum kwl_render_step
render_idle(
	struct kwl_render_thread *render)
{
	struct kwl_render_message *message;
	struct kwl_render_want want;
	uint64_t acquired_us;
	uint64_t acquire_cycles;
	uint64_t timeout;
	uint32_t image;
	unsigned index;
	unsigned slot;
	unsigned held;
	unsigned ahead;
	VkResult result;
	VkResult head_results[KWL_RENDER_HEADS];

	/* The want as it is now. */
	pthread_mutex_lock(&render->lock);

	want = render->want;
	held = render->held.output;
	ahead = render->ahead;

	pthread_mutex_unlock(&render->lock);

	/* Nothing wanted: the output's next image is taken ahead when it may be, with nothing in flight. */
	if (want.kind == KWL_RENDER_NONE) {
		if (ahead && !held && render->flight_count == 0U)
			return render_ahead(render);
		return KWL_RENDER_STEP_BLOCKED;
	}

	/* The ready's message, made first: without memory the step is tried again. */
	message = calloc(1, sizeof(*message));
	if (message == NULL) {
		usleep(1000);
		return KWL_RENDER_STEP_WORKED;
	}

	/*
	 * The output's image for a frame, unless held from before; with a job
	 * in flight only for a moment, so that its completion is told soon.
	 */
	acquired_us = 0U;
	acquire_cycles = 0U;
	timeout = KWL_RENDER_ACQUIRE_NS;
	if (render->flight_count > 0U)
		timeout = KWL_RENDER_POLL_NS;
	if (want.kind == KWL_RENDER_FRAME && !held) {
		result = render->ops.acquire_output(render->context, timeout, &image, &acquire_cycles, &acquired_us);

		/* None free yet: the requests are looked at, then the acquire is tried again. */
		if (result == VK_TIMEOUT || result == VK_NOT_READY) {
			free(message);
			return KWL_RENDER_STEP_WORKED;
		}

		/* A failure: the want goes down if it is still this one, and the main thread hears of it. */
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			pthread_mutex_lock(&render->lock);

			if (render->want.number == want.number)
				render->want.kind = KWL_RENDER_NONE;
			render->ahead = 0U;

			pthread_mutex_unlock(&render->lock);

			/* Told in order. */
			message->kind = KWL_RENDER_ACQUIRE_FAILED;
			message->number = want.number;
			message->want_kind = want.kind;
			message->result = result;
			render_post(render, message);
			return KWL_RENDER_STEP_WORKED;
		}

		/* Held until it is presented. */
		pthread_mutex_lock(&render->lock);

		render->held.output = 1U;
		render->held.output_image = image;
		render->ahead = 1U;

		pthread_mutex_unlock(&render->lock);
	} else if (want.kind == KWL_RENDER_FRAME) {
		/* An image held from before (taken ahead, or kept by a cancel) is ready now. */
		acquired_us = render_now_us(render);
	}

	/* Each wanted head not held, without waiting; what came of it is told with the ready. */
	for (index = 0; index < KWL_RENDER_HEADS; index++)
		head_results[index] = VK_SUCCESS;
	for (index = 0; want.kind == KWL_RENDER_FRAME && index < want.head_count; index++) {
		slot = want.heads[index];
		if (slot >= KWL_RENDER_HEADS)
			continue;
		pthread_mutex_lock(&render->lock);

		held = render->held.heads[slot];

		pthread_mutex_unlock(&render->lock);
		if (held)
			continue;

		/* Its next image, or why not. */
		result = render->ops.acquire_head(render->context, slot, &image);
		head_results[slot] = result;
		if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
			continue;
		head_results[slot] = VK_SUCCESS;

		/* Held until it is presented. */
		pthread_mutex_lock(&render->lock);

		render->held.heads[slot] = 1U;
		render->held.head_images[slot] = image;

		pthread_mutex_unlock(&render->lock);
	}

	/* The ready, with everything held: from now on no Vulkan until the answer. */
	message->kind = KWL_RENDER_READY;
	message->number = want.number;
	message->want_kind = want.kind;
	message->acquire_cycles = acquire_cycles;
	message->acquired_us = acquired_us;
	pthread_mutex_lock(&render->lock);

	message->held = render->held;

	pthread_mutex_unlock(&render->lock);
	for (index = 0; index < KWL_RENDER_HEADS; index++)
		message->held.head_results[index] = head_results[index];
	render->state = KWL_RENDER_STATE_ANSWER;
	render->ready_number = want.number;
	render_post(render, message);

	/* Succeeded: waiting for the answer. */
	return KWL_RENDER_STEP_WORKED;
}

/*
 * Takes the main thread's answer to the ready: a cancel goes back to the
 * wants with the images still held; a list is recorded, submitted and
 * presented (its want went down with the answer).  The images it presents
 * are no longer held.
 */
static enum kwl_render_step
render_answer(
	struct kwl_render_thread *render)
{
	struct kwl_render_message *completion;
	struct kwl_render_message *tasks;
	enum kwl_render_answer answer;
	struct kwl_render_job job;
	unsigned slot;

	/* The completion's message, made before the job is taken. */
	if (render->completion == NULL) {
		render->completion = calloc(1, sizeof(*render->completion));
		if (render->completion == NULL) {
			usleep(1000);
			return KWL_RENDER_STEP_WORKED;
		}
	}

	/* The answer to this ready, if one came. */
	pthread_mutex_lock(&render->lock);

	if (render->answer == KWL_RENDER_ANSWER_NONE || render->answer_number != render->ready_number) {
		pthread_mutex_unlock(&render->lock);
		return KWL_RENDER_STEP_BLOCKED;
	}

	/* Taken: the next answer is for the next ready. */
	answer = render->answer;
	job = render->job;
	render->answer = KWL_RENDER_ANSWER_NONE;

	/* A cancel: back to the wants, the images kept. */
	if (answer == KWL_RENDER_ANSWER_CANCEL) {
		render->state = KWL_RENDER_STATE_IDLE;
		pthread_mutex_unlock(&render->lock);
		return KWL_RENDER_STEP_WORKED;
	}

	/* The list: the images the job presents are let go. */
	if (job.kind == KWL_RENDER_FRAME)
		render->held.output = 0U;
	for (slot = 0; slot < KWL_RENDER_HEADS; slot++) {
		if (job.heads[slot])
			render->held.heads[slot] = 0U;
	}

	/* A job in flight from now on, being run; the tasks given before the list are taken with it. */
	render->busy = 1U;
	render->running = 1U;
	tasks = render->tasks;
	render->tasks = NULL;
	render->tasks_tail = NULL;

	pthread_mutex_unlock(&render->lock);

	/* The tasks go first, so that the list's commands come after them in the queue. */
	render_tasks_submit(render, tasks);

	/* Recorded, submitted and presented; the thread looks for the next want from now on. */
	completion = render->completion;
	render->completion = NULL;
	completion->kind = KWL_RENDER_COMPLETED;
	completion->number = job.number;
	completion->want_kind = job.kind;
	completion->job = job;
	render->ops.run(render->context, &job, &completion->outcome);
	render->state = KWL_RENDER_STATE_IDLE;
	pthread_mutex_lock(&render->lock);

	render->running = 0U;

	pthread_mutex_unlock(&render->lock);

	/* A job that was not submitted has no fence: it completes now, after the jobs before it. */
	if (completion->outcome.result != VK_SUCCESS) {
		while (render->flight_count > 0U)
			(void)render_reap(render, KWL_RENDER_FENCE_NS);
		pthread_mutex_lock(&render->lock);

		render->busy = 0U;

		pthread_mutex_unlock(&render->lock);
		render_post(render, completion);
		return KWL_RENDER_STEP_WORKED;
	}

	/* Succeeded: in flight after the jobs before it. */
	render->flights[render->flight_count] = completion;
	render->flight_count++;
	return KWL_RENDER_STEP_WORKED;
}

/*
 * Waits at most timeout_ns for the oldest job in flight's fence: once
 * signalled (or its wait failed), its completion goes to the main thread,
 * in submission order.  Returns WORKED when one was told, BLOCKED when it
 * is still running.
 */
static enum kwl_render_step
render_reap(
	struct kwl_render_thread *render,
	uint64_t timeout_ns)
{
	struct kwl_render_message *completion;
	unsigned index;
	VkResult result;

	/* The oldest's fence, for a while. */
	completion = render->flights[0];
	result = render->ops.wait(render->context, completion->job.slot, timeout_ns);
	if (result == VK_TIMEOUT)
		return KWL_RENDER_STEP_BLOCKED;

	/* Done, or its wait failed (the compositor's failure): the others move up. */
	if (result != VK_SUCCESS)
		completion->outcome.result = result;
	for (index = 1U; index < render->flight_count; index++)
		render->flights[index - 1U] = render->flights[index];
	render->flight_count--;
	render->flights[render->flight_count] = NULL;

	/* Nothing in flight any more: not busy (unless a job is taken meanwhile, which only this thread does). */
	if (render->flight_count == 0U) {
		pthread_mutex_lock(&render->lock);

		render->busy = 0U;

		pthread_mutex_unlock(&render->lock);
	}

	/* Told in order, before any ready that follows. */
	render_post(render, completion);
	return KWL_RENDER_STEP_WORKED;
}

/* Submits tasks in their order after those submitted before; a submission that failed is told in its turn. */
static void
render_tasks_submit(
	struct kwl_render_thread *render,
	struct kwl_render_message *tasks)
{
	struct kwl_render_message *task;
	VkResult result;

	/* Each task, oldest first. */
	while (tasks != NULL) {
		task = tasks;
		tasks = task->next;
		task->next = NULL;

		/* Its commands with its fence, under the owner's lock of the queue. */
		result = render->ops.submit_task(render->context, task->task_command, task->task_fence);
		task->result = result;

		/* In flight after the tasks before it. */
		if (render->task_flights_tail == NULL) {
			render->task_flights = task;
		} else {
			render->task_flights_tail->next = task;
		}

		/* The task is now the last one in flight. */
		render->task_flights_tail = task;
	}
}

/*
 * Tells the main thread of the tasks in flight that completed, oldest
 * first: the oldest's fence is waited for at most timeout_ns, the later
 * ones' not at all.  Returns WORKED when one was told, BLOCKED when the
 * oldest is still running.
 */
static enum kwl_render_step
render_tasks_reap(
	struct kwl_render_thread *render,
	uint64_t timeout_ns)
{
	struct kwl_render_message *task;
	enum kwl_render_step step;
	VkResult result;

	/* While the oldest has completed (or its submission or its wait failed). */
	step = KWL_RENDER_STEP_BLOCKED;
	while (render->task_flights != NULL) {
		/* A submitted task's fence, for a while. */
		task = render->task_flights;
		if (task->result == VK_SUCCESS) {
			result = render->ops.wait_task(render->context, task->task_fence, timeout_ns);
			if (result == VK_TIMEOUT)
				break;
			task->result = result;
		}

		/* Out of the flights; the next is only looked at. */
		timeout_ns = 0U;
		render->task_flights = task->next;
		if (render->task_flights == NULL)
			render->task_flights_tail = NULL;

		/* Told in order. */
		render_post(render, task);
		step = KWL_RENDER_STEP_WORKED;
	}

	/* Reports whether one was told. */
	return step;
}

/* Frees a chain of messages. */
static void
render_messages_free(
	struct kwl_render_message *messages)
{
	struct kwl_render_message *message;

	/* Each, oldest first. */
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		free(message);
	}
}

/* Queues a message for the main thread after every earlier one, and wakes it when it was not woken for the queue yet. */
static void
render_post(
	struct kwl_render_thread *render,
	struct kwl_render_message *message)
{
	unsigned wake;

	/* After the messages before it; a waiting main thread is signalled. */
	message->next = NULL;
	pthread_mutex_lock(&render->lock);

	if (render->messages_tail == NULL) {
		render->messages = message;
	} else {
		render->messages_tail->next = message;
	}

	/* The number of the last ready, for kwl_render_status. */
	if (message->kind == KWL_RENDER_READY)
		render->last_ready = message->number;

	/* The number of the last completion, for kwl_render_status. */
	if (message->kind == KWL_RENDER_COMPLETED)
		render->last_completed = message->number;

	/* The main thread is woken once for a queue it has not taken yet. */
	render->messages_tail = message;
	wake = 0U;
	if (!render->main_woken) {
		render->main_woken = 1U;
		wake = 1U;
	}

	/* A main thread waiting for a message wakes. */
	pthread_cond_broadcast(&render->answered);

	pthread_mutex_unlock(&render->lock);

	/* The main thread's poll returns. */
	if (wake)
		render_wake(render->wake_main);
}

/*
 * Acquires the output's next image while nothing is wanted, without
 * waiting, so that the next frame's ready goes out at once instead of
 * after the acquire (the time it takes is the display's round trip).
 * Taken only when the output may be (render_idle), it is held as an image
 * kept by a cancel is; a failure other than none being free stops the
 * taking ahead until a frame's acquire succeeds again.
 */
static enum kwl_render_step
render_ahead(
	struct kwl_render_thread *render)
{
	uint64_t cycles;
	uint64_t at_us;
	uint32_t image;
	VkResult result;

	/* The next image if one is free now. */
	result = render->ops.acquire_output(render->context, 0U, &image, &cycles, &at_us);

	/* Held, or not taken. */
	pthread_mutex_lock(&render->lock);

	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
		render->held.output = 1U;
		render->held.output_image = image;
	} else if (result != VK_TIMEOUT && result != VK_NOT_READY) {
		render->ahead = 0U;
	}

	pthread_mutex_unlock(&render->lock);

	/* Held: the next step finds the want; none free: the next change is waited for. */
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
		return KWL_RENDER_STEP_WORKED;
	return KWL_RENDER_STEP_BLOCKED;
}

/* The clock of the main thread's lines in microseconds (the monotonic clock without one), 0 when it fails. */
static uint64_t
render_now_us(
	const struct kwl_render_thread *render)
{
	struct timespec now;

	/* The owner's clock. */
	if (render->ops.now_us != NULL)
		return render->ops.now_us();

	/* The monotonic clock. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0U;

	/* Succeeded: microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

/* Gives the deadline wait_ms from now on the monotonic clock. */
static void
render_deadline(
	struct timespec *deadline,
	unsigned wait_ms)
{
	/* Now, and the wait added. */
	clock_gettime(CLOCK_MONOTONIC, deadline);
	deadline->tv_sec += (time_t)(wait_ms / 1000U);
	deadline->tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
}

/* Writes one byte to a wake pipe; a full pipe already wakes its reader. */
static void
render_wake(
	int descriptor)
{
	unsigned char byte;
	ssize_t written;

	/* One byte; EAGAIN means the reader has bytes to wake on already. */
	if (descriptor < 0)
		return;
	byte = 1U;
	do {
		written = write(descriptor, &byte, 1U);
	} while (written < 0 && errno == EINTR);
}
