/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT VGA console and interrupt-driven 8042 keyboard driver.
 */

#include <hal/hal.h>

#include <string.h>

#include "../../cons-wait.h"
#include "../asm.h"
#include "../defs.h"
#include "../irq.h"
#include "bootloader/include/amd64-handoff.h"
#include "drivers/graphics/pcat/vgafont.h"

#define VGA_MEMORY	((volatile uint16_t *)((uintptr_t)AMD64_DIRECT_BASE + 0x000b8000U))
#define VGA_INDEX	0x3d4U
#define VGA_DATA	0x3d5U

#define KBD_DATA			0x60U
#define KBD_STATUS			0x64U
#define KBD_COMMAND			0x64U
#define KBD_STATUS_OUTPUT		0x01U
#define KBD_STATUS_INPUT		0x02U
#define KBD_STATUS_AUX			0x20U
#define KBD_READ_CONFIGURATION		0x20U
#define KBD_WRITE_CONFIGURATION		0x60U
#define KBD_DISABLE_AUX			0xa7U
#define KBD_DISABLE_KEYBOARD		0xadU
#define KBD_ENABLE_KEYBOARD		0xaeU
#define KBD_CONFIGURATION_KEYBOARD_IRQ	0x01U
#define KBD_CONFIGURATION_AUX_IRQ	0x02U
#define KBD_CONFIGURATION_KEYBOARD_OFF	0x10U
#define KBD_CONFIGURATION_AUX_OFF	0x20U
#define KBD_CONFIGURATION_TRANSLATION	0x40U
#define KBD_WAIT_LOOPS			100000U
#define KBD_FLUSH_LIMIT			64U

/* 256 physical scan positions, two resync markers, and one ring sentinel. */
#define EVENT_COUNT 259U

struct console_output_token {
	uint32_t cpu;
	int interrupts_enabled;
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

static const struct zbl6_framebuffer *framebuffer;
static volatile uint32_t *framebuffer_pixels;
static uint16_t framebuffer_cells[HAL_CONS_ROWS * HAL_CONS_COLUMNS];
static unsigned framebuffer_x;
static unsigned framebuffer_y;
static unsigned drawn_cursor_row;
static unsigned drawn_cursor_column;
static int framebuffer_cursor_drawn;

/*
 * Console output is a leaf HAL service: it is used before the scheduler and
 * lock-rank machinery exist, from IRQ-disabled paths, and while reporting a
 * fault.  Keep its serialization self-contained.  The high word identifies
 * the physical CPU and the low word is a recursion depth.  Updating both in
 * one atomic operation closes the NMI window between publishing ownership and
 * publishing recursion state.
 *
 * Recursion is not used by the ordinary output implementation; every public
 * entry point calls a non-recursive locked helper.  It remains supported so a
 * fault or NMI on the CPU already printing can report the failure rather than
 * deadlocking on itself.  IRQ disabling prevents migration during ownership.
 */
static uint64_t console_output_state;

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
static __thread uint32_t console_test_cpu;
static __thread int console_test_interrupts_enabled = 1;
#endif

static const uint32_t vga_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
static struct zbl6_framebuffer console_test_framebuffer;
#endif

/*
 * The designated indexes preserve the sparse scan-code identity and are the
 * sole narrow designated-initializer exception in this source.
 */
static const char *const scan_symbols[128] = {
	[0x01] = "esc",
	[0x02] = "1",
	[0x03] = "2",
	[0x04] = "3",
	[0x05] = "4",
	[0x06] = "5",
	[0x07] = "6",
	[0x08] = "7",
	[0x09] = "8",
	[0x0a] = "9",
	[0x0b] = "0",
	[0x0c] = "minus",
	[0x0d] = "equal",
	[0x0e] = "backspace",
	[0x0f] = "tab",
	[0x10] = "q",
	[0x11] = "w",
	[0x12] = "e",
	[0x13] = "r",
	[0x14] = "t",
	[0x15] = "y",
	[0x16] = "u",
	[0x17] = "i",
	[0x18] = "o",
	[0x19] = "p",
	[0x1a] = "leftbrace",
	[0x1b] = "rightbrace",
	[0x1c] = "enter",
	[0x1d] = "leftctrl",
	[0x1e] = "a",
	[0x1f] = "s",
	[0x20] = "d",
	[0x21] = "f",
	[0x22] = "g",
	[0x23] = "h",
	[0x24] = "j",
	[0x25] = "k",
	[0x26] = "l",
	[0x27] = "semicolon",
	[0x28] = "apostrophe",
	[0x29] = "grave",
	[0x2a] = "leftshift",
	[0x2b] = "backslash",
	[0x2c] = "z",
	[0x2d] = "x",
	[0x2e] = "c",
	[0x2f] = "v",
	[0x30] = "b",
	[0x31] = "n",
	[0x32] = "m",
	[0x33] = "comma",
	[0x34] = "dot",
	[0x35] = "slash",
	[0x36] = "rightshift",
	[0x38] = "leftalt",
	[0x39] = "space",
	[0x3a] = "capslock",
	[0x3b] = "f1",
	[0x3c] = "f2",
	[0x3d] = "f3",
	[0x3e] = "f4",
	[0x3f] = "f5",
	[0x40] = "f6",
	[0x41] = "f7",
	[0x42] = "f8",
	[0x43] = "f9",
	[0x44] = "f10",
};

static int console_interrupt_disable(void);
static void console_interrupt_enable(void);
#ifndef ZEDBSD_CONSOLE_OUTPUT_TEST
static void console_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
#endif
static uint32_t console_cpu_identity(void);
static struct console_output_token console_output_lock(void);
static void console_output_unlock(struct console_output_token token);
static uint32_t framebuffer_color_locked(unsigned color);
static void framebuffer_draw_cell_locked(unsigned row, unsigned column, uint16_t cell, int cursor);
static void write_cell_locked(unsigned row, unsigned column, int character, uint8_t attribute);
static void update_cursor_locked(void);
static void clear_row_locked(unsigned row);
static void clear_locked(void);
static void reset_locked(void);
static void scroll_locked(void);
static void newline_locked(void);
static void put_graphic_locked(int character);
static void putc_locked(int character);
static void write_n_locked(const char *string, unsigned length);
static int write_n_at_locked(unsigned row, unsigned column, const char *string, unsigned length, uint8_t attribute);
static const char *scan_symbol(uint8_t scan, int extended);
static void set_event(struct hal_key_event *event, const char *symbol, uint32_t flags);
static int symbol_equal(const char *left, const char *right);
static void rebuild_keyboard_events_locked(void);
static void enqueue_keyboard_event_locked(const char *symbol, uint32_t flags);
static int snapshot_modifier(const char *symbol);
static int keyboard_wait_input_empty(void);
static int keyboard_write_command(uint8_t command);
static void keyboard_flush_output(void);
static int keyboard_read_configuration(uint8_t *configuration);
static int keyboard_write_configuration(uint8_t configuration);
static int keyboard_controller_init(void);
static void pump_keyboard_locked(void);
static void keyboard_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument);

/*
 * Acquires recursive console-output ownership.
 */
uint64_t
pcat_cons_output_begin(
	void)
{
	struct console_output_token token;
	uint64_t encoded;

	/* Acquires output ownership and preserves the caller's interrupt state. */
	token = console_output_lock();
	encoded = ((uint64_t)token.cpu << 32) |
	    (token.interrupts_enabled != 0 ? 1U : 0U);

	/* Reports the token required to release this ownership level. */
	return encoded;
}

/*
 * Releases recursive console-output ownership.
 */
void
pcat_cons_output_end(
	uint64_t encoded)
{
	struct console_output_token token;

	/* Reconstructs and releases the saved ownership token. */
	token.cpu = (uint32_t)(encoded >> 32);
	token.interrupts_enabled = (encoded & 1U) != 0;
	console_output_unlock(token);
}

/*
 * Updates the visible hardware cursor.
 */
void
hal_cons_update_cursor(
	void)
{
	struct console_output_token token;

	/* Serializes the cursor update with all console rendering. */
	token = console_output_lock();
	update_cursor_locked();
	console_output_unlock(token);
}

/*
 * Clears one console row.
 */
void
hal_cons_clear_row(
	unsigned row)
{
	struct console_output_token token;

	/* Serializes row clearing with all console rendering. */
	token = console_output_lock();
	clear_row_locked(row);
	console_output_unlock(token);
}

/*
 * Clears the complete console.
 */
void
hal_cons_clear(
	void)
{
	struct console_output_token token;

	/* Serializes console clearing with all rendering. */
	token = console_output_lock();
	clear_locked();
	console_output_unlock(token);
}

/*
 * Resets the console presentation state.
 */
void
hal_cons_reset(
	void)
{
	struct console_output_token token;

	/* Serializes the reset with all console rendering. */
	token = console_output_lock();
	reset_locked();
	console_output_unlock(token);
}

/*
 * Writes one character to the console.
 */
void
hal_cons_putc(
	int character)
{
	struct console_output_token token;

	/* Serializes character rendering with all console output. */
	token = console_output_lock();
	putc_locked(character);
	console_output_unlock(token);
}

/*
 * Writes a bounded byte string to the console.
 */
void
hal_cons_write_n(
	const char *string,
	unsigned length)
{
	struct console_output_token token;

	/* Ignores a missing input string. */
	if (string == NULL)
		return;

	/* Serializes bounded text rendering with all console output. */
	token = console_output_lock();
	write_n_locked(string, length);
	console_output_unlock(token);
}

/*
 * Writes a terminated byte string to the console.
 */
void
hal_cons_write(
	const char *string)
{
	unsigned length;

	/* Ignores a missing input string. */
	if (string == NULL)
		return;

	/* Measures the terminated input string. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured byte range. */
	hal_cons_write_n(string, length);
}

/*
 * Writes bounded text at a fixed console position.
 */
int
hal_cons_write_n_at(
	unsigned row,
	unsigned column,
	const char *string,
	unsigned length,
	uint8_t attribute)
{
	struct console_output_token token;
	int changed;

	/* Rejects a missing input string. */
	if (string == NULL)
		return -1;

	/* Serializes positioned rendering with all console output. */
	token = console_output_lock();
	changed = write_n_at_locked(
		row,
		column,
		string,
		length,
		attribute);
	console_output_unlock(token);

	/* Reports the number of cells changed. */
	return changed;
}

/*
 * Writes attributed text at a fixed console position.
 */
int
hal_cons_write_at_attr(
	unsigned row,
	unsigned column,
	const char *string,
	uint8_t attribute)
{
	unsigned length;
	int changed;

	/* Rejects a missing input string. */
	if (string == NULL)
		return -1;

	/* Measures the terminated input string. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured string with the requested attribute. */
	changed = hal_cons_write_n_at(
		row,
		column,
		string,
		length,
		attribute);

	/* Reports the number of cells changed. */
	return changed;
}

/*
 * Writes text at a fixed console position.
 */
void
hal_cons_write_at(
	unsigned row,
	unsigned column,
	const char *string)
{
	struct console_output_token token;
	unsigned length;

	/* Ignores a missing input string. */
	if (string == NULL)
		return;

	/* Measures the terminated input string. */
	length = 0;
	while (string[length] != '\0')
		length++;

	/* Writes the measured string with the current attribute. */
	token = console_output_lock();
	(void)write_n_at_locked(
		row,
		column,
		string,
		length,
		current_attribute);
	console_output_unlock(token);
}

/*
 * Clears from the current cursor to the end of its row.
 */
void
hal_cons_clear_to_eol(
	void)
{
	struct console_output_token token;
	unsigned current;

	/* Serializes row clearing with all console rendering. */
	token = console_output_lock();

	/* Clears every cell at or after a valid cursor. */
	if (cursor_row < HAL_CONS_ROWS &&
	    cursor_column < HAL_CONS_COLUMNS) {
		/* Clears the remainder of the current row. */
		for (current = cursor_column;
		     current < HAL_CONS_COLUMNS;
		     current++) {
			write_cell_locked(
				cursor_row,
				current,
				' ',
				current_attribute);
		}
	}

	/* Releases console-output serialization after clearing the row. */
	console_output_unlock(token);
}

/*
 * Clears from a specified position to the end of its row.
 */
int
hal_cons_clear_to_eol_at(
	unsigned row,
	unsigned column)
{
	struct console_output_token token;
	unsigned current;
	int changed;

	/* Initializes the invalid-position result. */
	changed = 0;

	/* Serializes row clearing with all console rendering. */
	token = console_output_lock();

	/* Clears every cell at or after a valid requested position. */
	if (row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS) {
		/* Clears the remainder of the requested row. */
		for (current = column;
		     current < HAL_CONS_COLUMNS;
		     current++) {
			write_cell_locked(
				row,
				current,
				' ',
				current_attribute);
		}

		/* Leaves the logical cursor at the requested position. */
		cursor_row = row;
		cursor_column = column;
		changed = 1;
	}

	/* Releases console-output serialization after the update. */
	console_output_unlock(token);

	/* Reports whether a valid row was cleared. */
	return changed;
}

/*
 * Sets the console cursor position.
 */
int
hal_cons_set_cursor(
	unsigned row,
	unsigned column)
{
	struct console_output_token token;
	int changed;

	/* Initializes the invalid-position result. */
	changed = 0;

	/* Serializes cursor movement with all console rendering. */
	token = console_output_lock();

	/* Applies a position within the fixed console geometry. */
	if (row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS) {
		cursor_row = row;
		cursor_column = column;
		update_cursor_locked();
		changed = 1;
	}

	/* Releases console-output serialization after moving the cursor. */
	console_output_unlock(token);

	/* Reports whether the requested position was valid. */
	return changed;
}

/*
 * Moves the console cursor to a signed position.
 */
void
hal_cons_move_cursor(
	int line,
	int column)
{
	/* Delegates validation to the unsigned cursor interface. */
	(void)hal_cons_set_cursor((unsigned)line, (unsigned)column);
}

/*
 * Shows or hides the hardware cursor.
 */
void
hal_cons_show_cursor(
	int visible)
{
	struct console_output_token token;

	/* Serializes visibility and rendering updates. */
	token = console_output_lock();
	cursor_visible = visible != 0;
	update_cursor_locked();
	console_output_unlock(token);
}

/*
 * Saves the current console presentation state.
 */
void
hal_cons_save_state(
	struct hal_cons_state *state)
{
	struct console_output_token token;

	/* Ignores a missing output record. */
	if (state == NULL)
		return;

	/* Captures a consistent presentation snapshot. */
	token = console_output_lock();
	state->mode = console_mode;
	state->row = cursor_row;
	state->column = cursor_column;
	state->cursor_visible = cursor_visible;
	console_output_unlock(token);
}

/*
 * Restores terminal mode and an optional presentation state.
 */
void
hal_cons_restore_terminal(
	const struct hal_cons_state *state)
{
	struct console_output_token token;

	/* Serializes restoration with all console rendering. */
	token = console_output_lock();
	console_mode = HAL_CONS_TERMINAL;

	/* Restores a valid saved cursor state when supplied. */
	if (state != NULL &&
	    state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS) {
		cursor_row = state->row;
		cursor_column = state->column;
		cursor_visible = state->cursor_visible;
	}

	/* Publishes the restored cursor and releases output serialization. */
	update_cursor_locked();
	console_output_unlock(token);
}

/*
 * Selects the console presentation mode.
 */
void
hal_cons_set_mode(
	enum hal_cons_mode mode)
{
	struct console_output_token token;

	/* Publishes the mode under the output serializer. */
	token = console_output_lock();
	console_mode = mode;
	console_output_unlock(token);
}

/*
 * Suspends hardware console rendering.
 */
void
hal_cons_suspend(
	void)
{
	struct console_output_token token;

	/* Serializes suspension with all console rendering. */
	token = console_output_lock();

	/* Leaves an already suspended console unchanged. */
	if (console_suspended) {
		console_output_unlock(token);

		/* Completes the no-op suspension request. */
		return;
	}

	/* Hides the VGA cursor when no framebuffer backend is active. */
	if (framebuffer_pixels == NULL) {
		asm_outb(VGA_INDEX, 0x0aU);
		asm_outb(VGA_DATA, 0x20U);
	}

	/* Publishes suspension before releasing output serialization. */
	console_suspended = 1;
	console_output_unlock(token);
}

/*
 * Resumes hardware console rendering.
 */
void
hal_cons_resume(
	void)
{
	struct console_output_token token;

	/* Serializes resumption with all console rendering. */
	token = console_output_lock();

	/* Leaves an active console unchanged. */
	if (!console_suspended) {
		console_output_unlock(token);

		/* Completes the no-op resume request. */
		return;
	}

	/* Re-enables rendering and reconstructs the visible terminal. */
	console_suspended = 0;
	clear_locked();
	console_output_unlock(token);
}

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
/*
 * Selects the simulated CPU identity for the output fixture.
 */
void
pcat_console_output_test_set_cpu(
	uint32_t cpu)
{
	/* Publishes the simulated CPU selected by the fixture. */
	console_test_cpu = cpu;
}

/*
 * Resets the framebuffer console output fixture.
 */
void
pcat_console_output_test_reset(
	uint32_t *pixels,
	size_t pixel_count)
{
	struct console_output_token token;

	/* Resets the simulated ownership and interrupt state. */
	__atomic_store_n(&console_output_state, 0, __ATOMIC_RELAXED);
	console_test_interrupts_enabled = 1;

	/* Installs and initializes the simulated framebuffer. */
	token = console_output_lock();
	memset(
		&console_test_framebuffer,
		0,
		sizeof(console_test_framebuffer));
	console_test_framebuffer.size = pixel_count * sizeof(*pixels);
	console_test_framebuffer.width = HAL_CONS_COLUMNS * 8U;
	console_test_framebuffer.height =
	    HAL_CONS_ROWS * PCAT_VGAFONT_HEIGHT;
	console_test_framebuffer.stride = console_test_framebuffer.width;
	console_test_framebuffer.format = ZBL6_FRAMEBUFFER_BGRX8888;
	framebuffer = &console_test_framebuffer;
	framebuffer_pixels = pixels;
	framebuffer_x = 0;
	framebuffer_y = 0;
	framebuffer_cursor_drawn = 0;
	console_suspended = 0;
	reset_locked();
	console_output_unlock(token);
}

/*
 * Injects reentrant output during a transient newline state.
 */
void
pcat_console_output_test_reentrant_transient(
	int character)
{
	struct console_output_token token;

	/* Models a fault or NMI after newline publishes its transient row. */
	token = console_output_lock();
	cursor_row = HAL_CONS_ROWS;
	hal_cons_putc(character);
	cursor_row = HAL_CONS_ROWS - 1U;
	cursor_column = 0;
	console_output_unlock(token);
}

/*
 * Reports the simulated framebuffer cursor state.
 */
int
pcat_console_output_test_state(
	unsigned *row,
	unsigned *column)
{
	struct console_output_token token;
	int valid;

	/* Captures the fixture state under output serialization. */
	token = console_output_lock();
	valid = cursor_row < HAL_CONS_ROWS;

	/* Checks the column only after the cursor row proves valid. */
	if (valid)
		valid = cursor_column < HAL_CONS_COLUMNS;

	/* Checks the framebuffer only after the cursor proves valid. */
	if (valid)
		valid = framebuffer != NULL;

	/* Checks mapped pixels only after a framebuffer proves present. */
	if (valid)
		valid = framebuffer_pixels != NULL;

	/* Publishes the current cursor when requested. */
	if (row != NULL)
		*row = cursor_row;

	/* Publishes the current column when requested. */
	if (column != NULL)
		*column = cursor_column;

	/* Releases output serialization after capturing the fixture state. */
	console_output_unlock(token);

	/* Reports whether the captured fixture state is valid. */
	return valid;
}
#endif

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
/*
 * Resets the PC/AT keyboard ownership fixture.
 */
void
pcat_input_ownership_test_reset(
	void)
{
	/* Clears all held-key and queued-event state. */
	memset(key_down, 0, sizeof(key_down));
	memset(events, 0, sizeof(events));
	event_head = 0;
	event_tail = 0;
	caps_lock = 0;
	shift_down = 0;
	ctrl_down = 0;
	alt_down = 0;
}

/*
 * Sets one held key in the keyboard ownership fixture.
 */
void
pcat_input_ownership_test_key(
	unsigned extended,
	unsigned scan,
	int down)
{
	unsigned state_index;

	/* Ignores a scan position outside the emulated keyboard state. */
	if (extended > 1U || scan >= 128U)
		return;

	/* Updates the selected physical key bit. */
	state_index = extended * 16U + (scan >> 3);

	/* Records the requested press or release state. */
	if (down) {
		key_down[state_index] |=
		    (uint8_t)(1U << (scan & 7U));
	} else {
		key_down[state_index] &=
		    (uint8_t)~(1U << (scan & 7U));
	}
}

/*
 * Sets the caps-lock state in the keyboard ownership fixture.
 */
void
pcat_input_ownership_test_caps(
	int locked)
{
	/* Publishes the simulated caps-lock state. */
	caps_lock = locked != 0;
}

/*
 * Rebuilds the keyboard ownership snapshot.
 */
void
pcat_input_ownership_test_rebuild(
	void)
{
	/* Rebuilds the fixture queue from the simulated held-key state. */
	rebuild_keyboard_events_locked();
}

/*
 * Enqueues one repeat event in the keyboard ownership fixture.
 */
void
pcat_input_ownership_test_repeat(
	const char *symbol)
{
	/* Enqueues the requested simulated repeat transition. */
	enqueue_keyboard_event_locked(symbol, HAL_KEY_EVENT_REPEAT);
}

/*
 * Removes one event from the keyboard ownership fixture.
 */
int
pcat_input_ownership_test_pop(
	struct hal_key_event *event)
{
	/* Reports an empty fixture queue. */
	if (event_tail == event_head)
		return 0;

	/* Copies the oldest event when requested. */
	if (event != NULL)
		*event = events[event_tail];

	/* Retires the consumed fixture event. */
	event_tail = (event_tail + 1U) % EVENT_COUNT;

	/* Reports a consumed fixture event. */
	return 1;
}
#endif

/*
 * Reports the active keyboard modifier mask.
 */
unsigned
hal_cons_modifiers(
	void)
{
	unsigned modifiers;

	/* Starts with no active modifier bits. */
	modifiers = 0;

	/* Reports an active shift key. */
	if (shift_down)
		modifiers |= 1U;

	/* Reports an active control key. */
	if (ctrl_down)
		modifiers |= 2U;

	/* Reports an active alternate key. */
	if (alt_down)
		modifiers |= 4U;

	/* Reports the combined modifier mask. */
	return modifiers;
}

/*
 * Tests whether a console key event is queued.
 */
int
hal_cons_poll_event(
	struct hal_key_event *event)
{
	bool enabled;
	int available;

	/* Inspects the queue under input serialization. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	available = event_head != event_tail;

	/* Copies the queued event without consuming it when requested. */
	if (available && event != NULL)
		*event = events[event_tail];

	/* Releases input serialization after inspecting the queue. */
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Reports whether an event is available. */
	return available;
}

/*
 * Reports the PC/AT console input capabilities.
 */
void
hal_cons_get_input_info(
	struct hal_cons_input_info *info)
{
	static const char *const symbols[] = {
		"esc", "backspace", "tab", "enter",
		"leftshift", "rightshift", "leftctrl", "rightctrl",
		"leftalt", "rightalt", "capslock", "home", "up",
		"pageup", "left", "right", "end", "down", "pagedown",
		"insert", "delete", "f1", "f2", "f3", "f4", "f5",
		"f6", "f7", "f8", "f9", "f10"
	};

	/* Ignores a missing output record. */
	if (info == NULL)
		return;

	/* Publishes the keyboard event and symbol capabilities. */
	info->flags = HAL_CONS_INPUT_TEXT | HAL_CONS_INPUT_RELEASE |
	    HAL_CONS_INPUT_REPEAT;
	info->symbols = symbols;
	info->symbol_count = sizeof(symbols) / sizeof(symbols[0]);
}

/*
 * Waits for and consumes one console key event.
 */
int
hal_cons_read_event(
	struct hal_key_event *event)
{
	struct hal_cons_wait_entry waiter;
	bool enabled;

	/* Initializes the reusable wait-queue entry for this task. */
	waiter.task = hal_task_get_current();
	waiter.next = NULL;
	waiter.queued = 0;

	/* Waits until the interrupt path publishes a queued key event. */
	for (;;) {
		enabled = hal_cons_wait_queue_lock(&input_waiters);

		/* Consumes the oldest available event. */
		if (event_head != event_tail) {
			/* Copies the event when requested. */
			if (event != NULL)
				*event = events[event_tail];

			/* Retires the event and releases input serialization. */
			event_tail = (event_tail + 1U) % EVENT_COUNT;
			hal_cons_wait_queue_unlock(&input_waiters, enabled);

			/* Reports a consumed key event. */
			return 1;
		}

		/* Queues this task before yielding to avoid a lost wakeup. */
		hal_cons_wait_queue_add(&input_waiters, &waiter);
		hal_cons_wait_queue_unlock(&input_waiters, enabled);
		kernel_wait_task();
	}
}

/*
 * Waits for and translates one text key event.
 */
int
hal_cons_getc(
	void)
{
	struct hal_key_event event;

	/* Waits until a press or repeat event represents a text character. */
	for (;;) {
		(void)hal_cons_read_event(&event);

		/* Ignores key-state snapshot events. */
		if ((event.flags & HAL_KEY_EVENT_SNAPSHOT) != 0)
			continue;

		/* Ignores events which do not produce text. */
		if ((event.flags &
		    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0) {
			continue;
		}

		/* Reports a one-byte key symbol directly. */
		if (event.symbol[1] == '\0')
			return event.symbol[0];

		/* Translates the enter key. */
		if (symbol_equal(event.symbol, "enter"))
			return '\r';

		/* Translates the tab key. */
		if (symbol_equal(event.symbol, "tab"))
			return '\t';

		/* Translates the backspace key. */
		if (symbol_equal(event.symbol, "backspace"))
			return '\b';

		/* Translates the escape key. */
		if (symbol_equal(event.symbol, "esc"))
			return 0x1b;
	}
}

/*
 * Reports the compatibility key state.
 */
int
hal_cons_key_state(
	int key)
{
	bool enabled;
	int down;

	/* Reads the compatibility key under input serialization. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	down = key == 0x170 ? shift_down : 0;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Reports the requested compatibility state. */
	return down;
}

/*
 * Discards all queued console input.
 */
void
hal_cons_drain_input(
	void)
{
	bool enabled;

	/* Advances the consumer to the published queue head. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	event_tail = event_head;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
}

/*
 * Initializes the PC/AT console and keyboard state.
 */
void
pcat_cons_init(
	void)
{
	struct console_output_token token;
	uint64_t aligned;
	uint64_t offset;
	uint64_t pixel;
	uint64_t pixel_count;
	unsigned index;

	/* Acquires output ownership before selecting the rendering backend. */
	token = console_output_lock();
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	framebuffer_pixels = NULL;
	framebuffer_cursor_drawn = 0;

	/* Selects a valid firmware framebuffer large enough for the console. */
	if (framebuffer != NULL &&
	    framebuffer->width >= HAL_CONS_COLUMNS * 8U &&
	    framebuffer->height >= HAL_CONS_ROWS * PCAT_VGAFONT_HEIGHT &&
	    framebuffer->stride >= framebuffer->width &&
	    (uint64_t)framebuffer->stride * framebuffer->height <=
	    framebuffer->size / sizeof(*framebuffer_pixels)) {
		aligned = framebuffer->physical_base & ~0x1fffffULL;
		offset = framebuffer->physical_base - aligned;
		framebuffer_pixels = (volatile uint32_t *)(uintptr_t)
		    (ZBL6_FRAMEBUFFER_VIRTUAL_BASE + offset);
		framebuffer_x =
		    (framebuffer->width - HAL_CONS_COLUMNS * 8U) / 2U;
		framebuffer_y = (framebuffer->height - HAL_CONS_ROWS *
		    PCAT_VGAFONT_HEIGHT) / 2U;

		/* Clears every visible firmware framebuffer pixel. */
		pixel_count = (uint64_t)framebuffer->stride *
		    framebuffer->height;
		for (pixel = 0; pixel < pixel_count; pixel++)
			framebuffer_pixels[pixel] = 0;
	}

	/* Resets rendering before releasing output ownership. */
	reset_locked();
	console_output_unlock(token);

	/* Resets the keyboard state before enabling input interrupts. */
	event_head = 0;
	event_tail = 0;
	shift_down = 0;
	ctrl_down = 0;
	alt_down = 0;
	caps_lock = 0;
	e0_prefix = 0;

	/* Clears every physical held-key bit. */
	for (index = 0; index < sizeof(key_down); index++)
		key_down[index] = 0;

	/* Initializes the task wait queue. */
	hal_cons_wait_queue_init(&input_waiters);
}

/*
 * Registers and enables the PC/AT keyboard interrupt.
 */
void
pcat_cons_irq_init(
	void)
{
	int status;

	/* Keeps the legacy line quiet while establishing controller ownership. */
	hal_irq_mask(IRQ_KEYBOARD);

	/* Registers the keyboard interrupt handler. */
	status = hal_irq_set_handler(
		IRQ_KEYBOARD,
		keyboard_interrupt,
		NULL);
	if (status != HAL_OK)
		HAL_FATAL("PC/AT keyboard IRQ registration failed");

	/* Establishes a known keyboard port and scan-code translation state. */
	status = keyboard_controller_init();
	if (status != HAL_OK) {
		hal_printf("input: i8042 keyboard unavailable (%d)\n", status);
		return;
	}

	/* Enables delivery of keyboard interrupts. */
	hal_irq_unmask(IRQ_KEYBOARD);
}

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
/* Disables simulated interrupts for console output serialization. */
static int
console_interrupt_disable(
	void)
{
	int enabled;

	/* Saves and disables the simulated interrupt state. */
	enabled = console_test_interrupts_enabled;
	console_test_interrupts_enabled = 0;

	/* Reports the prior simulated state. */
	return enabled;
}

/* Enables simulated interrupts after console output serialization. */
static void
console_interrupt_enable(
	void)
{
	/* Restores simulated interrupt delivery. */
	console_test_interrupts_enabled = 1;
}

/* Reports the fixture-selected physical CPU identity. */
static uint32_t
console_cpu_identity(
	void)
{
	/* Reports the current simulated CPU identity. */
	return console_test_cpu;
}
#else
/* Disables local interrupts for console output serialization. */
static int
console_interrupt_disable(
	void)
{
	int enabled;

	/* Saves and disables the architectural interrupt state. */
	enabled = hal_irq_disable() ? 1 : 0;

	/* Reports the prior architectural state. */
	return enabled;
}

/* Enables local interrupts after console output serialization. */
static void
console_interrupt_enable(
	void)
{
	/* Restores architectural interrupt delivery. */
	hal_irq_enable();
}

/* Reads one CPUID leaf for early console identity selection. */
static void
console_cpuid(
	uint32_t leaf,
	uint32_t subleaf,
	uint32_t *eax,
	uint32_t *ebx,
	uint32_t *ecx,
	uint32_t *edx)
{
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;

	/* Executes CPUID with the requested leaf and subleaf. */
	a = leaf;
	c = subleaf;
	__asm__ volatile("cpuid"
	    : "+a"(a), "=b"(b), "+c"(c), "=d"(d));

	/* Publishes every returned register. */
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
}

/* Reports a physical CPU identity before per-CPU state exists. */
static uint32_t
console_cpu_identity(
	void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t maximum;

	/* Finds the highest standard CPUID leaf. */
	console_cpuid(0, 0, &maximum, &ebx, &ecx, &edx);

	/* Prefers the extended topology x2APIC identity. */
	if (maximum >= 0x1fU) {
		console_cpuid(0x1fU, 0, &eax, &ebx, &ecx, &edx);

		/* Reports a populated topology level. */
		if (ebx != 0)
			return edx;
	}

	/* Falls back to the legacy topology x2APIC identity. */
	if (maximum >= 0x0bU) {
		console_cpuid(0x0bU, 0, &eax, &ebx, &ecx, &edx);

		/* Reports a populated topology level. */
		if (ebx != 0)
			return edx;
	}

	/* Reads and reports the initial APIC identity. */
	console_cpuid(1, 0, &eax, &ebx, &ecx, &edx);

	/* Reports the legacy initial APIC identifier. */
	return ebx >> 24;
}
#endif

/* Acquires recursive console-output ownership. */
static struct console_output_token
console_output_lock(
	void)
{
	struct console_output_token token;
	uint64_t observed;
	uint64_t desired;
	uint32_t depth;
	uint32_t owner;
	int acquired;

	/* Disables interrupts and identifies the non-migrating caller. */
	token.interrupts_enabled = console_interrupt_disable();
	token.cpu = console_cpu_identity();
	observed = __atomic_load_n(&console_output_state, __ATOMIC_ACQUIRE);

	/* Waits until the caller can acquire or recursively enter ownership. */
	for (;;) {
		depth = (uint32_t)observed;
		owner = (uint32_t)(observed >> 32);

		/* Selects a new or recursive ownership state. */
		if (depth == 0) {
			desired = ((uint64_t)token.cpu << 32) | 1U;
		} else if (owner == token.cpu) {
			/* Rejects recursion-depth overflow. */
			if (depth == UINT32_MAX)
				__builtin_trap();

			/* Extends recursive ownership by one level. */
			desired = observed + 1U;
		} else {
			hal_atomic_relax();
			observed = __atomic_load_n(
				&console_output_state,
				__ATOMIC_ACQUIRE);
			continue;
		}

		/* Publishes the selected ownership state atomically. */
		acquired = __atomic_compare_exchange_n(
			&console_output_state,
			&observed,
			desired,
			0,
			__ATOMIC_ACQUIRE,
			__ATOMIC_RELAXED);
		if (acquired)
			return token;
	}
}

/* Releases one level of recursive console-output ownership. */
static void
console_output_unlock(
	struct console_output_token token)
{
	uint64_t observed;
	uint64_t desired;
	uint32_t depth;
	uint32_t owner;
	int released;

	/* Reads the ownership state held by this CPU. */
	observed = __atomic_load_n(&console_output_state, __ATOMIC_RELAXED);

	/* Retries until this CPU publishes the reduced ownership depth. */
	for (;;) {
		depth = (uint32_t)observed;
		owner = (uint32_t)(observed >> 32);

		/* Rejects release by a CPU which does not own the console. */
		if (depth == 0 || owner != token.cpu)
			__builtin_trap();

		/* Selects and atomically publishes the reduced ownership depth. */
		desired = depth == 1U ? 0 : observed - 1U;
		released = __atomic_compare_exchange_n(
			&console_output_state,
			&observed,
			desired,
			0,
			__ATOMIC_RELEASE,
			__ATOMIC_RELAXED);

		/* Leaves the retry loop after publishing the new state. */
		if (released)
			break;
	}

	/* Restores interrupts only for the outermost caller which disabled them. */
	if (token.interrupts_enabled)
		console_interrupt_enable();
}

/* Converts one VGA palette index to the framebuffer format. */
static uint32_t
framebuffer_color_locked(
	unsigned color)
{
	uint32_t rgb;
	uint32_t converted;

	/* Selects the VGA palette entry for the requested attribute. */
	rgb = vga_palette[color & 15U];

	/* Swaps red and blue for an RGBX framebuffer. */
	if (framebuffer != NULL &&
	    framebuffer->format == ZBL6_FRAMEBUFFER_RGBX8888) {
		converted = ((rgb & 0xff0000U) >> 16) |
		    (rgb & 0x00ff00U) |
		    ((rgb & 0x0000ffU) << 16);

		/* Reports the converted RGBX palette value. */
		return converted;
	}

	/* Reports the native BGRX palette value. */
	return rgb;
}

/* Draws one text cell into the firmware framebuffer. */
static void
framebuffer_draw_cell_locked(
	unsigned row,
	unsigned column,
	uint16_t cell,
	int cursor)
{
	volatile uint32_t *out;
	uint8_t character;
	uint8_t attribute;
	uint8_t bits;
	uint32_t foreground;
	uint32_t background;
	uint64_t first_x;
	uint64_t first_y;
	uint64_t pixel_count;
	unsigned glyph_row;
	unsigned glyph_column;

	/* Decodes the character and attribute stored in the text cell. */
	character = (uint8_t)cell;
	attribute = (uint8_t)(cell >> 8);

	/* Rejects a cell outside the fixed console geometry. */
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return;

	/* Rejects an unavailable framebuffer backend. */
	if (framebuffer == NULL || framebuffer_pixels == NULL)
		return;

	/* Computes the pixel extent occupied by the requested cell. */
	first_x = (uint64_t)framebuffer_x + (uint64_t)column * 8U;
	first_y = (uint64_t)framebuffer_y +
	    (uint64_t)row * PCAT_VGAFONT_HEIGHT;
	pixel_count = (uint64_t)framebuffer->stride * framebuffer->height;

	/* Rejects a framebuffer with no usable stride. */
	if (framebuffer->stride == 0)
		return;

	/* Rejects a glyph wider than the visible framebuffer. */
	if (first_x + 8U > framebuffer->width)
		return;

	/* Rejects a glyph wider than the framebuffer stride. */
	if (first_x + 8U > framebuffer->stride)
		return;

	/* Rejects a glyph below the visible framebuffer. */
	if (first_y + PCAT_VGAFONT_HEIGHT > framebuffer->height)
		return;

	/* Rejects a framebuffer whose declared storage is too small. */
	if (pixel_count > framebuffer->size / sizeof(*framebuffer_pixels))
		return;

	/* Rejects a glyph whose last row exceeds the pixel extent. */
	if ((first_y + PCAT_VGAFONT_HEIGHT - 1U) *
	    framebuffer->stride + first_x + 8U > pixel_count) {
		return;
	}

	/* Inverts the cell colors while drawing the cursor. */
	if (cursor)
		attribute = (uint8_t)((attribute << 4) | (attribute >> 4));

	/* Resolves VGA attributes into framebuffer colors. */
	foreground = framebuffer_color_locked(attribute & 15U);
	background = framebuffer_color_locked((attribute >> 4) & 15U);

	/* Renders every row of the selected glyph. */
	for (glyph_row = 0;
	     glyph_row < PCAT_VGAFONT_HEIGHT;
	     glyph_row++) {
		bits = pcat_vgafont16[(unsigned)character *
		    PCAT_VGAFONT_HEIGHT + glyph_row];
		out = framebuffer_pixels +
		    (framebuffer_y + row * PCAT_VGAFONT_HEIGHT + glyph_row) *
		    framebuffer->stride + framebuffer_x + column * 8U;

		/* Renders all eight pixels in this glyph row. */
		for (glyph_column = 0; glyph_column < 8U; glyph_column++) {
			out[glyph_column] =
			    bits & (0x80U >> glyph_column) ?
			    foreground : background;
		}
	}
}

/* Writes one cell through the active console backend. */
static void
write_cell_locked(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	uint16_t cell;

	/* Ignores rendering while suspended or outside the console geometry. */
	if (console_suspended ||
	    row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS) {
		return;
	}

	/* Updates the framebuffer shadow and rendered cell when available. */
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		cell = (uint16_t)((uint8_t)character |
		    ((uint16_t)attribute << 8));
		framebuffer_cells[row * HAL_CONS_COLUMNS + column] = cell;
		framebuffer_draw_cell_locked(row, column, cell, 0);

		/* Completes the framebuffer-backed write. */
		return;
	}

	/* Writes the cell directly into VGA text memory. */
	VGA_MEMORY[row * HAL_CONS_COLUMNS + column] =
	    (uint16_t)((uint8_t)character | ((uint16_t)attribute << 8));
}

/* Updates the cursor through the active console backend. */
static void
update_cursor_locked(
	void)
{
	unsigned position;

	/* Computes the linear VGA cursor position. */
	position = cursor_row * HAL_CONS_COLUMNS + cursor_column;

	/* Leaves hardware state untouched while rendering is suspended. */
	if (console_suspended)
		return;

	/* Restores and redraws the framebuffer cursor when available. */
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		/* Restores the previously inverted cursor cell. */
		if (framebuffer_cursor_drawn &&
		    drawn_cursor_row < HAL_CONS_ROWS &&
		    drawn_cursor_column < HAL_CONS_COLUMNS) {
			framebuffer_draw_cell_locked(
				drawn_cursor_row,
				drawn_cursor_column,
				framebuffer_cells[drawn_cursor_row *
				HAL_CONS_COLUMNS + drawn_cursor_column],
				0);
		}

		/* Records whether the logical cursor should be drawn. */
		framebuffer_cursor_drawn = cursor_visible &&
		    cursor_row < HAL_CONS_ROWS &&
		    cursor_column < HAL_CONS_COLUMNS;

		/* Inverts the new cursor cell when it is visible. */
		if (framebuffer_cursor_drawn) {
			drawn_cursor_row = cursor_row;
			drawn_cursor_column = cursor_column;
			framebuffer_draw_cell_locked(
				cursor_row,
				cursor_column,
				framebuffer_cells[cursor_row * HAL_CONS_COLUMNS +
				cursor_column],
				1);
		}

		/* Completes the framebuffer cursor update. */
		return;
	}

	/* Programs the VGA cursor shape and position. */
	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, cursor_visible ? 0x0dU : 0x20U);
	asm_outb(VGA_INDEX, 0x0eU);
	asm_outb(VGA_DATA, (uint8_t)(position >> 8));
	asm_outb(VGA_INDEX, 0x0fU);
	asm_outb(VGA_DATA, (uint8_t)position);
}

/* Clears one row using the current text attribute. */
static void
clear_row_locked(
	unsigned row)
{
	unsigned column;

	/* Ignores a row outside the fixed console geometry. */
	if (row >= HAL_CONS_ROWS)
		return;

	/* Replaces every cell in the row with a space. */
	for (column = 0; column < HAL_CONS_COLUMNS; column++)
		write_cell_locked(row, column, ' ', current_attribute);
}

/* Clears the terminal and homes the cursor. */
static void
clear_locked(
	void)
{
	unsigned row;

	/* Clears every row of the fixed terminal. */
	for (row = 0; row < HAL_CONS_ROWS; row++)
		clear_row_locked(row);

	/* Homes and publishes the cursor. */
	cursor_row = 0;
	cursor_column = 0;
	update_cursor_locked();
}

/* Resets terminal attributes, mode, cursor, and contents. */
static void
reset_locked(
	void)
{
	/* Restores the default terminal presentation. */
	current_attribute = 0x07U;
	console_mode = HAL_CONS_TERMINAL;
	cursor_visible = 1;
	clear_locked();
}

/* Scrolls terminal contents upward by one row. */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;
	uint16_t cell;

	/* Scrolls and redraws the framebuffer shadow when active. */
	if (!console_suspended &&
	    framebuffer != NULL &&
	    framebuffer_pixels != NULL) {
		/* Copies every framebuffer row to its predecessor. */
		for (row = 1; row < HAL_CONS_ROWS; row++) {
			/* Copies every cell in this framebuffer row. */
			for (column = 0;
			     column < HAL_CONS_COLUMNS;
			     column++) {
				cell = framebuffer_cells[
				    row * HAL_CONS_COLUMNS + column];
				framebuffer_cells[
				    (row - 1U) * HAL_CONS_COLUMNS + column] =
				    cell;
				framebuffer_draw_cell_locked(
					row - 1U,
					column,
					cell,
					0);
			}
		}
	} else if (!console_suspended) {
		/* Copies every VGA row to its predecessor. */
		for (row = 1; row < HAL_CONS_ROWS; row++) {
			/* Copies every text cell in this VGA row. */
			for (column = 0;
			     column < HAL_CONS_COLUMNS;
			     column++) {
				VGA_MEMORY[
				    (row - 1U) * HAL_CONS_COLUMNS + column] =
				    VGA_MEMORY[row * HAL_CONS_COLUMNS + column];
			}
		}
	}

	/* Clears the vacated final row. */
	clear_row_locked(HAL_CONS_ROWS - 1U);
}

/* Advances output to the beginning of the next terminal row. */
static void
newline_locked(
	void)
{
	/* Advances the logical cursor and scrolls at the terminal bottom. */
	cursor_column = 0;
	if (++cursor_row >= HAL_CONS_ROWS) {
		scroll_locked();
		cursor_row = HAL_CONS_ROWS - 1U;
	}

	/* Publishes the cursor only while terminal mode owns presentation. */
	if (console_mode == HAL_CONS_TERMINAL)
		update_cursor_locked();
}

/* Writes one printable character at the logical cursor. */
static void
put_graphic_locked(
	int character)
{
	/* Wraps a cursor already beyond the visible row. */
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline_locked();

	/* Writes the glyph and advances the logical cursor. */
	write_cell_locked(
		cursor_row,
		cursor_column++,
		character,
		current_attribute);

	/* Wraps or publishes the advanced cursor. */
	if (cursor_column >= HAL_CONS_COLUMNS) {
		newline_locked();
	} else if (console_mode == HAL_CONS_TERMINAL) {
		update_cursor_locked();
	}
}

/* Interprets and writes one console byte. */
static void
putc_locked(
	int character)
{
	int printable;

	/* Mirrors early output to the QEMU debug console when configured. */
#ifdef HAL_PCAT_DEBUGCON
	asm_outb(0xe9U, (uint8_t)character);
#endif

	/* Handles a newline as a terminal row transition. */
	if (character == '\n') {
		newline_locked();
		return;
	}

	/* Handles carriage return without changing the row. */
	if (character == '\r') {
		cursor_column = 0;
		update_cursor_locked();
		return;
	}

	/* Erases one cell for a backspace operation. */
	if (character == '\b') {
		/* Moves left when the cursor is not already at column zero. */
		if (cursor_column != 0)
			cursor_column--;

		/* Erases the selected cell and publishes the updated cursor. */
		write_cell_locked(
			cursor_row,
			cursor_column,
			' ',
			current_attribute);
		update_cursor_locked();

		/* Completes the backspace operation. */
		return;
	}

	/* Expands a tab through the next eight-column boundary. */
	if (character == '\t') {
		/* Emits spaces until the cursor reaches the next tab stop. */
		do {
			put_graphic_locked(' ');
		} while ((cursor_column & 7U) != 0);

		/* Completes the expanded tab operation. */
		return;
	}

	/* Replaces non-ASCII control bytes with a visible placeholder. */
	printable = character >= 0x20 && character < 0x7f ?
	    character : '?';
	put_graphic_locked(printable);
}

/* Writes a bounded byte string while output ownership is held. */
static void
write_n_locked(
	const char *string,
	unsigned length)
{
	unsigned index;
	uint8_t byte;

	/* Starts at the first input byte. */
	index = 0;

	/* Ignores a missing input string. */
	if (string == NULL)
		return;

	/* Writes ASCII bytes and replaces each multibyte sequence once. */
	while (index < length) {
		byte = (uint8_t)string[index++];

		/* Emits ASCII directly and collapses a non-ASCII sequence. */
		if (byte < 0x80U) {
			putc_locked(byte);
		} else {
			/* Skips every continuation byte in this sequence. */
			while (index < length &&
			    ((uint8_t)string[index] & 0xc0U) == 0x80U) {
				index++;
			}

			/* Emits one placeholder for the skipped multibyte sequence. */
			putc_locked('?');
		}
	}
}

/* Writes bounded text at a fixed position while output is held. */
static int
write_n_at_locked(
	unsigned row,
	unsigned column,
	const char *string,
	unsigned length,
	uint8_t attribute)
{
	unsigned changed;
	unsigned index;
	uint8_t character;
	uint8_t selected_attribute;

	/* Initializes the rendered-cell count. */
	changed = 0;

	/* Rejects an invalid origin or missing input string. */
	if (row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS ||
	    string == NULL) {
		return -1;
	}

	/* Renders bytes until the input or fixed terminal geometry ends. */
	for (index = 0;
	     index < length && row < HAL_CONS_ROWS;
	     index++) {
		character = (uint8_t)string[index];

		/* Advances a newline to the next row. */
		if (character == '\n') {
			row++;
			column = 0;
			continue;
		}

		/* Returns carriage output to the first column. */
		if (character == '\r') {
			column = 0;
			continue;
		}

		/* Replaces non-ASCII bytes with a visible placeholder. */
		if (character >= 0x80U)
			character = '?';

		/* Stops before writing beyond the fixed row width. */
		if (column >= HAL_CONS_COLUMNS)
			break;

		/* Renders this byte with the selected text attribute. */
		selected_attribute = attribute ? attribute : 0x07U;
		write_cell_locked(
			row,
			column++,
			character,
			selected_attribute);
		changed++;
	}

	/* Leaves the logical cursor at the bounded final position. */
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ?
	    column : HAL_CONS_COLUMNS - 1U;

	/* Reports the number of cells changed. */
	return (int)changed;
}

/* Resolves a physical scan position to its key symbol. */
static const char *
scan_symbol(
	uint8_t scan,
	int extended)
{
	const char *symbol;

	/* Resolves a non-extended scan through the sparse identity table. */
	if (!extended)
		return scan_symbols[scan];

	/* Starts with no symbol for the extended scan position. */
	symbol = NULL;

	/* Resolves supported E0-prefixed scan positions. */
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
		break;
	}

	/* Reports the resolved symbol or an unsupported position. */
	return symbol;
}

/* Initializes one public key event. */
static void
set_event(
	struct hal_key_event *event,
	const char *symbol,
	uint32_t flags)
{
	unsigned index;

	/* Copies as much of the terminated symbol as the event can hold. */
	index = 0;
	while (index + 1U < HAL_KEY_SYMBOL_SIZE &&
	    symbol[index] != '\0') {
		event->symbol[index] = symbol[index];
		index++;
	}

	/* Terminates and clears the remainder of the symbol field. */
	while (index < HAL_KEY_SYMBOL_SIZE)
		event->symbol[index++] = '\0';

	/* Publishes the event classification flags. */
	event->flags = flags;
}

/* Compares two terminated key symbols. */
static int
symbol_equal(
	const char *left,
	const char *right)
{
	int equal;

	/* Skips the common prefix of both symbols. */
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	/* Tests whether both symbols ended at the same position. */
	equal = *left == *right;

	/* Reports whether the complete symbols match. */
	return equal;
}

/* Enqueues one keyboard event or rebuilds an overflowed snapshot. */
static void
enqueue_keyboard_event_locked(
	const char *symbol,
	uint32_t flags)
{
	unsigned next;

	/* Selects the next producer position in the event ring. */
	next = (event_head + 1U) % EVENT_COUNT;

	/* Rebuilds a truthful held-key snapshot instead of overwriting events. */
	if (next == event_tail) {
		rebuild_keyboard_events_locked();
		return;
	}

	/* Publishes the event at the current producer position. */
	set_event(&events[event_head], symbol, flags);
	event_head = next;
}

/* Tests whether a symbol is a snapshot-ordering modifier. */
static int
snapshot_modifier(
	const char *symbol)
{
	/* Recognizes the left shift key. */
	if (symbol_equal(symbol, "leftshift"))
		return 1;

	/* Recognizes the right shift key. */
	if (symbol_equal(symbol, "rightshift"))
		return 1;

	/* Recognizes the left control key. */
	if (symbol_equal(symbol, "leftctrl"))
		return 1;

	/* Recognizes the right control key. */
	if (symbol_equal(symbol, "rightctrl"))
		return 1;

	/* Recognizes the left alternate key. */
	if (symbol_equal(symbol, "leftalt"))
		return 1;

	/* Recognizes the right alternate key. */
	if (symbol_equal(symbol, "rightalt"))
		return 1;

	/* Recognizes the caps-lock key. */
	if (symbol_equal(symbol, "capslock"))
		return 1;

	/* Reports an ordinary held-key symbol. */
	return 0;
}

/* Waits until the 8042 can accept a command or data byte. */
static int
keyboard_wait_input_empty(
	void)
{
	unsigned spin;

	/* Bounds an absent or wedged legacy controller. */
	for (spin = 0; spin < KBD_WAIT_LOOPS; spin++) {
		if ((asm_inb(KBD_STATUS) & KBD_STATUS_INPUT) == 0)
			return HAL_OK;
		__asm__ volatile("pause");
	}

	/* Reports that the controller never accepted another byte. */
	return HAL_ERR_TIMEOUT;
}

/* Writes one 8042 controller command after its input buffer drains. */
static int
keyboard_write_command(
	uint8_t command)
{
	int status;

	status = keyboard_wait_input_empty();
	if (status != HAL_OK)
		return status;
	asm_outb(KBD_COMMAND, command);

	/* Reports successful command submission. */
	return HAL_OK;
}

/* Discards stale bytes left by firmware in the shared output buffer. */
static void
keyboard_flush_output(
	void)
{
	unsigned count;

	/* Both ports are disabled before this bounded drain begins. */
	for (count = 0; count < KBD_FLUSH_LIMIT; count++) {
		if ((asm_inb(KBD_STATUS) & KBD_STATUS_OUTPUT) == 0)
			break;
		(void)asm_inb(KBD_DATA);
	}
}

/* Reads the 8042 configuration byte without accepting an auxiliary byte. */
static int
keyboard_read_configuration(
	uint8_t *configuration)
{
	uint8_t status;
	unsigned spin;
	int result;

	result = keyboard_write_command(KBD_READ_CONFIGURATION);
	if (result != HAL_OK)
		return result;

	/* Waits for the controller response and drains any stale AUX byte. */
	for (spin = 0; spin < KBD_WAIT_LOOPS; spin++) {
		status = asm_inb(KBD_STATUS);
		if ((status & KBD_STATUS_OUTPUT) == 0) {
			__asm__ volatile("pause");
			continue;
		}
		if ((status & KBD_STATUS_AUX) != 0) {
			(void)asm_inb(KBD_DATA);
			continue;
		}
		*configuration = asm_inb(KBD_DATA);
		return HAL_OK;
	}

	/* Reports a controller which did not return its configuration. */
	return HAL_ERR_TIMEOUT;
}

/* Writes the 8042 configuration byte as one controller transaction. */
static int
keyboard_write_configuration(
	uint8_t configuration)
{
	int status;

	status = keyboard_write_command(KBD_WRITE_CONFIGURATION);
	if (status != HAL_OK)
		return status;
	status = keyboard_wait_input_empty();
	if (status != HAL_OK)
		return status;
	asm_outb(KBD_DATA, configuration);

	/* Reports successful configuration submission. */
	return HAL_OK;
}

/* Establishes the PC/AT keyboard port independently of firmware state. */
static int
keyboard_controller_init(
	void)
{
	uint8_t configuration;
	int status;

	/* Stops both sources before discarding firmware-owned output bytes. */
	status = keyboard_write_command(KBD_DISABLE_KEYBOARD);
	if (status != HAL_OK)
		return status;
	status = keyboard_write_command(KBD_DISABLE_AUX);
	if (status != HAL_OK)
		return status;
	status = keyboard_wait_input_empty();
	if (status != HAL_OK)
		return status;
	keyboard_flush_output();

	/* Selects translated set-1 input and leaves AUX disabled until opened. */
	status = keyboard_read_configuration(&configuration);
	if (status != HAL_OK)
		return status;
	configuration |= KBD_CONFIGURATION_KEYBOARD_IRQ |
	    KBD_CONFIGURATION_AUX_OFF |
	    KBD_CONFIGURATION_TRANSLATION;
	configuration &= (uint8_t)~(KBD_CONFIGURATION_AUX_IRQ |
	    KBD_CONFIGURATION_KEYBOARD_OFF);
	status = keyboard_write_configuration(configuration);
	if (status != HAL_OK)
		return status;

	/* Restarts the keyboard clock after its interrupt route is installed. */
	status = keyboard_write_command(KBD_ENABLE_KEYBOARD);
	if (status != HAL_OK)
		return status;
	return keyboard_wait_input_empty();
}

/* Rebuilds the event queue as a truthful held-key snapshot. */
static void
rebuild_keyboard_events_locked(
	void)
{
	const char *symbol;
	unsigned pass;
	unsigned extended;
	unsigned scan;
	unsigned state_index;

	/* Starts a new snapshot with the current lock state. */
	event_head = 0;
	event_tail = 0;
	set_event(
		&events[event_head],
		"",
		HAL_KEY_EVENT_RESYNC |
		(caps_lock ? HAL_KEY_EVENT_LOCK_CAPS : 0U));
	event_head = (event_head + 1U) % EVENT_COUNT;

	/* Emits modifiers before ordinary keys so the snapshot is truthful. */
	for (pass = 0; pass < 2U; pass++) {
		/* Examines base and E0-prefixed scan spaces. */
		for (extended = 0; extended < 2U; extended++) {
			/* Emits every held key belonging to this ordering pass. */
			for (scan = 0; scan < 128U; scan++) {
				state_index = extended * 16U + (scan >> 3);

				/* Skips keys which are not currently held. */
				if (((key_down[state_index] >>
				    (scan & 7U)) & 1U) == 0) {
					continue;
				}

				/* Resolves this held scan position to its public symbol. */
				symbol = scan_symbol(
					(uint8_t)scan,
					(int)extended);

				/* Skips scan positions without a public symbol. */
				if (symbol == NULL)
					continue;

				/* Selects modifiers first and ordinary keys second. */
				if (snapshot_modifier(symbol) != (pass == 0U))
					continue;

				/* Publishes this held key in the rebuilt snapshot. */
				set_event(
					&events[event_head],
					symbol,
					HAL_KEY_EVENT_PRESS |
					HAL_KEY_EVENT_SNAPSHOT);
				event_head = (event_head + 1U) % EVENT_COUNT;
			}
		}
	}

	/* Terminates the rebuilt snapshot. */
	set_event(&events[event_head], "", HAL_KEY_EVENT_RESYNC_END);
	event_head = (event_head + 1U) % EVENT_COUNT;
}

/* Drains available 8042 keyboard bytes into the event queue. */
static void
pump_keyboard_locked(
	void)
{
	const char *symbol;
	uint8_t status;
	uint8_t raw;
	uint8_t scan;
	int released;
	int extended;
	int was_down;
	unsigned state_index;
	uint32_t flags;

	/* Processes every available non-auxiliary byte. */
	for (;;) {
		status = asm_inb(KBD_STATUS);

		/* Stops when input is empty or belongs to the auxiliary device. */
		if ((status & 1U) == 0 ||
		    (status & KBD_STATUS_AUX) != 0) {
			break;
		}

		/* Reads the scan byte selected by the controller status. */
		raw = asm_inb(KBD_DATA);

		/* Records an E0 prefix for the next physical scan byte. */
		if (raw == 0xe0U) {
			e0_prefix = 1;
			continue;
		}

		/* Decodes the physical press or release position. */
		released = (raw & 0x80U) != 0;
		scan = raw & 0x7fU;
		extended = e0_prefix;
		e0_prefix = 0;
		state_index = (extended ? 16U : 0U) + (scan >> 3);
		was_down = (key_down[state_index] >> (scan & 7U)) & 1U;

		/* Updates the physical held-key bitmap. */
		if (released) {
			key_down[state_index] &=
			    (uint8_t)~(1U << (scan & 7));
		} else {
			key_down[state_index] |=
			    (uint8_t)(1U << (scan & 7));
		}

		/* Recomputes the public modifier state from physical keys. */
		shift_down =
		    ((key_down[0x2aU >> 3] >> (0x2aU & 7U)) |
		    (key_down[0x36U >> 3] >> (0x36U & 7U))) & 1U;
		ctrl_down =
		    ((key_down[0x1dU >> 3] >> (0x1dU & 7U)) |
		    (key_down[16U + (0x1dU >> 3)] >>
		    (0x1dU & 7U))) & 1U;
		alt_down =
		    ((key_down[0x38U >> 3] >> (0x38U & 7U)) |
		    (key_down[16U + (0x38U >> 3)] >>
		    (0x38U & 7U))) & 1U;

		/* Toggles caps lock only on a new press. */
		if (scan == 0x3aU && !released && !was_down)
			caps_lock = !caps_lock;

		/* Resolves the physical position to a public key symbol. */
		symbol = scan_symbol(scan, extended);

		/* Ignores physical positions without a public symbol. */
		if (symbol == NULL)
			continue;

		/* Classifies and enqueues this physical transition. */
		flags = released ? HAL_KEY_EVENT_RELEASE :
		    was_down ? HAL_KEY_EVENT_REPEAT : HAL_KEY_EVENT_PRESS;
		enqueue_keyboard_event_locked(symbol, flags);
	}
}

/* Handles one 8042 keyboard interrupt. */
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

	/* Starts with no detached input waiters. */
	waiters = NULL;

	/* Drains keyboard input and detaches waiters under queue serialization. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	pump_keyboard_locked();

	/* Selects all sleepers when at least one event is available. */
	if (event_head != event_tail)
		waiters = hal_cons_wait_queue_detach_all(&input_waiters);

	/* Releases input serialization after detaching eligible waiters. */
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Wakes consumers before completing the hardware interrupt. */
	hal_cons_wait_queue_notify_all(waiters);
	hal_irq_send_eoi(acknowledge);
}
