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
 *   browser-net       one: waits on the connections, the resolver's
 *                     waker and a control waker the caller rings (as does
 *                     net_loader_ring, from any thread, for a media
 *                     engine's news); runs every request's states, the
 *                     pool and the cache, and the time outs.
 *   browser-resolve   one: the system's resolver, which blocks; the answer
 *                     comes back through its waker.
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
 * with the kept body.  Behind it an instance's persistent cache (cache.c),
 * when the loader has one, keeps the same responses across runs: a
 * response put in the memory cache is queued to be stored too, a URL the
 * memory cache lacks is looked for there (the network thread waits for the
 * disk, holding no lock), and what it finds goes into the memory cache and
 * is judged as any entry is, by the wall clock since the response was
 * stored.
 *
 * The caller makes requests and cancels them on its thread; the network
 * thread takes them from a list under the loader's lock.  An ended request
 * goes back on another list and is freed by the caller's thread after its
 * callback (a cancelled one without it, once the network thread has
 * closed its connection).  A callback may start other requests and cancel
 * any request, itself included.
 */

#include "net/net.h"
#include "base/os-net.h"
#include "base/os-system.h"
#include "base/os-time.h"
#include "base/thread.h"
#include "storage/records.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
	struct wb_os_addresses *addresses;
	int status;
};

/*
 * An idle connection of the pool: the place it goes to (its key), its
 * socket and TLS, and since when it has been idle.
 */
struct loader_connection {
	struct loader_connection *next;
	char key[LOADER_KEY_MAX];
	wb_os_socket socket;
	struct net_tls *tls;
	uint64_t idle_since;
};

/*
 * A response of the memory cache: its URL (the key), what it gives back
 * (the Content-Type and the body), its ETag, when it was stored or last
 * revalidated, and how long it stays fresh (max-age in seconds, -1 for
 * none; no_cache revalidates it every time).
 *
 * The time is kept twice: stored_at on the loader's monotonic clock, and
 * stored_wall on the wall clock (milliseconds since 1970 UTC, which is
 * what the persistent cache's record keeps).  An entry read from the
 * persistent cache has no meaningful stored_at (the monotonic clock does
 * not span runs), so wall_clock is set and its age is the wall clock's;
 * a revalidation gives it a new stored_at and clears wall_clock.
 */
struct loader_entry {
	struct loader_entry *next;
	char *url;
	struct wb_buffer content_type;
	struct wb_buffer body;
	struct wb_buffer etag;
	uint64_t stored_at;
	int64_t stored_wall;
	int wall_clock;
	long max_age;
	int no_cache;
};

/*
 * One request: its number, the URL it asks for now (after the redirects so
 * far), its state and connection (the addresses and the index of the one
 * tried now, the socket,
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
	struct wb_os_addresses *addresses;
	size_t address_index;
	size_t address_count;
	wb_os_socket socket;
	struct net_tls *tls;
	unsigned wants;
	char key[LOADER_KEY_MAX];
	int reused;
	int retried;
	struct wb_buffer request;
	size_t sent;
	struct wb_buffer raw;
	struct net_http_framing framing;
	struct loader_entry *validating;
	int revalidated;
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
 * number, the items of its wait (items, room for items_capacity), and
 * whether net_loader_ring rang since the caller was last told (rang).
 *
 * The resolver's thread has its queue of jobs, its list of answers and
 * resolver_stopping under resolver_lock (resolver_wake tells it a job is
 * there), and rings the answers waker for each answer.
 *
 * Under lock, the caller's thread and the network thread meet: the
 * requests started and not yet taken by the network thread (incoming),
 * whether a cancel was asked of a running request (cancel_asked), the
 * requests ended and not yet taken by the caller (completed), whether the
 * caller was told of news it has not taken (notified), whether
 * net_loader_ring was called since the network thread last looked (rung),
 * and whether the network thread is to stop.  changed is signalled when
 * notified becomes set.  Ringing the control waker wakes the network
 * thread from its wait.
 *
 * The caller's own: its notify function and context (set when the loader
 * is made), how many requests it started that have neither ended to it
 * nor been cancelled (pending), and the cookie jar of the requests
 * (cookies, set before the first request and read by the network thread;
 * the jar has its own lock), and the persistent cache behind the memory
 * cache (disk, set and read the same way; NULL for none, with its own
 * lock).
 */
struct net_loader {
	struct net_request *requests;
	struct net_request *ended;
	struct net_request *ended_last;
	struct loader_connection *idle;
	struct loader_entry *cache;
	size_t cache_bytes;
	uint64_t next_number;
	struct wb_os_poll_item *items;
	size_t items_capacity;
	int rang;
	struct wb_thread resolver;
	struct wb_mutex resolver_lock;
	struct wb_cond resolver_wake;
	struct loader_job *jobs;
	struct loader_job *answers;
	int resolver_stopping;
	struct wb_os_waker *answers_waker;
	struct wb_thread thread;
	struct wb_mutex lock;
	struct wb_cond changed;
	struct net_request *incoming;
	struct net_request *incoming_last;
	struct net_request *completed;
	struct net_request *completed_last;
	int cancel_asked;
	int notified;
	int rung;
	int stopping;
	struct wb_os_waker *control;
	net_loader_notify notify;
	void *notify_context;
	size_t pending;
	struct net_cookie_jar *cookies;
	struct net_cache *disk;
};

static uint64_t loader_clock(void);
static void loader_poke(struct net_loader *loader);
static void loader_release(struct net_loader *loader);
static void loader_free_queue(struct net_request *queue);
static int loader_locks(struct net_loader *loader);
static void loader_locks_release(struct net_loader *loader);
static void loader_resolver(void *argument);
static void loader_run(void *argument);
static void loader_ready(struct net_loader *loader, size_t count);
static int loader_intake(struct net_loader *loader);
static void loader_tick(struct net_loader *loader);
static void loader_publish(struct net_loader *loader);
static size_t loader_gather(struct net_loader *loader, int *timeout);
static int loader_timeout(const struct net_loader *loader);
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
static size_t loader_connections(const struct net_loader *loader, const struct net_request *except);
static void loader_release_place(struct net_loader *loader, const char *key);
static void loader_keep(struct net_request *request);
static void loader_expire_idle(struct net_loader *loader, uint64_t now);
static struct loader_entry *loader_cache_find(struct net_loader *loader, const char *url);
static int loader_cache_fresh(const struct loader_entry *entry, uint64_t now);
static int loader_cache_answer(struct net_request *request, const struct loader_entry *entry);
static void loader_cache_store(struct net_request *request);
static void loader_cache_free(struct net_loader *loader, struct loader_entry *entry);
static struct loader_entry *loader_cache_insert(struct net_loader *loader, const char *url, const struct wb_storage_cache_entry *record);
static struct loader_entry *loader_cache_load(struct net_loader *loader, const char *url);
static void loader_cache_persist(struct net_loader *loader, const struct loader_entry *entry);
static int64_t loader_wall(void);

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
	int status;

	/* The loader, with no waker yet. */
	*loader = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->next_number = 1;
	made->notify = notify;
	made->notify_context = context;

	/* Its locks and conditions (the caller's timed waits measure the time on the monotonic clock themselves). */
	status = loader_locks(made);
	if (status != 0) {
		free(made);
		return status;
	}

	/* The items of the network thread's wait, with room for some connections. */
	made->items_capacity = 2U + LOADER_PER_HOST * 4U;
	made->items = calloc(made->items_capacity, sizeof(*made->items));
	if (made->items == NULL) {
		loader_release(made);
		return ENOMEM;
	}

	/* The waker the resolver's answers are signalled through. */
	status = wb_os_waker_open(&made->answers_waker);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* The control waker that wakes the network thread. */
	status = wb_os_waker_open(&made->control);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* The resolver's thread. */
	status = wb_thread_start("browser-resolve", 0, loader_resolver, made, &made->resolver);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* It runs, and the network thread starts. */
	status = wb_thread_start("browser-net", 0, loader_run, made, &made->thread);
	if (status != 0) {
		loader_release(made);
		return status;
	}

	/* Succeeded: the loader takes requests. */
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
 * Gives a loader the cookie jar its requests send and keep the cookies
 * of (NULL, as a new loader has, for none).  Called before the loader's
 * first request: the network thread reads it once the request has
 * reached it under the loader's lock.  The jar outlives the loader.
 */
void
net_loader_set_cookies(
	struct net_loader *loader,
	struct net_cookie_jar *jar)
{
	/* The jar of every request from now on. */
	loader->cookies = jar;
}

/*
 * Gives a loader the persistent cache behind its memory cache (NULL, as a
 * new loader has, for none).  Called before the loader's first request,
 * as net_loader_set_cookies is.  The cache outlives the loader.
 */
void
net_loader_set_cache(
	struct net_loader *loader,
	struct net_cache *cache)
{
	/* The cache of every request from now on. */
	loader->disk = cache;
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
	made->socket = WB_OS_SOCKET_NONE;
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
	wb_mutex_lock(&loader->lock);

	if (loader->incoming_last != NULL)
		loader->incoming_last->queue_next = made;
	else
		loader->incoming = made;
	loader->incoming_last = made;

	wb_mutex_unlock(&loader->lock);

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
	wb_mutex_lock(&loader->lock);

	request->asked_cancel = 1;
	loader->cancel_asked = 1;

	wb_mutex_unlock(&loader->lock);

	/* The network thread wakes to do so. */
	loader_poke(loader);
}

/* Tells how a request ended: 0, or an errno value (EPROTO for TLS, whose reason net_request_reason gives). */
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

/* Tells why a request failed over TLS ("" when it did not), valid in its callback. */
const char *
net_request_reason(
	const struct net_request *request)
{
	/* The reason the network thread kept. */
	return request->reason;
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
	uint64_t end;
	uint64_t now;
	uint64_t left;
	uint32_t milliseconds;

	/* The end of the wait on the monotonic clock. */
	end = 0;
	if (timeout > 0)
		end = wb_os_monotonic_ns() + (uint64_t)timeout * 1000000U;

	/* Until the network thread tells of news, or the time is up. */
	wb_mutex_lock(&loader->lock);

	/* Each wake of the condition, until there is news or the time is up. */
	while (!loader->notified) {
		if (timeout < 0) {
			wb_cond_wait(&loader->changed, &loader->lock);
			continue;
		}

		/* The time left, measured again on the monotonic clock after each wake. */
		now = wb_os_monotonic_ns();
		if (timeout == 0 || now >= end)
			break;
		left = end - now;

		/* The wait for the time left, in whole milliseconds rounded up so that a short wait still sleeps. */
		milliseconds = (uint32_t)(left / 1000000U);
		if (left % 1000000U != 0)
			milliseconds++;
		(void)wb_cond_wait_for(&loader->changed, &loader->lock, milliseconds);
	}

	wb_mutex_unlock(&loader->lock);
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
	/* All of them, however long their callbacks take. */
	(void)net_loader_dispatch_until(loader, UINT64_MAX);
}

/*
 * Does what net_loader_dispatch does until the monotonic clock
 * (wb_os_monotonic_ns) reaches a deadline: a callback that ends after it
 * is the last one called, and the requests after it stay, in their order,
 * for the next call.  Returns whether some stay.
 */
int
net_loader_dispatch_until(
	struct net_loader *loader,
	uint64_t deadline)
{
	struct net_request *taken;
	struct net_request *request;
	struct net_request *last;

	/* The ended requests, all of them, and the caller is told of the next news again. */
	wb_mutex_lock(&loader->lock);

	taken = loader->completed;
	loader->completed = NULL;
	loader->completed_last = NULL;
	loader->notified = 0;

	wb_mutex_unlock(&loader->lock);

	/* Each request in turn: its callback, then it goes. */
	while (taken != NULL) {
		request = taken;
		taken = request->queue_next;

		/* One cancelled meanwhile, even by an earlier callback, only goes. */
		if (!request->dropped) {
			request->delivered = 1;
			loader->pending--;
			if (request->done != NULL)
				request->done(request->context, request);
		}

		/* The request is freed. */
		loader_free(request);

		/* Past the deadline the rest wait. */
		if (taken != NULL && wb_os_monotonic_ns() >= deadline)
			break;
	}

	/* Nothing left. */
	if (taken == NULL)
		return 0;

	/* The rest go back before any that ended meanwhile. */
	for (last = taken; last->queue_next != NULL; last = last->queue_next)
		;
	wb_mutex_lock(&loader->lock);
	last->queue_next = loader->completed;
	if (loader->completed == NULL)
		loader->completed_last = last;
	loader->completed = taken;
	wb_mutex_unlock(&loader->lock);
	return 1;
}

/*
 * Tells the caller of news of its own (a media engine's state changed or
 * a picture came): the network thread wakes and tells the caller as it
 * tells of an ended request (notify, or a wake of net_loader_wait).  Any
 * thread may call it; it takes only the loader's lock.
 */
void
net_loader_ring(
	struct net_loader *loader)
{
	/* The mark the network thread takes at its next round. */
	wb_mutex_lock(&loader->lock);

	loader->rung = 1;

	wb_mutex_unlock(&loader->lock);

	/* The network thread wakes to take it. */
	loader_poke(loader);
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
	/* The monotonic clock. */
	return wb_os_monotonic_ns() / 1000000U;
}

/* The resolver's thread: each job's lookup, answered through the list and the waker, until the loader stops. */
static void
loader_resolver(
	void *argument)
{
	struct net_loader *loader;
	struct loader_job *job;

	/* The loader the thread looks names up for. */
	loader = argument;

	/* Each job, as they come. */
	for (;;) {
		/* The next job, waited for. */
		wb_mutex_lock(&loader->resolver_lock);

		/* Waits until a lookup job arrives or shutdown releases the resolver. */
		while (loader->jobs == NULL && !loader->resolver_stopping)
			wb_cond_wait(&loader->resolver_wake, &loader->resolver_lock);
		job = loader->jobs;
		if (job != NULL)
			loader->jobs = job->next;

		wb_mutex_unlock(&loader->resolver_lock);

		/* A loader that stops ends the thread. */
		if (job == NULL)
			break;

		/* The lookup, which may take long. */
		job->addresses = NULL;
		job->status = wb_os_resolve(job->host, job->port, &job->addresses);

		/* The answer, then the waker rung to wake the network thread. */
		wb_mutex_lock(&loader->resolver_lock);

		job->next = loader->answers;
		loader->answers = job;

		wb_mutex_unlock(&loader->resolver_lock);

		/* The ring. */
		wb_os_waker_ring(loader->answers_waker);
	}
}

/* Wakes the network thread from its wait (a ring before the last is drained adds nothing). */
static void
loader_poke(
	struct net_loader *loader)
{
	/* The control waker. */
	wb_os_waker_ring(loader->control);
}

/*
 * Stops the loader's threads (those that started) and frees everything it
 * holds: the requests in every list, the jobs and answers, the idle
 * connections, the cache, the wakers and the loader.
 */
static void
loader_release(
	struct net_loader *loader)
{
	struct net_request *request;
	struct loader_job *job;

	/* The network thread, told to stop; it ends after its round. */
	wb_mutex_lock(&loader->lock);

	loader->stopping = 1;

	wb_mutex_unlock(&loader->lock);

	/* Woken from its wait, then waited for (a thread that never started is passed over). */
	if (loader->thread.handle != NULL)
		loader_poke(loader);
	wb_thread_join(&loader->thread);

	/* The resolver's thread, told to stop. */
	wb_mutex_lock(&loader->resolver_lock);

	loader->resolver_stopping = 1;
	wb_cond_signal(&loader->resolver_wake);

	wb_mutex_unlock(&loader->resolver_lock);

	/* It ends after its current lookup. */
	wb_thread_join(&loader->resolver);

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
		wb_os_addresses_free(job->addresses);
		free(job);
	}

	/* The idle connections. */
	loader_expire_idle(loader, LOADER_NEVER);

	/* The cache. */
	while (loader->cache != NULL)
		loader_cache_free(loader, loader->cache);

	/* The wakers that were made. */
	wb_os_waker_close(loader->answers_waker);
	wb_os_waker_close(loader->control);

	/* The locks, the wait's items, then the loader. */
	loader_locks_release(loader);
	free(loader->items);
	free(loader);
}

/* Makes the loader's locks and conditions; on failure none is left. */
static int
loader_locks(
	struct net_loader *loader)
{
	int error;

	/* The resolver's lock and the condition its jobs come by. */
	error = wb_mutex_init(&loader->resolver_lock);
	if (error == 0)
		error = wb_cond_init(&loader->resolver_wake);

	/* The loader's lock and the condition its news comes by. */
	if (error == 0)
		error = wb_mutex_init(&loader->lock);
	if (error == 0)
		error = wb_cond_init(&loader->changed);

	/* A failure lets go of those made. */
	if (error != 0) {
		loader_locks_release(loader);
		return error;
	}

	/* Succeeded: the locks are made. */
	return 0;
}

/* Lets go of the loader's locks and conditions (those never made are passed over). */
static void
loader_locks_release(
	struct net_loader *loader)
{
	/* The resolver's, then the loader's. */
	wb_cond_destroy(&loader->resolver_wake);
	wb_mutex_destroy(&loader->resolver_lock);
	wb_cond_destroy(&loader->changed);
	wb_mutex_destroy(&loader->lock);
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
 * and a wait on the wakers and the connections, until the loader stops.
 */
static void
loader_run(
	void *argument)
{
	struct net_loader *loader;
	size_t count;
	size_t ready;
	int timeout;
	int status;
	int stop;

	/* The loader the thread runs, and nothing waited on yet. */
	loader = argument;
	count = 0;

	/* Each round, until the loader stops. */
	for (;;) {
		/* What the last wait found ready: the resolver's answers, the connections. */
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
		status = wb_os_poll(loader->items, count, timeout, &ready);
		if (status != 0)
			count = 0;
	}

	/* The ended requests leave the list; net_loader_destroy frees every request. */
	loader_reap(loader);
}

/*
 * Takes what the last wait found ready (the first count items): the
 * control waker drained, the resolver's answers, and the next steps of
 * each ready connection.
 */
static void
loader_ready(
	struct net_loader *loader,
	size_t count)
{
	struct net_request *request;
	size_t index;

	/* The control waker's bytes; what the caller asked is read under the lock. */
	wb_os_waker_drain(loader->control);

	/* The resolver's answers. */
	loader_answers(loader);

	/* Each ready connection takes its next steps. */
	for (index = 2U; index < count; index++) {
		if (loader->items[index].found == 0)
			continue;

		/* The request whose connection it is. */
		for (request = loader->requests; request != NULL; request = request->next) {
			if (request->cancelled || request->socket != loader->items[index].socket)
				continue;
			loader_step(request);
			break;
		}
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
	wb_mutex_lock(&loader->lock);

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

	/* Whether net_loader_ring rang: the caller is told at this round. */
	if (loader->rung) {
		loader->rang = 1;
		loader->rung = 0;
	}

	/* Whether the caller ends the loader. */
	stop = loader->stopping;

	wb_mutex_unlock(&loader->lock);

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
 * and tells the caller of them or of a ring of net_loader_ring: its
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
	wb_mutex_lock(&loader->lock);

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
	wb_cond_broadcast(&loader->changed);

	wb_mutex_unlock(&loader->lock);

	/* The network thread holds none of them now. */
	loader->ended = NULL;
	loader->ended_last = NULL;
	loader->rang = 0;

	/* The caller's notify, outside the lock. */
	if (tell && loader->notify != NULL)
		loader->notify(loader->notify_context);
}

/*
 * Lists the items of the next wait: the control waker, the resolver's
 * waker, then each connection for what it waits for.  *timeout is how long
 * the wait may last.  Returns how many items were listed.
 */
static size_t
loader_gather(
	struct net_loader *loader,
	int *timeout)
{
	struct net_request *request;
	struct wb_os_poll_item *grown;
	size_t needed;
	size_t count;
	int cut;

	/* Room for the two wakers and every request's connection, within the most one wait takes. */
	needed = 2U;
	for (request = loader->requests; request != NULL; request = request->next)
		needed++;
	if (needed > WB_OS_POLL_MAX)
		needed = WB_OS_POLL_MAX;

	/* More room when there is not enough (without memory, the connections left out are waited on soon after). */
	if (needed > loader->items_capacity) {
		grown = realloc(loader->items, needed * sizeof(*grown));
		if (grown != NULL) {
			loader->items = grown;
			loader->items_capacity = needed;
		}
	}

	/* The control waker, then the resolver's. */
	loader->items[0].socket = wb_os_waker_socket(loader->control);
	loader->items[0].wants = WB_OS_POLL_READABLE;
	loader->items[0].found = 0;
	loader->items[1].socket = wb_os_waker_socket(loader->answers_waker);
	loader->items[1].wants = WB_OS_POLL_READABLE;
	loader->items[1].found = 0;
	count = 2U;

	/* Each connection that waits for its socket, as many as there is room for. */
	cut = 0;
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request->cancelled || request->socket == WB_OS_SOCKET_NONE || request->wants == 0)
			continue;
		if (count == loader->items_capacity) {
			cut = 1;
			break;
		}

		/* Waits on this connection's socket. */
		loader->items[count].socket = request->socket;
		loader->items[count].wants = request->wants;
		loader->items[count].found = 0;
		count++;
	}

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
	size_t running;
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

	/* The URL's entry, in memory or else in the persistent cache, answering when it is fresh. */
	entry = NULL;
	if (request->plain) {
		entry = loader_cache_find(loader, wb_buffer_string(&url));
		if (entry == NULL)
			entry = loader_cache_load(loader, wb_buffer_string(&url));
	}
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
	request->revalidated = 0;
	if (entry != NULL && entry->etag.length != 0)
		request->validating = entry;

	/* No more connections than one wait takes, its two wakers aside: the request fails. */
	running = loader_connections(loader, request);
	if (running >= WB_OS_POLL_MAX - 2U)
		return EMFILE;

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
		request->socket = kept->socket;
		request->tls = kept->tls;
		request->reused = 1;
		free(kept);

		/* The request goes out over it. */
		error = loader_prepare(request);
		if (error != 0)
			return error;
		request->state = LOADER_SENDING;
		request->wants = WB_OS_POLL_WRITABLE;
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
	wb_mutex_lock(&loader->resolver_lock);

	job->next = loader->jobs;
	loader->jobs = job;
	wb_cond_signal(&loader->resolver_wake);

	wb_mutex_unlock(&loader->resolver_lock);

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

	/* The waker's bytes, all of them. */
	wb_os_waker_drain(loader->answers_waker);

	/* The answers, taken at once. */
	wb_mutex_lock(&loader->resolver_lock);

	answers = loader->answers;
	loader->answers = NULL;

	wb_mutex_unlock(&loader->resolver_lock);

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
			wb_os_addresses_free(job->addresses);
			free(job);
			continue;
		}

		/* A host that has no address fails the request. */
		if (job->status != 0 || job->addresses == NULL) {
			wb_os_addresses_free(job->addresses);
			free(job);
			loader_end(request, EHOSTUNREACH);
			continue;
		}

		/* The addresses, tried in turn from the first. */
		request->addresses = job->addresses;
		request->address_index = 0;
		request->address_count = wb_os_addresses_count(job->addresses);
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
	int status;
	int error;

	/* The addresses from the current one. */
	error = ECONNREFUSED;
	for (; request->address_index < request->address_count; request->address_index++) {
		status = wb_os_socket_connect(request->addresses, request->address_index, 0, &request->socket);
		if (status == 0 || status == EINPROGRESS) {
			request->wants = WB_OS_POLL_WRITABLE;
			request->deadline = loader_clock() + LOADER_TIMEOUT;
			return 0;
		}

		/* An address that refuses at once gives way to the next (its socket is closed already). */
		error = status;
		request->socket = WB_OS_SOCKET_NONE;
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
	int failure;
	int is_https;
	int error;

	/* The connection's outcome; one not finished yet (a connection has a peer) waits again. */
	failure = wb_os_socket_connect_result(request->socket);
	if (failure == EINPROGRESS)
		return EAGAIN;

	/* A refused or failed connection moves to the next address. */
	if (failure != 0) {
		wb_os_socket_close(request->socket);
		request->socket = WB_OS_SOCKET_NONE;
		request->address_index++;
		if (request->address_index >= request->address_count)
			return failure;
		error = loader_connect(request);
		if (error != 0)
			return error;
		return EAGAIN;
	}

	/* The addresses are no longer needed. */
	wb_os_addresses_free(request->addresses);
	request->addresses = NULL;
	request->address_index = 0;
	request->address_count = 0;

	/* The request's text. */
	error = loader_prepare(request);
	if (error != 0)
		return error;

	/* https starts TLS over the connection; http sends at once. */
	is_https = !strcmp(request->url.scheme, "https");
	if (is_https) {
		request->reason[0] = '\0';
		error = net_tls_start(request->socket, request->url.host, request->reason, &request->tls);
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
		error = wb_os_socket_send(request->socket, bytes, length, &sent);
		if (error == EAGAIN) {
			request->wants = WB_OS_POLL_WRITABLE;
			return EAGAIN;
		}

		/* A kept connection the server closed is replaced once. */
		if (error != 0 && request->reused) {
			error = loader_retry(request);
			return error;
		}

		/* Any other failure ends the request. */
		if (error != 0)
			return error;
	}

	/* The bytes went; all of them means the response comes next. */
	request->sent += sent;
	request->deadline = loader_clock() + LOADER_TIMEOUT;
	if (request->sent < request->request.length)
		return 0;

	/* Succeeded: the request is sent, and the response is read. */
	request->state = LOADER_RECEIVING;
	request->wants = WB_OS_POLL_READABLE;
	return 0;
}

/* Reads what the connection has now; its end finishes the response. */
static int
loader_receive(
	struct net_request *request)
{
	unsigned char chunk[LOADER_CHUNK];
	size_t received;
	int error;

	/* Over TLS, or the socket. */
	received = 0;
	if (request->tls != NULL) {
		error = net_tls_read_some(request->tls, chunk, sizeof(chunk), &received, &request->wants);
		if (error != 0)
			return error;
	} else {
		error = wb_os_socket_receive(request->socket, chunk, sizeof(chunk), &received);
		if (error == EAGAIN) {
			request->wants = WB_OS_POLL_READABLE;
			return EAGAIN;
		}

		/* Any other failure ends the request. */
		if (error != 0)
			return error;
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
	error = net_http_parse_response(request->loader->cookies, &request->url, &request->raw, &request->response, &location);
	wb_buffer_clear(&request->raw);
	if (error != 0) {
		wb_buffer_release(&location);
		return error;
	}

	/* A 304 to a revalidation is the kept body, fresh again. */
	if (request->response.status == 304 && request->validating != NULL) {
		request->validating->stored_at = loader_clock();
		request->validating->stored_wall = loader_wall();
		request->validating->wall_clock = 0;
		request->revalidated = 1;
		if (request->response.max_age >= 0)
			request->validating->max_age = request->response.max_age;
		error = loader_cache_answer(request, request->validating);
		if (error != 0) {
			wb_buffer_release(&location);
			return error;
		}

		/* The persistent cache keeps the response as freshly revalidated. */
		loader_cache_persist(request->loader, request->validating);
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
	wb_os_socket_close(request->socket);
	request->socket = WB_OS_SOCKET_NONE;
	request->wants = 0;

	/* The addresses of the lookup. */
	wb_os_addresses_free(request->addresses);
	request->addresses = NULL;
	request->address_index = 0;
	request->address_count = 0;
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
	const char *kept;

	/* Ended once. */
	if (request->state == LOADER_ENDED || request->cancelled)
		return;

	/* The reason an https request's connection failed, kept before the connection closes (a failure of its start is kept already). */
	if (error != 0 && request->tls != NULL) {
		kept = net_tls_reason(request->tls);
		if (kept[0] != '\0')
			snprintf(request->reason, sizeof(request->reason), "%s", kept);
	}

	/* The connection closes, and the request has ended. */
	loader_close(request);
	request->state = LOADER_ENDED;
	request->error = error;

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
	error = net_http_request_text_send(request->loader->cookies, &request->url, &request->request, 1, lines, request->kind, &send);
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

/*
 * Counts the requests but one that are past waiting and not over (each
 * holds or will hold a connection the network thread waits on).
 */
static size_t
loader_connections(
	const struct net_loader *loader,
	const struct net_request *except)
{
	const struct net_request *request;
	size_t count;

	/* Each request that is past waiting and not over, but the one asking. */
	count = 0;
	for (request = loader->requests; request != NULL; request = request->next) {
		if (request == except || request->cancelled || request->state == LOADER_ENDED)
			continue;
		if (request->state == LOADER_READY || request->state == LOADER_WAITING)
			continue;
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
	kept->socket = request->socket;
	kept->tls = request->tls;
	kept->idle_since = loader_clock();

	/* The connection is the pool's now. */
	request->socket = WB_OS_SOCKET_NONE;
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
		wb_os_socket_close(kept->socket);
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
	int64_t age;

	/* No entry, no max-age, or no-cache. */
	if (entry == NULL || entry->max_age <= 0 || entry->no_cache)
		return 0;

	/* An entry of an earlier run is as old as the wall clock says; a clock set back makes it stale. */
	if (entry->wall_clock) {
		age = loader_wall() - entry->stored_wall;
		if (age < 0)
			return 0;
		if (age < (int64_t)entry->max_age * 1000)
			return 1;
		return 0;
	}

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
	struct wb_storage_cache_entry record;
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

	/* An entry the 304 revalidated was refreshed already, and stays. */
	entry = loader_cache_find(loader, wb_buffer_string(&url));
	if (entry != NULL && entry == request->validating && request->revalidated) {
		wb_buffer_release(&url);
		return;
	}

	/* An older entry of the URL is replaced (by a 200 to its revalidation too, whose body changed). */
	if (entry != NULL)
		loader_cache_free(loader, entry);

	/* The response as a record: what the memory entry keeps, stamped with the wall clock. */
	memset(&record, 0, sizeof(record));
	record.url = wb_buffer_string(&url);
	record.url_length = url.length;
	record.content_type = (const char *)request->response.content_type.data;
	record.content_type_length = request->response.content_type.length;
	record.etag = (const char *)request->response.etag.data;
	record.etag_length = request->response.etag.length;
	record.stored = loader_wall();
	record.max_age = request->response.max_age;
	if (record.max_age < -1)
		record.max_age = -1;
	record.no_cache = request->response.no_cache != 0;
	record.body = request->response.body.data;
	record.body_length = request->response.body.length;

	/* The memory cache takes it, the least recently used going when the bodies are too many. */
	entry = loader_cache_insert(loader, record.url, &record);
	if (entry == NULL) {
		wb_buffer_release(&url);
		return;
	}

	/* The persistent cache queues it to be stored too. */
	loader_cache_persist(loader, entry);
	wb_buffer_release(&url);
}

/*
 * Puts a response in the memory cache, at the front, and drops the least
 * recently used entries until the bodies fit.  The entry's URL, type,
 * ETag and body are copies; its time is the response's stored time on
 * the wall clock and now on the monotonic one.  Returns the entry, or
 * NULL for lack of memory (nothing is kept).  The response's body is at
 * most LOADER_CACHE_MAX.
 */
static struct loader_entry *
loader_cache_insert(
	struct net_loader *loader,
	const char *url,
	const struct wb_storage_cache_entry *record)
{
	struct loader_entry *entry;
	struct loader_entry *last;
	int error;

	/* The entry. */
	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;

	/* Its URL, type, body and ETag. */
	entry->url = strdup(url);
	wb_buffer_init(&entry->content_type);
	wb_buffer_init(&entry->body);
	wb_buffer_init(&entry->etag);
	error = ENOMEM;
	if (entry->url != NULL)
		error = wb_buffer_append(&entry->content_type, record->content_type, record->content_type_length);
	if (error == 0)
		error = wb_buffer_append(&entry->body, record->body, record->body_length);
	if (error == 0)
		error = wb_buffer_append(&entry->etag, record->etag, record->etag_length);

	/* Its time and freshness. */
	entry->stored_at = loader_clock();
	entry->stored_wall = record->stored;
	entry->max_age = (long)record->max_age;
	entry->no_cache = record->no_cache;

	/* At the front; an entry that could not be made is dropped. */
	entry->next = loader->cache;
	loader->cache = entry;
	loader->cache_bytes += entry->body.length;
	if (error != 0) {
		loader_cache_free(loader, entry);
		return NULL;
	}

	/* The least recently used go until the bodies fit (the new entry, at the front, stays). */
	while (loader->cache_bytes > LOADER_CACHE_MAX) {
		last = loader->cache;
		while (last->next != NULL)
			last = last->next;
		if (last == entry)
			break;
		loader_cache_free(loader, last);
	}

	/* Succeeded: the memory cache has the entry. */
	return entry;
}

/*
 * Finds a URL's response in the persistent cache and puts it in the
 * memory cache, as an entry the wall clock dates.  Waits for the disk
 * (only the network thread calls it, holding no lock).  Returns the
 * entry, or NULL when there is no persistent cache, the URL is not in
 * it, or it cannot be had.
 */
static struct loader_entry *
loader_cache_load(
	struct net_loader *loader,
	const char *url)
{
	struct wb_storage_cache_entry record;
	struct loader_entry *entry;
	struct wb_buffer bytes;
	int found;
	int error;

	/* No persistent cache. */
	if (loader->disk == NULL)
		return NULL;

	/* The URL's record, checked to be the URL's. */
	wb_buffer_init(&bytes);
	found = 0;
	error = net_cache_read(loader->disk, url, strlen(url), &bytes, &found);
	if (error != 0 || !found) {
		wb_buffer_release(&bytes);
		return NULL;
	}

	/* The record, which the cache has decoded once already. */
	error = wb_storage_cache_decode(bytes.data, bytes.length, &record);
	if (error != 0 || record.body_length > LOADER_CACHE_MAX) {
		wb_buffer_release(&bytes);
		return NULL;
	}

	/* The memory cache takes it, dated by the wall clock until it is revalidated. */
	entry = loader_cache_insert(loader, url, &record);
	wb_buffer_release(&bytes);
	if (entry == NULL)
		return NULL;
	entry->wall_clock = 1;

	/* Succeeded: the entry answers or is revalidated like any other. */
	return entry;
}

/*
 * Queues an entry of the memory cache to be stored in the persistent
 * cache, as it is now (its stored time included).  Nothing waits; a
 * failure only means the response is not kept across runs.
 */
static void
loader_cache_persist(
	struct net_loader *loader,
	const struct loader_entry *entry)
{
	struct wb_storage_cache_entry record;

	/* No persistent cache. */
	if (loader->disk == NULL)
		return;

	/* The entry as a record. */
	memset(&record, 0, sizeof(record));
	record.url = entry->url;
	record.url_length = strlen(entry->url);
	record.content_type = (const char *)entry->content_type.data;
	record.content_type_length = entry->content_type.length;
	record.etag = (const char *)entry->etag.data;
	record.etag_length = entry->etag.length;
	record.stored = entry->stored_wall;
	record.max_age = entry->max_age;
	if (record.max_age < -1)
		record.max_age = -1;
	record.no_cache = entry->no_cache != 0;
	record.body = entry->body.data;
	record.body_length = entry->body.length;

	/* The cache queues the write; an error leaves the response in memory only. */
	(void)net_cache_store(loader->disk, &record);
}

/* Reads the wall clock, in milliseconds since 1970 UTC. */
static int64_t
loader_wall(void)
{
	/* The wall clock. */
	return wb_os_wall_ns() / INT64_C(1000000);
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
