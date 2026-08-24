/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG_SOCKET "/run/log"
#define LOG_PATH "/var/log/messages"
#define BOOT_LOG_PATH "/run/dmesg.boot"

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t reopening;

static void
handle_signal(int number)
{
	if (number == SIGHUP)
		reopening = 1;
	else
		stopping = 1;
}

static int
write_all(int descriptor, const void *buffer, size_t length)
{
	const char *cursor = buffer;
	while (length != 0) {
		ssize_t written = write(descriptor, cursor, length);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		cursor += written;
		length -= (size_t)written;
	}
	return 0;
}

static void
save_boot_log(void)
{
	char buffer[65536];
	size_t length = sizeof(buffer);
	int descriptor;
	if (sysctlbyname("kern.msgbuf", buffer, &length, NULL, 0) != 0)
		return;
	descriptor = open(BOOT_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (descriptor >= 0) {
		(void)write_all(descriptor, buffer, length);
		(void)fsync(descriptor);
		(void)close(descriptor);
	}
}

static int
open_output(void)
{
	return open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);
}

int
main(int argc, char **argv)
{
	struct sockaddr_un address;
	char message[2048];
	int socket_descriptor, output;
	(void)argv;

	if (argc != 1) {
		fprintf(stderr, "usage: syslogd\n");
		return 2;
	}
	(void)signal(SIGHUP, handle_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	save_boot_log();
	output = open_output();
	if (output < 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_PATH, strerror(errno));
		return 1;
	}
	socket_descriptor = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (socket_descriptor < 0)
		return 1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, LOG_SOCKET);
	(void)unlink(LOG_SOCKET);
	if (bind(socket_descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0 ||
	    chmod(LOG_SOCKET, 0666) != 0) {
		fprintf(stderr, "syslogd: %s: %s\n", LOG_SOCKET,
			strerror(errno));
		return 1;
	}
	while (!stopping) {
		ssize_t length;
		if (reopening) {
			int replacement = open_output();
			reopening = 0;
			if (replacement >= 0) {
				close(output);
				output = replacement;
			}
		}
		length =
		    recv(socket_descriptor, message, sizeof(message) - 1, 0);
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0)
			break;
		message[length] = '\0';
		if (memchr(message, '\0', (size_t)length) != NULL)
			continue;
		if (write_all(output, message, (size_t)length) != 0 ||
		    write_all(output, "\n", 1) != 0)
			fprintf(stderr, "syslogd: log write failed: %s\n",
				strerror(errno));
	}
	(void)fsync(output);
	close(output);
	close(socket_descriptor);
	unlink(LOG_SOCKET);
	return 0;
}
