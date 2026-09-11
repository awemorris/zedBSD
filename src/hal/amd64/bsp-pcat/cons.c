/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT early console.
 *
 * This is output only, and only until the kernel publishes kernel_putc.
 * It exists so the HAL and the earliest kernel initialization can report
 * diagnostics through hal_printf() before /dev/graphics and /dev/console
 * are up. The keyboard belongs to drivers/platform/pcat/ps2-8042.c and
 * the text grid to drivers/platform/pcat/graphics/text.c.
 */

#include <hal/hal.h>

#include <string.h>

#include "../asm.h"
#include "../defs.h"
#include "../irq.h"
#include "bootloader/include/amd64-handoff.h"
#include "drivers/platform/pcat/graphics/vgafont.h"

#define VGA_MEMORY vga_memory
#define VGA_INDEX	0x3d4U
#define VGA_DATA	0x3d5U

struct console_output_token {
	uint32_t cpu;
	int interrupts_enabled;
};

static volatile uint16_t *vga_memory = (volatile uint16_t *)((uintptr_t)AMD64_IMAGE_BASE + 0xb8000U);
static unsigned cursor_row;
static unsigned cursor_column;
static uint8_t current_attribute = 0x07U;
static int cursor_visible = 1;
static int console_suspended;

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

#ifdef KERN_CONSOLE_OUTPUT_TEST
static __thread uint32_t console_test_cpu;
static __thread int console_test_interrupts_enabled = 1;
#endif

static const uint32_t vga_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

#ifdef KERN_CONSOLE_OUTPUT_TEST
static struct zbl6_framebuffer console_test_framebuffer;
#endif

static int console_interrupt_disable(void);
static void console_interrupt_enable(void);
#ifndef KERN_CONSOLE_OUTPUT_TEST
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
 * Writes one character to the early console, or to the kernel console
 * once the kernel has published one.
 */
void
hal_putc(
	int character)
{
	struct console_output_token token;
	void (*kernel_output)(int c);

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(character);
		return;
	}

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

#ifdef KERN_CONSOLE_OUTPUT_TEST
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
	hal_putc(character);
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

#ifdef KERN_INPUT_OWNERSHIP_TEST

#endif

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

#ifdef KERN_CONSOLE_OUTPUT_TEST
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

	/* Publishes the advanced cursor. */
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
	} else {
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

/* Switches VGA access after the permanent uncached window becomes present. */
void
pcat_cons_paging_ready(void)
{
	vga_memory = (volatile uint16_t *)((uintptr_t)AMD64_LEGACY_MMIO_BASE + 0x18000U);
}
