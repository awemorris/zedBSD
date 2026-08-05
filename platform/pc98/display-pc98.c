/* Native PC-98 GDC display switching.  SPDX-License-Identifier: Zlib */
#include "platform/pc98/display-pc98.h"
#include <stdint.h>

#define GDC_FIFO_FULL 0x02U
#define GDC_START 0x0dU
#define GDC_STOP 0x0cU
#define GDC_TIMEOUT 1000000U

static uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}
static void outb(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}
static int command(uint16_t status, uint16_t port, uint8_t value)
{
	unsigned timeout;
	for (timeout = GDC_TIMEOUT; timeout != 0; timeout--)
		if ((inb(status) & GDC_FIFO_FULL) == 0) {
			outb(port, value);
			return 1;
		}
	return 0;
}
int boots_pc98_display_graphics_start(void)
{
	return command(0xa0, 0xa2, GDC_START);
}
int boots_pc98_display_graphics_stop(void)
{
	return command(0xa0, 0xa2, GDC_STOP);
}
int boots_pc98_display_text_restore(void)
{
	return boots_pc98_display_graphics_stop() &&
		command(0x60, 0x62, GDC_START);
}
