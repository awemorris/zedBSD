/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The URI functions (ws074-p087, which Amazon's scripts need once their
 * async code runs): encodeURI, encodeURIComponent, decodeURI and
 * decodeURIComponent (UTF-8 percent-encoding, a URIError for a lone
 * surrogate or a malformed sequence), and Annex B's escape and unescape.
 */

#include "js/builtin.h"

#include <errno.h>
#include <string.h>

/* The characters encodeURIComponent leaves as they are besides the letters and digits. */
static const char uri_marks[] = "-_.!~*'()";

/* What encodeURI leaves too, and what decodeURI does not decode: the reserved characters and #. */
static const char uri_reserved[] = ";/?:@&=+$,#";

/* What escape leaves as it is besides the letters and digits. */
static const char uri_escape_marks[] = "@*_+-./";

/* The hexadecimal digits of the escapes. */
static const char uri_hex[] = "0123456789ABCDEF";

static int uri_encode_uri(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_encode_component(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_decode_uri(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_decode_component(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_escape(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_unescape(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int uri_encode(struct vm_realm *realm, vm_value value, const char *also, vm_value *result);
static int uri_decode(struct vm_realm *realm, vm_value value, const char *keep, vm_value *result);
static int uri_in(uint16_t unit, const char *set);
static int uri_alphanumeric(uint16_t unit);
static int uri_kept(uint16_t unit, const char *marks, const char *also);
static int uri_hex_value(uint16_t unit);
static int uri_byte_at(const struct vm_string *string, uint32_t index);
static int uri_append_escape(struct wb_units *units, unsigned byte);
static int uri_finish(struct vm_realm *realm, struct wb_units *units, vm_value *result);

/*
 * Installs the URI functions and escape and unescape on the global object.
 */
int
js_builtin_install_uri(
	struct vm_realm *realm)
{
	int error;

	/* The six functions. */
	error = js_builtin_method(realm, realm->global, "encodeURI", 1, uri_encode_uri);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "encodeURIComponent", 1, uri_encode_component);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "decodeURI", 1, uri_decode_uri);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "decodeURIComponent", 1, uri_decode_component);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "escape", 1, uri_escape);
	if (error == 0)
		error = js_builtin_method(realm, realm->global, "unescape", 1, uri_unescape);
	if (error != 0)
		return error;

	/* Succeeded: the functions are installed. */
	return 0;
}

/* encodeURI(uri): everything but the letters, digits, marks and reserved characters percent-encoded. */
static int
uri_encode_uri(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The encoding that keeps the reserved characters. */
	status = uri_encode(realm, js_argument(args, count, 0), uri_reserved, result);
	if (status != 0)
		return status;

	/* Succeeded: the encoded string. */
	return 0;
}

/* encodeURIComponent(component): everything but the letters, digits and marks percent-encoded. */
static int
uri_encode_component(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The encoding that keeps nothing more. */
	status = uri_encode(realm, js_argument(args, count, 0), "", result);
	if (status != 0)
		return status;

	/* Succeeded: the encoded string. */
	return 0;
}

/* decodeURI(uri): the escapes decoded, but those of the reserved characters. */
static int
uri_decode_uri(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The decoding that keeps the reserved characters' escapes. */
	status = uri_decode(realm, js_argument(args, count, 0), uri_reserved, result);
	if (status != 0)
		return status;

	/* Succeeded: the decoded string. */
	return 0;
}

/* decodeURIComponent(component): every escape decoded. */
static int
uri_decode_component(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(this_value);

	/* The decoding that keeps no escape. */
	status = uri_decode(realm, js_argument(args, count, 0), "", result);
	if (status != 0)
		return status;

	/* Succeeded: the decoded string. */
	return 0;
}

/* escape(string): each code unit but the letters, digits and @*_+-./ as %XX, or %uXXXX above 255. */
static int
uri_escape(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct wb_units units;
	uint16_t unit;
	uint16_t text[6];
	uint32_t index;
	int kept;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The string. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;

	/* Each code unit. */
	wb_units_init(&units);
	for (index = 0; index < string->length && status == 0; index++) {
		unit = vm_string_at(string, index);
		kept = uri_kept(unit, uri_escape_marks, "");
		if (kept) {
			status = wb_units_append(&units, &unit, 1);
		} else if (unit < 256U) {
			status = uri_append_escape(&units, unit);
		} else {
			/* %uXXXX. */
			text[0] = '%';
			text[1] = 'u';
			text[2] = (uint16_t)uri_hex[(unit >> 12) & 0xFU];
			text[3] = (uint16_t)uri_hex[(unit >> 8) & 0xFU];
			text[4] = (uint16_t)uri_hex[(unit >> 4) & 0xFU];
			text[5] = (uint16_t)uri_hex[unit & 0xFU];
			status = wb_units_append(&units, text, 6);
		}
	}

	/* What failed goes back to the caller with the units let go. */
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Succeeded: the escaped string. */
	status = uri_finish(realm, &units, result);
	return status;
}

/* unescape(string): each %XX and %uXXXX as its code unit, anything else as it is. */
static int
uri_unescape(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct vm_string *string;
	struct wb_units units;
	uint16_t unit;
	uint16_t marker;
	uint32_t index;
	int digits[4];
	int status;

	UNUSED_PARAMETER(this_value);

	/* The string. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_string(realm, js_argument(args, count, 0), &string);
	if (status != 0)
		return status;

	/* Each code unit, an escape taking the units after it. */
	wb_units_init(&units);
	for (index = 0; index < string->length && status == 0; index++) {
		unit = vm_string_at(string, index);
		marker = 0;
		if (index + 1U < string->length)
			marker = vm_string_at(string, index + 1U);
		if (unit == '%' && index + 5U < string->length && marker == 'u') {
			digits[0] = uri_hex_value(vm_string_at(string, index + 2U));
			digits[1] = uri_hex_value(vm_string_at(string, index + 3U));
			digits[2] = uri_hex_value(vm_string_at(string, index + 4U));
			digits[3] = uri_hex_value(vm_string_at(string, index + 5U));
			if (digits[0] >= 0 && digits[1] >= 0 && digits[2] >= 0 && digits[3] >= 0) {
				unit = (uint16_t)((digits[0] << 12) | (digits[1] << 8) | (digits[2] << 4) | digits[3]);
				index += 5U;
			}
		} else if (unit == '%' && index + 2U < string->length) {
			digits[0] = uri_hex_value(vm_string_at(string, index + 1U));
			digits[1] = uri_hex_value(vm_string_at(string, index + 2U));
			if (digits[0] >= 0 && digits[1] >= 0) {
				unit = (uint16_t)((digits[0] << 4) | digits[1]);
				index += 2U;
			}
		}

		/* The code unit, escaped or not. */
		status = wb_units_append(&units, &unit, 1);
	}

	/* What failed goes back to the caller with the units let go. */
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Succeeded: the unescaped string. */
	status = uri_finish(realm, &units, result);
	return status;
}

/*
 * Percent-encodes a value's string as UTF-8, leaving the letters, digits,
 * marks and the characters of also; a lone surrogate is a URIError.
 */
static int
uri_encode(
	struct vm_realm *realm,
	vm_value value,
	const char *also,
	vm_value *result)
{
	struct vm_string *string;
	struct wb_units units;
	unsigned char bytes[4];
	uint32_t code_point;
	uint32_t index;
	uint16_t unit;
	uint16_t low;
	unsigned length;
	unsigned byte;
	int kept;
	int status;

	/* The string. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_string(realm, value, &string);
	if (status != 0)
		return status;

	/* Each code point. */
	wb_units_init(&units);
	for (index = 0; index < string->length; index++) {
		unit = vm_string_at(string, index);

		/* A character kept as it is. */
		kept = uri_kept(unit, uri_marks, also);
		if (kept) {
			status = wb_units_append(&units, &unit, 1);
			if (status != 0)
				break;
			continue;
		}

		/* The code point: a surrogate must be a high one followed by a low one. */
		code_point = unit;
		if (unit >= 0xDC00U && unit <= 0xDFFFU) {
			status = VM_THROWN;
			break;
		}

		/* A high surrogate pairs with the low one after it. */
		if (unit >= 0xD800U && unit <= 0xDBFFU) {
			low = 0;
			if (index + 1U < string->length)
				low = vm_string_at(string, index + 1U);
			if (low < 0xDC00U || low > 0xDFFFU) {
				status = VM_THROWN;
				break;
			}

			/* The pair's code point. */
			code_point = 0x10000U + (((uint32_t)unit - 0xD800U) << 10) + ((uint32_t)low - 0xDC00U);
			index++;
		}

		/* Its UTF-8 bytes. */
		if (code_point < 0x80U) {
			bytes[0] = (unsigned char)code_point;
			length = 1;
		} else if (code_point < 0x800U) {
			bytes[0] = (unsigned char)(0xC0U | (code_point >> 6));
			bytes[1] = (unsigned char)(0x80U | (code_point & 0x3FU));
			length = 2;
		} else if (code_point < 0x10000U) {
			bytes[0] = (unsigned char)(0xE0U | (code_point >> 12));
			bytes[1] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3FU));
			bytes[2] = (unsigned char)(0x80U | (code_point & 0x3FU));
			length = 3;
		} else {
			bytes[0] = (unsigned char)(0xF0U | (code_point >> 18));
			bytes[1] = (unsigned char)(0x80U | ((code_point >> 12) & 0x3FU));
			bytes[2] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3FU));
			bytes[3] = (unsigned char)(0x80U | (code_point & 0x3FU));
			length = 4;
		}

		/* Each byte as %XX. */
		for (byte = 0; byte < length && status == 0; byte++)
			status = uri_append_escape(&units, bytes[byte]);
		if (status != 0)
			break;
	}

	/* A lone surrogate is a URIError. */
	if (status == VM_THROWN) {
		wb_units_release(&units);
		status = vm_throw_error(realm, VM_ERROR_URI, "URI malformed");
		return status;
	}

	/* What failed goes back to the caller with the units let go. */
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Succeeded: the encoded string. */
	status = uri_finish(realm, &units, result);
	return status;
}

/*
 * Decodes the percent-escapes of a value's string as UTF-8, keeping the
 * escape of a single byte whose character is in keep; a malformed escape
 * or sequence is a URIError.
 */
static int
uri_decode(
	struct vm_realm *realm,
	vm_value value,
	const char *keep,
	vm_value *result)
{
	struct vm_string *string;
	struct wb_units units;
	uint32_t code_point;
	uint32_t minimum;
	uint32_t index;
	uint32_t start;
	uint16_t unit;
	uint16_t marker;
	uint16_t original[3];
	unsigned length;
	unsigned more;
	int byte;
	int kept;
	int status;

	/* The string. */
	*result = VM_VALUE_UNDEFINED;
	status = vm_to_string(realm, value, &string);
	if (status != 0)
		return status;

	/* Each code unit; an escape starts a sequence. */
	wb_units_init(&units);
	for (index = 0; index < string->length && status == 0; index++) {
		unit = vm_string_at(string, index);
		if (unit != '%') {
			status = wb_units_append(&units, &unit, 1);
			continue;
		}

		/* The first byte. */
		start = index;
		byte = uri_byte_at(string, index);
		if (byte < 0) {
			status = VM_THROWN;
			break;
		}

		/* Past the escape's digits. */
		index += 2U;

		/* A single byte: its character, unless it is one to keep escaped. */
		if (byte < 0x80) {
			unit = (uint16_t)byte;
			kept = uri_in(unit, keep);
			if (kept) {
				/* The escape as it was written. */
				original[0] = vm_string_at(string, start);
				original[1] = vm_string_at(string, start + 1U);
				original[2] = vm_string_at(string, start + 2U);
				status = wb_units_append(&units, original, 3);
			} else {
				status = wb_units_append(&units, &unit, 1);
			}

			/* The next code unit. */
			continue;
		}

		/* A sequence: its length from the first byte's leading ones. */
		if ((byte & 0xE0) == 0xC0) {
			length = 2;
			code_point = (uint32_t)byte & 0x1FU;
			minimum = 0x80U;
		} else if ((byte & 0xF0) == 0xE0) {
			length = 3;
			code_point = (uint32_t)byte & 0x0FU;
			minimum = 0x800U;
		} else if ((byte & 0xF8) == 0xF0) {
			length = 4;
			code_point = (uint32_t)byte & 0x07U;
			minimum = 0x10000U;
		} else {
			status = VM_THROWN;
			break;
		}

		/* The continuation bytes, each an escape of 10xxxxxx. */
		for (more = 1; more < length; more++) {
			index++;
			byte = -1;
			marker = 0;
			if (index < string->length)
				marker = vm_string_at(string, index);
			if (marker == '%')
				byte = uri_byte_at(string, index);
			if (byte < 0 || (byte & 0xC0) != 0x80)
				break;
			code_point = (code_point << 6) | ((uint32_t)byte & 0x3FU);
			index += 2U;
		}

		/* A short sequence, an overlong form, a surrogate or a code point too large is malformed. */
		if (more < length || code_point < minimum || (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
			status = VM_THROWN;
			break;
		}

		/* The code point, as one or two code units. */
		status = wb_units_append_code_point(&units, code_point);
	}

	/* A malformed escape is a URIError. */
	if (status == VM_THROWN) {
		wb_units_release(&units);
		status = vm_throw_error(realm, VM_ERROR_URI, "URI malformed");
		return status;
	}

	/* What failed goes back to the caller with the units let go. */
	if (status != 0) {
		wb_units_release(&units);
		return status;
	}

	/* Succeeded: the decoded string. */
	status = uri_finish(realm, &units, result);
	return status;
}

/* Tells whether a code unit is one of an ASCII set's characters. */
static int
uri_in(
	uint16_t unit,
	const char *set)
{
	const char *found;

	/* Only ASCII can be in the set. */
	if (unit == 0 || unit >= 0x80U)
		return 0;

	/* The set's characters. */
	found = strchr(set, (int)unit);
	if (found != NULL)
		return 1;

	/* Not one of them. */
	return 0;
}

/* Tells whether an encoding keeps a code unit as it is: a letter, a digit, or one of two sets. */
static int
uri_kept(
	uint16_t unit,
	const char *marks,
	const char *also)
{
	int kept;

	/* A letter or a digit. */
	kept = uri_alphanumeric(unit);
	if (kept)
		return 1;

	/* One of the marks. */
	kept = uri_in(unit, marks);
	if (kept)
		return 1;

	/* One of the others. */
	kept = uri_in(unit, also);
	if (kept)
		return 1;

	/* Encoded. */
	return 0;
}

/* Tells whether a code unit is an ASCII letter or digit. */
static int
uri_alphanumeric(
	uint16_t unit)
{
	/* The letters and the digits. */
	if (unit >= 'a' && unit <= 'z')
		return 1;
	if (unit >= 'A' && unit <= 'Z')
		return 1;
	if (unit >= '0' && unit <= '9')
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports a hexadecimal digit's value, -1 for anything else. */
static int
uri_hex_value(
	uint16_t unit)
{
	/* The three ranges. */
	if (unit >= '0' && unit <= '9')
		return (int)(unit - '0');
	if (unit >= 'a' && unit <= 'f')
		return (int)(unit - 'a') + 10;
	if (unit >= 'A' && unit <= 'F')
		return (int)(unit - 'A') + 10;

	/* Not a digit. */
	return -1;
}

/* Reports the byte of the escape %XX at an index, -1 when it is not one. */
static int
uri_byte_at(
	const struct vm_string *string,
	uint32_t index)
{
	int high;
	int low;

	/* The two digits after the %. */
	if (index + 2U >= string->length)
		return -1;
	high = uri_hex_value(vm_string_at(string, index + 1U));
	low = uri_hex_value(vm_string_at(string, index + 2U));
	if (high < 0 || low < 0)
		return -1;

	/* The byte. */
	return (high << 4) | low;
}

/* Appends a byte's escape %XX. */
static int
uri_append_escape(
	struct wb_units *units,
	unsigned byte)
{
	uint16_t text[3];
	int status;

	/* The three units. */
	text[0] = '%';
	text[1] = (uint16_t)uri_hex[(byte >> 4) & 0xFU];
	text[2] = (uint16_t)uri_hex[byte & 0xFU];
	status = wb_units_append(units, text, 3);
	if (status != 0)
		return status;

	/* Succeeded: the escape is appended. */
	return 0;
}

/* Makes the string of the units gathered and releases them. */
static int
uri_finish(
	struct vm_realm *realm,
	struct wb_units *units,
	vm_value *result)
{
	struct vm_string *string;

	/* The string. */
	string = vm_string_from_units(realm->heap, units->data, units->length);
	wb_units_release(units);
	if (string == NULL)
		return ENOMEM;

	/* Succeeded: its value. */
	*result = vm_value_cell(string);
	return 0;
}
