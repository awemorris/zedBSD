/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of diskpart's GPT codec: the edits a batch makes (add,
 * delete, and replacing an entry's type, PARTUUID, name and attributes)
 * on a disk in memory, with 512-byte and 4096-byte sectors.
 */

#include "userland/base/diskpart/table.h"
#include "include/uapi/gpt-type.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One disk in memory, whose bytes the codec reads and writes. */
struct memory_disk {
	uint8_t *bytes;
	uint64_t size;
};

static int memory_read(void *context, uint64_t offset, void *data, size_t size);
static int memory_write(void *context, uint64_t offset, const void *data, size_t size);
static int memory_flush(void *context);
static void check(int condition, const char *what);
static void run_sector_size(uint32_t sector_size);

/* How many checks failed. */
static int failures;

/*
 * Runs the codec's checks for both sector sizes.
 */
int
main(void)
{
	/* Runs the same edits on both sector sizes. */
	run_sector_size(512U);
	run_sector_size(4096U);

	/* Reports the verdict. */
	if (failures != 0) {
		printf("diskpart-table-test: FAIL (%d)\n", failures);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("diskpart-table-test: PASS\n");
	return 0;
}

/* Reads bytes of the disk in memory. */
static int
memory_read(
	void *context,
	uint64_t offset,
	void *data,
	size_t size)
{
	struct memory_disk *disk;

	/* Refuses a read past the disk's end. */
	disk = context;
	if (offset > disk->size || size > disk->size - offset)
		return EIO;

	/* Copies the bytes out. */
	memcpy(data, disk->bytes + offset, size);
	return 0;
}

/* Writes bytes of the disk in memory. */
static int
memory_write(
	void *context,
	uint64_t offset,
	const void *data,
	size_t size)
{
	struct memory_disk *disk;

	/* Refuses a write past the disk's end. */
	disk = context;
	if (offset > disk->size || size > disk->size - offset)
		return EIO;

	/* Copies the bytes in. */
	memcpy(disk->bytes + offset, data, size);
	return 0;
}

/* Flushes the disk in memory, which has nothing to flush. */
static int
memory_flush(
	void *context)
{
	(void)context;

	/* Succeeded: memory is always durable here. */
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Reports a failure by name. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Makes a GPT, edits it as a batch would, and reads it back. */
static void
run_sector_size(
	uint32_t sector_size)
{
	struct memory_disk disk;
	struct dp_table table;
	struct dp_io io;
	uint64_t mib;
	uint64_t sectors;
	uint8_t *untouched;
	uint8_t *copy;
	int started;
	int error;

	/* Makes a 256-MiB disk of zeros. */
	disk.size = UINT64_C(256) * 1024U * 1024U;
	disk.bytes = calloc(1, (size_t)disk.size);
	if (disk.bytes == NULL) {
		check(0, "memory for the disk");
		return;
	}
	sectors = disk.size / sector_size;
	mib = 1048576U / sector_size;
	io.context = &disk;
	io.sectors = sectors;
	io.sector_size = sector_size;
	io.read = memory_read;
	io.write = memory_write;
	io.flush = memory_flush;

	/* Initializes a GPT with an ESP and a data partition of another system. */
	error = dp_initialize_gpt(&table, &io, "11111111-2222-4333-8444-555555555555");
	check(error == 0, "init");
	error = dp_add(&table, 1, mib, 64U * mib, KERN_GPT_TYPE_EFI_SYSTEM,
	    "aaaaaaaa-0000-4000-8000-000000000001", "EFI System");
	check(error == 0, "add ESP");
	error = dp_add(&table, 2, 65U * mib, 64U * mib, KERN_GPT_TYPE_BASIC_DATA,
	    "aaaaaaaa-0000-4000-8000-000000000002", "Other");
	check(error == 0, "add other");
	error = dp_write(&table, &started);
	check(error == 0, "write the first table");
	dp_free(&table);

	/* Keeps the other system's entry to compare after the batch. */
	error = dp_load(&table, &io);
	check(error == 0, "load the first table");
	untouched = malloc(128);
	copy = malloc(128);
	if (untouched == NULL || copy == NULL) {
		check(0, "memory for the entries");
		free(untouched);
		free(copy);
		dp_free(&table);
		free(disk.bytes);
		return;
	}
	memcpy(untouched, table.edited + 128, 128);

	/* Adds two entries, as the ESP and the new partition of one creation. */
	error = dp_add(&table, 3, 129U * mib, 32U * mib, KERN_GPT_TYPE_ZEDBSD_UNASSIGNED,
	    "aaaaaaaa-0000-4000-8000-000000000003", "");
	check(error == 0, "add an unassigned partition");
	error = dp_add(&table, 4, 161U * mib, 32U * mib, KERN_GPT_TYPE_ZEDBSD_UNASSIGNED,
	    "aaaaaaaa-0000-4000-8000-000000000004", "");
	check(error == 0, "add a second unassigned partition");

	/* Marks one as root with a new PARTUUID and clears its attributes. */
	error = dp_modify(&table, 3, KERN_GPT_TYPE_ZEDBSD_UFS,
	    "bbbbbbbb-0000-4000-8000-000000000003", "zedBSD installed root", 0);
	check(error == 0, "set root");

	/* Refuses a PARTUUID that another entry already has, leaving the entry. */
	error = dp_modify(&table, 4, KERN_GPT_TYPE_ZEDBSD_SWAP,
	    "aaaaaaaa-0000-4000-8000-000000000002", "zedBSD installed swap", 0);
	check(error != 0, "a duplicate PARTUUID is refused");
	check(strcmp(table.parts[3].type, "7ef6a3e2-f243-445d-b77a-c37ca9605418") == 0,
	    "a refused change keeps the type");

	/* Refuses an unused slot and a name GPT cannot hold. */
	error = dp_modify(&table, 9, KERN_GPT_TYPE_ZEDBSD_SWAP,
	    "bbbbbbbb-0000-4000-8000-000000000009", "x", 0);
	check(error == ENOENT, "an unused slot is refused");
	error = dp_modify(&table, 4, KERN_GPT_TYPE_ZEDBSD_SWAP,
	    "bbbbbbbb-0000-4000-8000-000000000004",
	    "a name that is far too long for a GPT entry to hold", 0);
	check(error == ENAMETOOLONG, "a long name is refused");

	/* Marks the other as swap with an attribute bit, then deletes nothing more. */
	error = dp_modify(&table, 4, KERN_GPT_TYPE_ZEDBSD_SWAP,
	    "bbbbbbbb-0000-4000-8000-000000000004", "zedBSD installed swap", 1);
	check(error == 0, "set swap");

	/* Writes the batch's table once. */
	error = dp_write(&table, &started);
	check(error == 0, "write the batch");
	dp_free(&table);

	/* Reads the table back and checks every entry. */
	error = dp_load(&table, &io);
	check(error == 0, "load the batch's table");
	check(table.count == 4, "four entries");
	check(table.restrictions == 0, "no restrictions");
	check(memcmp(untouched, table.edited + 128, 128) == 0, "the other system's entry is byte for byte the same");
	check(strcmp(table.parts[2].type, "516e7cb6-6ecf-11d6-8ff8-00022d09712b") == 0, "root type");
	check(strcmp(table.parts[2].uuid, "bbbbbbbb-0000-4000-8000-000000000003") == 0, "root PARTUUID");
	check(strcmp(table.parts[2].name, "zedBSD installed root") == 0, "root name");
	check(table.parts[2].start == 129U * mib, "root start kept");
	check(table.parts[2].count == 32U * mib, "root size kept");
	check(strcmp(table.parts[3].type, "516e7cb5-6ecf-11d6-8ff8-00022d09712b") == 0, "swap type");
	check(table.parts[3].attributes == 1, "swap attributes");

	/* Deletes both new entries, as a failed creation takes them back. */
	error = dp_delete(&table, 3);
	check(error == 0, "delete root");
	error = dp_delete(&table, 4);
	check(error == 0, "delete swap");
	error = dp_write(&table, &started);
	check(error == 0, "write the deletion");
	dp_free(&table);
	error = dp_load(&table, &io);
	check(error == 0, "load after the deletion");
	check(table.count == 2, "two entries after the deletion");
	memcpy(copy, table.edited + 128, 128);
	check(memcmp(untouched, copy, 128) == 0, "the other entry survives the deletion");
	dp_free(&table);

	/* Releases the disk. */
	free(untouched);
	free(copy);
	free(disk.bytes);
}
