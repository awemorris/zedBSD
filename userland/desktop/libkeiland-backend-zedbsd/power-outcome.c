/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reads sessiond's answer to "POWER suspend" (sessiond/sleep-rules.c says
 * the lines, ws052-p011) into a sleep's outcome:
 *
 *   SLEPT woke=REASON [resume-error=ERRNO]
 *   NOSLEEP unsupported
 *   NOSLEEP device error=ERRNO device=NAME     (NAME may hold spaces: it is last)
 *   NOSLEEP network reason=WHY radio=IF error=ERRNO
 *   NOSLEEP cancelled
 *   ERROR busy
 *   ERROR                                     (or any line not understood)
 *
 * The errno names are read back into this system's numbers.
 */

#include "power-outcome.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* An errno and its name. */
struct outcome_errno {
	int error;
	const char *name;
};

/* The errno names sessiond's answers use (sessiond/sleep-rules.c). */
static const struct outcome_errno outcome_errnos[] = {
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

static int outcome_starts(const char *line, const char *word);
static const char *outcome_field(const char *line, const char *key);
static void outcome_copy_word(char *target, size_t size, const char *value);
static int outcome_errno_of(const char *value);
static enum kl_backend_sleep_network outcome_network_of(const char *value);

/*
 * Reads one answer line into outcome.  Returns the error session_answer
 * carries for it: 0 slept, EOPNOTSUPP unsupported, the device's error,
 * EBUSY for networkd or another sleep, ECANCELED, EIO otherwise.
 */
int
kl_backend_power_parse_outcome(
	const char *line,
	struct kl_backend_power_outcome *outcome)
{
	const char *value;
	int differs;
	int starts;

	/* Nothing known yet: an error unless the line says otherwise. */
	memset(outcome, 0, sizeof(*outcome));
	outcome->kind = KL_BACKEND_SLEEP_ERROR;
	outcome->error = EIO;

	/* Slept and woke, maybe a device not back. */
	starts = outcome_starts(line, "SLEPT");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_SLEPT;
		outcome->error = 0;
		value = outcome_field(line, "woke");
		outcome_copy_word(outcome->wake, sizeof(outcome->wake), value);
		value = outcome_field(line, "resume-error");
		if (value != NULL)
			outcome->resume_error = outcome_errno_of(value);
		return outcome->error;
	}

	/* The machine cannot sleep. */
	starts = outcome_starts(line, "NOSLEEP unsupported");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_UNSUPPORTED;
		outcome->error = EOPNOTSUPP;
		return outcome->error;
	}

	/* A device refused; its name is the rest of the line. */
	starts = outcome_starts(line, "NOSLEEP device");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_DEVICE;
		value = outcome_field(line, "error");
		outcome->error = EIO;
		if (value != NULL)
			outcome->error = outcome_errno_of(value);
		value = outcome_field(line, "device");
		if (value != NULL)
			snprintf(outcome->device, sizeof(outcome->device), "%s", value);
		differs = strcmp(outcome->device, "-");
		if (differs == 0)
			outcome->device[0] = '\0';
		return outcome->error;
	}

	/* networkd could not turn the radios off. */
	starts = outcome_starts(line, "NOSLEEP network");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_NETWORK;
		outcome->error = EBUSY;
		value = outcome_field(line, "reason");
		outcome->network = outcome_network_of(value);
		return outcome->error;
	}

	/* Cancelled before the kernel was asked. */
	starts = outcome_starts(line, "NOSLEEP cancelled");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_CANCELLED;
		outcome->error = ECANCELED;
		return outcome->error;
	}

	/* Another sleep is under way, or the machine is ending. */
	starts = outcome_starts(line, "ERROR busy");
	if (starts) {
		outcome->kind = KL_BACKEND_SLEEP_BUSY;
		outcome->error = EBUSY;
		return outcome->error;
	}

	/* Succeeded: anything else is an error of sessiond's. */
	return outcome->error;
}

/* Tells whether a line is a word, or starts with it and a space. */
static int
outcome_starts(
	const char *line,
	const char *word)
{
	size_t length;
	int differs;

	/* The word at the start. */
	length = strlen(word);
	differs = strncmp(line, word, length);
	if (differs != 0)
		return 0;

	/* Followed by the end or a space. */
	if (line[length] == '\0' || line[length] == ' ')
		return 1;

	/* Succeeded: a longer word. */
	return 0;
}

/* Gives the value after " KEY=" in a line, or NULL when it has none. */
static const char *
outcome_field(
	const char *line,
	const char *key)
{
	char pattern[32];
	const char *found;

	/* The key with its leading space and its equals sign. */
	snprintf(pattern, sizeof(pattern), " %s=", key);
	found = strstr(line, pattern);
	if (found == NULL)
		return NULL;

	/* Succeeded: what follows. */
	return found + strlen(pattern);
}

/* Copies one word of a value (up to a space) into target, empty for none. */
static void
outcome_copy_word(
	char *target,
	size_t size,
	const char *value)
{
	size_t length;

	/* No value. */
	target[0] = '\0';
	if (value == NULL)
		return;

	/* The word, cut to the target. */
	length = strcspn(value, " ");
	if (length >= size)
		length = size - 1U;
	memcpy(target, value, length);
	target[length] = '\0';
}

/* Gives the errno a name stands for (up to a space), EIO for one not known. */
static int
outcome_errno_of(
	const char *value)
{
	size_t length;
	size_t index;
	int differs;

	/* Each known name, whole. */
	length = strcspn(value, " ");
	for (index = 0U; index < sizeof(outcome_errnos) / sizeof(outcome_errnos[0]); index++) {
		if (strlen(outcome_errnos[index].name) != length)
			continue;
		differs = strncmp(value, outcome_errnos[index].name, length);
		if (differs == 0)
			return outcome_errnos[index].error;
	}

	/* Succeeded: one not known. */
	return EIO;
}

/* Gives why networkd could not turn the radios off. */
static enum kl_backend_sleep_network
outcome_network_of(
	const char *value)
{
	int starts;

	/* No reason given. */
	if (value == NULL)
		return KL_BACKEND_SLEEP_NETWORK_RADIO;

	/* Each reason word. */
	starts = outcome_starts(value, "timeout");
	if (starts)
		return KL_BACKEND_SLEEP_NETWORK_TIMEOUT;
	starts = outcome_starts(value, "confirmed");
	if (starts)
		return KL_BACKEND_SLEEP_NETWORK_CONFIRMED;
	starts = outcome_starts(value, "busy");
	if (starts)
		return KL_BACKEND_SLEEP_NETWORK_BUSY;

	/* Succeeded: a radio that would not go off. */
	return KL_BACKEND_SLEEP_NETWORK_RADIO;
}
