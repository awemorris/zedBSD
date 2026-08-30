/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/wifi-conf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIFI_CONF_HEADER "wifi-conf 1\n"
#define WIFI_CONF_SECURITY "wpa2-personal-ccmp"

static int
wifi_conf_fail(char *error, size_t capacity, int number, const char *format,
	       ...)
{
	static const char truncated[] = "wifi.conf: error [truncated]";
	char message[WIFI_CONF_DIAGNOSTIC_MAX];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(message)) {
		memcpy(message, truncated, sizeof(truncated));
		length = (int)(sizeof(truncated) - 1U);
	}
	if (error != NULL && capacity != 0) {
		if ((size_t)length + 1U <= capacity)
			memcpy(error, message, (size_t)length + 1U);
		else if (sizeof(truncated) <= capacity)
			memcpy(error, truncated, sizeof(truncated));
		else
			error[0] = '\0';
	}
	wifi_conf_explicit_clear(message, sizeof(message));
	errno = number;
	return -1;
}

void
wifi_conf_explicit_clear(void *storage, size_t length)
{
	volatile unsigned char *bytes = storage;

	if (storage == NULL)
		return;
	while (length != 0) {
		*bytes++ = 0;
		length--;
	}
}

void
wifi_conf_model_init(struct wifi_conf_model *model)
{
	if (model != NULL)
		memset(model, 0, sizeof(*model));
}

void
wifi_conf_model_clear(struct wifi_conf_model *model)
{
	if (model != NULL)
		wifi_conf_explicit_clear(model, sizeof(*model));
}

static int
hex_value(unsigned char byte)
{
	if (byte >= '0' && byte <= '9')
		return (int)(byte - '0');
	if (byte >= 'a' && byte <= 'f')
		return (int)(byte - 'a') + 10;
	if (byte >= 'A' && byte <= 'F')
		return (int)(byte - 'A') + 10;
	return -1;
}

static int
decode_quoted(const unsigned char **cursor_pointer, const unsigned char *end,
	      unsigned char *decoded, size_t decoded_capacity,
	      size_t *decoded_length, size_t record, const char *field,
	      char *error, size_t error_capacity)
{
	const unsigned char *cursor = *cursor_pointer;
	size_t used = 0;

	if (cursor == end || *cursor++ != '"')
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has malformed %s field",
		    (unsigned long)record, field);
	while (cursor < end) {
		unsigned char byte = *cursor++;
		int high, low;

		if (byte == '"') {
			*cursor_pointer = cursor;
			*decoded_length = used;
			return 0;
		}
		if (byte == '\\') {
			if (cursor == end)
				goto malformed;
			byte = *cursor++;
			switch (byte) {
			case '"':
			case '\\':
				break;
			case 't':
				byte = '\t';
				break;
			case 'n':
				byte = '\n';
				break;
			case 'r':
				byte = '\r';
				break;
			case 'x':
				if ((size_t)(end - cursor) < 2U)
					goto malformed;
				high = hex_value(cursor[0]);
				low = hex_value(cursor[1]);
				if (high < 0 || low < 0)
					goto malformed;
				byte = (unsigned char)((unsigned)high << 4 |
						       (unsigned)low);
				cursor += 2;
				break;
			default:
				goto malformed;
			}
		} else if (byte < 0x20U || byte > 0x7eU) {
			goto malformed;
		}
		if (used == decoded_capacity)
			return wifi_conf_fail(error, error_capacity, E2BIG,
			    "wifi.conf: record %lu %s exceeds decoded limit",
			    (unsigned long)record, field);
		decoded[used++] = byte;
	}

malformed:
	return wifi_conf_fail(error, error_capacity, EINVAL,
	    "wifi.conf: record %lu has malformed %s escape or quoting",
	    (unsigned long)record, field);
}

static int
consume_literal(const unsigned char **cursor_pointer,
		const unsigned char *end, const char *literal)
{
	const unsigned char *cursor = *cursor_pointer;
	size_t length = strlen(literal);

	if ((size_t)(end - cursor) < length ||
	    memcmp(cursor, literal, length) != 0)
		return -1;
	*cursor_pointer = cursor + length;
	return 0;
}

static int
ssid_duplicate(const struct wifi_conf_model *model,
	       const struct wifi_conf_profile *profile)
{
	size_t index;

	for (index = 0; index < model->profile_count; index++)
		if (model->profiles[index].ssid_length == profile->ssid_length &&
		    memcmp(model->profiles[index].ssid, profile->ssid,
			   profile->ssid_length) == 0)
			return 1;
	return 0;
}

static int
parse_record(const unsigned char *line, size_t length,
	     struct wifi_conf_model *model, size_t record, char *error,
	     size_t error_capacity)
{
	const unsigned char *cursor = line;
	const unsigned char *end = line + length;
	struct wifi_conf_profile *profile;
	size_t index;

	if (model->profile_count == WIFI_CONF_PROFILE_MAX)
		return wifi_conf_fail(error, error_capacity, E2BIG,
		    "wifi.conf: profile count exceeds %u",
		    WIFI_CONF_PROFILE_MAX);
	profile = &model->profiles[model->profile_count];
	memset(profile, 0, sizeof(*profile));
	if (consume_literal(&cursor, end, "network ") != 0 ||
	    decode_quoted(&cursor, end, profile->ssid,
		WIFI_CONF_SSID_MAX, &profile->ssid_length, record, "SSID",
		error, error_capacity) != 0)
		return -1;
	if (profile->ssid_length == 0)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has empty SSID",
		    (unsigned long)record);
	for (index = 0; index < profile->ssid_length; index++)
		if (profile->ssid[index] == 0)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: record %lu SSID contains NUL",
			    (unsigned long)record);
	if (consume_literal(&cursor, end, " " WIFI_CONF_SECURITY " ") != 0)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has unsupported security field",
		    (unsigned long)record);
	if (decode_quoted(&cursor, end, profile->passphrase,
		WIFI_CONF_PASSPHRASE_MAX, &profile->passphrase_length,
		record, "passphrase", error, error_capacity) != 0)
		return -1;
	if (profile->passphrase_length < 8U)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu passphrase is outside length bounds",
		    (unsigned long)record);
	for (index = 0; index < profile->passphrase_length; index++)
		if (profile->passphrase[index] < 0x20U ||
		    profile->passphrase[index] > 0x7eU)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: record %lu passphrase is not printable ASCII",
			    (unsigned long)record);
	if (consume_literal(&cursor, end, " ") != 0)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has malformed mode field",
		    (unsigned long)record);
	if (consume_literal(&cursor, end, "auto") == 0)
		profile->automatic = 1;
	else if (consume_literal(&cursor, end, "manual") == 0)
		profile->automatic = 0;
	else
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has unsupported mode",
		    (unsigned long)record);
	if (cursor != end)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: record %lu has trailing syntax",
		    (unsigned long)record);
	if (ssid_duplicate(model, profile))
		return wifi_conf_fail(error, error_capacity, EEXIST,
		    "wifi.conf: record %lu duplicates an SSID",
		    (unsigned long)record);
	if (profile->passphrase_length >
	    WIFI_CONF_PASSPHRASE_TOTAL_MAX - model->passphrase_bytes)
		return wifi_conf_fail(error, error_capacity, E2BIG,
		    "wifi.conf: decoded passphrase total exceeds %u bytes",
		    WIFI_CONF_PASSPHRASE_TOTAL_MAX);
	model->passphrase_bytes += profile->passphrase_length;
	model->profile_count++;
	return 0;
}

int
wifi_conf_parse(const void *input, size_t length, struct wifi_conf_model *model,
		char *error, size_t error_capacity)
{
	const unsigned char *bytes = input;
	const unsigned char *cursor;
	const unsigned char *end;
	const unsigned char *newline;
	struct wifi_conf_model parsed;
	size_t line_length;
	size_t record = 0;
	int result = -1;

	if (model == NULL || (input == NULL && length != 0))
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: invalid parser arguments");
	if (length > WIFI_CONF_FILE_MAX)
		return wifi_conf_fail(error, error_capacity, EFBIG,
		    "wifi.conf: file exceeds %u bytes", WIFI_CONF_FILE_MAX);
	if (length == 0)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: missing version header");
	if (memchr(bytes, '\0', length) != NULL)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: embedded NUL is not allowed");
	if (memchr(bytes, '\r', length) != NULL)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: carriage return is not allowed");
	wifi_conf_model_init(&parsed);
	cursor = bytes;
	end = bytes + length;
	newline = memchr(cursor, '\n', (size_t)(end - cursor));
	if (newline == NULL)
		goto missing_newline;
	line_length = (size_t)(newline - cursor) + 1U;
	if (line_length > WIFI_CONF_LINE_MAX) {
		wifi_conf_fail(error, error_capacity, E2BIG,
		    "wifi.conf: header line exceeds %u bytes", WIFI_CONF_LINE_MAX);
		goto out;
	}
	if (line_length != sizeof(WIFI_CONF_HEADER) - 1U ||
	    memcmp(cursor, WIFI_CONF_HEADER, sizeof(WIFI_CONF_HEADER) - 1U) != 0) {
		wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: unsupported or malformed version header");
		goto out;
	}
	cursor = newline + 1;
	while (cursor < end) {
		newline = memchr(cursor, '\n', (size_t)(end - cursor));
		if (newline == NULL)
			goto missing_newline;
		line_length = (size_t)(newline - cursor) + 1U;
		if (line_length > WIFI_CONF_LINE_MAX) {
			wifi_conf_fail(error, error_capacity, E2BIG,
			    "wifi.conf: record %lu line exceeds %u bytes",
			    (unsigned long)(record + 1U), WIFI_CONF_LINE_MAX);
			goto out;
		}
		record++;
		if (parse_record(cursor, line_length - 1U, &parsed, record,
			error, error_capacity) != 0)
			goto out;
		cursor = newline + 1;
	}
	wifi_conf_model_clear(model);
	*model = parsed;
	wifi_conf_model_clear(&parsed);
	result = 0;
	goto out;

missing_newline:
	wifi_conf_fail(error, error_capacity, EINVAL,
	    "wifi.conf: final newline is required");
out:
	wifi_conf_model_clear(&parsed);
	return result;
}

int
wifi_conf_validate(const struct wifi_conf_model *model, char *error,
		   size_t error_capacity)
{
	size_t index, other, total = 0;

	if (model == NULL)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: missing model");
	if (model->profile_count > WIFI_CONF_PROFILE_MAX)
		return wifi_conf_fail(error, error_capacity, E2BIG,
		    "wifi.conf: profile count exceeds %u", WIFI_CONF_PROFILE_MAX);
	for (index = 0; index < model->profile_count; index++) {
		const struct wifi_conf_profile *profile = &model->profiles[index];

		if (profile->ssid_length == 0 ||
		    profile->ssid_length > WIFI_CONF_SSID_MAX)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: model record %lu has invalid SSID length",
			    (unsigned long)(index + 1U));
		for (other = 0; other < profile->ssid_length; other++)
			if (profile->ssid[other] == 0)
				return wifi_conf_fail(error, error_capacity, EINVAL,
				    "wifi.conf: model record %lu SSID contains NUL",
				    (unsigned long)(index + 1U));
		if (profile->passphrase_length < 8U ||
		    profile->passphrase_length > WIFI_CONF_PASSPHRASE_MAX)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: model record %lu has invalid passphrase length",
			    (unsigned long)(index + 1U));
		for (other = 0; other < profile->passphrase_length; other++)
			if (profile->passphrase[other] < 0x20U ||
			    profile->passphrase[other] > 0x7eU)
				return wifi_conf_fail(error, error_capacity, EINVAL,
				    "wifi.conf: model record %lu passphrase is not printable ASCII",
				    (unsigned long)(index + 1U));
		if (profile->automatic != 0 && profile->automatic != 1)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: model record %lu has invalid mode",
			    (unsigned long)(index + 1U));
		if (profile->passphrase_length >
		    WIFI_CONF_PASSPHRASE_TOTAL_MAX - total)
			return wifi_conf_fail(error, error_capacity, E2BIG,
			    "wifi.conf: decoded passphrase total exceeds %u bytes",
			    WIFI_CONF_PASSPHRASE_TOTAL_MAX);
		total += profile->passphrase_length;
		for (other = 0; other < index; other++)
			if (model->profiles[other].ssid_length ==
				    profile->ssid_length &&
			    memcmp(model->profiles[other].ssid, profile->ssid,
				   profile->ssid_length) == 0)
				return wifi_conf_fail(error, error_capacity, EEXIST,
				    "wifi.conf: model contains duplicate SSIDs");
	}
	if (total != model->passphrase_bytes)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: model passphrase accounting mismatch");
	return 0;
}

int
wifi_conf_validate_profile(const void *ssid_pointer, size_t ssid_length,
		   const void *passphrase_pointer, size_t passphrase_length,
		   char *error, size_t error_capacity)
{
	const unsigned char *ssid = ssid_pointer;
	const unsigned char *passphrase = passphrase_pointer;
	size_t index;

	if (ssid == NULL || passphrase == NULL)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: missing profile field");
	if (ssid_length == 0 || ssid_length > WIFI_CONF_SSID_MAX)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: command SSID is outside length bounds");
	for (index = 0; index < ssid_length; index++)
		if (ssid[index] == 0)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: command SSID contains NUL");
	if (passphrase_length < 8U ||
	    passphrase_length > WIFI_CONF_PASSPHRASE_MAX)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: command passphrase is outside length bounds");
	for (index = 0; index < passphrase_length; index++)
		if (passphrase[index] < 0x20U || passphrase[index] > 0x7eU)
			return wifi_conf_fail(error, error_capacity, EINVAL,
			    "wifi.conf: command passphrase is not printable ASCII");
	return 0;
}

int
wifi_conf_set_key(struct wifi_conf_model *model, const void *ssid_pointer,
		  size_t ssid_length, const void *passphrase_pointer,
		  size_t passphrase_length, int automatic, char *error,
		  size_t error_capacity)
{
	const unsigned char *ssid = ssid_pointer;
	const unsigned char *passphrase = passphrase_pointer;
	struct wifi_conf_model updated;
	struct wifi_conf_profile *profile;
	size_t index;

	if (model == NULL)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: invalid set-key arguments");
	if (wifi_conf_validate_profile(ssid, ssid_length, passphrase,
		passphrase_length, error, error_capacity) != 0)
		return -1;
	if (automatic != 0 && automatic != 1)
		return wifi_conf_fail(error, error_capacity, EINVAL,
		    "wifi.conf: command mode is invalid");
	if (wifi_conf_validate(model, error, error_capacity) != 0)
		return -1;
	updated = *model;
	profile = NULL;
	for (index = 0; index < updated.profile_count; index++)
		if (updated.profiles[index].ssid_length == ssid_length &&
		    memcmp(updated.profiles[index].ssid, ssid, ssid_length) == 0) {
			profile = &updated.profiles[index];
			break;
		}
	if (profile == NULL) {
		if (updated.profile_count == WIFI_CONF_PROFILE_MAX) {
			wifi_conf_model_clear(&updated);
			return wifi_conf_fail(error, error_capacity, E2BIG,
			    "wifi.conf: profile count exceeds %u",
			    WIFI_CONF_PROFILE_MAX);
		}
		profile = &updated.profiles[updated.profile_count++];
		memset(profile, 0, sizeof(*profile));
		memcpy(profile->ssid, ssid, ssid_length);
		profile->ssid_length = ssid_length;
	} else {
		updated.passphrase_bytes -= profile->passphrase_length;
		wifi_conf_explicit_clear(profile->passphrase,
		    sizeof(profile->passphrase));
	}
	if (passphrase_length >
	    WIFI_CONF_PASSPHRASE_TOTAL_MAX - updated.passphrase_bytes) {
		wifi_conf_model_clear(&updated);
		return wifi_conf_fail(error, error_capacity, E2BIG,
		    "wifi.conf: decoded passphrase total exceeds %u bytes",
		    WIFI_CONF_PASSPHRASE_TOTAL_MAX);
	}
	memcpy(profile->passphrase, passphrase, passphrase_length);
	profile->passphrase_length = passphrase_length;
	profile->automatic = automatic;
	updated.passphrase_bytes += passphrase_length;
	if (wifi_conf_validate(&updated, error, error_capacity) != 0) {
		wifi_conf_model_clear(&updated);
		return -1;
	}
	wifi_conf_model_clear(model);
	*model = updated;
	wifi_conf_model_clear(&updated);
	return 0;
}

static size_t
encoded_length(const unsigned char *bytes, size_t length)
{
	size_t index, result = 0;

	for (index = 0; index < length; index++) {
		unsigned char byte = bytes[index];

		if (byte >= 0x20U && byte <= 0x7eU && byte != '"' &&
		    byte != '\\')
			result++;
		else if (byte == '"' || byte == '\\' || byte == '\t' ||
			 byte == '\n' || byte == '\r')
			result += 2U;
		else
			result += 4U;
	}
	return result;
}

static unsigned char *
append_literal(unsigned char *output, const char *literal)
{
	size_t length = strlen(literal);

	memcpy(output, literal, length);
	return output + length;
}

static unsigned char *
append_quoted(unsigned char *output, const unsigned char *bytes, size_t length)
{
	static const unsigned char hex[] = "0123456789ABCDEF";
	size_t index;

	*output++ = '"';
	for (index = 0; index < length; index++) {
		unsigned char byte = bytes[index];

		if (byte >= 0x20U && byte <= 0x7eU && byte != '"' &&
		    byte != '\\') {
			*output++ = byte;
			continue;
		}
		*output++ = '\\';
		switch (byte) {
		case '"':
			*output++ = '"';
			break;
		case '\\':
			*output++ = '\\';
			break;
		case '\t':
			*output++ = 't';
			break;
		case '\n':
			*output++ = 'n';
			break;
		case '\r':
			*output++ = 'r';
			break;
		default:
			*output++ = 'x';
			*output++ = hex[byte >> 4];
			*output++ = hex[byte & 0x0fU];
			break;
		}
	}
	*output++ = '"';
	return output;
}

int
wifi_conf_serialize(const struct wifi_conf_model *model, void *output,
		    size_t output_capacity, size_t *output_length, char *error,
		    size_t error_capacity)
{
	unsigned char *cursor = output;
	size_t index, needed = sizeof(WIFI_CONF_HEADER) - 1U;

	if (wifi_conf_validate(model, error, error_capacity) != 0)
		return -1;
	for (index = 0; index < model->profile_count; index++) {
		const struct wifi_conf_profile *profile = &model->profiles[index];
		size_t line_length;

		line_length = sizeof("network ") - 1U + 2U +
		    encoded_length(profile->ssid, profile->ssid_length) + 1U +
		    sizeof(WIFI_CONF_SECURITY) - 1U + 1U + 2U +
		    encoded_length(profile->passphrase,
			profile->passphrase_length) + 1U +
		    (profile->automatic ? 4U : 6U) + 1U;
		if (line_length > WIFI_CONF_LINE_MAX ||
		    line_length > WIFI_CONF_FILE_MAX - needed)
			return wifi_conf_fail(error, error_capacity, E2BIG,
			    "wifi.conf: canonical generation exceeds format bounds");
		needed += line_length;
	}
	if (output == NULL || output_capacity < needed)
		return wifi_conf_fail(error, error_capacity, ENOSPC,
		    "wifi.conf: serialization buffer is too small");
	cursor = append_literal(cursor, WIFI_CONF_HEADER);
	for (index = 0; index < model->profile_count; index++) {
		const struct wifi_conf_profile *profile = &model->profiles[index];

		cursor = append_literal(cursor, "network ");
		cursor = append_quoted(cursor, profile->ssid,
		    profile->ssid_length);
		cursor = append_literal(cursor, " " WIFI_CONF_SECURITY " ");
		cursor = append_quoted(cursor, profile->passphrase,
		    profile->passphrase_length);
		cursor = append_literal(cursor,
		    profile->automatic ? " auto\n" : " manual\n");
	}
	if (output_capacity > needed)
		*cursor = '\0';
	if (output_length != NULL)
		*output_length = needed;
	return 0;
}
