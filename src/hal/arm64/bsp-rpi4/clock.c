#include <hal/hal.h>
#include "../asm.h"
#include "gic.h"

#define ARM64_CNTP_INTID 30U
static uint64 timer_frequency;
static uint64 timer_interval;
static uint64 timer_deadline;
static uint64 timer_ticks;

void sched_clock(void);

static uint64 counter(void){uint64 v;__asm__ volatile("mrs %0, cntpct_el0":"=r"(v));return v;}
static void set_cval(uint64 v){__asm__ volatile("msr cntp_cval_el0, %0"::"r"(v));}

void hal_timer_set_freq(uint32 freq)
{
	if(freq==0)HAL_FATAL("zero timer frequency");
	__asm__ volatile("mrs %0, cntfrq_el0":"=r"(timer_frequency));
	timer_interval=timer_frequency/freq;if(!timer_interval)timer_interval=1;
	timer_deadline=counter()+timer_interval;set_cval(timer_deadline);
	__asm__ volatile("msr cntp_ctl_el0, %0\n\tisb"::"r"((uint64)1):"memory");
	rpi4_gic_unmask(ARM64_CNTP_INTID);
}

void rpi4_timer_interrupt(void)
{
	uint64 now=counter();timer_ticks++;
	do timer_deadline+=timer_interval;while(timer_deadline<=now);
	set_cval(timer_deadline);
	if(timer_ticks==1)hal_puts("ARM64 TIMER TICK\n");
	kernel_timer_handler();
	sched_clock();
}

uint64 hal_timer_get_tick(void){return timer_ticks;}
uint64 hal_timer_read_rtc(void){return timer_frequency?counter()/timer_frequency:0;}
hal_clock_t clock_get_tick_count(void){return (hal_clock_t)timer_ticks;}
void __attribute__((weak)) kernel_timer_handler(void){}
