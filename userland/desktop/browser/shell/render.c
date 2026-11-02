/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing thread of the browser's window ("browser-render", a thread
 * of libkeiland's): the view runs its pages on threads of its own and
 * commits a frame whenever the page changed (its frame_wake wakes this
 * thread); this thread draws the latest committed frame at the size the
 * window gives it, so that neither a long script nor a layout keeps the
 * window from being drawn, and the window's thread only takes the window's
 * events.
 *
 * With the GPU it owns the presenter (present.c) from the start to the
 * stop: it makes the swapchain fit a new size, draws, presents and tells
 * the view of each present (browser_view_presented, which runs the page's
 * animation frames).  A new size of a Wayland window is acknowledged just
 * before the first frame of that size is presented: the window's thread
 * takes the configure (kl_window_configure_take) and hands it here; this
 * thread asks the window's thread to acknowledge it (SHELL_POST_ACK, a
 * kl_window_post), waits for the answer, and only then presents, so that
 * no frame of the old size reaches the compositor after the
 * acknowledgement.  With the CPU it draws into frames the window's thread
 * shows (libkeiland's shared memory is the window's thread's).
 *
 * The window's thread stops it before the presenter, the window and the
 * view go, and goes on taking the window's events while it waits for the
 * stop (a present may need the window's thread on some systems).
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The name of the drawing thread. */
#define RENDER_THREAD_NAME	"browser-render"

/* How long a wait for the window's thread lasts before the stop is looked at again, in milliseconds. */
#define RENDER_WAIT_MS		100U

/* How long the window's thread waits for the drawing thread's end between takings of the window's events, in milliseconds. */
#define RENDER_STOP_MS		10U

/* The states of an acknowledgement asked of the window's thread. */
enum render_ack {
	RENDER_ACK_NONE,
	RENDER_ACK_ASKED,
	RENDER_ACK_DONE
};

/*
 * What one round of the drawing thread does, taken from the shared state
 * under the lock: whether to end, the size to draw at, the configure to
 * acknowledge before it (configure, serial), and whether anything was
 * committed yet.
 */
struct render_round {
	int stop;
	uint32_t width;
	uint32_t height;
	int configure;
	uint32_t serial;
	int has_commit;
};

static void render_run(void *argument);
static void render_wait(struct shell_render *render, int retry, struct render_round *round);
static int render_gpu(struct shell_render *render, const struct render_round *round, int *retry);
static int render_cpu(struct shell_render *render, const struct render_round *round);
static int render_ask_ack(struct shell_render *render, uint32_t serial);
static void render_requeue(struct shell_render *render, uint32_t serial);
static void render_tell(struct shell_render *render);
static int render_frame_fit(struct shell_cpu_frame *frame, uint32_t width, uint32_t height);
static void render_take_overlay(struct shell_render *render);
static int render_overlay_fit(struct shell_overlay *overlay, uint32_t width, uint32_t height);
static void render_overlay_blit(const struct shell_overlay *overlay, struct shell_cpu_frame *frame);

/*
 * Makes the drawing thread's lock and condition (before the view, whose
 * frame_wake uses them).  Returns 0, or ENOMEM.
 */
int
shell_render_init(
	struct shell_render *render)
{
	int error;

	/* Nothing is held yet; the CPU's frames are each in their place. */
	memset(render, 0, sizeof(*render));
	render->back = 0U;
	render->ready = 1U;
	render->front = 2U;

	/* The lock. */
	error = kl_mutex_init(&render->lock);
	if (error != 0)
		return ENOMEM;

	/* The condition its waits are told of. */
	error = kl_cond_init(&render->changed);
	if (error != 0) {
		kl_mutex_destroy(&render->lock);
		return ENOMEM;
	}

	/* Succeeded: the view may wake it. */
	render->made = 1;
	return 0;
}

/*
 * The view's frame_wake, on the view's thread: the page committed a new
 * frame, which the drawing thread draws.  It does not call into the view.
 */
void
shell_render_frame_wake(
	void *context)
{
	struct shell_render *render;

	/* The drawing thread's state. */
	render = context;
	kl_mutex_lock(&render->lock);

	/* A frame to draw (there is one from now on). */
	render->committed = 1;
	render->has_commit = 1;
	kl_cond_broadcast(&render->changed);

	kl_mutex_unlock(&render->lock);
}

/*
 * Starts the drawing thread for a view in a window: with the presenter's
 * GPU (present), or with the CPU (present NULL), at the size given last
 * (shell_render_resize).  wake (with wake_context) wakes the window's
 * thread when a frame was drawn or the drawing failed.  Returns 0, or the
 * error of the thread's start.
 */
int
shell_render_start(
	struct shell_render *render,
	struct browser_view *view,
	struct kl_window *kui,
	struct shell_present *present,
	void (*wake)(void *context),
	void *wake_context)
{
	struct kl_thread_options options;
	int error;

	/* What the thread draws with, fixed from now on. */
	render->view = view;
	render->kui = kui;
	render->present = present;
	render->wake = wake;
	render->wake_context = wake_context;

	/* The thread. */
	memset(&options, 0, sizeof(options));
	options.name = RENDER_THREAD_NAME;
	options.run = render_run;
	options.argument = render;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	error = kl_thread_start(&options, &render->thread);
	if (error != 0) {
		render->thread = NULL;
		return error;
	}

	/* Succeeded: the window is drawn from now on. */
	return 0;
}

/*
 * Gives the drawing thread the window's size, and the configure that
 * waits to be acknowledged before the first frame of it (configure
 * nonzero, its serial), on the window's thread.  A later size replaces
 * one not drawn yet.
 */
void
shell_render_resize(
	struct shell_render *render,
	uint32_t width,
	uint32_t height,
	int configure,
	uint32_t serial)
{
	/* The size, and the drawing thread told. */
	kl_mutex_lock(&render->lock);

	render->width = width;
	render->height = height;
	render->sized = 1;
	if (configure) {
		render->configure = 1;
		render->configure_serial = serial;
	}
	kl_cond_broadcast(&render->changed);

	kl_mutex_unlock(&render->lock);
}

/*
 * Carries out the acknowledgement the drawing thread asked for
 * (SHELL_POST_ACK), on the window's thread, and gives it the answer.
 */
void
shell_render_ack(
	struct shell_render *render)
{
	uint32_t serial;
	int asked;
	int error;

	/* The serial asked for, if an acknowledgement is waited for. */
	kl_mutex_lock(&render->lock);

	asked = 0;
	serial = 0U;
	if (render->ack_state == RENDER_ACK_ASKED) {
		asked = 1;
		serial = render->ack_serial;
	}

	kl_mutex_unlock(&render->lock);
	if (!asked)
		return;

	/* The acknowledgement (EINVAL when a newer configure was taken since; the drawing thread then draws for that one). */
	error = kl_window_configure_ack(render->kui, serial);

	/* The answer, to the drawing thread. */
	kl_mutex_lock(&render->lock);

	if (render->ack_state == RENDER_ACK_ASKED && render->ack_serial == serial) {
		render->ack_error = error;
		render->ack_state = RENDER_ACK_DONE;
		kl_cond_broadcast(&render->changed);
	}

	kl_mutex_unlock(&render->lock);
}

/*
 * Gives the drawing thread a picture to copy over the middle of every
 * frame from now on (pixels NULL or a size 0: none), on the window's
 * thread; it is copied, and the window is drawn again with it.  Memory
 * that runs out leaves the picture as it was (a line says so).
 */
void
shell_render_overlay(
	struct shell_render *render,
	const uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	int error;

	/* None is a size 0. */
	if (pixels == NULL || width == 0U || height == 0U) {
		width = 0U;
		height = 0U;
	}

	/* The picture, copied under the lock, and the drawing thread told. */
	kl_mutex_lock(&render->lock);

	error = render_overlay_fit(&render->overlay, width, height);
	if (error == 0) {
		if (width != 0U)
			memcpy(render->overlay.pixels, pixels, (size_t)width * (size_t)height * sizeof(uint32_t));
		render->overlay_changed = 1;
		kl_cond_broadcast(&render->changed);
	}

	kl_mutex_unlock(&render->lock);

	/* Memory that ran out. */
	if (error != 0) {
		printf("ZBROWSER ERROR overlay error=%d\n", error);
		fflush(stdout);
	}
}

/*
 * Places a picture copied over a frame of a size: in its middle, cut to
 * the frame (the part of the picture left of and above the frame's edge
 * when it is larger).  A frame without room gets a place of size 0.
 */
void
shell_render_place_overlay(
	const struct shell_overlay *overlay,
	uint32_t width,
	uint32_t height,
	struct kl_rect *place)
{
	/* The picture's size, cut to the frame. */
	place->width = (int)overlay->width;
	place->height = (int)overlay->height;
	if (place->width > (int)width)
		place->width = (int)width;
	if (place->height > (int)height)
		place->height = (int)height;

	/* In the middle. */
	place->x = ((int)width - place->width) / 2;
	place->y = ((int)height - place->height) / 2;
}

/*
 * Takes the CPU's frame the window's thread shows, on the window's
 * thread: the frame drawn last when a new one was drawn (*fresh 1; the
 * frame shown before goes back to the drawing thread), else the one taken
 * before (*fresh 0).  The frame is the window's until the next call.
 */
const struct shell_cpu_frame *
shell_render_cpu_frame(
	struct shell_render *render,
	int *fresh)
{
	const struct shell_cpu_frame *frame;
	unsigned shown;

	/* The window's thread reads the state from here on: a later frame wakes it again. */
	__atomic_store_n(&render->told, 0, __ATOMIC_RELEASE);

	/* The frame drawn last, swapped with the one shown. */
	kl_mutex_lock(&render->lock);

	*fresh = 0;
	if (render->fresh) {
		shown = render->front;
		render->front = render->ready;
		render->ready = shown;
		render->fresh = 0;
		*fresh = 1;
	}
	frame = &render->frames[render->front];

	kl_mutex_unlock(&render->lock);

	/* Succeeded: the frame to show. */
	return frame;
}

/*
 * Reports how the drawing goes, on the window's thread: how many frames
 * the GPU's way presented and the size of the last.  Returns nonzero when
 * the drawing failed (the window closes).
 */
int
shell_render_status(
	struct shell_render *render,
	unsigned long *shown,
	uint32_t *width,
	uint32_t *height)
{
	int failed;

	/* The window's thread reads the state from here on: a later frame wakes it again. */
	__atomic_store_n(&render->told, 0, __ATOMIC_RELEASE);

	/* The counts. */
	kl_mutex_lock(&render->lock);

	*shown = render->shown;
	*width = render->shown_width;
	*height = render->shown_height;
	failed = render->failed;

	kl_mutex_unlock(&render->lock);

	/* Succeeded: the window's thread knows. */
	return failed;
}

/*
 * Stops the drawing thread and waits for its end, on the window's thread,
 * taking the window's events meanwhile (an acknowledgement asked, or a
 * present that needs the window's thread).  Nothing when it never started.
 */
void
shell_render_stop(
	struct shell_render *render,
	struct shell_window *window)
{
	int ended;

	/* A thread that never started. */
	if (render->thread == NULL)
		return;

	/* The stop, told. */
	kl_mutex_lock(&render->lock);

	render->stop = 1;
	kl_cond_broadcast(&render->changed);

	kl_mutex_unlock(&render->lock);

	/* Its end, the window's events taken between short waits. */
	for (;;) {
		kl_mutex_lock(&render->lock);

		if (!render->ended)
			(void)kl_cond_wait_for(&render->changed, &render->lock, RENDER_STOP_MS);
		ended = render->ended;

		kl_mutex_unlock(&render->lock);
		if (ended)
			break;
		(void)shell_window_dispatch(window, 0);
	}

	/* The thread, given back. */
	(void)kl_thread_join(render->thread);
	render->thread = NULL;
}

/*
 * Releases what the drawing thread kept (once it is stopped and the view
 * that woke it is gone).
 */
void
shell_render_close(
	struct shell_render *render)
{
	unsigned index;

	/* Never made. */
	if (!render->made)
		return;

	/* The CPU's frames. */
	for (index = 0U; index < SHELL_CPU_FRAMES; index++)
		free(render->frames[index].pixels);

	/* The pictures copied over the frames. */
	free(render->overlay.pixels);
	free(render->drawn_overlay.pixels);

	/* The condition and the lock, and nothing is held any more. */
	kl_cond_destroy(&render->changed);
	kl_mutex_destroy(&render->lock);
	memset(render, 0, sizeof(*render));
}

/* The drawing thread: waits for something to draw, draws it, until it is stopped or the drawing fails. */
static void
render_run(
	void *argument)
{
	struct shell_render *render;
	struct render_round round;
	int retry;
	int error;

	/* Draws whenever there is something new to draw. */
	render = argument;
	retry = 0;
	for (;;) {
		render_wait(render, retry, &round);
		if (round.stop)
			break;

		/* The round, with the GPU or the CPU. */
		retry = 0;
		if (render->present != NULL) {
			error = render_gpu(render, &round, &retry);
		} else {
			error = render_cpu(render, &round);
		}

		/* A failure ends the drawing, and the window's thread closes the window. */
		if (error != 0) {
			kl_mutex_lock(&render->lock);

			render->failed = 1;

			kl_mutex_unlock(&render->lock);
			render_tell(render);
			break;
		}
	}

	/* The end, for the window's thread's stop. */
	kl_mutex_lock(&render->lock);

	render->ended = 1;
	kl_cond_broadcast(&render->changed);

	kl_mutex_unlock(&render->lock);
}

/*
 * Waits until there is something to draw (a committed frame, a new size
 * or a configure, with a frame committed) or the stop, and takes it.  With
 * retry, a round that could not draw is tried again without waiting.
 */
static void
render_wait(
	struct shell_render *render,
	int retry,
	struct render_round *round)
{
	int due;

	/* The state, once there is work. */
	kl_mutex_lock(&render->lock);

	/* Waits for the stop or the work. */
	for (;;) {
		if (render->stop)
			break;
		due = 0;
		if (render->committed || render->sized || render->overlay_changed || retry)
			due = 1;
		if (!render->has_commit)
			due = 0;
		if (render->configure)
			due = 1;
		if (due)
			break;
		kl_cond_wait(&render->changed, &render->lock);
	}

	/* What the round does: the size, the configure, and nothing new left behind. */
	round->stop = render->stop;
	round->width = render->width;
	round->height = render->height;
	round->configure = render->configure;
	round->serial = render->configure_serial;
	round->has_commit = render->has_commit;
	render->committed = 0;
	render->sized = 0;
	render->configure = 0;

	kl_mutex_unlock(&render->lock);

	/* The picture over the frames, as it is now. */
	render_take_overlay(render);
}

/*
 * One round with the GPU: the swapchain at the size, the configure
 * acknowledged, and the latest committed frame drawn and presented.
 * *retry asks for another round at once (the compositor gave no image in
 * time, or the swapchain went out of date).  Returns 0, or 1 after saying
 * why the drawing failed.
 */
static int
render_gpu(
	struct shell_render *render,
	const struct render_round *round,
	int *retry)
{
	struct shell_present *present;
	uint64_t serial;
	VkResult result;
	int error;

	/* A window minimized is not drawn; its configure needs no frame. */
	present = render->present;
	if (round->width == 0U || round->height == 0U) {
		if (round->configure)
			(void)render_ask_ack(render, round->serial);
		return 0;
	}

	/* The swapchain at the size. */
	if (round->width != present->extent.width || round->height != present->extent.height) {
		result = shell_present_resize(present, render->view, round->width, round->height);
		if (result != VK_SUCCESS) {
			fprintf(stderr, "browser: cannot resize: %s failed (%d)\n", present->operation, (int)result);
			return 1;
		}
	}

	/* The configure acknowledged before the first frame of its size; a newer one taken meanwhile is the next round's. */
	if (round->configure) {
		error = render_ask_ack(render, round->serial);
		if (error == EINVAL || error == ECANCELED)
			return 0;
		if (error != 0) {
			render_requeue(render, round->serial);
			*retry = 1;
			return 0;
		}
	}

	/* Nothing to draw before the page committed a frame. */
	if (!round->has_commit)
		return 0;

	/* The frame. */
	result = shell_present_frame(present, render->view, &render->drawn_overlay, &serial);

	/* The compositor gave no image back in time: once more (the stop is looked at between). */
	if (result == VK_TIMEOUT || result == VK_NOT_READY) {
		*retry = 1;
		return 0;
	}

	/* A swapchain that no longer fits the window is replaced, and the frame drawn in the next round. */
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		result = shell_present_resize(present, render->view, round->width, round->height);
		if (result != VK_SUCCESS) {
			fprintf(stderr, "browser: cannot resize: %s failed (%d)\n", present->operation, (int)result);
			return 1;
		}
		*retry = 1;
		return 0;
	}

	/* Says what failed. */
	if (result != VK_SUCCESS) {
		fprintf(stderr, "browser: cannot draw a frame: %s failed (%d)\n", present->operation, (int)result);
		return 1;
	}

	/* The page's animation frames run on the present. */
	browser_view_presented(render->view, serial);

	/* The window's thread hears of the frame (its FRAME line and its text input). */
	kl_mutex_lock(&render->lock);

	render->shown++;
	render->shown_width = present->extent.width;
	render->shown_height = present->extent.height;

	kl_mutex_unlock(&render->lock);
	render_tell(render);

	/* Succeeded: the frame is in the window. */
	return 0;
}

/*
 * One round with the CPU: the latest committed frame drawn into the
 * drawing thread's frame at the size, which becomes the one drawn last for
 * the window's thread to show.  Returns 0, or 1 after saying why the
 * drawing failed.
 */
static int
render_cpu(
	struct shell_render *render,
	const struct render_round *round)
{
	struct shell_cpu_frame *frame;
	unsigned drawn;
	int error;

	/* Nothing to draw before the page committed a frame, nor in a window minimized. */
	if (!round->has_commit)
		return 0;
	if (round->width == 0U || round->height == 0U)
		return 0;

	/* The drawing thread's frame at the size. */
	frame = &render->frames[render->back];
	error = render_frame_fit(frame, round->width, round->height);
	if (error != 0) {
		fprintf(stderr, "browser: cannot draw a frame: %s\n", strerror(error));
		return 1;
	}

	/* The page's latest committed frame (none yet is no frame). */
	error = browser_view_render_pixels(render->view, frame->pixels, frame->width, frame->height,
	    (size_t)frame->width * sizeof(uint32_t));
	if (error == ENOENT)
		return 0;
	if (error != 0) {
		fprintf(stderr, "browser: cannot draw a frame: %s\n", strerror(error));
		return 1;
	}

	/* The picture over it (the question's card), when there is one. */
	render_overlay_blit(&render->drawn_overlay, frame);

	/* The frame becomes the one drawn last; the one it replaces, not shown, is drawn into next. */
	kl_mutex_lock(&render->lock);

	drawn = render->back;
	render->back = render->ready;
	render->ready = drawn;
	render->fresh = 1;

	kl_mutex_unlock(&render->lock);
	render_tell(render);

	/* Succeeded: the window's thread shows it. */
	return 0;
}

/*
 * Asks the window's thread to acknowledge a configure and waits for its
 * answer.  Returns 0 once acknowledged (or acknowledged before),
 * EINVAL when a newer configure was taken meanwhile, ECANCELED when the
 * drawing thread is stopped, or the post's error.
 */
static int
render_ask_ack(
	struct shell_render *render,
	uint32_t serial)
{
	int error;

	/* What is asked. */
	kl_mutex_lock(&render->lock);

	render->ack_serial = serial;
	render->ack_error = 0;
	render->ack_state = RENDER_ACK_ASKED;

	kl_mutex_unlock(&render->lock);

	/* The request, among the window's inputs. */
	error = kl_window_post(render->kui, SHELL_POST_ACK, NULL);
	if (error != 0) {
		kl_mutex_lock(&render->lock);

		render->ack_state = RENDER_ACK_NONE;

		kl_mutex_unlock(&render->lock);
		return error;
	}

	/* The answer, or the stop. */
	kl_mutex_lock(&render->lock);

	/* Waits for the window's thread, looking at the stop between. */
	while (render->ack_state == RENDER_ACK_ASKED && !render->stop)
		(void)kl_cond_wait_for(&render->changed, &render->lock, RENDER_WAIT_MS);
	error = ECANCELED;
	if (render->ack_state == RENDER_ACK_DONE)
		error = render->ack_error;
	render->ack_state = RENDER_ACK_NONE;

	kl_mutex_unlock(&render->lock);

	/* A configure acknowledged before is acknowledged. */
	if (error == EALREADY)
		error = 0;
	if (error != 0)
		return error;

	/* Succeeded: the frames of its size may be presented. */
	return 0;
}

/* Puts back a configure whose acknowledgement could not be asked for, unless a newer one came. */
static void
render_requeue(
	struct shell_render *render,
	uint32_t serial)
{
	/* The configure, waiting again. */
	kl_mutex_lock(&render->lock);

	if (!render->configure) {
		render->configure = 1;
		render->configure_serial = serial;
	}

	kl_mutex_unlock(&render->lock);
}

/* Wakes the window's thread, once for a run of frames (it clears told before it reads the state). */
static void
render_tell(
	struct shell_render *render)
{
	int told;

	/* Already told and not yet read. */
	told = __atomic_exchange_n(&render->told, 1, __ATOMIC_ACQ_REL);
	if (told)
		return;

	/* The window's thread's wake. */
	render->wake(render->wake_context);
}

/* Makes a CPU's frame's memory the size; 0, or ENOMEM. */
static int
render_frame_fit(
	struct shell_cpu_frame *frame,
	uint32_t width,
	uint32_t height)
{
	uint32_t *pixels;
	size_t count;

	/* The size it has already. */
	if (frame->pixels != NULL && frame->width == width && frame->height == height)
		return 0;

	/* Memory for the new size (a size too large to count is refused). */
	if ((size_t)width > SIZE_MAX / (size_t)height / sizeof(uint32_t))
		return ENOMEM;
	count = (size_t)width * (size_t)height;
	pixels = calloc(count, sizeof(uint32_t));
	if (pixels == NULL)
		return ENOMEM;

	/* The new memory replaces the old. */
	free(frame->pixels);
	frame->pixels = pixels;
	frame->width = width;
	frame->height = height;

	/* Succeeded: the frame is the size. */
	return 0;
}

/* Takes the window's picture over the frames into the drawing thread's own copy, when it changed. */
static void
render_take_overlay(
	struct shell_render *render)
{
	struct shell_overlay *mine;
	int error;

	/* The window's picture, copied under the lock. */
	mine = &render->drawn_overlay;
	kl_mutex_lock(&render->lock);

	error = 0;
	if (render->overlay_changed) {
		error = render_overlay_fit(mine, render->overlay.width, render->overlay.height);
		if (error == 0 && mine->width != 0U)
			memcpy(mine->pixels, render->overlay.pixels, (size_t)mine->width * (size_t)mine->height * sizeof(uint32_t));
		render->overlay_changed = 0;
	}

	kl_mutex_unlock(&render->lock);

	/* Memory that ran out leaves no picture. */
	if (error != 0)
		mine->width = 0U;
}

/* Makes a picture's memory hold a size (0: none, the memory kept); 0, or ENOMEM. */
static int
render_overlay_fit(
	struct shell_overlay *overlay,
	uint32_t width,
	uint32_t height)
{
	uint32_t *pixels;
	size_t count;

	/* None. */
	overlay->width = 0U;
	overlay->height = 0U;
	if (width == 0U || height == 0U)
		return 0;

	/* Memory for more words when it holds too few (a size too large to count is refused). */
	if ((size_t)width > SIZE_MAX / (size_t)height / sizeof(uint32_t))
		return ENOMEM;
	count = (size_t)width * (size_t)height;
	if (count > overlay->capacity) {
		pixels = realloc(overlay->pixels, count * sizeof(uint32_t));
		if (pixels == NULL)
			return ENOMEM;
		overlay->pixels = pixels;
		overlay->capacity = count;
	}

	/* Succeeded: the memory holds the size. */
	overlay->width = width;
	overlay->height = height;
	return 0;
}

/* Copies the picture over a CPU's frame, in its middle. */
static void
render_overlay_blit(
	const struct shell_overlay *overlay,
	struct shell_cpu_frame *frame)
{
	struct kl_rect place;
	int row;

	/* No picture, or no room. */
	if (overlay->width == 0U)
		return;
	shell_render_place_overlay(overlay, frame->width, frame->height, &place);

	/* Each row of the picture that fits. */
	for (row = 0; row < place.height; row++) {
		memcpy(frame->pixels + (size_t)(place.y + row) * frame->width + (size_t)place.x,
		    overlay->pixels + (size_t)row * overlay->width, (size_t)place.width * sizeof(uint32_t));
	}
}
