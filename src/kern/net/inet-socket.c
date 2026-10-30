/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv4 sockets and interfaces.
 *
 * Every network device that has been configured through the interface
 * ioctls owns an inet interface record with its address, netmask, and
 * broadcast address.  The internet socket layer shared by UDP, TCP, and
 * ICMP binds and connects addresses, answers the name queries, and
 * implements the interface, route, and WLAN ioctls of AF_INET sockets.
 */

#include "kern/net/inet-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/route.h"
#include "kern/atomic.h"
#include "kern/cred.h"
#include "kern/uaccess.h"
#include "internal.h"
#include "ipv6.h"
#include <kern/kcrt.h>

#include <uapi/netif.h>
#include <uapi/netinet.h>
#include <uapi/route.h>
#include <uapi/wlan.h>
#include <uapi/errno.h>
#include <stdbool.h>
#include <hal/hal.h>

#define INET_IOCTL_DIRECTION_MASK KERN_IOC_INOUT
#define INET_IOCTL_SIZE_MASK (0x1fffUL << 16)
#define INET_IOCTL_GROUP_MASK (0xffUL << 8)
#define INET_IOCTL_NUMBER_MASK 0xffUL
/* How two sockets' local addresses of one family overlap: not, one is the wildcard, or the same. */
#define INET_OVERLAP_NONE	0
#define INET_OVERLAP_WILDCARD	1
#define INET_OVERLAP_SAME	2

#define INET_IOCTL_ENCODING_MASK                                               \
	(INET_IOCTL_DIRECTION_MASK | INET_IOCTL_SIZE_MASK |                     \
	 INET_IOCTL_GROUP_MASK | INET_IOCTL_NUMBER_MASK)

struct inet_interface {
	struct net_device *device;
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
};

union inet_wlan_ioctl_request {
	struct wlan_ioctl_header header;
	struct wlan_scan_request scan;
	struct wlan_scan_status_request scan_status;
	struct wlan_bss_request bss;
	struct wlan_connect_request connect;
	struct wlan_disconnect_request disconnect;
	struct wlan_status_request status;
};

static struct inet_interface interfaces[NET_DEVICE_MAX];
static atomic_uint_t interface_guard;

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

static bool interface_lock(void);
static void interface_unlock(bool enabled);
static struct inet_interface *interface_for_device_locked(struct net_device *device);
static int interface_ensure(struct net_device *device);
static void interface_update_route(struct net_device *device, uint32_t old_address, uint32_t old_netmask, uint32_t address, uint32_t netmask);
static int inet_socket_name(struct inet_socket *inet, struct sockaddr *address, socklen_t *length, int peer);
static unsigned device_flags(const struct net_device *device);
static void set_ifreq_address(struct ifreq *request, uint32_t address);
static int inet_ioctl_ifconf(uintptr_t argument);
static int is_route_request(unsigned long command);
static bool inet_ioctl_is_query(unsigned long command);
static bool inet_ioctl_caller_is_superuser(void);
static bool inet_ioctl_is_wlan_group(unsigned long command);
static void inet_ioctl_scrub(void *buffer, size_t size);
static int inet_ioctl_wlan_classify(unsigned long command, size_t *size, bool *query);
static int inet_ioctl_wlan_validate_name(const struct wlan_ioctl_header *header);
static int inet_ioctl_wlan_validate(const struct wlan_ioctl_header *header, size_t size);
static int inet_ioctl_wlan(unsigned long command, uintptr_t argument);
static int inet_create(int type, int protocol, struct socket **result);
static int inet6_create(int type, int protocol, struct socket **result);
static int inet6_socket_bind(struct inet_socket *inet, const struct sockaddr *address, socklen_t length);
static int inet6_socket_connect(struct inet_socket *inet, const struct sockaddr *address, socklen_t length);
static int inet6_socket_name(struct inet_socket *inet, struct sockaddr *address, socklen_t *length, int peer);
static int inet_ipv4_address_local(uint32_t local, unsigned *ifindex);
static int inet_overlap_ipv4(const struct inet_socket *existing, const struct inet_socket *candidate);
static int inet_overlap_ipv6(const struct inet_socket *existing, const struct inet_socket *candidate);
static void inet6_mapped(struct in6_addr *out, uint32_t address);

static const struct socket_family_ops inet_family = {.create = inet_create};

/* The family of AF_INET6 (ws130-p003): the same sockets, made IPv6 ones. */
static const struct socket_family_ops inet6_family = {.create = inet6_create};

/*
 * Reads the configured addresses of a device, even when unset.
 *
 * A device without an interface record, or one that is no longer live,
 * reports EADDRNOTAVAIL.
 */
int
inet_interface_configuration(
	struct net_device *device,
	uint32_t *address,
	uint32_t *netmask,
	uint32_t *broadcast)
{
	struct inet_interface *interface;
	bool enabled;
	int error;

	error = 0;

	/* A missing device has no addresses. */
	if (device == NULL)
		return EADDRNOTAVAIL;

	/* Copies the requested fields under the interface lock. */
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

	/* Reports why the lookup failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads the addresses of a device that has an address configured.
 */
int
inet_interface_address(
	struct net_device *device,
	uint32_t *address,
	uint32_t *netmask,
	uint32_t *broadcast)
{
	uint32_t configured;
	int error;

	/* Reads the configuration, which may lack an address. */
	error = inet_interface_configuration(device, &configured, netmask,
					     broadcast);
	if (error != 0)
		return error;
	if (configured == 0)
		return EADDRNOTAVAIL;
	if (address != NULL)
		*address = configured;

	/* Reports the configured address. */
	return 0;
}

/*
 * Tells whether an address is configured on a live interface of this host.
 *
 * A packet for such an address is delivered here, round the loopback
 * device, rather than sent out of the interface that carries the address.
 */
int
inet_address_is_local(
	uint32_t address)
{
	struct net_device *device;
	bool enabled;
	unsigned index;
	int live;
	int local;

	/* No interface carries the address until one is found. */
	local = 0;

	/* The unspecified address belongs to no interface. */
	if (address == INADDR_ANY)
		return 0;

	/* Looks for a live interface that carries the address. */
	enabled = interface_lock();

	/* Scans every interface slot; the lock keeps the table still. */
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		device = interfaces[index].device;

		/* Skips an empty slot and an interface with another address. */
		if (device == NULL)
			continue;
		if (interfaces[index].address != address)
			continue;

		/* An interface that is going away no longer owns its address. */
		live = net_device_is_live(device);
		if (!live)
			continue;

		/* Found: the address is this host's own. */
		local = 1;
		break;
	}

	/* Lets the interface table change again. */
	interface_unlock(enabled);

	/* Succeeded: reports whether the address is this host's own. */
	return local;
}

/*
 * Initializes the internet part of a socket object.
 */
void
inet_socket_object_init(
	struct inet_socket *inet,
	int type,
	int protocol,
	const struct socket_ops *ops)
{
	kern_memset(inet, 0, sizeof(*inet));
	socket_init_object(&inet->socket, AF_INET, type, protocol, ops);
	inet->family = AF_INET;
}

/*
 * Binds a socket to a local address and port.
 *
 * A specific address must belong to a live interface, which then also
 * binds the socket to that interface.
 */
int
inet_socket_bind(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	const struct sockaddr_in *input;
	uint32_t local;
	unsigned ifindex;
	int error;

	input = (const struct sockaddr_in *)address;
	ifindex = 0;

	/* An IPv6 socket takes an IPv6 address (ws130-p003). */
	if (inet != NULL && inet->family == AF_INET6) {
		error = inet6_socket_bind(inet, address, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Rejects a missing socket or anything but a complete AF_INET address. */
	if (inet == NULL ||
	    address == NULL ||
	    length < sizeof(*input) ||
	    input->sin_family != AF_INET)
		return EINVAL;

	/* A specific address must be configured on a live interface. */
	local = net_ntohl(input->sin_addr.s_addr);
	if (local != INADDR_ANY) {
		error = inet_ipv4_address_local(local, &ifindex);
		if (error != 0)
			return error;
		inet->ifindex = ifindex;
	}

	/* Records the local endpoint. */
	inet->local_address = local;
	inet->local_port = net_ntohs(input->sin_port);
	inet->inet_flags |= INET_SOCKET_BOUND;

	/* Reports the bound socket. */
	return 0;
}

/*
 * Records the remote address and port of a socket.
 */
int
inet_socket_connect(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	const struct sockaddr_in *input;
	int error;

	/* An IPv6 socket takes an IPv6 address (ws130-p003). */
	if (inet != NULL && inet->family == AF_INET6) {
		error = inet6_socket_connect(inet, address, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Rejects a missing socket or anything but a complete AF_INET address. */
	input = (const struct sockaddr_in *)address;
	if (inet == NULL ||
	    address == NULL ||
	    length < sizeof(*input) ||
	    input->sin_family != AF_INET)
		return EINVAL;

	/* The remote address must be specific. */
	inet->remote_address = net_ntohl(input->sin_addr.s_addr);
	inet->remote_port = net_ntohs(input->sin_port);
	if (inet->remote_address == INADDR_ANY)
		return EADDRNOTAVAIL;
	inet->inet_flags |= INET_SOCKET_CONNECTED;

	/* Reports the connected socket. */
	return 0;
}

/*
 * Tests whether two sockets' local endpoints conflict.
 *
 * Two sockets conflict when they share a port and their addresses are
 * equal or one is the wildcard; SO_REUSEADDR on both sides lifts the
 * wildcard case only.
 */
int
inet_socket_local_conflict(
	const struct inet_socket *existing,
	unsigned existing_reuse,
	const struct inet_socket *candidate,
	unsigned candidate_reuse)
{
	int overlap;
	int overlap6;

	/* Different or unbound ports never conflict. */
	if (existing == NULL ||
	    candidate == NULL ||
	    existing->local_port == 0 ||
	    candidate->local_port == 0 ||
	    existing->local_port != candidate->local_port)
		return 0;

	/* How the addresses both take overlap: the IPv4 ones, and the IPv6 ones (ws130-p003). */
	overlap = inet_overlap_ipv4(existing, candidate);
	overlap6 = inet_overlap_ipv6(existing, candidate);
	if (overlap6 > overlap)
		overlap = overlap6;

	/* Addresses in common or none. */
	if (overlap == INET_OVERLAP_NONE)
		return 0;
	if (overlap == INET_OVERLAP_SAME)
		return 1;

	/*
	 * SO_REUSEADDR permits wildcard/specific coexistence only when every
	 * participant opted in.  Exact duplicate local endpoints still
	 * require a future SO_REUSEPORT and are therefore rejected.
	 */
	if (existing_reuse == 0)
		return 1;
	if (candidate_reuse == 0)
		return 1;

	/* Reports permitted coexistence. */
	return 0;
}

/*
 * Reports the local address of a socket.
 */
int
inet_socket_getsockname(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length)
{
	int error;

	/* Reports why the lookup failed. */
	error = inet_socket_name(inet, address, length, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the remote address of a connected socket.
 */
int
inet_socket_getpeername(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length)
{
	int error;

	/* Reports why the lookup failed. */
	error = inet_socket_name(inet, address, length, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets SO_BINDTODEVICE, the only socket option handled at this layer.
 */
int
inet_socket_setsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	char name[IFNAMSIZ];
	struct net_device *device;
	int enabled;

	/* IPV6_V6ONLY of an IPv6 socket (ws130-p003), only before it is bound. */
	if (inet != NULL && level == IPPROTO_IPV6 && option == IPV6_V6ONLY) {
		if (inet->family != AF_INET6)
			return ENOPROTOOPT;
		if (value == NULL || length != sizeof(enabled))
			return EINVAL;
		if ((inet->inet_flags & (INET_SOCKET_BOUND | INET_SOCKET_CONNECTED)) != 0)
			return EINVAL;
		kern_memcpy(&enabled, value, sizeof(enabled));
		inet->v6only = enabled != 0;
		return 0;
	}

	/* Only SO_BINDTODEVICE is handled here. */
	if (inet == NULL || level != SOL_SOCKET || option != SO_BINDTODEVICE)
		return EOPNOTSUPP;

	/* Takes a terminated interface name. */
	if (value == NULL || length == 0 || length > sizeof(name))
		return EINVAL;
	kern_memset(name, 0, sizeof(name));
	kern_memcpy(name, value, length);
	if (name[length - 1U] != '\0')
		return EINVAL;

	/* An empty name unbinds the socket. */
	if (name[0] == '\0') {
		inet->ifindex = 0;
		return 0;
	}

	/* Binds to the named device. */
	device = net_device_find_ref(name);
	if (device == NULL)
		return ENODEV;
	inet->ifindex = device->ifindex;
	net_device_release(device);

	/* Reports the bound device. */
	return 0;
}

/*
 * Reads SO_BINDTODEVICE, the only socket option handled at this layer.
 */
int
inet_socket_getsockopt(
	struct inet_socket *inet,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct net_device *device;
	size_t required;
	int enabled;

	/* IPV6_V6ONLY of an IPv6 socket (ws130-p003). */
	if (inet != NULL && level == IPPROTO_IPV6 && option == IPV6_V6ONLY) {
		if (inet->family != AF_INET6)
			return ENOPROTOOPT;
		if (value == NULL || length == NULL || *length < sizeof(enabled))
			return EINVAL;
		enabled = (int)inet->v6only;
		kern_memcpy(value, &enabled, sizeof(enabled));
		*length = sizeof(enabled);
		return 0;
	}

	/* Only SO_BINDTODEVICE is handled here. */
	if (inet == NULL || level != SOL_SOCKET || option != SO_BINDTODEVICE)
		return EOPNOTSUPP;
	if (value == NULL || length == NULL)
		return EINVAL;

	/* An unbound socket reports an empty name. */
	device = net_device_find_by_index_ref(inet->ifindex);
	if (device != NULL)
		required = kern_strlen(device->name) + 1U;
	else
		required = 1U;
	if (*length < required) {
		net_device_release(device);
		return EINVAL;
	}

	/* Copies the terminated name. */
	kern_memset(value, 0, required);
	if (device != NULL)
		kern_memcpy(value, device->name, required);
	net_device_release(device);
	*length = (socklen_t)required;

	/* Reports the read name. */
	return 0;
}

/*
 * Handles the interface, route, and WLAN ioctls of an AF_INET socket.
 *
 * Queries are open to everyone; anything that changes state needs the
 * superuser.  The address ioctls also maintain the connected route.
 */
int
inet_socket_ioctl(
	struct socket *socket,
	unsigned long command,
	uintptr_t argument)
{
	struct ifreq request;
	struct net_device *device;
	struct inet_interface *interface;
	const struct sockaddr_in *input;
	bool enabled;
	uint32_t address;
	uint32_t broadcast;
	uint32_t netmask;
	uint32_t old_address;
	uint32_t old_netmask;
	uint32_t value;
	bool superuser;
	int ipv6_command;
	int query;
	int error;

	(void)socket;

	/* The WLAN group has its own dispatcher and privilege check. */
	if (inet_ioctl_is_wlan_group(command)) {
		error = inet_ioctl_wlan(command, argument);
		return error;
	}

	/* IPv6's ioctls (ws130-p002): reading is open to everyone, a change needs the superuser. */
	ipv6_command = ipv6_ioctl_handles(command);
	if (ipv6_command) {
		query = ipv6_ioctl_is_query(command);
		if (!query) {
			superuser = inet_ioctl_caller_is_superuser();
			if (!superuser)
				return EPERM;
		}

		/* Carried out. */
		error = ipv6_ioctl(command, argument);
		return error;
	}

	/* Everything but a query needs the superuser. */
	if (!inet_ioctl_is_query(command) &&
	    !inet_ioctl_caller_is_superuser())
		return EPERM;

	/* Routes and the interface list are handled elsewhere. */
	if (is_route_request(command)) {
		error = route_ioctl(command, argument);
		return error;
	}

	if (command == SIOCGIFCONF) {
		error = inet_ioctl_ifconf(argument);
		return error;
	}

	/* Reads the interface request. */
	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	request.ifr_name[IFNAMSIZ - 1U] = '\0';

	/* SIOCGIFNAME looks the device up by index instead of name. */
	if (command == SIOCGIFNAME) {
		device =
		    net_device_find_by_index_ref((unsigned)request.ifr_ifindex);
		if (device == NULL)
			return ENODEV;
		kern_memset(request.ifr_name, 0, sizeof(request.ifr_name));
		kern_memcpy(request.ifr_name, device->name,
		       kern_strnlen(device->name, sizeof(request.ifr_name) - 1U));
		error = copyout(&request, argument, sizeof(request));
		net_device_release(device);
		return error;
	}

	/* Finds the live device and gives it an interface record. */
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

	/* Handles the request; queries fall through to the copy out. */
	switch (command) {
	case SIOCGIFINDEX:
		request.ifr_ifindex = (int)device->ifindex;
		break;
	case SIOCGIFFLAGS:
		request.ifr_flags = (int)device_flags(device);
		break;
	case SIOCSIFFLAGS:
		/* Only the up flag can be changed: it opens or closes the device. */
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
		kern_memset(request.ifr_hwaddr, 0, sizeof(request.ifr_hwaddr));
		kern_memcpy(request.ifr_hwaddr, device->hwaddr, device->hwaddr_len);
		break;
	case SIOCGIFMTU:
		request.ifr_mtu = (int)device->mtu;
		break;
	case SIOCSIFMTU:
		/* A new MTU for the device, 0 for its own, then IPv6's link MTU follows it. */
		error = EINVAL;
		if (request.ifr_mtu >= 0)
			error = net_device_set_mtu(device, (unsigned)request.ifr_mtu);
		if (error == 0)
			ipv6_device_mtu_changed(device);
		net_device_release(device);
		return error;
	case SIOCGIFSTATS:
		kern_memset(&request.ifr_data, 0, sizeof(request.ifr_data));
		request.ifr_data.ifi_mtu = device->mtu;
		request.ifr_data.ifi_link_mbps = device->link_mbps;
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
	case SIOCSIFBRDADDR:
		/* Takes the new AF_INET address. */
		input = (const struct sockaddr_in *)&request.ifr_addr;
		if (input->sin_family != AF_INET) {
			net_device_release(device);
			return EAFNOSUPPORT;
		}

		value = net_ntohl(input->sin_addr.s_addr);

		/* Updates the interface record under the lock. */
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

		/* Derives the broadcast address once both address and mask are set. */
		if (interface->address != 0 &&
		    interface->netmask != 0 &&
		    interface->broadcast == 0)
			interface->broadcast =
			    interface->address | ~interface->netmask;
		address = interface->address;
		netmask = interface->netmask;
		interface_unlock(enabled);

		/* Replaces the connected route; what senders kept of the old address is looked up again. */
		interface_update_route(device, old_address, old_netmask, address,
				       netmask);
		route_changed();
		net_device_release(device);
		return 0;
	default:
		net_device_release(device);
		return EOPNOTSUPP;
	}

	/* Copies the answered query back. */
	error = copyout(&request, argument, sizeof(request));
	net_device_release(device);

	/* Reports why the copy failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Clears the interface table and registers AF_INET.
 */
int
inet_socket_init(
	void)
{
	struct net_device *references[NET_DEVICE_MAX];
	bool enabled;
	unsigned count;
	unsigned index;
	int error;

	count = 0;

	/* Drops the interfaces of a previous init outside the lock. */
	enabled = interface_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (interfaces[index].device != NULL)
			references[count++] = interfaces[index].device;
	}

	kern_memset(interfaces, 0, sizeof(interfaces));
	interface_unlock(enabled);
	for (index = 0; index < count; index++)
		net_device_release(references[index]);

	/* Registers the family. */

	/* Reports why the registration failed. */
	error = socket_family_register(AF_INET, &inet_family);
	if (error != 0)
		return error;

	/* And IPv6's (ws130-p003). */
	error = socket_family_register(AF_INET6, &inet6_family);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Drops the interface record of a device that is going away.
 */
void
inet_interface_purge_device(
	struct net_device *device)
{
	struct net_device *references[NET_DEVICE_MAX];
	bool enabled;
	unsigned count;
	unsigned index;

	count = 0;

	/* Ignores a missing device. */
	if (device == NULL)
		return;

	/* Clears the device's records and drops their references outside the lock. */
	enabled = interface_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (interfaces[index].device == device) {
			references[count++] = interfaces[index].device;
			kern_memset(&interfaces[index], 0,
			       sizeof(interfaces[index]));
		}
	}

	interface_unlock(enabled);

	/* What senders kept of the device's address is looked up again. */
	route_changed();
	for (index = 0; index < count; index++)
		net_device_release(references[index]);
}

/*
 * Tells whether a socket takes IPv4.
 *
 * An AF_INET socket does; an AF_INET6 one (ws130-p003) does when it is
 * not IPV6_V6ONLY and is IPv4-mapped, or still on [::] with no IPv6 peer.
 */
int
inet_socket_accepts_ipv4(
	const struct inet_socket *inet)
{
	int unspecified;

	/* The first family's sockets do. */
	if (inet->family == AF_INET)
		return 1;

	/* An IPv6-only socket does not; an IPv4-mapped one does. */
	if (inet->v6only)
		return 0;
	if (inet->mapped)
		return 1;

	/* One on an IPv6 address, or with an IPv6 peer, does not. */
	unspecified = in6_is_unspecified(&inet->local6);
	if (!unspecified)
		return 0;
	if ((inet->inet_flags & INET_SOCKET_CONNECTED) != 0)
		return 0;

	/* Succeeded: one on [::] does. */
	return 1;
}

/*
 * Tells whether a socket takes IPv6: an AF_INET6 one that is not
 * IPv4-mapped.
 */
int
inet_socket_accepts_ipv6(
	const struct inet_socket *inet)
{
	/* The first family's sockets and the IPv4-mapped ones do not. */
	if (inet->family != AF_INET6)
		return 0;
	if (inet->mapped)
		return 0;

	/* Succeeded: an IPv6 one. */
	return 1;
}

/*
 * Tells whether a socket speaks IPv6 to a peer: one that takes IPv6 and
 * is connected.
 */
int
inet_socket_speaks_ipv6(
	const struct inet_socket *inet)
{
	int accepts;

	/* Not one of IPv4. */
	accepts = inet_socket_accepts_ipv6(inet);
	if (!accepts)
		return 0;

	/* Not one without a peer. */
	if ((inet->inet_flags & INET_SOCKET_CONNECTED) == 0)
		return 0;

	/* Succeeded: an IPv6 one with a peer. */
	return 1;
}

/*
 * Names a peer in the socket's own family.
 *
 * An AF_INET socket's is the IPv4 address (host order; address6 is NULL).
 * An AF_INET6 socket's is the IPv6 address address6 with the scope of a
 * link-local one, or the IPv4 address IPv4-mapped when address6 is NULL.
 * output holds the name (a packet's source_address).
 */
void
inet_socket_peer_name(
	const struct inet_socket *inet,
	uint32_t address,
	const struct in6_addr *address6,
	unsigned scope,
	uint16_t port,
	uint8_t *output,
	uint8_t *length)
{
	struct sockaddr_in name4;
	struct sockaddr_in6 name6;
	int linklocal;

	/* The first family's name. */
	if (inet->family == AF_INET) {
		kern_memset(&name4, 0, sizeof(name4));
		name4.sin_family = AF_INET;
		name4.sin_port = net_htons(port);
		name4.sin_addr.s_addr = net_htonl(address);
		kern_memcpy(output, &name4, sizeof(name4));
		*length = (uint8_t)sizeof(name4);
		return;
	}

	/* The second's: the address, or the IPv4 one mapped. */
	kern_memset(&name6, 0, sizeof(name6));
	name6.sin6_family = AF_INET6;
	name6.sin6_port = net_htons(port);
	if (address6 != NULL)
		name6.sin6_addr = *address6;
	else
		inet6_mapped(&name6.sin6_addr, address);

	/* A link-local peer is named with its interface. */
	linklocal = in6_is_linklocal(&name6.sin6_addr);
	if (linklocal)
		name6.sin6_scope_id = scope;

	/* The name. */
	kern_memcpy(output, &name6, sizeof(name6));
	*length = (uint8_t)sizeof(name6);
}

/*
 * Gives the IPv4 address (host order) an IPv4-mapped IPv6 address carries.
 */
uint32_t
inet_socket_unmapped(
	const struct in6_addr *address)
{
	uint32_t value;

	/* The last four bytes, in host order. */
	value = (uint32_t)address->s6_addr[12] << 24;
	value |= (uint32_t)address->s6_addr[13] << 16;
	value |= (uint32_t)address->s6_addr[14] << 8;
	value |= (uint32_t)address->s6_addr[15];

	/* Succeeded: the IPv4 address. */
	return value;
}

/* Disables interrupts, when the HAL is present, and takes the interface lock. */
static bool
interface_lock(
	void)
{
	bool enabled;

	/* Without the HAL there are no interrupts to disable. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&interface_guard))
		__asm__ volatile("" ::: "memory");

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the interface lock and restores the interrupt state. */
static void
interface_unlock(
	bool enabled)
{
	atomic_store_release(&interface_guard, 0);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Finds the interface record of a device; the caller holds the lock. */
static struct inet_interface *
interface_for_device_locked(
	struct net_device *device)
{
	unsigned index;

	/* Searches the table. */
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (interfaces[index].device == device)
			return &interfaces[index];
	}

	/* Reports a device without a record. */
	return NULL;
}

/* Gives a live device an interface record, referencing the device. */
static int
interface_ensure(
	struct net_device *device)
{
	struct net_device *release_device;
	struct inet_interface *interface;
	bool enabled;
	unsigned index;
	unsigned free_index;
	int error;

	release_device = NULL;
	free_index = NET_DEVICE_MAX;
	error = 0;

	/* Rejects a missing device. */
	if (device == NULL)
		return ENODEV;

	/* A device that already has a record only needs to be live. */
	enabled = interface_lock();
	interface = interface_for_device_locked(device);
	if (interface != NULL) {
		if (net_device_is_live(device))
			error = 0;
		else
			error = ENODEV;
		goto out;
	}

	/* Takes the first free slot and a live reference on the device. */
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (interfaces[index].device == NULL) {
			free_index = index;
			break;
		}
	}

	if (free_index == NET_DEVICE_MAX) {
		error = ENOSPC;
		goto out;
	}

	if (!net_device_ref_live(device)) {
		error = ENODEV;
		goto out;
	}

	/*
	 * Clears the complete slot before publishing its new identity so
	 * values left by a removed interface cannot alias a reconnected
	 * device.
	 */
	kern_memset(&interfaces[free_index], 0, sizeof(interfaces[free_index]));
	interfaces[free_index].device = device;

	/* A device that died meanwhile gets its slot and reference back. */
	if (!net_device_is_live(device)) {
		release_device = interfaces[free_index].device;
		kern_memset(&interfaces[free_index], 0,
		       sizeof(interfaces[free_index]));
		error = ENODEV;
	}

out:
	interface_unlock(enabled);
	if (release_device != NULL)
		net_device_release(release_device);

	/* Reports whether the device has a record. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Replaces the connected route of an interface after an address change. */
static void
interface_update_route(
	struct net_device *device,
	uint32_t old_address,
	uint32_t old_netmask,
	uint32_t address,
	uint32_t netmask)
{
	/* Drops the old subnet route and adds the new one. */
	if (old_address != 0 && old_netmask != 0)
		(void)route_delete(old_address & old_netmask, old_netmask,
				   device);
	if (address != 0 && netmask != 0)
		(void)route_add_flags(address & netmask, netmask, 0, device,
				      RTF_UP | RTF_CONNECTED);
}

/* Copies the local or the remote endpoint into a socket address. */
static int
inet_socket_name(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length,
	int peer)
{
	struct sockaddr_in output;
	socklen_t copied;
	int error;

	/* Rejects a missing operand. */
	if (inet == NULL || address == NULL || length == NULL)
		return EINVAL;

	/* The remote endpoint exists only on a connected socket. */
	if (peer && !(inet->inet_flags & INET_SOCKET_CONNECTED))
		return ENOTCONN;

	/* An IPv6 socket names itself with an IPv6 address (ws130-p003). */
	if (inet->family == AF_INET6) {
		error = inet6_socket_name(inet, address, length, peer);
		if (error != 0)
			return error;
		return 0;
	}

	/* Builds the address in network order. */
	kern_memset(&output, 0, sizeof(output));
	output.sin_family = AF_INET;
	if (peer) {
		output.sin_addr.s_addr = net_htonl(inet->remote_address);
		output.sin_port = net_htons(inet->remote_port);
	} else {
		output.sin_addr.s_addr = net_htonl(inet->local_address);
		output.sin_port = net_htons(inet->local_port);
	}

	/* Copies as much as fits and reports the full length. */
	if (*length < sizeof(output))
		copied = *length;
	else
		copied = sizeof(output);
	kern_memcpy(address, &output, copied);
	*length = sizeof(output);

	/* Reports the copied address. */
	return 0;
}

/* Converts device state flags to interface flags. */
static unsigned
device_flags(
	const struct net_device *device)
{
	unsigned device_state;
	unsigned flags;

	/* Maps each device flag onto its interface flag. */
	device_state = net_device_flags_get(device);
	flags = 0;
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

	/* Reports the interface flags. */
	return flags;
}

/* Stores an IPv4 address in the address field of an interface request. */
static void
set_ifreq_address(
	struct ifreq *request,
	uint32_t address)
{
	struct sockaddr_in *output;

	output = (struct sockaddr_in *)&request->ifr_addr;
	kern_memset(&request->ifr_addr, 0, sizeof(request->ifr_addr));
	output->sin_family = AF_INET;
	output->sin_addr.s_addr = net_htonl(address);
}

/* Lists the interfaces for SIOCGIFCONF, or sizes the list without a buffer. */
static int
inet_ioctl_ifconf(
	uintptr_t argument)
{
	struct ifconf configuration;
	struct ifreq request;
	struct net_device *device;
	uint32_t required;
	uint32_t capacity;
	uint32_t copied;
	unsigned index;
	int error;

	copied = 0;

	/* Reads and validates the request. */
	if (argument == 0)
		return EFAULT;
	error = copyin(argument, &configuration, sizeof(configuration));
	if (error != 0)
		return error;
	if (configuration.ifc_reserved != 0 ||
	    (configuration.ifc_len % sizeof(struct ifreq)) != 0)
		return EINVAL;

	/* Without a buffer only the required size is reported. */
	required = net_device_count() * (uint32_t)sizeof(struct ifreq);
	if (configuration.ifc_buf == 0 || configuration.ifc_len == 0) {
		configuration.ifc_len = required;
		error = copyout(&configuration, argument, sizeof(configuration));
		return error;
	}

	/* Copies one record per device until the buffer is full. */
	capacity = configuration.ifc_len / (uint32_t)sizeof(struct ifreq);
	for (index = 0; index < capacity; index++) {
		device = net_device_at_ref(index);
		if (device == NULL)
			break;
		kern_memset(&request, 0, sizeof(request));
		kern_memcpy(request.ifr_name, device->name,
		       kern_strnlen(device->name, sizeof(request.ifr_name) - 1U));
		request.ifr_ifindex = (int)device->ifindex;
		error = copyout(&request, (uintptr_t)configuration.ifc_buf + copied,
			    sizeof(request));
		net_device_release(device);
		if (error != 0)
			return error;
		copied += sizeof(request);
	}

	/* Reports the bytes copied. */
	configuration.ifc_len = copied;

	/* Reports why the copy failed. */
	error = copyout(&configuration, argument, sizeof(configuration));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tests whether a command is one of the route ioctls. */
static int
is_route_request(
	unsigned long command)
{
	/* The route ioctls add, delete, and enumerate entries. */
	if (command == SIOCADDRT)
		return 1;
	if (command == SIOCDELRT)
		return 1;
	if (command == SIOCGRTENTRY)
		return 1;

	/* Reports another command. */
	return 0;
}

/* Lists the ioctls an unprivileged caller may issue. */
static bool
inet_ioctl_is_query(
	unsigned long command)
{
	/*
	 * Keeps the unprivileged surface an explicit allow-list.  In
	 * particular, a future driver-private ioctl must not become writable
	 * merely because its command was not known when this common
	 * dispatcher was written.
	 */
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

/* Tests whether the calling process runs as the superuser. */
static bool
inet_ioctl_caller_is_superuser(
	void)
{
	struct ucred *credential;
	bool permitted;

	/* Samples the caller's credential. */
	credential = cred_current_ref();
	permitted = cred_is_superuser(credential);
	cred_release(credential);

	/* Reports the privilege. */
	return permitted;
}

/* Tests whether a command belongs to the WLAN ioctl group. */
static bool
inet_ioctl_is_wlan_group(
	unsigned long command)
{
	/* The group is encoded in the command's group byte. */
	if ((command & INET_IOCTL_GROUP_MASK) !=
	    ((unsigned long)KERN_WLAN_IOCTL_GROUP << 8))
		return false;
	return true;
}

/* Overwrites a buffer that held secrets in a way the compiler keeps. */
static void
inet_ioctl_scrub(
	void *buffer,
	size_t size)
{
	volatile unsigned char *byte;

	/* Clears through a volatile pointer so the stores are not elided. */
	byte = buffer;
	while (size-- != 0)
		*byte++ = 0;
}

/* Identifies a WLAN ioctl, its request size, and whether it is a query. */
static int
inet_ioctl_wlan_classify(
	unsigned long command,
	size_t *size,
	bool *query)
{
	unsigned long expected;
	unsigned long encoded_size;
	unsigned number;

	/* Rejects a malformed encoding or a command outside the group. */
	if (size == NULL ||
	    query == NULL ||
	    (command & ~INET_IOCTL_ENCODING_MASK) != 0 ||
	    !inet_ioctl_is_wlan_group(command) ||
	    (command & INET_IOCTL_DIRECTION_MASK) != KERN_IOC_INOUT)
		return EINVAL;

	/* Maps the command number onto its request. */
	number = (unsigned)(command & INET_IOCTL_NUMBER_MASK);
	switch (number) {
	case 1:
		expected = SIOCSWLANSCAN;
		*size = sizeof(struct wlan_scan_request);
		*query = false;
		break;
	case 2:
		expected = SIOCGWLANSCAN;
		*size = sizeof(struct wlan_scan_status_request);
		*query = true;
		break;
	case 3:
		expected = SIOCGWLANBSS;
		*size = sizeof(struct wlan_bss_request);
		*query = true;
		break;
	case 4:
		expected = SIOCSWLANCONNECT;
		*size = sizeof(struct wlan_connect_request);
		*query = false;
		break;
	case 5:
		expected = SIOCSWLANDISCONNECT;
		*size = sizeof(struct wlan_disconnect_request);
		*query = false;
		break;
	case 6:
		expected = SIOCGWLANSTATUS;
		*size = sizeof(struct wlan_status_request);
		*query = true;
		break;
	default:
		return EINVAL;
	}

	/* The whole encoding, including the size, must match. */
	encoded_size = (command & INET_IOCTL_SIZE_MASK) >> 16;
	if (command != expected || encoded_size != *size)
		return EINVAL;

	/* Reports the classified command. */
	return 0;
}

/*
 * Checks the interface name of a WLAN request, which every version of the
 * request carries first.
 */
static int
inet_ioctl_wlan_validate_name(
	const struct wlan_ioctl_header *header)
{
	const void *terminator;

	/* Refuses a missing or empty name. */
	if (header == NULL || header->ifr_name[0] == '\0')
		return EINVAL;

	/* Refuses a name that fills the field without ending. */
	terminator = kern_memchr(header->ifr_name, '\0', sizeof(header->ifr_name));
	if (terminator == NULL)
		return EINVAL;

	/* Succeeded: the name can be looked up. */
	return 0;
}

/* Checks the version and size of a request for a WLAN device. */
static int
inet_ioctl_wlan_validate(
	const struct wlan_ioctl_header *header,
	size_t size)
{
	/* The request must be this kernel's version, at its size. */
	if (header->version != WLAN_ABI_VERSION || header->size != size)
		return EINVAL;

	/* Reports a valid header. */
	return 0;
}

/* Forwards a WLAN ioctl to the named WLAN device, scrubbing any secrets. */
static int
inet_ioctl_wlan(
	unsigned long command,
	uintptr_t argument)
{
	union inet_wlan_ioctl_request request;
	struct net_device *device;
	size_t size;
	bool query;
	unsigned capabilities;
	int error;

	device = NULL;
	size = 0;
	query = false;

	/* Classifies the command; changes need the superuser. */
	kern_memset(&request, 0, sizeof(request));
	error = inet_ioctl_wlan_classify(command, &size, &query);
	if (error != 0)
		goto out;
	if (!query && !inet_ioctl_caller_is_superuser()) {
		error = EPERM;
		goto out;
	}

	/* Reads and validates the request. */
	if (argument == 0) {
		error = EFAULT;
		goto out;
	}

	error = copyin(argument, &request, size);
	if (error != 0)
		goto out;
	error = inet_ioctl_wlan_validate_name(&request.header);
	if (error != 0)
		goto out;

	/* The named device must be a live WLAN device. */
	device = net_device_find_ref(request.header.ifr_name);
	if (device == NULL) {
		error = ENODEV;
		goto out;
	}

	capabilities = net_device_capabilities_get(device);
	if (!net_device_is_live(device)) {
		error = ENODEV;
		goto out_device;
	}

	/*
	 * An interface that is not a WLAN device answers no version of the
	 * request, so it is refused before the version is looked at: a caller
	 * built for another version still learns that the interface is not a
	 * radio, and goes on with the others.
	 */
	if ((capabilities & NET_DEVICE_CAP_WLAN) == 0) {
		error = EOPNOTSUPP;
		goto out_device;
	}

	/* Only a request of this kernel's version reaches the device. */
	error = inet_ioctl_wlan_validate(&request.header, size);
	if (error != 0)
		goto out_device;

	/* Forwards the request. */
	error = net_device_ioctl(device, command, &request);
	net_device_release(device);
	device = NULL;
	if (error != 0)
		goto out;

	/* The passphrase never travels back to user space. */
	if (command == SIOCSWLANCONNECT) {
		inet_ioctl_scrub(request.connect.passphrase,
				  sizeof(request.connect.passphrase));
		request.connect.passphrase_length = 0;
	}

	error = copyout(&request, argument, size);
	goto out;

out_device:
	net_device_release(device);
out:
	inet_ioctl_scrub(&request, sizeof(request));

	/* Reports why the request failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Creates an AF_INET socket of the protocol its type implies. */
static int
inet_create(
	int type,
	int protocol,
	struct socket **result)
{
	int error;

	/* Raw is ICMP, datagram is UDP, stream is TCP. */
	if (type == SOCK_RAW) {
		error = icmp_socket_create(protocol, result);
		return error;
	}

	if (type == SOCK_DGRAM) {
		error = udp_socket_create(protocol, result);
		return error;
	}

	if (type == SOCK_STREAM) {
		error = tcp_socket_create(protocol, result);
		return error;
	}

	/* Reports an unsupported type. */
	return EPROTONOSUPPORT;
}


/*
 * Creates an AF_INET6 socket (ws130-p003): a raw ICMPv6, a UDP or a TCP
 * one, made IPv6.
 */
static int
inet6_create(
	int type,
	int protocol,
	struct socket **result)
{
	struct inet_socket *inet;
	int error;

	/* Raw is ICMPv6, datagram is UDP, stream is TCP. */
	if (type == SOCK_RAW)
		error = icmp6_socket_create(protocol, result);
	else if (type == SOCK_DGRAM)
		error = udp_socket_create(protocol, result);
	else if (type == SOCK_STREAM)
		error = tcp_socket_create(protocol, result);
	else
		error = EPROTONOSUPPORT;
	if (error != 0)
		return error;

	/* The same socket, of the second family. */
	inet = (struct inet_socket *)*result;
	inet->family = AF_INET6;
	inet->socket.family = AF_INET6;

	/* Succeeded: the IPv6 socket. */
	return 0;
}

/* Finds the live interface an IPv4 address is configured on (EADDRNOTAVAIL: none). */
static int
inet_ipv4_address_local(
	uint32_t local,
	unsigned *ifindex)
{
	bool enabled;
	bool live;
	unsigned index;

	/* The interface that has the address. */
	*ifindex = 0;
	enabled = interface_lock();
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (interfaces[index].device == NULL || interfaces[index].address != local)
			continue;
		live = net_device_is_live(interfaces[index].device);
		if (live) {
			*ifindex = interfaces[index].device->ifindex;
			break;
		}
	}

	interface_unlock(enabled);

	/* None has it. */
	if (*ifindex == 0)
		return EADDRNOTAVAIL;

	/* Succeeded: the interface. */
	return 0;
}

/* Tells how two sockets' IPv4 addresses overlap, when both take IPv4. */
static int
inet_overlap_ipv4(
	const struct inet_socket *existing,
	const struct inet_socket *candidate)
{
	int accepts;

	/* Both must take IPv4. */
	accepts = inet_socket_accepts_ipv4(existing);
	if (!accepts)
		return INET_OVERLAP_NONE;
	accepts = inet_socket_accepts_ipv4(candidate);
	if (!accepts)
		return INET_OVERLAP_NONE;

	/* The same address, a wildcard and another, or two different ones. */
	if (existing->local_address == candidate->local_address)
		return INET_OVERLAP_SAME;
	if (existing->local_address == INADDR_ANY)
		return INET_OVERLAP_WILDCARD;
	if (candidate->local_address == INADDR_ANY)
		return INET_OVERLAP_WILDCARD;

	/* Succeeded: two different addresses do not overlap. */
	return INET_OVERLAP_NONE;
}

/* Tells how two sockets' IPv6 addresses overlap, when both take IPv6. */
static int
inet_overlap_ipv6(
	const struct inet_socket *existing,
	const struct inet_socket *candidate)
{
	int accepts;
	int same;
	int unspecified;

	/* Both must take IPv6. */
	accepts = inet_socket_accepts_ipv6(existing);
	if (!accepts)
		return INET_OVERLAP_NONE;
	accepts = inet_socket_accepts_ipv6(candidate);
	if (!accepts)
		return INET_OVERLAP_NONE;

	/* The same address. */
	same = in6_equal(&existing->local6, &candidate->local6);
	if (same)
		return INET_OVERLAP_SAME;

	/* A wildcard and another. */
	unspecified = in6_is_unspecified(&existing->local6);
	if (unspecified)
		return INET_OVERLAP_WILDCARD;
	unspecified = in6_is_unspecified(&candidate->local6);
	if (unspecified)
		return INET_OVERLAP_WILDCARD;

	/* Succeeded: two different addresses do not overlap. */
	return INET_OVERLAP_NONE;
}

/* Makes the IPv4-mapped IPv6 address (::ffff:a.b.c.d) of an IPv4 address in host order. */
static void
inet6_mapped(
	struct in6_addr *out,
	uint32_t address)
{
	kern_memset(out, 0, sizeof(*out));
	out->s6_addr[10] = 0xffU;
	out->s6_addr[11] = 0xffU;
	out->s6_addr[12] = (uint8_t)(address >> 24);
	out->s6_addr[13] = (uint8_t)(address >> 16);
	out->s6_addr[14] = (uint8_t)(address >> 8);
	out->s6_addr[15] = (uint8_t)address;
}

/*
 * Binds an AF_INET6 socket.  An IPv4-mapped address makes it an IPv4 one
 * (refused under IPV6_V6ONLY); [::] leaves it open to both; another
 * address must be one of the host's that may be used, and a link-local
 * one names its interface in the scope, which then binds the socket to it.
 */
static int
inet6_socket_bind(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	struct sockaddr_in6 input;
	struct net_device *device;
	unsigned flags;
	unsigned ifindex;
	uint32_t local;
	int mapped;
	int linklocal;
	int unspecified;
	int error;

	/* A complete AF_INET6 address. */
	if (address == NULL ||
	    length < sizeof(input) ||
	    address->sa_family != AF_INET6)
		return EINVAL;
	kern_memcpy(&input, address, sizeof(input));

	/* An IPv4-mapped address: an IPv4 one is configured on an interface (or the wildcard). */
	mapped = in6_is_v4mapped(&input.sin6_addr);
	if (mapped) {
		if (inet->v6only)
			return EADDRNOTAVAIL;
		local = inet_socket_unmapped(&input.sin6_addr);
		ifindex = 0;
		if (local != INADDR_ANY) {
			error = inet_ipv4_address_local(local, &ifindex);
			if (error != 0)
				return error;
		}

		/* The socket speaks IPv4 from now on (the wildcard keeps a SO_BINDTODEVICE interface, as IPv4's does). */
		if (ifindex != 0)
			inet->ifindex = ifindex;
		inet->local_address = local;
		inet->local6 = input.sin6_addr;
		inet->mapped = 1;
		inet->local_port = net_ntohs(input.sin6_port);
		inet->inet_flags |= INET_SOCKET_BOUND;
		return 0;
	}

	/* A link-local address is on the interface its scope names. */
	ifindex = 0;
	linklocal = in6_is_linklocal(&input.sin6_addr);
	if (linklocal) {
		if (input.sin6_scope_id == 0)
			return EINVAL;
		ifindex = input.sin6_scope_id;
	}

	/* A specific address must be the host's (on that interface), and not one still being checked. */
	unspecified = in6_is_unspecified(&input.sin6_addr);
	if (!unspecified) {
		device = NULL;
		if (ifindex != 0) {
			device = net_device_find_by_index_ref(ifindex);
			if (device == NULL)
				return EADDRNOTAVAIL;
		}
		error = ipv6_address_state(device, &input.sin6_addr, &flags);
		if (device != NULL)
			net_device_release(device);
		if (error != 0)
			return EADDRNOTAVAIL;
		if ((flags & (IN6_ADDRESS_TENTATIVE | IN6_ADDRESS_DUPLICATED)) != 0U)
			return EADDRNOTAVAIL;
	}

	/* Records the local endpoint; [::] keeps a SO_BINDTODEVICE interface, as IPv4's wildcard does. */
	if (ifindex != 0)
		inet->ifindex = ifindex;
	inet->local6 = input.sin6_addr;
	inet->local_address = 0;
	inet->local_port = net_ntohs(input.sin6_port);
	inet->inet_flags |= INET_SOCKET_BOUND;

	/* Succeeded: the bound socket. */
	return 0;
}

/*
 * Records the remote endpoint of an AF_INET6 socket.  An IPv4-mapped peer
 * makes it an IPv4 one (refused under IPV6_V6ONLY or on an IPv6 address);
 * an IPv6 peer is refused to an IPv4-mapped socket.  A link-local peer
 * needs its interface: the scope's, or the one the socket is bound to.
 */
static int
inet6_socket_connect(
	struct inet_socket *inet,
	const struct sockaddr *address,
	socklen_t length)
{
	struct sockaddr_in6 input;
	uint32_t remote;
	unsigned scope;
	int mapped;
	int linklocal;
	int unspecified;

	/* A complete AF_INET6 address. */
	if (address == NULL ||
	    length < sizeof(input) ||
	    address->sa_family != AF_INET6)
		return EINVAL;
	kern_memcpy(&input, address, sizeof(input));

	/* An IPv4-mapped peer: only for a socket that may speak IPv4. */
	mapped = in6_is_v4mapped(&input.sin6_addr);
	unspecified = in6_is_unspecified(&inet->local6);
	if (mapped) {
		if (inet->v6only)
			return ENETUNREACH;
		if (!inet->mapped && !unspecified)
			return EAFNOSUPPORT;
		remote = inet_socket_unmapped(&input.sin6_addr);
		if (remote == INADDR_ANY)
			return EADDRNOTAVAIL;

		/* The socket speaks IPv4 from now on. */
		inet->remote_address = remote;
		inet->remote6 = input.sin6_addr;
		inet->remote_port = net_ntohs(input.sin6_port);
		inet->mapped = 1;
		inet->inet_flags |= INET_SOCKET_CONNECTED;
		return 0;
	}

	/* An IPv6 peer: not for an IPv4-mapped socket, and a specific one. */
	if (inet->mapped)
		return EAFNOSUPPORT;
	unspecified = in6_is_unspecified(&input.sin6_addr);
	if (unspecified)
		return EADDRNOTAVAIL;

	/* A link-local peer: the interface it is on, the one the socket is bound to if any. */
	scope = 0;
	linklocal = in6_is_linklocal(&input.sin6_addr);
	if (linklocal) {
		scope = input.sin6_scope_id;
		if (scope == 0)
			scope = inet->ifindex;
		if (scope == 0)
			return EINVAL;
		if (inet->ifindex != 0 && scope != inet->ifindex)
			return EINVAL;
	}

	/* Records the remote endpoint. */
	inet->remote6 = input.sin6_addr;
	inet->remote_address = 0;
	inet->scope6 = scope;
	inet->remote_port = net_ntohs(input.sin6_port);
	inet->inet_flags |= INET_SOCKET_CONNECTED;

	/* Succeeded: the connected socket. */
	return 0;
}

/* Copies the local or the remote endpoint of an AF_INET6 socket into a socket address. */
static int
inet6_socket_name(
	struct inet_socket *inet,
	struct sockaddr *address,
	socklen_t *length,
	int peer)
{
	struct sockaddr_in6 output;
	socklen_t copied;
	int linklocal;

	/* The endpoint: IPv4-mapped, or the IPv6 one. */
	kern_memset(&output, 0, sizeof(output));
	output.sin6_family = AF_INET6;
	if (peer) {
		output.sin6_port = net_htons(inet->remote_port);
		if (inet->mapped)
			inet6_mapped(&output.sin6_addr, inet->remote_address);
		else
			output.sin6_addr = inet->remote6;
	} else {
		output.sin6_port = net_htons(inet->local_port);
		if (inet->mapped)
			inet6_mapped(&output.sin6_addr, inet->local_address);
		else
			output.sin6_addr = inet->local6;
	}

	/* A link-local one with its interface: the peer's, or the one the socket is on. */
	linklocal = in6_is_linklocal(&output.sin6_addr);
	if (linklocal && peer) {
		output.sin6_scope_id = inet->scope6;
	} else if (linklocal) {
		output.sin6_scope_id = inet->ifindex;
		if (output.sin6_scope_id == 0)
			output.sin6_scope_id = inet->scope6;
	}

	/* Copies as much as fits and reports the full length. */
	if (*length < sizeof(output))
		copied = *length;
	else
		copied = sizeof(output);
	kern_memcpy(address, &output, copied);
	*length = sizeof(output);

	/* Succeeded: the copied address. */
	return 0;
}
