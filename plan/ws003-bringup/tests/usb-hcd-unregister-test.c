/*
 * WS003 USB HCD stop/unregister lifecycle fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb.h>
#include <hal/hal.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct bus_observation {
	struct drv_usb_hcd *hcd;
	struct drv_usb_bus *bus;
	unsigned count;
};

static int stop_result;
static unsigned start_calls;
static unsigned quiesce_calls;
static unsigned stop_calls;
static unsigned enqueue_calls;

void *
hal_malloc(size_t size)
{
	return malloc(size);
}

void
hal_free(void *pointer)
{
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

uint64_t
sched_ticks(void)
{
	return 0;
}

void
sched_yield(void)
{
}

static int
test_start(struct drv_usb_hcd *hcd)
{
	assert(hcd != NULL);
	start_calls++;
	return 0;
}

static int
test_quiesce(struct drv_usb_hcd *hcd)
{
	assert(hcd != NULL);
	quiesce_calls++;
	return stop_result;
}

static void
test_stop(struct drv_usb_hcd *hcd)
{
	assert(hcd != NULL);
	stop_calls++;
}

static int
test_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	(void)hcd;
	(void)urb;
	enqueue_calls++;
	return ENOTSUP;
}

static int
test_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	(void)hcd;
	(void)urb;
	return ENOTSUP;
}

static int
observe_bus(struct drv_usb_bus *bus, void *argument)
{
	struct bus_observation *observation = argument;

	assert(bus != NULL);
	assert(drv_usb_bus_hcd(bus) == observation->hcd);
	if (observation->bus != NULL)
		assert(bus == observation->bus);
	observation->count++;
	return 0;
}

static unsigned
bus_count(struct drv_usb_hcd *hcd, struct drv_usb_bus *expected)
{
	struct bus_observation observation = {
		.hcd = hcd,
		.bus = expected,
		.count = 0,
	};

	assert(drv_usb_foreach_bus(observe_bus, &observation) == 0);
	return observation.count;
}

int
main(void)
{
	static const struct drv_usb_hcd_ops operations = {
		.start = test_start,
		.quiesce = test_quiesce,
		.stop = test_stop,
		.urb_enqueue = test_urb_enqueue,
		.urb_dequeue = test_urb_dequeue,
	};
	struct drv_usb_hcd hcd = {
		.name = "lifecycle-test",
		.ops = &operations,
		.root_port_count = 1,
	};
	struct drv_usb_bus *bus = NULL;
	struct drv_usb_urb *urb;

	assert(drv_usb_init() == 0);
	assert(drv_usb_hcd_register(&hcd, &bus) == 0);
	assert(bus != NULL);
	assert(start_calls == 1);
	assert(bus_count(&hcd, bus) == 1);

	stop_result = EBUSY;
	assert(drv_usb_hcd_unregister(&hcd) == EBUSY);
	assert(quiesce_calls == 1);
	assert(stop_calls == 0);
	assert(bus_count(&hcd, bus) == 1);
	urb = drv_usb_urb_alloc(drv_usb_bus_root_hub(bus), NULL, 0);
	assert(urb != NULL);
	assert(drv_usb_urb_setup(urb, NULL, 0, 0, 0, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(urb) == EBUSY);
	assert(enqueue_calls == 0);
	drv_usb_urb_free(urb);

	stop_result = 0;
	assert(drv_usb_hcd_unregister(&hcd) == 0);
	assert(quiesce_calls == 2);
	assert(stop_calls == 1);
	assert(bus_count(&hcd, NULL) == 0);
	assert(drv_usb_hcd_unregister(&hcd) == ENOENT);

	puts("USB HCD unregister test: PASS");
	return 0;
}
