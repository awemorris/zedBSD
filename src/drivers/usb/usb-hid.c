/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated hid-report.c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "drivers/hid/hid-report.h"

#include "kern/kmem.h"

#include <errno.h>
#include <string.h>

#define HID_ITEM_TYPE_MAIN 0U
#define HID_ITEM_TYPE_GLOBAL 1U
#define HID_ITEM_TYPE_LOCAL 2U

#define HID_MAIN_INPUT 8U
#define HID_MAIN_OUTPUT 9U
#define HID_MAIN_COLLECTION 10U
#define HID_MAIN_FEATURE 11U
#define HID_MAIN_END_COLLECTION 12U

#define HID_GLOBAL_USAGE_PAGE 0U
#define HID_GLOBAL_LOGICAL_MINIMUM 1U
#define HID_GLOBAL_LOGICAL_MAXIMUM 2U
#define HID_GLOBAL_REPORT_SIZE 7U
#define HID_GLOBAL_REPORT_ID 8U
#define HID_GLOBAL_REPORT_COUNT 9U
#define HID_GLOBAL_PUSH 10U
#define HID_GLOBAL_POP 11U

#define HID_LOCAL_USAGE 0U
#define HID_LOCAL_USAGE_MINIMUM 1U
#define HID_LOCAL_USAGE_MAXIMUM 2U
#define HID_LOCAL_DELIMITER 10U

#define HID_INPUT_CONSTANT 0x01U
#define HID_INPUT_VARIABLE 0x02U
#define HID_INPUT_RELATIVE 0x04U
#define HID_INPUT_SUPPORTED_FLAGS 0x07U

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01U
#define HID_USAGE_PAGE_KEYBOARD 0x07U
#define HID_USAGE_PAGE_BUTTON 0x09U

#define HID_USAGE_X 0x30U
#define HID_USAGE_Y 0x31U
#define HID_USAGE_WHEEL 0x38U
#define HID_USAGE_KEYBOARD_ERROR_MIN 0x01U
#define HID_USAGE_KEYBOARD_ERROR_MAX 0x03U

#define HID_FIELD_KEY 1U
#define HID_FIELD_AXIS 2U
#define HID_FIELD_KEYBOARD_ARRAY 3U

#define HID_LAYOUT_PROFILE_DESCRIPTOR 0U
#define HID_LAYOUT_PROFILE_BOOT_KEYBOARD 1U
#define HID_LAYOUT_PROFILE_BOOT_MOUSE 2U

struct hid_report_field {
	uint32_t bit_offset;
	uint32_t usage_minimum;
	uint32_t usage_maximum;
	int32_t logical_minimum;
	int32_t logical_maximum;
	uint16_t type;
	uint16_t code;
	uint8_t report_id;
	uint8_t bit_size;
	uint8_t kind;
	uint8_t reserved;
};

struct hid_report_description {
	uint32_t bit_count;
	size_t field_count;
	uint8_t id;
};

struct hid_report_layout {
	uint8_t descriptor[HID_REPORT_DESCRIPTOR_SIZE_MAX];
	size_t descriptor_size;
	struct hid_report_description reports[HID_REPORT_ID_COUNT_MAX];
	size_t report_count;
	struct hid_report_field fields[HID_REPORT_FIELD_COUNT_MAX];
	size_t field_count;
	struct input_capability capabilities[HID_REPORT_FIELD_COUNT_MAX + 1U];
	size_t capability_count;
	struct input_abs_axis absolute_axes[ABS_MAX + 1U];
	size_t absolute_axis_count;
	int uses_report_ids;
	uint8_t profile;
};

struct hid_global_state {
	uint32_t usage_page;
	uint32_t logical_maximum_raw;
	uint32_t report_size;
	uint32_t report_count;
	int32_t logical_minimum;
	uint8_t logical_maximum_size;
	uint8_t report_id;
	uint8_t logical_minimum_set;
	uint8_t logical_maximum_set;
};

struct hid_local_usage_span {
	uint32_t minimum;
	uint32_t maximum;
	uint8_t is_range;
};

struct hid_local_state {
	struct hid_local_usage_span usages[HID_REPORT_FIELD_COUNT_MAX];
	size_t usage_count;
	size_t open_range;
	uint8_t range_open;
};

struct hid_parser {
	struct hid_report_layout *layout;
	struct hid_global_state global;
	struct hid_global_state global_stack[HID_REPORT_GLOBAL_DEPTH_MAX];
	size_t global_depth;
	struct hid_local_state local;
	size_t collection_depth;
	int no_id_report_used;
	int supported_field_seen;
};

/* Supports the item unsigned operation. */
static uint32_t item_unsigned(const uint8_t *data, size_t size);

/* Supports the item unsigned operation. */
static uint32_t
item_unsigned(
	const uint8_t *data,
	size_t size)
{
	uint32_t value = 0;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < size; index++)
		value |= (uint32_t)data[index] << (index * 8U);

	/* Returns the computed result. */
	return value;
}

/* Supports the sign extend operation. */
static int32_t sign_extend(uint32_t value, unsigned bits);

/* Supports the sign extend operation. */
static int32_t
sign_extend(
	uint32_t value,
	unsigned bits)
{
	int64_t extended = value;

	/* Validates the current value. */
	if ((value & ((uint32_t)1U << (bits - 1U))) != 0)
		extended -= (int64_t)((uint64_t)1U << bits);

	/* Returns the computed result. */
	return (int32_t)extended;
}

/* Supports the item signed operation. */
static int item_signed(const uint8_t *data, size_t size, int32_t *result);

/* Supports the item signed operation. */
static int
item_signed(
	const uint8_t *data,
	size_t size,
	int32_t *result)
{
	/* Checks the current data size. */
	if (size != 1U && size != 2U && size != 4U)
		return EINVAL;
	*result = sign_extend(item_unsigned(data, size), (unsigned)size * 8U);
	/* Reports successful completion. */
	return 0;
}

/* Supports the local clear operation. */
static void local_clear(struct hid_local_state *local);

/* Supports the local clear operation. */
static void
local_clear(
	struct hid_local_state *local)
{
	memset(local, 0, sizeof(*local));
}

/* Supports the usage value operation. */
static int usage_value(const struct hid_global_state *global, const uint8_t *data, size_t size, uint32_t *result);

/* Supports the usage value operation. */
static int
usage_value(
	const struct hid_global_state *global,
	const uint8_t *data,
	size_t size,
	uint32_t *result)
{
	uint32_t raw;

	/* Checks the current data size. */
	if (size != 1U && size != 2U && size != 4U)
		return EINVAL;
	raw = item_unsigned(data, size);

	/* Checks the current data size. */
	if (size == 4U) {
		*result = raw;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles the global condition. */
	if (global->usage_page > UINT16_MAX)
		return EOPNOTSUPP;
	*result = (global->usage_page << 16U) | raw;
	/* Reports successful completion. */
	return 0;
}

/* Supports the local validate operation. */
static int local_validate(const struct hid_local_state *local);

/* Supports the local validate operation. */
static int
local_validate(
	const struct hid_local_state *local)
{
	/* Returns the computed result. */
	return local->range_open ? EINVAL : 0;
}

/* Supports the local usage at operation. */
static int local_usage_at(const struct hid_local_state *local, uint32_t index, uint32_t *usage);

/* Supports the local usage at operation. */
static int
local_usage_at(
	const struct hid_local_state *local,
	uint32_t index,
	uint32_t *usage)
{
	const struct hid_local_usage_span *span;
	uint32_t span_count;
	size_t span_index;

	/* Process each remaining element. */
	for (span_index = 0; span_index < local->usage_count; span_index++) {
		span = &local->usages[span_index];
		span_count = span->maximum - span->minimum + 1U;

		/* Checks the current index. */
		if (index < span_count) {
			*usage = span->minimum + index;
			/* Reports operation failure. */
			return 1;
		}
		index -= span_count;
	}

	/* Handles the local condition. */
	if (local->usage_count == 0U)
		return 0;
	*usage = local->usages[local->usage_count - 1U].maximum;
	/* Reports operation failure. */
	return 1;
}

/* Supports the find report operation. */
static struct hid_report_description * find_report(struct hid_report_layout *layout, uint8_t id);

/* Supports the find report operation. */
static struct hid_report_description *
find_report(
	struct hid_report_layout *layout,
	uint8_t id)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < layout->report_count; index++) {
		/* Handles the layout condition. */
		if (layout->reports[index].id == id)
			return &layout->reports[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the find report const operation. */
static const struct hid_report_description * find_report_const(const struct hid_report_layout *layout, uint8_t id);

/* Supports the find report const operation. */
static const struct hid_report_description *
find_report_const(
	const struct hid_report_layout *layout,
	uint8_t id)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < layout->report_count; index++) {
		/* Handles the layout condition. */
		if (layout->reports[index].id == id)
			return &layout->reports[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the add report operation. */
static int add_report(struct hid_report_layout *layout, uint8_t id, struct hid_report_description **result);

/* Supports the add report operation. */
static int
add_report(
	struct hid_report_layout *layout,
	uint8_t id,
	struct hid_report_description **result)
{
	struct hid_report_description *report;

	/* Checks the find report result. */
	if (find_report(layout, id) != NULL)
		return EINVAL;

	/* Handles the layout condition. */
	if (layout->report_count >= HID_REPORT_ID_COUNT_MAX)
		return E2BIG;
	report = &layout->reports[layout->report_count++];
	memset(report, 0, sizeof(*report));
	report->id = id;
	*result = report;
	/* Reports successful completion. */
	return 0;
}

/* Supports the add capability operation. */
static int add_capability(struct hid_report_layout *layout, uint16_t type, uint16_t code);

/* Supports the add capability operation. */
static int
add_capability(
	struct hid_report_layout *layout,
	uint16_t type,
	uint16_t code)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < layout->capability_count; index++) {
		/* Handles the layout condition. */
		if (layout->capabilities[index].type == type &&
		    layout->capabilities[index].code == code)

			/* Reports successful completion. */
			return 0;
	}

	/* Handles the layout condition. */
	if (layout->capability_count >= HID_REPORT_FIELD_COUNT_MAX + 1U)
		return E2BIG;
	layout->capabilities[layout->capability_count].type = type;
	layout->capabilities[layout->capability_count].code = code;
	layout->capability_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the add absolute axis operation. */
static int add_absolute_axis(struct hid_report_layout *layout, uint16_t code, int32_t minimum, int32_t maximum);

/* Supports the add absolute axis operation. */
static int
add_absolute_axis(
	struct hid_report_layout *layout,
	uint16_t code,
	int32_t minimum,
	int32_t maximum)
{
	struct input_abs_axis *axis;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < layout->absolute_axis_count; index++) {
		axis = &layout->absolute_axes[index];

		/* Handles the axis condition. */
		if (axis->code != code)
			continue;

		/* Returns the computed result. */
		return axis->info.minimum == minimum &&
				       axis->info.maximum == maximum
			       ? 0
			       : EINVAL;
	}

	/* Handles the layout condition. */
	if (layout->absolute_axis_count >= ABS_MAX + 1U)
		return E2BIG;
	axis = &layout->absolute_axes[layout->absolute_axis_count++];
	memset(axis, 0, sizeof(*axis));
	axis->code = code;
	axis->info.minimum = minimum;
	axis->info.maximum = maximum;
	axis->info.value = minimum;

	/* Reports successful completion. */
	return 0;
}

/* Supports the keyboard code operation. */
static uint16_t keyboard_code(uint16_t usage);

/* Supports the keyboard code operation. */
static uint16_t
keyboard_code(
	uint16_t usage)
{
	static const uint16_t alpha[26] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
		KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
		KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const uint16_t digits[10] = {
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
		KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	};

	/* Handles the usage condition. */
	if (usage >= 0x04U && usage <= 0x1dU)
		return alpha[usage - 0x04U];

	/* Handles the usage condition. */
	if (usage >= 0x1eU && usage <= 0x27U)
		return digits[usage - 0x1eU];

	/* Handles the usage condition. */
	if (usage >= 0x3aU && usage <= 0x43U)
		return (uint16_t)(KEY_F1 + usage - 0x3aU);
	/* Dispatch the selected operation case. */
	switch (usage) {
	case 0x28:
		/* Returns the computed result. */
		return KEY_ENTER;
	case 0x29:
		/* Returns the computed result. */
		return KEY_ESC;
	case 0x2a:
		/* Returns the computed result. */
		return KEY_BACKSPACE;
	case 0x2b:
		/* Returns the computed result. */
		return KEY_TAB;
	case 0x2c:
		/* Returns the computed result. */
		return KEY_SPACE;
	case 0x2d:
		/* Returns the computed result. */
		return KEY_MINUS;
	case 0x2e:
		/* Returns the computed result. */
		return KEY_EQUAL;
	case 0x2f:
		/* Returns the computed result. */
		return KEY_LEFTBRACE;
	case 0x30:
		/* Returns the computed result. */
		return KEY_RIGHTBRACE;
	case 0x31:
		/* Returns the computed result. */
		return KEY_BACKSLASH;
	case 0x33:
		/* Returns the computed result. */
		return KEY_SEMICOLON;
	case 0x34:
		/* Returns the computed result. */
		return KEY_APOSTROPHE;
	case 0x35:
		/* Returns the computed result. */
		return KEY_GRAVE;
	case 0x36:
		/* Returns the computed result. */
		return KEY_COMMA;
	case 0x37:
		/* Returns the computed result. */
		return KEY_DOT;
	case 0x38:
		/* Returns the computed result. */
		return KEY_SLASH;
	case 0x39:
		/* Returns the computed result. */
		return KEY_CAPSLOCK;
	case 0x44:
		/* Returns the computed result. */
		return KEY_F11;
	case 0x45:
		/* Returns the computed result. */
		return KEY_F12;
	case 0x49:
		/* Returns the computed result. */
		return KEY_INSERT;
	case 0x4a:
		/* Returns the computed result. */
		return KEY_HOME;
	case 0x4b:
		/* Returns the computed result. */
		return KEY_PAGEUP;
	case 0x4c:
		/* Returns the computed result. */
		return KEY_DELETE;
	case 0x4d:
		/* Returns the computed result. */
		return KEY_END;
	case 0x4e:
		/* Returns the computed result. */
		return KEY_PAGEDOWN;
	case 0x4f:
		/* Returns the computed result. */
		return KEY_RIGHT;
	case 0x50:
		/* Returns the computed result. */
		return KEY_LEFT;
	case 0x51:
		/* Returns the computed result. */
		return KEY_DOWN;
	case 0x52:
		/* Returns the computed result. */
		return KEY_UP;
	case 0xe0:
		/* Returns the computed result. */
		return KEY_LEFTCTRL;
	case 0xe1:
		/* Returns the computed result. */
		return KEY_LEFTSHIFT;
	case 0xe2:
		/* Returns the computed result. */
		return KEY_LEFTALT;
	case 0xe4:
		/* Returns the computed result. */
		return KEY_RIGHTCTRL;
	case 0xe5:
		/* Returns the computed result. */
		return KEY_RIGHTSHIFT;
	case 0xe6:
		/* Returns the computed result. */
		return KEY_RIGHTALT;
	default:
		/* Returns the computed result. */
		return KEY_RESERVED;
	}
}

/* Supports the usage to event operation. */
static int usage_to_event(uint32_t usage, unsigned input_flags, uint16_t *type, uint16_t *code, uint8_t *kind);

/* Supports the usage to event operation. */
static int
usage_to_event(
	uint32_t usage,
	unsigned input_flags,
	uint16_t *type,
	uint16_t *code,
	uint8_t *kind)
{
	uint16_t page = (uint16_t)(usage >> 16U);
	uint16_t value = (uint16_t)usage;
	uint16_t key;

	/* Handles the page condition. */
	if (page == HID_USAGE_PAGE_KEYBOARD) {
		key = keyboard_code(value);

		/* Handles the selected key. */
		if (key == KEY_RESERVED)
			return 0;
		*type = EV_KEY;
		*code = key;
		*kind = HID_FIELD_KEY;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles the page condition. */
	if (page == HID_USAGE_PAGE_BUTTON && value >= 1U && value <= 5U) {
		*type = EV_KEY;
		*code = (uint16_t)(BTN_LEFT + value - 1U);
		*kind = HID_FIELD_KEY;
		/* Reports operation failure. */
		return 1;
	}

	/* Handles the page condition. */
	if (page != HID_USAGE_PAGE_GENERIC_DESKTOP)
		return 0;

	/* Handles the input flags condition. */
	if ((input_flags & HID_INPUT_RELATIVE) != 0) {
		*type = EV_REL;
		*kind = HID_FIELD_AXIS;
		/* Dispatch the selected operation case. */
		switch (value) {
		case HID_USAGE_X:
			*code = REL_X;
			/* Reports operation failure. */
			return 1;
		case HID_USAGE_Y:
			*code = REL_Y;
			/* Reports operation failure. */
			return 1;
		case HID_USAGE_WHEEL:
			*code = REL_WHEEL;
			/* Reports operation failure. */
			return 1;
		default:
			/* Reports successful completion. */
			return 0;
		}
	}
	*type = EV_ABS;
	*kind = HID_FIELD_AXIS;
	/* Dispatch the selected operation case. */
	switch (value) {
	case HID_USAGE_X:
		*code = ABS_X;
		/* Reports operation failure. */
		return 1;
	case HID_USAGE_Y:
		*code = ABS_Y;
		/* Reports operation failure. */
		return 1;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/* Supports the logical maximum operation. */
static int logical_maximum(const struct hid_global_state *global, int32_t *result);

/* Supports the logical maximum operation. */
static int
logical_maximum(
	const struct hid_global_state *global,
	int32_t *result)
{
	uint32_t raw;

	/* Handles the global condition. */
	if (!global->logical_minimum_set || !global->logical_maximum_set)
		return EINVAL;
	raw = global->logical_maximum_raw;

	/* Handles the global condition. */
	if (global->logical_minimum < 0) {
		*result = sign_extend(
			raw, (unsigned)global->logical_maximum_size * 8U);
	} else {
		/* Handles the raw condition. */
		if (raw > INT32_MAX)
			return EINVAL;
		*result = (int32_t)raw;
	}

	/* Returns the computed result. */
	return global->logical_minimum <= *result ? 0 : EINVAL;
}

/* Supports the logical range fits field operation. */
static int logical_range_fits_field(int32_t minimum, int32_t maximum, uint32_t bit_size);

/* Supports the logical range fits field operation. */
static int
logical_range_fits_field(
	int32_t minimum,
	int32_t maximum,
	uint32_t bit_size)
{
	int64_t field_minimum, field_maximum;

	/* Handles the bit size condition. */
	if (bit_size == 0U || bit_size > 32U || minimum > maximum)
		return 0;

	/* Handles the minimum condition. */
	if (minimum < 0) {
		field_minimum = -(int64_t)(UINT64_C(1) << (bit_size - 1U));
		field_maximum = (int64_t)(UINT64_C(1) << (bit_size - 1U)) - 1;
	} else {
		field_minimum = 0;
		field_maximum =
			(int64_t)((UINT64_C(1) << bit_size) - UINT64_C(1));
	}

	/* Returns the computed result. */
	return (int64_t)minimum >= field_minimum &&
	       (int64_t)maximum <= field_maximum;
}

/* Supports the add field operation. */
static int add_field(struct hid_parser *parser, struct hid_report_description *report, uint32_t bit_offset, uint32_t usage_minimum, uint32_t usage_maximum, int32_t logical_minimum, int32_t logical_maximum, uint16_t type, uint16_t code, uint8_t bit_size, uint8_t kind);

/* Supports the add field operation. */
static int
add_field(
	struct hid_parser *parser,
	struct hid_report_description *report,
	uint32_t bit_offset,
	uint32_t usage_minimum,
	uint32_t usage_maximum,
	int32_t logical_minimum,
	int32_t logical_maximum,
	uint16_t type,
	uint16_t code,
	uint8_t bit_size,
	uint8_t kind)
{
	struct hid_report_layout *layout = parser->layout;
	struct hid_report_field *field;
	size_t index;
	int error;

	/* Handles the bit size condition. */
	if (bit_size == 0U || bit_size > 32U)
		return EINVAL;

	/* Handles the layout condition. */
	if (layout->field_count >= HID_REPORT_FIELD_COUNT_MAX)
		return E2BIG;

	/* Handles the kind condition. */
	if (kind != HID_FIELD_KEYBOARD_ARRAY) {
		/* Process each remaining element. */
		for (index = 0; index < layout->field_count; index++) {
			field = &layout->fields[index];

			/* Handles the field condition. */
			if (field->report_id == report->id &&
			    field->kind != HID_FIELD_KEYBOARD_ARRAY &&
			    field->type == type && field->code == code)

				/* Returns the computed result. */
				return EINVAL;
		}
	}

	/* Checks the operation status. */
	if (kind != HID_FIELD_KEYBOARD_ARRAY &&
	    (error = add_capability(layout, type, code)) != 0)

		/* Returns the computed result. */
		return error;

	/* Checks the operation status. */
	if (type == EV_ABS &&
	    (error = add_absolute_axis(layout, code, logical_minimum,
				       logical_maximum)) != 0)

		/* Returns the computed result. */
		return error;
	field = &layout->fields[layout->field_count++];
	memset(field, 0, sizeof(*field));
	field->bit_offset = bit_offset;
	field->usage_minimum = usage_minimum;
	field->usage_maximum = usage_maximum;
	field->logical_minimum = logical_minimum;
	field->logical_maximum = logical_maximum;
	field->type = type;
	field->code = code;
	field->report_id = report->id;
	field->bit_size = bit_size;
	field->kind = kind;
	report->field_count++;
	parser->supported_field_seen = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the add keyboard array capabilities operation. */
static int add_keyboard_array_capabilities(struct hid_report_layout *layout, uint32_t minimum, uint32_t maximum);

/* Supports the add keyboard array capabilities operation. */
static int
add_keyboard_array_capabilities(
	struct hid_report_layout *layout,
	uint32_t minimum,
	uint32_t maximum)
{
	uint16_t code;
	uint16_t page = (uint16_t)(minimum >> 16U);
	uint16_t first = (uint16_t)minimum;
	uint16_t last = (uint16_t)maximum;
	uint16_t usage;
	int error;

	/* Handles the page condition. */
	if (page != HID_USAGE_PAGE_KEYBOARD ||
	    page != (uint16_t)(maximum >> 16U))

		/* Returns the computed result. */
		return EOPNOTSUPP;
	/* Process each element required by the operation. */
	for (usage = 0; usage <= 0xe7U; usage++) {
		/* Handles the usage condition. */
		if (usage < first || usage > last)
			continue;
		code = keyboard_code(usage);

		/* Handles the code condition. */
		if (code == KEY_RESERVED)
			continue;
		error = add_capability(layout, EV_KEY, code);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse input operation. */
static int parse_input(struct hid_parser *parser, uint32_t flags);

/* Supports the parse input operation. */
static int
parse_input(
	struct hid_parser *parser,
	uint32_t flags)
{
	uint16_t type, code;
	uint8_t kind;
	struct hid_report_layout *layout = parser->layout;
	struct hid_report_description *report;
	const struct hid_local_usage_span *array_usage;
	uint32_t bit_offset, bits, count, index, usage;
	uint32_t accepted_minimum, accepted_maximum;
	uint32_t accepted_usage_minimum, accepted_usage_maximum;
	int32_t logical_max;
	int error;

	error = local_validate(&parser->local);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the active flags. */
	if ((flags & ~HID_INPUT_SUPPORTED_FLAGS) != 0U)
		return EOPNOTSUPP;

	/* Checks the parser state. */
	if (parser->global.report_size == 0U ||
	    parser->global.report_count == 0U)

		/* Returns the computed result. */
		return EINVAL;

	/* Checks the parser state. */
	if (parser->global.report_size > HID_REPORT_BITS_MAX ||
	    parser->global.report_count > HID_REPORT_BITS_MAX)

		/* Returns the computed result. */
		return E2BIG;

	/* Checks the parser state. */
	if (parser->global.report_id == 0U) {
		/* Handles the layout condition. */
		if (layout->uses_report_ids)
			return EINVAL;
		parser->no_id_report_used = 1;
		report = find_report(layout, 0);

		/* Checks the operation status. */
		if (report == NULL &&
		    (error = add_report(layout, 0, &report)) != 0)

			/* Returns the computed result. */
			return error;
	} else {
		/* Checks the parser state. */
		if (parser->no_id_report_used)
			return EINVAL;
		report = find_report(layout, parser->global.report_id);

		/* Handles the report availability. */
		if (report == NULL)
			return EINVAL;
	}
	count = parser->global.report_count;

	/* Checks the remaining item count. */
	if (count > (HID_REPORT_BITS_MAX - report->bit_count) /
			    parser->global.report_size)

		/* Returns the computed result. */
		return E2BIG;
	bits = count * parser->global.report_size;
	bit_offset = report->bit_count;

	/* Checks the active flags. */
	if ((flags & HID_INPUT_CONSTANT) != 0) {
		report->bit_count += bits;

		/* Reports successful completion. */
		return 0;
	}

	/*
	 * Logical bounds describe every Data field, including usages outside
	 * the v1 mapping.  Validate them before usage filtering so an
	 * impossible vendor field cannot be hidden in front of an otherwise
	 * supported field.
	 */
	if ((error = logical_maximum(&parser->global, &logical_max)) != 0)
		return error;

	/* Checks the logical range fits field result. */
	if (!logical_range_fits_field(parser->global.logical_minimum,
				      logical_max, parser->global.report_size))

		/* Returns the computed result. */
		return EINVAL;

	/* Checks the active flags. */
	if ((flags & HID_INPUT_VARIABLE) == 0) {
		/* Checks the parser state. */
		if (parser->local.usage_count != 1U ||
		    !parser->local.usages[0].is_range)

			/* Returns the computed result. */
			return EOPNOTSUPP;
		array_usage = &parser->local.usages[0];

		/* Handles the array usage condition. */
		if ((array_usage->minimum >> 16U) != HID_USAGE_PAGE_KEYBOARD ||
		    (array_usage->maximum >> 16U) != HID_USAGE_PAGE_KEYBOARD)
			goto advance;

		/* Checks the parser state. */
		if (parser->global.logical_minimum < 0 ||
		    logical_max > (int32_t)UINT16_MAX)

			/* Returns the computed result. */
			return EINVAL;

		/*
		 * Array values are usage IDs.  Only values admitted by both the
		 * local Usage range and the global Logical range may be
		 * advertised or decoded.  Keeping the intersection in the field
		 * also makes HID keyboard error usages 1..3 visible only when
		 * the descriptor actually permits them.
		 */
		accepted_minimum = (uint16_t)array_usage->minimum;

		/* Handles the uint32 t condition. */
		if ((uint32_t)parser->global.logical_minimum >
		    accepted_minimum) {
			accepted_minimum =
				(uint32_t)parser->global.logical_minimum;
		}
		accepted_maximum = (uint16_t)array_usage->maximum;

		/* Handles the uint32 t condition. */
		if ((uint32_t)logical_max < accepted_maximum)
			accepted_maximum = (uint32_t)logical_max;

		/* Handles the accepted minimum condition. */
		if (accepted_minimum > accepted_maximum)
			goto advance;
		accepted_usage_minimum =
			(HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_minimum;
		accepted_usage_maximum =
			(HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_maximum;
		error = add_keyboard_array_capabilities(
			layout, accepted_usage_minimum, accepted_usage_maximum);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			error = add_field(
				parser, report,
				bit_offset + index * parser->global.report_size,
				accepted_usage_minimum, accepted_usage_maximum,
				(int32_t)accepted_minimum,
				(int32_t)accepted_maximum, EV_KEY, KEY_RESERVED,
				(uint8_t)parser->global.report_size,
				HID_FIELD_KEYBOARD_ARRAY);

			/* Checks the operation status. */
			if (error != 0)
				return error;
		}
		goto advance;
	}
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Checks the local usage at result. */
		if (!local_usage_at(&parser->local, index, &usage) ||
		    !usage_to_event(usage, flags, &type, &code, &kind))
			continue;
		error = add_field(
			parser, report,
			bit_offset + index * parser->global.report_size, usage,
			usage, parser->global.logical_minimum, logical_max,
			type, code, (uint8_t)parser->global.report_size, kind);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
advance:
	report->bit_count += bits;

	/* Reports successful completion. */
	return 0;
}

/* Supports the parse main operation. */
static int parse_main(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);

/* Supports the parse main operation. */
static int
parse_main(
	struct hid_parser *parser,
	unsigned tag,
	const uint8_t *data,
	size_t size)
{
	int error;

	error = local_validate(&parser->local);

	/* Checks the operation status. */
	if (error != 0) {
		local_clear(&parser->local);

		/* Returns the computed result. */
		return error;
	}
	error = 0;

	/* Dispatch the selected operation case. */
	switch (tag) {
	case HID_MAIN_INPUT:
		/* Checks the current data size. */
		if (size == 0U)
			error = EINVAL;
		else
			error = parse_input(parser, item_unsigned(data, size));
		break;
	case HID_MAIN_COLLECTION:
		/* Checks the current data size. */
		if (size == 0U) {
			error = EINVAL;
		} else {
			/* Checks the parser state. */
			if (parser->collection_depth >=
			    HID_REPORT_COLLECTION_DEPTH_MAX)
				error = E2BIG;
			else
				parser->collection_depth++;
		}
		break;
	case HID_MAIN_END_COLLECTION:
		/* Checks the current data size. */
		if (size != 0U)
			error = EINVAL;
		else if (parser->collection_depth == 0U)
			error = EINVAL;
		else
			parser->collection_depth--;
		break;
	case HID_MAIN_OUTPUT:
	case HID_MAIN_FEATURE:
		/* Output and feature layouts are deliberately outside v1. */
		if (size == 0U)
			error = EINVAL;
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	local_clear(&parser->local);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the parse global operation. */
static int parse_global(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);

/* Supports the parse global operation. */
static int
parse_global(
	struct hid_parser *parser,
	unsigned tag,
	const uint8_t *data,
	size_t size)
{
	int function_result;
	struct hid_report_description *ignored;
	uint32_t value;
	int error;

	/* Dispatch the selected operation case. */
	switch (tag) {
	case HID_GLOBAL_USAGE_PAGE:
		/* Checks the current data size. */
		if (size == 0U)
			return EINVAL;
		value = item_unsigned(data, size);

		/* Validates the current value. */
		if (value > UINT16_MAX)
			return EOPNOTSUPP;
		parser->global.usage_page = value;

		/* Reports successful completion. */
		return 0;
	case HID_GLOBAL_LOGICAL_MINIMUM:
		error = item_signed(data, size,
				    &parser->global.logical_minimum);

		/* Checks the operation status. */
		if (error == 0)
			parser->global.logical_minimum_set = 1;

		/* Returns the computed result. */
		return error;
	case HID_GLOBAL_LOGICAL_MAXIMUM:
		/* Checks the current data size. */
		if (size != 1U && size != 2U && size != 4U)
			return EINVAL;
		parser->global.logical_maximum_raw = item_unsigned(data, size);
		parser->global.logical_maximum_size = (uint8_t)size;
		parser->global.logical_maximum_set = 1;

		/* Reports successful completion. */
		return 0;
	case HID_GLOBAL_REPORT_SIZE:
		/* Checks the current data size. */
		if (size == 0U)
			return EINVAL;
		parser->global.report_size = item_unsigned(data, size);

		/* Reports successful completion. */
		return 0;
	case HID_GLOBAL_REPORT_COUNT:
		/* Checks the current data size. */
		if (size == 0U)
			return EINVAL;
		parser->global.report_count = item_unsigned(data, size);

		/* Reports successful completion. */
		return 0;
	case HID_GLOBAL_REPORT_ID:
		/* Checks the current data size. */
		if (size != 1U || data[0] == 0U || parser->no_id_report_used)
			return EINVAL;
		parser->layout->uses_report_ids = 1;
		parser->global.report_id = data[0];

		/* Checks the find report result. */
		if (find_report(parser->layout, data[0]) != NULL)
			return 0;

		/* Obtains the add report result. */
		function_result = add_report(parser->layout, data[0], &ignored);

		/* Returns the computed result. */
		return function_result;
	case HID_GLOBAL_PUSH:
		/* Checks the current data size. */
		if (size != 0U)
			return EINVAL;

		/* Checks the parser state. */
		if (parser->global_depth >= HID_REPORT_GLOBAL_DEPTH_MAX)
			return E2BIG;
		parser->global_stack[parser->global_depth++] = parser->global;

		/* Reports successful completion. */
		return 0;
	case HID_GLOBAL_POP:
		/* Checks the current data size. */
		if (size != 0U || parser->global_depth == 0U)
			return EINVAL;
		parser->global = parser->global_stack[--parser->global_depth];

		/* Reports successful completion. */
		return 0;
	case 3U: /* Physical Minimum */
	case 4U: /* Physical Maximum */
	case 5U: /* Unit Exponent */
	case 6U: /* Unit */

		/* These globals cannot change supported input decoding. */
		return size == 1U || size == 2U || size == 4U ? 0 : EINVAL;
	default:
		/* Returns the computed result. */
		return EOPNOTSUPP;
	}
}

/* Supports the parse local operation. */
static int parse_local(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);

/* Supports the parse local operation. */
static int
parse_local(
	struct hid_parser *parser,
	unsigned tag,
	const uint8_t *data,
	size_t size)
{
	struct hid_local_usage_span *span;
	uint32_t usage;
	int error;

	/* Handles the tag condition. */
	if (tag == HID_LOCAL_DELIMITER) {
		return size == 1U || size == 2U || size == 4U ? EOPNOTSUPP
							      : EINVAL;
	}

	/* Handles the tag condition. */
	if (tag >= 3U && tag <= 9U)
		return size == 1U || size == 2U || size == 4U ? 0 : EINVAL;

	/* Handles the tag condition. */
	if (tag != HID_LOCAL_USAGE && tag != HID_LOCAL_USAGE_MINIMUM &&
	    tag != HID_LOCAL_USAGE_MAXIMUM)

		/* Returns the computed result. */
		return EOPNOTSUPP;
	error = usage_value(&parser->global, data, size, &usage);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	/* Dispatch the selected operation case. */
	switch (tag) {
	case HID_LOCAL_USAGE:
		/* Checks the parser state. */
		if (parser->local.usage_count >= HID_REPORT_FIELD_COUNT_MAX)
			return E2BIG;
		span = &parser->local.usages[parser->local.usage_count++];
		span->minimum = usage;
		span->maximum = usage;
		span->is_range = 0;

		/* Reports successful completion. */
		return 0;
	case HID_LOCAL_USAGE_MINIMUM:
		/* Checks the parser state. */
		if (parser->local.range_open)
			return EINVAL;

		/* Checks the parser state. */
		if (parser->local.usage_count >= HID_REPORT_FIELD_COUNT_MAX)
			return E2BIG;
		parser->local.open_range = parser->local.usage_count;
		span = &parser->local.usages[parser->local.usage_count++];
		span->minimum = usage;
		span->maximum = usage;
		span->is_range = 1;
		parser->local.range_open = 1;

		/* Reports successful completion. */
		return 0;
	case HID_LOCAL_USAGE_MAXIMUM:
		/* Checks the parser state. */
		if (!parser->local.range_open)
			return EINVAL;
		span = &parser->local.usages[parser->local.open_range];

		/* Handles the span condition. */
		if ((span->minimum >> 16U) != (usage >> 16U) ||
		    span->minimum > usage)

			/* Returns the computed result. */
			return EINVAL;
		span->maximum = usage;
		parser->local.range_open = 0;

		/* Reports successful completion. */
		return 0;
	default:
		/* Returns the computed result. */
		return EINVAL;
	}
}

/* Supports the parse descriptor operation. */
static int parse_descriptor(struct hid_parser *parser);

/* Supports the parse descriptor operation. */
static int
parse_descriptor(
	struct hid_parser *parser)
{
	uint8_t prefix;
	size_t size;
	unsigned type, tag;
	int error;
	const uint8_t *descriptor = parser->layout->descriptor;
	size_t length = parser->layout->descriptor_size;
	size_t offset = 0;

	/* Process each remaining element. */
	while (offset < length) {
		prefix = descriptor[offset++];

		/* Handles the prefix condition. */
		if (prefix == 0xfeU) {
			/* Checks the current data length. */
			if (length - offset < 2U)
				return EINVAL;
			size = descriptor[offset];
			offset += 2U;

			/* Checks the current data size. */
			if (size > length - offset)
				return EINVAL;
			offset += size;
			continue;
		}
		size = prefix & 0x03U;

		/* Checks the current data size. */
		if (size == 3U)
			size = 4U;

		/* Checks the current data size. */
		if (size > length - offset)
			return EINVAL;
		type = (prefix >> 2U) & 0x03U;
		tag = prefix >> 4U;

		/* Handles the type condition. */
		if (type == HID_ITEM_TYPE_MAIN) {
			error = parse_main(parser, tag, descriptor + offset,
					   size);
		} else if (type == HID_ITEM_TYPE_GLOBAL) {
			error = parse_global(parser, tag, descriptor + offset,
					     size);
		} else if (type == HID_ITEM_TYPE_LOCAL) {
			error = parse_local(parser, tag, descriptor + offset,
					    size);
		} else {
			error = EOPNOTSUPP;
		}

		/* Checks the operation status. */
		if (error != 0)
			return error;
		offset += size;
	}

	/* Checks the parser state. */
	if (parser->collection_depth != 0U || parser->global_depth != 0U)
		return EINVAL;

	/* Checks the parser state. */
	if (parser->local.usage_count != 0U || parser->local.range_open)
		return EINVAL;

	/* Checks the parser state. */
	if (!parser->supported_field_seen)
		return EOPNOTSUPP;

	/* Reports successful completion. */
	return 0;
}

/* Supports the layout allocate operation. */
static struct hid_report_layout *layout_allocate(void);

/* Supports the layout allocate operation. */
static struct hid_report_layout *
layout_allocate(
	void)
{
	struct hid_report_layout *layout;

	layout = kern_calloc(1, sizeof(*layout));

	/* Handles the layout availability. */
	if (layout != NULL) {
		layout->capabilities[0].type = EV_SYN;
		layout->capabilities[0].code = SYN_REPORT;
		layout->capability_count = 1;
	}

	/* Returns the computed result. */
	return layout;
}

/*
 * Implements the drv hid report layout parse operation.
 */
/*
 * Implements the drv hid report layout parse operation.
 */
int
drv_hid_report_layout_parse(
	const void *descriptor,
	size_t length,
	struct hid_report_layout **result)
{
	struct hid_report_layout *layout;
	struct hid_parser *parser;
	int error;

	/* Handles the descriptor availability. */
	if (descriptor == NULL || length == 0U || result == NULL)
		return EINVAL;

	/* Checks the current data length. */
	if (length > HID_REPORT_DESCRIPTOR_SIZE_MAX)
		return E2BIG;
	layout = layout_allocate();

	/* Handles the layout availability. */
	if (layout == NULL)
		return ENOMEM;
	layout->descriptor_size = length;
	layout->profile = HID_LAYOUT_PROFILE_DESCRIPTOR;
	memcpy(layout->descriptor, descriptor, length);
	parser = kern_calloc(1, sizeof(*parser));

	/* Handles the parser availability. */
	if (parser == NULL) {
		kern_free(layout);

		/* Returns the computed result. */
		return ENOMEM;
	}
	parser->layout = layout;
	error = parse_descriptor(parser);
	kern_free(parser);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(layout);

		/* Returns the computed result. */
		return error;
	}
	*result = layout;
	/* Reports successful completion. */
	return 0;
}

/* Supports the boot layout begin operation. */
static int boot_layout_begin(struct hid_report_layout **result, struct hid_report_layout **layout_result, struct hid_report_description **report_result);

/* Supports the boot layout begin operation. */
static int
boot_layout_begin(
	struct hid_report_layout **result,
	struct hid_report_layout **layout_result,
	struct hid_report_description **report_result)
{
	struct hid_report_layout *layout;
	int error;

	/* Handles the result availability. */
	if (result == NULL)
		return EINVAL;
	layout = layout_allocate();

	/* Handles the layout availability. */
	if (layout == NULL)
		return ENOMEM;
	error = add_report(layout, 0, report_result);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(layout);

		/* Returns the computed result. */
		return error;
	}
	*layout_result = layout;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv hid report layout boot keyboard operation.
 */
/*
 * Implements the drv hid report layout boot keyboard operation.
 */
int
drv_hid_report_layout_boot_keyboard(
	struct hid_report_layout **result)
{
	uint16_t usage;
	uint16_t code;
	struct hid_report_layout *layout;
	struct hid_report_description *report;
	struct hid_parser *parser;
	static const uint16_t modifier_usages[] = {
		0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6,
	};
	size_t index;
	int error;

	error = boot_layout_begin(result, &layout, &report);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_KEYBOARD;
	parser = kern_calloc(1, sizeof(*parser));

	/* Handles the parser availability. */
	if (parser == NULL) {
		kern_free(layout);

		/* Returns the computed result. */
		return ENOMEM;
	}
	parser->layout = layout;
	/* Process each remaining element. */
	for (index = 0;
	     index < sizeof(modifier_usages) / sizeof(modifier_usages[0]);
	     index++) {
		usage = modifier_usages[index];
		code = keyboard_code(usage);

		error = add_field(parser, report, (uint32_t)(usage - 0xe0U),
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | usage,
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | usage, 0,
				  1, EV_KEY, code, 1, HID_FIELD_KEY);

		/* Checks the operation status. */
		if (error != 0)
			goto fail;
	}
	error = add_keyboard_array_capabilities(
		layout, HID_USAGE_PAGE_KEYBOARD << 16U,
		(HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	/* Process each remaining element. */
	for (index = 0; index < 6U; index++) {
		error = add_field(parser, report, 16U + (uint32_t)index * 8U,
				  HID_USAGE_PAGE_KEYBOARD << 16U,
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU, 0,
				  255, EV_KEY, KEY_RESERVED, 8,
				  HID_FIELD_KEYBOARD_ARRAY);

		/* Checks the operation status. */
		if (error != 0)
			goto fail;
	}
	report->bit_count = 64U;
	kern_free(parser);
	*result = layout;
	/* Reports successful completion. */
	return 0;
fail:
	kern_free(parser);
	kern_free(layout);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv hid report layout boot mouse operation.
 */
/*
 * Implements the drv hid report layout boot mouse operation.
 */
int
drv_hid_report_layout_boot_mouse(
	struct hid_report_layout **result)
{
	struct hid_report_layout *layout;
	struct hid_report_description *report;
	struct hid_parser *parser;
	unsigned index;
	int error;

	error = boot_layout_begin(result, &layout, &report);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_MOUSE;
	parser = kern_calloc(1, sizeof(*parser));

	/* Handles the parser availability. */
	if (parser == NULL) {
		kern_free(layout);

		/* Returns the computed result. */
		return ENOMEM;
	}
	parser->layout = layout;
	/* Process each remaining element. */
	for (index = 0; index < 3U; index++) {
		error = add_field(parser, report, index,
				  (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
				  (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
				  0, 1, EV_KEY, (uint16_t)(BTN_LEFT + index), 1,
				  HID_FIELD_KEY);

		/* Checks the operation status. */
		if (error != 0)
			goto fail;
	}
	error = add_field(parser, report, 8,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
			  -127, 127, EV_REL, REL_X, 8, HID_FIELD_AXIS);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = add_field(parser, report, 16,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
			  -127, 127, EV_REL, REL_Y, 8, HID_FIELD_AXIS);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	report->bit_count = 24U;
	kern_free(parser);
	*result = layout;
	/* Reports successful completion. */
	return 0;
fail:
	kern_free(parser);
	kern_free(layout);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv hid report layout destroy operation.
 */
/*
 * Implements the drv hid report layout destroy operation.
 */
void
drv_hid_report_layout_destroy(
	struct hid_report_layout *layout)
{
	kern_free(layout);
}

/*
 * Implements the drv hid report layout get info operation.
 */
/*
 * Implements the drv hid report layout get info operation.
 */
int
drv_hid_report_layout_get_info(
	const struct hid_report_layout *layout,
	struct hid_report_layout_info *result)
{
	/* Handles the layout availability. */
	if (layout == NULL || result == NULL)
		return EINVAL;
	result->descriptor_size = layout->descriptor_size;
	result->report_count = layout->report_count;
	result->field_count = layout->field_count;
	result->capability_count = layout->capability_count;
	result->absolute_axis_count = layout->absolute_axis_count;
	result->uses_report_ids = layout->uses_report_ids;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv hid report layout get report operation.
 */
/*
 * Implements the drv hid report layout get report operation.
 */
int
drv_hid_report_layout_get_report(
	const struct hid_report_layout *layout,
	size_t index,
	struct hid_report_report_info *result)
{
	const struct hid_report_description *report;

	/* Handles the layout availability. */
	if (layout == NULL || result == NULL)
		return EINVAL;

	/* Checks the current index. */
	if (index >= layout->report_count)
		return ENOENT;
	report = &layout->reports[index];
	result->report_id = report->id;
	result->minimum_size = (report->bit_count + 7U) / 8U +
			       (layout->uses_report_ids ? 1U : 0U);
	result->field_count = report->field_count;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv hid report layout get capability operation.
 */
/*
 * Implements the drv hid report layout get capability operation.
 */
int
drv_hid_report_layout_get_capability(
	const struct hid_report_layout *layout,
	size_t index,
	struct input_capability *result)
{
	/* Handles the layout availability. */
	if (layout == NULL || result == NULL)
		return EINVAL;

	/* Checks the current index. */
	if (index >= layout->capability_count)
		return ENOENT;
	*result = layout->capabilities[index];
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv hid report layout get absolute axis operation.
 */
/*
 * Implements the drv hid report layout get absolute axis operation.
 */
int
drv_hid_report_layout_get_absolute_axis(
	const struct hid_report_layout *layout,
	size_t index,
	struct input_abs_axis *result)
{
	/* Handles the layout availability. */
	if (layout == NULL || result == NULL)
		return EINVAL;

	/* Checks the current index. */
	if (index >= layout->absolute_axis_count)
		return ENOENT;
	*result = layout->absolute_axes[index];
	/* Reports successful completion. */
	return 0;
}

/* Supports the extract value operation. */
static int extract_value(const uint8_t *data, size_t length, uint32_t bit_offset, uint8_t bit_size, uint32_t *result);

/* Supports the extract value operation. */
static int
extract_value(
	const uint8_t *data,
	size_t length,
	uint32_t bit_offset,
	uint8_t bit_size,
	uint32_t *result)
{
	size_t source_bit;
	uint32_t value = 0;
	unsigned bit;
	size_t available_bits;

	available_bits = length > SIZE_MAX / 8U ? SIZE_MAX : length * 8U;

	/* Handles the bit size condition. */
	if (bit_size == 0U || bit_size > 32U || bit_offset > available_bits ||
	    bit_size > available_bits - bit_offset)

		/* Returns the computed result. */
		return EINVAL;
	/* Process each remaining element. */
	for (bit = 0; bit < bit_size; bit++) {
		source_bit = (size_t)bit_offset + bit;

		/* Handles the data condition. */
		if ((data[source_bit / 8U] &
		     ((uint8_t)1U << (source_bit % 8U))) != 0)
			value |= (uint32_t)1U << bit;
	}
	*result = value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the decode field value operation. */
static int decode_field_value(const struct hid_report_field *field, const uint8_t *data, size_t length, uint32_t *raw_result, int32_t *value_result);

/* Supports the decode field value operation. */
static int
decode_field_value(
	const struct hid_report_field *field,
	const uint8_t *data,
	size_t length,
	uint32_t *raw_result,
	int32_t *value_result)
{
	uint32_t raw;
	int32_t value;
	int error;

	error = extract_value(data, length, field->bit_offset, field->bit_size,
			      &raw);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the field condition. */
	if (field->logical_minimum < 0) {
		value = sign_extend(raw, field->bit_size);
	} else {
		/* Handles the raw condition. */
		if (raw > INT32_MAX)
			return EINVAL;
		value = (int32_t)raw;
	}

	/* Validates the current value. */
	if (value < field->logical_minimum || value > field->logical_maximum)
		return EINVAL;
	*raw_result = raw;
	*value_result = value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the key already present operation. */
static int key_already_present(const struct hid_report_input *input, uint16_t code);

/* Supports the key already present operation. */
static int
key_already_present(
	const struct hid_report_input *input,
	uint16_t code)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < input->value_count; index++) {
		/* Validates the current input. */
		if (input->values[index].type == EV_KEY &&
		    input->values[index].code == code)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the append value operation. */
static int append_value(struct hid_report_input *input, uint16_t type, uint16_t code, int32_t value);

/* Supports the append value operation. */
static int
append_value(
	struct hid_report_input *input,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct hid_report_value *entry;

	/* Checks the key already present result. */
	if (type == EV_KEY && key_already_present(input, code))
		return 0;

	/* Validates the current input. */
	if (input->value_count >= HID_REPORT_VALUE_COUNT_MAX)
		return E2BIG;
	entry = &input->values[input->value_count++];
	entry->type = type;
	entry->code = code;
	entry->value = value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv hid report decode operation.
 */
/*
 * Implements the drv hid report decode operation.
 */
int
drv_hid_report_decode(
	const struct hid_report_layout *layout,
	const void *buffer,
	size_t length,
	struct hid_report_input *result)
{
	const struct hid_report_field *field_local;
	uint32_t raw_local;
	int32_t value_local;
	const struct hid_report_field *field_local1;
	uint32_t raw_local2;
	int32_t value_local3;
	uint32_t usage;
	uint16_t code;
	const struct hid_report_description *report;
	const uint8_t *bytes = buffer, *data;
	size_t payload_length, minimum_length, index;
	uint8_t report_id;
	int keyboard_error = 0;
	int error;

	/* Handles the layout availability. */
	if (layout == NULL || buffer == NULL || result == NULL)
		return EINVAL;

	/* Handles the layout condition. */
	if (layout->uses_report_ids) {
		/* Checks the current data length. */
		if (length == 0U)
			return EINVAL;
		report_id = bytes[0];
		report = find_report_const(layout, report_id);

		/* Handles the report availability. */
		if (report == NULL)
			return EINVAL;
		data = bytes + 1U;
		payload_length = length - 1U;
	} else {
		report_id = 0;
		report = find_report_const(layout, 0);

		/* Handles the report availability. */
		if (report == NULL)
			return EINVAL;
		data = bytes;
		payload_length = length;
	}
	minimum_length = (report->bit_count + 7U) / 8U;

	/* Handles the payload length condition. */
	if (payload_length < minimum_length)
		return EINVAL;

	/* Handles the layout condition. */
	if (layout->profile == HID_LAYOUT_PROFILE_BOOT_KEYBOARD &&
	    data[1] != 0U)

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the report condition. */
	if (report->field_count == 0U)
		return EOPNOTSUPP;

	/* Validate every selected field and recognize keyboard errors first. */
	for (index = 0; index < layout->field_count; index++) {
		field_local = &layout->fields[index];

		/* Handles the field local condition. */
		if (field_local->report_id != report_id)
			continue;
		error = decode_field_value(field_local, data, payload_length,
					   &raw_local, &value_local);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Handles the field local condition. */
		if (field_local->kind == HID_FIELD_KEYBOARD_ARRAY) {
			usage = (field_local->usage_minimum & 0xffff0000U) |
				raw_local;

			/* Handles the raw local condition. */
			if (raw_local > UINT16_MAX ||
			    usage < field_local->usage_minimum ||
			    usage > field_local->usage_maximum)

				/* Returns the computed result. */
				return EINVAL;

			/* Checks the operation status. */
			if ((uint16_t)usage >= HID_USAGE_KEYBOARD_ERROR_MIN &&
			    (uint16_t)usage <= HID_USAGE_KEYBOARD_ERROR_MAX)
				keyboard_error = 1;
		}
	}
	memset(result, 0, sizeof(*result));
	result->report_id = report_id;
	result->keyboard_error = (uint8_t)keyboard_error;
	/* Process each remaining element. */
	for (index = 0; index < layout->field_count; index++) {
		field_local1 = &layout->fields[index];

		/* Handles the field local1 condition. */
		if (field_local1->report_id != report_id)
			continue;
		error = decode_field_value(field_local1, data, payload_length,
					   &raw_local2, &value_local3);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/*
		 * A keyboard error usage invalidates the complete keyboard
		 * state, including modifier variables.  Non-keyboard fields in
		 * a composite report may still be returned.
		 */
		if (result->keyboard_error && (field_local1->usage_minimum >>
					       16U) == HID_USAGE_PAGE_KEYBOARD)
			continue;

		/* Handles the field local1 condition. */
		if (field_local1->kind == HID_FIELD_KEYBOARD_ARRAY) {
			/* Handles the raw local2 condition. */
			if (raw_local2 == 0U)
				continue;
			code = keyboard_code((uint16_t)raw_local2);

			/* Handles the code condition. */
			if (code == KEY_RESERVED)
				continue;
			error = append_value(result, EV_KEY, code, 1);
		} else if (field_local1->kind == HID_FIELD_KEY) {
			/* Handles the value local3 condition. */
			if (value_local3 == 0)
				continue;
			error = append_value(result, EV_KEY, field_local1->code,
					     1);
		} else {
			error = append_value(result, field_local1->type,
					     field_local1->code, value_local3);
		}

		/* Checks the operation status. */
		if (error != 0) {
			memset(result, 0, sizeof(*result));

			/* Returns the computed result. */
			return error;
		}
	}

	/* Reports successful completion. */
	return 0;
}
/* End consolidated hid-report.c. */

/* Begin consolidated usb-hid.c. */
/*
 * USB Human Interface Device input driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/hid/hid-report.h>
#include <drivers/usb-hid.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/input-device.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define USB_HID_CLASS 0x03U
#define USB_HID_DESCRIPTOR 0x21U
#define USB_HID_REPORT_DESCRIPTOR 0x22U
#define USB_REQUEST_GET_DESCRIPTOR 0x06U
#define USB_HID_REQUEST_SET_PROTOCOL 0x0bU
#define USB_HID_PROTOCOL_REPORT 1U
#define USB_HID_CONTROL_TIMEOUT_MS 1000U
#define USB_HID_DRAIN_TIMEOUT_MS 5000U
/* drv_input_device_register() accepts at most 63 bytes plus NUL. */
#define USB_HID_TEXT_MAX 64U
#define USB_HID_ERROR_MARKERS 16U
#define USB_HID_WORK_ARM (1U << 0)
#define USB_HID_WORK_COMPLETE (1U << 1)

struct usb_hid_report_state {
	uint8_t id;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
};

struct usb_hid {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	struct hid_report_layout *layout;
	struct input_device *input;
	struct thread *worker;
	struct spinlock lock;
	struct usb_hid *pending_next;
	uint8_t *buffer;
	size_t buffer_size;
	struct input_capability capabilities[HID_REPORT_FIELD_COUNT_MAX + 1U];
	struct input_abs_axis absolute_axes[ABS_MAX + 1U];
	struct usb_hid_report_state reports[HID_REPORT_ID_COUNT_MAX];
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	size_t capability_count;
	size_t absolute_axis_count;
	size_t report_count;
	unsigned work_pending;
	unsigned stopping;
	unsigned submit_active;
	unsigned activating;
	unsigned active;
	unsigned pending;
	unsigned error_markers;
	char name[USB_HID_TEXT_MAX];
	char physical_path[USB_HID_TEXT_MAX];
	char unique_id[USB_HID_TEXT_MAX];
};

static struct spinlock usb_hid_pending_lock;
static struct usb_hid *usb_hid_pending;
static unsigned usb_hid_input_is_ready;
static unsigned usb_hid_registered;

static uint16_t usb_hid_le16(const uint8_t *bytes);

/* Supports the usb hid le16 operation. */
static uint16_t
usb_hid_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static int usb_hid_report_descriptor_length(struct drv_usb_interface *interface, size_t *result);

/* Supports the usb hid report descriptor length operation. */
static int
usb_hid_report_descriptor_length(
	struct drv_usb_interface *interface,
	size_t *result)
{
	const uint8_t *subordinate;
	size_t report_length;
	const uint8_t *descriptor;
	size_t length, entries, entry;
	int error;
	const struct drv_usb_host_interface *alternate;
	unsigned count, index;
	size_t found = 0;

	alternate = drv_usb_interface_active_alternate(interface);

	/* Handles the alternate availability. */
	if (alternate == NULL || result == NULL)
		return EINVAL;
	count = drv_usb_host_interface_extra_count(alternate);
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		error = drv_usb_host_interface_extra(
			alternate, index, (const void **)&descriptor, &length);

		/* Checks the operation status. */
		if (error != 0)
			return error;

		/* Checks the current data length. */
		if (length < 2U || descriptor[0] != length ||
		    descriptor[1] != USB_HID_DESCRIPTOR)
			continue;

		/* Checks the current data length. */
		if (length < 6U)
			return EINVAL;
		entries = descriptor[5];

		/* Handles the entries condition. */
		if (entries == 0U || entries > (length - 6U) / 3U ||
		    6U + entries * 3U != length)

			/* Returns the computed result. */
			return EINVAL;
		/* Process each element required by the operation. */
		for (entry = 0; entry < entries; entry++) {
			subordinate = descriptor + 6U + entry * 3U;

			/* Handles the subordinate condition. */
			if (subordinate[0] != USB_HID_REPORT_DESCRIPTOR)
				continue;
			report_length = usb_hid_le16(subordinate + 1U);

			/* Handles the report length condition. */
			if (report_length == 0U ||
			    report_length > HID_REPORT_DESCRIPTOR_SIZE_MAX ||
			    found != 0U)

				/* Returns the computed result. */
				return EINVAL;
			found = report_length;
		}
	}

	/* Handles the found condition. */
	if (found == 0U)
		return ENOENT;
	*result = found;
	/* Reports successful completion. */
	return 0;
}

static int usb_hid_endpoint_capacity(struct drv_usb_interface *interface, struct drv_usb_endpoint *endpoint, size_t *result);

/* Supports the usb hid endpoint capacity operation. */
static int
usb_hid_endpoint_capacity(
	struct drv_usb_interface *interface,
	struct drv_usb_endpoint *endpoint,
	size_t *result)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	const struct drv_usb_superspeed_endpoint_companion_descriptor
		*companion;
	enum drv_usb_speed speed;
	uint16_t maximum;
	unsigned payload, packets;
	size_t capacity;

	descriptor = drv_usb_endpoint_descriptor(endpoint);

	/* Handles the descriptor availability. */
	if (descriptor == NULL || descriptor->interval == 0U || result == NULL)
		return EINVAL;
	maximum = drv_usb_endpoint_max_packet_size(endpoint);
	payload = maximum & 0x07ffU;
	packets = 1U + ((maximum >> 11U) & 3U);
	speed = drv_usb_device_speed(drv_usb_interface_device(interface));

	/* Handles the payload condition. */
	if (payload == 0U || (maximum & 0xe000U) != 0U || packets == 4U)
		return EINVAL;

	/* Handles the speed condition. */
	if (speed == DRV_USB_SPEED_LOW) {
		/* Handles the payload condition. */
		if (payload > 8U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_FULL) {
		/* Handles the payload condition. */
		if (payload > 64U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_HIGH) {
		/* Handles the payload condition. */
		if (payload > 1024U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_SUPER ||
		   speed == DRV_USB_SPEED_SUPER_PLUS) {
		/* Handles the payload condition. */
		if (payload > 1024U || packets != 1U)
			return EINVAL;
		companion = drv_usb_endpoint_superspeed_companion(endpoint);
		capacity =
			(size_t)payload *
			((size_t)drv_usb_endpoint_maximum_burst(endpoint) + 1U);

		/* Handles the companion availability. */
		if (companion != NULL && companion->bytes_per_interval != 0U) {
			/* Handles the companion condition. */
			if (companion->bytes_per_interval > capacity)
				return EINVAL;
			capacity = companion->bytes_per_interval;
		}
		*result = capacity;
		/* Reports successful completion. */
		return 0;
	} else {
		/* Returns the computed result. */
		return EINVAL;
	}
	*result = (size_t)payload * packets;
	/* Reports successful completion. */
	return 0;
}

static int usb_hid_find_endpoint(struct drv_usb_interface *interface, struct drv_usb_endpoint **result);

/* Supports the usb hid find endpoint operation. */
static int
usb_hid_find_endpoint(
	struct drv_usb_interface *interface,
	struct drv_usb_endpoint **result)
{
	struct drv_usb_endpoint *endpoint, *extra;

	endpoint = drv_usb_interface_find_endpoint(
		interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);

	/* Handles the endpoint availability. */
	if (endpoint == NULL)
		return ENODEV;
	extra = drv_usb_interface_find_endpoint(interface,
						DRV_USB_TRANSFER_INTERRUPT,
						DRV_USB_DIR_IN, endpoint);

	/* Handles the extra availability. */
	if (extra != NULL)
		return EOPNOTSUPP;
	*result = endpoint;
	/* Reports successful completion. */
	return 0;
}

static int usb_hid_fetch_layout(struct usb_hid *hid);

/* Supports the usb hid fetch layout operation. */
static int
usb_hid_fetch_layout(
	struct usb_hid *hid)
{
	struct hid_report_report_info report;
	struct hid_report_layout_info info;
	uint8_t *descriptor;
	size_t descriptor_length, actual = 0, index, capacity;
	size_t maximum_report = 0;
	int error;

	error = usb_hid_report_descriptor_length(hid->interface,
						 &descriptor_length);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	descriptor = hal_malloc(descriptor_length);

	/* Handles the descriptor availability. */
	if (descriptor == NULL)
		return ENOMEM;
	error = drv_usb_control(
		hid->device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_INTERFACE,
		USB_REQUEST_GET_DESCRIPTOR,
		(uint16_t)(USB_HID_REPORT_DESCRIPTOR << 8U),
		(uint16_t)drv_usb_interface_number(hid->interface), descriptor,
		descriptor_length, USB_HID_CONTROL_TIMEOUT_MS, &actual);

	/* Checks the operation status. */
	if (error == 0 && actual != descriptor_length)
		error = EIO;

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_hid_report_layout_parse(
			descriptor, descriptor_length, &hid->layout);
	}
	hal_free(descriptor);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_hid_report_layout_get_info(hid->layout, &info);

	/* Checks the operation status. */
	if (error != 0 || info.report_count == 0U ||
	    info.report_count > HID_REPORT_ID_COUNT_MAX ||
	    info.capability_count > HID_REPORT_FIELD_COUNT_MAX + 1U ||
	    info.absolute_axis_count > ABS_MAX + 1U)

		/* Returns the computed result. */
		return error != 0 ? error : EINVAL;
	hid->report_count = info.report_count;
	hid->capability_count = info.capability_count;
	hid->absolute_axis_count = info.absolute_axis_count;
	/* Process each remaining element. */
	for (index = 0; index < info.report_count; index++) {
		error = drv_hid_report_layout_get_report(hid->layout, index,
							 &report);

		/* Checks the operation status. */
		if (error != 0 || report.minimum_size == 0U ||
		    report.minimum_size > HID_REPORT_BITS_MAX / 8U + 1U)

			/* Returns the computed result. */
			return error != 0 ? error : EINVAL;
		hid->reports[index].id = report.report_id;

		/* Handles the report condition. */
		if (report.minimum_size > maximum_report)
			maximum_report = report.minimum_size;
	}
	/* Process each remaining element. */
	for (index = 0; index < info.capability_count; index++) {
		error = drv_hid_report_layout_get_capability(
			hid->layout, index, &hid->capabilities[index]);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	/* Process each remaining element. */
	for (index = 0; index < info.absolute_axis_count; index++) {
		error = drv_hid_report_layout_get_absolute_axis(
			hid->layout, index, &hid->absolute_axes[index]);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	error = usb_hid_endpoint_capacity(hid->interface, hid->endpoint,
					  &capacity);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the maximum report condition. */
	if (maximum_report > capacity)
		return EOVERFLOW;
	hid->buffer_size = maximum_report;

	/* Reports successful completion. */
	return 0;
}

static int usb_hid_set_report_protocol(struct usb_hid *hid);

/* Supports the usb hid set report protocol operation. */
static int
usb_hid_set_report_protocol(
	struct usb_hid *hid)
{
	int function_result;
	const struct drv_usb_interface_descriptor *descriptor;
	size_t actual = 0;

	descriptor = drv_usb_interface_descriptor(hid->interface);

	/* Handles the descriptor availability. */
	if (descriptor == NULL)
		return EINVAL;

	/* Non-Boot interfaces already have exactly one Report Protocol. */
	if (descriptor->interface_subclass != 1U)
		return 0;

	/* Obtains the drv usb control result. */
	function_result = drv_usb_control(
		hid->device,
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
			DRV_USB_RECIP_INTERFACE,
		USB_HID_REQUEST_SET_PROTOCOL, USB_HID_PROTOCOL_REPORT,
		(uint16_t)drv_usb_interface_number(hid->interface), NULL, 0,
		USB_HID_CONTROL_TIMEOUT_MS, &actual);

	/* Returns the computed result. */
	return function_result;
}

static int usb_hid_has_capability(const struct usb_hid *hid, uint16_t type, uint16_t code);

/* Supports the usb hid has capability operation. */
static int
usb_hid_has_capability(
	const struct usb_hid *hid,
	uint16_t type,
	uint16_t code)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < hid->capability_count; index++) {
		/* Handles the hid condition. */
		if (hid->capabilities[index].type == type &&
		    hid->capabilities[index].code == code)

			/* Reports operation failure. */
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

static void usb_hid_identity(struct usb_hid *hid);

/* Supports the usb hid identity operation. */
static void
usb_hid_identity(
	struct usb_hid *hid)
{
	const struct drv_usb_device_descriptor *descriptor;
	unsigned bus, address, port, interface_number;
	int error;

	/* Renders the topology as the physical path of the device. */
	descriptor = drv_usb_device_descriptor(hid->device);
	bus = drv_usb_bus_number(drv_usb_device_bus(hid->device));
	address = drv_usb_device_address(hid->device);
	port = drv_usb_device_port(hid->device);
	interface_number = drv_usb_interface_number(hid->interface);
	(void)snprintf(hid->physical_path, sizeof(hid->physical_path),
		       "usb%u/port%u/device%u/interface%u", bus, port, address,
		       interface_number);
	hid->unique_id[0] = '\0';

	/* Checks the file descriptor. */
	if (descriptor->serial_string != 0U) {
		(void)drv_usb_device_get_string(
			hid->device, descriptor->serial_string, 0,
			hid->unique_id, sizeof(hid->unique_id));
	}
	hid->name[0] = '\0';
	error = descriptor->product_string == 0U
			? ENOENT
			: drv_usb_device_get_string(
				  hid->device, descriptor->product_string, 0,
				  hid->name, sizeof(hid->name));

	/* Checks the operation status. */
	if (error == 0 && hid->name[0] != '\0')
		return;

	/* Handles the usb hid has capability condition. */
	if (usb_hid_has_capability(hid, EV_ABS, ABS_X))
		(void)snprintf(hid->name, sizeof(hid->name), "USB HID tablet");
	else if (usb_hid_has_capability(hid, EV_REL, REL_X))
		(void)snprintf(hid->name, sizeof(hid->name), "USB HID mouse");
	else
		(void)snprintf(hid->name, sizeof(hid->name),
			       "USB HID keyboard");
}

static void usb_hid_completion(struct drv_usb_urb *urb, void *argument);

/* Supports the usb hid completion operation. */
static void
usb_hid_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_hid *hid = argument;
	struct thread *worker;
	unsigned long irq;

	/* Handles the hid availability. */
	if (hid == NULL || urb != hid->urb)
		return;
	irq = spin_lock_irqsave(&hid->lock);
	hid->work_pending |= USB_HID_WORK_COMPLETE;
	worker = hid->worker;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Handles the worker availability. */
	if (worker != NULL)
		kernel_notify_task(worker->task);
}

static int usb_hid_begin_submit(struct usb_hid *hid);

/* Supports the usb hid begin submit operation. */
static int
usb_hid_begin_submit(
	struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	int admitted = !hid->stopping && !hid->submit_active;

	/* Handles the admitted condition. */
	if (admitted)
		hid->submit_active = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Returns the computed result. */
	return admitted ? 0 : EBUSY;
}

static void usb_hid_end_submit(struct usb_hid *hid);

/* Supports the usb hid end submit operation. */
static void
usb_hid_end_submit(
	struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);

	/* Handles the hid condition. */
	if (!hid->submit_active)
		__builtin_trap();
	hid->submit_active = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);
}

static int usb_hid_arm(struct usb_hid *hid);

/* Supports the usb hid arm operation. */
static int
usb_hid_arm(
	struct usb_hid *hid)
{
	int error;

	error = usb_hid_begin_submit(hid);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	memset(hid->buffer, 0, hid->buffer_size);
	error = drv_usb_urb_setup(hid->urb, hid->buffer, hid->buffer_size,
				  DRV_USB_URB_SHORT_OK, 0, usb_hid_completion,
				  hid);

	/* Checks the operation status. */
	if (error == 0)
		error = drv_usb_urb_submit(hid->urb);
	usb_hid_end_submit(hid);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static struct usb_hid_report_state *usb_hid_report_state(struct usb_hid *hid, uint8_t report_id);

/* Supports the usb hid report state operation. */
static struct usb_hid_report_state *
usb_hid_report_state(
	struct usb_hid *hid,
	uint8_t report_id)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < hid->report_count; index++) {
		/* Handles the hid condition. */
		if (hid->reports[index].id == report_id)
			return &hid->reports[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

static void usb_hid_publish_report(struct usb_hid *hid, const uint8_t *buffer, size_t length);

/* Supports the usb hid publish report operation. */
static void
usb_hid_publish_report(
	struct usb_hid *hid,
	const uint8_t *buffer,
	size_t length)
{
	const struct hid_report_value *value_local;
	const struct hid_report_value *value_local1;
	size_t word;
	unsigned long bit;
	int old_value;
	int new_value;
	struct hid_report_input decoded;
	struct usb_hid_report_state *state;
	unsigned long current[INPUT_BIT_WORDS(KEY_MAX)];
	unsigned long aggregate[INPUT_BIT_WORDS(KEY_MAX)];
	size_t index;
	unsigned code;
	int error, emitted = 0;

	error = drv_hid_report_decode(hid->layout, buffer, length, &decoded);

	/* Checks the operation status. */
	if (error != 0) {
		/* Checks the operation status. */
		if (hid->error_markers++ < USB_HID_ERROR_MARKERS) {
			hal_printf("usb-hid: malformed input usb%u device=%u "
				   "interface=%u length=%u error=%d\n",
				   drv_usb_bus_number(
					   drv_usb_device_bus(hid->device)),
				   drv_usb_device_address(hid->device),
				   drv_usb_interface_number(hid->interface),
				   (unsigned)length, error);
		}

		/* Returns the computed result. */
		return;
	}
	state = usb_hid_report_state(hid, decoded.report_id);

	/* Handles the state availability. */
	if (state == NULL)
		return;
	memset(current, 0, sizeof(current));
	/* Process each remaining element. */
	for (index = 0; index < decoded.value_count; index++) {
		value_local = &decoded.values[index];

		/* Handles the value local condition. */
		if (value_local->type == EV_KEY &&
		    value_local->code <= KEY_MAX) {
			current[value_local->code / INPUT_BITS_PER_WORD] |=
				1UL
				<< (value_local->code % INPUT_BITS_PER_WORD);
		}
	}

	/* Checks the operation status. */
	if (!decoded.keyboard_error) {
		memcpy(state->held, current, sizeof(state->held));
		memset(aggregate, 0, sizeof(aggregate));
		/* Process each remaining element. */
		for (index = 0; index < hid->report_count; index++) {
			/* Process each element required by the operation. */
			for (word = 0; word < INPUT_BIT_WORDS(KEY_MAX);
			     word++) {
				aggregate[word] |=
					hid->reports[index].held[word];
			}
		}
		/* Process each element required by the operation. */
		for (code = 0; code <= KEY_MAX; code++) {
			bit = 1UL << (code % INPUT_BITS_PER_WORD);
			old_value = (hid->held[code / INPUT_BITS_PER_WORD] &
				     bit) != 0;
			new_value = (aggregate[code / INPUT_BITS_PER_WORD] &
				     bit) != 0;

			/* Handles the old value condition. */
			if (old_value == new_value)
				continue;
			drv_input_device_emit(hid->input, EV_KEY,
					      (uint16_t)code, new_value);
			emitted = 1;
		}
		memcpy(hid->held, aggregate, sizeof(hid->held));
	}
	/* Process each remaining element. */
	for (index = 0; index < decoded.value_count; index++) {
		value_local1 = &decoded.values[index];

		/* Handles the value local1 condition. */
		if (value_local1->type == EV_KEY ||
		    (value_local1->type == EV_REL && value_local1->value == 0))
			continue;
		drv_input_device_emit(hid->input, value_local1->type,
				      value_local1->code, value_local1->value);
		emitted = 1;
	}

	/* Handles the emitted condition. */
	if (emitted)
		drv_input_device_emit(hid->input, EV_SYN, SYN_REPORT, 0);
}

static unsigned usb_hid_take_work(struct usb_hid *hid, int *stopping);

/* Supports the usb hid take work operation. */
static unsigned
usb_hid_take_work(
	struct usb_hid *hid,
	int *stopping)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	unsigned work = hid->work_pending;

	hid->work_pending = 0U;
	*stopping = hid->stopping != 0U;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Returns the computed result. */
	return work;
}

static void usb_hid_unpublish(struct usb_hid *hid);

/* Supports the usb hid unpublish operation. */
static void
usb_hid_unpublish(
	struct usb_hid *hid)
{
	struct input_device *input;
	unsigned long irq;

	irq = spin_lock_irqsave(&hid->lock);
	input = hid->input;
	hid->input = NULL;
	hid->active = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Handles the input availability. */
	if (input != NULL)
		drv_input_device_unregister(input);
}

static void usb_hid_runtime_stop(struct usb_hid *hid, const char *stage, int error, int transfer_status);

/* Supports the usb hid runtime stop operation. */
static void
usb_hid_runtime_stop(
	struct usb_hid *hid,
	const char *stage,
	int error,
	int transfer_status)
{
	unsigned long irq;
	int report;

	irq = spin_lock_irqsave(&hid->lock);
	hid->stopping = 1U;
	report = hid->error_markers++ < USB_HID_ERROR_MARKERS;
	spin_unlock_irqrestore(&hid->lock, irq);

	/*
 * Remove a device which cannot be rearmed instead of leaving a visible
	 * event node that can never produce another report.  Detach joins this
	 * worker before attempting the same idempotent unpublication. */
	usb_hid_unpublish(hid);

	/* Handles the report condition. */
	if (report) {
		/* Handles the transfer status condition. */
		if (transfer_status) {
			hal_printf(
				"usb-hid: terminal transfer stopped "
				"interface=%u status=%d; input unpublished\n",
				drv_usb_interface_number(hid->interface),
				error);
		} else {
			hal_printf("usb-hid: %s failed interface=%u error=%d; "
				   "input unpublished\n",
				   stage,
				   drv_usb_interface_number(hid->interface),
				   error);
		}
	}
}

static void usb_hid_worker(void *argument);

/* Supports the usb hid worker operation. */
static void
usb_hid_worker(
	void *argument)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	int stopping_now;
	unsigned work;
	int error, stopping;
	struct usb_hid *hid = argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		work = usb_hid_take_work(hid, &stopping);

		/* Handles the stopping condition. */
		if (stopping)
			return;

		/* Handles the work condition. */
		if (work == 0U) {
			kernel_wait_task();
			continue;
		}

		/* Handles the work condition. */
		if ((work & USB_HID_WORK_COMPLETE) != 0U) {
			error = drv_usb_urb_drain(hid->urb,
						  USB_HID_DRAIN_TIMEOUT_MS);

			/* Checks the operation status. */
			if (error != 0) {
				usb_hid_runtime_stop(hid, "completion drain",
						     error, 0);

				/* Returns the computed result. */
				return;
			}
			status = drv_usb_urb_status(hid->urb);

			/* Checks the operation status. */
			if (status == DRV_USB_URB_COMPLETE) {
				usb_hid_publish_report(
					hid, hid->buffer,
					drv_usb_urb_actual_length(hid->urb));
				work |= USB_HID_WORK_ARM;
			} else if (status == DRV_USB_URB_STALL) {
				error = drv_usb_endpoint_clear_halt(
					hid->endpoint);

				/* Checks the operation status. */
				if (error == 0) {
					work |= USB_HID_WORK_ARM;
				} else {
					usb_hid_runtime_stop(hid, "clear-halt",
							     error, 0);

					/* Returns the computed result. */
					return;
				}
			} else if (status != DRV_USB_URB_CANCELLED &&
				   status != DRV_USB_URB_DISCONNECTED) {
				usb_hid_runtime_stop(hid, "terminal transfer",
						     (int)status, 1);

				/* Returns the computed result. */
				return;
			}
		}

		/* Handles the work condition. */
		if ((work & USB_HID_WORK_ARM) != 0U) {
			error = usb_hid_arm(hid);

			/* Checks the operation status. */
			if (error != 0) {
				/*
 * EBUSY is expected only after detach closes
				 * admission.  In that case detach owns
				 * publication; any other EBUSY is still a
				 * terminal always-on-URB contract failure. */
				irq = spin_lock_irqsave(&hid->lock);
				stopping_now = hid->stopping != 0U;

				spin_unlock_irqrestore(&hid->lock, irq);

				/* Handles the stopping now condition. */
				if (!stopping_now) {
					usb_hid_runtime_stop(hid, "rearm",
							     error, 0);
				}

				/* Returns the computed result. */
				return;
			}
		}
	}
}

static int usb_hid_join_worker(struct usb_hid *hid);

/* Supports the usb hid join worker operation. */
static int
usb_hid_join_worker(
	struct usb_hid *hid)
{
	struct thread *worker = hid->worker;
	int error;

	/* Handles the worker availability. */
	if (worker == NULL)
		return 0;

	/* Handles the worker condition. */
	if (worker == curthread)
		return EBUSY;
	kernel_notify_task(worker->task);
	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	       THREAD_ZOMBIE)
		sched_yield();
	error = thread_wait(worker, NULL);

	/* Checks the operation status. */
	if (error == 0)
		hid->worker = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static void usb_hid_close_admission(struct usb_hid *hid);

/* Supports the usb hid close admission operation. */
static void
usb_hid_close_admission(
	struct usb_hid *hid)
{
	unsigned long irq;
	unsigned active;

	irq = spin_lock_irqsave(&hid->lock);
	hid->stopping = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Handles the worker availability. */
	if (hid->worker != NULL)
		kernel_notify_task(hid->worker->task);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&hid->lock);
		active = hid->submit_active || hid->activating;
		spin_unlock_irqrestore(&hid->lock, irq);

		/* Handles the active condition. */
		if (!active)
			return;
		sched_yield();
	}
}

static int usb_hid_activate(struct usb_hid *hid, int activation_claimed);

/* Supports the usb hid activate operation. */
static int
usb_hid_activate(
	struct usb_hid *hid,
	int activation_claimed)
{
	const struct drv_usb_device_descriptor *usb_descriptor;
	struct input_device_info info;
	struct thread *worker;
	unsigned long irq;
	int error;

	/* Handles the activation claimed condition. */
	if (!activation_claimed) {
		irq = spin_lock_irqsave(&hid->lock);

		/* Handles the hid condition. */
		if (hid->stopping || hid->active || hid->activating) {
			spin_unlock_irqrestore(&hid->lock, irq);

			/* Returns the computed result. */
			return hid->active ? 0 : EBUSY;
		}
		hid->activating = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
	}
	error = kthread_create(usb_hid_worker, hid, SCHED_PRIORITY_DEFAULT,
			       &worker);

	/* Checks the operation status. */
	if (error != 0)
		goto out;
	hid->worker = worker;
	usb_descriptor = drv_usb_device_descriptor(hid->device);
	memset(&info, 0, sizeof(info));
	info.name = hid->name;
	info.physical_path = hid->physical_path;
	info.unique_id = hid->unique_id;
	info.id.bustype = BUS_USB;
	info.id.vendor = usb_descriptor->vendor;
	info.id.product = usb_descriptor->product;
	info.id.version = usb_descriptor->device_release;
	info.capabilities = hid->capabilities;
	info.capability_count = hid->capability_count;
	info.absolute_axes = hid->absolute_axes;
	info.absolute_axis_count = hid->absolute_axis_count;
	error = drv_input_device_register(&info, &hid->input);

	/* Checks the operation status. */
	if (error != 0) {
		irq = spin_lock_irqsave(&hid->lock);
		hid->stopping = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
		thread_start(worker);
		(void)usb_hid_join_worker(hid);
		goto out;
	}

	/*
 * The first accepted request is part of the attach transaction.  A
	 * publication which can never receive a report is not a successful HID
	 * attachment.  Synchronous completion is safe: its callback only
	 * records work for the worker which is started below. */
	error = usb_hid_arm(hid);

	/* Checks the operation status. */
	if (error != 0) {
		usb_hid_unpublish(hid);
		irq = spin_lock_irqsave(&hid->lock);
		hid->stopping = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
		thread_start(worker);
		(void)usb_hid_join_worker(hid);
		goto out;
	}
	irq = spin_lock_irqsave(&hid->lock);
	hid->active = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);
	thread_start(worker);
	hal_printf("usb-hid: event device usb%u device=%u interface=%u "
		   "endpoint=%02x report-bytes=%u\n",
		   drv_usb_bus_number(drv_usb_device_bus(hid->device)),
		   drv_usb_device_address(hid->device),
		   drv_usb_interface_number(hid->interface),
		   drv_usb_endpoint_address(hid->endpoint),
		   (unsigned)hid->buffer_size);

out:
	irq = spin_lock_irqsave(&hid->lock);
	hid->activating = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static void usb_hid_pending_remove(struct usb_hid *hid);

/* Supports the usb hid pending remove operation. */
static void
usb_hid_pending_remove(
	struct usb_hid *hid)
{
	struct usb_hid **link;
	unsigned long irq = spin_lock_irqsave(&usb_hid_pending_lock);

	/* Handles the hid condition. */
	if (hid->pending) {
		/* Process each element required by the operation. */
		for (link = &usb_hid_pending; *link != NULL;
		     link = &(*link)->pending_next) {
			/* Handles the link condition. */
			if (*link == hid) {
				*link = hid->pending_next;
				break;
			}
		}
		hid->pending = 0U;
		hid->pending_next = NULL;
	}
	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);
}

static int usb_hid_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);

/* Supports the usb hid attach operation. */
static int
usb_hid_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *interface_descriptor;
	struct usb_hid *hid;
	unsigned long irq;
	int error, ready;

	(void)id;
	interface_descriptor = drv_usb_interface_descriptor(interface);

	/* Handles the interface descriptor availability. */
	if (interface_descriptor == NULL ||
	    interface_descriptor->interface_class != USB_HID_CLASS)

		/* Returns the computed result. */
		return ENODEV;
	hid = hal_malloc(sizeof(*hid));

	/* Handles the hid availability. */
	if (hid == NULL)
		return ENOMEM;
	memset(hid, 0, sizeof(*hid));
	hid->interface = interface;
	hid->device = drv_usb_interface_device(interface);
	spin_init(&hid->lock, LOCK_RANK_DEVICE, "usb hid");
	error = usb_hid_find_endpoint(interface, &hid->endpoint);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	error = usb_hid_fetch_layout(hid);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;

	/*
 * Report Protocol is a checked publication prerequisite.  There is no
	 * Boot-Protocol fallback for malformed or unsupported devices. */
	error = usb_hid_set_report_protocol(hid);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	usb_hid_identity(hid);
	hid->buffer = hal_malloc(hid->buffer_size);

	/* Handles the buffer availability. */
	if (hid->buffer == NULL) {
		error = ENOMEM;
		goto fail;
	}
	hid->urb = drv_usb_urb_alloc(hid->device, hid->endpoint, 0);

	/* Handles the urb availability. */
	if (hid->urb == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = drv_usb_interface_set_driver_data(interface, hid);

	/* Checks the operation status. */
	if (error != 0)
		goto fail;
	irq = spin_lock_irqsave(&usb_hid_pending_lock);
	ready = usb_hid_input_is_ready != 0U;

	/* Handles the ready condition. */
	if (!ready) {
		hid->pending = 1U;
		hid->pending_next = usb_hid_pending;
		usb_hid_pending = hid;
	}
	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);

	/* Handles the ready condition. */
	if (ready) {
		error = usb_hid_activate(hid, 0);

		/* Checks the operation status. */
		if (error != 0) {
			(void)drv_usb_interface_set_driver_data(interface,
								NULL);
			goto fail;
		}
	}

	/* Reports successful completion. */
	return 0;

fail:

	/* Handles the urb availability. */
	if (hid->urb != NULL)
		drv_usb_urb_free(hid->urb);

	/* Handles the buffer availability. */
	if (hid->buffer != NULL)
		hal_free(hid->buffer);

	/* Handles the layout availability. */
	if (hid->layout != NULL)
		drv_hid_report_layout_destroy(hid->layout);
	hal_free(hid);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int usb_hid_detach(struct drv_usb_interface *interface, unsigned flags);

/* Supports the usb hid detach operation. */
static int
usb_hid_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_hid *hid = drv_usb_interface_driver_data(interface);
	enum drv_usb_urb_status status;
	int drain_error = 0, join_error;

	(void)flags;

	/* Handles the hid availability. */
	if (hid == NULL)
		return 0;
	usb_hid_pending_remove(hid);
	usb_hid_close_admission(hid);

	/* Handles the urb availability. */
	if (hid->urb != NULL) {
		status = drv_usb_urb_status(hid->urb);

		/* Checks the operation status. */
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(hid->urb);
		drain_error =
			drv_usb_urb_drain(hid->urb, USB_HID_DRAIN_TIMEOUT_MS);
	}
	join_error = usb_hid_join_worker(hid);

	/* Checks the operation status. */
	if (drain_error != 0 || join_error != 0)
		return drain_error != 0 ? drain_error : join_error;

	/*
 * drv_input_device_unregister performs the one terminal held-key/button
	 * release before it detaches the old event generation. */
	usb_hid_unpublish(hid);
	(void)drv_usb_interface_set_driver_data(interface, NULL);

	/* Handles the urb availability. */
	if (hid->urb != NULL)
		drv_usb_urb_free(hid->urb);

	/* Handles the buffer availability. */
	if (hid->buffer != NULL)
		hal_free(hid->buffer);

	/* Handles the layout availability. */
	if (hid->layout != NULL)
		drv_hid_report_layout_destroy(hid->layout);
	hal_free(hid);

	/* Reports successful completion. */
	return 0;
}

static int usb_hid_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);

/* Supports the usb hid match operation. */
static int
usb_hid_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *endpoint;
	size_t descriptor_length, capacity;

	(void)id;

	/* Checks the usb hid report descriptor length result. */
	if (usb_hid_report_descriptor_length(interface, &descriptor_length) !=
		    0 ||
	    usb_hid_find_endpoint(interface, &endpoint) != 0 ||
	    usb_hid_endpoint_capacity(interface, endpoint, &capacity) != 0)

		/* Reports successful completion. */
		return 0;

	/* Returns the computed result. */
	return descriptor_length != 0U && capacity != 0U ? 100 : 0;
}

static const struct drv_usb_id usb_hid_ids[] = {
	{.match_flags = DRV_USB_ID_IF_CLASS, .interface_class = USB_HID_CLASS}};

static struct drv_usb_driver usb_hid_driver = {
	.name = "usb-hid",
	.ids = usb_hid_ids,
	.id_count = sizeof(usb_hid_ids) / sizeof(usb_hid_ids[0]),
	.match = usb_hid_match,
	.attach = usb_hid_attach,
	.detach = usb_hid_detach};

/*
 * Implements the drv usb hid driver register operation.
 */
int
drv_usb_hid_driver_register(
	void)
{
	int error;

	/* Handles the usb hid registered condition. */
	if (usb_hid_registered)
		return EALREADY;
	spin_init(&usb_hid_pending_lock, LOCK_RANK_DEVICE, "usb hid pending");
	usb_hid_pending = NULL;
	usb_hid_input_is_ready = 0U;
	error = drv_usb_driver_register(&usb_hid_driver);

	/* Checks the operation status. */
	if (error == 0)
		usb_hid_registered = 1U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv usb hid input ready operation.
 */
void
drv_usb_hid_input_ready(
	void)
{
	unsigned long hid_irq;
	struct usb_hid *hid, *claimed;
	unsigned interface_number;
	unsigned long irq;
	int error;

	/* Handles the usb hid registered condition. */
	if (!usb_hid_registered)
		return;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		interface_number = 0U;

		irq = spin_lock_irqsave(&usb_hid_pending_lock);
		usb_hid_input_is_ready = 1U;
		hid = usb_hid_pending;
		claimed = NULL;

		/* Handles the hid availability. */
		if (hid != NULL) {
			usb_hid_pending = hid->pending_next;
			hid->pending_next = NULL;
			hid->pending = 0U;

			/*
 * Pin the state against detach before dropping the list
			 * lock. Detach closes admission and joins this
			 * activation flag. */
			hid_irq = spin_lock_irqsave(&hid->lock);

			/* Handles the hid condition. */
			if (!hid->stopping && !hid->active &&
			    !hid->activating) {
				hid->activating = 1U;
				interface_number = drv_usb_interface_number(
					hid->interface);
				claimed = hid;
			}
			spin_unlock_irqrestore(&hid->lock, hid_irq);
		}
		spin_unlock_irqrestore(&usb_hid_pending_lock, irq);

		/* Handles the hid availability. */
		if (hid == NULL)
			return;

		/*
 * A stopped generation was removed by detach and owns its own
		 * free. Continue draining later pending interfaces instead of
		 * treating it as the end of the list. */
		if (claimed == NULL)
			continue;
		error = usb_hid_activate(claimed, 1);

		/* Checks the operation status. */
		if (error != 0) {
			hal_printf("usb-hid: deferred activation failed "
				   "interface=%u error=%d\n",
				   interface_number, error);
		}
	}
}
/* End consolidated usb-hid.c. */
