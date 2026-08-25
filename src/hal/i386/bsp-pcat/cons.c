/* PC/AT VGA text console and interrupt-driven 8042 keyboard.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../i386.h"
#include "../irq.h"
#include "../../cons-wait.h"

#define VGA_MEMORY ((volatile uint16_t *)(SYS_START + 0x000b8000U))
#define VGA_INDEX 0x3d4U
#define VGA_DATA  0x3d5U
#define KBD_DATA 0x60U
#define KBD_STATUS 0x64U
#define KBD_STATUS_AUX 0x20U
#define EVENT_COUNT 32U

static unsigned cursor_row, cursor_column;
static uint8_t current_attribute = 0x07U;
static int cursor_visible = 1;
static int console_suspended;
static enum hal_cons_mode console_mode = HAL_CONS_TERMINAL;
static struct hal_key_event events[EVENT_COUNT];
static unsigned event_head, event_tail;
static uint8_t key_down[32];
static int shift_down, ctrl_down, alt_down, caps_lock, e0_prefix;
static struct hal_cons_wait_queue input_waiters;

static void
write_cell(unsigned row, unsigned column, int character, uint8_t attribute)
{
	if (console_suspended)
		return;
	VGA_MEMORY[row * HAL_CONS_COLUMNS + column] =
	    (uint16_t)((uint8_t)character | ((uint16_t)attribute << 8));
}

void
hal_cons_update_cursor(void)
{
	unsigned position = cursor_row * HAL_CONS_COLUMNS + cursor_column;
	if (console_suspended)
		return;

	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, cursor_visible ? 0x0dU : 0x20U);
	asm_outb(VGA_INDEX, 0x0eU);
	asm_outb(VGA_DATA, (uint8_t)(position >> 8));
	asm_outb(VGA_INDEX, 0x0fU);
	asm_outb(VGA_DATA, (uint8_t)position);
}

void
hal_cons_clear_row(unsigned row)
{
	if (row >= HAL_CONS_ROWS) return;
	for (unsigned column = 0; column < HAL_CONS_COLUMNS; column++)
		write_cell(row, column, ' ', current_attribute);
}

void
hal_cons_clear(void)
{
	for (unsigned row = 0; row < HAL_CONS_ROWS; row++)
		hal_cons_clear_row(row);
	cursor_row = cursor_column = 0;
	hal_cons_update_cursor();
}

void
hal_cons_reset(void)
{
	current_attribute = 0x07U;
	console_mode = HAL_CONS_TERMINAL;
	cursor_visible = 1;
	hal_cons_clear();
}

static void
scroll(void)
{
	if (!console_suspended)
		for (unsigned row = 1; row < HAL_CONS_ROWS; row++)
			for (unsigned column = 0; column < HAL_CONS_COLUMNS; column++)
				VGA_MEMORY[(row - 1U) * HAL_CONS_COLUMNS + column] =
				    VGA_MEMORY[row * HAL_CONS_COLUMNS + column];
	hal_cons_clear_row(HAL_CONS_ROWS - 1U);
}

static void
newline(void)
{
	cursor_column = 0;
	if (++cursor_row >= HAL_CONS_ROWS) {
		scroll();
		cursor_row = HAL_CONS_ROWS - 1U;
	}
	if (console_mode == HAL_CONS_TERMINAL) hal_cons_update_cursor();
}

void
hal_cons_putc(int character)
{
#ifdef HAL_PCAT_DEBUGCON
	asm_outb(0xe9U, (uint8_t)character);
#endif
	if (character == '\n') { newline(); return; }
	if (character == '\r') { cursor_column = 0; hal_cons_update_cursor(); return; }
	if (character == '\b') {
		if (cursor_column != 0) cursor_column--;
		write_cell(cursor_row, cursor_column, ' ', current_attribute);
		hal_cons_update_cursor();
		return;
	}
	if (character == '\t') {
		do hal_cons_putc(' '); while ((cursor_column & 7U) != 0);
		return;
	}
	if (cursor_column >= HAL_CONS_COLUMNS) newline();
	write_cell(cursor_row, cursor_column++,
	    character >= 0x20 && character < 0x7f ? character : '?',
	    current_attribute);
	if (cursor_column >= HAL_CONS_COLUMNS) newline();
	else if (console_mode == HAL_CONS_TERMINAL) hal_cons_update_cursor();
}

void
hal_cons_write_n(const char *string, unsigned length)
{
	unsigned index = 0;
	if (string == 0) return;
	while (index < length) {
		uint8_t byte = (uint8_t)string[index++];
		if (byte < 0x80U) {
			hal_cons_putc(byte);
		} else {
			while (index < length &&
			    ((uint8_t)string[index] & 0xc0U) == 0x80U) index++;
			hal_cons_putc('?');
		}
	}
}

void
hal_cons_write(const char *string)
{
	unsigned length = 0;
	if (string == 0) return;
	while (string[length] != '\0') length++;
	hal_cons_write_n(string, length);
}

int
hal_cons_write_n_at(unsigned row, unsigned column, const char *string,
    unsigned length, uint8_t attribute)
{
	unsigned changed = 0;
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS || string == 0)
		return -1;
	for (unsigned index = 0; index < length && row < HAL_CONS_ROWS; index++) {
		uint8_t c = (uint8_t)string[index];
		if (c == '\n') { row++; column = 0; continue; }
		if (c == '\r') { column = 0; continue; }
		if (c >= 0x80U) c = '?';
		if (column >= HAL_CONS_COLUMNS) break;
		write_cell(row, column++, c, attribute ? attribute : 0x07U);
		changed++;
	}
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ? column : HAL_CONS_COLUMNS - 1U;
	return (int)changed;
}

int
hal_cons_write_at_attr(unsigned row, unsigned column, const char *string,
    uint8_t attribute)
{
	unsigned length = 0;
	if (string == 0) return -1;
	while (string[length] != '\0') length++;
	return hal_cons_write_n_at(row, column, string, length, attribute);
}

void hal_cons_write_at(unsigned row, unsigned column, const char *string)
{
	(void)hal_cons_write_at_attr(row, column, string, current_attribute);
}

void hal_cons_clear_to_eol(void)
{
	(void)hal_cons_clear_to_eol_at(cursor_row, cursor_column);
}

int hal_cons_clear_to_eol_at(unsigned row, unsigned column)
{
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS) return 0;
	for (unsigned current = column; current < HAL_CONS_COLUMNS; current++)
		write_cell(row, current, ' ', current_attribute);
	cursor_row = row; cursor_column = column;
	return 1;
}

int hal_cons_set_cursor(unsigned row, unsigned column)
{
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS) return 0;
	cursor_row = row; cursor_column = column; hal_cons_update_cursor();
	return 1;
}

void hal_cons_move_cursor(int line, int column)
{
	(void)hal_cons_set_cursor((unsigned)line, (unsigned)column);
}

void hal_cons_show_cursor(int visible)
{
	cursor_visible = visible != 0; hal_cons_update_cursor();
}

void hal_cons_save_state(struct hal_cons_state *state)
{
	if (state == 0) return;
	state->mode = console_mode; state->row = cursor_row;
	state->column = cursor_column; state->cursor_visible = cursor_visible;
}

void hal_cons_restore_terminal(const struct hal_cons_state *state)
{
	console_mode = HAL_CONS_TERMINAL;
	if (state != 0 && state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS) {
		cursor_row = state->row; cursor_column = state->column;
		cursor_visible = state->cursor_visible;
	}
	hal_cons_update_cursor();
}

void hal_cons_set_mode(enum hal_cons_mode mode) { console_mode = mode; }

void hal_cons_suspend(void)
{
	if (console_suspended)
		return;
	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, 0x20U);
	console_suspended = 1;
}

void hal_cons_resume(void)
{
	if (!console_suspended)
		return;
	console_suspended = 0;
	hal_cons_clear();
}

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

static const char *
scan_symbol(uint8_t scan, int extended)
{
	if (!extended)
		return scan_symbols[scan];
	switch (scan) {
	case 0x1d: return "rightctrl";
	case 0x38: return "rightalt";
	case 0x47: return "home";
	case 0x48: return "up";
	case 0x49: return "pageup";
	case 0x4b: return "left";
	case 0x4d: return "right";
	case 0x4f: return "end";
	case 0x50: return "down";
	case 0x51: return "pagedown";
	case 0x52: return "insert";
	case 0x53: return "delete";
	default: return NULL;
	}
}

static void
set_event(struct hal_key_event *event, const char *symbol, uint32_t flags)
{
	unsigned index = 0;
	while (index + 1U < HAL_KEY_SYMBOL_SIZE && symbol[index] != '\0') {
		event->symbol[index] = symbol[index];
		index++;
	}
	while (index < HAL_KEY_SYMBOL_SIZE)
		event->symbol[index++] = '\0';
	event->flags = flags;
}

static int
symbol_equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}
	return *left == *right;
}

unsigned hal_cons_modifiers(void)
{
	return (shift_down ? 1U : 0U) | (ctrl_down ? 2U : 0U) |
	    (alt_down ? 4U : 0U);
}

static void
pump_keyboard_locked(void)
{
	for (;;) {
		uint8_t status = asm_inb(KBD_STATUS);
		if ((status & 1U) == 0 || (status & KBD_STATUS_AUX) != 0)
			break;
		uint8_t raw = asm_inb(KBD_DATA), scan;
		int released, extended, was_down;
		const char *symbol;
		unsigned next, state_index;
		if (raw == 0xe0U) { e0_prefix = 1; continue; }
		released = (raw & 0x80U) != 0; scan = raw & 0x7fU;
		extended = e0_prefix; e0_prefix = 0;
		state_index = (extended ? 16U : 0U) + (scan >> 3);
		was_down = (key_down[state_index] >> (scan & 7U)) & 1U;
		if (released)
			key_down[state_index] &= (uint8_t)~(1U << (scan & 7));
		else
			key_down[state_index] |= (uint8_t)(1U << (scan & 7));
		shift_down = ((key_down[0x2aU >> 3] >> (0x2aU & 7U)) |
		    (key_down[0x36U >> 3] >> (0x36U & 7U))) & 1U;
		ctrl_down = ((key_down[0x1dU >> 3] >> (0x1dU & 7U)) |
		    (key_down[16U + (0x1dU >> 3)] >> (0x1dU & 7U))) & 1U;
		alt_down = ((key_down[0x38U >> 3] >> (0x38U & 7U)) |
		    (key_down[16U + (0x38U >> 3)] >> (0x38U & 7U))) & 1U;
		if (scan == 0x3aU && !released) caps_lock = !caps_lock;
		symbol = scan_symbol(scan, extended);
		if (symbol == NULL)
			continue;
		next = (event_head + 1U) % EVENT_COUNT;
		if (next == event_tail) continue;
		set_event(&events[event_head], symbol,
		    released ? HAL_KEY_EVENT_RELEASE :
		    was_down ? HAL_KEY_EVENT_REPEAT : HAL_KEY_EVENT_PRESS);
		event_head = next;
	}
}

static void
keyboard_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	struct hal_cons_wait_entry *waiters = NULL;
	bool enabled;

	(void)irq;
	(void)argument;
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	pump_keyboard_locked();
	if (event_head != event_tail)
		waiters = hal_cons_wait_queue_detach_all(&input_waiters);
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	hal_cons_wait_queue_notify_all(waiters);
	hal_irq_send_eoi(acknowledge);
}

int hal_cons_poll_event(struct hal_key_event *event)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);
	int available = event_head != event_tail;

	if (available && event != NULL)
		*event = events[event_tail];
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	return available;
}

int hal_cons_read_event(struct hal_key_event *event)
{
	struct hal_cons_wait_entry waiter;

	waiter.task = hal_task_get_current();
	waiter.next = NULL;
	waiter.queued = 0;
	for (;;) {
		bool enabled = hal_cons_wait_queue_lock(&input_waiters);

		if (event_head != event_tail) {
			if (event != NULL)
				*event = events[event_tail];
			event_tail = (event_tail + 1U) % EVENT_COUNT;
			hal_cons_wait_queue_unlock(&input_waiters, enabled);
			return 1;
		}
		hal_cons_wait_queue_add(&input_waiters, &waiter);
		hal_cons_wait_queue_unlock(&input_waiters, enabled);
		kernel_wait_task();
	}
}

int hal_cons_getc(void)
{
	struct hal_key_event event;
	for (;;) {
		(void)hal_cons_read_event(&event);
		if ((event.flags & (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0)
			continue;
		if (event.symbol[1] == '\0')
			return event.symbol[0];
		if (symbol_equal(event.symbol, "enter")) return '\r';
		if (symbol_equal(event.symbol, "tab")) return '\t';
		if (symbol_equal(event.symbol, "backspace")) return '\b';
		if (symbol_equal(event.symbol, "esc")) return 0x1b;
	}
}
int hal_cons_key_state(int key)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);
	int down = key == 0x170 ? shift_down : 0;

	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	return down;
}
void hal_cons_drain_input(void)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);

	event_tail = event_head;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
}

void i386_bsp_cons_init(void)
{
	event_head = event_tail = 0; shift_down = ctrl_down = alt_down = 0;
	caps_lock = e0_prefix = 0;
	for (unsigned i = 0; i < sizeof(key_down); i++) key_down[i] = 0;
	hal_cons_wait_queue_init(&input_waiters);
	hal_cons_reset();
}

void
i386_bsp_cons_irq_init(void)
{
	if (hal_irq_set_handler(IRQ_KEYBOARD, keyboard_interrupt, NULL) != HAL_OK)
		HAL_FATAL("PC/AT keyboard IRQ registration failed");
	hal_irq_unmask(IRQ_KEYBOARD);
}
