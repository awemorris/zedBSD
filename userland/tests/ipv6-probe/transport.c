/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The IPv6 transports of the probe (ws130-p003): AF_INET6's UDP, TCP and
 * raw ICMPv6 sockets.
 *
 * On the host itself (ipv6-probe -t): UDP between two sockets on ::1, the
 * source a connect chooses; an AF_INET6 socket on [::] that takes IPv4
 * too (IPV6_V6ONLY 0) gets an IPv4 datagram from an IPv4-mapped source and
 * answers it; it keeps the port from an AF_INET socket on 0.0.0.0, and one
 * that is IPV6_V6ONLY does not; TCP on ::1 (a connect, an accept and its
 * peer, data both ways), an IPv4 connection to an AF_INET6 listener on
 * [::] (an IPv4-mapped peer), a connect refused by a reset; an echo of
 * ICMPv6 to ::1 on a raw socket.  Through the router (ipv6-probe -T, after
 * the core's steps have an address and a default route): an echo to the
 * router's link-local address (the interface named by its scope) and to
 * its global one.  Each line is "IPV6 ..." on standard output; a failure
 * is "IPV6 FAIL step=<what> error=<errno>".
 */

#include "probe.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <uapi/netinet.h>

/*
 * The ports the probe uses on the host itself: after a base that each run
 * takes from its process ID (so that a run soon after another finds them
 * free), below the ephemeral ports.
 */
#define TRANSPORT_PORT_FIRST	30000U
#define TRANSPORT_PORT_RUNS	1500U
#define TRANSPORT_PORT_UDP	(transport_base + 1U)
#define TRANSPORT_PORT_DUAL	(transport_base + 2U)
#define TRANSPORT_PORT_ONLY	(transport_base + 3U)
#define TRANSPORT_PORT_TCP	(transport_base + 4U)
#define TRANSPORT_PORT_TCP_DUAL	(transport_base + 5U)
#define TRANSPORT_PORT_CLOSED	(transport_base + 6U)

/* How long a receive waits (milliseconds), and how long nothing must come for a datagram not to be taken. */
#define TRANSPORT_WAIT_MS	3000
#define TRANSPORT_QUIET_MS	500

/* The ICMPv6 echo's types and its identifier. */
#define TRANSPORT_ECHO_REQUEST	128U
#define TRANSPORT_ECHO_REPLY	129U
#define TRANSPORT_ECHO_ID	0x6b65U

/* The base of this run's ports (probe_transport_local). */
static unsigned transport_base;

/* The loopback address, ::1. */
static const struct in6_addr transport_loopback = { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };

static void transport_name6(struct sockaddr_in6 *name, const struct in6_addr *address, unsigned port, unsigned scope);
static void transport_name4(struct sockaddr_in *name, uint32_t address, unsigned port);
static int transport_mapped(const struct in6_addr *address, uint32_t ipv4);
static int transport_wait(int socket_fd, int timeout_ms);
static int transport_udp(void);
static int transport_dual(void);
static int transport_only(void);
static int transport_tcp(void);
static int transport_tcp_dual(void);
static int transport_refused(void);
static int transport_echo(const struct in6_addr *address, unsigned scope, const char *what);

/*
 * Runs the transports' steps on the host itself.  Returns 0, or 1 after a
 * failure's line.
 */
int
probe_transport_local(void)
{
	pid_t self;
	int status;

	/* This run's ports. */
	self = getpid();
	transport_base = TRANSPORT_PORT_FIRST + ((unsigned)self % TRANSPORT_PORT_RUNS) * 10U;
	printf("IPV6 ports from %u\n", transport_base + 1U);

	/* UDP on ::1, and its source. */
	status = transport_udp();
	if (status != 0)
		return status;

	/* An AF_INET6 socket that takes IPv4 too, and its port. */
	status = transport_dual();
	if (status != 0)
		return status;

	/* One that is IPV6_V6ONLY. */
	status = transport_only();
	if (status != 0)
		return status;

	/* TCP on ::1. */
	status = transport_tcp();
	if (status != 0)
		return status;

	/* An IPv4 connection to an AF_INET6 listener. */
	status = transport_tcp_dual();
	if (status != 0)
		return status;

	/* A connect refused. */
	status = transport_refused();
	if (status != 0)
		return status;

	/* An echo of ICMPv6 to ::1. */
	status = transport_echo(&transport_loopback, 0U, "loopback");
	if (status != 0)
		return status;

	/* Succeeded. */
	printf("IPV6 transports ok\n");
	return 0;
}

/*
 * Runs the transports' steps through the router: an echo to its
 * link-local address on the interface, and to its global one in the
 * prefix (the router answers both).  Returns 0, or 1 after a failure's
 * line.
 */
int
probe_transport_router(
	const struct in6_addr *router,
	unsigned ifindex,
	const struct in6_addr *prefix)
{
	struct in6_addr global;
	int status;

	/* The router's link-local address, on the interface its scope names. */
	status = transport_echo(router, ifindex, "router-link-local");
	if (status != 0)
		return status;

	/* Its global address: the prefix's ::2 (QEMU's user network). */
	global = *prefix;
	global.s6_addr[15] = 2U;
	status = transport_echo(&global, 0U, "router-global");
	if (status != 0)
		return status;

	/* Succeeded. */
	printf("IPV6 router transports ok\n");
	return 0;
}

/* Fills an AF_INET6 socket address. */
static void
transport_name6(
	struct sockaddr_in6 *name,
	const struct in6_addr *address,
	unsigned port,
	unsigned scope)
{
	memset(name, 0, sizeof(*name));
	name->sin6_family = AF_INET6;
	name->sin6_port = htons((uint16_t)port);
	if (address != NULL)
		name->sin6_addr = *address;
	name->sin6_scope_id = scope;
}

/* Fills an AF_INET socket address (the address in host order). */
static void
transport_name4(
	struct sockaddr_in *name,
	uint32_t address,
	unsigned port)
{
	memset(name, 0, sizeof(*name));
	name->sin_family = AF_INET;
	name->sin_port = htons((uint16_t)port);
	name->sin_addr.s_addr = htonl(address);
}

/* Tells whether an address is the IPv4-mapped one of an IPv4 address (host order). */
static int
transport_mapped(
	const struct in6_addr *address,
	uint32_t ipv4)
{
	struct in6_addr expected;
	int same;

	/* ::ffff:a.b.c.d. */
	memset(&expected, 0, sizeof(expected));
	expected.s6_addr[10] = 0xffU;
	expected.s6_addr[11] = 0xffU;
	expected.s6_addr[12] = (uint8_t)(ipv4 >> 24);
	expected.s6_addr[13] = (uint8_t)(ipv4 >> 16);
	expected.s6_addr[14] = (uint8_t)(ipv4 >> 8);
	expected.s6_addr[15] = (uint8_t)ipv4;
	same = memcmp(address, &expected, sizeof(expected));
	if (same != 0)
		return 0;

	/* Succeeded: the mapped address. */
	return 1;
}

/* Waits for a socket to have something to read; returns 1 when it has, 0 when the time ran out. */
static int
transport_wait(
	int socket_fd,
	int timeout_ms)
{
	struct pollfd wanted;
	int ready;

	/* The socket, readable. */
	wanted.fd = socket_fd;
	wanted.events = POLLIN;
	wanted.revents = 0;
	ready = poll(&wanted, 1, timeout_ms);
	if (ready <= 0)
		return 0;

	/* Succeeded: something to read. */
	return 1;
}

/* UDP between two sockets on ::1, and the source a connect chooses. */
static int
transport_udp(void)
{
	struct sockaddr_in6 name;
	struct sockaddr_in6 source;
	socklen_t length;
	char data[16];
	ssize_t got;
	unsigned port;
	int server;
	int client;
	int ready;
	int same;
	int error;

	/* The server on [::1] (the run's first port). */
	server = socket(AF_INET6, SOCK_DGRAM, 0);
	if (server < 0)
		return probe_fail("udp-socket", errno);
	transport_name6(&name, &transport_loopback, TRANSPORT_PORT_UDP, 0U);
	error = bind(server, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("udp-bind", errno);

	/* A datagram from another socket. */
	client = socket(AF_INET6, SOCK_DGRAM, 0);
	if (client < 0)
		return probe_fail("udp-client", errno);
	got = sendto(client, "ping6", 5U, 0, (struct sockaddr *)&name, sizeof(name));
	if (got != 5)
		return probe_fail("udp-sendto", errno);

	/* It comes from ::1. */
	ready = transport_wait(server, TRANSPORT_WAIT_MS);
	if (!ready)
		return probe_fail("udp-receive", ETIMEDOUT);
	length = sizeof(source);
	got = recvfrom(server, data, sizeof(data), 0, (struct sockaddr *)&source, &length);
	same = memcmp(data, "ping6", 5U);
	if (got != 5 || same != 0)
		return probe_fail("udp-data", EPROTO);
	same = memcmp(&source.sin6_addr, &transport_loopback, sizeof(transport_loopback));
	if (length != sizeof(source) ||
	    source.sin6_family != AF_INET6 ||
	    same != 0)
		return probe_fail("udp-source", EPROTO);
	port = ntohs(source.sin6_port);
	printf("IPV6 udp loopback ok port=%u\n", port);

	/* A connect chooses the source the datagrams go from (::1), which getsockname tells. */
	error = connect(client, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("udp-connect", errno);
	length = sizeof(source);
	error = getsockname(client, (struct sockaddr *)&source, &length);
	if (error != 0)
		return probe_fail("udp-getsockname", errno);
	same = memcmp(&source.sin6_addr, &transport_loopback, sizeof(transport_loopback));
	if (same != 0 || source.sin6_port == 0U)
		return probe_fail("udp-connect-source", EPROTO);
	printf("IPV6 udp connect source ok\n");

	/* Succeeded. */
	close(client);
	close(server);
	return 0;
}

/* An AF_INET6 socket on [::] takes IPv4 too, answers it, and keeps its port from an AF_INET socket. */
static int
transport_dual(void)
{
	struct sockaddr_in6 name;
	struct sockaddr_in6 source;
	struct sockaddr_in name4;
	struct sockaddr_in source4;
	socklen_t length;
	char data[16];
	ssize_t got;
	uint32_t address4;
	unsigned port;
	int dual;
	int v4;
	int other;
	int ready;
	int mapped;
	int error;

	/* The socket on [::], the second port. */
	dual = socket(AF_INET6, SOCK_DGRAM, 0);
	if (dual < 0)
		return probe_fail("dual-socket", errno);
	transport_name6(&name, NULL, TRANSPORT_PORT_DUAL, 0U);
	error = bind(dual, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("dual-bind", errno);

	/* An IPv4 datagram to 127.0.0.1 on that port comes from ::ffff:127.0.0.1. */
	v4 = socket(AF_INET, SOCK_DGRAM, 0);
	if (v4 < 0)
		return probe_fail("dual-v4-socket", errno);
	transport_name4(&name4, 0x7f000001U, TRANSPORT_PORT_DUAL);
	got = sendto(v4, "four", 4U, 0, (struct sockaddr *)&name4, sizeof(name4));
	if (got != 4)
		return probe_fail("dual-v4-sendto", errno);
	ready = transport_wait(dual, TRANSPORT_WAIT_MS);
	if (!ready)
		return probe_fail("dual-receive", ETIMEDOUT);
	length = sizeof(source);
	got = recvfrom(dual, data, sizeof(data), 0, (struct sockaddr *)&source, &length);
	mapped = transport_mapped(&source.sin6_addr, 0x7f000001U);
	if (got != 4 ||
	    source.sin6_family != AF_INET6 ||
	    !mapped)
		return probe_fail("dual-mapped-source", EPROTO);

	/* The answer, to the IPv4-mapped source, reaches the IPv4 socket from 127.0.0.1 on that port. */
	got = sendto(dual, "back", 4U, 0, (struct sockaddr *)&source, length);
	if (got != 4)
		return probe_fail("dual-answer", errno);
	ready = transport_wait(v4, TRANSPORT_WAIT_MS);
	if (!ready)
		return probe_fail("dual-answer-receive", ETIMEDOUT);
	length = sizeof(source4);
	got = recvfrom(v4, data, sizeof(data), 0, (struct sockaddr *)&source4, &length);
	address4 = ntohl(source4.sin_addr.s_addr);
	port = ntohs(source4.sin_port);
	if (got != 4 ||
	    address4 != 0x7f000001U ||
	    port != TRANSPORT_PORT_DUAL)
		return probe_fail("dual-answer-source", EPROTO);
	printf("IPV6 dual stack udp ok\n");

	/* An AF_INET socket on 0.0.0.0 and the port is refused: the port is taken for IPv4 too. */
	other = socket(AF_INET, SOCK_DGRAM, 0);
	if (other < 0)
		return probe_fail("dual-other-socket", errno);
	transport_name4(&name4, 0U, TRANSPORT_PORT_DUAL);
	error = bind(other, (struct sockaddr *)&name4, sizeof(name4));
	if (error == 0)
		return probe_fail("dual-port-shared", EEXIST);
	if (errno != EADDRINUSE)
		return probe_fail("dual-port-shared", errno);
	printf("IPV6 dual stack port ok\n");

	/* Succeeded. */
	close(other);
	close(v4);
	close(dual);
	return 0;
}

/* An IPV6_V6ONLY socket on [::] leaves IPv4 and its port to an AF_INET socket. */
static int
transport_only(void)
{
	struct sockaddr_in6 name;
	struct sockaddr_in name4;
	socklen_t length;
	char data[16];
	ssize_t got;
	int only;
	int v4;
	int sender;
	int enabled;
	int ready;
	int error;

	/* IPV6_V6ONLY, then [::] and the third port. */
	only = socket(AF_INET6, SOCK_DGRAM, 0);
	if (only < 0)
		return probe_fail("only-socket", errno);
	enabled = 1;
	error = setsockopt(only, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled));
	if (error != 0)
		return probe_fail("only-set", errno);
	transport_name6(&name, NULL, TRANSPORT_PORT_ONLY, 0U);
	error = bind(only, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("only-bind", errno);

	/* The option reads back, and is not changed once bound. */
	enabled = 0;
	length = sizeof(enabled);
	error = getsockopt(only, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, &length);
	if (error != 0 || enabled != 1)
		return probe_fail("only-get", EPROTO);
	enabled = 0;
	error = setsockopt(only, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled));
	if (error == 0)
		return probe_fail("only-set-bound", EPROTO);

	/* An AF_INET socket on 0.0.0.0 and that port is not refused. */
	v4 = socket(AF_INET, SOCK_DGRAM, 0);
	if (v4 < 0)
		return probe_fail("only-v4-socket", errno);
	transport_name4(&name4, 0U, TRANSPORT_PORT_ONLY);
	error = bind(v4, (struct sockaddr *)&name4, sizeof(name4));
	if (error != 0)
		return probe_fail("only-v4-bind", errno);

	/* An IPv4 datagram goes to it, not to the IPv6-only socket. */
	sender = socket(AF_INET, SOCK_DGRAM, 0);
	if (sender < 0)
		return probe_fail("only-sender", errno);
	transport_name4(&name4, 0x7f000001U, TRANSPORT_PORT_ONLY);
	got = sendto(sender, "four", 4U, 0, (struct sockaddr *)&name4, sizeof(name4));
	if (got != 4)
		return probe_fail("only-sendto", errno);
	ready = transport_wait(v4, TRANSPORT_WAIT_MS);
	if (!ready)
		return probe_fail("only-v4-receive", ETIMEDOUT);
	got = recv(v4, data, sizeof(data), 0);
	if (got != 4)
		return probe_fail("only-v4-data", EPROTO);
	ready = transport_wait(only, TRANSPORT_QUIET_MS);
	if (ready)
		return probe_fail("only-took-ipv4", EPROTO);
	printf("IPV6 v6only ok\n");

	/* Succeeded. */
	close(sender);
	close(v4);
	close(only);
	return 0;
}

/* TCP on ::1: a connect, an accept and its peer, data both ways. */
static int
transport_tcp(void)
{
	struct sockaddr_in6 name;
	struct sockaddr_in6 peer;
	struct sockaddr_in6 local;
	socklen_t length;
	char data[16];
	ssize_t got;
	int listener;
	int client;
	int accepted;
	int same;
	int error;

	/* A listener on [::1], the fourth port. */
	listener = socket(AF_INET6, SOCK_STREAM, 0);
	if (listener < 0)
		return probe_fail("tcp-socket", errno);
	transport_name6(&name, &transport_loopback, TRANSPORT_PORT_TCP, 0U);
	error = bind(listener, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("tcp-bind", errno);
	error = listen(listener, 4);
	if (error != 0)
		return probe_fail("tcp-listen", errno);

	/* A connect, and the accept. */
	client = socket(AF_INET6, SOCK_STREAM, 0);
	if (client < 0)
		return probe_fail("tcp-client", errno);
	error = connect(client, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("tcp-connect", errno);
	length = sizeof(peer);
	accepted = accept(listener, (struct sockaddr *)&peer, &length);
	if (accepted < 0)
		return probe_fail("tcp-accept", errno);

	/* The peer is the client: ::1 and its port. */
	length = sizeof(local);
	error = getsockname(client, (struct sockaddr *)&local, &length);
	if (error != 0)
		return probe_fail("tcp-getsockname", errno);
	same = memcmp(&peer.sin6_addr, &transport_loopback, sizeof(transport_loopback));
	if (peer.sin6_family != AF_INET6 ||
	    same != 0 ||
	    peer.sin6_port != local.sin6_port)
		return probe_fail("tcp-peer", EPROTO);

	/* Data both ways. */
	got = write(client, "hello", 5U);
	if (got != 5)
		return probe_fail("tcp-write", errno);
	got = read(accepted, data, sizeof(data));
	same = memcmp(data, "hello", 5U);
	if (got != 5 || same != 0)
		return probe_fail("tcp-read", EPROTO);
	got = write(accepted, "world", 5U);
	if (got != 5)
		return probe_fail("tcp-write-back", errno);
	got = read(client, data, sizeof(data));
	same = memcmp(data, "world", 5U);
	if (got != 5 || same != 0)
		return probe_fail("tcp-read-back", EPROTO);
	printf("IPV6 tcp loopback ok\n");

	/* Succeeded. */
	close(accepted);
	close(client);
	close(listener);
	return 0;
}

/* An IPv4 connection to an AF_INET6 listener on [::]: the peer is IPv4-mapped. */
static int
transport_tcp_dual(void)
{
	struct sockaddr_in6 name;
	struct sockaddr_in6 peer;
	struct sockaddr_in name4;
	socklen_t length;
	char data[16];
	ssize_t got;
	int listener;
	int client;
	int accepted;
	int mapped;
	int same;
	int error;

	/* A listener on [::], the fifth port. */
	listener = socket(AF_INET6, SOCK_STREAM, 0);
	if (listener < 0)
		return probe_fail("tcp-dual-socket", errno);
	transport_name6(&name, NULL, TRANSPORT_PORT_TCP_DUAL, 0U);
	error = bind(listener, (struct sockaddr *)&name, sizeof(name));
	if (error != 0)
		return probe_fail("tcp-dual-bind", errno);
	error = listen(listener, 4);
	if (error != 0)
		return probe_fail("tcp-dual-listen", errno);

	/* An IPv4 connect to 127.0.0.1 on that port, accepted with the peer ::ffff:127.0.0.1. */
	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0)
		return probe_fail("tcp-dual-client", errno);
	transport_name4(&name4, 0x7f000001U, TRANSPORT_PORT_TCP_DUAL);
	error = connect(client, (struct sockaddr *)&name4, sizeof(name4));
	if (error != 0)
		return probe_fail("tcp-dual-connect", errno);
	length = sizeof(peer);
	accepted = accept(listener, (struct sockaddr *)&peer, &length);
	if (accepted < 0)
		return probe_fail("tcp-dual-accept", errno);
	mapped = transport_mapped(&peer.sin6_addr, 0x7f000001U);
	if (peer.sin6_family != AF_INET6 || !mapped)
		return probe_fail("tcp-dual-peer", EPROTO);

	/* Data. */
	got = write(client, "four", 4U);
	if (got != 4)
		return probe_fail("tcp-dual-write", errno);
	got = read(accepted, data, sizeof(data));
	same = memcmp(data, "four", 4U);
	if (got != 4 || same != 0)
		return probe_fail("tcp-dual-read", EPROTO);
	printf("IPV6 dual stack tcp ok\n");

	/* Succeeded. */
	close(accepted);
	close(client);
	close(listener);
	return 0;
}

/* A connect to a port nobody listens on is refused (the reset comes over IPv6). */
static int
transport_refused(void)
{
	struct sockaddr_in6 name;
	int client;
	int error;

	/* [::1] and the sixth port have no listener. */
	client = socket(AF_INET6, SOCK_STREAM, 0);
	if (client < 0)
		return probe_fail("refused-socket", errno);
	transport_name6(&name, &transport_loopback, TRANSPORT_PORT_CLOSED, 0U);
	error = connect(client, (struct sockaddr *)&name, sizeof(name));
	if (error == 0)
		return probe_fail("refused", EPROTO);
	if (errno != ECONNREFUSED)
		return probe_fail("refused", errno);
	printf("IPV6 tcp refused ok\n");

	/* Succeeded. */
	close(client);
	return 0;
}

/* An echo of ICMPv6 on a raw socket: the request to an address (on an interface when scope is given), and its reply. */
static int
transport_echo(
	const struct in6_addr *address,
	unsigned scope,
	const char *what)
{
	struct sockaddr_in6 name;
	struct sockaddr_in6 source;
	socklen_t length;
	uint8_t message[16];
	uint8_t answer[256];
	ssize_t got;
	int raw;
	int ready;
	int same;
	int tries;

	/* A raw ICMPv6 socket. */
	raw = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (raw < 0)
		return probe_fail("echo-socket", errno);

	/* The request: type, code, checksum (the kernel's), identifier, sequence, data. */
	memset(message, 0, sizeof(message));
	message[0] = TRANSPORT_ECHO_REQUEST;
	message[4] = (uint8_t)(TRANSPORT_ECHO_ID >> 8);
	message[5] = (uint8_t)TRANSPORT_ECHO_ID;
	message[7] = 1U;
	memcpy(message + 8, "zed-ipv6", 8U);
	transport_name6(&name, address, 0U, scope);
	got = sendto(raw, message, sizeof(message), 0, (struct sockaddr *)&name, sizeof(name));
	if (got != (ssize_t)sizeof(message))
		return probe_fail("echo-sendto", errno);

	/* The reply among the messages that come (the request itself comes round the loopback too). */
	for (tries = 0; tries < 8; tries++) {
		ready = transport_wait(raw, TRANSPORT_WAIT_MS);
		if (!ready)
			break;
		length = sizeof(source);
		got = recvfrom(raw, answer, sizeof(answer), 0, (struct sockaddr *)&source, &length);
		if (got < 8)
			continue;
		if (answer[0] != TRANSPORT_ECHO_REPLY ||
		    answer[4] != message[4] ||
		    answer[5] != message[5])
			continue;
		same = memcmp(&source.sin6_addr, address, sizeof(*address));
		if (same != 0)
			continue;

		/* Succeeded: the reply, from the address. */
		printf("IPV6 echo %s ok\n", what);
		close(raw);
		return 0;
	}

	/* No reply. */
	close(raw);
	return probe_fail("echo", ETIMEDOUT);
}
