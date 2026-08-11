#include "../i386/clock.h"
#include "../i386/irq.h"
#include "../i386/asm.h"
#include "../i386/pic.h"

static hal_clock_t cpu_tick_count;

/*
 * Forward declaration
 */
static void init_8253(void);

/*
 * Initialize the clock module.
 */
void bsp_timer_init(void)
{
	cpu_tick_count = 0;

	init_8253();

	/* Unmask the IRQ of PIT. (Start interrupting from this moment.) */
	pic_set_irq_mask(IRQ_TIMER, 0);
}

/* Initialize the Programmable Interrupt Timer (PIT: 8253). */
static void init_8253(void)
{
	uint16 interval;

	/*
	 * Initialize the interval timer.
	 */

	/* 10 msec. (1996800 * 0.01 = 19968) */
	interval = 19968;

	/* set rate-generator mode
	 * ------------------------------------
	 *  0x34 = (0011 0100)b
	 * ------------------------------------
	 *  00  [7-6]: ch0
	 *  11  [5-4]: 16bit load (LSB first)
	 *  010 [3-1]: rate generator mode
	 *  0   [0]  : binary count
	 * ------------------------------------
	 */
	asm_outb(0x77, 0x34);

	/* rate LSB */
	asm_outb(0x71, interval&0xff);

	/* rate MSB */
	asm_outb(0x71, interval>>8);
}

/*
 * Get the CPU local tick count.
 */
hal_clock_t	clock_get_tick_count(void)
{
	return cpu_tick_count;
}

/*
 * Interrupt handler for PIT.
 */
void clock_handler(void)
{
	cpu_tick_count++;
}
