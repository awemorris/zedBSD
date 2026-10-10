/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private JSON reader/writer: metadata extensions survive even when their schema is unknown. */
#include "json.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MJ_BYTES_MAX (64U * 1024U * 1024U)
#define MJ_DEPTH_MAX 64U

/* The cursor owns no storage and reports parsing errors separately from allocation failure. */
struct mj_cursor {
	const char *at;
	const char *end;
	int error;
};
static struct mj_value *json_value(struct mj_cursor *cursor, unsigned depth);
static char *json_string(struct mj_cursor *cursor);
static void json_space(struct mj_cursor *cursor);
static int json_hex(struct mj_cursor *cursor, unsigned *number);
static int json_quote(FILE *output, const char *text);
static int json_emit(FILE *output, const struct mj_value *value, unsigned depth);

/*
 * Allocates an empty typed JSON value.
 */
struct mj_value *
mj_new(
	int type)
{
	struct mj_value *value;

	/* Each value exclusively owns its fields and children. */
	value = calloc(1U, sizeof(*value));
	if (value != NULL)
		value->type = type;

	/* Succeeded: NULL leaves allocation failure visible to the caller. */
	return value;
}

/*
 * Frees one value and its sibling chain recursively.
 */
void
mj_free(
	struct mj_value *value)
{
	struct mj_value *next;

	/* Children precede their parent; the parser caps nesting depth. */
	while (value != NULL) {
		next = value->next;
		mj_free(value->child);
		free(value->key);
		free(value->text);
		free(value);
		value = next;
	}
}

/*
 * Reads a complete bounded JSON document, rejecting trailing or incomplete data.
 */
int
mj_read(
	FILE *input,
	struct mj_value **value)
{
	struct mj_cursor cursor;
	char *bytes;
	char *grown;
	size_t length;
	size_t capacity;
	size_t count;
	int error;

	/* Grow only as needed; metadata files are independent of original media size. */
	bytes = NULL;
	length = 0U;
	capacity = 0U;
	*value = NULL;
	error = 0;
	for (;;) {
		if (capacity - length < 4096U) {
			if (capacity >= MJ_BYTES_MAX) {
				error = EFBIG;
				break;
			}

			/* Bound the document before reading an additional chunk. */
			capacity += 4096U;
			grown = realloc(bytes, capacity);
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}

			/* The successfully grown buffer retains all earlier bytes. */
			bytes = grown;
		}

		/* fread distinguishes a complete document from IO failure. */
		count = fread(bytes + length, 1U, capacity - length, input);
		length += count;
		if (count == 0U)
			break;
	}

	/* Reject IO errors before parsing any mutable model. */
	count = (size_t)ferror(input);
	if (count != 0U && error == 0)
		error = EIO;
	if (error == 0) {
		cursor.at = bytes;
		cursor.end = bytes + length;
		cursor.error = 0;
		*value = json_value(&cursor, 0U);
		json_space(&cursor);
		error = cursor.error;
		if (error == 0 && cursor.at != cursor.end)
			error = EINVAL;
	}

	/* A failed parse must not leave a partially usable document. */
	free(bytes);
	if (error != 0) {
		mj_free(*value);
		*value = NULL;
		return error;
	}

	/* Succeeded: the caller owns a complete JSON tree. */
	return 0;
}

/*
 * Writes valid JSON with line breaks between container elements.
 */
int
mj_write(
	FILE *output,
	const struct mj_value *value)
{
	int error;
	int result;

	/* Preserve all object keys, including future metadata extensions. */
	error = json_emit(output, value, 0U);
	if (error != 0)
		return error;
	result = fputc('\n', output);
	if (result == EOF)
		return EIO;

	/* Succeeded: flush/durability remain the persistence owner's responsibility. */
	return 0;
}

/*
 * Finds an object field by name, without transferring ownership.
 */
struct mj_value *
mj_get(
	struct mj_value *object,
	const char *key)
{
	struct mj_value *child;
	int same;

	/* Arrays and scalar values have no named fields. */
	if (object == NULL || object->type != MJ_OBJECT)
		return NULL;
	for (child = object->child; child != NULL; child = child->next) {
		same = strcmp(child->key, key);
		if (same == 0)
			return child;
	}

	/* Succeeded: a missing optional field is represented by NULL. */
	return NULL;
}

/*
 * Sets a scalar field while preserving unrelated extension fields.
 */
int
mj_set(
	struct mj_value *object,
	const char *key,
	int type,
	const char *text)
{
	struct mj_value *value;
	char *copy;
	int error;

	/* Build the replacement before changing an existing scalar. */
	copy = strdup(text);
	if (copy == NULL)
		return ENOMEM;
	value = mj_get(object, key);
	if (value == NULL) {
		value = mj_new(type);
		if (value == NULL) {
			free(copy);
			return ENOMEM;
		}

		/* A failed key allocation leaves both tree and replacement unmodified. */
		error = mj_attach(object, key, value);
		if (error != 0) {
			mj_free(value);
			free(copy);
			return error;
		}
	}

	/* The field retains its position; its former owned value is replaced. */
	mj_free(value->child);
	value->child = NULL;
	value->tail = NULL;
	free(value->text);
	value->text = copy;
	value->type = type;

	/* Succeeded: other fields and their nested data remain untouched. */
	return 0;
}

/*
 * Stores an integer as a JSON number.
 */
int
mj_number(
	struct mj_value *object,
	const char *key,
	int64_t number)
{
	char text[32];
	int error;

	/* Integer formatting is independent of locale. */
	snprintf(text, sizeof(text), "%lld", (long long)number);
	error = mj_set(object, key, MJ_NUMBER, text);

	/* Succeeded: the object owns the formatted number. */
	return error;
}

/*
 * Attaches a new named value; the caller retains ownership on failure.
 */
int
mj_attach(
	struct mj_value *object,
	const char *key,
	struct mj_value *value)
{
	/* Allocate the key before transferring the value to its new parent. */
	value->key = strdup(key);
	if (value->key == NULL)
		return ENOMEM;
	mj_append(object, value);

	/* Succeeded: the parent's child list owns the value. */
	return 0;
}

/*
 * Appends one owned child value to a container.
 */
void
mj_append(
	struct mj_value *array,
	struct mj_value *value)
{
	/* Append in constant time even for a library with thousands of files. */
	if (array->tail == NULL)
		array->child = value;
	else
		array->tail->next = value;
	array->tail = value;
}

/* Advances over exactly the whitespace admitted by JSON. */
static void
json_space(
	struct mj_cursor *cursor)
{
	/* String whitespace is handled only by json_string. */
	while (cursor->at < cursor->end && (*cursor->at == ' ' || *cursor->at == '\n' || *cursor->at == '\r' || *cursor->at == '\t'))
		cursor->at++;
}

/* Reads a four-digit Unicode escape. */
static int
json_hex(
	struct mj_cursor *cursor,
	unsigned *number)
{
	unsigned index;
	unsigned byte;

	/* Require four valid digits before decoding a code unit. */
	*number = 0U;
	for (index = 0U; index < 4U; index++) {
		if (cursor->at == cursor->end)
			return EINVAL;
		byte = (unsigned char)*cursor->at++;
		if (byte >= '0' && byte <= '9')
			byte -= '0';
		else if (byte >= 'a' && byte <= 'f')
			byte = byte - 'a' + 10U;
		else if (byte >= 'A' && byte <= 'F')
			byte = byte - 'A' + 10U;
		else
			return EINVAL;
		*number = *number * 16U + byte;
	}

	/* Succeeded: one UTF-16 code unit is available. */
	return 0;
}

/* Decodes escaped JSON strings to UTF-8, including paired Unicode surrogates. */
static char *
json_string(
	struct mj_cursor *cursor)
{
	char *text;
	const char *limit;
	size_t length;
	unsigned byte;
	unsigned low;
	int error;

	/* Decoding never produces more bytes than the encoded string's remaining span. */
	if (cursor->at == cursor->end || *cursor->at++ != '"') {
		cursor->error = EINVAL;
		return NULL;
	}

	/* Bound the allocation by the next unescaped closing quote, not the whole document. */
	limit = cursor->at;
	while (limit < cursor->end && *limit != '"') {
		if (*limit == '\\' && limit + 1 < cursor->end)
			limit++;
		limit++;
	}

	/* Allocate for this encoded string alone, avoiding quadratic memory use. */
	text = malloc((size_t)(limit - cursor->at) + 1U);
	if (text == NULL) {
		cursor->error = ENOMEM;
		return NULL;
	}

	/* A complete string excludes unescaped control characters and invalid escapes. */
	length = 0U;
	while (cursor->at < cursor->end) {
		byte = (unsigned char)*cursor->at++;
		if (byte == '"') {
			text[length] = 0;
			return text;
		}

		/* Ordinary UTF-8 bytes need no conversion. */
		if (byte < 32U)
			break;
		if (byte == '\\') {
			if (cursor->at == cursor->end)
				break;
			byte = (unsigned char)*cursor->at++;
			if (byte == 'u') {
				error = json_hex(cursor, &byte);
				if (error != 0 || byte == 0U)
					break;
				if (byte >= 0xd800U && byte <= 0xdbffU) {
					if (cursor->end - cursor->at < 2 || cursor->at[0] != '\\' || cursor->at[1] != 'u')
						break;
					cursor->at += 2;
					error = json_hex(cursor, &low);
					if (error != 0 || low < 0xdc00U || low > 0xdfffU)
						break;
					byte = 0x10000U + ((byte - 0xd800U) << 10U) + low - 0xdc00U;
				} else if (byte >= 0xdc00U && byte <= 0xdfffU) {
					break;
				}

				/* Encode non-ASCII scalars without losing extension text. */
				if (byte >= 0x10000U) {
					text[length++] = (char)(0xf0U | (byte >> 18U));
					text[length++] = (char)(0x80U | ((byte >> 12U) & 63U));
					text[length++] = (char)(0x80U | ((byte >> 6U) & 63U));
					byte = 0x80U | (byte & 63U);
				} else if (byte >= 0x800U) {
					text[length++] = (char)(0xe0U | (byte >> 12U));
					text[length++] = (char)(0x80U | ((byte >> 6U) & 63U));
					byte = 0x80U | (byte & 63U);
				} else if (byte >= 0x80U) {
					text[length++] = (char)(0xc0U | (byte >> 6U));
					byte = 0x80U | (byte & 63U);
				}
			} else if (byte == 'b') {
				byte = '\b';
			} else if (byte == 'f') {
				byte = '\f';
			} else if (byte == 'n') {
				byte = '\n';
			} else if (byte == 'r') {
				byte = '\r';
			} else if (byte == 't') {
				byte = '\t';
			} else if (byte != '"' && byte != '\\' && byte != '/') {
				break;
			}
		}

		/* Store exactly the decoded string bytes. */
		text[length++] = (char)byte;
	}

	/* Incomplete or malformed strings cannot become editable metadata. */
	free(text);
	cursor->error = EINVAL;
	return NULL;
}

/* Parses one value recursively, preserving scalars' lexical representation. */
static struct mj_value *
json_value(
	struct mj_cursor *cursor,
	unsigned depth)
{
	struct mj_value *value;
	struct mj_value *child;
	const char *start;
	char *key;
	char close;
	int type;
	int same;

	/* Limits nesting and refuses empty input. */
	json_space(cursor);
	if (depth > MJ_DEPTH_MAX || cursor->at == cursor->end) {
		cursor->error = EINVAL;
		return NULL;
	}

	/* Container and string syntax identify their JSON types directly. */
	type = MJ_NUMBER;
	if (*cursor->at == '{')
		type = MJ_OBJECT;
	else if (*cursor->at == '[')
		type = MJ_ARRAY;
	else if (*cursor->at == '"')
		type = MJ_STRING;
	value = mj_new(type);
	if (value == NULL) {
		cursor->error = ENOMEM;
		return NULL;
	}

	/* Read container entries with mandatory commas and object colons. */
	if (type == MJ_OBJECT || type == MJ_ARRAY) {
		close = ']';
		if (type == MJ_OBJECT)
			close = '}';
		cursor->at++;
		json_space(cursor);
		if (cursor->at < cursor->end && *cursor->at == close) {
			cursor->at++;
			return value;
		}

		/* Recursion attaches only completely parsed child values. */
		while (cursor->error == 0) {
			key = NULL;
			if (type == MJ_OBJECT) {
				key = json_string(cursor);
				json_space(cursor);
				if (cursor->error == 0 && cursor->at < cursor->end && *cursor->at == ':')
					cursor->at++;
				else
					cursor->error = EINVAL;
			}

			/* A failed child owns no part of its prospective parent. */
			child = NULL;
			if (cursor->error == 0)
				child = json_value(cursor, depth + 1U);
			if (child == NULL) {
				free(key);
				break;
			}

			/* Preserve unknown field names and nested structures. */
			child->key = key;
			mj_append(value, child);
			json_space(cursor);
			if (cursor->at == cursor->end) {
				cursor->error = EINVAL;
				break;
			}

			/* A comma continues the container; a closing delimiter ends it. */
			if (*cursor->at == close) {
				cursor->at++;
				return value;
			}

			/* Reject missing separators and trailing commas. */
			if (*cursor->at++ != ',') {
				cursor->error = EINVAL;
				break;
			}

			/* The next element may begin after whitespace. */
			json_space(cursor);
		}
	} else if (type == MJ_STRING) {
		value->text = json_string(cursor);
		if (cursor->error == 0)
			return value;
	} else {
		/* Numbers use JSON's grammar; future decimal/exponent fields remain lossless. */
		start = cursor->at;
		if (*cursor->at == '-')
			cursor->at++;
		if (cursor->at < cursor->end && *cursor->at == '0') {
			cursor->at++;
		} else if (cursor->at < cursor->end && *cursor->at >= '1' && *cursor->at <= '9') {
			while (cursor->at < cursor->end && *cursor->at >= '0' && *cursor->at <= '9')
				cursor->at++;
		} else {
			/* Only the three named literals are accepted outside numeric syntax. */
			cursor->at = start;
			type = MJ_LITERAL;
			if (cursor->end - cursor->at >= 4) {
				same = memcmp(cursor->at, "true", 4U);
				if (same == 0)
					cursor->at += 4;
				same = memcmp(start, "null", 4U);
				if (same == 0)
					cursor->at += 4;
			}

			/* false has a different length. */
			if (cursor->at == start && cursor->end - cursor->at >= 5) {
				same = memcmp(cursor->at, "false", 5U);
				if (same == 0)
					cursor->at += 5;
			}

			/* No recognized literal means invalid scalar syntax. */
			if (cursor->at == start)
				cursor->error = EINVAL;
		}

		/* Decimal and exponent components require at least one digit. */
		if (type == MJ_NUMBER) {
			if (cursor->at < cursor->end && *cursor->at == '.') {
				cursor->at++;
				if (cursor->at == cursor->end || *cursor->at < '0' || *cursor->at > '9')
					cursor->error = EINVAL;
				while (cursor->at < cursor->end && *cursor->at >= '0' && *cursor->at <= '9')
					cursor->at++;
			}

			/* Preserve exponent signs and digits exactly. */
			if (cursor->at < cursor->end && (*cursor->at == 'e' || *cursor->at == 'E')) {
				cursor->at++;
				if (cursor->at < cursor->end && (*cursor->at == '+' || *cursor->at == '-'))
					cursor->at++;
				if (cursor->at == cursor->end || *cursor->at < '0' || *cursor->at > '9')
					cursor->error = EINVAL;
				while (cursor->at < cursor->end && *cursor->at >= '0' && *cursor->at <= '9')
					cursor->at++;
			}
		}

		/* Copy lexical scalar text to owned storage. */
		if (cursor->error == 0) {
			value->type = type;
			value->text = strndup(start, (size_t)(cursor->at - start));
			if (value->text != NULL)
				return value;
			cursor->error = ENOMEM;
		}
	}

	/* Partial trees are never handed to persistence code. */
	mj_free(value);
	return NULL;
}

/* Quotes UTF-8 text with JSON escapes for control bytes. */
static int
json_quote(
	FILE *output,
	const char *text)
{
	const unsigned char *at;
	int result;

	/* UTF-8 bytes remain readable; JSON punctuation is escaped. */
	result = fputc('"', output);
	for (at = (const unsigned char *)text; *at != 0U && result != EOF; at++) {
		if (*at == '"' || *at == '\\')
			result = fputc('\\', output);
		if (result == EOF)
			break;
		if (*at < 32U)
			result = fprintf(output, "\\u%04x", (unsigned)*at);
		else
			result = fputc(*at, output);
	}

	/* A write failure is explicit to the atomic-save owner. */
	if (result == EOF)
		return EIO;
	result = fputc('"', output);
	if (result == EOF)
		return EIO;

	/* Succeeded: the text is a valid quoted JSON string. */
	return 0;
}

/* Serializes containers recursively without dropping unfamiliar fields. */
static int
json_emit(
	FILE *output,
	const struct mj_value *value,
	unsigned depth)
{
	const struct mj_value *child;
	unsigned index;
	int error;
	int result;
	char open;
	char close;

	/* Scalars preserve exact integer and future numeric forms. */
	if (value == NULL || depth > MJ_DEPTH_MAX)
		return EINVAL;
	if (value->type == MJ_STRING)
		return json_quote(output, value->text);
	if (value->type == MJ_NUMBER || value->type == MJ_LITERAL) {
		result = fputs(value->text, output);
		if (result == EOF)
			return EIO;
		return 0;
	}

	/* Containers use indentation to make manual recovery straightforward. */
	open = '[';
	close = ']';
	if (value->type == MJ_OBJECT) {
		open = '{';
		close = '}';
	}

	/* Write fields in their existing order. */
	result = fputc(open, output);
	for (child = value->child; child != NULL && result != EOF; child = child->next) {
		result = fputc('\n', output);
		for (index = 0U; index <= depth && result != EOF; index++)
			result = fputs("  ", output);
		if (child->key != NULL) {
			error = json_quote(output, child->key);
			if (error != 0)
				return error;
			result = fputs(": ", output);
		}

		/* Descendant failure aborts the replacement file. */
		error = json_emit(output, child, depth + 1U);
		if (error != 0)
			return error;
		if (child->next != NULL)
			result = fputc(',', output);
	}

	/* Close the container at its own indentation level. */
	if (result == EOF)
		return EIO;
	if (value->child != NULL) {
		result = fputc('\n', output);
		for (index = 0U; index < depth && result != EOF; index++)
			result = fputs("  ", output);
	}

	/* Publish the closing delimiter only after the last child. */
	result = fputc(close, output);
	if (result == EOF)
		return EIO;

	/* Succeeded: every value is complete and syntactically closed. */
	return 0;
}
