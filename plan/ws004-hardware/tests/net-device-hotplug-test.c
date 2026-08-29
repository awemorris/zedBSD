/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct packet_buf *queued_packet;
static atomic_uint close_count;
static unsigned release_count;
static unsigned transmit_count;
static atomic_uint sequence;
static atomic_uint close_sequence;
static atomic_uint address_purge_sequence;
static atomic_uint arp_purge_sequence;
static atomic_int block_close;
static atomic_int close_entered;
static atomic_int close_returned;
static atomic_int block_open;
static atomic_int open_entered;
static atomic_int open_returned;
static atomic_int open_result;
static atomic_int block_poll;
static atomic_int poll_entered;
static atomic_int poll_returned;
static atomic_int poll_result;
static atomic_int close_thread_started;
static atomic_int close_thread_returned;
static atomic_int gone_thread_started;
static atomic_int gone_thread_returned;
static atomic_int gone_result;
static atomic_int gone2_thread_started;
static atomic_int gone2_thread_returned;
static atomic_int gone2_result;
static atomic_int shutdown_thread_started;
static atomic_int shutdown_thread_returned;
static atomic_int shutdown_result;
static struct net_device *blocked_close_device;
static unsigned poll_count;
static atomic_uint poll_sequence;
static atomic_int schedule_during_open;
static _Thread_local int irq_enabled = 1;

bool
hal_irq_disable(void)
{
	int previous = irq_enabled;

	irq_enabled = 0;
	return previous != 0;
}

void
hal_irq_enable(void)
{
	irq_enabled = 1;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	memcpy((void *)destination, source, size);
	return 0;
}

void
inet_interface_purge_device(struct net_device *device)
{
	assert(device != NULL);
	address_purge_sequence = ++sequence;
}

void
arp_purge_device(struct net_device *device)
{
	assert(device != NULL);
	arp_purge_sequence = ++sequence;
}

void
net_worker_wakeup(void)
{
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	assert(device != NULL);
	assert(packet != NULL);
	assert(queued_packet == NULL);
	queued_packet = packet;
	return 0;
}

void
packet_buf_free(struct packet_buf *packet)
{
	struct net_device *device;

	if (packet == NULL)
		return;
	device = packet->device;
	packet->device = NULL;
	if (device != NULL)
		net_device_release(device);
}

static int
fixture_open(struct net_device *device)
{
	if (atomic_load_explicit(&block_open, memory_order_acquire)) {
		atomic_store_explicit(&open_entered, 1, memory_order_release);
		while (atomic_load_explicit(&block_open, memory_order_acquire))
			sched_yield();
	}
	(void)net_device_set_carrier(device, 1);
	if (atomic_load_explicit(&schedule_during_open, memory_order_acquire))
		net_device_schedule_poll(device);
	atomic_store_explicit(&open_returned, 1, memory_order_release);
	return 0;
}

static void
fixture_close(struct net_device *device)
{
	if (atomic_load_explicit(&block_close, memory_order_acquire) &&
	    (blocked_close_device == NULL || blocked_close_device == device)) {
		atomic_store_explicit(&close_entered, 1, memory_order_release);
		while (atomic_load_explicit(&block_close, memory_order_acquire))
			sched_yield();
	}
	close_count++;
	close_sequence = ++sequence;
	(void)net_device_set_carrier(device, 0);
	atomic_store_explicit(&close_returned, 1, memory_order_release);
}

static int
fixture_transmit(struct net_device *device, struct packet_buf *packet)
{
	assert(net_device_running(device));
	transmit_count++;
	packet_buf_free(packet);
	return 0;
}

static unsigned
fixture_poll_receive(struct net_device *device, unsigned budget)
{
	assert(net_device_is_live(device));
	assert(budget != 0);
	atomic_store_explicit(&poll_entered, 1, memory_order_release);
	while (atomic_load_explicit(&block_poll, memory_order_acquire))
		sched_yield();
	poll_count++;
	poll_sequence = ++sequence;
	atomic_store_explicit(&poll_returned, 1, memory_order_release);
	return budget;
}

static void
fixture_release(void *driver_data)
{
	assert(driver_data == &release_count);
	release_count++;
}

static const struct net_device_ops fixture_ops = {
	.open = fixture_open,
	.close = fixture_close,
	.transmit = fixture_transmit,
	.poll_receive = fixture_poll_receive,
	.release = fixture_release,
};

static struct net_device *
create_device(const char *name)
{
	struct net_device *device = net_device_alloc();

	assert(device != NULL);
	strcpy(device->name, name);
	device->mtu = 1500;
	device->hwaddr_len = 6;
	device->hwaddr[5] = 1;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->ops = &fixture_ops;
	device->driver_data = &release_count;
	assert(net_device_create(device) == 0);
	return device;
}

static void
destroy_allocated(struct net_device **devices, unsigned count)
{
	unsigned index;

	for (index = 0; index < count; index++)
		net_device_destroy(devices[index]);
}

static void
carrier_and_route_test(void)
{
	struct net_device *allocated[NET_DEVICE_MAX];
	struct net_device *device, *held;
	struct net_route route;
	struct packet_buf packet;
	unsigned index, released;

	device = create_device("ue0");
	assert(net_device_is_live(device));
	assert(!net_device_carrier(device));
	assert(net_device_open(device) == 0);
	assert(net_device_carrier(device));
	assert(net_device_running(device));
	assert((net_device_flags_get(device) & NET_DEVICE_UP) != 0);

	memset(&packet, 0, sizeof(packet));
	packet.length = 64;
	assert(net_device_transmit(device, &packet) == 0);
	assert(transmit_count == 1);
	assert(net_device_set_carrier(device, 0) == 0);
	assert(!net_device_running(device));
	memset(&packet, 0, sizeof(packet));
	packet.length = 64;
	assert(net_device_transmit(device, &packet) == ENETDOWN);
	assert(net_device_set_carrier(device, 1) == 0);

	assert(route_add(0x0a000000U, 0xff000000U, 0, device) == 0);
	assert(route_lookup_ref(0x0a010203U, &route) == 0);
	route_release(&route);
	held = net_device_find_ref("ue0");
	assert(held == device);

	sequence = close_sequence = address_purge_sequence =
	    arp_purge_sequence = 0;
	assert(net_device_gone(device) == 0);
	assert(close_count == 1);
	assert(close_sequence != 0);
	assert(address_purge_sequence > close_sequence);
	assert(arp_purge_sequence > address_purge_sequence);
	assert(net_device_count() == 0);
	assert(net_device_find_ref("ue0") == NULL);
	assert(!net_device_is_live(device));
	assert(!net_device_carrier(device));
	assert(!net_device_running(device));
	assert(net_device_set_carrier(device, 1) == ENODEV);
	assert(route_add(0, 0, 0, device) == ENODEV);
	assert(route_lookup_ref(0x0a010203U, &route) == ENETUNREACH);

	/* destroy consumes the owner reference but the held lookup keeps the
	 * registry slot unavailable until the final external release. */
	released = release_count;
	net_device_destroy(device);
	assert(release_count == released);
	for (index = 0; index < NET_DEVICE_MAX - 1U; index++) {
		allocated[index] = net_device_alloc();
		assert(allocated[index] != NULL);
	}
	assert(net_device_alloc() == NULL);
	net_device_release(held);
	assert(release_count == released + 1U);
	allocated[NET_DEVICE_MAX - 1U] = net_device_alloc();
	assert(allocated[NET_DEVICE_MAX - 1U] == device);
	destroy_allocated(allocated, NET_DEVICE_MAX);
}

static void
queued_receive_test(void)
{
	struct net_device *allocated[NET_DEVICE_MAX];
	struct net_device *device;
	struct packet_buf packet;
	unsigned index, released;

	device = create_device("ue0");
	assert(net_device_open(device) == 0);
	memset(&packet, 0, sizeof(packet));
	packet.length = 128;
	net_device_receive(device, &packet);
	assert(queued_packet == &packet);
	assert(net_device_gone(device) == 0);
	released = release_count;
	net_device_destroy(device);
	assert(release_count == released);
	for (index = 0; index < NET_DEVICE_MAX - 1U; index++) {
		allocated[index] = net_device_alloc();
		assert(allocated[index] != NULL);
	}
	assert(net_device_alloc() == NULL);
	queued_packet = NULL;
	packet_buf_free(&packet);
	assert(release_count == released + 1U);
	allocated[NET_DEVICE_MAX - 1U] = net_device_alloc();
	assert(allocated[NET_DEVICE_MAX - 1U] == device);
	destroy_allocated(allocated, NET_DEVICE_MAX);
}

static void
reconnect_test(void)
{
	unsigned index;

	for (index = 0; index < NET_DEVICE_MAX * 3U; index++) {
		struct net_device *device = create_device("ue0");
		struct net_route route;

		assert(net_device_open(device) == 0);
		assert(route_add(0xc0000200U, 0xffffff00U, 0, device) == 0);
		assert(net_device_gone(device) == 0);
		assert(route_lookup_ref(0xc0000201U, &route) == ENETUNREACH);
		net_device_destroy(device);
		assert(net_device_count() == 0);
	}
}

static void *
close_thread(void *argument)
{
	atomic_store_explicit(&close_thread_started, 1, memory_order_release);
	net_device_close(argument);
	atomic_store_explicit(&close_thread_returned, 1, memory_order_release);
	return NULL;
}

static void *
open_thread(void *argument)
{
	atomic_store(&open_result, net_device_open(argument));
	return NULL;
}

static void *
gone_thread(void *argument)
{
	atomic_store_explicit(&gone_thread_started, 1, memory_order_release);
	atomic_store(&gone_result, net_device_gone(argument));
	atomic_store_explicit(&gone_thread_returned, 1, memory_order_release);
	return NULL;
}

static void *
gone2_thread(void *argument)
{
	atomic_store_explicit(&gone2_thread_started, 1, memory_order_release);
	atomic_store(&gone2_result, net_device_gone(argument));
	atomic_store_explicit(&gone2_thread_returned, 1, memory_order_release);
	return NULL;
}

static void *
shutdown_thread(void *argument)
{
	(void)argument;
	atomic_store_explicit(&shutdown_thread_started, 1,
	    memory_order_release);
	atomic_store(&shutdown_result, net_device_shutdown_all());
	atomic_store_explicit(&shutdown_thread_returned, 1,
	    memory_order_release);
	return NULL;
}

static void *
poll_thread(void *argument)
{
	atomic_store(&poll_result, (int)net_device_poll(argument, 1));
	return NULL;
}

static void
poll_close_barrier_test(void)
{
	struct net_device *device = create_device("ue0");
	pthread_t close_worker, poll_worker;
	unsigned polled = poll_count;

	assert(net_device_open(device) == 0);
	sequence = close_sequence = poll_sequence = 0;
	atomic_store(&block_close, 0);
	atomic_store(&close_entered, 0);
	atomic_store(&close_returned, 0);
	atomic_store(&close_thread_started, 0);
	atomic_store(&close_thread_returned, 0);
	atomic_store(&block_poll, 1);
	atomic_store(&poll_entered, 0);
	atomic_store(&poll_returned, 0);
	atomic_store(&poll_result, 0);
	net_device_schedule_poll(device);
	assert(net_device_poll_pending(device));
	assert(pthread_create(&poll_worker, NULL, poll_thread, device) == 0);
	while (!atomic_load_explicit(&poll_entered, memory_order_acquire))
		sched_yield();
	assert(pthread_create(&close_worker, NULL, close_thread, device) == 0);
	while (!atomic_load_explicit(&close_thread_started,
				    memory_order_acquire) ||
	       (net_device_flags_get(device) & NET_DEVICE_UP) != 0)
		sched_yield();
	/* Closing is now published, so neither another schedule nor driver close
	 * may pass the in-flight poll callback. */
	net_device_schedule_poll(device);
	assert(!net_device_poll_pending(device));
	assert(!atomic_load_explicit(&close_entered, memory_order_acquire));
	assert(!atomic_load_explicit(&close_thread_returned,
				    memory_order_acquire));
	atomic_store_explicit(&block_poll, 0, memory_order_release);
	assert(pthread_join(poll_worker, NULL) == 0);
	assert(pthread_join(close_worker, NULL) == 0);
	assert(atomic_load(&poll_result) == 1);
	assert(atomic_load_explicit(&poll_returned, memory_order_acquire));
	assert(atomic_load_explicit(&close_returned, memory_order_acquire));
	assert(poll_sequence != 0 && close_sequence > poll_sequence);
	assert(poll_count == polled + 1U);
	/* Once final close has returned, poll_receive cannot start again. */
	net_device_schedule_poll(device);
	assert(!net_device_poll_pending(device));
	assert(net_device_poll(device, 1) == 0);
	assert(poll_count == polled + 1U);
	assert(net_device_gone(device) == 0);
	net_device_destroy(device);
}

static void
immediate_completion_open_test(void)
{
	struct net_device *device = create_device("ue0");
	unsigned polled = poll_count;

	atomic_store_explicit(&schedule_during_open, 1, memory_order_release);
	assert(net_device_open(device) == 0);
	atomic_store_explicit(&schedule_during_open, 0, memory_order_release);
	/* The schedule attempted inside ->open() is rejected while opening is
	 * published.  net_device_open() must provide the post-publication pass. */
	assert(net_device_poll_pending(device));
	assert(net_device_poll(device, 1) == 1);
	assert(poll_count == polled + 1U);
	assert(net_device_gone(device) == 0);
	net_device_destroy(device);
}

static void
irq_disabled_gone_test(void)
{
	struct net_device *device = create_device("ue0");
	struct net_device *found;

	assert(net_device_open(device) == 0);
	irq_enabled = 0;
	assert(net_device_gone(device) == EWOULDBLOCK);
	irq_enabled = 1;
	assert(net_device_is_live(device));
	assert(net_device_count() == 1);
	found = net_device_find_ref("ue0");
	assert(found == device);
	net_device_release(found);
	assert(net_device_gone(device) == 0);
	net_device_destroy(device);
}

static void
close_detach_race_test(void)
{
	struct net_device *device = create_device("ue0");
	pthread_t close_worker, gone_worker;
	unsigned released;

	assert(net_device_open(device) == 0);
	atomic_store(&close_entered, 0);
	atomic_store(&close_returned, 0);
	atomic_store(&close_thread_started, 0);
	atomic_store(&close_thread_returned, 0);
	atomic_store(&gone_thread_started, 0);
	atomic_store(&gone_thread_returned, 0);
	atomic_store(&gone_result, -1);
	atomic_store_explicit(&block_close, 1, memory_order_release);
	assert(pthread_create(&close_worker, NULL, close_thread, device) == 0);
	while (!atomic_load_explicit(&close_entered, memory_order_acquire))
		sched_yield();
	released = release_count;
	assert(pthread_create(&gone_worker, NULL, gone_thread, device) == 0);
	while (!atomic_load_explicit(&gone_thread_started, memory_order_acquire))
		sched_yield();
	while (net_device_is_live(device))
		sched_yield();
	/* Detach must not return and permit USB/HCD ownership release while a
	 * concurrent driver close remains inside its drain boundary. */
	assert(!atomic_load_explicit(&close_returned, memory_order_acquire));
	assert(!atomic_load_explicit(&gone_thread_returned, memory_order_acquire));
	assert(release_count == released);
	atomic_store_explicit(&block_close, 0, memory_order_release);
	assert(pthread_join(close_worker, NULL) == 0);
	assert(pthread_join(gone_worker, NULL) == 0);
	assert(atomic_load_explicit(&close_returned, memory_order_acquire));
	assert(atomic_load_explicit(&gone_thread_returned, memory_order_acquire));
	assert(atomic_load(&gone_result) == 0);
	assert(release_count == released);
	net_device_destroy(device);
	assert(release_count == released + 1U);
}

static void
open_detach_race_test(void)
{
	struct net_device *device = create_device("ue0");
	pthread_t gone_worker, open_worker;
	unsigned closed = close_count, released = release_count;

	atomic_store(&open_entered, 0);
	atomic_store(&open_returned, 0);
	atomic_store(&open_result, 0);
	atomic_store(&gone_thread_started, 0);
	atomic_store(&gone_thread_returned, 0);
	atomic_store(&gone_result, -1);
	atomic_store_explicit(&block_open, 1, memory_order_release);
	assert(pthread_create(&open_worker, NULL, open_thread, device) == 0);
	while (!atomic_load_explicit(&open_entered, memory_order_acquire))
		sched_yield();
	assert(pthread_create(&gone_worker, NULL, gone_thread, device) == 0);
	while (!atomic_load_explicit(&gone_thread_started, memory_order_acquire))
		sched_yield();
	while (net_device_is_live(device))
		sched_yield();
	assert(!atomic_load_explicit(&open_returned, memory_order_acquire));
	assert(!atomic_load_explicit(&gone_thread_returned, memory_order_acquire));
	assert(release_count == released);
	atomic_store_explicit(&block_open, 0, memory_order_release);
	assert(pthread_join(open_worker, NULL) == 0);
	assert(pthread_join(gone_worker, NULL) == 0);
	assert(atomic_load(&open_result) == ENODEV);
	assert(atomic_load(&gone_result) == 0);
	assert(close_count == closed + 1U);
	assert(release_count == released);
	net_device_destroy(device);
	assert(release_count == released + 1U);
}

static void
concurrent_gone_barrier_test(void)
{
	struct net_device *device = create_device("ue0");
	pthread_t first, second;
	unsigned closed = close_count, released = release_count;

	assert(net_device_open(device) == 0);
	atomic_store(&close_entered, 0);
	atomic_store(&close_returned, 0);
	atomic_store(&gone_thread_started, 0);
	atomic_store(&gone_thread_returned, 0);
	atomic_store(&gone_result, -1);
	atomic_store(&gone2_thread_started, 0);
	atomic_store(&gone2_thread_returned, 0);
	atomic_store(&gone2_result, -1);
	atomic_store_explicit(&block_close, 1, memory_order_release);
	assert(pthread_create(&first, NULL, gone_thread, device) == 0);
	while (!atomic_load_explicit(&close_entered, memory_order_acquire))
		sched_yield();
	assert(!net_device_is_live(device));
	assert(pthread_create(&second, NULL, gone2_thread, device) == 0);
	while (!atomic_load_explicit(&gone2_thread_started,
				    memory_order_acquire))
		sched_yield();
	/* The second removal owns a barrier reference and cannot authorize bus
	 * teardown while the first removal is still inside driver close. */
	assert(!atomic_load_explicit(&gone_thread_returned,
				    memory_order_acquire));
	assert(!atomic_load_explicit(&gone2_thread_returned,
				    memory_order_acquire));
	assert(release_count == released);
	atomic_store_explicit(&block_close, 0, memory_order_release);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(atomic_load(&gone_result) == 0);
	assert(atomic_load(&gone2_result) == 0);
	assert(close_count == closed + 1U);
	assert(release_count == released);
	net_device_destroy(device);
	assert(release_count == released + 1U);
}

static void
shutdown_all_open_references_test(void)
{
	struct net_device *device = create_device("ue0");
	struct net_device *removing = create_device("ue1");
	pthread_t removal_worker, shutdown_worker;
	unsigned closed = close_count, released = release_count;

	assert(net_device_open(device) == 0);
	assert(net_device_open(device) == 0);
	assert(net_device_open(removing) == 0);
	assert(net_device_running(device));
	atomic_store(&close_entered, 0);
	atomic_store(&close_returned, 0);
	atomic_store(&gone_thread_started, 0);
	atomic_store(&gone_thread_returned, 0);
	atomic_store(&gone_result, -1);
	atomic_store(&shutdown_thread_started, 0);
	atomic_store(&shutdown_thread_returned, 0);
	atomic_store(&shutdown_result, -1);
	blocked_close_device = removing;
	atomic_store_explicit(&block_close, 1, memory_order_release);
	assert(pthread_create(&removal_worker, NULL, gone_thread, removing) == 0);
	while (!atomic_load_explicit(&close_entered, memory_order_acquire))
		sched_yield();
	assert(pthread_create(&shutdown_worker, NULL, shutdown_thread, NULL) == 0);
	while (!atomic_load_explicit(&shutdown_thread_started,
				    memory_order_acquire))
		sched_yield();
	/* shutdown removes the remaining double-open device, but cannot pass the
	 * already-unlinked REMOVING device until its close barrier completes. */
	while (net_device_is_live(device))
		sched_yield();
	assert(!atomic_load_explicit(&shutdown_thread_returned,
				    memory_order_acquire));
	atomic_store_explicit(&block_close, 0, memory_order_release);
	assert(pthread_join(removal_worker, NULL) == 0);
	assert(pthread_join(shutdown_worker, NULL) == 0);
	blocked_close_device = NULL;
	assert(atomic_load(&gone_result) == 0);
	assert(atomic_load(&shutdown_result) == 0);
	assert(close_count == closed + 2U);
	assert(net_device_count() == 0);
	assert(!net_device_is_live(device));
	assert(!net_device_is_live(removing));
	assert(!net_device_running(device));
	net_device_destroy(device);
	net_device_destroy(removing);
	assert(release_count == released + 2U);
}

int
main(void)
{
	net_device_registry_init();
	route_init();
	carrier_and_route_test();
	queued_receive_test();
	immediate_completion_open_test();
	poll_close_barrier_test();
	irq_disabled_gone_test();
	close_detach_race_test();
	open_detach_race_test();
	concurrent_gone_barrier_test();
	reconnect_test();
	shutdown_all_open_references_test();
	puts("net-device hotplug tests: PASS");
	return 0;
}
