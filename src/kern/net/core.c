/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/route.h"
#include "internal.h"
#include "kern/sched.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdbool.h>
#include <string.h>

#define NET_POLL_BUDGET        16U
#define NET_WORK_BUDGET        32U
#define NET_INPUT_QUEUE_LIMIT  PACKET_BUF_POOL_COUNT

extern int ethernet_input(struct packet_buf *packet);

static struct packet_buf *input_head;
static struct packet_buf *input_tail;
static unsigned input_count;
static struct thread *worker_thread;
static struct net_stats network_stats;
static int network_stopping;

static struct packet_buf *
input_dequeue(void)
{
	struct packet_buf *packet;
	bool enabled = hal_irq_disable();

	packet = input_head;
	if (packet != NULL) {
		input_head = packet->next;
		if (input_head == NULL)
			input_tail = NULL;
		packet->next = NULL;
		if (input_count != 0)
			input_count--;
	}
	if (enabled)
		hal_irq_enable();
	return packet;
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	bool enabled;

	(void)device;
	if (packet == NULL)
		return EINVAL;
	enabled = hal_irq_disable();
	if (network_stopping || input_count >= NET_INPUT_QUEUE_LIMIT) {
		network_stats.input_dropped++;
		if (enabled)
			hal_irq_enable();
		packet_buf_free(packet);
		return ENOBUFS;
	}
	packet->next = NULL;
	if (input_tail != NULL)
		input_tail->next = packet;
	else
		input_head = packet;
	input_tail = packet;
	input_count++;
	if (worker_thread != NULL)
		sched_wakeup(worker_thread);
	if (enabled)
		hal_irq_enable();
	return 0;
}

void
net_worker_wakeup(void)
{
	if (worker_thread != NULL)
		sched_wakeup(worker_thread);
}

static unsigned
poll_devices(void)
{
	unsigned total = 0;
	unsigned index;

	for (index = 0; index < net_device_count() && total < NET_WORK_BUDGET;
	     index++) {
		struct net_device *device = net_device_at(index);
		unsigned count;

		if (device == NULL || !device->poll_scheduled ||
		    device->ops->poll_receive == NULL)
			continue;
		device->poll_scheduled = 0;
		count = device->ops->poll_receive(device, NET_POLL_BUDGET);
		total += count;
		if (count >= NET_POLL_BUDGET)
			device->poll_scheduled = 1;
	}
	return total;
}

static int
work_pending(void)
{
	unsigned index;

	if (input_head != NULL)
		return 1;
	for (index = 0; index < net_device_count(); index++) {
		struct net_device *device = net_device_at(index);

		if (device != NULL && device->poll_scheduled)
			return 1;
	}
	return 0;
}

static void
network_worker(void *argument)
{
	(void)argument;
	for (;;) {
		unsigned work;
		struct packet_buf *packet;

		tcp_timer_run();
		work = poll_devices();

		while (work < NET_WORK_BUDGET &&
		       (packet = input_dequeue()) != NULL) {
			network_stats.input_packets++;
			if (ethernet_input(packet) != 0)
				network_stats.input_errors++;
			work++;
		}
		if (work >= NET_WORK_BUDGET && work_pending()) {
			sched_yield();
			continue;
		}
		if (!work_pending()) {
			bool enabled = hal_irq_disable();
			uint64_t deadline = tcp_timer_next_deadline();

			if (!work_pending())
				sched_sleep(deadline);
			if (enabled)
				hal_irq_enable();
		}
	}
}

int
net_init(void)
{
	int error;

	packet_buf_pool_init();
	net_device_registry_init();
	socket_core_init();
	ethernet_init();
	route_init();
	input_head = input_tail = NULL;
	input_count = 0;
	worker_thread = NULL;
	network_stopping = 0;
	memset(&network_stats, 0, sizeof(network_stats));
	error = packet_socket_init();
	if (error != 0)
		return error;
	error = inet_socket_init();
	if (error != 0)
		return error;
	error = arp_init();
	if (error != 0)
		return error;
	error = ipv4_init();
	if (error != 0)
		return error;
	error = icmp_init();
	if (error != 0)
		return error;
	error = udp_init();
	if (error != 0)
		return error;
	error = tcp_init();
	if (error != 0)
		return error;
	error = kthread_create(network_worker, NULL, SCHED_PRIORITY_DEFAULT,
			       &worker_thread);
	if (error != 0)
		return error;
	thread_start(worker_thread);
	return 0;
}

void
net_shutdown_for_boot(void)
{
	struct packet_buf *packet;
	unsigned index;

	network_stopping = 1;
	for (index = 0; index < net_device_count(); index++) {
		struct net_device *device = net_device_at(index);

		if (device != NULL)
			net_device_close(device);
	}
	while ((packet = input_dequeue()) != NULL)
		packet_buf_free(packet);
}

void
net_get_stats(struct net_stats *stats)
{
	if (stats != NULL)
		*stats = network_stats;
}
