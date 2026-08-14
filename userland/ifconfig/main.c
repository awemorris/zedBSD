/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int
request(int descriptor, const char *name, unsigned long command,
	struct ifreq *item)
{
	if (netutil_ifreq(item, name) != 0 || ioctl(descriptor, command, item) != 0) {
		printf("ifconfig: %s: %s\n", name, strerror(errno)); return -1;
	}
	return 0;
}

static const char *
address_text(const struct sockaddr *address, char output[16])
{
	const struct sockaddr_in *inet = (const struct sockaddr_in *)address;
	return inet_ntop(AF_INET, &inet->sin_addr, output, 16) != NULL ? output : "?";
}

static int
show(int descriptor, const char *name)
{
	struct ifreq flags, mtu, address, mask, broadcast, hardware, stats;
	char a[16], m[16], b[16];
	if (request(descriptor, name, SIOCGIFFLAGS, &flags) != 0 ||
	    request(descriptor, name, SIOCGIFMTU, &mtu) != 0 ||
	    request(descriptor, name, SIOCGIFHWADDR, &hardware) != 0 ||
	    request(descriptor, name, SIOCGIFSTATS, &stats) != 0) return -1;
	printf("%s: flags=", name);
	if ((flags.ifr_flags & IFF_UP) != 0) printf("UP,");
	if ((flags.ifr_flags & IFF_RUNNING) != 0) printf("RUNNING,");
	if ((flags.ifr_flags & IFF_BROADCAST) != 0) printf("BROADCAST,");
	if ((flags.ifr_flags & IFF_MULTICAST) != 0) printf("MULTICAST,");
	printf("0x%x mtu %d\n", flags.ifr_flags, mtu.ifr_mtu);
	if (request(descriptor, name, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0 &&
	    request(descriptor, name, SIOCGIFNETMASK, &mask) == 0 &&
	    request(descriptor, name, SIOCGIFBRDADDR, &broadcast) == 0)
		printf("        inet %s netmask %s broadcast %s\n",
		    address_text(&address.ifr_addr, a), address_text(&mask.ifr_addr, m),
		    address_text(&broadcast.ifr_addr, b));
	printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
	    hardware.ifr_hwaddr[0], hardware.ifr_hwaddr[1], hardware.ifr_hwaddr[2],
	    hardware.ifr_hwaddr[3], hardware.ifr_hwaddr[4], hardware.ifr_hwaddr[5]);
	printf("        RX packets %llu bytes %llu errors %llu dropped %llu\n",
	    (unsigned long long)stats.ifr_data.ifi_ipackets,
	    (unsigned long long)stats.ifr_data.ifi_ibytes,
	    (unsigned long long)stats.ifr_data.ifi_ierrors,
	    (unsigned long long)stats.ifr_data.ifi_iqdrops);
	printf("        TX packets %llu bytes %llu errors %llu dropped %llu\n",
	    (unsigned long long)stats.ifr_data.ifi_opackets,
	    (unsigned long long)stats.ifr_data.ifi_obytes,
	    (unsigned long long)stats.ifr_data.ifi_oerrors,
	    (unsigned long long)stats.ifr_data.ifi_oqdrops);
	return 0;
}

static int
set_sockaddr(int descriptor, const char *name, unsigned long command,
	struct in_addr value)
{
	struct ifreq request_;
	struct sockaddr_in *inet;
	if (netutil_ifreq(&request_, name) != 0) return -1;
	inet = (struct sockaddr_in *)&request_.ifr_addr;
	inet->sin_family = AF_INET; inet->sin_addr = value;
	return ioctl(descriptor, command, &request_);
}

static int usage(void)
{
	puts("usage: ifconfig [-a] [interface [up|down|inet address[/prefix] [netmask mask]|broadcast address]]]");
	return 2;
}

int
main(int argc, char **argv)
{
	struct ifreq *interfaces, request_;
	unsigned count, index;
	int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), status = 0;
	if (descriptor < 0) { printf("ifconfig: socket: %s\n", strerror(errno)); return 1; }
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "-a") == 0)) {
		if (netutil_interfaces(descriptor, &interfaces, &count) != 0) status = 1;
		else { for (index = 0; index < count; index++) if (show(descriptor, interfaces[index].ifr_name) != 0) status = 1; free(interfaces); }
		close(descriptor); return status;
	}
	if (argc == 2) { status = show(descriptor, argv[1]) != 0; close(descriptor); return status; }
	if (argc == 3 && (strcmp(argv[2], "up") == 0 || strcmp(argv[2], "down") == 0)) {
		if (request(descriptor, argv[1], SIOCGIFFLAGS, &request_) != 0) status = 1;
		else { if (strcmp(argv[2], "up") == 0) request_.ifr_flags |= IFF_UP; else request_.ifr_flags &= ~IFF_UP; if (ioctl(descriptor, SIOCSIFFLAGS, &request_) != 0) status = 1; }
	} else if (argc >= 4 && strcmp(argv[2], "inet") == 0) {
		struct in_addr address, mask, broadcast; unsigned prefix;
		if (strchr(argv[3], '/') != NULL) {
			if (netutil_parse_cidr(argv[3], &address, &mask, &prefix) != 0 || argc != 4) { close(descriptor); return usage(); }
			broadcast.s_addr = address.s_addr | ~mask.s_addr;
		} else {
			if (netutil_parse_ipv4(argv[3], &address) != 0 || argc != 6 || strcmp(argv[4], "netmask") != 0 || netutil_parse_ipv4(argv[5], &mask) != 0 || netutil_mask_prefix(mask, &prefix) != 0) { close(descriptor); return usage(); }
			broadcast.s_addr = address.s_addr | ~mask.s_addr;
		}
		if (set_sockaddr(descriptor, argv[1], SIOCSIFNETMASK, mask) != 0 || set_sockaddr(descriptor, argv[1], SIOCSIFBRDADDR, broadcast) != 0 || set_sockaddr(descriptor, argv[1], SIOCSIFADDR, address) != 0) status = 1;
	} else if (argc == 4 && strcmp(argv[2], "broadcast") == 0) {
		struct in_addr broadcast;
		if (netutil_parse_ipv4(argv[3], &broadcast) != 0) { close(descriptor); return usage(); }
		status = set_sockaddr(descriptor, argv[1], SIOCSIFBRDADDR, broadcast) != 0;
	} else { close(descriptor); return usage(); }
	if (status) printf("ifconfig: %s: %s\n", argv[1], strerror(errno));
	close(descriptor); return status;
}
