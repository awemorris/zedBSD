/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The records the engine keeps in a storage backend
 * (<browser/storage.h>): how a record's key is made, and the bytes of a
 * cookie, an HTTP cache entry and a localStorage area.
 *
 * Everything here is pure: no system call, no thread, no global state.
 * Encoders append to a struct wb_buffer; decoders read a record that may
 * have been damaged on the disk or written by anything (a backend is not
 * trusted), check every length against the bytes that remain, and either
 * decode all of it or fail with EINVAL, never reading outside it.
 *
 * All integers in records are little-endian (as the localStorage files of
 * earlier engines).  A record starts with a magic line naming its kind and
 * version and holds the original key it was stored under, so a reader
 * that finds another record under the same key (a hash collision) sees
 * that it is not the one asked for.
 */

#ifndef KEILAND_BROWSER_STORAGE_RECORDS_H
#define KEILAND_BROWSER_STORAGE_RECORDS_H

#include "base/base.h"

#include <browser/storage.h>

#include <stddef.h>
#include <stdint.h>

/* The largest record, in bytes; a sink refuses more (EFBIG).  64 MiB. */
#define WB_STORAGE_RECORD_MAX		(64UL * 1024UL * 1024UL)

/*
 * The first bytes of each record kind, a line of ASCII ending in '\n'.
 * LOCAL 1 is the format of earlier engines (no origin inside, read only);
 * LOCAL 2 is what the engine writes.
 */
#define WB_STORAGE_MAGIC_COOKIE		"KEILAND COOKIE 1\n"
#define WB_STORAGE_MAGIC_CACHE		"KEILAND CACHE 1\n"
#define WB_STORAGE_MAGIC_LOCAL_1	"KEILAND LOCAL STORAGE 1\n"
#define WB_STORAGE_MAGIC_LOCAL_2	"KEILAND LOCAL STORAGE 2\n"

/* The flags of a stored cookie (struct wb_storage_cookie). */
#define WB_STORAGE_COOKIE_SECURE	0x01U	/* sent over https only */
#define WB_STORAGE_COOKIE_HTTP_ONLY	0x02U	/* hidden from document.cookie */
#define WB_STORAGE_COOKIE_HOST_ONLY	0x04U	/* no Domain attribute: this host only */
#define WB_STORAGE_COOKIE_SAME_SITE	0x18U	/* the SameSite bits: */
#define WB_STORAGE_COOKIE_SAME_SITE_UNSET	0x00U
#define WB_STORAGE_COOKIE_SAME_SITE_LAX		0x08U
#define WB_STORAGE_COOKIE_SAME_SITE_STRICT	0x10U
#define WB_STORAGE_COOKIE_SAME_SITE_NONE	0x18U
#define WB_STORAGE_COOKIE_FLAGS		0x1fU	/* every defined bit; a record with others is EINVAL */

/*
 * The engine's sink for a backend's read: the buffer the record's bytes go
 * to, and the first error of an append (0 while none).  The reader owns it
 * on its stack; wb_storage_sink_init binds it to a buffer the reader owns.
 */
struct browser_storage_sink {
	struct wb_buffer *buffer;
	size_t start;		/* the buffer's length when bound; the record is what follows */
	int error;
};

/*
 * A cookie as stored.  The strings are counted (not NUL-terminated) and
 * borrowed: from the caller when encoding, from the record's bytes when
 * decoding (valid while those bytes are).  Times are milliseconds since
 * 1970-01-01 00:00 UTC.
 */
struct wb_storage_cookie {
	const char *name;
	size_t name_length;
	const char *value;
	size_t value_length;
	const char *domain;	/* lower case, no leading dot */
	size_t domain_length;
	const char *path;
	size_t path_length;
	int64_t expires;	/* when it ends; persistent cookies only, so always set */
	int64_t created;	/* when it was first set (RFC 6265 creation-time) */
	int64_t used;		/* when it was last sent or read (last-access-time) */
	unsigned flags;		/* WB_STORAGE_COOKIE_* */
};

/*
 * An HTTP cache entry as stored: a 200 response to a plain GET, with what
 * the engine's memory cache keeps of it.  Strings and bytes are counted
 * and borrowed as in struct wb_storage_cookie.
 */
struct wb_storage_cache_entry {
	const char *url;
	size_t url_length;
	const char *content_type;	/* may be empty */
	size_t content_type_length;
	const char *etag;		/* may be empty: then max_age decides alone */
	size_t etag_length;
	int64_t stored;			/* when stored or last revalidated, ms since 1970 UTC */
	int64_t max_age;		/* seconds it stays fresh, or -1 for none */
	int no_cache;			/* 1: revalidate before every use; 0 otherwise */
	const unsigned char *body;
	size_t body_length;
};

/*
 * One item of a localStorage area: its key and value as UTF-16 units
 * (any 16-bit values; unpaired surrogates are kept), borrowed.
 */
struct wb_storage_local_item {
	const uint16_t *key;
	size_t key_length;
	const uint16_t *value;
	size_t value_length;
};

/*
 * Called once per item while decoding a localStorage area, in the order
 * the record holds them.  The units are valid only during the call.
 * Returns 0 to go on, or an errno value that stops the decoding and is
 * returned by it.
 */
typedef int (*wb_storage_local_each)(void *context, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);

/*
 * Binds a sink to a buffer for one read: what the backend appends goes
 * after the buffer's current length.
 */
void wb_storage_sink_init(struct browser_storage_sink *sink, struct wb_buffer *buffer);

/*
 * Reports a sink's state after the backend's read returned: the sink's
 * first error, or 0.  The record is buffer->data + sink->start, its length
 * buffer->length - sink->start.
 */
int wb_storage_sink_error(const struct browser_storage_sink *sink);

/*
 * The 64-bit FNV-1a hash of bytes: offset basis 14695981039346656037
 * (0xcbf29ce484222325), prime 1099511628211 (0x100000001b3), each byte
 * XORed in and the result multiplied by the prime modulo 2^64.
 * bytes may be NULL when length is 0 (the hash is then the basis).
 * The values are those of the FNV specification (draft-eastlake-fnv,
 * section 2); the hash is not a security measure, only a short name.
 */
uint64_t wb_storage_hash(const void *bytes, size_t length);

/*
 * Appends a cookie's original key: its domain, path and name, each as a
 * 32-bit little-endian length followed by its bytes (so a cookie with an
 * empty name never equals another's key).
 *
 * Returns 0, EINVAL (cookie or out NULL, a string NULL with a length
 * above 0, a length above UINT32_MAX), ENOMEM.
 */
int wb_storage_cookie_original_key(const struct wb_storage_cookie *cookie, struct wb_buffer *out);

/*
 * Writes the key text a record is stored under.
 *
 * original:  the original key's bytes: for a cookie the bytes of
 *            wb_storage_cookie_original_key, for a cache entry the URL's
 *            bytes, for a localStorage area the origin's bytes.  Not NULL;
 *            length more than 0.
 * collision: 0 for the first try; the next number when the key was found
 *            holding another original key.
 * text:      room for BROWSER_STORAGE_KEY_MAX + 1 bytes; receives the key,
 *            NUL-terminated.
 * The key is the original's bytes in lower-case hexadecimal (two digits
 * per byte, the first byte first) when that is at most
 * BROWSER_STORAGE_KEY_MAX characters and collision is 0 (so a localStorage
 * area of a short origin keeps the file name of earlier engines).
 * Otherwise it is the 16 lower-case hexadecimal digits of
 * wb_storage_hash(original), '-', and collision in decimal without leading
 * zeros.
 * Returns 0, EINVAL (an argument NULL, length 0).  A collision above 0 with
 * a short original gives the hash form too.
 */
int wb_storage_key_text(const void *original, size_t length, unsigned collision, char *text);

/*
 * Reports whether text is a valid key: 1 to BROWSER_STORAGE_KEY_MAX
 * characters, each of '0'-'9', 'a'-'f' or '-'.  Returns 1 or 0 (0 for NULL).
 * A backend's list hands only such keys to the engine; others are skipped.
 */
int wb_storage_key_valid(const char *text);

/*
 * Appends a cookie record:
 *   WB_STORAGE_MAGIC_COOKIE,
 *   u32 length and bytes of the original key (wb_storage_cookie_original_key),
 *   u32 length and bytes of name, value, domain, path (in that order),
 *   i64 expires, i64 created, i64 used (two's complement, little-endian),
 *   u32 flags.
 * Returns 0, EINVAL (NULL arguments, flags outside WB_STORAGE_COOKIE_FLAGS,
 * a length above UINT32_MAX), ENOMEM.  On failure out's length is unchanged.
 */
int wb_storage_cookie_encode(const struct wb_storage_cookie *cookie, struct wb_buffer *out);

/*
 * Decodes a cookie record.
 *
 * bytes, length: the record (NULL only when length is 0).
 * cookie:        receives the fields, pointing into bytes.
 * original, original_length: receive where the original key lies in bytes.
 * Returns 0, EINVAL (wrong magic, a length running past the end, bytes left
 * over at the end, an original key that does not equal the one rebuilt
 * from the decoded domain, path and name, unknown flags).  The times are
 * not checked against each other.  On failure *cookie is zeroed and
 * *original is NULL and *original_length 0.
 */
int wb_storage_cookie_decode(const void *bytes, size_t length, struct wb_storage_cookie *cookie, const unsigned char **original, size_t *original_length);

/*
 * Appends a cache record:
 *   WB_STORAGE_MAGIC_CACHE,
 *   u32 length and bytes of url (the original key),
 *   u32 length and bytes of content_type, then of etag,
 *   i64 stored, i64 max_age, u32 no_cache (0 or 1),
 *   u32 length and bytes of body.
 * Returns 0, EINVAL (NULL arguments, url empty, max_age below -1, no_cache
 * not 0 or 1, a length above UINT32_MAX), ENOMEM.  On failure out's length
 * is unchanged.
 */
int wb_storage_cache_encode(const struct wb_storage_cache_entry *entry, struct wb_buffer *out);

/*
 * Decodes a cache record into entry (pointing into bytes); the original key
 * is entry->url.  Returns 0 or EINVAL (wrong magic, lengths past the end,
 * bytes left over, url empty, max_age below -1, no_cache not 0 or 1).  On
 * failure *entry is zeroed.
 */
int wb_storage_cache_decode(const void *bytes, size_t length, struct wb_storage_cache_entry *entry);

/*
 * Appends a localStorage record of version 2:
 *   WB_STORAGE_MAGIC_LOCAL_2,
 *   u32 length and bytes of the origin (the original key),
 *   then for each item: u32 key length and u32 value length, both in
 *   UTF-16 units, then the key's units and the value's units, each unit
 *   u16 little-endian (the item layout of version 1).
 * items may be NULL when count is 0 (an empty area).
 * Returns 0, EINVAL (origin or out NULL, origin_length 0, items NULL with
 * count above 0, a length above UINT32_MAX), ENOMEM.  On failure out's
 * length is unchanged.
 */
int wb_storage_local_encode(const char *origin, size_t origin_length, const struct wb_storage_local_item *items, size_t count, struct wb_buffer *out);

/*
 * Decodes a localStorage record of version 1 or 2, calling each per item.
 *
 * origin, origin_length: receive where the origin lies in bytes for
 *                version 2; NULL and 0 for version 1 (which does not hold
 *                it: its file name did).
 * Returns 0, EINVAL (neither magic, a length past the end; an item cut
 * short at the end is EINVAL too, even though earlier engines kept the
 * items before it), ENOMEM, or the value each returned.  Items already
 * passed to each before an error stay passed; the caller discards them.
 */
int wb_storage_local_decode(const void *bytes, size_t length, const unsigned char **origin, size_t *origin_length, wb_storage_local_each each, void *context);

#endif
