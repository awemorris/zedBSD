/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library resolver dns support.
 */

#include "userland/base/libc/resolver-internal.h"

#include <netdb.h>
#include <string.h>

static void write16(uint8_t *p, uint16_t value);
static int encode_name(uint8_t *message, size_t capacity, size_t *offset, const char *name);
static uint16_t read16(const uint8_t *p);
static int rcode_error(unsigned rcode);
static int decode_name(const uint8_t *message, size_t length, size_t *offset, char *output, size_t capacity);
static uint32_t read32(const uint8_t *p);

/*
 * Implements the resolver dns build query operation.
 */
int
resolver_dns_build_query(
	uint8_t *message,
	size_t capacity,
	uint16_t id,
	const char *name,
	uint16_t type,
	size_t *length)
{
	size_t offset;
	int error;

	offset = 12;

	/* Handles the message availability. */
	if (message == NULL || name == NULL || length == NULL || capacity < 18U)
		return EAI_FAIL;
	memset(message, 0, capacity);
	write16(message, id);
	write16(message + 2, 0x0100U);
	write16(message + 4, 1U);
	error = encode_name(message, capacity, &offset, name);

	/* Handles an operation failure. */
	if (error != 0)
		return error;

	/* Checks the current offset. */
	if (offset + 4U > capacity)
		return EAI_OVERFLOW;
	write16(message + offset, type);
	write16(message + offset + 2, 1U);
	*length = offset + 4U;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the resolver dns parse operation.
 */
int
resolver_dns_parse(
	const uint8_t *message,
	size_t length,
	uint16_t id,
	const char *question,
	uint16_t qtype,
	struct resolver_result *result,
	int *truncated)
{
	char decoded[254];
	uint16_t type, class_, rdlength;
	uint32_t ttl;
	size_t rdata;
	uint16_t flags, qdcount, ancount;
	size_t offset;
	char name[254];
	unsigned index;
	int error;

	offset = 12;

	/* Handles the message availability. */
	if (message == NULL || result == NULL || length < 12U)
		return EAI_FAIL;

	/* Handles a failed read16 operation. */
	if (read16(message) != id)
		return EAI_AGAIN;
	flags = read16(message + 2);

	/* Checks the active flags. */
	if ((flags & 0x8000U) == 0 || (flags & 0x7800U) != 0)
		return EAI_FAIL;

	/* Handles the truncated availability. */
	if (truncated != NULL)
		*truncated = (flags & 0x0200U) != 0;
	error = rcode_error(flags & 15U);

	/* Handles an operation failure. */
	if (error != 0)
		return error;
	qdcount = read16(message + 4);
	ancount = read16(message + 6);

	/* Handles the qdcount condition. */
	if (qdcount != 1U)
		return EAI_FAIL;
	error = decode_name(message, length, &offset, name, sizeof(name));

	/* Handles an operation failure. */
	if (error != 0 || offset + 4U > length ||
	    read16(message + offset) != qtype ||
	    read16(message + offset + 2) != 1U)

		/* Returns the computed result. */
		return EAI_FAIL;

	/* Handles the question availability. */
	if (question != NULL && strcmp(name, question) != 0)
		return EAI_FAIL;
	offset += 4U;

	/* Process each remaining element. */
	for (index = 0; index < ancount; index++) {

		error =
		    decode_name(message, length, &offset, name, sizeof(name));

		/* Handles an operation failure. */
		if (error != 0 || offset + 10U > length)
			return EAI_FAIL;
		type = read16(message + offset);
		class_ = read16(message + offset + 2);
		ttl = read32(message + offset + 4);
		rdlength = read16(message + offset + 8);
		offset += 10U;
		rdata = offset;

		/* Checks the current offset. */
		if (offset + rdlength > length)
			return EAI_FAIL;

		/* Handles the class condition. */
		if (class_ == 1U && type == DNS_TYPE_A && rdlength == 4U &&
		    result->address_count < DNS_MAX_ADDRESSES) {
			memcpy(
			    &result->addresses[result->address_count++].s_addr,
			    message + offset, 4U);

			/* Checks the operation result. */
			if (result->ttl == 0 || ttl < result->ttl)
				result->ttl = ttl;
		} else if (class_ == 1U &&
			   (type == DNS_TYPE_CNAME || type == DNS_TYPE_PTR)) {

			error = decode_name(message, length, &rdata, decoded,
					    sizeof(decoded));

			/* Handles an operation failure. */
			if (error != 0 || rdata > offset + rdlength)
				return EAI_FAIL;

			/* Handles the type condition. */
			if (type == DNS_TYPE_PTR) {
				memcpy(result->ptr_name, decoded,
				       strlen(decoded) + 1U);
			} else {
				memcpy(result->canonical, decoded,
				       strlen(decoded) + 1U);

				/* Checks the operation result. */
				if (result->cname_count < 8U)
					memcpy(result->cname_chain
						   [result->cname_count++],
					       decoded, strlen(decoded) + 1U);
			}

			/* Checks the operation result. */
			if (result->ttl == 0 || ttl < result->ttl)
				result->ttl = ttl;
		}
		offset += rdlength;
	}

	/* Handles the qtype condition. */
	if (qtype == DNS_TYPE_A && result->address_count == 0U)
		return EAI_NONAME;

	/* Handles the qtype condition. */
	if (qtype == DNS_TYPE_PTR && result->ptr_name[0] == '\0')
		return EAI_NONAME;

	/* Reports successful completion. */
	return 0;
}

/* Supports the write16 operation. */
static void
write16(
	uint8_t *p,
	uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

/* Supports the encode name operation. */
static int
encode_name(
	uint8_t *message,
	size_t capacity,
	size_t *offset,
	const char *name)
{
	const char *dot;
	const char *label;
	size_t total, length;

	label = name;
	total = strlen(name);

	/* Handles the total condition. */
	if (total == 0 || total > 253U)
		return EAI_NONAME;

	/* Continue while the operation condition remains true. */
	while (*label != '\0') {

		dot = strchr(label, '.');
		length = dot != NULL ? (size_t)(dot - label) : strlen(label);

		/* Checks the current data length. */
		if (length == 0) {
			/* Handles the dot availability. */
			if (dot != NULL && dot[1] == '\0')
				break;

			/* Returns the computed result. */
			return EAI_NONAME;
		}

		/* Checks the current data length. */
		if (length > 63U || *offset + 1U + length >= capacity)
			return EAI_OVERFLOW;
		message[(*offset)++] = (uint8_t)length;
		memcpy(message + *offset, label, length);
		*offset += length;
		/* Handles the dot availability. */
		if (dot == NULL)
			break;
		label = dot + 1;
	}

	/* Checks the current offset. */
	if (*offset >= capacity)
		return EAI_OVERFLOW;
	message[(*offset)++] = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the read16 operation. */
static uint16_t
read16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Supports the rcode error operation. */
static int
rcode_error(
	unsigned rcode)
{
	/* Handles the rcode condition. */
	if (rcode == 3U)
		return EAI_NONAME;

	/* Handles the rcode condition. */
	if (rcode == 2U)
		return EAI_AGAIN;

	/* Returns the computed result. */
	return rcode == 0U ? 0 : EAI_FAIL;
}

/* Supports the decode name operation. */
static int
decode_name(
	const uint8_t *message,
	size_t length,
	size_t *offset,
	char *output,
	size_t capacity)
{
	uint16_t pointer;
	uint8_t size;
	size_t cursor, out, next;
	unsigned depth;
	int jumped;

	cursor = *offset;
	out = 0;
	next = cursor;
	depth = 0;
	jumped = 0;

	/* Continue until the operation reaches a terminal state. */
	while (1) {
		/* Checks the current cursor position. */
		if (cursor >= length || depth++ >= 16U)
			return EAI_FAIL;
		size = message[cursor++];

		/* Checks the current data size. */
		if ((size & 0xc0U) == 0xc0U) {
			/* Checks the current cursor position. */
			if (cursor >= length)
				return EAI_FAIL;
			pointer =
			    (uint16_t)((size & 0x3fU) << 8) | message[cursor++];

			/* Handles the pointer condition. */
			if (pointer >= length || pointer == cursor - 2U)
				return EAI_FAIL;

			/* Handles the jumped condition. */
			if (!jumped)
				next = cursor;
			cursor = pointer;
			jumped = 1;
			continue;
		}

		/* Checks the current data size. */
		if ((size & 0xc0U) != 0 || size > 63U || cursor + size > length)
			return EAI_FAIL;

		/* Checks the current data size. */
		if (size == 0) {
			/* Handles the jumped condition. */
			if (!jumped)
				next = cursor;
			break;
		}

		/* Handles the out condition. */
		if (out != 0) {
			/* Handles the out condition. */
			if (out + 1U >= capacity)
				return EAI_OVERFLOW;
			output[out++] = '.';
		}

		/* Handles the out condition. */
		if (out + size >= capacity)
			return EAI_OVERFLOW;
		memcpy(output + out, message + cursor, size);
		out += size;
		cursor += size;

		/* Handles the jumped condition. */
		if (!jumped)
			next = cursor;
	}
	output[out] = '\0';
	*offset = next;
	/* Reports successful completion. */
	return 0;
}

/* Supports the read32 operation. */
static uint32_t
read32(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}
