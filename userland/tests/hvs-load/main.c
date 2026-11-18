/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The HVS overlay load probe (ws214-p003b): claims a display that has
 * overlay planes (GPU_DISPLAY_HAS_OVERLAYS), with no compositor running,
 * shows a primary frame and then one scene of overlays after another (none,
 * one or two full-size, scaled up, reduced across, reduced down by two and
 * three, reduced on both axes), each for a while, committing the scene
 * again every refresh (moving the first overlay when it has room), and
 * releases the display.  The kernel's flip report (v3d.perf=1) counts the
 * underruns of each two-second window; the lines below give the scenes'
 * times so the windows can be matched:
 *
 *   HVS-LOAD display id=ID width=W height=H refresh=MHZ overlays=K
 *   HVS-LOAD scene=NAME start_ms=T result=applied|nospace|error=E
 *   HVS-LOAD scene=NAME end_ms=T commits=N dropped=D
 *   HVS-LOAD done error=E
 *
 *   hvs-load [--node=/dev/gpuN] [--seconds=10]
 *       (without --node, the first of /dev/gpu0 to /dev/gpu7 whose first
 *       display has overlays)
 */

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* How many GPU nodes the search for a display with overlays tries, and how long each scene lasts. */
#define LOAD_NODES		8U
#define LOAD_SECONDS		10U

/* The images' colours, BGRA with premultiplied alpha: a grey primary, a half-transparent red and a green overlay. */
#define LOAD_PRIMARY_COLOUR	0xff404040U
#define LOAD_FIRST_COLOUR	0x80800000U
#define LOAD_SECOND_COLOUR	0xc000c000U

/* One overlay's place in a scene: shown, its source size and its shown size (at the top-left unless moved). */
struct load_overlay {
	int shown;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t dst_width;
	uint32_t dst_height;
};

/* One scene: its name and both overlays. */
struct load_scene {
	const char *name;
	struct load_overlay first;
	struct load_overlay second;
};

/* The display, the lease and the images every scene uses. */
struct load_state {
	int fd;
	struct gpu_display_info info;
	uint64_t lease;
	uint64_t images[3];
	uint64_t serials[3];
	uint64_t sequence;
};

static int load_open(struct load_state *state, const char *node);
static int load_image(struct load_state *state, uint32_t colour, uint64_t *handle);
static int load_scene(struct load_state *state, const struct load_scene *scene, unsigned seconds);
static void load_plane(struct load_state *state, struct gpu_display_plane_state *plane, uint32_t index, const struct load_overlay *overlay, int image, uint32_t x);
static int load_commit(struct load_state *state, struct gpu_display_commit *commit);
static uint32_t load_size(uint32_t full, uint32_t numerator, uint32_t denominator);
static void load_overlay_set(struct load_overlay *overlay, uint32_t src_width, uint32_t src_height, uint32_t dst_width, uint32_t dst_height);
static uint64_t load_now_ms(void);

/*
 * Claims the display, shows every scene in turn and releases it.
 */
int
main(
	int argc,
	char **argv)
{
	struct load_state state;
	struct load_scene scenes[9];
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_display_commit commit;
	const char *node;
	char path[32];
	uint32_t width;
	uint32_t height;
	uint32_t narrow;
	unsigned seconds;
	unsigned index;
	int status;
	int error;

	/* The node and the length of each scene. */
	node = NULL;
	seconds = LOAD_SECONDS;
	for (index = 1; index < (unsigned)argc; index++) {
		/* --node=PATH or --seconds=N. */
		if (strncmp(argv[index], "--node=", 7) == 0) {
			node = argv[index] + 7;
		} else if (strncmp(argv[index], "--seconds=", 10) == 0) {
			seconds = (unsigned)strtoul(argv[index] + 10, NULL, 10);
		} else {
			fprintf(stderr, "usage: hvs-load [--node=/dev/gpuN] [--seconds=10]\n");
			return 2;
		}
	}

	/* The display with overlays: the node named, or the first node that has one. */
	memset(&state, 0, sizeof(state));
	error = ENODEV;
	if (node != NULL) {
		error = load_open(&state, node);
	} else {
		for (index = 0; index < LOAD_NODES && error != 0; index++) {
			/* Another node, after closing the last one tried. */
			if (state.fd > 0)
				close(state.fd);
			snprintf(path, sizeof(path), "/dev/gpu%u", index);
			error = load_open(&state, path);
		}
	}
	if (error != 0) {
		printf("HVS-LOAD done error=%d\n", error);
		return 1;
	}
	width = state.info.current_width;
	height = state.info.current_height;
	printf("HVS-LOAD display id=%u width=%u height=%u refresh=%u overlays=%u\n",
	    (unsigned)state.info.display_id, (unsigned)width, (unsigned)height,
	    (unsigned)state.info.refresh_millihz, (unsigned)(state.info.plane_count - 1U));

	/* The lease. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = state.info.display_id;
	claim.generation = state.info.generation;
	status = ioctl(state.fd, GPU_DISPLAY_CLAIM, &claim);
	if (status != 0) {
		printf("HVS-LOAD done error=%d\n", errno);
		return 1;
	}
	state.lease = claim.lease;

	/* The three images, each as large as the output. */
	error = load_image(&state, LOAD_PRIMARY_COLOUR, &state.images[0]);
	if (error == 0)
		error = load_image(&state, LOAD_FIRST_COLOUR, &state.images[1]);
	if (error == 0)
		error = load_image(&state, LOAD_SECOND_COLOUR, &state.images[2]);

	/* The primary, waited for. */
	if (error == 0) {
		memset(&commit, 0, sizeof(commit));
		commit.flags = GPU_DISPLAY_COMMIT_WAIT;
		commit.count = 1U;
		load_plane(&state, &commit.planes[0], 0U, NULL, 1, 0U);
		error = load_commit(&state, &commit);
	}

	/*
	 * The scenes.  A vertically scaled overlay reads lines of at most 2048
	 * pixels, so those scenes use a source no wider than that.
	 */
	narrow = width;
	if (narrow > 2048U)
		narrow = 2048U;
	memset(scenes, 0, sizeof(scenes));
	scenes[0].name = "primary";
	scenes[1].name = "one-full";
	load_overlay_set(&scenes[1].first, width, height, width, height);
	scenes[2].name = "two-full";
	scenes[2].first = scenes[1].first;
	scenes[2].second = scenes[1].first;
	scenes[3].name = "up-2x";
	load_overlay_set(&scenes[3].first, narrow / 2U, height / 2U, narrow, height);
	scenes[4].name = "down-x-2";
	load_overlay_set(&scenes[4].first, width, height, width / 2U, height);
	scenes[5].name = "down-y-2";
	load_overlay_set(&scenes[5].first, narrow, height, narrow, height / 2U);
	scenes[6].name = "down-y-3";
	load_overlay_set(&scenes[6].first, narrow, height, narrow, height / 3U);
	scenes[7].name = "down-xy-3/4";
	load_overlay_set(&scenes[7].first, narrow, height, load_size(narrow, 3U, 4U), load_size(height, 3U, 4U));
	scenes[8].name = "full-and-down-y-2";
	scenes[8].first = scenes[1].first;
	scenes[8].second = scenes[5].first;
	for (index = 0; index < 9U && error == 0; index++)
		error = load_scene(&state, &scenes[index], seconds);

	/* The display goes back to the console. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = state.lease;
	status = ioctl(state.fd, GPU_DISPLAY_RELEASE, &release);
	if (status != 0 && error == 0)
		error = errno;
	printf("HVS-LOAD done error=%d\n", error);
	close(state.fd);

	/* Succeeded or not, by the error printed. */
	if (error != 0)
		return 1;
	return 0;
}

/* Opens the node and finds its first display with overlays. */
static int
load_open(
	struct load_state *state,
	const char *node)
{
	int status;

	/* The node, read and write. */
	state->fd = open(node, O_RDWR);
	if (state->fd < 0)
		return errno;

	/* The first display. */
	memset(&state->info, 0, sizeof(state->info));
	state->info.version = GPU_ABI_VERSION;
	state->info.size = sizeof(state->info);
	state->info.index = 0U;
	status = ioctl(state->fd, GPU_DISPLAY_QUERY, &state->info);
	if (status != 0)
		return errno;

	/* It has overlays and a mode. */
	if ((state->info.flags & GPU_DISPLAY_HAS_OVERLAYS) == 0U || state->info.plane_count < 2U)
		return ENOTSUP;
	if (state->info.current_width == 0U || state->info.current_height == 0U)
		return ENOTSUP;

	/* Succeeded: the display is chosen. */
	return 0;
}

/* Makes one shared image as large as the output, filled with a colour. */
static int
load_image(
	struct load_state *state,
	uint32_t colour,
	uint64_t *handle)
{
	struct gpu_blob_create create;
	struct gpu_transfer transfer;
	uint32_t *row;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	int status;

	/* The storage. */
	width = state->info.current_width;
	height = state->info.current_height;
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = (uint64_t)width * height * 4U;
	create.flags = GPU_BLOB_MAPPABLE;
	status = ioctl(state->fd, GPU_BLOB_CREATE, &create);
	if (status != 0)
		return errno;
	*handle = create.handle;

	/* One row of the colour, written row by row. */
	row = malloc((size_t)width * 4U);
	if (row == NULL)
		return ENOMEM;
	for (x = 0U; x < width; x++)
		row[x] = colour;
	for (y = 0U; y < height; y++) {
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = create.handle;
		transfer.offset = (uint64_t)y * width * 4U;
		transfer.address = (uint64_t)(uintptr_t)row;
		transfer.bytes = width * 4U;
		status = ioctl(state->fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0) {
			free(row);
			return errno;
		}
	}

	/* The row is not needed again. */
	free(row);

	/* Succeeded: the image holds the colour. */
	return 0;
}

/*
 * Shows one scene for a while: both overlays set (or hidden) once, then the
 * first one committed again every refresh, moved back and forth by eight
 * pixels when it has room.
 */
static int
load_scene(
	struct load_state *state,
	const struct load_scene *scene,
	unsigned seconds)
{
	struct gpu_display_commit commit;
	struct gpu_display_wait wait;
	uint64_t start;
	uint64_t end;
	unsigned commits;
	unsigned dropped;
	uint32_t x;
	int error;

	/* Both overlays with their images, at the top-left. */
	memset(&commit, 0, sizeof(commit));
	commit.count = 2U;
	load_plane(state, &commit.planes[0], 1U, &scene->first, 1, 0U);
	load_plane(state, &commit.planes[1], 2U, &scene->second, 1, 0U);
	start = load_now_ms();
	error = load_commit(state, &commit);
	if (error == ENOSPC) {
		printf("HVS-LOAD scene=%s start_ms=%llu result=nospace\n", scene->name, (unsigned long long)start);
		return 0;
	}
	if (error != 0) {
		printf("HVS-LOAD scene=%s start_ms=%llu result=error=%d\n", scene->name, (unsigned long long)start, error);
		return error;
	}
	printf("HVS-LOAD scene=%s start_ms=%llu result=applied\n", scene->name, (unsigned long long)start);

	/* Every refresh, the first overlay again (moved when it has room), until the time is up. */
	commits = 0;
	dropped = 0;
	x = 0;
	end = start + (uint64_t)seconds * 1000U;
	while (load_now_ms() < end) {
		/* The next place, eight pixels along and back. */
		if (scene->first.shown && scene->first.dst_width + 8U <= state->info.current_width)
			x = 8U - x;
		memset(&commit, 0, sizeof(commit));
		commit.count = 1U;
		load_plane(state, &commit.planes[0], 1U, &scene->first, 0, x);
		error = load_commit(state, &commit);
		if (error != 0)
			break;
		commits++;
		if (commit.planes[0].result == GPU_DISPLAY_PLANE_DROPPED)
			dropped++;

		/* Paces the commits by the refresh: waits for this one's adoption. */
		memset(&wait, 0, sizeof(wait));
		wait.version = GPU_ABI_VERSION;
		wait.size = sizeof(wait);
		wait.lease = state->lease;
		wait.sequence = commit.sequence;
		wait.timeout_ns = 100000000U;
		(void)ioctl(state->fd, GPU_DISPLAY_WAIT, &wait);
	}
	printf("HVS-LOAD scene=%s end_ms=%llu commits=%u dropped=%u\n",
	    scene->name, (unsigned long long)load_now_ms(), commits, dropped);

	/* Reports a failed commit. */
	if (error != 0)
		return error;

	/* Succeeded: the scene ran its time. */
	return 0;
}

/*
 * Fills one plane's state: the primary over the whole output, or an overlay
 * from its scene (hidden when not shown), naming its image when asked.
 */
static void
load_plane(
	struct load_state *state,
	struct gpu_display_plane_state *plane,
	uint32_t index,
	const struct load_overlay *overlay,
	int image,
	uint32_t x)
{
	/* The plane, its next serial and no fence. */
	state->serials[index]++;
	plane->plane = index;
	plane->serial = state->serials[index];
	plane->wait_fd = -1;

	/* A hidden overlay names nothing. */
	if (overlay != NULL && !overlay->shown)
		return;

	/* Shown, with the image when it is new. */
	plane->flags = GPU_DISPLAY_PLANE_SHOWN;
	if (image) {
		plane->flags |= GPU_DISPLAY_PLANE_IMAGE;
		plane->handle = state->images[index];
		plane->width = state->info.current_width;
		plane->height = state->info.current_height;
		plane->stride = state->info.current_width * 4U;
		plane->format = GPU_PIXEL_BGRA8888;
	}

	/* The primary covers the output. */
	if (overlay == NULL) {
		plane->src_width = state->info.current_width;
		plane->src_height = state->info.current_height;
		plane->dst_width = state->info.current_width;
		plane->dst_height = state->info.current_height;
		return;
	}

	/* An overlay's source at the top-left of its image, shown at x along the top. */
	plane->src_width = overlay->src_width;
	plane->src_height = overlay->src_height;
	plane->dst_x = x;
	plane->dst_width = overlay->dst_width;
	plane->dst_height = overlay->dst_height;
}

/* Sends one commit of the lease; the unused planes have no fence. */
static int
load_commit(
	struct load_state *state,
	struct gpu_display_commit *commit)
{
	uint32_t index;
	int status;

	/* The framing, the lease and no signal. */
	commit->version = GPU_ABI_VERSION;
	commit->size = sizeof(*commit);
	commit->lease = state->lease;
	commit->generation = state->info.generation;
	commit->signal_fd = -1;
	for (index = commit->count; index < 1U + GPU_DISPLAY_OVERLAY_MAX; index++)
		commit->planes[index].wait_fd = -1;
	status = ioctl(state->fd, GPU_DISPLAY_COMMIT, commit);
	if (status != 0)
		return errno;

	/* Succeeded: the planes are recorded. */
	state->sequence = commit->sequence;
	return 0;
}

/* Sets a shown overlay's source and shown sizes. */
static void
load_overlay_set(
	struct load_overlay *overlay,
	uint32_t src_width,
	uint32_t src_height,
	uint32_t dst_width,
	uint32_t dst_height)
{
	/* Shown, at those sizes. */
	overlay->shown = 1;
	overlay->src_width = src_width;
	overlay->src_height = src_height;
	overlay->dst_width = dst_width;
	overlay->dst_height = dst_height;
}

/* Returns a part of a size, at least one. */
static uint32_t
load_size(
	uint32_t full,
	uint32_t numerator,
	uint32_t denominator)
{
	uint32_t part;

	/* The part, never zero. */
	part = (uint32_t)((uint64_t)full * numerator / denominator);
	if (part == 0U)
		part = 1U;
	return part;
}

/* The monotonic time in milliseconds. */
static uint64_t
load_now_ms(
	void)
{
	struct timespec now;

	/* The monotonic clock, which never jumps. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
