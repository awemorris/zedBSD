#include <hal/hal.h>
#include "../asm.h"
#include "gic.h"

#define ARM64_CNTP_INTID 30U

static uint64 timer_frequency;
static uint64 timer_interval;
static uint64 timer_deadline;
static uint64 timer_ticks;

static uint64
counter(void)
{
	uint64 value;
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
	return value;
}

static void
set_cval(uint64 value)
{
	__asm__ volatile("msr cntp_cval_el0, %0" : : "r"(value));
}

void
rpi4_timer_init(void)
{
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));
	if (timer_frequency == 0)
		HAL_FATAL("zero AArch64 timer frequency");
	timer_interval = timer_frequency / HAL_TIMER_FREQUENCY;
	if (timer_interval == 0)
		timer_interval = 1;
	timer_deadline = counter() + timer_interval;
	set_cval(timer_deadline);
	__asm__ volatile("msr cntp_ctl_el0, %0\n\tisb" : :
	    "r"((uint64)1) : "memory");
	rpi4_gic_unmask(ARM64_CNTP_INTID);
}

void
rpi4_timer_interrupt(hal_irq_ack_t acknowledge)
{
	uint64 now = counter();

	timer_ticks++;
	do
		timer_deadline += timer_interval;
	while (timer_deadline <= now);
	set_cval(timer_deadline);
	if (timer_ticks == 1)
		hal_puts("ARM64 TIMER TICK\n");
	kernel_timer_handler(0, acknowledge);
}

bool
hal_rtc_read(uint64 *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
