/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * keiland-printd's main thread (printd.h; plan/ws145/design.md §5.1): it
 * makes its spool and says where (SPOOL), then reads the backend's
 * commands from descriptor 3, a line each with the documents' descriptors
 * beside them (SCM_RIGHTS, taken in their order by the JOB lines):
 *
 *   JOB <job> <ipp|lpd> <host> <port> <path-or-queue> <title>   a document to print
 *   CANCEL <job>                                                stop a job
 *   NAME <seq> <host> <port>                                    ask an IPP printer its name
 *   BYE <n>                                                     end, when n commands were sent
 *
 * and answers ACCEPTED or REJECTED for each job (after the copy into the
 * spool), STATE lines as its threads send them, PATH, NAMED, and IDLE <n>
 * once nothing was to do for a minute.  The end of descriptor 3 ends the
 * daemon at once: the jobs are stopped and the spool removed.  So does a
 * line it cannot take for a command, or a JOB without its descriptor: the
 * backend starts another daemon (ws177-p022).
 *
 * The jobs accepted wait in the spool for their turn: one is sent at a time
 * to each printer and four in all; an IPP job the printer took gives its
 * place up while it is watched (ws177-p022, design §5.3).
 */

#include "printd.h"

#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* The backend's socket. */
#define PD_CONTROL		3

/* How long nothing to do makes the daemon say it is idle (seconds), and how often it looks (ms). */
#define PD_IDLE_AFTER		60
#define PD_TICK_MS		1000

/* The most descriptors waiting for their JOB lines. */
#define PD_FDS_MAX		16U

/*
 * The daemon: its spool, the jobs and the order the next one accepted
 * takes, the descriptors received and not yet taken, the bytes of a line
 * not yet ended, the commands received, the spool's bytes in use, the
 * user's and the host's names, the seed of the LPD jobs' numbers, when it
 * last had something to do and whether it said it is idle; the lock of the
 * socket's writes and of the jobs' flags.
 */
struct pd_daemon {
	char spool[512];
	struct pd_job jobs[PD_JOBS_MAX];
	uint64_t next_order;
	int fds[PD_FDS_MAX];
	size_t fd_count;
	char line[PD_LINE_MAX + 1];
	size_t line_length;
	unsigned long commands;
	uint64_t used;
	char user[64];
	char host[64];
	unsigned seed;
	time_t active;
	int idle_told;
	pthread_mutex_t lock;
};

/* The daemon of the program. */
static struct pd_daemon daemon_state;

int main(int argc, char **argv);
static int pd_read(void);
static int pd_command(char *line);
static int pd_job(char *fields);
static void pd_cancel(char *fields);
static void pd_name(char *fields);
static int pd_bye(char *fields);
static void *pd_job_thread(void *argument);
static void *pd_name_thread(void *argument);
static void pd_reap(void);
static void pd_schedule(void);
static int pd_place_free(const struct pd_job *job, unsigned *sending);
static void pd_start_job(struct pd_job *job);
static int pd_take_fd(void);
static int pd_busy(void);
static void pd_stop(void);
static int pd_word(char **cursor, char *word, size_t size);
static int pd_number(const char *text, unsigned long most, unsigned long *number);
static int pd_clean(const char *text);
static void pd_names(void);

/*
 * Runs the daemon until the backend ends it.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd poller;
	time_t now;
	int status;
	int ready;
	int busy;

	/* Nothing on the command line; no descriptor but the backend's goes on. */
	(void)argc;
	(void)argv;
	(void)closefrom(PD_CONTROL + 1);
	(void)signal(SIGPIPE, SIG_IGN);
	openlog("keiland-printd", LOG_PID, LOG_USER);
	(void)pthread_mutex_init(&daemon_state.lock, NULL);
	pd_names();
	daemon_state.seed = (unsigned)time(NULL) % 1000U;
	daemon_state.active = time(NULL);

	/* The spool, said to the backend; none ends the daemon. */
	status = pd_spool_open(daemon_state.spool, sizeof(daemon_state.spool));
	if (status != 0) {
		pd_send("FATAL runtime");
		return 1;
	}

	/* Where it is, for the backend to remove after a crash. */
	pd_send("SPOOL %s", daemon_state.spool);

	/* Each command, until the backend's socket ends. */
	for (;;) {
		poller.fd = PD_CONTROL;
		poller.events = POLLIN;
		poller.revents = 0;
		ready = poll(&poller, 1, PD_TICK_MS);
		if (ready < 0 && errno != EINTR)
			break;

		/* The commands that came; the end of the socket ends the daemon. */
		if (ready > 0) {
			status = pd_read();
			if (status != 0)
				break;
		}

		/* The jobs that ended, those whose turn came, and a minute with nothing to do. */
		pd_reap();
		pd_schedule();
		busy = pd_busy();
		now = time(NULL);
		if (!busy && !daemon_state.idle_told && now - daemon_state.active >= PD_IDLE_AFTER) {
			daemon_state.idle_told = 1;
			pd_send("IDLE %lu", daemon_state.commands);
		}
	}

	/* Stopped: the jobs, the spool. */
	pd_stop();
	return 0;
}

/*
 * Writes a line to the backend (its line feed added), under the lock so
 * that the threads' lines do not mix.
 */
void
pd_send(
	const char *format,
	...)
{
	va_list arguments;
	char line[PD_LINE_MAX + 1];
	size_t done;
	ssize_t sent;
	int length;

	/* The line. */
	va_start(arguments, format);
	length = vsnprintf(line, sizeof(line) - 1U, format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length > sizeof(line) - 2U)
		length = (int)(sizeof(line) - 2U);
	line[length] = '\n';
	length++;

	/* Written whole, the threads waiting. */
	(void)pthread_mutex_lock(&daemon_state.lock);
	done = 0;
	while (done < (size_t)length) {
		sent = send(PD_CONTROL, line + done, (size_t)length - done, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent <= 0)
			break;
		done += (size_t)sent;
	}

	/* The others may write. */
	(void)pthread_mutex_unlock(&daemon_state.lock);
}

/* Tells whether a job was asked to stop. */
int
pd_cancelled(
	struct pd_job *job)
{
	int cancel;

	/* Read under the lock. */
	(void)pthread_mutex_lock(&daemon_state.lock);
	cancel = job->cancel;
	(void)pthread_mutex_unlock(&daemon_state.lock);
	return cancel;
}

/*
 * Gives a job's sending place up: the printer took it (an IPP job watched
 * from here), so that the next job for the printer may start.
 */
void
pd_sent(
	struct pd_job *job)
{
	/* The place, under the lock the main thread counts with. */
	(void)pthread_mutex_lock(&daemon_state.lock);

	job->sending = 0;

	(void)pthread_mutex_unlock(&daemon_state.lock);
}

/*
 * Waits a number of seconds, a second at a time, and stops early when the
 * job is asked to stop.  Returns 1 when it was asked to stop, 0 after the
 * wait.
 */
int
pd_wait(
	struct pd_job *job,
	unsigned seconds)
{
	unsigned waited;
	int stop;

	/* A second at a time, looking at the job between. */
	for (waited = 0; waited < seconds; waited++) {
		stop = pd_cancelled(job);
		if (stop)
			return 1;
		sleep(1);
	}

	/* The last look after the wait. */
	stop = pd_cancelled(job);
	if (stop)
		return 1;

	/* Succeeded: the whole wait went. */
	return 0;
}

/* The user's login name, sent to the printers (IPP's requesting-user-name, LPD's P). */
const char *
pd_user(void)
{
	/* Found at the start. */
	return daemon_state.user;
}

/* This host's name, for LPD's files. */
const char *
pd_host_name(void)
{
	/* Found at the start. */
	return daemon_state.host;
}

/* Writes a line to the system's log (job numbers and results only). */
void
pd_log(
	const char *format,
	...)
{
	va_list arguments;

	/* To syslog. */
	va_start(arguments, format);
	vsyslog(LOG_INFO, format, arguments);
	va_end(arguments);
}

/*
 * Reads what the backend sent: bytes into the line, descriptors into the
 * FIFO; each whole line is a command.  Returns nonzero at the end of the
 * socket or when it broke the protocol.
 */
static int
pd_read(void)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int) * PD_FDS_MAX)];
	} control;
	struct cmsghdr *message;
	struct msghdr header;
	struct iovec vector;
	char bytes[PD_LINE_MAX];
	const int *descriptors;
	ssize_t got;
	size_t count;
	size_t index;
	char *end;
	int same;
	int ending;

	/* One read, the descriptors with it. */
	memset(&header, 0, sizeof(header));
	vector.iov_base = bytes;
	vector.iov_len = sizeof(bytes);
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = sizeof(control.space);
	got = recvmsg(PD_CONTROL, &header, MSG_CMSG_CLOEXEC);
	if (got < 0 && errno == EINTR)
		return 0;
	if (got <= 0 || (header.msg_flags & MSG_CTRUNC) != 0)
		return 1;

	/* The descriptors, in their order. */
	for (message = CMSG_FIRSTHDR(&header); message != NULL; message = CMSG_NXTHDR(&header, message)) {
		if (message->cmsg_level != SOL_SOCKET || message->cmsg_type != SCM_RIGHTS)
			continue;
		count = (message->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		descriptors = (const int *)(const void *)CMSG_DATA(message);
		for (index = 0; index < count; index++) {
			if (daemon_state.fd_count == PD_FDS_MAX) {
				(void)close(descriptors[index]);
				return 1;
			}

			/* Kept for its JOB line. */
			daemon_state.fds[daemon_state.fd_count] = descriptors[index];
			daemon_state.fd_count++;
		}
	}

	/* The bytes, line by line. */
	for (index = 0; index < (size_t)got; index++) {
		if (daemon_state.line_length == PD_LINE_MAX)
			return 1;
		daemon_state.line[daemon_state.line_length] = bytes[index];
		daemon_state.line_length++;
		if (bytes[index] != '\n')
			continue;

		/* A whole line. */
		daemon_state.line[daemon_state.line_length - 1U] = '\0';
		daemon_state.line_length = 0;
		end = strchr(daemon_state.line, '\r');
		if (end != NULL)
			*end = '\0';

		/* BYE is not counted; the others are commands. */
		same = strncmp(daemon_state.line, "BYE ", 4U);
		if (same == 0) {
			ending = pd_bye(daemon_state.line + 4);
			if (ending)
				return 1;
			continue;
		}

		/* A command; one the daemon cannot take ends it. */
		ending = pd_command(daemon_state.line);
		if (ending)
			return 1;
	}

	/* The socket goes on. */
	return 0;
}

/*
 * Carries out one command.  Returns 1 when the line breaks the protocol
 * (a word that is no command, a JOB without its descriptor): the daemon
 * then ends, and the backend starts another.
 */
static int
pd_command(
	char *line)
{
	int ending;
	int job;
	int cancel;
	int name;

	/* Counted (BYE compares), and something to do. */
	daemon_state.commands++;
	daemon_state.active = time(NULL);
	daemon_state.idle_told = 0;

	/* By its word. */
	job = strncmp(line, "JOB ", 4U);
	cancel = strncmp(line, "CANCEL ", 7U);
	name = strncmp(line, "NAME ", 5U);
	ending = 0;
	if (job == 0) {
		ending = pd_job(line + 4);
	} else if (cancel == 0) {
		pd_cancel(line + 7);
	} else if (name == 0) {
		pd_name(line + 5);
	} else {
		/* A word that is no command: the two ends do not agree any more. */
		pd_log("command not known");
		ending = 1;
	}

	/* Reports whether the daemon is to end. */
	if (ending)
		return 1;

	/* Succeeded: the command was carried out. */
	return 0;
}

/*
 * A job: its document's descriptor taken from the FIFO, its fields
 * checked, the document copied into the spool (ACCEPTED or REJECTED); it
 * waits there for its turn (pd_schedule).  Returns 1 when no descriptor
 * came with it (the daemon then ends).
 */
static int
pd_job(
	char *fields)
{
	struct pd_job *job;
	unsigned long number;
	unsigned long port;
	const char *detail;
	char word[PD_PATH_MAX];
	char protocol[8];
	size_t title_length;
	size_t index;
	int status;
	int ipp;
	int lpd;
	int fd;

	/* The document, which every JOB line takes; a JOB without one breaks the protocol. */
	fd = pd_take_fd();
	if (fd < 0) {
		pd_log("job without its document");
		return 1;
	}

	/* The job's number. */
	status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 0xffffffffUL, &number);
	if (status != 0 || number == 0UL) {
		(void)close(fd);
		pd_log("job line not readable");
		return 0;
	}

	/* A free slot. */
	job = NULL;
	for (index = 0; index < PD_JOBS_MAX; index++) {
		if (daemon_state.jobs[index].job == 0U) {
			job = &daemon_state.jobs[index];
			break;
		}
	}

	/* None free: busy. */
	if (job == NULL) {
		(void)close(fd);
		pd_send("REJECTED %lu busy", number);
		return 0;
	}

	/* The printer: protocol, host, port, path or queue; the title is the rest of the line. */
	memset(job, 0, sizeof(*job));
	port = 0;
	job->job = (uint32_t)number;
	status = pd_word(&fields, protocol, sizeof(protocol));
	if (status == 0)
		status = pd_word(&fields, job->host, sizeof(job->host));
	if (status == 0)
		status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 65535UL, &port);
	if (status == 0)
		status = pd_word(&fields, job->path, sizeof(job->path));
	title_length = strlen(fields);
	if (status == 0 && title_length >= sizeof(job->title))
		status = EINVAL;
	if (status == 0) {
		(void)snprintf(job->title, sizeof(job->title), "%s", fields);
		status = pd_clean(job->title);
	}

	/* The protocol by its word. */
	job->port = (unsigned)port;
	job->protocol = 0;
	ipp = strcmp(protocol, "ipp");
	lpd = strcmp(protocol, "lpd");
	if (ipp == 0)
		job->protocol = PD_IPP;
	else if (lpd == 0)
		job->protocol = PD_LPD;
	if (status != 0 || job->protocol == 0 || job->port == 0U) {
		(void)close(fd);
		memset(job, 0, sizeof(*job));
		pd_send("REJECTED %lu protocol", number);
		return 0;
	}

	/* The document into the spool. */
	status = pd_spool_copy(daemon_state.spool, job, fd, daemon_state.used, &detail);
	(void)close(fd);
	if (status != 0) {
		pd_send("REJECTED %lu %s", number, detail);
		memset(job, 0, sizeof(*job));
		return 0;
	}

	/* Accepted: its bytes counted, its place in the order taken. */
	daemon_state.used += job->size;
	job->order = daemon_state.next_order;
	daemon_state.next_order++;
	pd_send("ACCEPTED %lu", number);
	pd_log("job %lu accepted", number);

	/* Sent when its turn comes, after ACCEPTED is written. */
	pd_schedule();
	return 0;
}

/*
 * Starts the jobs whose turn came, oldest first: one at a time for each
 * printer, PD_SENDING_MAX in all.
 */
static void
pd_schedule(void)
{
	struct pd_job *oldest;
	struct pd_job *job;
	unsigned sending;
	size_t index;
	int free_place;

	/* Until no waiting job may start. */
	for (;;) {
		/* The oldest waiting job whose printer has no job being sent. */
		oldest = NULL;
		sending = 0U;
		for (index = 0; index < PD_JOBS_MAX; index++) {
			job = &daemon_state.jobs[index];
			if (job->job == 0U || job->started)
				continue;
			free_place = pd_place_free(job, &sending);
			if (!free_place)
				continue;
			if (oldest == NULL || job->order < oldest->order)
				oldest = job;
		}

		/* None, or every place taken. */
		if (oldest == NULL || sending >= PD_SENDING_MAX)
			return;

		/* Its thread. */
		pd_start_job(oldest);
	}
}

/*
 * Tells whether a job's printer has no job being sent, and counts the
 * jobs being sent in all.
 */
static int
pd_place_free(
	const struct pd_job *job,
	unsigned *sending)
{
	const struct pd_job *other;
	size_t index;
	int same_host;
	int free_place;

	/* The jobs holding a place, under the lock their threads change it with. */
	free_place = 1;
	*sending = 0U;
	(void)pthread_mutex_lock(&daemon_state.lock);

	for (index = 0; index < PD_JOBS_MAX; index++) {
		other = &daemon_state.jobs[index];
		if (other->job == 0U || !other->sending)
			continue;
		(*sending)++;
		same_host = strcmp(other->host, job->host);
		if (same_host == 0 &&
		    other->port == job->port &&
		    other->protocol == job->protocol)
			free_place = 0;
	}

	(void)pthread_mutex_unlock(&daemon_state.lock);

	/* Succeeded: whether the printer is free. */
	return free_place;
}

/* Starts a job's thread, its place taken; a thread that cannot start fails the job. */
static void
pd_start_job(
	struct pd_job *job)
{
	int status;

	/* The place, taken before the thread runs. */
	(void)pthread_mutex_lock(&daemon_state.lock);

	job->sending = 1;

	(void)pthread_mutex_unlock(&daemon_state.lock);

	/* Its own thread sends it. */
	job->started = 1;
	status = pthread_create(&job->thread, NULL, pd_job_thread, job);
	if (status != 0) {
		pd_send("STATE %lu failed io", (unsigned long)job->job);
		(void)unlink(job->file);
		daemon_state.used -= job->size;
		memset(job, 0, sizeof(*job));
		return;
	}
}

/* Asks a job to stop; one not known is told as cancelled. */
static void
pd_cancel(
	char *fields)
{
	struct pd_job *job;
	unsigned long number;
	char word[16];
	size_t index;
	int status;

	/* The job's number. */
	status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 0xffffffffUL, &number);
	if (status != 0)
		return;

	/* The job: one waiting for its turn ends at once, a thread's stops at its next step. */
	for (index = 0; index < PD_JOBS_MAX; index++) {
		job = &daemon_state.jobs[index];
		if (job->job != (uint32_t)number)
			continue;

		/* Not started: its spool file goes, and it is cancelled. */
		if (!job->started) {
			(void)unlink(job->file);
			daemon_state.used -= job->size;
			memset(job, 0, sizeof(*job));
			pd_send("STATE %lu cancelled", number);
			return;
		}

		/* Flagged for its thread. */
		(void)pthread_mutex_lock(&daemon_state.lock);

		job->cancel = 1;

		(void)pthread_mutex_unlock(&daemon_state.lock);
		return;
	}

	/* Not known. */
	pd_send("STATE %lu cancelled unknown", number);
}

/* Asks an IPP printer for its name, on a thread of its own. */
static void
pd_name(
	char *fields)
{
	struct pd_name *name;
	pthread_t thread;
	unsigned long seq;
	unsigned long port;
	char word[16];
	int status;

	/* The question. */
	name = calloc(1, sizeof(*name));
	if (name == NULL)
		return;
	status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 0xffffffffUL, &seq);
	if (status == 0)
		status = pd_word(&fields, name->host, sizeof(name->host));
	if (status == 0)
		status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 65535UL, &port);
	if (status != 0 || port == 0UL) {
		free(name);
		return;
	}

	/* Its numbers. */
	name->seq = (uint32_t)seq;
	name->port = (unsigned)port;

	/* Its thread answers NAMED. */
	status = pthread_create(&thread, NULL, pd_name_thread, name);
	if (status != 0) {
		pd_send("NAMED %lu", seq);
		free(name);
		return;
	}

	/* Not joined: it ends by itself. */
	(void)pthread_detach(thread);
}

/* The backend's BYE: the daemon ends when the backend sent as many commands as it read. Returns 1 to end. */
static int
pd_bye(
	char *fields)
{
	unsigned long count;
	char word[24];
	int status;
	int busy;

	/* The count, which must be the commands read. */
	status = pd_word(&fields, word, sizeof(word));
	if (status == 0)
		status = pd_number(word, 0xffffffffUL, &count);
	if (status != 0)
		return 0;

	/* Only when every command was read and nothing is held. */
	busy = pd_busy();
	if (count != daemon_state.commands || busy)
		return 0;
	return 1;
}

/* A job's thread: sends it by its protocol, then removes its spool file. */
static void *
pd_job_thread(
	void *argument)
{
	struct pd_job *job;

	/* By its protocol. */
	job = argument;
	if (job->protocol == PD_IPP)
		pd_ipp_job(job);
	else
		pd_lpd_job(job, (daemon_state.seed + job->job) % 1000U);

	/* The spool file goes; the main thread reaps the job, its place free. */
	(void)unlink(job->file);
	(void)pthread_mutex_lock(&daemon_state.lock);

	job->sending = 0;
	job->ended = 1;

	(void)pthread_mutex_unlock(&daemon_state.lock);
	return NULL;
}

/* A NAME's thread. */
static void *
pd_name_thread(
	void *argument)
{
	struct pd_name *name;

	/* Asked and answered. */
	name = argument;
	pd_ipp_name(name);
	free(name);
	return NULL;
}

/* Joins the jobs' threads that ended and frees their slots and spool. */
static void
pd_reap(void)
{
	struct pd_job *job;
	size_t index;
	int ended;

	/* Each job. */
	for (index = 0; index < PD_JOBS_MAX; index++) {
		job = &daemon_state.jobs[index];
		if (job->job == 0U || !job->started)
			continue;
		(void)pthread_mutex_lock(&daemon_state.lock);
		ended = job->ended;
		(void)pthread_mutex_unlock(&daemon_state.lock);
		if (!ended)
			continue;

		/* Joined; its bytes and slot are free. */
		(void)pthread_join(job->thread, NULL);
		daemon_state.used -= job->size;
		memset(job, 0, sizeof(*job));
		daemon_state.active = time(NULL);
	}
}

/* Takes the oldest descriptor received (-1 for none). */
static int
pd_take_fd(void)
{
	size_t index;
	int fd;

	/* None. */
	if (daemon_state.fd_count == 0U)
		return -1;

	/* The first, the rest moved up. */
	fd = daemon_state.fds[0];
	for (index = 1; index < daemon_state.fd_count; index++)
		daemon_state.fds[index - 1U] = daemon_state.fds[index];
	daemon_state.fd_count--;
	return fd;
}

/* Tells whether a job is held. */
static int
pd_busy(void)
{
	size_t index;

	/* Any slot in use. */
	for (index = 0; index < PD_JOBS_MAX; index++) {
		if (daemon_state.jobs[index].job != 0U)
			return 1;
	}

	/* None. */
	return 0;
}

/* Stops every job, waits for their threads and removes the spool. */
static void
pd_stop(void)
{
	size_t index;

	/* Each job asked to stop. */
	(void)pthread_mutex_lock(&daemon_state.lock);
	for (index = 0; index < PD_JOBS_MAX; index++)
		daemon_state.jobs[index].cancel = 1;
	(void)pthread_mutex_unlock(&daemon_state.lock);

	/* Their threads. */
	for (index = 0; index < PD_JOBS_MAX; index++) {
		if (daemon_state.jobs[index].job != 0U && daemon_state.jobs[index].started)
			(void)pthread_join(daemon_state.jobs[index].thread, NULL);
	}

	/* The descriptors not taken, and the spool. */
	while (daemon_state.fd_count > 0U)
		(void)close(pd_take_fd());
	pd_spool_close(daemon_state.spool);
}

/* Takes the next word of a line (up to a space) into a buffer; EINVAL for none or one too long. */
static int
pd_word(
	char **cursor,
	char *word,
	size_t size)
{
	size_t length;
	char *space;

	/* Up to the next space or the end. */
	space = strchr(*cursor, ' ');
	if (space == NULL)
		length = strlen(*cursor);
	else
		length = (size_t)(space - *cursor);
	if (length == 0U || length >= size)
		return EINVAL;

	/* The word, and the cursor after its space. */
	memcpy(word, *cursor, length);
	word[length] = '\0';
	*cursor += length;
	if (**cursor == ' ')
		(*cursor)++;
	return pd_clean(word);
}

/* Reads a decimal number not above a most; EINVAL for anything else. */
static int
pd_number(
	const char *text,
	unsigned long most,
	unsigned long *number)
{
	unsigned long value;

	/* Digits only. */
	if (*text == '\0')
		return EINVAL;
	value = 0;
	for (; *text != '\0'; text++) {
		if (*text < '0' || *text > '9')
			return EINVAL;
		value = value * 10UL + (unsigned long)(*text - '0');
		if (value > most)
			return EINVAL;
	}

	/* Read. */
	*number = value;
	return 0;
}

/* Refuses a text with a control character (C0, DEL; C1 comes as UTF-8 and is checked by the backend). */
static int
pd_clean(
	const char *text)
{
	const unsigned char *byte;

	/* Each byte. */
	for (byte = (const unsigned char *)text; *byte != '\0'; byte++) {
		if (*byte < 0x20U || *byte == 0x7fU)
			return EINVAL;
	}

	/* Clean. */
	return 0;
}

/* Finds the user's login name and this host's name (letters, digits, dots and hyphens). */
static void
pd_names(void)
{
	struct passwd *account;
	size_t index;
	char *byte;
	int status;

	/* The user. */
	account = getpwuid(getuid());
	(void)snprintf(daemon_state.user, sizeof(daemon_state.user), "user");
	if (account != NULL)
		(void)snprintf(daemon_state.user, sizeof(daemon_state.user), "%s", account->pw_name);

	/* The host, its other characters dropped. */
	status = gethostname(daemon_state.host, sizeof(daemon_state.host) - 1U);
	if (status != 0)
		(void)snprintf(daemon_state.host, sizeof(daemon_state.host), "keiland");
	daemon_state.host[31] = '\0';
	index = 0;
	for (byte = daemon_state.host; *byte != '\0'; byte++) {
		if ((*byte >= 'a' && *byte <= 'z') || (*byte >= 'A' && *byte <= 'Z') || (*byte >= '0' && *byte <= '9') ||
		    *byte == '.' || *byte == '-') {
			daemon_state.host[index] = *byte;
			index++;
		}
	}

	/* None left: a name of its own. */
	daemon_state.host[index] = '\0';
	if (index == 0U)
		(void)snprintf(daemon_state.host, sizeof(daemon_state.host), "keiland");
}
