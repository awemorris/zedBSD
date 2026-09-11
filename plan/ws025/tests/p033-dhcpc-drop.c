/* Test-only receive boundary; no hook is installed in the ordinary dhcpc. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static ssize_t p033_receive(int, void *, size_t, int, struct sockaddr *,
    socklen_t *);

#define recvfrom p033_receive
#include "userland/base/dhcpc/main.c"
#undef recvfrom

/* Suppresses real replies while the disposable guest's gate file exists. */
static ssize_t
p033_receive(
	int descriptor,
	void *buffer,
	size_t length,
	int flags,
	struct sockaddr *source,
	socklen_t *source_length)
{
	static const char notice[] = "p033: DHCP reply suppressed\n";
	socklen_t capacity;
	ssize_t count;
	int console;

	capacity = *source_length;
	for (;;) {
		if (timed_out) {
			errno = EINTR;
			return -1;
		}
		*source_length = capacity;
		count = recvfrom(descriptor, buffer, length, flags, source,
		    source_length);
		if (count < 0)
			return count;
		if (access("/run/p033-drop-dhcp", F_OK) != 0)
			return count;

		/* Confirms receipt on real hardware without allowing DHCP to select it. */
		console = open("/dev/console",
		    O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
		if (console >= 0) {
			(void)write(console, notice, sizeof(notice) - 1U);
			(void)close(console);
		}
	}
}
