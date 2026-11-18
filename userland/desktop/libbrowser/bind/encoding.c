/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Encoding API for scripts (ws074-p080): TextEncoder and TextDecoder,
 * UTF-8 only.
 *
 * The encoder writes a string's UTF-8, a lone surrogate as U+FFFD.  The
 * decoder is the Encoding Standard's UTF-8 decoder: a byte sequence that
 * is not UTF-8 decodes to U+FFFD for each maximal part of it (or throws a
 * TypeError when the decoder is fatal), a leading byte order mark is
 * dropped unless ignoreBOM, and with {stream: true} an unfinished
 * sequence waits for the next call.
 *
 * The engine has no typed arrays yet, so encode returns an array of the
 * bytes' numbers, and decode and encodeInto take any array-like object
 * (its length and its elements, each byte taken modulo 256): an array, the
 * arrays encode returns, and typed arrays once the engine has them.
 */

#include "bind/internal.h"

#include <errno.h>
#include <string.h>

/* The replacement character. */
#define ENCODING_REPLACEMENT	0xfffdU

/* The byte order mark. */
#define ENCODING_BOM		0xfeffU

/*
 * The state of a TextDecoder, the cell its object wraps: its options, and
 * the UTF-8 decoder's state between calls of a stream (the bytes still
 * needed and seen for the sequence under way, the code point so far and
 * the range the next byte must be in), and whether the byte order mark was
 * looked for since the stream began.
 */
struct encoding_decoder {
	struct vm_cell cell;
	int fatal;
	int ignore_bom;
	int bom_seen;
	unsigned needed;
	unsigned seen;
	uint32_t code_point;
	unsigned lower;
	unsigned upper;
};

static int encoding_decoder_of(struct vm_realm *realm, vm_value value, struct encoding_decoder **decoder);
static int encoding_encoder_check(struct vm_realm *realm, vm_value value);
static int encoding_label_is_utf8(const struct vm_string *label);
static int encoding_bytes(struct vm_realm *realm, vm_value input, struct wb_buffer *bytes);
static int encoding_decode_bytes(struct vm_realm *realm, struct encoding_decoder *decoder, const unsigned char *bytes, size_t length, int stream, struct wb_units *out);
static int encoding_step(struct encoding_decoder *decoder, unsigned byte, struct wb_units *out, int *consumed, int *error);
static int encoding_error(struct vm_realm *realm, struct encoding_decoder *decoder, struct wb_units *out);
static int encoding_emit(struct encoding_decoder *decoder, uint32_t code_point, struct wb_units *out);
static void encoding_reset(struct encoding_decoder *decoder);
static size_t encoding_next_code_point(const struct vm_string *string, size_t index, uint32_t *code_point);
static int encoding_encoder_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_encoder_encoding(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_encode(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_encode_into(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_decoder_construct(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_decoder_encoding(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_decoder_fatal(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_decoder_ignore_bom(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);
static int encoding_decode(struct vm_realm *realm, vm_value this_value, const vm_value *args, unsigned count, vm_value *result);

/* The state of a TextDecoder, which refers to nothing. */
static const struct vm_cell_type encoding_decoder_type = { "text-decoder", NULL, NULL };

/* The mark of a TextEncoder's object, which has no state. */
static const struct vm_cell_type encoding_encoder_type = { "text-encoder", NULL, NULL };

/*
 * The labels of UTF-8 (the Encoding Standard's table).  The table is
 * constant for the life of the program and ends with NULL.
 */
static const char *const encoding_utf8_labels[] = {
	"unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8", NULL
};

/*
 * The attributes of TextEncoder.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute encoding_encoder_attributes[] = {
	{ "encoding", encoding_encoder_encoding, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of TextEncoder.  The table is constant for the life of
 * the program.
 */
static const struct bind_operation encoding_encoder_operations[] = {
	{ "encode", 0, encoding_encode },
	{ "encodeInto", 2, encoding_encode_into },
	{ NULL, 0, NULL }
};

/*
 * The TextEncoder interface.
 */
const struct bind_interface bind_text_encoder_interface = {
	"TextEncoder", BIND_NO_PARENT, 0, encoding_encoder_construct, encoding_encoder_attributes, encoding_encoder_operations, NULL
};

/*
 * The attributes of TextDecoder.  The table is constant for the life of
 * the program.
 */
static const struct bind_attribute encoding_decoder_attributes[] = {
	{ "encoding", encoding_decoder_encoding, NULL },
	{ "fatal", encoding_decoder_fatal, NULL },
	{ "ignoreBOM", encoding_decoder_ignore_bom, NULL },
	{ NULL, NULL, NULL }
};

/*
 * The operations of TextDecoder.  The table is constant for the life of
 * the program.
 */
static const struct bind_operation encoding_decoder_operations[] = {
	{ "decode", 0, encoding_decode },
	{ NULL, 0, NULL }
};

/*
 * The TextDecoder interface.
 */
const struct bind_interface bind_text_decoder_interface = {
	"TextDecoder", BIND_NO_PARENT, 0, encoding_decoder_construct, encoding_decoder_attributes, encoding_decoder_operations, NULL
};

/* Makes a TextEncoder (new TextEncoder()). */
static int
encoding_encoder_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct vm_cell *mark;
	struct vm_object *object;

	UNUSED_PARAMETER(this_value);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The mark that says what the object is. */
	window = bind_window_of(realm);
	mark = vm_heap_alloc(realm->heap, &encoding_encoder_type, sizeof(*mark));
	if (mark == NULL)
		return ENOMEM;

	/* The object with TextEncoder's prototype. */
	object = vm_object_create(realm->heap, window->prototypes[BIND_TEXT_ENCODER]);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(mark);

	/* Succeeded: the encoder. */
	*result = vm_value_cell(object);
	return 0;
}

/* Reports a TextEncoder's encoding, always "utf-8" (encoding). */
static int
encoding_encoder_encoding(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The this value must be an encoder. */
	status = encoding_encoder_check(realm, this_value);
	if (status != 0)
		return status;

	/* Succeeded: the name. */
	status = bind_string(realm, "utf-8", result);
	if (status != 0)
		return status;
	return 0;
}

/* Writes a string's UTF-8 as an array of the bytes' numbers (encode). */
static int
encoding_encode(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	unsigned char bytes[4];
	struct vm_string *input;
	struct vm_object *array;
	vm_value input_value;
	uint32_t code_point;
	uint32_t made;
	size_t index;
	size_t step;
	size_t length;
	size_t byte;
	int status;

	/* The encoder. */
	status = encoding_encoder_check(realm, this_value);
	if (status != 0)
		return status;

	/* The string; undefined is the empty string. */
	input_value = js_argument(args, count, 0);
	if (input_value == VM_VALUE_UNDEFINED) {
		status = bind_string(realm, "", &input_value);
		if (status != 0)
			return status;
	}

	/* Its characters. */
	status = bind_to_string(realm, input_value, &input);
	if (status != 0)
		return status;

	/* The array the bytes go into. */
	status = bind_array_create(realm, &array);
	if (status != 0)
		return status;

	/* Each code point's bytes. */
	made = 0;
	index = 0;
	while (index < input->length) {
		step = encoding_next_code_point(input, index, &code_point);
		length = wb_utf8_encode(code_point, bytes);
		for (byte = 0; byte < length; byte++) {
			status = vm_object_define(realm->heap, array, vm_value_int32((int32_t)made), vm_value_int32(bytes[byte]), VM_PROPERTY_DEFAULT);
			if (status != 0)
				return status;
			made++;
		}

		/* The next code point. */
		index += step;
	}

	/* Succeeded: the bytes. */
	*result = vm_value_cell(array);
	return 0;
}

/*
 * Writes as much of a string's UTF-8 as fits into an array-like object,
 * whole code points only, and reports {read, written}: the string's code
 * units read and the bytes written (encodeInto).
 */
static int
encoding_encode_into(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	unsigned char bytes[4];
	struct vm_string *input;
	struct vm_string *length_atom;
	struct vm_object *report;
	vm_value destination;
	vm_value length_value;
	uint32_t capacity;
	uint32_t written;
	uint32_t code_point;
	size_t read;
	size_t step;
	size_t length;
	size_t byte;
	int is_object;
	int status;

	/* The encoder, the string and the destination, which must be an object. */
	status = encoding_encoder_check(realm, this_value);
	if (status != 0)
		return status;
	status = bind_to_string(realm, js_argument(args, count, 0), &input);
	if (status != 0)
		return status;
	destination = js_argument(args, count, 1);
	is_object = vm_value_is_object(destination);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.");
		return status;
	}

	/* How many bytes it holds. */
	length_atom = vm_atom_from_ascii(realm->heap, "length");
	if (length_atom == NULL)
		return ENOMEM;
	status = vm_get(realm, destination, vm_value_cell(length_atom), &length_value);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, length_value, &capacity);
	if (status != 0)
		return status;

	/* Each whole code point that fits. */
	read = 0;
	written = 0;
	while (read < input->length) {
		step = encoding_next_code_point(input, read, &code_point);
		length = wb_utf8_encode(code_point, bytes);
		if (written + length > capacity)
			break;

		/* Its bytes. */
		for (byte = 0; byte < length; byte++) {
			status = vm_set(realm, destination, vm_value_int32((int32_t)written), vm_value_int32(bytes[byte]), 1);
			if (status != 0)
				return status;
			written++;
		}

		/* The next code point. */
		read += step;
	}

	/* The report. */
	report = vm_object_create(realm->heap, realm->object_prototype);
	if (report == NULL)
		return ENOMEM;
	status = js_builtin_value(realm, report, "read", vm_value_number((double)read), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;
	status = js_builtin_value(realm, report, "written", vm_value_number((double)written), VM_PROPERTY_DEFAULT);
	if (status != 0)
		return status;

	/* Succeeded: the counts. */
	*result = vm_value_cell(report);
	return 0;
}

/*
 * Makes a TextDecoder (new TextDecoder(label, {fatal, ignoreBOM})); a label
 * other than UTF-8's throws a RangeError.
 */
static int
encoding_decoder_construct(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct bind_window *window;
	struct encoding_decoder *decoder;
	struct vm_object *object;
	struct vm_string *label;
	struct wb_buffer message;
	vm_value option;
	int present;
	int utf8;
	int status;

	UNUSED_PARAMETER(this_value);

	/* The label, UTF-8's when none is given. */
	window = bind_window_of(realm);
	utf8 = 1;
	if (count > 0 && args[0] != VM_VALUE_UNDEFINED) {
		status = bind_to_string(realm, args[0], &label);
		if (status != 0)
			return status;
		utf8 = encoding_label_is_utf8(label);
	}

	/* Another label is an encoding this pass does not have. */
	if (!utf8) {
		wb_buffer_init(&message);
		status = wb_buffer_append_string(&message, "Failed to construct 'TextDecoder': The encoding label provided ('");
		if (status == 0)
			status = vm_string_to_utf8(label, &message);
		if (status == 0)
			status = wb_buffer_append_string(&message, "') is invalid.");
		if (status == 0)
			status = vm_throw_range_error(realm, wb_buffer_string(&message));
		wb_buffer_release(&message);
		return status;
	}

	/* The state, with the options. */
	decoder = vm_heap_alloc(realm->heap, &encoding_decoder_type, sizeof(*decoder));
	if (decoder == NULL)
		return ENOMEM;
	decoder->fatal = 0;
	decoder->ignore_bom = 0;
	encoding_reset(decoder);
	decoder->bom_seen = 0;

	/* fatal. */
	status = bind_get_option(realm, js_argument(args, count, 1), "fatal", &present, &option);
	if (status != 0)
		return status;
	if (present)
		decoder->fatal = vm_to_boolean(option);

	/* ignoreBOM. */
	status = bind_get_option(realm, js_argument(args, count, 1), "ignoreBOM", &present, &option);
	if (status != 0)
		return status;
	if (present)
		decoder->ignore_bom = vm_to_boolean(option);

	/* The object with TextDecoder's prototype. */
	object = vm_object_create(realm->heap, window->prototypes[BIND_TEXT_DECODER]);
	if (object == NULL)
		return ENOMEM;
	object->kind = VM_KIND_PLATFORM;
	object->internal = vm_value_cell(decoder);

	/* Succeeded: the decoder. */
	*result = vm_value_cell(object);
	return 0;
}

/* Reports a TextDecoder's encoding, always "utf-8" (encoding). */
static int
encoding_decoder_encoding(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct encoding_decoder *decoder;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The decoder. */
	status = encoding_decoder_of(realm, this_value, &decoder);
	if (status != 0)
		return status;

	/* Succeeded: the name. */
	status = bind_string(realm, "utf-8", result);
	if (status != 0)
		return status;
	return 0;
}

/* Reports whether a TextDecoder throws on bytes that are not UTF-8 (fatal). */
static int
encoding_decoder_fatal(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct encoding_decoder *decoder;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The decoder. */
	status = encoding_decoder_of(realm, this_value, &decoder);
	if (status != 0)
		return status;

	/* Succeeded: the option. */
	*result = vm_value_boolean(decoder->fatal);
	return 0;
}

/* Reports whether a TextDecoder keeps a leading byte order mark (ignoreBOM). */
static int
encoding_decoder_ignore_bom(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct encoding_decoder *decoder;
	int status;

	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(count);

	/* The decoder. */
	status = encoding_decoder_of(realm, this_value, &decoder);
	if (status != 0)
		return status;

	/* Succeeded: the option. */
	*result = vm_value_boolean(decoder->ignore_bom);
	return 0;
}

/*
 * Decodes bytes as UTF-8 into a string (decode(input, {stream})); without
 * input only what a stream left waiting is finished.
 */
static int
encoding_decode(
	struct vm_realm *realm,
	vm_value this_value,
	const vm_value *args,
	unsigned count,
	vm_value *result)
{
	struct encoding_decoder *decoder;
	struct vm_string *text;
	struct wb_buffer bytes;
	struct wb_units out;
	vm_value option;
	int present;
	int stream;
	int status;

	/* The decoder and whether more bytes follow. */
	status = encoding_decoder_of(realm, this_value, &decoder);
	if (status != 0)
		return status;
	status = bind_get_option(realm, js_argument(args, count, 1), "stream", &present, &option);
	if (status != 0)
		return status;
	stream = 0;
	if (present)
		stream = vm_to_boolean(option);

	/* The bytes (none without input). */
	wb_buffer_init(&bytes);
	if (count > 0 && args[0] != VM_VALUE_UNDEFINED) {
		status = encoding_bytes(realm, args[0], &bytes);
		if (status != 0) {
			wb_buffer_release(&bytes);
			return status;
		}
	}

	/* The characters. */
	wb_units_init(&out);
	status = encoding_decode_bytes(realm, decoder, (const unsigned char *)bytes.data, bytes.length, stream, &out);
	wb_buffer_release(&bytes);
	if (status != 0) {
		wb_units_release(&out);
		return status;
	}

	/* As a string. */
	text = vm_string_from_units(realm->heap, out.data, out.length);
	wb_units_release(&out);
	if (text == NULL)
		return ENOMEM;

	/* Succeeded: the string. */
	*result = vm_value_cell(text);
	return 0;
}

/* Finds the state of the TextDecoder a value is, throwing a TypeError otherwise. */
static int
encoding_decoder_of(
	struct vm_realm *realm,
	vm_value value,
	struct encoding_decoder **decoder)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* A platform object. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One the binding made. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Whose cell is a decoder's. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A cell of the decoder's type. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &encoding_decoder_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: the state. */
	*decoder = (struct encoding_decoder *)cell;
	return 0;
}

/* Checks that a value is a TextEncoder, throwing a TypeError otherwise. */
static int
encoding_encoder_check(
	struct vm_realm *realm,
	vm_value value)
{
	struct vm_object *object;
	struct vm_cell *cell;
	int is_object;
	int is_cell;
	int status;

	/* A platform object. */
	is_object = vm_value_is_object(value);
	if (!is_object) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* One the binding made. */
	object = (struct vm_object *)vm_value_as_cell(value);
	if (object->kind != VM_KIND_PLATFORM) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Whose cell is an encoder's mark. */
	is_cell = vm_value_is_cell(object->internal);
	if (!is_cell) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* A cell of the encoder's type. */
	cell = vm_value_as_cell(object->internal);
	if (cell->type != &encoding_encoder_type) {
		status = bind_throw_illegal(realm);
		return status;
	}

	/* Succeeded: it is an encoder. */
	return 0;
}

/*
 * Tells whether a label names UTF-8: the label without its leading and
 * trailing ASCII whitespace, in lower case, is one of UTF-8's labels.
 */
static int
encoding_label_is_utf8(
	const struct vm_string *label)
{
	char folded[32];
	size_t start;
	size_t end;
	size_t index;
	size_t made;
	uint16_t unit;
	int space;
	int same;

	/* The label without the whitespace around it. */
	start = 0;
	end = label->length;
	while (start < end) {
		unit = vm_string_at(label, start);
		space = vm_is_space(unit);
		if (!space)
			break;
		start++;
	}
	while (end > start) {
		unit = vm_string_at(label, end - 1U);
		space = vm_is_space(unit);
		if (!space)
			break;
		end--;
	}

	/* A label longer than any of the table's is none of them. */
	if (end - start >= sizeof(folded))
		return 0;

	/* In lower case; a character past ASCII is in no label. */
	made = 0;
	for (index = start; index < end; index++) {
		unit = vm_string_at(label, index);
		if (unit > 0x7fU)
			return 0;
		if (unit >= 'A' && unit <= 'Z')
			unit = (uint16_t)(unit + 0x20U);
		folded[made] = (char)unit;
		made++;
	}

	/* The folded label ends. */
	folded[made] = '\0';

	/* Compares it with each of UTF-8's labels. */
	for (index = 0; encoding_utf8_labels[index] != NULL; index++) {
		same = strcmp(folded, encoding_utf8_labels[index]);
		if (same == 0)
			return 1;
	}

	/* Succeeded: it is not UTF-8's. */
	return 0;
}

/*
 * Reads the bytes of an array-like input: its length, then each element
 * as a number modulo 256.  Anything but an object throws a TypeError.
 */
static int
encoding_bytes(
	struct vm_realm *realm,
	vm_value input,
	struct wb_buffer *bytes)
{
	struct vm_string *length_atom;
	vm_value length_value;
	vm_value element;
	uint32_t length;
	uint32_t index;
	uint32_t byte;
	unsigned char value;
	int is_object;
	int status;

	/* Only an object holds bytes. */
	is_object = vm_value_is_object(input);
	if (!is_object) {
		status = vm_throw_type_error(realm, "Failed to execute 'decode' on 'TextDecoder': The provided value is not of type '(ArrayBuffer or ArrayBufferView)'.");
		return status;
	}

	/* How many. */
	length_atom = vm_atom_from_ascii(realm->heap, "length");
	if (length_atom == NULL)
		return ENOMEM;
	status = vm_get(realm, input, vm_value_cell(length_atom), &length_value);
	if (status != 0)
		return status;
	status = vm_to_uint32(realm, length_value, &length);
	if (status != 0)
		return status;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		status = vm_get(realm, input, vm_value_int32((int32_t)index), &element);
		if (status != 0)
			return status;
		status = vm_to_uint32(realm, element, &byte);
		if (status != 0)
			return status;

		/* Its low eight bits. */
		value = (unsigned char)(byte & 0xffU);
		status = wb_buffer_append(bytes, &value, 1);
		if (status != 0)
			return status;
	}

	/* Succeeded: the bytes. */
	return 0;
}

/*
 * Runs the Encoding Standard's UTF-8 decoder over bytes, appending the
 * characters; without stream the sequence under way ends with the bytes
 * (an unfinished one is an error) and the byte order mark is looked for
 * again at the next call.
 */
static int
encoding_decode_bytes(
	struct vm_realm *realm,
	struct encoding_decoder *decoder,
	const unsigned char *bytes,
	size_t length,
	int stream,
	struct wb_units *out)
{
	size_t index;
	int consumed;
	int error;
	int status;

	/* Each byte; one that ends a sequence as an error is read again as the start of the next. */
	index = 0;
	while (index < length) {
		status = encoding_step(decoder, bytes[index], out, &consumed, &error);
		if (status != 0)
			return status;
		if (consumed)
			index++;

		/* An error. */
		if (error) {
			status = encoding_error(realm, decoder, out);
			if (status != 0)
				return status;
		}
	}

	/* A stream waits for more bytes. */
	if (stream)
		return 0;

	/* Otherwise the stream ends here, and an unfinished sequence is an error. */
	error = 0;
	if (decoder->needed != 0)
		error = 1;
	encoding_reset(decoder);
	if (error) {
		status = encoding_error(realm, decoder, out);
		if (status != 0)
			return status;
	}

	/* Succeeded: the characters are appended, and the next call starts a new stream. */
	decoder->bom_seen = 0;
	return 0;
}

/*
 * Feeds one byte to the UTF-8 decoder: reports whether the byte was used
 * (a byte that cannot continue the sequence under way ends it and is read
 * again) and whether it made an error, and appends a finished code point.
 */
static int
encoding_step(
	struct encoding_decoder *decoder,
	unsigned byte,
	struct wb_units *out,
	int *consumed,
	int *error)
{
	int status;

	/* A byte that continues the sequence under way. */
	*consumed = 1;
	*error = 0;
	if (decoder->needed != 0) {
		/* One out of the range the sequence allows ends it as an error, and is not used. */
		if (byte < decoder->lower || byte > decoder->upper) {
			encoding_reset(decoder);
			*consumed = 0;
			*error = 1;
			return 0;
		}

		/* It adds six bits; the last one finishes the code point. */
		decoder->lower = 0x80U;
		decoder->upper = 0xbfU;
		decoder->code_point = (decoder->code_point << 6) | (byte & 0x3fU);
		decoder->seen++;
		if (decoder->seen < decoder->needed)
			return 0;

		/* The code point is finished. */
		status = encoding_emit(decoder, decoder->code_point, out);
		encoding_reset(decoder);
		return status;
	}

	/* The first byte of a sequence: ASCII is a code point of its own. */
	if (byte <= 0x7fU) {
		status = encoding_emit(decoder, byte, out);
		return status;
	}

	/* The lead byte of a sequence of two, three or four; the narrower ranges keep out overlong and surrogate forms. */
	if (byte >= 0xc2U && byte <= 0xdfU) {
		decoder->needed = 1;
		decoder->code_point = byte & 0x1fU;
	} else if (byte >= 0xe0U && byte <= 0xefU) {
		decoder->needed = 2;
		decoder->code_point = byte & 0x0fU;
		if (byte == 0xe0U)
			decoder->lower = 0xa0U;
		if (byte == 0xedU)
			decoder->upper = 0x9fU;
	} else if (byte >= 0xf0U && byte <= 0xf4U) {
		decoder->needed = 3;
		decoder->code_point = byte & 0x07U;
		if (byte == 0xf0U)
			decoder->lower = 0x90U;
		if (byte == 0xf4U)
			decoder->upper = 0x8fU;
	} else {
		/* A byte that starts nothing. */
		*error = 1;
	}

	/* Succeeded: the byte is used. */
	return 0;
}

/*
 * Handles a decoding error: a fatal decoder throws a TypeError (and its
 * stream starts anew), any other appends the replacement character.
 */
static int
encoding_error(
	struct vm_realm *realm,
	struct encoding_decoder *decoder,
	struct wb_units *out)
{
	int status;

	/* A fatal decoder throws. */
	if (decoder->fatal) {
		encoding_reset(decoder);
		decoder->bom_seen = 0;
		status = vm_throw_type_error(realm, "Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid.");
		return status;
	}

	/* Succeeded: the replacement character. */
	status = encoding_emit(decoder, ENCODING_REPLACEMENT, out);
	if (status != 0)
		return status;
	return 0;
}

/* Appends a decoded code point, dropping the stream's leading byte order mark unless ignoreBOM. */
static int
encoding_emit(
	struct encoding_decoder *decoder,
	uint32_t code_point,
	struct wb_units *out)
{
	int first;
	int status;

	/* The first code point of the stream is where the mark may be. */
	first = !decoder->bom_seen;
	decoder->bom_seen = 1;
	if (first && code_point == ENCODING_BOM && !decoder->ignore_bom)
		return 0;

	/* Succeeded: the code point. */
	status = wb_units_append_code_point(out, code_point);
	if (status != 0)
		return status;
	return 0;
}

/* Puts the decoder between sequences. */
static void
encoding_reset(
	struct encoding_decoder *decoder)
{
	/* Nothing under way, and any byte may continue the next sequence's first. */
	decoder->needed = 0;
	decoder->seen = 0;
	decoder->code_point = 0;
	decoder->lower = 0x80U;
	decoder->upper = 0xbfU;
}

/*
 * Reads the code point at a place of a string (a surrogate pair's, or
 * U+FFFD for a lone surrogate) and reports how many units it took.
 */
static size_t
encoding_next_code_point(
	const struct vm_string *string,
	size_t index,
	uint32_t *code_point)
{
	uint16_t unit;
	uint16_t next;

	/* A unit that is not a surrogate is itself. */
	unit = vm_string_at(string, index);
	if (unit < 0xd800U || unit > 0xdfffU) {
		*code_point = unit;
		return 1;
	}

	/* A trailing surrogate on its own, or a leading one at the end, is replaced. */
	*code_point = ENCODING_REPLACEMENT;
	if (unit > 0xdbffU || index + 1U >= string->length)
		return 1;

	/* A leading surrogate needs a trailing one after it. */
	next = vm_string_at(string, index + 1U);
	if (next < 0xdc00U || next > 0xdfffU)
		return 1;

	/* Succeeded: the pair's code point. */
	*code_point = 0x10000U + (((uint32_t)unit - 0xd800U) << 10) + ((uint32_t)next - 0xdc00U);
	return 2;
}
