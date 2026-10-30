/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host model of a TCP transfer (ws220-p001).
 *
 * Two hosts, A and B, run the kernel's own TCP (src/kern/net/tcp.c with
 * socket.c and packet-buf.c, built as the kernel builds them; shim.c stands
 * in for the rest of the kernel).  A sends a stream to B over a link of a
 * chosen rate, one-way delay and loss, with a device queue of a chosen
 * length on each side, the way a USB adapter queues frames for its one
 * transfer.  The clock is virtual, so a transfer of a gigabyte takes
 * seconds of the host's time.  Every interval the model prints the rate the
 * reader received at and the state and counts of both ends, so the way the
 * rate falls (and why) can be seen; at the end a summary line, and the exit
 * status says whether the whole stream arrived intact.
 *
 *   tcp-model [--bytes N] [--mbps R] [--delay-us D] [--loss-ab P] [--loss-ba P]
 *             [--queue F] [--seed S] [--wrap-before N] [--interval-ms I]
 *             [--limit-s L] [--reader-kbps K] [--peer-window W]
 *             [--overlap-ab P] [--overlap-bytes K] [--blind-at-ms T]
 *             [--blackhole-at-ms T] [--connect-blackhole] [--second-writer]
 *             [--unresolved-at-ms T --unresolved-ms D] [--connect-unresolved]
 *             [--expect-in-order] [--expect-srtt-us LOW:HIGH] [--lose-first-syn]
 *             [--ack-delay-ms D] [--writer-gap-ms G] [--delay-step-at-ms T --delay-step-us D]
 *             [--reader-pause-at-ms T --reader-pause-ms D]
 *             [--no-route-at-ms T --no-route-ms D]
 *             [--expect-first-srtt-us LOW:HIGH] [--expect-first-rttvar-us LOW:HIGH]
 *             [--expect-srtt-max-us N] [--expect-retransmits-max N] [--expect-retransmits-losses]
 *             [--expect-stall-rto]
 *             [--drop-ab LIST] [--expect-fast-retransmits LOW:HIGH] [--expect-timeouts-max N]
 *             [--close-at-ms T] [--maxunacktime S] [--expect-probe-gap-s LOW:HIGH]
 *             [--bottleneck-mbps R --bottleneck-queue BYTES] [--run-s S]
 *             [--receive-buffer N] [--expect-receive-limit LOW:HIGH] [--second-flow-at-ms T] [--fair-skip-s S]
 *             [--expect-slow-start] [--expect-ca-growth N] [--expect-cwnd-limited SHARE]
 *             [--expect-ssthresh-held] [--cc NAME] [--second-flow-cc NAME] [--measure-from-s S]
 *             [--close-first a|b] [--quiet]
 *
 * --loss-ab is the share (percent) of the frames from A to B (the data) the
 * link loses, --loss-ba of the frames back (the acknowledgements).
 * --wrap-before N starts A's sequence numbers N bytes before they wrap.
 * --peer-window W rewrites the window of every segment from B to A to W,
 * as a peer would advertise whose window does not follow its queue (a
 * Linux receiver after its buffer has grown): A's sender then sees no
 * window open while segments are outstanding.
 * --overlap-ab P moves the start of that share (percent) of A's data
 * segments K bytes (--overlap-bytes, default 100) back, filled with the
 * stream's bytes before it, as a peer that cuts its resends differently
 * would send them (ws220-p002): B must take the part it has not taken yet.
 * --blind-at-ms T gives B, at T, segments that look like A's but that A
 * did not send: a reset within the window but not at the next sequence
 * number, a reset outside the window, and a SYN (RFC 5961); none may end
 * the connection.
 * --blackhole-at-ms T makes the link lose everything from T on, so the
 * time A takes to give up can be seen (RFC 1122's R2, at least 100 s);
 * --connect-blackhole loses A's SYN from the start and reports when the
 * connect gives up (R2 for a SYN, at least 3 minutes).
 *
 * Each frame that leaves a device's queue runs a pass of the network
 * worker, as a USB adapter's transmit completion does
 * (net_device_schedule_poll): what the device refused is sent again then
 * (ws220-p003).
 * --second-writer makes a second writer write on A's connection whenever
 * A's device is given a data segment, while the writer (or the worker) that
 * gave it waits for the device's answer: two threads writing one socket.
 * The stream must still arrive whole and, with --expect-in-order on a link
 * that loses nothing, without a segment arriving ahead of a gap at B.
 * --unresolved-at-ms T --unresolved-ms D refuses every frame A gives its
 * device from T for D as IPv4 refuses one for a neighbour still being
 * resolved (EAGAIN, each refusal an ARP request); the summary counts the
 * refusals.  TCP takes such a segment as lost on the wire (ws220-p003
 * review-1 G1), so the refusals are the ring's segments once and then one
 * for each resend of the timer, and a neighbour that never answers ends
 * the connection after R2.  --connect-unresolved refuses A's frames so
 * from the start: the connect gives up after R2 for a SYN.
 * --expect-srtt-us LOW:HIGH checks A's smoothed round-trip time at the end
 * (ws220-p004) and that net.tcpstats gives it.  --lose-first-syn loses A's
 * first SYN, so the connection starts its data with RFC 6298's 3 seconds;
 * the line "connected" shows A's timeout then.
 * ws220-p004 review-1: --ack-delay-ms D makes B acknowledge as a peer with
 * delayed acknowledgements does: a pure acknowledgement is held up to D,
 * and one that comes while another is held is sent at once in its place
 * (an acknowledgement for every second segment); --writer-gap-ms G makes
 * A's writer write one piece and wait G, so each segment goes alone and its
 * acknowledgement is the delayed one.  --delay-step-at-ms T
 * --delay-step-us D changes the one-way delay to D at T (a round trip that
 * grows past the timeout).  --reader-pause-at-ms T --reader-pause-ms D
 * stops B's reader for D, so B's window closes and A probes it.
 * --no-route-at-ms T --no-route-ms D makes A's IPv4 fail for good
 * (ENETUNREACH) for D; the writer then waits 100 ms and writes the same
 * bytes again, as an application would.  The --expect-* options check A's
 * first round-trip sample (RFC 6298 section 2.2), its largest smoothed
 * round trip and its resends at the end (--expect-retransmits-losses: no
 * more resends than data frames the link lost, ws221-p008); --expect-stall-rto checks that
 * the longest stall is within what the timeouts explain: the doublings of
 * one RTO up to the most the backoff reached (e), (2^(e+1) - 1) RTO + 20 ms
 * (one loss with no backoff: one RTO; a resend lost too, or a loss before
 * the backoff ended (Karn): more).  ws220-p005: --drop-ab LIST loses the
 * data frames from A of those numbers (counted from 1 from the transfer's
 * start, comma separated), so one window can lose two segments;
 * --expect-fast-retransmits LOW:HIGH and --expect-timeouts-max N check A's
 * fast retransmits and its timeouts that really expired (the backoff's
 * steps).  --close-at-ms T makes A's writer stop at T and shut its side
 * (FIN_WAIT_1 behind what is outstanding); --maxunacktime S sets A's
 * TCP_MAXUNACKTIME; the "close:" line says when A's connection closed and
 * B's state, and the "persist:" line the gaps between A's resends after
 * the first (--expect-probe-gap-s checks them, in seconds).  A check that
 * fails prints "check: FAIL".
 *
 * The congestion control: --bottleneck-mbps R --bottleneck-queue BYTES puts
 * a router after A's wire, before the delay: a drop-tail queue of BYTES
 * (the frames that find it full are lost, as a router's congestion loses
 * them, unlike --queue, whose device refuses a frame and TCP sends it
 * again) and a link of R Mbps out of it.  --run-s S sends for S seconds
 * rather than a number of bytes (the stream must still arrive intact).
 * --receive-buffer N sets B's SO_RCVBUF on each connection it accepts.
 * The "receive:" line gives B's receive buffer at the end (it grows by
 * itself unless SO_RCVBUF set one) and the round trip B measured for it;
 * --expect-receive-limit LOW:HIGH checks the buffer's bytes.
 * --second-flow-at-ms T opens a second connection from A to B T into the
 * transfer, which writes and reads as fast as it can over the same
 * devices, wire and router; the "fair:" line gives the two connections'
 * shares and Jain's index of their rates each interval from --fair-skip-s
 * (default 5) seconds after it is up.  Every run checks that A's
 * congestion window ends within twice the most it had sent and not had
 * acknowledged (and two segments), the "cc:" line saying so, with the
 * share of the time the congestion window was what limited A (what A had
 * sent within a segment of it, below the peer's window); --expect-cwnd-
 * limited SHARE checks that share.  --expect-slow-start checks that while
 * A's slow start is limited by its window it doubles at least every three
 * round trips (A's estimate lags a queue that fills); --expect-ca-growth N that in each span of congestion
 * avoidance of eight round trips or more (the window limiting A for 80 %
 * of it) A's window grows by at least N segments a round trip;
 * --expect-ssthresh-held that a timeout repeated before a round-trip
 * sample leaves the slow start threshold as the timeout before it left
 * it (RFC 5681 section 3.1), the "timeouts:" line counting them.
 * --cc NAME and --second-flow-cc NAME set TCP_CONGESTION ("reno",
 * "cubic") on A's first and second connections; --measure-from-s S prints
 * the "steady:" line, the reader's rate from S seconds in to the end.
 *
 * --close-first a|b closes the connection after the transfer as close(2)
 * does, A's end first or B's (the accepted one), with B's listener left
 * open: each end's file close sends its FIN, waits while what it sent is
 * outstanding (the kernel's drain, at most TCP_CLOSE_DRAIN_MS), and the
 * end is freed.  The "close-order:" line says how long each end's close
 * waited; the second end's must end within a second, by the peer's
 * acknowledgement or its reset, not by the drain's time running out.
 */

#include "kern/net/tcp-socket.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/net/socket.h"
#include "model.h"
#include "src/kern/net/internal.h"

#include <uapi/errno.h>
#include <uapi/netinet.h>
#include <uapi/sysctl.h>

/* The host C library functions the model calls, declared by hand (the kernel headers are on the path). */
extern int printf(const char *format, ...);
extern void *malloc(size_t size);
extern void free(void *pointer);
extern void *memcpy(void *destination, const void *source, size_t count);
extern void *memset(void *destination, int value, size_t count);
extern int strcmp(const char *left, const char *right);
extern size_t strlen(const char *text);
extern double strtod(const char *text, char **end);
extern unsigned long long strtoull(const char *text, char **end, int base);

/* The port B listens on. */
#define MODEL_PORT 5001U

/* The most frames a device queue may be given. */
#define MODEL_QUEUE_MAX 256U

/* A nanosecond time that never comes. */
#define MODEL_NEVER UINT64_MAX

/* How long a writer the stack refused for want of packets sleeps before it tries again (tcp.c's TCP_SEND_RETRY_MS). */
#define MODEL_SEND_RETRY_NS 10000000ULL

/* The bytes a frame takes on the wire beyond the TCP segment: IPv4, Ethernet, its FCS, preamble and gap. */
#define MODEL_FRAME_OVERHEAD 58U

/* One frame on its way: what it carries, between which hosts, and when it arrives. */
struct model_frame {
	struct model_frame *next;
	uint64_t arrive_ns;
	uint32_t source;
	uint32_t destination;
	size_t length;
	uint8_t data[PACKET_BUF_MAX_STORAGE_SIZE];
};

/*
 * One direction of the link: the device queue of the sending host (the
 * packets it still holds, the oldest on the wire), the frames in flight,
 * and what happened to them.
 */
struct model_link {
	struct packet_buf *queue[MODEL_QUEUE_MAX];
	uint32_t queue_source[MODEL_QUEUE_MAX];
	uint32_t queue_destination[MODEL_QUEUE_MAX];
	unsigned queue_first;
	unsigned queue_count;
	uint64_t transmit_end_ns;
	struct model_frame *flight_head;
	struct model_frame *flight_tail;
	double loss;
	uint64_t frames;
	uint64_t queue_full;
	uint64_t lost;
	uint64_t receive_no_packet;
};

/* The model's settings, from the command line. */
struct model_settings {
	uint64_t bytes;
	double mbps;
	uint64_t delay_ns;
	double loss_ab;
	double loss_ba;
	unsigned queue;
	uint64_t seed;
	uint64_t wrap_before;
	uint64_t interval_ns;
	uint64_t limit_ns;
	double reader_kbps;
	unsigned peer_window;
	/* Both devices' MTU (0: Ethernet's 1500), and the writer's piece (--write-bytes, 1024 to 65536). */
	unsigned mtu;
	unsigned write_bytes;
	/* The segment size both ends are to settle on (--expect-mss, 0: not checked). */
	unsigned expect_mss;
	double overlap_ab;
	unsigned overlap_bytes;
	uint64_t blind_at_ns;
	uint64_t blackhole_at_ns;
	int connect_blackhole;
	int connect_unresolved;
	int second_writer;
	uint64_t unresolved_at_ns;
	uint64_t unresolved_ns;
	int expect_in_order;
	uint64_t expect_srtt_low;
	uint64_t expect_srtt_high;
	int lose_first_syn;
	uint64_t ack_delay_ns;
	uint64_t writer_gap_ns;
	uint64_t delay_step_at_ns;
	uint64_t delay_step_ns;
	uint64_t reader_pause_at_ns;
	uint64_t reader_pause_ns;
	uint64_t no_route_at_ns;
	uint64_t no_route_ns;
	uint64_t expect_first_srtt_low;
	uint64_t expect_first_srtt_high;
	uint64_t expect_first_rttvar_low;
	uint64_t expect_first_rttvar_high;
	uint64_t expect_srtt_max;
	uint64_t expect_retransmits_max;
	int expect_retransmits;
	int expect_retransmits_losses;
	int expect_stall_rto;
	uint64_t drop_ab[16];
	unsigned drop_ab_count;
	/* --drop-ab-run AT_MS:US (ws221-p008): A's data frames lost for US microseconds from AT_MS into the transfer, a burst. */
	uint64_t drop_run_first;
	uint64_t drop_run_count;
	/* --expect-stall-max-ms N: the reader got nothing for no longer than N ms at a time. */
	uint64_t expect_stall_max_ms;
	uint64_t expect_fast_low;
	uint64_t expect_fast_high;
	int expect_fast;
	uint64_t expect_timeouts_max;
	int expect_timeouts;
	uint64_t close_at_ns;
	unsigned maxunacktime;
	uint64_t expect_gap_low;
	uint64_t expect_gap_high;
	/*
	 * --pool-held N (ws221-p008): the packets other senders' copies hold,
	 * taken as they take them until the pool refuses one, less N left to
	 * A's own; -1 (the default) takes none.
	 */
	long pool_held_free;
	/* --send-buffer N (ws221-p008): A's SO_SNDBUF in bytes, 0 for the default. */
	unsigned send_buffer;
	/*
	 * --nodelay (ws221-p008): A sets TCP_NODELAY; --expect-segments-max N:
	 * A sent no more than N data segments (Nagle's algorithm joins short
	 * writes), --expect-segments-min N: no fewer.
	 */
	int nodelay;
	uint64_t expect_segments_max;
	uint64_t expect_segments_min;
	/* The router after A's wire (--bottleneck-mbps, 0: none) and its queue in bytes (--bottleneck-queue). */
	double bottleneck_mbps;
	uint64_t bottleneck_queue_bytes;
	/* How long a run lasts when it is timed (--run-s, 0: until the bytes have arrived). */
	uint64_t run_ns;
	/* B's SO_RCVBUF on each accepted connection (--receive-buffer, 0: the default). */
	unsigned receive_buffer;
	/* The bytes B's receive buffer ends within (--expect-receive-limit; expect_receive 0: not checked). */
	int expect_receive;
	uint64_t expect_receive_low;
	uint64_t expect_receive_high;
	/* When a second connection opens (--second-flow-at-ms), and how long after it is up the shares are counted. */
	uint64_t second_flow_at_ns;
	int second_flow;
	uint64_t fair_skip_ns;
	/* The congestion window's checks: slow start, congestion avoidance's growth, and its share as the limit. */
	/* A's congestion control on the first connection and on the second (--cc, --second-flow-cc; NULL: the default). */
	const char *cc;
	const char *second_cc;
	/* When the steady rate starts to be counted (--measure-from-s), after the first slow start. */
	uint64_t measure_from_ns;
	int expect_slow_start;
	int expect_ssthresh_held;
	double expect_ca_growth;
	double expect_cwnd_limited;
	/* Which end closes first after the transfer (--close-first: 'a', 'b'; 0: neither closes). */
	int close_first;
	int quiet;
};

/*
 * The second connection (--second-flow-at-ms): its two ends, the bytes
 * written and read (checked against the stream as the first's are), and
 * whether its connect is under way.
 */
struct model_flow {
	struct socket *sender;
	struct socket *receiver;
	uint64_t up_ns;
	uint64_t written;
	uint64_t received;
	uint64_t corrupt;
	int connecting;
};

/*
 * What the model sees of A's congestion window: the most A had sent and
 * not had acknowledged, the time the window limited A (of the time
 * measured), the slow start's doublings and the slowest of them in round
 * trips, and the spans of congestion avoidance with their slowest growth.
 */
struct model_cc {
	uint32_t sent_flight_max;
	uint64_t measured_ns;
	uint64_t limited_ns;
	uint64_t last_ns;
	uint32_t double_from;
	uint64_t double_since_ns;
	unsigned doublings;
	double double_rtts_max;
	uint64_t span_start_ns;
	uint32_t span_cwnd;
	uint64_t span_limited_ns;
	uint32_t span_cwnd_last;
	uint32_t span_ssthresh;
	int span_open;
	unsigned spans;
	double span_growth_min;
};

/* The most intervals whose rates the shares of two connections are counted from. */
#define MODEL_INTERVALS_MAX 4096U

/* The settings in force. */
static struct model_settings settings;

/* The two directions: 0 from A to B, 1 from B to A. */
static struct model_link links[2];

/* When the router after A's wire (--bottleneck-mbps) has sent what its queue holds, and the frames it lost. */
static uint64_t bottleneck_free_ns;
static uint64_t bottleneck_drops;

/* The second connection (--second-flow-at-ms). */
static struct model_flow second;

/* What the model sees of A's congestion window. */
static struct model_cc cc;

/* The bytes each connection's reader took each interval, for their shares (--second-flow-at-ms). */
static uint64_t interval_first[MODEL_INTERVALS_MAX];
static uint64_t interval_second[MODEL_INTERVALS_MAX];
static uint8_t interval_second_up[MODEL_INTERVALS_MAX];
static unsigned interval_count;

/* The time now, in nanoseconds. */
static uint64_t now_ns;

/* The state of the random numbers that decide losses. */
static uint64_t random_state;

/* The sockets: A's connection, B's listener, and B's accepted connection. */
static struct socket *sender;
static struct socket *listener;
static struct socket *receiver;

/* The writer on A: bytes still to write, the next byte's offset, and when it may try again after a refusal. */
static uint64_t send_left;
static uint64_t send_offset;
static uint64_t send_sleep_until_ns;

/*
 * The bytes of the writes still inside the stack (a second writer's place in
 * the stream is after them), and whether a second writer is writing now.
 */
static uint64_t writer_reserved;
static int second_writer_active;

/* A frame left a device's queue since the last pass of the network worker, which its completion runs. */
static int worker_pass;

/* When the transfer began, and the frames A's device refused as for an unresolved neighbour. */
static uint64_t transfer_start_ns;
static uint64_t unresolved_refusals;

/* Whether A's first SYN has been lost (--lose-first-syn). */
static int first_syn_lost;

/* B's pure acknowledgement held by --ack-delay-ms (NULL: none), and when it goes. */
static struct packet_buf *held_ack;
static uint32_t held_ack_source;
static uint32_t held_ack_destination;
static uint64_t held_ack_until_ns;

/* A's first round-trip sample as it stood when taken, whether it was taken, and A's largest SRTT. */
static uint32_t first_srtt_us;
static uint32_t first_rttvar_us;
static int first_sample_seen;
static uint32_t srtt_max_us;

/* Whether the one-way delay has been changed (--delay-step-at-ms), and A's writes refused for want of a route. */
static int delay_stepped;
static uint64_t no_route_refusals;

/*
 * The most times A resent one segment in a row, how often a resend was lost
 * again (the count reached 2), and the most A's timeout was doubled.
 */
static unsigned backoff_max;
static unsigned resends_in_row_max;
static uint64_t double_losses;
static unsigned resends_in_row_last;

/* A's data frames that have left its device since the transfer began (--drop-ab), and A's timeouts. */
static uint64_t data_frames_ab;
static uint64_t timeouts;
static unsigned backoff_last;

/* Timeouts repeated before a sample, the threshold the last timeout left, and the repeats that moved it. */
static uint64_t repeated_timeouts;
static uint32_t ssthresh_after_timeout;
static uint64_t ssthresh_moved;

/*
 * When A's connection closed (0: not), whether A's writer has shut its
 * side, and A's resends: how many have been seen and when the last was,
 * and the least and most time between two of them after the first.
 */
static uint64_t closed_at_ns;
static int writer_closed;
static uint64_t resends_seen;
static uint64_t resend_last_ns;
static uint64_t resend_gaps;
static int previous_in_persist;
static uint64_t gap_min_ns;
static uint64_t gap_max_ns;

/* The reader on B: bytes read, bytes that differed from what was written, and the bytes it may read now. */
static uint64_t received;
static uint64_t corrupt;
static double read_budget;
static uint64_t read_budget_ns;

/* The writer's or the reader's error, which ends the transfer. */
static int transfer_error;

/* The sequence number of the stream's first byte on A, which places a segment in the stream. */
static uint32_t stream_sequence;

/* How many of A's segments were moved back to overlap, and whether the blind segments were given. */
static uint64_t overlapped;
static int blind_given;

static int tcp_socket_create_inet(int type, int protocol, struct socket **result);
static void parse_settings(int argc, char **argv);
static void parse_range(const char *text, uint64_t *low, uint64_t *high);
static void parse_list(const char *text, uint64_t *list, unsigned *count);
static void note_gap(uint64_t gap);
static int dropped_on_purpose(struct model_link *link, const struct packet_buf *packet);
static uint64_t isn_tick_base(uint64_t wrap_before);
static double random_unit(void);
static uint8_t stream_byte(uint64_t offset);
static uint64_t frame_time_ns(size_t length, double mbps);
static uint64_t bottleneck_arrival(size_t length, uint64_t end_ns, int *dropped);
static void run_second_flow(void);
static uint32_t sent_flight(const struct tcp_socket *tcp);
static void watch_cc(void);
static void watch_cc_span(const struct tcp_socket *tcp, int open, int limited, uint64_t elapsed_ns);
static int check_cc(void);
static int check_fair(void);
static void link_start(struct model_link *link, uint64_t start_ns);
static void link_advance(struct model_link *link);
static void rewrite_window(struct model_frame *frame);
static void overlap_frame(struct model_frame *frame);
static void give_blind_segments(void);
static void give_blind_segment(uint32_t sequence, uint32_t acknowledgement, uint8_t flags);
static void run_applications(void);
static void run_writer(void);
static void run_reader(void);
static int connect_hosts(void);
static void step(void);
static int link_enqueue(unsigned host, struct packet_buf *packet, uint32_t source, uint32_t destination);
static int pure_ack(const struct packet_buf *packet);
static void report(uint64_t elapsed_ns, uint64_t interval_bytes);
static struct tcp_socket *tcp_of(struct socket *socket);
static int check_tcpstats(void);
static uint64_t close_end(struct socket *socket);
static int close_in_order(void);

/*
 * Runs the transfer and reports how it went.
 */
int
main(
	int argc,
	char **argv)
{
	struct socket_family_ops family;
	struct packet_buf_pool_usage usage;
	static struct packet_buf *held[PACKET_BUF_POOL_COUNT];
	struct packet_buf *held_packet;
	unsigned held_count;
	size_t send_bytes_max;
	int usage_jumbo_check;
	uint64_t next_report_ns;
	uint64_t last_received;
	uint64_t last_second_received;
	uint64_t measure_mark_ns;
	uint64_t measure_mark_bytes;
	double steady;
	uint64_t progress_mark;
	uint64_t last_progress_ns;
	uint64_t longest_stall_ns;
	uint64_t start_ns;
	uint64_t interval_bytes;
	uint64_t out_of_order;
	int option_value;
	char cc_name[TCP_CA_NAME_MAX];
	socklen_t cc_length;
	int compare;
	int same;
	double closed_s;
	int b_state;
	struct tcp_socket *a;
	double allowed_ms;
	double rate;
	double min_rate;
	double mean;
	unsigned slow_intervals;
	unsigned intervals;
	int error;
	int complete;

	/* The settings, and the clock that sets the sender's first sequence number. */
	parse_settings(argc, argv);
	random_state = settings.seed * 2654435761ULL + 1U;
	model_kernel_init(isn_tick_base(settings.wrap_before));
	if (settings.mtu != 0)
		model_device_mtu_set(settings.mtu);
	links[0].loss = settings.loss_ab / 100.0;
	links[1].loss = settings.loss_ba / 100.0;
	links[0].transmit_end_ns = MODEL_NEVER;
	links[1].transmit_end_ns = MODEL_NEVER;

	/* The kernel's socket core, packet pool and TCP, with IPv4 sockets made by TCP. */
	socket_core_init();
	packet_buf_pool_init();
	error = tcp_init();
	if (error != 0) {
		printf("tcp-model: tcp_init failed (%d)\n", error);
		return 2;
	}
	family.create = tcp_socket_create_inet;
	(void)socket_family_register(AF_INET, &family);

	/* The connection; with its SYN lost or refused, only the time the connect gives up is wanted. */
	if (settings.connect_blackhole)
		links[0].loss = 1.0;
	error = connect_hosts();
	if (settings.connect_blackhole || settings.connect_unresolved) {
		printf("tcp-model: summary connect gave up after %.3f s (state %d, error %d) unresolved_refusals=%llu "
		    "complete=%d mean_mbs=0\n", (double)now_ns / 1e9, (int)tcp_of(sender)->state, error,
		    (unsigned long long)unresolved_refusals, error != 0 && now_ns >= 180000000000ULL);
		return 0;
	}
	if (error != 0) {
		printf("tcp-model: no connection (%d)\n", error);
		return 2;
	}
	stream_sequence = tcp_of(sender)->send_unacknowledged;
	transfer_start_ns = now_ns;

	/* The copies of other senders to slow peers, which hold all the pool lets them, less what A is left. */
	held_count = 0;
	if (settings.pool_held_free >= 0) {
		for (;;) {
			held_packet = packet_buf_alloc_for(64U, 1460U, PACKET_BUF_ALLOC_HOLD, &error);
			if (held_packet == NULL || held_count == sizeof(held) / sizeof(held[0])) {
				if (held_packet != NULL)
					packet_buf_free(held_packet);
				break;
			}
			held[held_count++] = held_packet;
		}
		while (settings.pool_held_free > 0 && held_count != 0) {
			packet_buf_free(held[--held_count]);
			settings.pool_held_free--;
		}

		/* Says how many packets the other senders keep. */
		printf("tcp-model: pool held by other senders: %u packets\n", held_count);
	}

	/* A's congestion control, when the scenario names one. */
	if (settings.cc != NULL) {
		error = sender->ops->setsockopt(sender, IPPROTO_TCP, TCP_CONGESTION, settings.cc,
		    (socklen_t)strlen(settings.cc));
		if (error != 0) {
			printf("tcp-model: TCP_CONGESTION %s refused (%d)\n", settings.cc, error);
			return 2;
		}

		/* Read back as FreeBSD gives it: the name and its NUL. */
		cc_length = sizeof(cc_name);
		error = sender->ops->getsockopt(sender, IPPROTO_TCP, TCP_CONGESTION, cc_name, &cc_length);

		/* Reads back the name, no other length, no other bytes. */
		same = 0;
		if (error == 0 && cc_length == strlen(settings.cc) + 1U) {
			compare = strcmp(cc_name, settings.cc);
			if (compare == 0)
				same = 1;
		}
		if (!same) {
			printf("check: FAIL: TCP_CONGESTION reads back %d (%u bytes), not %s\n", error, (unsigned)cc_length,
			    settings.cc);
			return 1;
		}
	}

	/* A's TCP_NODELAY, when the scenario sets it. */
	if (settings.nodelay) {
		option_value = 1;
		error = sender->ops->setsockopt(sender, IPPROTO_TCP, TCP_NODELAY, &option_value, sizeof(option_value));
		if (error != 0) {
			printf("tcp-model: TCP_NODELAY refused (%d)\n", error);
			return 2;
		}
	}

	/* A's send buffer, when the scenario sets one. */
	if (settings.send_buffer != 0) {
		option_value = (int)settings.send_buffer;
		error = socket_setsockopt_common(sender, SOL_SOCKET, SO_SNDBUF, &option_value, sizeof(option_value));
		if (error != 0) {
			printf("tcp-model: SO_SNDBUF refused (%d)\n", error);
			return 2;
		}
	}

	/* A's TCP_MAXUNACKTIME, when the scenario sets one. */
	if (settings.maxunacktime != 0) {
		option_value = (int)settings.maxunacktime;
		error = sender->ops->setsockopt(sender, IPPROTO_TCP, TCP_MAXUNACKTIME, &option_value, sizeof(option_value));
		if (error != 0) {
			printf("tcp-model: TCP_MAXUNACKTIME refused (%d)\n", error);
			return 2;
		}
	}

	/* The connection, and A's timeout as it starts. */
	printf("tcp-model: connected at %llu us, A's first sequence number %u, rto_ms=%llu srtt_us=%u\n",
	    (unsigned long long)(now_ns / 1000U), stream_sequence,
	    (unsigned long long)kern_ticks_to_ms(tcp_of(sender)->rto), tcp_of(sender)->srtt_us);

	/* The transfer, reported each interval until the reader has it all or the time is up. */
	send_left = settings.bytes;
	start_ns = now_ns;
	next_report_ns = now_ns + settings.interval_ns;
	last_received = 0;
	last_second_received = 0;
	measure_mark_ns = 0;
	measure_mark_bytes = 0;
	progress_mark = 0;
	last_progress_ns = now_ns;
	longest_stall_ns = 0;
	min_rate = -1.0;
	slow_intervals = 0;
	intervals = 0;
	send_bytes_max = 0;
	while (received < settings.bytes && transfer_error == 0 && now_ns - start_ns < settings.limit_ns) {
		step();
		if (tcp_of(sender)->send_bytes > send_bytes_max)
			send_bytes_max = tcp_of(sender)->send_bytes;

		/* The link that loses everything from its time on. */
		if (settings.blackhole_at_ns != 0 && now_ns - start_ns >= settings.blackhole_at_ns) {
			links[0].loss = 1.0;
			links[1].loss = 1.0;
		}

		/* The blind segments, once, at their time. */
		if (settings.blind_at_ns != 0 && !blind_given && now_ns - start_ns >= settings.blind_at_ns) {
			give_blind_segments();
			blind_given = 1;
		}

		/* What the reader had when the steady rate starts to be counted. */
		if (settings.measure_from_ns != 0 && measure_mark_ns == 0 && now_ns - start_ns >= settings.measure_from_ns) {
			measure_mark_ns = now_ns;
			measure_mark_bytes = received;
		}

		/* The longest time the reader got nothing. */
		if (received != progress_mark) {
			if (now_ns - last_progress_ns > longest_stall_ns)
				longest_stall_ns = now_ns - last_progress_ns;
			last_progress_ns = now_ns;
			progress_mark = received;
		}

		/* An interval's report: the rate, and the state of both ends. */
		if (now_ns >= next_report_ns) {
			interval_bytes = received - last_received;
			rate = (double)interval_bytes / 1048576.0 / ((double)settings.interval_ns / 1e9);
			if (min_rate < 0.0 || rate < min_rate)
				min_rate = rate;
			if (rate < 1.0)
				slow_intervals++;
			intervals++;
			if (!settings.quiet)
				report(now_ns - start_ns, interval_bytes);

			/* Each connection's bytes in the interval, and whether the second was up long enough for its share. */
			if (interval_count < MODEL_INTERVALS_MAX) {
				interval_first[interval_count] = interval_bytes;
				interval_second[interval_count] = second.received - last_second_received;
				interval_second_up[interval_count] = 0;
				if (second.up_ns != 0 && second.up_ns + settings.fair_skip_ns <= next_report_ns - settings.interval_ns)
					interval_second_up[interval_count] = 1;
				interval_count++;
			}

			/* Moves the marks on to the next interval. */
			last_second_received = second.received;
			last_received = received;
			next_report_ns += settings.interval_ns;
		}
	}

	/* A connection A has closed: a second more, so its reset reaches B. */
	if (closed_at_ns != 0) {
		while (now_ns < closed_at_ns + 1000000000ULL)
			step();
	}

	/* A stall still going at the end counts too. */
	if (now_ns - last_progress_ns > longest_stall_ns)
		longest_stall_ns = now_ns - last_progress_ns;

	/* The rate from --measure-from-s on (the "steady:" line). */
	if (measure_mark_ns != 0 && now_ns > measure_mark_ns) {
		steady = (double)(received - measure_mark_bytes) / 1048576.0 / ((double)(now_ns - measure_mark_ns) / 1e9);
		printf("steady: from_s=%.3f mbs=%.3f\n", (double)(measure_mark_ns - start_ns) / 1e9, steady);
	}

	/* The summary: the stream whole, or for a timed run every byte that came as written. */
	complete = received == settings.bytes && corrupt == 0;
	if (settings.run_ns != 0)
		complete = received != 0 && corrupt == 0 && second.corrupt == 0 && transfer_error == 0;
	mean = 0.0;
	if (now_ns > start_ns)
		mean = (double)received / 1048576.0 / ((double)(now_ns - start_ns) / 1e9);
	/* The longest stall, against what the scenario expects. */
	if (settings.expect_stall_max_ms != 0 && longest_stall_ns > settings.expect_stall_max_ms * 1000000ULL)
		printf("check: FAIL: the reader got nothing for %.1f ms, more than %llu\n", (double)longest_stall_ns / 1e6,
		    (unsigned long long)settings.expect_stall_max_ms);

	/* The data segments A sent, against what the scenario expects of Nagle's algorithm. */
	if (settings.expect_segments_max != 0 || settings.expect_segments_min != 0) {
		printf("tcp-model: data segments %llu\n", (unsigned long long)tcp_of(sender)->counters.data_segments_out);
		if (settings.expect_segments_max != 0 &&
		    tcp_of(sender)->counters.data_segments_out > settings.expect_segments_max)
			printf("check: FAIL: A sent %llu data segments, more than %llu\n",
			    (unsigned long long)tcp_of(sender)->counters.data_segments_out,
			    (unsigned long long)settings.expect_segments_max);
		if (tcp_of(sender)->counters.data_segments_out < settings.expect_segments_min)
			printf("check: FAIL: A sent %llu data segments, fewer than %llu\n",
			    (unsigned long long)tcp_of(sender)->counters.data_segments_out,
			    (unsigned long long)settings.expect_segments_min);
	}

	/* A's ring held no more payload than its send buffer. */
	if (settings.send_buffer != 0) {
		printf("tcp-model: send buffer %u, most queued %zu\n", settings.send_buffer, send_bytes_max);
		if (send_bytes_max > settings.send_buffer)
			printf("check: FAIL: A queued %zu bytes, more than its send buffer of %u\n", send_bytes_max,
			    settings.send_buffer);
	}

	/* B's receive buffer at the end, against what the scenario expects of its sizing. */
	if (receiver != NULL) {
		printf("receive: b_limit_bytes=%zu b_packet_limit=%zu b_extra_packets=%u b_rtt_us=%u\n",
		    tcp_of(receiver)->inet.socket.receive_hiwat_bytes, tcp_of(receiver)->inet.socket.receive_packet_limit,
		    tcp_of(receiver)->receive_extra_packets, tcp_of(receiver)->receive_rtt_us);
		if (settings.expect_receive &&
		    (tcp_of(receiver)->inet.socket.receive_hiwat_bytes < settings.expect_receive_low ||
		     tcp_of(receiver)->inet.socket.receive_hiwat_bytes > settings.expect_receive_high))
			printf("check: FAIL: B's receive buffer is %zu bytes, not within %llu..%llu\n",
			    tcp_of(receiver)->inet.socket.receive_hiwat_bytes, (unsigned long long)settings.expect_receive_low,
			    (unsigned long long)settings.expect_receive_high);
	}

	/* With the pool held by other senders, the frames that arrived still found a packet. */
	if (held_count != 0 || settings.pool_held_free >= 0) {
		printf("tcp-model: pool held=%u receive_no_packet=%llu\n", held_count,
		    (unsigned long long)(links[0].receive_no_packet + links[1].receive_no_packet));
		if (links[0].receive_no_packet + links[1].receive_no_packet != 0)
			printf("check: FAIL: %llu frames found no packet while other senders held the pool\n",
			    (unsigned long long)(links[0].receive_no_packet + links[1].receive_no_packet));
	}
	while (held_count != 0)
		packet_buf_free(held[--held_count]);

	/* B's segments that came ahead of a gap. */
	out_of_order = 0;
	if (receiver != NULL)
		out_of_order = tcp_of(receiver)->counters.out_of_order_in;

	/* The summary line, which run.sh reads. */
	printf("tcp-model: summary bytes=%llu received=%llu corrupt=%llu seconds=%.3f mean_mbs=%.2f min_interval_mbs=%.2f "
	    "intervals=%u below_1mbs=%u longest_stall_ms=%.1f overlapped=%llu b_out_of_order=%llu unresolved_refusals=%llu "
	    "error=%d complete=%d\n",
	    (unsigned long long)settings.bytes, (unsigned long long)received, (unsigned long long)corrupt,
	    (double)(now_ns - start_ns) / 1e9, mean, min_rate < 0.0 ? 0.0 : min_rate, intervals, slow_intervals,
	    (double)longest_stall_ns / 1e6, (unsigned long long)overlapped,
	    (unsigned long long)out_of_order, (unsigned long long)unresolved_refusals, transfer_error, complete);
	report(now_ns - start_ns, 0);

	/* When A's connection closed and how B stands, and the gaps between A's resends (ws220-p005). */
	closed_s = -1.0;
	if (closed_at_ns != 0)
		closed_s = (double)(closed_at_ns - start_ns) / 1e9;
	b_state = -1;
	if (receiver != NULL)
		b_state = (int)tcp_of(receiver)->state;
	printf("close: a_closed_at_s=%.3f b_state=%d\n", closed_s, b_state);
	printf("persist: resends=%llu gaps=%llu gap_min_s=%.3f gap_max_s=%.3f\n", (unsigned long long)resends_seen,
	    (unsigned long long)resend_gaps, (double)gap_min_ns / 1e9, (double)gap_max_ns / 1e9);
	if (settings.expect_gap_high != 0 &&
	    (resend_gaps == 0 || gap_min_ns < settings.expect_gap_low * 1000000000ULL ||
	     gap_max_ns > settings.expect_gap_high * 1000000000ULL)) {
		printf("check: FAIL: the gaps between A's resends are not within %llu..%llu s\n",
		    (unsigned long long)settings.expect_gap_low, (unsigned long long)settings.expect_gap_high);
		return 1;
	}

	/* A link that loses nothing delivers A's segments in the order A queued them. */
	if (settings.expect_in_order && out_of_order != 0) {
		printf("check: FAIL: %llu segments reached B ahead of a gap on a link that loses nothing\n",
		    (unsigned long long)out_of_order);
		return 1;
	}

	/* A's round-trip estimate and resends (ws220-p004). */
	a = tcp_of(sender);
	printf("rtt: samples=%u srtt_us=%u rttvar_us=%u rto_ms=%llu backoff=%u first_srtt_us=%u first_rttvar_us=%u "
	    "srtt_max_us=%u retransmits=%llu immediate=%llu fast=%llu timeouts=%llu no_route_refusals=%llu\n",
	    a->rtt_samples, a->srtt_us, a->rttvar_us, (unsigned long long)kern_ticks_to_ms(a->rto), a->rto_backoff,
	    first_srtt_us, first_rttvar_us, srtt_max_us, (unsigned long long)a->counters.retransmit_segments,
	    (unsigned long long)a->counters.immediate_resends, (unsigned long long)a->counters.fast_retransmits,
	    (unsigned long long)timeouts, (unsigned long long)no_route_refusals);

	/* A's fast retransmits, when the scenario says how many (ws220-p005). */
	if (settings.expect_fast &&
	    (a->counters.fast_retransmits < settings.expect_fast_low || a->counters.fast_retransmits > settings.expect_fast_high)) {
		printf("check: FAIL: A made %llu fast retransmits, not %llu..%llu\n",
		    (unsigned long long)a->counters.fast_retransmits, (unsigned long long)settings.expect_fast_low,
		    (unsigned long long)settings.expect_fast_high);
		return 1;
	}

	/* The segment size both ends settled on, and the jumbo packets A's ring held at most (--expect-mss). */
	usage_jumbo_check = 0;
	if (settings.expect_mss != 0) {
		packet_buf_pool_usage(&usage);
		printf("mss: a.send_mss=%u b.receive_mss=%u jumbo_used_max=%u jumbo_alloc_failures=%llu\n", a->send_mss,
		    receiver != NULL ? tcp_of(receiver)->receive_mss : 0U, usage.jumbo_used_max,
		    (unsigned long long)usage.jumbo_alloc_failures);
		if (a->send_mss != settings.expect_mss)
			usage_jumbo_check = 1;
		if (receiver != NULL && tcp_of(receiver)->receive_mss != settings.expect_mss)
			usage_jumbo_check = 1;
		if (settings.expect_mss > 1460U && usage.jumbo_used_max == 0)
			usage_jumbo_check = 1;
		if (usage.jumbo_used_max > 2U * 8U)
			usage_jumbo_check = 1;
	}
	if (usage_jumbo_check) {
		printf("check: FAIL: the segment size is not %u, or the jumbo packets were not used within their bounds\n",
		    settings.expect_mss);
		return 1;
	}

	/* A's timeouts, when the scenario bounds them. */
	if (settings.expect_timeouts && timeouts > settings.expect_timeouts_max) {
		printf("check: FAIL: A had %llu timeouts, more than %llu\n", (unsigned long long)timeouts,
		    (unsigned long long)settings.expect_timeouts_max);
		return 1;
	}

	/* The first sample's estimate, when the scenario says where it should lie. */
	if (settings.expect_first_srtt_high != 0 &&
	    (first_srtt_us < settings.expect_first_srtt_low || first_srtt_us > settings.expect_first_srtt_high)) {
		printf("check: FAIL: A's first sample gave srtt %u us, not %llu..%llu\n", first_srtt_us,
		    (unsigned long long)settings.expect_first_srtt_low, (unsigned long long)settings.expect_first_srtt_high);
		return 1;
	}

	/* Its variation likewise (RFC 6298 section 2.2: half the sample). */
	if (settings.expect_first_rttvar_high != 0 &&
	    (first_rttvar_us < settings.expect_first_rttvar_low || first_rttvar_us > settings.expect_first_rttvar_high)) {
		printf("check: FAIL: A's first sample gave rttvar %u us, not %llu..%llu\n", first_rttvar_us,
		    (unsigned long long)settings.expect_first_rttvar_low,
		    (unsigned long long)settings.expect_first_rttvar_high);
		return 1;
	}

	/* A's largest SRTT, when the scenario bounds it. */
	if (settings.expect_srtt_max != 0 && srtt_max_us > settings.expect_srtt_max) {
		printf("check: FAIL: A's srtt reached %u us, more than %llu\n", srtt_max_us,
		    (unsigned long long)settings.expect_srtt_max);
		return 1;
	}

	/* The longest stall against the timeouts that explain it. */
	printf("stall: longest_ms=%.1f resends_in_row_max=%u double_losses=%llu backoff_max=%u\n",
	    (double)longest_stall_ns / 1e6, resends_in_row_max, (unsigned long long)double_losses, backoff_max);
	allowed_ms = (double)((2ULL << backoff_max) - 1U) * (double)kern_ticks_to_ms(a->rto) + 20.0;
	if (settings.expect_stall_rto && (double)longest_stall_ns / 1e6 > allowed_ms) {
		printf("check: FAIL: the longest stall %.1f ms is more than the %.1f ms the timeouts explain\n",
		    (double)longest_stall_ns / 1e6, allowed_ms);
		return 1;
	}

	/* A's resends, when the scenario bounds them. */
	if (settings.expect_retransmits && a->counters.retransmit_segments > settings.expect_retransmits_max) {
		printf("check: FAIL: A resent %llu segments, more than %llu\n",
		    (unsigned long long)a->counters.retransmit_segments, (unsigned long long)settings.expect_retransmits_max);
		return 1;
	}

	/* No resend without a loss, when the scenario asks (each lost data frame is resent once). */
	if (settings.expect_retransmits_losses && a->counters.retransmit_segments > links[0].lost) {
		printf("check: FAIL: A resent %llu segments, more than the %llu frames the link lost\n",
		    (unsigned long long)a->counters.retransmit_segments, (unsigned long long)links[0].lost);
		return 1;
	}

	/* A's SRTT at the end, when the scenario says where it should lie. */
	if (settings.expect_srtt_high != 0) {
		printf("rtt: srtt_us=%u rttvar_us=%u rto_ms=%llu\n", a->srtt_us, a->rttvar_us,
		    (unsigned long long)kern_ticks_to_ms(a->rto));
		if (a->srtt_us < settings.expect_srtt_low || a->srtt_us > settings.expect_srtt_high) {
			printf("check: FAIL: A's srtt %u us outside %llu..%llu\n", a->srtt_us,
			    (unsigned long long)settings.expect_srtt_low, (unsigned long long)settings.expect_srtt_high);
			return 1;
		}
	}

	/* net.tcpstats as the kernel copies it out must agree with the sockets (of a connection still up). */
	if (transfer_error == 0 && closed_at_ns == 0) {
		error = check_tcpstats();
		if (error != 0)
			return 1;
	}

	/* A's congestion window, and the shares of two connections. */
	error = check_cc();
	if (error != 0)
		return 1;
	error = check_fair();
	if (error != 0)
		return 1;

	/* Reports whether the whole stream arrived as it was written. */
	if (!complete)
		return 1;

	/* The close in the order the scenario names. */
	error = close_in_order();
	if (error != 0)
		return 1;

	/* Succeeded: the reader has every byte. */
	return 0;
}

/*
 * Closes one end as its file's close does and reports how long the close
 * waited: the FIN, then the drain's wait while anything sent is
 * outstanding, on a connection still alive, for at most
 * TCP_CLOSE_DRAIN_MS, then the end is freed.  The model runs without a
 * thread, so the kernel's own drain returns at once and the wait is made
 * here, with the links and the timers running.
 */
static uint64_t
close_end(
	struct socket *socket)
{
	struct tcp_socket *tcp;
	uint64_t begin_ns;

	tcp = tcp_of(socket);
	begin_ns = now_ns;
	socket_close_endpoint(socket);
	while (tcp->send_count != 0 && tcp->state != TCP_CLOSED && socket->error == 0 &&
	    now_ns - begin_ns < 10000000000ULL)
		step();
	socket_release(socket);

	/* Reports the time the close waited. */
	return now_ns - begin_ns;
}

/*
 * Closes the connection after the transfer, the end --close-first names
 * first, B's listener left open, and checks that the second end's close
 * ends within a second: by the acknowledgement of its FIN, or by a reset
 * when its peer has already been freed.
 */
static int
close_in_order(
	void)
{
	struct socket *first;
	struct socket *second_end;
	uint64_t first_ns;
	uint64_t second_ns;
	int second_state;
	int second_error;

	/* A scenario that does not close. */
	if (settings.close_first == 0)
		return 0;

	/* The ends in their order; the writer and the reader stop. */
	first = receiver;
	second_end = sender;
	if (settings.close_first == 'a') {
		first = sender;
		second_end = receiver;
	}
	sender = NULL;
	receiver = NULL;

	/* The first end, then a second for its FIN to reach the other, then the second end. */
	first_ns = close_end(first);
	second_ns = now_ns + 1000000000ULL;
	while (now_ns < second_ns)
		step();
	second_state = (int)tcp_of(second_end)->state;
	second_error = second_end->error;
	second_ns = close_end(second_end);
	printf("close-order: first=%c first_ms=%.1f second_ms=%.1f second_state_before=%d second_error=%d\n",
	    settings.close_first, (double)first_ns / 1e6, (double)second_ns / 1e6, second_state, second_error);

	/* The second end's close is answered, not left to the drain's time. */
	if (second_ns > 1000000000ULL) {
		printf("check: FAIL: the second end's close waited %.1f ms, more than a second\n", (double)second_ns / 1e6);
		return 1;
	}

	/* Succeeded: both ends closed in time. */
	return 0;
}

/* Creates a TCP socket for the IPv4 family, as the internet layer does. */
static int
tcp_socket_create_inet(
	int type,
	int protocol,
	struct socket **result)
{
	int error;

	(void)type;

	/* A TCP socket. */
	error = tcp_socket_create(protocol, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes a segment IPv4 routed out of a host's device: queued for the wire
 * when the device has room, refused (and freed) when it does not.
 */
int
model_link_output(
	unsigned host,
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	size_t header;
	int acknowledgement_only;

	/* A second writer writes while this segment's sender waits for the device. */
	header = (size_t)(packet->data[12] >> 4) * 4U;
	if (settings.second_writer && host == 0U && !second_writer_active && receiver != NULL &&
	    packet->length > header) {
		second_writer_active = 1;
		run_writer();
		second_writer_active = 0;
	}

	/* The link loses A's first SYN (it goes to the device and no further). */
	if (host == 0U && settings.lose_first_syn && !first_syn_lost && (packet->data[13] & 0x02U) != 0) {
		first_syn_lost = 1;
		packet_buf_free(packet);
		return 0;
	}

	/* IPv4 refuses every frame of the connect whose neighbour never resolves. */
	if (host == 0U && settings.connect_unresolved) {
		unresolved_refusals++;
		packet_buf_free(packet);
		return EAGAIN;
	}

	/* IPv4 refuses a frame for a neighbour still being resolved, and the frame is lost. */
	if (host == 0U && settings.unresolved_ns != 0 && transfer_start_ns != 0 &&
	    now_ns >= transfer_start_ns + settings.unresolved_at_ns &&
	    now_ns < transfer_start_ns + settings.unresolved_at_ns + settings.unresolved_ns) {
		unresolved_refusals++;
		packet_buf_free(packet);
		return EAGAIN;
	}

	/* A's IPv4 has no route for a while (--no-route-at-ms): the segment fails for good. */
	if (host == 0U && settings.no_route_ns != 0 && transfer_start_ns != 0 &&
	    now_ns >= transfer_start_ns + settings.no_route_at_ns &&
	    now_ns < transfer_start_ns + settings.no_route_at_ns + settings.no_route_ns) {
		no_route_refusals++;
		packet_buf_free(packet);
		return ENETUNREACH;
	}

	/*
	 * B delays its pure acknowledgements (--ack-delay-ms): the first is
	 * held, and the next sent at once in its place (it says as much).
	 */
	acknowledgement_only = pure_ack(packet);
	if (host == 1U && settings.ack_delay_ns != 0 && acknowledgement_only) {
		if (held_ack != NULL) {
			packet_buf_free(held_ack);
			held_ack = NULL;
		} else {
			held_ack = packet;
			held_ack_source = source;
			held_ack_destination = destination;
			held_ack_until_ns = now_ns + settings.ack_delay_ns;
			return 0;
		}
	}

	/* The device's queue, or its refusal. */
	return link_enqueue(host, packet, source, destination);
}

/* Queues a frame on a host's device: refused (and freed) when the device is full. */
static int
link_enqueue(
	unsigned host,
	struct packet_buf *packet,
	uint32_t source,
	uint32_t destination)
{
	struct model_link *link;
	unsigned slot;

	link = &links[host];

	/* A full device refuses the frame, as a USB adapter's driver does. */
	if (link->queue_count >= settings.queue) {
		link->queue_full++;
		packet_buf_free(packet);
		return ENOBUFS;
	}

	/* The frame waits its turn; an idle wire takes it at once. */
	slot = (link->queue_first + link->queue_count) % MODEL_QUEUE_MAX;
	link->queue[slot] = packet;
	link->queue_source[slot] = source;
	link->queue_destination[slot] = destination;
	link->queue_count++;
	if (link->queue_count == 1U)
		link_start(link, now_ns);

	/* Succeeded: the device has the frame. */
	return 0;
}

/* Tells whether a segment is a pure acknowledgement: no payload, no flag but ACK. */
static int
pure_ack(
	const struct packet_buf *packet)
{
	size_t header;

	/* Payload. */
	header = (size_t)(packet->data[12] >> 4) * 4U;
	if (packet->length > header)
		return 0;

	/* Succeeded: only the ACK flag says it is one. */
	if (packet->data[13] == 0x10U)
		return 1;
	return 0;
}

/* Reads the command line into the settings. */
static void
parse_settings(
	int argc,
	char **argv)
{
	int index;

	/* The defaults: 64 MiB over a gigabit LAN with a USB adapter's queue. */
	settings.bytes = 64ULL * 1048576ULL;
	settings.mbps = 1000.0;
	settings.delay_ns = 100000U;
	settings.loss_ab = 0.0;
	settings.loss_ba = 0.0;
	settings.queue = 9U;
	settings.seed = 1U;
	settings.wrap_before = 0;
	settings.interval_ns = 1000000000ULL;
	settings.limit_ns = 600ULL * 1000000000ULL;
	settings.reader_kbps = 0.0;
	settings.peer_window = 0;
	settings.mtu = 0;
	settings.write_bytes = 1024;
	settings.expect_mss = 0;
	settings.overlap_ab = 0.0;
	settings.overlap_bytes = 100U;
	settings.blind_at_ns = 0;
	settings.blackhole_at_ns = 0;
	settings.connect_blackhole = 0;
	settings.connect_unresolved = 0;
	settings.second_writer = 0;
	settings.unresolved_at_ns = 0;
	settings.unresolved_ns = 0;
	settings.expect_in_order = 0;
	settings.expect_srtt_low = 0;
	settings.expect_srtt_high = 0;
	settings.lose_first_syn = 0;
	settings.ack_delay_ns = 0;
	settings.writer_gap_ns = 0;
	settings.delay_step_at_ns = 0;
	settings.delay_step_ns = 0;
	settings.reader_pause_at_ns = 0;
	settings.reader_pause_ns = 0;
	settings.no_route_at_ns = 0;
	settings.no_route_ns = 0;
	settings.expect_first_srtt_low = 0;
	settings.expect_first_srtt_high = 0;
	settings.expect_first_rttvar_low = 0;
	settings.expect_first_rttvar_high = 0;
	settings.expect_srtt_max = 0;
	settings.expect_retransmits_max = 0;
	settings.expect_retransmits = 0;
	settings.expect_retransmits_losses = 0;
	settings.expect_stall_rto = 0;
	settings.drop_ab_count = 0;
	settings.expect_fast_low = 0;
	settings.expect_fast_high = 0;
	settings.expect_fast = 0;
	settings.expect_timeouts_max = 0;
	settings.expect_timeouts = 0;
	settings.close_at_ns = 0;
	settings.maxunacktime = 0;
	settings.pool_held_free = -1;
	settings.expect_gap_low = 0;
	settings.expect_gap_high = 0;
	settings.bottleneck_mbps = 0.0;
	settings.bottleneck_queue_bytes = 0;
	settings.run_ns = 0;
	settings.receive_buffer = 0;
	settings.expect_receive = 0;
	settings.expect_receive_low = 0;
	settings.expect_receive_high = 0;
	settings.second_flow_at_ns = 0;
	settings.second_flow = 0;
	settings.fair_skip_ns = 5000000000ULL;
	settings.expect_slow_start = 0;
	settings.expect_ca_growth = 0.0;
	settings.expect_cwnd_limited = 0.0;
	settings.close_first = 0;
	settings.quiet = 0;

	/* Each option and its value. */
	for (index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--quiet") == 0) {
			settings.quiet = 1;
			continue;
		}
		if (strcmp(argv[index], "--nodelay") == 0) {
			settings.nodelay = 1;
			continue;
		}
		if (strcmp(argv[index], "--connect-blackhole") == 0) {
			settings.connect_blackhole = 1;
			continue;
		}
		if (strcmp(argv[index], "--connect-unresolved") == 0) {
			settings.connect_unresolved = 1;
			continue;
		}
		if (strcmp(argv[index], "--second-writer") == 0) {
			settings.second_writer = 1;
			continue;
		}
		if (strcmp(argv[index], "--expect-in-order") == 0) {
			settings.expect_in_order = 1;
			continue;
		}
		if (strcmp(argv[index], "--expect-stall-rto") == 0) {
			settings.expect_stall_rto = 1;
			continue;
		}
		if (strcmp(argv[index], "--expect-retransmits-losses") == 0) {
			settings.expect_retransmits_losses = 1;
			continue;
		}
		if (strcmp(argv[index], "--expect-ssthresh-held") == 0) {
			settings.expect_ssthresh_held = 1;
			continue;
		}
		if (strcmp(argv[index], "--expect-slow-start") == 0) {
			settings.expect_slow_start = 1;
			continue;
		}
		if (strcmp(argv[index], "--lose-first-syn") == 0) {
			settings.lose_first_syn = 1;
			continue;
		}
		if (index + 1 >= argc)
			break;
		if (strcmp(argv[index], "--bytes") == 0)
			settings.bytes = strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--mbps") == 0)
			settings.mbps = strtod(argv[index + 1], NULL);
		else if (strcmp(argv[index], "--delay-us") == 0)
			settings.delay_ns = (uint64_t)(strtod(argv[index + 1], NULL) * 1000.0);
		else if (strcmp(argv[index], "--loss-ab") == 0)
			settings.loss_ab = strtod(argv[index + 1], NULL);
		else if (strcmp(argv[index], "--loss-ba") == 0)
			settings.loss_ba = strtod(argv[index + 1], NULL);
		else if (strcmp(argv[index], "--queue") == 0)
			settings.queue = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--seed") == 0)
			settings.seed = strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--wrap-before") == 0)
			settings.wrap_before = strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--interval-ms") == 0)
			settings.interval_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--limit-s") == 0)
			settings.limit_ns = strtoull(argv[index + 1], NULL, 0) * 1000000000ULL;
		else if (strcmp(argv[index], "--reader-kbps") == 0)
			settings.reader_kbps = strtod(argv[index + 1], NULL);
		else if (strcmp(argv[index], "--peer-window") == 0)
			settings.peer_window = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--mtu") == 0)
			settings.mtu = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--write-bytes") == 0)
			settings.write_bytes = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--expect-mss") == 0)
			settings.expect_mss = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--overlap-ab") == 0)
			settings.overlap_ab = strtod(argv[index + 1], NULL);
		else if (strcmp(argv[index], "--overlap-bytes") == 0)
			settings.overlap_bytes = (unsigned)strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--blackhole-at-ms") == 0)
			settings.blackhole_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--blind-at-ms") == 0)
			settings.blind_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--unresolved-at-ms") == 0)
			settings.unresolved_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--unresolved-ms") == 0)
			settings.unresolved_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--expect-srtt-us") == 0)
			parse_range(argv[index + 1], &settings.expect_srtt_low, &settings.expect_srtt_high);
		else if (strcmp(argv[index], "--ack-delay-ms") == 0)
			settings.ack_delay_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--writer-gap-ms") == 0)
			settings.writer_gap_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--delay-step-at-ms") == 0)
			settings.delay_step_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--delay-step-us") == 0)
			settings.delay_step_ns = strtoull(argv[index + 1], NULL, 0) * 1000ULL;
		else if (strcmp(argv[index], "--reader-pause-at-ms") == 0)
			settings.reader_pause_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--reader-pause-ms") == 0)
			settings.reader_pause_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--no-route-at-ms") == 0)
			settings.no_route_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--no-route-ms") == 0)
			settings.no_route_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		else if (strcmp(argv[index], "--expect-first-srtt-us") == 0)
			parse_range(argv[index + 1], &settings.expect_first_srtt_low, &settings.expect_first_srtt_high);
		else if (strcmp(argv[index], "--expect-first-rttvar-us") == 0)
			parse_range(argv[index + 1], &settings.expect_first_rttvar_low, &settings.expect_first_rttvar_high);
		else if (strcmp(argv[index], "--drop-ab") == 0)
			parse_list(argv[index + 1], settings.drop_ab, &settings.drop_ab_count);
		else if (strcmp(argv[index], "--expect-fast-retransmits") == 0) {
			parse_range(argv[index + 1], &settings.expect_fast_low, &settings.expect_fast_high);
			settings.expect_fast = 1;
		} else if (strcmp(argv[index], "--expect-timeouts-max") == 0) {
			settings.expect_timeouts_max = strtoull(argv[index + 1], NULL, 0);
			settings.expect_timeouts = 1;
		} else if (strcmp(argv[index], "--close-first") == 0) {
			settings.close_first = argv[index + 1][0];
		} else if (strcmp(argv[index], "--close-at-ms") == 0) {
			settings.close_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
		} else if (strcmp(argv[index], "--drop-ab-run") == 0) {
			parse_range(argv[index + 1], &settings.drop_run_first, &settings.drop_run_count);
		} else if (strcmp(argv[index], "--expect-stall-max-ms") == 0) {
			settings.expect_stall_max_ms = strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--expect-segments-max") == 0) {
			settings.expect_segments_max = strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--expect-segments-min") == 0) {
			settings.expect_segments_min = strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--send-buffer") == 0) {
			settings.send_buffer = (unsigned)strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--pool-held") == 0) {
			settings.pool_held_free = (long)strtod(argv[index + 1], NULL);
		} else if (strcmp(argv[index], "--maxunacktime") == 0) {
			settings.maxunacktime = (unsigned)strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--expect-probe-gap-s") == 0) {
			parse_range(argv[index + 1], &settings.expect_gap_low, &settings.expect_gap_high);
		} else if (strcmp(argv[index], "--bottleneck-mbps") == 0) {
			settings.bottleneck_mbps = strtod(argv[index + 1], NULL);
		} else if (strcmp(argv[index], "--bottleneck-queue") == 0) {
			settings.bottleneck_queue_bytes = strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--run-s") == 0) {
			settings.run_ns = strtoull(argv[index + 1], NULL, 0) * 1000000000ULL;
		} else if (strcmp(argv[index], "--expect-receive-limit") == 0) {
			parse_range(argv[index + 1], &settings.expect_receive_low, &settings.expect_receive_high);
			settings.expect_receive = 1;
		} else if (strcmp(argv[index], "--receive-buffer") == 0) {
			settings.receive_buffer = (unsigned)strtoull(argv[index + 1], NULL, 0);
		} else if (strcmp(argv[index], "--second-flow-at-ms") == 0) {
			settings.second_flow_at_ns = strtoull(argv[index + 1], NULL, 0) * 1000000ULL;
			settings.second_flow = 1;
		} else if (strcmp(argv[index], "--fair-skip-s") == 0) {
			settings.fair_skip_ns = strtoull(argv[index + 1], NULL, 0) * 1000000000ULL;
		} else if (strcmp(argv[index], "--measure-from-s") == 0) {
			settings.measure_from_ns = strtoull(argv[index + 1], NULL, 0) * 1000000000ULL;
		} else if (strcmp(argv[index], "--cc") == 0) {
			settings.cc = argv[index + 1];
		} else if (strcmp(argv[index], "--second-flow-cc") == 0) {
			settings.second_cc = argv[index + 1];
		} else if (strcmp(argv[index], "--expect-ca-growth") == 0) {
			settings.expect_ca_growth = strtod(argv[index + 1], NULL);
		} else if (strcmp(argv[index], "--expect-cwnd-limited") == 0) {
			settings.expect_cwnd_limited = strtod(argv[index + 1], NULL);
		} else if (strcmp(argv[index], "--expect-srtt-max-us") == 0)
			settings.expect_srtt_max = strtoull(argv[index + 1], NULL, 0);
		else if (strcmp(argv[index], "--expect-retransmits-max") == 0) {
			settings.expect_retransmits_max = strtoull(argv[index + 1], NULL, 0);
			settings.expect_retransmits = 1;
		}
		index++;
	}

	/* A queue the model can hold. */
	if (settings.queue == 0 || settings.queue > MODEL_QUEUE_MAX)
		settings.queue = MODEL_QUEUE_MAX;

	/* A timed run sends until its time is up: the bytes are not what ends it. */
	if (settings.run_ns != 0) {
		settings.bytes = UINT64_MAX / 2U;
		settings.limit_ns = settings.run_ns;
	}
}

/* Keeps the least and the most time between two of A's resends. */
static void
note_gap(
	uint64_t gap)
{
	/* The first gap, or a smaller or larger one. */
	if (resend_gaps == 0 || gap < gap_min_ns)
		gap_min_ns = gap;
	if (gap > gap_max_ns)
		gap_max_ns = gap;
	resend_gaps++;
}

/* Reads a comma separated list of up to 16 numbers. */
static void
parse_list(
	const char *text,
	uint64_t *list,
	unsigned *count)
{
	char *end;

	/* Each number and the comma after it. */
	*count = 0;
	while (*text != '\0' && *count < 16U) {
		list[*count] = strtoull(text, &end, 0);
		(*count)++;
		if (*end != ',')
			break;
		text = end + 1;
	}
}

/*
 * Tells whether a frame leaving A's device is one --drop-ab names: A's data
 * frames are counted from 1 from the transfer's start.
 */
static int
dropped_on_purpose(
	struct model_link *link,
	const struct packet_buf *packet)
{
	size_t header;
	unsigned index;

	/* Only A's data frames after the connection count. */
	if (link != &links[0] || (settings.drop_ab_count == 0 && settings.drop_run_count == 0) || transfer_start_ns == 0)
		return 0;
	header = (size_t)(packet->data[12] >> 4) * 4U;
	if (packet->length <= header)
		return 0;
	data_frames_ab++;

	/* Within the burst. */
	if (settings.drop_run_count != 0 &&
	    now_ns - transfer_start_ns >= settings.drop_run_first * 1000000ULL &&
	    now_ns - transfer_start_ns < settings.drop_run_first * 1000000ULL + settings.drop_run_count * 1000ULL)
		return 1;

	/* One of the numbers named. */
	for (index = 0; index < settings.drop_ab_count; index++) {
		if (settings.drop_ab[index] == data_frames_ab)
			return 1;
	}

	/* Succeeded: not one of them. */
	return 0;
}

/* Reads LOW:HIGH. */
static void
parse_range(
	const char *text,
	uint64_t *low,
	uint64_t *high)
{
	char *end;

	/* The low end, then the high end after the colon. */
	*low = strtoull(text, &end, 0);
	*high = *low;
	if (*end == ':')
		*high = strtoull(end + 1, NULL, 0);
}

/*
 * Finds the tick to start the clock at so that A's first data byte lies
 * wrap_before bytes before the sequence numbers wrap (0: an ordinary start).
 *
 * tcp_connect() takes the initial sequence number as
 * (tick * 1103515245 + port) ^ (generation * 2654435761); A's port is the
 * first ephemeral one (49152) and its first connect is generation 1.  The
 * multiplier is odd, so it has an inverse modulo 2^32.
 */
static uint64_t
isn_tick_base(
	uint64_t wrap_before)
{
	uint32_t inverse;
	uint32_t wanted;
	uint32_t product;
	unsigned round;

	/* An ordinary start at a second after boot. */
	if (wrap_before == 0)
		return 1000U;

	/* The inverse of the multiplier, by Newton's iteration. */
	inverse = 1103515245U;
	for (round = 0; round < 5U; round++)
		inverse *= 2U - 1103515245U * inverse;

	/* The SYN takes one number before the first data byte. */
	wanted = (uint32_t)(0x100000000ULL - wrap_before) - 1U;
	product = (wanted ^ 2654435761U) - 49152U;
	return (uint64_t)(uint32_t)(product * inverse);
}

/* A uniform random number in [0, 1), from a 64-bit xorshift. */
static double
random_unit(
	void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (double)(random_state >> 11) / 9007199254740992.0;
}

/* The byte of the stream at an offset, so the reader can tell a byte out of place. */
static uint8_t
stream_byte(
	uint64_t offset)
{
	return (uint8_t)((offset * 2654435761ULL) >> 24);
}

/* The time a segment of this length takes on a wire of mbps. */
static uint64_t
frame_time_ns(
	size_t length,
	double mbps)
{
	size_t bytes;

	/* The frame on the wire, never shorter than Ethernet's minimum with its preamble and gap. */
	bytes = length + MODEL_FRAME_OVERHEAD;
	if (bytes < 84U)
		bytes = 84U;
	return (uint64_t)((double)bytes * 8.0 * 1000.0 / mbps);
}

/*
 * Takes a frame of A's off its wire at end_ns into the router
 * (--bottleneck-mbps): lost (*dropped) when its queue has no room for it
 * behind what it still holds, else queued, and the time it arrives at B is
 * when the router has sent it, with the one-way delay.
 */
static uint64_t
bottleneck_arrival(
	size_t length,
	uint64_t end_ns,
	int *dropped)
{
	uint64_t backlog_ns;
	double backlog_bytes;
	uint64_t start_ns;

	/* What the queue still holds when the frame comes: the bytes the router's link has yet to send. */
	*dropped = 0;
	backlog_ns = 0;
	if (bottleneck_free_ns > end_ns)
		backlog_ns = bottleneck_free_ns - end_ns;
	backlog_bytes = (double)backlog_ns * settings.bottleneck_mbps / 8000.0;

	/* A full queue loses it. */
	if (backlog_bytes + (double)(length + MODEL_FRAME_OVERHEAD) > (double)settings.bottleneck_queue_bytes) {
		*dropped = 1;
		bottleneck_drops++;
		return 0;
	}

	/* It goes after what the queue holds. */
	start_ns = end_ns;
	if (bottleneck_free_ns > start_ns)
		start_ns = bottleneck_free_ns;
	bottleneck_free_ns = start_ns + frame_time_ns(length, settings.bottleneck_mbps);

	/* Succeeded: it arrives the delay after the router sent it. */
	return bottleneck_free_ns + settings.delay_ns;
}

/* Puts the oldest queued frame on the wire. */
static void
link_start(
	struct model_link *link,
	uint64_t start_ns)
{
	struct packet_buf *packet;

	packet = link->queue[link->queue_first];
	link->transmit_end_ns = start_ns + frame_time_ns(packet->length, settings.mbps);
}

/*
 * Finishes what the link has done by now: frames off the wire (lost, or on
 * their way), and frames that have arrived (given to TCP).
 */
static void
link_advance(
	struct model_link *link)
{
	struct model_frame *frame;
	struct packet_buf *packet;
	uint64_t end_ns;
	uint64_t arrive_ns;
	int arrived;
	int purpose;
	int dropped;
	int error;

	/* Each frame whose transmission has ended leaves the device. */
	arrived = 0;
	while (link->queue_count != 0 && link->transmit_end_ns <= now_ns) {
		packet = link->queue[link->queue_first];
		end_ns = link->transmit_end_ns;
		link->frames++;

		/* The link loses it (by chance, or on purpose), or the router after A's wire (a full queue), when there is one. */
		purpose = dropped_on_purpose(link, packet);
		dropped = 0;
		arrive_ns = end_ns + settings.delay_ns;
		if (random_unit() < link->loss || purpose) {
			dropped = 1;
		} else if (link == &links[0] && settings.bottleneck_mbps > 0.0) {
			arrive_ns = bottleneck_arrival(packet->length, end_ns, &dropped);
		}

		/* What is lost goes no further; the rest is carried to the other host. */
		if (dropped) {
			link->lost++;
		} else {
			frame = malloc(sizeof(*frame));
			frame->next = NULL;
			frame->arrive_ns = arrive_ns;
			frame->source = link->queue_source[link->queue_first];
			frame->destination = link->queue_destination[link->queue_first];
			frame->length = packet->length;
			memcpy(frame->data, packet->data, packet->length);
			if (link == &links[1] && settings.peer_window != 0)
				rewrite_window(frame);
			if (link == &links[0] && settings.overlap_ab > 0.0 && random_unit() < settings.overlap_ab / 100.0)
				overlap_frame(frame);
			if (link->flight_tail != NULL)
				link->flight_tail->next = frame;
			else
				link->flight_head = frame;
			link->flight_tail = frame;
		}

		/*
		 * The device has copied it and gives the packet back; the next one
		 * follows on the wire, and the completion runs a worker pass.
		 */
		packet_buf_free(packet);
		worker_pass = 1;
		link->queue_first = (link->queue_first + 1U) % MODEL_QUEUE_MAX;
		link->queue_count--;
		link->transmit_end_ns = MODEL_NEVER;
		if (link->queue_count != 0)
			link_start(link, end_ns);
	}

	/* Each frame that has arrived goes to TCP, and the applications see what it did. */
	while (link->flight_head != NULL && link->flight_head->arrive_ns <= now_ns) {
		frame = link->flight_head;
		link->flight_head = frame->next;
		if (link->flight_head == NULL)
			link->flight_tail = NULL;
		error = model_deliver(frame->data, frame->length, frame->source, frame->destination);
		if (error != 0)
			link->receive_no_packet++;
		free(frame);
		run_applications();
		arrived = 1;
	}

	/*
	 * The network worker has taken what had arrived: the acknowledgements
	 * TCP delayed for the end of that batch go now (ws221-p009), as the
	 * kernel's worker sends them after its input.
	 */
	if (arrived)
		tcp_ack_flush();
}

/*
 * Puts the --peer-window window in a segment from B, with its checksum made
 * again: in bytes, so the field is in the scale A takes B's windows in
 * (none in a SYN).
 */
static void
rewrite_window(
	struct model_frame *frame)
{
	struct tcp_socket *a;
	uint16_t checksum;
	unsigned field;

	/* The field: the bytes, scaled as A reads them. */
	field = settings.peer_window;
	a = NULL;
	if (sender != NULL)
		a = tcp_of(sender);
	if (a != NULL && a->wscale_on != 0 && (frame->data[13] & 0x02U) == 0)
		field >>= a->peer_wscale;
	if (field > 0xffffU)
		field = 0xffffU;

	/* The window field (bytes 14 and 15) and the checksum (16 and 17) of the TCP header. */
	frame->data[14] = (uint8_t)(field >> 8);
	frame->data[15] = (uint8_t)(field & 0xffU);
	frame->data[16] = 0;
	frame->data[17] = 0;
	checksum = net_checksum_pseudo(frame->source, frame->destination, IPPROTO_TCP, frame->data, frame->length);
	frame->data[16] = (uint8_t)(checksum >> 8);
	frame->data[17] = (uint8_t)(checksum & 0xffU);
}

/*
 * Moves the start of a data segment from A --overlap-bytes back, the bytes
 * put in front being the stream's own, with its checksum made again.  A
 * segment too near the stream's start, or one with a SYN or no data, is
 * left as it is.
 */
static void
overlap_frame(
	struct model_frame *frame)
{
	uint8_t moved[2048];
	uint32_t sequence;
	uint32_t offset;
	uint16_t checksum;
	size_t header;
	size_t payload;
	size_t extra;
	size_t index;

	/* Only a data segment, far enough into the stream, with room to grow. */
	header = (size_t)(frame->data[12] >> 4) * 4U;
	if (header > frame->length || (frame->data[13] & 0x02U) != 0)
		return;
	payload = frame->length - header;
	extra = settings.overlap_bytes;
	if (payload == 0 || frame->length + extra > sizeof(frame->data))
		return;
	sequence = ((uint32_t)frame->data[4] << 24) | ((uint32_t)frame->data[5] << 16) |
	    ((uint32_t)frame->data[6] << 8) | (uint32_t)frame->data[7];
	offset = sequence - stream_sequence;
	if (offset < extra)
		return;

	/* The header with the earlier sequence number, the stream's bytes before the payload, then the payload. */
	memcpy(moved, frame->data, header);
	sequence -= (uint32_t)extra;
	moved[4] = (uint8_t)(sequence >> 24);
	moved[5] = (uint8_t)(sequence >> 16);
	moved[6] = (uint8_t)(sequence >> 8);
	moved[7] = (uint8_t)sequence;
	for (index = 0; index < extra; index++)
		moved[header + index] = stream_byte((uint64_t)(offset - extra) + index);
	memcpy(moved + header + extra, frame->data + header, payload);
	frame->length += extra;
	memcpy(frame->data, moved, frame->length);

	/* The checksum over the segment as it now is. */
	frame->data[16] = 0;
	frame->data[17] = 0;
	checksum = net_checksum_pseudo(frame->source, frame->destination, IPPROTO_TCP, frame->data, frame->length);
	frame->data[16] = (uint8_t)(checksum >> 8);
	frame->data[17] = (uint8_t)(checksum & 0xffU);
	overlapped++;
}

/*
 * Gives B the segments of someone who knows the connection's addresses and
 * ports but not its sequence numbers: a reset within the window B
 * advertised but not at its next sequence number, a reset far outside it,
 * and a SYN.  B answers the first and the SYN with an acknowledgement
 * (RFC 5961 sections 3.2 and 4) and ignores the second; none resets.
 */
static void
give_blind_segments(
	void)
{
	struct tcp_socket *b;

	/* The numbers B expects now, which a blind sender does not know exactly. */
	b = tcp_of(receiver);
	give_blind_segment(b->receive_next + 1000U, b->send_next, 0x04U);
	give_blind_segment(b->receive_next + 0x80000000U, b->send_next, 0x04U);
	give_blind_segment(b->receive_next - 5000U, 0, 0x02U);
	printf("tcp-model: blind segments given at receive_next %u, B state %d\n", b->receive_next, (int)b->state);
}

/* Gives B one segment from A's address and port with the numbers and flags given. */
static void
give_blind_segment(
	uint32_t sequence,
	uint32_t acknowledgement,
	uint8_t flags)
{
	uint8_t segment[20];
	uint16_t port;
	uint16_t checksum;

	/* The header: A's port to B's, the numbers, no options, the flags and a window. */
	memset(segment, 0, sizeof(segment));
	port = tcp_of(sender)->inet.local_port;
	segment[0] = (uint8_t)(port >> 8);
	segment[1] = (uint8_t)port;
	segment[2] = (uint8_t)(MODEL_PORT >> 8);
	segment[3] = (uint8_t)MODEL_PORT;
	segment[4] = (uint8_t)(sequence >> 24);
	segment[5] = (uint8_t)(sequence >> 16);
	segment[6] = (uint8_t)(sequence >> 8);
	segment[7] = (uint8_t)sequence;
	segment[8] = (uint8_t)(acknowledgement >> 24);
	segment[9] = (uint8_t)(acknowledgement >> 16);
	segment[10] = (uint8_t)(acknowledgement >> 8);
	segment[11] = (uint8_t)acknowledgement;
	segment[12] = 5U << 4;
	segment[13] = flags;
	if (acknowledgement != 0)
		segment[13] |= 0x10U;
	segment[14] = 0xffU;
	segment[15] = 0xffU;
	checksum = net_checksum_pseudo(MODEL_ADDRESS_A, MODEL_ADDRESS_B, IPPROTO_TCP, segment, sizeof(segment));
	segment[16] = (uint8_t)(checksum >> 8);
	segment[17] = (uint8_t)(checksum & 0xffU);

	/* B takes it as it takes any segment from the link. */
	(void)model_deliver(segment, sizeof(segment), MODEL_ADDRESS_A, MODEL_ADDRESS_B);
	run_applications();
}

/* Lets the writer and the reader act on what has changed. */
static void
run_applications(
	void)
{
	run_writer();
	run_reader();
	run_second_flow();
}

/*
 * Runs the second connection (--second-flow-at-ms): its connect at its
 * time, B's accept of it, then its writer, which writes the stream as fast
 * as the stack takes it, and its reader, which checks what it reads.
 */
static void
run_second_flow(
	void)
{
	static uint8_t chunk[65536];
	struct sockaddr_in address;
	uint8_t buffer[65536];
	uint64_t index;
	ssize_t written;
	ssize_t got;
	int option_value;
	int error;
	int sender_state;
	uint8_t stream_value;

	/* Does nothing unless a second connection is asked for. */
	if (!settings.second_flow)
		return;

	/* Does nothing before the transfer has started. */
	if (transfer_start_ns == 0)
		return;

	/* Does nothing before the second connection's time. */
	if (now_ns < transfer_start_ns + settings.second_flow_at_ns)
		return;

	/* A's connect, once. */
	if (second.sender == NULL) {
		/* Makes A's socket. */
		error = socket_create(AF_INET, SOCK_STREAM, 0, &second.sender);
		if (error != 0) {
			transfer_error = error;
			return;
		}

		/* B's address. */
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = model_ntohs(MODEL_PORT);
		address.sin_addr.s_addr = model_ntohl(MODEL_ADDRESS_B);

		/* Gives the socket its own congestion control, when the scenario names one. */
		if (settings.second_cc != NULL) {
			error = second.sender->ops->setsockopt(second.sender, IPPROTO_TCP, TCP_CONGESTION, settings.second_cc,
			    (socklen_t)strlen(settings.second_cc));
			if (error != 0) {
				transfer_error = error;
				return;
			}
		}

		/* Starts the connect, which a non-blocking socket leaves under way. */
		error = second.sender->ops->connect(second.sender, (struct sockaddr *)&address, sizeof(address),
		    SOCKET_IO_NONBLOCK);
		if (error != EINPROGRESS) {
			transfer_error = error;
			return;
		}
		second.connecting = 1;
		return;
	}

	/* B's accept, with its receive buffer when the scenario sets one. */
	if (second.receiver == NULL) {
		(void)listener->ops->accept(listener, &second.receiver, NULL, NULL, SOCKET_IO_NONBLOCK);

		/* Nothing to accept yet. */
		if (second.receiver == NULL)
			return;

		/* Sets the receive buffer the scenario names. */
		if (settings.receive_buffer != 0) {
			option_value = (int)settings.receive_buffer;
			(void)socket_setsockopt_common(second.receiver, SOL_SOCKET, SO_RCVBUF, &option_value,
			    sizeof(option_value));
		}
	}

	/* Up once A's end is established. */
	if (second.connecting) {
		/* Still in the handshake. */
		sender_state = tcp_of(second.sender)->state;
		if (sender_state != TCP_ESTABLISHED)
			return;
		second.connecting = 0;
		second.up_ns = now_ns;
	}

	/* The writer: each piece the stack takes now. */
	for (;;) {
		/* Fills one piece with the stream's next bytes. */
		for (index = 0; index < settings.write_bytes && index < sizeof(chunk); index++) {
			stream_value = stream_byte(second.written + index);
			chunk[index] = stream_value;
		}
		written = second.sender->ops->sendto(second.sender, chunk, (size_t)index, MSG_DONTWAIT, NULL, 0);

		/* Stops when the stack takes no more, or fails. */
		if (written <= 0) {
			if (written != -EAGAIN)
				transfer_error = (int)-written;
			break;
		}
		second.written += (uint64_t)written;
	}

	/* The reader: everything that has arrived, each byte checked against the stream. */
	for (;;) {
		got = second.receiver->ops->recvfrom(second.receiver, buffer, sizeof(buffer), MSG_DONTWAIT, NULL, NULL);

		/* Stops when nothing more has arrived, or on a failure. */
		if (got <= 0) {
			if (got < 0 && got != -EAGAIN)
				transfer_error = (int)-got;
			break;
		}

		/* Counts each byte that is not the stream's. */
		for (index = 0; index < (uint64_t)got; index++) {
			stream_value = stream_byte(second.received + index);
			if (buffer[index] != stream_value)
				second.corrupt++;
		}
		second.received += (uint64_t)got;
	}
}

/*
 * Writes as much of the stream as the stack takes now.
 *
 * A write the stack refuses although its ring and the peer's window have
 * room was refused for want of packets: the kernel's writer then sleeps
 * TCP_SEND_RETRY_MS, and so does this one.  Otherwise it waits for an
 * acknowledgement, which the next event brings.  A second writer
 * (--second-writer), called while a write is still inside the stack, writes
 * one piece after the bytes of that write.
 */
static void
run_writer(
	void)
{
	static uint8_t chunk[65536];
	struct tcp_socket *tcp;
	uint64_t length;
	uint64_t index;
	uint64_t base;
	uint32_t window;
	uint32_t in_flight;
	uint32_t segment;
	int buffer_room;
	ssize_t written;
	int error;

	/* A writer asleep, or done. */
	if (sender == NULL || now_ns < send_sleep_until_ns)
		return;

	/* A writer that stops at its time and shuts its side (--close-at-ms), once the FIN fits the ring. */
	if (settings.close_at_ns != 0 && transfer_start_ns != 0 && now_ns >= transfer_start_ns + settings.close_at_ns) {
		if (!writer_closed) {
			error = sender->ops->shutdown(sender, SHUT_WR);
			if (error == 0)
				writer_closed = 1;
		}

		/* Nothing more is written. */
		return;
	}

	/* Each piece the stack takes, after the bytes of any write still inside it. */
	while (send_left > writer_reserved) {
		length = send_left - writer_reserved;
		if (length > settings.write_bytes)
			length = settings.write_bytes;
		if (length > sizeof(chunk))
			length = sizeof(chunk);
		base = send_offset + writer_reserved;
		for (index = 0; index < length; index++)
			chunk[index] = stream_byte(base + index);
		writer_reserved += length;
		written = sender->ops->sendto(sender, chunk, (size_t)length, MSG_DONTWAIT, NULL, 0);
		writer_reserved -= length;
		if (written > 0) {
			send_offset += (uint64_t)written;
			send_left -= (uint64_t)written;
			if (second_writer_active)
				return;

			/* A writer of one piece at a time (--writer-gap-ms) waits before the next. */
			if (settings.writer_gap_ns != 0) {
				send_sleep_until_ns = now_ns + settings.writer_gap_ns;
				return;
			}

			/* Otherwise the next piece at once. */
			continue;
		}

		/* No route (--no-route-at-ms): the application waits 100 ms and writes the same bytes again. */
		if (written == -ENETUNREACH && settings.no_route_ns != 0) {
			send_sleep_until_ns = now_ns + 100000000ULL;
			return;
		}

		/* An error other than a full ring or window ends the transfer. */
		if (written != -EAGAIN) {
			transfer_error = (int)-written;
			return;
		}

		/*
		 * A refusal with room left is a shortage of packets: the kernel's
		 * writer sleeps.  The room is the peer's window (one segment when
		 * it is closed) and the congestion window, the smaller, and the
		 * send buffer for one segment more (ws221-p008).
		 */
		tcp = tcp_of(sender);
		window = tcp->peer_window;
		if (window == 0) {
			window = 536U;
			if (tcp->send_mss != 0)
				window = tcp->send_mss;
		}
		if (tcp->cwnd != 0 && tcp->cwnd < window)
			window = tcp->cwnd;
		in_flight = tcp->send_next - tcp->send_unacknowledged;
		segment = (uint32_t)length;
		if (tcp->send_mss != 0 && tcp->send_mss < length)
			segment = tcp->send_mss;
		buffer_room = tcp->send_bytes == 0 || tcp->send_bytes + segment <= sender->send_hiwat_bytes;
		if (tcp->send_count < TCP_SEND_QUEUE_MAX && buffer_room &&
		    (in_flight == 0 || in_flight + (uint32_t)length <= window))
			send_sleep_until_ns = now_ns + MODEL_SEND_RETRY_NS;
		return;
	}
}

/* Reads what has arrived, as fast as the reader's rate allows. */
static void
run_reader(
	void)
{
	uint8_t buffer[65536];
	ssize_t got;
	size_t want;
	ssize_t index;

	/* No reader before the connection is accepted. */
	if (receiver == NULL)
		return;

	/* The reader stops for a while (--reader-pause-at-ms). */
	if (settings.reader_pause_ns != 0 && transfer_start_ns != 0 &&
	    now_ns >= transfer_start_ns + settings.reader_pause_at_ns &&
	    now_ns < transfer_start_ns + settings.reader_pause_at_ns + settings.reader_pause_ns)
		return;

	/* The reader's allowance grows with the time since it was last given. */
	if (settings.reader_kbps > 0.0) {
		read_budget += (double)(now_ns - read_budget_ns) * settings.reader_kbps * 1000.0 / 8.0 / 1e9;
		if (read_budget > 65536.0)
			read_budget = 65536.0;
		read_budget_ns = now_ns;
	}

	/* Each read until nothing is left (or the allowance is spent). */
	for (;;) {
		want = sizeof(buffer);
		if (settings.reader_kbps > 0.0) {
			if (read_budget < 1.0)
				return;
			if ((double)want > read_budget)
				want = (size_t)read_budget;
		}
		got = receiver->ops->recvfrom(receiver, buffer, want, MSG_DONTWAIT, NULL, NULL);
		if (got <= 0) {
			if (got < 0 && got != -EAGAIN)
				transfer_error = (int)-got;
			return;
		}

		/* Each byte checked against the stream. */
		for (index = 0; index < got; index++) {
			if (buffer[index] != stream_byte(received + (uint64_t)index))
				corrupt++;
		}
		received += (uint64_t)got;
		if (settings.reader_kbps > 0.0)
			read_budget -= (double)got;
	}
}

/*
 * Makes the connection: B listens, A connects without waiting, and the
 * link carries the handshake until B accepts.
 */
static int
connect_hosts(
	void)
{
	struct sockaddr_in address;
	uint64_t limit_ns;
	int option_value;
	int sender_state;
	int error;

	/* B's listener on its port, every address. */
	error = socket_create(AF_INET, SOCK_STREAM, 0, &listener);
	if (error != 0)
		return error;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = model_ntohs(MODEL_PORT);
	error = listener->ops->bind(listener, (struct sockaddr *)&address, sizeof(address));
	if (error != 0)
		return error;
	error = listener->ops->listen(listener, 4);
	if (error != 0)
		return error;

	/* A's connect to B, which completes later. */
	error = socket_create(AF_INET, SOCK_STREAM, 0, &sender);
	if (error != 0)
		return error;
	address.sin_addr.s_addr = model_ntohl(MODEL_ADDRESS_B);
	error = sender->ops->connect(sender, (struct sockaddr *)&address, sizeof(address), SOCKET_IO_NONBLOCK);
	if (error != EINPROGRESS)
		return error;

	/* The handshake, for at most ten seconds (400 when the SYN is lost on purpose, until the connect gives up). */
	limit_ns = 10000000000ULL;
	if (settings.connect_blackhole)
		limit_ns = 400000000000ULL;
	if (settings.connect_unresolved)
		limit_ns = 400000000000ULL;
	while (now_ns < limit_ns) {
		step();
		if (tcp_of(sender)->state == TCP_CLOSED)
			return ETIMEDOUT;
		if (receiver == NULL)
			(void)listener->ops->accept(listener, &receiver, NULL, NULL, SOCKET_IO_NONBLOCK);
		if (receiver != NULL && tcp_of(sender)->state == TCP_ESTABLISHED)
			break;
	}

	/* The handshake did not finish. */
	sender_state = tcp_of(sender)->state;
	if (receiver == NULL || sender_state != TCP_ESTABLISHED)
		return ETIMEDOUT;

	/* B's receive buffer, when the scenario sets one. */
	if (settings.receive_buffer != 0) {
		option_value = (int)settings.receive_buffer;
		error = socket_setsockopt_common(receiver, SOL_SOCKET, SO_RCVBUF, &option_value, sizeof(option_value));
		if (error != 0)
			return error;
	}

	/* Succeeded: both ends are up. */
	return 0;
}

/*
 * Moves the clock to the next thing that happens (a frame leaving a
 * device, a frame arriving, the kernel's tick, the writer waking) and does
 * it.
 */
static void
step(
	void)
{
	struct packet_buf *packet;
	struct tcp_socket *a;
	uint64_t next_ns;
	uint64_t tick_ns;
	unsigned direction;

	/* The next tick of the kernel's clock (KERN_CLOCK_HZ a second), and the earliest event before it. */
	tick_ns = (now_ns / (1000000000ULL / KERN_CLOCK_HZ) + 1U) * (1000000000ULL / KERN_CLOCK_HZ);
	next_ns = tick_ns;
	if (held_ack != NULL && held_ack_until_ns > now_ns && held_ack_until_ns < next_ns)
		next_ns = held_ack_until_ns;
	for (direction = 0; direction < 2U; direction++) {
		if (links[direction].queue_count != 0 && links[direction].transmit_end_ns < next_ns)
			next_ns = links[direction].transmit_end_ns;
		if (links[direction].flight_head != NULL && links[direction].flight_head->arrive_ns < next_ns)
			next_ns = links[direction].flight_head->arrive_ns;
	}
	if (send_sleep_until_ns > now_ns && send_sleep_until_ns < next_ns)
		next_ns = send_sleep_until_ns;

	/* The clock moves on; the shim's clock is in microseconds. */
	if (next_ns > now_ns)
		now_ns = next_ns;
	model_clock_set(now_ns / 1000U);

	/* The one-way delay changes at its time (--delay-step-at-ms). */
	if (settings.delay_step_ns != 0 && !delay_stepped && transfer_start_ns != 0 &&
	    now_ns >= transfer_start_ns + settings.delay_step_at_ns) {
		settings.delay_ns = settings.delay_step_ns;
		delay_stepped = 1;
	}

	/* B's held acknowledgement goes when its delay is up. */
	if (held_ack != NULL && now_ns >= held_ack_until_ns) {
		packet = held_ack;
		held_ack = NULL;
		(void)link_enqueue(1U, packet, held_ack_source, held_ack_destination);
	}

	/* What the links did by now. */
	for (direction = 0; direction < 2U; direction++)
		link_advance(&links[direction]);

	/* A tick, or a device's transmit completion: the network worker's pass, then the applications. */
	if (now_ns == tick_ns || worker_pass) {
		worker_pass = 0;
		tcp_timer_run();
		run_applications();
	}

	/* A writer waking from its sleep. */
	if (send_sleep_until_ns != 0 && now_ns >= send_sleep_until_ns) {
		send_sleep_until_ns = 0;
		run_applications();
	}

	/* A's first round-trip sample as the estimate stood right after it, and its largest SRTT (ws220-p004). */
	if (sender == NULL)
		return;
	a = tcp_of(sender);
	if (!first_sample_seen && a->rtt_samples != 0) {
		first_sample_seen = 1;
		first_srtt_us = a->srtt_us;
		first_rttvar_us = a->rttvar_us;
	}

	/* When A's connection closed. */
	if (closed_at_ns == 0 && transfer_start_ns != 0 && a->state == TCP_CLOSED)
		closed_at_ns = now_ns;

	/* The time between two of A's resends both in the persist state (the probes of a closed window). */
	if (a->counters.retransmit_segments != resends_seen) {
		if (a->persist != 0 && previous_in_persist)
			note_gap(now_ns - resend_last_ns);
		previous_in_persist = a->persist != 0;
		resends_seen = a->counters.retransmit_segments;
		resend_last_ns = now_ns;
	}

	/* The largest SRTT so far. */
	if (a->rtt_samples != 0 && a->srtt_us > srtt_max_us)
		srtt_max_us = a->srtt_us;

	/*
	 * A timeout that really expired doubles the backoff; one while the
	 * backoff of an earlier one holds (no sample between) repeats it, and
	 * the threshold should stay as that one left it.
	 */
	if (a->rto_backoff > backoff_last) {
		timeouts += a->rto_backoff - backoff_last;

		/* A timeout repeated before a sample, and whether it moved the threshold. */
		if (backoff_last != 0) {
			repeated_timeouts++;
			if (a->ssthresh != ssthresh_after_timeout)
				ssthresh_moved++;
		}
		ssthresh_after_timeout = a->ssthresh;
	}
	backoff_last = a->rto_backoff;

	/* The resends of one segment in a row, the times a resend was lost again, and the backoff. */
	if (a->rto_backoff > backoff_max)
		backoff_max = a->rto_backoff;
	if (a->retransmit_count > resends_in_row_max)
		resends_in_row_max = a->retransmit_count;
	if (a->retransmit_count >= 2U && resends_in_row_last < 2U)
		double_losses++;
	resends_in_row_last = a->retransmit_count;

	/* What A's congestion window did. */
	watch_cc();
}

/* Prints one interval: the rate, and what both ends and the links show. */
static void
report(
	uint64_t elapsed_ns,
	uint64_t interval_bytes)
{
	struct tcp_socket *a;
	struct tcp_socket *b;
	struct packet_buf_pool_usage usage;
	double rate;

	a = tcp_of(sender);
	b = receiver != NULL ? tcp_of(receiver) : NULL;
	packet_buf_pool_usage(&usage);
	rate = (double)interval_bytes / 1048576.0 / ((double)settings.interval_ns / 1e9);
	printf("t=%.3f mbs=%.2f received=%llu a.state=%d a.in_flight=%u a.ring=%u a.peer_window=%u a.backoff=%u a.srtt_us=%u a.rto_ms=%llu "
	    "a.retransmits=%llu a.immediate=%llu a.dupacks_in=%llu a.acks_outside=%llu a.refused=%llu a.send_fail=%llu "
	    "b.window=%u b.reorder=%u b.ooo_in=%llu b.ooo_kept=%llu b.old_in=%llu b.refused=%llu b.send_fail=%llu "
	    "link.ab.lost=%llu link.ab.full=%llu link.ba.lost=%llu link.ba.full=%llu rx_nobuf=%llu pool=%u/%u max=%u fail=%llu "
	    "jumbo=%u/%u max=%u fail=%llu a.mss=%u a.jumbo=%u b.rmss=%u a.cwnd=%u a.ssthresh=%u a.sent_flight=%u "
	    "second.received=%llu bottleneck_drops=%llu\n",
	    (double)elapsed_ns / 1e9, rate, (unsigned long long)received, (int)a->state, a->send_next - a->send_unacknowledged,
	    a->send_count, a->peer_window, a->retransmit_count, a->srtt_us, (unsigned long long)kern_ticks_to_ms(a->rto),
	    (unsigned long long)a->counters.retransmit_segments, (unsigned long long)a->counters.immediate_resends,
	    (unsigned long long)a->counters.duplicate_acks_in, (unsigned long long)a->counters.acks_outside,
	    (unsigned long long)a->counters.send_refused, (unsigned long long)a->counters.segment_send_failures,
	    b != NULL ? b->advertised_window : 0U, b != NULL ? b->reorder_count : 0U,
	    (unsigned long long)(b != NULL ? b->counters.out_of_order_in : 0U),
	    (unsigned long long)(b != NULL ? b->counters.out_of_order_kept : 0U),
	    (unsigned long long)(b != NULL ? b->counters.old_segments_in : 0U),
	    (unsigned long long)(b != NULL ? b->counters.receive_refused : 0U),
	    (unsigned long long)(b != NULL ? b->counters.segment_send_failures : 0U),
	    (unsigned long long)links[0].lost, (unsigned long long)links[0].queue_full,
	    (unsigned long long)links[1].lost, (unsigned long long)links[1].queue_full,
	    (unsigned long long)(links[0].receive_no_packet + links[1].receive_no_packet),
	    usage.used, usage.total, usage.used_max, (unsigned long long)usage.alloc_failures,
	    usage.jumbo_used, usage.jumbo_total, usage.jumbo_used_max, (unsigned long long)usage.jumbo_alloc_failures,
	    a->send_mss, a->send_jumbo, b != NULL ? b->receive_mss : 0U, a->cwnd, a->ssthresh, sent_flight(a),
	    (unsigned long long)second.received, (unsigned long long)bottleneck_drops);
}

/*
 * Reads net.tcpstats through tcp_stats_copy(), as sysctl does: the length
 * first, then the value, and checks it against the sockets: three of them
 * (the listener and the two ends), and the bytes A queued and B took in
 * order as the counts say.  Prints one line and reports 0 when they agree.
 */
static int
check_tcpstats(
	void)
{
	static uint8_t buffer[sizeof(struct tcp_stats_header) + 8U * sizeof(struct tcp_stats_entry)];
	unsigned sockets;
	const struct tcp_stats_header *header;
	const struct tcp_stats_entry *entry;
	uint64_t sent;
	uint64_t taken;
	const struct tcp_socket *sender_tcp;
	size_t needed;
	unsigned index;
	int error;

	/* A's connection, which the entries are checked against. */
	sender_tcp = tcp_of(sender);

	/* The length, then the value. */
	error = tcp_stats_copy(NULL, 0, &needed, 1);
	if (error != 0 || needed > sizeof(buffer)) {
		printf("tcpstats: FAIL: length %llu (%d)\n", (unsigned long long)needed, error);
		return 1;
	}
	error = tcp_stats_copy(buffer, sizeof(buffer), &needed, 1);
	if (error != 0) {
		printf("tcpstats: FAIL: copy (%d)\n", error);
		return 1;
	}

	/* The layout and the sockets' counts: three, and the second connection's two ends when it is open. */
	sockets = 3U;
	if (second.sender != NULL)
		sockets++;
	if (second.receiver != NULL)
		sockets++;
	header = (const struct tcp_stats_header *)(const void *)buffer;
	sent = 0;
	taken = 0;
	for (index = 0; index < header->count; index++) {
		entry = (const struct tcp_stats_entry *)(const void *)(buffer + header->struct_size + index * header->element_size);
		if (entry->local_port != MODEL_PORT && entry->state == TCP_ESTABLISHED)
			sent += entry->data_bytes_out;
		if (entry->local_port == MODEL_PORT && entry->state == TCP_ESTABLISHED)
			taken += entry->data_bytes_in;
	}
	if (header->version != TCP_STATS_VERSION ||
	    header->count != sockets ||
	    needed != sizeof(*header) + sockets * sizeof(*entry) ||
	    sent != send_offset + second.written ||
	    taken != received + second.received) {
		printf("tcpstats: FAIL: version %u count %u sent %llu (wrote %llu) taken %llu (read %llu)\n", header->version,
		    header->count, (unsigned long long)sent, (unsigned long long)(send_offset + second.written),
		    (unsigned long long)taken, (unsigned long long)(received + second.received));
		return 1;
	}

	/* The superuser is given the sequence numbers (ws220-p002), and every reader the round-trip estimate (p004). */
	for (index = 0; index < header->count; index++) {
		entry = (const struct tcp_stats_entry *)(const void *)(buffer + header->struct_size + index * header->element_size);
		if ((entry->features & TCP_STATS_HAVE_SEQUENCE) == 0) {
			printf("tcpstats: FAIL: the superuser's entry %u has no sequence numbers\n", index);
			return 1;
		}

		/* Every entry has the congestion control's fields, A's first connection the one the scenario named. */
		if ((entry->features & TCP_STATS_HAVE_CC) == 0) {
			printf("tcpstats: FAIL: entry %u has no congestion control\n", index);
			return 1;
		}
		if (entry->local_port == sender_tcp->inet.local_port &&
		    entry->state == TCP_ESTABLISHED &&
		    entry->cc != sender_tcp->cc) {
			printf("tcpstats: FAIL: A's congestion control %u (socket %u)\n", entry->cc, sender_tcp->cc);
			return 1;
		}

		/* Every entry has the estimate's fields (ws220-p004). */
		if ((entry->features & TCP_STATS_HAVE_RTT) == 0) {
			printf("tcpstats: FAIL: entry %u has no round-trip estimate\n", index);
			return 1;
		}

		/* A's entry (of its first connection) gives A's estimate and a timeout. */
		if (entry->local_port == sender_tcp->inet.local_port &&
		    entry->state == TCP_ESTABLISHED &&
		    (entry->srtt_us != sender_tcp->srtt_us ||
		     entry->rto_ms == 0)) {
			printf("tcpstats: FAIL: A's srtt_us %u (socket %u), rto_ms %u\n", entry->srtt_us, sender_tcp->srtt_us,
			    entry->rto_ms);
			return 1;
		}
	}

	/* Any other reader is given every socket without them. */
	error = tcp_stats_copy(buffer, sizeof(buffer), &needed, 0);
	if (error != 0 || header->count != sockets) {
		printf("tcpstats: FAIL: copy for a user (%d)\n", error);
		return 1;
	}
	for (index = 0; index < header->count; index++) {
		entry = (const struct tcp_stats_entry *)(const void *)(buffer + header->struct_size + index * header->element_size);
		if ((entry->features & TCP_STATS_HAVE_SEQUENCE) != 0 || entry->send_unacknowledged != 0 ||
		    entry->send_next != 0 || entry->receive_next != 0 || entry->local_port == 0) {
			printf("tcpstats: FAIL: a user's entry %u shows sequence numbers or lacks its port\n", index);
			return 1;
		}
	}

	/* Succeeded: the value agrees with the sockets. */
	printf("tcpstats: ok sockets=%u length=%llu packets_used_max=%u user_hides_sequence=1\n", header->count,
	    (unsigned long long)needed, header->packets_used_max);
	return 0;
}

/* The bytes A has handed to its device and not had acknowledged (the unsent entries at the ring's end left out). */
static uint32_t
sent_flight(
	const struct tcp_socket *tcp)
{
	uint32_t sent_end;
	unsigned slot;

	/* The end of what was sent: the first unsent entry's start, or the end of the ring. */
	sent_end = tcp->send_next;
	if (tcp->send_unsent != 0) {
		slot = (tcp->send_first + tcp->send_count - tcp->send_unsent) % TCP_SEND_QUEUE_MAX;
		sent_end = tcp->send_queue[slot].sequence;
	}

	/* Succeeded: from the oldest unacknowledged byte. */
	return sent_end - tcp->send_unacknowledged;
}

/*
 * Watches A's congestion window after each step: the most A has sent and
 * not had acknowledged; whether the window is what limits A now (what was
 * sent within a segment of it, and it below the peer's window), with the
 * time since the last step counted so; the doublings of a slow start the
 * window limits; and the spans of congestion avoidance.
 */
static void
watch_cc(
	void)
{
	struct tcp_socket *a;
	uint64_t elapsed_ns;
	uint64_t rtt_ns;
	uint32_t flight;
	uint32_t window;
	uint32_t mss;
	double rtts;
	int limited;
	int open;

	/* Only A's connection while it sends. */
	if (sender == NULL)
		return;
	if (transfer_start_ns == 0)
		return;
	a = tcp_of(sender);
	if (a->cwnd == 0)
		return;
	if (a->state != TCP_ESTABLISHED)
		return;

	/* The time since the last step. */
	elapsed_ns = 0;
	if (cc.last_ns != 0)
		elapsed_ns = now_ns - cc.last_ns;
	cc.last_ns = now_ns;

	/* The most sent and not acknowledged. */
	flight = sent_flight(a);
	if (flight > cc.sent_flight_max)
		cc.sent_flight_max = flight;

	/* Whether the window limits A now. */
	mss = a->send_mss;
	if (mss == 0)
		mss = 1460U;
	window = a->peer_window;
	if (window == 0)
		window = mss;
	limited = 0;
	if (a->cwnd < window) {
		if ((uint64_t)flight + mss >= a->cwnd)
			limited = 1;
	}

	/* Counts the step's time, and as limited when it was. */
	cc.measured_ns += elapsed_ns;
	if (limited)
		cc.limited_ns += elapsed_ns;

	/* The round trip the growth is counted in: A's estimate, or the link's when it has none. */
	rtt_ns = (uint64_t)a->srtt_us * 1000U;
	if (rtt_ns == 0)
		rtt_ns = 2U * settings.delay_ns + 1000000U;

	/*
	 * A slow start the window limits doubles it each round trip: the time
	 * since the last doubling (or since the window began to limit A) is
	 * kept at its largest, so a window that stops growing shows.
	 */
	if (a->cwnd >= a->ssthresh || !limited) {
		cc.double_from = a->cwnd;
		cc.double_since_ns = now_ns;
	} else {
		/* The first step of a slow start the window limits. */
		if (cc.double_from == 0) {
			cc.double_from = a->cwnd;
			cc.double_since_ns = now_ns;
		}

		/* Keeps the longest time since a doubling. */
		rtts = (double)(now_ns - cc.double_since_ns) / (double)rtt_ns;
		if (rtts > cc.double_rtts_max)
			cc.double_rtts_max = rtts;

		/* A doubling begins the next count. */
		if ((uint64_t)a->cwnd >= 2ULL * cc.double_from) {
			cc.doublings++;
			cc.double_from = a->cwnd;
			cc.double_since_ns = now_ns;
		}
	}

	/* Congestion avoidance, outside a recovery, after a loss set the threshold. */
	open = 0;
	if (a->ssthresh != UINT32_MAX &&
	    a->cwnd >= a->ssthresh &&
	    a->recovery == 0)
		open = 1;
	watch_cc_span(a, open, limited, elapsed_ns);
}

/*
 * Follows a span of congestion avoidance: it opens with the window it
 * starts at, counts the time the window limited A, and when it closes (a
 * loss, or the end) its growth in segments a round trip is kept at its
 * least, for a span of eight round trips or more the window limited for
 * 80 % of its time.
 */
static void
watch_cc_span(
	const struct tcp_socket *tcp,
	int open,
	int limited,
	uint64_t elapsed_ns)
{
	uint64_t duration_ns;
	uint64_t rtt_ns;
	uint32_t mss;
	double growth;
	int long_enough;

	/* A loss in the span (a new threshold) closes it as the end of the span does. */
	if (cc.span_open && open) {
		if (tcp->ssthresh != cc.span_ssthresh)
			open = 0;
	}

	/* Still open: the time the window limited A, and the window it has reached. */
	if (cc.span_open && open) {
		/* Counts the step's time when the window limited A. */
		if (limited)
			cc.span_limited_ns += elapsed_ns;
		cc.span_cwnd_last = tcp->cwnd;
		return;
	}

	/* A span that closes is counted when it is long enough and the window limited A in it. */
	if (cc.span_open) {
		cc.span_open = 0;
		duration_ns = now_ns - cc.span_start_ns;

		/* The round trip, A's estimate or the link's when it has none. */
		rtt_ns = (uint64_t)tcp->srtt_us * 1000U;
		if (rtt_ns == 0)
			rtt_ns = 2U * settings.delay_ns + 1000000U;
		mss = tcp->send_mss;
		if (mss == 0)
			mss = 1460U;

		/* A span of eight round trips or more, the window limiting A for 80 % of it. */
		long_enough = 0;
		if (duration_ns >= 8U * rtt_ns) {
			if ((double)cc.span_limited_ns >= 0.8 * (double)duration_ns)
				long_enough = 1;
		}
		if (long_enough) {
			growth = ((double)cc.span_cwnd_last - (double)cc.span_cwnd) / (double)mss /
			    ((double)duration_ns / (double)rtt_ns);

			/* Keeps the least growth. */
			if (cc.spans == 0 || growth < cc.span_growth_min)
				cc.span_growth_min = growth;
			cc.spans++;
		}
	}

	/* A new span from where the window stands. */
	if (open &&
	    tcp->ssthresh != UINT32_MAX &&
	    tcp->cwnd >= tcp->ssthresh &&
	    tcp->recovery == 0) {
		cc.span_open = 1;
		cc.span_start_ns = now_ns;
		cc.span_cwnd = tcp->cwnd;
		cc.span_cwnd_last = tcp->cwnd;
		cc.span_ssthresh = tcp->ssthresh;
		cc.span_limited_ns = 0;
	}
}

/*
 * Prints the "cc:" line and checks A's congestion window: always within
 * twice the most sent and not acknowledged and two segments, or the
 * initial window (a window that grows while it is not used is not), and
 * what the scenario asks of its share as the limit, its slow start and its
 * congestion avoidance.  Reports 0 when every check passes.
 */
static int
check_cc(
	void)
{
	struct tcp_socket *a;
	uint64_t bound;
	uint64_t initial;
	uint32_t mss;
	double share;
	int failed;

	/* The span still open at the end. */
	a = tcp_of(sender);
	watch_cc_span(a, 0, 0, 0);

	/* The line. */
	share = 0.0;
	if (cc.measured_ns != 0)
		share = (double)cc.limited_ns / (double)cc.measured_ns;
	printf("cc: cwnd=%u ssthresh=%u sent_flight_max=%u cwnd_limited_share=%.3f slow_start_doublings=%u "
	    "slow_start_worst_rtts=%.2f ca_spans=%u ca_growth_min=%.2f bottleneck_drops=%llu\n", a->cwnd, a->ssthresh,
	    cc.sent_flight_max, share, cc.doublings, cc.double_rtts_max, cc.spans, cc.span_growth_min,
	    (unsigned long long)bottleneck_drops);

	/*
	 * The window within twice the most it held and two segments, or the
	 * initial window (RFC 6928, as tcp.c sets it) when it never grew.
	 */
	failed = 0;
	mss = a->send_mss;
	if (mss == 0)
		mss = 1460U;
	initial = 10ULL * mss;
	if (initial > 14600U)
		initial = 14600U;
	if (initial < 2ULL * mss)
		initial = 2ULL * mss;
	bound = 2ULL * cc.sent_flight_max + 2ULL * mss;
	if (bound < initial)
		bound = initial;
	if (a->cwnd > bound) {
		printf("check: FAIL: A's congestion window %u is more than twice the most it sent (%u) and two segments\n",
		    a->cwnd, cc.sent_flight_max);
		failed = 1;
	}

	/* The share of the time the window limited A. */
	if (settings.expect_cwnd_limited > 0.0 && share < settings.expect_cwnd_limited) {
		printf("check: FAIL: the congestion window limited A %.3f of the time, less than %.3f\n", share,
		    settings.expect_cwnd_limited);
		failed = 1;
	}

	/* A timeout repeated before a sample holds the threshold (RFC 5681 section 3.1). */
	printf("timeouts: repeated=%llu ssthresh_moved=%llu\n", (unsigned long long)repeated_timeouts,
	    (unsigned long long)ssthresh_moved);
	if (settings.expect_ssthresh_held &&
	    (repeated_timeouts == 0 ||
	     ssthresh_moved != 0)) {
		printf("check: FAIL: %llu repeated timeouts, %llu of them moved the threshold\n",
		    (unsigned long long)repeated_timeouts, (unsigned long long)ssthresh_moved);
		failed = 1;
	}

	/*
	 * A slow start that doubled, never taking more than three round trips
	 * for it (one, and the round trip a filling queue adds before A's
	 * estimate follows; a window that stops growing takes many).
	 */
	if (settings.expect_slow_start &&
	    (cc.doublings == 0 ||
	     cc.double_rtts_max > 3.0)) {
		printf("check: FAIL: the slow start doubled %u times, the slowest in %.2f round trips (more than 3)\n",
		    cc.doublings, cc.double_rtts_max);
		failed = 1;
	}

	/* Congestion avoidance that grew. */
	if (settings.expect_ca_growth > 0.0 &&
	    (cc.spans == 0 ||
	     cc.span_growth_min < settings.expect_ca_growth)) {
		printf("check: FAIL: %u spans of congestion avoidance, the slowest growing %.2f segments a round trip "
		    "(less than %.2f)\n", cc.spans, cc.span_growth_min, settings.expect_ca_growth);
		failed = 1;
	}

	/* Reports whether every check passed. */
	if (failed)
		return 1;
	return 0;
}

/*
 * Prints the "fair:" line of two connections (--second-flow-at-ms): their
 * shares of what both readers took in the intervals from --fair-skip-s
 * after the second was up, Jain's index of their mean rates there and the
 * mean of each interval's index.  Checks what the scenario asks of the
 * index and of the second's share; reports 0 when they pass.
 */
static int
check_fair(
	void)
{
	uint64_t first_sum;
	uint64_t second_sum;
	double first_rate;
	double second_rate;
	double jain;
	double jain_sum;
	double total;
	double first_share;
	double second_share;
	double jain_mean;
	double interval_total;
	double interval_squares;
	unsigned counted;
	unsigned index;

	/* Only with a second connection. */
	if (!settings.second_flow)
		return 0;

	/* The intervals counted. */
	first_sum = 0;
	second_sum = 0;
	jain_sum = 0.0;
	counted = 0;
	for (index = 0; index < interval_count; index++) {
		if (!interval_second_up[index])
			continue;
		first_sum += interval_first[index];
		second_sum += interval_second[index];
		first_rate = (double)interval_first[index];
		second_rate = (double)interval_second[index];

		/* Jain's index of this interval's two rates. */
		if (first_rate + second_rate > 0.0) {
			interval_total = first_rate + second_rate;
			interval_squares = first_rate * first_rate + second_rate * second_rate;
			jain_sum += interval_total * interval_total / (2.0 * interval_squares);
		}
		counted++;
	}

	/* The shares and the indexes. */
	total = (double)first_sum + (double)second_sum;
	first_share = 0.0;
	second_share = 0.0;
	jain = 0.0;
	if (total > 0.0) {
		first_share = (double)first_sum / total;
		second_share = (double)second_sum / total;
		jain = total * total / (2.0 * ((double)first_sum * (double)first_sum + (double)second_sum * (double)second_sum));
	}
	jain_mean = 0.0;
	if (counted != 0)
		jain_mean = jain_sum / counted;
	printf("fair: intervals=%u first_share=%.3f second_share=%.3f jain=%.3f jain_mean=%.3f second_corrupt=%llu\n",
	    counted, first_share, second_share, jain, jain_mean, (unsigned long long)second.corrupt);

	/* Nothing to judge from, or a second stream that did not arrive as written. */
	if (counted == 0 || second.corrupt != 0) {
		printf("check: FAIL: no interval with both connections up, or the second's stream is corrupt\n");
		return 1;
	}

	/* Succeeded: the line is out. */
	return 0;
}

/* The TCP state of a socket (the socket is the first member of its endpoint). */
static struct tcp_socket *
tcp_of(
	struct socket *socket)
{
	return (struct tcp_socket *)socket;
}
