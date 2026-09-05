/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the private net-to-networkd protocol codec.
 */

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int parse_decimal(const char *, size_t, uint32_t *);
static int send_all(int, const void *, size_t);
static int read_all(int, void *, size_t);
static uint16_t load_u16(const unsigned char *);
static uint32_t load_u32(const unsigned char *);
static void store_u16(unsigned char *, uint16_t);
static void store_u32(unsigned char *, uint32_t);

/* Encodes one canonical ZNV2 frame header. */
int
networkd_protocol_header_encode(
	char *output,
	size_t capacity,
	const struct networkd_protocol_header *header,
	size_t *length)
{
	int count;

	/* Validates the complete header before formatting it. */
	if (output == NULL || header == NULL || length == NULL ||
	    header->request_id == 0U || header->opcode == 0U ||
	    header->payload_length > NETWORKD_RESPONSE_MAX) {
		errno = EINVAL;
		return -1;
	}

	/* Formats the canonical outer frame header. */
	count = snprintf(output, capacity, "%s %lu %lu %lu\n",
	    NETWORKD_PROTOCOL_MAGIC, (unsigned long)header->request_id,
	    (unsigned long)header->opcode,
	    (unsigned long)header->payload_length);
	if (count < 0 || (size_t)count >= capacity ||
	    (size_t)count > NETWORKD_PROTOCOL_HEADER_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	*length = (size_t)count;

	/* Reports successful completion. */
	return 0;
}

/* Decodes one complete canonical ZNV2 frame header. */
int
networkd_protocol_header_decode(
	const char *input,
	size_t length,
	struct networkd_protocol_header *header)
{
	const char *field[4];
	size_t field_length[4];
	size_t start;
	size_t index;
	unsigned item;
	uint32_t value[3];

	/* Rejects an incomplete or oversized outer header. */
	if (input == NULL || header == NULL || length == 0U ||
	    length > NETWORKD_PROTOCOL_HEADER_MAX || input[length - 1U] != '\n') {
		errno = EINVAL;
		return -1;
	}

	/* Splits exactly four single-space-delimited fields. */
	start = 0U;
	item = 0U;
	for (index = 0U; index < length; index++) {
		if (input[index] != ' ' && input[index] != '\n')
			continue;
		if (item >= 4U || index == start) {
			errno = EINVAL;
			return -1;
		}
		field[item] = input + start;
		field_length[item] = index - start;
		item++;
		start = index + 1U;
	}
	if (item != 4U || start != length) {
		errno = EINVAL;
		return -1;
	}

	/* Validates the magic and canonical decimal fields. */
	if (field_length[0] != sizeof(NETWORKD_PROTOCOL_MAGIC) - 1U ||
	    memcmp(field[0], NETWORKD_PROTOCOL_MAGIC,
	    sizeof(NETWORKD_PROTOCOL_MAGIC) - 1U) != 0 ||
	    parse_decimal(field[1], field_length[1], &value[0]) != 0 ||
	    parse_decimal(field[2], field_length[2], &value[1]) != 0 ||
	    parse_decimal(field[3], field_length[3], &value[2]) != 0 ||
	    value[0] == 0U || value[1] == 0U ||
	    value[2] > NETWORKD_RESPONSE_MAX) {
		errno = EINVAL;
		return -1;
	}
	header->request_id = value[0];
	header->opcode = value[1];
	header->payload_length = (size_t)value[2];

	/* Reports successful completion. */
	return 0;
}

/* Initializes a bounded field writer. */
void
networkd_field_writer_init(
	struct networkd_field_writer *writer,
	void *bytes,
	size_t capacity)
{
	/* Initializes all writer state. */
	if (writer == NULL)
		return;
	writer->bytes = bytes;
	writer->capacity = bytes != NULL ? capacity : 0U;
	writer->used = 0U;
}

/* Appends one explicitly sized field. */
int
networkd_field_write(
	struct networkd_field_writer *writer,
	uint16_t type,
	const void *value,
	size_t length)
{
	size_t required;

	/* Validates the field and its complete destination extent. */
	if (writer == NULL || writer->bytes == NULL || type == 0U ||
	    length > UINT16_MAX || (length != 0U && value == NULL) ||
	    writer->used > writer->capacity) {
		errno = EINVAL;
		return -1;
	}
	required = 4U + length;
	if (required > writer->capacity - writer->used) {
		errno = ENOSPC;
		return -1;
	}

	/* Stores the fixed-width header followed by the field bytes. */
	store_u16(writer->bytes + writer->used, type);
	store_u16(writer->bytes + writer->used + 2U, (uint16_t)length);
	if (length != 0U)
		memcpy(writer->bytes + writer->used + 4U, value, length);
	writer->used += required;

	/* Reports successful completion. */
	return 0;
}

/* Appends one fixed-width unsigned field. */
int
networkd_field_write_u32(
	struct networkd_field_writer *writer,
	uint16_t type,
	uint32_t value)
{
	unsigned char bytes[4];

	/* Encodes the number independently of host byte order. */
	store_u32(bytes, value);

	/* Returns the field append result. */
	return networkd_field_write(writer, type, bytes, sizeof(bytes));
}

/* Initializes a bounded field reader. */
void
networkd_field_reader_init(
	struct networkd_field_reader *reader,
	const void *bytes,
	size_t length)
{
	/* Initializes all reader state. */
	if (reader == NULL)
		return;
	reader->bytes = bytes;
	reader->length = bytes != NULL ? length : 0U;
	reader->offset = 0U;
}

/* Reads the next field, returning one at end of input. */
int
networkd_field_read(
	struct networkd_field_reader *reader,
	struct networkd_field *field)
{
	size_t length;

	/* Validates the reader and recognizes clean end of input. */
	if (reader == NULL || field == NULL || reader->bytes == NULL ||
	    reader->offset > reader->length) {
		errno = EINVAL;
		return -1;
	}
	if (reader->offset == reader->length)
		return 1;

	/* Validates the fixed header and declared value extent. */
	if (reader->length - reader->offset < 4U) {
		errno = EINVAL;
		return -1;
	}
	field->type = load_u16(reader->bytes + reader->offset);
	length = load_u16(reader->bytes + reader->offset + 2U);
	if (field->type == 0U ||
	    length > reader->length - reader->offset - 4U) {
		errno = EINVAL;
		return -1;
	}
	field->value = reader->bytes + reader->offset + 4U;
	field->length = length;
	reader->offset += 4U + length;

	/* Reports one decoded field. */
	return 0;
}

/* Decodes one fixed-width unsigned field. */
int
networkd_field_read_u32(
	const struct networkd_field *field,
	uint32_t *value)
{
	/* Validates and decodes the exact fixed-width representation. */
	if (field == NULL || value == NULL || field->length != 4U) {
		errno = EINVAL;
		return -1;
	}
	*value = load_u32(field->value);

	/* Reports successful completion. */
	return 0;
}

/* Writes one complete ZNV2 frame. */
int
networkd_protocol_write_frame(
	int descriptor,
	const struct networkd_protocol_header *header,
	const void *payload)
{
	char outer[NETWORKD_PROTOCOL_HEADER_MAX + 1U];
	size_t outer_length;

	/* Validates and encodes the complete outer header. */
	if (header == NULL ||
	    (header->payload_length != 0U && payload == NULL) ||
	    networkd_protocol_header_encode(outer, sizeof(outer), header,
	    &outer_length) != 0)
		return -1;

	/* Writes the header and its exact declared payload. */
	if (send_all(descriptor, outer, outer_length) != 0 ||
	    (header->payload_length != 0U &&
	    send_all(descriptor, payload, header->payload_length) != 0))
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Reads one complete bounded ZNV2 frame. */
int
networkd_protocol_read_frame(
	int descriptor,
	struct networkd_protocol_header *header,
	void *payload,
	size_t capacity,
	size_t maximum)
{
	char outer[NETWORKD_PROTOCOL_HEADER_MAX];
	size_t outer_length;
	ssize_t count;

	/* Validates caller-owned storage before reading input. */
	if (header == NULL || maximum > capacity ||
	    (maximum != 0U && payload == NULL)) {
		errno = EINVAL;
		return -1;
	}

	/* Reads one independently bounded newline-terminated header. */
	outer_length = 0U;
	while (outer_length < sizeof(outer)) {
		count = read(descriptor, outer + outer_length, 1U);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EPIPE;
			return -1;
		}
		outer_length++;
		if (outer[outer_length - 1U] == '\n')
			break;
	}
	if (outer_length == sizeof(outer) && outer[outer_length - 1U] != '\n') {
		errno = E2BIG;
		return -1;
	}

	/* Decodes the header before accepting any payload bytes. */
	if (networkd_protocol_header_decode(outer, outer_length, header) != 0)
		return -1;
	if (header->payload_length > maximum) {
		errno = EMSGSIZE;
		return -1;
	}

	/* Reads exactly the declared payload extent. */
	if (header->payload_length != 0U &&
	    read_all(descriptor, payload, header->payload_length) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Clears protocol storage through a volatile byte view. */
void
networkd_protocol_clear(
	void *storage,
	size_t length)
{
	volatile unsigned char *byte;

	/* Clears every supplied byte without an elidable library call. */
	byte = storage;
	while (byte != NULL && length != 0U) {
		*byte++ = 0U;
		length--;
	}
}

/* Parses one nonzero-width canonical unsigned decimal. */
static int
parse_decimal(
	const char *text,
	size_t length,
	uint32_t *value)
{
	uint32_t parsed;
	size_t index;
	unsigned digit;

	/* Rejects empty, signed, and noncanonical leading-zero forms. */
	if (text == NULL || value == NULL || length == 0U ||
	    (length > 1U && text[0] == '0'))
		return -1;

	/* Accumulates with an explicit overflow check. */
	parsed = 0U;
	for (index = 0U; index < length; index++) {
		if (text[index] < '0' || text[index] > '9')
			return -1;
		digit = (unsigned)(text[index] - '0');
		if (parsed > (UINT32_MAX - digit) / 10U)
			return -1;
		parsed = parsed * 10U + digit;
	}
	*value = parsed;

	/* Reports successful completion. */
	return 0;
}

/* Sends one exact byte extent without delivering SIGPIPE. */
static int
send_all(
	int descriptor,
	const void *storage,
	size_t length)
{
	const unsigned char *bytes;
	ssize_t count;
	size_t offset;

	/* Sends every remaining byte. */
	bytes = storage;
	offset = 0U;
	while (offset < length) {
		count = send(descriptor, bytes + offset, length - offset,
		    MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EPIPE;
			return -1;
		}
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Reads one exact byte extent with interrupt retry. */
static int
read_all(
	int descriptor,
	void *storage,
	size_t length)
{
	unsigned char *bytes;
	ssize_t count;
	size_t offset;

	/* Reads every remaining byte. */
	bytes = storage;
	offset = 0U;
	while (offset < length) {
		count = read(descriptor, bytes + offset, length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EPIPE;
			return -1;
		}
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Loads one protocol-order 16-bit value. */
static uint16_t
load_u16(
	const unsigned char *bytes)
{
	uint16_t value;

	/* Combines the two protocol-order bytes. */
	value = (uint16_t)((uint16_t)bytes[0] << 8);
	value = (uint16_t)(value | bytes[1]);

	/* Returns the decoded value. */
	return value;
}

/* Loads one protocol-order 32-bit value. */
static uint32_t
load_u32(
	const unsigned char *bytes)
{
	uint32_t value;

	/* Combines the four protocol-order bytes. */
	value = (uint32_t)bytes[0] << 24;
	value |= (uint32_t)bytes[1] << 16;
	value |= (uint32_t)bytes[2] << 8;
	value |= bytes[3];

	/* Returns the decoded value. */
	return value;
}

/* Stores one protocol-order 16-bit value. */
static void
store_u16(
	unsigned char *bytes,
	uint16_t value)
{
	/* Stores the most significant byte first. */
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}

/* Stores one protocol-order 32-bit value. */
static void
store_u32(
	unsigned char *bytes,
	uint32_t value)
{
	/* Stores the most significant byte first. */
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}
