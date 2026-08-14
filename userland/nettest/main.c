/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int
set_address(int descriptor, unsigned command, uint32_t address)
{
	struct ifreq request;
	struct sockaddr_in *inet = (struct sockaddr_in *)&request.ifr_addr;

	memset(&request, 0, sizeof(request));
	strcpy(request.ifr_name, "ne0");
	inet->sin_family = AF_INET;
	inet->sin_addr.s_addr = htonl(address);
	return ioctl(descriptor, command, &request);
}

static int
udp_test(const struct sockaddr_in *gateway)
{
	static const char message[] = "zedBSD UDP echo";
	struct sockaddr_in server = *gateway;
	struct timespec delay = { 0, 100000000L };
	char response[sizeof(message)];
	ssize_t count;
	int attempt, descriptor;

	server.sin_port = htons(8081);
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0) {
		printf("nettest: UDP socket failed (%d)\n", errno);
		return 1;
	}
	count = sendto(descriptor, message, sizeof(message) - 1U, 0,
	    (const struct sockaddr *)&server, sizeof(server));
	if (count != (ssize_t)(sizeof(message) - 1U)) {
		printf("nettest: UDP send failed (%d)\n", errno);
		close(descriptor);
		return 1;
	}
	for (attempt = 0; attempt < 30; attempt++) {
		count = recvfrom(descriptor, response, sizeof(response),
		    MSG_DONTWAIT, NULL, NULL);
		if (count == (ssize_t)(sizeof(message) - 1U) &&
		    memcmp(response, message, sizeof(message) - 1U) == 0) {
			puts("nettest: UDP echo reply received");
			close(descriptor);
			return 0;
		}
		if (count < 0 && errno != EAGAIN) {
			printf("nettest: UDP receive failed (%d)\n", errno);
			close(descriptor);
			return 1;
		}
		(void)nanosleep(&delay, NULL);
	}
	puts("nettest: UDP echo timed out");
	close(descriptor);
	return 1;
}

static int
http_test(const struct sockaddr_in *gateway)
{
	static const char request[] =
	    "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
	struct sockaddr_in server = *gateway;
	char response[256];
	ssize_t count;
	int descriptor;

	server.sin_port = htons(8080);
	descriptor = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (descriptor < 0) {
		printf("nettest: TCP socket failed (%d)\n", errno);
		return 1;
	}
	if (connect(descriptor, (const struct sockaddr *)&server,
	    sizeof(server)) != 0) {
		printf("nettest: TCP connect failed (%d)\n", errno);
		close(descriptor);
		return 1;
	}
	count = send(descriptor, request, sizeof(request) - 1U, 0);
	if (count != (ssize_t)(sizeof(request) - 1U)) {
		printf("nettest: HTTP request failed (count=%ld errno=%d)\n",
		    (long)count, errno);
		close(descriptor);
		return 1;
	}
	count = recv(descriptor, response, sizeof(response), 0);
	if (count <= 0) {
		printf("nettest: HTTP response failed (%d)\n", errno);
		close(descriptor);
		return 1;
	}
	printf("nettest: TCP HTTP response received (%ld bytes)\n",
	    (long)count);
	close(descriptor);
	return 0;
}

int
main(int argc, char **argv)
{
	struct sockaddr_in peer, source;
	struct timespec delay = { 0, 100000000L };
	uint8_t echo[16] = { 8, 0, 0, 0, 0x5a, 0x42, 0, 1,
	    'z','e','d','B','S','D','!','!' };
	uint8_t packet[64];
	socklen_t source_length;
	int descriptor, attempt;
	ssize_t count;

	descriptor = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (descriptor < 0) {
		printf("nettest: socket failed (%d)\n", errno);
		return 1;
	}
	if (set_address(descriptor, SIOCSIFADDR, 0x0a00020fU) != 0 ||
	    set_address(descriptor, SIOCSIFNETMASK, 0xffffff00U) != 0) {
		printf("nettest: configuring ne0 failed (%d)\n", errno);
		close(descriptor);
		return 1;
	}
	memset(&peer, 0, sizeof(peer));
	peer.sin_family = AF_INET;
	if (argc > 1) {
		if (inet_pton(AF_INET, argv[1], &peer.sin_addr) != 1) {
			printf("nettest: invalid IPv4 address: %s\n", argv[1]);
			close(descriptor);
			return 1;
		}
	} else {
		peer.sin_addr.s_addr = htonl(0x0a000202U);
	}
	for (attempt = 0; attempt < 10; attempt++) {
		count = sendto(descriptor, echo, sizeof(echo), 0,
		    (struct sockaddr *)&peer, sizeof(peer));
		if (count == (ssize_t)sizeof(echo))
			break;
		if (errno != EAGAIN) {
			printf("nettest: echo send failed (%d)\n", errno);
			close(descriptor);
			return 1;
		}
		(void)nanosleep(&delay, NULL);
	}
	if (attempt == 10) {
		puts("nettest: ARP resolution timed out");
		close(descriptor);
		return 1;
	}
	for (attempt = 0; attempt < 30; attempt++) {
		source_length = sizeof(source);
		count = recvfrom(descriptor, packet, sizeof(packet), MSG_DONTWAIT,
		    (struct sockaddr *)&source, &source_length);
		if (count >= 28 && (packet[0] >> 4) == 4 &&
		    (packet[0] & 0x0fU) >= 5U && packet[9] == IPPROTO_ICMP &&
		    (size_t)count >= (size_t)(packet[0] & 0x0fU) * 4U + 8U &&
		    packet[(packet[0] & 0x0fU) * 4U] == 0) {
			char address[16];
			size_t header_length = (size_t)(packet[0] & 0x0fU) * 4U;
			if (inet_ntop(AF_INET, &source.sin_addr, address,
			    sizeof(address)) == NULL)
				strcpy(address, "?");
			printf("nettest: ICMP echo reply from %s (%ld bytes, ttl %u)\n",
			    address, (long)(count - (ssize_t)header_length), packet[8]);
			close(descriptor);
			if (udp_test(&peer) != 0)
				return 1;
			return http_test(&peer);
		}
		if (count < 0 && errno != EAGAIN) {
			printf("nettest: receive failed (%d)\n", errno);
			close(descriptor);
			return 1;
		}
		(void)nanosleep(&delay, NULL);
	}
	puts("nettest: ICMP echo timed out");
	close(descriptor);
	return 1;
}
