/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library socket support.
 */

#include "syscall.h"

#include <uapi/syscall.h>
#include <uapi/netinet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

extern intptr_t syscall_result(intptr_t);

/*
 * One entry of the list getifaddrs() gives, with the storage its pointers
 * point into.
 *
 * Each entry is one block from malloc, so the list lives until
 * freeifaddrs() frees its entries one by one; the struct ifaddrs comes
 * first, so the entry's address is the node's.
 */
struct ifaddrs_node {
	struct ifaddrs entry;
	char name[IFNAMSIZ];
	struct sockaddr_storage address;
	struct sockaddr_storage netmask;
	struct sockaddr_storage broadcast;
	struct if_data data;
};


static intptr_t socket_call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static char *append_decimal(char *output, unsigned value);
static int recvmsg_put_control(struct msghdr *message, size_t room, size_t *used, int level, int type, const void *data, size_t length);
static int inet6_pton_text(const char *text, uint8_t *address);
static int inet6_ntop_text(const uint8_t *address, char *text, size_t size);
static int inet6_hex(char c);
static int inet6_dotted(const char *text, uint8_t *bytes);
static int inet6_group(const char **cursor, uint16_t *group);
static int inet6_zero_run(const uint16_t *groups, unsigned *start);
static char *inet6_put_group(char *output, uint16_t group);
static int ifaddrs_add_interface(int descriptor, const struct ifreq *interface, struct ifaddrs ***tail);
static struct ifaddrs_node *ifaddrs_node_add(const char *name, unsigned flags, struct ifaddrs ***tail);
static int ifaddrs_add_link(int descriptor, const char *name, unsigned flags, unsigned index, struct ifaddrs ***tail);
static int ifaddrs_add_inet(int descriptor, const char *name, unsigned flags, struct ifaddrs ***tail);
static int ifaddrs_add_inet6(int descriptor, const char *name, unsigned flags, unsigned index, struct ifaddrs ***tail);
static void ifaddrs_prefix_mask(unsigned prefix_length, struct sockaddr_in6 *mask);

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
	size_t control_used;
	ssize_t result;
	int hop_limit;
	int traffic_class;
	unsigned char tos;
	struct timeval stamp;
	int put;

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
	request.size = (uint32_t)sizeof(request);
	result = (ssize_t)socket_call(KERN_SYS_recvmsg, descriptor,
				      (uintptr_t)&request, 0, 0, 0, 0);

	/* Checks the operation result. */
	if (result >= 0) {
		/* Process each element required by the operation. */
		message->msg_namelen = request.name_length;
		message->msg_flags = (int)(request.output_flags & ~RECVMSG_INFORMATION);
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
		 * What the datagram came with, each as its control message after
		 * the ones before it, where there is room (none: MSG_CTRUNC): the
		 * time it was queued (SO_TIMESTAMP), its Type of Service byte
		 * (IP_RECVTOS) or traffic class (IPV6_RECVTCLASS), and its hop
		 * limit (IPV6_RECVHOPLIMIT).  A datagram carries no descriptors,
		 * so these never share the buffer with SCM_RIGHTS.
		 */
		control_used = message->msg_controllen;

		/* The time it was queued, as a struct timeval (SCM_TIMESTAMP). */
		if ((request.output_flags & RECVMSG_TIMESTAMP) != 0U) {
			stamp.tv_sec = (time_t)request.timestamp_seconds;
			stamp.tv_usec = (suseconds_t)(request.timestamp_nanoseconds / 1000);
			put = recvmsg_put_control(message, control_room, &control_used, SOL_SOCKET, SCM_TIMESTAMP, &stamp,
			    sizeof(stamp));
			if (put != 0)
				message->msg_flags |= MSG_CTRUNC;
		}

		/* An IPv4 datagram's Type of Service byte, as one unsigned char (IP_TOS). */
		if ((request.output_flags & RECVMSG_TOS) != 0U) {
			tos = (unsigned char)request.traffic_class;
			put = recvmsg_put_control(message, control_room, &control_used, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
			if (put != 0)
				message->msg_flags |= MSG_CTRUNC;
		}

		/* An IPv6 datagram's traffic class, as an int (IPV6_TCLASS). */
		if ((request.output_flags & RECVMSG_TCLASS) != 0U) {
			traffic_class = (int)request.traffic_class;
			put = recvmsg_put_control(message, control_room, &control_used, IPPROTO_IPV6, IPV6_TCLASS,
			    &traffic_class, sizeof(traffic_class));
			if (put != 0)
				message->msg_flags |= MSG_CTRUNC;
		}

		/* An IPv6 datagram's hop limit, as an int (IPV6_HOPLIMIT). */
		if ((request.output_flags & RECVMSG_HOP_LIMIT) != 0U) {
			hop_limit = (int)request.hop_limit;
			put = recvmsg_put_control(message, control_room, &control_used, IPPROTO_IPV6, IPV6_HOPLIMIT,
			    &hop_limit, sizeof(hop_limit));
			if (put != 0)
				message->msg_flags |= MSG_CTRUNC;
		}

		/* The room the control messages took. */
		message->msg_controllen = control_used;
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

/*
 * Puts one control message after the ones a received message already has.
 *
 * room is the size of the caller's control buffer and *used the bytes the
 * messages before this one take; on success *used grows by this one's
 * aligned size (or to the end of the buffer, when its padding does not
 * fit).  Reports -1, changing nothing, when even the message without its
 * padding does not fit, and the caller then marks MSG_CTRUNC.
 */
static int
recvmsg_put_control(
	struct msghdr *message,
	size_t room,
	size_t *used,
	int level,
	int type,
	const void *data,
	size_t length)
{
	struct cmsghdr *control;
	unsigned char *start;
	size_t needed;
	size_t aligned;

	/* The message has to fit, its padding need not. */
	needed = CMSG_LEN(length);
	if (*used > room || room - *used < needed)
		return -1;

	/* Fills the header and the data where the last message ended. */
	start = (unsigned char *)message->msg_control + *used;
	control = (struct cmsghdr *)start;
	control->cmsg_len = needed;
	control->cmsg_level = level;
	control->cmsg_type = type;
	memcpy(CMSG_DATA(control), data, length);

	/* Counts it, with the padding to the next one where that fits. */
	aligned = CMSG_SPACE(length);
	if (aligned > room - *used)
		aligned = room - *used;
	*used += aligned;

	/* Succeeded: the message is in the buffer. */
	return 0;
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

/*
 * Lists the interfaces with their indexes (POSIX if_nameindex).
 *
 * The list is one block from malloc: the entries, the entry that ends it
 * (index 0, name NULL), then the names.  if_freenameindex frees it.
 * Reports NULL with errno (ENOBUFS when there is no memory) on a failure.
 */
struct if_nameindex *
if_nameindex(
	void)
{
	struct ifconf configuration;
	struct ifreq *requests;
	struct if_nameindex *list;
	char *names;
	size_t count;
	size_t index;
	int descriptor;
	int status;

	/* The network's ioctl answers it. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return NULL;

	/* Asks how much room the list of the interfaces takes. */
	memset(&configuration, 0, sizeof(configuration));
	status = ioctl(descriptor, SIOCGIFCONF, &configuration);
	if (status != 0) {
		close(descriptor);
		return NULL;
	}

	/* Room for the requests, at least one so that malloc gives a block. */
	count = configuration.ifc_len / sizeof(struct ifreq);
	requests = malloc((count + 1U) * sizeof(struct ifreq));
	if (requests == NULL) {
		close(descriptor);
		errno = ENOBUFS;
		return NULL;
	}

	/* Reads the interfaces; one added since the question is left out. */
	configuration.ifc_len = (uint32_t)(count * sizeof(struct ifreq));
	configuration.ifc_buf = (uint64_t)(uintptr_t)requests;
	status = 0;
	if (count != 0U)
		status = ioctl(descriptor, SIOCGIFCONF, &configuration);
	close(descriptor);
	if (status != 0) {
		free(requests);
		return NULL;
	}

	/* An interface removed since the question shortens the list. */
	count = configuration.ifc_len / sizeof(struct ifreq);

	/* One block for the entries, the end and the names. */
	list = malloc((count + 1U) * sizeof(struct if_nameindex) + count * IF_NAMESIZE);
	if (list == NULL) {
		free(requests);
		errno = ENOBUFS;
		return NULL;
	}

	/* Fills an entry for each interface, its name copied behind the entries. */
	names = (char *)(list + count + 1U);
	for (index = 0U; index < count; index++) {
		memcpy(names + index * IF_NAMESIZE, requests[index].ifr_name, IF_NAMESIZE);
		names[index * IF_NAMESIZE + IF_NAMESIZE - 1U] = '\0';
		list[index].if_index = (unsigned)requests[index].ifr_ifindex;
		list[index].if_name = names + index * IF_NAMESIZE;
	}

	/* The entry that ends the list. */
	list[count].if_index = 0U;
	list[count].if_name = NULL;
	free(requests);

	/* Succeeded: the list, which the caller frees with if_freenameindex. */
	return list;
}

/* Frees a list if_nameindex returned. */
void
if_freenameindex(
	struct if_nameindex *list)
{
	/* The entries and the names are one block. */
	free(list);
}

/*
 * Lists the addresses of the network interfaces (getifaddrs(3) of the BSDs).
 *
 * Each interface the system lists gives an AF_PACKET entry for its link,
 * an AF_INET entry when it has an IPv4 address, and an AF_INET6 entry for
 * each IPv6 address (<ifaddrs.h> says what each holds).  An interface that
 * goes away while it is read is left out.  Reports 0 with the list in
 * *list, which the caller frees with freeifaddrs(), or -1 with errno.
 */
int
getifaddrs(
	struct ifaddrs **list)
{
	struct ifconf configuration;
	struct ifreq *interfaces;
	struct ifaddrs *head;
	struct ifaddrs **tail;
	size_t count;
	size_t index;
	int descriptor;
	int status;
	int saved;

	/* A place for the list has to be given. */
	if (list == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* The network's ioctls answer it. */
	descriptor = socket(AF_INET, SOCK_DGRAM, 0);
	if (descriptor < 0)
		return -1;

	/* Asks how much room the list of the interfaces takes. */
	memset(&configuration, 0, sizeof(configuration));
	status = ioctl(descriptor, SIOCGIFCONF, &configuration);
	if (status != 0) {
		saved = errno;
		close(descriptor);
		errno = saved;
		return -1;
	}

	/* Room for the interfaces, at least one so that malloc gives a block. */
	count = configuration.ifc_len / sizeof(struct ifreq);
	interfaces = malloc((count + 1U) * sizeof(struct ifreq));
	if (interfaces == NULL) {
		close(descriptor);
		errno = ENOMEM;
		return -1;
	}

	/* Reads the interfaces; one added since the question is left out. */
	configuration.ifc_len = (uint32_t)(count * sizeof(struct ifreq));
	configuration.ifc_buf = (uint64_t)(uintptr_t)interfaces;
	status = 0;
	if (count != 0U)
		status = ioctl(descriptor, SIOCGIFCONF, &configuration);
	if (status != 0) {
		saved = errno;
		free(interfaces);
		close(descriptor);
		errno = saved;
		return -1;
	}

	/* An interface removed since the question shortens the list. */
	count = configuration.ifc_len / sizeof(struct ifreq);

	/* Adds the entries of each interface, in the order the system lists them. */
	head = NULL;
	tail = &head;
	status = 0;
	for (index = 0U; index < count; index++) {
		status = ifaddrs_add_interface(descriptor, &interfaces[index], &tail);
		if (status != 0)
			break;
	}

	/* The ioctls and the interface names are no longer needed. */
	saved = errno;
	free(interfaces);
	close(descriptor);

	/* A failed entry frees the ones before it. */
	if (status != 0) {
		freeifaddrs(head);
		errno = saved;
		return -1;
	}

	/* Succeeded: the list, which the caller frees with freeifaddrs. */
	*list = head;
	return 0;
}

/*
 * Frees a list getifaddrs() gave, all of its entries.
 */
void
freeifaddrs(
	struct ifaddrs *list)
{
	struct ifaddrs *next;

	/* Each entry is its own block, its addresses inside it. */
	while (list != NULL) {
		next = list->ifa_next;
		free(list);
		list = next;
	}
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

/*
 * Adds the entries of one interface: its link, its IPv4 address and its
 * IPv6 addresses.  An interface that has gone away adds none.  Reports 0,
 * or -1 with errno ENOMEM when an entry could not be made.
 */
static int
ifaddrs_add_interface(
	int descriptor,
	const struct ifreq *interface,
	struct ifaddrs ***tail)
{
	struct ifreq request;
	char name[IFNAMSIZ];
	unsigned flags;
	unsigned index;
	int status;

	/* The interface's name, terminated. */
	memcpy(name, interface->ifr_name, IFNAMSIZ);
	name[IFNAMSIZ - 1] = '\0';
	index = (unsigned)interface->ifr_ifindex;

	/* Its flags; one that has gone away since it was listed is left out. */
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFFLAGS, &request);
	if (status != 0)
		return 0;
	flags = (unsigned)request.ifr_flags;

	/* The link. */
	status = ifaddrs_add_link(descriptor, name, flags, index, tail);
	if (status != 0)
		return -1;

	/* The IPv4 address. */
	status = ifaddrs_add_inet(descriptor, name, flags, tail);
	if (status != 0)
		return -1;

	/* The IPv6 addresses. */
	status = ifaddrs_add_inet6(descriptor, name, flags, index, tail);
	if (status != 0)
		return -1;

	/* Succeeded: the interface's entries are in the list. */
	return 0;
}

/*
 * Adds an empty entry for an interface at the end of the list, with its
 * name and flags.  Reports the entry, or NULL with errno ENOMEM.
 */
static struct ifaddrs_node *
ifaddrs_node_add(
	const char *name,
	unsigned flags,
	struct ifaddrs ***tail)
{
	struct ifaddrs_node *node;

	/* One block for the entry and what it points to. */
	node = calloc(1U, sizeof(*node));
	if (node == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	/* Its name and flags. */
	memcpy(node->name, name, IFNAMSIZ);
	node->entry.ifa_name = node->name;
	node->entry.ifa_flags = flags;

	/* Links it after the last entry. */
	**tail = &node->entry;
	*tail = &node->entry.ifa_next;

	/* Succeeded: the entry, for the caller to fill. */
	return node;
}

/*
 * Adds the entry of an interface's link: an AF_PACKET address with its
 * index and hardware address, and its statistics as ifa_data.  Reports 0,
 * or -1 with errno ENOMEM.
 */
static int
ifaddrs_add_link(
	int descriptor,
	const char *name,
	unsigned flags,
	unsigned index,
	struct ifaddrs ***tail)
{
	struct ifaddrs_node *node;
	struct sockaddr_l2 link;
	struct ifreq request;
	int status;

	/* The entry. */
	node = ifaddrs_node_add(name, flags, tail);
	if (node == NULL)
		return -1;

	/* The link's address: its index, and an Ethernet interface's hardware address. */
	memset(&link, 0, sizeof(link));
	link.sl2_family = AF_PACKET;
	link.sl2_ifindex = index;
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFHWADDR, &request);
	if (status == 0 && (flags & IFF_LOOPBACK) == 0U) {
		link.sl2_hatype = L2_HARDWARE_ETHER;
		link.sl2_halen = 6U;
		memcpy(link.sl2_addr, request.ifr_hwaddr, 6U);
	}

	memcpy(&node->address, &link, sizeof(link));
	node->entry.ifa_addr = (struct sockaddr *)&node->address;

	/* The interface's counters, as the BSDs give them. */
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFSTATS, &request);
	if (status == 0) {
		node->data = request.ifr_data;
		node->entry.ifa_data = &node->data;
	}

	/* Succeeded: the link's entry is in the list. */
	return 0;
}

/*
 * Adds the entry of an interface's IPv4 address, its netmask and, with
 * IFF_BROADCAST, its broadcast address, when it has an address.  Reports
 * 0, or -1 with errno ENOMEM.
 */
static int
ifaddrs_add_inet(
	int descriptor,
	const char *name,
	unsigned flags,
	struct ifaddrs ***tail)
{
	struct ifaddrs_node *node;
	struct sockaddr_in address;
	struct ifreq request;
	int status;

	/* The address; an interface without one has no entry. */
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFADDR, &request);
	if (status != 0)
		return 0;
	memcpy(&address, &request.ifr_addr, sizeof(address));
	if (address.sin_addr.s_addr == 0U)
		return 0;

	/* The entry, with the address. */
	node = ifaddrs_node_add(name, flags, tail);
	if (node == NULL)
		return -1;
	address.sin_family = AF_INET;
	memcpy(&node->address, &address, sizeof(address));
	node->entry.ifa_addr = (struct sockaddr *)&node->address;

	/* The netmask. */
	memset(&request, 0, sizeof(request));
	memcpy(request.ifr_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFNETMASK, &request);
	if (status == 0) {
		memcpy(&address, &request.ifr_addr, sizeof(address));
		address.sin_family = AF_INET;
		memcpy(&node->netmask, &address, sizeof(address));
		node->entry.ifa_netmask = (struct sockaddr *)&node->netmask;
	}

	/* The broadcast address, of an interface that broadcasts. */
	if ((flags & IFF_BROADCAST) != 0U) {
		memset(&request, 0, sizeof(request));
		memcpy(request.ifr_name, name, IFNAMSIZ);
		status = ioctl(descriptor, SIOCGIFBRDADDR, &request);
		if (status == 0) {
			memcpy(&address, &request.ifr_addr, sizeof(address));
			address.sin_family = AF_INET;
			memcpy(&node->broadcast, &address, sizeof(address));
			node->entry.ifa_broadaddr = (struct sockaddr *)&node->broadcast;
		}
	}

	/* Succeeded: the address's entry is in the list. */
	return 0;
}

/*
 * Adds an entry for each IPv6 address of an interface, with the netmask of
 * its prefix and, for a link-local one, the interface as its scope.  An
 * interface the system has no IPv6 for has none.  Reports 0, or -1 with
 * errno ENOMEM.
 */
static int
ifaddrs_add_inet6(
	int descriptor,
	const char *name,
	unsigned flags,
	unsigned index,
	struct ifaddrs ***tail)
{
	struct in6_ifaddrs addresses;
	struct ifaddrs_node *node;
	struct sockaddr_in6 address;
	struct sockaddr_in6 netmask;
	uint32_t entry;
	int status;

	/* The interface's IPv6 addresses. */
	memset(&addresses, 0, sizeof(addresses));
	memcpy(addresses.ifa_name, name, IFNAMSIZ);
	status = ioctl(descriptor, SIOCGIFADDRS_IN6, &addresses);
	if (status != 0)
		return 0;

	/* An entry for each. */
	for (entry = 0U; entry < addresses.ifa_count && entry < IN6_IFADDRS_MAX; entry++) {
		node = ifaddrs_node_add(name, flags, tail);
		if (node == NULL)
			return -1;

		/* The address, with the interface as the scope of a link-local one. */
		memset(&address, 0, sizeof(address));
		address.sin6_family = AF_INET6;
		address.sin6_addr = addresses.ifa_list[entry].ife_addr;
		if (IN6_IS_ADDR_LINKLOCAL(&address.sin6_addr))
			address.sin6_scope_id = index;
		memcpy(&node->address, &address, sizeof(address));
		node->entry.ifa_addr = (struct sockaddr *)&node->address;

		/* The netmask of its prefix. */
		ifaddrs_prefix_mask(addresses.ifa_list[entry].ife_prefixlen, &netmask);
		memcpy(&node->netmask, &netmask, sizeof(netmask));
		node->entry.ifa_netmask = (struct sockaddr *)&node->netmask;
	}

	/* Succeeded: the addresses' entries are in the list. */
	return 0;
}

/*
 * Writes the IPv6 netmask of a prefix length: that many one bits from the
 * top, zero bits after them (a length over 128 is taken as 128).
 */
static void
ifaddrs_prefix_mask(
	unsigned prefix_length,
	struct sockaddr_in6 *mask)
{
	unsigned byte;
	unsigned bits;

	memset(mask, 0, sizeof(*mask));
	mask->sin6_family = AF_INET6;

	/* Fills each byte with the bits of the prefix that fall in it. */
	for (byte = 0U; byte < 16U; byte++) {
		bits = 0U;
		if (prefix_length > byte * 8U)
			bits = prefix_length - byte * 8U;
		if (bits >= 8U) {
			mask->sin6_addr.s6_addr[byte] = 0xffU;
		} else {
			mask->sin6_addr.s6_addr[byte] = (uint8_t)(0xff00U >> bits);
		}
	}
}
