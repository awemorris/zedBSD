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
 * The window this opens is TCP_SEND_QUEUE_MAX segments; the congestion
 * window and the peer's advertised window narrow it further whenever they
 * are smaller.  Each queued segment keeps a copy of its payload in a
 * packet of the shared pool, so the 64-bit machines, whose pool is large,
 * keep more than the largest window their receive side grows to (768
 * segments of 1460 bytes, 1.1 MB, against 1 MB), and the 32-bit boards a
 * few.  When a link carries a transfer each way at once, each direction's
 * acknowledgements wait behind the other's data, so the two share one
 * round trip and divide the link as their windows stand to each other: a
 * ring smaller than the window this end offers the peer would leave this
 * end's sending that share of the link.
 */
#ifndef CONFIG_TCP_SEND_QUEUE_MAX
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64) || \
    defined(HAL_ARCH_RISCV64)
#define CONFIG_TCP_SEND_QUEUE_MAX 768
#else
#define CONFIG_TCP_SEND_QUEUE_MAX 8
#endif
#endif
#define TCP_SEND_QUEUE_MAX ((unsigned)CONFIG_TCP_SEND_QUEUE_MAX)

/* The congestion controls a connection may use: RFC 5681's (NewReno, RFC 6582) and CUBIC (RFC 9438). */
#define TCP_CC_RENO	0U
#define TCP_CC_CUBIC	1U

/*
 * How many segments that came ahead of a gap a connection keeps until the
 * gap is filled (BUG-222).  On the 64-bit machines it is the whole receive
 * window (tcp.c's TCP_RECEIVE_PACKETS, 128 segments: the 64 KB window in
 * segments as small as 512 bytes): a segment dropped for
 * want of room is one the sender (without SACK) resends a round trip
 * after the one before it, and with the window queued in the USB adapter a
 * round trip is tens of milliseconds (T1-516: a 2 % loss fell to a tenth
 * of the speed).  The window counts only the receive queue's free slots,
 * so the queue and the segments kept ahead stay within about one window
 * of packets together.  The 32-bit boards keep a few (tcp.c).
 * CONFIG_TCP_REORDER_MAX sets another (a build that measures longer round
 * trips, with CONFIG_TCP_RECEIVE_PACKETS).
 */
#if defined(CONFIG_TCP_REORDER_MAX)
#define TCP_REORDER_MAX ((unsigned)CONFIG_TCP_REORDER_MAX)
#elif defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64) || \
    defined(HAL_ARCH_RISCV64)
#define TCP_REORDER_MAX 128U
#else
#define TCP_REORDER_MAX 16U
#endif

/*
 * One segment in the send ring: its copy, where it lies in the sequence
 * space, its TCP flags, whether it went out as a probe of a window that
 * could not take it (set when it is marked sent), and whether
 * it has never been handed to the device (fresh: a short last segment
 * that Nagle's algorithm holds takes the next write's bytes), and whether
 * it was made unsent again after it went (rewound: by a timeout's
 * go-back-N or a Packet Too Big; its next sending is a resend, counted so
 * and never timed).  The
 * payload is length bytes at payload_offset in the copy's storage: the
 * copy itself carries the segment's first transmission, which pushes the
 * headers in front of the payload, so its data pointer is not the
 * payload's afterwards; the payload's bytes never change.
 */
struct tcp_pending {
	struct packet_buf *packet;
	uint32_t sequence;
	uint32_t advance;
	uint16_t length;
	uint16_t payload_offset;
	uint8_t flags;
	uint8_t probe;
	uint8_t fresh;
	uint8_t rewound;
	/*
	 * With SACK (RFC 6675): the peer reported the whole payload held
	 * (sacked), and the segment was sent again within the recovery that
	 * now runs (resent), by a fast retransmit, a partial acknowledgement
	 * or a hole the SACK blocks showed.
	 */
	uint8_t sacked;
	uint8_t resent;
	/*
	 * The order of that resend among the connection's (resend_serial): a
	 * later resend the peer reports held while this one is not says this
	 * one was lost too.
	 */
	uint16_t resend_order;
};

/*
 * What one connection has done since its socket was created, which
 * net.tcpstats reports (ws220-p001) to follow a transfer's control over
 * time.  Each count only grows, and is moved and read under the socket
 * lock, so a reader sees the counts of one moment.  The meaning of each is
 * that of the field of the same name in struct tcp_stats_entry
 * (<uapi/sysctl.h>).
 */
struct tcp_counters {
	uint64_t segments_out;
	uint64_t segments_in;
	uint64_t data_segments_out;
	uint64_t data_bytes_out;
	uint64_t data_bytes_in;
	uint64_t retransmit_segments;
	uint64_t retransmit_bytes;
	uint64_t immediate_resends;
	uint64_t send_refused;
	uint64_t send_buffer_sleeps;
	uint64_t segment_send_failures;
	uint64_t acks_in;
	uint64_t duplicate_acks_in;
	uint64_t acks_outside;
	uint64_t zero_windows_in;
	uint64_t zero_windows_out;
	uint64_t window_updates_out;
	uint64_t old_segments_in;
	uint64_t out_of_order_in;
	uint64_t out_of_order_kept;
	uint64_t receive_refused;
	uint64_t fast_retransmits;
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
	/*
	 * The peer's window in bytes, scaled by its window scale (RFC 7323)
	 * when both ends offered the option in their SYNs: wscale_offered
	 * says this end's SYN carries it (an active open always, a passive
	 * one when the peer's SYN did), peer_wscale is the peer's shift, and
	 * wscale_on says both offered it.  This end's own shift, which its SYN
	 * offers, scales the windows it advertises once wscale_on is set.
	 */
	uint32_t peer_window;
	uint8_t peer_wscale;
	uint8_t wscale_offered;
	uint8_t wscale_on;
	uint8_t receive_wscale;
	/*
	 * Selective acknowledgements (RFC 2018): sack_offered says this end's
	 * SYN carries SACK-Permitted (an active open always, a passive one when
	 * the peer's SYN did), and sack_on that both ends offered it.
	 */
	uint8_t sack_offered;
	uint8_t sack_on;
	/* The order the next resend of a loss takes (tcp_pending.resend_order). */
	uint16_t resend_serial;
	/*
	 * The sequence and acknowledgement numbers of the segment peer_window
	 * was last taken from (RFC 9293's SND.WL1 and SND.WL2).  A segment
	 * older than that one, by these two numbers, does not set the window
	 * again, so an acknowledgement overtaken on the way cannot replace the
	 * window with one the peer has already replaced itself.
	 */
	uint32_t send_window_sequence;
	uint32_t send_window_acknowledgement;
	/*
	 * The window last put in a segment, in bytes (the field scaled back by
	 * this end's shift); a read that opens it says so.
	 */
	uint32_t advertised_window;
	/*
	 * The right edge of the window last put in a segment (receive_next
	 * then, and that window), once one went (receive_edge_set): a window
	 * that opens it by less than min(half the buffer, a segment) keeps it
	 * where it was, the receiver's silly window avoidance.
	 */
	uint32_t receive_edge;
	uint8_t receive_edge_set;
	/*
	 * The maximum segment size the peer's SYN offered (0: it offered none),
	 * and the largest payload this end puts in a segment: the smaller of
	 * that offer (536, or 1220 over IPv6, without one) and what the route's
	 * interface carries, set when the connection is established.
	 */
	uint32_t peer_mss;
	uint32_t send_mss;
	/*
	 * The maximum segment size this end's SYN offered (0 before one went):
	 * what the route's interface carries, which the receive window and its
	 * updates count in.
	 */
	uint32_t receive_mss;
	/*
	 * The receive buffer's automatic sizing (dynamic right-sizing).
	 * receive_auto says the buffer grows by itself as the transfer asks
	 * (cleared for good once SO_RCVBUF sets one), and receive_extra_packets
	 * is how many packets beyond the default its queue took from the
	 * system's budget, which the socket gives back when it closes.  A
	 * measurement runs while receive_tune_on is set: it began with
	 * receive_next at receive_tune_base, at receive_tune_start_us, and
	 * ends when receive_next passes receive_tune_mark, the right edge of
	 * the window when it began.  receive_rtt_us is the round trip as this
	 * end sees it receiving (0: none measured).  All moved under the
	 * socket lock.
	 */
	uint8_t receive_auto;
	uint8_t receive_tune_on;
	unsigned receive_extra_packets;
	uint32_t receive_tune_mark;
	uint32_t receive_tune_base;
	uint64_t receive_tune_start_us;
	uint32_t receive_rtt_us;
	/*
	 * The congestion control of RFC 5681 in bytes: the congestion window
	 * (RFC 6928's ten segments to start), the slow start threshold, and
	 * the bytes acknowledged towards the next segment of growth in
	 * congestion avoidance.  What is in flight stays within the smaller of
	 * the congestion window and the peer's window.
	 */
	uint32_t cwnd;
	uint32_t ssthresh;
	uint32_t cwnd_acked;
	/*
	 * Whether the congestion window is what limits the sending (RFC 7661),
	 * measured over rounds: a round ends when the acknowledgements reach
	 * cwnd_round_end, the end of what was sent when it began.
	 * cwnd_round_flight is the most sent and not acknowledged (the bytes
	 * handed to the device, not those queued behind them) within the
	 * round, cwnd_prev_flight that of the round before; cwnd_blocked says
	 * the window held back a queued segment within the round (a write it
	 * refuses has the flight within a segment of it already).  The window
	 * grows only while a round uses it.
	 */
	uint32_t cwnd_round_end;
	uint32_t cwnd_round_flight;
	uint32_t cwnd_prev_flight;
	unsigned cwnd_blocked;
	/* The congestion control that grows and cuts the window (TCP_CC_*). */
	unsigned cc;
	/*
	 * CUBIC's state (RFC 9438), in bytes and milliseconds: the window
	 * before the last loss (W_max) and the window then (cwnd_prior); the
	 * epoch of congestion avoidance, while cubic_epoch_started, with its
	 * start, the window function's origin and K, and the estimate of
	 * Reno's window (W_est); when the window began to go unused within
	 * the epoch (cubic_limited_since_ms, 0 while it is used), which
	 * moves the epoch on by that time; the remainders the integer growth
	 * of W_est and of the window carry; and cubic_after_timeout, set by a
	 * timeout for the next epoch outside a recovery to start at the
	 * window it finds (K = 0).
	 */
	uint32_t cubic_w_max;
	uint32_t cubic_cwnd_prior;
	uint32_t cubic_origin;
	uint32_t cubic_k_ms;
	uint32_t cubic_w_est;
	uint32_t cubic_est_rem;
	uint32_t cubic_grow_rem;
	uint64_t cubic_epoch_ms;
	uint64_t cubic_limited_since_ms;
	unsigned cubic_epoch_started;
	unsigned cubic_after_timeout;
	/*
	 * Delayed acknowledgements (RFC 1122 4.2.3.2, RFC 5681 4.2): the
	 * in-order segments received since this end last sent anything, which
	 * the second of them acknowledges at once; one alone is acknowledged
	 * when the network worker has taken every segment that had arrived
	 * (tcp_ack_flush).  ack_listed says the connection is on that list,
	 * linked by ack_next, with a reference the flush drops.
	 */
	unsigned ack_owed;
	unsigned ack_listed;
	struct tcp_socket *ack_next;
	/*
	 * The way the connection's IPv4 segments go, as the last route lookup
	 * found it: its source address and next hop, through the interface
	 * the connection is bound to (inet.ifindex), valid while route_cached
	 * is set and route_generation() still returns route_generation_seen.
	 * A peer that is one of this host's own addresses is never kept (its
	 * segments go round the loopback device).  Guarded by the socket lock.
	 */
	unsigned route_cached;
	unsigned long route_generation_seen;
	uint32_t route_source;
	uint32_t route_next_hop;
	/*
	 * The same for IPv6 segments, under route_cached and
	 * route_generation_seen too: the interface the route goes out of
	 * (route_ifindex), its next hop, the interface's hop limit and the
	 * path's MTU; the source is inet.local6, which the first segment
	 * fixed.
	 */
	unsigned route_ifindex;
	struct in6_addr route_next_hop6;
	unsigned route_hop_limit;
	unsigned route_mtu;
	/*
	 * Segments queued and not yet acknowledged, oldest first.  Several
	 * may be in flight at once, bounded by the peer's advertised window
	 * and by the size of this ring; each keeps a copy of its payload so
	 * it can be sent again.  The deadline and the attempt count belong to
	 * the oldest entry, which is the one a timeout resends.
	 *
	 * The last send_unsent entries have not been handed to the device yet
	 * (ws220-p003): the device or the packet pool refused them, or a
	 * refused one came before them.  They go out in order, oldest first,
	 * through the output retry below, never through the retransmission
	 * timer, which the first sent entry starts.  So three positions follow
	 * a connection: send_unacknowledged; the end of what was sent (the
	 * first unsent entry's sequence, or send_next with none: RFC 9293's
	 * SND.NXT); and send_next, the end of what was queued, which the next
	 * segment takes.  Unlike BSD's snd_max, send_next is never behind
	 * what was sent.
	 */
	struct tcp_pending send_queue[CONFIG_TCP_SEND_QUEUE_MAX];
	unsigned send_first;
	unsigned send_count;
	unsigned send_unsent;
	/* The ring's copies that are jumbo packets (within TCP_SEND_JUMBO_MAX). */
	unsigned send_jumbo;
	/*
	 * The payload bytes of the ring's entries, which the socket's send
	 * buffer (SO_SNDBUF, send_hiwat_bytes) bounds: a data segment joins
	 * the ring while they stay within it, or the ring has no data.
	 */
	size_t send_bytes;
	/*
	 * Nagle's algorithm as Minshall changed it (and Linux has it): the
	 * end of the last data segment shorter than send_mss handed to the
	 * device, while send_small_outstanding says it is not yet
	 * acknowledged.  Only then is a short last entry held back.
	 */
	uint32_t send_small_end;
	unsigned send_small_outstanding;
	/*
	 * The end of the most ever handed to the device (BSD's snd_max): a
	 * timeout's go-back-N or a Packet Too Big makes entries unsent again
	 * that the peer may have had all along, and its acknowledgement of
	 * them, up to here, is taken.
	 */
	uint32_t send_max;
	/*
	 * Set while one thread hands unsent entries to the device with the
	 * socket lock released; another that queues meanwhile leaves its
	 * entry unsent for that one to send, so entries go out in order.
	 * Only the thread that set it moves the end of what was sent.
	 */
	unsigned send_flushing;
	/*
	 * Pure acknowledgements the device or the pool refused, which the
	 * output retry sends again, and the receive_next of the last one
	 * refused: while it stands they are duplicates, each of which the
	 * peer counts towards its fast retransmit; once it has moved, one
	 * acknowledgement says all of them.
	 */
	unsigned ack_pending;
	uint32_t ack_pending_receive_next;
	/*
	 * When the output retry runs for the unsent entries and the pending
	 * acknowledgements: on the network worker's next pass after a full
	 * device refused them (the device's transmit completion runs a pass),
	 * and at the deadline in any case.  Both are 0 while nothing waits.
	 */
	unsigned output_retry_on_pass;
	uint64_t output_retry_deadline;
	uint64_t retransmit_deadline;
	unsigned retransmit_count;
	/*
	 * The tick the retransmission timer first found the oldest segment
	 * unacknowledged since the peer last answered, or 0 while no timeout
	 * has run since then.  The attempt is given up once the timer has
	 * gone on resending for RFC 1122's R2 from that tick; an
	 * acknowledgement of any kind sets it back to 0, so a peer that
	 * answers is never given up.
	 */
	uint64_t retransmit_since;
	/*
	 * Set when the device or the pool refused the timer's last resend of
	 * the oldest segment, so a resend refused again is tried after
	 * TCP_SEND_RETRY_MS rather than the next tick (ws220-p003 review-1
	 * M2); 0 again once a resend goes or the timer starts over.
	 */
	unsigned retransmit_refused;
	/*
	 * The round-trip time estimate of RFC 6298 (ws220-p004): how many
	 * samples have been taken (0: none, so the next is the first of
	 * section 2.2), the smoothed round-trip time and its variation in
	 * microseconds, and the retransmission timeout, in clock ticks before
	 * any backoff (0 stands for the initial second; a handshake whose SYN
	 * timed out sets three seconds, section 5.7).
	 *
	 * rto_backoff is how many times that timeout is doubled: one more for
	 * each timeout that really expired, back to 0 only with a valid sample
	 * (Karn's rule, RFC 6298 section 5 note).  retransmit_count above, the
	 * resends of the oldest segment, is a different count (an immediate
	 * resend adds to it, and an acknowledgement that moves sets it back).
	 * retransmit_immediate marks a deadline set to now for an immediate
	 * resend (recovery, a reopened window), which does not double.
	 *
	 * One segment at a time is timed: while rtt_timing is set, the one
	 * ending at rtt_sequence, handed to the device at tick rtt_start; it
	 * is in the ring and marked sent.  A resend by the timer stops the
	 * timing (Karn), as do the segment going unsent again or taken back
	 * and the ring's reset; the next segment sent starts it again.
	 * syn_timed_out is set when the timer resent the SYN (or SYN-ACK), or
	 * a repeated SYN had the SYN-ACK sent again: the handshake then gives
	 * no sample and the data starts with three seconds.
	 */
	uint32_t rtt_samples;
	uint32_t srtt_us;
	uint32_t rttvar_us;
	uint64_t rto;
	unsigned rto_backoff;
	unsigned retransmit_immediate;
	unsigned rtt_timing;
	uint32_t rtt_sequence;
	uint64_t rtt_start;
	unsigned syn_timed_out;
	/*
	 * NewReno's recovery (RFC 6582, ws220-p005): duplicate_acks counts the
	 * duplicate acknowledgements in a row; the third starts a fast
	 * retransmit.  A fast retransmit or a timeout that really expired sets
	 * recover to the end of what was sent and recovery: while it lasts,
	 * each acknowledgement that moves but stops short of recover (a
	 * partial one) has the next segment resent at once, and one that
	 * reaches recover ends it.  recover means nothing outside a recovery
	 * (ws220-p005 review-1 G2: compared later it would turn false across
	 * 2 GiB).  recovery_by_timeout says a timeout started the recovery: when
	 * it ends exactly at recover, dupack_guard holds back the duplicates of
	 * that recover (the answers to the resends of a spurious timeout) until
	 * an acknowledgement goes beyond it (review-1 M1).  Duplicates count
	 * only while the oldest segment fits the window (review-1 G1): the
	 * answers to a closed window are not losses.
	 */
	unsigned duplicate_acks;
	uint32_t recover;
	unsigned recovery;
	unsigned recovery_by_timeout;
	unsigned dupack_guard;
	uint32_t dupack_guard_sequence;
	/*
	 * Each resend by the timer takes the next number, which no
	 * acknowledgement sets back: a refused resend is still the last one
	 * when the number is unchanged (review-1 M4).
	 */
	unsigned retransmit_serial;
	/*
	 * The persist state of a window too small for the oldest segment
	 * (ws220-p005, as FreeBSD has it): while persist is set the timer
	 * probes the window every clamp(RTO x 2^persist_shift, 5 s, 60 s)
	 * (the doubling stops at 512), apart from the retransmission backoff;
	 * persist_shift goes back to 0 when the state is entered and left.
	 * last_receive is the tick of the last segment from the peer: a peer
	 * that stops answering is dropped after 12 probes and an idle of
	 * min(2 h, RTO x 2559), and after 60 s once this end has closed
	 * (FIN_WAIT_1, LAST_ACK).  A peer that keeps answering is kept
	 * (RFC 1122 section 4.2.2.17) unless TCP_MAXUNACKTIME is set:
	 * maxunack (ticks, 0: off; maxunack_seconds as set) drops a
	 * connection with a reset when its data has not moved since
	 * unack_since (the tick data was queued with nothing outstanding, or
	 * an acknowledgement took more than a byte; 0 with nothing
	 * outstanding).
	 */
	unsigned persist;
	unsigned persist_shift;
	uint64_t last_receive;
	uint64_t maxunack;
	unsigned maxunack_seconds;
	uint64_t unack_since;
	/*
	 * TCP_NODELAY as the caller set it (set: Nagle's algorithm holds no
	 * short segment), and the idle-probe state for SO_KEEPALIVE: when the
	 * next probe is due and how many have gone unanswered.
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
	/*
	 * Segments that came ahead of receive_next (BUG-222): their payloads
	 * (the header pulled off), sequence numbers and lengths, ordered by
	 * sequence, kept until the gap before them is filled so that one lost
	 * segment costs the peer one resend, not the whole window.
	 */
	struct packet_buf *reorder[TCP_REORDER_MAX];
	uint32_t reorder_sequence[TCP_REORDER_MAX];
	uint32_t reorder_length[TCP_REORDER_MAX];
	unsigned reorder_count;
	/*
	 * With sack_on: the start of the segment last kept ahead of a gap,
	 * whose block an acknowledgement reports first (RFC 2018 section 4),
	 * while sack_recent_set; and a duplicate segment's range that the next
	 * acknowledgement reports once, in its first block (D-SACK, RFC 2883),
	 * while dsack_pending.  Guarded by the socket lock.
	 */
	uint32_t sack_recent;
	unsigned sack_recent_set;
	uint32_t dsack_start;
	uint32_t dsack_end;
	unsigned dsack_pending;
	struct tcp_counters counters;
};

int
tcp_init(void);
int
tcp_socket_create(
	int protocol,
	struct socket **result);
int
tcp_stats_copy(
	void *output,
	size_t capacity,
	size_t *needed,
	int superuser);
const char *
tcp_cc_name(
	unsigned cc);
int
tcp_cc_lookup(
	const char *name,
	size_t length,
	unsigned *cc);
unsigned
tcp_cc_default(void);
void
tcp_cc_set_default(
	unsigned cc);

#endif
