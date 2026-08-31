/* PC/AT VGA text console and interrupt-driven 8042 keyboard.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "bootloader/include/amd64-handoff.h"
#include "drivers/graphics/pcat/vgafont.h"
#include "../asm.h"
#include "../defs.h"
#include "../irq.h"
#include "../../cons-wait.h"

#include <string.h>

#define VGA_MEMORY ((volatile uint16_t *)((uintptr_t)AMD64_DIRECT_BASE + \
	0x000b8000U))
#define VGA_INDEX 0x3d4U
#define VGA_DATA  0x3d5U
#define KBD_DATA 0x60U
#define KBD_STATUS 0x64U
#define KBD_STATUS_AUX 0x20U
/* 256 physical scan positions, two resync markers, and one ring sentinel. */
#define EVENT_COUNT 259U

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
static const struct zbl6_framebuffer *framebuffer;
static volatile uint32_t *framebuffer_pixels;
static uint16_t framebuffer_cells[HAL_CONS_ROWS * HAL_CONS_COLUMNS];
static unsigned framebuffer_x, framebuffer_y;
static unsigned drawn_cursor_row, drawn_cursor_column;
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
 * entry point calls a non-recursive _locked helper.  It remains supported so
 * a fault/NMI on the CPU which is already printing can report the failure
 * instead of deadlocking on itself.  IRQ disabling prevents migration while
 * a CPU owns the state.
 */
static uint64_t console_output_state;

struct console_output_token {
	uint32_t cpu;
	int interrupts_enabled;
};

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
static _Thread_local uint32_t console_test_cpu;
static _Thread_local int console_test_interrupts_enabled = 1;

static int
console_interrupt_disable(void)
{
	int enabled = console_test_interrupts_enabled;

	console_test_interrupts_enabled = 0;
	return enabled;
}

static void
console_interrupt_enable(void)
{
	console_test_interrupts_enabled = 1;
}

static uint32_t
console_cpu_identity(void)
{
	return console_test_cpu;
}
#else
static int
console_interrupt_disable(void)
{
	return hal_irq_disable() ? 1 : 0;
}

static void
console_interrupt_enable(void)
{
	hal_irq_enable();
}

static void
console_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
	uint32_t *ecx, uint32_t *edx)
{
	uint32_t a = leaf, b, c = subleaf, d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	*eax = a;
	*ebx = b;
	*ecx = c;
	*edx = d;
}

/* CPUID is available before IA32_GS_BASE/per-CPU state is selected. */
static uint32_t
console_cpu_identity(void)
{
	uint32_t eax, ebx, ecx, edx, maximum;

	console_cpuid(0, 0, &maximum, &ebx, &ecx, &edx);
	if (maximum >= 0x1fU) {
		console_cpuid(0x1fU, 0, &eax, &ebx, &ecx, &edx);
		if (ebx != 0)
			return edx;
	}
	if (maximum >= 0x0bU) {
		console_cpuid(0x0bU, 0, &eax, &ebx, &ecx, &edx);
		if (ebx != 0)
			return edx;
	}
	console_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
	return ebx >> 24;
}
#endif

static struct console_output_token
console_output_lock(void)
{
	struct console_output_token token;
	uint64_t observed, desired;

	token.interrupts_enabled = console_interrupt_disable();
	token.cpu = console_cpu_identity();
	observed = __atomic_load_n(&console_output_state, __ATOMIC_ACQUIRE);
	for (;;) {
		uint32_t depth = (uint32_t)observed;
		uint32_t owner = (uint32_t)(observed >> 32);

		if (depth == 0)
			desired = ((uint64_t)token.cpu << 32) | 1U;
		else if (owner == token.cpu) {
			if (depth == UINT32_MAX)
				__builtin_trap();
			desired = observed + 1U;
		} else {
			hal_atomic_relax();
			observed = __atomic_load_n(&console_output_state,
			    __ATOMIC_ACQUIRE);
			continue;
		}
		if (__atomic_compare_exchange_n(&console_output_state, &observed,
		    desired, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return token;
	}
}

static void
console_output_unlock(struct console_output_token token)
{
	uint64_t observed, desired;

	observed = __atomic_load_n(&console_output_state, __ATOMIC_RELAXED);
	for (;;) {
		uint32_t depth = (uint32_t)observed;
		uint32_t owner = (uint32_t)(observed >> 32);

		if (depth == 0 || owner != token.cpu)
			__builtin_trap();
		desired = depth == 1U ? 0 : observed - 1U;
		if (__atomic_compare_exchange_n(&console_output_state, &observed,
		    desired, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
			break;
	}
	if (token.interrupts_enabled)
		console_interrupt_enable();
}

uint64_t
pcat_cons_output_begin(void)
{
	struct console_output_token token = console_output_lock();

	return ((uint64_t)token.cpu << 32) |
	    (token.interrupts_enabled != 0 ? 1U : 0U);
}

void
pcat_cons_output_end(uint64_t encoded)
{
	struct console_output_token token;

	token.cpu = (uint32_t)(encoded >> 32);
	token.interrupts_enabled = (encoded & 1U) != 0;
	console_output_unlock(token);
}

static const uint32_t vga_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static uint32_t
framebuffer_color_locked(unsigned color)
{
	uint32_t rgb = vga_palette[color & 15U];

	if (framebuffer != NULL &&
	    framebuffer->format == ZBL6_FRAMEBUFFER_RGBX8888)
		return ((rgb & 0xff0000U) >> 16) | (rgb & 0x00ff00U) |
		    ((rgb & 0x0000ffU) << 16);
	return rgb;
}

static void
framebuffer_draw_cell_locked(unsigned row, unsigned column, uint16_t cell,
	int cursor)
{
	uint8_t character = (uint8_t)cell;
	uint8_t attribute = (uint8_t)(cell >> 8);
	uint32_t foreground, background;
	uint64_t first_x, first_y, pixel_count;
	unsigned glyph_row, glyph_column;

	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS ||
	    framebuffer == NULL || framebuffer_pixels == NULL)
		return;
	first_x = (uint64_t)framebuffer_x + (uint64_t)column * 8U;
	first_y = (uint64_t)framebuffer_y +
	    (uint64_t)row * PCAT_VGAFONT_HEIGHT;
	pixel_count = (uint64_t)framebuffer->stride * framebuffer->height;
	if (framebuffer->stride == 0 || first_x + 8U > framebuffer->width ||
	    first_x + 8U > framebuffer->stride ||
	    first_y + PCAT_VGAFONT_HEIGHT > framebuffer->height ||
	    pixel_count > framebuffer->size / sizeof(*framebuffer_pixels) ||
	    (first_y + PCAT_VGAFONT_HEIGHT - 1U) * framebuffer->stride +
	    first_x + 8U > pixel_count)
		return;
	if (cursor) {
		attribute = (uint8_t)((attribute << 4) | (attribute >> 4));
	}
	foreground = framebuffer_color_locked(attribute & 15U);
	background = framebuffer_color_locked((attribute >> 4) & 15U);
	for (glyph_row = 0; glyph_row < PCAT_VGAFONT_HEIGHT; glyph_row++) {
		uint8_t bits = pcat_vgafont16[(unsigned)character *
		    PCAT_VGAFONT_HEIGHT + glyph_row];
		volatile uint32_t *out = framebuffer_pixels +
		    (framebuffer_y + row * PCAT_VGAFONT_HEIGHT + glyph_row) *
		    framebuffer->stride + framebuffer_x + column * 8U;
		for (glyph_column = 0; glyph_column < 8U; glyph_column++)
			out[glyph_column] = bits & (0x80U >> glyph_column) ?
			    foreground : background;
	}
}

static void
write_cell_locked(unsigned row, unsigned column, int character,
	uint8_t attribute)
{
	if (console_suspended || row >= HAL_CONS_ROWS ||
	    column >= HAL_CONS_COLUMNS)
		return;
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		uint16_t cell = (uint16_t)((uint8_t)character |
		    ((uint16_t)attribute << 8));
		framebuffer_cells[row * HAL_CONS_COLUMNS + column] = cell;
		framebuffer_draw_cell_locked(row, column, cell, 0);
		return;
	}
	VGA_MEMORY[row * HAL_CONS_COLUMNS + column] =
	    (uint16_t)((uint8_t)character | ((uint16_t)attribute << 8));
}

static void
update_cursor_locked(void)
{
	unsigned position = cursor_row * HAL_CONS_COLUMNS + cursor_column;

	if (console_suspended)
		return;
	if (framebuffer != NULL && framebuffer_pixels != NULL) {
		if (framebuffer_cursor_drawn &&
		    drawn_cursor_row < HAL_CONS_ROWS &&
		    drawn_cursor_column < HAL_CONS_COLUMNS)
			framebuffer_draw_cell_locked(drawn_cursor_row,
			    drawn_cursor_column,
			    framebuffer_cells[drawn_cursor_row *
			    HAL_CONS_COLUMNS + drawn_cursor_column], 0);
		framebuffer_cursor_drawn = cursor_visible &&
		    cursor_row < HAL_CONS_ROWS &&
		    cursor_column < HAL_CONS_COLUMNS;
		if (framebuffer_cursor_drawn) {
			drawn_cursor_row = cursor_row;
			drawn_cursor_column = cursor_column;
			framebuffer_draw_cell_locked(cursor_row, cursor_column,
			    framebuffer_cells[cursor_row * HAL_CONS_COLUMNS +
			    cursor_column], 1);
		}
		return;
	}

	asm_outb(VGA_INDEX, 0x0aU);
	asm_outb(VGA_DATA, cursor_visible ? 0x0dU : 0x20U);
	asm_outb(VGA_INDEX, 0x0eU);
	asm_outb(VGA_DATA, (uint8_t)(position >> 8));
	asm_outb(VGA_INDEX, 0x0fU);
	asm_outb(VGA_DATA, (uint8_t)position);
}

void
hal_cons_update_cursor(void)
{
	struct console_output_token token = console_output_lock();

	update_cursor_locked();
	console_output_unlock(token);
}

static void
clear_row_locked(unsigned row)
{
	unsigned column;

	if (row >= HAL_CONS_ROWS)
		return;
	for (column = 0; column < HAL_CONS_COLUMNS; column++)
		write_cell_locked(row, column, ' ', current_attribute);
}

void
hal_cons_clear_row(unsigned row)
{
	struct console_output_token token = console_output_lock();

	clear_row_locked(row);
	console_output_unlock(token);
}

static void
clear_locked(void)
{
	unsigned row;

	for (row = 0; row < HAL_CONS_ROWS; row++)
		clear_row_locked(row);
	cursor_row = cursor_column = 0;
	update_cursor_locked();
}

void
hal_cons_clear(void)
{
	struct console_output_token token = console_output_lock();

	clear_locked();
	console_output_unlock(token);
}

static void
reset_locked(void)
{
	current_attribute = 0x07U;
	console_mode = HAL_CONS_TERMINAL;
	cursor_visible = 1;
	clear_locked();
}

void
hal_cons_reset(void)
{
	struct console_output_token token = console_output_lock();

	reset_locked();
	console_output_unlock(token);
}

static void
scroll_locked(void)
{
	if (!console_suspended && framebuffer != NULL &&
	    framebuffer_pixels != NULL) {
		for (unsigned row = 1; row < HAL_CONS_ROWS; row++)
			for (unsigned column = 0; column < HAL_CONS_COLUMNS;
			    column++) {
				uint16_t cell = framebuffer_cells[row *
				    HAL_CONS_COLUMNS + column];
				framebuffer_cells[(row - 1U) * HAL_CONS_COLUMNS +
				    column] = cell;
				framebuffer_draw_cell_locked(row - 1U, column,
				    cell, 0);
			}
	} else if (!console_suspended)
		for (unsigned row = 1; row < HAL_CONS_ROWS; row++)
			for (unsigned column = 0; column < HAL_CONS_COLUMNS; column++)
				VGA_MEMORY[(row - 1U) * HAL_CONS_COLUMNS + column] =
				    VGA_MEMORY[row * HAL_CONS_COLUMNS + column];
	clear_row_locked(HAL_CONS_ROWS - 1U);
}

static void
newline_locked(void)
{
	cursor_column = 0;
	if (++cursor_row >= HAL_CONS_ROWS) {
		scroll_locked();
		cursor_row = HAL_CONS_ROWS - 1U;
	}
	if (console_mode == HAL_CONS_TERMINAL)
		update_cursor_locked();
}

static void
put_graphic_locked(int character)
{
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline_locked();
	write_cell_locked(cursor_row, cursor_column++, character,
	    current_attribute);
	if (cursor_column >= HAL_CONS_COLUMNS)
		newline_locked();
	else if (console_mode == HAL_CONS_TERMINAL)
		update_cursor_locked();
}

static void
putc_locked(int character)
{
#ifdef HAL_PCAT_DEBUGCON
	asm_outb(0xe9U, (uint8_t)character);
#endif
	if (character == '\n') {
		newline_locked();
		return;
	}
	if (character == '\r') {
		cursor_column = 0;
		update_cursor_locked();
		return;
	}
	if (character == '\b') {
		if (cursor_column != 0)
			cursor_column--;
		write_cell_locked(cursor_row, cursor_column, ' ',
		    current_attribute);
		update_cursor_locked();
		return;
	}
	if (character == '\t') {
		do {
			put_graphic_locked(' ');
		} while ((cursor_column & 7U) != 0);
		return;
	}
	put_graphic_locked(character >= 0x20 && character < 0x7f ?
	    character : '?');
}

void
hal_cons_putc(int character)
{
	struct console_output_token token = console_output_lock();

	putc_locked(character);
	console_output_unlock(token);
}

static void
write_n_locked(const char *string, unsigned length)
{
	unsigned index = 0;

	if (string == NULL)
		return;
	while (index < length) {
		uint8_t byte = (uint8_t)string[index++];
		if (byte < 0x80U) {
			putc_locked(byte);
		} else {
			while (index < length &&
			    ((uint8_t)string[index] & 0xc0U) == 0x80U)
				index++;
			putc_locked('?');
		}
	}
}

void
hal_cons_write_n(const char *string, unsigned length)
{
	struct console_output_token token;

	if (string == NULL)
		return;
	token = console_output_lock();
	write_n_locked(string, length);
	console_output_unlock(token);
}

void
hal_cons_write(const char *string)
{
	unsigned length = 0;

	if (string == NULL)
		return;
	while (string[length] != '\0') length++;
	hal_cons_write_n(string, length);
}

static int
write_n_at_locked(unsigned row, unsigned column, const char *string,
	unsigned length, uint8_t attribute)
{
	unsigned changed = 0;

	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS || string == NULL)
		return -1;
	for (unsigned index = 0; index < length && row < HAL_CONS_ROWS; index++) {
		uint8_t c = (uint8_t)string[index];
		if (c == '\n') { row++; column = 0; continue; }
		if (c == '\r') { column = 0; continue; }
		if (c >= 0x80U) c = '?';
		if (column >= HAL_CONS_COLUMNS) break;
		write_cell_locked(row, column++, c,
		    attribute ? attribute : 0x07U);
		changed++;
	}
	cursor_row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1U;
	cursor_column = column < HAL_CONS_COLUMNS ? column : HAL_CONS_COLUMNS - 1U;
	return (int)changed;
}

int
hal_cons_write_n_at(unsigned row, unsigned column, const char *string,
    unsigned length, uint8_t attribute)
{
	struct console_output_token token;
	int changed;

	if (string == NULL)
		return -1;
	token = console_output_lock();
	changed = write_n_at_locked(row, column, string, length, attribute);
	console_output_unlock(token);
	return changed;
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
	struct console_output_token token;
	unsigned length = 0;

	if (string == NULL)
		return;
	while (string[length] != '\0')
		length++;
	token = console_output_lock();
	(void)write_n_at_locked(row, column, string, length,
	    current_attribute);
	console_output_unlock(token);
}

void hal_cons_clear_to_eol(void)
{
	struct console_output_token token = console_output_lock();
	unsigned current;

	if (cursor_row < HAL_CONS_ROWS && cursor_column < HAL_CONS_COLUMNS) {
		for (current = cursor_column; current < HAL_CONS_COLUMNS;
		    current++)
			write_cell_locked(cursor_row, current, ' ',
			    current_attribute);
	}
	console_output_unlock(token);
}

int hal_cons_clear_to_eol_at(unsigned row, unsigned column)
{
	struct console_output_token token = console_output_lock();
	unsigned current;
	int changed = 0;

	if (row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS) {
		for (current = column; current < HAL_CONS_COLUMNS; current++)
			write_cell_locked(row, current, ' ', current_attribute);
		cursor_row = row;
		cursor_column = column;
		changed = 1;
	}
	console_output_unlock(token);
	return changed;
}

int hal_cons_set_cursor(unsigned row, unsigned column)
{
	struct console_output_token token = console_output_lock();
	int changed = 0;

	if (row < HAL_CONS_ROWS && column < HAL_CONS_COLUMNS) {
		cursor_row = row;
		cursor_column = column;
		update_cursor_locked();
		changed = 1;
	}
	console_output_unlock(token);
	return changed;
}

void hal_cons_move_cursor(int line, int column)
{
	(void)hal_cons_set_cursor((unsigned)line, (unsigned)column);
}

void hal_cons_show_cursor(int visible)
{
	struct console_output_token token = console_output_lock();

	cursor_visible = visible != 0;
	update_cursor_locked();
	console_output_unlock(token);
}

void hal_cons_save_state(struct hal_cons_state *state)
{
	struct console_output_token token;

	if (state == NULL)
		return;
	token = console_output_lock();
	state->mode = console_mode;
	state->row = cursor_row;
	state->column = cursor_column;
	state->cursor_visible = cursor_visible;
	console_output_unlock(token);
}

void hal_cons_restore_terminal(const struct hal_cons_state *state)
{
	struct console_output_token token = console_output_lock();

	console_mode = HAL_CONS_TERMINAL;
	if (state != NULL && state->row < HAL_CONS_ROWS &&
	    state->column < HAL_CONS_COLUMNS) {
		cursor_row = state->row;
		cursor_column = state->column;
		cursor_visible = state->cursor_visible;
	}
	update_cursor_locked();
	console_output_unlock(token);
}

void hal_cons_set_mode(enum hal_cons_mode mode)
{
	struct console_output_token token = console_output_lock();

	console_mode = mode;
	console_output_unlock(token);
}

void hal_cons_suspend(void)
{
	struct console_output_token token = console_output_lock();

	if (console_suspended)
		goto out;
	if (framebuffer_pixels == NULL) {
		asm_outb(VGA_INDEX, 0x0aU);
		asm_outb(VGA_DATA, 0x20U);
	}
	console_suspended = 1;
out:
	console_output_unlock(token);
}

void hal_cons_resume(void)
{
	struct console_output_token token = console_output_lock();

	if (!console_suspended)
		goto out;
	console_suspended = 0;
	clear_locked();
out:
	console_output_unlock(token);
}

#ifdef ZEDBSD_CONSOLE_OUTPUT_TEST
static struct zbl6_framebuffer console_test_framebuffer;

void
pcat_console_output_test_set_cpu(uint32_t cpu)
{
	console_test_cpu = cpu;
}

void
pcat_console_output_test_reset(uint32_t *pixels, size_t pixel_count)
{
	struct console_output_token token;

	__atomic_store_n(&console_output_state, 0, __ATOMIC_RELAXED);
	console_test_interrupts_enabled = 1;
	token = console_output_lock();
	memset(&console_test_framebuffer, 0, sizeof(console_test_framebuffer));
	console_test_framebuffer.size = pixel_count * sizeof(*pixels);
	console_test_framebuffer.width = HAL_CONS_COLUMNS * 8U;
	console_test_framebuffer.height =
	    HAL_CONS_ROWS * PCAT_VGAFONT_HEIGHT;
	console_test_framebuffer.stride = console_test_framebuffer.width;
	console_test_framebuffer.format = ZBL6_FRAMEBUFFER_BGRX8888;
	framebuffer = &console_test_framebuffer;
	framebuffer_pixels = pixels;
	framebuffer_x = framebuffer_y = 0;
	framebuffer_cursor_drawn = 0;
	console_suspended = 0;
	reset_locked();
	console_output_unlock(token);
}

void
pcat_console_output_test_reentrant_transient(int character)
{
	struct console_output_token token = console_output_lock();

	/* Model a fault/NMI arriving after newline published its transient row. */
	cursor_row = HAL_CONS_ROWS;
	hal_cons_putc(character);
	cursor_row = HAL_CONS_ROWS - 1U;
	cursor_column = 0;
	console_output_unlock(token);
}

int
pcat_console_output_test_state(unsigned *row, unsigned *column)
{
	struct console_output_token token = console_output_lock();
	int valid = cursor_row < HAL_CONS_ROWS &&
	    cursor_column < HAL_CONS_COLUMNS && framebuffer != NULL &&
	    framebuffer_pixels != NULL;

	if (row != NULL)
		*row = cursor_row;
	if (column != NULL)
		*column = cursor_column;
	console_output_unlock(token);
	return valid;
}
#endif

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

static void rebuild_keyboard_events_locked(void);

static void
enqueue_keyboard_event_locked(const char *symbol, uint32_t flags)
{
	unsigned next = (event_head + 1U) % EVENT_COUNT;

	if (next == event_tail) {
		rebuild_keyboard_events_locked();
		return;
	}
	set_event(&events[event_head], symbol, flags);
	event_head = next;
}

static int
snapshot_modifier(const char *symbol)
{
	return symbol_equal(symbol, "leftshift") ||
	    symbol_equal(symbol, "rightshift") ||
	    symbol_equal(symbol, "leftctrl") ||
	    symbol_equal(symbol, "rightctrl") ||
	    symbol_equal(symbol, "leftalt") ||
	    symbol_equal(symbol, "rightalt") ||
	    symbol_equal(symbol, "capslock");
}

static void
rebuild_keyboard_events_locked(void)
{
	unsigned pass, extended, scan;

	event_head = event_tail = 0;
	set_event(&events[event_head], "", HAL_KEY_EVENT_RESYNC |
	    (caps_lock ? HAL_KEY_EVENT_LOCK_CAPS : 0U));
	event_head = (event_head + 1U) % EVENT_COUNT;
	/* Modifiers precede ordinary held keys so the snapshot is truthful. */
	for (pass = 0; pass < 2U; pass++)
		for (extended = 0; extended < 2U; extended++)
			for (scan = 0; scan < 128U; scan++) {
			const char *symbol;
			unsigned state_index = extended * 16U + (scan >> 3);

			if (((key_down[state_index] >> (scan & 7U)) & 1U) == 0)
				continue;
			symbol = scan_symbol((uint8_t)scan, (int)extended);
			if (symbol == NULL ||
			    snapshot_modifier(symbol) != (pass == 0U))
				continue;
			set_event(&events[event_head], symbol,
			    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT);
			event_head = (event_head + 1U) % EVENT_COUNT;
		}
	set_event(&events[event_head], "", HAL_KEY_EVENT_RESYNC_END);
	event_head = (event_head + 1U) % EVENT_COUNT;
}

#ifdef ZEDBSD_INPUT_OWNERSHIP_TEST
void
pcat_input_ownership_test_reset(void)
{
	memset(key_down, 0, sizeof(key_down));
	memset(events, 0, sizeof(events));
	event_head = event_tail = 0;
	caps_lock = shift_down = ctrl_down = alt_down = 0;
}

void
pcat_input_ownership_test_key(unsigned extended, unsigned scan, int down)
{
	unsigned state_index;

	if (extended > 1U || scan >= 128U)
		return;
	state_index = extended * 16U + (scan >> 3);
	if (down)
		key_down[state_index] |= (uint8_t)(1U << (scan & 7U));
	else
		key_down[state_index] &= (uint8_t)~(1U << (scan & 7U));
}

void
pcat_input_ownership_test_caps(int locked)
{
	caps_lock = locked != 0;
}

void
pcat_input_ownership_test_rebuild(void)
{
	rebuild_keyboard_events_locked();
}

void
pcat_input_ownership_test_repeat(const char *symbol)
{
	enqueue_keyboard_event_locked(symbol, HAL_KEY_EVENT_REPEAT);
}

int
pcat_input_ownership_test_pop(struct hal_key_event *event)
{
	if (event_tail == event_head)
		return 0;
	if (event != NULL)
		*event = events[event_tail];
	event_tail = (event_tail + 1U) % EVENT_COUNT;
	return 1;
}
#endif

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
		unsigned state_index;
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
		if (scan == 0x3aU && !released && !was_down)
			caps_lock = !caps_lock;
		symbol = scan_symbol(scan, extended);
		if (symbol == NULL)
			continue;
		enqueue_keyboard_event_locked(symbol,
		    released ? HAL_KEY_EVENT_RELEASE :
		    was_down ? HAL_KEY_EVENT_REPEAT : HAL_KEY_EVENT_PRESS);
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

void
hal_cons_get_input_info(struct hal_cons_input_info *info)
{
	static const char *const symbols[] = {
	    "esc", "backspace", "tab", "enter",
	    "leftshift", "rightshift", "leftctrl", "rightctrl",
	    "leftalt", "rightalt", "capslock", "home", "up", "pageup",
	    "left", "right", "end", "down", "pagedown", "insert",
	    "delete", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
	    "f8", "f9", "f10"};
	if (info == NULL)
		return;
	info->flags = HAL_CONS_INPUT_TEXT | HAL_CONS_INPUT_RELEASE |
	    HAL_CONS_INPUT_REPEAT;
	info->symbols = symbols;
	info->symbol_count = sizeof(symbols) / sizeof(symbols[0]);
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
		if ((event.flags & HAL_KEY_EVENT_SNAPSHOT) != 0)
			continue;
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

void pcat_cons_init(void)
{
	struct console_output_token token = console_output_lock();

	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	framebuffer_pixels = NULL;
	framebuffer_cursor_drawn = 0;
	if (framebuffer != NULL && framebuffer->width >=
	    HAL_CONS_COLUMNS * 8U && framebuffer->height >=
	    HAL_CONS_ROWS * PCAT_VGAFONT_HEIGHT &&
	    framebuffer->stride >= framebuffer->width &&
	    (uint64_t)framebuffer->stride * framebuffer->height <=
	    framebuffer->size / sizeof(*framebuffer_pixels)) {
		uint64_t aligned = framebuffer->physical_base & ~0x1fffffULL;
		uint64_t offset = framebuffer->physical_base - aligned;
		framebuffer_pixels = (volatile uint32_t *)(uintptr_t)
		    (ZBL6_FRAMEBUFFER_VIRTUAL_BASE + offset);
		framebuffer_x = (framebuffer->width - HAL_CONS_COLUMNS * 8U) / 2U;
		framebuffer_y = (framebuffer->height - HAL_CONS_ROWS *
		    PCAT_VGAFONT_HEIGHT) / 2U;
		for (uint64_t pixel = 0; pixel <
		    (uint64_t)framebuffer->stride * framebuffer->height; pixel++)
			framebuffer_pixels[pixel] = 0;
	}
	reset_locked();
	console_output_unlock(token);
	event_head = event_tail = 0; shift_down = ctrl_down = alt_down = 0;
	caps_lock = e0_prefix = 0;
	for (unsigned i = 0; i < sizeof(key_down); i++) key_down[i] = 0;
	hal_cons_wait_queue_init(&input_waiters);
}

void
pcat_cons_irq_init(void)
{
	if (hal_irq_set_handler(IRQ_KEYBOARD, keyboard_interrupt, NULL) != HAL_OK)
		HAL_FATAL("PC/AT keyboard IRQ registration failed");
	hal_irq_unmask(IRQ_KEYBOARD);
}
