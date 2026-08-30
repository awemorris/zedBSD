/* SWAP-T011/T012 production-ABI QEMU guest exerciser.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define PAGE_SIZE ZEDBSD_SYSTEM_SWAP_PAGE_SIZE
#define SOURCE_COUNT ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT
#define FILE_SOURCE "boot0:swapfile"
#define FILE_ALIAS "boot0:/swapfile"
#define BAD_SOURCE "boot0:badswap"
#define RAW_SOURCE "/dev/sda4"
#define UNSUPPORTED_SOURCE "/unsupported.swap"
#define READY_BYTE 'R'
#define CONTINUE_BYTE 'C'
#define CANCEL_BYTE 'X'
#define PRESSURE_EXEC_HEADROOM_PAGES 1024U
#define PRESSURE_DRAIN_MARGIN_PAGES 64U

struct pool_snapshot {
	struct system_swap_source_info source[SOURCE_COUNT];
	struct vm_statistics statistics;
};

struct pressure_process {
	pid_t pid;
	int start;
	int ready;
	int proceed;
};

static int system_descriptor = -1;
static volatile sig_atomic_t pressure_active_page;

static size_t
signal_append_text(char *output, size_t at, size_t capacity, const char *text)
{
	while (*text != '\0' && at < capacity)
		output[at++] = *text++;
	return at;
}

static size_t
signal_append_number(char *output, size_t at, size_t capacity, uint64_t value)
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

static void
pressure_fault(int signal_number)
{
	struct vm_statistics statistics;
	char output[320];
	size_t at = 0;

	at = signal_append_text(output, at, sizeof(output),
	    "WS016-SWAP PRESSURE SIGNAL signal=");
	at = signal_append_number(output, at, sizeof(output),
	    (uint64_t)signal_number);
	at = signal_append_text(output, at, sizeof(output), " page=");
	at = signal_append_number(output, at, sizeof(output),
	    (uint64_t)pressure_active_page);
	if (at < sizeof(output))
		output[at++] = '\n';
	(void)write(STDOUT_FILENO, output, at);
	at = 0;
	at = signal_append_text(output, at, sizeof(output),
	    "WS016-SWAP PRESSURE SIGNAL-VM ");
	memset(&statistics, 0, sizeof(statistics));
	if (system_descriptor >= 0 && ioctl(system_descriptor,
	    ZEDBSD_SYSTEM_GET_VMSTAT, &statistics) == 0) {
		at = signal_append_text(output, at, sizeof(output), "free=");
		at = signal_append_number(output, at, sizeof(output),
		    statistics.physical_free);
		at = signal_append_text(output, at, sizeof(output), " page-out=");
		at = signal_append_number(output, at, sizeof(output),
		    statistics.vm_page_out);
		at = signal_append_text(output, at, sizeof(output), " swapped=");
		at = signal_append_number(output, at, sizeof(output),
		    statistics.vm_swapped);
		at = signal_append_text(output, at, sizeof(output), " swap-free=");
		at = signal_append_number(output, at, sizeof(output),
		    statistics.swap_free);
		at = signal_append_text(output, at, sizeof(output), " io-errors=");
		at = signal_append_number(output, at, sizeof(output),
		    statistics.vm_io_errors);
	} else
		at = signal_append_text(output, at, sizeof(output), "unavailable");
	if (at < sizeof(output))
		output[at++] = '\n';
	(void)write(STDOUT_FILENO, output, at);
	_exit(128 + signal_number);
}

static int
failure(const char *stage)
{
	printf("WS016-SWAP FAIL stage=%s errno=%d\n", stage, errno);
	fflush(stdout);
	return 1;
}

static int
get_statistics(struct vm_statistics *statistics)
{
	memset(statistics, 0, sizeof(*statistics));
	return ioctl(system_descriptor, ZEDBSD_SYSTEM_GET_VMSTAT, statistics);
}

static int
get_source(unsigned source_id, struct system_swap_source_info *source)
{
	memset(source, 0, sizeof(*source));
	source->version = ZEDBSD_SYSTEM_SWAP_VERSION;
	source->struct_size = (uint32_t)sizeof(*source);
	source->source_id = source_id;
	return ioctl(system_descriptor, ZEDBSD_SYSTEM_GET_SWAP_SOURCE, source);
}

static int
pool_statistics_stable(const struct vm_statistics *before,
		       const struct vm_statistics *after)
{
	return before->vm_page_in == after->vm_page_in &&
	    before->vm_page_out == after->vm_page_out &&
	    before->vm_swapped == after->vm_swapped &&
	    before->swap_total == after->swap_total &&
	    before->swap_free == after->swap_free &&
	    before->swap_extents == after->swap_extents &&
	    before->vm_commit_limit == after->vm_commit_limit &&
	    before->vm_commit_used == after->vm_commit_used &&
	    before->vm_commit_available == after->vm_commit_available;
}

static int
get_pool(struct pool_snapshot *pool)
{
	struct vm_statistics before, after;
	unsigned attempt, source_id;

	for (attempt = 0; attempt < 200U; attempt++) {
		memset(pool, 0, sizeof(*pool));
		if (get_statistics(&before) != 0)
			return -1;
		for (source_id = 0; source_id < SOURCE_COUNT; source_id++)
			if (get_source(source_id, &pool->source[source_id]) != 0)
				return -1;
		if (get_statistics(&after) != 0)
			return -1;
		if (pool_statistics_stable(&before, &after)) {
			pool->statistics = after;
			return 0;
		}
		(void)usleep(10000U);
	}
	errno = EAGAIN;
	return -1;
}

static int
source_active(const struct system_swap_source_info *source)
{
	return source->state == ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE &&
	    source->total_pages != 0 && source->used_pages <= source->total_pages;
}

static int
source_inactive(const struct system_swap_source_info *source)
{
	return source->state == ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE &&
	    source->header_version == 0 && source->total_pages == 0 &&
	    source->used_pages == 0 && source->source[0] == '\0';
}

static int
source_control_same(const struct system_swap_source_info *left,
		    const struct system_swap_source_info *right)
{
	struct system_swap_source_info left_copy = *left;
	struct system_swap_source_info right_copy = *right;

	left_copy.used_pages = 0;
	right_copy.used_pages = 0;
	return memcmp(&left_copy, &right_copy, sizeof(left_copy)) == 0;
}

static int
pool_coherent(const struct pool_snapshot *pool)
{
	uint64_t total = 0, free_pages = 0;
	unsigned source_id;

	for (source_id = 0; source_id < SOURCE_COUNT; source_id++) {
		const struct system_swap_source_info *source =
		    &pool->source[source_id];

		if (source_active(source)) {
			total += source->total_pages;
			free_pages += source->total_pages - source->used_pages;
		} else if (!source_inactive(source))
			return 0;
	}
	return pool->statistics.swap_total == total &&
	    pool->statistics.swap_free == free_pages &&
	    pool->statistics.vm_commit_used <=
	    pool->statistics.vm_commit_limit &&
	    pool->statistics.vm_commit_available ==
	    pool->statistics.vm_commit_limit -
	    pool->statistics.vm_commit_used;
}

static int
pool_control_same(const struct pool_snapshot *left,
		  const struct pool_snapshot *right)
{
	unsigned source_id;

	for (source_id = 0; source_id < SOURCE_COUNT; source_id++)
		if (!source_control_same(&left->source[source_id],
		    &right->source[source_id]))
			return 0;
	return left->statistics.swap_total == right->statistics.swap_total &&
	    left->statistics.swap_extents == right->statistics.swap_extents &&
	    left->statistics.vm_commit_limit ==
	    right->statistics.vm_commit_limit;
}

static int
wait_child(pid_t child)
{
	int status;
	pid_t result;

	do {
		result = waitpid(child, &status, 0);
	} while (result < 0 && errno == EINTR);
	if (result != child || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

static int
run_command(const char *path, const char *source, int nonroot)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return -1;
	if (child == 0) {
		if (nonroot && setuid((uid_t)65534) != 0)
			_exit(125);
		execl(path, path, source, (char *)NULL);
		_exit(126);
	}
	status = wait_child(child);
	printf("WS016-SWAP COMMAND path=%s source=%s uid=%s status=%d\n", path,
	    source, nonroot ? "nonroot" : "root", status);
	fflush(stdout);
	return status;
}

static int
make_control(struct system_swap_control *control, const char *source)
{
	size_t length = strlen(source);

	if (length == 0 || length >= sizeof(control->source))
		return -1;
	memset(control, 0, sizeof(*control));
	control->version = ZEDBSD_SYSTEM_SWAP_VERSION;
	control->struct_size = (uint32_t)sizeof(*control);
	memcpy(control->source, source, length + 1U);
	return 0;
}

static int
control_error(unsigned long request, const char *source)
{
	struct system_swap_control control;

	if (make_control(&control, source) != 0)
		return EINVAL;
	errno = 0;
	if (ioctl(system_descriptor, request, &control) == 0)
		return 0;
	return errno;
}

static int
write_result(int descriptor, int result)
{
	const unsigned char *bytes = (const unsigned char *)&result;
	size_t done = 0;

	while (done != sizeof(result)) {
		ssize_t count = write(descriptor, bytes + done,
		    sizeof(result) - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}
	return 0;
}

static int
read_result(int descriptor, int *result)
{
	unsigned char *bytes = (unsigned char *)result;
	size_t done = 0;

	while (done != sizeof(*result)) {
		ssize_t count = read(descriptor, bytes + done,
		    sizeof(*result) - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		done += (size_t)count;
	}
	return 0;
}

static int
nonroot_control_error(unsigned long request, const char *source)
{
	int result_pipe[2], result = -1;
	pid_t child;

	if (pipe(result_pipe) != 0)
		return -1;
	child = fork();
	if (child < 0) {
		(void)close(result_pipe[0]);
		(void)close(result_pipe[1]);
		return -1;
	}
	if (child == 0) {
		int child_result;

		(void)close(result_pipe[0]);
		child_result = setuid((uid_t)65534) == 0 ?
		    control_error(request, source) : -1;
		(void)write_result(result_pipe[1], child_result);
		(void)close(result_pipe[1]);
		_exit(0);
	}
	(void)close(result_pipe[1]);
	if (read_result(result_pipe[0], &result) != 0 || wait_child(child) != 0)
		result = -1;
	(void)close(result_pipe[0]);
	return result;
}

static int
expect_ioctl_failure_atomic(const char *name, unsigned long request,
			    const char *source, int expected, int nonroot)
{
	struct pool_snapshot before, after;
	int actual;

	if (get_pool(&before) != 0)
		return failure(name);
	actual = nonroot ? nonroot_control_error(request, source) :
	    control_error(request, source);
	if (get_pool(&after) != 0 || actual != expected ||
	    !pool_control_same(&before, &after) || !pool_coherent(&before) ||
	    !pool_coherent(&after)) {
		printf("WS016-SWAP NEGATIVE FAIL case=%s actual=%d expected=%d\n",
		    name, actual, expected);
		fflush(stdout);
		return failure(name);
	}
	printf("WS016-SWAP NEGATIVE PASS case=%s errno=%d\n", name, actual);
	fflush(stdout);
	return 0;
}

static int
expect_command_failure_atomic(const char *name, const char *path,
			      const char *source, int nonroot)
{
	struct pool_snapshot before, after;
	unsigned attempt;
	int status;

	if (get_pool(&before) != 0)
		return failure(name);
	memset(&after, 0, sizeof(after));
	status = run_command(path, source, nonroot);
	for (attempt = 0; attempt < 200U; attempt++) {
		if (get_pool(&after) == 0 &&
		    pool_control_same(&before, &after) &&
		    pool_coherent(&before) && pool_coherent(&after))
			break;
		(void)usleep(10000U);
	}
	if (status != 1 || attempt == 200U) {
		printf("WS016-SWAP COMMAND NEGATIVE FAIL case=%s status=%d "
		       "before-total=%llu after-total=%llu before-limit=%llu "
		       "after-limit=%llu before-used=%llu after-used=%llu\n",
		    name, status,
		    (unsigned long long)before.statistics.swap_total,
		    (unsigned long long)after.statistics.swap_total,
		    (unsigned long long)before.statistics.vm_commit_limit,
		    (unsigned long long)after.statistics.vm_commit_limit,
		    (unsigned long long)before.statistics.vm_commit_used,
		    (unsigned long long)after.statistics.vm_commit_used);
		fflush(stdout);
		return failure(name);
	}
	printf("WS016-SWAP COMMAND NEGATIVE PASS case=%s\n", name);
	fflush(stdout);
	return 0;
}

static unsigned char
page_pattern(size_t page, size_t offset, uint32_t generation)
{
	uint64_t value = ((uint64_t)page + 1U) * 0x9e3779b185ebca87ULL;

	value ^= ((uint64_t)offset + 1U) * 0xc2b2ae3d27d4eb4fULL;
	value ^= (uint64_t)generation * 0x165667b19e3779f9ULL;
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdULL;
	value ^= value >> 33;
	return (unsigned char)value;
}

static void
write_page_pattern(volatile unsigned char *page, size_t page_index,
		   uint32_t generation)
{
	static const uint16_t offset[] = {
		0, 1, 7, 31, 63, 127, 255, 511,
		767, 1023, 1535, 2047, 2559, 3071, 3583, 4095
	};
	size_t index;

	for (index = 0; index < sizeof(offset) / sizeof(offset[0]); index++)
		page[offset[index]] = page_pattern(page_index, offset[index],
		    generation);
}

static int
check_page_pattern(const volatile unsigned char *page, size_t page_index,
		   uint32_t generation)
{
	static const uint16_t offset[] = {
		0, 1, 7, 31, 63, 127, 255, 511,
		767, 1023, 1535, 2047, 2559, 3071, 3583, 4095
	};
	size_t index;

	for (index = 0; index < sizeof(offset) / sizeof(offset[0]); index++)
		if (page[offset[index]] != page_pattern(page_index,
		    offset[index], generation))
			return 0;
	return 1;
}

static int
write_byte(int descriptor, char byte)
{
	ssize_t count;

	do {
		count = write(descriptor, &byte, 1);
	} while (count < 0 && errno == EINTR);
	return count == 1 ? 0 : -1;
}

static int
read_byte(int descriptor, char expected)
{
	char byte;
	ssize_t count;

	do {
		count = read(descriptor, &byte, 1);
	} while (count < 0 && errno == EINTR);
	return count == 1 && byte == expected ? 0 : -1;
}

static int
pressure_worker(int ready, int proceed, uint32_t additional0,
		uint32_t additional1, int fill0, uint32_t minimum_page_in,
		uint32_t generation)
{
	struct system_swap_source_info source0, source1;
	struct vm_statistics before, after;
	volatile unsigned char *mapping;
	uint64_t extra_pages, target0, target1, wanted, maximum;
	size_t base_page = 0, length, pages, page, touched = 0;
	uint32_t initial1, reported0, reported1;
	int reached = 0, source1_latched = 0;

	if (signal(SIGSEGV, pressure_fault) == SIG_ERR ||
	    signal(SIGBUS, pressure_fault) == SIG_ERR)
		return failure("pressure-signal-handler");
	if (get_statistics(&before) != 0 || get_source(0, &source0) != 0 ||
	    get_source(1, &source1) != 0)
		return failure("pressure-initial-snapshot");
	if ((uint64_t)source0.used_pages + additional0 > UINT32_MAX ||
	    (uint64_t)source1.used_pages + additional1 > UINT32_MAX)
		return failure("pressure-target-overflow");
	target0 = fill0 ? source0.total_pages :
	    (uint64_t)source0.used_pages + additional0;
	target1 = (uint64_t)source1.used_pages + additional1;
	initial1 = source1.used_pages;
	reported0 = source0.used_pages;
	reported1 = source1.used_pages;
	extra_pages = (fill0 ? source0.total_pages - source0.used_pages :
	    additional0) + (uint64_t)additional1 + 1024U;
	wanted = before.physical_free + extra_pages * PAGE_SIZE;
	maximum = before.vm_commit_available > 16U * PAGE_SIZE ?
	    before.vm_commit_available - 16U * PAGE_SIZE : 0;
	if (wanted > maximum)
		wanted = maximum;
	wanted &= ~((uint64_t)PAGE_SIZE - 1U);
	if (wanted <= before.physical_free || wanted > (uint64_t)(size_t)-1)
		return failure("pressure-size");
	printf("WS016-SWAP PRESSURE BEGIN generation=%u free=%llu "
	       "commit-available=%llu wanted=%llu target0=%llu target1=%llu\n",
	    generation, (unsigned long long)before.physical_free,
	    (unsigned long long)before.vm_commit_available,
	    (unsigned long long)wanted, (unsigned long long)target0,
	    (unsigned long long)target1);
	fflush(stdout);
	length = (size_t)wanted;
	pages = length / PAGE_SIZE;
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return failure("pressure-mmap");
	printf("WS016-SWAP PRESSURE MMAP PASS generation=%u pages=%llu\n",
	    generation, (unsigned long long)pages);
	fflush(stdout);
	for (page = 0; page < pages; page++) {
		struct vm_statistics progress;

		pressure_active_page = (sig_atomic_t)page;
		write_page_pattern(mapping + page * PAGE_SIZE, page, generation);
		touched = page + 1U;
		if ((page & 1023U) == 1023U) {
			if (get_statistics(&progress) != 0 ||
			    get_source(0, &source0) != 0 ||
			    get_source(1, &source1) != 0) {
				(void)munmap((void *)mapping, length);
				return failure("pressure-progress-snapshot");
			}
			printf("WS016-SWAP PRESSURE TOUCH generation=%u pages=%llu "
			       "free=%llu page-out=%llu io-errors=%llu used0=%u "
			       "used1=%u\n",
			    generation, (unsigned long long)touched,
			    (unsigned long long)progress.physical_free,
			    (unsigned long long)(progress.vm_page_out -
			    before.vm_page_out),
			    (unsigned long long)(progress.vm_io_errors -
			    before.vm_io_errors), source0.used_pages,
			    source1.used_pages);
			fflush(stdout);
			reported0 = source0.used_pages;
			reported1 = source1.used_pages;
		}
		if (!fill0 && (page & 63U) != 63U)
			continue;
		if (get_source(0, &source0) != 0 || get_source(1, &source1) != 0)
			break;
		if (fill0 && !source1_latched && source1.used_pages > initial1) {
			if (source0.used_pages != source0.total_pages) {
				(void)munmap((void *)mapping, length);
				return failure("pressure-source-order");
			}
			source1_latched = 1;
			printf("WS016-SWAP NUMERIC FIRST PASS full0=%u first-used1=%u\n",
			    source0.used_pages, source1.used_pages);
			fflush(stdout);
		}
		if ((source0.used_pages != reported0 ||
		    source1.used_pages != reported1) &&
		    (page & 63U) == 63U && (page & 1023U) != 1023U) {
			struct vm_statistics progress;

			if (get_statistics(&progress) != 0)
				break;
			printf("WS016-SWAP PRESSURE SWAP generation=%u pages=%llu "
			       "free=%llu page-out=%llu io-errors=%llu used0=%u "
			       "used1=%u\n", generation,
			    (unsigned long long)touched,
			    (unsigned long long)progress.physical_free,
			    (unsigned long long)(progress.vm_page_out -
			    before.vm_page_out),
			    (unsigned long long)(progress.vm_io_errors -
			    before.vm_io_errors), source0.used_pages,
			    source1.used_pages);
			fflush(stdout);
			reported0 = source0.used_pages;
			reported1 = source1.used_pages;
		}
		reached = source0.used_pages >= target0 &&
		    source1.used_pages >= target1 &&
		    (!fill0 || source0.used_pages == source0.total_pages);
		if (reached)
			break;
	}
	if (!reached || (fill0 && !source1_latched) ||
	    get_source(0, &source0) != 0 ||
	    get_source(1, &source1) != 0) {
		(void)munmap((void *)mapping, length);
		return failure("pressure-target");
	}
	if (fill0) {
		struct vm_statistics headroom;
		size_t prefix_pages = PRESSURE_EXEC_HEADROOM_PAGES;
		size_t prefix_length = prefix_pages * PAGE_SIZE;

		printf("WS016-SWAP NUMERIC ORDER PASS full0=%u used1=%u\n",
		    source0.used_pages, source1.used_pages);
		fflush(stdout);
		if (touched <= prefix_pages) {
			(void)munmap((void *)mapping, length);
			return failure("pressure-headroom-size");
		}
		for (page = 0; page < prefix_pages; page++)
			if (!check_page_pattern(mapping + page * PAGE_SIZE, page,
			    generation)) {
				(void)munmap((void *)mapping, length);
				return failure("pressure-prefix-integrity");
			}
		printf("WS016-SWAP PRESSURE PREFIX READBACK PASS generation=%u "
		       "pages=%llu\n", generation,
		    (unsigned long long)prefix_pages);
		fflush(stdout);
		if (munmap((void *)mapping, prefix_length) != 0) {
			(void)munmap((void *)mapping, length);
			return failure("pressure-headroom-munmap");
		}
		mapping += prefix_length;
		length -= prefix_length;
		touched -= prefix_pages;
		base_page = prefix_pages;
		if (get_statistics(&headroom) != 0 ||
		    get_source(0, &source0) != 0 || get_source(1, &source1) != 0 ||
		    headroom.physical_free < prefix_length ||
		    headroom.vm_io_errors != before.vm_io_errors ||
		    !source_active(&source0) ||
		    source0.used_pages < PRESSURE_DRAIN_MARGIN_PAGES ||
		    !source_active(&source1)) {
			(void)munmap((void *)mapping, length);
			return failure("pressure-headroom-state");
		}
		printf("WS016-SWAP PRESSURE HEADROOM PASS generation=%u "
		       "prefix=%llu retained=%llu free=%llu used0=%u used1=%u\n",
		    generation, (unsigned long long)prefix_pages,
		    (unsigned long long)touched,
		    (unsigned long long)headroom.physical_free,
		    source0.used_pages, source1.used_pages);
		fflush(stdout);
	}
	printf("WS016-SWAP PRESSURE READY generation=%u pages=%llu used0=%u "
	       "total0=%u used1=%u total1=%u\n", generation,
	    (unsigned long long)touched,
	    source0.used_pages, source0.total_pages, source1.used_pages,
	    source1.total_pages);
	fflush(stdout);
	if (write_byte(ready, READY_BYTE) != 0 ||
	    read_byte(proceed, CONTINUE_BYTE) != 0) {
		(void)munmap((void *)mapping, length);
		return failure("pressure-coordination");
	}
	for (page = 0; page < touched; page++)
		if (!check_page_pattern(mapping + page * PAGE_SIZE,
		    page + base_page,
		    generation)) {
			(void)munmap((void *)mapping, length);
			return failure("pressure-data-integrity");
		}
	if (get_statistics(&after) != 0 || after.vm_page_in < before.vm_page_in ||
	    after.vm_page_in - before.vm_page_in < minimum_page_in ||
	    after.vm_page_out < before.vm_page_out ||
	    after.vm_io_errors != before.vm_io_errors) {
		(void)munmap((void *)mapping, length);
		return failure("pressure-statistics");
	}
	printf("WS016-SWAP PRESSURE READBACK PASS generation=%u pages=%llu "
	       "page-in=%llu page-out=%llu\n", generation,
	    (unsigned long long)touched,
	    (unsigned long long)(after.vm_page_in - before.vm_page_in),
	    (unsigned long long)(after.vm_page_out - before.vm_page_out));
	fflush(stdout);
	if (munmap((void *)mapping, length) != 0)
		return failure("pressure-munmap");
	return 0;
}

static int
pressure_prepare(struct pressure_process *process, uint32_t minimum0,
		 uint32_t minimum1, int fill0, uint32_t minimum_page_in,
		 uint32_t generation)
{
	int start_pipe[2], ready_pipe[2], proceed_pipe[2];
	pid_t child;

	memset(process, 0, sizeof(*process));
	process->start = process->ready = process->proceed = -1;
	if (pipe(start_pipe) != 0) {
		(void)failure("pressure-start-pipe");
		return -1;
	}
	if (pipe(ready_pipe) != 0) {
		(void)close(start_pipe[0]);
		(void)close(start_pipe[1]);
		(void)failure("pressure-ready-pipe");
		return -1;
	}
	if (pipe(proceed_pipe) != 0) {
		(void)close(start_pipe[0]);
		(void)close(start_pipe[1]);
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		(void)failure("pressure-proceed-pipe");
		return -1;
	}
	child = fork();
	if (child < 0) {
		(void)close(start_pipe[0]);
		(void)close(start_pipe[1]);
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		(void)close(proceed_pipe[0]);
		(void)close(proceed_pipe[1]);
		(void)failure("pressure-fork");
		return -1;
	}
	if (child == 0) {
		int status;

		(void)close(start_pipe[1]);
		(void)close(ready_pipe[0]);
		(void)close(proceed_pipe[1]);
		if (read_byte(start_pipe[0], CONTINUE_BYTE) != 0)
			_exit(124);
		(void)close(start_pipe[0]);
		status = pressure_worker(ready_pipe[1], proceed_pipe[0], minimum0,
		    minimum1, fill0, minimum_page_in, generation);
		(void)close(ready_pipe[1]);
		(void)close(proceed_pipe[0]);
		_exit(status);
	}
	(void)close(start_pipe[0]);
	(void)close(ready_pipe[1]);
	(void)close(proceed_pipe[0]);
	process->pid = child;
	process->start = start_pipe[1];
	process->ready = ready_pipe[0];
	process->proceed = proceed_pipe[1];
	return 0;
}

static void
pressure_cancel_prepared(struct pressure_process *process)
{
	if (process->start >= 0) {
		(void)write_byte(process->start, CANCEL_BYTE);
		(void)close(process->start);
		process->start = -1;
	}
	if (process->ready >= 0) {
		(void)close(process->ready);
		process->ready = -1;
	}
	if (process->proceed >= 0) {
		(void)close(process->proceed);
		process->proceed = -1;
	}
	if (process->pid > 0) {
		(void)wait_child(process->pid);
		process->pid = 0;
	}
}

static int
pressure_release(struct pressure_process *process)
{
	pid_t child = process->pid;

	if (child <= 0 || process->start < 0 || process->ready < 0 ||
	    process->proceed < 0)
		return -1;
	if (write_byte(process->start, CONTINUE_BYTE) != 0) {
		pressure_cancel_prepared(process);
		(void)failure("pressure-start");
		return -1;
	}
	(void)close(process->start);
	process->start = -1;
	if (read_byte(process->ready, READY_BYTE) != 0) {
		int saved_error = errno;
		int child_status = -1, raw_status = 0;
		pid_t waited;

		(void)close(process->ready);
		(void)close(process->proceed);
		process->ready = process->proceed = -1;
		do {
			waited = waitpid(child, &raw_status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited == child && WIFEXITED(raw_status))
			child_status = WEXITSTATUS(raw_status);
		errno = saved_error;
		printf("WS016-SWAP PRESSURE CHILD FAIL status=%d raw=%d",
		    child_status, raw_status);
		if (waited == child && WIFSIGNALED(raw_status))
			printf(" signal=%d", WTERMSIG(raw_status));
		printf("\n");
		fflush(stdout);
		process->pid = 0;
		(void)failure("pressure-ready");
		return -1;
	}
	(void)close(process->ready);
	process->ready = -1;
	return 0;
}

static int
pressure_start(struct pressure_process *process, uint32_t minimum0,
	       uint32_t minimum1, int fill0, uint32_t minimum_page_in,
	       uint32_t generation)
{
	if (pressure_prepare(process, minimum0, minimum1, fill0,
	    minimum_page_in, generation) != 0)
		return -1;
	return pressure_release(process);
}

static int
pressure_finish(struct pressure_process *process)
{
	int status;

	if (write_byte(process->proceed, CONTINUE_BYTE) != 0) {
		(void)close(process->proceed);
		(void)wait_child(process->pid);
		return -1;
	}
	(void)close(process->proceed);
	process->proceed = -1;
	status = wait_child(process->pid);
	return status == 0 ? 0 : -1;
}

static int
reservation_worker(int ready, int proceed)
{
	struct vm_statistics statistics;
	void *mapping;
	uint64_t bytes, remaining_limit, swap_bytes;

	if (get_statistics(&statistics) != 0)
		return failure("reservation-statistics");
	if (statistics.swap_total > UINT64_MAX / PAGE_SIZE)
		return failure("reservation-swap-overflow");
	swap_bytes = statistics.swap_total * PAGE_SIZE;
	if (statistics.vm_commit_limit < swap_bytes)
		return failure("reservation-commit-limit");
	remaining_limit = statistics.vm_commit_limit - swap_bytes;
	bytes = remaining_limit >= statistics.vm_commit_used ?
	    remaining_limit - statistics.vm_commit_used + PAGE_SIZE : PAGE_SIZE;
	bytes = (bytes + PAGE_SIZE - 1U) & ~((uint64_t)PAGE_SIZE - 1U);
	if (bytes > statistics.vm_commit_available ||
	    bytes > (uint64_t)(size_t)-1)
		return failure("reservation-size");
	mapping = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED || get_statistics(&statistics) != 0 ||
	    statistics.vm_commit_used <= remaining_limit)
		return failure("reservation-mmap");
	if (write_byte(ready, READY_BYTE) != 0 ||
	    read_byte(proceed, CONTINUE_BYTE) != 0)
		return failure("reservation-coordination");
	return munmap(mapping, (size_t)bytes) == 0 ? 0 :
	    failure("reservation-munmap");
}

static int
unsafe_remove_test(void)
{
	struct pool_snapshot before, after;
	int ready_pipe[2], proceed_pipe[2];
	pid_t child;

	if (pipe(ready_pipe) != 0)
		return failure("reservation-pipe");
	if (pipe(proceed_pipe) != 0) {
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		return failure("reservation-pipe");
	}
	child = fork();
	if (child < 0) {
		(void)close(ready_pipe[0]);
		(void)close(ready_pipe[1]);
		(void)close(proceed_pipe[0]);
		(void)close(proceed_pipe[1]);
		return failure("reservation-fork");
	}
	if (child == 0) {
		int status;

		(void)close(ready_pipe[0]);
		(void)close(proceed_pipe[1]);
		status = reservation_worker(ready_pipe[1], proceed_pipe[0]);
		_exit(status);
	}
	(void)close(ready_pipe[1]);
	(void)close(proceed_pipe[0]);
	if (read_byte(ready_pipe[0], READY_BYTE) != 0 ||
	    get_pool(&before) != 0 ||
	    control_error(ZEDBSD_SYSTEM_SWAP_REMOVE, FILE_SOURCE) != ENOMEM ||
	    get_pool(&after) != 0 ||
	    !pool_control_same(&before, &after) || !pool_coherent(&before) ||
	    !pool_coherent(&after)) {
		(void)write_byte(proceed_pipe[1], CONTINUE_BYTE);
		(void)wait_child(child);
		(void)close(ready_pipe[0]);
		(void)close(proceed_pipe[1]);
		return failure("unsafe-commit-removal");
	}
	if (write_byte(proceed_pipe[1], CONTINUE_BYTE) != 0 ||
	    wait_child(child) != 0)
		return failure("reservation-release");
	(void)close(ready_pipe[0]);
	(void)close(proceed_pipe[1]);
	printf("WS016-SWAP NEGATIVE PASS case=unsafe-commit-removal errno=%d\n",
	    ENOMEM);
	fflush(stdout);
	return 0;
}

static int
all_inactive(const struct pool_snapshot *pool)
{
	unsigned source_id;

	for (source_id = 0; source_id < SOURCE_COUNT; source_id++)
		if (!source_inactive(&pool->source[source_id]))
			return 0;
	return pool->statistics.swap_total == 0 &&
	    pool->statistics.swap_free == 0 && pool_coherent(pool);
}

static int
file_scenario(void)
{
	struct pool_snapshot initial, active, removed;
	struct system_swap_source_info source;
	struct pressure_process pressure;
	struct vm_statistics before_pressure, after_pressure;

	if (get_pool(&initial) != 0 || !all_inactive(&initial) ||
	    !pool_coherent(&initial))
		return failure("file-initial-pool");
	if (run_command("/sbin/swapon", FILE_SOURCE, 0) != 0 ||
	    get_pool(&active) != 0 || !source_active(&active.source[0]) ||
	    !pool_coherent(&active) ||
	    strcmp(active.source[0].source, FILE_SOURCE) != 0 ||
	    active.source[0].header_version != 2 ||
	    active.statistics.swap_total != active.source[0].total_pages ||
	    active.statistics.vm_commit_limit !=
	    initial.statistics.vm_commit_limit +
	    (uint64_t)active.source[0].total_pages * PAGE_SIZE)
		return failure("file-add");
	printf("WS016-SWAP FILE ADD PASS id=0 pages=%u label=%s\n",
	    active.source[0].total_pages, active.source[0].label);
	fflush(stdout);
	if (get_statistics(&before_pressure) != 0 ||
	    pressure_start(&pressure, 1024U, 0, 0, 1024U, 1U) != 0 ||
	    get_source(0, &source) != 0 || source.used_pages < 1024U ||
	    pressure_finish(&pressure) != 0 ||
	    get_statistics(&after_pressure) != 0 ||
	    after_pressure.vm_page_out < before_pressure.vm_page_out ||
	    after_pressure.vm_page_out - before_pressure.vm_page_out < 1024U ||
	    after_pressure.vm_page_in < before_pressure.vm_page_in ||
	    after_pressure.vm_page_in - before_pressure.vm_page_in < 1024U ||
	    after_pressure.vm_io_errors != before_pressure.vm_io_errors)
		return failure("file-pressure");
	if (get_pool(&active) != 0 || !pool_coherent(&active) ||
	    active.source[0].used_pages != 0 ||
	    run_command("/sbin/swapoff", FILE_SOURCE, 0) != 0 ||
	    get_pool(&removed) != 0 || !all_inactive(&removed) ||
	    removed.statistics.vm_commit_limit !=
	    initial.statistics.vm_commit_limit)
		return failure("file-remove");
	if (run_command("/sbin/swapon", FILE_SOURCE, 0) != 0 ||
	    get_source(0, &source) != 0 || !source_active(&source) ||
	    pressure_start(&pressure, 1024U, 0, 0, 1024U, 2U) != 0 ||
	    pressure_finish(&pressure) != 0)
		return failure("file-reuse-pressure");
	if (expect_ioctl_failure_atomic("duplicate-alias",
	    ZEDBSD_SYSTEM_SWAP_ADD, FILE_ALIAS, EEXIST, 0) != 0 ||
	    expect_ioctl_failure_atomic("malformed-header",
	    ZEDBSD_SYSTEM_SWAP_ADD, BAD_SOURCE, EINVAL, 0) != 0 ||
	    expect_ioctl_failure_atomic("unknown-removal",
	    ZEDBSD_SYSTEM_SWAP_REMOVE, BAD_SOURCE, ENOENT, 0) != 0 ||
	    expect_ioctl_failure_atomic("nonroot-control",
	    ZEDBSD_SYSTEM_SWAP_ADD, FILE_SOURCE, EPERM, 1) != 0 ||
	    expect_command_failure_atomic("nonroot-command", "/sbin/swapon",
	    FILE_SOURCE, 1) != 0 || unsafe_remove_test() != 0)
		return 1;
	if (run_command("/sbin/swapoff", FILE_SOURCE, 0) != 0 ||
	    get_pool(&removed) != 0 || !all_inactive(&removed) ||
	    removed.statistics.vm_commit_limit !=
	    initial.statistics.vm_commit_limit)
		return failure("file-reuse");
	printf("WS016-SWAP FILE REUSE PASS id=0\n");
	printf("WS016-SWAP PASS scenario=file\n");
	fflush(stdout);
	return 0;
}

static int
mixed_scenario(void)
{
	struct pool_snapshot initial, pool, drained;
	struct system_swap_source_info source0, source1;
	struct pressure_process old_pressure, skip_pressure, new_pressure;
	struct vm_statistics before, after;
	uint32_t boot_pages, raw_before_skip;
	uint64_t physical_commit_limit;

	if (get_pool(&initial) != 0 || !pool_coherent(&initial) ||
	    !source_active(&initial.source[0]) ||
	    strcmp(initial.source[0].source, FILE_SOURCE) != 0 ||
	    !source_inactive(&initial.source[1]))
		return failure("mixed-initial-pool");
	boot_pages = initial.source[0].total_pages;
	if (initial.statistics.vm_commit_limit <
	    (uint64_t)boot_pages * PAGE_SIZE)
		return failure("mixed-initial-commit");
	physical_commit_limit = initial.statistics.vm_commit_limit -
	    (uint64_t)boot_pages * PAGE_SIZE;
	if (run_command("/sbin/swapon", RAW_SOURCE, 0) != 0 ||
	    get_pool(&pool) != 0 || !pool_coherent(&pool) ||
	    !source_active(&pool.source[0]) ||
	    !source_active(&pool.source[1]) ||
	    strcmp(pool.source[1].source, RAW_SOURCE) != 0 ||
	    pool.source[1].header_version != 2 ||
	    pool.statistics.vm_commit_limit !=
	    initial.statistics.vm_commit_limit +
	    (uint64_t)pool.source[1].total_pages * PAGE_SIZE)
		return failure("mixed-raw-add");
	printf("WS016-SWAP MIXED ADD PASS id0=%s pages0=%u id1=%s pages1=%u "
	       "label1=%s\n", pool.source[0].source,
	    pool.source[0].total_pages, pool.source[1].source,
	    pool.source[1].total_pages, pool.source[1].label);
	fflush(stdout);
	if (get_statistics(&before) != 0)
		return failure("mixed-pressure-snapshot");
	if (pressure_prepare(&skip_pressure, 0, 256U, 0, 0, 34U) != 0) {
		return failure("mixed-skip-prepare");
	}
	if (pressure_start(&old_pressure, boot_pages, 1024U, 1, 1024U,
	    17U) != 0) {
		pressure_cancel_prepared(&skip_pressure);
		return failure("mixed-pressure-start");
	}
	if (get_source(0, &source0) != 0 || get_source(1, &source1) != 0 ||
	    !source_active(&source0) ||
	    source0.used_pages < PRESSURE_DRAIN_MARGIN_PAGES ||
	    !source_active(&source1)) {
		pressure_cancel_prepared(&skip_pressure);
		return failure("mixed-pressure-parent-snapshot");
	}
	printf("WS016-SWAP PRESSURE PARENT used0=%u total0=%u used1=%u "
	       "total1=%u\n", source0.used_pages, source0.total_pages,
	    source1.used_pages, source1.total_pages);
	printf("WS016-SWAP COMMAND PRECHECK PASS used0=%u minimum=%u\n",
	    source0.used_pages, PRESSURE_DRAIN_MARGIN_PAGES);
	fflush(stdout);
	/* The worker emits READY only after observing source 0 full and source 1
	 * at its target.  Waking this parent can page one of its own mappings back
	 * in and immediately reduce a source's used count, so the READY snapshot is
	 * the ordering proof; the parent snapshot is diagnostic, not a second
	 * equality gate. */
	if (run_command("/sbin/swapoff", FILE_SOURCE, 0) != 0) {
		pressure_cancel_prepared(&skip_pressure);
		return failure("mixed-drain-command");
	}
	if (get_pool(&drained) != 0 || !pool_coherent(&drained) ||
	    !source_inactive(&drained.source[0]) ||
	    !source_active(&drained.source[1]) ||
	    drained.statistics.vm_commit_limit != physical_commit_limit +
	    (uint64_t)drained.source[1].total_pages * PAGE_SIZE) {
		pressure_cancel_prepared(&skip_pressure);
		return failure("mixed-drain-state");
	}
	printf("WS016-SWAP COMMAND DRAIN PASS removed=0 preserved=1\n");
	printf("WS016-SWAP COMMAND DRAIN EVIDENCE state=inactive used=0 "
	       "io-errors=0 in-flight-contract=SWAP-T005\n");
	fflush(stdout);
	raw_before_skip = drained.source[1].used_pages;
	if (pressure_release(&skip_pressure) != 0 ||
	    get_source(0, &source0) != 0 || !source_inactive(&source0) ||
	    get_source(1, &source1) != 0 || !source_active(&source1) ||
	    pressure_finish(&skip_pressure) != 0)
		return failure("mixed-removed-skip");
	printf("WS016-SWAP REMOVED-SKIP PASS removed=0 active=1 "
	       "target1=%u\n", raw_before_skip + 256U);
	fflush(stdout);
	if (run_command("/sbin/swapon", FILE_SOURCE, 0) != 0 ||
	    get_pool(&pool) != 0 || !pool_coherent(&pool) ||
	    !source_active(&pool.source[0]) ||
	    !source_active(&pool.source[1]) ||
	    pool.statistics.vm_commit_limit !=
	    initial.statistics.vm_commit_limit +
	    (uint64_t)pool.source[1].total_pages * PAGE_SIZE ||
	    pressure_start(&new_pressure, 256U, 0, 0, 0, 51U) != 0 ||
	    pressure_finish(&new_pressure) != 0 ||
	    pressure_finish(&old_pressure) != 0 ||
	    get_statistics(&after) != 0 || before.vm_page_in > after.vm_page_in ||
	    after.vm_page_in - before.vm_page_in < 1024U ||
	    after.vm_io_errors != before.vm_io_errors ||
	    run_command("/sbin/swapoff", FILE_SOURCE, 0) != 0 ||
	    run_command("/sbin/swapoff", RAW_SOURCE, 0) != 0 ||
	    get_pool(&pool) != 0 || !all_inactive(&pool) ||
	    pool.statistics.vm_commit_limit != physical_commit_limit)
		return failure("mixed-id-reuse");
	printf("WS016-SWAP REUSE PASS id=0 old-generation=17 "
	       "new-generation=51 stale-token-errors=0\n");
	printf("WS016-SWAP PASS scenario=mixed\n");
	fflush(stdout);
	return 0;
}

static int
native_scenario(void)
{
	struct pool_snapshot pool;

	if (get_pool(&pool) != 0 || !pool_coherent(&pool) ||
	    !source_active(&pool.source[0]) ||
	    strcmp(pool.source[0].source, FILE_SOURCE) != 0 ||
	    expect_ioctl_failure_atomic("unsupported-backend",
	    ZEDBSD_SYSTEM_SWAP_ADD, UNSUPPORTED_SOURCE, EOPNOTSUPP, 0) != 0 ||
	    expect_ioctl_failure_atomic("root-raw-overlap",
	    ZEDBSD_SYSTEM_SWAP_ADD, RAW_SOURCE, EEXIST, 0) != 0 ||
	    expect_command_failure_atomic("root-overlap-command", "/sbin/swapon",
	    RAW_SOURCE, 0) != 0)
		return failure("native-root-overlap");
	printf("WS016-SWAP PASS scenario=native-root-negative\n");
	fflush(stdout);
	return 0;
}

static int
marker_present(const char *path)
{
	int descriptor = open(path, O_RDONLY);

	if (descriptor < 0)
		return 0;
	(void)close(descriptor);
	return 1;
}

int
main(void)
{
	struct system_swap_source_info source0;
	int status;

	system_descriptor = open("/dev/system", O_RDWR);
	if (system_descriptor < 0)
		return failure("open-system");
	printf("WS016-SWAP START\n");
	fflush(stdout);
	if (marker_present("/etc/ws016-native"))
		status = native_scenario();
	else if (get_source(0, &source0) != 0)
		status = failure("scenario-source");
	else if (source_active(&source0))
		status = mixed_scenario();
	else
		status = file_scenario();
	(void)close(system_descriptor);
	return status;
}
