/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The seat's devices: the display (/dev/gpu*), the panel's light
 * (/dev/backlight/backlight*), and the security keys' raw HID
 * nodes
 * (/dev/input/hidraw*) and the smart card slots (/dev/smartcard*,
 * ws161-p002, the user's approval U3) and the cameras (/dev/cam*) belong
 * to the seat's user, 0600, while sessiond runs a greeter or a session,
 * and go back to root when it stops (the display to root and the video
 * group, whose members start a desktop from the text console; the keys and
 * the cards to root alone:
 * whoever opens one can ask the key to sign; the cameras to root alone:
 * whoever opens one sees the room).  The login screen is not given the
 * keys, the cards and the cameras (ws172: its security key login is
 * passkey's, which opens them as root).
 *
 * The input devices (/dev/input/event*) stay root's and wheel's, 0640:
 * sessiond opens them for the compositor that holds the seat (below), so
 * their owner is never changed.
 *
 * devfs keeps an owner and a mode given to a name, also for a node made
 * again under that name (a key plugged in again).  A node that appears for
 * the first time comes with devfs's own owner.  The devices are given when
 * the greeter or the session starts, and again only when the system's
 * events on /dev/system tell of a device that came: the raw
 * HID nodes post the input devices' class, the smart card slots the USB
 * devices', the cameras their own.  The displays and the backlights post
 * none: one that attaches after the greeter or the session started is not
 * given until the next start (sessiond waits for the displays the kernel
 * is still attaching before the first greeter).  A kernel without the
 * events has the devices given at the starts only.
 *
 * Giving a device away does not take it from a process that opened it
 * before: there is no revoke in the kernel yet, so the graphical login is
 * for a machine with one user (login-manager-design.md §7).
 *
 * The compositors sessiond starts (the greeter's and the session's)
 * have the input devices opened for them here, which their user could not
 * open, and a device plugged in is read at once: each gets
 * one end of a socket pair of its own as descriptor SESSIOND_SEAT_FD
 * (--seat-fd), and asks on it with a line
 *
 *   OPEN eventN      the evdev node /dev/input/eventN
 *
 * which is answered with one line: OK with the node's descriptor
 * (SCM_RIGHTS), opened by sessiond for reading without waiting, or
 * FAIL errno.  Only the compositor that holds the seat now (the greeter
 * while it runs, then the session, then the greeter a Log Out starts) has
 * its requests granted; the others hear FAIL EPERM.  A descriptor given
 * before stays with the compositor until it closes it or ends (the same
 * as a node opened before its owner changed).
 */

#include "sessiond.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include <uapi/input.h>
#include <uapi/system.h>

/* The display devices sessiond looks for: /dev/gpu0 to /dev/gpu3. */
#define SEAT_GPU_COUNT		4U

/* The input devices' directory. */
#define SEAT_INPUT_DIRECTORY	"/dev/input"

/* The backlight devices sessiond looks for: /dev/backlight/backlight0 to backlight3. */
#define SEAT_BACKLIGHT_COUNT	4U

/* The smart card slots sessiond looks for: /dev/smartcard0 to smartcard7. */
#define SEAT_SMARTCARD_COUNT	8U

/* The cameras sessiond looks for: /dev/cam0 to /dev/cam3. */
#define SEAT_CAMERA_COUNT	4U

/* devfs's own modes, which the devices go back to (a GPU's is the kernel's, with its group). */
#define SEAT_GPU_MODE		GPU_NODE_MODE
#define SEAT_BACKLIGHT_MODE	0644
#define SEAT_KEY_MODE		0600
#define SEAT_CAMERA_MODE	0600

/* The wheel group, which owns the devices when no one has the seat. */
#define SEAT_WHEEL_GID		0

/* The kernel's system device, and the classes of its events that bring or take a seat's device. */
#define SEAT_SYSTEM_NODE	"/dev/system"
#define SEAT_EVENT_CLASSES	(KERN_SYSTEM_EVENT_INPUT | KERN_SYSTEM_EVENT_USB | KERN_SYSTEM_EVENT_CAMERA)

/* The records one read of the events takes. */
#define SEAT_EVENT_READ		8U

/* The longest request a compositor sends on its seat's socket, with its line end. */
#define SEAT_REQUEST_MAX	32U

/* The longest evdev node number a request may name (eventN, N of at most this many digits). */
#define SEAT_NODE_DIGITS	4U

/*
 * sessiond's end of one compositor's seat socket (SESSIOND_SEAT_FD on the
 * other side): the descriptor, and the bytes of a request not read whole
 * yet.  live is set while the slot holds a socket.
 */
struct seat_socket {
	unsigned live;
	int descriptor;
	int rdp_only;
	int rdp_event_index;
	int rdp_owner_fd;
	char line[SEAT_REQUEST_MAX];
	size_t used;
};

/*
 * The descriptor the seat's events are read on: /dev/system, subscribed to
 * SEAT_EVENT_CLASSES and read without waiting.  -1 when the kernel has no
 * events or the descriptor failed; the devices are then given at the
 * greeter's and the session's starts only.  Opened once at sessiond's start and kept until it ends;
 * sessiond's one thread uses it.
 */
static int seat_events = -1;
static int seat_rdp_listen = -1;

/*
 * The seat sockets of the compositors sessiond started and that have not
 * ended (a greeter, its session, and the greeter a Log Out starts while
 * the session still shows).  A slot is filled when sessiond makes the
 * socket for a compositor it starts and emptied when that compositor's
 * end closes; sessiond's one thread uses them.
 */
static struct seat_socket seat_sockets[SESSIOND_SEAT_SOCKETS];

/*
 * sessiond's end of the seat socket of the compositor that holds the seat
 * now, whose OPEN requests are granted; -1 while none does (the seat is
 * root's, or the holder ended).  It follows sessiond_seat_hold.
 */
static int seat_holder = -1;

static void seat_set(const char *path, uid_t uid, gid_t gid, mode_t mode);
static void seat_input(const char *prefix, uid_t uid, gid_t gid, mode_t mode);
static void seat_cameras(uid_t uid, gid_t gid);
static void seat_events_fail(const char *what, int error);
static void seat_socket_read(struct seat_socket *socket);
static void seat_socket_close(struct seat_socket *socket);
static void seat_request(struct seat_socket *socket, const char *line);
static int seat_node_open(const char *name, int *descriptor);
static void seat_answer(int socket, int error, int descriptor);
static void seat_rdp_listen_open(void);
static void seat_rdp_accept(void);
static int seat_event_index(const char *name);
static int seat_is_rdp_event(int event_index);
static int seat_is_rdp_virtual_device(int descriptor);

/*
 * Gives the displays and the backlights to a user, for that user only, and
 * the security keys, the smart card slots and the cameras too when keys is
 * set (a session's user; not the login screen's account, for whom they go
 * back to root).
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

	/* Every backlight, which the session's compositor sets. */
	for (index = 0U; index < SEAT_BACKLIGHT_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/backlight/backlight%u", index);
		seat_set(path, uid, gid, 0600);
	}

	/* The login screen's account gets no security key, smart card slot nor camera: they stay root's. */
	if (!keys) {
		seat_input("hidraw", 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
			snprintf(path, sizeof(path), "/dev/smartcard%u", index);
			seat_set(path, 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		}
		seat_cameras(0, SEAT_WHEEL_GID);

		/* The login screen's account has the displays and the backlights only. */
		return;
	}

	/* A session's user gets every security key's raw node, every smart card slot and every camera. */
	seat_input("hidraw", uid, gid, 0600);
	for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/smartcard%u", index);
		seat_set(path, uid, gid, 0600);
	}
	seat_cameras(uid, gid);
}

/*
 * Gives the seat's devices back to root with devfs's modes.
 */
void
sessiond_seat_restore(
	void)
{
	char path[32];
	unsigned index;

	/* Every display, root's and the video group's again (the GPU node admits root, its owner and, by the mode, its group). */
	for (index = 0U; index < SEAT_GPU_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/gpu%u", index);
		seat_set(path, 0, GPU_NODE_GROUP, SEAT_GPU_MODE);
	}

	/* Every security key's raw node, root's alone. */
	seat_input("hidraw", 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);

	/* Every backlight, which anyone reads and root sets. */
	for (index = 0U; index < SEAT_BACKLIGHT_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/backlight/backlight%u", index);
		seat_set(path, 0, SEAT_WHEEL_GID, SEAT_BACKLIGHT_MODE);
	}

	/* Every smart card slot and every camera, root's alone. */
	for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/smartcard%u", index);
		seat_set(path, 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
	}
	seat_cameras(0, SEAT_WHEEL_GID);

	/* No compositor has the input devices opened for it any more. */
	seat_holder = -1;
}

/*
 * Subscribes to the system's events of the input devices, the USB devices
 * and the cameras, so a device that comes is given at once.  Without them
 * the devices are given at the greeter's and the session's starts only.
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
	seat_rdp_listen_open();
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

/*
 * Makes the seat socket of a compositor sessiond is about to start: own is
 * sessiond's end (kept from the programs sessiond starts, and heard by the
 * greeter's and the session's loops from now on), child the end the
 * compositor gets as SESSIOND_SEAT_FD.  Returns 0, or an errno value (no
 * socket: the compositor opens the devices itself).
 */
int
sessiond_seat_socket(
	int *own,
	int *child)
{
	struct seat_socket *slot;
	unsigned index;
	int pair[2];
	int error;

	/* A free place for sessiond's end. */
	slot = NULL;
	for (index = 0U; index < SESSIOND_SEAT_SOCKETS; index++) {
		if (!seat_sockets[index].live) {
			slot = &seat_sockets[index];
			break;
		}
	}

	/* All taken: the compositor opens the devices itself. */
	if (slot == NULL) {
		sessiond_log("SESSIOND SEAT socket errno=%d", EMFILE);
		return EMFILE;
	}

	/* The pair. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		error = errno;
		sessiond_log("SESSIOND SEAT socket errno=%d", error);
		return error;
	}

	/* sessiond's end is its own: no program it starts gets it. */
	(void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	memset(slot, 0, sizeof(*slot));
	slot->live = 1U;
	slot->descriptor = pair[0];

	/* Succeeded: the child's end goes to the compositor. */
	*own = pair[0];
	*child = pair[1];
	return 0;
}

/*
 * Gives the seat's input devices to the compositor on a seat socket
 * (sessiond's end): its OPEN requests are granted from now on, and the
 * other compositors' refused.
 */
void
sessiond_seat_hold(
	int own)
{
	/* The holder. */
	seat_holder = own;
	sessiond_log("SESSIOND SEAT holder socket=%d", own);
}

/*
 * Fills a loop's poll entries with the seat sockets, at most room of them,
 * and reports how many it filled.
 */
nfds_t
sessiond_seat_poll_fill(
	struct pollfd *entries,
	nfds_t room)
{
	unsigned index;
	nfds_t count;

	/* Each live socket, readable when a request or its end comes. */
	count = 0;
	if (seat_rdp_listen >= 0 && count < room) {
		entries[count].fd = seat_rdp_listen;
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		count++;
	}
	for (index = 0U; index < SESSIOND_SEAT_SOCKETS && count < room; index++) {
		if (!seat_sockets[index].live)
			continue;
		entries[count].fd = seat_sockets[index].descriptor;
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		count++;
		if (seat_sockets[index].rdp_only && seat_sockets[index].rdp_owner_fd >= 0 && count < room) {
			entries[count].fd = seat_sockets[index].rdp_owner_fd;
			entries[count].events = POLLIN;
			entries[count].revents = 0;
			count++;
		}
	}

	/* Succeeded: the entries filled. */
	return count;
}

/*
 * Answers the requests on the seat sockets a poll found readable, and lets
 * go of the sockets whose compositor ended.
 */
void
sessiond_seat_poll_done(
	const struct pollfd *entries,
	nfds_t count)
{
	unsigned index;
	nfds_t entry;

	/* Each entry with something to read, matched to its socket. */
	for (entry = 0; entry < count; entry++) {
		if (entries[entry].revents == 0)
			continue;
		if (seat_rdp_listen >= 0 && entries[entry].fd == seat_rdp_listen) {
			seat_rdp_accept();
			continue;
		}
		for (index = 0U; index < SESSIOND_SEAT_SOCKETS; index++) {
			if (!seat_sockets[index].live)
				continue;
			if (seat_sockets[index].descriptor == entries[entry].fd) {
				seat_socket_read(&seat_sockets[index]);
				break;
			}
			if (seat_sockets[index].rdp_only && seat_sockets[index].rdp_owner_fd == entries[entry].fd) {
				char drain[16];
				ssize_t n;
				n = read(seat_sockets[index].rdp_owner_fd, drain, sizeof(drain));
				if (n <= 0 && errno != EAGAIN && errno != EINTR)
					seat_socket_close(&seat_sockets[index]);
				else if (n == 0)
					seat_socket_close(&seat_sockets[index]);
				break;
			}
		}
	}
}

/* Gives every camera node an owner, 0600 (devfs's own mode, so root's alone when given back). */
static void
seat_cameras(
	uid_t uid,
	gid_t gid)
{
	char path[32];
	unsigned index;

	/* Every camera that is there. */
	for (index = 0U; index < SEAT_CAMERA_COUNT; index++) {
		snprintf(path, sizeof(path), "/dev/cam%u", index);
		seat_set(path, uid, gid, SEAT_CAMERA_MODE);
	}
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

	/* Nothing to do for a node that already has them (no log line for a device given before). */
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

/* Gives up the seat's events after a failed read; the devices are given at the starts only from then on. */
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

/* Reads what a compositor sent on its seat socket and answers each whole request. */
static void
seat_socket_read(
	struct seat_socket *socket)
{
	ssize_t count;
	size_t length;
	char *end;

	/* What came; its end (or a failed read) is the compositor's end. */
	count = read(socket->descriptor, socket->line + socket->used, sizeof(socket->line) - 1U - socket->used);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return;
	if (count <= 0) {
		seat_socket_close(socket);
		return;
	}

	/* Kept after what came before, as one string. */
	socket->used += (size_t)count;
	socket->line[socket->used] = '\0';

	/* Each whole request, then what is left moves to the front. */
	for (;;) {
		end = strchr(socket->line, '\n');
		if (end == NULL)
			break;
		*end = '\0';
		length = (size_t)(end - socket->line) + 1U;
		seat_request(socket, socket->line);
		memmove(socket->line, socket->line + length, socket->used - length);
		socket->used -= length;
		socket->line[socket->used] = '\0';
	}

	/* A line longer than any request is refused and thrown away. */
	if (socket->used + 1U >= sizeof(socket->line)) {
		socket->used = 0U;
		socket->line[0] = '\0';
		seat_answer(socket->descriptor, EINVAL, -1);
	}
}

/* Lets go of the seat socket of a compositor that ended. */
static void
seat_socket_close(
	struct seat_socket *socket)
{
	/* The holder that ended holds nothing; the slot is free again. */
	if (seat_holder == socket->descriptor)
		seat_holder = -1;
	(void)close(socket->descriptor);
	if (socket->rdp_only && socket->rdp_owner_fd >= 0)
		(void)close(socket->rdp_owner_fd);
	socket->live = 0U;
	socket->descriptor = -1;
	socket->rdp_only = 0;
	socket->rdp_event_index = -1;
	socket->rdp_owner_fd = -1;
	socket->used = 0U;
}

/* Answers one request of a compositor: the node it names, opened for it when it holds the seat. */
static void
seat_request(
	struct seat_socket *socket,
	const char *line)
{
	const char *name;
	int descriptor;
	int match;
	int error;

	/* Only OPEN is asked. */
	match = strncmp(line, "OPEN ", 5U);
	if (match != 0) {
		seat_answer(socket->descriptor, EINVAL, -1);
		return;
	}

	/* The node's name follows the word. */
	name = line + 5;

	/* RDP-isolated seat: only its assigned /dev/input/eventN is permitted. */
	if (socket->rdp_only) {
		int ev_idx = seat_event_index(name);
		if (ev_idx < 0 || ev_idx != socket->rdp_event_index) {
			seat_answer(socket->descriptor, EPERM, -1);
			return;
		}
	} else {
		/* A compositor that does not hold the seat gets nothing of it. */
		if (socket->descriptor != seat_holder) {
			sessiond_log("SESSIOND SEAT open node=%.16s error=%d holder=0", name, EPERM);
			seat_answer(socket->descriptor, EPERM, -1);
			return;
		}
		if (seat_is_rdp_event(seat_event_index(name))) {
			sessiond_log("SESSIOND SEAT open node=%.16s error=%d rdp_reserved=1", name, EPERM);
			seat_answer(socket->descriptor, EPERM, -1);
			return;
		}
	}

	/* The node, opened as root (it stays root's). */
	descriptor = -1;
	error = seat_node_open(name, &descriptor);
	if (error == 0 && !socket->rdp_only && seat_is_rdp_virtual_device(descriptor)) {
		(void)close(descriptor);
		descriptor = -1;
		error = EPERM;
		sessiond_log("SESSIOND SEAT open node=%.16s error=%d rdp_virtual=1", name, error);
	} else {
		sessiond_log("SESSIOND SEAT open node=%.16s error=%d holder=1", name, error);
	}

	/* The answer, then sessiond's own copy goes. */
	seat_answer(socket->descriptor, error, descriptor);
	if (descriptor >= 0)
		(void)close(descriptor);
}

/*
 * Opens /dev/input/NAME for reading without waiting, when NAME is an evdev
 * node's (event and a few digits) and the path is a character device.
 * Returns 0 with the descriptor, or an errno value.
 */
static int
seat_node_open(
	const char *name,
	int *descriptor)
{
	char path[64];
	struct stat status;
	size_t digits;
	int opened;
	int error;
	int match;

	/* "event" and one to SEAT_NODE_DIGITS digits, nothing else (no path of the compositor's choosing). */
	match = strncmp(name, "event", 5U);
	if (match != 0)
		return EINVAL;
	for (digits = 0U; name[5U + digits] != '\0'; digits++) {
		if (name[5U + digits] < '0' || name[5U + digits] > '9')
			return EINVAL;
		if (digits >= SEAT_NODE_DIGITS)
			return EINVAL;
	}

	/* At least one digit. */
	if (digits == 0U)
		return EINVAL;

	/* The node, never through a link. */
	snprintf(path, sizeof(path), "%s/%s", SEAT_INPUT_DIRECTORY, name);
	opened = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (opened < 0)
		return errno;

	/* Only a device node is given. */
	error = fstat(opened, &status);
	if (error != 0) {
		error = errno;
		(void)close(opened);
		return error;
	}

	/* Anything but a character device is not an input device. */
	if ((status.st_mode & S_IFMT) != S_IFCHR) {
		(void)close(opened);
		return ENODEV;
	}

	/* Succeeded: the caller gives the descriptor and closes its own copy. */
	*descriptor = opened;
	return 0;
}

/* Sends one answer on a seat socket: OK with the descriptor, or FAIL with the errno value. */
static void
seat_answer(
	int socket,
	int error,
	int descriptor)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *message_control;
	struct msghdr header;
	struct iovec vector;
	char text[32];
	ssize_t sent;

	/* The line. */
	if (error == 0)
		snprintf(text, sizeof(text), "OK\n");
	else
		snprintf(text, sizeof(text), "FAIL %d\n", error);
	memset(&header, 0, sizeof(header));
	vector.iov_base = text;
	vector.iov_len = strlen(text);
	header.msg_iov = &vector;
	header.msg_iovlen = 1;

	/* The descriptor with it. */
	if (error == 0 && descriptor >= 0) {
		memset(&control, 0, sizeof(control));
		header.msg_control = control.space;
		header.msg_controllen = sizeof(control.space);
		message_control = CMSG_FIRSTHDR(&header);
		message_control->cmsg_level = SOL_SOCKET;
		message_control->cmsg_type = SCM_RIGHTS;
		message_control->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(message_control), &descriptor, sizeof(int));
	}

	/* One message (a compositor that went does not read it). */
	sent = sendmsg(socket, &header, MSG_NOSIGNAL);
	if (sent != (ssize_t)vector.iov_len)
		sessiond_log("SESSIOND SEAT answer lost errno=%d", errno);
}

/* Parses N from "eventN"; returns -1 if invalid. */
static int
seat_event_index(
	const char *name)
{
	char *end;
	long val;

	if (name == NULL || strncmp(name, "event", 5U) != 0 || name[5] < '0' || name[5] > '9')
		return -1;
	val = strtol(name + 5, &end, 10);
	if (*end != '\0' || val < 0L || val > 65535L)
		return -1;
	return (int)val;
}

/* Returns 1 if event_index is currently reserved for an active RDP session. */
static int
seat_is_rdp_event(
	int event_index)
{
	unsigned index;

	if (event_index < 0)
		return 0;
	for (index = 0U; index < SESSIOND_SEAT_SOCKETS; index++) {
		if (seat_sockets[index].live && seat_sockets[index].rdp_only &&
		    seat_sockets[index].rdp_event_index == event_index)
			return 1;
	}
	return 0;
}

/* Returns 1 if an open evdev node is an rdpd virtual HID device. */
static int
seat_is_rdp_virtual_device(
	int descriptor)
{
	struct input_id id;
	char dev_name[64];

	if (descriptor < 0)
		return 0;
	memset(&id, 0, sizeof(id));
	if (ioctl(descriptor, EVIOCGID, &id) == 0 &&
	    id.bustype == BUS_VIRTUAL &&
	    id.vendor == 0x1D6BU &&
	    id.product == 0x0001U)
		return 1;
	memset(dev_name, 0, sizeof(dev_name));
	if (ioctl(descriptor, EVIOCGNAME(sizeof(dev_name) - 1U), dev_name) >= 0) {
		dev_name[sizeof(dev_name) - 1U] = '\0';
		if (strcmp(dev_name, "zedBSD RDP Virtual Input") == 0)
			return 1;
	}
	return 0;
}

/* Maximum concurrent pending one-shot RDP extend credentials. */
#define SEAT_EXTEND_MAX		8U
#define SEAT_EXTEND_TTL_MS	30000LL

struct seat_extend_entry {
	unsigned active;
	uid_t uid;
	gid_t gid;
	int control;
	long long expires_ms;
	char extend_user[64];
	uint8_t nt_hash[16];
};

static struct seat_extend_entry seat_extend_table[SEAT_EXTEND_MAX];
static unsigned seat_extend_seq = 1U;

/* Opens the root-only Unix socket on which rdpd claims isolated RDP seats. */
static void
seat_rdp_listen_open(
	void)
{
	struct sockaddr_un addr;
	int fd;

	if (seat_rdp_listen >= 0)
		return;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	(void)unlink(SESSIOND_RDP_SOCKET);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SESSIOND_RDP_SOCKET);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
	    listen(fd, 8) != 0) {
		(void)close(fd);
		return;
	}
	(void)chmod(SESSIOND_RDP_SOCKET, 0600);
	seat_rdp_listen = fd;
}

/* Accepts one RDP seat claim ("RDP_SEAT <event_index>\n") from rdpd. */
static void
seat_rdp_accept(
	void)
{
	int client_fd;
	char buf[64];
	ssize_t n;
	int ev_idx;
	struct seat_socket *slot;
	unsigned index;
	int pair[2];
	struct pollfd pfd;

	client_fd = accept(seat_rdp_listen, NULL, NULL);
	if (client_fd < 0)
		return;
	(void)fcntl(client_fd, F_SETFD, FD_CLOEXEC);
	pfd.fd = client_fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, 200) <= 0) {
		(void)close(client_fd);
		return;
	}
	n = read(client_fd, buf, sizeof(buf) - 1U);
	if (n <= 0) {
		(void)close(client_fd);
		return;
	}
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	if (strncmp(buf, "RDP_EXTEND_LOOKUP ", 18U) == 0) {
		uint8_t nt_hash[16];
		char reply[64];
		static const char hex[] = "0123456789abcdef";
		size_t i;
		if (sessiond_extend_lookup(buf + 18U, nt_hash) == 0) {
			memcpy(reply, "OK ", 3U);
			for (i = 0U; i < 16U; i++) {
				reply[3U + i * 2U] = hex[(nt_hash[i] >> 4) & 0x0FU];
				reply[3U + i * 2U + 1U] = hex[nt_hash[i] & 0x0FU];
			}
			reply[35] = '\n';
			(void)write(client_fd, reply, 36U);
			memset(nt_hash, 0, sizeof(nt_hash));
			memset(reply, 0, sizeof(reply));
		} else {
			seat_answer(client_fd, ENOENT, -1);
		}
		(void)close(client_fd);
		return;
	}
	if (strncmp(buf, "RDP_EXTEND ", 11U) == 0) {
		char ext_user[64];
		unsigned long w = 0UL;
		unsigned long h = 0UL;
		int rdp_fd = -1;
		int rc;
		if (sscanf(buf + 11U, "%63s %lu %lu", ext_user, &w, &h) != 3) {
			seat_answer(client_fd, EINVAL, -1);
			(void)close(client_fd);
			return;
		}
		rc = sessiond_extend_consume(ext_user, (uint32_t)w, (uint32_t)h, &rdp_fd);
		if (rc != 0 || rdp_fd < 0) {
			seat_answer(client_fd, rc != 0 ? rc : ENOENT, -1);
			(void)close(client_fd);
			return;
		}
		seat_answer(client_fd, 0, rdp_fd);
		(void)close(rdp_fd);
		(void)close(client_fd);
		return;
	}
	if (strncmp(buf, "RDP_SEAT ", 9U) != 0) {
		seat_answer(client_fd, EINVAL, -1);
		(void)close(client_fd);
		return;
	}
	ev_idx = (int)strtol(buf + 9U, NULL, 10);
	if (ev_idx < 0) {
		seat_answer(client_fd, EINVAL, -1);
		(void)close(client_fd);
		return;
	}
	slot = NULL;
	for (index = 0U; index < SESSIOND_SEAT_SOCKETS; index++) {
		if (!seat_sockets[index].live) {
			slot = &seat_sockets[index];
			break;
		}
	}
	if (slot == NULL || socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		seat_answer(client_fd, EMFILE, -1);
		(void)close(client_fd);
		return;
	}
	(void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(client_fd, F_SETFL, O_NONBLOCK);
	memset(slot, 0, sizeof(*slot));
	slot->live = 1U;
	slot->descriptor = pair[0];
	slot->rdp_only = 1;
	slot->rdp_event_index = ev_idx;
	slot->rdp_owner_fd = client_fd;
	seat_answer(client_fd, 0, pair[1]);
	(void)close(pair[1]);
	sessiond_log("SESSIOND SEAT rdp claimed event=%d socket=%d", ev_idx, pair[0]);
}

static uint32_t
md4_rotl(
	uint32_t x,
	unsigned n)
{
	return (x << n) | (x >> (32U - n));
}

/* Computes RFC 1320 MD4 over UTF-16LE password for NTLM NT hash. */
static void
seat_nt_hash(
	const char *password,
	uint8_t out[16])
{
	uint8_t msg[256];
	size_t plen;
	size_t byte_len;
	size_t padded_len;
	size_t i;
	uint64_t bit_len;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;

	plen = strlen(password);
	if (plen > 110U)
		plen = 110U;
	byte_len = plen * 2U;
	memset(msg, 0, sizeof(msg));
	for (i = 0U; i < plen; i++) {
		msg[i * 2U] = (uint8_t)password[i];
		msg[i * 2U + 1U] = 0U;
	}
	msg[byte_len] = 0x80U;
	padded_len = ((byte_len + 8U) / 64U + 1U) * 64U;
	bit_len = (uint64_t)byte_len * 8ULL;
	for (i = 0U; i < 8U; i++)
		msg[padded_len - 8U + i] = (uint8_t)((bit_len >> (i * 8U)) & 0xFFU);

	a = 0x67452301U;
	b = 0xEFCDAB89U;
	c = 0x98BADCFEU;
	d = 0x10325476U;

	for (i = 0U; i < padded_len; i += 64U) {
		uint32_t x[16];
		uint32_t aa = a;
		uint32_t bb = b;
		uint32_t cc = c;
		uint32_t dd = d;
		size_t j;
		static const unsigned r1[4] = { 3U, 7U, 11U, 19U };
		static const unsigned r2[4] = { 3U, 5U, 9U, 13U };
		static const unsigned r3[4] = { 3U, 9U, 11U, 15U };
		static const unsigned k3[16] = {
			0U, 8U, 4U, 12U, 2U, 10U, 6U, 14U,
			1U, 9U, 5U, 13U, 3U, 11U, 7U, 15U
		};

		for (j = 0U; j < 16U; j++) {
			const uint8_t *p = msg + i + j * 4U;
			x[j] = (uint32_t)p[0] |
			       ((uint32_t)p[1] << 8) |
			       ((uint32_t)p[2] << 16) |
			       ((uint32_t)p[3] << 24);
		}
		for (j = 0U; j < 16U; j++) {
			uint32_t f = (b & c) | ((~b) & d);
			uint32_t t = md4_rotl(a + f + x[j], r1[j & 3U]);
			a = d;
			d = c;
			c = b;
			b = t;
		}
		for (j = 0U; j < 16U; j++) {
			uint32_t f = (b & c) | (b & d) | (c & d);
			uint32_t idx = (j & 3U) * 4U + (j >> 2);
			uint32_t t = md4_rotl(a + f + x[idx] + 0x5A827999U, r2[j & 3U]);
			a = d;
			d = c;
			c = b;
			b = t;
		}
		for (j = 0U; j < 16U; j++) {
			uint32_t f = b ^ c ^ d;
			uint32_t t = md4_rotl(a + f + x[k3[j]] + 0x6ED9EBA1U, r3[j & 3U]);
			a = d;
			d = c;
			c = b;
			b = t;
		}
		a += aa;
		b += bb;
		c += cc;
		d += dd;
	}
	{
		uint32_t s[4] = { a, b, c, d };
		for (i = 0U; i < 4U; i++) {
			out[i * 4U] = (uint8_t)(s[i] & 0xFFU);
			out[i * 4U + 1U] = (uint8_t)((s[i] >> 8) & 0xFFU);
			out[i * 4U + 2U] = (uint8_t)((s[i] >> 16) & 0xFFU);
			out[i * 4U + 3U] = (uint8_t)((s[i] >> 24) & 0xFFU);
		}
	}
	memset(msg, 0, sizeof(msg));
}

static void
seat_extend_prune(
	long long now_ms)
{
	size_t i;

	for (i = 0U; i < SEAT_EXTEND_MAX; i++) {
		if (seat_extend_table[i].active &&
		    now_ms >= seat_extend_table[i].expires_ms) {
			memset(&seat_extend_table[i], 0,
			       sizeof(seat_extend_table[i]));
		}
	}
}

int
sessiond_extend_prepare(
	const struct sessiond_account *account,
	int control,
	char *out_user,
	size_t user_cap,
	char *out_pass,
	size_t pass_cap,
	unsigned *out_port)
{
	static const char alphabet[] =
		"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
	struct seat_extend_entry *slot;
	long long now_ms;
	uint8_t rnd[24];
	int rfd;
	ssize_t got;
	size_t i;

	if (account == NULL || control < 0 || out_user == NULL ||
	    out_pass == NULL || out_port == NULL ||
	    user_cap < 32U || pass_cap < 25U)
		return -1;

	now_ms = sessiond_milliseconds();
	seat_extend_prune(now_ms);

	slot = NULL;
	for (i = 0U; i < SEAT_EXTEND_MAX; i++) {
		if (!seat_extend_table[i].active) {
			slot = &seat_extend_table[i];
			break;
		}
	}
	if (slot == NULL)
		return -1;

	rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (rfd < 0)
		return -1;
	got = read(rfd, rnd, sizeof(rnd));
	(void)close(rfd);
	if (got != (ssize_t)sizeof(rnd))
		return -1;

	for (i = 0U; i < sizeof(rnd); i++)
		out_pass[i] = alphabet[rnd[i] % (sizeof(alphabet) - 1U)];
	out_pass[sizeof(rnd)] = '\0';

	memset(slot, 0, sizeof(*slot));
	slot->active = 1U;
	slot->uid = account->passwd.pw_uid;
	slot->gid = account->passwd.pw_gid;
	slot->control = control;
	slot->expires_ms = now_ms + SEAT_EXTEND_TTL_MS;
	snprintf(slot->extend_user, sizeof(slot->extend_user), "%s+extend%u",
		 account->passwd.pw_name, seat_extend_seq++);
	seat_nt_hash(out_pass, slot->nt_hash);
	snprintf(out_user, user_cap, "%s", slot->extend_user);
	*out_port = 3389U;
	memset(rnd, 0, sizeof(rnd));
	return 0;
}

int
sessiond_extend_lookup(
	const char *extend_user,
	uint8_t out_nt_hash[16])
{
	long long now_ms;
	size_t i;

	if (extend_user == NULL || extend_user[0] == '\0')
		return -1;
	now_ms = sessiond_milliseconds();
	seat_extend_prune(now_ms);
	for (i = 0U; i < SEAT_EXTEND_MAX; i++) {
		if (seat_extend_table[i].active &&
		    strcmp(seat_extend_table[i].extend_user, extend_user) == 0) {
			memcpy(out_nt_hash, seat_extend_table[i].nt_hash, 16U);
			return 0;
		}
	}
	return -1;
}

int
sessiond_extend_consume(
	const char *extend_user,
	uint32_t width,
	uint32_t height,
	int *out_rdp_fd)
{
	long long now_ms;
	struct seat_extend_entry entry;
	int pair[2];
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec vec;
	char line[64];
	ssize_t sent;
	size_t i;

	if (extend_user == NULL || out_rdp_fd == NULL)
		return EINVAL;
	*out_rdp_fd = -1;
	now_ms = sessiond_milliseconds();
	seat_extend_prune(now_ms);

	memset(&entry, 0, sizeof(entry));
	for (i = 0U; i < SEAT_EXTEND_MAX; i++) {
		if (seat_extend_table[i].active &&
		    strcmp(seat_extend_table[i].extend_user, extend_user) == 0) {
			entry = seat_extend_table[i];
			memset(&seat_extend_table[i], 0,
			       sizeof(seat_extend_table[i]));
			break;
		}
	}
	if (!entry.active || entry.control < 0)
		return ENOENT;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
		return errno;
	(void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);

	snprintf(line, sizeof(line), "EXTEND_ATTACH %u %u\n",
		 (unsigned)width, (unsigned)height);
	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	vec.iov_base = line;
	vec.iov_len = strlen(line);
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	msg.msg_controllen = sizeof(control.space);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &pair[0], sizeof(int));

	sent = sendmsg(entry.control, &msg, MSG_NOSIGNAL);
	(void)close(pair[0]);
	if (sent != (ssize_t)vec.iov_len) {
		(void)close(pair[1]);
		return EIO;
	}
	*out_rdp_fd = pair[1];
	sessiond_log("SESSIOND EXTEND attached user=%s %ux%u",
		     extend_user, (unsigned)width, (unsigned)height);
	return 0;
}

void
sessiond_extend_forget(
	int control)
{
	size_t i;

	if (control < 0)
		return;
	for (i = 0U; i < SEAT_EXTEND_MAX; i++) {
		if (seat_extend_table[i].active &&
		    seat_extend_table[i].control == control) {
			memset(&seat_extend_table[i], 0,
			       sizeof(seat_extend_table[i]));
		}
	}
}
