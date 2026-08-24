/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BSD socket
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

#define SOCKET_MAX	32U
#define SOCKET_RECEIVE_MESSAGES_MAX	8U
#define SOCKET_BUFFER_DEFAULT	(64U * 1024U)
#define SOCKET_BUFFER_MIN	2048U
#define SOCKET_BUFFER_MAX	(1024U * 1024U)

struct file;
struct filedesc;
struct cwdinfo;
struct ucred;
struct packet_buf;
struct socket;
struct thread;

enum socket_lifecycle {
	SOCKET_OPEN = 1,
	SOCKET_CLOSING,
	SOCKET_CLOSED
};

struct socket_ops {
	int (
		*bind)(
		struct socket *,
		const struct sockaddr *,
		socklen_t);
	int (
		*connect)(
		struct socket *,
		const struct sockaddr *,
		socklen_t,
		unsigned);
	int (
		*listen)(
		struct socket *,
		int);
	int (
		*accept)(
		struct socket *,
		struct socket **,
		struct sockaddr *,
		socklen_t *,
		unsigned);
	ssize_t (
		*sendto)(
		struct socket *,
		const void *,
		size_t,
		int,
		const struct sockaddr *,
		socklen_t);
	ssize_t (
		*recvfrom)(
		struct socket *,
		void *,
		size_t,
		int,
		struct sockaddr *,
		socklen_t *);
	int (
		*shutdown)(
		struct socket *,
		int);
	int (
		*getsockname)(
		struct socket *,
		struct sockaddr *,
		socklen_t *);
	int (
		*getpeername)(
		struct socket *,
		struct sockaddr *,
		socklen_t *);
	int (
		*setsockopt)(
		struct socket *,
		int,
		int,
		const void *,
		socklen_t);
	int (
		*getsockopt)(
		struct socket *,
		int,
		int,
		void *,
		socklen_t *);
	int (
		*ioctl)(
		struct socket *,
		unsigned long,
		uintptr_t);
	int (
		*poll)(
		struct socket *,
		short,
		short *);
	void (
		*buffer_changed)(
		struct socket *,
		int);
	void (
		*endpoint_close)(
		struct socket *);
	void (
		*close)(
		struct socket *);
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
	struct wait_queue receive_space_waitq;
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
	size_t receive_packet_limit;
	size_t receive_hiwat_bytes;
	size_t send_hiwat_bytes;
	uint64_t receive_timeout_ticks;
	uint64_t send_timeout_ticks;
	unsigned reuse_address;
	unsigned read_shutdown;
	unsigned write_shutdown;
};

#define SOCKET_IO_NONBLOCK	0x0001U
#define SOCKET_IO_NOSIGNAL	0x0002U

struct socket_file_ref {
	struct file *file;
	struct socket *socket;
};

struct unix_recv_transaction {
	struct socket *socket;
	void *packet;
	uint64_t token;
	size_t copied;
	unsigned datagram;
	unsigned file_count;
	unsigned data_truncated;
	unsigned control_truncated;
	unsigned active;
	struct file *files[ZEDBSD_MSG_FD_MAX];
};

struct socket_family_ops {
	int (
		*create)(
		int type,
		int protocol,
		struct socket **result);
};

void
socket_core_init(void);
int
socket_family_register(
	int family,
	const struct socket_family_ops *ops);
int
socket_create(
	int family,
	int type,
	int protocol,
	struct socket **result);
int
socket_setsockopt_common(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length);
int
socket_getsockopt_common(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length);
void
socket_init_object(
	struct socket *socket,
	int family,
	int type,
	int protocol,
	const struct socket_ops *ops);
void
socket_ref(
	struct socket *socket);
int
socket_tryref(
	struct socket *socket);
void
socket_close_endpoint(
	struct socket *socket);
void
socket_release(
	struct socket *socket);
int
socket_enqueue_packet(
	struct socket *socket,
	struct packet_buf *packet);
int
socket_enqueue_packet_wait(
	struct socket *socket,
	struct packet_buf *packet,
	int flags,
	uint64_t timeout_ticks);
int
socket_requeue_packet_front(
	struct socket *socket,
	struct packet_buf *packet);
int
socket_dequeue_packet(
	struct socket *socket,
	int flags,
	struct packet_buf **result);
int
socket_file_create(
	struct socket *socket,
	struct file **result);
int
socket_file_reserve(
	struct file **result);
int
socket_file_attach(
	struct file *file,
	struct socket *socket);
struct socket *
socket_from_file(
	struct file *file);
int
socket_file_ref_get(
	struct filedesc *fd,
	int descriptor,
	struct socket_file_ref *reference);
void
socket_file_ref_put(
	struct socket_file_ref *reference);
unsigned
socket_file_effective_flags(
	const struct socket_file_ref *reference,
	int message_flags);
int
socket_take_error(
	struct socket *socket);
void
socket_set_error(
	struct socket *socket,
	int error);
void
socket_wake_receive(
	struct socket *socket);
void
socket_wake_send(
	struct socket *socket);
void
socket_wake_connect(
	struct socket *socket);
void
socket_wake_accept(
	struct socket *socket);
int
socket_poll_common(
	struct socket *socket,
	short events,
	short *revents);
unsigned
socket_count_current(void);

int
packet_socket_init(void);
int
unix_socket_init(void);
int
unix_socket_pair_create(
	int type,
	int protocol,
	struct socket **left_result,
	struct socket **right_result);
ssize_t
unix_socket_send_message(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length,
	struct file **files,
	unsigned count);
ssize_t
unix_socket_send_message_at(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length,
	struct file **files,
	unsigned count);
ssize_t
unix_socket_receive_message(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length,
	struct file **files,
	unsigned *file_count,
	unsigned *control_truncated);
ssize_t
unix_socket_receive_begin(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length,
	unsigned file_capacity,
	struct unix_recv_transaction *transaction);
void
unix_socket_receive_commit(
	struct unix_recv_transaction *transaction);
void
unix_socket_receive_abort(
	struct unix_recv_transaction *transaction);
int
unix_socket_bind_path(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	mode_t umask,
	const struct sockaddr *address,
	socklen_t length);
int
unix_socket_connect_path(
	struct socket *socket,
	struct cwdinfo *context,
	const struct ucred *cred,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags);
void
packet_socket_deliver(
	const struct packet_buf *packet,
	const uint8_t source[6],
	uint8_t packet_type);

#endif
