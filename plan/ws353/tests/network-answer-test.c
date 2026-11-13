/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The guest test of the network link's waits: libkeiland-backend-zedbsd's
 * network-zedbsd.c and daemon-link-zedbsd.c on the zedBSD kernel, linked
 * with -Wl,--wrap=poll so that each poll of the link's thread is counted.
 *
 * "stand-in" (the program built with NETWORKD_SOCKET in a folder of its
 * own): a stand-in networkd keeps the watch, answers a join after 3 s and
 * each frame of the scans' connection after 1 s.  It checks: the watch's
 * state; 3 s idle with no more than two waits of the link's thread; the
 * join answered (error 0) no earlier than the daemon's answer and within
 * 300 ms of it, the link's thread waking no more than four times over the
 * join and never for a POLLERR (the request's connection is shut down for
 * writing: the kernel must not report that as an error); the scans asked
 * for 8 s: a scan with the stand-in's network, the link's thread waking no
 * more than 24 times; the scans stopped (the stand-in told); the close.
 *
 * "real" (the program built with networkd's own socket): against the
 * guest's networkd, the watch's state and a scan request answered through
 * the update within 20 s (whatever the answer: the guest has no Wi-Fi),
 * the link's thread waking no more than eight times.
 *
 * Each check is a line "NAME: PASS ..." or "NAME: FAIL ..." on the result
 * file; the last line is "network-answer-test: PASS" or "... FAIL".
 *
 * usage: network-answer-test stand-in|real RESULT-FILE
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* How long the stand-in takes to answer a join, and a frame of the scans' connection, in milliseconds. */
#define TEST_JOIN_DELAY_MS	3000U
#define TEST_SCAN_DELAY_MS	1000U

/* The stand-in's scan: one network, "home". */
#define TEST_SCAN_TEXT	"wlan0 bss index=0 ssid=686f6d65 bssid=020000000001 rssi=-40 security=0\n"

/*
 * The waits of the link's thread: polls with a timeout other than 0 that
 * slept (ended on their timeout, or returned 1 ms or more after the call:
 * the frame reader's poll before each byte that has already arrived is not
 * a wait, nor is network_readable's look), those that ended on their
 * timeout, every poll with a timeout other than 0, and the POLLERR seen on
 * any descriptor.  The wrapper writes them from the link's thread; the
 * checks read them from the main thread.
 */
static unsigned link_waits;
static unsigned link_polls;
static unsigned link_timeouts;
static unsigned link_errors;

/* The link's thread, once it runs (link_known set). */
static pthread_t link_thread;
static int link_known;

/* The wake pipe lent to the backend. */
static int wake[2] = { -1, -1 };

/* The result file and whether every check passed. */
static FILE *result;
static int failed;

/* When the stand-in wrote the join's answer, in milliseconds; 0 until then. */
static unsigned long long join_answered_ms;

/* How many stops of the scans the stand-in was told. */
static unsigned scan_stops;

/* What a link's thread is given by the lent maker: the backend's run and argument. */
struct test_start {
	void *(*run)(void *);
	void *argument;
};

int __real_poll(struct pollfd *descriptors, nfds_t count, int timeout);
int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout);
void kl_backend_threads(struct kl_backend_threads *threads);
static int lent_start(const char *name, void *(*run)(void *), void *argument, pthread_t *thread);
static void lent_ended(const char *name);
static void *link_trampoline(void *argument);
static void say(int ok, const char *name, const char *format, ...);
static unsigned long long now_ms(void);
static unsigned load(const unsigned *counter);
static unsigned wait_changed(struct kl_backend_network *network, unsigned wanted, unsigned limit_ms);
static void wait_reachable(struct kl_backend_network *network, struct kl_backend_network_state *state, unsigned limit_ms);
static void pause_ms(unsigned milliseconds);
static int listen_at(const char *path);
static void *stand_in_main(void *argument);
static void *stand_in_client(void *argument);
static void stand_in_answer(int descriptor, const struct networkd_protocol_header *asked, const char *output);
static void test_stand_in(void);
static void test_real(void);

/*
 * Counts a wait of the link's thread, then waits as the C library does.
 */
int
__wrap_poll(
	struct pollfd *descriptors,
	nfds_t count,
	int timeout)
{
	struct timespec before;
	struct timespec after;
	long long slept_us;
	pthread_t self;
	nfds_t index;
	int ready;
	int mine;

	/* The wait itself, timed. */
	(void)clock_gettime(CLOCK_MONOTONIC, &before);
	ready = __real_poll(descriptors, count, timeout);
	(void)clock_gettime(CLOCK_MONOTONIC, &after);

	/* Only the link's thread's waits are counted; a look (timeout 0) is not a wait. */
	mine = 0;
	self = pthread_self();
	if (__atomic_load_n(&link_known, __ATOMIC_ACQUIRE) != 0 && pthread_equal(self, link_thread))
		mine = 1;
	if (!mine || timeout == 0)
		return ready;

	/* The poll, whether it slept, whether its timeout ended it, and each POLLERR it reported. */
	__atomic_add_fetch(&link_polls, 1U, __ATOMIC_RELEASE);
	slept_us = (long long)(after.tv_sec - before.tv_sec) * 1000000LL + (after.tv_nsec - before.tv_nsec) / 1000L;
	if (ready == 0 || slept_us >= 1000LL)
		__atomic_add_fetch(&link_waits, 1U, __ATOMIC_RELEASE);
	if (ready == 0)
		__atomic_add_fetch(&link_timeouts, 1U, __ATOMIC_RELEASE);
	for (index = 0; index < count && ready > 0; index++) {
		if ((descriptors[index].revents & POLLERR) != 0)
			__atomic_add_fetch(&link_errors, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the poll's own answer. */
	return ready;
}

/*
 * Lends the test's wake pipe and thread maker to the network's link, as
 * the compositor does through kl_backend_open.
 */
void
kl_backend_threads(
	struct kl_backend_threads *threads)
{
	/* The pipe and the maker. */
	memset(threads, 0, sizeof(*threads));
	threads->wake_descriptor = wake[1];
	threads->start = lent_start;
	threads->ended = lent_ended;
}

/* Runs the checks of the mode named; returns 0 when all passed. */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* The mode and the result file. */
	if (argc != 3) {
		fprintf(stderr, "usage: network-answer-test stand-in|real RESULT-FILE\n");
		return 2;
	}
	result = fopen(argv[2], "w");
	if (result == NULL)
		return 2;
	setvbuf(result, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);

	/* The wake pipe, which never blocks. */
	error = pipe(wake);
	if (error != 0)
		return 2;
	(void)fcntl(wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(wake[1], F_SETFL, O_NONBLOCK);

	/* The mode's checks. */
	if (strcmp(argv[1], "stand-in") == 0) {
		test_stand_in();
	} else {
		test_real();
	}

	/* The outcome. */
	if (failed) {
		fprintf(result, "network-answer-test: FAIL\n");
		fclose(result);
		return 1;
	}

	/* Succeeded: every check passed. */
	fprintf(result, "network-answer-test: PASS\n");
	fclose(result);
	return 0;
}

/* Makes the link's thread through a trampoline that names it to the poll wrapper. */
static int
lent_start(
	const char *name,
	void *(*run)(void *),
	void *argument,
	pthread_t *thread)
{
	struct test_start *start;
	int error;

	(void)name;

	/* What the thread runs. */
	start = malloc(sizeof(*start));
	if (start == NULL)
		return ENOMEM;
	start->run = run;
	start->argument = argument;

	/* The thread. */
	error = pthread_create(thread, NULL, link_trampoline, start);
	if (error != 0) {
		free(start);
		return error;
	}

	/* Succeeded: the thread runs. */
	return 0;
}

/* Takes the link's thread off: its waits are not counted any more. */
static void
lent_ended(
	const char *name)
{
	(void)name;

	/* No thread to count. */
	__atomic_store_n(&link_known, 0, __ATOMIC_RELEASE);
}

/* Names the thread as the link's, then runs the backend's thread. */
static void *
link_trampoline(
	void *argument)
{
	struct test_start start;

	/* The run and its argument. */
	start = *(struct test_start *)argument;
	free(argument);

	/* This thread's waits are counted from here on. */
	link_thread = pthread_self();
	__atomic_store_n(&link_known, 1, __ATOMIC_RELEASE);

	/* The backend's thread. */
	return start.run(start.argument);
}

/* Writes one check's line to the result file. */
static void
say(
	int ok,
	const char *name,
	const char *format,
	...)
{
	va_list arguments;

	/* The name and the verdict. */
	fprintf(result, "%s: %s ", name, ok ? "PASS" : "FAIL");
	if (!ok)
		failed = 1;

	/* What was seen. */
	va_start(arguments, format);
	vfprintf(result, format, arguments);
	va_end(arguments);
	fprintf(result, "\n");
}

/* Gives the monotonic clock in milliseconds. */
static unsigned long long
now_ms(
	void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

/* Reads a counter the link's thread writes. */
static unsigned
load(
	const unsigned *counter)
{
	/* The counter as it stands. */
	return __atomic_load_n(counter, __ATOMIC_ACQUIRE);
}

/*
 * Waits on the wake pipe (as the compositor's event loop does) and takes
 * the updates until one of the wanted changes came, at most limit_ms;
 * returns the changes seen.
 */
static unsigned
wait_changed(
	struct kl_backend_network *network,
	unsigned wanted,
	unsigned limit_ms)
{
	struct pollfd descriptor;
	unsigned long long until;
	unsigned long long now;
	unsigned changed;
	unsigned seen;
	char drained[64];

	/* Until a wanted change or the limit. */
	seen = 0U;
	until = now_ms() + limit_ms;
	for (;;) {
		(void)kl_backend_network_update(network, &changed);
		seen |= changed;
		if ((seen & wanted) != 0U)
			break;
		now = now_ms();
		if (now >= until)
			break;
		descriptor.fd = wake[0];
		descriptor.events = POLLIN;
		descriptor.revents = 0;
		(void)poll(&descriptor, 1, (int)(until - now));
		while (read(wake[0], drained, sizeof(drained)) > 0)
			continue;
	}

	/* Succeeded: what changed. */
	return seen;
}

/*
 * Waits until the watch reports a state (the watch's connection alone
 * reports the empty one first), at most limit_ms.
 */
static void
wait_reachable(
	struct kl_backend_network *network,
	struct kl_backend_network_state *state,
	unsigned limit_ms)
{
	unsigned long long until;
	unsigned long long now;

	/* Each state change until a reachable one or the limit. */
	until = now_ms() + limit_ms;
	for (;;) {
		kl_backend_network_get_state(network, state);
		if (state->reachable == 1U)
			break;
		now = now_ms();
		if (now >= until)
			break;
		(void)wait_changed(network, KL_BACKEND_NETWORK_CHANGED_STATE, (unsigned)(until - now));
	}
}

/* Sleeps for a while. */
static void
pause_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	/* The sleep. */
	wait.tv_sec = milliseconds / 1000U;
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}

/* Listens on a path (a new socket); returns the listener or -1. */
static int
listen_at(
	const char *path)
{
	struct sockaddr_un address;
	int listener;
	int error;

	/* The socket, bound and listening. */
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	(void)unlink(path);
	error = bind(listener, (struct sockaddr *)&address, sizeof(address));
	if (error == 0)
		error = listen(listener, 8);
	if (error != 0) {
		close(listener);
		return -1;
	}

	/* Succeeded: the listener. */
	return listener;
}

/* The stand-in networkd: each connection on a thread of its own, until the listener is shut. */
static void *
stand_in_main(
	void *argument)
{
	pthread_t thread;
	int *client;
	int listener;
	int descriptor;

	/* Each connection. */
	listener = *(int *)argument;
	for (;;) {
		descriptor = accept(listener, NULL, NULL);
		if (descriptor < 0)
			break;
		client = malloc(sizeof(*client));
		if (client == NULL) {
			close(descriptor);
			continue;
		}
		*client = descriptor;
		if (pthread_create(&thread, NULL, stand_in_client, client) != 0) {
			close(descriptor);
			free(client);
			continue;
		}
		(void)pthread_detach(thread);
	}

	/* Succeeded: the listener was shut. */
	return NULL;
}

/* One connection of the stand-in: a frame read and answered after its delay; the watch is kept. */
static void *
stand_in_client(
	void *argument)
{
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	char drained[64];
	int descriptor;
	int error;

	/* The frame. */
	descriptor = *(int *)argument;
	free(argument);
	error = networkd_protocol_read_frame(descriptor, &header, payload, sizeof(payload), NETWORKD_REQUEST_MAX);
	if (error != 0) {
		close(descriptor);
		return NULL;
	}

	/* The answer, after the frame's delay. */
	switch (header.opcode) {
	case NETWORKD_OP_SUBSCRIBE:
		stand_in_answer(descriptor, &header, "ne0 static online\nwifi state=idle interface=wlan0 ssid= radios=1 scan=0 radio=wlan0\n");
		while (read(descriptor, drained, sizeof(drained)) > 0)
			continue;
		break;
	case NETWORKD_OP_WIFI_CONNECT:
		pause_ms(TEST_JOIN_DELAY_MS);
		__atomic_store_n(&join_answered_ms, now_ms(), __ATOMIC_RELEASE);
		stand_in_answer(descriptor, &header, "");
		break;
	case NETWORKD_OP_WIFI_LIST:
		pause_ms(TEST_SCAN_DELAY_MS);
		stand_in_answer(descriptor, &header, TEST_SCAN_TEXT);
		break;
	case NETWORKD_OP_WIFI_SCAN_STOP:
		__atomic_add_fetch(&scan_stops, 1U, __ATOMIC_RELEASE);
		pause_ms(TEST_SCAN_DELAY_MS);
		stand_in_answer(descriptor, &header, "");
		break;
	default:
		pause_ms(TEST_SCAN_DELAY_MS);
		stand_in_answer(descriptor, &header, "");
		break;
	}

	/* The connection ends with the answer (the watch when its client goes). */
	close(descriptor);
	return NULL;
}

/* Writes an answer frame: OK, no errno, and the text. */
static void
stand_in_answer(
	int descriptor,
	const struct networkd_protocol_header *asked,
	const char *output)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[512];

	/* The fields. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	(void)networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS, NETWORKD_RESULT_OK);
	(void)networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR, 0U);
	(void)networkd_field_write(&writer, NETWORKD_FIELD_OUTPUT, output, strlen(output));

	/* The frame, of the request's id and opcode. */
	header.request_id = asked->request_id;
	header.opcode = asked->opcode;
	header.payload_length = (uint32_t)writer.used;
	(void)networkd_protocol_write_frame(descriptor, &header, payload);
}

/* The checks against the stand-in networkd. */
static void
test_stand_in(void)
{
	struct kl_backend_network_state state;
	struct kl_backend_network_ap aps[4];
	struct kl_backend_network *network;
	unsigned long long asked_ms;
	unsigned long long done_ms;
	unsigned long long answered_ms;
	pthread_t daemon;
	unsigned request;
	unsigned seen;
	unsigned waits;
	unsigned timeouts;
	unsigned errors;
	unsigned polls;
	size_t found;
	int listener;
	int error;

	/* The stand-in, then the watch's state. */
	listener = listen_at(NETWORKD_SOCKET);
	if (listener < 0) {
		say(0, "stand-in", "no listener at %s (errno %d)", NETWORKD_SOCKET, errno);
		return;
	}
	(void)pthread_create(&daemon, NULL, stand_in_main, &listener);
	network = kl_backend_network_open();
	if (network == NULL) {
		say(0, "open", "kl_backend_network_open failed");
		return;
	}
	wait_reachable(network, &state, 5000U);
	say(state.reachable == 1U, "watch", "reachable=%u", state.reachable);

	/* Idle: the link's thread sleeps on its connections. */
	waits = load(&link_waits);
	pause_ms(3000U);
	waits = load(&link_waits) - waits;
	say(waits <= 2U, "idle", "waits=%u in 3000 ms (wanted <= 2; a 10 ms look makes 300)", waits);

	/* A join, answered after 3 s: the answer wakes the link's thread. */
	waits = load(&link_waits);
	polls = load(&link_polls);
	timeouts = load(&link_timeouts);
	errors = load(&link_errors);
	asked_ms = now_ms();
	error = kl_backend_network_request(network, KL_BACKEND_NETWORK_REQUEST_JOIN, "home");
	seen = 0U;
	if (error == 0)
		seen = wait_changed(network, KL_BACKEND_NETWORK_CHANGED_DONE, 10000U);
	done_ms = now_ms();
	answered_ms = __atomic_load_n(&join_answered_ms, __ATOMIC_ACQUIRE);
	request = kl_backend_network_get_request(network, &error);
	waits = load(&link_waits) - waits;
	polls = load(&link_polls) - polls;
	timeouts = load(&link_timeouts) - timeouts;
	errors = load(&link_errors) - errors;
	say((seen & KL_BACKEND_NETWORK_CHANGED_DONE) != 0U && request == KL_BACKEND_NETWORK_REQUEST_JOIN && error == 0,
	    "join-answer", "request=%u error=%d after %llu ms", request, error, done_ms - asked_ms);
	say(answered_ms != 0ULL && done_ms >= answered_ms && done_ms - answered_ms <= 300ULL,
	    "join-latency", "taken %llu ms after the stand-in answered (wanted <= 300)", answered_ms != 0ULL ? done_ms - answered_ms : 0ULL);
	say(waits <= 4U && timeouts == 0U && errors == 0U,
	    "join-waits", "waits=%u timeouts=%u pollerr=%u over %llu ms (wanted <= 4, 0, 0; polls with a timeout %u)", waits, timeouts, errors, done_ms - asked_ms, polls);

	/* The scans asked for: the lease, then a reading every 3 s, each answered after 1 s. */
	waits = load(&link_waits);
	errors = load(&link_errors);
	(void)kl_backend_network_set_scanning(network, 1U);
	seen = wait_changed(network, KL_BACKEND_NETWORK_CHANGED_SCAN, 8000U);
	found = kl_backend_network_get_scan(network, aps, 4U);
	pause_ms(4000U);
	waits = load(&link_waits) - waits;
	errors = load(&link_errors) - errors;
	say((seen & KL_BACKEND_NETWORK_CHANGED_SCAN) != 0U && found == 1U && strcmp(aps[0].ssid, "home") == 0,
	    "scan-answer", "networks=%zu first=%s", found, found > 0U ? aps[0].ssid : "-");
	say(waits <= 24U && errors == 0U, "scan-waits", "waits=%u pollerr=%u (wanted <= 24, 0)", waits, errors);

	/* The scans no longer asked for: the stand-in is told. */
	(void)kl_backend_network_set_scanning(network, 0U);
	pause_ms(4000U);
	say(load(&scan_stops) >= 1U, "scan-stop", "stops=%u", load(&scan_stops));

	/* The close ends the link's thread. */
	kl_backend_network_close(network);
	say(__atomic_load_n(&link_known, __ATOMIC_ACQUIRE) == 0, "close", "the link's thread taken off");
	shutdown(listener, SHUT_RDWR);
	close(listener);
}

/* The checks against the guest's networkd. */
static void
test_real(void)
{
	struct kl_backend_network_state state;
	struct kl_backend_network *network;
	unsigned long long asked_ms;
	unsigned request;
	unsigned seen;
	unsigned waits;
	unsigned errors;
	int error;

	/* The watch's state. */
	network = kl_backend_network_open();
	if (network == NULL) {
		say(0, "real-open", "kl_backend_network_open failed");
		return;
	}
	wait_reachable(network, &state, 10000U);
	say(state.reachable == 1U, "real-watch", "reachable=%u interface=%s", state.reachable, state.interface);

	/* A scan request, answered by networkd through the update. */
	waits = load(&link_waits);
	errors = load(&link_errors);
	asked_ms = now_ms();
	error = kl_backend_network_request(network, KL_BACKEND_NETWORK_REQUEST_SCAN, NULL);
	seen = 0U;
	if (error == 0)
		seen = wait_changed(network, KL_BACKEND_NETWORK_CHANGED_DONE, 20000U);
	request = kl_backend_network_get_request(network, &error);
	waits = load(&link_waits) - waits;
	errors = load(&link_errors) - errors;
	say((seen & KL_BACKEND_NETWORK_CHANGED_DONE) != 0U && request == KL_BACKEND_NETWORK_REQUEST_SCAN,
	    "real-scan", "request=%u error=%d after %llu ms", request, error, now_ms() - asked_ms);
	say(waits <= 8U && errors == 0U, "real-waits", "waits=%u pollerr=%u (wanted <= 8, 0)", waits, errors);

	/* The close ends the link's thread. */
	kl_backend_network_close(network);
}
