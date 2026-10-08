/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD ping userland command.
 *
 * The program is installed set-user-ID root, as on the BSDs, because an
 * ICMP echo needs a raw socket and only the superuser may open one.  It
 * opens that socket first and then gives the privilege up for good.
 */

#include "userland/base/net/netutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int usage(void);
static void write16(uint8_t *p, uint16_t v);
static void write64(uint8_t *p, uint64_t v);
static uint16_t read16(const uint8_t *p);
static uint64_t read64(const uint8_t *p);
static int ping6(int descriptor, const struct sockaddr_in6 *peer, const char *name, uint32_t count, uint32_t interval_ms, uint32_t timeout_ms);
static ssize_t ping6_receive(int descriptor, uint8_t *packet, size_t size, struct sockaddr_in6 *source, int *hop_limit);

/*
 * Runs the ping command.
 */
int
main(
	int argc,
	char **argv)
{
	int function_result;
	char *end;
	unsigned long v;
	uint64_t rtt;
	socklen_t source_length;
	size_t ihl;
	uint8_t *icmp;
	uint64_t deadline, sent;
	ssize_t length;
	unsigned i;
	struct addrinfo hints, *addresses;
	struct sockaddr_in peer, source;
	struct timeval receive_timeout;
	uint8_t echo[64], packet[2048];
	uint32_t count, interval_ms, timeout_ms;
	uint64_t minimum, maximum, total;
	uint16_t identifier;
	unsigned transmitted, received, sequence, arg;
	char numeric[16];
	int descriptor, error, socket_error;
	int descriptor6;
	int socket6_error;
	int family;
	struct sockaddr_in6 peer6;
	uid_t real_user;
	struct timespec retry;
	struct timespec delay;

	count = 4;
	interval_ms = 1000;
	timeout_ms = 1000;
	minimum = 0;
	maximum = 0;
	total = 0;
	transmitted = 0;
	received = 0;
	arg = 1;

	/*
	 * Opens the ICMP socket while the set-user-ID bit still lends the
	 * superuser's privilege.  A failure is kept and reported after the
	 * arguments have been checked, where the program always reported it.
	 */
	descriptor = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	socket_error = errno;
	descriptor6 = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	socket6_error = errno;

	/*
	 * Gives the lent privilege up for good before any argument is read.
	 * Setting the user identity as the superuser replaces the real, the
	 * effective and the saved one, so it cannot be taken back later.
	 */
	real_user = getuid();
	error = setuid(real_user);
	if (error != 0) {
		printf("ping: setuid: %s\n", strerror(errno));

		/* Refuses to run with privilege it could not give up. */
		return 1;
	}

	/* -4 and -6 choose the family (ws130-p005; either by the name's addresses otherwise). */
	family = AF_UNSPEC;

	/* Process each remaining command-line operand. */
	while (arg < (unsigned)argc && argv[arg][0] == '-') {
		/* The family, a flag without an argument, anywhere among the options. */
		if (strcmp(argv[arg], "-4") == 0 || strcmp(argv[arg], "-6") == 0) {
			family = argv[arg][1] == '4' ? AF_INET : AF_INET6;
			arg++;
			continue;
		}

		/* Validates the command-line arguments. */
		if (arg + 1U >= (unsigned)argc) {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the selected command-line operation. */
		if (strcmp(argv[arg], "-c") == 0) {
			v = strtoul(argv[arg + 1], &end, 10);

			/* Checks the current endpoint. */
			if (*end != '\0' || v == 0 || v > 65535U) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
			count = (uint32_t)v;
		} else if (strcmp(argv[arg], "-i") == 0) {
			/* Validates the command-line arguments. */
			if (netutil_parse_milliseconds(argv[arg + 1],
						       &interval_ms) != 0) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
		} else if (strcmp(argv[arg], "-W") == 0) {
			/* Validates the command-line arguments. */
			if (netutil_parse_milliseconds(argv[arg + 1],
						       &timeout_ms) != 0 ||
			    timeout_ms == 0) {
				/* Obtains the usage result. */
				function_result = usage();

				/* Returns the computed result. */
				return function_result;
			}
		} else {
			/* Obtains the usage result. */
			function_result = usage();

			/* Returns the computed result. */
			return function_result;
		}
		arg += 2;
	}

	/* Validates the command-line arguments. */
	if (arg + 1U != (unsigned)argc) {
		/* Obtains the usage result. */
		function_result = usage();

		/* Returns the computed result. */
		return function_result;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_RAW;
	error = getaddrinfo(argv[arg], NULL, &hints, &addresses);

	/* Handles an operation failure. */
	if (error != 0) {
		printf("ping: %s: %s\n", argv[arg], gai_strerror(error));

		/* Reports operation failure. */
		return 1;
	}

	/* An IPv6 address: ICMPv6's echo (ws130-p005). */
	if (addresses->ai_family == AF_INET6) {
		peer6 = *(const struct sockaddr_in6 *)addresses->ai_addr;
		freeaddrinfo(addresses);
		if (descriptor >= 0)
			close(descriptor);
		if (descriptor6 < 0) {
			printf("ping: socket: %s\n", strerror(socket6_error));
			return 1;
		}
		error = ping6(descriptor6, &peer6, argv[arg], count, interval_ms, timeout_ms);
		return error;
	}

	/* An IPv4 one. */
	if (descriptor6 >= 0)
		close(descriptor6);
	peer = *(const struct sockaddr_in *)addresses->ai_addr;
	freeaddrinfo(addresses);
	inet_ntop(AF_INET, &peer.sin_addr, numeric, sizeof(numeric));
	printf("PING %s (%s): 56 data bytes\n", argv[arg], numeric);

	/* Reports why the ICMP socket opened at the start is missing. */
	if (descriptor < 0) {
		printf("ping: socket: %s\n", strerror(socket_error));

		/* Reports operation failure. */
		return 1;
	}
	receive_timeout.tv_sec = (time_t)(timeout_ms / 1000U);
	receive_timeout.tv_usec = (long)(timeout_ms % 1000U) * 1000L;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
			 sizeof(receive_timeout));

	/* Process each remaining element. */
	identifier = (uint16_t)(netutil_monotonic_us() ^ 0x5a42U);
	for (sequence = 1; sequence <= count; sequence++) {
		retry.tv_sec = 0;
		retry.tv_nsec = 20000000L;
		memset(echo, 0, sizeof(echo));
		echo[0] = 8;
		write16(echo + 4, identifier);
		write16(echo + 6, (uint16_t)sequence);
		sent = netutil_monotonic_us();
		write64(echo + 8, sent);

		/* Process each remaining element. */
		for (i = 16; i < sizeof(echo); i++)
			echo[i] = (uint8_t)i;
		deadline = sent + (uint64_t)timeout_ms * 1000U;
		do {
			length = sendto(descriptor, echo, sizeof(echo), 0,
					(struct sockaddr *)&peer, sizeof(peer));

			/* Checks the current data length. */
			if (length == (ssize_t)sizeof(echo))
				break;

			/* Handles the reported system error. */
			if (errno != EAGAIN)
				break;
			nanosleep(&retry, NULL);
		} while (netutil_monotonic_us() < deadline);
		transmitted++;

		/* Checks the current data length. */
		if (length == (ssize_t)sizeof(echo)) {
			/* Continue while the operation condition remains true. */
			while (netutil_monotonic_us() < deadline) {
				source_length = sizeof(source);
				length = recvfrom(
				    descriptor, packet, sizeof(packet), 0,
				    (struct sockaddr *)&source, &source_length);

				/* Checks the current data length. */
				if (length < 28)
					break;
				ihl = (size_t)(packet[0] & 15U) * 4U;

				/* Handles the packet condition. */
				if ((packet[0] >> 4) != 4 || ihl < 20U ||
				    ihl + 16U > (size_t)length ||
				    packet[9] != IPPROTO_ICMP ||
				    source.sin_addr.s_addr !=
					peer.sin_addr.s_addr)
					continue;
				icmp = packet + ihl;

				/* Handles a failed read16 operation. */
				if (icmp[0] != 0 || icmp[1] != 0 ||
				    read16(icmp + 4) != identifier ||
				    read16(icmp + 6) != sequence)
					continue;

				rtt = netutil_monotonic_us() -
					       read64(icmp + 8);
				received++;
				total += rtt;

				/*
				 * The first reply sets the minimum; a round
				 * trip of zero is a real measurement, not an
				 * unset minimum.
				 */
				if (received == 1) {
					minimum = rtt;
				} else if (rtt < minimum) {
					/* A faster reply lowers the minimum. */
					minimum = rtt;
				}

				/* Handles the rtt condition. */
				if (rtt > maximum)
					maximum = rtt;
				printf(
				    "%ld bytes from %s: icmp_seq=%u "
				    "ttl=%u time=%llu.%03llu ms\n",
				    (long)((size_t)length - ihl),
				    numeric, sequence, packet[8],
				    (unsigned long long)(rtt / 1000U),
				    (unsigned long long)(rtt % 1000U));
				break;
			}
		}

		/* Handles the sequence condition. */
		if (sequence != count) {
			delay.tv_sec = (time_t)(interval_ms / 1000U);
			delay.tv_nsec =
				(int32_t)(interval_ms % 1000U) * 1000000L;
			nanosleep(&delay, NULL);
		}
	}
	close(descriptor);
	printf("--- %s ping statistics ---\n%u packets transmitted, %u packets "
	       "received, %u%% packet loss\n",
	       argv[arg], transmitted, received,
	       transmitted == 0
		   ? 0
		   : (transmitted - received) * 100U / transmitted);

	/* Handles the received condition. */
	if (received != 0) {
		printf("round-trip min/avg/max = "
		       "%llu.%03llu/%llu.%03llu/%llu.%03llu ms\n",
		       (unsigned long long)(minimum / 1000U),
		       (unsigned long long)(minimum % 1000U),
		       (unsigned long long)(total / received / 1000U),
		       (unsigned long long)(total / received % 1000U),
		       (unsigned long long)(maximum / 1000U),
		       (unsigned long long)(maximum % 1000U));
	}

	/* Returns the computed result. */
	return received == 0;
}

/* Supports the usage operation. */
static int
usage(
	void)
{
	puts("usage: ping [-4|-6] [-c count] [-i interval] [-W timeout] host");

	/* Reports operation failure. */
	return 2;
}

/* Supports the write16 operation. */
static void
write16(
	uint8_t *p,
	uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* Supports the write64 operation. */
static void
write64(
	uint8_t *p,
	uint64_t v)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < 8U; i++)
		p[i] = (uint8_t)(v >> (56U - i * 8U));
}

/* Supports the read16 operation. */
static uint16_t
read16(
	const uint8_t *p)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Supports the read64 operation. */
static uint64_t
read64(
	const uint8_t *p)
{
	uint64_t v;
	unsigned i;

	/* Process each element required by the operation. */
	v = 0;
	for (i = 0; i < 8U; i++)
		v = v << 8 | p[i];

	/* Returns the computed result. */
	return v;
}

/*
 * Sends ICMPv6 echo requests to an IPv6 peer and reads its replies
 * (ws130-p005), the same way as the IPv4 echo: a raw ICMPv6 socket gives
 * the message without its IPv6 header (RFC 3542) and fills the checksum.
 * Returns 0 when a reply came.
 */
static int
ping6(
	int descriptor,
	const struct sockaddr_in6 *peer,
	const char *name,
	uint32_t count,
	uint32_t interval_ms,
	uint32_t timeout_ms)
{
	struct sockaddr_in6 source;
	struct timeval receive_timeout;
	struct timespec delay;
	uint8_t echo[64];
	uint8_t packet[2048];
	char numeric[INET6_ADDRSTRLEN + IF_NAMESIZE + 1];
	char zone[IF_NAMESIZE];
	char hop_text[16];
	const char *written;
	const char *named;
	ssize_t length;
	uint64_t minimum;
	uint64_t maximum;
	uint64_t total;
	uint64_t sent;
	uint64_t now;
	uint64_t deadline;
	uint64_t rtt;
	uint16_t identifier;
	unsigned transmitted;
	unsigned received;
	unsigned sequence;
	unsigned loss;
	unsigned i;
	int same;
	int enabled;
	int hop_limit;

	/* The peer as text, a link-local one with its interface. */
	written = inet_ntop(AF_INET6, &peer->sin6_addr, numeric, sizeof(numeric));
	if (written == NULL)
		strcpy(numeric, "?");
	named = NULL;
	if (peer->sin6_scope_id != 0U)
		named = if_indextoname(peer->sin6_scope_id, zone);
	if (named != NULL) {
		i = (unsigned)strlen(numeric);
		snprintf(numeric + i, sizeof(numeric) - i, "%%%s", zone);
	}
	printf("PING6 %s (%s): 56 data bytes\n", name, numeric);
	receive_timeout.tv_sec = (time_t)(timeout_ms / 1000U);
	receive_timeout.tv_usec = (long)(timeout_ms % 1000U) * 1000L;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

	/* The hop limit of each reply (ws177-p044); a kernel without it shows none. */
	enabled = 1;
	(void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &enabled, sizeof(enabled));

	/* Each echo. */
	identifier = (uint16_t)(netutil_monotonic_us() ^ 0x5a43U);
	minimum = 0;
	maximum = 0;
	total = 0;
	transmitted = 0;
	received = 0;
	for (sequence = 1; sequence <= count; sequence++) {
		/* The request: type 128, the identifier, the sequence and the time sent. */
		memset(echo, 0, sizeof(echo));
		echo[0] = 128;
		write16(echo + 4, identifier);
		write16(echo + 6, (uint16_t)sequence);
		sent = netutil_monotonic_us();
		write64(echo + 8, sent);
		for (i = 16; i < sizeof(echo); i++)
			echo[i] = (uint8_t)i;
		deadline = sent + (uint64_t)timeout_ms * 1000U;
		length = sendto(descriptor, echo, sizeof(echo), 0, (const struct sockaddr *)peer, sizeof(*peer));
		transmitted++;

		/* Its reply: type 129 from the peer with the same identifier and sequence. */
		while (length == (ssize_t)sizeof(echo)) {
			now = netutil_monotonic_us();
			if (now >= deadline)
				break;
			length = ping6_receive(descriptor, packet, sizeof(packet), &source, &hop_limit);
			if (length < 16)
				break;
			same = memcmp(&source.sin6_addr, &peer->sin6_addr, sizeof(source.sin6_addr));
			if (same != 0 || packet[0] != 129) {
				length = (ssize_t)sizeof(echo);
				continue;
			}
			if (read16(packet + 4) != identifier || read16(packet + 6) != sequence) {
				length = (ssize_t)sizeof(echo);
				continue;
			}

			/* Its round trip. */
			rtt = netutil_monotonic_us() - read64(packet + 8);
			received++;
			total += rtt;
			if (received == 1 || rtt < minimum)
				minimum = rtt;
			if (rtt > maximum)
				maximum = rtt;
			hop_text[0] = '\0';
			if (hop_limit >= 0)
				(void)snprintf(hop_text, sizeof(hop_text), " hlim=%d", hop_limit);
			printf("%ld bytes from %s: icmp_seq=%u%s time=%llu.%03llu ms\n", (long)length, numeric, sequence, hop_text,
			       (unsigned long long)(rtt / 1000U), (unsigned long long)(rtt % 1000U));
			break;
		}

		/* The interval before the next. */
		if (sequence != count) {
			delay.tv_sec = (time_t)(interval_ms / 1000U);
			delay.tv_nsec = (long)(interval_ms % 1000U) * 1000000L;
			nanosleep(&delay, NULL);
		}
	}
	close(descriptor);

	/* The statistics. */
	loss = 0;
	if (transmitted != 0)
		loss = (transmitted - received) * 100U / transmitted;
	printf("--- %s ping6 statistics ---\n%u packets transmitted, %u packets received, %u%% packet loss\n", name, transmitted, received, loss);
	if (received != 0) {
		printf("round-trip min/avg/max = %llu.%03llu/%llu.%03llu/%llu.%03llu ms\n",
		       (unsigned long long)(minimum / 1000U), (unsigned long long)(minimum % 1000U),
		       (unsigned long long)(total / received / 1000U), (unsigned long long)(total / received % 1000U),
		       (unsigned long long)(maximum / 1000U), (unsigned long long)(maximum % 1000U));
	}

	/* Succeeded when a reply came. */
	if (received == 0)
		return 1;
	return 0;
}

/*
 * Receives one ICMPv6 message: its bytes, its sender, and the hop limit it
 * came with (the IPV6_HOPLIMIT control message; -1 without one).
 * Returns recvmsg's answer.
 */
static ssize_t
ping6_receive(
	int descriptor,
	uint8_t *packet,
	size_t size,
	struct sockaddr_in6 *source,
	int *hop_limit)
{
	union {
		struct cmsghdr header;
		unsigned char room[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *item;
	struct msghdr message;
	struct iovec part;
	ssize_t length;
	size_t needed;

	/* The message, its sender and room for one control message. */
	*hop_limit = -1;
	part.iov_base = packet;
	part.iov_len = size;
	memset(&message, 0, sizeof(message));
	message.msg_name = source;
	message.msg_namelen = sizeof(*source);
	message.msg_iov = &part;
	message.msg_iovlen = 1;
	message.msg_control = control.room;
	message.msg_controllen = sizeof(control.room);
	length = recvmsg(descriptor, &message, 0);
	if (length < 0)
		return length;

	/* The hop limit among the control messages. */
	needed = CMSG_LEN(sizeof(int));
	for (item = CMSG_FIRSTHDR(&message);
	     item != NULL;
	     item = CMSG_NXTHDR(&message, item)) {
		if (item->cmsg_level != IPPROTO_IPV6 || item->cmsg_type != IPV6_HOPLIMIT)
			continue;
		if (item->cmsg_len >= needed)
			memcpy(hop_limit, CMSG_DATA(item), sizeof(int));
	}

	/* The message's length. */
	return length;
}
