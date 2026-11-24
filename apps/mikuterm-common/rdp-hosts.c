/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Parser and formatter for rdp/hosts (ssh_config-compatible Host blocks)
 * and user@host:port RDP target strings.
 */

#include "rdp-hosts.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int eq_ci(const char *a, const char *b);
static void trim_span(const char *line, size_t len, const char **start, const char **end);
static void copy_field(char *dst, size_t dst_sz, const char *src, size_t src_len);
static int parse_u32(const char *s, size_t len, uint32_t *out);

/*
 * Parses rdp/hosts text into list using default_port when Port is omitted.
 */
int
rdp_hosts_parse(
	const char *text,
	size_t size,
	uint16_t default_port,
	struct rdp_host_list *list)
{
	size_t pos;
	size_t line_start;
	size_t line_len;
	const char *s;
	const char *e;
	const char *kw_end;
	const char *val;
	char kw[32];
	size_t kw_len;
	struct rdp_host_entry *cur;
	uint32_t num;

	/* Validates arguments and initializes the output list. */
	if (list == NULL)
		return EINVAL;
	memset(list, 0, sizeof(*list));
	if (text == NULL || size == 0U)
		return 0;
	cur = NULL;
	pos = 0U;

	/* Iterates line by line over the configuration text. */
	while (pos < size) {
		line_start = pos;
		while (pos < size && text[pos] != '\n' && text[pos] != '\r')
			pos++;
		line_len = pos - line_start;
		while (pos < size && (text[pos] == '\n' || text[pos] == '\r'))
			pos++;
		trim_span(text + line_start, line_len, &s, &e);
		if (s == e || *s == '#')
			continue;
		kw_end = s;
		while (kw_end < e && !isspace((unsigned char)*kw_end) &&
		       *kw_end != '=') {
			kw_end++;
		}
		kw_len = (size_t)(kw_end - s);
		if (kw_len == 0U || kw_len >= sizeof(kw))
			continue;
		memcpy(kw, s, kw_len);
		kw[kw_len] = '\0';
		val = kw_end;
		while (val < e && (isspace((unsigned char)*val) || *val == '='))
			val++;
		if (val < e && *val == '"' && e - val >= 2 && *(e - 1) == '"') {
			val++;
			e--;
		}

		/* Starts a new Host block when Host is encountered. */
		if (eq_ci(kw, "Host")) {
			if (list->count >= RDP_HOSTS_MAX) {
				cur = NULL;
				continue;
			}
			cur = &list->entries[list->count++];
			memset(cur, 0, sizeof(*cur));
			cur->port = default_port > 0U ? default_port : 3389U;
			copy_field(cur->name, sizeof(cur->name), val,
				   (size_t)(e - val));
			copy_field(cur->host_name, sizeof(cur->host_name), val,
				   (size_t)(e - val));
			continue;
		}
		if (cur == NULL)
			continue;
		if (eq_ci(kw, "HostName")) {
			copy_field(cur->host_name, sizeof(cur->host_name), val,
				   (size_t)(e - val));
		} else if (eq_ci(kw, "User")) {
			copy_field(cur->user, sizeof(cur->user), val,
				   (size_t)(e - val));
		} else if (eq_ci(kw, "Domain")) {
			copy_field(cur->domain, sizeof(cur->domain), val,
				   (size_t)(e - val));
		} else if (eq_ci(kw, "Port")) {
			if (parse_u32(val, (size_t)(e - val), &num) == 0 &&
			    num >= 1U && num <= 65535U) {
				cur->port = (uint16_t)num;
			}
		} else if (eq_ci(kw, "Width")) {
			if (parse_u32(val, (size_t)(e - val), &num) == 0 &&
			    num >= 320U && num <= 7680U) {
				cur->width = num;
			}
		} else if (eq_ci(kw, "Height")) {
			if (parse_u32(val, (size_t)(e - val), &num) == 0 &&
			    num >= 200U && num <= 4320U) {
				cur->height = num;
			}
		}
	}

	/* Succeeded: rdp/hosts parsed. */
	return 0;
}

/*
 * Formats an rdp_host_list back into ssh_config-style Host blocks.
 */
int
rdp_hosts_format(
	const struct rdp_host_list *list,
	char **out,
	size_t *out_size)
{
	size_t cap;
	size_t len;
	char *buf;
	unsigned i;
	int n;
	const struct rdp_host_entry *e;
	if (list == NULL || out == NULL || out_size == NULL)
		return EINVAL;
	cap = 4096U;
	buf = (char *)malloc(cap);
	if (buf == NULL)
		return ENOMEM;
	len = 0U;
	buf[0] = '\0';

	/* Appends each Host block with its non-default settings. */
	for (i = 0U; i < list->count; i++) {
		e = &list->entries[i];
		if (len + 512U >= cap) {
			char *grown;
			cap *= 2U;
			grown = (char *)realloc(buf, cap);
			if (grown == NULL) {
				free(buf);
				return ENOMEM;
			}
			buf = grown;
		}
		n = snprintf(buf + len, cap - len, "Host %s\n", e->name);
		if (n > 0)
			len += (size_t)n;
		if (e->host_name[0] != '\0') {
			n = snprintf(buf + len, cap - len, "    HostName %s\n",
				     e->host_name);
			if (n > 0)
				len += (size_t)n;
		}
		if (e->port != 0U && e->port != 3389U) {
			n = snprintf(buf + len, cap - len, "    Port %u\n",
				     (unsigned)e->port);
			if (n > 0)
				len += (size_t)n;
		}
		if (e->user[0] != '\0') {
			n = snprintf(buf + len, cap - len, "    User %s\n",
				     e->user);
			if (n > 0)
				len += (size_t)n;
		}
		if (e->domain[0] != '\0') {
			n = snprintf(buf + len, cap - len, "    Domain %s\n",
				     e->domain);
			if (n > 0)
				len += (size_t)n;
		}
		if (e->width > 0U && e->height > 0U) {
			n = snprintf(buf + len, cap - len,
				     "    Width %u\n    Height %u\n",
				     e->width, e->height);
			if (n > 0)
				len += (size_t)n;
		}
		n = snprintf(buf + len, cap - len, "\n");
		if (n > 0)
			len += (size_t)n;
	}
	*out = buf;
	*out_size = len;

	/* Succeeded: host list formatted. */
	return 0;
}

/*
 * Parses [rdp://][domain\][user@]host[:port] into an rdp_host_entry.
 */
int
rdp_hosts_parse_target(
	const char *spec,
	uint16_t default_port,
	struct rdp_host_entry *out)
{
	const char *s;
	const char *at;
	const char *slash;
	const char *colon;
	uint32_t port_num;
	if (spec == NULL || out == NULL)
		return EINVAL;
	memset(out, 0, sizeof(*out));
	out->port = default_port > 0U ? default_port : 3389U;
	s = spec;
	while (*s != '\0' && isspace((unsigned char)*s))
		s++;
	if (strncmp(s, "rdp://", 6U) == 0)
		s += 6U;
	if (*s == '\0')
		return EINVAL;

	/* Splits optional [domain\]user@ prefix. */
	at = strchr(s, '@');
	if (at != NULL) {
		slash = strchr(s, '\\');
		if (slash != NULL && slash < at) {
			copy_field(out->domain, sizeof(out->domain), s,
				   (size_t)(slash - s));
			copy_field(out->user, sizeof(out->user), slash + 1,
				   (size_t)(at - (slash + 1)));
		} else {
			copy_field(out->user, sizeof(out->user), s,
				   (size_t)(at - s));
		}
		s = at + 1;
	}
	if (*s == '\0')
		return EINVAL;

	/* Splits optional :port suffix (ignoring IPv6 bracketed literals). */
	colon = strrchr(s, ':');
	if (colon != NULL && strchr(colon, ']') == NULL) {
		if (parse_u32(colon + 1, strlen(colon + 1), &port_num) != 0 ||
		    port_num < 1U || port_num > 65535U) {
			return EINVAL;
		}
		out->port = (uint16_t)port_num;
		copy_field(out->host_name, sizeof(out->host_name), s,
			   (size_t)(colon - s));
	} else {
		copy_field(out->host_name, sizeof(out->host_name), s, strlen(s));
	}
	if (out->host_name[0] == '\0')
		return EINVAL;
	copy_field(out->name, sizeof(out->name), out->host_name,
		   strlen(out->host_name));

	/* Succeeded: target specification parsed. */
	return 0;
}

/* Compares two ASCII strings case-insensitively. */
static int
eq_ci(
	const char *a,
	const char *b)
{
	while (*a != '\0' && *b != '\0') {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	if (*a != '\0' || *b != '\0')
		return 0;
	return 1;
}

/* Trims leading and trailing ASCII whitespace from a line slice. */
static void
trim_span(
	const char *line,
	size_t len,
	const char **start,
	const char **end)
{
	const char *s;
	const char *e;
	s = line;
	e = line + len;
	while (s < e && isspace((unsigned char)*s))
		s++;
	while (e > s && isspace((unsigned char)*(e - 1)))
		e--;
	*start = s;
	*end = e;
}

/* Copies up to dst_sz - 1 bytes into dst and NUL-terminates it. */
static void
copy_field(
	char *dst,
	size_t dst_sz,
	const char *src,
	size_t src_len)
{
	size_t n;
	if (dst_sz == 0U)
		return;
	n = src_len;
	if (n >= dst_sz)
		n = dst_sz - 1U;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/* Parses an unsigned decimal integer from a fixed-length string slice. */
static int
parse_u32(
	const char *s,
	size_t len,
	uint32_t *out)
{
	size_t i;
	uint32_t v;
	if (len == 0U)
		return EINVAL;
	v = 0U;
	for (i = 0U; i < len; i++) {
		if (!isdigit((unsigned char)s[i]))
			return EINVAL;
		if (v > 429496729U)
			return ERANGE;
		v = v * 10U + (uint32_t)(s[i] - '0');
	}
	*out = v;
	return 0;
}
