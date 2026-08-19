/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void write16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t read16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static void write64(uint8_t *p, uint64_t v) { unsigned i; for (i = 0; i < 8U; i++) p[i] = (uint8_t)(v >> (56U - i * 8U)); }
static uint64_t read64(const uint8_t *p) { uint64_t v = 0; unsigned i; for (i = 0; i < 8U; i++) v = v << 8 | p[i]; return v; }

static int usage(void)
{ puts("usage: ping [-c count] [-i interval] [-W timeout] host"); return 2; }

int
main(int argc, char **argv)
{
	struct addrinfo hints, *addresses;
	struct sockaddr_in peer, source;
	struct timeval receive_timeout;
	uint8_t echo[64], packet[2048];
	uint32_t count = 4, interval_ms = 1000, timeout_ms = 1000;
	uint64_t minimum = 0, maximum = 0, total = 0;
	uint16_t identifier;
	unsigned transmitted = 0, received = 0, sequence, arg = 1;
	char numeric[16];
	int descriptor, error;

	while (arg < (unsigned)argc && argv[arg][0] == '-') {
		if (arg + 1U >= (unsigned)argc) return usage();
		if (strcmp(argv[arg], "-c") == 0) { char *end; unsigned long v = strtoul(argv[arg + 1], &end, 10); if (*end != '\0' || v == 0 || v > 65535U) return usage(); count = (uint32_t)v; }
		else if (strcmp(argv[arg], "-i") == 0) { if (netutil_parse_milliseconds(argv[arg + 1], &interval_ms) != 0) return usage(); }
		else if (strcmp(argv[arg], "-W") == 0) { if (netutil_parse_milliseconds(argv[arg + 1], &timeout_ms) != 0 || timeout_ms == 0) return usage(); }
		else return usage();
		arg += 2;
	}
	if (arg + 1U != (unsigned)argc) return usage();
	memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_RAW; hints.ai_protocol = IPPROTO_ICMP;
	error = getaddrinfo(argv[arg], NULL, &hints, &addresses);
	if (error != 0) { printf("ping: %s: %s\n", argv[arg], gai_strerror(error)); return 1; }
	peer = *(const struct sockaddr_in *)addresses->ai_addr; freeaddrinfo(addresses);
	inet_ntop(AF_INET, &peer.sin_addr, numeric, sizeof(numeric));
	printf("PING %s (%s): 56 data bytes\n", argv[arg], numeric);
	descriptor = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (descriptor < 0) { printf("ping: socket: %s\n", strerror(errno)); return 1; }
	receive_timeout.tv_sec = (time_t)(timeout_ms / 1000U); receive_timeout.tv_usec = (long)(timeout_ms % 1000U) * 1000L;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
	identifier = (uint16_t)(netutil_monotonic_us() ^ 0x5a42U);
	for (sequence = 1; sequence <= count; sequence++) {
		uint64_t deadline, sent;
		struct timespec retry = { 0, 20000000L };
		ssize_t length;
		unsigned i;
		memset(echo, 0, sizeof(echo)); echo[0] = 8; write16(echo + 4, identifier); write16(echo + 6, (uint16_t)sequence);
		sent = netutil_monotonic_us(); write64(echo + 8, sent); for (i = 16; i < sizeof(echo); i++) echo[i] = (uint8_t)i;
		deadline = sent + (uint64_t)timeout_ms * 1000U;
		do { length = sendto(descriptor, echo, sizeof(echo), 0, (struct sockaddr *)&peer, sizeof(peer)); if (length == (ssize_t)sizeof(echo)) break; if (errno != EAGAIN) break; nanosleep(&retry, NULL); } while (netutil_monotonic_us() < deadline);
		transmitted++;
		if (length == (ssize_t)sizeof(echo)) {
			while (netutil_monotonic_us() < deadline) {
				socklen_t source_length = sizeof(source); size_t ihl; uint8_t *icmp;
				length = recvfrom(descriptor, packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_length);
				if (length < 28) break;
				ihl = (size_t)(packet[0] & 15U) * 4U;
				if ((packet[0] >> 4) != 4 || ihl < 20U || ihl + 16U > (size_t)length || packet[9] != IPPROTO_ICMP || source.sin_addr.s_addr != peer.sin_addr.s_addr) continue;
				icmp = packet + ihl;
				if (icmp[0] != 0 || icmp[1] != 0 || read16(icmp + 4) != identifier || read16(icmp + 6) != sequence) continue;
				{ uint64_t rtt = netutil_monotonic_us() - read64(icmp + 8); received++; total += rtt; if (minimum == 0 || rtt < minimum) minimum = rtt; if (rtt > maximum) maximum = rtt; printf("%ld bytes from %s: icmp_seq=%u ttl=%u time=%llu.%03llu ms\n", (long)((size_t)length - ihl), numeric, sequence, packet[8], (unsigned long long)(rtt / 1000U), (unsigned long long)(rtt % 1000U)); }
				break;
			}
		}
		if (sequence != count) { struct timespec delay = { (time_t)(interval_ms / 1000U), (int32_t)(interval_ms % 1000U) * 1000000L }; nanosleep(&delay, NULL); }
	}
	close(descriptor);
	printf("--- %s ping statistics ---\n%u packets transmitted, %u packets received, %u%% packet loss\n", argv[arg], transmitted, received, transmitted == 0 ? 0 : (transmitted - received) * 100U / transmitted);
	if (received != 0) printf("round-trip min/avg/max = %llu.%03llu/%llu.%03llu/%llu.%03llu ms\n", (unsigned long long)(minimum / 1000U), (unsigned long long)(minimum % 1000U), (unsigned long long)(total / received / 1000U), (unsigned long long)(total / received % 1000U), (unsigned long long)(maximum / 1000U), (unsigned long long)(maximum % 1000U));
	return received == 0;
}
