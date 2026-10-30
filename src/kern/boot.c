/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Boot parameters and boot sources.
 *
 * The boot loader hands the kernel a parameter string of space-separated
 * name=value tokens naming the boot partitions, the root, the swap
 * sources, and the init path.  A boot source context mounts each named
 * FAT boot partition privately, resolves boot<n>:<path> references into
 * them, and later publishes the retained mounts for the running system.
 */

#include "kern/boot.h"
#include <kern/kcrt.h>

#include "kern/block-identity.h"
#include "kern/fat.h"
#include "kern/namei.h"

#include <uapi/errno.h>

struct parameter_name {
	const char *text;
	size_t length;
};

#define PARAMETER_NAME(value) { value, sizeof(value) - 1U }

/* The largest width or height, and the largest refresh rate, display.mode= takes. */
#define DISPLAY_MODE_SIZE_MAX		16384U
#define DISPLAY_MODE_REFRESH_MAX	1000U

static const struct parameter_name parameter_names[KERN_BOOT_PARAMETER_COUNT] = {
	PARAMETER_NAME("boot0"),
	PARAMETER_NAME("boot1"),
	PARAMETER_NAME("boot2"),
	PARAMETER_NAME("boot3"),
	PARAMETER_NAME("rootpart"),
	PARAMETER_NAME("overlay-root"),
	PARAMETER_NAME("overlay-data"),
	PARAMETER_NAME("swap0"),
	PARAMETER_NAME("swap1"),
	PARAMETER_NAME("swap2"),
	PARAMETER_NAME("swap3"),
	PARAMETER_NAME("init"),
	PARAMETER_NAME("kmsg"),
	PARAMETER_NAME("login"),
	PARAMETER_NAME("display"),
	PARAMETER_NAME("display.mode"),
	PARAMETER_NAME("i915.start"),
	PARAMETER_NAME("i915.debug"),
	PARAMETER_NAME("i915.gen9"),
	PARAMETER_NAME("dc8200.zerocopy"),
	PARAMETER_NAME("mac0"),
	PARAMETER_NAME("cpuperf"),
	PARAMETER_NAME("cpuperf.hwp"),
};

static char firmware_source[KERN_BOOT_SOURCE_SELECTOR_SIZE];
static char configuration_source[KERN_BOOT_SOURCE_SELECTOR_SIZE];
static uint64_t configuration_matches;
static int provenance_selector(const struct kern_boot_partition_identity *identity, char *output);

static struct kern_boot_parameters current_parameters;
static int current_parameters_valid;
static int current_parameters_source_present;

static void parameters_reset(struct kern_boot_parameters *parameters);
static int parse_error(struct kern_boot_parameters *parameters, int error);
static int name_matches(const char *name, size_t length, const struct parameter_name *candidate);
static int parameter_key(const char *name, size_t length, enum kern_boot_parameter_key *key);
static void record_unknown(struct kern_boot_parameters *parameters, const char *name, size_t length);
static int parameter_separator(char character);
static int parameter_word(enum kern_boot_parameter_key key, const char *value, size_t length);
static int parameter_text_is(const char *value, size_t length, const char *word);
static int display_mode_number(const char *text, size_t length, size_t *position, uint32_t maximum, uint32_t *value);
static size_t skip_separators(const char *text, size_t position, size_t length);
static int selector_text(const char *text, size_t maximum, int device_name);
static int context_fail(struct kern_boot_source_context *context, unsigned slot, enum kern_boot_source_failure_stage stage, int error);
static int runtime_mount_lookup(struct kern_boot_source_slot *source, const char *relative, struct path *result);

/* Copies boot provenance before firmware storage can be reclaimed. */
int
kern_boot_provenance_set(const struct kern_boot_provenance *record)
{
	char firmware[KERN_BOOT_SOURCE_SELECTOR_SIZE];
	char configuration[KERN_BOOT_SOURCE_SELECTOR_SIZE];
	int error;

	firmware_source[0] = '\0';
	configuration_source[0] = '\0';
	configuration_matches = 0;
	if (record == NULL)
		return 0;
	if (record->version != KERN_BOOT_PROVENANCE_VERSION ||
	    record->config_matches == 0 || record->config_matches > 128U)
		return EINVAL;
	error = provenance_selector(&record->firmware, firmware);
	if (error == 0)
		error = provenance_selector(&record->configuration, configuration);
	if (error != 0)
		return error;
	kern_memcpy(firmware_source, firmware, sizeof(firmware_source));
	kern_memcpy(configuration_source, configuration, sizeof(configuration_source));
	configuration_matches = record->config_matches;
	return 0;
}

/* Returns an immutable selector, not the possibly overridden boot0 setting. */
const char *
kern_boot_source_selector(unsigned configuration)
{
	const char *source;

	source = configuration ? configuration_source : firmware_source;
	return source[0] != '\0' ? source : "unavailable";
}

uint64_t
kern_boot_config_matches(void)
{
	return configuration_matches;
}

/*
 * Parses a boot parameter string into a parameter record.
 *
 * The input must be terminated ASCII within the storage size.  Tokens are
 * printable characters separated by spaces, tabs or line ends.  A token of
 * the form name=value with a known name sets that parameter, which may
 * appear once and needs a value.  Anything else -- an unknown name, or a
 * token with no equals sign, such as the rootwait or quiet a firmware adds
 * for Linux -- is ignored and counted, and the first such name is
 * remembered for diagnostics (2026-09-27 user decision: the line a board's
 * firmware passes is not zedBSD's alone).  Any error leaves the record
 * empty.
 */
int
kern_boot_parameters_parse(
	struct kern_boot_parameters *parameters,
	const char *input,
	size_t input_capacity)
{
	enum kern_boot_parameter_key key;
	size_t length;
	size_t scan_limit;
	size_t position;
	size_t token_start;
	size_t token_end;
	size_t next;
	size_t equal;
	size_t value_start;
	size_t value_length;
	uint32_t mode_width;
	uint32_t mode_height;
	uint32_t mode_refresh;
	unsigned char byte;
	unsigned char token_byte;
	int terminated;
	int known;
	int separator;
	int error;

	length = 0;
	terminated = 0;

	/* Rejects a missing record. */
	if (parameters == NULL)
		return EINVAL;
	parameters_reset(parameters);

	/* No input is fine only when no capacity was claimed for it. */
	if (input == NULL) {
		if (input_capacity == 0U)
			return 0;
		return EINVAL;
	}

	if (input_capacity == 0U)
		return EINVAL;

	/* Finds the terminator within the storage size, refusing non-ASCII. */
	scan_limit = input_capacity;
	if (scan_limit > KERN_BOOT_PARAMETERS_STORAGE_SIZE)
		scan_limit = KERN_BOOT_PARAMETERS_STORAGE_SIZE;
	for (length = 0; length < scan_limit; length++) {
		byte = (unsigned char)input[length];
		if (byte == 0U) {
			terminated = 1;
			break;
		}

		if (byte > 0x7fU) {
			error = parse_error(parameters, EILSEQ);
			return error;
		}
	}

	if (!terminated) {
		if (input_capacity >= KERN_BOOT_PARAMETERS_STORAGE_SIZE)
			error = parse_error(parameters, E2BIG);
		else
			error = parse_error(parameters, EINVAL);
		return error;
	}

	/* Works on a private copy that the tokens are terminated in. */
	for (position = 0; position <= length; position++)
		parameters->storage[position] = input[position];

	/* Walks the tokens. */
	position = 0;
	while (position < length) {
		/* Skips the separators before the token. */
		position = skip_separators(parameters->storage, position,
		    length);
		if (position == length)
			break;

		/* Delimits a token of printable characters. */
		token_start = position;
		while (position < length) {
			/* A separator ends the token. */
			separator = parameter_separator(
			    parameters->storage[position]);
			if (separator)
				break;

			/* Refuses a control or non-ASCII character in a token. */
			token_byte =
			    (unsigned char)parameters->storage[position];
			if (token_byte < 0x21U || token_byte > 0x7eU) {
				error = parse_error(parameters, EINVAL);
				return error;
			}

			position++;
		}

		/* Finds where the next token may start. */
		token_end = position;
		next = skip_separators(parameters->storage, token_end, length);

		/* Finds the equals sign that splits a name from its value. */
		equal = token_start;
		while (equal < token_end && parameters->storage[equal] != '=')
			equal++;

		/* A token that names nothing known is counted and passed over. */
		known = 0;
		if (equal != token_start && equal != token_end) {
			known = parameter_key(parameters->storage + token_start,
					      equal - token_start,
					      &key);
		}

		if (!known) {
			record_unknown(parameters,
				       parameters->storage + token_start,
				       equal - token_start);
			position = next;
			continue;
		}

		/* A known name needs a value. */
		if (equal + 1U == token_end) {
			error = parse_error(parameters, EINVAL);
			return error;
		}

		/* A known name may appear once. */
		value_start = equal + 1U;
		value_length = token_end - value_start;
		if (parameters->value_offset[key] != KERN_BOOT_PARAMETER_OFFSET_ABSENT) {
			error = parse_error(parameters, EEXIST);
			return error;
		}

		/* The init path must be absolute and bounded. */
		if (key == KERN_BOOT_PARAMETER_INIT) {
			if (parameters->storage[value_start] != '/') {
				error = parse_error(parameters, EINVAL);
				return error;
			}

			if (value_length > KERN_BOOT_PARAMETERS_INIT_PATH_MAX) {
				error = parse_error(parameters, ENAMETOOLONG);
				return error;
			}
		}

		/*
		 * kmsg=, login=, display=, i915.start=, i915.debug=,
		 * i915.gen9=, dc8200.zerocopy=, cpuperf= and cpuperf.hwp=
		 * take one of their words.
		 */
		if (key == KERN_BOOT_PARAMETER_KMSG ||
		    key == KERN_BOOT_PARAMETER_LOGIN ||
		    key == KERN_BOOT_PARAMETER_DISPLAY ||
		    key == KERN_BOOT_PARAMETER_I915_START ||
		    key == KERN_BOOT_PARAMETER_I915_DEBUG ||
		    key == KERN_BOOT_PARAMETER_I915_GEN9 ||
		    key == KERN_BOOT_PARAMETER_DC8200_ZEROCOPY ||
		    key == KERN_BOOT_PARAMETER_CPUPERF ||
		    key == KERN_BOOT_PARAMETER_CPUPERF_HWP) {
			error = parameter_word(key, parameters->storage + value_start, value_length);
			if (error != 0) {
				error = parse_error(parameters, error);
				return error;
			}
		}

		/* display.mode= is a mode written WxH or WxH@R. */
		if (key == KERN_BOOT_PARAMETER_DISPLAY_MODE) {
			error = kern_boot_display_mode_parse(parameters->storage + value_start, value_length, &mode_width, &mode_height, &mode_refresh);
			if (error != 0) {
				error = parse_error(parameters, error);
				return error;
			}
		}

		/* Terminates the name and the value in place. */
		if (token_end < length)
			parameters->storage[token_end] = '\0';
		parameters->storage[equal] = '\0';

		/* Records the value. */
		parameters->value_offset[key] = (uint16_t)value_start;

		position = next;
	}

	/* Reports the parsed record. */
	return 0;
}

/*
 * Checks the word of kmsg= (quiet or console), login= (graphical or console),
 * display= (auto, hdmi, edp or panel), i915.start= (auto or manual),
 * i915.debug= (off or display), i915.gen9= (on or off),
 * dc8200.zerocopy= (off or test), cpuperf= (on or off) or cpuperf.hwp=
 * (on or off).
 */
static int
parameter_word(
	enum kern_boot_parameter_key key,
	const char *value,
	size_t length)
{
	int console;
	int other;

	/* display= takes the automatic choice (the external display, then the panel), HDMI, or the panel (edp, panel). */
	if (key == KERN_BOOT_PARAMETER_DISPLAY) {
		other = parameter_text_is(value, length, "auto");
		if (!other)
			other = parameter_text_is(value, length, "hdmi");
		if (!other)
			other = parameter_text_is(value, length, "edp");
		if (!other)
			other = parameter_text_is(value, length, "panel");
		if (!other)
			return EINVAL;

		/* Succeeded: one of display='s words. */
		return 0;
	}

	/*
	 * i915.start= starts the i915's devices by themselves once the kernel
	 * is ready (auto), or holds them until root asks with hw.gpu.start
	 * (manual).
	 */
	if (key == KERN_BOOT_PARAMETER_I915_START) {
		other = parameter_text_is(value, length, "auto");
		if (!other)
			other = parameter_text_is(value, length, "manual");
		if (!other)
			return EINVAL;

		/* Succeeded: one of i915.start='s words. */
		return 0;
	}

	/*
	 * i915.debug= keeps the i915's display path quiet (off), logs it in
	 * detail (display), offers the video decode engine that is not yet
	 * confirmed on hardware (video), or both (display,video).
	 */
	if (key == KERN_BOOT_PARAMETER_I915_DEBUG) {
		other = parameter_text_is(value, length, "off");
		if (!other)
			other = parameter_text_is(value, length, "display");
		if (!other)
			other = parameter_text_is(value, length, "video");
		if (!other)
			other = parameter_text_is(value, length, "display,video");
		if (!other)
			return EINVAL;

		/* Succeeded: one of i915.debug='s words. */
		return 0;
	}

	/*
	 * i915.gen9= lets the i915 drive a Gen9 GPU (Skylake to Comet Lake),
	 * on (the default when absent), or leaves it to the firmware, off.
	 * The i915 reads it from the loader's line at PCI attach, before this
	 * parse.
	 */
	if (key == KERN_BOOT_PARAMETER_I915_GEN9) {
		other = parameter_text_is(value, length, "on");
		if (!other)
			other = parameter_text_is(value, length, "off");
		if (!other)
			return EINVAL;

		/* Succeeded: one of i915.gen9='s words. */
		return 0;
	}

	/*
	 * dc8200.zerocopy= lets the VisionFive 2's display scan a program's
	 * render buffer directly (test, and the default when absent), or
	 * keeps every frame copied (off).
	 */
	if (key == KERN_BOOT_PARAMETER_DC8200_ZEROCOPY) {
		other = parameter_text_is(value, length, "off");
		if (!other)
			other = parameter_text_is(value, length, "test");
		if (!other)
			return EINVAL;

		/* Succeeded: one of dc8200.zerocopy='s words. */
		return 0;
	}

	/*
	 * cpuperf= turns the x86 processor driver off (off) or leaves it on
	 * (on, the default when absent); cpuperf.hwp= keeps it from turning
	 * HWP on (off) or lets it (on, the default).  The driver reads them
	 * from the loader's line when it attaches, before this parse.
	 */
	if (key == KERN_BOOT_PARAMETER_CPUPERF ||
	    key == KERN_BOOT_PARAMETER_CPUPERF_HWP) {
		other = parameter_text_is(value, length, "on");
		if (!other)
			other = parameter_text_is(value, length, "off");
		if (!other)
			return EINVAL;

		/* Succeeded: one of cpuperf='s or cpuperf.hwp='s words. */
		return 0;
	}

	/* kmsg= and login= both take console. */
	console = parameter_text_is(value, length, "console");
	if (console)
		return 0;

	/* kmsg= also takes quiet, login= also takes graphical. */
	other = 0;
	if (key == KERN_BOOT_PARAMETER_KMSG)
		other = parameter_text_is(value, length, "quiet");
	else if (key == KERN_BOOT_PARAMETER_LOGIN)
		other = parameter_text_is(value, length, "graphical");
	if (!other)
		return EINVAL;

	/* Succeeded: a word the parameter takes. */
	return 0;
}

/* Reports whether a counted text is exactly a terminated word. */
static int
parameter_text_is(
	const char *value,
	size_t length,
	const char *word)
{
	size_t index;

	/* Each character of the text against the word's. */
	for (index = 0; index < length; index++) {
		if (word[index] == '\0' || word[index] != value[index])
			return 0;
	}

	/* The word ends where the text does. */
	if (word[length] != '\0')
		return 0;

	/* Succeeded: the same word. */
	return 1;
}

/* Reads one decimal number of a display mode: 1 to maximum, at most five digits. */
static int
display_mode_number(
	const char *text,
	size_t length,
	size_t *position,
	uint32_t maximum,
	uint32_t *value)
{
	uint32_t number;
	unsigned digits;

	/* The digits from the position on. */
	number = 0U;
	digits = 0U;
	while (*position < length &&
	    text[*position] >= '0' &&
	    text[*position] <= '9') {
		/* Refuses a number longer than any size or rate the mode takes. */
		if (digits == 5U)
			return EINVAL;

		/* Takes the digit. */
		number = number * 10U + (uint32_t)(text[*position] - '0');
		digits++;
		(*position)++;
	}

	/* Refuses no digits, zero and a number over the maximum. */
	if (digits == 0U)
		return EINVAL;
	if (number == 0U || number > maximum)
		return EINVAL;

	/* Succeeded: the number is read. */
	*value = number;
	return 0;
}

/*
 * Reads the value of a parameter, or none when it was not given.
 */
const char *
kern_boot_parameters_value(
	const struct kern_boot_parameters *parameters,
	enum kern_boot_parameter_key key)
{
	uint16_t offset;

	/* Rejects a missing record or an unknown key. */
	if (parameters == NULL || (unsigned)key >= KERN_BOOT_PARAMETER_COUNT)
		return NULL;

	/* An absent parameter has no value. */
	offset = parameters->value_offset[key];
	if (offset == KERN_BOOT_PARAMETER_OFFSET_ABSENT)
		return NULL;

	/* Reports the value in the record's storage. */
	return parameters->storage + offset;
}

/*
 * Reads the selector of a boot partition slot.
 */
const char *
kern_boot_parameters_boot(
	const struct kern_boot_parameters *parameters,
	unsigned index)
{
	const char *value;

	/* Only four boot slots exist. */
	if (index >= 4U)
		return NULL;

	value = kern_boot_parameters_value(
		parameters,
		(enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_BOOT0 + index));

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the selector of a swap source slot.
 */
const char *
kern_boot_parameters_swap(
	const struct kern_boot_parameters *parameters,
	unsigned index)
{
	const char *value;

	/* Only four swap slots exist. */
	if (index >= 4U)
		return NULL;

	value = kern_boot_parameters_value(
		parameters,
		(enum kern_boot_parameter_key)(KERN_BOOT_PARAMETER_SWAP0 + index));

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the root partition selector.
 */
const char *
kern_boot_parameters_rootpart(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_ROOTPART);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the overlay root selector.
 */
const char *
kern_boot_parameters_overlay_root(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_OVERLAY_ROOT);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the overlay data selector.
 */
const char *
kern_boot_parameters_overlay_data(
	const struct kern_boot_parameters *parameters)
{
	const char *value;

	value = kern_boot_parameters_value(
		parameters,
		KERN_BOOT_PARAMETER_OVERLAY_DATA);

	/* Reports the selector, or none. */
	return value;
}

/*
 * Reads the init path, defaulting to /sbin/init.
 */
const char *
kern_boot_parameters_init_path(
	const struct kern_boot_parameters *parameters)
{
	const char *path;

	/* Falls back to the default when no path was given. */
	path = kern_boot_parameters_value(parameters, KERN_BOOT_PARAMETER_INIT);
	if (path == NULL)
		return "/sbin/init";

	/* Reports the given path. */
	return path;
}

/*
 * Reports how many unknown parameters were seen.
 */
unsigned
kern_boot_parameters_unknown_count(
	const struct kern_boot_parameters *parameters)
{
	/* A missing record saw nothing. */
	if (parameters == NULL)
		return 0U;

	/* Reports the count. */
	return parameters->unknown_count;
}

/*
 * Reads the first unknown parameter name, noting whether it was truncated.
 */
const char *
kern_boot_parameters_unknown_name(
	const struct kern_boot_parameters *parameters,
	int *truncated)
{
	/* Reports the truncation when asked. */
	if (truncated != NULL) {
		if (parameters != NULL && parameters->unknown_name_truncated != 0U)
			*truncated = 1;
		else
			*truncated = 0;
	}

	/* There is no name without an unknown parameter. */
	if (parameters == NULL || parameters->unknown_count == 0U)
		return NULL;

	/* Reports the remembered name. */
	return parameters->unknown_name;
}

/*
 * Reports whether a whole token is in a boot parameter string.
 *
 * The string is read up to its terminator or the parameter storage size,
 * whichever comes first; tokens are separated as the parser separates them.
 */
int
kern_boot_parameters_token_present(
	const char *text,
	const char *token)
{
	size_t position;
	size_t start;
	size_t length;
	int separator;
	int same;

	/* No string has no tokens. */
	if (text == NULL || token == NULL)
		return 0;

	/* Each token, compared whole. */
	position = 0;
	while (position < KERN_BOOT_PARAMETERS_STORAGE_SIZE && text[position] != '\0') {
		/* Steps over the separators. */
		separator = parameter_separator(text[position]);
		if (separator) {
			position++;
			continue;
		}

		/* The token's extent. */
		start = position;
		for (;;) {
			if (position >= KERN_BOOT_PARAMETERS_STORAGE_SIZE || text[position] == '\0')
				break;
			separator = parameter_separator(text[position]);
			if (separator)
				break;
			position++;
		}

		/* The same token, of the same length. */
		length = position - start;
		same = parameter_text_is(text + start, length, token);
		if (same)
			return 1;
	}

	/* Not there. */
	return 0;
}

/*
 * Reads a display mode written WxH or WxH@R.
 *
 * The width and height are 1 to 16384 pixels and the refresh rate 1 to
 * 1000 Hz; a mode without @R reports a refresh rate of 0 (the display's
 * own choice).  Nothing else may follow.
 */
int
kern_boot_display_mode_parse(
	const char *text,
	size_t length,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_hz)
{
	size_t position;
	int error;

	/* Refuses a call without its text or its results. */
	if (text == NULL ||
	    width == NULL ||
	    height == NULL ||
	    refresh_hz == NULL)
		return EINVAL;

	/* The width, then the x. */
	position = 0;
	error = display_mode_number(text, length, &position, DISPLAY_MODE_SIZE_MAX, width);
	if (error != 0)
		return error;
	if (position >= length || text[position] != 'x')
		return EINVAL;

	/* The height after the x. */
	position++;
	error = display_mode_number(text, length, &position, DISPLAY_MODE_SIZE_MAX, height);
	if (error != 0)
		return error;

	/* A mode without a refresh rate ends here. */
	*refresh_hz = 0U;
	if (position == length)
		return 0;

	/* Only @R may follow the height. */
	if (text[position] != '@')
		return EINVAL;

	/* The refresh rate after the @, and nothing after it. */
	position++;
	error = display_mode_number(text, length, &position, DISPLAY_MODE_REFRESH_MAX, refresh_hz);
	if (error != 0)
		return error;
	if (position != length)
		return EINVAL;

	/* Succeeded: the mode is read. */
	return 0;
}

/*
 * Parses the boot parameters the kernel keeps for the whole system.
 */
int
kern_boot_parameters_initialize(
	const char *input,
	size_t input_capacity)
{
	int error;

	/* Parses into the system record and remembers whether it is valid. */
	error = kern_boot_parameters_parse(&current_parameters, input, input_capacity);
	current_parameters_valid = error == 0;
	current_parameters_source_present = error == 0 && input != NULL;

	/* Reports why the parse failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the system boot parameters, or none before they are valid.
 */
const struct kern_boot_parameters *
kern_boot_parameters_current(
	void)
{
	/* The record exists only after a successful parse. */
	if (!current_parameters_valid)
		return NULL;
	return &current_parameters;
}

/*
 * Tests whether the boot loader supplied a parameter string.
 */
int
kern_boot_parameters_source_present(
	void)
{
	/* Both a valid parse and an actual input are required. */
	if (!current_parameters_valid)
		return 0;
	if (!current_parameters_source_present)
		return 0;

	/* Reports a supplied string. */
	return 1;
}

/*
 * Validates the spelling of a device selector.
 *
 * A selector is a /dev/ name, a UUID=, LABEL=, PARTUUID=, or PARTLABEL=
 * identity, or a bare device name.
 */
int
kern_boot_source_selector_validate(
	const char *selector)
{
	unsigned index;
	int error;

	/* The identity prefixes a selector may carry. */
	static const struct {
		const char *prefix;
		size_t length;
	} identities[] = {
		{ "UUID=", 5U },
		{ "LABEL=", 6U },
		{ "PARTUUID=", 9U },
		{ "PARTLABEL=", 10U },
	};

	/* Rejects a missing or empty selector. */
	if (selector == NULL || selector[0] == '\0')
		return EINVAL;

	/* A device path carries a device name. */
	if (kern_strncmp(selector, "/dev/", 5U) == 0) {
		error = selector_text(selector + 5U, DISK_NAME_MAX, 1);
		return error;
	}

	/* An identity prefix carries identity text. */
	for (index = 0; index < sizeof(identities) / sizeof(identities[0]); index++) {
		if (kern_strncmp(selector,
			    identities[index].prefix,
			    identities[index].length) == 0) {
			error = selector_text(selector + identities[index].length,
					     DISK_IDENTITY_TEXT_MAX, 0);
			return error;
		}
	}

	/* Anything else with an equals sign is an unknown identity. */
	if (kern_strchr(selector, '=') != NULL)
		return EINVAL;

	/* A bare name is a device name. */

	/* Reports why the validation failed. */
	error = selector_text(selector, DISK_NAME_MAX, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Parses a boot<n>:<path> reference into its slot and relative path.
 *
 * The path may not contain empty, dot, or dot-dot components.
 */
int
kern_boot_source_reference_parse(
	const char *text,
	struct kern_boot_source_reference *reference)
{
	const char *path;
	size_t length;
	size_t component;
	unsigned char byte;

	length = 0;
	component = 0;

	/* Rejects anything but boot<0-3>: followed by a path. */
	if (text == NULL ||
	    reference == NULL ||
	    text[0] != 'b' ||
	    text[1] != 'o' ||
	    text[2] != 'o' ||
	    text[3] != 't' ||
	    text[4] < '0' ||
	    text[4] > '3' ||
	    text[5] != ':')
		return EINVAL;
	path = text + 6U;
	if (*path == '/')
		path++;
	if (*path == '\0')
		return EINVAL;

	/* Checks every component and character of the path. */
	for (;;) {
		byte = (unsigned char)path[length];
		if (byte == '\0' || byte == '/') {
			if (length == component ||
			    (length - component == 1U && path[component] == '.') ||
			    (length - component == 2U &&
			     path[component] == '.' &&
			     path[component + 1U] == '.'))
				return EINVAL;
			if (byte == '\0')
				break;
			component = length + 1U;
		} else if (byte < 0x20U || byte > 0x7eU || byte == '\\') {
			return EINVAL;
		}

		length++;
		if (length >= sizeof(reference->relative))
			return ENAMETOOLONG;
	}

	/* Records the slot and the path. */
	reference->slot = (unsigned)(text[4] - '0');
	kern_memcpy(reference->relative, path, length + 1U);

	/* Reports the parsed reference. */
	return 0;
}

/*
 * Decides the root mode from the root and overlay parameters.
 *
 * A root partition alone means a native root; the two overlay
 * parameters together mean an overlay root.  Any other combination is
 * invalid.
 */
int
kern_boot_source_root_mode(
	const char *rootpart,
	const char *overlay_root,
	const char *overlay_data,
	enum kern_boot_root_mode *mode)
{
	/* Rejects a missing result. */
	if (mode == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_INVALID;

	/* A root partition excludes the overlay parameters. */
	if (rootpart != NULL) {
		if (overlay_root != NULL || overlay_data != NULL)
			return EINVAL;
		*mode = KERN_BOOT_ROOT_NATIVE;
		return 0;
	}

	/* An overlay root needs both overlay parameters. */
	if (overlay_root == NULL || overlay_data == NULL)
		return EINVAL;
	*mode = KERN_BOOT_ROOT_OVERLAY;

	/* Reports the overlay mode. */
	return 0;
}

/*
 * Tests whether a FAT variant may hold a boot source.
 */
int
kern_boot_source_fat_type_supported(
	enum bootfat_type type)
{
	/* Only FAT16 and FAT32 are supported. */
	if (type == KERN_FAT16)
		return 1;
	if (type == KERN_FAT32)
		return 1;

	/* Reports an unsupported variant. */
	return 0;
}

/*
 * Names a boot source failure stage for diagnostics.
 */
const char *
kern_boot_source_failure_stage_name(
	enum kern_boot_source_failure_stage stage)
{
	switch (stage) {
	case KERN_BOOT_SOURCE_FAILURE_SELECTOR:
		return "selector validation";
	case KERN_BOOT_SOURCE_FAILURE_RESOLVE:
		return "selector resolution";
	case KERN_BOOT_SOURCE_FAILURE_PARTITION:
		return "partition validation";
	case KERN_BOOT_SOURCE_FAILURE_DUPLICATE:
		return "duplicate detection";
	case KERN_BOOT_SOURCE_FAILURE_FILESYSTEM:
		return "FAT16/FAT32 validation";
	case KERN_BOOT_SOURCE_FAILURE_MOUNT:
		return "private mount";
	default:
		return "unknown stage";
	}
}

/*
 * Initializes an empty boot source context.
 */
void
kern_boot_source_context_init(
	struct kern_boot_source_context *context)
{
	/* Ignores a missing context. */
	if (context != NULL)
		kern_memset(context, 0, sizeof(*context));
}

/*
 * Unmounts every boot source of a context that was not published.
 *
 * The first unmount error is reported after every slot was tried.
 */
int
kern_boot_source_context_destroy(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing context. */
	if (context == NULL)
		return EINVAL;

	/* A published context is an immutable system-lifetime resolver. */
	if (context->runtime_published)
		return EBUSY;

	/* Unmounts the slots in reverse order, keeping the first failure. */
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		source = &context->slot[slot - 1U];
		if (source->mount == NULL)
			continue;
		error = unmount_private(source->mount);
		if (error != 0) {
			if (first_error == 0)
				first_error = error;
			continue;
		}

		kern_memset(source, 0, sizeof(*source));
	}

	/* Reports the first unmount failure. */
	return first_error;
}

/*
 * Mounts every boot partition named in the parameters.
 *
 * Slot 0 falls back to the loader's origin partition when no selector
 * names it.  Each partition must be a FAT16 or FAT32 partition that no
 * earlier slot uses.  On failure the context records the failing slot
 * and stage and is destroyed.
 */
int
kern_boot_source_context_mount(
	struct kern_boot_source_context *context,
	const struct kern_boot_parameters *parameters,
	struct disk *loader_origin,
	const char *loader_origin_selector)
{
	struct disk *disk;
	const char *selector;
	enum bootfat_type fat_type;
	unsigned slot;
	unsigned previous;
	int error;

	/* Rejects a missing operand or a context that is published or in use. */
	if (context == NULL || parameters == NULL)
		return EINVAL;
	if (context->runtime_published)
		return EBUSY;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].mount != NULL)
			return EBUSY;
	}

	context->failure_stage = KERN_BOOT_SOURCE_FAILURE_NONE;
	context->cleanup_error = 0;

	/* Mounts each named slot, and slot 0 from the loader origin if unnamed. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		selector = kern_boot_parameters_boot(parameters, slot);
		disk = NULL;
		if (selector == NULL && slot != 0U)
			continue;

		/* Resolves the selector, the origin selector, or the origin disk. */
		if (selector != NULL) {
			error = kern_boot_source_selector_validate(selector);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
				return error;
			}

			error = block_identity_resolve(selector, &disk);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
				return error;
			}
		} else if (loader_origin_selector != NULL) {
			error = kern_boot_source_selector_validate(
			    loader_origin_selector);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_SELECTOR, error);
				return error;
			}

			error = block_identity_resolve(loader_origin_selector, &disk);
			if (error != 0) {
				error = context_fail(context, slot,
				    KERN_BOOT_SOURCE_FAILURE_RESOLVE, error);
				return error;
			}
		} else if (loader_origin != NULL) {
			disk = loader_origin;
			disk_ref(disk);
		} else {
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_RESOLVE, ENXIO);
			return error;
		}

		/* The disk must be a partition no earlier slot uses. */
		if ((disk->d_flags & DISK_PARTITION) == 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_PARTITION, EINVAL);
			return error;
		}

		for (previous = 0; previous < slot; previous++) {
			if (context->slot[previous].configured &&
			    context->slot[previous].disk->d_dev == disk->d_dev)
				break;
		}

		if (previous != slot) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_DUPLICATE, EEXIST);
			return error;
		}

		/* The partition must carry a supported FAT variant. */
		error = drv_fat_probe_type(disk, &fat_type);
		if (error == 0 && !kern_boot_source_fat_type_supported(fat_type))
			error = EOPNOTSUPP;
		if (error != 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_FILESYSTEM, error);
			return error;
		}

		/* Mounts it privately; the mount holds its own disk reference. */
		error = mount_private("fat", disk, 0, NULL, &context->slot[slot].mount);
		if (error != 0) {
			disk_release(disk);
			error = context_fail(context, slot,
			    KERN_BOOT_SOURCE_FAILURE_MOUNT, error);
			return error;
		}

		context->slot[slot].disk = context->slot[slot].mount->m_disk;
		context->slot[slot].runtime_mount = context->slot[slot].mount;
		context->slot[slot].configured = 1U;
		disk_release(disk);
	}

	/* Reports the mounted sources. */
	return 0;
}

/*
 * Resolves a boot reference within the private mounts of a context.
 */
int
kern_boot_source_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	unsigned *slot_out,
	struct path *path_out)
{
	struct kern_boot_source_reference reference;
	int error;

	/* Rejects a missing context or result. */
	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);

	/* Parses the reference; its slot must be mounted. */
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	if (!context->slot[reference.slot].configured ||
	    context->slot[reference.slot].mount == NULL)
		return ENOENT;

	/* Looks the path up in the slot's mount. */
	error = mount_private_lookup(context->slot[reference.slot].mount,
				     reference.relative, path_out);
	if (error == 0 && slot_out != NULL)
		*slot_out = reference.slot;

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Marks a mounted slot as retained for the running system.
 */
int
kern_boot_source_retain_slot(
	struct kern_boot_source_context *context,
	unsigned slot)
{
	/* Rejects a missing context or a slot out of range. */
	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;

	/* Only a mounted slot can be retained. */
	if (!context->slot[slot].configured ||
	    context->slot[slot].mount == NULL)
		return ENOENT;
	context->slot[slot].retained = 1U;

	/* Reports the retained slot. */
	return 0;
}

/*
 * Marks every configured slot as retained.
 */
int
kern_boot_source_retain_configured(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;

	/* Rejects a missing or published context. */
	if (context == NULL || context->runtime_published)
		return EINVAL;

	/* Every configured slot must be completely mounted. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		source = &context->slot[slot];
		if (!source->configured)
			continue;
		if (source->mount == NULL ||
		    source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}

	/* Retains them all. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].configured)
			context->slot[slot].retained = 1U;
	}

	/* Reports the retained slots. */
	return 0;
}

/*
 * Publishes the retained slots as the system's boot source resolver.
 */
int
kern_boot_source_publish_runtime(
	struct kern_boot_source_context *context)
{
	const struct kern_boot_source_slot *source;
	unsigned slot;

	/* Rejects a missing or already published context. */
	if (context == NULL || context->runtime_published)
		return EINVAL;

	/* Every configured slot must be retained with a runtime mount. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		source = &context->slot[slot];
		if (!source->configured)
			continue;
		if (!source->retained ||
		    source->runtime_mount == NULL ||
		    source->disk == NULL)
			return EINVAL;
	}

	/* kern_vfs_init publishes before starting the first userspace process. */
	context->runtime_published = 1U;

	/* Reports the published context. */
	return 0;
}

/*
 * Resolves a boot reference through the published resolver.
 */
int
kern_boot_source_runtime_lookup(
	struct kern_boot_source_context *context,
	const char *text,
	struct path *path_out)
{
	struct kern_boot_source_reference reference;
	struct kern_boot_source_slot *source;
	int error;

	/* Rejects a missing context or result, or an unpublished context. */
	if (context == NULL || path_out == NULL)
		return EINVAL;
	path_init(path_out);
	if (!context->runtime_published)
		return ENXIO;

	/* Parses the reference; its slot must be retained. */
	error = kern_boot_source_reference_parse(text, &reference);
	if (error != 0)
		return error;
	source = &context->slot[reference.slot];
	if (!source->configured ||
	    !source->retained ||
	    source->runtime_mount == NULL)
		return ENOENT;

	/* Looks the path up in the slot's runtime mount. */

	/* Reports why the lookup failed. */
	error = runtime_mount_lookup(source, reference.relative, path_out);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Finds the slot that was mounted from a disk.
 */
int
kern_boot_source_find_disk(
	const struct kern_boot_source_context *context,
	const struct disk *disk,
	unsigned *slot_out)
{
	unsigned slot;

	/* Rejects a missing operand. */
	if (context == NULL || disk == NULL || slot_out == NULL)
		return EINVAL;

	/* Compares the device numbers of the configured slots. */
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++) {
		if (context->slot[slot].configured &&
		    context->slot[slot].disk != NULL &&
		    context->slot[slot].disk->d_dev == disk->d_dev) {
			*slot_out = slot;
			return 0;
		}
	}

	/* Reports a disk no slot uses. */
	return ENOENT;
}

/*
 * Turns a slot's private mount into the system root mount.
 */
int
kern_boot_source_promote_root(
	struct kern_boot_source_context *context,
	unsigned slot,
	struct mount **root_out)
{
	struct kern_boot_source_slot *source;
	int error;

	/* Rejects a missing context or a slot out of range. */
	if (context == NULL || slot >= KERN_BOOT_SOURCE_SLOT_COUNT)
		return EINVAL;

	/* Only a mounted slot can be promoted. */
	source = &context->slot[slot];
	if (!source->configured || source->mount == NULL)
		return ENOENT;

	/* Promotes the mount; the slot keeps it only as its runtime mount. */
	error = mount_private_promote_root(source->mount, root_out);
	if (error != 0)
		return error;
	source->mount = NULL;
	source->retained = 1U;
	source->promoted = 1U;

	/* Reports the promoted root. */
	return 0;
}

/*
 * Unmounts every slot that was not retained.
 *
 * The first unmount error is reported after every slot was tried.
 */
int
kern_boot_source_release_unused(
	struct kern_boot_source_context *context)
{
	struct kern_boot_source_slot *source;
	unsigned slot;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing context. */
	if (context == NULL)
		return EINVAL;

	/* Unmounts the unretained slots in reverse order. */
	for (slot = KERN_BOOT_SOURCE_SLOT_COUNT; slot != 0U; slot--) {
		source = &context->slot[slot - 1U];
		if (source->mount == NULL || source->retained)
			continue;
		error = unmount_private(source->mount);
		if (error != 0) {
			if (first_error == 0)
				first_error = error;
			continue;
		}

		kern_memset(source, 0, sizeof(*source));
	}

	/* Reports the first unmount failure. */
	return first_error;
}

/* Empties a parameter record. */
static void
parameters_reset(
	struct kern_boot_parameters *parameters)
{
	unsigned index;

	/* Marks every parameter absent and forgets the unknown names. */
	parameters->storage[0] = '\0';
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++)
		parameters->value_offset[index] =
		    KERN_BOOT_PARAMETER_OFFSET_ABSENT;
	parameters->unknown_count = 0;
	parameters->unknown_name_truncated = 0;
	parameters->unknown_name[0] = '\0';
}

/* Empties a parameter record and passes an error through. */
static int
parse_error(
	struct kern_boot_parameters *parameters,
	int error)
{
	parameters_reset(parameters);

	/* Reports the caller's error. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Compares an unterminated name with a parameter name. */
static int
name_matches(
	const char *name,
	size_t length,
	const struct parameter_name *candidate)
{
	size_t index;

	/* The lengths and every character must agree. */
	if (length != candidate->length)
		return 0;
	for (index = 0; index < length; index++) {
		if (name[index] != candidate->text[index])
			return 0;
	}

	/* Reports a matching name. */
	return 1;
}

/* Finds the key of a parameter name. */
static int
parameter_key(
	const char *name,
	size_t length,
	enum kern_boot_parameter_key *key)
{
	unsigned index;

	/* Searches the known names. */
	for (index = 0; index < KERN_BOOT_PARAMETER_COUNT; index++) {
		if (name_matches(name, length, &parameter_names[index])) {
			*key = (enum kern_boot_parameter_key)index;
			return 1;
		}
	}

	/* Reports an unknown name. */
	return 0;
}

/* Counts an unknown parameter and remembers the first name, truncated. */
static void
record_unknown(
	struct kern_boot_parameters *parameters,
	const char *name,
	size_t length)
{
	size_t copy_length;
	size_t index;

	/* Only the first unknown name is kept. */
	parameters->unknown_count++;
	if (parameters->unknown_count != 1U)
		return;

	/* Copies the name up to the limit, noting a truncation. */
	copy_length = length;
	if (copy_length > KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX) {
		copy_length = KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX;
		parameters->unknown_name_truncated = 1;
	}

	for (index = 0; index < copy_length; index++)
		parameters->unknown_name[index] = name[index];
	parameters->unknown_name[copy_length] = '\0';
}

/* Tells whether a character separates two parameters. */
static int
parameter_separator(
	char character)
{
	/* A space, a tab and either line end separate parameters. */
	if (character == ' ' || character == '\t')
		return 1;
	if (character == '\n' || character == '\r')
		return 1;

	/* Reports a character that belongs to a token. */
	return 0;
}

/* Returns the position of the first character from position on that is not a separator. */
static size_t
skip_separators(
	const char *text,
	size_t position,
	size_t length)
{
	int separator;

	/* Passes over separators until a token or the end. */
	while (position < length) {
		separator = parameter_separator(text[position]);
		if (!separator)
			break;
		position++;
	}

	/* Reports where the token, or the end, is. */
	return position;
}

/* Validates selector text: printable, no slash, bounded, no '=' in a name. */
static int
selector_text(
	const char *text,
	size_t maximum,
	int device_name)
{
	size_t length;
	unsigned char byte;

	/* Rejects missing or empty text. */
	length = 0;
	if (text == NULL || text[0] == '\0')
		return EINVAL;

	/* Checks every character and the length. */
	while (text[length] != '\0') {
		byte = (unsigned char)text[length];
		if (byte < 0x21U ||
		    byte > 0x7eU ||
		    byte == '/' ||
		    (device_name && byte == '='))
			return EINVAL;
		length++;
		if (length >= maximum)
			return ENAMETOOLONG;
	}

	/* Reports valid text. */
	return 0;
}

/* Records a mount failure in the context, destroys it, and passes the error. */
static int
context_fail(
	struct kern_boot_source_context *context,
	unsigned slot,
	enum kern_boot_source_failure_stage stage,
	int error)
{
	/* Remembers where the failure happened for the boot diagnostics. */
	context->failure_slot = slot;
	context->failure_stage = stage;
	context->cleanup_error = kern_boot_source_context_destroy(context);

	/* Reports the caller's error. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Looks a relative path up in a slot's runtime mount. */
static int
runtime_mount_lookup(
	struct kern_boot_source_slot *source,
	const char *relative,
	struct path *result)
{
	struct cwdinfo context;
	struct path root;
	int error;

	/* A private mount is looked up directly. */
	if (!source->promoted) {
		error = mount_private_lookup(source->runtime_mount, relative, result);
		return error;
	}

	/* A promoted root is looked up through a namespace rooted at it. */
	path_init(&root);
	path_set(&root, source->runtime_mount, source->runtime_mount->m_root);
	error = cwdinfo_init(&context, &root);
	path_release(&root);
	if (error != 0)
		return error;
	error = namei_path_at(&context, relative, result);
	cwdinfo_destroy(&context);

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Formats only supported, nonempty GPT signatures into stable selectors. */
static int
provenance_selector(const struct kern_boot_partition_identity *identity, char *output)
{
	static const uint8_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	static const char digits[] = "0123456789abcdef";
	unsigned i, position, nonzero;
	uint8_t byte;

	kern_memset(output, 0, KERN_BOOT_SOURCE_SELECTOR_SIZE);
	if (identity->index == 0 || identity->block_count == 0 ||
	    identity->first_lba > UINT64_MAX - identity->block_count)
		return EINVAL;
	if (identity->scheme == KERN_PARTITION_SCHEME_MBR)
		return 0;
	if (identity->scheme != KERN_PARTITION_SCHEME_GPT)
		return EINVAL;
	nonzero = 0;
	for (i = 0; i < 16; i++)
		nonzero |= identity->signature[i];
	if (nonzero == 0)
		return EINVAL;
	kern_memcpy(output, "PARTUUID=", 9);
	position = 9;
	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			output[position++] = '-';
		byte = identity->signature[order[i]];
		output[position++] = digits[byte >> 4];
		output[position++] = digits[byte & 15U];
	}
	return 0;
}
