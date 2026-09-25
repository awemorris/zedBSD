/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB 2.0 hub class driver (plan/ws035/phase046/design.md).
 *
 * The core enumerates the devices below a hub; this driver only reads the
 * hub: its descriptor, its ports through the hub class requests, and its
 * status change endpoint.  A driver's attach runs with the core's topology
 * lock held, so everything that enumerates runs on the hub's own thread,
 * which takes the lock only when it is free and never waits for it, so
 * that a detach while the core holds the lock can still join it.
 */

#include <drivers/usb/usb-hub.h>
#include <drivers/usb/usb.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#define USB_HUB_CLASS			9U
#define USB_HUB_DESCRIPTOR		0x29U
#define USB_HUB_REQUEST_TIMEOUT_MS	1000U
#define USB_HUB_POLL_MS			250U
#define USB_HUB_RESCAN_POLLS		8U	/* a rescan every two seconds regardless */
#define USB_HUB_RETRY_MS		20U
#define USB_HUB_RESET_TIMEOUT_MS	500U
#define USB_HUB_RESET_RECOVERY_MS	10U
#define USB_HUB_POWER_MINIMUM_MS	100U

/* Hub class requests and port features. */
#define USB_HUB_GET_STATUS		0U
#define USB_HUB_CLEAR_FEATURE		1U
#define USB_HUB_SET_FEATURE		3U
#define USB_HUB_GET_DESCRIPTOR		6U
#define USB_HUB_PORT_RESET		4U
#define USB_HUB_PORT_POWER		8U
#define USB_HUB_C_PORT_RESET		20U

struct usb_hub {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct thread *worker;
	volatile unsigned stopping;
	unsigned ports;
	unsigned multi_tt;
	unsigned think_time;
	unsigned power_good_ms;
	uint8_t change[32];
};

static int usb_hub_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_hub_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int usb_hub_detach(struct drv_usb_interface *interface, unsigned flags);
static void usb_hub_worker(void *argument);
static int usb_hub_setup(struct usb_hub *hub);
static int usb_hub_port_status(void *context, unsigned port, uint32_t *status);
static int usb_hub_clear_feature(void *context, unsigned port, uint16_t feature);
static int usb_hub_port_reset(void *context, unsigned port);
static void usb_hub_sleep_ms(struct usb_hub *hub, unsigned milliseconds);

static const struct drv_usb_hub_ops usb_hub_ops = {
	usb_hub_port_status,
	usb_hub_clear_feature,
	usb_hub_port_reset
};

static const struct drv_usb_id usb_hub_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS,
		.interface_class = USB_HUB_CLASS
	}
};

static struct drv_usb_driver usb_hub_driver = {
	.name = "usb-hub",
	.ids = usb_hub_ids,
	.id_count = sizeof(usb_hub_ids) / sizeof(usb_hub_ids[0]),
	.match = usb_hub_match,
	.attach = usb_hub_attach,
	.detach = usb_hub_detach
};

static unsigned usb_hub_registered;

/* Registers this driver with the USB subsystem. */
int
drv_usb_hub_driver_register(
	void)
{
	int error;

	if (usb_hub_registered)
		return EALREADY;
	error = drv_usb_driver_register(&usb_hub_driver);
	if (error == 0)
		usb_hub_registered = 1U;
	return error;
}

/* Claims a hub's interface: class 9 with an interrupt IN endpoint. */
static int
usb_hub_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *descriptor;

	(void)id;
	descriptor = drv_usb_interface_descriptor(interface);
	if (descriptor == NULL || descriptor->interface_class != USB_HUB_CLASS)
		return 0;
	if (drv_usb_interface_find_endpoint(interface, DRV_USB_TRANSFER_INTERRUPT,
	    DRV_USB_DIR_IN, NULL) == NULL)
		return 0;
	return 100;
}

/*
 * Binds to a hub and starts its thread; the hub's ports are read and
 * enumerated there, not here, because the core holds its lock during attach.
 */
static int
usb_hub_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *descriptor;
	struct usb_hub *hub;
	int error;

	(void)id;
	hub = kern_malloc(sizeof(*hub));
	if (hub == NULL)
		return ENOMEM;
	kern_memset(hub, 0, sizeof(*hub));
	hub->interface = interface;
	hub->device = drv_usb_interface_device(interface);
	hub->endpoint = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	descriptor = drv_usb_interface_descriptor(interface);

	/* A high-speed hub with protocol 2 has one transaction translator a port. */
	hub->multi_tt = descriptor != NULL && descriptor->interface_protocol == 2U;
	if (hub->endpoint == NULL) {
		kern_free(hub);
		return ENODEV;
	}

	error = drv_usb_interface_set_driver_data(interface, hub);
	if (error == 0)
		error = kthread_create(usb_hub_worker, hub, SCHED_PRIORITY_DEFAULT,
		    &hub->worker);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		kern_free(hub);
		return error;
	}
	thread_start(hub->worker);
	return 0;
}

/* Stops the hub's thread and waits for it; the core already took its devices. */
static int
usb_hub_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct usb_hub *hub;
	struct thread *worker;
	int error;

	(void)flags;
	hub = drv_usb_interface_driver_data(interface);
	if (hub == NULL)
		return 0;
	hub->stopping = 1U;
	worker = hub->worker;
	if (worker != NULL) {
		if (worker == curthread)
			return EBUSY;
		kernel_notify_task(worker->task);
		while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
		    THREAD_ZOMBIE)
			sched_yield();
		error = thread_wait(worker, NULL);
		if (error != 0)
			return error;
		hub->worker = NULL;
	}
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	kern_free(hub);
	return 0;
}

/*
 * Reads the hub, powers its ports, hands them to the core, then waits for
 * the status change endpoint and has the core rescan on every change.
 */
static void
usb_hub_worker(
	void *argument)
{
	struct usb_hub *hub;
	size_t actual;
	size_t length;
	unsigned quiet;
	int error;

	hub = argument;
	if (usb_hub_setup(hub) != 0)
		return;

	/* Makes the ports known, waiting while the core is busy. */
	for (;;) {
		if (hub->stopping)
			return;
		error = drv_usb_hub_attach_ports(hub->device, &usb_hub_ops, hub,
		    hub->ports, hub->multi_tt, hub->think_time);
		if (error != EBUSY)
			break;
		usb_hub_sleep_ms(hub, USB_HUB_RETRY_MS);
	}
	if (error != 0) {
		if (error != ENODEV)
			kern_logf("usb-hub: hub %u ports not used (%d)\n",
			    drv_usb_device_address(hub->device), error);
		return;
	}

	/* One bit a port, and bit 0 for the hub itself. */
	length = (hub->ports + 8U) / 8U;
	quiet = 0;
	while (!hub->stopping) {
		actual = 0;
		error = drv_usb_interrupt(hub->device, hub->endpoint, hub->change,
		    length, USB_HUB_POLL_MS, &actual);
		if (hub->stopping)
			break;

		/* A timeout is the quiet case; a periodic rescan covers a lost report. */
		if (error != 0 || actual == 0) {
			if (error != 0 && error != ETIMEDOUT)
				usb_hub_sleep_ms(hub, USB_HUB_POLL_MS);
			if (++quiet < USB_HUB_RESCAN_POLLS)
				continue;
		}
		quiet = 0;
		while (!hub->stopping && drv_usb_hub_changed(hub->device) == EBUSY)
			usb_hub_sleep_ms(hub, USB_HUB_RETRY_MS);
	}
}

/*
 * Reads the hub descriptor and powers every port.  A hub that switches no
 * power ignores the request; it costs nothing to send.
 */
static int
usb_hub_setup(
	struct usb_hub *hub)
{
	uint8_t descriptor[71];
	uint16_t characteristics;
	size_t actual;
	unsigned port;
	int error;

	actual = 0;
	error = drv_usb_control(hub->device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_DEVICE,
	    USB_HUB_GET_DESCRIPTOR, (uint16_t)(USB_HUB_DESCRIPTOR << 8), 0,
	    descriptor, sizeof(descriptor), USB_HUB_REQUEST_TIMEOUT_MS, &actual);
	if (error != 0 || actual < 7U || descriptor[1] != USB_HUB_DESCRIPTOR ||
	    descriptor[2] == 0) {
		kern_logf("usb-hub: hub %u descriptor unreadable (%d)\n",
		    drv_usb_device_address(hub->device), error != 0 ? error : EIO);
		return error != 0 ? error : EIO;
	}
	hub->ports = descriptor[2];
	characteristics = (uint16_t)(descriptor[3] | (descriptor[4] << 8));
	hub->think_time = (characteristics >> 5) & 3U;
	hub->power_good_ms = descriptor[5] * 2U;
	if (hub->power_good_ms < USB_HUB_POWER_MINIMUM_MS)
		hub->power_good_ms = USB_HUB_POWER_MINIMUM_MS;
	if (hub->ports > 8U * sizeof(hub->change) - 1U)
		hub->ports = 8U * sizeof(hub->change) - 1U;

	for (port = 1; port <= hub->ports; port++)
		(void)drv_usb_control(hub->device,
		    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_OTHER,
		    USB_HUB_SET_FEATURE, USB_HUB_PORT_POWER, (uint16_t)port,
		    NULL, 0, USB_HUB_REQUEST_TIMEOUT_MS, &actual);
	usb_hub_sleep_ms(hub, hub->power_good_ms);
	return 0;
}

/* Reads a port: wPortStatus in the low half, wPortChange in the high half. */
static int
usb_hub_port_status(
	void *context,
	unsigned port,
	uint32_t *status)
{
	struct usb_hub *hub;
	uint8_t bytes[4];
	size_t actual;
	int error;

	hub = context;
	actual = 0;
	error = drv_usb_control(hub->device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_OTHER,
	    USB_HUB_GET_STATUS, 0, (uint16_t)port, bytes, sizeof(bytes),
	    USB_HUB_REQUEST_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;
	if (actual != sizeof(bytes))
		return EIO;
	*status = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
	return 0;
}

/* Clears one port feature. */
static int
usb_hub_clear_feature(
	void *context,
	unsigned port,
	uint16_t feature)
{
	struct usb_hub *hub;
	size_t actual;

	hub = context;
	return drv_usb_control(hub->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_OTHER,
	    USB_HUB_CLEAR_FEATURE, feature, (uint16_t)port, NULL, 0,
	    USB_HUB_REQUEST_TIMEOUT_MS, &actual);
}

/*
 * Resets a port and waits until the hub says the reset is over, then for
 * the recovery time a device is allowed after a reset.
 */
static int
usb_hub_port_reset(
	void *context,
	unsigned port)
{
	struct usb_hub *hub;
	uint32_t status;
	size_t actual;
	unsigned waited;
	int error;

	hub = context;
	error = drv_usb_control(hub->device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_OTHER,
	    USB_HUB_SET_FEATURE, USB_HUB_PORT_RESET, (uint16_t)port, NULL, 0,
	    USB_HUB_REQUEST_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;

	for (waited = 0; waited < USB_HUB_RESET_TIMEOUT_MS; waited += 10U) {
		usb_hub_sleep_ms(hub, 10U);
		error = usb_hub_port_status(hub, port, &status);
		if (error != 0)
			return error;
		if ((status & (1U << USB_HUB_C_PORT_RESET)) != 0 ||
		    (status & (1U << USB_HUB_PORT_RESET)) == 0)
			break;
	}
	if (waited >= USB_HUB_RESET_TIMEOUT_MS)
		return ETIMEDOUT;
	(void)usb_hub_clear_feature(hub, port, USB_HUB_C_PORT_RESET);
	usb_hub_sleep_ms(hub, USB_HUB_RESET_RECOVERY_MS);
	return (status & 3U) == 3U ? 0 : ENODEV;
}

/* Sleeps on the hub's thread, waking early when the hub is being stopped. */
static void
usb_hub_sleep_ms(
	struct usb_hub *hub,
	unsigned milliseconds)
{
	if (hub->stopping)
		return;
	sched_sleep(sched_ticks() + kern_ms_to_ticks(milliseconds));
}
