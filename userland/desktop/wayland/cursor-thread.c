/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The cursor thread (ws213-p004): the cursor plane presented from the
 * cursor's box, once a refresh at most, apart from the main thread
 * (cursor-thread.h).
 *
 * A move of the image shown is presented once a refresh, at the latest
 * place: the pointer reports more often than the display shows (ws213-p001
 * C0).  The first move after a still refresh goes at once; a later one
 * waits until a little before the refresh is over (hold_us), so that no
 * refresh goes without a move.  A new image is not held.  An image is
 * copied into a swapchain image only when that swapchain image does not
 * hold it yet; only that swapchain image's previous copy is waited for, so
 * a copy into one image never holds back a present from another
 * (ws141-p016 review-B4 F1 (b)).
 */

#include "cursor-thread.h"
#include "threads.h"

#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* What one present attempt came to. */
enum cursor_present_result {
	/* Presented. */
	CURSOR_PRESENTED = 1,
	/* No swapchain image was free: tried again soon. */
	CURSOR_RETRY,
	/* A Vulkan call failed: the thread stops using Vulkan. */
	CURSOR_FAILED
};

/* How soon a step is tried again when no swapchain image was free, and while a copy still pins one of the box's copies, in microseconds. */
#define CURSOR_RETRY_US		1000U
#define CURSOR_PIN_POLL_US	5000U

/*
 * What a step read from the box: the place and output, the report's time,
 * the image, its generation, version and hotspot, whether the plane shows
 * the cursor, and the copy the step pinned (-1 none) while it reads it.
 */
struct cursor_snapshot {
	int32_t x;
	int32_t y;
	unsigned output;
	uint64_t event_us;
	const struct kwl_import *image;
	uint64_t generation;
	uint32_t version;
	int32_t hotspot_x;
	int32_t hotspot_y;
	unsigned show;
	int pin;
};

static void *cursor_main(void *argument);
static void cursor_wait(struct kwl_cursor_thread *cursor, uint64_t deadline_us);
static enum cursor_present_result cursor_present(struct kwl_cursor_thread *cursor, const struct cursor_snapshot *snapshot, const struct kwl_import *source, int32_t x, int32_t y);
static void cursor_record(struct kwl_cursor_thread *cursor, VkCommandBuffer command, VkImage target, const struct kwl_import *source);
static unsigned cursor_unpin_done(struct kwl_cursor_thread *cursor);
static void cursor_unpin(struct kwl_cursor_thread *cursor, int copy);
static void cursor_fail(struct kwl_cursor_thread *cursor, const char *step, VkResult result);

/*
 * Starts the cursor thread once the main thread has made the plane's
 * objects.
 */
int
kwl_cursor_thread_start(
	struct kwl_cursor_thread *cursor)
{
	sigset_t signals_all;
	sigset_t signals_before;
	unsigned index;
	int error;

	/* No swapchain image holds a copy of the box's copies yet. */
	for (index = 0; index < KWL_CURSOR_THREAD_IMAGES; index++)
		cursor->pinned[index] = -1;

	/* The thread presents from now on. */
	/* The thread blocks every signal: the stops and a child's end go to the event loop's thread. */
	(void)sigfillset(&signals_all);
	(void)pthread_sigmask(SIG_SETMASK, &signals_all, &signals_before);
	error = pthread_create(&cursor->thread, NULL, cursor_main, cursor);
	(void)pthread_sigmask(SIG_SETMASK, &signals_before, NULL);
	if (error != 0)
		return error;

	/* Succeeded: stopped by kwl_cursor_thread_stop. */
	cursor->started = 1U;
	return 0;
}

/*
 * Stops the cursor thread and waits for it: it waits for its own copies
 * first (100 ms each at most).  Returns ETIMEDOUT when a copy did not
 * finish, so that the main thread does not destroy objects the GPU may
 * still use; 0 otherwise.
 */
int
kwl_cursor_thread_stop(
	struct kwl_cursor_thread *cursor)
{
	/* A thread never started has nothing in flight. */
	if (!cursor->started)
		return 0;

	/* The request, which wakes the thread. */
	pthread_mutex_lock(&cursor->box->lock);

	cursor->box->stop = 1U;
	cursor->box->sequence++;
	pthread_cond_broadcast(&cursor->box->changed);

	pthread_mutex_unlock(&cursor->box->lock);

	/* The thread ends after its copies. */
	(void)pthread_join(cursor->thread, NULL);
	cursor->started = 0U;

	/* A copy that did not finish keeps the plane's objects alive. */
	if (cursor->stop_failed)
		return ETIMEDOUT;

	/* Succeeded: no Vulkan object of the plane is in use. */
	return 0;
}

/*
 * Runs one step of the cursor thread: reads the box, and presents the
 * plane when its image or place changed and the refresh allows.  Gives the
 * time to try again in deadline_us when the step waits for the refresh, a
 * free swapchain image or a copy's fence (0: wait for a change only).  The
 * thread calls it in a loop; a host test calls it step by step.
 */
enum kwl_cursor_step
kwl_cursor_thread_step(
	struct kwl_cursor_thread *cursor,
	uint64_t *deadline_us)
{
	struct cursor_snapshot snapshot;
	const struct kwl_import *source;
	enum cursor_present_result result;
	uint64_t now;
	unsigned index;
	unsigned pins;
	int32_t x;
	int32_t y;

	/* What the box says now, and what the main thread asks. */
	*deadline_us = 0U;
	pthread_mutex_lock(&cursor->box->lock);

	cursor->sequence = cursor->box->sequence;
	if (cursor->box->stop) {
		pthread_mutex_unlock(&cursor->box->lock);
		return KWL_CURSOR_STEP_STOP;
	}

	/* A park is answered once, and no Vulkan is called until it ends. */
	if (cursor->box->park) {
		if (!cursor->box->parked) {
			cursor->box->parked = 1U;
			pthread_cond_broadcast(&cursor->box->answered);
		}

		/* Parked until the main thread ends the park. */
		pthread_mutex_unlock(&cursor->box->lock);
		return KWL_CURSOR_STEP_PARKED;
	}

	/* The park is over; a failed thread does nothing more. */
	cursor->box->parked = 0U;
	if (cursor->failed) {
		pthread_mutex_unlock(&cursor->box->lock);
		return KWL_CURSOR_STEP_FAILED;
	}

	/* The place, the image and its hotspot, and whether the plane shows the cursor. */
	snapshot.x = cursor->box->x;
	snapshot.y = cursor->box->y;
	snapshot.output = cursor->box->output;
	snapshot.event_us = cursor->box->event_us;
	snapshot.image = cursor->box->image;
	snapshot.generation = cursor->box->image_generation;
	snapshot.version = cursor->box->image_version;
	snapshot.hotspot_x = cursor->box->hotspot_x;
	snapshot.hotspot_y = cursor->box->hotspot_y;
	snapshot.show = cursor->box->show;

	/*
	 * A copy of a client's cursor is pinned while this step may read it,
	 * so the main thread does not write it meanwhile (design 4.7 3).
	 */
	snapshot.pin = -1;
	for (index = 0; index < KWL_CURSOR_COPIES; index++) {
		if (snapshot.image != NULL && snapshot.image == cursor->copies[index]) {
			cursor->box->pins[index]++;
			snapshot.pin = (int)index;
		}
	}

	pthread_mutex_unlock(&cursor->box->lock);

	/* Copies seen done let go of the box's copies they read. */
	pins = cursor_unpin_done(cursor);

	/*
	 * The plane shows the image with its hotspot at the place, on the
	 * anchor only; otherwise it is empty where it was (the cursor is
	 * composed: a drag, an image the plane cannot show, a pointer on
	 * another display).
	 */
	source = NULL;
	x = cursor->x;
	y = cursor->y;
	if (snapshot.show && snapshot.output == KWL_PLANE_ANCHOR && snapshot.image != NULL) {
		source = snapshot.image;
		x = snapshot.x - snapshot.hotspot_x;
		y = snapshot.y - snapshot.hotspot_y;
	}

	/* Nothing changed since the last present. */
	if (cursor->presented &&
	    cursor->source == source &&
	    (source == NULL ||
	     (cursor->generation == snapshot.generation &&
	      cursor->version == snapshot.version &&
	      cursor->x == x &&
	      cursor->y == y))) {
		cursor_unpin(cursor, snapshot.pin);
		if (pins != 0U)
			*deadline_us = kwl_microseconds() + CURSOR_PIN_POLL_US;
		return KWL_CURSOR_STEP_IDLE;
	}

	/* A move of the image shown waits for the refresh's end (C0). */
	if (cursor->presented &&
	    cursor->source == source &&
	    source != NULL &&
	    cursor->generation == snapshot.generation &&
	    cursor->version == snapshot.version) {
		now = kwl_microseconds();
		if (now - cursor->presented_us < cursor->hold_us) {
			cursor_unpin(cursor, snapshot.pin);
			*deadline_us = cursor->presented_us + cursor->hold_us;
			pthread_mutex_lock(&cursor->box->lock);

			cursor->box->stats.held++;

			pthread_mutex_unlock(&cursor->box->lock);
			return KWL_CURSOR_STEP_IDLE;
		}
	}

	/* The image at its place. */
	result = cursor_present(cursor, &snapshot, source, x, y);
	if (result == CURSOR_RETRY) {
		*deadline_us = kwl_microseconds() + CURSOR_RETRY_US;
		return KWL_CURSOR_STEP_IDLE;
	}

	/* A failure was told to the main thread. */
	if (result == CURSOR_FAILED)
		return KWL_CURSOR_STEP_FAILED;

	/* Succeeded: the plane shows the latest. */
	return KWL_CURSOR_STEP_PRESENTED;
}

/*
 * Waits for the cursor thread's copies when it stops: each swapchain
 * image's fence for 100 ms at most.  A fence that does not signal sets
 * stop_failed (the objects must not be destroyed under the GPU).
 */
void
kwl_cursor_thread_finish(
	struct kwl_cursor_thread *cursor)
{
	VkResult result;
	unsigned index;

	/* A thread that failed calls no Vulkan; its fences are waited for by the main thread's close. */
	if (cursor->failed)
		return;

	/* Each image's last copy. */
	for (index = 0; index < cursor->output.image_count && index < KWL_CURSOR_THREAD_IMAGES; index++) {
		if (cursor->fences[index] == VK_NULL_HANDLE)
			continue;
		result = vkWaitForFences(cursor->device, 1U, &cursor->fences[index], VK_TRUE, KWL_CURSOR_FENCE_WAIT_NS);
		if (result != VK_SUCCESS)
			cursor->stop_failed = 1U;
	}

	/* The pins the copies held are let go. */
	for (index = 0; index < KWL_CURSOR_THREAD_IMAGES; index++) {
		cursor_unpin(cursor, cursor->pinned[index]);
		cursor->pinned[index] = -1;
	}
}

/*
 * Parks the cursor thread (park 1: it answers, then calls no Vulkan until
 * the park ends) or ends the park (park 0).  A park waits at most wait_ms
 * for the answer and returns ETIMEDOUT when none came (the thread is in a
 * Vulkan call that does not end); 0 otherwise.
 */
int
kwl_cursor_thread_park(
	struct kwl_cursor_box *box,
	unsigned park,
	unsigned wait_ms)
{
	struct timespec deadline;
	int error;

	/* The deadline on the monotonic clock the answers use. */
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += (time_t)(wait_ms / 1000U);
	deadline.tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	/* The request, and for a park its answer. */
	error = 0;
	pthread_mutex_lock(&box->lock);

	box->park = park;
	box->sequence++;
	pthread_cond_broadcast(&box->changed);
	while (park && !box->parked && error == 0)
		error = pthread_cond_timedwait(&box->answered, &box->lock, &deadline);
	if (!park || box->parked)
		error = 0;

	pthread_mutex_unlock(&box->lock);

	/* The thread did not answer in time. */
	if (error != 0)
		return ETIMEDOUT;

	/* Succeeded: parked, or free to present again. */
	return 0;
}

/* Runs the cursor thread's steps, waiting between them for a change or a deadline, until it is asked to stop. */
static void *
cursor_main(
	void *argument)
{
	struct kwl_cursor_thread *cursor;
	enum kwl_cursor_step step;
	uint64_t deadline;

	/* Step after step; a present is followed by another look at once. */
	cursor = argument;
	for (;;) {
		step = kwl_cursor_thread_step(cursor, &deadline);
		if (step == KWL_CURSOR_STEP_STOP)
			break;
		if (step == KWL_CURSOR_STEP_PRESENTED)
			continue;

		/* Nothing to do now: the next change of the box, or the deadline. */
		cursor_wait(cursor, deadline);
	}

	/* The copies in flight finish before the main thread destroys the objects. */
	kwl_cursor_thread_finish(cursor);
	return NULL;
}

/* Waits for the box to change after the last step, or for a deadline (kwl_microseconds' clock; 0 for none). */
static void
cursor_wait(
	struct kwl_cursor_thread *cursor,
	uint64_t deadline_us)
{
	struct timespec deadline;
	int error;

	/* The deadline on the monotonic clock, which kwl_microseconds reads too. */
	deadline.tv_sec = (time_t)(deadline_us / 1000000U);
	deadline.tv_nsec = (long)(deadline_us % 1000000U) * 1000L;

	/* Until the box changes, or the time comes. */
	error = 0;
	pthread_mutex_lock(&cursor->box->lock);

	while (cursor->box->sequence == cursor->sequence && error == 0) {
		if (deadline_us == 0U) {
			error = pthread_cond_wait(&cursor->box->changed, &cursor->box->lock);
		} else {
			error = pthread_cond_timedwait(&cursor->box->changed, &cursor->box->lock, &deadline);
		}
	}

	pthread_mutex_unlock(&cursor->box->lock);
}

/*
 * Presents the plane's image at a place: copies the cursor image into a
 * free swapchain image first unless that image holds it already.  A
 * failure is told to the main thread, and no Vulkan is called again.
 */
static enum cursor_present_result
cursor_present(
	struct kwl_cursor_thread *cursor,
	const struct cursor_snapshot *snapshot,
	const struct kwl_import *source,
	int32_t x,
	int32_t y)
{
	struct kwl_cursor_stats stats;
	VkDisplayPresentInfoKHR display;
	VkPresentInfoKHR present;
	VkSubmitInfo submit;
	VkPipelineStageFlags stage;
	VkSemaphore acquired;
	VkSemaphore wait;
	uint32_t image;
	uint32_t version;
	uint64_t generation;
	uint64_t mark;
	uint64_t spent;
	uint64_t started_us;
	uint64_t ended_us;
	VkResult result;
	int keep_pin;

	/* A free image now, or after a short wait. */
	memset(&stats, 0, sizeof(stats));
	started_us = kwl_microseconds();
	mark = kwl_cycles();
	acquired = cursor->acquired[cursor->next];
	result = vkAcquireNextImageKHR(cursor->device, cursor->output.swapchain, 0U, acquired, VK_NULL_HANDLE, &image);
	if (result == VK_NOT_READY || result == VK_TIMEOUT) {
		cursor_unpin(cursor, snapshot->pin);
		return CURSOR_RETRY;
	}

	/* Any other failure stops the thread's Vulkan. */
	if ((result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) || image >= cursor->output.image_count) {
		cursor_unpin(cursor, snapshot->pin);
		cursor_fail(cursor, "acquire", result);
		return CURSOR_FAILED;
	}

	/* The next acquisition's semaphore, and the identity of what the image will hold. */
	cursor->next = (cursor->next + 1U) % (cursor->output.image_count + 1U);
	version = 0U;
	generation = 0U;
	if (source != NULL) {
		version = snapshot->version;
		generation = snapshot->generation;
	}

	/* The acquire's part. */
	stats.acquire_cycles = kwl_cycles() - mark;

	/* The cursor image goes into the swapchain image once; later moves present it as it is. */
	mark = kwl_cycles();
	wait = acquired;
	keep_pin = 0;
	if (!cursor->held_valid[image] ||
	    cursor->held[image] != source ||
	    cursor->held_generation[image] != generation ||
	    cursor->held_version[image] != version) {
		/* This swapchain image's previous copy only; a copy into another image does not hold this one back. */
		result = vkWaitForFences(cursor->device, 1U, &cursor->fences[image], VK_TRUE, KWL_CURSOR_FENCE_WAIT_NS);
		stats.fence_cycles = kwl_cycles() - mark;
		if (result == VK_SUCCESS) {
			cursor_unpin(cursor, cursor->pinned[image]);
			cursor->pinned[image] = -1;
			result = vkResetFences(cursor->device, 1U, &cursor->fences[image]);
		}

		/* The copy recorded afresh. */
		if (result == VK_SUCCESS)
			result = vkResetCommandBuffer(cursor->commands[image], 0U);
		if (result == VK_SUCCESS) {
			cursor_record(cursor, cursor->commands[image], cursor->output.images[image], source);
			result = vkEndCommandBuffer(cursor->commands[image]);
		}

		/* Submitted after the acquisition, signalling the present's wait and the image's fence. */
		if (result == VK_SUCCESS) {
			stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			memset(&submit, 0, sizeof(submit));
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit.waitSemaphoreCount = 1U;
			submit.pWaitSemaphores = &acquired;
			submit.pWaitDstStageMask = &stage;
			submit.commandBufferCount = 1U;
			submit.pCommandBuffers = &cursor->commands[image];
			submit.signalSemaphoreCount = 1U;
			submit.pSignalSemaphores = &cursor->ready[image];
			pthread_mutex_lock(cursor->queue_lock);
			result = vkQueueSubmit(cursor->queue, 1U, &submit, cursor->fences[image]);
			pthread_mutex_unlock(cursor->queue_lock);
		}

		/* A failed copy stops the thread's Vulkan. */
		if (result != VK_SUCCESS) {
			cursor_unpin(cursor, snapshot->pin);
			cursor_fail(cursor, "copy", result);
			return CURSOR_FAILED;
		}

		/*
		 * The swapchain image holds this cursor image now; a copy of the
		 * box's copies stays pinned until its fence is seen done.
		 */
		cursor->held_valid[image] = 1U;
		cursor->held[image] = source;
		cursor->held_generation[image] = generation;
		cursor->held_version[image] = version;
		if (source != NULL && snapshot->pin >= 0) {
			cursor->pinned[image] = snapshot->pin;
			keep_pin = 1;
		}

		/* The present waits for the copy. */
		stats.copies = 1U;
		wait = cursor->ready[image];
	}

	/* A step that copied nothing from the box's copy does not keep it pinned. */
	if (!keep_pin)
		cursor_unpin(cursor, snapshot->pin);
	stats.copy_cycles = kwl_cycles() - mark - stats.fence_cycles;

	/* The whole image with its top-left corner at the place. */
	memset(&display, 0, sizeof(display));
	display.sType = VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR;
	display.srcRect.extent.width = cursor->output.width;
	display.srcRect.extent.height = cursor->output.height;
	display.dstRect.offset.x = x;
	display.dstRect.offset.y = y;
	display.dstRect.extent = display.srcRect.extent;
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.pNext = &display;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &wait;
	present.swapchainCount = 1U;
	present.pSwapchains = &cursor->output.swapchain;
	present.pImageIndices = &image;
	mark = kwl_cycles();
	pthread_mutex_lock(cursor->queue_lock);
	result = vkQueuePresentKHR(cursor->queue, &present);
	pthread_mutex_unlock(cursor->queue_lock);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		cursor_fail(cursor, "present", result);
		return CURSOR_FAILED;
	}

	/* The time in the present, its waits for the queue's and the device's locks included (ws213-p001 review-1 H3). */
	spent = kwl_cycles() - mark;
	ended_us = kwl_microseconds();
	stats.present_cycles = spent;
	stats.present_max_cycles = spent;
	stats.presents = 1U;
	stats.present_max_us = ended_us - started_us;
	if (cursor->presented && started_us - cursor->presented_us < 50000U)
		stats.present_gap_max_us = started_us - cursor->presented_us;

	/*
	 * What the plane shows now, and when.  The time is the present's
	 * start, not its end: counting the hold from its end spaced the
	 * presents by a refresh and that (ws211-p006).
	 */
	cursor->presented = 1U;
	cursor->presented_us = started_us;
	cursor->source = source;
	cursor->generation = generation;
	cursor->version = version;
	cursor->x = x;
	cursor->y = y;

	/* The counts go to the box for the main thread's KWL PERF lines. */
	pthread_mutex_lock(&cursor->box->lock);

	cursor->box->stats.presents += stats.presents;
	cursor->box->stats.copies += stats.copies;
	cursor->box->stats.fence_cycles += stats.fence_cycles;
	cursor->box->stats.acquire_cycles += stats.acquire_cycles;
	cursor->box->stats.copy_cycles += stats.copy_cycles;
	cursor->box->stats.present_cycles += stats.present_cycles;
	if (stats.present_max_cycles > cursor->box->stats.present_max_cycles)
		cursor->box->stats.present_max_cycles = stats.present_max_cycles;
	if (stats.present_max_us > cursor->box->stats.present_max_us)
		cursor->box->stats.present_max_us = stats.present_max_us;
	if (stats.present_gap_max_us > cursor->box->stats.present_gap_max_us)
		cursor->box->stats.present_gap_max_us = stats.present_gap_max_us;

	pthread_mutex_unlock(&cursor->box->lock);

	/* The frame log ties the present to the report it shows (no lock is held). */
	if (cursor->log) {
		printf("KWL CURSOR_PLANE present x=%d y=%d copy=%u event_us=%llu present_cycles=%llu at_us=%llu\n",
		       x,
		       y,
		       stats.copies,
		       (unsigned long long)snapshot->event_us,
		       (unsigned long long)spent,
		       (unsigned long long)ended_us);
	}

	/* Succeeded: presented. */
	return CURSOR_PRESENTED;
}

/* Records the swapchain image's clearing and the cursor image's copy into its top-left corner. */
static void
cursor_record(
	struct kwl_cursor_thread *cursor,
	VkCommandBuffer command,
	VkImage target,
	const struct kwl_import *source)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkClearColorValue clear;
	VkImageCopy copy;
	VkImageBlit blit;

	/* One use of the command buffer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(command, &begin);

	/* The swapchain image, its old contents dropped, takes the copy. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = 0U;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = target;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Transparent where the cursor image does not reach. */
	memset(&clear, 0, sizeof(clear));
	vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1U, &barrier.subresourceRange);

	/* The cursor image's host-written staging (B, G, R, A, premultiplied) into the corner. */
	if (source != NULL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		if (cursor->output.format == VK_FORMAT_B8G8R8A8_UNORM) {
			memset(&copy, 0, sizeof(copy));
			copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.srcSubresource.layerCount = 1U;
			copy.dstSubresource = copy.srcSubresource;
			copy.extent.width = source->width;
			copy.extent.height = source->height;
			copy.extent.depth = 1U;
			vkCmdCopyImage(command, source->staging, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
		} else {
			/* Another channel order is converted by a one-to-one blit. */
			memset(&blit, 0, sizeof(blit));
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.layerCount = 1U;
			blit.dstSubresource = blit.srcSubresource;
			blit.srcOffsets[1].x = (int32_t)source->width;
			blit.srcOffsets[1].y = (int32_t)source->height;
			blit.srcOffsets[1].z = 1;
			blit.dstOffsets[1] = blit.srcOffsets[1];
			vkCmdBlitImage(command, source->staging, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit, VK_FILTER_NEAREST);
		}
	}

	/* Then to the layout it is presented in. */
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}

/*
 * Lets go of the box's copies whose copies into a swapchain image are seen
 * done (their fence asked, not waited for).  Reports how many pins are
 * still held.
 */
static unsigned
cursor_unpin_done(
	struct kwl_cursor_thread *cursor)
{
	VkResult result;
	unsigned held;
	unsigned index;

	/* Each swapchain image whose last copy read one of the box's copies. */
	held = 0U;
	for (index = 0; index < KWL_CURSOR_THREAD_IMAGES; index++) {
		if (cursor->pinned[index] < 0)
			continue;

		/* Done: the pin goes; still running: it stays. */
		result = vkGetFenceStatus(cursor->device, cursor->fences[index]);
		if (result == VK_SUCCESS) {
			cursor_unpin(cursor, cursor->pinned[index]);
			cursor->pinned[index] = -1;
			continue;
		}

		/* Still read by the GPU. */
		held++;
	}

	/* The pins still held. */
	return held;
}

/* Lets go of one pin of the box's copies (-1: none). */
static void
cursor_unpin(
	struct kwl_cursor_thread *cursor,
	int copy)
{
	unsigned wake;

	/* No pin. */
	if (copy < 0 || copy >= (int)KWL_CURSOR_COPIES)
		return;

	/*
	 * One reader fewer; at zero the main thread may write the copy again,
	 * and is woken for it when it found both copies in use.
	 */
	pthread_mutex_lock(&cursor->box->lock);

	cursor->box->pins[copy]--;
	wake = 0U;
	if (cursor->box->pins[copy] == 0U && cursor->box->copy_wanted) {
		cursor->box->copy_wanted = 0U;
		wake = 1U;
	}

	pthread_mutex_unlock(&cursor->box->lock);

	/* The main thread's poll returns, with no lock held. */
	if (wake)
		kwl_thread_wake(cursor->wake_main);
}

/*
 * Stops the thread's use of Vulkan after a failed call, and tells the main
 * thread, which composes the cursor and closes the plane.
 */
static void
cursor_fail(
	struct kwl_cursor_thread *cursor,
	const char *step,
	VkResult result)
{
	unsigned char byte;
	ssize_t written;

	/* No Vulkan again; the line goes out with no lock held. */
	cursor->failed = 1U;
	printf("KWL CURSOR_PLANE failed step=%s result=%d\n", step, (int)result);

	/* The box says so. */
	pthread_mutex_lock(&cursor->box->lock);

	cursor->box->failed = 1U;
	pthread_cond_broadcast(&cursor->box->answered);

	pthread_mutex_unlock(&cursor->box->lock);

	/* The main thread's poll returns. */
	byte = 1U;
	if (cursor->wake_main < 0)
		return;
	do {
		written = write(cursor->wake_main, &byte, 1U);
	} while (written < 0 && errno == EINTR);
}
