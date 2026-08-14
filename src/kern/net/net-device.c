/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

#include <errno.h>
#include <string.h>

#define NET_DEVICE_ALLOCATED 1U
#define NET_DEVICE_LIVE      2U
#define NET_DEVICE_GONE      3U

extern int net_input_enqueue(struct net_device *, struct packet_buf *);
extern void net_worker_wakeup(void);
extern void route_purge_device(struct net_device *) __attribute__((weak));

static struct net_device devices[NET_DEVICE_MAX];
static uint8_t device_used[NET_DEVICE_MAX];
static struct net_device *device_head;
static unsigned live_count;
static unsigned next_ifindex = 1;

void
net_device_registry_init(void)
{
	memset(devices, 0, sizeof(devices));
	memset(device_used, 0, sizeof(device_used));
	device_head = NULL;
	live_count = 0;
	next_ifindex = 1;
}

struct net_device *
net_device_alloc(void)
{
	unsigned index;

	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (device_used[index])
			continue;
		device_used[index] = 1;
		memset(&devices[index], 0, sizeof(devices[index]));
		devices[index].state = NET_DEVICE_ALLOCATED;
		devices[index].refcount = 1;
		return &devices[index];
	}
	return NULL;
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
	struct net_device **tail;

	if (device == NULL || device->state != NET_DEVICE_ALLOCATED ||
	    !device_name_valid(device->name) || device->mtu == 0 ||
	    device->hwaddr_len == 0 ||
	    device->hwaddr_len > NET_DEVICE_HWADDR_MAX ||
	    device->ops == NULL || device->ops->transmit == NULL)
		return EINVAL;
	if (net_device_find(device->name) != NULL)
		return EEXIST;
	if (next_ifindex == 0)
		return ENOSPC;
	device->ifindex = next_ifindex++;
	device->state = NET_DEVICE_LIVE;
	device->next = NULL;
	for (tail = &device_head; *tail != NULL; tail = &(*tail)->next)
		;
	*tail = device;
	live_count++;
	return 0;
}

void
net_device_gone(struct net_device *device)
{
	struct net_device **link;

	if (device == NULL || device->state != NET_DEVICE_LIVE)
		return;
	for (link = &device_head; *link != NULL; link = &(*link)->next) {
		if (*link != device)
			continue;
		*link = device->next;
		if (live_count != 0)
			live_count--;
		break;
	}
	device->next = NULL;
	if (route_purge_device != NULL)
		route_purge_device(device);
	device->state = NET_DEVICE_GONE;
}

void
net_device_destroy(struct net_device *device)
{
	unsigned index;

	if (device == NULL ||
	    (device->state != NET_DEVICE_ALLOCATED &&
	     device->state != NET_DEVICE_GONE) ||
	    device->open_count != 0 || device->refcount != 1)
		return;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (&devices[index] != device)
			continue;
		memset(device, 0, sizeof(*device));
		device_used[index] = 0;
		return;
	}
}

struct net_device *
net_device_find(const char *name)
{
	struct net_device *device;

	if (name == NULL)
		return NULL;
	for (device = device_head; device != NULL; device = device->next)
		if (!strcmp(device->name, name))
			return device;
	return NULL;
}

struct net_device *
net_device_find_by_index(unsigned ifindex)
{
	struct net_device *device;

	for (device = device_head; device != NULL; device = device->next)
		if (device->ifindex == ifindex)
			return device;
	return NULL;
}

struct net_device *
net_device_at(unsigned index)
{
	struct net_device *device;

	for (device = device_head; device != NULL && index != 0;
	     device = device->next, index--)
		;
	return device;
}

unsigned
net_device_count(void)
{
	return live_count;
}

void
net_device_ref(struct net_device *device)
{
	if (device != NULL && device->refcount != 0)
		device->refcount++;
}

void
net_device_release(struct net_device *device)
{
	if (device != NULL && device->refcount != 0)
		device->refcount--;
}

int
net_device_open(struct net_device *device)
{
	int error = 0;

	if (device == NULL || device->state != NET_DEVICE_LIVE)
		return ENODEV;
	if (device->open_count == 0 && device->ops->open != NULL)
		error = device->ops->open(device);
	if (error == 0) {
		device->open_count++;
		net_device_ref(device);
		device->flags |= NET_DEVICE_UP;
	}
	return error;
}

void
net_device_close(struct net_device *device)
{
	if (device == NULL || device->open_count == 0)
		return;
	if (--device->open_count == 0) {
		device->flags &= ~(NET_DEVICE_UP | NET_DEVICE_RUNNING);
		if (device->ops->close != NULL)
			device->ops->close(device);
	}
	net_device_release(device);
}

int
net_device_transmit(struct net_device *device, struct packet_buf *packet)
{
	int error;
	size_t length;

	if (packet == NULL)
		return EINVAL;
	length = packet->length;
	if (device == NULL || device->state != NET_DEVICE_LIVE) {
		packet_buf_free(packet);
		return ENODEV;
	}
	if (!(device->flags & NET_DEVICE_UP)) {
		device->tx_dropped++;
		packet_buf_free(packet);
		return ENETDOWN;
	}
	error = device->ops->transmit(device, packet);
	if (error == 0) {
		device->tx_packets++;
		device->tx_bytes += length;
	} else {
		device->tx_errors++;
		device->tx_dropped++;
	}
	return error;
}

void
net_device_receive(struct net_device *device, struct packet_buf *packet)
{
	size_t length;

	if (packet == NULL)
		return;
	if (device == NULL || device->state != NET_DEVICE_LIVE) {
		packet_buf_free(packet);
		return;
	}
	length = packet->length;
	packet->device = device;
	if (net_input_enqueue(device, packet) != 0) {
		device->rx_dropped++;
		return;
	}
	device->rx_packets++;
	device->rx_bytes += length;
}

void
net_device_schedule_poll(struct net_device *device)
{
	if (device == NULL || device->state != NET_DEVICE_LIVE ||
	    device->ops->poll_receive == NULL)
		return;
	device->poll_scheduled = 1;
	net_worker_wakeup();
}
