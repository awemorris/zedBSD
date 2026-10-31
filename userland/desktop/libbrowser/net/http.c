/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HTTP/1.1 (RFC 9112), the first pass: a GET over a new TCP connection
 * per request (Connection: close), the response read to the end of the
 * connection, its body cut by Content-Length or decoded from chunks,
 * redirects followed (twenty at most), and the cookies of each response
 * kept and sent back (cookie.c).  An https URL's connection is TLS
 * (tls.c) under the same requests.
 *
 * net_http_fetch blocks the caller while it runs: the name lookup, the
 * connection and each read (a read waits NET_HTTP_TIMEOUT at most).  The
 * asynchronous loader (loader.c) makes the same requests and reads the
 * same responses without blocking, through net_http_request_text and
 * net_http_parse_response; persistent connections and the cache come
 * with ws074-p058.
 */

#include "net/net.h"

#include <compat/zlib/zlib.h>

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

/* How long a connection may stay silent, in milliseconds. */
#define NET_HTTP_TIMEOUT	30000

/* The most redirects a fetch follows. */
#define NET_HTTP_REDIRECTS	20

/* The largest response a fetch reads, in bytes. */
#define NET_HTTP_MAX_RESPONSE	((size_t)256U * 1024U * 1024U)

/*
 * The other headers of every request: the types a browser's navigation
 * accepts, the star, the slash and the star (written apart so that they do
 * not read as a comment) last; Japanese, then English; and no compression.
 * A server may answer "any type" with nothing a page can use: AWS WAF's
 * challenge (Amazon, BUG-227) sends its page, which gets the token, only
 * to a client that accepts HTML, and an empty 202 otherwise.
 */
#define NET_HTTP_HEADERS	"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*" "/" "*;q=0.8\r\n" \
				"Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n" \
				"Accept-Encoding: identity\r\n"

/* Writes a response's body into the directory BROWSER_NET_DUMP names, and lists it on the standard error. */
static void
http_dump(
	const char *url_text,
	const struct net_response *response)
{
	static unsigned counter;
	char path[512];
	const char *directory;
	FILE *file;

	directory = getenv("BROWSER_NET_DUMP");
	if (directory == NULL || directory[0] == '\0')
		return;
	snprintf(path, sizeof(path), "%s/%04u.bin", directory, counter++);
	file = fopen(path, "wb");
	if (file != NULL) {
		fwrite(response->body.data, 1U, response->body.length, file);
		fclose(file);
	}
	fprintf(stderr, "net: %s %d %s %s\n", path, response->status, response->content_type.data == NULL ? "" : wb_buffer_string(&response->content_type), url_text);
}

/* The fixed headers of an image's request: only the formats the decoders read. */
#define NET_HTTP_IMAGE_HEADERS	"Accept: image/png,image/jpeg\r\n" \
				"Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n" \
				"Accept-Encoding: identity\r\n"

/* The kind of the blocking fetches that follow (NET_FETCH_*); the browser is single-threaded. */
static int http_fetch_kind = NET_FETCH_DOCUMENT;

/* Sets what the blocking fetches that follow are for, so the Accept header follows. */
void
net_http_set_kind(
	int kind)
{
	http_fetch_kind = kind;
}

/*
 * What the headers of a response say that the fetch uses (besides the
 * Content-Type and the cookies, kept at once).
 */
struct http_headers {
	size_t content_length;
	int has_length;
	int chunked;
	int encoding;
};

/* The content codings of a response body that are decoded (the server may send gzip even when only identity was accepted). */
#define HTTP_ENCODING_NONE	0
#define HTTP_ENCODING_GZIP	1
#define HTTP_ENCODING_DEFLATE	2

/*
 * A request's connection: the socket, and its TLS for https (NULL for
 * http).
 */
struct http_connection {
	int descriptor;
	struct net_tls *tls;
};

static int http_request(const struct net_url *url, const struct net_send *send, struct net_response *response, struct wb_buffer *location);
static int http_decode_body(int encoding, struct net_response *response);
static int http_connect(const struct net_url *url, int *descriptor);
static int http_send_all(const struct http_connection *connection, const unsigned char *bytes, size_t length);
static int http_receive_all(const struct http_connection *connection, struct wb_buffer *raw);
static int http_receive(const struct http_connection *connection, unsigned char *bytes, size_t length, size_t *received);
static int http_is_web(const char *scheme);
static int http_header(const struct net_url *url, const char *line, size_t length, struct net_response *response, struct wb_buffer *location, struct http_headers *headers);
static int http_header_value(const char *line, size_t length, const char *name, const char **value, size_t *value_length);
static int http_dechunk(const unsigned char *body, size_t length, struct wb_buffer *out);
static void http_cache_control(const char *value, size_t length, struct net_response *response);
static int http_find_header(const unsigned char *raw, size_t end, const char *name, const char **value, size_t *value_length);
static const void *http_memmem(const void *haystack, size_t length, const void *needle, size_t needle_length);

/*
 * Fetches a URL with GET, following redirects: fills the response (its
 * final URL, status, Content-Type and body) or returns an errno value
 * (EPROTONOSUPPORT for a scheme other than http and https, or https
 * without the OpenSSL package; EPROTO for a TLS failure, whose reason
 * net_tls_error gives; EINVAL for a response
 * that is not HTTP, ELOOP for too many redirects).
 */
int
net_http_fetch(
	const char *url_text,
	struct net_response *response)
{
	return net_http_fetch_send(url_text, NULL, response);
}

/*
 * The same with what a script's request adds (send: the method, the header
 * lines and the body; NULL is a plain GET); a 303, or a 301 or 302 of a
 * POST, is asked again with GET and no body.
 */
int
net_http_fetch_send(
	const char *url_text,
	const struct net_send *send,
	struct net_response *response)
{
	struct net_send again;
	const struct net_send *current;
	struct net_url url;
	struct net_url next;
	struct wb_buffer location;
	int redirects;
	int is_web;
	int redirected;
	int error;

	/* The URL (and no TLS failure yet). */
	net_tls_clear_error();
	net_response_init(response);
	error = net_url_parse(url_text, strlen(url_text), NULL, &url);
	if (error != 0)
		return error;

	/* Each request, then the next one a redirect names. */
	wb_buffer_init(&location);
	current = send;
	for (redirects = 0;; redirects++) {
		/* Only http and https. */
		is_web = net_http_is_web(url.scheme);
		if (!is_web) {
			error = EPROTONOSUPPORT;
			break;
		}

		/* The request and its response. */
		wb_buffer_clear(&location);
		error = http_request(&url, current, response, &location);
		if (error != 0)
			break;

		/* A redirect with a place to go is followed; anything else is the response. */
		redirected = net_http_is_redirect(response->status);
		if (!redirected || location.length == 0)
			break;
		if (redirects == NET_HTTP_REDIRECTS) {
			error = ELOOP;
			break;
		}

		/* The method after a redirect. */
		if (current != NULL && (response->status == 303 || ((response->status == 301 || response->status == 302)
		    && current->method != NULL && strcmp(current->method, "POST") == 0))) {
			again = *current;
			again.method = NULL;
			again.body = NULL;
			again.body_length = 0;
			current = &again;
		}

		/* The next URL, resolved against this one. */
		error = net_url_parse(wb_buffer_string(&location), location.length, &url, &next);
		if (error != 0)
			break;
		net_url_release(&url);
		url = next;
	}

	/* The final URL, which relative URLs in the page resolve against. */
	if (error == 0)
		error = net_url_serialize(&url, 0, &response->url);
	net_url_release(&url);
	wb_buffer_release(&location);
	if (error != 0) {
		net_response_release(response);
		return error;
	}

	/* A developer's trace: BROWSER_NET_DUMP names a directory that receives every response body. */
	http_dump(url_text, response);

	/* Succeeded: the response is there. */
	return 0;
}

/*
 * Frees a response's buffers.
 */
void
net_response_release(
	struct net_response *response)
{
	/* The four buffers. */
	wb_buffer_release(&response->url);
	wb_buffer_release(&response->content_type);
	wb_buffer_release(&response->body);
	wb_buffer_release(&response->etag);
	wb_buffer_release(&response->headers);
}

/*
 * Starts a response empty: no status, no headers, no body, and nothing
 * said of caching (max_age -1).
 */
void
net_response_init(
	struct net_response *response)
{
	/* Everything cleared, with the buffers empty. */
	memset(response, 0, sizeof(*response));
	wb_buffer_init(&response->url);
	wb_buffer_init(&response->content_type);
	wb_buffer_init(&response->body);
	wb_buffer_init(&response->etag);
	wb_buffer_init(&response->headers);
	response->max_age = -1;
}

/*
 * Writes the text of a GET request for a URL: the request line, Host,
 * User-Agent, the fixed headers, Connection: close unless the connection
 * is to be kept, the extra header lines (NULL for none; each ends in CR LF)
 * and the URL's cookies.
 */
int
net_http_request_text(
	const struct net_url *url,
	struct wb_buffer *request,
	int keep_alive,
	const char *extra)
{
	return net_http_request_text_kind(url, request, keep_alive, extra, NET_FETCH_DOCUMENT);
}

/*
 * The same, for a kind of fetch: an image is asked for as PNG or JPEG only
 * (the decoders do not read WebP or AVIF, so a server that varies by Accept
 * answers with one that they do).
 */
int
net_http_request_text_kind(
	const struct net_url *url,
	struct wb_buffer *request,
	int keep_alive,
	const char *extra,
	int kind)
{
	return net_http_request_text_send(url, request, keep_alive, extra, kind, NULL);
}

/* Tells whether a header block (lines each ending in CR LF) has a header of the name. */
static int
http_has_header(
	const char *lines,
	const char *name)
{
	const char *end;
	size_t length;

	/* Each line's start against the name and a colon. */
	length = strlen(name);
	while (lines != NULL && *lines != '\0') {
		if (strncasecmp(lines, name, length) == 0 && lines[length] == ':')
			return 1;
		end = strchr(lines, '\n');
		if (end == NULL)
			break;
		lines = end + 1;
	}
	return 0;
}

/* Appends the header lines a script gave, but the ones the connection decides itself. */
static int
http_append_script_headers(
	struct wb_buffer *request,
	const char *lines)
{
	static const char *const forbidden[] = { "host", "content-length", "connection", "transfer-encoding",
	    "accept-encoding", "cookie", "user-agent", "te", "upgrade", "expect", NULL };
	const char *end;
	const char *colon;
	size_t length;
	size_t index;
	int skip;
	int error;

	/* Each line with a name before a colon, that is not forbidden. */
	error = 0;
	while (error == 0 && lines != NULL && *lines != '\0') {
		end = strchr(lines, '\n');
		length = end == NULL ? strlen(lines) : (size_t)(end - lines) + 1U;
		colon = memchr(lines, ':', length);
		skip = colon == NULL || colon == lines;
		for (index = 0; !skip && forbidden[index] != NULL; index++) {
			if ((size_t)(colon - lines) == strlen(forbidden[index]) && strncasecmp(lines, forbidden[index], (size_t)(colon - lines)) == 0)
				skip = 1;
		}
		if (!skip) {
			error = wb_buffer_append(request, lines, length);
			if (error == 0 && lines[length - 1U] != '\n')
				error = wb_buffer_append_string(request, "\r\n");
		}
		lines += length;
	}
	return error;
}

/*
 * The same, with what a script's request adds (send: the method, the
 * headers and the body; NULL for a plain GET).  For the script kind the
 * fixed Accept and Accept-Language are left out when the script gives them.
 */
int
net_http_request_text_send(
	const struct net_url *url,
	struct wb_buffer *request,
	int keep_alive,
	const char *extra,
	int kind,
	const struct net_send *send)
{
	const char *target;
	const char *method;
	const char *fixed;
	int error;

	/* The request line: the method, the path (at least "/") and the query. */
	method = "GET";
	if (send != NULL && send->method != NULL)
		method = send->method;
	target = url->path;
	if (target[0] == '\0')
		target = "/";
	error = wb_buffer_printf(request, "%s %s", method, target);
	if (error == 0 && url->query != NULL)
		error = wb_buffer_printf(request, "?%s", url->query);
	if (error == 0)
		error = wb_buffer_printf(request, " HTTP/1.1\r\nHost: %s", url->host);
	if (error == 0 && url->port >= 0)
		error = wb_buffer_printf(request, ":%d", url->port);
	if (error == 0)
		error = wb_buffer_printf(request, "\r\nUser-Agent: %s\r\n", net_user_agent());

	/* The fixed headers; a script's request keeps its own Accept and Accept-Language. */
	if (error == 0 && kind == NET_FETCH_SCRIPT_API) {
		if (!http_has_header(send != NULL ? send->headers : NULL, "accept"))
			error = wb_buffer_append_string(request, "Accept: *" "/" "*\r\n");
		if (error == 0 && !http_has_header(send != NULL ? send->headers : NULL, "accept-language"))
			error = wb_buffer_append_string(request, "Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n");
		if (error == 0)
			error = wb_buffer_append_string(request, "Accept-Encoding: identity\r\n");
	} else if (error == 0) {
		fixed = kind == NET_FETCH_IMAGE ? NET_HTTP_IMAGE_HEADERS : NET_HTTP_HEADERS;
		error = wb_buffer_append_string(request, fixed);
	}
	if (error == 0 && !keep_alive)
		error = wb_buffer_append_string(request, "Connection: close\r\n");
	if (error == 0 && send != NULL)
		error = http_append_script_headers(request, send->headers);
	if (error == 0 && extra != NULL)
		error = wb_buffer_append_string(request, extra);

	/* The length of a body (a POST without one says zero). */
	if (error == 0 && send != NULL && (send->body != NULL || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0))
		error = wb_buffer_printf(request, "Content-Length: %lu\r\n", (unsigned long)(send->body == NULL ? 0U : send->body_length));

	/* The cookies for the URL, and the end of the headers. */
	if (error == 0)
		error = net_cookie_header(url, request);
	if (error == 0)
		error = wb_buffer_append_string(request, "\r\n");
	if (error == 0 && send != NULL && send->body != NULL)
		error = wb_buffer_append(request, send->body, send->body_length);
	if (error != 0)
		return error;

	/* Succeeded: the request is written. */
	return 0;
}

/* Tells whether a scheme is one HTTP fetches: http or https. */
int
net_http_is_web(
	const char *scheme)
{
	int web;

	/* The two schemes. */
	web = http_is_web(scheme);
	return web;
}

/*
 * Starts the framing of a response with nothing read.
 */
void
net_http_framing_init(
	struct net_http_framing *framing)
{
	/* No headers, no body, not done. */
	memset(framing, 0, sizeof(*framing));
}

/*
 * Walks the bytes of a response read so far (raw, from its first byte) as
 * far as they go: the headers once they are complete (the status, the
 * length or the chunks, Connection), then the body, and sets done when the
 * response is complete.  A response whose body ends with the connection
 * is never done here (the end of the connection completes it), and its
 * connection is not kept.
 */
void
net_http_framing_update(
	struct net_http_framing *framing,
	const unsigned char *raw,
	size_t length)
{
	const unsigned char *end;
	const char *value;
	size_t value_length;
	size_t header_end;
	size_t line_end;
	size_t size;
	int status;
	int found;
	int differs;
	int digit;
	int c;

	/* The headers, once the empty line after them has come. */
	if (!framing->headers_done) {
		end = NULL;
		if (length >= 4U)
			end = (const unsigned char *)http_memmem(raw, length, "\r\n\r\n", 4U);
		if (end == NULL)
			return;
		header_end = (size_t)(end - raw);
		framing->headers_done = 1;
		framing->body_start = header_end + 4U;

		/* HTTP/1.1 keeps the connection unless it says close; 1.0 does not keep it. */
		status = 0;
		if (length >= 12U)
			status = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + (raw[11] - '0');
		framing->keep_alive = 0;
		if (length >= 8U && raw[7] == '1')
			framing->keep_alive = 1;
		found = http_find_header(raw, header_end, "connection", &value, &value_length);
		differs = 1;
		if (found && value_length == 5U)
			differs = strncasecmp(value, "close", 5U);
		if (differs == 0)
			framing->keep_alive = 0;

		/* A status without a body. */
		if (status == 204 || status == 304 || (status >= 100 && status < 200)) {
			framing->body_end = framing->body_start;
			framing->done = 1;
			return;
		}

		/* Chunks, a length, or the end of the connection. */
		found = http_find_header(raw, header_end, "transfer-encoding", &value, &value_length);
		differs = 1;
		if (found && value_length >= 7U)
			differs = strncasecmp(value + value_length - 7U, "chunked", 7U);
		if (differs == 0) {
			framing->chunked = 1;
			framing->position = framing->body_start;
		} else {
			found = http_find_header(raw, header_end, "content-length", &value, &value_length);
			if (found) {
				framing->body_end = framing->body_start + (size_t)strtoul(value, NULL, 10);
			} else {
				/* The end of the connection ends the body, and the connection with it. */
				framing->until_close = 1;
				framing->keep_alive = 0;
			}
		}
	}

	/* A body cut by its length is complete when all of it has come. */
	if (!framing->chunked) {
		if (!framing->until_close && length >= framing->body_end)
			framing->done = 1;
		return;
	}

	/* Chunks: each size line, its bytes and CR LF, from where the walk stopped. */
	while (!framing->done) {
		/* The size line, when all of it has come. */
		line_end = framing->position;
		while (line_end < length && raw[line_end] != '\n')
			line_end++;
		if (line_end >= length)
			return;
		size = 0;
		for (value_length = framing->position; value_length < line_end; value_length++) {
			c = raw[value_length];
			digit = -1;
			if (c >= '0' && c <= '9')
				digit = c - '0';
			if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
				digit = (c | 0x20) - 'a' + 10;
			if (digit < 0)
				break;
			size = size * 16U + (size_t)digit;
		}

		/* The last chunk without trailers: the CR LF right after its line ends the response. */
		if (size == 0 && line_end + 3U <= length && raw[line_end + 1U] == '\r' && raw[line_end + 2U] == '\n') {
			framing->body_end = line_end + 3U;
			framing->done = 1;
			return;
		}

		/* The last chunk with trailers: their empty line ends it. */
		if (size == 0) {
			end = (const unsigned char *)http_memmem(raw + line_end + 1U, length - line_end - 1U, "\r\n\r\n", 4U);
			if (end == NULL)
				return;
			framing->body_end = (size_t)(end - raw) + 4U;
			framing->done = 1;
			return;
		}

		/* The chunk's bytes and its CR LF, when they have all come. */
		if (line_end + 1U + size + 2U > length)
			return;
		framing->position = line_end + 1U + size + 2U;
	}
}

/* Sends one GET and reads its response; a redirect's Location goes to location. */
static int
http_request(
	const struct net_url *url,
	const struct net_send *send,
	struct net_response *response,
	struct wb_buffer *location)
{
	struct http_connection connection;
	struct wb_buffer request;
	struct wb_buffer raw;
	int is_https;
	int error;

	/* The request's text. */
	wb_buffer_init(&request);
	wb_buffer_init(&raw);
	error = net_http_request_text_send(url, &request, 0, NULL, send != NULL ? NET_FETCH_SCRIPT_API : http_fetch_kind, send);
	if (error != 0) {
		wb_buffer_release(&request);
		return error;
	}

	/* The connection, and TLS over it for https. */
	connection.tls = NULL;
	connection.descriptor = -1;
	error = http_connect(url, &connection.descriptor);
	is_https = !strcmp(url->scheme, "https");
	if (error == 0 && is_https)
		error = net_tls_open(connection.descriptor, url->host, &connection.tls);

	/* The request and everything the server sends until it closes. */
	if (error == 0)
		error = http_send_all(&connection, request.data, request.length);
	if (error == 0)
		error = http_receive_all(&connection, &raw);
	net_tls_close(connection.tls);
	if (connection.descriptor >= 0)
		close(connection.descriptor);

	/* The request was sent. */
	wb_buffer_release(&request);

	/* The response. */
	if (error == 0)
		error = net_http_parse_response(url, &raw, response, location);
	wb_buffer_release(&raw);
	if (error != 0)
		return error;

	/* Succeeded: the response is read. */
	return 0;
}

/* Looks up a URL's host and connects to its port (the first address that answers). */
static int
http_connect(
	const struct net_url *url,
	int *descriptor)
{
	struct addrinfo hints;
	struct addrinfo *found;
	struct addrinfo *address;
	char host[256];
	char port[16];
	size_t length;
	int number;
	int status;
	int error;

	/* The host without an IPv6 address's brackets. */
	length = strlen(url->host);
	if (length >= sizeof(host))
		return ENAMETOOLONG;
	memcpy(host, url->host, length + 1U);
	if (length >= 2U && host[0] == '[') {
		memmove(host, host + 1, length - 2U);
		host[length - 2U] = '\0';
	}

	/* The port, or the scheme's. */
	number = net_url_default_port(url->scheme);
	if (url->port >= 0)
		number = url->port;
	snprintf(port, sizeof(port), "%d", number);

	/* The addresses of the host. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	status = getaddrinfo(host, port, &hints, &found);
	if (status != 0)
		return EHOSTUNREACH;

	/* The first that accepts a connection. */
	error = ECONNREFUSED;
	*descriptor = -1;
	for (address = found; address != NULL; address = address->ai_next) {
		*descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (*descriptor < 0) {
			error = errno;
			continue;
		}

		/* A connection that works ends the search. */
		status = connect(*descriptor, address->ai_addr, address->ai_addrlen);
		if (status == 0) {
			error = 0;
			break;
		}

		/* Otherwise the next address. */
		error = errno;
		close(*descriptor);
		*descriptor = -1;
	}

	/* The addresses are no longer needed. */
	freeaddrinfo(found);
	if (error != 0)
		return error;

	/* Succeeded: the connection is open. */
	return 0;
}

/* Sends all of a buffer, over TLS or the socket (a closed connection does not raise SIGPIPE). */
static int
http_send_all(
	const struct http_connection *connection,
	const unsigned char *bytes,
	size_t length)
{
	ssize_t sent;
	size_t done;
	int error;

	/* TLS sends it all itself. */
	if (connection->tls != NULL) {
		error = net_tls_write(connection->tls, bytes, length);
		if (error != 0)
			return error;

		/* Succeeded: the request is sent over TLS. */
		return 0;
	}

	/* Until everything is sent. */
	for (done = 0; done < length; done += (size_t)sent) {
		sent = send(connection->descriptor, bytes + done, length - done, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR) {
			sent = 0;
			continue;
		}

		/* Any other failure ends the request. */
		if (sent < 0)
			return errno;
	}

	/* Succeeded: the request is sent. */
	return 0;
}

/* Reads until the server closes the connection (or the response grows too large, or the server falls silent). */
static int
http_receive_all(
	const struct http_connection *connection,
	struct wb_buffer *raw)
{
	struct pollfd waiting;
	unsigned char chunk[16384];
	size_t received;
	int pending;
	int ready;
	int error;

	/* Each read, after waiting for the connection to be readable (unless TLS already holds bytes). */
	for (;;) {
		pending = 0;
		if (connection->tls != NULL)
			pending = net_tls_pending(connection->tls);
		if (!pending) {
			waiting.fd = connection->descriptor;
			waiting.events = POLLIN;
			waiting.revents = 0;
			ready = poll(&waiting, 1, NET_HTTP_TIMEOUT);
			if (ready < 0 && errno == EINTR)
				continue;
			if (ready < 0)
				return errno;
			if (ready == 0)
				return ETIMEDOUT;
		}

		/* The bytes, or the end of the response. */
		error = http_receive(connection, chunk, sizeof(chunk), &received);
		if (error == EINTR)
			continue;
		if (error != 0)
			return error;
		if (received == 0)
			break;
		if (raw->length + received > NET_HTTP_MAX_RESPONSE)
			return EFBIG;
		error = wb_buffer_append(raw, chunk, received);
		if (error != 0)
			return error;
	}

	/* Succeeded: the whole response is read. */
	return 0;
}

/* Reads what is there, over TLS or the socket; *received is 0 at the end of the connection. */
static int
http_receive(
	const struct http_connection *connection,
	unsigned char *bytes,
	size_t length,
	size_t *received)
{
	ssize_t count;
	int error;

	/* TLS decrypts. */
	if (connection->tls != NULL) {
		error = net_tls_read(connection->tls, bytes, length, received);
		if (error != 0)
			return error;

		/* Succeeded: *received bytes (0 at the end). */
		return 0;
	}

	/* The socket's bytes. */
	*received = 0;
	count = recv(connection->descriptor, bytes, length, 0);
	if (count < 0)
		return errno;

	/* Succeeded: *received bytes (0 at the end). */
	*received = (size_t)count;
	return 0;
}

/* Tells whether a scheme is one the fetch speaks: http or https. */
static int
http_is_web(
	const char *scheme)
{
	int differs;

	/* The two schemes. */
	differs = strcmp(scheme, "http");
	if (differs == 0)
		return 1;
	differs = strcmp(scheme, "https");
	if (differs == 0)
		return 1;

	/* Any other scheme. */
	return 0;
}

/*
 * Parses a whole response: the status line, the headers (cookies kept, a
 * redirect's Location into location), and the body (cut by its length or
 * decoded from chunks).
 */
int
net_http_parse_response(
	const struct net_url *url,
	const struct wb_buffer *raw,
	struct net_response *response,
	struct wb_buffer *location)
{
	struct http_headers headers;
	const char *text;
	const char *line;
	const char *end;
	const char *newline;
	const unsigned char *body;
	size_t line_length;
	size_t body_length;
	int differs;
	int error;

	/* The status line: HTTP/1.x, a space, the three digits. */
	wb_buffer_clear(&response->headers);
	text = (const char *)raw->data;
	if (raw->length < 12U)
		return EINVAL;
	differs = strncmp(text, "HTTP/1.", 7);
	if (differs != 0 || text[8] != ' ')
		return EINVAL;
	response->status = (text[9] - '0') * 100 + (text[10] - '0') * 10 + (text[11] - '0');

	/* The headers, a line each, up to the empty line. */
	end = text + raw->length;
	newline = memchr(text, '\n', raw->length);
	if (newline == NULL)
		return EINVAL;
	line = newline + 1;
	memset(&headers, 0, sizeof(headers));
	error = 0;
	while (error == 0 && line < end) {
		/* The line without its CR LF; the empty one ends the headers. */
		newline = memchr(line, '\n', (size_t)(end - line));
		if (newline == NULL)
			newline = end;
		line_length = (size_t)(newline - line);
		if (line_length > 0 && line[line_length - 1U] == '\r')
			line_length--;
		if (line_length == 0) {
			line = newline + 1;
			break;
		}

		/* The header, then the next line. */
		error = http_header(url, line, line_length, response, location, &headers);
		line = newline + 1;
	}

	/* A buffer that could not grow, or a cookie that could not be kept. */
	if (error != 0)
		return error;

	/* The body: chunks, a length, or the rest of the connection. */
	body = (const unsigned char *)line;
	body_length = 0;
	if (line < end)
		body_length = (size_t)(end - line);
	if (headers.chunked) {
		error = http_dechunk(body, body_length, &response->body);
	} else {
		if (headers.has_length && headers.content_length < body_length)
			body_length = headers.content_length;
		error = wb_buffer_append(&response->body, body, body_length);
	}

	/* A body in a content coding is decoded (gzip, or zlib's deflate). */
	if (error == 0 && headers.encoding != HTTP_ENCODING_NONE)
		error = http_decode_body(headers.encoding, response);

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the response is parsed. */
	return 0;
}

/* Reads one header line: Content-Type, Content-Length, Transfer-Encoding, Location and Set-Cookie. */
static int
http_header(
	const struct net_url *url,
	const char *line,
	size_t length,
	struct net_response *response,
	struct wb_buffer *location,
	struct http_headers *headers)
{
	const char *value;
	size_t value_length;
	int found;
	int differs;
	int error;

	/* Every header but the cookies is kept for a script, one a line. */
	error = 0;
	found = http_header_value(line, length, "set-cookie", &value, &value_length);
	if (!found) {
		error = wb_buffer_append(&response->headers, line, length);
		if (error == 0)
			error = wb_buffer_append_byte(&response->headers, '\n');
	}

	/* The type of the body. */
	found = http_header_value(line, length, "content-type", &value, &value_length);
	if (found) {
		wb_buffer_clear(&response->content_type);
		error = wb_buffer_append(&response->content_type, value, value_length);
	}

	/* The length of the body. */
	found = http_header_value(line, length, "content-length", &value, &value_length);
	if (found) {
		headers->has_length = 1;
		headers->content_length = (size_t)strtoul(value, NULL, 10);
	}

	/* Chunks (the last coding named is chunked). */
	found = http_header_value(line, length, "transfer-encoding", &value, &value_length);
	if (found && value_length >= 7U) {
		differs = strncmp(value + value_length - 7U, "chunked", 7);
		if (differs == 0)
			headers->chunked = 1;
	}

	/* The content coding of the body. */
	found = http_header_value(line, length, "content-encoding", &value, &value_length);
	if (found && value_length == 4U && strncasecmp(value, "gzip", 4) == 0)
		headers->encoding = HTTP_ENCODING_GZIP;
	if (found && value_length == 6U && strncasecmp(value, "x-gzip", 6) == 0)
		headers->encoding = HTTP_ENCODING_GZIP;
	if (found && value_length == 7U && strncasecmp(value, "deflate", 7) == 0)
		headers->encoding = HTTP_ENCODING_DEFLATE;

	/* Where a redirect goes. */
	found = http_header_value(line, length, "location", &value, &value_length);
	if (found && error == 0) {
		wb_buffer_clear(location);
		error = wb_buffer_append(location, value, value_length);
	}

	/* A cookie to keep. */
	found = http_header_value(line, length, "set-cookie", &value, &value_length);
	if (found && error == 0)
		error = net_cookie_store(url, value, value_length);

	/* What caching the response may do. */
	found = http_header_value(line, length, "cache-control", &value, &value_length);
	if (found)
		http_cache_control(value, value_length, response);

	/* The version the response is, for revalidating it. */
	found = http_header_value(line, length, "etag", &value, &value_length);
	if (found && error == 0) {
		wb_buffer_clear(&response->etag);
		error = wb_buffer_append(&response->etag, value, value_length);
	}

	/* A header that could not be kept fails. */
	if (error != 0)
		return error;

	/* Succeeded: the header is read. */
	return 0;
}

/* Finds a header's value on a line when the line is that header (the name in any case); nonzero when found. */
static int
http_header_value(
	const char *line,
	size_t length,
	const char *name,
	const char **value,
	size_t *value_length)
{
	size_t name_length;
	size_t index;
	size_t start;
	size_t end;
	int c;

	/* The name and a colon. */
	name_length = strlen(name);
	if (length <= name_length || line[name_length] != ':')
		return 0;
	for (index = 0; index < name_length; index++) {
		c = (unsigned char)line[index];
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		if (c != (unsigned char)name[index])
			return 0;
	}

	/* The value without the whitespace around it. */
	start = name_length + 1U;
	end = length;
	while (start < end && (line[start] == ' ' || line[start] == '\t'))
		start++;
	while (end > start && (line[end - 1U] == ' ' || line[end - 1U] == '\t'))
		end--;

	/* Succeeded: the value is found. */
	*value = line + start;
	*value_length = end - start;
	return 1;
}

/* Decodes a chunked body: each hexadecimal size line and that many bytes, up to the zero chunk. */
static int
http_dechunk(
	const unsigned char *body,
	size_t length,
	struct wb_buffer *out)
{
	size_t position;
	size_t size;
	int digit;
	int c;
	int error;

	/* Each chunk. */
	position = 0;
	while (position < length) {
		/* The size in hexadecimal (an extension after ; is ignored), then the line's end. */
		size = 0;
		while (position < length) {
			c = body[position];
			digit = -1;
			if (c >= '0' && c <= '9')
				digit = c - '0';
			if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
				digit = (c | 0x20) - 'a' + 10;
			if (digit < 0)
				break;
			size = size * 16U + (size_t)digit;
			position++;
		}
		while (position < length && body[position] != '\n')
			position++;
		position++;

		/* The last chunk ends the body (trailers are ignored). */
		if (size == 0)
			break;

		/* The chunk's bytes (a short chunk at the end gives what there is). */
		if (position > length)
			break;
		if (size > length - position)
			size = length - position;
		error = wb_buffer_append(out, body + position, size);
		if (error != 0)
			return error;
		position += size;

		/* The CR LF after it. */
		while (position < length && body[position] != '\n')
			position++;
		position++;
	}

	/* Succeeded: the body is decoded. */
	return 0;
}

/* Inflates a body's bytes (a zlib stream, or raw deflate after a gzip header) into a buffer; EINVAL when they are no stream. */
static int
http_inflate(
	const unsigned char *bytes,
	size_t length,
	int window,
	struct wb_buffer *out)
{
	z_stream stream;
	unsigned char chunk[16384];
	int result;
	int error;

	memset(&stream, 0, sizeof(stream));
	if (inflateInit2(&stream, window) != Z_OK)
		return EINVAL;
	stream.next_in = (Bytef *)(uintptr_t)bytes;
	stream.avail_in = (uInt)length;
	error = 0;
	for (;;) {
		stream.next_out = chunk;
		stream.avail_out = sizeof(chunk);
		result = inflate(&stream, Z_FINISH);
		if (stream.avail_out != sizeof(chunk)) {
			error = wb_buffer_append(out, chunk, sizeof(chunk) - stream.avail_out);
			if (error != 0)
				break;
		}
		if (result == Z_STREAM_END)
			break;
		if (result != Z_OK && result != Z_BUF_ERROR) {
			error = EINVAL;
			break;
		}
		if (result == Z_BUF_ERROR && stream.avail_out != 0) {
			/* Truncated input: what was inflated is kept. */
			break;
		}
	}
	inflateEnd(&stream);
	return error;
}

/* Decodes a response's body in place from its content coding. */
static int
http_decode_body(
	int encoding,
	struct net_response *response)
{
	struct wb_buffer decoded;
	const unsigned char *bytes;
	size_t length;
	size_t position;
	unsigned flags;
	int error;

	/* A gzip member: the ten byte header, the optional fields, then raw deflate and an eight byte trailer. */
	bytes = response->body.data;
	length = response->body.length;
	if (length == 0)
		return 0;
	wb_buffer_init(&decoded);
	error = 0;
	if (encoding == HTTP_ENCODING_GZIP) {
		if (length < 18U || bytes[0] != 0x1fU || bytes[1] != 0x8bU || bytes[2] != 8U) {
			wb_buffer_release(&decoded);
			return 0;
		}
		flags = bytes[3];
		position = 10;
		if ((flags & 4U) != 0U && position + 2U <= length)
			position += 2U + (size_t)bytes[position] + ((size_t)bytes[position + 1U] << 8);
		if ((flags & 8U) != 0U) {
			while (position < length && bytes[position] != 0)
				position++;
			position++;
		}
		if ((flags & 16U) != 0U) {
			while (position < length && bytes[position] != 0)
				position++;
			position++;
		}
		if ((flags & 2U) != 0U)
			position += 2U;
		if (position >= length) {
			wb_buffer_release(&decoded);
			return 0;
		}
		error = http_inflate(bytes + position, length - position, -MAX_WBITS, &decoded);
	} else {
		error = http_inflate(bytes, length, MAX_WBITS, &decoded);
		if (error == EINVAL) {
			/* Some servers send raw deflate under this name. */
			wb_buffer_clear(&decoded);
			error = http_inflate(bytes, length, -MAX_WBITS, &decoded);
		}
	}

	/* A body that is no stream stays as it came. */
	if (error == EINVAL) {
		wb_buffer_release(&decoded);
		return 0;
	}
	if (error != 0) {
		wb_buffer_release(&decoded);
		return error;
	}
	wb_buffer_release(&response->body);
	response->body = decoded;
	return 0;
}

/* Tells whether a status is a redirect a fetch follows. */
int
net_http_is_redirect(
	int status)
{
	/* 301, 302, 303, 307 and 308. */
	if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
		return 1;
	return 0;
}

/*
 * Reads a Cache-Control header's directives: max-age (seconds), no-store
 * and no-cache (the others are left).
 */
static void
http_cache_control(
	const char *value,
	size_t length,
	struct net_response *response)
{
	size_t start;
	size_t end;
	size_t word;
	int differs;

	/* Each directive between commas. */
	start = 0;
	while (start < length) {
		while (start < length && (value[start] == ' ' || value[start] == '\t' || value[start] == ','))
			start++;
		end = start;
		while (end < length && value[end] != ',')
			end++;
		word = end - start;

		/* max-age=N. */
		differs = 1;
		if (word > 8U)
			differs = strncasecmp(value + start, "max-age=", 8U);
		if (differs == 0)
			response->max_age = strtol(value + start + 8U, NULL, 10);

		/* no-store. */
		differs = 1;
		if (word >= 8U)
			differs = strncasecmp(value + start, "no-store", 8U);
		if (differs == 0)
			response->no_store = 1;

		/* no-cache. */
		differs = 1;
		if (word >= 8U)
			differs = strncasecmp(value + start, "no-cache", 8U);
		if (differs == 0)
			response->no_cache = 1;

		/* The next directive. */
		start = end;
	}
}

/*
 * Finds a header's value among the header lines of a response's first
 * end bytes (after its status line); nonzero when found.
 */
static int
http_find_header(
	const unsigned char *raw,
	size_t end,
	const char *name,
	const char **value,
	size_t *value_length)
{
	const char *line;
	const char *limit;
	const char *newline;
	size_t line_length;
	int found;

	/* Each line after the status line. */
	limit = (const char *)raw + end;
	newline = memchr(raw, '\n', end);
	if (newline == NULL)
		return 0;
	for (line = newline + 1; line < limit; line = newline + 1) {
		newline = memchr(line, '\n', (size_t)(limit - line));
		if (newline == NULL)
			newline = limit;
		line_length = (size_t)(newline - line);
		if (line_length > 0 && line[line_length - 1U] == '\r')
			line_length--;

		/* The header, when this line is it. */
		found = http_header_value(line, line_length, name, value, value_length);
		if (found)
			return 1;
	}

	/* The header is not there. */
	return 0;
}

/* Finds the first place a run of bytes appears in others (NULL when it does not). */
static const void *
http_memmem(
	const void *haystack,
	size_t length,
	const void *needle,
	size_t needle_length)
{
	const unsigned char *bytes;
	size_t index;
	int differs;

	/* Each place the run could start. */
	bytes = haystack;
	if (needle_length == 0 || length < needle_length)
		return NULL;
	for (index = 0; index + needle_length <= length; index++) {
		differs = memcmp(bytes + index, needle, needle_length);
		if (differs == 0)
			return bytes + index;
	}

	/* Not there. */
	return NULL;
}
