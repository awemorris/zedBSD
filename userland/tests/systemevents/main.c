/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * systemevents: reads the system's events of /dev/system (ws132-p002).
 *
 *   systemevents [-c CLASSES] [-n COUNT] [-t MS]
 *                 subscribes to CLASSES (a comma list of power, lid, ac,
 *                 battery, disk, input, network, usb, or all, the default),
 *                 prints "ready at_ms=T" once subscribed, then one line an
 *                 event:
 *                   event SEQ CLASS ACTION VALUE SUBJECT DETAIL at_ms=T
 *                 and ends after COUNT events or MS milliseconds without one
 *                 (default: never), with "done events=N at_ms=T"; T is the
 *                 event's time, or the reader's, in milliseconds of the
 *                 monotonic clock (T1-129: to tell a reader that timed out
 *                 early from a system that was still)
 *   systemevents -p
 *                 prints the power's state (KERN_SYSTEM_GET_POWER):
 *                   power lid=1 ac=1 battery=50 charging=1
 *                 with "-" for what is not known
 *   systemevents -x
 *                 checks the device's refusals: a read before the
 *                 subscription, a subscription to nothing or to an unknown
 *                 class, nonzero reserved words, a buffer smaller than one
 *                 record, an empty nonblocking read; prints "refusals ok"
 *
 * Every way out says why on standard error, and the exit status is 0 only
 * when the command did what it was asked.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <uapi/system.h>

/* The node of the system device. */
#define SYSTEM_NODE "/dev/system"

/* One class's name and bit. */
struct class_name {
	const char *name;
	uint32_t bit;
};

static const struct class_name class_names[] = {
	{"power", KERN_SYSTEM_EVENT_POWER},
	{"lid", KERN_SYSTEM_EVENT_LID},
	{"ac", KERN_SYSTEM_EVENT_AC},
	{"battery", KERN_SYSTEM_EVENT_BATTERY},
	{"disk", KERN_SYSTEM_EVENT_DISK},
	{"input", KERN_SYSTEM_EVENT_INPUT},
	{"network", KERN_SYSTEM_EVENT_NETWORK},
	{"usb", KERN_SYSTEM_EVENT_USB},
	{"camera", KERN_SYSTEM_EVENT_CAMERA},
	{"overflow", KERN_SYSTEM_EVENT_OVERFLOW},
};

static int parse_classes(const char *text, uint32_t *classes);
static const char *class_text(uint32_t bit);
static const char *action_text(uint32_t action);
static const char *text_or_dash(const char *text);
static int print_power(void);
static int refused(int result, int expected, const char *what);
static int check_refusals(void);
static int read_events(uint32_t classes, long count, int timeout_ms);
static unsigned long long now_ms(void);
static int print_records(const struct system_event *events, ssize_t length, long *seen);

int
main(
	int argc,
	char **argv)
{
	uint32_t classes;
	long count;
	int timeout_ms;
	int option;
	int error;

	/* Every class, without end, unless the options say. */
	classes = KERN_SYSTEM_EVENT_CLASSES;
	count = -1;
	timeout_ms = -1;
	for (;;) {
		/* The next option, until none is left. */
		option = getopt(argc, argv, "c:n:t:px");
		if (option == -1)
			break;

		/* What it asks. */
		switch (option) {
		case 'c':
			error = parse_classes(optarg, &classes);
			if (error != 0) {
				fprintf(stderr, "systemevents: unknown class in %s\n", optarg);
				return 2;
			}

			break;
		case 'n':
			count = strtol(optarg, NULL, 10);
			break;
		case 't':
			timeout_ms = (int)strtol(optarg, NULL, 10);
			break;
		case 'p':
			return print_power();
		case 'x':
			return check_refusals();
		default:
			fprintf(stderr, "usage: systemevents [-c CLASSES] [-n COUNT] [-t MS] | -p | -x\n");
			return 2;
		}
	}

	/* Succeeded so far: the events. */
	return read_events(classes, count, timeout_ms);
}

/* Parses a comma list of class names, or "all". */
static int
parse_classes(
	const char *text,
	uint32_t *classes)
{
	char copy[128];
	char *word;
	char *rest;
	size_t index;
	int found;
	int same;

	/* "all" is every class. */
	same = strcmp(text, "all");
	if (same == 0) {
		*classes = KERN_SYSTEM_EVENT_CLASSES;
		return 0;
	}

	/* Each word is a class. */
	snprintf(copy, sizeof(copy), "%s", text);
	*classes = 0;
	rest = copy;
	for (word = strsep(&rest, ","); word != NULL; word = strsep(&rest, ",")) {
		/* The word's class. */
		found = 0;
		for (index = 0; index < sizeof(class_names) / sizeof(class_names[0]); index++) {
			same = strcmp(word, class_names[index].name);
			if (same == 0) {
				*classes |= class_names[index].bit;
				found = 1;
			}
		}

		/* A word of no class. */
		if (!found)
			return -1;
	}

	/* Succeeded: the classes. */
	return 0;
}

/* Names a class bit. */
static const char *
class_text(
	uint32_t bit)
{
	size_t index;

	/* The class of the bit. */
	for (index = 0; index < sizeof(class_names) / sizeof(class_names[0]); index++) {
		if (class_names[index].bit == bit)
			return class_names[index].name;
	}

	/* A bit of no class. */
	return "unknown";
}

/* Names an action. */
static const char *
action_text(
	uint32_t action)
{
	/* The name of each action. */
	switch (action) {
	case KERN_SYSTEM_EVENT_ADD:
		return "add";
	case KERN_SYSTEM_EVENT_REMOVE:
		return "remove";
	case KERN_SYSTEM_EVENT_CHANGE:
		return "change";
	case KERN_SYSTEM_EVENT_PRESS:
		return "press";
	default:
		return "unknown";
	}
}

/* Gives a text, or "-" for an empty one, so that every field of a line is a word. */
static const char *
text_or_dash(
	const char *text)
{
	/* An empty text. */
	if (text[0] == '\0')
		return "-";

	/* Succeeded: the text. */
	return text;
}

/* Prints the power's state. */
static int
print_power(void)
{
	struct system_power_info info;
	char lid[16];
	char ac[16];
	char battery[16];
	char charging[16];
	int result;
	int fd;

	/* The device. */
	fd = open(SYSTEM_NODE, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "systemevents: %s: %s\n", SYSTEM_NODE, strerror(errno));
		return 1;
	}

	/* The state. */
	memset(&info, 0, sizeof(info));
	result = ioctl(fd, KERN_SYSTEM_GET_POWER, &info);
	if (result != 0) {
		fprintf(stderr, "systemevents: KERN_SYSTEM_GET_POWER: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/* The device is no longer needed. */
	close(fd);

	/* Each field, or "-" when it is not known. */
	snprintf(lid, sizeof(lid), "-");
	snprintf(ac, sizeof(ac), "-");
	snprintf(battery, sizeof(battery), "-");
	snprintf(charging, sizeof(charging), "-");
	if ((info.known & KERN_SYSTEM_POWER_HAS_LID) != 0U)
		snprintf(lid, sizeof(lid), "%u", (unsigned)info.lid_open);
	if ((info.known & KERN_SYSTEM_POWER_HAS_AC) != 0U)
		snprintf(ac, sizeof(ac), "%u", (unsigned)info.ac_online);
	if ((info.known & KERN_SYSTEM_POWER_HAS_BATTERY) != 0U) {
		snprintf(battery, sizeof(battery), "%u", (unsigned)info.battery_percent);
		snprintf(charging, sizeof(charging), "%u", (unsigned)info.battery_charging);
	}

	/* Succeeded: the line. */
	printf("power lid=%s ac=%s battery=%s charging=%s\n", lid, ac, battery, charging);
	return 0;
}

/* Checks one refusal: the call failed with the error expected.  Reports 1 when it did not. */
static int
refused(
	int result,
	int expected,
	const char *what)
{
	/* Refused as expected. */
	if (result < 0 && errno == expected)
		return 0;

	/* Anything else. */
	fprintf(stderr, "systemevents: %s: result %d errno %d, expected errno %d\n", what, result, errno, expected);
	return 1;
}

/* Checks the device's refusals. */
static int
check_refusals(void)
{
	struct system_event_subscription subscription;
	struct system_event event;
	char small[64];
	int failed;
	int result;
	int fd;

	/* The device, nonblocking. */
	fd = open(SYSTEM_NODE, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "systemevents: %s: %s\n", SYSTEM_NODE, strerror(errno));
		return 1;
	}

	/* A read before the subscription. */
	failed = 0;
	result = (int)read(fd, &event, sizeof(event));
	failed += refused(result, EINVAL, "read before subscription");

	/* A subscription to nothing, to an unknown class, with a reserved word set. */
	memset(&subscription, 0, sizeof(subscription));
	result = ioctl(fd, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	failed += refused(result, EINVAL, "subscription to nothing");
	subscription.classes = 0x100U;
	result = ioctl(fd, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	failed += refused(result, EINVAL, "unknown class");
	subscription.classes = KERN_SYSTEM_EVENT_DISK;
	subscription.reserved[1] = 1;
	result = ioctl(fd, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	failed += refused(result, EINVAL, "reserved word");

	/* A good subscription. */
	subscription.reserved[1] = 0;
	result = ioctl(fd, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (result != 0) {
		fprintf(stderr, "systemevents: subscription: %s\n", strerror(errno));
		failed++;
	}

	/* Then a buffer too small for a record, and an empty read. */
	result = (int)read(fd, small, sizeof(small));
	failed += refused(result, EINVAL, "buffer smaller than a record");
	result = (int)read(fd, &event, sizeof(event));
	failed += refused(result, EAGAIN, "empty nonblocking read");
	close(fd);

	/* Some refusal was wrong. */
	if (failed != 0) {
		fprintf(stderr, "systemevents: %d refusals wrong\n", failed);
		return 1;
	}

	/* Succeeded: every refusal. */
	printf("refusals ok\n");
	return 0;
}

/* Subscribes and prints events, until the count or the time without one. */
static int
read_events(
	uint32_t classes,
	long count,
	int timeout_ms)
{
	struct system_event_subscription subscription;
	struct system_event events[8];
	struct pollfd poller;
	ssize_t length;
	long seen;
	int result;
	int ready;
	int fd;

	/* The device. */
	fd = open(SYSTEM_NODE, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "systemevents: %s: %s\n", SYSTEM_NODE, strerror(errno));
		return 1;
	}

	/* The subscription. */
	memset(&subscription, 0, sizeof(subscription));
	subscription.classes = classes & KERN_SYSTEM_EVENT_CLASSES;
	result = ioctl(fd, KERN_SYSTEM_EVENT_SUBSCRIBE, &subscription);
	if (result != 0) {
		fprintf(stderr, "systemevents: KERN_SYSTEM_EVENT_SUBSCRIBE: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/* The reader is ready for the events the test makes. */
	printf("ready at_ms=%llu\n", now_ms());
	fflush(stdout);

	/* Each event, until the count or the time without one. */
	seen = 0;
	while (count < 0 || seen < count) {
		/* Waits for records. */
		poller.fd = fd;
		poller.events = POLLIN;
		poller.revents = 0;
		ready = poll(&poller, 1, timeout_ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			fprintf(stderr, "systemevents: poll: %s\n", strerror(errno));
			close(fd);
			return 1;
		}

		/* The time passed without one. */
		if (ready == 0)
			break;

		/* The records. */
		length = read(fd, events, sizeof(events));
		result = print_records(events, length, &seen);
		if (result != 0) {
			close(fd);
			return 1;
		}
	}

	/* The device is no longer needed. */
	close(fd);

	/* Succeeded: the count, on both outputs. */
	printf("done events=%ld at_ms=%llu\n", seen, now_ms());
	fprintf(stderr, "systemevents: done events=%ld\n", seen);
	return 0;
}

/* Gives the monotonic clock in milliseconds. */
static unsigned long long
now_ms(void)
{
	struct timespec now;
	int result;

	/* The clock, or 0 when it cannot be read. */
	result = clock_gettime(CLOCK_MONOTONIC, &now);
	if (result != 0)
		return 0;

	/* Succeeded: milliseconds. */
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Prints the records of one read.  Reports 1 when the read failed or gave a part of a record. */
static int
print_records(
	const struct system_event *events,
	ssize_t length,
	long *seen)
{
	size_t index;

	/* A failed read. */
	if (length < 0) {
		fprintf(stderr, "systemevents: read: %s\n", strerror(errno));
		return 1;
	}

	/* A read of a part of a record. */
	if (length % (ssize_t)sizeof(events[0]) != 0) {
		fprintf(stderr, "systemevents: read %zd bytes, not whole records\n", length);
		return 1;
	}

	/* One line a record. */
	for (index = 0; index < (size_t)length / sizeof(events[0]); index++) {
		printf("event %llu %s %s %d %s %s at_ms=%llu\n", (unsigned long long)events[index].sequence,
		       class_text(events[index].class_bit), action_text(events[index].action),
		       (int)events[index].value, text_or_dash(events[index].subject),
		       text_or_dash(events[index].detail), (unsigned long long)(events[index].time_ns / 1000000U));
		(*seen)++;
	}

	/* Succeeded: the lines go out now. */
	fflush(stdout);
	return 0;
}
