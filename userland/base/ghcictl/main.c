/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI GPU status tool: lists every GPU node whose driver is the
 * generic GHCI driver, with its name (ghci:<PCI vendor>:<PCI device>),
 * the capabilities the device offers, its allocation limits and, when it
 * has a display, the display's modes.  display lists the display of the
 * first GHCI node alone.
 *
 *   ghcictl [list]
 *   ghcictl display
 */

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The GPU nodes looked at, and the prefix of the GHCI driver's names. */
#define NODES_MAX			16
#define GHCI_PREFIX			"ghci:"

/* The most modes listed for a display. */
#define MODES_MAX			16U

/*
 * The names of the capability bits a device reports.
 *
 * Values referenced: zedBSD include/uapi/gpu.h, gpu-display.h,
 * gpu-allocation.h, gpu-fence.h and gpu-job.h (GPU_CAP_*).
 */
static const struct {
	uint32_t bit;
	const char *name;
} capability_names[] = {
	{ 1U, "resource" },
	{ 2U, "capset" },
	{ 4U, "blob" },
	{ 8U, "transfer" },
	{ 16U, "command" },
	{ 32U, "present" },
	{ 64U, "mapping" },
	{ 128U, "display" },
	{ 256U, "share" },
	{ 512U, "notification" },
	{ 1024U, "allocation-share" },
	{ 2048U, "fence" },
	{ 4096U, "display-events" },
	{ 8192U, "job" },
	{ 16384U, "job-capacity" },
	{ 32768U, "display-control" }
};

static int list_all(void);
static int list_node(const char *path);
static void list_display(int fd);
static int open_ghci(char *path, size_t size);
static int show_display(void);
static void usage(void);

int
main(
	int argc,
	char **argv)
{
	int status;

	/* The list, the default. */
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
		status = list_all();
		return status;
	}

	/* The display of the first GHCI node. */
	if (argc == 2 && strcmp(argv[1], "display") == 0) {
		status = show_display();
		return status;
	}

	/* Anything else. */
	usage();
	return 2;
}

/* Lists every GHCI node: 0, or 1 when there is none. */
static int
list_all(void)
{
	char path[32];
	int index;
	int found;

	/* Every GPU node, those of the GHCI driver listed. */
	found = 0;
	for (index = 0; index < NODES_MAX; index++) {
		snprintf(path, sizeof(path), "/dev/gpu%d", index);
		found += list_node(path);
	}

	/* Says when there is none. */
	if (found == 0) {
		printf("ghcictl: no GHCI GPU\n");
		return 1;
	}

	/* Succeeded: the nodes are listed. */
	return 0;
}

/* Lists one node when it is a GHCI device; 1 when it was. */
static int
list_node(
	const char *path)
{
	struct gpu_info info;
	unsigned index;
	int status;
	int fd;

	/* A node that does not open, or of another driver, is skipped. */
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return 0;
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	status = ioctl(fd, GPU_GET_INFO, &info);
	if (status != 0 || strncmp(info.driver_name, GHCI_PREFIX, strlen(GHCI_PREFIX)) != 0) {
		close(fd);
		return 0;
	}

	/* The name, the limits and the capabilities. */
	printf("%s: %s\n", path, info.driver_name);
	printf("  resources: at most %u per open, %llu bytes each\n", info.max_resources, (unsigned long long)info.max_resource_bytes);
	printf("  capabilities:");
	for (index = 0U; index < sizeof(capability_names) / sizeof(capability_names[0]); index++) {
		if ((info.capabilities & capability_names[index].bit) != 0U)
			printf(" %s", capability_names[index].name);
	}
	printf("\n");

	/* The display and its modes. */
	if ((info.capabilities & GPU_CAP_DISPLAY) != 0U)
		list_display(fd);
	close(fd);
	return 1;
}

/* Lists the display of a node and its modes. */
static void
list_display(
	int fd)
{
	struct gpu_display_info display;
	struct gpu_display_mode mode;
	unsigned index;
	int status;

	/* The first output. */
	memset(&display, 0, sizeof(display));
	display.version = GPU_ABI_VERSION;
	display.size = sizeof(display);
	status = ioctl(fd, GPU_DISPLAY_QUERY, &display);
	if (status != 0) {
		printf("  display: not answered (%d)\n", errno);
		return;
	}
	printf("  display %u \"%s\": %ux%u now, %s\n", display.display_id, display.name, display.current_width, display.current_height,
	    (display.flags & GPU_DISPLAY_ACTIVE) != 0U ? "lit" : "dark");

	/* Its modes. */
	for (index = 0U; index < MODES_MAX; index++) {
		memset(&mode, 0, sizeof(mode));
		mode.version = GPU_ABI_VERSION;
		mode.size = sizeof(mode);
		mode.display_id = display.display_id;
		mode.generation = display.generation;
		mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
		mode.index = index;
		status = ioctl(fd, GPU_DISPLAY_MODE, &mode);
		if (status != 0)
			break;
		printf("  mode %u: %ux%u at %u.%03u Hz\n", index, mode.width, mode.height, mode.refresh_millihz / 1000U, mode.refresh_millihz % 1000U);
	}
}

/* Opens the first GHCI node and names it in path: the descriptor, or -1. */
static int
open_ghci(
	char *path,
	size_t size)
{
	struct gpu_info info;
	int index;
	int status;
	int other;
	int fd;

	/* Every GPU node until one is the GHCI driver's. */
	for (index = 0; index < NODES_MAX; index++) {
		snprintf(path, size, "/dev/gpu%d", index);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			/* A node that is there but does not open says why. */
			if (errno != ENOENT)
				fprintf(stderr, "ghcictl: %s: %s\n", path, strerror(errno));
			continue;
		}
		memset(&info, 0, sizeof(info));
		info.version = GPU_ABI_VERSION;
		info.size = sizeof(info);
		status = ioctl(fd, GPU_GET_INFO, &info);
		other = 1;
		if (status == 0)
			other = strncmp(info.driver_name, GHCI_PREFIX, strlen(GHCI_PREFIX));
		if (other == 0)
			return fd;
		close(fd);
	}

	/* None. */
	fprintf(stderr, "ghcictl: no GHCI GPU\n");
	return -1;
}

/* ghcictl display: lists the display of the first GHCI node; 0, or 1 when there is none. */
static int
show_display(void)
{
	struct gpu_info info;
	char path[32];
	int status;
	int fd;

	/* The first GHCI node. */
	fd = open_ghci(path, sizeof(path));
	if (fd < 0)
		return 1;

	/* A device without a display says so. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	status = ioctl(fd, GPU_GET_INFO, &info);
	if (status != 0 || (info.capabilities & GPU_CAP_DISPLAY) == 0U) {
		printf("%s: %s: no display\n", path, info.driver_name);
		close(fd);
		return 1;
	}

	/* Succeeded: the display and its modes. */
	printf("%s: %s\n", path, info.driver_name);
	list_display(fd);
	close(fd);
	return 0;
}

/* Prints how the tool is used. */
static void
usage(void)
{
	fprintf(stderr, "usage: ghcictl [list]\n");
	fprintf(stderr, "       ghcictl display\n");
}
