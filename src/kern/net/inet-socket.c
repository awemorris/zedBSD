/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/route.h"
#include "kern/atomic.h"
#include "kern/cred.h"
#include "kern/uaccess.h"
#include "internal.h"

#include <zedbsd/netif.h>
#include <zedbsd/netinet.h>
#include <zedbsd/route.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

struct inet_interface {
	struct net_device *device;
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
};

static struct inet_interface interfaces[NET_DEVICE_MAX];
static atomic_uint_t interface_guard;

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

static bool
interface_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;

	while (!atomic_try_acquire_zero(&interface_guard))
		__asm__ volatile("" ::: "memory");
	return enabled;
}

static void
interface_unlock(bool enabled)
{
	atomic_store_release(&interface_guard, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

static struct inet_interface *
interface_for_device_locked(struct net_device *device)
{
	unsigned index;

	for (index = 0; index < NET_DEVICE_MAX; index++)
		if (interfaces[index].device == device)
			return &interfaces[index];
	return NULL;
}

static int
interface_ensure(struct net_device *device)
{
	struct net_device *release_device = NULL;
	struct inet_interface *interface;
	bool enabled;
	unsigned index, free_index = NET_DEVICE_MAX;
	int error = 0;

	if (device == NULL)
		return ENODEV;
	enabled = interface_lock();
	interface = interface_for_device_locked(device);
	if (interface != NULL) {
		error = net_device_is_live(device) ? 0 : ENODEV;
		goto out;
	}
	for (index = 0; index < NET_DEVICE_MAX; index++)
		if (interfaces[index].device == NULL) {
			free_index = index;
			break;
		}
	if (free_index == NET_DEVICE_MAX) {
		error = ENOSPC;
		goto out;
	}
	if (!net_device_ref_live(device)) {
		error = ENODEV;
		goto out;
	}
	/* Clear the complete slot before publishing its new identity so values
	 * left by a removed interface cannot alias a reconnected device. */
	memset(&interfaces[free_index], 0, sizeof(interfaces[free_index]));
	interfaces[free_index].device = device;
	if (!net_device_is_live(device)) {
		release_device = interfaces[free_index].device;
		memset(&interfaces[free_index], 0,
		       sizeof(interfaces[free_index]));
		error = ENODEV;
	}
out:
	interface_unlock(enabled);
	if (release_device != NULL)
		net_device_release(release_device);
	return error;
}

int
inet_interface_configuration(struct net_device *device, uint32_t *address,
			     uint32_t *netmask, uint32_t *broadcast)
{
	struct inet_interface *interface;
	bool enabled;
	int error = 0;

	if (device == NULL)
		return EADDRNOTAVAIL;
	enabled = interface_lock();
	interface = interface_for_device_locked(device);
	if (interface == NULL || !net_device_is_live(device)) {
		error = EADDRNOTAVAIL;
	} else {
		if (address != NULL)
			*address = interface->address;
		if (netmask != NULL)
			*netmask = interface->netmask;
		if (broadcast != NULL)
			*broadcast = interface->broadcast;
	}
	interface_unlock(enabled);
	return error;
}

int
inet_interface_address(struct net_device *device, uint32_t *address,
		       uint32_t *netmask, uint32_t *broadcast)
{
	uint32_t configured;
	int error = inet_interface_configuration(device, &configured, netmask,
						 broadcast);

	if (error != 0 || configured == 0)
		return error != 0 ? error : EADDRNOTAVAIL;
	if (address != NULL)
		*address = configured;
	return 0;
}

static void
interface_update_route(struct net_device *device, uint32_t old_address,
		       uint32_t old_netmask, uint32_t address,
		       uint32_t netmask)
{
	if (old_address != 0 && old_netmask != 0)
		(void)route_delete(old_address & old_netmask, old_netmask,
				   device);
	if (address != 0 && netmask != 0)
		(void)route_add_flags(address & netmask, netmask, 0, device,
				      RTF_UP | RTF_CONNECTED);
}

void
inet_socket_object_init(struct inet_socket *inet, int type, int protocol,
			const struct socket_ops *ops)
{
	memset(inet, 0, sizeof(*inet));
	socket_init_object(&inet->socket, AF_INET, type, protocol, ops);
}

int
inet_socket_bind(struct inet_socket *inet, const struct sockaddr *address,
		 socklen_t length)
{
	const struct sockaddr_in *input = (const struct sockaddr_in *)address;
	bool enabled;
	uint32_t local;
	unsigned ifindex = 0, index;

	if (inet == NULL || address == NULL || length < sizeof(*input) ||
	    input->sin_family != AF_INET)
		return EINVAL;
	local = net_ntohl(input->sin_addr.s_addr);
	if (local != INADDR_ANY) {
		enabled = interface_lock();
		for (index = 0; index < NET_DEVICE_MAX; index++)
			if (interfaces[index].device != NULL &&
			    interfaces[index].address == local &&
			    net_device_is_live(interfaces[index].device)) {
				ifindex = interfaces[index].device->ifindex;
				break;
			}
		interface_unlock(enabled);
		if (ifindex == 0)
			return EADDRNOTAVAIL;
		inet->ifindex = ifindex;
	}
	inet->local_address = local;
	inet->local_port = net_ntohs(input->sin_port);
	inet->inet_flags |= INET_SOCKET_BOUND;
	return 0;
}

int
inet_socket_connect(struct inet_socket *inet, const struct sockaddr *address,
		    socklen_t length)
{
	const struct sockaddr_in *input = (const struct sockaddr_in *)address;

	if (inet == NULL || address == NULL || length < sizeof(*input) ||
	    input->sin_family != AF_INET)
		return EINVAL;
	inet->remote_address = net_ntohl(input->sin_addr.s_addr);
	inet->remote_port = net_ntohs(input->sin_port);
	if (inet->remote_address == INADDR_ANY)
		return EADDRNOTAVAIL;
	inet->inet_flags |= INET_SOCKET_CONNECTED;
	return 0;
}

int
inet_socket_local_conflict(const struct inet_socket *existing,
			   unsigned existing_reuse,
			   const struct inet_socket *candidate,
			   unsigned candidate_reuse)
{
	if (existing == NULL || candidate == NULL ||
	    existing->local_port == 0 || candidate->local_port == 0 ||
	    existing->local_port != candidate->local_port)
		return 0;
	if (existing->local_address != 0 && candidate->local_address != 0 &&
	    existing->local_address != candidate->local_address)
		return 0;
	/* SO_REUSEADDR permits wildcard/specific coexistence only when every
	 * participant opted in.  Exact duplicate local endpoints still require
	 * a future SO_REUSEPORT and are therefore rejected. */
	if (existing->local_address == candidate->local_address)
		return 1;
	return existing_reuse == 0 || candidate_reuse == 0;
}

static int
inet_socket_name(struct inet_socket *inet, struct sockaddr *address,
		 socklen_t *length, int peer)
{
	struct sockaddr_in output;
	socklen_t copied;

	if (inet == NULL || address == NULL || length == NULL)
		return EINVAL;
	if (peer && !(inet->inet_flags & INET_SOCKET_CONNECTED))
		return ENOTCONN;
	memset(&output, 0, sizeof(output));
	output.sin_family = AF_INET;
	output.sin_addr.s_addr =
	    net_htonl(peer ? inet->remote_address : inet->local_address);
	output.sin_port =
	    net_htons(peer ? inet->remote_port : inet->local_port);
	copied = *length < sizeof(output) ? *length : sizeof(output);
	memcpy(address, &output, copied);
	*length = sizeof(output);
	return 0;
}

int
inet_socket_getsockname(struct inet_socket *inet, struct sockaddr *address,
			socklen_t *length)
{
	return inet_socket_name(inet, address, length, 0);
}

int
inet_socket_getpeername(struct inet_socket *inet, struct sockaddr *address,
			socklen_t *length)
{
	return inet_socket_name(inet, address, length, 1);
}

int
inet_socket_setsockopt(struct inet_socket *inet, int level, int option,
		       const void *value, socklen_t length)
{
	char name[IFNAMSIZ];
	struct net_device *device;

	if (inet == NULL || level != SOL_SOCKET || option != SO_BINDTODEVICE)
		return EOPNOTSUPP;
	if (value == NULL || length == 0 || length > sizeof(name))
		return EINVAL;
	memset(name, 0, sizeof(name));
	memcpy(name, value, length);
	if (name[length - 1U] != '\0')
		return EINVAL;
	if (name[0] == '\0') {
		inet->ifindex = 0;
		return 0;
	}
	device = net_device_find_ref(name);
	if (device == NULL)
		return ENODEV;
	inet->ifindex = device->ifindex;
	net_device_release(device);
	return 0;
}

int
inet_socket_getsockopt(struct inet_socket *inet, int level, int option,
		       void *value, socklen_t *length)
{
	struct net_device *device;
	size_t required;

	if (inet == NULL || level != SOL_SOCKET || option != SO_BINDTODEVICE)
		return EOPNOTSUPP;
	if (value == NULL || length == NULL)
		return EINVAL;
	device = net_device_find_by_index_ref(inet->ifindex);
	required = device != NULL ? strlen(device->name) + 1U : 1U;
	if (*length < required) {
		net_device_release(device);
		return EINVAL;
	}
	memset(value, 0, required);
	if (device != NULL)
		memcpy(value, device->name, required);
	net_device_release(device);
	*length = (socklen_t)required;
	return 0;
}

static unsigned
device_flags(const struct net_device *device)
{
	unsigned device_state = net_device_flags_get(device);
	unsigned flags = 0;
	if (device_state & NET_DEVICE_UP)
		flags |= IFF_UP;
	if (device_state & NET_DEVICE_RUNNING)
		flags |= IFF_RUNNING;
	if (device_state & NET_DEVICE_BROADCAST)
		flags |= IFF_BROADCAST;
	if (device_state & NET_DEVICE_MULTICAST)
		flags |= IFF_MULTICAST;
	if (device_state & NET_DEVICE_LOOPBACK)
		flags |= IFF_LOOPBACK;
	return flags;
}

static void
set_ifreq_address(struct ifreq *request, uint32_t address)
{
	struct sockaddr_in *output = (struct sockaddr_in *)&request->ifr_addr;

	memset(&request->ifr_addr, 0, sizeof(request->ifr_addr));
	output->sin_family = AF_INET;
	output->sin_addr.s_addr = net_htonl(address);
}

static int
inet_ioctl_ifconf(uintptr_t argument)
{
	struct ifconf configuration;
	struct ifreq request;
	uint32_t required, capacity, copied = 0;
	unsigned index;
	int error;

	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &configuration, sizeof(configuration));
	if (error != 0)
		return error;
	if (configuration.ifc_reserved != 0 ||
	    (configuration.ifc_len % sizeof(struct ifreq)) != 0)
		return EINVAL;
	required = net_device_count() * (uint32_t)sizeof(struct ifreq);
	if (configuration.ifc_buf == 0 || configuration.ifc_len == 0) {
		configuration.ifc_len = required;
		return copyout(&configuration, argument, sizeof(configuration));
	}
	capacity = configuration.ifc_len / (uint32_t)sizeof(struct ifreq);
	for (index = 0; index < capacity; index++) {
		struct net_device *device = net_device_at_ref(index);
		if (device == NULL)
			break;
		memset(&request, 0, sizeof(request));
		memcpy(request.ifr_name, device->name,
		       strnlen(device->name, sizeof(request.ifr_name) - 1U));
		request.ifr_ifindex = (int)device->ifindex;
		error =
		    copyout(&request, (uintptr_t)configuration.ifc_buf + copied,
			    sizeof(request));
		net_device_release(device);
		if (error != 0)
			return error;
		copied += sizeof(request);
	}
	configuration.ifc_len = copied;
	return copyout(&configuration, argument, sizeof(configuration));
}

static int
is_route_request(unsigned long command)
{
	return command == SIOCADDRT || command == SIOCDELRT ||
	       command == SIOCGRTENTRY;
}

/*
 * Keep the unprivileged surface an explicit allow-list.  In particular, a
 * future driver-private ioctl must not become writable merely because its
 * command was not known when this common dispatcher was written.
 */
static bool
inet_ioctl_is_query(unsigned long command)
{
	switch (command) {
	case SIOCGIFCONF:
	case SIOCGIFNAME:
	case SIOCGIFINDEX:
	case SIOCGIFFLAGS:
	case SIOCGIFHWADDR:
	case SIOCGIFADDR:
	case SIOCGIFNETMASK:
	case SIOCGIFBRDADDR:
	case SIOCGIFMTU:
	case SIOCGIFSTATS:
	case SIOCGRTENTRY:
		return true;
	default:
		return false;
	}
}

static bool
inet_ioctl_caller_is_superuser(void)
{
	struct ucred *credential = cred_current_ref();
	bool permitted = cred_is_superuser(credential);

	cred_release(credential);
	return permitted;
}

int
inet_socket_ioctl(struct socket *socket, unsigned long command,
		  uintptr_t argument)
{
	struct ifreq request;
	struct net_device *device;
	struct inet_interface *interface;
	bool enabled;
	uint32_t address, broadcast, netmask, old_address, old_netmask;
	int error;

	(void)socket;
	if (!inet_ioctl_is_query(command) &&
	    !inet_ioctl_caller_is_superuser())
		return EPERM;
	if (is_route_request(command))
		return route_ioctl(command, argument);
	if (command == SIOCGIFCONF)
		return inet_ioctl_ifconf(argument);
	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	request.ifr_name[IFNAMSIZ - 1U] = '\0';
	if (command == SIOCGIFNAME) {
		device =
		    net_device_find_by_index_ref((unsigned)request.ifr_ifindex);
		if (device == NULL)
			return ENODEV;
		memset(request.ifr_name, 0, sizeof(request.ifr_name));
		memcpy(request.ifr_name, device->name,
		       strnlen(device->name, sizeof(request.ifr_name) - 1U));
		error = copyout(&request, argument, sizeof(request));
		net_device_release(device);
		return error;
	}
	device = net_device_find_ref(request.ifr_name);
	if (device == NULL)
		return ENODEV;
	error = interface_ensure(device);
	if (error != 0) {
		net_device_release(device);
		return error;
	}
	if (!net_device_is_live(device)) {
		net_device_release(device);
		return ENODEV;
	}
	switch (command) {
	case SIOCGIFINDEX:
		request.ifr_ifindex = (int)device->ifindex;
		break;
	case SIOCGIFFLAGS:
		request.ifr_flags = (int)device_flags(device);
		break;
	case SIOCSIFFLAGS:
		if ((request.ifr_flags & IFF_UP) != 0 &&
		    !(net_device_flags_get(device) & NET_DEVICE_UP)) {
			error = net_device_open(device);
			if (error != 0) {
				net_device_release(device);
				return error;
			}
		} else if ((request.ifr_flags & IFF_UP) == 0 &&
			   (net_device_flags_get(device) & NET_DEVICE_UP)) {
			net_device_close(device);
		}
		net_device_release(device);
		return 0;
	case SIOCGIFHWADDR:
		memset(request.ifr_hwaddr, 0, sizeof(request.ifr_hwaddr));
		memcpy(request.ifr_hwaddr, device->hwaddr, device->hwaddr_len);
		break;
	case SIOCGIFMTU:
		request.ifr_mtu = (int)device->mtu;
		break;
	case SIOCGIFSTATS:
		memset(&request.ifr_data, 0, sizeof(request.ifr_data));
		request.ifr_data.ifi_mtu = device->mtu;
		request.ifr_data.ifi_ipackets = device->rx_packets;
		request.ifr_data.ifi_ibytes = device->rx_bytes;
		request.ifr_data.ifi_ierrors = device->rx_errors;
		request.ifr_data.ifi_iqdrops = device->rx_dropped;
		request.ifr_data.ifi_opackets = device->tx_packets;
		request.ifr_data.ifi_obytes = device->tx_bytes;
		request.ifr_data.ifi_oerrors = device->tx_errors;
		request.ifr_data.ifi_oqdrops = device->tx_dropped;
		break;
	case SIOCGIFADDR:
		error = inet_interface_configuration(device, &address, NULL, NULL);
		if (error != 0) {
			net_device_release(device);
			return ENODEV;
		}
		set_ifreq_address(&request, address);
		break;
	case SIOCGIFNETMASK:
		error = inet_interface_configuration(device, NULL, &netmask, NULL);
		if (error != 0) {
			net_device_release(device);
			return ENODEV;
		}
		set_ifreq_address(&request, netmask);
		break;
	case SIOCGIFBRDADDR:
		error = inet_interface_configuration(device, NULL, NULL,
					     &broadcast);
		if (error != 0) {
			net_device_release(device);
			return ENODEV;
		}
		set_ifreq_address(&request, broadcast);
		break;
	case SIOCSIFADDR:
	case SIOCSIFNETMASK:
	case SIOCSIFBRDADDR: {
		const struct sockaddr_in *input =
		    (const struct sockaddr_in *)&request.ifr_addr;
		uint32_t value;
		if (input->sin_family != AF_INET) {
			net_device_release(device);
			return EAFNOSUPPORT;
		}
		value = net_ntohl(input->sin_addr.s_addr);
		enabled = interface_lock();
		interface = interface_for_device_locked(device);
		if (interface == NULL || !net_device_is_live(device)) {
			interface_unlock(enabled);
			net_device_release(device);
			return ENODEV;
		}
		old_address = interface->address;
		old_netmask = interface->netmask;
		if (command == SIOCSIFADDR)
			interface->address = value;
		if (command == SIOCSIFNETMASK)
			interface->netmask = value;
		if (command == SIOCSIFBRDADDR)
			interface->broadcast = value;
		if (interface->address != 0 && interface->netmask != 0 &&
		    interface->broadcast == 0)
			interface->broadcast =
			    interface->address | ~interface->netmask;
		address = interface->address;
		netmask = interface->netmask;
		interface_unlock(enabled);
		interface_update_route(device, old_address, old_netmask, address,
				       netmask);
		net_device_release(device);
		return 0;
	}
	default:
		net_device_release(device);
		return EOPNOTSUPP;
	}
	error = copyout(&request, argument, sizeof(request));
	net_device_release(device);
	return error;
}

static int
inet_create(int type, int protocol, struct socket **result)
{
	if (type == SOCK_RAW)
		return icmp_socket_create(protocol, result);
	if (type == SOCK_DGRAM)
		return udp_socket_create(protocol, result);
	if (type == SOCK_STREAM)
		return tcp_socket_create(protocol, result);
	return EPROTONOSUPPORT;
}

static const struct socket_family_ops inet_family = {.create = inet_create};

int
inet_socket_init(void)
{
	struct net_device *references[NET_DEVICE_MAX];
	bool enabled;
	unsigned count = 0, index;

	enabled = interface_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++)
		if (interfaces[index].device != NULL)
			references[count++] = interfaces[index].device;
	memset(interfaces, 0, sizeof(interfaces));
	interface_unlock(enabled);
	for (index = 0; index < count; index++)
		net_device_release(references[index]);
	return socket_family_register(AF_INET, &inet_family);
}

void
inet_interface_purge_device(struct net_device *device)
{
	struct net_device *references[NET_DEVICE_MAX];
	bool enabled;
	unsigned count = 0, index;

	if (device == NULL)
		return;
	enabled = interface_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++)
		if (interfaces[index].device == device) {
			references[count++] = interfaces[index].device;
			memset(&interfaces[index], 0,
			       sizeof(interfaces[index]));
		}
	interface_unlock(enabled);
	for (index = 0; index < count; index++)
		net_device_release(references[index]);
}
