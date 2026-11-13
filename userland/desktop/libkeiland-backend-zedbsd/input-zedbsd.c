/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The input devices on zedBSD (libkeiland-backend since ws131-p007): the
 * evdev nodes, opened by the session manager when it started the
 * compositor with a seat socket (sessiond's --seat-fd), and directly
 * otherwise.
 *
 * On the seat socket the compositor writes one line, OPEN eventN, and
 * sessiond answers one line: OK with the node's descriptor (SCM_RIGHTS),
 * or FAIL errno (FAIL EPERM when the compositor does not hold the seat).
 * Only the compositor's threads ask (the devices' thread and the finding's),
 * one request and its answer at a time under seat_lock, and the answer is
 * waited for without a time limit: sessiond answers once its loop runs
 * again, and the socket's end or the cancel ends the wait.  Opened by
 * sessiond, a node is read at once, before its owner has been changed to
 * the seat's user.  The event loop's own scan (without a thread) opens the
 * nodes directly: it never waits for sessiond, which does not answer while
 * it waits for the compositor's READY.
 */
#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "userland/desktop/libkeiland-backend/keiland-backend-evdev.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The directory whose eventN nodes are the evdev devices. */
#define INPUT_DIRECTORY "/dev/input"

/* The longest request and answer on the seat socket, with the line end. */
#define INPUT_SEAT_LINE 32U

static int event_node_name(const char *name);
static void refusals_forget(struct kl_backend *backend, uint64_t generation);
static int node_refused(const struct kl_backend *backend, unsigned events_heard, unsigned long node);
static void probe_device(struct kl_backend *backend, const char *path, unsigned long node);
static int open_node(struct kl_backend *backend, unsigned events_heard, unsigned ask, const char *path, unsigned long node, int *descriptor, struct kl_backend_input_caps *capabilities);
static int seat_socket(const struct kl_backend *backend);
static int seat_open(struct kl_backend *backend, unsigned long node, int *descriptor);
static int seat_ask(struct kl_backend *backend, int socket, const char *request, int *descriptor);
static int seat_answer(struct kl_backend *backend, int socket, char *answer, size_t size, int *descriptor);
static int find_known(const struct kl_backend_input_find *find, const char *path);
static int read_capabilities(int descriptor, struct kl_backend_input_caps *capabilities);

/*
 * Opens every evdev pointer and keyboard not already open.
 *
 * A missing directory leaves the seat without devices; it is not an error.
 * The event loop calls this periodically so late devices are picked up.
 * A node whose open failed with the device's own error is passed by until
 * the system's events tell of an input device that came or went
 * (events-zedbsd.c forgets the refusals then).
 */
void
kl_backend_input_scan(
	struct kl_backend *backend)
{
	DIR *directory;
	struct dirent *entry;
	char path[KL_BACKEND_INPUT_PATH_MAX];
	unsigned long node;
	int length;
	int valid;
	int refused;
	int open_already;

	/* The refusals are forgotten once an input device came or went. */
	refusals_forget(backend, backend->input_refused_generation);

	/* Without the directory there is nothing to read. */
	directory = opendir(INPUT_DIRECTORY);
	if (directory == NULL)
		return;

	/* Every eventN entry is a candidate device. */
	while (1) {
		/* The end of the directory ends the scan. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only eventN nodes speak evdev. */
		valid = event_node_name(entry->d_name);
		if (!valid)
			continue;

		/* A node that refused to open is not opened again while no device came or went. */
		node = strtoul(entry->d_name + 5, NULL, 10);
		refused = node_refused(backend, backend->events_descriptor >= 0, node);
		if (refused)
			continue;

		/* An overlong name cannot be an ordinary device node. */
		length = snprintf(path, sizeof(path), "%s/%s", INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;

		/* A node the compositor already reads is left alone. */
		open_already = 0;
		if (backend->host.input_known != NULL)
			open_already = backend->host.input_known(backend->host.data, path);
		if (open_already)
			continue;

		/* The compositor classifies the node and keeps it if it is a pointer or a keyboard. */
		probe_device(backend, path, node);
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: every present pointer and keyboard is open. */
	return;
}

/*
 * Tells that zedBSD's backend opens the evdev nodes itself (there is no
 * seat service), so a thread of the compositor's may find them.
 */
int
kl_backend_input_find_supported(void)
{
	/* Succeeded: the nodes are opened directly. */
	return 1;
}

/*
 * Copies into a finding what it needs of the backend's state, on the event
 * loop's thread: whether the system's events are heard (without them a
 * refusal is never forgotten, so none is remembered) and how often an
 * input device came or went.
 */
void
kl_backend_input_find_begin(
	struct kl_backend *backend,
	struct kl_backend_input_find *find)
{
	/* Nothing is opened yet. */
	find->count = 0U;

	/* The system's events as the event loop knows them now. */
	find->events_heard = 0U;
	if (backend->events_descriptor >= 0)
		find->events_heard = 1U;
	find->refused_generation = backend->input_refused_generation;
}

/*
 * Opens every evdev node the finding does not name as known, on the
 * thread that runs it, and lists the ones whose capabilities could be
 * read; nothing is offered to the compositor.  A node beyond the list's
 * room is left for the next finding.  Returns 0 (a missing directory is
 * no error).
 */
int
kl_backend_input_find(
	struct kl_backend *backend,
	struct kl_backend_input_find *find)
{
	struct kl_backend_input_node *found;
	DIR *directory;
	struct dirent *entry;
	char path[KL_BACKEND_INPUT_PATH_MAX];
	unsigned long node;
	int length;
	int valid;
	int refused;
	int known;
	int error;

	/* The refusals are forgotten once an input device came or went. */
	refusals_forget(backend, find->refused_generation);

	/* Without the directory there is nothing to open. */
	find->count = 0U;
	directory = opendir(INPUT_DIRECTORY);
	if (directory == NULL)
		return 0;

	/* Every eventN entry is a candidate device, until the list is full. */
	while (find->count < KL_BACKEND_INPUT_FOUND_MAX) {
		/* The end of the directory ends the finding. */
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only eventN nodes speak evdev. */
		valid = event_node_name(entry->d_name);
		if (!valid)
			continue;

		/* A node that refused to open is not opened again while no device came or went. */
		node = strtoul(entry->d_name + 5, NULL, 10);
		refused = node_refused(backend, find->events_heard, node);
		if (refused)
			continue;

		/* An overlong name cannot be an ordinary device node. */
		length = snprintf(path, sizeof(path), "%s/%s", INPUT_DIRECTORY, entry->d_name);
		if (length < 0 || (size_t)length >= sizeof(path))
			continue;

		/* A node the compositor already reads is left alone. */
		known = find_known(find, path);
		if (known)
			continue;

		/* The node and its capabilities, into the list's next place. */
		found = &find->nodes[find->count];
		error = open_node(backend, find->events_heard, 1U, path, node, &found->descriptor, &found->caps);
		if (error != 0)
			continue;
		(void)snprintf(found->path, sizeof(found->path), "%s", path);
		find->count++;
	}

	/* The directory stream is no longer needed. */
	closedir(directory);

	/* Succeeded: the list holds the nodes opened. */
	return 0;
}

/*
 * Opens one input device for the devices' thread (path is
 * /dev/input/eventN), through the seat socket when there is one, and reads
 * its capabilities into node.  A failure is not remembered as a refusal:
 * only the findings pass refused nodes by.
 */
int
kl_backend_input_open(
	struct kl_backend *backend,
	const char *path,
	struct kl_backend_input_node *node)
{
	const char *name;
	unsigned long number;
	int valid;
	int error;

	/* The node's number, from its name. */
	name = strrchr(path, '/');
	if (name == NULL)
		return EINVAL;
	name++;
	valid = event_node_name(name);
	if (!valid)
		return EINVAL;
	number = strtoul(name + 5, NULL, 10);

	/* The node and its capabilities, asked of sessiond where it opens them. */
	node->descriptor = -1;
	error = open_node(backend, 0U, 1U, path, number, &node->descriptor, &node->caps);
	if (error != 0)
		return error;
	(void)snprintf(node->path, sizeof(node->path), "%s", path);

	/* Succeeded: the caller owns the descriptor. */
	return 0;
}

/*
 * Ends the devices' thread's wait and every wait for sessiond's answer,
 * for good (the cancel pipe stays readable).
 */
void
kl_backend_input_cancel(
	struct kl_backend *backend)
{
	char byte;
	ssize_t written;

	/* Nothing to end without the pipe. */
	if (backend == NULL || backend->cancel[1] < 0)
		return;

	/* One byte makes the read end readable from now on. */
	byte = 0;
	written = write(backend->cancel[1], &byte, 1U);
	if (written != 1)
		printf("KL SEAT cancel errno=%d\n", errno);
}

/*
 * Reads an input axis range.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_absinfo(
	int descriptor,
	uint32_t axis,
	struct input_absinfo *info)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGABS(axis), info);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads an input device's name.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_name(
	int descriptor,
	char *name,
	size_t size)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGNAME(size), name);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads an input device's identity.
 *
 * A nonnegative ioctl response is success; failures return an errno value.
 */
int
kl_backend_input_id(
	int descriptor,
	struct input_id *id)
{
	int error;

	/* Queries the device's metadata. */
	error = ioctl(descriptor, EVIOCGID, id);
	if (error < 0)
		return errno;

	/* Succeeded: the device supplied the metadata. */
	return 0;
}

/*
 * Reads whole input events without blocking.
 *
 * EOF returns zero; failures preserve read's errno, or use EIO for torn events.
 */
ssize_t
kl_backend_input_read(
	int descriptor,
	struct input_event *events,
	size_t capacity)
{
	ssize_t bytes;

	/* Reads as many whole events as the buffer holds. */
	bytes = read(descriptor, events, capacity * sizeof(events[0]));
	if (bytes < 0)
		return -1;

	/* Reports the end of the device. */
	if (bytes == 0)
		return 0;

	/* Refuses an incomplete evdev event. */
	if (((size_t)bytes % sizeof(events[0])) != 0) {
		errno = EIO;
		return -1;
	}

	/* Succeeded: returns the number of complete events. */
	return bytes / (ssize_t)sizeof(events[0]);
}

/*
 * Closes an input device descriptor.
 *
 * zedBSD does not need a separate seat service to relinquish the node.
 */
void
kl_backend_input_close(
	struct kl_backend *backend,
	int descriptor)
{
	(void)backend;

	/* Releases the node's descriptor. */
	close(descriptor);

	/* Succeeded: the descriptor is no longer owned by the seat. */
	return;
}

/* Reports whether a directory entry is named eventN. */
static int
event_node_name(
	const char *name)
{
	const char *cursor;
	int prefix;

	/* The name starts with "event" and has at least one more character. */
	prefix = strncmp(name, "event", 5);
	if (prefix != 0 || name[5] == '\0')
		return 0;

	/* Everything after the prefix is a decimal digit. */
	for (cursor = name + 5; *cursor != '\0'; cursor++) {
		/* Any other character makes it some other kind of node. */
		if (*cursor < '0' || *cursor > '9')
			return 0;
	}

	/* Succeeded: the entry is an evdev node. */
	return 1;
}

/*
 * Reports whether eventN refused to open since the system's events last
 * told of an input device that came or went.  Without the events nothing
 * would tell when to try again, so no node is passed by.
 */
static int
node_refused(
	const struct kl_backend *backend,
	unsigned events_heard,
	unsigned long node)
{
	uint64_t bit;

	/* Without the system's events, and past the remembered nodes, every node is tried. */
	if (!events_heard)
		return 0;
	if (node >= 64UL * KL_BACKEND_INPUT_REFUSED_WORDS)
		return 0;

	/* The node's bit. */
	bit = (uint64_t)1 << (node % 64UL);
	return (backend->input_refused[node / 64UL] & bit) != 0U;
}

/*
 * Forgets the nodes that refused to open when an input device came or
 * went since they were last forgotten (generation is the count of those
 * events the caller was given).
 */
static void
refusals_forget(
	struct kl_backend *backend,
	uint64_t generation)
{
	/* No device came or went since. */
	if (generation == backend->input_refused_seen)
		return;

	/* Every node is tried again. */
	memset(backend->input_refused, 0, sizeof(backend->input_refused));
	backend->input_refused_seen = generation;
}

/* Opens one node (eventN) and offers it, with its capability bitmaps, to the compositor. */
static void
probe_device(
	struct kl_backend *backend,
	const char *path,
	unsigned long node)
{
	struct kl_backend_input_caps capabilities;
	unsigned events_heard;
	int descriptor;
	int error;
	int kept;

	/* The node and its capabilities. */
	events_heard = 0U;
	if (backend->events_descriptor >= 0)
		events_heard = 1U;
	error = open_node(backend, events_heard, 0U, path, node, &descriptor, &capabilities);
	if (error != 0)
		return;

	/* The compositor's classification keeps it, or it is closed here. */
	kept = 0;
	if (backend->host.input_found != NULL)
		kept = backend->host.input_found(backend->host.data, descriptor, path, &capabilities);
	if (!kept)
		close(descriptor);
}

/*
 * Opens one node (eventN) and reads its capability bitmaps: sessiond opens
 * it when ask is set and the compositor has a seat socket, the caller
 * otherwise.  A failure of the device's own is remembered while the
 * system's events are heard.  Returns 0 with the descriptor (the
 * caller's), or an errno value.
 */
static int
open_node(
	struct kl_backend *backend,
	unsigned events_heard,
	unsigned ask,
	const char *path,
	unsigned long node,
	int *descriptor,
	struct kl_backend_input_caps *capabilities)
{
	int opened;
	int socket;
	int error;

	/* A nonblocking descriptor that children cannot inherit, from sessiond or opened here. */
	socket = seat_socket(backend);
	opened = -1;
	error = 0;
	if (ask && socket >= 0) {
		error = seat_open(backend, node, &opened);
	} else {
		opened = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (opened < 0)
			error = errno;
	}

	/*
	 * The device's own failure is remembered; a node not the seat's user's
	 * yet (EACCES) is tried at every scan.
	 */
	if (error != 0) {
		if (events_heard && (error == EIO || error == ENXIO || error == ENODEV) && node < 64UL * KL_BACKEND_INPUT_REFUSED_WORDS)
			backend->input_refused[node / 64UL] |= (uint64_t)1 << (node % 64UL);
		return error;
	}

	/* Reads the bits that determine the device's role. */
	error = read_capabilities(opened, capabilities);
	if (error != 0) {
		close(opened);
		return error;
	}

	/* Succeeded: the caller owns the open node. */
	*descriptor = opened;
	return 0;
}

/* Tells whether a finding names a path as one the compositor reads already. */
static int
find_known(
	const struct kl_backend_input_find *find,
	const char *path)
{
	unsigned index;
	int same;

	/* Each path the compositor gave. */
	for (index = 0U; index < find->known_count && index < KL_BACKEND_INPUT_KNOWN_MAX; index++) {
		same = strcmp(find->known[index], path);
		if (same == 0)
			return 1;
	}

	/* The node is not read yet. */
	return 0;
}

/* Reads the event, key, relative and absolute capability bitmaps and the properties of one node. */
static int
read_capabilities(
	int descriptor,
	struct kl_backend_input_caps *capabilities)
{
	int error;

	/* Absent bitmaps read as empty. */
	memset(capabilities, 0, sizeof(*capabilities));

	/* The event types the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(0, sizeof(capabilities->event)), capabilities->event);
	if (error < 0)
		return errno;

	/* The key and button codes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(capabilities->key)), capabilities->key);
	if (error < 0)
		return errno;

	/* The relative axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_REL, sizeof(capabilities->relative)), capabilities->relative);
	if (error < 0)
		return errno;

	/* The absolute axes the node can produce. */
	error = ioctl(descriptor, EVIOCGBIT(EV_ABS, sizeof(capabilities->absolute)), capabilities->absolute);
	if (error < 0)
		return errno;

	/* Its properties; a kernel without EVIOCGPROP leaves them empty (ws159-p004). */
	error = ioctl(descriptor, EVIOCGPROP(sizeof(capabilities->properties)), capabilities->properties);
	if (error < 0)
		memset(capabilities->properties, 0, sizeof(capabilities->properties));

	/* Succeeded: the bitmaps describe the node. */
	return 0;
}

/* Reports the seat socket sessiond gave the compositor, or -1 (none, or the standard input of cleared options). */
static int
seat_socket(
	const struct kl_backend *backend)
{
	/* Above the standard descriptors only. */
	if (backend->options.seat_descriptor <= STDERR_FILENO)
		return -1;

	/* Succeeded: the socket. */
	return backend->options.seat_descriptor;
}

/*
 * Asks sessiond to open /dev/input/eventN (node is N), one request at a
 * time.  Returns 0 with the descriptor, or an errno value: sessiond's
 * refusal, EPIPE once the socket is gone, ECANCELED after the cancel.
 */
static int
seat_open(
	struct kl_backend *backend,
	unsigned long node,
	int *descriptor)
{
	char request[INPUT_SEAT_LINE];
	int socket;
	int error;

	/* The request's line. */
	socket = seat_socket(backend);
	(void)snprintf(request, sizeof(request), "OPEN event%lu\n", node);

	/* The request and its answer, alone on the socket. */
	if (backend->seat_lock_ready)
		pthread_mutex_lock(&backend->seat_lock);

	/* Nothing is asked on a socket given up. */
	error = EPIPE;
	if (!backend->seat_gone)
		error = seat_ask(backend, socket, request, descriptor);

	if (backend->seat_lock_ready)
		pthread_mutex_unlock(&backend->seat_lock);

	/* The outcome, for the log the tests read. */
	printf("KL SEAT open node=event%lu error=%d\n", node, error);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the descriptor. */
	return 0;
}

/*
 * Writes one request on the seat socket and reads its answer, under
 * seat_lock.  A socket that ended, a wait cancelled or an answer not
 * understood leaves the socket gone (a late answer could follow).
 */
static int
seat_ask(
	struct kl_backend *backend,
	int socket,
	const char *request,
	int *descriptor)
{
	char answer[INPUT_SEAT_LINE];
	size_t length;
	ssize_t sent;
	char *end;
	long value;
	int same;
	int error;

	/* The request, whole. */
	*descriptor = -1;
	length = strlen(request);
	sent = send(socket, request, length, MSG_NOSIGNAL);
	if (sent != (ssize_t)length) {
		backend->seat_gone = 1U;
		return EPIPE;
	}

	/* The answer's line and the descriptor that may come with it. */
	error = seat_answer(backend, socket, answer, sizeof(answer), descriptor);
	if (error != 0) {
		if (*descriptor >= 0) {
			(void)close(*descriptor);
			*descriptor = -1;
		}

		/* No later answer can be told from this one's: the socket is given up. */
		backend->seat_gone = 1U;
		return error;
	}

	/* OK with the descriptor. */
	same = strcmp(answer, "OK");
	if (same == 0 && *descriptor >= 0)
		return 0;

	/* A descriptor with anything else is not kept. */
	if (*descriptor >= 0) {
		(void)close(*descriptor);
		*descriptor = -1;
	}

	/* FAIL errno: sessiond's refusal, the socket still good. */
	same = strncmp(answer, "FAIL ", 5U);
	if (same == 0) {
		value = strtol(answer + 5, &end, 10);
		if (end != answer + 5 && *end == '\0' && value > 0 && value < 4096)
			return (int)value;
	}

	/* Anything else: the protocol is lost. */
	backend->seat_gone = 1U;
	return EPROTO;
}

/*
 * Reads one answer line from the seat socket into answer (without its line
 * end) and the descriptor that came with it, waiting without a time limit
 * until it is whole, the socket ends (EPIPE) or the cancel (ECANCELED).
 */
static int
seat_answer(
	struct kl_backend *backend,
	int socket,
	char *answer,
	size_t size,
	int *descriptor)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *message_control;
	struct pollfd entries[2];
	struct msghdr header;
	struct iovec vector;
	size_t least;
	size_t used;
	ssize_t length;
	nfds_t count;
	char *end;
	int received;
	int ready;

	/* What comes, until the line ends. */
	used = 0U;
	for (;;) {
		/* The answer, or the cancel. */
		entries[0].fd = socket;
		entries[0].events = POLLIN;
		entries[0].revents = 0;
		entries[1].fd = backend->cancel[0];
		entries[1].events = POLLIN;
		entries[1].revents = 0;
		count = 1;
		if (backend->cancel[0] >= 0)
			count = 2;
		ready = poll(entries, count, -1);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return errno;
		if (count == 2 && entries[1].revents != 0)
			return ECANCELED;

		/* What came, with its descriptor. */
		memset(&header, 0, sizeof(header));
		memset(&control, 0, sizeof(control));
		vector.iov_base = answer + used;
		vector.iov_len = size - 1U - used;
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = control.space;
		header.msg_controllen = sizeof(control.space);
		length = recvmsg(socket, &header, MSG_CMSG_CLOEXEC);
		if (length < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (length <= 0)
			return EPIPE;

		/* The descriptor, the first that came (sessiond sends one at most). */
		message_control = CMSG_FIRSTHDR(&header);
		least = CMSG_LEN(sizeof(int));
		if (message_control != NULL &&
		    message_control->cmsg_level == SOL_SOCKET &&
		    message_control->cmsg_type == SCM_RIGHTS &&
		    message_control->cmsg_len >= least) {
			memcpy(&received, CMSG_DATA(message_control), sizeof(int));
			if (*descriptor < 0)
				*descriptor = received;
			else
				(void)close(received);
		}

		/* The line ends here, or the answer is longer than any. */
		used += (size_t)length;
		answer[used] = '\0';
		end = strchr(answer, '\n');
		if (end != NULL) {
			*end = '\0';
			break;
		}

		/* No room left for the rest of the line. */
		if (used + 1U >= size)
			return EPROTO;
	}

	/* Succeeded: the answer's line. */
	return 0;
}
