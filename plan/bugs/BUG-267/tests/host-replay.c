/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of BUG-267: the USB touch screen of the monitor on the
 * Latitude 5330's USB-C (interface 0: ten fingers, five a report, with a
 * Contact Count; interface 1: a pen and a mouse of absolute X and Y).  The
 * descriptors are the ones the kernel dumped on the 5330 (usb-hid-rdesc,
 * 2026-10-08).  It checks that the parser takes both interfaces, that the
 * reports recorded on the 5330 (usb-hid-report) open one finger, and that
 * frames of two and three fingers open a slot each (the multitouch
 * protocol B the compositor reads).  The kernel's files are compiled
 * freestanding; this test supplies the allocator and memory functions.
 * usage: host-replay IF0.rdesc IF1.rdesc KERNEL.log
 */

#include <drivers/generic/hid-digitizer.h>
#include <drivers/generic/hid-touch.h>
#include <uapi/input.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest descriptor and report read. */
#define DESCRIPTOR_MAX	4096U
#define REPORT_MAX	64U

/* Interface 0's touch report: its ID and length, and where its fingers are (five of seven bytes each). */
#define TOUCH_ID	0x0cU
#define TOUCH_LENGTH	40U
#define TOUCH_COUNT_AT	2U
#define TOUCH_FINGER_AT	3U
#define TOUCH_FINGER	7U
#define TOUCH_FINGERS	5U

/* The checks that failed, and those that ran. */
static int failures;
static int checks;

void *kern_calloc(size_t count, size_t size);
void kern_free(void *pointer);
void *kern_memcpy(void *destination, const void *source, size_t length);
void *kern_memset(void *destination, int value, size_t length);

static void check(int condition, const char *what);
static size_t read_file(const char *path, uint8_t *bytes, size_t size);
static int count_events(const struct hid_touch_output *output, uint16_t code, int opened);
static void put_finger(uint8_t *report, unsigned index, unsigned id, unsigned x, unsigned y);
static void feed(struct hid_report_layout *layout, struct hid_touch_state *state, const uint8_t *report, size_t length,
	struct hid_touch_output *output);
static void test_parse(const char *if0, const char *if1);
static void test_recorded(struct hid_report_layout *layout, const char *log);
static void test_fingers(struct hid_report_layout *layout);

/* Allocates a zeroed object for the parser, as the kernel heap does. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	/* The host's heap. */
	return calloc(count, size);
}

/* Frees an object of the parser. */
void
kern_free(
	void *pointer)
{
	/* The host's heap. */
	free(pointer);
}

/* Copies bytes for the parser. */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t length)
{
	/* The host's copy. */
	return memcpy(destination, source, length);
}

/* Fills bytes for the parser. */
void *
kern_memset(
	void *destination,
	int value,
	size_t length)
{
	/* The host's fill. */
	return memset(destination, value, length);
}

/* Runs the checks. */
int
main(
	int argc,
	char **argv)
{
	static uint8_t descriptor[DESCRIPTOR_MAX];
	struct hid_report_layout *layout;
	size_t length;
	int error;

	/* The files. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-replay IF0.rdesc IF1.rdesc KERNEL.log\n");
		return 2;
	}

	/* Both interfaces parse. */
	test_parse(argv[1], argv[2]);

	/* Interface 0's layout, for its reports. */
	length = read_file(argv[1], descriptor, sizeof(descriptor));
	error = drv_hid_report_layout_parse(descriptor, length, &layout);
	check(error == 0, "interface 0 parses");
	if (error != 0)
		return 1;
	test_recorded(layout, argv[3]);
	test_fingers(layout);
	drv_hid_report_layout_destroy(layout);

	/* The result. */
	if (failures != 0) {
		fprintf(stderr, "host-replay: %d of %d checks FAILED\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-replay: ok (%d checks)\n", checks);
	return 0;
}

/* Counts a check, and says one that does not hold. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition) {
		printf("ok: %s\n", what);
		return;
	}

	/* A failure. */
	failures++;
	fprintf(stderr, "FAIL: %s\n", what);
}

/* Reads a whole file (none is read past the buffer).  Returns its length, 0 when it cannot be read. */
static size_t
read_file(
	const char *path,
	uint8_t *bytes,
	size_t size)
{
	FILE *file;
	size_t length;

	/* The file. */
	file = fopen(path, "rb");
	if (file == NULL)
		return 0;
	length = fread(bytes, 1, size, file);
	fclose(file);

	/* Its length. */
	return length;
}

/* Counts the tracking IDs an output opens (opened) or closes. */
static int
count_events(
	const struct hid_touch_output *output,
	uint16_t code,
	int opened)
{
	size_t index;
	int count;

	/* Each event of the code, opening (0 or more) or closing (-1). */
	count = 0;
	for (index = 0; index < output->event_count; index++) {
		if (output->events[index].type != EV_ABS || output->events[index].code != code)
			continue;
		if ((output->events[index].value >= 0) == (opened != 0))
			count++;
	}

	/* The count. */
	return count;
}

/* Writes one finger of a touch report: tip and confidence, its identifier, X and Y. */
static void
put_finger(
	uint8_t *report,
	unsigned index,
	unsigned id,
	unsigned x,
	unsigned y)
{
	uint8_t *finger;

	/* The finger's seven bytes. */
	finger = &report[TOUCH_FINGER_AT + index * TOUCH_FINGER];
	finger[0] = 0x05U;
	finger[1] = (uint8_t)id;
	finger[2] = (uint8_t)(id >> 8);
	finger[3] = (uint8_t)x;
	finger[4] = (uint8_t)(x >> 8);
	finger[5] = (uint8_t)y;
	finger[6] = (uint8_t)(y >> 8);
}

/* Decodes one report and runs it through the touch state machine. */
static void
feed(
	struct hid_report_layout *layout,
	struct hid_touch_state *state,
	const uint8_t *report,
	size_t length,
	struct hid_touch_output *output)
{
	struct hid_report_input decoded;
	int error;
	int touch;

	/* The report decoded, a touch report, translated. */
	memset(output, 0, sizeof(*output));
	error = drv_hid_report_decode(layout, report, length, &decoded);
	if (error != 0) {
		fprintf(stderr, "decode error %d (length %zu, count %u)\n", error, length, report[TOUCH_COUNT_AT]);
		check(0, "a touch report decodes");
		return;
	}
	touch = drv_hid_touch_report_is_touch(&decoded);
	if (!touch) {
		check(0, "a touch report is a touch report");
		return;
	}
	error = drv_hid_touch_translate(state, &decoded, output);
	if (error != 0)
		check(0, "a touch report translates");
}

/* Both interfaces' descriptors parse (interface 1 was refused at its mouse's X and Y, BUG-267). */
static void
test_parse(
	const char *if0,
	const char *if1)
{
	static uint8_t descriptor[DESCRIPTOR_MAX];
	size_t length;
	size_t item;
	int error;

	/* Interface 0. */
	length = read_file(if0, descriptor, sizeof(descriptor));
	error = drv_hid_report_layout_diagnose(descriptor, length, &item);
	printf("interface 0: error=%d item=%#zx length=%zu\n", error, item, length);
	check(length == 522U && error == 0, "interface 0 (522 bytes) is taken");

	/* Interface 1. */
	length = read_file(if1, descriptor, sizeof(descriptor));
	error = drv_hid_report_layout_diagnose(descriptor, length, &item);
	printf("interface 1: error=%d item=%#zx length=%zu\n", error, item, length);
	check(length == 442U && error == 0, "interface 1 (442 bytes, a pen and an absolute mouse) is taken");
}

/* The reports the 5330 recorded (one finger): one tracking ID opens, and closes when the finger lifts. */
static void
test_recorded(
	struct hid_report_layout *layout,
	const char *log)
{
	struct hid_report_touch_info touch;
	struct hid_touch_description description;
	struct hid_touch_state state;
	struct hid_touch_output output;
	uint8_t report[REPORT_MAX];
	char line[512];
	const char *header;
	unsigned values[32];
	FILE *file;
	size_t used;
	int fields;
	int reports;
	int opened;
	int closed;
	int error;
	int index;

	/* The touch screen's description: ten slots. */
	error = drv_hid_report_layout_get_touch(layout, &touch);
	check(error == 0 && touch.contacts == TOUCH_FINGERS && touch.count_present, "interface 0: five fingers a report, counted");
	error = drv_hid_touch_describe(&touch, &description);
	check(error == 0 && description.slots == 10U, "interface 0: ten slots");
	drv_hid_touch_reset(&state, description.slots);

	/* Each recorded report (a length line, then lines of 32 bytes), fed whole. */
	file = fopen(log, "r");
	check(file != NULL, "the 5330's kernel log is read");
	if (file == NULL)
		return;
	reports = 0;
	opened = 0;
	closed = 0;
	used = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "usb-hid-report: usb1 device 5 interface 0 ", 42) != 0)
			continue;
		header = strstr(line, "length=");
		if (header != NULL) {
			used = 0;
			continue;
		}

		/* A line of bytes, added to the report; a whole report is fed. */
		fields = sscanf(line + 42, "%*x: %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
			&values[0], &values[1], &values[2], &values[3], &values[4], &values[5], &values[6], &values[7],
			&values[8], &values[9], &values[10], &values[11], &values[12], &values[13], &values[14], &values[15],
			&values[16], &values[17], &values[18], &values[19], &values[20], &values[21], &values[22], &values[23],
			&values[24], &values[25], &values[26], &values[27], &values[28], &values[29], &values[30], &values[31]);
		for (index = 0; index < fields && used < sizeof(report); index++)
			report[used++] = (uint8_t)values[index];
		if (used != TOUCH_LENGTH)
			continue;
		feed(layout, &state, report, used, &output);
		opened += count_events(&output, ABS_MT_TRACKING_ID, 1);
		closed += count_events(&output, ABS_MT_TRACKING_ID, 0);
		reports++;
		used = 0;
	}

	/* The counts. */
	fclose(file);
	printf("recorded: %d reports, %d tracking IDs opened, %d closed\n", reports, opened, closed);
	check(reports == 300, "the 300 recorded reports are read");
	check(opened >= 1 && opened - closed <= 1, "the recorded single finger opens and lifts (the last touch may still be down at the end of the record)");
}

/* Frames of two and three fingers: a slot each, a slot freed when its finger goes. */
static void
test_fingers(
	struct hid_report_layout *layout)
{
	struct hid_report_touch_info touch;
	struct hid_touch_description description;
	struct hid_touch_state state;
	struct hid_touch_output output;
	uint8_t report[TOUCH_LENGTH];
	int slots;

	/* A fresh state. */
	(void)drv_hid_report_layout_get_touch(layout, &touch);
	(void)drv_hid_touch_describe(&touch, &description);
	drv_hid_touch_reset(&state, description.slots);

	/* Two fingers in one report (the device's identifiers 1 and 2). */
	memset(report, 0, sizeof(report));
	report[0] = TOUCH_ID;
	report[TOUCH_COUNT_AT] = 2;
	put_finger(report, 0, 1, 3000, 2000);
	put_finger(report, 1, 2, 6000, 4000);
	feed(layout, &state, report, sizeof(report), &output);
	slots = count_events(&output, ABS_MT_TRACKING_ID, 1);
	check(slots == 2, "two fingers in one report open two slots");

	/* Both move. */
	put_finger(report, 0, 1, 3100, 2000);
	put_finger(report, 1, 2, 6100, 4000);
	feed(layout, &state, report, sizeof(report), &output);
	check(count_events(&output, ABS_MT_POSITION_X, 1) == 2, "both fingers move");

	/* A third finger. */
	report[TOUCH_COUNT_AT] = 3;
	put_finger(report, 2, 3, 9000, 6000);
	feed(layout, &state, report, sizeof(report), &output);
	check(count_events(&output, ABS_MT_TRACKING_ID, 1) == 1, "a third finger opens a third slot");

	/* The first finger lifts (the device leaves it out of the count). */
	memset(report, 0, sizeof(report));
	report[0] = TOUCH_ID;
	report[TOUCH_COUNT_AT] = 2;
	put_finger(report, 0, 2, 6100, 4000);
	put_finger(report, 1, 3, 9000, 6000);
	feed(layout, &state, report, sizeof(report), &output);
	check(count_events(&output, ABS_MT_TRACKING_ID, 0) == 1, "the finger left out lifts");

	/* All lift (a count of 0). */
	memset(report, 0, sizeof(report));
	report[0] = TOUCH_ID;
	feed(layout, &state, report, sizeof(report), &output);
	check(count_events(&output, ABS_MT_TRACKING_ID, 0) == 2, "the last two lift");
}
