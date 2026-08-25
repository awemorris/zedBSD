/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void
handle_signal(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static void
ignore_signal(int signal_number)
{
	(void)signal_number;
}

static int
write_all(int descriptor, const char *buffer, size_t length)
{
	size_t offset = 0;
	while (offset < length) {
		ssize_t count =
		    write(descriptor, buffer + offset, length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}
	return 0;
}

static void
notify_init(const char *record)
{
	const char *value = getenv("ZEDBSD_NOTIFY_FD");
	if (value == NULL || strcmp(value, "3") != 0)
		return;
	(void)write_all(3, record, strlen(record));
	(void)close(3);
}

static int
open_listener(void)
{
	struct sockaddr_un address;
	int descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	(void)unlink(NETWORKD_SOCKET);
	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    chmod(NETWORKD_SOCKET, 0600) != 0 || listen(descriptor, 8) != 0) {
		int saved = errno;
		close(descriptor);
		errno = saved;
		return -1;
	}
	return descriptor;
}

static int
read_request(int descriptor, char buffer[NETWORKD_REQUEST_MAX])
{
	size_t used = 0;
	while (used + 1U < NETWORKD_REQUEST_MAX) {
		ssize_t count = read(descriptor, buffer + used,
				     NETWORKD_REQUEST_MAX - used - 1U);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0)
			return -1;
		if (count == 0)
			break;
		used += (size_t)count;
		if (memchr(buffer, '\n', used) != NULL)
			break;
	}
	if (used == 0 || used + 1U >= NETWORKD_REQUEST_MAX) {
		errno = used == 0 ? EPIPE : EOVERFLOW;
		return -1;
	}
	buffer[used] = '\0';
	{
		char *newline = strchr(buffer, '\n');
		if (newline == NULL || newline[1] != '\0' ||
		    (newline > buffer && newline[-1] == '\r')) {
			errno = EINVAL;
			return -1;
		}
		*newline = '\0';
	}
	return 0;
}

static void
clean_diagnostic(char *text)
{
	size_t index, length = strlen(text);
	while (length != 0 &&
	       (text[length - 1U] == '\n' || text[length - 1U] == '\r'))
		text[--length] = '\0';
	for (index = 0; index < length; index++)
		if ((unsigned char)text[index] < 32U || text[index] == 127)
			text[index] = ' ';
}

static int
run_command(char *const arguments[], unsigned timeout_seconds,
	    char diagnostic[CHILD_OUTPUT_MAX])
{
	char temporary[96];
	int output, status = 0, child_done = 0;
	pid_t child;
	unsigned ticks = 0, tick_limit = timeout_seconds * 100U;

	diagnostic[0] = '\0';
	if (snprintf(temporary, sizeof(temporary), "/run/networkd-child.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;
		return -1;
	}
	output = open(temporary, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (output < 0)
		return -1;
	child = fork();
	if (child == 0) {
		if (dup2(output, STDOUT_FILENO) < 0 ||
		    dup2(output, STDERR_FILENO) < 0)
			_exit(126);
		close(output);
		execv(arguments[0], arguments);
		_exit(127);
	}
	if (child < 0) {
		close(output);
		unlink(temporary);
		return -1;
	}
	while (!child_done) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			child_done = 1;
		else if (result < 0 && errno != EINTR)
			child_done = 1;
		if (child_done)
			break;
		if (ticks++ >= tick_limit) {
			(void)kill(child, SIGKILL);
			(void)waitpid(child, &status, 0);
			errno = ETIMEDOUT;
			break;
		}
		usleep(10000);
	}
	if (lseek(output, 0, SEEK_SET) >= 0) {
		ssize_t count = read(output, diagnostic, CHILD_OUTPUT_MAX - 1U);
		if (count > 0)
			diagnostic[count] = '\0';
	}
	close(output);
	unlink(temporary);
	clean_diagnostic(diagnostic);
	if (ticks > tick_limit)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ENOENT
									: EIO;
		return -1;
	}
	return 0;
}

static int
interface_exists(const char *name)
{
	uint32_t index;
	int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int result =
	    descriptor >= 0 ? netutil_ifindex(descriptor, name, &index) : -1;
	if (descriptor >= 0)
		close(descriptor);
	return result;
}

static int
append_interface_status(int descriptor, const char *name, char *output,
			const size_t capacity, size_t *used)
{
	struct ifreq flags, address;
	int count, has_address;
	if (netutil_ifreq(&flags, name) != 0 ||
	    ioctl(descriptor, SIOCGIFFLAGS, &flags) != 0)
		return -1;
	has_address =
	    netutil_ifreq(&address, name) == 0 &&
	    ioctl(descriptor, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0;
	count =
	    snprintf(output + *used, capacity - *used, "%s %s %s\n", name,
		     has_address ? "static" : "unconfigured",
		     (flags.ifr_flags & IFF_UP) != 0 ? "online" : "offline");
	if (count < 0 || (size_t)count >= capacity - *used) {
		errno = EOVERFLOW;
		return -1;
	}
	*used += (size_t)count;
	return 0;
}

static int
show_interfaces(const char *name, char *output, size_t capacity)
{
	struct ifreq *items = NULL;
	unsigned count = 0, index;
	size_t used = 0;
	int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), result = 0;
	if (descriptor < 0)
		return -1;
	if (name != NULL)
		result = append_interface_status(descriptor, name, output,
						 capacity, &used);
	else if (netutil_interfaces(descriptor, &items, &count) != 0)
		result = -1;
	else
		for (index = 0; index < count; index++)
			if (append_interface_status(
				descriptor, items[index].ifr_name, output,
				capacity, &used) != 0) {
				result = -1;
				break;
			}
	free(items);
	close(descriptor);
	return result;
}

static int
default_route_exists(void)
{
	struct rtentry route;
	int descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		const struct sockaddr_in *destination =
		    (const struct sockaddr_in *)&route.rt_dst;
		const struct sockaddr_in *mask =
		    (const struct sockaddr_in *)&route.rt_genmask;
		if (destination->sin_addr.s_addr == 0 &&
		    mask->sin_addr.s_addr == 0) {
			close(descriptor);
			return 1;
		}
	}
	close(descriptor);
	return errno == ENOENT ? 0 : -1;
}

static int
write_resolver(char *const addresses[], int count)
{
	char temporary[128], line[64];
	int descriptor, index, prior;
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;
		return -1;
	}
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (descriptor < 0)
		return -1;
	if (write_all(descriptor, "# Generated by networkd\n", 24) != 0)
		goto fail;
	for (index = 0; index < count; index++) {
		struct in_addr parsed;
		if (netutil_parse_ipv4(addresses[index], &parsed) != 0)
			goto fail;
		for (prior = 0; prior < index; prior++)
			if (strcmp(addresses[prior], addresses[index]) == 0)
				break;
		if (prior != index)
			continue;
		{
			int length =
			    snprintf(line, sizeof(line), "nameserver %s\n",
				     addresses[index]);
			if (length < 0 || (size_t)length >= sizeof(line) ||
			    write_all(descriptor, line, (size_t)length) != 0)
				goto fail;
		}
	}
	if (fsync(descriptor) != 0 || close(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	descriptor = -1;
	if (rename(temporary, "/etc/resolv.conf") != 0)
		goto fail;
	return 0;

fail:
	if (descriptor >= 0)
		close(descriptor);
	unlink(temporary);
	return -1;
}

static int
parse_seconds(const char *text, unsigned *result)
{
	char *end;
	unsigned long value;
	if (text == NULL || *text == '\0')
		return -1;
	value = strtoul(text, &end, 10);
	if (*end != '\0' || value < 1U || value > 3600U)
		return -1;
	*result = (unsigned)value;
	return 0;
}

static void
send_error(int client, int error, const char *reason)
{
	char response[NETWORKD_RESPONSE_MAX];
	if (reason == NULL || *reason == '\0')
		reason = strerror(error);
	(void)snprintf(response, sizeof(response), "%s ERR %d %s\n",
		       NETWORKD_PROTOCOL_VERSION, error, reason);
	(void)write_all(client, response, strlen(response));
}

static void
handle_request(int client)
{
	char request[NETWORKD_REQUEST_MAX], response[NETWORKD_RESPONSE_MAX];
	char diagnostic[CHILD_OUTPUT_MAX], *arguments[16], *token;
	char *items[16];
	int count = 0, result = -1, error = EINVAL;
	unsigned timeout = 10;

	if (read_request(client, request) != 0) {
		send_error(client, errno, "malformed request");
		return;
	}
	for (token = strtok(request, " \t"); token != NULL;
	     token = strtok(NULL, " \t")) {
		if (count == (int)(sizeof(items) / sizeof(items[0]))) {
			send_error(client, E2BIG, "too many operands");
			return;
		}
		items[count++] = token;
	}
	if (count < 2 || strcmp(items[0], NETWORKD_PROTOCOL_VERSION) != 0) {
		send_error(client, EINVAL, "unsupported protocol");
		return;
	}
	diagnostic[0] = '\0';
	if (strcmp(items[1], "SHOW") == 0 && (count == 2 || count == 3)) {
		strcpy(response, NETWORKD_PROTOCOL_VERSION " OK ");
		if (show_interfaces(count == 3 ? items[2] : NULL,
				    response + strlen(response),
				    sizeof(response) - strlen(response)) == 0) {
			(void)write_all(client, response, strlen(response));
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
		if (interface_exists(items[2]) == 0)
			result = run_command(arguments, 10, diagnostic);
		error = errno;
	} else if (strcmp(items[1], "STATIC") == 0 && count == 7 &&
		   strcmp(items[3], "ipv4") == 0 &&
		   strcmp(items[5], "netmask") == 0) {
		struct in_addr address, mask;
		unsigned prefix;
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
		char seconds[16];
		(void)snprintf(seconds, sizeof(seconds), "%u", timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = items[2];
		arguments[4] = NULL;
		if (interface_exists(items[2]) == 0)
			result =
			    run_command(arguments, timeout + 5U, diagnostic);
		error = errno;
	} else if (strcmp(items[1], "DEFAULTROUTE") == 0 && count == 3) {
		struct in_addr gateway;
		int present;
		if (netutil_parse_ipv4(items[2], &gateway) == 0 &&
		    (present = default_route_exists()) >= 0) {
			if (present)
				result = 0;
			else {
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
		return;
	}
	if (result == 0) {
		(void)write_all(client, NETWORKD_PROTOCOL_VERSION " OK\n", 6);
		return;
	}
	send_error(client, error != 0 ? error : EIO, diagnostic);
}

int
main(void)
{
	int listener;
	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	listener = open_listener();
	if (listener < 0) {
		char record[256];
		int saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s\n", saved,
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket: %s\n",
			strerror(saved));
		return 1;
	}
	notify_init("READY\n");
	while (!stopping) {
		int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (client >= 0) {
			handle_request(client);
			close(client);
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		sleep(1);
	}
	close(listener);
	unlink(NETWORKD_SOCKET);
	return stopping ? 0 : 1;
}
