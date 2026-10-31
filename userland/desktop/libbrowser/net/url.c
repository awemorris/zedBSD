/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * URLs: the WHATWG URL Standard's basic URL parser (without a state
 * override, which the URL setters need later), its serializer, the parts
 * the URL interface reports, and the conversions between file: URLs and
 * paths.
 *
 * The parser runs over the input's UTF-8 bytes: every state tests ASCII
 * characters, and a byte of a non-ASCII character is percent-encoded in
 * every set the parser uses, as its code point's UTF-8 is.  Hosts go to
 * host.c, which decodes them.
 */

#include "net/net.h"
#include "base/os-file.h"
#include "base/os-system.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The end of the input, as the parser's current character. */
#define URL_EOF			(-1)

/* The percent-encode sets (the WHATWG URL Standard's). */
#define URL_SET_C0		0
#define URL_SET_FRAGMENT	1
#define URL_SET_QUERY		2
#define URL_SET_SPECIAL_QUERY	3
#define URL_SET_PATH		4
#define URL_SET_USERINFO	5

/*
 * The states of the basic URL parser.
 */
enum url_state {
	URL_SCHEME_START,
	URL_SCHEME,
	URL_NO_SCHEME,
	URL_SPECIAL_RELATIVE_OR_AUTHORITY,
	URL_PATH_OR_AUTHORITY,
	URL_RELATIVE,
	URL_RELATIVE_SLASH,
	URL_SPECIAL_AUTHORITY_SLASHES,
	URL_SPECIAL_AUTHORITY_IGNORE_SLASHES,
	URL_AUTHORITY,
	URL_HOST,
	URL_PORT,
	URL_FILE,
	URL_FILE_SLASH,
	URL_FILE_HOST,
	URL_PATH_START,
	URL_PATH,
	URL_OPAQUE_PATH,
	URL_QUERY,
	URL_FRAGMENT
};

/*
 * A URL while it is parsed: each part growing in a buffer, the path as a
 * list of segments (malloc'd strings) unless it is opaque, and whether
 * the host, the query and the fragment are there at all.
 */
struct url_parts {
	struct wb_buffer scheme;
	struct wb_buffer username;
	struct wb_buffer password;
	struct wb_buffer host;
	int has_host;
	int port;
	struct wb_vector segments;
	struct wb_buffer opaque;
	int opaque_path;
	struct wb_buffer query;
	int has_query;
	struct wb_buffer fragment;
	int has_fragment;
};

/*
 * The parser: the cleaned input, where it stands, the state, the buffer
 * of the state that gathers, its flags, the base, and the URL being made.
 */
struct url_parser {
	const char *input;
	size_t length;
	long pointer;
	int state;
	struct wb_buffer buffer;
	int at_sign_seen;
	int inside_brackets;
	int password_token_seen;
	const struct net_url *base;
	struct url_parts url;
};

/*
 * A special scheme and its default port (-1 for none).
 */
struct url_special {
	const char *scheme;
	int port;
};

/*
 * The special schemes.  The table is constant for the life of the program
 * and ends with NULL.
 */
static const struct url_special url_specials[] = {
	{ "ftp", 21 },
	{ "file", -1 },
	{ "http", 80 },
	{ "https", 443 },
	{ "ws", 80 },
	{ "wss", 443 },
	{ NULL, 0 }
};

/* The User-Agent, made once from the system's part. */
static pthread_once_t url_agent_once = PTHREAD_ONCE_INIT;
static char url_agent[256];

static void url_parts_init(struct url_parts *parts);
static void url_agent_make(void);
static void url_parts_release(struct url_parts *parts);
static int url_clean(const char *input, size_t length, struct wb_buffer *cleaned);
static int url_step(struct url_parser *parser, int c);
static int url_scheme_start(struct url_parser *parser, int c);
static int url_scheme(struct url_parser *parser, int c);
static int url_scheme_done(struct url_parser *parser);
static int url_no_scheme(struct url_parser *parser, int c);
static int url_relative(struct url_parser *parser, int c);
static int url_relative_slash(struct url_parser *parser, int c);
static int url_authority(struct url_parser *parser, int c);
static int url_credentials(struct url_parser *parser);
static int url_host(struct url_parser *parser, int c);
static int url_port(struct url_parser *parser, int c);
static int url_file(struct url_parser *parser, int c);
static int url_file_slash(struct url_parser *parser, int c);
static int url_file_host(struct url_parser *parser, int c);
static int url_path_start(struct url_parser *parser, int c);
static int url_path(struct url_parser *parser, int c);
static int url_opaque_path(struct url_parser *parser, int c);
static int url_query(struct url_parser *parser, int c);
static void url_begin_query(struct url_parser *parser);
static void url_begin_fragment(struct url_parser *parser);
static int url_ends_segment(const struct url_parser *parser, int c);
static int url_char(const struct url_parser *parser, long at);
static int url_special_scheme(const char *scheme);
static int url_is_special(const struct url_parser *parser);
static int url_scheme_is(const struct url_parser *parser, const char *scheme);
static int url_in_set(int c, int set);
static int url_encode(struct wb_buffer *out, int c, int set);
static int url_copy_base(struct url_parser *parser, int path, int query);
static int url_push_segment(struct url_parts *parts, const char *text, size_t length);
static void url_clear_path(struct url_parts *parts);
static void url_shorten_path(struct url_parts *parts);
static int url_is_drive_letter(const char *text, size_t length, int normalized);
static int url_starts_with_drive_letter(const struct url_parser *parser, long at);
static int url_dot_segment(const struct wb_buffer *buffer, int dots);
static int url_parse_host(struct url_parser *parser);
static int url_finish(struct url_parts *parts, struct net_url *url);
static char *url_strdup(const struct wb_buffer *buffer);
static int url_split_path(const struct net_url *url, struct url_parts *parts);
static int url_origin(const struct net_url *url, struct wb_buffer *out);
static int url_hex(char digit);
static int url_named(const char *text, const char *name);

/*
 * Parses a URL (its UTF-8 text) against an optional base URL.
 *
 * Returns 0 with the URL filled (free it with net_url_release), EINVAL
 * when the text is not a URL (the Standard's failure), or ENOMEM.
 */
int
net_url_parse(
	const char *input,
	size_t length,
	const struct net_url *base,
	struct net_url *url)
{
	struct url_parser parser;
	struct wb_buffer cleaned;
	int c;
	int error;

	/* The input without the whitespace around it and the tabs and newlines in it. */
	memset(url, 0, sizeof(*url));
	wb_buffer_init(&cleaned);
	error = url_clean(input, length, &cleaned);
	if (error != 0) {
		wb_buffer_release(&cleaned);
		return error;
	}

	/* The parser at the start, in the scheme start state. */
	memset(&parser, 0, sizeof(parser));
	parser.input = wb_buffer_string(&cleaned);
	parser.length = cleaned.length;
	parser.state = URL_SCHEME_START;
	parser.base = base;
	wb_buffer_init(&parser.buffer);
	url_parts_init(&parser.url);

	/* Runs the states over every character and the end, until the URL is done or fails. */
	for (parser.pointer = 0; error == 0; parser.pointer++) {
		c = url_char(&parser, parser.pointer);
		error = url_step(&parser, c);

		/* The end was read and the state did not step back to read it again. */
		if (parser.pointer >= (long)parser.length)
			break;
	}

	/* The URL's strings, unless it failed. */
	if (error == 0)
		error = url_finish(&parser.url, url);
	url_parts_release(&parser.url);
	wb_buffer_release(&parser.buffer);
	wb_buffer_release(&cleaned);
	if (error != 0) {
		net_url_release(url);
		return error;
	}

	/* Succeeded: the URL is parsed. */
	return 0;
}

/*
 * Frees a URL's strings.
 */
void
net_url_release(
	struct net_url *url)
{
	/* Each part. */
	free(url->scheme);
	free(url->username);
	free(url->password);
	free(url->host);
	free(url->path);
	free(url->query);
	free(url->fragment);
	memset(url, 0, sizeof(*url));
	url->port = -1;
}

/*
 * Tells whether a URL's scheme is special (ftp, file, http, https, ws,
 * wss).
 */
int
net_url_is_special(
	const struct net_url *url)
{
	int special;

	/* By its scheme. */
	special = url_special_scheme(url->scheme);

	/* Reports it. */
	return special;
}

/*
 * Reports a scheme's default port, or -1 when it has none.
 */
int
net_url_default_port(
	const char *scheme)
{
	size_t index;
	int differs;

	/* A special scheme's port. */
	for (index = 0; url_specials[index].scheme != NULL; index++) {
		differs = strcmp(scheme, url_specials[index].scheme);
		if (differs == 0)
			return url_specials[index].port;
	}

	/* Other schemes have none. */
	return -1;
}

/*
 * Writes a URL's serialization (its href), without the fragment when
 * asked.
 */
int
net_url_serialize(
	const struct net_url *url,
	int exclude_fragment,
	struct wb_buffer *out)
{
	int credentials;
	int error;

	/* The scheme. */
	error = wb_buffer_printf(out, "%s:", url->scheme);

	/* The authority, when there is a host: // and the credentials. */
	credentials = 0;
	if (url->username[0] != '\0' || url->password[0] != '\0')
		credentials = 1;
	if (error == 0 && url->host != NULL)
		error = wb_buffer_append_string(out, "//");
	if (error == 0 && url->host != NULL && credentials)
		error = wb_buffer_append_string(out, url->username);
	if (error == 0 && url->host != NULL && url->password[0] != '\0')
		error = wb_buffer_printf(out, ":%s", url->password);
	if (error == 0 && url->host != NULL && credentials)
		error = wb_buffer_append_byte(out, '@');

	/* The host and the port. */
	if (error == 0 && url->host != NULL)
		error = wb_buffer_append_string(out, url->host);
	if (error == 0 && url->host != NULL && url->port >= 0)
		error = wb_buffer_printf(out, ":%d", url->port);

	/* A path starting with an empty segment and no host would read as a host: "/." keeps it a path. */
	if (error == 0 && url->host == NULL && !url->opaque_path && url->path[0] == '/' && url->path[1] == '/')
		error = wb_buffer_append_string(out, "/.");

	/* The path, the query and the fragment. */
	if (error == 0)
		error = wb_buffer_append_string(out, url->path);
	if (error == 0 && url->query != NULL)
		error = wb_buffer_printf(out, "?%s", url->query);
	if (error == 0 && url->fragment != NULL && !exclude_fragment)
		error = wb_buffer_printf(out, "#%s", url->fragment);
	if (error != 0)
		return error;

	/* Succeeded: the URL is serialized. */
	return 0;
}

/*
 * Writes one part of a URL as the URL interface reports it.
 */
int
net_url_component(
	const struct net_url *url,
	int part,
	struct wb_buffer *out)
{
	int error;

	/* Each part by its rules. */
	error = 0;
	switch (part) {
	case NET_URL_HREF:
		error = net_url_serialize(url, 0, out);
		break;
	case NET_URL_ORIGIN:
		error = url_origin(url, out);
		break;
	case NET_URL_PROTOCOL:
		error = wb_buffer_printf(out, "%s:", url->scheme);
		break;
	case NET_URL_USERNAME:
		error = wb_buffer_append_string(out, url->username);
		break;
	case NET_URL_PASSWORD:
		error = wb_buffer_append_string(out, url->password);
		break;
	case NET_URL_HOST:
		if (url->host != NULL)
			error = wb_buffer_append_string(out, url->host);
		if (error == 0 && url->host != NULL && url->port >= 0)
			error = wb_buffer_printf(out, ":%d", url->port);
		break;
	case NET_URL_HOSTNAME:
		if (url->host != NULL)
			error = wb_buffer_append_string(out, url->host);
		break;
	case NET_URL_PORT:
		if (url->port >= 0)
			error = wb_buffer_printf(out, "%d", url->port);
		break;
	case NET_URL_PATHNAME:
		error = wb_buffer_append_string(out, url->path);
		break;
	case NET_URL_SEARCH:
		if (url->query != NULL && url->query[0] != '\0')
			error = wb_buffer_printf(out, "?%s", url->query);
		break;
	case NET_URL_HASH:
		if (url->fragment != NULL && url->fragment[0] != '\0')
			error = wb_buffer_printf(out, "#%s", url->fragment);
		break;
	default:
		error = EINVAL;
		break;
	}

	/* A part not known, or a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the part is written. */
	return 0;
}

/*
 * Writes the local path a file: URL names (its path percent-decoded).
 * Returns EPROTONOSUPPORT for a URL of another scheme, and ENOENT for a
 * file URL on another host.
 */
int
net_url_file_path(
	const struct net_url *url,
	struct wb_buffer *out)
{
	struct wb_buffer decoded;
	int differs;
	int error;

	/* Only file: URLs name local files. */
	differs = strcmp(url->scheme, "file");
	if (differs != 0)
		return EPROTONOSUPPORT;

	/* The path's bytes, decoded. */
	wb_buffer_init(&decoded);
	error = net_percent_decode(url->path, strlen(url->path), &decoded);
	if (error != 0) {
		wb_buffer_release(&decoded);
		return error;
	}

	/* The local path the system makes of the host and the path (ENOENT for another machine). */
	error = wb_os_path_from_url(url->host, wb_buffer_string(&decoded), out);
	wb_buffer_release(&decoded);
	if (error != 0)
		return error;

	/* Succeeded: the path is written. */
	return 0;
}

/*
 * Writes the file: URL of an absolute local path (the host the system
 * splits off, then the path's bytes percent-encoded as a path, with % ?
 * and # escaped too).  Returns EINVAL for a path that is not absolute.
 */
int
net_url_from_file_path(
	const char *path,
	struct wb_buffer *out)
{
	struct wb_buffer host;
	struct wb_buffer local;
	const char *bytes;
	size_t index;
	int c;
	int error;

	/* The URL's host and path, as the system splits the local path. */
	wb_buffer_init(&host);
	wb_buffer_init(&local);
	error = wb_os_url_from_path(path, &host, &local);
	if (error != 0) {
		wb_buffer_release(&host);
		wb_buffer_release(&local);
		return error;
	}

	/* The scheme and the host, then each byte of the path. */
	error = wb_buffer_append_string(out, "file://");
	if (error == 0)
		error = wb_buffer_append(out, host.data, host.length);
	bytes = wb_buffer_string(&local);
	for (index = 0; error == 0 && index < local.length; index++) {
		c = (unsigned char)bytes[index];
		if (c == '%' || c == '#' || c == '?') {
			error = wb_buffer_printf(out, "%%%02X", (unsigned)c);
		} else {
			error = url_encode(out, c, URL_SET_PATH);
		}
	}
	wb_buffer_release(&host);
	wb_buffer_release(&local);

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the URL is written. */
	return 0;
}

/*
 * Reports the User-Agent the browser sends, which navigator.userAgent
 * reports too.  The string is static; any thread may call this.
 */
const char *
net_user_agent(void)
{
	/* Made on the first call. */
	pthread_once(&url_agent_once, url_agent_make);

	/* Succeeded: the string lives as long as the program. */
	return url_agent;
}

/*
 * Decodes the %XX escapes of a text (an escape that is not one stays as
 * it is).
 */
int
net_percent_decode(
	const char *text,
	size_t length,
	struct wb_buffer *out)
{
	size_t index;
	int high;
	int low;
	int error;

	/* Each byte, or the byte an escape names. */
	error = 0;
	for (index = 0; error == 0 && index < length; index++) {
		/* An escape of two hexadecimal digits is one byte. */
		high = -1;
		low = -1;
		if (text[index] == '%' && index + 2U < length) {
			high = url_hex(text[index + 1U]);
			low = url_hex(text[index + 2U]);
		}

		/* The byte the escape names. */
		if (high >= 0 && low >= 0) {
			error = wb_buffer_append_byte(out, (unsigned char)(high * 16 + low));
			index += 2U;
			continue;
		}

		/* Any other byte stays. */
		error = wb_buffer_append_byte(out, (unsigned char)text[index]);
	}

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the text is decoded. */
	return 0;
}

/* Starts the parts of a URL empty: no host, no query, no fragment, no port. */
static void
url_parts_init(
	struct url_parts *parts)
{
	/* Every buffer empty and every part absent. */
	memset(parts, 0, sizeof(*parts));
	wb_buffer_init(&parts->scheme);
	wb_buffer_init(&parts->username);
	wb_buffer_init(&parts->password);
	wb_buffer_init(&parts->host);
	wb_buffer_init(&parts->opaque);
	wb_buffer_init(&parts->query);
	wb_buffer_init(&parts->fragment);
	wb_vector_init(&parts->segments, sizeof(char *));
	parts->port = -1;
}

/* Frees the parts of a URL. */
static void
url_parts_release(
	struct url_parts *parts)
{
	/* The segments, then the buffers. */
	url_clear_path(parts);
	wb_vector_release(&parts->segments);
	wb_buffer_release(&parts->scheme);
	wb_buffer_release(&parts->username);
	wb_buffer_release(&parts->password);
	wb_buffer_release(&parts->host);
	wb_buffer_release(&parts->opaque);
	wb_buffer_release(&parts->query);
	wb_buffer_release(&parts->fragment);
}

/* Copies the input without leading and trailing C0 controls and spaces, and without any tab or newline. */
static int
url_clean(
	const char *input,
	size_t length,
	struct wb_buffer *cleaned)
{
	size_t start;
	size_t end;
	size_t index;
	int c;
	int error;

	/* The ends past the controls and spaces. */
	start = 0;
	end = length;
	while (start < end && (unsigned char)input[start] <= 0x20U)
		start++;
	while (end > start && (unsigned char)input[end - 1U] <= 0x20U)
		end--;

	/* The bytes between, without tabs and newlines. */
	error = 0;
	for (index = start; error == 0 && index < end; index++) {
		c = (unsigned char)input[index];
		if (c == 0x09 || c == 0x0a || c == 0x0d)
			continue;
		error = wb_buffer_append_byte(cleaned, (unsigned char)c);
	}

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the input is clean. */
	return 0;
}

/* Runs the current state on one character (URL_EOF at the end); 0, EINVAL for failure, or ENOMEM. */
static int
url_step(
	struct url_parser *parser,
	int c)
{
	int next;
	int error;

	/* Each state by the Standard; the short ones are here, the others in their functions. */
	error = 0;
	next = url_char(parser, parser->pointer + 1);
	switch (parser->state) {
	case URL_SCHEME_START:
		error = url_scheme_start(parser, c);
		break;
	case URL_SCHEME:
		error = url_scheme(parser, c);
		break;
	case URL_NO_SCHEME:
		error = url_no_scheme(parser, c);
		break;
	case URL_SPECIAL_RELATIVE_OR_AUTHORITY:
		/* Two slashes start an authority; anything else is relative to the base. */
		parser->state = URL_RELATIVE;
		parser->pointer--;
		if (c == '/' && next == '/')
			parser->state = URL_SPECIAL_AUTHORITY_IGNORE_SLASHES;
		if (c == '/' && next == '/')
			parser->pointer += 2;
		break;
	case URL_PATH_OR_AUTHORITY:
		/* A second slash starts an authority; anything else is the path. */
		parser->state = URL_AUTHORITY;
		if (c != '/')
			parser->state = URL_PATH;
		if (c != '/')
			parser->pointer--;
		break;
	case URL_RELATIVE:
		error = url_relative(parser, c);
		break;
	case URL_RELATIVE_SLASH:
		error = url_relative_slash(parser, c);
		break;
	case URL_SPECIAL_AUTHORITY_SLASHES:
		parser->state = URL_SPECIAL_AUTHORITY_IGNORE_SLASHES;
		parser->pointer--;
		if (c == '/' && next == '/')
			parser->pointer += 2;
		break;
	case URL_SPECIAL_AUTHORITY_IGNORE_SLASHES:
		/* Slashes are skipped; the authority starts at anything else. */
		if (c != '/' && c != '\\')
			parser->state = URL_AUTHORITY;
		if (c != '/' && c != '\\')
			parser->pointer--;
		break;
	case URL_AUTHORITY:
		error = url_authority(parser, c);
		break;
	case URL_HOST:
		error = url_host(parser, c);
		break;
	case URL_PORT:
		error = url_port(parser, c);
		break;
	case URL_FILE:
		error = url_file(parser, c);
		break;
	case URL_FILE_SLASH:
		error = url_file_slash(parser, c);
		break;
	case URL_FILE_HOST:
		error = url_file_host(parser, c);
		break;
	case URL_PATH_START:
		error = url_path_start(parser, c);
		break;
	case URL_PATH:
		error = url_path(parser, c);
		break;
	case URL_OPAQUE_PATH:
		error = url_opaque_path(parser, c);
		break;
	case URL_QUERY:
		error = url_query(parser, c);
		break;
	case URL_FRAGMENT:
		if (c != URL_EOF)
			error = url_encode(&parser->url.fragment, c, URL_SET_FRAGMENT);
		break;
	default:
		break;
	}

	/* A failure, or a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the character is consumed. */
	return 0;
}

/* The scheme start state: a letter starts a scheme; anything else means there is none. */
static int
url_scheme_start(
	struct url_parser *parser,
	int c)
{
	int error;

	/* Not a letter: the URL has no scheme. */
	if (c < 0 || (c | 0x20) < 'a' || (c | 0x20) > 'z') {
		parser->state = URL_NO_SCHEME;
		parser->pointer--;
		return 0;
	}

	/* The letter in lower case. */
	error = wb_buffer_append_byte(&parser->buffer, (unsigned char)(c | 0x20));
	parser->state = URL_SCHEME;
	if (error != 0)
		return error;

	/* Succeeded: the scheme has begun. */
	return 0;
}

/* The scheme state: letters, digits, + - and . gather until the colon. */
static int
url_scheme(
	struct url_parser *parser,
	int c)
{
	int error;

	/* A scheme character, letters in lower case. */
	if (c >= 'A' && c <= 'Z') {
		error = wb_buffer_append_byte(&parser->buffer, (unsigned char)(c + 0x20));
		return error;
	}

	/* The other scheme characters as they are. */
	if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
		error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
		return error;
	}

	/* Anything but the colon: there was no scheme, and the input is read again from its start. */
	if (c != ':') {
		wb_buffer_clear(&parser->buffer);
		parser->state = URL_NO_SCHEME;
		parser->pointer = -1;
		return 0;
	}

	/* The colon ends the scheme. */
	error = url_scheme_done(parser);
	if (error != 0)
		return error;

	/* Succeeded: the scheme is set. */
	return 0;
}

/* Sets the scheme gathered and picks the state its kind of URL goes on in. */
static int
url_scheme_done(
	struct url_parser *parser)
{
	const struct net_url *base;
	int special;
	int is_file;
	int same;
	int next;
	int error;

	/* The scheme. */
	error = wb_buffer_append(&parser->url.scheme, parser->buffer.data, parser->buffer.length);
	wb_buffer_clear(&parser->buffer);
	if (error != 0)
		return error;

	/* What follows depends on the scheme and the base. */
	base = parser->base;
	special = url_is_special(parser);
	is_file = url_scheme_is(parser, "file");
	same = 0;
	if (base != NULL)
		same = url_scheme_is(parser, base->scheme);
	next = url_char(parser, parser->pointer + 1);
	if (is_file) {
		parser->state = URL_FILE;
	} else if (special && same) {
		parser->state = URL_SPECIAL_RELATIVE_OR_AUTHORITY;
	} else if (special) {
		parser->state = URL_SPECIAL_AUTHORITY_SLASHES;
	} else if (next == '/') {
		parser->state = URL_PATH_OR_AUTHORITY;
		parser->pointer++;
	} else {
		parser->url.opaque_path = 1;
		parser->state = URL_OPAQUE_PATH;
	}

	/* Succeeded: the next state is chosen. */
	return 0;
}

/* The no scheme state: a URL without a scheme takes its base's, or fails without one. */
static int
url_no_scheme(
	struct url_parser *parser,
	int c)
{
	const struct net_url *base;
	struct url_parts *url;
	int differs;
	int error;

	/* No base, or a base with an opaque path that is not merely given a fragment, is a failure. */
	base = parser->base;
	url = &parser->url;
	if (base == NULL)
		return EINVAL;
	if (base->opaque_path && c != '#')
		return EINVAL;

	/* A fragment on a base with an opaque path. */
	if (base->opaque_path) {
		error = wb_buffer_append_string(&url->scheme, base->scheme);
		if (error == 0)
			error = wb_buffer_append_string(&url->opaque, base->path);
		url->opaque_path = 1;
		if (error == 0 && base->query != NULL) {
			url->has_query = 1;
			error = wb_buffer_append_string(&url->query, base->query);
		}

		/* The fragment the input gives. */
		url_begin_fragment(parser);
		return error;
	}

	/* Relative to the base, as a file URL or another. */
	differs = strcmp(base->scheme, "file");
	parser->state = URL_FILE;
	if (differs != 0)
		parser->state = URL_RELATIVE;
	parser->pointer--;

	/* Succeeded: the next state is chosen. */
	return 0;
}

/* The relative state: the base's scheme, then its authority, path and query unless the input replaces them. */
static int
url_relative(
	struct url_parser *parser,
	int c)
{
	struct url_parts *url;
	int special;
	int error;

	/* The base's scheme. */
	url = &parser->url;
	error = 0;
	if (url->scheme.length == 0)
		error = wb_buffer_append_string(&url->scheme, parser->base->scheme);
	if (error != 0)
		return error;

	/* A slash (a backslash too for special schemes) starts a new path or authority. */
	special = url_is_special(parser);
	if (c == '/' || (special && c == '\\')) {
		parser->state = URL_RELATIVE_SLASH;
		return 0;
	}

	/* Otherwise the base's authority, path and query. */
	error = url_copy_base(parser, 1, 1);
	if (error != 0)
		return error;

	/* A query or a fragment replaces the base's; anything else is a path relative to the base's. */
	if (c == '?') {
		url_begin_query(parser);
	} else if (c == '#') {
		url_begin_fragment(parser);
	} else if (c != URL_EOF) {
		wb_buffer_clear(&url->query);
		url->has_query = 0;
		url_shorten_path(url);
		parser->state = URL_PATH;
		parser->pointer--;
	}

	/* Succeeded: the next state is chosen. */
	return 0;
}

/* The relative slash state: a second slash starts an authority; otherwise the base's authority and a new path. */
static int
url_relative_slash(
	struct url_parser *parser,
	int c)
{
	int special;
	int error;

	/* Another slash: an authority (special schemes take backslashes as slashes). */
	special = url_is_special(parser);
	if (special && (c == '/' || c == '\\')) {
		parser->state = URL_SPECIAL_AUTHORITY_IGNORE_SLASHES;
		return 0;
	}

	/* A slash in a URL that is not special. */
	if (c == '/') {
		parser->state = URL_AUTHORITY;
		return 0;
	}

	/* Otherwise the base's authority and a path from the root. */
	error = url_copy_base(parser, 0, 0);
	parser->state = URL_PATH;
	parser->pointer--;
	if (error != 0)
		return error;

	/* Succeeded: the path follows. */
	return 0;
}

/* The authority state: the credentials up to the last @, then the host is read again from its start. */
static int
url_authority(
	struct url_parser *parser,
	int c)
{
	int special;
	int error;

	/* An @ ends credentials. */
	if (c == '@') {
		error = url_credentials(parser);
		return error;
	}

	/* The end of the authority: the host (and port) is read again from where the credentials ended. */
	special = url_is_special(parser);
	if (c == URL_EOF || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
		if (parser->at_sign_seen && parser->buffer.length == 0)
			return EINVAL;
		parser->pointer -= (long)parser->buffer.length + 1;
		wb_buffer_clear(&parser->buffer);
		parser->state = URL_HOST;
		return 0;
	}

	/* Anything else gathers. */
	error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
	if (error != 0)
		return error;

	/* Succeeded: the character is gathered. */
	return 0;
}

/* Turns the gathered text before an @ into the username and password (a second @ was part of them). */
static int
url_credentials(
	struct url_parser *parser)
{
	struct url_parts *url;
	struct wb_buffer *target;
	size_t index;
	int error;
	int c;

	/* An earlier @ was part of the credentials. */
	url = &parser->url;
	error = 0;
	target = &url->username;
	if (parser->password_token_seen)
		target = &url->password;
	if (parser->at_sign_seen)
		error = wb_buffer_append_string(target, "%40");
	parser->at_sign_seen = 1;

	/* Each character, encoded; the first colon starts the password. */
	for (index = 0; error == 0 && index < parser->buffer.length; index++) {
		c = parser->buffer.data[index];
		if (c == ':' && !parser->password_token_seen) {
			parser->password_token_seen = 1;
			target = &url->password;
			continue;
		}

		/* The character, encoded. */
		error = url_encode(target, c, URL_SET_USERINFO);
	}

	/* The gathered text is used. */
	wb_buffer_clear(&parser->buffer);
	if (error != 0)
		return error;

	/* Succeeded: the credentials are set. */
	return 0;
}

/* The host state: the host up to a colon outside brackets (a port follows) or the end of the authority. */
static int
url_host(
	struct url_parser *parser,
	int c)
{
	int special;
	int error;

	/* A colon outside brackets ends the host; the port follows. */
	special = url_is_special(parser);
	if (c == ':' && !parser->inside_brackets) {
		if (parser->buffer.length == 0)
			return EINVAL;
		error = url_parse_host(parser);
		parser->state = URL_PORT;
		return error;
	}

	/* The end of the authority ends the host (a special URL must have one). */
	if (c == URL_EOF || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
		parser->pointer--;
		if (special && parser->buffer.length == 0)
			return EINVAL;
		error = url_parse_host(parser);
		parser->state = URL_PATH_START;
		return error;
	}

	/* Brackets hold an IPv6 address, whose colons are not the port's. */
	if (c == '[')
		parser->inside_brackets = 1;
	if (c == ']')
		parser->inside_brackets = 0;
	error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
	if (error != 0)
		return error;

	/* Succeeded: the character is gathered. */
	return 0;
}

/* The port state: digits up to the end of the authority, 65535 at most; the scheme's default is no port. */
static int
url_port(
	struct url_parser *parser,
	int c)
{
	long port;
	size_t index;
	int special;
	int default_port;
	int error;

	/* A digit gathers. */
	if (c >= '0' && c <= '9') {
		error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
		return error;
	}

	/* Anything but the end of the authority is not a port. */
	special = url_is_special(parser);
	if (c != URL_EOF && c != '/' && c != '?' && c != '#' && !(special && c == '\\'))
		return EINVAL;

	/* The number, when there are digits. */
	if (parser->buffer.length != 0) {
		port = 0;
		for (index = 0; index < parser->buffer.length; index++) {
			port = port * 10 + (parser->buffer.data[index] - '0');
			if (port > 65535)
				return EINVAL;
		}

		/* The scheme's default port is no port. */
		default_port = net_url_default_port(wb_buffer_string(&parser->url.scheme));
		parser->url.port = (int)port;
		if (port == default_port)
			parser->url.port = -1;
		wb_buffer_clear(&parser->buffer);
	}

	/* Succeeded: the path starts. */
	parser->state = URL_PATH_START;
	parser->pointer--;
	return 0;
}

/* The file state: a file URL's empty host, and the base's parts unless the input replaces them. */
static int
url_file(
	struct url_parser *parser,
	int c)
{
	struct url_parts *url;
	int base_is_file;
	int drive;
	int error;

	/* The scheme is file, and the host empty. */
	url = &parser->url;
	wb_buffer_clear(&url->scheme);
	error = wb_buffer_append_string(&url->scheme, "file");
	wb_buffer_clear(&url->host);
	url->has_host = 1;
	if (error != 0)
		return error;

	/* A slash (or backslash) starts the host or the path. */
	if (c == '/' || c == '\\') {
		parser->state = URL_FILE_SLASH;
		return 0;
	}

	/* Without a file base the rest is the path. */
	base_is_file = 0;
	if (parser->base != NULL)
		base_is_file = url_named(parser->base->scheme, "file");
	if (!base_is_file) {
		parser->state = URL_PATH;
		parser->pointer--;
		return 0;
	}

	/* The base's host, path and query, unless the input replaces them. */
	error = url_copy_base(parser, 1, 1);
	if (error != 0)
		return error;
	if (c == '?') {
		url_begin_query(parser);
	} else if (c == '#') {
		url_begin_fragment(parser);
	} else if (c != URL_EOF) {
		/* A path relative to the base's, or from a drive letter of its own. */
		wb_buffer_clear(&url->query);
		url->has_query = 0;
		drive = url_starts_with_drive_letter(parser, parser->pointer);
		if (drive) {
			url_clear_path(url);
		} else {
			url_shorten_path(url);
		}

		/* The path follows. */
		parser->state = URL_PATH;
		parser->pointer--;
	}

	/* Succeeded: the next state is chosen. */
	return 0;
}

/* The file slash state: a second slash starts a host; otherwise the base's host (and drive letter) and a path. */
static int
url_file_slash(
	struct url_parser *parser,
	int c)
{
	const struct net_url *base;
	struct url_parts *url;
	size_t first_length;
	int base_is_file;
	int drive;
	int base_drive;
	int error;

	/* A second slash starts the host. */
	if (c == '/' || c == '\\') {
		parser->state = URL_FILE_HOST;
		return 0;
	}

	/* The path follows (whether it starts with a drive letter decides about the base's). */
	drive = url_starts_with_drive_letter(parser, parser->pointer);
	parser->state = URL_PATH;
	parser->pointer--;

	/* Without a file base, nothing more. */
	base = parser->base;
	url = &parser->url;
	base_is_file = 0;
	if (base != NULL)
		base_is_file = url_named(base->scheme, "file");
	if (!base_is_file)
		return 0;

	/* The base's host. */
	wb_buffer_clear(&url->host);
	url->has_host = 1;
	error = 0;
	if (base->host != NULL)
		error = wb_buffer_append_string(&url->host, base->host);
	if (error != 0)
		return error;

	/* The base's drive letter, unless the input has one. */
	if (drive || base->path[0] != '/')
		return 0;
	first_length = strcspn(base->path + 1, "/");
	base_drive = url_is_drive_letter(base->path + 1, first_length, 1);
	if (base_drive)
		error = url_push_segment(url, base->path + 1, 2);
	if (error != 0)
		return error;

	/* Succeeded: the path follows. */
	return 0;
}

/* The file host state: a file URL's host (localhost is none), or a drive letter where the host would be. */
static int
url_file_host(
	struct url_parser *parser,
	int c)
{
	struct url_parts *url;
	int drive;
	int local;
	int error;

	/* Gathers up to the end of the host. */
	if (c != URL_EOF && c != '/' && c != '\\' && c != '?' && c != '#') {
		error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
		return error;
	}

	/* The character after the host is read again by the next state. */
	parser->pointer--;

	/* A drive letter is the path's (the buffer stays for the path state). */
	url = &parser->url;
	drive = url_is_drive_letter((const char *)parser->buffer.data, parser->buffer.length, 0);
	if (drive) {
		parser->state = URL_PATH;
		return 0;
	}

	/* No host is the empty host. */
	parser->state = URL_PATH_START;
	if (parser->buffer.length == 0) {
		wb_buffer_clear(&url->host);
		url->has_host = 1;
		return 0;
	}

	/* A host; localhost is the empty host. */
	error = url_parse_host(parser);
	if (error != 0)
		return error;
	local = url_named(wb_buffer_string(&url->host), "localhost");
	if (local)
		wb_buffer_clear(&url->host);

	/* Succeeded: the host is set. */
	return 0;
}

/* The path start state: the path, or a query or fragment right away for a URL that is not special. */
static int
url_path_start(
	struct url_parser *parser,
	int c)
{
	int special;

	/* A special URL always has a path (its first slash is taken here). */
	special = url_is_special(parser);
	if (special) {
		parser->state = URL_PATH;
		if (c != '/' && c != '\\')
			parser->pointer--;
		return 0;
	}

	/* Otherwise a query, a fragment, a path, or nothing. */
	if (c == '?') {
		url_begin_query(parser);
	} else if (c == '#') {
		url_begin_fragment(parser);
	} else if (c != URL_EOF) {
		parser->state = URL_PATH;
		if (c != '/')
			parser->pointer--;
	}

	/* Succeeded: the next state is chosen. */
	return 0;
}

/* The path state: segments up to each slash, . and .. resolved, then a query or fragment. */
static int
url_path(
	struct url_parser *parser,
	int c)
{
	struct url_parts *url;
	int ends;
	int is_file;
	int drive;
	int slash;
	int special;
	int double_dot;
	int single_dot;
	int error;

	/* Inside a segment the character is encoded. */
	ends = url_ends_segment(parser, c);
	if (!ends) {
		error = url_encode(&parser->buffer, c, URL_SET_PATH);
		return error;
	}

	/* Whether the segment ends at a slash (the path goes on). */
	url = &parser->url;
	special = url_is_special(parser);
	slash = 0;
	if (c == '/' || (c == '\\' && special))
		slash = 1;

	/* ".." goes up; "." stays; anything else is a segment (a file URL's drive letter normalized). */
	error = 0;
	double_dot = url_dot_segment(&parser->buffer, 2);
	single_dot = url_dot_segment(&parser->buffer, 1);
	if (double_dot) {
		url_shorten_path(url);
		if (!slash)
			error = url_push_segment(url, "", 0);
	} else if (single_dot) {
		if (!slash)
			error = url_push_segment(url, "", 0);
	} else {
		is_file = url_scheme_is(parser, "file");
		drive = url_is_drive_letter((const char *)parser->buffer.data, parser->buffer.length, 0);
		if (is_file && url->segments.count == 0 && drive)
			parser->buffer.data[1] = ':';
		error = url_push_segment(url, (const char *)parser->buffer.data, parser->buffer.length);
	}

	/* The segment is used. */
	wb_buffer_clear(&parser->buffer);
	if (error != 0)
		return error;

	/* A query or a fragment after the path. */
	if (c == '?')
		url_begin_query(parser);
	if (c == '#')
		url_begin_fragment(parser);

	/* Succeeded: the segment is added. */
	return 0;
}

/* The opaque path state: everything up to a query or fragment, controls and non-ASCII encoded. */
static int
url_opaque_path(
	struct url_parser *parser,
	int c)
{
	int next;
	int error;

	/* A query or a fragment ends the path, and so does the end. */
	if (c == '?') {
		url_begin_query(parser);
		return 0;
	} else if (c == '#') {
		url_begin_fragment(parser);
		return 0;
	} else if (c == URL_EOF) {
		return 0;
	}

	/* A space before a query or fragment is encoded (it would otherwise be stripped as trailing). */
	next = url_char(parser, parser->pointer + 1);
	if (c == ' ' && (next == '?' || next == '#')) {
		error = wb_buffer_append_string(&parser->url.opaque, "%20");
		return error;
	}

	/* Any other character. */
	error = url_encode(&parser->url.opaque, c, URL_SET_C0);
	if (error != 0)
		return error;

	/* Succeeded: the character is added. */
	return 0;
}

/* The query state: characters gather up to a fragment or the end, then are encoded in the query's set. */
static int
url_query(
	struct url_parser *parser,
	int c)
{
	size_t index;
	int set;
	int special;
	int error;

	/* Gathers up to a fragment or the end. */
	if (c != '#' && c != URL_EOF) {
		error = wb_buffer_append_byte(&parser->buffer, (unsigned char)c);
		return error;
	}

	/* The query, encoded in the special-query set for special schemes. */
	special = url_is_special(parser);
	set = URL_SET_QUERY;
	if (special)
		set = URL_SET_SPECIAL_QUERY;
	error = 0;
	for (index = 0; error == 0 && index < parser->buffer.length; index++)
		error = url_encode(&parser->url.query, parser->buffer.data[index], set);
	wb_buffer_clear(&parser->buffer);
	if (error != 0)
		return error;

	/* A fragment follows the query. */
	if (c == '#')
		url_begin_fragment(parser);

	/* Succeeded: the query is set. */
	return 0;
}

/* Starts an empty query (it replaces any the URL had). */
static void
url_begin_query(
	struct url_parser *parser)
{
	/* The query is there, empty. */
	wb_buffer_clear(&parser->url.query);
	parser->url.has_query = 1;
	parser->state = URL_QUERY;
}

/* Starts an empty fragment. */
static void
url_begin_fragment(
	struct url_parser *parser)
{
	/* The fragment is there, empty. */
	wb_buffer_clear(&parser->url.fragment);
	parser->url.has_fragment = 1;
	parser->state = URL_FRAGMENT;
}

/* Tells whether a character ends a path segment: the end, a slash (a backslash for special URLs), ? or #. */
static int
url_ends_segment(
	const struct url_parser *parser,
	int c)
{
	int special;

	/* The characters that always end one. */
	if (c == URL_EOF || c == '/' || c == '?' || c == '#')
		return 1;

	/* A backslash in a special URL. */
	special = url_is_special(parser);
	if (special && c == '\\')
		return 1;
	return 0;
}

/* Reports the input's byte at a place, or URL_EOF past its end. */
static int
url_char(
	const struct url_parser *parser,
	long at)
{
	/* Past the end. */
	if (at < 0 || (size_t)at >= parser->length)
		return URL_EOF;

	/* The byte. */
	return (unsigned char)parser->input[at];
}

/* Tells whether a scheme is special. */
static int
url_special_scheme(
	const char *scheme)
{
	size_t index;
	int differs;

	/* One of the table's. */
	for (index = 0; url_specials[index].scheme != NULL; index++) {
		differs = strcmp(scheme, url_specials[index].scheme);
		if (differs == 0)
			return 1;
	}

	/* Another scheme. */
	return 0;
}

/* Tells whether the URL being parsed has a special scheme. */
static int
url_is_special(
	const struct url_parser *parser)
{
	int special;

	/* By its scheme so far. */
	special = url_special_scheme(wb_buffer_string(&parser->url.scheme));

	/* Reports it. */
	return special;
}

/* Tells whether the URL being parsed has a scheme. */
static int
url_scheme_is(
	const struct url_parser *parser,
	const char *scheme)
{
	int differs;

	/* The scheme so far against the name. */
	differs = strcmp(wb_buffer_string(&parser->url.scheme), scheme);
	if (differs != 0)
		return 0;
	return 1;
}

/* Tells whether a byte is in a percent-encode set. */
static int
url_in_set(
	int c,
	int set)
{
	/* The C0 control set: controls and everything past ~ (every byte of a non-ASCII character). */
	if (c < 0x20 || c > 0x7e)
		return 1;
	if (set == URL_SET_C0)
		return 0;

	/* The fragment set. */
	if (set == URL_SET_FRAGMENT) {
		if (c == ' ' || c == '"' || c == '<' || c == '>' || c == '`')
			return 1;
		return 0;
	}

	/* The query set, and the special-query set with the apostrophe. */
	if (c == ' ' || c == '"' || c == '#' || c == '<' || c == '>')
		return 1;
	if (set == URL_SET_QUERY)
		return 0;
	if (set == URL_SET_SPECIAL_QUERY) {
		if (c == '\'')
			return 1;
		return 0;
	}

	/* The path set. */
	if (c == '?' || c == '^' || c == '`' || c == '{' || c == '}')
		return 1;
	if (set == URL_SET_PATH)
		return 0;

	/* The userinfo set. */
	if (c == '/' || c == ':' || c == ';' || c == '=' || c == '@' || (c >= '[' && c <= ']') || c == '|')
		return 1;
	return 0;
}

/* Appends a byte, percent-encoded when it is in a set. */
static int
url_encode(
	struct wb_buffer *out,
	int c,
	int set)
{
	int inside;
	int error;

	/* In the set: %XX in upper case; otherwise the byte as it is. */
	inside = url_in_set(c, set);
	if (inside) {
		error = wb_buffer_printf(out, "%%%02X", (unsigned)c);
	} else {
		error = wb_buffer_append_byte(out, (unsigned char)c);
	}

	/* A buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the byte is appended. */
	return 0;
}

/* Copies the base's credentials, host and port, and when asked its path and query, into the URL. */
static int
url_copy_base(
	struct url_parser *parser,
	int path,
	int query)
{
	const struct net_url *base;
	struct url_parts *url;
	int error;

	/* The credentials. */
	base = parser->base;
	url = &parser->url;
	wb_buffer_clear(&url->username);
	wb_buffer_clear(&url->password);
	wb_buffer_clear(&url->host);
	error = wb_buffer_append_string(&url->username, base->username);
	if (error == 0)
		error = wb_buffer_append_string(&url->password, base->password);

	/* The host and the port. */
	url->has_host = 0;
	if (error == 0 && base->host != NULL) {
		url->has_host = 1;
		error = wb_buffer_append_string(&url->host, base->host);
	}

	/* The port. */
	url->port = base->port;

	/* The path. */
	if (error == 0 && path)
		error = url_split_path(base, url);

	/* The query. */
	if (error == 0 && query && base->query != NULL) {
		url->has_query = 1;
		wb_buffer_clear(&url->query);
		error = wb_buffer_append_string(&url->query, base->query);
	}

	/* A copy that could not be made. */
	if (error != 0)
		return error;

	/* Succeeded: the base's parts are copied. */
	return 0;
}

/* Adds a segment at the end of the path. */
static int
url_push_segment(
	struct url_parts *parts,
	const char *text,
	size_t length)
{
	char *segment;
	int error;

	/* A copy of the text. */
	segment = malloc(length + 1U);
	if (segment == NULL)
		return ENOMEM;
	if (length != 0)
		memcpy(segment, text, length);
	segment[length] = '\0';

	/* At the end of the list. */
	error = wb_vector_push(&parts->segments, &segment);
	if (error != 0) {
		free(segment);
		return error;
	}

	/* Succeeded: the path has the segment. */
	return 0;
}

/* Empties the path. */
static void
url_clear_path(
	struct url_parts *parts)
{
	size_t index;

	/* Each segment's string, then the list. */
	for (index = 0; index < parts->segments.count; index++)
		free(*(char **)wb_vector_at(&parts->segments, index));
	wb_vector_clear(&parts->segments);
}

/* Removes the path's last segment, unless it is a file URL's only drive letter. */
static void
url_shorten_path(
	struct url_parts *parts)
{
	char *last;
	int is_file;
	int drive;
	int differs;

	/* An empty path stays. */
	if (parts->segments.count == 0)
		return;

	/* A file URL keeps its drive letter. */
	differs = strcmp(wb_buffer_string(&parts->scheme), "file");
	is_file = 0;
	if (differs == 0)
		is_file = 1;
	last = *(char **)wb_vector_at(&parts->segments, 0);
	drive = url_is_drive_letter(last, strlen(last), 1);
	if (is_file && parts->segments.count == 1 && drive)
		return;

	/* The last segment goes. */
	last = *(char **)wb_vector_at(&parts->segments, parts->segments.count - 1U);
	free(last);
	wb_vector_pop(&parts->segments);
}

/* Tells whether a text is a Windows drive letter (a letter and : or |; only : when normalized). */
static int
url_is_drive_letter(
	const char *text,
	size_t length,
	int normalized)
{
	/* Two characters. */
	if (text == NULL || length != 2U)
		return 0;

	/* A letter. */
	if ((text[0] | 0x20) < 'a' || (text[0] | 0x20) > 'z')
		return 0;

	/* Then a colon, or a bar when not normalized. */
	if (text[1] == ':')
		return 1;
	if (text[1] == '|' && !normalized)
		return 1;
	return 0;
}

/* Tells whether the input from a place starts with a Windows drive letter (followed by the end or / \ ? #). */
static int
url_starts_with_drive_letter(
	const struct url_parser *parser,
	long at)
{
	int after;
	int is_letter;

	/* Two characters that are a drive letter. */
	if (at < 0 || (size_t)at + 2U > parser->length)
		return 0;
	is_letter = url_is_drive_letter(parser->input + at, 2, 0);
	if (!is_letter)
		return 0;

	/* The end, or a character that ends it. */
	after = url_char(parser, at + 2);
	if (after == URL_EOF || after == '/' || after == '\\' || after == '?' || after == '#')
		return 1;
	return 0;
}

/* Tells whether a buffer is a single-dot segment (dots 1) or a double-dot one (dots 2), with %2e counting as a dot. */
static int
url_dot_segment(
	const struct wb_buffer *buffer,
	int dots)
{
	const char *text;
	size_t index;
	int found;

	/* Counts the dots, each "." or "%2e". */
	text = wb_buffer_string(buffer);
	index = 0;
	found = 0;
	while (index < buffer->length) {
		if (text[index] == '.') {
			index++;
		} else if (index + 3U <= buffer->length && text[index] == '%' && text[index + 1U] == '2' &&
		    (text[index + 2U] | 0x20) == 'e') {
			index += 3U;
		} else {
			return 0;
		}

		/* One more dot. */
		found++;
	}

	/* The count must be the one asked for. */
	if (found != dots)
		return 0;
	return 1;
}

/* Parses the gathered text as the URL's host (opaque for a scheme that is not special), and empties the buffer. */
static int
url_parse_host(
	struct url_parser *parser)
{
	struct wb_buffer host;
	int special;
	int error;

	/* The host's serialization. */
	special = url_is_special(parser);
	wb_buffer_init(&host);
	error = net_host_parse(wb_buffer_string(&parser->buffer), parser->buffer.length, !special, &host);
	wb_buffer_clear(&parser->buffer);
	if (error != 0) {
		wb_buffer_release(&host);
		return error;
	}

	/* It replaces the URL's. */
	wb_buffer_clear(&parser->url.host);
	error = wb_buffer_append(&parser->url.host, host.data, host.length);
	parser->url.has_host = 1;
	wb_buffer_release(&host);
	if (error != 0)
		return error;

	/* Succeeded: the URL has its host. */
	return 0;
}

/* Turns the parts into the URL's strings. */
static int
url_finish(
	struct url_parts *parts,
	struct net_url *url)
{
	struct wb_buffer path;
	const char *segment;
	size_t index;
	int error;

	/* The path: the opaque one, or each segment after a slash. */
	wb_buffer_init(&path);
	error = 0;
	if (parts->opaque_path) {
		error = wb_buffer_append(&path, parts->opaque.data, parts->opaque.length);
	} else {
		for (index = 0; error == 0 && index < parts->segments.count; index++) {
			segment = *(char **)wb_vector_at(&parts->segments, index);
			error = wb_buffer_printf(&path, "/%s", segment);
		}
	}

	/* The parts that are always there. */
	url->scheme = url_strdup(&parts->scheme);
	url->username = url_strdup(&parts->username);
	url->password = url_strdup(&parts->password);
	url->path = url_strdup(&path);
	url->port = parts->port;
	url->opaque_path = parts->opaque_path;
	wb_buffer_release(&path);

	/* The parts that may be missing. */
	url->host = NULL;
	if (parts->has_host)
		url->host = url_strdup(&parts->host);
	url->query = NULL;
	if (parts->has_query)
		url->query = url_strdup(&parts->query);
	url->fragment = NULL;
	if (parts->has_fragment)
		url->fragment = url_strdup(&parts->fragment);

	/* Any copy may have failed. */
	if (error != 0)
		return error;
	if (url->scheme == NULL || url->username == NULL || url->password == NULL || url->path == NULL)
		return ENOMEM;
	if (parts->has_host && url->host == NULL)
		return ENOMEM;
	if (parts->has_query && url->query == NULL)
		return ENOMEM;
	if (parts->has_fragment && url->fragment == NULL)
		return ENOMEM;

	/* Succeeded: the URL has its strings. */
	return 0;
}

/* Copies a buffer into a new string (an empty one for an empty buffer). */
static char *
url_strdup(
	const struct wb_buffer *buffer)
{
	char *copy;

	/* The bytes and a terminator. */
	copy = malloc(buffer->length + 1U);
	if (copy == NULL)
		return NULL;
	if (buffer->length != 0)
		memcpy(copy, buffer->data, buffer->length);
	copy[buffer->length] = '\0';

	/* The copy. */
	return copy;
}

/* Splits a URL's serialized path into the segments of the URL being parsed. */
static int
url_split_path(
	const struct net_url *url,
	struct url_parts *parts)
{
	const char *start;
	const char *slash;
	int error;

	/* The segments the parts had go. */
	url_clear_path(parts);

	/* An opaque path is kept whole. */
	if (url->opaque_path) {
		parts->opaque_path = 1;
		wb_buffer_clear(&parts->opaque);
		error = wb_buffer_append_string(&parts->opaque, url->path);
		return error;
	}

	/* Each segment after a slash. */
	error = 0;
	start = url->path;
	while (error == 0 && *start == '/') {
		start++;
		slash = strchr(start, '/');
		if (slash == NULL)
			slash = start + strlen(start);
		error = url_push_segment(parts, start, (size_t)(slash - start));
		start = slash;
	}

	/* A segment that could not be copied. */
	if (error != 0)
		return error;

	/* Succeeded: the path is split. */
	return 0;
}

/* Writes a URL's origin: scheme://host[:port] for the network schemes, a blob: URL's inner URL's, or null. */
static int
url_origin(
	const struct net_url *url,
	struct wb_buffer *out)
{
	struct net_url inner;
	int is_blob;
	int is_web;
	int is_file;
	int special;
	int error;

	/* A blob: URL's origin is its path's, when that is an http or https URL. */
	is_blob = url_named(url->scheme, "blob");
	if (is_blob) {
		error = net_url_parse(url->path, strlen(url->path), NULL, &inner);
		is_web = 0;
		if (error == 0) {
			is_web = url_named(inner.scheme, "http");
			is_web += url_named(inner.scheme, "https");
		}

		/* The inner URL's origin, or an opaque one. */
		if (is_web) {
			error = url_origin(&inner, out);
		} else {
			error = wb_buffer_append_string(out, "null");
		}

		/* The inner URL goes. */
		net_url_release(&inner);
		return error;
	}

	/* A tuple origin for the network schemes; an opaque one (null) otherwise. */
	special = url_special_scheme(url->scheme);
	is_file = url_named(url->scheme, "file");
	if (!special || is_file) {
		error = wb_buffer_append_string(out, "null");
		return error;
	}

	/* scheme://host, and the port when there is one. */
	error = wb_buffer_printf(out, "%s://%s", url->scheme, url->host);
	if (error == 0 && url->port >= 0)
		error = wb_buffer_printf(out, ":%d", url->port);
	if (error != 0)
		return error;

	/* Succeeded: the origin is written. */
	return 0;
}

/* Reports a hexadecimal digit's value, or -1 for another character. */
static int
url_hex(
	char digit)
{
	/* The three ranges of digits. */
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;

	/* Not a digit. */
	return -1;
}

/* Tells whether a text is a name (a scheme or a host). */
static int
url_named(
	const char *text,
	const char *name)
{
	int differs;

	/* The two strings. */
	differs = strcmp(text, name);
	if (differs != 0)
		return 0;
	return 1;
}

/* Makes the User-Agent from the system's part of it. */
static void
url_agent_make(void)
{
	const char *system;

	/* The browser's names around the system's. */
	system = wb_os_user_agent_system();
	snprintf(
		url_agent,
		sizeof(url_agent),
		"Mozilla/5.0 (%s) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36",
		system);
}
