/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An encoder of the RFC 4251 section 5 types, written apart from
 * ssh-buf.c, for the unit tests: tests build their inputs (messages,
 * blobs, malformed variants) with it, so that a reader is not tested
 * only with bytes its own writer made.  A fixed array, no allocation; a
 * write past the end sets overflow and writes nothing more.
 */

#ifndef SSH_TEST_WIRE_H
#define SSH_TEST_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Keeps a helper a test does not call from being warned about. */
#if defined(__GNUC__)
#define WIRE_UNUSED __attribute__((unused))
#else
#define WIRE_UNUSED
#endif

/* The room of one wire buffer: enough for a KEXINIT, a hybrid reply, a key file. */
#define WIRE_MAX	65536U

/* One message being built: size bytes used of data; overflow 1 after a write that did not fit. */
struct wire {
	uint8_t data[WIRE_MAX];
	size_t size;
	int overflow;
};

/* Empties the buffer. */
WIRE_UNUSED static void
wire_reset(
	struct wire *wire)
{
	/* Starts with nothing written. */
	wire->size = 0;
	wire->overflow = 0;
}

/* Appends size raw bytes. */
WIRE_UNUSED static void
wire_bytes(
	struct wire *wire,
	const void *bytes,
	size_t size)
{
	/* Refuses a write that does not fit. */
	if (wire->overflow || size > WIRE_MAX - wire->size) {
		wire->overflow = 1;
		return;
	}

	/* Copies the bytes. */
	if (size > 0)
		memcpy(wire->data + wire->size, bytes, size);
	wire->size += size;
}

/* Appends one byte. */
WIRE_UNUSED static void
wire_u8(
	struct wire *wire,
	unsigned value)
{
	uint8_t byte;

	byte = (uint8_t)(value & 0xffU);

	/* Appends it. */
	wire_bytes(wire, &byte, 1);
}

/* Appends a 32-bit big-endian integer (RFC 4251 5, uint32). */
WIRE_UNUSED static void
wire_u32(
	struct wire *wire,
	uint32_t value)
{
	uint8_t bytes[4];

	/* Most significant byte first. */
	bytes[0] = (uint8_t)(value >> 24);
	bytes[1] = (uint8_t)(value >> 16);
	bytes[2] = (uint8_t)(value >> 8);
	bytes[3] = (uint8_t)value;

	/* Appends them. */
	wire_bytes(wire, bytes, 4);
}

/* Appends a string (RFC 4251 5): the uint32 length, then the bytes. */
WIRE_UNUSED static void
wire_string(
	struct wire *wire,
	const void *bytes,
	size_t size)
{
	/* The length, then the bytes. */
	wire_u32(wire, (uint32_t)size);
	wire_bytes(wire, bytes, size);
}

/* Appends a NUL-terminated text as a string. */
WIRE_UNUSED static void
wire_text(
	struct wire *wire,
	const char *text)
{
	/* The text without its NUL. */
	wire_string(wire, text, strlen(text));
}

/* Reads a hexadecimal text (pairs of digits; spaces and colons ignored) into out; returns the byte count, or 0 on a bad digit or no room. */
WIRE_UNUSED static size_t
wire_hex(
	const char *text,
	uint8_t *out,
	size_t capacity)
{
	size_t count;
	int high;
	int digit;

	count = 0;
	high = -1;

	/* Takes each digit, pairing them into bytes. */
	for (; *text != '\0'; text++) {
		if (*text == ' ' || *text == ':' || *text == '\n')
			continue;
		if (*text >= '0' && *text <= '9')
			digit = *text - '0';
		else if (*text >= 'a' && *text <= 'f')
			digit = *text - 'a' + 10;
		else if (*text >= 'A' && *text <= 'F')
			digit = *text - 'A' + 10;
		else
			return 0;
		if (high < 0) {
			high = digit;
			continue;
		}
		if (count >= capacity)
			return 0;
		out[count++] = (uint8_t)(high * 16 + digit);
		high = -1;
	}

	/* An odd digit count is malformed. */
	if (high >= 0)
		return 0;
	return count;
}

#endif
