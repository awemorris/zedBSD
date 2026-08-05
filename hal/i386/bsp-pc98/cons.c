/*
 * Simple Console
 */

#include <sys/hal/cons.h>
#include <sys/hal/fb.h>
#include <sys/kcrt/kcrt.h>	/* hal_memset16 */
#include "../i386/irq.h"	/* irq_enter_isr(), irq_leave_isr() */
#include "../i386/asm.h"	/* _asm_outb, SYS_START */

/* vram address */
#define VRAM_TEXT_ADDR		(0xA0000)
#define VRAM_ATTR_ADDR		(0xA2000)

/* screen setting */
static uint8 *vram_text = (uint8 *)VRAM_TEXT_ADDR + SYS_START;
static uint8 *vram_attr = (uint8 *)VRAM_ATTR_ADDR + SYS_START;
static int columns = 80;
static int lines = 25;

/* console status */
static int cur_col = 0;
static int cur_line = 0;
static uint8 cur_attr = 0xbf;

/* forward declaration */
static void clear_screen();
static void put_char(int c);
static void set_cursor_pos(int line, int col);
static void scroll_line();
static int get_keyboard_char();

void
bsp_cons_init(void)
{
	clear_screen();
}

/*
 * fg/bg 0-15.  The PC-98 text attribute plane has 3-bit GRB foreground
 * and no per-cell background, so bg degrades to nothing; the IRGB
 * intensity bit folds away.
 */
void
cons_set_attr(int fg, int bg)
{
	uint8 grb;

	(void)bg;
	grb = (uint8)((((fg >> 1) & 1) << 7) |	/* G */
		      (((fg >> 2) & 1) << 6) |	/* R */
		      ((fg & 1) << 5));		/* B */
	cur_attr = (uint8)(grb | 0x01);		/* visible */
}

void
cons_cls(void)
{
	clear_screen();
}

void
cons_putc(
	int c)
{
	/* While the framebuffer owns the display, the console is silent. */
	if (fb_is_active())
		return;
	put_char(c);
}

void
cons_puts(
	const char *s)
{
	if (fb_is_active())
		return;
	while(*s != '\0')
		put_char(*s++);
}

int
cons_getc(void)
{
	return get_keyboard_char();
}

static void
clear_screen(void)
{
	hal_memset(vram_text, 0, 160 * 25);
	hal_memset16((uint16 *)vram_attr, cur_attr, 80 * 25);
	set_cursor_pos(0, 0);
}

static void
put_char(
	int c)
{
	switch(c) {
	case '\n':
		cur_line++, cur_col = 0;
		break;
	default:
		*(vram_text + cur_line * 160 + cur_col * 2) = c;
		*(vram_attr + cur_line * 160 + cur_col * 2) = cur_attr;
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

	/* Update cursor. */
	set_cursor_pos(cur_line, cur_col);
}

static void
set_cursor_pos(
	int line,
	int col)
{
	uint16 addr = line * 80 + col;
	int timeout;

	/* Wait for FIFO full (bit1) is cleared. */
	for (timeout = 100000; timeout > 0; timeout--) {
		if((asm_inb(0x60) & 0x02) == 0)
			break;
	}
	if(timeout == 0)
		return;	/* GDC no response. */

	/* CSRW cmd */
	asm_outb(0x62, 0x49);
	asm_outb(0x60, addr & 0xff);
	asm_outb(0x60, (addr >> 8) & 0xff);

	cur_line = line;
	cur_col  = col;
}

static void
scroll_line(void)
{
	hal_memcpy(vram_text, vram_text + 160, 160 * 24);
	hal_memset(vram_text + 160 * 24, 0, 160);
	hal_memcpy(vram_attr, vram_attr + 160, 160 * 24);
	hal_memset16((uint16 *)(vram_attr + 160 * 24), cur_attr, 80);
}

/*
 * Simple Keybaord Driver.
 */

#define KBD_BUF_SIZE	(256)

int	kbd_buf[KBD_BUF_SIZE];
int	kbd_buf_len = 0;

/*
 * Input one character from keyboard.
 */
int get_keyboard_char()
{
        uint8 scancode;

        /* Receive 1 character from keyboard. */
        for(;;) {
                irq_enter_isr(IRQ_KEYBOARD);

                kbd_buf_len = 0;
                while(asm_inb(0x43) & 2) {
                        scancode = asm_inb(0x41);
                        if(kbd_buf_len < KBD_BUF_SIZE)
                                kbd_buf[kbd_buf_len++] = scancode;
                }

                irq_leave_isr(IRQ_KEYBOARD);

                if(kbd_buf_len == 0)
                        continue;

                /*
                 * Bit 7 is ON when the key is released.
                 * We use the press edge and ignore release edge.
                 */
                if((kbd_buf[0] & 0x80) != 0)
                        continue;
                        
                break;
        }

        /* TODO: scancode to ASCII. */
        return kbd_buf[0];
}
