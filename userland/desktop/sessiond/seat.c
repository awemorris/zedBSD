/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat's devices: the display (/dev/gpu*), the input devices
 * (/dev/input/event*), the panel's light (/dev/backlight/backlight*,
 * ws113-p013), and the security keys' raw HID nodes
 * (/dev/input/hidraw*) and the smart card slots (/dev/smartcard*,
 * ws161-p002, the user's approval U3) belong to the seat's user, 0600,
 * while sessiond runs a greeter or a session, and go back to root when it
 * stops (the keys and the cards to root alone: whoever opens one can ask
 * the key to sign).  The login screen is not given the keys and the cards
 * (ws172: its security key login is passkey's, which opens them as root).
 *
 * devfs keeps an owner and a mode given to a name, also for a node made
 * again under that name (a keyboard plugged in again).  A node that appears
 * for the first time comes with devfs's own owner.  sessiond hears the
 * system's events of the input devices and of the USB devices (the smart
 * card slots) on /dev/system (ws132-p002) and gives the devices again as
 * soon as one comes (BUG-264: the compositor, which hears the same event,
 * then finds the node already its user's).  The loops that wait for the
 * greeter and the session still give them every second, for a kernel
 * without the events and for the nodes that post none (a display that
 * attaches late).
 *
 * Giving a device away does not take it from a process that opened it
 * before: there is no revoke in the kernel yet, so the graphical login is
 * for a machine with one user (login-manager-design.md §7).
 */

#include "sessiond.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uapi/system.h>

/* The display devices sessiond looks for: /dev/gpu0 to /dev/gpu3. */
#define SEAT_GPU_COUNT		4U

/* The input devices' directory. */
#define SEAT_INPUT_DIRECTORY	"/dev/input"

/* The backlight devices sessiond looks for: /dev/backlight/backlight0 to backlight3. */
#define SEAT_BACKLIGHT_COUNT	4U

/* The smart card slots sessiond looks for: /dev/smartcard0 to smartcard7. */
#define SEAT_SMARTCARD_COUNT	8U

/* devfs's own modes, which the devices go back to. */
#define SEAT_GPU_MODE		0666
#define SEAT_INPUT_MODE		0640
#define SEAT_BACKLIGHT_MODE	0644
#define SEAT_KEY_MODE		0600

/* The wheel group, which owns the input devices when no one has the seat. */
#define SEAT_WHEEL_GID		0

/* The kernel's system device, and the classes of its events that bring or take a seat's device. */
#define SEAT_SYSTEM_NODE	"/dev/system"
#define SEAT_EVENT_CLASSES	(KERN_SYSTEM_EVENT_INPUT | KERN_SYSTEM_EVENT_USB)

/* The records one read of the events takes. */
#define SEAT_EVENT_READ		8U

/*
 * The descriptor the seat's events are read on: /dev/system, subscribed to
 * SEAT_EVENT_CLASSES and read without waiting.  -1 when the kernel has no
 * events or the descriptor failed; the loops' every-second giving is then
 * the only one.  Opened once at sessiond's start and kept until it ends;
 * sessiond's one thread uses it.
 */
static int seat_events = -1;

static void seat_set(const char *path, uid_t uid, gid_t gid, mode_t mode);
static void seat_input(const char *prefix, uid_t uid, gid_t gid, mode_t mode);
static void seat_events_fail(const char *what, int error);

/*
 * Gives the display and the input devices to a user, for that user only,
 * and the security keys and the smart card slots too when keys is set (a
 * session's user; not the login screen's account).
 */
void
sessiond_seat_give(
	uid_t uid,
	gid_t gid,
	int keys)
{
	char path[32];
	unsigned index;

	/* Every display. */
	for (index = 0U; index < SEAT_GPU_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/gpu%u", index);
		seat_set(path, uid, gid, 0600);
	}

	/* Every input device. */
	seat_input("event", uid, gid, 0600);

	/* Every backlight, which the session's compositor sets. */
	for (index = 0U; index < SEAT_BACKLIGHT_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/backlight/backlight%u", index);
		seat_set(path, uid, gid, 0600);
	}

	/* The login screen's account gets no security key nor smart card slot: they stay root's. */
	if (!keys) {
		seat_input("hidraw", 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
			snprintf(path, sizeof(path), "/dev/smartcard%u", index);
			seat_set(path, 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		}

		/* The login screen's account has the display and the input devices only. */
		return;
	}

	/* A session's user gets every security key's raw node and every smart card slot. */
	seat_input("hidraw", uid, gid, 0600);
	for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/smartcard%u", index);
		seat_set(path, uid, gid, 0600);
	}
}

/*
 * Gives the display and the input devices back to root with devfs's modes.
 */
void
sessiond_seat_restore(
	void)
{
	char path[32];
	unsigned index;

	/* Every display, which anyone may open again. */
	for (index = 0U; index < SEAT_GPU_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/gpu%u", index);
		seat_set(path, 0, SEAT_WHEEL_GID, SEAT_GPU_MODE);
	}

	/* Every input device, which root and wheel read; every security key's raw node, root's alone. */
	seat_input("event", 0, SEAT_WHEEL_GID, SEAT_INPUT_MODE);
	seat_input("hidraw", 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);

	/* Every backlight, which anyone reads and root sets. */
	for (index = 0U; index < SEAT_BACKLIGHT_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/backlight/backlight%u", index);
		seat_set(path, 0, SEAT_WHEEL_GID, SEAT_BACKLIGHT_MODE);
	}

	/* Every smart card slot, root's alone. */
	for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/smartcard%u", index);
		seat_set(path, 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
	}
}

/*
 * Subscribes to the system's events of the input and the USB devices, so a
 * device that comes is given at once.  A kernel without them leaves the
 * every-second giving alone.
 */
void
sessiond_seat_events_open(
	void)
{
	struct system_event_subscription subscription;
	int descriptor;
	int result;

	/* The device, read without waiting and kept from the programs sessiond starts. */
	descriptor = open(SEAT_SYSTEM_NODE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (descriptor < 0) {
		sessiond_log("SESSIOND SEAT events unavailable errno=%d", errno);
		return;
	}

	/* The classes; an older kernel refuses them. */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = SEAT_EVENT_CLASSES;
	result = ioctl(descriptor, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (result != 0) {
		sessiond_log("SESSIOND SEAT events unavailable errno=%d", errno);
		(void)close(descriptor);
		return;
	}

	/* Succeeded: a device that comes is heard from now on. */
	seat_events = descriptor;
	sessiond_log("SESSIOND SEAT events subscribed classes=0x%x", (unsigned)SEAT_EVENT_CLASSES);
}

/* Reports the descriptor the seat's events come on, -1 for none (the loops poll it). */
int
sessiond_seat_events_fd(
	void)
{
	/* The descriptor, or -1. */
	return seat_events;
}

/*
 * Reads every record of the seat's events waiting.  Reports 1 when a
 * device came, or records were lost (the caller gives the devices again),
 * and 0 otherwise: a device that went leaves nothing to give.
 */
int
sessiond_seat_events_collect(
	void)
{
	struct system_event events[SEAT_EVENT_READ];
	ssize_t length;
	size_t count;
	size_t index;
	int changed;

	/* Nothing subscribed. */
	if (seat_events < 0)
		return 0;

	/* Every record waiting. */
	changed = 0;
	for (;;) {
		/* A few records; none left ends the pass. */
		length = read(seat_events, events, sizeof(events));
		if (length < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		if (length < 0) {
			seat_events_fail("read", errno);
			break;
		}

		/* The device does not end, but a descriptor that does is given up. */
		if (length == 0) {
			seat_events_fail("end", 0);
			break;
		}

		/* Whole records only. */
		if ((size_t)length % sizeof(events[0]) != 0U) {
			seat_events_fail("record", EPROTO);
			break;
		}

		/* A device that came, or lost records, asks for the devices to be given again. */
		count = (size_t)length / sizeof(events[0]);
		for (index = 0; index < count; index++) {
			if (events[index].class_bit == KERN_SYSTEM_EVENT_OVERFLOW)
				changed = 1;
			else if (events[index].action == KERN_SYSTEM_EVENT_ADD)
				changed = 1;
		}
	}

	/* Reports whether the devices are to be given again. */
	if (changed)
		return 1;

	/* Succeeded: nothing came. */
	return 0;
}

/* Gives one device node an owner and a mode, when the node is there. */
static void
seat_set(
	const char *path,
	uid_t uid,
	gid_t gid,
	mode_t mode)
{
	struct stat status;
	int error;

	/* A device that is not there is left alone. */
	error = stat(path, &status);
	if (error != 0)
		return;

	/* Nothing to do for a node that already has them (no log line every second). */
	if (status.st_uid == uid && status.st_gid == gid && (status.st_mode & 07777) == mode)
		return;

	/* The owner first, so the new mode never lets the old owner in. */
	error = chown(path, uid, gid);
	if (error != 0) {
		sessiond_log("SESSIOND SEAT chown path=%s failed", path);
		return;
	}

	/* Then the mode. */
	error = chmod(path, mode);
	if (error != 0) {
		sessiond_log("SESSIOND SEAT chmod path=%s failed", path);
		return;
	}

	/* Succeeded: the device is the user's. */
	sessiond_log("SESSIOND SEAT path=%s uid=%u mode=%04o", path, (unsigned)uid, (unsigned)mode);
}

/* Gives every node of /dev/input whose name starts with prefix an owner and a mode. */
static void
seat_input(
	const char *prefix,
	uid_t uid,
	gid_t gid,
	mode_t mode)
{
	char path[300];
	struct dirent *entry;
	DIR *directory;
	size_t prefix_length;
	int match;

	/* The input devices there are now. */
	directory = opendir(SEAT_INPUT_DIRECTORY);
	if (directory == NULL)
		return;

	/* Each node of the kind. */
	prefix_length = strlen(prefix);
	for (;;) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		match = strncmp(entry->d_name, prefix, prefix_length);
		if (match != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", SEAT_INPUT_DIRECTORY, entry->d_name);
		seat_set(path, uid, gid, mode);
	}

	/* The listing is done with. */
	(void)closedir(directory);
}

/* Gives up the seat's events after a failed read; the every-second giving goes on. */
static void
seat_events_fail(
	const char *what,
	int error)
{
	/* The descriptor goes, and nothing more is heard. */
	sessiond_log("SESSIOND SEAT events failed what=%s errno=%d", what, error);
	(void)close(seat_events);
	seat_events = -1;
}
