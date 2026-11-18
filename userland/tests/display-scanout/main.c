/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU display scanout probe (ws037-p006): drives, with no compositor
 * running, what a compositor's direct scanout and cursor need of a GPU
 * node's first display.  It reads the node's capabilities, its pairing
 * (GPU_DEVICE_QUERY), the display's topology sequence and its scanout
 * constraints; claims the display; makes a blob in a second open of the
 * node (the client), fills it, exports it, imports it into the first open
 * (the compositor), closes the client and presents the import as it is
 * (GPU_DISPLAY_PRESENT with BLOB); then sets a cursor image, shows it in
 * the middle, moves it along a diagonal, hides it, shows it again and
 * releases the display (which hides it).  The lines the tests read:
 *
 *   DISPLAY-SCANOUT info caps=0xC share=0|1 events=0|1 control=0|1 driver=NAME
 *   DISPLAY-SCANOUT device roles=0xR companion=ID error=E
 *   DISPLAY-SCANOUT display id=ID generation=G flags=0xF cursor=0|1 mode=WxH@MHZ
 *   DISPLAY-SCANOUT events sequence=S error=E
 *   DISPLAY-SCANOUT constraints flags=0xF formats=0xF stride=A offset=A placement=0xP error=E
 *   DISPLAY-SCANOUT claim lease=L
 *   DISPLAY-SCANOUT shared blob=E write=E export=E import=E present=E sequence=S
 *   DISPLAY-SCANOUT cursor set=E show=E moves=N move=E hide=E again=E
 *   DISPLAY-SCANOUT done error=E
 *
 *   display-scanout [--node=/dev/gpu0] [--hold=SECONDS]
 *       (how long the shared frame and the cursor's stops are held; default 5)
 */

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* The node opened unless one is named, and how long each picture is held. */
#define SCANOUT_NODE		"/dev/gpu0"
#define SCANOUT_HOLD		5U

/*
 * The shared frame's colour, a red-orange: written as a 32-bit word, its
 * bytes in memory are B 0x20, G 0x50, R 0xd0, A 0xff, the BGRA8888 order.
 */
#define SCANOUT_COLOUR		0xffd05020U

/* The cursor image: 32 by 32, a white square with a black border two pixels wide, opaque. */
#define SCANOUT_CURSOR_SIDE	32U
#define SCANOUT_CURSOR_BORDER	2U
#define SCANOUT_CURSOR_WHITE	0xffffffffU
#define SCANOUT_CURSOR_BLACK	0xff000000U

/* The cursor's walk: this many moves of this many pixels, this many milliseconds apart. */
#define SCANOUT_MOVES		100U
#define SCANOUT_MOVE_STEP	4
#define SCANOUT_MOVE_MS		30U

/* The usage an exported image is described with (a colour attachment, as a swapchain's). */
#define SCANOUT_IMAGE_USAGE	0x10U

static int scanout_query(int fd, struct gpu_display_info *info);
static int scanout_shared(int fd, const char *node, const struct gpu_display_info *info, const struct gpu_scanout_constraints *constraints, uint64_t lease, unsigned hold);
static int scanout_cursor(int fd, const struct gpu_display_info *info, uint64_t lease, unsigned hold);
static int scanout_write_rows(int fd, uint64_t handle, uint32_t stride, uint32_t height, const uint32_t *row, uint32_t row_bytes);
static int scanout_cursor_move(int fd, const struct gpu_display_info *info, uint64_t lease, int32_t x, int32_t y);
static int scanout_cursor_hide(int fd, const struct gpu_display_info *info, uint64_t lease);
static void scanout_sleep_ms(unsigned milliseconds);

int
main(
	int argc,
	char **argv)
{
	struct gpu_info gpu;
	struct gpu_device_info device;
	struct gpu_display_info info;
	struct gpu_display_events events;
	struct gpu_scanout_constraints constraints;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	const char *node;
	unsigned hold;
	int index;
	int same;
	int fd;
	int status;
	int error;

	/* The command line. */
	node = SCANOUT_NODE;
	hold = SCANOUT_HOLD;
	for (index = 1; index < argc; index++) {
		same = strncmp(argv[index], "--node=", 7U);
		if (same == 0) {
			node = argv[index] + 7;
			continue;
		}

		/* How long each picture is held. */
		same = strncmp(argv[index], "--hold=", 7U);
		if (same == 0) {
			hold = (unsigned)strtoul(argv[index] + 7, NULL, 10);
			continue;
		}

		/* Anything else. */
		fprintf(stderr, "usage: display-scanout [--node=/dev/gpu0] [--hold=SECONDS]\n");
		return 2;
	}

	/* The compositor's open of the node. */
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("DISPLAY-SCANOUT done error=%d step=open\n", errno);
		return 1;
	}

	/* The node's capabilities. */
	memset(&gpu, 0, sizeof(gpu));
	gpu.version = GPU_ABI_VERSION;
	gpu.size = sizeof(gpu);
	status = ioctl(fd, GPU_GET_INFO, &gpu);
	if (status != 0) {
		printf("DISPLAY-SCANOUT done error=%d step=info\n", errno);
		return 1;
	}

	gpu.driver_name[sizeof(gpu.driver_name) - 1U] = '\0';
	printf("DISPLAY-SCANOUT info caps=0x%x share=%d events=%d control=%d driver=%s\n",
	    gpu.capabilities,
	    (gpu.capabilities & GPU_CAP_SHARE) != 0U,
	    (gpu.capabilities & GPU_CAP_DISPLAY_EVENTS) != 0U,
	    (gpu.capabilities & GPU_CAP_DISPLAY_CONTROL) != 0U,
	    gpu.driver_name);

	/* The node's pairing: its roles and its preferred renderer. */
	memset(&device, 0, sizeof(device));
	device.version = GPU_ABI_VERSION;
	device.size = sizeof(device);
	status = ioctl(fd, GPU_DEVICE_QUERY, &device);
	error = 0;
	if (status != 0)
		error = errno;
	printf("DISPLAY-SCANOUT device roles=0x%x companion=%llu error=%d\n",
	    device.roles,
	    (unsigned long long)device.companion_id,
	    error);

	/* The first display. */
	error = scanout_query(fd, &info);
	if (error != 0) {
		printf("DISPLAY-SCANOUT done error=%d step=query\n", error);
		return 1;
	}

	info.name[sizeof(info.name) - 1U] = '\0';
	printf("DISPLAY-SCANOUT display id=%u generation=%llu flags=0x%x cursor=%d mode=%ux%u@%u\n",
	    info.display_id,
	    (unsigned long long)info.generation,
	    info.flags,
	    (info.flags & GPU_DISPLAY_HAS_CURSOR) != 0U,
	    info.current_width,
	    info.current_height,
	    info.refresh_millihz);

	/* The topology sequence (a query, no acknowledgement). */
	memset(&events, 0, sizeof(events));
	events.version = GPU_ABI_VERSION;
	events.size = sizeof(events);
	status = ioctl(fd, GPU_DISPLAY_EVENTS, &events);
	error = 0;
	if (status != 0)
		error = errno;
	printf("DISPLAY-SCANOUT events sequence=%llu error=%d\n", (unsigned long long)events.sequence, error);

	/* The display's scanout constraints. */
	memset(&constraints, 0, sizeof(constraints));
	constraints.version = GPU_ABI_VERSION;
	constraints.size = sizeof(constraints);
	constraints.display_id = info.display_id;
	constraints.generation = info.generation;
	status = ioctl(fd, GPU_DISPLAY_CONSTRAINTS, &constraints);
	error = 0;
	if (status != 0)
		error = errno;
	printf("DISPLAY-SCANOUT constraints flags=0x%x formats=0x%x stride=%u offset=%u placement=0x%x error=%d\n",
	    constraints.flags,
	    constraints.formats,
	    constraints.stride_alignment,
	    constraints.offset_alignment,
	    constraints.placement,
	    error);

	/* The display's lease. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = info.display_id;
	claim.generation = info.generation;
	status = ioctl(fd, GPU_DISPLAY_CLAIM, &claim);
	if (status != 0) {
		printf("DISPLAY-SCANOUT done error=%d step=claim\n", errno);
		return 1;
	}

	printf("DISPLAY-SCANOUT claim lease=%llu\n", (unsigned long long)claim.lease);

	/* A client's blob shown as it is, held; then the cursor over it. */
	(void)scanout_shared(fd, node, &info, &constraints, claim.lease, hold);
	(void)scanout_cursor(fd, &info, claim.lease, hold);

	/* The lease goes (the cursor with it). */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = claim.lease;
	status = ioctl(fd, GPU_DISPLAY_RELEASE, &release);
	error = 0;
	if (status != 0)
		error = errno;
	printf("DISPLAY-SCANOUT done error=%d\n", error);

	/* Succeeded when the release did. */
	(void)close(fd);
	if (error != 0)
		return 1;
	return 0;
}

/* Reads the first display of the node's inventory. */
static int
scanout_query(
	int fd,
	struct gpu_display_info *info)
{
	int status;

	/* The display at index 0. */
	memset(info, 0, sizeof(*info));
	info->version = GPU_ABI_VERSION;
	info->size = sizeof(*info);
	info->index = 0U;
	status = ioctl(fd, GPU_DISPLAY_QUERY, info);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/*
 * Makes a blob of the display's size in a second open (the client), fills
 * it, exports it, imports it into the compositor's open, closes the client
 * and the export, and presents the import as it is; prints one line.
 * Returns the first error, or 0.
 */
static int
scanout_shared(
	int fd,
	const char *node,
	const struct gpu_display_info *info,
	const struct gpu_scanout_constraints *constraints,
	uint64_t lease,
	unsigned hold)
{
	struct gpu_blob_create blob;
	struct gpu_resource_export export_request;
	struct gpu_resource_import import_request;
	struct gpu_display_present present;
	uint32_t *row;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t alignment;
	uint32_t x;
	int client;
	int blob_error;
	int write_error;
	int export_error;
	int import_error;
	int present_error;
	int status;

	/* The frame's size, rows aligned as the display asks. */
	width = info->current_width;
	height = info->current_height;
	alignment = constraints->stride_alignment;
	if (alignment == 0U)
		alignment = 1U;
	stride = (width * 4U + alignment - 1U) / alignment * alignment;
	blob_error = 0;
	write_error = ENOENT;
	export_error = ENOENT;
	import_error = ENOENT;
	present_error = ENOENT;
	memset(&present, 0, sizeof(present));

	/* The client: its own open of the node. */
	client = open(node, O_RDWR | O_CLOEXEC);
	if (client < 0) {
		printf("DISPLAY-SCANOUT shared open=%d\n", errno);
		return errno;
	}

	/* The client's shareable blob. */
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = (uint64_t)stride * height;
	blob.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;
	status = ioctl(client, GPU_BLOB_CREATE, &blob);
	if (status != 0)
		blob_error = errno;

	/* Fills it with the colour, row by row. */
	row = malloc((size_t)width * 4U);
	if (blob_error == 0 && row != NULL) {
		for (x = 0U; x < width; x++)
			row[x] = SCANOUT_COLOUR;
		write_error = scanout_write_rows(client, blob.handle, stride, height, row, width * 4U);
	}

	free(row);

	/* Exports it with its image description. */
	memset(&export_request, 0, sizeof(export_request));
	export_request.fd = -1;
	if (write_error == 0) {
		export_request.version = GPU_ABI_VERSION;
		export_request.size = sizeof(export_request);
		export_request.handle = blob.handle;
		export_request.flags = GPU_HANDLE_CLOEXEC;
		export_request.image.version = GPU_ABI_VERSION;
		export_request.image.size = sizeof(export_request.image);
		export_request.image.width = width;
		export_request.image.height = height;
		export_request.image.format = GPU_PIXEL_BGRA8888;
		export_request.image.stride = stride;
		export_request.image.allocation_bytes = blob.bytes;
		export_request.image.usage = SCANOUT_IMAGE_USAGE;
		export_request.image.tiling = GPU_IMAGE_LINEAR;
		status = ioctl(client, GPU_RESOURCE_EXPORT, &export_request);
		export_error = 0;
		if (status != 0)
			export_error = errno;
	}

	/* Imports it into the compositor's open. */
	memset(&import_request, 0, sizeof(import_request));
	if (export_error == 0) {
		import_request.version = GPU_ABI_VERSION;
		import_request.size = sizeof(import_request);
		import_request.fd = export_request.fd;
		status = ioctl(fd, GPU_RESOURCE_IMPORT, &import_request);
		import_error = 0;
		if (status != 0)
			import_error = errno;
	}

	/* The client and the export go; the import alone keeps the blob. */
	if (export_request.fd >= 0)
		(void)close(export_request.fd);
	(void)close(client);

	/* Presents the import as it is. */
	if (import_error == 0) {
		present.version = GPU_ABI_VERSION;
		present.size = sizeof(present);
		present.lease = lease;
		present.handle = import_request.handle;
		present.width = width;
		present.height = height;
		present.stride = stride;
		present.format = GPU_PIXEL_BGRA8888;
		present.refresh_millihz = info->refresh_millihz;
		present.flags = GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB;
		present.generation = info->generation;
		status = ioctl(fd, GPU_DISPLAY_PRESENT, &present);
		present_error = 0;
		if (status != 0)
			present_error = errno;
	}

	/* What happened, then the frame stays up for the hold. */
	printf("DISPLAY-SCANOUT shared blob=%d write=%d export=%d import=%d present=%d sequence=%llu\n",
	    blob_error,
	    write_error,
	    export_error,
	    import_error,
	    present_error,
	    (unsigned long long)present.sequence);
	sleep(hold);

	/* Reports the presentation's outcome. */
	if (present_error != 0)
		return present_error;

	/* Succeeded: the client's blob was shown. */
	return 0;
}

/*
 * Sets a cursor image from storage, shows it in the middle of the display,
 * walks it along a diagonal, hides it, shows it again; prints one line.
 * Returns the first error, or 0.
 */
static int
scanout_cursor(
	int fd,
	const struct gpu_display_info *info,
	uint64_t lease,
	unsigned hold)
{
	struct gpu_resource_create create;
	struct gpu_display_cursor_image image;
	struct gpu_transfer transfer;
	uint32_t pixels[SCANOUT_CURSOR_SIDE * SCANOUT_CURSOR_SIDE];
	uint32_t x;
	uint32_t y;
	uint32_t moves;
	int32_t at_x;
	int32_t at_y;
	int set_error;
	int show_error;
	int move_error;
	int hide_error;
	int again_error;
	int status;

	set_error = 0;
	show_error = ENOENT;
	move_error = ENOENT;
	hide_error = ENOENT;
	again_error = ENOENT;
	moves = 0U;

	/* The image's storage. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = (uint64_t)SCANOUT_CURSOR_SIDE * SCANOUT_CURSOR_SIDE * 4U;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(fd, GPU_RESOURCE_CREATE, &create);
	if (status != 0)
		set_error = errno;

	/* The image: the border black, the rest white. */
	for (y = 0U; y < SCANOUT_CURSOR_SIDE; y++) {
		for (x = 0U; x < SCANOUT_CURSOR_SIDE; x++) {
			/* The border is the outer two pixels on every side. */
			pixels[y * SCANOUT_CURSOR_SIDE + x] = SCANOUT_CURSOR_WHITE;
			if (x < SCANOUT_CURSOR_BORDER || x >= SCANOUT_CURSOR_SIDE - SCANOUT_CURSOR_BORDER)
				pixels[y * SCANOUT_CURSOR_SIDE + x] = SCANOUT_CURSOR_BLACK;
			if (y < SCANOUT_CURSOR_BORDER || y >= SCANOUT_CURSOR_SIDE - SCANOUT_CURSOR_BORDER)
				pixels[y * SCANOUT_CURSOR_SIDE + x] = SCANOUT_CURSOR_BLACK;
		}
	}

	/* Writes it into the storage in one piece (its rows are packed). */
	if (set_error == 0) {
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = create.handle;
		transfer.offset = 0U;
		transfer.address = (uint64_t)(uintptr_t)pixels;
		transfer.bytes = (uint32_t)sizeof(pixels);
		status = ioctl(fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0)
			set_error = errno;
	}

	/* Sets the image. */
	if (set_error == 0) {
		memset(&image, 0, sizeof(image));
		image.version = GPU_ABI_VERSION;
		image.size = sizeof(image);
		image.lease = lease;
		image.handle = create.handle;
		image.width = SCANOUT_CURSOR_SIDE;
		image.height = SCANOUT_CURSOR_SIDE;
		image.stride = SCANOUT_CURSOR_SIDE * 4U;
		image.format = GPU_PIXEL_BGRA8888;
		image.generation = info->generation;
		status = ioctl(fd, GPU_DISPLAY_CURSOR_SET, &image);
		if (status != 0)
			set_error = errno;
	}

	/* Shows it in the middle and leaves it there for the hold. */
	at_x = (int32_t)(info->current_width / 2U);
	at_y = (int32_t)(info->current_height / 2U);
	if (set_error == 0) {
		show_error = scanout_cursor_move(fd, info, lease, at_x, at_y);
		sleep(hold);
	}

	/* Walks it down and to the right. */
	if (show_error == 0) {
		move_error = 0;
		for (moves = 0U; moves < SCANOUT_MOVES; moves++) {
			at_x += SCANOUT_MOVE_STEP;
			at_y += SCANOUT_MOVE_STEP;
			move_error = scanout_cursor_move(fd, info, lease, at_x, at_y);
			if (move_error != 0)
				break;

			scanout_sleep_ms(SCANOUT_MOVE_MS);
		}
	}

	/* Hides it for the hold, then shows it again where it was for the hold. */
	if (show_error == 0) {
		hide_error = scanout_cursor_hide(fd, info, lease);
		sleep(hold);
		again_error = scanout_cursor_move(fd, info, lease, at_x, at_y);
		sleep(hold);
	}

	/* What happened. */
	printf("DISPLAY-SCANOUT cursor set=%d show=%d moves=%u move=%d hide=%d again=%d\n",
	    set_error,
	    show_error,
	    moves,
	    move_error,
	    hide_error,
	    again_error);

	/* Reports the image's outcome. */
	if (set_error != 0)
		return set_error;

	/* Succeeded: the cursor was set and driven. */
	return 0;
}

/* Writes the same row to every row of a resource, rows a stride apart. */
static int
scanout_write_rows(
	int fd,
	uint64_t handle,
	uint32_t stride,
	uint32_t height,
	const uint32_t *row,
	uint32_t row_bytes)
{
	struct gpu_transfer transfer;
	uint32_t y;
	int status;

	/* One transfer for each row. */
	for (y = 0U; y < height; y++) {
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = handle;
		transfer.offset = (uint64_t)y * stride;
		transfer.address = (uint64_t)(uintptr_t)row;
		transfer.bytes = row_bytes;
		status = ioctl(fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0)
			return errno;
	}

	/* Succeeded: every row is written. */
	return 0;
}

/* Moves the cursor's top left to a position (and shows a hidden cursor). */
static int
scanout_cursor_move(
	int fd,
	const struct gpu_display_info *info,
	uint64_t lease,
	int32_t x,
	int32_t y)
{
	struct gpu_display_cursor_move move;
	int status;

	/* The request. */
	memset(&move, 0, sizeof(move));
	move.version = GPU_ABI_VERSION;
	move.size = sizeof(move);
	move.lease = lease;
	move.x = x;
	move.y = y;
	move.generation = info->generation;
	status = ioctl(fd, GPU_DISPLAY_CURSOR_MOVE, &move);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Hides the cursor. */
static int
scanout_cursor_hide(
	int fd,
	const struct gpu_display_info *info,
	uint64_t lease)
{
	struct gpu_display_cursor_hide hide;
	int status;

	/* The request. */
	memset(&hide, 0, sizeof(hide));
	hide.version = GPU_ABI_VERSION;
	hide.size = sizeof(hide);
	hide.lease = lease;
	hide.generation = info->generation;
	status = ioctl(fd, GPU_DISPLAY_CURSOR_HIDE, &hide);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Sleeps for a number of milliseconds. */
static void
scanout_sleep_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	/* The interval as seconds and nanoseconds. */
	wait.tv_sec = (time_t)(milliseconds / 1000U);
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}
