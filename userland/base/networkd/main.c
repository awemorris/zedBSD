/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static volatile sig_atomic_t stopping;

struct networkd_group_scan {
	unsigned network_records;
	unsigned gid_records;
};

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
scan_group_record(char *line, struct networkd_group_scan *scan)
{
	char *field[4], *cursor, *end;
	unsigned long gid;

	if (line == NULL || scan == NULL || line[0] == '\0' || line[0] == '#')
		return line != NULL && scan != NULL ? 0 : EINVAL;
	field[0] = line;
	cursor = strchr(field[0], ':');
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[1] = cursor + 1;
	cursor = strchr(field[1], ':');
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[2] = cursor + 1;
	cursor = strchr(field[2], ':');
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[3] = cursor + 1;
	if (strchr(field[3], ':') != NULL || field[0][0] == '\0' ||
	    field[2][0] == '\0')
		return EINVAL;
	for (cursor = field[0]; *cursor != '\0'; cursor++)
		if ((unsigned char)*cursor <= 32U ||
		    (unsigned char)*cursor == 127U)
			return EINVAL;
	for (cursor = field[2]; *cursor != '\0'; cursor++)
		if (*cursor < '0' || *cursor > '9')
			return EINVAL;
	errno = 0;
	gid = strtoul(field[2], &end, 10);
	if (errno != 0 || end == field[2] || *end != '\0' || gid > UINT_MAX)
		return EINVAL;
	if (strcmp(field[0], NETWORKD_GROUP_NAME) == 0)
		scan->network_records++;
	if (gid == (unsigned long)NETWORKD_GROUP_GID)
		scan->gid_records++;
	return 0;
}

static int
validate_network_group_database(void)
{
	char database[NETWORKD_GROUP_DATABASE_MAX + 1U];
	struct networkd_group_scan scan;
	size_t used = 0, start;
	ssize_t count;
	int descriptor, error = 0;

	descriptor = open(NETWORKD_GROUP_FILE, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return -1;
	while (used < NETWORKD_GROUP_DATABASE_MAX) {
		count = read(descriptor, database + used,
			     NETWORKD_GROUP_DATABASE_MAX - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			break;
		}
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (error == 0 && used == NETWORKD_GROUP_DATABASE_MAX) {
		char extra;
		do {
			count = read(descriptor, &extra, 1U);
		} while (count < 0 && errno == EINTR);
		if (count != 0)
			error = count < 0 ? errno : E2BIG;
	}
	if (close(descriptor) != 0 && error == 0)
		error = errno;
	if (error != 0) {
		errno = error;
		return -1;
	}
	if (memchr(database, '\0', used) != NULL) {
		errno = EINVAL;
		return -1;
	}
	database[used] = '\0';
	memset(&scan, 0, sizeof(scan));
	for (start = 0; start < used;) {
		char *line = database + start;
		char *newline = memchr(line, '\n', used - start);
		size_t length = newline != NULL ? (size_t)(newline - line)
						: used - start;

		line[length] = '\0';
		if (length != 0 && line[length - 1U] == '\r')
			line[length - 1U] = '\0';
		if (scan_group_record(line, &scan) != 0) {
			errno = EINVAL;
			return -1;
		}
		start += length + (newline != NULL ? 1U : 0U);
	}
	if (scan.network_records != 1U || scan.gid_records != 1U) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int
resolve_network_group(gid_t *result)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	struct group storage, *entry = NULL;
	int error;

	if (result == NULL) {
		errno = EINVAL;
		return -1;
	}
	error = getgrnam_r(NETWORKD_GROUP_NAME, &storage, buffer,
			   sizeof(buffer), &entry);
	if (error != 0) {
		errno = error;
		return -1;
	}
	if (entry == NULL) {
		errno = ENOENT;
		return -1;
	}
	if (entry->gr_name == NULL ||
	    strcmp(entry->gr_name, NETWORKD_GROUP_NAME) != 0 ||
	    entry->gr_gid != NETWORKD_GROUP_GID) {
		errno = EINVAL;
		return -1;
	}
	if (validate_network_group_database() != 0)
		return -1;
	*result = entry->gr_gid;
	return 0;
}

static int
listener_path_matches(const struct networkd_listener *listener)
{
	struct stat status;

	if (listener == NULL || !listener->owns_path)
		return 0;
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return 0;
	return S_ISSOCK(status.st_mode) && status.st_dev == listener->device &&
	       status.st_ino == listener->inode;
}

static int
close_listener(struct networkd_listener *listener)
{
	int error = 0;

	if (listener == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (listener->descriptor >= 0 && close(listener->descriptor) != 0)
		error = errno;
	listener->descriptor = -1;
	if (listener->owns_path) {
		if (listener_path_matches(listener)) {
			if (unlink(NETWORKD_SOCKET) != 0 && error == 0)
				error = errno;
		} else {
			struct stat status;
			/* A removed path needs no cleanup.  A replacement is never
			 * followed or unlinked by this daemon instance. */
			if (lstat(NETWORKD_SOCKET, &status) == 0 && error == 0)
				error = EBUSY;
		}
	}
	listener->owns_path = 0;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

static int
remove_stale_listener(void)
{
	struct stat status;

	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return errno == ENOENT ? 0 : -1;
	if (!S_ISSOCK(status.st_mode)) {
		errno = EEXIST;
		return -1;
	}
	return unlink(NETWORKD_SOCKET);
}

static int
open_listener(struct networkd_listener *listener)
{
	struct sockaddr_un address;
	struct stat status;
	mode_t old_mask;
	gid_t group;
	int saved;

	if (listener == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(listener, 0, sizeof(*listener));
	listener->descriptor = -1;
	listener->stage = "resolve-group";
	if (resolve_network_group(&group) != 0)
		return -1;
	listener->stage = "remove-stale";
	if (remove_stale_listener() != 0)
		return -1;
	listener->stage = "socket";
	listener->descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listener->descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	listener->stage = "bind";
	old_mask = umask(0177);
	if (bind(listener->descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0) {
		saved = errno;
		(void)umask(old_mask);
		errno = saved;
		goto fail;
	}
	(void)umask(old_mask);
	listener->stage = "identify";
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;
	if (!S_ISSOCK(status.st_mode)) {
		errno = EINVAL;
		goto fail;
	}
	listener->device = status.st_dev;
	listener->inode = status.st_ino;
	listener->owns_path = 1;
	listener->stage = "owner";
	if (lchown(NETWORKD_SOCKET, 0, group) != 0)
		goto fail;
	listener->stage = "mode";
	if (chmod(NETWORKD_SOCKET, 0660) != 0)
		goto fail;
	listener->stage = "verify";
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;
	if (!S_ISSOCK(status.st_mode) || status.st_dev != listener->device ||
	    status.st_ino != listener->inode || status.st_uid != 0 ||
	    status.st_gid != group || (status.st_mode & 07777U) != 0660U) {
		errno = EINVAL;
		goto fail;
	}
	listener->stage = "listen";
	if (listen(listener->descriptor, 8) != 0)
		goto fail;
	listener->stage = "ready";
	return 0;

fail:
	saved = errno != 0 ? errno : EIO;
	(void)close_listener(listener);
	errno = saved;
	return -1;
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
		     (flags.ifr_flags & IFF_RUNNING) != 0 ? "online" : "offline");
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
write_auth_log(const struct zedbsd_peercred *peer,
	       enum networkd_client_role role, int error)
{
	char record[NETWORKD_AUTH_LOG_MAX];
	int length;

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
	if (length < 0 || (size_t)length > sizeof(record) - 2U) {
		const char fallback[] =
		    "networkd: auth result=denied reason=record-overflow\n";
		(void)fputs(fallback, stderr);
		return;
	}
	record[length++] = '\n';
	record[length] = '\0';
	(void)fputs(record, stderr);
}

static int
authenticate_client(int client, struct zedbsd_peercred *peer,
		    enum networkd_client_role *role)
{
	socklen_t length;
	int error;

	if (peer == NULL || role == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(peer, 0, sizeof(*peer));
	length = sizeof(*peer);
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, peer, &length) != 0) {
		error = errno != 0 ? errno : EACCES;
		write_auth_log(NULL, NETWORKD_CLIENT_READ_ONLY, error);
		errno = error;
		return -1;
	}
	if (length != sizeof(*peer) || peer->pid < 0) {
		error = EINVAL;
		write_auth_log(NULL, NETWORKD_CLIENT_READ_ONLY, error);
		errno = error;
		return -1;
	}
	*role = peer->euid == 0 ? NETWORKD_CLIENT_ROOT
					 : NETWORKD_CLIENT_READ_ONLY;
	write_auth_log(peer, *role, 0);
	return 0;
}

static int
operation_allowed(enum networkd_client_role role, const char *operation)
{
	if (role == NETWORKD_CLIENT_ROOT)
		return 1;
	return operation != NULL && strcmp(operation, "SHOW") == 0;
}

static void
handle_request(int client, enum networkd_client_role role)
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
	if (!operation_allowed(role, items[1])) {
		send_error(client, EPERM, "operation not permitted");
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
	struct networkd_listener listener;
	int status = 0;
	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	if (open_listener(&listener) != 0) {
		char record[256];
		int saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s %s\n", saved,
			       listener.stage != NULL ? listener.stage : "unknown",
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket %s: %s\n",
			listener.stage != NULL ? listener.stage : "unknown",
			strerror(saved));
		return 1;
	}
	notify_init("READY\n");
	while (!stopping) {
		int client =
		    accept4(listener.descriptor, NULL, NULL, SOCK_CLOEXEC);
		if (client >= 0) {
			struct zedbsd_peercred peer;
			enum networkd_client_role role;

			if (authenticate_client(client, &peer, &role) == 0)
				handle_request(client, role);
			else
				send_error(client, EACCES, "authentication failed");
			close(client);
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		sleep(1);
	}
	status = stopping ? 0 : 1;
	if (close_listener(&listener) != 0) {
		fprintf(stderr, "networkd: control socket cleanup: %s\n",
			strerror(errno));
		status = 1;
	}
	return status;
}
