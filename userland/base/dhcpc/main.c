/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD dhcpc userland command.
 */

#include "userland/base/net/dhcp.h"
#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/route.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct snapshot {
	int flags;
	struct in_addr address, mask, broadcast;
};

#define CARRIER_POLL_INTERVAL_US 10000U

static volatile sig_atomic_t timed_out;

static int usage(void);
static int select_interface(int descriptor, char name[IFNAMSIZ]);
static int if_request(int descriptor, const char *name, unsigned long command, struct ifreq *request);
static int prepare_interface(int descriptor, const char *name, uint64_t deadline, struct snapshot *saved, int *rollback_error);
static int snapshot_interface(int descriptor, const char *name, struct snapshot *saved);
static int get_address(int descriptor, const char *name, unsigned long command, struct in_addr *address);
static int wait_for_carrier(int descriptor, const char *name, uint64_t deadline);
static int set_address(int descriptor, const char *name, unsigned long command, struct in_addr address);
static int restore_interface(int descriptor, const char *name, const struct snapshot *saved);
static int find_interface_default(int descriptor, uint32_t ifindex, struct rtentry *saved);
static int delete_interface_default(int descriptor, uint32_t ifindex);
static uint32_t transaction_id(const uint8_t mac[6]);
static int deadline_check(uint64_t deadline);
static int set_receive_deadline(int descriptor, uint64_t deadline);
static void sockaddr_value(struct sockaddr *address, uint32_t value);
static int write_resolver(const char *interface, const struct dhcp_lease *lease);
static void timeout_handler(int signal_number);

/*
 * Runs the dhcpc command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	int saved_errno;
	socklen_t slen_local;
	ssize_t count_local;
	socklen_t slen_local1;
	ssize_t count_local2;
	struct in_addr value_local;
	char *end;
	unsigned long v;
	int enabled;
	struct rtentry route;
	unsigned prefix;
	unsigned i;
	int error;
	struct snapshot saved;
	struct rtentry previous_default;
	struct ifreq request;
	struct sockaddr_in client, server, source;
	struct dhcp_lease offer, lease;
	uint8_t packet[600], mac[6];
	char interface[IFNAMSIZ] = {0}, text[16];
	uint32_t xid, timeout_seconds, ifindex;
	uint64_t deadline;
	size_t packet_length;
	const char *failure_stage;
	int control, socket_, verbose, arg, got_offer;
	int got_ack, interface_prepared;
	int previous_default_present, route_prepared, rollback_error;

	timeout_seconds = 15;
	failure_stage = "interface";
	socket_ = -1;
	verbose = 0;
	arg = 1;
	got_offer = 0;
	got_ack = 0;
	interface_prepared = 0;
	previous_default_present = 0;
	route_prepared = 0;
	rollback_error = 0;

	/* Process each remaining command-line operand. */
	while (arg < argc && argv[arg][0] == '-') {
		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], "-v") == 0) {
			verbose = 1;
			arg++;
		} else if (strcmp(argv[arg], "-t") == 0 && arg + 1 < argc) {

						v = strtoul(argv[arg + 1], &end, 10);

			/* Checks the current endpoint. */
			if (*end != '\0' || v == 0 || v > 3600U) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			timeout_seconds = (uint32_t)v;
			arg += 2;
		} else {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Validates the command-line arguments. */
	if (arg + 1 < argc) {
		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	/* Validates the command-line arguments. */
	if (arg < argc) {
		/* Validates the command-line arguments. */
		if (strlen(argv[arg]) >= IFNAMSIZ) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		strcpy(interface, argv[arg]);
	}
	control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	/* Handles the control condition. */
	if (control < 0) {
		printf("dhcpc: socket: %s\n", strerror(errno));

		/* Reports operation failure. */
		return 1;
	}

	/* Handles a failed select interface operation. */
	if (interface[0] == '\0' && select_interface(control, interface) != 0) {
		printf("dhcpc: select interface: %s\n", strerror(errno));
		close(control);

		/* Reports operation failure. */
		return 1;
	}
	memset(&previous_default, 0, sizeof(previous_default));
	deadline =
	    netutil_monotonic_us() + (uint64_t)timeout_seconds * 1000000U;
	timed_out = 0;
	(void)signal(SIGALRM, timeout_handler);
	(void)alarm(timeout_seconds);
	failure_stage = "carrier";

	/* Handles an operation failure. */
	if (prepare_interface(control, interface, deadline, &saved,
	    &rollback_error) != 0)
		goto fail;
	interface_prepared = 1;
	failure_stage = "identity";

	/* Handles a failed if request operation. */
	if (if_request(control, interface, SIOCGIFHWADDR, &request) != 0 ||
	    netutil_ifindex(control, interface, &ifindex) != 0)
		goto fail;
	memcpy(mac, request.ifr_hwaddr, sizeof(mac));
	failure_stage = "route";
	previous_default_present =
	    find_interface_default(control, ifindex, &previous_default);

	/* Handles the previous default present condition. */
	if (previous_default_present < 0)
		goto fail;
	route_prepared = 1;

	/* Handles a failed delete interface default operation. */
	if (delete_interface_default(control, ifindex) != 0)
		goto fail;
	failure_stage = "socket";
	socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	/* Handles the socket condition. */
	if (socket_ < 0)
		goto fail;

	enabled = 1;

	/* Handles a failed setsockopt operation. */
	if (setsockopt(socket_, SOL_SOCKET, SO_BINDTODEVICE, interface,
		       (socklen_t)strlen(interface) + 1U) != 0 ||
	    setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &enabled,
		       sizeof(enabled)) != 0)
		goto socket_fail;
	memset(&client, 0, sizeof(client));
	client.sin_family = AF_INET;
	client.sin_port = htons(68);
	client.sin_addr.s_addr = INADDR_ANY;

	/* Handles a failed bind operation. */
	if (bind(socket_, (struct sockaddr *)&client, sizeof(client)) != 0)
		goto socket_fail;
	memset(&server, 0, sizeof(server));

	/* Continue while the operation condition remains true. */
	server.sin_family = AF_INET;
	server.sin_port = htons(67);
	server.sin_addr.s_addr = INADDR_BROADCAST;
	xid = transaction_id(mac);
	failure_stage = "offer";
	while (!timed_out && netutil_monotonic_us() < deadline && !got_offer) {
		/* Handles a failed deadline check operation. */
		if (deadline_check(deadline) != 0 ||
		    dhcp_build(packet, sizeof(packet), &packet_length,
			       DHCP_DISCOVER, xid, mac, 0, 0) != 0 ||
		    sendto(socket_, packet, packet_length, 0,
			   (struct sockaddr *)&server, sizeof(server)) < 0)
			goto socket_fail;

		/* Handles the verbose condition. */
		if (verbose)
			printf("dhcpc: %s: broadcasting discover\n", interface);

		/* Continue until the operation reaches a terminal state. */
		for (;;) {

			slen_local = sizeof(source);

			/* Handles a failed set receive deadline operation. */
			if (set_receive_deadline(socket_, deadline) != 0)
				goto socket_fail;
			count_local = recvfrom(socket_, packet, sizeof(packet), 0,
				     (struct sockaddr *)&source, &slen_local);

			/* Handles the reported system error. */
			if (count_local < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK ||
			     errno == EINTR))
				break;

			/* Handles the count local condition. */
			if (count_local < 0)
				goto socket_fail;

			/* Handles a failed htons operation. */
			if (source.sin_port == htons(67) &&
			    dhcp_parse(packet, (size_t)count_local, xid, mac,
				       &offer) == 0 &&
			    offer.message_type == DHCP_OFFER &&
			    offer.server_identifier != 0) {
				got_offer = 1;
				break;
			}
		}
	}

	/* Handles the got offer condition. */
	if (!got_offer) {
		errno = ETIMEDOUT;
		goto socket_fail;
	}
		value_local.s_addr = offer.address;
		inet_ntop(AF_INET, &value_local, text, sizeof(text));
	printf("dhcpc: %s: offered %s", interface, text);
		value_local.s_addr = offer.server_identifier;
		inet_ntop(AF_INET, &value_local, text, sizeof(text));
	printf(" by %s\n", text);

	/* Continue while the operation condition remains true. */
	failure_stage = "ack";
	while (!timed_out && netutil_monotonic_us() < deadline && !got_ack) {
		/* Handles a failed deadline check operation. */
		if (deadline_check(deadline) != 0 ||
		    dhcp_build(packet, sizeof(packet), &packet_length,
			       DHCP_REQUEST, xid, mac, offer.address,
			       offer.server_identifier) != 0 ||
		    sendto(socket_, packet, packet_length, 0,
			   (struct sockaddr *)&server, sizeof(server)) < 0)
			goto socket_fail;

		/* Continue until the operation reaches a terminal state. */
		for (;;) {

			slen_local1 = sizeof(source);

			/* Handles a failed set receive deadline operation. */
			if (set_receive_deadline(socket_, deadline) != 0)
				goto socket_fail;
			count_local2 = recvfrom(socket_, packet, sizeof(packet), 0,
				     (struct sockaddr *)&source, &slen_local1);

			/* Handles the reported system error. */
			if (count_local2 < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK ||
			     errno == EINTR))
				break;

			/* Handles the count local2 condition. */
			if (count_local2 < 0)
				goto socket_fail;

			/* Handles a failed htons operation. */
			if (source.sin_port == htons(67) &&
			    dhcp_parse(packet, (size_t)count_local2, xid, mac,
				       &lease) == 0) {
				/* Handles the lease condition. */
				if (lease.message_type == DHCP_NAK) {
					errno = EACCES;
					goto socket_fail;
				}

				/* Handles the lease condition. */
				if (lease.message_type == DHCP_ACK &&
				    lease.netmask != 0 &&
				    lease.server_identifier ==
					offer.server_identifier) {
					got_ack = 1;
					break;
				}
			}
		}
	}

	/* Handles the got ack condition. */
	if (!got_ack) {
		errno = ETIMEDOUT;
		goto socket_fail;
	}
	failure_stage = "configuration";

	value_local.s_addr = lease.netmask;

	/* Handles a failed set address operation. */
	if (set_address(control, interface, SIOCSIFNETMASK, value_local) != 0)
		goto socket_fail;
	value_local.s_addr = lease.broadcast;

	/* Handles a failed set address operation. */
	if (set_address(control, interface, SIOCSIFBRDADDR, value_local) != 0)
		goto socket_fail;
	value_local.s_addr = lease.address;

	/* Handles a failed set address operation. */
	if (set_address(control, interface, SIOCSIFADDR, value_local) != 0)
		goto socket_fail;
	failure_stage = "route";

	/* Handles the lease condition. */
	if (lease.router_count != 0) {

		memset(&route, 0, sizeof(route));
		route.rt_flags = RTF_UP | RTF_GATEWAY | RTF_DYNAMIC;
		route.rt_ifindex = ifindex;
		sockaddr_value(&route.rt_dst, 0);
		sockaddr_value(&route.rt_genmask, 0);
		sockaddr_value(&route.rt_gateway, lease.routers[0]);

		/* Handles a failed ioctl operation. */
		if (ioctl(control, SIOCADDRT, &route) != 0)
			goto socket_fail;
	}
	prefix = 0;
		value_local.s_addr = lease.netmask;
		netutil_mask_prefix(value_local, &prefix);
		value_local.s_addr = lease.address;
		inet_ntop(AF_INET, &value_local, text, sizeof(text));
	printf("dhcpc: %s: address %s/%u\n", interface, text, prefix);

	/* Handles the lease condition. */
	if (lease.router_count != 0) {
		value_local.s_addr = lease.routers[0];
		inet_ntop(AF_INET, &value_local, text, sizeof(text));
		printf("dhcpc: %s: default route %s\n", interface, text);
	}

	/* Handles the lease condition. */
	if (lease.dns_count != 0) {
		/* Process each remaining element. */
		for (i = 0; i < lease.dns_count; i++) {
			value_local.s_addr = lease.dns_servers[i];
			inet_ntop(AF_INET, &value_local, text, sizeof(text));
			printf("dhcpc: %s: dns %s\n", interface, text);
		}
		failure_stage = "resolver";

		/* Handles a failed write resolver operation. */
		if (write_resolver(interface, &lease) != 0)
			goto socket_fail;
	}
	printf("dhcpc: %s: lease %u seconds\n", interface, lease.lease_time);
	(void)alarm(0);
	close(socket_);
	close(control);

	/* Reports successful completion. */
	return 0;
socket_fail:
fail:
				saved_errno = errno != 0 ? errno : EIO;

	(void)alarm(0);

	/* Handles the socket condition. */
	if (socket_ >= 0)
		close(socket_);

	/* Handles the route prepared condition. */
	if (route_prepared) {
		/* Handles an operation failure. */
		if (delete_interface_default(control, ifindex) != 0 &&
		    rollback_error == 0)
			rollback_error = errno;

		/* Handles an operation failure. */
		if (previous_default_present > 0 &&
		    ioctl(control, SIOCADDRT, &previous_default) != 0 &&
		    rollback_error == 0)
			rollback_error = errno;
	}

	/* Handles an operation failure. */
	if (interface_prepared &&
	    restore_interface(control, interface, &saved) != 0 &&
	    rollback_error == 0)
		rollback_error = errno;
	printf("dhcpc: %s: %s: %s\n", interface, failure_stage,
	       strerror(saved_errno));

	/* Handles an operation failure. */
	if (rollback_error != 0) {
		error = rollback_error;
		printf("dhcpc: %s: rollback: %s\n", interface,
		       strerror(error));
	}
	close(control);
	errno = saved_errno;

	/* Reports operation failure. */
	return 1;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	puts("usage: dhcpc [-v] [-t seconds] [interface]");

	/* Reports operation failure. */
	return 2;
}

/* Supports the select interface operation. */
static int
select_interface(
	int descriptor,
	char name[IFNAMSIZ])
{
	struct ifreq *items, request;
	unsigned count, index, candidates;

	candidates = 0;

	/* Handles a failed netutil interfaces operation. */
	if (netutil_interfaces(descriptor, &items, &count) != 0)
		return -1;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles a failed if request operation. */
		if (if_request(descriptor, items[index].ifr_name, SIOCGIFFLAGS,
			       &request) == 0 &&
		    (request.ifr_flags & IFF_BROADCAST) != 0) {
			strncpy(name, items[index].ifr_name, IFNAMSIZ - 1U);
			candidates++;
		}
	}
	free(items);

	/* Handles the candidates condition. */
	if (candidates != 1U) {
		errno = candidates == 0 ? ENODEV : EBUSY;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the if request operation. */
static int
if_request(
	int descriptor,
	const char *name,
	unsigned long command,
	struct ifreq *request)
{
	int function_result;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(request, name) != 0)
		return -1;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, command, request);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the prepare interface operation. */
static int
prepare_interface(
	int descriptor,
	const char *name,
	uint64_t deadline,
	struct snapshot *saved,
	int *rollback_error)
{
	struct ifreq request;
	struct in_addr zero = {0};
	int saved_errno;

	/* Handles a failed snapshot interface operation. */
	if (snapshot_interface(descriptor, name, saved) != 0)
		return -1;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&request, name) != 0)
		return -1;
	request.ifr_flags = saved->flags | IFF_UP;

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, SIOCSIFFLAGS, &request) != 0)
		return -1;

	/* Handles a failed wait for carrier operation. */
	if (wait_for_carrier(descriptor, name, deadline) == 0 &&
	    set_address(descriptor, name, SIOCSIFADDR, zero) == 0 &&
	    set_address(descriptor, name, SIOCSIFNETMASK, zero) == 0 &&
	    set_address(descriptor, name, SIOCSIFBRDADDR, zero) == 0)

		/* Reports successful completion. */
		return 0;

	saved_errno = errno;

	/* Handles an operation failure. */
	if (restore_interface(descriptor, name, saved) != 0 &&
	    rollback_error != NULL)
		*rollback_error = errno;
	errno = saved_errno;

	/* Reports operation failure. */
	return -1;
}

/* Supports the snapshot interface operation. */
static int
snapshot_interface(
	int descriptor,
	const char *name,
	struct snapshot *saved)
{
	struct ifreq request;

	memset(saved, 0, sizeof(*saved));

	/* Handles a failed if request operation. */
	if (if_request(descriptor, name, SIOCGIFFLAGS, &request) != 0)
		return -1;
	saved->flags = request.ifr_flags;

	/* Handles a failed get address operation. */
	if (get_address(descriptor, name, SIOCGIFADDR, &saved->address) != 0 ||
	    get_address(descriptor, name, SIOCGIFNETMASK, &saved->mask) != 0 ||
	    get_address(descriptor, name, SIOCGIFBRDADDR, &saved->broadcast) != 0)

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the get address operation. */
static int
get_address(
	int descriptor,
	const char *name,
	unsigned long command,
	struct in_addr *address)
{
	struct ifreq request;

	/* Handles a failed if request operation. */
	if (if_request(descriptor, name, command, &request) != 0)
		return -1;
	*address = ((struct sockaddr_in *)&request.ifr_addr)->sin_addr;
	/* Reports successful completion. */
	return 0;
}

/* Supports the wait for carrier operation. */
static int
wait_for_carrier(
	int descriptor,
	const char *name,
	uint64_t deadline)
{
	struct ifreq request;
	uint64_t now, remaining;
	useconds_t delay;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		now = netutil_monotonic_us();

		/* Handles the timed out condition. */
		if (timed_out || now >= deadline) {
			errno = ETIMEDOUT;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles a failed if request operation. */
		if (if_request(descriptor, name, SIOCGIFFLAGS, &request) != 0)
			return -1;

		/* Handles the request condition. */
		if ((request.ifr_flags & IFF_RUNNING) != 0)
			return 0;
		now = netutil_monotonic_us();

		/* Handles the now condition. */
		if (now >= deadline) {
			errno = ETIMEDOUT;

			/* Reports operation failure. */
			return -1;
		}
		remaining = deadline - now;
		delay = (useconds_t)(remaining < CARRIER_POLL_INTERVAL_US ?
		    remaining : CARRIER_POLL_INTERVAL_US);

		/* Handles the reported system error. */
		if (usleep(delay) != 0 && errno != EINTR)
			return -1;
	}
}

/* Supports the set address operation. */
static int
set_address(
	int descriptor,
	const char *name,
	unsigned long command,
	struct in_addr address)
{
	int function_result;
	struct ifreq request;
	struct sockaddr_in *inet;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&request, name) != 0)
		return -1;
	inet = (struct sockaddr_in *)&request.ifr_addr;
	inet->sin_family = AF_INET;
	inet->sin_addr = address;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, command, &request);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the restore interface operation. */
static int
restore_interface(
	int descriptor,
	const char *name,
	const struct snapshot *saved)
{
	struct ifreq request;
	int first_error;

	first_error = 0;

	/* Attempt every field even if an earlier restoration fails. */
	if (set_address(descriptor, name, SIOCSIFNETMASK, saved->mask) != 0)
		first_error = errno;

	/* Handles an operation failure. */
	if (set_address(descriptor, name, SIOCSIFBRDADDR, saved->broadcast) != 0 &&
	    first_error == 0)
		first_error = errno;

	/* Handles an operation failure. */
	if (set_address(descriptor, name, SIOCSIFADDR, saved->address) != 0 &&
	    first_error == 0)
		first_error = errno;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&request, name) == 0) {
		request.ifr_flags = saved->flags;

		/* Handles an operation failure. */
		if (ioctl(descriptor, SIOCSIFFLAGS, &request) != 0 &&
		    first_error == 0)
			first_error = errno;
	} else if (first_error == 0)
		first_error = errno;

	/* Handles an operation failure. */
	if (first_error != 0) {
		errno = first_error;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the find interface default operation. */
static int
find_interface_default(
	int descriptor,
	uint32_t ifindex,
	struct rtentry *saved)
{
	struct rtentry entry;
	unsigned ordinal;

	/* Process each element required by the operation. */
	for (ordinal = 0;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;

		/* Handles a failed ioctl operation. */
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0)
			return errno == ENOENT ? 0 : -1;

		/* Handles the entry condition. */
		if (entry.rt_ifindex == ifindex &&
		    ((struct sockaddr_in *)&entry.rt_dst)->sin_addr.s_addr ==
			0 &&
		    ((struct sockaddr_in *)&entry.rt_genmask)
			    ->sin_addr.s_addr == 0) {
			*saved = entry;
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Supports the delete interface default operation. */
static int
delete_interface_default(
	int descriptor,
	uint32_t ifindex)
{
	struct rtentry entry;
	unsigned ordinal;

	/* Continue until the operation reaches a terminal state. */
	ordinal = 0;
	for (;;) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;

		/* Handles a failed ioctl operation. */
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0)
			return errno == ENOENT ? 0 : -1;

		/* Handles the entry condition. */
		if (entry.rt_ifindex == ifindex &&
		    ((struct sockaddr_in *)&entry.rt_dst)->sin_addr.s_addr ==
			0 &&
		    ((struct sockaddr_in *)&entry.rt_genmask)
			    ->sin_addr.s_addr == 0) {
			/* Handles a failed ioctl operation. */
			if (ioctl(descriptor, SIOCDELRT, &entry) != 0)
				return -1;
			continue;
		}
		ordinal++;
	}
}

/* Supports the transaction id operation. */
static uint32_t
transaction_id(
	const uint8_t mac[6])
{
	uint32_t value;
	unsigned i;

	/* Process each element required by the operation. */
	value = (uint32_t)netutil_monotonic_us();
	for (i = 0; i < 6U; i++)
		value = value * 33U ^ mac[i];

	/* Returns the computed result. */
	return value;
}

/* Supports the deadline check operation. */
static int
deadline_check(
	uint64_t deadline)
{
	/* Handles a failed netutil monotonic us operation. */
	if (timed_out || netutil_monotonic_us() >= deadline) {
		errno = ETIMEDOUT;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the set receive deadline operation. */
static int
set_receive_deadline(
	int descriptor,
	uint64_t deadline)
{
	int function_result;
	struct timeval timeout;
	uint64_t now, remaining;

	now = netutil_monotonic_us();

	/* Handles the timed out condition. */
	if (timed_out || now >= deadline) {
		errno = ETIMEDOUT;

		/* Reports operation failure. */
		return -1;
	}
	remaining = deadline - now;

	/* Handles the remaining condition. */
	if (remaining > 1000000U)
		remaining = 1000000U;
	timeout.tv_sec = (time_t)(remaining / 1000000U);
	timeout.tv_usec = (long)(remaining % 1000000U);

	/*
 * The deadline check above excludes zero.  Preserve a nonzero socket
	 * timeout even on a sub-microsecond clock-resolution boundary. */
	if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		timeout.tv_usec = 1;

	/* Obtains the setsockopt result. */
	function_result = setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
	    sizeof(timeout));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the sockaddr value operation. */
static void
sockaddr_value(
	struct sockaddr *address,
	uint32_t value)
{
	struct sockaddr_in *inet;

	inet = (struct sockaddr_in *)address;
	memset(address, 0, sizeof(*address));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = value;
}

/* Supports the write resolver operation. */
static int
write_resolver(
	const char *interface,
	const struct dhcp_lease *lease)
{
	ssize_t count;
	size_t offset;
	char buffer[256], address[16], temporary[128];
	size_t used;
	unsigned index, prior;
	int descriptor, saved_errno;
	struct in_addr value;

	used = 0;
	used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
				 "# Generated by dhcpc for %s\n", interface);

	/* Process each remaining element. */
	for (index = 0; index < lease->dns_count && used < sizeof(buffer);
	     index++) {
		/* Process each remaining element. */
		value.s_addr = lease->dns_servers[index];
		for (prior = 0; prior < index; prior++)

			/* Handles the lease condition. */
			if (lease->dns_servers[prior] ==
			    lease->dns_servers[index])
				break;

		/* Handles the prior condition. */
		if (prior != index)
			continue;
		inet_ntop(AF_INET, &value, address, sizeof(address));
		used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
					 "nameserver %s\n", address);
	}

	/* Checks the current capacity usage. */
	if (used >= sizeof(buffer))
		return -1;

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Continue while the operation condition remains true. */
	offset = 0;
	while (offset < used) {

		count = write(descriptor, buffer + offset, used - offset);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0) {
			saved_errno = count < 0 ? errno : EIO;
			(void)close(descriptor);
			(void)unlink(temporary);
			errno = saved_errno;

			/* Reports operation failure. */
			return -1;
		}
		offset += (size_t)count;
	}

	/* Handles a failed fsync operation. */
	if (fsync(descriptor) != 0) {
		saved_errno = errno;
		(void)close(descriptor);
		(void)unlink(temporary);
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed close operation. */
	if (close(descriptor) != 0) {
		saved_errno = errno;
		(void)unlink(temporary);
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed rename operation. */
	if (rename(temporary, "/etc/resolv.conf") != 0) {
		saved_errno = errno;
		(void)unlink(temporary);
		errno = saved_errno;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the timeout handler operation. */
static void
timeout_handler(
	int signal_number)
{
	(void)signal_number;
	timed_out = 1;
}
