/*
 * NEC PC-98 HAL keyboard translation host test
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

/* Exercise the exact implementation embedded in the PC-98 HAL console.
 * Keeping the mapper private prevents a board-specific API from leaking out
 * of HAL solely for testing. */
#include <hal/hal.h>

#include "../src/hal/i386/bsp-pc98/jisx0208.c"

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
void hal_irq_unmask(int irq) { (void)irq; }
void hal_irq_send_eoi(hal_irq_ack_t acknowledge) { (void)acknowledge; }
int hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{ (void)irq; (void)handler; (void)argument; return HAL_OK; }
hal_task_t hal_task_get_current(void) { return (hal_task_t)1; }
void kernel_wait_task(void) {}
void kernel_notify_task(hal_task_t task) { (void)task; }
void hal_fatal(const char *file, int line, const char *message)
{ (void)file; (void)line; (void)message; }
#include "../src/hal/i386/bsp-pc98/cons.c"

static int failures;

#define CHECK(expression)                                                \
	do {                                                               \
		if (!(expression))                                           \
			failures++;                                            \
	} while (0)

static struct pc98_keyboard kb;

static int
tap(uint8_t scan)
{
	int key = pc98_keyboard_feed(&kb, scan);

	pc98_keyboard_feed(&kb, scan | 0x80U);
	return key;
}

int
main(void)
{
	pc98_keyboard_reset(&kb);

	CHECK(tap(0x1d) == 'a');
	CHECK(tap(0x29) == 'z');
	CHECK(tap(0x01) == '1');
	CHECK(tap(0x0a) == '0');
	CHECK(tap(0x34) == ' ');

	CHECK(tap(0x00) == HAL_KEY_ESCAPE);
	CHECK(tap(0x1c) == HAL_KEY_ENTER);
	CHECK(tap(0x0f) == HAL_KEY_TAB);
	CHECK(tap(0x3a) == HAL_KEY_UP);
	CHECK(tap(0x62) == HAL_KEY_F1);
	CHECK(tap(0x6b) == HAL_KEY_F10);

	CHECK(pc98_keyboard_feed(&kb, 0x1d | 0x80U) == 0);
	CHECK(pc98_keyboard_feed(&kb, 0x70) == 0);
	CHECK(tap(0x1d) == 'A');
	CHECK(tap(0x01) == '!');
	CHECK(tap(0x02) == '"');
	CHECK(pc98_keyboard_feed(&kb, 0x70 | 0x80U) == 0);
	CHECK(tap(0x1d) == 'a');

	CHECK(pc98_keyboard_feed(&kb, 0x74) == 0);
	CHECK(tap(0x1d) == 1);
	CHECK(tap(0x11) == 23);
	CHECK(pc98_keyboard_feed(&kb, 0x74 | 0x80U) == 0);

	CHECK(pc98_keyboard_feed(&kb, 0x71) == 0);
	pc98_keyboard_feed(&kb, 0x71 | 0x80U);
	CHECK(tap(0x1d) == 'A');
	CHECK(tap(0x01) == '1');
	CHECK(pc98_keyboard_feed(&kb, 0x71) == 0);
	pc98_keyboard_feed(&kb, 0x71 | 0x80U);
	CHECK(tap(0x1d) == 'a');

	pc98_keyboard_reset(&kb);
	CHECK(pc98_keyboard_is_down(&kb, 'a') == 0);
	pc98_keyboard_feed(&kb, 0x1d);
	CHECK(pc98_keyboard_is_down(&kb, 'a') == 1);
	CHECK(pc98_keyboard_is_down(&kb, 'A') == 1);
	pc98_keyboard_feed(&kb, 0x3a);
	CHECK(pc98_keyboard_is_down(&kb, HAL_KEY_UP) == 1);
	pc98_keyboard_feed(&kb, 0x1d | 0x80U);
	CHECK(pc98_keyboard_is_down(&kb, 'a') == 0);
	CHECK(pc98_keyboard_is_down(&kb, HAL_KEY_UP) == 1);
	CHECK(pc98_keyboard_is_down(&kb, 0x1ffff) == -1);

	return failures != 0;
}
