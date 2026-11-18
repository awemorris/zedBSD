/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU display colour bar probe (ws269-p007): opens a GPU node, claims
 * its first display, writes four vertical bars (red, green, blue, white,
 * from the left) into a resource of the node, and presents it once as
 * RGBA and once as BGRA, each held for some seconds.  The bars look the
 * same in both; swapped red and blue mean a wrong channel order.  Then it
 * releases the display, and the text console comes back.
 *
 * With --hold it stays claimed after the frames until killed (kill or
 * Ctrl-C), never releasing: the console must come back when the process
 * dies.  The line printed before claiming says so.
 *
 * The result is one line on the console after the release:
 *
 *   GPU-BARS PASS rgba=1 bgra=1 WxH
 *   GPU-BARS FAIL step=STEP error=E
 *
 *   gpu-bars [--node=/dev/gpu0] [--seconds=N] [--hold]
 *       (N: how long each frame stays, default 3)
 */

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The node opened unless one is named, and how long each frame stays. */
#define BARS_NODE		"/dev/gpu0"
#define BARS_SECONDS		3U

/* The number of bars across the frame. */
#define BARS_COUNT		4U

static int bars_query(int fd, struct gpu_display_info *info);
static int bars_fill(int fd, uint64_t handle, uint32_t width, uint32_t height, uint32_t format);
static int bars_present(int fd, const struct gpu_display_info *info, uint64_t lease, uint64_t handle, uint32_t format);
static void bars_pixel(uint8_t *pixel, uint32_t bar, uint32_t format);

int
main(
	int argc,
	char **argv)
{
	struct gpu_display_info info;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_resource_create create;
	const char *node;
	const char *step;
	unsigned seconds;
	uint32_t width;
	uint32_t height;
	int hold;
	int index;
	int fd;
	int status;
	int error;

	/* The command line. */
	node = BARS_NODE;
	seconds = BARS_SECONDS;
	hold = 0;
	for (index = 1; index < argc; index++) {
		if (strncmp(argv[index], "--node=", 7U) == 0) {
			node = argv[index] + 7;
			continue;
		}

		/* How long each frame stays. */
		if (strncmp(argv[index], "--seconds=", 10U) == 0) {
			seconds = (unsigned)strtoul(argv[index] + 10, NULL, 10);
			continue;
		}

		/* Stay claimed until killed. */
		if (strcmp(argv[index], "--hold") == 0) {
			hold = 1;
			continue;
		}

		/* Anything else. */
		fprintf(stderr, "usage: gpu-bars [--node=/dev/gpu0] [--seconds=N] [--hold]\n");
		return 2;
	}

	/* The node and its first display. */
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("GPU-BARS FAIL step=open error=%d\n", errno);
		return 1;
	}
	error = bars_query(fd, &info);
	if (error != 0) {
		printf("GPU-BARS FAIL step=query error=%d\n", error);
		return 1;
	}

	/* The frame's size: the current one, or the preferred. */
	width = info.current_width;
	height = info.current_height;
	if (width == 0U || height == 0U) {
		width = info.preferred_width;
		height = info.preferred_height;
	}
	if (width < BARS_COUNT || height == 0U) {
		printf("GPU-BARS FAIL step=size error=%d\n", EINVAL);
		return 1;
	}

	/* The storage for one frame. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = (uint64_t)width * height * 4U;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(fd, GPU_RESOURCE_CREATE, &create);
	if (status != 0) {
		printf("GPU-BARS FAIL step=create error=%d\n", errno);
		return 1;
	}

	/* The console is about to go; say what comes. */
	printf("GPU-BARS claim %ux%u: bars red green blue white, %s\n", width, height,
	    hold != 0 ? "then held until killed (kill or Ctrl-C)" : "then the console returns");
	fflush(stdout);

	/* The display's lease (the text console stops here). */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = info.display_id;
	claim.generation = info.generation;
	status = ioctl(fd, GPU_DISPLAY_CLAIM, &claim);
	if (status != 0) {
		printf("GPU-BARS FAIL step=claim error=%d\n", errno);
		return 1;
	}

	/* The bars as RGBA, then as BGRA. */
	step = "rgba";
	error = bars_fill(fd, create.handle, width, height, GPU_PIXEL_RGBA8888);
	if (error == 0)
		error = bars_present(fd, &info, claim.lease, create.handle, GPU_PIXEL_RGBA8888);
	if (error == 0) {
		sleep(seconds);
		step = "bgra";
		error = bars_fill(fd, create.handle, width, height, GPU_PIXEL_BGRA8888);
	}
	if (error == 0)
		error = bars_present(fd, &info, claim.lease, create.handle, GPU_PIXEL_BGRA8888);
	if (error == 0)
		sleep(seconds);

	/* Held: the lease stays until the process dies. */
	while (error == 0 && hold != 0)
		pause();

	/* The lease goes back, and the console with it. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = claim.lease;
	status = ioctl(fd, GPU_DISPLAY_RELEASE, &release);
	if (error == 0 && status != 0) {
		step = "release";
		error = errno;
	}

	/* The result, on the console again. */
	if (error != 0) {
		printf("GPU-BARS FAIL step=%s error=%d\n", step, error);
		return 1;
	}
	printf("GPU-BARS PASS rgba=1 bgra=1 %ux%u\n", width, height);
	return 0;
}

/* The first display of the node. */
static int
bars_query(
	int fd,
	struct gpu_display_info *info)
{
	int status;

	/* The request. */
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

/* Writes the four bars in the format, row by row. */
static int
bars_fill(
	int fd,
	uint64_t handle,
	uint32_t width,
	uint32_t height,
	uint32_t format)
{
	struct gpu_transfer transfer;
	uint8_t *row;
	uint32_t x;
	uint32_t y;
	int status;

	/* One row of the bars. */
	row = malloc((size_t)width * 4U);
	if (row == NULL)
		return ENOMEM;
	for (x = 0U; x < width; x++)
		bars_pixel(row + (size_t)x * 4U, (uint32_t)((uint64_t)x * BARS_COUNT / width), format);

	/* The same row down the frame. */
	for (y = 0U; y < height; y++) {
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = handle;
		transfer.offset = (uint64_t)y * width * 4U;
		transfer.address = (uint64_t)(uintptr_t)row;
		transfer.bytes = width * 4U;
		status = ioctl(fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0) {
			status = errno;
			free(row);
			return status;
		}
	}

	/* Succeeded. */
	free(row);
	return 0;
}

/* Presents the whole resource as one frame in the format. */
static int
bars_present(
	int fd,
	const struct gpu_display_info *info,
	uint64_t lease,
	uint64_t handle,
	uint32_t format)
{
	struct gpu_display_present present;
	uint32_t width;
	uint32_t height;
	int status;

	/* The frame's size, as main chose it. */
	width = info->current_width;
	height = info->current_height;
	if (width == 0U || height == 0U) {
		width = info->preferred_width;
		height = info->preferred_height;
	}

	/* The frame. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = lease;
	present.handle = handle;
	present.width = width;
	present.height = height;
	present.stride = width * 4U;
	present.format = format;
	present.refresh_millihz = info->refresh_millihz;
	if (present.refresh_millihz == 0U)
		present.refresh_millihz = 60000U;
	present.flags = GPU_DISPLAY_PRESENT_FIFO;
	present.generation = info->generation;
	status = ioctl(fd, GPU_DISPLAY_PRESENT, &present);
	if (status != 0)
		return errno;

	/* Succeeded: the frame is shown. */
	return 0;
}

/* One pixel of a bar (0 red, 1 green, 2 blue, 3 white) in the byte order of the format. */
static void
bars_pixel(
	uint8_t *pixel,
	uint32_t bar,
	uint32_t format)
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	/* The bar's colour. */
	red = (bar == 0U || bar == 3U) ? 0xffU : 0U;
	green = (bar == 1U || bar == 3U) ? 0xffU : 0U;
	blue = (bar == 2U || bar == 3U) ? 0xffU : 0U;

	/* RGBA: R, G, B, A in memory; BGRA: B, G, R, A. */
	if (format == GPU_PIXEL_RGBA8888) {
		pixel[0] = red;
		pixel[2] = blue;
	} else {
		pixel[0] = blue;
		pixel[2] = red;
	}
	pixel[1] = green;
	pixel[3] = 0xffU;
}
