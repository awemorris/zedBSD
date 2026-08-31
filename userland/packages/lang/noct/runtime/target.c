/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD Noct target adapters
 */

#include "userland/packages/lang/noct/runtime/zedbsd-api.h"

#ifdef NOCT_TARGET_ZEDBSD
#include <zedbsd/console.h>
#define TARGET_KEY_MASK ZEDBSD_CONSOLE_EVENT_KEY_MASK
#define TARGET_KEY_SHIFT ZEDBSD_CONSOLE_EVENT_SHIFT
#define TARGET_KEY_CTRL ZEDBSD_CONSOLE_EVENT_CTRL
#define TARGET_KEY_GRAPH ZEDBSD_CONSOLE_EVENT_GRAPH
#else
#include "hal/hal.h"
/* Private compatibility form used by firmware/runtime keyboard services. */
#define TARGET_KEY_MASK 0x000001ffU
#define TARGET_KEY_SHIFT 0x00010000U
#define TARGET_KEY_CTRL 0x00020000U
#define TARGET_KEY_GRAPH 0x00040000U
#endif
#include <noct/noct.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct target_term {
	const struct noct_services *services;
	unsigned row;
	unsigned column;
	uint8_t attribute;
	int open;
};

static struct target_term terminal;

static int directory_read(void *context, const char *path, size_t index, char *name, size_t capacity, int *is_directory);
static int term_open(void *context);
static void term_close(void *context);
static int term_is_tty(void *context);
static int term_size(void *context, unsigned *rows, unsigned *columns);
static int term_resized(void *context);
static int term_move_to(void *context, unsigned row, unsigned column);
static int term_write(void *context, const char *utf8, size_t length);
static int term_clear(void *context);
static int term_clear_to_eol(void *context);
static int term_set_style(void *context, const struct NoctTermStyle *style);
static int term_show_cursor(void *context, int visible);
static int term_flush(void *context);
static int translate_key(int event);
static int read_meta_suffix(struct target_term *term);
static int term_read_key(void *context, int timeout_ms);
static int term_pending_input(void *context);

/*
 * Implements the noct target register operation.
 */
int
noct_target_register(
	NoctEnv *env,
	const struct noct_services *services)
{
	int function_result;
	static const struct NoctDirectoryBackend directory = {
	    .read = directory_read,
	};
	static const struct NoctTermBackend term = {
	    .open = term_open,
	    .close = term_close,
	    .is_tty = term_is_tty,
	    .size = term_size,
	    .resized = term_resized,
	    .move_to = term_move_to,
	    .write = term_write,
	    .clear = term_clear,
	    .clear_to_eol = term_clear_to_eol,
	    .set_style = term_set_style,
	    .show_cursor = term_show_cursor,
	    .flush = term_flush,
	    .read_key = term_read_key,
	    .pending_input = term_pending_input,
	};

	memset(&terminal, 0, sizeof(terminal));
	terminal.services = services;
	terminal.attribute = 0xe1U;
	noct_set_directory_backend(&directory, (void *)services);

	/* Computes the function result. */
	function_result = noct_register_api_file(env) &&
	       noct_register_api_term_backend(env, &term, &terminal);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the noct target cleanup operation.
 */
void
noct_target_cleanup(
	void)
{
	noct_set_directory_backend(NULL, NULL);
	memset(&terminal, 0, sizeof(terminal));
}

/* Supports the directory read operation. */
static int
directory_read(
	void *context,
	const char *path,
	size_t index,
	char *name,
	size_t capacity,
	int *is_directory)
{
	const struct noct_services *services;
	struct noct_dirent entry;
	size_t length;
	int result;

	services = context;

	/* Handles the services availability. */
	if (services == NULL || services->directory_read == NULL ||
	    name == NULL || capacity == 0 || is_directory == NULL ||
	    index > 0xffffffffU)

		/* Reports operation failure. */
		return -1;
	result = services->directory_read(services->context, path,
					  (unsigned)index, &entry);

	/* Checks the operation result. */
	if (result <= 0)
		return result;
	length = strnlen(entry.name, sizeof(entry.name));

	/* Checks the current data length. */
	if (length >= capacity)
		return -1;
	memcpy(name, entry.name, length);
	name[length] = '\0';
	*is_directory = (entry.attributes & 0x10U) != 0;
	/* Reports operation failure. */
	return 1;
}

/* Supports the term open operation. */
static int
term_open(
	void *context)
{
	struct target_term *term;

	term = context;

	/* Handles the services availability. */
	if (term->services == NULL || term->services->screen_clear == NULL)
		return 0;

	/* Handles a failed screen clear operation. */
	if (!term->services->screen_clear(term->services->context))
		return 0;
	term->row = 0;
	term->column = 0;
	term->attribute = 0xe1U;
	term->open = 1;

	/* Handles the screen show cursor availability. */
	if (term->services->screen_show_cursor != NULL)
		(void)term->services->screen_show_cursor(
		    term->services->context, 1);

	/* Reports operation failure. */
	return 1;
}

/* Supports the term close operation. */
static void
term_close(
	void *context)
{
	struct target_term *term;

	term = context;

	/* Handles the services availability. */
	if (term->services != NULL &&
	    term->services->screen_show_cursor != NULL)
		(void)term->services->screen_show_cursor(
		    term->services->context, 1);
	term->open = 0;
}

/* Supports the term is tty operation. */
static int
term_is_tty(
	void *context)
{
	struct target_term *term;

	term = context;

	/* Returns the computed result. */
	return term->services != NULL &&
	       term->services->screen_put_utf8 != NULL &&
	       term->services->keyboard_read != NULL;
}

/* Supports the term size operation. */
static int
term_size(
	void *context,
	unsigned *rows,
	unsigned *columns)
{
	(void)context;

	/* Handles the rows availability. */
	if (rows == NULL || columns == NULL)
		return 0;
	*rows = 25;
	*columns = 80;
	/* Reports operation failure. */
	return 1;
}

/* Supports the term resized operation. */
static int
term_resized(
	void *context)
{
	(void)context;

	/* Reports successful completion. */
	return 0;
}

/* Supports the term move to operation. */
static int
term_move_to(
	void *context,
	unsigned row,
	unsigned column)
{
	int function_result;
	struct target_term *term;

	term = context;

	/* Handles the services availability. */
	if (term->services == NULL ||
	    term->services->screen_set_cursor == NULL || row == 0 || row > 25 ||
	    column == 0 || column > 80)

		/* Reports successful completion. */
		return 0;
	term->row = row - 1U;
	term->column = column - 1U;

	/* Computes the function result. */
	function_result = term->services->screen_set_cursor(term->services->context,
						 term->row, term->column);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the term write operation. */
static int
term_write(
	void *context,
	const char *utf8,
	size_t length)
{
	size_t start;
	int cells;
	struct target_term *term;
	size_t position;

	term = context;
	position = 0;

	/* Handles the services availability. */
	if (term->services == NULL || term->services->screen_put_utf8 == NULL ||
	    utf8 == NULL || length > 0xffffffffU)

		/* Reports successful completion. */
		return 0;

	/* Process each remaining element. */
	while (position < length) {

		start = position;

		/* Process each remaining element. */
		while (position < length && (uint8_t)utf8[position] != 0x1bU)
			position++;

		/* Handles the position condition. */
		if (position != start) {
			cells = term->services->screen_put_utf8(
			    term->services->context, term->row, term->column,
			    utf8 + start, (unsigned)(position - start),
			    term->attribute);

			/* Handles the cells condition. */
			if (cells < 0)
				return 0;
			term->column += (unsigned)cells;

			/* Handles the term condition. */
			if (term->column > 79U)
				term->column = 79U;
		}

		/* Handles the position condition. */
		if (position == length)
			break;

		/*
 * Remacs uses only SGR reset and reverse-video escapes in its
		 * composed rows.  Translate those to native PC-98 attributes.
		 */
		if (length - position >= 4U && utf8[position + 1U] == '[' &&
		    utf8[position + 3U] == 'm' &&
		    (utf8[position + 2U] == '0' ||
		     utf8[position + 2U] == '7')) {
			/* Handles the utf8 condition. */
			if (utf8[position + 2U] == '7')
				term->attribute |= 0x04U;
			else
				term->attribute = 0xe1U;
			position += 4U;
			continue;
		}

		/* Keep an unsupported escape visible for diagnostics. */
		cells = term->services->screen_put_utf8(
		    term->services->context, term->row, term->column,
		    utf8 + position, 1U, term->attribute);

		/* Handles the cells condition. */
		if (cells < 0)
			return 0;
		term->column += (unsigned)cells;
		position++;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the term clear operation. */
static int
term_clear(
	void *context)
{
	struct target_term *term;

	term = context;

	/* Handles a failed screen clear operation. */
	if (term->services == NULL || term->services->screen_clear == NULL ||
	    !term->services->screen_clear(term->services->context))

		/* Reports successful completion. */
		return 0;
	term->row = 0;
	term->column = 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the term clear to eol operation. */
static int
term_clear_to_eol(
	void *context)
{
	int function_result;
	struct target_term *term;

	term = context;

	/* Computes the function result. */
	function_result = term->services != NULL &&
	       term->services->screen_clear_to_eol != NULL &&
	       term->services->screen_clear_to_eol(term->services->context,
						   term->row, term->column);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the term set style operation. */
static int
term_set_style(
	void *context,
	const struct NoctTermStyle *style)
{
	struct target_term *term;
	uint8_t attribute;

	term = context;
	attribute = 0xe1U;

	/* Handles the style availability. */
	if (style == NULL)
		return 0;

	/* Handles the style condition. */
	if (style->foreground >= 0)
		attribute =
		    (uint8_t)(1U | ((unsigned)style->foreground & 7U) << 5);

	/* Handles the style condition. */
	if (style->reverse)
		attribute |= 0x04U;

	/* Handles the style condition. */
	if (style->underline)
		attribute |= 0x08U;
	term->attribute = attribute;

	/* Reports operation failure. */
	return 1;
}

/* Supports the term show cursor operation. */
static int
term_show_cursor(
	void *context,
	int visible)
{
	int function_result;
	struct target_term *term;

	term = context;

	/* Computes the function result. */
	function_result = term->services != NULL &&
	       term->services->screen_show_cursor != NULL &&
	       term->services->screen_show_cursor(term->services->context,
						  visible);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the term flush operation. */
static int
term_flush(
	void *context)
{
	struct target_term *term;

	term = context;

	/* Handles the services availability. */
	if (term->services != NULL && term->services->screen_set_cursor != NULL)
		(void)term->services->screen_set_cursor(
		    term->services->context, term->row, term->column);

	/* Reports operation failure. */
	return 1;
}

/* Supports the translate key operation. */
static int
translate_key(
	int event)
{
	unsigned shift;
	int modifiers;
	int key;

	shift = ((unsigned)event & TARGET_KEY_SHIFT ? 0x01U : 0) |
			 ((unsigned)event & TARGET_KEY_CTRL ? 0x10U : 0) |
			 ((unsigned)event & TARGET_KEY_GRAPH ? 0x08U : 0);
	modifiers = 0;
	key = event & (int)TARGET_KEY_MASK;

	/* Modifier make events describe state; they are not editor input. */
	if (key == 0x170 || key == 0x173 ||
	    key == 0x174) /* Shift, Graph, Ctrl */
		return -1;

	/* Dispatch the selected operation case. */
	switch (key) {
	case NOCT_BEUI_KEY_TAB:
		/*
 * Remacs completion and ordinary editor insertion both use the
		 * terminal's literal Tab character, not a modified Ctrl-I
		 * event. */
		return NOCT_BEUI_KEY_TAB;
	case NOCT_BEUI_KEY_BACKSPACE:
		/* Returns the computed result. */
		return 0x7f;
	case NOCT_BEUI_KEY_ENTER:
	case NOCT_BEUI_KEY_ESCAPE:
		/* Returns the computed result. */
		return key;
	case NOCT_BEUI_KEY_UP:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_UP;
	case NOCT_BEUI_KEY_DOWN:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_DOWN;
	case NOCT_BEUI_KEY_RIGHT:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_RIGHT;
	case NOCT_BEUI_KEY_LEFT:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_LEFT;
	case NOCT_BEUI_KEY_HOME:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_HOME;
	case NOCT_BEUI_KEY_END:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_END;
	case NOCT_BEUI_KEY_PAGE_UP:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_PGUP;
	case NOCT_BEUI_KEY_PAGE_DOWN:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_PGDN;
	case NOCT_BEUI_KEY_INSERT:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_INSERT;
	case NOCT_BEUI_KEY_DELETE:
		/* Returns the computed result. */
		return NOCT_TERM_KEY_DELETE;
	default:
		break;
	}

	/* Handles the selected key. */
	if (key >= NOCT_BEUI_KEY_F1 && key <= NOCT_BEUI_KEY_F10)
		return NOCT_TERM_KEY_F1 + key - NOCT_BEUI_KEY_F1;

	/* PC-98 Graph is the PC/AT Alt/Meta equivalent. */
	if ((shift & 0x08U) != 0)
		modifiers |= NOCT_TERM_MOD_META;

	/* Handles the shift condition. */
	if ((shift & 0x10U) != 0 && key == 0x20)
		return modifiers | NOCT_TERM_MOD_CTRL | 0x20;

	/* Handles the selected key. */
	if (key > 0 && key <= 0x1a)
		return modifiers | NOCT_TERM_MOD_CTRL | (key + 0x60);

	/* Handles the selected key. */
	if (key > 0x1a && key < 0x20)
		return modifiers | NOCT_TERM_MOD_CTRL | (key + 0x40);

	/* Returns the computed result. */
	return modifiers | key;
}

/* POSIX terminals encode M-x as ESC followed by x and Noct folds that byte sequence into one META event. The PC-98 BIOS reports the same input as two key events, so the target adapter performs the equivalent folding here. The available firmware clock has one-second resolution. Waiting for two changes guarantees at least one full second for a human to type the suffix, even when ESC arrived immediately before a second boundary. A bare ESC is therefore delayed by one to two seconds, but never blocks indefinitely. */
static int
read_meta_suffix(
	struct target_term *term)
{
	int function_result;
	int second;
	int previous;
	unsigned changes;

	changes = 0;

	/* Handles the keyboard poll availability. */
	if (term->services->keyboard_poll == NULL)
		return -1;

	/* Handles the clock second availability. */
	if (term->services->clock_second == NULL) {
		/* Computes the function result. */
		function_result = term->services->keyboard_poll(term->services->context) >=
			       0
			   ? term->services->keyboard_read(
				 term->services->context)
			   : -1;

		/* Returns the computed result. */
		return function_result;
	}
	previous = term->services->clock_second(term->services->context);

	/* Handles the previous condition. */
	if (previous < 0 || previous > 59)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed keyboard poll operation. */
		if (term->services->keyboard_poll(term->services->context) >= 0) {
			/* Computes the function result. */
			function_result = term->services->keyboard_read(
			    term->services->context);

			/* Returns the computed result. */
			return function_result;
		}
		second = term->services->clock_second(term->services->context);

		/* Handles the second condition. */
		if (second < 0 || second > 59)
			return -1;

		/* Handles the second condition. */
		if (second != previous) {
			previous = second;

			/* Handles the changes condition. */
			if (++changes == 2U)
				return -1;
		}
	}
}

/* Supports the term read key operation. */
static int
term_read_key(
	void *context,
	int timeout_ms)
{
	int function_result;
	int key;
	int translated;
	struct target_term *term;
	int allow_blocking = timeout_ms >= 1000;

	term = context;

	/* Handles the services availability. */
	if (term->services == NULL || term->services->keyboard_read == NULL)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		key = -1;

		/* Handles a failed keyboard poll operation. */
		if (term->services->keyboard_poll != NULL &&
		    term->services->keyboard_poll(term->services->context) >= 0)
			key = term->services->keyboard_read(
			    term->services->context);

		/*
 * The PC-98 BIOS offers a blocking read and a poll, but no
		 * millisecond timeout. Remacs uses a 20 ms grace read after
		 * each human keystroke to coalesce pasted input. Turning that
		 * grace read into a blocking BIOS call leaves every typed
		 * character waiting for the next one. Preserve blocking
		 * behavior only for the normal one-second event-loop wait. */
		else if (allow_blocking)
			key = term->services->keyboard_read(
			    term->services->context);

		/* Handles the selected key. */
		if (key < 0)
			return -1;
		translated = translate_key(key);

		/* Handles the translated condition. */
		if (translated < 0) {
			/*
 * A modifier-only event may precede the printable key
			 * in the same BIOS queue. Consume it and keep looking.
			 */
			continue;
		}

		/* Handles the translated condition. */
		if (translated == 0x1b) {
			key = read_meta_suffix(term);

			/* Handles the selected key. */
			if (key >= 0) {
				/* Computes the function result. */
				function_result = NOCT_TERM_MOD_META | translate_key(key);

				/* Returns the computed result. */
				return function_result;
			}
		}

		/* Returns the computed result. */
		return translated;
	}
}

/* Supports the term pending input operation. */
static int
term_pending_input(
	void *context)
{
	int function_result;
	struct target_term *term;

	term = context;

	/* Computes the function result. */
	function_result = term->services != NULL &&
	       term->services->keyboard_poll != NULL &&
	       term->services->keyboard_poll(term->services->context) >= 0;

	/* Returns the computed result. */
	return function_result;
}
