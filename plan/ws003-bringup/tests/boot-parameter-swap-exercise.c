/* BR-T46 production-ABI anonymous-memory swap exercise. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define PAGE_SIZE ZEDBSD_SYSTEM_SWAP_PAGE_SIZE
#define MIN_PRESSURE (8U * 1024U * 1024U)
#define MAX_PRESSURE (16U * 1024U * 1024U)
#define MIN_PAGE_OUT_PAGES 1024U
#define OBJECT_MAP_ADDRESS ((void *)(uintptr_t)0x20000000U)
#define OBJECT_TEST_PATH "/bin/top"

enum exercise_phase {
	EXERCISE_SETUP,
	EXERCISE_TOUCH,
	EXERCISE_AFTER_TOUCH,
	EXERCISE_OBJECT_SHARED,
	EXERCISE_READ,
	EXERCISE_AFTER_READ
};

static volatile sig_atomic_t active_phase = EXERCISE_SETUP;
static volatile sig_atomic_t active_page;
static int system_descriptor = -1;

static int
snapshot(int descriptor, struct vm_statistics *statistics)
{
	return ioctl(descriptor, ZEDBSD_SYSTEM_GET_VMSTAT, statistics);
}

static size_t
append_text(char *output, size_t at, size_t capacity, const char *text)
{
	while (*text != '\0' && at < capacity)
		output[at++] = *text++;
	return at;
}

static size_t
append_number(char *output, size_t at, size_t capacity, uint64_t value)
{
	char reverse[24];
	size_t count = 0;

	do {
		reverse[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0 && count < sizeof(reverse));
	while (count != 0 && at < capacity)
		output[at++] = reverse[--count];
	return at;
}

static const char *
phase_name(sig_atomic_t phase)
{
	switch (phase) {
	case EXERCISE_TOUCH:
		return "touch";
	case EXERCISE_AFTER_TOUCH:
		return "after-touch";
	case EXERCISE_OBJECT_SHARED:
		return "object-shared";
	case EXERCISE_READ:
		return "read";
	case EXERCISE_AFTER_READ:
		return "after-read";
	default:
		return "setup";
	}
}

static void
fault_signal(int signal_number)
{
	struct vm_statistics statistics;
	char output[320];
	size_t at = 0;

	at = append_text(output, at, sizeof(output),
	    "BR-T46-SWAP-EXERCISE SIGNAL signal=");
	at = append_number(output, at, sizeof(output), (uint64_t)signal_number);
	at = append_text(output, at, sizeof(output), " phase=");
	at = append_text(output, at, sizeof(output), phase_name(active_phase));
	at = append_text(output, at, sizeof(output), " page=");
	at = append_number(output, at, sizeof(output), (uint64_t)active_page);
	if (at < sizeof(output))
		output[at++] = '\n';
	(void)write(STDOUT_FILENO, output, at);
	/* The location marker is deliberately written before the diagnostic
	 * ioctl.  Even if VM pressure prevents the best-effort snapshot, the
	 * failing phase and page remain observable. */
	at = 0;
	at = append_text(output, at, sizeof(output),
	    "BR-T46-SWAP-EXERCISE SIGNAL-VM ");
	if (system_descriptor >= 0 &&
	    snapshot(system_descriptor, &statistics) == 0) {
		at = append_text(output, at, sizeof(output), "free=");
		at = append_number(output, at, sizeof(output),
		    statistics.physical_free);
		at = append_text(output, at, sizeof(output), " page-in=");
		at = append_number(output, at, sizeof(output),
		    statistics.vm_page_in);
		at = append_text(output, at, sizeof(output), " page-out=");
		at = append_number(output, at, sizeof(output),
		    statistics.vm_page_out);
		at = append_text(output, at, sizeof(output), " swapped=");
		at = append_number(output, at, sizeof(output),
		    statistics.vm_swapped);
		at = append_text(output, at, sizeof(output), " swap-free=");
		at = append_number(output, at, sizeof(output),
		    statistics.swap_free);
		at = append_text(output, at, sizeof(output), " io-errors=");
		at = append_number(output, at, sizeof(output),
		    statistics.vm_io_errors);
	} else {
		at = append_text(output, at, sizeof(output), "unavailable");
	}
	if (at < sizeof(output))
		output[at++] = '\n';
	(void)write(STDOUT_FILENO, output, at);
	_exit(128 + signal_number);
}

static unsigned char
page_pattern(size_t page)
{
	return (unsigned char)((page * 131U + 0x5aU) & 0xffU);
}

static int
failure(const char *stage)
{
	printf("BR-T46-SWAP-EXERCISE FAIL stage=%s\n", stage);
	return 1;
}

int
main(void)
{
	struct vm_statistics before, after_write, after_read;
	volatile unsigned char *mapping;
	volatile unsigned char *object_mapping;
	uint64_t swap_bytes, pressure, wanted;
	size_t length, pages, page, touched_pages = 0;
	unsigned char object_expected;
	int descriptor, object_descriptor;

	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0)
		return failure("open-system");
	system_descriptor = descriptor;
	if (signal(SIGSEGV, fault_signal) == SIG_ERR ||
	    signal(SIGBUS, fault_signal) == SIG_ERR) {
		(void)close(descriptor);
		return failure("signal-handler");
	}
	if (snapshot(descriptor, &before) != 0 || before.swap_total == 0 ||
	    before.physical_free < 4U * PAGE_SIZE) {
		(void)close(descriptor);
		return failure("initial-statistics");
	}
	object_descriptor = open(OBJECT_TEST_PATH, O_RDONLY);
	if (object_descriptor < 0 ||
	    pread(object_descriptor, &object_expected, 1, 0) != 1) {
		if (object_descriptor >= 0)
			(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("object-fixture");
	}
	swap_bytes = before.swap_total * (uint64_t)PAGE_SIZE;
	pressure = swap_bytes / 4U;
	if (pressure < MIN_PRESSURE)
		pressure = MIN_PRESSURE;
	if (pressure > MAX_PRESSURE)
		pressure = MAX_PRESSURE;
	/* Consume the currently free physical pages plus a bounded amount that
	 * can only be satisfied by reclaiming private pages to active swap. */
	wanted = before.physical_free + pressure;
	wanted &= ~((uint64_t)PAGE_SIZE - 1U);
	if (wanted == 0 || wanted > (uint64_t)(size_t)-1) {
		(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("exercise-size");
	}
	length = (size_t)wanted;
	pages = length / PAGE_SIZE;
	printf("BR-T46-SWAP-EXERCISE START free=%llu bytes=%llu pages=%llu\n",
	    (unsigned long long)before.physical_free,
	    (unsigned long long)wanted, (unsigned long long)pages);
	fflush(stdout);
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("mmap");
	}
	for (page = 0; page < pages; page++) {
		active_phase = EXERCISE_TOUCH;
		active_page = (sig_atomic_t)page;
		mapping[page * PAGE_SIZE] = page_pattern(page);
		touched_pages = page + 1U;
		if ((page & 1023U) == 1023U) {
			struct vm_statistics progress;
			uint64_t page_out;
			if (snapshot(descriptor, &progress) != 0) {
				(void)munmap((void *)mapping, length);
				(void)close(object_descriptor);
				(void)close(descriptor);
				return failure("progress-statistics");
			}
			page_out = progress.vm_page_out >= before.vm_page_out ?
			    progress.vm_page_out - before.vm_page_out : 0;
			printf("BR-T46-SWAP-EXERCISE TOUCH pages=%llu free=%llu "
			       "page-out=%llu\n",
			    (unsigned long long)(page + 1U),
			    (unsigned long long)progress.physical_free,
			    (unsigned long long)page_out);
			fflush(stdout);
			if (page_out >= MIN_PAGE_OUT_PAGES &&
			    progress.physical_free <= PAGE_SIZE)
				break;
		}
	}
	active_phase = EXERCISE_AFTER_TOUCH;
	active_page = (sig_atomic_t)touched_pages;
	if (snapshot(descriptor, &after_write) != 0 ||
	    after_write.vm_page_out < before.vm_page_out ||
	    after_write.vm_page_out - before.vm_page_out <
	    MIN_PAGE_OUT_PAGES || after_write.physical_free > PAGE_SIZE) {
		(void)munmap((void *)mapping, length);
		(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("object-pressure");
	}
	/* Exercise the object-backed new-page fault and a new page-table range
	 * while no more than one physical page remains free.  pread() above
	 * established an independent expected byte without creating this VM
	 * object mapping. */
	active_phase = EXERCISE_OBJECT_SHARED;
	active_page = 0;
	errno = 0;
	object_mapping = mmap(OBJECT_MAP_ADDRESS, PAGE_SIZE, PROT_READ,
	    MAP_SHARED | MAP_FIXED_NOREPLACE, object_descriptor, 0);
	if (object_mapping == MAP_FAILED) {
		printf("BR-T46-SWAP-EXERCISE OBJECT-MMAP FAIL address=%llu "
		       "errno=%d\n",
		    (unsigned long long)(uintptr_t)OBJECT_MAP_ADDRESS, errno);
		fflush(stdout);
		(void)munmap((void *)mapping, length);
		(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("object-mmap");
	}
	if (object_mapping[0] != object_expected) {
		printf("BR-T46-SWAP-EXERCISE OBJECT-CONTENT FAIL offset=0 "
		       "actual=%u expected=%u\n", (unsigned)object_mapping[0],
		    (unsigned)object_expected);
		fflush(stdout);
		(void)munmap((void *)object_mapping, PAGE_SIZE);
		(void)munmap((void *)mapping, length);
		(void)close(object_descriptor);
		(void)close(descriptor);
		return failure("object-content");
	}
	{
		struct vm_statistics object_statistics;

		if (snapshot(descriptor, &object_statistics) != 0) {
			(void)munmap((void *)object_mapping, PAGE_SIZE);
			(void)munmap((void *)mapping, length);
			(void)close(object_descriptor);
			(void)close(descriptor);
			return failure("object-statistics");
		}
		printf("BR-T46-SWAP-EXERCISE OBJECT-SHARED PASS free=%llu "
		       "page-in=%llu page-out=%llu\n",
		    (unsigned long long)object_statistics.physical_free,
		    (unsigned long long)(object_statistics.vm_page_in -
		    before.vm_page_in),
		    (unsigned long long)(object_statistics.vm_page_out -
		    before.vm_page_out));
		fflush(stdout);
	}
	(void)munmap((void *)object_mapping, PAGE_SIZE);
	(void)close(object_descriptor);
	/* The earliest pages are the oldest reclaim candidates.  Reading the
	 * complete mapping from the beginning faults them back in and verifies
	 * that the source-specific swap I/O preserved their contents. */
	for (page = 0; page < touched_pages; page++) {
		active_phase = EXERCISE_READ;
		active_page = (sig_atomic_t)page;
		if (mapping[page * PAGE_SIZE] != page_pattern(page)) {
			(void)munmap((void *)mapping, length);
			(void)close(descriptor);
			return failure("data-integrity");
		}
	}
	active_phase = EXERCISE_AFTER_READ;
	active_page = (sig_atomic_t)touched_pages;
	if (snapshot(descriptor, &after_read) != 0 ||
	    after_read.vm_page_in <= before.vm_page_in) {
		(void)munmap((void *)mapping, length);
		(void)close(descriptor);
		return failure("page-in");
	}
	printf("BR-T46-SWAP-EXERCISE PASS bytes=%llu page-in=%llu "
	       "page-out=%llu swapped=%llu\n",
	    (unsigned long long)((uint64_t)touched_pages * PAGE_SIZE),
	    (unsigned long long)(after_read.vm_page_in - before.vm_page_in),
	    (unsigned long long)(after_read.vm_page_out - before.vm_page_out),
	    (unsigned long long)after_read.vm_swapped);
	(void)munmap((void *)mapping, length);
	(void)close(descriptor);
	return 0;
}
