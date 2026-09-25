/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Memory and exceptions for the shell: allocation that throws instead of
 * returning NULL, the temporary allocations of one command, the blocks parsed
 * commands live in, and the handlers errors are thrown to.
 */

#include "userland/base/sh/shell.h"
#include "userland/base/sh/vars.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The size of one chunk of an arena, unless one allocation needs more. */
#define ARENA_CHUNK 4096U

/*
 * One chunk of an arena.  Chunks are chained newest first; the newest is the
 * one allocations are carved from.
 */
struct arena_chunk {
	struct arena_chunk *next;
	size_t used;
	size_t size;
	max_align_t data[];
};

/*
 * A block of memory for one parsed command, with a count of what holds it:
 * the evaluation of the command, and each function defined in it.
 */
struct sh_arena {
	struct arena_chunk *chunks;
	int references;
};

/*
 * The innermost place an exception is caught.
 *
 * Set up and taken down in strict nesting by sh_handler_push and
 * sh_handler_pop; NULL means an exception ends the shell.
 */
struct sh_handler *sh_handler;

/*
 * The temporary allocations of the commands being run, oldest first.
 *
 * What a command expands its words into is registered here, and released
 * to the mark taken when the command began, whether it finished or an
 * exception left it.
 */
static void **temp_items;
static size_t temp_count;
static size_t temp_capacity;

static void out_of_memory(void) __attribute__((noreturn));

/*
 * Allocates memory, throwing an error when there is none.
 */
void *
sh_malloc(
	size_t size)
{
	void *memory;

	/* Allocates at least one byte, so that NULL always means failure. */
	if (size == 0)
		size = 1;
	memory = malloc(size);
	if (memory == NULL)
		out_of_memory();

	/* Succeeded: the new memory. */
	return memory;
}

/*
 * Resizes memory, throwing an error when there is none.
 */
void *
sh_realloc(
	void *memory,
	size_t size)
{
	void *resized;

	/* Resizes to at least one byte, so that NULL always means failure. */
	if (size == 0)
		size = 1;
	resized = realloc(memory, size);
	if (resized == NULL)
		out_of_memory();

	/* Succeeded: the resized memory. */
	return resized;
}

/*
 * Copies a string, throwing an error when there is no memory.
 */
char *
sh_strdup(
	const char *text)
{
	char *copy;

	/* Copies the whole string with its terminator. */
	copy = sh_malloc(strlen(text) + 1U);
	strcpy(copy, text);

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Copies the first length bytes of a string.
 */
char *
sh_strndup(
	const char *text,
	size_t length)
{
	char *copy;

	/* Copies the bytes and terminates them. */
	copy = sh_malloc(length + 1U);
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Reports how many temporary allocations there are, as a mark to release to.
 */
size_t
sh_temp_mark(
	void)
{
	/* Succeeded: the current depth. */
	return temp_count;
}

/*
 * Registers an allocation as temporary, and returns it.
 */
void *
sh_temp_own(
	void *memory)
{
	void **grown;
	size_t capacity;

	/* A failed allocation is thrown here, where it is first seen. */
	if (memory == NULL)
		out_of_memory();

	/* Grows the registry when it is full. */
	if (temp_count == temp_capacity) {
		capacity = 64U;
		if (temp_capacity != 0)
			capacity = temp_capacity * 2U;
		grown = realloc(temp_items, capacity * sizeof(*grown));
		if (grown == NULL) {
			free(memory);
			out_of_memory();
		}

		/* The larger registry. */
		temp_items = grown;
		temp_capacity = capacity;
	}

	/* The registry holds the memory until the next command. */
	temp_items[temp_count++] = memory;

	/* Succeeded: the same memory, now released with its command. */
	return memory;
}

/*
 * Grows a temporary buffer: a new one is allocated and the used bytes are
 * copied; the old one stays registered and goes with its command.
 */
void *
sh_temp_grow(
	void *memory,
	size_t used,
	size_t capacity)
{
	void *grown;

	/* A larger temporary buffer with the same contents. */
	grown = sh_temp_own(sh_malloc(capacity));
	if (used > 0)
		memcpy(grown, memory, used);

	/* Succeeded: the new buffer. */
	return grown;
}

/*
 * Frees the temporary allocations made since a mark.
 */
void
sh_temp_release(
	size_t mark)
{
	/* Frees newest first, down to the mark. */
	while (temp_count > mark)
		free(temp_items[--temp_count]);
}

/*
 * Moves a descriptor the shell keeps for itself (a script, a dot file) to
 * 10 or more, closing on exec, out of the way of the descriptors 0 to 9 that
 * scripts redirect.  Returns the descriptor to use.
 */
int
sh_descriptor_high(
	int descriptor)
{
	int moved;

	/* A copy at 10 or more replaces it, when one can be made. */
	moved = fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
	if (moved < 0)
		return descriptor;
	(void)close(descriptor);

	/* Succeeded: the moved descriptor. */
	return moved;
}

/*
 * Prints a string quoted as dash quotes it: runs without a single quote go in
 * single quotes, and runs of single quotes in double quotes, so that it's is
 * written 'it'"'"'s'.  set, export -p, alias and trap print with this.
 */
void
sh_print_quoted_always(
	const char *text)
{
	const char *cursor;

	/* An empty string is two quotes. */
	if (*text == '\0') {
		fputs("''", stdout);
		return;
	}

	/* Alternates between the two kinds of run. */
	cursor = text;
	while (*cursor != '\0') {
		if (*cursor != '\'') {
			putchar('\'');
			while (*cursor != '\0' && *cursor != '\'')
				putchar(*cursor++);
			putchar('\'');
		} else {
			putchar('"');
			while (*cursor == '\'')
				putchar(*cursor++);
			putchar('"');
		}
	}
}

/*
 * Makes an empty arena held once by its maker.
 */
struct sh_arena *
sh_arena_new(
	void)
{
	struct sh_arena *arena;

	/* Allocates the arena with no chunks yet. */
	arena = sh_malloc(sizeof(*arena));
	arena->chunks = NULL;
	arena->references = 1;

	/* Succeeded: the new arena. */
	return arena;
}

/*
 * Allocates zeroed memory from an arena.
 */
void *
sh_arena_alloc(
	struct sh_arena *arena,
	size_t size)
{
	struct arena_chunk *chunk;
	size_t align;
	size_t chunk_size;
	void *memory;

	/* Rounds the size to the strictest alignment. */
	align = sizeof(max_align_t);
	size = (size + align - 1U) & ~(align - 1U);

	/* Starts a new chunk when the newest has no room left. */
	chunk = arena->chunks;
	if (chunk == NULL || chunk->size - chunk->used < size) {
		chunk_size = ARENA_CHUNK;
		if (size > ARENA_CHUNK)
			chunk_size = size;
		chunk = sh_malloc(sizeof(*chunk) + chunk_size);
		chunk->used = 0;
		chunk->size = chunk_size;
		chunk->next = arena->chunks;
		arena->chunks = chunk;
	}

	/* Carves the allocation from the chunk. */
	memory = (char *)chunk->data + chunk->used;
	chunk->used += size;
	memset(memory, 0, size);

	/* Succeeded: memory that lives as long as the arena. */
	return memory;
}

/*
 * Copies bytes into an arena as a string.
 */
char *
sh_arena_strndup(
	struct sh_arena *arena,
	const char *text,
	size_t length)
{
	char *copy;

	/* Copies and terminates the bytes. */
	copy = sh_arena_alloc(arena, length + 1U);
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Succeeded: the copy. */
	return copy;
}

/*
 * Takes one more reference on an arena.
 */
void
sh_arena_hold(
	struct sh_arena *arena)
{
	/* One more holder: a function whose body lives here. */
	arena->references++;
}

/*
 * Drops a reference on an arena, freeing it with the last one.
 */
void
sh_arena_release(
	struct sh_arena *arena)
{
	struct arena_chunk *chunk;

	/* Ignores a missing arena. */
	if (arena == NULL)
		return;

	/* Keeps an arena something still holds. */
	arena->references--;
	if (arena->references > 0)
		return;

	/* Frees every chunk, then the arena. */
	while (arena->chunks != NULL) {
		chunk = arena->chunks;
		arena->chunks = chunk->next;
		free(chunk);
	}

	/* The arena itself goes last. */
	free(arena);
}

/*
 * Installs a handler, recording what the shell has pushed so far.
 */
void
sh_handler_push(
	struct sh_handler *handler)
{
	/* Records the depths an exception caught here goes back to. */
	handler->redirect_depth = sh_redirect_depth();
	handler->input_depth = sh_input_depth();
	handler->capture_depth = sh_input_capture_depth();
	handler->local_depth = sh_var_local_depth();
	handler->temp_mark = sh_temp_mark();
	handler->loop_nest = sh_loop_nest;
	handler->function_nest = sh_function_nest;
	handler->dot_nest = sh_dot_nest;

	/* Makes it the innermost handler. */
	handler->previous = sh_handler;
	sh_handler = handler;
}

/*
 * Removes the innermost handler after what it guarded finished normally.
 */
void
sh_handler_pop(
	struct sh_handler *handler)
{
	/* The next handler out becomes the innermost again. */
	sh_handler = handler->previous;
}

/*
 * Removes a handler that caught an exception, putting the shell's pushed
 * state back to what it was when the handler was installed.
 */
void
sh_handler_unwind(
	struct sh_handler *handler)
{
	/* The handler is no longer innermost. */
	sh_handler = handler->previous;

	/* Puts back everything pushed since it was installed. */
	sh_redirect_unwind(handler->redirect_depth);
	sh_input_unwind(handler->input_depth);
	sh_input_capture_unwind(handler->capture_depth);
	sh_var_local_unwind(handler->local_depth);
	sh_temp_release(handler->temp_mark);
	sh_loop_nest = handler->loop_nest;
	sh_function_nest = handler->function_nest;
	sh_dot_nest = handler->dot_nest;
}

/*
 * Throws an exception to the innermost handler.
 */
void
sh_raise(
	int kind)
{
	/* With no handler at all, the shell ends. */
	if (sh_handler == NULL) {
		fflush(NULL);
		if (kind == SH_EX_EXIT)
			exit(sh_exception_status & 0xff);
		exit(2);
	}

	/* Unwinds to the handler. */
	longjmp(sh_handler->environment, kind);
}

/*
 * Throws an error whose message has been written, with a status.
 */
void
sh_raise_error(
	int status)
{
	/* The status goes with the exception. */
	sh_exception_status = status;
	sh_raise(SH_EX_ERROR);
}

/*
 * Set while the shell parses text only to see whether it can run it
 * itself instead of in a subshell: an error then raises without a
 * message, and the subshell that runs the text reports it.
 */
int sh_error_quiet;

/*
 * Reports an error and throws it.
 */
void
sh_error(
	const char *format,
	...)
{
	va_list arguments;

	/* A trial the shell makes quietly raises the error without a message. */
	if (sh_error_quiet > 0) {
		sh_exception_status = 2;
		sh_raise(SH_EX_ERROR);
	}

	/* Writes the message, named after the shell and the line. */
	fflush(stdout);
	if (sh_option[SH_OPT_INTERACTIVE])
		fprintf(stderr, "sh: ");
	else
		fprintf(stderr, "%s: %d: ", sh_arg0, sh_command_line);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);

	/* An error ends the command with status 2. */
	sh_exception_status = 2;
	sh_raise(SH_EX_ERROR);
}

/*
 * Reports a problem that does not stop the shell.
 */
void
sh_warn(
	const char *format,
	...)
{
	va_list arguments;

	/* Writes the message, named after the shell and the line. */
	fflush(stdout);
	if (sh_option[SH_OPT_INTERACTIVE])
		fprintf(stderr, "sh: ");
	else
		fprintf(stderr, "%s: %d: ", sh_arg0, sh_command_line);
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

/* Throws the lack of memory as an error. */
static void
out_of_memory(
	void)
{
	/* The message needs no memory of its own. */
	sh_error("out of memory");
}
