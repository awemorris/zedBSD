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

#include <zedbsd/syscall.h>
#include <zedbsd/netinet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <stdlib.h>

extern intptr_t syscall_result(intptr_t);

static intptr_t socket_call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static char *append_decimal(char *output, unsigned value);

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
	function_result = (int)socket_call(ZEDBSD_SYS_socket, domain, type, protocol, 0, 0,
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
	function_result = (int)socket_call(ZEDBSD_SYS_socketpair, domain, type, protocol,
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
	function_result = (int)socket_call(ZEDBSD_SYS_bind, descriptor, (uintptr_t)address,
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
	function_result = (int)socket_call(ZEDBSD_SYS_connect, descriptor,
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
	function_result = (int)socket_call(ZEDBSD_SYS_listen, descriptor, backlog, 0, 0, 0,
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
	function_result = (int)socket_call(ZEDBSD_SYS_accept, descriptor,
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
	function_result = (int)socket_call(ZEDBSD_SYS_accept, descriptor,
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
	function_result = (ssize_t)socket_call(ZEDBSD_SYS_sendto, descriptor,
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
	    ZEDBSD_SYS_recvfrom, descriptor, (uintptr_t)buffer, length, flags,
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
		if (descriptor_count > ZEDBSD_MSG_FD_MAX) {
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
	result = (ssize_t)socket_call(ZEDBSD_SYS_sendmsg, descriptor,
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
	int descriptors[ZEDBSD_MSG_FD_MAX];
	unsigned descriptor_capacity;
	unsigned char *buffer;
	size_t total, offset, i, copied;
	ssize_t result;

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

	/* Handles a failed CMSG SPACE operation. */
	if (message->msg_control != NULL &&
	    message->msg_controllen >= CMSG_SPACE(sizeof(int))) {
		descriptor_capacity =
		    (unsigned)((message->msg_controllen -
				CMSG_ALIGN(sizeof(struct cmsghdr))) /
			       sizeof(int));

		/* Handles the descriptor capacity condition. */
		if (descriptor_capacity > ZEDBSD_MSG_FD_MAX)
			descriptor_capacity = ZEDBSD_MSG_FD_MAX;
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
	result = (ssize_t)socket_call(ZEDBSD_SYS_recvmsg, descriptor,
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
	function_result = (int)socket_call(ZEDBSD_SYS_shutdown, descriptor, how, 0, 0, 0,
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
	function_result = (int)socket_call(ZEDBSD_SYS_getsockname, descriptor,
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
	function_result = (int)socket_call(ZEDBSD_SYS_getpeername, descriptor,
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
	function_result = (int)socket_call(ZEDBSD_SYS_setsockopt, descriptor, level,
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
	function_result = (int)socket_call(ZEDBSD_SYS_getsockopt, descriptor, level,
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

	output = buffer;

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
