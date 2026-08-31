/* Early X68000 text console using TVRAM and the licensed CGROM image. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmio.h"

#include <string.h>

#define X68K_TVRAM_PLANE_BYTES 0x00020000U
#define X68K_TEXT_ROW_BYTES    128U
#define X68K_FONT_HEIGHT       16U
#define X68K_CGROM_8X16        X68K_DEVICE_ADDRESS(0x00f3a800U)
#define X68K_TEXT_PALETTE      X68K_DEVICE_ADDRESS(0x00e82200U)

struct x68k_console_cell {
	uint8_t character;
	uint8_t attribute;
};

static struct x68k_console_cell shadow[HAL_CONS_ROWS][HAL_CONS_COLUMNS];
static struct hal_cons_state state = { HAL_CONS_TERMINAL, 0, 0, 1 };
static uint8_t current_attribute = HAL_CONS_NORMAL_ATTRIBUTE;

static void
draw_cell(unsigned row, unsigned column)
{
	const struct x68k_console_cell *cell = &shadow[row][column];
	const volatile uint8_t *font = (const volatile uint8_t *)X68K_CGROM_8X16;
	unsigned character = cell->character;
	unsigned scanline;

	for (scanline = 0; scanline < X68K_FONT_HEIGHT; scanline++) {
		uintptr_t offset = row * X68K_FONT_HEIGHT * X68K_TEXT_ROW_BYTES +
		    scanline * X68K_TEXT_ROW_BYTES + column;
		uint8_t pattern = font[character * X68K_FONT_HEIGHT + scanline];
		unsigned plane;
		for (plane = 0; plane < 4U; plane++) {
			volatile uint8_t *vram = (volatile uint8_t *)
			    X68K_DEVICE_ADDRESS(X68K_TVRAM_PHYSICAL +
			    plane * X68K_TVRAM_PLANE_BYTES);
			vram[offset] = plane < 3U ? pattern : 0;
		}
	}
}

void hal_cons_update_cursor(void) {}

void
hal_cons_clear_row(unsigned row)
{
	unsigned column;
	if (row >= HAL_CONS_ROWS)
		return;
	for (column = 0; column < HAL_CONS_COLUMNS; column++) {
		shadow[row][column].character = ' ';
		shadow[row][column].attribute = current_attribute;
		draw_cell(row, column);
	}
}

void
hal_cons_clear(void)
{
	unsigned row;
	for (row = 0; row < HAL_CONS_ROWS; row++)
		hal_cons_clear_row(row);
	state.row = state.column = 0;
}

void
hal_cons_reset(void)
{
	current_attribute = HAL_CONS_NORMAL_ATTRIBUTE;
	state.mode = HAL_CONS_TERMINAL;
	state.cursor_visible = 1;
	hal_cons_clear();
}

static void
scroll(void)
{
	unsigned row, column;
	for (row = 1; row < HAL_CONS_ROWS; row++)
		for (column = 0; column < HAL_CONS_COLUMNS; column++)
			shadow[row - 1][column] = shadow[row][column];
	for (row = 0; row < HAL_CONS_ROWS - 1; row++)
		for (column = 0; column < HAL_CONS_COLUMNS; column++)
			draw_cell(row, column);
	hal_cons_clear_row(HAL_CONS_ROWS - 1);
}

static void
newline(void)
{
	state.column = 0;
	if (++state.row >= HAL_CONS_ROWS) {
		scroll();
		state.row = HAL_CONS_ROWS - 1;
	}
}

void
x68k_cons_init(void)
{
	/* Preserve the firmware CRTC timings.  Palette entry 0 remains black and
	 * entry 7 supplies the three-plane white used by this bootstrap console. */
	hal_mmio_write16((volatile void *)(X68K_TEXT_PALETTE + 0U), 0x0000U);
	hal_mmio_write16((volatile void *)(X68K_TEXT_PALETTE + 14U), 0xffffU);
	hal_cons_reset();
}

void cons_cls(void) { hal_cons_clear(); }

void
cons_putc(int character)
{
	if (character == '\n') {
		newline();
		return;
	}
	if (character == '\r') {
		state.column = 0;
		return;
	}
	if (character == '\b') {
		if (state.column != 0)
			state.column--;
		shadow[state.row][state.column].character = ' ';
		draw_cell(state.row, state.column);
		return;
	}
	if (character == '\t') {
		do {
			cons_putc(' ');
		} while ((state.column & 7U) != 0);
		return;
	}
	if (state.column >= HAL_CONS_COLUMNS)
		newline();
	shadow[state.row][state.column].character =
	    (uint8_t)(character >= 0x20 && character < 0x7f ? character : '?');
	shadow[state.row][state.column].attribute = current_attribute;
	draw_cell(state.row, state.column++);
	if (state.column >= HAL_CONS_COLUMNS)
		newline();
}

void cons_puts(const char *string) { if (string != NULL) while (*string != '\0') cons_putc(*string++); }
int cons_getc(void)
{
	static const struct {
		const char *symbol;
		unsigned char character;
	} legacy_jis[] = {
	    {"jis-1", '1'}, {"jis-2", '2'}, {"jis-3", '3'},
	    {"jis-4", '4'}, {"jis-5", '5'}, {"jis-6", '6'},
	    {"jis-7", '7'}, {"jis-8", '8'}, {"jis-9", '9'},
	    {"jis-0", '0'}, {"jis-minus", '-'}, {"jis-caret", '^'},
	    {"jis-yen", '\\'}, {"jis-at", '@'}, {"jis-lbrace", '['},
	    {"jis-semi", ';'}, {"jis-colon", ':'}, {"jis-rbrace", ']'},
	    {"jis-comma", ','}, {"jis-dot", '.'}, {"jis-slash", '/'},
	    {"jis-ro", '_'}, {"jis-kp-slash", '/'}, {"jis-kp-star", '*'},
	    {"jis-kp-minus", '-'}, {"jis-kp-7", '7'}, {"jis-kp-8", '8'},
	    {"jis-kp-9", '9'}, {"jis-kp-plus", '+'}, {"jis-kp-4", '4'},
	    {"jis-kp-5", '5'}, {"jis-kp-6", '6'}, {"jis-kp-equal", '='},
	    {"jis-kp-1", '1'}, {"jis-kp-2", '2'}, {"jis-kp-3", '3'},
	    {"jis-kp-enter", '\n'}, {"jis-kp-0", '0'},
	    {"jis-kp-comma", ','}, {"jis-kp-dot", '.'},
	};
	struct hal_key_event event;
	for (;;) {
		unsigned index;

		(void)hal_cons_read_event(&event);
		if ((event.flags & HAL_KEY_EVENT_SNAPSHOT) != 0)
			continue;
		if ((event.flags & (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_REPEAT)) == 0)
			continue;
		if (event.symbol[0] != '\0' && event.symbol[1] == '\0')
			return (unsigned char)event.symbol[0];
		if (event.symbol[0] == 'e' && event.symbol[1] == 'n' &&
		    event.symbol[2] == 't' && event.symbol[3] == 'e' &&
		    event.symbol[4] == 'r' && event.symbol[5] == '\0')
			return '\n';
		for (index = 0; index < sizeof(legacy_jis) / sizeof(legacy_jis[0]);
		    index++)
			if (strcmp(event.symbol, legacy_jis[index].symbol) == 0)
				return legacy_jis[index].character;
	}
}
void cons_set_attr(int foreground, int background)
{ current_attribute = (uint8_t)(((background & 15) << 4) | (foreground & 15)); }
void hal_cons_putc(int character) { cons_putc(character); }
void hal_cons_move_cursor(int row, int column)
{ (void)hal_cons_set_cursor((unsigned)row, (unsigned)column); }
int hal_cons_getc(void) { return cons_getc(); }
void hal_cons_set_mode(enum hal_cons_mode mode) { state.mode = mode; }
void hal_cons_write(const char *string) { cons_puts(string); }
void hal_cons_write_n(const char *string, unsigned length)
{ if (string != NULL) while (length-- != 0) cons_putc(*string++); }

int
hal_cons_write_n_at(unsigned row, unsigned column, const char *string,
	unsigned length, uint8_t attribute)
{
	unsigned changed = 0;
	if (string == NULL || row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return -1;
	while (length-- != 0 && row < HAL_CONS_ROWS) {
		uint8_t character = (uint8_t)*string++;
		if (character == '\n') { row++; column = 0; continue; }
		if (character == '\r') { column = 0; continue; }
		if (column >= HAL_CONS_COLUMNS)
			break;
		shadow[row][column].character = character < 0x80 ? character : '?';
		shadow[row][column].attribute = attribute != 0 ? attribute :
		    current_attribute;
		draw_cell(row, column++);
		changed++;
	}
	state.row = row < HAL_CONS_ROWS ? row : HAL_CONS_ROWS - 1;
	state.column = column < HAL_CONS_COLUMNS ? column : HAL_CONS_COLUMNS - 1;
	return (int)changed;
}

int hal_cons_write_at_attr(unsigned row, unsigned column, const char *string,
	uint8_t attribute)
{ return string == NULL ? -1 : hal_cons_write_n_at(row, column, string,
	(unsigned)hal_strlen(string), attribute); }
void hal_cons_write_at(unsigned row, unsigned column, const char *string)
{ (void)hal_cons_write_at_attr(row, column, string, current_attribute); }
int hal_cons_clear_to_eol_at(unsigned row, unsigned column)
{
	unsigned index;
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return 0;
	for (index = column; index < HAL_CONS_COLUMNS; index++) {
		shadow[row][index].character = ' ';
		shadow[row][index].attribute = current_attribute;
		draw_cell(row, index);
	}
	state.row = row;
	state.column = column;
	return 1;
}
void hal_cons_clear_to_eol(void)
{ (void)hal_cons_clear_to_eol_at(state.row, state.column); }
int hal_cons_set_cursor(unsigned row, unsigned column)
{
	if (row >= HAL_CONS_ROWS || column >= HAL_CONS_COLUMNS)
		return 0;
	state.row = row;
	state.column = column;
	return 1;
}
void hal_cons_show_cursor(int visible) { state.cursor_visible = visible != 0; }
void hal_cons_save_state(struct hal_cons_state *output)
{ if (output != NULL) *output = state; }
void hal_cons_restore_terminal(const struct hal_cons_state *input)
{
	state.mode = HAL_CONS_TERMINAL;
	if (input != NULL && input->row < HAL_CONS_ROWS &&
	    input->column < HAL_CONS_COLUMNS)
		state = *input;
}

void hal_cons_suspend(void) {}
void hal_cons_resume(void) {}
