/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A small DER writer for the unit tests, written from ITU-T X.690 (02/2021)
 * 8.1 (identifier, definite length in the minimum octets), 8.3 (INTEGER),
 * 8.19 (OBJECT IDENTIFIER) without the library's reader: tests build
 * well-formed and deliberately malformed inputs with it (wrong lengths,
 * long-form lengths, indefinite length, high tags) to drive cryc-der,
 * cryc-x509 and the signature decoders.
 *
 * A writer appends to a caller's buffer; der_wrap puts one writer's bytes
 * inside a tag in another, so nesting is built inside out.  error is set
 * when the room is exhausted; every later call does nothing.
 */

#ifndef WS335_TESTS_DERWRITER_H
#define WS335_TESTS_DERWRITER_H

#include <stdlib.h>
#include <string.h>

/* A buffer being written. */
struct der_writer {
	unsigned char *data;
	size_t room;
	size_t used;
	int error;
};

/* Starts a writer over room bytes. */
static void __attribute__((unused))
der_init(
	struct der_writer *writer,
	unsigned char *data,
	size_t room)
{
	/* Empty, no error. */
	writer->data = data;
	writer->room = room;
	writer->used = 0;
	writer->error = 0;
}

/* Appends raw bytes (also used to write malformed encodings on purpose). */
static void __attribute__((unused))
der_raw(
	struct der_writer *writer,
	const unsigned char *bytes,
	size_t size)
{
	/* All or nothing. */
	if (writer->error || size > writer->room - writer->used) {
		writer->error = 1;
		return;
	}
	if (size > 0U)
		memcpy(writer->data + writer->used, bytes, size);
	writer->used += size;
}

/* Appends a length in the minimum octets (8.1.3.4 short form below 128, 8.1.3.5 long form otherwise). */
static void __attribute__((unused))
der_length(
	struct der_writer *writer,
	size_t length)
{
	unsigned char bytes[9];
	size_t count;
	size_t value;

	/* Short form. */
	if (length < 128U) {
		bytes[0] = (unsigned char)length;
		der_raw(writer, bytes, 1);
		return;
	}

	/* Long form: the count of octets, then the octets big-endian. */
	count = 0;
	for (value = length; value > 0U; value >>= 8)
		count++;
	bytes[0] = (unsigned char)(0x80U | count);
	for (value = 0; value < count; value++)
		bytes[1U + value] = (unsigned char)(length >> (8U * (count - 1U - value)));
	der_raw(writer, bytes, 1U + count);
}

/* Appends one element: a one-octet tag, the length, the contents. */
static void __attribute__((unused))
der_put(
	struct der_writer *writer,
	unsigned tag,
	const unsigned char *contents,
	size_t size)
{
	unsigned char octet;

	/* Identifier, length, contents. */
	octet = (unsigned char)tag;
	der_raw(writer, &octet, 1);
	der_length(writer, size);
	der_raw(writer, contents, size);
}

/* Appends the bytes of inner as the contents of one element with tag (a SEQUENCE, a SET, an explicit tag). */
static void __attribute__((unused))
der_wrap(
	struct der_writer *writer,
	unsigned tag,
	const struct der_writer *inner)
{
	/* An inner error spoils the outer writer. */
	if (inner->error) {
		writer->error = 1;
		return;
	}
	der_put(writer, tag, inner->data, inner->used);
}

/* Appends a non-negative INTEGER from big-endian magnitude bytes (8.3: a 0x00 is added when the top bit is set). */
static void __attribute__((unused))
der_unsigned(
	struct der_writer *writer,
	const unsigned char *magnitude,
	size_t size)
{
	unsigned char buffer[1100];

	/* Leading zero bytes removed (keeping one for zero). */
	while (size > 1U && magnitude[0] == 0U) {
		magnitude++;
		size--;
	}
	if (size + 1U > sizeof(buffer)) {
		writer->error = 1;
		return;
	}

	/* The sign octet when needed. */
	if (size > 0U && (magnitude[0] & 0x80U) != 0U) {
		buffer[0] = 0;
		memcpy(buffer + 1, magnitude, size);
		der_put(writer, 0x02U, buffer, size + 1U);
		return;
	}
	der_put(writer, 0x02U, magnitude, size);
}

/* Appends an OBJECT IDENTIFIER written as dotted text, "1.2.840.10045.2.1" (8.19). */
static void __attribute__((unused))
der_oid(
	struct der_writer *writer,
	const char *text)
{
	unsigned long arcs[32];
	unsigned char contents[160];
	unsigned char groups[8];
	size_t count;
	size_t used;
	size_t index;
	size_t group;
	unsigned long value;
	char *end;

	/* The arcs. */
	count = 0;
	while (*text != '\0' && count < 32U) {
		arcs[count++] = strtoul(text, &end, 10);
		text = *end == '.' ? end + 1 : end;
	}
	if (count < 2U) {
		writer->error = 1;
		return;
	}

	/* The first two arcs in one subidentifier, then each in base 128 (8.19.4, 8.19.2). */
	used = 0;
	for (index = 1; index < count; index++) {
		value = index == 1U ? arcs[0] * 40UL + arcs[1] : arcs[index];
		group = 0;
		do {
			groups[group++] = (unsigned char)(value & 0x7fUL);
			value >>= 7;
		} while (value > 0UL);
		while (group > 0U && used < sizeof(contents)) {
			group--;
			contents[used++] = (unsigned char)(groups[group] | (group > 0U ? 0x80U : 0U));
		}
	}
	der_put(writer, 0x06U, contents, used);
}

#endif
