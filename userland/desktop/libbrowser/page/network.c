/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page's side of the asynchronous loader: the embedder (the view)
 * makes a loader, gives it to its pages, runs its callbacks when the
 * loader tells it of news (or waits for the news, headless), and fetches
 * documents with it.  These calls keep the embedder off the network module
 * itself.
 */

#include "page/page.h"
#include "net/net.h"
#include "storage/instance.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The loader's client of the calling thread when it is not the loader's own (page_set_thread_client): NULL on the thread that made the loader. */
static _Thread_local struct net_loader_client *network_thread_client;

/*
 * Fetches a resource a page needs at once (a parser-blocking script the
 * scan did not find, a synchronous XMLHttpRequest, a frame's script): an
 * http or https one through the page's loader, which does the network on
 * its own thread while this thread waits for the request (the waiting
 * thread never touches a socket), anything else, or any resource of a
 * page without a loader, read as page_fetch reads it.  href is resolved
 * against the page's base; bytes receives the body and final_url (which
 * may be NULL) the URL after the redirects.  Returns 0 or an errno value.
 */
int
page_fetch_now(
	struct page *page,
	const char *href,
	int kind,
	struct wb_buffer *bytes,
	struct wb_buffer *final_url)
{
	struct net_response response;
	struct wb_buffer location;
	int remote;
	int error;

	/* A page with no base resolves nothing. */
	if (page->base == NULL)
		return EINVAL;

	/* The location, and whether the loader takes it. */
	wb_buffer_init(&location);
	error = page_resolve_location(page->base, href, &location);
	if (error != 0) {
		wb_buffer_release(&location);
		return error;
	}

	/* Whether the loader takes the location. */
	remote = net_loader_takes(wb_buffer_string(&location));

	/* Without the loader, or a local or data resource: read at once. */
	if (page->loader == NULL || !remote) {
		wb_buffer_release(&location);
		error = page_fetch(page_cookies(page), page->base, href, bytes, final_url);
		return error;
	}

	/* Through the loader, waited for. */
	error = page_fetch_response_now(page, wb_buffer_string(&location), kind, &response);
	wb_buffer_release(&location);
	if (error != 0)
		return error;

	/* The body and the final URL, then the response goes. */
	error = wb_buffer_append(bytes, response.body.data, response.body.length);
	if (error == 0 && final_url != NULL) {
		wb_buffer_clear(final_url);
		error = wb_buffer_append(final_url, response.url.data, response.url.length);
	}

	/* Gives the response back. */
	net_response_release(&response);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are there. */
	return 0;
}

/*
 * Fetches an http or https location (absolute) through a page's loader and
 * waits for it, filling a response as page_fetch_response does (on failure
 * the response is empty but for the reason of a TLS failure).  A page
 * without a loader, or a location the loader does not take, is read by
 * page_fetch_response.  Returns 0 or an errno value.
 */
int
page_fetch_response_now(
	struct page *page,
	const char *location,
	int kind,
	struct net_response *response)
{
	/* A GET without headers or a body of its own. */
	return page_send_response_now(page, location, kind, NULL, response);
}

/*
 * Makes a request of an http or https location (absolute) with the method,
 * headers and body of send (NULL: a plain GET) and waits for it, filling a
 * response as page_fetch_response_now does.  A page without a loader sends
 * it at once from this thread; a location the loader does not take is read
 * only for a plain GET (ENOTSUP otherwise).  Returns 0 or an errno value.
 */
int
page_send_response_now(
	struct page *page,
	const char *location,
	int kind,
	const struct net_send *send,
	struct net_response *response)
{
	struct net_request *request;
	struct net_loader_client *client;
	int method_order;
	int plain;
	int remote;
	int error;

	/* Without the loader, or a location it does not take: at once. */
	remote = net_loader_takes(location);
	plain = 1;
	if (send != NULL && send->method != NULL) {
		/* Whether the method is the plain GET. */
		method_order = strcmp(send->method, "GET");
		if (method_order != 0)
			plain = 0;
	}

	/* Sends a request of its own at once from this thread. */
	if (page->loader == NULL && remote && !plain) {
		error = net_http_fetch_send(page_cookies(page), location, send, response);
		return error;
	}

	/* Reads the location at once, which only a plain GET can be. */
	if (page->loader == NULL || !remote) {
		if (!plain) {
			net_response_init(response);
			return ENOTSUP;
		}
		error = page_fetch_response(page_cookies(page), location, location, response);
		return error;
	}

	/* The request of the calling thread's client, waited for. */
	net_response_init(response);
	client = page_client(page);
	error = net_loader_client_start_waited(client, location, kind, send, &request);
	if (error != 0)
		return error;
	error = net_request_wait(request);

	/* Its response (or only the reason of its failure), then the request goes. */
	if (error == 0)
		net_request_take_response(request, response);
	else
		snprintf(response->reason, sizeof(response->reason), "%s", net_request_reason(request));
	net_request_release(request);
	if (error != 0)
		return error;

	/* Succeeded: the response is the caller's. */
	return 0;
}

/*
 * Sets the client of the pages' loader the calling thread makes its
 * requests through (NULL: the loader's own, of the thread that made it):
 * a thread that runs the tasks of a page's frames sets its own, so that
 * the callbacks of its requests run on it.
 */
void
page_set_thread_client(
	struct net_loader_client *client)
{
	/* This thread's. */
	network_thread_client = client;
}

/* Reports the client of a page's loader the calling thread makes its requests through (the page has a loader). */
struct net_loader_client *
page_client(
	const struct page *page)
{
	/* The thread's own, or the loader's. */
	if (network_thread_client != NULL)
		return network_thread_client;
	return net_loader_own_client(page->loader);
}

/*
 * Gives a page the loader its http and https images are fetched with
 * (NULL reads them at once, blocking).
 */
void
page_set_loader(
	struct page *page,
	struct net_loader *loader)
{
	/* The loader the page's images use from now on. */
	page->loader = loader;
}

/*
 * Gives a page the instance whose cookies and Web Storage it uses (the
 * view's; it outlives the page).
 */
void
page_set_instance(
	struct page *page,
	struct browser_instance *instance)
{
	/* The instance the page's scripts and fetches use from now on. */
	page->instance = instance;
}

/*
 * Reports the instance a page uses: the one its view gave, or, for a page
 * made without a view, the process's default instance (made now when it
 * is the first; NULL without memory).
 */
struct browser_instance *
page_instance(
	struct page *page)
{
	struct browser_instance *instance;
	int error;

	/* The view's instance. */
	if (page->instance != NULL)
		return page->instance;

	/* Otherwise the process's default one, which the page keeps from now on. */
	error = wb_instance_default(&instance);
	if (error != 0)
		return NULL;

	/* Succeeded: the page's instance. */
	page->instance = instance;
	return instance;
}

/*
 * Reports the cookie jar of a page's instance (NULL when the page has no
 * instance for lack of memory: its fetches then send and keep no cookie).
 */
struct net_cookie_jar *
page_cookies(
	struct page *page)
{
	struct browser_instance *instance;

	/* The page's instance. */
	instance = page_instance(page);
	if (instance == NULL)
		return NULL;

	/* Succeeded: its jar. */
	return instance->cookies;
}

/*
 * Makes a loader (with its network and resolver threads) whose requests
 * use a cookie jar and a persistent cache (NULL for none); notify (which may be NULL) is called
 * with context from the loader's thread when there is news for
 * page_net_process.
 */
int
page_net_create(
	struct net_cookie_jar *jar,
	struct net_cache *cache,
	void (*notify)(void *context),
	void *context,
	struct net_loader **loader)
{
	int error;

	/* The network's loader. */
	error = net_loader_create(notify, context, loader);
	if (error != 0)
		return error;

	/* Its requests' cookies and persistent cache, before the first request. */
	net_loader_set_cookies(*loader, jar);
	net_loader_set_cache(*loader, cache);

	/* Succeeded: the loader takes requests. */
	return 0;
}

/*
 * Ends a loader and its requests (their callbacks are not called).
 */
void
page_net_destroy(
	struct net_loader *loader)
{
	/* The network's loader. */
	net_loader_destroy(loader);
}

/*
 * Tells how many of the loader's requests have not ended yet (cancelled
 * ones aside).
 */
size_t
page_net_pending(
	const struct net_loader *loader)
{
	size_t pending;

	/* The loader's count. */
	pending = net_loader_pending(loader);
	return pending;
}

/*
 * Waits until the loader has news for page_net_process, or for a number
 * of milliseconds at most (-1: until there is news).
 */
void
page_net_wait(
	struct net_loader *loader,
	int timeout)
{
	/* The loader's wait. */
	net_loader_wait(loader, timeout);
}

/*
 * Takes the loader's news: the callbacks of the requests that ended run
 * from here.
 */
void
page_net_process(
	struct net_loader *loader)
{
	/* The loader's callbacks. */
	net_loader_dispatch(loader);
}

/*
 * Tells whether a location is fetched through the loader (an http or
 * https URL) rather than read at once.
 */
int
page_net_is_remote(
	const char *location)
{
	int remote;

	/* http: and https: URLs. */
	remote = net_loader_takes(location);
	return remote;
}

/*
 * Starts fetching a document (or anything) from an http or https
 * location; done is called with context when it ends.
 */
int
page_net_fetch(
	struct net_loader *loader,
	const char *location,
	page_request_done done,
	void *context,
	struct net_request **request)
{
	int error;

	/* The loader's request. */
	error = net_loader_fetch(loader, location, done, context, request);
	if (error != 0)
		return error;

	/* Succeeded: the request runs. */
	return 0;
}

/*
 * Cancels a request (its callback is not called).
 */
void
page_net_cancel(
	struct net_request *request)
{
	/* The loader's cancel. */
	net_request_cancel(request);
}

/*
 * Tells why a request failed over TLS ("" when it did not), in its
 * callback.
 */
const char *
page_net_reason(
	const struct net_request *request)
{
	/* The loader's reason. */
	return net_request_reason(request);
}

/*
 * Reads how a request ended, in its callback: its error (0, or an errno
 * value), and for a response its body and final URL (a response of any
 * status is a document to show).
 */
int
page_net_result(
	const struct net_request *request,
	const unsigned char **bytes,
	size_t *length,
	const char **url)
{
	const struct net_response *response;
	int error;

	/* A request that failed has no response. */
	*bytes = NULL;
	*length = 0;
	*url = NULL;
	error = net_request_error(request);
	if (error != 0)
		return error;

	/* Succeeded: the response's body and final URL. */
	response = net_request_response(request);
	*bytes = response->body.data;
	*length = response->body.length;
	*url = wb_buffer_string(&response->url);
	return 0;
}
