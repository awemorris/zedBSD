/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD networkd userland command.
 */

#include "userland/base/net/netutil.h"
#include "userland/base/net/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/route.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_OUTPUT_MAX 384

static volatile sig_atomic_t stopping;

static int open_listener(void);
static void notify_init(const char *record);
static int write_all(int descriptor, const char *buffer, size_t length);
static void handle_request(int client);
static int read_request(int descriptor, char buffer[NETWORKD_REQUEST_MAX]);
static void send_error(int client, int error, const char *reason);
static int show_interfaces(const char *name, char *output, size_t capacity);
static int append_interface_status(int descriptor, const char *name, char *output, const size_t capacity, size_t *used);
static int interface_exists(const char *name);
static int run_command(char *const arguments[], unsigned timeout_seconds, char diagnostic[CHILD_OUTPUT_MAX]);
static void clean_diagnostic(char *text);
static int parse_seconds(const char *text, unsigned *result);
static int default_route_exists(void);
static int write_resolver(char *const addresses[], int count);
static void handle_signal(int signal_number);
static void ignore_signal(int signal_number);

/*
 * Runs the networkd command.
 */
int
main(
	void)
{
	char record[256];
	int saved;
	int client;
	int listener;

	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	listener = open_listener();

	/* Handles the listener condition. */
	if (listener < 0) {
		saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s\n", saved,
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket: %s\n",
			strerror(saved));

		/* Reports operation failure. */
		return 1;
	}
	notify_init("READY\n");

	/* Continue while the operation condition remains true. */
	while (!stopping) {
		client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		/* Handles the client condition. */
		if (client >= 0) {
			handle_request(client);
			close(client);
			continue;
		}

		/* Handles the reported system error. */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		sleep(1);
	}
	close(listener);
	unlink(NETWORKD_SOCKET);

	/* Returns the computed result. */
	return stopping ? 0 : 1;
}

/* Supports the open listener operation. */
static int
open_listener(
	void)
{
	int saved;
	struct sockaddr_un address;
	int descriptor;

	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	(void)unlink(NETWORKD_SOCKET);

	/* Handles a failed bind operation. */
	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    chmod(NETWORKD_SOCKET, 0600) != 0 || listen(descriptor, 8) != 0) {
		saved = errno;
		close(descriptor);
		errno = saved;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return descriptor;
}

/* Supports the notify init operation. */
static void
notify_init(
	const char *record)
{
	const char *value;

	value = getenv("ZEDBSD_NOTIFY_FD");

	/* Handles the value availability. */
	if (value == NULL || strcmp(value, "3") != 0)
		return;
	(void)write_all(3, record, strlen(record));
	(void)close(3);
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const char *buffer,
	size_t length)
{
	ssize_t count;
	size_t offset;

	/* Process each remaining element. */
	offset = 0;
	while (offset < length) {
		count = write(descriptor, buffer + offset, length - offset);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the handle request operation. */
static void
handle_request(
	int client)
{
	struct in_addr address, mask;
	unsigned prefix;
	char seconds[16];
	struct in_addr gateway;
	int present;
	char request[NETWORKD_REQUEST_MAX], response[NETWORKD_RESPONSE_MAX];
	char diagnostic[CHILD_OUTPUT_MAX], *arguments[16], *token;
	char *items[16];
	int count, result, error;
	unsigned timeout;

	count = 0;
	result = -1;
	error = EINVAL;
	timeout = 10;

	/* Handles a failed read request operation. */
	if (read_request(client, request) != 0) {
		send_error(client, errno, "malformed request");

		/* Returns the computed result. */
		return;
	}

	/* Process each element required by the operation. */
	for (token = strtok(request, " \t"); token != NULL;
	     token = strtok(NULL, " \t")) {
		/* Checks the remaining item count. */
		if (count == (int)(sizeof(items) / sizeof(items[0]))) {
			send_error(client, E2BIG, "too many operands");

			/* Returns the computed result. */
			return;
		}
		items[count++] = token;
	}

	/* Checks the remaining item count. */
	if (count < 2 || strcmp(items[0], NETWORKD_PROTOCOL_VERSION) != 0) {
		send_error(client, EINVAL, "unsupported protocol");

		/* Returns the computed result. */
		return;
	}
	diagnostic[0] = '\0';

	/* Selects the matching value. */
	if (strcmp(items[1], "SHOW") == 0 && (count == 2 || count == 3)) {
		strcpy(response, NETWORKD_PROTOCOL_VERSION " OK ");

		/* Handles a failed show interfaces operation. */
		if (show_interfaces(count == 3 ? items[2] : NULL,
				    response + strlen(response),
				    sizeof(response) - strlen(response)) == 0) {
			(void)write_all(client, response, strlen(response));

			/* Returns the computed result. */
			return;
		}
		error = errno;
	} else if ((strcmp(items[1], "UP") == 0 ||
		    strcmp(items[1], "DOWN") == 0) &&
		   count == 3) {
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = items[2];
		arguments[2] = strcmp(items[1], "UP") == 0 ? "up" : "down";
		arguments[3] = NULL;

		/* Handles a failed interface exists operation. */
		if (interface_exists(items[2]) == 0)
			result = run_command(arguments, 10, diagnostic);
		error = errno;
	} else if (strcmp(items[1], "STATIC") == 0 && count == 7 &&
		   strcmp(items[3], "ipv4") == 0 &&
		   strcmp(items[5], "netmask") == 0) {
		/* Handles a failed interface exists operation. */
		if (interface_exists(items[2]) == 0 &&
		    netutil_parse_ipv4(items[4], &address) == 0 &&
		    netutil_parse_ipv4(items[6], &mask) == 0 &&
		    netutil_mask_prefix(mask, &prefix) == 0) {
			arguments[0] = "/sbin/ifconfig";
			arguments[1] = items[2];
			arguments[2] = "inet";
			arguments[3] = items[4];
			arguments[4] = "netmask";
			arguments[5] = items[6];
			arguments[6] = NULL;
			result = run_command(arguments, 10, diagnostic);
		}
		error = errno;
	} else if (strcmp(items[1], "DHCP") == 0 && count == 4 &&
		   parse_seconds(items[3], &timeout) == 0) {
		(void)snprintf(seconds, sizeof(seconds), "%u", timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = items[2];
		arguments[4] = NULL;

		/* Handles a failed interface exists operation. */
		if (interface_exists(items[2]) == 0) {
			result =
			    run_command(arguments, timeout + 5U, diagnostic);
		}
		error = errno;
	} else if (strcmp(items[1], "DEFAULTROUTE") == 0 && count == 3) {
		/* Handles a failed netutil parse ipv4 operation. */
		if (netutil_parse_ipv4(items[2], &gateway) == 0 &&
		    (present = default_route_exists()) >= 0) {
			/* Handles the present condition. */
			if (present) {
				result = 0;
			} else {
				arguments[0] = "/sbin/route";
				arguments[1] = "add";
				arguments[2] = "default";
				arguments[3] = items[2];
				arguments[4] = NULL;
				result = run_command(arguments, 10, diagnostic);
			}
		}
		error = errno;
	} else if (strcmp(items[1], "DNS") == 0 && count >= 3) {
		result = write_resolver(&items[2], count - 2);
		error = errno;
	} else if (strcmp(items[1], "RELOAD") == 0 && count == 2) {
		result = 0;
	} else {
		send_error(client, EINVAL, "invalid operation or operands");

		/* Returns the computed result. */
		return;
	}

	/* Checks the operation result. */
	if (result == 0) {
		(void)write_all(client, NETWORKD_PROTOCOL_VERSION " OK\n", 6);

		/* Returns the computed result. */
		return;
	}
	send_error(client, error != 0 ? error : EIO, diagnostic);
}

/* Supports the read request operation. */
static int
read_request(
	int descriptor,
	char buffer[NETWORKD_REQUEST_MAX])
{
	ssize_t count;
	char *newline;
	size_t used;

	/* Continue while the operation condition remains true. */
	used = 0;
	while (used + 1U < NETWORKD_REQUEST_MAX) {
		count = read(descriptor, buffer + used,
				     NETWORKD_REQUEST_MAX - used - 1U);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0)
			return -1;

		/* Checks the remaining item count. */
		if (count == 0)
			break;
		used += (size_t)count;

		/* Handles a failed memchr operation. */
		if (memchr(buffer, '\n', used) != NULL)
			break;
	}

	/* Checks the current capacity usage. */
	if (used == 0 || used + 1U >= NETWORKD_REQUEST_MAX) {
		errno = used == 0 ? EPIPE : EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	buffer[used] = '\0';

	newline = strchr(buffer, '\n');

	/* Handles the newline availability. */
	if (newline == NULL || newline[1] != '\0' ||
	    (newline > buffer && newline[-1] == '\r')) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*newline = '\0';
	/* Reports successful completion. */
	return 0;
}

/* Supports the send error operation. */
static void
send_error(
	int client,
	int error,
	const char *reason)
{
	char response[NETWORKD_RESPONSE_MAX];

	/* Handles the reason availability. */
	if (reason == NULL || *reason == '\0')
		reason = strerror(error);
	(void)snprintf(response, sizeof(response), "%s ERR %d %s\n",
		       NETWORKD_PROTOCOL_VERSION, error, reason);
	(void)write_all(client, response, strlen(response));
}

/* Supports the show interfaces operation. */
static int
show_interfaces(
	const char *name,
	char *output,
	size_t capacity)
{
	struct ifreq *items;
	unsigned count, index;
	size_t used;
	int descriptor, result;

	items = NULL;
	count = 0;
	used = 0;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = 0;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles the name availability. */
	if (name != NULL) {
		result = append_interface_status(descriptor, name, output,
						 capacity, &used);
	} else if (netutil_interfaces(descriptor, &items, &count) != 0)
		result = -1;
	else

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			/* Handles a failed append interface status operation. */
			if (append_interface_status(
				descriptor, items[index].ifr_name, output,
				capacity, &used) != 0) {
				result = -1;
				break;
			}
		}
	free(items);
	close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Supports the append interface status operation. */
static int
append_interface_status(
	int descriptor,
	const char *name,
	char *output,
	const size_t capacity,
	size_t *used)
{
	struct ifreq flags, address;
	int count, has_address;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&flags, name) != 0 ||
	    ioctl(descriptor, SIOCGIFFLAGS, &flags) != 0)

		/* Reports operation failure. */
		return -1;
	has_address =
	    netutil_ifreq(&address, name) == 0 &&
	    ioctl(descriptor, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0;
	count =
	    snprintf(output + *used, capacity - *used, "%s %s %s\n", name,
		     has_address ? "static" : "unconfigured",
		     (flags.ifr_flags & IFF_RUNNING) != 0 ? "online" : "offline");

	/* Checks the remaining item count. */
	if (count < 0 || (size_t)count >= capacity - *used) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	*used += (size_t)count;
	/* Reports successful completion. */
	return 0;
}

/* Supports the interface exists operation. */
static int
interface_exists(
	const char *name)
{
	uint32_t index;
	int descriptor;
	int result;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = descriptor >= 0
		? netutil_ifindex(descriptor, name, &index)
		: -1;

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Supports the run command operation. */
static int
run_command(
	char *const arguments[],
	unsigned timeout_seconds,
	char diagnostic[CHILD_OUTPUT_MAX])
{
	pid_t result;
	ssize_t count;
	char temporary[96];
	int output, status, child_done;
	pid_t child;
	unsigned ticks, tick_limit;

	status = 0;
	child_done = 0;
	ticks = 0;
	tick_limit = timeout_seconds * 100U;

	diagnostic[0] = '\0';

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/run/networkd-child.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	output = open(temporary, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	/* Handles the output condition. */
	if (output < 0)
		return -1;
	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		/* Handles a failed dup2 operation. */
		if (dup2(output, STDOUT_FILENO) < 0 ||
		    dup2(output, STDERR_FILENO) < 0)
			_exit(126);
		close(output);
		execv(arguments[0], arguments);
		_exit(127);
	}

	/* Checks the child process state. */
	if (child < 0) {
		close(output);
		unlink(temporary);

		/* Reports operation failure. */
		return -1;
	}
	while (!child_done) {
		result = waitpid(child, &status, WNOHANG);

		/* Checks the operation result. */
		if (result == child)
			child_done = 1;
		else if (result < 0 && errno != EINTR)
			child_done = 1;

		/* Handles the child done condition. */
		if (child_done)
			break;

		/* Handles the ticks condition. */
		if (ticks++ >= tick_limit) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			errno = ETIMEDOUT;
			break;
		}
		usleep(10000);
	}

	/* Handles a failed lseek operation. */
	if (lseek(output, 0, SEEK_SET) >= 0) {
		count = read(output, diagnostic, CHILD_OUTPUT_MAX - 1U);

		/* Checks the remaining item count. */
		if (count > 0)
			diagnostic[count] = '\0';
	}
	close(output);
	unlink(temporary);
	clean_diagnostic(diagnostic);

	/* Handles the ticks condition. */
	if (ticks > tick_limit)
		return -1;

	/* Handles a failed WIFEXITED operation. */
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ENOENT
									: EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the clean diagnostic operation. */
static void
clean_diagnostic(
	char *text)
{
	size_t index, length;

	/* Process each remaining element. */
	length = strlen(text);
	while (length != 0 &&
	       (text[length - 1U] == '\n' || text[length - 1U] == '\r'))

	/* Process each remaining element. */
		text[--length] = '\0';
	for (index = 0; index < length; index++) {
		/* Validates the current text. */
		if ((unsigned char)text[index] < 32U || text[index] == 127)
			text[index] = ' ';
	}
}

/* Supports the parse seconds operation. */
static int
parse_seconds(
	const char *text,
	unsigned *result)
{
	char *end;
	unsigned long value;

	/* Handles the text availability. */
	if (text == NULL || *text == '\0')
		return -1;
	value = strtoul(text, &end, 10);

	/* Checks the current endpoint. */
	if (*end != '\0' || value < 1U || value > 3600U)
		return -1;
	*result = (unsigned)value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the default route exists operation. */
static int
default_route_exists(
	void)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *mask;
	struct rtentry route;
	int descriptor;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Process each remaining element. */
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		destination = (const struct sockaddr_in *)&route.rt_dst;
		mask = (const struct sockaddr_in *)&route.rt_genmask;

		/* Handles the destination condition. */
		if (destination->sin_addr.s_addr == 0 &&
		    mask->sin_addr.s_addr == 0) {
			close(descriptor);

			/* Reports operation failure. */
			return 1;
		}
	}
	close(descriptor);

	/* Returns the computed result. */
	return errno == ENOENT ? 0 : -1;
}

/* Supports the write resolver operation. */
static int
write_resolver(
	char *const addresses[],
	int count)
{
	int length;
	struct in_addr parsed;
	char temporary[128], line[64];
	int descriptor, index, prior;

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed write all operation. */
	if (write_all(descriptor, "# Generated by networkd\n", 24) != 0)
		goto fail;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles a failed netutil parse ipv4 operation. */
		if (netutil_parse_ipv4(addresses[index], &parsed) != 0)
			goto fail;

		/* Process each remaining element. */
		for (prior = 0; prior < index; prior++) {
			/* Selects the matching value. */
			if (strcmp(addresses[prior], addresses[index]) == 0)
				break;
		}

		/* Handles the prior condition. */
		if (prior != index)
			continue;
		length = snprintf(line, sizeof(line), "nameserver %s\n",
	     addresses[index]);

		/* Handles a failed write all operation. */
		if (length < 0 || (size_t)length >= sizeof(line) ||
		    write_all(descriptor, line, (size_t)length) != 0)
			goto fail;
	}

	/* Handles a failed fsync operation. */
	if (fsync(descriptor) != 0 || close(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	descriptor = -1;

	/* Handles a failed rename operation. */
	if (rename(temporary, "/etc/resolv.conf") != 0)
		goto fail;

	/* Reports successful completion. */
	return 0;

fail:

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);
	unlink(temporary);

	/* Reports operation failure. */
	return -1;
}

/* Supports the handle signal operation. */
static void
handle_signal(
	int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

/* Supports the ignore signal operation. */
static void
ignore_signal(
	int signal_number)
{
	(void)signal_number;
}
