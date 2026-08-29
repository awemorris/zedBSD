/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

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

static struct net_device devices[NET_DEVICE_MAX];
static uint8_t device_used[NET_DEVICE_MAX];
static struct net_device *device_head;
static unsigned live_count;
static unsigned next_ifindex = 1;
static unsigned removals_active;
static unsigned registry_stopping;
static atomic_uint_t device_guard;

static bool device_lock(void);
static void device_unlock(bool enabled);

struct device_finalizer {
	struct net_device *device;
	void (*release)(void *);
	void *driver_data;
};

static int
device_reclaim_locked(struct net_device *device,
		      struct device_finalizer *finalizer)
{
	if (device == NULL || !device->destroy_pending ||
	    refcount_load(&device->refs) != 0 || device->state == NET_DEVICE_LIVE ||
	    device->open_count != 0 || device->opening || device->closing ||
	    device->poll_active != 0 || device->reclaiming)
		return 0;
	device->reclaiming = 1;
	finalizer->device = device;
	finalizer->release = device->ops != NULL ? device->ops->release : NULL;
	finalizer->driver_data = device->driver_data;
	return 1;
}

static void
device_finalize(struct device_finalizer *finalizer)
{
	struct net_device *device = finalizer->device;
	bool enabled;
	unsigned index;

	if (device == NULL)
		return;
	if (finalizer->release != NULL)
		finalizer->release(finalizer->driver_data);
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

static void
device_put_locked(struct net_device *device,
		  struct device_finalizer *finalizer)
{
	if (!device->destroy_pending) {
		(void)refcount_put_not_last(&device->refs);
		return;
	}
	if (refcount_put(&device->refs))
		(void)device_reclaim_locked(device, finalizer);
}

static void
device_update_running_locked(struct net_device *device)
{
	if (device->state == NET_DEVICE_LIVE && device->open_count != 0 &&
	    (device->flags & NET_DEVICE_UP) != 0 && device->carrier)
		device->flags |= NET_DEVICE_RUNNING;
	else
		device->flags &= ~NET_DEVICE_RUNNING;
}

static void
device_wait_callbacks(struct net_device *device, int opening, int closing)
{
	bool enabled;
	int pending;

	for (;;) {
		enabled = device_lock();
		pending = device->poll_active != 0 ||
		    (opening && device->opening) ||
		    (closing && device->closing);
		device_unlock(enabled);
		if (!pending)
			return;
		/* Teardown entry points reject callers that already had IRQs
		 * disabled.  This is therefore a thread-context cooperative wait,
		 * never an unbounded spin in interrupt context. */
		if (sched_yield != NULL)
			sched_yield();
		else
			__asm__ volatile("" ::: "memory");
	}
}

static void
device_wait_removed(struct net_device *device)
{
	bool enabled;
	int pending;

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

static bool
device_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;

	while (!atomic_try_acquire_zero(&device_guard))
		__asm__ volatile("" ::: "memory");
	return enabled;
}

static void
device_unlock(bool enabled)
{
	atomic_store_release(&device_guard, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

void
net_device_registry_init(void)
{
	bool enabled = device_lock();

	memset(devices, 0, sizeof(devices));
	memset(device_used, 0, sizeof(device_used));
	device_head = NULL;
	live_count = 0;
	next_ifindex = 1;
	removals_active = 0;
	registry_stopping = 0;
	device_unlock(enabled);
}

struct net_device *
net_device_alloc(void)
{
	struct net_device *device = NULL;
	bool enabled = device_lock();
	unsigned index;

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
	return device;
}

static int
device_name_valid(const char *name)
{
	size_t length;

	if (name == NULL)
		return 0;
	length = strnlen(name, NET_DEVICE_NAME_MAX);
	return length != 0 && length < NET_DEVICE_NAME_MAX;
}

int
net_device_create(struct net_device *device)
{
	struct net_device **tail, *other;
	bool enabled;

	if (device == NULL || !device_name_valid(device->name) ||
	    device->mtu == 0 || device->hwaddr_len == 0 ||
	    device->hwaddr_len > NET_DEVICE_HWADDR_MAX || device->ops == NULL ||
	    device->ops->transmit == NULL)
		return EINVAL;
	enabled = device_lock();
	if (device->state != NET_DEVICE_ALLOCATED || registry_stopping) {
		device_unlock(enabled);
		return registry_stopping ? EBUSY : EINVAL;
	}
	for (other = device_head; other != NULL; other = other->next)
		if (!strcmp(other->name, device->name)) {
			device_unlock(enabled);
			return EEXIST;
		}
	if (next_ifindex == 0) {
		device_unlock(enabled);
		return ENOSPC;
	}
	device->ifindex = next_ifindex++;
	device->carrier = (device->flags & NET_DEVICE_RUNNING) != 0;
	device->flags &= ~NET_DEVICE_RUNNING;
	device->state = NET_DEVICE_LIVE;
	device->next = NULL;
	for (tail = &device_head; *tail != NULL; tail = &(*tail)->next)
		;
	*tail = device;
	live_count++;
	device_unlock(enabled);
	return 0;
}

int
net_device_gone(struct net_device *device)
{
	struct net_device **link;
	bool enabled;
	unsigned open_references = 0;
	int call_close = 0;

	if (device == NULL)
		return EINVAL;
	enabled = device_lock();
	if (device->state == NET_DEVICE_REMOVING) {
		/* A second bus teardown is also a barrier.  Hold the object until
		 * the first caller has closed producers and purged every identity. */
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
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		return 0;
	}
	/* false means IRQs were already disabled before device_lock().  Such a
	 * caller cannot join callbacks safely; leave the object entirely live so
	 * its bus owner can defer teardown without losing registry identity. */
	if (hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return EWOULDBLOCK;
	}
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
	device->poll_scheduled = 0;
	/* Keep the slot alive until close and every identity purge has finished. */
	refcount_get(&device->refs);
	if (device->open_count != 0 && !device->closing) {
		open_references = device->open_count;
		device->open_count = 0;
		device->closing = 1;
		call_close = device->ops != NULL && device->ops->close != NULL;
	}
	device_unlock(enabled);
	/* A driver close cannot race poll_receive(): once closing/GONE is
	 * published no new poll starts, and this joins the callback already in
	 * progress before the driver's I/O retirement boundary. */
	if (call_close)
		device_wait_callbacks(device, 0, 0);
	/* close() is the driver retirement boundary: asynchronous producers must
	 * no longer be able to reach driver_data when it returns. */
	if (call_close)
		device->ops->close(device);
	if (open_references != 0) {
		enabled = device_lock();
		device->closing = 0;
		device_unlock(enabled);
	}
	/* If open or close was already executing when removal won the lifecycle
	 * gate, join its compensating close before detach may release bus/HCD
	 * ownership. */
	device_wait_callbacks(device, 1, 1);
	if (route_purge_device != NULL)
		route_purge_device(device);
	if (inet_interface_purge_device != NULL)
		inet_interface_purge_device(device);
	if (arp_purge_device != NULL)
		arp_purge_device(device);
	/* Retire every open-derived reference before publishing completion.  A
	 * terminal shutdown waiting on removals_active may then rely on GONE as
	 * the complete driver-close and reference-drain boundary. */
	if (open_references != 0) {
		while (open_references-- != 0)
			net_device_release(device);
	}
	enabled = device_lock();
	if (device->state != NET_DEVICE_REMOVING)
		__builtin_trap();
	device->state = NET_DEVICE_GONE;
	if (removals_active == 0)
		__builtin_trap();
	removals_active--;
	device_unlock(enabled);
	net_device_release(device);
	return 0;
}

int
net_device_shutdown_all(void)
{
	bool enabled = device_lock();

	registry_stopping = 1U;
	device_unlock(enabled);
	for (;;) {
		struct net_device *device = net_device_at_ref(0);
		int error;

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

void
net_device_destroy(struct net_device *device)
{
	struct device_finalizer finalizer = {0};
	bool enabled;

	if (device == NULL)
		return;
	enabled = device_lock();
	if ((device->state != NET_DEVICE_ALLOCATED &&
	     device->state != NET_DEVICE_GONE) || device->destroy_pending) {
		device_unlock(enabled);
		return;
	}
	/* Consume the allocation owner's permanent reference.  Existing packet,
	 * route, or callback references retire through net_device_release(). */
	device->destroy_pending = 1;
	if (refcount_put(&device->refs))
		(void)device_reclaim_locked(device, &finalizer);
	device_unlock(enabled);
	device_finalize(&finalizer);
}

struct net_device *
net_device_find_ref(const char *name)
{
	struct net_device *device = NULL, *candidate;
	bool enabled;

	if (name == NULL)
		return NULL;
	enabled = device_lock();
	for (candidate = device_head; candidate != NULL; candidate = candidate->next)
		if (!strcmp(candidate->name, name)) {
			refcount_get(&candidate->refs);
			device = candidate;
			break;
		}
	device_unlock(enabled);
	return device;
}

struct net_device *
net_device_find_by_index_ref(unsigned ifindex)
{
	struct net_device *device = NULL, *candidate;
	bool enabled = device_lock();

	for (candidate = device_head; candidate != NULL; candidate = candidate->next)
		if (candidate->ifindex == ifindex) {
			refcount_get(&candidate->refs);
			device = candidate;
			break;
		}
	device_unlock(enabled);
	return device;
}

struct net_device *
net_device_at_ref(unsigned index)
{
	struct net_device *device;
	bool enabled = device_lock();

	for (device = device_head; device != NULL && index != 0;
	     device = device->next, index--)
		;
	if (device != NULL)
		refcount_get(&device->refs);
	device_unlock(enabled);
	return device;
}

unsigned
net_device_count(void)
{
	bool enabled = device_lock();
	unsigned result = live_count;

	device_unlock(enabled);
	return result;
}

void
net_device_ref(struct net_device *device)
{
	bool enabled;

	if (device == NULL)
		return;
	enabled = device_lock();
	if (refcount_load(&device->refs) != 0)
		refcount_get(&device->refs);
	device_unlock(enabled);
}

int
net_device_ref_live(struct net_device *device)
{
	bool enabled;
	int result = 0;

	if (device == NULL)
		return 0;
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE && !device->destroy_pending &&
	    refcount_tryget(&device->refs))
		result = 1;
	device_unlock(enabled);
	return result;
}

void
net_device_release(struct net_device *device)
{
	struct device_finalizer finalizer = {0};
	bool enabled;

	if (device == NULL)
		return;
	enabled = device_lock();
	device_put_locked(device, &finalizer);
	device_unlock(enabled);
	device_finalize(&finalizer);
}

int
net_device_is_live(const struct net_device *device)
{
	bool enabled;
	int live;

	if (device == NULL)
		return 0;
	enabled = device_lock();
	live = device->state == NET_DEVICE_LIVE && !device->destroy_pending;
	device_unlock(enabled);
	return live;
}

unsigned
net_device_flags_get(const struct net_device *device)
{
	bool enabled;
	unsigned flags = 0;

	if (device == NULL)
		return 0;
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE)
		flags = device->flags;
	device_unlock(enabled);
	return flags;
}

int
net_device_set_carrier(struct net_device *device, int carrier)
{
	bool enabled;
	int error = 0;

	if (device == NULL)
		return ENODEV;
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		error = ENODEV;
	} else {
		device->carrier = carrier != 0;
		device_update_running_locked(device);
	}
	device_unlock(enabled);
	return error;
}

int
net_device_carrier(const struct net_device *device)
{
	bool enabled;
	int carrier;

	if (device == NULL)
		return 0;
	enabled = device_lock();
	carrier = device->state == NET_DEVICE_LIVE && device->carrier;
	device_unlock(enabled);
	return carrier;
}

int
net_device_running(const struct net_device *device)
{
	return (net_device_flags_get(device) & NET_DEVICE_RUNNING) != 0;
}

int
net_device_open(struct net_device *device)
{
	bool enabled;
	int error = 0;
	int close_after_open = 0;
	int schedule_after_open = 0;

	if (device == NULL)
		return ENODEV;
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE || registry_stopping) {
		device_unlock(enabled);
		return registry_stopping ? EBUSY : ENODEV;
	}
	if (device->opening || device->closing) {
		device_unlock(enabled);
		return EBUSY;
	}
	if (device->open_count != 0) {
		device->open_count++;
		refcount_get(&device->refs);
		device->flags |= NET_DEVICE_UP;
		device_update_running_locked(device);
		device_unlock(enabled);
		return 0;
	}
	device->opening = 1;
	refcount_get(&device->refs);
	device_unlock(enabled);
	if (device->ops->open != NULL)
		error = device->ops->open(device);
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
	/* A producer may complete synchronously while open_count is still zero.
	 * Give every polling driver one post-publication pass so that bounded work
	 * published by such a completion cannot be stranded. */
	if (schedule_after_open)
		net_device_schedule_poll(device);
	net_device_release(device);
	return error;
}

void
net_device_close(struct net_device *device)
{
	bool enabled;
	int call_close = 0, last_close = 0;

	if (device == NULL)
		return;
	enabled = device_lock();
	if (device->open_count == 0 || device->opening || device->closing) {
		device_unlock(enabled);
		return;
	}
	/* Final close is a callback-join boundary and may yield.  If the caller
	 * already disabled IRQs, conservatively leave the open reference intact;
	 * a thread-context close or gone operation can retire it later. */
	if (device->open_count == 1 && hal_irq_disable != NULL && !enabled) {
		device_unlock(enabled);
		return;
	}
	device->open_count--;
	if (device->open_count == 0) {
		last_close = 1;
		device->closing = 1;
		device->poll_scheduled = 0;
		device->flags &= ~(NET_DEVICE_UP | NET_DEVICE_RUNNING);
		call_close = device->ops->close != NULL;
	}
	device_unlock(enabled);
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

int
net_device_transmit(struct net_device *device, struct packet_buf *packet)
{
	bool enabled;
	int error;
	size_t length;

	if (packet == NULL)
		return EINVAL;
	length = packet->length;
	if (device == NULL) {
		packet_buf_free(packet);
		return ENODEV;
	}
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE ||
	    (device->flags & (NET_DEVICE_UP | NET_DEVICE_RUNNING)) !=
	    (NET_DEVICE_UP | NET_DEVICE_RUNNING)) {
		error = device->state != NET_DEVICE_LIVE ? ENODEV : ENETDOWN;
		if (error == ENETDOWN)
			device->tx_dropped++;
		device_unlock(enabled);
		packet_buf_free(packet);
		return error;
	}
	refcount_get(&device->refs);
	device_unlock(enabled);
	error = device->ops->transmit(device, packet);
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
	return error;
}

void
net_device_receive(struct net_device *device, struct packet_buf *packet)
{
	bool enabled;
	size_t length;

	if (packet == NULL)
		return;
	if (device == NULL) {
		packet_buf_free(packet);
		return;
	}
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		packet_buf_free(packet);
		return;
	}
	length = packet->length;
	/* One reference follows the packet; the other protects stats publication
	 * if the queue consumer frees that packet before enqueue returns. */
	refcount_get(&device->refs);
	refcount_get(&device->refs);
	packet->device = device;
	device_unlock(enabled);
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

void
net_device_schedule_poll(struct net_device *device)
{
	bool enabled;
	int wake = 0;

	if (device == NULL)
		return;
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE &&
	    device->open_count != 0 && !device->opening && !device->closing &&
	    device->ops->poll_receive != NULL) {
		device->poll_scheduled = 1;
		wake = 1;
	}
	device_unlock(enabled);
	if (wake)
		net_worker_wakeup();
}

unsigned
net_device_poll(struct net_device *device, unsigned budget)
{
	bool enabled;
	unsigned count;

	if (device == NULL || budget == 0)
		return 0;
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE || device->open_count == 0 ||
	    device->opening || device->closing || !device->poll_scheduled ||
	    device->ops->poll_receive == NULL) {
		device_unlock(enabled);
		return 0;
	}
	device->poll_scheduled = 0;
	device->poll_active++;
	refcount_get(&device->refs);
	device_unlock(enabled);
	count = device->ops->poll_receive(device, budget);
	enabled = device_lock();
	if (device->poll_active == 0)
		__builtin_trap();
	device->poll_active--;
	if (device->state == NET_DEVICE_LIVE && device->open_count != 0 &&
	    !device->opening && !device->closing && count >= budget)
		device->poll_scheduled = 1;
	device_unlock(enabled);
	net_device_release(device);
	return count;
}

int
net_device_poll_pending(struct net_device *device)
{
	bool enabled;
	int pending;

	if (device == NULL)
		return 0;
	enabled = device_lock();
	pending = device->state == NET_DEVICE_LIVE && device->open_count != 0 &&
	    !device->opening && !device->closing && device->poll_scheduled;
	device_unlock(enabled);
	return pending;
}
