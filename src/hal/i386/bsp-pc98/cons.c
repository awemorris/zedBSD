/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "hal/console.h"

#define TERMINAL_FIRST_ROW 18U

static volatile uint16_t *const text_vram =
	(volatile uint16_t *)0x000a0000;
static volatile uint8_t *const attribute_vram =
	(volatile uint8_t *)0x000a2000;
static enum hal_cons_mode console_mode;
static unsigned cursor_row;
static unsigned cursor_column;
static int cursor_visible;
static int software_cursor_drawn;
static unsigned software_cursor_offset;
static uint8_t software_cursor_attribute[2];

static uint8_t port_in8(uint16_t port)
{
	uint8_t value;

	asm volatile("inb %w1, %0" : "=a" (value) : "Nd" (port));
	return value;
}

static void port_out8(uint16_t port, uint8_t value)
{
	asm volatile("outb %0, %w1" : : "a" (value), "Nd" (port));
}

/* Wait for room in the uPD7220 FIFO before each command or parameter byte. */
static int gdc_write(uint16_t port, uint8_t value)
{
	unsigned timeout;

	for (timeout = 100000; timeout; timeout--)
		if (!(port_in8(0x60) & 0x02))
			break;
	if (!timeout)
		return 0;
	port_out8(port, value);
	return 1;
}

static void software_cursor_remove(void)
{
	if (!software_cursor_drawn)
		return;
	attribute_vram[software_cursor_offset * 2U] =
		software_cursor_attribute[0];
	attribute_vram[(software_cursor_offset + 1U) * 2U] =
		software_cursor_attribute[1];
	software_cursor_drawn = 0;
}

static void write_cell(unsigned row, unsigned column, uint16_t code,
		       uint8_t attribute)
{
	unsigned offset = row * HAL_CONS_COLUMNS + column;

	/* A double-cell software cursor may cover the cell being rewritten. */
	software_cursor_remove();
	text_vram[offset] = code;
	attribute_vram[offset * 2] = attribute;
}

void hal_cons_clear_row(unsigned row)
{
	if (row >= HAL_CONS_ROWS)
		return;
	for (unsigned column = 0; column < HAL_CONS_COLUMNS; column++)
		write_cell(row, column, ' ', HAL_CONS_NORMAL_ATTRIBUTE);
}

void hal_cons_clear(void)
{
	for (unsigned row = 0; row < HAL_CONS_ROWS; row++)
		hal_cons_clear_row(row);
}

void hal_cons_reset(void)
{
	hal_cons_clear();
	console_mode = HAL_CONS_FIXED_MENU;
	cursor_row = 0;
	cursor_column = 0;
	cursor_visible = 1;
}

static void scroll(void)
{
	for (unsigned row = 0; row + 1 < HAL_CONS_ROWS; row++)
		for (unsigned column = 0; column < HAL_CONS_COLUMNS;
		     column++) {
			unsigned destination = row * HAL_CONS_COLUMNS + column;
			unsigned source = destination + HAL_CONS_COLUMNS;

			text_vram[destination] = text_vram[source];
			attribute_vram[destination * 2] =
				attribute_vram[source * 2];
		}
	hal_cons_clear_row(HAL_CONS_ROWS - 1);
	cursor_row = HAL_CONS_ROWS - 1;
}

static void newline(void)
{
	cursor_column = 0;
	if (++cursor_row < HAL_CONS_ROWS)
		return;
	if (console_mode == HAL_CONS_TERMINAL)
		scroll();
	else
		cursor_row = HAL_CONS_ROWS - 1;
}

static void put_single_cell(uint16_t code)
{
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline();
	write_cell(cursor_row, cursor_column, code,
		   HAL_CONS_NORMAL_ATTRIBUTE);
	cursor_column++;
}

void hal_cons_putc(int character)
{
	uint8_t byte = (uint8_t)character;

	if (byte == '\n') {
		newline();
		return;
	}
	if (byte == '\r') {
		cursor_column = 0;
		return;
	}
	if (byte == '\b') {
		if (cursor_column) {
			cursor_column--;
			write_cell(cursor_row, cursor_column, ' ',
				   HAL_CONS_NORMAL_ATTRIBUTE);
		}
		return;
	}
	put_single_cell(byte);
}

/* HAL-owned JIS X 0208 rows 1-84; Noct is not part of this dependency. */
extern const uint16_t hal_pc98_jisx0208_to_ucs[7896];

static uint32_t utf8_codepoint(const char **input, const char *end)
{
	const uint8_t *p = (const uint8_t *)*input;
	uint32_t codepoint;
	unsigned count;

	if ((const char *)p >= end)
		return 0;
	if (p[0] < 0x80U) {
		*input = (const char *)(p + 1);
		return p[0];
	}
	if ((p[0] & 0xe0U) == 0xc0U) {
		codepoint = p[0] & 0x1fU;
		count = 2;
		if (codepoint < 2U)
			count = 0;
	} else if ((p[0] & 0xf0U) == 0xe0U) {
		codepoint = p[0] & 0x0fU;
		count = 3;
	} else if ((p[0] & 0xf8U) == 0xf0U) {
		codepoint = p[0] & 0x07U;
		count = 4;
		if (codepoint > 4U)
			count = 0;
	} else {
		codepoint = 0;
		count = 0;
	}
	if (count == 0 || end - (const char *)p < (int)count) {
		*input = (const char *)(p + 1);
		return '?';
	}
	for (unsigned index = 1; index < count; index++) {
		if ((p[index] & 0xc0U) != 0x80U) {
			*input = (const char *)(p + 1);
			return '?';
		}
		codepoint = (codepoint << 6) | (p[index] & 0x3fU);
	}
	*input = (const char *)(p + count);
	if ((count == 2 && codepoint < 0x80U) ||
	    (count == 3 && codepoint < 0x800U) ||
	    (count == 4 && codepoint < 0x10000U) || codepoint > 0x10ffffU ||
	    (codepoint >= 0xd800U && codepoint <= 0xdfffU))
		return '?';
	return codepoint;
}

static uint16_t unicode_to_pc98(uint32_t codepoint, unsigned *width)
{
	if (codepoint < 0x80U) {
		*width = 1;
		return (uint16_t)codepoint;
	}
	if (codepoint >= 0xff61U && codepoint <= 0xff9fU) {
		*width = 1;
		return (uint16_t)(0xa1U + codepoint - 0xff61U);
	}
	if (codepoint <= 0xffffU)
		for (unsigned index = 0; index < 7896U; index++)
			if (hal_pc98_jisx0208_to_ucs[index] == codepoint) {
				uint16_t ku = (uint16_t)(0x21U + index / 94U);
				uint16_t ten = (uint16_t)(0x21U + index % 94U);

				*width = 2;
				/* PC-98 text VRAM stores the one-based JIS row in
				 * the low byte and the cell (ten) in the high byte. */
				return (uint16_t)((ten << 8) | (ku - 0x20U));
			}
	*width = 1;
	return '?';
}

void hal_cons_write(const char *string)
{
	unsigned length = 0;

	if (string == 0)
		return;
	while (string[length] != '\0')
		length++;
	hal_cons_write_n(string, length);
}

void hal_cons_write_n(const char *string, unsigned length)
{
	const char *end;

	if (string == 0)
		return;
	end = string + length;
	while (string < end) {
		uint32_t codepoint = utf8_codepoint(&string, end);
		unsigned width;
		uint16_t code;

		if (codepoint == '\n' || codepoint == '\r' || codepoint == '\b') {
			hal_cons_putc((int)codepoint);
			continue;
		}
		code = unicode_to_pc98(codepoint, &width);
		if (cursor_column + width > HAL_CONS_COLUMNS)
			newline();
		write_cell(cursor_row, cursor_column++, code,
			   HAL_CONS_NORMAL_ATTRIBUTE);
		if (width == 2U)
			write_cell(cursor_row, cursor_column++, code | 0x8000U,
				   HAL_CONS_NORMAL_ATTRIBUTE);
	}
}

/* Positional writes deliberately leave the logical cursor after the string. */
void hal_cons_write_at(unsigned row, unsigned column,
		       const char *string)
{
	if (row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS)
		return;
	cursor_row = row;
	cursor_column = column;
	hal_cons_write(string);
}

void hal_cons_clear_to_eol(void)
{
	for (unsigned column = cursor_column;
	     column < HAL_CONS_COLUMNS; column++)
		write_cell(cursor_row, column, ' ',
			   HAL_CONS_NORMAL_ATTRIBUTE);
}

/*
 * Write one row without scrolling.  The return value is the number of text
 * cells changed; a double-byte character is never split at the right edge.
 */
int hal_cons_write_at_attr(unsigned row, unsigned column, const char *string,
			   uint8_t attribute)
{
	unsigned length = 0;

	if (row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS || string == 0)
		return -1;
	while (string[length] != '\0')
		length++;
	return hal_cons_write_n_at(row, column, string, length, attribute);
}

/* Positional UTF-8 output used by the Noct Term backend. */
int hal_cons_write_n_at(unsigned row, unsigned column,
			       const char *string, unsigned length,
			       uint8_t attribute)
{
	const char *position = string;
	const char *end = string + length;
	unsigned changed = 0;

	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS ||
	    string == 0)
		return -1;
	while (position < end && row < HAL_CONS_ROWS) {
		uint32_t codepoint = utf8_codepoint(&position, end);
		unsigned width;
		uint16_t code;

		if (codepoint == '\r') {
			column = 0;
			continue;
		}
		if (codepoint == '\n') {
			column = 0;
			row++;
			continue;
		}
		code = unicode_to_pc98(codepoint, &width);
		if (column + width > HAL_CONS_COLUMNS)
			break;
		write_cell(row, column++, code, attribute);
		changed++;
		if (width == 2U) {
			write_cell(row, column++, code | 0x8000U, attribute);
			changed++;
		}
	}
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ? column :
		HAL_CONS_COLUMNS - 1U;
	return (int)changed;
}

int hal_cons_clear_to_eol_at(unsigned row, unsigned column)
{
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return 0;
	for (unsigned current = column; current < HAL_CONS_COLUMNS;
	     current++)
		write_cell(row, current, ' ', HAL_CONS_NORMAL_ATTRIBUTE);
	cursor_row = row;
	cursor_column = column;
	return 1;
}

int hal_cons_set_cursor(unsigned row, unsigned column)
{
	if (row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS)
		return 0;
	cursor_row = row;
	cursor_column = column;
	hal_cons_update_cursor();
	return 1;
}

void hal_cons_show_cursor(int visible)
{
	cursor_visible = visible != 0;
	hal_cons_update_cursor();
}

void hal_cons_save_state(struct hal_cons_state *state)
{
	if (state == 0)
		return;
	state->mode = console_mode;
	state->row = cursor_row;
	state->column = cursor_column;
	state->cursor_visible = cursor_visible;
}

void hal_cons_restore_terminal(const struct hal_cons_state *state)
{
	unsigned output_row = cursor_row;
	unsigned output_column = cursor_column;

	console_mode = HAL_CONS_TERMINAL;
	if (output_row >= HAL_CONS_ROWS) {
		output_row = TERMINAL_FIRST_ROW;
		output_column = 0;
	}
	if (state != 0 && state->mode == HAL_CONS_TERMINAL &&
	    state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS &&
	    state->row > output_row) {
		cursor_row = state->row;
		cursor_column = state->column;
	} else {
		cursor_row = output_row;
		cursor_column = output_column;
	}
	if (cursor_column != 0)
		newline();
	cursor_visible = 1;
	hal_cons_update_cursor();
}

void hal_cons_set_mode(enum hal_cons_mode mode)
{
	console_mode = mode;
	if (mode == HAL_CONS_TERMINAL) {
		cursor_row = TERMINAL_FIRST_ROW;
		cursor_column = 0;
	}
}

/* Program CSRFORM as well as CSRW so firmware cannot leave the cursor hidden. */
void hal_cons_update_cursor(void)
{
	unsigned address = cursor_row * HAL_CONS_COLUMNS + cursor_column;
	int wide = 0;

	software_cursor_remove();
	if (cursor_visible && cursor_column + 1U < HAL_CONS_COLUMNS) {
		uint16_t left = text_vram[address];
		uint16_t right = text_vram[address + 1U];

		wide = !(left & 0x8000U) && (right & 0x8000U) &&
			((left & 0x7fffU) == (right & 0x7fffU));
	}

	if (!gdc_write(0x62, 0x4b) ||
	    !gdc_write(0x60, cursor_visible && !wide ? 0x8f : 0x0f) ||
	    !gdc_write(0x60, 0x20) ||
	    !gdc_write(0x60, 0x7b))
		return;
	/* The uPD7220 cursor form controls only vertical raster shape.  Render a
	 * full-width Japanese cursor by reversing both text cells while leaving
	 * the one-cell hardware cursor hidden. */
	if (wide) {
		software_cursor_offset = address;
		software_cursor_attribute[0] = attribute_vram[address * 2U];
		software_cursor_attribute[1] = attribute_vram[(address + 1U) * 2U];
		attribute_vram[address * 2U] =
			software_cursor_attribute[0] ^ 0x04U;
		attribute_vram[(address + 1U) * 2U] =
			software_cursor_attribute[1] ^ 0x04U;
		software_cursor_drawn = 1;
		return;
	}
	if (!cursor_visible ||
	    !gdc_write(0x62, 0x49) ||
	    !gdc_write(0x60, (uint8_t)address))
		return;
	gdc_write(0x60, (uint8_t)(address >> 8));
}

/* PC-98 keyboard scan translation and polled input. */
struct pc98_keyboard {
	uint8_t shift;
	uint8_t ctrl;
	uint8_t graph;
	uint8_t caps;
	uint8_t kana;
	uint8_t down[16];
};
/*
 * NEC PC-98 keyboard scancode translation
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Scancode assignments follow the PC-98 keyboard (linux-pc98
 * drivers/input/keyboard/pc98kbd.c) and agree with the inverse map the
 * loader already carried.  The base/shift tables are the JIS layout; a
 * boot environment needs ASCII and romaji (Remacs SKK), so the kana
 * plane is intentionally absent.
 */



#define SCAN_MAX 0x80U

#define SCAN_SHIFT_L 0x70U
#define SCAN_CAPS    0x71U
#define SCAN_KANA    0x72U
#define SCAN_GRAPH   0x73U
#define SCAN_CTRL    0x74U
#define SCAN_SHIFT_R 0x7dU

/* Unshifted characters.  0 means "not a printable key" (handled by the
 * special-key table or ignored). */
static const uint8_t base_table[SCAN_MAX] = {
	[0x01] = '1', [0x02] = '2', [0x03] = '3', [0x04] = '4', [0x05] = '5',
	[0x06] = '6', [0x07] = '7', [0x08] = '8', [0x09] = '9', [0x0a] = '0',
	[0x0b] = '-', [0x0c] = '^', [0x0d] = '\\',
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1a] = '@', [0x1b] = '[',
	[0x1d] = 'a', [0x1e] = 's', [0x1f] = 'd', [0x20] = 'f', [0x21] = 'g',
	[0x22] = 'h', [0x23] = 'j', [0x24] = 'k', [0x25] = 'l', [0x26] = ';',
	[0x27] = ':', [0x28] = ']',
	[0x29] = 'z', [0x2a] = 'x', [0x2b] = 'c', [0x2c] = 'v', [0x2d] = 'b',
	[0x2e] = 'n', [0x2f] = 'm', [0x30] = ',', [0x31] = '.', [0x32] = '/',
	[0x33] = '\\', [0x34] = ' ',
};

/* Shifted characters for the same keys. */
static const uint8_t shift_table[SCAN_MAX] = {
	[0x01] = '!', [0x02] = '"', [0x03] = '#', [0x04] = '$', [0x05] = '%',
	[0x06] = '&', [0x07] = '\'', [0x08] = '(', [0x09] = ')', [0x0a] = '0',
	[0x0b] = '=', [0x0c] = '~', [0x0d] = '|',
	[0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
	[0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
	[0x1a] = '`', [0x1b] = '{',
	[0x1d] = 'A', [0x1e] = 'S', [0x1f] = 'D', [0x20] = 'F', [0x21] = 'G',
	[0x22] = 'H', [0x23] = 'J', [0x24] = 'K', [0x25] = 'L', [0x26] = '+',
	[0x27] = '*', [0x28] = '}',
	[0x29] = 'Z', [0x2a] = 'X', [0x2b] = 'C', [0x2c] = 'V', [0x2d] = 'B',
	[0x2e] = 'N', [0x2f] = 'M', [0x30] = '<', [0x31] = '>', [0x32] = '?',
	[0x33] = '_', [0x34] = ' ',
};

static int
special_key(uint8_t scan)
{
	switch (scan) {
	case 0x00: return HAL_KEY_ESCAPE;
	case 0x0e: return HAL_KEY_BACKSPACE;
	case 0x0f: return HAL_KEY_TAB;
	case 0x1c: return HAL_KEY_ENTER;
	case 0x36: return HAL_KEY_PAGE_UP;
	case 0x37: return HAL_KEY_PAGE_DOWN;
	case 0x38: return HAL_KEY_INSERT;
	case 0x39: return HAL_KEY_DELETE;
	case 0x3a: return HAL_KEY_UP;
	case 0x3b: return HAL_KEY_LEFT;
	case 0x3c: return HAL_KEY_RIGHT;
	case 0x3d: return HAL_KEY_DOWN;
	case 0x3e: return HAL_KEY_HOME;
	case 0x3f: return HAL_KEY_END;
	case 0x62: return HAL_KEY_F1;
	case 0x63: return HAL_KEY_F2;
	case 0x64: return HAL_KEY_F3;
	case 0x65: return HAL_KEY_F4;
	case 0x66: return HAL_KEY_F5;
	case 0x67: return HAL_KEY_F6;
	case 0x68: return HAL_KEY_F7;
	case 0x69: return HAL_KEY_F8;
	case 0x6a: return HAL_KEY_F9;
	case 0x6b: return HAL_KEY_F10;
	default: return 0;
	}
}

/* Normalized key -> PC-98 scancode, for the real-time press query.
 * The inverse of the tables above; letters and digits map back through
 * their base entries. */
static int
key_to_scan(int key)
{
	unsigned scan;

	if (key <= 0)
		return -1;
	if (key >= 'A' && key <= 'Z')
		key = key - 'A' + 'a';
	switch (key) {
	case HAL_KEY_ESCAPE: return 0x00;
	case HAL_KEY_BACKSPACE: return 0x0e;
	case HAL_KEY_TAB: return 0x0f;
	case HAL_KEY_ENTER: return 0x1c;
	case HAL_KEY_INSERT: return 0x38;
	case HAL_KEY_DELETE: return 0x39;
	case HAL_KEY_UP: return 0x3a;
	case HAL_KEY_LEFT: return 0x3b;
	case HAL_KEY_RIGHT: return 0x3c;
	case HAL_KEY_DOWN: return 0x3d;
	case HAL_KEY_HOME: return 0x3e;
	case HAL_KEY_END: return 0x3f;
	case HAL_KEY_PAGE_UP: return 0x36;
	case HAL_KEY_PAGE_DOWN: return 0x37;
	case HAL_KEY_SHIFT: return SCAN_SHIFT_L;
	default: break;
	}
	if (key >= HAL_KEY_F1 && key <= HAL_KEY_F10)
		return 0x62 + (key - HAL_KEY_F1);
	for (scan = 0; scan < SCAN_MAX; scan++)
		if (base_table[scan] == (uint8_t)key)
			return (int)scan;
	return -1;
}

static void
pc98_keyboard_reset(struct pc98_keyboard *kb)
{
	unsigned i;

	kb->shift = kb->ctrl = kb->graph = kb->caps = kb->kana = 0;
	for (i = 0; i < sizeof(kb->down); i++)
		kb->down[i] = 0;
}

static void
set_down(struct pc98_keyboard *kb, uint8_t scan, int pressed)
{
	uint8_t mask = (uint8_t)(1U << (scan & 7U));

	if (pressed)
		kb->down[scan >> 3] |= mask;
	else
		kb->down[scan >> 3] &= (uint8_t)~mask;
}

static int
pc98_keyboard_feed(struct pc98_keyboard *kb, uint8_t raw)
{
	uint8_t scan = raw & 0x7fU;
	int pressed = (raw & 0x80U) == 0;
	uint8_t ch;

	set_down(kb, scan, pressed);

	switch (scan) {
	case SCAN_SHIFT_L:
	case SCAN_SHIFT_R:
		kb->shift = (uint8_t)pressed;
		return 0;
	case SCAN_CTRL:
		kb->ctrl = (uint8_t)pressed;
		return 0;
	case SCAN_GRAPH:
		kb->graph = (uint8_t)pressed;
		return 0;
	case SCAN_CAPS:
		/* Caps and kana are locking keys: toggle on the make. */
		if (pressed)
			kb->caps ^= 1U;
		return 0;
	case SCAN_KANA:
		if (pressed)
			kb->kana ^= 1U;
		return 0;
	default:
		break;
	}

	/* Releases and modifiers produce no key. */
	if (!pressed)
		return 0;

	{
		int special = special_key(scan);

		if (special != 0)
			return special;
	}

	ch = kb->shift ? shift_table[scan] : base_table[scan];
	if (ch == 0)
		return 0;

	/* Caps lock affects letters only. */
	if (kb->caps && ch >= 'a' && ch <= 'z')
		ch = (uint8_t)(ch - 'a' + 'A');
	else if (kb->caps && ch >= 'A' && ch <= 'Z')
		ch = (uint8_t)(ch - 'A' + 'a');

	/* Control collapses a letter to its control code. */
	if (kb->ctrl) {
		if (ch >= 'a' && ch <= 'z')
			return ch - 'a' + 1;
		if (ch >= 'A' && ch <= 'Z')
			return ch - 'A' + 1;
	}
	return ch;
}

static int
pc98_keyboard_is_down(const struct pc98_keyboard *kb, int key)
{
	int scan = key_to_scan(key);

	if (scan < 0)
		return -1;
	return (kb->down[scan >> 3] >> (scan & 7)) & 1;
}

/* NEC PC-98 polled keyboard driver.  SPDX-License-Identifier: Zlib */


#define KBD_DATA 0x41U
#define KBD_STATUS 0x43U
#define KBD_RXRDY 0x02U
#define QUEUE_SIZE 32U

static struct pc98_keyboard keyboard;
static unsigned events[QUEUE_SIZE];
static unsigned head, tail;

static uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

unsigned hal_cons_modifiers(void)
{
	return (keyboard.shift ? HAL_KEY_EVENT_SHIFT : 0) |
		(keyboard.ctrl ? HAL_KEY_EVENT_CTRL : 0) |
		(keyboard.graph ? HAL_KEY_EVENT_GRAPH : 0);
}

static void pump(void)
{
	while ((inb(KBD_STATUS) & KBD_RXRDY) != 0) {
		uint8_t raw = inb(KBD_DATA);
		int key = pc98_keyboard_feed(&keyboard, raw);
		unsigned next;

		if (key == 0)
			continue;
		next = (head + 1U) % QUEUE_SIZE;
		if (next == tail)
			continue;
		events[head] = ((unsigned)key & HAL_KEY_EVENT_KEY_MASK) |
			hal_cons_modifiers();
		head = next;
	}
}

int hal_cons_poll_event(void)
{
	pump();
	return tail == head ? -1 : (int)events[tail];
}

int hal_cons_read_event(void)
{
	int event;
	while ((event = hal_cons_poll_event()) < 0)
		;
	tail = (tail + 1U) % QUEUE_SIZE;
	return event;
}

int cons_getc(void)
{
	return hal_cons_read_event() & (int)HAL_KEY_EVENT_KEY_MASK;
}

int hal_cons_key_state(int key)
{
	pump();
	return pc98_keyboard_is_down(&keyboard, key);
}

void hal_cons_drain_input(void)
{
	pump();
	tail = head;
}

void bsp_cons_init(void)
{
	pc98_keyboard_reset(&keyboard);
	head = tail = 0;
	hal_cons_reset();
}

void cons_cls(void)
{
	hal_cons_clear();
}

void cons_putc(int character)
{
	hal_cons_putc(character);
}

void cons_puts(const char *utf8)
{
	hal_cons_write(utf8);
}

void cons_set_attr(int foreground, int background)
{
	(void)foreground;
	(void)background;
}
