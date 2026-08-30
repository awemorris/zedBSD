/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/cred.h"
#include "kern/net/inet-socket.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"

#include <zedbsd/netif.h>
#include <zedbsd/route.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct ucred caller;
static unsigned route_calls;
static unsigned credential_refs;
static unsigned credential_releases;

struct ucred *
cred_current_ref(void)
{
	credential_refs++;
	return &caller;
}

void
cred_release(struct ucred *credential)
{
	assert(credential == &caller);
	credential_releases++;
}

int
cred_is_superuser(const struct ucred *credential)
{
	return credential != NULL && credential->euid == 0;
}

bool
hal_irq_disable(void)
{
	return false;
}

void
hal_irq_enable(void)
{
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	if (source == 0)
		return EFAULT;
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	if (destination == 0)
		return EFAULT;
	memcpy((void *)destination, source, size);
	return 0;
}

void
route_purge_device(struct net_device *device)
{
	(void)device;
}

void
arp_purge_device(struct net_device *device)
{
	(void)device;
}

int
route_add_flags(uint32_t network, uint32_t netmask, uint32_t gateway,
		struct net_device *device, unsigned flags)
{
	(void)network;
	(void)netmask;
	(void)gateway;
	(void)device;
	(void)flags;
	return 0;
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
	route_calls++;
	return argument == 0 ? EFAULT : 0;
}

int
socket_family_register(int family, const struct socket_family_ops *ops)
{
	(void)family;
	(void)ops;
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
	(void)packet;
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

static void
expect_nonroot_denied(unsigned long command)
{
	unsigned calls = route_calls;
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;

	caller.euid = 1000;
	assert(inet_socket_ioctl(NULL, command, 0) == EPERM);
	assert(route_calls == calls);
	assert(credential_refs == refs + 1U);
	assert(credential_releases == releases + 1U);
}

static void
expect_root_reaches_argument_check(unsigned long command)
{
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;

	caller.euid = 0;
	assert(inet_socket_ioctl(NULL, command, 0) == EFAULT);
	assert(credential_refs == refs + 1U);
	assert(credential_releases == releases + 1U);
}

static void
expect_nonroot_query(unsigned long command)
{
	unsigned refs = credential_refs;
	unsigned releases = credential_releases;

	caller.euid = 1000;
	assert(inet_socket_ioctl(NULL, command, 0) == EFAULT);
	assert(credential_refs == refs);
	assert(credential_releases == releases);
}

int
main(void)
{
	static const unsigned long mutations[] = {
		SIOCADDRT, SIOCDELRT, SIOCSIFFLAGS, SIOCSIFADDR,
		SIOCSIFNETMASK, SIOCSIFBRDADDR,
	};
	static const unsigned long queries[] = {
		SIOCGRTENTRY, SIOCGIFCONF, SIOCGIFNAME, SIOCGIFINDEX,
		SIOCGIFFLAGS, SIOCGIFHWADDR, SIOCGIFADDR,
		SIOCGIFNETMASK, SIOCGIFBRDADDR, SIOCGIFMTU,
		SIOCGIFSTATS,
	};
	unsigned index;

	for (index = 0; index < sizeof(mutations) / sizeof(mutations[0]); index++) {
		expect_nonroot_denied(mutations[index]);
		expect_root_reaches_argument_check(mutations[index]);
	}
	for (index = 0; index < sizeof(queries) / sizeof(queries[0]); index++)
		expect_nonroot_query(queries[index]);

	/* Unknown/private commands are privileged by default. */
	expect_nonroot_denied(0xdeadbeefUL);
	puts("inet ioctl authorization tests: PASS");
	return 0;
}
