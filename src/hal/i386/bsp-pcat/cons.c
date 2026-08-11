/*
 * BSP: PC/AT Console
 */

#include <sys/kern/hal.h>
#include <hal/runtime.h>

#include "../i386/bsp.h"

/*
 * Text VRAM address.
 */
#define VRAM_ADDR		(0xb8000)

/*
 * VRAM character.
 */
#define MK_VRAMCHAR(c, attr)	((c) | ((attr) << 8))

/*
 * Keyboard buffer size.
 */
#define KBD_BUF_SIZE		(256)

/*
 * Screen settings.
 */
uint16_t *vram;
static int screen_columns;
static int screen_lines;

/*
 * Console status.
 */
static int cur_col;
static int cur_line;
static uint8_t cur_attr;

/*
 * Keyboard buffer.
 */
static int kbd_buf[KBD_BUF_SIZE];
static int kbd_buf_len = 0;

/*
 * Forward declaration.
 */
static void scroll_line(void);
static int scancode_to_char(int scancode);
static void keyboard_handler(void *p);

/*
 * Initialize the cons module.
 */
void
bsp_cons_init(void)
{
	/* TODO: Determine the graphic mode. */
	vram = (uint16_t *)(VRAM_ADDR + SYS_START);
	screen_columns = 80;
	screen_lines = 25;

	cur_col = 0;
	cur_line = 0;
	cur_attr = 0x07;

	clear_screen();

	bsp_irq_set_handler(IRQ_KEYBOARD, keyboard_handler);
}

/*
 * Put a character.
 */
void
bsp_cons_putc(
	int c)
{
	switch (c) {
	case '\n':
		/* Line feed. */
		cur_line++;
		cur_col = 0;
		break;
	default:
		/* Write a character. */
		*(vram + cur_col + cur_line*columns) = MK_VRAMCHAR(c, cur_attr);
		cur_col++;
		break;
	}

	/* Line feed. */
	if(cur_col == columns)
		cur_col = 0, cur_line++;

	/* Scroll. */
	if(cur_line == lines) {
		scroll_line();
		cur_line = lines - 1, cur_col = 0;
	}

	/* Update the cursor position. */
	set_cursor_pos(cur_line, cur_col);
}

/*
 * Get a character.
 */
int
bsp_cons_getc(void)
{
	uint8  scancode;

	while (kbd_buf_len == 0)
		asm_hlt();

	/* TODO: Rotate kbd_buf. */
	scancode = kbd_buf[0];
	kbd_buf_len = 0;

	return scancode_to_char(scancode);
}

/*
 * Clear the console.
 */
void
bsp_cons_clear(void)
{
	uint16_t space_char;

	space_char = MK_VRAMCHAR(' ', cur_attr);

	memset16(vram, space_char, screen_columns * screen_lines);

	hal_cons_move_cursor(0, 0);
}

/*
 * Move the cursor.
 */
void
bsp_cons_move_cursor(
	int line,
	int col)
{
	uint32 addr;

	addr = col + line * screen_columns;

	asm_outb(0x3d4, 0x0e);
	asm_outb(0x3d5, addr >> 8);
	asm_outb(0x3d4, 0x0f);
	asm_outb(0x3d5, addr & 0xff);

	cur_line = line;
	cur_col  = col;
}

static void
scroll_line(void)
{
	uint16_t *p;
	uint16_t space_char;
	uint32_t count;
	uint32_t blank;
	uint32_t i;

	/* Move characters. */
	p = vram;
	count = columns * (lines - 1);
	for (i = 0; i < count; i++) {
		*p = *(p + columns);
		p++;
	}

	/* Fill the bottom line. */
	space_char = MK_VRAMCHAR(' ', cur_attr);
	memset16(p, space_char, columns);
}

/* IRQ handler. */
static void
keyboard_handler(
	void *p)
{
	/* Communicate with the keyboard controller. */
	kbd_buf_len = 0;
	while(asm_inb(0x64) & 1) {
		scancode = asm_inb(0x60);
		if(kbd_buf_len < KBD_BUF_SIZE)
			kbd_buf[kbd_buf_len++] = scancode;
	}

	bsp_irq_unmask(IRQ_KEYBOARD);
}

static int
scancode_to_char(
	int scancode)
{
	/* TODO */
	return scancode;
}
