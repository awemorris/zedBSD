/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GLSL compiler's memory and messages: the arena a compile or a link
 * allocates from, growing texts, and the info log's errors and warnings.
 *
 * An allocation that fails jumps to the arena's failure point instead of
 * returning, and so does a fatal error; the entry of the compile or the
 * link set that point and turns the jump into a failed result.
 */

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The size of an ordinary arena block (a larger allocation gets a block of its own). */
#define ARENA_BLOCK_SIZE	(64U * 1024U)

/* The alignment every allocation keeps. */
#define ARENA_ALIGNMENT		16U

/*
 * One block of an arena: a header, then the memory handed out from it.
 */
struct glsl_arena_block {
	struct glsl_arena_block *next;
	size_t size;
	size_t used;
};

static void arena_message(struct glsl_shader *shader, unsigned line, const char *kind, const char *format, va_list arguments);

/*
 * Allocates zeroed memory from an arena.
 *
 * It does not return on failure: it jumps to the arena's failure point.
 */
void *
glsl_alloc(
	struct glsl_arena *arena,
	size_t size)
{
	struct glsl_arena_block *block;
	size_t header;
	size_t block_size;
	unsigned char *memory;

	/* Rounds the request so the next allocation stays aligned. */
	size = (size + ARENA_ALIGNMENT - 1U) & ~(size_t)(ARENA_ALIGNMENT - 1U);
	header = (sizeof(struct glsl_arena_block) + ARENA_ALIGNMENT - 1U) & ~(size_t)(ARENA_ALIGNMENT - 1U);

	/* Takes the memory from the newest block when it has room. */
	block = arena->blocks;
	if (block != NULL && block->size - block->used >= size) {
		memory = (unsigned char *)block + header + block->used;
		block->used += size;
		memset(memory, 0, size);
		return memory;
	}

	/* Otherwise a new block, large enough for the request. */
	block_size = ARENA_BLOCK_SIZE;
	if (size > block_size)
		block_size = size;
	block = malloc(header + block_size);
	if (block == NULL)
		longjmp(*arena->failure, 1);

	/* The block becomes the newest, with the request taken from its start. */
	block->next = arena->blocks;
	block->size = block_size;
	block->used = size;
	arena->blocks = block;
	memory = (unsigned char *)block + header;
	memset(memory, 0, size);

	/* Succeeded: the zeroed memory. */
	return memory;
}

/*
 * Frees every block of an arena.
 */
void
glsl_arena_free(
	struct glsl_arena *arena)
{
	struct glsl_arena_block *block;
	struct glsl_arena_block *next;

	/* Releases the blocks newest first. */
	block = arena->blocks;
	while (block != NULL) {
		next = block->next;
		free(block);
		block = next;
	}

	/* The arena is empty again. */
	arena->blocks = NULL;
}

/*
 * Copies a piece of text into an arena as a terminated string.
 */
char *
glsl_strndup(
	struct glsl_arena *arena,
	const char *text,
	size_t length)
{
	char *copy;

	/* The copy with its terminator (the arena zeroes it). */
	copy = glsl_alloc(arena, length + 1U);
	memcpy(copy, text, length);

	/* Succeeded: the string. */
	return copy;
}

/*
 * Appends bytes to a growing text, keeping it terminated.
 *
 * The text is malloc'ed (it outlives the arena as the info log); running
 * out of memory jumps to the failure point.
 */
void
glsl_text_append(
	struct glsl_text *text,
	const char *data,
	size_t length,
	jmp_buf *failure)
{
	size_t capacity;
	char *grown;

	/* Grows the buffer to hold the bytes and a terminator. */
	if (text->length + length + 1U > text->capacity) {
		capacity = text->capacity * 2U;
		if (capacity < 256U)
			capacity = 256U;
		while (capacity < text->length + length + 1U)
			capacity *= 2U;
		grown = realloc(text->data, capacity);
		if (grown == NULL)
			longjmp(*failure, 1);
		text->data = grown;
		text->capacity = capacity;
	}

	/* Appends the bytes and terminates the text. */
	memcpy(text->data + text->length, data, length);
	text->length += length;
	text->data[text->length] = '\0';
}

/*
 * Appends formatted text to a growing text.
 */
void
glsl_text_printf(
	struct glsl_text *text,
	jmp_buf *failure,
	const char *format,
	...)
{
	va_list arguments;

	/* Formats through the va_list form. */
	va_start(arguments, format);
	glsl_text_vprintf(text, failure, format, arguments);
	va_end(arguments);
}

/*
 * Appends formatted text to a growing text, from a va_list.
 */
void
glsl_text_vprintf(
	struct glsl_text *text,
	jmp_buf *failure,
	const char *format,
	va_list arguments)
{
	char buffer[512];
	int length;

	/* Formats into a line-sized buffer (a message longer than that is cut). */
	length = vsnprintf(buffer, sizeof(buffer), format, arguments);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(buffer))
		length = (int)sizeof(buffer) - 1;

	/* Appends what was formatted. */
	glsl_text_append(text, buffer, (size_t)length, failure);
}

/*
 * Reports an error of a shader at a line.
 *
 * After GLSL_MAX_ERRORS errors the compile gives up (a jump to the
 * failure point), since later errors are mostly consequences.
 */
void
glsl_error(
	struct glsl_shader *shader,
	unsigned line,
	const char *format,
	...)
{
	va_list arguments;

	/* Appends the message to the log. */
	va_start(arguments, format);
	arena_message(shader, line, "error", format, arguments);
	va_end(arguments);

	/* Counts it, and gives up after too many. */
	shader->errors++;
	if (shader->errors >= GLSL_MAX_ERRORS)
		longjmp(shader->failure, 2);
}

/*
 * Reports a warning of a shader at a line.
 */
void
glsl_warning(
	struct glsl_shader *shader,
	unsigned line,
	const char *format,
	...)
{
	va_list arguments;

	/* Appends the message to the log. */
	va_start(arguments, format);
	arena_message(shader, line, "warning", format, arguments);
	va_end(arguments);
}

/*
 * Reports an error the compile cannot go on after (a syntax error), and
 * leaves the compile.
 */
void
glsl_fatal(
	struct glsl_shader *shader,
	unsigned line,
	const char *format,
	...)
{
	va_list arguments;

	/* Appends the message to the log. */
	va_start(arguments, format);
	arena_message(shader, line, "error", format, arguments);
	va_end(arguments);

	/* Leaves the compile with the error counted. */
	shader->errors++;
	longjmp(shader->failure, 2);
}

/* Appends one "0:LINE: KIND: message" line to a shader's log. */
static void
arena_message(
	struct glsl_shader *shader,
	unsigned line,
	const char *kind,
	const char *format,
	va_list arguments)
{
	/* The source string (always 0), the line and the kind, then the message and its end. */
	glsl_text_printf(&shader->log, &shader->failure, "0:%u: %s: ", line, kind);
	glsl_text_vprintf(&shader->log, &shader->failure, format, arguments);
	glsl_text_append(&shader->log, "\n", 1U, &shader->failure);
}
