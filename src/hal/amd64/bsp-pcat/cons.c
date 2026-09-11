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
#include "../../cons-keys.h"

#include <string.h>

#include "../../cons-wait.h"
#include "../asm.h"
#include "../defs.h"
#include "../irq.h"
#include "bootloader/include/amd64-handoff.h"
#include "drivers/platform/pcat/graphics/vgafont.h"

#define VGA_MEMORY vga_memory
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

static volatile uint16_t *vga_memory = (volatile uint16_t *)((uintptr_t)AMD64_IMAGE_BASE + 0xb8000U);
static unsigned cursor_row;
/*
 * One queued key event. The HAL interface passes the keysymbol and the
 * flags as separate parameters, so this record stays file-local.
 */
struct pcat_key_event {
	char symbol[HAL_KEY_SYMBOL_SIZE];
	uint32_t flags;
};

static unsigned cursor_column;
static uint8_t current_attribute = 0x07U;
static int cursor_visible = 1;
static int console_suspended;
static int event_mode;
static struct pcat_key_event events[EVENT_COUNT];
static unsigned event_head;
static unsigned event_tail;
static uint8_t key_down[32];
static struct hal_cons_wait_queue input_waiters;

static const struct zbl6_framebuffer *framebuffer;
static volatile uint32_t *framebuffer_pixels;
/*
 * Text console geometry. The 8x16 VGA font over the 640x480 firmware
 * framebuffer gives exactly 80 columns by 30 rows, so no centering band
 * remains above or below the text area.
 */
#define PCAT_CONS_COLUMNS	80U
#define PCAT_CONS_ROWS		30U

static uint16_t framebuffer_cells[PCAT_CONS_ROWS * PCAT_CONS_COLUMNS];
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
static int symbol_equal(const char *left, const char *right);

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
 * Writes a bounded byte string at the cursor.
 */
void
pcat_cons_write_n(
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
 * Writes a terminated byte string at the cursor.
 */
void
pcat_cons_write_string(
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
	pcat_cons_write_n(string, length);
}

/*
 * Writes bounded text at a fixed console position.
 */
static int
pcat_cons_write_n_at(
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
 * Writes a terminated UTF-8 string at a fixed console position with the
 * given attribute. Clipped at the end of the row; clearing is spaces.
 */
void
hal_cons_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	unsigned length;

	/* Ignores a missing input string. */
	if (utf8 == NULL)
		return;

	/* Measures the terminated input string. */
	length = 0;
	while (utf8[length] != '\0')
		length++;

	/* Writes the measured string with the requested attribute. */
	(void)pcat_cons_write_n_at(
		row,
		column,
		utf8,
		length,
		attribute);
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
	if (cursor_row < PCAT_CONS_ROWS &&
	    cursor_column < PCAT_CONS_COLUMNS) {
		/* Clears the remainder of the current row. */
		for (current = cursor_column;
		     current < PCAT_CONS_COLUMNS;
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
	if (row < PCAT_CONS_ROWS && column < PCAT_CONS_COLUMNS) {
		/* Clears the remainder of the requested row. */
		for (current = column;
		     current < PCAT_CONS_COLUMNS;
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
	if (row < PCAT_CONS_ROWS && column < PCAT_CONS_COLUMNS) {
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
 * Reports the cursor position and whether the cursor is visible.
 */
void
hal_cons_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	struct console_output_token token;

	/* Captures a consistent cursor snapshot. */
	token = console_output_lock();
	if (row != NULL)
		*row = cursor_row;
	if (column != NULL)
		*column = cursor_column;
	if (visible != NULL)
		*visible = cursor_visible;
	console_output_unlock(token);
}

/*
 * Reports the text console size in character cells.
 */
void
hal_cons_get_size(
	unsigned *cols,
	unsigned *rows)
{
	/* The geometry is fixed for this board. */
	if (cols != NULL)
		*cols = PCAT_CONS_COLUMNS;
	if (rows != NULL)
		*rows = PCAT_CONS_ROWS;
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
	console_test_framebuffer.width = PCAT_CONS_COLUMNS * 8U;
	console_test_framebuffer.height =
	    PCAT_CONS_ROWS * PCAT_VGAFONT_HEIGHT;
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
	cursor_row = PCAT_CONS_ROWS;
	hal_cons_putc(character);
	cursor_row = PCAT_CONS_ROWS - 1U;
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
	valid = cursor_row < PCAT_CONS_ROWS;

	/* Checks the column only after the cursor row proves valid. */
	if (valid)
		valid = cursor_column < PCAT_CONS_COLUMNS;

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
	struct pcat_key_event *event)
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
 * Copies one queued event into the caller's keysymbol and flags.
 */
static void
copy_event(
	const struct pcat_key_event *source,
	char *keysym,
	uint32_t *flags)
{
	unsigned index;

	/* Copies the NUL terminated keysymbol when requested. */
	if (keysym != NULL) {
		for (index = 0; index < HAL_KEY_SYMBOL_SIZE; index++)
			keysym[index] = source->symbol[index];
	}

	/* Copies the event flags when requested. */
	if (flags != NULL)
		*flags = source->flags;
}

/*
 * Enables or disables keyboard event mode.
 */
void
hal_cons_set_event_mode(
	int enable)
{
	struct console_output_token token;

	/* Serializes the mode change with all console rendering. */
	token = console_output_lock();
	event_mode = enable != 0;

	/* Terminal mode owns the visible cursor again. */
	if (!event_mode)
		update_cursor_locked();
	console_output_unlock(token);
}

/*
 * Tests whether a console key event is queued.
 */
int
hal_cons_poll_event(
	char *keysym,
	uint32_t *flags)
{
	bool enabled;
	int available;

	/* Inspects the queue under input serialization. */
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	available = event_head != event_tail;

	/* Copies the queued event without consuming it. */
	if (available)
		copy_event(&events[event_tail], keysym, flags);

	/* Releases input serialization after inspecting the queue. */
	hal_cons_wait_queue_unlock(&input_waiters, enabled);

	/* Reports whether an event is available. */
	return available;
}

/*
 * Waits for and consumes one console key event.
 */
int
hal_cons_read_event(
	char *keysym,
	uint32_t *flags)
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
			/* Copies the event out. */
			copy_event(&events[event_tail], keysym, flags);

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
	char symbol[HAL_KEY_SYMBOL_SIZE];
	uint32_t event_flags;

	/* Waits until a press or repeat event represents a text character. */
	for (;;) {
		(void)hal_cons_read_event(symbol, &event_flags);

		/* Ignores key-state snapshot events. */
		if ((event_flags & HAL_KEY_EVENT_SNAPSHOT) != 0)
			continue;

		/* Ignores events which do not produce text. */
		if ((event_flags &
		    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0) {
			continue;
		}

		/* Reports a one-byte key symbol directly. */
		if (symbol[1] == '\0')
			return symbol[0];

		/* Translates the enter key. */
		if (symbol_equal(symbol, "enter"))
			return '\r';

		/* Translates the tab key. */
		if (symbol_equal(symbol, "tab"))
			return '\t';

		/* Translates the backspace key. */
		if (symbol_equal(symbol, "backspace"))
			return '\b';

		/* Translates the escape key. */
		if (symbol_equal(symbol, "esc"))
			return 0x1b;
	}
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
prekern_pcat_cons_init(
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
	    framebuffer->width >= PCAT_CONS_COLUMNS * 8U &&
	    framebuffer->height >= PCAT_CONS_ROWS * PCAT_VGAFONT_HEIGHT &&
	    framebuffer->stride >= framebuffer->width &&
	    (uint64_t)framebuffer->stride * framebuffer->height <=
	    framebuffer->size / sizeof(*framebuffer_pixels)) {
		aligned = framebuffer->physical_base & ~0x1fffffULL;
		offset = framebuffer->physical_base - aligned;
		framebuffer_pixels = (volatile uint32_t *)(uintptr_t)
		    (ZBL6_FRAMEBUFFER_VIRTUAL_BASE + offset);
		framebuffer_x =
		    (framebuffer->width - PCAT_CONS_COLUMNS * 8U) / 2U;
		framebuffer_y = (framebuffer->height - PCAT_CONS_ROWS *
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

	/* Clears every physical held-key bit. */
	for (index = 0; index < sizeof(key_down); index++)
		key_down[index] = 0;

	/* Initializes the task wait queue. */
	hal_cons_wait_queue_init(&input_waiters);
}

/*
 * Leaves the PC/AT keyboard line to the kernel-side 8042 driver.
 *
 * The early console is output only, so the HAL no longer claims IRQ1 or
 * programs the controller. drivers/platform/pcat/ps2-8042.c owns the single
 * controller and publishes both evdev devices.
 */
void
prekern_pcat_cons_irq_init(
	void)
{
	/* Keeps the legacy line quiet until the kernel driver claims it. */
	hal_irq_mask(IRQ_KEYBOARD);
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
	if (row >= PCAT_CONS_ROWS || column >= PCAT_CONS_COLUMNS)
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
		bits = drv_pcat_vgafont16[(unsigned)character *
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
	    row >= PCAT_CONS_ROWS ||
	    column >= PCAT_CONS_COLUMNS) {
		return;
	}

	/* Updates the framebuffer shadow and rendered cell when available. */
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		cell = (uint16_t)((uint8_t)character |
		    ((uint16_t)attribute << 8));
		framebuffer_cells[row * PCAT_CONS_COLUMNS + column] = cell;
		framebuffer_draw_cell_locked(row, column, cell, 0);

		/* Completes the framebuffer-backed write. */
		return;
	}

	/* Writes the cell directly into VGA text memory. */
	VGA_MEMORY[row * PCAT_CONS_COLUMNS + column] =
	    (uint16_t)((uint8_t)character | ((uint16_t)attribute << 8));
}

/* Updates the cursor through the active console backend. */
static void
update_cursor_locked(
	void)
{
	unsigned position;

	/* Computes the linear VGA cursor position. */
	position = cursor_row * PCAT_CONS_COLUMNS + cursor_column;

	/* Leaves hardware state untouched while rendering is suspended. */
	if (console_suspended)
		return;

	/* Restores and redraws the framebuffer cursor when available. */
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		/* Restores the previously inverted cursor cell. */
		if (framebuffer_cursor_drawn &&
		    drawn_cursor_row < PCAT_CONS_ROWS &&
		    drawn_cursor_column < PCAT_CONS_COLUMNS) {
			framebuffer_draw_cell_locked(
				drawn_cursor_row,
				drawn_cursor_column,
				framebuffer_cells[drawn_cursor_row *
				PCAT_CONS_COLUMNS + drawn_cursor_column],
				0);
		}

		/* Records whether the logical cursor should be drawn. */
		framebuffer_cursor_drawn = cursor_visible &&
		    cursor_row < PCAT_CONS_ROWS &&
		    cursor_column < PCAT_CONS_COLUMNS;

		/* Inverts the new cursor cell when it is visible. */
		if (framebuffer_cursor_drawn) {
			drawn_cursor_row = cursor_row;
			drawn_cursor_column = cursor_column;
			framebuffer_draw_cell_locked(
				cursor_row,
				cursor_column,
				framebuffer_cells[cursor_row * PCAT_CONS_COLUMNS +
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
	if (row >= PCAT_CONS_ROWS)
		return;

	/* Replaces every cell in the row with a space. */
	for (column = 0; column < PCAT_CONS_COLUMNS; column++)
		write_cell_locked(row, column, ' ', current_attribute);
}

/* Clears the terminal and homes the cursor. */
static void
clear_locked(
	void)
{
	unsigned row;

	/* Clears every row of the fixed terminal. */
	for (row = 0; row < PCAT_CONS_ROWS; row++)
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
	event_mode = 0;
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
		for (row = 1; row < PCAT_CONS_ROWS; row++) {
			/* Copies every cell in this framebuffer row. */
			for (column = 0;
			     column < PCAT_CONS_COLUMNS;
			     column++) {
				cell = framebuffer_cells[
				    row * PCAT_CONS_COLUMNS + column];
				framebuffer_cells[
				    (row - 1U) * PCAT_CONS_COLUMNS + column] =
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
		for (row = 1; row < PCAT_CONS_ROWS; row++) {
			/* Copies every text cell in this VGA row. */
			for (column = 0;
			     column < PCAT_CONS_COLUMNS;
			     column++) {
				VGA_MEMORY[
				    (row - 1U) * PCAT_CONS_COLUMNS + column] =
				    VGA_MEMORY[row * PCAT_CONS_COLUMNS + column];
			}
		}
	}

	/* Clears the vacated final row. */
	clear_row_locked(PCAT_CONS_ROWS - 1U);
}

/* Advances output to the beginning of the next terminal row. */
static void
newline_locked(
	void)
{
	/* Advances the logical cursor and scrolls at the terminal bottom. */
	cursor_column = 0;
	if (++cursor_row >= PCAT_CONS_ROWS) {
		scroll_locked();
		cursor_row = PCAT_CONS_ROWS - 1U;
	}

	/* Publishes the cursor only while terminal mode owns presentation. */
	if (!event_mode)
		update_cursor_locked();
}

/* Writes one printable character at the logical cursor. */
static void
put_graphic_locked(
	int character)
{
	/* Wraps a cursor already beyond the visible row. */
	if (cursor_column >= PCAT_CONS_COLUMNS)
		newline_locked();

	/* Writes the glyph and advances the logical cursor. */
	write_cell_locked(
		cursor_row,
		cursor_column++,
		character,
		current_attribute);

	/* Wraps or publishes the advanced cursor. */
	if (cursor_column >= PCAT_CONS_COLUMNS) {
		newline_locked();
	} else if (!event_mode) {
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
	if (row >= PCAT_CONS_ROWS ||
	    column >= PCAT_CONS_COLUMNS ||
	    string == NULL) {
		return -1;
	}

	/* Renders bytes until the input or fixed terminal geometry ends. */
	for (index = 0;
	     index < length && row < PCAT_CONS_ROWS;
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
		if (column >= PCAT_CONS_COLUMNS)
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
	cursor_row = row < PCAT_CONS_ROWS ? row : PCAT_CONS_ROWS - 1U;
	cursor_column = column < PCAT_CONS_COLUMNS ?
	    column : PCAT_CONS_COLUMNS - 1U;

	/* Reports the number of cells changed. */
	return (int)changed;
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












/* Switches VGA access after the permanent uncached window becomes present. */
void
pcat_cons_paging_ready(void)
{
	vga_memory = (volatile uint16_t *)((uintptr_t)AMD64_LEGACY_MMIO_BASE + 0x18000U);
}
