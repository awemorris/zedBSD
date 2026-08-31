/*
 * ws004-p031 test-only USB HID interrupt checkpoint
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <stdint.h>
#include <string.h>

#define USB_HID_CLASS	0x03U
#define CHECKPOINT_DRAIN_TIMEOUT_MS	5000U
#define CHECKPOINT_MARKER_LIMIT	16U
#define CHECKPOINT_DETACH_MARKER_LIMIT	4U
#define CHECKPOINT_WORK_ARM	(1U << 0)
#define CHECKPOINT_WORK_COMPLETE	(1U << 1)

struct usb_hid_checkpoint {
	struct drv_usb_interface *interface;
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_urb *urb;
	struct thread *worker;
	struct spinlock lock;
	uint8_t *buffer;
	size_t buffer_size;
	unsigned generation;
	unsigned work_pending;
	unsigned stopping;
	unsigned submit_active;
	unsigned submit_count;
	unsigned completion_count;
	unsigned submit_markers;
	unsigned completion_markers;
	unsigned detach_markers;
};

static volatile unsigned checkpoint_generation;

static unsigned
checkpoint_bus_number(const struct usb_hid_checkpoint *checkpoint)
{
	return drv_usb_bus_number(drv_usb_device_bus(checkpoint->device));
}

static void
checkpoint_report_submit(struct usb_hid_checkpoint *checkpoint, int error)
{
	checkpoint->submit_count++;
	if (checkpoint->submit_markers >= CHECKPOINT_MARKER_LIMIT)
		return;
	checkpoint->submit_markers++;
	hal_printf("usb-hid-checkpoint: submit generation=%u usb%u device=%u "
	    "interface=%u sequence=%u error=%d\n", checkpoint->generation,
	    checkpoint_bus_number(checkpoint),
	    drv_usb_device_address(checkpoint->device),
	    drv_usb_interface_number(checkpoint->interface),
	    checkpoint->submit_count, error);
}

static void
checkpoint_report_completion(struct usb_hid_checkpoint *checkpoint,
	int drain_error)
{
	enum drv_usb_urb_status status = drv_usb_urb_status(checkpoint->urb);
	size_t actual = drv_usb_urb_actual_length(checkpoint->urb);

	checkpoint->completion_count++;
	if (checkpoint->completion_markers >= CHECKPOINT_MARKER_LIMIT)
		return;
	checkpoint->completion_markers++;
	hal_printf("usb-hid-checkpoint: completion generation=%u usb%u "
	    "device=%u interface=%u sequence=%u status=%u actual=%u "
	    "drain-error=%d\n", checkpoint->generation,
	    checkpoint_bus_number(checkpoint),
	    drv_usb_device_address(checkpoint->device),
	    drv_usb_interface_number(checkpoint->interface),
	    checkpoint->completion_count, (unsigned)status, (unsigned)actual,
	    drain_error);
}

static void
checkpoint_completion(struct drv_usb_urb *urb, void *argument)
{
	struct usb_hid_checkpoint *checkpoint = argument;
	struct thread *worker;
	unsigned long irq;

	if (checkpoint == NULL || urb != checkpoint->urb)
		return;
	irq = spin_lock_irqsave(&checkpoint->lock);
	checkpoint->work_pending |= CHECKPOINT_WORK_COMPLETE;
	worker = checkpoint->worker;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	if (worker != NULL)
		kernel_notify_task(worker->task);
}

static int
checkpoint_begin_submit(struct usb_hid_checkpoint *checkpoint)
{
	unsigned long irq = spin_lock_irqsave(&checkpoint->lock);
	int admitted = !checkpoint->stopping && !checkpoint->submit_active;

	if (admitted)
		checkpoint->submit_active = 1U;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	return admitted ? 0 : EBUSY;
}

static void
checkpoint_end_submit(struct usb_hid_checkpoint *checkpoint)
{
	unsigned long irq = spin_lock_irqsave(&checkpoint->lock);

	if (!checkpoint->submit_active)
		__builtin_trap();
	checkpoint->submit_active = 0;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
}

static int
checkpoint_arm(struct usb_hid_checkpoint *checkpoint)
{
	int error;

	error = checkpoint_begin_submit(checkpoint);
	if (error != 0)
		return error;
	memset(checkpoint->buffer, 0, checkpoint->buffer_size);
	error = drv_usb_urb_setup(checkpoint->urb, checkpoint->buffer,
	    checkpoint->buffer_size, DRV_USB_URB_SHORT_OK, 0,
	    checkpoint_completion, checkpoint);
	if (error == 0)
		error = drv_usb_urb_submit(checkpoint->urb);
	checkpoint_end_submit(checkpoint);
	checkpoint_report_submit(checkpoint, error);
	return error;
}

static unsigned
checkpoint_take_work(struct usb_hid_checkpoint *checkpoint, int *stopping)
{
	unsigned long irq = spin_lock_irqsave(&checkpoint->lock);
	unsigned work = checkpoint->work_pending;

	checkpoint->work_pending = 0;
	*stopping = checkpoint->stopping != 0;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	return work;
}

static void
checkpoint_worker(void *argument)
{
	struct usb_hid_checkpoint *checkpoint = argument;

	for (;;) {
		unsigned work;
		int drain_error, stopping;

		work = checkpoint_take_work(checkpoint, &stopping);
		if (stopping)
			return;
		if (work == 0) {
			kernel_wait_task();
			continue;
		}
		if ((work & CHECKPOINT_WORK_COMPLETE) != 0) {
			drain_error = drv_usb_urb_drain(checkpoint->urb,
			    CHECKPOINT_DRAIN_TIMEOUT_MS);
			checkpoint_report_completion(checkpoint, drain_error);
			if (drain_error != 0 ||
			    drv_usb_urb_status(checkpoint->urb) !=
			    DRV_USB_URB_COMPLETE)
				continue;
			work |= CHECKPOINT_WORK_ARM;
		}
		if ((work & CHECKPOINT_WORK_ARM) != 0)
			(void)checkpoint_arm(checkpoint);
	}
}

static int
checkpoint_buffer_size(struct drv_usb_interface *interface,
	struct drv_usb_endpoint *endpoint, size_t *result)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	enum drv_usb_speed speed;
	uint16_t maximum;
	unsigned packets, payload;

	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL || descriptor->interval == 0)
		return EINVAL;
	maximum = drv_usb_endpoint_max_packet_size(endpoint);
	payload = maximum & 0x07ffU;
	packets = 1U + ((maximum >> 11) & 3U);
	speed = drv_usb_device_speed(drv_usb_interface_device(interface));
	if (payload == 0 || (maximum & 0xe000U) != 0 || packets == 4U ||
	    (speed == DRV_USB_SPEED_LOW && (payload > 8U || packets != 1U)) ||
	    (speed == DRV_USB_SPEED_FULL &&
	    (payload > 64U || packets != 1U)) ||
	    (speed == DRV_USB_SPEED_HIGH && payload > 1024U) ||
	    (speed != DRV_USB_SPEED_LOW && speed != DRV_USB_SPEED_FULL &&
	    speed != DRV_USB_SPEED_HIGH))
		return EINVAL;
	*result = (size_t)payload * packets;
	return 0;
}

static int
checkpoint_attach(struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	const struct drv_usb_interface_descriptor *descriptor;
	struct usb_hid_checkpoint *checkpoint;
	struct drv_usb_endpoint *endpoint, *extra;
	struct thread *worker;
	size_t buffer_size;
	int error;

	(void)id;
	descriptor = drv_usb_interface_descriptor(interface);
	if (descriptor == NULL || descriptor->interface_class != USB_HID_CLASS)
		return ENODEV;
	endpoint = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, NULL);
	if (endpoint == NULL)
		return ENODEV;
	extra = drv_usb_interface_find_endpoint(interface,
	    DRV_USB_TRANSFER_INTERRUPT, DRV_USB_DIR_IN, endpoint);
	if (extra != NULL)
		return EOPNOTSUPP;
	error = checkpoint_buffer_size(interface, endpoint, &buffer_size);
	if (error != 0)
		return error;
	checkpoint = hal_malloc(sizeof(*checkpoint));
	if (checkpoint == NULL)
		return ENOMEM;
	memset(checkpoint, 0, sizeof(*checkpoint));
	checkpoint->buffer = hal_malloc(buffer_size);
	if (checkpoint->buffer == NULL) {
		hal_free(checkpoint);
		return ENOMEM;
	}
	checkpoint->interface = interface;
	checkpoint->device = drv_usb_interface_device(interface);
	checkpoint->endpoint = endpoint;
	checkpoint->buffer_size = buffer_size;
	checkpoint->generation = __atomic_add_fetch(&checkpoint_generation, 1U,
	    __ATOMIC_RELAXED);
	checkpoint->work_pending = CHECKPOINT_WORK_ARM;
	spin_init(&checkpoint->lock, LOCK_RANK_DEVICE, "usb-hid-checkpoint");
	checkpoint->urb = drv_usb_urb_alloc(checkpoint->device, endpoint, 0);
	if (checkpoint->urb == NULL) {
		hal_free(checkpoint->buffer);
		hal_free(checkpoint);
		return ENOMEM;
	}
	error = drv_usb_interface_set_driver_data(interface, checkpoint);
	if (error != 0) {
		drv_usb_urb_free(checkpoint->urb);
		hal_free(checkpoint->buffer);
		hal_free(checkpoint);
		return error;
	}
	error = kthread_create(checkpoint_worker, checkpoint,
	    SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0) {
		(void)drv_usb_interface_set_driver_data(interface, NULL);
		drv_usb_urb_free(checkpoint->urb);
		hal_free(checkpoint->buffer);
		hal_free(checkpoint);
		return error;
	}
	checkpoint->worker = worker;
	hal_printf("usb-hid-checkpoint: attach generation=%u usb%u device=%u "
	    "interface=%u endpoint=%02x length=%u\n", checkpoint->generation,
	    checkpoint_bus_number(checkpoint),
	    drv_usb_device_address(checkpoint->device),
	    drv_usb_interface_number(interface),
	    drv_usb_endpoint_address(endpoint), (unsigned)buffer_size);
	thread_start(worker);
	return 0;
}

static void
checkpoint_close_admission(struct usb_hid_checkpoint *checkpoint)
{
	struct thread *worker;
	unsigned active;
	unsigned long irq;

	irq = spin_lock_irqsave(&checkpoint->lock);
	checkpoint->stopping = 1U;
	worker = checkpoint->worker;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	if (worker != NULL)
		kernel_notify_task(worker->task);
	for (;;) {
		irq = spin_lock_irqsave(&checkpoint->lock);
		active = checkpoint->submit_active;
		spin_unlock_irqrestore(&checkpoint->lock, irq);
		if (!active)
			return;
		sched_yield();
	}
}

static int
checkpoint_join_worker(struct usb_hid_checkpoint *checkpoint)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&checkpoint->lock);
	worker = checkpoint->worker;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	if (worker == NULL)
		return 0;
	if (worker == curthread)
		return EBUSY;
	kernel_notify_task(worker->task);
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	    THREAD_ZOMBIE)
		sched_yield();
	error = thread_wait(worker, NULL);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&checkpoint->lock);
	if (checkpoint->worker != worker) {
		spin_unlock_irqrestore(&checkpoint->lock, irq);
		__builtin_trap();
	}
	checkpoint->worker = NULL;
	spin_unlock_irqrestore(&checkpoint->lock, irq);
	return 0;
}

static void
checkpoint_report_detach(struct usb_hid_checkpoint *checkpoint,
	int cancel_error, int drain_error, int join_error)
{
	if (checkpoint->detach_markers >= CHECKPOINT_DETACH_MARKER_LIMIT)
		return;
	checkpoint->detach_markers++;
	hal_printf("usb-hid-checkpoint: detach generation=%u usb%u device=%u "
	    "interface=%u submits=%u completions=%u cancel-error=%d "
	    "drain-error=%d join-error=%d\n",
	    checkpoint->generation, checkpoint_bus_number(checkpoint),
	    drv_usb_device_address(checkpoint->device),
	    drv_usb_interface_number(checkpoint->interface),
	    checkpoint->submit_count, checkpoint->completion_count, cancel_error,
	    drain_error, join_error);
}

static int
checkpoint_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct usb_hid_checkpoint *checkpoint =
	    drv_usb_interface_driver_data(interface);
	enum drv_usb_urb_status status;
	int cancel_error = 0, drain_error, join_error;

	(void)flags;
	if (checkpoint == NULL)
		return 0;
	checkpoint_close_admission(checkpoint);
	status = drv_usb_urb_status(checkpoint->urb);
	if (status == DRV_USB_URB_PENDING)
		cancel_error = drv_usb_urb_cancel(checkpoint->urb);
	drain_error = drv_usb_urb_drain(checkpoint->urb,
	    CHECKPOINT_DRAIN_TIMEOUT_MS);
	join_error = checkpoint_join_worker(checkpoint);
	if (drain_error != 0 || join_error != 0) {
		int error = drain_error != 0 ? drain_error : join_error;

		checkpoint_report_detach(checkpoint, cancel_error, drain_error,
		    join_error);
		return error;
	}
	/* A failed dequeue which races a natural terminal completion is harmless
	 * only after drain proves both terminal publication and HCD release. */
	checkpoint_report_detach(checkpoint, cancel_error, 0, 0);
	(void)drv_usb_interface_set_driver_data(interface, NULL);
	drv_usb_urb_free(checkpoint->urb);
	hal_free(checkpoint->buffer);
	hal_free(checkpoint);
	return 0;
}

static const struct drv_usb_id checkpoint_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS,
	.interface_class = USB_HID_CLASS
}};

static struct drv_usb_driver checkpoint_driver = {
	.name = "usb-hid-checkpoint",
	.ids = checkpoint_ids,
	.id_count = sizeof(checkpoint_ids) / sizeof(checkpoint_ids[0]),
	.attach = checkpoint_attach,
	.detach = checkpoint_detach
};

int
usb_hid_checkpoint_driver_register(void)
{
	return drv_usb_driver_register(&checkpoint_driver);
}
