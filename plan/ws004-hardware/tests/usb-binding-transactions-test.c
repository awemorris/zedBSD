/*
 * WS004 HW-T18 production USB binding/transaction fixture.
 *
 * Include the production core so this focused fixture can inspect its private
 * binding state without widening the public USB interface.
 *
 * SPDX-License-Identifier: Zlib
 */
#include "../../../src/drivers/usb.c"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

static atomic_uint checks;
static atomic_size_t live_allocations;

#define CHECK(expression) do { \
	unsigned check_number = atomic_fetch_add_explicit(&checks, 1U, \
	    memory_order_relaxed) + 1U; \
	if (!(expression)) { \
		fprintf(stderr, "check %u failed at %s:%d: %s\n", check_number, \
		    __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

void *
hal_malloc(size_t size)
{
	void *pointer = malloc(size);

	if (pointer != NULL)
		(void)atomic_fetch_add_explicit(&live_allocations, 1U,
		    memory_order_relaxed);
	return pointer;
}

void
hal_free(void *pointer)
{
	if (pointer == NULL)
		return;
	if (atomic_fetch_sub_explicit(&live_allocations, 1U,
	    memory_order_relaxed) == 0)
		abort();
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
	static atomic_uint_fast64_t ticks;

	return atomic_fetch_add_explicit(&ticks, 1U, memory_order_relaxed) + 1U;
}

void
sched_yield(void)
{
	thrd_yield();
}

bool
hal_irq_disable(void)
{
	return true;
}

void
hal_irq_enable(void)
{
}

static const uint8_t fixture_device_descriptor[] = {
	18, 1, 0x00, 0x02, 0xef, 2, 1, 64,
	0x34, 0x12, 0x15, 0x00, 0x00, 0x01, 0, 0, 0, 2
};

/*
 * Interface 0 is Mass Storage-shaped and keeps two long-lived bulk URBs.
 * Interface 1 is Audio-shaped: alt0 is idle and alt1 owns two isochronous
 * endpoints.  Interface 2 is a function primary which claims interface 3.
 */
static const uint8_t fixture_configuration[] = {
	9, 2, 105, 0, 4, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 8, 6, 0x50, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0,
	9, 4, 1, 0, 0, 1, 1, 0, 0,
	9, 4, 1, 1, 2, 1, 1, 0, 0,
	7, 5, 0x83, 1, 192, 0, 1,
	7, 5, 0x04, 1, 192, 0, 1,
	9, 4, 2, 0, 0, 0xfe, 1, 1, 0,
	9, 4, 3, 0, 1, 0xff, 0, 0, 0,
	7, 5, 0x86, 2, 64, 0, 0,
	9, 4, 3, 1, 1, 0xff, 0, 0, 0,
	7, 5, 0x87, 2, 64, 0, 0
};

/* Retained configuration 2 supplies a past endpoint object for the target
 * configuration admission race. */
static const uint8_t fixture_target_configuration[] = {
	9, 2, 32, 0, 1, 2, 0, 0x80, 50,
	9, 4, 4, 0, 2, 0xfd, 0, 0, 0,
	7, 5, 0x88, 2, 64, 0, 0,
	7, 5, 0x09, 2, 64, 0, 0
};

struct fake_controller {
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	unsigned bus_number;
	atomic_uint connected;
	atomic_uint connection_changed;
	atomic_uint configuration;
	atomic_uint alternate[DRV_USB_MAX_INTERFACES];
	atomic_uint endpoint_enabled[256];
	atomic_uint endpoint_enable_count;
	atomic_uint endpoint_disable_count;
	atomic_uint endpoint_reset_count;
	atomic_uint invalid_data_enqueue;
	atomic_uint data_enqueue_count;
	atomic_uint set_interface_count;
	atomic_uint hold_data;
	_Atomic(struct drv_usb_urb *) held_data_urb;
	atomic_uint pause_enqueue_address;
	atomic_uint pause_enqueue_entered;
	atomic_uint pause_enqueue_release;
	atomic_uint pause_enqueue_complete;
	atomic_uint pause_disable_address;
	atomic_uint pause_disable_entered;
	atomic_uint pause_disable_release;
	atomic_uint hold_custom_control;
	atomic_uint control_enqueue_count;
	atomic_uint control_active;
	atomic_uint control_max_active;
	atomic_uint dequeue_busy;
	atomic_uint dequeue_count;
	_Atomic(struct drv_usb_urb *) dequeued_urb;
	atomic_uint completion_after_dequeue;
	_Atomic(struct drv_usb_urb *) held_controls[8];
	atomic_uint pause_configuration_value;
	atomic_uint pause_configuration_entered;
	atomic_uint pause_configuration_release;
	atomic_uint event_sequence;
	atomic_uint enqueue_enter_sequence;
	atomic_uint completion_sequence;
	atomic_uint endpoint_disable_sequence;
	atomic_uint first_device_quiesce_sequence;
	struct drv_usb_interface *expected_detached_primary;
	struct drv_usb_interface *expected_detached_claimed;
	atomic_uint detached_binding_observed;
};

static struct fake_controller *
controller_from_hcd(struct drv_usb_hcd *hcd)
{
	return (struct fake_controller *)hcd->private_data[0];
}

static int
fake_start(struct drv_usb_hcd *hcd)
{
	(void)hcd;
	return 0;
}

static int
fake_quiesce(struct drv_usb_hcd *hcd)
{
	(void)hcd;
	return 0;
}

static void
fake_stop(struct drv_usb_hcd *hcd)
{
	(void)hcd;
}

static void
update_maximum(atomic_uint *maximum, unsigned value)
{
	unsigned old = atomic_load_explicit(maximum, memory_order_relaxed);

	while (old < value && !atomic_compare_exchange_weak_explicit(maximum,
	    &old, value, memory_order_relaxed, memory_order_relaxed))
		;
}

static void
fake_complete(struct fake_controller *controller, struct drv_usb_urb *urb,
	enum drv_usb_urb_status status, size_t actual)
{
	if (atomic_load_explicit(&controller->dequeued_urb,
	    memory_order_acquire) == urb) {
		(void)atomic_fetch_add_explicit(
		    &controller->completion_after_dequeue, 1U,
		    memory_order_relaxed);
		return;
	}
	drv_usb_hcd_complete(&controller->hcd, urb, status, actual);
}

static int
fake_enqueue_control(struct fake_controller *controller,
	struct drv_usb_urb *urb, const struct drv_usb_control_request *request)
{
	const uint8_t *bytes = NULL;
	size_t length = 0;
	unsigned index;

	if (request->request_type ==
	    (DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE) &&
	    request->request == 6U) {
		if ((request->value >> 8) == DRV_USB_DESCRIPTOR_DEVICE) {
			bytes = fixture_device_descriptor;
			length = sizeof(fixture_device_descriptor);
		} else if ((request->value >> 8) ==
		    DRV_USB_DESCRIPTOR_CONFIGURATION) {
			if ((request->value & 0xffU) == 0U) {
				bytes = fixture_configuration;
				length = sizeof(fixture_configuration);
			} else if ((request->value & 0xffU) == 1U) {
				bytes = fixture_target_configuration;
				length = sizeof(fixture_target_configuration);
			}
		}
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_DEVICE) && request->request == 5U) {
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_DEVICE) && request->request == 9U) {
		if (atomic_load_explicit(&controller->pause_configuration_value,
		    memory_order_acquire) != 0U &&
		    atomic_load_explicit(&controller->pause_configuration_value,
		    memory_order_relaxed) == request->value) {
			atomic_store_explicit(
			    &controller->pause_configuration_entered, 1U,
			    memory_order_release);
			while (atomic_load_explicit(
			    &controller->pause_configuration_release,
			    memory_order_acquire) == 0)
				thrd_yield();
		}
		atomic_store_explicit(&controller->configuration, request->value,
		    memory_order_release);
		for (index = 0; index < DRV_USB_MAX_INTERFACES; index++)
			atomic_store_explicit(&controller->alternate[index], 0U,
			    memory_order_release);
		for (index = 0; index < 256U; index++)
			atomic_store_explicit(&controller->endpoint_enabled[index], 0U,
			    memory_order_release);
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_INTERFACE) && request->request == 11U) {
		(void)atomic_fetch_add_explicit(&controller->set_interface_count,
		    1U, memory_order_relaxed);
		atomic_store_explicit(&controller->alternate[request->index],
		    request->value, memory_order_release);
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if ((request->request_type & 0x60U) == DRV_USB_REQUEST_VENDOR &&
	    atomic_load_explicit(&controller->hold_custom_control,
	    memory_order_acquire) != 0) {
		unsigned active;

		index = atomic_fetch_add_explicit(&controller->control_enqueue_count,
		    1U, memory_order_acq_rel);
		CHECK(index < sizeof(controller->held_controls) /
		    sizeof(controller->held_controls[0]));
		active = atomic_fetch_add_explicit(&controller->control_active, 1U,
		    memory_order_acq_rel) + 1U;
		update_maximum(&controller->control_max_active, active);
		atomic_store_explicit(&controller->held_controls[index], urb,
		    memory_order_release);
		return 0;
	}

	if (bytes == NULL) {
		fake_complete(controller, urb, DRV_USB_URB_STALL, 0);
		return 0;
	}
	if (length > drv_usb_urb_length(urb))
		length = drv_usb_urb_length(urb);
	memcpy(drv_usb_urb_buffer(urb), bytes, length);
	fake_complete(controller, urb, DRV_USB_URB_COMPLETE, length);
	return 0;
}

static int
fake_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	const struct drv_usb_control_request *request;
	struct drv_usb_endpoint *endpoint;
	uint8_t address;

	request = drv_usb_urb_control_request(urb);
	if (request != NULL)
		return fake_enqueue_control(controller, urb, request);

	endpoint = drv_usb_urb_endpoint(urb);
	address = drv_usb_endpoint_address(endpoint);
	(void)atomic_fetch_add_explicit(&controller->data_enqueue_count, 1U,
	    memory_order_relaxed);
	if (atomic_load_explicit(&controller->endpoint_enabled[address],
	    memory_order_acquire) == 0 ||
	    ((address == 0x83U || address == 0x04U) &&
	    atomic_load_explicit(&controller->alternate[1],
	    memory_order_acquire) != 1U))
		(void)atomic_fetch_add_explicit(&controller->invalid_data_enqueue,
		    1U, memory_order_relaxed);
	if (atomic_load_explicit(&controller->pause_enqueue_address,
	    memory_order_acquire) == address) {
		atomic_store_explicit(&controller->enqueue_enter_sequence,
		    atomic_fetch_add_explicit(&controller->event_sequence, 1U,
		    memory_order_acq_rel) + 1U, memory_order_release);
		atomic_store_explicit(&controller->pause_enqueue_entered, 1U,
		    memory_order_release);
		while (atomic_load_explicit(&controller->pause_enqueue_release,
		    memory_order_acquire) == 0)
			thrd_yield();
		if (atomic_load_explicit(&controller->pause_enqueue_complete,
		    memory_order_acquire) != 0) {
			atomic_store_explicit(&controller->completion_sequence,
			    atomic_fetch_add_explicit(&controller->event_sequence, 1U,
			    memory_order_acq_rel) + 1U, memory_order_release);
			fake_complete(controller, urb, DRV_USB_URB_COMPLETE,
			    drv_usb_urb_length(urb));
			return 0;
		}
	}
	if (atomic_load_explicit(&controller->hold_data,
	    memory_order_acquire) != 0) {
		struct drv_usb_urb *expected = NULL;

		CHECK(atomic_compare_exchange_strong_explicit(
		    &controller->held_data_urb, &expected, urb,
		    memory_order_release, memory_order_relaxed));
		return 0;
	}
	fake_complete(controller, urb, DRV_USB_URB_COMPLETE,
	    drv_usb_urb_length(urb));
	return 0;
}

static int
fake_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	struct drv_usb_urb *expected;
	unsigned index;

	(void)atomic_fetch_add_explicit(&controller->dequeue_count, 1U,
	    memory_order_relaxed);
	if (atomic_load_explicit(&controller->dequeue_busy,
	    memory_order_acquire) != 0)
		return EBUSY;
	/* Returning zero transfers terminal ownership to the USB core.  Win that
	 * ownership by unlinking the exact held request first; a completion which
	 * already unlinked it makes dequeue lose with EBUSY. */
	expected = urb;
	if (atomic_compare_exchange_strong_explicit(
	    &controller->held_data_urb, &expected, NULL,
	    memory_order_acq_rel, memory_order_acquire))
		goto claimed;
	for (index = 0; index < sizeof(controller->held_controls) /
	    sizeof(controller->held_controls[0]); index++) {
		expected = urb;
		if (!atomic_compare_exchange_strong_explicit(
		    &controller->held_controls[index], &expected, NULL,
		    memory_order_acq_rel, memory_order_acquire))
			continue;
		CHECK(atomic_fetch_sub_explicit(&controller->control_active, 1U,
		    memory_order_acq_rel) != 0U);
		goto claimed;
	}
	return EBUSY;

claimed:
	expected = NULL;
	if (!atomic_compare_exchange_strong_explicit(
	    &controller->dequeued_urb, &expected, urb,
	    memory_order_release, memory_order_relaxed))
		abort();
	return 0;
}

static int
fake_device_quiesce(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	struct drv_usb_interface *primary;
	struct drv_usb_interface *claimed;
	unsigned expected = 0;
	unsigned sequence;

	(void)device;
	primary = controller->expected_detached_primary;
	claimed = controller->expected_detached_claimed;
	if (primary != NULL) {
		CHECK(claimed != NULL);
		CHECK(drv_usb_interface_driver(primary) == NULL);
		CHECK(drv_usb_interface_driver_data(primary) == NULL);
		CHECK(atomic_load_acquire(&primary->binding_state) ==
		    USB_BINDING_DEAD);
		CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
		(void)atomic_fetch_add_explicit(
		    &controller->detached_binding_observed, 1U,
		    memory_order_relaxed);
	}
	sequence = atomic_fetch_add_explicit(&controller->event_sequence, 1U,
	    memory_order_acq_rel) + 1U;
	(void)atomic_compare_exchange_strong_explicit(
	    &controller->first_device_quiesce_sequence, &expected, sequence,
	    memory_order_release, memory_order_relaxed);
	return 0;
}

static void
fake_device_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	(void)hcd;
	(void)device;
}

static int
fake_endpoint_enable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);
	unsigned expected = 0;

	(void)atomic_fetch_add_explicit(&controller->endpoint_enable_count, 1U,
	    memory_order_relaxed);
	return atomic_compare_exchange_strong_explicit(
	    &controller->endpoint_enabled[address], &expected, 1U,
	    memory_order_release, memory_order_relaxed) ? 0 : EALREADY;
}

static int
fake_endpoint_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);
	unsigned expected = 1U;
	int disabled;

	if (atomic_load_explicit(&controller->pause_disable_address,
	    memory_order_acquire) == address) {
		atomic_store_explicit(&controller->pause_disable_entered, 1U,
		    memory_order_release);
		while (atomic_load_explicit(&controller->pause_disable_release,
		    memory_order_acquire) == 0)
			thrd_yield();
	}
	(void)atomic_fetch_add_explicit(&controller->endpoint_disable_count, 1U,
	    memory_order_relaxed);
	disabled = atomic_compare_exchange_strong_explicit(
	    &controller->endpoint_enabled[address], &expected, 0U,
	    memory_order_release, memory_order_relaxed);
	if (!disabled)
		return EALREADY;
	atomic_store_explicit(&controller->endpoint_disable_sequence,
	    atomic_fetch_add_explicit(&controller->event_sequence, 1U,
	    memory_order_acq_rel) + 1U, memory_order_release);
	return 0;
}

static int
fake_endpoint_reset(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);

	(void)atomic_fetch_add_explicit(&controller->endpoint_reset_count, 1U,
	    memory_order_relaxed);
	return atomic_load_explicit(&controller->endpoint_enabled[address],
	    memory_order_acquire) != 0 ? 0 : EBUSY;
}

static int
fake_root_control(struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request, void *buffer,
	size_t length, size_t *actual)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint32_t status;

	if (request->request == 0U && request->request_type == 0xa3U) {
		CHECK(buffer != NULL && length >= sizeof(status));
		status = atomic_load_explicit(&controller->connected,
		    memory_order_acquire) != 0 ? 1U | 2U | 0x400U : 0U;
		if (atomic_load_explicit(&controller->connection_changed,
		    memory_order_acquire) != 0)
			status |= 1U << 16;
		memcpy(buffer, &status, sizeof(status));
		*actual = sizeof(status);
		return 0;
	}
	if (request->request == 1U && request->request_type == 0x23U &&
	    request->value == 16U) {
		atomic_store_explicit(&controller->connection_changed, 0U,
		    memory_order_release);
		*actual = 0;
		return 0;
	}
	*actual = 0;
	return 0;
}

static int
fake_root_reset(struct drv_usb_hcd *hcd, unsigned port)
{
	(void)hcd;
	return port == 1U ? 0 : EINVAL;
}

static const struct drv_usb_hcd_ops fake_ops = {
	.start = fake_start,
	.quiesce = fake_quiesce,
	.stop = fake_stop,
	.urb_enqueue = fake_enqueue,
	.urb_dequeue = fake_dequeue,
	.device_quiesce = fake_device_quiesce,
	.device_disable = fake_device_disable,
	.endpoint_enable = fake_endpoint_enable,
	.endpoint_disable = fake_endpoint_disable,
	.endpoint_reset = fake_endpoint_reset,
	.root_hub_control = fake_root_control,
	.root_port_reset = fake_root_reset
};

static void
register_controller(struct fake_controller *controller)
{
	memset(controller, 0, sizeof(*controller));
	controller->hcd.name = "binding-transaction-fixture";
	controller->hcd.ops = &fake_ops;
	controller->hcd.root_port_count = 1;
	controller->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	controller->hcd.private_data[0] = (uintptr_t)controller;
	atomic_store_explicit(&controller->connected, 1U, memory_order_relaxed);
	atomic_store_explicit(&controller->connection_changed, 1U,
	    memory_order_relaxed);
	CHECK(drv_usb_hcd_register(&controller->hcd, &controller->bus) == 0);
	controller->bus_number = drv_usb_bus_number(controller->bus);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) != NULL);
}

static void
unregister_controller(struct fake_controller *controller)
{
	atomic_store_explicit(&controller->connected, 0U, memory_order_release);
	atomic_store_explicit(&controller->connection_changed, 1U,
	    memory_order_release);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
	CHECK(drv_usb_hcd_unregister(&controller->hcd) == 0);
}

static void
wait_for_nonzero(atomic_uint *value)
{
	unsigned spin;

	for (spin = 0; spin < 10000000U; spin++) {
		if (atomic_load_explicit(value, memory_order_acquire) != 0)
			return;
		thrd_yield();
	}
	CHECK(0 && "timed out waiting for fixture event");
}

static void
wait_for_disconnect(struct drv_usb_device *device)
{
	unsigned spin;

	for (spin = 0; spin < 10000000U; spin++) {
		if (device_is_disconnecting(device) &&
		    (atomic_load_acquire(&device->submit_gate) &
		    USB_IO_GATE_CLOSED) != 0)
			return;
		thrd_yield();
	}
	CHECK(0 && "timed out waiting for disconnect admission close");
}

static void
wait_for_disconnect_publication(struct drv_usb_device *device)
{
	unsigned spin;

	for (spin = 0; spin < 10000000U; spin++) {
		if (device_is_disconnecting(device))
			return;
		thrd_yield();
	}
	CHECK(0 && "timed out waiting for disconnect publication");
}

static void
settle_threads(void)
{
	struct timespec duration = { .tv_sec = 0, .tv_nsec = 10000000L };

	(void)thrd_sleep(&duration, NULL);
}

static void
complete_held_data(struct fake_controller *controller,
	enum drv_usb_urb_status status)
{
	struct drv_usb_urb *urb;

	urb = atomic_exchange_explicit(&controller->held_data_urb, NULL,
	    memory_order_acq_rel);
	CHECK(urb != NULL);
	fake_complete(controller, urb, status,
	    status == DRV_USB_URB_COMPLETE ? drv_usb_urb_length(urb) : 0);
}

static struct drv_usb_urb *
wait_for_control(struct fake_controller *controller, unsigned index)
{
	struct drv_usb_urb *urb;
	unsigned spin;

	for (spin = 0; spin < 10000000U; spin++) {
		urb = atomic_load_explicit(&controller->held_controls[index],
		    memory_order_acquire);
		if (urb != NULL)
			return urb;
		thrd_yield();
	}
	CHECK(0 && "timed out waiting for held control URB");
	return NULL;
}

static void
complete_control(struct fake_controller *controller, unsigned index)
{
	struct drv_usb_urb *urb;

	urb = atomic_exchange_explicit(&controller->held_controls[index], NULL,
	    memory_order_acq_rel);
	CHECK(urb != NULL);
	CHECK(atomic_fetch_sub_explicit(&controller->control_active, 1U,
	    memory_order_acq_rel) != 0);
	fake_complete(controller, urb, DRV_USB_URB_COMPLETE,
	    drv_usb_urb_length(urb));
}

struct callback_fixture {
	atomic_uint entered;
	atomic_uint release;
	atomic_uint returned;
};

static void
blocking_callback(struct drv_usb_urb *urb, void *argument)
{
	struct callback_fixture *fixture = argument;

	(void)urb;
	atomic_store_explicit(&fixture->entered, 1U, memory_order_release);
	while (atomic_load_explicit(&fixture->release,
	    memory_order_acquire) == 0)
		thrd_yield();
	atomic_store_explicit(&fixture->returned, 1U, memory_order_release);
}

static void
counting_callback(struct drv_usb_urb *urb, void *argument)
{
	atomic_uint *count = argument;

	(void)urb;
	(void)atomic_fetch_add_explicit(count, 1U, memory_order_relaxed);
}

struct completion_fixture {
	struct fake_controller *controller;
	enum drv_usb_urb_status status;
};

static int
complete_data_thread(void *argument)
{
	struct completion_fixture *fixture = argument;

	complete_held_data(fixture->controller, fixture->status);
	return 0;
}

struct cancel_fixture {
	struct drv_usb_urb *urb;
	atomic_uint *release;
	atomic_uint started;
	int result;
};

static int
cancel_thread(void *argument)
{
	struct cancel_fixture *fixture = argument;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	while (fixture->release != NULL && atomic_load_explicit(fixture->release,
	    memory_order_acquire) == 0U)
		thrd_yield();
	fixture->result = drv_usb_urb_cancel(fixture->urb);
	return 0;
}

struct try_completion_fixture {
	struct fake_controller *controller;
	atomic_uint *release;
	atomic_uint started;
	unsigned completed;
};

static int
try_complete_data_thread(void *argument)
{
	struct try_completion_fixture *fixture = argument;
	struct drv_usb_urb *urb;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	while (fixture->release != NULL && atomic_load_explicit(fixture->release,
	    memory_order_acquire) == 0U)
		thrd_yield();
	urb = atomic_exchange_explicit(&fixture->controller->held_data_urb, NULL,
	    memory_order_acq_rel);
	if (urb != NULL) {
		fake_complete(fixture->controller, urb, DRV_USB_URB_COMPLETE,
		    drv_usb_urb_length(urb));
		fixture->completed = 1U;
	}
	return 0;
}

struct alternate_fixture {
	struct drv_usb_interface *interface;
	unsigned alternate;
	atomic_uint done;
	int result;
};

static int
set_alternate_thread(void *argument)
{
	struct alternate_fixture *fixture = argument;

	fixture->result = drv_usb_interface_set_alternate(fixture->interface,
	    fixture->alternate);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct submit_thread_fixture {
	struct drv_usb_urb *urb;
	atomic_uint started;
	atomic_uint done;
	int result;
};

static int
submit_thread(void *argument)
{
	struct submit_thread_fixture *fixture = argument;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	fixture->result = drv_usb_urb_submit(fixture->urb);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct configuration_thread_fixture {
	struct drv_usb_device *device;
	unsigned configuration_value;
	atomic_uint done;
	int result;
};

static int
configuration_thread(void *argument)
{
	struct configuration_thread_fixture *fixture = argument;

	fixture->result = drv_usb_device_set_configuration(fixture->device,
	    fixture->configuration_value);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct disconnect_thread_fixture {
	struct fake_controller *controller;
	atomic_uint started;
	atomic_uint done;
};

static int
disconnect_thread(void *argument)
{
	struct disconnect_thread_fixture *fixture = argument;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	atomic_store_explicit(&fixture->controller->connected, 0U,
	    memory_order_release);
	atomic_store_explicit(&fixture->controller->connection_changed, 1U,
	    memory_order_release);
	drv_usb_hcd_root_hub_changed(&fixture->controller->hcd);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct quarantine_thread_fixture {
	struct fake_controller *controller;
	struct drv_usb_device *device;
	atomic_uint started;
	atomic_uint done;
	unsigned done_sequence;
};

static int
quarantine_thread(void *argument)
{
	struct quarantine_thread_fixture *fixture = argument;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	device_quarantine_selection(fixture->device, "fixture", EIO);
	fixture->done_sequence = atomic_fetch_add_explicit(
	    &fixture->controller->event_sequence, 1U,
	    memory_order_acq_rel) + 1U;
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct detach_thread_fixture {
	struct drv_usb_interface *interface;
	unsigned flags;
	atomic_uint done;
	int result;
};

static int
detach_thread(void *argument)
{
	struct detach_thread_fixture *fixture = argument;

	fixture->result = drv_usb_interface_detach(fixture->interface,
	    fixture->flags);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

struct getter_snapshot_fixture {
	struct drv_usb_interface *interface;
	const struct drv_usb_host_interface *alternate0;
	const struct drv_usb_host_interface *alternate1;
	atomic_uint stop;
	atomic_uint reads;
	atomic_uint invalid;
};

static void
getter_snapshot_invalid(struct getter_snapshot_fixture *fixture)
{
	atomic_store_explicit(&fixture->invalid, 1U, memory_order_relaxed);
}

static int
getter_snapshot_reader(void *argument)
{
	struct getter_snapshot_fixture *fixture = argument;
	const struct drv_usb_interface_descriptor *descriptor, *descriptor0;
	const struct drv_usb_interface_descriptor *descriptor1;
	const struct drv_usb_host_interface *active;
	struct drv_usb_endpoint *endpoint0, *endpoint1, *endpoint2;
	struct drv_usb_endpoint *input, *output;
	struct drv_usb_endpoint *alternate1_input, *alternate1_output;
	unsigned endpoint_count;

	descriptor0 = drv_usb_host_interface_descriptor(fixture->alternate0);
	descriptor1 = drv_usb_host_interface_descriptor(fixture->alternate1);
	alternate1_input = drv_usb_host_interface_endpoint(
	    fixture->alternate1, 0U);
	alternate1_output = drv_usb_host_interface_endpoint(
	    fixture->alternate1, 1U);
	while (atomic_load_explicit(&fixture->stop, memory_order_acquire) == 0U) {
		descriptor = drv_usb_interface_descriptor(fixture->interface);
		if (descriptor == descriptor0) {
			if (descriptor->alternate_setting != 0U ||
			    descriptor->endpoint_count != 0U)
				getter_snapshot_invalid(fixture);
		} else if (descriptor == descriptor1) {
			if (descriptor->alternate_setting != 1U ||
			    descriptor->endpoint_count != 2U)
				getter_snapshot_invalid(fixture);
		} else {
			getter_snapshot_invalid(fixture);
		}
		active = drv_usb_interface_active_alternate(fixture->interface);
		if (active != fixture->alternate0 && active != fixture->alternate1)
			getter_snapshot_invalid(fixture);
		endpoint_count = drv_usb_interface_endpoint_count(
		    fixture->interface);
		if (endpoint_count != 0U && endpoint_count != 2U)
			getter_snapshot_invalid(fixture);
		endpoint0 = drv_usb_interface_endpoint(fixture->interface, 0U);
		endpoint1 = drv_usb_interface_endpoint(fixture->interface, 1U);
		endpoint2 = drv_usb_interface_endpoint(fixture->interface, 2U);
		if (endpoint0 != NULL && endpoint0 != alternate1_input)
			getter_snapshot_invalid(fixture);
		if (endpoint1 != NULL && endpoint1 != alternate1_output)
			getter_snapshot_invalid(fixture);
		if (endpoint2 != NULL)
			getter_snapshot_invalid(fixture);
		input = drv_usb_interface_find_endpoint(fixture->interface,
		    DRV_USB_TRANSFER_ISOCHRONOUS, DRV_USB_DIR_IN, NULL);
		output = drv_usb_interface_find_endpoint(fixture->interface,
		    DRV_USB_TRANSFER_ISOCHRONOUS, DRV_USB_DIR_OUT, NULL);
		if (input != NULL && input != alternate1_input)
			getter_snapshot_invalid(fixture);
		if (output != NULL && output != alternate1_output)
			getter_snapshot_invalid(fixture);
		(void)atomic_fetch_add_explicit(&fixture->reads, 1U,
		    memory_order_relaxed);
	}
	return 0;
}

enum binding_attach_mode {
	BINDING_ATTACH_CLAIM_BUSY,
	BINDING_ATTACH_ABORT_CLEAN,
	BINDING_ATTACH_ABORT_RETAIN,
	BINDING_ATTACH_SUCCESS
};

static struct {
	enum binding_attach_mode mode;
	unsigned cleanup_may_finish;
	unsigned fail_normal_detach;
	unsigned keep_persistent_urb;
	unsigned persistent_urb_freed;
	unsigned attach_count;
	unsigned detach_count;
	atomic_uint detach_entered;
	unsigned attach_failed_count;
	unsigned forced_count;
	unsigned token;
	struct drv_usb_interface *primary;
	struct drv_usb_interface *claimed;
	struct drv_usb_interface *claim_target;
	struct drv_usb_urb *persistent_urb;
	uint8_t persistent_buffer[64];
} binding_driver_state;

static struct drv_usb_driver binding_driver;

static int
binding_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	return 100;
}

static int
binding_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *claimed;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	uint8_t byte = 0;

	(void)id;
	binding_driver_state.attach_count++;
	binding_driver_state.primary = interface;
	CHECK(drv_usb_interface_driver(interface) == &binding_driver);
	CHECK(atomic_load_acquire(&interface->binding_state) ==
	    USB_BINDING_PROBING);
	CHECK(drv_usb_interface_set_driver_data(interface,
	    &binding_driver_state.token) == 0);
	configuration = drv_usb_device_active_configuration(
	    drv_usb_interface_device(interface));
	claimed = drv_usb_configuration_find_interface(configuration, 3U);
	CHECK(claimed != NULL);
	binding_driver_state.claim_target = claimed;
	if (binding_driver_state.mode == BINDING_ATTACH_CLAIM_BUSY) {
		CHECK(drv_usb_interface_claim(interface, claimed) == EBUSY);
		CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
		return EBUSY;
	}
	CHECK(drv_usb_interface_claim(interface, claimed) == 0);
	binding_driver_state.claimed = claimed;

	/* PROBING is already an owner: attach may issue I/O on a claimed peer. */
	endpoint = drv_usb_interface_endpoint(claimed, 0U);
	CHECK(endpoint != NULL);
	urb = drv_usb_urb_alloc(drv_usb_interface_device(interface), endpoint, 0);
	CHECK(urb != NULL);
	CHECK(drv_usb_urb_setup(urb, &byte, sizeof(byte), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(urb) == 0);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	drv_usb_urb_free(urb);
	if (binding_driver_state.keep_persistent_urb) {
		binding_driver_state.persistent_urb = drv_usb_urb_alloc(
		    drv_usb_interface_device(interface), endpoint, 0);
		CHECK(binding_driver_state.persistent_urb != NULL);
		CHECK(drv_usb_urb_setup(binding_driver_state.persistent_urb,
		    binding_driver_state.persistent_buffer,
		    sizeof(binding_driver_state.persistent_buffer), 0, 0,
		    NULL, NULL) == 0);
	}

	return binding_driver_state.mode == BINDING_ATTACH_SUCCESS ? 0 : EIO;
}

static int
binding_detach(struct drv_usb_interface *interface, unsigned flags)
{
	binding_driver_state.detach_count++;
	atomic_store_explicit(&binding_driver_state.detach_entered, 1U,
	    memory_order_release);
	CHECK(interface == binding_driver_state.primary);
	CHECK(drv_usb_interface_driver(interface) != NULL);
	CHECK(atomic_load_acquire(&interface->binding_state) ==
	    USB_BINDING_UNBINDING);
	CHECK(drv_usb_interface_driver_data(interface) ==
	    &binding_driver_state.token);
	if (binding_driver_state.claimed != NULL)
		CHECK(drv_usb_interface_claimed_by(binding_driver_state.claimed) ==
		    interface);
	else
		CHECK(drv_usb_interface_claimed_by(
		    binding_driver_state.claim_target) == NULL);
	if ((flags & DRV_USB_DETACH_ATTACH_FAILED) != 0)
		binding_driver_state.attach_failed_count++;
	if ((flags & DRV_USB_DETACH_FORCE) != 0)
		binding_driver_state.forced_count++;
	if (binding_driver_state.persistent_urb != NULL) {
		drv_usb_urb_free(binding_driver_state.persistent_urb);
		binding_driver_state.persistent_urb = NULL;
		binding_driver_state.persistent_urb_freed++;
	}
	if (binding_driver_state.mode == BINDING_ATTACH_ABORT_RETAIN &&
	    !binding_driver_state.cleanup_may_finish)
		return EBUSY;
	if (binding_driver_state.mode == BINDING_ATTACH_SUCCESS &&
	    binding_driver_state.fail_normal_detach &&
	    (flags & DRV_USB_DETACH_FORCE) == 0)
		return EBUSY;
	return 0;
}

static const struct drv_usb_id binding_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
	.interface_class = 0xfe,
	.interface_subclass = 1
}};

static struct drv_usb_driver binding_driver = {
	.name = "binding-lifecycle-fixture",
	.ids = binding_ids,
	.id_count = sizeof(binding_ids) / sizeof(binding_ids[0]),
	.match = binding_match,
	.attach = binding_attach,
	.detach = binding_detach
};

struct control_thread_fixture {
	struct drv_usb_device *device;
	uint8_t request;
	unsigned timeout_ms;
	uint8_t buffer[16];
	atomic_uint started;
	atomic_uint done;
	int result;
};

static int
control_thread(void *argument)
{
	struct control_thread_fixture *fixture = argument;
	size_t actual = 0;

	atomic_store_explicit(&fixture->started, 1U, memory_order_release);
	fixture->result = drv_usb_control(fixture->device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_VENDOR | DRV_USB_RECIP_DEVICE,
	    fixture->request, 0, 0, fixture->buffer, sizeof(fixture->buffer),
	    fixture->timeout_ms, &actual);
	atomic_store_explicit(&fixture->done, 1U, memory_order_release);
	return 0;
}

static void
exercise_interface_transactions(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *storage, *stream;
	const struct drv_usb_host_interface *stream_alt1;
	struct drv_usb_endpoint *storage_in, *storage_out, *stream_in;
	struct drv_usb_urb *storage_in_urb, *storage_out_urb, *stream_urb;
	struct drv_usb_iso_packet packet = { .offset = 0, .length = 32 };
	struct callback_fixture callback = { 0 };
	struct completion_fixture completion;
	struct alternate_fixture switching;
	struct submit_thread_fixture first_submit = { 0 }, second_submit = { 0 };
	thrd_t completion_worker, switching_worker, first_submit_worker;
	thrd_t second_submit_worker;
	uint8_t storage_in_buffer[64], storage_out_buffer[64], stream_buffer[32];
	unsigned before;

	configuration = drv_usb_device_active_configuration(device);
	CHECK(configuration != NULL);
	storage = drv_usb_configuration_find_interface(configuration, 0U);
	stream = drv_usb_configuration_find_interface(configuration, 1U);
	CHECK(storage != NULL && stream != NULL);
	CHECK(drv_usb_interface_descriptor(stream)->alternate_setting == 0U);
	storage_in = drv_usb_interface_find_endpoint(storage,
	    DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	storage_out = drv_usb_interface_find_endpoint(storage,
	    DRV_USB_TRANSFER_BULK, DRV_USB_DIR_OUT, NULL);
	stream_alt1 = drv_usb_interface_find_alternate(stream, 1U);
	CHECK(storage_in != NULL && storage_out != NULL && stream_alt1 != NULL);
	stream_in = drv_usb_host_interface_endpoint(stream_alt1, 0U);
	CHECK(stream_in != NULL && drv_usb_endpoint_address(stream_in) == 0x83U);

	/* Storage keeps its BOT-shaped RX/TX objects for every stream switch. */
	storage_in_urb = drv_usb_urb_alloc(device, storage_in, 0);
	storage_out_urb = drv_usb_urb_alloc(device, storage_out, 0);
	stream_urb = drv_usb_urb_alloc(device, stream_in, 1U);
	CHECK(storage_in_urb != NULL && storage_out_urb != NULL &&
	    stream_urb != NULL);
	CHECK(drv_usb_urb_setup(storage_in_urb, storage_in_buffer,
	    sizeof(storage_in_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup(storage_out_urb, storage_out_buffer,
	    sizeof(storage_out_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);

	before = atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed);
	CHECK(drv_usb_urb_submit(stream_urb) == ENODEV);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before);
	CHECK(drv_usb_interface_set_alternate(stream, 1U) == 0);
	CHECK(drv_usb_interface_descriptor(stream)->alternate_setting == 1U);
	CHECK(atomic_load_explicit(&controller->endpoint_enabled[0x83],
	    memory_order_acquire) == 1U);

	/* The same inactive-alt URB becomes usable, drains, and remains reusable. */
	atomic_store_explicit(&controller->hold_data, 1U, memory_order_release);
	CHECK(drv_usb_urb_submit(stream_urb) == 0);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == stream_urb);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(stream_urb, 0) == 0);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == 0);
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);
	CHECK(drv_usb_urb_submit(stream_urb) == ENODEV);
	CHECK(drv_usb_interface_set_alternate(stream, 1U) == 0);
	CHECK(drv_usb_urb_submit(stream_urb) == 0);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(stream_urb, 0) == 0);

	/* Target in-flight work wins atomically and rejects the switch. */
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);
	CHECK(drv_usb_urb_submit(stream_urb) == 0);
	before = atomic_load_explicit(&controller->set_interface_count,
	    memory_order_relaxed);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == EBUSY);
	CHECK(atomic_load_explicit(&controller->set_interface_count,
	    memory_order_relaxed) == before);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(stream_urb, 0) == 0);

	/* A terminal callback still owns the target gate until it returns. */
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, blocking_callback, &callback) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);
	CHECK(drv_usb_urb_submit(stream_urb) == 0);
	completion.controller = controller;
	completion.status = DRV_USB_URB_COMPLETE;
	CHECK(thrd_create(&completion_worker, complete_data_thread,
	    &completion) == thrd_success);
	wait_for_nonzero(&callback.entered);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == EBUSY);
	atomic_store_explicit(&callback.release, 1U, memory_order_release);
	CHECK(thrd_join(completion_worker, NULL) == thrd_success);
	CHECK(atomic_load_explicit(&callback.returned,
	    memory_order_acquire) == 1U);
	CHECK(drv_usb_urb_drain(stream_urb, 0) == 0);

	/* Storage activity is a sibling and therefore does not close stream I/O. */
	CHECK(drv_usb_urb_submit(storage_in_urb) == 0);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == storage_in_urb);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == 0);
	CHECK(drv_usb_interface_descriptor(stream)->alternate_setting == 0U);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(storage_in_urb, 0) == 0);

	/* The switching winner closes admission before touching HCD endpoints. */
	CHECK(drv_usb_interface_set_alternate(stream, 1U) == 0);
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);
	atomic_store_explicit(&controller->pause_disable_address, 0x83U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_disable_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_disable_release, 0U,
	    memory_order_relaxed);
	memset(&switching, 0, sizeof(switching));
	switching.interface = stream;
	switching.alternate = 0U;
	CHECK(thrd_create(&switching_worker, set_alternate_thread,
	    &switching) == thrd_success);
	wait_for_nonzero(&controller->pause_disable_entered);
	before = atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed);
	CHECK(drv_usb_urb_submit(stream_urb) == EBUSY);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before);
	atomic_store_explicit(&controller->pause_disable_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(switching_worker, NULL) == thrd_success);
	CHECK(switching.result == 0);
	CHECK(atomic_load_explicit(&controller->invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	atomic_store_explicit(&controller->pause_disable_address, 0U,
	    memory_order_release);

	/* Both persistent Storage requests still work after every stream switch. */
	atomic_store_explicit(&controller->hold_data, 0U, memory_order_release);
	CHECK(drv_usb_urb_setup(storage_in_urb, storage_in_buffer,
	    sizeof(storage_in_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(storage_in_urb) == 0);
	CHECK(drv_usb_urb_drain(storage_in_urb, 0) == 0);
	CHECK(drv_usb_urb_submit(storage_out_urb) == 0);
	CHECK(drv_usb_urb_drain(storage_out_urb, 0) == 0);

	/* Two callers may observe one idle URB, but the HCD-ownership CAS gives
	 * exactly one submitter the admission bookkeeping.  The loser must not
	 * decrement either interface gate. */
	CHECK(drv_usb_interface_set_alternate(stream, 1U) == 0);
	CHECK(drv_usb_urb_setup(stream_urb, stream_buffer,
	    sizeof(stream_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_setup_isochronous(stream_urb, &packet, 1U) == 0);
	atomic_store_explicit(&controller->hold_data, 1U, memory_order_release);
	atomic_store_explicit(&controller->pause_enqueue_address, 0x83U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_enqueue_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_release, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_complete, 0U,
	    memory_order_relaxed);
	before = atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed);
	first_submit.urb = stream_urb;
	second_submit.urb = stream_urb;
	CHECK(thrd_create(&first_submit_worker, submit_thread, &first_submit) ==
	    thrd_success);
	wait_for_nonzero(&controller->pause_enqueue_entered);
	CHECK(thrd_create(&second_submit_worker, submit_thread, &second_submit) ==
	    thrd_success);
	CHECK(thrd_join(second_submit_worker, NULL) == thrd_success);
	CHECK(second_submit.result == EBUSY);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before + 1U);
	atomic_store_explicit(&controller->pause_enqueue_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(first_submit_worker, NULL) == thrd_success);
	CHECK(first_submit.result == 0);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == stream_urb);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(stream_urb, 0) == 0);
	CHECK((atomic_load_acquire(&stream->io_gate) & USB_IO_GATE_COUNT_MASK) ==
	    0U);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == 0);
	atomic_store_explicit(&controller->pause_enqueue_address, 0U,
	    memory_order_release);
	atomic_store_explicit(&controller->hold_data, 0U, memory_order_release);

	drv_usb_urb_free(stream_urb);
	drv_usb_urb_free(storage_out_urb);
	drv_usb_urb_free(storage_in_urb);
}

static void
exercise_public_getter_snapshot_race(struct drv_usb_device *device)
{
	struct getter_snapshot_fixture fixture = { 0 };
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *stream;
	thrd_t readers[3];
	unsigned index, iteration;
	int switch_error = 0;

	configuration = drv_usb_device_active_configuration(device);
	stream = drv_usb_configuration_find_interface(configuration, 1U);
	CHECK(stream != NULL);
	fixture.interface = stream;
	fixture.alternate0 = drv_usb_interface_find_alternate(stream, 0U);
	fixture.alternate1 = drv_usb_interface_find_alternate(stream, 1U);
	CHECK(fixture.alternate0 != NULL && fixture.alternate1 != NULL);
	CHECK(drv_usb_host_interface_endpoint_count(fixture.alternate0) == 0U);
	CHECK(drv_usb_host_interface_endpoint_count(fixture.alternate1) == 2U);
	CHECK(drv_usb_interface_set_alternate(stream, 0U) == 0);
	for (index = 0; index < sizeof(readers) / sizeof(readers[0]); index++)
		CHECK(thrd_create(&readers[index], getter_snapshot_reader,
		    &fixture) == thrd_success);
	wait_for_nonzero(&fixture.reads);

	/* Each public getter chooses its own acquire-loaded immutable snapshot.
	 * Calls may legitimately observe different alternates, but no individual
	 * endpoint lookup may combine the zero-endpoint alt0 count/base with the
	 * two-endpoint alt1 count/base. */
	for (iteration = 0; iteration < 512U; iteration++) {
		switch_error = drv_usb_interface_set_alternate(stream, 1U);
		if (switch_error != 0)
			break;
		thrd_yield();
		switch_error = drv_usb_interface_set_alternate(stream, 0U);
		if (switch_error != 0)
			break;
		thrd_yield();
	}
	atomic_store_explicit(&fixture.stop, 1U, memory_order_release);
	for (index = 0; index < sizeof(readers) / sizeof(readers[0]); index++)
		CHECK(thrd_join(readers[index], NULL) == thrd_success);
	CHECK(switch_error == 0);
	CHECK(atomic_load_explicit(&fixture.reads,
	    memory_order_relaxed) > 512U);
	CHECK(atomic_load_explicit(&fixture.invalid,
	    memory_order_relaxed) == 0U);
	CHECK(drv_usb_interface_active_alternate(stream) == fixture.alternate0);
}

static void
exercise_hcd_dequeue_ownership(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *storage;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb, *expected;
	atomic_uint callback_count = 0;
	atomic_uint release = 0;
	struct cancel_fixture cancelling;
	struct try_completion_fixture completing;
	thrd_t cancel_worker, completion_worker;
	uint8_t buffer[32];
	unsigned callback_base, dequeue_base, iteration;

	configuration = drv_usb_device_active_configuration(device);
	storage = drv_usb_configuration_find_interface(configuration, 0U);
	endpoint = drv_usb_interface_find_endpoint(storage,
	    DRV_USB_TRANSFER_BULK, DRV_USB_DIR_IN, NULL);
	CHECK(endpoint != NULL);
	urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(urb != NULL);

	/* Synchronous HCD completion may finish inside submit, but must leave the
	 * callback published once and no HCD ownership behind. */
	CHECK(drv_usb_urb_setup(urb, buffer, sizeof(buffer), 0, 0,
	    counting_callback, &callback_count) == 0);
	CHECK(drv_usb_urb_submit(urb) == 0);
	CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_COMPLETE);
	CHECK(atomic_load_explicit(&callback_count,
	    memory_order_relaxed) == 1U);
	CHECK(hal_atomic_load_acquire(&urb->hcd_owned) == 0U);

	/* A successful dequeue first removes the held HCD request.  The core then
	 * owns terminal publication, and the fake HCD records that a later complete
	 * for this request would violate the ownership transfer. */
	atomic_store_explicit(&controller->hold_data, 1U, memory_order_release);
	atomic_store_explicit(&controller->dequeue_busy, 0U,
	    memory_order_release);
	dequeue_base = atomic_load_explicit(&controller->dequeue_count,
	    memory_order_relaxed);
	CHECK(drv_usb_urb_setup(urb, buffer, sizeof(buffer), 0, 0,
	    counting_callback, &callback_count) == 0);
	CHECK(drv_usb_urb_submit(urb) == 0);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == urb);
	CHECK(drv_usb_urb_cancel(urb) == 0);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == NULL);
	CHECK(atomic_load_explicit(&controller->dequeued_urb,
	    memory_order_acquire) == urb);
	CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_CANCELLED);
	CHECK(hal_atomic_load_acquire(&urb->hcd_owned) == 0U);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	CHECK(atomic_load_explicit(&callback_count,
	    memory_order_relaxed) == 2U);
	CHECK(atomic_load_explicit(&controller->dequeue_count,
	    memory_order_relaxed) == dequeue_base + 1U);
	CHECK(atomic_load_explicit(&controller->completion_after_dequeue,
	    memory_order_relaxed) == 0U);
	expected = urb;
	CHECK(atomic_compare_exchange_strong_explicit(
	    &controller->dequeued_urb, &expected, NULL,
	    memory_order_release, memory_order_relaxed));

	/* If dequeue cannot unlink the request, HCD ownership remains live and its
	 * completion is the only terminal winner. */
	atomic_store_explicit(&controller->dequeue_busy, 1U,
	    memory_order_release);
	CHECK(drv_usb_urb_setup(urb, buffer, sizeof(buffer), 0, 0,
	    counting_callback, &callback_count) == 0);
	CHECK(drv_usb_urb_submit(urb) == 0);
	CHECK(drv_usb_urb_cancel(urb) == EBUSY);
	CHECK(atomic_load_explicit(&controller->held_data_urb,
	    memory_order_acquire) == urb);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_COMPLETE);
	CHECK(atomic_load_explicit(&callback_count,
	    memory_order_relaxed) == 3U);
	CHECK(drv_usb_urb_cancel(urb) == EINVAL);
	CHECK(atomic_load_explicit(&controller->completion_after_dequeue,
	    memory_order_relaxed) == 0U);

	/* Race the two legal terminal claimants repeatedly.  The held-pointer
	 * exchange is the shared ownership decision: cancel success excludes HCD
	 * completion, while a completion winner leaves cancel with EBUSY/EINVAL. */
	atomic_store_explicit(&controller->dequeue_busy, 0U,
	    memory_order_release);
	for (iteration = 0; iteration < 32U; iteration++) {
		memset(&cancelling, 0, sizeof(cancelling));
		memset(&completing, 0, sizeof(completing));
		atomic_store_explicit(&release, 0U, memory_order_relaxed);
		cancelling.urb = urb;
		cancelling.release = &release;
		completing.controller = controller;
		completing.release = &release;
		CHECK(drv_usb_urb_setup(urb, buffer, sizeof(buffer), 0, 0,
		    counting_callback, &callback_count) == 0);
		CHECK(drv_usb_urb_submit(urb) == 0);
		CHECK(atomic_load_explicit(&controller->held_data_urb,
		    memory_order_acquire) == urb);
		callback_base = atomic_load_explicit(&callback_count,
		    memory_order_relaxed);
		CHECK(thrd_create(&cancel_worker, cancel_thread, &cancelling) ==
		    thrd_success);
		CHECK(thrd_create(&completion_worker, try_complete_data_thread,
		    &completing) == thrd_success);
		wait_for_nonzero(&cancelling.started);
		wait_for_nonzero(&completing.started);
		atomic_store_explicit(&release, 1U, memory_order_release);
		CHECK(thrd_join(cancel_worker, NULL) == thrd_success);
		CHECK(thrd_join(completion_worker, NULL) == thrd_success);
		CHECK(drv_usb_urb_drain(urb, 0) == 0);
		CHECK(atomic_load_explicit(&callback_count,
		    memory_order_relaxed) == callback_base + 1U);
		if (cancelling.result == 0) {
			CHECK(completing.completed == 0U);
			CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_CANCELLED);
			CHECK(atomic_load_explicit(&controller->dequeued_urb,
			    memory_order_acquire) == urb);
			expected = urb;
			CHECK(atomic_compare_exchange_strong_explicit(
			    &controller->dequeued_urb, &expected, NULL,
			    memory_order_release, memory_order_relaxed));
		} else {
			CHECK(cancelling.result == EBUSY ||
			    cancelling.result == EINVAL);
			CHECK(completing.completed == 1U);
			CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_COMPLETE);
			CHECK(atomic_load_explicit(&controller->dequeued_urb,
			    memory_order_acquire) == NULL);
		}
		CHECK(atomic_load_explicit(&controller->completion_after_dequeue,
		    memory_order_relaxed) == 0U);
	}
	atomic_store_explicit(&controller->dequeue_busy, 0U,
	    memory_order_release);
	atomic_store_explicit(&controller->hold_data, 0U, memory_order_release);
	drv_usb_urb_free(urb);
}

static void
exercise_target_configuration_race(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *first, *target;
	struct drv_usb_interface *target_interface;
	struct drv_usb_endpoint *target_endpoint;
	struct drv_usb_urb *past_urb;
	struct configuration_thread_fixture switching = { 0 };
	thrd_t switching_worker;
	uint8_t buffer[32];
	unsigned before;

	first = drv_usb_device_active_configuration(device);
	target = drv_usb_device_configuration(device, 1U);
	CHECK(first != NULL && target != NULL && first != target);
	CHECK(drv_usb_configuration_descriptor(first)->configuration_value == 1U);
	CHECK(drv_usb_configuration_descriptor(target)->configuration_value == 2U);
	CHECK(drv_usb_device_set_configuration(device, 2U) == 0);
	target_interface = drv_usb_configuration_find_interface(target, 4U);
	CHECK(target_interface != NULL);
	target_endpoint = drv_usb_interface_endpoint(target_interface, 0U);
	CHECK(target_endpoint != NULL &&
	    drv_usb_endpoint_address(target_endpoint) == 0x88U);
	past_urb = drv_usb_urb_alloc(device, target_endpoint, 0);
	CHECK(past_urb != NULL);
	CHECK(drv_usb_urb_setup(past_urb, buffer, sizeof(buffer), 0, 0,
	    NULL, NULL) == 0);

	/* The endpoint object remains allocated after its configuration becomes
	 * inactive, but it cannot submit from that stale selection. */
	CHECK(drv_usb_device_set_configuration(device, 1U) == 0);
	before = atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed);
	CHECK(drv_usb_urb_submit(past_urb) == ENODEV);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before);

	/* A switch to that target closes both configurations before SET_CONFIG.
	 * A stale submit arriving in the wire transaction loses with EBUSY and
	 * never reaches an inactive HCD endpoint. */
	atomic_store_explicit(&controller->pause_configuration_value, 2U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_configuration_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_configuration_release, 0U,
	    memory_order_relaxed);
	switching.device = device;
	switching.configuration_value = 2U;
	CHECK(thrd_create(&switching_worker, configuration_thread,
	    &switching) == thrd_success);
	wait_for_nonzero(&controller->pause_configuration_entered);
	CHECK(drv_usb_urb_submit(past_urb) == EBUSY);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before);
	atomic_store_explicit(&controller->pause_configuration_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(switching_worker, NULL) == thrd_success);
	CHECK(switching.result == 0);
	CHECK(drv_usb_device_active_configuration(device) == target);
	atomic_store_explicit(&controller->pause_configuration_value, 0U,
	    memory_order_release);

	CHECK(drv_usb_urb_setup(past_urb, buffer, sizeof(buffer), 0, 0,
	    NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(past_urb) == 0);
	CHECK(drv_usb_urb_drain(past_urb, 0) == 0);
	CHECK(drv_usb_device_set_configuration(device, 1U) == 0);
	CHECK(drv_usb_device_active_configuration(device) == first);
	drv_usb_urb_free(past_urb);
	CHECK(atomic_load_explicit(&controller->invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
}

static void
exercise_cross_device_rejection(struct drv_usb_device *first,
	struct drv_usb_device *second)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *storage;
	struct drv_usb_endpoint *endpoint;

	configuration = drv_usb_device_active_configuration(second);
	CHECK(configuration != NULL);
	storage = drv_usb_configuration_find_interface(configuration, 0U);
	CHECK(storage != NULL);
	endpoint = drv_usb_interface_endpoint(storage, 0U);
	CHECK(endpoint != NULL && drv_usb_endpoint_device(endpoint) == second);
	CHECK(drv_usb_urb_alloc(first, endpoint, 0) == NULL);
}

static struct drv_usb_urb *
allocate_storage_input(struct drv_usb_device *device, uint8_t *buffer,
	size_t length)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *storage;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;

	configuration = drv_usb_device_active_configuration(device);
	CHECK(configuration != NULL);
	storage = drv_usb_configuration_find_interface(configuration, 0U);
	CHECK(storage != NULL);
	endpoint = drv_usb_interface_find_endpoint(storage, DRV_USB_TRANSFER_BULK,
	    DRV_USB_DIR_IN, NULL);
	CHECK(endpoint != NULL);
	urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(urb != NULL);
	CHECK(drv_usb_urb_setup(urb, buffer, length, 0, 0, NULL, NULL) == 0);
	return urb;
}

static void
exercise_quarantine_submit_barrier(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct submit_thread_fixture submitting = { 0 };
	struct quarantine_thread_fixture quarantining = {
		.controller = controller, .device = device
	};
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *interface;
	struct drv_usb_urb *urb, *blocked_urb;
	thrd_t submit_worker, quarantine_worker;
	uint8_t buffer[32], blocked_buffer[32];
	unsigned before, completion_sequence;

	urb = allocate_storage_input(device, buffer, sizeof(buffer));
	blocked_urb = allocate_storage_input(device, blocked_buffer,
	    sizeof(blocked_buffer));
	atomic_store_explicit(&controller->pause_enqueue_address, 0x81U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_enqueue_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_release, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_complete, 1U,
	    memory_order_release);
	submitting.urb = urb;
	CHECK(thrd_create(&submit_worker, submit_thread, &submitting) ==
	    thrd_success);
	wait_for_nonzero(&controller->pause_enqueue_entered);
	CHECK(thrd_create(&quarantine_worker, quarantine_thread, &quarantining) ==
	    thrd_success);
	wait_for_nonzero(&quarantining.started);
	wait_for_nonzero(&quarantining.done);
	CHECK(atomic_load_explicit(&quarantining.done,
	    memory_order_acquire) == 1U);
	CHECK(atomic_load_explicit(&controller->completion_sequence,
	    memory_order_acquire) == 0U);
	CHECK(atomic_load_explicit(&controller->first_device_quiesce_sequence,
	    memory_order_acquire) == 0U);
	before = atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed);
	CHECK(drv_usb_urb_submit(blocked_urb) == ENODEV);
	CHECK(atomic_load_explicit(&controller->data_enqueue_count,
	    memory_order_relaxed) == before);
	atomic_store_explicit(&controller->pause_enqueue_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(submit_worker, NULL) == thrd_success);
	CHECK(thrd_join(quarantine_worker, NULL) == thrd_success);
	CHECK(submitting.result == 0);
	completion_sequence = atomic_load_explicit(&controller->completion_sequence,
	    memory_order_acquire);
	/* Quarantine closes future admission immediately but deliberately does not
	 * synchronously drain an accepted commit: selection/control error paths may
	 * hold locks needed by a synchronous HCD callback.  HCD ownership retains
	 * the accepted graph until this later completion. */
	CHECK(completion_sequence != 0U &&
	    quarantining.done_sequence < completion_sequence);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	CHECK(device_is_quarantined(device));
	configuration = drv_usb_device_active_configuration(device);
	interface = drv_usb_configuration_find_interface(configuration, 0U);
	CHECK(interface != NULL);
	CHECK(drv_usb_interface_active_alternate(interface) == NULL);
	CHECK(drv_usb_urb_setup(urb, buffer, sizeof(buffer), 0, 0,
	    NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(urb) == ENODEV);
	drv_usb_urb_free(blocked_urb);
	drv_usb_urb_free(urb);
}

static void
exercise_disconnect_submit_barrier(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct submit_thread_fixture submitting = { 0 };
	struct disconnect_thread_fixture disconnecting = {
		.controller = controller
	};
	struct drv_usb_urb *urb;
	thrd_t submit_worker, disconnect_worker;
	uint8_t buffer[32];
	unsigned completion_sequence, quiesce_sequence;

	urb = allocate_storage_input(device, buffer, sizeof(buffer));
	atomic_store_explicit(&controller->pause_enqueue_address, 0x81U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_enqueue_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_release, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_enqueue_complete, 1U,
	    memory_order_release);
	submitting.urb = urb;
	CHECK(thrd_create(&submit_worker, submit_thread, &submitting) ==
	    thrd_success);
	wait_for_nonzero(&controller->pause_enqueue_entered);
	CHECK(thrd_create(&disconnect_worker, disconnect_thread,
	    &disconnecting) == thrd_success);
	wait_for_nonzero(&disconnecting.started);
	wait_for_disconnect(device);
	CHECK(atomic_load_explicit(&disconnecting.done,
	    memory_order_acquire) == 0U);
	CHECK(atomic_load_explicit(&controller->first_device_quiesce_sequence,
	    memory_order_acquire) == 0U);
	atomic_store_explicit(&controller->pause_enqueue_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(submit_worker, NULL) == thrd_success);
	CHECK(thrd_join(disconnect_worker, NULL) == thrd_success);
	CHECK(submitting.result == 0 && submitting.urb == urb);
	completion_sequence = atomic_load_explicit(&controller->completion_sequence,
	    memory_order_acquire);
	quiesce_sequence = atomic_load_explicit(
	    &controller->first_device_quiesce_sequence, memory_order_acquire);
	CHECK(completion_sequence != 0U && quiesce_sequence != 0U &&
	    completion_sequence < quiesce_sequence);
	/* The caller's allocated URB deliberately retains the software graph after
	 * checked HCD quiesce.  Its final put makes the ordinary retry releasable. */
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == device);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	drv_usb_urb_free(urb);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
}

static void
exercise_disconnect_selection_barrier(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *stream;
	struct alternate_fixture switching = { 0 };
	struct disconnect_thread_fixture disconnecting = {
		.controller = controller
	};
	thrd_t switching_worker, disconnect_worker;
	unsigned disable_sequence, quiesce_sequence;

	configuration = drv_usb_device_active_configuration(device);
	stream = drv_usb_configuration_find_interface(configuration, 1U);
	CHECK(stream != NULL);
	CHECK(drv_usb_interface_set_alternate(stream, 1U) == 0);
	atomic_store_explicit(&controller->event_sequence, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->endpoint_disable_sequence, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->first_device_quiesce_sequence, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_disable_address, 0x83U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_disable_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_disable_release, 0U,
	    memory_order_relaxed);
	switching.interface = stream;
	switching.alternate = 0U;
	CHECK(thrd_create(&switching_worker, set_alternate_thread, &switching) ==
	    thrd_success);
	wait_for_nonzero(&controller->pause_disable_entered);
	CHECK(thrd_create(&disconnect_worker, disconnect_thread,
	    &disconnecting) == thrd_success);
	wait_for_nonzero(&disconnecting.started);
	wait_for_disconnect_publication(device);
	CHECK(atomic_load_explicit(&disconnecting.done,
	    memory_order_acquire) == 0U);
	CHECK(atomic_load_explicit(&controller->first_device_quiesce_sequence,
	    memory_order_acquire) == 0U);
	CHECK(atomic_load_acquire(&device->selection_gate) != 0U);
	atomic_store_explicit(&controller->pause_disable_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(switching_worker, NULL) == thrd_success);
	CHECK(thrd_join(disconnect_worker, NULL) == thrd_success);
	/* Disconnect may make the internal control allocation fail before submit
	 * or reject its submit directly.  The contract here is that the already
	 * entered selection finishes (unsuccessfully) before HCD quiesce. */
	CHECK(switching.result != 0);
	disable_sequence = atomic_load_explicit(
	    &controller->endpoint_disable_sequence, memory_order_acquire);
	quiesce_sequence = atomic_load_explicit(
	    &controller->first_device_quiesce_sequence, memory_order_acquire);
	CHECK(disable_sequence != 0U && quiesce_sequence != 0U &&
	    disable_sequence < quiesce_sequence);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
}

static void
reset_binding_state(enum binding_attach_mode mode)
{
	memset(&binding_driver_state, 0, sizeof(binding_driver_state));
	binding_driver_state.mode = mode;
}

static void
exercise_bound_disconnect_cleanup(struct fake_controller *controller)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *primary, *claimed;
	struct drv_usb_device *device;
	unsigned detach_count;

	reset_binding_state(BINDING_ATTACH_SUCCESS);
	binding_driver_state.keep_persistent_urb = 1U;
	CHECK(drv_usb_driver_register(&binding_driver) == 0);
	register_controller(controller);
	device = drv_usb_find_device(controller->bus_number, 1U);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	primary = drv_usb_configuration_find_interface(configuration, 2U);
	claimed = drv_usb_configuration_find_interface(configuration, 3U);
	CHECK(primary != NULL && claimed != NULL);
	CHECK(binding_driver_state.attach_count == 1U);
	CHECK(drv_usb_interface_driver(primary) == &binding_driver);
	CHECK(drv_usb_interface_driver_data(primary) ==
	    &binding_driver_state.token);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_BOUND);
	CHECK(drv_usb_interface_claimed_by(claimed) == primary);
	CHECK(binding_driver_state.persistent_urb != NULL);
	controller->expected_detached_primary = primary;
	controller->expected_detached_claimed = claimed;

	/* Physical removal owns the private forced-detach path after the public
	 * binding gate is permanently closed.  HCD quiesce observes that binding
	 * state and the sibling claim already disappeared. */
	atomic_store_explicit(&controller->connected, 0U, memory_order_release);
	atomic_store_explicit(&controller->connection_changed, 1U,
	    memory_order_release);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
	CHECK(binding_driver_state.detach_count == 1U);
	CHECK(binding_driver_state.forced_count == 1U);
	CHECK(binding_driver_state.attach_failed_count == 0U);
	CHECK(binding_driver_state.persistent_urb == NULL);
	CHECK(binding_driver_state.persistent_urb_freed == 1U);
	CHECK(atomic_load_explicit(&controller->detached_binding_observed,
	    memory_order_relaxed) == 1U);

	/* Repeated disconnect notification has no device/binding to retain and
	 * must not invoke the callback or free its URB a second time. */
	detach_count = binding_driver_state.detach_count;
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
	CHECK(binding_driver_state.detach_count == detach_count);
	CHECK(binding_driver_state.persistent_urb_freed == 1U);
	CHECK(atomic_load_explicit(&controller->detached_binding_observed,
	    memory_order_relaxed) == 1U);
	unregister_controller(controller);
	CHECK(binding_driver_state.detach_count == detach_count);
	CHECK(drv_usb_driver_unregister(&binding_driver) == 0);
}

static void
exercise_binding_lifecycle(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *primary, *claimed;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *pending_urb;
	struct alternate_fixture claimed_switch = { 0 };
	struct detach_thread_fixture detaching = { 0 };
	thrd_t claimed_switch_worker, detach_worker;
	uint8_t byte = 0;
	unsigned detach_count;

	configuration = drv_usb_device_active_configuration(device);
	primary = drv_usb_configuration_find_interface(configuration, 2U);
	claimed = drv_usb_configuration_find_interface(configuration, 3U);
	CHECK(primary != NULL && claimed != NULL);
	endpoint = drv_usb_interface_endpoint(claimed, 0U);
	CHECK(endpoint != NULL);
	CHECK(drv_usb_driver_register(&binding_driver) == 0);

	/* An unbound transfer which wins target admission prevents probe from
	 * publishing its sibling claim.  Attach abort observes one clean
	 * provisional owner and leaves the target wholly unclaimed. */
	pending_urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(pending_urb != NULL);
	CHECK(drv_usb_urb_setup(pending_urb, &byte, sizeof(byte), 0, 0,
	    NULL, NULL) == 0);
	atomic_store_explicit(&controller->hold_data, 1U, memory_order_release);
	CHECK(drv_usb_urb_submit(pending_urb) == 0);
	reset_binding_state(BINDING_ATTACH_CLAIM_BUSY);
	CHECK(drv_usb_interface_probe(primary) == EBUSY);
	CHECK(binding_driver_state.attach_count == 1U);
	CHECK(binding_driver_state.detach_count == 1U);
	CHECK(binding_driver_state.attach_failed_count == 1U);
	CHECK(drv_usb_interface_driver(primary) == NULL);
	CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_DEAD);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(pending_urb, 0) == 0);
	drv_usb_urb_free(pending_urb);
	atomic_store_explicit(&controller->hold_data, 0U, memory_order_release);

	/* A failed attach uses detach while provisional ownership is visible. */
	reset_binding_state(BINDING_ATTACH_ABORT_CLEAN);
	CHECK(drv_usb_interface_probe(primary) == EIO);
	CHECK(binding_driver_state.attach_count == 1U);
	CHECK(binding_driver_state.detach_count == 1U);
	CHECK(binding_driver_state.attach_failed_count == 1U);
	CHECK(drv_usb_interface_driver(primary) == NULL);
	CHECK(drv_usb_interface_driver_data(primary) == NULL);
	CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_DEAD);

	/* Failed abort cleanup retains one closed binding for an exact retry. */
	pending_urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(pending_urb != NULL);
	CHECK(drv_usb_urb_setup(pending_urb, &byte, sizeof(byte), 0, 0,
	    NULL, NULL) == 0);
	reset_binding_state(BINDING_ATTACH_ABORT_RETAIN);
	CHECK(drv_usb_interface_probe(primary) == EIO);
	CHECK(binding_driver_state.attach_count == 1U);
	CHECK(binding_driver_state.detach_count == 1U);
	CHECK(binding_driver_state.attach_failed_count == 1U);
	CHECK(drv_usb_interface_driver(primary) == &binding_driver);
	CHECK(drv_usb_interface_driver_data(primary) ==
	    &binding_driver_state.token);
	CHECK(drv_usb_interface_claimed_by(claimed) == primary);
	CHECK(atomic_load_acquire(&primary->binding_state) ==
	    USB_BINDING_DETACH_PENDING);
	CHECK(drv_usb_interface_probe(primary) == EBUSY);
	CHECK(drv_usb_urb_submit(pending_urb) == ENODEV);
	binding_driver_state.cleanup_may_finish = 1U;
	CHECK(drv_usb_interface_detach(primary, DRV_USB_DETACH_FORCE) == 0);
	CHECK(binding_driver_state.detach_count == 2U);
	CHECK(binding_driver_state.forced_count == 1U);
	CHECK(drv_usb_interface_driver(primary) == NULL);
	CHECK(drv_usb_interface_driver_data(primary) == NULL);
	CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_DEAD);
	detach_count = binding_driver_state.detach_count;
	CHECK(drv_usb_interface_detach(primary, DRV_USB_DETACH_FORCE) == EINVAL);
	CHECK(binding_driver_state.detach_count == detach_count);
	drv_usb_urb_free(pending_urb);

	/* Detach cleanup may not clear a claim by opening a sibling gate which an
	 * alternate transaction already owns.  The callback finishes, clear waits,
	 * SET_INTERFACE publishes, and only then does the claim become DEAD. */
	reset_binding_state(BINDING_ATTACH_SUCCESS);
	CHECK(drv_usb_interface_probe(primary) == 0);
	CHECK(drv_usb_interface_claimed_by(claimed) == primary);
	atomic_store_explicit(&controller->pause_disable_address, 0x86U,
	    memory_order_release);
	atomic_store_explicit(&controller->pause_disable_entered, 0U,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->pause_disable_release, 0U,
	    memory_order_relaxed);
	claimed_switch.interface = claimed;
	claimed_switch.alternate = 1U;
	CHECK(thrd_create(&claimed_switch_worker, set_alternate_thread,
	    &claimed_switch) == thrd_success);
	wait_for_nonzero(&controller->pause_disable_entered);
	detaching.interface = primary;
	CHECK(thrd_create(&detach_worker, detach_thread, &detaching) ==
	    thrd_success);
	wait_for_nonzero(&binding_driver_state.detach_entered);
	CHECK(atomic_load_explicit(&detaching.done, memory_order_acquire) == 0U);
	CHECK(atomic_load_explicit(&claimed_switch.done,
	    memory_order_acquire) == 0U);
	CHECK(atomic_load_acquire(&primary->binding_state) ==
	    USB_BINDING_UNBINDING);
	CHECK(drv_usb_interface_claimed_by(claimed) == primary);
	atomic_store_explicit(&controller->pause_disable_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(claimed_switch_worker, NULL) == thrd_success);
	CHECK(thrd_join(detach_worker, NULL) == thrd_success);
	CHECK(claimed_switch.result == 0 && detaching.result == 0);
	CHECK(drv_usb_interface_descriptor(claimed)->alternate_setting == 1U);
	CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_DEAD);
	CHECK((atomic_load_acquire(&claimed->io_gate) & USB_IO_GATE_COUNT_MASK) ==
	    0U);
	atomic_store_explicit(&controller->pause_disable_address, 0U,
	    memory_order_release);
	CHECK(drv_usb_interface_set_alternate(claimed, 0U) == 0);

	/* Normal detach failure has the same retained, non-submit-capable retry. */
	reset_binding_state(BINDING_ATTACH_SUCCESS);
	CHECK(drv_usb_interface_probe(primary) == 0);
	CHECK(drv_usb_interface_driver(primary) == &binding_driver);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_BOUND);
	pending_urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(pending_urb != NULL);
	CHECK(drv_usb_urb_setup(pending_urb, &byte, sizeof(byte), 0, 0,
	    NULL, NULL) == 0);
	binding_driver_state.fail_normal_detach = 1U;
	CHECK(drv_usb_interface_detach(primary, 0) == EBUSY);
	CHECK(drv_usb_interface_driver(primary) == &binding_driver);
	CHECK(drv_usb_interface_claimed_by(claimed) == primary);
	CHECK(atomic_load_acquire(&primary->binding_state) ==
	    USB_BINDING_DETACH_PENDING);
	CHECK(drv_usb_urb_submit(pending_urb) == ENODEV);
	CHECK(drv_usb_interface_detach(primary, DRV_USB_DETACH_FORCE) == 0);
	CHECK(drv_usb_interface_driver(primary) == NULL);
	CHECK(drv_usb_interface_claimed_by(claimed) == NULL);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_DEAD);
	drv_usb_urb_free(pending_urb);

	CHECK(drv_usb_driver_unregister(&binding_driver) == 0);
}

static void
exercise_ep0_serialization(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct control_thread_fixture timed = {
		.device = device, .request = 0x55U, .timeout_ms = 1U
	};
	struct control_thread_fixture follower = {
		.device = device, .request = 0x56U, .timeout_ms = 0
	};
	thrd_t timed_worker, follower_worker;
	unsigned base;

	base = atomic_load_explicit(&controller->control_enqueue_count,
	    memory_order_relaxed);
	atomic_store_explicit(&controller->hold_custom_control, 1U,
	    memory_order_release);
	atomic_store_explicit(&controller->dequeue_busy, 1U,
	    memory_order_release);
	CHECK(thrd_create(&timed_worker, control_thread, &timed) == thrd_success);
	CHECK(wait_for_control(controller, base) != NULL);
	wait_for_nonzero(&controller->dequeue_count);
	settle_threads();
	CHECK(atomic_load_explicit(&timed.done, memory_order_acquire) == 0U);

	/* The next EP0 caller stays outside the HCD until the timed-out owner
	 * has terminal status, no callback, and no HCD ownership. */
	CHECK(thrd_create(&follower_worker, control_thread, &follower) ==
	    thrd_success);
	wait_for_nonzero(&follower.started);
	settle_threads();
	CHECK(atomic_load_explicit(&controller->control_enqueue_count,
	    memory_order_acquire) == base + 1U);
	CHECK(atomic_load_explicit(&controller->control_max_active,
	    memory_order_acquire) == 1U);
	complete_control(controller, base);
	CHECK(wait_for_control(controller, base + 1U) != NULL);
	CHECK(atomic_load_explicit(&controller->control_max_active,
	    memory_order_acquire) == 1U);
	complete_control(controller, base + 1U);
	CHECK(thrd_join(timed_worker, NULL) == thrd_success);
	CHECK(thrd_join(follower_worker, NULL) == thrd_success);
	CHECK(timed.result == ETIMEDOUT);
	CHECK(follower.result == 0);
	CHECK(atomic_load_explicit(&controller->control_active,
	    memory_order_acquire) == 0U);
	atomic_store_explicit(&controller->dequeue_busy, 0U,
	    memory_order_release);
	atomic_store_explicit(&controller->hold_custom_control, 0U,
	    memory_order_release);
}

int
main(void)
{
	struct fake_controller first_controller, second_controller;
	struct fake_controller quarantine_controller, disconnect_controller;
	struct fake_controller selection_disconnect_controller;
	struct fake_controller bound_disconnect_controller;
	struct drv_usb_device *first, *second, *barrier_device;

	CHECK(drv_usb_init() == 0);
	register_controller(&first_controller);
	register_controller(&second_controller);
	first = drv_usb_find_device(first_controller.bus_number, 1U);
	second = drv_usb_find_device(second_controller.bus_number, 1U);
	CHECK(first != NULL && second != NULL && first != second);

	exercise_interface_transactions(&first_controller, first);
	exercise_public_getter_snapshot_race(first);
	exercise_hcd_dequeue_ownership(&first_controller, first);
	exercise_target_configuration_race(&second_controller, second);
	exercise_cross_device_rejection(first, second);

	register_controller(&quarantine_controller);
	barrier_device = drv_usb_find_device(quarantine_controller.bus_number, 1U);
	CHECK(barrier_device != NULL);
	exercise_quarantine_submit_barrier(&quarantine_controller, barrier_device);
	CHECK(atomic_load_explicit(&quarantine_controller.invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	unregister_controller(&quarantine_controller);

	register_controller(&disconnect_controller);
	barrier_device = drv_usb_find_device(disconnect_controller.bus_number, 1U);
	CHECK(barrier_device != NULL);
	exercise_disconnect_submit_barrier(&disconnect_controller, barrier_device);
	CHECK(atomic_load_explicit(&disconnect_controller.invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	unregister_controller(&disconnect_controller);

	register_controller(&selection_disconnect_controller);
	barrier_device = drv_usb_find_device(
	    selection_disconnect_controller.bus_number, 1U);
	CHECK(barrier_device != NULL);
	exercise_disconnect_selection_barrier(&selection_disconnect_controller,
	    barrier_device);
	CHECK(atomic_load_explicit(
	    &selection_disconnect_controller.invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	unregister_controller(&selection_disconnect_controller);

	exercise_bound_disconnect_cleanup(&bound_disconnect_controller);

	exercise_binding_lifecycle(&first_controller, first);
	exercise_ep0_serialization(&first_controller, first);

	CHECK(atomic_load_explicit(&first_controller.invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	CHECK(atomic_load_explicit(&second_controller.invalid_data_enqueue,
	    memory_order_relaxed) == 0U);
	unregister_controller(&second_controller);
	unregister_controller(&first_controller);
	CHECK(atomic_load_explicit(&live_allocations,
	    memory_order_relaxed) == 0U);

	printf("usb binding transactions: %u checks passed\n",
	    atomic_load_explicit(&checks, memory_order_relaxed));
	return 0;
}
