/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI display test: claims the display of the GHCI GPU and, in each
 * of its modes, presents three frames, each held for some seconds:
 *
 *   1. eight vertical colour bars (white, yellow, cyan, green, magenta,
 *      red, blue, black from the left);
 *   2. a grey ramp from black on the left to white on the right, over a
 *      red-green-blue ramp in the lower half;
 *   3. a black frame with a one-pixel white border and a white cross
 *      through the centre (the edges of the picture must all be seen).
 *
 * The first mode's bars are shown once in RGBA and once in BGRA; they look
 * the same, and swapped red and blue mean a wrong channel order.  With
 * --hold it keeps the last frame until killed.  The result is one line:
 *
 *   GHCI-DISPLAY PASS modes=N frames=F
 *   GHCI-DISPLAY FAIL step=STEP error=E
 *
 *   ghci-display [--node=/dev/gpuN] [--seconds=N] [--hold]
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

/* The GPU nodes looked at for a GHCI device, and the prefix of its driver name. */
#define NODES_MAX			16
#define GHCI_PREFIX			"ghci:"

/* How long each frame stays, in seconds, and the most modes tried. */
#define SECONDS_DEFAULT			3U
#define MODES_MAX			8U

/* The patterns of the frames. */
#define PATTERN_BARS			0U
#define PATTERN_RAMP			1U
#define PATTERN_BORDER			2U

/*
 * The state of the test: the node, its display and the lease, the frames
 * presented so far.
 */
struct display_test {
	int fd;
	struct gpu_display_info info;
	uint64_t lease;
	unsigned frames;
	unsigned seconds;
};

static int find_node(const char *named, int *fd);
static int show(struct display_test *test, uint32_t width, uint32_t height, uint32_t refresh, unsigned pattern, uint32_t format);
static void pixel(uint8_t *out, unsigned pattern, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t format);

int
main(
	int argc,
	char **argv)
{
	struct display_test test;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_display_mode mode;
	const char *named;
	const char *step;
	uint32_t widths[MODES_MAX];
	uint32_t heights[MODES_MAX];
	uint32_t refreshes[MODES_MAX];
	unsigned modes;
	unsigned index;
	unsigned pattern;
	int hold;
	int argument;
	int status;
	int error;

	/* The command line. */
	memset(&test, 0, sizeof(test));
	named = NULL;
	test.seconds = SECONDS_DEFAULT;
	hold = 0;
	for (argument = 1; argument < argc; argument++) {
		if (strncmp(argv[argument], "--node=", 7U) == 0) {
			named = argv[argument] + 7;
			continue;
		}
		if (strncmp(argv[argument], "--seconds=", 10U) == 0) {
			test.seconds = (unsigned)strtoul(argv[argument] + 10, NULL, 10);
			continue;
		}
		if (strcmp(argv[argument], "--hold") == 0) {
			hold = 1;
			continue;
		}
		fprintf(stderr, "usage: ghci-display [--node=/dev/gpuN] [--seconds=N] [--hold]\n");
		return 2;
	}

	/* The node and its display. */
	error = find_node(named, &test.fd);
	if (error != 0) {
		printf("GHCI-DISPLAY FAIL step=node error=%d\n", error);
		return 1;
	}
	test.info.version = GPU_ABI_VERSION;
	test.info.size = sizeof(test.info);
	status = ioctl(test.fd, GPU_DISPLAY_QUERY, &test.info);
	if (status != 0) {
		printf("GHCI-DISPLAY FAIL step=query error=%d\n", errno);
		return 1;
	}

	/* The display's modes. */
	modes = 0U;
	for (index = 0U; index < MODES_MAX; index++) {
		memset(&mode, 0, sizeof(mode));
		mode.version = GPU_ABI_VERSION;
		mode.size = sizeof(mode);
		mode.display_id = test.info.display_id;
		mode.generation = test.info.generation;
		mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
		mode.index = index;
		status = ioctl(test.fd, GPU_DISPLAY_MODE, &mode);
		if (status != 0)
			break;
		widths[modes] = mode.width;
		heights[modes] = mode.height;
		refreshes[modes] = mode.refresh_millihz;
		printf("ghci-display: mode %u: %ux%u at %u mHz\n", index, mode.width, mode.height, mode.refresh_millihz);
		modes++;
	}
	if (modes == 0U) {
		printf("GHCI-DISPLAY FAIL step=modes error=%d\n", ENODEV);
		return 1;
	}

	/* The lease. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = test.info.display_id;
	claim.generation = test.info.generation;
	status = ioctl(test.fd, GPU_DISPLAY_CLAIM, &claim);
	if (status != 0) {
		printf("GHCI-DISPLAY FAIL step=claim error=%d\n", errno);
		return 1;
	}
	test.lease = claim.lease;

	/* The first mode's bars in BGRA, then every mode's three patterns in RGBA. */
	step = "bgra";
	error = show(&test, widths[0], heights[0], refreshes[0], PATTERN_BARS, GPU_DISPLAY_FORMAT_BGRA8888);
	for (index = 0U; index < modes && error == 0; index++) {
		for (pattern = PATTERN_BARS; pattern <= PATTERN_BORDER && error == 0; pattern++) {
			step = "present";
			error = show(&test, widths[index], heights[index], refreshes[index], pattern, GPU_DISPLAY_FORMAT_RGBA8888);
		}
	}

	/* Held: the lease stays until the process dies. */
	while (error == 0 && hold != 0)
		pause();

	/* The lease goes back, and the controller goes dark. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = test.lease;
	status = ioctl(test.fd, GPU_DISPLAY_RELEASE, &release);
	if (error == 0 && status != 0) {
		step = "release";
		error = errno;
	}

	/* The result. */
	if (error != 0) {
		printf("GHCI-DISPLAY FAIL step=%s error=%d\n", step, error);
		return 1;
	}
	printf("GHCI-DISPLAY PASS modes=%u frames=%u\n", modes, test.frames);
	return 0;
}

/* Opens the named node, or the first GPU node whose driver is GHCI's and has a display. */
static int
find_node(
	const char *named,
	int *fd)
{
	struct gpu_info info;
	char path[32];
	int index;
	int status;
	int candidate;

	for (index = 0; index < NODES_MAX; index++) {
		if (named != NULL)
			snprintf(path, sizeof(path), "%s", named);
		else
			snprintf(path, sizeof(path), "/dev/gpu%d", index);
		candidate = open(path, O_RDWR | O_CLOEXEC);
		if (candidate < 0) {
			if (named != NULL)
				return errno;
			continue;
		}
		memset(&info, 0, sizeof(info));
		info.version = GPU_ABI_VERSION;
		info.size = sizeof(info);
		status = ioctl(candidate, GPU_GET_INFO, &info);
		if (status == 0 &&
		    strncmp(info.driver_name, GHCI_PREFIX, strlen(GHCI_PREFIX)) == 0 &&
		    (info.capabilities & GPU_CAP_DISPLAY) != 0U) {
			printf("ghci-display: %s is %s\n", path, info.driver_name);
			*fd = candidate;
			return 0;
		}
		close(candidate);
		if (named != NULL)
			return ENODEV;
	}
	return ENODEV;
}

/* Writes a frame of a pattern into new storage, presents it and holds it. */
static int
show(
	struct display_test *test,
	uint32_t width,
	uint32_t height,
	uint32_t refresh,
	unsigned pattern,
	uint32_t format)
{
	struct gpu_resource_create create;
	struct gpu_resource_destroy destroy;
	struct gpu_transfer transfer;
	struct gpu_display_present present;
	uint8_t *row;
	uint32_t x;
	uint32_t y;
	int status;
	int error;

	/* The storage of the frame. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = (uint64_t)width * height * 4U;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(test->fd, GPU_RESOURCE_CREATE, &create);
	if (status != 0)
		return errno;

	/* The pattern, row by row. */
	error = 0;
	row = malloc((size_t)width * 4U);
	if (row == NULL)
		error = ENOMEM;
	for (y = 0U; y < height && error == 0; y++) {
		for (x = 0U; x < width; x++)
			pixel(row + (size_t)x * 4U, pattern, x, y, width, height, format);
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = create.handle;
		transfer.offset = (uint64_t)y * width * 4U;
		transfer.address = (uint64_t)(uintptr_t)row;
		transfer.bytes = width * 4U;
		status = ioctl(test->fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0)
			error = errno;
	}
	free(row);

	/* The frame on the screen. */
	if (error == 0) {
		memset(&present, 0, sizeof(present));
		present.version = GPU_ABI_VERSION;
		present.size = sizeof(present);
		present.lease = test->lease;
		present.handle = create.handle;
		present.width = width;
		present.height = height;
		present.stride = width * 4U;
		present.format = format;
		present.refresh_millihz = refresh;
		present.flags = GPU_DISPLAY_PRESENT_FIFO;
		present.generation = test->info.generation;
		status = ioctl(test->fd, GPU_DISPLAY_PRESENT, &present);
		if (status != 0)
			error = errno;
	}
	if (error == 0) {
		test->frames++;
		printf("ghci-display: %ux%u pattern %u %s shown\n", width, height, pattern, format == GPU_DISPLAY_FORMAT_RGBA8888 ? "RGBA" : "BGRA");
		fflush(stdout);
		sleep(test->seconds);
	}

	/* The storage goes; the driver copied the frame. */
	memset(&destroy, 0, sizeof(destroy));
	destroy.version = GPU_ABI_VERSION;
	destroy.size = sizeof(destroy);
	destroy.handle = create.handle;
	(void)ioctl(test->fd, GPU_RESOURCE_DESTROY, &destroy);
	return error;
}

/* One pixel of a pattern, in the byte order of the format. */
static void
pixel(
	uint8_t *out,
	unsigned pattern,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	uint32_t format)
{
	static const uint32_t bars[8] = {
		0xffffffU, 0xffff00U, 0x00ffffU, 0x00ff00U, 0xff00ffU, 0xff0000U, 0x0000ffU, 0x000000U
	};
	uint32_t rgb;
	uint32_t level;

	/* The colour, as 0xRRGGBB. */
	rgb = 0U;
	if (pattern == PATTERN_BARS) {
		rgb = bars[(uint64_t)x * 8U / width];
	} else if (pattern == PATTERN_RAMP) {
		level = (uint32_t)((uint64_t)x * 255U / (width - 1U));
		rgb = (level << 16) | (level << 8) | level;
		if (y >= height / 2U) {
			if (x < width / 3U)
				rgb = level << 16;
			else if (x < 2U * width / 3U)
				rgb = level << 8;
			else
				rgb = level;
		}
	} else {
		if (x == 0U || y == 0U || x == width - 1U || y == height - 1U || x == width / 2U || y == height / 2U)
			rgb = 0xffffffU;
	}

	/* RGBA: R, G, B, A in memory; BGRA: B, G, R, A. */
	if (format == GPU_DISPLAY_FORMAT_RGBA8888) {
		out[0] = (uint8_t)(rgb >> 16);
		out[2] = (uint8_t)rgb;
	} else {
		out[0] = (uint8_t)rgb;
		out[2] = (uint8_t)(rgb >> 16);
	}
	out[1] = (uint8_t)(rgb >> 8);
	out[3] = 0xffU;
}
