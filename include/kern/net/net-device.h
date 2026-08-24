/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Network device
 */

#ifndef ZEDBSD_KERN_NET_NET_DEVICE_H
#define ZEDBSD_KERN_NET_NET_DEVICE_H

#include <kern/atomic.h>
#include <stddef.h>
#include <stdint.h>

#define NET_DEVICE_NAME_MAX 16U
#define NET_DEVICE_HWADDR_MAX 8U
#define NET_DEVICE_MAX 8U

#define NET_DEVICE_UP 0x00000001U
#define NET_DEVICE_RUNNING 0x00000002U
#define NET_DEVICE_BROADCAST 0x00000004U
#define NET_DEVICE_MULTICAST 0x00000008U
#define NET_DEVICE_LOOPBACK 0x00000010U

struct packet_buf;
struct net_device;

struct net_device_ops {
	int (*open)(struct net_device *device);
	void (*close)(struct net_device *device);
	int (*transmit)(struct net_device *device, struct packet_buf *packet);
	unsigned (*poll_receive)(struct net_device *device, unsigned budget);
	int (*ioctl)(struct net_device *device, unsigned long request,
		     void *argument);
};

struct net_device {
	char name[NET_DEVICE_NAME_MAX];
	unsigned ifindex;
	unsigned flags;
	unsigned mtu;
	uint8_t hwaddr[NET_DEVICE_HWADDR_MAX];
	uint8_t hwaddr_len;
	const struct net_device_ops *ops;
	void *driver_data;
	unsigned open_count;
	refcount_t refs;
	unsigned state;
	unsigned opening;
	unsigned closing;
	unsigned poll_scheduled;
	uint64_t rx_packets;
	uint64_t rx_bytes;
	uint64_t rx_errors;
	uint64_t rx_dropped;
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t tx_errors;
	uint64_t tx_dropped;
	struct net_device *next;
};

void net_device_registry_init(void);
struct net_device *net_device_alloc(void);
int net_device_create(struct net_device *device);
void net_device_gone(struct net_device *device);
void net_device_destroy(struct net_device *device);
/*
 * Lookup functions return an owned reference.
 */
struct net_device *net_device_find_ref(const char *name);
struct net_device *net_device_find_by_index_ref(unsigned ifindex);
struct net_device *net_device_at_ref(unsigned index);
unsigned net_device_count(void);
void net_device_ref(struct net_device *device);
void net_device_release(struct net_device *device);
int net_device_open(struct net_device *device);
void net_device_close(struct net_device *device);
int net_device_transmit(struct net_device *device, struct packet_buf *packet);
void net_device_receive(struct net_device *device, struct packet_buf *packet);
void net_device_schedule_poll(struct net_device *device);
unsigned net_device_poll(struct net_device *device, unsigned budget);
int net_device_poll_pending(struct net_device *device);

#endif
