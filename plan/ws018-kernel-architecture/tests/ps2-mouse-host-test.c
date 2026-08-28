/* WS018 KA-T070: PC/AT PS/2 private evdev mouse lifecycle/publication fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define WS018_INPUT_HID_HOST_TEST 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/drivers/hid/ps2-mouse.c"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
		    __LINE__, #condition); \
		exit(1); \
	} \
} while (0)

struct captured_event {
	uint16_t type;
	uint16_t code;
	int32_t value;
};

struct response_byte {
	uint8_t value;
	unsigned auxiliary;
};

static struct input_device_info registered_info;
static struct input_device *const fake_input =
	(struct input_device *)(uintptr_t)0x1000U;
static struct captured_event events[64];
static struct response_byte responses[64];
static size_t event_count;
static unsigned response_head, response_tail;
static struct spinlock *host_spin_owner;
static struct mutex *host_mutex_owner;
static unsigned host_spin_depth, host_mutex_depth;
static unsigned register_count, mask_count, unmask_count, eoi_count;
static unsigned set_handler_count, mouse_command_count;
static unsigned left_down, right_down, middle_down;
static uint8_t controller_configuration;
static int expect_configuration, expect_auxiliary;
static int force_input_busy;
static hal_irq_handler_t registered_handler;

static void
reset_fixture(void)
{
	memset(&registered_info, 0, sizeof(registered_info));
	memset(events, 0, sizeof(events));
	memset(responses, 0, sizeof(responses));
	event_count = 0;
	response_head = response_tail = 0;
	host_spin_owner = NULL;
	host_mutex_owner = NULL;
	host_spin_depth = host_mutex_depth = 0;
	register_count = mask_count = unmask_count = eoi_count = 0;
	set_handler_count = mouse_command_count = 0;
	left_down = right_down = middle_down = 0;
	controller_configuration = I8042_CONFIG_AUX_OFF;
	expect_configuration = expect_auxiliary = 0;
	force_input_busy = 0;
	registered_handler = NULL;
}

static void
push_response(uint8_t value, unsigned auxiliary)
{
	CHECK(response_tail < ARRAY_COUNT(responses));
	responses[response_tail++] =
	    (struct response_byte){value, auxiliary != 0};
}

static void
inject_packet(uint8_t first, uint8_t x, uint8_t y)
{
	unsigned i;
	const uint8_t bytes[] = {first, x, y};

	for (i = 0; i < ARRAY_COUNT(bytes); i++) {
		push_response(bytes[i], 1);
		CHECK(registered_handler != NULL);
		registered_handler(PS2_MOUSE_IRQ, (hal_irq_ack_t)0x12U, NULL);
	}
	CHECK(response_head == response_tail);
	response_head = response_tail = 0;
}

static void
clear_events(void)
{
	event_count = 0;
}

static void
expect_event(size_t index, uint16_t type, uint16_t code, int32_t value)
{
	CHECK(index < event_count);
	CHECK(events[index].type == type);
	CHECK(events[index].code == code);
	CHECK(events[index].value == value);
}

uint8_t
ws018_input_hid_test_inb(uint16_t port)
{
	if (port == I8042_STATUS) {
		if (force_input_busy)
			return I8042_STATUS_INPUT;
		if (response_head != response_tail)
			return I8042_STATUS_OUTPUT |
			    (responses[response_head].auxiliary ?
				 I8042_STATUS_AUX : 0);
		return 0;
	}
	CHECK(port == I8042_DATA);
	CHECK(response_head != response_tail);
	return responses[response_head++].value;
}

void
ws018_input_hid_test_outb(uint16_t port, uint8_t value)
{
	if (port == I8042_COMMAND) {
		if (value == I8042_READ_CONFIG)
			push_response(controller_configuration, 0);
		else if (value == I8042_WRITE_CONFIG)
			expect_configuration = 1;
		else if (value == I8042_WRITE_AUX)
			expect_auxiliary = 1;
		return;
	}
	CHECK(port == I8042_DATA);
	if (expect_configuration) {
		controller_configuration = value;
		expect_configuration = 0;
		return;
	}
	CHECK(expect_auxiliary);
	expect_auxiliary = 0;
	mouse_command_count++;
	push_response(PS2_ACK, 1);
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	(void)rank;
	(void)name;
	CHECK(host_spin_owner == NULL);
	host_spin_owner = lock;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	CHECK(lock == host_spin_owner);
	CHECK(host_spin_depth == 0);
	host_spin_depth = 1;
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	CHECK(lock == host_spin_owner);
	CHECK(enabled == 1);
	CHECK(host_spin_depth == 1);
	host_spin_depth = 0;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)rank;
	(void)name;
	CHECK(host_mutex_owner == NULL);
	host_mutex_owner = mutex;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	CHECK(mutex == host_mutex_owner);
	CHECK(host_mutex_depth == 0);
	host_mutex_depth = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex == host_mutex_owner);
	CHECK(host_mutex_depth == 1);
	host_mutex_depth = 0;
}

int
input_device_register(const struct input_device_info *info,
	struct input_device **result)
{
	CHECK(info != NULL);
	CHECK(result != NULL);
	CHECK(register_count++ == 0);
	registered_info = *info;
	*result = fake_input;
	return 0;
}

void
input_device_emit(struct input_device *device, uint16_t type, uint16_t code,
	int32_t value)
{
	CHECK(device == fake_input);
	CHECK(host_spin_depth == 1);
	CHECK(event_count < ARRAY_COUNT(events));
	events[event_count++] = (struct captured_event){type, code, value};
	if (type == EV_KEY && code == BTN_LEFT)
		left_down = value != 0;
	if (type == EV_KEY && code == BTN_RIGHT)
		right_down = value != 0;
	if (type == EV_KEY && code == BTN_MIDDLE)
		middle_down = value != 0;
}

void
hal_irq_mask(int interrupt)
{
	CHECK(interrupt == PS2_MOUSE_IRQ);
	mask_count++;
}

void
hal_irq_unmask(int interrupt)
{
	CHECK(interrupt == PS2_MOUSE_IRQ);
	unmask_count++;
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	CHECK(acknowledge == (hal_irq_ack_t)0x12U);
	CHECK(host_spin_depth == 1);
	eoi_count++;
}

int
hal_irq_set_handler(int interrupt, hal_irq_handler_t handler, void *argument)
{
	CHECK(interrupt == PS2_MOUSE_IRQ);
	CHECK(argument == NULL);
	set_handler_count++;
	registered_handler = handler;
	return HAL_OK;
}

static void
check_capabilities(void)
{
	static const struct input_capability expected[] = {
		{EV_SYN, SYN_REPORT},
		{EV_REL, REL_X},
		{EV_REL, REL_Y},
		{EV_KEY, BTN_LEFT},
		{EV_KEY, BTN_RIGHT},
		{EV_KEY, BTN_MIDDLE},
	};

	CHECK(register_count == 1);
	CHECK(!strcmp(registered_info.name, "PC/AT PS/2 mouse"));
	CHECK(registered_info.capability_count == ARRAY_COUNT(expected));
	CHECK(!memcmp(registered_info.capabilities, expected, sizeof(expected)));
	CHECK(registered_info.open != NULL);
	CHECK(registered_info.close != NULL);
}

int
main(void)
{
	unsigned mask_before, unmask_before;

	reset_fixture();
	CHECK(pcat_ps2_mouse_init() == 0);
	check_capabilities();
	CHECK(set_handler_count == 1 && registered_handler == mouse_interrupt);
	CHECK(mask_count == 1);
	CHECK(reader_count == 0 && !mouse_active);

	force_input_busy = 1;
	CHECK(registered_info.open(NULL) == ENODEV);
	force_input_busy = 0;
	CHECK(reader_count == 0 && !mouse_active);
	CHECK(host_mutex_depth == 0 && host_spin_depth == 0);

	mask_before = mask_count;
	unmask_before = unmask_count;
	CHECK(registered_info.open(NULL) == 0);
	CHECK(reader_count == 1 && mouse_active);
	CHECK(mask_count == mask_before + 1U);
	CHECK(unmask_count == unmask_before + 1U);
	mask_before = mask_count;
	unmask_before = unmask_count;
	CHECK(registered_info.open(NULL) == 0);
	CHECK(reader_count == 2 && mouse_active);
	CHECK(mask_count == mask_before && unmask_count == unmask_before);

	inject_packet(0x29U, 5U, 0xfdU);
	CHECK(event_count == 4);
	expect_event(0, EV_REL, REL_X, 5);
	expect_event(1, EV_REL, REL_Y, 3);
	expect_event(2, EV_KEY, BTN_LEFT, 1);
	expect_event(3, EV_SYN, SYN_REPORT, 0);
	CHECK(left_down && !right_down && !middle_down);

	clear_events();
	inject_packet(0x09U, 1U, 0U);
	CHECK(event_count == 2);
	expect_event(0, EV_REL, REL_X, 1);
	expect_event(1, EV_SYN, SYN_REPORT, 0);

	clear_events();
	inject_packet(0x09U, 0U, 0U);
	CHECK(event_count == 0);
	inject_packet(0x08U, 0U, 0U);
	CHECK(event_count == 2);
	expect_event(0, EV_KEY, BTN_LEFT, 0);
	expect_event(1, EV_SYN, SYN_REPORT, 0);

	clear_events();
	inject_packet(0x1fU, 0xfeU, 4U);
	CHECK(event_count == 6);
	expect_event(0, EV_REL, REL_X, -2);
	expect_event(1, EV_REL, REL_Y, -4);
	expect_event(2, EV_KEY, BTN_LEFT, 1);
	expect_event(3, EV_KEY, BTN_RIGHT, 1);
	expect_event(4, EV_KEY, BTN_MIDDLE, 1);
	expect_event(5, EV_SYN, SYN_REPORT, 0);

	mask_before = mask_count;
	clear_events();
	registered_info.close(NULL);
	CHECK(reader_count == 1 && mouse_active);
	CHECK(mask_count == mask_before && event_count == 0);
	registered_info.close(NULL);
	CHECK(reader_count == 0 && !mouse_active);
	CHECK(mask_count == mask_before + 1U);
	CHECK(event_count == 4);
	expect_event(0, EV_KEY, BTN_LEFT, 0);
	expect_event(1, EV_KEY, BTN_RIGHT, 0);
	expect_event(2, EV_KEY, BTN_MIDDLE, 0);
	expect_event(3, EV_SYN, SYN_REPORT, 0);
	CHECK(!left_down && !right_down && !middle_down);

	clear_events();
	inject_packet(0x09U, 7U, 0xf8U);
	CHECK(event_count == 0);
	CHECK(packet_index == 0);

	CHECK(registered_info.open(NULL) == 0);
	CHECK(reader_count == 1 && mouse_active);
	inject_packet(0x09U, 0U, 0U);
	CHECK(event_count == 2);
	expect_event(0, EV_KEY, BTN_LEFT, 1);
	expect_event(1, EV_SYN, SYN_REPORT, 0);
	CHECK(left_down);
	registered_info.close(NULL);
	CHECK(reader_count == 0 && !mouse_active && !left_down);
	registered_info.close(NULL);
	CHECK(reader_count == 0);

	CHECK(eoi_count == 21);
	CHECK(mouse_command_count != 0);
	CHECK(host_mutex_depth == 0 && host_spin_depth == 0);
	puts("KA-T070 PC/AT PS/2 mouse lifecycle/publication: PASS");
	return 0;
}
