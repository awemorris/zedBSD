/* WS018 KA-T070: PC-98 private evdev mouse lifecycle/publication fixture.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define WS018_INPUT_HID_HOST_TEST 1

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/drivers/hid/pc98-busmouse.c"

#undef inb
#undef outb

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

static struct mutex *host_mutex_owner;
static unsigned host_lock_depth;
static struct input_device_info registered_info;
static struct input_device *const fake_input =
	(struct input_device *)(uintptr_t)0x1000U;
static struct thread *const fake_worker =
	(struct thread *)(uintptr_t)0x2000U;
static struct captured_event events[64];
static size_t event_count;
static uint8_t sample_bytes[4];
static unsigned sample_index;
static unsigned register_count, create_count, start_count;
static unsigned eoi_count, port_c_zero_count, port_c_mask_count;
static unsigned left_down, right_down, middle_down;
static int create_failures;

static void
reset_fixture(void)
{
	memset(&registered_info, 0, sizeof(registered_info));
	memset(events, 0, sizeof(events));
	event_count = 0;
	sample_index = 0;
	register_count = create_count = start_count = 0;
	eoi_count = port_c_zero_count = port_c_mask_count = 0;
	left_down = right_down = middle_down = 0;
	create_failures = 0;
	host_mutex_owner = NULL;
	host_lock_depth = 0;
}

static void
load_sample(int8_t dx, int8_t dy, uint32_t buttons)
{
	uint8_t x = (uint8_t)dx;
	uint8_t y = (uint8_t)dy;
	uint8_t state = 0;

	if ((buttons & MOUSE_BUTTON_LEFT) == 0)
		state |= PORTA_LEFT_RELEASED;
	if ((buttons & MOUSE_BUTTON_RIGHT) == 0)
		state |= PORTA_RIGHT_RELEASED;
	sample_bytes[0] = state | (x & 0x0fU);
	sample_bytes[1] = (x >> 4) & 0x0fU;
	sample_bytes[2] = y & 0x0fU;
	sample_bytes[3] = (y >> 4) & 0x0fU;
	sample_index = 0;
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
ws018_pc98_mouse_test_inb(uint16_t port)
{
	CHECK(port == MOUSE_PORT_A);
	CHECK(sample_index < ARRAY_COUNT(sample_bytes));
	return sample_bytes[sample_index++];
}

void
ws018_pc98_mouse_test_outb(uint16_t port, uint8_t value)
{
	if (port == MOUSE_PORT_C && value == 0x00U)
		port_c_zero_count++;
	if (port == MOUSE_PORT_C && value == 0x10U)
		port_c_mask_count++;
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
	CHECK(host_lock_depth == 0);
	host_lock_depth = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex == host_mutex_owner);
	CHECK(host_lock_depth == 1);
	host_lock_depth = 0;
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
	CHECK(host_lock_depth == 1);
	CHECK(event_count < ARRAY_COUNT(events));
	events[event_count++] = (struct captured_event){type, code, value};
	if (type == EV_KEY && code == BTN_LEFT)
		left_down = value != 0;
	if (type == EV_KEY && code == BTN_RIGHT)
		right_down = value != 0;
	if (type == EV_KEY && code == BTN_MIDDLE)
		middle_down = value != 0;
}

int
kthread_create(void (*entry)(void *), void *argument, int priority,
	struct thread **result)
{
	CHECK(entry == mouse_service);
	CHECK(argument == NULL);
	CHECK(priority == SCHED_PRIOR_LOW);
	CHECK(result != NULL);
	create_count++;
	if (create_failures != 0) {
		create_failures--;
		return EIO;
	}
	*result = fake_worker;
	return 0;
}

void
thread_start(struct thread *thread)
{
	CHECK(thread == fake_worker);
	start_count++;
}

int
hal_irq_service_wait(int interrupt, hal_irq_ack_t *acknowledge)
{
	(void)interrupt;
	(void)acknowledge;
	return HAL_ERR_STATE;
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	CHECK(acknowledge == (hal_irq_ack_t)0x98U);
	eoi_count++;
}

_Noreturn void
hal_fatal(const char *file, int line, const char *message)
{
	(void)file;
	(void)line;
	fprintf(stderr, "unexpected HAL fatal: %s\n", message);
	exit(1);
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
	CHECK(!strcmp(registered_info.name, "NEC PC-98 bus mouse"));
	CHECK(registered_info.capability_count == ARRAY_COUNT(expected));
	CHECK(!memcmp(registered_info.capabilities, expected, sizeof(expected)));
	CHECK(registered_info.open != NULL);
	CHECK(registered_info.close != NULL);
}

int
main(void)
{
	unsigned enable_before, mask_before;

	reset_fixture();
	CHECK(pc98_busmouse_init() == 0);
	check_capabilities();
	CHECK(port_c_mask_count == 1);
	CHECK(reader_count == 0 && !mouse_active && !worker_started);

	create_failures = 1;
	CHECK(registered_info.open(NULL) == EIO);
	CHECK(create_count == 1 && start_count == 0);
	CHECK(reader_count == 0 && !mouse_active && !worker_started);

	enable_before = port_c_zero_count;
	CHECK(registered_info.open(NULL) == 0);
	CHECK(create_count == 2 && start_count == 1);
	CHECK(reader_count == 1 && mouse_active && worker_started);
	CHECK(port_c_zero_count == enable_before + 1U);
	enable_before = port_c_zero_count;
	CHECK(registered_info.open(NULL) == 0);
	CHECK(reader_count == 2 && create_count == 2 && start_count == 1);
	CHECK(port_c_zero_count == enable_before);

	load_sample(5, 3, MOUSE_BUTTON_LEFT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(sample_index == 4 && event_count == 4);
	expect_event(0, EV_REL, REL_X, 5);
	expect_event(1, EV_REL, REL_Y, 3);
	expect_event(2, EV_KEY, BTN_LEFT, 1);
	expect_event(3, EV_SYN, SYN_REPORT, 0);
	CHECK(left_down && !right_down && !middle_down);

	clear_events();
	load_sample(1, 0, MOUSE_BUTTON_LEFT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 2);
	expect_event(0, EV_REL, REL_X, 1);
	expect_event(1, EV_SYN, SYN_REPORT, 0);

	clear_events();
	load_sample(0, 0, MOUSE_BUTTON_LEFT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 0);

	load_sample(0, 0, 0);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 2);
	expect_event(0, EV_KEY, BTN_LEFT, 0);
	expect_event(1, EV_SYN, SYN_REPORT, 0);
	CHECK(!left_down);

	clear_events();
	load_sample(-2, -4, MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 5);
	expect_event(0, EV_REL, REL_X, -2);
	expect_event(1, EV_REL, REL_Y, -4);
	expect_event(2, EV_KEY, BTN_LEFT, 1);
	expect_event(3, EV_KEY, BTN_RIGHT, 1);
	expect_event(4, EV_SYN, SYN_REPORT, 0);

	mask_before = port_c_mask_count;
	clear_events();
	registered_info.close(NULL);
	CHECK(reader_count == 1 && mouse_active);
	CHECK(port_c_mask_count == mask_before && event_count == 0);
	registered_info.close(NULL);
	CHECK(reader_count == 0 && !mouse_active);
	CHECK(port_c_mask_count == mask_before + 1U);
	CHECK(event_count == 3);
	expect_event(0, EV_KEY, BTN_LEFT, 0);
	expect_event(1, EV_KEY, BTN_RIGHT, 0);
	expect_event(2, EV_SYN, SYN_REPORT, 0);
	CHECK(!left_down && !right_down && !middle_down);

	clear_events();
	load_sample(7, 8, MOUSE_BUTTON_LEFT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 0 && sample_index == 0);

	enable_before = port_c_zero_count;
	CHECK(registered_info.open(NULL) == 0);
	CHECK(reader_count == 1 && mouse_active);
	CHECK(create_count == 2 && start_count == 1);
	CHECK(port_c_zero_count == enable_before + 1U);
	load_sample(0, 0, MOUSE_BUTTON_LEFT);
	mouse_service_irq((hal_irq_ack_t)0x98U);
	CHECK(event_count == 2);
	expect_event(0, EV_KEY, BTN_LEFT, 1);
	expect_event(1, EV_SYN, SYN_REPORT, 0);
	CHECK(left_down);
	registered_info.close(NULL);
	CHECK(!left_down && !mouse_active && reader_count == 0);

	registered_info.close(NULL);
	CHECK(reader_count == 0);
	CHECK(eoi_count == 7);
	CHECK(host_lock_depth == 0);
	puts("KA-T070 PC-98 bus-mouse lifecycle/publication: PASS");
	return 0;
}
