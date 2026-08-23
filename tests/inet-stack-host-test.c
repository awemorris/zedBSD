/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/byteorder.h"
#include "kern/net/ethernet.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/net/socket.h"
#include "kern/net/tcp-socket.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "../src/kern/net/internal.h"
#include "../src/kern/net/wire.h"

#include <zedbsd/netif.h>
#include <zedbsd/netinet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct packet_buf *transmitted;
static uint64_t ticks;
static struct thread *current_thread;
static struct thread host_thread;
struct process;

void __libc_assert_fail(const char *e, const char *f, int l)
{ fprintf(stderr, "%s:%d: assertion failed: %s\n", f, l, e); abort(); }
void *kern_calloc(size_t n, size_t s) { return calloc(n, s); }
void kern_free(void *p) { free(p); }
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
struct thread *thread_current(void) { return current_thread; }
int signal_pending_unblocked(const struct thread *thread)
{
	(void)thread;
	return 0;
}
int signal_send_process(struct process *process, int signal)
{
	(void)process;
	(void)signal;
	return 0;
}
void sched_sleep(uint64_t deadline) { ticks = deadline; }
void sched_wakeup(struct thread *thread) { (void)thread; }
uint64_t sched_ticks(void) { return ticks++; }
int kern_deadline_after(uint64_t now, uint64_t delta, uint64_t *deadline)
{
	if (now > UINT64_MAX - delta) return EOVERFLOW;
	*deadline = now + delta;
	return 0;
}
int copyin(uintptr_t source, void *destination, size_t length)
{ memcpy(destination, (const void *)source, length); return 0; }
int copyout(const void *source, uintptr_t destination, size_t length)
{ memcpy((void *)destination, source, length); return 0; }
int net_input_enqueue(struct net_device *d, struct packet_buf *p)
{ (void)d; packet_buf_free(p); return 0; }
void net_worker_wakeup(void) { }
void packet_socket_deliver(const struct packet_buf *p, const uint8_t s[6],
			   uint8_t t) { (void)p; (void)s; (void)t; }

static int fake_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	assert(transmitted == NULL);
	transmitted = packet;
	return 0;
}

static const struct net_device_ops fake_ops = { .transmit = fake_transmit };

static void configure(struct socket *control, const char *name,
		      unsigned command, uint32_t address)
{
	struct ifreq request;
	struct sockaddr_in *inet = (struct sockaddr_in *)&request.ifr_addr;

	memset(&request, 0, sizeof(request));
	strcpy(request.ifr_name, name);
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = net_htonl(address);
	assert(control->ops->ioctl(control, command, (uintptr_t)&request) == 0);
}

static struct packet_buf *frame(size_t length)
{
	struct packet_buf *packet = packet_buf_alloc(0);
	assert(packet != NULL && packet_buf_append(packet, length) != NULL);
	memset(packet->data, 0, length);
	return packet;
}

static void ethernet_header(uint8_t *data, const uint8_t destination[6],
			    const uint8_t source[6], uint16_t type)
{
	memcpy(data, destination, 6);
	memcpy(data + 6, source, 6);
	data[12] = (uint8_t)(type >> 8);
	data[13] = (uint8_t)type;
}

static void ipv4_header(struct ipv4_wire *ip, uint16_t total, uint8_t protocol,
			uint32_t source, uint32_t destination)
{
	uint16_t checksum;
	memset(ip, 0, sizeof(*ip));
	ip->version_ihl = 0x45;
	wire_put16(ip->total_length, total);
	wire_put16(ip->fragment, 0x4000);
	ip->ttl = 47;
	ip->protocol = protocol;
	wire_put32(ip->source, source);
	wire_put32(ip->destination, destination);
	checksum = net_checksum(ip, sizeof(*ip));
	wire_put16(ip->checksum, checksum);
}

static void input_arp(struct net_device *device, const uint8_t peer[6],
		      uint32_t peer_ip, uint32_t local_ip)
{
	static const uint8_t broadcast[6] = { 255,255,255,255,255,255 };
	struct packet_buf *packet = frame(14 + sizeof(struct arp_wire));
	struct arp_wire *arp = (struct arp_wire *)(packet->data + 14);

	packet->device = device;
	ethernet_header(packet->data, broadcast, peer, ETHERNET_TYPE_ARP);
	wire_put16(arp->hardware_type, 1);
	wire_put16(arp->protocol_type, ETHERNET_TYPE_IPV4);
	arp->hardware_length = 6; arp->protocol_length = 4;
	wire_put16(arp->operation, 1);
	memcpy(arp->sender_hardware, peer, 6);
	wire_put32(arp->sender_protocol, peer_ip);
	wire_put32(arp->target_protocol, local_ip);
	assert(ethernet_input(packet) == 0);
}

static void input_icmp(struct net_device *device, const uint8_t peer[6],
		       uint32_t peer_ip, uint32_t local_ip)
{
	struct packet_buf *packet = frame(14 + 20 + 8);
	struct ipv4_wire *ip = (struct ipv4_wire *)(packet->data + 14);
	struct icmp_wire *icmp = (struct icmp_wire *)(packet->data + 34);
	uint16_t checksum;
	int result;

	packet->device = device;
	ethernet_header(packet->data, device->hwaddr, peer, ETHERNET_TYPE_IPV4);
	ipv4_header(ip, 28, IPPROTO_ICMP, peer_ip, local_ip);
	icmp->type = 8; icmp->code = 0;
	packet->data[38] = 0x12; packet->data[39] = 0x34;
	packet->data[40] = 0; packet->data[41] = 1;
	checksum = net_checksum(icmp, 8);
	wire_put16(icmp->checksum, checksum);
	result = ethernet_input(packet);
	if (result != 0) fprintf(stderr, "ICMP input error: %d\n", result);
	assert(result == 0);
}

static void input_udp(struct net_device *device, const uint8_t peer[6],
		      uint32_t peer_ip, uint32_t local_ip, uint16_t source_port,
		      uint16_t destination_port, const char *payload)
{
	size_t payload_length = strlen(payload);
	struct packet_buf *packet = frame(14 + 20 + 8 + payload_length);
	struct ipv4_wire *ip = (struct ipv4_wire *)(packet->data + 14);
	struct udp_wire *udp = (struct udp_wire *)(packet->data + 34);

	packet->device = device;
	ethernet_header(packet->data, device->hwaddr, peer, ETHERNET_TYPE_IPV4);
	ipv4_header(ip, (uint16_t)(28 + payload_length), IPPROTO_UDP,
	    peer_ip, local_ip);
	wire_put16(udp->source, source_port);
	wire_put16(udp->destination, destination_port);
	wire_put16(udp->length, (uint16_t)(8 + payload_length));
	memcpy(packet->data + 42, payload, payload_length);
	assert(ethernet_input(packet) == 0);
}

static void input_tcp(struct net_device *device, const uint8_t peer[6],
		      uint32_t peer_ip, uint32_t local_ip, uint16_t source_port,
		      uint16_t destination_port, uint32_t sequence, uint32_t ack,
		      uint8_t flags, const char *payload)
{
	size_t payload_length = payload != NULL ? strlen(payload) : 0;
	struct packet_buf *packet = frame(14 + 20 + 20 + payload_length);
	struct ipv4_wire *ip = (struct ipv4_wire *)(packet->data + 14);
	struct tcp_wire *tcp = (struct tcp_wire *)(packet->data + 34);
	uint16_t checksum;

	packet->device = device;
	ethernet_header(packet->data, device->hwaddr, peer, ETHERNET_TYPE_IPV4);
	ipv4_header(ip, (uint16_t)(40 + payload_length), IPPROTO_TCP,
	    peer_ip, local_ip);
	wire_put16(tcp->source, source_port);
	wire_put16(tcp->destination, destination_port);
	wire_put32(tcp->sequence, sequence);
	wire_put32(tcp->acknowledgement, ack);
	tcp->data_offset = 5 << 4; tcp->flags = flags;
	wire_put16(tcp->window, 4096);
	if (payload_length != 0)
		memcpy(packet->data + 54, payload, payload_length);
	checksum = net_checksum_pseudo(peer_ip, local_ip, IPPROTO_TCP,
	    tcp, 20 + payload_length);
	wire_put16(tcp->checksum, checksum);
	assert(ethernet_input(packet) == 0);
}

int main(void)
{
	static const uint8_t local_mac[6] = { 0x52,0x54,0,0x12,0x34,0x56 };
	static const uint8_t peer_mac[6] = { 0x52,0x54,0,0xaa,0xbb,0xcc };
	const uint32_t local_ip = 0x0a00020fU, peer_ip = 0x0a000202U;
	struct net_device *device;
	struct socket *control, *udp_socket, *tcp_socket, *listener, *accepted;
	struct sockaddr_in address;
	char buffer[64];
	ssize_t count;
	uint32_t syn_sequence, peer_sequence = 0x10203040U;
	uint16_t tcp_local_port;

	packet_buf_pool_init(); net_device_registry_init(); socket_core_init();
	ethernet_init(); route_init();
	assert(inet_socket_init() == 0);
	assert(arp_init() == 0); assert(ipv4_init() == 0);
	assert(icmp_init() == 0); assert(udp_init() == 0); assert(tcp_init() == 0);
	device = net_device_alloc(); assert(device != NULL);
	strcpy(device->name, "ne0"); device->mtu = 1500;
	device->hwaddr_len = 6; memcpy(device->hwaddr, local_mac, 6);
	device->flags = NET_DEVICE_BROADCAST; device->ops = &fake_ops;
	assert(net_device_create(device) == 0); assert(net_device_open(device) == 0);
	assert(socket_create(AF_INET, SOCK_RAW, IPPROTO_ICMP, &control) == 0);
	configure(control, "ne0", SIOCSIFADDR, local_ip);
	configure(control, "ne0", SIOCSIFNETMASK, 0xffffff00U);

	input_arp(device, peer_mac, peer_ip, local_ip);
	assert(transmitted != NULL && transmitted->length == 42);
	assert(transmitted->data[21] == 2); packet_buf_free(transmitted); transmitted = NULL;
	input_icmp(device, peer_mac, peer_ip, local_ip);
	count = control->ops->recvfrom(control, buffer, sizeof(buffer),
	    MSG_DONTWAIT, NULL, NULL);
	assert(count == 28);
	assert(((uint8_t *)buffer)[0] == 0x45);
	assert(((uint8_t *)buffer)[8] == 47);
	assert(((uint8_t *)buffer)[9] == IPPROTO_ICMP);
	assert(((uint8_t *)buffer)[20] == 8);
	assert(transmitted != NULL && transmitted->data[34] == 0);
	assert(net_checksum(transmitted->data + 34, 8) == 0);
	packet_buf_free(transmitted); transmitted = NULL;

	assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &udp_socket) == 0);
	{
		struct timeval timeout = { 0, 20000 }, returned = { 0, 0 };
		socklen_t option_length = sizeof(returned);
		assert(socket_setsockopt_common(udp_socket, SOL_SOCKET,
		    SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
		assert(socket_getsockopt_common(udp_socket, SOL_SOCKET,
		    SO_RCVTIMEO, &returned, &option_length) == 0);
		assert(option_length == sizeof(returned));
		assert(returned.tv_sec == 0 && returned.tv_usec == 20000);
		current_thread = &host_thread;
		count = udp_socket->ops->recvfrom(udp_socket, buffer,
		    sizeof(buffer), 0, NULL, NULL);
		current_thread = NULL;
		assert(count == -EAGAIN);
	}
	memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
	address.sin_port = net_htons(1234); address.sin_addr.s_addr = 0;
	assert(udp_socket->ops->bind(udp_socket, (struct sockaddr *)&address,
	    sizeof(address)) == 0);
	input_udp(device, peer_mac, peer_ip, local_ip, 4321, 1234, "hello");
	count = udp_socket->ops->recvfrom(udp_socket, buffer, sizeof(buffer),
	    MSG_DONTWAIT, NULL, NULL);
	assert(count == 5 && !memcmp(buffer, "hello", 5));
	address.sin_port = net_htons(4321);
	address.sin_addr.s_addr = net_htonl(peer_ip);
	count = udp_socket->ops->sendto(udp_socket, "world", 5, 0,
	    (struct sockaddr *)&address, sizeof(address));
	assert(count == 5 && transmitted != NULL);
	assert(transmitted->data[23] == IPPROTO_UDP);
	packet_buf_free(transmitted); transmitted = NULL;
	/* Bind publication is atomic and SO_REUSEADDR requires both endpoints.
	 * Exact duplicate locals remain reserved for a future SO_REUSEPORT. */
	{
		struct socket *a, *b, *c;
		int reuse = 1;
		assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &a) == 0);
		assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &b) == 0);
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = net_htons(9000);
		assert(a->ops->bind(a, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
		address.sin_addr.s_addr = net_htonl(local_ip);
		assert(b->ops->bind(b, (struct sockaddr *)&address,
		    sizeof(address)) == EADDRINUSE);
		assert(((struct inet_socket *)b)->local_port == 0 &&
		    (((struct inet_socket *)b)->inet_flags & INET_SOCKET_BOUND) == 0);
		socket_release(b); socket_release(a);

		assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &a) == 0);
		assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &b) == 0);
		assert(socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &c) == 0);
		assert(socket_setsockopt_common(a, SOL_SOCKET, SO_REUSEADDR,
		    &reuse, sizeof(reuse)) == 0);
		assert(socket_setsockopt_common(b, SOL_SOCKET, SO_REUSEADDR,
		    &reuse, sizeof(reuse)) == 0);
		assert(socket_setsockopt_common(c, SOL_SOCKET, SO_REUSEADDR,
		    &reuse, sizeof(reuse)) == 0);
		address.sin_addr.s_addr = 0;
		assert(a->ops->bind(a, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
		address.sin_addr.s_addr = net_htonl(local_ip);
		assert(b->ops->bind(b, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
		address.sin_addr.s_addr = 0;
		assert(c->ops->bind(c, (struct sockaddr *)&address,
		    sizeof(address)) == EADDRINUSE);
		socket_release(c); socket_release(b); socket_release(a);
	}

	assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &tcp_socket) == 0);
	address.sin_port = net_htons(80); address.sin_addr.s_addr = net_htonl(peer_ip);
	assert(tcp_socket->ops->connect(tcp_socket, (struct sockaddr *)&address,
	    sizeof(address), 0) == EAGAIN);
	assert(transmitted != NULL && (transmitted->data[47] & 0x02) != 0);
	syn_sequence = wire_get32(transmitted->data + 38);
	tcp_local_port = wire_get16(transmitted->data + 34);
	packet_buf_free(transmitted); transmitted = NULL;
	ticks = ((struct tcp_socket *)tcp_socket)->retransmit_deadline;
	tcp_timer_run();
	assert(transmitted != NULL);
	assert(wire_get32(transmitted->data + 38) == syn_sequence);
	assert(((struct tcp_socket *)tcp_socket)->retransmit_count == 1);
	packet_buf_free(transmitted); transmitted = NULL;
	input_tcp(device, peer_mac, peer_ip, local_ip, 80, tcp_local_port,
	    peer_sequence, syn_sequence + 1U, 0x12, NULL);
	assert(((struct tcp_socket *)tcp_socket)->state == TCP_ESTABLISHED);
	assert(transmitted != NULL); packet_buf_free(transmitted); transmitted = NULL;
	count = tcp_socket->ops->sendto(tcp_socket, "GET", 3, 0, NULL, 0);
	assert(count == 3 && transmitted != NULL);
	packet_buf_free(transmitted); transmitted = NULL;
	input_tcp(device, peer_mac, peer_ip, local_ip, 80, tcp_local_port,
	    peer_sequence + 1U, syn_sequence + 4U, 0x18, "OK");
	assert(transmitted != NULL); packet_buf_free(transmitted); transmitted = NULL;
	count = tcp_socket->ops->recvfrom(tcp_socket, buffer, sizeof(buffer),
	    MSG_DONTWAIT, NULL, NULL);
	assert(count == 2 && !memcmp(buffer, "OK", 2));
	assert(tcp_socket->ops->shutdown(tcp_socket, SHUT_RD) == 0);
	assert(tcp_socket->ops->recvfrom(tcp_socket, buffer, sizeof(buffer),
	    MSG_DONTWAIT, NULL, NULL) == 0);
	assert(tcp_socket->ops->shutdown(tcp_socket, SHUT_WR) == 0);
	assert(transmitted != NULL);
	packet_buf_free(transmitted);
	transmitted = NULL;
	assert(tcp_socket->ops->sendto(tcp_socket, "x", 1, MSG_NOSIGNAL,
	    NULL, 0) == -EPIPE);

	/* SO_SNDTIMEO commits a connect failure.  The cancelled generation is
	 * removed from the wire state, so its late SYN-ACK cannot resurrect it. */
	{
		struct socket *timed;
		struct timeval timeout = { .tv_sec = 0, .tv_usec = 10000 };
		uint32_t old_sequence;
		uint16_t old_port;
		assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &timed) == 0);
		assert(socket_setsockopt_common(timed, SOL_SOCKET, SO_SNDTIMEO,
		    &timeout, sizeof(timeout)) == 0);
		current_thread = &host_thread;
		address.sin_port = net_htons(81);
		address.sin_addr.s_addr = net_htonl(peer_ip);
		assert(timed->ops->connect(timed, (struct sockaddr *)&address,
		    sizeof(address), 0) == ETIMEDOUT);
		current_thread = NULL;
		assert(transmitted != NULL);
		old_sequence = wire_get32(transmitted->data + 38);
		old_port = wire_get16(transmitted->data + 34);
		packet_buf_free(transmitted); transmitted = NULL;
		assert(((struct tcp_socket *)timed)->state == TCP_CLOSED);
		assert(((struct tcp_socket *)timed)->active_connect_generation == 0);
		input_tcp(device, peer_mac, peer_ip, local_ip, 81, old_port,
		    peer_sequence, old_sequence + 1U, 0x12, NULL);
		assert(((struct tcp_socket *)timed)->state == TCP_CLOSED);
		assert(transmitted == NULL);
		socket_release(timed);
	}

	assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == 0);
	memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
	address.sin_port = net_htons(8080);
	assert(listener->ops->bind(listener, (struct sockaddr *)&address,
	    sizeof(address)) == 0);
	assert(listener->ops->listen(listener, 4) == 0);
	input_tcp(device, peer_mac, peer_ip, local_ip, 50000, 8080,
	    0x55667788U, 0, 0x02, NULL);
	assert(transmitted != NULL && (transmitted->data[47] & 0x12) == 0x12);
	syn_sequence = wire_get32(transmitted->data + 38);
	packet_buf_free(transmitted); transmitted = NULL;
	/* A duplicate SYN reuses the half-open child and retransmits SYN-ACK. */
	assert(((struct tcp_socket *)listener)->half_open_count == 1);
	input_tcp(device, peer_mac, peer_ip, local_ip, 50000, 8080,
	    0x55667788U, 0, 0x02, NULL);
	assert(((struct tcp_socket *)listener)->half_open_count == 1);
	assert(transmitted != NULL && (transmitted->data[47] & 0x12) == 0x12 &&
	    wire_get32(transmitted->data + 38) == syn_sequence);
	packet_buf_free(transmitted); transmitted = NULL;
	input_tcp(device, peer_mac, peer_ip, local_ip, 50000, 8080,
	    0x55667789U, syn_sequence + 1U, 0x10, NULL);
	assert(listener->ops->accept(listener, &accepted, NULL, NULL,
	    SOCKET_IO_NONBLOCK) == 0);
	assert(((struct tcp_socket *)accepted)->state == TCP_ESTABLISHED);
	if (transmitted != NULL) { packet_buf_free(transmitted); transmitted = NULL; }
	socket_release(listener);
	/* An accepted child no longer belongs to the listener and survives it. */
	assert(((struct tcp_socket *)accepted)->state == TCP_ESTABLISHED);
	socket_release(accepted);
	if (transmitted != NULL) { packet_buf_free(transmitted); transmitted = NULL; }

	/* A full backlog does not allocate another half-open child. */
	assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == 0);
	memset(&address, 0, sizeof(address)); address.sin_family = AF_INET;
	address.sin_port = net_htons(8082);
	assert(listener->ops->bind(listener, (struct sockaddr *)&address,
	    sizeof(address)) == 0);
	assert(listener->ops->listen(listener, 1) == 0);
	input_tcp(device, peer_mac, peer_ip, local_ip, 50001, 8082,
	    0x1000U, 0, 0x02, NULL);
	assert(((struct tcp_socket *)listener)->half_open_count == 1);
	assert(transmitted != NULL);
	packet_buf_free(transmitted); transmitted = NULL;
	input_tcp(device, peer_mac, peer_ip, local_ip, 50002, 8082,
	    0x2000U, 0, 0x02, NULL);
	assert(((struct tcp_socket *)listener)->half_open_count == 1);
	assert(transmitted == NULL);
	socket_release(listener);

	/* TCP permits a reusable wildcard/specific bind pair, but rejects an
	 * ambiguous second listener on the overlapping local endpoint. */
	{
		struct socket *a, *b;
		int reuse = 1;
		assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &a) == 0);
		assert(socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &b) == 0);
		assert(socket_setsockopt_common(a, SOL_SOCKET, SO_REUSEADDR,
		    &reuse, sizeof(reuse)) == 0);
		assert(socket_setsockopt_common(b, SOL_SOCKET, SO_REUSEADDR,
		    &reuse, sizeof(reuse)) == 0);
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = net_htons(8081);
		assert(a->ops->bind(a, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
		address.sin_addr.s_addr = net_htonl(local_ip);
		assert(b->ops->bind(b, (struct sockaddr *)&address,
		    sizeof(address)) == 0);
		assert(a->ops->listen(a, 1) == 0);
		assert(b->ops->listen(b, 1) == EADDRINUSE);
		socket_release(b); socket_release(a);
	}

	socket_release(tcp_socket); socket_release(udp_socket); socket_release(control);
	if (transmitted != NULL) { packet_buf_free(transmitted); transmitted = NULL; }
	net_device_close(device); net_device_gone(device); net_device_destroy(device);
	assert(packet_buf_in_use() == 0);
	puts("zedBSD ARP/IPv4/ICMP/UDP/TCP host tests: PASS");
	return 0;
}
