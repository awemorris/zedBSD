/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* XXX: Need coding style fitting. */

/*
 * USB Human Interface Device input driver
 */

#include <drivers/generic/hid-input.h>
#include <drivers/generic/hidraw.h>
#include <drivers/generic/hid-report.h>
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb.h>
#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/kmem.h>
#include <kern/kcrt.h>

#include <stdint.h>
#include <uapi/errno.h>
#include "kern/klog.h"

#define USB_HID_CLASS			0x03U
#define USB_HID_DESCRIPTOR		0x21U
#define USB_HID_REPORT_DESCRIPTOR	0x22U
#define USB_REQUEST_GET_DESCRIPTOR	0x06U
#define USB_HID_REQUEST_SET_PROTOCOL	0x0bU
#define USB_HID_REQUEST_SET_REPORT	0x09U
#define USB_HID_REPORT_TYPE_OUTPUT	2U

/* How long a raw device's output report may take (ws161-p002). */
#define USB_HID_OUTPUT_TIMEOUT_MS	5000U
#define USB_HID_PROTOCOL_REPORT		1U
#define USB_HID_CONTROL_TIMEOUT_MS	1000U
#define USB_HID_DRAIN_TIMEOUT_MS	5000U

/* drv_input_device_register() accepts at most 63 bytes plus NUL. */
#define USB_HID_TEXT_MAX		64U
#define USB_HID_ERROR_MARKERS		16U
#define USB_HID_WORK_ARM		(1U << 0)
#define USB_HID_WORK_COMPLETE		(1U << 1)

struct usb_hid {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	/*
	 * The devices the report descriptor declares and the events of its
	 * reports (the HID input glue, ws143-p005), NULL for a raw interface.
	 */
	struct hid_input *hidinput;
	struct thread *worker;
	struct spinlock lock;
	struct usb_hid *pending_next;
	uint8_t *buffer;
	size_t buffer_size;
	/* The name and the place of the touch screen's or pad's device of its own. */
	char touch_name[USB_HID_TEXT_MAX];
	char touch_physical_path[USB_HID_TEXT_MAX];
	unsigned work_pending;
	/*
	 * When the last transfer finished (CLOCK_MONOTONIC milliseconds): the
	 * completion sets it under the lock, and the worker takes it for the
	 * time of every event of the report it publishes.  The one URB is not
	 * submitted again before the worker has published its report, so the
	 * time always belongs to the buffer.
	 */
	uint64_t completed_milliseconds;
	unsigned stopping;
	unsigned submit_active;
	unsigned activating;
	unsigned active;
	unsigned pending;
	unsigned error_markers;
	char name[USB_HID_TEXT_MAX];
	char physical_path[USB_HID_TEXT_MAX];
	char unique_id[USB_HID_TEXT_MAX];
	/*
	 * A raw interface (ws161-p002: the FIDO authenticators, usage page
	 * 0xF1D0) is published as /dev/input/hidrawN instead of an event
	 * device: raw is set when the report descriptor is read, which is then
	 * kept (raw_descriptor, raw_layout) and not parsed for input.  The
	 * input reports go to hidraw as they come; the output reports go out
	 * on the interrupt OUT endpoint (out_endpoint, NULL for a device
	 * without one: SET_REPORT on the control pipe) through out_buffer,
	 * one at a time under hidraw's output lock.
	 */
	unsigned raw;
	uint8_t *raw_descriptor;
	size_t raw_descriptor_size;
	struct drv_hidraw_layout raw_layout;
	struct drv_hidraw *hidraw;
	struct drv_usb_endpoint *out_endpoint;
	uint8_t *out_buffer;
};

static struct spinlock usb_hid_pending_lock;
static struct usb_hid *usb_hid_pending;
static unsigned usb_hid_input_is_ready;
static unsigned usb_hid_registered;

/*
 * Forward declaration
 */
static uint16_t usb_hid_le16(const uint8_t *bytes);
static int usb_hid_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_hid_detach(struct drv_usb_interface *interface, unsigned flags);
static int usb_hid_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_hid_report_descriptor_length(struct drv_usb_interface *interface, size_t *result);
static int usb_hid_endpoint_capacity(struct drv_usb_interface *interface, struct drv_usb_endpoint *endpoint, size_t *result);
static int usb_hid_find_endpoint(struct drv_usb_interface *interface, struct drv_usb_endpoint **result);
static int usb_hid_fetch_layout(struct usb_hid *hid);
static int usb_hid_set_report_protocol(struct usb_hid *hid);
static void usb_hid_identity(struct usb_hid *hid);
static void usb_hid_completion(struct drv_usb_urb *urb, void *argument);
static int usb_hid_begin_submit(struct usb_hid *hid);
static void usb_hid_end_submit(struct usb_hid *hid);
static int usb_hid_arm(struct usb_hid *hid);
static void usb_hid_publish_report(struct usb_hid *hid, const uint8_t *buffer, size_t length);
static unsigned usb_hid_take_work(struct usb_hid *hid, int *stopping);
static void usb_hid_unpublish(struct usb_hid *hid);
static void usb_hid_runtime_stop(struct usb_hid *hid, const char *stage, int error, int transfer_status);
static void usb_hid_worker(void *argument);
static int usb_hid_join_worker(struct usb_hid *hid);
static void usb_hid_close_admission(struct usb_hid *hid);
static int usb_hid_activate(struct usb_hid *hid, int activation_claimed);
static int usb_hid_activate_start(struct usb_hid *hid);
static void usb_hid_activate_undo(struct usb_hid *hid, struct thread *worker);
static void usb_hid_pending_remove(struct usb_hid *hid);
static int usb_hid_raw_prepare(struct usb_hid *hid);
static int usb_hid_raw_publish(struct usb_hid *hid);
static int usb_hid_raw_output(void *context, const uint8_t *report, size_t length);
static void usb_hid_free(struct usb_hid *hid);

/* What a raw interface's transport does: its output reports (ws161-p002). */
static const struct drv_hidraw_ops usb_hid_raw_ops = {
	.output = usb_hid_raw_output
};

/*
 * USB HID
 */

static const struct drv_usb_id usb_hid_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS,
		.interface_class = USB_HID_CLASS
	}
};

static struct drv_usb_driver usb_hid_driver = {
	.name = "usb-hid",
	.ids = usb_hid_ids,
	.id_count = sizeof(usb_hid_ids) / sizeof(usb_hid_ids[0]),
	.match = usb_hid_match,
	.attach = usb_hid_attach,
	.detach = usb_hid_detach
};

/*
 * Registers this driver with the USB subsystem.
 */
int
drv_usb_hid_driver_register(
	void)
{
	int error;

	/* Handles the usb hid registered condition. */
	if (usb_hid_registered)
		return EALREADY;
	spin_init(&usb_hid_pending_lock, LOCK_RANK_DEVICE, "usb hid pending");
	usb_hid_pending = NULL;
	usb_hid_input_is_ready = 0U;

	/* Checks the operation status. */
	error = drv_usb_driver_register(&usb_hid_driver);
	if (error == 0)
		usb_hid_registered = 1U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Activates the devices that were waiting for the input layer.
 */
void
drv_usb_hid_input_ready(
	void)
{
	unsigned long hid_irq;
	struct usb_hid *hid, *claimed;
	unsigned interface_number;
	unsigned long irq;
	int error;

	/* Handles the usb hid registered condition. */
	if (!usb_hid_registered)
		return;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		interface_number = 0U;

		irq = spin_lock_irqsave(&usb_hid_pending_lock);
		usb_hid_input_is_ready = 1U;
		hid = usb_hid_pending;
		claimed = NULL;

		/* Handles the hid availability. */
		if (hid != NULL) {
			usb_hid_pending = hid->pending_next;
			hid->pending_next = NULL;
			hid->pending = 0U;

			/*
			 * Pin the state against detach before dropping the list
			 * lock. Detach closes admission and joins this
			 * activation flag.
			 */
			hid_irq = spin_lock_irqsave(&hid->lock);

			/* Handles the hid condition. */
			if (!hid->stopping && !hid->active &&
			    !hid->activating) {
				hid->activating = 1U;
				interface_number = drv_usb_interface_number(
					hid->interface);
				claimed = hid;
			}

			spin_unlock_irqrestore(&hid->lock, hid_irq);
		}

		spin_unlock_irqrestore(&usb_hid_pending_lock, irq);

		/* Handles the hid availability. */
		if (hid == NULL)
			return;

		/*
		 * A stopped generation was removed by detach and owns its own
		 * free. Continue draining later pending interfaces instead of
		 * treating it as the end of the list.
		 */
		if (claimed == NULL)
			continue;

		/* Checks the operation status. */
		error = usb_hid_activate(claimed, 1);
		if (error != 0) {
			kern_logf("usb-hid: deferred activation failed "
				   "interface=%u error=%d\n",
				   interface_number, error);
		}
	}
}

/* Reports whether this driver can drive an interface. */
static int
usb_hid_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct drv_usb_endpoint *endpoint;
	size_t descriptor_length, capacity;

	(void)id;

	/* Checks the usb hid report descriptor length result. */
	if (usb_hid_report_descriptor_length(interface, &descriptor_length) !=
		    0 ||
	    usb_hid_find_endpoint(interface, &endpoint) != 0 ||
	    usb_hid_endpoint_capacity(interface, endpoint, &capacity) != 0) {
		/* Succeeded. */
		return 0;
	}

	/* Returns the computed result. */
	return descriptor_length != 0U && capacity != 0U ? 100 : 0;
}

/* Binds this driver to an interface the bus has matched. */
static int
usb_hid_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *interface_descriptor;
	struct usb_hid *hid;
	unsigned long irq;
	int error, ready;

	(void)id;

	/* Handles the interface descriptor availability. */
	interface_descriptor = drv_usb_interface_descriptor(interface);
	if (interface_descriptor == NULL ||
	    interface_descriptor->interface_class != USB_HID_CLASS) {
		/* Failed. */
		return ENODEV;
	}

	/* Handles the hid availability. */
	hid = kern_malloc(sizeof(*hid));
	if (hid == NULL)
		return ENOMEM;
	kern_memset(hid, 0, sizeof(*hid));
	hid->interface = interface;
	hid->device = drv_usb_interface_device(interface);
	spin_init(&hid->lock, LOCK_RANK_DEVICE, "usb hid");

	/* Checks the operation status. */
	error = usb_hid_find_endpoint(interface, &hid->endpoint);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = usb_hid_fetch_layout(hid);
	if (error != 0)
		goto fail;

	/* A raw interface's output endpoint and buffer. */
	if (hid->raw) {
		error = usb_hid_raw_prepare(hid);
		if (error != 0)
			goto fail;
	}

	/*
	 * Report Protocol is a checked publication prerequisite.  There is no
	 * Boot-Protocol fallback for malformed or unsupported devices.
	 */

	/* Checks the operation status. */
	error = usb_hid_set_report_protocol(hid);
	if (error != 0)
		goto fail;
	usb_hid_identity(hid);
	hid->buffer = kern_malloc(hid->buffer_size);

	/* Handles the buffer availability. */
	if (hid->buffer == NULL) {
		error = ENOMEM;
		goto fail;
	}

	hid->urb = drv_usb_urb_alloc(hid->device, hid->endpoint, 0);

	/* Handles the urb availability. */
	if (hid->urb == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/* Checks the operation status. */
	error = drv_usb_interface_set_driver_data(interface, hid);
	if (error != 0)
		goto fail;
	irq = spin_lock_irqsave(&usb_hid_pending_lock);

	/* Handles the ready condition. */
	ready = usb_hid_input_is_ready != 0U;
	if (!ready) {
		hid->pending = 1U;
		hid->pending_next = usb_hid_pending;
		usb_hid_pending = hid;
	}

	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);

	/* Handles the ready condition. */
	if (ready) {
		/* Checks the operation status. */
		error = usb_hid_activate(hid, 0);
		if (error != 0) {
			(void)drv_usb_interface_set_driver_data(interface,
								NULL);
			goto fail;
		}
	}

	/* Succeeded. */
	return 0;

fail:

	/* Everything allocated for the interface goes. */
	usb_hid_free(hid);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives that interface up and everything held for it. */
static int
usb_hid_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_hid *hid = drv_usb_interface_driver_data(interface);
	enum drv_usb_urb_status status;
	int drain_error = 0, join_error;

	(void)flags;

	/* Handles the hid availability. */
	if (hid == NULL)
		return 0;
	usb_hid_pending_remove(hid);
	usb_hid_close_admission(hid);

	/* Handles the urb availability. */
	if (hid->urb != NULL) {
		/* Checks the operation status. */
		status = drv_usb_urb_status(hid->urb);
		if (status == DRV_USB_URB_PENDING)
			(void)drv_usb_urb_cancel(hid->urb);
		drain_error =
			drv_usb_urb_drain(hid->urb, USB_HID_DRAIN_TIMEOUT_MS);
	}

	/* Checks the operation status. */
	join_error = usb_hid_join_worker(hid);
	if (drain_error != 0 || join_error != 0)
		return drain_error != 0 ? drain_error : join_error;

	/*
	 * drv_input_device_unregister performs the one terminal held-key/button
	 * release before it detaches the old event generation.
	 */
	usb_hid_unpublish(hid);
	(void)drv_usb_interface_set_driver_data(interface, NULL);

	/* Everything allocated for the interface goes. */
	usb_hid_free(hid);

	/* Succeeded. */
	return 0;
}

/* Reports how long the report descriptor of an interface is. */
static int
usb_hid_report_descriptor_length(
	struct drv_usb_interface *interface,
	size_t *result)
{
	const uint8_t *subordinate;
	size_t report_length;
	const uint8_t *descriptor;
	size_t length, entries, entry;
	int error;
	const struct drv_usb_host_interface *alternate;
	unsigned count, index;
	size_t found = 0;

	/* Handles the alternate availability. */
	alternate = drv_usb_interface_active_alternate(interface);
	if (alternate == NULL || result == NULL)
		return EINVAL;
	count = drv_usb_host_interface_extra_count(alternate);
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Checks the operation status. */
		error = drv_usb_host_interface_extra(
			alternate, index, (const void **)&descriptor, &length);
		if (error != 0)
			return error;

		/* Checks the current data length. */
		if (length < 2U || descriptor[0] != length ||
		    descriptor[1] != USB_HID_DESCRIPTOR)
			continue;

		/* Checks the current data length. */
		if (length < 6U)
			return EINVAL;

		/* Handles the entries condition. */
		entries = descriptor[5];
		if (entries == 0U || entries > (length - 6U) / 3U ||
		    6U + entries * 3U != length) {
			/* Failed. */
			return EINVAL;
		}
		/* Process each element required by the operation. */
		for (entry = 0; entry < entries; entry++) {
			/* Handles the subordinate condition. */
			subordinate = descriptor + 6U + entry * 3U;
			if (subordinate[0] != USB_HID_REPORT_DESCRIPTOR)
				continue;

			/* Handles the report length condition. */
			report_length = usb_hid_le16(subordinate + 1U);
			if (report_length == 0U ||
			    report_length > HID_REPORT_DESCRIPTOR_SIZE_MAX ||
			    found != 0U) {
				/* Failed. */
				return EINVAL;
			}
			found = report_length;
		}
	}

	/* Handles the found condition. */
	if (found == 0U)
		return ENOENT;
	*result = found;
	/* Succeeded. */
	return 0;
}

/* Reports how many bytes one interrupt endpoint carries. */
static int
usb_hid_endpoint_capacity(
	struct drv_usb_interface *interface,
	struct drv_usb_endpoint *endpoint,
	size_t *result)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	const struct drv_usb_superspeed_endpoint_companion_descriptor
		*companion;
	enum drv_usb_speed speed;
	uint16_t maximum;
	unsigned payload, packets;
	size_t capacity;

	/* Handles the descriptor availability. */
	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL || descriptor->interval == 0U || result == NULL)
		return EINVAL;
	maximum = drv_usb_endpoint_max_packet_size(endpoint);
	payload = maximum & 0x07ffU;
	packets = 1U + ((maximum >> 11U) & 3U);

	/* Handles the payload condition. */
	speed = drv_usb_device_speed(drv_usb_interface_device(interface));
	if (payload == 0U || (maximum & 0xe000U) != 0U || packets == 4U)
		return EINVAL;

	/* Handles the speed condition. */
	if (speed == DRV_USB_SPEED_LOW) {
		/* Handles the payload condition. */
		if (payload > 8U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_FULL) {
		/* Handles the payload condition. */
		if (payload > 64U || packets != 1U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_HIGH) {
		/* Handles the payload condition. */
		if (payload > 1024U)
			return EINVAL;
	} else if (speed == DRV_USB_SPEED_SUPER ||
		   speed == DRV_USB_SPEED_SUPER_PLUS) {
		/* Handles the payload condition. */
		if (payload > 1024U || packets != 1U)
			return EINVAL;
		companion = drv_usb_endpoint_superspeed_companion(endpoint);
		capacity =
			(size_t)payload *
			((size_t)drv_usb_endpoint_maximum_burst(endpoint) + 1U);

		/* Handles the companion availability. */
		if (companion != NULL && companion->bytes_per_interval != 0U) {
			/* Handles the companion condition. */
			if (companion->bytes_per_interval > capacity)
				return EINVAL;
			capacity = companion->bytes_per_interval;
		}

		*result = capacity;
		/* Succeeded. */
		return 0;
	} else {
		/* Failed. */
		return EINVAL;
	}

	*result = (size_t)payload * packets;
	/* Succeeded. */
	return 0;
}

/* Finds the interrupt-in endpoint of an interface. */
static int
usb_hid_find_endpoint(
	struct drv_usb_interface *interface,
	struct drv_usb_endpoint **result)
{
	struct drv_usb_endpoint *endpoint, *extra;

	/* Handles the endpoint availability. */
	endpoint = drv_usb_interface_find_endpoint(
		interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	if (endpoint == NULL)
		return ENODEV;

	/* Handles the extra availability. */
	extra = drv_usb_interface_find_endpoint(interface,
						DRV_USB_TRANSFER_INTERRUPT,
						DRV_USB_DIR_IN, endpoint);
	if (extra != NULL)
		return EOPNOTSUPP;
	*result = endpoint;
	/* Succeeded. */
	return 0;
}

/* Reads the report descriptor and describes the devices it declares. */
static int
usb_hid_fetch_layout(
	struct usb_hid *hid)
{
	uint8_t *descriptor;
	size_t descriptor_length;
	size_t actual;
	size_t capacity;
	size_t maximum_report;
	int raw;
	int error;

	/* How long the report descriptor is. */
	error = usb_hid_report_descriptor_length(hid->interface, &descriptor_length);
	if (error != 0)
		return error;

	/* Room for the descriptor. */
	descriptor = kern_malloc(descriptor_length);
	if (descriptor == NULL)
		return ENOMEM;

	/* The descriptor, read whole from the interface. */
	actual = 0;
	error = drv_usb_control(
		hid->device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_INTERFACE,
		USB_REQUEST_GET_DESCRIPTOR,
		(uint16_t)(USB_HID_REPORT_DESCRIPTOR << 8U),
		(uint16_t)drv_usb_interface_number(hid->interface),
		descriptor,
		descriptor_length,
		USB_HID_CONTROL_TIMEOUT_MS,
		&actual);
	if (error == 0 && actual != descriptor_length)
		error = EIO;

	/* A FIDO authenticator's interface is raw: the descriptor is kept and not parsed for input (ws161-p002). */
	if (error == 0) {
		raw = drv_hidraw_describe(descriptor, descriptor_length, &hid->raw_layout);
		if (raw == 0 &&
		    hid->raw_layout.usage_page == HIDRAW_USAGE_PAGE_FIDO &&
		    hid->raw_layout.usage == HIDRAW_USAGE_CTAPHID) {
			hid->raw = 1U;
			hid->raw_descriptor = descriptor;
			hid->raw_descriptor_size = descriptor_length;
			error = usb_hid_endpoint_capacity(hid->interface, hid->endpoint, &capacity);
			if (error != 0)
				return error;
			hid->buffer_size = capacity;
			return 0;
		}
	}

	/* Any other interface's descriptor is parsed for its input, and is no longer needed. */
	if (error == 0)
		error = drv_hid_input_prepare(descriptor, descriptor_length, &hid->hidinput);
	kern_free(descriptor);

	/* A descriptor that could not be read or described. */
	if (error != 0)
		return error;

	/* The interrupt endpoint's room for one transfer. */
	error = usb_hid_endpoint_capacity(hid->interface, hid->endpoint, &capacity);
	if (error != 0)
		return error;

	/* The longest report must fit one transfer, which is the buffer's size. */
	maximum_report = drv_hid_input_report_max(hid->hidinput);
	if (maximum_report > capacity)
		return EOVERFLOW;
	hid->buffer_size = maximum_report;

	/* Succeeded: the devices are described and the buffer's size is known. */
	return 0;
}

/* Puts the device into the report protocol, not the boot one. */
static int
usb_hid_set_report_protocol(
	struct usb_hid *hid)
{
	int error;
	const struct drv_usb_interface_descriptor *descriptor;
	size_t actual = 0;

	/* Handles the descriptor availability. */
	descriptor = drv_usb_interface_descriptor(hid->interface);
	if (descriptor == NULL)
		return EINVAL;

	/* Non-Boot interfaces already have exactly one Report Protocol. */
	if (descriptor->interface_subclass != 1U)
		return 0;

	/* Obtains the drv usb control result. */
	error = drv_usb_control(
		hid->device,
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
			DRV_USB_RECIP_INTERFACE,
		USB_HID_REQUEST_SET_PROTOCOL, USB_HID_PROTOCOL_REPORT,
		(uint16_t)drv_usb_interface_number(hid->interface), NULL, 0,
		USB_HID_CONTROL_TIMEOUT_MS, &actual);

	/* Returns the computed result. */
	return error;
}

/* Builds the name this device is presented to the kernel under. */
static void
usb_hid_identity(
	struct usb_hid *hid)
{
	const struct drv_usb_device_descriptor *descriptor;
	unsigned bus;
	unsigned address;
	unsigned port;
	unsigned interface_number;
	unsigned kind;
	int error;

	/* Renders the topology as the physical path of the device and of its touch device. */
	descriptor = drv_usb_device_descriptor(hid->device);
	bus = drv_usb_bus_number(drv_usb_device_bus(hid->device));
	address = drv_usb_device_address(hid->device);
	port = drv_usb_device_port(hid->device);
	interface_number = drv_usb_interface_number(hid->interface);
	(void)kern_snprintf(hid->physical_path,
			    sizeof(hid->physical_path),
			    "usb%u/port%u/device%u/interface%u",
			    bus,
			    port,
			    address,
			    interface_number);
	(void)kern_snprintf(hid->touch_physical_path,
			    sizeof(hid->touch_physical_path),
			    "usb%u/port%u/device%u/interface%u/touch",
			    bus,
			    port,
			    address,
			    interface_number);

	/* The serial number is the unique ID, when the device has one. */
	hid->unique_id[0] = '\0';
	if (descriptor->serial_string != 0U) {
		(void)drv_usb_device_get_string(hid->device,
						descriptor->serial_string,
						0,
						hid->unique_id,
						sizeof(hid->unique_id));
	}

	/* The product's name, when the device has one. */
	hid->name[0] = '\0';
	error = ENOENT;
	if (descriptor->product_string != 0U) {
		error = drv_usb_device_get_string(hid->device,
						  descriptor->product_string,
						  0,
						  hid->name,
						  sizeof(hid->name));
	}

	/* A named product's touch screen is the product's touch screen. */
	if (error == 0 && hid->name[0] != '\0') {
		(void)kern_snprintf(hid->touch_name, sizeof(hid->touch_name), "%s Touchscreen", hid->name);
		return;
	}

	/* A touch screen without a product name. */
	(void)kern_snprintf(hid->touch_name, sizeof(hid->touch_name), "USB HID touchscreen");

	/* A raw interface is named after what it is. */
	if (hid->raw) {
		(void)kern_snprintf(hid->name, sizeof(hid->name), "USB FIDO authenticator");
		return;
	}

	/* Any other device without a name is named after what it chiefly is. */
	kind = drv_hid_input_kind(hid->hidinput);
	switch (kind) {
	case HID_INPUT_KIND_PEN:
		(void)kern_snprintf(hid->name, sizeof(hid->name), "USB HID pen");
		break;
	case HID_INPUT_KIND_TABLET:
		(void)kern_snprintf(hid->name, sizeof(hid->name), "USB HID tablet");
		break;
	case HID_INPUT_KIND_MOUSE:
		(void)kern_snprintf(hid->name, sizeof(hid->name), "USB HID mouse");
		break;
	default:
		(void)kern_snprintf(hid->name, sizeof(hid->name), "USB HID keyboard");
		break;
	}
}

/* Takes one finished interrupt transfer. */
static void
usb_hid_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct usb_hid *hid = argument;
	struct thread *worker;
	unsigned long irq;
	uint64_t milliseconds;

	/* Handles the hid availability. */
	if (hid == NULL || urb != hid->urb)
		return;

	/* The report's time is when the transfer finished, not when the worker runs. */
	milliseconds = clock_milliseconds(NULL);

	/* Hands the finished transfer and its time to the worker. */
	irq = spin_lock_irqsave(&hid->lock);

	hid->work_pending |= USB_HID_WORK_COMPLETE;
	hid->completed_milliseconds = milliseconds;
	worker = hid->worker;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* Handles the worker availability. */
	if (worker != NULL)
		kernel_notify_task(worker->task);
}

/* Joins the gate that lets a transfer be submitted. */
static int
usb_hid_begin_submit(
	struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	int admitted = !hid->stopping && !hid->submit_active;

	/* Handles the admitted condition. */
	if (admitted)
		hid->submit_active = 1U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* Returns the computed result. */
	return admitted ? 0 : EBUSY;
}

static void usb_hid_end_submit(struct usb_hid *hid);

/* Leaves that gate. */
static void
usb_hid_end_submit(
	struct usb_hid *hid)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);

	/* Handles the hid condition. */
	if (!hid->submit_active)
		__builtin_trap();
	hid->submit_active = 0U;

	spin_unlock_irqrestore(&hid->lock, irq);
}

/* Puts the interrupt transfer back on its endpoint. */
static int
usb_hid_arm(
	struct usb_hid *hid)
{
	int error;

	/* Checks the operation status. */
	error = usb_hid_begin_submit(hid);
	if (error != 0)
		return error;
	kern_memset(hid->buffer, 0, hid->buffer_size);

	/* Checks the operation status. */
	error = drv_usb_urb_setup(hid->urb, hid->buffer, hid->buffer_size,
				  DRV_USB_URB_SHORT_OK, 0, usb_hid_completion,
				  hid);
	if (error == 0)
		error = drv_usb_urb_submit(hid->urb);
	usb_hid_end_submit(hid);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Hands one received report on: a raw interface's to hidraw, any other's to the input glue. */
static void
usb_hid_publish_report(
	struct usb_hid *hid,
	const uint8_t *buffer,
	size_t length)
{
	uint64_t milliseconds;
	unsigned long irq;

	/* A raw interface's report goes to its readers as it came. */
	if (hid->raw) {
		drv_hidraw_input(hid->hidraw, buffer, length);
		return;
	}

	/* Every event of the report is stamped with the time its transfer finished. */
	irq = spin_lock_irqsave(&hid->lock);

	milliseconds = hid->completed_milliseconds;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* The glue decodes the report and tells the devices what changed. */
	drv_hid_input_report(hid->hidinput, buffer, length, milliseconds);
}

/* Takes whatever the worker thread has to do next. */
static unsigned
usb_hid_take_work(
	struct usb_hid *hid,
	int *stopping)
{
	unsigned long irq = spin_lock_irqsave(&hid->lock);
	unsigned work = hid->work_pending;

	hid->work_pending = 0U;
	*stopping = hid->stopping != 0U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* Returns the computed result. */
	return work;
}

/* Takes this device back out of the input subsystem. */
static void
usb_hid_unpublish(
	struct usb_hid *hid)
{
	struct drv_hidraw *hidraw;
	unsigned long irq;

	/* The interface is no longer active, and its raw node is taken. */
	irq = spin_lock_irqsave(&hid->lock);

	hidraw = hid->hidraw;
	hid->hidraw = NULL;
	hid->active = 0U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* A raw interface's node goes; no output runs after this. */
	if (hidraw != NULL)
		drv_hidraw_unregister(hidraw);

	/* The input devices go, and the input layer releases what they held (a second call finds none). */
	if (hid->hidinput != NULL)
		drv_hid_input_unpublish(hid->hidinput);
}

/* Stops the transfers and the worker this device runs. */
static void
usb_hid_runtime_stop(
	struct usb_hid *hid,
	const char *stage,
	int error,
	int transfer_status)
{
	unsigned long irq;
	int report;

	irq = spin_lock_irqsave(&hid->lock);

	hid->stopping = 1U;
	report = hid->error_markers++ < USB_HID_ERROR_MARKERS;

	spin_unlock_irqrestore(&hid->lock, irq);

	/*
	 * Remove a device which cannot be rearmed instead of leaving a visible
	 * event node that can never produce another report.  Detach joins this
	 * worker before attempting the same idempotent unpublication.
	 */
	usb_hid_unpublish(hid);

	/* Handles the report condition. */
	if (report) {
		/* Handles the transfer status condition. */
		if (transfer_status) {
			kern_logf(
				"usb-hid: terminal transfer stopped "
				"interface=%u status=%d; input unpublished\n",
				drv_usb_interface_number(hid->interface),
				error);
		} else {
			kern_logf("usb-hid: %s failed interface=%u error=%d; "
				   "input unpublished\n",
				   stage,
				   drv_usb_interface_number(hid->interface),
				   error);
		}
	}
}

/* Decodes and publishes reports outside interrupt context. */
static void
usb_hid_worker(
	void *argument)
{
	enum drv_usb_urb_status status;
	unsigned long irq;
	int stopping_now;
	unsigned work;
	int error, stopping;
	struct usb_hid *hid = argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the stopping condition. */
		work = usb_hid_take_work(hid, &stopping);
		if (stopping)
			return;

		/* Handles the work condition. */
		if (work == 0U) {
			kern_thread_block();
			continue;
		}

		/* Handles the work condition. */
		if ((work & USB_HID_WORK_COMPLETE) != 0U) {
			/* Checks the operation status. */
			error = drv_usb_urb_drain(hid->urb,
						  USB_HID_DRAIN_TIMEOUT_MS);
			if (error != 0) {
				usb_hid_runtime_stop(hid, "completion drain",
						     error, 0);

				/* Returns the computed result. */
				return;
			}

			/* Checks the operation status. */
			status = drv_usb_urb_status(hid->urb);
			if (status == DRV_USB_URB_COMPLETE) {
				usb_hid_publish_report(
					hid, hid->buffer,
					drv_usb_urb_actual_length(hid->urb));
				work |= USB_HID_WORK_ARM;
			} else if (status == DRV_USB_URB_STALL) {
				/* Checks the operation status. */
				error = drv_usb_endpoint_clear_halt(
					hid->endpoint);
				if (error == 0) {
					work |= USB_HID_WORK_ARM;
				} else {
					usb_hid_runtime_stop(hid, "clear-halt",
							     error, 0);

					/* Returns the computed result. */
					return;
				}
			} else if (status != DRV_USB_URB_CANCELLED &&
				   status != DRV_USB_URB_DISCONNECTED) {
				usb_hid_runtime_stop(hid, "terminal transfer",
						     (int)status, 1);

				/* Returns the computed result. */
				return;
			}
		}

		/* Handles the work condition. */
		if ((work & USB_HID_WORK_ARM) != 0U) {
			/* Checks the operation status. */
			error = usb_hid_arm(hid);
			if (error != 0) {
				/*
				 * EBUSY is expected only after detach closes
				 * admission.  In that case detach owns
				 * publication; any other EBUSY is still a
				 * terminal always-on-URB contract failure.
				 */
				irq = spin_lock_irqsave(&hid->lock);
				stopping_now = hid->stopping != 0U;

				spin_unlock_irqrestore(&hid->lock, irq);

				/* Handles the stopping now condition. */
				if (!stopping_now) {
					usb_hid_runtime_stop(hid, "rearm",
							     error, 0);
				}

				/* Returns the computed result. */
				return;
			}
		}
	}
}

/* Waits for that worker to leave. */
static int
usb_hid_join_worker(
	struct usb_hid *hid)
{
	struct thread *worker = hid->worker;
	int error;

	/* Handles the worker availability. */
	if (worker == NULL)
		return 0;

	/* Handles the worker condition. */
	if (worker == curthread)
		return EBUSY;
	kernel_notify_task(worker->task);
	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	       THREAD_ZOMBIE)
		sched_yield();

	/* Checks the operation status. */
	error = thread_wait(worker, NULL);
	if (error == 0)
		hid->worker = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops new transfers being admitted. */
static void
usb_hid_close_admission(
	struct usb_hid *hid)
{
	unsigned long irq;
	unsigned active;

	irq = spin_lock_irqsave(&hid->lock);

	hid->stopping = 1U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* Handles the worker availability. */
	if (hid->worker != NULL)
		kernel_notify_task(hid->worker->task);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&hid->lock);
		active = hid->submit_active || hid->activating;
		spin_unlock_irqrestore(&hid->lock, irq);

		/* Handles the active condition. */
		if (!active)
			return;
		sched_yield();
	}
}

/* Brings the device into service and arms its first transfer. */
static int
usb_hid_activate(
	struct usb_hid *hid,
	int activation_claimed)
{
	unsigned long irq;
	int error;

	/* The activation is claimed here unless the caller already holds the claim. */
	if (!activation_claimed) {
		irq = spin_lock_irqsave(&hid->lock);

		/* A stopping, active or activating interface is not activated again. */
		if (hid->stopping || hid->active || hid->activating) {
			error = EBUSY;
			if (hid->active)
				error = 0;
			spin_unlock_irqrestore(&hid->lock, irq);

			/* An active interface is already in service; any other is busy. */
			return error;
		}

		/* The claim: detach waits for it to be given back. */
		hid->activating = 1U;

		spin_unlock_irqrestore(&hid->lock, irq);
	}

	/* The worker, the devices and the first transfer. */
	error = usb_hid_activate_start(hid);

	/* The claim is given back, whatever happened. */
	irq = spin_lock_irqsave(&hid->lock);

	hid->activating = 0U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* Reports why the interface could not be brought into service. */
	if (error != 0)
		return error;

	/* Succeeded: the interface is in service. */
	return 0;
}
/*
 * Starts an interface whose activation is claimed: makes its worker,
 * publishes its devices or its raw node, and arms the first transfer.  A
 * failure leaves nothing published and no worker.
 */
static int
usb_hid_activate_start(
	struct usb_hid *hid)
{
	const struct drv_usb_device_descriptor *usb_descriptor;
	struct hid_input_identity identity;
	struct thread *worker;
	unsigned long irq;
	int error;

	/* The worker that decodes the reports, not started yet. */
	error = kthread_create(usb_hid_worker, hid, SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;
	hid->worker = worker;

	/* How the devices are named, from the interface's identity. */
	usb_descriptor = drv_usb_device_descriptor(hid->device);
	kern_memset(&identity, 0, sizeof(identity));
	identity.name = hid->name;
	identity.physical_path = hid->physical_path;
	identity.unique_id = hid->unique_id;
	identity.touch_name = hid->touch_name;
	identity.touch_physical_path = hid->touch_physical_path;
	identity.id.bustype = BUS_USB;
	identity.id.vendor = usb_descriptor->vendor;
	identity.id.product = usb_descriptor->product;
	identity.id.version = usb_descriptor->device_release;

	/*
	 * The interface's own device, unless the interface is a touch screen
	 * and nothing else (its own capabilities are then EV_SYN alone); a
	 * raw interface's node in place of both.
	 */
	error = 0;
	if (hid->raw)
		error = usb_hid_raw_publish(hid);
	else
		error = drv_hid_input_publish(hid->hidinput, &identity);

	/* A failed publication takes back what was published and stops the worker. */
	if (error != 0) {
		usb_hid_activate_undo(hid, worker);
		return error;
	}

	/*
	 * The first accepted request is part of the attach transaction.  A
	 * publication which can never receive a report is not a successful HID
	 * attachment.  Synchronous completion is safe: its callback only
	 * records work for the worker which is started below.
	 */
	error = usb_hid_arm(hid);
	if (error != 0) {
		usb_hid_activate_undo(hid, worker);
		return error;
	}

	/* The interface is in service: detach now unpublishes it. */
	irq = spin_lock_irqsave(&hid->lock);

	hid->active = 1U;

	spin_unlock_irqrestore(&hid->lock, irq);

	/* The worker takes the reports from now on. */
	thread_start(worker);
	kern_logf("usb-hid: event device usb%u device=%u interface=%u endpoint=%02x report-bytes=%u\n",
		  drv_usb_bus_number(drv_usb_device_bus(hid->device)),
		  drv_usb_device_address(hid->device),
		  drv_usb_interface_number(hid->interface),
		  drv_usb_endpoint_address(hid->endpoint),
		  (unsigned)hid->buffer_size);

	/* Succeeded: the interface is in service. */
	return 0;
}

/* Takes back a failed activation: what was published goes, and the worker ends unstarted. */
static void
usb_hid_activate_undo(
	struct usb_hid *hid,
	struct thread *worker)
{
	unsigned long irq;

	/* The devices or the raw node go. */
	usb_hid_unpublish(hid);

	/* The worker is told to stop, started so it can see that, and joined. */
	irq = spin_lock_irqsave(&hid->lock);

	hid->stopping = 1U;

	spin_unlock_irqrestore(&hid->lock, irq);

	thread_start(worker);
	(void)usb_hid_join_worker(hid);
}


/* Takes this device off the list of those waiting to activate. */
static void
usb_hid_pending_remove(
	struct usb_hid *hid)
{
	struct usb_hid **link;
	unsigned long irq = spin_lock_irqsave(&usb_hid_pending_lock);

	/* Handles the hid condition. */
	if (hid->pending) {
		/* Process each element required by the operation. */
		for (link = &usb_hid_pending; *link != NULL;
		     link = &(*link)->pending_next) {
			/* Handles the link condition. */
			if (*link == hid) {
				*link = hid->pending_next;
				break;
			}
		}

		hid->pending = 0U;
		hid->pending_next = NULL;
	}

	spin_unlock_irqrestore(&usb_hid_pending_lock, irq);
}

/*
 * Prepares a raw interface's output: its interrupt OUT endpoint (none: the
 * reports go by SET_REPORT) and a buffer for one report with its ID.
 */
static int
usb_hid_raw_prepare(
	struct usb_hid *hid)
{
	size_t size;

	/* The interrupt OUT endpoint, when the interface has one. */
	hid->out_endpoint = drv_usb_interface_find_endpoint(
		hid->interface, DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_OUT, NULL);

	/* A device that takes no output report has nothing to write to. */
	if (hid->raw_layout.output_size == 0U)
		return ENODEV;

	/* One report and its ID's byte. */
	size = (size_t)hid->raw_layout.output_size + 1U;
	hid->out_buffer = kern_malloc(size);
	if (hid->out_buffer == NULL)
		return ENOMEM;

	/* Succeeded: the output is ready. */
	return 0;
}

/* Publishes a raw interface's node: what it is, its report descriptor and its transport. */
static int
usb_hid_raw_publish(
	struct usb_hid *hid)
{
	const struct drv_usb_device_descriptor *usb_descriptor;
	struct drv_hidraw_description description;
	int error;

	/* What the device is. */
	usb_descriptor = drv_usb_device_descriptor(hid->device);
	kern_memset(&description, 0, sizeof(description));
	description.info.bus = HIDRAW_BUS_USB;
	description.info.vendor = usb_descriptor->vendor;
	description.info.product = usb_descriptor->product;
	description.info.version = usb_descriptor->device_release;
	description.info.interface_number = (uint16_t)drv_usb_interface_number(hid->interface);
	description.info.usage_page = hid->raw_layout.usage_page;
	description.info.usage = hid->raw_layout.usage;
	description.info.input_size = hid->raw_layout.input_size;
	description.info.output_size = hid->raw_layout.output_size;
	if (hid->raw_layout.numbered)
		description.info.flags |= HIDRAW_INFO_NUMBERED;
	description.name = hid->name;
	description.physical_path = hid->physical_path;
	description.descriptor = hid->raw_descriptor;
	description.descriptor_size = hid->raw_descriptor_size;

	/* The node. */
	error = drv_hidraw_register(&description, &usb_hid_raw_ops, hid, &hid->hidraw);
	if (error != 0)
		return error;

	/* Succeeded: the node is published. */
	return 0;
}

/*
 * Sends one output report of a raw interface (hidraw's output, under its
 * output lock): the ID's byte is sent only by a device that numbers its
 * reports.  The interrupt OUT endpoint carries it, or SET_REPORT on the
 * control pipe for a device without one.
 */
static int
usb_hid_raw_output(
	void *context,
	const uint8_t *report,
	size_t length)
{
	struct usb_hid *hid;
	uint8_t *data;
	size_t size;
	size_t actual;
	uint8_t report_id;
	int error;

	/* The report, with or without its ID's byte. */
	hid = context;
	if (length < 2U || length > (size_t)hid->raw_layout.output_size + 1U)
		return EINVAL;
	report_id = report[0];
	kern_memcpy(hid->out_buffer, report, length);
	data = hid->out_buffer;
	size = length;
	if (!hid->raw_layout.numbered) {
		data = hid->out_buffer + 1U;
		size = length - 1U;
	}

	/* The interrupt OUT endpoint. */
	actual = 0U;
	if (hid->out_endpoint != NULL) {
		error = drv_usb_interrupt(hid->device, hid->out_endpoint, data, size,
					  USB_HID_OUTPUT_TIMEOUT_MS, &actual);
		if (error == 0 && actual != size)
			error = EIO;
		if (error != 0)
			return error;
		return 0;
	}

	/* SET_REPORT (Output) on the control pipe. */
	error = drv_usb_control(
		hid->device,
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
		USB_HID_REQUEST_SET_REPORT,
		(uint16_t)((USB_HID_REPORT_TYPE_OUTPUT << 8U) | report_id),
		(uint16_t)drv_usb_interface_number(hid->interface),
		data,
		size,
		USB_HID_OUTPUT_TIMEOUT_MS,
		&actual);
	if (error != 0)
		return error;

	/* Succeeded: the device took the report. */
	return 0;
}

/* Frees what was allocated for an interface, and its record. */
static void
usb_hid_free(
	struct usb_hid *hid)
{
	/* The transfer. */
	if (hid->urb != NULL)
		drv_usb_urb_free(hid->urb);

	/* The input and the output buffers. */
	if (hid->buffer != NULL)
		kern_free(hid->buffer);
	if (hid->out_buffer != NULL)
		kern_free(hid->out_buffer);

	/* The described devices (unpublished by now), or a raw interface's descriptor. */
	if (hid->hidinput != NULL)
		drv_hid_input_destroy(hid->hidinput);
	if (hid->raw_descriptor != NULL)
		kern_free(hid->raw_descriptor);

	/* The record. */
	kern_free(hid);
}

/* Reads a 16-bit descriptor field, least significant byte first. */
static uint16_t
usb_hid_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}
