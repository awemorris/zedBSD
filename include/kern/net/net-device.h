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

#define NET_DEVICE_CAP_WLAN 0x00000001U

struct packet_buf;
struct net_device;

struct net_device_ops {
	int (*open)(struct net_device *device);
	/* Stop I/O producers and drain callbacks synchronously.  close must not
	 * free driver_data; outstanding net_device references may still use it. */
	void (*close)(struct net_device *device);
	int (*transmit)(struct net_device *device, struct packet_buf *packet);
	unsigned (*poll_receive)(struct net_device *device, unsigned budget);
	/* Thread-context and non-reentrant.  The callback must not retain argument,
	 * recursively call net_device_ioctl(), or synchronously call
	 * net_device_close(), net_device_gone(), or net_device_shutdown_all(): those
	 * lifecycle paths may join this admission. */
	int (*ioctl)(struct net_device *device, unsigned long request,
		     void *argument);
	/* Optional final owner for dynamically allocated driver state.  This is
	 * called only after close has returned and the last net_device reference
	 * has retired.  It may free driver_data, but must not access device. */
	void (*release)(void *driver_data);
};

struct net_device {
	char name[NET_DEVICE_NAME_MAX];
	unsigned ifindex;
	uint64_t generation;
	unsigned flags;
	unsigned mtu;
	uint8_t hwaddr[NET_DEVICE_HWADDR_MAX];
	uint8_t hwaddr_len;
	unsigned capabilities;
	const struct net_device_ops *ops;
	void *driver_data;
	unsigned open_count;
	refcount_t refs;
	unsigned state;
	unsigned carrier;
	unsigned destroy_pending;
	unsigned reclaiming;
	unsigned opening;
	unsigned closing;
	unsigned poll_scheduled;
	unsigned poll_active;
	unsigned ioctl_active;
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
/* Thread-context teardown barrier.  The device becomes unobservable before
 * this joins open, close, poll, and admitted ioctl callbacks.  If IRQs were
 * already disabled, EWOULDBLOCK is returned without changing registry or
 * lifecycle state. */
int net_device_gone(struct net_device *device);
/* Thread-context terminal shutdown removes every live device and synchronously
 * retires all of its open references before returning.  If IRQs were already
 * disabled, EWOULDBLOCK is returned without stopping or changing the
 * registry. */
int net_device_shutdown_all(void);
void net_device_destroy(struct net_device *device);
/*
 * Lookup functions return an owned reference.
 */
struct net_device *net_device_find_ref(const char *name);
struct net_device *net_device_find_by_index_ref(unsigned ifindex);
struct net_device *net_device_at_ref(unsigned index);
unsigned net_device_count(void);
void net_device_ref(struct net_device *device);
int net_device_ref_live(struct net_device *device);
void net_device_release(struct net_device *device);
int net_device_is_live(const struct net_device *device);
unsigned net_device_flags_get(const struct net_device *device);
int net_device_set_carrier(struct net_device *device, int carrier);
int net_device_carrier(const struct net_device *device);
int net_device_running(const struct net_device *device);
int net_device_open(struct net_device *device);
/* The final close may yield while an in-flight poll callback retires.  A final
 * close requested with IRQs already disabled is conservatively deferred. */
void net_device_close(struct net_device *device);
int net_device_transmit(struct net_device *device, struct packet_buf *packet);
/* Run one thread-context driver ioctl against a kernel-owned request buffer.
 * This is the only supported driver-ioctl entry point: it admits the callback
 * under the common lifecycle gate and final close/removal joins it before
 * retiring driver_data.  IRQ-disabled callers receive EWOULDBLOCK without
 * admission.  The callback must not retain argument, recursively call
 * net_device_ioctl(), or synchronously call net_device_close(),
 * net_device_gone(), or net_device_shutdown_all(), because those lifecycle
 * paths may wait on this admission. */
int net_device_ioctl(struct net_device *device, unsigned long request,
		     void *argument);
unsigned net_device_capabilities_get(const struct net_device *device);
/* Record one genuine terminal failure reported after transmit accepted a
 * packet. Accepted packet/byte counters remain unchanged and this does not
 * classify the packet as dropped. The driver owns exactly-once publication. */
void net_device_tx_error(struct net_device *device);
void net_device_receive(struct net_device *device, struct packet_buf *packet);
void net_device_schedule_poll(struct net_device *device);
unsigned net_device_poll(struct net_device *device, unsigned budget);
int net_device_poll_pending(struct net_device *device);

#endif
