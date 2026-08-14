/*
 * BSD socket
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_SOCKET_H
#define ZEDBSD_KERN_NET_SOCKET_H

#include <zedbsd/socket.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SOCKET_MAX 32U
#define SOCKET_RECEIVE_PACKETS_MAX 8U

struct file;
struct packet_buf;
struct socket;
struct thread;

struct socket_ops {
	int (*bind)(struct socket *, const struct sockaddr *, socklen_t);
	int (*connect)(struct socket *, const struct sockaddr *, socklen_t);
	int (*listen)(struct socket *, int);
	int (*accept)(struct socket *, struct socket **, struct sockaddr *, socklen_t *);
	ssize_t (*sendto)(struct socket *, const void *, size_t, int, const struct sockaddr *, socklen_t);
	ssize_t (*recvfrom)(struct socket *, void *, size_t, int, struct sockaddr *, socklen_t *);
	int (*shutdown)(struct socket *, int);
	int (*getsockname)(struct socket *, struct sockaddr *, socklen_t *);
	int (*getpeername)(struct socket *, struct sockaddr *, socklen_t *);
	int (*setsockopt)(struct socket *, int, int, const void *, socklen_t);
	int (*getsockopt)(struct socket *, int, int, void *, socklen_t *);
	int (*ioctl)(struct socket *, unsigned long, uintptr_t);
	void (*close)(struct socket *);
};

struct socket {
	int family;
	int type;
	int protocol;
	int state;
	int error;
	unsigned flags;
	unsigned refcount;
	const struct socket_ops *ops;
	void *private_data;
	struct packet_buf *receive_head;
	struct packet_buf *receive_tail;
	unsigned receive_packets;
	size_t receive_bytes;
	size_t receive_limit;
	uint64_t receive_timeout_ticks;
	struct thread *receive_waiter;
	struct thread *send_waiter;
	struct thread *connect_waiter;
	struct thread *accept_waiter;
};

struct socket_family_ops {
	int (*create)(int type, int protocol, struct socket **result);
};

void socket_core_init(void);
int socket_family_register(int family, const struct socket_family_ops *ops);
int socket_create(int family, int type, int protocol, struct socket **result);
int socket_setsockopt_common(struct socket *, int level, int option,
			     const void *value, socklen_t length);
int socket_getsockopt_common(struct socket *, int level, int option,
			     void *value, socklen_t *length);
void socket_init_object(struct socket *socket, int family, int type,
			int protocol, const struct socket_ops *ops);
void socket_ref(struct socket *socket);
void socket_release(struct socket *socket);
int socket_enqueue_packet(struct socket *socket, struct packet_buf *packet);
int socket_requeue_packet_front(struct socket *socket,
				struct packet_buf *packet);
int socket_dequeue_packet(struct socket *socket, int flags,
			  struct packet_buf **result);
int socket_file_create(struct socket *socket, struct file **result);
struct socket *socket_from_file(struct file *file);

int packet_socket_init(void);
void packet_socket_deliver(const struct packet_buf *packet,
			   const uint8_t source[6], uint8_t packet_type);

#endif
