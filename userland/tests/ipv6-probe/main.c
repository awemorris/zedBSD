/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The IPv6 kernel probe (ws130-p002): tries the kernel's IPv6 core
 * without a transport, on a guest whose interface reaches a router (QEMU's
 * user network answers router solicitations with fec0::/64).
 *
 *   ipv6-probe [-i INTERFACE]
 *   ipv6-probe -t      (the transports on the host itself, ws130-p003: transport.c)
 *   ipv6-probe -T      (the core's steps, and the transports through the router before the removals)
 *
 * Without -i it takes the first interface that is up and not the
 * loopback.  On a routing socket made for IPv6 it: lists the loopback's
 * addresses (::1); adds a link-local address and waits for its duplicate
 * address detection to pass (RTM_ADDRINFO preferred); waits for the
 * Router Advertisement the kernel's solicitation brings (RTM_ROUTERADV)
 * and reads its prefix; adds an address in that prefix with lifetimes and
 * sees them counted; adds a default route through the router and reads
 * the routes back (the connected ones too); removes them; turns IPv6 off
 * and on again.  As a user who is not root (who gets no raw socket, so no
 * routing socket either), the listing still works and the first change is
 * refused (step=add-link-local, EPERM).  Each
 * line is "IPV6 ..." on standard output; the last is "IPV6 PASS" (status
 * 0) or "IPV6 FAIL step=<what> error=<errno>" (status 1).
 */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <uapi/netif.h>
#include <uapi/netinet.h>
#include <uapi/route.h>

#include "probe.h"

/* The longest a record is, and how long the probe waits for each event (milliseconds). */
#define PROBE_RECORD_MAX	2048U
#define PROBE_DAD_MS		5000
#define PROBE_RA_MS		20000

/* The link-local address the probe adds: fe80::1234:5678:9abc:def0. */
static const uint8_t probe_link_local[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0 };

static int probe_interface(int inet, char *name, size_t size, unsigned *ifindex);
static int probe_wait(int events, unsigned type, unsigned ifindex, const struct in6_addr *address, uint8_t *record, int timeout_ms);
static int probe_add(int inet, const char *name, const struct in6_addr *address, unsigned prefixlen, unsigned flags, uint32_t valid, uint32_t preferred);
static int probe_list(int inet, const char *name, struct in6_ifaddrs *list);
static void probe_print(const char *what, const struct in6_addr *address, unsigned prefixlen);
static int probe_prefix(const uint8_t *message, size_t length, struct in6_addr *prefix, unsigned *prefixlen);

int
main(
	int argc,
	char **argv)
{
	static uint8_t record[PROBE_RECORD_MAX];
	struct in6_ifaddrs list;
	struct in6_rtentry route;
	struct in6_aliasreq request;
	struct ifreq ifreq;
	struct rtm_routeradv *advert;
	struct in6_addr address;
	struct in6_addr router;
	struct in6_addr prefix;
	char name[IFNAMSIZ];
	const char *lookup;
	unsigned ifindex;
	unsigned prefixlen;
	unsigned index;
	unsigned routes;
	int inet;
	int events;
	int events_error;
	int found;
	int same;
	int through_router;
	int status;
	int error;

	/* -l [NAME]: the C library's IPv6 (ws130-p004), and a name's addresses in the DNS. */
	if (argc >= 2) {
		same = strcmp(argv[1], "-l");
		if (same == 0) {
			lookup = NULL;
			if (argc >= 3)
				lookup = argv[2];
			status = probe_libc(lookup);
			if (status != 0)
				return status;
			printf("IPV6 PASS\n");
			return 0;
		}
	}

	/* -t: the transports on the host itself (ws130-p003). */
	through_router = 0;
	if (argc == 2) {
		same = strcmp(argv[1], "-t");
		if (same == 0) {
			status = probe_transport_local();
			if (status != 0)
				return status;
			printf("IPV6 PASS\n");
			return 0;
		}

		/* -T: the core's steps, and the transports through the router. */
		same = strcmp(argv[1], "-T");
		if (same == 0)
			through_router = 1;
	}

	/*
	 * The sockets.  A raw socket (the route socket too) is the
	 * superuser's, so another user goes on without the events to show
	 * that the change itself is refused (step=add-link-local).
	 */
	inet = socket(AF_INET, SOCK_DGRAM, 0);
	if (inet < 0)
		return probe_fail("socket", errno);
	events_error = 0;
	events = socket(AF_ROUTE, SOCK_RAW, AF_INET6);
	if (events < 0)
		events_error = errno;
	if (events < 0 && events_error != EPERM)
		return probe_fail("route-socket", events_error);

	/* The interface: named, or the first that is up and not the loopback. */
	name[0] = '\0';
	if (argc == 3) {
		same = strcmp(argv[1], "-i");
		if (same == 0)
			snprintf(name, sizeof(name), "%s", argv[2]);
	}

	/* Its index. */
	error = probe_interface(inet, name, sizeof(name), &ifindex);
	if (error != 0)
		return probe_fail("interface", error);
	printf("IPV6 interface %s index=%u\n", name, ifindex);

	/* The loopback has ::1, and IPv6 on. */
	error = probe_list(inet, "lo0", &list);
	if (error != 0)
		return probe_fail("list-lo0", error);
	found = 0;
	for (index = 0U; index < list.ifa_count; index++) {
		if (list.ifa_list[index].ife_addr.s6_addr[15] == 1U && list.ifa_list[index].ife_prefixlen == 128U)
			found = 1;
	}

	/* Found, with IPv6 on. */
	if (!found || !list.ifa_enabled)
		return probe_fail("loopback-address", ENOENT);
	printf("IPV6 lo0 ::1 ok\n");

	/* A link-local address, detected, then preferred. */
	memcpy(address.s6_addr, probe_link_local, 16U);
	error = probe_add(inet, name, &address, 64U, 0U, IN6_LIFETIME_INFINITE, IN6_LIFETIME_INFINITE);
	if (error != 0)
		return probe_fail("add-link-local", error);

	/* A user the address was not refused to still needs the events. */
	if (events < 0)
		return probe_fail("route-socket", events_error);
	error = probe_wait(events, RTM_ADDRINFO, ifindex, &address, record, PROBE_DAD_MS);
	if (error != 0)
		return probe_fail("dad", error);
	if (((struct rtm_addrinfo *)record)->rtm_transition != RTM_ADDRINFO_PREFERRED)
		return probe_fail("dad-transition", EPROTO);
	probe_print("preferred", &address, 64U);

	/* The router answers the kernel's solicitation. */
	error = probe_wait(events, RTM_ROUTERADV, ifindex, NULL, record, PROBE_RA_MS);
	if (error != 0)
		return probe_fail("router-advertisement", error);
	advert = (struct rtm_routeradv *)record;
	router = advert->rtm_source;
	probe_print("router", &router, 128U);
	error = probe_prefix(record + sizeof(*advert), advert->rtm_message_length, &prefix, &prefixlen);
	if (error != 0)
		return probe_fail("prefix", error);
	probe_print("prefix", &prefix, prefixlen);

	/* An address in the prefix, with lifetimes counted down. */
	address = prefix;
	address.s6_addr[14] = 0x12U;
	address.s6_addr[15] = 0x34U;
	error = probe_add(inet, name, &address, prefixlen, IN6_IFF_AUTOCONF, 3600U, 1800U);
	if (error != 0)
		return probe_fail("add-prefix-address", error);
	error = probe_wait(events, RTM_ADDRINFO, ifindex, &address, record, PROBE_DAD_MS);
	if (error != 0)
		return probe_fail("prefix-dad", error);
	error = probe_list(inet, name, &list);
	if (error != 0)
		return probe_fail("list", error);
	found = 0;
	for (index = 0U; index < list.ifa_count; index++) {
		same = memcmp(&list.ifa_list[index].ife_addr, &address, 16U);
		if (same != 0)
			continue;
		printf("IPV6 lifetimes valid=%u preferred=%u flags=%x\n", list.ifa_list[index].ife_valid, list.ifa_list[index].ife_preferred,
		    list.ifa_list[index].ife_flags);
		if (list.ifa_list[index].ife_valid > 3500U && list.ifa_list[index].ife_valid <= 3600U &&
		    (list.ifa_list[index].ife_flags & IN6_IFF_TENTATIVE) == 0U)
			found = 1;
	}

	/* The lifetimes as given. */
	if (!found)
		return probe_fail("lifetimes", EPROTO);

	/* A default route through the router; the routes read back. */
	memset(&route, 0, sizeof(route));
	route.rt6_gateway = router;
	route.rt6_ifindex = ifindex;
	route.rt6_flags = RTF_GATEWAY;
	route.rt6_lifetime = 1800U;
	error = ioctl(inet, SIOCADDRT_IN6, &route);
	if (error != 0)
		return probe_fail("add-route", errno);
	routes = 0U;
	for (index = 0U;; index++) {
		memset(&route, 0, sizeof(route));
		route.rt6_index = index;
		error = ioctl(inet, SIOCGRTENTRY_IN6, &route);
		if (error != 0)
			break;
		probe_print("route", &route.rt6_dst, route.rt6_prefixlen);
		routes++;
	}

	/* The default and the two connected routes at least. */
	if (routes < 3U)
		return probe_fail("routes", ENOENT);

	/* With -T, the transports through the router while the address and the route are there (ws130-p003). */
	if (through_router) {
		status = probe_transport_router(&router, ifindex, &prefix);
		if (status != 0)
			return status;
	}

	/* The route and the address removed. */
	memset(&route, 0, sizeof(route));
	route.rt6_ifindex = ifindex;
	error = ioctl(inet, SIOCDELRT_IN6, &route);
	if (error != 0)
		return probe_fail("delete-route", errno);
	memset(&request, 0, sizeof(request));
	snprintf(request.ifra_name, sizeof(request.ifra_name), "%s", name);
	request.ifra_addr.sin6_family = AF_INET6;
	request.ifra_addr.sin6_addr = address;
	error = ioctl(inet, SIOCDIFADDR_IN6, &request);
	if (error != 0)
		return probe_fail("remove-address", errno);
	error = ioctl(inet, SIOCDIFADDR_IN6, &request);
	if (error == 0)
		return probe_fail("remove-again", EEXIST);
	if (errno != ENOENT)
		return probe_fail("remove-again", errno);
	printf("IPV6 removed ok\n");

	/* IPv6 off empties the interface, and on again. */
	memset(&ifreq, 0, sizeof(ifreq));
	snprintf(ifreq.ifr_name, sizeof(ifreq.ifr_name), "%s", name);
	ifreq.ifr_flags = 0;
	error = ioctl(inet, SIOCSIFINET6, &ifreq);
	if (error != 0)
		return probe_fail("off", errno);
	error = probe_list(inet, name, &list);
	if (error != 0)
		return probe_fail("off-list", error);
	if (list.ifa_count != 0U || list.ifa_enabled)
		return probe_fail("off-list", EPROTO);
	ifreq.ifr_flags = 1;
	error = ioctl(inet, SIOCSIFINET6, &ifreq);
	if (error != 0)
		return probe_fail("on", errno);
	printf("IPV6 off and on ok\n");

	/* Succeeded. */
	printf("IPV6 PASS\n");
	return 0;
}

/*
 * Says a failed step and gives the status.
 */
int
probe_fail(
	const char *step,
	int error)
{
	printf("IPV6 FAIL step=%s error=%d\n", step, error);
	return 1;
}

/* Finds the interface by name (filled when empty: the first up and not the loopback) and its index. */
static int
probe_interface(
	int inet,
	char *name,
	size_t size,
	unsigned *ifindex)
{
	struct ifreq requests[16];
	struct ifreq request;
	struct ifconf list;
	unsigned count;
	unsigned index;
	int same;
	int error;

	/* The interfaces. */
	memset(&list, 0, sizeof(list));
	list.ifc_len = sizeof(requests);
	list.ifc_buf = (uint64_t)(uintptr_t)requests;
	error = ioctl(inet, SIOCGIFCONF, &list);
	if (error != 0)
		return errno;
	count = list.ifc_len / sizeof(requests[0]);

	/* The one named, or the first that is up and not the loopback. */
	for (index = 0U; index < count; index++) {
		memset(&request, 0, sizeof(request));
		memcpy(request.ifr_name, requests[index].ifr_name, sizeof(request.ifr_name));
		error = ioctl(inet, SIOCGIFFLAGS, &request);
		if (error != 0)
			continue;
		same = strcmp(name, request.ifr_name);
		if (name[0] != '\0' && same != 0)
			continue;
		if (name[0] == '\0' && ((request.ifr_flags & IFF_LOOPBACK) != 0 || (request.ifr_flags & IFF_UP) == 0))
			continue;
		snprintf(name, size, "%s", request.ifr_name);
		*ifindex = (unsigned)requests[index].ifr_ifindex;
		return 0;
	}

	/* None. */
	return ENODEV;
}

/* Waits for a record of a type about an interface (and an address, when given). */
static int
probe_wait(
	int events,
	unsigned type,
	unsigned ifindex,
	const struct in6_addr *address,
	uint8_t *record,
	int timeout_ms)
{
	struct pollfd entry;
	const struct rtm_header *header;
	const struct rtm_addrinfo *info;
	const struct rtm_routeradv *advert;
	ssize_t count;
	int waited;
	int ready;
	int same;

	/* Records until the one asked for, or the time is up. */
	for (waited = 0; waited < timeout_ms; waited += 100) {
		entry.fd = events;
		entry.events = POLLIN;
		entry.revents = 0;
		ready = poll(&entry, 1, 100);
		if (ready <= 0)
			continue;
		count = read(events, record, PROBE_RECORD_MAX);
		if (count < (ssize_t)sizeof(*header))
			continue;
		header = (const struct rtm_header *)record;
		if (header->rtm_version != RTM_VERSION || header->rtm_type != type)
			continue;
		if (type == RTM_ROUTERADV) {
			advert = (const struct rtm_routeradv *)record;
			if (advert->rtm_ifindex == ifindex)
				return 0;
			continue;
		}

		/* An address's change on the interface, about the address. */
		info = (const struct rtm_addrinfo *)record;
		if (info->rtm_ifindex != ifindex)
			continue;
		same = 0;
		if (address != NULL)
			same = memcmp(&info->rtm_address, address, 16U);
		if (same != 0)
			continue;
		return 0;
	}

	/* Nothing came. */
	return ETIMEDOUT;
}

/* Adds an address to an interface. */
static int
probe_add(
	int inet,
	const char *name,
	const struct in6_addr *address,
	unsigned prefixlen,
	unsigned flags,
	uint32_t valid,
	uint32_t preferred)
{
	struct in6_aliasreq request;
	int error;

	/* The request. */
	memset(&request, 0, sizeof(request));
	snprintf(request.ifra_name, sizeof(request.ifra_name), "%s", name);
	request.ifra_addr.sin6_family = AF_INET6;
	request.ifra_addr.sin6_addr = *address;
	request.ifra_prefixlen = prefixlen;
	request.ifra_flags = flags;
	request.ifra_valid = valid;
	request.ifra_preferred = preferred;
	error = ioctl(inet, SIOCAIFADDR_IN6, &request);
	if (error != 0)
		return errno;
	return 0;
}

/* Lists an interface's addresses. */
static int
probe_list(
	int inet,
	const char *name,
	struct in6_ifaddrs *list)
{
	int error;

	/* The request. */
	memset(list, 0, sizeof(*list));
	snprintf(list->ifa_name, sizeof(list->ifa_name), "%s", name);
	error = ioctl(inet, SIOCGIFADDRS_IN6, list);
	if (error != 0)
		return errno;
	return 0;
}

/* Prints an address and its prefix length as eight groups. */
static void
probe_print(
	const char *what,
	const struct in6_addr *address,
	unsigned prefixlen)
{
	printf("IPV6 %s %x:%x:%x:%x:%x:%x:%x:%x/%u\n", what,
	    (unsigned)(address->s6_addr[0] << 8 | address->s6_addr[1]), (unsigned)(address->s6_addr[2] << 8 | address->s6_addr[3]),
	    (unsigned)(address->s6_addr[4] << 8 | address->s6_addr[5]), (unsigned)(address->s6_addr[6] << 8 | address->s6_addr[7]),
	    (unsigned)(address->s6_addr[8] << 8 | address->s6_addr[9]), (unsigned)(address->s6_addr[10] << 8 | address->s6_addr[11]),
	    (unsigned)(address->s6_addr[12] << 8 | address->s6_addr[13]), (unsigned)(address->s6_addr[14] << 8 | address->s6_addr[15]),
	    prefixlen);
}

/* Finds the first prefix information option of a Router Advertisement (from its type byte). */
static int
probe_prefix(
	const uint8_t *message,
	size_t length,
	struct in6_addr *prefix,
	unsigned *prefixlen)
{
	size_t position;
	size_t size;

	/* The options after the 16 bytes of the advertisement. */
	for (position = 16U; position + 2U <= length; position += size) {
		size = (size_t)message[position + 1U] * 8U;
		if (size == 0U || position + size > length)
			return EPROTO;
		if (message[position] == 3U && size == 32U) {
			*prefixlen = message[position + 2U];
			memcpy(prefix->s6_addr, message + position + 16U, 16U);
			return 0;
		}
	}

	/* No prefix. */
	return ENOENT;
}
