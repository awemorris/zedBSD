/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 early console.
 *
 * This writes to text memory and exists so the HAL and the early part of
 * kernel start-up have somewhere to print. It is output only. It reads
 * its bytes as UTF-8 and shows ASCII, the half-width katakana and the
 * non-kanji rows of JIS X 0208 (symbols, kana, Greek and Cyrillic), which
 * is what the start-up banner and the diagnostics use; any other
 * character is shown as a question mark. Once the kernel publishes a
 * console through kernel_putc, every byte goes there instead and this
 * code stops touching the screen.
 *
 * Keyboard input is not handled here. The 8251 keyboard and the bus
 * mouse are separate devices on separate interrupts, and each has its
 * own driver.
 */

#include <hal/hal.h>

#include <kern/kcrt.h>

#include "../defs.h"
#include "../irq.h"

/* Characters are 16-bit words; their attributes live in a second plane. */
#define CONS_TEXT_VRAM		((volatile uint16_t *)0x800a0000)
#define CONS_ATTRIBUTE_VRAM	((volatile uint8_t *)0x800a2000)

/*
 * The attribute plane seen as one 16-bit word per cell; the attribute is
 * the low byte, and the high byte has no meaning.
 */
#define CONS_ATTRIBUTE_VRAM_WORDS	((volatile uint16_t *)0x800a2000)

#define CONS_COLUMNS		80U
#define CONS_ROWS		25U

/* Green, red and blue lit, and the cell marked visible. */
#define CONS_ATTRIBUTE		0xe1U

/*
 * A JIS X 0208 character takes two cells.  Both hold the row byte minus
 * 0x20 in the low byte and the cell byte in the high byte, and the right
 * half has bit 7 of the low byte set.
 *
 * Procedure referenced: FreeBSD 11.4 sys/pc98/cbus/scterm-sck.c
 * (kanji_convert and the two-cell store in the kanji output path).
 */
#define CONS_WIDE_RIGHT		0x0080U

/*
 * Half-width katakana U+FF61-U+FF9F are the single-byte codes 0xa1-0xdf
 * of JIS X 0201, which the text hardware shows in one cell.
 */
#define CONS_HALF_KANA_FIRST	0xff61U
#define CONS_HALF_KANA_LAST	0xff9fU
#define CONS_HALF_KANA_CODE	0xa1U

/* The JIS X 0208 rows the table below covers, each of 94 cells. */
#define CONS_JIS_ROWS		7U
#define CONS_JIS_CELLS		94U

/* The uPD7220 status and command ports, and its cursor-address command. */
#define GDC_STATUS		0x0060U
#define GDC_COMMAND		0x0062U
#define GDC_PARAMETER		0x0060U
#define GDC_CSRW		0x49U
#define GDC_CSRFORM		0x4bU

/*
 * Cursor form. Bit 7 of the first parameter shows the cursor; its low
 * five bits are the scan lines per character row minus one, which the
 * sixteen-line font fixes at 15. The display logic reads that field
 * too, so a wrong value repeats the screen instead of only losing the
 * cursor.
 */
#define GDC_CSRFORM_SHOW	0x8fU
#define GDC_CSRFORM_TOP		0x20U
#define GDC_CSRFORM_BOTTOM	0x7bU
#define GDC_FIFO_FULL		0x02U

/*
 * The Unicode scalar of each JIS X 0208 cell in rows 1-7, 94 cells to a
 * row; zero is an unassigned cell.  The kanji rows are left out: the
 * early console prints none, and the full table is the display driver's.
 *
 * Values referenced: NoctLang src/api/jisx0208.h (noct_jisx0208_to_ucs,
 * its first 658 entries).
 */
static const uint16_t jis_to_ucs[CONS_JIS_ROWS * CONS_JIS_CELLS] = {
	0x3000, 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x30FB, 0xFF1A, 0xFF1B,
	0xFF1F, 0xFF01, 0x309B, 0x309C, 0x00B4, 0xFF40, 0x00A8, 0xFF3E,
	0xFFE3, 0xFF3F, 0x30FD, 0x30FE, 0x309D, 0x309E, 0x3003, 0x4EDD,
	0x3005, 0x3006, 0x3007, 0x30FC, 0x2015, 0x2010, 0xFF0F, 0xFF3C,
	0x301C, 0x2016, 0xFF5C, 0x2026, 0x2025, 0x2018, 0x2019, 0x201C,
	0x201D, 0xFF08, 0xFF09, 0x3014, 0x3015, 0xFF3B, 0xFF3D, 0xFF5B,
	0xFF5D, 0x3008, 0x3009, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E,
	0x300F, 0x3010, 0x3011, 0xFF0B, 0x2212, 0x00B1, 0x00D7, 0x00F7,
	0xFF1D, 0x2260, 0xFF1C, 0xFF1E, 0x2266, 0x2267, 0x221E, 0x2234,
	0x2642, 0x2640, 0x00B0, 0x2032, 0x2033, 0x2103, 0xFFE5, 0xFF04,
	0x00A2, 0x00A3, 0xFF05, 0xFF03, 0xFF06, 0xFF0A, 0xFF20, 0x00A7,
	0x2606, 0x2605, 0x25CB, 0x25CF, 0x25CE, 0x25C7, 0x25C6, 0x25A1,
	0x25A0, 0x25B3, 0x25B2, 0x25BD, 0x25BC, 0x203B, 0x3012, 0x2192,
	0x2190, 0x2191, 0x2193, 0x3013, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2208,
	0x220B, 0x2286, 0x2287, 0x2282, 0x2283, 0x222A, 0x2229, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2227,
	0x2228, 0x00AC, 0x21D2, 0x21D4, 0x2200, 0x2203, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x2220, 0x22A5, 0x2312, 0x2202, 0x2207, 0x2261, 0x2252,
	0x226A, 0x226B, 0x221A, 0x223D, 0x221D, 0x2235, 0x222B, 0x222C,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x212B,
	0x2030, 0x266F, 0x266D, 0x266A, 0x2020, 0x2021, 0x00B6, 0x0000,
	0x0000, 0x0000, 0x0000, 0x25EF, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0xFF10, 0xFF11, 0xFF12, 0xFF13, 0xFF14,
	0xFF15, 0xFF16, 0xFF17, 0xFF18, 0xFF19, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0xFF21, 0xFF22, 0xFF23, 0xFF24,
	0xFF25, 0xFF26, 0xFF27, 0xFF28, 0xFF29, 0xFF2A, 0xFF2B, 0xFF2C,
	0xFF2D, 0xFF2E, 0xFF2F, 0xFF30, 0xFF31, 0xFF32, 0xFF33, 0xFF34,
	0xFF35, 0xFF36, 0xFF37, 0xFF38, 0xFF39, 0xFF3A, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0xFF41, 0xFF42, 0xFF43, 0xFF44,
	0xFF45, 0xFF46, 0xFF47, 0xFF48, 0xFF49, 0xFF4A, 0xFF4B, 0xFF4C,
	0xFF4D, 0xFF4E, 0xFF4F, 0xFF50, 0xFF51, 0xFF52, 0xFF53, 0xFF54,
	0xFF55, 0xFF56, 0xFF57, 0xFF58, 0xFF59, 0xFF5A, 0x0000, 0x0000,
	0x0000, 0x0000, 0x3041, 0x3042, 0x3043, 0x3044, 0x3045, 0x3046,
	0x3047, 0x3048, 0x3049, 0x304A, 0x304B, 0x304C, 0x304D, 0x304E,
	0x304F, 0x3050, 0x3051, 0x3052, 0x3053, 0x3054, 0x3055, 0x3056,
	0x3057, 0x3058, 0x3059, 0x305A, 0x305B, 0x305C, 0x305D, 0x305E,
	0x305F, 0x3060, 0x3061, 0x3062, 0x3063, 0x3064, 0x3065, 0x3066,
	0x3067, 0x3068, 0x3069, 0x306A, 0x306B, 0x306C, 0x306D, 0x306E,
	0x306F, 0x3070, 0x3071, 0x3072, 0x3073, 0x3074, 0x3075, 0x3076,
	0x3077, 0x3078, 0x3079, 0x307A, 0x307B, 0x307C, 0x307D, 0x307E,
	0x307F, 0x3080, 0x3081, 0x3082, 0x3083, 0x3084, 0x3085, 0x3086,
	0x3087, 0x3088, 0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308E,
	0x308F, 0x3090, 0x3091, 0x3092, 0x3093, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x30A1, 0x30A2, 0x30A3, 0x30A4, 0x30A5, 0x30A6, 0x30A7, 0x30A8,
	0x30A9, 0x30AA, 0x30AB, 0x30AC, 0x30AD, 0x30AE, 0x30AF, 0x30B0,
	0x30B1, 0x30B2, 0x30B3, 0x30B4, 0x30B5, 0x30B6, 0x30B7, 0x30B8,
	0x30B9, 0x30BA, 0x30BB, 0x30BC, 0x30BD, 0x30BE, 0x30BF, 0x30C0,
	0x30C1, 0x30C2, 0x30C3, 0x30C4, 0x30C5, 0x30C6, 0x30C7, 0x30C8,
	0x30C9, 0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D0,
	0x30D1, 0x30D2, 0x30D3, 0x30D4, 0x30D5, 0x30D6, 0x30D7, 0x30D8,
	0x30D9, 0x30DA, 0x30DB, 0x30DC, 0x30DD, 0x30DE, 0x30DF, 0x30E0,
	0x30E1, 0x30E2, 0x30E3, 0x30E4, 0x30E5, 0x30E6, 0x30E7, 0x30E8,
	0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EE, 0x30EF, 0x30F0,
	0x30F1, 0x30F2, 0x30F3, 0x30F4, 0x30F5, 0x30F6, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0391, 0x0392,
	0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398, 0x0399, 0x039A,
	0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0, 0x03A1, 0x03A3,
	0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x03B1, 0x03B2,
	0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA,
	0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0, 0x03C1, 0x03C3,
	0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8, 0x03C9, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0410, 0x0411, 0x0412, 0x0413,
	0x0414, 0x0415, 0x0401, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A,
	0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422,
	0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A,
	0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0430, 0x0431, 0x0432, 0x0433,
	0x0434, 0x0435, 0x0451, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A,
	0x043B, 0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442,
	0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A,
	0x044B, 0x044C, 0x044D, 0x044E, 0x044F, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000
};

/*
 * The cursor, the suspension flag and the partial UTF-8 sequence: all
 * read and written only while output_lock is held.  utf8_remaining
 * counts the continuation bytes still expected, and utf8_minimum is the
 * smallest scalar the sequence's length may encode.
 */
static unsigned cursor_row;
static unsigned cursor_column;
static volatile unsigned output_lock;
static int output_suspended;
static uint32_t utf8_scalar;
static unsigned utf8_remaining;
static uint32_t utf8_minimum;

static bool output_enter(void);
static void output_leave(bool interrupts_enabled);
static int gdc_write(uint16_t port, uint8_t value);
static void update_cursor_locked(void);
static void clear_row_locked(unsigned row);
static void clear_locked(void);
static void scroll_locked(void);
static void move_vram_words(volatile uint16_t *destination, volatile const uint16_t *source, unsigned count);
static void newline_locked(void);
static void putc_locked(int character);
static void put_byte_locked(int byte);
static void put_scalar_locked(uint32_t scalar);
static void put_cell_locked(uint16_t code, unsigned width);
static uint16_t scalar_to_code(uint32_t scalar, unsigned *width);

/*
 * Writes one character to the early console, or to the kernel console
 * once the kernel has published one.
 */
void
hal_putc(
	int character)
{
	void (*kernel_output)(int c);
	bool interrupts_enabled;

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(character);
		return;
	}

	/* Serializes character rendering with all console output. */
	interrupts_enabled = output_enter();
	put_byte_locked(character & 0xff);
	output_leave(interrupts_enabled);
}

/*
 * Acquires console-output ownership.
 *
 * The return value is the caller's interrupt state, which the matching
 * release needs; it is opaque to the caller.
 */
uint32_t
pc98_cons_output_begin(
	void)
{
	/* Reports the saved interrupt state as the release token. */
	return output_enter() ? 1U : 0U;
}

/*
 * Releases console-output ownership.
 */
void
pc98_cons_output_end(
	uint32_t token)
{
	/* Restores the interrupt state the matching acquire saved. */
	output_leave(token != 0);
}

/*
 * Stops and resumes writes to text memory.
 *
 * A display driver that takes the hardware into a graphics mode calls
 * this so late HAL diagnostics do not corrupt the screen.
 */
void
pc98_cons_suspend_output(
	void)
{
	bool interrupts_enabled;

	/* Publishes the suspension under the output lock. */
	interrupts_enabled = output_enter();
	output_suspended = 1;
	output_leave(interrupts_enabled);
}

void
pc98_cons_resume_output(
	void)
{
	bool interrupts_enabled;

	/* Restores text output and repaints a known state. */
	interrupts_enabled = output_enter();
	output_suspended = 0;
	clear_locked();
	output_leave(interrupts_enabled);
}

/*
 * Prepares the PC-98 early console.
 *
 * This runs before kernel_entry(), so it may not allocate or block.
 */
void
prekern_bsp_cons_init(
	void)
{
	/* Starts from a known screen and cursor position. */
	output_lock = 0;
	output_suspended = 0;
	cursor_row = 0;
	cursor_column = 0;
	utf8_scalar = 0;
	utf8_remaining = 0;
	utf8_minimum = 0;
	clear_locked();
}

/*
 * Leaves the keyboard interrupt masked for its driver.
 *
 * The early console does not read the keyboard. The line stays masked
 * until the 8251 driver registers, so a key pressed during start-up
 * cannot raise an interrupt nobody handles.
 */
void
prekern_bsp_cons_irq_init(
	void)
{
	/* Keeps the keyboard line quiet until its driver claims it. */
	hal_irq_mask(IRQ_KEYBOARD);
}

/* Acquires the output lock with interrupts disabled. */
static bool
output_enter(
	void)
{
	bool interrupts_enabled;

	/* Keeps rendering atomic against interrupt-context output. */
	interrupts_enabled = hal_irq_disable();

	/* Spins until this CPU owns the output lock. */
	while (__atomic_exchange_n(&output_lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("pause" : : : "memory");

	/* Reports the interrupt state the release has to restore. */
	return interrupts_enabled;
}

/* Releases the output lock and restores the caller's interrupt state. */
static void
output_leave(
	bool interrupts_enabled)
{
	/* Publishes every pending cell write before releasing. */
	__atomic_store_n(&output_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (interrupts_enabled)
		hal_irq_enable();
}

/* Issues one byte to the uPD7220 once its command FIFO has room. */
static int
gdc_write(
	uint16_t port,
	uint8_t value)
{
	uint8_t status;
	unsigned timeout;

	/* Polls the FIFO through a bounded legacy timeout. */
	for (timeout = 100000U; timeout != 0U; timeout--) {
		/* Stops as soon as the FIFO accepts another byte. */
		status = hal_io_inp8(GDC_STATUS);
		if ((status & GDC_FIFO_FULL) == 0U)
			break;
	}

	/* Reports failure without issuing a byte after timeout. */
	if (timeout == 0U)
		return 0;
	hal_io_outp8(port, value);

	/* Reports one completed port write. */
	return 1;
}

/* Moves the hardware cursor to the tracked position. */
static void
update_cursor_locked(
	void)
{
	unsigned address;

	/* Leaves the hardware alone while another driver owns the screen. */
	if (output_suspended)
		return;

	/* States the row height and shows the cursor. */
	if (!gdc_write(GDC_COMMAND, GDC_CSRFORM))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_SHOW))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_TOP))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_BOTTOM))
		return;

	/* Writes the cell address as the two-parameter cursor command. */
	address = cursor_row * CONS_COLUMNS + cursor_column;
	if (!gdc_write(GDC_COMMAND, GDC_CSRW))
		return;
	if (!gdc_write(GDC_PARAMETER, (uint8_t)(address & 0xffU)))
		return;
	(void)gdc_write(GDC_PARAMETER, (uint8_t)((address >> 8) & 0xffU));
}

/* Fills one row with blanks. */
static void
clear_row_locked(
	unsigned row)
{
	unsigned column;
	unsigned offset;

	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Publishes a blank cell and its attribute across the row. */
	for (column = 0; column < CONS_COLUMNS; column++) {
		offset = row * CONS_COLUMNS + column;
		CONS_TEXT_VRAM[offset] = (uint16_t)' ';
		CONS_ATTRIBUTE_VRAM[offset * 2U] = CONS_ATTRIBUTE;
	}
}

/* Clears the screen and homes the cursor. */
static void
clear_locked(
	void)
{
	unsigned row;

	/* Blanks every row in order. */
	for (row = 0; row < CONS_ROWS; row++)
		clear_row_locked(row);

	/* Returns the cursor to the top-left cell. */
	cursor_row = 0;
	cursor_column = 0;
	update_cursor_locked();
}

/* Scrolls the screen up by one row. */
static void
scroll_locked(
	void)
{
	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Copies every row up one position, characters and attributes. */
	move_vram_words(&CONS_TEXT_VRAM[0], &CONS_TEXT_VRAM[CONS_COLUMNS],
			(CONS_ROWS - 1U) * CONS_COLUMNS);
	move_vram_words(&CONS_ATTRIBUTE_VRAM_WORDS[0],
			&CONS_ATTRIBUTE_VRAM_WORDS[CONS_COLUMNS],
			(CONS_ROWS - 1U) * CONS_COLUMNS);

	/* Blanks the row exposed at the bottom. */
	clear_row_locked(CONS_ROWS - 1U);
}

/*
 * Moves words in text memory towards lower addresses.
 *
 * A string move does it in one instruction per plane; a loop of separate
 * loads and stores costs several times more on a 386 without a cache.
 * Moving forwards is correct for an overlapping range only while the
 * destination lies below the source, which a scroll up guarantees.
 */
static void
move_vram_words(
	volatile uint16_t *destination,
	volatile const uint16_t *source,
	unsigned count)
{
	/* Copies the words in ascending address order. */
	__asm__ volatile("cld\n\trep movsw"
			 : "+D"(destination), "+S"(source), "+c"(count)
			 :
			 : "memory");
}

/* Advances the cursor to the start of the next line. */
static void
newline_locked(
	void)
{
	/* Returns to the first column of the following row. */
	cursor_column = 0;
	cursor_row++;

	/* Scrolls instead of leaving the last row. */
	if (cursor_row >= CONS_ROWS) {
		scroll_locked();
		cursor_row = CONS_ROWS - 1U;
	}
}

/* Renders one character at the cursor. */
static void
putc_locked(
	int character)
{
	/* Handles the line and column control characters. */
	if (character == '\n') {
		newline_locked();
		update_cursor_locked();
		return;
	}
	if (character == '\r') {
		cursor_column = 0;
		update_cursor_locked();
		return;
	}
	if (character == '\b') {
		/* Steps back within the current row only. */
		if (cursor_column > 0)
			cursor_column--;
		update_cursor_locked();
		return;
	}
	if (character == '\t') {
		/* Advances to the next eight-column stop. */
		cursor_column = (cursor_column + 8U) & ~7U;

		/* Wraps to the next line when the stop leaves the row. */
		if (cursor_column >= CONS_COLUMNS)
			newline_locked();
		update_cursor_locked();
		return;
	}

	/* Publishes one ASCII cell. */
	put_cell_locked((uint16_t)(uint8_t)character, 1U);
	update_cursor_locked();
}

/*
 * Feeds one byte of UTF-8 to the console.
 *
 * A malformed, overlong or surrogate sequence is shown as one question
 * mark, and a byte that interrupts a sequence ends it.
 */
static void
put_byte_locked(
	int byte)
{
	/* Ends an unfinished sequence that a new character interrupts. */
	if (utf8_remaining != 0U && (byte & 0xc0) != 0x80) {
		utf8_remaining = 0;
		putc_locked('?');
	}

	/* Writes an ASCII byte or a control character directly. */
	if (byte < 0x80) {
		putc_locked(byte);
		return;
	}

	/* Adds a continuation byte to the open sequence. */
	if ((byte & 0xc0) == 0x80) {
		/* Shows a continuation byte without a sequence as a stray. */
		if (utf8_remaining == 0U) {
			putc_locked('?');
			return;
		}

		/* Waits for the rest of the sequence. */
		utf8_scalar = (utf8_scalar << 6) | (uint32_t)(byte & 0x3f);
		utf8_remaining--;
		if (utf8_remaining != 0U)
			return;

		/* Refuses an overlong form, a surrogate or a scalar too large. */
		if (utf8_scalar < utf8_minimum ||
		    utf8_scalar > 0x10ffffU ||
		    (utf8_scalar >= 0xd800U && utf8_scalar <= 0xdfffU)) {
			putc_locked('?');
			return;
		}

		/* Shows the completed character. */
		put_scalar_locked(utf8_scalar);
		update_cursor_locked();
		return;
	}

	/* Opens a sequence of the length the lead byte announces. */
	if ((byte & 0xe0) == 0xc0) {
		utf8_scalar = (uint32_t)(byte & 0x1f);
		utf8_remaining = 1;
		utf8_minimum = 0x80U;
	} else if ((byte & 0xf0) == 0xe0) {
		utf8_scalar = (uint32_t)(byte & 0x0f);
		utf8_remaining = 2;
		utf8_minimum = 0x800U;
	} else if ((byte & 0xf8) == 0xf0) {
		utf8_scalar = (uint32_t)(byte & 0x07);
		utf8_remaining = 3;
		utf8_minimum = 0x10000U;
	} else {
		/* A byte that cannot start a sequence is a stray. */
		putc_locked('?');
	}
}

/* Shows one Unicode scalar in the cells its text code needs. */
static void
put_scalar_locked(
	uint32_t scalar)
{
	unsigned width;
	uint16_t code;

	/* Converts the scalar and stores its cells. */
	code = scalar_to_code(scalar, &width);
	put_cell_locked(code, width);
}

/* Stores one character of one or two cells and advances the cursor. */
static void
put_cell_locked(
	uint16_t code,
	unsigned width)
{
	unsigned offset;

	/* Starts a two-cell character on the next row when it would split. */
	if (cursor_column + width > CONS_COLUMNS)
		newline_locked();

	/* Publishes the left cell and its attribute. */
	offset = cursor_row * CONS_COLUMNS + cursor_column;
	if (!output_suspended) {
		CONS_TEXT_VRAM[offset] = code;
		CONS_ATTRIBUTE_VRAM[offset * 2U] = CONS_ATTRIBUTE;
	}
	cursor_column++;

	/* Publishes the right half of a two-cell character. */
	if (width == 2U) {
		if (!output_suspended) {
			CONS_TEXT_VRAM[offset + 1U] =
			    (uint16_t)(code | CONS_WIDE_RIGHT);
			CONS_ATTRIBUTE_VRAM[(offset + 1U) * 2U] =
			    CONS_ATTRIBUTE;
		}
		cursor_column++;
	}

	/* Wraps to the next line at the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();
}

/* Maps one Unicode scalar to a text code and the cells it takes. */
static uint16_t
scalar_to_code(
	uint32_t scalar,
	unsigned *width)
{
	unsigned index;
	unsigned row;
	unsigned cell;

	/* Maps ASCII to one cell. */
	*width = 1;
	if (scalar < 0x80U)
		return (uint16_t)scalar;

	/* Maps a half-width katakana to its JIS X 0201 byte. */
	if (scalar >= CONS_HALF_KANA_FIRST && scalar <= CONS_HALF_KANA_LAST)
		return (uint16_t)(CONS_HALF_KANA_CODE + scalar - CONS_HALF_KANA_FIRST);

	/* Searches the non-kanji JIS X 0208 rows for the scalar. */
	for (index = 0; index < CONS_JIS_ROWS * CONS_JIS_CELLS; index++) {
		/* Skips a cell holding another scalar or none. */
		if (jis_to_ucs[index] == 0U || jis_to_ucs[index] != scalar)
			continue;

		/* Encodes the row and cell as the text hardware takes them. */
		row = 0x21U + index / CONS_JIS_CELLS;
		cell = 0x21U + index % CONS_JIS_CELLS;
		*width = 2;
		return (uint16_t)((cell << 8) | (row - 0x20U));
	}

	/* Shows a character outside the table as a question mark. */
	return (uint16_t)'?';
}
