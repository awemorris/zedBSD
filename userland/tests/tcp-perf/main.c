/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A TCP throughput probe: connects to a peer that sinks or sources a
 * stream on the host and moves bytes one
 * way for a number of seconds.
 *
 *   tcp-perf [--cc NAME] send|recv ADDRESS PORT SECONDS [CHUNK]
 *   tcp-perf [--cc NAME] serve ADDRESS PORT SECONDS [CHUNK]
 *
 * The first byte on the connection tells the peer the direction: 'S' when
 * this program sends (the peer reads to the end), 'R' when it receives
 * (the peer writes until this program closes).  ADDRESS is an IPv4
 * address in dotted form or an IPv6 address, a link-local one followed by
 * %INTERFACE; CHUNK is the bytes of one read or write (default 131072).  Every second a line "t=SECONDS bytes=N mbps=RATE" gives the
 * second's rate; the last line is "TCP-PERF DIRECTION seconds=S bytes=N
 * mbps=RATE" over the whole transfer.  --cc NAME sets the connection's
 * congestion control (TCP_CONGESTION, "reno" or "cubic"; on serve the
 * listener's, which the accepted connection takes) before it connects.
 * It exits 0, 1 when the transfer failed, or 2 for a bad command line.
 * The source also builds on Linux.
 *
 * serve is the other end, so that this machine's accepted connections are
 * measured too: it listens on ADDRESS (:: or 0.0.0.0 for any) and PORT,
 * takes one connection from a tcp-perf send or recv, and receives until
 * that end closes or sends until it closes (at most SECONDS and ten more);
 * its last line names it "serve-recv" or "serve-send".
 *
 * The time is kept by an interval timer that raises SIGALRM every second,
 * not by reading the clock around each read or write: on a system whose
 * clock is read through a system call that would add two calls to each
 * one the transfer makes.  A read or write the signal interrupts is
 * simply made again.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* The default bytes of one read or write. */
#define PERF_CHUNK_DEFAULT 131072UL

/* The largest chunk the command line may ask for. */
#define PERF_CHUNK_MAX 4194304UL

/* The longest transfer the command line may ask for, in seconds. */
#define PERF_SECONDS_MAX 3600UL

/* What the command line asks for. */
#define PERF_MODE_RECV	0	/* connect and receive */
#define PERF_MODE_SEND	1	/* connect and send */
#define PERF_MODE_SERVE	2	/* listen, and move bytes the way the connection asks */

/* The seconds a serving end goes on past its own, so that the connecting end ends the transfer. */
#define PERF_SERVE_GRACE 10UL

/*
 * The seconds the interval timer has counted since the transfer started.
 *
 * The SIGALRM handler adds one; the main thread reads it.
 */
static volatile sig_atomic_t perf_ticks;

/* The congestion control --cc names (NULL: the system's), set once by main before any socket. */
static const char *perf_cc;

/*
 * The rate of each second and of the whole transfer.
 *
 * Only the main thread touches it.
 */
struct perf_progress {
	double start;
	double second_start;
	unsigned long long total;
	unsigned long long second_bytes;
	unsigned second;
};

static int perf_parse(int argc, char **argv, int *mode, unsigned long *seconds, unsigned long *chunk);
static int perf_address(const char *address_text, uint16_t port, struct sockaddr_storage *address, socklen_t *length);
static int perf_connect(const char *address_text, const char *port_text, int *descriptor);
static int perf_accept(const char *address_text, const char *port_text, int *descriptor);
static int perf_open(int mode, const char *address_text, const char *port_text, int *descriptor, int *sending);
static int perf_set_cc(int descriptor);
static double perf_now(void);
static void perf_tick(int signal_number);
static int perf_start_timer(void);
static void perf_count(struct perf_progress *progress, size_t bytes);
static int perf_send(int descriptor, unsigned char *buffer, unsigned long chunk, unsigned long seconds, int until_closed, struct perf_progress *progress);
static int perf_receive(int descriptor, unsigned char *buffer, unsigned long chunk, unsigned long seconds, struct perf_progress *progress);

/*
 * Moves bytes the way the command line asks and prints the rates.
 */
int
main(
	int argc,
	char **argv)
{
	struct perf_progress progress;
	unsigned char *buffer;
	unsigned long seconds;
	unsigned long chunk;
	unsigned long index;
	double elapsed;
	const char *name;
	int descriptor;
	int sending;
	int mode;
	int error;
	int compare;

	/* The congestion control, when --cc comes first. */
	compare = 1;
	if (argc >= 3)
		compare = strcmp(argv[1], "--cc");
	if (compare == 0) {
		perf_cc = argv[2];
		argc -= 2;
		argv += 2;
	}

	/* The mode, the duration and the chunk. */
	error = perf_parse(argc, argv, &mode, &seconds, &chunk);
	if (error != 0)
		return 2;

	/* The buffer of one read or write, filled with a changing byte. */
	buffer = malloc(chunk);
	if (buffer == NULL) {
		fprintf(stderr, "tcp-perf: no memory for a chunk of %lu bytes\n", chunk);
		return 1;
	}

	/* The bytes of the chunk differ from one to the next. */
	for (index = 0; index < chunk; index++)
		buffer[index] = (unsigned char)((index * 131UL + 7UL) & 0xffUL);

	/* The connection, and the way the bytes go on it. */
	descriptor = -1;
	sending = 0;
	error = perf_open(mode, argv[2], argv[3], &descriptor, &sending);
	if (error != 0) {
		free(buffer);
		return 1;
	}

	/* The names of the ends; a serving end goes on until the other one ends the transfer. */
	name = sending ? "send" : "recv";
	if (mode == PERF_MODE_SERVE) {
		name = sending ? "serve-send" : "serve-recv";
		seconds += PERF_SERVE_GRACE;
	}

	/* The transfer, timed from its first byte. */
	memset(&progress, 0, sizeof(progress));
	progress.start = perf_now();
	progress.second_start = progress.start;
	error = perf_start_timer();
	if (error != 0) {
		fprintf(stderr, "tcp-perf: no interval timer: %s\n", strerror(error));
		close(descriptor);
		free(buffer);
		return 1;
	}

	/* Moves the bytes in the direction asked. */
	if (sending)
		error = perf_send(descriptor, buffer, chunk, seconds, mode == PERF_MODE_SERVE, &progress);
	else
		error = perf_receive(descriptor, buffer, chunk, seconds, &progress);
	elapsed = perf_now() - progress.start;
	close(descriptor);
	free(buffer);
	if (error != 0) {
		fprintf(stderr, "tcp-perf: %s failed after %llu bytes: %s\n", name, progress.total, strerror(error));
		return 1;
	}

	/* Succeeded: the rate over the whole transfer. */
	if (elapsed <= 0.0)
		elapsed = 1e-9;
	printf("TCP-PERF %s seconds=%.3f bytes=%llu mbps=%.1f\n", name, elapsed, progress.total,
	    (double)progress.total * 8.0 / elapsed / 1e6);
	return 0;
}

/*
 * Opens the connection the mode asks for: a connecting end tells the
 * peer its direction in the first byte; a serving end reads it from the
 * end that connected, and sends when that end receives.
 */
static int
perf_open(
	int mode,
	const char *address_text,
	const char *port_text,
	int *descriptor,
	int *sending)
{
	char direction;
	ssize_t moved;
	int error;

	/* A serving end: the connection it takes, and the direction that end asks. */
	if (mode == PERF_MODE_SERVE) {
		error = perf_accept(address_text, port_text, descriptor);
		if (error != 0)
			return error;
		moved = read(*descriptor, &direction, 1);
		if (moved != 1 || (direction != 'S' && direction != 'R')) {
			fprintf(stderr, "tcp-perf: no direction byte from the connecting end\n");
			close(*descriptor);
			return EPROTO;
		}
		*sending = 0;
		if (direction == 'R')
			*sending = 1;
		return 0;
	}

	/* A connecting end. */
	error = perf_connect(address_text, port_text, descriptor);
	if (error != 0)
		return error;

	/* The direction byte, which the peer reads first. */
	direction = 'R';
	*sending = 0;
	if (mode == PERF_MODE_SEND) {
		direction = 'S';
		*sending = 1;
	}
	moved = write(*descriptor, &direction, 1);
	if (moved != 1) {
		fprintf(stderr, "tcp-perf: the direction byte was not sent\n");
		close(*descriptor);
		return EIO;
	}

	/* Succeeded: connected, the direction told. */
	return 0;
}

/* Reads the direction, the duration and the chunk from the command line. */
static int
perf_parse(
	int argc,
	char **argv,
	int *mode,
	unsigned long *seconds,
	unsigned long *chunk)
{
	char *end;
	int compare;
	int compare_recv;
	int compare_serve;

	/* Refuses a command line without the mode, the address, the port and the duration. */
	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: tcp-perf [--cc NAME] send|recv|serve ADDRESS PORT SECONDS [CHUNK]\n");
		return EINVAL;
	}

	/* The mode. */
	compare = strcmp(argv[1], "send");
	compare_recv = strcmp(argv[1], "recv");
	compare_serve = strcmp(argv[1], "serve");
	if (compare == 0) {
		*mode = PERF_MODE_SEND;
	} else if (compare_recv == 0) {
		*mode = PERF_MODE_RECV;
	} else if (compare_serve == 0) {
		*mode = PERF_MODE_SERVE;
	} else {
		fprintf(stderr, "tcp-perf: the mode is send, recv or serve, not %s\n", argv[1]);
		return EINVAL;
	}

	/* The duration in seconds. */
	errno = 0;
	*seconds = strtoul(argv[4], &end, 10);
	if (errno != 0 || *end != '\0' || *seconds == 0 || *seconds > PERF_SECONDS_MAX) {
		fprintf(stderr, "tcp-perf: bad duration %s\n", argv[4]);
		return EINVAL;
	}

	/* The chunk, the default when none is given. */
	*chunk = PERF_CHUNK_DEFAULT;
	if (argc == 6) {
		errno = 0;
		*chunk = strtoul(argv[5], &end, 0);
		if (errno != 0 || *end != '\0' || *chunk == 0 || *chunk > PERF_CHUNK_MAX) {
			fprintf(stderr, "tcp-perf: bad chunk %s\n", argv[5]);
			return EINVAL;
		}
	}

	/* Succeeded: every value is in range. */
	return 0;
}

/*
 * Reads ADDRESS into a socket address with the port: an IPv4 one, or an
 * IPv6 one whose %INTERFACE suffix, when there is one, names the scope.
 */
static int
perf_address(
	const char *address_text,
	uint16_t port,
	struct sockaddr_storage *address,
	socklen_t *length)
{
	struct sockaddr_in6 *address6;
	struct sockaddr_in *address4;
	char text[INET6_ADDRSTRLEN + IF_NAMESIZE + 1];
	const char *percent;
	size_t text_length;
	unsigned scope;
	int parsed;

	/* An IPv4 address in dotted form. */
	memset(address, 0, sizeof(*address));
	address4 = (struct sockaddr_in *)address;
	parsed = inet_pton(AF_INET, address_text, &address4->sin_addr);
	if (parsed == 1) {
		address4->sin_family = AF_INET;
		address4->sin_port = htons(port);
		*length = (socklen_t)sizeof(*address4);
		return 0;
	}

	/* Otherwise an IPv6 one, its scope cut off first. */
	text_length = strlen(address_text);
	if (text_length >= sizeof(text))
		return EINVAL;
	memcpy(text, address_text, text_length + 1U);
	percent = strchr(address_text, '%');
	scope = 0;
	if (percent != NULL) {
		text[percent - address_text] = '\0';
		scope = if_nametoindex(percent + 1);
		if (scope == 0U)
			return EINVAL;
	}

	/* The address itself. */
	address6 = (struct sockaddr_in6 *)address;
	parsed = inet_pton(AF_INET6, text, &address6->sin6_addr);
	if (parsed != 1)
		return EINVAL;

	/* Succeeded: an IPv6 address with its port and scope. */
	address6->sin6_family = AF_INET6;
	address6->sin6_port = htons(port);
	address6->sin6_scope_id = scope;
	*length = (socklen_t)sizeof(*address6);
	return 0;
}

/* Connects a blocking stream socket to the address and port given. */
static int
perf_connect(
	const char *address_text,
	const char *port_text,
	int *descriptor)
{
	struct sockaddr_storage address;
	unsigned long port;
	socklen_t length;
	char *end;
	int error;

	/* The port, which must be a number of 16 bits. */
	errno = 0;
	port = strtoul(port_text, &end, 10);
	if (errno != 0 || *end != '\0' || port == 0 || port > 65535UL) {
		fprintf(stderr, "tcp-perf: bad port %s\n", port_text);
		return EINVAL;
	}

	/* The peer's address. */
	error = perf_address(address_text, (uint16_t)port, &address, &length);
	if (error != 0) {
		fprintf(stderr, "tcp-perf: bad address %s\n", address_text);
		return error;
	}

	/* The socket, of the address's family. */
	*descriptor = socket(address.ss_family, SOCK_STREAM, 0);
	if (*descriptor < 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: socket: %s\n", strerror(error));
		return error;
	}

	/* Its congestion control, when one is named. */
	error = perf_set_cc(*descriptor);
	if (error != 0) {
		close(*descriptor);
		return error;
	}

	/* The connection, waited for. */
	error = connect(*descriptor, (struct sockaddr *)&address, length);
	if (error != 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: connect %s:%s: %s\n", address_text, port_text, strerror(error));
		close(*descriptor);
		return error;
	}

	/* Succeeded: the socket is connected. */
	return 0;
}

/*
 * Listens on the address and port given and takes one connection; a
 * written-to connection that closed raises no SIGPIPE.
 */
static int
perf_accept(
	const char *address_text,
	const char *port_text,
	int *descriptor)
{
	struct sockaddr_storage address;
	unsigned long port;
	socklen_t length;
	char *end;
	int listener;
	int reuse;
	int error;

	/* The port, which must be a number of 16 bits. */
	errno = 0;
	port = strtoul(port_text, &end, 10);
	if (errno != 0 || *end != '\0' || port == 0 || port > 65535UL) {
		fprintf(stderr, "tcp-perf: bad port %s\n", port_text);
		return EINVAL;
	}

	/* The address listened on. */
	error = perf_address(address_text, (uint16_t)port, &address, &length);
	if (error != 0) {
		fprintf(stderr, "tcp-perf: bad address %s\n", address_text);
		return error;
	}

	/* A peer that closes first ends a send with EPIPE, not the signal. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The listening socket. */
	listener = socket(address.ss_family, SOCK_STREAM, 0);
	if (listener < 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: socket: %s\n", strerror(error));
		return error;
	}

	/* Its congestion control, which the accepted connection takes. */
	error = perf_set_cc(listener);
	if (error != 0) {
		close(listener);
		return error;
	}

	/* Bound to the address, after an earlier run's connection on the port. */
	reuse = 1;
	(void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	error = bind(listener, (struct sockaddr *)&address, length);
	if (error != 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: bind %s:%s: %s\n", address_text, port_text, strerror(error));
		close(listener);
		return error;
	}

	/* Listening, said for the one that starts the connecting end. */
	error = listen(listener, 1);
	if (error != 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: listen: %s\n", strerror(error));
		close(listener);
		return error;
	}
	printf("ready\n");
	fflush(stdout);

	/* The one connection. */
	*descriptor = accept(listener, NULL, NULL);
	if (*descriptor < 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: accept: %s\n", strerror(error));
		close(listener);
		return error;
	}

	/* Succeeded: the connection taken, the listener closed. */
	close(listener);
	return 0;
}

/* Reads the monotonic clock in seconds. */
static double
perf_now(void)
{
	struct timespec now;

	/* The clock; a failure leaves the zeroed value, which only skews the rate. */
	memset(&now, 0, sizeof(now));
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the time in seconds. */
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Counts one more second of the transfer. */
static void
perf_tick(
	int signal_number)
{
	(void)signal_number;
	perf_ticks++;
}

/*
 * Starts the interval timer that counts the transfer's seconds, without
 * SA_RESTART so that a blocked read or write returns to look at them.
 */
static int
perf_start_timer(void)
{
	struct sigaction action;
	struct itimerval interval;
	int error;

	/* The handler of SIGALRM. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = perf_tick;
	sigemptyset(&action.sa_mask);
	error = sigaction(SIGALRM, &action, NULL);
	if (error != 0)
		return errno;

	/* A signal every second from now. */
	memset(&interval, 0, sizeof(interval));
	interval.it_interval.tv_sec = 1;
	interval.it_value.tv_sec = 1;
	error = setitimer(ITIMER_REAL, &interval, NULL);
	if (error != 0)
		return errno;

	/* Succeeded: the seconds are counted. */
	return 0;
}

/*
 * Counts bytes moved and prints the rate of each second that has ended.
 */
static void
perf_count(
	struct perf_progress *progress,
	size_t bytes)
{
	double span;
	double now;

	/* The bytes count toward the total and the running second. */
	progress->total += (unsigned long long)bytes;
	progress->second_bytes += (unsigned long long)bytes;

	/* A second still running prints nothing. */
	if ((unsigned)perf_ticks <= progress->second)
		return;

	/* The second that ended, its rate over its own length. */
	now = perf_now();
	span = now - progress->second_start;
	if (span <= 0.0)
		span = 1e-9;
	progress->second++;
	printf("t=%u bytes=%llu mbps=%.1f\n", progress->second, progress->second_bytes,
	    (double)progress->second_bytes * 8.0 / span / 1e6);
	fflush(stdout);
	progress->second_bytes = 0;
	progress->second_start = now;
}

/*
 * Writes chunks for the duration, then shuts the sending side down and
 * reads until the peer closes, so the peer has taken every byte.  With
 * until_closed the peer ends the transfer: its close ends the writes
 * without an error.
 */
static int
perf_send(
	int descriptor,
	unsigned char *buffer,
	unsigned long chunk,
	unsigned long seconds,
	int until_closed,
	struct perf_progress *progress)
{
	ssize_t moved;
	int error;

	/* Each write until the duration has passed. */
	for (;;) {
		if ((unsigned long)perf_ticks >= seconds)
			break;

		/* The write, which blocks until the kernel takes some of it. */
		moved = write(descriptor, buffer, chunk);
		if (moved < 0) {
			error = errno;
			if (error == EINTR)
				continue;
			if (until_closed && (error == EPIPE || error == ECONNRESET))
				return 0;
			return error;
		}

		/* The bytes count toward the second's rate. */
		perf_count(progress, (size_t)moved);
	}

	/* The end of the stream for the peer. */
	error = shutdown(descriptor, SHUT_WR);
	if (error != 0)
		return errno;

	/* Whatever the peer sends, until it closes. */
	for (;;) {
		moved = read(descriptor, buffer, chunk);
		if (moved == 0)
			break;
		if (moved < 0) {
			error = errno;
			if (error == EINTR)
				continue;
			return error;
		}
	}

	/* Succeeded: the peer closed after taking the stream. */
	return 0;
}

/*
 * Reads chunks for the duration or until the peer closes.
 */
static int
perf_receive(
	int descriptor,
	unsigned char *buffer,
	unsigned long chunk,
	unsigned long seconds,
	struct perf_progress *progress)
{
	ssize_t moved;
	int error;

	/* Each read until the duration has passed or the stream ends. */
	for (;;) {
		if ((unsigned long)perf_ticks >= seconds)
			break;

		/* The read, which blocks until some bytes arrive. */
		moved = read(descriptor, buffer, chunk);
		if (moved == 0)
			break;
		if (moved < 0) {
			error = errno;
			if (error == EINTR)
				continue;
			return error;
		}

		/* The bytes count toward the second's rate. */
		perf_count(progress, (size_t)moved);
	}

	/* Succeeded: the duration passed or the peer ended the stream. */
	return 0;
}

/* Sets a socket's congestion control to the one --cc named; reports 0, or the error. */
static int
perf_set_cc(
	int descriptor)
{
	int error;

	/* None named: the system's. */
	if (perf_cc == NULL)
		return 0;

	/* The name, without its NUL. */
	error = setsockopt(descriptor, IPPROTO_TCP, TCP_CONGESTION, perf_cc, (socklen_t)strlen(perf_cc));
	if (error != 0) {
		error = errno;
		fprintf(stderr, "tcp-perf: TCP_CONGESTION %s: %s\n", perf_cc, strerror(error));
		return error;
	}

	/* Succeeded: the socket uses it. */
	return 0;
}
