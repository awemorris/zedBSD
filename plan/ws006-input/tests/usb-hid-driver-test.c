/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/usb.h>
#include <kern/input-device.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct drv_usb_bus {
	unsigned number;
};
struct drv_usb_device {
	struct drv_usb_bus *bus;
	struct drv_usb_device_descriptor descriptor;
	enum drv_usb_speed speed;
	unsigned address;
	unsigned port;
};
struct drv_usb_host_interface {
	struct drv_usb_interface_descriptor descriptor;
	const uint8_t *extra;
	size_t extra_length;
};
struct drv_usb_endpoint {
	struct drv_usb_device *device;
	struct drv_usb_endpoint_descriptor descriptor;
	struct drv_usb_superspeed_endpoint_companion_descriptor companion;
	int companion_valid;
};
struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_host_interface *alternate;
	struct drv_usb_endpoint *endpoint;
	void *driver_data;
	unsigned number;
};
struct drv_usb_urb {
	enum drv_usb_urb_status status;
	size_t actual;
};
struct input_device {
	unsigned unused;
};

/* The production driver is compiled in this host fixture, but scheduler and
 * locking paths are section-collected.  Keep host libc's sigset_t out of the
 * kernel thread header by supplying the minimal compile-time surface here. */
#define ZEDBSD_KERN_LOCK_H
#define ZEDBSD_KERN_SCHED_H
#define ZEDBSD_KERN_THREAD_H
#define LOCK_RANK_DEVICE 100
#define SCHED_PRIORITY_DEFAULT 8
#define THREAD_ZOMBIE 5
struct spinlock {
	unsigned unused;
};
struct thread {
	void *task;
	unsigned state;
};
static struct thread *curthread;
static inline unsigned long spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}
static inline void spin_unlock_irqrestore(struct spinlock *lock,
	unsigned long irq)
{
	(void)lock;
	(void)irq;
}
static inline void spin_init(struct spinlock *lock, int rank, const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}
static inline unsigned atomic_raw_load_acquire(volatile unsigned *value)
{
	return *value;
}
void sched_yield(void);
void kernel_wait_task(void);
void kernel_notify_task(void *);
int kthread_create(void (*)(void *), void *, int, struct thread **);
void thread_start(struct thread *);
int thread_wait(struct thread *, void **);

#include "../../../src/drivers/usb-hid.c"

static size_t checks;
static struct input_event emitted[64];
static size_t emitted_count;
static unsigned control_count;
static uint8_t control_type, control_request;
static uint16_t control_value, control_index;
static struct drv_usb_urb fake_urb;
static const uint8_t *control_descriptor;
static size_t control_descriptor_length;
static int urb_submit_error;
static int input_register_error;
static unsigned input_register_count, input_unregister_count;
static size_t live_hal_allocations;

#define CHECK(expression)                                                     \
	do {                                                                    \
		checks++;                                                       \
		if (!(expression)) {                                            \
			fprintf(stderr, "usb-hid-driver-test:%u: %s\n",          \
			    __LINE__, #expression);                               \
			exit(1);                                                  \
		}                                                               \
	} while (0)

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

void *
hal_malloc(size_t size)
{
	void *result = malloc(size);

	if (result != NULL)
		live_hal_allocations++;
	return result;
}

void
hal_free(void *pointer)
{
	if (pointer != NULL) {
		CHECK(live_hal_allocations != 0U);
		live_hal_allocations--;
	}
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

struct drv_usb_bus *
drv_usb_device_bus(const struct drv_usb_device *device)
{
	return device->bus;
}

unsigned
drv_usb_bus_number(const struct drv_usb_bus *bus)
{
	return bus->number;
}

unsigned
drv_usb_device_address(const struct drv_usb_device *device)
{
	return device->address;
}

unsigned
drv_usb_device_port(const struct drv_usb_device *device)
{
	return device->port;
}

const struct drv_usb_device_descriptor *
drv_usb_device_descriptor(const struct drv_usb_device *device)
{
	return &device->descriptor;
}

int
drv_usb_device_get_string(struct drv_usb_device *device, unsigned index,
	unsigned language, char *buffer, size_t capacity)
{
	(void)device;
	(void)index;
	(void)language;
	if (capacity != 0U)
		buffer[0] = '\0';
	return ENOENT;
}

const struct drv_usb_host_interface *
drv_usb_interface_active_alternate(const struct drv_usb_interface *interface)
{
	return interface->alternate;
}

unsigned
drv_usb_host_interface_extra_count(const struct drv_usb_host_interface *host)
{
	return host->extra != NULL ? 1U : 0U;
}

int
drv_usb_host_interface_extra(const struct drv_usb_host_interface *host,
	unsigned index, const void **descriptor, size_t *length)
{
	if (index != 0U || host->extra == NULL)
		return ENOENT;
	*descriptor = host->extra;
	*length = host->extra_length;
	return 0;
}

struct drv_usb_device *
drv_usb_interface_device(const struct drv_usb_interface *interface)
{
	return interface->device;
}

const struct drv_usb_interface_descriptor *
drv_usb_interface_descriptor(const struct drv_usb_interface *interface)
{
	return &interface->alternate->descriptor;
}

unsigned
drv_usb_interface_number(const struct drv_usb_interface *interface)
{
	return interface->number;
}

struct drv_usb_endpoint *
drv_usb_interface_find_endpoint(struct drv_usb_interface *interface,
	enum drv_usb_transfer_type type, uint8_t direction,
	struct drv_usb_endpoint *after)
{
	if (type != DRV_USB_TRANSFER_INTERRUPT || direction != DRV_USB_DIR_IN ||
	    after != NULL)
		return NULL;
	return interface->endpoint;
}

const struct drv_usb_endpoint_descriptor *
drv_usb_endpoint_descriptor(const struct drv_usb_endpoint *endpoint)
{
	return &endpoint->descriptor;
}

uint16_t
drv_usb_endpoint_max_packet_size(const struct drv_usb_endpoint *endpoint)
{
	return endpoint->descriptor.maximum_packet_size;
}

const struct drv_usb_superspeed_endpoint_companion_descriptor *
drv_usb_endpoint_superspeed_companion(const struct drv_usb_endpoint *endpoint)
{
	return endpoint->companion_valid ? &endpoint->companion : NULL;
}

uint8_t
drv_usb_endpoint_maximum_burst(const struct drv_usb_endpoint *endpoint)
{
	return endpoint->companion_valid ? endpoint->companion.maximum_burst : 0;
}

enum drv_usb_speed
drv_usb_device_speed(const struct drv_usb_device *device)
{
	return device->speed;
}

int
drv_usb_control(struct drv_usb_device *device, uint8_t type, uint8_t request,
	uint16_t value, uint16_t index, void *buffer, size_t length,
	unsigned timeout, size_t *actual)
{
	(void)device;
	(void)buffer;
	(void)timeout;
	control_count++;
	control_type = type;
	control_request = request;
	control_value = value;
	control_index = index;
	if (request == USB_REQUEST_GET_DESCRIPTOR) {
		if (control_descriptor == NULL ||
		    length != control_descriptor_length)
			return EIO;
		memcpy(buffer, control_descriptor, length);
	}
	if (actual != NULL)
		*actual = length;
	return 0;
}

void *
drv_usb_interface_driver_data(const struct drv_usb_interface *interface)
{
	return interface->driver_data;
}

int
drv_usb_interface_set_driver_data(struct drv_usb_interface *interface,
	void *data)
{
	interface->driver_data = data;
	return 0;
}

uint8_t
drv_usb_endpoint_address(const struct drv_usb_endpoint *endpoint)
{
	return endpoint->descriptor.address;
}

int
drv_usb_endpoint_clear_halt(struct drv_usb_endpoint *endpoint)
{
	(void)endpoint;
	return 0;
}

struct drv_usb_urb *
drv_usb_urb_alloc(struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint, unsigned iso_count)
{
	(void)device;
	(void)endpoint;
	(void)iso_count;
	memset(&fake_urb, 0, sizeof(fake_urb));
	return &fake_urb;
}

void
drv_usb_urb_free(struct drv_usb_urb *urb)
{
	(void)urb;
}

int
drv_usb_urb_setup(struct drv_usb_urb *urb, void *buffer, size_t length,
	unsigned flags, unsigned timeout, drv_usb_urb_callback_t callback,
	void *argument)
{
	(void)buffer;
	(void)length;
	(void)flags;
	(void)timeout;
	(void)callback;
	(void)argument;
	urb->status = DRV_USB_URB_IDLE;
	return 0;
}

int
drv_usb_urb_submit(struct drv_usb_urb *urb)
{
	if (urb_submit_error != 0)
		return urb_submit_error;
	urb->status = DRV_USB_URB_PENDING;
	return 0;
}

int
drv_usb_urb_cancel(struct drv_usb_urb *urb)
{
	urb->status = DRV_USB_URB_CANCELLED;
	return 0;
}

int
drv_usb_urb_drain(struct drv_usb_urb *urb, unsigned timeout)
{
	(void)urb;
	(void)timeout;
	return 0;
}

enum drv_usb_urb_status
drv_usb_urb_status(const struct drv_usb_urb *urb)
{
	return urb->status;
}

size_t
drv_usb_urb_actual_length(const struct drv_usb_urb *urb)
{
	return urb->actual;
}

int
drv_usb_driver_register(struct drv_usb_driver *driver)
{
	(void)driver;
	return 0;
}

int
input_device_register(const struct input_device_info *info,
	struct input_device **result)
{
	static struct input_device input;

	(void)info;
	input_register_count++;
	if (input_register_error != 0)
		return input_register_error;
	*result = &input;
	return 0;
}

void
input_device_unregister(struct input_device *device)
{
	(void)device;
	input_unregister_count++;
}

void
sched_yield(void)
{
}

void
kernel_wait_task(void)
{
}

void
kernel_notify_task(void *task)
{
	(void)task;
}

int
kthread_create(void (*entry)(void *), void *argument, int priority,
	struct thread **result)
{
	static struct thread thread;

	(void)entry;
	(void)argument;
	(void)priority;
	*result = &thread;
	return 0;
}

void
thread_start(struct thread *thread)
{
	thread->state = THREAD_ZOMBIE;
}

int
thread_wait(struct thread *thread, void **status)
{
	(void)thread;
	(void)status;
	return 0;
}

void
input_device_emit(struct input_device *device, uint16_t type, uint16_t code,
	int32_t value)
{
	(void)device;
	CHECK(emitted_count < sizeof(emitted) / sizeof(emitted[0]));
	emitted[emitted_count].type = type;
	emitted[emitted_count].code = code;
	emitted[emitted_count].value = value;
	emitted_count++;
}

static const uint8_t keyboard_descriptor[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
	0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x15, 0x00, 0x25, 0x65, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
	0xc0
};

static const uint8_t mouse_descriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
	0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
	0xc0
};

static void
test_descriptor_and_endpoint(void)
{
	uint8_t hid_descriptor[] = { 9, 0x21, 0x11, 0x01, 0, 1, 0x22,
	    sizeof(keyboard_descriptor), 0 };
	struct drv_usb_bus bus = { .number = 2 };
	struct drv_usb_device device = { .bus = &bus,
	    .speed = DRV_USB_SPEED_LOW };
	struct drv_usb_host_interface host = {
	    .descriptor = { .interface_class = USB_HID_CLASS,
	    .interface_subclass = 1, .interface_protocol = 1 },
	    .extra = hid_descriptor, .extra_length = sizeof(hid_descriptor) };
	struct drv_usb_endpoint endpoint = { .device = &device,
	    .descriptor = { .address = 0x81, .attributes = 3,
	    .maximum_packet_size = 8, .interval = 10 } };
	struct drv_usb_interface interface = { .device = &device,
	    .alternate = &host, .endpoint = &endpoint, .number = 4 };
	size_t length = 0, capacity = 0;
	struct usb_hid hid;

	CHECK(usb_hid_report_descriptor_length(&interface, &length) == 0);
	CHECK(length == sizeof(keyboard_descriptor));
	CHECK(usb_hid_endpoint_capacity(&interface, &endpoint, &capacity) == 0);
	CHECK(capacity == 8U);
	endpoint.descriptor.interval = 0;
	CHECK(usb_hid_endpoint_capacity(&interface, &endpoint, &capacity) ==
	    EINVAL);
	endpoint.descriptor.interval = 10;
	endpoint.descriptor.maximum_packet_size = 9;
	CHECK(usb_hid_endpoint_capacity(&interface, &endpoint, &capacity) ==
	    EINVAL);
	endpoint.descriptor.maximum_packet_size = 8;
	memset(&hid, 0, sizeof(hid));
	hid.interface = &interface;
	hid.device = &device;
	control_count = 0;
	CHECK(usb_hid_set_report_protocol(&hid) == 0);
	CHECK(control_count == 1U);
	CHECK(control_type == (DRV_USB_REQUEST_CLASS |
	    DRV_USB_RECIP_INTERFACE));
	CHECK(control_request == USB_HID_REQUEST_SET_PROTOCOL);
	CHECK(control_value == USB_HID_PROTOCOL_REPORT);
	CHECK(control_index == 4U);
	host.descriptor.interface_subclass = 0;
	CHECK(usb_hid_set_report_protocol(&hid) == 0);
	CHECK(control_count == 1U);
	hid_descriptor[7] = 0;
	hid_descriptor[8] = 0;
	CHECK(usb_hid_report_descriptor_length(&interface, &length) == EINVAL);
}

static void
prepare_reports(struct usb_hid *hid, const uint8_t *descriptor, size_t length,
	struct input_device *input)
{
	struct hid_report_layout_info info;
	struct hid_report_report_info report;

	memset(hid, 0, sizeof(*hid));
	CHECK(hid_report_layout_parse(descriptor, length, &hid->layout) == 0);
	CHECK(hid_report_layout_get_info(hid->layout, &info) == 0);
	CHECK(info.report_count == 1U);
	CHECK(hid_report_layout_get_report(hid->layout, 0, &report) == 0);
	hid->report_count = 1;
	hid->reports[0].id = report.report_id;
	hid->input = input;
}

static void
test_keyboard_diff(void)
{
	struct usb_hid hid;
	struct input_device input;
	uint8_t press[8] = { 0x02, 0, 0x04, 0, 0, 0, 0, 0 };
	uint8_t release[8] = { 0 };
	uint8_t rollover[8] = { 0, 0, 1, 0, 0, 0, 0, 0 };

	prepare_reports(&hid, keyboard_descriptor, sizeof(keyboard_descriptor),
	    &input);
	emitted_count = 0;
	usb_hid_publish_report(&hid, press, sizeof(press));
	CHECK(emitted_count == 3U);
	CHECK(emitted[0].type == EV_KEY && emitted[0].code == KEY_A &&
	    emitted[0].value == 1);
	CHECK(emitted[1].type == EV_KEY &&
	    emitted[1].code == KEY_LEFTSHIFT && emitted[1].value == 1);
	CHECK(emitted[2].type == EV_SYN && emitted[2].code == SYN_REPORT);
	emitted_count = 0;
	usb_hid_publish_report(&hid, press, sizeof(press));
	CHECK(emitted_count == 0U);
	usb_hid_publish_report(&hid, rollover, sizeof(rollover));
	CHECK(emitted_count == 0U);
	usb_hid_publish_report(&hid, release, sizeof(release));
	CHECK(emitted_count == 3U);
	CHECK(emitted[0].code == KEY_A && emitted[0].value == 0);
	CHECK(emitted[1].code == KEY_LEFTSHIFT && emitted[1].value == 0);
	CHECK(emitted[2].type == EV_SYN);
	hid_report_layout_destroy(hid.layout);
}

static void
test_mouse_report(void)
{
	struct usb_hid hid;
	struct input_device input;
	uint8_t report[] = { 1, 5, (uint8_t)-3, 1 };
	uint8_t release[] = { 0, 0, 0, 0 };

	prepare_reports(&hid, mouse_descriptor, sizeof(mouse_descriptor), &input);
	emitted_count = 0;
	usb_hid_publish_report(&hid, report, sizeof(report));
	CHECK(emitted_count == 5U);
	CHECK(emitted[0].type == EV_KEY && emitted[0].code == BTN_LEFT &&
	    emitted[0].value == 1);
	CHECK(emitted[1].type == EV_REL && emitted[1].code == REL_X &&
	    emitted[1].value == 5);
	CHECK(emitted[2].type == EV_REL && emitted[2].code == REL_Y &&
	    emitted[2].value == -3);
	CHECK(emitted[3].type == EV_REL && emitted[3].code == REL_WHEEL &&
	    emitted[3].value == 1);
	CHECK(emitted[4].type == EV_SYN);
	emitted_count = 0;
	usb_hid_publish_report(&hid, release, sizeof(release));
	CHECK(emitted_count == 2U);
	CHECK(emitted[0].type == EV_KEY && emitted[0].code == BTN_LEFT &&
	    emitted[0].value == 0);
	CHECK(emitted[1].type == EV_SYN);
	hid_report_layout_destroy(hid.layout);
}

static void
test_runtime_failure_unpublishes(void)
{
	struct usb_hid hid;
	struct input_device input;
	struct drv_usb_interface interface = { .number = 7U };
	unsigned before = input_unregister_count;

	memset(&hid, 0, sizeof(hid));
	memset(&input, 0, sizeof(input));
	spin_init(&hid.lock, LOCK_RANK_DEVICE, "USB HID runtime failure");
	hid.interface = &interface;
	hid.input = &input;
	hid.active = 1U;
	usb_hid_runtime_stop(&hid, "fixture rearm", EIO, 0);
	CHECK(hid.stopping == 1U);
	CHECK(hid.active == 0U);
	CHECK(hid.input == NULL);
	CHECK(input_unregister_count == before + 1U);
	usb_hid_unpublish(&hid);
	CHECK(input_unregister_count == before + 1U);
}

static void
reset_attach_mocks(void)
{
	control_count = 0;
	control_descriptor = NULL;
	control_descriptor_length = 0;
	urb_submit_error = 0;
	input_register_error = 0;
	input_register_count = 0;
	input_unregister_count = 0;
	memset(&fake_urb, 0, sizeof(fake_urb));
	usb_hid_pending = NULL;
	usb_hid_input_is_ready = 0;
	usb_hid_registered = 0;
}

static void
test_attach_activation_and_unwind(void)
{
	struct usb_hid stopped_generation;
	uint8_t hid_descriptor[] = { 9, 0x21, 0x11, 0x01, 0, 1, 0x22,
	    sizeof(keyboard_descriptor), 0 };
	struct drv_usb_bus bus = { .number = 3 };
	struct drv_usb_device device = { .bus = &bus,
	    .descriptor = { .vendor = 0x1234, .product = 0x5678,
	    .device_release = 0x0102 }, .speed = DRV_USB_SPEED_LOW,
	    .address = 5, .port = 2 };
	struct drv_usb_host_interface host = {
	    .descriptor = { .interface_class = USB_HID_CLASS,
	    .interface_subclass = 1, .interface_protocol = 1 },
	    .extra = hid_descriptor, .extra_length = sizeof(hid_descriptor) };
	struct drv_usb_endpoint endpoint = { .device = &device,
	    .descriptor = { .address = 0x81, .attributes = 3,
	    .maximum_packet_size = 8, .interval = 10 } };
	struct drv_usb_interface interface = { .device = &device,
	    .alternate = &host, .endpoint = &endpoint, .number = 0 };

	reset_attach_mocks();
	control_descriptor = keyboard_descriptor;
	control_descriptor_length = sizeof(keyboard_descriptor);
	CHECK(drv_usb_hid_driver_register() == 0);
	CHECK(usb_hid_attach(&interface, &usb_hid_ids[0]) == 0);
	CHECK(interface.driver_data != NULL);
	CHECK(input_register_count == 0U);
	CHECK(usb_hid_pending != NULL);
	/* A detached/stopping generation can be the list head while a later
	 * interface remains activatable.  Skipping it must not terminate the
	 * pending-list drain. */
	memset(&stopped_generation, 0, sizeof(stopped_generation));
	spin_init(&stopped_generation.lock, LOCK_RANK_DEVICE,
	    "stopped usb hid fixture");
	stopped_generation.stopping = 1U;
	stopped_generation.pending = 1U;
	stopped_generation.pending_next = usb_hid_pending;
	usb_hid_pending = &stopped_generation;
	drv_usb_hid_input_ready();
	CHECK(input_register_count == 1U);
	CHECK(stopped_generation.pending == 0U);
	CHECK(usb_hid_pending == NULL);
	CHECK(fake_urb.status == DRV_USB_URB_PENDING);
	CHECK(usb_hid_detach(&interface, 0) == 0);
	CHECK(interface.driver_data == NULL);
	CHECK(input_unregister_count == 1U);
	CHECK(live_hal_allocations == 0U);

	/* An initial submission failure unpublishes the just-created generation
	 * and unwinds every allocation instead of leaving a dead event node. */
	reset_attach_mocks();
	control_descriptor = keyboard_descriptor;
	control_descriptor_length = sizeof(keyboard_descriptor);
	usb_hid_input_is_ready = 1;
	urb_submit_error = EIO;
	CHECK(usb_hid_attach(&interface, &usb_hid_ids[0]) == EIO);
	CHECK(interface.driver_data == NULL);
	CHECK(input_register_count == 1U);
	CHECK(input_unregister_count == 1U);
	CHECK(live_hal_allocations == 0U);

	/* General HID uses Report Protocol intrinsically: only GET_DESCRIPTOR is
	 * sent, while Boot interfaces additionally receive SET_PROTOCOL. */
	reset_attach_mocks();
	host.descriptor.interface_subclass = 0;
	control_descriptor = keyboard_descriptor;
	control_descriptor_length = sizeof(keyboard_descriptor);
	usb_hid_input_is_ready = 1;
	CHECK(usb_hid_attach(&interface, &usb_hid_ids[0]) == 0);
	CHECK(control_count == 1U);
	CHECK(control_request == USB_REQUEST_GET_DESCRIPTOR);
	CHECK(usb_hid_detach(&interface, 0) == 0);
	CHECK(live_hal_allocations == 0U);
}

int
main(void)
{
	test_descriptor_and_endpoint();
	test_keyboard_diff();
	test_mouse_report();
	test_runtime_failure_unpublishes();
	test_attach_activation_and_unwind();
	printf("usb-hid-driver-test: PASS (%zu checks)\n", checks);
	return 0;
}
