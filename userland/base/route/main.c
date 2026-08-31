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
		if (address.s_addr == 0 && mask.s_addr == 0)
			strcpy(destination, "default");
		else {

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
	     "if]|route add|delete -host host [gateway]");

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
