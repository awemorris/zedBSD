/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/socket.h"
#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/route.h"
#include "kern/net/wlan.h"
#include "internal.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdbool.h>
#include <string.h>

#define NET_POLL_BUDGET 16U
#define NET_WORK_BUDGET 32U
#define NET_INPUT_QUEUE_LIMIT PACKET_BUF_POOL_COUNT

extern int ethernet_input(struct packet_buf *packet);

static struct packet_buf *input_head;
static struct packet_buf *input_tail;
static unsigned input_count;
static struct thread *worker_thread;
static uint64_t worker_generation;
static struct net_stats network_stats;
static int network_stopping;
static struct spinlock input_lock;
static struct net_device *loopback_device;

static void
worker_generation_advance_locked(void)
{
	worker_generation++;
	if (worker_generation == 0U)
		worker_generation++;
}

static int
loopback_open(struct net_device *device)
{
	return net_device_set_carrier(device, 1);
}

static void
loopback_close(struct net_device *device)
{
	(void)net_device_set_carrier(device, 0);
}

static int
loopback_transmit(struct net_device *device, struct packet_buf *packet)
{
	net_device_receive(device, packet);
	return 0;
}

static const struct net_device_ops loopback_ops = {
    .open = loopback_open,
    .close = loopback_close,
    .transmit = loopback_transmit,
};

static int
loopback_init(void)
{
	int error;
	loopback_device = net_device_alloc();
	if (loopback_device == NULL)
		return ENOMEM;
	strcpy(loopback_device->name, "lo0");
	loopback_device->mtu = 65535;
	loopback_device->hwaddr_len = 6;
	loopback_device->hwaddr[5] = 1;
	loopback_device->flags = NET_DEVICE_LOOPBACK;
	loopback_device->ops = &loopback_ops;
	error = net_device_create(loopback_device);
	if (error != 0) {
		net_device_destroy(loopback_device);
		loopback_device = NULL;
	}
	return error;
}

static struct packet_buf *
input_dequeue(void)
{
	struct packet_buf *packet;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	packet = input_head;
	if (packet != NULL) {
		input_head = packet->next;
		if (input_head == NULL)
			input_tail = NULL;
		packet->next = NULL;
		if (input_count != 0)
			input_count--;
	}
	spin_unlock_irqrestore(&input_lock, irq);
	return packet;
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	unsigned long irq;
	struct thread *worker;

	(void)device;
	if (packet == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&input_lock);
	if (network_stopping || input_count >= NET_INPUT_QUEUE_LIMIT) {
		network_stats.input_dropped++;
		spin_unlock_irqrestore(&input_lock, irq);
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
	worker_generation_advance_locked();
	worker = worker_thread;
	spin_unlock_irqrestore(&input_lock, irq);
	if (worker != NULL)
		sched_wakeup(worker);
	return 0;
}

void
net_worker_wakeup(void)
{
	struct thread *worker;
	unsigned long irq = spin_lock_irqsave(&input_lock);

	worker_generation_advance_locked();
	worker = worker_thread;
	spin_unlock_irqrestore(&input_lock, irq);
	if (worker != NULL)
		sched_wakeup(worker);
}

static unsigned
poll_devices(void)
{
	unsigned total = 0;
	unsigned index;

	for (index = 0; total < NET_WORK_BUDGET; index++) {
		struct net_device *device = net_device_at_ref(index);
		unsigned count;

		if (device == NULL)
			break;
		count = net_device_poll(device, NET_POLL_BUDGET);
		total += count;
		net_device_release(device);
	}
	return total;
}

static int
work_pending(void)
{
	unsigned index;
	unsigned long irq;
	int pending;

	irq = spin_lock_irqsave(&input_lock);
	pending = input_head != NULL;
	spin_unlock_irqrestore(&input_lock, irq);
	if (pending)
		return 1;
	for (index = 0;; index++) {
		struct net_device *device = net_device_at_ref(index);

		if (device == NULL)
			break;
		if (net_device_poll_pending(device)) {
			net_device_release(device);
			return 1;
		}
		net_device_release(device);
	}
	return wlan_work_pending();
}

static void
network_worker(void *argument)
{
	(void)argument;
	for (;;) {
		unsigned work;
		struct packet_buf *packet;

		tcp_timer_run();
		wlan_timer_run(clock_ticks());
		work = poll_devices();

		while (work < NET_WORK_BUDGET &&
		       (packet = input_dequeue()) != NULL) {
			int error = ethernet_input(packet);
			unsigned long irq = spin_lock_irqsave(&input_lock);

			network_stats.input_packets++;
			if (error != 0)
				network_stats.input_errors++;
			spin_unlock_irqrestore(&input_lock, irq);
			work++;
		}
		if (work >= NET_WORK_BUDGET && work_pending()) {
			sched_yield();
			continue;
		}
		if (!work_pending()) {
			uint64_t deadline;
			uint64_t wlan_deadline;
			uint64_t observed;
			unsigned long irq;

			/* Observe the producer generation before sampling deadlines.  If
			 * a producer publishes a new timer without immediate work after
			 * this point, the final generation comparison prevents sleeping on
			 * the older deadline. */
			irq = spin_lock_irqsave(&input_lock);
			observed = worker_generation;
			spin_unlock_irqrestore(&input_lock, irq);
			deadline = tcp_timer_next_deadline();
			wlan_deadline = wlan_timer_next_deadline();
			if (wlan_deadline != 0 &&
			    (deadline == 0 || wlan_deadline < deadline))
				deadline = wlan_deadline;

			/* Every producer advances this generation while holding
			 * input_lock before waking us.  The final comparison and
			 * sched_sleep_locked() form one atomic handoff: an SMP producer
			 * can no longer wake a still-RUNNING worker and then have that
			 * worker publish THREAD_SLEEPING after the wake was lost. */
			if (!work_pending()) {
				irq = spin_lock_irqsave(&input_lock);
				if (observed == worker_generation && input_head == NULL)
					sched_sleep_locked(deadline, &input_lock);
				spin_unlock_irqrestore(&input_lock, irq);
			}
		}
	}
}

int
net_init(void)
{
	int error;

	spin_init(&input_lock, LOCK_RANK_NETWORK, "network input");
	packet_buf_pool_init();
	net_device_registry_init();
	wlan_core_init();
	error = loopback_init();
	if (error != 0)
		return error;
	socket_core_init();
	ethernet_init();
	route_init();
	input_head = input_tail = NULL;
	input_count = 0;
	{
		unsigned long irq = spin_lock_irqsave(&input_lock);

		worker_thread = NULL;
		worker_generation = 1U;
		network_stopping = 0;
		memset(&network_stats, 0, sizeof(network_stats));
		spin_unlock_irqrestore(&input_lock, irq);
	}
	error = packet_socket_init();
	if (error != 0)
		return error;
	error = route_socket_init();
	if (error != 0)
		return error;
	error = unix_socket_init();
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
	{
		unsigned long irq = spin_lock_irqsave(&input_lock);
		struct thread *worker = worker_thread;

		spin_unlock_irqrestore(&input_lock, irq);
		thread_start(worker);
	}
	return 0;
}

void
net_shutdown_for_boot(void)
{
	struct packet_buf *packet;
	unsigned long irq;
	int error, last_error;

	irq = spin_lock_irqsave(&input_lock);
	network_stopping = 1;
	spin_unlock_irqrestore(&input_lock, irq);
	/* USB and PCI teardown must not run past a failed network producer join.
	 * Keep the checked barrier live until every admitted callback and radio
	 * producer has retired.  Log only error transitions to avoid an unbounded
	 * shutdown-time diagnostic storm while a retry is still making progress. */
	last_error = 0;
	while ((error = net_device_shutdown_all()) != 0) {
		if (error != last_error)
			hal_printf("net: shutdown barrier retry (%d)\n", error);
		last_error = error;
		sched_yield();
	}
	last_error = 0;
	while ((error = wlan_station_shutdown_all()) != 0) {
		if (error != last_error)
			hal_printf("net: WLAN shutdown barrier retry (%d)\n", error);
		last_error = error;
		sched_yield();
	}
	while ((packet = input_dequeue()) != NULL)
		packet_buf_free(packet);
}

void
net_get_stats(struct net_stats *stats)
{
	if (stats != NULL) {
		unsigned long irq = spin_lock_irqsave(&input_lock);

		*stats = network_stats;
		spin_unlock_irqrestore(&input_lock, irq);
	}
}
