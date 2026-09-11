/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zedbsd/io-stats.h>
#include <zedbsd/cache-memory.h>

#define REQUIRE(x) do { if (!(x)) { printf("FILE CACHE FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
static unsigned char bytes[65536], observed[65536];
static struct io_stats before, after;

static int snapshot(struct io_stats *stats)
{
	size_t size;

	size = sizeof(*stats);
	return sysctlbyname("vfs.io.stats", stats, &size, NULL, 0) == 0 &&
	    size == sizeof(*stats) && stats->version == IO_STATS_VERSION;
}

static int cache_budget_acceptance(void)
{
	struct cache_memory_stats memory, grown, shrunk;
	unsigned char *mapped;
	uint64_t target, original;
	size_t size;
	unsigned run;
	int fd;

	size = sizeof(memory);
	REQUIRE(sysctlbyname("vfs.cache_memory.stats", &memory, &size, NULL, 0) == 0);
	REQUIRE(size == sizeof(memory) && memory.version == CACHE_MEMORY_VERSION);
	REQUIRE(memory.initialized && !memory.resizing && memory.pending_bytes == 0);
	REQUIRE(memory.target_bytes == (memory.managed_bytes / 4U & ~UINT64_C(4095)));
	REQUIRE(memory.usage[CACHE_MEMORY_WORKER].resident_bytes >= 69632);
	original = memory.target_bytes;
	fd = open("/ws025-cache-budget", O_CREAT | O_TRUNC | O_RDWR, 0600);
	REQUIRE(fd >= 0);
	for (run = 0; run < 4; run++)
		REQUIRE(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
	REQUIRE(fsync(fd) == 0);
	for (run = 0; run < 4; run++) {
		REQUIRE(pread(fd, observed, sizeof(observed), run * sizeof(bytes)) == sizeof(observed));
		REQUIRE(memcmp(bytes, observed, sizeof(bytes)) == 0);
	}
	REQUIRE(snapshot(&before));
	for (run = 0; run < 4; run++)
		REQUIRE(pread(fd, observed, sizeof(observed), run * sizeof(bytes)) == sizeof(observed));
	REQUIRE(snapshot(&after));
	REQUIRE(after.events[IO_UFS_CONTENT_READ].calls == before.events[IO_UFS_CONTENT_READ].calls);
	size = sizeof(grown);
	REQUIRE(sysctlbyname("vfs.cache_memory.stats", &grown, &size, NULL, 0) == 0);
	REQUIRE(grown.usage[CACHE_MEMORY_FILE_DATA].resident_bytes >= 262144);
	REQUIRE(grown.usage[CACHE_MEMORY_FILE_META].resident_bytes >= 8192);

	/* Mandatory mapping and dirty contents survive an impossible shrink. */
	mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	REQUIRE(mapped != MAP_FAILED);
	mapped[19] = 0x42;
	target = 0;
	errno = 0;
	REQUIRE(sysctlbyname("vfs.cache_memory.target_bytes", NULL, NULL, &target, sizeof(target)) == -1);
	REQUIRE(errno == EBUSY && mapped[19] == 0x42);
	REQUIRE(pread(fd, observed, 1, 19) == 1 && observed[0] == 0x42);
	REQUIRE(msync(mapped, 4096, MS_SYNC) == 0);
	REQUIRE(munmap(mapped, 4096) == 0);
	size = sizeof(grown);
	REQUIRE(sysctlbyname("vfs.cache_memory.stats", &grown, &size, NULL, 0) == 0);
	REQUIRE(grown.target_bytes == original && !grown.resizing);

	/* Reclaim enough clean storage to publish a smaller policy. */
	REQUIRE(grown.resident_bytes >= 131072);
	target = (grown.resident_bytes - 131072) & ~UINT64_C(4095);
	REQUIRE(sysctlbyname("vfs.cache_memory.target_bytes", NULL, NULL, &target, sizeof(target)) == 0);
	size = sizeof(shrunk);
	REQUIRE(sysctlbyname("vfs.cache_memory.stats", &shrunk, &size, NULL, 0) == 0);
	REQUIRE(shrunk.target_bytes == target && !shrunk.resizing);
	REQUIRE(shrunk.resident_bytes + shrunk.pending_bytes <= target);
	REQUIRE(shrunk.reclaimed_bytes > grown.reclaimed_bytes);
	REQUIRE(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
	REQUIRE(observed[19] == 0x42);
	REQUIRE(sysctlbyname("vfs.cache_memory.target_bytes", NULL, NULL, &original, sizeof(original)) == 0);
	REQUIRE(close(fd) == 0);
	printf("CACHE BUDGET managed=%llu worker=%llu resident_before=%llu resident_after=%llu target=%llu PASS\n",
	    (unsigned long long)memory.managed_bytes,
	    (unsigned long long)memory.usage[CACHE_MEMORY_WORKER].resident_bytes,
	    (unsigned long long)grown.resident_bytes,
	    (unsigned long long)shrunk.resident_bytes, (unsigned long long)target);
	return 0;
}

int main(void)
{
	int fd, other, lower, upper;
	unsigned i;
	unsigned char *mapping;
	unsigned char byte;

	for (i = 0; i < sizeof(bytes); i++)
		bytes[i] = (unsigned char)(i % 251U);
	fd = open("/ws025-cache", O_CREAT | O_TRUNC | O_RDWR, 0600);
	REQUIRE(fd >= 0);
	REQUIRE(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
	REQUIRE(fsync(fd) == 0);
	REQUIRE(close(fd) == 0);

	/* Populate once, close every user description, then measure the reopen. */
	fd = open("/ws025-cache", O_RDONLY);
	REQUIRE(fd >= 0);
	REQUIRE(read(fd, observed, sizeof(observed)) == sizeof(observed));
	REQUIRE(memcmp(bytes, observed, sizeof(bytes)) == 0);
	REQUIRE(close(fd) == 0);
	REQUIRE(snapshot(&before));
	fd = open("/ws025-cache", O_RDWR);
	REQUIRE(fd >= 0);
	REQUIRE(read(fd, observed, sizeof(observed)) == sizeof(observed));
	REQUIRE(snapshot(&after));
	REQUIRE(memcmp(bytes, observed, sizeof(bytes)) == 0);
	printf("CACHE WARM ufs_content_reads=%llu usb_reads=%llu\n",
	    (unsigned long long)(after.events[IO_UFS_CONTENT_READ].calls - before.events[IO_UFS_CONTENT_READ].calls),
	    (unsigned long long)(after.events[IO_USB_READ10].calls - before.events[IO_USB_READ10].calls));
	REQUIRE(after.events[IO_UFS_CONTENT_READ].calls == before.events[IO_UFS_CONTENT_READ].calls);
	REQUIRE(after.events[IO_USB_READ10].calls == before.events[IO_USB_READ10].calls);

	/* Alias writes and shared mappings use the same authoritative contents. */
	other = open("/ws025-cache", O_RDWR);
	REQUIRE(other >= 0);
	byte = 0xe3;
	REQUIRE(pwrite(other, &byte, 1, 123) == 1);
	REQUIRE(pread(fd, observed, 1, 123) == 1 && observed[0] == byte);
	mapping = mmap(NULL, sizeof(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	REQUIRE(mapping != MAP_FAILED);
	mapping[8193] = 0x9a;
	REQUIRE(pread(other, observed, 1, 8193) == 1 && observed[0] == 0x9a);
	REQUIRE(msync(mapping, sizeof(bytes), MS_SYNC) == 0);
	REQUIRE(munmap(mapping, sizeof(bytes)) == 0);
	REQUIRE(ftruncate(other, 5000) == 0);
	REQUIRE(pread(fd, observed, sizeof(observed), 0) == 5000);
	REQUIRE(pread(fd, observed, 1, 5000) == 0);
	REQUIRE(close(other) == 0);
	REQUIRE(close(fd) == 0);

	/* Old lower handles retain their layer while copy-up creates a new owner. */
	lower = open("/etc/ws025-cache-lower", O_RDONLY);
	REQUIRE(lower >= 0);
	REQUIRE(read(lower, observed, 1) == 1 && observed[0] == 'l');
	upper = open("/etc/ws025-cache-lower", O_RDWR);
	REQUIRE(upper >= 0);
	byte = 'X';
	REQUIRE(pwrite(upper, &byte, 1, 0) == 1);
	REQUIRE(pread(upper, observed, 1, 0) == 1 && observed[0] == 'X');
	REQUIRE(pread(lower, observed, 1, 0) == 1 && observed[0] == 'l');
	REQUIRE(close(upper) == 0);
	REQUIRE(close(lower) == 0);

	/* Reusing a pathname cannot reuse the retired inode's cached bytes. */
	REQUIRE(unlink("/ws025-cache") == 0);
	fd = open("/ws025-cache", O_CREAT | O_EXCL | O_RDWR, 0600);
	REQUIRE(fd >= 0);
	byte = 0x37;
	REQUIRE(write(fd, &byte, 1) == 1);
	REQUIRE(pread(fd, observed, 1, 0) == 1 && observed[0] == byte);
	REQUIRE(fsync(fd) == 0);
	REQUIRE(close(fd) == 0);
	REQUIRE(cache_budget_acceptance() == 0);
	puts("FILE CACHE PASS");
	return 0;
}
