/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display thread and the changes of the display: what
 * waits when the output opens, closes or moves -- the swapchain's and the
 * surface's creation and destruction (which claim and give back the
 * display), the cursor plane's, the cursor thread's stop, the device's
 * idle -- runs on one resident thread, a lane named "display", while the
 * event loop goes on taking the clients' requests and the input.
 *
 * A change of the display begins (kwl_display_begin): nothing is wanted of
 * the render thread from then on, and its park is asked without waiting.
 * The render thread parks after the frame in flight and the tasks given
 * before (drained: nothing it submitted still runs on the GPU) and says so
 * through its queue; the change goes on from the event loop's next look
 * (kwl_display_parked).  Its steps (kwl_display_close, kwl_display_open)
 * each do the main thread's part (what the frames use: the targets, the
 * scene, the layers, the heads), give the display thread its part, and go
 * on when the display thread's result is taken after the wake pipe
 * (kwl_display_take).  The change ends (kwl_display_end): the render
 * thread goes on and the next pass draws.  The display thread writes no
 * field of the compositor's: it works on the job's copies (the swapchain's
 * record, the cursor thread's) and the main thread takes them in.
 *
 * Where waiting is the point -- the end, the hand-over to another
 * compositor, the seat's pause -- or without the thread, the same steps
 * run on the event loop's thread, one after the other (display_inline).
 * A change running when such a caller comes ends first (kwl_display_settle).
 *
 * The game mode's display swapchain -- made at the display's mode with
 * images that may go out, their fds exported for the client they are lent
 * to, and destroyed once the present thread's present has returned
 * (kwl_display_scanout_open and _close) -- the heads' swapchains
 * (kwl_display_heads) and the reading of the displays connected
 * (kwl_display_enumerate) are the display thread's too.  A job that
 * must run on the event loop's thread waits for the thread's jobs given
 * before it, so a release given to the thread comes before a swapchain
 * made here.
 */

#include "compose.h"
#include "cursor-thread.h"
#include "extras.h"

#include "userland/desktop/libkeiland-backend/keiland-backend-display.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* How long a change of the display may take before a caller that waits for it ends the compositor, in milliseconds. */
#define DISPLAY_SETTLE_MS	10000U

/* How long one wait for the render thread's park lasts while a caller waits, in milliseconds. */
#define DISPLAY_PARK_LOOK_MS	100U

/* What a job of the display thread does. */
enum display_kind {
	/* The output's cursor plane and swapchain destroyed, the display given back. */
	DISPLAY_CLOSE = 1,
	/* The display taken, the output's swapchain made, and the cursor plane's. */
	DISPLAY_OPEN,
	/* The game mode's display swapchain made, its images exported. */
	DISPLAY_SCANOUT_OPEN,
	/* The game mode's display swapchain destroyed, once its present has returned. */
	DISPLAY_SCANOUT_CLOSE,
	/* Heads' swapchains destroyed (their displays given back) and made. */
	DISPLAY_HEADS,
	/* The displays connected read. */
	DISPLAY_ENUMERATE,
	/* The backlight opened and read. */
	DISPLAY_LIGHT_OPEN,
	/* The backlight set and read again. */
	DISPLAY_LIGHT_SET
};

/* The key of the backlight's setting: only the latest one not started yet is run. */
#define DISPLAY_LIGHT_KEY	1U

/*
 * One job of the display thread: its kind, the server (read on the thread
 * for the backend and the device only), what goes on with its result
 * (then and context), and its inputs and results.
 *
 * output is the output's swapchain record (the one to destroy, or the
 * prepared surface whose swapchain is made, as the main thread gave it),
 * display the output's display; parts the cursor plane taken off the
 * output (DISPLAY_CLOSE), plane whether a cursor plane is to be made and
 * cursor, plane_extent and plane_index the one made (DISPLAY_OPEN);
 * readback and scene_supported what the swapchain's images allow; game
 * the game mode's display swapchain (the main thread's record, whose
 * Vulkan part DISPLAY_SCANOUT_OPEN makes and DISPLAY_SCANOUT_CLOSE
 * destroys); ran says the job was run (a release dropped at the end runs
 * then, a game chain made and dropped is destroyed); rests and opens
 * (rest_count and open_count of them, the caller's until its then) the
 * heads' swapchains to destroy and to make (DISPLAY_HEADS); list the
 * displays read (DISPLAY_ENUMERATE, the caller's until its then);
 * percent the backlight's brightness asked for and read
 * (DISPLAY_LIGHT_*); error the result.  spare says the job is display_spare (no memory for one).
 */
struct display_job {
	struct kwl_lane_job lane;
	enum display_kind kind;
	struct kwl_server *server;
	kwl_display_fn *then;
	void *context;
	struct vkdemo_display output;
	VkDisplayKHR display;
	struct kwl_cursor_parts parts;
	struct kwl_head_rest heads[KWL_HEADS];
	unsigned head_count;
	unsigned plane;
	struct kwl_cursor_thread *cursor;
	VkExtent2D plane_extent;
	uint32_t plane_index;
	int readback;
	unsigned scene_supported;
	struct kwl_head_rest *rests;
	unsigned rest_count;
	struct kwl_head_opening *opens;
	unsigned open_count;
	struct kwl_display_list *list;
	struct kwl_game_chain *game;
	unsigned ran;
	unsigned percent;
	int error;
	unsigned spare;
};

/* A change of the display made of its steps: what goes on with its result (then and context). */
struct display_change {
	kwl_display_fn *then;
	void *context;
};

static struct display_job *display_job_new(struct kwl_server *server, enum display_kind kind, kwl_display_fn *then, void *context);
static void display_job_free(struct display_job *job);
static void display_submit(struct kwl_server *server, struct display_job *job);
static void display_run(struct kwl_lane_job *lane);
static void display_done(void *context, struct kwl_lane_job *lane);
static void display_drop(struct kwl_lane_job *lane);
static void display_done_step(struct kwl_server *server, struct display_job *job);
static void display_light_submit(struct kwl_server *server, struct display_job *job, uint64_t key);
static void display_open_run(struct display_job *job);
static void display_game_open_run(struct display_job *job);
static void display_game_close(struct kwl_server *server, struct kwl_game_chain *chain);
static struct display_change *display_change_new(kwl_display_fn *then, void *context);
static void display_change_end(struct kwl_server *server, struct display_change *change, int error);
static void display_open_parked(struct kwl_server *server, int error, void *context);
static void display_open_opened(struct kwl_server *server, int error, void *context);
static void display_close_parked(struct kwl_server *server, int error, void *context);
static void display_close_closed(struct kwl_server *server, int error, void *context);
static void display_sync_then(struct kwl_server *server, int error, void *context);

/* The display thread's jobs. */
static const struct kwl_lane_ops display_ops = {
	display_run,
	display_done,
	display_drop
};

/* The built-in panel's backlight, used by the display thread only (or here once it has stopped). */
static struct kl_backend_backlight *display_light;

/* The job used when no memory is left for one: run on the event loop's thread at once, so one is enough. */
static struct display_job display_spare;
static unsigned display_spare_used;

/*
 * Starts the display thread once the render thread runs (it needs the
 * threads' wake pipe).  Without it the changes run on the event loop's
 * thread (logged).
 */
void
kwl_display_start(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	int error;

	/* Once, after the wake pipe. */
	compose = server->compose;
	if (compose == NULL || compose->display_started || !server->threads_prepared)
		return;

	/* The game mode's present thread first (the display thread waits on it for a release). */
	kwl_scanout_start(server);

	/* The thread, which waits for the first job. */
	error = kwl_lane_start(&compose->display_lane, "display", 1U, server->wake[1]);
	if (error != 0) {
		printf("KWL DISPLAY thread=display errno=%d the display changes run in the event loop\n", error);
		return;
	}

	/* Succeeded: the changes run there from now on; the panel's backlight is opened and read there first. */
	compose->display_started = 1U;
	kwl_backlight_start(server);
}

/*
 * Stops the display thread at the end, after the change running (if any)
 * has ended, then the game mode's present thread.
 */
void
kwl_display_stop(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* Nothing runs. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* The change running ends, then the thread (a question not taken is dropped, a release runs here). */
	if (compose->display_started) {
		kwl_display_settle(server);
		kwl_lane_stop(&compose->display_lane);
		compose->display_started = 0U;
	}

	/* Then the game mode's present thread (a game mode still shown is left here). */
	kwl_scanout_stop(server);
}

/*
 * Takes what the display thread finished, on the event loop's thread,
 * after its wake pipe: each change goes on with its step's result.
 */
void
kwl_display_take(
	struct kwl_server *server)
{
	/* Nothing runs. */
	if (server->compose == NULL || !server->compose->display_started)
		return;

	/* Each result, in its order. */
	(void)kwl_lane_take(&server->compose->display_lane, server);
}

/*
 * Tells whether a change of the display runs.
 *
 * No frame is drawn meanwhile.
 */
int
kwl_display_busy(
	struct kwl_server *server)
{
	/* Without the device nothing changes. */
	if (server->compose == NULL)
		return 0;

	/* Succeeded: whether one runs. */
	return (int)server->compose->display_busy;
}

/*
 * Waits for the change of the display running (if any) to end, taking the
 * render thread's park and the display thread's results meanwhile: only
 * where waiting is the point (the end, the hand-over, the seat's pause),
 * or before a park of the event loop's own while a change's park is asked.
 * A change that does not end in DISPLAY_SETTLE_MS ends the compositor.
 */
void
kwl_display_settle(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t deadline;
	uint64_t now;
	unsigned taken;
	unsigned busy;

	/* Nothing runs, or this is a step of it (which cannot wait for its own end). */
	compose = server->compose;
	if (compose == NULL || !compose->display_busy || compose->display_in_step != 0U)
		return;

	/* Until it ends. */
	deadline = kwl_milliseconds() + DISPLAY_SETTLE_MS;
	while (compose->display_busy) {
		/* A change that lasts leaves nothing safe. */
		now = kwl_milliseconds();
		if (now > deadline) {
			kwl_fatal("KWL FAILED site=display errno=%d step=settle parking=%u", ETIMEDOUT, compose->display_parking);
			_exit(1);
		}

		/* Its park: the render thread's queue, which tells it, then the next look. */
		if (compose->display_parking) {
			(void)kwl_render_wait_message(&compose->render, DISPLAY_PARK_LOOK_MS);
			kwl_compose_poll(server);
			continue;
		}

		/* A step's result the display thread finished, which may give the next step. */
		taken = kwl_lane_take(&compose->display_lane, server);
		if (taken != 0U)
			continue;

		/* A step on the display thread: its end. */
		busy = kwl_lane_busy(&compose->display_lane);
		if (busy) {
			kwl_lane_finish(&compose->display_lane, server);
			continue;
		}

		/* Nothing it waits for: a change left half done is a fault of this file. */
		kwl_fatal("KWL FAILED site=display errno=%d step=settle-stuck", EINVAL);
		_exit(1);
	}
}

/*
 * Begins a change of the display: nothing is wanted of the render thread
 * from now on, and its park is asked; parked(server, 0, context) is called
 * once it is parked and drained, on the event loop's thread (at once when
 * the steps run there).  Returns 0, or EBUSY while another change runs
 * (the caller tries again later).
 */
int
kwl_display_begin(
	struct kwl_server *server,
	kwl_display_fn *parked,
	void *context)
{
	struct kwl_render_message *message;
	struct kwl_compose *compose;

	/* One change at a time. */
	compose = server->compose;
	if (compose->display_busy)
		return EBUSY;
	compose->display_busy = 1U;
	compose->display_began_ms = kwl_milliseconds();

	/* No frame from now on. */
	kwl_compose_want_nothing(server);

	/*
	 * The park asked without waiting, told by its message; where the steps
	 * run here, or inside a park of the event loop's own (the thread is
	 * parked already, and stays so until this change ends too), at once.
	 */
	message = NULL;
	if (compose->render_started && !compose->display_inline && compose->park_depth == 0U)
		message = calloc(1, sizeof(*message));
	if (message == NULL) {
		compose->display_in_step++;
		kwl_compose_park(server);
		parked(server, 0, context);
		compose->display_in_step--;
		return 0;
	}

	/* Succeeded: the change goes on at the park's message (kwl_display_parked). */
	compose->display_parking = 1U;
	compose->display_step = parked;
	compose->display_context = context;
	kwl_render_park_request(&compose->render, message);
	return 0;
}

/*
 * Goes on with the change whose park the render thread answered (its
 * message taken by kwl_compose_poll, after the completions before it).
 */
void
kwl_display_parked(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	kwl_display_fn *step;
	void *context;

	/* Only a park asked by a change. */
	compose = server->compose;
	if (compose == NULL || !compose->display_parking)
		return;

	/* Parked as kwl_compose_park would have, until kwl_display_end. */
	compose->display_parking = 0U;
	compose->park_depth++;
	step = compose->display_step;
	context = compose->display_context;
	compose->display_step = NULL;
	compose->display_context = NULL;

	/* The change's next step. */
	compose->display_in_step++;
	step(server, 0, context);
	compose->display_in_step--;
}

/*
 * Ends a change of the display: the render thread goes on, and the next
 * pass draws.
 */
void
kwl_display_end(
	struct kwl_server *server)
{
	struct kwl_compose *compose;

	/* Only a change that runs. */
	compose = server->compose;
	if (compose == NULL || !compose->display_busy)
		return;

	/* The render thread steps again, and everything is drawn. */
	kwl_compose_unpark(server);
	compose->display_busy = 0U;
	server->dirty = 1;
	printf("KWL DISPLAY change ms=%llu\n", (unsigned long long)(kwl_milliseconds() - compose->display_began_ms));
}

/*
 * Closes the output, a step of a change of the display (the render thread
 * parked and drained): the main thread lets go of what the frames use and
 * takes the cursor plane off; the display thread stops the cursor thread,
 * waits for the device to be idle and destroys the plane's objects, the
 * heads' and the output's swapchains and surfaces, which gives the
 * displays back.
 * then(server, 0, context) is called after.  A surface prepared but never
 * given a swapchain goes at once.
 */
void
kwl_display_close(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	struct kwl_compose *compose;
	struct display_job *job;
	uint64_t started;

	/* A surface prepared but never given a swapchain goes on its own. */
	compose = server->compose;
	if (!compose->output_open) {
		if (compose->output_prepared)
			vkdemo_display_close(compose->instance, compose->device, &compose->output);
		compose->output_prepared = 0;
		then(server, 0, context);
		return;
	}

	/* The plane taken off, then what the frames use let go (its time on the event loop logged). */
	job = display_job_new(server, DISPLAY_CLOSE, then, context);
	started = kwl_milliseconds();
	kwl_cursor_plane_detach(server, &job->parts);
	kwl_compose_output_let_go(server, job->heads, &job->head_count);
	printf("KWL DISPLAY step=let-go ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The swapchain and the surface are the display thread's from now on. */
	job->output = compose->output;
	job->display = compose->display;
	memset(&compose->output, 0, sizeof(compose->output));
	compose->output_prepared = 0;
	display_submit(server, job);
}

/*
 * Opens the output, a step of a change of the display (the render thread
 * parked): the main thread prepares the display's surface (and the
 * pipelines the first time); the display thread takes the display, makes
 * the swapchain and the cursor plane's objects; the main thread takes them
 * in (the targets, the scene, the layers).  then(server, error, context) is
 * called after, with 0 or the step's errno (nothing of the output is kept
 * then).  An open output needs nothing.
 */
void
kwl_display_open(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	struct kwl_compose *compose;
	struct display_job *job;
	int error;

	/* An open output needs nothing. */
	compose = server->compose;
	if (compose->output_open) {
		then(server, 0, context);
		return;
	}

	/* The surface, and the pass and pipelines for its format. */
	error = kwl_compose_output_prepare(server);
	if (error != 0) {
		then(server, error, context);
		return;
	}

	/* The display thread's part on a copy of the record, which the main thread does not use until it is taken in. */
	job = display_job_new(server, DISPLAY_OPEN, then, context);
	job->output = compose->output;
	job->display = compose->display;
	job->plane = server->threads_prepared;
	display_submit(server, job);
}

/*
 * Destroys heads' swapchains and surfaces and gives their displays back
 * (rests, once the device is idle), then makes heads' swapchains (opens,
 * each with its result), on the display thread, a step of a change of the
 * display (the render thread parked and drained);
 * then(server, 0, context) is called after.
 */
void
kwl_display_heads(
	struct kwl_server *server,
	struct kwl_head_rest *rests,
	unsigned rest_count,
	struct kwl_head_opening *opens,
	unsigned open_count,
	kwl_display_fn *then,
	void *context)
{
	struct display_job *job;

	/* Nothing to do. */
	if (rest_count == 0U && open_count == 0U) {
		then(server, 0, context);
		return;
	}

	/* The job. */
	job = display_job_new(server, DISPLAY_HEADS, then, context);
	job->rests = rests;
	job->rest_count = rest_count;
	job->opens = opens;
	job->open_count = open_count;
	display_submit(server, job);
}

/*
 * Reads the displays connected now (up to the ones followed) into list,
 * their names copied.  It touches no field of the compositor's, so the
 * display thread may call it.
 */
void
kwl_display_list_read(
	struct kwl_server *server,
	struct kwl_display_list *list)
{
	VkDisplayPropertiesKHR properties[KWL_COMPOSE_DISPLAYS];
	uint32_t count;
	unsigned index;

	/* The displays. */
	memset(list, 0, sizeof(*list));
	count = KWL_COMPOSE_DISPLAYS;
	list->result = vkGetPhysicalDeviceDisplayPropertiesKHR(server->compose->physical, &count, properties);
	if (list->result != VK_SUCCESS && list->result != VK_INCOMPLETE)
		return;

	/* Each one's handle, name and size. */
	list->result = VK_SUCCESS;
	list->count = count;
	for (index = 0U; index < count; index++) {
		list->displays[index] = properties[index].display;
		if (properties[index].displayName != NULL)
			(void)snprintf(list->names[index], sizeof(list->names[index]), "%s", properties[index].displayName);
		list->widths[index] = properties[index].physicalResolution.width;
		list->heights[index] = properties[index].physicalResolution.height;
	}
}

/*
 * Reads the displays connected now on the display thread (the backend may
 * probe the connectors), into list; then(server, 0, context) is called
 * after, on the event loop's thread.
 */
void
kwl_display_enumerate(
	struct kwl_server *server,
	struct kwl_display_list *list,
	kwl_display_fn *then,
	void *context)
{
	struct display_job *job;

	/* The job. */
	job = display_job_new(server, DISPLAY_ENUMERATE, then, context);
	job->list = list;
	display_submit(server, job);
}

/*
 * Asks the display thread to make the game mode's display swapchain of a
 * record (the window swapchain closed): the display's mode, the format and
 * the client's image count the main thread put in it, images that may go
 * out, and their fds.  then(server, error, context) is called on the event
 * loop's thread with 0, ENOTSUP when the display's chain cannot lend its
 * images, or another failure (nothing of the chain is left then).
 */
void
kwl_display_scanout_open(
	struct kwl_server *server,
	struct kwl_game_chain *chain,
	kwl_display_fn *then,
	void *context)
{
	struct display_job *job;

	/* The swapchain to make. */
	job = display_job_new(server, DISPLAY_SCANOUT_OPEN, then, context);
	job->game = chain;
	display_submit(server, job);
}

/*
 * Asks the display thread to destroy the game mode's display swapchain
 * once the present thread's present (if any) has returned, which gives
 * the display back; then(server, 0, context) is called after, on the event
 * loop's thread (at the end too, the release run there if the thread had
 * not run it).
 */
void
kwl_display_scanout_close(
	struct kwl_server *server,
	struct kwl_game_chain *chain,
	kwl_display_fn *then,
	void *context)
{
	struct display_job *job;

	/* The release. */
	job = display_job_new(server, DISPLAY_SCANOUT_CLOSE, then, context);
	job->game = chain;
	display_submit(server, job);
}

/*
 * Opens window mode's output as a change of its own: begun, opened, ended,
 * then the desktop fitted to what the output shows now against what it
 * showed when it last opened (kwl_output_rotated).  then(server, error,
 * context) is called after.  Returns 0 (also for an open output, then
 * called at once), or EBUSY while another change runs.
 */
int
kwl_display_output_open(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	struct display_change *change;
	int error;

	/* An open output needs nothing. */
	if (server->compose->output_open) {
		then(server, 0, context);
		return 0;
	}

	/* The change. */
	change = display_change_new(then, context);
	if (change == NULL)
		return ENOMEM;
	error = kwl_display_begin(server, display_open_parked, change);

	/* A change that did not begin is freed here. */
	if (error != 0) {
		free(change);
		return error;
	}

	/* Succeeded: the change began. */
	return 0;
}

/*
 * Closes window mode's output as a change of its own, without waiting:
 * begun, closed, ended (the display given back).  then(server, error,
 * context) is called after.  Returns 0 (also when nothing is open, then
 * called at once), ENOMEM, or EBUSY while another change runs.
 */
int
kwl_display_output_close(
	struct kwl_server *server,
	kwl_display_fn *then,
	void *context)
{
	struct display_change *change;
	struct kwl_compose *compose;
	int error;

	/* Nothing to destroy. */
	compose = server->compose;
	if (compose == NULL || (!compose->output_open && !compose->output_prepared)) {
		then(server, 0, context);
		return 0;
	}

	/* The change. */
	change = display_change_new(then, context);
	if (change == NULL)
		return ENOMEM;
	error = kwl_display_begin(server, display_close_parked, change);

	/* A change that did not begin is freed here. */
	if (error != 0) {
		free(change);
		return error;
	}

	/* Succeeded: the change began. */
	return 0;
}

/*
 * Creates the swapchain of window mode's display surface (preparing the
 * surface first when that was not done), which claims the display, and
 * waits for it: where the caller needs the output open on return.
 * Returns 0 or the open's errno.
 */
int
kwl_compose_output_open(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	int result;
	int error;

	/* An open output needs nothing. */
	compose = server->compose;
	if (compose->output_open)
		return 0;

	/* A change running ends first; this one's steps run here. */
	kwl_display_settle(server);
	result = EINPROGRESS;
	compose->display_inline++;
	error = kwl_display_output_open(server, display_sync_then, &result);
	compose->display_inline--;
	if (error != 0)
		return error;

	/* The open's result. */
	return result;
}

/*
 * Destroys the swapchain and the display surface once the device is idle,
 * which gives the display back, and waits for it (the hand-over, the
 * seat's pause, the end).
 */
void
kwl_compose_output_close(
	struct kwl_server *server)
{
	struct display_change *change;
	struct kwl_compose *compose;
	int result;
	int error;

	/* Nothing to destroy. */
	compose = server->compose;
	if (compose == NULL)
		return;

	/* A change running ends first. */
	kwl_display_settle(server);
	if (!compose->output_open && !compose->output_prepared)
		return;

	/* This one's steps run here. */
	result = EINPROGRESS;
	change = display_change_new(display_sync_then, &result);
	if (change == NULL) {
		kwl_fatal("KWL FAILED site=display errno=%d step=close", ENOMEM);
		_exit(1);
	}

	/* Begins the close inline, so that its steps run on this thread. */
	compose->display_inline++;
	error = kwl_display_begin(server, display_close_parked, change);
	compose->display_inline--;
	if (error != 0) {
		free(change);
		kwl_fatal("KWL FAILED site=display errno=%d step=close", error);
		_exit(1);
	}
}

/* A job of a kind, for then and context (display_spare when no memory is left: it runs here at once). */
static struct display_job *
display_job_new(
	struct kwl_server *server,
	enum display_kind kind,
	kwl_display_fn *then,
	void *context)
{
	struct display_job *job;

	/* A new one, or the spare. */
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		if (display_spare_used) {
			kwl_fatal("KWL FAILED site=display errno=%d step=job", ENOMEM);
			_exit(1);
		}

		/* Takes the spare job, which only one change can hold at a time. */
		display_spare_used = 1U;
		memset(&display_spare, 0, sizeof(display_spare));
		job = &display_spare;
		job->spare = 1U;
	}

	/* Succeeded: its kind and what goes on with its result. */
	job->kind = kind;
	job->server = server;
	job->then = then;
	job->context = context;
	return job;
}

/* Frees a job (the spare is only given back). */
static void
display_job_free(
	struct display_job *job)
{
	/* The spare is free again. */
	if (job->spare) {
		display_spare_used = 0U;
		return;
	}

	/* Succeeded: freed. */
	free(job);
}

/* Gives a job to the display thread, or runs it and takes its result here (the steps run here, or no thread, or the spare). */
static void
display_submit(
	struct kwl_server *server,
	struct display_job *job)
{
	struct kwl_compose *compose;

	/* Here, at once, after the thread's jobs given before (a release of the game mode's claim first). */
	compose = server->compose;
	if (!compose->display_started || compose->display_inline || job->spare) {
		kwl_lane_wait(&compose->display_lane);
		display_run(&job->lane);
		display_done(server, &job->lane);
		return;
	}

	/* Succeeded: the thread has it. */
	kwl_lane_submit(&compose->display_lane, &job->lane, &display_ops, 0U);
}

/* Runs a job on the display thread (or here): it writes the job only. */
static void
display_run(
	struct kwl_lane_job *lane)
{
	struct display_job *job;
	struct kwl_compose *compose;
	unsigned index;

	/* By its kind. */
	job = (struct display_job *)lane;
	compose = job->server->compose;
	switch (job->kind) {
	case DISPLAY_CLOSE:
		/* The cursor thread stopped and the plane's objects destroyed (which waits for the device), else the device idle. */
		job->error = 0;
		if (job->parts.thread != NULL)
			job->error = kwl_cursor_plane_stop(compose, job->parts.thread);
		else
			kwl_device_wait_idle(compose);
		if (job->error != 0)
			break;

		/* The heads' swapchains and surfaces, the output's, then the displays given back. */
		kwl_heads_rest_destroy(job->server, job->heads, job->head_count);
		vkdemo_display_close(compose->instance, compose->device, &job->output);
		kwl_os_display_release(job->server, compose->physical, job->display);
		break;
	case DISPLAY_OPEN:
		display_open_run(job);
		break;
	case DISPLAY_SCANOUT_OPEN:
		display_game_open_run(job);
		break;
	case DISPLAY_SCANOUT_CLOSE:
		/* The present running returns first (the display reads its image until then), then the swapchain goes. */
		kwl_scanout_present_wait();
		display_game_close(job->server, job->game);
		job->error = 0;
		break;
	case DISPLAY_HEADS:
		/* No present of the heads let go may still run, then they go; then the new ones. */
		if (job->rest_count > 0U) {
			kwl_device_wait_idle(compose);
			kwl_heads_rest_destroy(job->server, job->rests, job->rest_count);
		}
		for (index = 0U; index < job->open_count; index++)
			kwl_heads_make(job->server, &job->opens[index]);
		job->error = 0;
		break;
	case DISPLAY_ENUMERATE:
		kwl_display_list_read(job->server, job->list);
		job->error = 0;
		break;
	case DISPLAY_LIGHT_OPEN:
		/* Opened, and its brightness read. */
		job->error = kl_backend_backlight_open(&display_light);
		if (job->error != 0) {
			display_light = NULL;
			break;
		}
		(void)kl_backend_backlight_get(display_light, &job->percent);
		break;
	case DISPLAY_LIGHT_SET:
		/* Set, and read again (a failure leaves what the driver has). */
		if (display_light == NULL) {
			job->error = ENODEV;
			break;
		}
		job->error = kl_backend_backlight_set(display_light, job->percent);
		(void)kl_backend_backlight_get(display_light, &job->percent);
		break;
	default:
		job->error = EINVAL;
		break;
	}

	/* Succeeded: the job has run (its drop at the end needs no run of its own). */
	job->ran = 1U;
}

/* Takes a job's result on the event loop's thread, and goes on with the change. */
static void
display_done(
	void *context,
	struct kwl_lane_job *lane)
{
	struct kwl_server *server;
	struct kwl_compose *compose;

	/* What goes on with it, kept before the job goes; the change's next step runs as a step. */
	server = context;
	compose = server->compose;
	if (compose == NULL) {
		display_done_step(server, (struct display_job *)lane);
		return;
	}
	compose->display_in_step++;
	display_done_step(server, (struct display_job *)lane);
	compose->display_in_step--;
}

/* Takes a job's result (display_done), a step of its change. */
static void
display_done_step(
	struct kwl_server *server,
	struct display_job *job)
{
	struct kwl_cursor_thread *cursor;
	struct kwl_compose *compose;
	kwl_display_fn *then;
	VkExtent2D plane_extent;
	uint32_t plane_index;
	unsigned scene_supported;
	void *then_context;
	uint64_t started;
	unsigned index;
	int error;

	/* What goes on with it, kept before the job goes. */
	compose = server->compose;
	then = job->then;
	then_context = job->context;
	error = job->error;

	/* By its kind. */
	switch (job->kind) {
	case DISPLAY_CLOSE:
		/* The plane's copies and record go (a thread whose copies did not finish ends the compositor). */
		if (job->parts.thread != NULL)
			kwl_cursor_plane_detached(server, &job->parts, error);
		for (index = 0U; index < job->head_count; index++)
			printf("KWL OUTPUT head closed name=%s\n", job->heads[index].name);
		display_job_free(job);
		printf("KWL OUTPUT closed\n");
		then(server, 0, then_context);
		return;
	case DISPLAY_OPEN:
		/* A failure keeps nothing of the output (the display thread destroyed what it made). */
		if (error != 0) {
			display_job_free(job);
			memset(&compose->output, 0, sizeof(compose->output));
			compose->output_prepared = 0;
			then(server, error, then_context);
			return;
		}

		/* The swapchain is the output's, and the main thread's part takes it in. */
		compose->output = job->output;
		compose->readback = (unsigned)job->readback;
		scene_supported = job->scene_supported;
		cursor = job->cursor;
		plane_extent = job->plane_extent;
		plane_index = job->plane_index;
		display_job_free(job);
		started = kwl_milliseconds();
		error = kwl_compose_output_take(server, scene_supported, cursor, plane_extent, plane_index);
		printf("KWL DISPLAY step=take ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));
		then(server, error, then_context);
		return;
	case DISPLAY_LIGHT_OPEN:
		/* Open (and read), or not there; the clients hear of the light. */
		if (error != 0) {
			server->backlight_state = KWL_BACKLIGHT_NONE;
			server->backlight_error = error;
		} else {
			server->backlight_state = KWL_BACKLIGHT_OPEN;
			server->backlight_percent = job->percent;
		}
		display_job_free(job);
		kwl_displays_tell(server);
		return;
	case DISPLAY_LIGHT_SET:
		/* What the driver has now; a failure is logged. */
		if (error != 0)
			printf("KWL DISPLAYS light error=%d percent=%u\n", error, job->percent);
		server->backlight_percent = job->percent;
		display_job_free(job);
		return;
	default:
		display_job_free(job);
		then(server, error, then_context);
		return;
	}
}

/*
 * Asks the display thread to open the built-in panel's backlight and read
 * it, once (at the display thread's start, or at the first need); the
 * clients hear of the light when it is open.
 */
void
kwl_backlight_start(
	struct kwl_server *server)
{
	struct display_job *job;

	/* Once. */
	if (server->backlight_state != KWL_BACKLIGHT_UNKNOWN)
		return;

	/* The job. */
	server->backlight_state = KWL_BACKLIGHT_OPENING;
	job = display_job_new(server, DISPLAY_LIGHT_OPEN, NULL, NULL);
	display_light_submit(server, job, 0U);
}

/*
 * Gives the backlight's brightness as the display thread last read it (or
 * as last asked of it).  Returns 0, EAGAIN while it is being opened, or
 * the error of its open (no backlight).
 */
int
kwl_backlight_get(
	struct kwl_server *server,
	unsigned *percent)
{
	/* Opened first. */
	kwl_backlight_start(server);
	if (server->backlight_state == KWL_BACKLIGHT_NONE)
		return server->backlight_error;
	if (server->backlight_state != KWL_BACKLIGHT_OPEN)
		return EAGAIN;

	/* Succeeded: the copy. */
	*percent = server->backlight_percent;
	return 0;
}

/*
 * Asks the display thread to set the backlight (only the latest asking not
 * started yet is run); the copy follows at once, and the thread's reading
 * after.  Returns 0, or the error of its open (no backlight).
 */
int
kwl_backlight_set(
	struct kwl_server *server,
	unsigned percent)
{
	struct display_job *job;

	/* Opened first (the setting follows the open in the thread's order). */
	kwl_backlight_start(server);
	if (server->backlight_state == KWL_BACKLIGHT_NONE)
		return server->backlight_error;

	/* The copy, then the job. */
	server->backlight_percent = percent;
	job = display_job_new(server, DISPLAY_LIGHT_SET, NULL, NULL);
	job->percent = percent;
	display_light_submit(server, job, DISPLAY_LIGHT_KEY);
	return 0;
}

/*
 * Closes the backlight at the end, once the display thread has stopped.
 */
void
kwl_backlight_close(
	struct kwl_server *server)
{
	/* Closed. */
	kl_backend_backlight_close(display_light);
	display_light = NULL;
	server->backlight_state = KWL_BACKLIGHT_UNKNOWN;
}

/* Gives the display thread a backlight's job with a key (0 for none), or runs it here (no thread, or the end). */
static void
display_light_submit(
	struct kwl_server *server,
	struct display_job *job,
	uint64_t key)
{
	/* Here, at once. */
	if (server->compose == NULL || !server->compose->display_started || job->spare) {
		display_run(&job->lane);
		display_done(server, &job->lane);
		return;
	}

	/* Succeeded: the thread has it. */
	kwl_lane_submit(&server->compose->display_lane, &job->lane, &display_ops, key);
}

/*
 * Frees a job not run or not taken (the thread stopped): its change never
 * goes on, at the end.  The game mode's release runs here if it had not
 * run, and its then is called; a claim made and not taken goes back.
 */
static void
display_drop(
	struct kwl_lane_job *lane)
{
	struct display_job *job;

	/* The game mode's release always happens, and its caller hears of it. */
	job = (struct display_job *)lane;
	if (job->kind == DISPLAY_SCANOUT_CLOSE) {
		if (!job->ran)
			display_run(&job->lane);
		job->then(job->server, 0, job->context);
	}

	/* A game chain made that nobody takes in. */
	if (job->kind == DISPLAY_SCANOUT_OPEN && job->ran && job->error == 0)
		display_game_close(job->server, job->game);

	/* Freed. */
	display_job_free(job);
}

/*
 * Takes the display and makes the output's FIFO swapchain on the display
 * thread, in the format the pipelines were made for; its images are a
 * copy's source when the display allows it, for the mirror's heads and a
 * test image's capture.  Then the cursor plane's objects, when the output
 * has one.  A failure destroys what was made and gives the display back.
 */
static void
display_open_run(
	struct display_job *job)
{
	struct kwl_compose *compose;
	uint64_t started;
	VkResult result;

	/* Gives Vulkan the display permission before creating its swapchain. */
	compose = job->server->compose;
	result = kwl_os_display_acquire(job->server, compose->physical, job->display);
	if (result != VK_SUCCESS) {
		printf("KWL VULKAN_ERROR operation=display-acquire result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &job->output);
		job->error = EIO;
		return;
	}

	/* Both transfer directions first, for the scene's copies. */
	started = kwl_milliseconds();
	job->readback = 1;
	job->scene_supported = 1U;
	result = vkdemo_display_create_swapchain_usage(
		compose->physical,
		compose->device,
		compose->family,
		&job->output,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	/* A surface without both transfer directions keeps the existing capture-only path. */
	if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		job->scene_supported = 0U;
		result = vkdemo_display_create_swapchain(compose->physical, compose->device, compose->family, &job->output, job->readback);
	}

	/* A surface without readback retains the ordinary colour-only swapchain. */
	if (result == VK_ERROR_FORMAT_NOT_SUPPORTED && job->readback) {
		/* A display whose images cannot be read back still shows the desktop, without the capture. */
		printf("KWL SHOT unsupported: the swapchain images cannot be a copy's source\n");
		job->readback = 0;
		result = vkdemo_display_create_swapchain(compose->physical, compose->device, compose->family, &job->output, 0);
	}

	/* Refuses a swapchain the compositor cannot use: it failed, has too many images or has another format. */
	if (result != VK_SUCCESS ||
	    job->output.image_count > KWL_SWAPCHAIN_MAX ||
	    job->output.format != compose->format) {
		printf("KWL VULKAN_ERROR operation=swapchain result=%d images=%u format=%d\n", (int)result, job->output.image_count, (int)job->output.format);
		vkdemo_display_close(compose->instance, compose->device, &job->output);
		kwl_os_display_release(job->server, compose->physical, job->display);
		job->error = EIO;
		return;
	}

	/* Reports how long the swapchain took. */
	printf("KWL STARTUP step=output-swapchain ms=%llu\n", (unsigned long long)(kwl_milliseconds() - started));

	/* The cursor plane's objects, when the output has such a plane (it borrows the output's claim). */
	if (job->plane)
		job->cursor = kwl_cursor_plane_make(compose, &job->output, &job->plane_extent, &job->plane_index);

	/* Succeeded: the swapchain for the main thread to take in. */
	job->error = 0;
}

/*
 * Makes the game mode's display swapchain on the display thread: the
 * display plane's surface at the display's mode, the display taken, then a
 * FIFO swapchain in the format the main thread chose, whose images allow
 * drawing, copies and sampling (a client draws into them; window mode
 * composes them after the lend) and may go out (an external-memory record
 * in its creation), with enough images for the client's count beside the
 * two the display holds (the one shown, the one queued), and each image's
 * fd (vkGetMemoryFdKHR naming the image).  A failure destroys what was
 * made and gives the display back; ENOTSUP when the images cannot go out.
 */
static void
display_game_open_run(
	struct display_job *job)
{
	VkExternalMemoryImageCreateInfo external;
	VkSwapchainCreateInfoKHR create;
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[8];
	VkMemoryDedicatedAllocateInfo dedicated;
	VkMemoryGetFdInfoKHR get;
	VkFenceCreateInfo fence;
	PFN_vkGetMemoryFdKHR get_memory_fd;
	struct kwl_game_chain *chain;
	struct kwl_compose *compose;
	VkFormat format;
	VkResult result;
	uint32_t count;
	uint32_t index;
	unsigned found;

	/* Nothing made yet. */
	compose = job->server->compose;
	chain = job->game;
	format = chain->output.format;
	job->error = EIO;
	for (index = 0U; index < KWL_GAME_IMAGES; index++)
		chain->fds[index] = -1;

	/* The display plane's surface at the display's mode. */
	result = vkdemo_display_open_on(compose->instance, compose->physical, compose->display, compose->mode_width, compose->mode_height, &chain->output);
	chain->output.format = format;
	if (result != VK_SUCCESS) {
		printf("KWL SCANOUT game chain step=surface result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &chain->output);
		return;
	}

	/* The display's permission before the swapchain. */
	result = kwl_os_display_acquire(job->server, compose->physical, compose->display);
	if (result != VK_SUCCESS) {
		printf("KWL SCANOUT game chain step=display-acquire result=%d\n", (int)result);
		vkdemo_display_close(compose->instance, compose->device, &chain->output);
		return;
	}

	/* The surface shows the format chosen, in the sRGB colour space. */
	count = sizeof(formats) / sizeof(formats[0]);
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(compose->physical, chain->output.surface, &count, formats);
	found = 0U;
	for (index = 0U; result >= VK_SUCCESS && index < count; index++) {
		if (formats[index].format == format && formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			found = 1U;
	}
	if (!found) {
		printf("KWL SCANOUT game chain step=format format=%d\n", (int)format);
		job->error = ENOTSUP;
		goto failed;
	}

	/* The uses the surface allows of drawing, copies and sampling; drawing and sampling are needed. */
	result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(compose->physical, chain->output.surface, &capabilities);
	if (result != VK_SUCCESS)
		goto failed;
	chain->usage = capabilities.supportedUsageFlags &
	    (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if ((chain->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0U ||
	    (chain->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0U) {
		printf("KWL SCANOUT game chain step=usage usage=0x%x\n", (unsigned)capabilities.supportedUsageFlags);
		job->error = ENOTSUP;
		goto failed;
	}

	/* The client's count beside the two the display holds, within the surface's bounds and the game mode's. */
	count = chain->wanted + 2U;
	if (count < capabilities.minImageCount)
		count = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && count > capabilities.maxImageCount)
		count = capabilities.maxImageCount;
	if (count > KWL_GAME_IMAGES)
		count = KWL_GAME_IMAGES;

	/* The images may go out as opaque fds. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	/* A FIFO swapchain of the display's mode, opaque and unturned. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.pNext = &external;
	create.surface = chain->output.surface;
	create.minImageCount = count;
	create.imageFormat = format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent.width = compose->mode_width;
	create.imageExtent.height = compose->mode_height;
	create.imageArrayLayers = 1U;
	create.imageUsage = chain->usage;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	result = vkCreateSwapchainKHR(compose->device, &create, NULL, &chain->output.swapchain);
	if (result != VK_SUCCESS) {
		printf("KWL SCANOUT game chain step=swapchain result=%d images=%u\n", (int)result, count);
		goto failed;
	}

	/* The images, at most as many as the game mode lends. */
	count = 0U;
	result = vkGetSwapchainImagesKHR(compose->device, chain->output.swapchain, &count, NULL);
	if (result != VK_SUCCESS || count == 0U || count > KWL_GAME_IMAGES)
		goto failed;
	chain->output.images = calloc(count, sizeof(*chain->output.images));
	if (chain->output.images == NULL)
		goto failed;
	result = vkGetSwapchainImagesKHR(compose->device, chain->output.swapchain, &count, chain->output.images);
	if (result != VK_SUCCESS)
		goto failed;
	chain->output.image_count = count;

	/* Each image's fd: the request names the swapchain image, which has no memory handle of its own. */
	get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(compose->device, "vkGetMemoryFdKHR");
	if (get_memory_fd == NULL) {
		job->error = ENOTSUP;
		goto failed;
	}
	for (index = 0U; index < count; index++) {
		memset(&dedicated, 0, sizeof(dedicated));
		dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedicated.image = chain->output.images[index];
		memset(&get, 0, sizeof(get));
		get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
		get.pNext = &dedicated;
		get.memory = VK_NULL_HANDLE;
		get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		result = get_memory_fd(compose->device, &get, &chain->fds[index]);
		if (result != VK_SUCCESS) {
			printf("KWL SCANOUT game chain step=export result=%d image=%u\n", (int)result, index);
			chain->fds[index] = -1;
			job->error = ENOTSUP;
			goto failed;
		}
	}

	/* The present thread's fence, which tells when an acquired image is the client's. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(compose->device, &fence, NULL, &chain->acquired);
	if (result != VK_SUCCESS) {
		chain->acquired = VK_NULL_HANDLE;
		goto failed;
	}

	/* Succeeded: the game chain is the main thread's to lend. */
	job->error = 0;
	return;

failed:
	/* What was made goes, and the display goes back. */
	display_game_close(job->server, chain);
}

/*
 * Destroys the game mode's display swapchain (no present of it runs):
 * the fds not sent, the fence, the swapchain and its surface, and the
 * display given back.  The memory of an image a client still holds stays
 * its own until it lets go.
 */
static void
display_game_close(
	struct kwl_server *server,
	struct kwl_game_chain *chain)
{
	struct kwl_compose *compose;
	unsigned taken;
	uint32_t index;

	/* The fds not sent yet. */
	compose = server->compose;
	for (index = 0U; index < KWL_GAME_IMAGES; index++) {
		if (chain->fds[index] >= 0)
			close(chain->fds[index]);
		chain->fds[index] = -1;
	}

	/* The fence. */
	if (chain->acquired != VK_NULL_HANDLE)
		vkDestroyFence(compose->device, chain->acquired, NULL);
	chain->acquired = VK_NULL_HANDLE;

	/* The swapchain and its surface, then the display given back when a swapchain had taken it. */
	taken = 0U;
	if (chain->output.surface != VK_NULL_HANDLE)
		taken = 1U;
	vkdemo_display_close(compose->instance, compose->device, &chain->output);
	if (taken)
		kwl_os_display_release(server, compose->physical, compose->display);
}

/* A change made of steps, for then and context; NULL without memory. */
static struct display_change *
display_change_new(
	kwl_display_fn *then,
	void *context)
{
	struct display_change *change;

	/* The record. */
	change = calloc(1, sizeof(*change));
	if (change == NULL)
		return NULL;
	change->then = then;
	change->context = context;
	return change;
}

/* Ends a change made of steps and tells its result. */
static void
display_change_end(
	struct kwl_server *server,
	struct display_change *change,
	int error)
{
	kwl_display_fn *then;
	void *context;

	/* The record goes first. */
	then = change->then;
	context = change->context;
	free(change);

	/* The change ends, then its result. */
	kwl_display_end(server);
	then(server, error, context);
}

/* Window mode's open, parked: the output opened. */
static void
display_open_parked(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The open. */
	(void)error;
	kwl_display_open(server, display_open_opened, context);
}

/* Window mode's open, opened: the change ends, and the desktop follows what the output shows. */
static void
display_open_opened(
	struct kwl_server *server,
	int error,
	void *context)
{
	struct display_change *change;
	kwl_display_fn *then;
	void *then_context;

	/* A failure ends the change and is told. */
	change = context;
	if (error != 0) {
		display_change_end(server, change, error);
		return;
	}

	/*
	 * The change ends, then the desktop follows what the output shows now
	 * against what it showed when it last opened: another
	 * logical size (a move to a display of another size), another angle.
	 */
	then = change->then;
	then_context = change->context;
	free(change);
	kwl_display_end(server);
	(void)kwl_output_rotated(server);
	then(server, 0, then_context);
}

/* An output's close as a change of its own, parked: the output closed. */
static void
display_close_parked(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The close. */
	(void)error;
	kwl_display_close(server, display_close_closed, context);
}

/* An output's close as a change of its own, closed: the change ends. */
static void
display_close_closed(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The end, and the result. */
	display_change_end(server, context, error);
}

/* Keeps the result of a change whose steps ran here. */
static void
display_sync_then(
	struct kwl_server *server,
	int error,
	void *context)
{
	/* The result for the caller. */
	(void)server;
	*(int *)context = error;
}
