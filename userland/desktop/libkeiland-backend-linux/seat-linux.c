/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat on Linux (libkeiland-backend since ws131-p006): logind's or the
 * direct root seat, and the virtual terminal.
 *
 * The seat is chosen once at open: KEILAND_SEAT=logind or direct, or,
 * without it, logind for a Wayland session with an ID and direct
 * otherwise.  An unknown name refuses rather than falling back to root
 * access.  When standard input is a virtual terminal it is put in graphics
 * mode with its keyboard off while the compositor owns the display, and
 * each property changed is restored at close, partial opens too.
 */

#include "userland/desktop/libkeiland-backend-linux/seat-linux.h"

#include <errno.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* logind's seat (1) or the direct one (0), fixed at open. */
static unsigned seat_logind;

/* The virtual terminal's modes, valid once read, and which were changed. */
static int console_mode;
static int keyboard_mode;
static unsigned console_changed;
static unsigned keyboard_changed;

static int seat_console_take(void);

/*
 * Chooses and takes the seat and the primary node, then the virtual
 * terminal.
 */
int
kl_backend_seat_open(
	struct kl_backend *backend)
{
	const char *seat;
	const char *type;
	const char *session;
	int same;
	int error;

	/* An explicit choice, else logind for a Wayland session with an ID. */
	if (backend == NULL)
		return EINVAL;
	seat = getenv("KEILAND_SEAT");
	if (seat == NULL) {
		seat = "direct";
		type = getenv("XDG_SESSION_TYPE");
		session = getenv("XDG_SESSION_ID");
		if (type != NULL && session != NULL) {
			same = strcmp(type, "wayland");
			if (same == 0 && session[0] != '\0')
				seat = "logind";
		}
	}

	/* An unknown name cannot fall back to root device access. */
	same = strcmp(seat, "logind");
	if (same == 0) {
		seat_logind = 1;
	} else {
		same = strcmp(seat, "direct");
		if (same != 0)
			return EINVAL;
		seat_logind = 0;
	}

	/* The seat and its primary node, before Vulkan opens its own inquiry file. */
	if (seat_logind != 0)
		error = linux_logind_seat_open(backend);
	else
		error = linux_direct_seat_open();
	if (error != 0)
		return error;

	/* The virtual terminal, when standard input is one. */
	error = seat_console_take();
	if (error != 0)
		return error;

	/* Succeeded: the seat owns the display. */
	return 0;
}

/*
 * Restores the virtual terminal and returns the seat (partial opens too).
 */
void
kl_backend_seat_close(
	struct kl_backend *backend)
{
	/* Each changed property is restored, the keyboard first, after the display has been given back. */
	(void)backend;
	if (keyboard_changed != 0) {
		(void)ioctl(STDIN_FILENO, KDSKBMODE, keyboard_mode);
		keyboard_changed = 0;
	}
	if (console_changed != 0) {
		(void)ioctl(STDIN_FILENO, KDSETMODE, console_mode);
		console_changed = 0;
	}

	/* No primary node outlives the compositor. */
	if (seat_logind != 0)
		linux_logind_seat_close();
	else
		linux_direct_seat_close();
}

/*
 * The primary node's descriptor.
 */
int
kl_backend_seat_primary_fd(
	const struct kl_backend *backend)
{
	int descriptor;

	/* The chosen seat's. */
	(void)backend;
	if (seat_logind != 0)
		descriptor = linux_logind_drm_fd();
	else
		descriptor = linux_direct_drm_fd();
	return descriptor;
}

/*
 * The primary node's path.
 */
const char *
kl_backend_seat_primary_path(
	const struct kl_backend *backend)
{
	const char *path;

	/* The chosen seat's, fixed across resumes. */
	(void)backend;
	if (seat_logind != 0)
		path = linux_logind_drm_path();
	else
		path = linux_direct_drm_path();
	return path;
}

/*
 * Tells whether logind has paused the primary node (the direct seat never
 * pauses).
 */
int
kl_backend_seat_paused(
	const struct kl_backend *backend)
{
	int paused;

	/* Only logind pauses. */
	(void)backend;
	if (seat_logind == 0)
		return 0;
	paused = linux_logind_seat_paused();
	return paused;
}

/*
 * Opens an input device through the chosen seat.
 */
int
kl_backend_seat_device_open(
	struct kl_backend *backend,
	const char *path)
{
	int descriptor;

	/* The chosen seat's lease, or -1 with errno. */
	(void)backend;
	if (seat_logind != 0)
		descriptor = linux_logind_device_open(path);
	else
		descriptor = linux_direct_device_open(path);
	return descriptor;
}

/*
 * Returns an input device to the chosen seat.
 */
void
kl_backend_seat_device_close(
	struct kl_backend *backend,
	int descriptor)
{
	/* The chosen seat's lease. */
	(void)backend;
	if (seat_logind != 0)
		linux_logind_device_close(descriptor);
	else
		linux_direct_device_close(descriptor);
}

/*
 * Keeps a revoked device for logind's later signal (the direct seat keeps
 * nothing).
 */
int
kl_backend_seat_device_revoked(
	struct kl_backend *backend,
	int descriptor)
{
	int retained;

	/* Only logind resumes a revoked device. */
	(void)backend;
	if (seat_logind == 0)
		return 0;
	retained = linux_logind_device_revoked(descriptor);
	return retained;
}

/*
 * Lets logind's seat close or answer for a descriptor the compositor no
 * longer reads (the direct seat calls back about nothing).
 */
void
kl_backend_seat_device_let_go(
	struct kl_backend *backend,
	int descriptor)
{
	/* Only logind waits for an answer. */
	(void)backend;
	if (seat_logind == 0)
		return;
	linux_logind_device_let_go(descriptor);
}

/*
 * Lets logind hear that the compositor has let go of the paused display
 * (the direct seat never pauses).
 */
void
kl_backend_seat_paused_done(
	struct kl_backend *backend)
{
	/* Only logind pauses. */
	(void)backend;
	if (seat_logind == 0)
		return;
	linux_logind_paused_done();
}

/*
 * Counts logind's bus descriptor.
 */
size_t
kl_backend_seat_poll_count(
	const struct kl_backend *backend)
{
	/* Only logind's seat has a service to hear. */
	(void)backend;
	if (seat_logind == 0)
		return 0;
	return 1;
}

/*
 * Fills logind's bus descriptor, polled while paused too.
 */
void
kl_backend_seat_poll_fill(
	struct kl_backend *backend,
	struct pollfd *descriptors)
{
	/* The bus carries the resume as well as the pause. */
	(void)backend;
	if (seat_logind == 0)
		return;
	descriptors[0].fd = linux_logind_poll_fd();
	descriptors[0].events = POLLIN;
	descriptors[0].revents = 0;
}

/*
 * Dispatches logind's signals (queued ones too) and ends the compositor on
 * a failed or lost bus.
 */
void
kl_backend_seat_poll_done(
	struct kl_backend *backend,
	const struct pollfd *descriptors)
{
	int error;

	/* Signals already queued need dispatching even without new readiness. */
	if (seat_logind == 0)
		return;
	error = linux_logind_dispatch(backend);

	/* A failed dispatch or a disconnected authority ends the compositor through the ordinary cleanup. */
	if (error != 0 || (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		if (backend->host.session_stop != NULL)
			backend->host.session_stop(backend->host.data, KL_BACKEND_SESSION_LOST);
	}
}

/* Puts a virtual terminal on standard input in graphics mode with its keyboard off. */
static int
seat_console_take(
	void)
{
	int error;

	/* Nothing to do when standard input is not a virtual terminal. */
	error = ioctl(STDIN_FILENO, KDGETMODE, &console_mode);
	if (error != 0)
		return 0;

	/* The keyboard's mode, read before either property changes. */
	error = ioctl(STDIN_FILENO, KDGKBMODE, &keyboard_mode);
	if (error != 0)
		return errno;

	/* The console stops drawing while Vulkan owns the CRTC. */
	error = ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS);
	if (error != 0)
		return errno;
	console_changed = 1;

	/* The keys go to evdev only, not to the console too. */
	error = ioctl(STDIN_FILENO, KDSKBMODE, K_OFF);
	if (error != 0)
		return errno;
	keyboard_changed = 1;

	/* Succeeded: the console restores both at close. */
	return 0;
}

/*
 * Tells whether the backend wakes the compositor by itself: no, Linux's
 * areas (the bus, the sound card's controls, the wireless daemon, the
 * playback pumps) are read without waiting each pass.
 */
int
kl_backend_wakes(
	const struct kl_backend *backend)
{
	/* The compositor comes back soon instead. */
	(void)backend;
	return 0;
}

/*
 * Tells whether the backend tells every input device that comes or goes:
 * no, Linux's devices are found by the compositor's own scans.
 */
int
kl_backend_input_events(
	const struct kl_backend *backend)
{
	/* The compositor scans on its own. */
	(void)backend;
	return 0;
}
