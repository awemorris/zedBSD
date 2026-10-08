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

#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

static void write16(uint8_t *p, uint16_t value);
static int encode_name(uint8_t *message, size_t capacity, size_t *offset, const char *name);
static uint16_t read16(const uint8_t *p);
static int rcode_error(unsigned rcode);
static int decode_name(const uint8_t *message, size_t length, size_t *offset, char *output, size_t capacity);
static uint32_t read32(const uint8_t *p);
static int hosts_same_name(const char *one, const char *other);

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
			   type == DNS_TYPE_AAAA &&
			   rdlength == 16U &&
			   result->address6_count < DNS_MAX_ADDRESSES) {
			/* An IPv6 address (ws130-p004). */
			memcpy(result->addresses6[result->address6_count++].s6_addr, message + offset, 16U);

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
				if (result->cname_count < 8U) {
					memcpy(result->cname_chain
						   [result->cname_count++],
					       decoded, strlen(decoded) + 1U);
				}
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

	/* An IPv6 question without an IPv6 address (ws130-p004). */
	if (qtype == DNS_TYPE_AAAA && result->address6_count == 0U)
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

/* ------------------------------------------------------------------ *
 * IPv6 for the resolver (ws130-p004): an address's PTR name and the order
 * of IPv6 and IPv4 destinations.  Pure: the host tests build this file
 * alone.  The text of an address is inet_pton's and inet_ntop's (socket.c).
 * ------------------------------------------------------------------ */

/* How long an IPv6 address's PTR name is, with its NUL. */
#define INET6_PTR_LENGTH	73U

/*
 * Writes the reverse lookup's name of an IPv6 address
 * ("b.a.9.8. ... .ip6.arpa"): 0, or EAI_OVERFLOW.
 */
int
resolver_inet6_ptr_name(
	const uint8_t *address,
	char *output,
	size_t capacity)
{
	static const char digits[] = "0123456789abcdef";
	unsigned index;
	size_t at;

	/* Room for 32 nibbles with their dots and the zone. */
	if (capacity < INET6_PTR_LENGTH)
		return EAI_OVERFLOW;

	/* The nibbles, the last byte's low one first. */
	at = 0;
	for (index = 16U; index > 0; index--) {
		output[at++] = digits[address[index - 1U] & 0x0fU];
		output[at++] = '.';
		output[at++] = digits[address[index - 1U] >> 4];
		output[at++] = '.';
	}

	/* Succeeded: the zone. */
	memcpy(output + at, "ip6.arpa", sizeof("ip6.arpa"));
	return 0;
}

/*
 * Tells whether an IPv6 destination comes before the IPv4 ones, by the
 * source the host would send to it from (RFC 6724, as far as one source
 * tells): one that reaches it -- the loopback for the loopback, not the
 * loopback otherwise, and not a link-local one for a destination beyond
 * the link.  0 puts IPv6 after IPv4 (a network with IPv6 on the link but
 * no way out does not stall the application).
 */
int
resolver_inet6_preferred(
	const uint8_t *destination,
	const uint8_t *source)
{
	uint8_t loopback[16];
	uint8_t none[16];
	int destination_loopback;
	int source_loopback;
	int destination_link;
	int source_link;

	/* The two addresses compared with. */
	memset(none, 0, sizeof(none));
	memset(loopback, 0, sizeof(loopback));
	loopback[15] = 1;

	/* No source: no way there. */
	if (memcmp(source, none, sizeof(none)) == 0)
		return 0;

	/* The loopback goes with the loopback only. */
	destination_loopback = memcmp(destination, loopback, sizeof(loopback)) == 0;
	source_loopback = memcmp(source, loopback, sizeof(loopback)) == 0;
	if (destination_loopback != source_loopback)
		return 0;

	/* A link-local source reaches only the link. */
	destination_link = destination[0] == 0xfe && (destination[1] & 0xc0) == 0x80;
	source_link = source[0] == 0xfe && (source[1] & 0xc0) == 0x80;
	if (source_link && !destination_link)
		return 0;

	/* Succeeded: the source reaches it. */
	return 1;
}

/*
 * Tells whether an interface's IPv4 address (in host order) counts for
 * AI_ADDRCONFIG (ws177-p044): any but the unspecified one and loopback's.
 */
int
resolver_usable4(
	uint32_t address)
{
	/* The unspecified address: none configured. */
	if (address == 0U)
		return 0;

	/* 127.0.0.0/8, the loopback. */
	if ((address >> 24) == 127U)
		return 0;

	/* Counts. */
	return 1;
}

/*
 * Tells whether an interface's IPv6 address counts for AI_ADDRCONFIG
 * (ws177-p044, RFC 3493 with the common practice): not the unspecified
 * one, not ::1, not a link-local one, and settled (duplicate address
 * detection over and passed).
 */
int
resolver_usable6(
	const uint8_t *address,
	int settled)
{
	static const uint8_t loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
	static const uint8_t unspecified[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	int same;

	/* Not yet settled, or found to be another's. */
	if (!settled)
		return 0;

	/* The unspecified address and the loopback. */
	same = memcmp(address, unspecified, sizeof(unspecified));
	if (same == 0)
		return 0;
	same = memcmp(address, loopback, sizeof(loopback));
	if (same == 0)
		return 0;

	/* A link-local address reaches no name server off the link. */
	if (address[0] == 0xfeU && (address[1] & 0xc0U) == 0x80U)
		return 0;

	/* Counts. */
	return 1;
}

/*
 * Cuts a host name to its first label (NI_NOFQDN, ws177-p044): the part
 * before the first '.'; a name without one stays.
 */
void
resolver_short_name(
	char *name)
{
	char *dot;

	/* The first dot ends it (a dot first leaves the name as it is). */
	dot = strchr(name, '.');
	if (dot != NULL && dot != name)
		*dot = '\0';
}

/*
 * Reads a line of /etc/hosts ("ADDRESS NAME [ALIAS...]", '#' to the end
 * of the line a comment; the line is cut up in place) and tells whether it
 * names the host, ignoring case.  Returns 1 with the address and the
 * canonical name, or 0.  An IPv6 address with a zone is not taken.
 */
int
resolver_hosts_line(
	char *line,
	const char *name,
	struct resolver_hosts_entry *entry)
{
	char *words[2];
	char *cursor;
	char *word;
	char *comment;
	int found;
	int ok;

	/* The line without its comment, and its address. */
	comment = strchr(line, '#');
	if (comment != NULL)
		*comment = '\0';
	cursor = line;
	found = 0;
	words[0] = NULL;
	words[1] = NULL;
	for (;;) {
		/* The next word. */
		while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
			cursor++;
		if (*cursor == '\0')
			break;
		word = cursor;
		while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n')
			cursor++;
		if (*cursor != '\0')
			*cursor++ = '\0';

		/* The address, the canonical name, then the aliases; the host among the names. */
		if (words[0] == NULL) {
			words[0] = word;
			continue;
		}
		if (words[1] == NULL)
			words[1] = word;
		if (hosts_same_name(word, name))
			found = 1;
	}
	if (!found)
		return 0;

	/* The address of either family. */
	memset(entry, 0, sizeof(*entry));
	ok = inet_pton(AF_INET, words[0], entry->address);
	entry->family = AF_INET;
	if (ok != 1) {
		ok = inet_pton(AF_INET6, words[0], entry->address);
		entry->family = AF_INET6;
	}
	if (ok != 1)
		return 0;

	/* Succeeded: the canonical name, cut to fit. */
	strncpy(entry->canonical, words[1], sizeof(entry->canonical) - 1U);
	return 1;
}

/* Tells whether two host names are the same, ignoring the case of ASCII letters. */
static int
hosts_same_name(
	const char *one,
	const char *other)
{
	unsigned char left;
	unsigned char right;

	/* Each character, folded to lower case. */
	for (;;) {
		left = (unsigned char)*one++;
		right = (unsigned char)*other++;
		if (left >= 'A' && left <= 'Z')
			left = (unsigned char)(left - 'A' + 'a');
		if (right >= 'A' && right <= 'Z')
			right = (unsigned char)(right - 'A' + 'a');
		if (left != right)
			return 0;
		if (left == '\0')
			return 1;
	}
}
