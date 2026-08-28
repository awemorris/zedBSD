/*
 * KA-T021: UFS2 journal and snapshot behavior fixture
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KA_LEGACY_UFS_COMMON)
#include <kern/ufs-consistency.h>
#include <kern/ufs-snapshot.h>
#define JOURNAL_IO ufs_journal_io
#define JOURNAL ufs_journal
#define JOURNAL_INIT ufs_journal_init
#define JOURNAL_COMMIT ufs_journal_commit
#define JOURNAL_REPLAY ufs_journal_replay
#define SNAPSHOT_ENTRY ufs_snapshot_entry
#define SNAPSHOT ufs_snapshot
#define SNAPSHOT_INIT ufs_snapshot_init
#define SNAPSHOT_OPEN ufs_snapshot_open
#define SNAPSHOT_CREATE ufs_snapshot_create
#define SNAPSHOT_PRESERVE ufs_snapshot_preserve
#define SNAPSHOT_READ ufs_snapshot_read
#define SNAPSHOT_DELETE ufs_snapshot_delete
#else
#include "ufs2-consistency.h"
#define JOURNAL_IO ufs2_journal_io
#define JOURNAL ufs2_journal
#define JOURNAL_INIT ufs2_journal_init
#define JOURNAL_COMMIT ufs2_journal_commit
#define JOURNAL_REPLAY ufs2_journal_replay
#define SNAPSHOT_ENTRY ufs2_snapshot_entry
#define SNAPSHOT ufs2_snapshot
#define SNAPSHOT_INIT ufs2_snapshot_init
#define SNAPSHOT_OPEN ufs2_snapshot_open
#define SNAPSHOT_CREATE ufs2_snapshot_create
#define SNAPSHOT_PRESERVE ufs2_snapshot_preserve
#define SNAPSHOT_READ ufs2_snapshot_read
#define SNAPSHOT_DELETE ufs2_snapshot_delete
#endif

#define SECTOR_SIZE 512U
#define DISK_SECTORS 256U
#define JOURNAL_FIRST 100U
#define JOURNAL_SECTORS 8U
#define SNAPSHOT_FIRST 180U
#define SNAPSHOT_SECTORS 17U
#define SNAPSHOT_MAP_ENTRIES 16U
#define DESC_MAGIC UINT32_C(0x4a534655)
#define COMMIT_MAGIC UINT32_C(0x434a4655)

struct memory_disk {
	uint8_t sector[DISK_SECTORS][SECTOR_SIZE];
	unsigned reads;
	unsigned writes;
	unsigned flushes;
};

static unsigned checks;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "KA-T021: check failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                    \
			exit(1);                                             \
		}                                                            \
	} while (0)

static int
memory_read(void *context, uint64_t first, uint32_t count, void *buffer)
{
	struct memory_disk *disk = context;

	if (buffer == NULL || count == 0 || first >= DISK_SECTORS ||
	    count > DISK_SECTORS - first)
		return EIO;
	memcpy(buffer, disk->sector[first], (size_t)count * SECTOR_SIZE);
	disk->reads++;
	return 0;
}

static int
memory_write(void *context, uint64_t first, uint32_t count,
	const void *buffer)
{
	struct memory_disk *disk = context;

	if (buffer == NULL || count == 0 || first >= DISK_SECTORS ||
	    count > DISK_SECTORS - first)
		return EIO;
	memcpy(disk->sector[first], buffer, (size_t)count * SECTOR_SIZE);
	disk->writes++;
	return 0;
}

static int
memory_flush(void *context)
{
	struct memory_disk *disk = context;

	disk->flushes++;
	return 0;
}

static void
put32(uint8_t *pointer, uint32_t value)
{
	pointer[0] = (uint8_t)value;
	pointer[1] = (uint8_t)(value >> 8);
	pointer[2] = (uint8_t)(value >> 16);
	pointer[3] = (uint8_t)(value >> 24);
}

static void
put64(uint8_t *pointer, uint64_t value)
{
	put32(pointer, (uint32_t)value);
	put32(pointer + 4, (uint32_t)(value >> 32));
}

static uint32_t
get32(const uint8_t *pointer)
{
	return (uint32_t)pointer[0] | (uint32_t)pointer[1] << 8 |
	    (uint32_t)pointer[2] << 16 | (uint32_t)pointer[3] << 24;
}

static uint32_t
digest(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	uint32_t value = UINT32_C(2166136261);
	size_t index;

	for (index = 0; index < length; index++) {
		value ^= bytes[index];
		value *= UINT32_C(16777619);
	}
	return value;
}

static void
fill_pattern(void *buffer, size_t length, uint8_t seed)
{
	uint8_t *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++)
		bytes[index] = (uint8_t)(seed + index * 29U);
}

static void
write_pending_transaction(struct memory_disk *disk, uint64_t sequence,
	uint64_t target, const uint8_t *payload, uint32_t sectors,
	int committed)
{
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint32_t payload_digest;

	payload_digest = digest(payload, (size_t)sectors * SECTOR_SIZE);
	memset(descriptor, 0, sizeof(descriptor));
	put32(descriptor, DESC_MAGIC);
	put32(descriptor + 4, 1);
	put64(descriptor + 8, sequence);
	put64(descriptor + 16, target);
	put32(descriptor + 24, sectors);
	put32(descriptor + 28, payload_digest);
	put32(descriptor + 32, digest(descriptor, 32));
	memcpy(disk->sector[JOURNAL_FIRST], descriptor, sizeof(descriptor));
	memcpy(disk->sector[JOURNAL_FIRST + 1U], payload,
	    (size_t)sectors * SECTOR_SIZE);
	memset(commit, 0, sizeof(commit));
	if (committed) {
		put32(commit, COMMIT_MAGIC);
		put64(commit + 8, sequence);
		put32(commit + 16, payload_digest);
		put32(commit + 20, digest(commit, 20));
	}
	memcpy(disk->sector[JOURNAL_FIRST + 1U + sectors], commit,
	    sizeof(commit));
}

static void
test_journal(void)
{
	struct memory_disk disk;
	struct JOURNAL_IO io;
	struct JOURNAL journal;
	uint8_t payload[SECTOR_SIZE * 2U];
	uint8_t zeros[SECTOR_SIZE * 2U];

	memset(&disk, 0, sizeof(disk));
	memset(zeros, 0, sizeof(zeros));
	fill_pattern(payload, sizeof(payload), 0x31);
	io.context = &disk;
	io.read = memory_read;
	io.write = memory_write;
	io.flush = memory_flush;

	CHECK(JOURNAL_INIT(NULL, &io, JOURNAL_FIRST, JOURNAL_SECTORS) ==
	    EINVAL);
	CHECK(JOURNAL_INIT(&journal, &io, JOURNAL_FIRST, 2) == EINVAL);
	CHECK(JOURNAL_INIT(&journal, &io, JOURNAL_FIRST, JOURNAL_SECTORS) == 0);
	CHECK(journal.next_sequence == 1);
	CHECK(JOURNAL_COMMIT(&journal, 10, payload, 2) == 0);
	CHECK(memcmp(disk.sector[10], payload, sizeof(payload)) == 0);
	CHECK(get32(disk.sector[JOURNAL_FIRST]) == 0);
	CHECK(journal.next_sequence == 2);
	CHECK(disk.flushes >= 5);
	CHECK(JOURNAL_COMMIT(&journal, JOURNAL_FIRST + 1U, payload, 1) ==
	    EINVAL);

	memset(disk.sector[30], 0, sizeof(payload));
	write_pending_transaction(&disk, 7, 30, payload, 2, 1);
	CHECK(JOURNAL_REPLAY(&journal) == 0);
	CHECK(memcmp(disk.sector[30], payload, sizeof(payload)) == 0);
	CHECK(get32(disk.sector[JOURNAL_FIRST]) == 0);
	CHECK(journal.next_sequence == 8);

	memset(disk.sector[40], 0, sizeof(payload));
	write_pending_transaction(&disk, 8, 40, payload, 2, 0);
	CHECK(JOURNAL_REPLAY(&journal) == 0);
	CHECK(memcmp(disk.sector[40], zeros, sizeof(zeros)) == 0);
	CHECK(get32(disk.sector[JOURNAL_FIRST]) == 0);

	write_pending_transaction(&disk, 9, 50, payload, 2, 1);
	disk.sector[JOURNAL_FIRST][32] ^= 1U;
	CHECK(JOURNAL_REPLAY(&journal) == EIO);
	CHECK(memcmp(disk.sector[50], zeros, sizeof(zeros)) == 0);
}

static void
test_snapshot(void)
{
	struct memory_disk disk;
	struct JOURNAL_IO io;
	struct SNAPSHOT snapshot;
	struct SNAPSHOT reopened;
	struct SNAPSHOT_ENTRY map[SNAPSHOT_MAP_ENTRIES];
	struct SNAPSHOT_ENTRY reopened_map[SNAPSHOT_MAP_ENTRIES];
	uint8_t original[SECTOR_SIZE];
	uint8_t replacement[SECTOR_SIZE];
	uint8_t result[SECTOR_SIZE];

	memset(&disk, 0, sizeof(disk));
	fill_pattern(original, sizeof(original), 0x42);
	fill_pattern(replacement, sizeof(replacement), 0x93);
	memcpy(disk.sector[60], original, sizeof(original));
	io.context = &disk;
	io.read = memory_read;
	io.write = memory_write;
	io.flush = memory_flush;

	CHECK(SNAPSHOT_INIT(&snapshot, &io, 128, SNAPSHOT_FIRST,
	    SNAPSHOT_SECTORS, map, 1) == EINVAL);
	CHECK(SNAPSHOT_INIT(&snapshot, &io, 128, SNAPSHOT_FIRST,
	    SNAPSHOT_SECTORS, map, SNAPSHOT_MAP_ENTRIES) == 0);
	CHECK(SNAPSHOT_OPEN(&snapshot) == 0);
	CHECK(snapshot.active == 0);
	CHECK(SNAPSHOT_CREATE(&snapshot) == 0);
	CHECK(snapshot.active == 1);
	CHECK(SNAPSHOT_CREATE(&snapshot) == EBUSY);
	CHECK(SNAPSHOT_PRESERVE(&snapshot, 60, 1) == 0);
	CHECK(snapshot.next_record == 1);
	CHECK(SNAPSHOT_PRESERVE(&snapshot, 60, 1) == 0);
	CHECK(snapshot.next_record == 1);
	memcpy(disk.sector[60], replacement, sizeof(replacement));
	CHECK(SNAPSHOT_READ(&snapshot, 60, 1, result) == 0);
	CHECK(memcmp(result, original, sizeof(result)) == 0);

	CHECK(SNAPSHOT_INIT(&reopened, &io, 128, SNAPSHOT_FIRST,
	    SNAPSHOT_SECTORS, reopened_map, SNAPSHOT_MAP_ENTRIES) == 0);
	CHECK(SNAPSHOT_OPEN(&reopened) == 0);
	CHECK(reopened.active == 1);
	CHECK(reopened.next_record == 1);
	CHECK(SNAPSHOT_READ(&reopened, 60, 1, result) == 0);
	CHECK(memcmp(result, original, sizeof(result)) == 0);
	CHECK(SNAPSHOT_DELETE(&reopened) == 0);
	CHECK(reopened.active == 0);
	CHECK(SNAPSHOT_READ(&reopened, 60, 1, result) == EINVAL);
	CHECK(SNAPSHOT_DELETE(&reopened) == ENOENT);

	CHECK(SNAPSHOT_CREATE(&reopened) == 0);
	disk.sector[SNAPSHOT_FIRST][32] ^= 1U;
	CHECK(SNAPSHOT_INIT(&snapshot, &io, 128, SNAPSHOT_FIRST,
	    SNAPSHOT_SECTORS, map, SNAPSHOT_MAP_ENTRIES) == 0);
	CHECK(SNAPSHOT_OPEN(&snapshot) == EIO);
}

int
main(void)
{
	test_journal();
	test_snapshot();
	printf("KA-T021: PASS (%u checks)\n", checks);
	return 0;
}
