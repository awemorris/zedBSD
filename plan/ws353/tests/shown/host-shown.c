/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the shown thread and of the render thread's present
 * fences (wayland/render-thread.c), with fake fences and a fake frame run:
 * the presents of a frame get fences and go to the shown thread, which
 * tells each once its fence is signalled, a swapchain's in present order
 * and a slow head apart from the output; a present not queued is told
 * failed at once and its fence is spare again; a present made without a
 * fence (none could be made) is told at once; without a shown thread the
 * presents are told with the frame's completion; a failed wait (device
 * lost) tells every present failed; a stop waits for the presents given;
 * the thread sleeps with nothing given and waits once for each present.
 */

#include "userland/desktop/wayland/render-thread.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The most fences one case makes. */
#define TEST_FENCES	64U

/*
 * A fake fence: signalled by the test, reset by the shown thread, made and
 * destroyed through the operations (counted).  Under fake_lock.
 */
struct fake_fence {
	unsigned signalled;
	unsigned used;
};

/* The fake fences' lock and the signal a waiter waits for. */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed = PTHREAD_COND_INITIALIZER;

/* Whether the device is lost (a wait then fails), and the fences made and destroyed; under fake_lock. */
static unsigned fake_lost;
static unsigned fake_made;
static unsigned fake_destroyed;
static unsigned fake_waits;

/* What the fake run does: the presents' results, and whether making a fence fails. */
static VkResult run_present_result;
static VkResult run_head_result;
static VkResult run_result;
static unsigned fake_create_fails;

/* The fences the last run was given: the output's and each head's. */
static VkFence run_present_fence;
static VkFence run_head_fences[KWL_RENDER_HEADS];

/* The number of failed checks. */
static unsigned test_failures;

/* Messages taken while drawing a frame that the case has not looked at yet, oldest first. */
static struct kwl_render_message *stash;
static struct kwl_render_message *stash_tail;

static void check(int condition, const char *what);
static VkResult fake_create(void *context, VkFence *fence);
static void fake_destroy(void *context, VkFence fence);
static VkResult fake_wait(void *context, const VkFence *fences, unsigned count);
static VkResult fake_status(void *context, VkFence fence);
static VkResult fake_reset(void *context, VkFence fence);
static void fake_signal(VkFence fence);
static VkResult op_acquire_output(void *context, uint64_t timeout_ns, uint32_t *image, uint64_t *cycles, uint64_t *at_us);
static VkResult op_acquire_head(void *context, unsigned slot, uint32_t *image);
static void op_run(void *context, const struct kwl_render_job *job, struct kwl_render_outcome *outcome);
static VkResult op_wait(void *context, unsigned slot, uint64_t timeout_ns);
static VkResult op_submit_task(void *context, VkCommandBuffer command, VkFence fence);
static VkResult op_wait_task(void *context, VkFence fence, uint64_t timeout_ns);
static void setup(struct kwl_render_thread *render, struct kwl_shown *shown, unsigned with_shown);
static void teardown(struct kwl_render_thread *render, struct kwl_shown *shown, unsigned with_shown);
static uint64_t frame(struct kwl_render_thread *render, unsigned head);
static unsigned take_shown(struct kwl_render_thread *render, uint64_t *numbers, unsigned *keys, VkResult *results, unsigned capacity, unsigned *completed);
static unsigned spare_count(struct kwl_shown *shown);
static void case_order(void);
static void case_slow_head(void);
static void case_not_queued(void);
static void case_no_fence(void);
static void case_no_shown(void);
static void case_lost(void);
static void case_thread(void);
static void *shown_runner(void *argument);

/* Runs every case and says PASS or how many checks failed. */
int
main(
	void)
{
	/* Each case with its own threads' state. */
	case_order();
	case_slow_head();
	case_not_queued();
	case_no_fence();
	case_no_shown();
	case_lost();
	case_thread();

	/* The verdict. */
	if (test_failures != 0U) {
		printf("FAIL shown: %u checks\n", test_failures);
		return 1;
	}

	/* Every check held. */
	printf("PASS shown\n");
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;
	printf("FAIL %s\n", what);
	test_failures++;
}

/* Makes an unsignalled fake fence, or fails when told to. */
static VkResult
fake_create(
	void *context,
	VkFence *fence)
{
	struct fake_fence *fake;

	/* Making a fence fails while the case says so. */
	(void)context;
	pthread_mutex_lock(&fake_lock);
	if (fake_create_fails) {
		pthread_mutex_unlock(&fake_lock);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* Counted. */
	fake_made++;
	pthread_mutex_unlock(&fake_lock);

	/* A new one. */
	fake = calloc(1, sizeof(*fake));
	if (fake == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*fence = (VkFence)fake;
	return VK_SUCCESS;
}

/* Destroys a fake fence (counted). */
static void
fake_destroy(
	void *context,
	VkFence fence)
{
	/* Counted, then freed. */
	(void)context;
	pthread_mutex_lock(&fake_lock);
	fake_destroyed++;
	pthread_mutex_unlock(&fake_lock);
	free((struct fake_fence *)fence);
}

/* Waits until any of the fences is signalled, or the device is lost. */
static VkResult
fake_wait(
	void *context,
	const VkFence *fences,
	unsigned count)
{
	struct fake_fence *fake;
	unsigned index;
	unsigned any;

	/* Until one is signalled. */
	(void)context;
	pthread_mutex_lock(&fake_lock);
	fake_waits++;
	for (;;) {
		any = 0U;
		for (index = 0U; index < count; index++) {
			fake = (struct fake_fence *)fences[index];
			if (fake->signalled)
				any = 1U;
		}

		/* One signalled, or the device lost, ends the wait. */
		if (any || fake_lost)
			break;
		pthread_cond_wait(&fake_changed, &fake_lock);
	}

	/* The look is over. */
	pthread_mutex_unlock(&fake_lock);

	/* Lost, or one is signalled. */
	if (!any)
		return VK_ERROR_DEVICE_LOST;
	return VK_SUCCESS;
}

/* Tells whether a fake fence is signalled. */
static VkResult
fake_status(
	void *context,
	VkFence fence)
{
	struct fake_fence *fake;
	unsigned signalled;

	/* Looked at under the lock. */
	(void)context;
	fake = (struct fake_fence *)fence;
	pthread_mutex_lock(&fake_lock);
	signalled = fake->signalled;
	pthread_mutex_unlock(&fake_lock);
	if (signalled)
		return VK_SUCCESS;
	return VK_NOT_READY;
}

/* Resets a fake fence. */
static VkResult
fake_reset(
	void *context,
	VkFence fence)
{
	struct fake_fence *fake;

	/* Unsignalled again. */
	(void)context;
	fake = (struct fake_fence *)fence;
	pthread_mutex_lock(&fake_lock);
	fake->signalled = 0U;
	pthread_mutex_unlock(&fake_lock);
	return VK_SUCCESS;
}

/* Signals a fake fence (the display took its present). */
static void
fake_signal(
	VkFence fence)
{
	struct fake_fence *fake;

	/* Signalled, and a waiter woken. */
	fake = (struct fake_fence *)fence;
	pthread_mutex_lock(&fake_lock);
	fake->signalled = 1U;
	pthread_cond_broadcast(&fake_changed);
	pthread_mutex_unlock(&fake_lock);
}

/* The output's image: always one free. */
static VkResult
op_acquire_output(
	void *context,
	uint64_t timeout_ns,
	uint32_t *image,
	uint64_t *cycles,
	uint64_t *at_us)
{
	/* Image 0, at once. */
	(void)context;
	(void)timeout_ns;
	*image = 0U;
	*cycles = 0U;
	*at_us = 0U;
	return VK_SUCCESS;
}

/* A head's image: always one free. */
static VkResult
op_acquire_head(
	void *context,
	unsigned slot,
	uint32_t *image)
{
	/* Image 0, at once. */
	(void)context;
	(void)slot;
	*image = 0U;
	return VK_SUCCESS;
}

/* Runs a frame: notes its present fences and gives the case's results. */
static void
op_run(
	void *context,
	const struct kwl_render_job *job,
	struct kwl_render_outcome *outcome)
{
	unsigned index;

	/* The fences the presents were given. */
	(void)context;
	memset(outcome, 0, sizeof(*outcome));
	run_present_fence = job->present_fence;
	for (index = 0U; index < KWL_RENDER_HEADS; index++)
		run_head_fences[index] = job->head_fences[index];

	/* The case's results: a failed run presents nothing. */
	outcome->result = run_result;
	if (run_result != VK_SUCCESS)
		return;
	outcome->present_result = run_present_result;
	for (index = 0U; index < KWL_RENDER_HEADS; index++) {
		if (!job->heads[index])
			continue;
		outcome->heads[index] = 1U;
		outcome->head_results[index] = run_head_result;
	}
}

/* A job's fence: always signalled already. */
static VkResult
op_wait(
	void *context,
	unsigned slot,
	uint64_t timeout_ns)
{
	/* Done. */
	(void)context;
	(void)slot;
	(void)timeout_ns;
	return VK_SUCCESS;
}

/* No task is given in this test. */
static VkResult
op_submit_task(
	void *context,
	VkCommandBuffer command,
	VkFence fence)
{
	/* Nothing. */
	(void)context;
	(void)command;
	(void)fence;
	return VK_SUCCESS;
}

/* No task is given in this test. */
static VkResult
op_wait_task(
	void *context,
	VkFence fence,
	uint64_t timeout_ns)
{
	/* Nothing. */
	(void)context;
	(void)fence;
	(void)timeout_ns;
	return VK_SUCCESS;
}

/* Makes a render thread (stepped by the test) and, when asked, a shown thread it gives its fences to. */
static void
setup(
	struct kwl_render_thread *render,
	struct kwl_shown *shown,
	unsigned with_shown)
{
	struct kwl_render_ops render_ops;
	struct kwl_shown_ops shown_ops;
	int error;

	/* The fakes' state. */
	fake_lost = 0U;
	fake_made = 0U;
	fake_destroyed = 0U;
	fake_waits = 0U;
	fake_create_fails = 0U;
	run_result = VK_SUCCESS;
	run_present_result = VK_SUCCESS;
	run_head_result = VK_SUCCESS;

	/* The render thread's operations. */
	memset(&render_ops, 0, sizeof(render_ops));
	render_ops.acquire_output = op_acquire_output;
	render_ops.acquire_head = op_acquire_head;
	render_ops.run = op_run;
	render_ops.wait = op_wait;
	render_ops.submit_task = op_submit_task;
	render_ops.wait_task = op_wait_task;
	error = kwl_render_init(render, &render_ops, NULL, -1);
	check(error == 0, "render init");

	/* The shown thread's, given to the render thread. */
	if (!with_shown)
		return;
	memset(&shown_ops, 0, sizeof(shown_ops));
	shown_ops.create = fake_create;
	shown_ops.destroy = fake_destroy;
	shown_ops.wait = fake_wait;
	shown_ops.status = fake_status;
	shown_ops.reset = fake_reset;
	error = kwl_shown_init(shown, &shown_ops, NULL, render);
	check(error == 0, "shown init");
	render->shown = shown;
}

/* Frees both threads' state; every fence made is destroyed by then. */
static void
teardown(
	struct kwl_render_thread *render,
	struct kwl_shown *shown,
	unsigned with_shown)
{
	struct kwl_render_message *messages;
	struct kwl_render_message *message;

	/* The messages left, those kept too. */
	messages = kwl_render_take(render);
	if (stash != NULL) {
		stash_tail->next = messages;
		messages = stash;
		stash = NULL;
		stash_tail = NULL;
	}
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		free(message);
	}

	/* The shown thread's spares, then the render thread. */
	if (with_shown) {
		kwl_shown_destroy(shown);
		check(fake_made == fake_destroyed, "every fence made is destroyed");
	}

	/* The render thread's state. */
	kwl_render_destroy(render);
}

/*
 * Draws one frame through the render thread's steps: the want (with a head
 * in slot head when head is below KWL_RENDER_HEADS), the ready, the list,
 * the run and the completion.  Returns the job's number.
 */
static uint64_t
frame(
	struct kwl_render_thread *render,
	unsigned head)
{
	struct kwl_render_message *messages;
	struct kwl_render_message *message;
	struct kwl_render_job job;
	enum kwl_render_step step;
	unsigned heads[1];
	unsigned count;
	uint64_t number;

	/* The messages so far wait for the case. */
	messages = kwl_render_take(render);
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		message->next = NULL;
		if (stash_tail == NULL) {
			stash = message;
		} else {
			stash_tail->next = message;
		}

		/* The new end. */
		stash_tail = message;
	}

	/* The want, and the ready it brings. */
	heads[0] = head;
	count = 0U;
	if (head < KWL_RENDER_HEADS)
		count = 1U;
	kwl_render_set_want(render, KWL_RENDER_FRAME, heads, count, &number);
	step = kwl_render_step(render);
	check(step == KWL_RENDER_STEP_WORKED, "the want brings a ready");
	messages = kwl_render_take(render);
	check(messages != NULL && messages->kind == KWL_RENDER_READY, "a ready");
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		free(message);
	}

	/* The list: run, then its completion (the fake job's fence is done). */
	memset(&job, 0, sizeof(job));
	job.number = number;
	job.kind = KWL_RENDER_FRAME;
	job.slot = 0U;
	if (count != 0U)
		job.heads[head] = 1U;
	kwl_render_answer(render, number, KWL_RENDER_ANSWER_LIST, &job);
	step = kwl_render_step(render);
	check(step == KWL_RENDER_STEP_WORKED, "the list runs");
	step = kwl_render_step(render);
	check(step == KWL_RENDER_STEP_WORKED, "the job completes");

	/* Its number. */
	return number;
}

/*
 * Takes the render thread's messages: the shown ones' numbers, keys and
 * results (up to capacity), and how many completions came.  Returns how
 * many shown messages came.
 */
static unsigned
take_shown(
	struct kwl_render_thread *render,
	uint64_t *numbers,
	unsigned *keys,
	VkResult *results,
	unsigned capacity,
	unsigned *completed)
{
	struct kwl_render_message *messages;
	struct kwl_render_message *message;
	unsigned count;

	/* Each message, in order: those kept while frames were drawn first. */
	count = 0U;
	*completed = 0U;
	messages = kwl_render_take(render);
	if (stash != NULL) {
		stash_tail->next = messages;
		messages = stash;
		stash = NULL;
		stash_tail = NULL;
	}
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		if (message->kind == KWL_RENDER_COMPLETED)
			(*completed)++;
		if (message->kind == KWL_RENDER_SHOWN) {
			if (count < capacity) {
				numbers[count] = message->number;
				keys[count] = message->shown_key;
				results[count] = message->result;
			}

			/* Counted, kept or not. */
			count++;
		}

		/* Done with. */
		free(message);
	}

	/* The shown ones counted. */
	return count;
}

/* Counts the shown thread's spare fences. */
static unsigned
spare_count(
	struct kwl_shown *shown)
{
	struct kwl_shown_present *present;
	unsigned count;

	/* Under its lock. */
	count = 0U;
	pthread_mutex_lock(&shown->lock);
	for (present = shown->spares; present != NULL; present = present->next)
		count++;
	pthread_mutex_unlock(&shown->lock);
	return count;
}

/*
 * The output's presents of three frames are told shown in present order,
 * one wait each, nothing before its fence is signalled, and the fences
 * are used again (three frames, at most two fences).
 */
static void
case_order(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	VkFence fences[3];
	uint64_t numbers[8];
	uint64_t frames[3];
	unsigned keys[8];
	VkResult results[8];
	enum kwl_render_step step;
	unsigned completed;
	unsigned count;
	unsigned index;

	/* Two frames presented, neither signalled: nothing told, the thread waits (not stepped). */
	setup(&render, &shown, 1U);
	frames[0] = frame(&render, KWL_RENDER_HEADS);
	fences[0] = run_present_fence;
	frames[1] = frame(&render, KWL_RENDER_HEADS);
	fences[1] = run_present_fence;
	check(fences[0] != VK_NULL_HANDLE && fences[1] != VK_NULL_HANDLE && fences[0] != fences[1], "order: each present has a fence of its own");
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 0U && completed == 2U, "order: completions, nothing shown before the fences");

	/* Both signalled: the first is told (the oldest only), then the second. */
	fake_signal(fences[0]);
	fake_signal(fences[1]);
	step = kwl_shown_step(&shown);
	check(step == KWL_RENDER_STEP_WORKED, "order: a step tells");
	step = kwl_shown_step(&shown);
	check(step == KWL_RENDER_STEP_WORKED, "order: a second step tells");
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 2U, "order: two shown");
	check(numbers[0] == frames[0] && numbers[1] == frames[1], "order: in present order");
	check(keys[0] == KWL_RENDER_SHOWN_OUTPUT && keys[1] == KWL_RENDER_SHOWN_OUTPUT, "order: the output's");
	check(results[0] == VK_SUCCESS && results[1] == VK_SUCCESS, "order: shown");
	check(spare_count(&shown) == 2U, "order: both fences spare again");

	/* A third frame takes a spare fence (no new one), and is told once signalled. */
	frames[2] = frame(&render, KWL_RENDER_HEADS);
	fences[2] = run_present_fence;
	check(fake_made == 2U, "order: a spare fence used again");
	fake_signal(fences[2]);
	step = kwl_shown_step(&shown);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 1U && numbers[0] == frames[2], "order: the third shown");

	/* Nothing waits: the thread is blocked; one wait for each present. */
	step = kwl_shown_step(&shown);
	check(step == KWL_RENDER_STEP_BLOCKED, "order: blocked with nothing given");
	check(fake_waits == 3U, "order: one wait for each present");
	check(shown.counts.shown == 3U, "order: three counted shown");

	/* The stop with nothing given ends at once. */
	kwl_shown_stop(&shown);
	step = kwl_shown_step(&shown);
	check(step == KWL_RENDER_STEP_STOP, "order: the stop");
	for (index = 0U; index < 3U; index++)
		(void)fences[index];
	teardown(&render, &shown, 1U);
}

/*
 * A frame on the output and a slow head: the output's presents of later
 * frames are told while the head's first is not signalled (one wait for
 * both swapchains), and the head's is told once signalled.
 */
static void
case_slow_head(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	uint64_t numbers[8];
	uint64_t first;
	uint64_t second;
	unsigned keys[8];
	VkResult results[8];
	VkFence head_fence;
	VkFence output_fences[2];
	unsigned completed;
	unsigned count;

	/* The first frame on the output and the head in slot 2, the second on the output alone. */
	setup(&render, &shown, 1U);
	first = frame(&render, 2U);
	output_fences[0] = run_present_fence;
	head_fence = run_head_fences[2];
	check(head_fence != VK_NULL_HANDLE, "slow head: the head's present has a fence");
	second = frame(&render, KWL_RENDER_HEADS);
	output_fences[1] = run_present_fence;

	/* The output's two presents signalled, the head's not: both output's are told. */
	fake_signal(output_fences[0]);
	fake_signal(output_fences[1]);
	(void)kwl_shown_step(&shown);
	(void)kwl_shown_step(&shown);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 2U, "slow head: the output's two told");
	check(numbers[0] == first && keys[0] == KWL_RENDER_SHOWN_OUTPUT, "slow head: the first frame's output");
	check(numbers[1] == second && keys[1] == KWL_RENDER_SHOWN_OUTPUT, "slow head: the second frame's output");

	/* The head's then. */
	fake_signal(head_fence);
	(void)kwl_shown_step(&shown);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 1U && numbers[0] == first && keys[0] == 3U, "slow head: the head's told with its key");

	/* Done. */
	kwl_shown_stop(&shown);
	(void)kwl_shown_step(&shown);
	teardown(&render, &shown, 1U);
}

/*
 * A present that was not queued (out of memory) is told failed at once, and
 * its fence is spare again untouched; a refused present (out of date) is
 * queued and told once its fence is signalled, with its result; a failed
 * run tells its presents failed.
 */
static void
case_not_queued(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	uint64_t numbers[8];
	uint64_t number;
	unsigned keys[8];
	VkResult results[8];
	unsigned completed;
	unsigned count;

	/* Out of memory: told at once, failed, and the fence spare. */
	setup(&render, &shown, 1U);
	run_present_result = VK_ERROR_OUT_OF_HOST_MEMORY;
	number = frame(&render, KWL_RENDER_HEADS);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 1U && numbers[0] == number && results[0] == VK_ERROR_OUT_OF_HOST_MEMORY, "not queued: told failed at once");
	check(spare_count(&shown) == 1U, "not queued: its fence spare again");

	/* Out of date: queued, told once signalled, with its result. */
	run_present_result = VK_ERROR_OUT_OF_DATE_KHR;
	number = frame(&render, KWL_RENDER_HEADS);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 0U, "out of date: not told before its fence");
	check(spare_count(&shown) == 0U, "out of date: the fence used again, with the shown thread");
	fake_signal(run_present_fence);
	(void)kwl_shown_step(&shown);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 1U && numbers[0] == number && results[0] == VK_ERROR_OUT_OF_DATE_KHR, "out of date: told with its result");

	/* A failed run (its submission): the output's and the head's presents told failed at once. */
	run_present_result = VK_SUCCESS;
	run_result = VK_ERROR_DEVICE_LOST;
	number = frame(&render, 0U);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 2U && results[0] == VK_ERROR_DEVICE_LOST && results[1] == VK_ERROR_DEVICE_LOST, "failed run: both presents told failed");
	check(keys[0] == KWL_RENDER_SHOWN_OUTPUT && keys[1] == 1U && numbers[1] == number, "failed run: the output's and the head's");
	check(spare_count(&shown) == 2U, "failed run: both fences spare");

	/* Done. */
	kwl_shown_stop(&shown);
	(void)kwl_shown_step(&shown);
	teardown(&render, &shown, 1U);
}

/* No fence can be made: the present goes without one and is told shown at once. */
static void
case_no_fence(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	uint64_t numbers[8];
	uint64_t number;
	unsigned keys[8];
	VkResult results[8];
	unsigned completed;
	unsigned count;

	/* Making fences fails. */
	setup(&render, &shown, 1U);
	fake_create_fails = 1U;
	number = frame(&render, KWL_RENDER_HEADS);
	check(run_present_fence == VK_NULL_HANDLE, "no fence: the present has none");
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 1U && numbers[0] == number && results[0] == VK_SUCCESS, "no fence: told shown at once");

	/* Done. */
	kwl_shown_stop(&shown);
	(void)kwl_shown_step(&shown);
	teardown(&render, &shown, 1U);
}

/* Without a shown thread the presents are told with the completion, before it, and the run gets no fence. */
static void
case_no_shown(void)
{
	struct kwl_render_thread render;
	struct kwl_render_messages_seen {
		unsigned shown_before;
		unsigned completed;
	} seen;
	struct kwl_render_message *messages;
	struct kwl_render_message *message;
	uint64_t number;

	/* A frame on the output and the head in slot 0. */
	setup(&render, NULL, 0U);
	number = frame(&render, 0U);
	check(run_present_fence == VK_NULL_HANDLE && run_head_fences[0] == VK_NULL_HANDLE, "no shown: no fences");

	/* Two shown messages, then the completion. */
	memset(&seen, 0, sizeof(seen));
	messages = kwl_render_take(&render);
	if (stash != NULL) {
		stash_tail->next = messages;
		messages = stash;
		stash = NULL;
		stash_tail = NULL;
	}
	while (messages != NULL) {
		message = messages;
		messages = message->next;
		if (message->kind == KWL_RENDER_SHOWN && message->number == number && !seen.completed)
			seen.shown_before++;
		if (message->kind == KWL_RENDER_COMPLETED)
			seen.completed++;
		free(message);
	}

	/* Both presents before the completion. */
	check(seen.shown_before == 2U && seen.completed == 1U, "no shown: the output's and the head's told with the completion");
	teardown(&render, NULL, 0U);
}

/* A failed wait (the device lost) tells every present waited for failed, and destroys their fences. */
static void
case_lost(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	uint64_t numbers[8];
	unsigned keys[8];
	VkResult results[8];
	unsigned completed;
	unsigned count;

	/* Two frames, then the device lost. */
	setup(&render, &shown, 1U);
	(void)frame(&render, KWL_RENDER_HEADS);
	(void)frame(&render, 1U);
	pthread_mutex_lock(&fake_lock);
	fake_lost = 1U;
	pthread_mutex_unlock(&fake_lock);
	(void)kwl_shown_step(&shown);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 3U, "lost: the three presents told");
	check(results[0] == VK_ERROR_DEVICE_LOST && results[2] == VK_ERROR_DEVICE_LOST, "lost: failed");
	check(fake_destroyed == 3U && shown.counts.failed == 3U, "lost: their fences destroyed");

	/* Done. */
	kwl_shown_stop(&shown);
	(void)kwl_shown_step(&shown);
	teardown(&render, &shown, 1U);
}

/* Runs the shown thread's loop for case_thread. */
static void *
shown_runner(
	void *argument)
{
	/* The thread's own loop. */
	return kwl_shown_main(argument);
}

/*
 * The shown thread on a thread of its own: it sleeps with nothing given,
 * tells presents as they are signalled, and a stop asked while presents
 * wait ends it only after they are told.
 */
static void
case_thread(void)
{
	struct kwl_render_thread render;
	struct kwl_shown shown;
	pthread_t thread;
	VkFence fences[2];
	uint64_t numbers[8];
	unsigned keys[8];
	VkResult results[8];
	unsigned completed;
	unsigned count;
	unsigned sleeps;
	unsigned rounds;
	int error;

	/* The thread, with nothing given: it sleeps and waits for no fence. */
	setup(&render, &shown, 1U);
	error = pthread_create(&thread, NULL, shown_runner, &shown);
	check(error == 0, "thread: started");
	usleep(50000);
	pthread_mutex_lock(&shown.lock);
	sleeps = (unsigned)shown.counts.sleeps;
	pthread_mutex_unlock(&shown.lock);
	check(sleeps == 1U, "thread: one sleep with nothing given");
	pthread_mutex_lock(&fake_lock);
	check(fake_waits == 0U, "thread: no wait for a fence while idle");
	pthread_mutex_unlock(&fake_lock);

	/* Two frames given, the stop asked before their fences: the thread waits for them. */
	(void)frame(&render, KWL_RENDER_HEADS);
	fences[0] = run_present_fence;
	(void)frame(&render, KWL_RENDER_HEADS);
	fences[1] = run_present_fence;
	kwl_shown_stop(&shown);
	usleep(50000);
	count = take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 0U, "thread: nothing told before the fences");

	/* Signalled: both told, then the thread ends. */
	fake_signal(fences[0]);
	fake_signal(fences[1]);
	(void)pthread_join(thread, NULL);
	count = 0U;
	for (rounds = 0U; rounds < 1U; rounds++)
		count += take_shown(&render, numbers, keys, results, 8U, &completed);
	check(count == 2U, "thread: both told before the end");
	teardown(&render, &shown, 1U);
}
