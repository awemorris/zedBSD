/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The common helpers of browser: memory arenas, growable byte
 * buffers and arrays, UTF-8 and UTF-16, hashing and whole-file reading.
 *
 * Nothing here knows about the web.  Every other part of the browser
 * builds on these, and they build only on the C library, so the engine
 * compiles the same on the host (for the tests) and on zedBSD.
 *
 * Fallible functions return 0 on success or an errno value.
 */

#ifndef KEILAND_BROWSER_BASE_H
#define KEILAND_BROWSER_BASE_H

#include <stddef.h>
#include <stdint.h>

/* Marks a parameter a function has to take but does not read. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The code point that replaces bytes or units that do not decode. */
#define WB_REPLACEMENT		0xfffdU

/* The largest Unicode code point. */
#define WB_CODE_POINT_MAX	0x10ffffU

/*
 * One block of an arena's memory.
 *
 * The blocks of an arena form a list, the newest first; the space after
 * used is where the next allocation goes.
 */
struct wb_arena_block {
	struct wb_arena_block *next;
	size_t size;
	size_t used;
};

/*
 * Memory handed out in pieces and freed all at once.
 *
 * Parsers keep their temporary trees in an arena and release it when the
 * tree has been turned into something longer-lived.  An arena whose head
 * is NULL owns nothing.
 */
struct wb_arena {
	struct wb_arena_block *head;
	size_t block_size;
};

/*
 * A growable run of bytes.
 *
 * data is NULL until the first byte is added.  There is always room for a
 * terminating NUL past length once data exists, so the bytes can be read
 * as a C string when they hold text.
 */
struct wb_buffer {
	unsigned char *data;
	size_t length;
	size_t capacity;
};

/*
 * A growable run of UTF-16 code units.
 *
 * It is how decoded document text and JavaScript strings are built up
 * before they are turned into VM strings.
 */
struct wb_units {
	uint16_t *data;
	size_t length;
	size_t capacity;
};

/*
 * A growable array of equally sized items.
 *
 * The items move when the array grows, so a pointer to one is only good
 * until the next push.
 */
struct wb_vector {
	void *items;
	size_t count;
	size_t capacity;
	size_t item_size;
};

/* The arena (arena.c). */
void wb_arena_init(struct wb_arena *arena, size_t block_size);
void *wb_arena_alloc(struct wb_arena *arena, size_t size);
void *wb_arena_zalloc(struct wb_arena *arena, size_t size);
char *wb_arena_strndup(struct wb_arena *arena, const char *string, size_t length);
void wb_arena_release(struct wb_arena *arena);

/* The byte buffer (buffer.c). */
void wb_buffer_init(struct wb_buffer *buffer);
int wb_buffer_reserve(struct wb_buffer *buffer, size_t extra);
int wb_buffer_append(struct wb_buffer *buffer, const void *bytes, size_t length);
int wb_buffer_append_byte(struct wb_buffer *buffer, unsigned char byte);
int wb_buffer_append_string(struct wb_buffer *buffer, const char *string);
int wb_buffer_append_utf8(struct wb_buffer *buffer, uint32_t code_point);
int wb_buffer_printf(struct wb_buffer *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
void wb_buffer_clear(struct wb_buffer *buffer);
void wb_buffer_release(struct wb_buffer *buffer);
const char *wb_buffer_string(const struct wb_buffer *buffer);
const void *wb_memmem(const void *haystack, size_t length, const void *needle, size_t needle_length);

/* The UTF-16 buffer (buffer.c). */
void wb_units_init(struct wb_units *units);
int wb_units_reserve(struct wb_units *units, size_t extra);
int wb_units_append(struct wb_units *units, const uint16_t *source, size_t length);
int wb_units_append_code_point(struct wb_units *units, uint32_t code_point);
void wb_units_clear(struct wb_units *units);
void wb_units_release(struct wb_units *units);

/* The array (vector.c). */
void wb_vector_init(struct wb_vector *vector, size_t item_size);
int wb_vector_push(struct wb_vector *vector, const void *item);
void *wb_vector_at(const struct wb_vector *vector, size_t index);
void wb_vector_pop(struct wb_vector *vector);
void wb_vector_clear(struct wb_vector *vector);
void wb_vector_release(struct wb_vector *vector);

/* Unicode encodings (utf.c). */
size_t wb_utf8_decode(const unsigned char *bytes, size_t length, uint32_t *code_point);
size_t wb_utf8_encode(uint32_t code_point, unsigned char *out);
size_t wb_utf16_decode(const uint16_t *units, size_t length, uint32_t *code_point);
size_t wb_utf16_encode(uint32_t code_point, uint16_t *out);
int wb_utf8_to_units(const unsigned char *bytes, size_t length, struct wb_units *units);
int wb_units_to_utf8(const uint16_t *units, size_t length, struct wb_buffer *buffer);

/* Hashing (hash.c). */
uint32_t wb_hash_bytes(const void *bytes, size_t length);
uint32_t wb_hash_units(const uint16_t *units, size_t length);

/* Files (file.c). */
int wb_file_read(const char *path, struct wb_buffer *buffer);
int wb_file_write(const char *path, const void *bytes, size_t length);

#endif
