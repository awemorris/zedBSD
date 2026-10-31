/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel around the real TCP of the host model (ws220-p001).
 *
 * src/kern/net/tcp.c, socket.c, packet-buf.c, checksum.c and in6.c are
 * compiled as the kernel compiles them and linked with this file, which
 * stands in for everything else they call: a virtual clock, locks and wait
 * queues of one thread, two network devices (one a host) with a route to
 * each other's address, and an IPv4 output that hands each segment to the
 * link of model.c.  Nothing here changes how TCP decides; the model only
 * moves its segments and its clock.
 */

#include "kern/net/tcp-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/clock.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/uaccess.h"
#include "kern/signal.h"
#include "kern/syscall.h"
#include "kern/thread.h"
#include "kern/waitq.h"
#include "src/kern/net/internal.h"
#include "src/kern/net/ipv6.h"
#include "model.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

/* The host C library functions this file calls, declared by hand (the kernel headers are on the path). */
extern void *calloc(size_t count, size_t size);
extern void free(void *pointer);
extern void *memcpy(void *destination, const void *source, size_t count);
extern void *memmove(void *destination, const void *source, size_t count);
extern void *memset(void *destination, int value, size_t count);
extern int memcmp(const void *left, const void *right, size_t count);
extern size_t strlen(const char *string);
extern int strcmp(const char *left, const char *right);

/*
 * The two devices of the model: device 1 is host A (10.0.0.1), device 2
 * host B (10.0.0.2).  They live as long as the program.
 */
static struct net_device model_devices[2];

/* The process every kernel thread belongs to; the model has none and TCP only compares against it. */
struct process process0;

/* The TCP input that IPv4 delivers to, registered by tcp_init(). */
static ipv4_input_fn model_tcp_input;

/* The virtual clock in microseconds since the model began, and the tick it began at. */
static uint64_t model_now_us;
static uint64_t model_tick_base;

/*
 * Sets up the two devices and the start of the clock.
 */
void
model_kernel_init(
	uint64_t tick_base)
{
	unsigned index;

	/* The clock starts at the tick asked for (it chooses the initial sequence numbers). */
	model_tick_base = tick_base;
	model_now_us = 0;

	/* Two live devices, each with its own index and address. */
	for (index = 0; index < 2U; index++) {
		memset(&model_devices[index], 0, sizeof(model_devices[index]));
		model_devices[index].ifindex = index + 1U;
		model_devices[index].mtu = 1500U;
	}
}

/*
 * Moves the virtual clock to a later time.
 */
void
model_clock_set(
	uint64_t now_us)
{
	model_now_us = now_us;
}

/*
 * Reports the virtual clock in microseconds.
 */
uint64_t
model_clock_us(
	void)
{
	return model_now_us;
}

/*
 * Delivers a segment that came off the link to TCP, as IPv4 would.
 */
int
model_deliver(
	const void *data,
	size_t length,
	uint32_t source,
	uint32_t destination)
{
	struct packet_buf *packet;
	void *payload;
	int error;

	/* A packet from the pool, as a driver takes one for each frame it receives (a jumbo one for a large frame). */
	packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM, length, PACKET_BUF_ALLOC_RECEIVE, &error);
	if (packet == NULL)
		return ENOBUFS;

	/* The segment copied in. */
	payload = packet_buf_append(packet, length);
	if (payload == NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}
	memcpy(payload, data, length);

	/* TCP takes the packet. */
	(void)model_tcp_input(packet, source, destination);

	/* Succeeded: delivered. */
	return 0;
}

/* Sets both devices' MTU (--mtu: a jumbo link). */
void
model_device_mtu_set(
	unsigned mtu)
{
	model_devices[0].mtu = mtu;
	model_devices[1].mtu = mtu;
}

/*
 * Reports the device of a host (0 is A, 1 is B).
 */
struct net_device *
model_device(
	unsigned host)
{
	return &model_devices[host];
}

/* The clock, in kernel ticks (MODEL_TICK_HZ a second, the rate of amd64). */
uint64_t
sched_ticks(
	void)
{
	return model_tick_base + model_now_us / (1000000U / MODEL_TICK_HZ);
}

/*
 * Converts milliseconds to the model's ticks, rounding up as a wait does.
 */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Reports the ticks that cover the time. */
	return (milliseconds * MODEL_TICK_HZ + 999U) / 1000U;
}

/*
 * Converts microseconds to the model's ticks, rounding up as a wait does.
 */
uint64_t
kern_us_to_ticks(
	uint64_t microseconds)
{
	/* Reports the ticks that cover the time. */
	return (microseconds * MODEL_TICK_HZ + 999999U) / 1000000U;
}

/*
 * Converts the model's ticks to milliseconds, rounding down as a report does.
 */
uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	/* Reports no more time than the ticks counted. */
	return ticks * 1000U / MODEL_TICK_HZ;
}

/*
 * Converts the model's ticks to microseconds, rounding down as a report does.
 */
uint64_t
kern_ticks_to_us(
	uint64_t ticks)
{
	/* Reports no more time than the ticks counted. */
	return ticks * (1000000U / MODEL_TICK_HZ);
}

/*
 * Converts the model's ticks to a timespec, rounding down.
 */
void
kern_ticks_to_timespec(
	uint64_t ticks,
	struct timespec *result)
{
	/* Whole seconds, then the rest in nanoseconds. */
	result->tv_sec = (time_t)(ticks / MODEL_TICK_HZ);
	result->tv_nsec = (long)(ticks % MODEL_TICK_HZ * (KERN_NSEC_PER_SEC / MODEL_TICK_HZ));
}

/*
 * Converts a duration to the model's ticks, rounding up; the model's
 * durations never overflow.
 */
int
kern_duration_to_ticks_ceil(
	const struct timespec *duration,
	uint64_t *ticks)
{
	/* The whole seconds and the fraction rounded up to a tick. */
	*ticks = (uint64_t)duration->tv_sec * MODEL_TICK_HZ +
	    ((uint64_t)duration->tv_nsec * MODEL_TICK_HZ + KERN_NSEC_PER_SEC - 1U) / KERN_NSEC_PER_SEC;

	/* Succeeded: the ticks that cover the duration. */
	return 0;
}

/*
 * Reports the time between two of the model's ticks, in nanoseconds.
 */
uint64_t
kern_clock_resolution_ns(
	void)
{
	/* Reports one tick. */
	return KERN_NSEC_PER_SEC / MODEL_TICK_HZ;
}

/*
 * Reports the model's ticks in a second.
 */
uint32_t
kern_clock_tick_rate(
	void)
{
	/* Reports the model's rate. */
	return MODEL_TICK_HZ;
}

/* The monotonic counter, the virtual clock at a megahertz (the receive buffer's sizing times the round trip with it). */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	*counter = model_now_us;
	*frequency_hz = 1000000U;
	return true;
}

/* A sleep never happens in the model: its callers run without a thread. */
void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;
}

/* The model runs without a thread, so no call waits for anything. */
struct thread *
thread_current(
	void)
{
	return NULL;
}

/* No thread, so no signal. */
int
signal_pending_unblocked(
	const struct thread *thread)
{
	(void)thread;
	return 0;
}

/* No thread, so no signal is sent. */
int
signal_send_thread(
	struct thread *thread,
	int signo)
{
	(void)thread;
	(void)signo;
	return 0;
}

/* One thread: a lock has nothing to exclude. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

/* One thread: a lock has nothing to exclude. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

/* One thread: a lock has nothing to exclude. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

/* No thread waits, so a wait queue holds nothing. */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	(void)queue;
	(void)name;
}

/* No thread waits, so a wait queue holds nothing. */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	(void)queue;
	return 0;
}

/* A wait that the model's callers never reach ends at once. */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	(void)queue;
	(void)condition_lock;
	(void)observed;
	(void)deadline;
	(void)flags;
	return ETIMEDOUT;
}

/* No thread waits, so a wake finds no one. */
void
waitq_wake_one(
	struct wait_queue *queue)
{
	(void)queue;
}

/* No thread waits, so a wake finds no one. */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	(void)queue;
}

/* The model polls its applications after each event, so readiness needs no notice. */
void
poll_notify(
	void)
{
}

/* The model runs the timer itself at each tick. */
void
net_worker_wakeup(
	void)
{
}

/* A deadline a while after now. */
int
kern_deadline_after(
	uint64_t now,
	uint64_t delta,
	uint64_t *deadline)
{
	*deadline = now + delta;
	return 0;
}

/* A deadline a while after now. */
int
syscall_restart_deadline_after(
	uint64_t ticks,
	uint64_t *deadline)
{
	*deadline = sched_ticks() + ticks;
	return 0;
}

/* The kernel heap is the host's. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

/* The kernel heap is the host's. */
void
kern_free(
	void *pointer)
{
	free(pointer);
}

/* The kcrt copies are the host's. */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	return memcpy(destination, source, count);
}

/* The kcrt copies are the host's. */
void *
kern_memmove(
	void *destination,
	const void *source,
	size_t count)
{
	return memmove(destination, source, count);
}

/* The kcrt copies are the host's. */
void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	return memset(destination, value, count);
}

/* The kcrt copies are the host's. */
int
kern_memcmp(
	const void *left,
	const void *right,
	size_t count)
{
	return memcmp(left, right, count);
}

/* The kcrt copies are the host's. */
size_t
kern_strlen(
	const char *string)
{
	return strlen(string);
}

/* The kcrt copies are the host's. */
int
kern_strcmp(
	const char *left,
	const char *right)
{
	return strcmp(left, right);
}

/* A device is the model's own and is never released. */
struct net_device *
net_device_find_by_index_ref(
	unsigned ifindex)
{
	/* One of the two devices, or none. */
	if (ifindex == 1U || ifindex == 2U)
		return &model_devices[ifindex - 1U];
	return NULL;
}

/* A device is the model's own and is never released. */
void
net_device_ref(
	struct net_device *device)
{
	(void)device;
}

/* A device is the model's own and is never released. */
void
net_device_release(
	struct net_device *device)
{
	(void)device;
}

/* The way to the other host: B's address goes out of A's device, and A's out of B's. */
int
route_lookup_ref(
	uint32_t destination,
	struct net_route *result)
{
	/* The other host's address. */
	memset(result, 0, sizeof(*result));
	if (destination == MODEL_ADDRESS_B) {
		result->device = &model_devices[0];
		return 0;
	}
	if (destination == MODEL_ADDRESS_A) {
		result->device = &model_devices[1];
		return 0;
	}

	/* No other address is reachable. */
	return ENETUNREACH;
}

/* The model's writer never sends from pinned user pages (ws221-p008). */
int
copyin_pinned(
	const struct uaccess_pin *pin,
	size_t offset,
	void *destination,
	size_t size)
{
	(void)pin;
	(void)offset;
	(void)destination;
	(void)size;
	return EFAULT;
}

/* The model's table never changes (ws221-p008). */
unsigned long
route_generation(
	void)
{
	return 0;
}

/* The way to the other host through a device: as route_lookup_ref (ws221-p008). */
int
route_lookup_device_ref(
	uint32_t destination,
	const struct net_device *device,
	struct net_route *result)
{
	(void)device;
	return route_lookup_ref(destination, result);
}

/* Neither host's address is the other's (ws221-p008). */
int
inet_address_is_local(
	uint32_t address)
{
	(void)address;
	return 0;
}

/* A route holds no reference in the model. */
void
route_release(
	struct net_route *route)
{
	(void)route;
}

/* A device's address: A's for device 1, B's for device 2. */
int
inet_interface_address(
	struct net_device *device,
	uint32_t *address,
	uint32_t *netmask,
	uint32_t *broadcast)
{
	(void)netmask;
	(void)broadcast;

	/* The host the device belongs to. */
	if (device == &model_devices[0])
		*address = MODEL_ADDRESS_A;
	else
		*address = MODEL_ADDRESS_B;
	return 0;
}

/* As ipv4_output, the next hop being the destination in the model (ws221-p008). */
int
ipv4_output_routed(
	struct net_device *device,
	uint32_t destination,
	uint32_t next_hop,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	(void)next_hop;
	(void)source;
	return ipv4_output(device, destination, protocol, packet);
}

/* As ipv4_output: the model's device takes the source from its own address (ws221-p008). */
int
ipv4_output_sourced(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	uint32_t source,
	struct packet_buf *packet)
{
	(void)source;
	return ipv4_output(device, destination, protocol, packet);
}

/* IPv4 hands the segment to the link out of the device it was routed to. */
int
ipv4_output(
	struct net_device *device,
	uint32_t destination,
	uint8_t protocol,
	struct packet_buf *packet)
{
	uint32_t source;
	int error;

	(void)protocol;

	/* The source is the device's address, as the pseudo-header was computed with. */
	(void)inet_interface_address(device, &source, NULL, NULL);
	error = model_link_output(device == &model_devices[0] ? 0U : 1U, packet, source, destination);

	/* Reports a full device queue (the packet is consumed either way). */
	if (error != 0)
		return error;

	/* Succeeded: the device has the segment. */
	return 0;
}

/* Only TCP registers with the model's IPv4. */
int
ipv4_protocol_register(
	uint8_t protocol,
	ipv4_input_fn input)
{
	(void)protocol;
	model_tcp_input = input;
	return 0;
}

/* The model has no IPv6. */
int
ipv6_protocol_register(
	uint8_t next_header,
	ipv6_input_fn input)
{
	(void)next_header;
	(void)input;
	return 0;
}

/* The model has no IPv6. */
int
ipv6_output(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint8_t next_header,
	unsigned hop_limit,
	struct packet_buf *packet)
{
	(void)device;
	(void)source;
	(void)destination;
	(void)next_header;
	(void)hop_limit;
	packet_buf_free(packet);
	return ENETUNREACH;
}

/* The model has no IPv6. */
int
ipv6_route_source(
	struct net_device *device,
	const struct in6_addr *destination,
	struct in6_addr *source,
	unsigned *mtu)
{
	(void)device;
	(void)destination;
	(void)source;
	(void)mtu;
	return ENETUNREACH;
}

/* The model has no IPv6. */
int
ipv6_route_way(
	struct net_device *device,
	const struct in6_addr *destination,
	struct net_device **output,
	struct ipv6_way *way)
{
	(void)device;
	(void)destination;
	(void)output;
	(void)way;
	return ENETUNREACH;
}

/* The model has no IPv6. */
int
ipv6_output_way(
	struct net_device *device,
	const struct in6_addr *source,
	const struct in6_addr *destination,
	const struct ipv6_way *way,
	uint8_t next_header,
	struct packet_buf *packet)
{
	(void)device;
	(void)source;
	(void)destination;
	(void)way;
	(void)next_header;
	packet_buf_free(packet);
	return ENETUNREACH;
}

/* The model has no IPv6 addresses of its own. */
int
ipv6_address_is_local(
	const struct in6_addr *address)
{
	(void)address;
	return 0;
}

/* A device's largest frame, as the kernel reckons it (the model's devices are Ethernet, never the loopback). */
size_t
net_device_frame_max(
	const struct net_device *device)
{
	size_t frame;

	/* The header and the MTU, within the largest packet after its headroom. */
	frame = 14U + (size_t)device->mtu;
	if (frame > PACKET_BUF_MAX_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM)
		frame = PACKET_BUF_MAX_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM;
	return frame;
}

/* An IPv4 TCP socket, as the internet layer makes one. */
void
inet_socket_object_init(
	struct inet_socket *inet,
	int type,
	int protocol,
	const struct socket_ops *ops)
{
	memset(inet, 0, sizeof(*inet));
	socket_init_object(&inet->socket, AF_INET, type, protocol, ops);
	inet->family = AF_INET;
}

/* Binds to the address and port given, as the internet layer does for IPv4. */
int
inet_socket_bind(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	const struct sockaddr_in *input;

	(void)length;

	/* The address and port, in host order. */
	input = (const struct sockaddr_in *)address;
	inet->local_address = model_ntohl(input->sin_addr.s_addr);
	inet->local_port = model_ntohs(input->sin_port);
	inet->inet_flags |= INET_SOCKET_BOUND;
	return 0;
}

/* Records the peer, as the internet layer does for IPv4. */
int
inet_socket_connect(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	const struct sockaddr_in *input;

	(void)length;

	/* The peer's address and port, in host order. */
	input = (const struct sockaddr_in *)address;
	inet->remote_address = model_ntohl(input->sin_addr.s_addr);
	inet->remote_port = model_ntohs(input->sin_port);
	inet->inet_flags |= INET_SOCKET_CONNECTED;
	return 0;
}

/* Two sockets conflict on the same port with the same or a wildcard address. */
int
inet_socket_local_conflict(
	const struct inet_socket *existing,
	unsigned existing_reuse,
	const struct inet_socket *candidate,
	unsigned candidate_reuse)
{
	(void)existing_reuse;
	(void)candidate_reuse;

	/* Another port, or a connection (which the four-tuple tells apart). */
	if (existing->local_port != candidate->local_port)
		return 0;
	if ((existing->inet_flags & INET_SOCKET_CONNECTED) != 0)
		return 0;

	/* The same or a wildcard address. */
	if (existing->local_address == candidate->local_address)
		return 1;
	if (existing->local_address == 0 || candidate->local_address == 0)
		return 1;
	return 0;
}

/* Only IPv4 sockets exist in the model. */
int
inet_socket_accepts_ipv4(
	const struct inet_socket *inet)
{
	(void)inet;
	return 1;
}

/* Only IPv4 sockets exist in the model. */
int
inet_socket_accepts_ipv6(
	const struct inet_socket *inet)
{
	(void)inet;
	return 0;
}

/* Only IPv4 sockets exist in the model. */
int
inet_socket_speaks_ipv6(
	const struct inet_socket *inet)
{
	(void)inet;
	return 0;
}

/* The model never asks for a name. */
int
inet_socket_getsockname(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length)
{
	(void)inet;
	(void)address;
	(void)length;
	return 0;
}

/* The model never asks for a name. */
int
inet_socket_getpeername(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length)
{
	(void)inet;
	(void)address;
	(void)length;
	return 0;
}

/* The model sets no option of the internet layer. */
int
inet_socket_setsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	(void)inet;
	(void)level;
	(void)option;
	(void)value;
	(void)length;
	return ENOPROTOOPT;
}

/* The model reads no option of the internet layer. */
int
inet_socket_getsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	(void)inet;
	(void)level;
	(void)option;
	(void)value;
	(void)length;
	return ENOPROTOOPT;
}

/* The model makes no ioctl. */
int
inet_socket_ioctl(
	struct socket *socket,
	unsigned long command,
	uintptr_t argument)
{
	(void)socket;
	(void)command;
	(void)argument;
	return ENOTTY;
}
