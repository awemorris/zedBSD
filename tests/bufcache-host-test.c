/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/buf.h"
#include "kern/disk.h"
#include "kern/sysctl.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SECTORS 256U

struct fixture {
	unsigned char bytes[SECTORS * 512U];
	unsigned reads;
	unsigned writes;
	int fail_write;
};

static int
fixture_submit(struct disk *disk, struct bio *bio)
{
	struct fixture *fixture = disk->d_data;
	size_t bytes = (size_t)bio->b_block_count * 512U;
	unsigned char *where = fixture->bytes + bio->b_mapped_block * 512U;
	if (bio->b_op == BIO_READ) {
		(void)__atomic_add_fetch(&fixture->reads, 1U, __ATOMIC_RELAXED);
		memcpy(bio->b_data, where, bytes);
	} else if (bio->b_op == BIO_WRITE) {
		(void)__atomic_add_fetch(&fixture->writes, 1U, __ATOMIC_RELAXED);
		if (fixture->fail_write) {
			bio_complete(bio, EIO, 0);
			return 0;
		}
		memcpy(where, bio->b_data, bytes);
	} else if (bio->b_op != BIO_FLUSH) {
		return EOPNOTSUPP;
	}
	bio_complete(bio, 0, bio->b_op == BIO_FLUSH ? 0 : bytes);
	return 0;
}
static const struct disk_ops fixture_ops = { .submit = fixture_submit };

struct read_worker {
	struct disk *disk;
	volatile unsigned *ready;
	volatile unsigned *start;
	int error;
	unsigned char value;
};

static void *
concurrent_read(void *argument)
{
	struct read_worker *worker = argument;
	unsigned char sector[512];
	(void)__atomic_add_fetch(worker->ready, 1U, __ATOMIC_RELEASE);
	while (__atomic_load_n(worker->start, __ATOMIC_ACQUIRE) == 0)
		;
	worker->error = disk_read(worker->disk, 32, 1, sector);
	worker->value = sector[0];
	return NULL;
}

int
main(void)
{
	static struct fixture fixture;
	struct zedbsd_bufcache_stats stats;
	struct disk *disk;
	struct disk *slice;
	struct buf *pinned[16];
	unsigned char sector[512];
	int mib[3] = { CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_STATS };
	int meta[2] = { CTL_SYSCTL, CTL_SYSCTL_NAME2OID };
	int oid[CTL_MAXNAME];
	size_t length;
	uint64_t limit;
	unsigned i;

	assert(buf_init() == 0);
	disk_registry_reset();
	disk = disk_alloc();
	assert(disk != NULL);
	strcpy(disk->d_name, "cache0");
	disk->d_block_size = 512;
	disk->d_block_count = SECTORS;
	disk->d_max_transfer_blocks = 8;
	disk->d_ops = &fixture_ops;
	disk->d_data = &fixture;
	assert(disk_create(disk) == 0);

	fixture.bytes[0] = 0x12;
	assert(disk_read(disk, 0, 1, sector) == 0 && sector[0] == 0x12);
	assert(disk_read(disk, 0, 1, sector) == 0);
	assert(fixture.reads == 1);
	memset(sector, 0x5a, sizeof(sector));
	assert(disk_write(disk, 1, 1, sector) == 0);
	assert(fixture.writes == 1);
	assert(fixture.bytes[512] == 0x5a);

	/* A partition alias and its absolute parent range share one cache line. */
	slice = disk_alloc();
	assert(slice != NULL);
	strcpy(slice->d_name, "cache0p1");
	slice->d_flags = DISK_PARTITION;
	slice->d_block_size = 512;
	slice->d_block_count = 16;
	slice->d_parent = disk;
	slice->d_parent_offset = 8;
	assert(disk_create(slice) == 0);
	fixture.bytes[8U * 512U] = 0x33;
	assert(buf_invalidate(disk, 8, 1, BUF_INVALIDATE_DISCARD) == 0);
	assert(disk_read(disk, 8, 1, sector) == 0);
	i = __atomic_load_n(&fixture.reads, __ATOMIC_RELAXED);
	assert(disk_read(slice, 0, 1, sector) == 0 && sector[0] == 0x33);
	assert(__atomic_load_n(&fixture.reads, __ATOMIC_RELAXED) == i);
	assert(disk_gone_if_idle(slice) == 0);
	assert(disk_destroy(slice) == 0);

	/* All contenders for one cold key must observe one backend READ. */
	{
		pthread_t threads[8];
		struct read_worker workers[8];
		volatile unsigned ready = 0, start = 0;
		unsigned reads;
		fixture.bytes[32U * 512U] = 0xa7;
		assert(buf_invalidate(disk, 32, 1, BUF_INVALIDATE_DISCARD) == 0);
		reads = __atomic_load_n(&fixture.reads, __ATOMIC_RELAXED);
		for (i = 0; i < 8; i++) {
			memset(&workers[i], 0, sizeof(workers[i]));
			workers[i].disk = disk;
			workers[i].ready = &ready;
			workers[i].start = &start;
			assert(pthread_create(&threads[i], NULL, concurrent_read,
			    &workers[i]) == 0);
		}
		while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 8U)
			;
		__atomic_store_n(&start, 1U, __ATOMIC_RELEASE);
		for (i = 0; i < 8; i++) {
			assert(pthread_join(threads[i], NULL) == 0);
			assert(workers[i].error == 0 && workers[i].value == 0xa7);
		}
		assert(__atomic_load_n(&fixture.reads, __ATOMIC_RELAXED) ==
		    reads + 1U);
	}

	/* A write error remains dirty and a later sync can recover it. */
	fixture.fail_write = 1;
	memset(sector, 0x6b, sizeof(sector));
	assert(disk_write(disk, 40, 1, sector) == EIO);
	buf_get_stats(&stats);
	assert(stats.dirty_bytes != 0 && stats.writeback_errors != 0);
	fixture.fail_write = 0;
	assert(disk_sync(disk) == 0);
	buf_get_stats(&stats);
	assert(stats.dirty_bytes == 0 && fixture.bytes[40U * 512U] == 0x6b);

	length = sizeof(stats);
	assert(kern_sysctl(mib, 3, &stats, &length, NULL, 0, 0) == 0);
	assert(length == sizeof(stats));
	assert(stats.hits >= 2 && stats.misses >= 1);
	assert(stats.dirty_bytes == 0 && stats.read_bios >= 1);

	length = sizeof(oid);
	assert(kern_sysctl(meta, 2, oid, &length,
	    "vfs.bufcache.max_bytes", sizeof("vfs.bufcache.max_bytes"), 0) == 0);
	assert(length == 3 * sizeof(int));
	assert(oid[0] == CTL_VFS && oid[1] == VFS_BUFCACHE &&
	    oid[2] == VFS_BUFCACHE_MAX_BYTES);

	length = sizeof(limit);
	assert(kern_sysctl(oid, 3, &limit, &length, NULL, 0, 0) == 0);
	assert(kern_sysctl(oid, 3, NULL, NULL, &limit, sizeof(limit), 0) == EPERM);
	assert(kern_sysctl(oid, 3, NULL, NULL, &limit, sizeof(limit), 1) == 0);

	/* Pinned cache lines make an undersized hard-cap change fail atomically. */
	for (i = 0; i < 16; i++)
		assert(buf_get(disk, (uint64_t)i * 8U, &pinned[i]) == 0);
	{
		uint64_t minimum = 64U * 1024U;
		assert(buf_set_max_bytes(minimum) == EBUSY);
	}
	for (i = 0; i < 16; i++)
		buf_release(pinned[i]);
	assert(buf_set_max_bytes(64U * 1024U) == 0);

	assert(buf_invalidate_disk(disk, 0) == 0);
	assert(disk_gone_if_idle(disk) == 0);
	assert(disk_destroy(disk) == 0);
	puts("zedBSD buffer cache/sysctl host tests: OK");
	return 0;
}
