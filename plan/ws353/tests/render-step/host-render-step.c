/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the render thread's waits
 * (userland/desktop/wayland/render-thread.c), with fake Vulkan operations
 * whose fences and images the test hands out.
 *
 * The thread runs on its own; the test plays the main thread.  It checks
 * that the thread waits for one thing at a time and never with a timeout
 * (every fence wait is given UINT64_MAX, every acquire 0 or UINT64_MAX),
 * that it tells every completion without being asked (a layer's picture,
 * two frames, a frame whose present failed), that the slots bound the
 * frames in flight, that the acquire blocks only with nothing in flight,
 * that a park drains what is in flight and holds back the wants, and the
 * tasks' order, failures, parks and stop as before.
 */

#include "userland/desktop/wayland/render-thread.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How many task fences and submissions the test keeps, and how long it waits for a message it expects. */
#define TEST_FENCES	32U
#define TEST_LOG	64U
#define TEST_WAIT_MS	2000U

/* How long the test watches for something that must not happen. */
#define TEST_QUIET_MS	150U

/* What the test waits to see counted in the main thread's view. */
enum test_count {
	TEST_READIES = 1,
	TEST_COMPLETED,
	TEST_TASKS,
	TEST_PARKED,
	TEST_ACQUIRE_FAILED
};

/*
 * The main thread's view of the render thread, built from its messages:
 * the ready outstanding (its number, until answered), the jobs in flight
 * as the compositor counts them, and what has been told so far.
 */
struct test_view {
	unsigned ready_valid;
	uint64_t ready_number;
	unsigned in_flight;
	unsigned readies;
	unsigned completed;
	unsigned completed_slots[TEST_LOG];
	VkResult completed_present[TEST_LOG];
	unsigned tasks;
	unsigned task_numbers[TEST_LOG];
	VkResult task_results[TEST_LOG];
	unsigned parked;
	unsigned acquire_failed;
	VkResult acquire_result;
};

/*
 * The fake GPU and display, under test_lock (test_changed signals every
 * change): each slot's fence and each task's fence (set by the test,
 * cleared by the wait that sees it, as the compositor resets a slot's),
 * the submissions in order (task numbers, 1000 + a job's number), the
 * output's free images and an acquire's failure to give, what the next
 * job's present reports, and the counts of the calls and of the waits
 * given a timeout the thread must not use.
 */
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed = PTHREAD_COND_INITIALIZER;
static unsigned job_signalled[KWL_RENDER_FLIGHTS];
static unsigned task_signalled[TEST_FENCES];
static unsigned fail_submit[TEST_FENCES];
static unsigned long submitted[TEST_LOG];
static unsigned submitted_count;
static unsigned images_free;
static VkResult acquire_error;
static VkResult present_next;
static unsigned job_waits;
static unsigned task_waits;
static unsigned acquire_calls;
static unsigned acquire_blocked;
static uint64_t acquire_timeout_last;
static unsigned bad_timeouts;

/* The checks that failed, counted by the test's thread only. */
static unsigned failures;

static VkResult op_acquire_output(void *context, uint64_t timeout_ns, uint32_t *image, uint64_t *cycles, uint64_t *at_us);
static VkResult op_acquire_head(void *context, unsigned slot, uint32_t *image);
static void op_run(void *context, const struct kwl_render_job *job, struct kwl_render_outcome *outcome);
static VkResult op_wait(void *context, unsigned slot, uint64_t timeout_ns);
static VkResult op_submit_task(void *context, VkCommandBuffer command, VkFence fence);
static VkResult op_wait_task(void *context, VkFence fence, uint64_t timeout_ns);
static struct kwl_render_message *task_new(unsigned number);
static void log_add(unsigned long entry);
static void signal_job(unsigned slot);
static void signal_task(unsigned number);
static void free_images(unsigned count);
static void fail_acquire(VkResult result);
static void set_present(VkResult result);
static unsigned read_count(const unsigned *counter);
static void check(int condition, const char *what);
static void take(struct kwl_render_thread *render, struct test_view *view);
static unsigned view_count(const struct test_view *view, enum test_count which);
static int pump(struct kwl_render_thread *render, struct test_view *view, enum test_count which, unsigned target);
static void quiet(struct kwl_render_thread *render, struct test_view *view);
static void want(struct kwl_render_thread *render, enum kwl_render_kind kind);
static void answer_list(struct kwl_render_thread *render, struct test_view *view, enum kwl_render_kind kind, unsigned slot);
static void *stop_main(void *argument);
static void sleep_ms(unsigned milliseconds);

/* The fake operations. */
static const struct kwl_render_ops test_ops = {
	op_acquire_output,
	op_acquire_head,
	op_run,
	op_wait,
	NULL,
	op_submit_task,
	op_wait_task
};

int
main(void)
{
	struct kwl_render_thread render;
	struct kwl_render_message *message;
	struct test_view view;
	pthread_t stopper;
	unsigned waits_before;
	unsigned calls_before;
	int pipes[2];
	int met;
	int error;

	/* The main thread's wake pipe, never blocking, and the thread started. */
	error = pipe(pipes);
	check(error == 0, "pipe");
	(void)fcntl(pipes[1], F_SETFL, O_NONBLOCK);
	(void)fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	memset(&view, 0, sizeof(view));
	present_next = VK_SUCCESS;
	acquire_error = VK_SUCCESS;
	error = kwl_render_init(&render, &test_ops, NULL, pipes[1]);
	check(error == 0, "init");
	error = kwl_render_start(&render);
	check(error == 0, "start");

	/* Idle: nothing wanted and nothing in flight costs no call. */
	quiet(&render, &view);
	check(read_count(&acquire_calls) == 0U, "idle: no acquire");
	check(read_count(&job_waits) == 0U, "idle: no fence wait");

	/*
	 * L: a layer's picture, then nothing asked.  The thread waits for its
	 * fence once (no look every moment) and tells the completion when it
	 * is signalled, with nothing more asked of it.
	 */
	want(&render, KWL_RENDER_LAYER);
	met = pump(&render, &view, TEST_READIES, 1U);
	check(met, "L: the ready came");
	check(read_count(&acquire_calls) == 0U, "L: a layer acquires no image");
	answer_list(&render, &view, KWL_RENDER_LAYER, 0U);
	quiet(&render, &view);
	check(view.completed == 0U, "L: nothing told before the fence");
	check(read_count(&job_waits) == 1U, "L: the fence waited for once");
	signal_job(0U);
	met = pump(&render, &view, TEST_COMPLETED, 1U);
	check(met, "L: the picture's completion came without a request");
	check(view.in_flight == 0U, "L: nothing in flight");
	check(read_count(&job_waits) == 1U, "L: one wait for one fence");

	/* F: two frames, then nothing asked: both complete, oldest first, and nothing stays in flight. */
	free_images(2U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 2U);
	check(met, "F: the first ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 3U);
	check(met, "F: the second ready came with the first in flight");
	pthread_mutex_lock(&test_lock);

	check(acquire_timeout_last == 0U, "F: the acquire with a frame in flight does not wait");

	pthread_mutex_unlock(&test_lock);
	answer_list(&render, &view, KWL_RENDER_FRAME, 1U);
	check(view.in_flight == 2U, "F: two in flight");
	signal_job(1U);
	quiet(&render, &view);
	check(view.completed == 1U, "F: the second waits for the first");
	signal_job(0U);
	met = pump(&render, &view, TEST_COMPLETED, 3U);
	check(met, "F: both completions came without a request");
	check(view.completed_slots[1] == 0U && view.completed_slots[2] == 1U, "F: completions in submission order");
	check(view.in_flight == 0U, "F: nothing in flight");

	/* P: a frame whose present failed (out of date) completes too. */
	free_images(1U);
	set_present(VK_ERROR_OUT_OF_DATE_KHR);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 4U);
	check(met, "P: the ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	signal_job(0U);
	met = pump(&render, &view, TEST_COMPLETED, 4U);
	check(met, "P: the failed present's completion came");
	set_present(VK_SUCCESS);
	check(view.completed_present[3] == VK_ERROR_OUT_OF_DATE_KHR, "P: told with its present's result");
	check(view.in_flight == 0U, "P: nothing in flight");

	/* S: both slots in flight hold a third want back without an acquire, until the oldest completes. */
	free_images(3U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 5U);
	check(met, "S: the first ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 6U);
	check(met, "S: the second ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 1U);
	calls_before = read_count(&acquire_calls);
	want(&render, KWL_RENDER_FRAME);
	quiet(&render, &view);
	check(view.readies == 6U, "S: no ready while every slot is in flight");
	check(read_count(&acquire_calls) == calls_before, "S: no acquire while every slot is in flight");
	signal_job(0U);
	met = pump(&render, &view, TEST_READIES, 7U);
	check(met, "S: the ready came once the oldest completed");
	check(view.completed == 5U, "S: the completion came before the ready");
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	signal_job(1U);
	signal_job(0U);
	met = pump(&render, &view, TEST_COMPLETED, 7U);
	check(met, "S: the rest completed");
	check(view.in_flight == 0U, "S: nothing in flight");

	/*
	 * A: with a frame in flight and no image free, the acquire does not
	 * wait (tried once, then the fence); with nothing in flight it blocks
	 * until an image is free, without trying again and again.
	 */
	free_images(1U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 8U);
	check(met, "A: the ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	calls_before = read_count(&acquire_calls);
	want(&render, KWL_RENDER_FRAME);
	quiet(&render, &view);
	check(read_count(&acquire_calls) == calls_before + 1U, "A: one acquire without waiting while a frame is in flight");
	check(view.readies == 8U, "A: no ready without an image");
	signal_job(0U);
	met = pump(&render, &view, TEST_COMPLETED, 8U);
	check(met, "A: the frame completed");
	quiet(&render, &view);
	check(read_count(&acquire_blocked) == 1U, "A: the acquire blocks with nothing in flight");
	check(read_count(&acquire_calls) == calls_before + 2U, "A: blocked once, not tried again");
	pthread_mutex_lock(&test_lock);

	check(acquire_timeout_last == UINT64_MAX, "A: the blocking acquire waits until an image is free");

	pthread_mutex_unlock(&test_lock);
	free_images(1U);
	met = pump(&render, &view, TEST_READIES, 9U);
	check(met, "A: the ready came once an image was free");
	kwl_render_answer(&render, view.ready_number, KWL_RENDER_ANSWER_CANCEL, NULL);
	view.ready_valid = 0U;
	kwl_render_set_want(&render, KWL_RENDER_NONE, NULL, 0U, &view.ready_number);
	quiet(&render, &view);

	/* O: a display that stops makes the blocked acquire fail, which is told and takes the want down. */
	error = kwl_render_park(&render, 1000U);
	check(error == 0, "O: parked to forget the held image");
	kwl_render_forget(&render, 1U, 0U);
	kwl_render_unpark(&render);
	want(&render, KWL_RENDER_FRAME);
	quiet(&render, &view);
	check(read_count(&acquire_blocked) == 1U, "O: the acquire blocks");
	fail_acquire(VK_ERROR_OUT_OF_DATE_KHR);
	met = pump(&render, &view, TEST_ACQUIRE_FAILED, 1U);
	check(met, "O: the failed acquire was told");
	check(view.acquire_result == VK_ERROR_OUT_OF_DATE_KHR, "O: as out of date");
	fail_acquire(VK_SUCCESS);
	error = kwl_render_park(&render, 1000U);
	check(error == 0, "O: the thread parks after the failed acquire");
	kwl_render_unpark(&render);

	/*
	 * K: a park asked without waiting while a frame is in flight is
	 * answered after the frame's completion, and holds a want back until
	 * the park ends.
	 */
	free_images(2U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 10U);
	check(met, "K: the ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 1U);
	message = calloc(1, sizeof(*message));
	check(message != NULL, "K: message");
	kwl_render_park_request(&render, message);
	want(&render, KWL_RENDER_FRAME);
	quiet(&render, &view);
	check(view.parked == 0U, "K: not parked before the frame completes");
	check(view.readies == 10U, "K: no ready while parking");
	signal_job(1U);
	met = pump(&render, &view, TEST_PARKED, 1U);
	check(met, "K: parked once the frame completed");
	check(view.completed == 9U, "K: the completion came before the park");
	quiet(&render, &view);
	check(view.readies == 10U, "K: no ready while parked");
	kwl_render_unpark(&render);
	met = pump(&render, &view, TEST_READIES, 11U);
	check(met, "K: the ready came once the park ended");
	kwl_render_answer(&render, view.ready_number, KWL_RENDER_ANSWER_CANCEL, NULL);
	view.ready_valid = 0U;
	kwl_render_set_want(&render, KWL_RENDER_NONE, NULL, 0U, &view.ready_number);
	quiet(&render, &view);

	/*
	 * T: a task and a want, then a second task and the list: the tasks are
	 * submitted before the job given after them, and the fences are
	 * waited for in submission order across tasks and jobs.
	 */
	pthread_mutex_lock(&test_lock);

	submitted_count = 0U;

	pthread_mutex_unlock(&test_lock);
	kwl_render_task(&render, task_new(1U));
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 12U);
	check(met, "T: the ready came with the task in flight");
	kwl_render_task(&render, task_new(2U));
	answer_list(&render, &view, KWL_RENDER_FRAME, 0U);
	signal_task(1U);
	met = pump(&render, &view, TEST_TASKS, 1U);
	check(met, "T: task 1 told");
	quiet(&render, &view);
	pthread_mutex_lock(&test_lock);

	check(submitted_count == 3U, "T: three submissions");
	check(submitted[0] == 1U, "T: task 1 first");
	check(submitted[1] == 2U, "T: task 2 before the job given after it");
	check(submitted[2] >= 1000U, "T: the job last");

	pthread_mutex_unlock(&test_lock);
	signal_job(0U);
	quiet(&render, &view);
	check(view.tasks == 1U && view.completed == 9U, "T: the job waits for task 2, submitted before it");
	signal_task(2U);
	met = pump(&render, &view, TEST_COMPLETED, 10U);
	check(met, "T: the job's completion came");
	check(view.tasks == 2U && view.task_numbers[1] == 2U, "T: task 2 told before it");

	/* B: a failed submission is told in its turn, after the task before it. */
	pthread_mutex_lock(&test_lock);

	fail_submit[4] = 1U;

	pthread_mutex_unlock(&test_lock);
	kwl_render_task(&render, task_new(3U));
	kwl_render_task(&render, task_new(4U));
	quiet(&render, &view);
	check(view.tasks == 2U, "B: the failure waits for task 3");
	signal_task(3U);
	met = pump(&render, &view, TEST_TASKS, 4U);
	check(met, "B: both told");
	check(view.task_numbers[2] == 3U && view.task_results[2] == VK_SUCCESS, "B: task 3 succeeded");
	check(view.task_numbers[3] == 4U && view.task_results[3] == VK_ERROR_DEVICE_LOST, "B: task 4 failed");

	/* C: while parked, a task is submitted and its completion told. */
	error = kwl_render_park(&render, 1000U);
	check(error == 0, "C: parked");
	kwl_render_task(&render, task_new(5U));
	quiet(&render, &view);
	signal_task(5U);
	met = pump(&render, &view, TEST_TASKS, 5U);
	check(met && view.task_numbers[4] == 5U, "C: task 5 completed and told while parked");
	kwl_render_unpark(&render);

	/* E: a park asked without waiting drains the task given before it, and is told after the task's completion. */
	kwl_render_task(&render, task_new(7U));
	quiet(&render, &view);
	message = calloc(1, sizeof(*message));
	check(message != NULL, "E: message");
	kwl_render_park_request(&render, message);
	quiet(&render, &view);
	check(view.parked == 1U, "E: not parked before task 7 completes");
	signal_task(7U);
	met = pump(&render, &view, TEST_PARKED, 2U);
	check(met, "E: parked");
	check(view.tasks == 6U && view.task_numbers[5] == 7U, "E: task 7's completion before the park's message");
	kwl_render_unpark(&render);

	/* D: a stop waits for the task and the frame in flight. */
	free_images(1U);
	want(&render, KWL_RENDER_FRAME);
	met = pump(&render, &view, TEST_READIES, 13U);
	check(met, "D: the ready came");
	answer_list(&render, &view, KWL_RENDER_FRAME, 1U);
	kwl_render_task(&render, task_new(6U));
	quiet(&render, &view);
	error = pthread_create(&stopper, NULL, stop_main, &render);
	check(error == 0, "D: stopper");
	quiet(&render, &view);
	check(view.tasks == 6U && view.completed == 10U, "D: nothing told before the fences");
	signal_task(6U);
	signal_job(1U);
	(void)pthread_join(stopper, NULL);
	take(&render, &view);
	check(view.tasks == 7U && view.task_numbers[6] == 6U, "D: the stop came after task 6's completion");
	check(view.completed == 11U, "D: and after the frame's");

	/* Every wait the thread made: no timeout but none and forever, and no fence waited for twice. */
	pthread_mutex_lock(&test_lock);

	check(bad_timeouts == 0U, "no wait with a timeout");
	waits_before = job_waits + task_waits;

	pthread_mutex_unlock(&test_lock);
	check(waits_before == 11U + 6U, "one wait for each fence");
	printf("waits jobs+tasks=%u acquires=%u\n", waits_before, read_count(&acquire_calls));

	/* The rest. */
	take(&render, &view);
	kwl_render_destroy(&render);
	close(pipes[0]);
	close(pipes[1]);

	/* The summary. */
	if (failures != 0U) {
		printf("FAIL failures=%u\n", failures);
		return 1;
	}
	printf("PASS render step\n");
	return 0;
}

/* Gives the output's next free image; without one, VK_NOT_READY when not waiting, else waits until one is free or the display fails. */
static VkResult
op_acquire_output(
	void *context,
	uint64_t timeout_ns,
	uint32_t *image,
	uint64_t *cycles,
	uint64_t *at_us)
{
	static uint32_t next_image;
	VkResult result;

	(void)context;

	/* The call and its timeout, which is none or forever. */
	*cycles = 0U;
	*at_us = 0U;
	pthread_mutex_lock(&test_lock);

	acquire_calls++;
	acquire_timeout_last = timeout_ns;
	if (timeout_ns != 0U && timeout_ns != UINT64_MAX)
		bad_timeouts++;

	/* None free and not waiting. */
	if (images_free == 0U && timeout_ns == 0U) {
		pthread_mutex_unlock(&test_lock);
		return VK_NOT_READY;
	}

	/* Until one is free, or the display fails. */
	acquire_blocked++;
	while (images_free == 0U && acquire_error == VK_SUCCESS)
		(void)pthread_cond_wait(&test_changed, &test_lock);
	acquire_blocked--;
	result = acquire_error;
	if (result == VK_SUCCESS) {
		images_free--;
		*image = next_image++;
	}

	pthread_mutex_unlock(&test_lock);

	/* The image, or the display's failure. */
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: an image held. */
	return VK_SUCCESS;
}

/* Gives a head's image 0 at once. */
static VkResult
op_acquire_head(
	void *context,
	unsigned slot,
	uint32_t *image)
{
	(void)context;
	(void)slot;

	/* Always free. */
	*image = 0U;
	return VK_SUCCESS;
}

/* Logs a job's submission; its present reports what the test set. */
static void
op_run(
	void *context,
	const struct kwl_render_job *job,
	struct kwl_render_outcome *outcome)
{
	(void)context;

	/* Submitted and presented. */
	log_add(1000U + (unsigned long)job->number);
	outcome->result = VK_SUCCESS;
	pthread_mutex_lock(&test_lock);

	outcome->present_result = present_next;

	pthread_mutex_unlock(&test_lock);
}

/* Waits for a slot's fence until the test signals it, then resets it. */
static VkResult
op_wait(
	void *context,
	unsigned slot,
	uint64_t timeout_ns)
{
	unsigned signalled;

	(void)context;

	/* The call and its timeout, which is forever. */
	pthread_mutex_lock(&test_lock);

	job_waits++;
	if (timeout_ns != UINT64_MAX)
		bad_timeouts++;
	while (!job_signalled[slot % KWL_RENDER_FLIGHTS] && timeout_ns == UINT64_MAX)
		(void)pthread_cond_wait(&test_changed, &test_lock);
	signalled = job_signalled[slot % KWL_RENDER_FLIGHTS];
	job_signalled[slot % KWL_RENDER_FLIGHTS] = 0U;

	pthread_mutex_unlock(&test_lock);

	/* Not signalled within a wait that had a timeout. */
	if (!signalled)
		return VK_TIMEOUT;

	/* Succeeded: the job completed. */
	return VK_SUCCESS;
}

/* Logs a task's submission, or fails the one asked to. */
static VkResult
op_submit_task(
	void *context,
	VkCommandBuffer command,
	VkFence fence)
{
	unsigned number;
	unsigned fail;

	(void)context;
	(void)fence;

	/* The task's number is its command buffer's. */
	number = (unsigned)(uintptr_t)command;
	pthread_mutex_lock(&test_lock);

	fail = fail_submit[number % TEST_FENCES];

	pthread_mutex_unlock(&test_lock);
	if (fail)
		return VK_ERROR_DEVICE_LOST;

	/* Submitted. */
	log_add(number);
	return VK_SUCCESS;
}

/* Waits for a task's fence until the test signals it. */
static VkResult
op_wait_task(
	void *context,
	VkFence fence,
	uint64_t timeout_ns)
{
	unsigned number;
	unsigned signalled;

	(void)context;

	/* The call and its timeout, which is forever. */
	number = (unsigned)(uintptr_t)fence;
	pthread_mutex_lock(&test_lock);

	task_waits++;
	if (timeout_ns != UINT64_MAX)
		bad_timeouts++;
	while (!task_signalled[number % TEST_FENCES] && timeout_ns == UINT64_MAX)
		(void)pthread_cond_wait(&test_changed, &test_lock);
	signalled = task_signalled[number % TEST_FENCES];

	pthread_mutex_unlock(&test_lock);

	/* Not signalled within a wait that had a timeout. */
	if (!signalled)
		return VK_TIMEOUT;

	/* Succeeded: the task completed. */
	return VK_SUCCESS;
}

/* Makes a task's message: its number is its command buffer's and its fence's. */
static struct kwl_render_message *
task_new(
	unsigned number)
{
	struct kwl_render_message *task;

	/* The record the thread posts back. */
	task = calloc(1, sizeof(*task));
	if (task == NULL)
		abort();
	task->number = number;
	task->task_command = (VkCommandBuffer)(uintptr_t)number;
	task->task_fence = (VkFence)(uintptr_t)number;
	return task;
}

/* Adds a submission to the log. */
static void
log_add(
	unsigned long entry)
{
	/* In order. */
	pthread_mutex_lock(&test_lock);

	if (submitted_count < TEST_LOG)
		submitted[submitted_count++] = entry;

	pthread_mutex_unlock(&test_lock);
}

/* Signals a slot's fence. */
static void
signal_job(
	unsigned slot)
{
	/* Seen by the wait for it. */
	pthread_mutex_lock(&test_lock);

	job_signalled[slot % KWL_RENDER_FLIGHTS] = 1U;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Signals a task's fence. */
static void
signal_task(
	unsigned number)
{
	/* Seen by the wait for it. */
	pthread_mutex_lock(&test_lock);

	task_signalled[number % TEST_FENCES] = 1U;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Frees images of the output for the acquire. */
static void
free_images(
	unsigned count)
{
	/* Seen by an acquire waiting. */
	pthread_mutex_lock(&test_lock);

	images_free += count;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Makes the output's acquire fail (VK_SUCCESS to end it). */
static void
fail_acquire(
	VkResult result)
{
	/* Seen by an acquire waiting. */
	pthread_mutex_lock(&test_lock);

	acquire_error = result;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Says what the next job's present reports. */
static void
set_present(
	VkResult result)
{
	/* Seen by the next run. */
	pthread_mutex_lock(&test_lock);

	present_next = result;

	pthread_mutex_unlock(&test_lock);
}

/* Reads one of the fake's counters. */
static unsigned
read_count(
	const unsigned *counter)
{
	unsigned value;

	/* Under the fake's lock. */
	pthread_mutex_lock(&test_lock);

	value = *counter;

	pthread_mutex_unlock(&test_lock);
	return value;
}

/* Counts a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A failure is printed. */
	if (condition)
		return;
	printf("FAIL %s\n", what);
	failures++;
}

/* Takes the messages into the main thread's view, as the compositor applies them, and frees them. */
static void
take(
	struct kwl_render_thread *render,
	struct test_view *view)
{
	struct kwl_render_message *messages;
	struct kwl_render_message *message;

	/* Each message, oldest first. */
	messages = kwl_render_take(render);
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		switch (message->kind) {
		case KWL_RENDER_READY:
			view->ready_valid = 1U;
			view->ready_number = message->number;
			view->readies++;
			break;
		case KWL_RENDER_ACQUIRE_FAILED:
			view->acquire_failed++;
			view->acquire_result = message->result;
			break;
		case KWL_RENDER_COMPLETED:
			if (view->completed < TEST_LOG) {
				view->completed_slots[view->completed] = message->job.slot;
				view->completed_present[view->completed] = message->outcome.present_result;
			}
			view->completed++;
			if (view->in_flight > 0U)
				view->in_flight--;
			break;
		case KWL_RENDER_TASK_DONE:
			if (view->tasks < TEST_LOG) {
				view->task_numbers[view->tasks] = (unsigned)message->number;
				view->task_results[view->tasks] = message->result;
			}
			view->tasks++;
			break;
		case KWL_RENDER_PARKED:
			view->parked++;
			break;
		default:
			break;
		}
		free(message);
	}
}

/* Gives one of the view's counts. */
static unsigned
view_count(
	const struct test_view *view,
	enum test_count which)
{
	/* By what the test waits for. */
	switch (which) {
	case TEST_READIES:
		return view->readies;
	case TEST_COMPLETED:
		return view->completed;
	case TEST_TASKS:
		return view->tasks;
	case TEST_PARKED:
		return view->parked;
	case TEST_ACQUIRE_FAILED:
		return view->acquire_failed;
	default:
		break;
	}
	return 0U;
}

/* Takes messages until a count reaches its target, as the compositor waits for the thread (kwl_compose_complete); returns whether it did in time. */
static int
pump(
	struct kwl_render_thread *render,
	struct test_view *view,
	enum test_count which,
	unsigned target)
{
	unsigned waited;

	/* Until the count, or the test's patience ends. */
	for (waited = 0U; waited < TEST_WAIT_MS; waited += 10U) {
		take(render, view);
		if (view_count(view, which) >= target)
			return 1;
		(void)kwl_render_wait_message(render, 10U);
	}

	/* The count never came. */
	take(render, view);
	if (view_count(view, which) >= target)
		return 1;
	return 0;
}

/* Takes messages for a while, to see that something does not happen. */
static void
quiet(
	struct kwl_render_thread *render,
	struct test_view *view)
{
	/* The thread runs meanwhile. */
	sleep_ms(TEST_QUIET_MS);
	take(render, view);
}

/* Raises a want of a kind (none of the heads). */
static void
want(
	struct kwl_render_thread *render,
	enum kwl_render_kind kind)
{
	uint64_t number;

	/* The number is the ready's to answer. */
	kwl_render_set_want(render, kind, NULL, 0U, &number);
}

/* Answers the ready outstanding with a list for a slot, as the compositor does after its walk: one more in flight. */
static void
answer_list(
	struct kwl_render_thread *render,
	struct test_view *view,
	enum kwl_render_kind kind,
	unsigned slot)
{
	struct kwl_render_job job;

	/* The job, for the ready outstanding. */
	check(view->ready_valid, "a ready to answer");
	memset(&job, 0, sizeof(job));
	job.number = view->ready_number;
	job.kind = kind;
	job.slot = slot;
	kwl_render_answer(render, view->ready_number, KWL_RENDER_ANSWER_LIST, &job);
	view->ready_valid = 0U;
	view->in_flight++;
}

/* Stops the render thread. */
static void *
stop_main(
	void *argument)
{
	/* Returns once the thread has ended. */
	kwl_render_stop(argument);
	return NULL;
}

/* Sleeps a while. */
static void
sleep_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	/* The whole time. */
	wait.tv_sec = (time_t)(milliseconds / 1000U);
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
