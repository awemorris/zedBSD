/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the USB HID report ring (src/drivers/usb/usb-hid-ring.c):
 * the records come back oldest first with their sequence, a full ring
 * overwrites the oldest, a reader behind the ring moves to the oldest
 * record held, and reads of any size put the byte stream back together.
 */

#include <drivers/usb/usb-hid-ring.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define RECORD	sizeof(struct drv_usb_hid_ring_record)

static unsigned failures;
static unsigned checks;

static void check(int condition, const char *what);
static void make(struct drv_usb_hid_ring_record *record, unsigned value);
static size_t read_all(const struct drv_usb_hid_ring *ring, uint64_t *offset, uint8_t *out, size_t capacity, size_t step);

int
main(
	void)
{
	static struct drv_usb_hid_ring_record storage[8];
	static uint8_t out[RECORD * 64];
	struct drv_usb_hid_ring_record record;
	const struct drv_usb_hid_ring_record *got;
	struct drv_usb_hid_ring ring;
	uint64_t offset;
	uint64_t next;
	size_t bytes;
	size_t step;
	unsigned i;
	int ordered;

	/* Refuses no records and a capacity that is not a power of two. */
	check(drv_usb_hid_ring_init(&ring, NULL, 8) == EINVAL, "init refuses no records");
	check(drv_usb_hid_ring_init(&ring, storage, 0) == EINVAL, "init refuses capacity 0");
	check(drv_usb_hid_ring_init(&ring, storage, 6) == EINVAL, "init refuses capacity 6");
	check(drv_usb_hid_ring_init(&ring, storage, 8) == 0, "init takes capacity 8");

	/* An empty ring reads nothing and leaves the offset. */
	bytes = drv_usb_hid_ring_read(&ring, 0, out, sizeof(out), &next);
	check(bytes == 0 && next == 0, "empty ring reads 0 bytes");

	/* Three records come back in order with sequences 0, 1, 2 and their fields. */
	for (i = 0; i < 3; i++) {
		make(&record, i);
		record.sequence = 999;
		drv_usb_hid_ring_put(&ring, &record);
	}
	offset = 0;
	bytes = read_all(&ring, &offset, out, sizeof(out), sizeof(out));
	got = (const struct drv_usb_hid_ring_record *)(const void *)out;
	check(bytes == 3 * RECORD && offset == 3 * RECORD, "3 records read whole");
	check(got[0].sequence == 0 && got[1].sequence == 1 && got[2].sequence == 2, "sequences 0 1 2 (the ring's, not the caller's)");
	check(got[2].bytes[0] == 2 && got[2].length == 7 && got[2].interface == 2 && got[2].counter_us == 2000, "fields kept");

	/* A reader that caught up reads nothing until a new record, then only that one. */
	bytes = drv_usb_hid_ring_read(&ring, offset, out, sizeof(out), &next);
	check(bytes == 0 && next == offset, "caught-up reader reads 0");
	make(&record, 3);
	drv_usb_hid_ring_put(&ring, &record);
	bytes = drv_usb_hid_ring_read(&ring, offset, out, sizeof(out), &next);
	check(bytes == RECORD && got[0].sequence == 3 && next == 4 * RECORD, "next read gets only the new record");

	/* Twenty records in all overwrite the oldest: 12 to 19 are held, in order. */
	for (i = 4; i < 20; i++) {
		make(&record, i);
		drv_usb_hid_ring_put(&ring, &record);
	}
	offset = 0;
	bytes = read_all(&ring, &offset, out, sizeof(out), sizeof(out));
	check(bytes == 8 * RECORD && offset == 20 * RECORD, "wrapped ring reads 8 records from offset 0");
	ordered = 1;
	for (i = 0; i < 8; i++) {
		if (got[i].sequence != 12 + i || got[i].bytes[0] != (uint8_t)(12 + i) || got[i].counter_us != (12 + i) * 1000ULL)
			ordered = 0;
	}
	check(ordered, "wrapped ring holds 12..19 oldest first");

	/* A reader whose record was overwritten moves to the oldest held. */
	bytes = drv_usb_hid_ring_read(&ring, 5 * RECORD + 10, out, RECORD, &next);
	check(bytes == RECORD && got[0].sequence == 12 && next == 13 * RECORD, "overwritten offset moves to the oldest");

	/* Reads of odd sizes put the same bytes back together. */
	for (step = 1; step <= 2 * RECORD + 3; step += 7) {
		static uint8_t whole[RECORD * 8];

		offset = 0;
		bytes = read_all(&ring, &offset, whole, sizeof(whole), sizeof(whole));
		offset = 12 * RECORD;
		if (read_all(&ring, &offset, out, sizeof(out), step) != bytes || memcmp(out, whole, bytes) != 0) {
			check(0, "odd-sized reads match the whole read");
			break;
		}
	}
	check(step > 2 * RECORD + 3, "reads of 1..131 bytes rebuild the stream");

	/* A read from the middle of a held record continues it. */
	bytes = drv_usb_hid_ring_read(&ring, 15 * RECORD + 16, out, RECORD - 16, &next);
	check(bytes == RECORD - 16 && next == 16 * RECORD, "mid-record offset continues that record");
	check(memcmp(out, (const uint8_t *)&storage[15 & 7] + 16, RECORD - 16) == 0, "mid-record bytes are record 15's tail");

	/* A sequence past the newest reads nothing. */
	bytes = drv_usb_hid_ring_read(&ring, 40 * RECORD, out, sizeof(out), &next);
	check(bytes == 0 && next == 40 * RECORD, "offset past the newest reads 0");

	/* Many wraps keep the newest capacity records in order. */
	for (i = 20; i < 1000; i++) {
		make(&record, i);
		drv_usb_hid_ring_put(&ring, &record);
	}
	offset = 0;
	bytes = read_all(&ring, &offset, out, sizeof(out), RECORD * 3);
	ordered = bytes == 8 * RECORD;
	for (i = 0; ordered && i < 8; i++) {
		if (got[i].sequence != 992 + i)
			ordered = 0;
	}
	check(ordered && offset == 1000 * RECORD, "after 1000 records holds 992..999");

	/* The summary. */
	if (failures != 0) {
		printf("host-ring: FAIL %u/%u\n", failures, checks);
		return 1;
	}
	printf("host-ring: PASS %u/%u\n", checks, checks);
	return 0;
}

/* Counts one check and prints a failed one. */
static void
check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
		return;
	}
	printf("ok: %s\n", what);
}

/* Fills a record whose fields all follow from value. */
static void
make(
	struct drv_usb_hid_ring_record *record,
	unsigned value)
{
	memset(record, 0, sizeof(*record));
	record->counter_us = value * 1000ULL;
	record->monotonic_us = value * 1000ULL;
	record->length = 7;
	record->bus = 0;
	record->address = 1;
	record->interface = (uint8_t)(value % 3U);
	record->endpoint = 0x82;
	record->kept = 7;
	record->bytes[0] = (uint8_t)value;
	record->bytes[1] = 0x5a;
}

/* Reads from *offset until nothing is left, step bytes a read, and gives the bytes read. */
static size_t
read_all(
	const struct drv_usb_hid_ring *ring,
	uint64_t *offset,
	uint8_t *out,
	size_t capacity,
	size_t step)
{
	size_t done;
	size_t got;
	size_t want;

	done = 0;
	while (done < capacity) {
		want = step;
		if (want > capacity - done)
			want = capacity - done;
		got = drv_usb_hid_ring_read(ring, *offset, out + done, want, offset);
		done += got;
		if (got == 0)
			break;
	}
	return done;
}
