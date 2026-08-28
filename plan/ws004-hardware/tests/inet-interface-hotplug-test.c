/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/byteorder.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"

#include <zedbsd/netif.h>
#include <zedbsd/netinet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
route_purge_device(struct net_device *device)
{
	assert(device != NULL);
}

void
arp_purge_device(struct net_device *device)
{
	assert(device != NULL);
}

int
route_add_flags(uint32_t network, uint32_t netmask, uint32_t gateway,
		struct net_device *device, unsigned flags)
{
	(void)network;
	(void)netmask;
	(void)gateway;
	(void)flags;
	return net_device_is_live(device) ? 0 : ENODEV;
}

int
route_delete(uint32_t network, uint32_t netmask, struct net_device *device)
{
	(void)network;
	(void)netmask;
	(void)device;
	return 0;
}

int
route_ioctl(unsigned long request, uintptr_t argument)
{
	(void)request;
	(void)argument;
	return EOPNOTSUPP;
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	assert(family == AF_INET);
	assert(ops != NULL);
	return 0;
}

int
icmp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
udp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
tcp_socket_create(int protocol, struct socket **result)
{
	(void)protocol;
	(void)result;
	return EOPNOTSUPP;
}

int
net_input_enqueue(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

void
net_worker_wakeup(void)
{
}

void
packet_buf_free(struct packet_buf *packet)
{
	(void)packet;
}

static int
dummy_transmit(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	packet_buf_free(packet);
	return 0;
}

static const struct net_device_ops dummy_ops = {
	.transmit = dummy_transmit,
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
	device->flags = NET_DEVICE_BROADCAST;
	device->ops = &dummy_ops;
	assert(net_device_create(device) == 0);
	return device;
}

static int
set_address(const char *name, uint32_t address)
{
	struct ifreq request;
	struct sockaddr_in *inet = (struct sockaddr_in *)&request.ifr_addr;

	memset(&request, 0, sizeof(request));
	strcpy(request.ifr_name, name);
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = net_htonl(address);
	return inet_socket_ioctl(NULL, SIOCSIFADDR, (uintptr_t)&request);
}

int
main(void)
{
	struct net_device *first, *second;
	uint32_t address;

	net_device_registry_init();
	assert(inet_socket_init() == 0);
	first = create_device("ue0");
	assert(set_address("ue0", 0x0a000001U) == 0);
	assert(inet_interface_configuration(first, &address, NULL, NULL) == 0);
	assert(address == 0x0a000001U);

	assert(net_device_gone(first) == 0);
	assert(set_address("ue0", 0x0a000002U) == ENODEV);
	assert(inet_interface_configuration(first, &address, NULL, NULL) ==
	       EADDRNOTAVAIL);
	net_device_destroy(first);
	second = create_device("ue1");
	assert(second == first);
	assert(inet_interface_configuration(second, &address, NULL, NULL) ==
	       EADDRNOTAVAIL);
	assert(net_device_gone(second) == 0);
	net_device_destroy(second);
	puts("inet interface hotplug tests: PASS");
	return 0;
}
