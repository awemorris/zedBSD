/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/resolver-internal.h"

#include <netdb.h>
#include <string.h>

static uint16_t
read16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t
read32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3];
}

static void
write16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value >> 8);
	p[1] = (uint8_t)value;
}

static int
encode_name(uint8_t *message, size_t capacity, size_t *offset,
	const char *name)
{
	const char *label = name;
	size_t total = strlen(name), length;

	if (total == 0 || total > 253U)
		return EAI_NONAME;
	while (*label != '\0') {
		const char *dot = strchr(label, '.');
		length = dot != NULL ? (size_t)(dot - label) : strlen(label);
		if (length == 0) {
			if (dot != NULL && dot[1] == '\0') break;
			return EAI_NONAME;
		}
		if (length > 63U || *offset + 1U + length >= capacity)
			return EAI_OVERFLOW;
		message[(*offset)++] = (uint8_t)length;
		memcpy(message + *offset, label, length);
		*offset += length;
		if (dot == NULL) break;
		label = dot + 1;
	}
	if (*offset >= capacity) return EAI_OVERFLOW;
	message[(*offset)++] = 0;
	return 0;
}

static int
decode_name(const uint8_t *message, size_t length, size_t *offset,
	char *output, size_t capacity)
{
	size_t cursor = *offset, out = 0, next = cursor;
	unsigned depth = 0;
	int jumped = 0;

	while (1) {
		uint8_t size;
		if (cursor >= length || depth++ >= 16U) return EAI_FAIL;
		size = message[cursor++];
		if ((size & 0xc0U) == 0xc0U) {
			uint16_t pointer;
			if (cursor >= length) return EAI_FAIL;
			pointer = (uint16_t)((size & 0x3fU) << 8) | message[cursor++];
			if (pointer >= length || pointer == cursor - 2U) return EAI_FAIL;
			if (!jumped) next = cursor;
			cursor = pointer;
			jumped = 1;
			continue;
		}
		if ((size & 0xc0U) != 0 || size > 63U || cursor + size > length)
			return EAI_FAIL;
		if (size == 0) {
			if (!jumped) next = cursor;
			break;
		}
		if (out != 0) {
			if (out + 1U >= capacity) return EAI_OVERFLOW;
			output[out++] = '.';
		}
		if (out + size >= capacity) return EAI_OVERFLOW;
		memcpy(output + out, message + cursor, size);
		out += size;
		cursor += size;
		if (!jumped) next = cursor;
	}
	output[out] = '\0';
	*offset = next;
	return 0;
}

int
resolver_dns_build_query(uint8_t *message, size_t capacity, uint16_t id,
	const char *name, uint16_t type, size_t *length)
{
	size_t offset = 12;
	int error;

	if (message == NULL || name == NULL || length == NULL || capacity < 18U)
		return EAI_FAIL;
	memset(message, 0, capacity);
	write16(message, id);
	write16(message + 2, 0x0100U);
	write16(message + 4, 1U);
	error = encode_name(message, capacity, &offset, name);
	if (error != 0) return error;
	if (offset + 4U > capacity) return EAI_OVERFLOW;
	write16(message + offset, type);
	write16(message + offset + 2, 1U);
	*length = offset + 4U;
	return 0;
}

static int
rcode_error(unsigned rcode)
{
	if (rcode == 3U) return EAI_NONAME;
	if (rcode == 2U) return EAI_AGAIN;
	return rcode == 0U ? 0 : EAI_FAIL;
}

int
resolver_dns_parse(const uint8_t *message, size_t length, uint16_t id,
	const char *question, uint16_t qtype, struct resolver_result *result,
	int *truncated)
{
	uint16_t flags, qdcount, ancount;
	size_t offset = 12;
	char name[254];
	unsigned index;
	int error;

	if (message == NULL || result == NULL || length < 12U) return EAI_FAIL;
	if (read16(message) != id) return EAI_AGAIN;
	flags = read16(message + 2);
	if ((flags & 0x8000U) == 0 || (flags & 0x7800U) != 0) return EAI_FAIL;
	if (truncated != NULL) *truncated = (flags & 0x0200U) != 0;
	error = rcode_error(flags & 15U);
	if (error != 0) return error;
	qdcount = read16(message + 4);
	ancount = read16(message + 6);
	if (qdcount != 1U) return EAI_FAIL;
	error = decode_name(message, length, &offset, name, sizeof(name));
	if (error != 0 || offset + 4U > length ||
	    read16(message + offset) != qtype || read16(message + offset + 2) != 1U)
		return EAI_FAIL;
	if (question != NULL && strcmp(name, question) != 0) return EAI_FAIL;
	offset += 4U;
	for (index = 0; index < ancount; index++) {
		uint16_t type, class_, rdlength;
		uint32_t ttl;
		size_t rdata;
		error = decode_name(message, length, &offset, name, sizeof(name));
		if (error != 0 || offset + 10U > length) return EAI_FAIL;
		type = read16(message + offset);
		class_ = read16(message + offset + 2);
		ttl = read32(message + offset + 4);
		rdlength = read16(message + offset + 8);
		offset += 10U;
		rdata = offset;
		if (offset + rdlength > length) return EAI_FAIL;
		if (class_ == 1U && type == DNS_TYPE_A && rdlength == 4U &&
		    result->address_count < DNS_MAX_ADDRESSES) {
			memcpy(&result->addresses[result->address_count++].s_addr,
			    message + offset, 4U);
			if (result->ttl == 0 || ttl < result->ttl) result->ttl = ttl;
		} else if (class_ == 1U &&
		    (type == DNS_TYPE_CNAME || type == DNS_TYPE_PTR)) {
			char decoded[254];
			error = decode_name(message, length, &rdata, decoded,
			    sizeof(decoded));
			if (error != 0 || rdata > offset + rdlength) return EAI_FAIL;
			if (type == DNS_TYPE_PTR) {
				memcpy(result->ptr_name, decoded, strlen(decoded) + 1U);
			} else {
				memcpy(result->canonical, decoded, strlen(decoded) + 1U);
				if (result->cname_count < 8U)
					memcpy(result->cname_chain[result->cname_count++],
					    decoded, strlen(decoded) + 1U);
			}
			if (result->ttl == 0 || ttl < result->ttl) result->ttl = ttl;
		}
		offset += rdlength;
	}
	if (qtype == DNS_TYPE_A && result->address_count == 0U)
		return EAI_NONAME;
	if (qtype == DNS_TYPE_PTR && result->ptr_name[0] == '\0')
		return EAI_NONAME;
	return 0;
}
