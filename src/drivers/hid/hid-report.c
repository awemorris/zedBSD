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

static uint32_t
item_unsigned(const uint8_t *data, size_t size)
{
	uint32_t value = 0;
	size_t index;

	for (index = 0; index < size; index++)
		value |= (uint32_t)data[index] << (index * 8U);
	return value;
}

static int32_t
sign_extend(uint32_t value, unsigned bits)
{
	int64_t extended = value;

	if ((value & ((uint32_t)1U << (bits - 1U))) != 0)
		extended -= (int64_t)((uint64_t)1U << bits);
	return (int32_t)extended;
}

static int
item_signed(const uint8_t *data, size_t size, int32_t *result)
{
	if (size != 1U && size != 2U && size != 4U)
		return EINVAL;
	*result = sign_extend(item_unsigned(data, size), (unsigned)size * 8U);
	return 0;
}

static void
local_clear(struct hid_local_state *local)
{
	memset(local, 0, sizeof(*local));
}

static int
usage_value(const struct hid_global_state *global, const uint8_t *data,
	size_t size, uint32_t *result)
{
	uint32_t raw;

	if (size != 1U && size != 2U && size != 4U)
		return EINVAL;
	raw = item_unsigned(data, size);
	if (size == 4U) {
		*result = raw;
		return 0;
	}
	if (global->usage_page > UINT16_MAX)
		return EOPNOTSUPP;
	*result = (global->usage_page << 16U) | raw;
	return 0;
}

static int
local_validate(const struct hid_local_state *local)
{
	return local->range_open ? EINVAL : 0;
}

static int
local_usage_at(const struct hid_local_state *local, uint32_t index,
	uint32_t *usage)
{
	const struct hid_local_usage_span *span;
	uint32_t span_count;
	size_t span_index;

	for (span_index = 0; span_index < local->usage_count; span_index++) {
		span = &local->usages[span_index];
		span_count = span->maximum - span->minimum + 1U;
		if (index < span_count) {
			*usage = span->minimum + index;
			return 1;
		}
		index -= span_count;
	}
	if (local->usage_count == 0U)
		return 0;
	*usage = local->usages[local->usage_count - 1U].maximum;
	return 1;
}

static struct hid_report_description *
find_report(struct hid_report_layout *layout, uint8_t id)
{
	size_t index;

	for (index = 0; index < layout->report_count; index++)
		if (layout->reports[index].id == id)
			return &layout->reports[index];
	return NULL;
}

static const struct hid_report_description *
find_report_const(const struct hid_report_layout *layout, uint8_t id)
{
	size_t index;

	for (index = 0; index < layout->report_count; index++)
		if (layout->reports[index].id == id)
			return &layout->reports[index];
	return NULL;
}

static int
add_report(struct hid_report_layout *layout, uint8_t id,
	struct hid_report_description **result)
{
	struct hid_report_description *report;

	if (find_report(layout, id) != NULL)
		return EINVAL;
	if (layout->report_count >= HID_REPORT_ID_COUNT_MAX)
		return E2BIG;
	report = &layout->reports[layout->report_count++];
	memset(report, 0, sizeof(*report));
	report->id = id;
	*result = report;
	return 0;
}

static int
add_capability(struct hid_report_layout *layout, uint16_t type, uint16_t code)
{
	size_t index;

	for (index = 0; index < layout->capability_count; index++)
		if (layout->capabilities[index].type == type &&
		    layout->capabilities[index].code == code)
			return 0;
	if (layout->capability_count >= HID_REPORT_FIELD_COUNT_MAX + 1U)
		return E2BIG;
	layout->capabilities[layout->capability_count].type = type;
	layout->capabilities[layout->capability_count].code = code;
	layout->capability_count++;
	return 0;
}

static int
add_absolute_axis(struct hid_report_layout *layout, uint16_t code,
	int32_t minimum, int32_t maximum)
{
	struct input_abs_axis *axis;
	size_t index;

	for (index = 0; index < layout->absolute_axis_count; index++) {
		axis = &layout->absolute_axes[index];
		if (axis->code != code)
			continue;
		return axis->info.minimum == minimum &&
		       axis->info.maximum == maximum ? 0 : EINVAL;
	}
	if (layout->absolute_axis_count >= ABS_MAX + 1U)
		return E2BIG;
	axis = &layout->absolute_axes[layout->absolute_axis_count++];
	memset(axis, 0, sizeof(*axis));
	axis->code = code;
	axis->info.minimum = minimum;
	axis->info.maximum = maximum;
	axis->info.value = minimum;
	return 0;
}

static uint16_t
keyboard_code(uint16_t usage)
{
	static const uint16_t alpha[26] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
		KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P,
		KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X,
		KEY_Y, KEY_Z,
	};
	static const uint16_t digits[10] = {
		KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
		KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	};

	if (usage >= 0x04U && usage <= 0x1dU)
		return alpha[usage - 0x04U];
	if (usage >= 0x1eU && usage <= 0x27U)
		return digits[usage - 0x1eU];
	if (usage >= 0x3aU && usage <= 0x43U)
		return (uint16_t)(KEY_F1 + usage - 0x3aU);
	switch (usage) {
	case 0x28: return KEY_ENTER;
	case 0x29: return KEY_ESC;
	case 0x2a: return KEY_BACKSPACE;
	case 0x2b: return KEY_TAB;
	case 0x2c: return KEY_SPACE;
	case 0x2d: return KEY_MINUS;
	case 0x2e: return KEY_EQUAL;
	case 0x2f: return KEY_LEFTBRACE;
	case 0x30: return KEY_RIGHTBRACE;
	case 0x31: return KEY_BACKSLASH;
	case 0x33: return KEY_SEMICOLON;
	case 0x34: return KEY_APOSTROPHE;
	case 0x35: return KEY_GRAVE;
	case 0x36: return KEY_COMMA;
	case 0x37: return KEY_DOT;
	case 0x38: return KEY_SLASH;
	case 0x39: return KEY_CAPSLOCK;
	case 0x44: return KEY_F11;
	case 0x45: return KEY_F12;
	case 0x49: return KEY_INSERT;
	case 0x4a: return KEY_HOME;
	case 0x4b: return KEY_PAGEUP;
	case 0x4c: return KEY_DELETE;
	case 0x4d: return KEY_END;
	case 0x4e: return KEY_PAGEDOWN;
	case 0x4f: return KEY_RIGHT;
	case 0x50: return KEY_LEFT;
	case 0x51: return KEY_DOWN;
	case 0x52: return KEY_UP;
	case 0xe0: return KEY_LEFTCTRL;
	case 0xe1: return KEY_LEFTSHIFT;
	case 0xe2: return KEY_LEFTALT;
	case 0xe4: return KEY_RIGHTCTRL;
	case 0xe5: return KEY_RIGHTSHIFT;
	case 0xe6: return KEY_RIGHTALT;
	default: return KEY_RESERVED;
	}
}

static int
usage_to_event(uint32_t usage, unsigned input_flags, uint16_t *type,
	uint16_t *code, uint8_t *kind)
{
	uint16_t page = (uint16_t)(usage >> 16U);
	uint16_t value = (uint16_t)usage;
	uint16_t key;

	if (page == HID_USAGE_PAGE_KEYBOARD) {
		key = keyboard_code(value);
		if (key == KEY_RESERVED)
			return 0;
		*type = EV_KEY;
		*code = key;
		*kind = HID_FIELD_KEY;
		return 1;
	}
	if (page == HID_USAGE_PAGE_BUTTON && value >= 1U && value <= 5U) {
		*type = EV_KEY;
		*code = (uint16_t)(BTN_LEFT + value - 1U);
		*kind = HID_FIELD_KEY;
		return 1;
	}
	if (page != HID_USAGE_PAGE_GENERIC_DESKTOP)
		return 0;
	if ((input_flags & HID_INPUT_RELATIVE) != 0) {
		*type = EV_REL;
		*kind = HID_FIELD_AXIS;
		switch (value) {
		case HID_USAGE_X: *code = REL_X; return 1;
		case HID_USAGE_Y: *code = REL_Y; return 1;
		case HID_USAGE_WHEEL: *code = REL_WHEEL; return 1;
		default: return 0;
		}
	}
	*type = EV_ABS;
	*kind = HID_FIELD_AXIS;
	switch (value) {
	case HID_USAGE_X: *code = ABS_X; return 1;
	case HID_USAGE_Y: *code = ABS_Y; return 1;
	default: return 0;
	}
}

static int
logical_maximum(const struct hid_global_state *global, int32_t *result)
{
	uint32_t raw;

	if (!global->logical_minimum_set || !global->logical_maximum_set)
		return EINVAL;
	raw = global->logical_maximum_raw;
	if (global->logical_minimum < 0)
		*result = sign_extend(raw,
		    (unsigned)global->logical_maximum_size * 8U);
	else {
		if (raw > INT32_MAX)
			return EINVAL;
		*result = (int32_t)raw;
	}
	return global->logical_minimum <= *result ? 0 : EINVAL;
}

static int
logical_range_fits_field(int32_t minimum, int32_t maximum,
	uint32_t bit_size)
{
	int64_t field_minimum, field_maximum;

	if (bit_size == 0U || bit_size > 32U || minimum > maximum)
		return 0;
	if (minimum < 0) {
		field_minimum = -(int64_t)(UINT64_C(1) << (bit_size - 1U));
		field_maximum =
		    (int64_t)(UINT64_C(1) << (bit_size - 1U)) - 1;
	} else {
		field_minimum = 0;
		field_maximum =
		    (int64_t)((UINT64_C(1) << bit_size) - UINT64_C(1));
	}
	return (int64_t)minimum >= field_minimum &&
	    (int64_t)maximum <= field_maximum;
}

static int
add_field(struct hid_parser *parser, struct hid_report_description *report,
	uint32_t bit_offset, uint32_t usage_minimum, uint32_t usage_maximum,
	int32_t logical_minimum, int32_t logical_maximum, uint16_t type,
	uint16_t code, uint8_t bit_size, uint8_t kind)
{
	struct hid_report_layout *layout = parser->layout;
	struct hid_report_field *field;
	size_t index;
	int error;

	if (bit_size == 0U || bit_size > 32U)
		return EINVAL;
	if (layout->field_count >= HID_REPORT_FIELD_COUNT_MAX)
		return E2BIG;
	if (kind != HID_FIELD_KEYBOARD_ARRAY)
		for (index = 0; index < layout->field_count; index++) {
			field = &layout->fields[index];
			if (field->report_id == report->id &&
			    field->kind != HID_FIELD_KEYBOARD_ARRAY &&
			    field->type == type && field->code == code)
				return EINVAL;
		}
	if (kind != HID_FIELD_KEYBOARD_ARRAY &&
	    (error = add_capability(layout, type, code)) != 0)
		return error;
	if (type == EV_ABS &&
	    (error = add_absolute_axis(layout, code, logical_minimum,
		logical_maximum)) != 0)
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
	return 0;
}

static int
add_keyboard_array_capabilities(struct hid_report_layout *layout,
	uint32_t minimum, uint32_t maximum)
{
	uint16_t page = (uint16_t)(minimum >> 16U);
	uint16_t first = (uint16_t)minimum;
	uint16_t last = (uint16_t)maximum;
	uint16_t usage;
	int error;

	if (page != HID_USAGE_PAGE_KEYBOARD ||
	    page != (uint16_t)(maximum >> 16U))
		return EOPNOTSUPP;
	for (usage = 0; usage <= 0xe7U; usage++) {
		uint16_t code;

		if (usage < first || usage > last)
			continue;
		code = keyboard_code(usage);
		if (code == KEY_RESERVED)
			continue;
		error = add_capability(layout, EV_KEY, code);
		if (error != 0)
			return error;
	}
	return 0;
}

static int
parse_input(struct hid_parser *parser, uint32_t flags)
{
	struct hid_report_layout *layout = parser->layout;
	struct hid_report_description *report;
	const struct hid_local_usage_span *array_usage;
	uint32_t bit_offset, bits, count, index, usage;
	uint32_t accepted_minimum, accepted_maximum;
	uint32_t accepted_usage_minimum, accepted_usage_maximum;
	int32_t logical_max;
	int error;

	error = local_validate(&parser->local);
	if (error != 0)
		return error;
	if ((flags & ~HID_INPUT_SUPPORTED_FLAGS) != 0U)
		return EOPNOTSUPP;
	if (parser->global.report_size == 0U ||
	    parser->global.report_count == 0U)
		return EINVAL;
	if (parser->global.report_size > HID_REPORT_BITS_MAX ||
	    parser->global.report_count > HID_REPORT_BITS_MAX)
		return E2BIG;
	if (parser->global.report_id == 0U) {
		if (layout->uses_report_ids)
			return EINVAL;
		parser->no_id_report_used = 1;
		report = find_report(layout, 0);
		if (report == NULL && (error = add_report(layout, 0, &report)) != 0)
			return error;
	} else {
		if (parser->no_id_report_used)
			return EINVAL;
		report = find_report(layout, parser->global.report_id);
		if (report == NULL)
			return EINVAL;
	}
	count = parser->global.report_count;
	if (count > (HID_REPORT_BITS_MAX - report->bit_count) /
	    parser->global.report_size)
		return E2BIG;
	bits = count * parser->global.report_size;
	bit_offset = report->bit_count;
	if ((flags & HID_INPUT_CONSTANT) != 0) {
		report->bit_count += bits;
		return 0;
	}
	/*
	 * Logical bounds describe every Data field, including usages outside the
	 * v1 mapping.  Validate them before usage filtering so an impossible
	 * vendor field cannot be hidden in front of an otherwise supported field.
	 */
	if ((error = logical_maximum(&parser->global, &logical_max)) != 0)
		return error;
	if (!logical_range_fits_field(parser->global.logical_minimum,
	    logical_max, parser->global.report_size))
		return EINVAL;
	if ((flags & HID_INPUT_VARIABLE) == 0) {
		if (parser->local.usage_count != 1U ||
		    !parser->local.usages[0].is_range)
			return EOPNOTSUPP;
		array_usage = &parser->local.usages[0];
		if ((array_usage->minimum >> 16U) !=
		    HID_USAGE_PAGE_KEYBOARD ||
		    (array_usage->maximum >> 16U) !=
		    HID_USAGE_PAGE_KEYBOARD)
			goto advance;
		if (parser->global.logical_minimum < 0 ||
		    logical_max > (int32_t)UINT16_MAX)
			return EINVAL;
		/*
		 * Array values are usage IDs.  Only values admitted by both the
		 * local Usage range and the global Logical range may be advertised
		 * or decoded.  Keeping the intersection in the field also makes HID
		 * keyboard error usages 1..3 visible only when the descriptor
		 * actually permits them.
		 */
		accepted_minimum = (uint16_t)array_usage->minimum;
		if ((uint32_t)parser->global.logical_minimum > accepted_minimum)
			accepted_minimum = (uint32_t)parser->global.logical_minimum;
		accepted_maximum = (uint16_t)array_usage->maximum;
		if ((uint32_t)logical_max < accepted_maximum)
			accepted_maximum = (uint32_t)logical_max;
		if (accepted_minimum > accepted_maximum)
			goto advance;
		accepted_usage_minimum =
		    (HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_minimum;
		accepted_usage_maximum =
		    (HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_maximum;
		error = add_keyboard_array_capabilities(layout,
		    accepted_usage_minimum, accepted_usage_maximum);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++) {
			error = add_field(parser, report,
			    bit_offset + index * parser->global.report_size,
			    accepted_usage_minimum, accepted_usage_maximum,
			    (int32_t)accepted_minimum,
			    (int32_t)accepted_maximum,
			    EV_KEY, KEY_RESERVED,
			    (uint8_t)parser->global.report_size,
			    HID_FIELD_KEYBOARD_ARRAY);
			if (error != 0)
				return error;
		}
		goto advance;
	}
	for (index = 0; index < count; index++) {
		uint16_t type, code;
		uint8_t kind;

		if (!local_usage_at(&parser->local, index, &usage) ||
		    !usage_to_event(usage, flags, &type, &code, &kind))
			continue;
		error = add_field(parser, report,
		    bit_offset + index * parser->global.report_size, usage, usage,
		    parser->global.logical_minimum, logical_max, type, code,
		    (uint8_t)parser->global.report_size, kind);
		if (error != 0)
			return error;
	}
advance:
	report->bit_count += bits;
	return 0;
}

static int
parse_main(struct hid_parser *parser, unsigned tag, const uint8_t *data,
	size_t size)
{
	int error;

	error = local_validate(&parser->local);
	if (error != 0) {
		local_clear(&parser->local);
		return error;
	}
	error = 0;

	switch (tag) {
	case HID_MAIN_INPUT:
		if (size == 0U)
			error = EINVAL;
		else
			error = parse_input(parser, item_unsigned(data, size));
		break;
	case HID_MAIN_COLLECTION:
		if (size == 0U)
			error = EINVAL;
		else {
			if (parser->collection_depth >=
			    HID_REPORT_COLLECTION_DEPTH_MAX)
				error = E2BIG;
			else
				parser->collection_depth++;
		}
		break;
	case HID_MAIN_END_COLLECTION:
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
	return error;
}

static int
parse_global(struct hid_parser *parser, unsigned tag, const uint8_t *data,
	size_t size)
{
	struct hid_report_description *ignored;
	uint32_t value;
	int error;

	switch (tag) {
	case HID_GLOBAL_USAGE_PAGE:
		if (size == 0U)
			return EINVAL;
		value = item_unsigned(data, size);
		if (value > UINT16_MAX)
			return EOPNOTSUPP;
		parser->global.usage_page = value;
		return 0;
	case HID_GLOBAL_LOGICAL_MINIMUM:
		error = item_signed(data, size, &parser->global.logical_minimum);
		if (error == 0)
			parser->global.logical_minimum_set = 1;
		return error;
	case HID_GLOBAL_LOGICAL_MAXIMUM:
		if (size != 1U && size != 2U && size != 4U)
			return EINVAL;
		parser->global.logical_maximum_raw = item_unsigned(data, size);
		parser->global.logical_maximum_size = (uint8_t)size;
		parser->global.logical_maximum_set = 1;
		return 0;
	case HID_GLOBAL_REPORT_SIZE:
		if (size == 0U)
			return EINVAL;
		parser->global.report_size = item_unsigned(data, size);
		return 0;
	case HID_GLOBAL_REPORT_COUNT:
		if (size == 0U)
			return EINVAL;
		parser->global.report_count = item_unsigned(data, size);
		return 0;
	case HID_GLOBAL_REPORT_ID:
		if (size != 1U || data[0] == 0U || parser->no_id_report_used)
			return EINVAL;
		parser->layout->uses_report_ids = 1;
		parser->global.report_id = data[0];
		if (find_report(parser->layout, data[0]) != NULL)
			return 0;
		return add_report(parser->layout, data[0], &ignored);
	case HID_GLOBAL_PUSH:
		if (size != 0U)
			return EINVAL;
		if (parser->global_depth >= HID_REPORT_GLOBAL_DEPTH_MAX)
			return E2BIG;
		parser->global_stack[parser->global_depth++] = parser->global;
		return 0;
	case HID_GLOBAL_POP:
		if (size != 0U || parser->global_depth == 0U)
			return EINVAL;
		parser->global = parser->global_stack[--parser->global_depth];
		return 0;
	case 3U: /* Physical Minimum */
	case 4U: /* Physical Maximum */
	case 5U: /* Unit Exponent */
	case 6U: /* Unit */
		/* These globals cannot change supported input decoding. */
		return size == 1U || size == 2U || size == 4U ? 0 : EINVAL;
	default:
		return EOPNOTSUPP;
	}
}

static int
parse_local(struct hid_parser *parser, unsigned tag, const uint8_t *data,
	size_t size)
{
	struct hid_local_usage_span *span;
	uint32_t usage;
	int error;

	if (tag == HID_LOCAL_DELIMITER)
		return size == 1U || size == 2U || size == 4U ?
		    EOPNOTSUPP : EINVAL;
	if (tag >= 3U && tag <= 9U)
		return size == 1U || size == 2U || size == 4U ? 0 : EINVAL;
	if (tag != HID_LOCAL_USAGE && tag != HID_LOCAL_USAGE_MINIMUM &&
	    tag != HID_LOCAL_USAGE_MAXIMUM)
		return EOPNOTSUPP;
	error = usage_value(&parser->global, data, size, &usage);
	if (error != 0)
		return error;
	switch (tag) {
	case HID_LOCAL_USAGE:
		if (parser->local.usage_count >= HID_REPORT_FIELD_COUNT_MAX)
			return E2BIG;
		span = &parser->local.usages[parser->local.usage_count++];
		span->minimum = usage;
		span->maximum = usage;
		span->is_range = 0;
		return 0;
	case HID_LOCAL_USAGE_MINIMUM:
		if (parser->local.range_open)
			return EINVAL;
		if (parser->local.usage_count >= HID_REPORT_FIELD_COUNT_MAX)
			return E2BIG;
		parser->local.open_range = parser->local.usage_count;
		span = &parser->local.usages[parser->local.usage_count++];
		span->minimum = usage;
		span->maximum = usage;
		span->is_range = 1;
		parser->local.range_open = 1;
		return 0;
	case HID_LOCAL_USAGE_MAXIMUM:
		if (!parser->local.range_open)
			return EINVAL;
		span = &parser->local.usages[parser->local.open_range];
		if ((span->minimum >> 16U) != (usage >> 16U) ||
		    span->minimum > usage)
			return EINVAL;
		span->maximum = usage;
		parser->local.range_open = 0;
		return 0;
	default:
		return EINVAL;
	}
}

static int
parse_descriptor(struct hid_parser *parser)
{
	const uint8_t *descriptor = parser->layout->descriptor;
	size_t length = parser->layout->descriptor_size;
	size_t offset = 0;

	while (offset < length) {
		uint8_t prefix = descriptor[offset++];
		size_t size;
		unsigned type, tag;
		int error;

		if (prefix == 0xfeU) {
			if (length - offset < 2U)
				return EINVAL;
			size = descriptor[offset];
			offset += 2U;
			if (size > length - offset)
				return EINVAL;
			offset += size;
			continue;
		}
		size = prefix & 0x03U;
		if (size == 3U)
			size = 4U;
		if (size > length - offset)
			return EINVAL;
		type = (prefix >> 2U) & 0x03U;
		tag = prefix >> 4U;
		if (type == HID_ITEM_TYPE_MAIN)
			error = parse_main(parser, tag, descriptor + offset, size);
		else if (type == HID_ITEM_TYPE_GLOBAL)
			error = parse_global(parser, tag, descriptor + offset, size);
		else if (type == HID_ITEM_TYPE_LOCAL)
			error = parse_local(parser, tag, descriptor + offset, size);
		else
			error = EOPNOTSUPP;
		if (error != 0)
			return error;
		offset += size;
	}
	if (parser->collection_depth != 0U || parser->global_depth != 0U)
		return EINVAL;
	if (parser->local.usage_count != 0U || parser->local.range_open)
		return EINVAL;
	if (!parser->supported_field_seen)
		return EOPNOTSUPP;
	return 0;
}

static struct hid_report_layout *
layout_allocate(void)
{
	struct hid_report_layout *layout;

	layout = kern_calloc(1, sizeof(*layout));
	if (layout != NULL) {
		layout->capabilities[0].type = EV_SYN;
		layout->capabilities[0].code = SYN_REPORT;
		layout->capability_count = 1;
	}
	return layout;
}

int
hid_report_layout_parse(const void *descriptor, size_t length,
	struct hid_report_layout **result)
{
	struct hid_report_layout *layout;
	struct hid_parser *parser;
	int error;

	if (descriptor == NULL || length == 0U || result == NULL)
		return EINVAL;
	if (length > HID_REPORT_DESCRIPTOR_SIZE_MAX)
		return E2BIG;
	layout = layout_allocate();
	if (layout == NULL)
		return ENOMEM;
	layout->descriptor_size = length;
	layout->profile = HID_LAYOUT_PROFILE_DESCRIPTOR;
	memcpy(layout->descriptor, descriptor, length);
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);
		return ENOMEM;
	}
	parser->layout = layout;
	error = parse_descriptor(parser);
	kern_free(parser);
	if (error != 0) {
		kern_free(layout);
		return error;
	}
	*result = layout;
	return 0;
}

static int
boot_layout_begin(struct hid_report_layout **result,
	struct hid_report_layout **layout_result,
	struct hid_report_description **report_result)
{
	struct hid_report_layout *layout;
	int error;

	if (result == NULL)
		return EINVAL;
	layout = layout_allocate();
	if (layout == NULL)
		return ENOMEM;
	error = add_report(layout, 0, report_result);
	if (error != 0) {
		kern_free(layout);
		return error;
	}
	*layout_result = layout;
	return 0;
}

int
hid_report_layout_boot_keyboard(struct hid_report_layout **result)
{
	struct hid_report_layout *layout;
	struct hid_report_description *report;
	struct hid_parser *parser;
	static const uint16_t modifier_usages[] = {
		0xe0, 0xe1, 0xe2, 0xe4, 0xe5, 0xe6,
	};
	size_t index;
	int error;

	error = boot_layout_begin(result, &layout, &report);
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_KEYBOARD;
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);
		return ENOMEM;
	}
	parser->layout = layout;
	for (index = 0; index < sizeof(modifier_usages) /
	    sizeof(modifier_usages[0]); index++) {
		uint16_t usage = modifier_usages[index];
		uint16_t code = keyboard_code(usage);

		error = add_field(parser, report,
		    (uint32_t)(usage - 0xe0U),
		    (HID_USAGE_PAGE_KEYBOARD << 16U) | usage,
		    (HID_USAGE_PAGE_KEYBOARD << 16U) | usage,
		    0, 1, EV_KEY, code, 1, HID_FIELD_KEY);
		if (error != 0)
			goto fail;
	}
	error = add_keyboard_array_capabilities(layout,
	    HID_USAGE_PAGE_KEYBOARD << 16U,
	    (HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU);
	if (error != 0)
		goto fail;
	for (index = 0; index < 6U; index++) {
		error = add_field(parser, report, 16U + (uint32_t)index * 8U,
		    HID_USAGE_PAGE_KEYBOARD << 16U,
		    (HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU,
		    0, 255, EV_KEY, KEY_RESERVED, 8,
		    HID_FIELD_KEYBOARD_ARRAY);
		if (error != 0)
			goto fail;
	}
	report->bit_count = 64U;
	kern_free(parser);
	*result = layout;
	return 0;
fail:
	kern_free(parser);
	kern_free(layout);
	return error;
}

int
hid_report_layout_boot_mouse(struct hid_report_layout **result)
{
	struct hid_report_layout *layout;
	struct hid_report_description *report;
	struct hid_parser *parser;
	unsigned index;
	int error;

	error = boot_layout_begin(result, &layout, &report);
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_MOUSE;
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);
		return ENOMEM;
	}
	parser->layout = layout;
	for (index = 0; index < 3U; index++) {
		error = add_field(parser, report, index,
		    (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
		    (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
		    0, 1, EV_KEY, (uint16_t)(BTN_LEFT + index), 1,
		    HID_FIELD_KEY);
		if (error != 0)
			goto fail;
	}
	error = add_field(parser, report, 8,
	    (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
	    (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
	    -127, 127, EV_REL, REL_X, 8, HID_FIELD_AXIS);
	if (error != 0)
		goto fail;
	error = add_field(parser, report, 16,
	    (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
	    (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
	    -127, 127, EV_REL, REL_Y, 8, HID_FIELD_AXIS);
	if (error != 0)
		goto fail;
	report->bit_count = 24U;
	kern_free(parser);
	*result = layout;
	return 0;
fail:
	kern_free(parser);
	kern_free(layout);
	return error;
}

void
hid_report_layout_destroy(struct hid_report_layout *layout)
{
	kern_free(layout);
}

int
hid_report_layout_get_info(const struct hid_report_layout *layout,
	struct hid_report_layout_info *result)
{
	if (layout == NULL || result == NULL)
		return EINVAL;
	result->descriptor_size = layout->descriptor_size;
	result->report_count = layout->report_count;
	result->field_count = layout->field_count;
	result->capability_count = layout->capability_count;
	result->absolute_axis_count = layout->absolute_axis_count;
	result->uses_report_ids = layout->uses_report_ids;
	return 0;
}

int
hid_report_layout_get_report(const struct hid_report_layout *layout,
	size_t index, struct hid_report_report_info *result)
{
	const struct hid_report_description *report;

	if (layout == NULL || result == NULL)
		return EINVAL;
	if (index >= layout->report_count)
		return ENOENT;
	report = &layout->reports[index];
	result->report_id = report->id;
	result->minimum_size = (report->bit_count + 7U) / 8U +
	    (layout->uses_report_ids ? 1U : 0U);
	result->field_count = report->field_count;
	return 0;
}

int
hid_report_layout_get_capability(const struct hid_report_layout *layout,
	size_t index, struct input_capability *result)
{
	if (layout == NULL || result == NULL)
		return EINVAL;
	if (index >= layout->capability_count)
		return ENOENT;
	*result = layout->capabilities[index];
	return 0;
}

int
hid_report_layout_get_absolute_axis(const struct hid_report_layout *layout,
	size_t index, struct input_abs_axis *result)
{
	if (layout == NULL || result == NULL)
		return EINVAL;
	if (index >= layout->absolute_axis_count)
		return ENOENT;
	*result = layout->absolute_axes[index];
	return 0;
}

static int
extract_value(const uint8_t *data, size_t length, uint32_t bit_offset,
	uint8_t bit_size, uint32_t *result)
{
	uint32_t value = 0;
	unsigned bit;
	size_t available_bits;

	available_bits = length > SIZE_MAX / 8U ? SIZE_MAX : length * 8U;
	if (bit_size == 0U || bit_size > 32U ||
	    bit_offset > available_bits ||
	    bit_size > available_bits - bit_offset)
		return EINVAL;
	for (bit = 0; bit < bit_size; bit++) {
		size_t source_bit = (size_t)bit_offset + bit;

		if ((data[source_bit / 8U] &
		    ((uint8_t)1U << (source_bit % 8U))) != 0)
			value |= (uint32_t)1U << bit;
	}
	*result = value;
	return 0;
}

static int
decode_field_value(const struct hid_report_field *field, const uint8_t *data,
	size_t length, uint32_t *raw_result, int32_t *value_result)
{
	uint32_t raw;
	int32_t value;
	int error;

	error = extract_value(data, length, field->bit_offset,
	    field->bit_size, &raw);
	if (error != 0)
		return error;
	if (field->logical_minimum < 0)
		value = sign_extend(raw, field->bit_size);
	else {
		if (raw > INT32_MAX)
			return EINVAL;
		value = (int32_t)raw;
	}
	if (value < field->logical_minimum || value > field->logical_maximum)
		return EINVAL;
	*raw_result = raw;
	*value_result = value;
	return 0;
}

static int
key_already_present(const struct hid_report_input *input, uint16_t code)
{
	size_t index;

	for (index = 0; index < input->value_count; index++)
		if (input->values[index].type == EV_KEY &&
		    input->values[index].code == code)
			return 1;
	return 0;
}

static int
append_value(struct hid_report_input *input, uint16_t type, uint16_t code,
	int32_t value)
{
	struct hid_report_value *entry;

	if (type == EV_KEY && key_already_present(input, code))
		return 0;
	if (input->value_count >= HID_REPORT_VALUE_COUNT_MAX)
		return E2BIG;
	entry = &input->values[input->value_count++];
	entry->type = type;
	entry->code = code;
	entry->value = value;
	return 0;
}

int
hid_report_decode(const struct hid_report_layout *layout, const void *buffer,
	size_t length, struct hid_report_input *result)
{
	const struct hid_report_description *report;
	const uint8_t *bytes = buffer, *data;
	size_t payload_length, minimum_length, index;
	uint8_t report_id;
	int keyboard_error = 0;
	int error;

	if (layout == NULL || buffer == NULL || result == NULL)
		return EINVAL;
	if (layout->uses_report_ids) {
		if (length == 0U)
			return EINVAL;
		report_id = bytes[0];
		report = find_report_const(layout, report_id);
		if (report == NULL)
			return EINVAL;
		data = bytes + 1U;
		payload_length = length - 1U;
	} else {
		report_id = 0;
		report = find_report_const(layout, 0);
		if (report == NULL)
			return EINVAL;
		data = bytes;
		payload_length = length;
	}
	minimum_length = (report->bit_count + 7U) / 8U;
	if (payload_length < minimum_length)
		return EINVAL;
	if (layout->profile == HID_LAYOUT_PROFILE_BOOT_KEYBOARD && data[1] != 0U)
		return EINVAL;
	if (report->field_count == 0U)
		return EOPNOTSUPP;
	/* Validate every selected field and recognize keyboard errors first. */
	for (index = 0; index < layout->field_count; index++) {
		const struct hid_report_field *field = &layout->fields[index];
		uint32_t raw;
		int32_t value;

		if (field->report_id != report_id)
			continue;
		error = decode_field_value(field, data, payload_length, &raw, &value);
		if (error != 0)
			return error;
		if (field->kind == HID_FIELD_KEYBOARD_ARRAY) {
			uint32_t usage = (field->usage_minimum & 0xffff0000U) | raw;

			if (raw > UINT16_MAX || usage < field->usage_minimum ||
			    usage > field->usage_maximum)
				return EINVAL;
			if ((uint16_t)usage >= HID_USAGE_KEYBOARD_ERROR_MIN &&
			    (uint16_t)usage <= HID_USAGE_KEYBOARD_ERROR_MAX)
				keyboard_error = 1;
		}
	}
	memset(result, 0, sizeof(*result));
	result->report_id = report_id;
	result->keyboard_error = (uint8_t)keyboard_error;
	for (index = 0; index < layout->field_count; index++) {
		const struct hid_report_field *field = &layout->fields[index];
		uint32_t raw;
		int32_t value;

		if (field->report_id != report_id)
			continue;
		error = decode_field_value(field, data, payload_length, &raw, &value);
		if (error != 0)
			return error;
		/*
		 * A keyboard error usage invalidates the complete keyboard state,
		 * including modifier variables.  Non-keyboard fields in a composite
		 * report may still be returned.
		 */
		if (result->keyboard_error &&
		    (field->usage_minimum >> 16U) == HID_USAGE_PAGE_KEYBOARD)
			continue;
		if (field->kind == HID_FIELD_KEYBOARD_ARRAY) {
			uint16_t code;

			if (raw == 0U)
				continue;
			code = keyboard_code((uint16_t)raw);
			if (code == KEY_RESERVED)
				continue;
			error = append_value(result, EV_KEY, code, 1);
		} else if (field->kind == HID_FIELD_KEY) {
			if (value == 0)
				continue;
			error = append_value(result, EV_KEY, field->code, 1);
		} else {
			error = append_value(result, field->type, field->code,
			    value);
		}
		if (error != 0) {
			memset(result, 0, sizeof(*result));
			return error;
		}
	}
	return 0;
}
