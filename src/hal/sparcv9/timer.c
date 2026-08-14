/* UltraSPARC tick/tick_cmpr scheduler timer. */
#include <hal/hal.h>
#include "asi.h"
#include <kern/sched.h>

static uint64 tick_frequency,tick_interval,tick_deadline,timer_ticks;
void sparcv9_timer_init(uint64 frequency){tick_frequency=frequency;}
void hal_timer_set_freq(uint32 freq)
{
	if(!freq||!tick_frequency)HAL_FATAL("invalid SPARC V9 timer frequency");
	tick_interval=tick_frequency/freq;if(!tick_interval)tick_interval=1;
	tick_deadline=sparcv9_tick()+tick_interval;sparcv9_clear_tick_interrupt();sparcv9_tick_compare(tick_deadline);
}
void sun4u_timer_interrupt(void)
{
	uint64 now=sparcv9_tick();sparcv9_clear_tick_interrupt();timer_ticks++;
	do tick_deadline+=tick_interval;while(tick_deadline<=now);
	sparcv9_tick_compare(tick_deadline);if(timer_ticks==1)hal_puts("SPARCV9 TIMER TICK\n");kernel_timer_handler();sched_clock();
}
uint64 hal_timer_get_tick(void){return timer_ticks;}
uint64 hal_timer_read_rtc(void){return tick_frequency?sparcv9_tick()/tick_frequency:0;}
hal_clock_t clock_get_tick_count(void){return(hal_clock_t)timer_ticks;}
void __attribute__((weak))kernel_timer_handler(void){}
