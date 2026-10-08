/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library socket support.
 */

#include "userland/base/libc/syscall.h"

#include <uapi/syscall.h>
#include <uapi/netinet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

extern intptr_t syscall_result(intptr_t);

static intptr_t socket_call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static char *append_decimal(char *output, unsigned value);
static int inet6_pton_text(const char *text, uint8_t *address);
static int inet6_ntop_text(const uint8_t *address, char *text, size_t size);
static int inet6_hex(char c);
static int inet6_dotted(const char *text, uint8_t *bytes);
static int inet6_group(const char **cursor, uint16_t *group);
static int inet6_zero_run(const uint16_t *groups, unsigned *start);
static char *inet6_put_group(char *output, uint16_t group);

/*
 * Implements the socket operation.
 */
int
socket(
	int domain,
	int type,
	int protocol)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_socket, domain, type, protocol, 0, 0,
				0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sockatmark operation.
 */
int
sockatmark(
	int descriptor)
{
	int function_result;
	int at_mark;
	socklen_t length;

	length = sizeof(at_mark);

	/* Computes the function result. */
	function_result = getsockopt(descriptor, SOL_SOCKET, SO_ATMARK, &at_mark,
			  &length) == 0
		   ? at_mark
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the socketpair operation.
 */
int
socketpair(
	int domain,
	int type,
	int protocol,
	int descriptors[2])
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_socketpair, domain, type, protocol,
				(uintptr_t)descriptors, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the bind operation.
 */
int
bind(
	int descriptor,
	const struct sockaddr *address,
	socklen_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_bind, descriptor, (uintptr_t)address,
				length, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the connect operation.
 */
int
connect(
	int descriptor,
	const struct sockaddr *address,
	socklen_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_connect, descriptor,
				(uintptr_t)address, length, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the listen operation.
 */
int
listen(
	int descriptor,
	int backlog)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_listen, descriptor, backlog, 0, 0, 0,
				0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the accept operation.
 */
int
accept(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_accept, descriptor,
				(uintptr_t)address, (uintptr_t)length, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the accept4 operation.
 */
int
accept4(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length,
	int flags)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_accept, descriptor,
				(uintptr_t)address, (uintptr_t)length, flags, 0,
				0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sendto operation.
 */
ssize_t
sendto(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)socket_call(KERN_SYS_sendto, descriptor,
				    (uintptr_t)buffer, length, flags,
				    (uintptr_t)address, address_length);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the send operation.
 */
ssize_t
send(
	int descriptor,
	const void *buffer,
	size_t length,
	int flags)
{
	ssize_t function_result;

	/* Obtains the sendto result. */
	function_result = sendto(descriptor, buffer, length, flags, NULL, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the recvfrom operation.
 */
ssize_t
recvfrom(
	int descriptor,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	ssize_t function_result;

	/* Computes the function result. */
	function_result = (ssize_t)socket_call(
	    KERN_SYS_recvfrom, descriptor, (uintptr_t)buffer, length, flags,
	    (uintptr_t)address, (uintptr_t)address_length);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the recv operation.
 */
ssize_t
recv(
	int descriptor,
	void *buffer,
	size_t length,
	int flags)
{
	ssize_t function_result;

	/* Obtains the recvfrom result. */
	function_result = recvfrom(descriptor, buffer, length, flags, NULL, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sendmsg operation.
 */
ssize_t
sendmsg(
	int descriptor,
	const struct msghdr *message,
	int flags)
{
	struct sendmsg_args request;
	const struct cmsghdr *control;
	const int *descriptors;
	unsigned descriptor_count;
	unsigned char *buffer;
	size_t total, offset, i;
	ssize_t result;

	control = NULL;
	descriptors = NULL;
	descriptor_count = 0;
	total = 0;
	offset = 0;

	/* Handles the message availability. */
	if (message == NULL ||
	    (message->msg_iovlen != 0 && message->msg_iov == NULL)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the message condition. */
	if (message->msg_controllen != 0) {
		/* Handles the msg control availability. */
		if (message->msg_control == NULL ||
		    message->msg_controllen < sizeof(struct cmsghdr)) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		control = message->msg_control;

		/* Handles a failed CMSG LEN operation. */
		if (control->cmsg_level != SOL_SOCKET ||
		    control->cmsg_type != SCM_RIGHTS ||
		    control->cmsg_len < CMSG_LEN(sizeof(int)) ||
		    control->cmsg_len > message->msg_controllen ||
		    (control->cmsg_len - CMSG_ALIGN(sizeof(*control))) %
			    sizeof(int) !=
			0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		descriptor_count = (unsigned)((control->cmsg_len -
					       CMSG_ALIGN(sizeof(*control))) /
					      sizeof(int));

		/* Handles the descriptor count condition. */
		if (descriptor_count > KERN_MSG_FD_MAX) {
			errno = EMSGSIZE;

			/* Reports operation failure. */
			return -1;
		}
		descriptors = (const int *)CMSG_DATA(control);
	}

	/* Process each element required by the operation. */
	for (i = 0; i < message->msg_iovlen; i++) {
		/* Handles the message condition. */
		if (message->msg_iov[i].iov_len > SIZE_MAX - total) {
			errno = EMSGSIZE;

			/* Reports operation failure. */
			return -1;
		}
		total += message->msg_iov[i].iov_len;
	}
	buffer = total != 0 ? malloc(total) : NULL;

	/* Handles the buffer availability. */
	if (total != 0 && buffer == NULL) {
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < message->msg_iovlen; i++) {
		memcpy(buffer + offset, message->msg_iov[i].iov_base,
		       message->msg_iov[i].iov_len);
		offset += message->msg_iov[i].iov_len;
	}
	memset(&request, 0, sizeof(request));
	request.data = (uapi_ptr_t)(uintptr_t)(buffer != NULL ? (void *)buffer
							      : (void *)"");
	request.data_length = total;
	request.name = (uapi_ptr_t)(uintptr_t)message->msg_name;
	request.name_length =
	    message->msg_name != NULL ? message->msg_namelen : 0;
	request.flags = (uint32_t)flags;
	request.descriptors = (uapi_ptr_t)(uintptr_t)descriptors;
	request.descriptor_count = descriptor_count;
	result = (ssize_t)socket_call(KERN_SYS_sendmsg, descriptor,
				      (uintptr_t)&request, 0, 0, 0, 0);
	free(buffer);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the recvmsg operation.
 */
ssize_t
recvmsg(
	int descriptor,
	struct msghdr *message,
	int flags)
{
	size_t part;
	struct cmsghdr *control;
	size_t bytes;
	struct recvmsg_args request;
	int descriptors[KERN_MSG_FD_MAX];
	unsigned descriptor_capacity;
	unsigned char *buffer;
	size_t total, offset, i, copied;
	size_t control_room;
	size_t needed;
	ssize_t result;
	int hop_limit;

	descriptor_capacity = 0;
	total = 0;
	offset = 0;

	/* Handles the message availability. */
	if (message == NULL ||
	    (message->msg_iovlen != 0 && message->msg_iov == NULL)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* The room the caller gave for control messages. */
	control_room = 0;
	if (message->msg_control != NULL)
		control_room = message->msg_controllen;

	/* Handles a failed CMSG SPACE operation. */
	if (message->msg_control != NULL &&
	    message->msg_controllen >= CMSG_SPACE(sizeof(int))) {
		descriptor_capacity =
		    (unsigned)((message->msg_controllen -
				CMSG_ALIGN(sizeof(struct cmsghdr))) /
			       sizeof(int));

		/* Handles the descriptor capacity condition. */
		if (descriptor_capacity > KERN_MSG_FD_MAX)
			descriptor_capacity = KERN_MSG_FD_MAX;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < message->msg_iovlen; i++) {
		/* Handles the message condition. */
		if (message->msg_iov[i].iov_len > SIZE_MAX - total) {
			errno = EMSGSIZE;

			/* Reports operation failure. */
			return -1;
		}
		total += message->msg_iov[i].iov_len;
	}
	buffer = total != 0 ? malloc(total) : NULL;

	/* Handles the buffer availability. */
	if (total != 0 && buffer == NULL) {
		errno = ENOMEM;

		/* Reports operation failure. */
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.data = (uapi_ptr_t)(uintptr_t)(buffer != NULL ? (void *)buffer
							      : (void *)"");
	request.data_capacity = total;
	request.name = (uapi_ptr_t)(uintptr_t)message->msg_name;
	request.name_capacity =
	    message->msg_name != NULL ? message->msg_namelen : 0;
	request.flags = (uint32_t)flags;
	request.descriptors = (uapi_ptr_t)(uintptr_t)descriptors;
	request.descriptor_capacity = descriptor_capacity;
	result = (ssize_t)socket_call(KERN_SYS_recvmsg, descriptor,
				      (uintptr_t)&request, 0, 0, 0, 0);

	/* Checks the operation result. */
	if (result >= 0) {
		/* Process each element required by the operation. */
		message->msg_namelen = request.name_length;
		message->msg_flags = (int)request.output_flags;
		copied = (size_t)result < total ? (size_t)result : total;
		for (i = 0; i < message->msg_iovlen && copied != 0; i++) {
			part = message->msg_iov[i].iov_len < copied
		  ? message->msg_iov[i].iov_len
		  : copied;
			memcpy(message->msg_iov[i].iov_base, buffer + offset,
			       part);
			offset += part;
			copied -= part;
		}

		/* Handles the request condition. */
		if (request.descriptor_count != 0) {
			control = message->msg_control;
			bytes = request.descriptor_count * sizeof(int);
			control->cmsg_level = SOL_SOCKET;
			control->cmsg_type = SCM_RIGHTS;
			control->cmsg_len = CMSG_LEN(bytes);
			memcpy(CMSG_DATA(control), descriptors, bytes);
			message->msg_controllen = CMSG_SPACE(bytes);
		} else {
			message->msg_controllen = 0;
		}

		/*
		 * The hop limit the datagram came with (IPV6_RECVHOPLIMIT,
		 * ws177-p044) as an IPV6_HOPLIMIT message, where there is room
		 * (none: MSG_CTRUNC).
		 */
		if ((request.output_flags & RECVMSG_HOP_LIMIT) != 0U) {
			message->msg_flags = (int)(request.output_flags & ~RECVMSG_HOP_LIMIT);
			needed = CMSG_SPACE(sizeof(int));
			if (request.descriptor_count == 0U && control_room >= needed) {
				control = message->msg_control;
				hop_limit = (int)request.hop_limit;
				control->cmsg_level = IPPROTO_IPV6;
				control->cmsg_type = IPV6_HOPLIMIT;
				control->cmsg_len = CMSG_LEN(sizeof(int));
				memcpy(CMSG_DATA(control), &hop_limit, sizeof(int));
				message->msg_controllen = CMSG_SPACE(sizeof(int));
			} else {
				message->msg_flags |= MSG_CTRUNC;
			}
		}
	}
	free(buffer);

	/* Returns the computed result. */
	return result;
}

/*
 * Implements the shutdown operation.
 */
int
shutdown(
	int descriptor,
	int how)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_shutdown, descriptor, how, 0, 0, 0,
				0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getsockname operation.
 */
int
getsockname(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_getsockname, descriptor,
				(uintptr_t)address, (uintptr_t)length, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getpeername operation.
 */
int
getpeername(
	int descriptor,
	struct sockaddr *address,
	socklen_t *length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_getpeername, descriptor,
				(uintptr_t)address, (uintptr_t)length, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the setsockopt operation.
 */
int
setsockopt(
	int descriptor,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_setsockopt, descriptor, level,
				option, (uintptr_t)value, length, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getsockopt operation.
 */
int
getsockopt(
	int descriptor,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	int function_result;

	/* Computes the function result. */
	function_result = (int)socket_call(KERN_SYS_getsockopt, descriptor, level,
				option, (uintptr_t)value, (uintptr_t)length, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the htons operation.
 */
uint16_t
htons(
	uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

	/* Returns the computed result. */
	return value;
#else

	/* Returns the computed result. */
	return (uint16_t)(value << 8) | (uint16_t)(value >> 8);
#endif
}

/*
 * Implements the ntohs operation.
 */
uint16_t
ntohs(
	uint16_t value)
{
	uint16_t function_result;

	/* Obtains the htons result. */
	function_result = htons(value);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the htonl operation.
 */
uint32_t
htonl(
	uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

	/* Returns the computed result. */
	return value;
#else

	/* Returns the computed result. */
	return (value << 24) | ((value << 8) & 0x00ff0000U) |
	       ((value >> 8) & 0x0000ff00U) | (value >> 24);
#endif
}

/*
 * Implements the ntohl operation.
 */
uint32_t
ntohl(
	uint32_t value)
{
	uint32_t function_result;

	/* Obtains the htonl result. */
	function_result = htonl(value);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the inet aton operation.
 */
int
inet_aton(
	const char *text,
	struct in_addr *address)
{
	uint32_t value;
	unsigned part;
	int index;

	value = 0;

	/* Handles the text availability. */
	if (text == NULL || address == NULL)
		return 0;

	/* Process each remaining element. */
	for (index = 0; index < 4; index++) {
		/* Validates the current text. */
		if (*text < '0' || *text > '9')
			return 0;
		part = 0;
		do {
			part = part * 10U + (unsigned)(*text++ - '0');

			/* Handles the part condition. */
			if (part > 255U)
				return 0;
		} while (*text >= '0' && *text <= '9');
		value = value << 8 | part;

		/* Checks the current index. */
		if (index != 3) {
			/* Validates the current text. */
			if (*text++ != '.')
				return 0;
		} else if (*text != '\0') {
			/* Reports successful completion. */
			return 0;
		}
	}
	address->s_addr = htonl(value);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the inet addr operation.
 */
uint32_t
inet_addr(
	const char *text)
{
	uint32_t function_result;
	struct in_addr address;

	/* Computes the function result. */
	function_result = inet_aton(text, &address) ? address.s_addr : INADDR_BROADCAST;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the inet pton operation.
 */
int
inet_pton(
	int family,
	const char *text,
	void *address)
{
	int function_result;

	/* An IPv6 address (ws130-p004). */
	if (family == AF_INET6) {
		function_result = inet6_pton_text(text, address);
		return function_result;
	}

	/* Handles the family condition. */
	if (family != AF_INET) {
		errno = EAFNOSUPPORT;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the inet aton result. */
	function_result = inet_aton(text, address);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the inet ntop operation.
 */
const char *
inet_ntop(
	int family,
	const void *address,
	char *text,
	socklen_t length)
{
	uint32_t value;
	char buffer[16], *output;
	unsigned index;
	int error;

	output = buffer;

	/* An IPv6 address in RFC 5952's form (ws130-p004). */
	if (family == AF_INET6) {
		if (address == NULL || text == NULL) {
			errno = EFAULT;
			return NULL;
		}
		error = inet6_ntop_text(address, text, (size_t)length);
		if (error != 0) {
			errno = ENOSPC;
			return NULL;
		}
		return text;
	}

	/* Handles the family condition. */
	if (family != AF_INET) {
		errno = EAFNOSUPPORT;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles the address availability. */
	if (address == NULL || text == NULL) {
		errno = EFAULT;

		/* Reports that no result is available. */
		return NULL;
	}

	/* Process each remaining element. */
	value = ntohl(((const struct in_addr *)address)->s_addr);
	for (index = 0; index < 4U; index++) {
		output = append_decimal(output,
					(value >> (24U - index * 8U)) & 0xffU);

		/* Checks the current index. */
		if (index != 3U)
			*output++ = '.';
	}
	*output = '\0';
	/* Handles the output condition. */
	if ((size_t)(output - buffer) + 1U > length) {
		errno = ENOSPC;

		/* Reports that no result is available. */
		return NULL;
	}
	memcpy(text, buffer, (size_t)(output - buffer) + 1U);

	/* Returns the computed result. */
	return text;
}

/*
 * Implements the inet ntoa operation.
 *
 * POSIX allows the result to live in a static area, so the returned text is
 * only valid until this thread calls the function again.
 */
char *
inet_ntoa(struct in_addr address)
{
	static __thread char text[16];

	/* Handles a conversion failure by reporting an unusable address. */
	if (inet_ntop(AF_INET, &address, text, (socklen_t)sizeof(text)) == NULL) {
		text[0] = '\0';

		/* Returns the empty result. */
		return text;
	}

	/* Returns the computed result. */
	return text;
}

/* Supports the socket call operation. */
static intptr_t
socket_call(
	uint32_t number,
	uintptr_t a0,
	uintptr_t a1,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4,
	uintptr_t a5)
{
	intptr_t function_result;

	/* Obtains the syscall result result. */
	function_result = syscall_result(__syscall6(number, a0, a1, a2, a3, a4, a5));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the append decimal operation. */
static char *
append_decimal(
	char *output,
	unsigned value)
{
	/* Validates the current value. */
	if (value >= 100U)
		*output++ = (char)('0' + value / 100U);
	/* Validates the current value. */
	if (value >= 10U)
		*output++ = (char)('0' + value / 10U % 10U);
	*output++ = (char)('0' + value % 10U);
	/* Returns the computed result. */
	return output;
}

/*
 * Reports the index of the interface of a name (POSIX), 0 when there is
 * none (ws130-p004).
 */
unsigned
if_nametoindex(
	const char *name)
{
	struct ifreq request;
	size_t length;
	int descriptor;
	int status;

	/* A name that fits. */
	if (name == NULL)
		return 0;
	length = strlen(name);
	if (length >= sizeof(request.ifr_name))
		return 0;
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, length);

	/* The network's ioctl answers it. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return 0;
	status = ioctl(descriptor, SIOCGIFINDEX, &request);
	close(descriptor);
	if (status != 0)
		return 0;

	/* Succeeded: its index. */
	return (unsigned)request.ifr_ifindex;
}

/*
 * Writes the name of the interface of an index into name (IF_NAMESIZE
 * bytes) (POSIX), NULL with errno ENXIO when there is none (ws130-p004).
 */
char *
if_indextoname(
	unsigned index,
	char *name)
{
	struct ifreq request;
	int descriptor;
	int status;

	/* The network's ioctl answers it. */
	memset(&request, 0, sizeof(request));
	request.ifr_ifindex = (int)index;
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return NULL;
	status = ioctl(descriptor, SIOCGIFNAME, &request);
	close(descriptor);
	if (status != 0) {
		errno = ENXIO;
		return NULL;
	}

	/* Succeeded: its name. */
	request.ifr_name[IF_NAMESIZE - 1] = '\0';
	memcpy(name, request.ifr_name, IF_NAMESIZE);
	return name;
}

/* ------------------------------------------------------------------ *
 * IPv6 addresses as text (ws130-p004)
 *
 * inet_pton reads the written forms of RFC 4291 section 2.2; inet_ntop
 * writes the one form of RFC 5952 (lowercase, no leading zeros, the
 * longest run of two or more zero groups as "::", the first of equal
 * runs, and a v4-mapped address with its IPv4 part dotted).
 * ------------------------------------------------------------------ */

/* How many 16-bit groups an address has. */
#define INET6_GROUPS		8U

/*
 * Reads an IPv6 address in one of RFC 4291's written forms into its 16
 * bytes: 1 when it is one, 0 when it is not.
 */
static int
inet6_pton_text(
	const char *text,
	uint8_t *address)
{
	uint16_t groups[INET6_GROUPS];
	uint8_t dotted[4];
	const char *cursor;
	unsigned count;
	unsigned index;
	unsigned moved;
	int gap;
	int ok;

	/* A leading "::" opens the gap at once. */
	cursor = text;
	count = 0;
	gap = -1;
	if (cursor[0] == ':') {
		if (cursor[1] != ':')
			return 0;
		gap = 0;
		cursor += 2;
	}

	/* The groups, each after its colon, an embedded IPv4 address last. */
	while (*cursor != '\0') {
		/* No room for another group. */
		if (count == INET6_GROUPS)
			return 0;

		/* The dotted IPv4 address takes the last two groups. */
		ok = inet6_dotted(cursor, dotted);
		if (ok) {
			if (count > INET6_GROUPS - 2U)
				return 0;
			groups[count++] = (uint16_t)(dotted[0] << 8 | dotted[1]);
			groups[count++] = (uint16_t)(dotted[2] << 8 | dotted[3]);
			break;
		}

		/* A group of one to four hex digits. */
		ok = inet6_group(&cursor, &groups[count]);
		if (!ok)
			return 0;
		count++;

		/* The text may end after a group. */
		if (*cursor == '\0')
			break;
		if (*cursor != ':')
			return 0;
		cursor++;

		/* A second colon is the gap, once. */
		if (*cursor == ':') {
			if (gap >= 0)
				return 0;
			gap = (int)count;
			cursor++;
			continue;
		}

		/* A colon ends no address. */
		if (*cursor == '\0')
			return 0;
	}

	/* Without a gap the groups are all there; with one, it stands for one group at least. */
	if (gap < 0 && count != INET6_GROUPS)
		return 0;
	if (gap >= 0 && count == INET6_GROUPS)
		return 0;

	/* The groups after the gap move to the end, and the gap is zeros. */
	if (gap >= 0) {
		moved = count - (unsigned)gap;
		for (index = 0; index < moved; index++)
			groups[INET6_GROUPS - 1U - index] = groups[count - 1U - index];
		for (index = (unsigned)gap; index < INET6_GROUPS - moved; index++)
			groups[index] = 0;
	}

	/* Succeeded: the bytes in network order. */
	for (index = 0; index < INET6_GROUPS; index++) {
		address[2U * index] = (uint8_t)(groups[index] >> 8);
		address[2U * index + 1U] = (uint8_t)groups[index];
	}
	return 1;
}

/*
 * Writes an IPv6 address in RFC 5952's form: 0, or ENOSPC when the text
 * does not fit its size.
 */
static int
inet6_ntop_text(
	const uint8_t *address,
	char *text,
	size_t size)
{
	uint16_t groups[INET6_GROUPS];
	char buffer[48];
	char *output;
	unsigned index;
	unsigned start;
	int run;
	int mapped;
	int written;

	/* The groups. */
	for (index = 0; index < INET6_GROUPS; index++)
		groups[index] = (uint16_t)(address[2U * index] << 8 | address[2U * index + 1U]);

	/* A v4-mapped address keeps its IPv4 part dotted (RFC 5952 section 5). */
	mapped = 1;
	for (index = 0; index < 5U; index++) {
		if (groups[index] != 0)
			mapped = 0;
	}
	if (mapped && groups[5] == 0xffffU) {
		written = snprintf(buffer, sizeof(buffer), "::ffff:%u.%u.%u.%u", address[12], address[13], address[14], address[15]);
		if (written < 0 || (size_t)written + 1U > size)
			return ENOSPC;
		memcpy(text, buffer, (size_t)written + 1U);
		return 0;
	}

	/* The groups, the longest run of zeros as "::". */
	run = inet6_zero_run(groups, &start);
	output = buffer;
	for (index = 0; index < INET6_GROUPS; index++) {
		/* The run: "::" once, in place of its groups. */
		if (run > 0 && index == start) {
			*output++ = ':';
			*output++ = ':';
			index += (unsigned)run - 1U;
			continue;
		}

		/* A colon between groups that are not after the run. */
		if (index != 0 && !(run > 0 && index == start + (unsigned)run))
			*output++ = ':';
		output = inet6_put_group(output, groups[index]);
	}
	*output = '\0';

	/* Succeeded when it fits. */
	if ((size_t)(output - buffer) + 1U > size)
		return ENOSPC;
	memcpy(text, buffer, (size_t)(output - buffer) + 1U);
	return 0;
}

/* Reports a hex digit's value, or -1 for another character. */
static int
inet6_hex(
	char c)
{
	/* The decimal digits. */
	if (c >= '0' && c <= '9')
		return c - '0';

	/* The letters, either case. */
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	/* Not a hex digit. */
	return -1;
}

/* Reads a dotted IPv4 address that ends the text; 1 with its 4 bytes, 0 when the text is not one. */
static int
inet6_dotted(
	const char *text,
	uint8_t *bytes)
{
	unsigned part;
	unsigned value;
	unsigned digits;

	/* Four decimal parts, dots between. */
	for (part = 0; part < 4U; part++) {
		value = 0;
		digits = 0;
		while (*text >= '0' && *text <= '9') {
			value = value * 10U + (unsigned)(*text - '0');
			digits++;
			text++;
			if (digits > 3U || value > 255U)
				return 0;
		}

		/* Each part has digits. */
		if (digits == 0)
			return 0;
		bytes[part] = (uint8_t)value;

		/* A dot after the first three, the end after the last. */
		if (part < 3U && *text != '.')
			return 0;
		if (part < 3U)
			text++;
	}

	/* Succeeded only at the end of the text. */
	if (*text != '\0')
		return 0;
	return 1;
}

/* Reads a group of one to four hex digits, moving the cursor past it; 1, or 0 when there is none. */
static int
inet6_group(
	const char **cursor,
	uint16_t *group)
{
	const char *text;
	unsigned value;
	unsigned digits;
	int digit;

	/* The digits. */
	text = *cursor;
	value = 0;
	digits = 0;
	for (;;) {
		digit = inet6_hex(*text);
		if (digit < 0)
			break;
		value = value << 4 | (unsigned)digit;
		digits++;
		text++;
		if (digits > 4U)
			return 0;
	}

	/* A group has a digit at least. */
	if (digits == 0)
		return 0;

	/* Succeeded. */
	*group = (uint16_t)value;
	*cursor = text;
	return 1;
}

/* Finds the longest run of two or more zero groups (the first of equal ones); its length, 0 for none, and its start. */
static int
inet6_zero_run(
	const uint16_t *groups,
	unsigned *start)
{
	unsigned index;
	unsigned length;
	unsigned best;
	unsigned best_start;

	/* Each run of zeros. */
	best = 0;
	best_start = 0;
	length = 0;
	for (index = 0; index < INET6_GROUPS; index++) {
		if (groups[index] != 0) {
			length = 0;
			continue;
		}
		length++;
		if (length > best) {
			best = length;
			best_start = index + 1U - length;
		}
	}

	/* One zero group stays written. */
	if (best < 2U)
		return 0;

	/* Succeeded: the run. */
	*start = best_start;
	return (int)best;
}

/* Writes a group in lowercase hex without leading zeros, and returns the end. */
static char *
inet6_put_group(
	char *output,
	uint16_t group)
{
	static const char digits[] = "0123456789abcdef";
	int shift;
	int started;

	/* The nibbles from the highest, the leading zeros left out. */
	started = 0;
	for (shift = 12; shift >= 0; shift -= 4) {
		if (!started && shift > 0 && ((group >> shift) & 0x0fU) == 0)
			continue;
		started = 1;
		*output++ = digits[(group >> shift) & 0x0fU];
	}

	/* Returns the end. */
	return output;
}
