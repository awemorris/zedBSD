/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The TCP socket implementation.
 *
 * A socket may have several segments outstanding at once, held oldest first
 * in a fixed ring.  How many is bounded by that ring and by the window the
 * peer advertises, so a sender does not have to wait a round trip for each
 * segment.  An acknowledgement retires every segment it covers.  The network
 * worker's timer resends the oldest outstanding segment with exponential
 * backoff until it is acknowledged or the attempt is abandoned; the ones
 * behind it follow once it is taken.
 *
 * A segment the device or the packet pool refuses stays in the ring unsent
 * (ws220-p003), with every segment queued after it, and the writer goes on:
 * the network worker sends them in order on its next pass after the device
 * has room (its transmit completion runs one) or when the pool may have
 * packets again.  A pure acknowledgement refused the same way is counted
 * and sent again with them.
 *
 * Listeners hold half-open children until their handshake completes and then
 * queue them for accept.  Payload is delivered in order only; anything else
 * is dropped and re-acknowledged.
 *
 * An AF_INET6 socket (ws130-p003) speaks IPv6 to an IPv6 peer, and IPv4 to
 * an IPv4-mapped one; a listener on [::] that is not IPV6_V6ONLY takes the
 * IPv4 connections for its port too, whose children are IPv4-mapped.
 */

#include "kern/net/tcp-socket.h"
#include "kern/net/byteorder.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/route.h"
#include "kern/clock.h"
#include "kern/kmem.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/uaccess.h"
#include "internal.h"
#include "ipv6.h"
#include "tcp-cubic.h"
#include "wire.h"
#include <kern/kcrt.h>

#include <uapi/netinet.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U

/*
 * The largest payload a segment of this end carries: one standard packet
 * holds it after the default headroom (the link and IP headers) with a TCP
 * header of the longest options.  A connection's segments are its
 * send_mss: the smaller of the peer's offer and what the interface carries.
 */
#define TCP_SEGMENT_MAX (PACKET_BUF_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM - 64U)

/*
 * The same in the largest packet, a jumbo one where the pool has them: the
 * segments of an interface whose frames are larger than a standard packet
 * holds.
 */
#define TCP_SEGMENT_MAX_JUMBO (PACKET_BUF_MAX_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM - 64U)

/*
 * The segment size towards an IPv4 peer behind a gateway: Ethernet's.
 * Nothing here learns an IPv4 path's MTU (ICMP's fragmentation needed is
 * not read), and a router between two jumbo networks may well carry only
 * 1500-byte frames, which would drop every larger segment unseen.
 */
#define TCP_GATEWAY_MSS 1460U

/*
 * The jumbo packets one connection holds at most: as copies in its send
 * ring, and promised by its receive window.  The pool has few of them
 * (PACKET_BUF_JUMBO_COUNT, some kept for the drivers' receive), so that one
 * connection leaves room for others; eight segments of a 9000-byte MTU
 * still keep a gigabit link busy over a LAN's round trip.
 */
#define TCP_SEND_JUMBO_MAX 8U
#define TCP_RECEIVE_JUMBO_MAX 8U

/*
 * The segment size a peer whose SYN offered none takes (RFC 9293 section
 * 3.7.1: 536 over IPv4; RFC 8200 section 8.3: 1220 over IPv6).
 */
#define TCP_DEFAULT_MSS 536U
#define TCP_DEFAULT_MSS6 1220U

/*
 * The headroom of a segment's copy in the send ring: the default headroom
 * for the link and IP headers and a TCP header with the longest options,
 * which its first transmission pushes in front of the payload.  Two bytes
 * more than that put the frame of an IPv4 segment without options (54
 * bytes of headers) on a word, which is where the device's copy of it is
 * fastest.
 */
#define TCP_COPY_HEADROOM (PACKET_BUF_DEFAULT_HEADROOM + 62U)

/* The IPv4 and TCP headers without options, and the IPv6 and TCP headers. */
#define TCP_HEADERS 40U
#define TCP_HEADERS6 60U

/*
 * The congestion window a connection starts with (RFC 6928): ten segments,
 * but no more than 14600 bytes unless two segments are larger.
 */
#define TCP_INITIAL_WINDOW_SEGMENTS 10U
#define TCP_INITIAL_WINDOW_BYTES 14600U

/* The largest congestion window, well within 32 bits. */
#define TCP_CWND_MAX 0x40000000U

/* The TCP option kinds this end reads in a SYN (RFC 9293 section 3.2). */
#define TCP_OPTION_END 0U
#define TCP_OPTION_NOP 1U
#define TCP_OPTION_MSS 2U
#define TCP_OPTION_WSCALE 3U
#define TCP_OPTION_SACK_PERMITTED 4U
#define TCP_OPTION_SACK 5U

/*
 * The most SACK blocks an acknowledgement carries: four fill the forty
 * bytes of options with the two NOPs in front (RFC 2018 section 3, no
 * timestamps here).
 */
#define TCP_SACK_BLOCKS_MAX 4U

/* The most holes the SACK blocks show that one acknowledgement has resent (RFC 6675 NextSeg). */
#define TCP_SACK_RESENDS_MAX 4U

/* The largest window scale shift (RFC 7323 section 2.3). */
#define TCP_WSCALE_MAX 14U

/*
 * This end's window scale shift: the smallest that lets the window cover
 * the largest receive buffer a socket may ask for (SOCKET_BUFFER_MAX, 1 MB:
 * 65535 << 5 is just over 2 MB, << 4 just under 1 MB).  The windows it
 * advertises then go in steps of 32 bytes, always rounded down.
 */
#define TCP_RECEIVE_WSCALE 5U

/*
 * The receive buffer a connection starts with: twice the common default
 * where the pool is large (the 64-bit machines), so that a window of 128
 * KB, which the scale lets the peer see, keeps a fast link busy over a
 * longer round trip.  Its queue's packet limit takes that many bytes
 * however the segments are cut (TCP_RECEIVE_PACKETS of
 * SOCKET_STREAM_PACKET_BYTES).  SO_RCVBUF sets another, and on the 64-bit
 * machines the buffer grows by itself (TCP_RECEIVE_AUTO).
 */
#if PACKET_BUF_POOL_COUNT >= 1024U
#define TCP_RECEIVE_BUFFER (128U * 1024U)
#else
#define TCP_RECEIVE_BUFFER SOCKET_BUFFER_DEFAULT
#endif

/*
 * Whether a connection's receive buffer grows by itself, as the transfer
 * asks, from TCP_RECEIVE_BUFFER up to TCP_RECEIVE_AUTO_MAX: on the machines
 * whose pool has room for it (the 64-bit ones; the small pool of the
 * 32-bit boards keeps its fixed queue).  A connection whose window is what
 * holds the transfer back (a fast link with a round trip longer than 128
 * KB lasts: 2.5 Gbit/s over 1 ms takes 300 KB) would otherwise run at the
 * window over the round trip.  SO_RCVBUF turns it off for its socket.
 *
 * Procedure referenced: M. Fisk and W. Feng, "Dynamic Right-Sizing in
 * TCP", Los Alamos Computer Science Institute Symposium, 2001 (the
 * receiver takes the time one window's worth of data takes to arrive as
 * the round trip, and keeps its buffer at twice what one round trip
 * brings).
 */
#if PACKET_BUF_POOL_COUNT >= 1024U
#define TCP_RECEIVE_AUTO 1U
#else
#define TCP_RECEIVE_AUTO 0U
#endif

/*
 * The largest buffer a connection grows to by itself: the largest
 * SO_RCVBUF takes, which this end's window scale (TCP_RECEIVE_WSCALE)
 * still covers.
 */
#define TCP_RECEIVE_AUTO_MAX SOCKET_BUFFER_MAX

/*
 * The share of the packet pool the receive queues may take together
 * beyond each one's TCP_RECEIVE_PACKETS, grown by themselves or by
 * SO_RCVBUF: a quarter, so that a connection with the largest buffer
 * leaves the pool to the copies the senders keep and to the frames the
 * devices receive.
 */
#define TCP_RECEIVE_EXTRA_SHARE 4U

/*
 * The send buffer a connection starts with: on the 64-bit machines more
 * than their ring holds in segments of an Ethernet MTU (256 of 1460
 * bytes), so the ring bounds a transfer unless SO_SNDBUF sets a smaller
 * buffer; elsewhere the common default, which is more than their ring of
 * 8 holds.
 */
#if PACKET_BUF_POOL_COUNT >= 1024U
#define TCP_SEND_BUFFER (384U * 1024U)
#else
#define TCP_SEND_BUFFER SOCKET_BUFFER_DEFAULT
#endif

/*
 * The largest segment this end takes, which the SYN offers (BUG-222): an
 * Ethernet frame's 1500 bytes less the IP and TCP headers.  It fits one
 * packet's storage with its headers.
 */
#define TCP_RECEIVE_MSS 1460U

/* The same over IPv6, whose header is 20 bytes larger (ws130-p003). */
#define TCP_RECEIVE_MSS6 1440U

/* How a listener matches a segment: not, on the wildcard address, or on the exact one. */
#define TCP_MATCH_NONE		0
#define TCP_MATCH_WILDCARD	1
#define TCP_MATCH_EXACT		2

/*
 * The addresses of a segment that came in: IPv4 ones (family AF_INET), or
 * IPv6 ones with the interface it came in on (ws130-p003).
 */
struct tcp_segment_addresses {
	int family;
	uint32_t source;
	uint32_t destination;
	struct in6_addr source6;
	struct in6_addr destination6;
	unsigned ifindex;
};

/*
 * How many packets a socket's receive queue may hold.  The window is
 * SOCKET_STREAM_PACKET_BYTES for each one still free (the queue fills a
 * packet to at least that before it takes another), so this is what bounds
 * a transfer over a link with any delay (the common limit of 8 was
 * an 8 KB window, about 1 MB/s on a LAN).  The large pool of the 64-bit
 * machines gives a connection 128, the 128 KB of its buffer
 * (TCP_RECEIVE_BUFFER) however the peer cuts its segments: the window
 * promised is then always taken.  The small pool of the 32-bit boards keeps
 * the common limit.  CONFIG_TCP_RECEIVE_PACKETS sets another (a build that
 * measures longer round trips).
 */
#if defined(CONFIG_TCP_RECEIVE_PACKETS)
#define TCP_RECEIVE_PACKETS ((unsigned)CONFIG_TCP_RECEIVE_PACKETS)
#elif PACKET_BUF_POOL_COUNT >= 1024U
#define TCP_RECEIVE_PACKETS 128U
#elif PACKET_BUF_POOL_COUNT >= 256U
#define TCP_RECEIVE_PACKETS 44U
#else
#define TCP_RECEIVE_PACKETS SOCKET_RECEIVE_MESSAGES_MAX
#endif
/*
 * How many segments ahead of a gap a connection keeps (BUG-222): the
 * larger pool of the 64-bit machines has room for TCP_REORDER_MAX of them,
 * the small pool of the 32-bit boards for a few.
 */
#if PACKET_BUF_POOL_COUNT >= 256U
#define TCP_REORDER_LIMIT TCP_REORDER_MAX
#else
#define TCP_REORDER_LIMIT 4U
#endif
#define TCP_EPHEMERAL_FIRST 49152U
/* The initial retransmission timeout of RFC 6298, one second. */
#define TCP_INITIAL_RTO KERN_MS_TO_TICKS(1000U)

/*
 * The least term the round-trip variation adds to the smoothed round-trip
 * time in the retransmission timeout (ws220-p004): the timeout is
 * SRTT + max(G, 4 RTTVAR, 200 ms), always 200 ms beyond the round trip,
 * as Linux has it, so a peer that delays its acknowledgement up to 200 ms
 * does not look lost.  RFC 6298 section 2.4 asks for at least a second;
 * the user chose 200 ms (2026-10-13).
 */
#define TCP_RTO_MIN_TERM_US 200000U

/*
 * The timeout a connection starts its data with when its SYN had to be
 * resent and no round trip was measured (RFC 6298 section 5.7).
 */
#define TCP_SYN_RETRIED_RTO KERN_MS_TO_TICKS(3000U)

/*
 * The persist timer (ws220-p005, FreeBSD's): the probe interval is the RTO
 * doubled by the persist backoff and held between 5 and 60 seconds; the
 * backoff stops at its twelfth step; a peer silent through all of them is
 * given up after min(2 hours, RTO x the backoff's total, 2559); one this end
 * has closed to after 60 seconds of silence.
 */
#define TCP_PERSIST_MIN KERN_MS_TO_TICKS(5000U)
#define TCP_PERSIST_MAX KERN_MS_TO_TICKS(60000U)
#define TCP_PERSIST_SHIFT_MAX 12U
#define TCP_PERSIST_TOTAL_BACKOFF 2559U
#define TCP_PERSIST_IDLE_MAX KERN_MS_TO_TICKS(7200000U)
#define TCP_PERSIST_CLOSED_IDLE KERN_MS_TO_TICKS(60000U)

/*
 * The system's TCP_MAXUNACKTIME for a new socket, in milliseconds: 0 turns
 * the check off, as on FreeBSD (the user's decision, 2026-10-13).
 */
#ifndef CONFIG_TCP_MAXUNACKTIME_MS
#define CONFIG_TCP_MAXUNACKTIME_MS 0U
#endif

/*
 * The congestion control a new socket starts with until net.tcp.congestion
 * sets another (TCP_CC_RENO or TCP_CC_CUBIC): CUBIC, as Linux, Windows and
 * FreeBSD have it, which keeps a long round trip's link busier after a
 * loss and is no slower on a LAN.
 */
#ifndef CONFIG_TCP_CC_DEFAULT
#define CONFIG_TCP_CC_DEFAULT TCP_CC_CUBIC
#endif

/* The congestion controls by number (TCP_CC_*), with the names TCP_CONGESTION and net.tcp.congestion give them. */
#define TCP_CC_COUNT 2U

/* The duplicate acknowledgements in a row that start a fast retransmit (RFC 5681 section 3.2). */
#define TCP_DUPLICATE_ACKS_FAST 3U

/*
 * The in-order segments one acknowledgement waits for at most; fewer are
 * acknowledged when the network worker has taken the segments that had
 * arrived (tcp_ack_flush), so a flight that pauses is acknowledged at
 * once.  RFC 5681 section 4.2 asks for every second full segment; the
 * 64-bit machines, whose receive window holds 128 segments, acknowledge
 * every eighth, as a receiver that merges segments (Linux's GRO) does
 * more sparsely still: at 10 Gbit/s an acknowledgement for every second
 * segment costs the sender more than the link (a peer handling 200,000
 * acknowledgements a second sent 3 Gbit/s, one handling a quarter of them
 * 5 Gbit/s).  The 32-bit boards, whose window is a few segments, keep two.
 */
#ifndef CONFIG_TCP_ACK_SEGMENTS
#if PACKET_BUF_POOL_COUNT >= 1024U
#define CONFIG_TCP_ACK_SEGMENTS 8
#else
#define CONFIG_TCP_ACK_SEGMENTS 2
#endif
#endif
#define TCP_ACK_SEGMENTS ((unsigned)CONFIG_TCP_ACK_SEGMENTS)

/*
 * What an immediate resend (retransmit_immediate) is for (ws220-p005): a
 * loss (a fast retransmit or a partial acknowledgement's), whose resend
 * stops any timing (review-1 M2), or a probe the reopened window now takes,
 * which stops only its own.
 */
#define TCP_IMMEDIATE_LOSS 1U
#define TCP_IMMEDIATE_PROBE 2U

/* The longest round-trip sample taken, so the sums in microseconds stay within 32 bits. */
#define TCP_RTT_SAMPLE_MAX_US 60000000U
/* How long closing a connection waits for what it sent to be taken. */
#define TCP_CLOSE_DRAIN_MS 10000U

/* What a closing connection waits for. */
enum tcp_drain {
	/* A free entry in the retransmission ring, for the FIN. */
	TCP_DRAIN_ROOM,
	/* Every byte of data acknowledged; a FIN may still be outstanding. */
	TCP_DRAIN_DATA,
	/* Everything acknowledged, the FIN included. */
	TCP_DRAIN_ALL,
};
/*
 * The pause before sending again when buffers are short: a connect whose
 * SYN found no packet for its copy; a writer whose copy found none; the
 * output retry of segments the packet pool refused, or a device that
 * refused again on a retry that sent nothing; and a timer's resend the
 * device refused again.
 */
#define TCP_SYN_RETRY_MS 250U
#define TCP_SEND_RETRY_MS 10U

/*
 * When the output retry runs at the latest after a full device refused a
 * segment (ws220-p003).  The device's transmit completion runs the network
 * worker sooner; this is for a device that runs it for nothing.
 */
#define TCP_OUTPUT_RETRY_DEVICE_MS 1U

/*
 * The most pure acknowledgements of one receive_next kept to be sent again:
 * three duplicates are what a peer's fast retransmit waits for.
 */
#define TCP_ACK_PENDING_MAX 3U

/*
 * The longest the retransmission timeout grows to as it doubles for each
 * resend: RFC 6298 section 2.5 allows a maximum of at least 60 seconds.
 */
#define TCP_RETRANSMIT_TIMEOUT_MAX KERN_MS_TO_TICKS(60000U)

/*
 * How long the timer goes on resending the oldest segment before the
 * attempt is given up (RFC 1122 section 4.2.3.5, R2), counted from the
 * first timeout after the peer last answered.  R2 is at least 100 seconds
 * for data and at least 3 minutes for a SYN; a time rather than a count of
 * resends, so it holds whatever the timeouts are (ws220-p002).
 */
#define TCP_GIVE_UP_DATA_MS 100000U
#define TCP_GIVE_UP_SYN_MS 180000U

/*
 * A test build may start every connection's sequence numbers this many
 * bytes before they wrap (ws220-p002), so that a transfer of a few times
 * that many bytes crosses the wrap every time instead of one time in
 * four for a gigabyte.  0, the default, chooses them as usual.
 */
#ifndef CONFIG_TCP_ISN_WRAP_BEFORE
#define CONFIG_TCP_ISN_WRAP_BEFORE 0U
#endif

/* How a reset that came in is taken (RFC 5961 section 3.2). */
enum tcp_reset_check {
	/* Its sequence number is not within the window: it is ignored. */
	TCP_RESET_IGNORE,
	/* It is within the window but not the next expected: a challenge acknowledgement answers it. */
	TCP_RESET_CHALLENGE,
	/* It is exactly the next sequence number expected: the connection is reset. */
	TCP_RESET_ACCEPT,
};

/*
 * What became of a segment handed to the output, and how it is sent again
 * (ws220-p003).  Only DEVICE and WAIT keep a segment unsent for the output
 * retry; LOST and HARD leave it sent, to the retransmission timer.
 */
enum tcp_refusal {
	/* The device took it. */
	TCP_REFUSAL_NONE,
	/* A full device queue: on the network worker's next pass, which the device's transmit completion runs. */
	TCP_REFUSAL_DEVICE,
	/* An empty packet pool: after TCP_SEND_RETRY_MS. */
	TCP_REFUSAL_WAIT,
	/*
	 * A neighbour still being resolved (IPv4 asked ARP for it): lost as on
	 * the wire, so the timer's backoff and R2 bound how often ARP is asked
	 * again and when a peer that never answers is given up (review-1 G1).
	 */
	TCP_REFUSAL_LOST,
	/* An error that will not pass (no route, a device gone, too large). */
	TCP_REFUSAL_HARD,
};

/*
 * Idle probing for SO_KEEPALIVE, in clock ticks.  The defaults are the
 * traditional ones: probe after two hours of silence, then every 75 seconds,
 * and give up after nine unanswered probes.  A build may shorten them so the
 * behaviour can be observed in a test that does not run for hours.
 */
#ifndef CONFIG_TCP_KEEPALIVE_IDLE_MS
#define CONFIG_TCP_KEEPALIVE_IDLE_MS 7200000U
#endif
#ifndef CONFIG_TCP_KEEPALIVE_INTERVAL_MS
#define CONFIG_TCP_KEEPALIVE_INTERVAL_MS 75000U
#endif
#ifndef CONFIG_TCP_KEEPALIVE_COUNT
#define CONFIG_TCP_KEEPALIVE_COUNT 9U
#endif

#define TCP_KEEPALIVE_IDLE \
	((uint64_t)CONFIG_TCP_KEEPALIVE_IDLE_MS * KERN_CLOCK_HZ / 1000U)
#define TCP_KEEPALIVE_INTERVAL \
	((uint64_t)CONFIG_TCP_KEEPALIVE_INTERVAL_MS * KERN_CLOCK_HZ / 1000U)
#define TCP_KEEPALIVE_COUNT ((unsigned)CONFIG_TCP_KEEPALIVE_COUNT)
#define TCP_LISTEN_BACKLOG_MAX 16U

/*
 * The number of sockets the retransmission timer snapshots on its own
 * stack.  More registered sockets are snapshotted into an allocated array
 * sized to them, so the timer reaches every socket (BUG-107).
 */
#define TCP_TIMER_STACK_SNAPSHOT 32U

struct tcp_endpoint {
	struct tcp_socket tcp;
	struct tcp_endpoint *next;
};

/*
 * The persist backoff, FreeBSD's tcp_backoff: the multiple of the RTO at
 * each step of persist_shift.  Constant for the kernel's life.
 */
static const unsigned tcp_persist_backoff[TCP_PERSIST_SHIFT_MAX + 1U] = {
	1U, 2U, 4U, 8U, 16U, 32U, 64U, 128U, 256U, 512U, 512U, 512U, 512U
};

static struct tcp_endpoint *tcp_sockets;
static uint16_t next_ephemeral;
static struct spinlock tcp_registry_lock;

/*
 * The congestion control a new socket starts with (TCP_CC_*), which
 * net.tcp.congestion sets: one word, read and written atomically under no
 * lock (a socket made meanwhile takes the old one or the new).
 */
static unsigned tcp_cc_default_value = CONFIG_TCP_CC_DEFAULT;

/* The names of the congestion controls by number (TCP_CC_*), which TCP_CONGESTION and net.tcp.congestion use. */
static const char *const tcp_cc_names[TCP_CC_COUNT] = { "reno", "cubic" };

/*
 * The connections owing a delayed acknowledgement, linked by ack_next,
 * each holding a socket reference the flush drops (tcp_ack_flush).  The
 * lock guards the head and the links.
 */
static struct tcp_socket *tcp_ack_head;
static struct spinlock tcp_ack_lock;

/*
 * The packets all receive queues together may take beyond their
 * TCP_RECEIVE_PACKETS (tcp_socket.receive_extra_packets, summed): it stays
 * within TCP_RECEIVE_EXTRA_SHARE of the pool, a socket adding to it only
 * by compare and exchange and taking its part back when it closes.  No
 * lock guards it.
 */
static atomic_uint_t tcp_receive_extra_packets;

static struct tcp_endpoint * tcp_endpoint(struct socket *socket);
static void tcp_forget_peer(struct tcp_endpoint *endpoint);
static void tcp_listener_remove(struct tcp_endpoint *child);
static void tcp_listener_established(struct tcp_endpoint *child);
static int tcp_port_in_use(const struct tcp_endpoint *candidate, int strict);
static int tcp_allocate_port_locked(struct tcp_endpoint *endpoint);
static int tcp_route(struct tcp_endpoint *endpoint, struct net_device **device, uint32_t *source);
static int tcp_route_cached(struct tcp_endpoint *endpoint, struct net_device **device, uint32_t *source, uint32_t *next_hop);
static void tcp_route_remember(struct tcp_endpoint *endpoint, struct net_device *device, uint32_t source, unsigned long generation);
static int tcp_send_segment_at(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, const void *data, size_t length);
static int tcp_send_segment_news(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, const void *data, size_t length, int news_only);
static int tcp_send_segment_cut(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, const void *data, size_t length);
static int tcp_output_segment(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, const void *data, size_t length, struct packet_buf *carrier, int news_only);
static int tcp_send_segment_carried(struct tcp_endpoint *endpoint, uint32_t sequence, uint8_t flags, struct packet_buf *carrier);
static void tcp_stats_fill_locked(const struct tcp_endpoint *endpoint, uint64_t now, int superuser, struct tcp_stats_entry *entry);
static int tcp_allocate_port(struct tcp_endpoint *endpoint);
static enum tcp_refusal tcp_refusal_of(int error);
static uint64_t tcp_refusal_delay(enum tcp_refusal refusal);
static int tcp_refusal_retries(enum tcp_refusal refusal);
static unsigned tcp_sent_count_locked(const struct tcp_endpoint *endpoint);
static uint32_t tcp_sent_end_locked(const struct tcp_endpoint *endpoint);
static void tcp_mark_sent_locked(struct tcp_endpoint *endpoint, uint64_t now);
static void tcp_unmark_sent_locked(struct tcp_endpoint *endpoint, const struct packet_buf *packet);
static void tcp_output_refused_locked(struct tcp_endpoint *endpoint, enum tcp_refusal refusal, int progressed, uint64_t now);
static void tcp_output_retry_settle_locked(struct tcp_endpoint *endpoint);
static int tcp_output_retry_due_locked(const struct tcp_endpoint *endpoint, uint64_t now);
static void tcp_output_retry(struct tcp_endpoint *endpoint, uint8_t *payload, uint64_t now);
static void tcp_send_unsent(struct tcp_endpoint *endpoint, uint8_t *payload);
static void tcp_send_ack_pending(struct tcp_endpoint *endpoint);
static int tcp_send_ack(struct tcp_endpoint *endpoint, int duplicate);
static int tcp_send_ack_news(struct tcp_endpoint *endpoint, int duplicate, int news_only);
static int tcp_sequence_before(uint32_t left, uint32_t right);
static int tcp_sequence_at_or_before(uint32_t left, uint32_t right);
static uint32_t tcp_initial_sequence(uint32_t chosen);
static uint64_t tcp_backoff_timeout(uint64_t rto, unsigned count);
static uint64_t tcp_rto_locked(const struct tcp_endpoint *endpoint);
static void tcp_rtt_sample_locked(struct tcp_endpoint *endpoint, uint64_t now);
static void tcp_rtt_forget_locked(struct tcp_endpoint *endpoint);
static void tcp_rtt_handshake_locked(struct tcp_endpoint *endpoint, uint32_t acknowledgement, uint64_t now);
static int tcp_retransmit_given_up_locked(struct tcp_endpoint *endpoint, uint64_t now);
static enum tcp_reset_check tcp_reset_check_locked(const struct tcp_endpoint *endpoint, uint32_t sequence);
static int tcp_send_window_update_allowed_locked(const struct tcp_endpoint *endpoint, uint32_t sequence, uint32_t acknowledgement);
static int tcp_probe_fits_locked(const struct tcp_endpoint *endpoint);
static int tcp_persist_applies_locked(const struct tcp_endpoint *endpoint);
static uint64_t tcp_persist_interval_locked(const struct tcp_endpoint *endpoint);
static int tcp_persist_given_up_locked(const struct tcp_endpoint *endpoint, uint64_t now);
static int tcp_maxunack_expired_locked(const struct tcp_endpoint *endpoint, uint64_t now);
static void tcp_persist_leave_locked(struct tcp_endpoint *endpoint);
static int tcp_oldest_fits_window_locked(const struct tcp_endpoint *endpoint);
static uint32_t tcp_syn_offered_mss(const struct tcp_wire *tcp, size_t header_length);
static int tcp_syn_offered_wscale(const struct tcp_wire *tcp, size_t header_length);
static int tcp_syn_offered_sack(const struct tcp_wire *tcp, size_t header_length);
static unsigned tcp_sack_block_add(uint32_t blocks[TCP_SACK_BLOCKS_MAX][2], unsigned count, uint32_t start, uint32_t end, uint32_t acknowledgement);
static unsigned tcp_sack_blocks(struct tcp_endpoint *endpoint, uint32_t blocks[TCP_SACK_BLOCKS_MAX][2], uint32_t *acknowledgement);
static uint32_t tcp_segment_window_locked(const struct tcp_endpoint *endpoint, const struct tcp_wire *tcp);
static void tcp_start_sending(struct tcp_endpoint *endpoint, uint32_t offered_mss);
static uint32_t tcp_device_mss(const struct net_device *device, int ipv6, int local);
static uint32_t tcp_path_mss(struct tcp_endpoint *endpoint, struct net_device *device, int ipv6);
static int tcp_copy_is_jumbo(const struct packet_buf *packet);
static uint32_t tcp_receive_mss(const struct tcp_endpoint *endpoint);
static uint32_t tcp_segment_size_locked(const struct tcp_endpoint *endpoint);
static uint32_t tcp_send_limit_locked(const struct tcp_endpoint *endpoint);
static int tcp_send_room_locked(const struct tcp_endpoint *endpoint, size_t length);
static int tcp_send_buffer_room_locked(const struct tcp_endpoint *endpoint, size_t length);
static int tcp_nagle_holds_locked(const struct tcp_endpoint *endpoint);
static int tcp_unsent_fits_cwnd_locked(const struct tcp_endpoint *endpoint);
static size_t tcp_tail_room_locked(const struct tcp_endpoint *endpoint);
static void tcp_cwnd_grow_locked(struct tcp_endpoint *endpoint, uint32_t acked);
static void tcp_cwnd_loss_locked(struct tcp_endpoint *endpoint, int timeout, int repeated);
static void tcp_cwnd_round_reset_locked(struct tcp_endpoint *endpoint);
static void tcp_cwnd_note_sent_locked(struct tcp_endpoint *endpoint);
static int tcp_cwnd_limited_locked(const struct tcp_endpoint *endpoint, uint32_t mss);
static void tcp_cwnd_round_advance_locked(struct tcp_endpoint *endpoint);
static uint64_t tcp_cc_now_ms(void);
static void tcp_cubic_grow_locked(struct tcp_endpoint *endpoint, uint32_t acked, uint32_t mss);
static void tcp_cubic_loss_locked(struct tcp_endpoint *endpoint, uint32_t flight, int timeout);
static void tcp_cc_set_locked(struct tcp_endpoint *endpoint, unsigned cc);
static void tcp_ack_delay(struct tcp_endpoint *endpoint);
static int tcp_send_wake_due_locked(const struct tcp_endpoint *endpoint);
struct tcp_discard {
	struct packet_buf *packets[CONFIG_TCP_SEND_QUEUE_MAX];
	unsigned count;
};

static void tcp_retransmit_reset(struct tcp_endpoint *endpoint, struct tcp_discard *discard);
static void tcp_discard_free(struct tcp_discard *discard);
static uint32_t tcp_in_flight(const struct tcp_endpoint *endpoint);
static void tcp_retire_acknowledged(struct tcp_endpoint *endpoint, uint32_t acknowledgement, struct tcp_discard *discard);
static int tcp_sack_take_locked(struct tcp_endpoint *endpoint, const struct tcp_wire *tcp, size_t header_length);
static void tcp_sack_clear_locked(struct tcp_endpoint *endpoint, int sacked);
static void tcp_sack_resend(struct tcp_endpoint *endpoint);
static void tcp_retransmit_clear(struct tcp_endpoint *endpoint);
static void tcp_reorder_keep(struct tcp_endpoint *endpoint, struct packet_buf **packet, uint32_t sequence);
static void tcp_reorder_drain(struct tcp_endpoint *endpoint, int discard);
static void tcp_reorder_free(struct tcp_endpoint *endpoint);
static void tcp_connect_cancel_locked(struct tcp_endpoint *endpoint, uint32_t generation, struct tcp_discard *discard);
static int tcp_send_reliable(struct tcp_endpoint *endpoint, uint8_t flags, const void *data, const struct uaccess_pin *pin, size_t pin_offset, size_t length);
static ssize_t tcp_send_bytes(struct socket *socket, const void *buffer, const struct uaccess_pin *pin, size_t pin_offset, size_t length, int flags);
static ssize_t tcp_send_pinned(struct socket *socket, const struct uaccess_pin *pin, size_t offset, size_t length, int flags);
static int tcp_bind(struct socket *socket, const struct sockaddr *address, socklen_t length);
static int tcp_listen(struct socket *socket, int backlog);
static int tcp_accept(struct socket *socket, struct socket **result, struct sockaddr *address, socklen_t *length, unsigned io_flags);
static int tcp_connect(struct socket *socket, const struct sockaddr *address, socklen_t length, unsigned io_flags);
static ssize_t tcp_sendto(struct socket *socket, const void *buffer, size_t length, int flags, const struct sockaddr *address, socklen_t address_length);
static ssize_t tcp_recvfrom(struct socket *socket, void *buffer, size_t length, int flags, struct sockaddr *address, socklen_t *address_length);
static uint32_t tcp_receive_window(struct tcp_endpoint *endpoint);
static uint32_t tcp_receive_offered(const struct tcp_endpoint *endpoint);
static uint32_t tcp_receive_sws_step(const struct tcp_endpoint *endpoint);
static unsigned tcp_receive_extra_take(unsigned wanted);
static void tcp_receive_extra_give(unsigned count);
static unsigned tcp_receive_packets_set_locked(struct tcp_endpoint *endpoint, unsigned packets);
static int tcp_receive_clock_us(uint64_t *now_us);
static void tcp_receive_tune_start_locked(struct tcp_endpoint *endpoint, uint64_t now_us);
static void tcp_receive_tune_locked(struct tcp_endpoint *endpoint);
static void tcp_buffer_changed(struct socket *socket, int option);
static int tcp_reset_error_locked(const struct tcp_endpoint *endpoint, uint8_t flags, uint32_t acknowledgement);
static void tcp_send_reset(const struct tcp_segment_addresses *addresses, const struct tcp_wire *segment, size_t payload_length);
static void tcp_send_reset6(const struct tcp_segment_addresses *addresses, struct packet_buf *packet);
static int tcp_shutdown(struct socket *socket, int how);
static int tcp_getsockname(struct socket *socket, struct sockaddr *address, socklen_t *length);
static int tcp_getpeername(struct socket *socket, struct sockaddr *address, socklen_t *length);
static void tcp_close(struct socket *socket);
static void tcp_close_drain(struct tcp_endpoint *endpoint, enum tcp_drain until);
static void tcp_endpoint_close(struct socket *socket);
static int tcp_poll(struct socket *socket, short events, short *revents);
static int tcp_setsockopt(struct socket *socket, int level, int option, const void *value, socklen_t length);
static int tcp_getsockopt(struct socket *socket, int level, int option, void *value, socklen_t *length);
static void tcp_keepalive_changed(struct socket *socket);
static void tcp_keepalive_arm_locked(struct tcp_endpoint *endpoint);
static struct tcp_endpoint * tcp_lookup(const struct tcp_segment_addresses *addresses, uint16_t source_port, uint16_t destination_port);
static int tcp_connection_matches(const struct tcp_endpoint *endpoint, const struct tcp_segment_addresses *addresses, uint16_t source_port, uint16_t destination_port);
static int tcp_listener_matches(const struct tcp_endpoint *endpoint, const struct tcp_segment_addresses *addresses, uint16_t destination_port);
static struct tcp_endpoint * tcp_passive_syn(struct tcp_endpoint *listener, const struct tcp_segment_addresses *addresses, uint16_t source_port, uint32_t sequence, uint32_t offered_mss, int offered_wscale, int offered_sack);
static void tcp_child_addresses(struct tcp_endpoint *child, const struct tcp_endpoint *listener, const struct tcp_segment_addresses *addresses);
static int tcp_input(struct packet_buf *packet, uint32_t source, uint32_t destination);
static int tcp6_input(struct packet_buf *packet, const struct in6_addr *source, const struct in6_addr *destination);
static int tcp_segment_input(struct packet_buf *packet, const struct tcp_segment_addresses *addresses);
static int tcp6_route(struct tcp_endpoint *endpoint, struct net_device **device, struct in6_addr *source);
static int tcp6_route_cached(struct tcp_endpoint *endpoint, struct net_device **device, struct in6_addr *source, struct ipv6_way *way);
static void tcp6_route_remember(struct tcp_endpoint *endpoint, struct net_device *device, unsigned long generation);

static const struct socket_ops tcp_ops = {
	.bind = tcp_bind,
	.connect = tcp_connect,
	.listen = tcp_listen,
	.accept = tcp_accept,
	.sendto = tcp_sendto,
	.send_pinned = tcp_send_pinned,
	.recvfrom = tcp_recvfrom,
	.shutdown = tcp_shutdown,
	.getsockname = tcp_getsockname,
	.getpeername = tcp_getpeername,
	.ioctl = inet_socket_ioctl,
	.poll = tcp_poll,
	.close = tcp_close,
	.endpoint_close = tcp_endpoint_close,
	.buffer_changed = tcp_buffer_changed,
	.setsockopt = tcp_setsockopt,
	.getsockopt = tcp_getsockopt,
	.keepalive_changed = tcp_keepalive_changed,
};

/*
 * Creates a closed TCP socket and registers it.
 */
int
tcp_socket_create(
	int protocol,
	struct socket **result)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;

	/* Rejects a missing result or a protocol other than TCP. */
	if (result == NULL || (protocol != 0 && protocol != IPPROTO_TCP))
		return EPROTONOSUPPORT;

	/* Allocates the endpoint and links it into the registry. */
	endpoint = kern_calloc(1, sizeof(*endpoint));
	if (endpoint == NULL)
		return ENOMEM;
	inet_socket_object_init(&endpoint->tcp.inet, SOCK_STREAM, IPPROTO_TCP,
	    &tcp_ops);
	endpoint->tcp.state = TCP_CLOSED;

	/*
	 * Its receive queue takes the packets the window it offers counts, and
	 * the bytes of its buffer, which grows by itself where the pool has
	 * room until SO_RCVBUF sets one.
	 */
	endpoint->tcp.inet.socket.receive_packet_limit = TCP_RECEIVE_PACKETS;
	endpoint->tcp.inet.socket.receive_hiwat_bytes = TCP_RECEIVE_BUFFER;
	endpoint->tcp.receive_auto = (uint8_t)TCP_RECEIVE_AUTO;
	endpoint->tcp.inet.socket.send_hiwat_bytes = TCP_SEND_BUFFER;

	/* The system's TCP_MAXUNACKTIME (ws220-p005). */
	endpoint->tcp.maxunack_seconds = CONFIG_TCP_MAXUNACKTIME_MS / 1000U;
	endpoint->tcp.maxunack = KERN_MS_TO_TICKS(CONFIG_TCP_MAXUNACKTIME_MS);

	/* The system's congestion control, net.tcp.congestion. */
	endpoint->tcp.cc = tcp_cc_default();

	/* Puts the socket in the registry of sockets. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	endpoint->next = tcp_sockets;
	tcp_sockets = endpoint;

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	*result = &endpoint->tcp.inet.socket;

	/* Reports the created socket. */
	return 0;
}

/*
 * Initializes the TCP registry and registers the input handler.
 */
int
tcp_init(
	void)
{
	int error;

	tcp_sockets = NULL;
	next_ephemeral = TCP_EPHEMERAL_FIRST;
	spin_init(&tcp_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "TCP socket registry");
	spin_init(&tcp_ack_lock, LOCK_RANK_NETWORK, "TCP delayed acknowledgements");
	tcp_ack_head = NULL;

	/* Reports why the registration failed. */
	error = ipv4_protocol_register(IPPROTO_TCP, tcp_input);
	if (error != 0)
		return error;

	/* And from IPv6 (ws130-p003). */
	error = ipv6_protocol_register(IPPROTO_TCP, tcp6_input);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Decides what an expired idle probe means for one socket.
 *
 * The caller holds the socket lock.  Returns 1 when a probe should be sent,
 * -1 when the peer has failed to answer every probe allowed, and 0 when the
 * socket no longer needs one.
 */
static int
tcp_keepalive_expire_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now)
{
	/* Stops probing a connection that is no longer established. */
	if (endpoint->tcp.inet.socket.keepalive == 0 ||
	    endpoint->tcp.state != TCP_ESTABLISHED) {
		endpoint->tcp.keepalive_deadline = 0;
		endpoint->tcp.keepalive_probes = 0;
		return 0;
	}

	/* Declares the peer unreachable once every probe went unanswered. */
	if (endpoint->tcp.keepalive_probes >= TCP_KEEPALIVE_COUNT) {
		endpoint->tcp.keepalive_deadline = 0;
		endpoint->tcp.keepalive_probes = 0;
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		return -1;
	}

	/* Schedules the next probe and asks for this one. */
	endpoint->tcp.keepalive_probes++;
	endpoint->tcp.keepalive_deadline = now + TCP_KEEPALIVE_INTERVAL;
	return 1;
}

/*
 * Takes an ICMPv6 error about a segment this host sent (ws130-p003): a
 * refusal (port unreachable) ends a connect still sending its SYN, as a
 * reset would; the other errors are soft (RFC 1122 section 4.2.3.9) and
 * leave the connection to its timers.
 */
void
tcp6_error(
	const struct in6_addr *source,
	const struct in6_addr *destination,
	uint16_t source_port,
	uint16_t destination_port,
	int error)
{
	struct tcp_segment_addresses addresses;
	struct tcp_endpoint *endpoint;
	struct tcp_discard retransmit;
	unsigned long socket_irq;
	int refused;

	/* Only a refusal is a hard error. */
	if (error != ECONNREFUSED)
		return;

	/* The connection the segment went from, found as its answer would be. */
	kern_memset(&addresses, 0, sizeof(addresses));
	addresses.family = AF_INET6;
	addresses.source6 = *destination;
	addresses.destination6 = *source;
	endpoint = tcp_lookup(&addresses, destination_port, source_port);
	if (endpoint == NULL)
		return;

	/* A connect still sending its SYN is refused. */
	retransmit.count = 0;
	socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	refused = 0;
	if (endpoint->tcp.state == TCP_SYN_SENT) {
		tcp_retransmit_reset(endpoint, &retransmit);
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		tcp_forget_peer(endpoint);
		refused = 1;
	}

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/* The connect hears of it. */
	tcp_discard_free(&retransmit);
	if (refused) {
		socket_set_error(&endpoint->tcp.inet.socket, ECONNREFUSED);
		socket_wake_connect(&endpoint->tcp.inet.socket);
	}
	socket_release(&endpoint->tcp.inet.socket);
}

/*
 * Takes a Packet Too Big for a destination (RFC 8201 section 4): each
 * connection to it whose segments the new path MTU no longer carries
 * cuts them to it (never under IPv6's minimum), and the entries already
 * sent that are larger, which the path dropped, go again at once, cut, as
 * Linux's simple retransmit has them: they are unsent again from the
 * first of them, so the output retry sends them in order, and they are
 * not a loss the congestion window answers.  Entries queued larger than
 * the new size are cut when they are sent (tcp_send_segment_cut).
 */
void
tcp6_path_mtu_lowered(
	const struct in6_addr *destination,
	unsigned mtu)
{
	struct tcp_endpoint *endpoint;
	struct tcp_pending *entry;
	unsigned long irq;
	unsigned long socket_irq;
	unsigned index;
	unsigned slot;
	unsigned sent;
	uint32_t mss;
	int ipv6;
	int same;
	int wake;

	/* The segment size the path carries. */
	if (mtu < IPV6_MINIMUM_MTU)
		mtu = IPV6_MINIMUM_MTU;
	mss = mtu - TCP_HEADERS6;

	/* Every connection to the destination over IPv6. */
	wake = 0;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		/* Only a connection that speaks IPv6 to the destination. */
		ipv6 = inet_socket_speaks_ipv6(&endpoint->tcp.inet);
		if (!ipv6)
			continue;
		same = in6_equal(&endpoint->tcp.inet.remote6, destination);
		if (!same)
			continue;

		/* Holds the connection still while its segments are cut. */
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

		/* Only a connection whose segments are larger. */
		if (endpoint->tcp.send_mss == 0 || endpoint->tcp.send_mss <= mss) {
			spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
			continue;
		}

		/* The connection's segments are cut to the new size. */
		endpoint->tcp.send_mss = mss;

		/*
		 * The first entry sent that is larger, and every one after it,
		 * unsent again, unless a thread is sending them now (it cuts the
		 * rest; the timer resends the oldest).
		 */
		sent = tcp_sent_count_locked(endpoint);
		for (index = 0; index < sent; index++) {
			slot = (endpoint->tcp.send_first + index) % TCP_SEND_QUEUE_MAX;
			entry = &endpoint->tcp.send_queue[slot];
			if (entry->length > mss)
				break;
		}

		/* Unsends the entries from the first larger one, or has the timer probe with it. */
		if (index < sent && endpoint->tcp.send_flushing == 0) {
			for (slot = index; slot < sent; slot++)
				endpoint->tcp.send_queue[(endpoint->tcp.send_first + slot) % TCP_SEND_QUEUE_MAX].rewound = 1;
			endpoint->tcp.send_unsent = endpoint->tcp.send_count - index;
			endpoint->tcp.rtt_timing = 0;
			if (index == 0) {
				endpoint->tcp.retransmit_deadline = 0;
				endpoint->tcp.retransmit_count = 0;
				endpoint->tcp.retransmit_since = 0;
				endpoint->tcp.retransmit_refused = 0;
				endpoint->tcp.retransmit_immediate = 0;
				endpoint->tcp.persist = 0;
				endpoint->tcp.persist_shift = 0;
			}

			/* The output retry runs at once, on the next pass. */
			endpoint->tcp.output_retry_on_pass = 1;
			endpoint->tcp.output_retry_deadline = sched_ticks();
			wake = 1;
		} else if (index < sent && index == 0) {
			endpoint->tcp.retransmit_immediate = TCP_IMMEDIATE_PROBE;
			endpoint->tcp.retransmit_deadline = sched_ticks();
			wake = 1;
		}

		/* The window's use is measured afresh in the smaller segments. */
		tcp_cwnd_round_reset_locked(endpoint);

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* The worker sends them. */
	if (wake)
		net_worker_wakeup();
}

/*
 * Retransmits expired segments, abandoning attempts that failed too often.
 *
 * Each pass also sends what the device or the packet pool refused before,
 * once its retry is due (ws220-p003); the network worker runs a pass after
 * each device's transmit completion as well as at its deadlines.
 */
/*
 * The payload of a segment the timer pass resends or sends late, copied
 * out of the ring so that it is sent with the socket lock released.  The
 * network worker is the one thread that runs the pass, and a jumbo
 * segment's payload is too large for its stack.
 */
static uint8_t tcp_timer_payload[TCP_SEGMENT_MAX_JUMBO];

void
tcp_timer_run(
	void)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *stack_snapshot[TCP_TIMER_STACK_SNAPSHOT];
	struct tcp_endpoint **snapshot;
	struct tcp_endpoint **allocated;
	unsigned registered;
	unsigned capacity;
	unsigned count;
	unsigned index;
	int referenced;
	unsigned long irq;
	uint64_t now;
	struct socket *socket;
	struct tcp_discard discard;
	struct tcp_pending *oldest;
	uint8_t *payload;
	uint8_t flags;
	uint32_t sequence;
	uint32_t skip;
	unsigned rewind;
	size_t length;
	uint64_t delay;
	enum tcp_refusal refusal;
	unsigned sent;
	unsigned serial;
	unsigned exponent;
	unsigned immediate;
	uint64_t rto;
	int retries;
	int still_oldest;
	int given_up;
	int error;
	int failed;
	int probe;
	int persist;
	int reset_peer;
	int before;
	int repeated;
	uint32_t sent_end;
	uint32_t reset_sequence;

	count = 0;
	registered = 0;
	payload = tcp_timer_payload;
	now = sched_ticks();

	/* The acknowledgements delayed since the last pass go first. */
	tcp_ack_flush();

	/* Holds the registry still while its sockets are counted. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* Counts the registered sockets, which sizes the snapshot. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next)
		registered++;

	/* Lets sockets be created and closed again while the snapshot is sized. */
	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/*
	 * Uses the stack snapshot, or an allocated one when there are more
	 * sockets than it holds.  Without memory the stack snapshot serves
	 * the first sockets now and the others on a later pass.
	 */
	snapshot = stack_snapshot;
	capacity = TCP_TIMER_STACK_SNAPSHOT;
	allocated = NULL;
	if (registered > TCP_TIMER_STACK_SNAPSHOT)
		allocated = kern_calloc(registered, sizeof(*allocated));

	/* Switches to the allocated snapshot when there is one. */
	if (allocated != NULL) {
		snapshot = allocated;
		capacity = registered;
	}

	/*
	 * Snapshots the referenced sockets so no registry lock is held during
	 * I/O.  A socket created since the count waits for the next pass.
	 */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		/* Leaves a socket past the snapshot for the next pass. */
		if (count >= capacity)
			break;

		/* Skips a socket whose closing has begun. */
		referenced = socket_tryref(&endpoint->tcp.inet.socket);
		if (!referenced)
			continue;

		/* Keeps the referenced socket in the snapshot. */
		snapshot[count] = endpoint;
		count++;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Handles each socket whose retransmission deadline passed. */
	for (index = 0; index < count; index++) {
		discard.count = 0;
		endpoint = snapshot[index];
		socket = &endpoint->tcp.inet.socket;

		/* Sends the segments and acknowledgements refused before, when their retry is due. */
		tcp_output_retry(endpoint, payload, now);

		/*
		 * A connection whose data has not moved for its TCP_MAXUNACKTIME
		 * is dropped with a reset (ws220-p005), whatever its timers.
		 */
		irq = spin_lock_irqsave(&socket->lock);
		reset_peer = tcp_maxunack_expired_locked(endpoint, now);

		/*
		 * Only a segment handed to the device has a retransmission
		 * deadline; the unsent ones wait for the output retry.
		 */
		sent = tcp_sent_count_locked(endpoint);
		if (!reset_peer &&
		    (sent == 0 ||
		     endpoint->tcp.retransmit_deadline > now)) {
			/*
			 * With nothing to retransmit, an armed idle probe is
			 * the other reason this socket may need attention.
			 */
			probe = 0;
			if (endpoint->tcp.send_count == 0 &&
			    endpoint->tcp.keepalive_deadline != 0 &&
			    endpoint->tcp.keepalive_deadline <= now)
				probe = tcp_keepalive_expire_locked(endpoint, now);
			spin_unlock_irqrestore(&socket->lock, irq);

			/* A peer that answered no probe at all is gone. */
			if (probe < 0) {
				tcp_forget_peer(endpoint);
				socket_set_error(socket, ETIMEDOUT);
				socket_wake_connect(socket);
				socket_wake_receive(socket);
				socket_release(socket);
				continue;
			}

			/*
			 * The probe is an acknowledgement for the last byte
			 * already sent, which the peer answers without
			 * delivering anything to its reader.
			 */
			if (probe > 0)
				(void)tcp_send_segment_at(endpoint,
				    endpoint->tcp.send_next - 1U, TCP_ACK,
				    NULL, 0);
			socket_release(socket);
			continue;
		}

		/*
		 * A window too small for the oldest segment is probed by the
		 * persist timer, apart from the retransmission backoff, and
		 * given up only by its own rules (ws220-p005); otherwise a
		 * segment resent for R2 without an answer from the peer abandons
		 * the attempt (RFC 1122 section 4.2.3.5).  Entering or leaving
		 * the persist state starts its backoff over.
		 */
		persist = 0;
		given_up = 0;
		if (!reset_peer)
			persist = tcp_persist_applies_locked(endpoint);
		if (reset_peer) {
			given_up = 1;
		} else if (persist) {
			if (endpoint->tcp.persist == 0) {
				endpoint->tcp.persist = 1;
				endpoint->tcp.persist_shift = 0;
			}

			/* The persist timer's own rules (FreeBSD's). */
			given_up = tcp_persist_given_up_locked(endpoint, now);
		} else {
			tcp_persist_leave_locked(endpoint);
			given_up = tcp_retransmit_given_up_locked(endpoint, now);
		}

		/*
		 * A connection dropped for TCP_MAXUNACKTIME tells its peer with a
		 * reset, before it forgets the peer.  Its sequence number is
		 * SND.UNA, which a peer that has acknowledged all it took expects
		 * (RFC 5961 section 3.2 takes a reset only there); a peer that
		 * challenges it is answered by the reset of a connection gone.
		 */
		if (reset_peer) {
			reset_sequence = endpoint->tcp.send_unacknowledged;
			spin_unlock_irqrestore(&socket->lock, irq);
			(void)tcp_send_segment_at(endpoint, reset_sequence, TCP_RST | TCP_ACK, NULL, 0);
			irq = spin_lock_irqsave(&socket->lock);
		}

		/* A connection given up or dropped is closed, and those waiting on it hear why. */
		if (given_up) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.state = TCP_CLOSED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_forget_peer(endpoint);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&discard);
			socket_set_error(socket, ETIMEDOUT);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				/* Queue ownership. */
				socket_release(socket);
				/* Timer snapshot ownership. */
				socket_release(socket);
				continue;
			}

			socket_wake_connect(socket);
			socket_wake_receive(socket);
			socket_release(socket);
			continue;
		}

		/*
		 * The oldest outstanding segment is the one a timeout
		 * resends; anything after it follows once the peer
		 * acknowledges this one.  It is copied out so the send can
		 * happen with the lock released.
		 */
		oldest = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
		sequence = oldest->sequence;
		flags = oldest->flags;
		length = oldest->length;
		if (length > sizeof(tcp_timer_payload)) {
			tcp_retransmit_reset(endpoint, &discard);
			endpoint->tcp.state = TCP_CLOSED;
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_forget_peer(endpoint);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&discard);
			socket_set_error(socket, EIO);
			socket_release(socket);
			continue;
		}

		/*
		 * Its payload from what the peer has not acknowledged: a part
		 * of it may be, when it went cut (tcp_send_segment_cut).
		 */
		skip = 0;
		before = tcp_sequence_before(sequence, endpoint->tcp.send_unacknowledged);
		if (length != 0 && before) {
			skip = endpoint->tcp.send_unacknowledged - sequence;
			if (skip >= length)
				skip = 0;
		}
		if (length != 0)
			kern_memcpy(payload, oldest->packet->storage + oldest->payload_offset + skip, length - skip);

		/*
		 * Counts the resend of the oldest segment, and backs the timeout
		 * off exponentially, up to the longest, when it really expired:
		 * an immediate resend (recovery, a reopened window) does not
		 * double it (ws220-p004 review-1 G1).
		 */
		endpoint->tcp.retransmit_count++;
		endpoint->tcp.retransmit_serial++;
		serial = endpoint->tcp.retransmit_serial;
		immediate = endpoint->tcp.retransmit_immediate;
		endpoint->tcp.retransmit_immediate = 0;
		if (!immediate && !persist)
			endpoint->tcp.rto_backoff++;
		exponent = endpoint->tcp.rto_backoff;

		/*
		 * A timeout that really expired starts a recovery up to the most
		 * ever sent (RFC 6582 section 4, BSD's snd_max), whose duplicates start no
		 * fast retransmit, and goes back to the oldest segment (RFC 5681
		 * section 3.1, as BSD sets snd_nxt back to snd_una): the timer
		 * resends it, every segment sent after it is unsent again, and
		 * they go again in order as the congestion window, one segment
		 * now, grows in slow start with each acknowledgement.  A burst
		 * that lost a whole flight then costs one timeout and a few
		 * round trips, not a round trip for each segment.  The resends
		 * are not timed (Karn); the first new segment's sample ends the
		 * backoff.  Not while a thread is sending the unsent entries: the
		 * segments after the oldest then wait for their partial
		 * acknowledgements as before.
		 */
		if (!immediate && !persist) {
			/*
			 * The SACK marks go (RFC 6675 section 5.1): after a
			 * timeout what the peer holds is learnt again.
			 */
			tcp_sack_clear_locked(endpoint, 1);

			/* Repeated within the recovery an earlier timeout started, or the first. */
			repeated = 0;
			if (endpoint->tcp.recovery_by_timeout != 0)
				repeated = 1;
			tcp_cwnd_loss_locked(endpoint, 1, repeated);
			endpoint->tcp.recover = endpoint->tcp.send_max;
			sent_end = tcp_sent_end_locked(endpoint);
			before = tcp_sequence_before(endpoint->tcp.recover, sent_end);
			if (before)
				endpoint->tcp.recover = sent_end;
			endpoint->tcp.recovery = 1;
			endpoint->tcp.recovery_by_timeout = 0;
			endpoint->tcp.duplicate_acks = 0;
			endpoint->tcp.dupack_guard = 0;
			sent = tcp_sent_count_locked(endpoint);
			if (endpoint->tcp.send_flushing == 0) {
				endpoint->tcp.recovery_by_timeout = 1;
				for (rewind = 1; rewind < sent; rewind++)
					endpoint->tcp.send_queue[(endpoint->tcp.send_first + rewind) % TCP_SEND_QUEUE_MAX].rewound = 1;
				endpoint->tcp.send_unsent = endpoint->tcp.send_count - 1U;
			} else {
				endpoint->tcp.recovery_by_timeout = 2;
			}

			/* The window's use is measured afresh from the one segment it now lets go. */
			tcp_cwnd_round_reset_locked(endpoint);
		}

		/*
		 * The next deadline: the timeout doubled as many times as it is
		 * backed off, or in the persist state the next probe's interval,
		 * whose step then moves on (up to the last).
		 */
		rto = tcp_rto_locked(endpoint);
		delay = tcp_backoff_timeout(rto, exponent);
		if (persist) {
			delay = tcp_persist_interval_locked(endpoint);
			if (endpoint->tcp.persist_shift < TCP_PERSIST_SHIFT_MAX)
				endpoint->tcp.persist_shift++;
		}

		/* The deadline itself. */
		endpoint->tcp.retransmit_deadline = now + delay;

		/*
		 * Karn's rule (RFC 6298 section 3): an acknowledgement of a
		 * segment resent may answer either sending, so the segment
		 * resent is timed no longer.  A timeout that really expired stops
		 * any timing, as BSD does; an immediate resend (recovery, a
		 * reopened window) stops it only when it resends the segment
		 * timed, so a later segment's sample still ends the backoff
		 * (ws220-p004 review-1 G1: reopened windows came every round trip
		 * from a peer like this one).  Since ws220-p005 the immediate
		 * resends of a loss (a fast retransmit, a partial acknowledgement)
		 * stop any timing too (p005 review-1 M2: the segment timed behind
		 * a hole would give a sample of the recovery, not of the round
		 * trip); a probe's stops only its own.  A SYN resent
		 * gives the handshake no sample, and the data three seconds
		 * (section 5.7).
		 */
		if (immediate != TCP_IMMEDIATE_PROBE) {
			endpoint->tcp.rtt_timing = 0;
		} else if (endpoint->tcp.rtt_timing != 0 && endpoint->tcp.rtt_sequence == sequence + oldest->advance) {
			endpoint->tcp.rtt_timing = 0;
		}

		/* A SYN (or SYN-ACK) resent. */
		if (endpoint->tcp.state == TCP_SYN_SENT || endpoint->tcp.state == TCP_SYN_RECEIVED)
			endpoint->tcp.syn_timed_out = 1;

		/*
		 * A resend into a window that cannot take it probes the window
		 * (RFC 9293 section 3.8.6.1, ws220-p005): it is sent again at
		 * once when the window opens, rather than at the backed-off
		 * timeout.
		 */
		oldest->probe = 0;
		if (length != 0 &&
		    sequence + oldest->advance - endpoint->tcp.send_unacknowledged > (uint32_t)endpoint->tcp.peer_window)
			oldest->probe = 1;

		/* Sent again for a loss within this recovery: the SACK resends pass it by. */
		if (immediate == TCP_IMMEDIATE_LOSS) {
			oldest->resent = 1;
			oldest->resend_order = endpoint->tcp.resend_serial++;
		}

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Resends the oldest segment, cut to the segment size. */
		error = tcp_send_segment_cut(endpoint, sequence + skip, flags, payload, length - skip);
		refusal = tcp_refusal_of(error);

		/*
		 * A resend the device took is counted for net.tcpstats.  One the
		 * device or the pool refused did not happen: the backoff is taken
		 * back and the resend tried again soon (ws220-p003), rather than
		 * after the doubled timeout; soon is TCP_SEND_RETRY_MS once it has
		 * been refused before (review-1 M2), so a device that never
		 * completes is not tried every tick.  One lost for an unresolved
		 * neighbour keeps the backoff, as one lost on the wire.
		 */
		irq = spin_lock_irqsave(&socket->lock);

		if (error == 0) {
			endpoint->tcp.counters.retransmit_segments++;
			endpoint->tcp.counters.retransmit_bytes += length - skip;
		}

		/* A refused resend of the segment still oldest, whose backoff no one has moved since. */
		sent = tcp_sent_count_locked(endpoint);
		retries = tcp_refusal_retries(refusal);
		still_oldest = 0;
		if (retries &&
		    sent != 0 &&
		    endpoint->tcp.send_queue[endpoint->tcp.send_first].sequence == sequence &&
		    endpoint->tcp.retransmit_serial == serial)
			still_oldest = 1;
		if (still_oldest) {
			delay = tcp_refusal_delay(refusal);
			if (endpoint->tcp.retransmit_refused != 0)
				delay = tcp_refusal_delay(TCP_REFUSAL_WAIT);
			if (endpoint->tcp.retransmit_count != 0)
				endpoint->tcp.retransmit_count--;
			endpoint->tcp.retransmit_deadline = now + delay;

			/* The doubling taken back too, unless a sample has set it again meanwhile; an immediate one stays so. */
			if (!immediate && !persist && endpoint->tcp.rto_backoff == exponent)
				endpoint->tcp.rto_backoff--;
			if (persist && endpoint->tcp.persist_shift != 0)
				endpoint->tcp.persist_shift--;
			endpoint->tcp.retransmit_immediate = immediate;

			/* The next refused resend knows this one was refused too. */
			endpoint->tcp.retransmit_refused = 1;
		}

		/* A resend that went, or was lost as on the wire, ends the run of refusals. */
		if (!retries)
			endpoint->tcp.retransmit_refused = 0;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* A hard send error abandons the attempt the segment belonged to. */
		if (refusal == TCP_REFUSAL_HARD) {
			failed = 0;
			irq = spin_lock_irqsave(&socket->lock);
			sent = tcp_sent_count_locked(endpoint);
			if (sent != 0 &&
			    endpoint->tcp.send_queue[endpoint->tcp.send_first]
				.sequence == sequence) {
				tcp_retransmit_reset(endpoint, &discard);
				endpoint->tcp.state = TCP_CLOSED;
				endpoint->tcp.active_connect_generation = 0;
				endpoint->tcp.connect_wait_deadline = 0;
				tcp_forget_peer(endpoint);
				failed = 1;
			}

			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&discard);
			if (!failed) {
				socket_release(socket);
				continue;
			}

			socket_set_error(socket, error);
			if (endpoint->tcp.listener != NULL) {
				tcp_listener_remove(endpoint);
				/* Queue ownership. */
				socket_release(socket);
				/* Timer snapshot ownership. */
				socket_release(socket);
				continue;
			}

			socket_wake_connect(socket);
			socket_release(socket);
			continue;
		}

		socket_release(socket);
	}

	/* Frees the allocated snapshot; the references were all released above. */
	if (allocated != NULL)
		kern_free(allocated);
}

/*
 * Reports the earliest retransmission deadline, or zero without one.
 *
 * A socket's deadline is its oldest sent segment's retransmission, or with
 * nothing queued its idle probe, and its output retry when that comes
 * first (ws220-p003): unsent segments and pending acknowledgements have no
 * retransmission deadline of their own.
 */
uint64_t
tcp_timer_next_deadline(
	void)
{
	struct tcp_endpoint *endpoint;
	uint64_t deadline;
	unsigned long irq;
	uint64_t candidate;
	uint64_t retry;
	uint64_t unacked;
	unsigned long socket_irq;
	unsigned sent;

	deadline = 0;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* Takes the minimum over every socket with a segment outstanding. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);

		/* The oldest sent segment's retransmission, or with nothing queued the idle probe. */
		sent = tcp_sent_count_locked(endpoint);
		candidate = 0;
		if (sent != 0)
			candidate = endpoint->tcp.retransmit_deadline;
		else if (endpoint->tcp.send_count == 0)
			candidate = endpoint->tcp.keepalive_deadline;

		/*
		 * The output retry's deadline, unless a thread is sending the
		 * unsent entries: the retry is not due then, and a deadline
		 * already passed would keep the worker from sleeping until that
		 * thread is done.  It wakes the worker when it is.
		 */
		retry = 0;
		if (endpoint->tcp.send_flushing == 0)
			retry = endpoint->tcp.output_retry_deadline;

		/* The TCP_MAXUNACKTIME limit, when it comes first (ws220-p005). */
		unacked = 0;
		if (endpoint->tcp.maxunack != 0 && endpoint->tcp.unack_since != 0)
			unacked = endpoint->tcp.unack_since + endpoint->tcp.maxunack;
		if (unacked != 0 && (candidate == 0 || unacked < candidate))
			candidate = unacked;

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/* The output retry, when it is due before the socket's other deadline. */
		if (retry != 0 && (candidate == 0 || retry < candidate))
			candidate = retry;

		/* The earliest so far. */
		if (candidate != 0 && (deadline == 0 || candidate < deadline))
			deadline = candidate;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the earliest deadline. */
	return deadline;
}

/*
 * Copies net.tcpstats: the header and each registered TCP socket.
 *
 * The registry lock is held across the count and the copy, so the entries
 * written are the ones counted; each socket is read under its own lock,
 * taken after the registry's as the timer takes them.  Without an output,
 * or with one shorter than needed (ENOMEM), only the length is reported.
 * A reader that is not the superuser is given every socket without its
 * sequence numbers.
 */
int
tcp_stats_copy(
	void *output,
	size_t capacity,
	size_t *needed,
	int superuser)
{
	struct tcp_stats_header header;
	struct tcp_stats_entry entry;
	struct packet_buf_pool_usage usage;
	struct tcp_endpoint *endpoint;
	uint8_t *place;
	unsigned long irq;
	unsigned long socket_irq;
	uint32_t count;
	uint64_t now;
	int error;

	/* The moment of the reading and the packet pool's use at it. */
	now = sched_ticks();
	packet_buf_pool_usage(&usage);

	/* The sockets, counted and copied with the registry held still. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* Counts them, which sizes the value. */
	count = 0;
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next)
		count++;

	/* The length, and nothing written without a buffer or with one too small. */
	*needed = sizeof(header) + (size_t)count * sizeof(entry);
	error = 0;
	if (output != NULL && capacity < *needed)
		error = ENOMEM;
	if (output == NULL || error != 0) {
		spin_unlock_irqrestore(&tcp_registry_lock, irq);
		return error;
	}

	/* The header. */
	kern_memset(&header, 0, sizeof(header));
	header.version = TCP_STATS_VERSION;
	header.struct_size = sizeof(header);
	header.element_size = sizeof(entry);
	header.count = count;
	header.packets_total = usage.total;
	header.packets_used = usage.used;
	header.packets_used_max = usage.used_max;
	header.packet_alloc_failures = usage.alloc_failures;
	header.time_ms = kern_ticks_to_ms(now);
	header.jumbo_total = usage.jumbo_total;
	header.jumbo_used = usage.jumbo_used;
	header.jumbo_used_max = usage.jumbo_used_max;
	header.jumbo_reserve = usage.jumbo_reserve;
	header.jumbo_alloc_failures = usage.jumbo_alloc_failures;
	place = output;
	kern_memcpy(place, &header, sizeof(header));
	place += sizeof(header);

	/* Each socket after it, as one moment of its own state. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		tcp_stats_fill_locked(endpoint, now, superuser, &entry);
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		kern_memcpy(place, &entry, sizeof(entry));
		place += sizeof(entry);
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Succeeded: the header and every socket are in the output. */
	return 0;
}

/*
 * Gives the name of a congestion control (TCP_CC_*), as TCP_CONGESTION and
 * net.tcp.congestion show it.
 */
const char *
tcp_cc_name(
	unsigned cc)
{
	/* A number this kernel does not have is shown as the first. */
	if (cc >= TCP_CC_COUNT)
		return tcp_cc_names[TCP_CC_RENO];

	/* Succeeded: its name. */
	return tcp_cc_names[cc];
}

/*
 * Finds the congestion control a name of length bytes names, as FreeBSD
 * reads TCP_CONGESTION: up to the first NUL, no more than TCP_CA_NAME_MAX
 * - 1 bytes of it (a longer one is cut there).  An empty name fails with
 * EINVAL, one this kernel does not have with ESRCH.
 */
int
tcp_cc_lookup(
	const char *name,
	size_t length,
	unsigned *cc)
{
	const char *known;
	size_t used;
	size_t index;
	unsigned candidate;
	int same;
	int error;

	/* Counts the bytes before a NUL, within the longest name. */
	used = 0;
	while (used < length &&
	       used < TCP_CA_NAME_MAX - 1U &&
	       name[used] != '\0')
		used++;

	/* An empty name names nothing. */
	if (used == 0)
		return EINVAL;

	/* Looks for the congestion control whose name is the same bytes and no more. */
	error = ESRCH;
	for (candidate = 0;
	     candidate < TCP_CC_COUNT;
	     candidate++) {
		known = tcp_cc_names[candidate];

		/* Compares the bytes given with this name's. */
		same = 1;
		for (index = 0; index < used; index++) {
			/* A byte that differs makes it another name. */
			if (known[index] != name[index]) {
				same = 0;
				break;
			}
		}

		/* The whole name matches only when this one ends where the given one does. */
		if (same && known[used] == '\0') {
			*cc = candidate;
			error = 0;
			break;
		}
	}

	/* Reports that no congestion control has the name. */
	if (error != 0)
		return error;

	/* Succeeded: the congestion control is found. */
	return 0;
}

/* Gives the congestion control a new socket starts with (TCP_CC_*). */
unsigned
tcp_cc_default(
	void)
{
	unsigned cc;

	/* The word net.tcp.congestion last set. */
	cc = __atomic_load_n(&tcp_cc_default_value, __ATOMIC_RELAXED);
	return cc;
}

/* Sets the congestion control a new socket starts with (TCP_CC_*, one tcp_cc_lookup found). */
void
tcp_cc_set_default(
	unsigned cc)
{
	/* Only a number this kernel has. */
	if (cc >= TCP_CC_COUNT)
		return;

	/* The word new sockets read. */
	__atomic_store_n(&tcp_cc_default_value, cc, __ATOMIC_RELAXED);
}

/*
 * Arms or disarms the idle probe for one socket.
 *
 * The caller holds the socket lock.  Only an established connection probes:
 * there is nothing to keep alive before it exists or after it is gone.
 */
static void
tcp_keepalive_arm_locked(
	struct tcp_endpoint *endpoint)
{
	endpoint->tcp.keepalive_probes = 0;

	/* Disarms when the option is off or the connection is not up. */
	if (endpoint->tcp.inet.socket.keepalive == 0 ||
	    endpoint->tcp.state != TCP_ESTABLISHED) {
		endpoint->tcp.keepalive_deadline = 0;
		return;
	}

	/* Restarts the idle period. */
	endpoint->tcp.keepalive_deadline = sched_ticks() + TCP_KEEPALIVE_IDLE;
}

/*
 * Reacts to SO_KEEPALIVE being set or cleared on an existing socket.
 */
static void
tcp_keepalive_changed(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return;
	irq = spin_lock_irqsave(&socket->lock);
	tcp_keepalive_arm_locked(endpoint);
	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Sets one option at the TCP protocol level.
 */
static int
tcp_setsockopt(
	struct socket *socket,
	int level,
	int option,
	const void *value,
	socklen_t length)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;
	unsigned cc;
	int enabled;
	int error;
	int wake;

	/* IPv6's options (IPV6_V6ONLY) are the internet socket layer's (ws130-p003). */
	if (socket != NULL && level == IPPROTO_IPV6) {
		endpoint = tcp_endpoint(socket);
		error = inet_socket_setsockopt(&endpoint->tcp.inet, level, option, value, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Only this protocol's own level is handled here. */
	if (socket == NULL || level != IPPROTO_TCP)
		return ENOPROTOOPT;

	/*
	 * TCP_CONGESTION: a name, which takes effect at once in any state;
	 * the window and the threshold stay, the new control's measures start
	 * afresh.
	 */
	if (option == TCP_CONGESTION) {
		/* Only a TCP socket has a congestion control. */
		endpoint = tcp_endpoint(socket);
		if (endpoint == NULL)
			return ENOPROTOOPT;

		/* A name has to be given. */
		if (value == NULL)
			return EINVAL;

		/* Finds the congestion control the name names. */
		error = tcp_cc_lookup(value, length, &cc);
		if (error != 0)
			return error;

		/* Has the connection use it. */
		irq = spin_lock_irqsave(&socket->lock);

		tcp_cc_set_locked(endpoint, cc);

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Succeeded: the connection uses it. */
		return 0;
	}

	/* The other options are an int. */
	if (option != TCP_NODELAY && option != TCP_MAXUNACKTIME)
		return ENOPROTOOPT;
	if (value == NULL || length != sizeof(enabled))
		return EINVAL;
	kern_memcpy(&enabled, value, sizeof(enabled));

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return ENOPROTOOPT;

	/*
	 * TCP_MAXUNACKTIME: the seconds a connection's data may go without
	 * progress before it is dropped with a reset, 0 for never (ws220-p005).
	 */
	if (option == TCP_MAXUNACKTIME) {
		if (enabled < 0)
			return EINVAL;
		irq = spin_lock_irqsave(&socket->lock);

		endpoint->tcp.maxunack_seconds = (unsigned)enabled;
		endpoint->tcp.maxunack = KERN_MS_TO_TICKS((uint64_t)(unsigned)enabled * 1000U);

		spin_unlock_irqrestore(&socket->lock, irq);

		/* The worker learns of the new deadline. */
		net_worker_wakeup();
		return 0;
	}

	/*
	 * TCP_NODELAY: set, Nagle's algorithm holds no short segment back, and
	 * one it holds now has the network worker send it.
	 */
	irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.nodelay = enabled != 0;
	wake = 0;
	if (endpoint->tcp.nodelay != 0 && endpoint->tcp.send_unsent != 0) {
		endpoint->tcp.output_retry_on_pass = 1;
		if (endpoint->tcp.output_retry_deadline == 0)
			endpoint->tcp.output_retry_deadline = sched_ticks();
		wake = 1;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/* The worker learns of the segment it sends. */
	if (wake)
		net_worker_wakeup();

	/* Reports successful completion. */
	return 0;
}

/*
 * Reads one option at the TCP protocol level.
 */
static int
tcp_getsockopt(
	struct socket *socket,
	int level,
	int option,
	void *value,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	const char *name;
	unsigned long irq;
	unsigned cc;
	size_t name_length;
	int enabled;
	int error;

	/* IPv6's options (IPV6_V6ONLY) are the internet socket layer's (ws130-p003). */
	if (socket != NULL && level == IPPROTO_IPV6) {
		endpoint = tcp_endpoint(socket);
		error = inet_socket_getsockopt(&endpoint->tcp.inet, level, option, value, length);
		if (error != 0)
			return error;
		return 0;
	}

	/* Only this protocol's own level is handled here. */
	if (socket == NULL || level != IPPROTO_TCP)
		return ENOPROTOOPT;

	/* TCP_CONGESTION: the name and its NUL, cut to the length given (as FreeBSD gives it). */
	if (option == TCP_CONGESTION) {
		/* Only a TCP socket has a congestion control. */
		endpoint = tcp_endpoint(socket);
		if (endpoint == NULL)
			return ENOPROTOOPT;

		/* A place to put the name has to be given. */
		if (value == NULL || length == NULL)
			return EINVAL;

		/* Samples the congestion control the connection uses. */
		irq = spin_lock_irqsave(&socket->lock);

		cc = endpoint->tcp.cc;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Copies its name, no more of it than the length given. */
		name = tcp_cc_name(cc);
		name_length = kern_strlen(name) + 1U;
		if (name_length > *length)
			name_length = *length;
		kern_memcpy(value, name, name_length);
		*length = (socklen_t)name_length;

		/* Succeeded: the name is given. */
		return 0;
	}

	/* The other options are an int. */
	if (option != TCP_NODELAY && option != TCP_MAXUNACKTIME)
		return ENOPROTOOPT;
	if (value == NULL || length == NULL || *length < sizeof(enabled))
		return EINVAL;

	endpoint = tcp_endpoint(socket);

	/* Handles a socket that is not a TCP endpoint. */
	if (endpoint == NULL)
		return ENOPROTOOPT;

	/* The option's value: TCP_NODELAY as set, or TCP_MAXUNACKTIME in seconds. */
	irq = spin_lock_irqsave(&socket->lock);

	if (option == TCP_MAXUNACKTIME) {
		enabled = (int)endpoint->tcp.maxunack_seconds;
	} else {
		enabled = endpoint->tcp.nodelay != 0;
	}

	spin_unlock_irqrestore(&socket->lock, irq);
	kern_memcpy(value, &enabled, sizeof(enabled));
	*length = sizeof(enabled);

	/* Reports successful completion. */
	return 0;
}

/* Converts a socket to its endpoint. */
static struct tcp_endpoint *
tcp_endpoint(
	struct socket *socket)
{
	return (struct tcp_endpoint *)socket;
}

/* Clears the peer address of an endpoint. */
static void
tcp_forget_peer(
	struct tcp_endpoint *endpoint)
{
	endpoint->tcp.inet.remote_address = 0;
	kern_memset(&endpoint->tcp.inet.remote6, 0, sizeof(endpoint->tcp.inet.remote6));
	endpoint->tcp.inet.scope6 = 0;
	endpoint->tcp.inet.remote_port = 0;
	endpoint->tcp.inet.inet_flags &= ~INET_SOCKET_CONNECTED;
}

/* Removes a child from its listener's half-open or accept queue. */
static void
tcp_listener_remove(
	struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link;
	struct tcp_socket *previous;
	unsigned long irq;

	previous = NULL;

	/* Ignores a child without a listener. */
	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	/* Searches the half-open queue first. */
	for (link = &listener->tcp.half_open_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			child->tcp.queue_next = NULL;
			child->tcp.listener = NULL;
			spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
			return;
		}
	}

	/* Then the accept queue, fixing its tail. */
	for (link = &listener->tcp.accept_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.accept_tail == &child->tcp)
				listener->tcp.accept_tail = previous;
			if (listener->tcp.accept_count != 0)
				listener->tcp.accept_count--;
			child->tcp.queue_next = NULL;
			child->tcp.listener = NULL;
			spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
			return;
		}

		previous = *link;
	}

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
}

/* Moves a child whose handshake completed to its listener's accept queue. */
static void
tcp_listener_established(
	struct tcp_endpoint *child)
{
	struct tcp_endpoint *listener;
	struct tcp_socket **link;
	unsigned long irq;

	/* Ignores a child without a listener. */
	if (child == NULL || child->tcp.listener == NULL)
		return;
	listener = (struct tcp_endpoint *)child->tcp.listener;
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	/* Unlinks the child from the half-open queue. */
	for (link = &listener->tcp.half_open_head; *link != NULL;
	     link = &(*link)->queue_next) {
		if (*link == &child->tcp) {
			*link = child->tcp.queue_next;
			if (listener->tcp.half_open_count != 0)
				listener->tcp.half_open_count--;
			break;
		}
	}

	/* Appends it to the accept queue and wakes the acceptors. */
	child->tcp.queue_next = NULL;
	if (listener->tcp.accept_tail != NULL)
		listener->tcp.accept_tail->queue_next = &child->tcp;
	else
		listener->tcp.accept_head = &child->tcp;
	listener->tcp.accept_tail = &child->tcp;
	listener->tcp.accept_count++;
	waitq_wake_all(&listener->tcp.inet.socket.accept_waitq);
	poll_notify();

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
}

/* Tests whether another socket conflicts with a candidate's local address. */
static int
tcp_port_in_use(
	const struct tcp_endpoint *candidate,
	int strict)
{
	const struct tcp_endpoint *endpoint;
	int conflict;

	/* A strict check ignores address reuse. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		if (endpoint == candidate)
			continue;
		if (strict)
			conflict = inet_socket_local_conflict(&endpoint->tcp.inet, 0,
			    &candidate->tcp.inet, 0);
		else
			conflict = inet_socket_local_conflict(&endpoint->tcp.inet,
			    endpoint->tcp.inet.bind_reuse_address,
			    &candidate->tcp.inet,
			    candidate->tcp.inet.bind_reuse_address);
		if (conflict)
			return 1;
	}

	return 0;
}

/* Assigns a free ephemeral port; the caller holds the registry lock. */
static int
tcp_allocate_port_locked(
	struct tcp_endpoint *endpoint)
{
	unsigned attempts;
	uint16_t port;

	/* Tries successive ephemeral ports, wrapping within the range. */
	for (attempts = 0; attempts < 16384U; attempts++) {
		port = next_ephemeral;
		next_ephemeral++;
		if (next_ephemeral < TCP_EPHEMERAL_FIRST)
			next_ephemeral = TCP_EPHEMERAL_FIRST;
		endpoint->tcp.inet.local_port = port;
		if (!tcp_port_in_use(endpoint, 1)) {
			endpoint->tcp.inet.inet_flags |= INET_SOCKET_BOUND;
			return 0;
		}

		endpoint->tcp.inet.local_port = 0;
	}

	return EADDRINUSE;
}

/* Finds the output device and source address for an endpoint's peer. */
static int
tcp_route(
	struct tcp_endpoint *endpoint,
	struct net_device **device,
	uint32_t *source)
{
	struct net_route route;
	struct net_device *output;
	int have_route;
	int lookup;
	int error;

	/*
	 * A bound interface wins over the route's device, so the route is
	 * looked up only without one: a connection remembers the interface of
	 * its first segment, and every later segment goes by it.
	 */
	have_route = 0;
	if (endpoint->tcp.inet.ifindex == 0) {
		lookup = route_lookup_ref(endpoint->tcp.inet.remote_address, &route);
		if (lookup == 0)
			have_route = 1;
	}
	if (endpoint->tcp.inet.ifindex != 0)
		output = net_device_find_by_index_ref(endpoint->tcp.inet.ifindex);
	else if (have_route)
		output = route.device;
	else
		output = NULL;
	if (endpoint->tcp.inet.ifindex == 0 && output != NULL)
		route.device = NULL;
	if (have_route)
		route_release(&route);
	if (output == NULL)
		return ENETUNREACH;

	/* Takes the device's address as the source and remembers the interface. */
	error = inet_interface_address(output, source, NULL, NULL);
	if (error != 0) {
		net_device_release(output);
		return error;
	}

	endpoint->tcp.inet.ifindex = output->ifindex;
	if (endpoint->tcp.inet.local_address == 0)
		endpoint->tcp.inet.local_address = *source;
	*device = output;
	return 0;
}

/*
 * Takes the way the connection's last lookup found, when nothing has
 * changed since: the bound interface (with a reference), the source and
 * the next hop.  Reports 0 when there is none to take, and the caller
 * looks it up.
 */
static int
tcp_route_cached(
	struct tcp_endpoint *endpoint,
	struct net_device **device,
	uint32_t *source,
	uint32_t *next_hop)
{
	struct net_device *output;
	unsigned long irq;
	unsigned ifindex;
	int valid;

	/* What was kept, while the table and the addresses are as they were. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	valid = endpoint->tcp.route_cached != 0 &&
	    endpoint->tcp.route_generation_seen == route_generation();
	*source = endpoint->tcp.route_source;
	*next_hop = endpoint->tcp.route_next_hop;
	ifindex = endpoint->tcp.inet.ifindex;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* Nothing kept, or something changed. */
	if (!valid || ifindex == 0)
		return 0;

	/* The interface, when it is still there. */
	output = net_device_find_by_index_ref(ifindex);
	if (output == NULL)
		return 0;

	/* Succeeded: the way the segment goes. */
	*device = output;
	return 1;
}

/*
 * Keeps the way a lookup found for the connection's later segments: the
 * source and the route's next hop through the device, with the count of
 * changes read before the lookup (generation).  A peer that is one of
 * this host's own addresses, or one no route reaches through the device,
 * is not kept.
 */
static void
tcp_route_remember(
	struct tcp_endpoint *endpoint,
	struct net_device *device,
	uint32_t source,
	unsigned long generation)
{
	struct net_route route;
	unsigned long irq;
	uint32_t destination;
	uint32_t next_hop;
	int local;
	int error;

	/* A peer of this host's own goes round the loopback device: nothing kept. */
	destination = endpoint->tcp.inet.remote_address;
	local = inet_address_is_local(destination);
	if (local)
		return;

	/* The next hop: the route's gateway, else the peer itself. */
	error = route_lookup_device_ref(destination, device, &route);
	if (error != 0)
		return;
	next_hop = destination;
	if (route.gateway != 0)
		next_hop = route.gateway;
	route_release(&route);

	/* Kept for the segments after, with the count it holds for. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	endpoint->tcp.route_cached = 1;
	endpoint->tcp.route_generation_seen = generation;
	endpoint->tcp.route_source = source;
	endpoint->tcp.route_next_hop = next_hop;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
}

/*
 * Finds the output device (NULL: the route's) and the source address for
 * an IPv6 endpoint's peer (ws130-p003): the bound interface, or the one a
 * link-local peer is on, and the bound address or the one chosen (RFC 6724),
 * which the endpoint then keeps.
 */
static int
tcp6_route(
	struct tcp_endpoint *endpoint,
	struct net_device **device,
	struct in6_addr *source)
{
	struct net_device *output;
	struct in6_addr chosen;
	unsigned ifindex;
	int linklocal;
	int unspecified;
	int error;

	/* The interface asked for. */
	ifindex = endpoint->tcp.inet.ifindex;
	linklocal = in6_is_linklocal(&endpoint->tcp.inet.remote6);
	if (ifindex == 0 && linklocal)
		ifindex = endpoint->tcp.inet.scope6;
	output = NULL;
	if (ifindex != 0) {
		output = net_device_find_by_index_ref(ifindex);
		if (output == NULL)
			return ENETUNREACH;
	}

	/* The way to the peer, and the source it would take. */
	error = ipv6_route_source(output, &endpoint->tcp.inet.remote6, &chosen, NULL);
	if (error != 0) {
		if (output != NULL)
			net_device_release(output);
		return error;
	}

	/* The endpoint keeps the first source, as its name and its connection's. */
	unspecified = in6_is_unspecified(&endpoint->tcp.inet.local6);
	if (unspecified)
		endpoint->tcp.inet.local6 = chosen;
	*source = endpoint->tcp.inet.local6;
	*device = output;

	/* Succeeded: the device (or NULL) and the source. */
	return 0;
}

/*
 * Takes the way the connection's last IPv6 lookup found, while nothing
 * has changed since: the interface (with a reference), the source, and
 * the next hop, hop limit and MTU.  Reports 0 when there is none to take,
 * and the caller looks it up.
 */
static int
tcp6_route_cached(
	struct tcp_endpoint *endpoint,
	struct net_device **device,
	struct in6_addr *source,
	struct ipv6_way *way)
{
	struct net_device *output;
	unsigned long generation;
	unsigned long irq;
	unsigned ifindex;
	int valid;

	/* What was kept, while the table and the addresses are as they were. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	generation = route_generation();
	valid = 0;
	if (endpoint->tcp.route_cached != 0 && endpoint->tcp.route_generation_seen == generation)
		valid = 1;
	*source = endpoint->tcp.inet.local6;
	way->next_hop = endpoint->tcp.route_next_hop6;
	way->hop_limit = endpoint->tcp.route_hop_limit;
	way->mtu = endpoint->tcp.route_mtu;
	ifindex = endpoint->tcp.route_ifindex;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* Nothing kept, or something changed. */
	if (!valid || ifindex == 0)
		return 0;

	/* The interface, when it is still there. */
	output = net_device_find_by_index_ref(ifindex);
	if (output == NULL)
		return 0;

	/* Succeeded: the way the segment goes. */
	*device = output;
	return 1;
}

/*
 * Keeps the way an IPv6 lookup found for the connection's later
 * segments: the interface the route goes out of, its next hop, hop limit
 * and MTU, with the count of changes read before the lookup (generation).  device (may
 * be NULL) is the interface the lookup was asked for.  A peer that is one
 * of this host's own addresses, or one no route reaches, is not kept.
 */
static void
tcp6_route_remember(
	struct tcp_endpoint *endpoint,
	struct net_device *device,
	unsigned long generation)
{
	struct net_device *output;
	struct ipv6_way way;
	unsigned long irq;
	unsigned ifindex;
	int error;

	/* The way to the peer; the host's own and an unreachable peer give none. */
	error = ipv6_route_way(device, &endpoint->tcp.inet.remote6, &output, &way);
	if (error != 0)
		return;
	ifindex = output->ifindex;
	net_device_release(output);

	/* Kept for the segments after, with the count it holds for. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	endpoint->tcp.route_cached = 1;
	endpoint->tcp.route_generation_seen = generation;
	endpoint->tcp.route_ifindex = ifindex;
	endpoint->tcp.route_next_hop6 = way.next_hop;
	endpoint->tcp.route_hop_limit = way.hop_limit;
	endpoint->tcp.route_mtu = way.mtu;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
}

/*
 * Transmits one segment with an explicit sequence number and counts it.
 *
 * Every segment this end sends goes through here, so the counts of
 * net.tcpstats see each one: sent, or failed on its way to the device (an
 * acknowledgement or a resend that is then lost, as on the wire).  The
 * caller holds no socket lock, which the counting takes.
 */
static int
tcp_send_segment_at(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	const void *data,
	size_t length)
{
	int error;

	/* Sends the segment whatever it tells. */
	error = tcp_send_segment_news(endpoint, sequence, flags, data, length, 0);
	if (error != 0)
		return error;

	/* Succeeded: the segment went. */
	return 0;
}

/*
 * Transmits one segment as tcp_send_segment_at does; with news_only, a pure
 * acknowledgement that would repeat the last segment's acknowledgement and
 * window is not sent (EALREADY) and not counted.
 */
static int
tcp_send_segment_news(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	const void *data,
	size_t length,
	int news_only)
{
	unsigned long irq;
	int error;

	/* Builds and hands the segment to the device. */
	error = tcp_output_segment(endpoint, sequence, flags, data, length, NULL, news_only);

	/* One that would have told nothing new did not go. */
	if (error == EALREADY)
		return error;

	/* Counts the segment and, when it went, a closed window it told of. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	if (error != 0) {
		endpoint->tcp.counters.segment_send_failures++;
	} else {
		endpoint->tcp.counters.segments_out++;
		if (endpoint->tcp.advertised_window == 0)
			endpoint->tcp.counters.zero_windows_out++;

		/* Every segment acknowledges what was received: no acknowledgement is owed any more. */
		endpoint->tcp.ack_owed = 0;
	}

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* Reports why the segment did not go. */
	if (error != 0)
		return error;

	/* Succeeded: the device has the segment. */
	return 0;
}

/*
 * Transmits an entry's payload as segments of the connection's size: one,
 * or several when the size was lowered after it was queued (a Packet Too
 * Big).  Only the last carries the entry's FIN and PSH.  It reports the
 * first segment's error; a later one refused is lost as on the wire, for
 * the acknowledgements and the timer to recover.  The caller holds no
 * socket lock.
 */
static int
tcp_send_segment_cut(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	const void *data,
	size_t length)
{
	const uint8_t *bytes;
	unsigned long irq;
	uint32_t mss;
	size_t offset;
	size_t piece;
	uint8_t piece_flags;
	int error;

	/* The connection's segment size now. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	mss = tcp_segment_size_locked(endpoint);

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* One segment holds it. */
	if (length <= mss)
		return tcp_send_segment_at(endpoint, sequence, flags, data, length);

	/* Several: the first reports, the others go as they can. */
	bytes = data;
	offset = 0;
	error = 0;
	while (offset < length) {
		piece = length - offset;
		piece_flags = flags;
		if (piece > mss) {
			piece = mss;
			piece_flags = (uint8_t)(flags & ~(TCP_FIN | TCP_PSH));
		}

		/* Sends the piece; only the first one's failure is the call's. */
		error = tcp_send_segment_at(endpoint, sequence + (uint32_t)offset, piece_flags, bytes + offset, piece);
		if (error != 0 && offset == 0)
			return error;
		offset += piece;
	}

	/* Succeeded: the first went. */
	return 0;
}

/*
 * Transmits a segment whose payload is a packet of its own, the carrier,
 * whose reference passes to the output, and counts it: a segment's copy
 * in the send ring, which carries its first transmission (the headers go
 * into its headroom, in front of the payload).  The caller holds no
 * socket lock, which the counting takes.
 */
static int
tcp_send_segment_carried(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	struct packet_buf *carrier)
{
	unsigned long irq;
	int error;

	/* Builds the segment around the payload and hands it to the device. */
	error = tcp_output_segment(endpoint, sequence, flags, NULL, 0, carrier, 0);

	/* Counts the segment as tcp_send_segment_at does. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	if (error != 0) {
		endpoint->tcp.counters.segment_send_failures++;
	} else {
		endpoint->tcp.counters.segments_out++;
		if (endpoint->tcp.advertised_window == 0)
			endpoint->tcp.counters.zero_windows_out++;

		/* Every segment acknowledges what was received: no acknowledgement is owed any more. */
		endpoint->tcp.ack_owed = 0;
	}

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* Reports why the segment did not go. */
	if (error != 0)
		return error;

	/* Succeeded: the device has the segment. */
	return 0;
}

/*
 * Fills one net.tcpstats entry from a socket, under its lock.
 *
 * now is the tick of the reading, which the time left of the
 * retransmission deadline is measured from.  The sequence numbers are given
 * to the superuser only (ws220-p002): with them and the addresses, a
 * process could put data or a reset into another user's connection.
 */
static void
tcp_stats_fill_locked(
	const struct tcp_endpoint *endpoint,
	uint64_t now,
	int superuser,
	struct tcp_stats_entry *entry)
{
	const struct tcp_socket *tcp;
	const struct socket *socket;
	const struct tcp_counters *counters;
	uint32_t sent_end;
	unsigned sent;
	uint64_t rto;
	uint64_t timeout;
	int ipv6;

	/* The parts of the endpoint read, and an entry with every field not kept 0. */
	tcp = &endpoint->tcp;
	socket = &tcp->inet.socket;
	counters = &tcp->counters;
	kern_memset(entry, 0, sizeof(*entry));

	/* What has been handed to the device, which unsent segments are not part of (ws220-p003). */
	sent_end = tcp_sent_end_locked(endpoint);
	sent = tcp_sent_count_locked(endpoint);

	/* The addresses: IPv6 for a socket that speaks it, else IPv4 in network order. */
	ipv6 = inet_socket_speaks_ipv6(&tcp->inet);
	if (ipv6) {
		entry->family = TCP_STATS_FAMILY_INET6;
		kern_memcpy(entry->local_address, tcp->inet.local6.s6_addr, 16U);
		kern_memcpy(entry->remote_address, tcp->inet.remote6.s6_addr, 16U);
	} else {
		entry->family = TCP_STATS_FAMILY_INET;
		wire_put32(entry->local_address, tcp->inet.local_address);
		wire_put32(entry->remote_address, tcp->inet.remote_address);
	}

	/* The state, the ports, and the parts of TCP this stack keeps (none yet beyond the base). */
	entry->state = (uint32_t)tcp->state;
	entry->features = 0;
	entry->local_port = tcp->inet.local_port;
	entry->remote_port = tcp->inet.remote_port;

	/*
	 * The sequence numbers, for the superuser only; TCP_STATS_HAVE_SEQUENCE
	 * tells a reader they are given, so their 0 otherwise means "not given".
	 */
	if (superuser) {
		entry->features |= TCP_STATS_HAVE_SEQUENCE;
		entry->send_unacknowledged = tcp->send_unacknowledged;
		entry->send_next = sent_end;
		entry->receive_next = tcp->receive_next;
	}

	/* The send side: the peer's window, and the ring that holds what is in flight and what waits unsent. */
	entry->send_window = tcp->peer_window;

	/* Window scaling, when both ends offered it (RFC 7323). */
	if (tcp->wscale_on != 0)
		entry->features |= TCP_STATS_HAVE_WSCALE;
	entry->in_flight_bytes = sent_end - tcp->send_unacknowledged;
	entry->send_segments = tcp->send_count;
	entry->send_ring = TCP_SEND_QUEUE_MAX;
	entry->send_mss = tcp_segment_size_locked(endpoint);

	/* The congestion window and the slow start threshold (RFC 5681), once the connection has them. */
	if (tcp->cwnd != 0) {
		entry->features |= TCP_STATS_HAVE_CWND;
		entry->cwnd = tcp->cwnd;
		entry->ssthresh = tcp->ssthresh;
	}

	/* The congestion control, and CUBIC's memory of the last loss and its epoch. */
	entry->features |= TCP_STATS_HAVE_CC;
	entry->cc = TCP_STATS_CC_RENO;
	if (tcp->cc == TCP_CC_CUBIC) {
		entry->cc = TCP_STATS_CC_CUBIC;
		entry->cubic_w_max_kb = tcp->cubic_w_max / 1024U;
		entry->cubic_k_ms = tcp->cubic_k_ms;
	}

	/* A queued segment the window held back in this round. */
	if (tcp->cwnd_blocked != 0)
		entry->cwnd_state |= TCP_STATS_CWND_BLOCKED;

	/* A CUBIC epoch runs. */
	if (tcp->cubic_epoch_started != 0)
		entry->cwnd_state |= TCP_STATS_CWND_EPOCH;

	/* CUBIC finds the window after a timeout at the next epoch's start. */
	if (tcp->cubic_after_timeout != 0)
		entry->cwnd_state |= TCP_STATS_CWND_AFTER_TIMEOUT;

	/*
	 * The round-trip estimate (ws220-p004; srtt_us and rttvar_us mean
	 * nothing while rtt_samples is 0), and the retransmission timer: how
	 * many times its timeout is doubled (timeouts since the last valid
	 * sample), the timeout it waits now (up to 60 seconds), and what is
	 * left of it.
	 */
	entry->features |= TCP_STATS_HAVE_RTT;
	entry->srtt_us = tcp->srtt_us;
	entry->rttvar_us = tcp->rttvar_us;
	rto = tcp_rto_locked(endpoint);
	timeout = tcp_backoff_timeout(rto, tcp->rto_backoff);
	entry->rtt_samples = tcp->rtt_samples;
	entry->backoff = tcp->rto_backoff;
	entry->rto_ms = (uint32_t)kern_ticks_to_ms(timeout);

	/* The time left of a deadline still ahead; one due or none is 0. */
	if (sent != 0 && tcp->retransmit_deadline > now)
		entry->retransmit_in_ms = (uint32_t)kern_ticks_to_ms(tcp->retransmit_deadline - now);

	/* The receive side: what the peer may send, and what waits for the reader. */
	entry->receive_window = tcp->advertised_window;
	entry->reorder_segments = tcp->reorder_count;
	entry->receive_packets = (uint32_t)socket->receive_packets;
	entry->receive_bytes = (uint32_t)socket->receive_bytes;
	entry->receive_limit_bytes = (uint32_t)socket->receive_hiwat_bytes;

	/* The segment size the SYN offered (the default of the family before one went). */
	entry->receive_mss = tcp->receive_mss;
	if (entry->receive_mss == 0) {
		if (ipv6)
			entry->receive_mss = TCP_RECEIVE_MSS6;
		else
			entry->receive_mss = TCP_RECEIVE_MSS;
	}

	/* The counts since the socket was created. */
	entry->segments_out = counters->segments_out;
	entry->segments_in = counters->segments_in;
	entry->data_segments_out = counters->data_segments_out;
	entry->data_bytes_out = counters->data_bytes_out;
	entry->data_bytes_in = counters->data_bytes_in;
	entry->retransmit_segments = counters->retransmit_segments;
	entry->retransmit_bytes = counters->retransmit_bytes;
	entry->immediate_resends = counters->immediate_resends;
	entry->send_refused = counters->send_refused;
	entry->send_buffer_sleeps = counters->send_buffer_sleeps;
	entry->segment_send_failures = counters->segment_send_failures;
	entry->acks_in = counters->acks_in;
	entry->duplicate_acks_in = counters->duplicate_acks_in;
	entry->acks_outside = counters->acks_outside;
	entry->zero_windows_in = counters->zero_windows_in;
	entry->zero_windows_out = counters->zero_windows_out;
	entry->window_updates_out = counters->window_updates_out;
	entry->old_segments_in = counters->old_segments_in;
	entry->out_of_order_in = counters->out_of_order_in;
	entry->out_of_order_kept = counters->out_of_order_kept;
	entry->receive_refused = counters->receive_refused;

	/* Fast retransmits (ws220-p005). */
	entry->features |= TCP_STATS_HAVE_FAST_RETRANSMIT;
	entry->fast_retransmits = counters->fast_retransmits;
}

/* Builds and transmits one segment with an explicit sequence number. */
static int
tcp_output_segment(
	struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint8_t flags,
	const void *data,
	size_t length,
	struct packet_buf *carrier,
	int news_only)
{
	struct net_device *device;
	struct packet_buf *packet;
	struct tcp_wire *tcp;
	uint32_t acknowledgement;
	struct in6_addr source6;
	struct ipv6_way way6;
	unsigned long generation;
	uint32_t next_hop;
	uint32_t source;
	int routed;
	uint16_t checksum;
	uint32_t window;
	uint32_t offered;
	uint32_t step;
	uint16_t field;
	unsigned shift;
	uint16_t mss;
	uint8_t *option;
	size_t options;
	uint32_t sack_blocks[TCP_SACK_BLOCKS_MAX][2];
	uint32_t sack_acknowledgement;
	unsigned sack_count;
	unsigned index;
	size_t at;
	void *payload;
	int ipv6;
	int error;

	/*
	 * A SYN carries the maximum segment size this end takes: what the
	 * route's interface carries (tcp_path_mss), which the receive window
	 * counts in; without the option a peer assumes its own size.
	 */
	options = 0;
	if ((flags & TCP_SYN) != 0)
		options = 4U;
	if ((flags & TCP_SYN) != 0 && endpoint->tcp.sack_offered != 0)
		options += 4U;
	if ((flags & TCP_SYN) != 0 && endpoint->tcp.wscale_offered != 0)
		options += 4U;

	/*
	 * A segment without data that acknowledges (not a SYN or a reset)
	 * reports the segments kept ahead of a gap and a duplicate's range
	 * (RFC 2018, RFC 2883): two NOPs, then kind 5 with a length of 2 and
	 * eight bytes a block.  One with data carries none: its payload is a
	 * full segment, which the options would push past the MTU.
	 */
	sack_count = 0;
	sack_acknowledgement = 0;
	if ((flags & (TCP_SYN | TCP_RST)) == 0 &&
	    (flags & TCP_ACK) != 0 &&
	    length == 0 &&
	    carrier == NULL &&
	    endpoint->tcp.sack_on != 0)
		sack_count = tcp_sack_blocks(endpoint, sack_blocks, &sack_acknowledgement);

	/* Makes room for the option the blocks make. */
	if (sack_count != 0)
		options = 4U + 8U * sack_count;

	/* Routes the segment: over IPv6 to an IPv6 peer (ws130-p003), else over IPv4. */
	source = 0;
	mss = 0;
	ipv6 = inet_socket_speaks_ipv6(&endpoint->tcp.inet);
	routed = 0;
	next_hop = 0;
	if (ipv6) {
		/*
		 * The way the connection's last lookup found, while nothing has
		 * changed since; else a lookup, kept for the segments after.
		 */
		routed = tcp6_route_cached(endpoint, &device, &source6, &way6);
		error = 0;
		if (!routed) {
			generation = route_generation();
			error = tcp6_route(endpoint, &device, &source6);
			if (error == 0)
				tcp6_route_remember(endpoint, device, generation);
		}
	} else {
		/*
		 * The way the connection's last lookup found, while nothing has
		 * changed since; else a lookup, kept for the segments after.
		 */
		routed = tcp_route_cached(endpoint, &device, &source, &next_hop);
		error = 0;
		if (!routed) {
			generation = route_generation();
			error = tcp_route(endpoint, &device, &source);
			if (error == 0)
				tcp_route_remember(endpoint, device, source, generation);
		}
	}

	/* Gives the carrier back when no route could be found. */
	if (error != 0) {
		packet_buf_free(carrier);
		return error;
	}

	/* The size a SYN offers, which the connection's receive window then counts in. */
	if ((flags & TCP_SYN) != 0) {
		mss = (uint16_t)tcp_path_mss(endpoint, device, ipv6);
		endpoint->tcp.receive_mss = mss;
	}

	/*
	 * A carrier holds the payload already: the header goes in front of
	 * it, into the headroom its copy was made with.
	 */
	if (carrier != NULL) {
		packet = carrier;
		tcp = packet_buf_push(packet, sizeof(*tcp) + options);
		if (tcp == NULL) {
			packet_buf_free(packet);
			if (device != NULL)
				net_device_release(device);
			return EMSGSIZE;
		}
	} else {
		/*
		 * Allocates its buffer.  An empty pool is ENOMEM, which tells it
		 * from a full device queue (ENOBUFS from the driver): the device
		 * says when it has room again, the pool does not.
		 */
		packet = packet_buf_alloc_for(PACKET_BUF_DEFAULT_HEADROOM,
		    sizeof(*tcp) + options + length, 0, &error);
		if (packet == NULL) {
			if (device != NULL)
				net_device_release(device);

			/* An empty pool is reported as out of memory. */
			if (error == ENOBUFS)
				return ENOMEM;
			return error;
		}

		/* A segment too large for one packet's storage is never sent (the callers keep within TCP_SEGMENT_MAX_JUMBO). */
		tcp = packet_buf_append(packet, sizeof(*tcp) + options);
		payload = packet_buf_append(packet, length);
		if (tcp == NULL || payload == NULL) {
			packet_buf_free(packet);
			if (device != NULL)
				net_device_release(device);
			return EMSGSIZE;
		}

		/* The payload. */
		if (length != 0)
			kern_memcpy(payload, data, length);
	}

	/* Fills the header with the current receive state and the free space. */
	kern_memset(tcp, 0, sizeof(*tcp));
	wire_put16(tcp->source, endpoint->tcp.inet.local_port);
	wire_put16(tcp->destination, endpoint->tcp.inet.remote_port);
	wire_put32(tcp->sequence, sequence);
	tcp->data_offset = (uint8_t)((5U + options / 4U) << 4);
	tcp->flags = flags;

	/*
	 * A SYN's options after the fixed header: the MSS (kind 2, length 4),
	 * SACK-Permitted (two NOPs, kind 4, length 2) and the window scale (a
	 * NOP, kind 3, length 3, this end's shift), each offered.
	 */
	option = (uint8_t *)(tcp + 1);
	at = 0;
	if ((flags & TCP_SYN) != 0) {
		option[0] = TCP_OPTION_MSS;
		option[1] = 4U;
		option[2] = (uint8_t)(mss >> 8);
		option[3] = (uint8_t)(mss & 0xffU);
		at = 4U;

		/* Then SACK-Permitted when this end offers selective acknowledgements. */
		if (endpoint->tcp.sack_offered != 0) {
			option[at] = TCP_OPTION_NOP;
			option[at + 1U] = TCP_OPTION_NOP;
			option[at + 2U] = TCP_OPTION_SACK_PERMITTED;
			option[at + 3U] = 2U;
			at += 4U;
		}

		/* Then the window scale. */
		if (endpoint->tcp.wscale_offered != 0) {
			option[at] = TCP_OPTION_NOP;
			option[at + 1U] = TCP_OPTION_WSCALE;
			option[at + 2U] = 3U;
			option[at + 3U] = endpoint->tcp.receive_wscale;
		}
	}

	/* An acknowledgement's SACK blocks, each its left and right edge. */
	if (sack_count != 0) {
		option[0] = TCP_OPTION_NOP;
		option[1] = TCP_OPTION_NOP;
		option[2] = TCP_OPTION_SACK;
		option[3] = (uint8_t)(2U + 8U * sack_count);

		/* Writes each block's left edge and the sequence after its right edge. */
		for (index = 0; index < sack_count; index++) {
			wire_put32(option + 4U + 8U * index, sack_blocks[index][0]);
			wire_put32(option + 8U + 8U * index, sack_blocks[index][1]);
		}
	}

	/*
	 * The acknowledgement, read once for its field and the edge, without
	 * the socket lock: receive_next moves with the receive queue under that
	 * lock (socket_enqueue_stream_advance), and taking the lock for every
	 * segment slowed the receive path.  What it would spare is rare: an
	 * acknowledgement a moment old with the window already narrowed.
	 */
	acknowledgement = endpoint->tcp.receive_next;

	/* One with SACK blocks acknowledges what they were made against (they lie above it). */
	if (sack_count != 0)
		acknowledgement = sack_acknowledgement;

	/*
	 * The window: scaled by this end's shift once both ends scale, never
	 * in a SYN (RFC 7323 section 2.2), and rounded down to what the field
	 * says.  The bytes it says are what the peer was promised.
	 */
	window = tcp_receive_window(endpoint);

	/*
	 * The receiver's silly window avoidance (RFC 9293 section 3.8.6.2.2):
	 * the right edge offered moves on only by min(half the buffer, a
	 * segment) or more; a smaller opening keeps the edge offered before.
	 * The window is never more than the room, so a smaller room than
	 * that edge is what is offered.
	 */
	if ((flags & TCP_SYN) == 0 && endpoint->tcp.receive_edge_set != 0) {
		offered = tcp_receive_offered(endpoint);
		step = tcp_receive_sws_step(endpoint);
		if (window > offered && window - offered < step)
			window = offered;
	}

	/* The shift the window is scaled by: none in a SYN or without scaling. */
	shift = 0;
	if (endpoint->tcp.wscale_on != 0 && (flags & TCP_SYN) == 0)
		shift = endpoint->tcp.receive_wscale;
	if ((window >> shift) > 0xffffU)
		window = 0xffffU << shift;
	field = (uint16_t)(window >> shift);

	/*
	 * A window update that would repeat the last segment's acknowledgement
	 * and window does not go: an acknowledgement sent meanwhile told the
	 * peer of the room already, and the same one again is a duplicate
	 * acknowledgement to it, which its fast retransmit counts.
	 */
	if (news_only && sack_count == 0 && endpoint->tcp.receive_edge_set != 0 &&
	    acknowledgement + endpoint->tcp.advertised_window == endpoint->tcp.receive_edge &&
	    ((uint32_t)field << shift) == endpoint->tcp.advertised_window) {
		packet_buf_free(packet);
		if (device != NULL)
			net_device_release(device);
		return EALREADY;
	}

	/* What the peer is now told: the window and its right edge. */
	endpoint->tcp.advertised_window = (uint32_t)field << shift;
	endpoint->tcp.receive_edge = acknowledgement + endpoint->tcp.advertised_window;
	endpoint->tcp.receive_edge_set = 1;

	/* Both in their fields. */
	wire_put32(tcp->acknowledgement, acknowledgement);
	wire_put16(tcp->window, field);

	/* A copy sent before may carry the marks of its last way out. */
	packet->flags &= ~(PACKET_BUF_TX_CSUM_TCP | PACKET_BUF_TX_CSUM_IPV4);

	/* Hands the segment to IPv6, with the checksum over its pseudo-header. */
	if (ipv6) {
		checksum = net_checksum_pseudo6(source6.s6_addr, endpoint->tcp.inet.remote6.s6_addr, IPPROTO_TCP,
		    packet->data, packet->length);
		wire_put16(tcp->checksum, checksum);
		if (routed) {
			error = ipv6_output_way(device, &source6, &endpoint->tcp.inet.remote6, &way6, IPPROTO_TCP,
			    packet);
		} else {
			error = ipv6_output(device, &source6, &endpoint->tcp.inet.remote6, IPPROTO_TCP, 0U, packet);
		}
		if (device != NULL)
			net_device_release(device);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * Hands the segment to IPv4: with its checksum, or with the
	 * pseudo-header's partial sum for a device that completes it.
	 */
	if (device != NULL && (device->capabilities & NET_DEVICE_CAP_TX_CSUM_IPV4_TCP) != 0) {
		checksum = net_checksum_pseudo_partial(source, endpoint->tcp.inet.remote_address,
		    IPPROTO_TCP, packet->length);
		packet->flags |= PACKET_BUF_TX_CSUM_TCP;
		packet->l4_offset = (uint16_t)(packet->data - packet->storage);
	} else {
		checksum = net_checksum_pseudo(source, endpoint->tcp.inet.remote_address,
		    IPPROTO_TCP, packet->data, packet->length);
	}

	/* The checksum, or the partial sum, in its field. */
	wire_put16(tcp->checksum, checksum);
	if (routed) {
		error = ipv4_output_routed(device, endpoint->tcp.inet.remote_address, next_hop,
		    IPPROTO_TCP, source, packet);
	} else {
		error = ipv4_output_sourced(device, endpoint->tcp.inet.remote_address,
		    IPPROTO_TCP, source, packet);
	}
	net_device_release(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Assigns a free ephemeral port under the registry lock. */
static int
tcp_allocate_port(
	struct tcp_endpoint *endpoint)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&tcp_registry_lock);

	error = tcp_allocate_port_locked(endpoint);

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes the retransmission record out of an endpoint for the caller to free.
 *
 * The unsent entries go with it, and so do the pending acknowledgements and
 * the output retry: a connection reset or given up owes the peer nothing, and
 * one just established has none yet.  send_flushing stays with the thread
 * that set it, which finds the ring emptied when it comes back.
 */
static void
tcp_retransmit_reset(
	struct tcp_endpoint *endpoint,
	struct tcp_discard *discard)
{
	unsigned index;
	unsigned slot;

	/* Hands every outstanding payload to the caller to free unlocked. */
	discard->count = 0;
	for (index = 0; index < endpoint->tcp.send_count; index++) {
		slot = (endpoint->tcp.send_first + index) % TCP_SEND_QUEUE_MAX;
		discard->packets[discard->count++] =
		    endpoint->tcp.send_queue[slot].packet;
		endpoint->tcp.send_queue[slot].packet = NULL;
	}
	endpoint->tcp.send_first = 0;
	endpoint->tcp.send_count = 0;
	endpoint->tcp.send_unsent = 0;
	endpoint->tcp.send_jumbo = 0;
	endpoint->tcp.send_bytes = 0;
	endpoint->tcp.send_small_outstanding = 0;
	endpoint->tcp.retransmit_deadline = 0;
	endpoint->tcp.retransmit_count = 0;
	endpoint->tcp.retransmit_since = 0;
	endpoint->tcp.retransmit_refused = 0;
	endpoint->tcp.retransmit_immediate = 0;
	endpoint->tcp.recovery = 0;
	endpoint->tcp.recovery_by_timeout = 0;
	endpoint->tcp.dupack_guard = 0;
	endpoint->tcp.duplicate_acks = 0;
	endpoint->tcp.persist = 0;
	endpoint->tcp.persist_shift = 0;
	endpoint->tcp.unack_since = 0;

	/* No segment is left to time; the estimate and its backoff stay for the connection. */
	endpoint->tcp.rtt_timing = 0;

	/* Nothing is owed the device any more. */
	endpoint->tcp.ack_pending = 0;
	tcp_output_retry_settle_locked(endpoint);
}

/* Frees the payloads a reset handed over, outside any lock. */
static void
tcp_discard_free(
	struct tcp_discard *discard)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < discard->count; index++)
		packet_buf_free(discard->packets[index]);
	discard->count = 0;
}

/*
 * Tells whether one sequence number comes before another.
 *
 * Sequence numbers are compared modulo 2^32 (RFC 9293 section 3.4): left
 * comes first when it lies in the half of the space before right, which
 * stays true where the numbers wrap from 2^32 - 1 to 0.
 */
static int
tcp_sequence_before(
	uint32_t left,
	uint32_t right)
{
	int32_t distance;

	/* The signed distance from right to left: negative when left comes first. */
	distance = (int32_t)(left - right);

	/* left lies in the half of the space before right. */
	if (distance < 0)
		return 1;

	/* Succeeded: left is right itself or comes after it. */
	return 0;
}

/* Tells whether one sequence number is another or comes before it, modulo 2^32. */
static int
tcp_sequence_at_or_before(
	uint32_t left,
	uint32_t right)
{
	int before;

	/* The same number. */
	if (left == right)
		return 1;

	/* An earlier one. */
	before = tcp_sequence_before(left, right);
	if (before)
		return 1;

	/* Succeeded: left comes after right. */
	return 0;
}

/*
 * Gives the initial sequence number of a new connection.
 *
 * chosen is the number the caller picked; a build with
 * CONFIG_TCP_ISN_WRAP_BEFORE puts the first data byte that many bytes
 * before the wrap instead, the SYN taking the number before it.
 */
static uint32_t
tcp_initial_sequence(
	uint32_t chosen)
{
	uint32_t wrap_before;

	/* The ordinary build keeps the number the caller picked. */
	wrap_before = (uint32_t)CONFIG_TCP_ISN_WRAP_BEFORE;
	if (wrap_before == 0U)
		return chosen;

	/* Succeeded: the SYN's number, one before the first data byte. */
	return 0U - wrap_before - 1U;
}

/*
 * Gives the retransmission timeout after a number of resends.
 *
 * The connection's timeout (rto, in ticks; see tcp_rto_locked) doubles for
 * each resend (RFC 6298 section 5.5) and stops at
 * TCP_RETRANSMIT_TIMEOUT_MAX.
 */
static uint64_t
tcp_backoff_timeout(
	uint64_t rto,
	unsigned count)
{
	uint64_t timeout;
	unsigned index;

	/* Doubles the connection's timeout once per resend, until it reaches the longest. */
	timeout = rto;
	for (index = 0; index < count; index++) {
		timeout *= 2U;
		if (timeout >= TCP_RETRANSMIT_TIMEOUT_MAX)
			break;
	}

	/* A doubling past the longest timeout is held at it. */
	if (timeout > TCP_RETRANSMIT_TIMEOUT_MAX)
		timeout = TCP_RETRANSMIT_TIMEOUT_MAX;

	/* Succeeded: the timeout the next deadline waits. */
	return timeout;
}

/*
 * Gives a connection's retransmission timeout before any backoff, in ticks,
 * under the socket lock: the one its round-trip estimate gives, or the
 * initial second before the first sample (RFC 6298 section 2.1).
 */
static uint64_t
tcp_rto_locked(
	const struct tcp_endpoint *endpoint)
{
	/* No sample yet. */
	if (endpoint->tcp.rto == 0)
		return TCP_INITIAL_RTO;

	/* Succeeded: the estimate's timeout. */
	return endpoint->tcp.rto;
}

/*
 * Takes the round-trip sample of the segment being timed, which an
 * acknowledgement has just covered, under the socket lock (RFC 6298
 * section 2, ws220-p004).
 *
 * The sample is the ticks since the segment was handed to the device, in
 * microseconds (0 when it came back within the tick).  The first sets
 * SRTT to it and RTTVAR to half of it; each later one moves RTTVAR a
 * quarter and SRTT an eighth of the way.  The timeout is then SRTT +
 * max(G, 4 RTTVAR, TCP_RTO_MIN_TERM_US), G being one tick, rounded up to a
 * tick and at most TCP_RETRANSMIT_TIMEOUT_MAX.
 */
static void
tcp_rtt_sample_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now)
{
	uint64_t ticks;
	uint64_t sample;
	uint64_t variation;
	uint64_t difference;
	uint64_t term;
	uint64_t granularity;
	uint64_t timeout_us;
	uint64_t timeout;

	/*
	 * The segment is timed no longer; the next one sent is.  A valid
	 * sample ends the backoff (RFC 6298 section 5 note).
	 */
	endpoint->tcp.rtt_timing = 0;
	endpoint->tcp.rto_backoff = 0;

	/* The sample in microseconds, bounded so the sums below stay small. */
	ticks = now - endpoint->tcp.rtt_start;
	sample = ticks * 1000000U / KERN_CLOCK_HZ;
	if (sample > TCP_RTT_SAMPLE_MAX_US)
		sample = TCP_RTT_SAMPLE_MAX_US;

	/* The first sample (RFC 6298 section 2.2), or a later one (section 2.3). */
	if (endpoint->tcp.rtt_samples == 0) {
		endpoint->tcp.srtt_us = (uint32_t)sample;
		endpoint->tcp.rttvar_us = (uint32_t)(sample / 2U);
	} else {
		/* |SRTT - R|, taken before SRTT moves. */
		if (endpoint->tcp.srtt_us > sample) {
			difference = endpoint->tcp.srtt_us - sample;
		} else {
			difference = sample - endpoint->tcp.srtt_us;
		}

		/* RTTVAR a quarter of the way to the difference, then SRTT an eighth of the way to the sample. */
		variation = ((uint64_t)endpoint->tcp.rttvar_us * 3U + difference) / 4U;
		endpoint->tcp.rttvar_us = (uint32_t)variation;
		endpoint->tcp.srtt_us = (uint32_t)(((uint64_t)endpoint->tcp.srtt_us * 7U + sample) / 8U);
	}

	/* One more sample, the count held at its largest. */
	if (endpoint->tcp.rtt_samples != UINT32_MAX)
		endpoint->tcp.rtt_samples++;

	/* The term added to SRTT: the largest of the clock's tick, 4 RTTVAR and the least term. */
	granularity = 1000000U / KERN_CLOCK_HZ;
	term = (uint64_t)endpoint->tcp.rttvar_us * 4U;
	if (term < granularity)
		term = granularity;
	if (term < TCP_RTO_MIN_TERM_US)
		term = TCP_RTO_MIN_TERM_US;
	timeout_us = endpoint->tcp.srtt_us + term;

	/*
	 * In ticks, rounded up, and one tick more: a deadline counted from
	 * the tick a segment went in can expire up to a tick before the
	 * timeout has really passed (ws220-p004 review-1 M4, a peer whose
	 * delayed acknowledgement comes just at the least term).  No longer
	 * than the longest timeout.
	 */
	timeout = (timeout_us * KERN_CLOCK_HZ + 999999U) / 1000000U + 1U;
	if (timeout > TCP_RETRANSMIT_TIMEOUT_MAX)
		timeout = TCP_RETRANSMIT_TIMEOUT_MAX;
	endpoint->tcp.rto = timeout;
}

/*
 * Forgets a connection's round-trip estimate, under the socket lock: a new
 * connect measures its own, starting from the initial second.
 */
static void
tcp_rtt_forget_locked(
	struct tcp_endpoint *endpoint)
{
	/* No sample, no timeout of its own, no backoff, nothing timed. */
	endpoint->tcp.rtt_samples = 0;
	endpoint->tcp.srtt_us = 0;
	endpoint->tcp.rttvar_us = 0;
	endpoint->tcp.rto = 0;
	endpoint->tcp.rto_backoff = 0;
	endpoint->tcp.rtt_timing = 0;
	endpoint->tcp.syn_timed_out = 0;
	endpoint->tcp.recovery = 0;
	endpoint->tcp.recovery_by_timeout = 0;
	endpoint->tcp.dupack_guard = 0;
	endpoint->tcp.duplicate_acks = 0;
	endpoint->tcp.persist = 0;
	endpoint->tcp.persist_shift = 0;
	endpoint->tcp.unack_since = 0;
}

/*
 * Takes what the handshake measured, under the socket lock, as an
 * acknowledgement of this end's SYN (or SYN-ACK) completes it, before the
 * ring is emptied (ws220-p004).
 *
 * The SYN timed and never resent gives the first sample.  One whose timer
 * expired, or whose SYN-ACK was sent again for a repeated SYN, gives none
 * (Karn's rule), and the data then starts with a timeout of three seconds
 * rather than the initial one (RFC 6298 section 5.7).  Either way the
 * backoff of the handshake ends: the data has its own timeout.  The caller
 * calls this before tcp_retransmit_reset() empties the ring.
 */
static void
tcp_rtt_handshake_locked(
	struct tcp_endpoint *endpoint,
	uint32_t acknowledgement,
	uint64_t now)
{
	int covered;

	/* The SYN timed, never resent, and acknowledged now. */
	covered = 0;
	if (endpoint->tcp.rtt_timing != 0 && endpoint->tcp.syn_timed_out == 0)
		covered = tcp_sequence_at_or_before(endpoint->tcp.rtt_sequence, acknowledgement);
	if (covered)
		tcp_rtt_sample_locked(endpoint, now);

	/* A SYN resent, without a sample. */
	if (endpoint->tcp.syn_timed_out != 0 && endpoint->tcp.rtt_samples == 0)
		endpoint->tcp.rto = TCP_SYN_RETRIED_RTO;

	/* The data starts without the handshake's backoff. */
	endpoint->tcp.rto_backoff = 0;
	endpoint->tcp.syn_timed_out = 0;
	endpoint->tcp.rtt_timing = 0;
}

/*
 * Tells whether the timer has resent the oldest segment for long enough to
 * give the attempt up, under the socket lock.
 *
 * The first timeout since the peer last answered starts the count; R2 runs
 * from it, three minutes while the connection is still being set up (a
 * SYN is what is resent) and 100 seconds after (RFC 1122 section 4.2.3.5).
 */
static int
tcp_retransmit_given_up_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now)
{
	uint64_t limit;

	/* The first timeout since the peer answered starts the time R2 counts. */
	if (endpoint->tcp.retransmit_since == 0) {
		endpoint->tcp.retransmit_since = now;
		return 0;
	}

	/* A connection being set up resends its SYN for longer than one that carries data. */
	if (endpoint->tcp.state == TCP_SYN_SENT ||
	    endpoint->tcp.state == TCP_SYN_RECEIVED) {
		limit = KERN_MS_TO_TICKS(TCP_GIVE_UP_SYN_MS);
	} else {
		limit = KERN_MS_TO_TICKS(TCP_GIVE_UP_DATA_MS);
	}

	/* R2 has not passed yet: the segment is resent again. */
	if (now - endpoint->tcp.retransmit_since < limit)
		return 0;

	/* Succeeded: R2 has passed without an answer, so the attempt is given up. */
	return 1;
}

/*
 * Tells how a reset of a connection past its SYN is taken, under the socket
 * lock (RFC 9293 section 3.10.7.4, RFC 5961 section 3.2).
 *
 * A reset whose sequence number is the next one expected resets the
 * connection.  One elsewhere within the window this end advertised is
 * answered by an acknowledgement, which a peer that really reset answers
 * with a reset of the right number; a blind reset from someone who does
 * not know the number ends nothing.  Any other is ignored.
 */
static enum tcp_reset_check
tcp_reset_check_locked(
	const struct tcp_endpoint *endpoint,
	uint32_t sequence)
{
	uint32_t offset;

	/* The next sequence number expected resets the connection. */
	if (sequence == endpoint->tcp.receive_next)
		return TCP_RESET_ACCEPT;

	/* Elsewhere in the window this end last advertised: the peer is asked again. */
	offset = sequence - endpoint->tcp.receive_next;
	if (offset < (uint32_t)endpoint->tcp.advertised_window)
		return TCP_RESET_CHALLENGE;

	/* Succeeded: outside the window, the reset is not this connection's. */
	return TCP_RESET_IGNORE;
}

/*
 * Tells whether an acceptable acknowledgement may set the peer's window,
 * under the socket lock (RFC 9293 section 3.10.7.4).
 *
 * It may when its segment is newer than the one the window came from:
 * a later sequence number (SND.WL1), or the same one and an
 * acknowledgement at least as far (SND.WL2).  The caller has checked that
 * the acknowledgement lies within what was sent.
 */
static int
tcp_send_window_update_allowed_locked(
	const struct tcp_endpoint *endpoint,
	uint32_t sequence,
	uint32_t acknowledgement)
{
	int newer;

	/* A segment from later in the peer's sequence space. */
	newer = tcp_sequence_before(endpoint->tcp.send_window_sequence, sequence);
	if (newer)
		return 1;

	/* An older segment never sets the window. */
	if (sequence != endpoint->tcp.send_window_sequence)
		return 0;

	/* The same segment number with an acknowledgement as far or farther. */
	newer = tcp_sequence_at_or_before(endpoint->tcp.send_window_acknowledgement, acknowledgement);
	if (newer)
		return 1;

	/* Succeeded: the window came from a newer acknowledgement, which stands. */
	return 0;
}

/*
 * Tells whether the oldest segment sent went out as a probe of a window
 * that could not take it and now fits the window the peer advertises,
 * under the socket lock (ws220-p005).
 */
static int
tcp_probe_fits_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *oldest;
	uint32_t end;

	/* No probe: an ordinary segment, which a reopened window says nothing of. */
	oldest = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
	if (oldest->probe == 0)
		return 0;

	/* The window does not reach its end yet: the timer probes again. */
	end = oldest->sequence + oldest->advance - endpoint->tcp.send_unacknowledged;
	if (end > (uint32_t)endpoint->tcp.peer_window)
		return 0;

	/* Succeeded: the probe fits now. */
	return 1;
}

/*
 * Tells whether a connection is in the persist state, under the socket
 * lock: a data segment is the oldest sent and the window the peer
 * advertises cannot take it, in a state that sends data (ws220-p005).
 */
static int
tcp_persist_applies_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *oldest;
	unsigned sent;
	int fits;

	/* A state with data to send: before the handshake there is no window, after both FINs no data. */
	if (endpoint->tcp.state != TCP_ESTABLISHED &&
	    endpoint->tcp.state != TCP_CLOSE_WAIT &&
	    endpoint->tcp.state != TCP_FIN_WAIT_1 &&
	    endpoint->tcp.state != TCP_LAST_ACK)
		return 0;

	/* A data segment sent and not acknowledged. */
	sent = tcp_sent_count_locked(endpoint);
	if (sent == 0)
		return 0;
	oldest = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
	if (oldest->length == 0)
		return 0;

	/* The window takes it: the retransmission timer's, not the persist timer's. */
	fits = tcp_oldest_fits_window_locked(endpoint);
	if (fits)
		return 0;

	/* Succeeded: the window is too small for it. */
	return 1;
}

/*
 * Gives the persist timer's next interval, under the socket lock: the RTO
 * times the persist backoff's step, held between 5 and 60 seconds
 * (FreeBSD's tcp_setpersist).
 */
static uint64_t
tcp_persist_interval_locked(
	const struct tcp_endpoint *endpoint)
{
	uint64_t interval;
	unsigned shift;

	/* The RTO times the step's multiple. */
	shift = endpoint->tcp.persist_shift;
	if (shift > TCP_PERSIST_SHIFT_MAX)
		shift = TCP_PERSIST_SHIFT_MAX;
	interval = tcp_rto_locked(endpoint) * tcp_persist_backoff[shift];

	/* Held between the least and the most. */
	if (interval < TCP_PERSIST_MIN)
		interval = TCP_PERSIST_MIN;
	if (interval > TCP_PERSIST_MAX)
		interval = TCP_PERSIST_MAX;

	/* Succeeded: the interval in ticks. */
	return interval;
}

/*
 * Tells whether the persist timer gives a connection up, under the socket
 * lock (FreeBSD's tcp_timer_persist).
 *
 * A peer that answers (last_receive keeps moving) is never given up here
 * (RFC 1122 section 4.2.2.17); TCP_MAXUNACKTIME is the way to drop one.  A
 * silent one is given up after the last backoff step and an idle of
 * min(2 hours, RTO x 2559), or after 60 seconds once this end has closed.
 */
static int
tcp_persist_given_up_locked(
	const struct tcp_endpoint *endpoint,
	uint64_t now)
{
	uint64_t idle;
	uint64_t limit;

	/* The time since the peer last sent anything. */
	idle = now - endpoint->tcp.last_receive;

	/* After every backoff step, silent for the longest idle. */
	limit = tcp_rto_locked(endpoint) * TCP_PERSIST_TOTAL_BACKOFF;
	if (limit > TCP_PERSIST_IDLE_MAX)
		limit = TCP_PERSIST_IDLE_MAX;
	if (endpoint->tcp.persist_shift >= TCP_PERSIST_SHIFT_MAX && idle >= limit)
		return 1;

	/* Closed by this end (FIN_WAIT_1, LAST_ACK), silent for a minute. */
	if (endpoint->tcp.state == TCP_FIN_WAIT_1 || endpoint->tcp.state == TCP_LAST_ACK) {
		if (idle >= TCP_PERSIST_CLOSED_IDLE)
			return 1;
	}

	/* Succeeded: the window is probed again. */
	return 0;
}

/*
 * Tells whether a connection's data has made no progress for its
 * TCP_MAXUNACKTIME, under the socket lock (ws220-p005).
 */
static int
tcp_maxunack_expired_locked(
	const struct tcp_endpoint *endpoint,
	uint64_t now)
{
	/* The option is off. */
	if (endpoint->tcp.maxunack == 0)
		return 0;

	/* Nothing waits for an acknowledgement. */
	if (endpoint->tcp.unack_since == 0)
		return 0;

	/* Not that long yet. */
	if (now - endpoint->tcp.unack_since < endpoint->tcp.maxunack)
		return 0;

	/* Succeeded: no progress for that long. */
	return 1;
}

/*
 * Leaves the persist state, under the socket lock: the window has opened,
 * the probe was answered, or the timer resends a segment the window takes.
 * The backoff of a step starts over the next time (FreeBSD's t_rxtshift).
 */
static void
tcp_persist_leave_locked(
	struct tcp_endpoint *endpoint)
{
	/* Out of the state, its backoff back at the first step. */
	endpoint->tcp.persist = 0;
	endpoint->tcp.persist_shift = 0;
}

/*
 * Tells whether the oldest segment sent fits the window the peer
 * advertises, under the socket lock: while it does
 * not, the peer's acknowledgements answer a closed window.
 */
static int
tcp_oldest_fits_window_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *oldest;
	uint32_t end;

	/* The oldest segment's end, from SND.UNA. */
	oldest = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
	end = oldest->sequence + oldest->advance - endpoint->tcp.send_unacknowledged;

	/* Beyond the window. */
	if (end > (uint32_t)endpoint->tcp.peer_window)
		return 0;

	/* Succeeded: it fits. */
	return 1;
}

/* Reports the sequence space the outstanding segments occupy. */
static uint32_t
tcp_in_flight(
	const struct tcp_endpoint *endpoint)
{
	/* Returns the computed result. */
	return endpoint->tcp.send_next - endpoint->tcp.send_unacknowledged;
}

/*
 * Reads the maximum segment size a SYN offers (RFC 9293 section 3.2), or
 * 0 when it offers none.  The options end at the header's end, at an end
 * of options, or at one whose length does not fit.
 */
static uint32_t
tcp_syn_offered_mss(
	const struct tcp_wire *tcp,
	size_t header_length)
{
	const uint8_t *options;
	size_t length;
	size_t at;
	uint8_t kind;
	uint8_t size;

	/* The options between the fixed header and the data. */
	options = (const uint8_t *)(tcp + 1);
	length = header_length - sizeof(*tcp);

	/* Each option in turn until the MSS is found or the options end. */
	at = 0;
	while (at < length) {
		kind = options[at];
		if (kind == TCP_OPTION_END)
			break;
		if (kind == TCP_OPTION_NOP) {
			at++;
			continue;
		}

		/* An option with a length: one that does not fit ends the list. */
		if (at + 1U >= length)
			break;
		size = options[at + 1U];
		if (size < 2U || at + size > length)
			break;

		/* The MSS option: kind 2, length 4, the size in network order. */
		if (kind == TCP_OPTION_MSS && size == 4U)
			return ((uint32_t)options[at + 2U] << 8) | options[at + 3U];
		at += size;
	}

	/* None offered. */
	return 0;
}

/*
 * Reads the window scale shift a SYN offers (RFC 7323 section 2.2), no
 * more than 14, or -1 when it offers none.
 */
static int
tcp_syn_offered_wscale(
	const struct tcp_wire *tcp,
	size_t header_length)
{
	const uint8_t *options;
	size_t length;
	size_t at;
	uint8_t kind;
	uint8_t size;
	unsigned shift;

	/* The options between the fixed header and the data. */
	options = (const uint8_t *)(tcp + 1);
	length = header_length - sizeof(*tcp);

	/* Each option in turn until the window scale is found or the options end. */
	at = 0;
	while (at < length) {
		kind = options[at];
		if (kind == TCP_OPTION_END)
			break;
		if (kind == TCP_OPTION_NOP) {
			at++;
			continue;
		}

		/* An option with a length: one that does not fit ends the list. */
		if (at + 1U >= length)
			break;
		size = options[at + 1U];
		if (size < 2U || at + size > length)
			break;

		/* The window scale option: kind 3, length 3, the shift (a larger one is taken as 14). */
		if (kind == TCP_OPTION_WSCALE && size == 3U) {
			shift = options[at + 2U];
			if (shift > TCP_WSCALE_MAX)
				shift = TCP_WSCALE_MAX;
			return (int)shift;
		}

		/* Any other option is skipped by its length. */
		at += size;
	}

	/* None offered. */
	return -1;
}

/* Tells whether a SYN offers SACK-Permitted (RFC 2018 section 2: kind 4, length 2). */
static int
tcp_syn_offered_sack(
	const struct tcp_wire *tcp,
	size_t header_length)
{
	const uint8_t *options;
	size_t length;
	size_t at;
	uint8_t kind;
	uint8_t size;

	/* The options between the fixed header and the data. */
	options = (const uint8_t *)(tcp + 1);
	length = header_length - sizeof(*tcp);

	/* Each option in turn until SACK-Permitted is found or the options end. */
	at = 0;
	while (at < length) {
		kind = options[at];

		/* The end of the options. */
		if (kind == TCP_OPTION_END)
			break;

		/* A padding byte is a single byte long. */
		if (kind == TCP_OPTION_NOP) {
			at++;
			continue;
		}

		/* An option with a length: one that does not fit ends the list. */
		if (at + 1U >= length)
			break;

		/* Reads the option's length. */
		size = options[at + 1U];

		/* A length too short or running past the options ends the list. */
		if (size < 2U || at + size > length)
			break;

		/* Reports the SYN as offering SACK-Permitted. */
		if (kind == TCP_OPTION_SACK_PERMITTED && size == 2U)
			return 1;

		/* Any other option is skipped by its length. */
		at += size;
	}

	/* None offered. */
	return 0;
}

/*
 * Adds a block of kept segments, cut to what lies above the
 * acknowledgement it goes with (a block at or below it would read as a
 * D-SACK, RFC 2883), and returns the count of blocks then.
 */
static unsigned
tcp_sack_block_add(
	uint32_t blocks[TCP_SACK_BLOCKS_MAX][2],
	unsigned count,
	uint32_t start,
	uint32_t end,
	uint32_t acknowledgement)
{
	int taken;
	int below;

	/* Wholly taken already: none. */
	taken = tcp_sequence_at_or_before(end, acknowledgement);
	if (taken)
		return count;

	/* Cuts the block to start at the acknowledgement when it began below it. */
	below = tcp_sequence_before(start, acknowledgement);
	if (below)
		start = acknowledgement;

	/* Records the block. */
	blocks[count][0] = start;
	blocks[count][1] = end;

	/* Succeeded: one more. */
	return count + 1U;
}

/*
 * Fills the SACK blocks an acknowledgement reports (RFC 2018 section 4,
 * RFC 2883), each its left edge and the sequence after its right, and
 * returns how many: first a duplicate's range the last segment in brought
 * (D-SACK, reported once), then the block of the segments kept ahead of a
 * gap that holds the one kept last, then the other kept blocks from the
 * highest down.  Kept segments that touch or overlap make one block.
 */
static unsigned
tcp_sack_blocks(
	struct tcp_endpoint *endpoint,
	uint32_t blocks[TCP_SACK_BLOCKS_MAX][2],
	uint32_t *acknowledgement)
{
	struct tcp_socket *tcp = &endpoint->tcp;
	unsigned long irq;
	unsigned count;
	unsigned index;
	unsigned first;
	uint32_t start;
	uint32_t end;
	uint32_t entry_end;
	uint32_t recent_start;
	int recent_found;
	int joins;
	int longer;
	int from_start;
	int before_end;

	/* Reports nothing when no segment is kept and no duplicate is owed, read without the lock (most acknowledgements). */
	count = 0;
	if (tcp->reorder_count == 0 && tcp->dsack_pending == 0)
		return 0;

	/* Reads the kept segments and the owed duplicate under the socket lock. */
	irq = spin_lock_irqsave(&tcp->inet.socket.lock);

	/* The acknowledgement the blocks go with, read with them. */
	*acknowledgement = tcp->receive_next;

	/* A duplicate's range, once. */
	if (tcp->dsack_pending != 0) {
		blocks[count][0] = tcp->dsack_start;
		blocks[count][1] = tcp->dsack_end;
		count++;
		tcp->dsack_pending = 0;
	}

	/* The block that holds the segment kept last, found from the lowest. */
	recent_found = 0;
	recent_start = 0;
	index = 0;
	while (tcp->sack_recent_set != 0 && index < tcp->reorder_count) {
		start = tcp->reorder_sequence[index];
		end = start + tcp->reorder_length[index];
		index++;

		/* The segments that touch or overlap it join it. */
		for (;;) {
			/* The last kept segment ends the block. */
			if (index >= tcp->reorder_count)
				break;

			/* A segment starting beyond the block's end leaves a gap, which ends it. */
			joins = tcp_sequence_at_or_before(tcp->reorder_sequence[index], end);
			if (!joins)
				break;

			/* Extends the block over the segment when it reaches further. */
			entry_end = tcp->reorder_sequence[index] + tcp->reorder_length[index];
			longer = tcp_sequence_before(end, entry_end);
			if (longer)
				end = entry_end;

			/* Moves on to the next kept segment. */
			index++;
		}

		/* The block reported first when it holds the segment kept last. */
		from_start = tcp_sequence_at_or_before(start, tcp->sack_recent);
		before_end = tcp_sequence_before(tcp->sack_recent, end);
		if (from_start && before_end) {
			recent_found = 1;
			recent_start = start;
			count = tcp_sack_block_add(blocks, count, start, end, *acknowledgement);
			break;
		}
	}

	/* The others from the highest down, each from its last segment back to its first. */
	index = tcp->reorder_count;
	while (index > 0 && count < TCP_SACK_BLOCKS_MAX) {
		index--;
		start = tcp->reorder_sequence[index];
		end = start + tcp->reorder_length[index];
		first = index;
		for (;;) {
			/* The lowest kept segment ends the block. */
			if (first == 0)
				break;

			/* A segment ending below the block's start leaves a gap, which ends it. */
			entry_end = tcp->reorder_sequence[first - 1U] + tcp->reorder_length[first - 1U];
			joins = tcp_sequence_at_or_before(start, entry_end);
			if (!joins)
				break;

			/* Takes the segment into the block, which then starts there. */
			first--;
			start = tcp->reorder_sequence[first];

			/* Extends the block's end when the segment reaches further. */
			longer = tcp_sequence_before(end, entry_end);
			if (longer)
				end = entry_end;
		}

		/* The next block down starts below this one. */
		index = first;

		/* The block reported first is not repeated. */
		if (recent_found && start == recent_start)
			continue;

		/* Adds the block. */
		count = tcp_sack_block_add(blocks, count, start, end, *acknowledgement);
	}

	spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);

	/* Succeeded: the blocks filled. */
	return count;
}

/*
 * Marks the sent segments that an acknowledgement's SACK blocks report
 * held (RFC 2018 section 3, RFC 6675 section 4), each wholly within one
 * block, and returns how many it newly marks, or -1 when the segment
 * carries no SACK option.  A block is read modulo 2^32 and cut to what
 * lies between the acknowledgement and the most ever sent; one whose
 * edges are out of order says nothing.  The caller holds the socket lock.
 */
static int
tcp_sack_take_locked(
	struct tcp_endpoint *endpoint,
	const struct tcp_wire *tcp,
	size_t header_length)
{
	struct tcp_pending *entry;
	const uint8_t *options;
	const uint8_t *block;
	size_t length;
	size_t at;
	uint8_t kind;
	uint8_t size;
	uint32_t left;
	uint32_t right;
	uint32_t low;
	uint32_t high;
	unsigned blocks;
	unsigned index;
	unsigned sent;
	unsigned position;
	int found;
	int newly;
	int empty;
	int below;
	int above;
	int starts_inside;
	int ends_inside;
	int resend_held;
	uint16_t latest;

	/* The options between the fixed header and the data, and the SACK option among them. */
	options = (const uint8_t *)(tcp + 1);
	length = header_length - sizeof(*tcp);
	found = 0;
	block = NULL;
	blocks = 0;
	at = 0;
	while (at < length && !found) {
		kind = options[at];

		/* The end of the options. */
		if (kind == TCP_OPTION_END)
			break;

		/* A padding byte is a single byte long. */
		if (kind == TCP_OPTION_NOP) {
			at++;
			continue;
		}

		/* An option with a length: one that does not fit ends the list. */
		if (at + 1U >= length)
			break;

		/* Reads the option's length. */
		size = options[at + 1U];

		/* A length too short or running past the options ends the list. */
		if (size < 2U || at + size > length)
			break;

		/* The SACK option: kind 5, two bytes and eight a block. */
		if (kind == TCP_OPTION_SACK &&
		    size >= 10U &&
		    (size - 2U) % 8U == 0) {
			found = 1;
			block = options + at + 2U;
			blocks = (size - 2U) / 8U;
		}

		/* Any other option is skipped by its length. */
		at += size;
	}

	/* None: the acknowledgement says nothing of held segments. */
	if (!found)
		return -1;

	/* Each block marks the sent segments it holds whole. */
	low = endpoint->tcp.send_unacknowledged;
	high = endpoint->tcp.send_max;
	sent = tcp_sent_count_locked(endpoint);
	newly = 0;
	resend_held = 0;
	latest = 0;
	for (index = 0; index < blocks; index++) {
		left = wire_get32(block + 8U * index);
		right = wire_get32(block + 4U + 8U * index);

		/* A block whose edges are out of order says nothing. */
		empty = tcp_sequence_at_or_before(right, left);
		if (empty)
			continue;

		/* Cuts the block to what is outstanding: not below the acknowledgement, not above the most sent. */
		below = tcp_sequence_before(left, low);
		if (below)
			left = low;

		/* Cuts the block's right edge to the most sent. */
		above = tcp_sequence_before(high, right);
		if (above)
			right = high;

		/* A block wholly outside what is outstanding is empty by now. */
		empty = tcp_sequence_at_or_before(right, left);
		if (empty)
			continue;

		/* The segments within it. */
		for (position = 0; position < sent; position++) {
			entry = &endpoint->tcp.send_queue[(endpoint->tcp.send_first + position) % TCP_SEND_QUEUE_MAX];

			/* A segment already marked, or one that carries no data, is passed by. */
			if (entry->sacked != 0 || entry->length == 0)
				continue;

			/* Only a segment that lies wholly within the block is held. */
			starts_inside = tcp_sequence_at_or_before(left, entry->sequence);
			if (!starts_inside)
				continue;

			/* The segment's end must not pass the block's right edge. */
			ends_inside = tcp_sequence_at_or_before(entry->sequence + entry->length, right);
			if (!ends_inside)
				continue;

			/* Marks the segment as held by the peer. */
			entry->sacked = 1;
			newly++;

			/* The latest resend the peer now reports held. */
			if (entry->resent != 0 &&
			    (!resend_held ||
			     (int16_t)(entry->resend_order - latest) > 0)) {
				latest = entry->resend_order;
				resend_held = 1;
			}
		}
	}

	/*
	 * A resend not held while a later one is was lost as well (as Linux
	 * finds lost retransmissions): it may go again, the oldest at once.
	 */
	for (position = 0; resend_held && position < sent; position++) {
		entry = &endpoint->tcp.send_queue[(endpoint->tcp.send_first + position) % TCP_SEND_QUEUE_MAX];

		/* Only a resend that is not held while a later one is was lost. */
		if (entry->sacked != 0 ||
		    entry->resent == 0 ||
		    (int16_t)(latest - entry->resend_order) <= 0)
			continue;

		/* Lets the segment go again. */
		entry->resent = 0;

		/* The oldest goes at once. */
		if (position == 0) {
			endpoint->tcp.retransmit_deadline = sched_ticks();
			endpoint->tcp.retransmit_immediate = TCP_IMMEDIATE_LOSS;
			endpoint->tcp.counters.immediate_resends++;
		}
	}

	/* Succeeded: the count newly marked. */
	return newly;
}

/*
 * Takes the resend marks off every segment in the ring, and with sacked
 * the SACK marks too.  The caller holds the socket lock.
 */
static void
tcp_sack_clear_locked(
	struct tcp_endpoint *endpoint,
	int sacked)
{
	struct tcp_pending *entry;
	unsigned position;

	/* Each entry in the ring. */
	for (position = 0; position < endpoint->tcp.send_count; position++) {
		entry = &endpoint->tcp.send_queue[(endpoint->tcp.send_first + position) % TCP_SEND_QUEUE_MAX];
		entry->resent = 0;

		/* The SACK mark goes too when asked. */
		if (sacked)
			entry->sacked = 0;
	}
}

/*
 * Resends, within a fast recovery, the holes the SACK blocks show lost
 * (RFC 6675 section 5, NextSeg's first rule): from the lowest, a segment
 * not held, not yet resent in this recovery, with more than
 * (DupThresh - 1) segments' bytes held above it (IsLost), while what is
 * in the network (SetPipe: each byte not held and not lost counts once,
 * and once more when resent) leaves room in the congestion window.  The
 * oldest segment is the fast retransmit's and the partial
 * acknowledgements' (the timer sends it), so it is passed by.  A resend
 * holds a reference on the segment's copy while the socket lock is
 * released, so an acknowledgement that retires it meanwhile does not
 * free it.  At most TCP_SACK_RESENDS_MAX go for one acknowledgement.
 */
static void
tcp_sack_resend(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket = &endpoint->tcp.inet.socket;
	struct tcp_pending *entry;
	struct tcp_pending *chosen;
	struct packet_buf *copy;
	const uint8_t *payload;
	unsigned long irq;
	unsigned position;
	unsigned sent;
	unsigned resends;
	uint32_t held_above;
	uint32_t threshold;
	uint32_t pipe;
	uint32_t sequence;
	uint32_t mss;
	uint16_t length;
	uint8_t flags;
	enum tcp_refusal refusal;
	int lost;
	int retries;
	int error;

	/* One resend at a time, each chosen afresh. */
	for (resends = 0; resends < TCP_SACK_RESENDS_MAX; resends++) {
		irq = spin_lock_irqsave(&socket->lock);

		/* Only within a fast recovery with SACK. */
		if (endpoint->tcp.sack_on == 0 ||
		    endpoint->tcp.recovery == 0 ||
		    endpoint->tcp.recovery_by_timeout != 0) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return;
		}

		/*
		 * From the highest down: the bytes held above each segment,
		 * whether it is lost, what is in the network, and the lowest
		 * lost one not yet resent (the oldest passed by).
		 */
		mss = tcp_segment_size_locked(endpoint);
		threshold = (TCP_DUPLICATE_ACKS_FAST - 1U) * mss;
		sent = tcp_sent_count_locked(endpoint);
		held_above = 0;
		pipe = 0;
		chosen = NULL;
		position = sent;
		while (position > 0) {
			position--;
			entry = &endpoint->tcp.send_queue[(endpoint->tcp.send_first + position) % TCP_SEND_QUEUE_MAX];
			if (entry->sacked != 0) {
				held_above += entry->length;
				continue;
			}

			/* Lost when more than the threshold's bytes are held above it. */
			lost = held_above > threshold;

			/* A segment not lost is in the network once. */
			if (!lost)
				pipe += entry->advance;

			/* A resend is in the network once more. */
			if (entry->resent != 0)
				pipe += entry->advance;

			/* The lowest lost segment not yet resent is the next to go; the oldest is not. */
			if (lost &&
			    entry->resent == 0 &&
			    entry->length != 0 &&
			    position != 0)
				chosen = entry;
		}

		/* Nothing to resend, or no room for it. */
		if (chosen == NULL || pipe + chosen->length > endpoint->tcp.cwnd) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return;
		}

		/* Taken for the resend: marked, its copy held, the timing stopped (Karn). */
		chosen->resent = 1;
		chosen->resend_order = endpoint->tcp.resend_serial++;
		copy = chosen->packet;
		packet_buf_ref(copy);
		payload = copy->storage + chosen->payload_offset;
		sequence = chosen->sequence;
		length = chosen->length;
		flags = chosen->flags;
		endpoint->tcp.rtt_timing = 0;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Resends it, cut to the segment size. */
		error = tcp_send_segment_cut(endpoint, sequence, flags, payload, length);
		refusal = tcp_refusal_of(error);
		retries = tcp_refusal_retries(refusal);

		/* Counts the resend that went, and marks one the device or the pool refused for another try. */
		irq = spin_lock_irqsave(&socket->lock);

		/* A resend that went is counted. */
		if (error == 0) {
			endpoint->tcp.counters.retransmit_segments++;
			endpoint->tcp.counters.retransmit_bytes += length;
			endpoint->tcp.counters.immediate_resends++;
		}

		/* A refused one is marked to go again. */
		sent = tcp_sent_count_locked(endpoint);
		for (position = 0; retries && position < sent; position++) {
			entry = &endpoint->tcp.send_queue[(endpoint->tcp.send_first + position) % TCP_SEND_QUEUE_MAX];
			if (entry->sequence == sequence)
				entry->resent = 0;
		}

		spin_unlock_irqrestore(&socket->lock, irq);
		packet_buf_free(copy);

		/* A failed resend ends this round: the next acknowledgement tries again. */
		if (error != 0)
			return;
	}
}

/*
 * Reports the window a segment other than a SYN advertises, in bytes:
 * its window field shifted by the peer's scale once both ends offered it.
 * The caller holds the socket lock.
 */
static uint32_t
tcp_segment_window_locked(
	const struct tcp_endpoint *endpoint,
	const struct tcp_wire *tcp)
{
	uint32_t window;

	/* The field, unscaled without the option. */
	window = wire_get16(tcp->window);
	if (endpoint->tcp.wscale_on != 0)
		window <<= endpoint->tcp.peer_wscale;

	/* Succeeded: the window in bytes. */
	return window;
}

/*
 * Sets up what an established connection sends with: the segment size,
 * the smaller of the peer's offer (offered_mss; 0 for none) and what the
 * route's interface carries, and the congestion window it starts with
 * (RFC 6928).  Called without the socket lock, which the route lookup
 * must not be taken under.
 */
static void
tcp_start_sending(
	struct tcp_endpoint *endpoint,
	uint32_t offered_mss)
{
	struct net_device *device;
	struct in6_addr source6;
	unsigned long irq;
	uint32_t source;
	uint32_t limit;
	uint32_t mss;
	uint32_t window;
	int ipv6;
	int error;

	/* What the route's interface carries, no more than one packet holds (a standard one without a route). */
	limit = TCP_SEGMENT_MAX;
	ipv6 = inet_socket_speaks_ipv6(&endpoint->tcp.inet);
	if (ipv6) {
		error = tcp6_route(endpoint, &device, &source6);
		if (error == 0) {
			limit = tcp_path_mss(endpoint, device, 1);
			if (device != NULL)
				net_device_release(device);
		}
	} else {
		error = tcp_route(endpoint, &device, &source);
		if (error == 0) {
			limit = tcp_path_mss(endpoint, device, 0);
			net_device_release(device);
		}
	}

	/* The peer's offer, or the size every peer takes without one. */
	mss = offered_mss;
	if (mss == 0) {
		if (ipv6)
			mss = TCP_DEFAULT_MSS6;
		else
			mss = TCP_DEFAULT_MSS;
	}

	/* No larger than the path carries. */
	if (mss > limit)
		mss = limit;

	/* Ten segments to start, but no more than 14600 bytes unless two segments are larger (RFC 6928). */
	window = TCP_INITIAL_WINDOW_SEGMENTS * mss;
	if (window > TCP_INITIAL_WINDOW_BYTES)
		window = TCP_INITIAL_WINDOW_BYTES;
	if (window < 2U * mss)
		window = 2U * mss;

	/* Published under the lock, before the writers send. */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	endpoint->tcp.peer_mss = offered_mss;
	endpoint->tcp.send_mss = mss;
	endpoint->tcp.cwnd = window;
	endpoint->tcp.ssthresh = UINT32_MAX;
	endpoint->tcp.cwnd_acked = 0;

	/* The window's use is measured from the start of the sending. */
	tcp_cwnd_round_reset_locked(endpoint);

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
}

/*
 * Reports the largest segment payload a device carries: its MTU less the
 * IP and TCP headers, within the frame a packet holds for it
 * (net_device_frame_max) and the storage of the packet class that frame
 * takes.  A peer that is this host's own (local) goes round the loopback,
 * whose frames stay in standard packets.
 */
static uint32_t
tcp_device_mss(
	const struct net_device *device,
	int ipv6,
	int local)
{
	uint32_t headers;
	uint32_t limit;
	size_t frame;

	/* A standard packet's segment, unless the device's frames need jumbo ones. */
	if (ipv6)
		headers = TCP_HEADERS6;
	else
		headers = TCP_HEADERS;
	limit = TCP_SEGMENT_MAX;
	if (local || device == NULL)
		return limit;
	frame = net_device_frame_max(device);
	if (frame > PACKET_BUF_STORAGE_SIZE - PACKET_BUF_DEFAULT_HEADROOM)
		limit = TCP_SEGMENT_MAX_JUMBO;

	/* Within the frame, past the link header, and within the MTU. */
	if (frame > 14U + headers && frame - 14U - headers < limit)
		limit = (uint32_t)(frame - 14U - headers);
	if (device->mtu > headers && device->mtu - headers < limit)
		limit = device->mtu - headers;

	/* Succeeded: the device's segment size. */
	return limit;
}

/*
 * Reports the segment size of the way to the connection's peer through
 * device (over IPv6 NULL is the route's): tcp_device_mss, over IPv6 no
 * more than the path MTU less the headers (Packet Too Big lowers it),
 * and over IPv4 TCP_GATEWAY_MSS at most through a gateway.  Called without
 * the socket lock, which the route lookups must not be taken under.
 */
static uint32_t
tcp_path_mss(
	struct tcp_endpoint *endpoint,
	struct net_device *device,
	int ipv6)
{
	struct net_route route;
	struct in6_addr chosen;
	unsigned path_mtu;
	uint32_t limit;
	int gateway;
	int local;
	int error;

	/* Over IPv6: the device's, and the path's. */
	if (ipv6) {
		local = in6_is_loopback(&endpoint->tcp.inet.remote6) ||
		    ipv6_address_is_local(&endpoint->tcp.inet.remote6);
		limit = tcp_device_mss(device, 1, local);
		if (device == NULL && !local)
			limit = TCP_SEGMENT_MAX_JUMBO;
		path_mtu = 0;
		error = ipv6_route_source(device, &endpoint->tcp.inet.remote6, &chosen, &path_mtu);
		if (error == 0 && path_mtu > TCP_HEADERS6 && path_mtu - TCP_HEADERS6 < limit)
			limit = path_mtu - TCP_HEADERS6;
		return limit;
	}

	/* Over IPv4: the device's, and Ethernet's through a gateway. */
	local = inet_address_is_local(endpoint->tcp.inet.remote_address);
	limit = tcp_device_mss(device, 0, local);
	if (local || limit <= TCP_GATEWAY_MSS)
		return limit;
	gateway = 0;
	error = route_lookup_ref(endpoint->tcp.inet.remote_address, &route);
	if (error == 0) {
		gateway = route.gateway != 0;
		route_release(&route);
	}

	/* A peer through a gateway gets the size every router passes. */
	if (gateway)
		limit = TCP_GATEWAY_MSS;

	/* Succeeded: the path's segment size. */
	return limit;
}

/* Reports whether a ring's copy is a jumbo packet. */
static int
tcp_copy_is_jumbo(
	const struct packet_buf *packet)
{
	/* A packet larger than a standard one's storage. */
	return packet != NULL && packet->capacity > PACKET_BUF_STORAGE_SIZE;
}

/*
 * Reports the segment size this end's SYN offered, which the receive
 * window counts in, or the family's default before one went.
 */
static uint32_t
tcp_receive_mss(
	const struct tcp_endpoint *endpoint)
{
	int ipv6;

	/* Offered. */
	if (endpoint->tcp.receive_mss != 0)
		return endpoint->tcp.receive_mss;

	/* Not yet: the default of the connection's family. */
	ipv6 = inet_socket_speaks_ipv6(&endpoint->tcp.inet);
	if (ipv6)
		return TCP_RECEIVE_MSS6;

	/* Succeeded: the IPv4 default. */
	return TCP_RECEIVE_MSS;
}

/*
 * Reports the connection's segment size, or the default for a peer that
 * offered none while it is not yet set.  The caller holds the socket lock.
 */
static uint32_t
tcp_segment_size_locked(
	const struct tcp_endpoint *endpoint)
{
	/* Not set before the connection is established. */
	if (endpoint->tcp.send_mss == 0)
		return TCP_DEFAULT_MSS;

	/* Succeeded: the size set at establishment. */
	return endpoint->tcp.send_mss;
}

/*
 * Reports how much may be in flight: the peer's window (an unknown or
 * closed one lets one segment go) and the congestion window, whichever is
 * smaller.  The caller holds the socket lock.
 */
static uint32_t
tcp_send_limit_locked(
	const struct tcp_endpoint *endpoint)
{
	uint32_t limit;

	/* The peer's window, one segment when it is closed or not known. */
	limit = endpoint->tcp.peer_window;
	if (limit == 0)
		limit = tcp_segment_size_locked(endpoint);

	/* The congestion window, once the connection has one. */
	if (endpoint->tcp.cwnd != 0 && endpoint->tcp.cwnd < limit)
		limit = endpoint->tcp.cwnd;

	/* Succeeded: the smaller limit. */
	return limit;
}

/*
 * Tells whether a data segment of length bytes may join what is in flight:
 * always with nothing in flight, else within the send limit.  The caller
 * holds the socket lock.
 */
static int
tcp_send_room_locked(
	const struct tcp_endpoint *endpoint,
	size_t length)
{
	uint32_t in_flight;
	uint32_t limit;

	/* One segment may always go. */
	in_flight = tcp_in_flight(endpoint);
	if (in_flight == 0)
		return 1;

	/* Others while they stay within the limit. */
	limit = tcp_send_limit_locked(endpoint);
	if ((uint64_t)in_flight + length > limit)
		return 0;

	/* Succeeded: it fits. */
	return 1;
}

/*
 * Tells whether a data segment of length bytes fits the socket's send
 * buffer with the payload the ring holds: always into a ring without
 * data, so a buffer smaller than a segment still sends.  The caller holds
 * the socket lock.
 */
static int
tcp_send_buffer_room_locked(
	const struct tcp_endpoint *endpoint,
	size_t length)
{
	size_t limit;

	/* One segment may always be queued. */
	if (endpoint->tcp.send_bytes == 0)
		return 1;

	/* Others while they stay within the buffer. */
	limit = endpoint->tcp.inet.socket.send_hiwat_bytes;
	if (endpoint->tcp.send_bytes + length > limit)
		return 0;

	/* Succeeded: it fits. */
	return 1;
}

/*
 * Tells whether Nagle's algorithm (RFC 9293 section 3.7.4, with
 * Minshall's change as Linux has it) holds the ring's last entry back
 * from the device: an unsent data segment shorter than the connection's
 * segment size, while a short segment sent before it is not yet
 * acknowledged and TCP_NODELAY is not set.  Full segments in flight hold
 * nothing back, so the last short piece of a transfer goes at once and
 * the acknowledgement of a full flight is never all it waits for.  It
 * goes once that short segment is acknowledged (tcp_retire_acknowledged
 * has the output retry send it), once later writes have filled it or
 * queued behind it, or once the option is set.  The caller holds the
 * socket lock.
 */
static int
tcp_nagle_holds_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *entry;
	unsigned slot;
	uint32_t sent_end;
	uint32_t segment_size;
	int at_or_before;

	/* Never with the option set, or with no unsent entry. */
	if (endpoint->tcp.nodelay != 0 || endpoint->tcp.send_unsent == 0)
		return 0;

	/*
	 * Never while no short segment sent is outstanding; one the device
	 * refused, unsent again, is not (it may be this entry itself).
	 */
	if (endpoint->tcp.send_small_outstanding == 0)
		return 0;
	sent_end = tcp_sent_end_locked(endpoint);
	at_or_before = tcp_sequence_at_or_before(endpoint->tcp.send_small_end, sent_end);
	if (!at_or_before)
		return 0;

	/* The last entry: a data segment shorter than a full one. */
	slot = (endpoint->tcp.send_first + endpoint->tcp.send_count - 1U) % TCP_SEND_QUEUE_MAX;
	entry = &endpoint->tcp.send_queue[slot];
	if (entry->length == 0 || (entry->flags & (TCP_SYN | TCP_FIN)) != 0)
		return 0;
	segment_size = tcp_segment_size_locked(endpoint);
	if (entry->length >= segment_size)
		return 0;

	/* Succeeded: it waits. */
	return 1;
}

/*
 * Tells whether the first unsent entry may go within the congestion
 * window with what was sent before it: always with nothing sent
 * outstanding, a control segment, or before the connection has a window.
 * Entries were queued within the window, so this holds back only those a
 * timeout made unsent again (its go-back-N goes in slow start) or a
 * window a loss cut since.  The caller holds the socket lock.
 */
static int
tcp_unsent_fits_cwnd_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *entry;
	uint32_t flight;
	unsigned slot;

	/* Nothing waits, or no window yet. */
	if (endpoint->tcp.send_unsent == 0 || endpoint->tcp.cwnd == 0)
		return 1;

	/* The first unsent entry: a data segment. */
	slot = (endpoint->tcp.send_first + endpoint->tcp.send_count - endpoint->tcp.send_unsent) % TCP_SEND_QUEUE_MAX;
	entry = &endpoint->tcp.send_queue[slot];
	if (entry->length == 0)
		return 1;

	/* Within the window with what is in flight, or alone. */
	flight = tcp_sent_end_locked(endpoint) - endpoint->tcp.send_unacknowledged;
	if (flight == 0 || (uint64_t)flight + entry->length <= endpoint->tcp.cwnd)
		return 1;

	/* Succeeded: it waits. */
	return 0;
}

/*
 * Reports how many bytes of a write the ring's last entry still takes in
 * place of a new segment: one never handed to the device, a data segment
 * shorter than the connection's segment size, up to that size and the
 * room after its payload in its copy; 0 when there is no such entry.  So
 * a short write that Nagle's algorithm holds, or that waits behind a
 * refused one, is filled by the next writes rather than followed by more
 * short segments.  The caller holds the socket lock.
 */
static size_t
tcp_tail_room_locked(
	const struct tcp_endpoint *endpoint)
{
	const struct tcp_pending *entry;
	unsigned slot;
	size_t room;
	size_t tailroom;
	uint32_t segment;

	/* An unsent last entry. */
	if (endpoint->tcp.send_unsent == 0)
		return 0;
	slot = (endpoint->tcp.send_first + endpoint->tcp.send_count - 1U) % TCP_SEND_QUEUE_MAX;
	entry = &endpoint->tcp.send_queue[slot];

	/* Never sent, and a data segment. */
	if (entry->fresh == 0 || entry->length == 0 || (entry->flags & (TCP_SYN | TCP_FIN)) != 0)
		return 0;

	/* Short of the segment size, within its copy's room. */
	segment = tcp_segment_size_locked(endpoint);
	if (entry->length >= segment)
		return 0;
	room = segment - entry->length;
	tailroom = packet_buf_tailroom(entry->packet);
	if (room > tailroom)
		room = tailroom;

	/* Succeeded: the bytes it takes. */
	return room;
}

/*
 * Tells whether a writer waiting for room is woken now: when nothing is
 * left in flight, or when half the ring's entries, half the send buffer
 * and a quarter of the send limit are free (a low-water mark, as
 * SO_SNDLOWAT), so that a write
 * queues many segments rather than the one or two an acknowledgement
 * frees, and the system call's 64 KB pieces of a write are taken whole
 * rather than copied in for a part of them.  The caller holds the socket
 * lock.
 */
static int
tcp_send_wake_due_locked(
	const struct tcp_endpoint *endpoint)
{
	uint32_t in_flight;
	uint32_t limit;
	unsigned ring_free;

	/* An empty ring always wakes. */
	if (endpoint->tcp.send_count == 0)
		return 1;

	/* Too few free entries, or too little of the send buffer free. */
	ring_free = TCP_SEND_QUEUE_MAX - endpoint->tcp.send_count;
	if (ring_free < (TCP_SEND_QUEUE_MAX + 1U) / 2U)
		return 0;
	if (endpoint->tcp.send_bytes > endpoint->tcp.inet.socket.send_hiwat_bytes / 2U)
		return 0;

	/* Too little of the limit free. */
	in_flight = tcp_in_flight(endpoint);
	limit = tcp_send_limit_locked(endpoint);
	if (in_flight < limit && limit - in_flight >= limit / 4U)
		return 1;

	/* Not yet. */
	return 0;
}

/*
 * Grows the congestion window for bytes newly acknowledged outside a
 * recovery (RFC 5681 section 3.1): in slow start by what was taken, no
 * more than two segments an acknowledgement (RFC 3465's L, for a peer that
 * acknowledges every second segment); past the threshold by one segment a
 * window's worth of acknowledged bytes.  Only a window the sending uses
 * grows (RFC 7661): one the peer's window or the writer keeps from being
 * filled stays as it is, and its acknowledgements count for nothing.  The
 * caller holds the socket lock.
 */
static void
tcp_cwnd_grow_locked(
	struct tcp_endpoint *endpoint,
	uint32_t acked)
{
	uint32_t mss;
	uint32_t step;
	int limited;

	/* A connection not yet sending has no window to grow. */
	if (endpoint->tcp.cwnd == 0)
		return;
	mss = tcp_segment_size_locked(endpoint);

	/*
	 * A window the sending leaves unused does not grow.  CUBIC's epoch
	 * notes from when, so that the time does not count in it (RFC 9438
	 * section 4.2).
	 */
	limited = tcp_cwnd_limited_locked(endpoint, mss);
	if (!limited) {
		/* Notes the start of the unused time once, in a running CUBIC epoch. */
		if (endpoint->tcp.cc == TCP_CC_CUBIC &&
		    endpoint->tcp.cubic_epoch_started != 0 &&
		    endpoint->tcp.cubic_limited_since_ms == 0)
			endpoint->tcp.cubic_limited_since_ms = tcp_cc_now_ms();
		return;
	}

	/* Slow start. */
	if (endpoint->tcp.cwnd < endpoint->tcp.ssthresh) {
		step = acked;
		if (step > 2U * mss)
			step = 2U * mss;
		if (endpoint->tcp.cwnd <= TCP_CWND_MAX - step)
			endpoint->tcp.cwnd += step;
		return;
	}

	/* Congestion avoidance: CUBIC's window function. */
	if (endpoint->tcp.cc == TCP_CC_CUBIC) {
		tcp_cubic_grow_locked(endpoint, acked, mss);
		return;
	}

	/* Reno's: a segment for each window acknowledged. */
	endpoint->tcp.cwnd_acked += acked;
	if (endpoint->tcp.cwnd_acked < endpoint->tcp.cwnd)
		return;
	endpoint->tcp.cwnd_acked -= endpoint->tcp.cwnd;
	if (endpoint->tcp.cwnd <= TCP_CWND_MAX - mss)
		endpoint->tcp.cwnd += mss;
}

/*
 * Takes a loss into the congestion window (RFC 5681 sections 3.1 and
 * 3.2): the threshold becomes half of what was in flight, at least two
 * segments; the window becomes the threshold after a fast retransmit, one
 * segment after a timeout.  A timeout repeated within the recovery an
 * earlier timeout started (what it resends the timer has resent already)
 * holds the threshold and only takes the window to one segment.  The
 * caller holds the socket lock.
 */
static void
tcp_cwnd_loss_locked(
	struct tcp_endpoint *endpoint,
	int timeout,
	int repeated)
{
	uint32_t flight;
	uint32_t mss;
	uint32_t threshold;

	/* A connection not yet sending has no window to cut. */
	if (endpoint->tcp.cwnd == 0)
		return;
	mss = tcp_segment_size_locked(endpoint);

	/*
	 * A repeated timeout: the threshold is held constant (RFC 5681
	 * section 3.1, the segment having been resent by way of the timer).
	 */
	if (timeout && repeated) {
		endpoint->tcp.cwnd = mss;
		endpoint->tcp.cwnd_acked = 0;
		return;
	}

	/* What was sent and not acknowledged, which CUBIC cuts its own way. */
	flight = tcp_sent_end_locked(endpoint) - endpoint->tcp.send_unacknowledged;
	if (endpoint->tcp.cc == TCP_CC_CUBIC) {
		tcp_cubic_loss_locked(endpoint, flight, timeout);
		return;
	}

	/* Reno's: half of it, at least two segments. */
	threshold = flight / 2U;
	if (threshold < 2U * mss)
		threshold = 2U * mss;
	endpoint->tcp.ssthresh = threshold;

	/* The window to go on with. */
	if (timeout)
		endpoint->tcp.cwnd = mss;
	else
		endpoint->tcp.cwnd = threshold;
	endpoint->tcp.cwnd_acked = 0;
}

/*
 * Starts the measure of whether the congestion window is in use afresh,
 * where the window is set apart from its growth (the start of the
 * sending, the end of a recovery, a timeout's go-back-N, a smaller
 * segment size, another congestion control): a round to the end of what
 * was sent, with what is in flight now and nothing before it, and no
 * CUBIC epoch.  The caller holds the socket lock.
 */
static void
tcp_cwnd_round_reset_locked(
	struct tcp_endpoint *endpoint)
{
	uint32_t sent_end;

	/* A round to the end of what was sent, holding what is in flight. */
	sent_end = tcp_sent_end_locked(endpoint);
	endpoint->tcp.cwnd_round_end = sent_end;
	endpoint->tcp.cwnd_round_flight = sent_end - endpoint->tcp.send_unacknowledged;
	endpoint->tcp.cwnd_prev_flight = 0;
	endpoint->tcp.cwnd_blocked = 0;

	/* CUBIC's epoch starts again at the next acknowledgement of congestion avoidance. */
	endpoint->tcp.cubic_epoch_started = 0;
	endpoint->tcp.cubic_limited_since_ms = 0;
}

/*
 * Takes a segment just handed to the device into the round's measure:
 * the most sent and not acknowledged so far in the round.  The caller
 * holds the socket lock.
 */
static void
tcp_cwnd_note_sent_locked(
	struct tcp_endpoint *endpoint)
{
	uint32_t flight;

	/* What was sent and not acknowledged, with this segment. */
	flight = tcp_sent_end_locked(endpoint) - endpoint->tcp.send_unacknowledged;
	if (flight > endpoint->tcp.cwnd_round_flight)
		endpoint->tcp.cwnd_round_flight = flight;
}

/*
 * Tells whether the congestion window is in use, so that it may grow: it
 * held back a queued segment in this round; or, with the most sent and
 * not acknowledged in this round or the one before, in slow start it is
 * less than twice that, and past the threshold within a segment of it.
 * The caller holds the socket lock.
 *
 * Procedure referenced:
 * RFC 7661 section 4.2 and RFC 9438 section 5.8 (no growth while the
 * window is not used), and Linux 6.12 include/net/tcp.h
 * (tcp_is_cwnd_limited: in slow start, twice the most in flight).
 */
static int
tcp_cwnd_limited_locked(
	const struct tcp_endpoint *endpoint,
	uint32_t mss)
{
	uint32_t flight;

	/* A queued segment the window held back. */
	if (endpoint->tcp.cwnd_blocked != 0)
		return 1;

	/* The most in flight in this round or the one before. */
	flight = endpoint->tcp.cwnd_round_flight;
	if (endpoint->tcp.cwnd_prev_flight > flight)
		flight = endpoint->tcp.cwnd_prev_flight;

	/* Slow start: less than twice that. */
	if (endpoint->tcp.cwnd < endpoint->tcp.ssthresh) {
		/* The window is less than twice the most in flight: it is in use. */
		if ((uint64_t)endpoint->tcp.cwnd < 2ULL * flight)
			return 1;

		/* The window is twice that or more: it is not in use. */
		return 0;
	}

	/* Congestion avoidance: within a segment of it. */
	if ((uint64_t)flight + mss >= endpoint->tcp.cwnd)
		return 1;

	/* Not in use. */
	return 0;
}

/*
 * Ends the round once the acknowledgements reach its end: its most in
 * flight becomes the round before's, and a new round runs to the end of
 * what is sent now, holding what is in flight now.  The caller holds the
 * socket lock.
 */
static void
tcp_cwnd_round_advance_locked(
	struct tcp_endpoint *endpoint)
{
	uint32_t sent_end;
	int reached;

	/* Not before the acknowledgements reach the round's end. */
	reached = tcp_sequence_at_or_before(endpoint->tcp.cwnd_round_end, endpoint->tcp.send_unacknowledged);
	if (!reached)
		return;

	/* The round's measure moves back one, and the next round starts. */
	sent_end = tcp_sent_end_locked(endpoint);
	endpoint->tcp.cwnd_prev_flight = endpoint->tcp.cwnd_round_flight;
	endpoint->tcp.cwnd_round_flight = sent_end - endpoint->tcp.send_unacknowledged;
	endpoint->tcp.cwnd_round_end = sent_end;
	endpoint->tcp.cwnd_blocked = 0;
}

/* Gives the time CUBIC counts in: the kernel's ticks in milliseconds. */
static uint64_t
tcp_cc_now_ms(
	void)
{
	uint64_t ticks;
	uint64_t now;

	/* The ticks since boot, in milliseconds. */
	ticks = sched_ticks();
	now = kern_ticks_to_ms(ticks);
	return now;
}

/*
 * Grows the window in congestion avoidance as CUBIC does (RFC 9438
 * sections 4.2 to 4.5), for acked bytes newly acknowledged while the
 * window is in use.  An epoch starts at the first such acknowledgement:
 * K and the origin from the window before the last loss (K = 0 at the
 * window found after a timeout, section 4.8), W_est at the window.  The
 * time the window went unused since moves the epoch on.  The window goes
 * towards W_cubic one round trip ahead, at most half again a round trip,
 * and never below W_est while W_cubic is.  The caller holds the socket
 * lock.
 */
static void
tcp_cubic_grow_locked(
	struct tcp_endpoint *endpoint,
	uint32_t acked,
	uint32_t mss)
{
	uint64_t now;
	uint64_t elapsed;
	uint64_t rtt_ms;
	uint64_t target;
	uint64_t ceiling;
	uint64_t growth;
	uint64_t estimate;
	uint64_t share;
	uint32_t cwnd;
	uint32_t w_cubic;

	/* Reads the time and the window the epoch is measured from. */
	now = tcp_cc_now_ms();
	cwnd = endpoint->tcp.cwnd;

	/* Starts the epoch, when none runs. */
	if (endpoint->tcp.cubic_epoch_started == 0) {
		endpoint->tcp.cubic_epoch_started = 1;
		endpoint->tcp.cubic_epoch_ms = now;
		endpoint->tcp.cubic_limited_since_ms = 0;
		endpoint->tcp.cubic_w_est = cwnd;
		endpoint->tcp.cubic_est_rem = 0;
		endpoint->tcp.cubic_grow_rem = 0;
		endpoint->tcp.cwnd_acked = 0;

		/* K and the origin: the window found after a timeout, or the window before the last loss. */
		if (endpoint->tcp.cubic_after_timeout != 0) {
			endpoint->tcp.cubic_k_ms = 0;
			endpoint->tcp.cubic_origin = cwnd;
			endpoint->tcp.cubic_w_max = cwnd;

			/* The timeout's mark ends once no recovery runs. */
			if (endpoint->tcp.recovery == 0)
				endpoint->tcp.cubic_after_timeout = 0;
		} else if (cwnd < endpoint->tcp.cubic_w_max) {
			endpoint->tcp.cubic_k_ms = tcp_cubic_k_ms(endpoint->tcp.cubic_w_max, cwnd, mss);
			endpoint->tcp.cubic_origin = endpoint->tcp.cubic_w_max;
		} else {
			endpoint->tcp.cubic_k_ms = 0;
			endpoint->tcp.cubic_origin = cwnd;
		}
	}

	/* The time the window went unused does not count in the epoch (section 4.2). */
	if (endpoint->tcp.cubic_limited_since_ms != 0) {
		/* Moves the epoch on by the time since, unless the clock is behind it. */
		if (now > endpoint->tcp.cubic_limited_since_ms)
			endpoint->tcp.cubic_epoch_ms += now - endpoint->tcp.cubic_limited_since_ms;
		endpoint->tcp.cubic_limited_since_ms = 0;
	}

	/* The time into the epoch, none when the clock is behind its start. */
	elapsed = 0;
	if (now > endpoint->tcp.cubic_epoch_ms)
		elapsed = now - endpoint->tcp.cubic_epoch_ms;

	/* The round trip, none before a sample. */
	rtt_ms = 0;
	if (endpoint->tcp.rtt_samples != 0)
		rtt_ms = endpoint->tcp.srtt_us / 1000U;

	/* The target: W_cubic a round trip ahead, within the window and half again (section 4.4). */
	target = tcp_cubic_window(endpoint->tcp.cubic_origin, endpoint->tcp.cubic_k_ms, elapsed + rtt_ms, mss);
	ceiling = (uint64_t)cwnd + cwnd / 2U;
	if (target > ceiling)
		target = ceiling;

	/*
	 * Towards it: (target - cwnd) / cwnd of the bytes acknowledged, a
	 * segment of growth for each segment of them, the remainder carried
	 * (section 4.5).
	 */
	if (target > cwnd) {
		growth = (uint64_t)acked * (target - cwnd) + endpoint->tcp.cubic_grow_rem;
		endpoint->tcp.cwnd_acked += (uint32_t)(growth / cwnd);
		endpoint->tcp.cubic_grow_rem = (uint32_t)(growth % cwnd);
	}

	/* The segments of growth that came. */
	while (endpoint->tcp.cwnd_acked >= mss) {
		endpoint->tcp.cwnd_acked -= mss;

		/* A segment is added unless the window would pass its largest. */
		if (endpoint->tcp.cwnd <= TCP_CWND_MAX - mss)
			endpoint->tcp.cwnd += mss;
	}

	/*
	 * Value referenced:
	 * RFC 9438 section 4.3: alpha = 3 (1 - beta) / (1 + beta), 0.529
	 * with beta = 0.7, taken as 529 thousandths; 1 once W_est reaches
	 * the window before the loss.
	 */
	share = 529U;
	if (endpoint->tcp.cubic_w_est >= endpoint->tcp.cubic_cwnd_prior)
		share = 1000U;

	/* W_est grows by alpha segments a window acknowledged, the remainder carried. */
	estimate = share * acked * mss + endpoint->tcp.cubic_est_rem;
	endpoint->tcp.cubic_w_est += (uint32_t)(estimate / (1000ULL * cwnd));
	endpoint->tcp.cubic_est_rem = (uint32_t)(estimate % (1000ULL * cwnd));

	/* W_est stays within the largest window. */
	if (endpoint->tcp.cubic_w_est > TCP_CWND_MAX)
		endpoint->tcp.cubic_w_est = TCP_CWND_MAX;

	/* Where W_cubic now (no round trip ahead) is below W_est, the window is at least W_est (section 4.3). */
	w_cubic = tcp_cubic_window(endpoint->tcp.cubic_origin, endpoint->tcp.cubic_k_ms, elapsed, mss);
	if (w_cubic < endpoint->tcp.cubic_w_est && endpoint->tcp.cwnd < endpoint->tcp.cubic_w_est)
		endpoint->tcp.cwnd = endpoint->tcp.cubic_w_est;
}

/*
 * Takes a loss into CUBIC's window (RFC 9438 sections 4.6 to 4.8): the
 * window before it becomes W_max, less when it is below the one before
 * (fast convergence), and cwnd_prior; the threshold becomes beta of what
 * was in flight, at least two segments; the window becomes the threshold
 * after a fast retransmit, one segment after a timeout, whose next epoch
 * starts at the window it finds.  The epoch ends.  The caller holds the
 * socket lock.
 */
static void
tcp_cubic_loss_locked(
	struct tcp_endpoint *endpoint,
	uint32_t flight,
	int timeout)
{
	uint64_t threshold;
	uint32_t mss;
	uint32_t cwnd;

	/* The segment size and the window the loss finds. */
	mss = tcp_segment_size_locked(endpoint);
	cwnd = endpoint->tcp.cwnd;

	/*
	 * Value referenced:
	 * RFC 9438 section 4.7: below the last W_max, W_max = cwnd (1 + beta)
	 * / 2, 17 / 20 with beta = 0.7.
	 */
	if (cwnd < endpoint->tcp.cubic_w_max)
		endpoint->tcp.cubic_w_max = (uint32_t)((uint64_t)cwnd * 17U / 20U);
	else
		endpoint->tcp.cubic_w_max = cwnd;
	endpoint->tcp.cubic_cwnd_prior = cwnd;

	/*
	 * Value referenced:
	 * RFC 9438 section 4.6: ssthresh = flight_size beta, beta = 0.7, at
	 * least two segments.
	 */
	threshold = (uint64_t)flight * 7U / 10U;
	if (threshold < 2ULL * mss)
		threshold = 2ULL * mss;
	endpoint->tcp.ssthresh = (uint32_t)threshold;

	/* The window to go on with, and the epoch ended. */
	if (timeout) {
		endpoint->tcp.cwnd = mss;
		endpoint->tcp.cubic_after_timeout = 1;
	} else {
		endpoint->tcp.cwnd = (uint32_t)threshold;
		endpoint->tcp.cubic_after_timeout = 0;
	}

	/* The growth carried and the epoch's measures start over. */
	endpoint->tcp.cwnd_acked = 0;
	endpoint->tcp.cubic_epoch_started = 0;
	endpoint->tcp.cubic_limited_since_ms = 0;
}

/*
 * Has a connection use another congestion control: the window and the
 * threshold stay, the measures of the one before go (its growth towards
 * the next segment, the round of the window's use, CUBIC's epoch and its
 * memory of the last loss).  The caller holds the socket lock.
 */
static void
tcp_cc_set_locked(
	struct tcp_endpoint *endpoint,
	unsigned cc)
{
	/* The same control: nothing to start again. */
	if (endpoint->tcp.cc == cc)
		return;

	/* The new one, with nothing of the old one's. */
	endpoint->tcp.cc = cc;
	endpoint->tcp.cwnd_acked = 0;
	endpoint->tcp.cubic_w_max = 0;
	endpoint->tcp.cubic_cwnd_prior = 0;
	endpoint->tcp.cubic_after_timeout = 0;

	/* The window's use is measured afresh for it. */
	tcp_cwnd_round_reset_locked(endpoint);
}

/*
 * Owes the peer an acknowledgement of one more in-order segment: the
 * second sends it at once (RFC 5681 section 4.2); a first one waits on the
 * list of delayed acknowledgements, which the network worker flushes when
 * it has taken the input that had arrived.
 */
static void
tcp_ack_delay(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket;
	unsigned long irq;
	unsigned owed;
	int listing;
	int referenced;

	/* One more owed, and whether this call puts the connection on the list. */
	socket = &endpoint->tcp.inet.socket;
	irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.ack_owed++;
	owed = endpoint->tcp.ack_owed;
	listing = 0;
	if (owed < TCP_ACK_SEGMENTS && endpoint->tcp.ack_listed == 0) {
		endpoint->tcp.ack_listed = 1;
		listing = 1;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/* The second segment (or the TCP_ACK_SEGMENTS-th) is acknowledged now. */
	if (owed >= TCP_ACK_SEGMENTS) {
		(void)tcp_send_ack(endpoint, 0);
		return;
	}

	/* Already on the list: the flush acknowledges this one too. */
	if (!listing)
		return;

	/* A socket that is closing gets its acknowledgement now rather than a reference. */
	referenced = socket_tryref(socket);
	if (!referenced) {
		irq = spin_lock_irqsave(&socket->lock);

		endpoint->tcp.ack_listed = 0;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Acknowledges now, without the reference the socket would not give. */
		(void)tcp_send_ack(endpoint, 0);
		return;
	}

	/* On the list, with the reference the flush drops. */
	irq = spin_lock_irqsave(&tcp_ack_lock);

	endpoint->tcp.ack_next = tcp_ack_head;
	tcp_ack_head = &endpoint->tcp;

	spin_unlock_irqrestore(&tcp_ack_lock, irq);
}

/*
 * Sends the delayed acknowledgements still owed, and empties the list.
 */
void
tcp_ack_flush(
	void)
{
	struct tcp_socket *list;
	struct tcp_socket *next;
	struct tcp_endpoint *endpoint;
	struct socket *socket;
	unsigned long irq;
	unsigned owed;

	/* Takes the whole list. */
	irq = spin_lock_irqsave(&tcp_ack_lock);

	list = tcp_ack_head;
	tcp_ack_head = NULL;

	spin_unlock_irqrestore(&tcp_ack_lock, irq);

	/* Each connection still owing one sends it; one that sent anything since owes none. */
	while (list != NULL) {
		next = list->ack_next;
		socket = &list->inet.socket;
		endpoint = tcp_endpoint(socket);
		irq = spin_lock_irqsave(&socket->lock);

		owed = endpoint->tcp.ack_owed;
		endpoint->tcp.ack_listed = 0;
		endpoint->tcp.ack_next = NULL;
		if (endpoint->tcp.state == TCP_CLOSED)
			owed = 0;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Sends the acknowledgement owed, if the connection still owes one. */
		if (owed != 0)
			(void)tcp_send_ack(endpoint, 0);
		socket_release(socket);
		list = next;
	}
}

/*
 * Retires every segment the acknowledgement covers.
 *
 * The caller holds the socket lock and frees the returned payloads once it
 * has released it.  The deadline restarts for whatever is still outstanding.
 */
static void
tcp_retire_acknowledged(
	struct tcp_endpoint *endpoint,
	uint32_t acknowledgement,
	struct tcp_discard *discard)
{
	struct tcp_pending *entry;
	uint32_t end;
	unsigned sent;
	uint64_t now;
	uint64_t rto;
	uint64_t timeout;
	int progressed = 0;
	int recovering;
	int ended;
	uint32_t acked = 0;
	int probe_answered = 0;
	int going_back;
	int beyond;
	int covered;
	int copy_jumbo;
	int small_acked;
	int before;
	int oldest_resent;

	/*
	 * Takes the covered entries off the front of the ring.  Only an entry
	 * handed to the device can be covered (an acknowledgement past what was
	 * sent is refused before this), so the unsent ones are never taken.
	 */
	for (;;) {
		/* Stops when no entry sent is left. */
		sent = tcp_sent_count_locked(endpoint);
		if (sent == 0)
			break;

		/* The oldest entry, and the sequence number after it. */
		entry = &endpoint->tcp.send_queue[endpoint->tcp.send_first];
		end = entry->sequence + entry->advance;

		/*
		 * Stops at the first segment the peer has not taken, compared
		 * modulo 2^32 so that it stays correct where the numbers wrap.
		 */
		before = tcp_sequence_before(acknowledgement, end);
		if (before)
			break;

		/* A probe answered ends the backoff of the closed window (review-1 M3). */
		if (entry->probe != 0)
			probe_answered = 1;

		/* The sequence space taken, which TCP_MAXUNACKTIME counts as progress. */
		acked += entry->advance;

		/* Hands its copy to the caller to free, and takes it off the ring. */
		copy_jumbo = tcp_copy_is_jumbo(entry->packet);
		if (copy_jumbo && endpoint->tcp.send_jumbo != 0)
			endpoint->tcp.send_jumbo--;
		endpoint->tcp.send_bytes -= entry->length;
		discard->packets[discard->count++] = entry->packet;
		entry->packet = NULL;
		endpoint->tcp.send_first =
		    (endpoint->tcp.send_first + 1U) % TCP_SEND_QUEUE_MAX;
		endpoint->tcp.send_count--;
		progressed = 1;
	}

	/*
	 * An acknowledgement that takes nothing (a duplicate, which a peer
	 * sends for each segment after a gap, or the answer of a peer whose
	 * window is closed) leaves the timer as it is (RFC 6298 section 5.3):
	 * restarting it would put the resend of the lost segment off for as
	 * long as duplicates come.  The peer answered, so the attempts start
	 * over: a connection is not given up while its peer answers.  The
	 * backoff stays (ws220-p004 review-1 G2): the resends to a closed
	 * window keep doubling apart, and only a sample ends the backoff.
	 */
	if (!progressed) {
		endpoint->tcp.retransmit_count = 0;
		endpoint->tcp.retransmit_since = 0;
		endpoint->tcp.retransmit_refused = 0;
		return;
	}

	/*
	 * An acknowledgement that stops at a segment the peer reported held
	 * says the peer dropped it after all (it reneged, RFC 2018 section
	 * 8): the marks no longer say what the peer has.
	 */
	sent = tcp_sent_count_locked(endpoint);
	if (sent != 0 && endpoint->tcp.send_queue[endpoint->tcp.send_first].sacked != 0)
		tcp_sack_clear_locked(endpoint, 1);

	/*
	 * With the short segment sent before it acknowledged, a short last
	 * entry that Nagle's algorithm held may go.
	 */
	if (endpoint->tcp.send_small_outstanding != 0) {
		small_acked = tcp_sequence_at_or_before(endpoint->tcp.send_small_end, acknowledgement);
		if (small_acked)
			endpoint->tcp.send_small_outstanding = 0;
	}

	/*
	 * Unsent entries the congestion window held back (after a timeout's
	 * go-back-N, or a window cut by a loss) or Nagle's algorithm held go
	 * as far as the room this acknowledgement made: the network worker,
	 * which takes it, sends them on its next pass.
	 */
	if (endpoint->tcp.send_unsent != 0) {
		endpoint->tcp.output_retry_on_pass = 1;
		if (endpoint->tcp.output_retry_deadline == 0)
			endpoint->tcp.output_retry_deadline = sched_ticks();
	}

	/*
	 * The timer follows whatever is now the oldest segment sent; with
	 * none, an unsent one starts it when it goes (ws220-p003).  In a
	 * recovery (RFC 6582 section 3.2), an acknowledgement short of recover
	 * is a partial one: the next hole is resent at once rather than after
	 * another timeout; one that reaches recover ends the recovery.  The
	 * duplicates in a row start over (ws220-p005).
	 */
	recovering = 0;
	if (endpoint->tcp.recovery != 0)
		recovering = tcp_sequence_before(acknowledgement, endpoint->tcp.recover);

	/*
	 * A recovery a timeout started that ends exactly at recover holds back
	 * the duplicates of recover (review-1 M1); one beyond it lets them go.
	 */
	if (endpoint->tcp.dupack_guard != 0) {
		beyond = tcp_sequence_before(endpoint->tcp.dupack_guard_sequence, acknowledgement);
		if (beyond)
			endpoint->tcp.dupack_guard = 0;
	}

	/* The recovery ends here; one a timeout started, exactly at recover, guards its duplicates. */
	ended = 0;
	if (endpoint->tcp.recovery != 0 && !recovering) {
		ended = 1;
		if (endpoint->tcp.recovery_by_timeout != 0 && acknowledgement == endpoint->tcp.recover) {
			endpoint->tcp.dupack_guard = 1;
			endpoint->tcp.dupack_guard_sequence = acknowledgement;
		}

		/* Out of the recovery: recover means nothing until the next one (review-1 G2). */
		endpoint->tcp.recovery = 0;
		endpoint->tcp.recovery_by_timeout = 0;

		/* The resends of this recovery are not those of the next. */
		tcp_sack_clear_locked(endpoint, 0);
	}

	/* The backoff of a closed window ends when its probe is answered, and the persist state with it. */
	if (probe_answered) {
		endpoint->tcp.rto_backoff = 0;
		tcp_persist_leave_locked(endpoint);
	}

	/*
	 * An acknowledgement that took more than a byte is progress for
	 * TCP_MAXUNACKTIME (as FreeBSD counts it); with nothing left
	 * waiting, the time stops.
	 */
	if (endpoint->tcp.send_count == 0) {
		endpoint->tcp.unack_since = 0;
	} else if (acked > 1U) {
		endpoint->tcp.unack_since = sched_ticks();
	}

	/*
	 * The congestion window grows with what the acknowledgement took
	 * (RFC 5681 section 3.1), but not in a recovery; one that ends here
	 * leaves it at the threshold set when the loss was found (RFC 6582
	 * section 3.2 step 3, this end not inflating it meanwhile).
	 */
	going_back = recovering && endpoint->tcp.recovery_by_timeout == 1U;
	if (ended) {
		if (endpoint->tcp.cwnd > endpoint->tcp.ssthresh)
			endpoint->tcp.cwnd = endpoint->tcp.ssthresh;
		endpoint->tcp.cwnd_acked = 0;

		/* The window's use is measured afresh after the recovery. */
		tcp_cwnd_round_reset_locked(endpoint);
	} else if (going_back || (!recovering && endpoint->tcp.recovery == 0)) {
		tcp_cwnd_grow_locked(endpoint, acked);
	}

	/* The round of the window's use ends once this acknowledgement reaches its end. */
	tcp_cwnd_round_advance_locked(endpoint);

	/* The duplicates and the resends of the oldest start over, and the peer has answered. */
	endpoint->tcp.duplicate_acks = 0;
	endpoint->tcp.retransmit_count = 0;
	endpoint->tcp.retransmit_since = 0;
	endpoint->tcp.retransmit_refused = 0;
	sent = tcp_sent_count_locked(endpoint);
	now = sched_ticks();

	/* An acknowledgement that covers the segment being timed gives a round-trip sample (ws220-p004). */
	covered = 0;
	if (endpoint->tcp.rtt_timing != 0)
		covered = tcp_sequence_at_or_before(endpoint->tcp.rtt_sequence, acknowledgement);
	if (covered)
		tcp_rtt_sample_locked(endpoint, now);
	rto = tcp_rto_locked(endpoint);

	/*
	 * No timer without a segment in flight, at once when recovering (an
	 * immediate resend, which does not double the timeout), else the
	 * timeout from now, backed off until a valid sample (Karn).  The
	 * sample above comes first, so the timeout is the new one.
	 */
	timeout = tcp_backoff_timeout(rto, endpoint->tcp.rto_backoff);
	endpoint->tcp.retransmit_immediate = 0;

	/* Notes whether the oldest segment was resent already within this recovery. */
	oldest_resent = 0;
	if (sent != 0)
		oldest_resent = endpoint->tcp.send_queue[endpoint->tcp.send_first].resent != 0;

	/* Sets the deadline by whether anything is in flight and whether to resend at once. */
	if (sent == 0) {
		endpoint->tcp.retransmit_deadline = 0;
	} else if (recovering && !going_back && !oldest_resent) {
		endpoint->tcp.retransmit_deadline = now;
		endpoint->tcp.retransmit_immediate = TCP_IMMEDIATE_LOSS;
		endpoint->tcp.counters.immediate_resends++;
	} else {
		endpoint->tcp.retransmit_deadline = now + timeout;
	}
}

/* Drops the retransmission record and wakes senders. */
static void
tcp_retransmit_clear(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket;
	struct tcp_discard packet;
	unsigned long irq;

	/* Takes the queued segment out under the socket lock. */
	socket = &endpoint->tcp.inet.socket;
	irq = spin_lock_irqsave(&socket->lock);

	tcp_retransmit_reset(endpoint, &packet);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Frees them and lets a blocked sender continue. */
	tcp_discard_free(&packet);
	socket_wake_send(socket);
}

/* Abandons an active connect of a given generation; the caller holds the socket lock. */
static void
tcp_connect_cancel_locked(
	struct tcp_endpoint *endpoint,
	uint32_t generation,
	struct tcp_discard *discard)
{
	struct socket *socket;

	socket = &endpoint->tcp.inet.socket;
	discard->count = 0;

	/*
	 * The generation check prevents a waiter from cancelling a later
	 * attempt which reused the same socket.
	 */
	if (endpoint->tcp.state != TCP_SYN_SENT ||
	    endpoint->tcp.active_connect_generation != generation)
		return;

	/* Closes the socket and wakes everyone waiting on the attempt. */
	tcp_retransmit_reset(endpoint, discard);
	endpoint->tcp.state = TCP_CLOSED;
	endpoint->tcp.active_connect_generation = 0;
	endpoint->tcp.connect_wait_deadline = 0;
	tcp_forget_peer(endpoint);
	waitq_wake_all(&socket->connect_waitq);
	waitq_wake_all(&socket->send_waitq);
	poll_notify();
}

/*
 * Queues a segment that is retransmitted until acknowledged, and sends it.
 *
 * The segment joins the ring unsent.  When nothing queued before it is
 * still unsent and no other thread is sending, this thread sends it at
 * once from the caller's data; otherwise it waits its turn, sent by the
 * thread sending the ones before it or by the output retry already set for
 * them (ws220-p003).  One the device or the pool refuses stays in the ring
 * for the output retry, and the call succeeds: the ring holds the data.
 * One lost for an unresolved neighbour stays sent, as one lost on the
 * wire, for the retransmission timer.  Only a segment that failed for good
 * (no route) while still the last in the ring is taken back, and its error
 * reported; one with others queued after it stays, as a segment lost on
 * the wire, since taking it back would make its writer send its bytes
 * twice.
 */
static int
tcp_send_reliable(
	struct tcp_endpoint *endpoint,
	uint8_t flags,
	const void *data,
	const struct uaccess_pin *pin,
	size_t pin_offset,
	size_t length)
{
	struct packet_buf *copy;
	struct packet_buf *spare;
	struct tcp_pending *entry;
	void *payload;
	void *tail;
	uint32_t sequence;
	uint32_t advance;
	uint64_t now;
	unsigned long irq;
	unsigned slot;
	unsigned sent;
	enum tcp_refusal refusal;
	int room;
	int control;
	int append;
	int nagle;
	int wake;
	int jumbo;
	int owner;
	int retries;
	int deadline_new;
	int error;
	unsigned copy_flags;
	struct tcp_discard discard;

	/*
	 * Keeps a copy of the payload for retransmission, with room in front
	 * for the headers its first transmission pushes: a jumbo packet for a
	 * payload a standard one cannot hold.  A data segment's copy is held
	 * until the peer has it, which may be long, so it leaves the pool's
	 * reserve to the receive and to what only passes through; a SYN or a
	 * FIN, which carries none, may take one.
	 */
	copy_flags = 0U;
	if (length != 0)
		copy_flags = PACKET_BUF_ALLOC_HOLD;
	copy = packet_buf_alloc_for(TCP_COPY_HEADROOM, length, copy_flags, &error);
	if (copy == NULL) {
		if (error == EMSGSIZE)
			return EMSGSIZE;
		return ENOBUFS;
	}
	payload = packet_buf_append(copy, length);
	if (payload == NULL) {
		packet_buf_free(copy);
		return ENOBUFS;
	}

	/* The payload: from a kernel buffer, or straight from the writer's pinned pages. */
	if (length != 0 && pin != NULL) {
		error = copyin_pinned(pin, pin_offset, payload, length);
		if (error != 0) {
			packet_buf_free(copy);
			return error;
		}
	} else if (length != 0) {
		kern_memcpy(payload, data, length);
	}

	/* SYN and FIN each consume one sequence number. */
	advance = (uint32_t)length;
	if ((flags & (TCP_SYN | TCP_FIN)) != 0)
		advance++;

	/*
	 * The segment joins the queued ones if both the ring and the peer's
	 * advertised window have room for it; the unsent ones count, being
	 * promised to the peer already.  A control segment is always
	 * admitted: refusing a SYN or a FIN for want of window would stall
	 * the connection itself rather than the data on it.
	 */
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	control = (flags & (TCP_SYN | TCP_FIN)) != 0;
	jumbo = tcp_copy_is_jumbo(copy);
	append = !control && length != 0 && length <= tcp_tail_room_locked(endpoint);
	if (!append && (endpoint->tcp.send_count >= TCP_SEND_QUEUE_MAX ||
	    (jumbo && endpoint->tcp.send_jumbo >= TCP_SEND_JUMBO_MAX))) {
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);
		packet_buf_free(copy);
		return EAGAIN;
	}
	if (!control && length != 0) {
		room = tcp_send_room_locked(endpoint, length) && tcp_send_buffer_room_locked(endpoint, length);
		if (!room) {
			spin_unlock_irqrestore(
			    &endpoint->tcp.inet.socket.lock, irq);
			packet_buf_free(copy);
			return EAGAIN;
		}
	}

	/*
	 * The bytes join the short last entry, which takes them whole (its
	 * copy is put back), or a new entry goes at the end of the ring,
	 * unsent.
	 */
	spare = NULL;
	if (append) {
		slot = (endpoint->tcp.send_first + endpoint->tcp.send_count - 1U) % TCP_SEND_QUEUE_MAX;
		entry = &endpoint->tcp.send_queue[slot];
		tail = packet_buf_append(entry->packet, length);
		kern_memcpy(tail, payload, length);
		entry->length = (uint16_t)(entry->length + length);
		entry->advance += advance;
		entry->flags |= flags;
		spare = copy;
		copy = entry->packet;
		jumbo = tcp_copy_is_jumbo(copy);
		sequence = entry->sequence;
		flags = entry->flags;
	} else {
		sequence = endpoint->tcp.send_next;
		slot = (endpoint->tcp.send_first + endpoint->tcp.send_count) %
		    TCP_SEND_QUEUE_MAX;
		entry = &endpoint->tcp.send_queue[slot];
		entry->packet = copy;
		entry->sequence = sequence;
		entry->advance = advance;
		entry->length = (uint16_t)length;
		entry->payload_offset = (uint16_t)(copy->data - copy->storage);
		entry->flags = flags;
		entry->fresh = 1;
		entry->sacked = 0;
		entry->resent = 0;
		endpoint->tcp.send_count++;
		endpoint->tcp.send_unsent++;
		if (jumbo)
			endpoint->tcp.send_jumbo++;
	}
	endpoint->tcp.send_next += advance;
	endpoint->tcp.send_bytes += length;

	/* Data queued for its first transmission is counted for net.tcpstats (bytes a short entry took join its segment). */
	if (length != 0) {
		if (!append)
			endpoint->tcp.counters.data_segments_out++;
		endpoint->tcp.counters.data_bytes_out += length;
	}

	/*
	 * Data queued with nothing waiting starts the time TCP_MAXUNACKTIME
	 * measures (ws220-p005), a deadline the worker must learn of.
	 */
	deadline_new = 0;
	if (length != 0 && endpoint->tcp.unack_since == 0) {
		endpoint->tcp.unack_since = sched_ticks();
		deadline_new = 1;
	}

	/*
	 * This thread sends the entry itself when it is the only unsent one,
	 * no other thread is sending and Nagle's algorithm does not hold it:
	 * then it keeps its place in the stream.  send_flushing makes this
	 * thread the only one to move the end of what was sent until it gives
	 * it up below.  An entry queued behind a short one Nagle's algorithm
	 * held, which may go now, has the network worker send them.
	 */
	now = sched_ticks();
	owner = 0;
	nagle = tcp_nagle_holds_locked(endpoint);
	if (endpoint->tcp.send_unsent == 1U && endpoint->tcp.send_flushing == 0 && !nagle) {
		/* The first segment in flight arms the retransmission timer. */
		sent = tcp_sent_count_locked(endpoint);
		if (sent == 0)
			deadline_new = 1;
		endpoint->tcp.send_flushing = 1;
		tcp_mark_sent_locked(endpoint, now);
		owner = 1;

		/*
		 * The copy carries the first transmission itself; the reference
		 * taken for it keeps the copy while the lock is released, should
		 * an acknowledgement or a reset take the entry meanwhile.
		 */
		packet_buf_ref(copy);
	}

	/* Has the output retry send the unsent entries when no thread is sending them. */
	wake = 0;
	if (!owner && endpoint->tcp.send_unsent > 1U && endpoint->tcp.send_flushing == 0 &&
	    endpoint->tcp.output_retry_on_pass == 0 && endpoint->tcp.output_retry_deadline == 0) {
		endpoint->tcp.output_retry_on_pass = 1;
		endpoint->tcp.output_retry_deadline = now;
		wake = 1;
	}

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* The copy a short entry's bytes were added from goes back to the pool. */
	if (spare != NULL)
		packet_buf_free(spare);

	/* Succeeded: the entry waits behind the unsent ones, for whoever sends them. */
	if (!owner) {
		if (wake)
			net_worker_wakeup();
		return 0;
	}

	/*
	 * Publish the retransmission record before output.  A fast ACK may
	 * then safely retire it, and the timer can never observe an unowned
	 * pointer.  The copy goes out with the headers pushed in front of its
	 * payload, the reference taken above passing to the output.
	 */
	error = tcp_send_segment_carried(endpoint, sequence, flags, copy);
	refusal = tcp_refusal_of(error);
	retries = tcp_refusal_retries(refusal);

	/* Settles what became of the entry, and gives the sending up. */
	discard.count = 0;
	irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);

	/* One the device took is in flight, for the measure of the congestion window's use. */
	if (!retries)
		tcp_cwnd_note_sent_locked(endpoint);

	if (retries) {
		/*
		 * A segment the device or the pool had no room for is unsent
		 * again, ahead of any queued after it, and the output retry
		 * sends it (a SYN or a FIN too, which no one else would try
		 * again).  net.tcpstats counts the data ones.
		 */
		tcp_unmark_sent_locked(endpoint, copy);
		if (length != 0)
			endpoint->tcp.counters.send_refused++;
		tcp_output_refused_locked(endpoint, refusal, 1, now);
		error = 0;
	} else if (refusal == TCP_REFUSAL_LOST) {
		/* Lost for an unresolved neighbour: the timer resends it, and R2 ends a peer that never answers. */
		error = 0;
	} else if (refusal == TCP_REFUSAL_HARD) {
		/*
		 * A segment that failed for good is taken back while it is the
		 * last in the ring (a connect's SYN without a route reports the
		 * error); with others queued after it, it stays.
		 */
		slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
		    1U) % TCP_SEND_QUEUE_MAX;
		if (!append && endpoint->tcp.send_unsent == 0 &&
		    endpoint->tcp.send_count != 0 &&
		    endpoint->tcp.send_queue[slot].packet == copy) {
			discard.packets[discard.count++] = copy;
			endpoint->tcp.send_queue[slot].packet = NULL;
			endpoint->tcp.send_count--;
			if (jumbo && endpoint->tcp.send_jumbo != 0)
				endpoint->tcp.send_jumbo--;
			endpoint->tcp.send_bytes -= length;
			endpoint->tcp.send_next = sequence;

			/* A data segment taken back was not queued for net.tcpstats after all: its writer sends the bytes again. */
			if (length != 0) {
				endpoint->tcp.counters.data_segments_out--;
				endpoint->tcp.counters.data_bytes_out -= length;
			}

			/* A segment taken back is not timed (ws220-p004 review-1 M1); the next one sent is. */
			if (endpoint->tcp.rtt_timing != 0 && endpoint->tcp.rtt_sequence == sequence + advance)
				endpoint->tcp.rtt_timing = 0;

			/* Nothing in flight is timed. */
			sent = tcp_sent_count_locked(endpoint);
			if (sent == 0) {
				endpoint->tcp.retransmit_deadline = 0;
				endpoint->tcp.retransmit_count = 0;
				endpoint->tcp.retransmit_since = 0;
				endpoint->tcp.retransmit_refused = 0;
				endpoint->tcp.retransmit_immediate = 0;
			}
		} else {
			error = 0;
		}
	}

	/*
	 * Entries other threads queued while this one was sending are left to
	 * the network worker's next pass, which has a buffer to copy their
	 * payloads into.
	 */
	if (!retries && endpoint->tcp.send_unsent != 0) {
		endpoint->tcp.output_retry_on_pass = 1;
		endpoint->tcp.output_retry_deadline = now;
	}

	/* send_flushing given up: the next sender is whoever finds the first unsent entry. */
	endpoint->tcp.send_flushing = 0;
	tcp_output_retry_settle_locked(endpoint);

	/*
	 * The worker learns of the new deadlines: one this segment armed, or
	 * the output retry's, which it did not count while this thread was
	 * sending.  A segment sent behind others in flight changes neither
	 * (the timer runs from the oldest), so the worker, busy with the
	 * acknowledgements, is not woken once a segment for nothing.
	 */
	if (endpoint->tcp.output_retry_on_pass != 0 || endpoint->tcp.output_retry_deadline != 0 ||
	    endpoint->tcp.persist != 0 || refusal != TCP_REFUSAL_NONE)
		deadline_new = 1;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, irq);

	/* Frees the copy taken back, outside the lock. */
	tcp_discard_free(&discard);

	/* The worker learns of a new deadline. */
	if (deadline_new)
		net_worker_wakeup();

	/* Reports a segment taken back for an error that will not pass. */
	if (error != 0)
		return error;

	/* Succeeded: the segment is on its way, or waits in the ring. */
	return 0;
}

/*
 * Tells how a segment the stack could not hand to the device is sent
 * again, from the error its output reported (ws220-p003).
 */
static enum tcp_refusal
tcp_refusal_of(
	int error)
{
	/* The device took it. */
	if (error == 0)
		return TCP_REFUSAL_NONE;

	/* A full device queue, which the device's transmit completion empties (ECM, NCM, GENET). */
	if (error == ENOBUFS || error == EBUSY)
		return TCP_REFUSAL_DEVICE;

	/* An empty packet pool, which tells no one when it has packets again. */
	if (error == ENOMEM)
		return TCP_REFUSAL_WAIT;

	/* A neighbour still being resolved: the segment is lost, as on the wire. */
	if (error == EAGAIN)
		return TCP_REFUSAL_LOST;

	/* Any other error does not pass by waiting. */
	return TCP_REFUSAL_HARD;
}

/* Tells whether a segment so refused stays unsent for the output retry. */
static int
tcp_refusal_retries(
	enum tcp_refusal refusal)
{
	/* A full device. */
	if (refusal == TCP_REFUSAL_DEVICE)
		return 1;

	/* An empty pool. */
	if (refusal == TCP_REFUSAL_WAIT)
		return 1;

	/* Succeeded: sent, lost as on the wire, or failed for good, it is the timer's. */
	return 0;
}

/* Gives the ticks after which a refused output is tried again at the latest. */
static uint64_t
tcp_refusal_delay(
	enum tcp_refusal refusal)
{
	uint64_t delay;

	/*
	 * A full device runs the network worker when it has room again, so
	 * its delay only covers a device that does not; the pool tells no
	 * one, so the retry waits rather than spinning.
	 */
	if (refusal == TCP_REFUSAL_DEVICE) {
		delay = kern_ms_to_ticks(TCP_OUTPUT_RETRY_DEVICE_MS);
	} else {
		delay = kern_ms_to_ticks(TCP_SEND_RETRY_MS);
	}

	/* At least one tick, so the retry comes after the pass that set it. */
	if (delay == 0)
		delay = 1;

	/* Succeeded: the delay. */
	return delay;
}

/* Counts the ring's entries handed to the device, under the socket lock. */
static unsigned
tcp_sent_count_locked(
	const struct tcp_endpoint *endpoint)
{
	/* The unsent entries are the last ones of the ring. */
	return endpoint->tcp.send_count - endpoint->tcp.send_unsent;
}

/*
 * Gives the end of what was handed to the device (RFC 9293's SND.NXT),
 * under the socket lock: the first unsent entry's sequence number, or the
 * end of what was queued when every entry was sent.
 */
static uint32_t
tcp_sent_end_locked(
	const struct tcp_endpoint *endpoint)
{
	unsigned slot;

	/* Everything queued was sent. */
	if (endpoint->tcp.send_unsent == 0)
		return endpoint->tcp.send_next;

	/* The first unsent entry, after the ones sent. */
	slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
	    endpoint->tcp.send_unsent) % TCP_SEND_QUEUE_MAX;

	/* Succeeded: what was sent ends where it starts. */
	return endpoint->tcp.send_queue[slot].sequence;
}

/*
 * Marks the first unsent entry as handed to the device, under the socket
 * lock; only the thread that set send_flushing calls it.
 *
 * An entry sent with none in flight before it is the oldest one in flight,
 * so it starts the retransmission timer: an entry that has not left has no
 * timeout to wait, and its first sending is not counted as a resend.
 */
static void
tcp_mark_sent_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now)
{
	struct tcp_pending *entry;
	unsigned sent;
	unsigned slot;
	uint64_t rto;
	uint64_t interval;
	int persist;
	int before;
	uint32_t segment_size;
	uint64_t timeout;

	/* What was in flight before it, and the entry itself, the first unsent one. */
	sent = tcp_sent_count_locked(endpoint);
	slot = (endpoint->tcp.send_first + sent) % TCP_SEND_QUEUE_MAX;
	entry = &endpoint->tcp.send_queue[slot];

	/* It is no longer owed to the device: the end of what was sent moves past it, and takes no more bytes. */
	endpoint->tcp.send_unsent--;
	entry->fresh = 0;
	if (sent == 0 && endpoint->tcp.send_count == 1U && endpoint->tcp.send_unsent == 0)
		endpoint->tcp.send_max = entry->sequence + entry->advance;
	before = tcp_sequence_before(endpoint->tcp.send_max, entry->sequence + entry->advance);
	if (before)
		endpoint->tcp.send_max = entry->sequence + entry->advance;

	/* A short data segment is the one Nagle's algorithm waits on until it is acknowledged. */
	segment_size = tcp_segment_size_locked(endpoint);
	if (entry->length != 0 && entry->length < segment_size) {
		endpoint->tcp.send_small_end = entry->sequence + entry->advance;
		endpoint->tcp.send_small_outstanding = 1;
	}

	/* The oldest segment in flight starts the timer afresh, with the connection's timeout, still backed off until a sample. */
	if (sent == 0) {
		rto = tcp_rto_locked(endpoint);
		timeout = tcp_backoff_timeout(rto, endpoint->tcp.rto_backoff);
		endpoint->tcp.retransmit_count = 0;
		endpoint->tcp.retransmit_since = 0;
		endpoint->tcp.retransmit_refused = 0;
		endpoint->tcp.retransmit_immediate = 0;
		endpoint->tcp.retransmit_deadline = now + timeout;
	}

	/*
	 * A segment that goes beyond the window the peer last advertised (a
	 * closed or too small window lets one segment go) is a probe of that
	 * window (ws220-p005): the peer refuses it unless the window has
	 * opened, and it is sent again at once when it does.
	 */
	entry->probe = 0;
	if (entry->length != 0 &&
	    entry->sequence + entry->advance - endpoint->tcp.send_unacknowledged > (uint32_t)endpoint->tcp.peer_window)
		entry->probe = 1;

	/*
	 * A probe sent with nothing else in flight starts the persist state:
	 * its next probe is the persist timer's, not a retransmission
	 * (ws220-p005).
	 */
	persist = 0;
	if (entry->probe != 0 && sent == 0)
		persist = tcp_persist_applies_locked(endpoint);
	if (persist) {
		endpoint->tcp.persist = 1;
		endpoint->tcp.persist_shift = 0;
		interval = tcp_persist_interval_locked(endpoint);
		endpoint->tcp.retransmit_deadline = now + interval;
		endpoint->tcp.persist_shift = 1;
	}

	/* With no segment being timed, this one is, unless it is a resend (Karn): one sample a round trip. */
	if (endpoint->tcp.rtt_timing == 0 && entry->rewound == 0) {
		endpoint->tcp.rtt_timing = 1;
		endpoint->tcp.rtt_sequence = entry->sequence + entry->advance;
		endpoint->tcp.rtt_start = now;
	}
}

/*
 * Takes back the mark of an entry the device or the pool refused, under the
 * socket lock: it is unsent again, first of the unsent entries.
 *
 * packet is the entry's copy, which tells it from another entry put in the
 * same place.  Only the last entry sent can be the caller's, since only the
 * thread that set send_flushing marks entries; when an acknowledgement or a
 * reset has taken it out of the ring meanwhile, nothing changes.
 */
static void
tcp_unmark_sent_locked(
	struct tcp_endpoint *endpoint,
	const struct packet_buf *packet)
{
	uint32_t sent_end;
	int inside;
	uint32_t end;
	unsigned sent;
	unsigned slot;

	/* No entry sent is left: a reset emptied the ring, or an acknowledgement took the entry. */
	sent = tcp_sent_count_locked(endpoint);
	if (sent == 0)
		return;

	/* The last entry sent, which another packet shows is not the caller's. */
	slot = (endpoint->tcp.send_first + sent - 1U) % TCP_SEND_QUEUE_MAX;
	if (endpoint->tcp.send_queue[slot].packet != packet)
		return;

	/* It is owed to the device again, ahead of every entry queued after it. */
	endpoint->tcp.send_unsent++;

	/*
	 * A recovery reaches no further than what was sent (review-1 L3): a
	 * segment first sent after this is no hole a partial acknowledgement
	 * should resend.
	 */
	sent_end = tcp_sent_end_locked(endpoint);
	if (endpoint->tcp.recovery != 0) {
		inside = tcp_sequence_before(sent_end, endpoint->tcp.recover);
		if (inside)
			endpoint->tcp.recover = sent_end;
	}

	/* A segment that did not go is not timed; the next one sent is. */
	end = endpoint->tcp.send_queue[slot].sequence + endpoint->tcp.send_queue[slot].advance;
	if (endpoint->tcp.rtt_timing != 0 && endpoint->tcp.rtt_sequence == end)
		endpoint->tcp.rtt_timing = 0;

	/* With nothing left in flight, nothing is timed. */
	if (sent == 1U) {
		endpoint->tcp.retransmit_deadline = 0;
		endpoint->tcp.retransmit_count = 0;
		endpoint->tcp.retransmit_since = 0;
		endpoint->tcp.retransmit_refused = 0;
		endpoint->tcp.retransmit_immediate = 0;
	}
}

/*
 * Sets the output retry after the device or the pool refused a segment,
 * under the socket lock, keeping a sooner retry already set.
 *
 * A full device is tried again on the network worker's next pass, which
 * its transmit completion runs, and after TCP_OUTPUT_RETRY_DEVICE_MS at the
 * latest; when a retry of the worker sent nothing at all (progressed is 0),
 * the latest is TCP_SEND_RETRY_MS instead, so a device that never completes
 * is not tried every tick.  The pool and an unresolved neighbour are tried
 * again after TCP_SEND_RETRY_MS only.
 */
static void
tcp_output_refused_locked(
	struct tcp_endpoint *endpoint,
	enum tcp_refusal refusal,
	int progressed,
	uint64_t now)
{
	uint64_t delay;
	uint64_t deadline;

	/*
	 * A full device: the next pass, and the latest after it.  The pool:
	 * only after the wait.  The retry is one for the socket, so a pass
	 * already asked for and an earlier deadline already set stand
	 * (review-1 L1): the retry serves every refused segment and
	 * acknowledgement at the soonest any of them asked for.
	 */
	if (refusal == TCP_REFUSAL_DEVICE) {
		delay = tcp_refusal_delay(TCP_REFUSAL_DEVICE);
		if (!progressed)
			delay = tcp_refusal_delay(TCP_REFUSAL_WAIT);
		endpoint->tcp.output_retry_on_pass = 1;
	} else {
		delay = tcp_refusal_delay(TCP_REFUSAL_WAIT);
	}

	/* The earlier of the deadline set and this one. */
	deadline = now + delay;
	if (endpoint->tcp.output_retry_deadline == 0 ||
	    deadline < endpoint->tcp.output_retry_deadline)
		endpoint->tcp.output_retry_deadline = deadline;
}

/*
 * Clears the output retry once nothing waits for it, under the socket lock:
 * no unsent entry and no pending acknowledgement.
 */
static void
tcp_output_retry_settle_locked(
	struct tcp_endpoint *endpoint)
{
	int holds;

	/* Unsent entries still wait, unless the one left waits for Nagle's algorithm, which an acknowledgement ends. */
	if (endpoint->tcp.send_unsent > 1U)
		return;
	if (endpoint->tcp.send_unsent == 1U) {
		holds = tcp_nagle_holds_locked(endpoint);
		if (!holds)
			return;
	}

	/* Pending acknowledgements still wait. */
	if (endpoint->tcp.ack_pending != 0)
		return;

	/* Nothing does: the worker has no retry of this socket to wake for. */
	endpoint->tcp.output_retry_on_pass = 0;
	endpoint->tcp.output_retry_deadline = 0;
}

/*
 * Tells whether the output retry of a socket is due now, under the socket
 * lock: something waits for it, no thread is sending the unsent entries
 * already, and the pass or the deadline it waits for has come.
 */
static int
tcp_output_retry_due_locked(
	const struct tcp_endpoint *endpoint,
	uint64_t now)
{
	/* Nothing waits. */
	if (endpoint->tcp.send_unsent == 0 && endpoint->tcp.ack_pending == 0)
		return 0;

	/* A thread is sending the unsent entries; the retry stays for after it. */
	if (endpoint->tcp.send_flushing != 0)
		return 0;

	/* A full device refused, and this is a pass after it. */
	if (endpoint->tcp.output_retry_on_pass != 0)
		return 1;

	/* The deadline has come. */
	if (endpoint->tcp.output_retry_deadline != 0 &&
	    endpoint->tcp.output_retry_deadline <= now)
		return 1;

	/* Succeeded: the retry is not due yet. */
	return 0;
}

/*
 * Sends again what the device or the pool refused, once its retry is due:
 * the pending acknowledgements, then the unsent entries in order.
 *
 * The network worker calls it for each socket on each pass, with its own
 * buffer for the payloads.  It does not wake the worker when the device
 * refuses again: the device's completion or the deadline brings the next
 * try, so a full device is not tried in a loop.
 */
static void
tcp_output_retry(
	struct tcp_endpoint *endpoint,
	uint8_t *payload,
	uint64_t now)
{
	struct socket *socket;
	unsigned long irq;
	int due;
	int owner;

	/* The socket whose lock guards the ring and the retry. */
	socket = &endpoint->tcp.inet.socket;
	owner = 0;

	/*
	 * Takes the retry when it is due, and with unsent entries the sending
	 * of them, under one hold of the lock: a writer queueing meanwhile
	 * leaves its entry to this pass.  A refusal sets the retry again.  A
	 * retry nothing waits for any more is cleared first, so its deadline
	 * cannot keep the worker awake.
	 */
	irq = spin_lock_irqsave(&socket->lock);

	tcp_output_retry_settle_locked(endpoint);
	due = tcp_output_retry_due_locked(endpoint, now);
	if (due) {
		endpoint->tcp.output_retry_on_pass = 0;
		endpoint->tcp.output_retry_deadline = 0;
		if (endpoint->tcp.send_unsent != 0) {
			endpoint->tcp.send_flushing = 1;
			owner = 1;
		}
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/* The retry is not due. */
	if (!due)
		return;

	/* The acknowledgements first: following a data segment, they would not count as duplicates. */
	tcp_send_ack_pending(endpoint);

	/* Then the unsent entries, oldest first. */
	if (owner)
		tcp_send_unsent(endpoint, payload);
}

/*
 * Hands the unsent entries to the device in order, until none is left or
 * the device or the pool refuses one; the caller has set send_flushing,
 * which this gives up.
 *
 * Each entry is marked sent and its payload copied into payload
 * (TCP_SEGMENT_MAX_JUMBO bytes) under the socket lock, then handed to the device with the lock
 * released: a reset may free the ring's copies meanwhile.  Finding none
 * left and giving up send_flushing happen under one hold of the lock, so an
 * entry a writer queues meanwhile is never left without a sender.
 */
static void
tcp_send_unsent(
	struct tcp_endpoint *endpoint,
	uint8_t *payload)
{
	struct socket *socket;
	struct tcp_pending *entry;
	struct packet_buf *packet;
	unsigned long irq;
	unsigned slot;
	uint32_t sequence;
	size_t length;
	uint8_t flags;
	uint8_t resend;
	uint64_t now;
	enum tcp_refusal refusal;
	int progressed;
	int retries;
	int holds;
	int fits;
	int error;

	/* The socket whose lock guards the ring and the retry. */
	socket = &endpoint->tcp.inet.socket;
	progressed = 0;

	/* Sends each unsent entry in turn, the lock held between them. */
	irq = spin_lock_irqsave(&socket->lock);

	for (;;) {
		/* Stops when every entry has gone, or at the last one Nagle's algorithm holds. */
		if (endpoint->tcp.send_unsent == 0)
			break;
		if (endpoint->tcp.send_unsent == 1U) {
			holds = tcp_nagle_holds_locked(endpoint);
			if (holds)
				break;
		}

		/*
		 * Or at one the congestion window does not take now: an
		 * acknowledgement sends it, and the window is in use.
		 */
		fits = tcp_unsent_fits_cwnd_locked(endpoint);
		if (!fits) {
			endpoint->tcp.cwnd_blocked = 1;
			break;
		}

		/* The first unsent entry, marked sent and copied out. */
		slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
		    endpoint->tcp.send_unsent) % TCP_SEND_QUEUE_MAX;
		entry = &endpoint->tcp.send_queue[slot];
		packet = entry->packet;
		sequence = entry->sequence;
		flags = entry->flags;
		length = entry->length;
		resend = entry->rewound;
		now = sched_ticks();
		tcp_mark_sent_locked(endpoint, now);
		entry->rewound = 0;

		/*
		 * A payload larger than a segment is never queued (a write
		 * queues at most send_mss bytes a segment); one that were would
		 * be left marked and unsent, for the timer to abandon its
		 * connection.
		 */
		if (length > TCP_SEGMENT_MAX_JUMBO) {
			progressed = 1;
			continue;
		}

		/* Its payload, which the ring's copy may not outlive once the lock is released. */
		if (length != 0)
			kern_memcpy(payload, packet->storage + entry->payload_offset, length);

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Hands it to the device, cut to the segment size. */
		error = tcp_send_segment_cut(endpoint, sequence, flags, payload, length);
		refusal = tcp_refusal_of(error);

		/*
		 * A refused entry is unsent again, and the retry set for it
		 * and those after it; net.tcpstats counts the data ones.
		 */
		retries = tcp_refusal_retries(refusal);
		irq = spin_lock_irqsave(&socket->lock);
		if (retries) {
			tcp_unmark_sent_locked(endpoint, packet);
			slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
			    endpoint->tcp.send_unsent) % TCP_SEND_QUEUE_MAX;
			if (endpoint->tcp.send_unsent != 0 && endpoint->tcp.send_queue[slot].packet == packet)
				endpoint->tcp.send_queue[slot].rewound = resend;
			if (length != 0)
				endpoint->tcp.counters.send_refused++;
			tcp_output_refused_locked(endpoint, refusal, progressed, now);
			break;
		}

		/* One the device took is in flight, for the measure of the congestion window's use. */
		tcp_cwnd_note_sent_locked(endpoint);

		/* A resend of what a timeout or a Packet Too Big made unsent again is counted so for net.tcpstats. */
		if (error == 0 && resend != 0 && length != 0) {
			endpoint->tcp.counters.retransmit_segments++;
			endpoint->tcp.counters.retransmit_bytes += length;
		}

		/*
		 * One that went, or was lost for an unresolved neighbour, or
		 * failed for good, is the timer's now.  Each of the others lost
		 * the same way asks ARP once more, at most a ring of them; the
		 * timer's backoff spaces the asking after that.
		 */
		progressed = 1;
	}

	/* Gives the sending up with nothing more to do now. */
	endpoint->tcp.send_flushing = 0;
	tcp_output_retry_settle_locked(endpoint);

	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Sends the pure acknowledgements the device or the pool refused before.
 *
 * While receive_next stands they are duplicates, sent as many as were
 * refused (at most TCP_ACK_PENDING_MAX), each of which the peer counts
 * towards its fast retransmit; once it has moved, one acknowledges all of
 * them.  Called by the output retry of the network worker, it leaves a
 * refusal to the next retry rather than waking the worker.
 */
static void
tcp_send_ack_pending(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket;
	unsigned long irq;
	uint32_t sequence;
	uint32_t receive_next;
	unsigned count;
	unsigned sent;
	unsigned total;
	uint64_t now;
	enum tcp_refusal refusal;
	int progressed;
	int retries;
	int error;

	/* The socket whose lock guards the ring and the retry. */
	socket = &endpoint->tcp.inet.socket;

	/* Takes the pending acknowledgements, and the sequence number they carry. */
	irq = spin_lock_irqsave(&socket->lock);

	count = endpoint->tcp.ack_pending;
	receive_next = endpoint->tcp.ack_pending_receive_next;
	if (count != 0 && receive_next != endpoint->tcp.receive_next) {
		count = 1;
		receive_next = endpoint->tcp.receive_next;
	}

	/* They are this call's now, sent at the end of what was sent. */
	endpoint->tcp.ack_pending = 0;
	sequence = tcp_sent_end_locked(endpoint);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* None was pending. */
	if (count == 0)
		return;

	/* Sends them one by one until the device or the pool refuses one; one lost otherwise is lost as on the wire. */
	refusal = TCP_REFUSAL_NONE;
	retries = 0;
	for (sent = 0; sent < count; sent++) {
		error = tcp_send_segment_at(endpoint, sequence, TCP_ACK, NULL, 0);
		refusal = tcp_refusal_of(error);
		retries = tcp_refusal_retries(refusal);
		if (retries)
			break;
	}

	/* Every one went, or is lost for good. */
	if (!retries)
		return;

	/*
	 * The ones not sent are pending again, joined to any refused
	 * meanwhile of the same receive_next; a newer receive_next refused
	 * meanwhile stands for them.
	 */
	now = sched_ticks();
	irq = spin_lock_irqsave(&socket->lock);

	/* A connection closed meanwhile owes the peer nothing (review-1 L4). */
	if (endpoint->tcp.state == TCP_CLOSED) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return;
	}

	/* The ones not sent join the pending ones, or stand for them. */
	if (endpoint->tcp.ack_pending == 0) {
		endpoint->tcp.ack_pending = count - sent;
		endpoint->tcp.ack_pending_receive_next = receive_next;
	} else if (endpoint->tcp.ack_pending_receive_next == receive_next) {
		total = endpoint->tcp.ack_pending + count - sent;
		if (total > TCP_ACK_PENDING_MAX)
			total = TCP_ACK_PENDING_MAX;
		endpoint->tcp.ack_pending = total;
	}

	/* The retry for them, later when this retry sent nothing at all. */
	progressed = 0;
	if (sent != 0)
		progressed = 1;
	tcp_output_refused_locked(endpoint, refusal, progressed, now);

	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Sends a pure acknowledgement of what this end has received, with the
 * window as it now stands.
 *
 * Its sequence number is the end of what was sent (RFC 9293's SND.NXT): a
 * peer whose window is closed takes a segment at that number only.  One
 * the device or the pool refuses is kept to be sent again by the output
 * retry (ws220-p003), and the network worker woken to wait for it.
 * duplicate is 1 for the acknowledgement of a segment that came ahead of a
 * gap, which a peer's fast retransmit counts: up to TCP_ACK_PENDING_MAX of
 * those are kept for one receive_next.  Any other (a window update, a
 * challenge, an answer to an old segment) is kept as one, since repeating
 * it would look like duplicates to the peer (review-1 M1).  Once
 * receive_next has moved, one says all of them; one that goes with a newer
 * receive_next than the pending ones says all of them too, and they are
 * dropped.  A connection closed meanwhile keeps none (review-1 L4).
 */
static int
tcp_send_ack(
	struct tcp_endpoint *endpoint,
	int duplicate)
{
	int error;

	/* Sends the acknowledgement whatever it tells. */
	error = tcp_send_ack_news(endpoint, duplicate, 0);
	if (error != 0)
		return error;

	/* Succeeded: the acknowledgement went. */
	return 0;
}

/*
 * Sends a pure acknowledgement as tcp_send_ack does; with news_only (a
 * reader's window update), not when it would repeat the last segment's
 * acknowledgement and window (EALREADY, nothing kept to send again).
 */
static int
tcp_send_ack_news(
	struct tcp_endpoint *endpoint,
	int duplicate,
	int news_only)
{
	struct socket *socket;
	unsigned long irq;
	uint32_t sequence;
	uint64_t now;
	enum tcp_refusal refusal;
	int retries;
	int error;

	/* The socket whose lock guards the ring and the retry. */
	socket = &endpoint->tcp.inet.socket;

	/* The end of what was sent. */
	irq = spin_lock_irqsave(&socket->lock);

	sequence = tcp_sent_end_locked(endpoint);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Hands it to the device, unless it would tell nothing new. */
	error = tcp_send_segment_news(endpoint, sequence, TCP_ACK, NULL, 0, news_only);
	if (error == EALREADY)
		return error;
	refusal = tcp_refusal_of(error);
	retries = tcp_refusal_retries(refusal);

	/* A connection closed meanwhile owes the peer nothing. */
	now = sched_ticks();
	irq = spin_lock_irqsave(&socket->lock);

	if (endpoint->tcp.state == TCP_CLOSED)
		retries = 0;

	/* Keeps a refused one for the output retry, or drops the pending ones a newer one covers. */
	if (retries) {
		if (endpoint->tcp.ack_pending != 0 &&
		    endpoint->tcp.ack_pending_receive_next == endpoint->tcp.receive_next) {
			if (duplicate && endpoint->tcp.ack_pending < TCP_ACK_PENDING_MAX)
				endpoint->tcp.ack_pending++;
		} else {
			endpoint->tcp.ack_pending = 1;
			endpoint->tcp.ack_pending_receive_next = endpoint->tcp.receive_next;
		}

		/* The retry for it. */
		tcp_output_refused_locked(endpoint, refusal, 1, now);
	} else if (error == 0 &&
	    endpoint->tcp.ack_pending != 0 &&
	    endpoint->tcp.ack_pending_receive_next != endpoint->tcp.receive_next) {
		endpoint->tcp.ack_pending = 0;
		tcp_output_retry_settle_locked(endpoint);
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/* A refused one: the worker learns of the retry's deadline. */
	if (retries) {
		net_worker_wakeup();
		return error;
	}

	/* Reports an acknowledgement lost for good. */
	if (error != 0)
		return error;

	/* Succeeded: the device has it. */
	return 0;
}

/* Binds a socket to a local address and port. */
static int
tcp_bind(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length)
{
	struct tcp_endpoint *endpoint;
	int error;
	unsigned long irq;
	unsigned long socket_irq;

	/* A socket binds only once. */
	endpoint = tcp_endpoint(socket);
	if ((endpoint->tcp.inet.inet_flags & INET_SOCKET_BOUND) != 0)
		return EINVAL;

	/* Records the reuse option in force at bind time. */
	socket_irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.inet.bind_reuse_address = socket->reuse_address;

	spin_unlock_irqrestore(&socket->lock, socket_irq);

	error = inet_socket_bind(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Allocates a port for zero, or checks the requested one. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	if (endpoint->tcp.inet.local_port == 0)
		error = tcp_allocate_port_locked(endpoint);
	else if (tcp_port_in_use(endpoint, 0))
		error = EADDRINUSE;
	if (error != 0) {
		endpoint->tcp.inet.local_address = 0;
		kern_memset(&endpoint->tcp.inet.local6, 0, sizeof(endpoint->tcp.inet.local6));
		endpoint->tcp.inet.mapped = 0;
		endpoint->tcp.inet.local_port = 0;
		endpoint->tcp.inet.ifindex = 0;
		endpoint->tcp.inet.bind_reuse_address = 0;
		endpoint->tcp.inet.inet_flags &= ~INET_SOCKET_BOUND;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports why the bind failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Puts a bound socket into the listening state. */
static int
tcp_listen(
	struct socket *socket,
	int backlog)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *other;
	unsigned long irq;
	int conflict;

	endpoint = tcp_endpoint(socket);
	conflict = 0;

	/* Listening again is harmless; listening on a connection is not. */
	if (endpoint->tcp.state != TCP_CLOSED) {
		if (endpoint->tcp.state == TCP_LISTEN)
			return 0;
		return EISCONN;
	}

	if ((endpoint->tcp.inet.inet_flags & INET_SOCKET_BOUND) == 0 ||
	    endpoint->tcp.inet.local_port == 0)
		return EDESTADDRREQ;

	/* Clamps the backlog. */
	if (backlog < 0)
		backlog = 0;
	if ((unsigned)backlog > TCP_LISTEN_BACKLOG_MAX)
		backlog = TCP_LISTEN_BACKLOG_MAX;

	/* No other listener may cover the same local address. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (other = tcp_sockets; other != NULL; other = other->next) {
		if (other != endpoint &&
		    other->tcp.state == TCP_LISTEN &&
		    inet_socket_local_conflict(&other->tcp.inet, 0,
		    &endpoint->tcp.inet, 0)) {
			conflict = 1;
			break;
		}
	}

	if (!conflict) {
		if (backlog == 0)
			endpoint->tcp.listen_backlog = 1U;
		else
			endpoint->tcp.listen_backlog = (unsigned)backlog;
		endpoint->tcp.state = TCP_LISTEN;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the listen result. */
	if (conflict)
		return EADDRINUSE;
	return 0;
}

/* Takes the next established child off a listener. */
static int
tcp_accept(
	struct socket *socket,
	struct socket **result,
	struct sockaddr *address,
	socklen_t *length,
	unsigned io_flags)
{
	struct tcp_endpoint *listener;
	struct tcp_socket *accepted;
	struct thread *thread;
	unsigned long irq;
	int error;
	uint64_t sequence;

	listener = tcp_endpoint(socket);
	thread = thread_current();

	/* Rejects a missing result or a socket that is not listening. */
	if (result == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&socket->lock);

	if (listener->tcp.state != TCP_LISTEN) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return EINVAL;
	}

	/* Waits for a child unless the accept must not block. */
	while (listener->tcp.accept_head == NULL) {
		if ((io_flags & SOCKET_IO_NONBLOCK) != 0 || thread == NULL) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EAGAIN;
		}

		if (signal_pending_unblocked(thread)) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		sequence = waitq_sequence(&socket->accept_waitq);
		error = waitq_sleep(&socket->accept_waitq, &socket->lock, sequence,
		    0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}
	}

	/* Reports the peer and dequeues the child. */
	accepted = listener->tcp.accept_head;
	if (address != NULL && length != NULL) {
		error = inet_socket_getpeername(&accepted->inet, address, length);
		if (error != 0) {
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}
	}

	listener->tcp.accept_head = accepted->queue_next;
	if (listener->tcp.accept_head == NULL)
		listener->tcp.accept_tail = NULL;
	if (listener->tcp.accept_count != 0)
		listener->tcp.accept_count--;
	accepted->queue_next = NULL;
	accepted->listener = NULL;
	*result = &accepted->inet.socket;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the accepted socket. */
	return 0;
}

/* Starts or resumes an active connection to a peer. */
static int
tcp_connect(
	struct socket *socket,
	const struct sockaddr *address,
	socklen_t length,
	unsigned io_flags)
{
	struct tcp_endpoint *endpoint;
	struct thread *thread;
	struct tcp_discard cancelled;
	uint64_t deadline;
	uint64_t timeout;
	uint32_t generation;
	unsigned long irq;
	unsigned attempt;
	int error;
	uint64_t sequence;

	endpoint = tcp_endpoint(socket);
	thread = thread_current();
	cancelled.count = 0;
	deadline = 0;

	/*
	 * STOP/CONT is transparent at the syscall layer.  A redispatched connect
	 * resumes the existing attempt; an ordinary second connect still
	 * observes EALREADY/EISCONN as required by the socket API.
	 */
	if (thread != NULL && thread->syscall_stop_redispatch) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == TCP_ESTABLISHED) {
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return 0;
		}

		if (socket->error != 0) {
			error = socket->error;
			socket->error = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return error;
		}

		if (endpoint->tcp.state != TCP_SYN_SENT ||
		    endpoint->tcp.active_connect_generation == 0) {
			endpoint->tcp.connect_wait_deadline = 0;
			spin_unlock_irqrestore(&socket->lock, irq);
			return ECONNABORTED;
		}

		generation = endpoint->tcp.active_connect_generation;
		deadline = endpoint->tcp.connect_wait_deadline;
		goto wait_for_connect;
	}

	/* Only a closed socket can start connecting. */
	irq = spin_lock_irqsave(&socket->lock);

	if (endpoint->tcp.state == TCP_SYN_SENT)
		error = EALREADY;
	else if (endpoint->tcp.state != TCP_CLOSED)
		error = EISCONN;
	else
		error = 0;

	spin_unlock_irqrestore(&socket->lock, irq);

	if (error != 0)
		return error;

	/* Records the peer and takes a local port when unbound. */
	error = inet_socket_connect(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;
	if (endpoint->tcp.inet.remote_port == 0) {
		tcp_forget_peer(endpoint);
		return EADDRNOTAVAIL;
	}

	if (endpoint->tcp.inet.local_port == 0) {
		error = tcp_allocate_port(endpoint);
		if (error != 0) {
			tcp_forget_peer(endpoint);
			return error;
		}
	}

	/* Starts a new attempt with a fresh generation and sequence space. */
	endpoint->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    endpoint->tcp.inet.local_port;
	irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.connect_generation++;
	if (endpoint->tcp.connect_generation == 0)
		endpoint->tcp.connect_generation++;
	generation = endpoint->tcp.connect_generation;
	endpoint->tcp.active_connect_generation = generation;
	endpoint->tcp.connect_wait_deadline = 0;

	/*
	 * Make distinct attempts use distinct wire sequence spaces even when
	 * the scheduler tick did not advance between them.
	 */
	endpoint->tcp.send_next ^= generation * 2654435761U;

	/* A test build may start the sequence numbers just before they wrap. */
	endpoint->tcp.send_next = tcp_initial_sequence(endpoint->tcp.send_next);
	endpoint->tcp.send_unacknowledged = endpoint->tcp.send_next;
	endpoint->tcp.state = TCP_SYN_SENT;

	/* A new peer's way is looked up afresh. */
	endpoint->tcp.route_cached = 0;

	/* The SYN offers window scaling; the SYN-ACK says whether the peer takes it. */
	endpoint->tcp.wscale_offered = 1;
	endpoint->tcp.wscale_on = 0;
	endpoint->tcp.peer_wscale = 0;
	endpoint->tcp.receive_wscale = TCP_RECEIVE_WSCALE;

	/* And selective acknowledgements; the SYN-ACK says whether the peer takes them. */
	endpoint->tcp.sack_offered = 1;
	endpoint->tcp.sack_on = 0;
	endpoint->tcp.sack_recent_set = 0;
	endpoint->tcp.dsack_pending = 0;

	/* A new connection measures its own round trip (ws220-p004). */
	tcp_rtt_forget_locked(endpoint);

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Sends the SYN, retrying a few times when buffers are short. */
	for (attempt = 0; attempt < 4U; attempt++) {
		error = tcp_send_reliable(endpoint, TCP_SYN, NULL, NULL, 0, 0);
		if (error == 0)
			break;
		if (error != EAGAIN && error != EBUSY && error != ENOBUFS) {
			irq = spin_lock_irqsave(&socket->lock);
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
			return error;
		}

		if (thread != NULL)
			sched_sleep(sched_ticks() +
			    kern_ms_to_ticks(TCP_SYN_RETRY_MS));
	}

	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		tcp_connect_cancel_locked(endpoint, generation, &cancelled);
		spin_unlock_irqrestore(&socket->lock, irq);
		tcp_discard_free(&cancelled);
		return error;
	}

	/* A non-blocking connect completes through the timer and poll. */
	if ((io_flags & SOCKET_IO_NONBLOCK) != 0)
		return EINPROGRESS;
	if (thread == NULL)
		return EAGAIN;

	/* Applies the send timeout as the wait deadline. */
	irq = spin_lock_irqsave(&socket->lock);

	timeout = socket->send_timeout_ticks;
	if (timeout != 0) {
		error = kern_deadline_after(sched_ticks(), timeout, &deadline);
		if (error != 0) {
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
			return error;
		}
	}

	endpoint->tcp.connect_wait_deadline = deadline;
wait_for_connect:
	/* Waits for the handshake, a failure, or the deadline. */
	while (endpoint->tcp.state == TCP_SYN_SENT &&
	    endpoint->tcp.active_connect_generation == generation &&
	    socket->error == 0) {
		sequence = waitq_sequence(&socket->connect_waitq);
		error = waitq_sleep(&socket->connect_waitq, &socket->lock,
		    sequence, deadline, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			/*
			 * POSIX requires an interrupted blocking connect to leave
			 * the request in progress.  The retransmission timer owns
			 * the SYN_SENT attempt from here; poll(POLLOUT) plus
			 * SO_ERROR observes completion.
			 */
			spin_unlock_irqrestore(&socket->lock, irq);
			return EINTR;
		}

		if (error == ETIMEDOUT) {
			tcp_connect_cancel_locked(endpoint, generation, &cancelled);
			spin_unlock_irqrestore(&socket->lock, irq);
			tcp_discard_free(&cancelled);
			return error;
		}
	}

	/* Reports a recorded failure, or the final state. */
	if (socket->error != 0) {
		error = socket->error;
		socket->error = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		spin_unlock_irqrestore(&socket->lock, irq);
		return error;
	}

	if (endpoint->tcp.state == TCP_ESTABLISHED)
		error = 0;
	else
		error = ETIMEDOUT;
	endpoint->tcp.connect_wait_deadline = 0;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Sends one segment of data on an established connection. */
static ssize_t
tcp_sendto(
	struct socket *socket,
	const void *buffer,
	size_t length,
	int flags,
	const struct sockaddr *address,
	socklen_t address_length)
{
	ssize_t result;

	(void)address_length;

	/* Rejects an address, or a missing buffer. */
	if (address != NULL || (buffer == NULL && length != 0))
		return -EINVAL;

	/* Sends from the kernel buffer. */
	result = tcp_send_bytes(socket, buffer, NULL, 0, length, flags);

	/* The bytes queued, or why none were. */
	return result;
}

/* Sends from a writer's pinned pages without an address (write(2), socket_file_write_pinned). */
static ssize_t
tcp_send_pinned(
	struct socket *socket,
	const struct uaccess_pin *pin,
	size_t offset,
	size_t length,
	int flags)
{
	ssize_t result;

	/* Rejects a missing pin. */
	if (pin == NULL && length != 0)
		return -EINVAL;

	/* Sends from the pages. */
	result = tcp_send_bytes(socket, NULL, pin, offset, length, flags);

	/* The bytes queued, or why none were. */
	return result;
}

/*
 * Queues bytes for the peer, from a kernel buffer or (pin not NULL) from
 * length bytes at pin_offset in a writer's pinned pages.
 */
static ssize_t
tcp_send_bytes(
	struct socket *socket,
	const void *buffer,
	const struct uaccess_pin *pin,
	size_t pin_offset,
	size_t length,
	int flags)
{
	struct tcp_endpoint *endpoint;
	struct thread *thread;
	unsigned long irq;
	enum tcp_state state;
	size_t total;
	size_t chunk;
	size_t tail;
	uint8_t segment_flags;
	int standard_only;
	int resize;
	int room;
	int error;
	uint64_t sequence;

	endpoint = tcp_endpoint(socket);
	thread = thread_current();

	/* Rejects unsupported flags. */
	if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0)
		return -EINVAL;

	/*
	 * A shut-down or unconnected socket cannot send; what ended a
	 * connection (a reset: ECONNRESET) is told once, first.
	 */
	irq = spin_lock_irqsave(&socket->lock);

	error = 0;
	if (endpoint->tcp.state != TCP_ESTABLISHED && endpoint->tcp.state != TCP_CLOSE_WAIT) {
		error = socket->error;
		socket->error = 0;
	}
	if (error != 0) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -error;
	}

	if (socket->write_shutdown) {
		spin_unlock_irqrestore(&socket->lock, irq);
		if ((flags & MSG_NOSIGNAL) == 0 && thread != NULL &&
		    thread->proc != NULL)
			(void)signal_send_thread(thread, SIGPIPE);
		return -EPIPE;
	}

	/*
	 * A connection sends while it is established, and after the peer's FIN
	 * too (CLOSE_WAIT): the peer shut only its own writing and still reads
	 * the answer (RFC 793; BUG-151, a client's request, SHUT_WR, then the
	 * answer).
	 */
	if (endpoint->tcp.state != TCP_ESTABLISHED &&
	    endpoint->tcp.state != TCP_CLOSE_WAIT) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return -ENOTCONN;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	if (length == 0)
		return 0;

	/*
	 * Queues the bytes a segment at a time, each of the connection's
	 * segment size, for as long as the ring and the windows have room;
	 * the call returns what it queued, waiting for room only before its
	 * first segment.  A segment the device or the pool refuses on its way
	 * out stays queued for the output retry, so the writer
	 * goes on without sleeping for it.  Only two things send it back to
	 * wait: another writer took the room first, and it waits for room
	 * again; or no packet was left for the segment's copy, and it sleeps
	 * TCP_SEND_RETRY_MS, since the pool the whole stack shares tells no
	 * one when it has packets again.  A blocking send waits as long as the
	 * connection lasts; only a signal, an error or the end of the
	 * connection ends the wait.
	 */
	total = 0;
	error = 0;
	standard_only = 0;
	while (total < length) {
		/*
		 * The next segment: the connection's size, or what is left; one
		 * a standard packet holds once the pool had no jumbo packet for
		 * a copy (smaller than the segment size is always allowed); or
		 * what the short last segment still takes, which fills it.
		 */
		irq = spin_lock_irqsave(&socket->lock);

		chunk = tcp_segment_size_locked(endpoint);
		tail = tcp_tail_room_locked(endpoint);
		if (tail != 0)
			chunk = tail;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* Keeps the piece within what a standard packet carries after a jumbo copy failed, and within the write. */
		if (standard_only && chunk > TCP_SEGMENT_MAX)
			chunk = TCP_SEGMENT_MAX;
		if (chunk > length - total)
			chunk = length - total;

		/*
		 * Waits for room rather than for silence: several segments may
		 * be queued, so the send proceeds as long as the ring has a
		 * free entry and the windows have space for this many more
		 * bytes.  Past the first segment the call returns instead.
		 */
		for (;;) {
			irq = spin_lock_irqsave(&socket->lock);
			room = endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX &&
			    tcp_send_room_locked(endpoint, chunk) &&
			    tcp_send_buffer_room_locked(endpoint, chunk) &&
			    (chunk <= TCP_SEGMENT_MAX ||
			    endpoint->tcp.send_jumbo < TCP_SEND_JUMBO_MAX);
			if (room) {
				spin_unlock_irqrestore(&socket->lock, irq);
				break;
			}

			/* What was queued already is the call's result. */
			if (total != 0) {
				spin_unlock_irqrestore(&socket->lock, irq);
				return (ssize_t)total;
			}

			if ((flags & MSG_DONTWAIT) != 0 || thread == NULL) {
				spin_unlock_irqrestore(&socket->lock, irq);
				return -EAGAIN;
			}

			sequence = waitq_sequence(&socket->send_waitq);
			error = waitq_sleep(&socket->send_waitq, &socket->lock, sequence,
			    0, WAITQ_INTERRUPTIBLE);
			if (socket->error != 0) {
				error = socket->error;
				socket->error = 0;
				spin_unlock_irqrestore(&socket->lock, irq);
				return -error;
			}

			spin_unlock_irqrestore(&socket->lock, irq);
			if (error == EINTR)
				return -EINTR;
		}

		/* Our own traffic also restarts the idle period. */
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == TCP_ESTABLISHED && socket->keepalive != 0) {
			endpoint->tcp.keepalive_probes = 0;
			endpoint->tcp.keepalive_deadline =
			    sched_ticks() + TCP_KEEPALIVE_IDLE;
		}
		spin_unlock_irqrestore(&socket->lock, irq);

		/* Queues the segment and sends it, or leaves it to the output retry; the write's last one is pushed. */
		segment_flags = TCP_ACK;
		if (total + chunk == length)
			segment_flags |= TCP_PSH;
		if (pin != NULL) {
			error = tcp_send_reliable(endpoint, segment_flags, NULL, pin, pin_offset + total, chunk);
		} else {
			error = tcp_send_reliable(endpoint, segment_flags, (const uint8_t *)buffer + total, NULL, 0,
			    chunk);
		}

		/* Queued: the next segment. */
		if (error == 0) {
			total += chunk;
			continue;
		}

		/* No jumbo packet for the copy: the segment is cut to fit a standard one, at once. */
		if (error == ENOBUFS && chunk > TCP_SEGMENT_MAX) {
			standard_only = 1;
			continue;
		}

		/*
		 * A segment the path's MTU, lowered meanwhile (a Packet Too
		 * Big), no longer carries was taken back: its bytes go again at
		 * the new size.
		 */
		if (error == EMSGSIZE) {
			irq = spin_lock_irqsave(&socket->lock);

			resize = chunk > tcp_segment_size_locked(endpoint);

			spin_unlock_irqrestore(&socket->lock, irq);

			/* The bytes go again at the new size. */
			if (resize)
				continue;
		}

		/* What was queued already is the call's result; the error, if it lasts, is the next call's. */
		if (total != 0)
			return (ssize_t)total;

		/* Another writer took the room first: waits for room again. */
		if (error == EAGAIN)
			continue;

		/* An error the connection cannot pass. */
		if (error != ENOBUFS)
			return -error;

		/* No packet for the copy: a writer that does not wait is told to try later. */
		if ((flags & MSG_DONTWAIT) != 0 || thread == NULL)
			return -EAGAIN;
		if (signal_pending_unblocked(thread))
			return -EINTR;
		irq = spin_lock_irqsave(&socket->lock);

		error = socket->error;
		socket->error = 0;
		state = endpoint->tcp.state;

		/* The sleep below is counted for net.tcpstats when it is taken. */
		if (error == 0 && (state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT))
			endpoint->tcp.counters.send_buffer_sleeps++;

		spin_unlock_irqrestore(&socket->lock, irq);

		/* A failed or ended connection stops the wait; otherwise the pool gets a moment. */
		if (error != 0)
			return -error;
		if (state != TCP_ESTABLISHED && state != TCP_CLOSE_WAIT)
			return -EPIPE;
		sched_sleep(sched_ticks() + kern_ms_to_ticks(TCP_SEND_RETRY_MS));
	}

	/* Succeeded: the bytes are queued, and the connection delivers them. */
	return (ssize_t)total;
}

/* Receives data from the socket's queue, keeping an unread remainder. */
static ssize_t
tcp_recvfrom(
	struct socket *socket,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *address,
	socklen_t *address_length)
{
	struct tcp_endpoint *endpoint;
	struct packet_buf *packet;
	size_t copied;
	size_t take;
	uint32_t window;
	uint32_t mss;
	uint32_t offered;
	uint32_t opened;
	uint32_t step;
	unsigned long irq;
	int dequeue_flags;
	int update;
	int sent;
	int error;

	endpoint = tcp_endpoint(socket);

	(void)address;
	(void)address_length;

	/* A shut-down or drained closing connection reports end of file. */
	if (socket->read_shutdown)
		return 0;
	if (endpoint->tcp.state == TCP_CLOSE_WAIT && socket->receive_head == NULL)
		return 0;

	/*
	 * After the peer's FIN was read once (its end-of-file marker taken),
	 * every later read reports the end again instead of waiting for data
	 * that cannot come (BUG-151: a client that shut its own side first
	 * reaches TIME_WAIT, and its second read slept for ever).
	 */
	if ((endpoint->tcp.state == TCP_LAST_ACK ||
	     endpoint->tcp.state == TCP_TIME_WAIT) &&
	    socket->receive_head == NULL)
		return 0;
	if (endpoint->tcp.state == TCP_CLOSED &&
	    (endpoint->tcp.inet.inet_flags & INET_SOCKET_CONNECTED) != 0 &&
	    socket->error == 0 &&
	    socket->receive_head == NULL)
		return 0;

	/*
	 * Takes the queued packets in order and copies what fits, waiting (as
	 * the flags allow) only for the first: one read takes as much as is
	 * queued, up to its length.  A short unread suffix goes back to the
	 * queue's head.  The end-of-file marker, an empty packet, ends the
	 * read: taken alone it is the end of the stream, after data it goes
	 * back for the next read.
	 */
	copied = 0;
	dequeue_flags = flags;
	for (;;) {
		/* The next packet; past the first, only one already queued. */
		error = socket_dequeue_packet(socket, dequeue_flags, &packet);
		if (error != 0) {
			if (copied != 0)
				break;
			return -error;
		}

		/* Past the first packet, only one already queued is taken. */
		dequeue_flags = MSG_DONTWAIT;

		/* The end of the stream after data waits for the next read. */
		if (packet->length == 0 && copied != 0) {
			(void)socket_requeue_packet_front(socket, packet);
			break;
		}

		/* The part of it that fits. */
		take = packet->length;
		if (take > length - copied)
			take = length - copied;
		if (take != 0)
			kern_memcpy((uint8_t *)buffer + copied, packet->data, take);
		copied += take;

		/* A short unread suffix goes back to the head; the read is full. */
		if (take < packet->length) {
			(void)packet_buf_pull(packet, take);
			(void)socket_requeue_packet_front(socket, packet);
			break;
		}
		packet_buf_free(packet);

		/* The end of the stream taken alone, or a full read, ends it. */
		if (take == 0 || copied == length)
			break;
	}

	/*
	 * Tells the peer about room the read has made (RFC 1122 4.2.3.3) when
	 * the window it was told of was nearly closed: too small for a full
	 * segment and now able to take one, or under two segments and now two
	 * segments larger.  A sender whose segments are larger than ours
	 * (QEMU's user network sends 1440 bytes whatever MSS we offer) avoids
	 * sending into a small window and would otherwise wait seconds for its
	 * persist timer each time the queue fills.  A window that was not
	 * nearly closed is left to the next acknowledgement.  This stack's own
	 * sender sends a segment it put beyond a closed or too small window
	 * again when an acknowledgement shows the window takes it (ws220-p005);
	 * a segment refused for want of room although the window took it is
	 * resent on duplicate acknowledgements or the timer (p007 keeps the
	 * window's promise).
	 */
	window = tcp_receive_window(endpoint);
	mss = tcp_receive_mss(endpoint);
	offered = tcp_receive_offered(endpoint);
	update = 0;
	if (window > offered) {
		opened = window - offered;
		step = tcp_receive_sws_step(endpoint);
		if (offered < mss && opened >= step)
			update = 1;
		else if (opened >= 2U * mss || opened >= socket->receive_hiwat_bytes / 2U)
			update = 1;
	}

	if (update != 0 &&
	    (endpoint->tcp.state == TCP_ESTABLISHED ||
	     endpoint->tcp.state == TCP_FIN_WAIT_1 ||
	     endpoint->tcp.state == TCP_FIN_WAIT_2)) {
		sent = tcp_send_ack_news(endpoint, 0, 1);

		/*
		 * An update that went is counted for net.tcpstats (a refused one is
		 * sent again later; one an acknowledgement sent meanwhile made
		 * the same as the last did not go).
		 */
		if (sent == 0) {
			irq = spin_lock_irqsave(&socket->lock);

			endpoint->tcp.counters.window_updates_out++;

			spin_unlock_irqrestore(&socket->lock, irq);
		}
	}

	/* Reports the bytes received. */
	return (ssize_t)copied;
}

/*
 * Reports what the peer was last offered and has not filled: the right
 * edge of the last window sent less receive_next (0 once it is passed, or
 * before any window went).  Read without the socket lock, as the window.
 */
static uint32_t
tcp_receive_offered(
	const struct tcp_endpoint *endpoint)
{
	int filled;

	/* None went yet. */
	if (endpoint->tcp.receive_edge_set == 0)
		return 0;

	/* Or it is filled. */
	filled = tcp_sequence_at_or_before(endpoint->tcp.receive_edge, endpoint->tcp.receive_next);
	if (filled)
		return 0;

	/* Succeeded: the part left. */
	return endpoint->tcp.receive_edge - endpoint->tcp.receive_next;
}

/*
 * Reports the least step the offered window's right edge moves by (RFC
 * 9293 section 3.8.6.2.2): half the receive buffer or a segment of this
 * end's size, the smaller.
 */
static uint32_t
tcp_receive_sws_step(
	const struct tcp_endpoint *endpoint)
{
	uint32_t half;
	uint32_t mss;

	/* The smaller of the two. */
	half = (uint32_t)(endpoint->tcp.inet.socket.receive_hiwat_bytes / 2U);
	mss = tcp_receive_mss(endpoint);
	if (half < mss)
		return half;

	/* Succeeded: a segment. */
	return mss;
}

/*
 * Reports the window to advertise: the room left in the receive queue.
 *
 * The queue is bounded twice, by bytes and by how many packets it may hold
 * (the packets come from one small pool the whole stack shares), so the
 * window is the smaller of the bytes left and SOCKET_STREAM_PACKET_BYTES for
 * each packet left, which every packet but the last holds at least.  A connection that offered segments larger than a standard packet
 * holds also promises no more than TCP_RECEIVE_JUMBO_MAX jumbo packets,
 * those its queue holds counted, for the pool has few of them.  A peer
 * that keeps within it is never refused.  It is read without
 * the socket lock, because segments are also sent with that lock held; a
 * value a moment old is what any peer sees anyway.  It is in bytes; the
 * segment's field scales it.
 */
static uint32_t
tcp_receive_window(
	struct tcp_endpoint *endpoint)
{
	const struct socket *socket = &endpoint->tcp.inet.socket;
	size_t limit = socket->receive_hiwat_bytes;
	size_t used = socket->receive_bytes;
	size_t room;
	size_t packets;
	size_t jumbo;
	uint32_t mss;

	/* The bytes left. */
	room = used < limit ? limit - used : 0;

	/* A full segment for each packet the queue may still take. */
	if (socket->receive_packet_limit != 0) {
		packets = socket->receive_packets < socket->receive_packet_limit ?
		    socket->receive_packet_limit - socket->receive_packets : 0;
		if (room > packets * SOCKET_STREAM_PACKET_BYTES)
			room = packets * SOCKET_STREAM_PACKET_BYTES;
	}

	/* A jumbo segment for each jumbo packet the connection may still hold. */
	mss = endpoint->tcp.receive_mss;
	if (mss > TCP_SEGMENT_MAX) {
		jumbo = socket->receive_jumbo < TCP_RECEIVE_JUMBO_MAX ?
		    TCP_RECEIVE_JUMBO_MAX - socket->receive_jumbo : 0;
		if (room > jumbo * mss)
			room = jumbo * mss;
	}

	/* No more than the largest scaled field says (the caller scales and rounds it). */
	if (room > (size_t)0xffffU << TCP_WSCALE_MAX)
		room = (size_t)0xffffU << TCP_WSCALE_MAX;
	return (uint32_t)room;
}

/*
 * Takes up to wanted packets from the receive queues' shared budget, as
 * many as it has left, and reports how many it took.
 */
static unsigned
tcp_receive_extra_take(
	unsigned wanted)
{
	struct packet_buf_pool_usage usage;
	unsigned budget;
	unsigned taken;
	unsigned granted;
	int exchanged;

	/* The budget is a share of the packets the pool really got (fewer when memory was short). */
	packet_buf_pool_usage(&usage);
	budget = usage.total / TCP_RECEIVE_EXTRA_SHARE;

	/* Adds the part granted to what the queues hold, unless another socket moved it meanwhile. */
	taken = atomic_load_acquire(&tcp_receive_extra_packets);
	for (;;) {
		/* Nothing is left to take. */
		if (taken >= budget)
			return 0;

		granted = budget - taken;
		if (granted > wanted)
			granted = wanted;
		exchanged = atomic_compare_exchange(&tcp_receive_extra_packets, &taken, taken + granted);
		if (exchanged)
			break;
	}

	/* Succeeded: the packets this socket now holds of the budget. */
	return granted;
}

/* Gives packets a receive queue held back to the shared budget. */
static void
tcp_receive_extra_give(
	unsigned count)
{
	/* Nothing held is nothing to give. */
	if (count == 0)
		return;

	/* Subtracts them (adding the two's complement). */
	(void)atomic_fetch_add_relaxed(&tcp_receive_extra_packets, 0U - count);
}

/*
 * Sets how many packets a connection's receive queue may hold: packets,
 * but never fewer than TCP_RECEIVE_PACKETS, and those beyond it only as
 * the shared budget grants them (packets beyond what it had are taken,
 * those it no longer needs given back).  Reports the limit set.  The
 * caller holds the socket lock, or the socket is not yet reachable.
 */
static unsigned
tcp_receive_packets_set_locked(
	struct tcp_endpoint *endpoint,
	unsigned packets)
{
	unsigned wanted;
	unsigned held;

	/* The packets wanted beyond the default. */
	wanted = 0;
	if (packets > TCP_RECEIVE_PACKETS)
		wanted = packets - TCP_RECEIVE_PACKETS;

	/* Takes what is missing from the budget, or gives back what is not needed. */
	held = endpoint->tcp.receive_extra_packets;
	if (wanted > held) {
		held += tcp_receive_extra_take(wanted - held);
	} else if (wanted < held) {
		tcp_receive_extra_give(held - wanted);
		held = wanted;
	}

	/* The queue's limit counts what it holds of the budget. */
	endpoint->tcp.receive_extra_packets = held;
	endpoint->tcp.inet.socket.receive_packet_limit = TCP_RECEIVE_PACKETS + held;

	/* Succeeded: the limit now in force. */
	return TCP_RECEIVE_PACKETS + held;
}

/*
 * Reads the time in microseconds from the machine's monotonic counter, for
 * the receive buffer's sizing: the clock's tick (a millisecond or ten) is
 * longer than a round trip on a LAN.  Reports 0 without a counter.
 */
static int
tcp_receive_clock_us(
	uint64_t *now_us)
{
	uint64_t counter;
	uint64_t frequency;
	bool have;

	/* Refuses when the machine has no counter, or one of no known rate. */
	have = kern_rtc_read_counter(&counter, &frequency);
	if (!have || frequency == 0)
		return 0;

	/* Whole seconds and the rest apart, so that the product stays within 64 bits. */
	*now_us = counter / frequency * 1000000U + counter % frequency * 1000000U / frequency;

	/* Succeeded: the time is read. */
	return 1;
}

/*
 * Starts a measurement for the receive buffer's sizing at now_us: it ends
 * when receive_next passes the right edge of the window this end offers
 * now (at least a segment ahead).  The caller holds the socket lock.
 */
static void
tcp_receive_tune_start_locked(
	struct tcp_endpoint *endpoint,
	uint64_t now_us)
{
	uint32_t window;
	uint32_t mss;

	/* The window as the next acknowledgement offers it, a segment at least. */
	window = tcp_receive_window(endpoint);
	mss = tcp_receive_mss(endpoint);
	if (window < mss)
		window = mss;

	/* The measurement runs from here to that edge. */
	endpoint->tcp.receive_tune_base = endpoint->tcp.receive_next;
	endpoint->tcp.receive_tune_mark = endpoint->tcp.receive_next + window;
	endpoint->tcp.receive_tune_start_us = now_us;
	endpoint->tcp.receive_tune_on = 1;
}

/*
 * Grows a connection's receive buffer when its window is what holds the
 * transfer back (dynamic right-sizing), after a payload queued in order.
 *
 * A measurement ends when the window it began with has arrived: as long as
 * the window holds the peer back, that takes one round trip, and longer
 * otherwise.  The round trip as this end sees it, receive_rtt_us, is the
 * shortest of those times.  What one round trip brings is then the bytes
 * received scaled by the round trip over the time they took (a window that
 * took longer than a round trip did not hold the peer back, and its bytes
 * count for less); when twice that is more than the buffer,
 * and the reader keeps up (the queue holds less than half the buffer, so
 * the window is what limits, not the reader), the buffer grows to twice
 * it, up to TCP_RECEIVE_AUTO_MAX and as far as the shared budget lets the
 * packet limit follow.  It never shrinks: a window offered stays offered.
 * The caller holds the socket lock.
 */
static void
tcp_receive_tune_locked(
	struct tcp_endpoint *endpoint)
{
	struct socket *socket = &endpoint->tcp.inet.socket;
	uint64_t now_us;
	uint64_t elapsed;
	uint64_t received;
	uint64_t round_trip;
	uint64_t wanted;
	unsigned packets;
	int passed;
	int have;

	/* Only a buffer that grows by itself is measured. */
	if (endpoint->tcp.receive_auto == 0)
		return;

	/* A measurement whose window has not all arrived goes on. */
	if (endpoint->tcp.receive_tune_on) {
		passed = tcp_sequence_at_or_before(endpoint->tcp.receive_tune_mark, endpoint->tcp.receive_next);
		if (!passed)
			return;
	}

	/* The time now; without a counter nothing is measured. */
	have = tcp_receive_clock_us(&now_us);
	if (!have)
		return;

	/* The first payload starts the first measurement. */
	if (endpoint->tcp.receive_tune_on == 0) {
		tcp_receive_tune_start_locked(endpoint, now_us);
		return;
	}

	/*
	 * A window that came within the counter's resolution says nothing of
	 * the round trip: the measurement goes on to the next window's edge.
	 */
	elapsed = now_us - endpoint->tcp.receive_tune_start_us;
	if (elapsed == 0) {
		endpoint->tcp.receive_tune_mark = endpoint->tcp.receive_next + tcp_receive_window(endpoint);
		return;
	}

	/*
	 * The round trip: the shortest sample yet.  Each is the round trip or
	 * longer, and the round trip itself only while the window holds the
	 * peer back, so a longer one says nothing of it.
	 */
	round_trip = endpoint->tcp.receive_rtt_us;
	if (elapsed > TCP_RTT_SAMPLE_MAX_US)
		elapsed = TCP_RTT_SAMPLE_MAX_US;
	if (round_trip == 0 || elapsed < round_trip)
		round_trip = elapsed;
	endpoint->tcp.receive_rtt_us = (uint32_t)round_trip;

	/* Twice what one round trip brought. */
	received = endpoint->tcp.receive_next - endpoint->tcp.receive_tune_base;
	wanted = 2U * (received * round_trip / elapsed);
	if (wanted > TCP_RECEIVE_AUTO_MAX)
		wanted = TCP_RECEIVE_AUTO_MAX;

	/*
	 * The buffer grows when the window held the transfer back: more than
	 * the buffer wanted, and a reader that keeps up.  The bytes follow the
	 * packets the budget granted, which every packet holds at least.
	 */
	if (wanted > socket->receive_hiwat_bytes &&
	    socket->receive_bytes < socket->receive_hiwat_bytes / 2U) {
		packets = (unsigned)((wanted + SOCKET_STREAM_PACKET_BYTES - 1U) / SOCKET_STREAM_PACKET_BYTES);
		packets = tcp_receive_packets_set_locked(endpoint, packets);
		if ((size_t)packets * SOCKET_STREAM_PACKET_BYTES > socket->receive_hiwat_bytes)
			socket->receive_hiwat_bytes = (size_t)packets * SOCKET_STREAM_PACKET_BYTES;
	}

	/* The next measurement starts here. */
	tcp_receive_tune_start_locked(endpoint, now_us);
}

/*
 * Takes a receive buffer SO_RCVBUF set (the socket layer has stored its
 * bytes): the buffer no longer grows by itself, and the queue's packet
 * limit follows the bytes as far as the shared budget lets it (no fewer
 * than the default), so that a buffer larger than the default is a larger
 * window too.
 */
static void
tcp_buffer_changed(
	struct socket *socket,
	int option)
{
	struct tcp_endpoint *endpoint;
	unsigned long irq;
	unsigned packets;

	/* Only the receive buffer concerns the window. */
	if (option != SO_RCVBUF)
		return;

	/* Fixes the buffer and sets the packets it takes. */
	endpoint = tcp_endpoint(socket);
	irq = spin_lock_irqsave(&socket->lock);

	endpoint->tcp.receive_auto = 0;
	endpoint->tcp.receive_tune_on = 0;
	if (TCP_RECEIVE_AUTO != 0U) {
		packets = (unsigned)((socket->receive_hiwat_bytes + SOCKET_STREAM_PACKET_BYTES - 1U) /
		    SOCKET_STREAM_PACKET_BYTES);
		(void)tcp_receive_packets_set_locked(endpoint, packets);
	}

	spin_unlock_irqrestore(&socket->lock, irq);
}

/* Shuts down one or both directions, sending a FIN for the write side. */
static int
tcp_shutdown(
	struct socket *socket,
	int how)
{
	struct tcp_endpoint *endpoint;
	enum tcp_state old_state;
	enum tcp_state closing_state;
	unsigned long irq;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Rejects an unknown direction. */
	if (how != SHUT_WR && how != SHUT_RDWR && how != SHUT_RD)
		return EINVAL;

	/* The read side is shut down by flag alone. */
	irq = spin_lock_irqsave(&socket->lock);

	if (how == SHUT_RD || how == SHUT_RDWR)
		socket->read_shutdown = 1;
	if (how == SHUT_RD) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	if (socket->write_shutdown) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	spin_unlock_irqrestore(&socket->lock, irq);

	/*
	 * A full retransmission ring is first given time to make room for the
	 * FIN, as a close does: a writer that has just filled the ring and
	 * shuts its side is not told to try again.
	 */
	tcp_close_drain(endpoint, TCP_DRAIN_ROOM);
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->write_shutdown) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return 0;
	}

	/* The write side moves to the closing state and sends a FIN. */
	if (endpoint->tcp.state != TCP_ESTABLISHED &&
	    endpoint->tcp.state != TCP_CLOSE_WAIT) {
		spin_unlock_irqrestore(&socket->lock, irq);
		return ENOTCONN;
	}

	old_state = endpoint->tcp.state;
	if (old_state == TCP_CLOSE_WAIT)
		closing_state = TCP_LAST_ACK;
	else
		closing_state = TCP_FIN_WAIT_1;
	socket->write_shutdown = 1;
	endpoint->tcp.state = closing_state;

	spin_unlock_irqrestore(&socket->lock, irq);

	/* A FIN that could not be sent restores the state. */
	error = tcp_send_reliable(endpoint, TCP_FIN | TCP_ACK, NULL, NULL, 0, 0);
	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == closing_state) {
			endpoint->tcp.state = old_state;
			socket->write_shutdown = 0;
		}

		spin_unlock_irqrestore(&socket->lock, irq);
	}

	/* Reports why the shutdown failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the local address. */
static int
tcp_getsockname(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Reports the failure. */
	error = inet_socket_getsockname(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the peer address. */
static int
tcp_getpeername(
	struct socket *socket,
	struct sockaddr *address,
	socklen_t *length)
{
	struct tcp_endpoint *endpoint;
	int error;

	endpoint = tcp_endpoint(socket);

	/* Reports the failure. */
	error = inet_socket_getpeername(&endpoint->tcp.inet, address, length);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Closes a socket, aborting its queued children and unregistering it. */
static void
tcp_close(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint **link;
	struct tcp_socket *queued;
	unsigned long irq;

	/* A connection sends its FIN; a child leaves its listener. */
	endpoint = tcp_endpoint(socket);
	if (endpoint->tcp.state == TCP_ESTABLISHED ||
	    endpoint->tcp.state == TCP_CLOSE_WAIT)
		(void)tcp_shutdown(socket, SHUT_RDWR);
	if (endpoint->tcp.listener != NULL)
		tcp_listener_remove(endpoint);

	/* A listener aborts every half-open and unaccepted child. */
	queued = endpoint->tcp.half_open_head;
	while (queued != NULL) {
		endpoint->tcp.half_open_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
		queued = endpoint->tcp.half_open_head;
	}

	queued = endpoint->tcp.accept_head;
	while (queued != NULL) {
		endpoint->tcp.accept_head = queued->queue_next;
		queued->queue_next = NULL;
		queued->listener = NULL;
		socket_set_error(&queued->inet.socket, ECONNABORTED);
		socket_release(&queued->inet.socket);
		queued = endpoint->tcp.accept_head;
	}

	/* Unregisters the endpoint and frees it. */
	irq = spin_lock_irqsave(&tcp_registry_lock);

	for (link = &tcp_sockets; *link != NULL; link = &(*link)->next) {
		if (*link == endpoint) {
			*link = endpoint->next;
			break;
		}
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* The packets its receive queue took beyond the default go back to the other connections. */
	tcp_receive_extra_give(endpoint->tcp.receive_extra_packets);
	endpoint->tcp.receive_extra_packets = 0;

	tcp_retransmit_clear(endpoint);
	tcp_reorder_free(endpoint);
	kern_free(endpoint);
}

/* Reports whether the wait a closing connection is making is over. */
static int
tcp_drain_done(
	const struct tcp_endpoint *endpoint,
	enum tcp_drain until)
{
	unsigned index;

	/* Room for one more segment, or nothing outstanding at all. */
	if (until == TCP_DRAIN_ROOM)
		return endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX;
	if (until == TCP_DRAIN_ALL)
		return endpoint->tcp.send_count == 0;

	/* No outstanding segment carries data; a FIN alone does not count. */
	for (index = 0; index < endpoint->tcp.send_count; index++) {
		if (endpoint->tcp.send_queue[(endpoint->tcp.send_first + index) %
		    TCP_SEND_QUEUE_MAX].length != 0)
			return 0;
	}
	return 1;
}

/*
 * Waits, for a bounded time, while a closing connection still needs the
 * peer to take what it sent.
 *
 * The endpoint is freed when the last reference goes, and with it the copies
 * kept for retransmission, so the data sent must be taken before then.  The
 * wait happens while the closing file still holds its reference, so that
 * acknowledgements and the retransmission timer reach the endpoint.  It ends
 * when the condition holds, the connection has failed, or TCP_CLOSE_DRAIN_MS
 * has passed.  A kernel thread (the network
 * worker among them) does not wait: it is what would deliver the
 * acknowledgements.
 */
static void
tcp_close_drain(
	struct tcp_endpoint *endpoint,
	enum tcp_drain until)
{
	struct socket *socket = &endpoint->tcp.inet.socket;
	struct thread *thread = thread_current();
	uint64_t deadline;
	uint64_t sequence;
	unsigned long irq;

	/* Only a process may wait for the network. */
	if (thread == NULL || thread->proc == NULL || thread->proc == &process0)
		return;

	deadline = sched_ticks() + kern_ms_to_ticks(TCP_CLOSE_DRAIN_MS);
	irq = spin_lock_irqsave(&socket->lock);

	/* Waits while the condition does not hold on a connection still alive. */
	while (!tcp_drain_done(endpoint, until) &&
	    endpoint->tcp.state != TCP_CLOSED && socket->error == 0 &&
	    sched_ticks() < deadline) {
		sequence = waitq_sequence(&socket->send_waitq);
		(void)waitq_sleep(&socket->send_waitq, &socket->lock, sequence,
		    deadline, 0);
	}

	spin_unlock_irqrestore(&socket->lock, irq);
}

/*
 * Closes a connection when its last file goes away: sends the FIN and lets
 * what was sent reach the peer.
 *
 * The socket layer has already marked both directions shut, so the FIN is
 * sent here rather than through tcp_shutdown(), which would take the mark to
 * mean it had been sent.  A full retransmission ring is first given time to
 * make room for it.
 */
static void
tcp_endpoint_close(
	struct socket *socket)
{
	struct tcp_endpoint *endpoint = tcp_endpoint(socket);
	enum tcp_state old_state;
	enum tcp_state closing_state;
	unsigned long irq;
	int error;

	/* Only a connection that has not sent its FIN owes one. */
	irq = spin_lock_irqsave(&socket->lock);
	old_state = endpoint->tcp.state;
	spin_unlock_irqrestore(&socket->lock, irq);
	if (old_state != TCP_ESTABLISHED && old_state != TCP_CLOSE_WAIT)
		return;

	/* Makes room for the FIN, then sends it. */
	tcp_close_drain(endpoint, TCP_DRAIN_ROOM);
	closing_state = old_state == TCP_CLOSE_WAIT ? TCP_LAST_ACK :
	    TCP_FIN_WAIT_1;
	irq = spin_lock_irqsave(&socket->lock);
	if (endpoint->tcp.state == old_state)
		endpoint->tcp.state = closing_state;
	spin_unlock_irqrestore(&socket->lock, irq);
	error = tcp_send_reliable(endpoint, TCP_FIN | TCP_ACK, NULL, NULL, 0, 0);
	if (error != 0) {
		irq = spin_lock_irqsave(&socket->lock);
		if (endpoint->tcp.state == closing_state)
			endpoint->tcp.state = old_state;
		spin_unlock_irqrestore(&socket->lock, irq);
		return;
	}

	/*
	 * Lets what was sent be taken, the FIN included, for at most the drain
	 * time.  The FIN goes out through the retransmission ring, and a first
	 * transmission can be lost (a USB adapter drops a frame while its one
	 * transfer is busy); freeing the endpoint before the FIN is taken would
	 * leave it never sent again.  Closing second, a peer that has gone
	 * costs only that bounded wait.
	 */
	tcp_close_drain(endpoint, TCP_DRAIN_ALL);
}

/* Reports the readiness of a socket. */
static int
tcp_poll(
	struct socket *socket,
	short events,
	short *revents)
{
	struct tcp_endpoint *endpoint;
	short result;
	unsigned long irq;

	endpoint = tcp_endpoint(socket);
	result = 0;

	/* Rejects a missing socket or result. */
	if (socket == NULL || revents == NULL)
		return EINVAL;

	/* Derives readiness from the state under the socket lock. */
	irq = spin_lock_irqsave(&socket->lock);

	if (socket->error != 0)
		result |= POLLERR;
	if (endpoint->tcp.state == TCP_LISTEN) {
		/* A listener is readable when a child awaits accept. */
		if (endpoint->tcp.accept_head != NULL)
			result |= events & (POLLIN | POLLRDNORM);
	} else {
		/* Data, end of file, or a closed peer makes a connection readable. */
		if (socket->receive_head != NULL ||
		    socket->read_shutdown ||
		    endpoint->tcp.state == TCP_CLOSE_WAIT ||
		    endpoint->tcp.state == TCP_TIME_WAIT ||
		    (endpoint->tcp.state == TCP_CLOSED &&
		     (endpoint->tcp.inet.inet_flags & INET_SOCKET_CONNECTED) != 0))
			result |= events & (POLLIN | POLLRDNORM);

		/*
		 * A connection is writable while the send ring has room: a
		 * write does not have to wait for everything already sent to
		 * be acknowledged, only for space to put one more segment.
		 */
		if (!socket->write_shutdown &&
		    (endpoint->tcp.state == TCP_ESTABLISHED ||
		     endpoint->tcp.state == TCP_CLOSE_WAIT) &&
		    endpoint->tcp.send_count < TCP_SEND_QUEUE_MAX)
			result |= events & (POLLOUT | POLLWRNORM);
		if (endpoint->tcp.state == TCP_SYN_SENT && socket->error != 0)
			result |= events & (POLLOUT | POLLWRNORM);
		/*
		 * The connection hangs up when the peer's side is gone (its FIN
		 * or reset, or a read shutdown) or the socket is closing.  Our own
		 * SHUT_WR is not a hangup: it only stops POLLOUT above, so a
		 * client that sent a request and shut its side still sleeps until
		 * the answer comes (BUG-151, as Linux and FreeBSD do).
		 */
		if (socket->read_shutdown ||
		    endpoint->tcp.state == TCP_CLOSE_WAIT ||
		    endpoint->tcp.state == TCP_TIME_WAIT ||
		    socket->lifecycle != SOCKET_OPEN)
			result |= POLLHUP;
	}

	if (socket->lifecycle != SOCKET_OPEN)
		result |= POLLHUP;

	spin_unlock_irqrestore(&socket->lock, irq);

	*revents = result;

	/* Reports the derived readiness. */
	return 0;
}

/* Finds the referenced connection or listener for an incoming segment. */
static struct tcp_endpoint *
tcp_lookup(
	const struct tcp_segment_addresses *addresses,
	uint16_t source_port,
	uint16_t destination_port)
{
	struct tcp_endpoint *endpoint;
	struct tcp_endpoint *wildcard;
	unsigned long irq;
	unsigned long socket_irq;
	int match;
	int referenced;

	wildcard = NULL;
	irq = spin_lock_irqsave(&tcp_registry_lock);

	/* A connection matches on the full four-tuple. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		match = tcp_connection_matches(endpoint, addresses, source_port, destination_port);
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (match)
			goto found;
	}

	/* A listener matches on the port, preferring an exact address. */
	for (endpoint = tcp_sockets; endpoint != NULL; endpoint = endpoint->next) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		match = tcp_listener_matches(endpoint, addresses, destination_port);
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (match == TCP_MATCH_EXACT)
			goto found;
		if (match == TCP_MATCH_WILDCARD)
			wildcard = endpoint;
	}

	endpoint = wildcard;
found:
	/* References the match unless it is being closed. */
	if (endpoint != NULL) {
		referenced = socket_tryref(&endpoint->tcp.inet.socket);
		if (!referenced)
			endpoint = NULL;
	}

	spin_unlock_irqrestore(&tcp_registry_lock, irq);

	/* Reports the referenced endpoint, or NULL. */
	return endpoint;
}

/*
 * Tells whether a connection is the one a segment belongs to, under the
 * socket lock: its ports, its peer, and its local address (or none yet),
 * of the segment's family.
 */
static int
tcp_connection_matches(
	const struct tcp_endpoint *endpoint,
	const struct tcp_segment_addresses *addresses,
	uint16_t source_port,
	uint16_t destination_port)
{
	int accepts;
	int same;
	int unspecified;

	/* A connection, on the ports. */
	if (endpoint->tcp.state == TCP_CLOSED ||
	    endpoint->tcp.state == TCP_LISTEN ||
	    endpoint->tcp.inet.local_port != destination_port ||
	    endpoint->tcp.inet.remote_port != source_port)
		return 0;

	/* An IPv4 segment: a socket that speaks IPv4, with the addresses. */
	if (addresses->family == AF_INET) {
		accepts = inet_socket_accepts_ipv4(&endpoint->tcp.inet);
		if (!accepts || endpoint->tcp.inet.remote_address != addresses->source)
			return 0;
		if (endpoint->tcp.inet.local_address != 0 &&
		    endpoint->tcp.inet.local_address != addresses->destination)
			return 0;
		return 1;
	}

	/* An IPv6 one: a socket that speaks IPv6, with the addresses. */
	accepts = inet_socket_accepts_ipv6(&endpoint->tcp.inet);
	if (!accepts)
		return 0;
	same = in6_equal(&endpoint->tcp.inet.remote6, &addresses->source6);
	if (!same)
		return 0;
	unspecified = in6_is_unspecified(&endpoint->tcp.inet.local6);
	same = in6_equal(&endpoint->tcp.inet.local6, &addresses->destination6);
	if (!unspecified && !same)
		return 0;

	/* Succeeded: the segment's connection. */
	return 1;
}

/*
 * Tells how a listener matches a segment, under the socket lock: not, on
 * the wildcard address, or on the exact one, of the segment's family (an
 * IPv6 listener that takes IPv4 too matches IPv4 segments on [::]).
 */
static int
tcp_listener_matches(
	const struct tcp_endpoint *endpoint,
	const struct tcp_segment_addresses *addresses,
	uint16_t destination_port)
{
	int accepts;
	int same;
	int unspecified;

	/* A listener on the port. */
	if (endpoint->tcp.state != TCP_LISTEN ||
	    endpoint->tcp.inet.local_port != destination_port)
		return TCP_MATCH_NONE;

	/* An IPv4 segment: a listener that takes IPv4, on its address or the wildcard. */
	if (addresses->family == AF_INET) {
		accepts = inet_socket_accepts_ipv4(&endpoint->tcp.inet);
		if (!accepts)
			return TCP_MATCH_NONE;
		if (endpoint->tcp.inet.local_address == addresses->destination)
			return TCP_MATCH_EXACT;
		if (endpoint->tcp.inet.local_address == 0)
			return TCP_MATCH_WILDCARD;
		return TCP_MATCH_NONE;
	}

	/* An IPv6 one: a listener that takes IPv6, on its address or [::]. */
	accepts = inet_socket_accepts_ipv6(&endpoint->tcp.inet);
	if (!accepts)
		return TCP_MATCH_NONE;
	same = in6_equal(&endpoint->tcp.inet.local6, &addresses->destination6);
	if (same)
		return TCP_MATCH_EXACT;
	unspecified = in6_is_unspecified(&endpoint->tcp.inet.local6);
	if (unspecified)
		return TCP_MATCH_WILDCARD;

	/* Succeeded: another address. */
	return TCP_MATCH_NONE;
}

/* Creates a half-open child for a SYN that reached a listener. */
static struct tcp_endpoint *
tcp_passive_syn(
	struct tcp_endpoint *listener,
	const struct tcp_segment_addresses *addresses,
	uint16_t source_port,
	uint32_t sequence,
	uint32_t offered_mss,
	int offered_wscale,
	int offered_sack)
{
	struct socket *created;
	struct tcp_endpoint *child;
	unsigned long irq;
	unsigned packets;
	int error;

	/* Creates the child socket, of the listener's family. */
	error = socket_create(listener->tcp.inet.family, SOCK_STREAM, IPPROTO_TCP, &created);
	if (error != 0)
		return NULL;
	child = tcp_endpoint(created);

	/* The listener must still listen and have backlog room. */
	irq = spin_lock_irqsave(&listener->tcp.inet.socket.lock);

	if (listener->tcp.state != TCP_LISTEN ||
	    listener->tcp.half_open_count + listener->tcp.accept_count >=
	    listener->tcp.listen_backlog) {
		spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);
		socket_release(created);
		return NULL;
	}

	/* The child inherits the listener's address, options, and timeouts. */
	tcp_child_addresses(child, listener, addresses);
	child->tcp.inet.local_port = listener->tcp.inet.local_port;
	child->tcp.inet.remote_port = source_port;
	child->tcp.inet.ifindex = listener->tcp.inet.ifindex;
	child->tcp.inet.inet_flags = INET_SOCKET_BOUND | INET_SOCKET_CONNECTED;
	child->tcp.receive_next = sequence + 1U;
	child->tcp.peer_mss = offered_mss;

	/* A SYN that offers window scaling is answered with the option, and both ends then scale (RFC 7323). */
	if (offered_wscale >= 0) {
		child->tcp.wscale_offered = 1;
		child->tcp.wscale_on = 1;
		child->tcp.peer_wscale = (uint8_t)offered_wscale;
		child->tcp.receive_wscale = TCP_RECEIVE_WSCALE;
	}

	/* A SYN that offers selective acknowledgements is answered with SACK-Permitted, and both ends use them. */
	child->tcp.sack_offered = (uint8_t)(offered_sack != 0);
	child->tcp.sack_on = (uint8_t)(offered_sack != 0);

	child->tcp.send_next = (uint32_t)sched_ticks() * 1103515245U +
	    child->tcp.inet.local_port + source_port;
	child->tcp.send_next = tcp_initial_sequence(child->tcp.send_next);
	child->tcp.send_unacknowledged = child->tcp.send_next;
	child->tcp.listener = &listener->tcp;
	child->tcp.inet.socket.receive_timeout_ticks =
	    listener->tcp.inet.socket.receive_timeout_ticks;
	child->tcp.inet.socket.send_timeout_ticks =
	    listener->tcp.inet.socket.send_timeout_ticks;
	child->tcp.maxunack = listener->tcp.maxunack;
	child->tcp.maxunack_seconds = listener->tcp.maxunack_seconds;
	child->tcp.cc = listener->tcp.cc;
	child->tcp.inet.socket.reuse_address =
	    listener->tcp.inet.socket.reuse_address;
	child->tcp.inet.bind_reuse_address =
	    listener->tcp.inet.bind_reuse_address;

	/*
	 * A receive buffer SO_RCVBUF set on the listener is the child's too,
	 * fixed as the listener's is (BSD and Linux pass it on the same way);
	 * the child is not yet where a segment finds it.
	 */
	if (TCP_RECEIVE_AUTO != 0U && listener->tcp.receive_auto == 0) {
		child->tcp.receive_auto = 0;
		child->tcp.inet.socket.receive_hiwat_bytes = listener->tcp.inet.socket.receive_hiwat_bytes;
		packets = (unsigned)((child->tcp.inet.socket.receive_hiwat_bytes + SOCKET_STREAM_PACKET_BYTES - 1U) /
		    SOCKET_STREAM_PACKET_BYTES);
		(void)tcp_receive_packets_set_locked(child, packets);
	}

	child->tcp.queue_next = listener->tcp.half_open_head;
	listener->tcp.half_open_head = &child->tcp;
	listener->tcp.half_open_count++;

	spin_unlock_irqrestore(&listener->tcp.inet.socket.lock, irq);

	/* Answers with a SYN-ACK, dropping the child when that fails. */
	irq = spin_lock_irqsave(&child->tcp.inet.socket.lock);

	child->tcp.state = TCP_SYN_RECEIVED;

	spin_unlock_irqrestore(&child->tcp.inet.socket.lock, irq);

	/* Sends the SYN-ACK. */
	error = tcp_send_reliable(child, TCP_SYN | TCP_ACK, NULL, NULL, 0, 0);
	if (error != 0) {
		tcp_listener_remove(child);
		socket_release(created);
		return NULL;
	}

	/* Reports the half-open child. */
	return child;
}

/*
 * Gives a listener's child its addresses: the segment's peer, and the
 * listener's address or, on a wildcard, the one the segment came to.  An
 * IPv4 segment to an IPv6 listener makes an IPv4-mapped child.
 */
static void
tcp_child_addresses(
	struct tcp_endpoint *child,
	const struct tcp_endpoint *listener,
	const struct tcp_segment_addresses *addresses)
{
	int unspecified;
	int linklocal;

	/* An IPv4 connection: the listener's address, or the segment's. */
	child->tcp.inet.v6only = listener->tcp.inet.v6only;
	if (addresses->family == AF_INET) {
		if (listener->tcp.inet.local_address == 0)
			child->tcp.inet.local_address = addresses->destination;
		else
			child->tcp.inet.local_address = listener->tcp.inet.local_address;
		child->tcp.inet.remote_address = addresses->source;
		if (child->tcp.inet.family == AF_INET6)
			child->tcp.inet.mapped = 1;
		return;
	}

	/* An IPv6 one: the same, with the interface a link-local peer is on. */
	unspecified = in6_is_unspecified(&listener->tcp.inet.local6);
	if (unspecified)
		child->tcp.inet.local6 = addresses->destination6;
	else
		child->tcp.inet.local6 = listener->tcp.inet.local6;
	child->tcp.inet.remote6 = addresses->source6;
	linklocal = in6_is_linklocal(&addresses->source6);
	if (linklocal)
		child->tcp.inet.scope6 = addresses->ifindex;
}

/*
 * Tells what error a reset reports to the socket, under the socket lock.
 *
 * A connection that is still sending its SYN has not been accepted by
 * anyone, so a reset there means the peer refused it: connect(2) reports
 * ECONNREFUSED.  RFC 793 accepts such a reset only when it acknowledges the
 * SYN, so a stray one cannot end the attempt; 0 means "ignore it".  A reset
 * in any other state resets a connection that existed.
 */
static int
tcp_reset_error_locked(
	const struct tcp_endpoint *endpoint,
	uint8_t flags,
	uint32_t acknowledgement)
{
	/* A connection past the handshake is reset. */
	if (endpoint->tcp.state != TCP_SYN_SENT)
		return ECONNRESET;

	/* A reset that acknowledges nothing does not answer the SYN. */
	if ((flags & TCP_ACK) == 0)
		return 0;

	/* A reset for another sequence space answers some other SYN. */
	if (acknowledgement != endpoint->tcp.send_next)
		return 0;

	/* Succeeded: the peer refused this connection. */
	return ECONNREFUSED;
}

/* Takes an IPv4 TCP segment whose checksum holds. */
static int
tcp_input(
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	struct tcp_segment_addresses addresses;
	uint16_t checksum;
	int error;

	/* Drops a short or corrupt segment. */
	if (packet == NULL || packet->length < sizeof(struct tcp_wire)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Checks the checksum, unless the device found it right. */
	checksum = 0;
	if ((packet->flags & PACKET_BUF_RX_CSUM_L4) == 0)
		checksum = net_checksum_pseudo(source, destination, IPPROTO_TCP, packet->data, packet->length);
	if (checksum != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Hands it on with its addresses. */
	kern_memset(&addresses, 0, sizeof(addresses));
	addresses.family = AF_INET;
	addresses.source = source;
	addresses.destination = destination;
	error = tcp_segment_input(packet, &addresses);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes an IPv6 TCP segment whose checksum holds (ws130-p003). */
static int
tcp6_input(
	struct packet_buf *packet,
	const struct in6_addr *source,
	const struct in6_addr *destination)
{
	struct tcp_segment_addresses addresses;
	uint16_t checksum;
	int multicast;
	int error;

	/* Drops a short or corrupt segment, and one to a group. */
	if (packet == NULL || packet->length < sizeof(struct tcp_wire)) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Checks the checksum, unless the device found it right. */
	checksum = 0;
	if ((packet->flags & PACKET_BUF_RX_CSUM_L4) == 0) {
		checksum = net_checksum_pseudo6(source->s6_addr, destination->s6_addr, IPPROTO_TCP, packet->data,
		    packet->length);
	}

	/* Drops a corrupt one. */
	if (checksum != 0) {
		packet_buf_free(packet);
		return EINVAL;
	}

	multicast = in6_is_multicast(destination);
	if (multicast) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Hands it on with its addresses and the interface it came in on. */
	kern_memset(&addresses, 0, sizeof(addresses));
	addresses.family = AF_INET6;
	addresses.source6 = *source;
	addresses.destination6 = *destination;
	if (packet->device != NULL)
		addresses.ifindex = packet->device->ifindex;
	error = tcp_segment_input(packet, &addresses);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Handles an incoming TCP segment of either family, its checksum checked. */
static int
tcp_segment_input(
	struct packet_buf *packet,
	const struct tcp_segment_addresses *addresses)
{
	const struct tcp_wire *tcp;
	struct tcp_endpoint *endpoint;
	uint32_t sequence;
	uint32_t acknowledgement;
	uint16_t source_port;
	uint16_t destination_port;
	size_t header_length;
	size_t payload_length;
	uint8_t flags;
	enum tcp_state state;
	unsigned long socket_irq;
	struct tcp_discard retransmit;
	int reset_error;
	enum tcp_reset_check reset_check;
	uint32_t resend_sequence;
	int resend;
	int established;
	uint32_t sent_end;
	unsigned sent;
	unsigned slot;
	int old_payload;
	int accept_payload;
	int discard_payload;
	int queued;
	int advanced;
	int enqueue_error;
	int window_opened;
	int duplicate;
	int counted;
	int sack_new;
	int sack_resend;
	int moved;
	int fast;
	int reprobe;
	uint32_t previous_unacknowledged;
	int window_update;
	int acknowledges_unsent;
	int acknowledges_old;
	uint32_t overlap;
	int starts_before;
	uint32_t segment_window;
	uint32_t previous_window;
	int offered_wscale;
	int offered_sack;
	uint32_t offered_mss;
	int within_max;
	int before;
	int retired;
	int wake;
	int accept_fin;
	int gap_filled;
	struct packet_buf *eof;
	void *pulled;

	/* Drops a segment whose header does not fit. */
	tcp = (const struct tcp_wire *)packet->data;
	header_length = (size_t)(tcp->data_offset >> 4) * 4U;
	if (header_length < sizeof(*tcp) || header_length > packet->length) {
		packet_buf_free(packet);
		return EINVAL;
	}

	/* Finds the connection or listener; an unmatched segment is dropped. */
	source_port = wire_get16(tcp->source);
	destination_port = wire_get16(tcp->destination);
	endpoint = tcp_lookup(addresses, source_port, destination_port);
	if (endpoint == NULL) {
		tcp_send_reset(addresses, tcp, packet->length - header_length);
		packet_buf_free(packet);
		return 0;
	}

	sequence = wire_get32(tcp->sequence);
	acknowledgement = wire_get32(tcp->acknowledgement);
	flags = tcp->flags;
	payload_length = packet->length - header_length;
	socket_irq = spin_lock_irqsave(

	    &endpoint->tcp.inet.socket.lock);
	state = endpoint->tcp.state;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/* A listener answers a bare SYN with a half-open child. */
	if (state == TCP_LISTEN) {
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN) {
			offered_mss = tcp_syn_offered_mss(tcp, header_length);
			offered_wscale = tcp_syn_offered_wscale(tcp, header_length);
			offered_sack = tcp_syn_offered_sack(tcp, header_length);
			(void)tcp_passive_syn(endpoint, addresses, source_port, sequence, offered_mss, offered_wscale,
			    offered_sack);
		}
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* A reset closes the connection and fails any pending connect. */
	if (flags & TCP_RST) {
		retransmit.count = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);

		/* Tells a refused connect from a reset connection. */
		reset_error = tcp_reset_error_locked(endpoint, flags,
		    acknowledgement);

		/*
		 * Past the SYN, a reset is taken only at the next sequence
		 * number expected; one elsewhere in the window is challenged,
		 * and one outside it ignored (RFC 5961 section 3.2).
		 */
		reset_check = TCP_RESET_ACCEPT;
		if (endpoint->tcp.state != TCP_SYN_SENT)
			reset_check = tcp_reset_check_locked(endpoint, sequence);
		if (reset_check == TCP_RESET_IGNORE)
			reset_error = 0;

		/* Answers a reset within the window with an acknowledgement, and the connection stays. */
		if (reset_check == TCP_RESET_CHALLENGE) {
			spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock,
			    socket_irq);
			(void)tcp_send_ack(endpoint, 0);
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}

		/* Ignores a reset that does not answer this connection's SYN, or lies outside the window. */
		if (reset_error == 0) {
			spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock,
			    socket_irq);
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}

		tcp_retransmit_reset(endpoint, &retransmit);
		endpoint->tcp.state = TCP_CLOSED;
		endpoint->tcp.active_connect_generation = 0;
		endpoint->tcp.connect_wait_deadline = 0;
		tcp_forget_peer(endpoint);

		/*
		 * A connection that was reset sends no more: after the error is
		 * told, a write is EPIPE (the BSD stacks' socantsendmore), not
		 * ENOTCONN as for a socket never connected.
		 */
		if (reset_error == ECONNRESET)
			endpoint->tcp.inet.socket.write_shutdown = 1;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
		socket_set_error(&endpoint->tcp.inet.socket, reset_error);
		if (endpoint->tcp.listener != NULL) {
			tcp_listener_remove(endpoint);
			packet_buf_free(packet);
			/* Drop the listener queue and lookup references. */
			socket_release(&endpoint->tcp.inet.socket);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}

		socket_wake_connect(&endpoint->tcp.inet.socket);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* A half-open child completes on the handshake ACK, or repeats its SYN-ACK. */
	if (state == TCP_SYN_RECEIVED) {
		retransmit.count = 0;
		resend_sequence = 0;
		resend = 0;
		established = 0;
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
		    sequence + 1U == endpoint->tcp.receive_next) {
			resend = 1;

			/*
			 * The SYN-ACK is the oldest thing outstanding on a
			 * half-open connection, so that is what a repeated
			 * SYN asks to have sent again.
			 */
			resend_sequence = endpoint->tcp.send_count != 0
			    ? endpoint->tcp.send_queue[
				  endpoint->tcp.send_first].sequence
			    : endpoint->tcp.send_next;

			/*
			 * The SYN-ACK sent again is a resend: no sample from
			 * its handshake, and the data starts with three seconds
			 * (ws220-p004 review-1 M3).
			 */
			endpoint->tcp.rtt_timing = 0;
			endpoint->tcp.syn_timed_out = 1;
		}

		/*
		 * The handshake's ACK establishes the connection and gives the
		 * peer's first window, which later segments update by SND.WL1
		 * and SND.WL2 (RFC 9293 section 3.10.7.3).
		 */
		if ((flags & TCP_ACK) != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			tcp_rtt_handshake_locked(endpoint, acknowledgement, sched_ticks());
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_reset(endpoint, &retransmit);
			endpoint->tcp.peer_window = tcp_segment_window_locked(endpoint, tcp);
			endpoint->tcp.send_window_sequence = sequence;
			endpoint->tcp.send_window_acknowledgement = acknowledgement;
			endpoint->tcp.state = TCP_ESTABLISHED;
			endpoint->tcp.last_receive = sched_ticks();
			tcp_keepalive_arm_locked(endpoint);
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
		if (resend)
			(void)tcp_send_segment_at(endpoint, resend_sequence,
			    TCP_SYN | TCP_ACK, NULL, 0);
		if (established) {
			tcp_start_sending(endpoint, endpoint->tcp.peer_mss);
			socket_wake_send(&endpoint->tcp.inet.socket);
			tcp_listener_established(endpoint);
		}

		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/* An active connect completes on the SYN-ACK for its sequence. */
	if (state == TCP_SYN_SENT) {
		retransmit.count = 0;
		established = 0;

		/*
		 * The segment size and the congestion window, from the SYN-ACK's
		 * offer and the route, are in place before the writers the
		 * connection's establishment wakes below can send.
		 */
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK))
			tcp_start_sending(endpoint, tcp_syn_offered_mss(tcp, header_length));
		socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
		if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    endpoint->tcp.active_connect_generation != 0 &&
		    acknowledgement == endpoint->tcp.send_next) {
			tcp_rtt_handshake_locked(endpoint, acknowledgement, sched_ticks());
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retransmit_reset(endpoint, &retransmit);
			endpoint->tcp.receive_next = sequence + 1U;

			/* Scaling is on when the SYN-ACK offers it too; the SYN-ACK's own window is never scaled. */
			offered_wscale = tcp_syn_offered_wscale(tcp, header_length);
			if (offered_wscale >= 0 && endpoint->tcp.wscale_offered != 0) {
				endpoint->tcp.wscale_on = 1;
				endpoint->tcp.peer_wscale = (uint8_t)offered_wscale;
			}

			/* Selective acknowledgements are on when the SYN-ACK offers them too. */
			offered_sack = tcp_syn_offered_sack(tcp, header_length);
			if (endpoint->tcp.sack_offered != 0 && offered_sack)
				endpoint->tcp.sack_on = 1;

			endpoint->tcp.peer_window = wire_get16(tcp->window);
			endpoint->tcp.send_window_sequence = sequence;
			endpoint->tcp.send_window_acknowledgement = acknowledgement;
			endpoint->tcp.state = TCP_ESTABLISHED;
			endpoint->tcp.last_receive = sched_ticks();
			endpoint->tcp.active_connect_generation = 0;
			endpoint->tcp.connect_wait_deadline = 0;
			tcp_keepalive_arm_locked(endpoint);
			waitq_wake_all(&endpoint->tcp.inet.socket.connect_waitq);
			waitq_wake_all(&endpoint->tcp.inet.socket.send_waitq);
			poll_notify();
			established = 1;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		tcp_discard_free(&retransmit);
		if (established)
			(void)tcp_send_ack(endpoint, 0);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/*
	 * A SYN on a connection past its handshake (most often the peer's
	 * SYN-ACK sent again because the handshake's last ACK was lost) is
	 * answered by an acknowledgement and dropped (RFC 5961 section 4): the
	 * peer that lost the ACK completes its handshake on it, and a SYN from
	 * anyone else ends nothing.
	 */
	if ((flags & TCP_SYN) != 0) {
		/* Acknowledges what this end has, and drops the SYN. */
		(void)tcp_send_ack(endpoint, 0);
		packet_buf_free(packet);
		socket_release(&endpoint->tcp.inet.socket);
		return 0;
	}

	/*
	 * Anything arriving from the peer is proof the connection is alive, so
	 * the idle period starts over and the unanswered probes are forgotten.
	 * This includes the peer's answer to a probe of our own.
	 */
	socket_irq = spin_lock_irqsave(&endpoint->tcp.inet.socket.lock);
	if (endpoint->tcp.state == TCP_ESTABLISHED &&
	    endpoint->tcp.inet.socket.keepalive != 0) {
		endpoint->tcp.keepalive_probes = 0;
		endpoint->tcp.keepalive_deadline =
		    sched_ticks() + TCP_KEEPALIVE_IDLE;
	}

	/* The segment of a connection past its handshake is counted for net.tcpstats. */
	endpoint->tcp.counters.segments_in++;

	/* The peer answers: the persist timer does not give it up (ws220-p005). */
	endpoint->tcp.last_receive = sched_ticks();
	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/*
	 * An acknowledgement retires every segment it covers, not only the
	 * oldest, and carries the peer's current window so the next send
	 * knows how much it may put in flight.
	 */
	if ((flags & TCP_ACK) != 0) {
		retransmit.count = 0;
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		endpoint->tcp.counters.acks_in++;

		/*
		 * Where the acknowledgement lies, modulo 2^32 (RFC 9293
		 * section 3.4): past what was handed to the device (the
		 * unsent entries are not; ws220-p003), before what is
		 * outstanding, or within it.
		 */
		sent_end = tcp_sent_end_locked(endpoint);
		acknowledges_unsent = tcp_sequence_before(sent_end, acknowledgement);

		/*
		 * Entries made unsent again after they went (a timeout's
		 * go-back-N, a Packet Too Big) that the peer had after all are
		 * sent as far as it acknowledges them, up to the most ever sent;
		 * not while a thread is sending them.
		 */
		within_max = tcp_sequence_at_or_before(acknowledgement, endpoint->tcp.send_max);
		if (acknowledges_unsent && endpoint->tcp.send_flushing == 0 && within_max) {
			while (endpoint->tcp.send_unsent != 0) {
				slot = (endpoint->tcp.send_first + endpoint->tcp.send_count -
				    endpoint->tcp.send_unsent) % TCP_SEND_QUEUE_MAX;
				before = tcp_sequence_before(endpoint->tcp.send_queue[slot].sequence, acknowledgement);
				if (!before)
					break;
				endpoint->tcp.send_queue[slot].rewound = 0;
				endpoint->tcp.send_queue[slot].fresh = 0;
				endpoint->tcp.send_unsent--;
			}

			/* Where the acknowledgement lies now that those entries are sent. */
			sent_end = tcp_sent_end_locked(endpoint);
			acknowledges_unsent = tcp_sequence_before(sent_end, acknowledgement);
		}
		acknowledges_old = tcp_sequence_before(acknowledgement,
		    endpoint->tcp.send_unacknowledged);

		/*
		 * An acknowledgement of something not yet sent is answered by
		 * an acknowledgement, and the segment dropped (RFC 9293
		 * section 3.10.7.4); net.tcpstats counts it.
		 */
		if (acknowledges_unsent) {
			endpoint->tcp.counters.acks_outside++;
			spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
			(void)tcp_send_ack(endpoint, 0);
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return 0;
		}

		/*
		 * One within what is outstanding retires what it covers.  An
		 * older one, overtaken on the way by a later one, is ignored,
		 * and net.tcpstats counts it; the segment's data and FIN are
		 * still taken below.
		 */
		previous_unacknowledged = endpoint->tcp.send_unacknowledged;
		if (!acknowledges_old) {
			endpoint->tcp.send_unacknowledged = acknowledgement;
			tcp_retire_acknowledged(endpoint, acknowledgement,
			    &retransmit);
		} else {
			endpoint->tcp.counters.acks_outside++;
		}

		/*
		 * The window is taken from an acceptable acknowledgement whose
		 * segment is newer than the one the window last came from
		 * (SND.WL1 and SND.WL2), so an old one cannot set it back.
		 */
		previous_window = endpoint->tcp.peer_window;
		segment_window = tcp_segment_window_locked(endpoint, tcp);
		window_opened = 0;
		window_update = 0;
		if (!acknowledges_old)
			window_update = tcp_send_window_update_allowed_locked(endpoint, sequence, acknowledgement);
		if (window_update) {
			if (segment_window > endpoint->tcp.peer_window)
				window_opened = 1;
			endpoint->tcp.peer_window = segment_window;
			endpoint->tcp.send_window_sequence = sequence;
			endpoint->tcp.send_window_acknowledgement = acknowledgement;

			/* A closed window is counted for net.tcpstats. */
			if (endpoint->tcp.peer_window == 0)
				endpoint->tcp.counters.zero_windows_in++;
		}

		/*
		 * The SACK blocks mark the segments the peer holds above the
		 * acknowledgement (RFC 6675 section 4): how many it newly
		 * reports, or -1 when it carries none.
		 */
		sack_new = -1;
		if (endpoint->tcp.sack_on != 0 && !acknowledges_old)
			sack_new = tcp_sack_take_locked(endpoint, tcp, header_length);

		/*
		 * So is a duplicate acknowledgement (RFC 5681 section 2): one
		 * that takes nothing new while data waits, carries no data, no
		 * SYN or FIN, and leaves the window as it was.  With SACK only
		 * one that reports a segment newly held counts (RFC 6675
		 * section 2): a D-SACK alone answers a resend that was not
		 * needed.
		 */
		sent = tcp_sent_count_locked(endpoint);
		duplicate = 0;
		if (retransmit.count == 0 &&
		    acknowledgement == previous_unacknowledged &&
		    sent != 0 &&
		    endpoint->tcp.peer_window == previous_window &&
		    payload_length == 0 &&
		    (flags & (TCP_SYN | TCP_FIN)) == 0)
			duplicate = 1;

		/* With SACK, an acknowledgement that reports no segment newly held does not count. */
		if (duplicate && sack_new == 0)
			duplicate = 0;

		/*
		 * An acknowledgement into the middle of the oldest segment moved
		 * SND.UNA without taking a segment: it is no duplicate, and the
		 * duplicates in a row start over (review-1 L1).
		 */
		moved = tcp_sequence_before(previous_unacknowledged, acknowledgement);
		if (moved)
			endpoint->tcp.duplicate_acks = 0;

		/*
		 * A duplicate counts towards a fast retransmit only while the
		 * oldest segment fits the window (review-1 G1: the answers to a
		 * closed window are not losses) and outside the duplicates of a
		 * recover a timeout ended at (review-1 M1).  Duplicates that do
		 * not count leave the count as it is (review-1 L2).
		 */
		counted = 0;
		if (duplicate) {
			endpoint->tcp.counters.duplicate_acks_in++;
			counted = tcp_oldest_fits_window_locked(endpoint);
		}

		/* Not the duplicates of the recover a timeout ended at. */
		if (counted && endpoint->tcp.dupack_guard != 0 && acknowledgement == endpoint->tcp.dupack_guard_sequence)
			counted = 0;

		/* One more in a row. */
		if (counted)
			endpoint->tcp.duplicate_acks++;

		/*
		 * The third duplicate in a row outside a recovery has the oldest
		 * segment resent at once (RFC 5681 section 3.2, RFC 6582 section
		 * 3.2) and starts a recovery up to what has been sent.  recover is
		 * not compared outside a recovery (review-1 G2).
		 */
		fast = 0;
		if (counted && endpoint->tcp.duplicate_acks == TCP_DUPLICATE_ACKS_FAST && endpoint->tcp.recovery == 0)
			fast = 1;

		/* The fast retransmit: the window halved, a recovery up to what was sent, and the oldest resent at once. */
		if (fast) {
			tcp_sack_clear_locked(endpoint, 0);
			tcp_cwnd_loss_locked(endpoint, 0, 0);
			endpoint->tcp.recover = tcp_sent_end_locked(endpoint);
			endpoint->tcp.recovery = 1;
			endpoint->tcp.recovery_by_timeout = 0;
			endpoint->tcp.dupack_guard = 0;
			endpoint->tcp.retransmit_deadline = sched_ticks();
			endpoint->tcp.retransmit_immediate = TCP_IMMEDIATE_LOSS;
			endpoint->tcp.counters.fast_retransmits++;
			endpoint->tcp.counters.immediate_resends++;
		}

		/*
		 * The oldest segment went out last as a probe of a window that
		 * could not take it, and the window now takes it (ws220-p005,
		 * after any acknowledgement, review-1 G1): the peer refused it,
		 * so it is sent again now, and the backoff of the closed window
		 * ends (review-1 M3).  Any other reopened window says nothing of
		 * what the peer took, and the resend it used to bring was a
		 * spurious one (ws220-p004 §6).
		 */
		reprobe = 0;
		if (sent != 0 && endpoint->tcp.retransmit_immediate == 0)
			reprobe = tcp_probe_fits_locked(endpoint);
		if (reprobe) {
			endpoint->tcp.send_queue[endpoint->tcp.send_first].probe = 0;
			endpoint->tcp.rto_backoff = 0;
			tcp_persist_leave_locked(endpoint);
			endpoint->tcp.retransmit_deadline = sched_ticks();
			endpoint->tcp.retransmit_immediate = TCP_IMMEDIATE_PROBE;
			endpoint->tcp.counters.immediate_resends++;
		}

		/*
		 * A writer waits for room in flight, which an acknowledgement
		 * makes by retiring segments or by reopening the window; a
		 * window update alone retires nothing but must wake it too.  It
		 * is woken once a good part of the room is free, not for every
		 * acknowledgement: each write then queues many segments.
		 */
		retired = retransmit.count != 0;
		wake = 0;
		if (retired || window_opened)
			wake = tcp_send_wake_due_locked(endpoint);

		/* A fast recovery with SACK resends the holes the blocks show (RFC 6675 section 5). */
		sack_resend = 0;
		if (endpoint->tcp.sack_on != 0 &&
		    endpoint->tcp.recovery != 0 &&
		    endpoint->tcp.recovery_by_timeout == 0)
			sack_resend = 1;

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);
		if (retired)
			tcp_discard_free(&retransmit);
		if (wake)
			socket_wake_send(&endpoint->tcp.inet.socket);

		/* Resends the holes outside the socket lock. */
		if (sack_resend)
			tcp_sack_resend(endpoint);
	}

	/*
	 * In-order payload is queued and then acknowledged; a read shutdown
	 * discards it.  Only payload that was actually queued moves
	 * receive_next: a segment the receive queue has no room for is left
	 * unacknowledged, so the peer sends it again instead of believing it
	 * delivered.  The acknowledgement goes out either way, carrying the
	 * window as it now stands.
	 */
	if (payload_length != 0) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		accept_payload = sequence == endpoint->tcp.receive_next;
		discard_payload = endpoint->tcp.inet.socket.read_shutdown;
		old_payload = tcp_sequence_at_or_before(sequence + (uint32_t)payload_length,
		    endpoint->tcp.receive_next);

		/*
		 * A segment that starts before receive_next and ends after it
		 * (a resend the peer cut differently, or one that overlaps a
		 * part already taken) has its part already taken cut off, and
		 * the rest is taken in order (RFC 9293 section 3.10.7.4).
		 */
		overlap = 0;
		if (!accept_payload && !old_payload) {
			starts_before = tcp_sequence_before(sequence, endpoint->tcp.receive_next);
			if (starts_before)
				overlap = endpoint->tcp.receive_next - sequence;
		}

		/*
		 * What was taken already, wholly or in part, is reported once as
		 * a duplicate (D-SACK, RFC 2883 section 4): the peer learns its
		 * resend was not needed.
		 */
		if (endpoint->tcp.sack_on != 0 &&
		    (old_payload || overlap != 0)) {
			endpoint->tcp.dsack_start = sequence;
			endpoint->tcp.dsack_end = endpoint->tcp.receive_next;

			/* A segment taken wholly is reported to its end, one taken in part to where it was cut. */
			if (old_payload)
				endpoint->tcp.dsack_end = sequence + (uint32_t)payload_length;
			endpoint->tcp.dsack_pending = 1;
		}

		/* The segment then starts at receive_next, shorter by the part cut off. */
		if (overlap != 0) {
			sequence = endpoint->tcp.receive_next;
			payload_length -= overlap;
			accept_payload = 1;
		}

		/* A segment taken already, or one ahead of a gap, is counted for net.tcpstats. */
		if (!accept_payload && old_payload)
			endpoint->tcp.counters.old_segments_in++;
		else if (!accept_payload)
			endpoint->tcp.counters.out_of_order_in++;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/*
		 * A segment wholly before receive_next was already taken; the
		 * peer sends it again because it did not see the acknowledgement,
		 * so it gets one (RFC 793).
		 */
		if (!accept_payload && old_payload) {
			(void)tcp_send_ack(endpoint, 0);
			goto payload_done;
		}

		/*
		 * A segment ahead of receive_next (one before it was lost) is
		 * kept until the gap is filled, and the peer is told of the gap
		 * at once by a duplicate acknowledgement (RFC 5681 section 4.2),
		 * so that it resends the one segment missing after three of them
		 * rather than the whole window after its timer (BUG-222).  A
		 * duplicate acknowledgement retires nothing and restarts no
		 * timer of this end's own sender (tcp_retire_acknowledged).
		 */
		if (!accept_payload) {
			pulled = NULL;
			if (!discard_payload)
				pulled = packet_buf_pull(packet, header_length);
			if (pulled != NULL)
				tcp_reorder_keep(endpoint, &packet, sequence);
			(void)tcp_send_ack(endpoint, 1);
			goto payload_done;
		}

		/* The header comes off, and with it any part of the payload already taken. */
		pulled = packet_buf_pull(packet, header_length + overlap);
		if (pulled == NULL) {
			packet_buf_free(packet);
			socket_release(&endpoint->tcp.inet.socket);
			return EINVAL;
		}

		/*
		 * Queued with receive_next moved past it in the same hold of
		 * the socket lock, so that no segment sent meanwhile tells of
		 * the old receive_next with a window this payload already
		 * narrowed (to the peer a duplicate acknowledgement).
		 */
		queued = 1;
		advanced = 0;
		if (!discard_payload) {
			enqueue_error = socket_enqueue_stream_advance(&endpoint->tcp.inet.socket, packet,
			    &endpoint->tcp.receive_next);
			packet = NULL;
			queued = 0;

			/* A payload that was queued has moved receive_next already. */
			if (enqueue_error == 0) {
				queued = 1;
				advanced = 1;
			}
		}

		/* A discarded payload moves receive_next here; a queued one is counted. */
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);

		/* A payload that was discarded still moves receive_next past it. */
		if (queued && !advanced && endpoint->tcp.receive_next == sequence)
			endpoint->tcp.receive_next += (uint32_t)payload_length;

		/* A payload taken is counted. */
		if (queued)
			endpoint->tcp.counters.data_bytes_in += payload_length;

		/* A payload queued in order may end a measurement of the receive buffer's sizing. */
		if (advanced)
			tcp_receive_tune_locked(endpoint);

		/* A segment the receive queue had no room for is counted for net.tcpstats. */
		if (!queued)
			endpoint->tcp.counters.receive_refused++;

		/* Segments kept ahead of a gap mean this one filled it, or part of it. */
		gap_filled = endpoint->tcp.reorder_count != 0;
		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/*
		 * The segments kept ahead that now follow on, then one
		 * acknowledgement for all: at once for a segment that filled a gap
		 * (RFC 5681 section 4.2) or that the queue refused, else delayed
		 * to the second segment or the end of the batch of input.
		 */
		if (queued)
			tcp_reorder_drain(endpoint, discard_payload);
		if (!queued || gap_filled)
			(void)tcp_send_ack(endpoint, 0);
		else
			tcp_ack_delay(endpoint);
		if (!queued)
			goto fin_done;
	}

payload_done:
	/* An in-order FIN advances the state and queues an end-of-file marker. */
	if ((flags & TCP_FIN) != 0) {
		socket_irq = spin_lock_irqsave(
		    &endpoint->tcp.inet.socket.lock);
		accept_fin = sequence + (uint32_t)payload_length ==
		    endpoint->tcp.receive_next;
		if (accept_fin) {
			endpoint->tcp.receive_next++;
			if (endpoint->tcp.state == TCP_ESTABLISHED)
				endpoint->tcp.state = TCP_CLOSE_WAIT;
			else if (endpoint->tcp.state == TCP_FIN_WAIT_1 ||
			    endpoint->tcp.state == TCP_FIN_WAIT_2)
				endpoint->tcp.state = TCP_TIME_WAIT;
		}

		spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

		/*
		 * A FIN that is not the next thing, most often one sent again
		 * because the acknowledgement of the first was lost, is answered
		 * the same way; left unanswered, the peer repeats it forever.
		 */
		if (!accept_fin) {
			(void)tcp_send_ack(endpoint, 0);
			goto fin_done;
		}

		/* Acknowledges the FIN, and tells the reader of the end. */
		(void)tcp_send_ack(endpoint, 0);
		eof = packet_buf_alloc(0);
		if (eof != NULL)
			(void)socket_enqueue_packet(&endpoint->tcp.inet.socket,
			    eof);
		else
			socket_wake_receive(&endpoint->tcp.inet.socket);
	}

fin_done:
	/* A fully acknowledged FIN of ours advances the closing states. */
	socket_irq = spin_lock_irqsave(

	    &endpoint->tcp.inet.socket.lock);
	if (endpoint->tcp.state == TCP_FIN_WAIT_1 &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_FIN_WAIT_2;
	if (endpoint->tcp.state == TCP_LAST_ACK &&
	    endpoint->tcp.send_unacknowledged == endpoint->tcp.send_next)
		endpoint->tcp.state = TCP_CLOSED;

	spin_unlock_irqrestore(&endpoint->tcp.inet.socket.lock, socket_irq);

	/* Frees a packet that was not queued and drops the lookup reference. */
	if (packet != NULL)
		packet_buf_free(packet);
	socket_release(&endpoint->tcp.inet.socket);
	return 0;
}

/*
 * Answers a segment that belongs to no connection with a reset (RFC 793).
 *
 * The peer of a connection this end has already freed would otherwise
 * repeat its segment, a FIN most often, until its own timers give up.  A
 * reset is never answered, so two ends cannot exchange them forever.
 */
static void
tcp_send_reset(
	const struct tcp_segment_addresses *addresses,
	const struct tcp_wire *segment,
	size_t payload_length)
{
	struct net_route route;
	struct net_device *device;
	struct packet_buf *packet;
	struct tcp_wire *tcp;
	uint32_t sequence;
	uint32_t acknowledgement;
	uint16_t checksum;
	uint8_t flags;
	int error;

	/* Never answers a reset. */
	if ((segment->flags & TCP_RST) != 0)
		return;

	/*
	 * An acknowledging segment is answered from the sequence it expects;
	 * any other is acknowledged past its payload, SYN and FIN.
	 */
	sequence = 0;
	acknowledgement = 0;
	flags = TCP_RST;
	if ((segment->flags & TCP_ACK) != 0) {
		sequence = wire_get32(segment->acknowledgement);
	} else {
		acknowledgement = wire_get32(segment->sequence) + (uint32_t)payload_length;
		if ((segment->flags & TCP_SYN) != 0)
			acknowledgement++;
		if ((segment->flags & TCP_FIN) != 0)
			acknowledgement++;
		flags |= TCP_ACK;
	}

	/* Builds the reset. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	if (packet == NULL)
		return;

	tcp = packet_buf_append(packet, sizeof(*tcp));
	if (tcp == NULL) {
		packet_buf_free(packet);
		return;
	}

	kern_memset(tcp, 0, sizeof(*tcp));
	wire_put16(tcp->source, wire_get16(segment->destination));
	wire_put16(tcp->destination, wire_get16(segment->source));
	wire_put32(tcp->sequence, sequence);
	wire_put32(tcp->acknowledgement, acknowledgement);
	tcp->data_offset = 5U << 4;
	tcp->flags = flags;

	/* An IPv6 segment is answered over IPv6 (ws130-p003). */
	if (addresses->family == AF_INET6) {
		tcp_send_reset6(addresses, packet);
		return;
	}

	/* Finds the interface the peer is reached through. */
	error = route_lookup_ref(addresses->source, &route);
	if (error != 0) {
		packet_buf_free(packet);
		return;
	}

	device = route.device;
	route.device = NULL;
	route_release(&route);
	if (device == NULL) {
		packet_buf_free(packet);
		return;
	}

	/* Sends it; a reset that is lost is sent again for the next segment. */
	checksum = net_checksum_pseudo(addresses->destination, addresses->source, IPPROTO_TCP,
	    packet->data, packet->length);
	wire_put16(tcp->checksum, checksum);
	(void)ipv4_output(device, addresses->source, IPPROTO_TCP, packet);
	net_device_release(device);
}

/*
 * Sends a built reset over IPv6, from the address the segment came to, out
 * of the interface it came in on for a link-local peer.  The packet is
 * consumed.
 */
static void
tcp_send_reset6(
	const struct tcp_segment_addresses *addresses,
	struct packet_buf *packet)
{
	struct net_device *device;
	struct tcp_wire *tcp;
	uint16_t checksum;
	int linklocal;

	/* The interface of a link-local peer. */
	device = NULL;
	linklocal = in6_is_linklocal(&addresses->source6);
	if (linklocal) {
		device = net_device_find_by_index_ref(addresses->ifindex);
		if (device == NULL) {
			packet_buf_free(packet);
			return;
		}
	}

	/* Sends it; a reset that is lost is sent again for the next segment. */
	tcp = (struct tcp_wire *)packet->data;
	checksum = net_checksum_pseudo6(addresses->destination6.s6_addr, addresses->source6.s6_addr, IPPROTO_TCP,
	    packet->data, packet->length);
	wire_put16(tcp->checksum, checksum);
	(void)ipv6_output(device, &addresses->destination6, &addresses->source6, IPPROTO_TCP, 0U, packet);
	if (device != NULL)
		net_device_release(device);
}

/*
 * Keeps a payload that came ahead of receive_next (its header pulled off)
 * in the connection's reorder queue, in sequence order, when it lies
 * within the window and the queue has room (a full queue gives up its
 * farthest segment for a nearer one).  A segment already kept, or none
 * kept, leaves *packet for the caller to free; a kept one is taken.
 */
static void
tcp_reorder_keep(
	struct tcp_endpoint *endpoint,
	struct packet_buf **packet,
	uint32_t sequence)
{
	struct tcp_socket *tcp = &endpoint->tcp;
	struct packet_buf *dropped;
	unsigned long irq;
	uint32_t length;
	uint32_t window;
	uint32_t offset;
	uint32_t held;
	unsigned index;
	unsigned place;
	int before;

	/* The payload's place: within the window this end last advertised. */
	length = (uint32_t)(*packet)->length;
	if (length == 0U)
		return;
	dropped = NULL;
	irq = spin_lock_irqsave(&tcp->inet.socket.lock);

	window = tcp->advertised_window;
	if (window < TCP_RECEIVE_MSS)
		window = TCP_RECEIVE_MSS;
	offset = sequence - tcp->receive_next;
	if (offset == 0U || offset >= window || length > window - offset) {
		spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
		return;
	}

	/* Its place in the order (a segment of the same start is kept already). */
	for (place = 0; place < tcp->reorder_count; place++) {
		if (tcp->reorder_sequence[place] == sequence) {
			/* Reported once as a duplicate (D-SACK, RFC 2883 section 4). */
			if (tcp->sack_on != 0) {
				held = tcp->reorder_length[place];

				/* The duplicate covers the shorter of the two copies. */
				if (held > length)
					held = length;

				/* Records the range for the next acknowledgement, and makes this segment the block reported first. */
				tcp->dsack_start = sequence;
				tcp->dsack_end = sequence + held;
				tcp->dsack_pending = 1;
				tcp->sack_recent = sequence;
				tcp->sack_recent_set = 1;
			}

			spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
			return;
		}

		/* The first one after it, compared modulo 2^32. */
		before = tcp_sequence_before(sequence, tcp->reorder_sequence[place]);
		if (before)
			break;
	}

	/*
	 * A full queue: the farthest goes for a nearer one, or the new one is
	 * not kept.  With SACK none that a block may have reported goes (that
	 * would renege, RFC 2018 section 8): the new one is not kept, and the
	 * peer, which knows the gaps, sends the nearer ones again.
	 */
	if (tcp->reorder_count >= TCP_REORDER_LIMIT) {
		if (place >= tcp->reorder_count || tcp->sack_on != 0) {
			spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
			return;
		}

		/* The farthest taken out. */
		tcp->reorder_count--;
		dropped = tcp->reorder[tcp->reorder_count];
		tcp->reorder[tcp->reorder_count] = NULL;
	}

	/* Kept in its place. */
	for (index = tcp->reorder_count; index > place; index--) {
		tcp->reorder[index] = tcp->reorder[index - 1U];
		tcp->reorder_sequence[index] = tcp->reorder_sequence[index - 1U];
		tcp->reorder_length[index] = tcp->reorder_length[index - 1U];
	}

	/* Into the place made. */
	tcp->reorder[place] = *packet;
	tcp->reorder_sequence[place] = sequence;
	tcp->reorder_length[place] = length;
	tcp->reorder_count++;
	tcp->counters.out_of_order_kept++;
	*packet = NULL;

	/* The block an acknowledgement reports first is the one this joins. */
	tcp->sack_recent = sequence;
	tcp->sack_recent_set = 1;

	spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);

	/* Succeeded: the farthest one given up, when one was. */
	if (dropped != NULL)
		packet_buf_free(dropped);
}

/*
 * Moves the kept segments that now follow receive_next into the receive
 * queue, a part already taken cut off, and moves receive_next past them;
 * those wholly taken already are freed.  With discard (the read side shut
 * down) they are taken and freed.  Stops at the next gap, or at a segment
 * the queue has no room for (freed: the peer sends it again).
 */
static void
tcp_reorder_drain(
	struct tcp_endpoint *endpoint,
	int discard)
{
	struct tcp_socket *tcp = &endpoint->tcp;
	struct packet_buf *packet;
	unsigned long irq;
	uint32_t sequence;
	uint32_t length;
	uint32_t taken;
	unsigned index;
	void *pulled;
	int reaches;
	int error;

	/* Each kept segment at the front, while it reaches receive_next. */
	for (;;) {
		irq = spin_lock_irqsave(&tcp->inet.socket.lock);

		if (tcp->reorder_count == 0U) {
			spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
			return;
		}

		/* The front one still lies after receive_next, compared modulo 2^32: the gap is open. */
		reaches = tcp_sequence_at_or_before(tcp->reorder_sequence[0], tcp->receive_next);
		if (!reaches) {
			spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
			return;
		}

		/* The front one taken out, and how much of it was taken already. */
		packet = tcp->reorder[0];
		sequence = tcp->reorder_sequence[0];
		length = tcp->reorder_length[0];
		for (index = 1; index < tcp->reorder_count; index++) {
			tcp->reorder[index - 1U] = tcp->reorder[index];
			tcp->reorder_sequence[index - 1U] = tcp->reorder_sequence[index];
			tcp->reorder_length[index - 1U] = tcp->reorder_length[index];
		}

		/* The queue one shorter. */
		tcp->reorder_count--;
		tcp->reorder[tcp->reorder_count] = NULL;
		taken = tcp->receive_next - sequence;

		spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);

		/* Wholly taken already, or not wanted: freed. */
		if (taken >= length || discard) {
			if (discard && taken < length) {
				irq = spin_lock_irqsave(&tcp->inet.socket.lock);
				if (tcp->receive_next == sequence + taken)
					tcp->receive_next = sequence + length;
				spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
			}

			/* Freed. */
			packet_buf_free(packet);
			continue;
		}

		/* The part taken already cut off. */
		if (taken != 0U) {
			pulled = packet_buf_pull(packet, taken);
			if (pulled == NULL) {
				packet_buf_free(packet);
				return;
			}
		}

		/*
		 * The rest queued, receive_next past it in the same hold of the
		 * lock (a queue without room frees it: the peer sends it again).
		 */
		error = socket_enqueue_stream_advance(&tcp->inet.socket, packet, &tcp->receive_next);
		if (error != 0)
			return;

		/* Succeeded for this one: counted. */
		irq = spin_lock_irqsave(&tcp->inet.socket.lock);

		tcp->counters.data_bytes_in += length - taken;

		spin_unlock_irqrestore(&tcp->inet.socket.lock, irq);
	}
}

/* Frees the segments a closing connection kept ahead of a gap. */
static void
tcp_reorder_free(
	struct tcp_endpoint *endpoint)
{
	unsigned index;

	/* Each one kept. */
	for (index = 0; index < endpoint->tcp.reorder_count; index++) {
		packet_buf_free(endpoint->tcp.reorder[index]);
		endpoint->tcp.reorder[index] = NULL;
	}

	/* None kept now. */
	endpoint->tcp.reorder_count = 0;
}
