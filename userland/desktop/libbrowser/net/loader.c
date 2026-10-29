/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The asynchronous loader: http and https
 * requests that never block the caller.  The loader runs them on a
 * network thread of its own; the caller hears that there is news through
 * its notify function (or waits for it, net_loader_wait) and takes it with
 * net_loader_dispatch, which calls each ended request's callback on the
 * caller's thread.  The caller sees no descriptor.
 *
 * The threads, each loader's own:
 *
 *   browser-net       one: polls the connections, the resolver's pipe, a
 *                     control pipe the caller wakes it through, and the
 *                     descriptors watched for the caller (a media engine's
 *                     wake, read empty when it rings); runs every request's
 *                     states, the pool and the cache, and the time outs.
 *   browser-resolve   one: getaddrinfo, which blocks; the answer comes back
 *                     through its pipe.
 *
 * A request's states: its host's addresses looked up on the resolver's
 * thread, a non-blocking connect
 * to each address in turn, the TLS handshake for https, the request sent,
 * and the response read until its framing (its length, its last chunk, or
 * the end of the connection) says it is complete.  The response is parsed
 * as the synchronous fetch parses it (http.c); a redirect starts the
 * request again at its Location.  A request that makes no progress for
 * LOADER_TIMEOUT fails with ETIMEDOUT.
 *
 * Connections are kept: a response that leaves its
 * connection open puts it in the loader's pool of idle connections, by
 * scheme, host and port, and the next request to the same place takes it
 * instead of connecting again (a kept connection the server has closed
 * meanwhile is replaced once by a new one).  At most LOADER_PER_HOST
 * connections to one place are open at a time; the requests past that wait
 * for one to end.  An idle connection is closed after LOADER_IDLE.
 *
 * The memory cache keeps the 200 responses that say they may be kept
 * (Cache-Control max-age, or an ETag to revalidate them; not no-store), by
 * URL, up to LOADER_CACHE_MAX bytes of bodies (the least recently used go
 * first).  A fresh entry answers a request without the network; a stale
 * one with an ETag is revalidated with If-None-Match, and a 304 answers
 * with the kept body.
 *
 * The caller makes requests and cancels them on its thread; the network
 * thread takes them from a list under the loader's lock.  An ended request
 * goes back on another list and is freed by the caller's thread after its
 * callback (a cancelled one without it, once the network thread has
 * closed its connection).  A callback may start other requests and cancel
 * any request, itself included.
 */

#include "net/net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

/* Whether the system keeps a thread's name (pthread_setname_np). */
#if defined(__linux__) || defined(__FreeBSD__)
#define LOADER_THREAD_NAMES	1
#else
#define LOADER_THREAD_NAMES	0
#endif

/* How long a request may make no progress, in milliseconds. */
#define LOADER_TIMEOUT		30000U

/* The most redirects a request follows. */
#define LOADER_REDIRECTS	20

/* The largest response a request reads, in bytes. */
#define LOADER_MAX_RESPONSE	((size_t)256U * 1024U * 1024U)

/* The bytes read from a connection at a time. */
#define LOADER_CHUNK		16384U

/* The most connections open to one scheme, host and port at a time. */
#define LOADER_PER_HOST		6

/* How long an idle connection is kept, in milliseconds. */
#define LOADER_IDLE		30000U

/* The most bytes of bodies the memory cache keeps. */
#define LOADER_CACHE_MAX	((size_t)64U * 1024U * 1024U)

/* The longest place (scheme://host:port) a connection is kept for. */
#define LOADER_KEY_MAX		320U

/* The most descriptors watched for the caller at a time. */
#define LOADER_WATCH_MAX	64U

/* How long the network thread waits at most when it could not poll every connection, in milliseconds. */
#define LOADER_CUT_WAIT		10

/* The deadline of a request that waits for a connection (it does not time out while it waits). */
#define LOADER_NEVER		((uint64_t)-1)

/* A request's states. */
enum loader_state {
	LOADER_READY,
	LOADER_WAITING,
	LOADER_RESOLVING,
	LOADER_CONNECTING,
	LOADER_HANDSHAKE,
	LOADER_SENDING,
	LOADER_RECEIVING,
	LOADER_ENDED
};

/*
 * A lookup for the resolver's thread: the request it is for (by number,
 * since the request may be gone when the answer comes), the host and the
 * port; then the answer.  Jobs wait in the loader's queue and answers in
 * its list of results, both under its lock.
 */
struct loader_job {
	struct loader_job *next;
	uint64_t number;
	char host[256];
	char port[16];
	struct addrinfo *addresses;
	int status;
};

/*
 * An idle connection of the pool: the place it goes to (its key), its
 * socket and TLS, and since when it has been idle.
 */
struct loader_connection {
	struct loader_connection *next;
	char key[LOADER_KEY_MAX];
	int descriptor;
	struct net_tls *tls;
	uint64_t idle_since;
};

/*
 * A response of the memory cache: its URL (the key), what it gives back
 * (the Content-Type and the body), its ETag, when it was stored or last
 * revalidated, and how long it stays fresh (max-age in seconds, -1 for
 * none; no_cache revalidates it every time).
 */
struct loader_entry {
	struct loader_entry *next;
	char *url;
	struct wb_buffer content_type;
	struct wb_buffer body;
	struct wb_buffer etag;
	uint64_t stored_at;
	long max_age;
	int no_cache;
};

/*
 * One request: its number, the URL it asks for now (after the redirects so
 * far), its state and connection (the addresses left to try, the socket,
 * TLS, and what the socket must become ready for; the place it goes to,
 * whether the connection came from the pool and whether a kept connection
 * was replaced already), the request's text and how much of it went, the
 * response's bytes so far and how far they frame it, the cache entry it
 * revalidates (or NULL), when it times out (on the loader's monotonic
 * clock, in milliseconds), and its callback.  error, response and reason
 * (the TLS failure's) are the outcome the callback reads.
 *
 * Who touches what: the caller's thread fills the request and hands it to
 * the network thread through the loader's incoming list; from then on the
 * network thread alone runs it (the state, the connection, cancelled and
 * retired), until it hands it back through the completed list.  The
 * caller's thread alone reads delivered and dropped, and frees the
 * request.  asked_cancel is under the loader's lock.  queue_next links the
 * request in whichever of the incoming, ended and completed lists holds
 * it.
 */
struct net_request {
	struct net_loader *loader;
	struct net_request *next;
	struct net_request *queue_next;
	uint64_t number;
	int state;
	int cancelled;
	int retired;
	int asked_cancel;
	int delivered;
	int dropped;
	struct net_url url;
	int redirects;
	struct addrinfo *addresses;
	struct addrinfo *address;
	int descriptor;
	struct net_tls *tls;
	short wants;
	char key[LOADER_KEY_MAX];
	int reused;
	int retried;
	struct wb_buffer request;
	size_t sent;
	struct wb_buffer raw;
	struct net_http_framing framing;
	struct loader_entry *validating;
	uint64_t deadline;
	net_request_done done;
	void *context;
	int error;
	struct net_response response;
	char reason[NET_TLS_REASON_MAX];
	int kind;
	char *method;
	char *headers;
	unsigned char *body;
	size_t body_length;
	int has_body;
	int plain;
};

/*
 * The loader.
 *
 * The network thread's own: the requests it runs (requests), the ones
 * that ended since it last handed them over (ended to ended_last, in the
 * order they ended), the pool of idle connections, the memory cache (most
 * recently used first) with the bytes of its bodies, the next request's
 * number, the descriptors of its poll (fds, room for fds_capacity; the
 * watched descriptors from watch_first on), and whether a watched
 * descriptor rang since the caller was last told (rang).
 *
 * The resolver's thread has its queue of jobs, its list of answers and
 * resolver_stopping under resolver_lock (resolver_wake tells it a job is
 * there), and writes a byte to its pipe for each answer.
 *
 * Under lock, the caller's thread and the network thread meet: the
 * requests started and not yet taken by the network thread (incoming),
 * whether a cancel was asked of a running request (cancel_asked), the
 * requests ended and not yet taken by the caller (completed), whether the
 * caller was told of news it has not taken (notified), the descriptors
 * watched for the caller, the generation of that set and the one the
 * network thread polls now, and whether the network thread is to stop.
 * changed is signalled when notified becomes set and when the polled
 * generation moves.  A byte in the control pipe wakes the network thread
 * from its poll.
 *
 * The caller's own: its notify function and context (set when the loader
 * is made), and how many requests it started that have neither ended to it
 * nor been cancelled (pending).
 */
struct net_loader {
	struct net_request *requests;
	struct net_request *ended;
	struct net_request *ended_last;
	struct loader_connection *idle;
	struct loader_entry *cache;
	size_t cache_bytes;
	uint64_t next_number;
	struct pollfd *fds;
	size_t fds_capacity;
	size_t watch_first;
	int rang;
	pthread_t resolver;
	int resolver_started;
	pthread_mutex_t resolver_lock;
	pthread_cond_t resolver_wake;
	struct loader_job *jobs;
	struct loader_job *answers;
	int resolver_stopping;
	int pipe_read;
	int pipe_write;
	pthread_t thread;
	int thread_started;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	struct net_request *incoming;
	struct net_request *incoming_last;
	struct net_request *completed;
	struct net_request *completed_last;
	int cancel_asked;
	int notified;
	int watches[LOADER_WATCH_MAX];
	size_t watch_count;
	uint64_t watch_generation;
	uint64_t polled_generation;
	int stopping;
	int control_read;
	int control_write;
	net_loader_notify notify;
	void *notify_context;
	size_t pending;
};

static uint64_t loader_clock(void);
static void loader_name_thread(const char *name);
static void loader_poke(struct net_loader *loader);
static void loader_release(struct net_loader *loader);
static void loader_free_queue(struct net_request *queue);
static void *loader_resolver(void *argument);
static void *loader_run(void *argument);
static void loader_ready(struct net_loader *loader, size_t count);
static int loader_intake(struct net_loader *loader);
static void loader_tick(struct net_loader *loader);
static void loader_publish(struct net_loader *loader);
static size_t loader_gather(struct net_loader *loader, int *timeout);
static int loader_timeout(const struct net_loader *loader);
static void loader_drain(int descriptor);
static int loader_begin(struct net_request *request);
static void loader_answers(struct net_loader *loader);
static void loader_step(struct net_request *request);
static int loader_connect(struct net_request *request);
static int loader_connected(struct net_request *request);
static int loader_handshake(struct net_request *request);
static int loader_send(struct net_request *request);
static int loader_receive(struct net_request *request);
static int loader_finish(struct net_request *request);
static void loader_close(struct net_request *request);
static void loader_end(struct net_request *request, int error);
static void loader_retire(struct net_request *request);
static void loader_reap(struct net_loader *loader);
static void loader_free(struct net_request *request);
static int loader_prepare(struct net_request *request);
static int loader_retry(struct net_request *request);
static int loader_active(const struct net_loader *loader, const char *key);
static void loader_release_place(struct net_loader *loader, const char *key);
static void loader_keep(struct net_request *request);
static void loader_expire_idle(struct net_loader *loader, uint64_t now);
static struct loader_entry *loader_cache_find(struct net_loader *loader, const char *url);
static int loader_cache_fresh(const struct loader_entry *entry, uint64_t now);
static int loader_cache_answer(struct net_request *request, const struct loader_entry *entry);
static void loader_cache_store(struct net_request *request);
static void loader_cache_free(struct net_loader *loader, struct loader_entry *entry);

/*
 * Makes a loader and starts its resolver's thread and its network thread.
 * notify (which may be NULL) is called with context from the network
 * thread when there is news for net_loader_dispatch: once, until the
 * caller has run net_loader_dispatch.
 */
int
net_loader_create(
	net_loader_notify notify,
	void *context,
	struct net_loader **loader)
{
	struct net_loader *made;
	pthread_condattr_t attributes;
	int descriptors[2];
	int status;

	/* The loader, with no pipe yet. */
	*loader = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->next_number = 1;
	made->pipe_read = -1;
	made->pipe_write = -1;
	made->control_read = -1;
	made->control_write = -1;
	made->notify = notify;
	made->notify_context = context;

	/* Its locks and conditions; the caller's waits run on the monotonic clock. */
	pthread_mutex_init(&made->resolver_lock, NULL);
	pthread_cond_init(&made->resolver_wake, NULL);
	pthread_mutex_init(&made->lock, NULL);
	pthread_condattr_init(&attributes);
	(void)pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	pthread_cond_init(&made->changed, &attributes);
	pthread_condattr_destroy(&attributes);

	/* The descriptors of the network thread's poll, with room for some connections. */
	made->fds_capacity = 2U + LOADER_WATCH_MAX + LOADER_PER_HOST * 4U;
	made->fds = calloc(made->fds_capacity, sizeof(*made->fds));
	if (made->fds == NULL) {
		loader_release(made);
		return ENOMEM;
	}

	/* The pipe the resolver's answers are signalled through; neither end blocks. */
	status = pipe2(descriptors, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		status = errno;
		loader_release(made);
		return status;
	}

	/* Its two ends. */
	made->pipe_read = descriptors[0];
	made->pipe_write = descriptors[1];

	/* The control pipe that wakes the network thread; neither end blocks. */
	status = pipe2(descriptors, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		status = errno;
		loader_release(made);
		return status;
	}

	/* Its two ends. */
	made->control_read = descriptors[0];
	made->control_write = descriptors[1];

	/* The resolver's thread. */
	status = pthread_create(&made->resolver, NULL, loader_resolver, made);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* It runs, and the network thread starts. */
	made->resolver_started = 1;
	status = pthread_create(&made->thread, NULL, loader_run, made);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* Succeeded: the loader takes requests. */
	made->thread_started = 1;
	*loader = made;
	return 0;
}

/*
 * Ends a loader: its threads are stopped, and its requests are dropped
 * without their callbacks.  notify is not called once this returns.
 */
void
net_loader_destroy(
	struct net_loader *loader)
{
	/* Nothing to end. */
	if (loader == NULL)
		return;

	/* The threads, then everything they held. */
	loader_release(loader);
}

/*
 * Starts a GET of an http or https URL; done is called with context when
 * it ends (well or not), from net_loader_dispatch.  *request (which may be
 * NULL) is the request, for cancelling it before then.
 */
int
net_loader_fetch(
	struct net_loader *loader,
	const char *url,
	net_request_done done,
	void *context,
	struct net_request **request)
{
	return net_loader_fetch_kind(loader, url, NET_FETCH_DOCUMENT, done, context, request);
}

/* The same for a kind of fetch (NET_FETCH_*), which decides the Accept header. */
int
net_loader_fetch_kind(
	struct net_loader *loader,
	const char *url,
	int kind,
	net_request_done done,
	void *context,
	struct net_request **request)
{
	return net_loader_send(loader, url, kind, NULL, done, context, request);
}

/*
 * Starts a request with a method, header lines and a body (send; NULL is
 * a plain GET).  Only a plain GET is answered from or kept in the cache,
 * and a redirect of a POST becomes a GET as a browser's does.  A request
 * that cannot even start fails here; one that fails later ends with its
 * error through its callback.
 */
int
net_loader_send(
	struct net_loader *loader,
	const char *url,
	int kind,
	const struct net_send *send,
	net_request_done done,
	void *context,
	struct net_request **request)
{
	struct net_request *made;
	int is_web;
	int error;

	/* The request. */
	if (request != NULL)
		*request = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->loader = loader;
	made->descriptor = -1;
	made->kind = kind;
	made->done = done;
	made->context = context;
	made->plain = 1;
	wb_buffer_init(&made->request);
	wb_buffer_init(&made->raw);
	net_response_init(&made->response);

	/* What a script added: the method, the header lines and the body, copied. */
	if (send != NULL) {
		made->plain = (send->method == NULL || strcmp(send->method, "GET") == 0) && send->headers == NULL && send->body == NULL;
		if (send->method != NULL)
			made->method = strdup(send->method);
		if (send->headers != NULL)
			made->headers = strdup(send->headers);
		if (send->body != NULL && send->body_length > 0) {
			made->body = malloc(send->body_length);
			if (made->body != NULL) {
				memcpy(made->body, send->body, send->body_length);
				made->body_length = send->body_length;
			}
		}
		if ((send->method != NULL && made->method == NULL) || (send->headers != NULL && made->headers == NULL)
		    || (send->body != NULL && send->body_length > 0 && made->body == NULL)) {
			loader_free(made);
			return ENOMEM;
		}
		made->has_body = send->body != NULL;
	}

	/* Its URL, which must be http or https. */
	error = net_url_parse(url, strlen(url), NULL, &made->url);
	if (error != 0) {
		loader_free(made);
		return error;
	}

	/* Only http and https are fetched. */
	is_web = net_http_is_web(made->url.scheme);
	if (!is_web) {
		loader_free(made);
		return EPROTONOSUPPORT;
	}

	/* Handed to the network thread, which begins it. */
	pthread_mutex_lock(&loader->lock);

	if (loader->incoming_last != NULL)
		loader->incoming_last->queue_next = made;
	else
		loader->incoming = made;
	loader->incoming_last = made;

	pthread_mutex_unlock(&loader->lock);

	/* The network thread wakes to take it. */
	loader_poke(loader);

	/* Succeeded: the request runs, and counts until it ends or is cancelled. */
	loader->pending++;
	if (request != NULL)
		*request = made;
	return 0;
}

/*
 * Cancels a request: its callback is not called, and it is freed once the
 * network thread has closed its connection (or after the callback that
 * cancels it returns).
 */
void
net_request_cancel(
	struct net_request *request)
{
	struct net_loader *loader;

	/* Nothing to cancel. */
	if (request == NULL)
		return;

	/* A request whose callback runs now is freed when it returns. */
	if (request->delivered)
		return;

	/* A request cancelled already. */
	if (request->dropped)
		return;

	/* Its callback will not be called, and it no longer counts. */
	loader = request->loader;
	request->dropped = 1;
	loader->pending--;

	/* The network thread is asked to close its connection and hand it back. */
	pthread_mutex_lock(&loader->lock);

	request->asked_cancel = 1;
	loader->cancel_asked = 1;

	pthread_mutex_unlock(&loader->lock);

	/* The network thread wakes to do so. */
	loader_poke(loader);
}

/* Tells how a request ended: 0, or an errno value (EPROTO for TLS, whose reason net_tls_error gives). */
int
net_request_error(
	const struct net_request *request)
{
	/* The request's outcome. */
	return request->error;
}

/* Gives a request's response (its final URL, status, Content-Type and body), valid in its callback. */
const struct net_response *
net_request_response(
	const struct net_request *request)
{
	/* The response the request read. */
	return &request->response;
}

/*
 * Tells how many requests were started that have neither ended to their
 * callbacks nor been cancelled.
 */
size_t
net_loader_pending(
	const struct net_loader *loader)
{
	/* The caller's own count. */
	return loader->pending;
}

/*
 * Waits until the network thread has news for net_loader_dispatch, or for
 * a number of milliseconds at most (-1: until there is news).  For a
 * caller without a notify function (the headless modes).
 */
void
net_loader_wait(
	struct net_loader *loader,
	int timeout)
{
	struct timespec deadline;
	int status;

	/* The deadline on the monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	if (timeout > 0) {
		deadline.tv_sec += timeout / 1000;
		deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
	}

	/* A carry of the nanoseconds into the seconds. */
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	/* Until the network thread tells of news, or the deadline passes. */
	pthread_mutex_lock(&loader->lock);

	/* Each wake of the condition, until there is news or the time is up. */
	status = 0;
	while (!loader->notified && status != ETIMEDOUT) {
		if (timeout < 0)
			pthread_cond_wait(&loader->changed, &loader->lock);
		else
			status = pthread_cond_timedwait(&loader->changed, &loader->lock, &deadline);
	}

	pthread_mutex_unlock(&loader->lock);
}

/*
 * Takes the network thread's news: each request that ended (cancelled
 * ones aside) has its callback called, in the order they ended, then is
 * freed.  The callbacks may start and cancel requests.  The next news
 * calls notify again.
 */
void
net_loader_dispatch(
	struct net_loader *loader)
{
	struct net_request *taken;
	struct net_request *request;

	/* The ended requests, all of them, and the caller is told of the next news again. */
	pthread_mutex_lock(&loader->lock);

	taken = loader->completed;
	loader->completed = NULL;
	loader->completed_last = NULL;
	loader->notified = 0;

	pthread_mutex_unlock(&loader->lock);

	/* Each request in turn: its callback (with its TLS reason as the thread's), then it goes. */
	while (taken != NULL) {
		request = taken;
		taken = request->queue_next;

		/* One cancelled meanwhile, even by an earlier callback, only goes. */
		if (!request->dropped) {
			request->delivered = 1;
			loader->pending--;
			net_tls_set_error(request->reason);
			if (request->done != NULL)
				request->done(request->context, request);
		}

		/* The request is freed. */
		loader_free(request);
	}
}

/*
 * Watches a descriptor for the caller: when it becomes readable, the
 * network thread reads it empty and tells the caller as it tells of an
 * ended request.  Returns 0, or ENOSPC when LOADER_WATCH_MAX are watched.
 */
int
net_loader_watch(
	struct net_loader *loader,
	int descriptor)
{
	int full;

	/* The descriptor joins the set, which the next poll uses. */
	full = 0;
	pthread_mutex_lock(&loader->lock);

	/* A full set takes no more; the generation moves with each change. */
	if (loader->watch_count == LOADER_WATCH_MAX) {
		full = 1;
	} else {
		loader->watches[loader->watch_count] = descriptor;
		loader->watch_count++;
		loader->watch_generation++;
	}

	pthread_mutex_unlock(&loader->lock);

	/* No room. */
	if (full)
		return ENOSPC;

	/* Succeeded: the network thread wakes to poll it. */
	loader_poke(loader);
	return 0;
}

/*
 * Stops watching a descriptor, and waits until the network thread no
 * longer polls or reads it: the caller may close it then.
 */
void
net_loader_unwatch(
	struct net_loader *loader,
	int descriptor)
{
	uint64_t generation;
	size_t index;
	int found;

	/* The descriptor leaves the set (the last one takes its place). */
	found = 0;
	generation = 0;
	pthread_mutex_lock(&loader->lock);

	/* The descriptor's entry, when it is watched. */
	for (index = 0; index < loader->watch_count; index++) {
		if (loader->watches[index] != descriptor)
			continue;
		loader->watch_count--;
		loader->watches[index] = loader->watches[loader->watch_count];
		loader->watch_generation++;
		generation = loader->watch_generation;
		found = 1;
		break;
	}

	pthread_mutex_unlock(&loader->lock);

	/* One not watched needs no wait. */
	if (!found)
		return;

	/* The network thread wakes and polls the new set. */
	loader_poke(loader);

	/* Until its poll no longer has the descriptor. */
	pthread_mutex_lock(&loader->lock);

	/* Each wake of the condition, until the generation polled is the new set's. */
	while (loader->polled_generation < generation)
		pthread_cond_wait(&loader->changed, &loader->lock);

	pthread_mutex_unlock(&loader->lock);
}

/*
 * Tells whether a location is one the loader fetches (an http or https
 * URL).
 */
int
net_loader_takes(
	const char *location)
{
	int differs;

	/* http: and https: URLs. */
	differs = strncmp(location, "http://", 7);
	if (differs == 0)
		return 1;
	differs = strncmp(location, "https://", 8);
	if (differs == 0)
		return 1;

	/* Paths and other schemes are read at once. */
	return 0;
}

/* Reads the loader's monotonic clock, in milliseconds. */
static uint64_t
loader_clock(void)
{
	struct timespec now;
	uint64_t milliseconds;

	/* The monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	milliseconds = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
	return milliseconds;
}

/* The resolver's thread: each job's lookup, answered through the list and the pipe, until the loader stops. */
static void *
loader_resolver(
	void *argument)
{
	struct net_loader *loader;
	struct loader_job *job;
	struct addrinfo hints;
	unsigned char signal;

	/* The thread's name. */
	loader = argument;
	loader_name_thread("browser-resolve");

	/* Each job, as they come. */
	for (;;) {
		/* The next job, waited for. */
		pthread_mutex_lock(&loader->resolver_lock);

		/* Waits until a lookup job arrives or shutdown releases the resolver. */
		while (loader->jobs == NULL && !loader->resolver_stopping)
			pthread_cond_wait(&loader->resolver_wake, &loader->resolver_lock);
		job = loader->jobs;
		if (job != NULL)
			loader->jobs = job->next;

		pthread_mutex_unlock(&loader->resolver_lock);

		/* A loader that stops ends the thread. */
		if (job == NULL)
			break;

		/* The lookup, which may take long. */
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		job->addresses = NULL;
		job->status = getaddrinfo(job->host, job->port, &hints, &job->addresses);

		/* The answer, then a byte through the pipe to wake the network thread. */
		pthread_mutex_lock(&loader->resolver_lock);

		job->next = loader->answers;
		loader->answers = job;

		pthread_mutex_unlock(&loader->resolver_lock);

		/* The byte. */
		signal = 1;
		(void)write(loader->pipe_write, &signal, 1);
	}

	/* The loader stopped. */
	return NULL;
}

/* Names the calling thread, on a system that keeps names (others leave it unnamed). */
static void
loader_name_thread(
	const char *name)
{
	/* The name, or nothing on a system without names. */
#if LOADER_THREAD_NAMES
	(void)pthread_setname_np(pthread_self(), name);
#else
	(void)name;
#endif
}

/* Wakes the network thread from its poll (a full control pipe is readable already). */
static void
loader_poke(
	struct net_loader *loader)
{
	unsigned char byte;
	ssize_t wrote;

	/* One byte. */
	byte = 1;
	wrote = write(loader->control_write, &byte, 1U);
	(void)wrote;
}

/*
 * Stops the loader's threads (those that started) and frees everything it
 * holds: the requests in every list, the jobs and answers, the idle
 * connections, the cache, the pipes and the loader.
 */
static void
loader_release(
	struct net_loader *loader)
{
	struct net_request *request;
	struct loader_job *job;

	/* The network thread, told to stop; it ends after its round. */
	pthread_mutex_lock(&loader->lock);

	loader->stopping = 1;

	pthread_mutex_unlock(&loader->lock);

	/* Woken from its poll, then waited for. */
	if (loader->thread_started) {
		loader_poke(loader);
		pthread_join(loader->thread, NULL);
	}

	/* The resolver's thread, told to stop. */
	pthread_mutex_lock(&loader->resolver_lock);

	loader->resolver_stopping = 1;
	pthread_cond_signal(&loader->resolver_wake);

	pthread_mutex_unlock(&loader->resolver_lock);

	/* It ends after its current lookup. */
	if (loader->resolver_started)
		pthread_join(loader->resolver, NULL);

	/* The requests the network thread ran. */
	while (loader->requests != NULL) {
		request = loader->requests;
		loader->requests = request->next;
		loader_free(request);
	}

	/* Those it ended and did not hand over, those it did not take, and those the caller did not take. */
	loader_free_queue(loader->ended);
	loader_free_queue(loader->incoming);
	loader_free_queue(loader->completed);

	/* The jobs and answers left. */
	while (loader->jobs != NULL) {
		job = loader->jobs;
		loader->jobs = job->next;
		free(job);
	}

	/* The answers nobody took. */
	while (loader->answers != NULL) {
		job = loader->answers;
		loader->answers = job->next;
		if (job->addresses != NULL)
			freeaddrinfo(job->addresses);
		free(job);
	}

	/* The idle connections. */
	loader_expire_idle(loader, LOADER_NEVER);

	/* The cache. */
	while (loader->cache != NULL)
		loader_cache_free(loader, loader->cache);

	/* The pipes that were made. */
	if (loader->pipe_read >= 0)
		close(loader->pipe_read);
	if (loader->pipe_write >= 0)
		close(loader->pipe_write);
	if (loader->control_read >= 0)
		close(loader->control_read);
	if (loader->control_write >= 0)
		close(loader->control_write);

	/* The locks, the poll's descriptors, then the loader. */
	pthread_mutex_destroy(&loader->resolver_lock);
	pthread_cond_destroy(&loader->resolver_wake);
	pthread_mutex_destroy(&loader->lock);
	pthread_cond_destroy(&loader->changed);
	free(loader->fds);
	free(loader);
}

/* Frees the requests of a list linked by queue_next. */
static void
loader_free_queue(
	struct net_request *queue)
{
	struct net_request *request;

	/* Each request of the list. */
	while (queue != NULL) {
		request = queue;
		queue = request->queue_next;
		loader_free(request);
	}
}

/*
 * The network thread: rounds of what became ready, what the caller asked,
 * the cache's answers and the time outs, the ended requests handed over,
 * and a poll of the pipes, the connections and the watched descriptors,
 * until the loader stops.
 */
static void *
loader_run(
	void *argument)
{
	struct net_loader *loader;
	size_t count;
	int timeout;
	int ready;
	int stop;

	/* The thread's name, and nothing polled yet. */
	loader = argument;
	loader_name_thread("browser-net");
	count = 0;

	/* Each round, until the loader stops. */
	for (;;) {
		/* What the last poll found ready: the resolver's answers, the connections, the watched descriptors. */
		loader_ready(loader, count);

		/* What the caller asked meanwhile: new requests, cancels, or the end. */
		stop = loader_intake(loader);
		if (stop)
			break;

		/* The cache's answers, the idle connections kept too long, the requests out of time. */
		loader_tick(loader);

		/* The ended requests go to the caller, who is told. */
		loader_publish(loader);

		/* The wait for the next thing to happen. */
		count = loader_gather(loader, &timeout);
		ready = poll(loader->fds, (nfds_t)count, timeout);
		if (ready < 0)
			count = 0;
	}

	/* The ended requests leave the list; net_loader_destroy frees every request. */
	loader_reap(loader);
	return NULL;
}

/*
 * Takes what the last poll found ready (the first count descriptors of
 * fds): the control pipe emptied, the resolver's answers, the next steps
 * of each ready connection, and each watched descriptor that rang read
 * empty (the caller is told).
 */
static void
loader_ready(
	struct net_loader *loader,
	size_t count)
{
	struct net_request *request;
	size_t index;

	/* The control pipe's bytes; what the caller asked is read under the lock. */
	loader_drain(loader->control_read);

	/* The resolver's answers. */
	loader_answers(loader);

	/* Each ready connection takes its next steps. */
	for (index = 2U; index < count && index < loader->watch_first; index++) {
		if (loader->fds[index].revents == 0)
			continue;

		/* The request whose connection it is. */
		for (request = loader->requests; request != NULL; request = request->next) {
			if (request->cancelled || request->descriptor != loader->fds[index].fd)
				continue;
			loader_step(request);
			break;
		}
	}

	/* Each watched descriptor that rang is read empty, and the caller hears of it. */
	for (index = loader->watch_first; index < count; index++) {
		if (loader->fds[index].revents == 0)
			continue;
		loader_drain(loader->fds[index].fd);
		loader->rang = 1;
	}
}

/*
 * Takes what the caller asked: the requests it started (begun, or ended
 * at once when cancelled already) and the cancels of running requests
 * (their connections closed).  Returns nonzero when the loader stops.
 */
static int
loader_intake(
	struct net_loader *loader)
{
	struct net_request *taken;
	struct net_request *request;
	int error;
	int stop;

	/* The new requests and the cancels, marked while the caller cannot change them. */
	pthread_mutex_lock(&loader->lock);

	taken = loader->incoming;
	loader->incoming = NULL;
	loader->incoming_last = NULL;

	/* A new request may have been cancelled already. */
	for (request = taken; request != NULL; request = request->queue_next)
		request->cancelled = request->asked_cancel;

	/* The running requests whose cancel was asked. */
	if (loader->cancel_asked) {
		for (request = loader->requests; request != NULL; request = request->next) {
			if (request->asked_cancel)
				request->cancelled = 1;
		}

		/* Marks the asks as seen. */
		loader->cancel_asked = 0;
	}

	/* Whether the caller ends the loader. */
	stop = loader->stopping;

	pthread_mutex_unlock(&loader->lock);

	/* Each running request cancelled now: its connection closed, its place given up, and back to the caller. */
	for (request = loader->requests; request != NULL; request = request->next) {
		if (!request->cancelled || request->retired)
			continue;
		loader_close(request);
		loader_release_place(loader, request->key);
		loader_retire(request);
	}

	/* Each new request joins the list; it begins unless it was cancelled or the loader stops. */
	while (taken != NULL) {
		request = taken;
		taken = request->queue_next;
		request->queue_next = NULL;
		request->number = loader->next_number;
		loader->next_number++;
		request->next = loader->requests;
		loader->requests = request;

		/* A loader that stops begins nothing. */
		if (stop)
			continue;

		/* One cancelled before it began goes back at once. */
		if (request->cancelled) {
			loader_retire(request);
			continue;
		}

		/* Its lookup, its connection or the cache's answer; one that cannot begin ends. */
		error = loader_begin(request);
		if (error != 0)
			loader_end(request, error);
	}

	/* Whether the loader stops. */
	return stop;
}

/*
 * Ends the requests the cache answered, closes the idle connections kept
 * too long, and ends the requests that made no progress in time.
 */
static void
loader_tick(
	struct net_loader *loader)
{
	struct net_request *request;
	uint64_t now;

	/* The requests the cache answered. */
	now = loader_clock();
	for (request = loader->requests; request != NULL; request = request->next) {
		if (!request->cancelled && request->state == LOADER_READY)
			loader_end(request, 0);
	}

	/* The idle connections kept too long. */
	loader_expire_idle(loader, now);

	/* The requests that made no progress in time. */
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->state == LOADER_ENDED || request->state == LOADER_WAITING)
			continue;
		if (now >= request->deadline)
			loader_end(request, ETIMEDOUT);
	}
}

/*
 * Hands the requests that ended (in the order they ended) to the caller,
 * and tells the caller of them or of a watched descriptor that rang: its
 * notify is called once until it runs net_loader_dispatch, and a caller
 * waiting in net_loader_wait wakes.
 */
static void
loader_publish(
	struct net_loader *loader)
{
	int tell;

	/* The ended requests leave the network thread's list. */
	loader_reap(loader);

	/* Nothing ended and nothing rang: nothing to tell. */
	if (loader->ended == NULL && !loader->rang)
		return;

	/* The ended requests follow those the caller has not taken; the caller is told once. */
	tell = 0;
	pthread_mutex_lock(&loader->lock);

	/* The ended requests, after those already waiting. */
	if (loader->ended != NULL) {
		if (loader->completed_last != NULL)
			loader->completed_last->queue_next = loader->ended;
		else
			loader->completed = loader->ended;
		loader->completed_last = loader->ended_last;
	}

	/* The caller is told when it was not told already; a waiting caller wakes. */
	if (!loader->notified)
		tell = 1;
	loader->notified = 1;
	pthread_cond_broadcast(&loader->changed);

	pthread_mutex_unlock(&loader->lock);

	/* The network thread holds none of them now. */
	loader->ended = NULL;
	loader->ended_last = NULL;
	loader->rang = 0;

	/* The caller's notify, outside the lock. */
	if (tell && loader->notify != NULL)
		loader->notify(loader->notify_context);
}

/*
 * Lists the descriptors of the next poll in fds: the control pipe, the
 * resolver's pipe, each connection for what it waits for, then the
 * watched descriptors (from watch_first on); the set of watched
 * descriptors polled is marked as the caller's latest.  *timeout is how
 * long the poll may wait.  Returns how many descriptors were listed.
 */
static size_t
loader_gather(
	struct net_loader *loader,
	int *timeout)
{
	struct net_request *request;
	struct pollfd *grown;
	size_t needed;
	size_t count;
	size_t index;
	int cut;

	/* Room for the two pipes, every request's connection and every watched descriptor. */
	needed = 2U + LOADER_WATCH_MAX;
	for (request = loader->requests; request != NULL; request = request->next)
		needed++;

	/* More room when there is not enough (without memory, the connections left out are polled soon after). */
	if (needed > loader->fds_capacity) {
		grown = realloc(loader->fds, needed * sizeof(*grown));
		if (grown != NULL) {
			loader->fds = grown;
			loader->fds_capacity = needed;
		}
	}

	/* The control pipe, then the resolver's. */
	loader->fds[0].fd = loader->control_read;
	loader->fds[0].events = POLLIN;
	loader->fds[0].revents = 0;
	loader->fds[1].fd = loader->pipe_read;
	loader->fds[1].events = POLLIN;
	loader->fds[1].revents = 0;
	count = 2U;

	/* Each connection that waits for its socket, while the watched descriptors keep their room. */
	cut = 0;
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->descriptor < 0 || request->wants == 0)
			continue;
		if (count + LOADER_WATCH_MAX == loader->fds_capacity) {
			cut = 1;
			break;
		}

		/* Watches this connection's socket. */
		loader->fds[count].fd = request->descriptor;
		loader->fds[count].events = request->wants;
		loader->fds[count].revents = 0;
		count++;
	}

	/* The watched descriptors as the caller leaves them now; a caller waiting to close one learns it is no longer polled. */
	loader->watch_first = count;
	pthread_mutex_lock(&loader->lock);

	/* Each watched descriptor, for reading. */
	for (index = 0; index < loader->watch_count; index++) {
		loader->fds[count].fd = loader->watches[index];
		loader->fds[count].events = POLLIN;
		loader->fds[count].revents = 0;
		count++;
	}

	/* A new set polled: a caller waiting in net_loader_unwatch may go on. */
	if (loader->polled_generation != loader->watch_generation) {
		loader->polled_generation = loader->watch_generation;
		pthread_cond_broadcast(&loader->changed);
	}

	pthread_mutex_unlock(&loader->lock);

	/* The earliest time out, sooner when connections were left out. */
	*timeout = loader_timeout(loader);
	if (cut && (*timeout < 0 || *timeout > LOADER_CUT_WAIT))
		*timeout = LOADER_CUT_WAIT;
	return count;
}

/*
 * Tells how long the network thread may wait before the loader must run
 * anyway (the earliest time out), in milliseconds, or -1 when there is no
 * request to time out.
 */
static int
loader_timeout(
	const struct net_loader *loader)
{
	const struct net_request *request;
	uint64_t earliest;
	uint64_t now;
	int found;

	/* The earliest deadline of a request that runs. */
	found = 0;
	earliest = 0;
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->state == LOADER_ENDED || request->state == LOADER_WAITING)
			continue;
		if (request->state == LOADER_READY)
			return 0;
		if (!found || request->deadline < earliest)
			earliest = request->deadline;
		found = 1;
	}

	/* No request, no time out. */
	if (!found)
		return -1;

	/* The time left, none when it has passed. */
	now = loader_clock();
	if (earliest <= now)
		return 0;
	if (earliest - now > 60000U)
		return 60000;
	return (int)(earliest - now);
}

/* Reads a descriptor that does not block until it has nothing more. */
static void
loader_drain(
	int descriptor)
{
	unsigned char drained[64];
	ssize_t got;

	/* Its bytes, all of them. */
	do {
		got = read(descriptor, drained, sizeof(drained));
	} while (got > 0);
}

/*
 * Starts a request at its URL: answered by the cache when it holds the URL
 * fresh; else over a kept connection to the place, or, when fewer than
 * LOADER_PER_HOST are open there, over a new one (the host's lookup given
 * to the resolver's thread); else it waits for one to end.
 */
static int
loader_begin(
	struct net_request *request)
{
	struct net_loader *loader;
	struct loader_connection **link;
	struct loader_connection *kept;
	struct loader_entry *entry;
	struct loader_job *job;
	struct wb_buffer url;
	size_t length;
	uint64_t now;
	int fresh;
	int active;
	int error;
	int port;

	/* The place the request goes to. */
	loader = request->loader;
	now = loader_clock();
	port = net_url_default_port(request->url.scheme);
	if (request->url.port >= 0)
		port = request->url.port;
	snprintf(request->key, sizeof(request->key), "%s://%s:%d", request->url.scheme, request->url.host, port);

	/* A fresh answer of the cache ends the request at the next run of the loader. */
	wb_buffer_init(&url);
	error = net_url_serialize(&request->url, 1, &url);
	if (error != 0) {
		wb_buffer_release(&url);
		return error;
	}

	/* The URL's entry, answering when it is fresh. */
	entry = NULL;
	if (request->plain)
		entry = loader_cache_find(loader, wb_buffer_string(&url));
	wb_buffer_release(&url);
	fresh = loader_cache_fresh(entry, now);
	if (fresh) {
		error = loader_cache_answer(request, entry);
		if (error != 0)
			return error;
		request->state = LOADER_READY;
		request->wants = 0;
		return 0;
	}

	/* A stale entry with an ETag is revalidated. */
	request->validating = NULL;
	if (entry != NULL && entry->etag.length != 0)
		request->validating = entry;

	/* A kept connection to the place, when there is one (not after a kept one failed). */
	for (link = &loader->idle; *link != NULL && !request->retried; link = &(*link)->next) {
		error = strcmp((*link)->key, request->key);
		if (error == 0)
			break;
	}

	/* One is there: the request takes it. */
	if (!request->retried && *link != NULL) {
		/* Taken out of the pool. */
		kept = *link;
		*link = kept->next;
		request->descriptor = kept->descriptor;
		request->tls = kept->tls;
		request->reused = 1;
		free(kept);

		/* The request goes out over it. */
		error = loader_prepare(request);
		if (error != 0)
			return error;
		request->state = LOADER_SENDING;
		request->wants = POLLOUT;
		return 0;
	}

	/* A place with all its connections open: the request waits for one to end. */
	request->reused = 0;
	active = loader_active(loader, request->key);
	if (active >= LOADER_PER_HOST) {
		request->state = LOADER_WAITING;
		request->wants = 0;
		request->deadline = LOADER_NEVER;
		return 0;
	}

	/* The job: the host without an IPv6 address's brackets, and the port or the scheme's. */
	job = calloc(1, sizeof(*job));
	if (job == NULL)
		return ENOMEM;
	job->number = request->number;
	length = strlen(request->url.host);
	if (length >= sizeof(job->host)) {
		free(job);
		return ENAMETOOLONG;
	}

	/* The host, without an IPv6 address's brackets. */
	memcpy(job->host, request->url.host, length + 1U);
	if (length >= 2U && job->host[0] == '[') {
		memmove(job->host, job->host + 1, length - 2U);
		job->host[length - 2U] = '\0';
	}

	/* The port, or the scheme's. */
	snprintf(job->port, sizeof(job->port), "%d", port);

	/* The request waits for the answer, for as long as a request may be silent. */
	request->state = LOADER_RESOLVING;
	request->wants = 0;
	request->deadline = loader_clock() + LOADER_TIMEOUT;

	/* The job to the thread. */
	pthread_mutex_lock(&loader->resolver_lock);

	job->next = loader->jobs;
	loader->jobs = job;
	pthread_cond_signal(&loader->resolver_wake);

	pthread_mutex_unlock(&loader->resolver_lock);

	/* Succeeded: the lookup is under way. */
	return 0;
}

/* Takes the resolver's answers: each request that waits for one starts to connect (or fails). */
static void
loader_answers(
	struct net_loader *loader)
{
	struct loader_job *answers;
	struct loader_job *job;
	struct net_request *request;
	int error;

	/* The pipe's bytes, all of them. */
	loader_drain(loader->pipe_read);

	/* The answers, taken at once. */
	pthread_mutex_lock(&loader->resolver_lock);

	answers = loader->answers;
	loader->answers = NULL;

	pthread_mutex_unlock(&loader->resolver_lock);

	/* Each answer to its request, when it is still there and waiting. */
	while (answers != NULL) {
		job = answers;
		answers = job->next;
		for (request = loader->requests; request != NULL; request = request->next) {
			if (request->number == job->number)
				break;
		}

		/* An answer nobody waits for any more. */
		if (request == NULL || request->cancelled || request->state != LOADER_RESOLVING) {
			if (job->addresses != NULL)
				freeaddrinfo(job->addresses);
			free(job);
			continue;
		}

		/* A host that has no address fails the request. */
		if (job->status != 0 || job->addresses == NULL) {
			if (job->addresses != NULL)
				freeaddrinfo(job->addresses);
			free(job);
			loader_end(request, EHOSTUNREACH);
			continue;
		}

		/* The addresses, tried in turn from the first. */
		request->addresses = job->addresses;
		request->address = job->addresses;
		free(job);
		request->state = LOADER_CONNECTING;
		error = loader_connect(request);
		if (error != 0)
			loader_end(request, error);

		/* The connection goes on when its socket becomes writable. */
	}
}

/* Takes a request as far as its socket allows, ending it when it is done or fails. */
static void
loader_step(
	struct net_request *request)
{
	int error;

	/* The step of each state, until one must wait (EAGAIN) or the request ends. */
	error = 0;
	while (error == 0 && !request->cancelled && request->state != LOADER_ENDED) {
		/* Decide by the state. */
		switch (request->state) {
		case LOADER_CONNECTING:
			error = loader_connected(request);
			break;
		case LOADER_HANDSHAKE:
			error = loader_handshake(request);
			break;
		case LOADER_SENDING:
			error = loader_send(request);
			break;
		case LOADER_RECEIVING:
			error = loader_receive(request);
			break;
		default:
			error = EAGAIN;
			break;
		}
	}

	/* Waiting is not a failure; anything else ends the request. */
	if (error != 0 && error != EAGAIN)
		loader_end(request, error);
}

/*
 * Starts a non-blocking connection to the request's current address,
 * moving past the addresses that cannot even be tried.
 */
static int
loader_connect(
	struct net_request *request)
{
	struct addrinfo *address;
	int status;
	int error;

	/* The addresses from the current one. */
	error = ECONNREFUSED;
	for (address = request->address; address != NULL; address = address->ai_next) {
		request->address = address;
		request->descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (request->descriptor < 0) {
			error = errno;
			continue;
		}

		/* The socket does not block, and its connection starts. */
		(void)fcntl(request->descriptor, F_SETFL, O_NONBLOCK);
		status = connect(request->descriptor, address->ai_addr, address->ai_addrlen);
		if (status == 0 || errno == EINPROGRESS) {
			request->wants = POLLOUT;
			request->deadline = loader_clock() + LOADER_TIMEOUT;
			return 0;
		}

		/* An address that refuses at once gives way to the next. */
		error = errno;
		close(request->descriptor);
		request->descriptor = -1;
	}

	/* No address could be tried. */
	return error;
}

/*
 * Checks a connection that was under way: connected, it goes on to TLS or
 * the request; refused, the next address is tried.
 */
static int
loader_connected(
	struct net_request *request)
{
	struct sockaddr_storage peer;
	socklen_t size;
	int failure;
	int status;
	int is_https;
	int error;

	/* The connection's outcome. */
	failure = 0;
	size = sizeof(failure);
	status = getsockopt(request->descriptor, SOL_SOCKET, SO_ERROR, &failure, &size);
	if (status != 0)
		failure = errno;
	if (failure == EINPROGRESS || failure == EALREADY)
		return EAGAIN;

	/* No error yet is not a connection yet: a connection has a peer. */
	if (failure == 0) {
		size = sizeof(peer);
		status = getpeername(request->descriptor, (struct sockaddr *)&peer, &size);
		if (status != 0 && errno == ENOTCONN)
			return EAGAIN;
	}

	/* A refused or failed connection moves to the next address. */
	if (failure != 0) {
		close(request->descriptor);
		request->descriptor = -1;
		request->address = request->address->ai_next;
		if (request->address == NULL)
			return failure;
		error = loader_connect(request);
		if (error != 0)
			return error;
		return EAGAIN;
	}

	/* The addresses are no longer needed. */
	freeaddrinfo(request->addresses);
	request->addresses = NULL;
	request->address = NULL;

	/* The request's text. */
	error = loader_prepare(request);
	if (error != 0)
		return error;

	/* https starts TLS over the connection; http sends at once. */
	is_https = !strcmp(request->url.scheme, "https");
	if (is_https) {
		net_tls_clear_error();
		error = net_tls_start(request->descriptor, request->url.host, &request->tls);
		if (error != 0)
			return error;
		request->state = LOADER_HANDSHAKE;
		return 0;
	}

	/* Succeeded: the request can be sent. */
	request->state = LOADER_SENDING;
	return 0;
}

/* Takes the TLS handshake a step. */
static int
loader_handshake(
	struct net_request *request)
{
	int error;

	/* The step; waiting says what for. */
	error = net_tls_handshake(request->tls, &request->wants);
	if (error != 0)
		return error;

	/* Succeeded: the connection is secure, and the request can be sent. */
	request->state = LOADER_SENDING;
	request->deadline = loader_clock() + LOADER_TIMEOUT;
	return 0;
}

/* Sends as much of the request as the connection takes now. */
static int
loader_send(
	struct net_request *request)
{
	const unsigned char *bytes;
	size_t length;
	size_t sent;
	ssize_t count;
	int error;

	/* What is left to send. */
	bytes = request->request.data + request->sent;
	length = request->request.length - request->sent;
	sent = 0;

	/* Over TLS, or the socket. */
	if (request->tls != NULL) {
		error = net_tls_write_some(request->tls, bytes, length, &sent, &request->wants);
		if (error != 0)
			return error;
	} else {
		count = send(request->descriptor, bytes, length, MSG_NOSIGNAL);
		if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
			request->wants = POLLOUT;
			return EAGAIN;
		}

		/* A kept connection the server closed is replaced once. */
		if (count < 0 && request->reused) {
			error = loader_retry(request);
			return error;
		}

		/* Any other failure ends the request. */
		if (count < 0)
			return errno;
		sent = (size_t)count;
	}

	/* The bytes went; all of them means the response comes next. */
	request->sent += sent;
	request->deadline = loader_clock() + LOADER_TIMEOUT;
	if (request->sent < request->request.length)
		return 0;

	/* Succeeded: the request is sent, and the response is read. */
	request->state = LOADER_RECEIVING;
	request->wants = POLLIN;
	return 0;
}

/* Reads what the connection has now; its end finishes the response. */
static int
loader_receive(
	struct net_request *request)
{
	unsigned char chunk[LOADER_CHUNK];
	size_t received;
	ssize_t count;
	int error;

	/* Over TLS, or the socket. */
	received = 0;
	if (request->tls != NULL) {
		error = net_tls_read_some(request->tls, chunk, sizeof(chunk), &received, &request->wants);
		if (error != 0)
			return error;
	} else {
		count = recv(request->descriptor, chunk, sizeof(chunk), 0);
		if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
			request->wants = POLLIN;
			return EAGAIN;
		}

		/* Any other failure ends the request. */
		if (count < 0)
			return errno;
		received = (size_t)count;
	}

	/* A kept connection closed before anything of the response came is replaced once. */
	if (received == 0 && request->reused && request->raw.length == 0) {
		error = loader_retry(request);
		return error;
	}

	/* The end of the connection is the end of the response. */
	if (received == 0) {
		error = loader_finish(request);
		return error;
	}

	/* The bytes, within the largest response. */
	if (request->raw.length + received > LOADER_MAX_RESPONSE)
		return EFBIG;
	error = wb_buffer_append(&request->raw, chunk, received);
	if (error != 0)
		return error;

	/* A response its framing says is complete ends here, its connection kept when it may be. */
	request->deadline = loader_clock() + LOADER_TIMEOUT;
	net_http_framing_update(&request->framing, request->raw.data, request->raw.length);
	if (request->framing.done) {
		error = loader_finish(request);
		return error;
	}

	/* Succeeded: more may come. */
	return 0;
}

/*
 * Finishes a response read to its end: parsed, it either ends the request
 * or, as a redirect with a Location, starts it again there.
 */
static int
loader_finish(
	struct net_request *request)
{
	struct wb_buffer location;
	struct net_url next;
	int redirected;
	int is_web;
	int error;

	/* The connection goes back to the pool when the response left it open, else it is closed. */
	if (request->framing.done && request->framing.keep_alive)
		loader_keep(request);
	loader_close(request);

	/* The response, with nothing of the last one's caching left. */
	wb_buffer_clear(&request->response.content_type);
	wb_buffer_clear(&request->response.body);
	wb_buffer_clear(&request->response.etag);
	wb_buffer_clear(&request->response.headers);
	request->response.max_age = -1;
	request->response.no_store = 0;
	request->response.no_cache = 0;
	wb_buffer_init(&location);
	error = net_http_parse_response(&request->url, &request->raw, &request->response, &location);
	wb_buffer_clear(&request->raw);
	if (error != 0) {
		wb_buffer_release(&location);
		return error;
	}

	/* A 304 to a revalidation is the kept body, fresh again. */
	if (request->response.status == 304 && request->validating != NULL) {
		request->validating->stored_at = loader_clock();
		if (request->response.max_age >= 0)
			request->validating->max_age = request->response.max_age;
		error = loader_cache_answer(request, request->validating);
		if (error != 0) {
			wb_buffer_release(&location);
			return error;
		}
	}

	/* A response that is not a redirect with a place to go ends the request at this URL. */
	redirected = net_http_is_redirect(request->response.status);
	if (!redirected || location.length == 0) {
		wb_buffer_release(&location);
		wb_buffer_clear(&request->response.url);
		error = net_url_serialize(&request->url, 0, &request->response.url);
		if (error != 0)
			return error;
		loader_cache_store(request);
		loader_end(request, 0);
		return 0;
	}

	/* Too many redirects. */
	if (request->redirects == LOADER_REDIRECTS) {
		wb_buffer_release(&location);
		return ELOOP;
	}

	/* The next URL, resolved against this one, which must be http or https. */
	error = net_url_parse(wb_buffer_string(&location), location.length, &request->url, &next);
	wb_buffer_release(&location);
	if (error != 0)
		return error;
	is_web = net_http_is_web(next.scheme);
	if (!is_web) {
		net_url_release(&next);
		return EPROTONOSUPPORT;
	}

	/* A 303, or a 301 or 302 of a POST, is asked again with GET and no body; the others keep their method. */
	if (request->response.status == 303 || ((request->response.status == 301 || request->response.status == 302)
	    && request->method != NULL && strcmp(request->method, "POST") == 0)) {
		free(request->method);
		request->method = NULL;
		free(request->body);
		request->body = NULL;
		request->body_length = 0;
		request->has_body = 0;
	}

	/* The request starts again there. */
	net_url_release(&request->url);
	request->url = next;
	request->redirects++;
	error = loader_begin(request);
	if (error != 0)
		return error;

	/* Succeeded: the lookup of the next host waits (the step loop stops here). */
	return EAGAIN;
}

/* Closes a request's connection and frees its addresses (it may start another). */
static void
loader_close(
	struct net_request *request)
{
	/* TLS, then the socket. */
	if (request->tls != NULL)
		net_tls_close(request->tls);
	request->tls = NULL;
	if (request->descriptor >= 0)
		close(request->descriptor);
	request->descriptor = -1;
	request->wants = 0;

	/* The addresses of the lookup. */
	if (request->addresses != NULL)
		freeaddrinfo(request->addresses);
	request->addresses = NULL;
	request->address = NULL;
}

/*
 * Ends a request with an outcome: its connection closed, the TLS reason of
 * an https failure kept, its place given up, and the request goes back to
 * the caller (whose thread calls its callback).
 */
static void
loader_end(
	struct net_request *request,
	int error)
{
	int differs;

	/* Ended once. */
	if (request->state == LOADER_ENDED || request->cancelled)
		return;
	loader_close(request);
	request->state = LOADER_ENDED;
	request->error = error;

	/* The reason an https request failed, as this thread's TLS kept it. */
	differs = strcmp(request->url.scheme, "https");
	if (error != 0 && differs == 0)
		snprintf(request->reason, sizeof(request->reason), "%s", net_tls_error());

	/* A request waiting for its place may go. */
	loader_release_place(request->loader, request->key);

	/* Back to the caller. */
	loader_retire(request);
}

/* Puts a request that ended or was cancelled on the list handed to the caller next (once). */
static void
loader_retire(
	struct net_request *request)
{
	struct net_loader *loader;

	/* Already on it. */
	if (request->retired)
		return;

	/* At the end of the list, in the order the requests ended. */
	loader = request->loader;
	request->retired = 1;
	request->queue_next = NULL;
	if (loader->ended_last != NULL)
		loader->ended_last->queue_next = request;
	else
		loader->ended = request;
	loader->ended_last = request;
}

/* Takes the requests that went back to the caller out of the network thread's list. */
static void
loader_reap(
	struct net_loader *loader)
{
	struct net_request **link;
	struct net_request *request;

	/* Each request that went back leaves the list. */
	link = &loader->requests;
	while (*link != NULL) {
		request = *link;
		if (!request->retired) {
			link = &request->next;
			continue;
		}

		/* Unlinked; the caller frees it. */
		*link = request->next;
		request->next = NULL;
	}
}

/* Frees a request and what it holds. */
static void
loader_free(
	struct net_request *request)
{
	/* The connection, the URL and the buffers. */
	loader_close(request);
	net_url_release(&request->url);
	wb_buffer_release(&request->request);
	wb_buffer_release(&request->raw);
	net_response_release(&request->response);
	free(request->method);
	free(request->headers);
	free(request->body);
	free(request);
}

/*
 * Prepares a request to go out over its connection: its text (keeping the
 * connection, and If-None-Match for a revalidation), nothing sent or read
 * yet, and a new time out.
 */
static int
loader_prepare(
	struct net_request *request)
{
	struct net_send send;
	struct wb_buffer extra;
	const char *lines;
	int error;

	/* If-None-Match with the kept response's ETag, for a revalidation. */
	wb_buffer_init(&extra);
	lines = NULL;
	if (request->validating != NULL) {
		error = wb_buffer_printf(&extra, "If-None-Match: %s\r\n", wb_buffer_string(&request->validating->etag));
		if (error != 0) {
			wb_buffer_release(&extra);
			return error;
		}

		/* The line goes after the fixed headers. */
		lines = wb_buffer_string(&extra);
	}

	/* The text, keeping the connection. */
	wb_buffer_clear(&request->request);
	send.method = request->method;
	send.headers = request->headers;
	send.body = request->has_body ? (request->body != NULL ? request->body : (const unsigned char *)"") : NULL;
	send.body_length = request->body_length;
	error = net_http_request_text_send(&request->url, &request->request, 1, lines, request->kind, &send);
	wb_buffer_release(&extra);
	if (error != 0)
		return error;

	/* Nothing sent or read, and a new time out. */
	request->sent = 0;
	wb_buffer_clear(&request->raw);
	net_http_framing_init(&request->framing);
	request->deadline = loader_clock() + LOADER_TIMEOUT;

	/* Succeeded: the request can go out. */
	return 0;
}

/*
 * Replaces a kept connection that turned out closed by a new connection
 * (once): the request starts again from its lookup.
 */
static int
loader_retry(
	struct net_request *request)
{
	int error;

	/* A request whose kept connection failed twice fails. */
	if (request->retried)
		return ECONNRESET;

	/* The old connection goes, and the request starts again without the pool. */
	loader_close(request);
	request->retried = 1;
	request->reused = 0;
	error = loader_begin(request);
	if (error != 0)
		return error;

	/* Succeeded: the request waits for its new connection. */
	return EAGAIN;
}

/* Counts the requests that hold or are opening a connection to a place. */
static int
loader_active(
	const struct net_loader *loader,
	const char *key)
{
	const struct net_request *request;
	int count;
	int differs;

	/* Each request that is past waiting and not over. */
	count = 0;
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->state == LOADER_ENDED)
			continue;
		if (request->state == LOADER_READY || request->state == LOADER_WAITING)
			continue;
		differs = strcmp(request->key, key);
		if (differs == 0)
			count++;
	}

	/* The count. */
	return count;
}

/* Starts the first request that waits for a place, when the place has room again. */
static void
loader_release_place(
	struct net_loader *loader,
	const char *key)
{
	struct net_request *request;
	int differs;
	int error;

	/* A place without a key (a request that never started) frees nothing. */
	if (key[0] == '\0')
		return;

	/* The first request waiting for the place starts; one that cannot fails. */
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->state != LOADER_WAITING)
			continue;
		differs = strcmp(request->key, key);
		if (differs != 0)
			continue;
		error = loader_begin(request);
		if (error != 0)
			loader_end(request, error);
		break;
	}
}

/* Puts a request's connection in the pool of idle connections (the request no longer holds it). */
static void
loader_keep(
	struct net_request *request)
{
	struct net_loader *loader;
	struct loader_connection *kept;

	/* The pool's entry; without memory the connection is simply closed. */
	loader = request->loader;
	kept = calloc(1, sizeof(*kept));
	if (kept == NULL)
		return;
	memcpy(kept->key, request->key, sizeof(kept->key));
	kept->descriptor = request->descriptor;
	kept->tls = request->tls;
	kept->idle_since = loader_clock();

	/* The connection is the pool's now. */
	request->descriptor = -1;
	request->tls = NULL;
	request->wants = 0;
	kept->next = loader->idle;
	loader->idle = kept;
}

/* Closes the idle connections that have been idle since before a time (LOADER_NEVER: all of them). */
static void
loader_expire_idle(
	struct net_loader *loader,
	uint64_t now)
{
	struct loader_connection **link;
	struct loader_connection *kept;

	/* Each idle connection. */
	link = &loader->idle;
	while (*link != NULL) {
		kept = *link;
		if (now != LOADER_NEVER && kept->idle_since + LOADER_IDLE > now) {
			link = &kept->next;
			continue;
		}

		/* Unlinked and closed. */
		*link = kept->next;
		if (kept->tls != NULL)
			net_tls_close(kept->tls);
		close(kept->descriptor);
		free(kept);
	}
}

/* Finds a URL's entry in the cache, and makes it the most recently used. */
static struct loader_entry *
loader_cache_find(
	struct net_loader *loader,
	const char *url)
{
	struct loader_entry **link;
	struct loader_entry *entry;
	int differs;

	/* Each entry. */
	for (link = &loader->cache; *link != NULL; link = &(*link)->next) {
		differs = strcmp((*link)->url, url);
		if (differs != 0)
			continue;

		/* Moved to the front. */
		entry = *link;
		*link = entry->next;
		entry->next = loader->cache;
		loader->cache = entry;
		return entry;
	}

	/* The URL is not kept. */
	return NULL;
}

/* Tells whether an entry answers without the network: kept for less than its max-age, and not no-cache. */
static int
loader_cache_fresh(
	const struct loader_entry *entry,
	uint64_t now)
{
	/* No entry, no max-age, or no-cache. */
	if (entry == NULL || entry->max_age <= 0 || entry->no_cache)
		return 0;

	/* Younger than its max-age. */
	if (now < entry->stored_at + (uint64_t)entry->max_age * 1000U)
		return 1;

	/* Stale. */
	return 0;
}

/* Answers a request from a cache entry: a 200 with its Content-Type and body. */
static int
loader_cache_answer(
	struct net_request *request,
	const struct loader_entry *entry)
{
	int error;

	/* The response the entry keeps, at the request's URL. */
	request->response.status = 200;
	wb_buffer_clear(&request->response.content_type);
	wb_buffer_clear(&request->response.body);
	wb_buffer_clear(&request->response.url);
	error = wb_buffer_append(&request->response.content_type, entry->content_type.data, entry->content_type.length);
	if (error == 0)
		error = wb_buffer_append(&request->response.body, entry->body.data, entry->body.length);
	if (error == 0)
		error = net_url_serialize(&request->url, 0, &request->response.url);
	if (error != 0)
		return error;

	/* Succeeded: the request has its response. */
	return 0;
}

/*
 * Keeps a request's final 200 response in the cache when it may be kept
 * (a max-age or an ETag, and not no-store), in place of an older one of
 * the same URL; the least recently used go when the bodies are too many.
 */
static void
loader_cache_store(
	struct net_request *request)
{
	struct net_loader *loader;
	struct loader_entry *entry;
	struct wb_buffer url;
	int error;

	/* Only a 200 that says it may be kept, and not larger than the cache. */
	loader = request->loader;
	if (!request->plain || request->response.status != 200 || request->response.no_store)
		return;
	if (request->response.max_age <= 0 && request->response.etag.length == 0)
		return;
	if (request->response.body.length > LOADER_CACHE_MAX)
		return;

	/* The URL, the key; an older response of it goes. */
	wb_buffer_init(&url);
	error = net_url_serialize(&request->url, 1, &url);
	if (error != 0) {
		wb_buffer_release(&url);
		return;
	}

	/* An older entry of the URL is replaced (a revalidated one stays). */
	entry = loader_cache_find(loader, wb_buffer_string(&url));
	if (entry != NULL && entry != request->validating)
		loader_cache_free(loader, entry);
	if (entry != NULL && entry == request->validating) {
		/* A revalidated entry was refreshed already. */
		wb_buffer_release(&url);
		return;
	}

	/* The entry. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		wb_buffer_release(&url);
		return;
	}

	/* Its URL, type, body, ETag, time and freshness. */
	entry->url = strdup(wb_buffer_string(&url));
	wb_buffer_release(&url);
	wb_buffer_init(&entry->content_type);
	wb_buffer_init(&entry->body);
	wb_buffer_init(&entry->etag);
	error = ENOMEM;
	if (entry->url != NULL)
		error = wb_buffer_append(&entry->content_type, request->response.content_type.data, request->response.content_type.length);
	if (error == 0)
		error = wb_buffer_append(&entry->body, request->response.body.data, request->response.body.length);
	if (error == 0)
		error = wb_buffer_append(&entry->etag, request->response.etag.data, request->response.etag.length);
	entry->stored_at = loader_clock();
	entry->max_age = request->response.max_age;
	entry->no_cache = request->response.no_cache;

	/* At the front; an entry that could not be made is dropped. */
	entry->next = loader->cache;
	loader->cache = entry;
	loader->cache_bytes += entry->body.length;
	if (error != 0) {
		loader_cache_free(loader, entry);
		return;
	}

	/* The least recently used go until the bodies fit. */
	while (loader->cache_bytes > LOADER_CACHE_MAX) {
		entry = loader->cache;
		while (entry->next != NULL)
			entry = entry->next;
		loader_cache_free(loader, entry);
	}
}

/* Takes an entry out of the cache and frees it (no request revalidates it any more). */
static void
loader_cache_free(
	struct net_loader *loader,
	struct loader_entry *entry)
{
	struct loader_entry **link;
	struct net_request *request;

	/* Unlinked. */
	for (link = &loader->cache; *link != NULL; link = &(*link)->next) {
		if (*link == entry) {
			*link = entry->next;
			break;
		}
	}

	/* The requests that revalidate it no longer do. */
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->validating == entry)
			request->validating = NULL;
	}

	/* Its bytes, then it. */
	loader->cache_bytes -= entry->body.length;
	free(entry->url);
	wb_buffer_release(&entry->content_type);
	wb_buffer_release(&entry->body);
	wb_buffer_release(&entry->etag);
	free(entry);
}
