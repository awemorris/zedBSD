/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver of sshd-pair-host.sh: sshd's three processes on a
 * development host without root, for one connection.  In the place of the
 * listener (which another part provides) it reads a configuration and a
 * host key, listens on 127.0.0.1, takes one connection and runs the
 * monitor on it with this program as the monitor's children (-Zpreauth,
 * -Zsession), so the monitor, the pre-authentication process and the
 * session process are the real ones.
 *
 * So that nothing of the host account is touched, getpwnam_r is replaced
 * here: the account the test runs as gets the home directory "home" and
 * the shell "shell.sh" of the directory this program is in (the work
 * directory of the script), and no other account exists.  syslog goes to
 * the file "log" there, not to the host's log.  The children find the
 * work directory from their own program path, since the monitor starts
 * them with an empty environment.
 *
 *   sshd-pair <config file> <port file> <result file>
 *
 * The port file receives the port listened on; the result file the
 * monitor's exit status and the number of bytes the MaxStartups pipe got.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "sshd-monitor.h"

/* The room for paths. */
#define PAIR_PATH_MAX	1024U

/* The work directory: where this program is. */
static char pair_work[PAIR_PATH_MAX];

/* The name of the account the test runs as. */
static char pair_user[SSH_USER_MAX + 1];

static void pair_find_work(void);
static int pair_read_file(const char *path, char *text, size_t capacity, size_t *size);

/*
 * Answers the account the test runs as, with the work directory's home
 * and shell; every other name is no account.
 */
int
getpwnam_r(
	const char *name,
	struct passwd *account,
	char *buffer,
	size_t size,
	struct passwd **result)
{
	size_t need;
	int different;

	/* Only the test's own account exists. */
	*result = NULL;
	pair_find_work();
	different = strcmp(name, pair_user);
	if (different != 0)
		return 0;

	/* Fills it: the name, the home and the shell in the caller's buffer. */
	need = strlen(pair_user) + 1U + strlen(pair_work) * 2U + 32U;
	if (need > size)
		return ERANGE;
	memset(account, 0, sizeof(*account));
	sprintf(buffer, "%s", pair_user);
	account->pw_name = buffer;
	buffer += strlen(buffer) + 1U;
	sprintf(buffer, "x");
	account->pw_passwd = buffer;
	buffer += 2;
	sprintf(buffer, "%s/home", pair_work);
	account->pw_dir = buffer;
	buffer += strlen(buffer) + 1U;
	sprintf(buffer, "%s/shell.sh", pair_work);
	account->pw_shell = buffer;
	buffer += strlen(buffer) + 1U;
	*buffer = '\0';
	account->pw_gecos = buffer;
	account->pw_uid = getuid();
	account->pw_gid = getgid();
	*result = account;
	return 0;
}

/* Keeps the log file of the work directory open later; nothing to do here. */
void
openlog(
	const char *identity,
	int option,
	int facility)
{
	/* The log is the work directory's file. */
	(void)identity;
	(void)option;
	(void)facility;
}

/* Appends one log line, with the process id, to the work directory's file. */
void
syslog(
	int priority,
	const char *format,
	...)
{
	char line[1024];
	char path[PAIR_PATH_MAX + 8U];
	va_list arguments;
	int descriptor;
	int length;
	ssize_t written;

	/* The line. */
	pair_find_work();
	length = snprintf(line, sizeof(line), "%ld %d ", (long)getpid(), priority & 7);
	va_start(arguments, format);
	(void)vsnprintf(line + length, sizeof(line) - (size_t)length - 1U, format, arguments);
	va_end(arguments);
	strcat(line, "\n");

	/* Appended whole. */
	(void)snprintf(path, sizeof(path), "%s/log", pair_work);
	descriptor = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return;
	written = write(descriptor, line, strlen(line));
	(void)written;
	(void)close(descriptor);
}

/*
 * Runs one connection, or one of the monitor's children.
 */
int
main(
	int argc,
	char **argv)
{
	static char text[SSH_KEY_FILE_MAX + 1U];
	struct sshd_context context;
	struct ssh_key host_key;
	struct ssh_buf blob;
	struct sockaddr_in address;
	socklen_t address_size;
	char error_text[SSH_CONFIG_ERROR_MAX];
	char program[PAIR_PATH_MAX + 16U];
	char mark[4];
	FILE *output;
	size_t size;
	int listener;
	int client;
	int started[2];
	int status;
	int one;
	ssize_t got;
	struct pollfd waiting;
	int is_preauth;
	int is_session;
	int ready;
	int bound;
	int listening;
	int granted;

	/* The monitor's children: this program is also what the monitor starts for them. */
	pair_find_work();
	is_preauth = 0;
	is_session = 0;
	if (argc >= 2) {
		is_preauth = (strcmp(argv[1], SSHD_EXEC_PREAUTH) == 0);
		is_session = (strcmp(argv[1], SSHD_EXEC_SESSION) == 0);
	}

	/* Runs as the pre-authentication or session child when asked to; neither returns. */
	if (is_preauth)
		sshd_preauth_run();
	if (is_session)
		sshd_session_run();

	/* Refuses a command line that does not name the three files. */
	if (argc != 4) {
		fprintf(stderr, "usage: sshd-pair <config> <port file> <result file>\n");
		return 2;
	}

	/* The settings and the host key, as the listener reads them. */
	memset(&context, 0, sizeof(context));
	sshd_config_defaults(&context.config);

	/* Reads and parses the configuration file. */
	status = pair_read_file(argv[1], text, sizeof(text), &size);
	if (status == 0)
		status = sshd_config_parse(&context.config, text, size, error_text);
	if (status != 0) {
		fprintf(stderr, "sshd-pair: config: %s\n", error_text);
		return 2;
	}

	/* Reads and parses the host key the configuration names, then wipes the text. */
	(void)sshd_config_finish(&context.config);
	status = pair_read_file(context.config.host_key[0], text, sizeof(text), &size);
	if (status == 0)
		status = ssh_key_parse_private(text, size, &host_key);
	ssh_wipe(text, sizeof(text));
	if (status != 0) {
		fprintf(stderr, "sshd-pair: host key: %d\n", status);
		return 2;
	}

	/* Keeps the host key's public blob in the context, where the monitor takes it from. */
	ssh_buf_init(&blob, 64, 0);
	ssh_key_put_public_blob(host_key.public_key, &blob);
	memcpy(context.host_key_blob, blob.data, SSH_KEY_BLOB_SIZE);
	ssh_buf_free(&blob);
	context.privileged = 0;

	/* Listens on 127.0.0.1 at a port the system picks, and tells the script which. */
	listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	one = 1;
	(void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	bound = bind(listener, (struct sockaddr *)&address, sizeof(address));
	listening = -1;
	if (bound == 0)
		listening = listen(listener, 1);
	if (bound != 0 || listening != 0) {
		perror("sshd-pair: listen");
		return 2;
	}

	/* Writes the port the system picked to the file the script reads. */
	address_size = sizeof(address);
	(void)getsockname(listener, (struct sockaddr *)&address, &address_size);
	output = fopen(argv[2], "w");
	if (output == NULL)
		return 2;
	fprintf(output, "%u\n", (unsigned)ntohs(address.sin_port));
	fclose(output);

	/* Takes one connection, waiting no longer than a minute. */
	waiting.fd = listener;
	waiting.events = POLLIN;
	waiting.revents = 0;
	ready = poll(&waiting, 1, 60000);
	if (ready <= 0) {
		fprintf(stderr, "sshd-pair: no connection\n");
		return 2;
	}

	/* Accepts the connection and keeps the listener from reaching the children. */
	address_size = sizeof(address);
	client = accept(listener, (struct sockaddr *)&address, &address_size);
	(void)close(listener);
	if (client < 0)
		return 2;
	(void)fcntl(client, F_SETFD, FD_CLOEXEC);
	(void)snprintf(context.peer, sizeof(context.peer), "%s %u", inet_ntoa(address.sin_addr), (unsigned)ntohs(address.sin_port));

	/* Runs the monitor with this program as its children, and the MaxStartups pipe. */
	ready = pipe(started);
	if (ready != 0)
		return 2;
	(void)fcntl(started[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(started[1], F_SETFD, FD_CLOEXEC);
	(void)snprintf(program, sizeof(program), "%s/sshd-pair", pair_work);
	status = sshd_monitor_run_program(&context, &host_key, client, started[1], program);
	got = read(started[0], mark, sizeof(mark));
	(void)close(started[0]);

	/* Tells the script how the monitor ended. */
	output = fopen(argv[3], "w");
	if (output == NULL)
		return 2;
	granted = 0;
	if (got > 0)
		granted = (int)got;
	fprintf(output, "status %d granted %d\n", status, granted);
	fclose(output);
	return status;
}

/* Finds the work directory (the directory of this program) and the account the test runs as, once. */
static void
pair_find_work(void)
{
	struct passwd account;
	struct passwd *found;
	char buffer[2048];
	char *slash;
	ssize_t got;
	int error;

	/* Already found. */
	if (pair_work[0] != '\0')
		return;

	/* The program's own path, without its name. */
	got = readlink("/proc/self/exe", pair_work, sizeof(pair_work) - 1U);
	if (got <= 0) {
		strcpy(pair_work, ".");
	} else {
		pair_work[got] = '\0';
		slash = strrchr(pair_work, '/');
		if (slash != NULL)
			*slash = '\0';
	}

	/* The real name of the account (getpwuid_r is not replaced). */
	found = NULL;
	error = getpwuid_r(getuid(), &account, buffer, sizeof(buffer), &found);
	if (error == 0 && found != NULL)
		strncpy(pair_user, account.pw_name, sizeof(pair_user) - 1U);
}

/* Reads a whole file of at most capacity - 1 bytes into text (NUL-terminated); 0 or an errno value. */
static int
pair_read_file(
	const char *path,
	char *text,
	size_t capacity,
	size_t *size)
{
	int descriptor;
	ssize_t got;

	/* Reads it in one go. */
	*size = 0;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	got = read(descriptor, text, capacity - 1U);
	(void)close(descriptor);
	if (got < 0)
		return EIO;
	text[got] = '\0';
	*size = (size_t)got;
	return 0;
}
