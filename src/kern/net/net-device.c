/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network device registry.
 *
 * Devices live in a fixed table and move through allocated, live,
 * removing, and gone states under one spin lock.  A live device is
 * reference counted by packets, routes, and callbacks in flight; open
 * and close, polling, and ioctl callbacks are joined before a device is
 * removed, and the last reference of a destroyed device runs the
 * driver's release hook.  The HAL and protocol hooks are weak so that
 * host tests can use the registry alone.
 */

#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"

#include <zedbsd/route.h>

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define NET_DEVICE_ALLOCATED 1U
#define NET_DEVICE_LIVE      2U
#define NET_DEVICE_REMOVING  3U
#define NET_DEVICE_GONE      4U

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern int net_input_enqueue(struct net_device *, struct packet_buf *);
extern void net_worker_wakeup(void);
extern void sched_yield(void) __attribute__((weak));
extern void route_purge_device(struct net_device *) __attribute__((weak));
extern void inet_interface_purge_device(struct net_device *)
    __attribute__((weak));
extern void arp_purge_device(struct net_device *) __attribute__((weak));
extern void route_socket_notify(unsigned, uint64_t, unsigned, unsigned)
    __attribute__((weak));

static struct net_device devices[NET_DEVICE_MAX];
static uint8_t device_used[NET_DEVICE_MAX];
static struct net_device *device_head;
static unsigned live_count;
static unsigned next_ifindex = 1;
static uint64_t next_device_generation = 1U;
static unsigned removals_active;
static unsigned registry_stopping;
static atomic_uint_t device_guard;

struct device_finalizer {
	struct net_device *device;
	void (*release)(void *);
	void *driver_data;
};

static int device_reclaim_locked(struct net_device *device, struct device_finalizer *finalizer);
static void device_finalize(struct device_finalizer *finalizer);
static void device_put_locked(struct net_device *device, struct device_finalizer *finalizer);
static void device_update_running_locked(struct net_device *device);
static void device_wait_callbacks(struct net_device *device, int opening, int closing);
static void device_wait_removed(struct net_device *device);
static bool device_lock(void);
static void device_unlock(bool enabled);
static int device_name_valid(const char *name);

/*
 * Empties the device registry.
 */
void
net_device_registry_init(
	void)
{
	bool enabled;

	enabled = device_lock();
	memset(devices, 0, sizeof(devices));
	memset(device_used, 0, sizeof(device_used));
	device_head = NULL;
	live_count = 0;
	next_ifindex = 1;
	removals_active = 0;
	registry_stopping = 0;
	device_unlock(enabled);
}

/*
 * Allocates a device slot for a driver to fill in.
 *
 * The device starts with the allocation owner's reference.
 */
struct net_device *
net_device_alloc(
	void)
{
	struct net_device *device;
	bool enabled;
	unsigned index;

	device = NULL;

	/* Takes the first free slot. */
	enabled = device_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (device_used[index])
			continue;
		device_used[index] = 1;
		memset(&devices[index], 0, sizeof(devices[index]));
		devices[index].state = NET_DEVICE_ALLOCATED;
		refcount_init(&devices[index].refs, 1);
		device = &devices[index];
		break;
	}
	device_unlock(enabled);

	/* Reports the device, or none. */
	return device;
}

/*
 * Publishes a filled-in device under a unique name and interface index.
 */
int
net_device_create(
	struct net_device *device)
{
	struct net_device **tail;
	struct net_device *other;
	bool enabled;

	/* Rejects an incomplete description. */
	if (device == NULL ||
	    !device_name_valid(device->name) ||
	    device->mtu == 0 ||
	    device->hwaddr_len == 0 ||
	    device->hwaddr_len > NET_DEVICE_HWADDR_MAX ||
	    device->ops == NULL ||
	    device->ops->transmit == NULL)
		return EINVAL;

	/* Only an allocated device can be published, and not while stopping. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_ALLOCATED || registry_stopping) {
		device_unlock(enabled);
		if (registry_stopping)
			return EBUSY;
		return EINVAL;
	}

	/* The name must be unique and identifiers must remain. */
	for (other = device_head; other != NULL; other = other->next) {
		if (!strcmp(other->name, device->name)) {
			device_unlock(enabled);
			return EEXIST;
		}
	}
	if (next_ifindex == 0 || next_device_generation == 0U) {
		device_unlock(enabled);
		return ENOSPC;
	}

	/* Assigns the identifiers; the driver's running flag becomes the carrier. */
	device->ifindex = next_ifindex++;
	device->generation = next_device_generation++;
	device->carrier = (device->flags & NET_DEVICE_RUNNING) != 0;
	device->flags &= ~NET_DEVICE_RUNNING;
	device->state = NET_DEVICE_LIVE;
	device->next = NULL;

	/* Appends the device to the list. */
	tail = &device_head;
	while (*tail != NULL)
		tail = &(*tail)->next;
	*tail = device;
	live_count++;
	device_unlock(enabled);

	/* Reports the published device. */
	return 0;
}

/*
 * Removes a device whose hardware has gone away.
 *
 * The device leaves the list, its open state is closed, every protocol
 * identity is purged, and the callbacks in flight are joined before it
 * is marked gone.  A caller with interrupts disabled cannot join and
 * gets EWOULDBLOCK; a second caller waits for the first.
 */
int
net_device_gone(
	struct net_device *device)
{
	struct net_device **link;
	bool enabled;
	unsigned open_references;
	unsigned event_ifindex;
	unsigned event_flags;
	uint64_t event_generation;
	int call_close;

	open_references = 0;
	event_ifindex = 0;
	event_flags = 0;
	event_generation = 0U;
	call_close = 0;

	/* Rejects a missing device. */
	if (device == NULL)
		return EINVAL;

	enabled = device_lock();

	/*
	 * A second bus teardown is also a barrier.  Hold the object until
	 * the first caller has closed producers and purged every identity.
	 */
	if (device->state == NET_DEVICE_REMOVING) {
		if (hal_irq_disable != NULL && !enabled) {
			device_unlock(enabled);
			return EWOULDBLOCK;
		}
		refcount_get(&device->refs);
		device_unlock(enabled);
		device_wait_removed(device);
		net_device_release(device);
		return 0;
	}

	/* A device that is not live has nothing to remove. */
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		return 0;
	}

	/*
	 * false means IRQs were already disabled before device_lock().  Such
	 * a caller cannot join callbacks safely; leave the object entirely
	 * live so its bus owner can defer teardown without losing registry
	 * identity.
	 */
	if (hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return EWOULDBLOCK;
	}

	/* Unlinks the device and marks it removing. */
	for (link = &device_head; *link != NULL; link = &(*link)->next) {
		if (*link != device)
			continue;
		*link = device->next;
		if (live_count != 0)
			live_count--;
		break;
	}
	device->next = NULL;
	device->state = NET_DEVICE_REMOVING;
	if (removals_active == UINT_MAX)
		__builtin_trap();
	removals_active++;
	device->carrier = 0;
	device->flags &= ~(NET_DEVICE_UP | NET_DEVICE_RUNNING);
	event_ifindex = device->ifindex;
	event_generation = device->generation;
	event_flags = device->flags;
	device->poll_scheduled = 0;

	/* Keeps the slot alive until close and every identity purge has finished. */
	refcount_get(&device->refs);
	if (device->open_count != 0 && !device->closing) {
		open_references = device->open_count;
		device->open_count = 0;
		device->closing = 1;
		call_close = device->ops != NULL && device->ops->close != NULL;
	}
	device_unlock(enabled);

	/* Announces the removal. */
	if (route_socket_notify != NULL)
		route_socket_notify(event_ifindex, event_generation, event_flags,
		    RTM_IFINFO_REMOVAL);

	/*
	 * A driver close cannot race poll_receive() or ioctl(): once
	 * closing/GONE is published no new callback starts, and this joins
	 * every admitted callback before the driver's I/O retirement
	 * boundary.
	 */
	if (call_close)
		device_wait_callbacks(device, 0, 0);

	/*
	 * close() closes admission. A driver retaining an incomplete checked stop
	 * must pin its context/device and join that independent retirement work in
	 * its bus detach barrier before releasing any resources or driver_data.
	 */
	if (call_close)
		device->ops->close(device);
	if (open_references != 0) {
		enabled = device_lock();
		device->closing = 0;
		device_unlock(enabled);
	}

	/*
	 * If open or close was already executing when removal won the
	 * lifecycle gate, join its compensating close before detach may
	 * release bus/HCD ownership.
	 */
	device_wait_callbacks(device, 1, 1);

	/* Purges every protocol identity of the device. */
	if (route_purge_device != NULL)
		route_purge_device(device);
	if (inet_interface_purge_device != NULL)
		inet_interface_purge_device(device);
	if (arp_purge_device != NULL)
		arp_purge_device(device);

	/*
	 * Retires every open-derived reference before publishing completion.
	 * A terminal shutdown waiting on removals_active may then rely on
	 * GONE as the complete driver-close and reference-drain boundary.
	 */
	if (open_references != 0) {
		while (open_references-- != 0)
			net_device_release(device);
	}

	/* Publishes the completed removal. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_REMOVING)
		__builtin_trap();
	device->state = NET_DEVICE_GONE;
	if (removals_active == 0)
		__builtin_trap();
	removals_active--;
	device_unlock(enabled);
	net_device_release(device);

	/* Reports the removed device. */
	return 0;
}

/*
 * Removes every device and waits for concurrent removals to finish.
 *
 * The registry accepts no new devices afterwards.
 */
int
net_device_shutdown_all(
	void)
{
	struct net_device *device;
	bool enabled;
	int error;

	enabled = device_lock();

	/*
	 * Terminal shutdown joins callbacks and may yield.  Reject an
	 * interrupt-context caller before publishing the irreversible
	 * registry stop.
	 */
	if (hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return EWOULDBLOCK;
	}
	registry_stopping = 1U;
	device_unlock(enabled);

	/* Removes the devices one by one, then waits out other removals. */
	for (;;) {
		device = net_device_at_ref(0);
		if (device == NULL) {
			enabled = device_lock();
			error = removals_active != 0;
			device_unlock(enabled);
			if (!error)
				return 0;
			if (sched_yield != NULL)
				sched_yield();
			else
				__asm__ volatile("" ::: "memory");
			continue;
		}
		error = net_device_gone(device);
		net_device_release(device);
		if (error != 0)
			return error;
	}
}

/*
 * Gives up the allocation owner's reference on an allocated or gone device.
 *
 * The slot is reclaimed, and the driver's release hook run, once every
 * other reference has been dropped.
 */
void
net_device_destroy(
	struct net_device *device)
{
	struct device_finalizer finalizer;
	bool enabled;

	memset(&finalizer, 0, sizeof(finalizer));

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Only an allocated or gone device is destroyed, and only once. */
	enabled = device_lock();
	if ((device->state != NET_DEVICE_ALLOCATED &&
	     device->state != NET_DEVICE_GONE) ||
	    device->destroy_pending) {
		device_unlock(enabled);
		return;
	}

	/*
	 * Consumes the allocation owner's permanent reference.  Existing
	 * packet, route, or callback references retire through
	 * net_device_release().
	 */
	device->destroy_pending = 1;
	if (refcount_put(&device->refs))
		(void)device_reclaim_locked(device, &finalizer);
	device_unlock(enabled);
	device_finalize(&finalizer);
}

/*
 * References the live device with a name, or none.
 */
struct net_device *
net_device_find_ref(
	const char *name)
{
	struct net_device *device;
	struct net_device *candidate;
	bool enabled;

	device = NULL;

	/* Rejects a missing name. */
	if (name == NULL)
		return NULL;

	/* Searches the live list under the lock. */
	enabled = device_lock();
	for (candidate = device_head; candidate != NULL; candidate = candidate->next) {
		if (!strcmp(candidate->name, name)) {
			refcount_get(&candidate->refs);
			device = candidate;
			break;
		}
	}
	device_unlock(enabled);

	/* Reports the referenced device, or none. */
	return device;
}

/*
 * References the live device with an interface index, or none.
 */
struct net_device *
net_device_find_by_index_ref(
	unsigned ifindex)
{
	struct net_device *device;
	struct net_device *candidate;
	bool enabled;

	device = NULL;

	/* Searches the live list under the lock. */
	enabled = device_lock();
	for (candidate = device_head; candidate != NULL; candidate = candidate->next) {
		if (candidate->ifindex == ifindex) {
			refcount_get(&candidate->refs);
			device = candidate;
			break;
		}
	}
	device_unlock(enabled);

	/* Reports the referenced device, or none. */
	return device;
}

/*
 * References the live device at a position in the list, or none.
 */
struct net_device *
net_device_at_ref(
	unsigned index)
{
	struct net_device *device;
	bool enabled;

	/* Walks the live list to the position. */
	enabled = device_lock();
	device = device_head;
	while (device != NULL && index != 0) {
		device = device->next;
		index--;
	}
	if (device != NULL)
		refcount_get(&device->refs);
	device_unlock(enabled);

	/* Reports the referenced device, or none. */
	return device;
}

/*
 * Reports the number of live devices.
 */
unsigned
net_device_count(
	void)
{
	bool enabled;
	unsigned result;

	/* Samples the count under the lock. */
	enabled = device_lock();
	result = live_count;
	device_unlock(enabled);

	/* Reports the sampled count. */
	return result;
}

/*
 * Takes a reference on a device that still has one.
 */
void
net_device_ref(
	struct net_device *device)
{
	bool enabled;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* A device whose references are gone cannot be revived. */
	enabled = device_lock();
	if (refcount_load(&device->refs) != 0)
		refcount_get(&device->refs);
	device_unlock(enabled);
}

/*
 * Takes a reference on a device only while it is live.
 */
int
net_device_ref_live(
	struct net_device *device)
{
	bool enabled;
	int result;

	result = 0;

	/* Ignores a missing device. */
	if (device == NULL)
		return 0;

	/* References a live device that is not being destroyed. */
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE &&
	    !device->destroy_pending &&
	    refcount_tryget(&device->refs))
		result = 1;
	device_unlock(enabled);

	/* Reports whether the reference was taken. */
	return result;
}

/*
 * Drops a reference on a device, reclaiming a destroyed one with the last.
 */
void
net_device_release(
	struct net_device *device)
{
	struct device_finalizer finalizer;
	bool enabled;

	memset(&finalizer, 0, sizeof(finalizer));

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Drops the reference under the lock; finalizes outside it. */
	enabled = device_lock();
	device_put_locked(device, &finalizer);
	device_unlock(enabled);
	device_finalize(&finalizer);
}

/*
 * Tests whether a device is live and not being destroyed.
 */
int
net_device_is_live(
	const struct net_device *device)
{
	bool enabled;
	int live;

	/* A missing device is not live. */
	if (device == NULL)
		return 0;

	/* Samples the state under the lock. */
	enabled = device_lock();
	live = device->state == NET_DEVICE_LIVE && !device->destroy_pending;
	device_unlock(enabled);

	/* Reports the sampled state. */
	return live;
}

/*
 * Reads the flags of a live device, or none.
 */
unsigned
net_device_flags_get(
	const struct net_device *device)
{
	bool enabled;
	unsigned flags;

	flags = 0;

	/* A missing device has no flags. */
	if (device == NULL)
		return 0;

	/* Samples the flags under the lock. */
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE)
		flags = device->flags;
	device_unlock(enabled);

	/* Reports the sampled flags. */
	return flags;
}

/*
 * Records a carrier change reported by the driver.
 *
 * A change updates the running flag and is announced on the route
 * socket.
 */
int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	bool enabled;
	unsigned event_ifindex;
	unsigned event_flags;
	unsigned transition;
	uint64_t event_generation;
	int error;

	event_ifindex = 0;
	event_flags = 0;
	transition = 0;
	event_generation = 0U;
	error = 0;

	/* Rejects a missing device. */
	if (device == NULL)
		return ENODEV;

	/* Records the carrier of a live device. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		error = ENODEV;
	} else {
		carrier = carrier != 0;
		if (device->carrier != (unsigned)carrier) {
			device->carrier = (unsigned)carrier;
			device_update_running_locked(device);
			event_ifindex = device->ifindex;
			event_generation = device->generation;
			event_flags = device->flags;
			if (carrier)
				transition = RTM_IFINFO_CARRIER_UP;
			else
				transition = RTM_IFINFO_CARRIER_DOWN;
		}
	}
	device_unlock(enabled);

	/* Announces a change. */
	if (transition != 0U && route_socket_notify != NULL)
		route_socket_notify(event_ifindex, event_generation, event_flags,
		    transition);

	/* Reports the update result. */
	return error;
}

/*
 * Reads the carrier of a live device.
 */
int
net_device_carrier(
	const struct net_device *device)
{
	bool enabled;
	int carrier;

	/* A missing device has no carrier. */
	if (device == NULL)
		return 0;

	/* Samples the carrier under the lock. */
	enabled = device_lock();
	carrier = device->state == NET_DEVICE_LIVE && device->carrier;
	device_unlock(enabled);

	/* Reports the sampled carrier. */
	return carrier;
}

/*
 * Tests whether a device is up with carrier.
 */
int
net_device_running(
	const struct net_device *device)
{
	unsigned flags;

	/* The running flag is maintained from the up flag and the carrier. */
	flags = net_device_flags_get(device);
	if ((flags & NET_DEVICE_RUNNING) == 0)
		return 0;
	return 1;
}

/*
 * Reads the capabilities of a live device, or none.
 */
unsigned
net_device_capabilities_get(
	const struct net_device *device)
{
	bool enabled;
	unsigned capabilities;

	capabilities = 0;

	/* A missing device has no capabilities. */
	if (device == NULL)
		return 0;

	/* Samples the capabilities under the lock. */
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE && !device->destroy_pending)
		capabilities = device->capabilities;
	device_unlock(enabled);

	/* Reports the sampled capabilities. */
	return capabilities;
}

/*
 * Opens a device, running the driver's open hook on the first open.
 *
 * Each open holds a reference until its close.  A device that was
 * removed while its open hook ran is closed again at once.
 */
int
net_device_open(
	struct net_device *device)
{
	bool enabled;
	int error;
	int close_after_open;
	int schedule_after_open;

	error = 0;
	close_after_open = 0;
	schedule_after_open = 0;

	/* Rejects a missing device. */
	if (device == NULL)
		return ENODEV;

	/* Only a live device with no open or close in progress can be opened. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE || registry_stopping) {
		device_unlock(enabled);
		if (registry_stopping)
			return EBUSY;
		return ENODEV;
	}
	if (device->opening || device->closing) {
		device_unlock(enabled);
		return EBUSY;
	}

	/* A further open only counts. */
	if (device->open_count != 0) {
		device->open_count++;
		refcount_get(&device->refs);
		device->flags |= NET_DEVICE_UP;
		device_update_running_locked(device);
		device_unlock(enabled);
		return 0;
	}

	/* The first open runs the driver's hook outside the lock. */
	device->opening = 1;
	refcount_get(&device->refs);
	device_unlock(enabled);
	if (device->ops->open != NULL)
		error = device->ops->open(device);

	/* Publishes the open, or undoes it when the device was removed meanwhile. */
	enabled = device_lock();
	device->opening = 0;
	if (error == 0 && device->state == NET_DEVICE_LIVE) {
		device->open_count = 1;
		refcount_get(&device->refs);
		device->flags |= NET_DEVICE_UP;
		device_update_running_locked(device);
		schedule_after_open = device->ops->poll_receive != NULL;
	} else if (error == 0) {
		device->closing = 1;
		close_after_open = 1;
		error = ENODEV;
	}
	device_unlock(enabled);
	if (close_after_open && device->ops->close != NULL)
		device->ops->close(device);
	if (close_after_open) {
		enabled = device_lock();
		device->closing = 0;
		device_unlock(enabled);
	}

	/*
	 * A producer may complete synchronously while open_count is still
	 * zero.  Give every polling driver one post-publication pass so that
	 * bounded work published by such a completion cannot be stranded.
	 */
	if (schedule_after_open)
		net_device_schedule_poll(device);
	net_device_release(device);

	/* Reports the open result. */
	return error;
}

/*
 * Closes a device, running the driver's close hook on the last close.
 */
void
net_device_close(
	struct net_device *device)
{
	bool enabled;
	int call_close;
	int last_close;

	call_close = 0;
	last_close = 0;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Only an open device with no open or close in progress can be closed. */
	enabled = device_lock();
	if (device->open_count == 0 || device->opening || device->closing) {
		device_unlock(enabled);
		return;
	}

	/*
	 * Final close is a callback-join boundary and may yield.  If the
	 * caller already disabled IRQs, conservatively leave the open
	 * reference intact; a thread-context close or gone operation can
	 * retire it later.
	 */
	if (device->open_count == 1 && hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return;
	}

	/* The last close takes the device down. */
	device->open_count--;
	if (device->open_count == 0) {
		last_close = 1;
		device->closing = 1;
		device->poll_scheduled = 0;
		device->flags &= ~(NET_DEVICE_UP | NET_DEVICE_RUNNING);
		call_close = device->ops->close != NULL;
	}
	device_unlock(enabled);

	/* Joins the callbacks, then runs the driver's close hook. */
	if (last_close)
		device_wait_callbacks(device, 0, 0);
	if (call_close)
		device->ops->close(device);
	if (last_close) {
		enabled = device_lock();
		device->closing = 0;
		device_unlock(enabled);
	}
	net_device_release(device);
}

/*
 * Transmits a packet through a running device.
 *
 * The packet is consumed and the statistics updated either way.
 */
int
net_device_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	bool enabled;
	size_t length;
	int error;

	/* Rejects a missing packet or device. */
	if (packet == NULL)
		return EINVAL;
	length = packet->length;
	if (device == NULL) {
		packet_buf_free(packet);
		return ENODEV;
	}

	/* Only a live, up, and running device transmits. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE ||
	    (device->flags & (NET_DEVICE_UP | NET_DEVICE_RUNNING)) !=
	    (NET_DEVICE_UP | NET_DEVICE_RUNNING)) {
		if (device->state != NET_DEVICE_LIVE)
			error = ENODEV;
		else
			error = ENETDOWN;
		if (error == ENETDOWN)
			device->tx_dropped++;
		device_unlock(enabled);
		packet_buf_free(packet);
		return error;
	}

	/* Hands the packet to the driver with a reference held. */
	refcount_get(&device->refs);
	device_unlock(enabled);
	error = device->ops->transmit(device, packet);

	/* Accounts the result. */
	enabled = device_lock();
	if (error == 0) {
		device->tx_packets++;
		device->tx_bytes += length;
	} else {
		device->tx_errors++;
		device->tx_dropped++;
	}
	device_unlock(enabled);
	net_device_release(device);

	/* Reports the transmit result. */
	return error;
}

/*
 * Forwards an ioctl to a driver in thread context.
 *
 * The callback is counted so that close and removal can join it; a
 * device that went away meanwhile reports ENODEV.
 */
int
net_device_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	bool enabled;
	int error;
	int live;

	/* Rejects a missing device. */
	if (device == NULL)
		return ENODEV;

	enabled = device_lock();

	/*
	 * Driver ioctls are thread-context operations.  In particular, do
	 * not run an arbitrary callback with the caller's interrupt state
	 * disabled.
	 */
	if (hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return EWOULDBLOCK;
	}

	/* Only a stable live device with an ioctl hook accepts requests. */
	if (device->state != NET_DEVICE_LIVE || device->destroy_pending) {
		device_unlock(enabled);
		return ENODEV;
	}
	if (device->opening || device->closing || registry_stopping) {
		device_unlock(enabled);
		return EBUSY;
	}
	if (device->ops == NULL || device->ops->ioctl == NULL) {
		device_unlock(enabled);
		return EOPNOTSUPP;
	}
	device->ioctl_active++;
	refcount_get(&device->refs);
	device_unlock(enabled);

	/*
	 * The callback is non-reentrant and must not synchronously enter a
	 * same-device close/removal or terminal shutdown barrier: those
	 * paths may join this count.
	 */
	error = device->ops->ioctl(device, request, argument);

	/* Retires the callback and checks that the device survived it. */
	enabled = device_lock();
	if (device->ioctl_active == 0)
		__builtin_trap();
	device->ioctl_active--;
	live = device->state == NET_DEVICE_LIVE && !device->destroy_pending;
	device_unlock(enabled);
	net_device_release(device);

	/* Reports the driver's result, or the device's disappearance. */
	if (!live)
		return ENODEV;
	return error;
}

/*
 * Counts a transmit error reported by the driver.
 */
void
net_device_tx_error(
	struct net_device *device)
{
	bool enabled;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Counts under the lock. */
	enabled = device_lock();
	device->tx_errors++;
	device_unlock(enabled);
}

/*
 * Queues a packet received by a driver for the network worker.
 *
 * The packet is consumed and the statistics updated either way.
 */
void
net_device_receive(
	struct net_device *device,
	struct packet_buf *packet)
{
	bool enabled;
	size_t length;

	/* Ignores a missing packet; drops one without a device. */
	if (packet == NULL)
		return;
	if (device == NULL) {
		packet_buf_free(packet);
		return;
	}

	/* Only a live device receives. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		packet_buf_free(packet);
		return;
	}
	length = packet->length;

	/*
	 * One reference follows the packet; the other protects stats
	 * publication if the queue consumer frees that packet before enqueue
	 * returns.
	 */
	refcount_get(&device->refs);
	refcount_get(&device->refs);
	packet->device = device;
	device_unlock(enabled);

	/* Queues the packet and accounts the outcome. */
	if (net_input_enqueue(device, packet) != 0) {
		enabled = device_lock();
		device->rx_dropped++;
		device_unlock(enabled);
		net_device_release(device);
		return;
	}
	enabled = device_lock();
	device->rx_packets++;
	device->rx_bytes += length;
	device_unlock(enabled);
	net_device_release(device);
}

/*
 * Asks the network worker to poll a device.
 */
void
net_device_schedule_poll(
	struct net_device *device)
{
	bool enabled;
	int wake;

	wake = 0;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Only a stable open polling device is scheduled. */
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE &&
	    device->open_count != 0 &&
	    !device->opening &&
	    !device->closing &&
	    device->ops->poll_receive != NULL) {
		device->poll_scheduled = 1;
		wake = 1;
	}
	device_unlock(enabled);
	if (wake)
		net_worker_wakeup();
}

/*
 * Runs a scheduled poll of a device within a budget.
 *
 * A poll that used the whole budget leaves the device scheduled.
 */
unsigned
net_device_poll(
	struct net_device *device,
	unsigned budget)
{
	bool enabled;
	unsigned count;

	/* Rejects a missing device or an empty budget. */
	if (device == NULL || budget == 0)
		return 0;

	/* Only a stable open device with a pending poll is polled. */
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE ||
	    device->open_count == 0 ||
	    device->opening ||
	    device->closing ||
	    !device->poll_scheduled ||
	    device->ops->poll_receive == NULL) {
		device_unlock(enabled);
		return 0;
	}
	device->poll_scheduled = 0;
	device->poll_active++;
	refcount_get(&device->refs);
	device_unlock(enabled);

	/* Runs the driver's poll hook outside the lock. */
	count = device->ops->poll_receive(device, budget);

	/* Retires the callback and reschedules an exhausted budget. */
	enabled = device_lock();
	if (device->poll_active == 0)
		__builtin_trap();
	device->poll_active--;
	if (device->state == NET_DEVICE_LIVE &&
	    device->open_count != 0 &&
	    !device->opening &&
	    !device->closing &&
	    count >= budget)
		device->poll_scheduled = 1;
	device_unlock(enabled);
	net_device_release(device);

	/* Reports the packets handled. */
	return count;
}

/*
 * Tests whether a device has a poll scheduled.
 */
int
net_device_poll_pending(
	struct net_device *device)
{
	bool enabled;
	int pending;

	/* A missing device has no poll. */
	if (device == NULL)
		return 0;

	/* A poll is pending on a stable open device that scheduled one. */
	enabled = device_lock();
	pending = 0;
	if (device->state == NET_DEVICE_LIVE &&
	    device->open_count != 0 &&
	    !device->opening &&
	    !device->closing &&
	    device->poll_scheduled)
		pending = 1;
	device_unlock(enabled);

	/* Reports the sampled state. */
	return pending;
}

/* Claims a destroyed, unreferenced, idle device for finalization. */
static int
device_reclaim_locked(
	struct net_device *device,
	struct device_finalizer *finalizer)
{
	/* Nothing may still refer to or run on the device. */
	if (device == NULL ||
	    !device->destroy_pending ||
	    refcount_load(&device->refs) != 0 ||
	    device->state == NET_DEVICE_LIVE ||
	    device->open_count != 0 ||
	    device->opening ||
	    device->closing ||
	    device->poll_active != 0 ||
	    device->ioctl_active != 0 ||
	    device->reclaiming)
		return 0;

	/* Records what the finalizer must release. */
	device->reclaiming = 1;
	finalizer->device = device;
	if (device->ops != NULL)
		finalizer->release = device->ops->release;
	else
		finalizer->release = NULL;
	finalizer->driver_data = device->driver_data;

	/* Reports the claimed device. */
	return 1;
}

/* Runs the driver's release hook and frees a claimed device slot. */
static void
device_finalize(
	struct device_finalizer *finalizer)
{
	struct net_device *device;
	bool enabled;
	unsigned index;

	device = finalizer->device;

	/* Nothing was claimed. */
	if (device == NULL)
		return;

	/* Releases the driver state outside the lock. */
	if (finalizer->release != NULL)
		finalizer->release(finalizer->driver_data);

	/* Frees the slot. */
	enabled = device_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (&devices[index] != device || !device_used[index])
			continue;
		if (!device->reclaiming || refcount_load(&device->refs) != 0)
			__builtin_trap();
		memset(device, 0, sizeof(*device));
		device_used[index] = 0;
		break;
	}
	device_unlock(enabled);
}

/* Drops a reference under the lock, claiming a destroyed device with the last. */
static void
device_put_locked(
	struct net_device *device,
	struct device_finalizer *finalizer)
{
	/* A device that is not being destroyed always keeps its owner's reference. */
	if (!device->destroy_pending) {
		(void)refcount_put_not_last(&device->refs);
		return;
	}

	/* The last reference of a destroyed device claims it. */
	if (refcount_put(&device->refs))
		(void)device_reclaim_locked(device, finalizer);
}

/* Derives the running flag from the state, open count, up flag, and carrier. */
static void
device_update_running_locked(
	struct net_device *device)
{
	if (device->state == NET_DEVICE_LIVE &&
	    device->open_count != 0 &&
	    (device->flags & NET_DEVICE_UP) != 0 &&
	    device->carrier)
		device->flags |= NET_DEVICE_RUNNING;
	else
		device->flags &= ~NET_DEVICE_RUNNING;
}

/* Waits until no poll, ioctl, or selected open/close callback is running. */
static void
device_wait_callbacks(
	struct net_device *device,
	int opening,
	int closing)
{
	bool enabled;
	int pending;

	/* Samples the callback counts and yields while any is active. */
	for (;;) {
		enabled = device_lock();
		pending = 0;
		if (device->poll_active != 0 ||
		    device->ioctl_active != 0 ||
		    (opening && device->opening) ||
		    (closing && device->closing))
			pending = 1;
		device_unlock(enabled);
		if (!pending)
			return;

		/*
		 * Teardown entry points reject callers that already had IRQs
		 * disabled.  This is therefore a thread-context cooperative
		 * wait, never an unbounded spin in interrupt context.
		 */
		if (sched_yield != NULL)
			sched_yield();
		else
			__asm__ volatile("" ::: "memory");
	}
}

/* Waits until a removal in progress has completed. */
static void
device_wait_removed(
	struct net_device *device)
{
	bool enabled;
	int pending;

	/* Samples the state and yields while the removal runs. */
	for (;;) {
		enabled = device_lock();
		pending = device->state == NET_DEVICE_REMOVING;
		device_unlock(enabled);
		if (!pending)
			return;
		if (sched_yield != NULL)
			sched_yield();
		else
			__asm__ volatile("" ::: "memory");
	}
}

/* Disables interrupts, when the HAL is present, and takes the registry lock. */
static bool
device_lock(
	void)
{
	bool enabled;

	/* Without the HAL there are no interrupts to disable. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&device_guard))
		__asm__ volatile("" ::: "memory");

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the registry lock and restores the interrupt state. */
static void
device_unlock(
	bool enabled)
{
	atomic_store_release(&device_guard, 0);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Tests whether a device name is non-empty and fits its field. */
static int
device_name_valid(
	const char *name)
{
	size_t length;

	/* Rejects a missing name. */
	if (name == NULL)
		return 0;

	/* The name must be terminated within the field and non-empty. */
	length = strnlen(name, NET_DEVICE_NAME_MAX);
	if (length == 0)
		return 0;
	if (length >= NET_DEVICE_NAME_MAX)
		return 0;

	/* Reports a usable name. */
	return 1;
}
