/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The answer to a compositor's "POWER suspend" (ws052-p011): the line made
 * from what the sleep's helper saw, the class of networkd's answer to
 * SLEEP_PREPARE, and the errno names the line carries.
 *
 *   SLEPT woke=REASON [resume-error=ERRNO]   slept and woke
 *   NOSLEEP unsupported                       the machine cannot sleep
 *   NOSLEEP device error=ERRNO device=NAME    a device refused (NAME may hold spaces: it is last)
 *   NOSLEEP network reason=WHY radio=- error=ERRNO
 *                                             networkd could not turn the radios off
 *   NOSLEEP cancelled                         a cancel came before the kernel was asked
 *   ERROR busy                                another sleep is under way
 *   ERROR                                     anything else
 */

#include "sleep-rules.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* An errno and its name. */
struct sleep_errno {
	int error;
	const char *name;
};

/* The errno names the answers use. */
static const struct sleep_errno sleep_errnos[] = {
	{ EPERM, "EPERM" },
	{ ENOENT, "ENOENT" },
	{ EINTR, "EINTR" },
	{ EIO, "EIO" },
	{ ENXIO, "ENXIO" },
	{ EAGAIN, "EAGAIN" },
	{ ENOMEM, "ENOMEM" },
	{ EACCES, "EACCES" },
	{ EBUSY, "EBUSY" },
	{ ENODEV, "ENODEV" },
	{ EINVAL, "EINVAL" },
	{ ENOSPC, "ENOSPC" },
	{ ETIMEDOUT, "ETIMEDOUT" },
	{ ECONNREFUSED, "ECONNREFUSED" },
	{ EOPNOTSUPP, "EOPNOTSUPP" },
	{ ECANCELED, "ECANCELED" }
};

static const char *sleep_network_reason(enum sessiond_sleep_network network);

/*
 * Classes networkd's answer to SLEEP_PREPARE: ok (the radios are off),
 * unavailable (networkd is not running: no socket, or nobody listening),
 * timed out, or refused with an error and its stage ("confirmed
 * transaction", "Wi-Fi operation in progress; retry", "sleep: Wi-Fi
 * radio").
 */
enum sessiond_sleep_network
sessiond_sleep_network_of(
	int ok,
	int unavailable,
	int timed_out,
	int error,
	const char *stage)
{
	const char *found;

	/* networkd is not there: nothing to turn off. */
	if (unavailable)
		return SESSIOND_SLEEP_NETWORK_ABSENT;

	/* No answer in time. */
	if (timed_out)
		return SESSIOND_SLEEP_NETWORK_TIMEOUT;

	/* The radios are off. */
	if (ok)
		return SESSIOND_SLEEP_NETWORK_OFF;

	/* A confirmed change waits. */
	found = NULL;
	if (stage != NULL)
		found = strstr(stage, "confirmed");
	if (found != NULL)
		return SESSIOND_SLEEP_NETWORK_CONFIRMED;

	/* The user's own Wi-Fi request is under way. */
	if (stage != NULL)
		found = strstr(stage, "in progress");
	if (found != NULL && error == EBUSY)
		return SESSIOND_SLEEP_NETWORK_BUSY;

	/* Succeeded: a radio would not go off. */
	return SESSIOND_SLEEP_NETWORK_RADIO;
}

/*
 * Makes the answer line (without its newline) from what the helper saw.
 * Returns 0, or ENOSPC when the line did not fit (it is then cut).
 */
int
sessiond_sleep_answer(
	const struct sessiond_sleep_seen *seen,
	char *line,
	size_t size)
{
	const char *device;
	int length;

	/* A cancel before the kernel was asked. */
	if (seen->cancelled) {
		length = snprintf(line, size, "NOSLEEP cancelled");
	} else if (seen->network != SESSIOND_SLEEP_NETWORK_OFF && seen->network != SESSIOND_SLEEP_NETWORK_ABSENT) {
		/* networkd could not turn the radios off: the kernel was not asked. */
		length = snprintf(line, size, "NOSLEEP network reason=%s radio=- error=%s",
				  sleep_network_reason(seen->network), sessiond_sleep_errno_name(seen->network_error));
	} else if (seen->ioctl_error == EOPNOTSUPP) {
		/* The machine cannot sleep to idle. */
		length = snprintf(line, size, "NOSLEEP unsupported");
	} else if (seen->ioctl_error == EBUSY) {
		/* Another sleep is under way. */
		length = snprintf(line, size, "ERROR busy");
	} else if (seen->ioctl_error != 0) {
		/* The kernel refused the request otherwise. */
		length = snprintf(line, size, "ERROR");
	} else if (seen->result != 0) {
		/* A device refused; the machine did not sleep. */
		device = seen->device;
		if (device[0] == '\0')
			device = "-";
		length = snprintf(line, size, "NOSLEEP device error=%s device=%.*s",
				  sessiond_sleep_errno_name(seen->result), (int)SESSIOND_SLEEP_DEVICE_MAX, device);
	} else if (seen->resume_result != 0) {
		/* Slept and woke, a device not back. */
		length = snprintf(line, size, "SLEPT woke=%s resume-error=%s", seen->wake, sessiond_sleep_errno_name(seen->resume_result));
	} else {
		/* Slept and woke. */
		length = snprintf(line, size, "SLEPT woke=%s", seen->wake);
	}

	/* A line cut short. */
	if (length < 0 || (size_t)length >= size)
		return ENOSPC;

	/* Succeeded: the line is whole. */
	return 0;
}

/* Gives an errno's name ("EBUSY"), or "EUNKNOWN" for one the answers do not name. */
const char *
sessiond_sleep_errno_name(
	int error)
{
	size_t index;

	/* Each named errno. */
	for (index = 0U; index < sizeof(sleep_errnos) / sizeof(sleep_errnos[0]); index++) {
		if (sleep_errnos[index].error == error)
			return sleep_errnos[index].name;
	}

	/* Succeeded: one without a name here. */
	return "EUNKNOWN";
}

/* Gives the reason word of a networkd failure in the answer. */
static const char *
sleep_network_reason(
	enum sessiond_sleep_network network)
{
	/* Each failure. */
	switch (network) {
	case SESSIOND_SLEEP_NETWORK_TIMEOUT:
		return "timeout";
	case SESSIOND_SLEEP_NETWORK_CONFIRMED:
		return "confirmed";
	case SESSIOND_SLEEP_NETWORK_BUSY:
		return "busy";
	case SESSIOND_SLEEP_NETWORK_RADIO:
	case SESSIOND_SLEEP_NETWORK_OFF:
	case SESSIOND_SLEEP_NETWORK_ABSENT:
	default:
		break;
	}

	/* Succeeded: a radio would not go off. */
	return "radio";
}
