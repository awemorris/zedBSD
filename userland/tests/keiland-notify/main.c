/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-notify: posts a notification on the desktop (WS156 p002, the
 * tests' client) and prints what becomes of it.
 *
 *   keiland-notify [--app=NAME] [--urgent] [--action] [--replaces=ID]
 *                  [--withdraw-after-ms=N] [--wait-ms=N] TITLE [BODY]
 *
 * The lines, each on its own:
 *   KEILAND-NOTIFY open capabilities=0xB | none error=E
 *   KEILAND-NOTIFY posted request=R id=N
 *   KEILAND-NOTIFY result request=R error=E
 *   KEILAND-NOTIFY activated id=N
 *   KEILAND-NOTIFY closed id=N reason=dismissed|expired|cleared|withdrawn
 *   KEILAND-NOTIFY done status=S
 * It waits --wait-ms (default 2000) after the post for the events, or until
 * the notification closed; with --withdraw-after-ms it takes it back then.
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int notify_round(struct wl_display *display, struct kl_system *system, int timeout_ms);
static int notify_events(struct kl_system *system, uint32_t asked, uint32_t *id);
static const char *notify_reason(unsigned reason);
static unsigned long long notify_clock(void);
static const char *notify_value(const char *argument, const char *name);

/* Posts the notification and prints its events. */
int
main(
	int argc,
	char **argv)
{
	struct kl_notification notification;
	struct wl_display *display;
	struct kl_system *system;
	const char *value;
	unsigned long long started;
	unsigned long long now;
	unsigned long wait_ms;
	unsigned long withdraw_ms;
	uint32_t request;
	uint32_t id;
	int withdrawn;
	int closed;
	int option;
	int index;
	int error;

	/* The options, then the title and the body. */
	memset(&notification, 0, sizeof(notification));
	wait_ms = 2000UL;
	withdraw_ms = 0UL;
	for (index = 1; index < argc; index++) {
		/* An option, until the first word that is not one. */
		option = strncmp(argv[index], "--", 2);
		if (option != 0)
			break;

		/* Its value. */
		value = notify_value(argv[index], "--app=");
		if (value != NULL)
			notification.app = value;
		value = notify_value(argv[index], "--replaces=");
		if (value != NULL)
			notification.replaces = (uint32_t)strtoul(value, NULL, 10);
		value = notify_value(argv[index], "--wait-ms=");
		if (value != NULL)
			wait_ms = strtoul(value, NULL, 10);
		value = notify_value(argv[index], "--withdraw-after-ms=");
		if (value != NULL)
			withdraw_ms = strtoul(value, NULL, 10);
		option = strcmp(argv[index], "--urgent");
		if (option == 0)
			notification.flags |= KL_NOTIFY_URGENT;
		option = strcmp(argv[index], "--action");
		if (option == 0)
			notification.flags |= KL_NOTIFY_ACTION;
	}

	/* The title is needed. */
	if (index >= argc) {
		fprintf(stderr, "usage: keiland-notify [--app=NAME] [--urgent] [--action] [--replaces=ID] [--withdraw-after-ms=N] [--wait-ms=N] TITLE [BODY]\n");
		return 2;
	}

	/* The words. */
	notification.title = argv[index];
	if (index + 1 < argc)
		notification.body = argv[index + 1];

	/* The desktop and its system. */
	display = wl_display_connect(NULL);
	if (display == NULL) {
		printf("KEILAND-NOTIFY none error=%d\n", errno);
		return 1;
	}

	/* Its system. */
	system = kl_system_open(display);
	if (system == NULL) {
		printf("KEILAND-NOTIFY none error=%d\n", errno);
		wl_display_disconnect(display);
		return 1;
	}

	/* What it offers. */
	printf("KEILAND-NOTIFY open capabilities=0x%x\n", kl_system_capabilities(system));

	/* The post. */
	error = kl_system_notify(system, &notification, &request);
	if (error != 0) {
		printf("KEILAND-NOTIFY result request=0 error=%d\n", error);
		kl_system_close(system);
		wl_display_disconnect(display);
		return 1;
	}

	/* Its events, until it closed or the time is up (withdrawn on the way when asked). */
	id = 0U;
	withdrawn = 0;
	closed = 0;
	started = notify_clock();
	for (;;) {
		now = notify_clock();
		if (now - started >= wait_ms || closed)
			break;
		if (withdraw_ms != 0UL && !withdrawn && id != 0U && now - started >= withdraw_ms) {
			withdrawn = 1;
			(void)kl_system_notify_withdraw(system, id, NULL);
		}

		/* A round of the display, then its events. */
		error = notify_round(display, system, 50);
		if (error != 0)
			break;
		closed = notify_events(system, request, &id);
	}

	/* Done. */
	printf("KEILAND-NOTIFY done status=%d\n", error);
	fflush(stdout);
	kl_system_close(system);
	wl_display_disconnect(display);
	return 0;
}

/* Reads the display's events for up to timeout_ms, then dispatches the system; 0, or -1 when the display went. */
static int
notify_round(
	struct wl_display *display,
	struct kl_system *system,
	int timeout_ms)
{
	struct pollfd descriptor;
	unsigned changed;
	int prepared;
	int status;

	/* Dispatches what was read before, until the display may be read. */
	for (;;) {
		prepared = wl_display_prepare_read(display);
		if (prepared == 0)
			break;
		(void)wl_display_dispatch_pending(display);
	}

	/* Sends what is queued, and waits for the display's events. */
	(void)wl_display_flush(display);
	descriptor.fd = wl_display_get_fd(display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1, timeout_ms);
	if (status > 0)
		status = wl_display_read_events(display);
	else
		wl_display_cancel_read(display);

	/* A failed read means the display went. */
	if (status < 0)
		return -1;

	/* The application's queue, then the system's. */
	(void)wl_display_dispatch_pending(display);
	status = kl_system_dispatch(system, &changed);
	if (status != 0)
		return -1;
	return 0;
}

/* Prints the events that came; returns 1 once the notification closed. */
static int
notify_events(
	struct kl_system *system,
	uint32_t asked,
	uint32_t *id)
{
	struct kl_notify_event event;
	uint32_t request;
	int error;
	int taken;
	int closed;

	/* The results (a refusal of the post). */
	closed = 0;
	for (;;) {
		taken = kl_system_take_result(system, &request, &error);
		if (!taken)
			break;
		printf("KEILAND-NOTIFY result request=%u error=%d\n", request, error);
		if (request == asked && error != 0)
			closed = 1;
	}

	/* The notification's events. */
	for (;;) {
		taken = kl_system_take_notify_event(system, &event);
		if (!taken)
			break;
		if (event.kind == KL_NOTIFY_POSTED) {
			printf("KEILAND-NOTIFY posted request=%u id=%u\n", event.request, event.id);
			if (event.request == asked)
				*id = event.id;
		} else if (event.kind == KL_NOTIFY_ACTIVATED) {
			printf("KEILAND-NOTIFY activated id=%u\n", event.id);
		} else if (event.kind == KL_NOTIFY_CLOSED) {
			printf("KEILAND-NOTIFY closed id=%u reason=%s\n", event.id, notify_reason(event.reason));
			if (event.id == *id)
				closed = 1;
		}
	}

	/* Shown at once. */
	fflush(stdout);
	return closed;
}

/* Names a reason a notification closed. */
static const char *
notify_reason(
	unsigned reason)
{
	/* Each by its word. */
	switch (reason) {
	case KL_NOTIFY_DISMISSED:
		return "dismissed";
	case KL_NOTIFY_EXPIRED:
		return "expired";
	case KL_NOTIFY_CLEARED:
		return "cleared";
	case KL_NOTIFY_WITHDRAWN:
		return "withdrawn";
	default:
		break;
	}

	/* Another. */
	return "other";
}

/* Reads a steady clock in milliseconds. */
static unsigned long long
notify_clock(void)
{
	struct timespec now;
	int status;

	/* The monotonic clock (0 when it cannot be read). */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0ULL;
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Returns what follows an option's name in an argument, or NULL. */
static const char *
notify_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The name, then the value. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;
	return argument + length;
}
