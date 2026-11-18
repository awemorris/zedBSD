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
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int parse_decimal(const char *, size_t, uint32_t *);
static int lan_record_mtu(const char *, const char *, unsigned *);
static int send_all(int, const void *, size_t);
static int read_all(int, void *, size_t, uint64_t);
static int wait_input(int, uint64_t);
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
	return networkd_protocol_read_frame_timed(descriptor, header, payload,
	    capacity, maximum, 0U);
}

/* Bounds the whole response, including trickled header and payload bytes. */
int
networkd_protocol_read_frame_timed(
	int descriptor,
	struct networkd_protocol_header *header,
	void *payload,
	size_t capacity,
	size_t maximum,
	unsigned seconds)
{
	char outer[NETWORKD_PROTOCOL_HEADER_MAX];
	struct timespec now;
	uint64_t deadline;
	size_t outer_length;

	/* Validates caller-owned storage before reading input. */
	if (header == NULL || maximum > capacity ||
	    (maximum != 0U && payload == NULL)) {
		errno = EINVAL;
		return -1;
	}

	/* Reads one independently bounded newline-terminated header. */
	deadline = 0U;
	if (seconds != 0U) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			return -1;
		deadline = ((uint64_t)now.tv_sec + seconds) * 1000000ULL +
		    (uint64_t)now.tv_nsec / 1000U;
	}
	outer_length = 0U;
	while (outer_length < sizeof(outer)) {
		if (read_all(descriptor, outer + outer_length, 1U, deadline) != 0)
			return -1;
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
	    read_all(descriptor, payload, header->payload_length, deadline) != 0)
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

/*
 * Writes one wired interface's LAN_ENABLE record.
 *
 * The record is not NUL-counted: its length goes to *length.  An MTU is
 * written for an interface the daemon configures, never for a disabled one.
 */
int
networkd_lan_record_format(
	const struct networkd_lan_record *record,
	char *output,
	size_t capacity,
	size_t *length)
{
	size_t used;
	int count;

	/* Rejects what cannot be written. */
	if (record == NULL || output == NULL || length == NULL || capacity == 0U) {
		errno = EINVAL;
		return -1;
	}

	/* Rejects an MTU outside the range a record may carry. */
	if (record->mtu != 0U &&
	    (record->mtu < NETWORKD_LAN_RECORD_MTU_MIN ||
	     record->mtu > NETWORKD_LAN_RECORD_MTU_MAX)) {
		errno = EINVAL;
		return -1;
	}

	/* Writes the interface and what is asked of it. */
	switch (record->mode) {
	case NETWORKD_LAN_RECORD_DISABLED:
		count = snprintf(output, capacity, "%s disabled", record->interface);
		break;
	case NETWORKD_LAN_RECORD_DHCP:
		count = snprintf(output, capacity, "%s dhcp %u", record->interface, record->dhcp_timeout);
		break;
	case NETWORKD_LAN_RECORD_STATIC:
		count = snprintf(output, capacity, "%s static %s %s", record->interface, record->address, record->netmask);
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	/* Rejects a record that did not fit. */
	if (count < 0 || (size_t)count >= capacity) {
		errno = EOVERFLOW;
		return -1;
	}

	/* Remembers how much of the output is written. */
	used = (size_t)count;

	/* Appends the MTU of an interface the daemon configures. */
	if (record->mtu != 0U && record->mode != NETWORKD_LAN_RECORD_DISABLED) {
		count = snprintf(output + used, capacity - used, " mtu %u", record->mtu);
		if (count < 0 || (size_t)count >= capacity - used) {
			errno = EOVERFLOW;
			return -1;
		}

		/* The MTU words are part of the record now. */
		used += (size_t)count;
	}

	/* Reports the record's length. */
	*length = used;

	/* Succeeded: the record is written. */
	return 0;
}

/*
 * Reads one wired interface's LAN_ENABLE record.
 *
 * Anything but the exact forms networkd_lan_record_format writes is
 * refused with EINVAL.
 */
int
networkd_lan_record_parse(
	const char *text,
	size_t length,
	struct networkd_lan_record *record)
{
	char copy[NETWORKD_LAN_RECORD_MAX];
	char *word[7];
	char *token;
	char *rest;
	unsigned words;
	uint32_t timeout;
	size_t word_length;
	void *nul;
	int compare;
	int compare_dhcp;
	int compare_static;
	int parsed;

	/* Rejects a record that cannot be held or carries a NUL. */
	if (text == NULL || record == NULL || length == 0U || length >= sizeof(copy)) {
		errno = EINVAL;
		return -1;
	}

	/* A NUL would end the record early. */
	nul = memchr(text, '\0', length);
	if (nul != NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Takes a copy the words can be cut out of. */
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Splits the record into its words, no more than the longest form has. */
	words = 0U;
	token = strtok_r(copy, " ", &rest);
	while (token != NULL) {
		if (words == sizeof(word) / sizeof(word[0])) {
			errno = EINVAL;
			return -1;
		}

		/* Keeps the word and goes on to the next. */
		word[words] = token;
		words++;
		token = strtok_r(NULL, " ", &rest);
	}

	/* Every record names an interface and what is asked of it. */
	if (words < 2U) {
		errno = EINVAL;
		return -1;
	}

	/* The name must fit the record. */
	word_length = strlen(word[0]);
	if (word_length >= sizeof(record->interface)) {
		errno = EINVAL;
		return -1;
	}

	/* Starts the record with the interface name. */
	memset(record, 0, sizeof(*record));
	strcpy(record->interface, word[0]);

	/* Reads the kind of record and its operands. */
	compare = strcmp(word[1], "disabled");
	compare_dhcp = strcmp(word[1], "dhcp");
	compare_static = strcmp(word[1], "static");
	if (compare == 0 && words == 2U) {
		/* Left alone by the daemon. */
		record->mode = NETWORKD_LAN_RECORD_DISABLED;
	} else if (compare_dhcp == 0 && (words == 3U || words == 5U)) {
		/* DHCP with its timeout, and an MTU. */
		word_length = strlen(word[2]);
		parsed = parse_decimal(word[2], word_length, &timeout);
		if (parsed != 0 || timeout == 0U || timeout > 3600U) {
			errno = EINVAL;
			return -1;
		}

		/* The mode and its timeout. */
		record->mode = NETWORKD_LAN_RECORD_DHCP;
		record->dhcp_timeout = (unsigned)timeout;
		if (words == 5U) {
			parsed = lan_record_mtu(word[3], word[4], &record->mtu);
			if (parsed != 0)
				return -1;
		}
	} else if (compare_static == 0 && (words == 4U || words == 6U)) {
		/* A static address and mask, and an MTU. */
		word_length = strlen(word[2]);
		if (word_length >= sizeof(record->address)) {
			errno = EINVAL;
			return -1;
		}

		/* The mask must fit too. */
		word_length = strlen(word[3]);
		if (word_length >= sizeof(record->netmask)) {
			errno = EINVAL;
			return -1;
		}

		/* The mode and its address and mask. */
		record->mode = NETWORKD_LAN_RECORD_STATIC;
		strcpy(record->address, word[2]);
		strcpy(record->netmask, word[3]);
		if (words == 6U) {
			parsed = lan_record_mtu(word[4], word[5], &record->mtu);
			if (parsed != 0)
				return -1;
		}
	} else {
		/* No other form. */
		errno = EINVAL;
		return -1;
	}

	/* Succeeded: the record is read. */
	return 0;
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
	size_t length,
	uint64_t deadline)
{
	unsigned char *bytes;
	ssize_t count;
	size_t offset;

	/* Reads every remaining byte. */
	bytes = storage;
	offset = 0U;
	while (offset < length) {
		if (deadline != 0U && wait_input(descriptor, deadline) != 0)
			return -1;
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

/* Waits without renewing the transaction deadline on partial data or signals. */
static int
wait_input(
	int descriptor,
	uint64_t deadline)
{
	struct timespec now;
	struct pollfd input;
	uint64_t current;
	uint64_t remaining;
	int result;

	for (;;) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
			return -1;
		current = (uint64_t)now.tv_sec * 1000000ULL +
		    (uint64_t)now.tv_nsec / 1000U;
		if (current >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		remaining = (deadline - current + 999U) / 1000U;
		input.fd = descriptor;
		input.events = POLLIN;
		input.revents = 0;
		result = poll(&input, 1U,
		    remaining > INT_MAX ? INT_MAX : (int)remaining);
		if (result > 0)
			return 0;
		if (result < 0 && errno != EINTR)
			return -1;
	}
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

/* Reads a record's "mtu N" suffix, N within the bounds any device could take. */
static int
lan_record_mtu(
	const char *key,
	const char *text,
	unsigned *mtu)
{
	uint32_t value;
	size_t text_length;
	int compare;
	int parsed;

	/* The key is "mtu". */
	compare = strcmp(key, "mtu");
	if (compare != 0) {
		errno = EINVAL;
		return -1;
	}

	/* The value is a decimal within the bounds. */
	text_length = strlen(text);
	parsed = parse_decimal(text, text_length, &value);
	if (parsed != 0 ||
	    value < NETWORKD_LAN_RECORD_MTU_MIN ||
	    value > NETWORKD_LAN_RECORD_MTU_MAX) {
		errno = EINVAL;
		return -1;
	}

	/* Succeeded: the MTU is read. */
	*mtu = (unsigned)value;
	return 0;
}
