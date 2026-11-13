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
 * devfs keeps an owner and a mode given to a name, also for a node made
 * again under that name (a keyboard plugged in again).  A node that appears
 * for the first time comes with devfs's own owner.  sessiond hears the
 * system's events of the input devices, of the USB devices (the smart
 * card slots) and of the cameras on /dev/system (ws132-p002) and gives
 * the devices again as soon as one comes (BUG-264: the compositor,
 * which hears the same event, then finds the node already its user's).  The loops that wait for the
 * greeter and the session still give them every second, for a kernel
 * without the events and for the nodes that post none (a display that
 * attaches late).
 *
 * Giving a device away does not take it from a process that opened it
 * before: there is no revoke in the kernel yet, so the graphical login is
 * for a machine with one user (login-manager-design.md §7).
 *
 * The compositors sessiond starts (the greeter's and the session's) also
 * have the input devices opened for them here, so that a device plugged in
 * is read at once and not after its node's owner has changed: each gets
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
#include <unistd.h>
#include <uapi/gpu.h>
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
#define SEAT_INPUT_MODE		0640
#define SEAT_BACKLIGHT_MODE	0644
#define SEAT_KEY_MODE		0600
#define SEAT_CAMERA_MODE	0600

/* The wheel group, which owns the input devices when no one has the seat. */
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
	char line[SEAT_REQUEST_MAX];
	size_t used;
};

/*
 * The descriptor the seat's events are read on: /dev/system, subscribed to
 * SEAT_EVENT_CLASSES and read without waiting.  -1 when the kernel has no
 * events or the descriptor failed; the loops' every-second giving is then
 * the only one.  Opened once at sessiond's start and kept until it ends;
 * sessiond's one thread uses it.
 */
static int seat_events = -1;

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

	/* The login screen's account gets no security key, smart card slot nor camera: they stay root's. */
	if (!keys) {
		seat_input("hidraw", 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		for (index = 0U; index < SEAT_SMARTCARD_COUNT; index++) {
			snprintf(path, sizeof(path), "/dev/smartcard%u", index);
			seat_set(path, 0, SEAT_WHEEL_GID, SEAT_KEY_MODE);
		}
		seat_cameras(0, SEAT_WHEEL_GID);

		/* The login screen's account has the display and the input devices only. */
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
 * Gives the display and the input devices back to root with devfs's modes.
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

	/* Every input device, which root and wheel read; every security key's raw node, root's alone. */
	seat_input("event", 0, SEAT_WHEEL_GID, SEAT_INPUT_MODE);
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
	for (index = 0U; index < SESSIOND_SEAT_SOCKETS && count < room; index++) {
		if (!seat_sockets[index].live)
			continue;
		entries[count].fd = seat_sockets[index].descriptor;
		entries[count].events = POLLIN;
		entries[count].revents = 0;
		count++;
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
		for (index = 0U; index < SESSIOND_SEAT_SOCKETS; index++) {
			if (!seat_sockets[index].live || seat_sockets[index].descriptor != entries[entry].fd)
				continue;
			seat_socket_read(&seat_sockets[index]);
			break;
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
	socket->live = 0U;
	socket->descriptor = -1;
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

	/* A compositor that does not hold the seat gets nothing of it. */
	if (socket->descriptor != seat_holder) {
		sessiond_log("SESSIOND SEAT open node=%.16s error=%d holder=0", name, EPERM);
		seat_answer(socket->descriptor, EPERM, -1);
		return;
	}

	/* The node, opened as root (its owner may not have been given yet). */
	descriptor = -1;
	error = seat_node_open(name, &descriptor);
	sessiond_log("SESSIOND SEAT open node=%.16s error=%d holder=1", name, error);

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
