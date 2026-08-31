/*
 * WS004 HW-T26 production USB recovery contract fixture.
 *
 * The production core is included so the focused test can inspect its private
 * halt, gate, generation, quarantine, and preallocated-recovery ownership
 * without widening the public USB API.
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

static atomic_uint checks;
static atomic_size_t live_allocations;
static atomic_uint allocation_forbidden;
static atomic_uint forbidden_allocations;

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
	void *pointer;

	if (atomic_load_explicit(&allocation_forbidden,
	    memory_order_acquire) != 0)
		(void)atomic_fetch_add_explicit(&forbidden_allocations, 1U,
		    memory_order_relaxed);
	pointer = malloc(size);
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

static const uint8_t recovery_device_descriptor[] = {
	18, 1, 0x00, 0x02, 0, 0, 0, 64,
	0x34, 0x12, 0x26, 0x00, 0x00, 0x01, 0, 0, 0, 1
};

/* Interface zero has the bulk pair used for halt recovery and an alternate
 * interrupt endpoint.  Interface one supplies an independently admitted
 * sibling endpoint for target-versus-device drain checks. */
static const uint8_t recovery_configuration[] = {
	9, 2, 64, 0, 2, 1, 0, 0x80, 50,
	9, 4, 0, 0, 2, 0xff, 0, 0, 0,
	7, 5, 0x81, 2, 64, 0, 0,
	7, 5, 0x02, 2, 64, 0, 0,
	9, 4, 0, 1, 1, 0xff, 0, 0, 0,
	7, 5, 0x83, 3, 8, 0, 1,
	9, 4, 1, 0, 1, 0xfe, 0, 0, 0,
	7, 5, 0x84, 3, 8, 0, 1
};

enum recovery_event_kind {
	RECOVERY_EVENT_ENDPOINT_DISABLE,
	RECOVERY_EVENT_DEVICE_QUIESCE,
	RECOVERY_EVENT_DEVICE_DISABLE,
	RECOVERY_EVENT_ROOT_RESET,
	RECOVERY_EVENT_DEVICE_ENABLE,
	RECOVERY_EVENT_SET_ADDRESS,
	RECOVERY_EVENT_SET_CONFIGURATION,
	RECOVERY_EVENT_SET_INTERFACE,
	RECOVERY_EVENT_ENDPOINT_ENABLE,
	RECOVERY_EVENT_CLEAR_HALT_WIRE,
	RECOVERY_EVENT_ENDPOINT_RESET
};

struct recovery_event {
	enum recovery_event_kind kind;
	unsigned value;
};

enum control_behavior {
	CONTROL_COMPLETE,
	CONTROL_STALL,
	CONTROL_IO_ERROR,
	CONTROL_TIMEOUT,
	CONTROL_PRE_ENQUEUE_ERROR,
	CONTROL_HOLD,
	CONTROL_DISCONNECT
};

enum data_behavior {
	DATA_COMPLETE,
	DATA_STALL,
	DATA_IO_ERROR,
	DATA_HOLD
};

struct fake_controller {
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	unsigned bus_number;
	atomic_uint connected;
	atomic_uint enabled;
	atomic_uint connection_changed;
	unsigned configuration;
	unsigned alternate[DRV_USB_MAX_INTERFACES];
	unsigned endpoint_enabled[256];
	unsigned endpoint_toggle[256];
	unsigned endpoint_enable_count;
	unsigned endpoint_disable_count;
	unsigned endpoint_reset_count;
	unsigned data_enqueue_count;
	unsigned control_enqueue_count;
	unsigned clear_halt_count;
	unsigned last_clear_address;
	struct drv_usb_control_request last_clear_request;
	enum control_behavior clear_behavior;
	enum data_behavior data_behavior;
	struct drv_usb_urb *held_data;
	struct drv_usb_urb *held_control;
	struct recovery_event events[128];
	unsigned event_count;
	unsigned expect_halted_reset;
	unsigned reset_saw_halted;
	unsigned fail_endpoint_disable_address;
	unsigned fail_endpoint_enable_address;
	unsigned fail_endpoint_reset_address;
	int fail_device_quiesce;
	int fail_root_reset;
	int fail_device_enable;
	int fail_set_address;
	int fail_set_configuration;
	int fail_set_interface;
	unsigned inject_connection_edge;
	unsigned inject_connection_edge_on_endpoint_disable_address;
};

static struct fake_controller *
controller_from_hcd(struct drv_usb_hcd *hcd)
{
	return (struct fake_controller *)hcd->private_data[0];
}

static void
record_event(struct fake_controller *controller,
	enum recovery_event_kind kind, unsigned value)
{
	CHECK(controller->event_count < sizeof(controller->events) /
	    sizeof(controller->events[0]));
	controller->events[controller->event_count].kind = kind;
	controller->events[controller->event_count].value = value;
	controller->event_count++;
}

static int
event_index(const struct fake_controller *controller,
	enum recovery_event_kind kind, unsigned after)
{
	unsigned index;

	for (index = after; index < controller->event_count; index++)
		if (controller->events[index].kind == kind)
			return (int)index;
	return -1;
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
fake_complete(struct fake_controller *controller, struct drv_usb_urb *urb,
	enum drv_usb_urb_status status, size_t actual)
{
	drv_usb_hcd_complete(&controller->hcd, urb, status, actual);
}

static int
fake_enqueue_control(struct fake_controller *controller,
	struct drv_usb_urb *urb, const struct drv_usb_control_request *request)
{
	const uint8_t *bytes = NULL;
	size_t length = 0;

	controller->control_enqueue_count++;
	if (request->request_type ==
	    (DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE) &&
	    request->request == USB_REQ_GET_DESCRIPTOR) {
		if ((request->value >> 8) == DRV_USB_DESCRIPTOR_DEVICE) {
			bytes = recovery_device_descriptor;
			length = sizeof(recovery_device_descriptor);
		} else if ((request->value >> 8) ==
		    DRV_USB_DESCRIPTOR_CONFIGURATION) {
			bytes = recovery_configuration;
			length = sizeof(recovery_configuration);
		}
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE) &&
	    request->request == USB_REQ_SET_ADDRESS) {
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE) &&
	    request->request == USB_REQ_SET_CONFIGURATION) {
		record_event(controller, RECOVERY_EVENT_SET_CONFIGURATION,
		    request->value);
		if (controller->fail_set_configuration != 0) {
			int error = controller->fail_set_configuration;

			controller->fail_set_configuration = 0;
			fake_complete(controller, urb,
			    error == EPIPE ? DRV_USB_URB_STALL :
			    DRV_USB_URB_IO_ERROR, 0);
			return 0;
		}
		controller->configuration = request->value;
		memset(controller->alternate, 0, sizeof(controller->alternate));
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_INTERFACE) && request->request ==
	    USB_REQ_SET_INTERFACE) {
		record_event(controller, RECOVERY_EVENT_SET_INTERFACE,
		    ((unsigned)request->index << 16) | request->value);
		if (controller->fail_set_interface != 0) {
			int error = controller->fail_set_interface;

			controller->fail_set_interface = 0;
			fake_complete(controller, urb,
			    error == EPIPE ? DRV_USB_URB_STALL :
			    DRV_USB_URB_IO_ERROR, 0);
			return 0;
		}
		controller->alternate[request->index] = request->value;
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if (request->request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_ENDPOINT) && request->request ==
	    USB_REQ_CLEAR_FEATURE && request->value ==
	    USB_FEATURE_ENDPOINT_HALT && request->length == 0) {
		controller->clear_halt_count++;
		controller->last_clear_request = *request;
		controller->last_clear_address = request->index;
		record_event(controller, RECOVERY_EVENT_CLEAR_HALT_WIRE,
		    request->index);
		if (controller->clear_behavior == CONTROL_PRE_ENQUEUE_ERROR) {
			controller->clear_behavior = CONTROL_COMPLETE;
			return EIO;
		}
		if (controller->clear_behavior == CONTROL_HOLD) {
			CHECK(controller->held_control == NULL);
			controller->held_control = urb;
			return 0;
		}
		if (controller->clear_behavior == CONTROL_STALL) {
			controller->clear_behavior = CONTROL_COMPLETE;
			fake_complete(controller, urb, DRV_USB_URB_STALL, 0);
			return 0;
		}
		if (controller->clear_behavior == CONTROL_IO_ERROR) {
			controller->clear_behavior = CONTROL_COMPLETE;
			fake_complete(controller, urb, DRV_USB_URB_IO_ERROR, 0);
			return 0;
		}
		if (controller->clear_behavior == CONTROL_TIMEOUT) {
			controller->clear_behavior = CONTROL_COMPLETE;
			fake_complete(controller, urb, DRV_USB_URB_TIMEOUT, 0);
			return 0;
		}
		if (controller->clear_behavior == CONTROL_DISCONNECT) {
			struct drv_usb_device *device = drv_usb_urb_device(urb);

			controller->clear_behavior = CONTROL_COMPLETE;
			(void)hal_atomic_fetch_or_release(&device->lifecycle,
			    USB_DEVICE_LIFECYCLE_DISCONNECTING);
			io_gate_close(&device->binding_transactions);
			io_gate_close(&device->submit_gate);
			fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
			return 0;
		}
		fake_complete(controller, urb, DRV_USB_URB_COMPLETE, 0);
		return 0;
	} else if ((request->request_type & 0x60U) ==
	    DRV_USB_REQUEST_VENDOR && controller->clear_behavior == CONTROL_HOLD) {
		CHECK(controller->held_control == NULL);
		controller->held_control = urb;
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
	const struct drv_usb_control_request *request =
	    drv_usb_urb_control_request(urb);
	uint8_t address;

	if (request != NULL)
		return fake_enqueue_control(controller, urb, request);
	address = drv_usb_endpoint_address(drv_usb_urb_endpoint(urb));
	controller->data_enqueue_count++;
	CHECK(controller->endpoint_enabled[address] != 0);
	if (controller->data_behavior == DATA_HOLD) {
		CHECK(controller->held_data == NULL);
		controller->held_data = urb;
		return 0;
	}
	if (controller->data_behavior == DATA_STALL) {
		controller->data_behavior = DATA_COMPLETE;
		fake_complete(controller, urb, DRV_USB_URB_STALL, 0);
		return 0;
	}
	if (controller->data_behavior == DATA_IO_ERROR) {
		controller->data_behavior = DATA_COMPLETE;
		fake_complete(controller, urb, DRV_USB_URB_IO_ERROR, 0);
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

	if (controller->held_data == urb) {
		controller->held_data = NULL;
		return 0;
	}
	if (controller->held_control == urb) {
		controller->held_control = NULL;
		return 0;
	}
	return EBUSY;
}

static int
fake_device_enable(struct drv_usb_hcd *hcd, struct drv_usb_device *device)
{
	struct fake_controller *controller = controller_from_hcd(hcd);

	(void)device;
	record_event(controller, RECOVERY_EVENT_DEVICE_ENABLE, 0);
	if (controller->fail_device_enable != 0) {
		int error = controller->fail_device_enable;

		controller->fail_device_enable = 0;
		return error;
	}
	return 0;
}

static int
fake_device_set_address(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device, unsigned address)
{
	struct fake_controller *controller = controller_from_hcd(hcd);

	(void)device;
	record_event(controller, RECOVERY_EVENT_SET_ADDRESS, address);
	if (controller->fail_set_address != 0) {
		int error = controller->fail_set_address;

		controller->fail_set_address = 0;
		return error;
	}
	return 0;
}

static int
fake_device_quiesce(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	struct fake_controller *controller = controller_from_hcd(hcd);

	(void)device;
	record_event(controller, RECOVERY_EVENT_DEVICE_QUIESCE, 0);
	if (controller->fail_device_quiesce != 0) {
		int error = controller->fail_device_quiesce;

		controller->fail_device_quiesce = 0;
		return error;
	}
	return 0;
}

static void
fake_device_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_device *device)
{
	struct fake_controller *controller = controller_from_hcd(hcd);

	(void)device;
	record_event(controller, RECOVERY_EVENT_DEVICE_DISABLE, 0);
}

static int
fake_endpoint_enable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);

	controller->endpoint_enable_count++;
	record_event(controller, RECOVERY_EVENT_ENDPOINT_ENABLE, address);
	if (controller->fail_endpoint_enable_address == address) {
		controller->fail_endpoint_enable_address = 0;
		return EIO;
	}
	if (controller->endpoint_enabled[address] != 0)
		return 0;
	controller->endpoint_enabled[address] = 1U;
	return 0;
}

static int
fake_endpoint_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);

	controller->endpoint_disable_count++;
	record_event(controller, RECOVERY_EVENT_ENDPOINT_DISABLE, address);
	if (controller->fail_endpoint_disable_address == address) {
		controller->fail_endpoint_disable_address = 0;
		return EIO;
	}
	if (controller->endpoint_enabled[address] == 0)
		return 0;
	controller->endpoint_enabled[address] = 0U;
	if (controller->inject_connection_edge_on_endpoint_disable_address ==
	    address) {
		controller->inject_connection_edge_on_endpoint_disable_address = 0U;
		atomic_store_explicit(&controller->connection_changed, 1U,
		    memory_order_release);
	}
	return 0;
}

static int
fake_endpoint_reset(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct fake_controller *controller = controller_from_hcd(hcd);
	uint8_t address = drv_usb_endpoint_address(endpoint);

	controller->endpoint_reset_count++;
	record_event(controller, RECOVERY_EVENT_ENDPOINT_RESET, address);
	CHECK(controller->endpoint_enabled[address] != 0);
	if (controller->expect_halted_reset) {
		controller->reset_saw_halted =
		    atomic_load_acquire(&endpoint->halted) != 0;
		controller->expect_halted_reset = 0U;
	}
	if (controller->fail_endpoint_reset_address == address) {
		controller->fail_endpoint_reset_address = 0;
		return EIO;
	}
	controller->endpoint_toggle[address] = 0U;
	return 0;
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
		    memory_order_acquire) != 0 ? 1U | 0x400U : 0U;
		if (atomic_load_explicit(&controller->enabled,
		    memory_order_acquire) != 0)
			status |= 2U;
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
	struct fake_controller *controller = controller_from_hcd(hcd);

	record_event(controller, RECOVERY_EVENT_ROOT_RESET, port);
	if (port != 1U)
		return EINVAL;
	if (controller->inject_connection_edge) {
		controller->inject_connection_edge = 0U;
		atomic_store_explicit(&controller->connection_changed, 1U,
		    memory_order_release);
	}
	if (controller->fail_root_reset != 0) {
		int error = controller->fail_root_reset;

		controller->fail_root_reset = 0;
		return error;
	}
	return 0;
}

static const struct drv_usb_hcd_ops fake_ops = {
	.start = fake_start,
	.quiesce = fake_quiesce,
	.stop = fake_stop,
	.device_enable = fake_device_enable,
	.device_set_address = fake_device_set_address,
	.device_quiesce = fake_device_quiesce,
	.device_disable = fake_device_disable,
	.urb_enqueue = fake_enqueue,
	.urb_dequeue = fake_dequeue,
	.endpoint_enable = fake_endpoint_enable,
	.endpoint_disable = fake_endpoint_disable,
	.endpoint_reset = fake_endpoint_reset,
	.root_hub_control = fake_root_control,
	.root_port_reset = fake_root_reset
};

enum recovery_binding_mode {
	RECOVERY_BIND_CLAIM_SIBLING,
	RECOVERY_BIND_INDEPENDENT
};

static struct {
	enum recovery_binding_mode mode;
	unsigned primary_token;
	unsigned secondary_token;
	unsigned primary_attach_count;
	unsigned primary_detach_count;
	unsigned secondary_attach_count;
	unsigned secondary_detach_count;
	struct drv_usb_interface *primary;
	struct drv_usb_interface *sibling;
} recovery_binding;

static int
recovery_primary_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	return 100;
}

static int
recovery_primary_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *sibling;

	(void)id;
	recovery_binding.primary_attach_count++;
	recovery_binding.primary = interface;
	CHECK(atomic_load_acquire(&interface->binding_state) ==
	    USB_BINDING_PROBING);
	CHECK(drv_usb_interface_set_driver_data(interface,
	    &recovery_binding.primary_token) == 0);
	configuration = drv_usb_device_active_configuration(
	    drv_usb_interface_device(interface));
	sibling = drv_usb_configuration_find_interface(configuration, 1U);
	CHECK(sibling != NULL);
	recovery_binding.sibling = sibling;
	if (recovery_binding.mode == RECOVERY_BIND_INDEPENDENT)
		return 0;
	CHECK(drv_usb_interface_claim(interface, sibling) == 0);
	return 0;
}

static int
recovery_primary_detach(struct drv_usb_interface *interface, unsigned flags)
{
	(void)flags;
	CHECK(interface == recovery_binding.primary);
	recovery_binding.primary_detach_count++;
	return 0;
}

static const struct drv_usb_id recovery_primary_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS,
	.interface_class = 0xffU
}};

static struct drv_usb_driver recovery_primary_driver = {
	.name = "usb-recovery-primary-fixture",
	.ids = recovery_primary_ids,
	.id_count = sizeof(recovery_primary_ids) /
	    sizeof(recovery_primary_ids[0]),
	.match = recovery_primary_match,
	.attach = recovery_primary_attach,
	.detach = recovery_primary_detach
};

static int
recovery_secondary_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)interface;
	(void)id;
	return 100;
}

static int
recovery_secondary_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	(void)id;
	recovery_binding.secondary_attach_count++;
	CHECK(drv_usb_interface_set_driver_data(interface,
	    &recovery_binding.secondary_token) == 0);
	return 0;
}

static int
recovery_secondary_detach(struct drv_usb_interface *interface,
	unsigned flags)
{
	(void)interface;
	(void)flags;
	recovery_binding.secondary_detach_count++;
	return 0;
}

static const struct drv_usb_id recovery_secondary_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS,
	.interface_class = 0xfeU
}};

static struct drv_usb_driver recovery_secondary_driver = {
	.name = "usb-recovery-secondary-fixture",
	.ids = recovery_secondary_ids,
	.id_count = sizeof(recovery_secondary_ids) /
	    sizeof(recovery_secondary_ids[0]),
	.match = recovery_secondary_match,
	.attach = recovery_secondary_attach,
	.detach = recovery_secondary_detach
};

static void
set_binding_mode(enum recovery_binding_mode mode)
{
	recovery_binding.mode = mode;
	recovery_binding.primary = NULL;
	recovery_binding.sibling = NULL;
}

static struct drv_usb_device *
register_controller(struct fake_controller *controller)
{
	struct drv_usb_device *device;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *primary, *sibling;

	memset(controller, 0, sizeof(*controller));
	controller->hcd.name = "usb-recovery-contract-fixture";
	controller->hcd.ops = &fake_ops;
	controller->hcd.root_port_count = 1U;
	controller->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	controller->hcd.private_data[0] = (uintptr_t)controller;
	atomic_store_explicit(&controller->connected, 1U, memory_order_relaxed);
	atomic_store_explicit(&controller->enabled, 1U, memory_order_relaxed);
	atomic_store_explicit(&controller->connection_changed, 1U,
	    memory_order_relaxed);
	CHECK(drv_usb_hcd_register(&controller->hcd, &controller->bus) == 0);
	controller->bus_number = drv_usb_bus_number(controller->bus);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	device = drv_usb_find_device(controller->bus_number, 1U);
	CHECK(device != NULL);
	CHECK(drv_usb_device_state(device) == DRV_USB_STATE_CONFIGURED);
	configuration = drv_usb_device_active_configuration(device);
	primary = drv_usb_configuration_find_interface(configuration, 0U);
	sibling = drv_usb_configuration_find_interface(configuration, 1U);
	CHECK(primary != NULL && sibling != NULL);
	CHECK(drv_usb_interface_driver(primary) == &recovery_primary_driver);
	CHECK(drv_usb_interface_driver_data(primary) ==
	    &recovery_binding.primary_token);
	CHECK(atomic_load_acquire(&primary->binding_state) == USB_BINDING_BOUND);
	if (recovery_binding.mode == RECOVERY_BIND_CLAIM_SIBLING) {
		CHECK(drv_usb_interface_claimed_by(sibling) == primary);
		CHECK(drv_usb_interface_driver(sibling) == NULL);
	} else {
		CHECK(drv_usb_interface_claimed_by(sibling) == NULL);
		CHECK(drv_usb_interface_driver(sibling) ==
		    &recovery_secondary_driver);
		CHECK(drv_usb_interface_driver_data(sibling) ==
		    &recovery_binding.secondary_token);
	}
	return device;
}

static void
unregister_controller(struct fake_controller *controller)
{
	controller->fail_device_quiesce = 0;
	controller->fail_endpoint_disable_address = 0U;
	controller->fail_endpoint_enable_address = 0U;
	controller->fail_endpoint_reset_address = 0U;
	controller->held_data = NULL;
	controller->held_control = NULL;
	atomic_store_explicit(&controller->connected, 0U, memory_order_release);
	atomic_store_explicit(&controller->connection_changed, 1U,
	    memory_order_release);
	drv_usb_hcd_root_hub_changed(&controller->hcd);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == NULL);
	CHECK(drv_usb_hcd_unregister(&controller->hcd) == 0);
}

static struct drv_usb_endpoint *
find_endpoint(struct drv_usb_device *device, unsigned interface_number,
	uint8_t address)
{
	struct drv_usb_configuration *configuration =
	    drv_usb_device_active_configuration(device);
	struct drv_usb_interface *interface =
	    drv_usb_configuration_find_interface(configuration, interface_number);
	unsigned index;

	CHECK(interface != NULL);
	for (index = 0; index < drv_usb_interface_endpoint_count(interface);
	    index++) {
		struct drv_usb_endpoint *endpoint =
		    drv_usb_interface_endpoint(interface, index);

		if (drv_usb_endpoint_address(endpoint) == address)
			return endpoint;
	}
	return NULL;
}

static void
complete_held_data(struct fake_controller *controller,
	enum drv_usb_urb_status status)
{
	struct drv_usb_urb *urb = controller->held_data;

	CHECK(urb != NULL);
	controller->held_data = NULL;
	fake_complete(controller, urb, status,
	    status == DRV_USB_URB_COMPLETE ? drv_usb_urb_length(urb) : 0);
}

struct stall_callback_fixture {
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *rearm;
	unsigned callback_count;
	unsigned saw_halted;
	int rearm_result;
	int callback_clear_result;
};

static void
stall_callback(struct drv_usb_urb *urb, void *argument)
{
	struct stall_callback_fixture *fixture = argument;

	CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_STALL);
	fixture->callback_count++;
	fixture->saw_halted =
	    atomic_load_acquire(&fixture->endpoint->halted) != 0;
	fixture->rearm_result = drv_usb_urb_submit(fixture->rearm);
	fixture->callback_clear_result =
	    drv_usb_endpoint_clear_halt(fixture->endpoint);
}

static void
exercise_selection_reset_semantics(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration =
	    drv_usb_device_active_configuration(device);
	struct drv_usb_interface *interface =
	    drv_usb_configuration_find_interface(configuration, 0U);
	const struct drv_usb_host_interface *alternate0, *alternate1;
	struct drv_usb_endpoint *bulk_in, *interrupt_in;
	unsigned clear_before, reset_before;

	CHECK(interface != NULL);
	alternate0 = drv_usb_interface_find_alternate(interface, 0U);
	alternate1 = drv_usb_interface_find_alternate(interface, 1U);
	CHECK(alternate0 != NULL && alternate1 != NULL);
	bulk_in = drv_usb_host_interface_endpoint(alternate0, 0U);
	interrupt_in = drv_usb_host_interface_endpoint(alternate1, 0U);
	CHECK(bulk_in != NULL && drv_usb_endpoint_address(bulk_in) == 0x81U);
	CHECK(interrupt_in != NULL &&
	    drv_usb_endpoint_address(interrupt_in) == 0x83U);

	/* A failed SET_INTERFACE rolls scheduling back without resetting the old
	 * host toggle or clearing its core halt state. */
	controller->endpoint_toggle[0x81U] = 1U;
	atomic_store_release(&bulk_in->halted, 1U);
	controller->fail_set_interface = EPIPE;
	controller->event_count = 0U;
	reset_before = controller->endpoint_reset_count;
	CHECK(drv_usb_interface_set_alternate(interface, 1U) == EPIPE);
	CHECK(drv_usb_interface_active_alternate(interface) == alternate0);
	CHECK(controller->endpoint_toggle[0x81U] == 1U);
	CHECK(atomic_load_acquire(&bulk_in->halted) != 0);
	CHECK(controller->endpoint_reset_count == reset_before);
	CHECK(controller->endpoint_enabled[0x81U] != 0 &&
	    controller->endpoint_enabled[0x02U] != 0 &&
	    controller->endpoint_enabled[0x83U] == 0);

	/* A confirmed alternate selection resets the target schedule to DATA0 and
	 * publishes its retained endpoint unhalted only after the HCD callback. */
	controller->endpoint_toggle[0x83U] = 1U;
	atomic_store_release(&interrupt_in->halted, 1U);
	controller->event_count = 0U;
	reset_before = controller->endpoint_reset_count;
	CHECK(drv_usb_interface_set_alternate(interface, 1U) == 0);
	CHECK(drv_usb_interface_active_alternate(interface) == alternate1);
	CHECK(controller->endpoint_toggle[0x83U] == 0U);
	CHECK(atomic_load_acquire(&interrupt_in->halted) == 0);
	CHECK(controller->endpoint_reset_count == reset_before + 1U);
	CHECK(event_index(controller, RECOVERY_EVENT_SET_INTERFACE, 0U) >= 0);
	CHECK(event_index(controller, RECOVERY_EVENT_ENDPOINT_RESET, 0U) >
	    event_index(controller, RECOVERY_EVENT_SET_INTERFACE, 0U));

	/* The old alternate remains retained, but cannot be recovered while stale. */
	clear_before = controller->clear_halt_count;
	CHECK(drv_usb_endpoint_clear_halt(bulk_in) == ENODEV);
	CHECK(controller->clear_halt_count == clear_before);
	CHECK(drv_usb_endpoint_clear_halt(&device->endpoint0) == EINVAL);

	controller->endpoint_toggle[0x81U] = 1U;
	CHECK(drv_usb_interface_set_alternate(interface, 0U) == 0);
	CHECK(drv_usb_interface_active_alternate(interface) == alternate0);
	CHECK(controller->endpoint_toggle[0x81U] == 0U);
	CHECK(atomic_load_acquire(&bulk_in->halted) == 0);
}

static void
exercise_endpoint_recovery(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_endpoint *endpoint = find_endpoint(device, 0U, 0x81U);
	struct drv_usb_endpoint *sibling = find_endpoint(device, 1U, 0x84U);
	struct drv_usb_urb *stalled, *rearm, *active, *sibling_urb, *control_urb;
	struct drv_usb_control_request vendor = {
		.request_type = DRV_USB_DIR_OUT | DRV_USB_REQUEST_VENDOR |
		    DRV_USB_RECIP_DEVICE,
		.request = 0x55U
	};
	struct stall_callback_fixture callback;
	unsigned char byte = 0;
	unsigned clear_before, enqueue_before, reset_before;
	int wire_index, reset_index;

	CHECK(endpoint != NULL && sibling != NULL);
	stalled = drv_usb_urb_alloc(device, endpoint, 0);
	rearm = drv_usb_urb_alloc(device, endpoint, 0);
	active = drv_usb_urb_alloc(device, endpoint, 0);
	sibling_urb = drv_usb_urb_alloc(device, sibling, 0);
	control_urb = drv_usb_urb_alloc(device, NULL, 0);
	CHECK(stalled != NULL && rearm != NULL && active != NULL &&
	    sibling_urb != NULL && control_urb != NULL);
	memset(&callback, 0, sizeof(callback));
	callback.endpoint = endpoint;
	callback.rearm = rearm;
	CHECK(drv_usb_urb_setup(rearm, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	CHECK(drv_usb_urb_setup(stalled, &byte, sizeof(byte), 0, 0,
	    stall_callback, &callback) == 0);
	controller->data_behavior = DATA_STALL;
	enqueue_before = controller->data_enqueue_count;
	CHECK(drv_usb_urb_submit(stalled) == 0);
	CHECK(drv_usb_urb_drain(stalled, 0) == 0);
	CHECK(callback.callback_count == 1U && callback.saw_halted != 0);
	CHECK(callback.rearm_result == EPIPE);
	CHECK(callback.callback_clear_result == EBUSY);
	CHECK(controller->data_enqueue_count == enqueue_before + 1U);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);

	controller->event_count = 0U;
	controller->expect_halted_reset = 1U;
	reset_before = controller->endpoint_reset_count;
	atomic_store_explicit(&allocation_forbidden, 1U, memory_order_release);
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == 0);
	atomic_store_explicit(&allocation_forbidden, 0U, memory_order_release);
	CHECK(atomic_load_explicit(&forbidden_allocations,
	    memory_order_relaxed) == 0U);
	CHECK(controller->endpoint_reset_count == reset_before + 1U);
	CHECK(controller->reset_saw_halted != 0);
	CHECK(atomic_load_acquire(&endpoint->halted) == 0);
	CHECK(controller->last_clear_request.request_type ==
	    (DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_ENDPOINT));
	CHECK(controller->last_clear_request.request == USB_REQ_CLEAR_FEATURE);
	CHECK(controller->last_clear_request.value == USB_FEATURE_ENDPOINT_HALT);
	CHECK(controller->last_clear_request.index == 0x81U);
	CHECK(controller->last_clear_request.length == 0U);
	wire_index = event_index(controller, RECOVERY_EVENT_CLEAR_HALT_WIRE, 0U);
	reset_index = event_index(controller, RECOVERY_EVENT_ENDPOINT_RESET, 0U);
	CHECK(wire_index >= 0 && reset_index > wire_index);

	/* Preventive clear is never optimized away when the core latch is clear. */
	clear_before = controller->clear_halt_count;
	reset_before = controller->endpoint_reset_count;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == 0);
	CHECK(controller->clear_halt_count == clear_before + 1U);
	CHECK(controller->endpoint_reset_count == reset_before + 1U);
	CHECK(atomic_load_acquire(&endpoint->halted) == 0);

	CHECK(drv_usb_urb_submit(rearm) == 0);
	CHECK(drv_usb_urb_drain(rearm, 0) == 0);

	/* Ordinary I/O failure neither creates nor clears an endpoint halt. */
	CHECK(drv_usb_urb_setup(active, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	controller->data_behavior = DATA_IO_ERROR;
	CHECK(drv_usb_urb_submit(active) == 0);
	CHECK(drv_usb_urb_drain(active, 0) == 0);
	CHECK(drv_usb_urb_status(active) == DRV_USB_URB_IO_ERROR);
	CHECK(atomic_load_acquire(&endpoint->halted) == 0);

	/* Target-interface ownership rejects before the wire request. */
	CHECK(drv_usb_urb_setup(active, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	controller->data_behavior = DATA_HOLD;
	CHECK(drv_usb_urb_submit(active) == 0);
	clear_before = controller->clear_halt_count;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EBUSY);
	CHECK(controller->clear_halt_count == clear_before);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(active, 0) == 0);

	/* A sibling-interface URB does not own the target interface gate. */
	CHECK(drv_usb_urb_setup(sibling_urb, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	controller->data_behavior = DATA_HOLD;
	CHECK(drv_usb_urb_submit(sibling_urb) == 0);
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == 0);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(sibling_urb, 0) == 0);

	/* Endpoint-zero ownership rejects before a second control enqueue. */
	controller->clear_behavior = CONTROL_HOLD;
	CHECK(drv_usb_urb_setup_control(control_urb, &vendor, NULL, 0, 0,
	    NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(control_urb) == 0);
	clear_before = controller->clear_halt_count;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EBUSY);
	CHECK(controller->clear_halt_count == clear_before);
	CHECK(drv_usb_urb_cancel(control_urb) == 0);
	CHECK(drv_usb_urb_drain(control_urb, 0) == 0);
	controller->clear_behavior = CONTROL_COMPLETE;

	/* Endpoint-zero STALL is terminal for one control request only and never
	 * latches control admission against the following request. */
	CHECK(drv_usb_urb_setup_control(control_urb, &vendor, NULL, 0, 0,
	    NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(control_urb) == 0);
	CHECK(drv_usb_urb_drain(control_urb, 0) == 0);
	CHECK(drv_usb_urb_status(control_urb) == DRV_USB_URB_STALL);
	CHECK(atomic_load_acquire(&device->endpoint0.halted) == 0);
	CHECK(drv_usb_urb_setup_control(control_urb, &vendor, NULL, 0, 0,
	    NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(control_urb) == 0);
	CHECK(drv_usb_urb_drain(control_urb, 0) == 0);
	CHECK(drv_usb_urb_status(control_urb) == DRV_USB_URB_STALL);

	/* A device-side STALL and pre-enqueue error preserve the latch without
	 * invoking host reset or quarantining a known device state. */
	CHECK(drv_usb_urb_setup(stalled, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	controller->data_behavior = DATA_STALL;
	CHECK(drv_usb_urb_submit(stalled) == 0);
	CHECK(drv_usb_urb_drain(stalled, 0) == 0);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	reset_before = controller->endpoint_reset_count;
	controller->clear_behavior = CONTROL_STALL;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EPIPE);
	CHECK(controller->endpoint_reset_count == reset_before);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	CHECK(!device_is_quarantined(device));
	controller->clear_behavior = CONTROL_PRE_ENQUEUE_ERROR;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EIO);
	CHECK(controller->endpoint_reset_count == reset_before);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	CHECK(!device_is_quarantined(device));
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == 0);

	drv_usb_urb_free(control_urb);
	drv_usb_urb_free(sibling_urb);
	drv_usb_urb_free(active);
	drv_usb_urb_free(rearm);
	drv_usb_urb_free(stalled);
}

static void
exercise_successful_device_reset(struct fake_controller *controller,
	struct drv_usb_device *device)
{
	struct drv_usb_configuration *configuration =
	    drv_usb_device_active_configuration(device);
	struct drv_usb_interface *first =
	    drv_usb_configuration_find_interface(configuration, 0U);
	struct drv_usb_interface *second =
	    drv_usb_configuration_find_interface(configuration, 1U);
	const struct drv_usb_host_interface *alternate1;
	struct drv_usb_endpoint *first_endpoint;
	struct drv_usb_endpoint *second_endpoint = find_endpoint(device, 1U, 0x84U);
	struct drv_usb_driver *first_driver;
	struct drv_usb_device *saved_parent;
	struct drv_usb_urb *active;
	void *first_driver_data;
	uint64_t generation, port_generation;
	unsigned address_before;
	unsigned char byte = 0;
	int disable, quiesce, release, reset, enable, address, configure, alternate;

	CHECK(configuration != NULL && first != NULL && second != NULL);
	CHECK(second_endpoint != NULL);
	first_driver = drv_usb_interface_driver(first);
	first_driver_data = drv_usb_interface_driver_data(first);
	CHECK(first_driver == &recovery_primary_driver);
	CHECK(drv_usb_interface_claimed_by(second) == first);

	/* Topology ownership is a nonblocking preflight condition.  A caller which
	 * loses it must not wait behind a root worker that may itself be joining the
	 * caller's callback or submission admission. */
	CHECK(atomic_try_acquire_zero(&usb_topology_gate));
	controller->event_count = 0U;
	CHECK(drv_usb_device_reset(device) == EBUSY);
	CHECK(controller->event_count == 0U);
	CHECK(!device_is_quarantined(device));
	usb_topology_unlock();

	/* Direct-root topology and an enabled connected port are preflight rules;
	 * both failures occur before endpoint/HCD side effects. */
	controller->event_count = 0U;
	atomic_store_explicit(&controller->enabled, 0U, memory_order_release);
	CHECK(drv_usb_device_reset(device) == ENODEV);
	CHECK(controller->event_count == 0U);
	CHECK(!device_is_quarantined(device));
	atomic_store_explicit(&controller->enabled, 1U, memory_order_release);
	saved_parent = device->parent;
	device->parent = device;
	CHECK(drv_usb_device_reset(device) == ENOTSUP);
	CHECK(controller->event_count == 0U);
	device->parent = saved_parent;

	/* Preserve and restore a nonzero alternate, not merely configuration zero. */
	alternate1 = drv_usb_interface_find_alternate(first, 1U);
	CHECK(alternate1 != NULL);
	CHECK(drv_usb_interface_set_alternate(first, 1U) == 0);
	CHECK(drv_usb_interface_active_alternate(first) == alternate1);
	first_endpoint = find_endpoint(device, 0U, 0x83U);
	CHECK(first_endpoint != NULL);
	active = drv_usb_urb_alloc(device, second_endpoint, 0);
	CHECK(active != NULL);
	CHECK(drv_usb_urb_setup(active, &byte, sizeof(byte), 0, 0, NULL,
	    NULL) == 0);
	controller->data_behavior = DATA_HOLD;
	CHECK(drv_usb_urb_submit(active) == 0);
	controller->event_count = 0U;
	CHECK(drv_usb_device_reset(device) == EBUSY);
	CHECK(controller->event_count == 0U);
	complete_held_data(controller, DRV_USB_URB_COMPLETE);
	CHECK(drv_usb_urb_drain(active, 0) == 0);

	generation = device->generation;
	port_generation = device->port_generation;
	address_before = device->address;
	controller->event_count = 0U;
	CHECK(drv_usb_device_reset(device) == 0);
	CHECK(drv_usb_find_device(controller->bus_number, 1U) == device);
	CHECK(device->generation == generation);
	CHECK(device->port_generation == port_generation);
	CHECK(device->address == address_before);
	CHECK(drv_usb_device_active_configuration(device) == configuration);
	CHECK(drv_usb_configuration_find_interface(configuration, 0U) == first);
	CHECK(drv_usb_configuration_find_interface(configuration, 1U) == second);
	CHECK(drv_usb_interface_active_alternate(first) == alternate1);
	CHECK(find_endpoint(device, 0U, 0x83U) == first_endpoint);
	CHECK(find_endpoint(device, 1U, 0x84U) == second_endpoint);
	CHECK(drv_usb_interface_driver(first) == first_driver);
	CHECK(drv_usb_interface_driver_data(first) == first_driver_data);
	CHECK(drv_usb_interface_claimed_by(second) == first);
	disable = event_index(controller, RECOVERY_EVENT_ENDPOINT_DISABLE, 0U);
	quiesce = event_index(controller, RECOVERY_EVENT_DEVICE_QUIESCE, 0U);
	release = event_index(controller, RECOVERY_EVENT_DEVICE_DISABLE, 0U);
	reset = event_index(controller, RECOVERY_EVENT_ROOT_RESET, 0U);
	enable = event_index(controller, RECOVERY_EVENT_DEVICE_ENABLE, 0U);
	address = event_index(controller, RECOVERY_EVENT_SET_ADDRESS, 0U);
	configure = event_index(controller, RECOVERY_EVENT_SET_CONFIGURATION, 0U);
	alternate = event_index(controller, RECOVERY_EVENT_SET_INTERFACE,
	    (unsigned)configure + 1U);
	CHECK(disable >= 0 && quiesce > disable && release > quiesce &&
	    reset > release && enable > reset && address > enable &&
	    configure > address);
	CHECK(alternate > configure);
	CHECK(event_index(controller, RECOVERY_EVENT_ENDPOINT_ENABLE,
	    (unsigned)alternate + 1U) > alternate);
	CHECK(event_index(controller, RECOVERY_EVENT_ENDPOINT_RESET,
	    (unsigned)alternate + 1U) > alternate);
	CHECK(controller->endpoint_toggle[0x83U] == 0U);
	CHECK(controller->endpoint_toggle[0x84U] == 0U);
	CHECK(!device_is_quarantined(device));
	CHECK(drv_usb_device_state(device) == DRV_USB_STATE_CONFIGURED);
	drv_usb_urb_free(active);
}

static void
exercise_multiple_owner_preflight(void)
{
	struct fake_controller controller;
	struct drv_usb_device *device;

	set_binding_mode(RECOVERY_BIND_INDEPENDENT);
	device = register_controller(&controller);
	controller.event_count = 0U;
	CHECK(drv_usb_device_reset(device) == ENOTSUP);
	CHECK(controller.event_count == 0U);
	CHECK(!device_is_quarantined(device));
	unregister_controller(&controller);
	set_binding_mode(RECOVERY_BIND_CLAIM_SIBLING);
}

static void
exercise_reset_failures(void)
{
	struct fake_controller controller;
	struct drv_usb_device *device;
	int reset_index;

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_endpoint_disable_address = 0x02U;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(event_index(&controller, RECOVERY_EVENT_DEVICE_QUIESCE, 0U) < 0);
	CHECK(!device_is_quarantined(device));
	CHECK(controller.endpoint_enabled[0x81U] != 0 &&
	    controller.endpoint_enabled[0x02U] != 0);
	CHECK(drv_usb_device_reset(device) == 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.inject_connection_edge_on_endpoint_disable_address = 0x84U;
	CHECK(drv_usb_device_reset(device) == ENODEV);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_DEVICE_QUIESCE, 0U) < 0);
	CHECK(event_index(&controller, RECOVERY_EVENT_ENDPOINT_ENABLE, 0U) < 0);
	CHECK(controller.endpoint_enabled[0x81U] == 0U);
	CHECK(controller.endpoint_enabled[0x02U] == 0U);
	CHECK(controller.endpoint_enabled[0x84U] == 0U);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_endpoint_disable_address = 0x02U;
	controller.fail_endpoint_enable_address = 0x81U;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(event_index(&controller, RECOVERY_EVENT_DEVICE_QUIESCE, 0U) < 0);
	CHECK(device_is_quarantined(device));
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_device_quiesce = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_ROOT_RESET, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_root_reset = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_DEVICE_ENABLE, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.inject_connection_edge = 1U;
	CHECK(drv_usb_device_reset(device) == ENODEV);
	CHECK(device_is_quarantined(device));
	reset_index = event_index(&controller, RECOVERY_EVENT_ROOT_RESET, 0U);
	CHECK(reset_index >= 0);
	CHECK(event_index(&controller, RECOVERY_EVENT_DEVICE_ENABLE,
	    (unsigned)reset_index + 1U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_device_enable = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_SET_ADDRESS, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_set_address = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_SET_CONFIGURATION, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_set_configuration = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_ENDPOINT_ENABLE, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	CHECK(drv_usb_interface_set_alternate(
	    drv_usb_configuration_find_interface(
	    drv_usb_device_active_configuration(device), 0U), 1U) == 0);
	controller.event_count = 0U;
	controller.fail_set_interface = EIO;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_SET_INTERFACE, 0U) >= 0);
	CHECK(event_index(&controller, RECOVERY_EVENT_ENDPOINT_ENABLE, 0U) < 0);
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_endpoint_enable_address = 0x02U;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	unregister_controller(&controller);

	device = register_controller(&controller);
	controller.event_count = 0U;
	controller.fail_endpoint_reset_address = 0x81U;
	CHECK(drv_usb_device_reset(device) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(event_index(&controller, RECOVERY_EVENT_ENDPOINT_RESET, 0U) >= 0);
	unregister_controller(&controller);
}

static struct drv_usb_urb *
create_stalled_urb(struct fake_controller *controller,
	struct drv_usb_device *device, struct drv_usb_endpoint **endpoint_out)
{
	struct drv_usb_endpoint *endpoint = find_endpoint(device, 0U, 0x81U);
	struct drv_usb_urb *urb;
	unsigned char byte = 0;

	CHECK(endpoint != NULL);
	urb = drv_usb_urb_alloc(device, endpoint, 0);
	CHECK(urb != NULL);
	CHECK(drv_usb_urb_setup(urb, &byte, sizeof(byte), 0, 0, NULL, NULL) == 0);
	controller->data_behavior = DATA_STALL;
	CHECK(drv_usb_urb_submit(urb) == 0);
	CHECK(drv_usb_urb_drain(urb, 0) == 0);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	*endpoint_out = endpoint;
	return urb;
}

static void
exercise_ambiguous_clear_failures(void)
{
	struct fake_controller controller;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	unsigned reset_before;

	device = register_controller(&controller);
	urb = create_stalled_urb(&controller, device, &endpoint);
	controller.clear_behavior = CONTROL_IO_ERROR;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EIO);
	CHECK(device_is_quarantined(device));
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	drv_usb_urb_free(urb);
	unregister_controller(&controller);

	device = register_controller(&controller);
	urb = create_stalled_urb(&controller, device, &endpoint);
	controller.clear_behavior = CONTROL_TIMEOUT;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == ETIMEDOUT);
	CHECK(device_is_quarantined(device));
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	drv_usb_urb_free(urb);
	unregister_controller(&controller);

	/* Once the wire request succeeds, host reset failure retains the core latch
	 * and quarantines rather than publishing a half-reset endpoint. */
	device = register_controller(&controller);
	urb = create_stalled_urb(&controller, device, &endpoint);
	reset_before = controller.endpoint_reset_count;
	controller.fail_endpoint_reset_address = 0x81U;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == EIO);
	CHECK(controller.endpoint_reset_count == reset_before + 1U);
	CHECK(device_is_quarantined(device));
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	drv_usb_urb_free(urb);
	unregister_controller(&controller);

	/* Terminal disconnect wins after the accepted wire request.  The operation
	 * unwinds its own counts but never reopens lifecycle gates closed by teardown. */
	device = register_controller(&controller);
	urb = create_stalled_urb(&controller, device, &endpoint);
	reset_before = controller.endpoint_reset_count;
	controller.clear_behavior = CONTROL_DISCONNECT;
	CHECK(drv_usb_endpoint_clear_halt(endpoint) == ENODEV);
	CHECK(controller.endpoint_reset_count == reset_before);
	CHECK(device_is_disconnecting(device));
	CHECK((atomic_load_acquire(&device->binding_transactions) &
	    USB_IO_GATE_CLOSED) != 0);
	CHECK((atomic_load_acquire(&device->submit_gate) &
	    USB_IO_GATE_CLOSED) != 0);
	CHECK(atomic_load_acquire(&endpoint->halted) != 0);
	drv_usb_urb_free(urb);
	unregister_controller(&controller);
}

static void
exercise_endpoint_reset_registration_requirement(void)
{
	struct drv_usb_hcd_ops incomplete_ops = fake_ops;
	struct drv_usb_hcd incomplete_hcd;
	struct drv_usb_bus *bus = NULL;

	memset(&incomplete_hcd, 0, sizeof(incomplete_hcd));
	incomplete_ops.endpoint_reset = NULL;
	incomplete_hcd.name = "missing-endpoint-reset-fixture";
	incomplete_hcd.ops = &incomplete_ops;
	incomplete_hcd.root_port_count = 1U;
	CHECK(drv_usb_hcd_register(&incomplete_hcd, &bus) == EINVAL);
	CHECK(bus == NULL);
}

int
main(void)
{
	struct fake_controller controller;
	struct drv_usb_device *device;
	size_t baseline, driver_baseline;

	CHECK(drv_usb_init() == 0);
	exercise_endpoint_reset_registration_requirement();
	driver_baseline = atomic_load_explicit(&live_allocations,
	    memory_order_relaxed);
	CHECK(drv_usb_driver_register(&recovery_primary_driver) == 0);
	CHECK(drv_usb_driver_register(&recovery_secondary_driver) == 0);
	baseline = atomic_load_explicit(&live_allocations, memory_order_relaxed);
	set_binding_mode(RECOVERY_BIND_CLAIM_SIBLING);
	device = register_controller(&controller);
	CHECK(device->recovery_urb != NULL);
	exercise_selection_reset_semantics(&controller, device);
	exercise_endpoint_recovery(&controller, device);
	exercise_successful_device_reset(&controller, device);
	unregister_controller(&controller);
	CHECK(atomic_load_explicit(&live_allocations, memory_order_relaxed) ==
	    baseline);
	exercise_multiple_owner_preflight();
	exercise_ambiguous_clear_failures();
	exercise_reset_failures();
	CHECK(atomic_load_explicit(&live_allocations, memory_order_relaxed) ==
	    baseline);
	CHECK(drv_usb_driver_unregister(&recovery_secondary_driver) == 0);
	CHECK(drv_usb_driver_unregister(&recovery_primary_driver) == 0);
	CHECK(atomic_load_explicit(&live_allocations, memory_order_relaxed) ==
	    driver_baseline);
	CHECK(atomic_load_explicit(&forbidden_allocations,
	    memory_order_relaxed) == 0U);
	printf("usb recovery contract: %u checks passed\n",
	    atomic_load_explicit(&checks, memory_order_relaxed));
	return 0;
}
