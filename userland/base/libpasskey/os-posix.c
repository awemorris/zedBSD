/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The report functions on an open raw HID node (ws161-p004; os.h), the
 * same on zedBSD's /dev/input/hidrawN and Linux's /dev/hidrawN: an output
 * report is written whole (its first byte is the report ID, 0, which the
 * kernel does not send); an input report is read whole after poll() says
 * one waits.
 */

#include "os.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

static int os_write(void *context, const uint8_t *report, size_t size);
static int os_read(void *context, uint8_t *report, size_t size, unsigned timeout_ms);

/* Gives the report functions on an open node. */
void
pk_os_posix_io(
	struct pk_os_hid *handle,
	struct pk_hid_io *io)
{
	/* The node's write and read. */
	io->context = handle;
	io->write = os_write;
	io->read = os_read;
}

/* Closes a key's node (the grab goes with it). */
void
pk_os_close(
	struct pk_os_hid *handle)
{
	/* Once. */
	if (handle->descriptor < 0)
		return;
	(void)close(handle->descriptor);
	handle->descriptor = -1;
}

/* Sends one output report (its ID's byte, then the report). */
static int
os_write(
	void *context,
	const uint8_t *report,
	size_t size)
{
	struct pk_os_hid *handle;
	ssize_t written;

	/* The whole report at once. */
	handle = context;
	written = write(handle->descriptor, report, size);
	if (written < 0)
		return errno;
	if ((size_t)written != size)
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Waits at most timeout_ms for one input report of size bytes. */
static int
os_read(
	void *context,
	uint8_t *report,
	size_t size,
	unsigned timeout_ms)
{
	struct pk_os_hid *handle;
	struct pollfd wait;
	ssize_t got;
	int ready;

	/* A report to read, or the time out (a signal waits again). */
	handle = context;
	wait.fd = handle->descriptor;
	wait.events = POLLIN;
	wait.revents = 0;
	do {
		ready = poll(&wait, 1, (int)timeout_ms);
	} while (ready < 0 && errno == EINTR);
	if (ready < 0)
		return errno;
	if (ready == 0)
		return ETIMEDOUT;
	if ((wait.revents & (POLLHUP | POLLERR)) != 0 && (wait.revents & POLLIN) == 0)
		return ENODEV;

	/* The report, whole. */
	got = read(handle->descriptor, report, size);
	if (got < 0)
		return errno;
	if ((size_t)got != size)
		return EIO;

	/* Succeeded. */
	return 0;
}
