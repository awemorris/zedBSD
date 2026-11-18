/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD route userland command.
 */

#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/route.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int show_routes(int descriptor);
static uint32_t get_address(const struct sockaddr *sa);
static int usage(void);
static void set_address(struct sockaddr *sa, uint32_t value);
static int route6_main(int descriptor, int argc, char **argv);
static int route6_show(int descriptor);
static int route6_change(int descriptor, int add, int argc, char **argv);

/*
 * Runs the route command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int status;
	uint32_t original;
	struct rtentry route;
	struct in_addr destination = {0}, mask = {0}, gateway = {0};
	const char *ifname;
	unsigned prefix;
	int descriptor, add, arg;

	ifname = NULL;
	prefix = 0;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	arg = 2;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return 1;

	/* -6: the IPv6 routes (ws130-p005). */
	if (argc >= 2 && strcmp(argv[1], "-6") == 0) {
		status = route6_main(descriptor, argc - 1, argv + 1);
		close(descriptor);
		return status;
	}

	/* Handles the selected command-line operation. */
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "show") == 0) ||
	    (argc == 3 && strcmp(argv[1], "-n") == 0 &&
	     strcmp(argv[2], "show") == 0)) {
		status = show_routes(descriptor);
		close(descriptor);

		/* Returns the computed result. */
		return status;
	}

	/* Handles the selected command-line operation. */
	if (argc < 3 || ((add = strcmp(argv[1], "add") == 0) == 0 &&
			 strcmp(argv[1], "delete") != 0)) {
		close(descriptor);

		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}
	memset(&route, 0, sizeof(route));
	route.rt_flags = RTF_UP | (add ? RTF_STATIC : 0);

	/* Handles the selected command-line operation. */
	if (strcmp(argv[arg], "default") == 0) {
		arg++;
	} else if (strcmp(argv[arg], "-net") == 0 && ++arg < argc) {
		/* Validates the command-line arguments. */
		if (netutil_parse_cidr(argv[arg++], &destination, &mask,
				       &prefix) != 0) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		original = destination.s_addr;
		destination.s_addr &= mask.s_addr;

		/* Handles the original condition. */
		if (original != destination.s_addr) {
			puts("route: destination contains host bits");
			close(descriptor);

			/* Reports operation failure. */
			return 1;
		}
	} else if (strcmp(argv[arg], "-host") == 0 && ++arg < argc) {
		/* Validates the command-line arguments. */
		if (netutil_parse_ipv4(argv[arg++], &destination) != 0) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		mask.s_addr = 0xffffffffU;
		route.rt_flags |= RTF_HOST;
	} else {
		close(descriptor);

		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the selected command-line operation. */
	if (arg < argc && strcmp(argv[arg], "-interface") == 0) {
		/* Validates the command-line arguments. */
		if (++arg >= argc) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		ifname = argv[arg++];
	} else if (arg < argc && strcmp(argv[arg], "-ifp") != 0) {
		/* Validates the command-line arguments. */
		if (netutil_parse_ipv4(argv[arg++], &gateway) != 0) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		route.rt_flags |= RTF_GATEWAY;
	}

	/* Handles the selected command-line operation. */
	if (arg < argc && strcmp(argv[arg], "-ifp") == 0) {
		/* Validates the command-line arguments. */
		if (++arg >= argc) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		ifname = argv[arg++];
	}

	/* Validates the command-line arguments. */
	if (arg != argc || (add && (route.rt_flags & RTF_GATEWAY) != 0 &&
			    gateway.s_addr == 0)) {
		close(descriptor);

		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles a failed netutil ifindex operation. */
	if (ifname != NULL &&
	    netutil_ifindex(descriptor, ifname, &route.rt_ifindex) != 0) {
		printf("route: %s: %s\n", ifname, strerror(errno));
		close(descriptor);

		/* Reports operation failure. */
		return 1;
	}
	set_address(&route.rt_dst, destination.s_addr);
	set_address(&route.rt_genmask, mask.s_addr);
	set_address(&route.rt_gateway, gateway.s_addr);

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, add ? SIOCADDRT : SIOCDELRT, &route) != 0) {
		printf("route: %s\n", strerror(errno));
		close(descriptor);

		/* Reports operation failure. */
		return 1;
	}
	close(descriptor);

	/* Reports successful completion. */
	return 0;
}

/* Supports the show routes operation. */
static int
show_routes(
	int descriptor)
{
	char base[16];
	struct rtentry route;
	char destination[20], gateway[16], name[IFNAMSIZ];
	unsigned prefix;
	struct in_addr address;
	struct in_addr mask;
	struct in_addr next_hop;

	puts("Destination       Gateway         Flags   Netif");

	/* Process each remaining element. */
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		address.s_addr = get_address(&route.rt_dst);
		mask.s_addr = get_address(&route.rt_genmask);
		next_hop.s_addr = get_address(&route.rt_gateway);
		(void)netutil_mask_prefix(mask, &prefix);

		/* Handles the address condition. */
		if (address.s_addr == 0 && mask.s_addr == 0) {
			strcpy(destination, "default");
		} else {
			inet_ntop(AF_INET, &address, base, sizeof(base));
			snprintf(destination, sizeof(destination), "%s/%u",
				 base, prefix);
		}

		/* Handles the next hop condition. */
		if (next_hop.s_addr == 0)
			strcpy(gateway, "link");
		else
			inet_ntop(AF_INET, &next_hop, gateway, sizeof(gateway));

		/* Handles a failed netutil ifname operation. */
		if (netutil_ifname(descriptor, route.rt_ifindex, name) != 0)
			strcpy(name, "?");
		printf("%-17s %-15s U%s%s%s%s %s\n", destination, gateway,
		       (route.rt_flags & RTF_GATEWAY) ? "G" : "",
		       (route.rt_flags & RTF_HOST) ? "H" : "",
		       (route.rt_flags & RTF_DYNAMIC) ? "D" : "",
		       (route.rt_flags & RTF_CONNECTED) ? "C" : "", name);
	}

	/* Returns the computed result. */
	return errno == ENOENT ? 0 : 1;
}

/* Supports the get address operation. */
static uint32_t
get_address(
	const struct sockaddr *sa)
{
	/* Returns the computed result. */
	return ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	puts("usage: route [show]|route add|delete default gateway [-ifp "
	     "if]|route add|delete -net network/prefix [gateway|-interface "
	     "if]|route add|delete -host host [gateway]|route -6 [show]|"
	     "route -6 add|delete default|prefix/length [gateway] [-ifp if]");

	/* Reports operation failure. */
	return 2;
}

/* Supports the set address operation. */
static void
set_address(
	struct sockaddr *sa,
	uint32_t value)
{
	struct sockaddr_in *inet;

	inet = (struct sockaddr_in *)sa;
	memset(sa, 0, sizeof(*sa));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = value;
}

/*
 * The IPv6 routes (ws130-p005): "route -6 [show]", and "route -6
 * add|delete default|PREFIX/LENGTH [GATEWAY] [-ifp INTERFACE]" (a
 * link-local gateway needs its interface).
 */
static int
route6_main(
	int descriptor,
	int argc,
	char **argv)
{
	int add;
	int same;
	int status;

	/* The table. */
	if (argc == 1) {
		status = route6_show(descriptor);
		return status;
	}
	same = strcmp(argv[1], "show");
	if (argc == 2 && same == 0) {
		status = route6_show(descriptor);
		return status;
	}

	/* A route added or removed. */
	add = strcmp(argv[1], "add") == 0;
	same = strcmp(argv[1], "delete");
	if (argc < 3 || (!add && same != 0)) {
		status = usage();
		return status;
	}

	/* Succeeded or not, the change. */
	status = route6_change(descriptor, add, argc - 2, argv + 2);
	return status;
}

/* Shows the IPv6 routes: the destination and its length, the gateway, the flags and the interface. */
static int
route6_show(
	int descriptor)
{
	struct in6_rtentry route;
	char destination[INET6_ADDRSTRLEN + 5];
	char base[INET6_ADDRSTRLEN];
	char gateway[INET6_ADDRSTRLEN];
	char name[IFNAMSIZ];
	const char *written;
	int status;

	/* The header. */
	puts("Destination                    Gateway                        Flags   Netif");

	/* Each route, until the kernel says there is no more. */
	memset(&route, 0, sizeof(route));
	for (;;) {
		status = ioctl(descriptor, SIOCGRTENTRY_IN6, &route);
		if (status != 0)
			break;

		/* The destination: default, or the prefix and its length. */
		written = inet_ntop(AF_INET6, &route.rt6_dst, base, sizeof(base));
		if (written == NULL)
			strcpy(base, "?");
		if (route.rt6_prefixlen == 0U && IN6_IS_ADDR_UNSPECIFIED(&route.rt6_dst))
			strcpy(destination, "default");
		else
			snprintf(destination, sizeof(destination), "%s/%u", base, (unsigned)route.rt6_prefixlen);

		/* The gateway: the link when there is none. */
		strcpy(gateway, "link");
		if ((route.rt6_flags & RTF_GATEWAY) != 0U) {
			written = inet_ntop(AF_INET6, &route.rt6_gateway, gateway, sizeof(gateway));
			if (written == NULL)
				strcpy(gateway, "?");
		}

		/* The interface. */
		status = netutil_ifname(descriptor, route.rt6_ifindex, name);
		if (status != 0)
			strcpy(name, "?");
		printf("%-30s %-30s U%s%s%s%s %s\n", destination, gateway,
		       (route.rt6_flags & RTF_GATEWAY) ? "G" : "",
		       (route.rt6_flags & RTF_HOST) ? "H" : "",
		       (route.rt6_flags & RTF_DYNAMIC) ? "D" : "",
		       (route.rt6_flags & RTF_CONNECTED) ? "C" : "", name);
		route.rt6_index++;
	}

	/* Succeeded when the table ended. */
	if (errno != ENOENT)
		return 1;
	return 0;
}

/* Adds or removes an IPv6 route from its words: the destination, the gateway, and the interface after -ifp. */
static int
route6_change(
	int descriptor,
	int add,
	int argc,
	char **argv)
{
	struct in6_rtentry route;
	unsigned prefix;
	int arg;
	int same;
	int status;
	int ok;

	/* The destination: default, or a prefix. */
	memset(&route, 0, sizeof(route));
	route.rt6_flags = RTF_UP;
	if (add)
		route.rt6_flags |= RTF_STATIC;
	route.rt6_lifetime = IN6_LIFETIME_INFINITE;
	arg = 0;
	same = strcmp(argv[arg], "default");
	if (same != 0) {
		status = netutil_parse_cidr6(argv[arg], &route.rt6_dst, &prefix);
		if (status != 0)
			return usage();
		route.rt6_prefixlen = prefix;
	}
	arg++;

	/* The gateway, when one follows. */
	if (arg < argc) {
		same = strcmp(argv[arg], "-ifp");
		if (same != 0) {
			ok = inet_pton(AF_INET6, argv[arg], &route.rt6_gateway);
			if (ok != 1)
				return usage();
			route.rt6_flags |= RTF_GATEWAY;
			arg++;
		}
	}

	/* The interface after -ifp. */
	if (arg + 1 < argc) {
		same = strcmp(argv[arg], "-ifp");
		if (same != 0)
			return usage();
		status = netutil_ifindex(descriptor, argv[arg + 1], &route.rt6_ifindex);
		if (status != 0) {
			printf("route: %s: %s\n", argv[arg + 1], strerror(errno));
			return 1;
		}
		arg += 2;
	}
	if (arg != argc)
		return usage();

	/* The kernel's change. */
	status = ioctl(descriptor, add ? SIOCADDRT_IN6 : SIOCDELRT_IN6, &route);
	if (status != 0) {
		printf("route: %s\n", strerror(errno));
		return 1;
	}

	/* Succeeded. */
	return 0;
}
