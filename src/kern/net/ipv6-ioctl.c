/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * IPv6's ioctls (ws130-p002; netif.h and route.h): an interface's
 * addresses (add or renew, remove, list), IPv6 on or off on an
 * interface, and the routes (add, delete, read one by its ordinal).
 * They are taken on AF_INET sockets (inet-socket.c); the listing and the
 * reading are open to everyone, the changes need the superuser (checked
 * there).
 */

#include "ipv6.h"

#include "kern/net/net-device.h"
#include "kern/uaccess.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/netif.h>
#include <uapi/route.h>

static int ipv6_ioctl_address(unsigned long command, uintptr_t argument);
static int ipv6_ioctl_list(uintptr_t argument);
static int ipv6_ioctl_enable(uintptr_t argument);
static int ipv6_ioctl_route(unsigned long command, uintptr_t argument);
static int ipv6_ioctl_route_get(uintptr_t argument);
static uint32_t ipv6_ioctl_seconds_left(uint64_t deadline_ms, uint64_t now_ms);

/* Tells whether a command is one of IPv6's. */
int
ipv6_ioctl_handles(
	unsigned long command)
{
	/* Each of them. */
	switch (command) {
	case SIOCAIFADDR_IN6:
	case SIOCDIFADDR_IN6:
	case SIOCGIFADDRS_IN6:
	case SIOCSIFINET6:
	case SIOCADDRT_IN6:
	case SIOCDELRT_IN6:
	case SIOCGRTENTRY_IN6:
		return 1;
	default:
		break;
	}

	/* Another family's. */
	return 0;
}

/* Tells whether a command of IPv6's only reads (open to everyone). */
int
ipv6_ioctl_is_query(
	unsigned long command)
{
	/* The listing and the reading. */
	if (command == SIOCGIFADDRS_IN6)
		return 1;
	if (command == SIOCGRTENTRY_IN6)
		return 1;

	/* A change. */
	return 0;
}

/* Carries out one of IPv6's ioctls (the caller checked the privilege). */
int
ipv6_ioctl(
	unsigned long command,
	uintptr_t argument)
{
	int error;

	/* A request to read or write. */
	if (argument == 0U)
		return EFAULT;

	/* Each command. */
	switch (command) {
	case SIOCAIFADDR_IN6:
	case SIOCDIFADDR_IN6:
		error = ipv6_ioctl_address(command, argument);
		break;
	case SIOCGIFADDRS_IN6:
		error = ipv6_ioctl_list(argument);
		break;
	case SIOCSIFINET6:
		error = ipv6_ioctl_enable(argument);
		break;
	case SIOCADDRT_IN6:
	case SIOCDELRT_IN6:
		error = ipv6_ioctl_route(command, argument);
		break;
	case SIOCGRTENTRY_IN6:
		error = ipv6_ioctl_route_get(argument);
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	/* Not done. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Adds (or renews) or removes an address of an interface (struct in6_aliasreq). */
static int
ipv6_ioctl_address(
	unsigned long command,
	uintptr_t argument)
{
	struct in6_aliasreq request;
	struct net_device *device;
	int error;

	/* The request, its name ended, the address of the family. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	request.ifra_name[IFNAMSIZ - 1U] = '\0';
	if (request.ifra_addr.sin6_family != AF_INET6)
		return EAFNOSUPPORT;

	/* The interface. */
	device = net_device_find_ref(request.ifra_name);
	if (device == NULL)
		return ENODEV;

	/* Added or renewed (only the caller's flags), or removed. */
	if (command == SIOCAIFADDR_IN6) {
		error = ipv6_address_add(device, &request.ifra_addr.sin6_addr, request.ifra_prefixlen, request.ifra_flags & IN6_ADDRESS_CALLER,
		    request.ifra_valid, request.ifra_preferred);
	} else {
		error = ipv6_address_remove(device, &request.ifra_addr.sin6_addr);
	}

	/* The interface let go. */
	net_device_release(device);

	/* Not done. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Lists an interface's addresses (struct in6_ifaddrs). */
static int
ipv6_ioctl_list(
	uintptr_t argument)
{
	struct in6_ifaddrs answer;
	struct in6_address entries[IN6_IFADDRS_MAX];
	struct net_device *device;
	unsigned count;
	unsigned index;
	uint64_t now;
	int enabled;
	int error;

	/* The interface named. */
	error = copyin(argument, answer.ifa_name, sizeof(answer.ifa_name));
	if (error != 0)
		return error;
	answer.ifa_name[IFNAMSIZ - 1U] = '\0';
	device = net_device_find_ref(answer.ifa_name);
	if (device == NULL)
		return ENODEV;

	/* Its addresses. */
	error = ipv6_address_list(device, entries, IN6_IFADDRS_MAX, &count, &enabled);
	net_device_release(device);
	if (error != 0)
		return error;

	/* The answer: each address and the seconds left of its lifetimes. */
	now = ipv6_now_ms();
	kern_memset(&answer.ifa_count, 0, sizeof(answer) - sizeof(answer.ifa_name));
	answer.ifa_count = count;
	answer.ifa_enabled = (uint32_t)enabled;
	for (index = 0U; index < count; index++) {
		answer.ifa_list[index].ife_addr = entries[index].address;
		answer.ifa_list[index].ife_prefixlen = entries[index].prefixlen;
		answer.ifa_list[index].ife_flags = entries[index].flags;
		answer.ifa_list[index].ife_valid = ipv6_ioctl_seconds_left(entries[index].valid_ms, now);
		answer.ifa_list[index].ife_preferred = ipv6_ioctl_seconds_left(entries[index].preferred_ms, now);
	}

	/* Copied out. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Turns IPv6 on or off on an interface (struct ifreq, ifr_flags 1 or 0). */
static int
ipv6_ioctl_enable(
	uintptr_t argument)
{
	struct ifreq request;
	struct net_device *device;
	int error;

	/* The request and its interface. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	request.ifr_name[IFNAMSIZ - 1U] = '\0';
	device = net_device_find_ref(request.ifr_name);
	if (device == NULL)
		return ENODEV;

	/* On or off. */
	error = ipv6_enable(device, request.ifr_flags != 0);
	net_device_release(device);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Adds or deletes a route (struct in6_rtentry). */
static int
ipv6_ioctl_route(
	unsigned long command,
	uintptr_t argument)
{
	struct in6_rtentry entry;
	struct in6_route route;
	struct net_device *device;
	uint64_t now;
	int error;

	/* The request, on an interface named by its index. */
	error = copyin(argument, &entry, sizeof(entry));
	if (error != 0)
		return error;
	if (entry.rt6_prefixlen > 128U)
		return EINVAL;

	/* Deleted. */
	if (command == SIOCDELRT_IN6) {
		error = ipv6_route_delete(&entry.rt6_dst, entry.rt6_prefixlen, entry.rt6_ifindex);
		return error;
	}

	/* Added: the interface it goes out of. */
	if (entry.rt6_ifindex == 0U)
		return EINVAL;
	device = net_device_find_by_index_ref(entry.rt6_ifindex);
	if (device == NULL)
		return ENODEV;
	now = ipv6_now_ms();
	kern_memset(&route, 0, sizeof(route));
	route.destination = entry.rt6_dst;
	route.prefixlen = entry.rt6_prefixlen;
	route.gateway = entry.rt6_gateway;
	route.ifindex = entry.rt6_ifindex;
	route.device = device;
	route.flags = entry.rt6_flags | RTF_UP;
	route.metric = entry.rt6_metric;
	if (entry.rt6_lifetime != IN6_LIFETIME_INFINITE)
		route.expires_ms = now + (uint64_t)entry.rt6_lifetime * 1000U + 1U;
	error = ipv6_route_add(&route);
	net_device_release(device);
	if (error != 0)
		return error;

	/* Succeeded: the route borrows the interface's IPv6 record's reference. */
	return 0;
}

/* Reads the route at an ordinal (struct in6_rtentry, rt6_index in). */
static int
ipv6_ioctl_route_get(
	uintptr_t argument)
{
	struct in6_rtentry entry;
	struct in6_route route;
	uint32_t ordinal;
	uint64_t now;
	int error;

	/* The ordinal asked. */
	error = copyin(argument, &entry, sizeof(entry));
	if (error != 0)
		return error;
	ordinal = entry.rt6_index;

	/* The route. */
	error = ipv6_route_get(ordinal, &route);
	if (error != 0)
		return error;

	/* The answer. */
	now = ipv6_now_ms();
	kern_memset(&entry, 0, sizeof(entry));
	entry.rt6_dst = route.destination;
	entry.rt6_gateway = route.gateway;
	entry.rt6_prefixlen = route.prefixlen;
	entry.rt6_flags = route.flags;
	entry.rt6_ifindex = route.ifindex;
	entry.rt6_metric = route.metric;
	entry.rt6_lifetime = ipv6_ioctl_seconds_left(route.expires_ms, now);
	entry.rt6_index = ordinal;
	error = copyout(&entry, argument, sizeof(entry));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives the whole seconds left until a deadline (IN6_LIFETIME_INFINITE for none, 0 when it passed). */
static uint32_t
ipv6_ioctl_seconds_left(
	uint64_t deadline_ms,
	uint64_t now_ms)
{
	uint64_t seconds;

	/* None. */
	if (deadline_ms == 0U)
		return IN6_LIFETIME_INFINITE;

	/* Passed. */
	if (deadline_ms <= now_ms)
		return 0U;

	/* The seconds left, within the field. */
	seconds = (deadline_ms - now_ms) / 1000U;
	if (seconds >= IN6_LIFETIME_INFINITE)
		seconds = IN6_LIFETIME_INFINITE - 1U;
	return (uint32_t)seconds;
}
