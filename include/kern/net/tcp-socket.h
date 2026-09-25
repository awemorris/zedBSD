/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * TCP socket
 */

#ifndef KERN_KERN_NET_TCP_SOCKET_H
#define KERN_KERN_NET_TCP_SOCKET_H

#include "kern/net/inet-socket.h"

enum tcp_state {
	TCP_CLOSED,
	TCP_LISTEN,
	TCP_SYN_SENT,
	TCP_SYN_RECEIVED,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSE_WAIT,
	TCP_LAST_ACK,
	TCP_TIME_WAIT
};

/*
 * How many segments may be outstanding at once, and one of them.
 *
 * The window this opens is TCP_SEND_QUEUE_MAX segments; the peer's own
 * advertised window narrows it further whenever that is smaller.
 */
#ifndef CONFIG_TCP_SEND_QUEUE_MAX
#define CONFIG_TCP_SEND_QUEUE_MAX 8
#endif
#define TCP_SEND_QUEUE_MAX ((unsigned)CONFIG_TCP_SEND_QUEUE_MAX)

struct tcp_pending {
	struct packet_buf *packet;
	uint32_t sequence;
	uint32_t advance;
	uint16_t length;
	uint8_t flags;
};

struct tcp_socket {
	struct inet_socket inet;
	enum tcp_state state;
	uint32_t send_next;
	uint32_t send_unacknowledged;
	uint32_t receive_next;
	uint32_t connect_generation;
	uint32_t active_connect_generation;
	uint64_t connect_wait_deadline;
	uint16_t peer_window;
	/* The window last put in a segment; a read that opens it says so. */
	uint16_t advertised_window;
	/*
	 * Segments that have been sent and not yet acknowledged, oldest
	 * first.  Several may be in flight at once, bounded by the peer's
	 * advertised window and by the size of this ring; each keeps a copy
	 * of its payload so it can be sent again.  The deadline and the
	 * attempt count belong to the oldest entry, which is the one a
	 * timeout resends.
	 */
	struct tcp_pending send_queue[CONFIG_TCP_SEND_QUEUE_MAX];
	unsigned send_first;
	unsigned send_count;
	uint64_t retransmit_deadline;
	unsigned retransmit_count;
	/*
	 * TCP_NODELAY as the caller set it, and the idle-probe state for
	 * SO_KEEPALIVE: when the next probe is due and how many have gone
	 * unanswered.
	 */
	unsigned nodelay;
	uint64_t keepalive_deadline;
	unsigned keepalive_probes;
	unsigned listen_backlog;
	unsigned half_open_count;
	unsigned accept_count;
	struct tcp_socket *listener;
	struct tcp_socket *queue_next;
	struct tcp_socket *half_open_head;
	struct tcp_socket *accept_head;
	struct tcp_socket *accept_tail;
};

int
tcp_init(void);
int
tcp_socket_create(
	int protocol,
	struct socket **result);

#endif
