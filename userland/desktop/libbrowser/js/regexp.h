/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The regular expression engine of JavaScript's RegExp (plan/ws074/design.md
 * §12.3, ws074-p027): a pattern and its flags are compiled to a program
 * that a backtracking matcher runs over a string's UTF-16 code units (or
 * its code points under the u flag).  The engine knows nothing of the VM:
 * the RegExp built-ins (builtin_regexp.c) keep a program with each object
 * and turn a match into the result array.
 */

#ifndef KEILAND_BROWSER_JS_REGEXP_H
#define KEILAND_BROWSER_JS_REGEXP_H

#include <stddef.h>
#include <stdint.h>

/* The flags, in the order the flags getter spells them (d g i m s u v y). */
#define JS_REGEXP_HAS_INDICES	0x01U
#define JS_REGEXP_GLOBAL	0x02U
#define JS_REGEXP_IGNORE_CASE	0x04U
#define JS_REGEXP_MULTILINE	0x08U
#define JS_REGEXP_DOT_ALL	0x10U
#define JS_REGEXP_UNICODE	0x20U
#define JS_REGEXP_UNICODE_SETS	0x40U
#define JS_REGEXP_STICKY	0x80U

/* A capture that did not take part in the match. */
#define JS_REGEXP_UNSET		((size_t)-1)

struct js_regexp_program;

/*
 * The string a program runs over: UTF-16 code units, or Latin-1 bytes
 * (exactly one of the two is not NULL, unless the string is empty), and
 * who is asked whether a long match goes on.
 */
struct js_regexp_input {
	const uint16_t *units;
	const unsigned char *latin1;
	size_t length;

	/*
	 * Called now and then while a match runs (NULL for never): a status
	 * other than 0 ends the match with that status (the caller's task was
	 * asked to end).
	 */
	int (*poll)(void *context);
	void *poll_context;
};

/* Compiling (regexp.c). */
int js_regexp_parse_flags(const uint16_t *text, size_t length, unsigned *flags);
int js_regexp_compile(const uint16_t *pattern, size_t length, unsigned flags, struct js_regexp_program **program, const char **message);
void js_regexp_free(struct js_regexp_program *program);

/* What a program has. */
unsigned js_regexp_flags(const struct js_regexp_program *program);
uint32_t js_regexp_capture_count(const struct js_regexp_program *program);
int js_regexp_has_names(const struct js_regexp_program *program);
int js_regexp_capture_name(const struct js_regexp_program *program, uint32_t capture, const uint16_t **name, size_t *length);

/* Matching (regexp.c). */
int js_regexp_match(const struct js_regexp_program *program, const struct js_regexp_input *input, size_t start, size_t *captures, int *matched);

#endif
