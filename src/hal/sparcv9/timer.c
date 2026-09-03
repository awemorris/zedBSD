/* UltraSPARC tick/tick_cmpr scheduler timer. */
#include <hal/hal.h>
#include "asi.h"
#include "irq.h"

static uint64_t tick_frequency;
static uint64_t tick_interval;
static uint64_t tick_deadline;
static uint64_t timer_ticks;
static unsigned deferred_ticks;

void
sparcv9_timer_init(uint64_t frequency)
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
	uint64_t now = sparcv9_tick();
	uint64_t trap_level;

	sparcv9_clear_tick_interrupt();
	timer_ticks++;
	do
		tick_deadline += tick_interval;
	while (tick_deadline <= now);
	sparcv9_tick_compare(tick_deadline);
	if (timer_ticks == 1)
		hal_puts("SPARCV9 TIMER TICK\n");
	/*
	 * A timer may interrupt a user syscall trap at TL2.  The scheduler may
	 * only switch the saved outer TL1 context, so acknowledge and account the
	 * nested tick here, then replay it through the kernel contract at the next
	 * TL1 timer boundary.
	 */
	__asm__ volatile("rdpr %%tl,%0" : "=r"(trap_level));
	if (trap_level > 1) {
		hal_irq_send_eoi(acknowledge);
		if (deferred_ticks != ~0U)
			deferred_ticks++;
		return;
	}
	kernel_timer_handler(0, acknowledge);
	while (deferred_ticks != 0) {
		deferred_ticks--;
		kernel_timer_handler(0, sparcv9_irq_begin(0));
	}
}

bool
hal_rtc_read_counter(uint64_t *counter, uint64_t *freq_hz)
{
	if (counter == NULL || freq_hz == NULL || tick_frequency == 0U)
		return false;
	*counter = sparcv9_tick();
	*freq_hz = tick_frequency;
	return true;
}

bool
hal_rtc_read_epoch_time(uint64_t *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
