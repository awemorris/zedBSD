/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Cookies (RFC 6265): an instance's cookie jar, the cookies of its
 * responses and of its pages' document.cookie, and the Cookie header of
 * a request.
 *
 * A cookie has a name, a value, a domain (the request's host, or its
 * Domain attribute, which must domain-match the host), a path (its Path
 * attribute, or the request path's directory), flags (host-only without a
 * Domain, Secure, HttpOnly) and an expiry.  Max-Age, or else Expires,
 * gives it an expiry (RFC 6265 section 5.3, step 3) and makes it
 * persistent; without either it is a session cookie, which lasts as long
 * as the jar.  A cookie whose expiry has passed is removed when it is set,
 * when a request or a script would see it, and when the jar is opened.
 * A script (document.cookie) neither sees nor sets an HttpOnly cookie.
 * The SameSite attribute is not read yet.
 *
 * The jar keeps its persistent cookies in the instance's storage
 * (BROWSER_STORAGE_COOKIES, one record per cookie, storage/records.h): it
 * reads them all when it is opened, and queues a write when one is set and
 * a remove when one goes.  The queue writes behind (storage/queue.h), so
 * no call here waits for the disk.  Session cookies are never written.
 * Since every record of the space is in the jar after the opening, the key
 * of a new record is chosen among the keys the jar holds, without reading.
 *
 * The jar is shared by the page's thread (document.cookie, the blocking
 * fetches) and the loaders' network threads (their requests and
 * responses), so each call below holds the jar's lock while it reads or
 * changes it.  The storage queue's lock is taken inside it.
 */

#include "net/net.h"
#include "base/os-time.h"
#include "storage/queue.h"
#include "storage/records.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The most cookies a jar keeps (the oldest is dropped first). */
#define COOKIE_MAX		512U

/* The most collision numbers tried for the key of a cookie's record (storage/records.h). */
#define COOKIE_COLLISIONS	16U

/* The expiry of a session cookie, which has none. */
#define COOKIE_SESSION		INT64_C(-1)

/* The latest expiry kept, 9999-12-31 23:59:59.999 UTC in milliseconds: later ones are this. */
#define COOKIE_EXPIRY_MAX	INT64_C(253402300799999)

/* Milliseconds in a second and in a day. */
#define COOKIE_SECOND		INT64_C(1000)
#define COOKIE_DAY		INT64_C(86400000)

/*
 * One cookie of a jar: malloc'd strings, its flags, its times and the key
 * of its record in the storage.
 */
struct cookie {
	char *name;
	char *value;
	char *domain;
	char *path;
	int host_only;
	int secure;
	int http_only;
	unsigned same_site;	/* WB_STORAGE_COOKIE_SAME_SITE_* as its record had it (unset until SameSite is read) */
	int64_t expires;	/* ms since 1970 UTC, or COOKIE_SESSION */
	int64_t created;	/* when it was first set, kept when it is replaced */
	int64_t used;		/* when it was last sent or read; changed in memory only */
	char key[BROWSER_STORAGE_KEY_MAX + 1];	/* its record's key, "" when it has none (a session cookie) */
};

/*
 * An instance's cookie jar: the cookies in the order they were set (the
 * first is the oldest), under the lock, and the storage queue their
 * records go to.  It lives as long as its instance; the instance's views
 * (and their loaders) are gone before it is closed.
 */
struct net_cookie_jar {
	struct cookie cookies[COOKIE_MAX];
	size_t count;
	pthread_mutex_t lock;
	struct wb_storage_queue *queue;
};

/*
 * The month names of a cookie date, by their first three letters in lower
 * case (January first).  The table is constant for the life of the
 * program.
 */
static const char *const cookie_months[12] = {
	"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
};

/*
 * The days of each month of a year that is not a leap year (January
 * first).  The table is constant for the life of the program.
 */
static const int cookie_month_days[12] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int cookie_load(struct net_cookie_jar *jar);
static int cookie_load_one(struct net_cookie_jar *jar, const char *key, int64_t now);
static int cookie_from_record(const struct wb_storage_cookie *record, const char *key, struct cookie *cookie);
static void cookie_sort(struct net_cookie_jar *jar);
static int cookie_store(struct net_cookie_jar *jar, const struct net_url *url, const char *header, size_t length, int from_script);
static int cookie_pairs(struct net_cookie_jar *jar, const struct net_url *url, int for_script, struct wb_buffer *out);
static int64_t cookie_now(void);
static int cookie_max_age(const char *value, size_t length, int64_t now, int64_t *expires);
static int cookie_parse_date(const char *text, size_t length, int64_t *time);
static int cookie_date_delimiter(unsigned char c);
static int cookie_date_time(const char *token, size_t length, int *hour, int *minute, int *second);
static int cookie_date_digits(const char *token, size_t length, size_t least, size_t most, int *number);
static int cookie_date_month(const char *token, size_t length, int *month);
static int64_t cookie_days_from_civil(int64_t year, int month, int day);
static int cookie_days_in_month(int year, int month);
static void cookie_record(const struct cookie *cookie, struct wb_storage_cookie *record);
static void cookie_choose_key(struct net_cookie_jar *jar, struct cookie *cookie);
static int cookie_key_used(const struct net_cookie_jar *jar, const char *key);
static void cookie_save(struct net_cookie_jar *jar, const struct cookie *cookie);
static char *cookie_copy(const char *text, size_t length);
static void cookie_free(struct cookie *cookie);
static void cookie_remove(struct net_cookie_jar *jar, size_t index, int forget);
static int cookie_same(const struct cookie *a, const struct cookie *b);
static int cookie_domain_matches(const char *host, const char *domain);
static int cookie_path_matches(const char *request_path, const char *path);
static void cookie_default_path(const char *request_path, struct wb_buffer *out);
static int cookie_attribute(const char *text, size_t length, const char *name, const char **value, size_t *value_length);
static void cookie_lower(char *text);

/*
 * Opens a cookie jar over a storage queue, with the persistent cookies
 * the queue's backend keeps.
 *
 * queue: the instance's storage queue (a queue without a backend keeps
 *        nothing); borrowed until net_cookie_jar_close.
 * Records that do not decode, that have expired, that repeat a cookie
 * read before or that would pass the jar's limit are removed from the
 * storage.  A list or a read that fails leaves the jar with what was read
 * (an empty jar at worst).  Waits for the storage; the caller holds no
 * lock.  Returns 0, or ENOMEM (nothing is left open).
 */
int
net_cookie_jar_open(
	struct wb_storage_queue *queue,
	struct net_cookie_jar **jar)
{
	struct net_cookie_jar *made;
	int error;

	/* The jar, empty. */
	*jar = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Its lock and its storage. */
	pthread_mutex_init(&made->lock, NULL);
	made->queue = queue;

	/* The cookies its storage kept; only a lack of memory fails the opening. */
	error = cookie_load(made);
	if (error != 0) {
		net_cookie_jar_close(made);
		return error;
	}

	/* Succeeded: the jar has the persistent cookies that are still alive. */
	*jar = made;
	return 0;
}

/*
 * Closes a cookie jar: frees its cookies (their records stay in the
 * storage).  No other call on the jar may be in progress or follow.  NULL
 * does nothing.
 */
void
net_cookie_jar_close(
	struct net_cookie_jar *jar)
{
	size_t index;

	/* No jar. */
	if (jar == NULL)
		return;

	/* Each cookie's strings. */
	for (index = 0; index < jar->count; index++)
		cookie_free(&jar->cookies[index]);

	/* Then the lock and the jar. */
	pthread_mutex_destroy(&jar->lock);
	free(jar);
}

/*
 * Keeps the cookie a Set-Cookie header of a response to a URL sets (or
 * deletes, for one already expired).  A header that sets nothing, and a
 * NULL jar, keep nothing.  Returns 0 or ENOMEM.
 */
int
net_cookie_store(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	const char *header,
	size_t length)
{
	int error;

	/* Without a jar nothing is kept. */
	if (jar == NULL)
		return 0;

	/* A response may set any cookie. */
	pthread_mutex_lock(&jar->lock);

	error = cookie_store(jar, url, header, length, 0);

	pthread_mutex_unlock(&jar->lock);

	/* The jar could not take it. */
	if (error != 0)
		return error;

	/* Succeeded: the jar is up to date. */
	return 0;
}

/*
 * Keeps the cookie a script sets with document.cookie on a document of a
 * URL, in the form of a Set-Cookie header.  A cookie marked HttpOnly, and
 * one that would replace an HttpOnly cookie, is ignored, as is everything
 * with a NULL jar.  Returns 0 or ENOMEM.
 */
int
net_cookie_store_script(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	const char *text,
	size_t length)
{
	int error;

	/* Without a jar nothing is kept. */
	if (jar == NULL)
		return 0;

	/* A script may not touch the cookies only HTTP sees. */
	pthread_mutex_lock(&jar->lock);

	error = cookie_store(jar, url, text, length, 1);

	pthread_mutex_unlock(&jar->lock);

	/* The jar could not take it. */
	if (error != 0)
		return error;

	/* Succeeded: the jar is up to date. */
	return 0;
}

/*
 * Appends the Cookie header of a request to a URL ("Cookie: a=b; c=d"
 * and CR LF), when any cookie of the jar is for it (never with a NULL
 * jar).
 */
int
net_cookie_header(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	struct wb_buffer *out)
{
	struct wb_buffer pairs;
	int error;

	/* Without a jar there is no cookie to send. */
	if (jar == NULL)
		return 0;

	/* The pairs are gathered apart from the header. */
	wb_buffer_init(&pairs);

	/* The name and value pairs of every cookie for the URL. */
	pthread_mutex_lock(&jar->lock);

	error = cookie_pairs(jar, url, 0, &pairs);

	pthread_mutex_unlock(&jar->lock);

	/* The header, when there is a cookie. */
	if (error == 0 && pairs.length != 0)
		error = wb_buffer_printf(out, "Cookie: %s\r\n", wb_buffer_string(&pairs));
	wb_buffer_release(&pairs);
	if (error != 0)
		return error;

	/* Succeeded: the header is written (or there was none to write). */
	return 0;
}

/*
 * Appends what document.cookie reads on a document of a URL: the pairs of
 * the jar's cookies for it that are not HttpOnly ("a=b; c=d", or nothing;
 * nothing with a NULL jar).
 */
int
net_cookie_string(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	struct wb_buffer *out)
{
	int error;

	/* Without a jar a script sees no cookie. */
	if (jar == NULL)
		return 0;

	/* The pairs a script may see. */
	pthread_mutex_lock(&jar->lock);

	error = cookie_pairs(jar, url, 1, out);

	pthread_mutex_unlock(&jar->lock);

	/* The pairs could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the pairs are written. */
	return 0;
}

/* Reads every cookie record of the jar's storage into the jar, oldest first. */
static int
cookie_load(
	struct net_cookie_jar *jar)
{
	struct wb_buffer keys;
	const char *key;
	size_t count;
	size_t index;
	int64_t now;
	int error;

	/* The keys of the space; a list that fails leaves the jar empty. */
	wb_buffer_init(&keys);
	count = 0;
	error = wb_storage_queue_list(jar->queue, BROWSER_STORAGE_COOKIES, &keys, &count);
	if (error == ENOMEM) {
		wb_buffer_release(&keys);
		return ENOMEM;
	}
	if (error != 0) {
		wb_buffer_release(&keys);
		return 0;
	}

	/* Each record, until the memory runs out (each key ends with its NUL). */
	now = cookie_now();
	key = (const char *)keys.data;
	for (index = 0; index < count; index++) {
		error = cookie_load_one(jar, key, now);
		if (error != 0)
			break;
		key += strlen(key) + 1U;
	}
	wb_buffer_release(&keys);
	if (error != 0)
		return error;

	/* The jar's order is the order the cookies were set in. */
	cookie_sort(jar);

	/* Succeeded: the jar holds the records that are still alive. */
	return 0;
}

/*
 * Reads one cookie record into the jar; a record the jar does not keep is
 * removed from the storage.  Returns 0 or ENOMEM.
 */
static int
cookie_load_one(
	struct net_cookie_jar *jar,
	const char *key,
	int64_t now)
{
	struct wb_storage_cookie record;
	struct wb_buffer bytes;
	struct cookie cookie;
	const unsigned char *original;
	size_t original_length;
	size_t index;
	int found;
	int same;
	int error;

	/* The record's bytes. */
	wb_buffer_init(&bytes);
	found = 0;
	error = wb_storage_queue_read(jar->queue, BROWSER_STORAGE_COOKIES, key, &bytes, &found);
	if (error == ENOMEM) {
		wb_buffer_release(&bytes);
		return ENOMEM;
	}

	/* A record that could not be read is taken as absent and removed. */
	if (error != 0) {
		wb_buffer_release(&bytes);
		(void)wb_storage_queue_remove(jar->queue, BROWSER_STORAGE_COOKIES, key);
		return 0;
	}

	/* A record gone since the list. */
	if (!found) {
		wb_buffer_release(&bytes);
		return 0;
	}

	/* A record that does not decode, or whose cookie has expired, is removed. */
	error = wb_storage_cookie_decode(bytes.data, bytes.length, &record, &original, &original_length);
	if (error != 0 || record.expires <= now) {
		wb_buffer_release(&bytes);
		(void)wb_storage_queue_remove(jar->queue, BROWSER_STORAGE_COOKIES, key);
		return 0;
	}

	/* The cookie's own copy. */
	error = cookie_from_record(&record, key, &cookie);
	wb_buffer_release(&bytes);
	if (error != 0)
		return error;

	/* A second record of a cookie read before. */
	for (index = 0; index < jar->count; index++) {
		same = cookie_same(&jar->cookies[index], &cookie);
		if (same)
			break;
	}

	/* That one, and one past the jar's limit, are removed. */
	if (index < jar->count || jar->count == COOKIE_MAX) {
		cookie_free(&cookie);
		(void)wb_storage_queue_remove(jar->queue, BROWSER_STORAGE_COOKIES, key);
		return 0;
	}

	/* Succeeded: the jar has the cookie. */
	jar->cookies[jar->count] = cookie;
	jar->count++;
	return 0;
}

/* Makes a jar's cookie from a decoded record kept under a key. */
static int
cookie_from_record(
	const struct wb_storage_cookie *record,
	const char *key,
	struct cookie *cookie)
{
	/* The strings, each a copy of its own. */
	memset(cookie, 0, sizeof(*cookie));
	cookie->name = cookie_copy(record->name, record->name_length);
	cookie->value = cookie_copy(record->value, record->value_length);
	cookie->domain = cookie_copy(record->domain, record->domain_length);
	cookie->path = cookie_copy(record->path, record->path_length);
	if (cookie->name == NULL || cookie->value == NULL || cookie->domain == NULL || cookie->path == NULL) {
		cookie_free(cookie);
		return ENOMEM;
	}

	/* The flags. */
	if ((record->flags & WB_STORAGE_COOKIE_SECURE) != 0)
		cookie->secure = 1;
	if ((record->flags & WB_STORAGE_COOKIE_HTTP_ONLY) != 0)
		cookie->http_only = 1;
	if ((record->flags & WB_STORAGE_COOKIE_HOST_ONLY) != 0)
		cookie->host_only = 1;
	cookie->same_site = record->flags & WB_STORAGE_COOKIE_SAME_SITE;

	/* The times and the key. */
	cookie->expires = record->expires;
	cookie->created = record->created;
	cookie->used = record->used;
	strcpy(cookie->key, key);

	/* Succeeded: the cookie is made. */
	return 0;
}

/* Orders a jar's cookies by when they were set, the oldest first (an insertion sort keeps equal ones in place). */
static void
cookie_sort(
	struct net_cookie_jar *jar)
{
	struct cookie moving;
	size_t index;
	size_t place;

	/* Each cookie moves down past the later-set ones before it. */
	for (index = 1; index < jar->count; index++) {
		moving = jar->cookies[index];
		place = index;
		while (place > 0 && jar->cookies[place - 1U].created > moving.created) {
			jar->cookies[place] = jar->cookies[place - 1U];
			place--;
		}
		jar->cookies[place] = moving;
	}
}

/*
 * Keeps the cookie of a Set-Cookie header (or of document.cookie when
 * from_script is set, which leaves HttpOnly cookies alone).
 */
static int
cookie_store(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	const char *header,
	size_t length,
	int from_script)
{
	struct cookie cookie;
	struct cookie *kept;
	struct wb_buffer path;
	const char *pair_end;
	const char *equals;
	const char *attributes;
	const char *value;
	const char *next;
	size_t value_length;
	size_t name_length;
	size_t index;
	int64_t now;
	int64_t max_age_expires;
	int64_t date_expires;
	int has_max_age;
	int has_expires;
	int expired;
	int matches;
	int found;

	/* The name and value before the first semicolon (the name may be empty). */
	memset(&cookie, 0, sizeof(cookie));
	pair_end = memchr(header, ';', length);
	if (pair_end == NULL)
		pair_end = header + length;
	equals = memchr(header, '=', (size_t)(pair_end - header));
	if (equals == NULL)
		return 0;
	name_length = (size_t)(equals - header);
	while (name_length > 0 && header[name_length - 1U] == ' ')
		name_length--;
	value = equals + 1;
	while (value < pair_end && *value == ' ')
		value++;
	value_length = (size_t)(pair_end - value);
	while (value_length > 0 && value[value_length - 1U] == ' ')
		value_length--;
	cookie.name = cookie_copy(header, name_length);
	cookie.value = cookie_copy(value, value_length);

	/* The attributes after it. */
	now = cookie_now();
	has_max_age = 0;
	has_expires = 0;
	max_age_expires = 0;
	date_expires = 0;
	attributes = pair_end;
	while (attributes < header + length) {
		attributes++;
		while (attributes < header + length && *attributes == ' ')
			attributes++;
		next = memchr(attributes, ';', (size_t)(header + length - attributes));
		if (next == NULL)
			next = header + length;

		/* Domain: a leading dot dropped, in lower case. */
		found = cookie_attribute(attributes, (size_t)(next - attributes), "domain", &value, &value_length);
		if (found && value_length > 0) {
			if (*value == '.') {
				value++;
				value_length--;
			}

			/* A later Domain replaces an earlier one. */
			free(cookie.domain);
			cookie.domain = cookie_copy(value, value_length);
		}

		/* Path: only one that starts with a slash. */
		found = cookie_attribute(attributes, (size_t)(next - attributes), "path", &value, &value_length);
		if (found && value_length > 0 && *value == '/') {
			free(cookie.path);
			cookie.path = cookie_copy(value, value_length);
		}

		/* Max-Age: seconds from now (zero or less is already over); a later valid one replaces an earlier one. */
		found = cookie_attribute(attributes, (size_t)(next - attributes), "max-age", &value, &value_length);
		if (found) {
			found = cookie_max_age(value, value_length, now, &max_age_expires);
			if (found)
				has_max_age = 1;
		}

		/* Expires: a cookie date (RFC 6265 section 5.1.1); one that does not parse is ignored. */
		found = cookie_attribute(attributes, (size_t)(next - attributes), "expires", &value, &value_length);
		if (found) {
			found = cookie_parse_date(value, value_length, &date_expires);
			if (found)
				has_expires = 1;
		}

		/* The flags. */
		found = cookie_attribute(attributes, (size_t)(next - attributes), "secure", &value, &value_length);
		if (found)
			cookie.secure = 1;
		found = cookie_attribute(attributes, (size_t)(next - attributes), "httponly", &value, &value_length);
		if (found)
			cookie.http_only = 1;
		attributes = next;
	}

	/* The expiry: Max-Age before Expires (RFC 6265 section 5.3, step 3); neither makes a session cookie. */
	cookie.expires = COOKIE_SESSION;
	if (has_max_age) {
		cookie.expires = max_age_expires;
	} else if (has_expires) {
		cookie.expires = date_expires;
	}

	/* Whether it is over already, and its times (a replaced cookie's creation time is kept below). */
	expired = 0;
	if (cookie.expires != COOKIE_SESSION && cookie.expires <= now)
		expired = 1;
	cookie.created = now;
	cookie.used = now;
	cookie.same_site = WB_STORAGE_COOKIE_SAME_SITE_UNSET;

	/* No Domain: the request's host alone; a Domain must cover the host. */
	if (cookie.domain != NULL)
		cookie_lower(cookie.domain);
	if (cookie.domain == NULL && url->host != NULL) {
		cookie.domain = cookie_copy(url->host, strlen(url->host));
		cookie.host_only = 1;
	}

	/* A cookie that could not be copied, or whose domain misses the host, is not kept. */
	matches = 0;
	if (cookie.domain != NULL && cookie.name != NULL && cookie.value != NULL && url->host != NULL)
		matches = cookie_domain_matches(url->host, cookie.domain);
	if (!matches) {
		cookie_free(&cookie);
		return 0;
	}

	/* No Path: the directory of the request's path. */
	if (cookie.path == NULL) {
		wb_buffer_init(&path);
		cookie_default_path(url->path, &path);
		cookie.path = cookie_copy(wb_buffer_string(&path), path.length);
		wb_buffer_release(&path);
	}

	/* A path that could not be copied. */
	if (cookie.path == NULL) {
		cookie_free(&cookie);
		return ENOMEM;
	}

	/* A script cannot set a cookie only HTTP may see. */
	if (from_script && cookie.http_only) {
		cookie_free(&cookie);
		return 0;
	}

	/* A cookie of the same name, domain and path is replaced (or only removed when expired). */
	for (index = 0; index < jar->count; index++) {
		matches = cookie_same(&jar->cookies[index], &cookie);
		if (!matches)
			continue;

		/* A script cannot replace an HttpOnly cookie either. */
		if (from_script && jar->cookies[index].http_only) {
			cookie_free(&cookie);
			return 0;
		}

		/*
		 * The old cookie gives way, its creation time staying with the
		 * new one (RFC 6265 section 5.3, step 11.3).  A persistent new
		 * one takes over its record's key, so the write replaces the
		 * record; otherwise the record goes with it (a session cookie
		 * left in its place must not come back after a restart).
		 */
		cookie.created = jar->cookies[index].created;
		if (!expired && cookie.expires != COOKIE_SESSION && jar->cookies[index].key[0] != '\0') {
			strcpy(cookie.key, jar->cookies[index].key);
			cookie_remove(jar, index, 0);
		} else {
			cookie_remove(jar, index, 1);
		}
		break;
	}

	/* An expired cookie is only removed. */
	if (expired) {
		cookie_free(&cookie);
		return 0;
	}

	/* A full jar drops its oldest cookie. */
	if (jar->count == COOKIE_MAX)
		cookie_remove(jar, 0, 1);

	/* A persistent cookie without a record yet gets a key that no other record of the jar has. */
	if (cookie.expires != COOKIE_SESSION && cookie.key[0] == '\0')
		cookie_choose_key(jar, &cookie);

	/* The jar keeps the cookie. */
	jar->cookies[jar->count] = cookie;
	kept = &jar->cookies[jar->count];
	jar->count++;

	/* Its record is queued (a failure leaves it in memory only). */
	if (kept->key[0] != '\0')
		cookie_save(jar, kept);

	/* Succeeded: the jar keeps the cookie. */
	return 0;
}

/*
 * Appends the name and value pairs of the cookies for a URL, separated by
 * "; " (only the ones a script may see when for_script is set).  An
 * expired cookie is removed instead; each one written is marked used.
 */
static int
cookie_pairs(
	struct net_cookie_jar *jar,
	const struct net_url *url,
	int for_script,
	struct wb_buffer *out)
{
	struct cookie *cookie;
	const char *host;
	size_t index;
	int64_t now;
	int is_https;
	int written;
	int matches;
	int error;

	/* Each cookie whose domain, path and security fit the URL. */
	host = url->host;
	if (host == NULL)
		return 0;
	now = cookie_now();
	is_https = !strcmp(url->scheme, "https");
	written = 0;
	error = 0;
	index = 0;
	while (error == 0 && index < jar->count) {
		cookie = &jar->cookies[index];

		/* An expired cookie goes, the next one moving into its place. */
		if (cookie->expires != COOKIE_SESSION && cookie->expires <= now) {
			cookie_remove(jar, index, 1);
			continue;
		}

		/* The domain. */
		index++;
		if (cookie->host_only) {
			matches = !strcmp(host, cookie->domain);
		} else {
			matches = cookie_domain_matches(host, cookie->domain);
		}

		/* The path and the security must fit too. */
		if (!matches)
			continue;
		matches = cookie_path_matches(url->path, cookie->path);
		if (!matches || (cookie->secure && !is_https))
			continue;

		/* A script does not see an HttpOnly cookie. */
		if (for_script && cookie->http_only)
			continue;

		/* "; " between them. */
		if (written)
			error = wb_buffer_append_string(out, "; ");

		/* The name (when it has one) and the value; the use is kept in memory, not written to the storage. */
		written = 1;
		cookie->used = now;
		if (error == 0 && cookie->name[0] != '\0')
			error = wb_buffer_printf(out, "%s=", cookie->name);
		if (error == 0)
			error = wb_buffer_append_string(out, cookie->value);
	}

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the pairs are written (or there was none). */
	return 0;
}

/* Reports the wall clock in milliseconds since 1970-01-01 00:00 UTC, the unit of the cookies' times. */
static int64_t
cookie_now(void)
{
	int64_t nanoseconds;

	/* The system's wall clock. */
	nanoseconds = wb_os_wall_ns();

	/* Succeeded: the time in milliseconds. */
	return nanoseconds / INT64_C(1000000);
}

/*
 * Reads a Max-Age value (RFC 6265 section 5.2.2): an optional '-' and
 * digits.  Reports 1 with the expiry it gives (now when it is zero or
 * less, so the cookie is over), or 0 for a value to ignore.
 */
static int
cookie_max_age(
	const char *value,
	size_t length,
	int64_t now,
	int64_t *expires)
{
	int64_t seconds;
	size_t index;
	int negative;

	/* An optional minus sign, then at least one digit. */
	index = 0;
	negative = 0;
	if (length > 0 && value[0] == '-') {
		negative = 1;
		index = 1;
	}
	if (index == length)
		return 0;

	/* The digits, saturating at the latest expiry kept. */
	seconds = 0;
	for (; index < length; index++) {
		if (value[index] < '0' || value[index] > '9')
			return 0;
		if (seconds < COOKIE_EXPIRY_MAX / COOKIE_SECOND)
			seconds = seconds * 10 + (value[index] - '0');
	}

	/* Zero or less ends the cookie now. */
	if (negative || seconds == 0) {
		*expires = now;
		return 1;
	}

	/* Succeeded: the expiry, no later than the latest kept. */
	*expires = COOKIE_EXPIRY_MAX;
	if (seconds < (COOKIE_EXPIRY_MAX - now) / COOKIE_SECOND)
		*expires = now + seconds * COOKIE_SECOND;
	return 1;
}

/*
 * Parses a cookie date (RFC 6265 section 5.1.1): the first time, day of
 * the month, month and year among its tokens, a year of two digits read
 * as 1970 to 2069.  Reports 1 with the time in milliseconds since 1970 UTC
 * (no later than COOKIE_EXPIRY_MAX), or 0 when it is not a date.
 */
static int
cookie_parse_date(
	const char *text,
	size_t length,
	int64_t *time)
{
	const char *token;
	size_t token_length;
	size_t index;
	int64_t days;
	int found_time;
	int found_day;
	int found_month;
	int found_year;
	int hour;
	int minute;
	int second;
	int day;
	int month;
	int year;
	int found;
	int delimiter;
	int days_in_month;

	/* Each token between delimiters, tried as the parts not found yet in the algorithm's order. */
	found_time = 0;
	found_day = 0;
	found_month = 0;
	found_year = 0;
	hour = 0;
	minute = 0;
	second = 0;
	day = 0;
	month = 0;
	year = 0;
	index = 0;
	while (index < length) {
		/* The delimiters before the token. */
		delimiter = cookie_date_delimiter((unsigned char)text[index]);
		if (delimiter) {
			index++;
			continue;
		}

		/* The token, up to the next delimiter. */
		token = text + index;
		token_length = 0;
		while (index < length) {
			delimiter = cookie_date_delimiter((unsigned char)text[index]);
			if (delimiter)
				break;
			index++;
			token_length++;
		}

		/* The first token of each kind counts: the time, the day of the month, the month, the year. */
		if (!found_time) {
			found = cookie_date_time(token, token_length, &hour, &minute, &second);
			if (found) {
				found_time = 1;
				continue;
			}
		}
		if (!found_day) {
			found = cookie_date_digits(token, token_length, 1, 2, &day);
			if (found) {
				found_day = 1;
				continue;
			}
		}
		if (!found_month) {
			found = cookie_date_month(token, token_length, &month);
			if (found) {
				found_month = 1;
				continue;
			}
		}
		if (!found_year) {
			found = cookie_date_digits(token, token_length, 2, 4, &year);
			if (found)
				found_year = 1;
		}
	}

	/* A date without all four parts is not one. */
	if (!found_time || !found_day || !found_month || !found_year)
		return 0;

	/* Two-digit years: 70 to 99 are 1970 to 1999, 0 to 69 are 2000 to 2069. */
	if (year >= 70 && year <= 99) {
		year += 1900;
	} else if (year >= 0 && year <= 69) {
		year += 2000;
	}

	/* The parts must be in range, and the day must exist in its month. */
	if (day < 1 || day > 31 || year < 1601 || hour > 23 || minute > 59 || second > 59)
		return 0;
	days_in_month = cookie_days_in_month(year, month);
	if (day > days_in_month)
		return 0;

	/* Succeeded: the time, no later than the latest kept. */
	days = cookie_days_from_civil(year, month, day);
	*time = days * COOKIE_DAY + ((int64_t)hour * 3600 + (int64_t)minute * 60 + second) * COOKIE_SECOND;
	if (*time > COOKIE_EXPIRY_MAX)
		*time = COOKIE_EXPIRY_MAX;
	return 1;
}

/* Tells whether a byte is a delimiter of a cookie date (%x09, %x20-2F, %x3B-40, %x5B-60, %x7B-7E). */
static int
cookie_date_delimiter(
	unsigned char c)
{
	/* The tab and the four ranges of ASCII. */
	if (c == 0x09)
		return 1;
	if (c >= 0x20 && c <= 0x2f)
		return 1;
	if (c >= 0x3b && c <= 0x40)
		return 1;
	if (c >= 0x5b && c <= 0x60)
		return 1;
	if (c >= 0x7b && c <= 0x7e)
		return 1;

	/* Anything else is part of a token. */
	return 0;
}

/*
 * Reads a time token: 1*2DIGIT ":" 1*2DIGIT ":" 1*2DIGIT, then nothing or
 * a non-digit and anything.  Reports 1 with its parts, or 0.
 */
static int
cookie_date_time(
	const char *token,
	size_t length,
	int *hour,
	int *minute,
	int *second)
{
	int parts[3];
	size_t index;
	size_t digits;
	int part;

	/* The three numbers, a colon after the first two. */
	index = 0;
	for (part = 0; part < 3; part++) {
		parts[part] = 0;
		digits = 0;
		while (index < length && digits < 2U && token[index] >= '0' && token[index] <= '9') {
			parts[part] = parts[part] * 10 + (token[index] - '0');
			index++;
			digits++;
		}

		/* A part without a digit. */
		if (digits == 0)
			return 0;

		/* The colon after the hour and the minute. */
		if (part < 2) {
			if (index == length || token[index] != ':')
				return 0;
			index++;
		}
	}

	/* What follows must not be a third digit. */
	if (index < length && token[index] >= '0' && token[index] <= '9')
		return 0;

	/* Succeeded: the time's parts. */
	*hour = parts[0];
	*minute = parts[1];
	*second = parts[2];
	return 1;
}

/*
 * Reads a token of least to most digits, then nothing or a non-digit and
 * anything (the day of the month, the year).  Reports 1 with the number,
 * or 0.
 */
static int
cookie_date_digits(
	const char *token,
	size_t length,
	size_t least,
	size_t most,
	int *number)
{
	size_t index;
	int value;

	/* The digits at the start. */
	value = 0;
	index = 0;
	while (index < length && index < most && token[index] >= '0' && token[index] <= '9') {
		value = value * 10 + (token[index] - '0');
		index++;
	}

	/* Too few, or one more digit than allowed. */
	if (index < least)
		return 0;
	if (index < length && token[index] >= '0' && token[index] <= '9')
		return 0;

	/* Succeeded: the number. */
	*number = value;
	return 1;
}

/* Reads a month token by its first three letters in any case; reports 1 with the month (1 to 12), or 0. */
static int
cookie_date_month(
	const char *token,
	size_t length,
	int *month)
{
	char letters[4];
	size_t index;
	int differs;
	int c;

	/* The first three letters in lower case. */
	if (length < 3U)
		return 0;
	for (index = 0; index < 3U; index++) {
		c = (unsigned char)token[index];
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		letters[index] = (char)c;
	}
	letters[3] = '\0';

	/* The month whose name starts with them. */
	for (index = 0; index < 12U; index++) {
		differs = strcmp(letters, cookie_months[index]);
		if (differs == 0) {
			*month = (int)index + 1;
			return 1;
		}
	}

	/* No month. */
	return 0;
}

/*
 * Reports the days from 1970-01-01 to a date of the proleptic Gregorian
 * calendar (the "days from civil" algorithm of Howard Hinnant's chrono
 * date algorithms, which he placed in the public domain).
 */
static int64_t
cookie_days_from_civil(
	int64_t year,
	int month,
	int day)
{
	int64_t era;
	int64_t year_of_era;
	int64_t day_of_year;
	int64_t day_of_era;

	/* The year counted from March, so that the leap day is the last of it, and its era of 400 years. */
	if (month <= 2)
		year--;
	era = year / 400;
	if (year < 0)
		era = (year - 399) / 400;
	year_of_era = year - era * 400;

	/* The day of that year (from March) and of the era. */
	if (month > 2) {
		day_of_year = (153 * (month - 3) + 2) / 5 + day - 1;
	} else {
		day_of_year = (153 * (month + 9) + 2) / 5 + day - 1;
	}
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;

	/* Succeeded: the days since 1970-01-01, which is 719468 days after 0000-03-01. */
	return era * 146097 + day_of_era - 719468;
}

/* Reports the number of days in a month (1 to 12) of a Gregorian year. */
static int
cookie_days_in_month(
	int year,
	int month)
{
	int leap;

	/* February of a leap year has one more. */
	leap = 0;
	if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
		leap = 1;
	if (month == 2 && leap)
		return 29;

	/* Succeeded: the month's days. */
	return cookie_month_days[month - 1];
}

/* Fills the record of a cookie (its strings borrowed). */
static void
cookie_record(
	const struct cookie *cookie,
	struct wb_storage_cookie *record)
{
	/* The strings. */
	memset(record, 0, sizeof(*record));
	record->name = cookie->name;
	record->name_length = strlen(cookie->name);
	record->value = cookie->value;
	record->value_length = strlen(cookie->value);
	record->domain = cookie->domain;
	record->domain_length = strlen(cookie->domain);
	record->path = cookie->path;
	record->path_length = strlen(cookie->path);

	/* The times. */
	record->expires = cookie->expires;
	record->created = cookie->created;
	record->used = cookie->used;

	/* The flags. */
	record->flags = cookie->same_site;
	if (cookie->secure)
		record->flags |= WB_STORAGE_COOKIE_SECURE;
	if (cookie->http_only)
		record->flags |= WB_STORAGE_COOKIE_HTTP_ONLY;
	if (cookie->host_only)
		record->flags |= WB_STORAGE_COOKIE_HOST_ONLY;
}

/*
 * Gives a persistent cookie the key of its record: the first of its keys
 * (by collision number) that no other cookie of the jar holds.  A cookie
 * for which none is left, or whose key cannot be made, keeps no record
 * ("" key) and lives in memory only.
 */
static void
cookie_choose_key(
	struct net_cookie_jar *jar,
	struct cookie *cookie)
{
	struct wb_storage_cookie record;
	struct wb_buffer original;
	unsigned collision;
	int used;
	int error;

	/* The original key: the domain, the path and the name. */
	cookie->key[0] = '\0';
	cookie_record(cookie, &record);
	wb_buffer_init(&original);
	error = wb_storage_cookie_original_key(&record, &original);
	if (error != 0) {
		wb_buffer_release(&original);
		return;
	}

	/* Each collision number until a key is free. */
	for (collision = 0; collision < COOKIE_COLLISIONS; collision++) {
		error = wb_storage_key_text(original.data, original.length, collision, cookie->key);
		if (error != 0)
			break;
		used = cookie_key_used(jar, cookie->key);
		if (!used) {
			wb_buffer_release(&original);
			return;
		}
	}

	/* No key: the cookie is not saved. */
	wb_buffer_release(&original);
	cookie->key[0] = '\0';
}

/* Tells whether a cookie of the jar holds a record's key. */
static int
cookie_key_used(
	const struct net_cookie_jar *jar,
	const char *key)
{
	size_t index;
	int differs;

	/* Each cookie's key. */
	for (index = 0; index < jar->count; index++) {
		differs = strcmp(jar->cookies[index].key, key);
		if (differs == 0)
			return 1;
	}

	/* No cookie holds it. */
	return 0;
}

/*
 * Queues the write of a cookie's record under its key.  A record that
 * cannot be made or queued leaves the cookie in memory (it is written
 * again when it is set again); the queue keeps a backend's error for the
 * view to tell.
 */
static void
cookie_save(
	struct net_cookie_jar *jar,
	const struct cookie *cookie)
{
	struct wb_storage_cookie record;
	struct wb_buffer bytes;
	int error;

	/* The record's bytes. */
	cookie_record(cookie, &record);
	wb_buffer_init(&bytes);
	error = wb_storage_cookie_encode(&record, &bytes);

	/* Queued for the storage thread, which writes it behind. */
	if (error == 0)
		(void)wb_storage_queue_write(jar->queue, BROWSER_STORAGE_COOKIES, cookie->key, bytes.data, bytes.length);
	wb_buffer_release(&bytes);
}

/* Copies a text into a new string. */
static char *
cookie_copy(
	const char *text,
	size_t length)
{
	char *copy;

	/* The bytes and a terminator. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return NULL;
	if (length > 0)
		memcpy(copy, text, length);
	copy[length] = '\0';

	/* The copy. */
	return copy;
}

/* Frees a cookie's strings. */
static void
cookie_free(
	struct cookie *cookie)
{
	/* The four strings. */
	free(cookie->name);
	free(cookie->value);
	free(cookie->domain);
	free(cookie->path);
	memset(cookie, 0, sizeof(*cookie));
}

/*
 * Removes the jar's cookie at an index, the later ones moving down; with
 * forget, its record (when it has one) is removed from the storage too.
 */
static void
cookie_remove(
	struct net_cookie_jar *jar,
	size_t index,
	int forget)
{
	/* The record, queued for removal. */
	if (forget && jar->cookies[index].key[0] != '\0')
		(void)wb_storage_queue_remove(jar->queue, BROWSER_STORAGE_COOKIES, jar->cookies[index].key);

	/* The cookie's strings, then the gap closed. */
	cookie_free(&jar->cookies[index]);
	memmove(&jar->cookies[index], &jar->cookies[index + 1U], (jar->count - index - 1U) * sizeof(jar->cookies[0]));
	jar->count--;
}

/* Tells whether two cookies have the same name, domain and path (one replaces the other). */
static int
cookie_same(
	const struct cookie *a,
	const struct cookie *b)
{
	int differs;

	/* The name, the domain, then the path. */
	differs = strcmp(a->name, b->name);
	if (differs != 0)
		return 0;
	differs = strcmp(a->domain, b->domain);
	if (differs != 0)
		return 0;
	differs = strcmp(a->path, b->path);
	if (differs != 0)
		return 0;

	/* The same cookie. */
	return 1;
}

/* Tells whether a host domain-matches a domain: the same, or a subdomain of it (not an IP address). */
static int
cookie_domain_matches(
	const char *host,
	const char *domain)
{
	size_t host_length;
	size_t domain_length;
	int differs;

	/* The same string. */
	differs = strcmp(host, domain);
	if (differs == 0)
		return 1;

	/* A host that ends with a dot and the domain. */
	host_length = strlen(host);
	domain_length = strlen(domain);
	if (host_length <= domain_length || domain_length == 0)
		return 0;
	differs = strcmp(host + host_length - domain_length, domain);
	if (differs != 0)
		return 0;
	if (host[host_length - domain_length - 1U] != '.')
		return 0;

	/* An IP address has no subdomains. */
	if (host[0] == '[' || (host[host_length - 1U] >= '0' && host[host_length - 1U] <= '9'))
		return 0;
	return 1;
}

/* Tells whether a request path path-matches a cookie's path. */
static int
cookie_path_matches(
	const char *request_path,
	const char *path)
{
	size_t length;
	int differs;

	/* The same path. */
	if (request_path[0] == '\0')
		request_path = "/";
	differs = strcmp(request_path, path);
	if (differs == 0)
		return 1;

	/* A prefix that ends in a slash, or is followed by one. */
	length = strlen(path);
	differs = strncmp(request_path, path, length);
	if (differs != 0)
		return 0;
	if (length > 0 && path[length - 1U] == '/')
		return 1;
	if (request_path[length] == '/')
		return 1;
	return 0;
}

/* Writes the default path of a request path: its directory (up to the last slash), or "/". */
static void
cookie_default_path(
	const char *request_path,
	struct wb_buffer *out)
{
	const char *last;

	/* A path not starting with a slash, or with only the first slash, is "/". */
	last = strrchr(request_path, '/');
	if (request_path[0] != '/' || last == request_path) {
		wb_buffer_append_string(out, "/");
		return;
	}

	/* Up to the last slash. */
	wb_buffer_append(out, request_path, (size_t)(last - request_path));
}

/* Finds an attribute (name, and an optional =value) in one attribute's text; nonzero when it is that one. */
static int
cookie_attribute(
	const char *text,
	size_t length,
	const char *name,
	const char **value,
	size_t *value_length)
{
	size_t name_length;
	size_t index;
	size_t end;
	int c;

	/* The name in any case, then the end or =. */
	name_length = strlen(name);
	if (length < name_length)
		return 0;
	for (index = 0; index < name_length; index++) {
		c = (unsigned char)text[index];
		if (c >= 'A' && c <= 'Z')
			c += 0x20;
		if (c != (unsigned char)name[index])
			return 0;
	}

	/* Spaces, then the end or "=". */
	index = name_length;
	while (index < length && text[index] == ' ')
		index++;
	if (index < length && text[index] != '=')
		return 0;

	/* The value after =, without surrounding spaces. */
	*value = text + length;
	*value_length = 0;
	if (index < length) {
		index++;
		while (index < length && text[index] == ' ')
			index++;
		end = length;
		while (end > index && text[end - 1U] == ' ')
			end--;
		*value = text + index;
		*value_length = end - index;
	}

	/* The attribute. */
	return 1;
}

/* Folds a string's ASCII letters to lower case. */
static void
cookie_lower(
	char *text)
{
	/* Each byte. */
	for (; *text != '\0'; text++) {
		if (*text >= 'A' && *text <= 'Z')
			*text = (char)(*text + 0x20);
	}
}
