/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void
timeout_handler(int signal_number)
{
	(void)signal_number;
	timed_out = 1;
}

static int
if_request(int descriptor, const char *name, unsigned long command,
	   struct ifreq *request)
{
	if (netutil_ifreq(request, name) != 0)
		return -1;
	return ioctl(descriptor, command, request);
}

static int
get_address(int descriptor, const char *name, unsigned long command,
	    struct in_addr *address)
{
	struct ifreq request;
	if (if_request(descriptor, name, command, &request) != 0)
		return -1;
	*address = ((struct sockaddr_in *)&request.ifr_addr)->sin_addr;
	return 0;
}

static int
set_address(int descriptor, const char *name, unsigned long command,
	    struct in_addr address)
{
	struct ifreq request;
	struct sockaddr_in *inet;
	if (netutil_ifreq(&request, name) != 0)
		return -1;
	inet = (struct sockaddr_in *)&request.ifr_addr;
	inet->sin_family = AF_INET;
	inet->sin_addr = address;
	return ioctl(descriptor, command, &request);
}

static int
snapshot_interface(int descriptor, const char *name, struct snapshot *saved)
{
	struct ifreq request;

	memset(saved, 0, sizeof(*saved));
	if (if_request(descriptor, name, SIOCGIFFLAGS, &request) != 0)
		return -1;
	saved->flags = request.ifr_flags;
	if (get_address(descriptor, name, SIOCGIFADDR, &saved->address) != 0 ||
	    get_address(descriptor, name, SIOCGIFNETMASK, &saved->mask) != 0 ||
	    get_address(descriptor, name, SIOCGIFBRDADDR, &saved->broadcast) != 0)
		return -1;
	return 0;
}

static int
restore_interface(int descriptor, const char *name,
		  const struct snapshot *saved)
{
	struct ifreq request;
	int first_error = 0;

	/* Attempt every field even if an earlier restoration fails. */
	if (set_address(descriptor, name, SIOCSIFNETMASK, saved->mask) != 0)
		first_error = errno;
	if (set_address(descriptor, name, SIOCSIFBRDADDR, saved->broadcast) != 0 &&
	    first_error == 0)
		first_error = errno;
	if (set_address(descriptor, name, SIOCSIFADDR, saved->address) != 0 &&
	    first_error == 0)
		first_error = errno;
	if (netutil_ifreq(&request, name) == 0) {
		request.ifr_flags = saved->flags;
		if (ioctl(descriptor, SIOCSIFFLAGS, &request) != 0 &&
		    first_error == 0)
			first_error = errno;
	} else if (first_error == 0)
		first_error = errno;
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	return 0;
}

static int
wait_for_carrier(int descriptor, const char *name, uint64_t deadline)
{
	for (;;) {
		struct ifreq request;
		uint64_t now, remaining;
		useconds_t delay;

		now = netutil_monotonic_us();
		if (timed_out || now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (if_request(descriptor, name, SIOCGIFFLAGS, &request) != 0)
			return -1;
		if ((request.ifr_flags & IFF_RUNNING) != 0)
			return 0;
		now = netutil_monotonic_us();
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		remaining = deadline - now;
		delay = (useconds_t)(remaining < CARRIER_POLL_INTERVAL_US ?
		    remaining : CARRIER_POLL_INTERVAL_US);
		if (usleep(delay) != 0 && errno != EINTR)
			return -1;
	}
}

static int
prepare_interface(int descriptor, const char *name, uint64_t deadline,
		  struct snapshot *saved, int *rollback_error)
{
	struct ifreq request;
	struct in_addr zero = {0};
	int saved_errno;

	if (snapshot_interface(descriptor, name, saved) != 0)
		return -1;
	if (netutil_ifreq(&request, name) != 0)
		return -1;
	request.ifr_flags = saved->flags | IFF_UP;
	if (ioctl(descriptor, SIOCSIFFLAGS, &request) != 0)
		return -1;
	if (wait_for_carrier(descriptor, name, deadline) == 0 &&
	    set_address(descriptor, name, SIOCSIFADDR, zero) == 0 &&
	    set_address(descriptor, name, SIOCSIFNETMASK, zero) == 0 &&
	    set_address(descriptor, name, SIOCSIFBRDADDR, zero) == 0)
		return 0;

	saved_errno = errno;
	if (restore_interface(descriptor, name, saved) != 0 &&
	    rollback_error != NULL)
		*rollback_error = errno;
	errno = saved_errno;
	return -1;
}

static int
deadline_check(uint64_t deadline)
{
	if (timed_out || netutil_monotonic_us() >= deadline) {
		errno = ETIMEDOUT;
		return -1;
	}
	return 0;
}

static int
set_receive_deadline(int descriptor, uint64_t deadline)
{
	struct timeval timeout;
	uint64_t now, remaining;

	now = netutil_monotonic_us();
	if (timed_out || now >= deadline) {
		errno = ETIMEDOUT;
		return -1;
	}
	remaining = deadline - now;
	if (remaining > 1000000U)
		remaining = 1000000U;
	timeout.tv_sec = (time_t)(remaining / 1000000U);
	timeout.tv_usec = (long)(remaining % 1000000U);
	/* The deadline check above excludes zero.  Preserve a nonzero socket
	 * timeout even on a sub-microsecond clock-resolution boundary. */
	if (timeout.tv_sec == 0 && timeout.tv_usec == 0)
		timeout.tv_usec = 1;
	return setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
	    sizeof(timeout));
}

static int
select_interface(int descriptor, char name[IFNAMSIZ])
{
	struct ifreq *items, request;
	unsigned count, index, candidates = 0;
	if (netutil_interfaces(descriptor, &items, &count) != 0)
		return -1;
	for (index = 0; index < count; index++) {
		if (if_request(descriptor, items[index].ifr_name, SIOCGIFFLAGS,
			       &request) == 0 &&
		    (request.ifr_flags & IFF_BROADCAST) != 0) {
			strncpy(name, items[index].ifr_name, IFNAMSIZ - 1U);
			candidates++;
		}
	}
	free(items);
	if (candidates != 1U) {
		errno = candidates == 0 ? ENODEV : EBUSY;
		return -1;
	}
	return 0;
}

static uint32_t
transaction_id(const uint8_t mac[6])
{
	uint32_t value = (uint32_t)netutil_monotonic_us();
	unsigned i;
	for (i = 0; i < 6U; i++)
		value = value * 33U ^ mac[i];
	return value;
}

static void
sockaddr_value(struct sockaddr *address, uint32_t value)
{
	struct sockaddr_in *inet = (struct sockaddr_in *)address;
	memset(address, 0, sizeof(*address));
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = value;
}

static int
delete_interface_default(int descriptor, uint32_t ifindex)
{
	struct rtentry entry;
	unsigned ordinal = 0;
	for (;;) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0)
			return errno == ENOENT ? 0 : -1;
		if (entry.rt_ifindex == ifindex &&
		    ((struct sockaddr_in *)&entry.rt_dst)->sin_addr.s_addr ==
			0 &&
		    ((struct sockaddr_in *)&entry.rt_genmask)
			    ->sin_addr.s_addr == 0) {
			if (ioctl(descriptor, SIOCDELRT, &entry) != 0)
				return -1;
			continue;
		}
		ordinal++;
	}
}

static int
find_interface_default(int descriptor, uint32_t ifindex,
		       struct rtentry *saved)
{
	struct rtentry entry;
	unsigned ordinal;

	for (ordinal = 0;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0)
			return errno == ENOENT ? 0 : -1;
		if (entry.rt_ifindex == ifindex &&
		    ((struct sockaddr_in *)&entry.rt_dst)->sin_addr.s_addr ==
			0 &&
		    ((struct sockaddr_in *)&entry.rt_genmask)
			    ->sin_addr.s_addr == 0) {
			*saved = entry;
			return 1;
		}
	}
}

static int
write_resolver(const char *interface, const struct dhcp_lease *lease)
{
	char buffer[256], address[16], temporary[128];
	size_t used = 0;
	unsigned index, prior;
	int descriptor, saved_errno;
	used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
				 "# Generated by dhcpc for %s\n", interface);
	for (index = 0; index < lease->dns_count && used < sizeof(buffer);
	     index++) {
		struct in_addr value = {lease->dns_servers[index]};
		for (prior = 0; prior < index; prior++)
			if (lease->dns_servers[prior] ==
			    lease->dns_servers[index])
				break;
		if (prior != index)
			continue;
		inet_ntop(AF_INET, &value, address, sizeof(address));
		used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
					 "nameserver %s\n", address);
	}
	if (used >= sizeof(buffer))
		return -1;
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (descriptor < 0)
		return -1;
	{
		size_t offset = 0;
		while (offset < used) {
			ssize_t count =
			    write(descriptor, buffer + offset, used - offset);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				saved_errno = count < 0 ? errno : EIO;
				(void)close(descriptor);
				(void)unlink(temporary);
				errno = saved_errno;
				return -1;
			}
			offset += (size_t)count;
		}
	}
	if (fsync(descriptor) != 0) {
		saved_errno = errno;
		(void)close(descriptor);
		(void)unlink(temporary);
		errno = saved_errno;
		return -1;
	}
	if (close(descriptor) != 0) {
		saved_errno = errno;
		(void)unlink(temporary);
		errno = saved_errno;
		return -1;
	}
	if (rename(temporary, "/etc/resolv.conf") != 0) {
		saved_errno = errno;
		(void)unlink(temporary);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int
usage(void)
{
	puts("usage: dhcpc [-v] [-t seconds] [interface]");
	return 2;
}

int
main(int argc, char **argv)
{
	struct snapshot saved;
	struct rtentry previous_default;
	struct ifreq request;
	struct sockaddr_in client, server, source;
	struct dhcp_lease offer, lease;
	uint8_t packet[600], mac[6];
	char interface[IFNAMSIZ] = {0}, text[16];
	uint32_t xid, timeout_seconds = 15, ifindex;
	uint64_t deadline;
	size_t packet_length;
	const char *failure_stage = "interface";
	int control, socket_ = -1, verbose = 0, arg = 1, got_offer = 0;
	int got_ack = 0, interface_prepared = 0;
	int previous_default_present = 0, route_prepared = 0, rollback_error = 0;

	while (arg < argc && argv[arg][0] == '-') {
		if (strcmp(argv[arg], "-v") == 0) {
			verbose = 1;
			arg++;
		} else if (strcmp(argv[arg], "-t") == 0 && arg + 1 < argc) {
			char *end;
			unsigned long v = strtoul(argv[arg + 1], &end, 10);
			if (*end != '\0' || v == 0 || v > 3600U)
				return usage();
			timeout_seconds = (uint32_t)v;
			arg += 2;
		} else
			return usage();
	}
	if (arg + 1 < argc)
		return usage();
	if (arg < argc) {
		if (strlen(argv[arg]) >= IFNAMSIZ)
			return usage();
		strcpy(interface, argv[arg]);
	}
	control = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (control < 0) {
		printf("dhcpc: socket: %s\n", strerror(errno));
		return 1;
	}
	if (interface[0] == '\0' && select_interface(control, interface) != 0) {
		printf("dhcpc: select interface: %s\n", strerror(errno));
		close(control);
		return 1;
	}
	memset(&previous_default, 0, sizeof(previous_default));
	deadline =
	    netutil_monotonic_us() + (uint64_t)timeout_seconds * 1000000U;
	timed_out = 0;
	(void)signal(SIGALRM, timeout_handler);
	(void)alarm(timeout_seconds);
	failure_stage = "carrier";
	if (prepare_interface(control, interface, deadline, &saved,
	    &rollback_error) != 0)
		goto fail;
	interface_prepared = 1;
	failure_stage = "identity";
	if (if_request(control, interface, SIOCGIFHWADDR, &request) != 0 ||
	    netutil_ifindex(control, interface, &ifindex) != 0)
		goto fail;
	memcpy(mac, request.ifr_hwaddr, sizeof(mac));
	failure_stage = "route";
	previous_default_present =
	    find_interface_default(control, ifindex, &previous_default);
	if (previous_default_present < 0)
		goto fail;
	route_prepared = 1;
	if (delete_interface_default(control, ifindex) != 0)
		goto fail;
	failure_stage = "socket";
	socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket_ < 0)
		goto fail;
	{
		int enabled = 1;
		if (setsockopt(socket_, SOL_SOCKET, SO_BINDTODEVICE, interface,
			       (socklen_t)strlen(interface) + 1U) != 0 ||
		    setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &enabled,
			       sizeof(enabled)) != 0)
			goto socket_fail;
	}
	memset(&client, 0, sizeof(client));
	client.sin_family = AF_INET;
	client.sin_port = htons(68);
	client.sin_addr.s_addr = INADDR_ANY;
	if (bind(socket_, (struct sockaddr *)&client, sizeof(client)) != 0)
		goto socket_fail;
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(67);
	server.sin_addr.s_addr = INADDR_BROADCAST;
	xid = transaction_id(mac);
	failure_stage = "offer";
	while (!timed_out && netutil_monotonic_us() < deadline && !got_offer) {
		if (deadline_check(deadline) != 0 ||
		    dhcp_build(packet, sizeof(packet), &packet_length,
			       DHCP_DISCOVER, xid, mac, 0, 0) != 0 ||
		    sendto(socket_, packet, packet_length, 0,
			   (struct sockaddr *)&server, sizeof(server)) < 0)
			goto socket_fail;
		if (verbose)
			printf("dhcpc: %s: broadcasting discover\n", interface);
		for (;;) {
			socklen_t slen = sizeof(source);
			ssize_t count;

			if (set_receive_deadline(socket_, deadline) != 0)
				goto socket_fail;
			count = recvfrom(socket_, packet, sizeof(packet), 0,
				     (struct sockaddr *)&source, &slen);
			if (count < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK ||
			     errno == EINTR))
				break;
			if (count < 0)
				goto socket_fail;
			if (source.sin_port == htons(67) &&
			    dhcp_parse(packet, (size_t)count, xid, mac,
				       &offer) == 0 &&
			    offer.message_type == DHCP_OFFER &&
			    offer.server_identifier != 0) {
				got_offer = 1;
				break;
			}
		}
	}
	if (!got_offer) {
		errno = ETIMEDOUT;
		goto socket_fail;
	}
	{
		struct in_addr value = {offer.address};
		inet_ntop(AF_INET, &value, text, sizeof(text));
		printf("dhcpc: %s: offered %s", interface, text);
		value.s_addr = offer.server_identifier;
		inet_ntop(AF_INET, &value, text, sizeof(text));
		printf(" by %s\n", text);
	}
	failure_stage = "ack";
	while (!timed_out && netutil_monotonic_us() < deadline && !got_ack) {
		if (deadline_check(deadline) != 0 ||
		    dhcp_build(packet, sizeof(packet), &packet_length,
			       DHCP_REQUEST, xid, mac, offer.address,
			       offer.server_identifier) != 0 ||
		    sendto(socket_, packet, packet_length, 0,
			   (struct sockaddr *)&server, sizeof(server)) < 0)
			goto socket_fail;
		for (;;) {
			socklen_t slen = sizeof(source);
			ssize_t count;

			if (set_receive_deadline(socket_, deadline) != 0)
				goto socket_fail;
			count = recvfrom(socket_, packet, sizeof(packet), 0,
				     (struct sockaddr *)&source, &slen);
			if (count < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK ||
			     errno == EINTR))
				break;
			if (count < 0)
				goto socket_fail;
			if (source.sin_port == htons(67) &&
			    dhcp_parse(packet, (size_t)count, xid, mac,
				       &lease) == 0) {
				if (lease.message_type == DHCP_NAK) {
					errno = EACCES;
					goto socket_fail;
				}
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
	if (!got_ack) {
		errno = ETIMEDOUT;
		goto socket_fail;
	}
	failure_stage = "configuration";
	{
		struct in_addr value;
		value.s_addr = lease.netmask;
		if (set_address(control, interface, SIOCSIFNETMASK, value) != 0)
			goto socket_fail;
		value.s_addr = lease.broadcast;
		if (set_address(control, interface, SIOCSIFBRDADDR, value) != 0)
			goto socket_fail;
		value.s_addr = lease.address;
		if (set_address(control, interface, SIOCSIFADDR, value) != 0)
			goto socket_fail;
	}
	failure_stage = "route";
	if (lease.router_count != 0) {
		struct rtentry route;
		memset(&route, 0, sizeof(route));
		route.rt_flags = RTF_UP | RTF_GATEWAY | RTF_DYNAMIC;
		route.rt_ifindex = ifindex;
		sockaddr_value(&route.rt_dst, 0);
		sockaddr_value(&route.rt_genmask, 0);
		sockaddr_value(&route.rt_gateway, lease.routers[0]);
		if (ioctl(control, SIOCADDRT, &route) != 0)
			goto socket_fail;
	}
	{
		struct in_addr value = {lease.address};
		unsigned prefix = 0;
		struct in_addr mask = {lease.netmask};
		netutil_mask_prefix(mask, &prefix);
		inet_ntop(AF_INET, &value, text, sizeof(text));
		printf("dhcpc: %s: address %s/%u\n", interface, text, prefix);
	}
	if (lease.router_count != 0) {
		struct in_addr value = {lease.routers[0]};
		inet_ntop(AF_INET, &value, text, sizeof(text));
		printf("dhcpc: %s: default route %s\n", interface, text);
	}
	if (lease.dns_count != 0) {
		unsigned i;
		for (i = 0; i < lease.dns_count; i++) {
			struct in_addr value = {lease.dns_servers[i]};
			inet_ntop(AF_INET, &value, text, sizeof(text));
			printf("dhcpc: %s: dns %s\n", interface, text);
		}
		failure_stage = "resolver";
		if (write_resolver(interface, &lease) != 0)
			goto socket_fail;
	}
	printf("dhcpc: %s: lease %u seconds\n", interface, lease.lease_time);
	(void)alarm(0);
	close(socket_);
	close(control);
	return 0;
socket_fail:
fail:
	{
		int saved_errno = errno != 0 ? errno : EIO;
		int error;

		(void)alarm(0);
		if (socket_ >= 0)
			close(socket_);
		if (route_prepared) {
			if (delete_interface_default(control, ifindex) != 0 &&
			    rollback_error == 0)
				rollback_error = errno;
			if (previous_default_present > 0 &&
			    ioctl(control, SIOCADDRT, &previous_default) != 0 &&
			    rollback_error == 0)
				rollback_error = errno;
		}
		if (interface_prepared &&
		    restore_interface(control, interface, &saved) != 0 &&
		    rollback_error == 0)
			rollback_error = errno;
		printf("dhcpc: %s: %s: %s\n", interface, failure_stage,
		       strerror(saved_errno));
		if (rollback_error != 0) {
			error = rollback_error;
			printf("dhcpc: %s: rollback: %s\n", interface,
			       strerror(error));
		}
		close(control);
		errno = saved_errno;
		return 1;
	}
}
