/* PC/AT VGA text console and polled 8042 keyboard.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../defs.h"

#define VGA_MEMORY ((volatile uint16_t *)((uintptr_t)AMD64_DIRECT_BASE + \
	0x000b8000U))
#define VGA_INDEX 0x3d4U
#define VGA_DATA  0x3d5U
#define KBD_DATA 0x60U
#define KBD_STATUS 0x64U
#define EVENT_COUNT 32U

static unsigned cursor_row, cursor_column;
static uint8_t current_attribute = 0x07U;
static int cursor_visible = 1;
static int console_suspended;
static enum hal_cons_mode console_mode = HAL_CONS_TERMINAL;
static unsigned events[EVENT_COUNT], event_head, event_tail;
static uint8_t key_down[32];
static int shift_down, ctrl_down, alt_down, caps_lock, e0_prefix;

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

static const char normal_map[128] = {
	[0x01]=27,[0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
	[0x07]='6',[0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',[0x0c]='-',
	[0x0d]='=',[0x0e]=8,[0x0f]=9,[0x10]='q',[0x11]='w',[0x12]='e',
	[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',
	[0x19]='p',[0x1a]='[',[0x1b]=']',[0x1c]=13,[0x1e]='a',[0x1f]='s',
	[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',
	[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',[0x2b]='\\',
	[0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',[0x31]='n',
	[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',[0x39]=' '
};

static const char shift_map[128] = {
	[0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',
	[0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',[0x0c]='_',[0x0d]='+',
	[0x1a]='{',[0x1b]='}',[0x27]=':',[0x28]='"',[0x29]='~',[0x2b]='|',
	[0x33]='<',[0x34]='>',[0x35]='?'
};

unsigned hal_cons_modifiers(void)
{
	return (shift_down ? HAL_KEY_EVENT_SHIFT : 0U) |
	    (ctrl_down ? HAL_KEY_EVENT_CTRL : 0U) |
	    (alt_down ? HAL_KEY_EVENT_GRAPH : 0U);
}

static int
scan_to_key(uint8_t scan, int extended)
{
	if (extended) {
		switch (scan) {
		case 0x48: return HAL_KEY_UP; case 0x50: return HAL_KEY_DOWN;
		case 0x4b: return HAL_KEY_LEFT; case 0x4d: return HAL_KEY_RIGHT;
		case 0x47: return HAL_KEY_HOME; case 0x4f: return HAL_KEY_END;
		case 0x49: return HAL_KEY_PAGE_UP; case 0x51: return HAL_KEY_PAGE_DOWN;
		case 0x52: return HAL_KEY_INSERT; case 0x53: return HAL_KEY_DELETE;
		default: return 0;
		}
	}
	if (scan >= 0x3b && scan <= 0x44) return HAL_KEY_F1 + scan - 0x3b;
	if (normal_map[scan] >= 'a' && normal_map[scan] <= 'z') {
		int upper = shift_down ^ caps_lock;
		int key = normal_map[scan];
		if (upper) key -= 'a' - 'A';
		if (ctrl_down) key = (key | 0x20) - 'a' + 1;
		return key;
	}
	return shift_down && shift_map[scan] ? shift_map[scan] : normal_map[scan];
}

static void
pump_keyboard(void)
{
	while (asm_inb(KBD_STATUS) & 1U) {
		uint8_t raw = asm_inb(KBD_DATA), scan;
		int released, key;
		unsigned next;
		if (raw == 0xe0U) { e0_prefix = 1; continue; }
		released = (raw & 0x80U) != 0; scan = raw & 0x7fU;
		if (released) key_down[scan >> 3] &= (uint8_t)~(1U << (scan & 7));
		else key_down[scan >> 3] |= (uint8_t)(1U << (scan & 7));
		if (scan == 0x2aU || scan == 0x36U) shift_down = !released;
		else if (scan == 0x1dU) ctrl_down = !released;
		else if (scan == 0x38U) alt_down = !released;
		else if (scan == 0x3aU && !released) caps_lock = !caps_lock;
		if (released) { e0_prefix = 0; continue; }
		key = scan_to_key(scan, e0_prefix); e0_prefix = 0;
		if (key == 0) continue;
		next = (event_head + 1U) % EVENT_COUNT;
		if (next == event_tail) continue;
		events[event_head] = ((unsigned)key & HAL_KEY_EVENT_KEY_MASK) |
		    hal_cons_modifiers();
		event_head = next;
	}
}

int hal_cons_poll_event(void)
{
	pump_keyboard();
	return event_head == event_tail ? -1 : (int)events[event_tail];
}

int hal_cons_read_event(void)
{
	int event;
	while ((event = hal_cons_poll_event()) < 0) ;
	event_tail = (event_tail + 1U) % EVENT_COUNT;
	return event;
}

int hal_cons_getc(void) { return hal_cons_read_event() & HAL_KEY_EVENT_KEY_MASK; }
int hal_cons_key_state(int key)
{
	(void)key; pump_keyboard();
	return key == HAL_KEY_SHIFT ? shift_down : 0;
}
void hal_cons_drain_input(void) { pump_keyboard(); event_tail = event_head; }

void pcat_cons_init(void)
{
	event_head = event_tail = 0; shift_down = ctrl_down = alt_down = 0;
	caps_lock = e0_prefix = 0;
	for (unsigned i = 0; i < sizeof(key_down); i++) key_down[i] = 0;
	hal_cons_reset();
}
