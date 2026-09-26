/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Independent GPU import, the choice between window mode and fullscreen mode
 * (WS035 compositing design, D0), and fullscreen mode's direct scanout.
 */

#include "zwl.h"
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int claim_display(struct zwl_server *server);
static void schedule_direct(struct zwl_server *server);
static void adopt_commit(struct zwl_server *server, struct zwl_object *surface);
static void place_window(struct zwl_server *server, struct zwl_object *surface);
static int enter_fullscreen(struct zwl_server *server);
static int enter_window_mode(struct zwl_server *server);
static int present_current(struct zwl_server *server, struct zwl_object *surface);
static void hidden_callbacks(struct zwl_server *server, struct zwl_object *shown);

/*
 * Opens the compositor's independent renderer context and queries its display.
 */
int
zwl_gpu_open(
	struct zwl_server *server)
{
	struct gpu_info information;
	struct gpu_display_mode mode;
	int error;

	/* The compositor obtains its own GPU fd; no producer session fd is accepted. */
	server->gpu = open(server->gpu_path, O_RDWR | O_CLOEXEC);
	if (server->gpu < 0)
		return errno;

	/* Shared images require both typed import and native blob scanout support. */
	memset(&information, 0, sizeof(information));
	information.version = GPU_ABI_VERSION;
	information.size = sizeof(information);
	error = ioctl(server->gpu, GPU_GET_INFO, &information);
	if (error != 0)
		return errno;

	/* Do not fall back to CPU copies when shared display support is absent. */
	if ((information.capabilities & (GPU_CAP_SHARE | GPU_CAP_DISPLAY)) != (GPU_CAP_SHARE | GPU_CAP_DISPLAY))
		return ENOTSUP;

	/* Select the first native display without claiming it during idle service startup. */
	memset(&server->display, 0, sizeof(server->display));
	server->display.version = GPU_ABI_VERSION;
	server->display.size = sizeof(server->display);
	error = ioctl(server->gpu, GPU_DISPLAY_QUERY, &server->display);
	if (error != 0)
		return errno;

	/* Blob support is explicit rather than inferred from ordinary pixel presentation. */
	if ((server->display.flags & (GPU_DISPLAY_CONNECTED | GPU_DISPLAY_BLOB)) != (GPU_DISPLAY_CONNECTED | GPU_DISPLAY_BLOB))
		return ENOTSUP;

	/* Validate the configured fullscreen geometry without changing active scanout. */
	memset(&mode, 0, sizeof(mode));
	mode.version = GPU_ABI_VERSION;
	mode.size = sizeof(mode);
	mode.display_id = server->display.display_id;
	mode.generation = server->display.generation;
	mode.operation = GPU_DISPLAY_MODE_VALIDATE;
	mode.width = server->width;
	mode.height = server->height;
	mode.refresh_millihz = 0;
	error = ioctl(server->gpu, GPU_DISPLAY_MODE, &mode);
	if (error != 0)
		return errno;

	/* The driver resolves the selected extent to its own supported refresh. */
	server->refresh = mode.refresh_millihz;
	if (server->refresh == 0)
		return EINVAL;

	/* The machine log distinguishes this consumer context from producer imports. */
	printf("ZWL GPU fd=%d driver=%s display=%u generation=%llu width=%u height=%u\n", server->gpu, information.driver_name, server->display.display_id, (unsigned long long)server->display.generation, server->width, server->height);

	/* Succeeded: the independent compositor session supports its selected output. */
	return 0;
}

/*
 * Imports a typed capability and compares authoritative metadata with its wire claim.
 */
int
zwl_gpu_import(
	struct zwl_object *buffer,
	int descriptor,
	const struct gpu_image_descriptor *image)
{
	int error;
	int mismatch;

	/* K accepts only fd input and supplies every native identity and metadata field. */
	memset(&buffer->image, 0, sizeof(buffer->image));
	buffer->image.version = GPU_ABI_VERSION;
	buffer->image.size = sizeof(buffer->image);
	buffer->image.fd = descriptor;
	error = ioctl(buffer->client->server->gpu, GPU_RESOURCE_IMPORT, &buffer->image);
	if (error != 0)
		return errno;

	/* Userspace metadata cannot reinterpret a capability's storage or device identity. */
	mismatch = memcmp(&buffer->image.image, image, sizeof(*image));
	if (mismatch != 0)
		return EINVAL;

	/* Only an image the size of the output can be scanned out in fullscreen mode. */
	buffer->scanout = 0;
	if (image->width == buffer->client->server->width &&
	    image->height == buffer->client->server->height &&
	    image->tiling == GPU_IMAGE_LINEAR)
		buffer->scanout = 1;

	/* Import reports the native allocation identity shared across independent contexts. */
	printf("ZWL IMPORT client=%llu buffer=%u gpu_fd=%d resource=%u handle=%llu device=%llu bytes=%llu\n", (unsigned long long)buffer->client->number, buffer->id, buffer->client->server->gpu, buffer->image.resource_id, (unsigned long long)buffer->image.handle, (unsigned long long)image->device_id, (unsigned long long)image->allocation_bytes);

	/* Succeeded: this wl_buffer owns a distinct consumer resource handle. */
	return 0;
}

/*
 * Releases the display lease before withdrawing the previous front-image hold.
 */
int
zwl_unscan(
	struct zwl_server *server)
{
	struct gpu_display_release release;
	struct zwl_client *client;
	struct zwl_object *front;
	int error;

	/* An idle compositor has no scanout allocation to retire. */
	if (server->lease == 0)
		return 0;

	/* Only a successful release proves the hardware no longer borrows this image. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = server->lease;
	error = ioctl(server->gpu, GPU_DISPLAY_RELEASE, &release);
	if (error != 0) {
		error = errno;
		printf("ZWL GPU_ERROR operation=release errno=%d\n", error);
		server->failed = 1;

		/* Closing the owning session lets K retain any uncertain hardware ownership. */
		close(server->gpu);
		server->gpu = -1;
	}

	/* Stop issuing reuse events after an uncertain release; every connection will close. */
	if (server->failed) {
		/* Every producer must stop receiving reuse events after uncertain native retirement. */
		for (client = server->clients; client != NULL; client = client->next)
			client->fatal = 1;
	}

	/* The hardware lease or the entire owning fd has now been withdrawn. */
	server->lease = 0;
	front = server->front;
	server->front = NULL;
	server->front_surface = NULL;
	zwl_buffer_put(front);

	/* A display without a front surface gives no client input focus. */
	zwl_seat_focus(server);

	/* Reports a hardware release failure without announcing safe producer reuse. */
	if (error != 0)
		return error;

	/* Succeeded: no front image is retained by this display lease. */
	return 0;
}

/*
 * Presents a completed producer image and releases the displaced image afterward.
 */
int
zwl_present(
	struct zwl_object *surface)
{
	struct zwl_server *server;
	struct zwl_object *buffer;
	struct zwl_object *previous;
	struct gpu_display_present present;
	struct gpu_image_descriptor *image;
	uint64_t mark;
	int error;

	/* Null committed attachment unmaps this surface instead of presenting pixels. */
	server = surface->client->server;
	buffer = surface->queued;
	if (buffer == NULL) {
		/* Unmapping another client's hidden surface must not withdraw the current front. */
		if (server->front_surface == surface) {
			/* Native release completes before the previous mapped content loses its hold. */
			error = zwl_unscan(server);
			if (error != 0)
				return error;
		}

		/* No pending content can retain the previous mapped surface image. */
		zwl_buffer_put(surface->current);
		surface->current = NULL;
		surface->ready = 0;
		zwl_callbacks_done(&surface->committed_callbacks);
		return 0;
	}

	/* Display ownership belongs to this consumer open alone. */
	error = claim_display(server);
	if (error != 0)
		return error;

	/* GPU-resident linear storage is sent directly to SET_SCANOUT_BLOB. */
	image = &buffer->image.image;
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = server->lease;
	present.handle = buffer->image.handle;
	present.offset = image->offset;
	present.frame = server->frame + 1U;
	present.width = image->width;
	present.height = image->height;
	present.stride = image->stride;
	present.format = image->format;
	present.refresh_millihz = server->refresh;
	present.flags = GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB;
	present.generation = server->display.generation;
	mark = zwl_cycles();
	error = ioctl(server->gpu, GPU_DISPLAY_PRESENT, &present);
	server->perf.present_cycles += zwl_cycles() - mark;
	server->perf.presents++;
	if (error != 0)
		return errno;

	/* Successful fenced selection is the first point when the previous front may be released. */
	previous = server->front;
	zwl_buffer_get(buffer);
	server->front = buffer;
	server->front_surface = surface;
	server->frame++;
	zwl_buffer_put(previous);

	/* The completed commit replaces surface content independently from global scanout. */
	previous = surface->current;
	surface->current = buffer;
	surface->queued = NULL;
	surface->ready = 0;
	zwl_buffer_put(previous);

	/* Names the presentation when the per-frame lines were asked for. */
	if (server->log_frames)
		printf("ZWL PRESENT client=%llu surface=%u buffer=%u resource=%u frame=%llu sequence=%llu width=%u height=%u flags=%u refresh=%u\n", (unsigned long long)surface->client->number, surface->id, buffer->id, buffer->image.resource_id, (unsigned long long)server->frame, (unsigned long long)present.sequence, image->width, image->height, present.flags, present.refresh_millihz);

	/* Input follows the surface that is now on the display. */
	zwl_seat_focus(server);

	/* Frame callbacks pace FIFO clients but do not release the newly selected front buffer. */
	zwl_callbacks_done(&surface->committed_callbacks);

	/* Succeeded: both the surface and hardware own the completed current image. */
	return 0;
}

/*
 * Reports whether a surface's queued image may be used: each of its acquire
 * fences is done (a later generation, or the same one signaled or failed).
 * Done fences are closed.  Nothing is waited for.
 */
int
zwl_fence_ready(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct gpu_fence_state state;
	unsigned index;
	unsigned kept;
	int error;

	/* A commit without fences is ready at once. */
	if (surface->fence_count == 0)
		return 1;

	/* Each fence still pending is kept; the others are closed. */
	kept = 0;
	for (index = 0; index < surface->fence_count; index++) {
		/* Generation zero reads the fence's current generation and state. */
		memset(&state, 0, sizeof(state));
		state.version = GPU_ABI_VERSION;
		state.size = sizeof(state);
		state.fd = surface->fences[index].fd;
		error = ioctl(server->gpu, GPU_FENCE_QUERY, &state);

		/* Still rendering: the generation, or an earlier one, is pending. */
		if ((error == 0 && state.generation < surface->fences[index].generation) ||
		    (error == 0 &&
		     state.generation == surface->fences[index].generation &&
		     state.state == GPU_FENCE_PENDING)) {
			surface->fences[kept] = surface->fences[index];
			kept++;
			continue;
		}

		/* Done, or a fence that can no longer be read, which is not waited for. */
		close(surface->fences[index].fd);
	}

	/* The pending fences remain. */
	surface->fence_count = kept;

	/* Some image is still being drawn. */
	if (kept != 0) {
		surface->fence_waited = 1;
		return 0;
	}

	/* Succeeded: all done. */
	if (server->log_frames && surface->fence_waited)
		printf("ZWL ACQUIRED surface=%u waited_ms=%llu\n", surface->id, (unsigned long long)(zwl_milliseconds() - surface->fence_ms));
	return 1;
}

/*
 * Applies the commits of this event-loop pass, chooses the mode from the
 * topmost window, and shows the result: a frame in window mode, or the
 * fullscreen window's image in fullscreen mode.
 */
void
zwl_schedule(
	struct zwl_server *server)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	struct zwl_object *top;
	unsigned fullscreen;
	uint64_t now;
	int ready;
	int error;

	/* Without a Vulkan device the compositor shows one surface directly, as before. */
	if (server->compose == NULL) {
		schedule_direct(server);
		return;
	}

	/* The glass look's clock turns over. */
	if (server->glass)
		zwl_glass_tick(server);

	/* Every committed surface takes its new image. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live surface with a commit. */
			if (surface->kind != ZWL_SURFACE ||
			    !surface->ready ||
			    surface->dead)
				continue;

			/* A commit whose image is still being drawn waits for a later pass. */
			ready = zwl_fence_ready(server, surface);
			if (ready)
				adopt_commit(server, surface);
		}
	}

	/* Fullscreen mode when the topmost window is fullscreen with an image that can be the output. */
	top = zwl_top_window(server);
	fullscreen = 0;
	if (top != NULL &&
	    top->fullscreen &&
	    top->current != NULL &&
	    top->current->scanout)
		fullscreen = 1;

	/* The mode, switched when it changes. */
	if (fullscreen && server->windowed)
		error = enter_fullscreen(server);
	else if (!fullscreen && !server->windowed)
		error = enter_window_mode(server);
	else
		error = 0;
	if (error != 0) {
		/* A switch waiting for the frame in flight is tried again on a later pass. */
		if (error == EAGAIN)
			return;
		printf("ZWL MODE_ERROR errno=%d\n", error);
		server->failed = 1;
		return;
	}

	/* Fullscreen mode shows the window's newest image directly (hidden wl_shm windows are still copied). */
	if (!server->windowed) {
		(void)zwl_shm_upload(server);
		if (top->fresh || server->front_surface != top) {
			error = present_current(server, top);
			if (error != 0) {
				printf("ZWL GPU_ERROR operation=present errno=%d\n", error);
				(void)zwl_error(top->client, top->id, "GPU presentation failed");
			}
		}

		/* The windows under it get their frame callbacks. */
		hidden_callbacks(server, top);
		return;
	}

	/* Input goes to the topmost window. */
	server->front_surface = top;
	zwl_seat_focus(server);

	/* wl_shm images are copied, and their buffers released, while no frame is in flight. */
	(void)zwl_shm_upload(server);

	/* The windows the last frame told get a moment to commit (frame pacing). */
	now = zwl_milliseconds();
	if (server->awaiting != 0 && now - server->frame_done_ms < server->frame_wait_ms)
		return;

	/* Window mode draws when something changed and no frame is in flight. */
	if (server->dirty) {
		error = zwl_compose_draw(server);
		if (error != 0)
			server->failed = 1;
	}
}

/*
 * Ends the frame in flight when its fence fd became readable.
 */
void
zwl_frame_done(
	struct zwl_server *server)
{
	int error;

	/* The frame's held buffers and callbacks are released. */
	error = zwl_compose_complete(server);
	if (error != 0)
		server->failed = 1;
}

/*
 * Returns the topmost mapped window of a live client, or NULL.
 */
struct zwl_object *
zwl_top_window(
	struct zwl_server *server)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	struct zwl_object *top;

	/* The highest map order wins. */
	top = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != ZWL_SURFACE || surface->dead || !surface->mapped)
				continue;
			if (top == NULL || surface->map_order > top->map_order)
				top = surface;
		}
	}

	/* Succeeded: the window on top, if any. */
	return top;
}

/* Presents queued commits fairly after the current batch of client requests is decoded (no Vulkan). */
static void
schedule_direct(
	struct zwl_server *server)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	struct zwl_object *chosen;
	int ready;
	int error;

	/* A later same-surface commit has already replaced any unpresented mailbox image. */
	chosen = NULL;
	for (client = server->clients; client != NULL; client = client->next) {
		/* A failed connection cannot submit new display ownership. */
		if (client->fatal)
			continue;

		/* Choose the oldest remaining surface commit across independent clients. */
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			/* Only a live surface with committed content participates in scheduling. */
			if (surface->kind != ZWL_SURFACE ||
			    !surface->ready ||
			    surface->dead)
				continue;

			/* A commit whose image is still being drawn waits for a later pass. */
			ready = zwl_fence_ready(server, surface);
			if (!ready)
				continue;

			/* A single presentation per event-loop pass bounds scheduling latency. */
			if (chosen == NULL || surface->commit_order < chosen->commit_order)
				chosen = surface;
		}
	}

	/* An idle event-loop pass performs no GPU operation. */
	if (chosen == NULL)
		return;

	/* Failed presentation leaves front ownership intact until disconnect cleanup. */
	error = zwl_present(chosen);
	if (error != 0) {
		printf("ZWL GPU_ERROR operation=present errno=%d\n", error);
		(void)zwl_error(chosen->client, chosen->id, "GPU presentation failed");
	}

	/* Succeeded: the selected commit was presented or its client was marked for cleanup. */
	return;
}

/*
 * Makes a surface's committed image its current one: a first image maps the
 * window, a null one unmaps it.  The image it replaces is released once no
 * frame holds it.
 */
static void
adopt_commit(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct zwl_object *previous;

	/* The queued image becomes current; a wl_shm image is copied before the next frame. */
	previous = surface->current;
	surface->current = surface->queued;
	surface->queued = NULL;
	surface->ready = 0;
	surface->fresh = 1;
	server->dirty = 1;
	if (surface->current != NULL && surface->current->shm != NULL)
		surface->shm_upload = 1;

	/* An awaited window has committed. */
	if (surface->awaited) {
		surface->awaited = 0;
		server->awaiting--;
	}

	/* A cursor, or a surface with no role, is not a window. */
	if (surface->cursor_role || surface->role == NULL) {
		zwl_buffer_put(previous);
		return;
	}

	/* A first image maps the window on top; a null one unmaps it. */
	if (surface->current != NULL && !surface->mapped) {
		surface->mapped = 1;
		server->map_order++;
		surface->map_order = server->map_order;
		place_window(server, surface);
		printf("ZWL MAP client=%llu surface=%u x=%d y=%d\n", (unsigned long long)surface->client->number, surface->id, surface->x, surface->y);
		zwl_glass_mapped(server, surface);
	} else if (surface->current == NULL && surface->mapped) {
		surface->mapped = 0;
		zwl_callbacks_done(&surface->committed_callbacks);
		printf("ZWL UNMAP client=%llu surface=%u\n", (unsigned long long)surface->client->number, surface->id);
	}

	/* The replaced image. */
	zwl_buffer_put(previous);
}

/*
 * Places a new window: a fullscreen one at the origin, others centred and
 * cascaded by ZWL_CASCADE_STEP (design D6).
 */
static void
place_window(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	int32_t step;
	int32_t width;
	int32_t height;
	uint32_t buffer_width;
	uint32_t buffer_height;

	/* A fullscreen window covers the output. */
	if (surface->fullscreen) {
		surface->x = 0;
		surface->y = 0;
		return;
	}

	/* Centred, then moved down and right by the number of windows placed so far (in a cycle of eight). */
	width = (int32_t)server->width;
	height = (int32_t)server->height;
	if (surface->current != NULL) {
		zwl_buffer_size(surface->current, &buffer_width, &buffer_height);
		width = (int32_t)buffer_width;
		height = (int32_t)buffer_height;
	}

	/* The cascade step of this window. */
	step = ZWL_CASCADE_STEP * (int32_t)(server->windows % 8U);
	server->windows++;

	/* The glass look keeps the space of the system bar and a title bar free. */
	if (server->glass) {
		zwl_glass_place(server, surface, width, height, step);
		return;
	}

	/* Centred on the output. */
	surface->x = ((int32_t)server->width - width) / 2 + step;
	surface->y = ((int32_t)server->height - height) / 2 + step;
	if (surface->x < 0)
		surface->x = 0;
	if (surface->y < 0)
		surface->y = 0;
}

/*
 * Leaves window mode: once no frame is in flight, the swapchain and surface
 * are destroyed, which gives the display back.  Returns EAGAIN while a
 * frame is still in flight.
 */
static int
enter_fullscreen(
	struct zwl_server *server)
{
	uint64_t start;

	/* The frame in flight finishes first. */
	if (server->frame_fd >= 0)
		return EAGAIN;

	/* The output goes; the display is then claimed by the first present. */
	start = zwl_milliseconds();
	zwl_compose_output_close(server);
	server->windowed = 0;
	server->mode_switch_ms = zwl_milliseconds() - start;
	printf("ZWL MODE fullscreen switch_ms=%llu\n", (unsigned long long)server->mode_switch_ms);

	/* Succeeded. */
	return 0;
}

/*
 * Enters window mode: the display lease of fullscreen mode is released and
 * the swapchain is made again.
 */
static int
enter_window_mode(
	struct zwl_server *server)
{
	uint64_t start;
	int error;

	/* The directly scanned-out image, and the lease, go. */
	start = zwl_milliseconds();
	error = zwl_unscan(server);
	if (error != 0)
		return error;

	/* The swapchain over the display. */
	error = zwl_compose_output_open(server);
	if (error != 0)
		return error;

	/* Succeeded: the next pass draws a frame. */
	server->windowed = 1;
	server->dirty = 1;
	server->mode_switch_ms = zwl_milliseconds() - start;
	printf("ZWL MODE window switch_ms=%llu\n", (unsigned long long)server->mode_switch_ms);
	return 0;
}

/*
 * Presents a surface's current image as the whole output (fullscreen mode):
 * no copy, the client's image is what the display scans out.
 */
static int
present_current(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct gpu_display_present present;
	struct gpu_image_descriptor *image;
	struct zwl_object *buffer;
	struct zwl_object *previous;
	uint64_t mark;
	int error;

	/* The display is this compositor's. */
	error = claim_display(server);
	if (error != 0)
		return error;

	/* The image as it is: linear storage, blob scanout. */
	buffer = surface->current;
	image = &buffer->image.image;
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = server->lease;
	present.handle = buffer->image.handle;
	present.offset = image->offset;
	present.frame = server->frame + 1U;
	present.width = image->width;
	present.height = image->height;
	present.stride = image->stride;
	present.format = image->format;
	present.refresh_millihz = server->refresh;
	present.flags = GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB;
	present.generation = server->display.generation;
	mark = zwl_cycles();
	error = ioctl(server->gpu, GPU_DISPLAY_PRESENT, &present);
	server->perf.present_cycles += zwl_cycles() - mark;
	server->perf.presents++;
	if (error != 0)
		return errno;

	/* The display holds the new image; the one it replaces is released. */
	previous = server->front;
	zwl_buffer_get(buffer);
	server->front = buffer;
	server->front_surface = surface;
	server->frame++;
	surface->fresh = 0;
	zwl_buffer_put(previous);

	/* Names the presentation when the per-frame lines were asked for. */
	if (server->log_frames)
		printf("ZWL PRESENT client=%llu surface=%u buffer=%u resource=%u frame=%llu sequence=%llu width=%u height=%u flags=%u refresh=%u direct=1\n", (unsigned long long)surface->client->number, surface->id, buffer->id, buffer->image.resource_id, (unsigned long long)server->frame, (unsigned long long)present.sequence, image->width, image->height, present.flags, present.refresh_millihz);

	/* Input follows the fullscreen window; its frame callbacks are done. */
	zwl_seat_focus(server);
	zwl_callbacks_done(&surface->committed_callbacks);

	/* Succeeded. */
	return 0;
}

/*
 * Sends the frame callbacks of the windows fullscreen mode hides, so that
 * their clients are not held waiting for a frame that never shows them.
 */
static void
hidden_callbacks(
	struct zwl_server *server,
	struct zwl_object *shown)
{
	struct zwl_client *client;
	struct zwl_object *surface;

	/* Every surface but the one shown. */
	for (client = server->clients; client != NULL; client = client->next) {
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind == ZWL_SURFACE && surface != shown)
				zwl_callbacks_done(&surface->committed_callbacks);
		}
	}
}

/* Claims an idle display lease while preserving its generation and exclusive owner. */
static int
claim_display(
	struct zwl_server *server)
{
	struct gpu_display_claim claim;
	int error;

	/* Successive frames share the compositor's existing reservation. */
	if (server->lease != 0)
		return 0;

	/* Claiming does not transfer authority from any client GPU session. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = server->display.display_id;
	claim.generation = server->display.generation;
	error = ioctl(server->gpu, GPU_DISPLAY_CLAIM, &claim);
	if (error != 0)
		return errno;

	/* The nonzero lease remains owned until explicit release or GPU fd close. */
	server->lease = claim.lease;

	/* Succeeded: only this compositor session may select the display's front image. */
	return 0;
}
