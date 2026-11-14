/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stand-ins for the parts of the compositor and its backend that the system
 * extension's files call but host-system.c does not look at: the event
 * loop's helpers (main.c), the I/O threads (io.c), the greeter, the
 * application notices and the session's key settings.  They cut system.c's
 * dependencies at the link: the parts the test controls (the network, the
 * sound, the power, the key store, the compositor's objects and clock) stay
 * in host-system.c.
 *
 * The test's event loop (host-system.c's server thread) passes every few
 * milliseconds, takes the timers due on its own clock each pass and counts
 * every pass as woken, so nothing here needs to wake it.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/*
 * Arms a part's timer for at_ms on the server's timers, or disarms it for
 * UINT64_MAX, as main.c does; the test's loop takes the timers due.
 */
void
kwl_timer_set(
	struct kwl_server *server,
	struct kwl_timer *timer,
	uint64_t at_ms)
{
	int error;
	int armed;

	/* No time: nothing waits for it. */
	if (at_ms == UINT64_MAX) {
		kwl_timer_disarm(&server->timers, timer);
		return;
	}

	/* Armed for that time already: nothing changes. */
	armed = kwl_timer_armed(timer);
	if (armed && timer->due_ms == at_ms)
		return;

	/* The timer fires at the first pass at or after its time; a full heap is a fault of the test. */
	error = kwl_timer_arm(&server->timers, timer, at_ms);
	if (error != 0)
		printf("FAIL stub: timer %s not armed (%d)\n", timer->name, error);
}

/*
 * Asks for a pass soon: the test's loop passes every few milliseconds
 * anyway.
 */
void
kwl_soon(
	struct kwl_server *server)
{
	/* Nothing to ask. */
	UNUSED_PARAMETER(server);
}

/*
 * Adds a descriptor to the next wait: the test's loop waits on its socket
 * alone and passes every few milliseconds, so nothing is added.
 */
void
kwl_watch(
	struct kwl_server *server,
	int descriptor,
	short events)
{
	/* Nothing to wait for. */
	UNUSED_PARAMETER(server);
	UNUSED_PARAMETER(descriptor);
	UNUSED_PARAMETER(events);
}

/*
 * Marks a client failed; the test has no retiring of clients, it reads the
 * mark.
 */
void
kwl_client_fail(
	struct kwl_client *client)
{
	/* Failed from now. */
	client->fatal = 1;
	client->fatal_time = kwl_milliseconds();
}

/*
 * Gives the file I/O thread a job: the test runs none, so the caller does
 * the work itself (io.c's answer when the thread does not run).
 */
int
kwl_io_file_submit(
	struct kwl_server *server,
	struct kwl_lane_job *job,
	const struct kwl_lane_ops *ops,
	uint64_t key)
{
	UNUSED_PARAMETER(server);
	UNUSED_PARAMETER(job);
	UNUSED_PARAMETER(ops);
	UNUSED_PARAMETER(key);

	/* No thread: the caller does it. */
	return ENOTSUP;
}

/*
 * Gives the device I/O thread a job: the test runs none, so the caller
 * does the work itself (io.c's answer when the thread does not run).
 */
int
kwl_io_device_submit(
	struct kwl_server *server,
	struct kwl_lane_job *job,
	const struct kwl_lane_ops *ops,
	uint64_t key)
{
	UNUSED_PARAMETER(server);
	UNUSED_PARAMETER(job);
	UNUSED_PARAMETER(ops);
	UNUSED_PARAMETER(key);

	/* No thread: the caller does it. */
	return ENOTSUP;
}

/*
 * Tells the greeter the keys changed: the test runs no greeter.
 */
void
kwl_greeter_keys_changed(
	struct kwl_server *server)
{
	/* No greeter. */
	UNUSED_PARAMETER(server);
}

/*
 * Posts an application's notice: the test shows none, so none is posted.
 */
uint32_t
kwl_notify_app_post(
	struct kwl_server *server,
	const char *app,
	const char *title,
	const char *body,
	const char *command,
	const char *lock_text)
{
	UNUSED_PARAMETER(server);
	UNUSED_PARAMETER(app);
	UNUSED_PARAMETER(title);
	UNUSED_PARAMETER(body);
	UNUSED_PARAMETER(command);
	UNUSED_PARAMETER(lock_text);

	/* No notice: the ID of none. */
	return 0U;
}

/*
 * Asks sessiond about the security key: the test's sessiond has no key
 * information (session-none.c's answer).
 */
int
kl_backend_session_key_info(
	struct kl_backend *backend)
{
	UNUSED_PARAMETER(backend);

	/* Not here. */
	return ENOTSUP;
}

/*
 * Gives the security key's information: none.
 */
void
kl_backend_session_key_info_get(
	const struct kl_backend *backend,
	struct kl_backend_key_info *info)
{
	/* None. */
	UNUSED_PARAMETER(backend);
	memset(info, 0, sizeof(*info));
}

/*
 * Changes the security key's PIN: not offered by the test's sessiond.
 */
int
kl_backend_session_key_pin(
	struct kl_backend *backend,
	const char *current,
	const char *pin)
{
	UNUSED_PARAMETER(backend);
	UNUSED_PARAMETER(current);
	UNUSED_PARAMETER(pin);

	/* Nothing is sent. */
	return ENOTSUP;
}

/*
 * Resets the security key: not offered by the test's sessiond.
 */
int
kl_backend_session_key_reset(
	struct kl_backend *backend,
	const char *password)
{
	UNUSED_PARAMETER(backend);
	UNUSED_PARAMETER(password);

	/* Nothing is sent. */
	return ENOTSUP;
}

/*
 * Gives how many keys a reset removed: none.
 */
unsigned
kl_backend_session_key_removed(
	const struct kl_backend *backend)
{
	UNUSED_PARAMETER(backend);

	/* None removed. */
	return 0U;
}

/*
 * Gives the unlocking methods allowed: every one (session-none.c's
 * answer).
 */
unsigned
kl_backend_session_methods_get(
	const struct kl_backend *backend)
{
	UNUSED_PARAMETER(backend);

	/* Every method. */
	return KL_BACKEND_METHODS_ALL;
}

/*
 * Sets the unlocking methods allowed: not offered by the test's sessiond.
 */
int
kl_backend_session_set_methods(
	struct kl_backend *backend,
	const char *password,
	unsigned methods)
{
	UNUSED_PARAMETER(backend);
	UNUSED_PARAMETER(password);
	UNUSED_PARAMETER(methods);

	/* Nothing is sent. */
	return ENOTSUP;
}

/*
 * Gives what a key's unlocking asks: both the PIN and the touch
 * (session-none.c's answer).
 */
void
kl_backend_session_options_get(
	const struct kl_backend *backend,
	unsigned *key_pin,
	unsigned *key_touch)
{
	/* Both asked. */
	UNUSED_PARAMETER(backend);
	*key_pin = 1U;
	*key_touch = 1U;
}

/*
 * Sets what a key's unlocking asks: not offered by the test's sessiond.
 */
int
kl_backend_session_set_options(
	struct kl_backend *backend,
	const char *password,
	unsigned key_pin,
	unsigned key_touch)
{
	UNUSED_PARAMETER(backend);
	UNUSED_PARAMETER(password);
	UNUSED_PARAMETER(key_pin);
	UNUSED_PARAMETER(key_touch);

	/* Nothing is sent. */
	return ENOTSUP;
}
