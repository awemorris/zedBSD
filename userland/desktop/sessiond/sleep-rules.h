/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The answer sessiond gives a compositor's "POWER suspend" (sleep-rules.c,
 * ws052-p011, plan/ws052/phase007/phase.md section 1.1): one line made
 * from what the sleep's helper saw -- networkd's SLEEP_PREPARE, whether it
 * was cancelled, and the kernel's KERN_SYSTEM_SLEEP.  It knows nothing of
 * processes or sockets, so the host tests run it alone.
 */

#ifndef SESSIOND_SLEEP_RULES_H
#define SESSIOND_SLEEP_RULES_H

#include <stddef.h>
#include <stdint.h>

/* The longest answer line, without its newline. */
#define SESSIOND_SLEEP_LINE_MAX		160U

/* The longest device name the kernel gives (KERN_SYSTEM_SLEEP_DEVICE_MAX). */
#define SESSIOND_SLEEP_DEVICE_MAX	48U

/* What networkd's SLEEP_PREPARE came to. */
enum sessiond_sleep_network {
	/* The radios are off (or there was nothing to turn off). */
	SESSIOND_SLEEP_NETWORK_OFF,
	/* networkd is not running: the sleep goes on without it. */
	SESSIOND_SLEEP_NETWORK_ABSENT,
	/* A radio would not go off. */
	SESSIOND_SLEEP_NETWORK_RADIO,
	/* networkd did not answer in time. */
	SESSIOND_SLEEP_NETWORK_TIMEOUT,
	/* A confirmed network change waits to be decided. */
	SESSIOND_SLEEP_NETWORK_CONFIRMED,
	/* The user's own Wi-Fi request was under way. */
	SESSIOND_SLEEP_NETWORK_BUSY
};

/*
 * What the sleep's helper saw: networkd's part (and its error), whether
 * a cancel came before the kernel was asked, the errno of the ioctl itself
 * (0 when the kernel made the attempt), and the kernel's outcome (the
 * wake's name as the kernel's events say it, sleep.c).
 */
struct sessiond_sleep_seen {
	enum sessiond_sleep_network network;
	int network_error;
	unsigned cancelled;
	int ioctl_error;
	int32_t result;
	int32_t resume_result;
	const char *wake;
	char device[SESSIOND_SLEEP_DEVICE_MAX];
};

enum sessiond_sleep_network sessiond_sleep_network_of(int ok, int unavailable, int timed_out, int error, const char *stage);
int sessiond_sleep_answer(const struct sessiond_sleep_seen *seen, char *line, size_t size);
const char *sessiond_sleep_errno_name(int error);

#endif
