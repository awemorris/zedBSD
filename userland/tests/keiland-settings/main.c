/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-settings: a client of libkeiland's settings for the tests of
 * WS135 (ws135-p003).
 *
 *   keiland-settings [--app=NAME] [--timeout-ms=N] COMMAND ...
 *
 * The commands run in order: "get KEY", "dump" (every key), "set KEY
 * VALUE", "reset KEY" (each waits up to the timeout for its result) and
 * "watch PREFIX" (the changes of the keys starting with PREFIX until the
 * timeout).  Each line starts with KEILAND-SETTINGS:
 *
 *   KEILAND-SETTINGS value key=K value=V flags=F | error=E   (E: 0 or the errno's name)
 *   KEILAND-SETTINGS result request=R error=E
 *   KEILAND-SETTINGS change key=K value=V flags=F   (V is - when none)
 *   KEILAND-SETTINGS done status=S
 */

#include <keiland/keiland.h>

#include <wayland-client.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Marks a callback argument the probe does not inspect. */
#define UNUSED_PARAMETER(name) ((void)(name))

/* The commands the probe runs, as probe_kind tells them from the words. */
enum probe_kind {
	PROBE_NONE,
	PROBE_GET,
	PROBE_DUMP,
	PROBE_SET,
	PROBE_RESET,
	PROBE_WATCH
};

/* The keys "dump" prints, the desktop's in the table's order. */
static const char *const probe_keys[] = {
	"wallpaper",
	"window.opacity",
	"mouse.speed",
	"mouse.acceleration",
	"mouse.natural",
	"touchpad.speed",
	"touchpad.acceleration",
	"touchpad.natural",
	"keyboard.repeat.rate",
	"keyboard.repeat.delay",
	"sound.volume",
	"sound.muted",
	"sound.available",
	"terminal.ambiguous-wide"
};

static int probe_options(int argc, char **argv, const char **app, int *timeout_ms);
static enum probe_kind probe_kind(int argc, char **argv, int arg);
static int probe_run(struct wl_display *display, struct kl_settings *settings, enum probe_kind kind, char **argv, int arg, int timeout_ms, int *status);
static void probe_get(struct kl_settings *settings, const char *key);
static void probe_watch(struct wl_display *display, struct kl_settings *settings, const char *prefix, int timeout_ms);
static int probe_wait(struct wl_display *display, struct kl_settings *settings, uint32_t request, int timeout_ms);
static int probe_round(struct wl_display *display, struct kl_settings *settings, int timeout_ms);
static void probe_change(void *data, const char *key, const char *value, unsigned flags);
static unsigned long long probe_clock_ms(void);
static const char *probe_error(int error);

/*
 * Runs the commands on the command line against the desktop's settings.
 *
 * Exits 0 when every command ran and every request was answered, or 1.
 */
int
main(
	int argc,
	char **argv)
{
	struct wl_display *display;
	struct kl_settings *settings;
	enum probe_kind kind;
	const char *app;
	int timeout_ms;
	int status;
	int arg;

	/* Prints each line as it is made, so that a test reading the pipe sees it at once. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* Reads the options; the commands follow them. */
	arg = probe_options(argc, argv, &app, &timeout_ms);

	/* Connects to the compositor. */
	display = wl_display_connect(NULL);
	if (display == NULL) {
		printf("KEILAND-SETTINGS failed step=connect errno=%d\n", errno);
		return 1;
	}

	/* Opens the settings on the display. */
	settings = kl_settings_open(display, app);
	if (settings == NULL) {
		printf("KEILAND-SETTINGS failed step=open errno=%d\n", errno);
		return 1;
	}

	/* The log line the tests read. */
	printf("KEILAND-SETTINGS open app=%s\n", app != NULL ? app : "-");

	/* Runs each command in turn. */
	status = 0;
	while (arg < argc) {
		/* Tells which command the word is. */
		kind = probe_kind(argc, argv, arg);

		/* A word that is no command ends the run. */
		if (kind == PROBE_NONE) {
			printf("KEILAND-SETTINGS failed step=usage word=%s\n", argv[arg]);
			status = 1;
			break;
		}

		/* Runs the command; it gives where the next one starts. */
		arg = probe_run(display, settings, kind, argv, arg, timeout_ms, &status);
	}

	/* The log line the tests read. */
	printf("KEILAND-SETTINGS done status=%d\n", status);

	/* Lets the settings and the display go. */
	kl_settings_close(settings);
	wl_display_disconnect(display);

	/* Reports a command that failed or was not answered. */
	if (status != 0)
		return status;

	/* Succeeded: every command ran and was answered. */
	return 0;
}

/* Reads the leading options (--app=NAME, --timeout-ms=N); returns where the commands start. */
static int
probe_options(
	int argc,
	char **argv,
	const char **app,
	int *timeout_ms)
{
	int differs;
	int arg;

	/* The defaults: no application, and three seconds for each wait. */
	*app = NULL;
	*timeout_ms = 3000;

	/* Reads each leading word that starts with "--". */
	for (arg = 1; arg < argc; arg++) {
		/* The first word that is no option ends them. */
		differs = strncmp(argv[arg], "--", 2);
		if (differs != 0)
			break;

		/* --app=NAME names the application's own settings. */
		differs = strncmp(argv[arg], "--app=", 6);
		if (differs == 0) {
			*app = argv[arg] + 6;
			continue;
		}

		/* --timeout-ms=N bounds each wait. */
		differs = strncmp(argv[arg], "--timeout-ms=", 13);
		if (differs == 0)
			*timeout_ms = atoi(argv[arg] + 13);
	}

	/* Reports where the commands start. */
	return arg;
}

/* Tells which command the word at arg is; PROBE_NONE for any other word, or a command without its words. */
static enum probe_kind
probe_kind(
	int argc,
	char **argv,
	int arg)
{
	int differs;

	/* get KEY. */
	differs = strcmp(argv[arg], "get");
	if (differs == 0 && arg + 1 < argc)
		return PROBE_GET;

	/* dump. */
	differs = strcmp(argv[arg], "dump");
	if (differs == 0)
		return PROBE_DUMP;

	/* set KEY VALUE. */
	differs = strcmp(argv[arg], "set");
	if (differs == 0 && arg + 2 < argc)
		return PROBE_SET;

	/* reset KEY. */
	differs = strcmp(argv[arg], "reset");
	if (differs == 0 && arg + 1 < argc)
		return PROBE_RESET;

	/* watch PREFIX. */
	differs = strcmp(argv[arg], "watch");
	if (differs == 0 && arg + 1 < argc)
		return PROBE_WATCH;

	/* Any other word is no command. */
	return PROBE_NONE;
}

/* Runs one command; a request not answered sets *status to 1.  Returns where the next command starts. */
static int
probe_run(
	struct wl_display *display,
	struct kl_settings *settings,
	enum probe_kind kind,
	char **argv,
	int arg,
	int timeout_ms,
	int *status)
{
	uint32_t request;
	size_t index;
	int missing;
	int error;
	int next;

	/* Runs the command the word names. */
	switch (kind) {
	case PROBE_GET:
		/* Prints the key's value. */
		probe_get(settings, argv[arg + 1]);
		next = arg + 2;
		break;
	case PROBE_DUMP:
		/* Prints every key's value. */
		for (index = 0; index < sizeof(probe_keys) / sizeof(probe_keys[0]); index++)
			probe_get(settings, probe_keys[index]);
		next = arg + 1;
		break;
	case PROBE_SET:
		/* Asks for the key to take the value. */
		error = kl_settings_set(settings, argv[arg + 1], argv[arg + 2], &request);
		printf("KEILAND-SETTINGS set key=%s value=%s error=%s\n", argv[arg + 1], argv[arg + 2], probe_error(error));

		/* Waits for the answer to a request that was sent. */
		if (error == 0) {
			missing = probe_wait(display, settings, request, timeout_ms);
			*status |= missing;
		}
		next = arg + 3;
		break;
	case PROBE_RESET:
		/* Asks for the key to go back to its default. */
		error = kl_settings_reset(settings, argv[arg + 1], &request);
		printf("KEILAND-SETTINGS reset key=%s error=%s\n", argv[arg + 1], probe_error(error));

		/* Waits for the answer to a request that was sent. */
		if (error == 0) {
			missing = probe_wait(display, settings, request, timeout_ms);
			*status |= missing;
		}
		next = arg + 2;
		break;
	case PROBE_WATCH:
		/* Prints the changes of the keys until the timeout. */
		probe_watch(display, settings, argv[arg + 1], timeout_ms);
		next = arg + 2;
		break;
	default:
		/* No other command is run (probe_kind refused it). */
		next = arg + 1;
		break;
	}

	/* Reports where the next command starts. */
	return next;
}

/* Prints a key's value, or why there is none. */
static void
probe_get(
	struct kl_settings *settings,
	const char *key)
{
	char value[KL_SETTINGS_VALUE_MAX];
	unsigned flags;
	int error;

	/* Copies the value and its flags; a key without one prints why. */
	flags = 0;
	error = kl_settings_get(settings, key, value, sizeof(value), &flags);
	if (error != 0) {
		printf("KEILAND-SETTINGS value key=%s error=%s\n", key, probe_error(error));
		return;
	}

	/* The log line the tests read. */
	printf("KEILAND-SETTINGS value key=%s value=%s flags=%u\n", key, value, flags);
}

/* Watches the keys starting with prefix, printing their changes until the timeout. */
static void
probe_watch(
	struct wl_display *display,
	struct kl_settings *settings,
	const char *prefix,
	int timeout_ms)
{
	unsigned long long until;
	unsigned long long now;
	int error;

	/* Adds the watch. */
	error = kl_settings_watch(settings, prefix, probe_change, NULL, NULL);

	/* The log line the tests read. */
	printf("KEILAND-SETTINGS watch prefix=%s error=%s\n", prefix, probe_error(error));

	/* Takes rounds until the timeout; the watch prints each change. */
	now = probe_clock_ms();
	until = now + (unsigned long long)timeout_ms;
	for (;;) {
		/* The timeout ends the watch. */
		now = probe_clock_ms();
		if (now >= until)
			break;

		/* Reads and dispatches for the time left; a display that went ends the watch. */
		error = probe_round(display, settings, (int)(until - now));
		if (error != 0)
			break;
	}
}

/* Waits for a request's result (watching nothing meanwhile); returns 0, or 1 when it does not come. */
static int
probe_wait(
	struct wl_display *display,
	struct kl_settings *settings,
	uint32_t request,
	int timeout_ms)
{
	unsigned long long until;
	unsigned long long now;
	uint32_t finished;
	int answered;
	int answer;
	int taken;
	int error;

	/* Sends the request now. */
	(void)wl_display_flush(display);

	/* Takes rounds until the result or the timeout. */
	answered = 0;
	finished = 0;
	answer = 0;
	now = probe_clock_ms();
	until = now + (unsigned long long)timeout_ms;
	for (;;) {
		/* Takes a finished request; this one's ends the wait. */
		taken = kl_settings_take_result(settings, &finished, &answer);
		if (taken && finished == request) {
			answered = 1;
			break;
		}

		/* Another request's result: the next may be this one's. */
		if (taken)
			continue;

		/* The timeout ends the wait. */
		now = probe_clock_ms();
		if (now >= until)
			break;

		/* Reads and dispatches for the time left; a display that went ends the wait. */
		error = probe_round(display, settings, (int)(until - now));
		if (error != 0)
			break;
	}

	/* No answer came. */
	if (!answered) {
		printf("KEILAND-SETTINGS result request=%u missing\n", request);
		return 1;
	}

	/* The log line the tests read. */
	printf("KEILAND-SETTINGS result request=%u error=%s\n", finished, probe_error(answer));

	/* Succeeded: the answer came. */
	return 0;
}

/* Reads the display's events for up to timeout_ms, then dispatches the settings; returns 0 or -1 when the display went. */
static int
probe_round(
	struct wl_display *display,
	struct kl_settings *settings,
	int timeout_ms)
{
	struct pollfd descriptor;
	int prepared;
	int status;

	/* Dispatches the events read before, until the display may be read. */
	for (;;) {
		prepared = wl_display_prepare_read(display);
		if (prepared == 0)
			break;
		(void)wl_display_dispatch_pending(display);
	}

	/* Sends what is queued. */
	(void)wl_display_flush(display);

	/* Waits for the display's events. */
	descriptor.fd = wl_display_get_fd(display);
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	status = poll(&descriptor, 1, timeout_ms);
	if (status > 0) {
		/* Reads the events that came. */
		status = wl_display_read_events(display);
	} else {
		/* Nothing came: the read is given up. */
		wl_display_cancel_read(display);
	}

	/* A wait or a read that failed means the display went. */
	if (status < 0)
		return -1;

	/* Dispatches the application's queue. */
	(void)wl_display_dispatch_pending(display);

	/* Then the settings'. */
	status = kl_settings_dispatch(settings);
	if (status != 0)
		printf("KEILAND-SETTINGS dispatch error=%d\n", status);

	/* Succeeded: the round is over. */
	return 0;
}

/* Prints a change a watch heard. */
static void
probe_change(
	void *data,
	const char *key,
	const char *value,
	unsigned flags)
{
	UNUSED_PARAMETER(data);

	/* The log line the tests read; a key with no value prints -. */
	printf("KEILAND-SETTINGS change key=%s value=%s flags=%u\n", key, value != NULL ? value : "-", flags);
}

/* Reads a steady clock in milliseconds. */
static unsigned long long
probe_clock_ms(
	void)
{
	struct timespec now;

	/* Reads the monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in milliseconds. */
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Names an errno value the settings give, so that the tests read the same on every system. */
static const char *
probe_error(
	int error)
{
	/* Names each errno value the settings give. */
	switch (error) {
	case 0:
		return "0";
	case ENOENT:
		return "ENOENT";
	case EINVAL:
		return "EINVAL";
	case EPERM:
		return "EPERM";
	case ENOTSUP:
		return "ENOTSUP";
	case EBUSY:
		return "EBUSY";
	case ENODEV:
		return "ENODEV";
	case EAGAIN:
		return "EAGAIN";
	case ERANGE:
		return "ERANGE";
	case EIO:
		return "EIO";
	case EPIPE:
		return "EPIPE";
	default:
		break;
	}

	/* Any other value is named alike. */
	return "other";
}
