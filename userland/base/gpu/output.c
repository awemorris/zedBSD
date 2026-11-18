/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The output of gpu: the escaping of text that comes from other programs
 * and devices, the records of the porcelain form, and the names of the
 * capability bits, the roles and the errors.
 *
 * Text gpu did not write itself (a driver's name, a display's name, an
 * EDID's text, a word given on the line) is escaped before it reaches the
 * terminal: control characters, C1 controls and bytes that are not UTF-8
 * become \xHH, and every correct UTF-8 character is left as it is.  The
 * porcelain form also escapes the space, = and \, so that a value never
 * splits its record.
 */

#include "gpu-command.h"

#include <uapi/gpu-allocation.h>
#include <uapi/gpu-fence.h>
#include <uapi/gpu-job.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The longest text a field escapes; a longer one is cut there. */
#define GPU_TEXT_MAX			256U

/*
 * One capability bit and its name in the output.
 *
 * The names are those of the GPU_CAP_ macros, in lower case with dashes.
 */
struct gpu_capability {
	uint32_t bit;
	const char *name;
};

/*
 * One errno value and its name in the porcelain form.
 */
struct gpu_error {
	int error;
	const char *name;
};

/*
 * The capability bits a node can report.
 *
 * Values referenced: zedBSD include/uapi/gpu.h, gpu-display.h,
 * gpu-allocation.h, gpu-fence.h and gpu-job.h (GPU_CAP_*).
 */
static const struct gpu_capability gpu_capabilities[] = {
	{ GPU_CAP_RESOURCE, "resource" },
	{ GPU_CAP_CAPSET, "capset" },
	{ GPU_CAP_BLOB, "blob" },
	{ GPU_CAP_TRANSFER, "transfer" },
	{ GPU_CAP_COMMAND, "command" },
	{ GPU_CAP_PRESENT, "present" },
	{ GPU_CAP_MAPPING, "mapping" },
	{ GPU_CAP_DISPLAY, "display" },
	{ GPU_CAP_SHARE, "share" },
	{ GPU_CAP_NOTIFICATION, "notification" },
	{ GPU_CAP_ALLOCATION_SHARE, "allocation-share" },
	{ GPU_CAP_FENCE, "fence" },
	{ GPU_CAP_DISPLAY_EVENTS, "display-events" },
	{ GPU_CAP_JOB, "job" },
	{ GPU_CAP_JOB_CAPACITY, "job-capacity" },
	{ GPU_CAP_DISPLAY_CONTROL, "display-control" },
	{ GPU_CAP_HOST_BLOB, "host-blob" },
	{ GPU_CAP_IMAGE_LAYOUTS, "image-layouts" },
	{ GPU_CAP_JOB_SUBMIT, "job-submit" },
	{ GPU_CAP_JOB_RETIRE, "job-retire" }
};

/* The errors gpu names in the porcelain form; any other is its number. */
static const struct gpu_error gpu_errors[] = {
	{ EPERM, "EPERM" },
	{ ENOENT, "ENOENT" },
	{ EINTR, "EINTR" },
	{ EIO, "EIO" },
	{ ENXIO, "ENXIO" },
	{ ENOMEM, "ENOMEM" },
	{ EACCES, "EACCES" },
	{ EFAULT, "EFAULT" },
	{ EBUSY, "EBUSY" },
	{ ENODEV, "ENODEV" },
	{ EINVAL, "EINVAL" },
	{ ENOSPC, "ENOSPC" },
	{ EAGAIN, "EAGAIN" },
	{ ENOSYS, "ENOSYS" },
	{ ENOTSUP, "ENOTSUP" },
	{ EOPNOTSUPP, "EOPNOTSUPP" },
	{ ETIMEDOUT, "ETIMEDOUT" },
	{ ESTALE, "ESTALE" },
	{ EMSGSIZE, "EMSGSIZE" }
};

/*
 * The escaped text of the field being printed.
 *
 * gpu prints from one thread and one field at a time; the buffer holds the
 * last field's escaped text only until it is printed.
 */
static char gpu_escaped[GPU_TEXT_MAX * GPU_ESCAPE_EXPANSION + 1U];

static size_t gpu_utf8_length(const unsigned char *bytes, size_t remaining);
static int gpu_escaped_byte(unsigned char byte, unsigned porcelain);
static size_t gpu_append(char *out, size_t size, size_t used, const char *separator, const char *text);

/*
 * Escapes length bytes of text into out.
 *
 * Control characters (below 0x20 and 0x7f), the C1 controls U+0080 to
 * U+009F and every byte that is not part of a correct UTF-8 character become
 * \xHH; with porcelain, so do the space, = and \.  out is always
 * terminated; an escape that does not fit is left out whole.  Returns the
 * length written.
 */
size_t
gpu_escape(
	const char *text,
	size_t length,
	unsigned porcelain,
	char *out,
	size_t size)
{
	const unsigned char *bytes;
	size_t sequence;
	size_t written;
	size_t at;
	size_t step;
	int escaped;

	/* Nothing fits in no room. */
	if (size == 0U)
		return 0U;

	/* Walks the text one character, or one bad byte, at a time. */
	bytes = (const unsigned char *)text;
	written = 0U;
	at = 0U;
	while (at < length) {
		/* Works out how long the character is, 0 for a byte that starts none. */
		sequence = gpu_utf8_length(bytes + at, length - at);

		/* A single byte is copied unless it must be escaped. */
		if (sequence == 1U) {
			escaped = gpu_escaped_byte(bytes[at], porcelain);
			if (!escaped) {
				if (written + 1U >= size)
					break;
				out[written] = (char)bytes[at];
				written++;
				at++;
				continue;
			}
		}

		/* A correct character of several bytes is copied whole. */
		if (sequence > 1U) {
			if (written + sequence >= size)
				break;
			memcpy(out + written, bytes + at, sequence);
			written += sequence;
			at += sequence;
			continue;
		}

		/*
		 * A byte that is escaped: a control, a C1 control (both of its
		 * bytes, one at a time), or a byte of no correct character.
		 */
		if (written + GPU_ESCAPE_EXPANSION >= size)
			break;
		step = (size_t)snprintf(out + written, size - written, "\\x%02X", bytes[at]);
		written += step;
		at++;
	}

	/* Ends the text. */
	out[written] = '\0';

	/* Succeeded: the length of the escaped text. */
	return written;
}

/*
 * Measures a text that may fill its array without a terminating NUL.
 */
size_t
gpu_text_length(
	const char *text,
	size_t max)
{
	size_t length;

	/* Counts up to the NUL or the end of the array. */
	length = 0U;
	while (length < max && text[length] != '\0')
		length++;

	/* The text's length. */
	return length;
}

/*
 * Prints the first line of the porcelain form, which names its version.
 */
void
gpu_porcelain_begin(void)
{
	/* The version of the records' keys. */
	printf("porcelain version=1\n");
}

/*
 * Starts one porcelain record of a kind.
 */
void
gpu_record_begin(
	const char *kind)
{
	/* The kind leads the record. */
	fputs(kind, stdout);
}

/*
 * Ends the porcelain record being printed.
 */
void
gpu_record_end(void)
{
	/* One record a line. */
	fputc('\n', stdout);
}

/*
 * Prints one text field of a porcelain record, escaped, from at most max
 * bytes of text.
 */
void
gpu_field_text(
	const char *key,
	const char *text,
	size_t max)
{
	size_t length;

	/* Escapes the text for the porcelain form. */
	length = gpu_text_length(text, max);
	if (length > GPU_TEXT_MAX)
		length = GPU_TEXT_MAX;
	(void)gpu_escape(text, length, 1U, gpu_escaped, sizeof(gpu_escaped));

	/* The field. */
	printf(" %s=%s", key, gpu_escaped);
}

/*
 * Prints one decimal field of a porcelain record.
 */
void
gpu_field_unsigned(
	const char *key,
	unsigned long long value)
{
	/* The field. */
	printf(" %s=%llu", key, value);
}

/*
 * Prints one hexadecimal field of a porcelain record, a set of bits.
 */
void
gpu_field_hex(
	const char *key,
	unsigned long long value)
{
	/* The field. */
	printf(" %s=0x%llx", key, value);
}

/*
 * Prints a text for people, escaped, from at most max bytes of text.
 */
void
gpu_print_text(
	const char *text,
	size_t max)
{
	size_t length;

	/* Escapes the text for the terminal. */
	length = gpu_text_length(text, max);
	if (length > GPU_TEXT_MAX)
		length = GPU_TEXT_MAX;
	(void)gpu_escape(text, length, 0U, gpu_escaped, sizeof(gpu_escaped));

	/* The text. */
	fputs(gpu_escaped, stdout);
}

/*
 * Prints a text for people, escaped, and pads it with spaces to width
 * columns, counting each byte of the escaped text as a column.
 */
void
gpu_print_column(
	const char *text,
	size_t max,
	size_t width)
{
	size_t length;
	size_t written;

	/* Escapes the text for the terminal. */
	length = gpu_text_length(text, max);
	if (length > GPU_TEXT_MAX)
		length = GPU_TEXT_MAX;
	written = gpu_escape(text, length, 0U, gpu_escaped, sizeof(gpu_escaped));

	/* The text, then at least one space and as many as reach the width. */
	fputs(gpu_escaped, stdout);
	do {
		fputc(' ', stdout);
		written++;
	} while (written < width);
}

/*
 * Names one capability bit; NULL for a bit gpu does not know.
 */
const char *
gpu_capability_name(
	uint32_t bit)
{
	size_t index;

	/* Looks the bit up. */
	for (index = 0U; index < sizeof(gpu_capabilities) / sizeof(gpu_capabilities[0]); index++) {
		if (gpu_capabilities[index].bit == bit)
			return gpu_capabilities[index].name;
	}

	/* A bit without a name. */
	return NULL;
}

/*
 * Writes the names of a set of capability bits into out, separated, in the
 * order of the bits; a bit without a name is written as 0x and its value.
 * Returns the length written; "none" for no bit.
 */
size_t
gpu_capability_names(
	uint32_t capabilities,
	const char *separator,
	char *out,
	size_t size)
{
	char number[16];
	const char *name;
	uint32_t bit;
	unsigned shift;
	size_t used;

	/* Starts empty. */
	used = 0U;
	if (size != 0U)
		out[0] = '\0';

	/* No bit is written as none. */
	if (capabilities == 0U) {
		used = gpu_append(out, size, used, separator, "none");
		return used;
	}

	/* Writes each bit that is set, from the lowest. */
	for (shift = 0U; shift < 32U; shift++) {
		bit = 1U << shift;
		if ((capabilities & bit) == 0U)
			continue;

		/* Its name, or its value, after the separator. */
		name = gpu_capability_name(bit);
		if (name == NULL) {
			(void)snprintf(number, sizeof(number), "0x%x", bit);
			name = number;
		}
		used = gpu_append(out, size, used, separator, name);
	}

	/* Succeeded: the names are written. */
	return used;
}

/*
 * Writes the names of a node's roles (GPU_DEVICE_QUERY) into out,
 * separated; "none" for no role.  Returns the length written.
 */
size_t
gpu_role_names(
	uint32_t roles,
	const char *separator,
	char *out,
	size_t size)
{
	char number[16];
	uint32_t other;
	size_t used;

	/* Starts empty. */
	used = 0U;
	if (size != 0U)
		out[0] = '\0';

	/* A node that renders. */
	if ((roles & GPU_DEVICE_RENDER) != 0U)
		used = gpu_append(out, size, used, separator, "render");

	/* A node that has displays. */
	if ((roles & GPU_DEVICE_DISPLAY) != 0U)
		used = gpu_append(out, size, used, separator, "display");

	/* A role gpu does not know, by its value. */
	other = roles & ~(GPU_DEVICE_RENDER | GPU_DEVICE_DISPLAY);
	if (other != 0U) {
		(void)snprintf(number, sizeof(number), "0x%x", other);
		used = gpu_append(out, size, used, separator, number);
	}

	/* No role is written as none. */
	if (used == 0U)
		used = gpu_append(out, size, used, separator, "none");

	/* Succeeded: the roles are written. */
	return used;
}

/*
 * Names an errno value for the porcelain form; NULL for one gpu does not
 * name.
 */
const char *
gpu_error_name(
	int error)
{
	size_t index;

	/* Looks the value up; where two names share one value, the first wins. */
	for (index = 0U; index < sizeof(gpu_errors) / sizeof(gpu_errors[0]); index++) {
		if (gpu_errors[index].error == error)
			return gpu_errors[index].name;
	}

	/* A value without a name. */
	return NULL;
}

/*
 * Reports a request that was refused or failed.
 *
 * The message goes to the standard error after "gpu: ".  The porcelain form
 * also prints "result verb=VERB error=NAME" so that a script reads the
 * outcome.  The caller then ends with GPU_EXIT_FAILED.
 */
void
gpu_fail(
	const struct gpu_invocation *command,
	const char *verb,
	int error,
	const char *format,
	...)
{
	va_list arguments;
	const char *name;

	/* The message for people. */
	fflush(stdout);
	fputs("gpu: ", stderr);
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);

	/* The record for a script, the error by name or by number. */
	if (command->porcelain) {
		gpu_record_begin("result");
		gpu_field_text("verb", verb, GPU_TEXT_MAX);
		name = gpu_error_name(error);
		if (name != NULL) {
			gpu_field_text("error", name, GPU_TEXT_MAX);
		} else {
			gpu_field_unsigned("error", (unsigned long long)error);
		}
		gpu_record_end();
	}
}

/*
 * Writes a refresh in millihertz as hertz, with only the decimals that are
 * not 0: 60000 is "60", 59940 is "59.94".
 */
void
gpu_refresh_text(
	uint32_t refresh_mhz,
	char *text,
	size_t size)
{
	uint32_t fraction;

	/* Whole hertz need no decimals. */
	fraction = refresh_mhz % 1000U;
	if (fraction == 0U) {
		(void)snprintf(text, size, "%u", refresh_mhz / 1000U);
		return;
	}

	/* Hundredths when the thousandths are 0. */
	if (fraction % 100U == 0U) {
		(void)snprintf(text, size, "%u.%u", refresh_mhz / 1000U, fraction / 100U);
		return;
	}
	if (fraction % 10U == 0U) {
		(void)snprintf(text, size, "%u.%02u", refresh_mhz / 1000U, fraction / 10U);
		return;
	}

	/* All three decimals. */
	(void)snprintf(text, size, "%u.%03u", refresh_mhz / 1000U, fraction);
}

/*
 * Measures the UTF-8 character at bytes: its length when it is a correct
 * character other than a C1 control, 1 for a byte below 0x80, and 0 for a
 * byte that starts no correct character or starts a C1 control.
 *
 * Values referenced: The Unicode Standard, Version 15.0, Table 3-7
 * (well-formed UTF-8 byte sequences).
 */
static size_t
gpu_utf8_length(
	const unsigned char *bytes,
	size_t remaining)
{
	unsigned char lead;
	unsigned char low;
	unsigned char high;
	size_t length;
	size_t index;

	/* A byte of ASCII is a character of its own. */
	lead = bytes[0];
	if (lead < 0x80U)
		return 1U;

	/* Works out the length and the range of the second byte from the lead. */
	low = 0x80U;
	high = 0xbfU;
	if (lead >= 0xc2U && lead <= 0xdfU) {
		length = 2U;
	} else if (lead == 0xe0U) {
		length = 3U;
		low = 0xa0U;
	} else if (lead >= 0xe1U && lead <= 0xecU) {
		length = 3U;
	} else if (lead == 0xedU) {
		/* Not the surrogates. */
		length = 3U;
		high = 0x9fU;
	} else if (lead >= 0xeeU && lead <= 0xefU) {
		length = 3U;
	} else if (lead == 0xf0U) {
		length = 4U;
		low = 0x90U;
	} else if (lead >= 0xf1U && lead <= 0xf3U) {
		length = 4U;
	} else if (lead == 0xf4U) {
		/* Not past U+10FFFF. */
		length = 4U;
		high = 0x8fU;
	} else {
		return 0U;
	}

	/* A character cut off by the end of the text is no character. */
	if (remaining < length)
		return 0U;

	/* The second byte's own range. */
	if (bytes[1] < low || bytes[1] > high)
		return 0U;

	/* The C1 controls, U+0080 to U+009F, are escaped like the others. */
	if (lead == 0xc2U && bytes[1] <= 0x9fU)
		return 0U;

	/* Every later byte continues the character. */
	for (index = 2U; index < length; index++) {
		if (bytes[index] < 0x80U || bytes[index] > 0xbfU)
			return 0U;
	}

	/* Succeeded: a correct character of that length. */
	return length;
}

/* Says whether a byte below 0x80 must be escaped; nonzero when it must. */
static int
gpu_escaped_byte(
	unsigned char byte,
	unsigned porcelain)
{
	/* The control characters, whatever the form. */
	if (byte < 0x20U || byte == 0x7fU)
		return 1;

	/* What would split a porcelain record or look like an escape in it. */
	if (porcelain) {
		if (byte == ' ')
			return 1;
		if (byte == '=')
			return 1;
		if (byte == '\\')
			return 1;
	}

	/* A byte that is printed as it is. */
	return 0;
}

/*
 * Appends a text to out, after the separator unless out is empty, when
 * both fit whole; the length then used.
 */
static size_t
gpu_append(
	char *out,
	size_t size,
	size_t used,
	const char *separator,
	const char *text)
{
	size_t separator_length;
	size_t length;

	/* The separator goes only between two texts. */
	separator_length = 0U;
	if (used != 0U)
		separator_length = strlen(separator);

	/* A text that does not fit whole with its separator is left out. */
	length = strlen(text);
	if (used + separator_length + length >= size)
		return used;

	/* Appends the separator, then the text with its end. */
	memcpy(out + used, separator, separator_length);
	memcpy(out + used + separator_length, text, length + 1U);

	/* The length now used. */
	return used + separator_length + length;
}
