/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Loopback TCP bulk test (ws034-p042).
 *
 * For each size, a child connects to a listener on 127.0.0.1 and sends that
 * many bytes with one write(), then closes; the parent reads until end of
 * file and checks the count and the bytes.  Each case is bounded by an alarm
 * so a lost segment shows as a timeout rather than a hang.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char pattern(size_t i) { return (unsigned char)(i * 7U + (i >> 8)); }

/* Seconds a case may take: TCPBULK_ALARM, or 20. */
static unsigned
limit(void)
{
	const char *text = getenv("TCPBULK_ALARM");

	return text != NULL && atoi(text) > 0 ? (unsigned)atoi(text) : 20U;
}

/* How much the reader had when the alarm went off, for the report. */
static volatile size_t progress;
static volatile size_t expected;

static void
timed_out(int signal_number)
{
	char line[96];
	int n;

	(void)signal_number;
	n = snprintf(line, sizeof(line), "TCPBULK size=%zu TIMEOUT received=%zu FAIL\n",
		     (size_t)expected, (size_t)progress);
	(void)write(1, line, (size_t)n);
	_exit(4);
}

static int
run(size_t size, int port)
{
	struct sockaddr_in address;
	unsigned char *buffer;
	size_t received = 0, i;
	ssize_t n = 0;
	int listener, peer, one = 1, bad = 0, status, shared;
	pid_t child;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listener, 1) != 0) {
		printf("TCPBULK size=%zu setup errno=%d\n", size, errno);
		return 1;
	}
	buffer = malloc(size);

	/*
	 * TCPBULK_SHARED=1 fills the buffer before fork() and the writer sends
	 * from it, so the reader's first write to its own copy is a
	 * copy-on-write fault on a page the blocked writer has pinned
	 * (ws034-p043).
	 */
	shared = getenv("TCPBULK_SHARED") != NULL;
	if (shared)
		for (i = 0; i < size; i++)
			buffer[i] = pattern(i);

	child = fork();
	if (child == 0) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		size_t sent = 0;
		unsigned char *out = shared ? buffer : malloc(size);

		/*
		 * The writer fills a buffer of its own after fork(): the pages
		 * a blocked write() pins are then its alone, and the reader's
		 * are not shared with them (see plan/ws034/phase042/phase.md).
		 */
		if (!shared)
			for (i = 0; i < size; i++)
				out[i] = pattern(i);
		buffer = out;
		alarm(limit());
		if (connect(s, (struct sockaddr *)&address, sizeof(address)) != 0)
			_exit(2);
		while (sent < size) {
			n = write(s, buffer + sent, size - sent);
			if (n <= 0) {
				printf("TCPBULK writer stopped sent=%zu errno=%d\n", sent, errno);
				fflush(stdout);
				_exit(3);
			}
			sent += (size_t)n;
		}
		close(s);
		_exit(0);
	}

	expected = size;
	progress = 0;
	signal(SIGALRM, timed_out);
	alarm(limit());
	if (getenv("TCPBULK_TRACE") != NULL)
		(void)write(2, "TCPBULK accepting\n", 18);
	peer = accept(listener, NULL, NULL);
	if (getenv("TCPBULK_TRACE") != NULL)
		(void)write(2, "TCPBULK accepted\n", 17);
	memset(buffer, 0, size);
	while (peer >= 0 && (n = read(peer, buffer + received,
	    received < size ? size - received : 1)) > 0) {
		received += (size_t)n;
		progress = received;
		if (getenv("TCPBULK_TRACE") != NULL) {
			char line[48];
			int k = snprintf(line, sizeof(line), "TCPBULK read %zd total %zu\n", n, received);
			(void)write(2, line, (size_t)k);
		}
	}
	if (getenv("TCPBULK_TRACE") != NULL) {
		char line[64];
		int k = snprintf(line, sizeof(line), "TCPBULK read loop ended n=%zd errno=%d\n", n, errno);
		(void)write(2, line, (size_t)k);
	}
	alarm(0);
	for (i = 0; i < received && i < size; i++)
		if (buffer[i] != pattern(i))
			bad++;
	waitpid(child, &status, 0);
	printf("TCPBULK size=%zu received=%zu corrupt=%d child=%d %s\n", size, received,
	       bad, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
	       received == size && bad == 0 ? "OK" : "FAIL");
	free(buffer);
	if (peer >= 0)
		close(peer);
	close(listener);
	return received == size && bad == 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	static const size_t sizes[] = { 100, 1024, 1025, 1600, 4096, 4097, 16384, 65536, 262144 };
	int failed = 0;
	size_t i;

	if (argc > 1)
		return run(strtoul(argv[1], NULL, 0), 5600 + (int)(strtoul(argv[1], NULL, 0) % 400U));
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		failed |= run(sizes[i], 5600 + (int)i);
	printf("TCPBULK %s\n", failed ? "FAIL" : "PASS");
	return failed;
}
