/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * amd64 SMP lifetime and resource-accounting stress.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define WORKERS 8
#define ITERATIONS 4167
#define PAGE_SIZE 4096U
#define EVENTS_PER_ITERATION 3U

static int warm_path(unsigned worker);
static int resource_snapshot(int system_fd, struct system_resource_info *resources);
static int worker_main(unsigned worker);
static int resources_equal(const struct system_resource_info *left, const struct system_resource_info *right);
static void print_resource_delta(const struct system_resource_info *before, const struct system_resource_info *after);

/*
 * Runs the tests command.
 */
int
main(
	void)
{
	struct system_resource_info before, after;
	pid_t children[WORKERS];
	unsigned worker;
	int system_fd;
	int status;

	system_fd = open("/dev/system", O_RDONLY);

	/* Handles the system fd condition. */
	if (system_fd < 0) {
		printf("SMP_STRESS_FAIL:open-system:%d\n", errno);

		/* Reports operation failure. */
		return 1;
	}

	/*
 * Stabilize the inode/namecache population before taking the baseline.
	 */
	/* Process each element required by the operation. */
	for (worker = 0; worker < WORKERS; worker++) {
		/* Handles a failed warm path operation. */
		if (warm_path(worker) != 0) {
			printf("SMP_STRESS_FAIL:warm:%u:%d\n", worker, errno);

			/* Reports operation failure. */
			return 2;
		}
	}

	/* Handles a failed resource snapshot operation. */
	if (resource_snapshot(system_fd, &before) != 0) {
		printf("SMP_STRESS_FAIL:snapshot-before:%d\n", errno);

		/* Returns the computed result. */
		return 3;
	}

	/* Process each element required by the operation. */
	for (worker = 0; worker < WORKERS; worker++) {
		children[worker] = fork();

		/* Handles the children condition. */
		if (children[worker] < 0) {
			printf("SMP_STRESS_FAIL:fork:%u:%d\n", worker, errno);

			/* Returns the computed result. */
			return 4;
		}

		/* Handles the children condition. */
		if (children[worker] == 0)
			_exit(worker_main(worker));
	}

	/* Process each element required by the operation. */
	for (worker = 0; worker < WORKERS; worker++) {
		/* Handles a failed waitpid operation. */
		if (waitpid(children[worker], &status, 0) != children[worker] ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("SMP_STRESS_FAIL:child:%u:%d\n", worker, status);

			/* Returns the computed result. */
			return 5;
		}
	}

	/* Handles a failed resource snapshot operation. */
	if (resource_snapshot(system_fd, &after) != 0) {
		printf("SMP_STRESS_FAIL:snapshot-after:%d\n", errno);

		/* Returns the computed result. */
		return 6;
	}

	/* Handles a failed resources equal operation. */
	if (!resources_equal(&before, &after)) {
		print_resource_delta(&before, &after);
		printf("SMP_STRESS_FAIL:resource-baseline\n");

		/* Returns the computed result. */
		return 7;
	}

	printf("AMD64_SMP_STRESS_PASS events=%u\n",
	       WORKERS * ITERATIONS * EVENTS_PER_ITERATION);

	/* Reports successful completion. */
	return 0;
}

/* Supports the warm path operation. */
static int
warm_path(
	unsigned worker)
{
	char path[] = "/tmp/s0";
	int descriptor;

	path[sizeof(path) - 2U] = (char)('0' + worker);
	descriptor = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed close operation. */
	if (close(descriptor) != 0 || unlink(path) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the resource snapshot operation. */
static int
resource_snapshot(
	int system_fd,
	struct system_resource_info *resources)
{
	int function_result;

	memset(resources, 0, sizeof(*resources));

	/* Obtains the ioctl result. */
	function_result = ioctl(system_fd, ZEDBSD_SYSTEM_GET_RESOURCES, resources);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the worker main operation. */
static int
worker_main(
	unsigned worker)
{
	volatile unsigned char *shared;
	unsigned char received;
	int pair[2];
	volatile unsigned char *mapping;
	unsigned char value;
	struct stat status;
	int socket_descriptor;
	char path[] = "/tmp/s0";
	unsigned iteration;
	int file_descriptor;

	path[sizeof(path) - 2U] = (char)('0' + worker);
	file_descriptor = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);

	/* Handles the file descriptor condition. */
	if (file_descriptor < 0)
		return 10;

	/* Handles a failed ftruncate operation. */
	if (ftruncate(file_descriptor, PAGE_SIZE) != 0)
		return 18;

	/* Process each element required by the operation. */
	for (iteration = 0; iteration < ITERATIONS; iteration++) {
		value = (unsigned char)(worker ^ iteration);

		mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		/* Handles an operation failure. */
		if (mapping == MAP_FAILED)
			return 11;
		mapping[iteration & (PAGE_SIZE - 1U)] = value;

		/* Handles a failed munmap operation. */
		if (munmap((void *)mapping, PAGE_SIZE) != 0)
			return 12;

		/*
 * Every pass crosses the VFS/file/inode path.  Periodic writes
		 * also exercise concurrent metadata and buffered block I/O
		 * without making the 100k-event scheduler gate depend on PIO
		 * throughput. */
		if (fstat(file_descriptor, &status) != 0)
			return 13;

		/* Handles a failed pwrite operation. */
		if ((iteration & 511U) == 0 &&
		    pwrite(file_descriptor, &value, 1,
			   (off_t)(iteration & (PAGE_SIZE - 1U))) != 1)

			/* Returns the computed result. */
			return 17;

		/* Handles the iteration condition. */
		if ((iteration & 511U) == 0) {
			shared = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED, file_descriptor, 0);

			/* Handles an operation failure. */
			if (shared == MAP_FAILED)
				return 19;
			shared[(worker * 37U + iteration) & (PAGE_SIZE - 1U)] =
			    value;

			/* Handles a failed msync operation. */
			if (msync((void *)shared, PAGE_SIZE, MS_SYNC) != 0 ||
			    munmap((void *)shared, PAGE_SIZE) != 0)

				/* Returns the computed result. */
				return 20;
		}

		socket_descriptor = socket(AF_INET, SOCK_DGRAM, 0);

		/* Handles the socket descriptor condition. */
		if (socket_descriptor < 0) {
			printf("SMP_STRESS_SOCKET_FAIL:%u:%u:%d\n", worker,
			       iteration, errno);

			/* Returns the computed result. */
			return 14;
		}

		/* Handles a failed close operation. */
		if (close(socket_descriptor) != 0)
			return 15;

		/* Handles the iteration condition. */
		if ((iteration & 255U) == 0) {
			received = 0;

			/* Handles a failed socketpair operation. */
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
				return 21;

			/* Handles a failed write operation. */
			if (write(pair[0], &value, 1) != 1 ||
			    read(pair[1], &received, 1) != 1 ||
			    received != value || close(pair[0]) != 0 ||
			    close(pair[1]) != 0)

				/* Returns the computed result. */
				return 22;
		}
	}

	/* Handles a failed close operation. */
	if (close(file_descriptor) != 0) {
		printf("SMP_STRESS_FILE_CLOSE_FAIL:%u:%d\n", worker, errno);

		/* Returns the computed result. */
		return 16;
	}

	/* Handles a failed unlink operation. */
	if (unlink(path) != 0) {
		printf("SMP_STRESS_UNLINK_FAIL:%u:%d\n", worker, errno);

		/* Returns the computed result. */
		return 16;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the resources equal operation. */
static int
resources_equal(
	const struct system_resource_info *left,
	const struct system_resource_info *right)
{
	int function_result;

	/* Computes the function result. */
	function_result = memcmp(left, right, sizeof(*left)) == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the print resource delta operation. */
static void
print_resource_delta(
	const struct system_resource_info *before,
	const struct system_resource_info *after)
{
	const uint64_t *a;
	const uint64_t *b;
	static const char *const names[] = {
	    "process", "thread",    "filedesc",	 "file",    "pipe",
	    "mount",   "inode",	    "namecache", "vmspace", "vm_object",
	    "vm_page", "swap_slot", "disk",	 "bio",	    "socket",
	    "packet",  "net_device"};
	size_t index;

	a = (const uint64_t *)before;
	b = (const uint64_t *)after;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		/* Handles the a condition. */
		if (a[index] != b[index]) {
			printf("SMP_STRESS_RESOURCE_DELTA:%s:%llu:%llu\n",
			       names[index], (unsigned long long)a[index],
			       (unsigned long long)b[index]);
		}
	}
}
