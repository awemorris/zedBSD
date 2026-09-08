/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network core.
 *
 * Received packets are queued for one worker thread, which also polls
 * the devices and runs the TCP and WLAN timers.  Producers advance a
 * generation under the input lock before waking the worker, so that a
 * wakeup can never be lost between the worker's last check and its
 * sleep.  The loopback device and the protocol subsystems are set up
 * here.
 */

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

static void worker_generation_advance_locked(void);
static int loopback_open(struct net_device *device);
static void loopback_close(struct net_device *device);
static int loopback_transmit(struct net_device *device, struct packet_buf *packet);
static int loopback_init(void);
static struct packet_buf *input_dequeue(void);
static unsigned poll_devices(void);
static int work_pending(void);
static void network_worker(void *argument);
static void wlan_retirement_worker(void *argument);

static const struct net_device_ops loopback_ops = {
    .open = loopback_open,
    .close = loopback_close,
    .transmit = loopback_transmit,
};

/*
 * Queues a received packet for the worker thread.
 *
 * The packet is consumed: dropped while the network is stopping or the
 * queue is full, otherwise queued and the worker woken.
 */
int
net_input_enqueue(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct thread *worker;
	unsigned long irq;

	(void)device;

	/* Rejects a missing packet. */
	if (packet == NULL)
		return EINVAL;

	/* Drops the packet while stopping or when the queue is full. */
	irq = spin_lock_irqsave(&input_lock);

	if (network_stopping || input_count >= NET_INPUT_QUEUE_LIMIT) {
		network_stats.input_dropped++;
		spin_unlock_irqrestore(&input_lock, irq);
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* Appends the packet and publishes the new work. */
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

	/* Wakes the worker outside the lock. */
	if (worker != NULL)
		sched_wakeup(worker);

	/* Reports the queued packet. */
	return 0;
}

/*
 * Wakes the worker thread after a producer published new work.
 */
void
net_worker_wakeup(
	void)
{
	struct thread *worker;
	unsigned long irq;

	/* Publishes the work before waking the worker. */
	irq = spin_lock_irqsave(&input_lock);

	worker_generation_advance_locked();
	worker = worker_thread;

	spin_unlock_irqrestore(&input_lock, irq);

	if (worker != NULL)
		sched_wakeup(worker);
}

/*
 * Initializes the network stack and starts the worker thread.
 *
 * The device registry, loopback device, socket families, and protocols
 * come up in dependency order; any failure is reported to the caller.
 */
int
net_init(
	void)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	/* Brings up the packet pool, the devices, and the loopback device. */
	spin_init(&input_lock, LOCK_RANK_NETWORK, "network input");
	packet_buf_pool_init();
	net_device_registry_init();
	wlan_core_init();
	error = loopback_init();
	if (error != 0)
		return error;

	/* Brings up the socket core, Ethernet, and the routing table. */
	socket_core_init();
	ethernet_init();
	route_init();

	/* Starts with an empty input queue and cleared statistics. */
	input_head = NULL;
	input_tail = NULL;
	input_count = 0;
	irq = spin_lock_irqsave(&input_lock);

	worker_thread = NULL;
	worker_generation = 1U;
	network_stopping = 0;
	memset(&network_stats, 0, sizeof(network_stats));

	spin_unlock_irqrestore(&input_lock, irq);

	/* Registers the socket families and the protocols. */
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

	/*
	 * Retirement may wait in a driver while the packet worker progresses.
	 * Create both threads before starting either.
	 */
	error = kthread_create(wlan_retirement_worker, NULL, SCHED_PRIORITY_DEFAULT,
	    &worker);
	if (error != 0)
		return error;
	error = kthread_create(network_worker, NULL, SCHED_PRIORITY_DEFAULT,
			       &worker_thread);
	if (error != 0) {
		(void)thread_abort_new(worker);
		return error;
	}

	thread_start(worker);
	irq = spin_lock_irqsave(&input_lock);

	worker = worker_thread;

	spin_unlock_irqrestore(&input_lock, irq);

	thread_start(worker);

	/* Reports the running network stack. */
	return 0;
}

/*
 * Stops the network before the kernel hands the machine back to a boot.
 *
 * Input is refused, every device and WLAN station is shut down behind a
 * retried barrier, and the queued packets are dropped.
 */
void
net_shutdown_for_boot(
	void)
{
	struct packet_buf *packet;
	unsigned long irq;
	int error;
	int last_error;

	/* Refuses new input. */
	irq = spin_lock_irqsave(&input_lock);

	network_stopping = 1;

	spin_unlock_irqrestore(&input_lock, irq);

	/*
	 * USB and PCI teardown must not run past a failed network producer
	 * join.  Keep the checked barrier live until every admitted callback
	 * and radio producer has retired.  Log only error transitions to
	 * avoid an unbounded shutdown-time diagnostic storm while a retry is
	 * still making progress.
	 */
	last_error = 0;
	for (;;) {
		error = net_device_shutdown_all();
		if (error == 0)
			break;
		if (error != last_error)
			hal_printf("net: shutdown barrier retry (%d)\n", error);
		last_error = error;
		sched_yield();
	}

	last_error = 0;
	for (;;) {
		error = wlan_station_shutdown_all();
		if (error == 0)
			break;
		if (error != last_error)
			hal_printf("net: WLAN shutdown barrier retry (%d)\n", error);
		last_error = error;
		sched_yield();
	}

	/* Drops the queued packets. */
	for (;;) {
		packet = input_dequeue();
		if (packet == NULL)
			break;
		packet_buf_free(packet);
	}
}

/*
 * Copies the network statistics.
 */
void
net_get_stats(
	struct net_stats *stats)
{
	unsigned long irq;

	/* Ignores a missing result. */
	if (stats == NULL)
		return;

	/* Samples the statistics under the input lock. */
	irq = spin_lock_irqsave(&input_lock);

	*stats = network_stats;

	spin_unlock_irqrestore(&input_lock, irq);
}

/*
 * Runs the WLAN station retirement poll.
 *
 * A persistent bounded poll avoids lost wakeups and works while interfaces
 * are down.  Per-station backoff controls actual stop attempts; no packet
 * admission or network-worker callback is needed to make retirement
 * progress.
 */
static void
wlan_retirement_worker(void *argument)
{
	uint64_t now;

	(void)argument;
	for (;;) {
		wlan_retirement_run(clock_ticks());
		now = clock_ticks();
		sched_sleep(now < UINT64_MAX - KERN_CLOCK_HZ ?
		    now + KERN_CLOCK_HZ : UINT64_MAX);
	}
}

/* Advances the producer generation, skipping zero; the caller holds the lock. */
static void
worker_generation_advance_locked(
	void)
{
	worker_generation++;
	if (worker_generation == 0U)
		worker_generation++;
}

/* Brings the loopback device up by asserting its carrier. */
static int
loopback_open(
	struct net_device *device)
{
	int error;

	/* Reports the carrier change. */
	error = net_device_set_carrier(device, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes the loopback device down by dropping its carrier. */
static void
loopback_close(
	struct net_device *device)
{
	(void)net_device_set_carrier(device, 0);
}

/* Receives a transmitted packet straight back on the loopback device. */
static int
loopback_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	net_device_receive(device, packet);

	/* Reports the transmitted packet. */
	return 0;
}

/* Creates and registers the loopback device lo0. */
static int
loopback_init(
	void)
{
	int error;

	/* Describes the device. */
	loopback_device = net_device_alloc();
	if (loopback_device == NULL)
		return ENOMEM;
	strcpy(loopback_device->name, "lo0");
	loopback_device->mtu = 65535;
	loopback_device->hwaddr_len = 6;
	loopback_device->hwaddr[5] = 1;
	loopback_device->flags = NET_DEVICE_LOOPBACK;
	loopback_device->ops = &loopback_ops;

	/* Registers it, destroying it again on failure. */
	error = net_device_create(loopback_device);
	if (error != 0) {
		net_device_destroy(loopback_device);
		loopback_device = NULL;
	}

	/* Reports why the registration failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes the oldest queued packet, or none. */
static struct packet_buf *
input_dequeue(
	void)
{
	struct packet_buf *packet;
	unsigned long irq;

	/* Unlinks the head of the queue under the lock. */
	irq = spin_lock_irqsave(&input_lock);

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

	/* Reports the packet, or none. */
	return packet;
}

/* Polls every device within the work budget and counts the packets. */
static unsigned
poll_devices(
	void)
{
	struct net_device *device;
	unsigned total;
	unsigned index;
	unsigned count;

	total = 0;

	/* Polls the devices in registry order until the budget is spent. */
	for (index = 0; total < NET_WORK_BUDGET; index++) {
		device = net_device_at_ref(index);
		if (device == NULL)
			break;
		count = net_device_poll(device, NET_POLL_BUDGET);
		total += count;
		net_device_release(device);
	}

	/* Reports the packets handled. */
	return total;
}

/* Tests whether the worker has queued packets, device polls, or WLAN work. */
static int
work_pending(
	void)
{
	struct net_device *device;
	unsigned index;
	unsigned long irq;
	int pending;

	/* Queued input is work. */
	irq = spin_lock_irqsave(&input_lock);

	pending = input_head != NULL;

	spin_unlock_irqrestore(&input_lock, irq);

	if (pending)
		return 1;

	/* A device with a pending poll is work. */
	for (index = 0;; index++) {
		device = net_device_at_ref(index);
		if (device == NULL)
			break;
		if (net_device_poll_pending(device)) {
			net_device_release(device);
			return 1;
		}

		net_device_release(device);
	}

	/* Otherwise the WLAN layer decides. */
	pending = wlan_work_pending();

	/* Reports the WLAN answer. */
	return pending;
}

/* Runs the network worker: timers, device polls, and queued input. */
static void
network_worker(
	void *argument)
{
	struct packet_buf *packet;
	unsigned work;
	unsigned long irq;
	unsigned long sleep_irq;
	uint64_t deadline;
	uint64_t wlan_deadline;
	uint64_t observed;
	int error;

	(void)argument;

	/* Works until the budget is spent, then sleeps until the next event. */
	for (;;) {
		/* Runs the timers and polls the devices. */
		tcp_timer_run();
		wlan_timer_run(clock_ticks());
		work = poll_devices();

		/* Handles queued input within the remaining budget. */
		while (work < NET_WORK_BUDGET) {
			packet = input_dequeue();
			if (packet == NULL)
				break;
			error = ethernet_input(packet);
			irq = spin_lock_irqsave(&input_lock);
			network_stats.input_packets++;
			if (error != 0)
				network_stats.input_errors++;
			spin_unlock_irqrestore(&input_lock, irq);
			work++;
		}

		/* Yields between full budgets while work remains. */
		if (work >= NET_WORK_BUDGET && work_pending()) {
			sched_yield();
			continue;
		}

		if (work_pending())
			continue;

		/*
		 * Observes the producer generation before sampling deadlines.
		 * If a producer publishes a new timer without immediate work
		 * after this point, the final generation comparison prevents
		 * sleeping on the older deadline.
		 */
		sleep_irq = spin_lock_irqsave(&input_lock);
		observed = worker_generation;
		spin_unlock_irqrestore(&input_lock, sleep_irq);
		deadline = tcp_timer_next_deadline();
		wlan_deadline = wlan_timer_next_deadline();
		if (wlan_deadline != 0 &&
		    (deadline == 0 || wlan_deadline < deadline))
			deadline = wlan_deadline;

		/*
		 * Every producer advances this generation while holding
		 * input_lock before waking us.  The final comparison and
		 * sched_sleep_locked() form one atomic handoff: an SMP producer
		 * can no longer wake a still-RUNNING worker and then have that
		 * worker publish THREAD_SLEEPING after the wake was lost.
		 */
		if (!work_pending()) {
			sleep_irq = spin_lock_irqsave(&input_lock);
			if (observed == worker_generation && input_head == NULL)
				sched_sleep_locked(deadline, &input_lock);
			spin_unlock_irqrestore(&input_lock, sleep_irq);
		}
	}
}
