/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's btsnoop record (ws143-p005 i02, see snoop.h).
 */

#include "userland/base/bluetoothd/snoop.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The datalink of HCI as H4 (a packet type byte first), and the microseconds from the year 0 to 1970. */
#define SNOOP_DATALINK_H4	1002U
#define SNOOP_EPOCH_US		UINT64_C(0x00dcddb30f2f8000)

/* The flags: received (not sent), and a command or an event (not data). */
#define SNOOP_FLAG_RECEIVED	0x01U
#define SNOOP_FLAG_CONTROL	0x02U

static void snoop_put32(uint8_t *bytes, uint32_t value);

/*
 * Builds the file's header.  Returns its length, or 0 when the buffer is
 * too small.
 */
size_t
btd_snoop_header(
	uint8_t *out,
	size_t size)
{
	/* Refuses a buffer too small. */
	if (size < BTD_SNOOP_HEADER)
		return 0U;

	/* The identification, the version and the datalink. */
	memcpy(out, "btsnoop\0", 8U);
	snoop_put32(&out[8], 1U);
	snoop_put32(&out[12], SNOOP_DATALINK_H4);
	return BTD_SNOOP_HEADER;
}

/*
 * Builds one packet's record: the original and included lengths (the
 * packet type byte counted), the flags, no drops, the time, the type byte
 * and the packet (cut to what fits).  Returns its length, or 0 when the
 * buffer cannot hold even the record's head and type byte.
 */
size_t
btd_snoop_record(
	uint8_t *out,
	size_t size,
	uint8_t packet_type,
	int received,
	uint64_t unix_us,
	const uint8_t *packet,
	size_t length)
{
	uint64_t stamp;
	uint32_t flags;
	size_t included;

	/* Refuses a buffer without room for the head and the type byte. */
	if (size < BTD_SNOOP_RECORD + 1U)
		return 0U;

	/* What is included of the packet. */
	included = length;
	if (included > size - BTD_SNOOP_RECORD - 1U)
		included = size - BTD_SNOOP_RECORD - 1U;

	/* The flags: the direction, and whether it is a command or an event. */
	flags = 0;
	if (received)
		flags |= SNOOP_FLAG_RECEIVED;
	if (packet_type == BTD_SNOOP_COMMAND || packet_type == BTD_SNOOP_EVENT)
		flags |= SNOOP_FLAG_CONTROL;

	/* The record's head. */
	snoop_put32(&out[0], (uint32_t)(length + 1U));
	snoop_put32(&out[4], (uint32_t)(included + 1U));
	snoop_put32(&out[8], flags);
	snoop_put32(&out[12], 0U);
	stamp = unix_us + SNOOP_EPOCH_US;
	snoop_put32(&out[16], (uint32_t)(stamp >> 32));
	snoop_put32(&out[20], (uint32_t)stamp);

	/* The type byte and the packet. */
	out[BTD_SNOOP_RECORD] = packet_type;
	memcpy(&out[BTD_SNOOP_RECORD + 1U], packet, included);
	return BTD_SNOOP_RECORD + 1U + included;
}

/*
 * Makes a new record file (0600, never through a link) and writes its
 * header.  Returns the descriptor, or -1 with errno.
 */
int
btd_snoop_open(
	const char *path)
{
	uint8_t header[BTD_SNOOP_HEADER];
	ssize_t written;
	size_t length;
	int descriptor;
	int error;

	/* The file. */
	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return -1;

	/* Its header. */
	length = btd_snoop_header(header, sizeof(header));
	written = write(descriptor, header, length);
	if (written != (ssize_t)length) {
		error = EIO;
		if (written < 0)
			error = errno;
		(void)close(descriptor);
		errno = error;
		return -1;
	}

	/* Succeeded: the open file. */
	return descriptor;
}

/*
 * Appends one packet's record at the time now (a failed write is not
 * reported: the record is a help, not the daemon's work).
 */
void
btd_snoop_write(
	int descriptor,
	uint8_t packet_type,
	int received,
	const uint8_t *packet,
	size_t length)
{
	uint8_t record[BTD_SNOOP_RECORD + 1U + BTD_SNOOP_PACKET_MAX];
	struct timespec now;
	uint64_t unix_us;
	size_t used;
	int error;

	/* Nothing without a file. */
	if (descriptor < 0)
		return;

	/* The time now. */
	unix_us = 0;
	error = clock_gettime(CLOCK_REALTIME, &now);
	if (error == 0)
		unix_us = (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;

	/* The record, written whole. */
	used = btd_snoop_record(record, sizeof(record), packet_type, received, unix_us, packet, length);
	if (used != 0U)
		(void)write(descriptor, record, used);
}

/* Writes a big-endian 32-bit number. */
static void
snoop_put32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;
}
