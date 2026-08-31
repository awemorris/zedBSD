/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ifconfig userland command.
 */

#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int show(int descriptor, const char *name);
static int request(int descriptor, const char *name, unsigned long command, struct ifreq *item);
static const char *address_text(const struct sockaddr *address, char output[16]);
static int usage(void);
static int set_sockaddr(int descriptor, const char *name, unsigned long command, struct in_addr value);

/*
 * Runs the ifconfig command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	struct in_addr address_local, mask_local, broadcast_local;
	struct in_addr broadcast_local1;
	unsigned prefix;
	struct ifreq *interfaces, request_;
	unsigned count, index;
	int descriptor, status;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	status = 0;

	/* Checks the file descriptor. */
	if (descriptor < 0) {
		printf("ifconfig: socket: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the selected command-line operation. */
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "-a") == 0)) {
		/* Handles a failed netutil interfaces operation. */
		if (netutil_interfaces(descriptor, &interfaces, &count) != 0)
			status = 1;
		else {
			/* Process each remaining element. */
			for (index = 0; index < count; index++)

				/* Handles a failed show operation. */
				if (show(descriptor,
					 interfaces[index].ifr_name) != 0)
					status = 1;
			free(interfaces);
		}
		close(descriptor);

		/* Returns the computed result. */
		return status;
	}

	/* Validates the command-line arguments. */
	if (argc == 2) {
		status = show(descriptor, argv[1]) != 0;
		close(descriptor);

		/* Returns the computed result. */
		return status;
	}

	/* Handles the selected command-line operation. */
	if (argc == 3 &&
	    (strcmp(argv[2], "up") == 0 || strcmp(argv[2], "down") == 0)) {
		/* Validates the command-line arguments. */
		if (request(descriptor, argv[1], SIOCGIFFLAGS, &request_) != 0)
			status = 1;
		else {
			/* Handles the selected command-line operation. */
			if (strcmp(argv[2], "up") == 0)
				request_.ifr_flags |= IFF_UP;
			else
				request_.ifr_flags &= ~IFF_UP;

			/* Handles a failed ioctl operation. */
			if (ioctl(descriptor, SIOCSIFFLAGS, &request_) != 0)
				status = 1;
		}
	} else if (argc >= 4 && strcmp(argv[2], "inet") == 0) {
		/* Validates the command-line arguments. */
		if (strchr(argv[3], '/') != NULL) {
			/* Validates the command-line arguments. */
			if (netutil_parse_cidr(argv[3], &address_local, &mask_local,
					       &prefix) != 0 ||
			    argc != 4) {
				close(descriptor);

				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			broadcast_local.s_addr = address_local.s_addr | ~mask_local.s_addr;
		} else {
			/* Handles the selected command-line operation. */
			if (netutil_parse_ipv4(argv[3], &address_local) != 0 ||
			    argc != 6 || strcmp(argv[4], "netmask") != 0 ||
			    netutil_parse_ipv4(argv[5], &mask_local) != 0 ||
			    netutil_mask_prefix(mask_local, &prefix) != 0) {
				close(descriptor);

				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			broadcast_local.s_addr = address_local.s_addr | ~mask_local.s_addr;
		}

		/* Validates the command-line arguments. */
		if (set_sockaddr(descriptor, argv[1], SIOCSIFNETMASK, mask_local) !=
			0 ||
		    set_sockaddr(descriptor, argv[1], SIOCSIFBRDADDR,
				 broadcast_local) != 0 ||
		    set_sockaddr(descriptor, argv[1], SIOCSIFADDR, address_local) !=
			0)
			status = 1;
	} else if (argc == 4 && strcmp(argv[2], "broadcast") == 0) {
		/* Validates the command-line arguments. */
		if (netutil_parse_ipv4(argv[3], &broadcast_local1) != 0) {
			close(descriptor);

			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		status = set_sockaddr(descriptor, argv[1], SIOCSIFBRDADDR,
				      broadcast_local1) != 0;
	} else {
		close(descriptor);

		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation status. */
	if (status)
		printf("ifconfig: %s: %s\n", argv[1], strerror(errno));
	close(descriptor);

	/* Returns the computed result. */
	return status;
}

/* Supports the show operation. */
static int
show(
	int descriptor,
	const char *name)
{
	struct ifreq flags, mtu, address, mask, broadcast, hardware, stats;
	char a[16], m[16], b[16];

	/* Handles a failed request operation. */
	if (request(descriptor, name, SIOCGIFFLAGS, &flags) != 0 ||
	    request(descriptor, name, SIOCGIFMTU, &mtu) != 0 ||
	    request(descriptor, name, SIOCGIFHWADDR, &hardware) != 0 ||
	    request(descriptor, name, SIOCGIFSTATS, &stats) != 0)

		/* Reports operation failure. */
		return -1;
	printf("%s: flags=", name);

	/* Checks the active flags. */
	if ((flags.ifr_flags & IFF_UP) != 0)
		printf("UP,");

	/* Checks the active flags. */
	if ((flags.ifr_flags & IFF_RUNNING) != 0)
		printf("RUNNING,");

	/* Checks the active flags. */
	if ((flags.ifr_flags & IFF_BROADCAST) != 0)
		printf("BROADCAST,");

	/* Checks the active flags. */
	if ((flags.ifr_flags & IFF_MULTICAST) != 0)
		printf("MULTICAST,");
	printf("0x%x mtu %d\n", flags.ifr_flags, mtu.ifr_mtu);

	/* Handles a failed request operation. */
	if (request(descriptor, name, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0 &&
	    request(descriptor, name, SIOCGIFNETMASK, &mask) == 0 &&
	    request(descriptor, name, SIOCGIFBRDADDR, &broadcast) == 0)
		printf("        inet %s netmask %s broadcast %s\n",
		       address_text(&address.ifr_addr, a),
		       address_text(&mask.ifr_addr, m),
		       address_text(&broadcast.ifr_addr, b));
	printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
	       hardware.ifr_hwaddr[0], hardware.ifr_hwaddr[1],
	       hardware.ifr_hwaddr[2], hardware.ifr_hwaddr[3],
	       hardware.ifr_hwaddr[4], hardware.ifr_hwaddr[5]);
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

	/* Reports successful completion. */
	return 0;
}

/* Supports the request operation. */
static int
request(
	int descriptor,
	const char *name,
	unsigned long command,
	struct ifreq *item)
{
	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(item, name) != 0 ||
	    ioctl(descriptor, command, item) != 0) {
		printf("ifconfig: %s: %s\n", name, strerror(errno));

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the address text operation. */
static const char *
address_text(
	const struct sockaddr *address,
	char output[16])
{
	const char *function_result;
	const struct sockaddr_in *inet;

	inet = (const struct sockaddr_in *)address;

	/* Computes the function result. */
	function_result = inet_ntop(AF_INET, &inet->sin_addr, output, 16) != NULL ? output
								       : "?";

	/* Returns the computed result. */
	return function_result;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	puts("usage: ifconfig [-a] [interface [up|down|inet address[/prefix] "
	     "[netmask mask]|broadcast address]]]");

	/* Reports operation failure. */
	return 2;
}

/* Supports the set sockaddr operation. */
static int
set_sockaddr(
	int descriptor,
	const char *name,
	unsigned long command,
	struct in_addr value)
{
	int function_result;
	struct ifreq request_;
	struct sockaddr_in *inet;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&request_, name) != 0)
		return -1;
	inet = (struct sockaddr_in *)&request_.ifr_addr;
	inet->sin_family = AF_INET;
	inet->sin_addr = value;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, command, &request_);

	/* Returns the computed result. */
	return function_result;
}
