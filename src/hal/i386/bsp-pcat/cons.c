/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT VGA text-console and 8042 keyboard implementation.
 */

#include <hal/hal.h>

#include <string.h>

#include "../../cons-wait.h"
#include "../asm.h"
#include "../defs.h"
#include "../i386.h"
#include "../irq.h"

#define VGA_MEMORY ((volatile uint16_t *)(SYS_START + 0x000b8000U))
#define VGA_INDEX 0x3d4U
#define VGA_DATA 0x3d5U
#define KBD_DATA 0x60U
#define KBD_STATUS 0x64U
#define KBD_STATUS_AUX 0x20U

/* Includes 256 scan positions, two resync markers, and one ring sentinel. */
#define EVENT_COUNT 259U

static const char *const scan_symbols[128] = {
	[0x01] = "esc", [0x02] = "1", [0x03] = "2", [0x04] = "3",
	[0x05] = "4", [0x06] = "5", [0x07] = "6", [0x08] = "7",
	[0x09] = "8", [0x0a] = "9", [0x0b] = "0", [0x0c] = "minus",
	[0x0d] = "equal", [0x0e] = "backspace", [0x0f] = "tab",
	[0x10] = "q", [0x11] = "w", [0x12] = "e", [0x13] = "r",
	[0x14] = "t", [0x15] = "y", [0x16] = "u", [0x17] = "i",
	[0x18] = "o", [0x19] = "p", [0x1a] = "leftbrace",
	[0x1b] = "rightbrace", [0x1c] = "enter", [0x1d] = "leftctrl",
	[0x1e] = "a", [0x1f] = "s", [0x20] = "d", [0x21] = "f",
	[0x22] = "g", [0x23] = "h", [0x24] = "j", [0x25] = "k",
	[0x26] = "l", [0x27] = "semicolon", [0x28] = "apostrophe",
	[0x29] = "grave", [0x2a] = "leftshift", [0x2b] = "backslash",
	[0x2c] = "z", [0x2d] = "x", [0x2e] = "c", [0x2f] = "v",
	[0x30] = "b", [0x31] = "n", [0x32] = "m", [0x33] = "comma",
	[0x34] = "dot", [0x35] = "slash", [0x36] = "rightshift",
	[0x38] = "leftalt", [0x39] = "space", [0x3a] = "capslock",
	[0x3b] = "f1", [0x3c] = "f2", [0x3d] = "f3", [0x3e] = "f4",
	[0x3f] = "f5", [0x40] = "f6", [0x41] = "f7", [0x42] = "f8",
	[0x43] = "f9", [0x44] = "f10",
};

static unsigned cursor_row;
static unsigned cursor_column;
static uint8_t current_attribute = 0x07U;
static int cursor_visible = 1;
static int console_suspended;
static enum hal_cons_mode console_mode = HAL_CONS_TERMINAL;
static struct hal_key_event events[EVENT_COUNT];
static unsigned event_head;
static unsigned event_tail;
static uint8_t key_down[32];
static int shift_down;
static int ctrl_down;
static int alt_down;
static int caps_lock;
static int e0_prefix;
static struct hal_cons_wait_queue input_waiters;

static void write_cell(unsigned row, unsigned column, int character, uint8_t attribute);
static void scroll(void);
static void newline(void);
static const char *scan_symbol(uint8_t scan, int extended);
static void set_event(struct hal_key_event *event, const char *symbol, uint32_t flags);
static int symbol_equal(const char *left, const char *right);
static void rebuild_keyboard_events_locked(void);
static void enqueue_keyboard_event_locked(const char *symbol, uint32_t flags);
static int snapshot_modifier(const char *symbol);
static void pump_keyboard_locked(void);
static void keyboard_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument);

/*
 * Updates the PC/AT VGA hardware cursor.
 */
void
hal_cons_update_cursor(
	void)
{
	unsigned position;

	/* Computes the current text-memory cell before checking suspension. */
	position = cursor_row * HAL_CONS_COLUMNS + cursor_column;

	/* Suppresses hardware-cursor updates during graphics ownership. */
	if (console_suspended)
		return;

	/* Programs visibility and the split cursor position in register order. */
	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, cursor_visible ? 0x0dU : 0x20U);
	asm_outb(VGA_INDEX, 0x0eU);
	asm_outb(VGA_DATA, (uint8_t)(position >> 8));
	asm_outb(VGA_INDEX, 0x0fU);
	asm_outb(VGA_DATA, (uint8_t)position);
}

/*
 * Clears one PC/AT text-console row.
 */
void
hal_cons_clear_row(
	unsigned row)
{
	unsigned column;

	/* Ignores rows outside the visible text grid. */
	if (row >= HAL_CONS_ROWS)
		return;

	/* Fills every cell with a blank in the current attribute. */
	for (column = 0; column < HAL_CONS_COLUMNS; column++)
		write_cell(row, column, ' ', current_attribute);
}

/*
 * Clears the complete PC/AT text console.
 */
void
hal_cons_clear(
	void)
{
	unsigned row;

	/* Clears every visible row. */
	for (row = 0; row < HAL_CONS_ROWS; row++)
		hal_cons_clear_row(row);

	/* Homes and publishes the cursor. */
	cursor_column = 0;
	cursor_row = 0;
	hal_cons_update_cursor();
}

/*
 * Resets the PC/AT console to its terminal defaults.
 */
void
hal_cons_reset(
	void)
{
	/* Restores the default attribute, mode, and visible cursor. */
	current_attribute = 0x07U;
	console_mode = HAL_CONS_TERMINAL;
	cursor_visible = 1;

	/* Clears the display and publishes its home position. */
	hal_cons_clear();
}

/*
 * Writes one character to the PC/AT text console.
 */
void
hal_cons_putc(
	int character)
{
	int display_character;

	/* Mirrors every byte to the optional emulator debug console. */
#ifdef HAL_PCAT_DEBUGCON
	asm_outb(0xe9U, (uint8_t)character);
#endif

	/* Handles newline as a cursor operation. */
	if (character == '\n') {
		newline();
		return;
	}

	/* Handles carriage return without advancing rows. */
	if (character == '\r') {
		cursor_column = 0;
		hal_cons_update_cursor();
		return;
	}

	/* Erases one cell for a backspace within the current row. */
	if (character == '\b') {
		/* Moves left only when the cursor is not already at column zero. */
		if (cursor_column != 0)
			cursor_column--;
		write_cell(cursor_row, cursor_column, ' ', current_attribute);
		hal_cons_update_cursor();
		return;
	}

	/* Expands a tab through the next eight-column stop. */
	if (character == '\t') {
		do {
			hal_cons_putc(' ');
		} while ((cursor_column & 7U) != 0);
		return;
	}

	/* Wraps before writing when the cursor already lies past the row. */
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline();

	/* Replaces non-printable bytes and writes the selected text cell. */
	display_character =
	    character >= 0x20 && character < 0x7f ? character : '?';
	write_cell(
		cursor_row,
		cursor_column++,
		display_character,
		current_attribute);

	/* Wraps a full row or publishes the terminal cursor. */
	if (cursor_column >= HAL_CONS_COLUMNS) {
		newline();
	} else if (console_mode == HAL_CONS_TERMINAL) {
		hal_cons_update_cursor();
	}
}

/*
 * Writes a bounded byte string to the PC/AT text console.
 */
void
hal_cons_write_n(
	const char *string,
	unsigned length)
{
	uint8_t byte;
	unsigned index;

	/* Ignores a missing string. */
	if (string == NULL)
		return;

	/* Emits ASCII and substitutes one marker for each UTF-8 sequence. */
	index = 0;
	while (index < length) {
		byte = (uint8_t)string[index];
		index++;

		/* Emits ASCII directly or substitutes one non-ASCII sequence. */
		if (byte < 0x80U) {
			hal_cons_putc(byte);
		} else {
			/* Skips every continuation byte belonging to this sequence. */
			while (index < length &&
			    ((uint8_t)string[index] & 0xc0U) == 0x80U) {
				index++;
			}
			hal_cons_putc('?');
		}
	}
}

/*
 * Writes one terminated string to the PC/AT text console.
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
 * Writes a bounded string at one PC/AT text-grid position.
 */
int
hal_cons_write_n_at(
	unsigned row,
	unsigned column,
	const char *string,
	unsigned length,
	uint8_t attribute)
{
	uint8_t character;
	uint8_t cell_attribute;
	unsigned changed;
	unsigned index;

	/* Requires a visible origin and source string. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS ||
	    string == NULL) {
		return -1;
	}

	/* Selects the explicit or default cell attribute. */
	cell_attribute = attribute != 0U ? attribute : 0x07U;
	changed = 0;

	/* Writes each byte while the destination remains on screen. */
	for (index = 0;
	     index < length && row < HAL_CONS_ROWS;
	     index++) {
		character = (uint8_t)string[index];

		/* Advances to the next row for a newline byte. */
		if (character == '\n') {
			row++;
			column = 0;
			continue;
		}

		/* Returns to the current row origin for carriage return. */
		if (character == '\r') {
			column = 0;
			continue;
		}

		/* Replaces non-ASCII bytes before reaching video memory. */
		if (character >= 0x80U)
			character = '?';

		/* Stops rather than wrapping an explicitly positioned write. */
		if (column >= HAL_CONS_COLUMNS)
			break;

		/* Writes and accounts for one visible cell. */
		write_cell(row, column++, character, cell_attribute);
		changed++;
	}

	/* Clips and stores the final logical cursor position. */
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ?
	    column : HAL_CONS_COLUMNS - 1U;

	/* Returns the number of changed cells. */
	return (int)changed;
}

/*
 * Writes one attributed string at a PC/AT text-grid position.
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

	/* Requires a source string. */
	if (string == NULL)
		return -1;

	/* Measures the string through its terminating byte. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured string with the selected attribute. */
	result = hal_cons_write_n_at(row, column, string, length, attribute);

	/* Returns the positioned-write result. */
	return result;
}

/*
 * Writes one string at a PC/AT text-grid position.
 */
void
hal_cons_write_at(
	unsigned row,
	unsigned column,
	const char *string)
{
	/* Writes with the current console attribute. */
	(void)hal_cons_write_at_attr(row, column, string, current_attribute);
}

/*
 * Clears from the current PC/AT cursor through the row end.
 */
void
hal_cons_clear_to_eol(
	void)
{
	/* Clears from the stored cursor position. */
	(void)hal_cons_clear_to_eol_at(cursor_row, cursor_column);
}

/*
 * Clears from one PC/AT text-grid position through the row end.
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

	/* Clears every remaining cell with the current attribute. */
	for (current = column; current < HAL_CONS_COLUMNS; current++)
		write_cell(row, current, ' ', current_attribute);

	/* Stores the requested cursor position. */
	cursor_row = row;
	cursor_column = column;

	/* Reports a cleared row suffix. */
	return 1;
}

/*
 * Sets the PC/AT text-console cursor position.
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
 * Moves the PC/AT text-console cursor.
 */
void
hal_cons_move_cursor(
	int line,
	int column)
{
	/* Applies the signed coordinates through the validating setter. */
	(void)hal_cons_set_cursor((unsigned)line, (unsigned)column);
}

/*
 * Changes PC/AT hardware-cursor visibility.
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
 * Saves the current PC/AT console state.
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
 * Restores PC/AT terminal mode and optional cursor state.
 */
void
hal_cons_restore_terminal(
	const struct hal_cons_state *state)
{
	/* Selects terminal output before applying an optional valid position. */
	console_mode = HAL_CONS_TERMINAL;

	/* Restores the cursor only from a visible saved position. */
	if (state != NULL && state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS) {
		cursor_row = state->row;
		cursor_column = state->column;
		cursor_visible = state->cursor_visible;
	}

	/* Publishes the restored terminal cursor. */
	hal_cons_update_cursor();
}

/*
 * Selects the PC/AT console output mode.
 */
void
hal_cons_set_mode(
	enum hal_cons_mode mode)
{
	/* Publishes the requested console mode. */
	console_mode = mode;
}

/*
 * Suspends PC/AT text-memory and cursor updates.
 */
void
hal_cons_suspend(
	void)
{
	/* Avoids duplicate hardware suspension. */
	if (console_suspended)
		return;

	/* Hides the hardware cursor before blocking text-memory writes. */
	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, 0x20U);
	console_suspended = 1;
}

/*
 * Resumes and clears the PC/AT text console.
 */
void
hal_cons_resume(
	void)
{
	/* Avoids a display reset while the console is active. */
	if (!console_suspended)
		return;

	/* Re-enables text-memory writes before clearing the screen. */
	console_suspended = 0;
	hal_cons_clear();
}

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
/*
 * Resets PC/AT keyboard ownership state for the focused host test.
 */
void
pcat_input_ownership_test_reset(
	void)
{
	/* Clears held-key and queued-event state. */
	memset(key_down, 0, sizeof(key_down));
	memset(events, 0, sizeof(events));
	event_tail = 0;
	event_head = 0;
	alt_down = 0;
	ctrl_down = 0;
	shift_down = 0;
	caps_lock = 0;
}

/*
 * Changes one held PC/AT key for the focused host test.
 */
void
pcat_input_ownership_test_key(
	unsigned extended,
	unsigned scan,
	int down)
{
	unsigned state_index;

	/* Ignores scan positions outside the two 128-key maps. */
	if (extended > 1U || scan >= 128U)
		return;

	/* Sets or clears the selected held-key bit. */
	state_index = extended * 16U + (scan >> 3);

	/* Applies the requested state to the selected bitmap bit. */
	if (down) {
		key_down[state_index] |= (uint8_t)(1U << (scan & 7U));
	} else {
		key_down[state_index] &= (uint8_t)~(1U << (scan & 7U));
	}
}

/*
 * Changes PC/AT Caps Lock state for the focused host test.
 */
void
pcat_input_ownership_test_caps(
	int locked)
{
	/* Stores normalized Caps Lock state. */
	caps_lock = locked != 0;
}

/*
 * Rebuilds the PC/AT keyboard snapshot for the focused host test.
 */
void
pcat_input_ownership_test_rebuild(
	void)
{
	/* Rebuilds queued events from held-key state. */
	rebuild_keyboard_events_locked();
}

/*
 * Queues a PC/AT repeat event for the focused host test.
 */
void
pcat_input_ownership_test_repeat(
	const char *symbol)
{
	/* Appends one synthetic repeat event. */
	enqueue_keyboard_event_locked(symbol, HAL_KEY_EVENT_REPEAT);
}

/*
 * Pops one PC/AT event for the focused host test.
 */
int
pcat_input_ownership_test_pop(
	struct hal_key_event *event)
{
	/* Reports an empty event ring. */
	if (event_tail == event_head)
		return 0;

	/* Copies an optional event and advances the ring tail. */
	if (event != NULL)
		*event = events[event_tail];
	event_tail = (event_tail + 1U) % EVENT_COUNT;

	/* Reports one consumed event. */
	return 1;
}
#endif

/*
 * Reports the active PC/AT keyboard modifier mask.
 */
unsigned
hal_cons_modifiers(
	void)
{
	unsigned modifiers;

	/* Starts with no active modifier bits. */
	modifiers = 0;

	/* Adds the shift modifier when either shift key is held. */
	if (shift_down)
		modifiers |= 1U;

	/* Adds the control modifier while its key is held. */
	if (ctrl_down)
		modifiers |= 2U;

	/* Adds the alternate modifier while its key is held. */
	if (alt_down)
		modifiers |= 4U;

	/* Returns the collected modifier bits. */
	return modifiers;
}

/*
 * Reports whether a PC/AT keyboard event is queued.
 */
int
hal_cons_poll_event(
	struct hal_key_event *event)
{
	bool enabled;
	int available;

	/* Samples the event ring under its wait-queue lock. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	available = event_head != event_tail;

	/* Copies the oldest event when requested and available. */
	if (available && event != NULL)
		*event = events[event_tail];
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Returns whether an event was available without consuming it. */
	return available;
}

/*
 * Reports PC/AT keyboard input capabilities.
 */
void
hal_cons_get_input_info(
	struct hal_cons_input_info *info)
{
	static const char *const symbols[] = {
		"esc", "backspace", "tab", "enter",
		"leftshift", "rightshift", "leftctrl", "rightctrl",
		"leftalt", "rightalt", "capslock", "home", "up", "pageup",
		"left", "right", "end", "down", "pagedown", "insert",
		"delete", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
		"f8", "f9", "f10"
	};

	/* Ignores a missing capability destination. */
	if (info == NULL)
		return;

	/* Publishes event modes and the stable symbol inventory. */
	info->flags = HAL_CONS_INPUT_TEXT | HAL_CONS_INPUT_RELEASE |
	    HAL_CONS_INPUT_REPEAT;
	info->symbols = symbols;
	info->symbol_count = sizeof(symbols) / sizeof(symbols[0]);
}

/*
 * Reads one PC/AT keyboard event, waiting when necessary.
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
		enabled = hal_cons_wait_queue_lock(&input_waiters);

		/* Consumes and reports the oldest queued event. */
		if (event_head != event_tail) {
			/* Copies the event only when result storage was supplied. */
			if (event != NULL)
				*event = events[event_tail];
			event_tail = (event_tail + 1U) % EVENT_COUNT;
			hal_cons_wait_queue_unlock(&input_waiters, enabled);
			return 1;
		}

		/* Queues this task before releasing the lock and sleeping. */
		hal_cons_wait_queue_add(&input_waiters, &waiter);
		hal_cons_wait_queue_unlock(&input_waiters, enabled);
		kernel_wait_task();
	}
}

/*
 * Reads one text character from PC/AT keyboard events.
 */
int
hal_cons_getc(
	void)
{
	struct hal_key_event event;

	/* Waits until an event maps to a supported text character. */
	for (;;) {
		(void)hal_cons_read_event(&event);

		/* Ignores snapshot-only ownership records. */
		if ((event.flags & HAL_KEY_EVENT_SNAPSHOT) != 0)
			continue;

		/* Ignores releases and other non-character event types. */
		if ((event.flags &
		    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0) {
			continue;
		}

		/* Returns literal one-byte symbols directly. */
		if (event.symbol[1] == '\0')
			return event.symbol[0];

		/* Maps supported named control-key symbols in priority order. */
		if (symbol_equal(event.symbol, "enter"))
			return '\r';
		else if (symbol_equal(event.symbol, "tab"))
			return '\t';
		else if (symbol_equal(event.symbol, "backspace"))
			return '\b';
		else if (symbol_equal(event.symbol, "esc"))
			return 0x1b;
	}
}

/*
 * Reports legacy PC/AT keyboard state for one key code.
 */
int
hal_cons_key_state(
	int key)
{
	bool enabled;
	int down;

	/* Samples the supported legacy Shift key under the input lock. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	down = key == 0x170 ? shift_down : 0;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Returns the sampled legacy key state. */
	return down;
}

/*
 * Discards all queued PC/AT keyboard events.
 */
void
hal_cons_drain_input(
	void)
{
	bool enabled;

	/* Moves the ring tail to its head under the input lock. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	event_tail = event_head;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
}

/*
 * Initializes the PC/AT console and keyboard state.
 */
void
i386_bsp_cons_init(
	void)
{
	unsigned i;

	/* Clears event, modifier, and scan-prefix state. */
	event_tail = 0;
	event_head = 0;
	alt_down = 0;
	ctrl_down = 0;
	shift_down = 0;
	e0_prefix = 0;
	caps_lock = 0;

	/* Clears every held-key bitmap byte. */
	for (i = 0; i < sizeof(key_down); i++)
		key_down[i] = 0;

	/* Initializes wait ownership before resetting the display. */
	hal_cons_wait_queue_init(&input_waiters);
	hal_cons_reset();
}

/*
 * Enables interrupt-driven PC/AT keyboard input.
 */
void
i386_bsp_cons_irq_init(
	void)
{
	int result;

	/* Registers the keyboard IRQ handler before unmasking delivery. */
	result = hal_irq_set_handler(IRQ_KEYBOARD, keyboard_interrupt, NULL);

	/* Rejects a failed keyboard IRQ registration. */
	if (result != HAL_OK)
		HAL_FATAL("PC/AT keyboard IRQ registration failed");
	hal_irq_unmask(IRQ_KEYBOARD);
}

/* Writes one PC/AT VGA text cell unless display output is suspended. */
static void
write_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	/* Suppresses text-memory writes during graphics ownership. */
	if (console_suspended)
		return;

	/* Publishes the character and attribute as one volatile cell write. */
	VGA_MEMORY[row * HAL_CONS_COLUMNS + column] =
	    (uint16_t)((uint8_t)character | ((uint16_t)attribute << 8));
}

/* Scrolls the PC/AT text console upward by one row. */
static void
scroll(
	void)
{
	unsigned row;
	unsigned column;

	/* Copies visible rows upward while text memory is available. */
	if (!console_suspended) {
		/* Traverses every source row after the first. */
		for (row = 1; row < HAL_CONS_ROWS; row++) {
			/* Copies every cell into the preceding row. */
			for (column = 0; column < HAL_CONS_COLUMNS; column++) {
				VGA_MEMORY[(row - 1U) * HAL_CONS_COLUMNS + column] =
				    VGA_MEMORY[row * HAL_CONS_COLUMNS + column];
			}
		}
	}

	/* Clears the final row through the normal cell writer. */
	hal_cons_clear_row(HAL_CONS_ROWS - 1U);
}

/* Advances the PC/AT cursor to the next text row. */
static void
newline(
	void)
{
	/* Moves to the next row and scrolls at the bottom edge. */
	cursor_column = 0;
	cursor_row++;

	/* Scrolls and clips the cursor after crossing the bottom edge. */
	if (cursor_row >= HAL_CONS_ROWS) {
		scroll();
		cursor_row = HAL_CONS_ROWS - 1U;
	}

	/* Publishes the cursor only while terminal mode owns it. */
	if (console_mode == HAL_CONS_TERMINAL)
		hal_cons_update_cursor();
}

/* Resolves one set-one scan code to its stable key symbol. */
static const char *
scan_symbol(
	uint8_t scan,
	int extended)
{
	const char *symbol;

	/* Returns the base scan-table symbol for a non-extended key. */
	if (!extended)
		return scan_symbols[scan];

	/* Maps the supported extended scan positions. */
	switch (scan) {
	case 0x1d:
		symbol = "rightctrl";
		break;
	case 0x38:
		symbol = "rightalt";
		break;
	case 0x47:
		symbol = "home";
		break;
	case 0x48:
		symbol = "up";
		break;
	case 0x49:
		symbol = "pageup";
		break;
	case 0x4b:
		symbol = "left";
		break;
	case 0x4d:
		symbol = "right";
		break;
	case 0x4f:
		symbol = "end";
		break;
	case 0x50:
		symbol = "down";
		break;
	case 0x51:
		symbol = "pagedown";
		break;
	case 0x52:
		symbol = "insert";
		break;
	case 0x53:
		symbol = "delete";
		break;
	default:
		symbol = NULL;
		break;
	}

	/* Returns the mapped extended symbol. */
	return symbol;
}

/* Copies one key symbol and flags into an event record. */
static void
set_event(
	struct hal_key_event *event,
	const char *symbol,
	uint32_t flags)
{
	unsigned index;

	/* Copies as much of the terminated symbol as the event can hold. */
	index = 0;
	while (index + 1U < HAL_KEY_SYMBOL_SIZE && symbol[index] != '\0') {
		event->symbol[index] = symbol[index];
		index++;
	}

	/* Terminates and clears the remainder of the symbol buffer. */
	while (index < HAL_KEY_SYMBOL_SIZE) {
		event->symbol[index] = '\0';
		index++;
	}

	/* Publishes the event flags after its symbol. */
	event->flags = flags;
}

/* Tests two terminated key symbols for equality. */
static int
symbol_equal(
	const char *left,
	const char *right)
{
	/* Advances through every matching non-terminating byte. */
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	/* Reports two symbols ending at the same byte. */
	if (*left == *right)
		return 1;

	/* Reports different keyboard symbols. */
	return 0;
}

/* Appends one keyboard event or rebuilds an overflowed ring. */
static void
enqueue_keyboard_event_locked(
	const char *symbol,
	uint32_t flags)
{
	unsigned next;

	/* Computes the next event-ring head position. */
	next = (event_head + 1U) % EVENT_COUNT;

	/* Replaces an overflowing event stream with a state snapshot. */
	if (next == event_tail) {
		rebuild_keyboard_events_locked();
		return;
	}

	/* Publishes the new event before advancing the ring head. */
	set_event(&events[event_head], symbol, flags);
	event_head = next;
}

/* Tests whether one symbol represents snapshot-priority modifier state. */
static int
snapshot_modifier(
	const char *symbol)
{
	/* Tests modifier names in their existing short-circuit order. */
	if (symbol_equal(symbol, "leftshift"))
		return 1;
	else if (symbol_equal(symbol, "rightshift"))
		return 1;
	else if (symbol_equal(symbol, "leftctrl"))
		return 1;
	else if (symbol_equal(symbol, "rightctrl"))
		return 1;
	else if (symbol_equal(symbol, "leftalt"))
		return 1;
	else if (symbol_equal(symbol, "rightalt"))
		return 1;
	else if (symbol_equal(symbol, "capslock"))
		return 1;

	/* Reports an ordinary non-modifier symbol. */
	return 0;
}

/* Rebuilds a truthful held-key snapshot after event-ring overflow. */
static void
rebuild_keyboard_events_locked(
	void)
{
	const char *symbol;
	unsigned state_index;
	unsigned pass;
	unsigned extended;
	unsigned scan;

	/* Starts a fresh ring with the current lock-state resync marker. */
	event_tail = 0;
	event_head = 0;
	set_event(
		&events[event_head],
		"",
		HAL_KEY_EVENT_RESYNC |
		(caps_lock ? HAL_KEY_EVENT_LOCK_CAPS : 0U));
	event_head = (event_head + 1U) % EVENT_COUNT;

	/* Emits modifiers before ordinary held keys for a truthful snapshot. */
	for (pass = 0; pass < 2U; pass++) {
		/* Traverses the base and extended scan maps. */
		for (extended = 0; extended < 2U; extended++) {
			/* Examines every physical scan position. */
			for (scan = 0; scan < 128U; scan++) {
				state_index = extended * 16U + (scan >> 3);

				/* Skips scan positions which are not held. */
				if (((key_down[state_index] >> (scan & 7U)) & 1U) == 0)
					continue;

				/* Resolves and filters this held scan position by pass. */
				symbol = scan_symbol((uint8_t)scan, (int)extended);

				/* Skips held scans without a public symbol. */
				if (symbol == NULL)
					continue;

				/* Selects modifiers first and ordinary keys second. */
				if (snapshot_modifier(symbol) != (pass == 0U))
					continue;

				/* Appends the held-key snapshot event. */
				set_event(
					&events[event_head],
					symbol,
					HAL_KEY_EVENT_PRESS |
					HAL_KEY_EVENT_SNAPSHOT);
				event_head = (event_head + 1U) % EVENT_COUNT;
			}
		}
	}

	/* Terminates the rebuilt snapshot stream. */
	set_event(&events[event_head], "", HAL_KEY_EVENT_RESYNC_END);
	event_head = (event_head + 1U) % EVENT_COUNT;
}

/* Drains available 8042 keyboard bytes into the event ring. */
static void
pump_keyboard_locked(
	void)
{
	const char *symbol;
	uint32_t event_flags;
	unsigned state_index;
	uint8_t status;
	uint8_t raw;
	uint8_t scan;
	int released;
	int extended;
	int was_down;

	/* Reads keyboard bytes until the controller has none available. */
	for (;;) {
		status = asm_inb(KBD_STATUS);

		/* Stops for an empty output buffer or an auxiliary-device byte. */
		if ((status & 1U) == 0 || (status & KBD_STATUS_AUX) != 0)
			break;

		/* Consumes the pending keyboard-controller byte. */
		raw = asm_inb(KBD_DATA);

		/* Retains an E0 prefix for the following scan byte. */
		if (raw == 0xe0U) {
			e0_prefix = 1;
			continue;
		}

		/* Decodes release, scan position, and extended-map state. */
		released = (raw & 0x80U) != 0;
		scan = raw & 0x7fU;
		extended = e0_prefix;
		e0_prefix = 0;
		state_index = (extended ? 16U : 0U) + (scan >> 3);
		was_down = (key_down[state_index] >> (scan & 7U)) & 1U;

		/* Updates the selected held-key bit. */
		if (released) {
			key_down[state_index] &= (uint8_t)~(1U << (scan & 7));
		} else {
			key_down[state_index] |= (uint8_t)(1U << (scan & 7));
		}

		/* Recomputes aggregate modifier state from both scan maps. */
		shift_down = ((key_down[0x2aU >> 3] >> (0x2aU & 7U)) |
		    (key_down[0x36U >> 3] >> (0x36U & 7U))) & 1U;
		ctrl_down = ((key_down[0x1dU >> 3] >> (0x1dU & 7U)) |
		    (key_down[16U + (0x1dU >> 3)] >> (0x1dU & 7U))) & 1U;
		alt_down = ((key_down[0x38U >> 3] >> (0x38U & 7U)) |
		    (key_down[16U + (0x38U >> 3)] >> (0x38U & 7U))) & 1U;

		/* Toggles Caps Lock only on its first press. */
		if (scan == 0x3aU && !released && !was_down)
			caps_lock = !caps_lock;

		/* Ignores scan positions without a published symbol. */
		symbol = scan_symbol(scan, extended);

		/* Skips the decoded scan when no public symbol exists. */
		if (symbol == NULL)
			continue;

		/* Selects release, repeat, or first-press event semantics. */
		if (released) {
			event_flags = HAL_KEY_EVENT_RELEASE;
		} else if (was_down) {
			event_flags = HAL_KEY_EVENT_REPEAT;
		} else {
			event_flags = HAL_KEY_EVENT_PRESS;
		}
		enqueue_keyboard_event_locked(symbol, event_flags);
	}
}

/* Handles one PC/AT keyboard IRQ and wakes detached waiters. */
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

	/* Pumps input and detaches waiters while holding the event-ring lock. */
	waiters = NULL;
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	pump_keyboard_locked();

	/* Detaches all waiters when an event became available. */
	if (event_head != event_tail)
		waiters = hal_cons_wait_queue_detach_all(&input_waiters);
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Wakes detached tasks before completing the hardware IRQ. */
	hal_cons_wait_queue_notify_all(waiters);
	hal_irq_send_eoi(acknowledge);
}
