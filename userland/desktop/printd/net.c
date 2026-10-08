/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connections to the printers (plan/ws145/design.md §5.3): a host's
 * addresses tried in their order, each connection given 10 seconds, then
 * blocking sends and receives that may stand still for a minute; a
 * document is sent from its spool file a piece at a time, a job asked to
 * stop stopping between two pieces.
 *
 * A host's name is looked up by a helper thread, at most four at once, and
 * given ten seconds: a resolver that does not answer is left to its thread,
 * which frees what it found when it ends (ws177-p022).  An address written
 * as numbers is taken as it is.
 */

#include "printd.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* The piece of a document sent at once. */
#define NET_CHUNK		(64U * 1024U)

/*
 * A look up of a host's name, shared by the thread that asks and the
 * helper thread that looks: the name and the service, what was found and
 * the resolver's answer, whether it is done, and how many of the two
 * threads still hold it (the last one frees it, and the addresses when
 * the asker gave up).  net_lock guards done, the result and holders.
 */
struct net_lookup {
	char host[PD_HOST_MAX];
	char service[16];
	struct addrinfo *found;
	int status;
	int done;
	int holders;
	pthread_cond_t ready;
};

/*
 * The look ups' lock, the condition a place frees on, and the look ups
 * being made (at most PD_LOOKUPS_MAX; a helper that outlived its asker
 * still counts until it ends).
 */
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t net_place = PTHREAD_COND_INITIALIZER;
static unsigned net_lookups;

static int net_connect_one(const struct addrinfo *address, int *fd);
static int net_resolve(const char *host, const char *service, struct addrinfo **found, const char **detail);
static void *net_lookup_thread(void *argument);
static void net_lookup_release(struct net_lookup *lookup);
static void net_deadline(struct timespec *deadline, unsigned seconds);

/*
 * Connects to a printer.  Returns 0 with the socket, or an errno value
 * with the word of the failure (unreachable, refused, timeout).
 */
int
pd_connect(
	const char *host,
	unsigned port,
	int *fd,
	const char **detail)
{
	struct addrinfo *found;
	struct addrinfo *address;
	struct timeval wait;
	char service[16];
	int status;
	int error;

	/* The host's addresses for a stream, looked up within ten seconds. */
	*fd = -1;
	*detail = "unreachable";
	(void)snprintf(service, sizeof(service), "%u", port);
	status = net_resolve(host, service, &found, detail);
	if (status != 0)
		return status;

	/* The first that takes the connection. */
	error = EHOSTUNREACH;
	for (address = found; address != NULL; address = address->ai_next) {
		error = net_connect_one(address, fd);
		if (error == 0)
			break;
	}

	/* The addresses are not needed after; a failure's word. */
	freeaddrinfo(found);
	if (error != 0) {
		if (error == ECONNREFUSED)
			*detail = "refused";
		else if (error == ETIMEDOUT)
			*detail = "timeout";
		return error;
	}

	/* Blocking from here, a minute standing still at most. */
	wait.tv_sec = PD_IDLE_SECONDS;
	wait.tv_usec = 0;
	(void)setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
	(void)setsockopt(*fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
	*detail = "";
	return 0;
}

/*
 * Writes all the bytes of a buffer.  Returns 0, or an errno value.
 */
int
pd_write_all(
	int fd,
	const void *data,
	size_t size)
{
	const unsigned char *cursor;
	ssize_t sent;

	/* Until every byte is out. */
	cursor = data;
	while (size > 0U) {
		sent = send(fd, cursor, size, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0)
			return errno;
		if (sent == 0)
			return EIO;
		cursor += sent;
		size -= (size_t)sent;
	}

	/* Every byte is out. */
	return 0;
}

/*
 * Sends a job's spool file.  Returns 0, ECANCELED when the job was asked
 * to stop meanwhile, or an errno value.
 */
int
pd_send_file(
	int fd,
	struct pd_job *job)
{
	unsigned char *chunk;
	uint64_t done;
	ssize_t got;
	int file;
	int error;
	int stop;

	/* The file and a piece's room. */
	file = open(job->file, O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return errno;
	chunk = malloc(NET_CHUNK);
	if (chunk == NULL) {
		(void)close(file);
		return ENOMEM;
	}

	/* Piece by piece, as many bytes as were copied. */
	done = 0;
	error = 0;
	while (done < job->size) {
		/* A job asked to stop stops between two pieces. */
		stop = pd_cancelled(job);
		if (stop) {
			error = ECANCELED;
			break;
		}

		/* The next piece. */
		got = read(file, chunk, NET_CHUNK);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			error = EIO;
			break;
		}

		/* Sent. */
		error = pd_write_all(fd, chunk, (size_t)got);
		if (error != 0)
			break;
		done += (uint64_t)got;
	}

	/* The file and the room go. */
	free(chunk);
	(void)close(file);
	return error;
}

/*
 * Reads what is there (waiting up to the socket's time out).  Returns the
 * count, 0 at the end, or -1.
 */
ssize_t
pd_read_some(
	int fd,
	void *data,
	size_t size)
{
	ssize_t got;

	/* One read, again when interrupted. */
	for (;;) {
		got = recv(fd, data, size, 0);
		if (got < 0 && errno == EINTR)
			continue;
		return got;
	}
}

/*
 * Finds a host's addresses: one written as numbers at once, a name by a
 * helper thread within PD_LOOKUP_SECONDS (waiting for one of the
 * PD_LOOKUPS_MAX places counts in it).  Returns 0 with the addresses (the
 * caller frees them), or an errno value with the word of the failure.
 */
static int
net_resolve(
	const char *host,
	const char *service,
	struct addrinfo **found,
	const char **detail)
{
	struct addrinfo hints;
	struct net_lookup *lookup;
	struct timespec deadline;
	pthread_t thread;
	int status;
	int waited;

	/* An address written as numbers needs no resolver. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;
	status = getaddrinfo(host, service, &hints, found);
	if (status == 0)
		return 0;

	/* The look up, shared with its helper. */
	lookup = calloc(1, sizeof(*lookup));
	if (lookup == NULL) {
		*detail = "io";
		return ENOMEM;
	}

	/* The name and the service, and both threads holding it. */
	(void)snprintf(lookup->host, sizeof(lookup->host), "%s", host);
	(void)snprintf(lookup->service, sizeof(lookup->service), "%s", service);
	(void)pthread_cond_init(&lookup->ready, NULL);
	lookup->holders = 2;

	/* A place among the look ups, then the helper, all within the time. */
	net_deadline(&deadline, PD_LOOKUP_SECONDS);
	(void)pthread_mutex_lock(&net_lock);

	waited = 0;
	while (net_lookups >= PD_LOOKUPS_MAX && waited == 0)
		waited = pthread_cond_timedwait(&net_place, &net_lock, &deadline);
	if (net_lookups >= PD_LOOKUPS_MAX) {
		(void)pthread_mutex_unlock(&net_lock);
		(void)pthread_cond_destroy(&lookup->ready);
		free(lookup);
		*detail = "timeout";
		return ETIMEDOUT;
	}

	/* A place taken. */
	net_lookups++;

	(void)pthread_mutex_unlock(&net_lock);

	/* The helper looks; it ends by itself. */
	status = pthread_create(&thread, NULL, net_lookup_thread, lookup);
	if (status != 0) {
		(void)pthread_mutex_lock(&net_lock);

		net_lookups--;
		(void)pthread_cond_signal(&net_place);

		(void)pthread_mutex_unlock(&net_lock);
		(void)pthread_cond_destroy(&lookup->ready);
		free(lookup);
		*detail = "io";
		return status;
	}

	/* Not joined: the helper ends by itself. */
	(void)pthread_detach(thread);

	/* Its answer, waited for until the time is up. */
	(void)pthread_mutex_lock(&net_lock);

	waited = 0;
	while (!lookup->done && waited == 0)
		waited = pthread_cond_timedwait(&lookup->ready, &net_lock, &deadline);
	status = lookup->status;
	*found = NULL;
	if (lookup->done && status == 0) {
		*found = lookup->found;
		lookup->found = NULL;
	}

	/* Whether the answer came in time. */
	waited = lookup->done;

	(void)pthread_mutex_unlock(&net_lock);

	/* This thread lets the look up go; the helper frees it when it is last. */
	net_lookup_release(lookup);

	/* No answer in time. */
	if (!waited) {
		*detail = "timeout";
		return ETIMEDOUT;
	}

	/* The name has no address. */
	if (status != 0) {
		*detail = "unreachable";
		return EHOSTUNREACH;
	}

	/* Succeeded: the addresses. */
	return 0;
}

/* A look up's helper: asks the resolver, gives the answer and its place up. */
static void *
net_lookup_thread(
	void *argument)
{
	struct net_lookup *lookup;
	struct addrinfo hints;
	struct addrinfo *found;
	int status;

	/* The resolver, which may take long. */
	lookup = argument;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	found = NULL;
	status = getaddrinfo(lookup->host, lookup->service, &hints, &found);

	/* The answer, and the place among the look ups free again. */
	(void)pthread_mutex_lock(&net_lock);

	lookup->status = status;
	lookup->found = found;
	lookup->done = 1;
	(void)pthread_cond_signal(&lookup->ready);
	net_lookups--;
	(void)pthread_cond_signal(&net_place);

	(void)pthread_mutex_unlock(&net_lock);

	/* This thread lets the look up go. */
	net_lookup_release(lookup);
	return NULL;
}

/* Lets a look up go; the last of its two threads frees it and what it found and nobody took. */
static void
net_lookup_release(
	struct net_lookup *lookup)
{
	int last;

	/* One holder less. */
	(void)pthread_mutex_lock(&net_lock);

	lookup->holders--;
	last = lookup->holders == 0;

	(void)pthread_mutex_unlock(&net_lock);

	/* Others hold it still. */
	if (!last)
		return;

	/* The last: the addresses nobody took, and the look up. */
	if (lookup->found != NULL)
		freeaddrinfo(lookup->found);
	(void)pthread_cond_destroy(&lookup->ready);
	free(lookup);
}

/* The time a number of seconds from now, on the clock the conditions wait by. */
static void
net_deadline(
	struct timespec *deadline,
	unsigned seconds)
{
	/* Now, and the seconds after. */
	(void)clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_sec += (time_t)seconds;
}

/* Connects to one address within the connection's time; 0 or an errno value. */
static int
net_connect_one(
	const struct addrinfo *address,
	int *fd)
{
	struct pollfd poller;
	socklen_t length;
	int flags;
	int status;
	int error;
	int asked;
	int made;

	/* A socket that does not block while it connects. */
	made = socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
	if (made < 0)
		return errno;
	flags = fcntl(made, F_GETFL);
	(void)fcntl(made, F_SETFL, flags | O_NONBLOCK);

	/* The connection, waited for. */
	status = connect(made, address->ai_addr, address->ai_addrlen);
	error = 0;
	if (status != 0 && errno != EINPROGRESS)
		error = errno;
	if (status != 0 && error == 0) {
		poller.fd = made;
		poller.events = POLLOUT;
		poller.revents = 0;
		status = poll(&poller, 1, PD_CONNECT_SECONDS * 1000);
		if (status == 0)
			error = ETIMEDOUT;
		else if (status < 0)
			error = errno;
		else {
			length = sizeof(error);
			asked = getsockopt(made, SOL_SOCKET, SO_ERROR, &error, &length);
			if (asked != 0)
				error = errno;
		}
	}

	/* Not connected. */
	if (error != 0) {
		(void)close(made);
		return error;
	}

	/* Connected: blocking again. */
	(void)fcntl(made, F_SETFL, flags);
	*fd = made;
	return 0;
}
