/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session process of sshd: the connection protocol as the user.
 *
 * It takes the settings, the user and the exported transport from the
 * monitor, becomes the user (as root) before it reads anything of the
 * client's, imports the transport and runs the session channels of RFC
 * 4254 6: a pseudo-terminal it opens itself (so the user owns it), the
 * accepted environment, and one shell, command or sftp subsystem per
 * channel, each run under the user's login shell.  A command's standard
 * streams are pipes; with a terminal they are the terminal.  The child
 * gets only those three descriptors.  When the child has ended and all of
 * its output has been sent, the channel gets exit-status or exit-signal,
 * EOF and CLOSE (6.10).  Re-exchanges are signed by the monitor; the
 * login records of terminals are written by the monitor.
 *
 * The environment of a child (sshd(8) LOGIN PROCESS, "basic
 * environment"): HOME, USER, LOGNAME, SHELL, PATH, SSH_CONNECTION,
 * SSH_CLIENT, with a terminal SSH_TTY and TERM, and of what the client
 * asks for (6.4) only LANG, LC_* and TERM, so that a client cannot steer
 * the child with LD_* or the like.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

#include "ssh-channel.h"
#include "ssh-ttymodes.h"
#include "sshd-child.h"
#include "sshd-monitor.h"

/* The most bytes one read of a child takes. */
#define SESSION_READ_CHUNK	16384U

/*
 * The most bytes one read of the connection takes: the room the packet
 * layer keeps beside a packet not yet whole (SSH_PACKET_INPUT_MAX), so a
 * read always fits.
 */
#define SESSION_CLIENT_READ	4096U

/* The most environment variables a client may set on one channel. */
#define SESSION_ENV_MAX		32U

/* The room for one accepted variable as NAME=VALUE with its NUL. */
#define SESSION_ENV_ENTRY	(SSH_CHANNEL_ENV_NAME_MAX + 1U + SSH_CHANNEL_ENV_VALUE_MAX + 1U)

/* The variables sshd sets itself, at most. */
#define SESSION_ENV_FIXED	10U

/* The room for one variable sshd sets itself. */
#define SESSION_ENV_FIXED_ENTRY	(SSH_CONFIG_PATH_MAX + 32U)

/* The search path of a child, as the console login gives it. */
#define SESSION_PATH		"/bin:/sbin:/usr/bin:/usr/sbin"

/* The shell of an account that names none. */
#define SESSION_SHELL		"/bin/sh"

/* The output above which the children's output is not read until the client takes more. */
#define SESSION_OUTPUT_SOFT	(1024U * 1024U)

/* The longest a poll waits when nothing is due, in milliseconds. */
#define SESSION_POLL_MS		10000

/* The room for a pseudo-terminal's name as openpty writes it. */
#define SESSION_TTY_NAME_MAX	64U

/* What a read of a child's stream found. */
#define SESSION_READ_MORE	1
#define SESSION_READ_NONE	0
#define SESSION_READ_END	(-1)

/* The room for "address port address port". */
#define SESSION_CONNECTION_MAX	128U

/* The one subsystem these programs run. */
#define SESSION_SFTP		"sftp"

/* The global request ClientAliveInterval sends (OpenSSH PROTOCOL 2.4: any name the peer does not know is answered). */
#define SESSION_KEEPALIVE	"keepalive@openssh.com"

/*
 * Values referenced: RFC 4254 section 6.10, the signal names of
 * exit-signal and of "signal" requests (6.9), without the "SIG" prefix:
 * ABRT, ALRM, FPE, HUP, ILL, INT, KILL, PIPE, QUIT, SEGV, TERM, USR1,
 * USR2.  The numbers are the local <signal.h>'s.
 */
struct session_signal_name {
	const char *name;
	int number;
};

/*
 * The program's state of one session channel: what the client asked for
 * before the child started, and the child and its descriptors after.
 *
 * channel is the channel of the table (NULL while the slot is unused).
 * have_pty says a pseudo-terminal was asked for: master and slave are its
 * two ends (slave only until the child has it), tty its path, term, size
 * and modes what the client gave.  env holds the accepted NAME=VALUE
 * strings.  child is the started process (its own process group);
 * to_child takes the client's data (a pipe, or the master), from_child
 * and err_child give the child's output (the master, or two pipes);
 * input keeps the client's data not yet written.  client_eof is the
 * client's EOF; exited and wait_status the child's end; out_done and
 * err_done say its output streams have ended.  recorded says a login
 * record of the terminal was written.  finished says the end of the
 * channel was sent.
 */
struct session_link {
	struct ssh_channel *channel;
	int started;
	int have_pty;
	int master;
	int slave;
	char tty[SESSION_TTY_NAME_MAX];
	char term[SSH_CHANNEL_TERM_MAX + 1];
	struct winsize size;
	char env[SESSION_ENV_MAX][SESSION_ENV_ENTRY];
	unsigned env_count;
	pid_t child;
	int to_child;
	int from_child;
	int err_child;
	struct ssh_buf input;
	int client_eof;
	int exited;
	int wait_status;
	int out_done;
	int err_done;
	int recorded;
	int finished;
};

/*
 * The connection of the session process.
 *
 * child holds the settings.  account is the user (its strings in
 * account_buffer).  connection and client_text are the texts of
 * SSH_CONNECTION and SSH_CLIENT.  message is
 * the scratch buffer each outgoing message is built in.  alive_missed
 * counts keepalives not answered; last_input is when the client last sent
 * something (seconds, monotonic).
 */
struct session {
	struct sshd_child child;
	struct ssh_transport transport;
	struct ssh_channels channels;
	struct session_link links[SSH_CHANNEL_MAX];
	struct passwd account;
	char account_buffer[SSHD_ACCOUNT_BUFFER];
	char connection[SESSION_CONNECTION_MAX];
	char client_text[SESSION_CONNECTION_MAX];
	struct ssh_buf message;
	unsigned alive_missed;
	uint64_t last_input;
};

/*
 * The write end of the pipe the SIGCHLD handler writes one byte to, so
 * that poll wakes when a child ends.  Set once before the handler is
 * installed and never changed; -1 before.
 */
static int session_wake_descriptor = -1;

/* The signal names of RFC 4254 6.10 and their local numbers. */
static const struct session_signal_name session_signals[] = {
	{ "ABRT", SIGABRT },
	{ "ALRM", SIGALRM },
	{ "FPE", SIGFPE },
	{ "HUP", SIGHUP },
	{ "ILL", SIGILL },
	{ "INT", SIGINT },
	{ "KILL", SIGKILL },
	{ "PIPE", SIGPIPE },
	{ "QUIT", SIGQUIT },
	{ "SEGV", SIGSEGV },
	{ "TERM", SIGTERM },
	{ "USR1", SIGUSR1 },
	{ "USR2", SIGUSR2 }
};

static int session_take_start(struct session *session, char *user, struct ssh_buf *state);
static int session_become_user(struct session *session, const char *user);
static void session_describe_connection(struct session *session);
static void session_on_child(int signal_number);
static int session_serve(struct session *session, int wake);
static int session_message(struct session *session, const struct ssh_buf *payload);
static int session_open(struct session *session, const struct ssh_buf *payload);
static int session_data(struct session *session, const struct ssh_buf *payload);
static int session_end(struct session *session, const struct ssh_buf *payload);
static int session_request(struct session *session, const struct ssh_buf *payload);
static int session_pty(struct session_link *link, const struct ssh_channel_request *request);
static int session_env(struct session_link *link, const struct ssh_channel_request *request);
static int session_start(struct session *session, struct session_link *link, const char *command, size_t command_size);
static void session_exec(struct session *session, struct session_link *link, const char *command, char *const environment[]);
static int session_build_environment(struct session *session, const struct session_link *link, char storage[][SESSION_ENV_FIXED_ENTRY], char **environment);
static int session_signal_number(const char *name);
static const char *session_signal_name(int number);
static int session_send(struct session *session);
static void session_reap(struct session *session);
static void session_pump(struct session *session, struct session_link *link);
static int session_read_child(struct session *session, struct session_link *link, int extended);
static int session_finish(struct session *session, struct session_link *link);
static void session_release(struct session_link *link);
static void session_hang_up(struct session *session);
static struct session_link *session_link_of(struct session *session, struct ssh_channel *channel);
static void session_close_descriptor(int *descriptor);
static int session_alive(struct session *session, uint64_t now);
static unsigned short session_clip(uint32_t value);

/*
 * Runs the session process (sshd started with SSHD_EXEC_SESSION): reads
 * the settings, the user and the exported transport from the monitor,
 * takes the user's ids, imports the transport and runs the connection
 * protocol.
 */
void
sshd_session_run(void)
{
	static struct session session;
	struct ssh_buf state;
	struct ssh_transport_ops ops;
	struct sigaction action;
	char user[SSH_USER_MAX + 1];
	int wake[2];
	int status;
	unsigned index;

	/* Starts as a child of the monitor and takes the settings, the user and the transport. */
	sshd_child_start();
	memset(&session, 0, sizeof(session));
	ssh_buf_init(&state, SSH_MON_STATE_MAX, 1);
	status = session_take_start(&session, user, &state);
	if (status != 0) {
		ssh_buf_free(&state);
		syslog(LOG_ERR, "session: no start from the monitor");
		_exit(1);
	}

	/* Becomes the user before anything of the client's is read. */
	status = session_become_user(&session, user);
	if (status != 0) {
		ssh_buf_free(&state);
		syslog(LOG_ERR, "session: cannot become %.64s", user);
		_exit(1);
	}

	/* Takes the connection over, signing re-exchanges through the monitor. */
	memset(&ops, 0, sizeof(ops));
	ops.host_key = sshd_child_host_key;
	ops.sign = sshd_child_sign;
	ops.context = &session.child;
	status = ssh_transport_import(&session.transport, state.data, state.size, &ops, sshd_child_now());
	ssh_buf_free(&state);
	if (status != 0) {
		syslog(LOG_ERR, "session: the transport could not be taken over");
		_exit(1);
	}

	/* Sets up the channels and the message buffer of the session. */
	ssh_channels_init(&session.channels);
	ssh_buf_init(&session.message, SSH_PACKET_LIMIT, 0);
	session_describe_connection(&session);
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		session.links[index].master = -1;
		session.links[index].slave = -1;
		session.links[index].to_child = -1;
		session.links[index].from_child = -1;
		session.links[index].err_child = -1;
	}

	/* Wakes poll when a child ends. */
	status = pipe(wake);
	if (status != 0)
		_exit(1);
	(void)fcntl(wake[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(wake[1], F_SETFD, FD_CLOEXEC);
	(void)fcntl(wake[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(wake[1], F_SETFL, O_NONBLOCK);
	session_wake_descriptor = wake[1];
	memset(&action, 0, sizeof(action));
	action.sa_handler = session_on_child;
	(void)sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	(void)sigaction(SIGCHLD, &action, NULL);

	/* Runs the connection until the client goes. */
	session.last_input = sshd_child_now();
	status = session_serve(&session, wake[0]);

	/* Ends what is left: the children's terminals hang up, their records close. */
	session_hang_up(&session);
	(void)sshd_child_flush(&session.transport);
	ssh_transport_free(&session.transport);
	ssh_buf_free(&session.message);
	if (status != 0)
		_exit(1);
	_exit(0);
}

/*
 * Reads CONFIG and SESSION_START from the monitor: the settings into
 * session, the user (SSH_USER_MAX + 1 bytes) and the exported transport
 * into state.  Returns 0 or EPROTO.
 */
static int
session_take_start(
	struct session *session,
	char *user,
	struct ssh_buf *state)
{
	struct ssh_buf input;
	struct ssh_buf body;
	struct ssh_reader reader;
	enum ssh_mon_type type;
	const uint8_t *data;
	size_t size;
	int status;

	/* The two frames, in their order. */
	ssh_buf_init(&input, 4U + (size_t)SSH_MON_STATE_MAX, 1);
	ssh_buf_init(&body, SSH_MON_STATE_MAX, 1);
	status = sshd_child_read_config(&input, &session->child);
	if (status == 0)
		status = sshd_child_read_frame(&input, &type, &body);
	if (status == 0 && type != SSH_MON_SESSION_START)
		status = EPROTO;

	/* SESSION_START: string user, string transport, nothing after. */
	if (status == 0) {
		ssh_reader_init(&reader, body.data, body.size);
		ssh_reader_get_cstring(&reader, user, SSH_USER_MAX + 1U);
		ssh_reader_get_string(&reader, &data, &size);
		status = ssh_reader_finish(&reader);
		if (status == 0 &&
		    (user[0] == '\0' ||
		     size == 0))
			status = EPROTO;
		if (status == 0) {
			ssh_buf_put_bytes(state, data, size);
			status = ssh_buf_error(state);
		}
	}

	/* Releases the buffers. */
	ssh_buf_free(&input);
	ssh_buf_free(&body);
	if (status != 0)
		return EPROTO;

	/* Succeeded: the settings, the user and the transport are here. */
	return 0;
}

/*
 * Becomes the user: as root takes its groups, group and user (and checks
 * that root cannot be taken back); without root the process must already
 * be the user.  Then changes to the home directory (the root directory
 * when there is none).  Returns 0 or EPERM.
 */
static int
session_become_user(
	struct session *session,
	const char *user)
{
	struct passwd *found;
	uid_t self;
	uid_t real_user;
	gid_t real_group;
	int changed;

	/* Finds the account. */
	found = NULL;
	changed = getpwnam_r(user, &session->account, session->account_buffer, sizeof(session->account_buffer), &found);
	if (changed != 0 || found == NULL)
		return EPERM;

	/* Takes the user's ids, or checks that they are this process's already. */
	self = geteuid();
	if (session->child.privileged) {
		if (self != 0)
			return EPERM;
		changed = initgroups(session->account.pw_name, session->account.pw_gid);
		if (changed != 0)
			return EPERM;
		changed = setgid(session->account.pw_gid);
		if (changed != 0)
			return EPERM;
		changed = setuid(session->account.pw_uid);
		if (changed != 0)
			return EPERM;
		if (session->account.pw_uid != 0) {
			changed = setuid(0);
			if (changed == 0)
				return EPERM;
		}
	}

	/* Takes the identity of the process. */
	real_user = getuid();
	real_group = getgid();
	self = geteuid();
	if (real_user != session->account.pw_uid || self != session->account.pw_uid)
		return EPERM;
	if (session->child.privileged && real_group != session->account.pw_gid)
		return EPERM;

	/* Starts in the home directory. */
	changed = -1;
	if (session->account.pw_dir != NULL && session->account.pw_dir[0] != '\0')
		changed = chdir(session->account.pw_dir);
	if (changed != 0)
		(void)chdir("/");

	/* Succeeded: the process is the user. */
	return 0;
}

/*
 * Writes SSH_CONNECTION's text, "client address, client port, server
 * address, server port" (ssh(1) ENVIRONMENT), and SSH_CLIENT's, "client
 * address, client port, server port", from the connection's two ends; an
 * end that cannot be told is "unknown 0".
 */
static void
session_describe_connection(
	struct session *session)
{
	struct sockaddr_storage peer;
	struct sockaddr_storage local;
	socklen_t size;
	char peer_text[INET6_ADDRSTRLEN];
	char local_text[INET6_ADDRSTRLEN];
	unsigned peer_port;
	unsigned local_port;
	int looked;

	/* The two ends, each as an address and a port. */
	strcpy(peer_text, "unknown");
	strcpy(local_text, "unknown");
	peer_port = 0;
	local_port = 0;
	size = sizeof(peer);
	looked = getpeername(SSHD_FD_CLIENT, (struct sockaddr *)&peer, &size);
	if (looked == 0 && peer.ss_family == AF_INET) {
		(void)inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, peer_text, sizeof(peer_text));
		peer_port = ntohs(((struct sockaddr_in *)&peer)->sin_port);
	} else if (looked == 0 && peer.ss_family == AF_INET6) {
		(void)inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, peer_text, sizeof(peer_text));
		peer_port = ntohs(((struct sockaddr_in6 *)&peer)->sin6_port);
	}

	/* Looks up the local address of the connection. */
	size = sizeof(local);
	looked = getsockname(SSHD_FD_CLIENT, (struct sockaddr *)&local, &size);
	if (looked == 0 && local.ss_family == AF_INET) {
		(void)inet_ntop(AF_INET, &((struct sockaddr_in *)&local)->sin_addr, local_text, sizeof(local_text));
		local_port = ntohs(((struct sockaddr_in *)&local)->sin_port);
	} else if (looked == 0 && local.ss_family == AF_INET6) {
		(void)inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&local)->sin6_addr, local_text, sizeof(local_text));
		local_port = ntohs(((struct sockaddr_in6 *)&local)->sin6_port);
	}

	/* The text: both addresses are at most INET6_ADDRSTRLEN and both ports five digits. */
	peer_text[sizeof(peer_text) - 1U] = '\0';
	local_text[sizeof(local_text) - 1U] = '\0';
	sprintf(session->connection, "%s %u %s %u", peer_text, peer_port & 0xffffU, local_text, local_port & 0xffffU);
	sprintf(session->client_text, "%s %u %u", peer_text, peer_port & 0xffffU, local_port & 0xffffU);
}

/* Wakes the main loop when a child ends (the SIGCHLD handler: one write, nothing else). */
static void
session_on_child(
	int signal_number)
{
	static const char mark = 0;
	int saved;

	/* Keeps errno as the interrupted code had it. */
	(void)signal_number;
	saved = errno;
	if (session_wake_descriptor >= 0)
		(void)write(session_wake_descriptor, &mark, 1);
	errno = saved;
}

/*
 * Runs the connection: the client's messages, the children's output and
 * input, their ends, the keepalives and the re-exchanges, until the
 * client goes (0) or the connection fails (1).
 */
static int
session_serve(
	struct session *session,
	int wake)
{
	struct ssh_buf payload;
	struct pollfd waiting[2 + 3 * SSH_CHANNEL_MAX];
	struct session_link *owners[2 + 3 * SSH_CHANNEL_MAX];
	int kinds[2 + 3 * SSH_CHANNEL_MAX];
	uint8_t chunk[SESSION_READ_CHUNK];
	struct session_link *link;
	const uint8_t *pending;
	size_t size;
	ssize_t got;
	size_t room;
	unsigned count;
	unsigned index;
	int timeout;
	int ready;
	int status;
	char drain[64];

	/* Received messages may hold anything the user types: the buffer is secret. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);
	status = 0;

	/* Each round: messages, children, output, then a wait. */
	for (;;) {
		/* Handles every message the input holds. */
		for (;;) {
			status = ssh_transport_next(&session->transport, &payload);
			if (status == EAGAIN) {
				status = 0;
				break;
			}

			/* Stops on a failure. */
			if (status != 0)
				break;
			status = session_message(session, &payload);
			ssh_buf_reset(&payload);
			if (status != 0)
				break;
		}

		/* Stops when the loop failed. */
		if (status != 0)
			break;

		/* Reaps the children that ended, moves data to them, and ends the channels that are done. */
		session_reap(session);
		for (index = 0; index < SSH_CHANNEL_MAX; index++) {
			link = &session->links[index];
			if (link->channel == NULL)
				continue;
			session_pump(session, link);
			status = session_finish(session, link);
			if (status != 0)
				break;
		}

		/* Stops when finishing a link failed. */
		if (status != 0)
			break;

		/* Writes what is pending, as much as the connection takes now. */
		pending = ssh_transport_pending(&session->transport, &size);
		while (size > 0) {
			got = write(SSHD_FD_CLIENT, pending, size);
			if (got <= 0)
				break;
			ssh_transport_written(&session->transport, (size_t)got);
			pending = ssh_transport_pending(&session->transport, &size);
		}

		/* Stops when the transport has closed. */
		if (session->transport.state == SSH_TRANSPORT_CLOSED) {
			status = EPIPE;
			break;
		}

		/* Waits for the client, the monitor's end, the wake pipe, and every child stream that can move. */
		count = 0;
		waiting[count].fd = SSHD_FD_CLIENT;
		waiting[count].events = POLLIN;
		if (size > 0)
			waiting[count].events |= POLLOUT;
		kinds[count] = 0;
		owners[count] = NULL;
		count++;
		waiting[count].fd = wake;
		waiting[count].events = POLLIN;
		kinds[count] = 1;
		owners[count] = NULL;
		count++;
		for (index = 0; index < SSH_CHANNEL_MAX; index++) {
			link = &session->links[index];
			if (link->channel == NULL || !link->started)
				continue;
			room = ssh_channel_send_room(link->channel);
			if (size < SESSION_OUTPUT_SOFT && room > 0) {
				if (link->from_child >= 0 && !link->out_done) {
					waiting[count].fd = link->from_child;
					waiting[count].events = POLLIN;
					kinds[count] = 2;
					owners[count] = link;
					count++;
				}

				/* Waits for the error stream of the link too. */
				if (link->err_child >= 0 && !link->err_done) {
					waiting[count].fd = link->err_child;
					waiting[count].events = POLLIN;
					kinds[count] = 3;
					owners[count] = link;
					count++;
				}
			}

			/* Waits to write the input of the link when some is queued. */
			if (link->to_child >= 0 && link->input.size > 0) {
				waiting[count].fd = link->to_child;
				waiting[count].events = POLLOUT;
				kinds[count] = 4;
				owners[count] = link;
				count++;
			}
		}

		/* Clears the results of the previous poll and sets the timeout. */
		for (index = 0; index < count; index++)
			waiting[index].revents = 0;
		timeout = SESSION_POLL_MS;
		if (session->child.config.client_alive_interval != 0 &&
		    session->child.config.client_alive_interval < (unsigned)(SESSION_POLL_MS / 1000))
			timeout = (int)session->child.config.client_alive_interval * 1000;
		ready = poll(waiting, (nfds_t)count, timeout);
		if (ready < 0 && errno != EINTR) {
			status = EIO;
			break;
		}

		/* Handles what is ready. */
		for (index = 0; ready > 0 && index < count; index++) {
			if (waiting[index].revents == 0)
				continue;
			if (kinds[index] == 0 && (waiting[index].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
				/* The client: an end of file is the client gone. */
				got = read(SSHD_FD_CLIENT, chunk, SESSION_CLIENT_READ);
				if (got == 0) {
					status = EPIPE;
					break;
				}

				/* Feeds the bytes that were read to the transport. */
				if (got > 0) {
					session->last_input = sshd_child_now();
					session->alive_missed = 0;
					status = ssh_transport_input(&session->transport, chunk, (size_t)got);
					if (status != 0)
						break;
				}
			} else if (kinds[index] == 1) {
				/* The wake pipe: emptied here, the children reaped at the top. */
				do {
					got = read(wake, drain, sizeof(drain));
				} while (got > 0);
			} else if (kinds[index] == 2) {
				session_read_child(session, owners[index], 0);
			} else if (kinds[index] == 3) {
				session_read_child(session, owners[index], SSH_EXTENDED_DATA_STDERR);
			} else if (kinds[index] == 4) {
				session_pump(session, owners[index]);
			}
		}

		/* Stops when handling the events failed. */
		if (status != 0)
			break;

		/* Lets the transport re-key, and keeps the client alive. */
		status = ssh_transport_tick(&session->transport, sshd_child_now());
		if (status == 0)
			status = session_alive(session, sshd_child_now());
		if (status != 0)
			break;
	}

	/* Wipes what was read. */
	ssh_wipe(chunk, sizeof(chunk));
	ssh_buf_free(&payload);

	/* The client going is the normal end. */
	if (status == EPIPE || status == ECONNABORTED)
		return 0;

	/* Reports a connection that failed, and why. */
	syslog(LOG_NOTICE, "session: the connection of %.63s failed (%d)", session->child.peer, status);
	return 1;
}

/*
 * Handles one message of the connection protocol (RFC 4254).  An error
 * from a parser is the client breaking the protocol: the connection ends
 * with SSH_DISCONNECT_PROTOCOL_ERROR.
 */
static int
session_message(
	struct session *session,
	const struct ssh_buf *payload)
{
	struct ssh_channel *adjusted;
	char name[SSH_NAME_MAX + 1];
	int want_reply;
	int status;

	/* Dispatches by the message number. */
	status = 0;
	switch (payload->data[0]) {
	case SSH_MSG_GLOBAL_REQUEST:
		ssh_buf_reset(&session->message);
		status = ssh_channel_global_request(&session->channels, payload->data, payload->size, name, &want_reply, &session->message);
		if (status == 0 && session->message.size > 0)
			status = session_send(session);
		break;
	case SSH_MSG_REQUEST_SUCCESS:
	case SSH_MSG_REQUEST_FAILURE:
		/* The answer to a keepalive: the client is alive. */
		session->alive_missed = 0;
		break;
	case SSH_MSG_CHANNEL_OPEN:
		status = session_open(session, payload);
		break;
	case SSH_MSG_CHANNEL_WINDOW_ADJUST:
		status = ssh_channel_parse_window_adjust(&session->channels, payload->data, payload->size, &adjusted);
		break;
	case SSH_MSG_CHANNEL_DATA:
	case SSH_MSG_CHANNEL_EXTENDED_DATA:
		status = session_data(session, payload);
		break;
	case SSH_MSG_CHANNEL_EOF:
	case SSH_MSG_CHANNEL_CLOSE:
		status = session_end(session, payload);
		break;
	case SSH_MSG_CHANNEL_REQUEST:
		status = session_request(session, payload);
		break;
	case SSH_MSG_CHANNEL_SUCCESS:
	case SSH_MSG_CHANNEL_FAILURE:
		/* This side asks nothing with a reply wanted; an answer is dropped. */
		break;
	default:
		/* Authentication messages after the login are ignored (RFC 4252 5.1), as are local ones; anything else breaks the protocol. */
		if (payload->data[0] >= SSH_MSG_USERAUTH_REQUEST && payload->data[0] <= 79)
			break;
		if (payload->data[0] >= 192)
			break;
		status = EPROTO;
		break;
	}

	/* Ends the connection on a broken message. */
	if (status == EINVAL || status == EPROTO) {
		ssh_transport_disconnect(&session->transport, SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
		return status;
	}

	/* Returns any other failure. */
	if (status != 0)
		return status;

	/* Succeeded: the message is handled. */
	return 0;
}

/*
 * Answers a CHANNEL_OPEN.  A session channel beyond MaxSessions is
 * confirmed and closed at once (the table has room for SSH_CHANNEL_MAX;
 * MaxSessions may be lower).
 */
static int
session_open(
	struct session *session,
	const struct ssh_buf *payload)
{
	struct ssh_channel *channel;
	struct session_link *link;
	unsigned index;
	unsigned active;
	int status;

	/* Lets the table answer: a session channel or a refusal. */
	ssh_buf_reset(&session->message);
	status = ssh_channel_accept_open(&session->channels, payload->data, payload->size, &session->message, &channel);
	if (status != 0)
		return status;
	status = session_send(session);
	if (status != 0 || channel == NULL)
		return status;

	/* Counts the sessions open before this one. */
	active = 0;
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (session->links[index].channel != NULL)
			active++;
	}

	/* Takes the link of the channel's slot. */
	link = &session->links[channel->local_id];
	memset(link, 0, sizeof(*link));
	link->channel = channel;
	link->master = -1;
	link->slave = -1;
	link->to_child = -1;
	link->from_child = -1;
	link->err_child = -1;
	link->child = -1;
	ssh_buf_init(&link->input, SSH_CHANNEL_WINDOW + SSH_CHANNEL_PACKET, 1);
	channel->user = link;

	/* A session beyond MaxSessions is closed at once. */
	if (active >= session->child.config.max_sessions) {
		link->finished = 1;
		ssh_buf_reset(&session->message);
		ssh_channel_put_close(channel, &session->message);
		status = session_send(session);
		if (status != 0)
			return status;
	}

	/* Succeeded: the channel is open. */
	return 0;
}

/*
 * Takes CHANNEL_DATA or CHANNEL_EXTENDED_DATA: data for a running child
 * is kept until written to it; anything else (extended data, data before
 * the child or after its input closed, data on a closing channel) is
 * dropped and given back to the window at once.
 */
static int
session_data(
	struct session *session,
	const struct ssh_buf *payload)
{
	struct ssh_channel *channel;
	struct session_link *link;
	const uint8_t *data;
	size_t size;
	uint32_t extended;
	int status;

	/* Reads the message against the channel's window. */
	status = ssh_channel_parse_data(&session->channels, payload->data, payload->size, &channel, &data, &size, &extended);
	if (status != 0)
		return status;
	link = session_link_of(session, channel);

	/* Keeps data for a child that reads its input. */
	if (link != NULL &&
	    channel->state == SSH_CHANNEL_OPEN &&
	    extended == 0 &&
	    link->to_child >= 0) {
		ssh_buf_put_bytes(&link->input, data, size);
		status = ssh_buf_error(&link->input);
		if (status != 0)
			return EPROTO;
		return 0;
	}

	/* Drops the rest and gives the window back. */
	ssh_buf_reset(&session->message);
	ssh_channel_consumed(channel, size, &session->message);
	if (session->message.size > 0) {
		status = session_send(session);
		if (status != 0)
			return status;
	}

	/* Succeeded: the data is taken. */
	return 0;
}

/*
 * Takes CHANNEL_EOF (the child's input closes once its kept data is
 * written) or CHANNEL_CLOSE (this side closes too, the child's
 * descriptors close and its process group hangs up; the slot is freed
 * when both closes have gone).
 */
static int
session_end(
	struct session *session,
	const struct ssh_buf *payload)
{
	struct ssh_channel *channel;
	struct session_link *link;
	int status;
	int freed;

	/* Reads the message. */
	status = ssh_channel_parse_end(&session->channels, payload->data, payload->size, &channel);
	if (status != 0)
		return status;
	link = session_link_of(session, channel);

	/* An EOF only marks the input's end. */
	if (payload->data[0] == SSH_MSG_CHANNEL_EOF) {
		if (link != NULL)
			link->client_eof = 1;
		return 0;
	}

	/* A CLOSE is answered with this side's, if not sent yet. */
	if (!channel->close_sent) {
		ssh_buf_reset(&session->message);
		ssh_channel_put_close(channel, &session->message);
		status = session_send(session);
		if (status != 0)
			return status;
	}

	/* The channel is gone both ways: the child loses its streams, the slot is freed. */
	if (link != NULL)
		session_release(link);
	freed = ssh_channel_closed(&session->channels, channel);
	(void)freed;

	/* Succeeded: the channel is closed. */
	return 0;
}

/*
 * Handles one CHANNEL_REQUEST.  On a closing channel, or for a request
 * that is not offered, a wanted reply is FAILURE.
 */
static int
session_request(
	struct session *session,
	const struct ssh_buf *payload)
{
	struct ssh_channel_request request;
	struct ssh_channel *channel;
	struct session_link *link;
	int success;
	int status;
	int number;

	/* Reads the request. */
	status = ssh_channel_parse_request(&session->channels, payload->data, payload->size, &request);
	if (status != 0)
		return status;
	channel = ssh_channel_find(&session->channels, request.recipient);
	if (channel == NULL)
		return EPROTO;
	link = session_link_of(session, channel);

	/* Carries out what may be carried out. */
	success = 0;
	if (link != NULL &&
	    channel->state == SSH_CHANNEL_OPEN &&
	    !link->finished) {
		switch (request.type) {
		case SSH_CREQ_PTY:
			if (!link->started && !link->have_pty)
				success = session_pty(link, &request);
			break;
		case SSH_CREQ_ENV:
			if (!link->started)
				success = session_env(link, &request);
			break;
		case SSH_CREQ_SHELL:
			if (!link->started) {
				status = session_start(session, link, NULL, 0);
				if (status == 0)
					success = 1;
			}

			break;
		case SSH_CREQ_EXEC:
			if (!link->started) {
				status = session_start(session, link, (const char *)request.command, request.command_size);
				if (status == 0)
					success = 1;
			}

			break;
		case SSH_CREQ_SUBSYSTEM:
			status = strcmp(request.name, SESSION_SFTP);
			if (!link->started &&
			    status == 0 &&
			    session->child.config.sftp_command[0] != '\0') {
				status = session_start(session, link, session->child.config.sftp_command, strlen(session->child.config.sftp_command));
				if (status == 0)
					success = 1;
			}

			break;
		case SSH_CREQ_WINDOW_CHANGE:
			if (link->have_pty && link->master >= 0) {
				link->size.ws_col = session_clip(request.columns);
				link->size.ws_row = session_clip(request.rows);
				link->size.ws_xpixel = session_clip(request.width);
				link->size.ws_ypixel = session_clip(request.height);
				status = ioctl(link->master, TIOCSWINSZ, &link->size);
				if (status == 0)
					success = 1;
			}

			break;
		case SSH_CREQ_SIGNAL:
			number = session_signal_number(request.name);
			if (link->started &&
			    !link->exited &&
			    number != 0) {
				status = kill(-link->child, number);
				if (status == 0)
					success = 1;
			}

			break;
		default:
			/* exit-status and exit-signal come from a server, eow and the rest are not offered. */
			break;
		}
	}

	/* Answers when asked to. */
	if (request.want_reply) {
		ssh_buf_reset(&session->message);
		ssh_channel_put_reply(channel, success, &session->message);
		status = session_send(session);
		if (status != 0)
			return status;
	}

	/* Succeeded: the request is answered. */
	return 0;
}

/*
 * Takes a pty-req: opens a pseudo-terminal (as the user, who then owns
 * it), applies the client's modes and size to it.  Returns 1 (done) or 0
 * (refused).
 */
static int
session_pty(
	struct session_link *link,
	const struct ssh_channel_request *request)
{
	struct termios modes;
	char name[SESSION_TTY_NAME_MAX];
	int opened;
	int status;

	/* The size the client's terminal has (a field too large for the local type is taken as the largest). */
	memset(&link->size, 0, sizeof(link->size));
	link->size.ws_col = session_clip(request->columns);
	link->size.ws_row = session_clip(request->rows);
	link->size.ws_xpixel = session_clip(request->width);
	link->size.ws_ypixel = session_clip(request->height);

	/* Opens the pair; the name is checked to be a pseudo-terminal's, which the monitor's record needs. */
	memset(name, 0, sizeof(name));
	opened = openpty(&link->master, &link->slave, name, NULL, &link->size);
	if (opened != 0) {
		link->master = -1;
		link->slave = -1;
		return 0;
	}

	/* Marks both descriptors close-on-exec and terminates the name. */
	(void)fcntl(link->master, F_SETFD, FD_CLOEXEC);
	(void)fcntl(link->slave, F_SETFD, FD_CLOEXEC);
	name[sizeof(name) - 1U] = '\0';

	/* Applies the client's modes over the terminal's own. */
	status = tcgetattr(link->slave, &modes);
	if (status == 0) {
		status = ssh_ttymodes_apply(request->modes, request->modes_size, &modes);
		if (status == 0)
			(void)tcsetattr(link->slave, TCSANOW, &modes);
	}

	/* Closes both descriptors when the modes could not be applied. */
	if (status != 0) {
		session_close_descriptor(&link->master);
		session_close_descriptor(&link->slave);
		return 0;
	}

	/* Keeps the terminal for the child. */
	strcpy(link->tty, name);
	strcpy(link->term, request->term);
	link->have_pty = 1;
	return 1;
}

/*
 * Takes an env request for LANG, LC_* or TERM only (a name of letters,
 * digits and '_'), at most SESSION_ENV_MAX of them.  Returns 1 (taken)
 * or 0 (refused).
 */
static int
session_env(
	struct session_link *link,
	const struct ssh_channel_request *request)
{
	size_t index;
	size_t name_size;
	int same;
	int allowed;
	char byte;

	/* Only the three kinds of name. */
	allowed = 0;
	same = strcmp(request->name, "LANG");
	if (same == 0)
		allowed = 1;
	same = strcmp(request->name, "TERM");
	if (same == 0)
		allowed = 1;
	same = strncmp(request->name, "LC_", 3);
	if (same == 0 && request->name[3] != '\0')
		allowed = 1;
	if (!allowed || link->env_count >= SESSION_ENV_MAX)
		return 0;

	/* A name of letters, digits and '_' only. */
	name_size = strlen(request->name);
	for (index = 0; index < name_size; index++) {
		byte = request->name[index];
		if ((byte < 'A' || byte > 'Z') &&
		    (byte < '0' || byte > '9') &&
		    byte != '_')
			return 0;
	}

	/* Keeps NAME=VALUE (both fit: the parser bounds them). */
	strcpy(link->env[link->env_count], request->name);
	strcat(link->env[link->env_count], "=");
	strcat(link->env[link->env_count], request->value);
	link->env_count++;
	return 1;
}

/*
 * Starts the channel's child: the login shell with no arguments (command
 * NULL) or with -c and command (command_size bytes, which may not hold a
 * NUL), on the terminal or on three pipes.  Returns 0, EACCES (the
 * shell cannot be executed), EINVAL or EIO.
 */
static int
session_start(
	struct session *session,
	struct session_link *link,
	const char *command,
	size_t command_size)
{
	static char storage[SESSION_ENV_FIXED][SESSION_ENV_FIXED_ENTRY];
	char *environment[SESSION_ENV_FIXED + SESSION_ENV_MAX + 1];
	const char *shell;
	char *text;
	int runnable;
	int in_pipe[2];
	int out_pipe[2];
	int err_pipe[2];
	const void *nul;
	uint32_t result;
	struct ssh_buf body;
	pid_t child;
	int status;

	/*
	 * A shell this process (already the user) cannot execute is refused
	 * here, so the client hears of it; a shell that goes away between this
	 * check and the start still ends the child with 127.
	 */
	shell = session->account.pw_shell;
	if (shell == NULL || shell[0] == '\0')
		shell = SESSION_SHELL;
	runnable = access(shell, X_OK);
	if (runnable != 0) {
		syslog(LOG_NOTICE, "session: the shell %.200s of %.64s cannot be executed", shell, session->account.pw_name);
		return EACCES;
	}

	/* A command is text: one with a NUL inside is refused. */
	text = NULL;
	if (command != NULL) {
		nul = memchr(command, 0, command_size);
		if (nul != NULL)
			return EINVAL;
		text = malloc(command_size + 1U);
		if (text == NULL)
			return EIO;
		memcpy(text, command, command_size);
		text[command_size] = '\0';
	}

	/* The environment the child gets. */
	status = session_build_environment(session, link, storage, environment);
	if (status != 0) {
		free(text);
		return EIO;
	}

	/* The child's streams: the terminal, or three pipes. */
	in_pipe[0] = in_pipe[1] = -1;
	out_pipe[0] = out_pipe[1] = -1;
	err_pipe[0] = err_pipe[1] = -1;
	if (!link->have_pty) {
		status = pipe(in_pipe);
		if (status == 0)
			status = pipe(out_pipe);
		if (status == 0)
			status = pipe(err_pipe);
		if (status != 0) {
			session_close_descriptor(&in_pipe[0]);
			session_close_descriptor(&in_pipe[1]);
			session_close_descriptor(&out_pipe[0]);
			session_close_descriptor(&out_pipe[1]);
			free(text);
			return EIO;
		}

		/* Marks the parent's ends close-on-exec. */
		(void)fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
		(void)fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
		(void)fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
	}

	/* Starts the child. */
	child = fork();
	if (child < 0) {
		session_close_descriptor(&in_pipe[0]);
		session_close_descriptor(&in_pipe[1]);
		session_close_descriptor(&out_pipe[0]);
		session_close_descriptor(&out_pipe[1]);
		session_close_descriptor(&err_pipe[0]);
		session_close_descriptor(&err_pipe[1]);
		free(text);
		return EIO;
	}

	/* Runs the command in the child. */
	if (child == 0) {
		if (!link->have_pty) {
			(void)dup2(in_pipe[0], 0);
			(void)dup2(out_pipe[1], 1);
			(void)dup2(err_pipe[1], 2);
		}

		/* Executes the command through the shell. */
		session_exec(session, link, text, environment);
	}

	/* Releases the command text. */
	free(text);

	/* Keeps the parent's ends, non-blocking, and lets the child's go. */
	link->child = child;
	link->started = 1;
	if (link->have_pty) {
		session_close_descriptor(&link->slave);
		link->to_child = link->master;
		link->from_child = link->master;
		link->err_done = 1;
		(void)fcntl(link->master, F_SETFL, O_NONBLOCK);
	} else {
		session_close_descriptor(&in_pipe[0]);
		session_close_descriptor(&out_pipe[1]);
		session_close_descriptor(&err_pipe[1]);
		link->to_child = in_pipe[1];
		link->from_child = out_pipe[0];
		link->err_child = err_pipe[0];
		(void)fcntl(link->to_child, F_SETFL, O_NONBLOCK);
		(void)fcntl(link->from_child, F_SETFL, O_NONBLOCK);
		(void)fcntl(link->err_child, F_SETFL, O_NONBLOCK);
	}

	/* A terminal's login is recorded by the monitor; a refusal is only logged. */
	if (link->have_pty) {
		ssh_buf_init(&body, 128, 0);
		ssh_buf_put_cstring(&body, link->tty);
		ssh_buf_put_u32(&body, (uint32_t)child);
		result = EACCES;
		status = ssh_buf_error(&body);
		if (status == 0)
			status = sshd_child_ask(SSH_MON_LOGIN_RECORD, body.data, body.size, &result);
		ssh_buf_free(&body);
		if (status == 0 && result == 0)
			link->recorded = 1;
		else
			syslog(LOG_INFO, "session: no login record for %.32s", link->tty);
	}

	/* Succeeded: the child runs. */
	return 0;
}

/*
 * Runs in the new child: a session of its own (with the terminal as its
 * controlling terminal), only its three streams, signals as a new
 * program expects them, then the login shell.  Never returns.
 */
static void
session_exec(
	struct session *session,
	struct session_link *link,
	const char *command,
	char *const environment[])
{
	char *arguments[4];
	char login_name[SSH_CONFIG_PATH_MAX + 2U];
	const char *shell;
	const char *base;
	sigset_t none;

	/* Signals as a new program expects them. */
	(void)signal(SIGPIPE, SIG_DFL);
	(void)signal(SIGCHLD, SIG_DFL);
	(void)sigemptyset(&none);
	(void)sigprocmask(SIG_SETMASK, &none, NULL);

	/* A session and a process group of its own, and the terminal as its controlling terminal. */
	(void)setsid();
	if (link->have_pty) {
		(void)ioctl(link->slave, TIOCSCTTY, 0);
		(void)dup2(link->slave, 0);
		(void)dup2(link->slave, 1);
		(void)dup2(link->slave, 2);
	}

	/* Nothing but the three streams reaches the program. */
	(void)closefrom(3);

	/* The login shell: "-name" for a shell, "name -c command" for a command. */
	shell = session->account.pw_shell;
	if (shell == NULL || shell[0] == '\0')
		shell = SESSION_SHELL;
	base = strrchr(shell, '/');
	if (base == NULL)
		base = shell;
	else
		base++;
	if (command == NULL) {
		login_name[0] = '-';
		strncpy(login_name + 1, base, sizeof(login_name) - 2U);
		login_name[sizeof(login_name) - 1U] = '\0';
		arguments[0] = login_name;
		arguments[1] = NULL;
	} else {
		arguments[0] = (char *)base;
		arguments[1] = "-c";
		arguments[2] = (char *)command;
		arguments[3] = NULL;
	}

	/* Executes the shell; a failure ends the child with the status 127. */
	(void)execve(shell, arguments, environment);
	_exit(127);
}

/*
 * Builds the child's environment into environment (NULL-terminated):
 * sshd's own variables in storage, then the client's accepted ones (a
 * TERM of pty-req wins over the client's TERM).  Returns 0 or EIO.
 */
static int
session_build_environment(
	struct session *session,
	const struct session_link *link,
	char storage[][SESSION_ENV_FIXED_ENTRY],
	char **environment)
{
	const char *shell;
	size_t home_size;
	size_t shell_size;
	unsigned fixed;
	unsigned count;
	unsigned index;
	int same;

	/* The account's variables, each bounded by its room. */
	shell = session->account.pw_shell;
	if (shell == NULL || shell[0] == '\0')
		shell = SESSION_SHELL;
	home_size = strlen(session->account.pw_dir);
	shell_size = strlen(shell);
	if (home_size > SSH_CONFIG_PATH_MAX || shell_size > SSH_CONFIG_PATH_MAX)
		return EIO;
	fixed = 0;
	sprintf(storage[fixed++], "HOME=%s", session->account.pw_dir);
	sprintf(storage[fixed++], "USER=%.64s", session->account.pw_name);
	sprintf(storage[fixed++], "LOGNAME=%.64s", session->account.pw_name);
	sprintf(storage[fixed++], "SHELL=%s", shell);
	sprintf(storage[fixed++], "PATH=%s", SESSION_PATH);
	sprintf(storage[fixed++], "SSH_CONNECTION=%s", session->connection);
	sprintf(storage[fixed++], "SSH_CLIENT=%s", session->client_text);
	if (link->have_pty) {
		sprintf(storage[fixed++], "SSH_TTY=%s", link->tty);
		if (link->term[0] != '\0')
			sprintf(storage[fixed++], "TERM=%s", link->term);
	}

	/* sshd's variables first. */
	count = 0;
	for (index = 0; index < fixed; index++)
		environment[count++] = storage[index];

	/* Then the client's, without a TERM when pty-req gave one. */
	for (index = 0; index < link->env_count; index++) {
		same = strncmp(link->env[index], "TERM=", 5);
		if (same == 0 &&
		    link->have_pty &&
		    link->term[0] != '\0')
			continue;
		environment[count++] = (char *)link->env[index];
	}

	/* Ends the environment list. */
	environment[count] = NULL;

	/* Succeeded: the environment is built. */
	return 0;
}

/* Returns the local number of a signal name of RFC 4254 6.10 (without "SIG"), or 0 for another name. */
static int
session_signal_number(
	const char *name)
{
	size_t index;
	int same;

	/* Looks the name up. */
	for (index = 0; index < sizeof(session_signals) / sizeof(session_signals[0]); index++) {
		same = strcmp(session_signals[index].name, name);
		if (same == 0)
			return session_signals[index].number;
	}

	/* Not a name of the list. */
	return 0;
}

/* Returns the RFC 4254 6.10 name of a local signal number, or NULL when it has none. */
static const char *
session_signal_name(
	int number)
{
	size_t index;

	/* Looks the number up. */
	for (index = 0; index < sizeof(session_signals) / sizeof(session_signals[0]); index++) {
		if (session_signals[index].number == number)
			return session_signals[index].name;
	}

	/* A signal the list has no name for. */
	return NULL;
}

/* Sends the message built in session->message; 0 or the transport's error. */
static int
session_send(
	struct session *session)
{
	int status;

	/* Refuses a message that was not built, then queues it. */
	status = ssh_buf_error(&session->message);
	if (status == 0 && session->message.size > 0)
		status = ssh_transport_send(&session->transport, session->message.data, session->message.size);
	ssh_buf_reset(&session->message);
	if (status != 0)
		return status;

	/* Succeeded: the message is queued. */
	return 0;
}

/* Reaps every child that has ended and notes its end in its link. */
static void
session_reap(
	struct session *session)
{
	pid_t reaped;
	int status;
	unsigned index;

	/* Takes each ended child, without waiting. */
	for (;;) {
		reaped = waitpid(-1, &status, WNOHANG);
		if (reaped <= 0)
			break;
		for (index = 0; index < SSH_CHANNEL_MAX; index++) {
			if (session->links[index].channel == NULL || session->links[index].child != reaped)
				continue;
			session->links[index].exited = 1;
			session->links[index].wait_status = status;
		}
	}
}

/*
 * Writes the client's kept data to the child (as much as it takes now),
 * gives the written bytes back to the window, and closes the child's
 * input after the client's EOF once everything is written (a terminal is
 * left open: it is the child's output too).
 */
static void
session_pump(
	struct session *session,
	struct session_link *link)
{
	ssize_t written;

	/* Writes what the child takes. */
	while (link->to_child >= 0 && link->input.size > 0) {
		written = write(link->to_child, link->input.data, link->input.size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			/* A child that closed its input takes nothing more; the data is dropped. */
			if (written < 0 &&
			    errno != EAGAIN &&
			    errno != EWOULDBLOCK) {
				ssh_buf_reset(&session->message);
				ssh_channel_consumed(link->channel, link->input.size, &session->message);
				(void)session_send(session);
				ssh_buf_reset(&link->input);
				if (!link->have_pty)
					session_close_descriptor(&link->to_child);
			}

			break;
		}

		/* Drops the bytes that were written from the input. */
		ssh_buf_consume(&link->input, (size_t)written);
		ssh_buf_reset(&session->message);
		ssh_channel_consumed(link->channel, (size_t)written, &session->message);
		(void)session_send(session);
	}

	/* The client's EOF closes the input once it is all written. */
	if (link->client_eof &&
	    link->input.size == 0 &&
	    !link->have_pty)
		session_close_descriptor(&link->to_child);
}

/*
 * Reads the child's output (extended 0) or error output (the stderr
 * type) and sends it as data, no more than the channel's room.  The end
 * of a stream (an end of file, or EIO from a terminal whose last holder
 * closed it) marks it done.  Returns SESSION_READ_MORE (bytes were sent),
 * SESSION_READ_NONE (nothing now, or no room) or SESSION_READ_END.
 */
static int
session_read_child(
	struct session *session,
	struct session_link *link,
	int extended)
{
	uint8_t chunk[SESSION_READ_CHUNK];
	size_t room;
	ssize_t got;
	int descriptor;
	int status;

	/* Reads at most what the channel may send now. */
	descriptor = link->from_child;
	if (extended != 0)
		descriptor = link->err_child;
	room = ssh_channel_send_room(link->channel);
	if (room > sizeof(chunk))
		room = sizeof(chunk);
	if (descriptor < 0 || room == 0)
		return SESSION_READ_NONE;
	got = read(descriptor, chunk, room);
	if (got < 0 &&
	    (errno == EINTR ||
	     errno == EAGAIN ||
	     errno == EWOULDBLOCK))
		return SESSION_READ_NONE;
	if (got <= 0) {
		/* The stream ended. */
		if (extended != 0) {
			link->err_done = 1;
			session_close_descriptor(&link->err_child);
		} else {
			link->out_done = 1;
		}

		/* Reports the end of the stream. */
		return SESSION_READ_END;
	}

	/* Sends the bytes on the channel. */
	ssh_buf_reset(&session->message);
	status = ssh_channel_put_data(link->channel, (uint32_t)extended, chunk, (size_t)got, &session->message);
	if (status == 0)
		(void)session_send(session);
	ssh_wipe(chunk, sizeof(chunk));

	/* Succeeded: bytes went out. */
	return SESSION_READ_MORE;
}

/*
 * Ends the channel once its child has ended and all its output is sent:
 * the exit status or signal (RFC 4254 6.10), EOF and CLOSE; the
 * terminal's login record is closed.  A terminal's output is read until
 * it has nothing more (other holders of the terminal may keep it open).
 * Returns 0 or the transport's error.
 */
static int
session_finish(
	struct session *session,
	struct session_link *link)
{
	const char *name;
	size_t room;
	int signal_number;
	int status;
	int found;

	/* Waits for the child. */
	if (!link->started ||
	    !link->exited ||
	    link->finished)
		return 0;

	/* Takes what the terminal still holds; once it holds nothing, its output is done. */
	if (link->have_pty && !link->out_done) {
		do {
			found = session_read_child(session, link, 0);
		} while (found == SESSION_READ_MORE);
		room = ssh_channel_send_room(link->channel);
		if (found == SESSION_READ_NONE && room > 0)
			link->out_done = 1;
	}

	/* Waits for all of the output. */
	if (!link->out_done || !link->err_done)
		return 0;

	/* The exit status, or the signal that ended it. */
	ssh_buf_reset(&session->message);
	if (WIFSIGNALED(link->wait_status)) {
		signal_number = WTERMSIG(link->wait_status);
		name = session_signal_name(signal_number);
		if (name != NULL) {
			ssh_channel_put_exit_signal(link->channel, name, 0, &session->message);
		} else {
			/* A signal without a name of 6.10 is told as the shell tells it: 128 and the number. */
			ssh_channel_put_exit_status(link->channel, (uint32_t)(128 + signal_number), &session->message);
		}
	} else {
		ssh_channel_put_exit_status(link->channel, (uint32_t)WEXITSTATUS(link->wait_status), &session->message);
	}

	/* Sends the exit status or the signal. */
	status = session_send(session);

	/* Then EOF and CLOSE, each a message of its own. */
	if (status == 0) {
		ssh_channel_put_eof(link->channel, &session->message);
		status = session_send(session);
	}

	/* Sends CLOSE. */
	if (status == 0) {
		ssh_channel_put_close(link->channel, &session->message);
		status = session_send(session);
	}

	/* Marks the link as finished. */
	link->finished = 1;

	/* The child is gone: its streams and its record close. */
	session_release(link);
	if (status != 0)
		return status;

	/* Succeeded: the channel's end is queued. */
	return 0;
}

/*
 * Lets the link's child go: its descriptors close (a terminal hangs up),
 * its process group gets SIGHUP when it is still running, and the login
 * record of its terminal is closed.  The link stays until its channel is
 * freed.
 */
static void
session_release(
	struct session_link *link)
{
	struct ssh_buf body;
	uint32_t result;
	int status;

	/* Hangs up a child that still runs. */
	if (link->started &&
	    !link->exited &&
	    link->child > 0)
		(void)kill(-link->child, SIGHUP);

	/* Closes every descriptor of the link (the master is both of a terminal's ends). */
	if (link->to_child == link->master)
		link->to_child = -1;
	if (link->from_child == link->master)
		link->from_child = -1;
	session_close_descriptor(&link->to_child);
	session_close_descriptor(&link->from_child);
	session_close_descriptor(&link->err_child);
	session_close_descriptor(&link->master);
	session_close_descriptor(&link->slave);
	ssh_buf_free(&link->input);

	/* Closes the terminal's login record. */
	if (link->recorded) {
		ssh_buf_init(&body, 128, 0);
		ssh_buf_put_cstring(&body, link->tty);
		ssh_buf_put_u32(&body, (uint32_t)link->child);
		result = EACCES;
		status = ssh_buf_error(&body);
		if (status == 0)
			status = sshd_child_ask(SSH_MON_LOGOUT_RECORD, body.data, body.size, &result);
		ssh_buf_free(&body);
		link->recorded = 0;
	}

	/* A link whose channel is freed is unused again. */
	if (link->channel != NULL &&
	    link->channel->close_sent &&
	    link->channel->close_received) {
		link->channel->user = NULL;
		link->channel = NULL;
	}
}

/* Ends every child at the end of the connection: their terminals hang up, their records close. */
static void
session_hang_up(
	struct session *session)
{
	unsigned index;

	/* Lets each link go. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (session->links[index].channel == NULL)
			continue;
		session_release(&session->links[index]);
		session->links[index].channel = NULL;
	}
}

/* Returns the link of a channel, or NULL. */
static struct session_link *
session_link_of(
	struct session *session,
	struct ssh_channel *channel)
{
	/* The link is the one of the channel's slot, when it is the channel's. */
	if (channel == NULL || channel->local_id >= SSH_CHANNEL_MAX)
		return NULL;
	if (session->links[channel->local_id].channel != channel)
		return NULL;
	return &session->links[channel->local_id];
}

/* Closes a descriptor that is open and marks it closed. */
static void
session_close_descriptor(
	int *descriptor)
{
	/* Closes it once. */
	if (*descriptor >= 0)
		(void)close(*descriptor);
	*descriptor = -1;
}

/*
 * Keeps the client alive (ClientAliveInterval): after an interval with
 * nothing from the client, a keepalive with a reply wanted; after
 * ClientAliveCountMax of them unanswered, the connection ends.  Returns
 * 0 or ETIMEDOUT.
 */
static int
session_alive(
	struct session *session,
	uint64_t now)
{
	int status;

	/* Off, or the client spoke within the interval. */
	if (session->child.config.client_alive_interval == 0)
		return 0;
	if (now < session->last_input + session->child.config.client_alive_interval)
		return 0;

	/* Too many unanswered: the client is gone. */
	if (session->alive_missed >= session->child.config.client_alive_count) {
		syslog(LOG_INFO, "session: no answer from %.63s", session->child.peer);
		ssh_transport_disconnect(&session->transport, SSH_DISCONNECT_BY_APPLICATION, "timeout");
		return ETIMEDOUT;
	}

	/* Asks once more. */
	ssh_buf_reset(&session->message);
	ssh_channel_put_global_request(SESSION_KEEPALIVE, 1, &session->message);
	status = session_send(session);
	session->alive_missed++;
	session->last_input = now;
	if (status != 0)
		return status;

	/* Succeeded: a keepalive is queued. */
	return 0;
}

/* Returns a terminal size field as the local type holds it: the largest value for one too large. */
static unsigned short
session_clip(
	uint32_t value)
{
	/* A value past 65535 is the largest. */
	if (value > 0xffffU)
		return 0xffffU;

	/* The value as it is. */
	return (unsigned short)value;
}
