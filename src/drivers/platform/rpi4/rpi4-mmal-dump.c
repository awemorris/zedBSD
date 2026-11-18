/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame dump of the boot-time decoder probe (see rpi4-mmal-dump.h).
 *
 * The records are added by the probe's one thread and never change or go
 * away once published, so a reader needs no lock: the published count is
 * stored with release after the record is complete.  A write to the device
 * starts the probe (rpi4-mmal-probe.c waits for it).
 */

#include "drivers/platform/rpi4/rpi4-mmal-dump.h"
#include <kern/cdev.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

/* The device's number (a major of its own) and name. */
#define DUMP_DEVICE_NUMBER	0x00160000U
#define DUMP_DEVICE_NAME	"mmal-probe"

/* The most records, and the most bytes of frames, the dump keeps. */
#define DUMP_RECORDS		32U
#define DUMP_BUDGET		(32U * 1024U * 1024U)

/* One kept frame: its header line, padded, and its bytes. */
struct dump_record {
	char header[DRV_RPI4_MMAL_DUMP_HEADER];
	uint8_t *data;
	uint32_t bytes;
};

/* The records; dump_count of them are published. */
static struct dump_record dump_records[DUMP_RECORDS];
static unsigned dump_count;

/* The bytes of frames kept so far, and whether the device is published; the probe's thread only. */
static uint32_t dump_used;
static bool dump_published;

/*
 * Whether a write asked the probe to run: set by the first writer, which
 * then calls dump_on_run; never cleared (the probe runs once).
 */
static unsigned dump_run_requested;

/* What the first write starts (the probe's thread); set once before the device is published. */
static void (*dump_on_run)(void);

static ssize_t dump_read(struct file *file, void *buffer, size_t size);
static ssize_t dump_write(struct file *file, const void *buffer, size_t size);

/* The device: reads the records; a write starts the probe. */
static const struct cdev_ops dump_ops = {
	.read = dump_read,
	.write = dump_write,
	.flags = CDEV_READ_NEVER_WAITS,
};

/*
 * Publishes /dev/mmal-probe once.
 */
int
drv_rpi4_mmal_dump_publish(
	void (*on_run)(void))
{
	int error;

	/* Published once. */
	if (dump_published)
		return 0;
	dump_on_run = on_run;

	/* The device. */
	error = cdev_register(DUMP_DEVICE_NAME, (dev_t)DUMP_DEVICE_NUMBER, &dump_ops, NULL);
	if (error != 0) {
		kern_logf("rpi4-mmal-probe: /dev/%s not published (%d)\n", DUMP_DEVICE_NAME, error);
		return error;
	}

	/* Succeeded: the device reads the records and takes the request to run. */
	dump_published = true;
	return 0;
}

/*
 * Keeps a copy of one frame with its header line.
 */
int
drv_rpi4_mmal_dump_add(
	const char *header,
	const uint8_t *data,
	uint32_t bytes)
{
	struct dump_record *record;
	size_t length;

	/* Refuses a frame beyond the budgets. */
	if (dump_count >= DUMP_RECORDS || bytes > DUMP_BUDGET - dump_used)
		return ENOSPC;

	/* The header: the line, spaces, and a newline last. */
	record = &dump_records[dump_count];
	length = kern_strlen(header);
	if (length > DRV_RPI4_MMAL_DUMP_HEADER - 1U)
		length = DRV_RPI4_MMAL_DUMP_HEADER - 1U;
	kern_memset(record->header, ' ', sizeof(record->header));
	kern_memcpy(record->header, header, length);
	record->header[DRV_RPI4_MMAL_DUMP_HEADER - 1U] = '\n';

	/* The frame's bytes. */
	record->data = kern_malloc(bytes);
	if (record->data == NULL)
		return ENOMEM;
	kern_memcpy(record->data, data, bytes);
	record->bytes = bytes;
	dump_used += bytes;

	/* Publishes the record. */
	__atomic_store_n(&dump_count, dump_count + 1U, __ATOMIC_RELEASE);

	/* Succeeded: the frame is kept. */
	return 0;
}

/* Takes any write as the request to run the probe. */
static ssize_t
dump_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	unsigned expected;
	int first;

	/* The bytes do not matter; the first write starts the probe. */
	(void)file;
	(void)buffer;
	expected = 0;
	first = __atomic_compare_exchange_n(&dump_run_requested, &expected, 1U, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (first && dump_on_run != NULL)
		dump_on_run();

	/* Succeeded: every byte is taken. */
	return (ssize_t)size;
}

/*
 * Reads the records from the file's offset on: each one's header, then
 * its bytes.  Zero at the end.
 */
static ssize_t
dump_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	const struct dump_record *record;
	uint8_t *out;
	uint64_t offset;
	uint64_t start;
	uint64_t length;
	size_t done;
	size_t chunk;
	unsigned count;
	unsigned index;

	/* Finds the record and the place in it the offset names. */
	if (file->f_offset < 0)
		return -EINVAL;
	count = __atomic_load_n(&dump_count, __ATOMIC_ACQUIRE);
	offset = (uint64_t)file->f_offset;
	out = buffer;
	done = 0;
	start = 0;
	for (index = 0; index < count && done < size; index++) {
		record = &dump_records[index];
		length = DRV_RPI4_MMAL_DUMP_HEADER + (uint64_t)record->bytes;
		if (offset >= start + length) {
			start += length;
			continue;
		}

		/* The part of the header, then of the bytes, at the offset. */
		if (offset - start < DRV_RPI4_MMAL_DUMP_HEADER) {
			chunk = DRV_RPI4_MMAL_DUMP_HEADER - (size_t)(offset - start);
			if (chunk > size - done)
				chunk = size - done;
			kern_memcpy(out + done, record->header + (offset - start), chunk);
			done += chunk;
			offset += chunk;
		}

		/* Then the bytes. */
		if (done < size && offset - start >= DRV_RPI4_MMAL_DUMP_HEADER) {
			chunk = (size_t)(start + length - offset);
			if (chunk > size - done)
				chunk = size - done;
			kern_memcpy(out + done, record->data + (offset - start - DRV_RPI4_MMAL_DUMP_HEADER), chunk);
			done += chunk;
			offset += chunk;
		}

		/* The next record starts after this one. */
		start += length;
	}

	/* Succeeded: the bytes read; the offset moves past them. */
	file->f_offset = (off_t)offset;
	return (ssize_t)done;
}
