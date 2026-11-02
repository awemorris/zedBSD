/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network side of browser (plan/ws074/design.md §9): the
 * URL (the WHATWG URL Standard's parser and serializer, url.c), its hosts
 * (host.c: domains, IPv4 and IPv6 addresses, the punycode of
 * internationalized domains), data: URLs (data.c), HTTP/1.1 (http.c), TLS
 * (tls.c), cookies (cookie.c) and the asynchronous loader (loader.c).
 *
 * A URL's parts are kept as they are serialized: ASCII strings, already
 * percent-encoded, which the caller owns through the URL and frees with
 * net_url_release.
 */

#ifndef KEILAND_BROWSER_NET_H
#define KEILAND_BROWSER_NET_H

#include "base/base.h"
#include "base/os-net.h"

/*
 * A parsed URL.
 *
 * scheme is in lower case without its colon.  host is NULL for a URL
 * without one, and the empty string for an empty host (file:///).  port
 * is -1 when there is none (or it is the scheme's default).  path is the
 * serialized path: the segments each after a slash, or, when opaque_path
 * is set, the opaque path as it is (mailto:, data:).  query and fragment
 * are NULL when the URL has none (they may be empty).
 */
struct net_url {
	char *scheme;
	char *username;
	char *password;
	char *host;
	int port;
	char *path;
	int opaque_path;
	char *query;
	char *fragment;
};

/*
 * The parts of a URL the URL interface reports (net_url_component).
 */
enum net_url_part {
	NET_URL_HREF,
	NET_URL_ORIGIN,
	NET_URL_PROTOCOL,
	NET_URL_USERNAME,
	NET_URL_PASSWORD,
	NET_URL_HOST,
	NET_URL_HOSTNAME,
	NET_URL_PORT,
	NET_URL_PATHNAME,
	NET_URL_SEARCH,
	NET_URL_HASH
};

/*
 * A data: URL's body: its MIME type (serialized, such as
 * "text/plain;charset=US-ASCII") and its bytes.
 */
struct net_data {
	struct wb_buffer mime;
	struct wb_buffer body;
};

/*
 * The response of an HTTP fetch: the final URL (after the redirects), the
 * status, the Content-Type header's value, the body (decoded from
 * chunks), and what the response says of caching it: Cache-Control's
 * max-age in seconds (-1 without one), no-store and no-cache, and its
 * ETag (empty without one).
 */
struct net_response {
	struct wb_buffer url;
	int status;
	struct wb_buffer content_type;
	struct wb_buffer body;
	long max_age;
	int no_store;
	int no_cache;
	struct wb_buffer etag;
	/* The response headers but Set-Cookie, one "Name: value" per line, each ending in LF (ws074-p232). */
	struct wb_buffer headers;
};

/*
 * What a script's request adds to a plain GET (ws074-p232): the method
 * (NULL is GET), extra header lines (each "Name: value" ending in CR LF,
 * NULL for none; the fixed headers a name replaces are left out, and
 * the connection's own, such as Host, are ignored) and the body (NULL for none).
 */
struct net_send {
	const char *method;
	const char *headers;
	const unsigned char *body;
	size_t body_length;
};

/*
 * How far the bytes of a response read so far go (net_http_framing_update):
 * whether the headers are complete and where the body starts, how the body
 * ends (its length, its chunks, or the end of the connection), how far the
 * chunks have been walked, whether the connection may carry another request
 * after it, and whether the response is complete.
 */
struct net_http_framing {
	int headers_done;
	size_t body_start;
	int chunked;
	int until_close;
	size_t body_end;
	size_t position;
	int keep_alive;
	int done;
};

/* A request of the asynchronous loader, and the loader. */
struct net_loader;
struct net_request;

/* An instance's cookie jar (cookie.c) and the storage queue it keeps its persistent cookies in (storage/queue.h). */
struct net_cookie_jar;
struct wb_storage_queue;

/* An instance's persistent HTTP cache (cache.c), and the record of one stored response (storage/records.h). */
struct net_cache;
struct wb_storage_cache_entry;

/* A request's callback: called once when it ends (net_request_error tells how), from net_loader_dispatch. */
typedef void (*net_request_done)(void *context, struct net_request *request);

/*
 * The loader's notify: called from the loader's network thread when there
 * is news for net_loader_dispatch (once, until the caller has run it).
 */
typedef void (*net_loader_notify)(void *context);

/* The longest reason of a TLS failure kept (with its NUL). */
#define NET_TLS_REASON_MAX	256U

/* URLs (url.c). */
int net_url_parse(const char *input, size_t length, const struct net_url *base, struct net_url *url);
void net_url_release(struct net_url *url);
int net_url_is_special(const struct net_url *url);
int net_url_default_port(const char *scheme);
int net_url_serialize(const struct net_url *url, int exclude_fragment, struct wb_buffer *out);
int net_url_component(const struct net_url *url, int part, struct wb_buffer *out);
int net_url_file_path(const struct net_url *url, struct wb_buffer *out);
int net_url_from_file_path(const char *path, struct wb_buffer *out);
const char *net_user_agent(void);
int net_percent_decode(const char *text, size_t length, struct wb_buffer *out);

/* Hosts (host.c). */
int net_host_parse(const char *input, size_t length, int opaque, struct wb_buffer *out);

/* HTTP (http.c). */
/* The cookies of a request go to and come from a jar (NULL for none). */
int net_http_fetch(struct net_cookie_jar *jar, const char *url, struct net_response *response);
int net_http_fetch_send(struct net_cookie_jar *jar, const char *url, const struct net_send *send, struct net_response *response);
void net_http_set_kind(int kind);
int net_http_request_text(struct net_cookie_jar *jar, const struct net_url *url, struct wb_buffer *request, int keep_alive, const char *extra);
int net_http_request_text_kind(struct net_cookie_jar *jar, const struct net_url *url, struct wb_buffer *request, int keep_alive, const char *extra, int kind);
int net_http_request_text_send(struct net_cookie_jar *jar, const struct net_url *url, struct wb_buffer *request, int keep_alive, const char *extra, int kind, const struct net_send *send);

/* What a request is for: the Accept header follows it (ws074-p230). */
#define NET_FETCH_DOCUMENT	0
#define NET_FETCH_IMAGE		1
#define NET_FETCH_SCRIPT_API	2
void net_response_init(struct net_response *response);
void net_http_framing_init(struct net_http_framing *framing);
void net_http_framing_update(struct net_http_framing *framing, const unsigned char *raw, size_t length);
int net_http_parse_response(struct net_cookie_jar *jar, const struct net_url *url, const struct wb_buffer *raw, struct net_response *response, struct wb_buffer *location);
int net_http_is_redirect(int status);
int net_http_is_web(const char *scheme);
void net_response_release(struct net_response *response);

/* TLS for https (tls.c): the OpenSSL package's library, loaded when first needed. */
struct net_tls;
int net_tls_add_ca_file(const char *path);
int net_tls_open(wb_os_socket socket, const char *host, struct net_tls **tls);
int net_tls_start(wb_os_socket socket, const char *host, struct net_tls **tls);
int net_tls_handshake(struct net_tls *tls, unsigned *wants);
int net_tls_read_some(struct net_tls *tls, unsigned char *bytes, size_t length, size_t *received, unsigned *wants);
int net_tls_write_some(struct net_tls *tls, const unsigned char *bytes, size_t length, size_t *sent, unsigned *wants);
int net_tls_read(struct net_tls *tls, unsigned char *bytes, size_t length, size_t *received);
int net_tls_write(struct net_tls *tls, const unsigned char *bytes, size_t length);
int net_tls_pending(const struct net_tls *tls);
void net_tls_close(struct net_tls *tls);
void net_tls_clear_error(void);
void net_tls_set_error(const char *reason);
const char *net_tls_error(void);

/* The asynchronous loader (loader.c). */
int net_loader_create(net_loader_notify notify, void *context, struct net_loader **loader);
void net_loader_destroy(struct net_loader *loader);
/* The jar of the loader's requests (NULL, the start, for none); set before the first request. */
void net_loader_set_cookies(struct net_loader *loader, struct net_cookie_jar *jar);
/* The persistent cache behind the loader's memory cache (NULL, the start, for none); set before the first request. */
void net_loader_set_cache(struct net_loader *loader, struct net_cache *cache);
int net_loader_fetch(struct net_loader *loader, const char *url, net_request_done done, void *context, struct net_request **request);
int net_loader_fetch_kind(struct net_loader *loader, const char *url, int kind, net_request_done done, void *context, struct net_request **request);
int net_loader_send(struct net_loader *loader, const char *url, int kind, const struct net_send *send, net_request_done done, void *context, struct net_request **request);
void net_request_cancel(struct net_request *request);
int net_request_error(const struct net_request *request);
const struct net_response *net_request_response(const struct net_request *request);
size_t net_loader_pending(const struct net_loader *loader);
void net_loader_wait(struct net_loader *loader, int timeout);
void net_loader_dispatch(struct net_loader *loader);
int net_loader_dispatch_until(struct net_loader *loader, uint64_t deadline);
/* Called from a media engine's thread too: sets a mark and wakes the network thread, nothing more. */
void net_loader_ring(struct net_loader *loader);
int net_loader_takes(const char *location);

/* Cookies (cookie.c): an instance's jar over its storage queue; a NULL jar keeps and sends nothing. */
int net_cookie_jar_open(struct wb_storage_queue *queue, struct net_cookie_jar **jar);
void net_cookie_jar_close(struct net_cookie_jar *jar);
int net_cookie_store(struct net_cookie_jar *jar, const struct net_url *url, const char *header, size_t length);
int net_cookie_header(struct net_cookie_jar *jar, const struct net_url *url, struct wb_buffer *out);
int net_cookie_store_script(struct net_cookie_jar *jar, const struct net_url *url, const char *text, size_t length);
int net_cookie_string(struct net_cookie_jar *jar, const struct net_url *url, struct wb_buffer *out);

/* The persistent HTTP cache (cache.c): an instance's index of the responses its storage keeps. */
int net_cache_open(struct wb_storage_queue *queue, size_t limit, struct net_cache **cache);
void net_cache_close(struct net_cache *cache);
int net_cache_read(struct net_cache *cache, const char *url, size_t length, struct wb_buffer *record, int *found);
int net_cache_store(struct net_cache *cache, const struct wb_storage_cache_entry *entry);

/* data: URLs (data.c). */
int net_data_parse(const struct net_url *url, struct net_data *data);
void net_data_release(struct net_data *data);

#endif
