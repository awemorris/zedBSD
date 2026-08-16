/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define NET_DEVICE_ALLOCATED 1U
#define NET_DEVICE_LIVE      2U
#define NET_DEVICE_GONE      3U

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern int net_input_enqueue(struct net_device *, struct packet_buf *);
extern void net_worker_wakeup(void);
extern void route_purge_device(struct net_device *) __attribute__((weak));
extern void inet_interface_purge_device(struct net_device *)
    __attribute__((weak));

static struct net_device devices[NET_DEVICE_MAX];
static uint8_t device_used[NET_DEVICE_MAX];
static struct net_device *device_head;
static unsigned live_count;
static unsigned next_ifindex = 1;
static atomic_uint_t device_guard;

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
	if (device->state != NET_DEVICE_ALLOCATED) {
		device_unlock(enabled);
		return EINVAL;
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
	device->state = NET_DEVICE_LIVE;
	device->next = NULL;
	for (tail = &device_head; *tail != NULL; tail = &(*tail)->next)
		;
	*tail = device;
	live_count++;
	device_unlock(enabled);
	return 0;
}

void
net_device_gone(struct net_device *device)
{
	struct net_device **link;
	bool enabled;

	if (device == NULL)
		return;
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		return;
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
	device->state = NET_DEVICE_GONE;
	device->poll_scheduled = 0;
	device_unlock(enabled);
	if (route_purge_device != NULL)
		route_purge_device(device);
	if (inet_interface_purge_device != NULL)
		inet_interface_purge_device(device);
}

void
net_device_destroy(struct net_device *device)
{
	bool enabled;
	unsigned index;

	if (device == NULL)
		return;
	enabled = device_lock();
	if ((device->state != NET_DEVICE_ALLOCATED &&
	     device->state != NET_DEVICE_GONE) || device->open_count != 0 ||
	    device->opening || device->closing || refcount_load(&device->refs) != 1) {
		device_unlock(enabled);
		return;
	}
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (&devices[index] != device)
			continue;
		if (!refcount_put(&device->refs))
			__builtin_trap();
		memset(device, 0, sizeof(*device));
		device_used[index] = 0;
		break;
	}
	device_unlock(enabled);
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
	if (device != NULL)
		refcount_get(&device->refs);
}

void
net_device_release(struct net_device *device)
{
	if (device != NULL)
		(void)refcount_put_not_last(&device->refs);
}

int
net_device_open(struct net_device *device)
{
	bool enabled;
	int error = 0;

	if (device == NULL)
		return ENODEV;
	enabled = device_lock();
	if (device->state != NET_DEVICE_LIVE) {
		device_unlock(enabled);
		return ENODEV;
	}
	if (device->opening || device->closing) {
		device_unlock(enabled);
		return EBUSY;
	}
	if (device->open_count != 0) {
		device->open_count++;
		refcount_get(&device->refs);
		device->flags |= NET_DEVICE_UP;
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
	} else if (error == 0) {
		error = ENODEV;
	}
	device_unlock(enabled);
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
	device->open_count--;
	if (device->open_count == 0) {
		last_close = 1;
		device->closing = 1;
		device->flags &= ~(NET_DEVICE_UP | NET_DEVICE_RUNNING);
		call_close = device->ops->close != NULL;
	}
	device_unlock(enabled);
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
	if (device->state != NET_DEVICE_LIVE || !(device->flags & NET_DEVICE_UP)) {
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
	refcount_get(&device->refs);
	packet->device = device;
	device_unlock(enabled);
	if (net_input_enqueue(device, packet) != 0) {
		enabled = device_lock();
		device->rx_dropped++;
		device_unlock(enabled);
		return;
	}
	enabled = device_lock();
	device->rx_packets++;
	device->rx_bytes += length;
	device_unlock(enabled);
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
	if (device->state != NET_DEVICE_LIVE || !device->poll_scheduled ||
	    device->ops->poll_receive == NULL) {
		device_unlock(enabled);
		return 0;
	}
	device->poll_scheduled = 0;
	refcount_get(&device->refs);
	device_unlock(enabled);
	count = device->ops->poll_receive(device, budget);
	enabled = device_lock();
	if (device->state == NET_DEVICE_LIVE && count >= budget)
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
	pending = device->state == NET_DEVICE_LIVE && device->poll_scheduled;
	device_unlock(enabled);
	return pending;
}
