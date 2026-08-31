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
#include <grp.h>
#include <limits.h>
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
#define NETWORKD_AUTH_LOG_MAX 512U
#define NETWORKD_GROUP_DATABASE_MAX 8192U
#define NETWORKD_GROUP_BUFFER_MAX 2048U
#define NETWORKD_GROUP_FILE "/etc/group"
#define NETWORKD_GROUP_GID ((gid_t)69)
#define NETWORKD_GROUP_NAME "network"

enum networkd_client_role {
	NETWORKD_CLIENT_ROOT,
	NETWORKD_CLIENT_READ_ONLY
};

struct networkd_listener {
	int descriptor;
	dev_t device;
	ino_t inode;
	int owns_path;
	const char *stage;
};

struct networkd_group_scan {
	unsigned network_records;
	unsigned gid_records;
};

static volatile sig_atomic_t stopping;

static int scan_group_record(char *line, struct networkd_group_scan *scan);
static int validate_network_group_database(void);
static int resolve_network_group(gid_t *result);
static int listener_path_matches(const struct networkd_listener *listener);
static int close_listener(struct networkd_listener *listener);
static int remove_stale_listener(void);
static int open_listener(struct networkd_listener *listener);
static void notify_init(const char *record);
static int write_all(int descriptor, const char *buffer, size_t length);
static void write_auth_log(const struct zedbsd_peercred *peer,
			   enum networkd_client_role role, int error);
static int authenticate_client(int client, struct zedbsd_peercred *peer,
			       enum networkd_client_role *role);
static int operation_allowed(enum networkd_client_role role,
			     const char *operation);
static void handle_request(int client, enum networkd_client_role role);
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
	int status;
	struct zedbsd_peercred peer;
	enum networkd_client_role role;
	struct networkd_listener listener;

	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	status = 0;

	/* Creates and verifies the privileged network control endpoint. */
	if (open_listener(&listener) != 0) {
		saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s %s\n", saved,
			       listener.stage != NULL ? listener.stage : "unknown",
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket %s: %s\n",
			listener.stage != NULL ? listener.stage : "unknown",
			strerror(saved));

		/* Reports operation failure. */
		return 1;
	}
	notify_init("READY\n");

	/* Continue while the operation condition remains true. */
	while (!stopping) {
		client = accept4(listener.descriptor, NULL, NULL, SOCK_CLOEXEC);

		/* Handles the client condition. */
		if (client >= 0) {
			/* Handles a failed authenticate client operation. */
			if (authenticate_client(client, &peer, &role) == 0)
				handle_request(client, role);
			else
				send_error(client, EACCES,
				    "authentication failed");
			close(client);
			continue;
		}

		/* Handles the reported system error. */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		sleep(1);
	}
	status = stopping ? 0 : 1;

	/* Removes only the socket pathname created by this daemon instance. */
	if (close_listener(&listener) != 0) {
		fprintf(stderr, "networkd: control socket cleanup: %s\n",
			strerror(errno));
		status = 1;
	}

	/* Returns the computed result. */
	return status;
}

/* Validates one record from the trusted network group database. */
static int
scan_group_record(
	char *line,
	struct networkd_group_scan *scan)
{
	char *field[4];
	char *cursor;
	char *end;
	unsigned long gid;

	/* Handles the line availability. */
	if (line == NULL || scan == NULL || line[0] == '\0' || line[0] == '#')
		return line != NULL && scan != NULL ? 0 : EINVAL;
	field[0] = line;
	cursor = strchr(field[0], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[1] = cursor + 1;
	cursor = strchr(field[1], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[2] = cursor + 1;
	cursor = strchr(field[2], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[3] = cursor + 1;

	/* Handles a failed strchr operation. */
	if (strchr(field[3], ':') != NULL || field[0][0] == '\0' ||
	    field[2][0] == '\0')

		/* Returns the computed result. */
		return EINVAL;
	for (cursor = field[0]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if ((unsigned char)*cursor <= 32U ||
		    (unsigned char)*cursor == 127U)

			/* Returns the computed result. */
			return EINVAL;
	}
	for (cursor = field[2]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return EINVAL;
	}
	errno = 0;
	gid = strtoul(field[2], &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || end == field[2] || *end != '\0' || gid > UINT_MAX)
		return EINVAL;

	/* Selects the matching value. */
	if (strcmp(field[0], NETWORKD_GROUP_NAME) == 0)
		scan->network_records++;

	/* Handles the gid condition. */
	if (gid == (unsigned long)NETWORKD_GROUP_GID)
		scan->gid_records++;

	/* Reports successful completion. */
	return 0;
}

/* Ensures that name and numeric identity each have one unambiguous record. */
static int
validate_network_group_database(
	void)
{
	char database[NETWORKD_GROUP_DATABASE_MAX + 1U];
	struct networkd_group_scan scan;
	char *line;
	char *newline;
	char extra;
	size_t used;
	size_t start;
	size_t length;
	ssize_t count;
	int descriptor;
	int error;

	used = 0;
	error = 0;
	descriptor = open(NETWORKD_GROUP_FILE, O_RDONLY | O_CLOEXEC);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	while (used < NETWORKD_GROUP_DATABASE_MAX) {
		count = read(descriptor, database + used,
			     NETWORKD_GROUP_DATABASE_MAX - used);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0) {
			error = errno;
			break;
		}

		/* Checks the remaining item count. */
		if (count == 0)
			break;
		used += (size_t)count;
	}

	/* Handles an operation failure. */
	if (error == 0 && used == NETWORKD_GROUP_DATABASE_MAX) {
		do {
			count = read(descriptor, &extra, 1U);
		} while (count < 0 && errno == EINTR);

		/* Checks the remaining item count. */
		if (count != 0)
			error = count < 0 ? errno : E2BIG;
	}

	/* Handles an operation failure. */
	if (close(descriptor) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed memchr operation. */
	if (memchr(database, '\0', used) != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	database[used] = '\0';
	memset(&scan, 0, sizeof(scan));
	for (start = 0; start < used;) {
		line = database + start;
		newline = memchr(line, '\n', used - start);
		length = newline != NULL ? (size_t)(newline - line)
					 : used - start;
		line[length] = '\0';

		/* Checks the current data length. */
		if (length != 0 && line[length - 1U] == '\r')
			line[length - 1U] = '\0';

		/* Handles a failed scan group record operation. */
		if (scan_group_record(line, &scan) != 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		start += length + (newline != NULL ? 1U : 0U);
	}

	/* Handles the scan condition. */
	if (scan.network_records != 1U || scan.gid_records != 1U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Resolves the fixed network operator group without accepting ambiguity. */
static int
resolve_network_group(
	gid_t *result)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	struct group storage;
	struct group *entry;
	int error;

	entry = NULL;

	/* Handles the result availability. */
	if (result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	error = getgrnam_r(NETWORKD_GROUP_NAME, &storage, buffer,
			   sizeof(buffer), &entry);

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the entry availability. */
	if (entry == NULL) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the gr name availability. */
	if (entry->gr_name == NULL ||
	    strcmp(entry->gr_name, NETWORKD_GROUP_NAME) != 0 ||
	    entry->gr_gid != NETWORKD_GROUP_GID) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed validate network group database operation. */
	if (validate_network_group_database() != 0)
		return -1;
	*result = entry->gr_gid;
	/* Reports successful completion. */
	return 0;
}

/* Checks whether the public pathname still names this listener instance. */
static int
listener_path_matches(
	const struct networkd_listener *listener)
{
	int function_result;
	struct stat status;

	/* Handles the listener availability. */
	if (listener == NULL || !listener->owns_path)
		return 0;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return 0;

	/* Computes the function result. */
	function_result = S_ISSOCK(status.st_mode) && status.st_dev == listener->device &&
	       status.st_ino == listener->inode;

	/* Returns the computed result. */
	return function_result;
}

/* Closes a listener without unlinking a pathname replaced by another owner. */
static int
close_listener(
	struct networkd_listener *listener)
{
	struct stat status;
	int error;

	error = 0;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed close operation. */
	if (listener->descriptor >= 0 && close(listener->descriptor) != 0)
		error = errno;
	listener->descriptor = -1;

	/* Handles the listener condition. */
	if (listener->owns_path) {
		/* Handles the listener path matches condition. */
		if (listener_path_matches(listener)) {
			/* Handles an operation failure. */
			if (unlink(NETWORKD_SOCKET) != 0 && error == 0)
				error = errno;
		} else if (lstat(NETWORKD_SOCKET, &status) == 0 && error == 0) {
			/*
			 * A replacement endpoint belongs to another daemon
			 * instance and must never be removed here.
			 */
			error = EBUSY;
		}
	}
	listener->owns_path = 0;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Removes a stale socket, but refuses to replace a non-socket object. */
static int
remove_stale_listener(
	void)
{
	int function_result;
	struct stat status;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return errno == ENOENT ? 0 : -1;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EEXIST;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the unlink result. */
	function_result = unlink(NETWORKD_SOCKET);

	/* Returns the computed result. */
	return function_result;
}

/* Creates the group-accessible network control listener. */
static int
open_listener(
	struct networkd_listener *listener)
{
	struct sockaddr_un address;
	struct stat status;
	mode_t old_mask;
	gid_t group;
	int saved;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(listener, 0, sizeof(*listener));
	listener->descriptor = -1;
	listener->stage = "resolve-group";

	/* Handles a failed resolve network group operation. */
	if (resolve_network_group(&group) != 0)
		return -1;
	listener->stage = "remove-stale";

	/* Handles a failed remove stale listener operation. */
	if (remove_stale_listener() != 0)
		return -1;
	listener->stage = "socket";
	listener->descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Handles the listener condition. */
	if (listener->descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	listener->stage = "bind";
	old_mask = umask(0177);

	/* Handles a failed bind operation. */
	if (bind(listener->descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0) {
		saved = errno;
		(void)umask(old_mask);
		errno = saved;
		goto fail;
	}
	(void)umask(old_mask);
	listener->stage = "identify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EINVAL;
		goto fail;
	}
	listener->device = status.st_dev;
	listener->inode = status.st_ino;
	listener->owns_path = 1;
	listener->stage = "owner";

	/* Handles a failed lchown operation. */
	if (lchown(NETWORKD_SOCKET, 0, group) != 0)
		goto fail;
	listener->stage = "mode";

	/* Handles a failed chmod operation. */
	if (chmod(NETWORKD_SOCKET, 0660) != 0)
		goto fail;
	listener->stage = "verify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode) || status.st_dev != listener->device ||
	    status.st_ino != listener->inode || status.st_uid != 0 ||
	    status.st_gid != group || (status.st_mode & 07777U) != 0660U) {
		errno = EINVAL;
		goto fail;
	}
	listener->stage = "listen";

	/* Handles a failed listen operation. */
	if (listen(listener->descriptor, 8) != 0)
		goto fail;
	listener->stage = "ready";

	/* Reports successful completion. */
	return 0;

fail:
	saved = errno != 0 ? errno : EIO;
	(void)close_listener(listener);
	errno = saved;

	/* Reports operation failure. */
	return -1;
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

/* Records the peer credential decision without exposing request contents. */
static void
write_auth_log(
	const struct zedbsd_peercred *peer,
	enum networkd_client_role role,
	int error)
{
	const char fallback[] =
	    "networkd: auth result=denied reason=record-overflow\n";
	char record[NETWORKD_AUTH_LOG_MAX];
	int length;

	/* Handles an operation failure. */
	if (error != 0 || peer == NULL) {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=denied reason=peercred error=%d",
		    error != 0 ? error : EACCES);
	} else {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=admitted role=%s pid=%ld euid=%lu egid=%lu",
		    role == NETWORKD_CLIENT_ROOT ? "root-all" : "nonroot-show",
		    (long)peer->pid, (unsigned long)peer->euid,
		    (unsigned long)peer->egid);
	}

	/* Checks the current data length. */
	if (length < 0 || (size_t)length > sizeof(record) - 2U) {
		(void)fputs(fallback, stderr);

		/* Returns the computed result. */
		return;
	}
	record[length++] = '\n';
	record[length] = '\0';
	(void)fputs(record, stderr);
}

/* Obtains immutable credentials for the connected Unix-domain peer. */
static int
authenticate_client(
	int client,
	struct zedbsd_peercred *peer,
	enum networkd_client_role *role)
{
	socklen_t length;
	int error;

	/* Handles the peer availability. */
	if (peer == NULL || role == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(peer, 0, sizeof(*peer));
	length = sizeof(*peer);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, peer, &length) != 0) {
		error = errno != 0 ? errno : EACCES;
		write_auth_log(NULL, NETWORKD_CLIENT_READ_ONLY, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current data length. */
	if (length != sizeof(*peer) || peer->pid < 0) {
		error = EINVAL;
		write_auth_log(NULL, NETWORKD_CLIENT_READ_ONLY, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}
	*role = peer->euid == 0 ? NETWORKD_CLIENT_ROOT
				 : NETWORKD_CLIENT_READ_ONLY;
	write_auth_log(peer, *role, 0);

	/* Reports successful completion. */
	return 0;
}

/* Restricts non-root clients to read-only state inspection. */
static int
operation_allowed(
	enum networkd_client_role role,
	const char *operation)
{
	int function_result;

	/* Handles the role condition. */
	if (role == NETWORKD_CLIENT_ROOT)
		return 1;

	/* Computes the function result. */
	function_result = operation != NULL && strcmp(operation, "SHOW") == 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the handle request operation. */
static void
handle_request(
	int client,
	enum networkd_client_role role)
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

	/* Applies authorization before dispatching or parsing operands. */
	if (!operation_allowed(role, items[1])) {
		send_error(client, EPERM, "operation not permitted");

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
	int standard_output, standard_error;
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
		/* Close every inherited or duplicated descriptor on failure. */
		standard_output = dup2(output, STDOUT_FILENO);

		/* Handles the standard output condition. */
		if (standard_output < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		standard_error = dup2(output, STDERR_FILENO);

		/* Handles an operation failure. */
		if (standard_error < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		close(output);
		execv(arguments[0], arguments);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
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
