/* sun4u serial console implementing the existing HAL console contract. */
#include <hal/hal.h>
#include "uart.h"

static struct hal_cons_state state = { HAL_CONS_TERMINAL, 0, 0, 1 };

void bsp_cons_init(void) {}
void cons_cls(void) { hal_cons_clear(); }
void cons_putc(int c)
{
	sun4u_uart_putc(c);
	if (c == '\n') state.row++, state.column = 0;
	else if (c == '\r') state.column = 0;
	else if (c == '\b') { if (state.column != 0) state.column--; }
	else if (c >= 0x20) state.column++;
	if (state.column >= HAL_CONS_COLUMNS) state.column = 0, state.row++;
	if (state.row >= HAL_CONS_ROWS) state.row = HAL_CONS_ROWS - 1;
}
void cons_puts(const char *s) { if (s) while (*s) cons_putc(*s++); }
int cons_getc(void) { return sun4u_uart_getc(); }
void cons_set_attr(int fg, int bg) { (void)fg; (void)bg; }
void hal_cons_reset(void) { state.mode = HAL_CONS_TERMINAL; state.row = state.column = 0; }
void hal_cons_putc(int c) { cons_putc(c); }
void hal_cons_clear(void) { cons_puts("\033[2J\033[H"); state.row = state.column = 0; }
void hal_cons_move_cursor(int r, int c) { (void)hal_cons_set_cursor((unsigned)r, (unsigned)c); }
int hal_cons_getc(void) { return cons_getc(); }
void hal_cons_set_mode(enum hal_cons_mode mode) { state.mode = mode; }
void hal_cons_write(const char *s) { cons_puts(s); }
void hal_cons_write_n(const char *s, unsigned n) { if (s) while (n--) cons_putc(*s++); }
void hal_cons_write_at(unsigned r, unsigned c, const char *s)
{ (void)hal_cons_set_cursor(r, c); cons_puts(s); }
void hal_cons_clear_row(unsigned r) { (void)r; cons_puts("\033[2K"); }
void hal_cons_clear_to_eol(void) { cons_puts("\033[K"); }
int hal_cons_write_at_attr(unsigned r,unsigned c,const char*s,uint8 a)
{ (void)a; hal_cons_write_at(r,c,s); return s ? hal_strlen(s) : -1; }
int hal_cons_write_n_at(unsigned r,unsigned c,const char*s,unsigned n,uint8 a)
{ (void)a; if (!hal_cons_set_cursor(r,c)) return -1; hal_cons_write_n(s,n); return (int)n; }
int hal_cons_clear_to_eol_at(unsigned r,unsigned c)
{ if (!hal_cons_set_cursor(r,c)) return 0; hal_cons_clear_to_eol(); return 1; }
int hal_cons_set_cursor(unsigned r, unsigned c)
{
	if (r >= HAL_CONS_ROWS || c >= HAL_CONS_COLUMNS) return 0;
	hal_printf("\033[%u;%uH", r + 1U, c + 1U); state.row=r; state.column=c; return 1;
}
void hal_cons_show_cursor(int visible) { state.cursor_visible = visible != 0; cons_puts(visible ? "\033[?25h" : "\033[?25l"); }
void hal_cons_save_state(struct hal_cons_state *out) { if (out) *out = state; }
void hal_cons_restore_terminal(const struct hal_cons_state *in)
{ state.mode=HAL_CONS_TERMINAL; if(in)state=*in; }
void hal_cons_update_cursor(void) {}
int hal_cons_read_event(void) { return cons_getc(); }
int hal_cons_poll_event(void) { return sun4u_uart_poll() ? cons_getc() : 0; }
int hal_cons_key_state(int key) { (void)key; return 0; }
void hal_cons_drain_input(void) { while (sun4u_uart_poll()) (void)cons_getc(); }
unsigned hal_cons_modifiers(void) { return 0; }
