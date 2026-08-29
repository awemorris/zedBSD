/*
 * WS004 p023 test-image-only raw NVMe write/flush/readback helper.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NVME_GUEST_BYTES 4096U
#define NVME_GUEST_STRESS_COMMANDS 96U
#define NVME_GUEST_CONCURRENT_WORKERS 4U
#define NVME_GUEST_CONCURRENT_COMMANDS 32U

static int
parse_offset(const char *text, off_t *result)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' ||
	    value > INT64_MAX)
		return -1;
	*result = (off_t)value;
	return 0;
}

static void
make_pattern(uint8_t pattern[NVME_GUEST_BYTES], off_t offset)
{
	uint64_t seed = (uint64_t)offset ^ UINT64_C(0x7a65644253444e56);
	size_t index;

	for (index = 0; index < NVME_GUEST_BYTES; index++) {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		pattern[index] = (uint8_t)(seed ^ index);
	}
}

static int
write_exact(int descriptor, const uint8_t *buffer, size_t length,
	off_t offset)
{
	size_t done = 0;

	while (done < length) {
		ssize_t amount = pwrite(descriptor, buffer + done,
		    length - done, offset + (off_t)done);

		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0)
			return -1;
		done += (size_t)amount;
	}
	return 0;
}

static int
read_exact(int descriptor, uint8_t *buffer, size_t length, off_t offset)
{
	size_t done = 0;

	while (done < length) {
		ssize_t amount = pread(descriptor, buffer + done,
		    length - done, offset + (off_t)done);

		if (amount < 0 && errno == EINTR)
			continue;
		if (amount <= 0) {
			if (amount == 0)
				errno = EIO;
			return -1;
		}
		done += (size_t)amount;
	}
	return 0;
}

static int
fail(const char *stage, const char *device, off_t offset, int saved)
{
	fprintf(stderr,
	    "HW-T20 NVME-IO FAIL stage=%s device=%s offset=%llu errno=%d\n",
	    stage, device, (unsigned long long)offset, saved);
	return 1;
}

static int
exercise_range(const char *device, off_t offset, unsigned commands,
	int writing)
{
	uint8_t expected[NVME_GUEST_BYTES], actual[NVME_GUEST_BYTES];
	off_t current;
	unsigned index;
	int descriptor, saved;

	descriptor = open(device, writing ? O_RDWR : O_RDONLY);
	if (descriptor < 0)
		return fail("open", device, offset, errno);
	if (writing) {
		for (index = 0; index < commands; index++) {
			current = offset + (off_t)((uint64_t)index * NVME_GUEST_BYTES);
			make_pattern(expected, current);
			if (write_exact(descriptor, expected, sizeof(expected), current)) {
				saved = errno;
				(void)close(descriptor);
				return fail("pwrite", device, current, saved);
			}
		}
		if (fsync(descriptor) != 0) {
			saved = errno;
			(void)close(descriptor);
			return fail("fsync", device, offset, saved);
		}
	}
	for (index = 0; index < commands; index++) {
		current = offset + (off_t)((uint64_t)index * NVME_GUEST_BYTES);
		make_pattern(expected, current);
		memset(actual, 0, sizeof(actual));
		if (read_exact(descriptor, actual, sizeof(actual), current)) {
			saved = errno;
			(void)close(descriptor);
			return fail("pread", device, current, saved);
		}
		if (memcmp(actual, expected, sizeof(actual)) != 0) {
			(void)close(descriptor);
			return fail("compare", device, current, EIO);
		}
	}
	if (close(descriptor) != 0)
		return fail("close", device, offset, errno);
	return 0;
}

static int
exercise_concurrently(const char *device, off_t offset, int writing)
{
	pid_t children[NVME_GUEST_CONCURRENT_WORKERS];
	unsigned started = 0, worker;
	int failed = 0;

	for (worker = 0; worker < NVME_GUEST_CONCURRENT_WORKERS; worker++) {
		off_t worker_offset = offset + (off_t)((uint64_t)worker *
		    NVME_GUEST_CONCURRENT_COMMANDS * NVME_GUEST_BYTES);
		pid_t child = fork();

		if (child < 0) {
			(void)fail("fork", device, worker_offset, errno);
			failed = 1;
			break;
		}
		if (child == 0)
			_exit(exercise_range(device, worker_offset,
			    NVME_GUEST_CONCURRENT_COMMANDS, writing));
		children[started++] = child;
	}
	for (worker = 0; worker < started; worker++) {
		int status;
		pid_t result;

		do {
			result = waitpid(children[worker], &status, 0);
		} while (result < 0 && errno == EINTR);
		if (result != children[worker] || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 0)
			failed = 1;
	}
	return failed;
}

int
main(int argc, char **argv)
{
	uint8_t expected[NVME_GUEST_BYTES], actual[NVME_GUEST_BYTES];
	const char *mode, *device;
	off_t offset, current;
	unsigned commands, index;
	int concurrent, stress, writing;
	int descriptor, saved;

	if (argc != 4 || (strcmp(argv[1], "write") != 0 &&
	    strcmp(argv[1], "verify") != 0 &&
	    strcmp(argv[1], "stress-write") != 0 &&
	    strcmp(argv[1], "stress-verify") != 0 &&
	    strcmp(argv[1], "concurrent-write") != 0 &&
	    strcmp(argv[1], "concurrent-verify") != 0) ||
	    parse_offset(argv[3], &offset)) {
		fprintf(stderr,
		    "usage: nvme-io-guest write|verify|stress-write|stress-verify|concurrent-write|concurrent-verify DEVICE BYTE-OFFSET\n");
		return 2;
	}
	mode = argv[1];
	device = argv[2];
	concurrent = strncmp(mode, "concurrent-", 11U) == 0;
	stress = strncmp(mode, "stress-", 7U) == 0;
	writing = strcmp(mode, "write") == 0 ||
	    strcmp(mode, "stress-write") == 0 ||
	    strcmp(mode, "concurrent-write") == 0;
	commands = concurrent ? NVME_GUEST_CONCURRENT_WORKERS *
	    NVME_GUEST_CONCURRENT_COMMANDS :
	    (stress ? NVME_GUEST_STRESS_COMMANDS : 1U);
	if ((uint64_t)offset > (uint64_t)INT64_MAX -
	    (uint64_t)(commands - 1U) * NVME_GUEST_BYTES)
		return fail("range", device, offset, EOVERFLOW);
	printf("HW-T20 NVME-IO BEGIN mode=%s device=%s offset=%llu\n",
	    mode, device, (unsigned long long)offset);
	if (concurrent) {
		if (exercise_concurrently(device, offset, writing))
			return 1;
		if (writing) {
			printf("HW-T20 NVME-IO CONCURRENT-WRITE workers=%u commands=%u bytes=%u offset=%llu\n",
			    NVME_GUEST_CONCURRENT_WORKERS, commands,
			    commands * NVME_GUEST_BYTES, (unsigned long long)offset);
			printf("HW-T20 NVME-IO FSYNC offset=%llu\n",
			    (unsigned long long)offset);
		}
		printf("HW-T20 NVME-IO CONCURRENT-READBACK workers=%u commands=%u bytes=%u offset=%llu\n",
		    NVME_GUEST_CONCURRENT_WORKERS, commands,
		    commands * NVME_GUEST_BYTES, (unsigned long long)offset);
		printf("HW-T20 NVME-IO PASS mode=%s offset=%llu\n", mode,
		    (unsigned long long)offset);
		return 0;
	}
	descriptor = open(device, writing ? O_RDWR : O_RDONLY);
	if (descriptor < 0)
		return fail("open", device, offset, errno);
	if (writing) {
		for (index = 0; index < commands; index++) {
			current = offset + (off_t)((uint64_t)index * NVME_GUEST_BYTES);
			make_pattern(expected, current);
			if (write_exact(descriptor, expected, sizeof(expected), current)) {
				saved = errno;
				(void)close(descriptor);
				return fail("pwrite", device, current, saved);
			}
		}
		if (stress)
			printf("HW-T20 NVME-IO STRESS-WRITE commands=%u bytes=%u offset=%llu\n",
			    commands, commands * NVME_GUEST_BYTES,
			    (unsigned long long)offset);
		else
			printf("HW-T20 NVME-IO WRITE bytes=%u offset=%llu\n",
			    NVME_GUEST_BYTES, (unsigned long long)offset);
		if (fsync(descriptor) != 0) {
			saved = errno;
			(void)close(descriptor);
			return fail("fsync", device, offset, saved);
		}
		printf("HW-T20 NVME-IO FSYNC offset=%llu\n",
		    (unsigned long long)offset);
	}
	for (index = 0; index < commands; index++) {
		current = offset + (off_t)((uint64_t)index * NVME_GUEST_BYTES);
		make_pattern(expected, current);
		memset(actual, 0, sizeof(actual));
		if (read_exact(descriptor, actual, sizeof(actual), current)) {
			saved = errno;
			(void)close(descriptor);
			return fail("pread", device, current, saved);
		}
		if (memcmp(actual, expected, sizeof(actual)) != 0) {
			(void)close(descriptor);
			return fail("compare", device, current, EIO);
		}
	}
	if (stress)
		printf("HW-T20 NVME-IO STRESS-READBACK commands=%u bytes=%u offset=%llu\n",
		    commands, commands * NVME_GUEST_BYTES,
		    (unsigned long long)offset);
	else
		printf("HW-T20 NVME-IO READBACK bytes=%u offset=%llu\n",
		    NVME_GUEST_BYTES, (unsigned long long)offset);
	if (close(descriptor) != 0)
		return fail("close", device, offset, errno);
	printf("HW-T20 NVME-IO PASS mode=%s offset=%llu\n", mode,
	    (unsigned long long)offset);
	return 0;
}
