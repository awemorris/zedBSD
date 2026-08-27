/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * IN-T12 guest probe.  Device-node names are used only for enumeration and
 * diagnostics.  Roles are derived exclusively from EVIOCGBIT results.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zedbsd/input.h>

#define BITS_PER_WORD (sizeof(unsigned long) * 8U)
#define BIT_WORDS(maximum)                                                     \
	(((maximum) + 1U + BITS_PER_WORD - 1U) / BITS_PER_WORD)
#define BIT_BYTES(maximum) (BIT_WORDS(maximum) * sizeof(unsigned long))

static int
bit_is_set(const unsigned long *bits, unsigned code)
{
	return (bits[code / BITS_PER_WORD] &
		(1UL << (code % BITS_PER_WORD))) != 0;
}

static int
event_node_name(const char *name)
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
query_bits(int descriptor, unsigned event_type, void *bits,
	size_t length, const char *path)
{
	memset(bits, 0, length);
	if (ioctl(descriptor, EVIOCGBIT(event_type, length), bits) == 0)
		return 0;
	fprintf(stderr, "IN-T12 FAIL path=%s EVIOCGBIT(%u): %s\n", path,
		event_type, strerror(errno));
	return -1;
}

static void
print_set_bits(const char *label, const unsigned long *bits, unsigned maximum)
{
	unsigned code;
	int separator = 0;

	printf(" %s=", label);
	for (code = 0; code <= maximum; code++) {
		if (!bit_is_set(bits, code))
			continue;
		printf("%s%u", separator ? "," : "", code);
		separator = 1;
	}
	if (!separator)
		putchar('-');
}

static int
is_keyboard(const unsigned long *event_bits, const unsigned long *key_bits)
{
	return bit_is_set(event_bits, EV_KEY) && bit_is_set(key_bits, KEY_A) &&
	       bit_is_set(key_bits, KEY_Z) && bit_is_set(key_bits, KEY_1) &&
	       bit_is_set(key_bits, KEY_0) && bit_is_set(key_bits, KEY_ENTER) &&
	       bit_is_set(key_bits, KEY_SPACE) &&
	       bit_is_set(key_bits, KEY_LEFTSHIFT) &&
	       bit_is_set(key_bits, KEY_LEFTCTRL);
}

static int
is_relative_pointer(const unsigned long *event_bits,
	const unsigned long *key_bits, const unsigned long *relative_bits)
{
	return bit_is_set(event_bits, EV_KEY) &&
	       bit_is_set(event_bits, EV_REL) &&
	       bit_is_set(relative_bits, REL_X) &&
	       bit_is_set(relative_bits, REL_Y) &&
	       bit_is_set(key_bits, BTN_LEFT) &&
	       bit_is_set(key_bits, BTN_RIGHT) &&
	       bit_is_set(key_bits, BTN_MIDDLE);
}

static int
bytes_are(const void *buffer, size_t length, unsigned char value)
{
	const unsigned char *bytes = buffer;
	size_t index;

	for (index = 0; index < length; index++)
		if (bytes[index] != value)
			return 0;
	return 1;
}

static int
expect_enotty_unchanged(int descriptor, unsigned long request, void *buffer,
	size_t length, const char *operation, const char *path)
{
	memset(buffer, 0xa5, length);
	errno = 0;
	if (ioctl(descriptor, request, buffer) == -1 && errno == ENOTTY &&
	    bytes_are(buffer, length, 0xa5))
		return 0;
	fprintf(stderr,
		"IN-T12 FAIL path=%s %s: result must be ENOTTY without mutation "
		"(errno=%d)\n",
		path, operation, errno);
	return -1;
}

static int
test_capability_copy_contract(int descriptor,
	const unsigned long *event_bits, const char *path)
{
	unsigned char short_bits[1];
	unsigned char oversized[BIT_BYTES(EV_MAX) + 2U * sizeof(unsigned long)];
	unsigned char rejected[BIT_BYTES(EV_MAX)];
	unsigned char unsupported[sizeof(unsigned long)];
	unsigned char untouched = 0xa5;
	size_t exact_size = BIT_BYTES(EV_MAX);
	unsigned long malformed;

	if (ioctl(descriptor, EVIOCGBIT(0, 0), &untouched) != 0 ||
	    untouched != 0xa5) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s zero-length EVIOCGBIT\n", path);
		return -1;
	}
	if (query_bits(descriptor, 0, short_bits, sizeof(short_bits), path) != 0)
		return -1;
	if (memcmp(short_bits, event_bits, sizeof(short_bits)) != 0) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s short EVIOCGBIT prefix mismatch\n",
			path);
		return -1;
	}
	memset(oversized, 0xa5, sizeof(oversized));
	if (ioctl(descriptor, EVIOCGBIT(0, sizeof(oversized)), oversized) != 0) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s oversized EVIOCGBIT: %s\n",
			path, strerror(errno));
		return -1;
	}
	if (memcmp(oversized, event_bits, exact_size) != 0 ||
	    !bytes_are(oversized + exact_size,
		    sizeof(oversized) - exact_size, 0)) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s oversized EVIOCGBIT copy/zero-fill\n",
			path);
		return -1;
	}
	malformed = ZEDBSD_IOC(ZEDBSD_IOC_IN, ZEDBSD_EVDEV_IOC_GROUP, 0x20,
		sizeof(rejected));
	if (expect_enotty_unchanged(descriptor, malformed, rejected,
		    sizeof(rejected), "malformed-direction EVIOCGBIT", path) != 0)
		return -1;
	if (expect_enotty_unchanged(descriptor,
		    EVIOCGBIT(EV_MSC, sizeof(unsupported)), unsupported,
		    sizeof(unsupported), "unsupported-type EVIOCGBIT", path) != 0)
		return -1;
	return 0;
}

static int
test_key_state(int descriptor, const unsigned long *key_bits, const char *path)
{
	unsigned long state[BIT_WORDS(KEY_MAX)];
	unsigned char oversized[BIT_BYTES(KEY_MAX) + sizeof(unsigned long)];
	unsigned char untouched = 0xa5;
	unsigned code;

	if (ioctl(descriptor, EVIOCGKEY(0), &untouched) != 0 ||
	    untouched != 0xa5) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s zero-length EVIOCGKEY\n", path);
		return -1;
	}
	memset(state, 0xa5, sizeof(state));
	if (ioctl(descriptor, EVIOCGKEY(sizeof(state)), state) != 0) {
		fprintf(stderr, "IN-T12 FAIL path=%s EVIOCGKEY: %s\n", path,
			strerror(errno));
		return -1;
	}
	for (code = 0; code <= KEY_MAX; code++)
		if (bit_is_set(state, code) && !bit_is_set(key_bits, code)) {
			fprintf(stderr,
				"IN-T12 FAIL path=%s EVIOCGKEY undeclared code=%u\n",
				path, code);
			return -1;
		}
	memset(oversized, 0xa5, sizeof(oversized));
	if (ioctl(descriptor, EVIOCGKEY(sizeof(oversized)), oversized) != 0) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s oversized EVIOCGKEY: %s\n", path,
			strerror(errno));
		return -1;
	}
	if (memcmp(oversized, state, sizeof(state)) != 0 ||
	    !bytes_are(oversized + sizeof(state),
		    sizeof(oversized) - sizeof(state), 0)) {
		fprintf(stderr,
			"IN-T12 FAIL path=%s oversized EVIOCGKEY copy/zero-fill\n",
			path);
		return -1;
	}
	return 0;
}

static int
test_unsupported_axis(int descriptor, const unsigned long *absolute_bits,
	const char *path)
{
	struct input_absinfo rejected;
	unsigned axis;

	for (axis = 0; axis <= ABS_MAX; axis++)
		if (!bit_is_set(absolute_bits, axis))
			return expect_enotty_unchanged(
			    descriptor, EVIOCGABS(axis), &rejected,
			    sizeof(rejected), "unsupported-axis EVIOCGABS", path);
	fprintf(stderr,
		"IN-T12 FAIL path=%s has no unadvertised ABS axis to reject\n",
		path);
	return -1;
}

static int
inspect_device(const char *path, unsigned *keyboards,
	unsigned *relative_pointers)
{
	unsigned long event_bits[BIT_WORDS(EV_MAX)];
	unsigned long key_bits[BIT_WORDS(KEY_MAX)];
	unsigned long relative_bits[BIT_WORDS(REL_MAX)];
	unsigned long absolute_bits[BIT_WORDS(ABS_MAX)];
	unsigned attempt;
	int descriptor = -1, keyboard, relative_pointer;

	for (attempt = 0; attempt < 5U; attempt++) {
		descriptor = open(path, O_RDONLY | O_NONBLOCK);
		if (descriptor >= 0 || errno != ENODEV)
			break;
		sleep(1);
	}
	if (descriptor < 0) {
		fprintf(stderr, "IN-T12 FAIL open path=%s attempts=%u: %s\n",
			path, attempt < 5U ? attempt + 1U : 5U, strerror(errno));
		return -1;
	}
	if (query_bits(descriptor, 0, event_bits, sizeof(event_bits), path) != 0)
		goto fail;
	if (query_bits(descriptor, EV_KEY, key_bits, sizeof(key_bits), path) != 0)
		goto fail;
	if (query_bits(descriptor, EV_REL, relative_bits,
		    sizeof(relative_bits), path) != 0)
		goto fail;
	if (query_bits(descriptor, EV_ABS, absolute_bits,
		    sizeof(absolute_bits), path) != 0)
		goto fail;
	if (test_capability_copy_contract(descriptor, event_bits, path) != 0 ||
	    test_key_state(descriptor, key_bits, path) != 0 ||
	    test_unsupported_axis(descriptor, absolute_bits, path) != 0)
		goto fail;
	close(descriptor);

	keyboard = is_keyboard(event_bits, key_bits);
	relative_pointer =
	    is_relative_pointer(event_bits, key_bits, relative_bits);
	*keyboards += (unsigned)keyboard;
	*relative_pointers += (unsigned)relative_pointer;
	printf("IN-T12 caps path=%s", path);
	print_set_bits("ev", event_bits, EV_MAX);
	print_set_bits("key", key_bits, KEY_MAX);
	print_set_bits("rel", relative_bits, REL_MAX);
	printf(" roles=%s%s%s boundaries=pass\n", keyboard ? "keyboard" : "",
	       keyboard && relative_pointer ? "," : "",
	       relative_pointer ? "relative-pointer" :
				  (keyboard ? "" : "unclassified"));
	return 0;

fail:
	close(descriptor);
	return -1;
}

int
main(void)
{
	DIR *directory;
	struct dirent *entry;
	unsigned devices = 0, keyboards = 0, relative_pointers = 0;
	int failed = 0;

	/* QEMU monitor sendkey releases may still be draining after exec. */
	sleep(1);
	directory = opendir("/dev/input");
	if (directory == NULL) {
		fprintf(stderr, "IN-T12 FAIL opendir /dev/input: %s\n",
			strerror(errno));
		return 1;
	}
	while ((entry = readdir(directory)) != NULL) {
		char path[sizeof("/dev/input/") + sizeof(entry->d_name)];

		if (!event_node_name(entry->d_name))
			continue;
		if (snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name) >=
		    (int)sizeof(path)) {
			fprintf(stderr, "IN-T12 FAIL event-node path too long\n");
			failed = 1;
			continue;
		}
		devices++;
		if (inspect_device(path, &keyboards, &relative_pointers) != 0)
			failed = 1;
	}
	closedir(directory);

	if (failed || keyboards != 1U || relative_pointers != 1U) {
		fprintf(stderr,
			"IN-T12 FAIL devices=%u keyboards=%u relative-pointers=%u\n",
			devices, keyboards, relative_pointers);
		return 1;
	}
	printf("IN-T12 PASS devices=%u keyboards=%u relative-pointers=%u\n",
	       devices, keyboards, relative_pointers);
	return 0;
}
