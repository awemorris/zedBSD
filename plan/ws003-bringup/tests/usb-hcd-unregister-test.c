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
#include <string.h>

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
static int dequeue_result = ENOTSUP;
static struct drv_usb_urb *pending_urb;
static uint32_t root_status_value;
static uint32_t cleared_features;
static unsigned root_reset_calls;
static uint64_t fake_ticks;
static uint64_t complete_at_tick;
static struct drv_usb_hcd *completion_hcd;

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
	return fake_ticks++;
}

void
sched_yield(void)
{
	fake_ticks++;
	if (complete_at_tick != 0 && fake_ticks >= complete_at_tick &&
	    pending_urb != NULL) {
		drv_usb_hcd_complete(completion_hcd, pending_urb,
		    DRV_USB_URB_COMPLETE, 0);
		complete_at_tick = 0;
	}
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
	enqueue_calls++;
	assert(pending_urb == NULL);
	pending_urb = urb;
	return 0;
}

static int
test_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	(void)hcd;
	(void)urb;
	return dequeue_result;
}

static int
test_root_control(struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request, void *buffer,
	size_t length, size_t *actual)
{
	(void)hcd;
	if (request->request == 0 && buffer != NULL &&
	    length >= sizeof(root_status_value)) {
		memcpy(buffer, &root_status_value, sizeof(root_status_value));
		if (actual != NULL)
			*actual = sizeof(root_status_value);
		return 0;
	}
	if (request->request == 1 && request->value >= 16U &&
	    request->value <= 23U) {
		cleared_features |= 1U << request->value;
		root_status_value &= ~(1U << request->value);
		if (actual != NULL)
			*actual = 0;
		return 0;
	}
	return ENOTSUP;
}

static int
test_root_reset(struct drv_usb_hcd *hcd, unsigned port)
{
	(void)hcd;
	assert(port == 1U);
	root_reset_calls++;
	return EIO;
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
	static const uint8_t companion_raw[] = { 6U, 48U, 15U, 0U, 0U, 4U };
	struct drv_usb_superspeed_endpoint_companion_descriptor companion;
	static const struct drv_usb_hcd_ops operations = {
		.start = test_start,
		.quiesce = test_quiesce,
		.stop = test_stop,
		.urb_enqueue = test_urb_enqueue,
		.urb_dequeue = test_urb_dequeue,
		.root_hub_control = test_root_control,
		.root_port_reset = test_root_reset,
	};
	struct drv_usb_hcd hcd = {
		.name = "lifecycle-test",
		.ops = &operations,
		.root_port_count = 1,
	};
	struct drv_usb_bus *bus = NULL;
	struct drv_usb_hcd ownership_hcd = {
		.name = "ownership-test",
		.ops = &operations,
		.root_port_count = 1,
	};
	struct drv_usb_bus *ownership_bus = NULL;
	struct drv_usb_urb *urb;

	assert(drv_usb_decode_superspeed_endpoint_companion(companion_raw,
	    sizeof(companion_raw), &companion) == 0);
	assert(companion.maximum_burst == 15U);
	assert(drv_usb_decode_superspeed_endpoint_companion(companion_raw,
	    sizeof(companion_raw) - 1U, &companion) == EINVAL);
	{
		uint8_t malformed[sizeof(companion_raw)];

		memcpy(malformed, companion_raw, sizeof(malformed));
		malformed[1] = DRV_USB_DESCRIPTOR_ENDPOINT;
		assert(drv_usb_decode_superspeed_endpoint_companion(malformed,
		    sizeof(malformed), &companion) == EINVAL);
	}

	assert(drv_usb_init() == 0);
	assert(drv_usb_hcd_register(&hcd, &bus) == 0);
	assert(bus != NULL);
	assert(start_calls == 1);
	assert(bus_count(&hcd, bus) == 1);
	assert(drv_usb_device_hcd_data(drv_usb_bus_root_hub(bus), 0) == 0);
	assert(drv_usb_device_set_hcd_data(drv_usb_bus_root_hub(bus), 0,
	    (uintptr_t)&hcd) == 0);
	assert(drv_usb_device_hcd_data(drv_usb_bus_root_hub(bus), 0) ==
	    (uintptr_t)&hcd);
	assert(drv_usb_device_set_hcd_data(drv_usb_bus_root_hub(bus), 0, 0) == 0);
	assert(drv_usb_device_set_hcd_data(drv_usb_bus_root_hub(bus), 4, 0) ==
	    EINVAL);

	/* Every advertised change bit is acknowledged even while disconnected.
	 * After the first connected change fails reset, a later PRC-only scan
	 * must not retry enumeration without a new connection transition. */
	root_status_value = (1U << 16) | (1U << 17) | (1U << 19) |
	    (1U << 20) | (1U << 21) | (1U << 22) | (1U << 23);
	drv_usb_hcd_root_hub_changed(&hcd);
	assert(cleared_features == ((1U << 16) | (1U << 17) |
	    (1U << 19) | (1U << 20) | (1U << 21) | (1U << 22) |
	    (1U << 23)));
	assert(root_reset_calls == 0);
	cleared_features = 0;
	root_status_value = 1U | (1U << 16);
	drv_usb_hcd_root_hub_changed(&hcd);
	assert(cleared_features == (1U << 16));
	assert(root_reset_calls == 1);
	cleared_features = 0;
	root_status_value |= 1U << 20;
	drv_usb_hcd_root_hub_changed(&hcd);
	assert(cleared_features == (1U << 20));
	assert(root_reset_calls == 1);

	stop_result = EBUSY;
	assert(drv_usb_hcd_unregister(&hcd) == EBUSY);
	assert(quiesce_calls == 1);
	assert(stop_calls == 0);
	assert(bus_count(&hcd, bus) == 1);
	urb = drv_usb_urb_alloc(drv_usb_bus_root_hub(bus), NULL, 0);
	assert(urb == NULL);
	assert(enqueue_calls == 0);

	stop_result = 0;
	assert(drv_usb_hcd_unregister(&hcd) == 0);
	assert(quiesce_calls == 2);
	assert(stop_calls == 1);
	assert(bus_count(&hcd, NULL) == 0);
	assert(drv_usb_hcd_unregister(&hcd) == ENOENT);

	/* A caller may drop its reference while the HCD still owns a pending
	 * URB.  Unregister must retain both the root hub and the URB until the
	 * HCD publishes a terminal state and releases that ownership. */
	quiesce_calls = 0;
	stop_calls = 0;
	assert(drv_usb_hcd_register(&ownership_hcd, &ownership_bus) == 0);
	urb = drv_usb_urb_alloc(drv_usb_bus_root_hub(ownership_bus), NULL, 0);
	assert(urb != NULL);
	assert(drv_usb_urb_setup(urb, NULL, 0, 0, 0, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(urb) == 0);
	assert(pending_urb == urb);
	drv_usb_urb_free(urb);
	assert(drv_usb_hcd_unregister(&ownership_hcd) == EBUSY);
	assert(quiesce_calls == 1);
	assert(stop_calls == 0);
	assert(bus_count(&ownership_hcd, ownership_bus) == 1);
	drv_usb_hcd_complete(&ownership_hcd, pending_urb,
	    DRV_USB_URB_DISCONNECTED, 0);
	pending_urb = NULL;
	assert(drv_usb_hcd_unregister(&ownership_hcd) == 0);
	assert(quiesce_calls == 2);
	assert(stop_calls == 1);
	assert(bus_count(&ownership_hcd, NULL) == 0);

	/* A failed timeout cancellation must not let a synchronous reusable URB
	 * escape while the HCD still owns its request or buffer.  Complete it
	 * after drv_usb_urb_wait() has exhausted its cancellation grace period;
	 * wait_reusable() preserves ETIMEDOUT but waits for ownership release. */
	quiesce_calls = 0;
	stop_calls = 0;
	fake_ticks = 0;
	assert(drv_usb_hcd_register(&ownership_hcd, &ownership_bus) == 0);
	urb = drv_usb_urb_alloc(drv_usb_bus_root_hub(ownership_bus), NULL, 0);
	assert(urb != NULL);
	assert(drv_usb_urb_setup(urb, NULL, 0, 0, 10, NULL, NULL) == 0);
	assert(drv_usb_urb_submit(urb) == 0);
	assert(pending_urb == urb);
	completion_hcd = &ownership_hcd;
	dequeue_result = EBUSY;
	complete_at_tick = 250;
	assert(drv_usb_urb_wait_reusable(urb) == ETIMEDOUT);
	assert(fake_ticks >= 250);
	assert(complete_at_tick == 0);
	assert(drv_usb_urb_status(urb) == DRV_USB_URB_COMPLETE);
	pending_urb = NULL;
	completion_hcd = NULL;
	dequeue_result = ENOTSUP;
	assert(drv_usb_urb_setup(urb, NULL, 0, 0, 0, NULL, NULL) == 0);
	drv_usb_urb_free(urb);
	assert(drv_usb_hcd_unregister(&ownership_hcd) == 0);
	assert(quiesce_calls == 1);
	assert(stop_calls == 1);
	assert(bus_count(&ownership_hcd, NULL) == 0);

	puts("USB HCD unregister test: PASS");
	return 0;
}
