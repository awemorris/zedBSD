/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 text-console and interrupt-driven keyboard implementation.
 */

#include <hal/hal.h>

#include <string.h>

#include "../../cons-wait.h"
#include "../defs.h"
#include "../irq.h"
#include "keyboard-map.h"

#define SCAN_MAX 0x80U

#define SCAN_SHIFT_L 0x70U
#define SCAN_CAPS 0x71U
#define SCAN_KANA 0x72U
#define SCAN_GRAPH 0x73U
#define SCAN_CTRL 0x74U
#define SCAN_SHIFT_R 0x7dU

#define KBD_DATA 0x41U
#define KBD_STATUS 0x43U
#define KBD_RXRDY 0x02U

/* Includes 128 physical scans, two resync markers, and one ring sentinel. */
#define QUEUE_SIZE 131U

/* Unshifted PC-98 JIS characters indexed by keyboard scan code. */
static const uint16_t base_table[SCAN_MAX] = {
	[0x01] = HAL_KEY_JIS_1, [0x02] = HAL_KEY_JIS_2,
	[0x03] = HAL_KEY_JIS_3, [0x04] = HAL_KEY_JIS_4,
	[0x05] = HAL_KEY_JIS_5, [0x06] = HAL_KEY_JIS_6,
	[0x07] = HAL_KEY_JIS_7, [0x08] = HAL_KEY_JIS_8,
	[0x09] = HAL_KEY_JIS_9, [0x0a] = HAL_KEY_JIS_0,
	[0x0b] = HAL_KEY_JIS_MINUS, [0x0c] = HAL_KEY_JIS_CARET,
	[0x0d] = HAL_KEY_JIS_YEN,
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1a] = HAL_KEY_JIS_AT, [0x1b] = HAL_KEY_JIS_LBRACE,
	[0x1d] = 'a', [0x1e] = 's', [0x1f] = 'd', [0x20] = 'f', [0x21] = 'g',
	[0x22] = 'h', [0x23] = 'j', [0x24] = 'k', [0x25] = 'l',
	[0x26] = HAL_KEY_JIS_SEMI, [0x27] = HAL_KEY_JIS_COLON,
	[0x28] = HAL_KEY_JIS_RBRACE,
	[0x29] = 'z', [0x2a] = 'x', [0x2b] = 'c', [0x2c] = 'v', [0x2d] = 'b',
	[0x2e] = 'n', [0x2f] = 'm', [0x30] = HAL_KEY_JIS_COMMA,
	[0x31] = HAL_KEY_JIS_DOT, [0x32] = HAL_KEY_JIS_SLASH,
	[0x33] = HAL_KEY_JIS_RO, [0x34] = ' ',
};

/* HAL-owned JIS X 0208 rows 1 through 84. */
extern const uint16_t hal_pc98_jisx0208_to_ucs[7896];

static volatile uint16_t *const text_vram =
	(volatile uint16_t *)0x800a0000;
static volatile uint8_t *const attribute_vram =
	(volatile uint8_t *)0x800a2000;
static enum hal_cons_mode console_mode;
static unsigned cursor_row;
static unsigned cursor_column;
static int cursor_visible;
static int console_suspended;
static int software_cursor_drawn;
static unsigned software_cursor_offset;
static uint8_t software_cursor_attribute[2];
static struct pc98_keyboard keyboard;
static struct hal_key_event events[QUEUE_SIZE];
static unsigned head;
static unsigned tail;
static struct hal_cons_wait_queue input_waiters;

static uint8_t port_in8(uint16_t port);
static void port_out8(uint16_t port, uint8_t value);
static int gdc_write(uint16_t port, uint8_t value);
static void software_cursor_remove(void);
static void write_cell(unsigned row, unsigned column, uint16_t code, uint8_t attribute);
static void scroll(void);
static void newline(void);
static void put_single_cell(uint16_t code);
static uint32_t utf8_codepoint(const char **input, const char *end);
static uint16_t unicode_to_pc98(uint32_t codepoint, unsigned *width);
static int special_key(uint8_t scan);
static int key_to_scan(int key);
static void set_down(struct pc98_keyboard *kb, uint8_t scan, int pressed);
static int modifier_key(uint8_t scan);
static int pc98_keyboard_scan_down(const struct pc98_keyboard *kb, uint8_t scan);
static unsigned pc98_keyboard_modifiers(const struct pc98_keyboard *kb);
static const char *special_symbol(unsigned key);
static int event_from_legacy(struct hal_key_event *event, unsigned legacy);
static int snapshot_modifier(unsigned key);
static void rebuild_keyboard_events_locked(void);
static void enqueue_raw_locked(uint8_t raw);
static uint8_t inb(uint16_t port);
static bool input_lock_acquire(void);
static void input_lock_release(bool enabled);
static void pump_locked(void);
static void keyboard_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument);
static int legacy_character(const char *symbol);

/*
 * Clears one PC-98 text-console row.
 */
void
hal_cons_clear_row(
	unsigned row)
{
	unsigned column;

	/* Ignores rows outside the visible text grid. */
	if (row >= HAL_CONS_ROWS)
		return;

	/* Fills every cell with a normal attributed blank. */
	for (column = 0; column < HAL_CONS_COLUMNS; column++) {
		write_cell(
			row,
			column,
			' ',
			HAL_CONS_NORMAL_ATTRIBUTE);
	}
}

/*
 * Clears the complete PC-98 text console.
 */
void
hal_cons_clear(
	void)
{
	unsigned row;

	/* Clears every visible row. */
	for (row = 0; row < HAL_CONS_ROWS; row++)
		hal_cons_clear_row(row);
}

/*
 * Resets the PC-98 console to its fixed-menu defaults.
 */
void
hal_cons_reset(
	void)
{
	/* Clears display contents before resetting logical state. */
	hal_cons_clear();

	/* Restores the fixed-menu mode and visible home cursor. */
	console_mode = HAL_CONS_FIXED_MENU;
	cursor_row = 0;
	cursor_column = 0;
	cursor_visible = 1;
}

/*
 * Writes one character to the PC-98 text console.
 */
void
hal_cons_putc(
	int character)
{
	uint8_t byte;

	/* Narrows the input through the existing byte-oriented interface. */
	byte = (uint8_t)character;

	/* Applies control bytes or emits one ordinary text cell. */
	if (byte == '\n') {
		newline();
	} else if (byte == '\r') {
		cursor_column = 0;
	} else if (byte == '\b') {
		/* Erases one preceding cell when the cursor can move left. */
		if (cursor_column != 0) {
			cursor_column--;
			write_cell(
				cursor_row,
				cursor_column,
				' ',
				HAL_CONS_NORMAL_ATTRIBUTE);
		}
	} else {
		put_single_cell(byte);
	}

	/* Publishes the cursor while terminal mode owns it. */
	if (console_mode == HAL_CONS_TERMINAL)
		hal_cons_update_cursor();
}

/*
 * Writes one terminated UTF-8 string to the PC-98 text console.
 */
void
hal_cons_write(
	const char *string)
{
	unsigned length;

	/* Ignores a missing string. */
	if (string == NULL)
		return;

	/* Measures the string through its terminating byte. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured byte range. */
	hal_cons_write_n(string, length);
}

/*
 * Writes a bounded UTF-8 string to the PC-98 text console.
 */
void
hal_cons_write_n(
	const char *string,
	unsigned length)
{
	const char *end;
	uint32_t codepoint;
	unsigned width;
	uint16_t code;

	/* Ignores a missing string. */
	if (string == NULL)
		return;

	/* Decodes and emits every complete or substituted code point. */
	end = string + length;
	while (string < end) {
		codepoint = utf8_codepoint(&string, end);

		/* Delegates supported control characters to the byte writer. */
		if (codepoint == '\n' || codepoint == '\r' ||
		    codepoint == '\b') {
			hal_cons_putc((int)codepoint);
			continue;
		}

		/* Converts the code point and wraps before a complete glyph. */
		code = unicode_to_pc98(codepoint, &width);
		if (cursor_column + width > HAL_CONS_COLUMNS)
			newline();

		/* Writes the first text cell. */
		write_cell(
			cursor_row,
			cursor_column++,
			code,
			HAL_CONS_NORMAL_ATTRIBUTE);

		/* Writes the marked second cell for a double-cell glyph. */
		if (width == 2U) {
			write_cell(
				cursor_row,
				cursor_column++,
				code | 0x8000U,
				HAL_CONS_NORMAL_ATTRIBUTE);
		}
	}

	/* Publishes the cursor while terminal mode owns it. */
	if (console_mode == HAL_CONS_TERMINAL)
		hal_cons_update_cursor();
}

/*
 * Writes one string at a PC-98 text-grid position.
 */
void
hal_cons_write_at(
	unsigned row,
	unsigned column,
	const char *string)
{
	/* Ignores a position outside the visible grid. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return;

	/* Stores the requested origin and emits the string from it. */
	cursor_row = row;
	cursor_column = column;
	hal_cons_write(string);
}

/*
 * Clears from the current PC-98 cursor through the row end.
 */
void
hal_cons_clear_to_eol(
	void)
{
	unsigned column;

	/* Clears every cell after the stored cursor position. */
	for (column = cursor_column; column < HAL_CONS_COLUMNS; column++) {
		write_cell(
			cursor_row,
			column,
			' ',
			HAL_CONS_NORMAL_ATTRIBUTE);
	}
}

/*
 * Writes one attributed string at a PC-98 text-grid position.
 */
int
hal_cons_write_at_attr(
	unsigned row,
	unsigned column,
	const char *string,
	uint8_t attribute)
{
	unsigned length;
	int result;

	/* Requires a visible origin and source string. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS ||
	    string == NULL) {
		return -1;
	}

	/* Measures the string through its terminating byte. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured row without splitting a wide glyph. */
	result = hal_cons_write_n_at(
		row,
		column,
		string,
		length,
		attribute);

	/* Returns the positioned-write result. */
	return result;
}

/*
 * Writes a bounded UTF-8 string at a PC-98 text-grid position.
 */
int
hal_cons_write_n_at(
	unsigned row,
	unsigned column,
	const char *string,
	unsigned length,
	uint8_t attribute)
{
	const char *position = string;
	const char *end = string + length;
	unsigned changed = 0;
	uint32_t codepoint;
	unsigned width;
	uint16_t code;

	/* Requires a visible origin and source string. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS ||
	    string == NULL) {
		return -1;
	}

	/* Decodes text while the destination remains on screen. */
	while (position < end && row < HAL_CONS_ROWS) {
		codepoint = utf8_codepoint(&position, end);

		/* Returns to the current row origin for carriage return. */
		if (codepoint == '\r') {
			column = 0;
			continue;
		}

		/* Advances to the next row for a newline. */
		if (codepoint == '\n') {
			column = 0;
			row++;
			continue;
		}

		/* Stops rather than splitting a wide glyph at the right edge. */
		code = unicode_to_pc98(codepoint, &width);
		if (column + width > HAL_CONS_COLUMNS)
			break;

		/* Writes and accounts for the first text cell. */
		write_cell(row, column++, code, attribute);
		changed++;

		/* Writes and accounts for the marked second cell when needed. */
		if (width == 2U) {
			write_cell(row, column++, code | 0x8000U, attribute);
			changed++;
		}
	}

	/* Clips and stores the final logical cursor position. */
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ?
	    column : HAL_CONS_COLUMNS - 1U;

	/* Returns the number of changed cells. */
	return (int)changed;
}

/*
 * Clears from one PC-98 text-grid position through the row end.
 */
int
hal_cons_clear_to_eol_at(
	unsigned row,
	unsigned column)
{
	unsigned current;

	/* Reports no change for a position outside the visible grid. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return 0;

	/* Clears every remaining cell with the normal attribute. */
	for (current = column; current < HAL_CONS_COLUMNS; current++) {
		write_cell(
			row,
			current,
			' ',
			HAL_CONS_NORMAL_ATTRIBUTE);
	}

	/* Stores the requested cursor position. */
	cursor_row = row;
	cursor_column = column;

	/* Reports a cleared row suffix. */
	return 1;
}

/*
 * Sets the PC-98 text-console cursor position.
 */
int
hal_cons_set_cursor(
	unsigned row,
	unsigned column)
{
	/* Rejects a position outside the visible grid. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return 0;

	/* Stores and publishes the selected cursor position. */
	cursor_row = row;
	cursor_column = column;
	hal_cons_update_cursor();

	/* Reports a valid cursor position. */
	return 1;
}

/*
 * Changes PC-98 cursor visibility.
 */
void
hal_cons_show_cursor(
	int visible)
{
	/* Stores and publishes normalized cursor visibility. */
	cursor_visible = visible != 0;
	hal_cons_update_cursor();
}

/*
 * Saves the current PC-98 console state.
 */
void
hal_cons_save_state(
	struct hal_cons_state *state)
{
	/* Ignores a missing state destination. */
	if (state == NULL)
		return;

	/* Copies mode, position, and cursor visibility. */
	state->mode = console_mode;
	state->row = cursor_row;
	state->column = cursor_column;
	state->cursor_visible = cursor_visible;
}

/*
 * Restores PC-98 terminal mode around retained output state.
 */
void
hal_cons_restore_terminal(
	const struct hal_cons_state *state)
{
	unsigned output_row;
	unsigned output_column;

	/* Captures the current logical output position. */
	output_row = cursor_row;
	output_column = cursor_column;

	/* Selects terminal mode and repairs an invalid retained position. */
	console_mode = HAL_CONS_TERMINAL;
	if (output_row >= HAL_CONS_ROWS) {
		output_row = 0;
		output_column = 0;
	}

	/* Selects a later valid saved row or retains the output position. */
	if (state != NULL && state->mode == HAL_CONS_TERMINAL &&
	    state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS &&
	    state->row > output_row) {
		cursor_row = state->row;
		cursor_column = state->column;
	} else {
		cursor_row = output_row;
		cursor_column = output_column;
	}

	/* Starts subsequent terminal output on a fresh row when necessary. */
	if (cursor_column != 0)
		newline();

	/* Restores and publishes cursor visibility. */
	cursor_visible = 1;
	hal_cons_update_cursor();
}

/*
 * Selects the PC-98 console output mode.
 */
void
hal_cons_set_mode(
	enum hal_cons_mode mode)
{
	/* Publishes the requested console mode. */
	console_mode = mode;
}

/*
 * Suspends PC-98 text and cursor updates.
 */
void
hal_cons_suspend(
	void)
{
	/* Avoids duplicate hardware suspension. */
	if (console_suspended)
		return;

	/* Removes any software cursor before programming the GDC. */
	software_cursor_remove();

	/* Hides the GDC cursor in the existing command-byte order. */
	(void)gdc_write(0x62U, 0x4bU);
	(void)gdc_write(0x60U, 0x0fU);
	(void)gdc_write(0x60U, 0x20U);
	(void)gdc_write(0x60U, 0x7bU);

	/* Blocks subsequent display-memory writes. */
	console_suspended = 1;
}

/*
 * Resumes and clears the PC-98 text console.
 */
void
hal_cons_resume(
	void)
{
	/* Avoids a display reset while the console is active. */
	if (!console_suspended)
		return;

	/* Restores terminal ownership and the logical home position. */
	console_suspended = 0;
	console_mode = HAL_CONS_TERMINAL;
	cursor_column = 0;
	cursor_row = 0;

	/* Clears the display and publishes the visible cursor. */
	hal_cons_clear();
	hal_cons_update_cursor();
}

/*
 * Updates the PC-98 hardware or software cursor.
 */
void
hal_cons_update_cursor(
	void)
{
	unsigned address;
	uint16_t left;
	uint16_t right;
	int wide;
	int written;

	/* Computes the current text-memory cell and default cursor form. */
	address = cursor_row * HAL_CONS_COLUMNS + cursor_column;
	wide = 0;

	/* Suppresses GDC access while another display owner is active. */
	if (console_suspended)
		return;

	/* Removes a stale software cursor before inspecting the new cell. */
	software_cursor_remove();

	/* Detects a complete two-cell Japanese glyph at the cursor. */
	if (cursor_visible && cursor_column + 1U < HAL_CONS_COLUMNS) {
		left = text_vram[address];
		right = text_vram[address + 1U];
		wide = (left & 0x8000U) == 0U &&
		    (right & 0x8000U) != 0U &&
		    (left & 0x7fffU) == (right & 0x7fffU);
	}

	/* Starts the GDC cursor-form command. */
	written = gdc_write(0x62U, 0x4bU);
	if (!written)
		return;

	/* Programs the requested visible or hidden raster form. */
	written = gdc_write(0x60U, cursor_visible && !wide ? 0x8fU : 0x0fU);
	if (!written)
		return;

	/* Programs the retained middle cursor-form byte. */
	written = gdc_write(0x60U, 0x20U);
	if (!written)
		return;

	/* Completes the cursor-form command. */
	written = gdc_write(0x60U, 0x7bU);
	if (!written)
		return;

	/* Draws a full-width cursor by reversing both glyph attributes. */
	if (wide) {
		software_cursor_offset = address;
		software_cursor_attribute[0] = attribute_vram[address * 2U];
		software_cursor_attribute[1] =
		    attribute_vram[(address + 1U) * 2U];
		attribute_vram[address * 2U] =
		    software_cursor_attribute[0] ^ 0x04U;
		attribute_vram[(address + 1U) * 2U] =
		    software_cursor_attribute[1] ^ 0x04U;
		software_cursor_drawn = 1;
		return;
	}

	/* Leaves the hidden hardware cursor without a position command. */
	if (!cursor_visible)
		return;

	/* Starts the visible one-cell cursor-address command. */
	written = gdc_write(0x62U, 0x49U);
	if (!written)
		return;

	/* Programs the low cursor-address byte. */
	written = gdc_write(0x60U, (uint8_t)address);
	if (!written)
		return;

	/* Programs the high cursor-address byte. */
	(void)gdc_write(0x60U, (uint8_t)(address >> 8));
}

/*
 * Resets one PC-98 keyboard translation state.
 */
void
pc98_keyboard_reset(
	struct pc98_keyboard *kb)
{
	unsigned i;

	/* Clears every modifier and lock state. */
	kb->shift = kb->ctrl = kb->graph = kb->caps = kb->kana = 0;

	/* Clears the held-key bitmap. */
	for (i = 0; i < sizeof(kb->down); i++)
		kb->down[i] = 0;

	/* Clears the emitted-key record for every scan position. */
	for (i = 0; i < sizeof(kb->last_key) / sizeof(kb->last_key[0]); i++)
		kb->last_key[i] = 0;
}

/*
 * Translates one raw PC-98 keyboard scan into a legacy event.
 */
int
pc98_keyboard_feed(
	struct pc98_keyboard *kb,
	uint8_t raw,
	unsigned *result)
{
	uint8_t scan;
	int pressed;
	int was_down;
	uint16_t ch;
	int key;
	int special;

	/* Decodes the scan position and make-or-break state. */
	scan = raw & 0x7fU;
	pressed = (raw & 0x80U) == 0U;
	was_down = pc98_keyboard_scan_down(kb, scan);

	/* Rejects a missing event destination. */
	if (result == NULL)
		return 0;

	/* Recovers a break key or maps a modifier make before state changes. */
	key = pressed ? modifier_key(scan) : kb->last_key[scan];
	set_down(kb, scan, pressed);

	/* Updates modifier and lock state for the physical scan. */
	switch (scan) {
	case SCAN_SHIFT_L:
	case SCAN_SHIFT_R:
		kb->shift = (uint8_t)(
		    pc98_keyboard_scan_down(kb, SCAN_SHIFT_L) ||
		    pc98_keyboard_scan_down(kb, SCAN_SHIFT_R));
		break;
	case SCAN_CTRL:
		kb->ctrl = (uint8_t)pressed;
		break;
	case SCAN_GRAPH:
		kb->graph = (uint8_t)pressed;
		break;
	case SCAN_CAPS:
		/* Toggles Caps Lock only for a new make transition. */
		if (pressed && !was_down)
			kb->caps ^= 1U;
		break;
	case SCAN_KANA:
		/* Toggles Kana Lock only for a new make transition. */
		if (pressed && !was_down)
			kb->kana ^= 1U;
		break;
	default:
		break;
	}

	/* Emits a release only when the scan has a stable key identity. */
	if (!pressed) {
		kb->last_key[scan] = 0;

		/* Recovers a modifier identity absent from the emitted-key table. */
		if (key == 0)
			key = modifier_key(scan);

		/* Drops a release whose scan never acquired a stable identity. */
		if (key == 0)
			return 0;

		*result = ((unsigned)key & HAL_KEY_EVENT_KEY_MASK) |
		    pc98_keyboard_modifiers(kb) |
		    HAL_KEY_EVENT_RELEASE_PRIVATE;

		/* Reports one translated release event. */
		return 1;
	}

	/* Resolves an ordinary make through special and base tables. */
	if (key == 0) {
		/* Prefers a mapped navigation or function key over base text. */
		special = special_key(scan);
		if (special != 0) {
			key = special;
		} else {
			/* Resolves the remaining physical scan through base text. */
			ch = base_table[scan];
			if (ch == 0)
				return 0;
			key = ch;
		}
	}

	/* Records key identity and publishes press-or-repeat state. */
	kb->last_key[scan] = (uint16_t)key;
	*result = ((unsigned)key & HAL_KEY_EVENT_KEY_MASK) |
	    pc98_keyboard_modifiers(kb) |
	    (was_down ? HAL_KEY_EVENT_REPEAT_PRIVATE : 0U);

	/* Reports one translated make event. */
	return 1;
}

/*
 * Reports whether one PC-98 keyboard key is held.
 */
int
pc98_keyboard_is_down(
	const struct pc98_keyboard *kb,
	int key)
{
	int scan;

	/* Resolves the public key value to a physical scan position. */
	scan = key_to_scan(key);
	if (scan < 0)
		return -1;

	/* Returns the selected held-key bitmap bit. */
	return (kb->down[scan >> 3] >> (scan & 7)) & 1;
}

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
/*
 * Resets PC-98 keyboard ownership state for the focused host test.
 */
void
pc98_input_ownership_test_reset(
	void)
{
	/* Clears translated keyboard and queued-event state. */
	pc98_keyboard_reset(&keyboard);
	memset(events, 0, sizeof(events));
	tail = 0;
	head = 0;
}

/*
 * Feeds one raw PC-98 keyboard scan for the focused host test.
 */
void
pc98_input_ownership_test_raw(
	uint8_t raw)
{
	/* Translates and enqueues the supplied scan. */
	enqueue_raw_locked(raw);
}

/*
 * Rebuilds the PC-98 keyboard snapshot for the focused host test.
 */
void
pc98_input_ownership_test_rebuild(
	void)
{
	/* Rebuilds queued events from held-key state. */
	rebuild_keyboard_events_locked();
}

/*
 * Pops one PC-98 event for the focused host test.
 */
int
pc98_input_ownership_test_pop(
	struct hal_key_event *event)
{
	/* Reports an empty event ring. */
	if (tail == head)
		return 0;

	/* Copies an optional event and advances the ring tail. */
	if (event != NULL)
		*event = events[tail];
	tail = (tail + 1U) % QUEUE_SIZE;

	/* Reports one consumed event. */
	return 1;
}
#endif

/*
 * Reports the active PC-98 keyboard modifier mask.
 */
unsigned
hal_cons_modifiers(
	void)
{
	bool enabled;
	unsigned modifiers;

	/* Samples modifier state under the input wait-queue lock. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	modifiers = (keyboard.shift ? HAL_KEY_EVENT_SHIFT_PRIVATE : 0U) |
	    (keyboard.ctrl ? HAL_KEY_EVENT_CTRL_PRIVATE : 0U) |
	    (keyboard.graph ? HAL_KEY_EVENT_GRAPH_PRIVATE : 0U);
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Returns the sampled modifier mask. */
	return modifiers;
}

/*
 * Reports whether a PC-98 keyboard event is queued.
 */
int
hal_cons_poll_event(
	struct hal_key_event *event)
{
	bool enabled;
	int available;

	/* Samples the event ring under its wait-queue lock. */
	enabled = input_lock_acquire();
	available = tail != head;

	/* Copies the next event only when the caller requested its value. */
	if (available && event != NULL)
		*event = events[tail];
	input_lock_release(enabled);

	/* Returns whether an event was available without consuming it. */
	return available;
}

/*
 * Reports PC-98 keyboard input capabilities.
 */
void
hal_cons_get_input_info(
	struct hal_cons_input_info *info)
{
	static const char *const symbols[] = {
		"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k",
		"l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v",
		"w", "x", "y", "z", " ",
		"jis-1", "jis-2", "jis-3", "jis-4", "jis-5", "jis-6",
		"jis-7", "jis-8", "jis-9", "jis-0", "jis-minus",
		"jis-caret", "jis-yen", "jis-at", "jis-lbrace", "jis-semi",
		"jis-colon", "jis-rbrace", "jis-comma", "jis-dot",
		"jis-slash", "jis-ro", "esc", "backspace", "tab", "enter",
		"leftshift", "rightshift", "leftctrl", "leftalt", "capslock",
		"kana", "home", "up", "pageup", "left", "right", "end",
		"down", "pagedown", "insert", "delete", "f1", "f2", "f3",
		"f4", "f5", "f6", "f7", "f8", "f9", "f10"
	};

	/* Ignores a missing capability destination. */
	if (info == NULL)
		return;

	/* Publishes event modes and the stable symbol inventory. */
	info->flags = HAL_CONS_INPUT_RELEASE | HAL_CONS_INPUT_REPEAT;
	info->symbols = symbols;
	info->symbol_count = sizeof(symbols) / sizeof(symbols[0]);
}

/*
 * Reads one PC-98 keyboard event, waiting when necessary.
 */
int
hal_cons_read_event(
	struct hal_key_event *event)
{
	struct hal_cons_wait_entry waiter;
	bool enabled;

	/* Initializes the current task's stack-owned wait entry. */
	waiter.task = hal_task_get_current();
	waiter.next = NULL;
	waiter.queued = 0;

	/* Rechecks the event ring after every task wakeup. */
	for (;;) {
		enabled = input_lock_acquire();

		/* Consumes and reports the oldest queued event. */
		if (tail != head) {
			/* Copies the event when the caller supplied storage. */
			if (event != NULL)
				*event = events[tail];
			tail = (tail + 1U) % QUEUE_SIZE;
			input_lock_release(enabled);
			return 1;
		}

		/* Queues this task before releasing the lock and sleeping. */
		hal_cons_wait_queue_add(&input_waiters, &waiter);
		input_lock_release(enabled);
		kernel_wait_task();
	}
}

/*
 * Reads one text character from PC-98 keyboard events.
 */
int
hal_cons_getc(
	void)
{
	struct hal_key_event event;
	int character;

	/* Waits until an event maps to a supported text character. */
	for (;;) {
		(void)hal_cons_read_event(&event);

		/* Ignores snapshot-only ownership records. */
		if ((event.flags & HAL_KEY_EVENT_SNAPSHOT) != 0U)
			continue;

		/* Ignores releases and other non-character event types. */
		if ((event.flags &
		    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0U) {
			continue;
		}

		/* Returns literal one-byte symbols directly. */
		if (event.symbol[1] == '\0')
			return event.symbol[0];

		/* Maps supported named symbols through the legacy interface. */
		character = legacy_character(event.symbol);
		if (character >= 0)
			return character;
	}
}

/*
 * Reports legacy PC-98 keyboard state for one key value.
 */
int
hal_cons_key_state(
	int key)
{
	bool enabled;
	int down;

	/* Samples the selected physical key under the input lock. */
	enabled = input_lock_acquire();
	down = pc98_keyboard_is_down(&keyboard, key);
	input_lock_release(enabled);

	/* Returns the sampled key state or unsupported-key marker. */
	return down;
}

/*
 * Discards all queued PC-98 keyboard events.
 */
void
hal_cons_drain_input(
	void)
{
	bool enabled;

	/* Moves the ring tail to its head under the input lock. */
	enabled = input_lock_acquire();
	tail = head;
	input_lock_release(enabled);
}

/*
 * Initializes the PC-98 console and keyboard state.
 */
void
i386_bsp_cons_init(
	void)
{
	/* Clears translated keyboard and queued-event state. */
	pc98_keyboard_reset(&keyboard);
	tail = 0;
	head = 0;

	/* Initializes wait ownership before resetting the display. */
	hal_cons_wait_queue_init(&input_waiters);
	hal_cons_reset();
}

/*
 * Enables interrupt-driven PC-98 keyboard input.
 */
void
i386_bsp_cons_irq_init(
	void)
{
	int result;

	/* Registers the keyboard IRQ handler before unmasking delivery. */
	result = hal_irq_set_handler(IRQ_KEYBOARD, keyboard_interrupt, NULL);
	if (result != HAL_OK)
		HAL_FATAL("PC-98 keyboard IRQ registration failed");

	/* Opens keyboard delivery after successful handler registration. */
	hal_irq_unmask(IRQ_KEYBOARD);
}

/* Reads one byte from an I/O port. */
static uint8_t
port_in8(
	uint16_t port)
{
	uint8_t value;

	/* Reads the selected port through an ordered instruction. */
	__asm__ volatile("inb %w1, %0" : "=a" (value) : "Nd" (port));

	/* Returns the sampled port byte. */
	return value;
}

/* Writes one byte to an I/O port. */
static void
port_out8(
	uint16_t port,
	uint8_t value)
{
	/* Writes the supplied byte through an ordered instruction. */
	__asm__ volatile("outb %0, %w1" : : "a" (value), "Nd" (port));
}

/* Writes one uPD7220 command or parameter byte when FIFO space appears. */
static int
gdc_write(
	uint16_t port,
	uint8_t value)
{
	uint8_t status;
	unsigned timeout;

	/* Polls the uPD7220 FIFO through the bounded legacy timeout. */
	for (timeout = 100000U; timeout != 0U; timeout--) {
		/* Samples capacity and stops when the FIFO accepts another byte. */
		status = port_in8(0x60U);
		if ((status & 0x02U) == 0U)
			break;
	}

	/* Reports failure without issuing a byte after timeout. */
	if (timeout == 0U)
		return 0;

	/* Writes the byte after observing FIFO capacity. */
	port_out8(port, value);

	/* Reports one completed port write. */
	return 1;
}

/* Removes a drawn two-cell software cursor. */
static void
software_cursor_remove(
	void)
{
	/* Leaves attributes untouched when no software cursor is drawn. */
	if (!software_cursor_drawn)
		return;

	/* Restores both saved attributes before clearing draw state. */
	attribute_vram[software_cursor_offset * 2U] =
	    software_cursor_attribute[0];
	attribute_vram[(software_cursor_offset + 1U) * 2U] =
	    software_cursor_attribute[1];
	software_cursor_drawn = 0;
}

/* Writes one PC-98 text and attribute cell. */
static void
write_cell(
	unsigned row,
	unsigned column,
	uint16_t code,
	uint8_t attribute)
{
	unsigned offset;

	/* Computes the target text-memory offset. */
	offset = row * HAL_CONS_COLUMNS + column;

	/* Suppresses video-memory writes during graphics ownership. */
	if (console_suspended)
		return;

	/* Removes a double-cell cursor that may cover this update. */
	software_cursor_remove();

	/* Publishes text before its corresponding attribute. */
	text_vram[offset] = code;
	attribute_vram[offset * 2U] = attribute;
}

/* Scrolls the PC-98 text console upward by one row. */
static void
scroll(
	void)
{
	unsigned row;
	unsigned column;
	unsigned destination;
	unsigned source;

	/* Copies visible rows upward while text memory is available. */
	if (!console_suspended) {
		/* Visits every visible destination row in display order. */
		for (row = 0; row + 1U < HAL_CONS_ROWS; row++) {
			/* Copies every text and attribute cell from the next row. */
			for (column = 0; column < HAL_CONS_COLUMNS; column++) {
				destination = row * HAL_CONS_COLUMNS + column;
				source = destination + HAL_CONS_COLUMNS;
				text_vram[destination] = text_vram[source];
				attribute_vram[destination * 2U] =
				    attribute_vram[source * 2U];
			}
		}
	}

	/* Clears the final row through the normal cell writer. */
	hal_cons_clear_row(HAL_CONS_ROWS - 1U);

	/* Leaves the cursor on the final visible row. */
	cursor_row = HAL_CONS_ROWS - 1U;
}

/* Advances the PC-98 cursor to the next text row. */
static void
newline(
	void)
{
	/* Advances to the next row after returning to column zero. */
	cursor_column = 0;
	cursor_row++;

	/* Stops while the advanced row remains visible. */
	if (cursor_row < HAL_CONS_ROWS)
		return;

	/* Scrolls terminal output or clips fixed-menu output. */
	if (console_mode == HAL_CONS_TERMINAL) {
		scroll();
	} else {
		cursor_row = HAL_CONS_ROWS - 1U;
	}
}

/* Writes one single-cell PC-98 character. */
static void
put_single_cell(
	uint16_t code)
{
	/* Wraps before writing when the cursor lies beyond the row. */
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline();

	/* Writes and advances past one normal attributed cell. */
	write_cell(
		cursor_row,
		cursor_column,
		code,
		HAL_CONS_NORMAL_ATTRIBUTE);
	cursor_column++;
}

/* Decodes one UTF-8 code point or substitutes an invalid byte. */
static uint32_t
utf8_codepoint(
	const char **input,
	const char *end)
{
	const uint8_t *p;
	uint32_t codepoint;
	unsigned count;
	unsigned index;

	/* Loads the current input byte position. */
	p = (const uint8_t *)*input;

	/* Reports the sentinel code point at the range end. */
	if ((const char *)p >= end)
		return 0;

	/* Consumes and returns one ASCII byte directly. */
	if (p[0] < 0x80U) {
		*input = (const char *)(p + 1);
		return p[0];
	}

	/* Determines the encoded width and initial payload bits. */
	if ((p[0] & 0xe0U) == 0xc0U) {
		codepoint = p[0] & 0x1fU;
		count = 2;

		/* Rejects the overlong two-byte lead-byte range. */
		if (codepoint < 2U)
			count = 0;
	} else if ((p[0] & 0xf0U) == 0xe0U) {
		codepoint = p[0] & 0x0fU;
		count = 3;
	} else if ((p[0] & 0xf8U) == 0xf0U) {
		codepoint = p[0] & 0x07U;
		count = 4;

		/* Rejects lead bytes above the Unicode scalar range. */
		if (codepoint > 4U)
			count = 0;
	} else {
		codepoint = 0;
		count = 0;
	}

	/* Consumes one invalid lead byte for an impossible or short sequence. */
	if (count == 0U || end - (const char *)p < (int)count) {
		*input = (const char *)(p + 1);
		return '?';
	}

	/* Accumulates every validated continuation-byte payload. */
	for (index = 1U; index < count; index++) {
		/* Substitutes the lead byte when this continuation is malformed. */
		if ((p[index] & 0xc0U) != 0x80U) {
			*input = (const char *)(p + 1);
			return '?';
		}
		codepoint = (codepoint << 6) | (p[index] & 0x3fU);
	}

	/* Publishes the position after the complete byte sequence. */
	*input = (const char *)(p + count);

	/* Substitutes overlong, surrogate, and out-of-range scalars. */
	if ((count == 2U && codepoint < 0x80U) ||
	    (count == 3U && codepoint < 0x800U) ||
	    (count == 4U && codepoint < 0x10000U) ||
	    codepoint > 0x10ffffU ||
	    (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
		return '?';
	}

	/* Returns the validated Unicode scalar. */
	return codepoint;
}

/* Maps one Unicode scalar to PC-98 text code and cell width. */
static uint16_t
unicode_to_pc98(
	uint32_t codepoint,
	unsigned *width)
{
	unsigned index;
	uint16_t ku;
	uint16_t ten;

	/* Maps ASCII directly to one text cell. */
	if (codepoint < 0x80U) {
		*width = 1;
		return (uint16_t)codepoint;
	}

	/* Maps half-width katakana directly to one text cell. */
	if (codepoint >= 0xff61U && codepoint <= 0xff9fU) {
		*width = 1;
		return (uint16_t)(0xa1U + codepoint - 0xff61U);
	}

	/* Searches BMP scalars in the HAL-owned JIS X 0208 table. */
	if (codepoint <= 0xffffU) {
		/* Visits each retained JIS table entry in row-major order. */
		for (index = 0; index < 7896U; index++) {
			/* Skips entries that do not represent the requested scalar. */
			if (hal_pc98_jisx0208_to_ucs[index] != codepoint)
				continue;

			/* Encodes the matching JIS row and cell for PC-98 VRAM. */
			ku = (uint16_t)(0x21U + index / 94U);
			ten = (uint16_t)(0x21U + index % 94U);
			*width = 2;

			/* Returns the PC-98 byte ordering for this wide glyph. */
			return (uint16_t)((ten << 8) | (ku - 0x20U));
		}
	}

	/* Substitutes an unmapped scalar with one question-mark cell. */
	*width = 1;
	return '?';
}

/* Maps one special PC-98 scan position to its legacy key value. */
static int
special_key(
	uint8_t scan)
{
	int key;

	/* Selects the supported navigation or function key. */
	key = 0;
	switch (scan) {
	case 0x00:
		key = HAL_KEY_ESCAPE;
		break;
	case 0x0e:
		key = HAL_KEY_BACKSPACE;
		break;
	case 0x0f:
		key = HAL_KEY_TAB;
		break;
	case 0x1c:
		key = HAL_KEY_ENTER;
		break;
	case 0x36:
		key = HAL_KEY_PAGE_UP;
		break;
	case 0x37:
		key = HAL_KEY_PAGE_DOWN;
		break;
	case 0x38:
		key = HAL_KEY_INSERT;
		break;
	case 0x39:
		key = HAL_KEY_DELETE;
		break;
	case 0x3a:
		key = HAL_KEY_UP;
		break;
	case 0x3b:
		key = HAL_KEY_LEFT;
		break;
	case 0x3c:
		key = HAL_KEY_RIGHT;
		break;
	case 0x3d:
		key = HAL_KEY_DOWN;
		break;
	case 0x3e:
		key = HAL_KEY_HOME;
		break;
	case 0x3f:
		key = HAL_KEY_END;
		break;
	case 0x62:
		key = HAL_KEY_F1;
		break;
	case 0x63:
		key = HAL_KEY_F2;
		break;
	case 0x64:
		key = HAL_KEY_F3;
		break;
	case 0x65:
		key = HAL_KEY_F4;
		break;
	case 0x66:
		key = HAL_KEY_F5;
		break;
	case 0x67:
		key = HAL_KEY_F6;
		break;
	case 0x68:
		key = HAL_KEY_F7;
		break;
	case 0x69:
		key = HAL_KEY_F8;
		break;
	case 0x6a:
		key = HAL_KEY_F9;
		break;
	case 0x6b:
		key = HAL_KEY_F10;
		break;
	default:
		break;
	}

	/* Returns the mapped key or the unsupported-scan sentinel. */
	return key;
}

/* Maps one legacy key value to its PC-98 scan position. */
static int
key_to_scan(
	int key)
{
	unsigned scan;
	int mapped_scan;

	/* Rejects non-key values. */
	if (key <= 0)
		return -1;

	/* Normalizes upper-case letters through the base table. */
	if (key >= 'A' && key <= 'Z')
		key = key - 'A' + 'a';

	/* Resolves printable JIS aliases to their physical positions. */
	mapped_scan = -1;
	switch (key) {
	case '1':
		mapped_scan = 0x01;
		break;
	case '2':
		mapped_scan = 0x02;
		break;
	case '3':
		mapped_scan = 0x03;
		break;
	case '4':
		mapped_scan = 0x04;
		break;
	case '5':
		mapped_scan = 0x05;
		break;
	case '6':
		mapped_scan = 0x06;
		break;
	case '7':
		mapped_scan = 0x07;
		break;
	case '8':
		mapped_scan = 0x08;
		break;
	case '9':
		mapped_scan = 0x09;
		break;
	case '0':
		mapped_scan = 0x0a;
		break;
	case '-':
		mapped_scan = 0x0b;
		break;
	case '^':
		mapped_scan = 0x0c;
		break;
	case '\\':
		mapped_scan = 0x0d;
		break;
	case '@':
		mapped_scan = 0x1a;
		break;
	case '[':
		mapped_scan = 0x1b;
		break;
	case ';':
		mapped_scan = 0x26;
		break;
	case ':':
		mapped_scan = 0x27;
		break;
	case ']':
		mapped_scan = 0x28;
		break;
	case ',':
		mapped_scan = 0x30;
		break;
	case '.':
		mapped_scan = 0x31;
		break;
	case '/':
		mapped_scan = 0x32;
		break;
	default:
		break;
	}

	/* Returns a printable-key scan without probing later maps. */
	if (mapped_scan >= 0)
		return mapped_scan;

	/* Resolves control and navigation aliases to physical positions. */
	switch (key) {
	case HAL_KEY_ESCAPE:
		mapped_scan = 0x00;
		break;
	case HAL_KEY_BACKSPACE:
		mapped_scan = 0x0e;
		break;
	case HAL_KEY_TAB:
		mapped_scan = 0x0f;
		break;
	case HAL_KEY_ENTER:
		mapped_scan = 0x1c;
		break;
	case HAL_KEY_INSERT:
		mapped_scan = 0x38;
		break;
	case HAL_KEY_DELETE:
		mapped_scan = 0x39;
		break;
	case HAL_KEY_UP:
		mapped_scan = 0x3a;
		break;
	case HAL_KEY_LEFT:
		mapped_scan = 0x3b;
		break;
	case HAL_KEY_RIGHT:
		mapped_scan = 0x3c;
		break;
	case HAL_KEY_DOWN:
		mapped_scan = 0x3d;
		break;
	case HAL_KEY_HOME:
		mapped_scan = 0x3e;
		break;
	case HAL_KEY_END:
		mapped_scan = 0x3f;
		break;
	case HAL_KEY_PAGE_UP:
		mapped_scan = 0x36;
		break;
	case HAL_KEY_PAGE_DOWN:
		mapped_scan = 0x37;
		break;
	case HAL_KEY_SHIFT_L:
		mapped_scan = SCAN_SHIFT_L;
		break;
	case HAL_KEY_SHIFT_R:
		mapped_scan = SCAN_SHIFT_R;
		break;
	case HAL_KEY_CTRL:
		mapped_scan = SCAN_CTRL;
		break;
	case HAL_KEY_GRAPH:
		mapped_scan = SCAN_GRAPH;
		break;
	case HAL_KEY_CAPS_LOCK:
		mapped_scan = SCAN_CAPS;
		break;
	case HAL_KEY_KANA:
		mapped_scan = SCAN_KANA;
		break;
	default:
		break;
	}

	/* Returns a mapped control-key scan before scanning base entries. */
	if (mapped_scan >= 0)
		return mapped_scan;

	/* Maps the contiguous function-key range. */
	if (key >= HAL_KEY_F1 && key <= HAL_KEY_F10)
		return 0x62 + (key - HAL_KEY_F1);

	/* Searches base entries for a letter or other literal key. */
	for (scan = 0; scan < SCAN_MAX; scan++) {
		/* Returns the first physical scan carrying this base value. */
		if (base_table[scan] == (uint8_t)key)
			return (int)scan;
	}

	/* Reports an unsupported legacy key value. */
	return -1;
}

/* Changes one PC-98 held-key bitmap position. */
static void
set_down(
	struct pc98_keyboard *kb,
	uint8_t scan,
	int pressed)
{
	uint8_t mask;

	/* Computes the bit mask for this physical scan position. */
	mask = (uint8_t)(1U << (scan & 7U));

	/* Sets or clears the held-key bit. */
	if (pressed) {
		kb->down[scan >> 3] |= mask;
	} else {
		kb->down[scan >> 3] &= (uint8_t)~mask;
	}
}

/* Maps one modifier scan position to its legacy key value. */
static int
modifier_key(
	uint8_t scan)
{
	int key;

	/* Selects the supported modifier or lock key. */
	key = 0;
	switch (scan) {
	case SCAN_SHIFT_L:
		key = HAL_KEY_SHIFT_L;
		break;
	case SCAN_SHIFT_R:
		key = HAL_KEY_SHIFT_R;
		break;
	case SCAN_CTRL:
		key = HAL_KEY_CTRL;
		break;
	case SCAN_GRAPH:
		key = HAL_KEY_GRAPH;
		break;
	case SCAN_CAPS:
		key = HAL_KEY_CAPS_LOCK;
		break;
	case SCAN_KANA:
		key = HAL_KEY_KANA;
		break;
	default:
		break;
	}

	/* Returns the mapped key or the unsupported-scan sentinel. */
	return key;
}

/* Reports one PC-98 held-key bitmap position. */
static int
pc98_keyboard_scan_down(
	const struct pc98_keyboard *kb,
	uint8_t scan)
{
	/* Returns the selected physical scan bit. */
	return (kb->down[scan >> 3] >> (scan & 7U)) & 1U;
}

/* Builds the private modifier flags for one PC-98 event. */
static unsigned
pc98_keyboard_modifiers(
	const struct pc98_keyboard *kb)
{
	unsigned modifiers;

	/* Combines current Shift, Control, and Graph state. */
	modifiers = (kb->shift ? HAL_KEY_EVENT_SHIFT_PRIVATE : 0U) |
	    (kb->ctrl ? HAL_KEY_EVENT_CTRL_PRIVATE : 0U) |
	    (kb->graph ? HAL_KEY_EVENT_GRAPH_PRIVATE : 0U);

	/* Returns the combined private modifier flags. */
	return modifiers;
}

/* Maps one legacy PC-98 key value to its stable symbol. */
static const char *
special_symbol(
	unsigned key)
{
	static const char *const function[] = {
		"f1", "f2", "f3", "f4", "f5",
		"f6", "f7", "f8", "f9", "f10"
	};
	const char *symbol;

	/* Resolves the contiguous function-key range first. */
	if (key >= HAL_KEY_F1 && key <= HAL_KEY_F10)
		return function[key - HAL_KEY_F1];

	/* Selects the supported control, navigation, or JIS symbol. */
	symbol = NULL;
	switch (key) {
	case HAL_KEY_ESCAPE:
		symbol = "esc";
		break;
	case HAL_KEY_BACKSPACE:
		symbol = "backspace";
		break;
	case HAL_KEY_TAB:
		symbol = "tab";
		break;
	case HAL_KEY_ENTER:
		symbol = "enter";
		break;
	case HAL_KEY_PAGE_UP:
		symbol = "pageup";
		break;
	case HAL_KEY_PAGE_DOWN:
		symbol = "pagedown";
		break;
	case HAL_KEY_INSERT:
		symbol = "insert";
		break;
	case HAL_KEY_DELETE:
		symbol = "delete";
		break;
	case HAL_KEY_UP:
		symbol = "up";
		break;
	case HAL_KEY_LEFT:
		symbol = "left";
		break;
	case HAL_KEY_RIGHT:
		symbol = "right";
		break;
	case HAL_KEY_DOWN:
		symbol = "down";
		break;
	case HAL_KEY_HOME:
		symbol = "home";
		break;
	case HAL_KEY_END:
		symbol = "end";
		break;
	case HAL_KEY_SHIFT_L:
		symbol = "leftshift";
		break;
	case HAL_KEY_SHIFT_R:
		symbol = "rightshift";
		break;
	case HAL_KEY_CTRL:
		symbol = "leftctrl";
		break;
	case HAL_KEY_GRAPH:
		symbol = "leftalt";
		break;
	case HAL_KEY_CAPS_LOCK:
		symbol = "capslock";
		break;
	case HAL_KEY_KANA:
		symbol = "kana";
		break;
	case HAL_KEY_JIS_1:
		symbol = "jis-1";
		break;
	case HAL_KEY_JIS_2:
		symbol = "jis-2";
		break;
	case HAL_KEY_JIS_3:
		symbol = "jis-3";
		break;
	case HAL_KEY_JIS_4:
		symbol = "jis-4";
		break;
	case HAL_KEY_JIS_5:
		symbol = "jis-5";
		break;
	case HAL_KEY_JIS_6:
		symbol = "jis-6";
		break;
	case HAL_KEY_JIS_7:
		symbol = "jis-7";
		break;
	case HAL_KEY_JIS_8:
		symbol = "jis-8";
		break;
	case HAL_KEY_JIS_9:
		symbol = "jis-9";
		break;
	case HAL_KEY_JIS_0:
		symbol = "jis-0";
		break;
	case HAL_KEY_JIS_MINUS:
		symbol = "jis-minus";
		break;
	case HAL_KEY_JIS_CARET:
		symbol = "jis-caret";
		break;
	case HAL_KEY_JIS_YEN:
		symbol = "jis-yen";
		break;
	case HAL_KEY_JIS_AT:
		symbol = "jis-at";
		break;
	case HAL_KEY_JIS_LBRACE:
		symbol = "jis-lbrace";
		break;
	case HAL_KEY_JIS_SEMI:
		symbol = "jis-semi";
		break;
	case HAL_KEY_JIS_COLON:
		symbol = "jis-colon";
		break;
	case HAL_KEY_JIS_RBRACE:
		symbol = "jis-rbrace";
		break;
	case HAL_KEY_JIS_COMMA:
		symbol = "jis-comma";
		break;
	case HAL_KEY_JIS_DOT:
		symbol = "jis-dot";
		break;
	case HAL_KEY_JIS_SLASH:
		symbol = "jis-slash";
		break;
	case HAL_KEY_JIS_RO:
		symbol = "jis-ro";
		break;
	default:
		break;
	}

	/* Returns the mapped symbol or the unsupported-key marker. */
	return symbol;
}

/* Converts one private legacy event to the public event representation. */
static int
event_from_legacy(
	struct hal_key_event *event,
	unsigned legacy)
{
	const char *symbol;
	unsigned key;
	unsigned index;
	char character[2];

	/* Resolves the private key value to a named or literal symbol. */
	key = legacy & HAL_KEY_EVENT_KEY_MASK;
	index = 0;
	symbol = special_symbol(key);
	if (symbol == NULL && key <= 0xffU && key != 0U) {
		character[0] = (char)key;
		character[1] = '\0';
		symbol = character;
	}

	/* Rejects a private event without a public symbol. */
	if (symbol == NULL)
		return 0;

	/* Copies as much of the terminated symbol as the event can hold. */
	while (index + 1U < HAL_KEY_SYMBOL_SIZE && symbol[index] != '\0') {
		event->symbol[index] = symbol[index];
		index++;
	}

	/* Terminates and clears the remainder of the symbol buffer. */
	while (index < HAL_KEY_SYMBOL_SIZE) {
		event->symbol[index] = '\0';
		index++;
	}

	/* Converts private make, break, and repeat state to public flags. */
	if ((legacy & HAL_KEY_EVENT_RELEASE_PRIVATE) != 0U) {
		event->flags = HAL_KEY_EVENT_RELEASE;
	} else if ((legacy & HAL_KEY_EVENT_REPEAT_PRIVATE) != 0U) {
		event->flags = HAL_KEY_EVENT_REPEAT;
	} else {
		event->flags = HAL_KEY_EVENT_PRESS;
	}

	/* Reports one converted event. */
	return 1;
}

/* Tests whether one key receives snapshot modifier priority. */
static int
snapshot_modifier(
	unsigned key)
{
	/* Reports modifier and lock keys in their existing test order. */
	if (key == HAL_KEY_SHIFT_L || key == HAL_KEY_SHIFT_R)
		return 1;
	else if (key == HAL_KEY_CTRL)
		return 1;
	else if (key == HAL_KEY_GRAPH)
		return 1;
	else if (key == HAL_KEY_CAPS_LOCK)
		return 1;
	else if (key == HAL_KEY_KANA)
		return 1;

	/* Reports an ordinary non-modifier key. */
	return 0;
}

/* Rebuilds a truthful PC-98 held-key snapshot after ring overflow. */
static void
rebuild_keyboard_events_locked(
	void)
{
	unsigned pass;
	unsigned scan;
	unsigned key;
	int down;
	int converted;
	int modifier;

	/* Starts a fresh ring with the current lock-state resync marker. */
	tail = 0;
	head = 0;
	memset(&events[head], 0, sizeof(events[head]));
	events[head].flags = HAL_KEY_EVENT_RESYNC |
	    (keyboard.caps ? HAL_KEY_EVENT_LOCK_CAPS : 0U) |
	    (keyboard.kana ? HAL_KEY_EVENT_LOCK_KANA : 0U);
	head = (head + 1U) % QUEUE_SIZE;

	/* Emits modifiers before ordinary held keys for a truthful snapshot. */
	for (pass = 0; pass < 2U; pass++) {
		/* Visits every physical scan during each priority pass. */
		for (scan = 0; scan < SCAN_MAX; scan++) {
			/* Samples the held state and retained identity for this scan. */
			key = keyboard.last_key[scan];
			down = pc98_keyboard_scan_down(&keyboard, (uint8_t)scan);
			if (!down) {
				continue;
			}

			/* Skips held scans that lack an emitted-key identity. */
			if (key == 0U)
				continue;

			/* Filters this held key into its priority pass. */
			modifier = snapshot_modifier(key);
			if (modifier != (pass == 0U))
				continue;

			/* Converts the held key to its public snapshot event. */
			converted = event_from_legacy(&events[head], key);
			if (!converted)
				continue;

			/* Appends the converted held-key snapshot. */
			events[head].flags = HAL_KEY_EVENT_PRESS |
			    HAL_KEY_EVENT_SNAPSHOT;
			head = (head + 1U) % QUEUE_SIZE;
		}
	}

	/* Terminates the rebuilt snapshot with a resync-end marker. */
	memset(&events[head], 0, sizeof(events[head]));
	events[head].flags = HAL_KEY_EVENT_RESYNC_END;
	head = (head + 1U) % QUEUE_SIZE;
}

/* Translates and enqueues one raw PC-98 keyboard scan. */
static void
enqueue_raw_locked(
	uint8_t raw)
{
	unsigned event;
	unsigned next;
	int translated;
	int converted;

	/* Translates the raw scan into a private event. */
	translated = pc98_keyboard_feed(&keyboard, raw, &event);
	if (!translated)
		return;

	/* Replaces an overflowing event stream with a state snapshot. */
	next = (head + 1U) % QUEUE_SIZE;
	if (next == tail) {
		rebuild_keyboard_events_locked();
		return;
	}

	/* Converts the private event into the public ring representation. */
	converted = event_from_legacy(&events[head], event);
	if (converted)
		head = next;
}

/* Reads one PC-98 keyboard-controller byte. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t value;

	/* Reads the selected port through an ordered instruction. */
	__asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));

	/* Returns the sampled port byte. */
	return value;
}

/* Acquires the PC-98 input wait-queue lock. */
static bool
input_lock_acquire(
	void)
{
	bool enabled;

	/* Acquires the lock and records prior interrupt state. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);

	/* Returns the saved interrupt state. */
	return enabled;
}

/* Releases the PC-98 input wait-queue lock. */
static void
input_lock_release(
	bool enabled)
{
	/* Releases the lock and restores prior interrupt state. */
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
}

/* Drains available PC-98 keyboard-controller bytes into the event ring. */
static void
pump_locked(
	void)
{
	uint8_t raw;
	uint8_t status;

	/* Reads and enqueues every currently available keyboard byte. */
	for (;;) {
		/* Samples the controller and stops when no receive byte remains. */
		status = inb(KBD_STATUS);
		if ((status & KBD_RXRDY) == 0U)
			break;

		/* Consumes and translates the pending controller byte. */
		raw = inb(KBD_DATA);
		enqueue_raw_locked(raw);
	}
}

/* Services one PC-98 keyboard interrupt. */
static void
keyboard_interrupt(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	struct hal_cons_wait_entry *waiters;
	bool enabled;

	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(argument);

	/* Pumps input and detaches waiters under the queue lock. */
	waiters = NULL;
	enabled = input_lock_acquire();
	pump_locked();
	if (tail != head)
		waiters = hal_cons_wait_queue_detach_all(&input_waiters);
	input_lock_release(enabled);

	/* Wakes detached tasks before acknowledging the interrupt. */
	hal_cons_wait_queue_notify_all(waiters);
	hal_irq_send_eoi(acknowledge);
}

/* Maps one stable PC-98 symbol to its legacy character value. */
static int
legacy_character(
	const char *symbol)
{
	/* Maps the exact JIS aliases in their existing priority order. */
	if (strcmp(symbol, "jis-1") == 0)
		return '1';
	else if (strcmp(symbol, "jis-2") == 0)
		return '2';
	else if (strcmp(symbol, "jis-3") == 0)
		return '3';
	else if (strcmp(symbol, "jis-4") == 0)
		return '4';
	else if (strcmp(symbol, "jis-5") == 0)
		return '5';
	else if (strcmp(symbol, "jis-6") == 0)
		return '6';
	else if (strcmp(symbol, "jis-7") == 0)
		return '7';
	else if (strcmp(symbol, "jis-8") == 0)
		return '8';
	else if (strcmp(symbol, "jis-9") == 0)
		return '9';
	else if (strcmp(symbol, "jis-0") == 0)
		return '0';
	else if (strcmp(symbol, "jis-minus") == 0)
		return '-';
	else if (strcmp(symbol, "jis-caret") == 0)
		return '^';
	else if (strcmp(symbol, "jis-yen") == 0)
		return '\\';
	else if (strcmp(symbol, "jis-at") == 0)
		return '@';
	else if (strcmp(symbol, "jis-lbrace") == 0)
		return '[';
	else if (strcmp(symbol, "jis-semi") == 0)
		return ';';
	else if (strcmp(symbol, "jis-colon") == 0)
		return ':';
	else if (strcmp(symbol, "jis-rbrace") == 0)
		return ']';
	else if (strcmp(symbol, "jis-comma") == 0)
		return ',';
	else if (strcmp(symbol, "jis-dot") == 0)
		return '.';
	else if (strcmp(symbol, "jis-slash") == 0)
		return '/';
	else if (strcmp(symbol, "jis-ro") == 0)
		return '\\';

	/* Preserves the historical two-byte control-symbol tests. */
	if (symbol[0] == 'e' && symbol[1] == 'n')
		return '\r';
	else if (symbol[0] == 't' && symbol[1] == 'a')
		return '\t';
	else if (symbol[0] == 'b' && symbol[1] == 'a')
		return '\b';
	else if (symbol[0] == 'e' && symbol[1] == 's')
		return 0x1b;

	/* Reports a symbol without a supported legacy character. */
	return -1;
}
