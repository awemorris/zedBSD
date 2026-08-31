/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "drivers/hid/hid-report.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
static size_t allocation_attempts;
static size_t allocation_fail_at;
static size_t live_allocations;

static void
fail(unsigned line, const char *expression)
{
	fprintf(stderr, "hid-report-test:%u: check failed: %s\n", line,
	    expression);
	exit(1);
}

#define CHECK(expression)                                                     \
	do {                                                                    \
		checks++;                                                       \
		if (!(expression))                                              \
			fail(__LINE__, #expression);                              \
	} while (0)

void *
kern_calloc(size_t count, size_t size)
{
	void *result;

	allocation_attempts++;
	if (allocation_fail_at != 0U &&
	    allocation_attempts == allocation_fail_at)
		return NULL;
	if (count != 0U && size > SIZE_MAX / count)
		return NULL;
	result = calloc(count, size);
	if (result != NULL)
		live_allocations++;
	return result;
}

void
kern_free(void *pointer)
{
	if (pointer == NULL)
		return;
	CHECK(live_allocations != 0U);
	live_allocations--;
	free(pointer);
}

static void
allocator_reset(size_t fail_at)
{
	CHECK(live_allocations == 0U);
	allocation_attempts = 0;
	allocation_fail_at = fail_at;
}

static int
has_capability(const struct hid_report_layout *layout, uint16_t type,
	uint16_t code)
{
	struct hid_report_layout_info info;
	struct input_capability capability;
	size_t index;

	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	for (index = 0; index < info.capability_count; index++) {
		CHECK(hid_report_layout_get_capability(layout, index,
		    &capability) == 0);
		if (capability.type == type && capability.code == code)
			return 1;
	}
	return 0;
}

static int
find_value(const struct hid_report_input *input, uint16_t type, uint16_t code,
	int32_t *value)
{
	size_t index;

	for (index = 0; index < input->value_count; index++) {
		if (input->values[index].type != type ||
		    input->values[index].code != code)
			continue;
		if (value != NULL)
			*value = input->values[index].value;
		return 1;
	}
	return 0;
}

static void
expect_parse_error(const void *descriptor, size_t length, int expected)
{
	struct hid_report_layout *sentinel =
	    (struct hid_report_layout *)(uintptr_t)1U;
	size_t before = live_allocations;

	CHECK(hid_report_layout_parse(descriptor, length, &sentinel) == expected);
	CHECK(sentinel == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(live_allocations == before);
}

static void
expect_decode_error(const struct hid_report_layout *layout,
	const void *report, size_t length, int expected)
{
	struct hid_report_input output;
	struct hid_report_input original;

	memset(&output, 0xa5, sizeof(output));
	original = output;
	CHECK(hid_report_decode(layout, report, length, &output) == expected);
	CHECK(memcmp(&output, &original, sizeof(output)) == 0);
}

static const uint8_t keyboard_descriptor[] = {
	0x05, 0x01,       /* Usage Page (Generic Desktop). */
	0x09, 0x06,       /* Usage (Keyboard). */
	0xa1, 0x01,       /* Collection (Application). */
	0x05, 0x07,       /* Usage Page (Keyboard). */
	0x19, 0xe0,       /* Usage Minimum (Left Control). */
	0x29, 0xe7,       /* Usage Maximum (Right GUI). */
	0x15, 0x00,       /* Logical Minimum (0). */
	0x25, 0x01,       /* Logical Maximum (1). */
	0x75, 0x01,       /* Report Size (1). */
	0x95, 0x08,       /* Report Count (8). */
	0x81, 0x02,       /* Input (Data, Variable, Absolute). */
	0x95, 0x01,
	0x75, 0x08,
	0x81, 0x01,       /* Input (Constant): reserved byte. */
	0x05, 0x08,       /* Usage Page (LED). */
	0x19, 0x01,
	0x29, 0x05,
	0x95, 0x05,
	0x75, 0x01,
	0x91, 0x02,       /* Output fields must not move Input offsets. */
	0x95, 0x01,
	0x75, 0x03,
	0x91, 0x01,
	0x05, 0x07,
	0x19, 0x00,
	0x29, 0x65,
	0x15, 0x00,
	0x25, 0x65,
	0x95, 0x06,
	0x75, 0x08,
	0x81, 0x00,       /* Input (Data, Array, Absolute). */
	0xc0,
};

static const uint8_t narrow_keyboard_array_descriptor[] = {
	0x05, 0x01,       /* Usage Page (Generic Desktop). */
	0x09, 0x06,       /* Usage (Keyboard). */
	0xa1, 0x01,       /* Collection (Application). */
	0x05, 0x07,       /* Usage Page (Keyboard). */
	0x19, 0x04,       /* Usage Minimum (Keyboard A). */
	0x29, 0x1d,       /* Usage Maximum (Keyboard Z). */
	0x15, 0x04,       /* Logical Minimum (4). */
	0x25, 0x04,       /* Logical Maximum (4). */
	0x75, 0x08,
	0x95, 0x01,
	0x81, 0x00,       /* Input (Data, Array, Absolute). */
	0xc0,
};

static const uint8_t usage_range_then_explicit_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x15, 0x81,
	0x25, 0x7f,
	0x75, 0x08,
	0x95, 0x03,
	0x19, 0x30,       /* Usage range X..Y comes first. */
	0x29, 0x31,
	0x09, 0x38,       /* Explicit Wheel follows the range. */
	0x81, 0x06,
	0xc0,
};

static const uint8_t usage_explicit_then_range_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x15, 0x81,
	0x25, 0x7f,
	0x75, 0x08,
	0x95, 0x03,
	0x09, 0x38,       /* Explicit Wheel comes first. */
	0x19, 0x30,       /* Usage range X..Y follows it. */
	0x29, 0x31,
	0x81, 0x06,
	0xc0,
};

static const uint8_t mouse_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x09, 0x01,
	0xa1, 0x00,
	0x05, 0x09,
	0x19, 0x01,
	0x29, 0x03,
	0x15, 0x00,
	0x25, 0x01,
	0x95, 0x03,
	0x75, 0x01,
	0x81, 0x02,
	0x95, 0x01,
	0x75, 0x05,
	0x81, 0x01,
	0x05, 0x01,
	0x09, 0x30,
	0x09, 0x31,
	0x09, 0x38,
	0x15, 0x81,
	0x25, 0x7f,
	0x75, 0x08,
	0x95, 0x03,
	0x81, 0x06,
	0xc0,
	0xc0,
};

static const uint8_t tablet_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x05, 0x09,
	0x19, 0x01,
	0x29, 0x03,
	0x15, 0x00,
	0x25, 0x01,
	0x95, 0x03,
	0x75, 0x01,
	0x81, 0x02,
	0x95, 0x01,
	0x75, 0x05,
	0x81, 0x01,
	0x05, 0x01,
	0x09, 0x30,
	0x09, 0x31,
	0x16, 0x00, 0x00,
	0x26, 0xff, 0x7f,
	0x75, 0x10,
	0x95, 0x02,
	0x81, 0x02,
	0xc0,
};

static const uint8_t sparse_id_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x15, 0x81,
	0x25, 0x7f,
	0x75, 0x08,
	0x95, 0x01,
	0x85, 0x01,
	0x09, 0x30,
	0x81, 0x06,
	0x85, 0x07,
	0x09, 0x31,
	0x81, 0x06,
	0x85, 0x01,       /* Select the existing sparse identity again. */
	0x09, 0x38,
	0x81, 0x06,
	0x85, 0xff,
	0x05, 0x09,
	0x09, 0x01,
	0x15, 0x00,
	0x25, 0x01,
	0x75, 0x01,
	0x95, 0x01,
	0x81, 0x02,
	0x75, 0x07,
	0x95, 0x01,
	0x81, 0x01,
	0xc0,
};

static const uint8_t cross_byte_descriptor[] = {
	0x05, 0x01,
	0x09, 0x02,
	0xa1, 0x01,
	0x75, 0x03,
	0x95, 0x01,
	0x81, 0x01,       /* Three constant leading bits. */
	0x09, 0x30,
	0x16, 0x00, 0xf8, /* -2048. */
	0x26, 0xff, 0x07, /*  2047. */
	0x75, 0x0c,
	0x95, 0x01,
	0x81, 0x06,
	0xc0,
};

static void
test_keyboard(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	struct hid_report_report_info report_info;
	struct hid_report_input input;
	uint8_t report[8] = {0x02, 0, 0x04, 0x04, 0x05, 0, 0, 0};
	int32_t value;

	CHECK(hid_report_layout_parse(keyboard_descriptor,
	    sizeof(keyboard_descriptor), &layout) == 0);
	CHECK(layout != NULL);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.descriptor_size == sizeof(keyboard_descriptor));
	CHECK(info.report_count == 1U);
	CHECK(info.field_count == 12U);
	CHECK(info.uses_report_ids == 0);
	CHECK(hid_report_layout_get_report(layout, 0, &report_info) == 0);
	CHECK(report_info.report_id == 0U);
	CHECK(report_info.minimum_size == 8U);
	CHECK(report_info.field_count == 12U);
	CHECK(has_capability(layout, EV_SYN, SYN_REPORT));
	CHECK(has_capability(layout, EV_KEY, KEY_A));
	CHECK(has_capability(layout, EV_KEY, KEY_B));
	CHECK(has_capability(layout, EV_KEY, KEY_LEFTSHIFT));
	CHECK(!has_capability(layout, EV_KEY, KEY_RESERVED));

	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(input.report_id == 0U);
	CHECK(input.keyboard_error == 0U);
	CHECK(input.value_count == 3U);
	CHECK(find_value(&input, EV_KEY, KEY_LEFTSHIFT, &value));
	CHECK(value == 1);
	CHECK(find_value(&input, EV_KEY, KEY_A, &value));
	CHECK(value == 1);
	CHECK(find_value(&input, EV_KEY, KEY_B, &value));
	CHECK(value == 1);

	/* Array duplicates are one held key, not repeated transitions. */
	report[0] = 0;
	report[2] = 0x04;
	report[3] = 0x04;
	report[4] = 0x04;
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(input.value_count == 1U);
	CHECK(find_value(&input, EV_KEY, KEY_A, NULL));

	/* An advertised but unmapped keypad usage is ignored safely. */
	report[2] = 0x65;
	report[3] = 0;
	report[4] = 0;
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(input.value_count == 0U);

	/*
	 * All three HID keyboard error usages invalidate the complete key
	 * snapshot.  A held modifier and an ordinary array key must not leak
	 * through, and the explicit flag prevents this from looking like a
	 * release-all snapshot to the eventual USB producer.
	 */
	for (report[2] = 0x01; report[2] <= 0x03; report[2]++) {
		report[0] = 0x02;
		report[3] = 0x04;
		CHECK(hid_report_decode(layout, report, sizeof(report), &input) ==
		    0);
		CHECK(input.keyboard_error == 1U);
		CHECK(input.value_count == 0U);
	}

	expect_decode_error(layout, report, sizeof(report) - 1U, EINVAL);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_keyboard_array_range_intersection(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	struct hid_report_input input;
	uint8_t report = 0x04;
	int32_t value;

	CHECK(hid_report_layout_parse(narrow_keyboard_array_descriptor,
	    sizeof(narrow_keyboard_array_descriptor), &layout) == 0);
	CHECK(layout != NULL);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.field_count == 1U);
	CHECK(info.capability_count == 2U); /* SYN_REPORT and KEY_A only. */
	CHECK(has_capability(layout, EV_KEY, KEY_A));
	CHECK(!has_capability(layout, EV_KEY, KEY_B));
	CHECK(!has_capability(layout, EV_KEY, KEY_Z));

	CHECK(hid_report_decode(layout, &report, sizeof(report), &input) == 0);
	CHECK(input.keyboard_error == 0U);
	CHECK(input.value_count == 1U);
	CHECK(find_value(&input, EV_KEY, KEY_A, &value));
	CHECK(value == 1);

	/* Values outside the Logical intersection are not decodable usages. */
	report = 0x05;
	expect_decode_error(layout, &report, sizeof(report), EINVAL);
	report = 0x01;
	expect_decode_error(layout, &report, sizeof(report), EINVAL);

	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_mixed_local_usage_order(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_input input;
	uint8_t report[] = {1, 2, 3};
	int32_t value;

	CHECK(hid_report_layout_parse(usage_range_then_explicit_descriptor,
	    sizeof(usage_range_then_explicit_descriptor), &layout) == 0);
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value));
	CHECK(value == 1);
	CHECK(find_value(&input, EV_REL, REL_Y, &value));
	CHECK(value == 2);
	CHECK(find_value(&input, EV_REL, REL_WHEEL, &value));
	CHECK(value == 3);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);

	CHECK(hid_report_layout_parse(usage_explicit_then_range_descriptor,
	    sizeof(usage_explicit_then_range_descriptor), &layout) == 0);
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_WHEEL, &value));
	CHECK(value == 1);
	CHECK(find_value(&input, EV_REL, REL_X, &value));
	CHECK(value == 2);
	CHECK(find_value(&input, EV_REL, REL_Y, &value));
	CHECK(value == 3);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_mouse(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_report_info info;
	struct hid_report_input input;
	uint8_t report[] = {0x05, 0x7f, 0x81, 0xff};
	int32_t value;

	CHECK(hid_report_layout_parse(mouse_descriptor, sizeof(mouse_descriptor),
	    &layout) == 0);
	CHECK(hid_report_layout_get_report(layout, 0, &info) == 0);
	CHECK(info.minimum_size == sizeof(report));
	CHECK(has_capability(layout, EV_KEY, BTN_LEFT));
	CHECK(has_capability(layout, EV_KEY, BTN_MIDDLE));
	CHECK(has_capability(layout, EV_REL, REL_X));
	CHECK(has_capability(layout, EV_REL, REL_Y));
	CHECK(has_capability(layout, EV_REL, REL_WHEEL));
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(input.value_count == 5U);
	CHECK(find_value(&input, EV_KEY, BTN_LEFT, &value) && value == 1);
	CHECK(find_value(&input, EV_KEY, BTN_MIDDLE, &value) && value == 1);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == 127);
	CHECK(find_value(&input, EV_REL, REL_Y, &value) && value == -127);
	CHECK(find_value(&input, EV_REL, REL_WHEEL, &value) && value == -1);

	/* -128 is outside this descriptor's declared -127..127 range. */
	report[1] = 0x80;
	expect_decode_error(layout, report, sizeof(report), EINVAL);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_tablet(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	struct input_abs_axis axis;
	struct hid_report_input input;
	uint8_t report[] = {0x01, 0x34, 0x12, 0xff, 0x7f};
	int32_t value;

	CHECK(hid_report_layout_parse(tablet_descriptor,
	    sizeof(tablet_descriptor), &layout) == 0);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.absolute_axis_count == 2U);
	CHECK(hid_report_layout_get_absolute_axis(layout, 0, &axis) == 0);
	CHECK(axis.code == ABS_X);
	CHECK(axis.info.minimum == 0 && axis.info.maximum == 32767);
	CHECK(hid_report_layout_get_absolute_axis(layout, 1, &axis) == 0);
	CHECK(axis.code == ABS_Y);
	CHECK(axis.info.minimum == 0 && axis.info.maximum == 32767);
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(find_value(&input, EV_KEY, BTN_LEFT, &value) && value == 1);
	CHECK(find_value(&input, EV_ABS, ABS_X, &value) && value == 0x1234);
	CHECK(find_value(&input, EV_ABS, ABS_Y, &value) && value == 32767);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_sparse_and_reselected_ids(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	struct hid_report_report_info report_info;
	struct hid_report_input input;
	uint8_t first[] = {0x01, 0x05, 0xff};
	uint8_t seventh[] = {0x07, 0xfe};
	uint8_t last[] = {0xff, 0x01};
	uint8_t unknown[] = {0x02, 0};
	uint8_t prefix_only[] = {0x01};
	int32_t value;

	CHECK(hid_report_layout_parse(sparse_id_descriptor,
	    sizeof(sparse_id_descriptor), &layout) == 0);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.uses_report_ids != 0);
	CHECK(info.report_count == 3U);
	CHECK(hid_report_layout_get_report(layout, 0, &report_info) == 0);
	CHECK(report_info.report_id == 1U);
	CHECK(report_info.minimum_size == 3U);
	CHECK(report_info.field_count == 2U);
	CHECK(hid_report_layout_get_report(layout, 1, &report_info) == 0);
	CHECK(report_info.report_id == 7U);
	CHECK(report_info.minimum_size == 2U);
	CHECK(hid_report_layout_get_report(layout, 2, &report_info) == 0);
	CHECK(report_info.report_id == 255U);

	CHECK(hid_report_decode(layout, first, sizeof(first), &input) == 0);
	CHECK(input.report_id == 1U);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == 5);
	CHECK(find_value(&input, EV_REL, REL_WHEEL, &value) && value == -1);
	CHECK(hid_report_decode(layout, seventh, sizeof(seventh), &input) == 0);
	CHECK(input.report_id == 7U);
	CHECK(find_value(&input, EV_REL, REL_Y, &value) && value == -2);
	CHECK(hid_report_decode(layout, last, sizeof(last), &input) == 0);
	CHECK(input.report_id == 255U);
	CHECK(find_value(&input, EV_KEY, BTN_LEFT, &value) && value == 1);
	expect_decode_error(layout, unknown, sizeof(unknown), EINVAL);
	expect_decode_error(layout, prefix_only, sizeof(prefix_only), EINVAL);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_cross_byte_signed_field(void)
{
	static const uint8_t signed_32_descriptor[] = {
		0x05, 0x01, 0x09, 0x30,
		0x17, 0x00, 0x00, 0x00, 0x80,
		0x27, 0xff, 0xff, 0xff, 0x7f,
		0x75, 0x20, 0x95, 0x01, 0x81, 0x06,
	};
	struct hid_report_layout *layout = NULL;
	struct hid_report_input input;
	uint8_t negative_two[] = {0xf0, 0x7f};
	uint8_t positive_291[] = {0x18, 0x09};
	uint8_t negative_one_32[] = {0xff, 0xff, 0xff, 0xff};
	int32_t value;

	CHECK(hid_report_layout_parse(cross_byte_descriptor,
	    sizeof(cross_byte_descriptor), &layout) == 0);
	CHECK(hid_report_decode(layout, negative_two, sizeof(negative_two),
	    &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == -2);
	CHECK(hid_report_decode(layout, positive_291, sizeof(positive_291),
	    &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == 291);
	expect_decode_error(layout, negative_two, 1U, EINVAL);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);

	CHECK(hid_report_layout_parse(signed_32_descriptor,
	    sizeof(signed_32_descriptor), &layout) == 0);
	CHECK(hid_report_decode(layout, negative_one_32,
	    sizeof(negative_one_32), &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == -1);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_global_stack_and_long_item(void)
{
	static const uint8_t descriptor[] = {
		0x05, 0x01, 0x09, 0x30,
		0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
		0x35, 0x00, 0x45, 0x7f, 0x55, 0x00, 0x65, 0x00,
		0xa4, 0x75, 0x01, 0x95, 0x08, 0xb4,
		0xfe, 0x02, 0x77, 0xaa, 0x55,
		0x81, 0x06,
	};
	static const uint8_t unclosed_push[] = {
		0x05, 0x01, 0x09, 0x30,
		0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
		0x81, 0x06, 0xa4,
	};
	struct hid_report_layout *layout = NULL;
	struct hid_report_report_info info;
	struct hid_report_input input;
	uint8_t report[] = {0xfe};
	int32_t value;

	CHECK(hid_report_layout_parse(descriptor, sizeof(descriptor), &layout) ==
	    0);
	CHECK(hid_report_layout_get_report(layout, 0, &info) == 0);
	CHECK(info.minimum_size == 1U);
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == -2);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
	expect_parse_error(unclosed_push, sizeof(unclosed_push), EINVAL);
}

static void
test_boot_profiles(void)
{
	struct hid_report_layout *keyboard = NULL;
	struct hid_report_layout *mouse = NULL;
	struct hid_report_layout_info info;
	struct hid_report_report_info report_info;
	struct hid_report_input input;
	uint8_t keyboard_report[] = {0x22, 0, 0x04, 0x04, 0x05, 0, 0, 0};
	uint8_t bad_reserved[] = {0, 1, 0, 0, 0, 0, 0, 0};
	uint8_t mouse_report[] = {0x05, 0x7f, 0x81};
	int32_t value;

	CHECK(hid_report_layout_boot_keyboard(&keyboard) == 0);
	CHECK(hid_report_layout_get_info(keyboard, &info) == 0);
	CHECK(info.descriptor_size == 0U && info.report_count == 1U);
	CHECK(info.uses_report_ids == 0);
	CHECK(hid_report_layout_get_report(keyboard, 0, &report_info) == 0);
	CHECK(report_info.minimum_size == 8U);
	CHECK(hid_report_decode(keyboard, keyboard_report,
	    sizeof(keyboard_report), &input) == 0);
	CHECK(input.value_count == 4U);
	CHECK(find_value(&input, EV_KEY, KEY_LEFTSHIFT, NULL));
	CHECK(find_value(&input, EV_KEY, KEY_RIGHTSHIFT, NULL));
	CHECK(find_value(&input, EV_KEY, KEY_A, NULL));
	CHECK(find_value(&input, EV_KEY, KEY_B, NULL));
	expect_decode_error(keyboard, bad_reserved, sizeof(bad_reserved), EINVAL);
	expect_decode_error(keyboard, keyboard_report,
	    sizeof(keyboard_report) - 1U, EINVAL);

	CHECK(hid_report_layout_boot_mouse(&mouse) == 0);
	CHECK(hid_report_layout_get_report(mouse, 0, &report_info) == 0);
	CHECK(report_info.minimum_size == 3U);
	CHECK(hid_report_decode(mouse, mouse_report, sizeof(mouse_report),
	    &input) == 0);
	CHECK(find_value(&input, EV_KEY, BTN_LEFT, &value) && value == 1);
	CHECK(find_value(&input, EV_KEY, BTN_MIDDLE, &value) && value == 1);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == 127);
	CHECK(find_value(&input, EV_REL, REL_Y, &value) && value == -127);
	expect_decode_error(mouse, mouse_report, sizeof(mouse_report) - 1U,
	    EINVAL);

	hid_report_layout_destroy(keyboard);
	hid_report_layout_destroy(mouse);
	CHECK(live_allocations == 0U);
}

static void
test_descriptor_ownership(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_input input;
	uint8_t descriptor[sizeof(mouse_descriptor)];
	uint8_t report[] = {0, 1, 2, 3};
	int32_t value;

	memcpy(descriptor, mouse_descriptor, sizeof(descriptor));
	CHECK(hid_report_layout_parse(descriptor, sizeof(descriptor), &layout) ==
	    0);
	memset(descriptor, 0, sizeof(descriptor));
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == 1);
	CHECK(find_value(&input, EV_REL, REL_Y, &value) && value == 2);
	CHECK(find_value(&input, EV_REL, REL_WHEEL, &value) && value == 3);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
}

static void
test_unknown_usage_policy(void)
{
	static const uint8_t mixed[] = {
		0x05, 0x0c,
		0x09, 0x01,
		0x15, 0x00,
		0x25, 0x7f,
		0x75, 0x08,
		0x95, 0x01,
		0x81, 0x02,
		0x05, 0x01,
		0x09, 0x30,
		0x15, 0x81,
		0x25, 0x7f,
		0x75, 0x08,
		0x95, 0x01,
		0x81, 0x06,
	};
	static const uint8_t unknown_only[] = {
		0x05, 0x0c, 0x09, 0x01, 0x15, 0x00, 0x25, 0x7f,
		0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	};
	struct hid_report_layout *layout = NULL;
	struct hid_report_input input;
	uint8_t report[] = {0xee, 0xfb};
	int32_t value;

	CHECK(hid_report_layout_parse(mixed, sizeof(mixed), &layout) == 0);
	CHECK(hid_report_decode(layout, report, sizeof(report), &input) == 0);
	CHECK(input.value_count == 1U);
	CHECK(find_value(&input, EV_REL, REL_X, &value) && value == -5);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
	expect_parse_error(unknown_only, sizeof(unknown_only), EOPNOTSUPP);
}

static void
test_truncation_and_structure_errors(void)
{
	static const uint8_t short_one[] = {0x05};
	static const uint8_t short_two[] = {0x06, 0x01};
	static const uint8_t short_four[] = {0x07, 1, 2, 3};
	static const uint8_t long_prefix[] = {0xfe};
	static const uint8_t long_header[] = {0xfe, 1};
	static const uint8_t long_data[] = {0xfe, 2, 0x77, 0xaa};
	static const uint8_t end_underflow[] = {0xc0};
	static const uint8_t open_collection[] = {0xa1, 1};
	static const uint8_t pop_underflow[] = {0xb4};
	static const uint8_t id_zero[] = {0x85, 0};
	static const uint8_t delimiter[] = {0xa9, 1};
	static const uint8_t reserved_item[] = {0x0c};
	static const uint8_t reversed_usage[] = {
		0x05, 0x07, 0x19, 0x05, 0x29, 0x04,
		0x15, 0, 0x25, 0x10, 0x75, 8, 0x95, 1, 0x81, 0,
	};
	static const uint8_t reversed_logical[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 1, 0x25, 0,
		0x75, 8, 0x95, 1, 0x81, 2,
	};
	static const uint8_t zero_size[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 0, 0x25, 1,
		0x75, 0, 0x95, 1, 0x81, 2,
	};
	static const uint8_t zero_count[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 0, 0x25, 1,
		0x75, 1, 0x95, 0, 0x81, 2,
	};
	static const uint8_t wide_field[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 0, 0x25, 1,
		0x75, 33, 0x95, 1, 0x81, 2,
	};
	static const uint8_t mixed_ids[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 0x81, 0x25, 0x7f,
		0x75, 8, 0x95, 1, 0x81, 6,
		0x85, 1, 0x09, 0x31, 0x81, 6,
	};
	uint8_t nesting[17U * 2U];
	uint8_t pushes[17U];
	size_t index;

	expect_parse_error(short_one, sizeof(short_one), EINVAL);
	expect_parse_error(short_two, sizeof(short_two), EINVAL);
	expect_parse_error(short_four, sizeof(short_four), EINVAL);
	expect_parse_error(long_prefix, sizeof(long_prefix), EINVAL);
	expect_parse_error(long_header, sizeof(long_header), EINVAL);
	expect_parse_error(long_data, sizeof(long_data), EINVAL);
	expect_parse_error(end_underflow, sizeof(end_underflow), EINVAL);
	expect_parse_error(open_collection, sizeof(open_collection), EINVAL);
	expect_parse_error(pop_underflow, sizeof(pop_underflow), EINVAL);
	expect_parse_error(id_zero, sizeof(id_zero), EINVAL);
	expect_parse_error(delimiter, sizeof(delimiter), EOPNOTSUPP);
	expect_parse_error(reserved_item, sizeof(reserved_item), EOPNOTSUPP);
	expect_parse_error(reversed_usage, sizeof(reversed_usage), EINVAL);
	expect_parse_error(reversed_logical, sizeof(reversed_logical), EINVAL);
	expect_parse_error(zero_size, sizeof(zero_size), EINVAL);
	expect_parse_error(zero_count, sizeof(zero_count), EINVAL);
	expect_parse_error(wide_field, sizeof(wide_field), EINVAL);
	expect_parse_error(mixed_ids, sizeof(mixed_ids), EINVAL);

	for (index = 0; index < 17U; index++) {
		nesting[index * 2U] = 0xa1;
		nesting[index * 2U + 1U] = 1;
		pushes[index] = 0xa4;
	}
	expect_parse_error(nesting, sizeof(nesting), E2BIG);
	expect_parse_error(pushes, sizeof(pushes), E2BIG);
}

static void
test_logical_range_representability(void)
{
	static const uint8_t variable_unsigned[] = {
		0x05, 0x01, 0x09, 0x30,
		0x15, 0x00, 0x25, 0xff,
		0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
	};
	static const uint8_t variable_signed[] = {
		0x05, 0x01, 0x09, 0x30,
		0x16, 0x38, 0xff, 0x25, 0x64,
		0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	};
	static const uint8_t array_unsigned[] = {
		0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
		0x15, 0x00, 0x25, 0xff,
		0x75, 0x01, 0x95, 0x01, 0x81, 0x00,
	};
	static const uint8_t array_signed[] = {
		0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
		0x16, 0x38, 0xff, 0x25, 0x64,
		0x75, 0x08, 0x95, 0x01, 0x81, 0x00,
	};
	static const uint8_t unsupported_impossible_then_supported[] = {
		0x06, 0x00, 0xff, 0x09, 0x01,
		0x15, 0x00, 0x25, 0xff,
		0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
		0x05, 0x01, 0x09, 0x30,
		0x15, 0x81, 0x25, 0x7f,
		0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	};

	expect_parse_error(variable_unsigned, sizeof(variable_unsigned), EINVAL);
	expect_parse_error(variable_signed, sizeof(variable_signed), EINVAL);
	expect_parse_error(array_unsigned, sizeof(array_unsigned), EINVAL);
	expect_parse_error(array_signed, sizeof(array_signed), EINVAL);
	expect_parse_error(unsupported_impossible_then_supported,
	    sizeof(unsupported_impossible_then_supported), EINVAL);
}

static size_t
append_byte(uint8_t *buffer, size_t capacity, size_t used, uint8_t value)
{
	CHECK(used < capacity);
	buffer[used] = value;
	return used + 1U;
}

static void
test_report_id_limit(void)
{
	uint8_t descriptor[512];
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	size_t used = 0, index;

	static const uint8_t globals[] = {
		0x05, 0x07, 0x15, 0x00, 0x25, 0x01,
		0x75, 0x01, 0x95, 0x01,
	};
	memcpy(descriptor, globals, sizeof(globals));
	used = sizeof(globals);
	for (index = 0; index < HID_REPORT_ID_COUNT_MAX; index++) {
		used = append_byte(descriptor, sizeof(descriptor), used, 0x85);
		used = append_byte(descriptor, sizeof(descriptor), used,
		    (uint8_t)(index + 1U));
		used = append_byte(descriptor, sizeof(descriptor), used, 0x09);
		used = append_byte(descriptor, sizeof(descriptor), used,
		    (uint8_t)(0x04U + index));
		used = append_byte(descriptor, sizeof(descriptor), used, 0x81);
		used = append_byte(descriptor, sizeof(descriptor), used, 0x02);
	}
	CHECK(hid_report_layout_parse(descriptor, used, &layout) == 0);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.report_count == HID_REPORT_ID_COUNT_MAX);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);

	used = append_byte(descriptor, sizeof(descriptor), used, 0x85);
	used = append_byte(descriptor, sizeof(descriptor), used, 33U);
	expect_parse_error(descriptor, used, E2BIG);
}

static void
test_field_and_bit_limits(void)
{
	static const uint8_t fields_256[] = {
		0x05, 0x07, 0x19, 0x00, 0x29, 0xff,
		0x15, 0x00, 0x26, 0xff, 0x00,
		0x75, 0x08, 0x96, 0x00, 0x01, 0x81, 0x00,
	};
	static const uint8_t fields_257[] = {
		0x05, 0x07, 0x19, 0x00, 0x29, 0xff,
		0x15, 0x00, 0x26, 0xff, 0x00,
		0x75, 0x08, 0x96, 0x01, 0x01, 0x81, 0x00,
	};
	static const uint8_t bits_8192[] = {
		0x75, 0x01, 0x96, 0xff, 0x1f, 0x81, 0x01,
		0x05, 0x07, 0x09, 0x04, 0x15, 0x00, 0x25, 0x01,
		0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
	};
	static const uint8_t bits_8193[] = {
		0x75, 0x01, 0x96, 0x00, 0x20, 0x81, 0x01,
		0x05, 0x07, 0x09, 0x04, 0x15, 0x00, 0x25, 0x01,
		0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
	};
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	struct hid_report_report_info report_info;
	struct hid_report_input input;
	uint8_t field_report[256];
	uint8_t bit_report[1024];
	int error;

	error = hid_report_layout_parse(fields_256, sizeof(fields_256), &layout);
	if (error != 0)
		fprintf(stderr, "256-field boundary parse returned %d\n", error);
	CHECK(error == 0);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.field_count == HID_REPORT_FIELD_COUNT_MAX);
	memset(field_report, 0, sizeof(field_report));
	field_report[0] = 0x04;
	field_report[sizeof(field_report) - 1U] = 0x04;
	CHECK(hid_report_decode(layout, field_report, sizeof(field_report),
	    &input) == 0);
	CHECK(input.value_count == 1U);
	CHECK(find_value(&input, EV_KEY, KEY_A, NULL));
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
	expect_parse_error(fields_257, sizeof(fields_257), E2BIG);

	CHECK(hid_report_layout_parse(bits_8192, sizeof(bits_8192), &layout) ==
	    0);
	CHECK(hid_report_layout_get_report(layout, 0, &report_info) == 0);
	CHECK(report_info.minimum_size == sizeof(bit_report));
	memset(bit_report, 0, sizeof(bit_report));
	bit_report[sizeof(bit_report) - 1U] = 0x80;
	CHECK(hid_report_decode(layout, bit_report, sizeof(bit_report), &input) ==
	    0);
	CHECK(input.value_count == 1U);
	CHECK(find_value(&input, EV_KEY, KEY_A, NULL));
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
	expect_parse_error(bits_8193, sizeof(bits_8193), E2BIG);
}

static void
test_descriptor_size_limit(void)
{
	static const uint8_t supported[] = {
		0x05, 0x01, 0x09, 0x30, 0x15, 0x81, 0x25, 0x7f,
		0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
	};
	uint8_t descriptor[HID_REPORT_DESCRIPTOR_SIZE_MAX + 1U];
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout_info info;
	size_t used, item, index;

	memcpy(descriptor, supported, sizeof(supported));
	used = sizeof(supported);
	for (item = 0; item < 15U; item++) {
		descriptor[used++] = 0xfe;
		descriptor[used++] = 255U;
		descriptor[used++] = 0x77;
		for (index = 0; index < 255U; index++)
			descriptor[used++] = (uint8_t)index;
	}
	CHECK(HID_REPORT_DESCRIPTOR_SIZE_MAX - used == 212U);
	descriptor[used++] = 0xfe;
	descriptor[used++] = 209U;
	descriptor[used++] = 0x78;
	for (index = 0; index < 209U; index++)
		descriptor[used++] = (uint8_t)index;
	CHECK(used == HID_REPORT_DESCRIPTOR_SIZE_MAX);
	CHECK(hid_report_layout_parse(descriptor, used, &layout) == 0);
	CHECK(hid_report_layout_get_info(layout, &info) == 0);
	CHECK(info.descriptor_size == HID_REPORT_DESCRIPTOR_SIZE_MAX);
	hid_report_layout_destroy(layout);
	CHECK(live_allocations == 0U);
	descriptor[used++] = 0;
	expect_parse_error(descriptor, used, E2BIG);
}

static void
test_getter_and_argument_errors(void)
{
	struct hid_report_layout *layout = NULL;
	struct hid_report_layout *sentinel =
	    (struct hid_report_layout *)(uintptr_t)1U;
	struct hid_report_layout_info info;
	struct hid_report_report_info report_info;
	struct input_capability capability;
	struct input_abs_axis axis;
	struct hid_report_input input;
	uint8_t report[8] = {0};

	CHECK(hid_report_layout_parse(NULL, 1U, &sentinel) == EINVAL);
	CHECK(sentinel == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(hid_report_layout_parse(keyboard_descriptor, 0U, &sentinel) ==
	    EINVAL);
	CHECK(hid_report_layout_parse(keyboard_descriptor,
	    sizeof(keyboard_descriptor), NULL) == EINVAL);
	CHECK(hid_report_layout_boot_keyboard(NULL) == EINVAL);
	CHECK(hid_report_layout_boot_mouse(NULL) == EINVAL);
	CHECK(hid_report_layout_get_info(NULL, &info) == EINVAL);
	CHECK(hid_report_layout_get_info(NULL, NULL) == EINVAL);
	CHECK(hid_report_decode(NULL, report, sizeof(report), &input) == EINVAL);

	CHECK(hid_report_layout_parse(keyboard_descriptor,
	    sizeof(keyboard_descriptor), &layout) == 0);
	CHECK(hid_report_layout_get_info(layout, NULL) == EINVAL);
	CHECK(hid_report_layout_get_report(layout, 1U, &report_info) == ENOENT);
	CHECK(hid_report_layout_get_report(layout, 0U, NULL) == EINVAL);
	CHECK(hid_report_layout_get_capability(layout,
	    HID_REPORT_FIELD_COUNT_MAX, &capability) == ENOENT);
	CHECK(hid_report_layout_get_capability(layout, 0U, NULL) == EINVAL);
	CHECK(hid_report_layout_get_absolute_axis(layout, 0U, &axis) == ENOENT);
	CHECK(hid_report_layout_get_absolute_axis(layout, 0U, NULL) == EINVAL);
	CHECK(hid_report_decode(layout, NULL, sizeof(report), &input) == EINVAL);
	CHECK(hid_report_decode(layout, report, sizeof(report), NULL) == EINVAL);
	hid_report_layout_destroy(layout);
	hid_report_layout_destroy(NULL);
	CHECK(live_allocations == 0U);
}

static void
test_allocation_failures(void)
{
	struct hid_report_layout *layout;

	allocator_reset(1U);
	layout = (struct hid_report_layout *)(uintptr_t)1U;
	CHECK(hid_report_layout_parse(keyboard_descriptor,
	    sizeof(keyboard_descriptor), &layout) == ENOMEM);
	CHECK(layout == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(live_allocations == 0U);

	allocator_reset(2U);
	layout = (struct hid_report_layout *)(uintptr_t)1U;
	CHECK(hid_report_layout_parse(keyboard_descriptor,
	    sizeof(keyboard_descriptor), &layout) == ENOMEM);
	CHECK(layout == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(live_allocations == 0U);

	allocator_reset(1U);
	layout = (struct hid_report_layout *)(uintptr_t)1U;
	CHECK(hid_report_layout_boot_keyboard(&layout) == ENOMEM);
	CHECK(layout == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(live_allocations == 0U);

	allocator_reset(2U);
	layout = (struct hid_report_layout *)(uintptr_t)1U;
	CHECK(hid_report_layout_boot_mouse(&layout) == ENOMEM);
	CHECK(layout == (struct hid_report_layout *)(uintptr_t)1U);
	CHECK(live_allocations == 0U);
	allocator_reset(0U);
}

int
main(void)
{
	allocator_reset(0U);
	test_keyboard();
	test_keyboard_array_range_intersection();
	test_mixed_local_usage_order();
	test_mouse();
	test_tablet();
	test_sparse_and_reselected_ids();
	test_cross_byte_signed_field();
	test_global_stack_and_long_item();
	test_boot_profiles();
	test_descriptor_ownership();
	test_unknown_usage_policy();
	test_truncation_and_structure_errors();
	test_logical_range_representability();
	test_report_id_limit();
	test_field_and_bit_limits();
	test_descriptor_size_limit();
	test_getter_and_argument_errors();
	test_allocation_failures();
	CHECK(live_allocations == 0U);
	printf("hid report tests: %zu checks passed\n", checks);
	return 0;
}
