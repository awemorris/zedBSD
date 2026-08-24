/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static intptr_t
socket_call(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2,
	    uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return syscall_result(__syscall6(number, a0, a1, a2, a3,
	    a4, a5));
}

int socket(int domain, int type, int protocol)
{
	return (int)socket_call(ZEDBSD_SYS_socket, domain, type, protocol,
	    0, 0, 0);
}

int
sockatmark(int descriptor)
{
	int at_mark;
	socklen_t length = sizeof(at_mark);
	return getsockopt(descriptor, SOL_SOCKET, SO_ATMARK, &at_mark, &length) == 0 ?
		at_mark : -1;
}

int socketpair(int domain, int type, int protocol, int descriptors[2])
{
	return (int)socket_call(ZEDBSD_SYS_socketpair, domain, type, protocol,
	    (uintptr_t)descriptors, 0, 0);
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

int
accept4(int descriptor, struct sockaddr *address, socklen_t *length,
	int flags)
{
	return (int)socket_call(ZEDBSD_SYS_accept, descriptor,
	    (uintptr_t)address, (uintptr_t)length, flags, 0, 0);
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

ssize_t
sendmsg(int descriptor, const struct msghdr *message, int flags)
{
	struct sendmsg_args request;
	const struct cmsghdr *control = NULL;
	const int *descriptors = NULL;
	unsigned descriptor_count = 0;
	unsigned char *buffer;
	size_t total = 0, offset = 0, i;
	ssize_t result;
	if (message == NULL || (message->msg_iovlen != 0 &&
	    message->msg_iov == NULL)) { errno = EINVAL; return -1; }
	if (message->msg_controllen != 0) {
		if (message->msg_control == NULL ||
		    message->msg_controllen < sizeof(struct cmsghdr)) {
			errno = EINVAL; return -1;
		}
		control = message->msg_control;
		if (control->cmsg_level != SOL_SOCKET ||
		    control->cmsg_type != SCM_RIGHTS ||
		    control->cmsg_len < CMSG_LEN(sizeof(int)) ||
		    control->cmsg_len > message->msg_controllen ||
		    (control->cmsg_len - CMSG_ALIGN(sizeof(*control))) %
		    sizeof(int) != 0) {
			errno = EINVAL; return -1;
		}
		descriptor_count = (unsigned)((control->cmsg_len -
		    CMSG_ALIGN(sizeof(*control))) / sizeof(int));
		if (descriptor_count > ZEDBSD_MSG_FD_MAX) {
			errno = EMSGSIZE; return -1;
		}
		descriptors = (const int *)CMSG_DATA(control);
	}
	for (i = 0; i < message->msg_iovlen; i++) {
		if (message->msg_iov[i].iov_len > SIZE_MAX - total) {
			errno = EMSGSIZE; return -1;
		}
		total += message->msg_iov[i].iov_len;
	}
	buffer = total != 0 ? malloc(total) : NULL;
	if (total != 0 && buffer == NULL) { errno = ENOMEM; return -1; }
	for (i = 0; i < message->msg_iovlen; i++) {
		memcpy(buffer + offset, message->msg_iov[i].iov_base,
		    message->msg_iov[i].iov_len);
		offset += message->msg_iov[i].iov_len;
	}
	memset(&request, 0, sizeof(request));
	request.data = (uapi_ptr_t)(uintptr_t)(buffer != NULL ?
	    (void *)buffer : (void *)"");
	request.data_length = total;
	request.name = (uapi_ptr_t)(uintptr_t)message->msg_name;
	request.name_length = message->msg_name != NULL ?
	    message->msg_namelen : 0;
	request.flags = (uint32_t)flags;
	request.descriptors = (uapi_ptr_t)(uintptr_t)descriptors;
	request.descriptor_count = descriptor_count;
	result = (ssize_t)socket_call(ZEDBSD_SYS_sendmsg, descriptor,
	    (uintptr_t)&request, 0, 0, 0, 0);
	free(buffer);
	return result;
}

ssize_t
recvmsg(int descriptor, struct msghdr *message, int flags)
{
	struct recvmsg_args request;
	int descriptors[ZEDBSD_MSG_FD_MAX];
	unsigned descriptor_capacity = 0;
	unsigned char *buffer;
	size_t total = 0, offset = 0, i, copied;
	ssize_t result;
	if (message == NULL || (message->msg_iovlen != 0 &&
	    message->msg_iov == NULL)) { errno = EINVAL; return -1; }
	if (message->msg_control != NULL &&
	    message->msg_controllen >= CMSG_SPACE(sizeof(int))) {
		descriptor_capacity = (unsigned)((message->msg_controllen -
		    CMSG_ALIGN(sizeof(struct cmsghdr))) / sizeof(int));
		if (descriptor_capacity > ZEDBSD_MSG_FD_MAX)
			descriptor_capacity = ZEDBSD_MSG_FD_MAX;
	}
	for (i = 0; i < message->msg_iovlen; i++) {
		if (message->msg_iov[i].iov_len > SIZE_MAX - total) {
			errno = EMSGSIZE; return -1;
		}
		total += message->msg_iov[i].iov_len;
	}
	buffer = total != 0 ? malloc(total) : NULL;
	if (total != 0 && buffer == NULL) { errno = ENOMEM; return -1; }
	memset(&request, 0, sizeof(request));
	request.data = (uapi_ptr_t)(uintptr_t)(buffer != NULL ?
	    (void *)buffer : (void *)"");
	request.data_capacity = total;
	request.name = (uapi_ptr_t)(uintptr_t)message->msg_name;
	request.name_capacity = message->msg_name != NULL ?
	    message->msg_namelen : 0;
	request.flags = (uint32_t)flags;
	request.descriptors = (uapi_ptr_t)(uintptr_t)descriptors;
	request.descriptor_capacity = descriptor_capacity;
	result = (ssize_t)socket_call(ZEDBSD_SYS_recvmsg, descriptor,
	    (uintptr_t)&request, 0, 0, 0, 0);
	if (result >= 0) {
		message->msg_namelen = request.name_length;
		message->msg_flags = (int)request.output_flags;
		copied = (size_t)result < total ? (size_t)result : total;
		for (i = 0; i < message->msg_iovlen && copied != 0; i++) {
			size_t part = message->msg_iov[i].iov_len < copied ?
			    message->msg_iov[i].iov_len : copied;
			memcpy(message->msg_iov[i].iov_base, buffer + offset, part);
			offset += part;
			copied -= part;
		}
		if (request.descriptor_count != 0) {
			struct cmsghdr *control = message->msg_control;
			size_t bytes = request.descriptor_count * sizeof(int);
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
	return result;
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
