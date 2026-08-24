/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/route.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static void
set_address(struct sockaddr *sa, uint32_t value)
{
	struct sockaddr_in *inet = (struct sockaddr_in *)sa;
	memset(sa, 0, sizeof(*sa));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = value;
}
static uint32_t
get_address(const struct sockaddr *sa)
{
	return ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
}

static int
show_routes(int descriptor)
{
	struct rtentry route;
	char destination[20], gateway[16], name[IFNAMSIZ];
	unsigned prefix;
	puts("Destination       Gateway         Flags   Netif");
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		struct in_addr addr = {get_address(&route.rt_dst)};
		struct in_addr mask = {get_address(&route.rt_genmask)};
		struct in_addr gw = {get_address(&route.rt_gateway)};
		(void)netutil_mask_prefix(mask, &prefix);
		if (addr.s_addr == 0 && mask.s_addr == 0)
			strcpy(destination, "default");
		else {
			char base[16];
			inet_ntop(AF_INET, &addr, base, sizeof(base));
			snprintf(destination, sizeof(destination), "%s/%u",
				 base, prefix);
		}
		if (gw.s_addr == 0)
			strcpy(gateway, "link");
		else
			inet_ntop(AF_INET, &gw, gateway, sizeof(gateway));
		if (netutil_ifname(descriptor, route.rt_ifindex, name) != 0)
			strcpy(name, "?");
		printf("%-17s %-15s U%s%s%s%s %s\n", destination, gateway,
		       (route.rt_flags & RTF_GATEWAY) ? "G" : "",
		       (route.rt_flags & RTF_HOST) ? "H" : "",
		       (route.rt_flags & RTF_DYNAMIC) ? "D" : "",
		       (route.rt_flags & RTF_CONNECTED) ? "C" : "", name);
	}
	return errno == ENOENT ? 0 : 1;
}

static int
usage(void)
{
	puts("usage: route [show]|route add|delete default gateway [-ifp "
	     "if]|route add|delete -net network/prefix [gateway|-interface "
	     "if]|route add|delete -host host [gateway]");
	return 2;
}

int
main(int argc, char **argv)
{
	struct rtentry route;
	struct in_addr destination = {0}, mask = {0}, gateway = {0};
	const char *ifname = NULL;
	unsigned prefix = 0;
	int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), add, arg = 2;
	if (descriptor < 0)
		return 1;
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "show") == 0) ||
	    (argc == 3 && strcmp(argv[1], "-n") == 0 &&
	     strcmp(argv[2], "show") == 0)) {
		int status = show_routes(descriptor);
		close(descriptor);
		return status;
	}
	if (argc < 3 || ((add = strcmp(argv[1], "add") == 0) == 0 &&
			 strcmp(argv[1], "delete") != 0)) {
		close(descriptor);
		return usage();
	}
	memset(&route, 0, sizeof(route));
	route.rt_flags = RTF_UP | (add ? RTF_STATIC : 0);
	if (strcmp(argv[arg], "default") == 0) {
		arg++;
	} else if (strcmp(argv[arg], "-net") == 0 && ++arg < argc) {
		uint32_t original;
		if (netutil_parse_cidr(argv[arg++], &destination, &mask,
				       &prefix) != 0) {
			close(descriptor);
			return usage();
		}
		original = destination.s_addr;
		destination.s_addr &= mask.s_addr;
		if (original != destination.s_addr) {
			puts("route: destination contains host bits");
			close(descriptor);
			return 1;
		}
	} else if (strcmp(argv[arg], "-host") == 0 && ++arg < argc) {
		if (netutil_parse_ipv4(argv[arg++], &destination) != 0) {
			close(descriptor);
			return usage();
		}
		mask.s_addr = 0xffffffffU;
		route.rt_flags |= RTF_HOST;
	} else {
		close(descriptor);
		return usage();
	}
	if (arg < argc && strcmp(argv[arg], "-interface") == 0) {
		if (++arg >= argc) {
			close(descriptor);
			return usage();
		}
		ifname = argv[arg++];
	} else if (arg < argc && strcmp(argv[arg], "-ifp") != 0) {
		if (netutil_parse_ipv4(argv[arg++], &gateway) != 0) {
			close(descriptor);
			return usage();
		}
		route.rt_flags |= RTF_GATEWAY;
	}
	if (arg < argc && strcmp(argv[arg], "-ifp") == 0) {
		if (++arg >= argc) {
			close(descriptor);
			return usage();
		}
		ifname = argv[arg++];
	}
	if (arg != argc || (add && (route.rt_flags & RTF_GATEWAY) != 0 &&
			    gateway.s_addr == 0)) {
		close(descriptor);
		return usage();
	}
	if (ifname != NULL &&
	    netutil_ifindex(descriptor, ifname, &route.rt_ifindex) != 0) {
		printf("route: %s: %s\n", ifname, strerror(errno));
		close(descriptor);
		return 1;
	}
	set_address(&route.rt_dst, destination.s_addr);
	set_address(&route.rt_genmask, mask.s_addr);
	set_address(&route.rt_gateway, gateway.s_addr);
	if (ioctl(descriptor, add ? SIOCADDRT : SIOCDELRT, &route) != 0) {
		printf("route: %s\n", strerror(errno));
		close(descriptor);
		return 1;
	}
	close(descriptor);
	return 0;
}
