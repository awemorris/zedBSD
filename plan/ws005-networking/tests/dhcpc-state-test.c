/* WS005 DHCP carrier/configuration transaction fixture. SPDX-License-Identifier:
 * Zlib */
#include "userland/base/net/dhcp.h"
#include "userland/base/net/netutil.h"

#include <errno.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int fixture_ioctl(int, unsigned long, ...);
static int fixture_usleep(useconds_t);
static uint64_t fixture_monotonic_us(void);
static int fixture_socket(int, int, int);
static int fixture_setsockopt(int, int, int, const void *, socklen_t);
static int fixture_bind(int, const struct sockaddr *, socklen_t);
static ssize_t fixture_sendto(int, const void *, size_t, int,
	const struct sockaddr *, socklen_t);
static ssize_t fixture_recvfrom(int, void *, size_t, int, struct sockaddr *,
	socklen_t *);
static int fixture_close(int);
static int fixture_open(const char *, int, ...);
static ssize_t fixture_write(int, const void *, size_t);
static int fixture_fsync(int);
static int fixture_rename(const char *, const char *);
static int fixture_unlink(const char *);
static pid_t fixture_getpid(void);
static int fixture_printf(const char *, ...);
static char *fixture_strerror(int);
static sighandler_t fixture_signal(int, sighandler_t);
static unsigned fixture_alarm(unsigned);
static int fixture_dhcp_build(uint8_t *, size_t, size_t *, uint8_t, uint32_t,
	const uint8_t[6], uint32_t, uint32_t);
static int fixture_dhcp_parse(const uint8_t *, size_t, uint32_t,
	const uint8_t[6], struct dhcp_lease *);

#define ioctl fixture_ioctl
#define usleep fixture_usleep
#define netutil_monotonic_us fixture_monotonic_us
#define socket fixture_socket
#define setsockopt fixture_setsockopt
#define bind fixture_bind
#define sendto fixture_sendto
#define recvfrom fixture_recvfrom
#define close fixture_close
#define open fixture_open
#define write fixture_write
#define fsync fixture_fsync
#define rename fixture_rename
#define unlink fixture_unlink
#define getpid fixture_getpid
#define printf fixture_printf
#define strerror fixture_strerror
#define signal fixture_signal
#define alarm fixture_alarm
#define dhcp_build fixture_dhcp_build
#define dhcp_parse fixture_dhcp_parse
#define main dhcpc_program_main
#include "userland/base/dhcpc/main.c"
#undef main
#undef dhcp_parse
#undef dhcp_build
#undef alarm
#undef signal
#undef printf
#undef getpid
#undef unlink
#undef rename
#undef fsync
#undef write
#undef strerror
#undef open
#undef close
#undef recvfrom
#undef sendto
#undef bind
#undef setsockopt
#undef socket
#undef netutil_monotonic_us
#undef usleep
#undef ioctl

struct fake_interface {
	int flags;
	struct in_addr address, mask, broadcast;
	uint64_t now;
	unsigned carrier_after_sleeps;
	unsigned sleeps;
	unsigned set_address_count;
	unsigned set_mask_count;
	unsigned set_broadcast_count;
	unsigned set_flags_count;
	unsigned socket_count;
	unsigned close_count;
	unsigned last_alarm;
	unsigned fail_clear_mask_once;
	unsigned fail_lease_mask_once;
	unsigned discover_seen;
	unsigned request_seen;
	unsigned receive_count;
	unsigned invalid_receive_count;
	unsigned send_succeeds;
	unsigned require_route_absent_at_send;
	unsigned serve_offer;
	unsigned serve_ack;
	unsigned lease_router;
	unsigned lease_dns;
	unsigned fail_resolver_open;
	unsigned fail_resolver_write;
	unsigned fail_resolver_fsync;
	unsigned fail_resolver_close;
	unsigned fail_resolver_rename;
	unsigned resolver_open_count;
	unsigned resolver_write_count;
	unsigned resolver_fsync_count;
	unsigned resolver_close_count;
	unsigned resolver_rename_count;
	unsigned resolver_unlink_count;
	unsigned resolver_fd_open;
	unsigned fail_restore_route;
	unsigned fail_route_get_once;
	unsigned fail_route_delete_once;
	unsigned route_add_count;
	unsigned route_delete_count;
	uint64_t receive_timeout_us;
	int route_present;
	struct rtentry route;
	char output[2048];
	size_t output_used;
	char resolver_current[256];
	char resolver_temporary[256];
	size_t resolver_temporary_used;
	uint8_t last_message_type;
};

static struct fake_interface fake;
static int fixture_errno;

int *
__libc_errno_location(void)
{
	return &fixture_errno;
}

static void
fail(const char *message)
{
	fprintf(stderr, "dhcpc-state-test: %s\n", message);
	exit(1);
}

static void
reset_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.flags = IFF_BROADCAST | IFF_MULTICAST;
	fake.address.s_addr = UINT32_C(0x01020304);
	fake.mask.s_addr = UINT32_C(0xffffff00);
	fake.broadcast.s_addr = UINT32_C(0x010203ff);
	fake.now = UINT64_C(1000000);
	fake.carrier_after_sleeps = (unsigned)-1;
	strcpy(fake.resolver_current, "nameserver 192.0.2.53\n");
	fixture_errno = 0;
	timed_out = 0;
}

static void
put_address(struct ifreq *request, struct in_addr address)
{
	struct sockaddr_in *inet = (struct sockaddr_in *)&request->ifr_addr;

	memset(&request->ifr_addr, 0, sizeof(request->ifr_addr));
	inet->sin_family = AF_INET;
	inet->sin_addr = address;
}

static struct in_addr
request_address(const struct ifreq *request)
{
	return ((const struct sockaddr_in *)&request->ifr_addr)->sin_addr;
}

static int
fixture_ioctl(int descriptor, unsigned long command, ...)
{
	struct ifreq *request;
	struct rtentry *route;
	struct in_addr value;
	void *argument;
	va_list arguments;

	(void)descriptor;
	va_start(arguments, command);
	argument = va_arg(arguments, void *);
	va_end(arguments);
	if (command == SIOCGRTENTRY) {
		route = argument;
		if (fake.fail_route_get_once) {
			fake.fail_route_get_once = 0;
			errno = EIO;
			return -1;
		}
		if (!fake.route_present || route->rt_index != 0) {
			errno = ENOENT;
			return -1;
		}
		*route = fake.route;
		route->rt_index = 0;
		return 0;
	}
	if (command == SIOCDELRT) {
		route = argument;
		if (fake.fail_route_delete_once) {
			fake.fail_route_delete_once = 0;
			errno = EIO;
			return -1;
		}
		if (!fake.route_present ||
		    route->rt_ifindex != fake.route.rt_ifindex) {
			errno = ENOENT;
			return -1;
		}
		fake.route_present = 0;
		fake.route_delete_count++;
		return 0;
	}
	if (command == SIOCADDRT) {
		route = argument;
		if (fake.fail_restore_route && fake.route_add_count != 0) {
			errno = EIO;
			return -1;
		}
		fake.route = *route;
		fake.route_present = 1;
		fake.route_add_count++;
		return 0;
	}
	request = argument;
	switch (command) {
	case SIOCGIFFLAGS:
		request->ifr_flags = fake.flags;
		return 0;
	case SIOCSIFFLAGS:
		fake.flags = request->ifr_flags;
		if ((fake.flags & IFF_UP) != 0 && fake.carrier_after_sleeps == 0)
			fake.flags |= IFF_RUNNING;
		fake.set_flags_count++;
		return 0;
	case SIOCGIFADDR:
		put_address(request, fake.address);
		return 0;
	case SIOCGIFNETMASK:
		put_address(request, fake.mask);
		return 0;
	case SIOCGIFBRDADDR:
		put_address(request, fake.broadcast);
		return 0;
	case SIOCSIFADDR:
		value = request_address(request);
		if (value.s_addr == 0 && (fake.flags & IFF_RUNNING) == 0)
			fail("configuration cleared before carrier");
		fake.address = value;
		fake.set_address_count++;
		return 0;
	case SIOCSIFNETMASK:
		value = request_address(request);
		if (value.s_addr == 0 && fake.fail_clear_mask_once) {
			fake.fail_clear_mask_once = 0;
			errno = EIO;
			return -1;
		}
		if (value.s_addr != 0 && fake.fail_lease_mask_once) {
			fake.fail_lease_mask_once = 0;
			errno = EIO;
			return -1;
		}
		fake.mask = value;
		fake.set_mask_count++;
		return 0;
	case SIOCSIFBRDADDR:
		fake.broadcast = request_address(request);
		fake.set_broadcast_count++;
		return 0;
	case SIOCGIFHWADDR:
		memcpy(request->ifr_hwaddr,
		    (uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x00, 0x01}, 6U);
		return 0;
	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}

int
netutil_ifreq(struct ifreq *request, const char *name)
{
	if (request == NULL || name == NULL || strlen(name) >= IFNAMSIZ) {
		errno = EINVAL;
		return -1;
	}
	memset(request, 0, sizeof(*request));
	strncpy(request->ifr_name, name, IFNAMSIZ - 1U);
	return 0;
}

int
netutil_ifindex(int descriptor, const char *name, uint32_t *result)
{
	(void)descriptor;
	(void)name;
	*result = 7U;
	return 0;
}

int
netutil_interfaces(int descriptor, struct ifreq **items, unsigned *count)
{
	(void)descriptor;
	(void)items;
	(void)count;
	errno = EIO;
	return -1;
}

int
netutil_mask_prefix(struct in_addr mask, unsigned *prefix)
{
	(void)mask;
	*prefix = 24U;
	return 0;
}

static uint64_t
fixture_monotonic_us(void)
{
	return fake.now;
}

static int
fixture_usleep(useconds_t delay)
{
	if (delay == 0 || delay > CARRIER_POLL_INTERVAL_US)
		fail("invalid carrier sleep");
	fake.now += delay;
	fake.sleeps++;
	if (fake.sleeps >= fake.carrier_after_sleeps)
		fake.flags |= IFF_RUNNING;
	return 0;
}

static int
fixture_socket(int domain, int type, int protocol)
{
	(void)domain;
	(void)type;
	(void)protocol;
	return fake.socket_count++ == 0 ? 10 : 11;
}

static int
fixture_setsockopt(int descriptor, int level, int option, const void *value,
		   socklen_t length)
{
	(void)descriptor;
	if (level == SOL_SOCKET && option == SO_RCVTIMEO) {
		const struct timeval *timeout = value;

		if (length != sizeof(*timeout) || timeout->tv_sec < 0 ||
		    timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)
			fail("invalid receive timeout option");
		fake.receive_timeout_us =
		    (uint64_t)timeout->tv_sec * 1000000U +
		    (uint64_t)timeout->tv_usec;
		if (fake.receive_timeout_us == 0 ||
		    fake.receive_timeout_us > 1000000U)
			fail("receive timeout escaped bounded deadline");
	}
	return 0;
}

static int
fixture_bind(int descriptor, const struct sockaddr *address, socklen_t length)
{
	(void)descriptor;
	(void)address;
	(void)length;
	return 0;
}

static ssize_t
fixture_sendto(int descriptor, const void *buffer, size_t length, int flags,
	       const struct sockaddr *address, socklen_t address_length)
{
	(void)buffer;
	(void)flags;
	(void)address;
	(void)address_length;
	if (descriptor != 11 || length == 0 || fake.address.s_addr != 0 ||
	    fake.mask.s_addr != 0 || fake.broadcast.s_addr != 0)
		fail("DHCP send did not observe zero interface configuration");
	if (fake.require_route_absent_at_send && fake.route_present)
		fail("DHCP send retained the previous default route");
	if (fake.last_message_type == DHCP_DISCOVER)
		fake.discover_seen++;
	else if (fake.last_message_type == DHCP_REQUEST)
		fake.request_seen++;
	else
		fail("unexpected DHCP message sent");
	if (fake.send_succeeds)
		return (ssize_t)length;
	errno = EIO;
	return -1;
}

static ssize_t
fixture_recvfrom(int descriptor, void *buffer, size_t length, int flags,
		 struct sockaddr *address, socklen_t *address_length)
{
	(void)descriptor;
	(void)buffer;
	(void)length;
	(void)flags;
	fake.receive_count++;
	if (fake.invalid_receive_count != 0) {
		struct sockaddr_in *source = (struct sockaddr_in *)address;

		fake.invalid_receive_count--;
		fake.now += 100000U;
		memset(source, 0, sizeof(*source));
		source->sin_family = AF_INET;
		source->sin_port = htons(68);
		*address_length = sizeof(*source);
		return 1;
	}
	if ((fake.last_message_type == DHCP_DISCOVER && fake.serve_offer) ||
	    (fake.last_message_type == DHCP_REQUEST && fake.serve_ack)) {
		struct sockaddr_in *source = (struct sockaddr_in *)address;

		if (descriptor != 11 || address_length == NULL ||
		    *address_length < sizeof(*source))
			fail("invalid DHCP receive arguments");
		memset(source, 0, sizeof(*source));
		source->sin_family = AF_INET;
		source->sin_port = htons(67);
		*address_length = sizeof(*source);
		return 1;
	}
	if (fake.receive_timeout_us == 0)
		fail("recvfrom reached without a receive deadline");
	fake.now += fake.receive_timeout_us;
	errno = EAGAIN;
	return -1;
}

static int
fixture_close(int descriptor)
{
	if (descriptor == 12) {
		if (!fake.resolver_fd_open)
			fail("resolver descriptor closed twice");
		fake.resolver_fd_open = 0;
		fake.resolver_close_count++;
		if (fake.fail_resolver_close) {
			errno = EIO;
			return -1;
		}
		return 0;
	}
	fake.close_count++;
	return 0;
}

static int
fixture_open(const char *path, int flags, ...)
{
	if (fake.fail_resolver_open) {
		errno = EIO;
		return -1;
	}
	if (strncmp(path, "/etc/resolv.conf.tmp.", 21U) != 0 ||
	    (flags & (O_WRONLY | O_CREAT | O_EXCL)) !=
		(O_WRONLY | O_CREAT | O_EXCL) || fake.resolver_fd_open)
		fail("invalid resolver temporary open");
	fake.resolver_open_count++;
	fake.resolver_fd_open = 1U;
	fake.resolver_temporary[0] = '\0';
	fake.resolver_temporary_used = 0;
	return 12;
}

static ssize_t
fixture_write(int descriptor, const void *buffer, size_t length)
{
	if (descriptor != 12 || !fake.resolver_fd_open ||
	    fake.resolver_temporary_used + length >=
		sizeof(fake.resolver_temporary))
		fail("invalid resolver write");
	fake.resolver_write_count++;
	if (fake.fail_resolver_write) {
		errno = EIO;
		return -1;
	}
	memcpy(fake.resolver_temporary + fake.resolver_temporary_used, buffer,
	    length);
	fake.resolver_temporary_used += length;
	fake.resolver_temporary[fake.resolver_temporary_used] = '\0';
	return (ssize_t)length;
}

static int
fixture_fsync(int descriptor)
{
	if (descriptor != 12 || !fake.resolver_fd_open)
		fail("invalid resolver fsync");
	fake.resolver_fsync_count++;
	if (fake.fail_resolver_fsync) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int
fixture_rename(const char *source, const char *destination)
{
	if (strncmp(source, "/etc/resolv.conf.tmp.", 21U) != 0 ||
	    strcmp(destination, "/etc/resolv.conf") != 0 ||
	    fake.resolver_fd_open)
		fail("invalid resolver rename");
	fake.resolver_rename_count++;
	if (fake.fail_resolver_rename) {
		errno = EIO;
		return -1;
	}
	strcpy(fake.resolver_current, fake.resolver_temporary);
	fake.resolver_temporary[0] = '\0';
	fake.resolver_temporary_used = 0;
	return 0;
}

static int
fixture_unlink(const char *path)
{
	if (strncmp(path, "/etc/resolv.conf.tmp.", 21U) != 0)
		fail("invalid resolver unlink");
	fake.resolver_unlink_count++;
	fake.resolver_temporary[0] = '\0';
	fake.resolver_temporary_used = 0;
	return 0;
}

static pid_t
fixture_getpid(void)
{
	return 42;
}

static int
fixture_printf(const char *format, ...)
{
	int count;
	va_list arguments;

	if (fake.output_used >= sizeof(fake.output))
		return -1;
	va_start(arguments, format);
	count = vsnprintf(fake.output + fake.output_used,
	    sizeof(fake.output) - fake.output_used, format, arguments);
	va_end(arguments);
	if (count < 0 || (size_t)count >= sizeof(fake.output) - fake.output_used)
		return -1;
	fake.output_used += (size_t)count;
	return count;
}

static char *
fixture_strerror(int error)
{
	if (error == ETIMEDOUT)
		return "Connection timed out";
	if (error == EIO)
		return "Input/output error";
	if (error == ENETDOWN)
		return "Network is down";
	return "fixture error";
}

static sighandler_t
fixture_signal(int signal_number, sighandler_t handler)
{
	(void)signal_number;
	return handler;
}

static unsigned
fixture_alarm(unsigned seconds)
{
	fake.last_alarm = seconds;
	return 0;
}

static int
fixture_dhcp_build(uint8_t *packet, size_t capacity, size_t *length,
		   uint8_t type, uint32_t xid, const uint8_t mac[6],
		   uint32_t requested, uint32_t server)
{
	(void)xid;
	(void)mac;
	if (packet == NULL || capacity < 300U ||
	    (type != DHCP_DISCOVER && type != DHCP_REQUEST))
		return -1;
	if (type == DHCP_DISCOVER && (requested != 0 || server != 0))
		fail("DISCOVER carried stale lease data");
	if (type == DHCP_REQUEST &&
	    (requested != htonl(UINT32_C(0x0a000002)) ||
	     server != htonl(UINT32_C(0x0a000001))))
		fail("REQUEST did not select the offered lease");
	memset(packet, 0, 300U);
	*length = 300U;
	fake.last_message_type = type;
	return 0;
}

static int
fixture_dhcp_parse(const uint8_t *packet, size_t length, uint32_t xid,
		   const uint8_t mac[6], struct dhcp_lease *lease)
{
	(void)packet;
	(void)length;
	(void)xid;
	(void)mac;
	memset(lease, 0, sizeof(*lease));
	lease->address = htonl(UINT32_C(0x0a000002));
	lease->server_identifier = htonl(UINT32_C(0x0a000001));
	if (fake.last_message_type == DHCP_DISCOVER && fake.serve_offer) {
		lease->message_type = DHCP_OFFER;
		return 0;
	}
	if (fake.last_message_type == DHCP_REQUEST && fake.serve_ack) {
		lease->message_type = DHCP_ACK;
		lease->netmask = htonl(UINT32_C(0xffffff00));
		lease->broadcast = htonl(UINT32_C(0x0a0000ff));
		lease->lease_time = 3600U;
		if (fake.lease_router) {
			lease->routers[0] = htonl(UINT32_C(0x0a000001));
			lease->router_count = 1U;
		}
		if (fake.lease_dns) {
			lease->dns_servers[0] = htonl(UINT32_C(0x0a000001));
			lease->dns_count = 1U;
		}
		return 0;
	}
	return -1;
}

static void
require_original_state(void)
{
	if (fake.flags != (IFF_BROADCAST | IFF_MULTICAST) ||
	    fake.address.s_addr != UINT32_C(0x01020304) ||
	    fake.mask.s_addr != UINT32_C(0xffffff00) ||
	    fake.broadcast.s_addr != UINT32_C(0x010203ff))
		fail("interface snapshot was not restored");
}

static void
test_delayed_carrier(void)
{
	struct snapshot saved;
	int rollback_error = 0;

	reset_fake();
	fake.carrier_after_sleeps = 3U;
	if (prepare_interface(10, "ue0", fake.now + 100000U, &saved,
	    &rollback_error) != 0 || rollback_error != 0)
		fail("delayed carrier rejected");
	if (fake.sleeps != 3U || (fake.flags & (IFF_UP | IFF_RUNNING)) !=
	    (IFF_UP | IFF_RUNNING))
		fail("carrier was not awaited with sleeps");
	if (fake.address.s_addr != 0 || fake.mask.s_addr != 0 ||
	    fake.broadcast.s_addr != 0)
		fail("configuration was not cleared after carrier");
	restore_interface(10, "ue0", &saved);
	require_original_state();
}

static void
test_clean_interface(void)
{
	struct snapshot saved;
	int rollback_error = 0;

	reset_fake();
	fake.address.s_addr = 0;
	fake.mask.s_addr = 0;
	fake.broadcast.s_addr = 0;
	fake.carrier_after_sleeps = 0;
	if (prepare_interface(10, "ue0", fake.now + 100000U, &saved,
	    &rollback_error) != 0 || rollback_error != 0)
		fail("clean interface rejected");
	if (fake.address.s_addr != 0 || fake.mask.s_addr != 0 ||
	    fake.broadcast.s_addr != 0)
		fail("clean interface configuration changed");
	restore_interface(10, "ue0", &saved);
	if (fake.flags != (IFF_BROADCAST | IFF_MULTICAST) ||
	    fake.address.s_addr != 0 || fake.mask.s_addr != 0 ||
	    fake.broadcast.s_addr != 0)
		fail("clean interface snapshot was not restored");
}

static void
test_timeout_rollback(void)
{
	struct snapshot saved;
	int rollback_error = 0;

	reset_fake();
	if (prepare_interface(10, "ue0", fake.now + 25000U, &saved,
	    &rollback_error) == 0 || errno != ETIMEDOUT || rollback_error != 0)
		fail("carrier timeout not reported");
	if (fake.sleeps != 3U || fake.now != UINT64_C(1025000))
		fail("carrier timeout was not bounded by sleeping");
	require_original_state();
}

static void
test_partial_clear_rollback(void)
{
	struct snapshot saved;
	int rollback_error = 0;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.fail_clear_mask_once = 1U;
	if (prepare_interface(10, "ue0", fake.now + 100000U, &saved,
	    &rollback_error) == 0 || errno != EIO || rollback_error != 0)
		fail("partial clear failure not preserved");
	if (fake.set_address_count < 2U || fake.set_mask_count == 0 ||
	    fake.set_broadcast_count == 0 || fake.set_flags_count < 2U)
		fail("rollback did not attempt every field");
	require_original_state();
}

static void
test_discover_zero_and_failure_rollback(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	if (dhcpc_program_main(4, arguments) != 1 || fake.discover_seen != 1U)
		fail("failed DISCOVER transaction result");
	if (fake.last_alarm != 0 || fake.close_count != 2U)
		fail("failed transaction resources not retired");
	require_original_state();
}

static void
test_success_commits_lease(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	if (dhcpc_program_main(4, arguments) != 0 || fake.discover_seen != 1U ||
	    fake.request_seen != 1U)
		fail("successful DHCP transaction result");
	if (fake.address.s_addr != htonl(UINT32_C(0x0a000002)) ||
	    fake.mask.s_addr != htonl(UINT32_C(0xffffff00)) ||
	    fake.broadcast.s_addr != htonl(UINT32_C(0x0a0000ff)))
		fail("successful lease was not committed");
	if ((fake.flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
		fail("successful interface state was rolled back");
	if (fake.last_alarm != 0 || fake.close_count != 2U)
		fail("successful transaction resources not retired");
}

static void
set_fake_default_route(unsigned flags, uint32_t gateway)
{
	memset(&fake.route, 0, sizeof(fake.route));
	fake.route.rt_flags = flags;
	fake.route.rt_ifindex = 7U;
	sockaddr_value(&fake.route.rt_dst, 0);
	sockaddr_value(&fake.route.rt_genmask, 0);
	sockaddr_value(&fake.route.rt_gateway, gateway);
	fake.route_present = 1;
}

static void
require_default_route_exact(const struct rtentry *expected)
{
	if (!fake.route_present ||
	    memcmp(&fake.route, expected, sizeof(*expected)) != 0)
		fail("previous default route was not restored exactly");
}

static void
test_route_snapshot_failure_preserves_state(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	fake.fail_route_get_once = 1U;
	if (dhcpc_program_main(4, arguments) != 1 || fake.discover_seen != 0 ||
	    fake.route_delete_count != 0 || fake.route_add_count != 0 ||
	    strstr(fake.output, "route: Input/output error") == NULL)
		fail("route snapshot failure was not isolated before DHCP");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_route_delete_failure_restores_state(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	fake.fail_route_delete_once = 1U;
	if (dhcpc_program_main(4, arguments) != 1 || fake.discover_seen != 0 ||
	    fake.route_delete_count != 1U || fake.route_add_count != 1U ||
	    strstr(fake.output, "route: Input/output error") == NULL)
		fail("route delete failure was not rolled back before DHCP");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_static_default_offer_timeout_rollback(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	if (dhcpc_program_main(4, arguments) != 1 ||
	    strstr(fake.output, "offer: Connection timed out") == NULL ||
	    fake.route_delete_count != 1U || fake.route_add_count != 1U)
		fail("offer timeout did not roll back the early route transaction");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_static_default_send_failure_rollback(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	if (dhcpc_program_main(4, arguments) != 1 || fake.discover_seen != 1U ||
	    strstr(fake.output, "offer: Input/output error") == NULL ||
	    fake.route_delete_count != 1U || fake.route_add_count != 1U)
		fail("send failure did not roll back the early route transaction");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_static_default_configuration_failure_rollback(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.fail_lease_mask_once = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	if (dhcpc_program_main(4, arguments) != 1 ||
	    strstr(fake.output, "configuration: Input/output error") == NULL ||
	    fake.route_delete_count != 1U || fake.route_add_count != 1U)
		fail("configuration failure did not restore the early route");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_invalid_receive_deadline(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.invalid_receive_count = 100U;
	if (dhcpc_program_main(4, arguments) != 1)
		fail("invalid receive stream returned success");
	if (fake.receive_count > 10U)
		fail("invalid receive stream escaped the total deadline");
	if (strstr(fake.output, "offer: Connection timed out") == NULL)
		fail("invalid receive timeout stage was not retained");
	require_original_state();
}

static void
test_ack_timeout_stage(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	if (dhcpc_program_main(4, arguments) != 1 || fake.discover_seen != 1U ||
	    fake.request_seen != 1U ||
	    strstr(fake.output, "ack: Connection timed out") == NULL)
		fail("ACK timeout stage was not retained");
	require_original_state();
}

static void
test_static_default_route_rollback(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	struct rtentry expected;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.lease_router = 1U;
	fake.lease_dns = 1U;
	fake.fail_resolver_open = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	expected = fake.route;
	if (dhcpc_program_main(4, arguments) != 1 ||
	    fake.route_add_count != 2U ||
	    fake.route_delete_count != 2U ||
	    strstr(fake.output, "resolver: Input/output error") == NULL)
		fail("static default route was not restored transactionally");
	require_default_route_exact(&expected);
	require_original_state();
}

static void
test_static_default_removed_without_dhcp_router(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	if (dhcpc_program_main(4, arguments) != 0 || fake.route_present ||
	    fake.route_delete_count != 1U || fake.route_add_count != 0)
		fail("stale static default survived a routerless DHCP lease");
}

static void
test_static_default_replaced_by_dhcp_route(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	const struct sockaddr_in *gateway;

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.lease_router = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	if (dhcpc_program_main(4, arguments) != 0 || !fake.route_present ||
	    fake.route.rt_flags != (RTF_UP | RTF_GATEWAY | RTF_DYNAMIC) ||
	    fake.route.rt_ifindex != 7U || fake.route_delete_count != 1U ||
	    fake.route_add_count != 1U)
		fail("successful DHCP transaction did not replace the old route");
	gateway = (const struct sockaddr_in *)&fake.route.rt_gateway;
	if (gateway->sin_addr.s_addr != htonl(UINT32_C(0x0a000001)))
		fail("successful DHCP transaction restored the old gateway");
}

static void
prepare_resolver_transaction(void)
{
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.lease_dns = 1U;
}

static void
test_resolver_success_commits_atomically(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	const char *expected =
	    "# Generated by dhcpc for ue0\nnameserver 10.0.0.1\n";

	reset_fake();
	prepare_resolver_transaction();
	if (dhcpc_program_main(4, arguments) != 0 ||
	    strcmp(fake.resolver_current, expected) != 0 ||
	    fake.resolver_open_count != 1U ||
	    fake.resolver_write_count != 1U ||
	    fake.resolver_fsync_count != 1U ||
	    fake.resolver_close_count != 1U ||
	    fake.resolver_rename_count != 1U ||
	    fake.resolver_unlink_count != 0 || fake.resolver_fd_open ||
	    fake.resolver_temporary_used != 0 || fake.close_count != 2U)
		fail("successful resolver update was not an atomic replacement");
}

enum resolver_failure {
	RESOLVER_WRITE_FAILURE,
	RESOLVER_FSYNC_FAILURE,
	RESOLVER_CLOSE_FAILURE,
	RESOLVER_RENAME_FAILURE
};

static void
test_resolver_failure_preserves_old(enum resolver_failure failure)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};
	const char *previous = "nameserver 192.0.2.53\n";

	reset_fake();
	prepare_resolver_transaction();
	if (failure == RESOLVER_WRITE_FAILURE)
		fake.fail_resolver_write = 1U;
	else if (failure == RESOLVER_FSYNC_FAILURE)
		fake.fail_resolver_fsync = 1U;
	else if (failure == RESOLVER_CLOSE_FAILURE)
		fake.fail_resolver_close = 1U;
	else
		fake.fail_resolver_rename = 1U;
	if (dhcpc_program_main(4, arguments) != 1 || errno != EIO ||
	    strcmp(fake.resolver_current, previous) != 0 ||
	    fake.resolver_open_count != 1U ||
	    fake.resolver_close_count != 1U ||
	    fake.resolver_unlink_count != 1U || fake.resolver_fd_open ||
	    fake.resolver_temporary_used != 0 || fake.close_count != 2U ||
	    strstr(fake.output, "resolver: Input/output error") == NULL)
		fail("resolver failure did not preserve old state and errno");
	if (failure == RESOLVER_WRITE_FAILURE &&
	    (fake.resolver_write_count != 1U ||
	     fake.resolver_fsync_count != 0 || fake.resolver_rename_count != 0))
		fail("resolver write failure advanced the transaction");
	if (failure == RESOLVER_FSYNC_FAILURE &&
	    (fake.resolver_write_count != 1U ||
	     fake.resolver_fsync_count != 1U || fake.resolver_rename_count != 0))
		fail("resolver fsync failure advanced the transaction");
	if (failure == RESOLVER_CLOSE_FAILURE &&
	    (fake.resolver_write_count != 1U ||
	     fake.resolver_fsync_count != 1U || fake.resolver_rename_count != 0))
		fail("resolver close failure advanced the transaction");
	if (failure == RESOLVER_RENAME_FAILURE &&
	    (fake.resolver_write_count != 1U ||
	     fake.resolver_fsync_count != 1U || fake.resolver_rename_count != 1U))
		fail("resolver rename failure did not reach the commit boundary");
	require_original_state();
}

static void
test_route_rollback_failure_diagnostic(void)
{
	char *arguments[] = {"dhcpc", "-t", "1", "ue0", NULL};

	reset_fake();
	fake.carrier_after_sleeps = 0;
	fake.send_succeeds = 1U;
	fake.serve_offer = 1U;
	fake.serve_ack = 1U;
	fake.lease_router = 1U;
	fake.lease_dns = 1U;
	fake.fail_resolver_open = 1U;
	fake.fail_restore_route = 1U;
	fake.require_route_absent_at_send = 1U;
	set_fake_default_route(RTF_UP | RTF_GATEWAY | RTF_STATIC,
	    htonl(UINT32_C(0xc0000201)));
	if (dhcpc_program_main(4, arguments) != 1 ||
	    strstr(fake.output, "rollback: Input/output error") == NULL)
		fail("route rollback failure was not diagnosed");
	require_original_state();
}

static void
test_actual_dhcp_ciaddr_zero(void)
{
	uint8_t packet[600], mac[6] = {2, 0, 0, 0, 0, 1};
	size_t length;
	unsigned index;

	memset(packet, 0xa5, sizeof(packet));
	if (dhcp_build(packet, sizeof(packet), &length, DHCP_DISCOVER,
	    UINT32_C(0x12345678), mac, 0, 0) != 0 || length < 300U)
		fail("actual DISCOVER build failed");
	for (index = 12U; index < 16U; index++)
		if (packet[index] != 0)
			fail("actual DISCOVER ciaddr was not zero");
	memset(packet, 0xa5, sizeof(packet));
	if (dhcp_build(packet, sizeof(packet), &length, DHCP_REQUEST,
	    UINT32_C(0x12345678), mac, htonl(UINT32_C(0x0a000002)),
	    htonl(UINT32_C(0x0a000001))) != 0 || length < 300U)
		fail("actual REQUEST build failed");
	for (index = 12U; index < 16U; index++)
		if (packet[index] != 0)
			fail("actual REQUEST ciaddr was not zero");
}

int
main(void)
{
	test_delayed_carrier();
	test_clean_interface();
	test_timeout_rollback();
	test_partial_clear_rollback();
	test_discover_zero_and_failure_rollback();
	test_invalid_receive_deadline();
	test_ack_timeout_stage();
	test_route_snapshot_failure_preserves_state();
	test_route_delete_failure_restores_state();
	test_static_default_offer_timeout_rollback();
	test_static_default_send_failure_rollback();
	test_static_default_configuration_failure_rollback();
	test_static_default_route_rollback();
	test_static_default_removed_without_dhcp_router();
	test_static_default_replaced_by_dhcp_route();
	test_route_rollback_failure_diagnostic();
	test_resolver_success_commits_atomically();
	test_resolver_failure_preserves_old(RESOLVER_WRITE_FAILURE);
	test_resolver_failure_preserves_old(RESOLVER_FSYNC_FAILURE);
	test_resolver_failure_preserves_old(RESOLVER_CLOSE_FAILURE);
	test_resolver_failure_preserves_old(RESOLVER_RENAME_FAILURE);
	test_success_commits_lease();
	test_actual_dhcp_ciaddr_zero();
	puts("dhcpc state test: PASS");
	return 0;
}
