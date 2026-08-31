/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Test-only IN-T41 production USB HID/evdev guest probe. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/input.h>

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))
#define BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define BIT_WORDS(maximum) \
	(((maximum) + 1U + BITS_PER_WORD - 1U) / BITS_PER_WORD)
#define MAX_USB_INPUTS 16U
#define EVENT_BATCH 32U
#define RECORD_TIMEOUT_MS 15000
#define HOTPLUG_POLLS 400U
#define HOTPLUG_POLL_US 50000U
#define STORAGE_CHUNK 65536U
#define STORAGE_TARGET UINT64_C(67108864)

static unsigned char storage_buffer[STORAGE_CHUNK];

enum input_role {
	ROLE_NONE = 0,
	ROLE_KEYBOARD = 1,
	ROLE_RELATIVE = 2,
	ROLE_ABSOLUTE = 4
};

struct usb_input {
	char path[128];
	struct input_id id;
	enum input_role role;
};

static int
bit_set(const unsigned long *bits, unsigned code)
{
	return (bits[code / BITS_PER_WORD] &
		(1UL << (code % BITS_PER_WORD))) != 0;
}

static int
event_name(const char *name)
{
	const char *cursor;

	if (strncmp(name, "event", 5) != 0 || name[5] == '\0')
		return 0;
	for (cursor = name + 5; *cursor != '\0'; cursor++)
		if (*cursor < '0' || *cursor > '9')
			return 0;
	return 1;
}

static int
get_bits(int descriptor, unsigned type, unsigned long *bits, size_t size)
{
	memset(bits, 0, size);
	return ioctl(descriptor, EVIOCGBIT(type, size), bits);
}

static enum input_role
classify(int descriptor)
{
	unsigned long event_bits[BIT_WORDS(EV_MAX)];
	unsigned long key_bits[BIT_WORDS(KEY_MAX)];
	unsigned long relative_bits[BIT_WORDS(REL_MAX)];
	unsigned long absolute_bits[BIT_WORDS(ABS_MAX)];
	enum input_role role = ROLE_NONE;

	if (get_bits(descriptor, 0, event_bits, sizeof(event_bits)) != 0 ||
	    get_bits(descriptor, EV_KEY, key_bits, sizeof(key_bits)) != 0 ||
	    get_bits(descriptor, EV_REL, relative_bits,
		sizeof(relative_bits)) != 0 ||
	    get_bits(descriptor, EV_ABS, absolute_bits,
		sizeof(absolute_bits)) != 0)
		return ROLE_NONE;
	if (bit_set(event_bits, EV_KEY) && bit_set(key_bits, KEY_A) &&
	    bit_set(key_bits, KEY_ENTER) && bit_set(key_bits, KEY_LEFTSHIFT))
		role = (enum input_role)(role | ROLE_KEYBOARD);
	if (bit_set(event_bits, EV_REL) && bit_set(relative_bits, REL_X) &&
	    bit_set(relative_bits, REL_Y) && bit_set(event_bits, EV_KEY) &&
	    bit_set(key_bits, BTN_LEFT))
		role = (enum input_role)(role | ROLE_RELATIVE);
	if (bit_set(event_bits, EV_ABS) && bit_set(absolute_bits, ABS_X) &&
	    bit_set(absolute_bits, ABS_Y) && bit_set(event_bits, EV_KEY) &&
	    bit_set(key_bits, BTN_LEFT))
		role = (enum input_role)(role | ROLE_ABSOLUTE);
	return role;
}

static const char *
role_name(enum input_role role)
{
	switch (role) {
	case ROLE_KEYBOARD: return "keyboard";
	case ROLE_RELATIVE: return "relative";
	case ROLE_ABSOLUTE: return "absolute";
	default: return "unclassified";
	}
}

static enum input_role
parse_role(const char *name)
{
	if (strcmp(name, "keyboard") == 0)
		return ROLE_KEYBOARD;
	if (strcmp(name, "relative") == 0)
		return ROLE_RELATIVE;
	if (strcmp(name, "absolute") == 0)
		return ROLE_ABSOLUTE;
	return ROLE_NONE;
}

static int
scan_inputs(struct usb_input *inputs, unsigned *count)
{
	DIR *directory;
	struct dirent *entry;
	unsigned found = 0;
	int failed = 0;

	directory = opendir("/dev/input");
	if (directory == NULL) {
		fprintf(stderr, "USB-HID-GUEST FAIL opendir: %s\n",
			strerror(errno));
		return -1;
	}
	while ((entry = readdir(directory)) != NULL) {
		struct usb_input input;
		int descriptor;

		if (!event_name(entry->d_name))
			continue;
		if (snprintf(input.path, sizeof(input.path), "/dev/input/%s",
			entry->d_name) >= (int)sizeof(input.path)) {
			failed = 1;
			continue;
		}
		descriptor = open(input.path, O_RDONLY | O_NONBLOCK);
		if (descriptor < 0)
			continue;
		memset(&input.id, 0, sizeof(input.id));
		if (ioctl(descriptor, EVIOCGID, &input.id) != 0 ||
		    input.id.bustype != BUS_USB) {
			close(descriptor);
			continue;
		}
		input.role = classify(descriptor);
		close(descriptor);
		if (found == MAX_USB_INPUTS) {
			fprintf(stderr, "USB-HID-GUEST FAIL too many USB inputs\n");
			failed = 1;
			continue;
		}
		inputs[found++] = input;
	}
	closedir(directory);
	*count = found;
	return failed ? -1 : 0;
}

static int
role_matches(enum input_role role, struct usb_input *result, unsigned *matches)
{
	struct usb_input inputs[MAX_USB_INPUTS];
	unsigned count = 0, index;

	*matches = 0;
	if (scan_inputs(inputs, &count) != 0)
		return -1;
	for (index = 0; index < count; index++)
		if (inputs[index].role == role) {
			*result = inputs[index];
			(*matches)++;
		}
	return 0;
}

static int
unique_role(enum input_role role, struct usb_input *result)
{
	unsigned matches;

	if (role_matches(role, result, &matches) != 0)
		return -1;
	if (matches != 1U) {
		fprintf(stderr,
			"USB-HID-GUEST FAIL role=%s matches=%u\n",
			role_name(role), matches);
		return -1;
	}
	return 0;
}

static int
inventory(int argc, char **argv)
{
	struct usb_input inputs[MAX_USB_INPUTS];
	unsigned count = 0, index, expected = 0, seen = 0;
	int argument;

	for (argument = 2; argument < argc; argument++) {
		enum input_role role = parse_role(argv[argument]);

		if (role == ROLE_NONE || (expected & (unsigned)role) != 0) {
			fprintf(stderr, "USB-HID-GUEST FAIL invalid expected role\n");
			return 1;
		}
		expected |= (unsigned)role;
	}
	if (expected == 0 || scan_inputs(inputs, &count) != 0)
		return 1;
	for (index = 0; index < count; index++) {
		printf("USB-HID-GUEST DEVICE path=%s role=%s bus=%u "
		       "vendor=%04x product=%04x version=%04x\n",
		       inputs[index].path, role_name(inputs[index].role),
		       inputs[index].id.bustype, inputs[index].id.vendor,
		       inputs[index].id.product, inputs[index].id.version);
		if (inputs[index].role == ROLE_NONE ||
		    (expected & (unsigned)inputs[index].role) == 0 ||
		    (seen & (unsigned)inputs[index].role) != 0) {
			fprintf(stderr,
				"USB-HID-GUEST FAIL unexpected/duplicate USB role\n");
			return 1;
		}
		seen |= (unsigned)inputs[index].role;
	}
	if (seen != expected) {
		fprintf(stderr,
			"USB-HID-GUEST FAIL inventory expected=%u seen=%u\n",
			expected, seen);
		return 1;
	}
	printf("USB-HID-GUEST INVENTORY PASS roles=%u devices=%u\n", seen,
	       count);
	return 0;
}

static int
record_matches(enum input_role role, const struct input_event *events,
	unsigned count, int *first, int *second, int *sync)
{
	unsigned index;

	for (index = 0; index < count; index++) {
		const struct input_event *event = &events[index];

		if (event->type == EV_SYN && event->code == SYN_DROPPED) {
			fprintf(stderr, "USB-HID-GUEST FAIL SYN_DROPPED\n");
			return -1;
		}
		if (event->type == EV_SYN && event->code == SYN_REPORT) {
			*sync = 1;
			continue;
		}
		if (role == ROLE_KEYBOARD && event->type == EV_KEY &&
		    event->code == KEY_A) {
			printf("USB-HID-GUEST EVENT role=keyboard type=%u code=%u "
			       "value=%d\n", event->type, event->code,
			       event->value);
			if (event->value == 1)
				*first = 1;
			if (event->value == 0)
				*second = 1;
		}
		if (role == ROLE_RELATIVE && event->type == EV_REL &&
		    event->code == REL_X && event->value == 7) {
			printf("USB-HID-GUEST EVENT role=relative type=%u code=%u "
			       "value=%d\n", event->type, event->code,
			       event->value);
			*first = 1;
		}
		if (role == ROLE_RELATIVE && event->type == EV_REL &&
		    event->code == REL_Y && event->value == -5) {
			printf("USB-HID-GUEST EVENT role=relative type=%u code=%u "
			       "value=%d\n", event->type, event->code,
			       event->value);
			*second = 1;
		}
		if (role == ROLE_ABSOLUTE && event->type == EV_ABS &&
		    event->code == ABS_X && event->value == 20000) {
			printf("USB-HID-GUEST EVENT role=absolute type=%u code=%u "
			       "value=%d\n", event->type, event->code,
			       event->value);
			*first = 1;
		}
		if (role == ROLE_ABSOLUTE && event->type == EV_ABS &&
		    event->code == ABS_Y && event->value == 400) {
			printf("USB-HID-GUEST EVENT role=absolute type=%u code=%u "
			       "value=%d\n", event->type, event->code,
			       event->value);
			*second = 1;
		}
	}
	return 0;
}

static int
record_role(enum input_role role)
{
	struct usb_input input;
	struct input_event events[EVENT_BATCH];
	struct pollfd polled;
	int descriptor, first = 0, second = 0, sync = 0;
	int remaining = RECORD_TIMEOUT_MS;

	if (unique_role(role, &input) != 0)
		return 1;
	descriptor = open(input.path, O_RDONLY | O_NONBLOCK);
	if (descriptor < 0) {
		fprintf(stderr, "USB-HID-GUEST FAIL open %s: %s\n", input.path,
			strerror(errno));
		return 1;
	}
	printf("USB-HID-GUEST READY role=%s path=%s\n", role_name(role),
	       input.path);
	fflush(stdout);
	polled.fd = descriptor;
	polled.events = POLLIN | POLLHUP;
	while (remaining > 0 && !(first && second && sync)) {
		ssize_t bytes;
		int waited = remaining > 250 ? 250 : remaining;
		int result = poll(&polled, 1, waited);

		remaining -= waited;
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 || (polled.revents & (POLLERR | POLLHUP)) != 0) {
			fprintf(stderr, "USB-HID-GUEST FAIL record poll\n");
			close(descriptor);
			return 1;
		}
		if (result == 0)
			continue;
		bytes = read(descriptor, events, sizeof(events));
		if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (bytes <= 0 || (bytes % (ssize_t)sizeof(events[0])) != 0 ||
		    record_matches(role, events,
			(unsigned)(bytes / (ssize_t)sizeof(events[0])), &first,
			&second, &sync) != 0) {
			fprintf(stderr, "USB-HID-GUEST FAIL malformed event read\n");
			close(descriptor);
			return 1;
		}
	}
	close(descriptor);
	if (!(first && second && sync)) {
		fprintf(stderr, "USB-HID-GUEST FAIL record timeout role=%s\n",
			role_name(role));
		return 1;
	}
	printf("USB-HID-GUEST RECORD PASS role=%s\n", role_name(role));
	return 0;
}

static int
hotplug_role(enum input_role role)
{
	struct usb_input old_input, new_input;
	struct input_event events[EVENT_BATCH];
	struct pollfd polled;
	unsigned attempt, matches;
	int descriptor, detached = 0;

	if (unique_role(role, &old_input) != 0)
		return 1;
	descriptor = open(old_input.path, O_RDONLY | O_NONBLOCK);
	if (descriptor < 0)
		return 1;
	printf("USB-HID-GUEST HOTPLUG READY role=%s old=%s\n",
	       role_name(role), old_input.path);
	fflush(stdout);
	polled.fd = descriptor;
	polled.events = POLLIN | POLLHUP;
	for (attempt = 0; attempt < HOTPLUG_POLLS; attempt++) {
		ssize_t bytes;

		if (poll(&polled, 1, 50) < 0 && errno != EINTR)
			break;
		if ((polled.revents & POLLHUP) != 0) {
			detached = 1;
			break;
		}
		if ((polled.revents & POLLIN) == 0)
			continue;
		bytes = read(descriptor, events, sizeof(events));
		if (bytes == 0) {
			detached = 1;
			break;
		}
		if (bytes < 0 && errno != EAGAIN && errno != EINTR)
			break;
	}
	if (!detached) {
		fprintf(stderr, "USB-HID-GUEST FAIL detach timeout role=%s\n",
			role_name(role));
		close(descriptor);
		return 1;
	}
	printf("USB-HID-GUEST HOTPLUG DETACHED role=%s old=%s\n",
	       role_name(role), old_input.path);
	fflush(stdout);
	for (attempt = 0; attempt < HOTPLUG_POLLS; attempt++) {
		if (role_matches(role, &new_input, &matches) == 0 &&
		    matches == 1U) {
			if (strcmp(old_input.path, new_input.path) == 0) {
				fprintf(stderr,
					"USB-HID-GUEST FAIL stale event number reused\n");
				close(descriptor);
				return 1;
			}
			close(descriptor);
			printf("USB-HID-GUEST HOTPLUG RELEASED role=%s old=%s "
			       "current=%s\n", role_name(role), old_input.path,
			       new_input.path);
			fflush(stdout);
			break;
		}
		usleep(HOTPLUG_POLL_US);
	}
	if (attempt == HOTPLUG_POLLS) {
		fprintf(stderr, "USB-HID-GUEST FAIL reinsert timeout role=%s\n",
			role_name(role));
		close(descriptor);
		return 1;
	}
	/* The controller now removes the new generation. */
	for (attempt = 0; attempt < HOTPLUG_POLLS; attempt++) {
		if (role_matches(role, &new_input, &matches) == 0 && matches == 0U)
			break;
		usleep(HOTPLUG_POLL_US);
	}
	if (attempt == HOTPLUG_POLLS) {
		fprintf(stderr, "USB-HID-GUEST FAIL second detach timeout\n");
		return 1;
	}
	printf("USB-HID-GUEST HOTPLUG SECOND-DETACHED role=%s\n",
	       role_name(role));
	fflush(stdout);
	/* After the old fd closed, the lowest released event number is reusable. */
	for (attempt = 0; attempt < HOTPLUG_POLLS; attempt++) {
		if (role_matches(role, &new_input, &matches) == 0 &&
		    matches == 1U) {
			if (strcmp(old_input.path, new_input.path) != 0) {
				fprintf(stderr,
					"USB-HID-GUEST FAIL released event number not reused "
					"old=%s new=%s\n", old_input.path,
					new_input.path);
				return 1;
			}
			printf("USB-HID-GUEST HOTPLUG PASS role=%s reused=%s\n",
			       role_name(role), new_input.path);
			return 0;
		}
		usleep(HOTPLUG_POLL_US);
	}
	fprintf(stderr, "USB-HID-GUEST FAIL final reinsert timeout role=%s\n",
		role_name(role));
	return 1;
}

static int
storage_read(const char *path)
{
	uint64_t total = 0;
	int descriptor;

	descriptor = open(path, O_RDONLY);
	if (descriptor < 0) {
		fprintf(stderr, "USB-HID-GUEST FAIL storage open %s: %s\n",
			path, strerror(errno));
		return 1;
	}
	printf("USB-HID-GUEST STORAGE READY path=%s bytes=%llu\n", path,
	       (unsigned long long)STORAGE_TARGET);
	fflush(stdout);
	while (total < STORAGE_TARGET) {
		size_t requested = sizeof(storage_buffer);
		ssize_t bytes;

		if (STORAGE_TARGET - total < requested)
			requested = (size_t)(STORAGE_TARGET - total);
		bytes = read(descriptor, storage_buffer, requested);
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes < 0) {
			fprintf(stderr, "USB-HID-GUEST FAIL storage read: %s\n",
				strerror(errno));
			close(descriptor);
			return 1;
		}
		if (bytes == 0) {
			if (lseek(descriptor, 0, SEEK_SET) != 0) {
				fprintf(stderr,
					"USB-HID-GUEST FAIL storage rewind: %s\n",
					strerror(errno));
				close(descriptor);
				return 1;
			}
			continue;
		}
		total += (uint64_t)bytes;
	}
	close(descriptor);
	printf("USB-HID-GUEST STORAGE PASS bytes=%llu\n",
	       (unsigned long long)total);
	return 0;
}

static void
usage(const char *program)
{
	fprintf(stderr,
		"usage: %s inventory ROLE... | record ROLE | hotplug ROLE | "
		"storage DEVICE\n",
		program);
}

int
main(int argc, char **argv)
{
	enum input_role role;

	if (argc >= 3 && strcmp(argv[1], "inventory") == 0)
		return inventory(argc, argv);
	if (argc == 3 && strcmp(argv[1], "storage") == 0)
		return storage_read(argv[2]);
	if (argc != 3 ||
	    ((strcmp(argv[1], "record") != 0) &&
	     (strcmp(argv[1], "hotplug") != 0))) {
		usage(argv[0]);
		return 2;
	}
	role = parse_role(argv[2]);
	if (role == ROLE_NONE) {
		usage(argv[0]);
		return 2;
	}
	if (strcmp(argv[1], "record") == 0)
		return record_role(role);
	return hotplug_role(role);
}
