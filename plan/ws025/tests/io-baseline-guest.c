/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/io-stats.h>

#define SAMPLES 101
#define REQUIRE(x) do { if (!(x)) { \
	printf("BASELINE FAIL line=%d errno=%d\n", __LINE__, errno); return 1; \
} } while (0)

static unsigned char data[65536], check[65536];
static struct io_stats before, after, delta[SAMPLES];
static uint64_t elapsed[SAMPLES], ordered[SAMPLES];

static uint64_t nanos(const struct timespec *time)
{
	return (uint64_t)time->tv_sec * UINT64_C(1000000000) + time->tv_nsec;
}

static int snapshot(struct io_stats *stats)
{
	size_t length = sizeof(*stats);
	return sysctlbyname("vfs.io.stats", stats, &length, NULL, 0) == 0 &&
	    length == sizeof(*stats) && stats->version == IO_STATS_VERSION &&
	    stats->count == IO_STAT_COUNT;
}

static void report_delta(const char *label)
{
	unsigned event;
	printf("%s", label);
	for (event = 0; event < IO_STAT_COUNT; event++)
		printf(" %u:%llu:%llu", event,
		    (unsigned long long)(after.events[event].calls - before.events[event].calls),
		    (unsigned long long)(after.events[event].bytes - before.events[event].bytes));
	printf("\n");
}

int main(void)
{
	struct memory_stats memory;
	struct timespec start, end;
	size_t length;
	uint64_t key;
	unsigned sample, event, i, j, mode;
	int fd;

	/* The diagnostic interface is readable, sized, and immutable. */
	length = 0;
	REQUIRE(sysctlbyname("vfs.io.stats", NULL, &length, NULL, 0) == 0);
	REQUIRE(length == sizeof(before));
	length = sizeof(before) - 1;
	REQUIRE(sysctlbyname("vfs.io.stats", &before, &length, NULL, 0) == -1);
	REQUIRE(errno == ENOMEM && length == sizeof(before));
	REQUIRE(sysctlbyname("vfs.io.stats", NULL, NULL, &before, 1) == -1);
	REQUIRE(errno == EPERM);
	REQUIRE(snapshot(&before));
	length = sizeof(memory);
	REQUIRE(sysctlbyname("hw.memory.stats", &memory, &length, NULL, 0) == 0);
	REQUIRE(length == sizeof(memory) && memory.version == MEMORY_STATS_VERSION);
	printf("MEM valid=%u ranges=%llu usable=%llu highest=%llu usable_highest=%llu "
	    "mapped=%llu initial=%llu managed=%llu reserved=%llu allocated=%llu free=%llu\n",
	    memory.boot_ranges_valid, (unsigned long long)memory.boot_range_count,
	    (unsigned long long)memory.boot_usable_bytes,
	    (unsigned long long)memory.boot_highest_end,
	    (unsigned long long)memory.boot_usable_highest_end,
	    (unsigned long long)memory.direct_mapped_bytes,
	    (unsigned long long)memory.allocator_initial_bytes,
	    (unsigned long long)memory.physical_managed_bytes,
	    (unsigned long long)memory.physical_reserved_bytes,
	    (unsigned long long)memory.physical_allocated_bytes,
	    (unsigned long long)memory.physical_free_bytes);

	REQUIRE(snapshot(&before));
	fd = open("/ws025-baseline", O_CREAT | O_TRUNC | O_RDWR, 0600);
	REQUIRE(fd >= 0);
	memset(data, 0x57, sizeof(data));
	for (i = 0; i < 4; i++)
		REQUIRE(write(fd, data, sizeof(data)) == sizeof(data));
	REQUIRE(fsync(fd) == 0);
	REQUIRE(snapshot(&after));
	report_delta("CREATEBASE");
	REQUIRE(snapshot(&before));
	REQUIRE(pread(fd, check, sizeof(check), 0) == sizeof(check));
	REQUIRE(memcmp(data, check, sizeof(data)) == 0);
	REQUIRE(snapshot(&after));
	report_delta("READBASE");

	/* Mode 0 reproduces q087; mode 1 overwrites 256 KiB of distinct content. */
	for (mode = 0; mode < 2; mode++) {
		for (sample = 0; sample < SAMPLES; sample++) {
			memset(data, (int)(sample + 1), sizeof(data));
			REQUIRE(snapshot(&before));
			REQUIRE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
			for (i = 0; i < 4; i++)
				REQUIRE(pwrite(fd, data, sizeof(data),
				    mode ? (off_t)i * sizeof(data) : 0) == sizeof(data));
			REQUIRE(fsync(fd) == 0);
			REQUIRE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
			REQUIRE(snapshot(&after));
			elapsed[sample] = ordered[sample] = nanos(&end) - nanos(&start);
			for (event = 0; event < IO_STAT_COUNT; event++) {
				delta[sample].events[event].calls =
				    after.events[event].calls - before.events[event].calls;
				delta[sample].events[event].bytes =
				    after.events[event].bytes - before.events[event].bytes;
			}
			for (i = 0; i < (mode ? 4U : 1U); i++) {
				REQUIRE(pread(fd, check, sizeof(check),
				    (off_t)i * sizeof(check)) == sizeof(check));
				REQUIRE(memcmp(data, check, sizeof(data)) == 0);
			}
		}
		/* Nearest-rank quantiles; preserve the original samples as well. */
		for (i = 1; i < SAMPLES; i++) {
			key = ordered[i];
			j = i;
			while (j != 0 && ordered[j - 1] > key) {
				ordered[j] = ordered[j - 1];
				j--;
			}
			ordered[j] = key;
		}
		printf("QUANTILE mode=%u n=%u p50_ns=%llu p95_ns=%llu p99_ns=%llu\n",
		    mode, SAMPLES, (unsigned long long)ordered[50],
		    (unsigned long long)ordered[95], (unsigned long long)ordered[99]);
		for (sample = 0; sample < SAMPLES; sample++) {
			printf("SAMPLE mode=%u n=%u ns=%llu", mode, sample,
			    (unsigned long long)elapsed[sample]);
			for (event = 0; event < IO_STAT_COUNT; event++)
				printf(" %u:%llu:%llu", event,
				    (unsigned long long)delta[sample].events[event].calls,
				    (unsigned long long)delta[sample].events[event].bytes);
			printf("\n");
		}
	}
	REQUIRE(close(fd) == 0);
	puts("BASELINE PASS");
	return 0;
}
