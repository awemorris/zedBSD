/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* XXX: Need coding style fitting. */

/*
 * The HID report descriptor parser and report decoder, shared by every HID
 * transport (ws159-p003: moved unchanged out of the USB HID driver so that
 * the I2C-HID driver uses it too).
 */

#include <drivers/generic/hid-digitizer.h>
#include <drivers/generic/hid-touch.h>
#include <drivers/generic/hid-report.h>
#include <kern/input-device.h>
#include <kern/kmem.h>
#include <kern/kcrt.h>

#include <stdint.h>
#include <uapi/errno.h>

#define HID_ITEM_TYPE_MAIN		0U
#define HID_ITEM_TYPE_GLOBAL		1U
#define HID_ITEM_TYPE_LOCAL		2U

#define HID_MAIN_INPUT			8U
#define HID_MAIN_OUTPUT			9U
#define HID_MAIN_COLLECTION		10U
#define HID_MAIN_FEATURE		11U
#define HID_MAIN_END_COLLECTION		12U

#define HID_GLOBAL_USAGE_PAGE		0U
#define HID_GLOBAL_LOGICAL_MINIMUM	1U
#define HID_GLOBAL_LOGICAL_MAXIMUM	2U
#define HID_GLOBAL_PHYSICAL_MINIMUM	3U
#define HID_GLOBAL_PHYSICAL_MAXIMUM	4U
#define HID_GLOBAL_UNIT_EXPONENT	5U
#define HID_GLOBAL_UNIT			6U
#define HID_GLOBAL_REPORT_SIZE		7U
#define HID_GLOBAL_REPORT_ID		8U
#define HID_GLOBAL_REPORT_COUNT		9U
#define HID_GLOBAL_PUSH			10U
#define HID_GLOBAL_POP			11U

#define HID_LOCAL_USAGE			0U
#define HID_LOCAL_USAGE_MINIMUM		1U
#define HID_LOCAL_USAGE_MAXIMUM		2U
#define HID_LOCAL_DELIMITER		10U

#define HID_INPUT_CONSTANT		0x01U
#define HID_INPUT_VARIABLE		0x02U
#define HID_INPUT_RELATIVE		0x04U
#define HID_INPUT_SUPPORTED_FLAGS	0x07U

#define HID_USAGE_PAGE_GENERIC_DESKTOP	0x01U
#define HID_USAGE_PAGE_KEYBOARD		0x07U
#define HID_USAGE_PAGE_BUTTON		0x09U
#define HID_USAGE_PAGE_CONSUMER		0x0cU
#define HID_USAGE_PAGE_DIGITIZER	0x0dU

/* The application collections of the Digitizer page that hold a pen. */
#define HID_USAGE_DIGITIZER		0x000d0001U
#define HID_USAGE_PEN			0x000d0002U

/* The application collection of a touch screen, and the collection of one of its fingers. */
#define HID_USAGE_TOUCH_SCREEN		0x000d0004U
#define HID_USAGE_FINGER		0x000d0022U

/* The application collection of a touch pad, whose fingers are a touch screen's (ws159-p003). */
#define HID_USAGE_TOUCH_PAD		0x000d0005U

/* The Digitizer usages of a finger besides Tip Switch, and the report's Contact Count. */
#define HID_USAGE_CONFIDENCE		0x47U
#define HID_USAGE_CONTACT_ID		0x51U
#define HID_USAGE_CONTACT_COUNT		0x54U
#define HID_USAGE_SCAN_TIME		0x56U

/* What touch_item() calls the Contact Count and the Scan Time: above every finger item (HID_TOUCH_ITEM_*). */
#define TOUCH_ITEM_CONTACT_COUNT	0x10U
#define TOUCH_ITEM_SCAN_TIME		0x11U

/* What touch_item() calls a touch pad's button n: TOUCH_ITEM_BUTTON + n (ws159-p003). */
#define TOUCH_ITEM_BUTTON		0x12U

/* The most feature report fields of a Precision Touchpad a layout keeps (ws159-p003). */
#define HID_FEATURE_FIELDS_MAX		16U

/* The Digitizer usages that are absolute axes of a pen. */
#define HID_USAGE_TIP_PRESSURE		0x30U
#define HID_USAGE_X_TILT		0x3dU
#define HID_USAGE_Y_TILT		0x3eU

/* The unit systems of a HID Unit item (its lowest nibble). */
#define HID_UNIT_SYSTEM_SI_LINEAR	1U
#define HID_UNIT_SYSTEM_SI_ROTATION	2U
#define HID_UNIT_SYSTEM_ENGLISH_LINEAR	3U
#define HID_UNIT_SYSTEM_ENGLISH_ROTATION 4U

#define HID_USAGE_X			0x30U
#define HID_USAGE_Y			0x31U
#define HID_USAGE_WHEEL			0x38U
/* The Consumer page's horizontal scroll (AC Pan), which mice send beside the wheel. */
#define HID_USAGE_AC_PAN		0x0238U
#define HID_USAGE_KEYBOARD_ERROR_MIN	0x01U
#define HID_USAGE_KEYBOARD_ERROR_MAX	0x03U

#define HID_FIELD_KEY			1U
#define HID_FIELD_AXIS			2U
#define HID_FIELD_KEYBOARD_ARRAY	3U
#define HID_FIELD_DIGITIZER		4U
#define HID_FIELD_TOUCH			5U

#define HID_LAYOUT_PROFILE_DESCRIPTOR		0U
#define HID_LAYOUT_PROFILE_BOOT_KEYBOARD	1U
#define HID_LAYOUT_PROFILE_BOOT_MOUSE		2U

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
	/* How many Finger collections of a touch screen this report carries. */
	uint8_t touch_fingers;
};

/*
 * One report that has feature items: its identifier and its length in bits
 * so far (after its identifier byte).  It lives in its layout.
 */
struct hid_feature_report {
	uint8_t id;
	uint32_t bit_count;
};

/*
 * One field of a feature report that a driver sets or reads: its usage,
 * its report and where it is in that report.  It lives in its layout.
 */
struct hid_feature_field {
	uint32_t usage;
	uint8_t report_id;
	uint8_t bit_size;
	uint32_t bit_offset;
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
	/* Which kind of pen collection the descriptor has (HID_REPORT_PEN_*). */
	uint8_t pen;
	/*
	 * The touch screen: the most Finger collections one report carries,
	 * whether reports carry a Contact Count, and the first finger's X and Y
	 * (set once each is seen).
	 */
	uint8_t touch_count_present;
	uint8_t touch_x_set;
	uint8_t touch_y_set;
	size_t touch_contacts;
	struct input_absinfo touch_x;
	struct input_absinfo touch_y;
	/* The touch screen's Scan Time, when a report carries one: its logical maximum and its unit. */
	uint8_t touch_scan_present;
	int32_t touch_scan_maximum;
	uint32_t touch_scan_unit_ns;
	/* Whether the fingers are a touch pad's, and how many buttons the pad reports (ws159-p003). */
	uint8_t touch_pad;
	uint8_t touch_buttons;
	/*
	 * The feature reports (ws159-p003): the length in bits of each report
	 * that has a feature item, and the fields of the Precision Touchpad's
	 * usages that a driver sets or reads (hid_feature_usage()).
	 */
	struct hid_feature_report feature_reports[HID_REPORT_ID_COUNT_MAX];
	size_t feature_report_count;
	struct hid_feature_field feature_fields[HID_FEATURE_FIELDS_MAX];
	size_t feature_field_count;
};

struct hid_global_state {
	uint32_t usage_page;
	uint32_t logical_maximum_raw;
	uint32_t report_size;
	uint32_t report_count;
	int32_t logical_minimum;
	int32_t physical_minimum;
	int32_t physical_maximum;
	uint32_t unit_exponent;
	uint32_t unit;
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
	/* The usage that opened each collection that is still open. */
	uint32_t collection_usages[HID_REPORT_COLLECTION_DEPTH_MAX];
	/*
	 * The open Finger collection of a touch screen: the collection depth it
	 * sits at (0 when none is open), and its place among its report's
	 * fingers (-1 until its first field names the report).
	 */
	size_t finger_depth;
	int finger_contact;
	int no_id_report_used;
	int supported_field_seen;
	/*
	 * Where the item being parsed starts in the descriptor (BUG-267: a
	 * refusal names it); the descriptor's length once every item was taken
	 * and the checks of the whole descriptor run.
	 */
	size_t item_offset;
};

static uint32_t item_unsigned(const uint8_t *data, size_t size);
static int32_t sign_extend(uint32_t value, unsigned bits);
static int item_signed(const uint8_t *data, size_t size, int32_t *result);
static void local_clear(struct hid_local_state *local);
static int usage_value(const struct hid_global_state *global, const uint8_t *data, size_t size, uint32_t *result);
static int local_validate(const struct hid_local_state *local);
static int local_usage_at(const struct hid_local_state *local, uint32_t index, uint32_t *usage);
static int local_names_keyboard(const struct hid_local_state *local);
static struct hid_report_description * find_report(struct hid_report_layout *layout, uint8_t id);
static const struct hid_report_description * find_report_const(const struct hid_report_layout *layout, uint8_t id);
static int add_report(struct hid_report_layout *layout, uint8_t id, struct hid_report_description **result);
static int add_capability(struct hid_report_layout *layout, uint16_t type, uint16_t code);
static int add_absolute_axis(struct hid_report_layout *layout, uint16_t code, int32_t minimum, int32_t maximum);
static uint16_t keyboard_code(uint16_t usage);
static int usage_to_event(uint32_t usage, unsigned input_flags, int in_pen, uint16_t *type, uint16_t *code, uint8_t *kind);
static int digitizer_to_event(uint16_t usage, uint16_t *type, uint16_t *code, uint8_t *kind);
static int add_digitizer_capabilities(struct hid_report_layout *layout, uint16_t usage);
static int parser_in_pen(const struct hid_parser *parser);
static int32_t unit_exponent_value(uint32_t raw);
static int32_t axis_resolution(const struct hid_global_state *global, int32_t logical_minimum, int32_t logical_maximum);
static void set_axis_resolution(struct hid_report_layout *layout, uint16_t code, int32_t resolution);
static int parser_in_touch(const struct hid_parser *parser);
static int parser_in_touch_pad(const struct hid_parser *parser);
static int parse_feature(struct hid_parser *parser, uint32_t flags);
static int hid_feature_usage(uint32_t usage);
static struct hid_feature_report *feature_report(struct hid_report_layout *layout, uint8_t id);
static unsigned touch_item(uint32_t usage);
static int add_touch_field(struct hid_parser *parser, struct hid_report_description *report, uint32_t bit_offset, uint32_t usage, int32_t logical_maximum);
static void touch_axis(const struct hid_global_state *global, int32_t logical_maximum, struct input_absinfo *info);
static uint32_t scan_time_unit(const struct hid_global_state *global);
static int logical_maximum(const struct hid_global_state *global, int32_t *result);
static int logical_range_fits_field(int32_t minimum, int32_t maximum, uint32_t bit_size);
static int add_field(struct hid_parser *parser, struct hid_report_description *report, uint32_t bit_offset, uint32_t usage_minimum, uint32_t usage_maximum, int32_t logical_minimum, int32_t logical_maximum, uint16_t type, uint16_t code, uint8_t bit_size, uint8_t kind);
static int add_keyboard_array_capabilities(struct hid_report_layout *layout, uint32_t minimum, uint32_t maximum);
static int parse_input(struct hid_parser *parser, uint32_t flags);
static void close_collection(struct hid_parser *parser);
static int parse_main(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);
static int parse_global(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);
static int parse_local(struct hid_parser *parser, unsigned tag, const uint8_t *data, size_t size);
static int parse_descriptor(struct hid_parser *parser);
static int layout_parse(const void *descriptor, size_t length, struct hid_report_layout **result, size_t *item_offset);
static struct hid_report_layout * layout_allocate(void);
static int boot_layout_begin(struct hid_report_layout **result, struct hid_report_layout **layout_result, struct hid_report_description **report_result);
static int extract_value(const uint8_t *data, size_t length, uint32_t bit_offset, uint8_t bit_size, uint32_t *result);
static int decode_field_value(const struct hid_report_field *field, const uint8_t *data, size_t length, uint32_t *raw_result, int32_t *value_result);
static int key_already_present(const struct hid_report_input *input, uint16_t code);
static int append_value(struct hid_report_input *input, uint16_t type, uint16_t code, int32_t value);

/* Reads the unsigned value one report descriptor item carries. */
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

/* Extends a value of the given width to a full signed one. */
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

/* Reads the signed value one report descriptor item carries. */
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
	/* Succeeded. */
	return 0;
}

/* Forgets the local items collected for the next main item. */
static void
local_clear(
	struct hid_local_state *local)
{
	kern_memset(local, 0, sizeof(*local));
}

/* Reports one collected usage, with its page attached. */
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

	/* Checks the current data size. */
	raw = item_unsigned(data, size);
	if (size == 4U) {
		*result = raw;
		/* Succeeded. */
		return 0;
	}

	/* Handles the global condition. */
	if (global->usage_page > UINT16_MAX)
		return EOPNOTSUPP;
	*result = (global->usage_page << 16U) | raw;
	/* Succeeded. */
	return 0;
}

/* Refuses local items that do not describe a usable field. */
static int
local_validate(
	const struct hid_local_state *local)
{
	/* Returns the computed result. */
	return local->range_open ? EINVAL : 0;
}

/* Asks whether any usage collected for the next main item is a keyboard usage. */
static int
local_names_keyboard(
	const struct hid_local_state *local)
{
	const struct hid_local_usage_span *span;
	size_t span_index;

	/* Looks at both ends of every usage and range. */
	for (span_index = 0; span_index < local->usage_count; span_index++) {
		span = &local->usages[span_index];

		/* A range that starts on the keyboard page names keys. */
		if ((span->minimum >> 16U) == HID_USAGE_PAGE_KEYBOARD)
			return 1;

		/* A range that ends on the keyboard page names keys too. */
		if ((span->maximum >> 16U) == HID_USAGE_PAGE_KEYBOARD)
			return 1;
	}

	/* No usage is on the keyboard page. */
	return 0;
}

/* Reports the usage that belongs to one field of an array. */
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

		/* Checks the current index. */
		span_count = span->maximum - span->minimum + 1U;
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

/* Finds the report of a given kind and identifier. */
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

static const struct hid_report_description * find_report_const(const struct hid_report_layout *layout, uint8_t id);

/* Finds that report without permission to change it. */
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

/* Adds a report of a given kind and identifier to the layout. */
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
	kern_memset(report, 0, sizeof(*report));
	report->id = id;
	*result = report;
	/* Succeeded. */
	return 0;
}

/* Records that the device can report one thing. */
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
		    layout->capabilities[index].code == code) {
			/* Succeeded. */
			return 0;
		}
	}

	/* Handles the layout condition. */
	if (layout->capability_count >= HID_REPORT_FIELD_COUNT_MAX + 1U)
		return E2BIG;
	layout->capabilities[layout->capability_count].type = type;
	layout->capabilities[layout->capability_count].code = code;
	layout->capability_count++;

	/* Succeeded. */
	return 0;
}

/* Records an axis the device reports an absolute position on. */
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
		/* Handles the axis condition. */
		axis = &layout->absolute_axes[index];
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
	kern_memset(axis, 0, sizeof(*axis));
	axis->code = code;
	axis->info.minimum = minimum;
	axis->info.maximum = maximum;
	axis->info.value = minimum;

	/* Succeeded. */
	return 0;
}

/* Renders one keyboard usage as the key code the kernel uses. */
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
	static const uint16_t keypad_digits[10] = {
		KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5,
		KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KP0,
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

	/* Keypad 1 to 9 and 0 follow each other from usage 0x59. */
	if (usage >= 0x59U && usage <= 0x62U)
		return keypad_digits[usage - 0x59U];

	/* F13 to F24 follow each other from usage 0x68. */
	if (usage >= 0x68U && usage <= 0x73U)
		return (uint16_t)(KEY_F13 + usage - 0x68U);

	/* Maps each remaining usage the HID usage tables name for a keyboard. */
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
	case 0x32:
		/* The non-US # key, which evdev also calls backslash. */
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
	case 0x46:
		/* Print Screen, which evdev calls SysRq. */
		return KEY_SYSRQ;
	case 0x47:
		/* Scroll Lock. */
		return KEY_SCROLLLOCK;
	case 0x48:
		/* Pause. */
		return KEY_PAUSE;
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
	case 0x53:
		/* Num Lock. */
		return KEY_NUMLOCK;
	case 0x54:
		/* Keypad slash. */
		return KEY_KPSLASH;
	case 0x55:
		/* Keypad asterisk. */
		return KEY_KPASTERISK;
	case 0x56:
		/* Keypad minus. */
		return KEY_KPMINUS;
	case 0x57:
		/* Keypad plus. */
		return KEY_KPPLUS;
	case 0x58:
		/* Keypad Enter. */
		return KEY_KPENTER;
	case 0x63:
		/* Keypad dot (Delete with Num Lock off). */
		return KEY_KPDOT;
	case 0x64:
		/* The non-US backslash key next to the left Shift (<> on ISO). */
		return KEY_102ND;
	case 0x65:
		/* Application (Menu), which evdev calls Compose. */
		return KEY_COMPOSE;
	case 0x66:
		/* Power. */
		return KEY_POWER;
	case 0x67:
		/* Keypad equals. */
		return KEY_KPEQUAL;
	case 0x85:
		/* Keypad comma. */
		return KEY_KPCOMMA;
	case 0x87:
		/* International 1: the Japanese backslash-underscore (Ro) key. */
		return KEY_RO;
	case 0x88:
		/* International 2: Japanese Katakana/Hiragana. */
		return KEY_KATAKANAHIRAGANA;
	case 0x89:
		/* International 3: the Japanese Yen key. */
		return KEY_YEN;
	case 0x8a:
		/* International 4: Japanese Henkan (convert). */
		return KEY_HENKAN;
	case 0x8b:
		/* International 5: Japanese Muhenkan (no convert). */
		return KEY_MUHENKAN;
	case 0x8c:
		/* International 6: the Japanese keypad comma. */
		return KEY_KPJPCOMMA;
	case 0x90:
		/* LANG1: Korean Hangul/English. */
		return KEY_HANGEUL;
	case 0x91:
		/* LANG2: Korean Hanja. */
		return KEY_HANJA;
	case 0x92:
		/* LANG3: Japanese Katakana. */
		return KEY_KATAKANA;
	case 0x93:
		/* LANG4: Japanese Hiragana. */
		return KEY_HIRAGANA;
	case 0x94:
		/* LANG5: Japanese Zenkaku/Hankaku. */
		return KEY_ZENKAKUHANKAKU;
	case 0xe0:
		/* Returns the computed result. */
		return KEY_LEFTCTRL;
	case 0xe1:
		/* Returns the computed result. */
		return KEY_LEFTSHIFT;
	case 0xe2:
		/* Returns the computed result. */
		return KEY_LEFTALT;
	case 0xe3:
		/* The left GUI (Super, Windows) key. */
		return KEY_LEFTMETA;
	case 0xe4:
		/* Returns the computed result. */
		return KEY_RIGHTCTRL;
	case 0xe5:
		/* Returns the computed result. */
		return KEY_RIGHTSHIFT;
	case 0xe6:
		/* Returns the computed result. */
		return KEY_RIGHTALT;
	case 0xe7:
		/* The right GUI (Super, Windows) key. */
		return KEY_RIGHTMETA;
	default:
		/* Returns the computed result. */
		return KEY_RESERVED;
	}
}

/* Renders one usage as the input event it stands for. */
static int
usage_to_event(
	uint32_t usage,
	unsigned input_flags,
	int in_pen,
	uint16_t *type,
	uint16_t *code,
	uint8_t *kind)
{
	uint16_t page = (uint16_t)(usage >> 16U);
	uint16_t value = (uint16_t)usage;
	uint16_t key;

	/* Handles the page condition. */
	if (page == HID_USAGE_PAGE_KEYBOARD) {
		/* Handles the selected key. */
		key = keyboard_code(value);
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

	/* A Digitizer usage counts only inside a pen collection. */
	if (page == HID_USAGE_PAGE_DIGITIZER) {
		/* Ignores the fingers and touch screens the driver does not handle. */
		if (!in_pen)
			return 0;

		/* Reports whether the pen usage is one the driver maps. */
		if (digitizer_to_event(value, type, code, kind))
			return 1;

		/* Ignores a pen usage outside the mapping. */
		return 0;
	}

	/* A relative AC Pan is the horizontal wheel of a mouse. */
	if (page == HID_USAGE_PAGE_CONSUMER) {
		/* Ignores every other consumer control. */
		if (value != HID_USAGE_AC_PAN)
			return 0;

		/* Ignores a pan that reports a position rather than a motion. */
		if ((input_flags & HID_INPUT_RELATIVE) == 0)
			return 0;

		/* Reports the pan as horizontal wheel motion. */
		*type = EV_REL;
		*code = REL_HWHEEL;
		*kind = HID_FIELD_AXIS;
		/* Reports the mapped event. */
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
			/* Succeeded. */
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
		/* Succeeded. */
		return 0;
	}
}

/* Reports the largest value a field of that width can hold. */
static int
logical_maximum(
	const struct hid_global_state *global,
	int32_t *result)
{
	uint32_t raw;

	/* Handles the global condition. */
	if (!global->logical_minimum_set || !global->logical_maximum_set)
		return EINVAL;

	/* Handles the global condition. */
	raw = global->logical_maximum_raw;
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

/* Asks whether a declared range fits the field it is declared on. */
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

/* Adds one field of a report to the layout. */
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

	/*
	 * Refuses a second field for the same axis or pen switch in one
	 * report, which would leave its value ambiguous.  A key may have
	 * several fields: two keyboard usages can stand for one key (the US
	 * Backslash 0x31 and the ISO Non-US # 0x32 are both KEY_BACKSLASH),
	 * and the decoder holds a key while any of its fields is set.
	 */
	if (kind != HID_FIELD_KEYBOARD_ARRAY && kind != HID_FIELD_KEY) {
		/* Process each remaining element. */
		for (index = 0; index < layout->field_count; index++) {
			/* Handles the field condition. */
			field = &layout->fields[index];
			if (field->report_id == report->id &&
			    field->kind != HID_FIELD_KEYBOARD_ARRAY &&
			    field->type == type && field->code == code) {
				/* Failed. */
				return EINVAL;
			}
		}
	}

	/* Declares the tool and button events a pen switch turns into. */
	if (kind == HID_FIELD_DIGITIZER) {
		/* Reports a capability table that is full. */
		error = add_digitizer_capabilities(layout, code);
		if (error != 0)
			return error;
	}

	/* Checks the operation status. */
	if (kind != HID_FIELD_KEYBOARD_ARRAY &&
	    kind != HID_FIELD_DIGITIZER &&
	    kind != HID_FIELD_TOUCH &&
	    (error = add_capability(layout, type, code)) != 0) {
		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	if (type == EV_ABS &&
	    (error = add_absolute_axis(layout, code, logical_minimum,
				       logical_maximum)) != 0) {
		/* Failed. */
		return error;
	}
	field = &layout->fields[layout->field_count++];
	kern_memset(field, 0, sizeof(*field));
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

	/* Succeeded. */
	return 0;
}

/* Records every key an array field of a keyboard can report. */
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
	    page != (uint16_t)(maximum >> 16U)) {
		/* Failed. */
		return EOPNOTSUPP;
	}
	/* Process each element required by the operation. */
	for (usage = 0; usage <= 0xe7U; usage++) {
		/* Handles the usage condition. */
		if (usage < first || usage > last)
			continue;

		/* Handles the code condition. */
		code = keyboard_code(usage);
		if (code == KEY_RESERVED)
			continue;

		/* Checks the operation status. */
		error = add_capability(layout, EV_KEY, code);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/* Takes one input main item into the layout. */
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
	int32_t resolution;
	int in_pen;
	int in_touch;
	int keyboard_array;
	int found;
	int error;

	/* Checks the operation status. */
	error = local_validate(&parser->local);
	if (error != 0)
		return error;

	/* Asks whether the fields of this item belong to a pen or to a touch screen. */
	in_pen = parser_in_pen(parser);
	in_touch = parser_in_touch(parser);

	/* Checks the active flags. */
	if ((flags & ~HID_INPUT_SUPPORTED_FLAGS) != 0U)
		return EOPNOTSUPP;

	/* Checks the parser state. */
	if (parser->global.report_size == 0U ||
	    parser->global.report_count == 0U) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the parser state. */
	if (parser->global.report_size > HID_REPORT_BITS_MAX ||
	    parser->global.report_count > HID_REPORT_BITS_MAX) {
		/* Returns the computed result. */
		return E2BIG;
	}

	/* Checks the parser state. */
	if (parser->global.report_id == 0U) {
		/* Handles the layout condition. */
		if (layout->uses_report_ids)
			return EINVAL;
		parser->no_id_report_used = 1;

		/* Checks the operation status. */
		report = find_report(layout, 0);
		if (report == NULL &&
		    (error = add_report(layout, 0, &report)) != 0) {
			/* Failed. */
			return error;
		}
	} else {
		/* Checks the parser state. */
		if (parser->no_id_report_used)
			return EINVAL;

		/* Handles the report availability. */
		report = find_report(layout, parser->global.report_id);
		if (report == NULL)
			return EINVAL;
	}

	/* Checks the remaining item count. */
	count = parser->global.report_count;
	if (count > (HID_REPORT_BITS_MAX - report->bit_count) /
			    parser->global.report_size) {
		/* Returns the computed result. */
		return E2BIG;
	}
	bits = count * parser->global.report_size;
	bit_offset = report->bit_count;

	/* Checks the active flags. */
	if ((flags & HID_INPUT_CONSTANT) != 0) {
		report->bit_count += bits;

		/* Succeeded. */
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
				      logical_max, parser->global.report_size)) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the active flags. */
	if ((flags & HID_INPUT_VARIABLE) == 0) {
		/*
		 * Skips an array that names no keyboard usage: the driver maps
		 * only keyboard arrays, and the others (a system control list,
		 * a vendor channel) report nothing it publishes.
		 */
		keyboard_array = local_names_keyboard(&parser->local);
		if (!keyboard_array) {
			report->bit_count += bits;

			/* Succeeded. */
			return 0;
		}

		/* Refuses a keyboard array that is not one range of usages. */
		if (parser->local.usage_count != 1U ||
		    !parser->local.usages[0].is_range) {
			/* Failed. */
			return EOPNOTSUPP;
		}

		/* Handles the array usage condition. */
		array_usage = &parser->local.usages[0];
		if ((array_usage->minimum >> 16U) != HID_USAGE_PAGE_KEYBOARD ||
		    (array_usage->maximum >> 16U) != HID_USAGE_PAGE_KEYBOARD)
			goto advance;

		/* Checks the parser state. */
		if (parser->global.logical_minimum < 0 ||
		    logical_max > (int32_t)UINT16_MAX) {
			/* Failed. */
			return EINVAL;
		}

		/*
		 * Array values are usage IDs.  Only values admitted by both the
		 * local Usage range and the global Logical range may be
		 * advertised or decoded.  Keeping the intersection in the field
		 * also makes HID keyboard error usages 1..3 visible only when
		 * the descriptor actually permits them.
		 */

		/* Handles the uint32 t condition. */
		accepted_minimum = (uint16_t)array_usage->minimum;
		if ((uint32_t)parser->global.logical_minimum >
		    accepted_minimum) {
			accepted_minimum =
				(uint32_t)parser->global.logical_minimum;
		}

		/* Handles the uint32 t condition. */
		accepted_maximum = (uint16_t)array_usage->maximum;
		if ((uint32_t)logical_max < accepted_maximum)
			accepted_maximum = (uint32_t)logical_max;

		/* Handles the accepted minimum condition. */
		if (accepted_minimum > accepted_maximum)
			goto advance;
		accepted_usage_minimum =
			(HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_minimum;
		accepted_usage_maximum =
			(HID_USAGE_PAGE_KEYBOARD << 16U) | accepted_maximum;

		/* Checks the operation status. */
		error = add_keyboard_array_capabilities(
			layout, accepted_usage_minimum, accepted_usage_maximum);
		if (error != 0)
			return error;
		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			/* Checks the operation status. */
			error = add_field(
				parser, report,
				bit_offset + index * parser->global.report_size,
				accepted_usage_minimum, accepted_usage_maximum,
				(int32_t)accepted_minimum,
				(int32_t)accepted_maximum, EV_KEY, KEY_RESERVED,
				(uint8_t)parser->global.report_size,
				HID_FIELD_KEYBOARD_ARRAY);
			if (error != 0)
				return error;
		}

		goto advance;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* A field without a usage reports nothing. */
		found = local_usage_at(&parser->local, index, &usage);
		if (!found)
			continue;

		/* A touch screen's fields go to the touch state machine only. */
		if (in_touch) {
			error = add_touch_field(parser, report,
				bit_offset + index * parser->global.report_size,
				usage, logical_max);
			if (error != 0)
				return error;
			continue;
		}

		/* A usage without an event reports nothing. */
		found = usage_to_event(usage, flags, in_pen, &type, &code, &kind);
		if (!found)
			continue;

		/* Checks the operation status. */
		error = add_field(
			parser, report,
			bit_offset + index * parser->global.report_size, usage,
			usage, parser->global.logical_minimum, logical_max,
			type, code, (uint8_t)parser->global.report_size, kind);
		if (error != 0)
			return error;

		/* Gives an absolute axis the resolution its physical size implies. */
		if (type == EV_ABS) {
			resolution = axis_resolution(&parser->global,
				parser->global.logical_minimum,
				logical_max);
			set_axis_resolution(layout, code, resolution);
		}

		/* Marks the layout as a pen once it takes a pen switch. */
		if (kind == HID_FIELD_DIGITIZER &&
		    layout->pen == HID_REPORT_PEN_NONE)
			layout->pen = (uint8_t)in_pen;
	}

advance:
	report->bit_count += bits;

	/* Succeeded. */
	return 0;
}

/* Closes the innermost open collection, and the finger it held. */
static void
close_collection(
	struct hid_parser *parser)
{
	/* The Finger collection that closes ends its finger. */
	if (parser->finger_depth == parser->collection_depth) {
		parser->finger_depth = 0;
		parser->finger_contact = -1;
	}

	/* One collection fewer is open. */
	parser->collection_depth--;
}

/* Takes one main item into the layout. */
static int
parse_main(
	struct hid_parser *parser,
	unsigned tag,
	const uint8_t *data,
	size_t size)
{
	int in_touch;
	int error;

	/* Checks the operation status. */
	error = local_validate(&parser->local);
	if (error != 0) {
		local_clear(&parser->local);

		/* Failed. */
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
			    HID_REPORT_COLLECTION_DEPTH_MAX) {
				error = E2BIG;
			} else {
				/* Remembers the usage that opens the collection. */
				parser->collection_usages[parser->collection_depth] = 0;
				if (parser->local.usage_count != 0U) {
					parser->collection_usages[parser->collection_depth] =
						parser->local.usages[0].minimum;
				}

				parser->collection_depth++;

				/* A Finger collection of a touch screen opens one finger, not yet placed. */
				if (parser->collection_usages[parser->collection_depth - 1U] == HID_USAGE_FINGER) {
					in_touch = parser_in_touch(parser);
					if (in_touch) {
						parser->finger_depth = parser->collection_depth;
						parser->finger_contact = -1;
					}
				}
			}
		}

		break;
	case HID_MAIN_END_COLLECTION:
		/* Checks the current data size. */
		if (size != 0U)
			error = EINVAL;
		else if (parser->collection_depth == 0U)
			error = EINVAL;
		else
			close_collection(parser);
		break;
	case HID_MAIN_OUTPUT:
		/* Output layouts are deliberately outside v1. */
		if (size == 0U)
			error = EINVAL;
		break;
	case HID_MAIN_FEATURE:
		/* A feature item: the Precision Touchpad's fields are kept (ws159-p003). */
		if (size == 0U)
			error = EINVAL;
		else
			error = parse_feature(parser, item_unsigned(data, size));
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

/* Takes one global item into the parser state. */
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

		/* Validates the current value. */
		value = item_unsigned(data, size);
		if (value > UINT16_MAX)
			return EOPNOTSUPP;
		parser->global.usage_page = value;

		/* Succeeded. */
		return 0;
	case HID_GLOBAL_LOGICAL_MINIMUM:

		/* Checks the operation status. */
		error = item_signed(data, size,
				    &parser->global.logical_minimum);
		if (error == 0)
			parser->global.logical_minimum_set = 1;

		/* Failed. */
		return error;
	case HID_GLOBAL_LOGICAL_MAXIMUM:
		/* Checks the current data size. */
		if (size != 1U && size != 2U && size != 4U)
			return EINVAL;
		parser->global.logical_maximum_raw = item_unsigned(data, size);
		parser->global.logical_maximum_size = (uint8_t)size;
		parser->global.logical_maximum_set = 1;

		/* Succeeded. */
		return 0;
	case HID_GLOBAL_REPORT_SIZE:
		/* Checks the current data size. */
		if (size == 0U)
			return EINVAL;
		parser->global.report_size = item_unsigned(data, size);

		/* Succeeded. */
		return 0;
	case HID_GLOBAL_REPORT_COUNT:
		/* Checks the current data size. */
		if (size == 0U)
			return EINVAL;
		parser->global.report_count = item_unsigned(data, size);

		/* Succeeded. */
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

		/* Succeeded. */
		return 0;
	case HID_GLOBAL_POP:
		/* Checks the current data size. */
		if (size != 0U || parser->global_depth == 0U)
			return EINVAL;
		parser->global = parser->global_stack[--parser->global_depth];

		/* Succeeded. */
		return 0;
	case HID_GLOBAL_PHYSICAL_MINIMUM:
		/* The physical size of the axes that follow, lower end. */
		error = item_signed(data, size, &parser->global.physical_minimum);
		if (error != 0)
			return error;

		/* Succeeded: the lower physical end is recorded. */
		return 0;
	case HID_GLOBAL_PHYSICAL_MAXIMUM:
		/* The physical size of the axes that follow, upper end. */
		error = item_signed(data, size, &parser->global.physical_maximum);
		if (error != 0)
			return error;

		/* Succeeded: the upper physical end is recorded. */
		return 0;
	case HID_GLOBAL_UNIT_EXPONENT:
		/* Refuses an item with no value or an odd width. */
		if (size != 1U && size != 2U && size != 4U)
			return EINVAL;

		/* The power of ten the physical ends are scaled by. */
		parser->global.unit_exponent = item_unsigned(data, size);

		/* Succeeded: the exponent is recorded. */
		return 0;
	case HID_GLOBAL_UNIT:
		/* Refuses an item with no value or an odd width. */
		if (size != 1U && size != 2U && size != 4U)
			return EINVAL;

		/* The unit system and dimensions of the physical ends. */
		parser->global.unit = item_unsigned(data, size);

		/* Succeeded: the unit is recorded. */
		return 0;
	default:
		/* Failed. */
		return EOPNOTSUPP;
	}
}

/* Takes one local item into the parser state. */
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
	    tag != HID_LOCAL_USAGE_MAXIMUM) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Checks the operation status. */
	error = usage_value(&parser->global, data, size, &usage);
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

		/* Succeeded. */
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

		/* Succeeded. */
		return 0;
	case HID_LOCAL_USAGE_MAXIMUM:
		/* Checks the parser state. */
		if (!parser->local.range_open)
			return EINVAL;

		/* Handles the span condition. */
		span = &parser->local.usages[parser->local.open_range];
		if ((span->minimum >> 16U) != (usage >> 16U) ||
		    span->minimum > usage) {
			/* Failed. */
			return EINVAL;
		}
		span->maximum = usage;
		parser->local.range_open = 0;

		/* Succeeded. */
		return 0;
	default:
		/* Failed. */
		return EINVAL;
	}
}

/* Walks a whole report descriptor, item by item. */
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
		/* Handles the prefix condition. */
		parser->item_offset = offset;
		prefix = descriptor[offset++];
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

		/* Checks the current data size. */
		size = prefix & 0x03U;
		if (size == 3U)
			size = 4U;

		/* Checks the current data size. */
		if (size > length - offset)
			return EINVAL;
		type = (prefix >> 2U) & 0x03U;

		/* Handles the type condition. */
		tag = prefix >> 4U;
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
		if (error != 0)
			return error;
		offset += size;
	}

	/* The checks of the whole descriptor, past its last item. */
	parser->item_offset = length;

	/* Checks the parser state. */
	if (parser->collection_depth != 0U || parser->global_depth != 0U)
		return EINVAL;

	/* Checks the parser state. */
	if (parser->local.usage_count != 0U || parser->local.range_open)
		return EINVAL;

	/* Checks the parser state. */
	if (!parser->supported_field_seen)
		return EOPNOTSUPP;

	/* Succeeded. */
	return 0;
}

/* Takes the memory one parsed layout lives in. */
static struct hid_report_layout *
layout_allocate(
	void)
{
	struct hid_report_layout *layout;

	/* Handles the layout availability. */
	layout = kern_calloc(1, sizeof(*layout));
	if (layout != NULL) {
		layout->capabilities[0].type = EV_SYN;
		layout->capabilities[0].code = SYN_REPORT;
		layout->capability_count = 1;
	}

	/* Returns the computed result. */
	return layout;
}

/*
 * Parses a report descriptor into a layout this kernel can use.
 */
int
drv_hid_report_layout_parse(
	const void *descriptor,
	size_t length,
	struct hid_report_layout **result)
{
	size_t item_offset;
	int error;

	/* The layout; where a refusal happened is not wanted here. */
	error = layout_parse(descriptor, length, result, &item_offset);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns the layout. */
	return 0;
}

/*
 * Tells where the parser refuses a report descriptor (BUG-267, for the
 * kernel's log): the offset of the item it refused, or the descriptor's
 * length when every item was taken and the whole descriptor was refused
 * (a collection left open, a usage left over, nothing this kernel uses).
 * Returns the parser's error (0 when it takes the descriptor; *item_offset
 * is then the length).  Nothing is kept.
 */
int
drv_hid_report_layout_diagnose(
	const void *descriptor,
	size_t length,
	size_t *item_offset)
{
	struct hid_report_layout *layout;
	int error;

	/* The parse, with where it stopped. */
	*item_offset = 0;
	layout = NULL;
	error = layout_parse(descriptor, length, &layout, item_offset);
	if (error != 0)
		return error;

	/* A layout that parsed is not kept. */
	drv_hid_report_layout_destroy(layout);

	/* Succeeded: the parser takes the descriptor. */
	return 0;
}

/* Parses a report descriptor into a layout, telling where the parser stopped (the item it refused). */
static int
layout_parse(
	const void *descriptor,
	size_t length,
	struct hid_report_layout **result,
	size_t *item_offset)
{
	struct hid_report_layout *layout;
	struct hid_parser *parser;
	int error;

	/* Nowhere yet. */
	*item_offset = 0;

	/* Handles the descriptor availability. */
	if (descriptor == NULL || length == 0U || result == NULL)
		return EINVAL;

	/* Checks the current data length. */
	if (length > HID_REPORT_DESCRIPTOR_SIZE_MAX)
		return E2BIG;

	/* Handles the layout availability. */
	layout = layout_allocate();
	if (layout == NULL)
		return ENOMEM;
	layout->descriptor_size = length;
	layout->profile = HID_LAYOUT_PROFILE_DESCRIPTOR;
	kern_memcpy(layout->descriptor, descriptor, length);

	/* Handles the parser availability. */
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);

		/* Failed. */
		return ENOMEM;
	}

	/* The walk of the items, and where it stopped. */
	parser->layout = layout;
	error = parse_descriptor(parser);
	*item_offset = parser->item_offset;
	kern_free(parser);
	if (error != 0) {
		kern_free(layout);

		/* Failed. */
		return error;
	}

	/* Succeeded: the caller owns the layout. */
	*result = layout;
	return 0;
}

/* Starts a layout for one of the two boot protocols. */
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

	/* Handles the layout availability. */
	layout = layout_allocate();
	if (layout == NULL)
		return ENOMEM;

	/* Checks the operation status. */
	error = add_report(layout, 0, report_result);
	if (error != 0) {
		kern_free(layout);

		/* Failed. */
		return error;
	}

	*layout_result = layout;
	/* Succeeded. */
	return 0;
}

/*
 * Builds the fixed layout the boot keyboard protocol has.
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
		0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
	};
	size_t index;
	int error;

	/* Checks the operation status. */
	error = boot_layout_begin(result, &layout, &report);
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_KEYBOARD;

	/* Handles the parser availability. */
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);

		/* Failed. */
		return ENOMEM;
	}

	parser->layout = layout;
	/* Process each remaining element. */
	for (index = 0;
	     index < sizeof(modifier_usages) / sizeof(modifier_usages[0]);
	     index++) {
		usage = modifier_usages[index];
		code = keyboard_code(usage);

		/* Checks the operation status. */
		error = add_field(parser, report, (uint32_t)(usage - 0xe0U),
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | usage,
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | usage, 0,
				  1, EV_KEY, code, 1, HID_FIELD_KEY);
		if (error != 0)
			goto fail;
	}

	/* Checks the operation status. */
	error = add_keyboard_array_capabilities(
		layout, HID_USAGE_PAGE_KEYBOARD << 16U,
		(HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU);
	if (error != 0)
		goto fail;
	/* Process each remaining element. */
	for (index = 0; index < 6U; index++) {
		/* Checks the operation status. */
		error = add_field(parser, report, 16U + (uint32_t)index * 8U,
				  HID_USAGE_PAGE_KEYBOARD << 16U,
				  (HID_USAGE_PAGE_KEYBOARD << 16U) | 0xffU, 0,
				  255, EV_KEY, KEY_RESERVED, 8,
				  HID_FIELD_KEYBOARD_ARRAY);
		if (error != 0)
			goto fail;
	}

	report->bit_count = 64U;
	kern_free(parser);
	*result = layout;
	/* Succeeded. */
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
 * Builds the fixed layout the boot mouse protocol has.
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

	/* Checks the operation status. */
	error = boot_layout_begin(result, &layout, &report);
	if (error != 0)
		return error;
	layout->profile = HID_LAYOUT_PROFILE_BOOT_MOUSE;

	/* Handles the parser availability. */
	parser = kern_calloc(1, sizeof(*parser));
	if (parser == NULL) {
		kern_free(layout);

		/* Failed. */
		return ENOMEM;
	}

	parser->layout = layout;
	/* Process each remaining element. */
	for (index = 0; index < 3U; index++) {
		/* Checks the operation status. */
		error = add_field(parser, report, index,
				  (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
				  (HID_USAGE_PAGE_BUTTON << 16U) | (index + 1U),
				  0, 1, EV_KEY, (uint16_t)(BTN_LEFT + index), 1,
				  HID_FIELD_KEY);
		if (error != 0)
			goto fail;
	}

	/* Checks the operation status. */
	error = add_field(parser, report, 8,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_X,
			  -127, 127, EV_REL, REL_X, 8, HID_FIELD_AXIS);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = add_field(parser, report, 16,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
			  (HID_USAGE_PAGE_GENERIC_DESKTOP << 16U) | HID_USAGE_Y,
			  -127, 127, EV_REL, REL_Y, 8, HID_FIELD_AXIS);
	if (error != 0)
		goto fail;
	report->bit_count = 24U;
	kern_free(parser);
	*result = layout;
	/* Succeeded. */
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
 * Gives a layout and its memory back.
 */
void
drv_hid_report_layout_destroy(
	struct hid_report_layout *layout)
{
	kern_free(layout);
}

/*
 * Reports what a layout says the device is.
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
	result->pen = layout->pen;
	result->touch_contacts = layout->touch_contacts;

	/* Succeeded. */
	return 0;
}

/*
 * Reports one report of a layout by its index.
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

	/* Succeeded. */
	return 0;
}

/*
 * Reports one thing the device can report, by index.
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
	/* Succeeded. */
	return 0;
}

/*
 * Reports one absolute axis of the device, by index.
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
	/* Succeeded. */
	return 0;
}

/*
 * Reports the touch screen a layout has: how many fingers a report carries,
 * whether the reports count them, and the fingers' X and Y.  ENOENT when the
 * layout has no touch screen with a position.
 */
int
drv_hid_report_layout_get_touch(
	const struct hid_report_layout *layout,
	struct hid_report_touch_info *result)
{
	/* Refuses a missing layout or result. */
	if (layout == NULL || result == NULL)
		return EINVAL;

	/* A layout without fingers, or whose fingers have no position, has no touch screen. */
	if (layout->touch_contacts == 0U)
		return ENOENT;
	if (!layout->touch_x_set)
		return ENOENT;
	if (!layout->touch_y_set)
		return ENOENT;

	/* Copies the description of the fingers. */
	result->contacts = layout->touch_contacts;
	result->count_present = layout->touch_count_present;
	result->x = layout->touch_x;
	result->y = layout->touch_y;

	/* And its Scan Time, when the reports carry one. */
	result->scan_time_present = layout->touch_scan_present;
	result->scan_time_maximum = layout->touch_scan_maximum;
	result->scan_time_unit_ns = layout->touch_scan_unit_ns;

	/* And whether it is a touch pad, with how many buttons. */
	result->pad = layout->touch_pad;
	result->buttons = layout->touch_buttons;

	/* Succeeded: the layout has a touch screen. */
	return 0;
}

/*
 * Finds where a feature report field of a Precision Touchpad usage
 * (HID_REPORT_USAGE_*) is, and how long its report's data is.
 */
int
drv_hid_report_layout_get_feature(
	const struct hid_report_layout *layout,
	uint32_t usage,
	struct hid_report_feature_info *result)
{
	const struct hid_feature_field *field;
	size_t index;
	size_t report;

	/* Refuses a missing layout or result. */
	if (layout == NULL || result == NULL)
		return EINVAL;

	/* Looks for the usage among the kept fields. */
	for (index = 0; index < layout->feature_field_count; index++) {
		/* A field of another usage is not it. */
		field = &layout->feature_fields[index];
		if (field->usage != usage)
			continue;

		/* The field's place. */
		result->report_id = field->report_id;
		result->bit_offset = field->bit_offset;
		result->bit_size = field->bit_size;

		/* Its report's data, in whole bytes. */
		result->data_size = 0;
		for (report = 0; report < layout->feature_report_count; report++) {
			if (layout->feature_reports[report].id == field->report_id)
				result->data_size = (layout->feature_reports[report].bit_count + 7U) / 8U;
		}

		/* Succeeded: the field is found. */
		return 0;
	}

	/* The descriptor has no such feature. */
	return ENOENT;
}

/* Takes one field's bits out of a report. */
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

	/* Handles the bit size condition. */
	available_bits = length > SIZE_MAX / 8U ? SIZE_MAX : length * 8U;
	if (bit_size == 0U || bit_size > 32U || bit_offset > available_bits ||
	    bit_size > available_bits - bit_offset) {
		/* Failed. */
		return EINVAL;
	}
	/* Process each remaining element. */
	for (bit = 0; bit < bit_size; bit++) {
		/* Handles the data condition. */
		source_bit = (size_t)bit_offset + bit;
		if ((data[source_bit / 8U] &
		     ((uint8_t)1U << (source_bit % 8U))) != 0)
			value |= (uint32_t)1U << bit;
	}

	*result = value;
	/* Succeeded. */
	return 0;
}

/* Renders one field's bits as the value it stands for. */
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

	/* Checks the operation status. */
	error = extract_value(data, length, field->bit_offset, field->bit_size,
			      &raw);
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
	/* Succeeded. */
	return 0;
}

/* Asks whether a key is already among those being reported. */
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
		    input->values[index].code == code) {
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Appends one decoded value to the events being built. */
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

	/* Succeeded. */
	return 0;
}

/*
 * Renders one report as the input events it stands for.
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

		/* Handles the report availability. */
		report = find_report_const(layout, report_id);
		if (report == NULL)
			return EINVAL;
		data = bytes + 1U;
		payload_length = length - 1U;
	} else {
		report_id = 0;

		/* Handles the report availability. */
		report = find_report_const(layout, 0);
		if (report == NULL)
			return EINVAL;
		data = bytes;
		payload_length = length;
	}

	/* Handles the payload length condition. */
	minimum_length = (report->bit_count + 7U) / 8U;
	if (payload_length < minimum_length)
		return EINVAL;

	/* Handles the layout condition. */
	if (layout->profile == HID_LAYOUT_PROFILE_BOOT_KEYBOARD &&
	    data[1] != 0U) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the report condition. */
	if (report->field_count == 0U)
		return EOPNOTSUPP;

	/* Validate every selected field and recognize keyboard errors first. */
	for (index = 0; index < layout->field_count; index++) {
		/* Handles the field local condition. */
		field_local = &layout->fields[index];
		if (field_local->report_id != report_id)
			continue;

		/* Checks the operation status. */
		error = decode_field_value(field_local, data, payload_length,
					   &raw_local, &value_local);
		if (error != 0)
			return error;

		/* Handles the field local condition. */
		if (field_local->kind == HID_FIELD_KEYBOARD_ARRAY) {
			/* Handles the raw local condition. */
			usage = (field_local->usage_minimum & 0xffff0000U) |
				raw_local;
			if (raw_local > UINT16_MAX ||
			    usage < field_local->usage_minimum ||
			    usage > field_local->usage_maximum) {
				/* Failed. */
				return EINVAL;
			}

			/* Checks the operation status. */
			if ((uint16_t)usage >= HID_USAGE_KEYBOARD_ERROR_MIN &&
			    (uint16_t)usage <= HID_USAGE_KEYBOARD_ERROR_MAX)
				keyboard_error = 1;
		}
	}

	kern_memset(result, 0, sizeof(*result));
	result->report_id = report_id;
	result->keyboard_error = (uint8_t)keyboard_error;
	/* Process each remaining element. */
	for (index = 0; index < layout->field_count; index++) {
		/* Handles the field local1 condition. */
		field_local1 = &layout->fields[index];
		if (field_local1->report_id != report_id)
			continue;

		/* Checks the operation status. */
		error = decode_field_value(field_local1, data, payload_length,
					   &raw_local2, &value_local3);
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

			/* Handles the code condition. */
			code = keyboard_code((uint16_t)raw_local2);
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
		if (error != 0) {
			kern_memset(result, 0, sizeof(*result));

			/* Failed. */
			return error;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Renders one Digitizer usage of a pen as the event it stands for. */
static int
digitizer_to_event(
	uint16_t usage,
	uint16_t *type,
	uint16_t *code,
	uint8_t *kind)
{
	/* Chooses between an absolute axis and a switch of the pen. */
	switch (usage) {
	case HID_USAGE_TIP_PRESSURE:
		/* The pressure, reported raw with its logical range. */
		*type = EV_ABS;
		*code = ABS_PRESSURE;
		*kind = HID_FIELD_AXIS;
		return 1;
	case HID_USAGE_X_TILT:
		/* The tilt towards the positive X axis. */
		*type = EV_ABS;
		*code = ABS_TILT_X;
		*kind = HID_FIELD_AXIS;
		return 1;
	case HID_USAGE_Y_TILT:
		/* The tilt towards the positive Y axis. */
		*type = EV_ABS;
		*code = ABS_TILT_Y;
		*kind = HID_FIELD_AXIS;
		return 1;
	case HID_DIGITIZER_USAGE_IN_RANGE:
	case HID_DIGITIZER_USAGE_INVERT:
	case HID_DIGITIZER_USAGE_TIP_SWITCH:
	case HID_DIGITIZER_USAGE_BARREL_SWITCH:
	case HID_DIGITIZER_USAGE_ERASER:
	case HID_DIGITIZER_USAGE_SECONDARY_BARREL:
		/* A switch the pen state machine combines into tool and contact. */
		*type = HID_REPORT_TYPE_DIGITIZER;
		*code = usage;
		*kind = HID_FIELD_DIGITIZER;
		return 1;
	default:
		/* The usage has no pen event. */
		return 0;
	}
}

/* Declares the evdev events one pen switch can produce. */
static int
add_digitizer_capabilities(
	struct hid_report_layout *layout,
	uint16_t usage)
{
	int error;

	/* Chooses the tool or button events the switch turns into. */
	switch (usage) {
	case HID_DIGITIZER_USAGE_IN_RANGE:
		/* In Range brings the writing end into range. */
		error = add_capability(layout, EV_KEY, BTN_TOOL_PEN);
		break;
	case HID_DIGITIZER_USAGE_INVERT:
		/* Invert brings the eraser end into range instead. */
		error = add_capability(layout, EV_KEY, BTN_TOOL_RUBBER);
		break;
	case HID_DIGITIZER_USAGE_TIP_SWITCH:
		/* The tip touching the surface is the contact. */
		error = add_capability(layout, EV_KEY, BTN_TOUCH);
		break;
	case HID_DIGITIZER_USAGE_ERASER:
		/* The eraser end is a tool of its own. */
		error = add_capability(layout, EV_KEY, BTN_TOOL_RUBBER);
		if (error != 0)
			return error;

		/* Its contact is reported as the contact of that tool. */
		error = add_capability(layout, EV_KEY, BTN_TOUCH);
		break;
	case HID_DIGITIZER_USAGE_BARREL_SWITCH:
		/* The first side button. */
		error = add_capability(layout, EV_KEY, BTN_STYLUS);
		break;
	case HID_DIGITIZER_USAGE_SECONDARY_BARREL:
		/* The second side button. */
		error = add_capability(layout, EV_KEY, BTN_STYLUS2);
		break;
	default:
		/* Refuses a switch the pen state machine does not know. */
		return EINVAL;
	}

	/* Reports a capability table that is full. */
	if (error != 0)
		return error;

	/* A device without In Range still has a pen tool while it touches. */
	error = add_capability(layout, EV_KEY, BTN_TOOL_PEN);
	if (error != 0)
		return error;

	/* Succeeded: the switch's events are declared. */
	return 0;
}

/* Reports which kind of pen collection the parser is inside, if any. */
static int
parser_in_pen(
	const struct hid_parser *parser)
{
	uint32_t usage;
	size_t depth;

	/* Looks for a pen application collection among the open ones. */
	for (depth = 0; depth < parser->collection_depth; depth++) {
		usage = parser->collection_usages[depth];

		/* A Pen collection is a pen on a display. */
		if (usage == HID_USAGE_PEN)
			return HID_REPORT_PEN_DISPLAY;

		/* A Digitizer collection is a pen on a separate tablet. */
		if (usage == HID_USAGE_DIGITIZER)
			return HID_REPORT_PEN_TABLET;
	}

	/* Reports that no pen collection is open. */
	return HID_REPORT_PEN_NONE;
}

/* Reads a Unit Exponent item, whose low nibble is a signed power of ten. */
static int32_t
unit_exponent_value(
	uint32_t raw)
{
	/* A value above the nibble is already a full signed number. */
	if (raw > 15U)
		return sign_extend(raw, 32U);

	/* The nibble values 8 to 15 stand for -8 to -1. */
	if (raw >= 8U)
		return (int32_t)raw - 16;

	/* The nibble values 0 to 7 stand for themselves. */
	return (int32_t)raw;
}

/*
 * Computes the resolution of an absolute axis from its physical size.
 *
 * A linear axis is in units per millimetre and a rotation in units per
 * radian, as evdev readers expect.  An axis without a physical size or a
 * known unit has resolution zero.
 */
static int32_t
axis_resolution(
	const struct hid_global_state *global,
	int32_t logical_minimum,
	int32_t logical_maximum)
{
	int64_t logical_span;
	int64_t physical_span;
	int64_t numerator;
	int64_t denominator;
	int32_t exponent;
	uint32_t system;

	/* The span of the logical values and of the physical size. */
	logical_span = (int64_t)logical_maximum - (int64_t)logical_minimum;
	physical_span = (int64_t)global->physical_maximum -
		(int64_t)global->physical_minimum;

	/* Refuses an axis that declares no physical size. */
	if (logical_span <= 0 || physical_span <= 0)
		return 0;

	/* Scales the numerator to the unit readers expect. */
	system = global->unit & 0x0fU;
	switch (system) {
	case HID_UNIT_SYSTEM_SI_LINEAR:
		/* Centimetres to millimetres. */
		numerator = logical_span;
		denominator = physical_span * 10;
		break;
	case HID_UNIT_SYSTEM_ENGLISH_LINEAR:
		/* Inches to millimetres (25.4 mm, kept in tenths). */
		numerator = logical_span * 10;
		denominator = physical_span * 254;
		break;
	case HID_UNIT_SYSTEM_SI_ROTATION:
		/* Radians already. */
		numerator = logical_span;
		denominator = physical_span;
		break;
	case HID_UNIT_SYSTEM_ENGLISH_ROTATION:
		/* Degrees to radians (180 / pi, kept in thousandths). */
		numerator = logical_span * 57296;
		denominator = physical_span * 1000;
		break;
	default:
		/* A unit the driver cannot convert has no resolution. */
		return 0;
	}

	/* Applies a positive power of ten, which makes the physical size larger. */
	exponent = unit_exponent_value(global->unit_exponent);
	while (exponent > 0 && denominator < INT64_MAX / 10) {
		denominator *= 10;
		exponent--;
	}

	/* Applies a negative power of ten, which makes the physical size smaller. */
	while (exponent < 0 && numerator < INT64_MAX / 10) {
		numerator *= 10;
		exponent++;
	}

	/* Refuses a resolution that does not fit the absinfo field. */
	if (numerator / denominator > INT32_MAX)
		return 0;

	/* Succeeded: the rounded units per millimetre or per radian. */
	return (int32_t)((numerator + denominator / 2) / denominator);
}

/* Stores the resolution of an absolute axis the layout already declares. */
static void
set_axis_resolution(
	struct hid_report_layout *layout,
	uint16_t code,
	int32_t resolution)
{
	struct input_abs_axis *axis;
	size_t index;

	/* Finds the axis and keeps the first resolution given to it. */
	for (index = 0; index < layout->absolute_axis_count; index++) {
		axis = &layout->absolute_axes[index];

		/* Skips the other axes. */
		if (axis->code != code)
			continue;

		/* Keeps a resolution an earlier field already set. */
		if (axis->info.resolution == 0)
			axis->info.resolution = resolution;

		/* The axis is found: no other entry has its code. */
		return;
	}
}

/* Asks whether a Touch Screen application collection is open. */
static int
parser_in_touch(
	const struct hid_parser *parser)
{
	size_t depth;

	/* Looks for the touch screen among the open collections. */
	for (depth = 0; depth < parser->collection_depth; depth++) {
		/* A Touch Screen collection holds the fingers. */
		if (parser->collection_usages[depth] == HID_USAGE_TOUCH_SCREEN)
			return 1;

		/* So does a Touch Pad collection (ws159-p003). */
		if (parser->collection_usages[depth] == HID_USAGE_TOUCH_PAD)
			return 1;
	}

	/* No touch screen is open. */
	return 0;
}

/* Asks whether a Touch Pad application collection is open (ws159-p003). */
static int
parser_in_touch_pad(
	const struct hid_parser *parser)
{
	size_t depth;

	/* Looks for the touch pad among the open collections. */
	for (depth = 0; depth < parser->collection_depth; depth++) {
		/* A Touch Pad collection's fingers and buttons are a pad's. */
		if (parser->collection_usages[depth] == HID_USAGE_TOUCH_PAD)
			return 1;
	}

	/* No touch pad is open. */
	return 0;
}

/*
 * Tells which touch item a usage of a touch screen is: HID_TOUCH_ITEM_* for
 * a finger's field, TOUCH_ITEM_CONTACT_COUNT for the Contact Count,
 * TOUCH_ITEM_SCAN_TIME for the Scan Time, 0 for a usage the touch screen
 * ignores.
 */
static unsigned
touch_item(
	uint32_t usage)
{
	uint16_t page;
	uint16_t value;

	/* Splits the usage into its page and its number. */
	page = (uint16_t)(usage >> 16U);
	value = (uint16_t)usage;

	/* The finger's X and Y are Generic Desktop usages. */
	if (page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
		if (value == HID_USAGE_X)
			return HID_TOUCH_ITEM_X;
		if (value == HID_USAGE_Y)
			return HID_TOUCH_ITEM_Y;
		return 0;
	}

	/* A touch pad's buttons 1 to 3 are Button page usages (ws159-p003). */
	if (page == HID_USAGE_PAGE_BUTTON) {
		if (value >= 1U && value <= HID_TOUCH_BUTTONS_MAX)
			return TOUCH_ITEM_BUTTON + value - 1U;
		return 0;
	}

	/* Everything else of a touch screen outside the Digitizer page is ignored. */
	if (page != HID_USAGE_PAGE_DIGITIZER)
		return 0;

	/* Chooses the Digitizer usages the touch state machine reads. */
	switch (value) {
	case HID_DIGITIZER_USAGE_TIP_SWITCH:
		return HID_TOUCH_ITEM_TIP;
	case HID_USAGE_CONFIDENCE:
		return HID_TOUCH_ITEM_CONFIDENCE;
	case HID_USAGE_CONTACT_ID:
		return HID_TOUCH_ITEM_CONTACT_ID;
	case HID_USAGE_CONTACT_COUNT:
		return TOUCH_ITEM_CONTACT_COUNT;
	case HID_USAGE_SCAN_TIME:
		return TOUCH_ITEM_SCAN_TIME;
	default:
		return 0;
	}
}

/*
 * Adds one field of a touch screen: a finger's item under the finger's
 * place in its report, or the report's Contact Count.  A usage the touch
 * screen does not read, a finger item outside a Finger collection, and a
 * finger past HID_TOUCH_CONTACTS_MAX add nothing.
 */
static int
add_touch_field(
	struct hid_parser *parser,
	struct hid_report_description *report,
	uint32_t bit_offset,
	uint32_t usage,
	int32_t logical_maximum)
{
	struct hid_report_layout *layout;
	unsigned item;
	uint16_t code;
	int pad;
	int error;

	/* Only the usages the touch state machine reads become fields. */
	layout = parser->layout;
	item = touch_item(usage);
	if (item == 0U)
		return 0;

	/* A touch screen's fields inside a Touch Pad collection are a pad's (ws159-p003). */
	pad = parser_in_touch_pad(parser);
	if (pad)
		layout->touch_pad = 1;

	/* The Contact Count belongs to the report, outside the fingers. */
	if (item >= TOUCH_ITEM_BUTTON && item < TOUCH_ITEM_BUTTON + HID_TOUCH_BUTTONS_MAX) {
		/* A pad's button belongs to the report too, and only a pad has one. */
		if (parser->finger_depth != 0U || !pad)
			return 0;
		code = HID_TOUCH_BUTTON_CODE(item - TOUCH_ITEM_BUTTON);
		if (item - TOUCH_ITEM_BUTTON + 1U > layout->touch_buttons)
			layout->touch_buttons = (uint8_t)(item - TOUCH_ITEM_BUTTON + 1U);
	} else if (item == TOUCH_ITEM_CONTACT_COUNT) {
		if (parser->finger_depth != 0U)
			return 0;
		code = HID_TOUCH_CONTACT_COUNT_CODE;
		layout->touch_count_present = 1;
	} else if (item == TOUCH_ITEM_SCAN_TIME) {
		/* So does the Scan Time, with its range and its unit. */
		if (parser->finger_depth != 0U)
			return 0;
		code = HID_TOUCH_SCAN_TIME_CODE;
		layout->touch_scan_present = 1;
		layout->touch_scan_maximum = logical_maximum;
		layout->touch_scan_unit_ns = scan_time_unit(&parser->global);
	} else {
		/* A finger's item outside a Finger collection belongs to no finger. */
		if (parser->finger_depth == 0U)
			return 0;

		/* The finger's first field gives it the next place in its report. */
		if (parser->finger_contact < 0) {
			if (report->touch_fingers >= HID_TOUCH_CONTACTS_MAX)
				return 0;
			parser->finger_contact = (int)report->touch_fingers;
			report->touch_fingers++;
			if (report->touch_fingers > layout->touch_contacts)
				layout->touch_contacts = report->touch_fingers;
		}

		/* The code names the finger and the item. */
		code = HID_TOUCH_CODE((unsigned)parser->finger_contact, item);

		/* The first finger's X and Y describe every finger's position. */
		if (item == HID_TOUCH_ITEM_X && !layout->touch_x_set) {
			touch_axis(&parser->global, logical_maximum, &layout->touch_x);
			layout->touch_x_set = 1;
		}

		/* And the first finger's Y. */
		if (item == HID_TOUCH_ITEM_Y && !layout->touch_y_set) {
			touch_axis(&parser->global, logical_maximum, &layout->touch_y);
			layout->touch_y_set = 1;
		}
	}

	/* Adds the field; the touch state machine declares its own events. */
	error = add_field(parser, report, bit_offset, usage, usage,
			  parser->global.logical_minimum, logical_maximum,
			  HID_REPORT_TYPE_TOUCH, code,
			  (uint8_t)parser->global.report_size, HID_FIELD_TOUCH);
	if (error != 0)
		return error;

	/* Succeeded: the field is part of the report. */
	return 0;
}

/*
 * Gives the unit of a Scan Time in nanoseconds: the field's Unit when it is
 * a time (seconds, to the power one, in any system) with a Unit Exponent
 * from -9 to 0, and otherwise 100 us, the unit Windows requires.  Unit and
 * Unit Exponent are global items, so a Scan Time declared after the
 * fingers' X and Y may still carry their length unit; that is not a time.
 */
static uint32_t
scan_time_unit(
	const struct hid_global_state *global)
{
	uint32_t unit_ns;
	int32_t exponent;
	int32_t power;

	/* A unit that is not a time, or no unit, gives the default. */
	if ((global->unit & 0x0fU) == 0U)
		return HID_TOUCH_SCAN_TIME_UNIT_NS;
	if ((global->unit & ~0x0fU) != 0x1000U)
		return HID_TOUCH_SCAN_TIME_UNIT_NS;

	/* So does an exponent the nanoseconds cannot express. */
	exponent = unit_exponent_value(global->unit_exponent);
	if (exponent < -9 || exponent > 0)
		return HID_TOUCH_SCAN_TIME_UNIT_NS;

	/* Ten to the power 9 + exponent nanoseconds. */
	unit_ns = 1U;
	for (power = 0; power < 9 + exponent; power++)
		unit_ns *= 10U;

	/* Succeeded: the Scan Time's unit. */
	return unit_ns;
}

/* Describes a finger's position axis from the field's logical range and physical size. */
static void
touch_axis(
	const struct hid_global_state *global,
	int32_t logical_maximum,
	struct input_absinfo *info)
{
	/* The logical range, at rest at its minimum, with the resolution its size implies. */
	info->value = global->logical_minimum;
	info->minimum = global->logical_minimum;
	info->maximum = logical_maximum;
	info->fuzz = 0;
	info->flat = 0;
	info->resolution = axis_resolution(global, global->logical_minimum,
					   logical_maximum);
}

/*
 * Takes one feature main item (ws159-p003): its fields are laid after the
 * ones before them in their report, and the fields of the Precision
 * Touchpad's usages are kept for the driver that sets or reads them.
 */
static int
parse_feature(
	struct hid_parser *parser,
	uint32_t flags)
{
	struct hid_report_layout *layout;
	struct hid_feature_report *report;
	struct hid_feature_field *field;
	uint32_t count;
	uint32_t bits;
	uint32_t index;
	uint32_t usage;
	uint32_t bit_offset;
	int kept;
	int found;

	/* A feature item without a size or a count is malformed. */
	layout = parser->layout;
	if (parser->global.report_size == 0U || parser->global.report_count == 0U)
		return EINVAL;
	if (parser->global.report_size > HID_REPORT_BITS_MAX || parser->global.report_count > HID_REPORT_BITS_MAX)
		return E2BIG;

	/* The report the item belongs to. */
	report = feature_report(layout, parser->global.report_id);
	if (report == NULL)
		return E2BIG;

	/* Refuses a report that grows past what a report may hold. */
	count = parser->global.report_count;
	if (count > (HID_REPORT_BITS_MAX - report->bit_count) / parser->global.report_size)
		return E2BIG;
	bits = count * parser->global.report_size;
	bit_offset = report->bit_count;

	/* A constant item is padding: it only moves the next field on. */
	if ((flags & HID_INPUT_CONSTANT) != 0U) {
		report->bit_count += bits;
		return 0;
	}

	/* Keeps each field of a usage a driver needs. */
	for (index = 0; index < count; index++) {
		/* A field without a usage, or of another usage, is not kept. */
		found = local_usage_at(&parser->local, index, &usage);
		if (!found)
			continue;
		kept = hid_feature_usage(usage);
		if (!kept)
			continue;

		/* A layout with no room keeps no more fields. */
		if (layout->feature_field_count >= HID_FEATURE_FIELDS_MAX)
			break;

		/* The field: its usage, its report and its place. */
		field = &layout->feature_fields[layout->feature_field_count];
		field->usage = usage;
		field->report_id = parser->global.report_id;
		field->bit_size = (uint8_t)parser->global.report_size;
		field->bit_offset = bit_offset + index * parser->global.report_size;
		layout->feature_field_count++;
	}

	/* Succeeded: the report is longer by the item. */
	report->bit_count += bits;
	return 0;
}

/* Tells whether a usage is one of the Precision Touchpad's feature usages. */
static int
hid_feature_usage(
	uint32_t usage)
{
	/* The usages of HID_REPORT_USAGE_*. */
	switch (usage) {
	case HID_REPORT_USAGE_DEVICE_MODE:
	case HID_REPORT_USAGE_CONTACT_COUNT_MAXIMUM:
	case HID_REPORT_USAGE_SURFACE_SWITCH:
	case HID_REPORT_USAGE_BUTTON_SWITCH:
	case HID_REPORT_USAGE_PAD_TYPE:
	case HID_REPORT_USAGE_LATENCY_MODE:
		return 1;
	default:
		return 0;
	}
}

/* Finds the feature report of an identifier, adding it when it is new; NULL when the table is full. */
static struct hid_feature_report *
feature_report(
	struct hid_report_layout *layout,
	uint8_t id)
{
	struct hid_feature_report *report;
	size_t index;

	/* A report already seen. */
	for (index = 0; index < layout->feature_report_count; index++) {
		if (layout->feature_reports[index].id == id)
			return &layout->feature_reports[index];
	}

	/* No room for another report. */
	if (layout->feature_report_count >= HID_REPORT_ID_COUNT_MAX)
		return NULL;

	/* A new report, empty. */
	report = &layout->feature_reports[layout->feature_report_count];
	report->id = id;
	report->bit_count = 0;
	layout->feature_report_count++;

	/* Succeeded: the report. */
	return report;
}
