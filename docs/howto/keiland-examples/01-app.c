/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smallest Keiland application: it connects to the desktop, posts a
 * notification, listens to the desktop for two seconds and disconnects.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How long the program listens to the desktop, in microseconds. */
#define LISTEN_US	2000000U

int
main(void)
{
	struct kl_app_options options;
	struct kl_app_event event;
	struct kl_app *app;
	uint64_t deadline;
	uint64_t now;
	int status;
	int taken;
	int wait;

	/* Connects to the desktop (the compositor WAYLAND_DISPLAY names) as org.example.Hello. */
	memset(&options, 0, sizeof(options));
	options.application = "org.example.Hello";
	app = kl_app_open(&options);
	if (app == NULL) {
		fprintf(stderr, "hello: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Shows that the program is there, under its application ID. */
	status = kl_app_notify(app, "Hello", "A Keiland application is running.");
	if (status != 0)
		fprintf(stderr, "hello: no notification (%s)\n", strerror(status));

	/* Listens until the deadline: waits for the desktop, then takes what it queued. */
	deadline = kl_clock_us() + LISTEN_US;
	for (;;) {
		/* Ends at the deadline. */
		now = kl_clock_us();
		if (now >= deadline)
			break;

		/* Waits for the desktop, at most until the deadline. */
		wait = (int)((deadline - now) / 1000U);
		status = kl_app_dispatch(app, wait);
		if (status != 0) {
			fprintf(stderr, "hello: the desktop went away\n");
			break;
		}

		/* Takes every event queued, oldest first. */
		for (;;) {
			taken = kl_app_take(app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: light or dark, or the accent. */
			if (event.kind == KL_APP_THEME)
				printf("hello: the appearance changed\n");
		}
	}

	/* Disconnects. */
	kl_app_close(app);

	/* Succeeded: the program connected and listened. */
	printf("hello: done\n");
	return 0;
}
