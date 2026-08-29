/*
 * WS004 HW-T14 production USB-core fixture.
 *
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

static unsigned checks;
static size_t allocation_attempts;
static size_t allocation_fail_at;
static size_t live_allocations;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, \
		    __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

void *
hal_malloc(size_t size)
{
	void *pointer;

	allocation_attempts++;
	if (allocation_fail_at != 0 &&
	    allocation_attempts == allocation_fail_at)
		return NULL;
	pointer = malloc(size);
	if (pointer != NULL)
		live_allocations++;
	return pointer;
}

void
hal_free(void *pointer)
{
	if (pointer == NULL)
		return;
	CHECK(live_allocations != 0);
	live_allocations--;
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
	static uint64_t ticks;

	return ++ticks;
}

void
sched_yield(void)
{
}

static const uint8_t device_descriptor[] = {
	18, 1, 0x00, 0x02, 0xef, 2, 1, 64,
	0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 5, 2
};

static const uint8_t storage_configuration[] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 8, 6, 0x50, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0
};

static const uint8_t unsupported_vendor_configuration[] = {
	9, 2, 48, 0, 2, 1, 0, 0x80, 50,
	9, 4, 0, 0, 1, 0xff, 0, 0, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	9, 4, 2, 0, 2, 0xff, 0, 0, 0,
	7, 5, 0x86, 2, 64, 0, 0,
	7, 5, 0x07, 2, 64, 0, 0
};

static const uint8_t tied_first_configuration[] = {
	9, 2, 18, 0, 1, 1, 0, 0x80, 50,
	9, 4, 7, 0, 0, 2, 0x0d, 0, 0
};

static const uint8_t sparse_interface_configuration[] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 50,
	9, 4, 200, 0, 2, 8, 6, 0x50, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0
};

static const uint8_t ncm_configuration[] = {
	9, 2, 88, 0, 2, 2, 0, 0x80, 50,
	8, 11, 0, 2, 2, 0x0d, 0, 0,
	9, 4, 0, 0, 1, 2, 0x0d, 0, 0,
	5, 0x24, 0, 0x10, 0x01,
	5, 0x24, 6, 0, 1,
	13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,
	7, 5, 0x83, 3, 16, 0, 9,
	9, 4, 1, 0, 0, 0x0a, 0, 1, 0,
	9, 4, 1, 1, 2, 0x0a, 0, 1, 0,
	7, 5, 0x84, 2, 0x00, 0x02, 0,
	7, 5, 0x05, 2, 0x00, 0x02, 0
};

static const uint8_t composite_teardown_configuration[] = {
	9, 2, 111, 0, 3, 2, 0, 0x80, 50,
	8, 11, 0, 2, 2, 0x0d, 0, 0,
	9, 4, 0, 0, 1, 2, 0x0d, 0, 0,
	5, 0x24, 0, 0x10, 0x01,
	5, 0x24, 6, 0, 1,
	13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,
	7, 5, 0x83, 3, 16, 0, 9,
	9, 4, 1, 0, 0, 0x0a, 0, 1, 0,
	9, 4, 1, 1, 2, 0x0a, 0, 1, 0,
	7, 5, 0x84, 2, 0x00, 0x02, 0,
	7, 5, 0x05, 2, 0x00, 0x02, 0,
	9, 4, 2, 0, 2, 8, 6, 0x50, 0,
	7, 5, 0x86, 2, 64, 0, 0,
	7, 5, 0x07, 2, 64, 0, 0
};

static const uint8_t malformed_configuration[] = {
	9, 2, 27, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 0, 2, 0x0d, 0, 0,
	9, 4, 0, 0, 0, 2, 0x0d, 0, 0
};

static const uint8_t missing_endpoint_configuration[] = {
	9, 2, 18, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 1, 2, 0x0d, 0, 0
};

static const uint8_t aliased_endpoint_configuration[] = {
	9, 2, 41, 0, 2, 1, 0, 0x80, 50,
	9, 4, 0, 0, 1, 2, 0x0d, 0, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	9, 4, 1, 0, 1, 0x0a, 0, 1, 0,
	7, 5, 0x81, 2, 64, 0, 0
};

static const uint8_t language_string[] = { 4, 3, 0x09, 0x04 };
static const uint8_t mac_string[] = {
	26, 3, '0', 0, '0', 0, '1', 0, '1', 0, '2', 0, '2', 0,
	'A', 0, 'A', 0, 'B', 0, 'B', 0, 'C', 0, 'C', 0
};
static const uint8_t malformed_string[] = { 4, 3, 0x00, 0xd8 };

struct fake_controller {
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	unsigned bus_number;
	unsigned connected;
	unsigned malformed;
	unsigned vendor_first;
	unsigned tie_first;
	unsigned teardown_composite;
	unsigned configuration;
	unsigned alternate[DRV_USB_MAX_INTERFACES];
	unsigned endpoint_enabled[256];
	unsigned endpoint_enable_count;
	unsigned endpoint_disable_count;
	unsigned invalid_endpoint_operations;
	unsigned set_interface_count;
	unsigned last_set_interface_number;
	unsigned last_set_interface_value;
	unsigned fail_set_interface;
	unsigned fail_set_interface_io;
	unsigned fail_set_configuration;
	unsigned fail_set_configuration_io;
	unsigned fail_enqueue;
	unsigned hold_async;
	struct drv_usb_urb *held_async_urb;
	unsigned fail_enable_address;
	unsigned fail_disable_address;
	unsigned expected_published_alternate;
	struct drv_usb_interface *observed_interface;
	unsigned teardown_tracking;
	unsigned teardown_sequence;
	unsigned teardown_detach_sequence;
	unsigned teardown_later_detach_sequence;
	unsigned teardown_device_quiesce_sequence;
	unsigned teardown_device_disable_sequence;
	unsigned teardown_hcd_quiesce_sequence;
	unsigned teardown_stop_sequence;
	int teardown_detach_error;
	int teardown_device_quiesce_error;
};

static struct fake_controller *
fake_controller(struct drv_usb_hcd *hcd)
{
	return (struct fake_controller *)hcd->private_data[0];
}

static int
fake_start(struct drv_usb_hcd *hcd)
{
	(void)hcd;
	return 0;
}

static void
fake_stop(struct drv_usb_hcd *hcd)
{
	struct fake_controller *controller = fake_controller(hcd);

	if (controller->teardown_tracking)
		controller->teardown_stop_sequence =
		    ++controller->teardown_sequence;
}

static int
fake_quiesce(struct drv_usb_hcd *hcd)
{
	struct fake_controller *controller = fake_controller(hcd);

	if (controller->teardown_tracking)
		controller->teardown_hcd_quiesce_sequence =
		    ++controller->teardown_sequence;
	return 0;
}

static int
fake_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct fake_controller *controller = fake_controller(hcd);
	const struct drv_usb_control_request *request =
	    drv_usb_urb_control_request(urb);
	const uint8_t *bytes = NULL;
	size_t length = 0;

	CHECK(request != NULL);
	if (controller->fail_enqueue) {
		controller->fail_enqueue = 0;
		return EIO;
	}
	if (controller->hold_async) {
		CHECK(controller->held_async_urb == NULL);
		controller->held_async_urb = urb;
		return 0;
	}
	if (request->request == 6 && (request->value >> 8) == 1) {
		bytes = device_descriptor;
		length = sizeof(device_descriptor);
	} else if (request->request == 6 && (request->value >> 8) == 2) {
		unsigned index = request->value & 0xffU;

		if (controller->malformed == 1) {
			bytes = malformed_configuration;
			length = sizeof(malformed_configuration);
		} else if (controller->malformed == 2) {
			bytes = missing_endpoint_configuration;
			length = sizeof(missing_endpoint_configuration);
		} else if (controller->malformed == 3) {
			bytes = aliased_endpoint_configuration;
			length = sizeof(aliased_endpoint_configuration);
		} else if (controller->malformed == 4 && index == 0) {
			bytes = sparse_interface_configuration;
			length = sizeof(sparse_interface_configuration);
		} else if (controller->vendor_first && index == 0) {
			bytes = unsupported_vendor_configuration;
			length = sizeof(unsupported_vendor_configuration);
		} else if (controller->tie_first && index == 0) {
			bytes = tied_first_configuration;
			length = sizeof(tied_first_configuration);
		} else if (index == 0) {
			bytes = storage_configuration;
			length = sizeof(storage_configuration);
		} else if (index == 1) {
			bytes = controller->teardown_composite ?
			    composite_teardown_configuration : ncm_configuration;
			length = controller->teardown_composite ?
			    sizeof(composite_teardown_configuration) :
			    sizeof(ncm_configuration);
		}
	} else if (request->request == 6 && (request->value >> 8) == 3) {
		switch (request->value & 0xffU) {
		case 0:
			bytes = language_string;
			length = sizeof(language_string);
			break;
		case 3:
			bytes = mac_string;
			length = sizeof(mac_string);
			break;
		case 4:
			bytes = malformed_string;
			length = sizeof(malformed_string);
			break;
		default:
			break;
		}
	} else if (request->request == 5) {
		drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request == 9) {
		if (controller->fail_set_configuration) {
			controller->fail_set_configuration = 0;
			drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_STALL, 0);
			return 0;
		}
		if (controller->fail_set_configuration_io) {
			controller->fail_set_configuration_io = 0;
			drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_IO_ERROR, 0);
			return 0;
		}
		controller->configuration = request->value;
		memset(controller->alternate, 0, sizeof(controller->alternate));
		memset(controller->endpoint_enabled, 0,
		    sizeof(controller->endpoint_enabled));
		drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request == 11) {
		controller->set_interface_count++;
		controller->last_set_interface_number = request->index;
		controller->last_set_interface_value = request->value;
		if (controller->fail_set_interface) {
			controller->fail_set_interface = 0;
			drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_STALL, 0);
			return 0;
		}
		if (controller->fail_set_interface_io) {
			controller->fail_set_interface_io = 0;
			drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_IO_ERROR, 0);
			return 0;
		}
		controller->alternate[request->index] = request->value;
		drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	}
	if (bytes == NULL) {
		drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_STALL, 0);
		return 0;
	}
	if (length > drv_usb_urb_length(urb))
		length = drv_usb_urb_length(urb);
	memcpy(drv_usb_urb_buffer(urb), bytes, length);
	drv_usb_hcd_complete(hcd, urb, DRV_USB_URB_COMPLETE, length);
	return 0;
}

static int
fake_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	(void)hcd;
	(void)urb;
	return 0;
}

static int
fake_device_quiesce(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	struct fake_controller *controller = fake_controller(hcd);

	(void)device;
	if (controller->teardown_tracking)
		controller->teardown_device_quiesce_sequence =
		    ++controller->teardown_sequence;
	return controller->teardown_device_quiesce_error;
}

static void
fake_device_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	struct fake_controller *controller = fake_controller(hcd);

	(void)device;
	if (controller->teardown_tracking)
		controller->teardown_device_disable_sequence =
		    ++controller->teardown_sequence;
}

static int
fake_endpoint_enable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = fake_controller(hcd);
	const struct drv_usb_interface_descriptor *descriptor;
	uint8_t address = drv_usb_endpoint_address(endpoint);

	descriptor = drv_usb_interface_descriptor(controller->observed_interface);
	if (controller->expected_published_alternate != UINT32_MAX &&
	    controller->observed_interface != NULL &&
	    drv_usb_endpoint_device(endpoint) ==
	    drv_usb_interface_device(controller->observed_interface))
		CHECK(descriptor->alternate_setting ==
		    controller->expected_published_alternate);
	controller->endpoint_enable_count++;
	if (controller->endpoint_enabled[address]) {
		controller->invalid_endpoint_operations++;
		return EALREADY;
	}
	if (controller->fail_enable_address == address) {
		controller->fail_enable_address = 0;
		return EIO;
	}
	controller->endpoint_enabled[address] = 1U;
	return 0;
}

static int
fake_endpoint_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = fake_controller(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);

	controller->endpoint_disable_count++;
	if (!controller->endpoint_enabled[address]) {
		controller->invalid_endpoint_operations++;
		return EALREADY;
	}
	if (controller->fail_disable_address == address) {
		controller->fail_disable_address = 0;
		return EIO;
	}
	controller->endpoint_enabled[address] = 0;
	return 0;
}

static int
fake_root_control(struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request, void *buffer,
	size_t length, size_t *actual)
{
	struct fake_controller *controller = fake_controller(hcd);
	uint32_t status;

	if (request->request == 0 && request->request_type == 0xa3U) {
		CHECK(buffer != NULL && length >= sizeof(status));
		status = controller->connected ? 1U | 0x400U | (1U << 16) :
		    (1U << 16);
		memcpy(buffer, &status, sizeof(status));
		*actual = sizeof(status);
		return 0;
	}
	*actual = 0;
	return 0;
}

static int
fake_root_reset(struct drv_usb_hcd *hcd, unsigned port)
{
	(void)hcd;
	return port == 1 ? 0 : EINVAL;
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
	.root_hub_control = fake_root_control,
	.root_port_reset = fake_root_reset
};

static void
fake_register_capabilities(struct fake_controller *controller,
	unsigned malformed, unsigned capabilities)
{
	memset(controller, 0, sizeof(*controller));
	controller->malformed = malformed;
	controller->connected = 1;
	controller->expected_published_alternate = UINT32_MAX;
	controller->hcd.name = "fixture";
	controller->hcd.ops = &fake_ops;
	controller->hcd.root_port_count = 1;
	controller->hcd.capabilities = capabilities;
	controller->hcd.private_data[0] = (uintptr_t)controller;
	CHECK(drv_usb_hcd_register(&controller->hcd, &controller->bus) == 0);
	controller->bus_number = drv_usb_bus_number(controller->bus);
}

static void
fake_register(struct fake_controller *controller, unsigned malformed)
{
	fake_register_capabilities(controller, malformed, 0);
}

static void
fake_unregister(struct fake_controller *controller)
{
	controller->connected = 0;
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(controller->invalid_endpoint_operations == 0);
	CHECK(drv_usb_hcd_unregister(&controller->hcd) == 0);
}

enum fake_attach_mode {
	FAKE_ATTACH_SELECT_ONLY,
	FAKE_ATTACH_CLAIM_FAIL,
	FAKE_ATTACH_CLAIM_SUCCESS
};

static struct {
	enum fake_attach_mode mode;
	int detach_error;
	unsigned match_count;
	unsigned attach_count;
	unsigned detach_count;
	struct drv_usb_interface *owner;
	struct drv_usb_interface *target;
} fake_driver_state;

static int
fake_driver_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	fake_driver_state.match_count++;
	return 20;
}

static int
fake_driver_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *target;
	int error;

	(void)id;
	fake_driver_state.attach_count++;
	fake_driver_state.owner = interface;
	configuration = drv_usb_device_active_configuration(
	    drv_usb_interface_device(interface));
	target = drv_usb_configuration_find_interface(configuration, 1);
	fake_driver_state.target = target;
	if (fake_driver_state.mode == FAKE_ATTACH_SELECT_ONLY)
		return ENOTSUP;
	if (target == NULL)
		return ENODEV;
	error = drv_usb_interface_claim(interface, target);
	if (error != 0)
		return error;
	return fake_driver_state.mode == FAKE_ATTACH_CLAIM_FAIL ? EIO : 0;
}

static int
fake_driver_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct fake_controller *controller;
	struct drv_usb_device *device;

	(void)flags;
	fake_driver_state.detach_count++;
	device = drv_usb_interface_device(interface);
	controller = fake_controller(drv_usb_bus_hcd(
	    drv_usb_device_bus(device)));
	if (controller->teardown_tracking) {
		controller->teardown_detach_sequence =
		    ++controller->teardown_sequence;
		return controller->teardown_detach_error;
	}
	return fake_driver_state.detach_error;
}

static const struct drv_usb_id fake_driver_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
	.interface_class = 2,
	.interface_subclass = 0x0d
}};

static struct drv_usb_driver fake_driver = {
	.name = "fixture-ncm",
	.ids = fake_driver_ids,
	.id_count = sizeof(fake_driver_ids) / sizeof(fake_driver_ids[0]),
	.match = fake_driver_match,
	.attach = fake_driver_attach,
	.detach = fake_driver_detach
};

static int
fake_storage_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	return 20;
}

static int
fake_storage_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	return 0;
}

static int
fake_storage_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct fake_controller *controller;
	struct drv_usb_device *device;

	(void)flags;
	device = drv_usb_interface_device(interface);
	controller = fake_controller(drv_usb_bus_hcd(
	    drv_usb_device_bus(device)));
	if (controller->teardown_tracking)
		controller->teardown_later_detach_sequence =
		    ++controller->teardown_sequence;
	return 0;
}

static const struct drv_usb_id fake_storage_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS,
	.interface_class = 8,
	.interface_subclass = 6
}};

static struct drv_usb_driver fake_storage_driver = {
	.name = "fixture-storage",
	.ids = fake_storage_ids,
	.id_count = sizeof(fake_storage_ids) / sizeof(fake_storage_ids[0]),
	.match = fake_storage_match,
	.attach = fake_storage_attach,
	.detach = fake_storage_detach
};

struct drain_fixture {
	struct drv_usb_hcd *hcd;
	struct drv_usb_urb *urb;
	atomic_uint callback_entered;
	atomic_uint callback_release;
	atomic_uint callback_returned;
	atomic_uint drain_started;
	atomic_uint drain_done;
	int drain_result;
};

static void
blocking_callback(struct drv_usb_urb *urb, void *argument)
{
	struct drain_fixture *fixture = argument;

	(void)urb;
	atomic_store_explicit(&fixture->callback_entered, 1U,
	    memory_order_release);
	while (atomic_load_explicit(&fixture->callback_release,
	    memory_order_acquire) == 0)
		thrd_yield();
	atomic_store_explicit(&fixture->callback_returned, 1U,
	    memory_order_release);
}

static int
completion_thread(void *argument)
{
	struct drain_fixture *fixture = argument;

	drv_usb_hcd_complete(fixture->hcd, fixture->urb,
	    DRV_USB_URB_COMPLETE, 0);
	return 0;
}

static int
drain_thread(void *argument)
{
	struct drain_fixture *fixture = argument;

	atomic_store_explicit(&fixture->drain_started, 1U,
	    memory_order_release);
	fixture->drain_result = drv_usb_urb_drain(fixture->urb, 0);
	atomic_store_explicit(&fixture->drain_done, 1U,
	    memory_order_release);
	return 0;
}

int
main(void)
{
	struct fake_controller controller, malformed, missing_endpoint;
	struct fake_controller aliased_endpoint, sparse_interface;
	struct fake_controller interface_io, claim_controller, fault_controller;
	struct fake_controller tie_controller, no_driver_controller;
	struct fake_controller hotplug_success, hotplug_detach_failure;
	struct fake_controller shutdown_success, shutdown_detach_failure;
	struct fake_controller shutdown_device_failure;
	struct fake_controller capability_controller;
	struct drv_usb_hcd invalid_hcd;
	struct drv_usb_hcd_ops invalid_ops;
	struct drv_usb_bus *invalid_bus = NULL;
	struct drv_usb_device *device;
	struct drv_usb_urb *held_urb;
	struct drv_usb_control_request control_request = { 0 };
	struct drain_fixture drain_fixture;
	thrd_t completion_worker, drain_worker;
	struct timespec settle = { .tv_sec = 0, .tv_nsec = 10000000L };
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *control, *data, *storage;
	const struct drv_usb_host_interface *alternate;
	const struct drv_usb_interface_descriptor *descriptor;
	const struct drv_usb_interface_association_descriptor *iad;
	const void *extra;
	size_t baseline, extra_length, raw_length;
	unsigned fault_nth, fault_success_nth = 0;
	char string[32];

	CHECK(drv_usb_init() == 0);
	memset(&invalid_hcd, 0, sizeof(invalid_hcd));
	invalid_ops = fake_ops;
	invalid_ops.endpoint_disable = NULL;
	invalid_hcd.name = "invalid one-sided endpoint contract";
	invalid_hcd.ops = &invalid_ops;
	CHECK(drv_usb_hcd_register(&invalid_hcd, &invalid_bus) == EINVAL);
	memset(&invalid_hcd, 0, sizeof(invalid_hcd));
	invalid_hcd.name = "invalid unknown capability";
	invalid_hcd.ops = &fake_ops;
	invalid_hcd.root_port_count = 1;
	invalid_hcd.capabilities = 1U << 31;
	CHECK(drv_usb_hcd_register(&invalid_hcd, &invalid_bus) == EINVAL);
	fake_register_capabilities(&capability_controller, 0,
	    DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device = drv_usb_find_device(capability_controller.bus_number, 0);
	CHECK(device != NULL);
	CHECK(drv_usb_device_hcd_capabilities(device) ==
	    DRV_USB_HCD_CAP_CONCURRENT_URBS);
	fake_unregister(&capability_controller);
	CHECK(drv_usb_driver_register(&fake_driver) == 0);
	fake_register(&controller, 0);
	controller.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&controller.hcd);
	device = drv_usb_find_device(controller.bus_number, 1);
	CHECK(device != NULL);
	CHECK(drv_usb_device_hcd_capabilities(device) == 0);
	CHECK(drv_usb_device_configuration_count(device) == 2);
	configuration = drv_usb_device_configuration(device, 1);
	CHECK(configuration != NULL);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(drv_usb_configuration_descriptor(configuration)->
	    configuration_value == 2);
	CHECK(controller.configuration == 2);
	CHECK(fake_driver_state.attach_count == 1);
	CHECK(drv_usb_configuration_raw_descriptors(configuration,
	    &raw_length) != NULL && raw_length == sizeof(ncm_configuration));
	CHECK(drv_usb_configuration_interface_count(configuration) == 2);
	CHECK(drv_usb_configuration_iad_count(configuration) == 1);
	iad = drv_usb_configuration_iad(configuration, 0);
	CHECK(iad != NULL && iad->first_interface == 0 &&
	    iad->interface_count == 2);
	control = drv_usb_configuration_find_interface(configuration, 0);
	data = drv_usb_configuration_find_interface(configuration, 1);
	CHECK(control != NULL && data != NULL);
	CHECK(drv_usb_interface_alternate_count(control) == 1);
	CHECK(drv_usb_interface_alternate_count(data) == 2);
	alternate = drv_usb_interface_alternate(control, 0);
	CHECK(drv_usb_host_interface_extra_count(alternate) == 3);
	CHECK(drv_usb_host_interface_extra(alternate, 1, &extra,
	    &extra_length) == 0);
	CHECK(extra_length == 5 && ((const uint8_t *)extra)[1] == 0x24 &&
	    ((const uint8_t *)extra)[2] == 6 &&
	    ((const uint8_t *)extra)[4] == 1);
	CHECK(drv_usb_device_get_string(device, 3, 0, string,
	    sizeof(string)) == 0);
	CHECK(strcmp(string, "001122AABBCC") == 0);
	CHECK(drv_usb_device_get_string(device, 4, 0, string,
	    sizeof(string)) == EILSEQ);
	CHECK(drv_usb_device_get_string(device, 3, 0, string, 4) == ENOSPC);

	controller.observed_interface = data;
	CHECK(controller.endpoint_enabled[0x83] == 1);
	/* Claims have a lifecycle owner.  Re-probe after the selection-only
	 * candidate abort so this claim is made by a provisional binding. */
	fake_driver_state.mode = FAKE_ATTACH_CLAIM_SUCCESS;
	CHECK(drv_usb_interface_probe(control) == 0);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claim(control, data) == EBUSY);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	CHECK(drv_usb_interface_probe(data) == EBUSY);
	controller.expected_published_alternate = 0;
	CHECK(drv_usb_interface_set_alternate(data, 1) == 0);
	CHECK(controller.last_set_interface_number == 1 &&
	    controller.last_set_interface_value == 1);
	descriptor = drv_usb_interface_descriptor(data);
	CHECK(descriptor->alternate_setting == 1);
	CHECK(drv_usb_interface_endpoint_count(data) == 2);
	CHECK(drv_usb_endpoint_address(drv_usb_interface_endpoint(data, 0)) ==
	    0x84);
	CHECK(controller.endpoint_enabled[0x83] == 1 &&
	    controller.endpoint_enabled[0x84] == 1 &&
	    controller.endpoint_enabled[0x05] == 1);
	held_urb = drv_usb_urb_alloc(device, NULL, 0);
	CHECK(held_urb != NULL);
	CHECK(drv_usb_urb_drain(held_urb, 10U) == 0);
	CHECK(drv_usb_urb_setup_control_flags(held_urb, &control_request,
	    NULL, 0, DRV_USB_URB_RECLAIM_SAFE, 100U, NULL, NULL) == 0);
	CHECK(drv_usb_urb_flags(held_urb) == DRV_USB_URB_RECLAIM_SAFE);
	CHECK(drv_usb_urb_setup_control(held_urb, &control_request, NULL, 0,
	    100U, NULL, NULL) == 0);
	CHECK(drv_usb_urb_flags(held_urb) == 0);
	controller.fail_enqueue = 1U;
	CHECK(drv_usb_urb_submit(held_urb) == EIO);
	CHECK(drv_usb_urb_status(held_urb) == DRV_USB_URB_IDLE);
	CHECK(drv_usb_device_hcd_urb_count(device) == 0);
	CHECK(drv_usb_urb_drain(held_urb, 10U) == 0);

	memset(&drain_fixture, 0, sizeof(drain_fixture));
	drain_fixture.hcd = &controller.hcd;
	drain_fixture.urb = held_urb;
	CHECK(drv_usb_urb_setup_control(held_urb, &control_request, NULL, 0,
	    100U, blocking_callback, &drain_fixture) == 0);
	controller.hold_async = 1U;
	CHECK(drv_usb_urb_submit(held_urb) == 0);
	CHECK(controller.held_async_urb == held_urb);
	CHECK(drv_usb_device_hcd_urb_count(device) == 1U);
	CHECK(thrd_create(&completion_worker, completion_thread,
	    &drain_fixture) == thrd_success);
	while (atomic_load_explicit(&drain_fixture.callback_entered,
	    memory_order_acquire) == 0)
		thrd_yield();
	CHECK(drv_usb_urb_status(held_urb) == DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_device_hcd_urb_count(device) == 1U);
	CHECK(drv_usb_urb_drain(held_urb, 10U) == ETIMEDOUT);
	CHECK(drv_usb_urb_status(held_urb) == DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_device_hcd_urb_count(device) == 1U);
	CHECK(atomic_load_explicit(&drain_fixture.callback_returned,
	    memory_order_acquire) == 0);
	CHECK(thrd_create(&drain_worker, drain_thread, &drain_fixture) ==
	    thrd_success);
	while (atomic_load_explicit(&drain_fixture.drain_started,
	    memory_order_acquire) == 0)
		thrd_yield();
	(void)thrd_sleep(&settle, NULL);
	CHECK(atomic_load_explicit(&drain_fixture.drain_done,
	    memory_order_acquire) == 0);
	atomic_store_explicit(&drain_fixture.callback_release, 1U,
	    memory_order_release);
	CHECK(thrd_join(completion_worker, NULL) == thrd_success);
	CHECK(thrd_join(drain_worker, NULL) == thrd_success);
	CHECK(atomic_load_explicit(&drain_fixture.callback_returned,
	    memory_order_acquire) == 1U);
	CHECK(atomic_load_explicit(&drain_fixture.drain_done,
	    memory_order_acquire) == 1U);
	CHECK(drain_fixture.drain_result == 0);
	CHECK(drv_usb_device_hcd_urb_count(device) == 0);
	CHECK(drv_usb_urb_drain(held_urb, 10U) == 0);
	controller.held_async_urb = NULL;
	controller.hold_async = 0;
	controller.expected_published_alternate = 1;
	CHECK(drv_usb_interface_set_alternate(data, 0) == 0);
	controller.expected_published_alternate = 0;
	CHECK(drv_usb_interface_set_alternate(data, 1) == 0);
	drv_usb_urb_free(held_urb);
	controller.expected_published_alternate = 1;
	controller.fail_disable_address = 0x05;
	CHECK(drv_usb_interface_set_alternate(data, 0) == EIO);
	CHECK(drv_usb_interface_descriptor(data)->alternate_setting == 1);
	CHECK(controller.endpoint_enabled[0x84] == 1 &&
	    controller.endpoint_enabled[0x05] == 1);
	CHECK(drv_usb_interface_set_alternate(data, 0) == 0);
	CHECK(controller.endpoint_enabled[0x84] == 0 &&
	    controller.endpoint_enabled[0x05] == 0);
	controller.expected_published_alternate = 0;
	controller.fail_enable_address = 0x05;
	CHECK(drv_usb_interface_set_alternate(data, 1) == EIO);
	CHECK(drv_usb_interface_descriptor(data)->alternate_setting == 0);
	CHECK(controller.alternate[1] == 0);
	CHECK(controller.endpoint_enabled[0x84] == 0 &&
	    controller.endpoint_enabled[0x05] == 0);
	controller.fail_set_interface = 1;
	CHECK(drv_usb_interface_set_alternate(data, 1) == EPIPE);
	CHECK(drv_usb_interface_descriptor(data)->alternate_setting == 0);
	CHECK(drv_usb_interface_set_alternate(data, 1) == 0);
	CHECK(controller.endpoint_enabled[0x83] == 1 &&
	    controller.endpoint_enabled[0x84] == 1 &&
	    controller.endpoint_enabled[0x05] == 1);
	CHECK(drv_usb_interface_detach(control, 0) == 0);
	CHECK(drv_usb_interface_driver(control) == NULL);
	CHECK(drv_usb_interface_claimed_by(data) == NULL);
	fake_driver_state.mode = FAKE_ATTACH_SELECT_ONLY;
	controller.expected_published_alternate = 1;
	controller.fail_disable_address = 0x84;
	CHECK(drv_usb_device_set_configuration(device, 1) == EIO);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(controller.configuration == 2);
	CHECK(controller.endpoint_enabled[0x83] == 1 &&
	    controller.endpoint_enabled[0x84] == 1 &&
	    controller.endpoint_enabled[0x05] == 1);
	CHECK(drv_usb_interface_set_alternate(data, 0) == 0);
	controller.expected_published_alternate = 0;
	controller.fail_set_configuration = 1;
	CHECK(drv_usb_device_set_configuration(device, 1) == EPIPE);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(controller.configuration == 2);
	CHECK(controller.endpoint_enabled[0x83] == 1);
	controller.fail_enable_address = 0x07;
	CHECK(drv_usb_device_set_configuration(device, 1) == EIO);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(controller.configuration == 2);
	CHECK(controller.endpoint_enabled[0x81] == 0 &&
	    controller.endpoint_enabled[0x86] == 0 &&
	    controller.endpoint_enabled[0x07] == 0 &&
	    controller.endpoint_enabled[0x83] == 1);
	controller.fail_set_configuration_io = 1;
	CHECK(drv_usb_device_set_configuration(device, 1) == EIO);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(controller.configuration == 2);
	CHECK(controller.endpoint_enabled[0x83] == 0);
	CHECK(drv_usb_device_set_configuration(device, 1) == ENODEV);

	fake_unregister(&controller);

	fake_register(&interface_io, 0);
	drv_usb_hcd_root_hub_changed(&interface_io.hcd);
	device = drv_usb_find_device(interface_io.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	data = drv_usb_configuration_find_interface(configuration, 1);
	CHECK(data != NULL);
	interface_io.observed_interface = data;
	interface_io.expected_published_alternate = 0;
	interface_io.fail_set_interface_io = 1;
	CHECK(drv_usb_interface_set_alternate(data, 1) == EIO);
	CHECK(drv_usb_interface_descriptor(data)->alternate_setting == 0);
	CHECK(interface_io.endpoint_enabled[0x84] == 0 &&
	    interface_io.endpoint_enabled[0x05] == 0);
	CHECK(drv_usb_interface_set_alternate(data, 1) == EBUSY);
	fake_unregister(&interface_io);

	fake_register(&tie_controller, 0);
	tie_controller.tie_first = 1U;
	drv_usb_hcd_root_hub_changed(&tie_controller.hcd);
	device = drv_usb_find_device(tie_controller.bus_number, 1);
	CHECK(device != NULL);
	CHECK(drv_usb_configuration_descriptor(
	    drv_usb_device_active_configuration(device))->configuration_value == 1);
	CHECK(tie_controller.configuration == 1);
	fake_unregister(&tie_controller);

	baseline = live_allocations;
	fake_register(&claim_controller, 0);
	drv_usb_hcd_root_hub_changed(&claim_controller.hcd);
	device = drv_usb_find_device(claim_controller.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	data = drv_usb_configuration_find_interface(configuration, 1);
	CHECK(control != NULL && data != NULL);
	fake_driver_state.mode = FAKE_ATTACH_CLAIM_FAIL;
	CHECK(drv_usb_interface_probe(control) == EIO);
	CHECK(drv_usb_interface_driver(control) == NULL);
	CHECK(drv_usb_interface_claimed_by(data) == NULL);
	fake_driver_state.mode = FAKE_ATTACH_CLAIM_SUCCESS;
	CHECK(drv_usb_interface_probe(control) == 0);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	fake_driver_state.detach_error = EBUSY;
	CHECK(drv_usb_interface_detach(control, 0) == EBUSY);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	fake_driver_state.detach_error = 0;
	CHECK(drv_usb_interface_detach(control, 0) == 0);
	CHECK(drv_usb_interface_driver(control) == NULL);
	CHECK(drv_usb_interface_claimed_by(data) == NULL);
	CHECK(drv_usb_interface_probe(control) == 0);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	fake_driver_state.detach_error = EBUSY;
	claim_controller.connected = 0;
	drv_usb_hcd_root_hub_changed(&claim_controller.hcd);
	CHECK(drv_usb_find_device(claim_controller.bus_number, 1) == device);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	fake_driver_state.detach_error = 0;
	drv_usb_hcd_root_hub_changed(&claim_controller.hcd);
	CHECK(drv_usb_find_device(claim_controller.bus_number, 1) == NULL);
	CHECK(drv_usb_hcd_unregister(&claim_controller.hcd) == 0);
	CHECK(live_allocations == baseline);
	fake_driver_state.mode = FAKE_ATTACH_SELECT_ONLY;

	fake_register(&malformed, 1);
	drv_usb_hcd_root_hub_changed(&malformed.hcd);
	CHECK(drv_usb_find_device(malformed.bus_number, 1) == NULL);
	CHECK(drv_usb_hcd_unregister(&malformed.hcd) == 0);
	fake_register(&missing_endpoint, 2);
	drv_usb_hcd_root_hub_changed(&missing_endpoint.hcd);
	CHECK(drv_usb_find_device(missing_endpoint.bus_number, 1) == NULL);
	CHECK(drv_usb_hcd_unregister(&missing_endpoint.hcd) == 0);
	fake_register(&aliased_endpoint, 3);
	drv_usb_hcd_root_hub_changed(&aliased_endpoint.hcd);
	CHECK(drv_usb_find_device(aliased_endpoint.bus_number, 1) == NULL);
	CHECK(drv_usb_hcd_unregister(&aliased_endpoint.hcd) == 0);
	fake_register(&sparse_interface, 4);
	drv_usb_hcd_root_hub_changed(&sparse_interface.hcd);
	device = drv_usb_find_device(sparse_interface.bus_number, 1);
	CHECK(device != NULL);
	CHECK(drv_usb_configuration_find_interface(
	    drv_usb_device_configuration(device, 0), 200) != NULL);
	fake_unregister(&sparse_interface);

	for (fault_nth = 1; fault_nth <= 128U; fault_nth++) {
		baseline = live_allocations;
		fake_register(&fault_controller, 0);
		allocation_fail_at = allocation_attempts + fault_nth;
		drv_usb_hcd_root_hub_changed(&fault_controller.hcd);
		allocation_fail_at = 0;
		device = drv_usb_find_device(fault_controller.bus_number, 1);
		if (device != NULL) {
			fault_success_nth = fault_nth;
			fake_unregister(&fault_controller);
		} else {
			CHECK(drv_usb_hcd_unregister(&fault_controller.hcd) == 0);
		}
		CHECK(live_allocations == baseline);
		if (fault_success_nth != 0)
			break;
	}
	CHECK(fault_success_nth > 20U && fault_success_nth <= 128U);
	CHECK(drv_usb_driver_unregister(&fake_driver) == 0);
	fake_register(&no_driver_controller, 0);
	drv_usb_hcd_root_hub_changed(&no_driver_controller.hcd);
	device = drv_usb_find_device(no_driver_controller.bus_number, 1);
	CHECK(device != NULL);
	CHECK(drv_usb_configuration_descriptor(
	    drv_usb_device_active_configuration(device))->configuration_value == 1);
	CHECK(no_driver_controller.configuration == 1);
	fake_unregister(&no_driver_controller);
	CHECK(live_allocations == 0);

	/* The normal port-removal path has the same function-driver-before-DMA
	 * ownership boundary as terminal shutdown.  A composite fixture fixes the
	 * all-functions rule: the first detach may fail, but a later independent
	 * function must still detach before device quiesce is attempted. */
	CHECK(drv_usb_driver_register(&fake_driver) == 0);
	CHECK(drv_usb_driver_register(&fake_storage_driver) == 0);
	fake_driver_state.mode = FAKE_ATTACH_CLAIM_SUCCESS;
	fake_driver_state.detach_error = 0;

	fake_register(&hotplug_success, 0);
	hotplug_success.teardown_tracking = 1U;
	hotplug_success.teardown_composite = 1U;
	hotplug_success.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&hotplug_success.hcd);
	device = drv_usb_find_device(hotplug_success.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	data = drv_usb_configuration_find_interface(configuration, 1);
	storage = drv_usb_configuration_find_interface(configuration, 2);
	CHECK(control != NULL && data != NULL && storage != NULL);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	CHECK(drv_usb_interface_driver(storage) == &fake_storage_driver);
	hotplug_success.connected = 0;
	drv_usb_hcd_root_hub_changed(&hotplug_success.hcd);
	CHECK(drv_usb_find_device(hotplug_success.bus_number, 1) == NULL);
	CHECK(hotplug_success.teardown_detach_sequence == 1U);
	CHECK(hotplug_success.teardown_later_detach_sequence == 2U);
	CHECK(hotplug_success.teardown_device_quiesce_sequence == 3U);
	CHECK(hotplug_success.teardown_device_disable_sequence == 4U);
	CHECK(hotplug_success.teardown_sequence == 4U);
	hotplug_success.teardown_tracking = 0;
	fake_unregister(&hotplug_success);

	fake_register(&hotplug_detach_failure, 0);
	hotplug_detach_failure.teardown_tracking = 1U;
	hotplug_detach_failure.teardown_composite = 1U;
	hotplug_detach_failure.teardown_detach_error = EBUSY;
	hotplug_detach_failure.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&hotplug_detach_failure.hcd);
	device = drv_usb_find_device(hotplug_detach_failure.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	data = drv_usb_configuration_find_interface(configuration, 1);
	storage = drv_usb_configuration_find_interface(configuration, 2);
	CHECK(control != NULL && data != NULL && storage != NULL);
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	CHECK(drv_usb_interface_driver(storage) == &fake_storage_driver);
	hotplug_detach_failure.connected = 0;
	drv_usb_hcd_root_hub_changed(&hotplug_detach_failure.hcd);
	CHECK(drv_usb_find_device(hotplug_detach_failure.bus_number, 1) ==
	    device);
	CHECK(hotplug_detach_failure.teardown_detach_sequence == 1U);
	CHECK(hotplug_detach_failure.teardown_later_detach_sequence == 2U);
	CHECK(hotplug_detach_failure.teardown_device_quiesce_sequence == 3U);
	CHECK(hotplug_detach_failure.teardown_device_disable_sequence == 0U);
	CHECK(hotplug_detach_failure.teardown_sequence == 3U);
	/* Failed owner and its claim remain intact; the later successful
	 * independent function has released its ownership. */
	CHECK(drv_usb_interface_driver(control) == &fake_driver);
	CHECK(drv_usb_interface_claimed_by(data) == control);
	CHECK(drv_usb_interface_driver(storage) == NULL);
	hotplug_detach_failure.teardown_tracking = 0;
	hotplug_detach_failure.teardown_detach_error = 0;
	drv_usb_hcd_root_hub_changed(&hotplug_detach_failure.hcd);
	CHECK(drv_usb_find_device(hotplug_detach_failure.bus_number, 1) == NULL);
	fake_unregister(&hotplug_detach_failure);
	CHECK(drv_usb_driver_unregister(&fake_storage_driver) == 0);

	/* drv_usb_shutdown() is a terminal boundary: the platform resets after
	 * HCD stop, while failed teardown deliberately retains callback-visible
	 * objects.  Exercise all three buses in one terminal invocation. */

	fake_register(&shutdown_success, 0);
	shutdown_success.teardown_tracking = 1U;
	shutdown_success.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&shutdown_success.hcd);
	device = drv_usb_find_device(shutdown_success.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	CHECK(control != NULL &&
	    drv_usb_interface_driver(control) == &fake_driver);

	fake_register(&shutdown_detach_failure, 0);
	shutdown_detach_failure.teardown_tracking = 1U;
	shutdown_detach_failure.teardown_detach_error = EBUSY;
	shutdown_detach_failure.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&shutdown_detach_failure.hcd);
	device = drv_usb_find_device(shutdown_detach_failure.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	CHECK(control != NULL &&
	    drv_usb_interface_driver(control) == &fake_driver);

	fake_register(&shutdown_device_failure, 0);
	shutdown_device_failure.teardown_tracking = 1U;
	shutdown_device_failure.teardown_device_quiesce_error = EIO;
	shutdown_device_failure.vendor_first = 1U;
	drv_usb_hcd_root_hub_changed(&shutdown_device_failure.hcd);
	device = drv_usb_find_device(shutdown_device_failure.bus_number, 1);
	CHECK(device != NULL);
	configuration = drv_usb_device_active_configuration(device);
	control = drv_usb_configuration_find_interface(configuration, 0);
	CHECK(control != NULL &&
	    drv_usb_interface_driver(control) == &fake_driver);

	drv_usb_shutdown();
	CHECK(shutdown_success.teardown_detach_sequence == 1U);
	CHECK(shutdown_success.teardown_device_quiesce_sequence == 2U);
	CHECK(shutdown_success.teardown_hcd_quiesce_sequence == 3U);
	CHECK(shutdown_success.teardown_stop_sequence == 4U);
	CHECK(shutdown_success.teardown_sequence == 4U);

	CHECK(shutdown_detach_failure.teardown_detach_sequence == 1U);
	CHECK(shutdown_detach_failure.teardown_device_quiesce_sequence == 2U);
	CHECK(shutdown_detach_failure.teardown_hcd_quiesce_sequence == 3U);
	CHECK(shutdown_detach_failure.teardown_stop_sequence == 0U);
	CHECK(shutdown_detach_failure.teardown_sequence == 3U);

	CHECK(shutdown_device_failure.teardown_detach_sequence == 1U);
	CHECK(shutdown_device_failure.teardown_device_quiesce_sequence == 2U);
	CHECK(shutdown_device_failure.teardown_hcd_quiesce_sequence == 3U);
	CHECK(shutdown_device_failure.teardown_stop_sequence == 0U);
	CHECK(shutdown_device_failure.teardown_sequence == 3U);

	printf("usb function model: %u checks passed\n", checks);
	fflush(stdout);
	/* The terminal shutdown contract intentionally retains the core graph
	 * until reset; bypass process-exit leak accounting for only that graph. */
	_Exit(0);
}
