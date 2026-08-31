/*
 * USB Human Interface Device input driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/hid/hid-report.h>
#include <drivers/usb-hid.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/input-device.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define USB_HID_CLASS 0x03U
#define USB_HID_DESCRIPTOR 0x21U
#define USB_HID_REPORT_DESCRIPTOR 0x22U
#define USB_REQUEST_GET_DESCRIPTOR 0x06U
#define USB_HID_REQUEST_SET_PROTOCOL 0x0bU
#define USB_HID_PROTOCOL_REPORT 1U
#define USB_HID_CONTROL_TIMEOUT_MS 1000U
#define USB_HID_DRAIN_TIMEOUT_MS 5000U
/* input_device_register() accepts at most 63 bytes plus NUL. */
#define USB_HID_TEXT_MAX 64U
#define USB_HID_ERROR_MARKERS 16U
#define USB_HID_WORK_ARM (1U << 0)
#define USB_HID_WORK_COMPLETE (1U << 1)

struct usb_hid_report_state {
	uint8_t id;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
};

struct usb_hid {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	struct hid_report_layout *layout;
	struct input_device *input;
	struct thread *worker;
	struct spinlock lock;
	struct usb_hid *pending_next;
	uint8_t *buffer;
	size_t buffer_size;
	struct input_capability capabilities[HID_REPORT_FIELD_COUNT_MAX + 1U];
	struct input_abs_axis absolute_axes[ABS_MAX + 1U];
	struct usb_hid_report_state reports[HID_REPORT_ID_COUNT_MAX];
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	size_t capability_count;
	size_t absolute_axis_count;
	size_t report_count;
	unsigned work_pending;
	unsigned stopping;
	unsigned submit_active;
	unsigned activating;
	unsigned active;
	unsigned pending;
	unsigned error_markers;
	char name[USB_HID_TEXT_MAX];
	char physical_path[USB_HID_TEXT_MAX];
	char unique_id[USB_HID_TEXT_MAX];
};

static struct spinlock usb_hid_pending_lock;
static struct usb_hid *usb_hid_pending;
static unsigned usb_hid_input_is_ready;
static unsigned usb_hid_registered;

static uint16_t
usb_hid_le16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static int
usb_hid_report_descriptor_length(struct drv_usb_interface *interface,
	size_t *result)
{
	const struct drv_usb_host_interface *alternate;
	unsigned count, index;
	size_t found = 0;

	alternate = drv_usb_interface_active_alternate(interface);
	if (alternate == NULL || result == NULL)
		return EINVAL;
	count = drv_usb_host_interface_extra_count(alternate);
	for (index = 0; index < count; index++) {
		const uint8_t *descriptor;
		size_t length, entries, entry;
		int error;

		error = drv_usb_host_interface_extra(alternate, index,
		    (const void **)&descriptor, &length);
		if (error != 0)
			return error;
		if (length < 2U || descriptor[0] != length ||
		    descriptor[1] != USB_HID_DESCRIPTOR)
			continue;
		if (length < 6U)
			return EINVAL;
		entries = descriptor[5];
		if (entries == 0U || entries > (length - 6U) / 3U ||
		    6U + entries * 3U != length)
			return EINVAL;
		for (entry = 0; entry < entries; entry++) {
			const uint8_t *subordinate = descriptor + 6U + entry * 3U;
			size_t report_length;

			if (subordinate[0] != USB_HID_REPORT_DESCRIPTOR)
				continue;
			report_length = usb_hid_le16(subordinate + 1U);
			if (report_length == 0U ||
			    report_length > HID_REPORT_DESCRIPTOR_SIZE_MAX ||
			    found != 0U)
				return EINVAL;
			found = report_length;
		}
	}
	if (found == 0U)
		return ENOENT;
	*result = found;
	return 0;
}

static int
usb_hid_endpoint_capacity(struct drv_usb_interface *interface,
	struct drv_usb_endpoint *endpoint, size_t *result)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	const struct drv_usb_superspeed_endpoint_companion_descriptor *companion;
	enum drv_usb_speed speed;
	uint16_t maximum;
	unsigned payload, packets;
	size_t capacity;

	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL || descriptor->interval == 0U || result == NULL)
		return EINVAL;
	maximum = drv_usb_endpoint_max_packet_size(endpoint);
	payload = maximum & 0x07ffU;
	packets = 1U + ((maximum >> 11U) & 3U);
	speed = drv_usb_device_speed(drv_usb_interface_device(interface));
	if (payload == 0U || (maximum & 0xe000U) != 0U || packets == 4U)
		return EINVAL;
	if (speed == DRV_USB_SPEED_LOW) {
		if (payload > 8U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_FULL) {
		if (payload > 64U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_HIGH) {
		if (payload > 1024U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_SUPER ||
	    speed == DRV_USB_SPEED_SUPER_PLUS) {
		if (payload > 1024U || packets != 1U)
			return EINVAL;
		companion = drv_usb_endpoint_superspeed_companion(endpoint);
		capacity = (size_t)payload *
		    ((size_t)drv_usb_endpoint_maximum_burst(endpoint) + 1U);
		if (companion != NULL && companion->bytes_per_interval != 0U) {
			if (companion->bytes_per_interval > capacity)
				return EINVAL;
			capacity = companion->bytes_per_interval;
		}
		*result = capacity;
		return 0;
	} else {
		return EINVAL;
	}
	*result = (size_t)payload * packets;
	return 0;
}

static int
usb_hid_find_endpoint(struct drv_usb_interface *interface,
	struct drv_usb_endpoint **result)
{
	struct drv_usb_endpoint *endpoint, *extra;

	endpoint = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	if (endpoint == NULL)
		return ENODEV;
	extra = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, endpoint);
	if (extra != NULL)
		return EOPNOTSUPP;
	*result = endpoint;
	return 0;
}

static int
usb_hid_fetch_layout(struct usb_hid *hid)
{
	struct hid_report_layout_info info;
	uint8_t *descriptor;
	size_t descriptor_length, actual = 0, index, capacity;
	size_t maximum_report = 0;
	int error;

	error = usb_hid_report_descriptor_length(hid->interface,
	    &descriptor_length);
	if (error != 0)
		return error;
	descriptor = hal_malloc(descriptor_length);
	if (descriptor == NULL)
		return ENOMEM;
	error = drv_usb_control(hid->device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
	    DRV_USB_RECIP_INTERFACE, USB_REQUEST_GET_DESCRIPTOR,
	    (uint16_t)(USB_HID_REPORT_DESCRIPTOR << 8U),
	    (uint16_t)drv_usb_interface_number(hid->interface), descriptor,
	    descriptor_length, USB_HID_CONTROL_TIMEOUT_MS, &actual);
	if (error == 0 && actual != descriptor_length)
		error = EIO;
	if (error == 0)
		error = hid_report_layout_parse(descriptor, descriptor_length,
		    &hid->layout);
	hal_free(descriptor);
	if (error != 0)
		return error;
	error = hid_report_layout_get_info(hid->layout, &info);
	if (error != 0 || info.report_count == 0U ||
	    info.report_count > HID_REPORT_ID_COUNT_MAX ||
	    info.capability_count > HID_REPORT_FIELD_COUNT_MAX + 1U ||
	    info.absolute_axis_count > ABS_MAX + 1U)
		return error != 0 ? error : EINVAL;
	hid->report_count = info.report_count;
	hid->capability_count = info.capability_count;
	hid->absolute_axis_count = info.absolute_axis_count;
	for (index = 0; index < info.report_count; index++) {
		struct hid_report_report_info report;

		error = hid_report_layout_get_report(hid->layout, index, &report);
		if (error != 0 || report.minimum_size == 0U ||
		    report.minimum_size > HID_REPORT_BITS_MAX / 8U + 1U)
			return error != 0 ? error : EINVAL;
		hid->reports[index].id = report.report_id;
		if (report.minimum_size > maximum_report)
			maximum_report = report.minimum_size;
	}
	for (index = 0; index < info.capability_count; index++) {
		error = hid_report_layout_get_capability(hid->layout, index,
		    &hid->capabilities[index]);
		if (error != 0)
			return error;
	}
	for (index = 0; index < info.absolute_axis_count; index++) {
		error = hid_report_layout_get_absolute_axis(hid->layout, index,
		    &hid->absolute_axes[index]);
		if (error != 0)
			return error;
	}
	error = usb_hid_endpoint_capacity(hid->interface, hid->endpoint,
	    &capacity);
	if (error != 0)
		return error;
	if (maximum_report > capacity)
		return EOVERFLOW;
	hid->buffer_size = maximum_report;
	return 0;
}

static int
usb_hid_set_report_protocol(struct usb_hid *hid)
{
	const struct drv_usb_interface_descriptor *descriptor;
	size_t actual = 0;

	descriptor = drv_usb_interface_descriptor(hid->interface);
	if (descriptor == NULL)
		return EINVAL;
	/* Non-Boot interfaces already have exactly one Report Protocol. */
	if (descriptor->interface_subclass != 1U)
		return 0;
	return drv_usb_control(hid->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
	    USB_HID_REQUEST_SET_PROTOCOL, USB_HID_PROTOCOL_REPORT,
	    (uint16_t)drv_usb_interface_number(hid->interface), NULL, 0,
	    USB_HID_CONTROL_TIMEOUT_MS, &actual);
}

static int
usb_hid_has_capability(const struct usb_hid *hid, uint16_t type,
	uint16_t code)
{
	size_t index;

	for (index = 0; index < hid->capability_count; index++)
		if (hid->capabilities[index].type == type &&
		    hid->capabilities[index].code == code)
			return 1;
	return 0;
}

static void
usb_hid_identity(struct usb_hid *hid)
{
	const struct drv_usb_device_descriptor *descriptor;
	unsigned bus, address, port, interface_number;
	int error;

	descriptor = drv_usb_device_descriptor(hid->device);
	bus = drv_usb_bus_number(drv_usb_device_bus(hid->device));
	address = drv_usb_device_address(hid->device);
	port = drv_usb_device_port(hid->device);
	interface_number = drv_usb_interface_number(hid->interface);
	(void)snprintf(hid->physical_path, sizeof(hid->physical_path),
	    "usb%u/port%u/device%u/interface%u", bus, port, address,
	    interface_number);
	hid->unique_id[0] = '\0';
	if (descriptor->serial_string != 0U)
		(void)drv_usb_device_get_string(hid->device,
		    descriptor->serial_string, 0, hid->unique_id,
		    sizeof(hid->unique_id));
	hid->name[0] = '\0';
	error = descriptor->product_string == 0U ? ENOENT :
	    drv_usb_device_get_string(hid->device, descriptor->product_string, 0,
	    hid->name, sizeof(hid->name));
	if (error == 0 && hid->name[0] != '\0')
		return;
	if (usb_hid_has_capability(hid, EV_ABS, ABS_X))
		(void)snprintf(hid->name, sizeof(hid->name), "USB HID tablet");
	else if (usb_hid_has_capability(hid, EV_REL, REL_X))
		(void)snprintf(hid->name, sizeof(hid->name), "USB HID mouse");
	else
		(void)snprintf(hid->name, sizeof(hid->name), "USB HID keyboard");
}

static void
usb_hid_completion(struct drv_usb_urb *urb, void *argument)
{
	struct usb_hid *hid = argument;
	struct thread *worker;
	unsigned long irq;

	if (hid == NULL || urb != hid->urb)
		return;
	irq = spin_lock_irqsave(&hid->lock);
	hid->work_pending |= USB_HID_WORK_COMPLETE;
	worker = hid->worker;
	spin_unlock_irqrestore(&hid->lock, irq);
	if (worker != NULL)
		kernel_notify_task(worker->task);
}

static int
usb_hid_begin_submit(struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	int admitted = !hid->stopping && !hid->submit_active;

	if (admitted)
		hid->submit_active = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);
	return admitted ? 0 : EBUSY;
}

static void
usb_hid_end_submit(struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);

	if (!hid->submit_active)
		__builtin_trap();
	hid->submit_active = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);
}

static int
usb_hid_arm(struct usb_hid *hid)
{
	int error;

	error = usb_hid_begin_submit(hid);
	if (error != 0)
		return error;
	memset(hid->buffer, 0, hid->buffer_size);
	error = drv_usb_urb_setup(hid->urb, hid->buffer, hid->buffer_size,
	    DRV_USB_URB_SHORT_OK, 0, usb_hid_completion, hid);
	if (error == 0)
		error = drv_usb_urb_submit(hid->urb);
	usb_hid_end_submit(hid);
	return error;
}

static struct usb_hid_report_state *
usb_hid_report_state(struct usb_hid *hid, uint8_t report_id)
{
	size_t index;

	for (index = 0; index < hid->report_count; index++)
		if (hid->reports[index].id == report_id)
			return &hid->reports[index];
	return NULL;
}

static void
usb_hid_publish_report(struct usb_hid *hid, const uint8_t *buffer,
	size_t length)
{
	struct hid_report_input decoded;
	struct usb_hid_report_state *state;
	unsigned long current[INPUT_BIT_WORDS(KEY_MAX)];
	unsigned long aggregate[INPUT_BIT_WORDS(KEY_MAX)];
	size_t index;
	unsigned code;
	int error, emitted = 0;

	error = hid_report_decode(hid->layout, buffer, length, &decoded);
	if (error != 0) {
		if (hid->error_markers++ < USB_HID_ERROR_MARKERS)
			hal_printf("usb-hid: malformed input usb%u device=%u "
			    "interface=%u length=%u error=%d\n",
			    drv_usb_bus_number(drv_usb_device_bus(hid->device)),
			    drv_usb_device_address(hid->device),
			    drv_usb_interface_number(hid->interface),
			    (unsigned)length, error);
		return;
	}
	state = usb_hid_report_state(hid, decoded.report_id);
	if (state == NULL)
		return;
	memset(current, 0, sizeof(current));
	for (index = 0; index < decoded.value_count; index++) {
		const struct hid_report_value *value = &decoded.values[index];

		if (value->type == EV_KEY && value->code <= KEY_MAX)
			current[value->code / INPUT_BITS_PER_WORD] |=
			    1UL << (value->code % INPUT_BITS_PER_WORD);
	}
	if (!decoded.keyboard_error) {
		memcpy(state->held, current, sizeof(state->held));
		memset(aggregate, 0, sizeof(aggregate));
		for (index = 0; index < hid->report_count; index++) {
			size_t word;

			for (word = 0; word < INPUT_BIT_WORDS(KEY_MAX); word++)
				aggregate[word] |= hid->reports[index].held[word];
		}
		for (code = 0; code <= KEY_MAX; code++) {
			unsigned long bit = 1UL << (code % INPUT_BITS_PER_WORD);
			int old_value = (hid->held[code /
			    INPUT_BITS_PER_WORD] & bit) != 0;
			int new_value = (aggregate[code /
			    INPUT_BITS_PER_WORD] & bit) != 0;

			if (old_value == new_value)
				continue;
			input_device_emit(hid->input, EV_KEY, (uint16_t)code,
			    new_value);
			emitted = 1;
		}
		memcpy(hid->held, aggregate, sizeof(hid->held));
	}
	for (index = 0; index < decoded.value_count; index++) {
		const struct hid_report_value *value = &decoded.values[index];

		if (value->type == EV_KEY ||
		    (value->type == EV_REL && value->value == 0))
			continue;
		input_device_emit(hid->input, value->type, value->code,
		    value->value);
		emitted = 1;
	}
	if (emitted)
		input_device_emit(hid->input, EV_SYN, SYN_REPORT, 0);
}

static unsigned
usb_hid_take_work(struct usb_hid *hid, int *stopping)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	unsigned work = hid->work_pending;

	hid->work_pending = 0U;
	*stopping = hid->stopping != 0U;
	spin_unlock_irqrestore(&hid->lock, irq);
	return work;
}

static void
usb_hid_unpublish(struct usb_hid *hid)
{
	struct input_device *input;
	unsigned long irq;

	irq = spin_lock_irqsave(&hid->lock);
	input = hid->input;
	hid->input = NULL;
	hid->active = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);
	if (input != NULL)
		input_device_unregister(input);
}

static void
usb_hid_runtime_stop(struct usb_hid *hid, const char *stage, int error,
	int transfer_status)
{
	unsigned long irq;
	int report;

	irq = spin_lock_irqsave(&hid->lock);
	hid->stopping = 1U;
	report = hid->error_markers++ < USB_HID_ERROR_MARKERS;
	spin_unlock_irqrestore(&hid->lock, irq);
	/* Remove a device which cannot be rearmed instead of leaving a visible
	 * event node that can never produce another report.  Detach joins this
	 * worker before attempting the same idempotent unpublication. */
	usb_hid_unpublish(hid);
	if (report) {
		if (transfer_status)
			hal_printf("usb-hid: terminal transfer stopped "
			    "interface=%u status=%d; input unpublished\n",
			    drv_usb_interface_number(hid->interface), error);
		else
			hal_printf("usb-hid: %s failed interface=%u error=%d; "
			    "input unpublished\n", stage,
			    drv_usb_interface_number(hid->interface), error);
	}
}

static void
usb_hid_worker(void *argument)
{
	struct usb_hid *hid = argument;

	for (;;) {
		unsigned work;
		int error, stopping;

		work = usb_hid_take_work(hid, &stopping);
		if (stopping)
			return;
		if (work == 0U) {
			kernel_wait_task();
			continue;
		}
		if ((work & USB_HID_WORK_COMPLETE) != 0U) {
			enum drv_usb_urb_status status;

			error = drv_usb_urb_drain(hid->urb,
			    USB_HID_DRAIN_TIMEOUT_MS);
			if (error != 0) {
				usb_hid_runtime_stop(hid, "completion drain", error, 0);
				return;
			}
			status = drv_usb_urb_status(hid->urb);
			if (status == DRV_USB_URB_COMPLETE) {
				usb_hid_publish_report(hid, hid->buffer,
				    drv_usb_urb_actual_length(hid->urb));
				work |= USB_HID_WORK_ARM;
			} else if (status == DRV_USB_URB_STALL) {
				error = drv_usb_endpoint_clear_halt(hid->endpoint);
				if (error == 0)
					work |= USB_HID_WORK_ARM;
				else {
					usb_hid_runtime_stop(hid, "clear-halt", error, 0);
					return;
				}
			} else if (status != DRV_USB_URB_CANCELLED &&
			    status != DRV_USB_URB_DISCONNECTED) {
				usb_hid_runtime_stop(hid, "terminal transfer",
				    (int)status, 1);
				return;
			}
		}
		if ((work & USB_HID_WORK_ARM) != 0U) {
			error = usb_hid_arm(hid);
			if (error != 0) {
				/* EBUSY is expected only after detach closes admission.  In
				 * that case detach owns publication; any other EBUSY is still
				 * a terminal always-on-URB contract failure. */
				unsigned long irq = spin_lock_irqsave(&hid->lock);
				int stopping_now = hid->stopping != 0U;

				spin_unlock_irqrestore(&hid->lock, irq);
				if (!stopping_now)
					usb_hid_runtime_stop(hid, "rearm", error, 0);
				return;
			}
		}
	}
}

static int
usb_hid_join_worker(struct usb_hid *hid)
{
	struct thread *worker = hid->worker;
	int error;

	if (worker == NULL)
		return 0;
	if (worker == curthread)
		return EBUSY;
	kernel_notify_task(worker->task);
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	    THREAD_ZOMBIE)
		sched_yield();
	error = thread_wait(worker, NULL);
	if (error == 0)
		hid->worker = NULL;
	return error;
}

static void
usb_hid_close_admission(struct usb_hid *hid)
{
	unsigned long irq;
	unsigned active;

	irq = spin_lock_irqsave(&hid->lock);
	hid->stopping = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);
	if (hid->worker != NULL)
		kernel_notify_task(hid->worker->task);
	for (;;) {
		irq = spin_lock_irqsave(&hid->lock);
		active = hid->submit_active || hid->activating;
		spin_unlock_irqrestore(&hid->lock, irq);
		if (!active)
			return;
		sched_yield();
	}
}

static int
usb_hid_activate(struct usb_hid *hid, int activation_claimed)
{
	const struct drv_usb_device_descriptor *usb_descriptor;
	struct input_device_info info;
	struct thread *worker;
	unsigned long irq;
	int error;

	if (!activation_claimed) {
		irq = spin_lock_irqsave(&hid->lock);
		if (hid->stopping || hid->active || hid->activating) {
			spin_unlock_irqrestore(&hid->lock, irq);
			return hid->active ? 0 : EBUSY;
		}
		hid->activating = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
	}
	error = kthread_create(usb_hid_worker, hid, SCHED_PRIORITY_DEFAULT,
	    &worker);
	if (error != 0)
		goto out;
	hid->worker = worker;
	usb_descriptor = drv_usb_device_descriptor(hid->device);
	memset(&info, 0, sizeof(info));
	info.name = hid->name;
	info.physical_path = hid->physical_path;
	info.unique_id = hid->unique_id;
	info.id.bustype = BUS_USB;
	info.id.vendor = usb_descriptor->vendor;
	info.id.product = usb_descriptor->product;
	info.id.version = usb_descriptor->device_release;
	info.capabilities = hid->capabilities;
	info.capability_count = hid->capability_count;
	info.absolute_axes = hid->absolute_axes;
	info.absolute_axis_count = hid->absolute_axis_count;
	error = input_device_register(&info, &hid->input);
	if (error != 0) {
		irq = spin_lock_irqsave(&hid->lock);
		hid->stopping = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
		thread_start(worker);
		(void)usb_hid_join_worker(hid);
		goto out;
	}
	/* The first accepted request is part of the attach transaction.  A
	 * publication which can never receive a report is not a successful HID
	 * attachment.  Synchronous completion is safe: its callback only records
	 * work for the worker which is started below. */
	error = usb_hid_arm(hid);
	if (error != 0) {
		usb_hid_unpublish(hid);
		irq = spin_lock_irqsave(&hid->lock);
		hid->stopping = 1U;
		spin_unlock_irqrestore(&hid->lock, irq);
		thread_start(worker);
		(void)usb_hid_join_worker(hid);
		goto out;
	}
	irq = spin_lock_irqsave(&hid->lock);
	hid->active = 1U;
	spin_unlock_irqrestore(&hid->lock, irq);
	thread_start(worker);
	hal_printf("usb-hid: event device usb%u device=%u interface=%u "
	    "endpoint=%02x report-bytes=%u\n",
	    drv_usb_bus_number(drv_usb_device_bus(hid->device)),
	    drv_usb_device_address(hid->device),
	    drv_usb_interface_number(hid->interface),
	    drv_usb_endpoint_address(hid->endpoint), (unsigned)hid->buffer_size);

out:
	irq = spin_lock_irqsave(&hid->lock);
	hid->activating = 0U;
	spin_unlock_irqrestore(&hid->lock, irq);
	return error;
}

static void
usb_hid_pending_remove(struct usb_hid *hid)
{
	struct usb_hid **link;
	unsigned long irq = spin_lock_irqsave(&usb_hid_pending_lock);

	if (hid->pending) {
		for (link = &usb_hid_pending; *link != NULL;
		    link = &(*link)->pending_next)
			if (*link == hid) {
				*link = hid->pending_next;
				break;
			}
		hid->pending = 0U;
		hid->pending_next = NULL;
	}
	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);
}

static int
usb_hid_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *interface_descriptor;
	struct usb_hid *hid;
	unsigned long irq;
	int error, ready;

	(void)id;
	interface_descriptor = drv_usb_interface_descriptor(interface);
	if (interface_descriptor == NULL ||
	    interface_descriptor->interface_class != USB_HID_CLASS)
		return ENODEV;
	hid = hal_malloc(sizeof(*hid));
	if (hid == NULL)
		return ENOMEM;
	memset(hid, 0, sizeof(*hid));
	hid->interface = interface;
	hid->device = drv_usb_interface_device(interface);
	spin_init(&hid->lock, LOCK_RANK_DEVICE, "usb hid");
	error = usb_hid_find_endpoint(interface, &hid->endpoint);
	if (error != 0)
		goto fail;
	error = usb_hid_fetch_layout(hid);
	if (error != 0)
		goto fail;
	/* Report Protocol is a checked publication prerequisite.  There is no
	 * Boot-Protocol fallback for malformed or unsupported devices. */
	error = usb_hid_set_report_protocol(hid);
	if (error != 0)
		goto fail;
	usb_hid_identity(hid);
	hid->buffer = hal_malloc(hid->buffer_size);
	if (hid->buffer == NULL) {
		error = ENOMEM;
		goto fail;
	}
	hid->urb = drv_usb_urb_alloc(hid->device, hid->endpoint, 0);
	if (hid->urb == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = drv_usb_interface_set_driver_data(interface, hid);
	if (error != 0)
		goto fail;
	irq = spin_lock_irqsave(&usb_hid_pending_lock);
	ready = usb_hid_input_is_ready != 0U;
	if (!ready) {
		hid->pending = 1U;
		hid->pending_next = usb_hid_pending;
		usb_hid_pending = hid;
	}
	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);
	if (ready) {
		error = usb_hid_activate(hid, 0);
		if (error != 0) {
			(void)drv_usb_interface_set_driver_data(interface, NULL);
			goto fail;
		}
	}
	return 0;

fail:
	if (hid->urb != NULL)
		drv_usb_urb_free(hid->urb);
	if (hid->buffer != NULL)
		hal_free(hid->buffer);
	if (hid->layout != NULL)
		hid_report_layout_destroy(hid->layout);
	hal_free(hid);
	return error;
}

static int
usb_hid_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct usb_hid *hid = drv_usb_interface_driver_data(interface);
	enum drv_usb_urb_status status;
	int drain_error = 0, join_error;

	(void)flags;
	if (hid == NULL)
		return 0;
	usb_hid_pending_remove(hid);
	usb_hid_close_admission(hid);
	if (hid->urb != NULL) {
		status = drv_usb_urb_status(hid->urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(hid->urb);
		drain_error = drv_usb_urb_drain(hid->urb,
		    USB_HID_DRAIN_TIMEOUT_MS);
	}
	join_error = usb_hid_join_worker(hid);
	if (drain_error != 0 || join_error != 0)
		return drain_error != 0 ? drain_error : join_error;
	/* input_device_unregister performs the one terminal held-key/button
	 * release before it detaches the old event generation. */
	usb_hid_unpublish(hid);
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	if (hid->urb != NULL)
		drv_usb_urb_free(hid->urb);
	if (hid->buffer != NULL)
		hal_free(hid->buffer);
	if (hid->layout != NULL)
		hid_report_layout_destroy(hid->layout);
	hal_free(hid);
	return 0;
}

static int
usb_hid_match(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *endpoint;
	size_t descriptor_length, capacity;

	(void)id;
	if (usb_hid_report_descriptor_length(interface, &descriptor_length) != 0 ||
	    usb_hid_find_endpoint(interface, &endpoint) != 0 ||
	    usb_hid_endpoint_capacity(interface, endpoint, &capacity) != 0)
		return 0;
	return descriptor_length != 0U && capacity != 0U ? 100 : 0;
}

static const struct drv_usb_id usb_hid_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS,
	.interface_class = USB_HID_CLASS
}};

static struct drv_usb_driver usb_hid_driver = {
	.name = "usb-hid",
	.ids = usb_hid_ids,
	.id_count = sizeof(usb_hid_ids) / sizeof(usb_hid_ids[0]),
	.match = usb_hid_match,
	.attach = usb_hid_attach,
	.detach = usb_hid_detach
};

int
drv_usb_hid_driver_register(void)
{
	int error;

	if (usb_hid_registered)
		return EALREADY;
	spin_init(&usb_hid_pending_lock, LOCK_RANK_DEVICE, "usb hid pending");
	usb_hid_pending = NULL;
	usb_hid_input_is_ready = 0U;
	error = drv_usb_driver_register(&usb_hid_driver);
	if (error == 0)
		usb_hid_registered = 1U;
	return error;
}

void
drv_usb_hid_input_ready(void)
{
	if (!usb_hid_registered)
		return;
	for (;;) {
		struct usb_hid *hid, *claimed;
		unsigned interface_number = 0U;
		unsigned long irq;
		int error;

		irq = spin_lock_irqsave(&usb_hid_pending_lock);
		usb_hid_input_is_ready = 1U;
		hid = usb_hid_pending;
		claimed = NULL;
		if (hid != NULL) {
			unsigned long hid_irq;

			usb_hid_pending = hid->pending_next;
			hid->pending_next = NULL;
			hid->pending = 0U;
			/* Pin the state against detach before dropping the list lock.
			 * Detach closes admission and joins this activation flag. */
			hid_irq = spin_lock_irqsave(&hid->lock);
			if (!hid->stopping && !hid->active && !hid->activating) {
				hid->activating = 1U;
				interface_number =
				    drv_usb_interface_number(hid->interface);
				claimed = hid;
			}
			spin_unlock_irqrestore(&hid->lock, hid_irq);
		}
		spin_unlock_irqrestore(&usb_hid_pending_lock, irq);
		if (hid == NULL)
			return;
		/* A stopped generation was removed by detach and owns its own free.
		 * Continue draining later pending interfaces instead of treating it as
		 * the end of the list. */
		if (claimed == NULL)
			continue;
		error = usb_hid_activate(claimed, 1);
		if (error != 0)
			hal_printf("usb-hid: deferred activation failed "
			    "interface=%u error=%d\n",
			    interface_number, error);
	}
}
