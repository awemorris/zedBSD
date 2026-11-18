/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sshd program: reads the configuration and the host key, listens, and
 * starts a monitor for every connection (sshd.h).
 *
 *   sshd [-D] [-e] [-t] [-f config]
 *
 * -f names the configuration (SSHD_CONFIG_PATH by default).  -t checks the
 * configuration and the host key and exits: 0 when both are usable, 1 with
 * the error ("line N: ..." for the configuration) on the standard error.
 * -D is accepted for the service manager: sshd always runs in the
 * foreground.  -e copies the log to the standard error.  Started by a
 * monitor with SSHD_EXEC_PREAUTH or SSHD_EXEC_SESSION as its first
 * argument, the program is that child instead.
 *
 * The host key is the first HostKey file that holds an ssh-ed25519
 * private key; a file of another type is passed over with a log line.
 *
 * The listener forks one monitor per accepted connection, and starts the
 * monitor's children from this program's own path (so that a copy outside
 * SSHD_PROGRAM starts copies of itself).  MaxStartups: a monitor whose
 * MaxStartups pipe has not given its byte is a connection not yet
 * authenticated; while MaxStartups of those exist, a new connection is
 * closed at once after accept.  Every descriptor of the listener is
 * close-on-exec.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include "sshd-monitor.h"

/* The configuration read when -f names none. */
#define SSHD_CONFIG_PATH	"/etc/ssh/sshd_config"

/* The queue of connections not yet accepted. */
#define MAIN_BACKLOG		128

/* The most monitors the listener keeps track of; a connection beyond them is closed at once. */
#define MAIN_MONITORS_MAX	1024U

/* The room for a peer's "address port". */
#define MAIN_PEER_MAX		64U

/*
 * One monitor the listener started: its process and the read end of its
 * MaxStartups pipe (-1 once the byte came or the monitor closed it).
 */
struct main_monitor {
	pid_t pid;
	int started;
	int used;
};

/*
 * The monitors running, for MaxStartups and for reaping.  Filled when a
 * connection is accepted, emptied when the monitor is reaped; only the
 * main loop touches it (the signal handlers only write to the wake pipe).
 */
static struct main_monitor main_monitors[MAIN_MONITORS_MAX];

/*
 * The write end of the pipe the signal handlers write one byte to, so
 * that poll wakes; -1 before it is made.  Set once.
 */
static int main_wake_descriptor = -1;

/* Set by SIGTERM, SIGINT or SIGHUP: the listener stops accepting and exits. */
static volatile sig_atomic_t main_stop;

static void main_usage(void);
static int main_read_file(const char *path, char *text, size_t capacity, size_t *size);
static int main_load_config(const char *path, struct sshd_config *config);
static int main_load_host_key(const struct sshd_config *config, struct ssh_key *key, int quiet);
static int main_listen(const struct sshd_config *config);
static void main_on_child(int signal_number);
static void main_on_stop(int signal_number);
static void main_reap(void);
static unsigned main_unauthenticated(void);
static void main_accept(int listener, const struct sshd_context *context, const struct ssh_key *key, const char *program);
static void main_describe_peer(const struct sockaddr_storage *address, char *peer);

/*
 * Runs the sshd program, or the monitor's child it was started as.
 */
int
main(
	int argc,
	char **argv)
{
	static struct sshd_context context;
	static struct ssh_key key;
	struct pollfd waiting[2 + MAIN_MONITORS_MAX];
	struct sigaction action;
	struct ssh_buf blob;
	const char *config_path;
	char *resolved;
	size_t resolved_size;
	uid_t self;
	char program[SSH_CONFIG_PATH_MAX];
	char drain[64];
	unsigned count;
	unsigned index;
	ssize_t got;
	int test_only;
	int to_stderr;
	int option;
	int status;
	int listener;
	int wake[2];
	int ready;

	/* The monitor's children: the program started with their argument. */
	if (argc >= 2) {
		status = strcmp(argv[1], SSHD_EXEC_PREAUTH);
		if (status == 0)
			sshd_preauth_run();
		status = strcmp(argv[1], SSHD_EXEC_SESSION);
		if (status == 0)
			sshd_session_run();
	}

	/* The options. */
	config_path = SSHD_CONFIG_PATH;
	test_only = 0;
	to_stderr = 0;
	for (;;) {
		option = getopt(argc, argv, "Def:t");
		if (option == -1)
			break;
		if (option == 'D') {
			/* Always in the foreground. */
		} else if (option == 'e') {
			to_stderr = 1;
		} else if (option == 'f') {
			config_path = optarg;
		} else if (option == 't') {
			test_only = 1;
		} else {
			main_usage();
			return 1;
		}
	}

	/* Refuses any argument that is not an option. */
	if (optind != argc) {
		main_usage();
		return 1;
	}

	/* The configuration and the host key; -t stops here. */
	status = main_load_config(config_path, &context.config);
	if (status != 0)
		return 1;
	status = main_load_host_key(&context.config, &key, test_only);
	if (status != 0)
		return 1;
	if (test_only) {
		ssh_key_clear(&key);
		return 0;
	}

	/* The log, and the mode: without root only this account is served. */
	if (to_stderr)
		openlog("sshd", LOG_PID | LOG_PERROR, LOG_AUTH);
	else
		openlog("sshd", LOG_PID, LOG_AUTH);
	self = geteuid();
	context.privileged = 0;
	if (self == 0)
		context.privileged = 1;
	else
		syslog(LOG_NOTICE, "not running as root: serving only uid %lu", (unsigned long)self);
	ssh_buf_init(&blob, SSH_KEY_BLOB_SIZE + 8U, 0);
	ssh_key_put_public_blob(key.public_key, &blob);
	memcpy(context.host_key_blob, blob.data, SSH_KEY_BLOB_SIZE);
	ssh_buf_free(&blob);

	/* The children are started from this program's own path when it can be told. */
	strcpy(program, SSHD_PROGRAM);
	resolved = realpath(argv[0], NULL);
	resolved_size = sizeof(program);
	if (resolved != NULL)
		resolved_size = strlen(resolved);
	if (resolved != NULL && resolved_size < sizeof(program))
		strcpy(program, resolved);
	free(resolved);

	/* Listens. */
	listener = main_listen(&context.config);
	if (listener < 0) {
		ssh_key_clear(&key);
		return 1;
	}

	/* Wakes poll on SIGCHLD and on the signals that stop the listener. */
	status = pipe(wake);
	if (status != 0)
		return 1;
	(void)fcntl(wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(wake[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(wake[1], F_SETFL, O_NONBLOCK);
	main_wake_descriptor = wake[1];
	memset(&action, 0, sizeof(action));
	(void)sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	action.sa_handler = main_on_child;
	(void)sigaction(SIGCHLD, &action, NULL);
	action.sa_handler = main_on_stop;
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGHUP, &action, NULL);
	(void)signal(SIGPIPE, SIG_IGN);
	syslog(LOG_INFO, "listening on %.63s port %u", context.config.listen_address, (unsigned)context.config.port);

	/* Accepts connections until stopped. */
	while (!main_stop) {
		main_reap();

		/* Waits for a connection, a child's end, or a monitor's grant. */
		count = 0;
		waiting[count].fd = listener;
		waiting[count].events = POLLIN;
		waiting[count].revents = 0;
		count++;
		waiting[count].fd = wake[0];
		waiting[count].events = POLLIN;
		waiting[count].revents = 0;
		count++;
		for (index = 0; index < MAIN_MONITORS_MAX; index++) {
			if (!main_monitors[index].used || main_monitors[index].started < 0)
				continue;
			waiting[count].fd = main_monitors[index].started;
			waiting[count].events = POLLIN;
			waiting[count].revents = 0;
			count++;
		}

		/* Waits until a descriptor is ready. */
		ready = poll(waiting, (nfds_t)count, -1);
		if (ready < 0)
			continue;

		/* Empties the wake pipe; the children are reaped at the top. */
		if (waiting[1].revents != 0) {
			do {
				got = read(wake[0], drain, sizeof(drain));
			} while (got > 0);
		}

		/* A monitor's byte (or its end): that connection no longer counts as one not authenticated. */
		for (index = 0; index < MAIN_MONITORS_MAX; index++) {
			if (!main_monitors[index].used || main_monitors[index].started < 0)
				continue;
			waiting[0].fd = main_monitors[index].started;
			waiting[0].events = POLLIN;
			waiting[0].revents = 0;
			ready = poll(waiting, 1, 0);
			if (ready <= 0)
				continue;
			got = read(main_monitors[index].started, drain, 1);
			(void)got;
			(void)close(main_monitors[index].started);
			main_monitors[index].started = -1;
		}

		/* A connection. */
		main_accept(listener, &context, &key, program);
	}

	/* Stops: the monitors run on with their connections. */
	(void)close(listener);
	ssh_key_clear(&key);
	syslog(LOG_INFO, "stopped");
	return 0;
}

/* Prints the usage to the standard error. */
static void
main_usage(void)
{
	/* The one form. */
	fputs("usage: sshd [-D] [-e] [-t] [-f config]\n", stderr);
}

/* Reads a whole file of at most capacity - 1 bytes into text; 0, an errno value, or EMSGSIZE when larger. */
static int
main_read_file(
	const char *path,
	char *text,
	size_t capacity,
	size_t *size)
{
	ssize_t got;
	size_t total;
	int descriptor;
	int saved;

	/* Opens it. */
	*size = 0;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		saved = errno;
		return saved;
	}

	/* Reads up to one byte more than may be held, so a larger file is seen. */
	total = 0;
	for (;;) {
		got = read(descriptor, text + total, capacity - total);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			(void)close(descriptor);
			return EIO;
		}

		/* Ends the loop at the end of the file. */
		if (got == 0)
			break;
		total += (size_t)got;
		if (total >= capacity) {
			(void)close(descriptor);
			return EMSGSIZE;
		}
	}

	/* Closes the file and ends the text. */
	(void)close(descriptor);
	text[total] = '\0';
	*size = total;

	/* Succeeded: text holds the file. */
	return 0;
}

/*
 * Reads and checks the configuration at path; an error is printed with
 * its line ("sshd: <path>: line N: ...").  Returns 0 or 1.
 */
static int
main_load_config(
	const char *path,
	struct sshd_config *config)
{
	static char text[SSH_CONFIG_FILE_MAX + 1U];
	char error_text[SSH_CONFIG_ERROR_MAX];
	size_t size;
	int status;

	/* Reads the file. */
	status = main_read_file(path, text, sizeof(text), &size);
	if (status != 0) {
		fprintf(stderr, "sshd: %s: %s\n", path, strerror(status));
		return 1;
	}

	/* Reads the settings over the defaults. */
	sshd_config_defaults(config);
	error_text[0] = '\0';
	status = sshd_config_parse(config, text, size, error_text);
	if (status != 0) {
		fprintf(stderr, "sshd: %s: %s\n", path, error_text);
		return 1;
	}

	/* Releases what the parser kept. */
	(void)sshd_config_finish(config);

	/* Succeeded: the settings are read. */
	return 0;
}

/*
 * Loads the first HostKey file that holds an ssh-ed25519 private key into
 * key; another type is passed over (a log line, or nothing when quiet).
 * Returns 0, or 1 with the reason printed when there is none.
 */
static int
main_load_host_key(
	const struct sshd_config *config,
	struct ssh_key *key,
	int quiet)
{
	static char text[SSH_KEY_FILE_MAX + 1U];
	size_t size;
	unsigned index;
	int status;

	/* Tries each file in its order. */
	for (index = 0; index < config->host_key_count; index++) {
		status = main_read_file(config->host_key[index], text, sizeof(text), &size);
		if (status == 0)
			status = ssh_key_parse_private(text, size, key);
		ssh_wipe(text, sizeof(text));
		if (status == 0)
			return 0;
		if (status == ENOTSUP && !quiet)
			syslog(LOG_INFO, "host key %.200s is not ssh-ed25519: passed over", config->host_key[index]);
		else if (status != ENOTSUP && status != ENOENT)
			fprintf(stderr, "sshd: host key %s: %s\n", config->host_key[index], strerror(status));
	}

	/* Without a key of the one type there is nothing to offer. */
	fprintf(stderr, "sshd: no ssh-ed25519 host key\n");
	return 1;
}

/* Opens the listening socket of the configuration (close-on-exec); the socket, or -1 with the reason printed. */
static int
main_listen(
	const struct sshd_config *config)
{
	struct sockaddr_in address4;
	struct sockaddr_in6 address6;
	int descriptor;
	int family;
	int one;
	int status;

	/* The socket of the family. */
	family = AF_INET;
	if (config->inet6)
		family = AF_INET6;
	descriptor = socket(family, SOCK_STREAM, 0);
	if (descriptor < 0) {
		fprintf(stderr, "sshd: socket: %s\n", strerror(errno));
		return -1;
	}

	/* Marks the socket close-on-exec and lets the address be reused. */
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	one = 1;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	/* Binds the address and the port. */
	if (config->inet6) {
		memset(&address6, 0, sizeof(address6));
		address6.sin6_family = AF_INET6;
		address6.sin6_port = htons(config->port);
		status = inet_pton(AF_INET6, config->listen_address, &address6.sin6_addr);
		if (status != 1)
			address6.sin6_addr = in6addr_any;
		status = bind(descriptor, (struct sockaddr *)&address6, sizeof(address6));
	} else {
		memset(&address4, 0, sizeof(address4));
		address4.sin_family = AF_INET;
		address4.sin_port = htons(config->port);
		status = inet_pton(AF_INET, config->listen_address, &address4.sin_addr);
		if (status != 1) {
			fprintf(stderr, "sshd: ListenAddress %s is not an IPv4 address\n", config->listen_address);
			(void)close(descriptor);
			return -1;
		}

		/* Binds the socket to the IPv4 address. */
		status = bind(descriptor, (struct sockaddr *)&address4, sizeof(address4));
	}

	/* Reports a bind failure and closes the socket. */
	if (status != 0) {
		fprintf(stderr, "sshd: bind %s port %u: %s\n", config->listen_address, (unsigned)config->port, strerror(errno));
		(void)close(descriptor);
		return -1;
	}

	/* Listens. */
	status = listen(descriptor, MAIN_BACKLOG);
	if (status != 0) {
		fprintf(stderr, "sshd: listen: %s\n", strerror(errno));
		(void)close(descriptor);
		return -1;
	}

	/* Succeeded: the socket listens. */
	return descriptor;
}

/* Wakes the main loop when a child ends (one write, nothing else). */
static void
main_on_child(
	int signal_number)
{
	static const char mark = 0;
	ssize_t written;
	int saved;

	/* Keeps errno as the interrupted code had it. */
	(void)signal_number;
	saved = errno;
	if (main_wake_descriptor >= 0) {
		written = write(main_wake_descriptor, &mark, 1);
		(void)written;
	}

	/* Restores the error number of the interrupted code. */
	errno = saved;
}

/* Asks the main loop to stop. */
static void
main_on_stop(
	int signal_number)
{
	/* The flag, then a wake. */
	main_stop = 1;
	main_on_child(signal_number);
}

/* Reaps every monitor that ended and frees its slot. */
static void
main_reap(void)
{
	pid_t reaped;
	int status;
	unsigned index;

	/* Takes each ended child, without waiting. */
	for (;;) {
		reaped = waitpid(-1, &status, WNOHANG);
		if (reaped <= 0)
			break;
		for (index = 0; index < MAIN_MONITORS_MAX; index++) {
			if (!main_monitors[index].used || main_monitors[index].pid != reaped)
				continue;
			if (main_monitors[index].started >= 0)
				(void)close(main_monitors[index].started);
			memset(&main_monitors[index], 0, sizeof(main_monitors[index]));
			main_monitors[index].started = -1;
		}
	}
}

/* Returns how many monitors have not yet told of a login. */
static unsigned
main_unauthenticated(void)
{
	unsigned count;
	unsigned index;

	/* Counts the open MaxStartups pipes. */
	count = 0;
	for (index = 0; index < MAIN_MONITORS_MAX; index++) {
		if (main_monitors[index].used && main_monitors[index].started >= 0)
			count++;
	}

	/* The count. */
	return count;
}

/*
 * Accepts one connection (when one is waiting) and forks its monitor;
 * beyond MaxStartups connections not yet authenticated, or when no slot
 * is free, closes it at once.
 */
static void
main_accept(
	int listener,
	const struct sshd_context *context,
	const struct ssh_key *key,
	const char *program)
{
	struct sshd_context child_context;
	struct sockaddr_storage address;
	struct ssh_key child_key;
	struct pollfd waiting;
	socklen_t size;
	pid_t child;
	char peer[MAIN_PEER_MAX];
	unsigned index;
	unsigned slot;
	unsigned other;
	unsigned waiting_count;
	int started[2];
	int client;
	int ready;
	int status;

	/* Takes the connection, if there is one. */
	waiting.fd = listener;
	waiting.events = POLLIN;
	waiting.revents = 0;
	ready = poll(&waiting, 1, 0);
	if (ready <= 0)
		return;
	size = sizeof(address);
	client = accept(listener, (struct sockaddr *)&address, &size);
	if (client < 0)
		return;
	(void)fcntl(client, F_SETFD, FD_CLOEXEC);
	main_describe_peer(&address, peer);

	/* MaxStartups, and the room of the table. */
	slot = MAIN_MONITORS_MAX;
	for (index = 0; index < MAIN_MONITORS_MAX; index++) {
		if (!main_monitors[index].used) {
			slot = index;
			break;
		}
	}

	/* Counts the connections that have not been authenticated. */
	waiting_count = main_unauthenticated();
	if (slot == MAIN_MONITORS_MAX || waiting_count >= context->config.max_startups) {
		syslog(LOG_NOTICE, "too many connections not authenticated: %.63s closed", peer);
		(void)close(client);
		return;
	}

	/* The MaxStartups pipe of the new monitor. */
	status = pipe(started);
	if (status != 0) {
		(void)close(client);
		return;
	}

	/* Marks both ends of the pipe close-on-exec. */
	(void)fcntl(started[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(started[1], F_SETFD, FD_CLOEXEC);

	/* Starts the monitor. */
	child = fork();
	if (child < 0) {
		(void)close(started[0]);
		(void)close(started[1]);
		(void)close(client);
		return;
	}

	/* Runs the monitor in the child. */
	if (child == 0) {
		/* The monitor keeps only its connection and its end of the pipe. */
		(void)close(listener);
		(void)close(started[0]);
		if (main_wake_descriptor >= 0)
			(void)close(main_wake_descriptor);
		for (other = 0; other < MAIN_MONITORS_MAX; other++) {
			if (main_monitors[other].used && main_monitors[other].started >= 0)
				(void)close(main_monitors[other].started);
		}

		/* Restores the default handling of the signals. */
		(void)signal(SIGCHLD, SIG_DFL);
		(void)signal(SIGTERM, SIG_DFL);
		(void)signal(SIGINT, SIG_DFL);
		(void)signal(SIGHUP, SIG_DFL);
		child_context = *context;
		strcpy(child_context.peer, peer);
		child_key = *key;
		status = sshd_monitor_run_program(&child_context, &child_key, client, started[1], program);
		_exit(status);
	}

	/* Keeps the monitor's pipe; the connection is the monitor's. */
	(void)close(client);
	(void)close(started[1]);
	main_monitors[slot].used = 1;
	main_monitors[slot].pid = child;
	main_monitors[slot].started = started[0];
}

/* Writes "address port" of a peer into peer (MAIN_PEER_MAX). */
static void
main_describe_peer(
	const struct sockaddr_storage *address,
	char *peer)
{
	char text[INET6_ADDRSTRLEN];
	unsigned port;

	/* The address and the port of the family. */
	strcpy(text, "unknown");
	port = 0;
	if (address->ss_family == AF_INET) {
		(void)inet_ntop(AF_INET, &((const struct sockaddr_in *)address)->sin_addr, text, sizeof(text));
		port = ntohs(((const struct sockaddr_in *)address)->sin_port);
	} else if (address->ss_family == AF_INET6) {
		(void)inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)address)->sin6_addr, text, sizeof(text));
		port = ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
	}

	/* Ends the address text. */
	text[sizeof(text) - 1U] = '\0';
	sprintf(peer, "%s %u", text, port & 0xffffU);
}
