/* sun4u serial console implementing the existing HAL console contract. */
#include <hal/hal.h>
#include "uart.h"

static struct hal_cons_state state = { HAL_CONS_TERMINAL, 0, 0, 1 };
static int input_peek = -1;

void sun4u_cons_init(void) {}
static void console_putc(int c)
{
	sun4u_uart_putc(c);
	if (c == '\n') state.row++, state.column = 0;
	else if (c == '\r') state.column = 0;
	else if (c == '\b') { if (state.column != 0) state.column--; }
	else if (c >= 0x20) state.column++;
	if (state.column >= HAL_CONS_COLUMNS) state.column = 0, state.row++;
	if (state.row >= HAL_CONS_ROWS) state.row = HAL_CONS_ROWS - 1;
}
static void console_puts(const char *s) { if (s) while (*s) console_putc(*s++); }
static int console_getc(void) { return sun4u_uart_getc(); }
void hal_cons_reset(void) { state.mode = HAL_CONS_TERMINAL; state.row = state.column = 0; }
void hal_cons_putc(int c) { console_putc(c); }
void hal_cons_clear(void) { console_puts("\033[2J\033[H"); state.row = state.column = 0; }
void hal_cons_move_cursor(int r, int c) { (void)hal_cons_set_cursor((unsigned)r, (unsigned)c); }
int hal_cons_getc(void) { return hal_cons_read_event() & HAL_KEY_EVENT_KEY_MASK; }
void hal_cons_set_mode(enum hal_cons_mode mode) { state.mode = mode; }
void hal_cons_write(const char *s) { console_puts(s); }
void hal_cons_write_n(const char *s, unsigned n) { if (s) while (n--) console_putc(*s++); }
void hal_cons_write_at(unsigned r, unsigned c, const char *s)
{ (void)hal_cons_set_cursor(r, c); console_puts(s); }
void hal_cons_clear_row(unsigned r) { (void)r; console_puts("\033[2K"); }
void hal_cons_clear_to_eol(void) { console_puts("\033[K"); }
int hal_cons_write_at_attr(unsigned r,unsigned c,const char*s,uint8_t a)
{ (void)a; hal_cons_write_at(r,c,s); return s ? hal_strlen(s) : -1; }
int hal_cons_write_n_at(unsigned r,unsigned c,const char*s,unsigned n,uint8_t a)
{ (void)a; if (!hal_cons_set_cursor(r,c)) return -1; hal_cons_write_n(s,n); return (int)n; }
int hal_cons_clear_to_eol_at(unsigned r,unsigned c)
{ if (!hal_cons_set_cursor(r,c)) return 0; hal_cons_clear_to_eol(); return 1; }
int hal_cons_set_cursor(unsigned r, unsigned c)
{
	if (r >= HAL_CONS_ROWS || c >= HAL_CONS_COLUMNS) return 0;
	hal_printf("\033[%u;%uH", r + 1U, c + 1U); state.row=r; state.column=c; return 1;
}
void hal_cons_show_cursor(int visible) { state.cursor_visible = visible != 0; console_puts(visible ? "\033[?25h" : "\033[?25l"); }
void hal_cons_save_state(struct hal_cons_state *out) { if (out) *out = state; }
void hal_cons_restore_terminal(const struct hal_cons_state *in)
{ state.mode=HAL_CONS_TERMINAL; if(in)state=*in; }
void hal_cons_update_cursor(void) {}
int hal_cons_poll_event(void)
{ if (input_peek < 0 && sun4u_uart_poll()) input_peek=console_getc(); return input_peek; }
int hal_cons_read_event(void)
{ int event; while ((event=hal_cons_poll_event()) < 0) ; input_peek=-1; return event; }
int hal_cons_key_state(int key) { (void)key; return 0; }
void hal_cons_drain_input(void)
{ input_peek=-1; while (sun4u_uart_poll()) (void)console_getc(); }
unsigned hal_cons_modifiers(void) { return 0; }
void hal_cons_suspend(void) {}
void hal_cons_resume(void) {}
