/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU display control probe (ws113-p012): drives GPU_DISPLAY_REFRESH
 * and GPU_DISPLAY_POWER on a display of a GPU node (the first of the
 * inventory unless --index names another, ws051-p004b), with no
 * compositor running.  It counts the refresh boundaries for a second,
 * claims the display, presents one solid frame when the node takes
 * storage, waits, powers the display off (and checks no boundary comes
 * while it is off), waits, powers it on, waits, and releases it.  The lines the tests
 * read:
 *
 *   DISPLAY-CONTROL display id=ID generation=G flags=0xF power=0|1 counter=0|1
 *   DISPLAY-CONTROL chosen index=N count=C name=NAME
 *   DISPLAY-CONTROL refresh now|shown|on boundaries=N ms=M virtual=0|1 error=E
 *       (now: before the claim, when an output that scans nothing out makes no boundary; shown: with the
 *       lease's frame; on: after power on)
 *   DISPLAY-CONTROL claim lease=L / present error=E / power state=off|on error=E
 *   DISPLAY-CONTROL refresh-off error=E                         (ETIMEDOUT while off is right)
 *   DISPLAY-CONTROL done error=E
 *
 *   display-control [--node=/dev/gpu0] [--hold=SECONDS] [--index=N]
 *       (each wait: shown, off, on; default 3.  N: the display at index N
 *       of GPU_DISPLAY_QUERY, default 0, the resident output; claiming
 *       another connector moves the output to it)
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

/* The node opened unless one is named, and how long the display stays off and on. */
#define CONTROL_NODE		"/dev/gpu0"
#define CONTROL_HOLD		3U

/* How many inventory entries the search for a display ID walks at most. */
#define CONTROL_MAX_DISPLAYS	64U

/* The solid colour of the frame presented (BGRA: a green). */
#define CONTROL_COLOUR		0xff30c060U

static int control_query(int fd, uint32_t index, struct gpu_display_info *info);
static int control_find(int fd, uint32_t display_id, struct gpu_display_info *info);
static int control_refresh(int fd, const struct gpu_display_info *info, uint64_t cursor, uint64_t timeout_ns, struct gpu_display_refresh *result);
static void control_count(int fd, const struct gpu_display_info *info, const char *label);
static int control_power(int fd, const struct gpu_display_info *info, uint32_t state);
static int control_present(int fd, const struct gpu_display_info *info, uint64_t lease);
static uint64_t control_now_ms(void);

int
main(
	int argc,
	char **argv)
{
	struct gpu_display_info info;
	struct gpu_display_info found;
	struct gpu_display_claim claim;
	struct gpu_display_release release;
	struct gpu_display_refresh refresh;
	const char *node;
	uint32_t chosen;
	unsigned hold;
	int index;
	int same;
	int fd;
	int status;
	int error;

	/* The command line. */
	node = CONTROL_NODE;
	hold = CONTROL_HOLD;
	chosen = 0U;
	for (index = 1; index < argc; index++) {
		same = strncmp(argv[index], "--node=", 7U);
		if (same == 0) {
			node = argv[index] + 7;
			continue;
		}

		/* How long it stays off, then on. */
		same = strncmp(argv[index], "--hold=", 7U);
		if (same == 0) {
			hold = (unsigned)strtoul(argv[index] + 7, NULL, 10);
			continue;
		}

		/* Which display of the inventory is claimed. */
		same = strncmp(argv[index], "--index=", 8U);
		if (same == 0) {
			chosen = (uint32_t)strtoul(argv[index] + 8, NULL, 10);
			continue;
		}

		/* Anything else. */
		fprintf(stderr, "usage: display-control [--node=/dev/gpu0] [--hold=SECONDS] [--index=N]\n");
		return 2;
	}

	/* The node, and the chosen display. */
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("DISPLAY-CONTROL done error=%d step=open\n", errno);
		return 1;
	}

	/* The chosen display (index 0 is the resident output). */
	error = control_query(fd, chosen, &info);
	if (error != 0) {
		printf("DISPLAY-CONTROL done error=%d step=query\n", error);
		return 1;
	}

	/* What it is and offers. */
	printf("DISPLAY-CONTROL display id=%u generation=%llu flags=0x%x power=%d counter=%d\n",
	    info.display_id,
	    (unsigned long long)info.generation,
	    info.flags,
	    (info.flags & GPU_DISPLAY_POWER_CONTROL) != 0U,
	    (info.flags & GPU_DISPLAY_REFRESH_COUNTER) != 0U);

	/* Which one it is of how many. */
	info.name[sizeof(info.name) - 1U] = '\0';
	printf("DISPLAY-CONTROL chosen index=%u count=%u name=%s\n", chosen, info.count, info.name);

	/* The boundaries of a second, as the display is now. */
	control_count(fd, &info, "now");

	/* The display's lease. */
	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = info.display_id;
	claim.generation = info.generation;
	status = ioctl(fd, GPU_DISPLAY_CLAIM, &claim);
	if (status != 0) {
		printf("DISPLAY-CONTROL done error=%d step=claim\n", errno);
		return 1;
	}

	/* The lease the tests read. */
	printf("DISPLAY-CONTROL claim lease=%llu\n", (unsigned long long)claim.lease);

	/* One solid frame, when the node takes storage (the boundaries then follow the lease's frame). */
	error = control_present(fd, &info, claim.lease);
	printf("DISPLAY-CONTROL present error=%d\n", error);

	/* The boundaries of a second while the lease's frame is shown. */
	control_count(fd, &info, "shown");
	sleep(hold);

	/* Off: no boundary while it is off, and the query says so. */
	error = control_power(fd, &info, GPU_DISPLAY_POWER_OFF);
	printf("DISPLAY-CONTROL power state=off error=%d\n", error);
	error = control_find(fd, claim.display_id, &found);
	if (error == 0)
		info = found;
	printf("DISPLAY-CONTROL query powered_off=%d\n", (info.flags & GPU_DISPLAY_POWERED_OFF) != 0U);
	error = control_refresh(fd, &info, 0U, 0U, &refresh);
	if (error == 0)
		error = control_refresh(fd, &info, refresh.sequence, 300000000ULL, &refresh);
	printf("DISPLAY-CONTROL refresh-off error=%d\n", error);
	sleep(hold);

	/* On again, and its boundaries. */
	error = control_power(fd, &info, GPU_DISPLAY_POWER_ON);
	printf("DISPLAY-CONTROL power state=on error=%d\n", error);
	control_count(fd, &info, "on");
	sleep(hold);

	/* The lease goes. */
	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = claim.lease;
	status = ioctl(fd, GPU_DISPLAY_RELEASE, &release);
	error = 0;
	if (status != 0)
		error = errno;
	printf("DISPLAY-CONTROL done error=%d\n", error);

	/* Succeeded when the release did. */
	(void)close(fd);
	if (error != 0)
		return 1;
	return 0;
}

/* Reads the display at an index of the node's inventory. */
static int
control_query(
	int fd,
	uint32_t index,
	struct gpu_display_info *info)
{
	int status;

	/* The display at the index. */
	memset(info, 0, sizeof(*info));
	info->version = GPU_ABI_VERSION;
	info->size = sizeof(*info);
	info->index = index;
	status = ioctl(fd, GPU_DISPLAY_QUERY, info);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/*
 * Reads a display by its ID: a claim that moved the output makes the
 * claimed display the resident one, index 0, so its index changes.
 */
static int
control_find(
	int fd,
	uint32_t display_id,
	struct gpu_display_info *info)
{
	uint32_t index;
	int error;

	/* Walks the inventory until the display or its end (the query's EINVAL). */
	for (index = 0U; index < CONTROL_MAX_DISPLAYS; index++) {
		error = control_query(fd, index, info);
		if (error != 0)
			return error;

		/* The display with the ID. */
		if (info->display_id == display_id)
			break;
	}

	/* Not in the inventory. */
	if (index == CONTROL_MAX_DISPLAYS)
		return ENOENT;

	/* Succeeded: info is the display. */
	return 0;
}

/* Waits for a refresh boundary after a cursor (0: the count now). */
static int
control_refresh(
	int fd,
	const struct gpu_display_info *info,
	uint64_t cursor,
	uint64_t timeout_ns,
	struct gpu_display_refresh *result)
{
	int status;

	/* The request. */
	memset(result, 0, sizeof(*result));
	result->version = GPU_ABI_VERSION;
	result->size = sizeof(*result);
	result->display_id = info->display_id;
	result->generation = info->generation;
	result->cursor = cursor;
	result->timeout_ns = timeout_ns;
	status = ioctl(fd, GPU_DISPLAY_REFRESH, result);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Counts the boundaries of about a second and prints them. */
static void
control_count(
	int fd,
	const struct gpu_display_info *info,
	const char *label)
{
	struct gpu_display_refresh refresh;
	uint64_t start;
	uint64_t first;
	uint64_t cursor;
	uint64_t now;
	unsigned virtual_clock;
	int error;

	/* The count now. */
	error = control_refresh(fd, info, 0U, 0U, &refresh);
	if (error != 0) {
		printf("DISPLAY-CONTROL refresh %s error=%d\n", label, error);
		return;
	}

	/* Each boundary after the last, for a second. */
	start = control_now_ms();
	first = refresh.sequence;
	cursor = refresh.sequence;
	virtual_clock = 0U;
	for (;;) {
		/* A second is enough. */
		now = control_now_ms();
		if (now - start >= 1000U)
			break;

		/* The next boundary. */
		error = control_refresh(fd, info, cursor, 200000000ULL, &refresh);
		if (error != 0)
			break;
		cursor = refresh.sequence;
		if ((refresh.flags & GPU_DISPLAY_VIRTUAL_CLOCK) != 0U)
			virtual_clock = 1U;
	}

	/* What was seen. */
	printf("DISPLAY-CONTROL refresh %s boundaries=%llu ms=%llu virtual=%u error=%d\n",
	    label,
	    (unsigned long long)(cursor - first),
	    (unsigned long long)(control_now_ms() - start),
	    virtual_clock,
	    error);
}

/* Powers the display on or off. */
static int
control_power(
	int fd,
	const struct gpu_display_info *info,
	uint32_t state)
{
	struct gpu_display_power power;
	int status;

	/* The request. */
	memset(&power, 0, sizeof(power));
	power.version = GPU_ABI_VERSION;
	power.size = sizeof(power);
	power.display_id = info->display_id;
	power.generation = info->generation;
	power.state = state;
	status = ioctl(fd, GPU_DISPLAY_POWER, &power);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/* Presents one solid frame of the display's current size from node storage. */
static int
control_present(
	int fd,
	const struct gpu_display_info *info,
	uint64_t lease)
{
	struct gpu_resource_create create;
	struct gpu_transfer transfer;
	struct gpu_display_present present;
	uint32_t *row;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	int status;

	/* The frame's size: the current one, or the preferred. */
	width = info->current_width;
	height = info->current_height;
	if (width == 0U || height == 0U) {
		width = info->preferred_width;
		height = info->preferred_height;
	}

	/* A display without a size takes no frame. */
	if (width == 0U || height == 0U)
		return EINVAL;

	/* The storage. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = (uint64_t)width * height * 4U;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(fd, GPU_RESOURCE_CREATE, &create);
	if (status != 0)
		return errno;

	/* One row of the colour, written row by row. */
	row = malloc((size_t)width * 4U);
	if (row == NULL)
		return ENOMEM;
	for (x = 0U; x < width; x++)
		row[x] = CONTROL_COLOUR;
	for (y = 0U; y < height; y++) {
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = create.handle;
		transfer.offset = (uint64_t)y * width * 4U;
		transfer.address = (uint64_t)(uintptr_t)row;
		transfer.bytes = width * 4U;
		status = ioctl(fd, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0) {
			free(row);
			return errno;
		}
	}

	/* The row is not needed again. */
	free(row);

	/* The frame. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = lease;
	present.handle = create.handle;
	present.width = width;
	present.height = height;
	present.stride = width * 4U;
	present.format = GPU_PIXEL_BGRA8888;
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

/* The monotonic time in milliseconds. */
static uint64_t
control_now_ms(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
