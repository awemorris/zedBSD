/* X68000 keyboard receiver using the MC68901 USART and IRQ 0x4c. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "keyboard.h"
#include "keyboard-map.h"
#include "mmio.h"
#include "../../cons-wait.h"

#define X68K_KEYBOARD_VECTOR 0x4c
#define X68K_KEYBOARD_QUEUE  64U
#define X68K_SEND_SPINS      1000000U

#define MFP_IERA 3U
#define MFP_IMRA 9U
#define MFP_VR   11U
#define MFP_TBCR 13U
#define MFP_TBDR 16U
#define MFP_UCR  20U
#define MFP_RSR  21U
#define MFP_TSR  22U
#define MFP_UDR  23U

#define MFP_RECEIVE_FULL 0x10U
#define MFP_RSR_BF       0x80U
#define MFP_RSR_RE       0x01U
#define MFP_TSR_BE       0x80U
#define MFP_TSR_TE       0x01U

static struct x68k_keyboard_state keyboard;
static unsigned events[X68K_KEYBOARD_QUEUE];
static unsigned event_head, event_tail;
static uint32_t overflow_count, receive_error_count;
static struct hal_cons_wait_queue input_waiters;

static void
enqueue_raw(uint8_t raw)
{
	int event = x68k_keyboard_feed(&keyboard, raw);
	unsigned next;
	if (event < 0)
		return;
	next = (event_head + 1U) % X68K_KEYBOARD_QUEUE;
	if (next == event_tail) {
		/* Preserve queued input and deterministically discard the new key. */
		overflow_count++;
		return;
	}
	events[event_head] = (unsigned)event;
	event_head = next;
}

static void
receive_one(void)
{
	uint8_t status = x68k_mfp_read(MFP_RSR);
	uint8_t raw = x68k_mfp_read(MFP_UDR);
	if ((status & MFP_RSR_BF) != 0)
		enqueue_raw(raw);
	else
		receive_error_count++;
}

static void
keyboard_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	struct hal_cons_wait_entry *waiters = NULL;
	bool enabled;

	(void)irq;
	(void)argument;
	enabled = hal_cons_wait_queue_lock(&input_waiters);
	receive_one();
	if (event_head != event_tail)
		waiters = hal_cons_wait_queue_detach_all(&input_waiters);
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	hal_cons_wait_queue_notify_all(waiters);
	hal_irq_send_eoi(acknowledge);
}

static int
send_command(uint8_t command)
{
	unsigned spins;
	for (spins = 0; spins < X68K_SEND_SPINS; spins++)
		if ((x68k_mfp_read(MFP_TSR) & MFP_TSR_BE) != 0) {
			x68k_mfp_write(MFP_UDR, command);
			return 0;
		}
	return -1;
}

void
x68k_keyboard_init(void)
{
	uint8_t enabled;

	x68k_keyboard_state_reset(&keyboard);
	event_head = event_tail = 0;
	overflow_count = receive_error_count = 0;
	hal_cons_wait_queue_init(&input_waiters);
	/* System port 3 bit 3 enables keyboard data transmission. */
	x68k_sysport_write(3U, 0x08U);
	enabled = x68k_mfp_read(MFP_IERA);
	x68k_mfp_write(MFP_IERA, (uint8_t)(enabled & ~MFP_RECEIVE_FULL));
	x68k_mfp_write(MFP_TBCR, 0x10U);
	x68k_mfp_write(MFP_TBDR, 13U);
	x68k_mfp_write(MFP_TBCR, 0x01U);
	x68k_mfp_write(MFP_UCR, 0x88U); /* x16 clock, 8 data, 1 stop. */
	x68k_mfp_write(MFP_RSR, MFP_RSR_RE);
	x68k_mfp_write(MFP_TSR, MFP_TSR_TE);
	(void)x68k_mfp_read(MFP_UDR);
	x68k_mfp_write(MFP_VR, 0x40U);
	hal_irq_set_handler(X68K_KEYBOARD_VECTOR, keyboard_interrupt, NULL);
	enabled = x68k_mfp_read(MFP_IMRA);
	x68k_mfp_write(MFP_IMRA, (uint8_t)(enabled | MFP_RECEIVE_FULL));
	enabled = x68k_mfp_read(MFP_IERA);
	x68k_mfp_write(MFP_IERA, (uint8_t)(enabled | MFP_RECEIVE_FULL));
	/* 0x49 is the X68000 keyboard protocol's enable command. */
	if (send_command(0x49U) != 0)
		hal_puts("X68K keyboard transmit timeout\n");
}

unsigned
hal_cons_modifiers(void)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);
	unsigned modifiers = x68k_keyboard_modifiers(&keyboard);

	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	return modifiers;
}

int
hal_cons_poll_event(void)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);
	int event = event_tail == event_head ? -1 : (int)events[event_tail];

	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	return event;
}

int
hal_cons_read_event(void)
{
	struct hal_cons_wait_entry waiter;

	waiter.task = hal_task_get_current();
	waiter.next = NULL;
	waiter.queued = 0;
	for (;;) {
		bool enabled = hal_cons_wait_queue_lock(&input_waiters);

		if (event_tail != event_head) {
			int event = (int)events[event_tail];

			event_tail = (event_tail + 1U) % X68K_KEYBOARD_QUEUE;
			hal_cons_wait_queue_unlock(&input_waiters, enabled);
			return event;
		}
		hal_cons_wait_queue_add(&input_waiters, &waiter);
		hal_cons_wait_queue_unlock(&input_waiters, enabled);
		kernel_wait_task();
	}
}

int
hal_cons_key_state(int key)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);
	int down = x68k_keyboard_key_state(&keyboard, key);

	hal_cons_wait_queue_unlock(&input_waiters, enabled);
	return down;
}

void
hal_cons_drain_input(void)
{
	bool enabled = hal_cons_wait_queue_lock(&input_waiters);

	event_tail = event_head;
	hal_cons_wait_queue_unlock(&input_waiters, enabled);
}
