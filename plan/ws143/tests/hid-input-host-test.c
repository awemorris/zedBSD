/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the HID input glue (ws143-p005 i01a,
 * src/drivers/generic/hid-input.c).
 *
 * The glue was moved out of usb-hid.c, and must not change what a USB HID
 * device tells its readers.  The test runs the glue beside the old
 * usb-hid code (hid-input-old.c, copied from git a6988363c) on the same
 * descriptors and reports and checks that both register the same devices
 * and emit the same events in the same order:
 *
 *   check FILE...   fixed descriptors (a boot keyboard, a keyboard of two
 *                   report IDs, a mouse, a pen, a touch screen, and the
 *                   FILEs: the Latitude 5330's touchpad, Logitech's
 *                   receiver), each with fixed-seed random reports, and the
 *                   glue's own rules (held keys joined over report IDs, an
 *                   ErrorRollOver keeps the keys, no relative 0, a touch
 *                   screen alone, a full input layer, unpublishing twice),
 *                   and the pure checks of /dev/hid-host (its setup, a
 *                   report shorter than its ID declares).
 *   fuzz COUNT SEED FILE...
 *                   COUNT mutated and generated descriptors and random
 *                   reports for each, built with ASan and UBSan
 *                   (hid-report-fuzz.sh): the parser, the state machines and
 *                   the glue must not fault, the old and the new must agree
 *                   (errors included), and every key code stays within
 *                   KEY_MAX.
 *
 * The kernel's files and the old copy are compiled freestanding as the
 * kernel compiles them (their error numbers are the kernel's); this file,
 * compiled for the host, supplies the kernel's allocator, formatter and log, and an
 * input layer that records what it is given.
 */

#include "hid-input-old.h"

#include <drivers/generic/hid-host.h>
#include <drivers/generic/hid-input.h>
#include <kern/input-device.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/errno.h>

/* The devices the recording input layer keeps, and the events it records for one report. */
#define FAKE_DEVICES_MAX	64U
#define FAKE_EVENTS_MAX		8192U

/* The longest text the input layer takes, as input.c's INPUT_TEXT_MAX. */
#define FAKE_TEXT_MAX		64U

/* The capabilities one recorded device may declare: a main device's, or a touch device's. */
#define FAKE_CAPABILITIES_MAX	(HID_REPORT_FIELD_COUNT_MAX + 1U + HID_TOUCH_CAPABILITY_COUNT)

/* The random reports each fixed descriptor gets, and each fuzzed one. */
#define CHECK_REPORTS		20000U
#define FUZZ_REPORTS		200U

/* The longest report the test hands on: the parser's longest and some more. */
#define REPORT_BYTES_MAX	1100U

/* The longest descriptor read from a file or made by the fuzzer. */
#define DESCRIPTOR_BYTES_MAX	8192U

/* The descriptors read from the command line. */
#define FILES_MAX		8U

/* What a recorded event belongs to: the main device or the touch device of one side. */
#define ROLE_MAIN		0U
#define ROLE_TOUCH		1U
#define ROLE_OTHER		2U

/*
 * One device the recording input layer was given: whether it is still
 * registered, and everything its registration declared, copied.
 */
struct fake_device {
	int used;
	char name[FAKE_TEXT_MAX];
	char physical_path[FAKE_TEXT_MAX];
	char unique_id[FAKE_TEXT_MAX];
	struct input_id id;
	struct input_capability capabilities[FAKE_CAPABILITIES_MAX];
	size_t capability_count;
	struct input_abs_axis axes[ABS_MAX + 1U];
	size_t axis_count;
	uint32_t properties;
};

/* One event a device emitted, by the device's slot. */
struct fake_event {
	unsigned device;
	uint16_t type;
	uint16_t code;
	int32_t value;
	uint64_t milliseconds;
};

/* One event as the comparison sees it: the role of its device instead of its slot. */
struct role_event {
	unsigned role;
	uint16_t type;
	uint16_t code;
	int32_t value;
	uint64_t milliseconds;
};

/* One descriptor under test: its name for the messages, and its bytes. */
struct descriptor {
	const char *name;
	uint8_t bytes[DESCRIPTOR_BYTES_MAX];
	size_t size;
};

/* The registered devices of the recording input layer; a slot is reused once unregistered. */
static struct fake_device fake_devices[FAKE_DEVICES_MAX];

/*
 * How many more registrations the recording input layer accepts before it
 * answers ENOSPC, as a full input layer does.  Large unless a check lowers
 * it for one publication.
 */
static unsigned fake_registrations_left = 1000000U;

/* The events recorded since the log was last emptied; overflow is set when more came than it holds. */
static struct fake_event fake_events[FAKE_EVENTS_MAX];
static size_t fake_event_count;
static int fake_overflow;

/* The events of the old and of the new side for one report, by role. */
static struct role_event old_events[FAKE_EVENTS_MAX];
static struct role_event new_events[FAKE_EVENTS_MAX];

/* The old side's record (large, so not on the stack). */
static struct old_hid old_side;

/* The checks that failed, and the comparisons made. */
static unsigned failures;
static unsigned long comparisons;

/* The events the comparisons covered (each side's). */
static unsigned long events_compared;

/* The fuzzer's random state (a fixed seed makes a run repeatable). */
static uint64_t random_state = 0x9e3779b97f4a7c15ULL;

/* The descriptors of the command line. */
static struct descriptor file_descriptors[FILES_MAX];
static size_t file_descriptor_count;

/* A boot keyboard: modifiers, a reserved byte, six key slots, and five LEDs out. */
static const uint8_t boot_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xc0
};

/* A keyboard of two report IDs: ID 1 holds two key slots, ID 2 two more. */
static const uint8_t two_id_keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x85, 0x01, 0x05, 0x07, 0x95, 0x02, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
	0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0x85, 0x02, 0x95, 0x02, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
	0xc0
};

/* A wheel mouse: three buttons, five bits of padding, X, Y and the wheel, a byte each. */
static const uint8_t wheel_mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
	0x81, 0x06, 0xc0, 0xc0
};

/* A pen on a tablet (report ID 2): In Range and Tip Switch, X, Y and Tip Pressure. */
static const uint8_t tablet_pen[] = {
	0x05, 0x0d, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x20, 0xa1, 0x00,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x09, 0x32, 0x09, 0x42,
	0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x75, 0x10, 0x95, 0x01,
	0x26, 0x60, 0x54, 0x09, 0x30, 0x81, 0x02, 0x26, 0xbc, 0x34, 0x09, 0x31,
	0x81, 0x02, 0x05, 0x0d, 0x26, 0xff, 0x0f, 0x09, 0x30, 0x81, 0x02, 0xc0,
	0xc0
};

/*
 * A touch screen and nothing else (report ID 1): two fingers, each Tip
 * Switch, seven bits of padding, a Contact Identifier, X and Y.
 */
static const uint8_t touch_screen[] = {
	0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01,
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x95, 0x01, 0x09, 0x42, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x75, 0x08,
	0x95, 0x01, 0x25, 0x7f, 0x09, 0x51, 0x81, 0x02, 0x05, 0x01, 0x75, 0x10,
	0x26, 0xff, 0x0f, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0xc0,
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x95, 0x01, 0x09, 0x42, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03, 0x75, 0x08,
	0x95, 0x01, 0x25, 0x7f, 0x09, 0x51, 0x81, 0x02, 0x05, 0x01, 0x75, 0x10,
	0x26, 0xff, 0x0f, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02, 0xc0,
	0xc0
};

/* A vendor-defined report and nothing an evdev device declares. */
static const uint8_t vendor_only[] = {
	0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x08, 0x09, 0x01, 0x81, 0x02, 0xc0
};

void *kern_malloc(size_t size);
void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);
void *kern_memcpy(void *destination, const void *source, size_t count);
void *kern_memset(void *destination, int value, size_t count);
size_t kern_strnlen(const char *text, size_t maximum);
int kern_snprintf(char *buffer, size_t capacity, const char *format, ...) __attribute__((format(printf, 3, 4)));
void kern_logf(const char *format, ...) __attribute__((format(printf, 1, 2)));

static int run_check(int count, char **arguments);
static int run_fuzz(int count, char **arguments);
static int read_files(int count, char **arguments);
static void check(int condition, const char *what);
static void log_reset(void);
static unsigned device_slot(const struct input_device *device);
static unsigned old_role(unsigned slot);
static unsigned new_role(const struct hid_input *input, unsigned slot);
static size_t take_old_events(void);
static size_t take_new_events(const struct hid_input *input);
static int compare_devices(const struct input_device *old_device, int new_number, const char *what);
static int axis_differs(const struct input_abs_axis *old_axis, const struct input_abs_axis *new_axis);
static int compare_side_by_side(const struct descriptor *descriptor, unsigned reports, int quiet);
static void compare_reports(const struct descriptor *descriptor, struct hid_input *input, unsigned reports);
static size_t random_report(const struct hid_input *input, uint8_t *report);
static uint32_t random_next(void);
static void set_descriptor(struct descriptor *descriptor, const char *name, const uint8_t *bytes, size_t size);
static void check_keyboard(void);
static void check_two_ids(void);
static void check_mouse(void);
static void check_touch_alone(void);
static void check_vendor_only(void);
static void check_full_layer(void);
static void check_short_reports(void);
static void check_setup(void);
static void fuzz_mutate(struct descriptor *descriptor, const struct descriptor *base);
static void fuzz_generate(struct descriptor *descriptor);
static void fuzz_item(struct descriptor *descriptor);
static int publish_new(struct hid_input *input);
static int publish_old(void);
static void events_of(struct hid_input *input, const uint8_t *report, size_t length, struct role_event *events, size_t *count);

/*
 * Runs the check or the fuzz, as the first argument says.
 */
int
main(
	int count,
	char **arguments)
{
	int status;
	int same;

	/* The mode is the first argument. */
	if (count < 2) {
		fprintf(stderr, "usage: hid-input-host-test check FILE... | fuzz COUNT SEED FILE...\n");
		return 2;
	}

	/* The fixed check. */
	same = strcmp(arguments[1], "check");
	if (same == 0) {
		status = run_check(count - 2, arguments + 2);
		return status;
	}

	/* The fuzz. */
	same = strcmp(arguments[1], "fuzz");
	if (same == 0) {
		status = run_fuzz(count - 2, arguments + 2);
		return status;
	}

	/* Neither. */
	fprintf(stderr, "hid-input-host-test: unknown mode %s\n", arguments[1]);
	return 2;
}

/*
 * Allocates memory for the kernel's files, as the kernel heap does.
 */
void *
kern_malloc(
	size_t size)
{
	/* The host's heap, which ASan watches. */
	return malloc(size);
}

/*
 * Allocates zeroed memory for the kernel's files.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	/* The host's heap. */
	return calloc(count, size);
}

/*
 * Frees memory of the kernel's files.
 */
void
kern_free(
	void *pointer)
{
	/* The host's heap. */
	free(pointer);
}

/*
 * Copies bytes for the kernel's files, as kcrt does.
 */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	/* The host's copy. */
	return memcpy(destination, source, count);
}

/*
 * Fills bytes for the kernel's files, as kcrt does.
 */
void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	/* The host's fill. */
	return memset(destination, value, count);
}

/*
 * Measures a text for the kernel's files, at most maximum bytes, as kcrt does.
 */
size_t
kern_strnlen(
	const char *text,
	size_t maximum)
{
	/* The host's measure. */
	return strnlen(text, maximum);
}

/*
 * Formats text for the kernel's files, as kcrt does.
 */
int
kern_snprintf(
	char *buffer,
	size_t capacity,
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* The host's formatter. */
	va_start(arguments, format);
	length = vsnprintf(buffer, capacity, format, arguments);
	va_end(arguments);

	/* Succeeded: the length the whole text needs. */
	return length;
}

/*
 * Takes a line for the kernel's log, which the test does not print.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* The test only counts on the glue's own counters. */
	(void)format;
}

/*
 * Registers a device with the recording input layer: copies what it
 * declares, or answers ENOSPC when the layer is full and ENAMETOOLONG for a
 * text as long as the input layer refuses.
 */
int
drv_input_device_register(
	const struct input_device_info *info,
	struct input_device **result)
{
	struct fake_device *device;
	size_t name_length;
	size_t path_length;
	size_t unique_length;
	unsigned slot;

	/* A full layer, as INPUT_DEVICE_MAX makes one. */
	if (fake_registrations_left == 0U)
		return ENOSPC;

	/* The texts the input layer refuses. */
	name_length = strlen(info->name);
	path_length = strlen(info->physical_path);
	unique_length = strlen(info->unique_id);
	if (name_length >= FAKE_TEXT_MAX)
		return ENAMETOOLONG;
	if (path_length >= FAKE_TEXT_MAX)
		return ENAMETOOLONG;
	if (unique_length >= FAKE_TEXT_MAX)
		return ENAMETOOLONG;

	/* Declarations larger than the record holds would be a test's mistake. */
	if (info->capability_count > FAKE_CAPABILITIES_MAX)
		return EINVAL;
	if (info->absolute_axis_count > ABS_MAX + 1U)
		return EINVAL;

	/* The first free slot. */
	for (slot = 0; slot < FAKE_DEVICES_MAX; slot++) {
		/* A slot in use is passed by. */
		if (!fake_devices[slot].used)
			break;
	}

	/* No free slot is a full layer too. */
	if (slot == FAKE_DEVICES_MAX)
		return ENOSPC;

	/* What the registration declares. */
	device = &fake_devices[slot];
	memset(device, 0, sizeof(*device));
	device->used = 1;
	snprintf(device->name, sizeof(device->name), "%s", info->name);
	snprintf(device->physical_path, sizeof(device->physical_path), "%s", info->physical_path);
	snprintf(device->unique_id, sizeof(device->unique_id), "%s", info->unique_id);
	device->id = info->id;
	memcpy(device->capabilities, info->capabilities, info->capability_count * sizeof(info->capabilities[0]));
	device->capability_count = info->capability_count;
	memcpy(device->axes, info->absolute_axes, info->absolute_axis_count * sizeof(info->absolute_axes[0]));
	device->axis_count = info->absolute_axis_count;
	device->properties = info->properties;
	fake_registrations_left--;

	/* Succeeded: the device is the slot's record. */
	*result = (struct input_device *)device;
	return 0;
}

/*
 * Takes a device out of the recording input layer.
 */
void
drv_input_device_unregister(
	struct input_device *device)
{
	unsigned slot;

	/* The slot is free again. */
	slot = device_slot(device);
	check(fake_devices[slot].used, "unregister of a registered device");
	fake_devices[slot].used = 0;
}

/*
 * Records one event a device emitted.
 */
void
drv_input_device_emit_at(
	struct input_device *device,
	uint16_t type,
	uint16_t code,
	int32_t value,
	uint64_t milliseconds)
{
	struct fake_event *event;

	/* No device tells nothing, as the input layer does. */
	if (device == NULL)
		return;

	/* A log that is full marks the report. */
	if (fake_event_count == FAKE_EVENTS_MAX) {
		fake_overflow = 1;
		return;
	}

	/* The event, by its device's slot. */
	event = &fake_events[fake_event_count];
	event->device = device_slot(device);
	event->type = type;
	event->code = code;
	event->value = value;
	event->milliseconds = milliseconds;
	fake_event_count++;
}

/*
 * Reports a device's node number: its slot.
 */
unsigned
drv_input_device_number(
	const struct input_device *device)
{
	/* The slot stands for the node. */
	return device_slot(device);
}

/* Runs the fixed checks and the side-by-side comparison of the fixed descriptors. */
static int
run_check(
	int count,
	char **arguments)
{
	static struct descriptor fixed;
	size_t index;
	int error;

	/* The descriptors of the command line. */
	error = read_files(count, arguments);
	if (error != 0)
		return 2;

	/* The glue's own rules. */
	check_keyboard();
	check_two_ids();
	check_mouse();
	check_touch_alone();
	check_vendor_only();
	check_full_layer();
	check_short_reports();
	check_setup();

	/* The old and the new side by side on each fixed descriptor. */
	set_descriptor(&fixed, "boot keyboard", boot_keyboard, sizeof(boot_keyboard));
	(void)compare_side_by_side(&fixed, CHECK_REPORTS, 0);
	set_descriptor(&fixed, "two-ID keyboard", two_id_keyboard, sizeof(two_id_keyboard));
	(void)compare_side_by_side(&fixed, CHECK_REPORTS, 0);
	set_descriptor(&fixed, "wheel mouse", wheel_mouse, sizeof(wheel_mouse));
	(void)compare_side_by_side(&fixed, CHECK_REPORTS, 0);
	set_descriptor(&fixed, "tablet pen", tablet_pen, sizeof(tablet_pen));
	(void)compare_side_by_side(&fixed, CHECK_REPORTS, 0);
	set_descriptor(&fixed, "touch screen", touch_screen, sizeof(touch_screen));
	(void)compare_side_by_side(&fixed, CHECK_REPORTS, 0);
	for (index = 0; index < file_descriptor_count; index++)
		(void)compare_side_by_side(&file_descriptors[index], CHECK_REPORTS, 0);

	/* The verdict. */
	printf("hid-input-host-test: %lu comparisons, %lu events, %u failures\n", comparisons, events_compared, failures);
	if (failures != 0U) {
		printf("hid-input-host-test: FAIL\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("hid-input-host-test: PASS\n");
	return 0;
}

/* Runs the fuzz on the number of descriptors given, from the seed given. */
static int
run_fuzz(
	int count,
	char **arguments)
{
	static struct descriptor bases[FILES_MAX + 5U];
	static struct descriptor fuzzed;
	unsigned long iterations;
	unsigned long accepted;
	unsigned long total;
	size_t base_count;
	uint32_t choice;
	int error;

	/* The count, the seed, and the descriptors of the command line. */
	if (count < 2) {
		fprintf(stderr, "usage: hid-input-host-test fuzz COUNT SEED FILE...\n");
		return 2;
	}

	/* The count, the generator's start from the seed, and the files. */
	total = strtoul(arguments[0], NULL, 10);
	random_state = strtoull(arguments[1], NULL, 0) * 2862933555777941757ULL + 3037000493ULL;
	error = read_files(count - 2, arguments + 2);
	if (error != 0)
		return 2;

	/* The bases the mutations start from. */
	set_descriptor(&bases[0], "boot keyboard", boot_keyboard, sizeof(boot_keyboard));
	set_descriptor(&bases[1], "two-ID keyboard", two_id_keyboard, sizeof(two_id_keyboard));
	set_descriptor(&bases[2], "wheel mouse", wheel_mouse, sizeof(wheel_mouse));
	set_descriptor(&bases[3], "tablet pen", tablet_pen, sizeof(tablet_pen));
	set_descriptor(&bases[4], "touch screen", touch_screen, sizeof(touch_screen));
	base_count = 5U;
	memcpy(&bases[base_count], file_descriptors, file_descriptor_count * sizeof(file_descriptors[0]));
	base_count += file_descriptor_count;

	/* The descriptors: half mutated from a base, half made from items. */
	accepted = 0;
	for (iterations = 0; iterations < total; iterations++) {
		/* The next descriptor. */
		choice = random_next();
		if ((choice & 1U) != 0U) {
			choice = random_next();
			fuzz_mutate(&fuzzed, &bases[choice % base_count]);
		} else {
			fuzz_generate(&fuzzed);
		}

		/* The two sides on it, quietly unless they disagree. */
		accepted += (unsigned long)compare_side_by_side(&fuzzed, FUZZ_REPORTS, 1);
	}

	/* The verdict. */
	printf("hid-report-fuzz: %lu descriptors (%lu accepted), %lu comparisons, %lu events, %u failures\n",
	       iterations,
	       accepted,
	       comparisons,
	       events_compared,
	       failures);
	if (failures != 0U) {
		printf("hid-report-fuzz: FAIL\n");
		return 1;
	}

	/* Succeeded: nothing faulted and the two sides agreed. */
	printf("hid-report-fuzz: PASS\n");
	return 0;
}

/* Reads the descriptors the command line names. */
static int
read_files(
	int count,
	char **arguments)
{
	struct descriptor *descriptor;
	FILE *file;
	int index;

	/* Each file, up to FILES_MAX. */
	for (index = 0; index < count; index++) {
		/* More files than the table holds are a usage mistake. */
		if (file_descriptor_count == FILES_MAX) {
			fprintf(stderr, "hid-input-host-test: more than %u files\n", FILES_MAX);
			return 1;
		}

		/* The file's bytes. */
		file = fopen(arguments[index], "rb");
		if (file == NULL) {
			fprintf(stderr, "hid-input-host-test: cannot open %s\n", arguments[index]);
			return 1;
		}

		/* Read whole, up to the record's room. */
		descriptor = &file_descriptors[file_descriptor_count];
		descriptor->name = arguments[index];
		descriptor->size = fread(descriptor->bytes, 1, sizeof(descriptor->bytes), file);
		(void)fclose(file);
		file_descriptor_count++;
	}

	/* Succeeded: the files are read. */
	return 0;
}

/* Counts a failed check and says which. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;

	/* The failure. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Empties the event log. */
static void
log_reset(
	void)
{
	/* No event, no overflow. */
	fake_event_count = 0;
	fake_overflow = 0;
}

/* Reports the slot of a recorded device. */
static unsigned
device_slot(
	const struct input_device *device)
{
	const struct fake_device *record;

	/* The device is a slot's record. */
	record = (const struct fake_device *)device;
	return (unsigned)(record - fake_devices);
}

/* Reports the role of a slot on the old side. */
static unsigned
old_role(
	unsigned slot)
{
	unsigned main_slot;
	unsigned touch_slot;

	/* The old side's main device. */
	if (old_side.input != NULL) {
		main_slot = device_slot(old_side.input);
		if (main_slot == slot)
			return ROLE_MAIN;
	}

	/* Its touch device. */
	if (old_side.touch_input != NULL) {
		touch_slot = device_slot(old_side.touch_input);
		if (touch_slot == slot)
			return ROLE_TOUCH;
	}

	/* Some other device's, which would be a mistake. */
	return ROLE_OTHER;
}

/* Reports the role of a slot on the new side. */
static unsigned
new_role(
	const struct hid_input *input,
	unsigned slot)
{
	int event;
	int touch_event;

	/* The new side's devices. */
	drv_hid_input_numbers(input, &event, &touch_event);

	/* Its main device. */
	if (event >= 0 && (unsigned)event == slot)
		return ROLE_MAIN;

	/* Its touch device. */
	if (touch_event >= 0 && (unsigned)touch_event == slot)
		return ROLE_TOUCH;

	/* Some other device's. */
	return ROLE_OTHER;
}

/* Moves the log into the old side's events, by role. */
static size_t
take_old_events(
	void)
{
	size_t index;

	/* Each recorded event. */
	for (index = 0; index < fake_event_count; index++) {
		old_events[index].role = old_role(fake_events[index].device);
		old_events[index].type = fake_events[index].type;
		old_events[index].code = fake_events[index].code;
		old_events[index].value = fake_events[index].value;
		old_events[index].milliseconds = fake_events[index].milliseconds;
	}

	/* The number of events. */
	return fake_event_count;
}

/* Moves the log into the new side's events, by role. */
static size_t
take_new_events(
	const struct hid_input *input)
{
	size_t index;

	/* Each recorded event. */
	for (index = 0; index < fake_event_count; index++) {
		new_events[index].role = new_role(input, fake_events[index].device);
		new_events[index].type = fake_events[index].type;
		new_events[index].code = fake_events[index].code;
		new_events[index].value = fake_events[index].value;
		new_events[index].milliseconds = fake_events[index].milliseconds;
	}

	/* The number of events. */
	return fake_event_count;
}

/*
 * Compares what the old and the new side registered for one device: its
 * texts, identity, capabilities, axes and properties.  Reports 1 when they
 * are the same.
 */
static int
compare_devices(
	const struct input_device *old_device,
	int new_number,
	const char *what)
{
	const struct fake_device *old_record;
	const struct fake_device *new_record;
	const struct input_abs_axis *old_axis;
	const struct input_abs_axis *new_axis;
	size_t index;
	int differs;
	int texts;
	int identities;
	int capabilities;

	/* Neither side has the device. */
	if (old_device == NULL && new_number < 0)
		return 1;

	/* One side has it and the other not. */
	if (old_device == NULL || new_number < 0) {
		check(0, what);
		return 0;
	}

	/* Every field of the two registrations. */
	old_record = (const struct fake_device *)old_device;
	new_record = &fake_devices[new_number];
	differs = 0;
	texts = strcmp(old_record->name, new_record->name);
	texts |= strcmp(old_record->physical_path, new_record->physical_path);
	texts |= strcmp(old_record->unique_id, new_record->unique_id);
	identities = memcmp(&old_record->id, &new_record->id, sizeof(old_record->id));
	if (texts != 0)
		differs = 1;
	else if (identities != 0)
		differs = 1;
	else if (old_record->capability_count != new_record->capability_count)
		differs = 1;
	else if (old_record->axis_count != new_record->axis_count)
		differs = 1;
	else if (old_record->properties != new_record->properties)
		differs = 1;

	/* The capabilities, once their counts agree. */
	if (!differs) {
		capabilities = memcmp(old_record->capabilities,
				      new_record->capabilities,
				      old_record->capability_count * sizeof(old_record->capabilities[0]));
		if (capabilities != 0)
			differs = 1;
	}

	/* The axes field by field (their padding is not theirs to compare). */
	for (index = 0; index < old_record->axis_count && !differs; index++) {
		old_axis = &old_record->axes[index];
		new_axis = &new_record->axes[index];
		differs = axis_differs(old_axis, new_axis);
	}

	/* The verdict. */
	comparisons++;
	check(!differs, what);
	if (differs)
		return 0;

	/* Succeeded: the registrations are the same. */
	return 1;
}

/* Reports whether two registered axes differ in their code or range. */
static int
axis_differs(
	const struct input_abs_axis *old_axis,
	const struct input_abs_axis *new_axis)
{
	/* Each field of the axis. */
	if (old_axis->code != new_axis->code)
		return 1;
	if (old_axis->info.value != new_axis->info.value)
		return 1;
	if (old_axis->info.minimum != new_axis->info.minimum)
		return 1;
	if (old_axis->info.maximum != new_axis->info.maximum)
		return 1;
	if (old_axis->info.fuzz != new_axis->info.fuzz)
		return 1;
	if (old_axis->info.flat != new_axis->info.flat)
		return 1;
	if (old_axis->info.resolution != new_axis->info.resolution)
		return 1;

	/* The same axis. */
	return 0;
}

/*
 * Prepares, publishes and feeds random reports to the old and the new side
 * on one descriptor and compares every step.  Reports 1 when the
 * descriptor was accepted (by both).
 */
static int
compare_side_by_side(
	const struct descriptor *descriptor,
	unsigned reports,
	int quiet)
{
	struct hid_input *input;
	char old_name[FAKE_TEXT_MAX];
	const char *new_name;
	unsigned kind;
	int old_error;
	int new_error;
	int event;
	int touch_event;
	char message[160];

	/* Both sides parse and describe it. */
	input = NULL;
	old_error = old_hid_prepare(&old_side, descriptor->bytes, descriptor->size);
	new_error = drv_hid_input_prepare(descriptor->bytes, descriptor->size, &input);
	comparisons++;
	if (old_error != new_error) {
		snprintf(message, sizeof(message), "%s: prepare old=%d new=%d", descriptor->name, old_error, new_error);
		check(0, message);
	}

	/* A refused descriptor has nothing more to compare. */
	if (old_error != 0 || new_error != 0) {
		old_hid_destroy(&old_side);
		if (new_error == 0)
			drv_hid_input_destroy(input);
		if (!quiet)
			printf("ok: %s (%zu bytes): both refuse it (%d)\n", descriptor->name, descriptor->size, new_error);
		return 0;
	}

	/* The longest report, and the bound the transports rely on. */
	snprintf(message, sizeof(message), "%s: report_max", descriptor->name);
	check(old_side.buffer_size == drv_hid_input_report_max(input), message);
	check(drv_hid_input_report_max(input) <= HID_REPORT_BITS_MAX / 8U + 1U, message);

	/* The name of a device without one of its own. */
	old_hid_fallback_name(&old_side, old_name, sizeof(old_name));
	kind = drv_hid_input_kind(input);
	new_name = "USB HID keyboard";
	if (kind == HID_INPUT_KIND_PEN)
		new_name = "USB HID pen";
	else if (kind == HID_INPUT_KIND_TABLET)
		new_name = "USB HID tablet";
	else if (kind == HID_INPUT_KIND_MOUSE)
		new_name = "USB HID mouse";
	snprintf(message, sizeof(message), "%s: fallback name old=%s", descriptor->name, old_name);
	check(strcmp(old_name, new_name) == 0, message);

	/* Both publish; they must succeed or fail alike, and register alike. */
	old_error = publish_old();
	new_error = publish_new(input);
	snprintf(message, sizeof(message), "%s: publish old=%d new=%d", descriptor->name, old_error, new_error);
	check(old_error == new_error, message);
	drv_hid_input_numbers(input, &event, &touch_event);
	snprintf(message, sizeof(message), "%s: main device", descriptor->name);
	(void)compare_devices(old_side.input, event, message);
	snprintf(message, sizeof(message), "%s: touch device", descriptor->name);
	(void)compare_devices(old_side.touch_input, touch_event, message);

	/* The reports, when both published. */
	if (old_error == 0 && new_error == 0)
		compare_reports(descriptor, input, reports);

	/* Both take their devices back and free them. */
	old_hid_unpublish(&old_side);
	drv_hid_input_unpublish(input);
	old_hid_destroy(&old_side);
	drv_hid_input_destroy(input);

	/* A summary line for a fixed descriptor. */
	if (!quiet)
		printf("ok: %s (%zu bytes): old and new agree on %u reports\n", descriptor->name, descriptor->size, reports);

	/* Succeeded: the descriptor was accepted. */
	return 1;
}

/* Feeds random reports to both sides and compares the events of each. */
static void
compare_reports(
	const struct descriptor *descriptor,
	struct hid_input *input,
	unsigned reports)
{
	static uint8_t report[REPORT_BYTES_MAX];
	size_t old_count;
	size_t new_count;
	size_t length;
	size_t index;
	unsigned number;
	uint64_t milliseconds;
	char message[160];
	int differs;
	int events;

	/* Each report, at a later time each. */
	milliseconds = 1000;
	for (number = 0; number < reports; number++) {
		length = random_report(input, report);
		milliseconds += 1 + (random_next() % 20U);

		/* The old side's events. */
		log_reset();
		old_hid_report(&old_side, report, length, milliseconds);
		old_count = take_old_events();
		check(!fake_overflow, "old event log overflow");

		/* The new side's. */
		log_reset();
		drv_hid_input_report(input, report, length, milliseconds);
		new_count = take_new_events(input);
		check(!fake_overflow, "new event log overflow");

		/* The same events, in the same order, to the same roles. */
		comparisons++;
		differs = 0;
		if (old_count != new_count)
			differs = 1;
		if (!differs) {
			events = memcmp(old_events, new_events, old_count * sizeof(old_events[0]));
			if (events != 0)
				differs = 1;
		}

		/* A difference ends the descriptor's reports. */
		if (differs) {
			snprintf(message,
				 sizeof(message),
				 "%s: report %u (length %zu): old %zu events, new %zu",
				 descriptor->name,
				 number,
				 length,
				 old_count,
				 new_count);
			check(0, message);
			return;
		}

		/* Every event stays within the event types and the key codes. */
		events_compared += new_count;
		for (index = 0; index < new_count; index++) {
			check(new_events[index].role != ROLE_OTHER, "an event of no device of this side");
			check(new_events[index].type <= EV_MAX, "an event type beyond EV_MAX");
			if (new_events[index].type == EV_KEY)
				check(new_events[index].code <= KEY_MAX, "a key code beyond KEY_MAX");
		}
	}
}

/*
 * Makes one random report: mostly the length of a declared report and its
 * ID, sometimes any length and any first byte.
 */
static size_t
random_report(
	const struct hid_input *input,
	uint8_t *report)
{
	size_t maximum;
	size_t length;
	size_t index;
	uint32_t choice;
	int numbered;

	/* The length: mostly the longest report's, sometimes any up to a little more. */
	maximum = drv_hid_input_report_max(input);
	choice = random_next() % 10U;
	length = maximum;
	if (choice == 0U)
		length = random_next() % (maximum + 4U);
	else if (choice == 1U)
		length = random_next() % REPORT_BYTES_MAX;

	/* The bytes: random, with few bits set in most of them so keys come and go. */
	for (index = 0; index < length; index++) {
		report[index] = (uint8_t)random_next();
		choice = random_next();
		if ((choice % 4U) != 0U)
			report[index] &= (uint8_t)random_next();
	}

	/* The first byte, mostly one of the declared report IDs. */
	numbered = drv_hid_input_report_ids(input);
	choice = random_next();
	if (length > 0U && (choice % 5U) != 0U && numbered) {
		choice = random_next();
		report[0] = old_side.reports[choice % old_side.report_count].id;
	}

	/* The report's length. */
	return length;
}

/* Reports the next random number (xorshift64*). */
static uint32_t
random_next(
	void)
{
	/* One step of the generator. */
	random_state ^= random_state >> 12;
	random_state ^= random_state << 25;
	random_state ^= random_state >> 27;
	return (uint32_t)((random_state * 2685821657736338717ULL) >> 32);
}

/* Copies bytes into a descriptor under test. */
static void
set_descriptor(
	struct descriptor *descriptor,
	const char *name,
	const uint8_t *bytes,
	size_t size)
{
	/* The name and the bytes. */
	descriptor->name = name;
	memcpy(descriptor->bytes, bytes, size);
	descriptor->size = size;
}

/*
 * Checks a boot keyboard: a key and another, both released, and an
 * ErrorRollOver that keeps them; its sizes and kind.
 */
static void
check_keyboard(
	void)
{
	static const uint8_t press_a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
	static const uint8_t press_ab[8] = { 0, 0, 0x04, 0x05, 0, 0, 0, 0 };
	static const uint8_t rollover[8] = { 0, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
	static const uint8_t release[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct hid_input *input;
	struct role_event events[16];
	size_t count;
	int error;

	/* The descriptor's sizes and kind. */
	error = drv_hid_input_prepare(boot_keyboard, sizeof(boot_keyboard), &input);
	check(error == 0, "keyboard: prepare");
	if (error != 0)
		return;
	check(drv_hid_input_report_max(input) == 8U, "keyboard: report_max 8");
	check(!drv_hid_input_report_ids(input), "keyboard: no report IDs");
	check(drv_hid_input_kind(input) == HID_INPUT_KIND_KEYBOARD, "keyboard: kind");
	error = publish_new(input);
	check(error == 0, "keyboard: publish");

	/* A: KEY_A down and a frame. */
	events_of(input, press_a, sizeof(press_a), events, &count);
	check(count == 2U && events[0].code == KEY_A && events[0].value == 1 && events[1].type == EV_SYN, "keyboard: A down");

	/* A and B: only B is new. */
	events_of(input, press_ab, sizeof(press_ab), events, &count);
	check(count == 2U && events[0].code == KEY_B && events[0].value == 1, "keyboard: B down");

	/* ErrorRollOver: the keys stay as they were, nothing is told. */
	events_of(input, rollover, sizeof(rollover), events, &count);
	check(count == 0U, "keyboard: ErrorRollOver keeps the keys");

	/* Nothing held: A and B up. */
	events_of(input, release, sizeof(release), events, &count);
	check(count == 3U && events[0].code == KEY_A && events[0].value == 0 && events[1].code == KEY_B && events[1].value == 0, "keyboard: A and B up");

	/* A report of the wrong length is refused and counted. */
	events_of(input, release, 3U, events, &count);
	check(count == 0U && drv_hid_input_malformed(input) == 1U, "keyboard: a short report is malformed");

	/* Unpublishing twice frees one device once. */
	drv_hid_input_unpublish(input);
	drv_hid_input_unpublish(input);
	drv_hid_input_destroy(input);
	printf("ok: keyboard rules\n");
}

/* Checks that a key one report ID holds is not released by another report ID. */
static void
check_two_ids(
	void)
{
	static const uint8_t first_a[3] = { 1, 0x04, 0 };
	static const uint8_t second_b[3] = { 2, 0x05, 0 };
	static const uint8_t first_none[3] = { 1, 0, 0 };
	static const uint8_t unknown_id[3] = { 7, 0x06, 0 };
	struct hid_input *input;
	struct role_event events[16];
	size_t count;
	int error;

	/* The descriptor: two reports with their ID's byte. */
	error = drv_hid_input_prepare(two_id_keyboard, sizeof(two_id_keyboard), &input);
	check(error == 0, "two IDs: prepare");
	if (error != 0)
		return;
	check(drv_hid_input_report_ids(input), "two IDs: report IDs");
	check(drv_hid_input_report_max(input) == 3U, "two IDs: report_max 3");
	error = publish_new(input);
	check(error == 0, "two IDs: publish");

	/* ID 1 holds A, ID 2 holds B. */
	events_of(input, first_a, sizeof(first_a), events, &count);
	check(count == 2U && events[0].code == KEY_A && events[0].value == 1, "two IDs: A down");
	events_of(input, second_b, sizeof(second_b), events, &count);
	check(count == 2U && events[0].code == KEY_B && events[0].value == 1, "two IDs: B down");

	/* ID 1 empty releases A only. */
	events_of(input, first_none, sizeof(first_none), events, &count);
	check(count == 2U && events[0].code == KEY_A && events[0].value == 0, "two IDs: A up, B kept");

	/* An undeclared ID tells nothing. */
	events_of(input, unknown_id, sizeof(unknown_id), events, &count);
	check(count == 0U, "two IDs: an unknown ID tells nothing");

	/* The devices go. */
	drv_hid_input_unpublish(input);
	drv_hid_input_destroy(input);
	printf("ok: two report IDs\n");
}

/* Checks a mouse: a relative axis that did not move tells nothing. */
static void
check_mouse(
	void)
{
	static const uint8_t move_x[4] = { 0, 5, 0, 0 };
	static const uint8_t still[4] = { 0, 0, 0, 0 };
	struct hid_input *input;
	struct role_event events[16];
	size_t count;
	int error;

	/* The descriptor and its kind. */
	error = drv_hid_input_prepare(wheel_mouse, sizeof(wheel_mouse), &input);
	check(error == 0, "mouse: prepare");
	if (error != 0)
		return;
	check(drv_hid_input_kind(input) == HID_INPUT_KIND_MOUSE, "mouse: kind");
	error = publish_new(input);
	check(error == 0, "mouse: publish");

	/* X 5: REL_X 5 and a frame, no REL_Y nor wheel. */
	events_of(input, move_x, sizeof(move_x), events, &count);
	check(count == 2U && events[0].type == EV_REL && events[0].code == REL_X && events[0].value == 5, "mouse: REL_X 5 alone");

	/* Nothing moved: nothing told, not even a frame. */
	events_of(input, still, sizeof(still), events, &count);
	check(count == 0U, "mouse: no relative 0");

	/* The devices go. */
	drv_hid_input_unpublish(input);
	drv_hid_input_destroy(input);
	printf("ok: mouse rules\n");
}

/* Checks a touch screen and nothing else: no main device, the touch device of its own. */
static void
check_touch_alone(
	void)
{
	struct hid_input *input;
	int event;
	int touch_event;
	int error;

	/* The descriptor, published. */
	error = drv_hid_input_prepare(touch_screen, sizeof(touch_screen), &input);
	check(error == 0, "touch screen: prepare");
	if (error != 0)
		return;
	error = publish_new(input);
	check(error == 0, "touch screen: publish");

	/* Only the touch device. */
	drv_hid_input_numbers(input, &event, &touch_event);
	check(event == -1 && touch_event >= 0, "touch screen: the touch device alone");
	check(touch_event >= 0 && fake_devices[touch_event].properties != 0U, "touch screen: its properties");

	/* The devices go. */
	drv_hid_input_unpublish(input);
	drv_hid_input_numbers(input, &event, &touch_event);
	check(event == -1 && touch_event == -1, "touch screen: unpublished");
	drv_hid_input_destroy(input);
	printf("ok: touch screen alone\n");
}

/* Checks a descriptor that declares no evdev device: ENODEV, nothing left registered. */
static void
check_vendor_only(
	void)
{
	struct hid_input *input;
	unsigned slot;
	int error;
	int left;

	/* The descriptor. */
	error = drv_hid_input_prepare(vendor_only, sizeof(vendor_only), &input);
	if (error != 0) {
		printf("ok: vendor-only descriptor refused by prepare (%d)\n", error);
		return;
	}

	/* Publication finds nothing to publish. */
	error = publish_new(input);
	check(error == old_hid_enodev(), "vendor only: ENODEV");
	left = 0;
	for (slot = 0; slot < FAKE_DEVICES_MAX; slot++) {
		/* Any device still registered is a leak. */
		if (fake_devices[slot].used)
			left = 1;
	}

	/* Nothing may stay registered. */
	check(!left, "vendor only: nothing registered");
	drv_hid_input_destroy(input);
	printf("ok: vendor-only descriptor publishes nothing\n");
}

/*
 * Checks a full input layer: the touch device of a two-device descriptor
 * does not fit, the main device is taken back, and the publication can be
 * tried again once there is room.
 */
static void
check_full_layer(
	void)
{
	struct hid_input *input;
	const struct descriptor *pad;
	size_t index;
	unsigned slot;
	int event;
	int touch_event;
	int error;
	int left;
	int found;

	/* The 5330's touchpad (a main device and a touch device) from the command line. */
	pad = NULL;
	for (index = 0; index < file_descriptor_count; index++) {
		found = strstr(file_descriptors[index].name, "synaptics") != NULL;
		if (found)
			pad = &file_descriptors[index];
	}

	/* Without it the check is skipped. */
	if (pad == NULL) {
		printf("skip: full layer (no touchpad descriptor given)\n");
		return;
	}

	/* Its description. */
	error = drv_hid_input_prepare(pad->bytes, pad->size, &input);
	check(error == 0, "full layer: prepare");
	if (error != 0)
		return;

	/* Room for one device: the second gets ENOSPC, and the first is taken back. */
	fake_registrations_left = 1U;
	error = publish_new(input);
	fake_registrations_left = 1000000U;
	check(error == ENOSPC, "full layer: ENOSPC");
	drv_hid_input_numbers(input, &event, &touch_event);
	check(event == -1 && touch_event == -1, "full layer: nothing kept");
	left = 0;
	for (slot = 0; slot < FAKE_DEVICES_MAX; slot++) {
		/* Any device still registered is a leak. */
		if (fake_devices[slot].used)
			left = 1;
	}

	/* Nothing may stay registered. */
	check(!left, "full layer: nothing registered");

	/* Room again: the same description publishes. */
	error = publish_new(input);
	check(error == 0, "full layer: published on the second try");
	drv_hid_input_numbers(input, &event, &touch_event);
	check(event >= 0 && touch_event >= 0, "full layer: both devices");
	drv_hid_input_unpublish(input);
	drv_hid_input_destroy(input);
	printf("ok: full input layer\n");
}

/* Mutates a copy of a base descriptor: bytes replaced, inserted, deleted, or cut. */
static void
fuzz_mutate(
	struct descriptor *descriptor,
	const struct descriptor *base)
{
	unsigned mutations;
	unsigned number;
	size_t place;
	uint32_t kind;

	/* The base. */
	set_descriptor(descriptor, "mutated", base->bytes, base->size);

	/* One to eight mutations. */
	mutations = 1U + random_next() % 8U;
	for (number = 0; number < mutations; number++) {
		kind = random_next() % 4U;
		place = 0;
		if (descriptor->size > 0U)
			place = random_next() % descriptor->size;

		/* Each kind of mutation. */
		switch (kind) {
		case 0:
			/* A byte replaced. */
			if (descriptor->size > 0U)
				descriptor->bytes[place] = (uint8_t)random_next();
			break;
		case 1:
			/* A byte inserted. */
			if (descriptor->size < DESCRIPTOR_BYTES_MAX) {
				memmove(descriptor->bytes + place + 1, descriptor->bytes + place, descriptor->size - place);
				descriptor->bytes[place] = (uint8_t)random_next();
				descriptor->size++;
			}

			break;
		case 2:
			/* A byte deleted. */
			if (descriptor->size > 0U) {
				memmove(descriptor->bytes + place, descriptor->bytes + place + 1, descriptor->size - place - 1);
				descriptor->size--;
			}

			break;
		default:
			/* The rest cut off. */
			descriptor->size = place;
			break;
		}
	}
}

/*
 * Makes a descriptor of random items in their proper form: short items of
 * the main, global and local kinds with sizes 0, 1, 2 and 4, values near
 * the parser's limits, sometimes a long item, sometimes more than the
 * parser takes.
 */
static void
fuzz_generate(
	struct descriptor *descriptor)
{
	size_t target;
	uint32_t choice;

	/* Up to about 300 bytes, sometimes past the parser's 4096. */
	descriptor->name = "generated";
	descriptor->size = 0;
	target = 8U + random_next() % 300U;
	choice = random_next();
	if ((choice % 50U) == 0U)
		target = 4000U + random_next() % 200U;

	/* Items until the size is reached. */
	while (descriptor->size + 6U < target)
		fuzz_item(descriptor);
}

/* Appends one random item to a generated descriptor. */
static void
fuzz_item(
	struct descriptor *descriptor)
{
	static const uint8_t tags[] = {
		0x80, 0x90, 0xb0, 0xa0, 0xc0,
		0x04, 0x14, 0x24, 0x34, 0x44, 0x54, 0x64, 0x74, 0x84, 0x94, 0xa4, 0xb4,
		0x08, 0x18, 0x28
	};
	static const uint32_t values[] = {
		0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0c, 0x0d, 0x10, 0x20,
		0x22, 0x30, 0x31, 0x32, 0x38, 0x42, 0x47, 0x51, 0x54, 0x56, 0x65, 0x7f,
		0x80, 0xff, 0x100, 0xff00, 0xffff, 0x10000, 0x7fffffff, 0xffffffff
	};
	static const uint8_t sizes[] = { 0, 1, 2, 4 };
	uint8_t size;
	uint8_t code;
	uint8_t tag;
	uint32_t value;
	uint32_t choice;
	unsigned index;

	/* A long item now and then. */
	choice = random_next();
	if ((choice % 64U) == 0U) {
		descriptor->bytes[descriptor->size] = 0xfe;
		descriptor->bytes[descriptor->size + 1] = (uint8_t)(random_next() % 3U);
		descriptor->bytes[descriptor->size + 2] = (uint8_t)random_next();
		descriptor->size += 3U;
		return;
	}

	/* A tag, a size and a value. */
	tag = tags[random_next() % sizeof(tags)];
	size = sizes[random_next() % sizeof(sizes)];
	value = values[random_next() % (sizeof(values) / sizeof(values[0]))];
	choice = random_next();
	if ((choice % 8U) == 0U)
		value = random_next();

	/* The prefix byte: the tag and the size's code (3 stands for 4 bytes). */
	code = size;
	if (size == 4U)
		code = 3U;
	descriptor->bytes[descriptor->size] = (uint8_t)(tag | code);
	descriptor->size++;

	/* The value's bytes, least significant first. */
	for (index = 0; index < size; index++) {
		descriptor->bytes[descriptor->size] = (uint8_t)(value >> (8U * index));
		descriptor->size++;
	}
}

/* Publishes the new side under the test's identity. */
static int
publish_new(
	struct hid_input *input)
{
	struct hid_input_identity identity;
	int error;

	/* The identity both sides are given. */
	memset(&identity, 0, sizeof(identity));
	identity.name = "Test Device";
	identity.physical_path = "usb1/port2/device3/interface0";
	identity.unique_id = "SERIAL";
	identity.touch_name = "Test Device Touchscreen";
	identity.touch_physical_path = "usb1/port2/device3/interface0/touch";
	identity.id.bustype = BUS_USB;
	identity.id.vendor = 0x1234;
	identity.id.product = 0x5678;
	identity.id.version = 0x0100;

	/* The glue's publication. */
	error = drv_hid_input_publish(input, &identity);
	return error;
}

/* Publishes the old side under the same identity. */
static int
publish_old(
	void)
{
	struct input_id id;
	int error;

	/* The identity the new side is given. */
	memset(&id, 0, sizeof(id));
	id.bustype = BUS_USB;
	id.vendor = 0x1234;
	id.product = 0x5678;
	id.version = 0x0100;

	/* The old driver's registration. */
	error = old_hid_publish(&old_side,
				"Test Device",
				"usb1/port2/device3/interface0",
				"SERIAL",
				"Test Device Touchscreen",
				"usb1/port2/device3/interface0/touch",
				&id);
	return error;
}

/* Hands one report to the new side and gives back the events it emitted (at most 16). */
static void
events_of(
	struct hid_input *input,
	const uint8_t *report,
	size_t length,
	struct role_event *events,
	size_t *count)
{
	size_t taken;

	/* The report, then its events. */
	log_reset();
	drv_hid_input_report(input, report, length, 42U);
	taken = take_new_events(input);
	if (taken > 16U)
		taken = 16U;
	memcpy(events, new_events, taken * sizeof(events[0]));
	*count = taken;
}

/* Checks which reports /dev/hid-host refuses as shorter than their report ID declares. */
static void
check_short_reports(
	void)
{
	static const uint8_t report[8] = { 1, 0x04, 0, 0, 0, 0, 0, 0 };
	struct hid_input *input;
	int error;

	/* A boot keyboard: 8 bytes, no report ID. */
	error = drv_hid_input_prepare(boot_keyboard, sizeof(boot_keyboard), &input);
	check(error == 0, "short: keyboard prepare");
	if (error != 0)
		return;
	check(drv_hid_input_report_short(input, report, 7U) == 1, "short: 7 bytes of 8");
	check(drv_hid_input_report_short(input, report, 8U) == 0, "short: 8 bytes");
	check(drv_hid_input_report_short(input, report, 9U) == 0, "short: 9 bytes (the rest unread)");
	drv_hid_input_destroy(input);

	/* Two report IDs of 3 bytes each, the ID first. */
	error = drv_hid_input_prepare(two_id_keyboard, sizeof(two_id_keyboard), &input);
	check(error == 0, "short: two IDs prepare");
	if (error != 0)
		return;
	check(drv_hid_input_report_short(input, report, 0U) == 1, "short: no ID byte");
	check(drv_hid_input_report_short(input, report, 2U) == 1, "short: 2 bytes of 3");
	check(drv_hid_input_report_short(input, report, 3U) == 0, "short: 3 bytes");
	check(drv_hid_input_report_short(input, (const uint8_t *)"\x07\x00", 2U) == 0, "short: an undeclared ID is the layout's");
	drv_hid_input_destroy(input);
	printf("ok: short reports\n");
}

/* Checks the setup of /dev/hid-host: a good one, and each way one is malformed. */
static void
check_setup(
	void)
{
	static struct hid_host_setup good;
	static struct hid_host_setup bad;

	/* A well formed setup: a Bluetooth keyboard. */
	memset(&good, 0, sizeof(good));
	good.magic = HID_HOST_MAGIC;
	good.version = HID_HOST_VERSION;
	good.bus = BUS_BLUETOOTH;
	good.vendor = 0x1234;
	good.product = 0x5678;
	good.descriptor_size = sizeof(boot_keyboard);
	snprintf(good.name, sizeof(good.name), "Probe Keyboard");
	snprintf(good.physical_path, sizeof(good.physical_path), "bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:01");
	snprintf(good.unique_id, sizeof(good.unique_id), "0A:0B:0C:0D:0E:01");
	memcpy(good.descriptor, boot_keyboard, sizeof(boot_keyboard));
	check(sizeof(good) == 4324U, "setup: 4324 bytes");
	check(drv_hid_host_setup_valid(&good) == 1, "setup: a good one");

	/* A virtual device is allowed too. */
	bad = good;
	bad.bus = BUS_VIRTUAL;
	check(drv_hid_host_setup_valid(&bad) == 1, "setup: a virtual device");

	/* Each malformed field. */
	bad = good;
	bad.magic ^= 1U;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: the magic");
	bad = good;
	bad.version = 2U;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: the version");
	bad = good;
	bad.bus = BUS_USB;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a program may not claim USB");
	bad = good;
	bad.descriptor_size = 0U;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: an empty descriptor");
	bad = good;
	bad.descriptor_size = HID_HOST_DESCRIPTOR_MAX + 1U;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a descriptor of 4097");
	bad = good;
	bad.descriptor_size = HID_HOST_DESCRIPTOR_MAX;
	check(drv_hid_host_setup_valid(&bad) == 1, "setup: a descriptor of 4096");
	bad = good;
	bad.reserved[3] = 1U;
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a reserved word");
	bad = good;
	memset(bad.name, 'x', sizeof(bad.name));
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a name without its NUL");
	bad = good;
	memset(bad.physical_path, 'x', sizeof(bad.physical_path));
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a place without its NUL");
	bad = good;
	memset(bad.unique_id, 'x', sizeof(bad.unique_id));
	check(drv_hid_host_setup_valid(&bad) == 0, "setup: a unique ID without its NUL");
	bad = good;
	memset(bad.name, 'x', sizeof(bad.name) - 1U);
	bad.name[sizeof(bad.name) - 1U] = '\0';
	check(drv_hid_host_setup_valid(&bad) == 1, "setup: a name of 63 bytes");
	printf("ok: hid-host setup\n");
}
