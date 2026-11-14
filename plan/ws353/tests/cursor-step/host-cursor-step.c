/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the cursor thread's waits
 * (userland/desktop/wayland/cursor-thread.c), with fake Vulkan calls whose
 * fences and swapchain images the test hands out.
 *
 * The thread runs on its own; the test plays the main thread (it writes
 * the box), the GPU (it signals the copies' fences) and the display (it
 * frees the presented images).  It checks that the thread waits for one
 * thing at a time and for nothing on a clock but a held move: every acquire
 * is given 0 or UINT64_MAX and blocks only after a change with no image
 * free (no retry), every fence wait while presenting is given UINT64_MAX
 * and comes at once after the present, the box's copy a copy read is let go
 * (and the main thread woken) once the copy is done, an idle thread calls
 * nothing, a park is answered at once when idle and once the blocked
 * acquire returns otherwise, and a failed present still waits for its copy.
 */

#include "userland/desktop/wayland/cursor-thread.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How many swapchain images the fake plane has, and how long the test waits for something it expects. */
#define TEST_IMAGES	3U
#define TEST_WAIT_MS	2000U

/* How long the test watches for something that must not happen. */
#define TEST_QUIET_MS	150U

/* How long a move of the same image is held after a present in the test, in microseconds. */
#define TEST_HOLD_US	60000U

/* The plane's place on the output the test moves the cursor to. */
#define TEST_X	100
#define TEST_Y	50

/* What the test waits to see counted by the fakes. */
enum test_count {
	TEST_PRESENTS = 1,
	TEST_SUBMITS,
	TEST_ACQUIRE_CALLS,
	TEST_ACQUIRE_BLOCKED,
	TEST_FENCE_WAITING
};

/*
 * The fake GPU and display, under test_lock (test_changed signals every
 * change): each image's fence (signalled or not; a submit clears it, the
 * test sets it), each image's state (busy from its acquire until the
 * display lets it go: at once after its present unless keep_busy, else
 * when the test frees it), what the next present reports, and the counts
 * of the calls, of the calls blocked now, and of the waits given a timeout
 * the thread must not use.
 */
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed = PTHREAD_COND_INITIALIZER;
static unsigned fence_signalled[TEST_IMAGES];
static unsigned image_busy[TEST_IMAGES];
static unsigned keep_busy;
static VkResult present_next;
static unsigned presents;
static int32_t present_x;
static int32_t present_y;
static unsigned submits;
static unsigned acquire_calls;
static unsigned acquire_blocks;
static unsigned acquire_blocked;
static unsigned fence_waits;
static unsigned fence_waiting;
static unsigned stop_waits;
static unsigned stopping;
static unsigned bad_timeouts;

/* The compositor's queue lock, which the thread takes around its submits and presents. */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

/* The checks that failed, counted by the test's thread only. */
static unsigned failures;

/* The fake plane's images and the box's two copies, which the thread reads only by address. */
static VkImage test_images[TEST_IMAGES];
static struct kwl_import test_copies[KWL_CURSOR_COPIES];

static VkFence fence_of(unsigned index);
static unsigned fence_index(VkFence fence);
static unsigned read_count(enum test_count which);
static int wait_count(enum test_count which, unsigned target);
static void signal_fence(unsigned index);
static void free_image(unsigned index);
static void set_keep_busy(unsigned keep);
static void box_init(struct kwl_cursor_box *box);
static void box_show(struct kwl_cursor_box *box, const struct kwl_import *image, uint32_t version, int32_t x, int32_t y);
static void box_move(struct kwl_cursor_box *box, int32_t x, int32_t y);
static unsigned box_pins(struct kwl_cursor_box *box, unsigned copy);
static void thread_init(struct kwl_cursor_thread *cursor, struct kwl_cursor_box *box, int wake);
static unsigned wakes_read(int descriptor);
static void *free_later(void *argument);
static void check(int condition, const char *what);
static void sleep_ms(unsigned milliseconds);
static uint64_t now_us(void);

int
main(void)
{
	struct kwl_cursor_thread cursor;
	struct kwl_cursor_box box;
	pthread_t freer;
	uint64_t started;
	uint64_t elapsed;
	unsigned before;
	unsigned calls_before;
	unsigned index;
	int pipes[2];
	int met;
	int error;

	/* The main thread's wake pipe, never blocking; every fence signalled and every image free, as made. */
	error = pipe(pipes);
	check(error == 0, "pipe");
	(void)fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(pipes[1], F_SETFL, O_NONBLOCK);
	for (index = 0; index < TEST_IMAGES; index++)
		fence_signalled[index] = 1U;
	present_next = VK_SUCCESS;

	/* The box and the thread, started with the plane empty. */
	box_init(&box);
	thread_init(&cursor, &box, pipes[1]);
	error = kwl_cursor_thread_start(&cursor);
	check(error == 0, "start");

	/*
	 * E: the empty plane is presented first: the clearing copy is
	 * submitted, the present made, then the copy's fence is waited for
	 * with no deadline, and nothing more until it is signalled.
	 */
	met = wait_count(TEST_FENCE_WAITING, 1U);
	check(met, "E: the copy's fence is waited for");
	check(read_count(TEST_PRESENTS) == 1U, "E: after the present");
	check(read_count(TEST_SUBMITS) == 1U, "E: one copy");
	sleep_ms(TEST_QUIET_MS);
	check(read_count(TEST_ACQUIRE_CALLS) == 1U, "E: nothing else while the copy runs");
	signal_fence(0U);
	met = wait_count(TEST_FENCE_WAITING, 0U);
	check(met, "E: the wait returned");

	/* I: an idle thread calls nothing. */
	calls_before = read_count(TEST_ACQUIRE_CALLS);
	sleep_ms(TEST_QUIET_MS);
	check(read_count(TEST_ACQUIRE_CALLS) == calls_before, "I: no acquire while idle");
	check(read_count(TEST_PRESENTS) == 1U, "I: no present while idle");

	/*
	 * C: a client's cursor through the box's copy 0: the copy pins it
	 * while the GPU reads it, and the copy's end lets it go and wakes the
	 * main thread, which found both copies in use.
	 */
	(void)wakes_read(pipes[0]);
	box_show(&box, &test_copies[0], 1U, TEST_X, TEST_Y);
	met = wait_count(TEST_FENCE_WAITING, 1U);
	check(met, "C: the copy's fence is waited for");
	check(read_count(TEST_PRESENTS) == 2U, "C: presented before the wait");
	check(box_pins(&box, 0U) == 1U, "C: the box's copy is pinned while the GPU reads it");
	pthread_mutex_lock(&box.lock);

	box.copy_wanted = 1U;

	pthread_mutex_unlock(&box.lock);
	sleep_ms(TEST_QUIET_MS);
	check(box_pins(&box, 0U) == 1U, "C: still pinned before the copy is done");
	check(wakes_read(pipes[0]) == 0U, "C: no wake before the copy is done");
	signal_fence(0U);
	met = wait_count(TEST_FENCE_WAITING, 0U);
	check(met, "C: the wait returned");
	sleep_ms(20U);
	check(box_pins(&box, 0U) == 0U, "C: let go once the copy is done");
	check(wakes_read(pipes[0]) == 1U, "C: the main thread woken once");
	pthread_mutex_lock(&test_lock);

	check(present_x == TEST_X && present_y == TEST_Y, "C: presented at the place");

	pthread_mutex_unlock(&test_lock);

	/*
	 * H: the first move after a still refresh goes at once; a move of the
	 * same image right after its present is held until the refresh's end
	 * (the one deadline), then presented without a copy.
	 */
	before = read_count(TEST_PRESENTS);
	box_move(&box, TEST_X + 5, TEST_Y);
	met = wait_count(TEST_PRESENTS, before + 1U);
	check(met, "H: the first move goes at once");
	before = read_count(TEST_PRESENTS);
	started = now_us();
	box_move(&box, TEST_X + 10, TEST_Y);
	sleep_ms(10U);
	check(read_count(TEST_PRESENTS) == before, "H: the move is held");
	met = wait_count(TEST_PRESENTS, before + 1U);
	elapsed = now_us() - started;
	check(met, "H: the held move is presented");
	check(elapsed >= TEST_HOLD_US / 2U, "H: after the hold");
	check(read_count(TEST_SUBMITS) == 2U, "H: no copy for a move");
	pthread_mutex_lock(&test_lock);

	check(present_x == TEST_X + 10, "H: at the latest place");

	pthread_mutex_unlock(&test_lock);
	sleep_ms(TEST_QUIET_MS);

	/*
	 * A: with every image held by the display, a change tries once
	 * without waiting and then blocks until one is free, with no retry;
	 * the image freed is presented at once.
	 */
	set_keep_busy(1U);
	pthread_mutex_lock(&test_lock);

	for (index = 0; index < TEST_IMAGES; index++)
		image_busy[index] = 1U;

	pthread_mutex_unlock(&test_lock);
	calls_before = read_count(TEST_ACQUIRE_CALLS);
	before = read_count(TEST_PRESENTS);
	box_move(&box, TEST_X + 20, TEST_Y);
	met = wait_count(TEST_ACQUIRE_BLOCKED, 1U);
	check(met, "A: the acquire blocks");
	sleep_ms(TEST_QUIET_MS);
	check(read_count(TEST_ACQUIRE_CALLS) == calls_before + 2U, "A: once at once, once blocked, no retry");
	check(read_count(TEST_PRESENTS) == before, "A: no present without an image");
	free_image(0U);
	met = wait_count(TEST_PRESENTS, before + 1U);
	check(met, "A: presented once an image was free");
	check(read_count(TEST_ACQUIRE_BLOCKED) == 0U, "A: the acquire returned");
	sleep_ms(TEST_QUIET_MS);

	/* P: a park asked while idle is answered at once, and the thread calls nothing until it ends. */
	free_image(0U);
	set_keep_busy(0U);
	started = now_us();
	error = kwl_cursor_thread_park(&box, 1U, 2000U);
	elapsed = now_us() - started;
	check(error == 0, "P: parked");
	check(elapsed < 100000U, "P: answered at once");
	calls_before = read_count(TEST_ACQUIRE_CALLS);
	box_move(&box, TEST_X + 30, TEST_Y);
	sleep_ms(TEST_QUIET_MS);
	check(read_count(TEST_ACQUIRE_CALLS) == calls_before, "P: nothing while parked");
	before = read_count(TEST_PRESENTS);
	error = kwl_cursor_thread_park(&box, 0U, 0U);
	check(error == 0, "P: the park ends");
	met = wait_count(TEST_PRESENTS, before + 1U);
	check(met, "P: the move after the park is presented");
	sleep_ms(TEST_QUIET_MS);

	/*
	 * B: a park asked while the acquire blocks is answered once the
	 * display frees an image (the plane's previous present ends), well
	 * within the park's 2 s.
	 */
	set_keep_busy(1U);
	pthread_mutex_lock(&test_lock);

	for (index = 0; index < TEST_IMAGES; index++)
		image_busy[index] = 1U;

	pthread_mutex_unlock(&test_lock);
	box_move(&box, TEST_X + 40, TEST_Y);
	met = wait_count(TEST_ACQUIRE_BLOCKED, 1U);
	check(met, "B: the acquire blocks");
	error = pthread_create(&freer, NULL, free_later, NULL);
	check(error == 0, "B: freer");
	started = now_us();
	error = kwl_cursor_thread_park(&box, 1U, 2000U);
	elapsed = now_us() - started;
	check(error == 0, "B: parked");
	check(elapsed >= 150000U, "B: answered after the image came free");
	check(elapsed < 1000000U, "B: within the park's limit");
	(void)pthread_join(freer, NULL);
	set_keep_busy(0U);
	pthread_mutex_lock(&test_lock);

	for (index = 0; index < TEST_IMAGES; index++)
		image_busy[index] = 0U;

	pthread_mutex_unlock(&test_lock);
	error = kwl_cursor_thread_park(&box, 0U, 0U);
	check(error == 0, "B: the park ends");
	sleep_ms(TEST_QUIET_MS);

	/*
	 * N: a new image (copy 1) is copied and presented without a hold, and
	 * its pin let go after the copy, with no wake when none was wanted.
	 */
	(void)wakes_read(pipes[0]);
	before = read_count(TEST_PRESENTS);
	box_show(&box, &test_copies[1], 1U, TEST_X, TEST_Y);
	met = wait_count(TEST_FENCE_WAITING, 1U);
	check(met, "N: the new image's copy is waited for");
	check(read_count(TEST_PRESENTS) == before + 1U, "N: a new image is not held");
	check(box_pins(&box, 1U) == 1U, "N: copy 1 pinned");
	signal_fence(0U);
	met = wait_count(TEST_FENCE_WAITING, 0U);
	check(met, "N: the wait returned");
	sleep_ms(20U);
	check(box_pins(&box, 1U) == 0U, "N: copy 1 let go");
	check(wakes_read(pipes[0]) == 0U, "N: no wake when none was wanted");

	/* S: a stop; its copies' fences are checked with the stop's deadline only. */
	pthread_mutex_lock(&test_lock);

	stopping = 1U;

	pthread_mutex_unlock(&test_lock);
	error = kwl_cursor_thread_stop(&cursor);
	check(error == 0, "S: stopped");
	check(cursor.stop_failed == 0U, "S: the copies were done");
	pthread_mutex_lock(&test_lock);

	check(bad_timeouts == 0U, "every wait while presenting has no deadline, every acquire 0 or none");
	check(stop_waits <= TEST_IMAGES, "the stop's waits");

	pthread_mutex_unlock(&test_lock);
	check(cursor.counts.copies == cursor.counts.copy_waits, "one wait for each copy");
	check(cursor.counts.copies == 3U, "three copies (empty, copy 0, copy 1)");
	check(cursor.counts.acquires_waiting == 2U, "two acquires waited");
	check(cursor.counts.holds == 1U, "one hold, for the held move");
	printf("counts presents=%llu copies=%llu copy_waits=%llu acquires_now=%llu acquires_waiting=%llu sleeps=%llu holds=%llu\n",
	       (unsigned long long)cursor.counts.presents,
	       (unsigned long long)cursor.counts.copies,
	       (unsigned long long)cursor.counts.copy_waits,
	       (unsigned long long)cursor.counts.acquires_now,
	       (unsigned long long)cursor.counts.acquires_waiting,
	       (unsigned long long)cursor.counts.sleeps,
	       (unsigned long long)cursor.counts.holds);

	/*
	 * F: a present that fails (out of date) after a copy still waits for
	 * the copy first, lets its pin go, and tells the main thread.
	 */
	pthread_mutex_lock(&test_lock);

	stopping = 0U;
	present_next = VK_ERROR_OUT_OF_DATE_KHR;

	pthread_mutex_unlock(&test_lock);
	(void)wakes_read(pipes[0]);
	box_init(&box);
	thread_init(&cursor, &box, pipes[1]);
	box_show(&box, &test_copies[0], 2U, TEST_X, TEST_Y);
	error = kwl_cursor_thread_start(&cursor);
	check(error == 0, "F: start");
	met = wait_count(TEST_FENCE_WAITING, 1U);
	check(met, "F: the copy is waited for after the failed present");
	pthread_mutex_lock(&box.lock);

	check(box.failed == 0U, "F: not failed before the copy is done");

	pthread_mutex_unlock(&box.lock);
	signal_fence(0U);
	sleep_ms(TEST_QUIET_MS);
	pthread_mutex_lock(&box.lock);

	check(box.failed == 1U, "F: the failure told");

	pthread_mutex_unlock(&box.lock);
	check(box_pins(&box, 0U) == 0U, "F: the pin let go");
	check(wakes_read(pipes[0]) >= 1U, "F: the main thread woken");
	pthread_mutex_lock(&test_lock);

	stopping = 1U;

	pthread_mutex_unlock(&test_lock);
	error = kwl_cursor_thread_stop(&cursor);
	check(error == 0, "F: stopped");

	/* The verdict. */
	if (failures != 0U) {
		printf("host-cursor-step: FAIL %u\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-cursor-step: PASS\n");
	return 0;
}

/* The fake: an image of the plane at once when one is free, else none (timeout 0) or the wait for one (UINT64_MAX). */
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR(
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t *image)
{
	unsigned index;
	unsigned blocked;

	(void)device;
	(void)swapchain;
	(void)semaphore;
	(void)fence;

	/* The call counted, and its timeout judged. */
	pthread_mutex_lock(&test_lock);

	acquire_calls++;
	if (timeout != 0U && timeout != UINT64_MAX)
		bad_timeouts++;
	blocked = 0U;
	for (;;) {
		/* The first free image. */
		for (index = 0; index < TEST_IMAGES; index++) {
			if (!image_busy[index])
				break;
		}
		if (index < TEST_IMAGES)
			break;

		/* None free: none at once, or the wait for one. */
		if (timeout == 0U) {
			pthread_mutex_unlock(&test_lock);
			return VK_NOT_READY;
		}
		if (!blocked) {
			blocked = 1U;
			acquire_blocks++;
			acquire_blocked++;
			pthread_cond_broadcast(&test_changed);
		}
		pthread_cond_wait(&test_changed, &test_lock);
	}
	if (blocked) {
		acquire_blocked--;
		pthread_cond_broadcast(&test_changed);
	}
	image_busy[index] = 1U;

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: the image is the thread's. */
	*image = index;
	return VK_SUCCESS;
}

/* The fake: the fences unsignalled. */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetFences(
	VkDevice device,
	uint32_t count,
	const VkFence *fences)
{
	uint32_t index;

	(void)device;

	/* Each fence. */
	pthread_mutex_lock(&test_lock);

	for (index = 0; index < count; index++)
		fence_signalled[fence_index(fences[index])] = 0U;

	pthread_mutex_unlock(&test_lock);

	/* Succeeded. */
	return VK_SUCCESS;
}

/* The fake: waits until the fence is signalled, or for a stop's deadline. */
VKAPI_ATTR VkResult VKAPI_CALL
vkWaitForFences(
	VkDevice device,
	uint32_t count,
	const VkFence *fences,
	VkBool32 all,
	uint64_t timeout)
{
	unsigned index;

	(void)device;
	(void)all;

	/* One fence at a time; a deadline only when the thread stops. */
	index = fence_index(fences[0]);
	pthread_mutex_lock(&test_lock);

	if (count != 1U)
		bad_timeouts++;
	if (stopping) {
		stop_waits++;
		if (!fence_signalled[index]) {
			pthread_mutex_unlock(&test_lock);
			return VK_TIMEOUT;
		}
		pthread_mutex_unlock(&test_lock);
		return VK_SUCCESS;
	}
	if (timeout != UINT64_MAX)
		bad_timeouts++;
	fence_waits++;
	fence_waiting++;
	pthread_cond_broadcast(&test_changed);
	while (!fence_signalled[index])
		pthread_cond_wait(&test_changed, &test_lock);
	fence_waiting--;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: the copy is done. */
	return VK_SUCCESS;
}

/* The fake: nothing to reset. */
VKAPI_ATTR VkResult VKAPI_CALL
vkResetCommandBuffer(
	VkCommandBuffer command,
	VkCommandBufferResetFlags flags)
{
	(void)command;
	(void)flags;
	return VK_SUCCESS;
}

/* The fake: nothing to begin. */
VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(
	VkCommandBuffer command,
	const VkCommandBufferBeginInfo *begin)
{
	(void)command;
	(void)begin;
	return VK_SUCCESS;
}

/* The fake: nothing to end. */
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(
	VkCommandBuffer command)
{
	(void)command;
	return VK_SUCCESS;
}

/* The fake: no barrier. */
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(
	VkCommandBuffer command,
	VkPipelineStageFlags source,
	VkPipelineStageFlags target,
	VkDependencyFlags dependency,
	uint32_t memory_count,
	const VkMemoryBarrier *memory,
	uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffer,
	uint32_t image_count,
	const VkImageMemoryBarrier *image)
{
	(void)command;
	(void)source;
	(void)target;
	(void)dependency;
	(void)memory_count;
	(void)memory;
	(void)buffer_count;
	(void)buffer;
	(void)image_count;
	(void)image;
}

/* The fake: no clearing. */
VKAPI_ATTR void VKAPI_CALL
vkCmdClearColorImage(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout layout,
	const VkClearColorValue *color,
	uint32_t count,
	const VkImageSubresourceRange *ranges)
{
	(void)command;
	(void)image;
	(void)layout;
	(void)color;
	(void)count;
	(void)ranges;
}

/* The fake: no copy. */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImage(
	VkCommandBuffer command,
	VkImage source,
	VkImageLayout source_layout,
	VkImage target,
	VkImageLayout target_layout,
	uint32_t count,
	const VkImageCopy *regions)
{
	(void)command;
	(void)source;
	(void)source_layout;
	(void)target;
	(void)target_layout;
	(void)count;
	(void)regions;
}

/* The fake: no blit. */
VKAPI_ATTR void VKAPI_CALL
vkCmdBlitImage(
	VkCommandBuffer command,
	VkImage source,
	VkImageLayout source_layout,
	VkImage target,
	VkImageLayout target_layout,
	uint32_t count,
	const VkImageBlit *regions,
	VkFilter filter)
{
	(void)command;
	(void)source;
	(void)source_layout;
	(void)target;
	(void)target_layout;
	(void)count;
	(void)regions;
	(void)filter;
}

/* The fake: the copy runs until the test signals its fence. */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit(
	VkQueue queue,
	uint32_t count,
	const VkSubmitInfo *submit,
	VkFence fence)
{
	(void)queue;
	(void)count;
	(void)submit;

	/* The fence the copy will signal. */
	pthread_mutex_lock(&test_lock);

	fence_signalled[fence_index(fence)] = 0U;
	submits++;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);

	/* Succeeded: submitted. */
	return VK_SUCCESS;
}

/* The fake: the place taken, the image let go by the display at once unless the test keeps it, and the result the test set. */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(
	VkQueue queue,
	const VkPresentInfoKHR *present)
{
	const VkDisplayPresentInfoKHR *display;
	VkResult result;

	(void)queue;

	/* The place, and the image let go or kept. */
	display = present->pNext;
	pthread_mutex_lock(&test_lock);

	present_x = display->dstRect.offset.x;
	present_y = display->dstRect.offset.y;
	presents++;
	if (!keep_busy)
		image_busy[present->pImageIndices[0]] = 0U;
	result = present_next;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);

	/* The result the test chose. */
	return result;
}

/* Reads the monotonic clock in microseconds, as the compositor's clock does. */
uint64_t
kwl_microseconds(void)
{
	uint64_t now;

	/* The clock the thread's deadlines are on. */
	now = now_us();

	/* Succeeded. */
	return now;
}

/* Gives a cycle count; the test does not measure cycles. */
uint64_t
kwl_cycles(void)
{
	return 0U;
}

/* Writes the main thread's wake byte. */
void
kwl_thread_wake(
	int descriptor)
{
	unsigned char byte;
	ssize_t written;

	/* One byte; a full pipe already wakes. */
	byte = 1U;
	written = write(descriptor, &byte, 1U);
	(void)written;
}

/* Gives the fake fence of an image (non-zero handles). */
static VkFence
fence_of(
	unsigned index)
{
	return (VkFence)(uintptr_t)(index + 1U);
}

/* Gives the image a fake fence belongs to. */
static unsigned
fence_index(
	VkFence fence)
{
	unsigned index;

	/* The handle less one. */
	index = (unsigned)((uintptr_t)fence - 1U);
	if (index >= TEST_IMAGES) {
		fprintf(stderr, "bad fence\n");
		abort();
	}

	/* Succeeded. */
	return index;
}

/* Reads one of the fakes' counts. */
static unsigned
read_count(
	enum test_count which)
{
	unsigned count;

	/* The count under the fakes' lock. */
	count = 0U;
	pthread_mutex_lock(&test_lock);

	switch (which) {
	case TEST_PRESENTS:
		count = presents;
		break;
	case TEST_SUBMITS:
		count = submits;
		break;
	case TEST_ACQUIRE_CALLS:
		count = acquire_calls;
		break;
	case TEST_ACQUIRE_BLOCKED:
		count = acquire_blocked;
		break;
	case TEST_FENCE_WAITING:
		count = fence_waiting;
		break;
	}

	pthread_mutex_unlock(&test_lock);

	/* The count. */
	return count;
}

/* Waits until a count reaches a target, TEST_WAIT_MS at most; 1 when it did. */
static int
wait_count(
	enum test_count which,
	unsigned target)
{
	uint64_t started;
	unsigned count;

	/* Looks every millisecond (the test's own clock, not the thread's). */
	started = now_us();
	for (;;) {
		count = read_count(which);
		if (count == target)
			return 1;
		if (now_us() - started > (uint64_t)TEST_WAIT_MS * 1000U)
			return 0;
		sleep_ms(1U);
	}
}

/* Signals an image's fence: its copy is done. */
static void
signal_fence(
	unsigned index)
{
	/* The fence, and the waiters woken. */
	pthread_mutex_lock(&test_lock);

	fence_signalled[index] = 1U;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Lets an image go, as the display does when the plane's present ends. */
static void
free_image(
	unsigned index)
{
	/* The image, and the waiters woken. */
	pthread_mutex_lock(&test_lock);

	image_busy[index] = 0U;
	pthread_cond_broadcast(&test_changed);

	pthread_mutex_unlock(&test_lock);
}

/* Sets whether the display keeps the presented images (1) or lets them go at once (0). */
static void
set_keep_busy(
	unsigned keep)
{
	/* The mode. */
	pthread_mutex_lock(&test_lock);

	keep_busy = keep;

	pthread_mutex_unlock(&test_lock);
}

/* Makes the box as the compositor does: empty, the pointer at the origin, its signals on the monotonic clock. */
static void
box_init(
	struct kwl_cursor_box *box)
{
	pthread_condattr_t attributes;

	/* The box's lock and signals. */
	memset(box, 0, sizeof(*box));
	(void)pthread_mutex_init(&box->lock, NULL);
	(void)pthread_condattr_init(&attributes);
	(void)pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	(void)pthread_cond_init(&box->changed, &attributes);
	(void)pthread_cond_init(&box->answered, &attributes);
	(void)pthread_condattr_destroy(&attributes);

	/* The two copies the thread may pin. */
	box->copies[0] = &test_copies[0];
	box->copies[1] = &test_copies[1];
	box->output = KWL_PLANE_ANCHOR;
}

/* Shows an image at a place, as the main thread publishes it. */
static void
box_show(
	struct kwl_cursor_box *box,
	const struct kwl_import *image,
	uint32_t version,
	int32_t x,
	int32_t y)
{
	/* The image, its place, and the change. */
	pthread_mutex_lock(&box->lock);

	box->image = image;
	box->image_generation = 1U;
	box->image_version = version;
	box->show = 1U;
	box->output = KWL_PLANE_ANCHOR;
	box->x = x;
	box->y = y;
	box->sequence++;
	pthread_cond_broadcast(&box->changed);

	pthread_mutex_unlock(&box->lock);
}

/* Moves the pointer, as the input thread writes the box. */
static void
box_move(
	struct kwl_cursor_box *box,
	int32_t x,
	int32_t y)
{
	/* The place, and the change. */
	pthread_mutex_lock(&box->lock);

	box->x = x;
	box->y = y;
	box->sequence++;
	pthread_cond_broadcast(&box->changed);

	pthread_mutex_unlock(&box->lock);
}

/* Reads how many of the thread's copies read one of the box's copies. */
static unsigned
box_pins(
	struct kwl_cursor_box *box,
	unsigned copy)
{
	unsigned pins;

	/* The count under the box's lock. */
	pthread_mutex_lock(&box->lock);

	pins = box->pins[copy];

	pthread_mutex_unlock(&box->lock);

	/* The count. */
	return pins;
}

/* Makes the thread's record as the plane's open does, with the fake plane's objects. */
static void
thread_init(
	struct kwl_cursor_thread *cursor,
	struct kwl_cursor_box *box,
	int wake)
{
	unsigned index;

	/* The plane: its images, each with its fence; the box's copies; the hold. */
	memset(cursor, 0, sizeof(*cursor));
	cursor->box = box;
	cursor->queue_lock = &queue_lock;
	cursor->output.images = test_images;
	cursor->output.image_count = TEST_IMAGES;
	cursor->output.format = VK_FORMAT_B8G8R8A8_UNORM;
	cursor->output.width = 64U;
	cursor->output.height = 64U;
	for (index = 0; index < TEST_IMAGES; index++)
		cursor->fences[index] = fence_of(index);
	cursor->copies[0] = &test_copies[0];
	cursor->copies[1] = &test_copies[1];
	cursor->hold_us = TEST_HOLD_US;
	cursor->wake_main = wake;
	test_copies[0].width = 32U;
	test_copies[0].height = 32U;
	test_copies[1].width = 32U;
	test_copies[1].height = 32U;
}

/* Reads the main thread's wake bytes; gives how many there were. */
static unsigned
wakes_read(
	int descriptor)
{
	unsigned char bytes[16];
	ssize_t got;
	unsigned count;

	/* Every byte there now. */
	count = 0U;
	for (;;) {
		got = read(descriptor, bytes, sizeof(bytes));
		if (got <= 0)
			break;
		count += (unsigned)got;
	}

	/* The bytes read. */
	return count;
}

/* Frees image 0 after 200 ms, as the display lets the plane's previous present go. */
static void *
free_later(
	void *argument)
{
	(void)argument;

	/* The wait, then the image. */
	sleep_ms(200U);
	free_image(0U);
	return NULL;
}

/* Counts a failed check and prints it. */
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

/* Sleeps the test's thread. */
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

/* Reads the monotonic clock in microseconds. */
static uint64_t
now_us(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}
