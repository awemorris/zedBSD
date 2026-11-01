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
 * made without a view, the default instance of the calling thread (made
 * now when it is the thread's first; NULL without memory).
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

	/* Otherwise the thread's default one, which the page keeps from now on. */
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
 * use a cookie jar (NULL for none); notify (which may be NULL) is called
 * with context from the loader's thread when there is news for
 * page_net_process.
 */
int
page_net_create(
	struct net_cookie_jar *jar,
	void (*notify)(void *context),
	void *context,
	struct net_loader **loader)
{
	int error;

	/* The network's loader. */
	error = net_loader_create(notify, context, loader);
	if (error != 0)
		return error;

	/* Its requests' cookies, before the first request. */
	net_loader_set_cookies(*loader, jar);

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
