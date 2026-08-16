#include <hal/hal.h>
#include "uart.h"
#include "framebuffer.h"

struct cell{uint8 character,attribute;};
static struct cell shadow[HAL_CONS_ROWS][HAL_CONS_COLUMNS];
static struct hal_cons_state state={HAL_CONS_TERMINAL,0,0,1};
static uint8 current_attribute=HAL_CONS_NORMAL_ATTRIBUTE;
static int input_peek=-1;

static void draw(unsigned row,unsigned column)
{struct cell*c=&shadow[row][column];rpi4_framebuffer_cell(row,column,c->character,c->attribute);}
static void erase_cursor(void)
{if(state.row<HAL_CONS_ROWS&&state.column<HAL_CONS_COLUMNS)draw(state.row,state.column);}
void hal_cons_update_cursor(void)
{if(state.cursor_visible)rpi4_framebuffer_cursor(state.row,state.column,1);}

void hal_cons_clear_row(unsigned row)
{
	if(row>=HAL_CONS_ROWS)return;
	for(unsigned column=0;column<HAL_CONS_COLUMNS;column++){
		shadow[row][column].character=' ';shadow[row][column].attribute=current_attribute;
		draw(row,column);
	}
}
void hal_cons_clear(void)
{for(unsigned row=0;row<HAL_CONS_ROWS;row++)hal_cons_clear_row(row);state.row=state.column=0;hal_cons_update_cursor();}
void hal_cons_reset(void)
{current_attribute=HAL_CONS_NORMAL_ATTRIBUTE;state.mode=HAL_CONS_TERMINAL;state.cursor_visible=1;hal_cons_clear();}

static void scroll(void)
{
	for(unsigned row=1;row<HAL_CONS_ROWS;row++)for(unsigned column=0;column<HAL_CONS_COLUMNS;column++)
		shadow[row-1][column]=shadow[row][column];
	for(unsigned row=0;row<HAL_CONS_ROWS-1;row++)for(unsigned column=0;column<HAL_CONS_COLUMNS;column++)draw(row,column);
	hal_cons_clear_row(HAL_CONS_ROWS-1);
}
static void newline(void)
{state.column=0;if(++state.row>=HAL_CONS_ROWS){scroll();state.row=HAL_CONS_ROWS-1;}}

void rpi4_cons_init(void){rpi4_uart_init();for(unsigned r=0;r<HAL_CONS_ROWS;r++)for(unsigned c=0;c<HAL_CONS_COLUMNS;c++){shadow[r][c].character=' ';shadow[r][c].attribute=current_attribute;}}
static void console_putc(int character)
{
	rpi4_uart_putc(character);erase_cursor();
	if(character=='\n'){newline();hal_cons_update_cursor();return;}
	if(character=='\r'){state.column=0;hal_cons_update_cursor();return;}
	if(character=='\b'){
		if(state.column)state.column--;
		shadow[state.row][state.column].character=' ';
		draw(state.row,state.column);hal_cons_update_cursor();return;
	}
	if(character=='\t'){do console_putc(' ');while(state.column&7U);return;}
	if(state.column>=HAL_CONS_COLUMNS)newline();
	shadow[state.row][state.column].character=(uint8)(character>=0x20&&character<0x7f?character:'?');
	shadow[state.row][state.column].attribute=current_attribute;draw(state.row,state.column++);
	if(state.column>=HAL_CONS_COLUMNS)newline();
	hal_cons_update_cursor();
}
static void console_puts(const char*s){if(s)while(*s)console_putc(*s++);}
static int console_getc(void){return rpi4_uart_getc();}
void hal_cons_putc(int c){console_putc(c);}
void hal_cons_move_cursor(int row,int column){(void)hal_cons_set_cursor((unsigned)row,(unsigned)column);}
int hal_cons_getc(void){return hal_cons_read_event()&HAL_KEY_EVENT_KEY_MASK;}
void hal_cons_set_mode(enum hal_cons_mode mode){state.mode=mode;}
void hal_cons_write(const char*s){console_puts(s);}
void hal_cons_write_n(const char*s,unsigned n){if(s)while(n--)console_putc(*s++);}
int hal_cons_write_n_at(unsigned row,unsigned column,const char*s,unsigned n,uint8 attr)
{
	unsigned changed=0;if(!s||row>=HAL_CONS_ROWS||column>=HAL_CONS_COLUMNS)return -1;erase_cursor();
	while(n--&&row<HAL_CONS_ROWS){uint8 c=(uint8)*s++;if(c=='\n'){row++;column=0;continue;}if(c=='\r'){column=0;continue;}if(column>=HAL_CONS_COLUMNS)break;
		shadow[row][column].character=c<0x80?c:'?';shadow[row][column].attribute=attr?attr:current_attribute;draw(row,column++);changed++;}
	state.row=row<HAL_CONS_ROWS?row:HAL_CONS_ROWS-1;state.column=column<HAL_CONS_COLUMNS?column:HAL_CONS_COLUMNS-1;hal_cons_update_cursor();return(int)changed;
}
int hal_cons_write_at_attr(unsigned r,unsigned c,const char*s,uint8 attr){unsigned n=0;if(!s)return -1;while(s[n])n++;return hal_cons_write_n_at(r,c,s,n,attr);}
void hal_cons_write_at(unsigned r,unsigned c,const char*s){(void)hal_cons_write_at_attr(r,c,s,current_attribute);}
int hal_cons_clear_to_eol_at(unsigned r,unsigned c){if(r>=HAL_CONS_ROWS||c>=HAL_CONS_COLUMNS)return 0;erase_cursor();for(unsigned x=c;x<HAL_CONS_COLUMNS;x++){shadow[r][x].character=' ';shadow[r][x].attribute=current_attribute;draw(r,x);}state.row=r;state.column=c;hal_cons_update_cursor();return 1;}
void hal_cons_clear_to_eol(void){(void)hal_cons_clear_to_eol_at(state.row,state.column);}
int hal_cons_set_cursor(unsigned r,unsigned c){if(r>=HAL_CONS_ROWS||c>=HAL_CONS_COLUMNS)return 0;erase_cursor();state.row=r;state.column=c;hal_cons_update_cursor();return 1;}
void hal_cons_show_cursor(int visible){erase_cursor();state.cursor_visible=visible!=0;hal_cons_update_cursor();}
void hal_cons_save_state(struct hal_cons_state*out){if(out)*out=state;}
void hal_cons_restore_terminal(const struct hal_cons_state*in){erase_cursor();state.mode=HAL_CONS_TERMINAL;if(in&&in->row<HAL_CONS_ROWS&&in->column<HAL_CONS_COLUMNS)state=*in;hal_cons_update_cursor();}
int hal_cons_poll_event(void){if(input_peek<0&&rpi4_uart_poll())input_peek=console_getc();return input_peek;}
int hal_cons_read_event(void){int event;while((event=hal_cons_poll_event())<0);input_peek=-1;return event;}
int hal_cons_key_state(int key){(void)key;return 0;}
void hal_cons_drain_input(void){input_peek=-1;while(rpi4_uart_poll())(void)console_getc();}
unsigned hal_cons_modifiers(void){return 0;}
void hal_cons_suspend(void){}
void hal_cons_resume(void){}
