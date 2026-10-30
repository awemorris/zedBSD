/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The camera framework: /dev/camN, its owner, its buffers and queues, and
 * the stream state shared with the source drivers.
 *
 * A buffer is a slot.  A slot is UNUSED until its owner queues it, FREE
 * while it waits in the free queue, ACTIVE while a source fills it, and
 * DONE while it waits in the done queue to be dequeued.  The source takes
 * slots and completes or discards them without sleeping; everything that
 * sleeps (the source's mode, start and stop, buffer allocation) runs
 * under the device mutex and outside the spinlock.  Only the framework
 * stops a stream: STREAM_OFF, the owner's close, the device's retirement,
 * and STREAM_OFF out of FAULT are the only callers of the source's stop.
 *
 * Imported buffers are GPU allocations an application exported: the slot
 * keeps the capability's handle and writes the frame through an uncached
 * view of its pages, since the GPU reads that memory without caching.
 */

#include <drivers/camera/cam.h>
#include <drivers/gpu/gpu-scanout.h>
#include <kern/cdev.h>
#include <kern/clock.h>
#include <kern/file.h>
#include <kern/handle.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/page.h>
#include <kern/pmem.h>
#include <kern/poll.h>
#include <kern/sched.h>
#include <kern/uaccess.h>
#include <kern/vm-device.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/system.h>
#include <uapi/time.h>

#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The first dev_t of the camera major; camN is the base plus N. */
#define CAM_DEVICE_BASE		0x00170000U

/* The buffers a camera allows when its source names no limit. */
#define CAM_DEFAULT_BUFFERS	CAM_BUFFERS_MAX

/* The fewest buffers a stream starts with. */
#define CAM_MIN_START_BUFFERS	1U

/*
 * The GPU core and the uncached views are in only some kernels.  An
 * imported buffer needs both, so they are declared weak and tested before
 * use; a kernel without them refuses CAM_MEMORY_IMPORT.
 */
extern int drv_gpu_shared_backing(struct kernel_handle *handle, struct drv_gpu_scanout_backing *backing, uint64_t *device_identity) __attribute__((weak));
extern int kern_pmem_map_uncached(const struct kern_pmem *run, void **mapped) __attribute__((weak));
extern int kern_pmem_unmap_uncached(void *mapped, size_t size) __attribute__((weak));

/* The system's events, which a kernel may be built without. */
extern void kern_system_event_post(uint32_t class_bit, uint32_t action, int32_t value, const char *subject, const char *detail) __attribute__((weak));

/*
 * The states of a stream.
 *
 * IDLE has no buffers, BUFFERS has buffers and no running source,
 * STREAMING has a running source, STOPPING is the source's stop in
 * progress (no slot is handed out), and FAULT is a source that cannot go
 * on and waits for its owner to stop it.
 */
enum cam_state {
	CAM_STATE_IDLE,
	CAM_STATE_BUFFERS,
	CAM_STATE_STREAMING,
	CAM_STATE_STOPPING,
	CAM_STATE_FAULT
};

/*
 * The life of one slot, as the comment at the top of the file describes.
 */
enum cam_slot_state {
	CAM_SLOT_UNUSED,
	CAM_SLOT_FREE,
	CAM_SLOT_ACTIVE,
	CAM_SLOT_DONE
};

/*
 * One buffer of a stream.
 *
 * The memory is one physically contiguous run, zeroed when it is
 * allocated and addressed through the cached direct map.  An imported
 * slot instead holds the GPU capability it was queued with and addresses
 * its pages through an uncached view; it has neither until its first
 * queue.  next links the slot into the free or the done queue; the device
 * spinlock guards state, next, mappings and metadata.
 */
struct drv_cam_slot {
	struct drv_cam_device *device;
	struct drv_cam_slot *next;
	unsigned index;
	enum cam_slot_state state;
	struct kern_pmem memory;
	struct kernel_handle *import;
	uint8_t *address;
	size_t bytes;
	size_t capacity;
	unsigned mappings;
	struct cam_frame_metadata metadata;
};

/*
 * One registered camera and its node.
 *
 * Registration holds one reference and the published cdev generation
 * another, so the structure outlives a retirement while an open file or
 * a mapping still names it.  The mutex serializes the requests that
 * change the stream and the retirement; the spinlock guards the queues,
 * the slot states, the counters, the admission count and the state.  The
 * mutex is always taken before the spinlock.
 */
struct drv_cam_device {
	struct drv_cam_device_desc desc;
	char driver_name[16];
	unsigned number;
	refcount_t references;
	struct mutex mutex;
	struct spinlock lock;
	struct wait_queue waitq;
	struct cdev *node;
	enum cam_state state;
	unsigned gone;
	unsigned admissions;
	struct file *owner;
	unsigned format_set;
	struct drv_cam_mode_result mode;
	uint32_t memory;
	unsigned buffer_count;
	unsigned mapped;
	struct drv_cam_slot slots[CAM_BUFFERS_MAX];
	struct drv_cam_slot *free_head;
	struct drv_cam_slot *free_tail;
	struct drv_cam_slot *done_head;
	struct drv_cam_slot *done_tail;
	uint32_t sequence;
	uint32_t last_sequence;
	unsigned have_last_sequence;
	uint64_t last_frame_ns;
	int fault_error;
	uint64_t frames;
	uint64_t dropped_no_buffer;
	uint64_t dropped_overflow;
	uint64_t dropped_error;
	uint64_t dropped_transport;
	uint64_t sequence_gaps;
};

/*
 * The cameras by node number.
 *
 * A number is taken at registration and given back after the node is
 * withdrawn, so a camera plugged in again reuses the lowest free name.
 * cam_registry_lock guards the table.
 */
static struct drv_cam_device *cam_numbers[CAM_DEVICES_MAX];

/* Guards cam_numbers. */
static struct spinlock cam_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "camera registry", 0, 0
};

/*
 * Forward declaration.
 */
static int cam_number_reserve(struct drv_cam_device *device);
static void cam_number_release(struct drv_cam_device *device);
static void cam_device_release(void *argument);
static uint64_t cam_now_ns(void);
static void cam_post(const struct drv_cam_device *device, uint32_t action);
static int cam_enter(struct drv_cam_device *device);
static void cam_leave(struct drv_cam_device *device);
static void cam_queue_append(struct drv_cam_slot **head, struct drv_cam_slot **tail, struct drv_cam_slot *slot);
static struct drv_cam_slot *cam_queue_pop(struct drv_cam_slot **head, struct drv_cam_slot **tail);
static void cam_slots_reset_locked(struct drv_cam_device *device);
static void cam_stream_stop(struct drv_cam_device *device);
static void cam_buffers_free(struct drv_cam_device *device);
static int cam_buffers_allocate(struct drv_cam_device *device, unsigned count);
static void cam_buffers_prepare_import(struct drv_cam_device *device, unsigned count);
static int cam_import_available(const struct drv_cam_device *device);
static int cam_slot_import(struct drv_cam_device *device, struct drv_cam_slot *slot, int32_t descriptor);
static int cam_record_check(uint32_t version, uint32_t size, size_t expected);
static void cam_format_from_result(struct cam_format *format, const struct drv_cam_mode_result *result);
static int cam_get_info(struct drv_cam_device *device, uintptr_t argument);
static int cam_enum_format(struct drv_cam_device *device, uintptr_t argument);
static int cam_enum_mode(struct drv_cam_device *device, uintptr_t argument);
static int cam_enum_interval(struct drv_cam_device *device, uintptr_t argument);
static int cam_get_format(struct drv_cam_device *device, uintptr_t argument);
static int cam_set_format(struct drv_cam_device *device, struct file *file, uintptr_t argument, int trying);
static int cam_request_buffers(struct drv_cam_device *device, struct file *file, uintptr_t argument);
static int cam_query_buffer(struct drv_cam_device *device, uintptr_t argument);
static int cam_queue_buffer(struct drv_cam_device *device, struct file *file, uintptr_t argument);
static int cam_dequeue_buffer(struct drv_cam_device *device, struct file *file, uintptr_t argument);
static int cam_stream_on(struct drv_cam_device *device, struct file *file);
static int cam_stream_off(struct drv_cam_device *device, struct file *file);
static int cam_control_info(struct drv_cam_device *device, uintptr_t argument);
static int cam_control_get(struct drv_cam_device *device, uintptr_t argument);
static int cam_control_set(struct drv_cam_device *device, struct file *file, uintptr_t argument);
static int cam_get_counters(struct drv_cam_device *device, uintptr_t argument);
static uint32_t cam_slot_flags_locked(const struct drv_cam_slot *slot);
static int cam_open(struct file *file);
static int cam_close(struct file *file);
static ssize_t cam_read(struct file *file, void *buffer, size_t size);
static ssize_t cam_write(struct file *file, const void *buffer, size_t size);
static int cam_ioctl(struct file *file, unsigned long command, uintptr_t argument);
static int cam_ioctl_admitted(struct drv_cam_device *device, struct file *file, unsigned long command, uintptr_t argument);
static int cam_poll(struct file *file, short events, short *revents);
static int cam_mmap(struct file *file, off_t offset, size_t bytes, uint32_t prot, struct vm_device_mapping **result);
static void cam_mapping_release(void *owner);

/*
 * Registers one camera and publishes /dev/camN.
 */
int
drv_cam_register(
	const struct drv_cam_device_desc *desc,
	struct drv_cam_device **result)
{
	/* The node's dispatch shared by every published generation. */
	static const struct cdev_ops cam_operations = {
		.open = cam_open,
		.close = cam_close,
		.read = cam_read,
		.write = cam_write,
		.ioctl = cam_ioctl,
		.poll = cam_poll,
		.mmap = cam_mmap
	};
	struct drv_cam_device *device;
	struct cdev *node;
	char name[32];
	unsigned index;
	int error;

	/* Refuses a missing destination or description. */
	if (result == NULL || desc == NULL)
		return EINVAL;

	/* Refuses a source without the operations every stream needs. */
	if (desc->source == NULL)
		return EINVAL;
	if (desc->source->enum_format == NULL || desc->source->enum_mode == NULL)
		return EINVAL;
	if (desc->source->set_mode == NULL || desc->source->try_mode == NULL)
		return EINVAL;
	if (desc->source->start == NULL || desc->source->stop == NULL)
		return EINVAL;

	/* Allocates the device. */
	device = kern_calloc(1U, sizeof(*device));
	if (device == NULL)
		return ENOMEM;

	/* Copies the description; the driver's name is kept in the device. */
	device->desc = *desc;
	if (desc->driver_name != NULL)
		kern_snprintf(device->driver_name, sizeof(device->driver_name), "%s", desc->driver_name);
	device->desc.driver_name = device->driver_name;
	device->desc.device_name[sizeof(device->desc.device_name) - 1U] = '\0';
	device->desc.location[sizeof(device->desc.location) - 1U] = '\0';

	/* A source that names no limit gets the framework's. */
	if (device->desc.max_buffers == 0U || device->desc.max_buffers > CAM_BUFFERS_MAX)
		device->desc.max_buffers = CAM_DEFAULT_BUFFERS;

	/* Initializes an idle device that holds only its registration reference. */
	refcount_init(&device->references, 1U);
	error = mutex_init(&device->mutex, LOCK_RANK_DEVICE, "camera");
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* Initializes the queue lock, the wait queue and the idle state of the device. */
	spin_init(&device->lock, LOCK_RANK_DEVICE, "camera queues");
	waitq_init(&device->waitq, "camera");
	device->state = CAM_STATE_IDLE;
	device->memory = CAM_MEMORY_DEVICE;

	/* Initializes every slot as unused. */
	for (index = 0U; index < CAM_BUFFERS_MAX; index++) {
		device->slots[index].device = device;
		device->slots[index].index = index;
		device->slots[index].state = CAM_SLOT_UNUSED;
	}

	/* Takes the lowest free node number. */
	error = cam_number_reserve(device);
	if (error != 0) {
		kern_free(device);
		return error;
	}

	/* The generation keeps the device alive until its finalizer runs. */
	kern_snprintf(name, sizeof(name), "cam%u", device->number);
	refcount_get(&device->references);
	error = cdev_register_managed(name,
	    (dev_t)(CAM_DEVICE_BASE + device->number),
	    &cam_operations,
	    device,
	    cam_device_release,
	    &node);
	if (error != 0) {
		cam_number_release(device);
		cam_device_release(device);
		cam_device_release(device);
		return error;
	}

	/* Keeps the caller reference for the retirement. */
	device->node = node;

	/* Tells the seat that a camera came, so its user is given the node. */
	cam_post(device, KERN_SYSTEM_EVENT_ADD);

	/* Succeeded: the camera is visible in /dev. */
	kern_logf("cam: %s: %s (%s) at %s\n", name, device->desc.device_name, device->driver_name, device->desc.location);
	*result = device;
	return 0;
}

/*
 * Retires one camera and withdraws its node.
 */
void
drv_cam_unregister(
	struct drv_cam_device *device)
{
	unsigned long irq;
	uint64_t observed;

	/* Nothing to retire. */
	if (device == NULL)
		return;

	/*
	 * GONE refuses every new request and wakes the dequeue waiters, which
	 * then leave with ENODEV.  A request already admitted that waits for
	 * the mutex finds GONE when it gets it and leaves too.
	 */
	irq = spin_lock_irqsave(&device->lock);

	device->gone = 1U;
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Tells pollers that the camera is gone. */
	poll_notify();

	/* Stops a running or failed stream through the source, once. */
	mutex_lock(&device->mutex);
	cam_stream_stop(device);
	mutex_unlock(&device->mutex);

	/* Waits until no request is in progress, so no one calls the source any more. */
	irq = spin_lock_irqsave(&device->lock);

	while (device->admissions != 0U) {
		observed = waitq_sequence(&device->waitq);
		(void)waitq_sleep(&device->waitq, &device->lock, observed, 0, 0U);
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Withdraws the name; a stale inode keeps only the device structure alive. */
	(void)cdev_unregister(device->node);
	cdev_release(device->node);
	device->node = NULL;
	cam_post(device, KERN_SYSTEM_EVENT_REMOVE);

	/* Gives back the number for the next camera. */
	cam_number_release(device);

	/* Drops the registration reference. */
	cam_device_release(device);
}

/*
 * Begins a new frame and reports its sequence number.
 */
uint32_t
drv_cam_frame_begin(
	struct drv_cam_device *device)
{
	unsigned long irq;
	uint32_t sequence;

	/* The sequence moves once per frame begun, delivered or not. */
	irq = spin_lock_irqsave(&device->lock);

	sequence = device->sequence;
	device->sequence++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports the frame's number. */
	return sequence;
}

/*
 * Takes the first free slot for a frame.
 */
int
drv_cam_slot_take(
	struct drv_cam_device *device,
	uint32_t sequence,
	struct drv_cam_slot **slot)
{
	struct drv_cam_slot *taken;
	unsigned long irq;

	/* Refuses a missing destination. */
	if (slot == NULL)
		return EINVAL;

	/* Hands out a slot only while the stream runs. */
	taken = NULL;
	irq = spin_lock_irqsave(&device->lock);

	if (device->state == CAM_STATE_STREAMING && device->gone == 0U)
		taken = cam_queue_pop(&device->free_head, &device->free_tail);

	/* ACTIVE keeps the slot out of every queue until done or discard. */
	if (taken != NULL) {
		taken->state = CAM_SLOT_ACTIVE;
		kern_memset(&taken->metadata, 0, sizeof(taken->metadata));
		taken->metadata.sequence = sequence;
		taken->metadata.exposure = CAM_METADATA_UNKNOWN;
		taken->metadata.analogue_gain = CAM_METADATA_UNKNOWN;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports that no slot is free or the stream does not run. */
	if (taken == NULL)
		return EAGAIN;

	/* Succeeded: the source owns the slot until done or discard. */
	*slot = taken;
	return 0;
}

/*
 * Reports the kernel address a CPU-writing source fills.
 */
void *
drv_cam_slot_address(
	const struct drv_cam_slot *slot)
{
	/* The direct-map address of the slot's run, or the uncached view of an import. */
	return slot->address;
}

/*
 * Reports the bus address a DMA source gives its hardware.
 */
uint64_t
drv_cam_slot_bus_address(
	const struct drv_cam_slot *slot)
{
	/* A CPU-writing source's slot has no bus address of its own. */
	if ((slot->device->desc.flags & DRV_CAM_SOURCE_DMA) == 0U)
		return 0U;

	/* The physical address seen through the device's DMA window. */
	return (uint64_t)slot->memory.paddr + slot->device->desc.dma_bus_offset;
}

/*
 * Reports how many bytes a slot holds.
 */
size_t
drv_cam_slot_capacity(
	const struct drv_cam_slot *slot)
{
	/* The largest frame of the stream's mode. */
	return slot->capacity;
}

/*
 * Completes an active slot and wakes its readers.
 */
void
drv_cam_slot_done(
	struct drv_cam_device *device,
	struct drv_cam_slot *slot,
	size_t used_bytes,
	const struct drv_cam_frame_info *info)
{
	unsigned long irq;
	uint64_t now;
	uint32_t gap;

	/* The time the frame finished, which stands in for a missing timestamp. */
	now = cam_now_ns();

	/* Never reports more bytes than the slot holds. */
	if (used_bytes > slot->capacity)
		used_bytes = slot->capacity;

	/* Moves the slot to the done queue with its metadata. */
	irq = spin_lock_irqsave(&device->lock);

	/* A slot that is no longer active was reset by a stop; it is not the source's to complete. */
	if (slot->state != CAM_SLOT_ACTIVE) {
		spin_unlock_irqrestore(&device->lock, irq);
		return;
	}

	/* Records how much of the slot the source filled and when. */
	slot->metadata.used_bytes = (uint32_t)used_bytes;
	slot->metadata.timestamp_ns = now;
	if (info != NULL) {
		if (info->timestamp_ns != 0U)
			slot->metadata.timestamp_ns = info->timestamp_ns;
		slot->metadata.flags = info->flags;
		slot->metadata.device_pts = info->device_pts;
		slot->metadata.device_scr = info->device_scr;
	}

	/*
	 * The gap between two delivered sequence numbers is the number of
	 * frames the source began but could not deliver.
	 */
	if (device->have_last_sequence != 0U) {
		gap = slot->metadata.sequence - device->last_sequence;
		if (gap > 1U)
			device->sequence_gaps += gap - 1U;
	}

	/* Remembers this frame so the next one can be checked for a gap. */
	device->last_sequence = slot->metadata.sequence;
	device->have_last_sequence = 1U;
	device->last_frame_ns = now;
	device->frames++;

	/* DONE waits for the owner's dequeue. */
	slot->state = CAM_SLOT_DONE;
	cam_queue_append(&device->done_head, &device->done_tail, slot);
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Tells pollers that a frame is ready. */
	poll_notify();
}

/*
 * Returns an active slot unfilled to the head of the free queue.
 */
void
drv_cam_slot_discard(
	struct drv_cam_device *device,
	struct drv_cam_slot *slot)
{
	unsigned long irq;

	/* Puts the slot first so the next frame takes it at once. */
	irq = spin_lock_irqsave(&device->lock);

	if (slot->state == CAM_SLOT_ACTIVE) {
		slot->state = CAM_SLOT_FREE;
		slot->next = device->free_head;
		device->free_head = slot;
		if (device->free_tail == NULL)
			device->free_tail = slot;
	}

	spin_unlock_irqrestore(&device->lock, irq);
}

/*
 * Counts a frame the source could not deliver.
 */
void
drv_cam_frame_dropped(
	struct drv_cam_device *device,
	uint32_t reason)
{
	unsigned long irq;

	/* Each reason has a counter of its own. */
	irq = spin_lock_irqsave(&device->lock);

	/* Selects the counter of the reason. */
	switch (reason) {
	case CAM_DROP_NO_BUFFER:
		device->dropped_no_buffer++;
		break;

	case CAM_DROP_OVERFLOW:
		device->dropped_overflow++;
		break;

	case CAM_DROP_TRANSPORT:
		device->dropped_transport++;
		break;

	default:
		device->dropped_error++;
		break;
	}

	spin_unlock_irqrestore(&device->lock, irq);
}

/*
 * Tells that the source cannot go on.
 */
void
drv_cam_source_fault(
	struct drv_cam_device *device,
	int error)
{
	unsigned long irq;

	/* FAULT holds until the owner stops the stream, which calls the source's stop. */
	irq = spin_lock_irqsave(&device->lock);

	if (device->state == CAM_STATE_STREAMING) {
		device->state = CAM_STATE_FAULT;
		device->fault_error = error;
	}

	/* Wakes the waiters so they see the fault. */
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Tells pollers about the error. */
	poll_notify();
}

/*
 * Tells pollers that a source counter worth looking at has changed.
 */
void
drv_cam_source_counters_changed(
	struct drv_cam_device *device)
{
	UNUSED_PARAMETER(device);

	/* A poller re-reads the counters when it is woken. */
	poll_notify();
}

/* Takes the lowest free node number for a device. */
static int
cam_number_reserve(
	struct drv_cam_device *device)
{
	unsigned long irq;
	unsigned number;
	int error;

	/* Finds the first empty entry of the table. */
	error = ENOSPC;
	irq = spin_lock_irqsave(&cam_registry_lock);

	for (number = 0U; number < CAM_DEVICES_MAX; number++) {
		if (cam_numbers[number] == NULL) {
			cam_numbers[number] = device;
			device->number = number;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&cam_registry_lock, irq);

	/* Reports that four cameras are already attached. */
	if (error != 0)
		return error;

	/* Succeeded: the device has a number. */
	return 0;
}

/* Gives back a device's node number. */
static void
cam_number_release(
	struct drv_cam_device *device)
{
	unsigned long irq;

	/* Empties the device's entry. */
	irq = spin_lock_irqsave(&cam_registry_lock);

	if (cam_numbers[device->number] == device)
		cam_numbers[device->number] = NULL;

	spin_unlock_irqrestore(&cam_registry_lock, irq);
}

/* Frees the device and its buffers after the last reference lets it go. */
static void
cam_device_release(
	void *argument)
{
	struct drv_cam_device *device;
	int last;

	/* Drops one reference. */
	device = argument;
	last = refcount_put(&device->references);
	if (last == 0)
		return;

	/* No file or mapping is left, so the buffers can go with the device. */
	cam_buffers_free(device);
	kern_free(device);
}

/* Tells the system's events that a camera node came or went. */
static void
cam_post(
	const struct drv_cam_device *device,
	uint32_t action)
{
	char subject[KERN_SYSTEM_EVENT_SUBJECT_MAX];
	char detail[KERN_SYSTEM_EVENT_DETAIL_MAX];

	/* A kernel without the system's events posts nothing. */
	if (kern_system_event_post == NULL)
		return;

	/* The node, the driver and the product, then the event. */
	kern_snprintf(subject, sizeof(subject), "cam%u", device->number);
	kern_snprintf(detail, sizeof(detail), "driver=%s name=%s", device->driver_name, device->desc.device_name);
	kern_system_event_post(KERN_SYSTEM_EVENT_CAMERA, action, 0, subject, detail);
}

/* Reads the monotonic clock in nanoseconds. */
static uint64_t
cam_now_ns(
	void)
{
	struct timespec now;
	int error;

	/* Reads the clock; a clock that cannot be read reports 0. */
	error = kern_clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Reports the time in nanoseconds. */
	return (uint64_t)now.tv_sec * KERN_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

/*
 * Admits one request.  Every request holds the admission until it
 * returns, so the retirement can wait for the last one.
 */
static int
cam_enter(
	struct drv_cam_device *device)
{
	unsigned long irq;
	int error;

	/* Counts the request unless the device is gone. */
	error = 0;
	irq = spin_lock_irqsave(&device->lock);

	if (device->gone != 0U)
		error = ENODEV;
	else
		device->admissions++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports a retired device. */
	if (error != 0)
		return error;

	/* Succeeded: the request is admitted. */
	return 0;
}

/* Ends one admitted request and wakes a retirement waiting for the last. */
static void
cam_leave(
	struct drv_cam_device *device)
{
	unsigned long irq;

	/* Zero admissions with GONE lets the retirement go on. */
	irq = spin_lock_irqsave(&device->lock);

	device->admissions--;
	if (device->gone != 0U && device->admissions == 0U)
		waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);
}

/* Appends a slot to a queue. */
static void
cam_queue_append(
	struct drv_cam_slot **head,
	struct drv_cam_slot **tail,
	struct drv_cam_slot *slot)
{
	/* The slot becomes the last. */
	slot->next = NULL;
	if (*tail != NULL) {
		(*tail)->next = slot;
	} else {
		*head = slot;
	}

	/* The slot is now the last of the queue. */
	*tail = slot;
}

/* Takes the first slot of a queue, or NULL. */
static struct drv_cam_slot *
cam_queue_pop(
	struct drv_cam_slot **head,
	struct drv_cam_slot **tail)
{
	struct drv_cam_slot *slot;

	/* An empty queue has no first slot. */
	slot = *head;
	if (slot == NULL)
		return NULL;

	/* The second slot becomes the first. */
	*head = slot->next;
	if (*head == NULL)
		*tail = NULL;
	slot->next = NULL;

	/* Reports the slot taken. */
	return slot;
}

/* Makes every slot unused and empties both queues; the spinlock is held. */
static void
cam_slots_reset_locked(
	struct drv_cam_device *device)
{
	unsigned index;

	/* No slot stays queued, active or done. */
	for (index = 0U; index < device->buffer_count; index++) {
		device->slots[index].state = CAM_SLOT_UNUSED;
		device->slots[index].next = NULL;
	}

	/* Empties both queues. */
	device->free_head = NULL;
	device->free_tail = NULL;
	device->done_head = NULL;
	device->done_tail = NULL;
}

/*
 * Stops a running or failed stream through the source and returns every
 * buffer to unused.  The mutex is held.  The source's stop is called only
 * from here.
 */
static void
cam_stream_stop(
	struct drv_cam_device *device)
{
	unsigned long irq;
	unsigned running;

	/* STOPPING hands out no slot while the source stops. */
	running = 0U;
	irq = spin_lock_irqsave(&device->lock);

	if (device->state == CAM_STATE_STREAMING || device->state == CAM_STATE_FAULT) {
		device->state = CAM_STATE_STOPPING;
		running = 1U;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* A stream that does not run has nothing to stop. */
	if (running == 0U)
		return;

	/* The source stops its transfers and discards its active slots. */
	device->desc.source->stop(device->desc.source_argument);

	/* Every buffer is unused again, and the waiters see the stream stopped. */
	irq = spin_lock_irqsave(&device->lock);

	cam_slots_reset_locked(device);
	device->state = CAM_STATE_BUFFERS;
	device->fault_error = 0;
	device->have_last_sequence = 0U;
	waitq_wake_all(&device->waitq);

	spin_unlock_irqrestore(&device->lock, irq);

	/* Tells pollers the stream stopped. */
	poll_notify();
}

/* Frees every buffer; no mapping of them is left. */
static void
cam_buffers_free(
	struct drv_cam_device *device)
{
	struct drv_cam_slot *slot;
	unsigned index;

	/* Gives every run back to the physical allocator, or every import back to the GPU. */
	for (index = 0U; index < device->buffer_count; index++) {
		slot = &device->slots[index];
		if (slot->import != NULL) {
			if (slot->address != NULL)
				(void)kern_pmem_unmap_uncached(slot->address, slot->bytes);
			handle_put(slot->import);
			slot->import = NULL;
		} else if (slot->address != NULL) {
			(void)kern_pmem_free(&slot->memory);
		}

		/* Forgets the memory the slot used. */
		slot->address = NULL;
		slot->bytes = 0U;
		slot->capacity = 0U;
		slot->state = CAM_SLOT_UNUSED;
		slot->next = NULL;
	}

	/* Returns the device to having no buffers. */
	device->buffer_count = 0U;
	device->memory = CAM_MEMORY_DEVICE;
	device->free_head = NULL;
	device->free_tail = NULL;
	device->done_head = NULL;
	device->done_tail = NULL;
}

/*
 * Allocates up to count zeroed buffers of the mode's frame size and
 * reports how many it got.  The mutex is held and no buffer exists.
 */
static int
cam_buffers_allocate(
	struct drv_cam_device *device,
	unsigned count)
{
	struct drv_cam_slot *slot;
	size_t bytes;
	unsigned index;
	int error;

	/* Each buffer is whole pages so it can be mapped on its own. */
	bytes = ((size_t)device->mode.image_bytes + KERN_PAGE_SIZE - 1U) & ~((size_t)KERN_PAGE_SIZE - 1U);
	if (bytes == 0U)
		return EINVAL;

	/* Allocates one run per buffer and stops at the first failure. */
	for (index = 0U; index < count; index++) {
		slot = &device->slots[index];
		error = kern_pmem_alloc(bytes, KERN_PAGE_SIZE, &slot->memory);
		if (error != 0)
			break;

		/* The run's direct-map address. */
		slot->address = kern_pmem_to_kernel(slot->memory.paddr);
		if (slot->address == NULL) {
			(void)kern_pmem_free(&slot->memory);
			break;
		}

		/* A new buffer reads as zeros, never as an older owner's data. */
		kern_memset(slot->address, 0, bytes);
		slot->bytes = bytes;
		slot->capacity = device->mode.image_bytes;
		slot->state = CAM_SLOT_UNUSED;
		slot->mappings = 0U;
		slot->next = NULL;
		device->buffer_count = index + 1U;
	}

	/* Reports that not even one buffer could be had. */
	if (device->buffer_count == 0U)
		return ENOMEM;

	/* Succeeded: buffer_count buffers exist. */
	device->memory = CAM_MEMORY_DEVICE;
	return 0;
}

/*
 * Makes count imported slots that have no memory yet; each takes its GPU
 * allocation on its first queue.  The mutex is held and no buffer exists.
 */
static void
cam_buffers_prepare_import(
	struct drv_cam_device *device,
	unsigned count)
{
	struct drv_cam_slot *slot;
	unsigned index;

	/* Every slot waits for its first queue to name its memory. */
	for (index = 0U; index < count; index++) {
		slot = &device->slots[index];
		slot->import = NULL;
		slot->address = NULL;
		slot->bytes = 0U;
		slot->capacity = device->mode.image_bytes;
		slot->state = CAM_SLOT_UNUSED;
		slot->mappings = 0U;
		slot->next = NULL;
	}

	/* Publishes the prepared slots as imported memory. */
	device->buffer_count = count;
	device->memory = CAM_MEMORY_IMPORT;
}

/* Tells whether this kernel and this source can fill imported GPU memory. */
static int
cam_import_available(
	const struct drv_cam_device *device)
{
	/* The source writes with the CPU, which the uncached view serves. */
	if ((device->desc.flags & DRV_CAM_SOURCE_CPU_WRITES) == 0U)
		return 0;

	/* The GPU core and the uncached views are in this kernel. */
	if (drv_gpu_shared_backing == NULL ||
	    kern_pmem_map_uncached == NULL ||
	    kern_pmem_unmap_uncached == NULL)
		return 0;

	/* Succeeded: imported memory can be filled. */
	return 1;
}

/*
 * Binds an imported slot to the GPU allocation behind a descriptor and
 * maps its pages uncached for the source to write.
 *
 * The allocation must be one contiguous run of RAM that the GPU maps
 * uncached and that holds a whole frame; anything else is EXDEV.  The slot
 * keeps the descriptor's handle until the buffers are freed, so the pages
 * stay put while the source writes them.  The mutex is held.
 */
static int
cam_slot_import(
	struct drv_cam_device *device,
	struct drv_cam_slot *slot,
	int32_t descriptor)
{
	struct drv_gpu_scanout_backing backing;
	struct kernel_handle *handle;
	struct kern_pmem run;
	uint64_t identity;
	uint64_t extent;
	size_t bytes;
	void *view;
	int error;

	/* The first queue of an imported slot names its memory. */
	if (descriptor < 0)
		return EINVAL;
	handle = handle_fd_get(descriptor, KERNEL_HANDLE_DRIVER);
	if (handle == NULL)
		return EINVAL;

	/* Asks the GPU core for the pages; the exporter's identity is not needed by a CPU writer. */
	kern_memset(&backing, 0, sizeof(backing));
	error = drv_gpu_shared_backing(handle, &backing, &identity);
	if (error == ENOTSUP)
		error = EXDEV;
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* The whole frame in one run of ordinary RAM that the GPU reads uncached. */
	bytes = ((size_t)device->mode.image_bytes + KERN_PAGE_SIZE - 1U) & ~((size_t)KERN_PAGE_SIZE - 1U);
	extent = backing.page_count * backing.page_bytes;
	if ((backing.flags & DRV_GPU_BACKING_CONTIGUOUS) == 0U ||
	    (backing.flags & DRV_GPU_BACKING_DEVICE) != 0U ||
	    (backing.flags & DRV_GPU_BACKING_UNCACHED) == 0U ||
	    (backing.pages[0] & (KERN_PAGE_SIZE - 1U)) != 0U ||
	    backing.bytes < device->mode.image_bytes ||
	    extent < bytes) {
		handle_put(handle);
		return EXDEV;
	}

	/* The source's view of the pages, uncached like the GPU's. */
	run.paddr = (kern_physaddr_t)backing.pages[0];
	run.size = bytes;
	error = kern_pmem_map_uncached(&run, &view);
	if (error != 0) {
		handle_put(handle);
		return error;
	}

	/* Succeeded: the slot holds the allocation until the buffers are freed. */
	slot->import = handle;
	slot->memory = run;
	slot->address = view;
	slot->bytes = bytes;
	return 0;
}

/* Checks a record's version and size against the one the request takes. */
static int
cam_record_check(
	uint32_t version,
	uint32_t size,
	size_t expected)
{
	/* Refuses another version of the ABI. */
	if (version != CAM_ABI_VERSION)
		return EINVAL;

	/* Refuses a record of another layout. */
	if (size != expected)
		return EINVAL;

	/* Succeeded: the record is of this ABI. */
	return 0;
}

/* Answers CAM_GET_INFO. */
static int
cam_get_info(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_format_desc format;
	struct cam_info info;
	unsigned index;
	int import_available;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &info, sizeof(info));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(info.version, info.size, sizeof(info));
	if (error != 0)
		return error;

	/* Describes the camera. */
	kern_memset(&info, 0, sizeof(info));
	info.version = CAM_ABI_VERSION;
	info.size = sizeof(info);
	kern_snprintf(info.driver, sizeof(info.driver), "%s", device->driver_name);
	kern_snprintf(info.name, sizeof(info.name), "%s", device->desc.device_name);
	kern_snprintf(info.location, sizeof(info.location), "%s", device->desc.location);
	info.capabilities = device->desc.capabilities;

	/* Advertises GPU import when this kernel and this source can fill imported memory. */
	import_available = cam_import_available(device);
	if (import_available)
		info.capabilities |= CAM_CAP_IMPORT_GPU;

	/* Describes the buffer limit and the clock. */
	info.max_buffers = device->desc.max_buffers;
	info.clock_hz = device->desc.clock_hz;

	/* Counts the formats the source lists, up to the first it does not have. */
	index = 0U;
	for (;;) {
		kern_memset(&format, 0, sizeof(format));
		error = device->desc.source->enum_format(device->desc.source_argument, index, &format);
		if (error != 0)
			break;
		index++;
	}

	/* Records how many formats were found. */
	info.format_count = index;

	/* The largest payload the transport takes, when the source has a limit. */
	if (device->desc.source->host_payload_max != NULL)
		info.host_payload_max = device->desc.source->host_payload_max(device->desc.source_argument);

	/* Hands the record back. */
	error = copyout(&info, argument, sizeof(info));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the description. */
	return 0;
}

/* Answers CAM_ENUM_FORMAT. */
static int
cam_enum_format(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_format_desc desc;
	unsigned index;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &desc, sizeof(desc));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(desc.version, desc.size, sizeof(desc));
	if (error != 0)
		return error;

	/* Asks the source for the format at the index. */
	index = desc.index;
	kern_memset(&desc, 0, sizeof(desc));
	error = device->desc.source->enum_format(device->desc.source_argument, index, &desc);
	if (error != 0)
		return error;

	/* The framework's own fields of the record. */
	desc.version = CAM_ABI_VERSION;
	desc.size = sizeof(desc);
	desc.index = index;

	/* Hands the record back. */
	error = copyout(&desc, argument, sizeof(desc));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the format. */
	return 0;
}

/* Answers CAM_ENUM_MODE. */
static int
cam_enum_mode(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_mode_desc desc;
	uint32_t format;
	unsigned index;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &desc, sizeof(desc));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(desc.version, desc.size, sizeof(desc));
	if (error != 0)
		return error;

	/* Asks the source for the mode of the format at the index. */
	format = desc.format;
	index = desc.index;
	kern_memset(&desc, 0, sizeof(desc));
	error = device->desc.source->enum_mode(device->desc.source_argument, format, index, &desc);
	if (error != 0)
		return error;

	/* The framework's own fields of the record. */
	desc.version = CAM_ABI_VERSION;
	desc.size = sizeof(desc);
	desc.format = format;
	desc.index = index;

	/* Hands the record back. */
	error = copyout(&desc, argument, sizeof(desc));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the mode. */
	return 0;
}

/* Answers CAM_ENUM_INTERVAL. */
static int
cam_enum_interval(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_interval_desc desc;
	uint32_t interval;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &desc, sizeof(desc));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(desc.version, desc.size, sizeof(desc));
	if (error != 0)
		return error;

	/* A source without the operation has no intervals beyond the inline ones. */
	if (device->desc.source->enum_interval == NULL)
		return ENOENT;

	/* Asks the source for the interval. */
	interval = 0U;
	error = device->desc.source->enum_interval(device->desc.source_argument,
	    desc.format,
	    desc.mode_index,
	    desc.index,
	    &interval);
	if (error != 0)
		return error;

	/* Hands the record back. */
	desc.interval_100ns = interval;
	desc.reserved[0] = 0U;
	desc.reserved[1] = 0U;
	error = copyout(&desc, argument, sizeof(desc));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the interval. */
	return 0;
}

/* Copies a source's mode answer into a user record. */
static void
cam_format_from_result(
	struct cam_format *format,
	const struct drv_cam_mode_result *result)
{
	/* Every answered field, and the framework's header. */
	kern_memset(format, 0, sizeof(*format));
	format->version = CAM_ABI_VERSION;
	format->size = sizeof(*format);
	format->format = result->format;
	format->width = result->width;
	format->height = result->height;
	format->interval_100ns = result->interval_100ns;
	format->stride_bytes = result->stride_bytes;
	format->image_bytes = result->image_bytes;
	format->payload_bytes = result->payload_bytes;
	format->bayer_order = result->bayer_order;
	format->sample_shift = result->sample_shift;
	format->mode_index = result->mode_index;
}

/* Answers CAM_GET_FORMAT. */
static int
cam_get_format(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct drv_cam_mode_result mode;
	struct cam_format format;
	unsigned set;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &format, sizeof(format));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(format.version, format.size, sizeof(format));
	if (error != 0)
		return error;

	/* Samples the format set last. */
	mutex_lock(&device->mutex);
	set = device->format_set;
	mode = device->mode;
	mutex_unlock(&device->mutex);

	/* Reports that no format has been set. */
	if (set == 0U)
		return EINVAL;

	/* Hands the record back. */
	cam_format_from_result(&format, &mode);
	error = copyout(&format, argument, sizeof(format));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the format. */
	return 0;
}

/*
 * Answers CAM_SET_FORMAT and CAM_TRY_FORMAT.
 *
 * A setting takes ownership and is allowed only with no buffers; a try
 * does not take ownership and is refused while another open owns buffers
 * or a stream, so the device is not asked anything under a running
 * stream.  ENOSPC still hands back the answer, which tells the caller
 * what payload the mode needs.
 */
static int
cam_set_format(
	struct drv_cam_device *device,
	struct file *file,
	uintptr_t argument,
	int trying)
{
	struct drv_cam_mode_request request;
	struct drv_cam_mode_result result;
	struct cam_format format;
	int copy_error;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &format, sizeof(format));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(format.version, format.size, sizeof(format));
	if (error != 0)
		return error;

	/* The request the source answers. */
	kern_memset(&request, 0, sizeof(request));
	request.format = format.format;
	request.width = format.width;
	request.height = format.height;
	request.interval_100ns = format.interval_100ns;
	kern_memset(&result, 0, sizeof(result));

	/* Holds the stream still while the source is asked. */
	mutex_lock(&device->mutex);

	/* A request that came in before the retirement leaves without asking the source. */
	if (device->gone != 0U) {
		mutex_unlock(&device->mutex);
		return ENODEV;
	}

	/* Decides whether this open may ask at all. */
	if (trying != 0) {
		if (device->owner != NULL &&
		    device->owner != file &&
		    device->state != CAM_STATE_IDLE)
			error = EBUSY;
		else if (device->state == CAM_STATE_STREAMING ||
		    device->state == CAM_STATE_FAULT ||
		    device->state == CAM_STATE_STOPPING)
			error = EBUSY;
	} else {
		if (device->owner != NULL && device->owner != file)
			error = EBUSY;
		else if (device->state != CAM_STATE_IDLE)
			error = EBUSY;
	}

	/* Asks the source; a setting also commits the mode on the device. */
	if (error == 0) {
		if (trying != 0) {
			error = device->desc.source->try_mode(device->desc.source_argument, &request, &result);
		} else {
			error = device->desc.source->set_mode(device->desc.source_argument, &request, &result);
		}
	}

	/* A setting that succeeded is the stream's format and makes this open the owner. */
	if (error == 0 && trying == 0) {
		device->mode = result;
		device->format_set = 1U;
		device->owner = file;
	}

	mutex_unlock(&device->mutex);

	/* Reports a refusal that carries no answer. */
	if (error != 0 && error != ENOSPC)
		return error;

	/* Hands the answer back, also with ENOSPC. */
	cam_format_from_result(&format, &result);
	copy_error = copyout(&format, argument, sizeof(format));
	if (copy_error != 0)
		return copy_error;

	/* Reports a mode the host cannot carry. */
	if (error != 0)
		return error;

	/* Succeeded: the caller has the answer. */
	return 0;
}

/*
 * Answers CAM_REQUEST_BUFFERS.
 *
 * Zero frees the buffers and gives up ownership.  A mapped buffer keeps
 * the buffers, and a running stream keeps them too.
 */
static int
cam_request_buffers(
	struct drv_cam_device *device,
	struct file *file,
	uintptr_t argument)
{
	struct cam_request_buffers request;
	unsigned count;
	unsigned bytes;
	int import_available;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(request.version, request.size, sizeof(request));
	if (error != 0)
		return error;

	/* Holds the stream still while the buffers change. */
	error = 0;
	mutex_lock(&device->mutex);

	/* Decides whether the buffers may change now. */
	if (device->gone != 0U) {
		error = ENODEV;
	} else if (device->owner != NULL && device->owner != file) {
		error = EBUSY;
	} else if (device->state != CAM_STATE_IDLE && device->state != CAM_STATE_BUFFERS) {
		error = EBUSY;
	} else if (device->mapped != 0U) {
		error = EBUSY;
	}

	/* Frees what there is; zero also gives up ownership. */
	if (error == 0) {
		cam_buffers_free(device);
		device->state = CAM_STATE_IDLE;
		if (request.count == 0U) {
			device->owner = NULL;
			device->format_set = 0U;
		}
	}

	/* Refuses a buffer request before a format. */
	if (error == 0 &&
	    request.count != 0U &&
	    device->format_set == 0U)
		error = EINVAL;

	/* Refuses memory the framework does not provide. */
	if (error == 0 && request.count != 0U) {
		if (request.memory == CAM_MEMORY_IMPORT) {
			import_available = cam_import_available(device);
			if (!import_available)
				error = EOPNOTSUPP;
		} else if (request.memory != CAM_MEMORY_DEVICE) {
			error = EOPNOTSUPP;
		} else if ((device->desc.flags & DRV_CAM_SOURCE_CPU_WRITES) == 0U) {
			error = ENOTSUP;
		}
	}

	/* Allocates as many as the camera allows, or prepares as many imports. */
	if (error == 0 && request.count != 0U) {
		count = request.count;
		if (count > device->desc.max_buffers)
			count = device->desc.max_buffers;
		if (request.memory == CAM_MEMORY_IMPORT)
			cam_buffers_prepare_import(device, count);
		else
			error = cam_buffers_allocate(device, count);
		if (error == 0) {
			device->state = CAM_STATE_BUFFERS;
			device->owner = file;
		}
	}

	/* The answer: how many and how large. */
	count = device->buffer_count;
	bytes = 0U;
	if (count != 0U)
		bytes = (unsigned)device->slots[0].capacity;

	mutex_unlock(&device->mutex);

	/* Reports a refusal or a failed allocation. */
	if (error != 0)
		return error;

	/* Hands the record back. */
	request.count = count;
	request.bytes = bytes;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the caller owns count buffers. */
	return 0;
}

/* Reports the flags of a slot; the spinlock is held. */
static uint32_t
cam_slot_flags_locked(
	const struct drv_cam_slot *slot)
{
	uint32_t flags;

	/* The state of the slot. */
	flags = 0U;
	if (slot->state == CAM_SLOT_FREE)
		flags |= CAM_BUFFER_QUEUED;
	else if (slot->state == CAM_SLOT_ACTIVE)
		flags |= CAM_BUFFER_ACTIVE;
	else if (slot->state == CAM_SLOT_DONE)
		flags |= CAM_BUFFER_DONE;

	/* A slot that some address space maps. */
	if (slot->mappings != 0U)
		flags |= CAM_BUFFER_MAPPED;

	/* A slot bound to imported GPU memory. */
	if (slot->import != NULL)
		flags |= CAM_BUFFER_IMPORTED;

	/* Reports the flags. */
	return flags;
}

/* Answers CAM_QUERY_BUFFER. */
static int
cam_query_buffer(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct drv_cam_slot *slot;
	struct cam_buffer buffer;
	unsigned long irq;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(buffer.version, buffer.size, sizeof(buffer));
	if (error != 0)
		return error;

	/* Describes the buffer at the index. */
	error = 0;
	mutex_lock(&device->mutex);

	if (buffer.index >= device->buffer_count) {
		error = EINVAL;
	} else {
		slot = &device->slots[buffer.index];
		buffer.memory = device->memory;
		buffer.bytes = (uint32_t)slot->capacity;
		buffer.mmap_offset = 0U;
		if (device->memory == CAM_MEMORY_DEVICE)
			buffer.mmap_offset = (uint64_t)buffer.index * slot->bytes;
		buffer.fd = -1;
		irq = spin_lock_irqsave(&device->lock);
		buffer.flags = cam_slot_flags_locked(slot);
		buffer.metadata = slot->metadata;
		spin_unlock_irqrestore(&device->lock, irq);
	}

	mutex_unlock(&device->mutex);

	/* Reports an index beyond the buffers. */
	if (error != 0)
		return error;

	/* Hands the record back. */
	buffer.reserved[0] = 0U;
	buffer.reserved[1] = 0U;
	error = copyout(&buffer, argument, sizeof(buffer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the buffer's place. */
	return 0;
}

/* Answers CAM_QUEUE_BUFFER: an unused buffer joins the free queue. */
static int
cam_queue_buffer(
	struct drv_cam_device *device,
	struct file *file,
	uintptr_t argument)
{
	struct drv_cam_slot *slot;
	struct cam_buffer buffer;
	unsigned long irq;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(buffer.version, buffer.size, sizeof(buffer));
	if (error != 0)
		return error;

	/* Only the owner queues, and only a buffer that exists. */
	mutex_lock(&device->mutex);

	if (device->gone != 0U) {
		error = ENODEV;
	} else if (device->owner != file) {
		error = EBUSY;
	} else if (buffer.index >= device->buffer_count) {
		error = EINVAL;
	}

	/*
	 * An imported slot takes its memory on its first queue and keeps it;
	 * a later queue names no memory.  The slot is UNUSED then, because
	 * only a queue moves it out of UNUSED.
	 */
	if (error == 0 && device->memory == CAM_MEMORY_IMPORT) {
		slot = &device->slots[buffer.index];
		if (buffer.memory != CAM_MEMORY_IMPORT)
			error = EINVAL;
		else if (slot->import == NULL)
			error = cam_slot_import(device, slot, buffer.fd);
		else if (buffer.fd != -1)
			error = EINVAL;
	}

	/* Queues the buffer unless the stream or the owner still has it. */
	if (error == 0) {
		slot = &device->slots[buffer.index];
		irq = spin_lock_irqsave(&device->lock);

		if (slot->state != CAM_SLOT_UNUSED) {
			error = EBUSY;
		} else {
			slot->state = CAM_SLOT_FREE;
			cam_queue_append(&device->free_head, &device->free_tail, slot);
		}

		spin_unlock_irqrestore(&device->lock, irq);
	}

	mutex_unlock(&device->mutex);

	/* Reports a refused queue. */
	if (error != 0)
		return error;

	/* Succeeded: the buffer waits for a frame. */
	return 0;
}

/*
 * Answers CAM_DEQUEUE_BUFFER: hands back the oldest finished frame,
 * waiting for one unless the file does not block.
 *
 * The wait holds no mutex.  It ends with the frame, or with EINVAL when
 * the stream stops, EIO on a fault, ENODEV on the retirement, ETIMEDOUT
 * when the caller's limit passes and EINTR on a signal.
 */
static int
cam_dequeue_buffer(
	struct drv_cam_device *device,
	struct file *file,
	uintptr_t argument)
{
	struct drv_cam_slot *slot;
	struct cam_buffer buffer;
	unsigned long irq;
	uint64_t deadline;
	uint64_t observed;
	int nonblocking;
	int status;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &buffer, sizeof(buffer));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(buffer.version, buffer.size, sizeof(buffer));
	if (error != 0)
		return error;

	/* The deadline of the wait, and whether to wait at all. */
	deadline = 0U;
	if (buffer.timeout_ms != 0U)
		deadline = sched_ticks() + kern_ms_to_ticks(buffer.timeout_ms);
	status = file_status_flags_get(file);
	nonblocking = 0;
	if ((status & O_NONBLOCK) != 0)
		nonblocking = 1;

	/* Waits for a finished frame or the end of the stream. */
	slot = NULL;
	irq = spin_lock_irqsave(&device->lock);

	for (;;) {
		/* Decides whether the wait is over and how. */
		if (device->gone != 0U) {
			error = ENODEV;
			break;
		} else if (device->owner != file) {
			error = EBUSY;
			break;
		} else if (device->done_head != NULL) {
			slot = cam_queue_pop(&device->done_head, &device->done_tail);
			break;
		} else if (device->state == CAM_STATE_FAULT) {
			error = EIO;
			break;
		} else if (device->state != CAM_STATE_STREAMING && device->state != CAM_STATE_STOPPING) {
			error = EINVAL;
			break;
		} else if (nonblocking != 0) {
			error = EAGAIN;
			break;
		}

		/* Sleeps until a frame, a stop, a fault or the retirement wakes it. */
		observed = waitq_sequence(&device->waitq);
		error = waitq_sleep(&device->waitq, &device->lock, observed, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR || error == ETIMEDOUT)
			break;
		error = 0;
	}

	/* The frame leaves the stream; its metadata goes to the caller. */
	if (slot != NULL) {
		slot->state = CAM_SLOT_UNUSED;
		buffer.metadata = slot->metadata;
		buffer.index = slot->index;
		buffer.memory = device->memory;
		buffer.bytes = (uint32_t)slot->capacity;
		buffer.mmap_offset = 0U;
		if (device->memory == CAM_MEMORY_DEVICE)
			buffer.mmap_offset = (uint64_t)slot->index * slot->bytes;
		buffer.flags = cam_slot_flags_locked(slot);
		buffer.fd = -1;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports a wait that ended without a frame. */
	if (slot == NULL)
		return error;

	/* Hands the record back. */
	buffer.reserved[0] = 0U;
	buffer.reserved[1] = 0U;
	error = copyout(&buffer, argument, sizeof(buffer));
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the frame until it queues the buffer again. */
	return 0;
}

/* Answers CAM_STREAM_ON. */
static int
cam_stream_on(
	struct drv_cam_device *device,
	struct file *file)
{
	struct drv_cam_slot *slot;
	unsigned long irq;
	unsigned index;
	int error;

	/* Holds the stream still while the source starts. */
	error = 0;
	mutex_lock(&device->mutex);

	/* Decides whether this open may start the stream now. */
	if (device->gone != 0U) {
		error = ENODEV;
	} else if (device->owner != file) {
		error = EBUSY;
	} else if (device->state == CAM_STATE_STREAMING) {
		error = EBUSY;
	} else if (device->state != CAM_STATE_BUFFERS) {
		error = EINVAL;
	}

	/* A stream needs a queued buffer to begin with. */
	if (error == 0) {
		irq = spin_lock_irqsave(&device->lock);

		if (device->free_head == NULL) {
			error = EINVAL;
		} else {
			device->state = CAM_STATE_STREAMING;
			device->have_last_sequence = 0U;
			device->fault_error = 0;
		}

		spin_unlock_irqrestore(&device->lock, irq);
	}

	/* The source starts its transfers; slots can be taken from now on. */
	if (error == 0) {
		error = device->desc.source->start(device->desc.source_argument);
		if (error != 0) {
			/* A failed start leaves no slot active; anything left over is queued again. */
			irq = spin_lock_irqsave(&device->lock);

			for (index = 0U; index < device->buffer_count; index++) {
				slot = &device->slots[index];
				if (slot->state == CAM_SLOT_ACTIVE) {
					slot->state = CAM_SLOT_FREE;
					cam_queue_append(&device->free_head, &device->free_tail, slot);
				}
			}

			/* The stream is not running, so the device is back to having buffers. */
			device->state = CAM_STATE_BUFFERS;

			spin_unlock_irqrestore(&device->lock, irq);
		}
	}

	mutex_unlock(&device->mutex);

	/* Reports a refused or failed start. */
	if (error != 0)
		return error;

	/* Succeeded: frames are coming. */
	return 0;
}

/* Answers CAM_STREAM_OFF, also out of FAULT. */
static int
cam_stream_off(
	struct drv_cam_device *device,
	struct file *file)
{
	int error;

	/* Only the owner stops the stream. */
	error = 0;
	mutex_lock(&device->mutex);

	if (device->gone != 0U) {
		error = ENODEV;
	} else if (device->owner != file) {
		error = EBUSY;
	} else {
		cam_stream_stop(device);
	}

	mutex_unlock(&device->mutex);

	/* Reports a refused stop. */
	if (error != 0)
		return error;

	/* Succeeded: every buffer is unused again. */
	return 0;
}

/* Answers CAM_GET_CONTROL_INFO. */
static int
cam_control_info(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_control_info info;
	uint32_t id;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &info, sizeof(info));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(info.version, info.size, sizeof(info));
	if (error != 0)
		return error;

	/* A source without controls reports each one unsupported. */
	id = info.id;
	kern_memset(&info, 0, sizeof(info));
	info.flags = CAM_CONTROL_UNSUPPORTED;
	if (device->desc.source->control_info != NULL) {
		mutex_lock(&device->mutex);
		error = device->desc.source->control_info(device->desc.source_argument, id, &info);
		mutex_unlock(&device->mutex);
		if (error != 0)
			return error;
	}

	/* Hands the record back. */
	info.version = CAM_ABI_VERSION;
	info.size = sizeof(info);
	info.id = id;
	error = copyout(&info, argument, sizeof(info));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the control's range. */
	return 0;
}

/* Answers CAM_GET_CONTROL. */
static int
cam_control_get(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_control control;
	int64_t value;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &control, sizeof(control));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(control.version, control.size, sizeof(control));
	if (error != 0)
		return error;

	/* A source without controls has no value to read. */
	if (device->desc.source->get_control == NULL)
		return EOPNOTSUPP;

	/* Asks the source for the value. */
	value = 0;
	mutex_lock(&device->mutex);
	error = device->desc.source->get_control(device->desc.source_argument, control.id, &value);
	mutex_unlock(&device->mutex);
	if (error != 0)
		return error;

	/* Hands the record back. */
	control.value = value;
	control.reserved = 0U;
	error = copyout(&control, argument, sizeof(control));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the value. */
	return 0;
}

/* Answers CAM_SET_CONTROL; only the owner, or anyone while there is none. */
static int
cam_control_set(
	struct drv_cam_device *device,
	struct file *file,
	uintptr_t argument)
{
	struct cam_control control;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &control, sizeof(control));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(control.version, control.size, sizeof(control));
	if (error != 0)
		return error;

	/* A source without controls has nothing to set. */
	if (device->desc.source->set_control == NULL)
		return EOPNOTSUPP;

	/* Sets the value unless another open owns the camera. */
	mutex_lock(&device->mutex);

	if (device->gone != 0U)
		error = ENODEV;
	else if (device->owner != NULL && device->owner != file)
		error = EBUSY;
	else
		error = device->desc.source->set_control(device->desc.source_argument, control.id, control.value);

	mutex_unlock(&device->mutex);

	/* Reports a refused or failed setting. */
	if (error != 0)
		return error;

	/* Succeeded: the control has the value. */
	return 0;
}

/* Answers CAM_GET_COUNTERS: the source's counts and the framework's. */
static int
cam_get_counters(
	struct drv_cam_device *device,
	uintptr_t argument)
{
	struct cam_counters counters;
	unsigned long irq;
	uint64_t now;
	uint64_t last;
	int error;

	/* Reads the caller's record. */
	error = copyin(argument, &counters, sizeof(counters));
	if (error != 0)
		return error;

	/* Refuses a record of another version or layout. */
	error = cam_record_check(counters.version, counters.size, sizeof(counters));
	if (error != 0)
		return error;

	/* The source's transport counts. */
	kern_memset(&counters, 0, sizeof(counters));
	if (device->desc.source->read_counters != NULL) {
		mutex_lock(&device->mutex);
		(void)device->desc.source->read_counters(device->desc.source_argument, &counters);
		mutex_unlock(&device->mutex);
	}

	/* The framework's frame counts. */
	irq = spin_lock_irqsave(&device->lock);

	counters.frames = device->frames;
	counters.dropped_no_buffer = device->dropped_no_buffer;
	counters.dropped_overflow = device->dropped_overflow;
	counters.dropped_error = device->dropped_error;
	counters.dropped_transport = device->dropped_transport;
	counters.sequence_gaps = device->sequence_gaps;
	last = device->last_frame_ns;

	spin_unlock_irqrestore(&device->lock, irq);

	/* How long ago the last frame finished, when one did. */
	now = cam_now_ns();
	if (last != 0U && now >= last)
		counters.last_frame_age_ms = (uint32_t)((now - last) / 1000000U);

	/* Hands the record back. */
	counters.version = CAM_ABI_VERSION;
	counters.size = sizeof(counters);
	error = copyout(&counters, argument, sizeof(counters));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the counts. */
	return 0;
}

/* Opens a camera node; any number of opens may query it. */
static int
cam_open(
	struct file *file)
{
	struct drv_cam_device *device;
	int error;

	/* Refuses a retired camera. */
	device = file->f_data;
	error = cam_enter(device);
	if (error != 0)
		return error;

	/* Leaves the request that cam_enter admitted. */
	cam_leave(device);

	/* Succeeded: the file may query the camera. */
	return 0;
}

/*
 * Closes a camera node.
 *
 * The close comes only after the last mapping is gone, because every
 * mapping retains the file.  The owner's close stops its stream and frees
 * its buffers.  After the retirement the stream was stopped already, so
 * only the buffers are freed.
 */
static int
cam_close(
	struct file *file)
{
	struct drv_cam_device *device;
	unsigned long irq;
	unsigned gone;

	/* Counts the close as a request unless the camera is gone. */
	device = file->f_data;
	irq = spin_lock_irqsave(&device->lock);

	gone = device->gone;
	if (gone == 0U)
		device->admissions++;

	spin_unlock_irqrestore(&device->lock, irq);

	/* Gives up what this open owned. */
	mutex_lock(&device->mutex);

	if (device->owner == file) {
		if (gone == 0U)
			cam_stream_stop(device);
		if (device->mapped == 0U)
			cam_buffers_free(device);
		device->state = CAM_STATE_IDLE;
		device->owner = NULL;
		device->format_set = 0U;
	}

	mutex_unlock(&device->mutex);

	/* Ends the counted request. */
	if (gone == 0U)
		cam_leave(device);

	/* Succeeded: the file no longer uses the camera. */
	return 0;
}

/* Refuses read(2); frames come through the buffers. */
static ssize_t
cam_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	UNUSED_PARAMETER(file);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);

	/* A camera is read through its mapped buffers only. */
	return -EOPNOTSUPP;
}

/* Refuses write(2). */
static ssize_t
cam_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	UNUSED_PARAMETER(file);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);

	/* A camera takes no data. */
	return -EOPNOTSUPP;
}

/* Admits one request of a camera node and dispatches it. */
static int
cam_ioctl(
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	struct drv_cam_device *device;
	int error;

	/* Refuses a retired camera. */
	device = file->f_data;
	error = cam_enter(device);
	if (error != 0)
		return error;

	/* Answers the request while it is admitted. */
	error = cam_ioctl_admitted(device, file, command, argument);
	cam_leave(device);

	/* Reports a refused or failed request. */
	if (error != 0)
		return error;

	/* Succeeded: the request is answered. */
	return 0;
}

/* Dispatches one admitted request. */
static int
cam_ioctl_admitted(
	struct drv_cam_device *device,
	struct file *file,
	unsigned long command,
	uintptr_t argument)
{
	int error;

	/* Dispatches on the request. */
	switch (command) {
	case CAM_GET_INFO:
		error = cam_get_info(device, argument);
		break;

	case CAM_ENUM_FORMAT:
		error = cam_enum_format(device, argument);
		break;

	case CAM_ENUM_MODE:
		error = cam_enum_mode(device, argument);
		break;

	case CAM_ENUM_INTERVAL:
		error = cam_enum_interval(device, argument);
		break;

	case CAM_GET_FORMAT:
		error = cam_get_format(device, argument);
		break;

	case CAM_SET_FORMAT:
		error = cam_set_format(device, file, argument, 0);
		break;

	case CAM_TRY_FORMAT:
		error = cam_set_format(device, file, argument, 1);
		break;

	case CAM_REQUEST_BUFFERS:
		error = cam_request_buffers(device, file, argument);
		break;

	case CAM_QUERY_BUFFER:
		error = cam_query_buffer(device, argument);
		break;

	case CAM_QUEUE_BUFFER:
		error = cam_queue_buffer(device, file, argument);
		break;

	case CAM_DEQUEUE_BUFFER:
		error = cam_dequeue_buffer(device, file, argument);
		break;

	case CAM_STREAM_ON:
		error = cam_stream_on(device, file);
		break;

	case CAM_STREAM_OFF:
		error = cam_stream_off(device, file);
		break;

	case CAM_GET_CONTROL_INFO:
		error = cam_control_info(device, argument);
		break;

	case CAM_GET_CONTROL:
		error = cam_control_get(device, argument);
		break;

	case CAM_SET_CONTROL:
		error = cam_control_set(device, file, argument);
		break;

	case CAM_GET_COUNTERS:
		error = cam_get_counters(device, argument);
		break;

	default:
		/* CAM_EXPORT_BUFFER is reserved and every other request unknown. */
		error = ENOTTY;
		break;
	}

	/* Reports a refused or failed request. */
	if (error != 0)
		return error;

	/* Succeeded: the request is answered. */
	return 0;
}

/*
 * Reports the readiness of a camera node.
 *
 * Only the owner sees POLLIN, so another open never spins on frames it
 * cannot dequeue.  FAULT is POLLERR and the retirement POLLERR | POLLHUP.
 */
static int
cam_poll(
	struct file *file,
	short events,
	short *revents)
{
	struct drv_cam_device *device;
	unsigned long irq;

	/* A poll result needs storage owned by the caller. */
	if (revents == NULL)
		return EINVAL;

	/* Samples the queues and the state under one lock. */
	*revents = 0;
	device = file->f_data;
	irq = spin_lock_irqsave(&device->lock);

	if (device->gone != 0U) {
		*revents = POLLERR | POLLHUP;
	} else if (device->owner == file) {
		if (device->done_head != NULL)
			*revents |= events & (POLLIN | POLLRDNORM);
		if (device->state == CAM_STATE_FAULT)
			*revents |= POLLERR;
	}

	spin_unlock_irqrestore(&device->lock, irq);

	/* Succeeded: the readiness bits reflect the queues. */
	return 0;
}

/*
 * Maps one buffer for reading.
 *
 * The offset names the buffer (its index times the buffer's size in whole
 * pages) and the range stays within it.  Only the owner maps, and every
 * mapping retains the file, so the buffers outlive the mapping's file.
 */
static int
cam_mmap(
	struct file *file,
	off_t offset,
	size_t bytes,
	uint32_t prot,
	struct vm_device_mapping **result)
{
	struct drv_cam_device *device;
	struct drv_cam_slot *slot;
	unsigned long irq;
	uint64_t index;
	uint64_t inside;
	int error;

	/* Refuses a retired camera. */
	device = file->f_data;
	error = cam_enter(device);
	if (error != 0)
		return error;

	/* Refuses writing; a camera's buffers are written by its source only. */
	if ((prot & KERN_PROT_WRITE) != 0U) {
		cam_leave(device);
		return EACCES;
	}

	/* Refuses a range that is not whole pages. */
	if (offset < 0 || bytes == 0U ||
	    ((uint64_t)offset & (KERN_PAGE_SIZE - 1U)) != 0U ||
	    (bytes & (KERN_PAGE_SIZE - 1U)) != 0U) {
		cam_leave(device);
		return EINVAL;
	}

	/* Holds the buffers still while one is mapped. */
	error = 0;
	slot = NULL;
	mutex_lock(&device->mutex);

	if (device->owner != file) {
		error = EBUSY;
	} else if (device->buffer_count == 0U || device->memory != CAM_MEMORY_DEVICE) {
		error = EINVAL;
	} else {
		/* Finds the buffer and the place in it. */
		index = (uint64_t)offset / device->slots[0].bytes;
		inside = (uint64_t)offset % device->slots[0].bytes;
		if (index >= device->buffer_count)
			error = EINVAL;
		else if (inside + bytes > device->slots[0].bytes)
			error = EINVAL;
		else
			slot = &device->slots[index];
	}

	/* Hands the buffer's pages to the VM; the mapping retains the file. */
	if (error == 0) {
		error = vm_device_create(file,
		    slot->memory.paddr + inside,
		    slot->address + inside,
		    bytes,
		    0U,
		    KERN_PROT_READ,
		    cam_mapping_release,
		    slot,
		    result);
	}

	/* A mapped buffer keeps the buffers from being freed. */
	if (error == 0) {
		irq = spin_lock_irqsave(&device->lock);

		slot->mappings++;
		device->mapped++;

		spin_unlock_irqrestore(&device->lock, irq);
	}

	mutex_unlock(&device->mutex);
	cam_leave(device);

	/* Reports a refused or failed mapping. */
	if (error != 0)
		return error;

	/* Succeeded: the caller maps the buffer. */
	return 0;
}

/* Ends one mapping of a buffer. */
static void
cam_mapping_release(
	void *owner)
{
	struct drv_cam_device *device;
	struct drv_cam_slot *slot;
	unsigned long irq;

	/* The last mapping of the buffers lets them be freed again. */
	slot = owner;
	device = slot->device;
	irq = spin_lock_irqsave(&device->lock);

	if (slot->mappings != 0U)
		slot->mappings--;
	if (device->mapped != 0U)
		device->mapped--;

	spin_unlock_irqrestore(&device->lock, irq);
}
