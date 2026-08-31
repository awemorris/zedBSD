#include <hal/hal.h>
#include "../bsp.h"
#include "uart.h"
#include "framebuffer.h"
#include "../../cons-wait.h"

#define INPUT_EVENT_COUNT 64U

struct cell{uint8_t character,attribute;};
static struct cell shadow[HAL_CONS_ROWS][HAL_CONS_COLUMNS];
static struct hal_cons_state state={HAL_CONS_TERMINAL,0,0,1};
static uint8_t current_attribute=HAL_CONS_NORMAL_ATTRIBUTE;
static struct hal_key_event input_events[INPUT_EVENT_COUNT];
static unsigned input_head, input_tail;
static struct hal_cons_wait_queue input_waiters;

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

void rpi4_cons_init(void){rpi4_uart_init();input_head=input_tail=0;hal_cons_wait_queue_init(&input_waiters);for(unsigned r=0;r<HAL_CONS_ROWS;r++)for(unsigned c=0;c<HAL_CONS_COLUMNS;c++){shadow[r][c].character=' ';shadow[r][c].attribute=current_attribute;}}
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
	shadow[state.row][state.column].character=(uint8_t)(character>=0x20&&character<0x7f?character:'?');
	shadow[state.row][state.column].attribute=current_attribute;draw(state.row,state.column++);
	if(state.column>=HAL_CONS_COLUMNS)newline();
	hal_cons_update_cursor();
}
static void console_puts(const char*s){if(s)while(*s)console_putc(*s++);}
static int console_getc(void){return rpi4_uart_getc();}
void hal_cons_putc(int c){console_putc(c);}
void hal_cons_move_cursor(int row,int column){(void)hal_cons_set_cursor((unsigned)row,(unsigned)column);}
int hal_cons_getc(void){struct hal_key_event event;for(;;){(void)hal_cons_read_event(&event);if((event.flags&HAL_KEY_EVENT_PRESS)!=0&&event.symbol[1]=='\0')return event.symbol[0];}}
void hal_cons_set_mode(enum hal_cons_mode mode){state.mode=mode;}
void hal_cons_write(const char*s){console_puts(s);}
void hal_cons_write_n(const char*s,unsigned n){if(s)while(n--)console_putc(*s++);}
int hal_cons_write_n_at(unsigned row,unsigned column,const char*s,unsigned n,uint8_t attr)
{
	unsigned changed=0;if(!s||row>=HAL_CONS_ROWS||column>=HAL_CONS_COLUMNS)return -1;erase_cursor();
	while(n--&&row<HAL_CONS_ROWS){uint8_t c=(uint8_t)*s++;if(c=='\n'){row++;column=0;continue;}if(c=='\r'){column=0;continue;}if(column>=HAL_CONS_COLUMNS)break;
		shadow[row][column].character=c<0x80?c:'?';shadow[row][column].attribute=attr?attr:current_attribute;draw(row,column++);changed++;}
	state.row=row<HAL_CONS_ROWS?row:HAL_CONS_ROWS-1;state.column=column<HAL_CONS_COLUMNS?column:HAL_CONS_COLUMNS-1;hal_cons_update_cursor();return(int)changed;
}
int hal_cons_write_at_attr(unsigned r,unsigned c,const char*s,uint8_t attr){unsigned n=0;if(!s)return -1;while(s[n])n++;return hal_cons_write_n_at(r,c,s,n,attr);}
void hal_cons_write_at(unsigned r,unsigned c,const char*s){(void)hal_cons_write_at_attr(r,c,s,current_attribute);}
int hal_cons_clear_to_eol_at(unsigned r,unsigned c){if(r>=HAL_CONS_ROWS||c>=HAL_CONS_COLUMNS)return 0;erase_cursor();for(unsigned x=c;x<HAL_CONS_COLUMNS;x++){shadow[r][x].character=' ';shadow[r][x].attribute=current_attribute;draw(r,x);}state.row=r;state.column=c;hal_cons_update_cursor();return 1;}
void hal_cons_clear_to_eol(void){(void)hal_cons_clear_to_eol_at(state.row,state.column);}
int hal_cons_set_cursor(unsigned r,unsigned c){if(r>=HAL_CONS_ROWS||c>=HAL_CONS_COLUMNS)return 0;erase_cursor();state.row=r;state.column=c;hal_cons_update_cursor();return 1;}
void hal_cons_show_cursor(int visible){erase_cursor();state.cursor_visible=visible!=0;hal_cons_update_cursor();}
void hal_cons_save_state(struct hal_cons_state*out){if(out)*out=state;}
void hal_cons_restore_terminal(const struct hal_cons_state*in){erase_cursor();state.mode=HAL_CONS_TERMINAL;if(in&&in->row<HAL_CONS_ROWS&&in->column<HAL_CONS_COLUMNS)state=*in;hal_cons_update_cursor();}
static void rpi4_console_interrupt(int irq,hal_irq_ack_t acknowledge,void*argument)
{
	struct hal_cons_wait_entry*waiters=NULL;bool enabled;
	(void)irq;(void)argument;enabled=hal_cons_wait_queue_lock(&input_waiters);
	while(rpi4_uart_poll()){
		unsigned next=(input_head+1U)%INPUT_EVENT_COUNT;
		int character=console_getc();if(next==input_tail)continue;
		for(unsigned i=0;i<HAL_KEY_SYMBOL_SIZE;i++)input_events[input_head].symbol[i]='\0';
		input_events[input_head].symbol[0]=(char)character;
		input_events[input_head].flags=HAL_KEY_EVENT_PRESS;input_head=next;
	}
	if(input_head!=input_tail)waiters=hal_cons_wait_queue_detach_all(&input_waiters);
	hal_cons_wait_queue_unlock(&input_waiters,enabled);rpi4_uart_clear_rx_irq();
	hal_cons_wait_queue_notify_all(waiters);hal_irq_send_eoi(acknowledge);
}
void rpi4_cons_irq_init(void)
{
	const struct rpi4_fdt_info*info=rpi4_boot_info();
	if(info==NULL||info->uart_irq==0||hal_irq_set_handler((int)info->uart_irq,rpi4_console_interrupt,NULL)!=HAL_OK)HAL_FATAL("Raspberry Pi UART IRQ registration failed");
	rpi4_uart_enable_rx_irq();hal_irq_unmask((int)info->uart_irq);
}
int hal_cons_poll_event(struct hal_key_event*event){bool enabled=hal_cons_wait_queue_lock(&input_waiters);int available=input_head!=input_tail;if(available&&event!=NULL)*event=input_events[input_tail];hal_cons_wait_queue_unlock(&input_waiters,enabled);return available;}
void hal_cons_get_input_info(struct hal_cons_input_info*info){if(info){info->flags=HAL_CONS_INPUT_TEXT;info->symbols=NULL;info->symbol_count=0;}}
int hal_cons_read_event(struct hal_key_event*event){struct hal_cons_wait_entry waiter={hal_task_get_current(),NULL,0};for(;;){bool enabled=hal_cons_wait_queue_lock(&input_waiters);if(input_head!=input_tail){if(event!=NULL)*event=input_events[input_tail];input_tail=(input_tail+1U)%INPUT_EVENT_COUNT;hal_cons_wait_queue_unlock(&input_waiters,enabled);return 1;}hal_cons_wait_queue_add(&input_waiters,&waiter);hal_cons_wait_queue_unlock(&input_waiters,enabled);kernel_wait_task();}}
int hal_cons_key_state(int key){(void)key;return 0;}
void hal_cons_drain_input(void){bool enabled=hal_cons_wait_queue_lock(&input_waiters);input_tail=input_head;hal_cons_wait_queue_unlock(&input_waiters,enabled);}
unsigned hal_cons_modifiers(void){return 0;}
void hal_cons_suspend(void){}
void hal_cons_resume(void){}
