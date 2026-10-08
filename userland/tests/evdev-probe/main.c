/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The evdev probe (ws143-p005): finds an input device's node and prints
 * what it reports, for the QEMU checks of the HID input glue (USB devices,
 * and the Bluetooth devices of /dev/hid-host).
 *
 *   evdev-probe [-b BUS] [-n NAME] [-p PLACE] [-N] [-t MS] [-r MS] [-l]
 *
 * A node matches when its bus is BUS (usb, bluetooth, virtual or a
 * number), its name contains NAME and its physical place contains PLACE;
 * -N takes only a node that was not there when the probe started.  The
 * probe waits up to -t MS (5000) for a matching node, prints
 *   EVDEV node=/dev/input/eventN bus=B vendor=V product=P version=R ev=0xBITS name="..." phys=... uniq=...
 * and then each event as "EVDEV event type=T code=C value=V" until the
 * device goes ("EVDEV gone", the read answered ENODEV) or no event came
 * for -r MS (10000, "EVDEV quiet").  -l lists every matching node instead
 * and ends with "EVDEV count=N".  Lines are written as they come.  The
 * status is 0, or 1 when no node matched ("EVDEV none").
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <uapi/input.h>

/* The directory of the nodes, and the most nodes the probe remembers. */
#define PROBE_DIRECTORY		"/dev/input"
#define PROBE_NODES_MAX		64U

/* The longest text a device reports. */
#define PROBE_TEXT_MAX		64U

/* How often the directory is looked at again while waiting (milliseconds). */
#define PROBE_POLL_MS		100

/* The events one read takes. */
#define PROBE_EVENTS_READ	16U

/*
 * What the command line asks: the bus (-1 for any), the texts the name and
 * the place must contain (NULL for any), whether only a new node counts,
 * the waits, and whether to list.
 */
struct probe_options {
	int bus;
	const char *name;
	const char *place;
	int new_only;
	long wait_ms;
	long quiet_ms;
	int list;
};

/* The node numbers present when the probe started, for -N. */
static unsigned char probe_present[PROBE_NODES_MAX];

static int probe_parse(int argc, char **argv, struct probe_options *options);
static int probe_bus(const char *text);
static int probe_find(const struct probe_options *options, char *path, size_t size, int *descriptor);
static int probe_matches(const struct probe_options *options, int descriptor);
static void probe_describe(const char *path, int descriptor);
static int probe_list(const struct probe_options *options);
static int probe_read(int descriptor, long quiet_ms);
static void probe_note_present(void);
static int probe_node_number(const char *name);
static long probe_milliseconds(void);

/*
 * Finds the node and prints its events, or lists the matching nodes.
 */
int
main(
	int argc,
	char **argv)
{
	struct probe_options options;
	char path[300];
	int descriptor;
	int status;
	int found;

	/* Each line is written as it comes, also into a file. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* The command line. */
	status = probe_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: evdev-probe [-b BUS] [-n NAME] [-p PLACE] [-N] [-t MS] [-r MS] [-l]\n");
		return 2;
	}

	/* The nodes already there, which -N leaves out. */
	probe_note_present();

	/* A list of the matching nodes. */
	if (options.list) {
		status = probe_list(&options);
		return status;
	}

	/* The node, within the wait. */
	found = probe_find(&options, path, sizeof(path), &descriptor);
	if (!found) {
		printf("EVDEV none\n");
		return 1;
	}

	/* What it is, then its events. */
	probe_describe(path, descriptor);
	status = probe_read(descriptor, options.quiet_ms);
	(void)close(descriptor);
	return status;
}

/* Reads the command line into the options; nonzero for a mistake. */
static int
probe_parse(
	int argc,
	char **argv,
	struct probe_options *options)
{
	int option;

	/* The defaults: any node, a wait of 5 s, 10 s of quiet. */
	memset(options, 0, sizeof(*options));
	options->bus = -1;
	options->wait_ms = 5000;
	options->quiet_ms = 10000;

	/* Each option. */
	for (;;) {
		option = getopt(argc, argv, "b:n:p:Nt:r:l");
		if (option == -1)
			break;

		/* What the option sets. */
		switch (option) {
		case 'b':
			options->bus = probe_bus(optarg);
			if (options->bus < 0)
				return 1;
			break;
		case 'n':
			options->name = optarg;
			break;
		case 'p':
			options->place = optarg;
			break;
		case 'N':
			options->new_only = 1;
			break;
		case 't':
			options->wait_ms = strtol(optarg, NULL, 10);
			break;
		case 'r':
			options->quiet_ms = strtol(optarg, NULL, 10);
			break;
		case 'l':
			options->list = 1;
			break;
		default:
			return 1;
		}
	}

	/* No operands. */
	if (optind != argc)
		return 1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Reports the bus a text names (a word or a number), -1 for none. */
static int
probe_bus(
	const char *text)
{
	char *end;
	long number;
	int same;

	/* The words. */
	same = strcmp(text, "usb");
	if (same == 0)
		return BUS_USB;
	same = strcmp(text, "bluetooth");
	if (same == 0)
		return BUS_BLUETOOTH;
	same = strcmp(text, "virtual");
	if (same == 0)
		return BUS_VIRTUAL;
	/* A number. */
	number = strtol(text, &end, 0);
	if (*end != '\0' || number < 0 || number > 0xffff)
		return -1;

	/* Succeeded: the bus's number. */
	return (int)number;
}

/*
 * Waits for a matching node and opens it.  Reports 1 with the node's path
 * and its open descriptor, or 0 when none matched within the wait.
 */
static int
probe_find(
	const struct probe_options *options,
	char *path,
	size_t size,
	int *descriptor)
{
	struct dirent *entry;
	DIR *directory;
	long started;
	long now;
	int number;
	int matches;
	int opened;

	/* Looks until the wait is over. */
	started = probe_milliseconds();
	for (;;) {
		/* Each node of the directory. */
		directory = opendir(PROBE_DIRECTORY);
		while (directory != NULL) {
			entry = readdir(directory);
			if (entry == NULL)
				break;

			/* Only the event nodes, and with -N only new ones. */
			number = probe_node_number(entry->d_name);
			if (number < 0)
				continue;
			if (options->new_only && probe_present[number])
				continue;

			/* The node, opened to be asked. */
			snprintf(path, size, "%s/%s", PROBE_DIRECTORY, entry->d_name);
			opened = open(path, O_RDONLY | O_NONBLOCK);
			if (opened < 0)
				continue;

			/* A match ends the search. */
			matches = probe_matches(options, opened);
			if (matches) {
				(void)closedir(directory);
				*descriptor = opened;
				return 1;
			}

			/* Anything else is closed. */
			(void)close(opened);
		}

		/* The listing is done with. */
		if (directory != NULL)
			(void)closedir(directory);

		/* The wait is over. */
		now = probe_milliseconds();
		if (now - started >= options->wait_ms)
			return 0;

		/* A little later. */
		(void)poll(NULL, 0, PROBE_POLL_MS);
	}
}

/* Reports whether an open node is the device the options ask for. */
static int
probe_matches(
	const struct probe_options *options,
	int descriptor)
{
	struct input_id id;
	char name[PROBE_TEXT_MAX];
	char place[PROBE_TEXT_MAX];
	const char *found;
	int error;

	/* Its bus. */
	memset(&id, 0, sizeof(id));
	error = ioctl(descriptor, EVIOCGID, &id);
	if (error != 0)
		return 0;
	if (options->bus >= 0 && id.bustype != options->bus)
		return 0;

	/* Its name. */
	memset(name, 0, sizeof(name));
	error = ioctl(descriptor, EVIOCGNAME(sizeof(name) - 1U), name);
	if (error < 0)
		return 0;
	if (options->name != NULL) {
		found = strstr(name, options->name);
		if (found == NULL)
			return 0;
	}

	/* Its place (a device may have none). */
	memset(place, 0, sizeof(place));
	(void)ioctl(descriptor, EVIOCGPHYS(sizeof(place) - 1U), place);
	if (options->place != NULL) {
		found = strstr(place, options->place);
		if (found == NULL)
			return 0;
	}

	/* Succeeded: the device matches. */
	return 1;
}

/* Prints what a node is: its identity, its event types and its texts. */
static void
probe_describe(
	const char *path,
	int descriptor)
{
	struct input_id id;
	unsigned long types;
	char name[PROBE_TEXT_MAX];
	char place[PROBE_TEXT_MAX];
	char unique[PROBE_TEXT_MAX];

	/* The identity and the texts (empty when the device has none). */
	memset(&id, 0, sizeof(id));
	memset(name, 0, sizeof(name));
	memset(place, 0, sizeof(place));
	memset(unique, 0, sizeof(unique));
	types = 0;
	(void)ioctl(descriptor, EVIOCGID, &id);
	(void)ioctl(descriptor, EVIOCGNAME(sizeof(name) - 1U), name);
	(void)ioctl(descriptor, EVIOCGPHYS(sizeof(place) - 1U), place);
	(void)ioctl(descriptor, EVIOCGUNIQ(sizeof(unique) - 1U), unique);
	(void)ioctl(descriptor, EVIOCGBIT(0, sizeof(types)), &types);

	/* One line. */
	printf("EVDEV node=%s bus=%u vendor=%04x product=%04x version=%04x ev=0x%lx name=\"%s\" phys=%s uniq=%s\n",
	       path,
	       (unsigned)id.bustype,
	       (unsigned)id.vendor,
	       (unsigned)id.product,
	       (unsigned)id.version,
	       types,
	       name,
	       place,
	       unique);
}

/* Lists every matching node; the status is 0 when one matched. */
static int
probe_list(
	const struct probe_options *options)
{
	struct dirent *entry;
	DIR *directory;
	char path[300];
	unsigned count;
	int number;
	int matches;
	int opened;

	/* Each node of the directory. */
	count = 0;
	directory = opendir(PROBE_DIRECTORY);
	while (directory != NULL) {
		entry = readdir(directory);
		if (entry == NULL)
			break;

		/* Only the event nodes, and with -N only new ones. */
		number = probe_node_number(entry->d_name);
		if (number < 0)
			continue;
		if (options->new_only && probe_present[number])
			continue;

		/* The node, if it matches. */
		snprintf(path, sizeof(path), "%s/%s", PROBE_DIRECTORY, entry->d_name);
		opened = open(path, O_RDONLY | O_NONBLOCK);
		if (opened < 0)
			continue;
		matches = probe_matches(options, opened);
		if (matches) {
			probe_describe(path, opened);
			count++;
		}

		/* The node is closed again. */
		(void)close(opened);
	}

	/* The listing is done with. */
	if (directory != NULL)
		(void)closedir(directory);

	/* The count. */
	printf("EVDEV count=%u\n", count);
	if (count == 0U)
		return 1;

	/* Succeeded: a node matched. */
	return 0;
}

/*
 * Prints the node's events until it goes or is quiet for quiet_ms.  The
 * status is 0 either way, 1 for a read that failed otherwise.
 */
static int
probe_read(
	int descriptor,
	long quiet_ms)
{
	struct input_event events[PROBE_EVENTS_READ];
	struct pollfd entry;
	ssize_t length;
	size_t count;
	size_t index;
	int ready;

	/* Events until the device goes or nothing comes. */
	for (;;) {
		entry.fd = descriptor;
		entry.events = POLLIN;
		entry.revents = 0;
		ready = poll(&entry, 1, (int)quiet_ms);
		if (ready == 0) {
			printf("EVDEV quiet\n");
			return 0;
		}

		/* The events waiting. */
		length = read(descriptor, events, sizeof(events));
		if (length < 0 && errno == EAGAIN)
			continue;
		if (length < 0 && errno == ENODEV) {
			printf("EVDEV gone\n");
			return 0;
		}

		/* Any other failure. */
		if (length < 0) {
			printf("EVDEV error errno=%d\n", errno);
			return 1;
		}

		/* A node that ended. */
		if (length == 0) {
			printf("EVDEV gone\n");
			return 0;
		}

		/* Each event. */
		count = (size_t)length / sizeof(events[0]);
		for (index = 0; index < count; index++) {
			printf("EVDEV event type=%u code=%u value=%d\n",
			       (unsigned)events[index].type,
			       (unsigned)events[index].code,
			       (int)events[index].value);
		}
	}
}

/* Notes the event nodes present now, which -N leaves out. */
static void
probe_note_present(
	void)
{
	struct dirent *entry;
	DIR *directory;
	int number;

	/* Each event node of the directory. */
	directory = opendir(PROBE_DIRECTORY);
	while (directory != NULL) {
		entry = readdir(directory);
		if (entry == NULL)
			break;
		number = probe_node_number(entry->d_name);
		if (number >= 0)
			probe_present[number] = 1;
	}

	/* The listing is done with. */
	if (directory != NULL)
		(void)closedir(directory);
}

/* Reports the N of a name eventN (below PROBE_NODES_MAX), -1 for any other name. */
static int
probe_node_number(
	const char *name)
{
	char *end;
	long number;
	int prefix;

	/* The prefix. */
	prefix = strncmp(name, "event", 5);
	if (prefix != 0)
		return -1;

	/* The number after it, whole. */
	number = strtol(name + 5, &end, 10);
	if (end == name + 5 || *end != '\0')
		return -1;
	if (number < 0 || number >= (long)PROBE_NODES_MAX)
		return -1;

	/* Succeeded: the node's number. */
	return (int)number;
}

/* Reports the monotonic clock in milliseconds. */
static long
probe_milliseconds(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}
