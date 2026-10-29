/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libkeiland's network (<keiland/keiland_socket.h>) over the
 * loopback: addresses read and written, names resolved, TCP over IPv4 and
 * IPv6 that blocks and that does not, a connection refused, accept's flags,
 * a wait for many sockets, its time running out, wakers rung from other
 * threads, the end of a stream, a send after the peer closed (no SIGPIPE),
 * UDP, and the IPv6-only default.  It prints PASS, FAIL or NOT-DONE for each
 * check and a SUMMARY line, and exits 0 only when no check failed.  It is
 * built against libkeiland, and with -fsanitize=thread for the wakers'
 * races (socket-test.sh).
 */

#include <keiland/keiland_socket.h>
#include <keiland/keiland_thread.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* The pairs of connected sockets one wait watches, and the one written to. */
#define TEST_PAIRS		16U
#define TEST_PAIR_WRITTEN	11U

/* The threads that ring one waker, and how often each rings. */
#define TEST_RINGERS		4U
#define TEST_RINGS		10000U

/* A short wait that runs out, and how late any wait may end (ms). */
#define TEST_WAIT_MS		50
#define TEST_LATE_MS		5000

/* What the checks counted. */
struct test_counts {
	unsigned passed;
	unsigned failed;
	unsigned not_done;
};

/* A connected pair over the loopback: the client's end and the server's. */
struct test_pair {
	struct kl_socket *client;
	struct kl_socket *server;
};

/*
 * A thread that rings a waker after a while, or many times; with a counter,
 * it adds to the counter before each ring (what a ring tells the waiter).
 */
struct test_ringer {
	struct kl_socket_waker *waker;
	unsigned delay_ms;
	unsigned rings;
	atomic_uint *counter;
};

/* The results of the checks. */
static struct test_counts test_counts;

static void test_check(int passed, const char *name);
static void test_not_done(const char *name);
static int64_t test_now_ms(void);
static int test_listen(unsigned family, unsigned flags, struct kl_socket **listener, struct kl_address *address);
static int test_pair_open(unsigned family, struct test_pair *pair);
static void test_pair_close(struct test_pair *pair);
static void test_ring(void *argument);
static void test_addresses(void);
static void test_resolve(void);
static void test_stream(unsigned family, const char *name);
static void test_nonblocking(unsigned family);
static void test_refused(void);
static void test_accept_flags(void);
static void test_many(void);
static void test_wakers(void);
static void test_ends(void);
static void test_datagrams(unsigned family);
static void test_ipv6_only(void);
static void test_poll_limits(void);

/*
 * Runs every check and reports.
 */
int
main(void)
{
	/* The checks, one group at a time. */
	test_addresses();
	test_resolve();
	test_stream(KL_ADDRESS_IPV4, "stream-ipv4");
	test_stream(KL_ADDRESS_IPV6, "stream-ipv6");
	test_nonblocking(KL_ADDRESS_IPV4);
	test_nonblocking(KL_ADDRESS_IPV6);
	test_refused();
	test_accept_flags();
	test_many();
	test_wakers();
	test_ends();
	test_datagrams(KL_ADDRESS_IPV4);
	test_datagrams(KL_ADDRESS_IPV6);
	test_ipv6_only();
	test_poll_limits();

	/* The summary. */
	printf("SUMMARY pass=%u fail=%u not-done=%u\n", test_counts.passed, test_counts.failed, test_counts.not_done);

	/* Failed when any check did. */
	if (test_counts.failed != 0U)
		return 1;

	/* Succeeded: no check failed. */
	return 0;
}

/* Prints a check's result and counts it. */
static void
test_check(
	int passed,
	const char *name)
{
	/* PASS or FAIL with the check's name. */
	if (passed) {
		printf("PASS %s\n", name);
		test_counts.passed++;
	} else {
		printf("FAIL %s\n", name);
		test_counts.failed++;
	}
}

/* Prints a check the host cannot make. */
static void
test_not_done(
	const char *name)
{
	/* NOT-DONE with the check's name. */
	printf("NOT-DONE %s\n", name);
	test_counts.not_done++;
}

/* Reads the monotonic clock in milliseconds. */
static int64_t
test_now_ms(void)
{
	struct timespec now;

	/* The clock that only goes forward. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Milliseconds. */
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Opens a stream socket listening on the loopback at a port the system picks, and gives its address. */
static int
test_listen(
	unsigned family,
	unsigned flags,
	struct kl_socket **listener,
	struct kl_address *address)
{
	struct kl_address loopback;
	int error;

	/* The socket, bound to the loopback and listening. */
	error = kl_socket_open(family, KL_SOCKET_STREAM, flags, listener);
	if (error != 0)
		return error;
	kl_address_loopback(family, 0U, &loopback);
	error = kl_socket_bind(*listener, &loopback);
	if (error == 0)
		error = kl_socket_listen(*listener, 0U);
	if (error == 0)
		error = kl_socket_local_address(*listener, address);
	if (error != 0) {
		kl_socket_close(*listener);
		*listener = NULL;
		return error;
	}

	/* Succeeded: it listens at *address. */
	return 0;
}

/* Opens a connected pair over the loopback (sockets that block). */
static int
test_pair_open(
	unsigned family,
	struct test_pair *pair)
{
	struct kl_socket *listener;
	struct kl_address address;
	int error;

	/* Nothing open yet. */
	pair->client = NULL;
	pair->server = NULL;

	/* A listener, a client connected to it, and the server's end taken. */
	error = test_listen(family, 0U, &listener, &address);
	if (error != 0)
		return error;
	error = kl_socket_open(family, KL_SOCKET_STREAM, 0U, &pair->client);
	if (error == 0)
		error = kl_socket_connect(pair->client, &address);
	if (error == 0)
		error = kl_socket_accept(listener, 0U, &pair->server, NULL);
	kl_socket_close(listener);
	if (error != 0) {
		test_pair_close(pair);
		return error;
	}

	/* Succeeded: the pair is connected. */
	return 0;
}

/* Closes a pair's ends. */
static void
test_pair_close(
	struct test_pair *pair)
{
	/* Both ends (NULL ones are nothing to close). */
	kl_socket_close(pair->client);
	kl_socket_close(pair->server);
	pair->client = NULL;
	pair->server = NULL;
}

/* Rings a waker after a delay, as many times as asked. */
static void
test_ring(
	void *argument)
{
	struct test_ringer *ringer;
	unsigned index;

	/* The waker, after the delay, rung again and again. */
	ringer = argument;
	if (ringer->delay_ms != 0U)
		kl_thread_sleep(ringer->delay_ms);
	for (index = 0; index < ringer->rings; index++) {
		if (ringer->counter != NULL)
			(void)atomic_fetch_add(ringer->counter, 1U);
		kl_socket_waker_ring(ringer->waker);
	}
}

/* Checks reading and writing addresses. */
static void
test_addresses(void)
{
	struct kl_address address;
	char text[KL_ADDRESS_TEXT];
	char small[8];
	int error;

	/* IPv4. */
	error = kl_address_parse("192.0.2.1", 993U, &address);
	if (error == 0)
		error = kl_address_format(&address, text, sizeof(text));
	test_check(error == 0 && address.family == KL_ADDRESS_IPV4 && strcmp(text, "192.0.2.1:993") == 0, "address-ipv4");

	/* IPv6, and with a scope. */
	error = kl_address_parse("2001:db8::1", 993U, &address);
	if (error == 0)
		error = kl_address_format(&address, text, sizeof(text));
	test_check(error == 0 && address.family == KL_ADDRESS_IPV6 && strcmp(text, "[2001:db8::1]:993") == 0, "address-ipv6");
	error = kl_address_parse("fe80::1%2", 80U, &address);
	if (error == 0)
		error = kl_address_format(&address, text, sizeof(text));
	test_check(error == 0 && address.scope == 2U && strcmp(text, "[fe80::1%2]:80") == 0, "address-scope");

	/* What is not an address in numbers. */
	test_check(kl_address_parse("1.2.3", 1U, &address) == EINVAL, "address-short");
	test_check(kl_address_parse("1.2.3.4%1", 1U, &address) == EINVAL, "address-ipv4-scope");
	test_check(kl_address_parse("fe80::1%", 1U, &address) == EINVAL, "address-empty-scope");
	test_check(kl_address_parse("localhost", 1U, &address) == EINVAL, "address-name");
	test_check(kl_address_parse("", 1U, &address) == EINVAL, "address-empty");

	/* Too little room, and the any and loopback addresses. */
	(void)kl_address_parse("192.0.2.1", 993U, &address);
	test_check(kl_address_format(&address, small, sizeof(small)) == ENOSPC, "address-room");
	kl_address_loopback(KL_ADDRESS_IPV6, 7U, &address);
	error = kl_address_format(&address, text, sizeof(text));
	test_check(error == 0 && strcmp(text, "[::1]:7") == 0, "address-loopback");
	kl_address_any(KL_ADDRESS_IPV4, 8U, &address);
	error = kl_address_format(&address, text, sizeof(text));
	test_check(error == 0 && strcmp(text, "0.0.0.0:8") == 0, "address-any");
}

/* Checks resolving names and numbers. */
static void
test_resolve(void)
{
	struct kl_address addresses[8];
	char text[KL_ADDRESS_TEXT];
	size_t count;
	size_t index;
	int loopback;
	int error;

	/* localhost is the loopback, whichever family. */
	error = kl_socket_resolve("localhost", 25U, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, 8U, &count);
	loopback = error == 0 && count >= 1U;
	for (index = 0; index < count && loopback; index++) {
		(void)kl_address_format(&addresses[index], text, sizeof(text));
		if (strcmp(text, "127.0.0.1:25") != 0 && strcmp(text, "[::1]:25") != 0)
			loopback = 0;
	}
	test_check(loopback, "resolve-localhost");

	/* A number is its own address, of one family. */
	error = kl_socket_resolve("127.0.0.1", 80U, KL_ADDRESS_IPV4, KL_SOCKET_STREAM, addresses, 8U, &count);
	test_check(error == 0 && count == 1U && addresses[0].family == KL_ADDRESS_IPV4 && addresses[0].port == 80U,
	    "resolve-number");
	error = kl_socket_resolve("::1", 80U, KL_ADDRESS_IPV6, KL_SOCKET_DATAGRAM, addresses, 8U, &count);
	test_check(error == 0 && count == 1U && addresses[0].family == KL_ADDRESS_IPV6, "resolve-number-ipv6");

	/* A name under .invalid has no address (EAGAIN where the host has no resolver at all). */
	error = kl_socket_resolve("no-such-host.invalid", 80U, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, 8U, &count);
	if (error == EAGAIN)
		test_not_done("resolve-none (no resolver)");
	else
		test_check(error == ENOENT && count == 0U, "resolve-none");

	/* No room is refused. */
	error = kl_socket_resolve("localhost", 80U, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, 0U, &count);
	test_check(error == EINVAL, "resolve-no-room");
}

/* Checks a stream over the loopback that blocks: connect, accept, the addresses, bytes both ways and the end. */
static void
test_stream(
	unsigned family,
	const char *name)
{
	struct kl_socket *listener;
	struct kl_socket *client;
	struct kl_socket *server;
	struct kl_address address;
	struct kl_address peer;
	struct kl_address client_local;
	char buffer[16];
	size_t sent;
	size_t received;
	int error;

	/* A listener (no IPv6 on the host: not done). */
	error = test_listen(family, 0U, &listener, &address);
	if (error == EAFNOSUPPORT || error == EADDRNOTAVAIL) {
		test_not_done(name);
		return;
	}
	if (error != 0) {
		test_check(0, name);
		return;
	}

	/* The client connects; the server's end is accepted with the client's address. */
	client = NULL;
	server = NULL;
	error = kl_socket_open(family, KL_SOCKET_STREAM, 0U, &client);
	if (error == 0)
		error = kl_socket_connect(client, &address);
	if (error == 0)
		error = kl_socket_accept(listener, 0U, &server, &peer);
	if (error == 0)
		error = kl_socket_local_address(client, &client_local);
	if (error != 0 || peer.port != client_local.port || peer.family != family) {
		test_check(0, name);
		kl_socket_close(server);
		kl_socket_close(client);
		kl_socket_close(listener);
		return;
	}

	/* Bytes from the client to the server and back. */
	error = kl_socket_send(client, "hello", 5U, &sent);
	if (error == 0)
		error = kl_socket_receive(server, buffer, sizeof(buffer), &received);
	test_check(error == 0 && sent == 5U && received == 5U && memcmp(buffer, "hello", 5U) == 0, name);
	error = kl_socket_send(server, "back", 4U, &sent);
	if (error == 0)
		error = kl_socket_receive(client, buffer, sizeof(buffer), &received);
	test_check(error == 0 && received == 4U && memcmp(buffer, "back", 4U) == 0, "stream-back");

	/* The client's sending closed: the server receives the end. */
	error = kl_socket_shutdown(client, KL_SOCKET_SHUT_SEND);
	if (error == 0)
		error = kl_socket_receive(server, buffer, sizeof(buffer), &received);
	test_check(error == 0 && received == 0U, "stream-end");
	test_check(kl_socket_peer_address(client, &peer) == 0 && peer.port == address.port, "stream-peer");
	kl_socket_close(server);
	kl_socket_close(client);
	kl_socket_close(listener);
}

/* Checks a connection that does not block: in progress, then writable, then connected. */
static void
test_nonblocking(
	unsigned family)
{
	struct kl_socket *listener;
	struct kl_socket *client;
	struct kl_socket *server;
	struct kl_socket_wait wait;
	struct kl_address address;
	const char *name;
	size_t ready;
	int connected;
	int error;

	/* A listener. */
	name = "connect-nonblocking-ipv4";
	if (family == KL_ADDRESS_IPV6)
		name = "connect-nonblocking-ipv6";
	error = test_listen(family, 0U, &listener, &address);
	if (error == EAFNOSUPPORT || error == EADDRNOTAVAIL) {
		test_not_done(name);
		return;
	}
	if (error != 0) {
		test_check(0, name);
		return;
	}

	/* The connection starts (or is made at once over the loopback), and the wait finds it writable. */
	error = kl_socket_open(family, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &client);
	if (error != 0) {
		test_check(0, name);
		kl_socket_close(listener);
		return;
	}
	connected = kl_socket_connect(client, &address);
	wait.socket = client;
	wait.waker = NULL;
	wait.events = KL_SOCKET_WRITABLE;
	error = kl_socket_poll(&wait, 1U, TEST_LATE_MS, &ready);
	if (error == 0 && ready == 1U && (wait.ready & KL_SOCKET_WRITABLE) != 0U)
		error = kl_socket_connect_result(client);
	else
		error = EIO;
	test_check((connected == 0 || connected == EINPROGRESS) && error == 0, name);

	/* The server takes it. */
	error = kl_socket_accept(listener, 0U, &server, NULL);
	test_check(error == 0, "connect-nonblocking-accept");
	if (error == 0)
		kl_socket_close(server);
	kl_socket_close(client);
	kl_socket_close(listener);
}

/* Checks a connection refused by a port nobody listens on. */
static void
test_refused(void)
{
	struct kl_socket *listener;
	struct kl_socket *client;
	struct kl_socket_wait wait;
	struct kl_address address;
	size_t ready;
	int connected;
	int error;

	/* A port that was just listened on and is closed now. */
	error = test_listen(KL_ADDRESS_IPV4, 0U, &listener, &address);
	if (error != 0) {
		test_check(0, "connect-refused");
		return;
	}
	kl_socket_close(listener);

	/* The connection that does not block fails: the wait ends at once and the result is ECONNREFUSED. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &client);
	if (error != 0) {
		test_check(0, "connect-refused");
		return;
	}
	connected = kl_socket_connect(client, &address);
	if (connected == EINPROGRESS) {
		wait.socket = client;
		wait.waker = NULL;
		wait.events = KL_SOCKET_WRITABLE;
		error = kl_socket_poll(&wait, 1U, TEST_LATE_MS, &ready);
		if (error == 0 && ready == 1U)
			connected = kl_socket_connect_result(client);
	}
	test_check(connected == ECONNREFUSED, "connect-refused");
	kl_socket_close(client);

	/* A connection that blocks is refused too. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &client);
	if (error == 0) {
		error = kl_socket_connect(client, &address);
		kl_socket_close(client);
	}
	test_check(error == ECONNREFUSED, "connect-refused-blocking");
}

/* Checks accept on a listener that does not block, and the accepted socket's flags. */
static void
test_accept_flags(void)
{
	struct kl_socket *listener;
	struct kl_socket *client;
	struct kl_socket *server;
	struct kl_socket_wait wait;
	struct kl_address address;
	char buffer[4];
	size_t received;
	size_t ready;
	int flags;
	int error;

	/* A listener that does not block, with nobody waiting: EAGAIN. */
	error = test_listen(KL_ADDRESS_IPV4, KL_SOCKET_NONBLOCKING, &listener, &address);
	if (error != 0) {
		test_check(0, "accept-none");
		return;
	}
	error = kl_socket_accept(listener, 0U, &server, NULL);
	test_check(error == EAGAIN && server == NULL, "accept-none");

	/* A client; the wait finds the listener readable; the accepted socket does not block as asked. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &client);
	if (error == 0)
		error = kl_socket_connect(client, &address);
	if (error != 0) {
		test_check(0, "accept-ready");
		kl_socket_close(listener);
		return;
	}
	wait.socket = listener;
	wait.waker = NULL;
	wait.events = KL_SOCKET_READABLE;
	error = kl_socket_poll(&wait, 1U, TEST_LATE_MS, &ready);
	test_check(error == 0 && ready == 1U && wait.ready == KL_SOCKET_READABLE, "accept-ready");
	error = kl_socket_accept(listener, KL_SOCKET_NONBLOCKING, &server, NULL);
	if (error == 0) {
		error = kl_socket_receive(server, buffer, sizeof(buffer), &received);
		kl_socket_close(server);
	}
	test_check(error == EAGAIN, "accept-nonblocking");

	/* Another, accepted as blocking although the listener does not block. */
	kl_socket_close(client);
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0U, &client);
	if (error == 0)
		error = kl_socket_connect(client, &address);
	if (error == 0)
		error = kl_socket_poll(&wait, 1U, TEST_LATE_MS, &ready);
	if (error == 0)
		error = kl_socket_accept(listener, 0U, &server, NULL);
	flags = -1;
	if (error == 0) {
		flags = fcntl((int)kl_socket_native(server), F_GETFL);
		kl_socket_close(server);
	}
	test_check(error == 0 && flags >= 0 && (flags & O_NONBLOCK) == 0, "accept-blocking");
	kl_socket_close(client);
	kl_socket_close(listener);
}

/* Checks a wait for many sockets: only the one written to is ready, and a wait with nothing ready runs out. */
static void
test_many(void)
{
	struct test_pair pairs[TEST_PAIRS];
	struct kl_socket_wait waits[TEST_PAIRS];
	int64_t start;
	int64_t waited;
	size_t sent;
	size_t ready;
	unsigned opened;
	unsigned index;
	unsigned right;
	int error;

	/* The pairs. */
	opened = 0;
	for (index = 0; index < TEST_PAIRS; index++) {
		error = test_pair_open(KL_ADDRESS_IPV4, &pairs[index]);
		if (error != 0)
			break;
		opened++;
	}
	if (opened != TEST_PAIRS) {
		test_check(0, "poll-many-open");
		for (index = 0; index < opened; index++)
			test_pair_close(&pairs[index]);
		return;
	}

	/* Nothing to read anywhere: the wait runs out after its time. */
	for (index = 0; index < TEST_PAIRS; index++) {
		waits[index].socket = pairs[index].server;
		waits[index].waker = NULL;
		waits[index].events = KL_SOCKET_READABLE;
	}
	start = test_now_ms();
	error = kl_socket_poll(waits, TEST_PAIRS, TEST_WAIT_MS, &ready);
	waited = test_now_ms() - start;
	test_check(error == 0 && ready == 0U && waited >= TEST_WAIT_MS && waited < TEST_LATE_MS, "poll-timeout");

	/* One client writes: its server's end alone is ready. */
	error = kl_socket_send(pairs[TEST_PAIR_WRITTEN].client, "x", 1U, &sent);
	if (error == 0)
		error = kl_socket_poll(waits, TEST_PAIRS, TEST_LATE_MS, &ready);
	right = error == 0 && ready == 1U;
	for (index = 0; index < TEST_PAIRS && right; index++) {
		if (index == TEST_PAIR_WRITTEN && waits[index].ready != KL_SOCKET_READABLE)
			right = 0;
		if (index != TEST_PAIR_WRITTEN && waits[index].ready != 0U)
			right = 0;
	}
	test_check(right, "poll-many");

	/* The pairs closed. */
	for (index = 0; index < TEST_PAIRS; index++)
		test_pair_close(&pairs[index]);
}

/* Checks wakers: a ring from another thread ends a wait without an end, rings merge, many rings race a wait. */
static void
test_wakers(void)
{
	struct kl_thread_options options;
	struct kl_thread *threads[TEST_RINGERS];
	struct test_ringer ringer;
	struct test_ringer ringers[TEST_RINGERS];
	struct kl_socket_waker *waker;
	struct kl_socket_wait waits[2];
	struct test_pair pair;
	atomic_uint counter;
	unsigned started;
	unsigned index;
	unsigned seen;
	size_t ready;
	int error;

	/* A waker, and a pair whose socket nothing is written to. */
	error = kl_socket_waker_open(&waker);
	if (error == 0)
		error = test_pair_open(KL_ADDRESS_IPV4, &pair);
	if (error != 0) {
		test_check(0, "waker-open");
		return;
	}

	/* A thread rings after 50 ms: the wait without an end returns with the waker ready, the socket not. */
	memset(&options, 0, sizeof(options));
	options.name = "test-ringer";
	options.run = test_ring;
	ringer.waker = waker;
	ringer.delay_ms = TEST_WAIT_MS;
	ringer.rings = 1U;
	ringer.counter = NULL;
	options.argument = &ringer;
	error = kl_thread_start(&options, &threads[0]);
	waits[0].socket = pair.server;
	waits[0].waker = NULL;
	waits[0].events = KL_SOCKET_READABLE;
	waits[1].socket = NULL;
	waits[1].waker = waker;
	waits[1].events = KL_SOCKET_READABLE;
	if (error == 0) {
		error = kl_socket_poll(waits, 2U, KL_SOCKET_FOREVER, &ready);
		(void)kl_thread_join(threads[0]);
	}
	test_check(error == 0 && ready == 1U && waits[1].ready == KL_SOCKET_READABLE && waits[0].ready == 0U, "waker-ends-wait");

	/* Three rings before a wait are one: found once, then quiet. */
	kl_socket_waker_ring(waker);
	kl_socket_waker_ring(waker);
	kl_socket_waker_ring(waker);
	error = kl_socket_poll(&waits[1], 1U, 0, &ready);
	test_check(error == 0 && ready == 1U, "waker-merge");
	error = kl_socket_poll(&waits[1], 1U, 0, &ready);
	test_check(error == 0 && ready == 0U, "waker-quiet");

	/* Rings from several threads race the waits; one last ring after them is still found. */
	started = 0;
	for (index = 0; index < TEST_RINGERS; index++) {
		ringers[index].waker = waker;
		ringers[index].delay_ms = 0U;
		ringers[index].rings = TEST_RINGS;
		ringers[index].counter = NULL;
		options.argument = &ringers[index];
		error = kl_thread_start(&options, &threads[index]);
		if (error != 0)
			break;
		started++;
	}
	for (index = 0; index < 1000U; index++)
		(void)kl_socket_poll(&waits[1], 1U, 1, &ready);
	for (index = 0; index < started; index++)
		(void)kl_thread_join(threads[index]);
	(void)kl_socket_poll(&waits[1], 1U, 0, &ready);
	kl_socket_waker_ring(waker);
	error = kl_socket_poll(&waits[1], 1U, TEST_LATE_MS, &ready);
	test_check(started == TEST_RINGERS && error == 0 && ready == 1U, "waker-race");

	/*
	 * Each ringer adds to a counter before it rings: the waiter, looking at
	 * the counter after every wait, sees the last value before a wait runs
	 * out (a ring lost would leave it waiting with the counter short).
	 */
	(void)kl_socket_poll(&waits[1], 1U, 0, &ready);
	atomic_init(&counter, 0U);
	started = 0;
	for (index = 0; index < TEST_RINGERS; index++) {
		ringers[index].counter = &counter;
		options.argument = &ringers[index];
		error = kl_thread_start(&options, &threads[index]);
		if (error != 0)
			break;
		started++;
	}
	seen = 0;
	error = 0;
	while (seen < started * TEST_RINGS && error == 0) {
		error = kl_socket_poll(&waits[1], 1U, TEST_LATE_MS, &ready);
		if (error == 0 && ready == 0U)
			error = ETIMEDOUT;
		seen = atomic_load(&counter);
	}
	for (index = 0; index < started; index++)
		(void)kl_thread_join(threads[index]);
	test_check(started == TEST_RINGERS && error == 0 && seen == TEST_RINGERS * TEST_RINGS, "waker-no-lost-ring");
	test_pair_close(&pair);
	kl_socket_waker_close(waker);
}

/* Checks the peer's close: the end received, and a send that fails without a signal. */
static void
test_ends(void)
{
	struct test_pair pair;
	struct kl_socket_wait wait;
	char buffer[4096];
	size_t received;
	size_t sent;
	size_t ready;
	unsigned tries;
	int error;

	/* The server closes: the client's wait finds it readable, and the receive is the end. */
	error = test_pair_open(KL_ADDRESS_IPV4, &pair);
	if (error != 0) {
		test_check(0, "end-received");
		return;
	}
	kl_socket_close(pair.server);
	pair.server = NULL;
	wait.socket = pair.client;
	wait.waker = NULL;
	wait.events = KL_SOCKET_READABLE;
	error = kl_socket_poll(&wait, 1U, TEST_LATE_MS, &ready);
	if (error == 0 && ready == 1U)
		error = kl_socket_receive(pair.client, buffer, sizeof(buffer), &received);
	else
		error = EIO;
	test_check(error == 0 && received == 0U, "end-received");

	/* Sends to the closed peer fail with EPIPE or ECONNRESET, and no signal ends the program. */
	memset(buffer, 'x', sizeof(buffer));
	error = 0;
	for (tries = 0; tries < 100U && error == 0; tries++)
		error = kl_socket_send(pair.client, buffer, sizeof(buffer), &sent);
	test_check(error == EPIPE || error == ECONNRESET, "send-after-close");
	test_pair_close(&pair);
}

/* Checks datagrams over the loopback: sent to an address, received with where they came from, cut to the room. */
static void
test_datagrams(
	unsigned family)
{
	struct kl_socket *receiver;
	struct kl_socket *sender;
	struct kl_address loopback;
	struct kl_address address;
	struct kl_address sender_address;
	struct kl_address from;
	const char *name;
	char buffer[4];
	size_t sent;
	size_t received;
	int error;

	/* A receiver bound to the loopback (no IPv6 on the host: not done). */
	name = "datagram-ipv4";
	if (family == KL_ADDRESS_IPV6)
		name = "datagram-ipv6";
	error = kl_socket_open(family, KL_SOCKET_DATAGRAM, 0U, &receiver);
	if (error == EAFNOSUPPORT) {
		test_not_done(name);
		return;
	}
	if (error != 0) {
		test_check(0, name);
		return;
	}
	kl_address_loopback(family, 0U, &loopback);
	error = kl_socket_bind(receiver, &loopback);
	if (error == EADDRNOTAVAIL) {
		test_not_done(name);
		kl_socket_close(receiver);
		return;
	}
	if (error == 0)
		error = kl_socket_local_address(receiver, &address);

	/* A sender bound too, so that its address can be compared. */
	sender = NULL;
	if (error == 0)
		error = kl_socket_open(family, KL_SOCKET_DATAGRAM, 0U, &sender);
	if (error == 0)
		error = kl_socket_bind(sender, &loopback);
	if (error == 0)
		error = kl_socket_local_address(sender, &sender_address);

	/* Ten bytes sent, four received (the rest cut), from the sender. */
	if (error == 0)
		error = kl_socket_send_to(sender, "0123456789", 10U, &address, &sent);
	if (error == 0)
		error = kl_socket_receive_from(receiver, buffer, sizeof(buffer), &from, &received);
	test_check(error == 0 && sent == 10U && received == 4U && memcmp(buffer, "0123", 4U) == 0 &&
	    from.port == sender_address.port && from.family == family, name);
	kl_socket_close(sender);
	kl_socket_close(receiver);
}

/* Checks that an IPv6 socket talks IPv6 alone until told otherwise. */
static void
test_ipv6_only(void)
{
	struct kl_socket *socket6;
	socklen_t length;
	int only;
	int status;
	int error;

	/* The socket (no IPv6 on the host: not done). */
	error = kl_socket_open(KL_ADDRESS_IPV6, KL_SOCKET_STREAM, 0U, &socket6);
	if (error == EAFNOSUPPORT) {
		test_not_done("ipv6-only");
		return;
	}

	/* 1 when it opens, 0 when set so; the option means nothing to a datagram's NO_DELAY. */
	only = -1;
	length = sizeof(only);
	status = error;
	if (status == 0)
		status = getsockopt((int)kl_socket_native(socket6), IPPROTO_IPV6, IPV6_V6ONLY, &only, &length);
	test_check(status == 0 && only == 1, "ipv6-only");
	error = kl_socket_set_option(socket6, KL_SOCKET_IPV6_ONLY, 0);
	length = sizeof(only);
	if (error == 0)
		status = getsockopt((int)kl_socket_native(socket6), IPPROTO_IPV6, IPV6_V6ONLY, &only, &length);
	test_check(error == 0 && status == 0 && only == 0, "ipv6-only-off");
	test_check(kl_socket_set_option(socket6, KL_SOCKET_BROADCAST, 1) == ENOPROTOOPT, "option-wrong-kind");
	test_check(kl_socket_set_option(socket6, KL_SOCKET_NO_DELAY, 2) == EINVAL, "option-value");
	kl_socket_close(socket6);
}

/* Checks a wait's limits: none but time, a wait naming both or neither, unknown events. */
static void
test_poll_limits(void)
{
	struct kl_socket_waker *waker;
	struct kl_socket_wait wait;
	int64_t start;
	int64_t waited;
	size_t ready;
	int error;

	/* No waits: only the time passes; without an end it is refused. */
	start = test_now_ms();
	error = kl_socket_poll(NULL, 0U, TEST_WAIT_MS, &ready);
	waited = test_now_ms() - start;
	test_check(error == 0 && ready == 0U && waited >= TEST_WAIT_MS, "poll-none");
	test_check(kl_socket_poll(NULL, 0U, KL_SOCKET_FOREVER, &ready) == EINVAL, "poll-none-forever");

	/* A wait with neither, with both, or with an event a wait cannot ask for. */
	error = kl_socket_waker_open(&waker);
	if (error != 0) {
		test_check(0, "poll-wrong");
		return;
	}
	memset(&wait, 0, sizeof(wait));
	wait.events = KL_SOCKET_READABLE;
	test_check(kl_socket_poll(&wait, 1U, 0, &ready) == EINVAL, "poll-neither");
	wait.waker = waker;
	wait.events = KL_SOCKET_FAILED;
	test_check(kl_socket_poll(&wait, 1U, 0, &ready) == EINVAL, "poll-unknown-event");
	kl_socket_waker_close(waker);
}
