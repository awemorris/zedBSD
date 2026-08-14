/*
 * TCP socket
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_TCP_SOCKET_H
#define ZEDBSD_KERN_NET_TCP_SOCKET_H

#include "kern/net/inet-socket.h"

enum tcp_state {
	TCP_CLOSED,
	TCP_SYN_SENT,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
	TCP_TIME_WAIT
};

struct tcp_socket {
	struct inet_socket inet;
	enum tcp_state state;
	uint32_t send_next;
	uint32_t send_unacknowledged;
	uint32_t receive_next;
	uint16_t peer_window;
	struct packet_buf *retransmit;
	uint32_t retransmit_sequence;
	uint64_t retransmit_deadline;
	uint8_t retransmit_flags;
	unsigned retransmit_count;
};

int tcp_init(void);
int tcp_socket_create(int protocol, struct socket **result);

#endif
