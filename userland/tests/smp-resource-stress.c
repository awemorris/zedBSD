/*
 * amd64 SMP lifetime and resource-accounting stress.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
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

static int
resource_snapshot(int system_fd, struct zedbsd_system_resources *resources)
{
	memset(resources, 0, sizeof(*resources));
	return ioctl(system_fd, ZEDBSD_SYSTEM_GET_RESOURCES, resources);
}

static int
resources_equal(const struct zedbsd_system_resources *left,
		const struct zedbsd_system_resources *right)
{
	return memcmp(left, right, sizeof(*left)) == 0;
}

static void
print_resource_delta(const struct zedbsd_system_resources *before,
		     const struct zedbsd_system_resources *after)
{
	const uint64_t *a = (const uint64_t *)before;
	const uint64_t *b = (const uint64_t *)after;
	static const char *const names[] = {
		"process", "thread", "filedesc", "file", "pipe",
		"mount", "inode", "namecache", "vmspace", "vm_object",
		"vm_page", "swap_slot", "disk", "bio", "socket",
		"packet", "net_device"
	};
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
		if (a[index] != b[index])
			printf("SMP_STRESS_RESOURCE_DELTA:%s:%llu:%llu\n",
			       names[index], (unsigned long long)a[index],
			       (unsigned long long)b[index]);
	}
}

static int
warm_path(unsigned worker)
{
	char path[] = "/disk1/s0";
	int descriptor;

	path[sizeof(path) - 2U] = (char)('0' + worker);
	descriptor = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (descriptor < 0)
		return -1;
	if (close(descriptor) != 0 || unlink(path) != 0)
		return -1;
	return 0;
}

static int
worker_main(unsigned worker)
{
	char path[] = "/disk1/s0";
	unsigned iteration;
	int file_descriptor;

	path[sizeof(path) - 2U] = (char)('0' + worker);
	file_descriptor = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
	if (file_descriptor < 0)
		return 10;

	for (iteration = 0; iteration < ITERATIONS; iteration++) {
		volatile unsigned char *mapping;
		unsigned char value = (unsigned char)(worker ^ iteration);
		struct stat status;
		int socket_descriptor;

		mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return 11;
		mapping[iteration & (PAGE_SIZE - 1U)] = value;
		if (munmap((void *)mapping, PAGE_SIZE) != 0)
			return 12;

		/* Every pass crosses the VFS/file/inode path.  Periodic writes also
		 * exercise concurrent metadata and buffered block I/O without making
		 * the 100k-event scheduler gate depend on PIO throughput. */
		if (fstat(file_descriptor, &status) != 0)
			return 13;
		if ((iteration & 511U) == 0 &&
		    pwrite(file_descriptor, &value, 1,
			   (off_t)(iteration & (PAGE_SIZE - 1U))) != 1)
			return 17;

		socket_descriptor = socket(AF_INET, SOCK_DGRAM, 0);
		if (socket_descriptor < 0) {
			printf("SMP_STRESS_SOCKET_FAIL:%u:%u:%d\n", worker,
			       iteration, errno);
			return 14;
		}
		if (close(socket_descriptor) != 0)
			return 15;
	}

	if (close(file_descriptor) != 0) {
		printf("SMP_STRESS_FILE_CLOSE_FAIL:%u:%d\n", worker, errno);
		return 16;
	}
	if (unlink(path) != 0) {
		printf("SMP_STRESS_UNLINK_FAIL:%u:%d\n", worker, errno);
		return 16;
	}
	return 0;
}

int
main(void)
{
	struct zedbsd_system_resources before, after;
	pid_t children[WORKERS];
	unsigned worker;
	int system_fd;
	int status;

	system_fd = open("/dev/system", O_RDONLY);
	if (system_fd < 0) {
		printf("SMP_STRESS_FAIL:open-system:%d\n", errno);
		return 1;
	}

	/* Stabilize the inode/namecache population before taking the baseline. */
	for (worker = 0; worker < WORKERS; worker++) {
		if (warm_path(worker) != 0) {
			printf("SMP_STRESS_FAIL:warm:%u:%d\n", worker, errno);
			return 2;
		}
	}
	if (resource_snapshot(system_fd, &before) != 0) {
		printf("SMP_STRESS_FAIL:snapshot-before:%d\n", errno);
		return 3;
	}

	for (worker = 0; worker < WORKERS; worker++) {
		children[worker] = fork();
		if (children[worker] < 0) {
			printf("SMP_STRESS_FAIL:fork:%u:%d\n", worker, errno);
			return 4;
		}
		if (children[worker] == 0)
			_exit(worker_main(worker));
	}

	for (worker = 0; worker < WORKERS; worker++) {
		if (waitpid(children[worker], &status, 0) != children[worker] ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("SMP_STRESS_FAIL:child:%u:%d\n", worker, status);
			return 5;
		}
	}

	if (resource_snapshot(system_fd, &after) != 0) {
		printf("SMP_STRESS_FAIL:snapshot-after:%d\n", errno);
		return 6;
	}
	if (!resources_equal(&before, &after)) {
		print_resource_delta(&before, &after);
		printf("SMP_STRESS_FAIL:resource-baseline\n");
		return 7;
	}

	printf("AMD64_SMP_STRESS_PASS events=%u\n",
	       WORKERS * ITERATIONS * EVENTS_PER_ITERATION);
	return 0;
}
