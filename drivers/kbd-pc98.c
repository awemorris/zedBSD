/* NEC PC-98 polled keyboard driver.  SPDX-License-Identifier: Zlib */
#include "drivers/kbd-pc98.h"
#include "drivers/kbd-pc98-map.h"

#include <stdint.h>

#define KBD_DATA 0x41U
#define KBD_STATUS 0x43U
#define KBD_RXRDY 0x02U
#define QUEUE_SIZE 32U

static struct boots_kbd_pc98 keyboard;
static unsigned events[QUEUE_SIZE];
static unsigned head, tail;

static uint8_t inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile ("inb %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

unsigned boots_kbd_pc98_modifiers(void)
{
	return (keyboard.shift ? BOOTS_KBD_EVENT_SHIFT : 0) |
		(keyboard.ctrl ? BOOTS_KBD_EVENT_CTRL : 0) |
		(keyboard.graph ? BOOTS_KBD_EVENT_GRAPH : 0);
}

static void pump(void)
{
	while ((inb(KBD_STATUS) & KBD_RXRDY) != 0) {
		uint8_t raw = inb(KBD_DATA);
		int key = boots_kbd_pc98_feed(&keyboard, raw);
		unsigned next;

		if (key == 0)
			continue;
		next = (head + 1U) % QUEUE_SIZE;
		if (next == tail)
			continue;
		events[head] = ((unsigned)key & BOOTS_KBD_EVENT_KEY_MASK) |
			boots_kbd_pc98_modifiers();
		head = next;
	}
}

int boots_kbd_pc98_poll_event(void)
{
	pump();
	return tail == head ? -1 : (int)events[tail];
}

int boots_kbd_pc98_read_event(void)
{
	int event;
	while ((event = boots_kbd_pc98_poll_event()) < 0)
		;
	tail = (tail + 1U) % QUEUE_SIZE;
	return event;
}

int boots_kbd_pc98_poll(void)
{
	int event = boots_kbd_pc98_poll_event();
	return event < 0 ? -1 : event & (int)BOOTS_KBD_EVENT_KEY_MASK;
}

int boots_kbd_pc98_read(void)
{
	return boots_kbd_pc98_read_event() & (int)BOOTS_KBD_EVENT_KEY_MASK;
}

int boots_kbd_pc98_state(int key)
{
	pump();
	return boots_kbd_pc98_is_down(&keyboard, key);
}

void boots_kbd_pc98_drain(void)
{
	pump();
	tail = head;
}
