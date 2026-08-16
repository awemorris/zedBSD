/* UltraSPARC tick/tick_cmpr scheduler timer. */
#include <hal/hal.h>
#include "asi.h"

static uint64 tick_frequency;
static uint64 tick_interval;
static uint64 tick_deadline;
static uint64 timer_ticks;

void
sparcv9_timer_init(uint64 frequency)
{
	if (frequency == 0)
		HAL_FATAL("invalid SPARC V9 timer frequency");
	tick_frequency = frequency;
	tick_interval = tick_frequency / HAL_TIMER_FREQUENCY;
	if (tick_interval == 0)
		tick_interval = 1;
	tick_deadline = sparcv9_tick() + tick_interval;
	sparcv9_clear_tick_interrupt();
	sparcv9_tick_compare(tick_deadline);
}

void
sun4u_timer_interrupt(hal_irq_ack_t acknowledge)
{
	uint64 now = sparcv9_tick();

	sparcv9_clear_tick_interrupt();
	timer_ticks++;
	do
		tick_deadline += tick_interval;
	while (tick_deadline <= now);
	sparcv9_tick_compare(tick_deadline);
	if (timer_ticks == 1)
		hal_puts("SPARCV9 TIMER TICK\n");
	kernel_timer_handler(0, acknowledge);
}

bool
hal_rtc_read(uint64 *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
