/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/libc/syscall.h"

#include <zedbsd/syscall.h>
#include <zedbsd/netinet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

extern intptr_t zedbsd_syscall_result(intptr_t);

static intptr_t
socket_call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2,
	    uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return zedbsd_syscall_result(zedbsd_syscall6(number, a0, a1, a2, a3,
	    a4, a5));
}

int socket(int domain, int type, int protocol)
{
	return (int)socket_call(ZEDBSD_SYS_socket, domain, type, protocol,
	    0, 0, 0);
}

int bind(int descriptor, const struct sockaddr *address, socklen_t length)
{
	return (int)socket_call(ZEDBSD_SYS_bind, descriptor,
	    (uintptr_t)address, length, 0, 0, 0);
}

int connect(int descriptor, const struct sockaddr *address, socklen_t length)
{
	return (int)socket_call(ZEDBSD_SYS_connect, descriptor,
	    (uintptr_t)address, length, 0, 0, 0);
}

int listen(int descriptor, int backlog)
{
	return (int)socket_call(ZEDBSD_SYS_listen, descriptor, backlog,
	    0, 0, 0, 0);
}

int accept(int descriptor, struct sockaddr *address, socklen_t *length)
{
	return (int)socket_call(ZEDBSD_SYS_accept, descriptor,
	    (uintptr_t)address, (uintptr_t)length, 0, 0, 0);
}

ssize_t sendto(int descriptor, const void *buffer, size_t length, int flags,
	       const struct sockaddr *address, socklen_t address_length)
{
	return (ssize_t)socket_call(ZEDBSD_SYS_sendto, descriptor,
	    (uintptr_t)buffer, length, flags, (uintptr_t)address,
	    address_length);
}

ssize_t send(int descriptor, const void *buffer, size_t length, int flags)
{
	return sendto(descriptor, buffer, length, flags, NULL, 0);
}

ssize_t recvfrom(int descriptor, void *buffer, size_t length, int flags,
		 struct sockaddr *address, socklen_t *address_length)
{
	return (ssize_t)socket_call(ZEDBSD_SYS_recvfrom, descriptor,
	    (uintptr_t)buffer, length, flags, (uintptr_t)address,
	    (uintptr_t)address_length);
}

ssize_t recv(int descriptor, void *buffer, size_t length, int flags)
{
	return recvfrom(descriptor, buffer, length, flags, NULL, NULL);
}

int shutdown(int descriptor, int how)
{
	return (int)socket_call(ZEDBSD_SYS_shutdown, descriptor, how,
	    0, 0, 0, 0);
}

int getsockname(int descriptor, struct sockaddr *address, socklen_t *length)
{
	return (int)socket_call(ZEDBSD_SYS_getsockname, descriptor,
	    (uintptr_t)address, (uintptr_t)length, 0, 0, 0);
}

int getpeername(int descriptor, struct sockaddr *address, socklen_t *length)
{
	return (int)socket_call(ZEDBSD_SYS_getpeername, descriptor,
	    (uintptr_t)address, (uintptr_t)length, 0, 0, 0);
}

int setsockopt(int descriptor, int level, int option, const void *value,
	       socklen_t length)
{
	return (int)socket_call(ZEDBSD_SYS_setsockopt, descriptor, level,
	    option, (uintptr_t)value, length, 0);
}

int getsockopt(int descriptor, int level, int option, void *value,
	       socklen_t *length)
{
	return (int)socket_call(ZEDBSD_SYS_getsockopt, descriptor, level,
	    option, (uintptr_t)value, (uintptr_t)length, 0);
}

uint16_t htons(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return value;
#else
	return (uint16_t)(value << 8) | (uint16_t)(value >> 8);
#endif
}

uint16_t ntohs(uint16_t value) { return htons(value); }

uint32_t htonl(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return value;
#else
	return (value << 24) | ((value << 8) & 0x00ff0000U) |
	    ((value >> 8) & 0x0000ff00U) | (value >> 24);
#endif
}

uint32_t ntohl(uint32_t value) { return htonl(value); }

int
inet_aton(const char *text, struct in_addr *address)
{
	uint32_t value = 0;
	unsigned part;
	int index;

	if (text == NULL || address == NULL)
		return 0;
	for (index = 0; index < 4; index++) {
		if (*text < '0' || *text > '9')
			return 0;
		part = 0;
		do {
			part = part * 10U + (unsigned)(*text++ - '0');
			if (part > 255U)
				return 0;
		} while (*text >= '0' && *text <= '9');
		value = value << 8 | part;
		if (index != 3) {
			if (*text++ != '.')
				return 0;
		} else if (*text != '\0') {
			return 0;
		}
	}
	address->s_addr = htonl(value);
	return 1;
}

uint32_t inet_addr(const char *text)
{
	struct in_addr address;
	return inet_aton(text, &address) ? address.s_addr : INADDR_BROADCAST;
}

int inet_pton(int family, const char *text, void *address)
{
	if (family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	return inet_aton(text, address);
}

static char *append_decimal(char *output, unsigned value)
{
	if (value >= 100U) *output++ = (char)('0' + value / 100U);
	if (value >= 10U) *output++ = (char)('0' + value / 10U % 10U);
	*output++ = (char)('0' + value % 10U);
	return output;
}

const char *
inet_ntop(int family, const void *address, char *text, socklen_t length)
{
	uint32_t value;
	char buffer[16], *output = buffer;
	unsigned index;

	if (family != AF_INET) {
		errno = EAFNOSUPPORT;
		return NULL;
	}
	if (address == NULL || text == NULL) {
		errno = EFAULT;
		return NULL;
	}
	value = ntohl(((const struct in_addr *)address)->s_addr);
	for (index = 0; index < 4U; index++) {
		output = append_decimal(output, (value >> (24U - index * 8U)) & 0xffU);
		if (index != 3U) *output++ = '.';
	}
	*output = '\0';
	if ((size_t)(output - buffer) + 1U > length) {
		errno = ENOSPC;
		return NULL;
	}
	memcpy(text, buffer, (size_t)(output - buffer) + 1U);
	return text;
}
