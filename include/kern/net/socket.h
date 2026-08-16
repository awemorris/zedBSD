/*
 * BSD socket
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_SOCKET_H
#define ZEDBSD_KERN_NET_SOCKET_H

#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <zedbsd/socket.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SOCKET_MAX 32U
#define SOCKET_RECEIVE_PACKETS_MAX 8U

struct file;
struct filedesc;
struct packet_buf;
struct socket;
struct thread;

enum socket_lifecycle {
	SOCKET_OPEN = 1,
	SOCKET_CLOSING,
	SOCKET_CLOSED
};

struct socket_ops {
	int (*bind)(struct socket *, const struct sockaddr *, socklen_t);
	int (*connect)(struct socket *, const struct sockaddr *, socklen_t,
	    unsigned);
	int (*listen)(struct socket *, int);
	int (*accept)(struct socket *, struct socket **, struct sockaddr *,
	    socklen_t *, unsigned);
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
	refcount_t refs;
	struct spinlock lock;
	struct wait_queue receive_waitq;
	struct wait_queue send_waitq;
	struct wait_queue connect_waitq;
	struct wait_queue accept_waitq;
	enum socket_lifecycle lifecycle;
	const struct socket_ops *ops;
	void *private_data;
	struct packet_buf *receive_head;
	struct packet_buf *receive_tail;
	unsigned receive_packets;
	size_t receive_bytes;
	size_t receive_limit;
	uint64_t receive_timeout_ticks;
	uint64_t send_timeout_ticks;
	unsigned reuse_address;
	unsigned read_shutdown;
	unsigned write_shutdown;
};

#define SOCKET_IO_NONBLOCK 0x0001U
#define SOCKET_IO_NOSIGNAL 0x0002U

struct socket_file_ref {
	struct file *file;
	struct socket *socket;
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
int socket_tryref(struct socket *socket);
void socket_release(struct socket *socket);
int socket_enqueue_packet(struct socket *socket, struct packet_buf *packet);
int socket_requeue_packet_front(struct socket *socket,
				struct packet_buf *packet);
int socket_dequeue_packet(struct socket *socket, int flags,
			  struct packet_buf **result);
int socket_file_create(struct socket *socket, struct file **result);
struct socket *socket_from_file(struct file *file);
int socket_file_ref_get(struct filedesc *, int, struct socket_file_ref *);
void socket_file_ref_put(struct socket_file_ref *);
unsigned socket_file_effective_flags(const struct socket_file_ref *, int);
int socket_take_error(struct socket *);
void socket_set_error(struct socket *, int);
void socket_wake_receive(struct socket *);
void socket_wake_send(struct socket *);
void socket_wake_connect(struct socket *);
void socket_wake_accept(struct socket *);
unsigned socket_count_current(void);

int packet_socket_init(void);
void packet_socket_deliver(const struct packet_buf *packet,
			   const uint8_t source[6], uint8_t packet_type);

#endif
