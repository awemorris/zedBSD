/*
 * BSP for PC/AT 8254 PIT
 */

#include <sys/hal.h>
#include "../i386/bsp.h"

/*
 * Initial timer frequency.
 */
#define INIT_TIMER_HZ	(10)

/*
 * Tick count.
 */
static uint64_t cpu_tick_count;

/*
 * Forward declaration
 */
static void timer_handler(void *p);
static void init_8254(void);

/*
 * Initialize the PIT.
 */
void
bsp_pit_init(void)
{
	/* Initialize 8254 PIT. */
	init_8254(INIT_TIMER_HZ);

	/* Install the timer handler. */
	bsp_irq_set_handler(IRQ_TIMER, timer_handler, NULL);

	/* Unmask the timer IRQ. */
	bsp_irq_unmask(IRQ_TIMER);
}

/*
 * Set the timer frequency.
 */
void
bsp_pit_set_freq(
	uint32_t freq)
{
	uint16_t interval;

	/*
	 * Set the timer frequency.
	 *
	 * XXX: Is this valid?
	 */
	interval = 1000000 / freq;

	/* Initialize 8254 PIT. */
	init_8254(interval);
}

/*
 * Get a tick count.
 */
uint64_t
bsp_pit_get_tick(void)
{
	return cpu_tick_count;
}

/* Realtime ISR. */
static void
timer_handler(
	void *p)
{
	cpu_tick_count++;

	/*
	 * Call the kernel handler.
	 *  - kernel_timer_handler() will do task scheduling.
	 */
	kernel_timer_handler();
}

/* Initialize 8254 PIT. */
static void
init_8254(
	uint16_t interval)
{
	/*
	 * Set rate-generator mode.
	 * ------------------------------------
	 *  0x34 = (0011 0100)b
	 * ------------------------------------
	 *	 00 [7-6]: ch0
	 *   11 [5-4]: 16bit load (LSB first)
	 *   010[3-1]: rate generator mode
	 *   0  [0]  : binary count
	 * ------------------------------------
	 */
	asm_outb(0x43, 0x34);

	/* rate LSB */
	asm_outb(0x40, interval&0xff);

	/* rate MSB */
	asm_outb(0x40, interval>>8);
}
