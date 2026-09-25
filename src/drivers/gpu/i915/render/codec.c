/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The wire codec of the Vulkan executor (see codec.h).
 *
 * Every reader and writer here checks its cursor's latched error first, so
 * a command that ran past its end or overflowed its reply fails as a whole
 * instead of decoding the rest out of place.  The per-command arena under
 * the generated record codec is carved here as well.
 */

#include "codec.h"
#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

/*
 * Reads one little-endian wire word.
 *
 * A word that would run past the end latches the reader's error and reads
 * as zero.
 */
uint32_t
drv_i915_wire_read_u32(
	struct i915_wire_reader *reader)
{
	const uint8_t *bytes;
	uint32_t word;

	/* A prior error keeps every later read from advancing. */
	if (reader->error != 0)
		return 0U;

	/* A word that would run past the end fails the whole command. */
	if (reader->offset + 4U > reader->size) {
		reader->error = 1;
		return 0U;
	}

	/* Composes the word in wire order, which is little endian whatever the host is. */
	bytes = reader->base + reader->offset;
	word = (uint32_t)bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;
	reader->offset += 4U;

	/* Succeeded: the word was inside the command. */
	return word;
}

/*
 * Reads one little-endian 64-bit wire value, the low word first.
 */
uint64_t
drv_i915_wire_read_u64(
	struct i915_wire_reader *reader)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two words; either short read latches the error and reads as zero. */
	low = drv_i915_wire_read_u32(reader);
	high = drv_i915_wire_read_u32(reader);

	/* Reports the composed value. */
	return low | (high << 32);
}

/*
 * Reads a wire object identity, which is one 64-bit value.
 */
i915_vk_handle
drv_i915_wire_read_handle(
	struct i915_wire_reader *reader)
{
	i915_vk_handle handle;

	/* Reads the identity as a plain 64-bit value. */
	handle = drv_i915_wire_read_u64(reader);

	/* Reports the identity; zero after a short read. */
	return handle;
}

/*
 * Returns a pointer to count * element wire bytes and advances past them.
 *
 * The caller reads the span in place; a span that would run past the end
 * latches the reader's error and yields NULL.
 */
const void *
drv_i915_wire_read_array(
	struct i915_wire_reader *reader,
	size_t count,
	size_t element)
{
	const void *data;
	size_t bytes;

	/* A prior error yields nothing. */
	if (reader->error != 0)
		return NULL;

	/* The span must lie inside the command. */
	bytes = count * element;
	if (reader->offset + bytes > reader->size) {
		reader->error = 1;
		return NULL;
	}

	/* Hands out the span and moves the cursor past it. */
	data = reader->base + reader->offset;
	reader->offset += bytes;

	/* Succeeded: the span is inside the command. */
	return data;
}

/*
 * Appends one little-endian word to the reply.
 *
 * A word that would overflow the reply latches the writer's error.
 */
void
drv_i915_wire_reply_u32(
	struct i915_wire_writer *writer,
	uint32_t value)
{
	uint8_t *bytes;

	/* A prior error stops further writes. */
	if (writer->error != 0)
		return;

	/* A full reply fails the stream when it ends. */
	if (writer->offset + 4U > writer->size) {
		writer->error = 1;
		return;
	}

	/* Lays the word down in the same byte order as the request. */
	bytes = writer->base + writer->offset;
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
	writer->offset += 4U;
}

/*
 * Appends one 64-bit value to the reply, the low word first.
 */
void
drv_i915_wire_reply_u64(
	struct i915_wire_writer *writer,
	uint64_t value)
{
	/* Writes the low word first, matching the reader's composition order. */
	drv_i915_wire_reply_u32(writer, (uint32_t)value);
	drv_i915_wire_reply_u32(writer, (uint32_t)(value >> 32));
}

/*
 * Appends raw bytes to the reply.
 *
 * A run that would overflow the reply is dropped whole and latches the
 * writer's error.
 */
void
drv_i915_wire_reply_bytes(
	struct i915_wire_writer *writer,
	const void *data,
	size_t bytes)
{
	/* A prior error stops further writes. */
	if (writer->error != 0)
		return;

	/* A run that does not fit fails the stream when it ends. */
	if (writer->offset + bytes > writer->size) {
		writer->error = 1;
		return;
	}

	/* Copies the run and moves the cursor past it. */
	kern_memcpy(writer->base + writer->offset, data, bytes);
	writer->offset += bytes;
}

/*
 * Returns count zeroed elements from the command's arena.
 *
 * A count the command cannot carry, or an arena too small for it, latches
 * the reader's error and yields NULL.
 */
void *
i915_vkc_array(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	uint64_t count,
	size_t element)
{
	size_t bytes;
	size_t start;

	/* A prior error yields nothing. */
	if (reader->error != 0)
		return NULL;

	/*
	 * Every element occupies at least one byte of the command, so a count
	 * beyond the bytes that remain is malformed and must not size an
	 * allocation.
	 */
	if (count > (uint64_t)(reader->size - reader->offset)) {
		reader->error = 1;
		return NULL;
	}

	/* An element of no size cannot be counted. */
	if (element == 0U) {
		reader->error = 1;
		return NULL;
	}

	/* The total must not wrap. */
	if (count > (uint64_t)(SIZE_MAX / element)) {
		reader->error = 1;
		return NULL;
	}

	/* Rounds the run and its start to eight bytes, so any record type is aligned. */
	bytes = ((size_t)count * element + 7U) & ~(size_t)7U;
	start = (arena->used + 7U) & ~(size_t)7U;

	/* A session without an arena decodes no records. */
	if (arena->base == NULL) {
		reader->error = 1;
		return NULL;
	}

	/* The aligned start must still be inside the arena. */
	if (start > arena->size) {
		reader->error = 1;
		return NULL;
	}

	/* The run must fit behind the start. */
	if (bytes > arena->size - start) {
		reader->error = 1;
		return NULL;
	}

	/* Takes the run for the current command and clears it. */
	arena->used = start + bytes;
	kern_memset(arena->base + start, 0, bytes);

	/* Succeeded: the elements live until the command returns. */
	return arena->base + start;
}

/*
 * Reads a string into the arena.
 *
 * An absent string reads as NULL.
 */
const char *
i915_vkc_read_string(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena)
{
	uint64_t bytes;
	char *text;

	/* Reads the length, which includes the terminator; zero is an absent string. */
	bytes = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return NULL;
	if (bytes == 0U)
		return NULL;

	/* Takes room for the string in the arena. */
	text = i915_vkc_array(reader, arena, bytes, 1U);
	if (text == NULL)
		return NULL;

	/* Copies the string's bytes and skips their padding. */
	i915_vkc_read_bytes(reader, text, (size_t)bytes);
	if (reader->error != 0)
		return NULL;

	/* Terminates the string, so a sender that omitted the terminator cannot cause an overrun. */
	text[bytes - 1U] = '\0';

	/* Succeeded: the string lives until the command returns. */
	return text;
}

/*
 * Reads bytes and the padding that rounds them to four.
 */
void
i915_vkc_read_bytes(
	struct i915_wire_reader *reader,
	void *destination,
	size_t bytes)
{
	size_t padded;

	/* A prior error keeps the cursor where it is. */
	if (reader->error != 0)
		return;

	/* A length that cannot be padded is malformed. */
	if (bytes > SIZE_MAX - 3U) {
		reader->error = 1;
		return;
	}

	/* The padded run must lie inside the command. */
	padded = (bytes + 3U) & ~(size_t)3U;
	if (padded > reader->size - reader->offset) {
		reader->error = 1;
		return;
	}

	/* Copies the bytes; an empty run copies nothing. */
	if (bytes != 0U)
		kern_memcpy(destination, reader->base + reader->offset, bytes);

	/* Moves the cursor past the bytes and their padding. */
	reader->offset += padded;
}

/*
 * Reads a float as its bits; no floating-point register is involved.
 */
void
i915_vkc_read_float(
	struct i915_wire_reader *reader,
	float *destination)
{
	uint32_t bits;

	/* Reads the bits and stores them unchanged. */
	bits = drv_i915_wire_read_u32(reader);
	kern_memcpy(destination, &bits, sizeof(bits));
}

/*
 * Skips the one optional external-memory declaration libvulkan may chain to
 * a buffer or an image.
 */
void
i915_vkc_skip_external_chain(
	struct i915_wire_reader *reader)
{
	uint64_t present;

	/* Reads whether a declaration follows (wire.c: [present], then the record). */
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U)
		return;

	/* Skips the record: [sType][pNext = 0][handle types]. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
}

/*
 * Writes bytes and the padding that rounds them to four.
 */
void
i915_vkc_reply_bytes(
	struct i915_wire_writer *writer,
	const void *source,
	size_t bytes)
{
	static const uint8_t zero[4] = { 0U, 0U, 0U, 0U };
	size_t padded;

	/* Writes the bytes themselves. */
	padded = (bytes + 3U) & ~(size_t)3U;
	drv_i915_wire_reply_bytes(writer, source, bytes);

	/* Writes the zero padding that rounds them to four. */
	if (padded != bytes)
		drv_i915_wire_reply_bytes(writer, zero, padded - bytes);
}

/*
 * Writes a float as its bits.
 */
void
i915_vkc_reply_float(
	struct i915_wire_writer *writer,
	const float *source)
{
	uint32_t bits;

	/* Takes the bits without a floating-point register and writes them as a word. */
	kern_memcpy(&bits, source, sizeof(bits));
	drv_i915_wire_reply_u32(writer, bits);
}
