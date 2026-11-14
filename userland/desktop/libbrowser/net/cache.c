/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The persistent HTTP cache: an instance's index of the responses its
 * storage keeps (BROWSER_STORAGE_CACHE, one record per response,
 * storage/records.h), and the reads and writes of those records.
 *
 * The loaders (net/loader.c) keep a memory cache of their own and use
 * this one as its second level: a response they store goes here too, and
 * a URL their memory cache lacks is looked for here.  Every loader of an
 * instance shares the one cache, each from its own network thread.
 *
 * The index maps a record's key to its size and its times.  It is kept in
 * memory and as one record of the same space, under the key "0" (a key
 * the records' own keys never take: a hexadecimal key has an even number
 * of digits, a hashed one a '-'), and opening the cache reads that record
 * alone, never a response.  The index record is
 *   INDEX_MAGIC,
 *   then for each response: u16 length and bytes of its key, u64 size of
 *   its record, i64 stored, i64 used (little-endian, milliseconds since
 *   1970 UTC for the times).
 * It is queued again each time a response is added or removed; a use
 * only moves the response's last-use time in memory, and the record is
 * written again at most once in INDEX_USE_INTERVAL (and when the cache is
 * closed).  An index that does not decode is dropped as an empty one.  A
 * response record the index does not list (the index was not written
 * before the program ended) is removed when the cache is opened.
 *
 * A response's key is the key of its URL with collision number 0.  A
 * record is checked against the URL it is asked for when it is read, so
 * two URLs that share a key (possible only for a URL of more than 100
 * bytes, whose key is a 64-bit hash) cannot give each other's response:
 * the later one replaces the earlier one, which is only a cache miss.
 *
 * The total of the records' sizes is kept under a limit: when a response
 * is added, the least recently used ones are removed until it fits.  The
 * number of responses is limited too, so that the index stays small
 * enough to write again at each change.
 *
 * Locking: the cache's lock covers the index; the storage queue's lock is
 * taken inside it, and only by the calls that do not wait (write and
 * remove).  A read of a record waits for the disk, so it holds no lock.
 * Writing a response (net_cache_store) never waits, which lets the
 * network thread call it.
 */

#include "net/net.h"
#include "base/thread.h"
#include "base/os-time.h"
#include "storage/queue.h"
#include "storage/records.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The key of the index record; no response's key can equal it. */
#define CACHE_INDEX_KEY		"0"

/* The first bytes of the index record, a line of ASCII ending in '\n'. */
#define CACHE_INDEX_MAGIC	"KEILAND CACHE INDEX 1\n"

/* The most responses the index lists (the least recently used go first). */
#define CACHE_ENTRY_MAX		16384U

/* The most often the index is written again for a use alone, in milliseconds. */
#define CACHE_INDEX_INTERVAL	INT64_C(60000)

/*
 * One response of the index: its record's key, the size of its record
 * and when it was stored (or last revalidated) and last used, in
 * milliseconds since 1970 UTC.
 */
struct cache_entry {
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	uint64_t bytes;
	int64_t stored;
	int64_t used;
};

/*
 * An instance's persistent HTTP cache: the index of its responses, the
 * sum of their sizes against the limit, and the storage queue their
 * records go to.  It lives as long as its instance; the instance's views
 * (and their loaders) are gone before it is closed.
 *
 * entries is the index (count of capacity items), under lock.  dirty
 * says the index record lacks a change (a use) that has not been queued;
 * written is when the record was last queued.
 */
struct net_cache {
	struct wb_mutex lock;
	struct wb_storage_queue *queue;
	struct cache_entry *entries;
	size_t count;
	size_t capacity;
	uint64_t total;
	uint64_t limit;
	int dirty;
	int64_t written;
};

static int cache_load(struct net_cache *cache);
static int cache_sweep(struct net_cache *cache);
static int cache_compare_keys(const void *left, const void *right);
static int cache_decode_index(struct net_cache *cache, const unsigned char *bytes, size_t length);
static int cache_encode_index(const struct net_cache *cache, struct wb_buffer *out);
static void cache_write_index(struct net_cache *cache);
static int cache_push(struct net_cache *cache, const char *key, uint64_t bytes, int64_t stored, int64_t used);
static size_t cache_find(const struct net_cache *cache, const char *key);
static void cache_drop(struct net_cache *cache, size_t index, int remove_record);
static void cache_evict(struct net_cache *cache, const char *keep, uint64_t room, size_t slots);
static int64_t cache_now(void);
static void cache_put16(struct wb_buffer *out, unsigned value, int *error);
static void cache_put64(struct wb_buffer *out, uint64_t value, int *error);
static uint64_t cache_get64(const unsigned char *bytes);

/*
 * Opens a persistent cache over a storage queue: reads the index record
 * (and nothing else of the cache's space but the list of its keys, to
 * remove records the index does not list).
 *
 * queue: the instance's storage queue; borrowed until net_cache_close.
 * limit: the most bytes of records to keep.
 * An index that cannot be read or decoded is an empty one.  Waits for the
 * storage; the caller holds no lock.  Returns 0, or ENOMEM (nothing is
 * left open).
 */
int
net_cache_open(
	struct wb_storage_queue *queue,
	size_t limit,
	struct net_cache **cache)
{
	struct net_cache *made;
	int error;

	/* The cache, empty. */
	*cache = NULL;
	made = calloc(1, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Its lock, its storage and its limit. */
	error = wb_mutex_init(&made->lock);
	if (error != 0) {
		free(made);
		return error;
	}
	made->queue = queue;
	made->limit = limit;
	made->written = cache_now();

	/* The index the storage kept; only a lack of memory fails the opening. */
	error = cache_load(made);
	if (error != 0) {
		net_cache_close(made);
		return error;
	}

	/* Succeeded: the cache lists the responses the storage keeps. */
	*cache = made;
	return 0;
}

/*
 * Closes a persistent cache: queues the index if a use changed it, then
 * frees it (the records stay in the storage).  Called before the
 * storage queue is closed, so that the write reaches the backend.  No
 * other call on the cache may be in progress or follow.  NULL does
 * nothing.
 */
void
net_cache_close(
	struct net_cache *cache)
{
	/* No cache. */
	if (cache == NULL)
		return;

	/* A use not yet in the index record is written now. */
	if (cache->dirty)
		cache_write_index(cache);

	/* Then the lock and the index. */
	wb_mutex_destroy(&cache->lock);
	free(cache->entries);
	free(cache);
}

/*
 * Reads the stored response of a URL, if the cache has one.
 *
 * url, length: the URL as the loader keys it (its serialization).
 * record:      an empty buffer; receives the record's bytes when found.
 *              It is left empty otherwise.
 * found:       receives 1 when the record exists, decodes and is the
 *              response of this URL (it then also counts as used now),
 *              and 0 otherwise.  A record that cannot be read or that
 *              does not decode is removed; one that is another URL's
 *              (the same key) is left.
 * Waits for the storage (the caller holds no lock of the engine).
 * Returns 0 (found or not) or ENOMEM.
 */
int
net_cache_read(
	struct net_cache *cache,
	const char *url,
	size_t length,
	struct wb_buffer *record,
	int *found)
{
	struct wb_storage_cache_entry decoded;
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	size_t index;
	int present;
	int error;

	/* Nothing found yet. */
	*found = 0;
	if (length == 0)
		return 0;

	/* The URL's key. */
	error = wb_storage_key_text(url, length, 0, key);
	if (error != 0)
		return 0;

	/* The index decides whether the storage is asked at all. */
	wb_mutex_lock(&cache->lock);

	index = cache_find(cache, key);

	wb_mutex_unlock(&cache->lock);

	/* A URL the index does not list is not kept. */
	if (index == (size_t)-1)
		return 0;

	/* The record's bytes, waited for. */
	present = 0;
	error = wb_storage_queue_read(cache->queue, BROWSER_STORAGE_CACHE, key, record, &present);
	if (error == ENOMEM) {
		wb_buffer_clear(record);
		return ENOMEM;
	}

	/* A record that cannot be read, or is gone, leaves the index. */
	if (error != 0 || !present) {
		wb_buffer_clear(record);

		wb_mutex_lock(&cache->lock);

		index = cache_find(cache, key);
		if (index != (size_t)-1)
			cache_drop(cache, index, error != 0);
		cache_write_index(cache);

		wb_mutex_unlock(&cache->lock);

		/* Succeeded: the cache has no such response. */
		return 0;
	}

	/* A record that does not decode is removed. */
	error = wb_storage_cache_decode(record->data, record->length, &decoded);
	if (error != 0) {
		wb_buffer_clear(record);

		wb_mutex_lock(&cache->lock);

		index = cache_find(cache, key);
		if (index != (size_t)-1)
			cache_drop(cache, index, 1);
		cache_write_index(cache);

		wb_mutex_unlock(&cache->lock);

		/* Succeeded: the cache has no such response. */
		return 0;
	}

	/* The record of another URL that has the same key is not this URL's response. */
	if (decoded.url_length != length || memcmp(decoded.url, url, length) != 0) {
		wb_buffer_clear(record);
		return 0;
	}

	/* The response is used now; the index record follows at most once in a while. */
	wb_mutex_lock(&cache->lock);

	index = cache_find(cache, key);
	if (index != (size_t)-1) {
		cache->entries[index].used = cache_now();
		cache->dirty = 1;
		if (cache->entries[index].used - cache->written >= CACHE_INDEX_INTERVAL)
			cache_write_index(cache);
	}

	wb_mutex_unlock(&cache->lock);

	/* Succeeded: the record is the URL's response. */
	*found = 1;
	return 0;
}

/*
 * Queues a response to be stored, replacing the one of the same URL, and
 * removes the least recently used responses until the limit holds.
 *
 * entry: the response (the fields of struct wb_storage_cache_entry; its
 *        stored time is in wall-clock milliseconds).
 * Never waits, so the loader's network thread may call it.  A response
 * whose record is larger than the limit is not stored (and the URL's
 * older one goes).  When the queue has too many bytes waiting (ENOSPC)
 * a tenth of the stored bytes is removed and the write tried once more.
 * Returns 0, EINVAL (the entry cannot be encoded), EFBIG (larger than
 * the limit or than a record may be), ENOSPC or ENOMEM (not stored).
 */
int
net_cache_store(
	struct net_cache *cache,
	const struct wb_storage_cache_entry *entry)
{
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	struct wb_buffer record;
	size_t index;
	uint64_t old_bytes;
	int64_t now;
	int error;

	/* The URL's key. */
	if (entry->url_length == 0)
		return EINVAL;
	error = wb_storage_key_text(entry->url, entry->url_length, 0, key);
	if (error != 0)
		return EINVAL;

	/* The record. */
	wb_buffer_init(&record);
	error = wb_storage_cache_encode(entry, &record);
	if (error != 0) {
		wb_buffer_release(&record);
		return error;
	}

	/* The index and the queue change together. */
	wb_mutex_lock(&cache->lock);

	/* A record beyond the limit is not kept; the URL's older one is stale now. */
	if (record.length > cache->limit) {
		index = cache_find(cache, key);
		if (index != (size_t)-1) {
			cache_drop(cache, index, 1);
			cache_write_index(cache);
		}

		wb_mutex_unlock(&cache->lock);

		wb_buffer_release(&record);
		return EFBIG;
	}

	/* The record, queued to be written; a queue that is full makes room by dropping old responses, once. */
	error = wb_storage_queue_write(cache->queue, BROWSER_STORAGE_CACHE, key, record.data, record.length);
	if (error == ENOSPC) {
		cache_evict(cache, key, cache->total - cache->total / 10U, 0);
		error = wb_storage_queue_write(cache->queue, BROWSER_STORAGE_CACHE, key, record.data, record.length);
	}
	if (error != 0) {
		/* Whatever the queue made room by dropping is not in the index record yet. */
		cache->dirty = 1;

		wb_mutex_unlock(&cache->lock);

		wb_buffer_release(&record);
		return error;
	}

	/* The index's item of the key: the older response's size goes, the new one's comes. */
	now = cache_now();
	index = cache_find(cache, key);
	if (index != (size_t)-1) {
		old_bytes = cache->entries[index].bytes;
		cache->total -= old_bytes;
		cache->entries[index].bytes = record.length;
		cache->entries[index].stored = entry->stored;
		cache->entries[index].used = now;
		cache->total += record.length;
	} else {
		/* A new response; the index keeps room for it. */
		cache_evict(cache, key, UINT64_MAX, CACHE_ENTRY_MAX - 1U);
		error = cache_push(cache, key, record.length, entry->stored, now);
		if (error != 0) {
			wb_mutex_unlock(&cache->lock);

			wb_buffer_release(&record);
			return error;
		}
		cache->total += record.length;
	}

	/* The total fits the limit again, the least recently used going first; the index record follows. */
	cache_evict(cache, key, cache->limit, CACHE_ENTRY_MAX);
	cache_write_index(cache);

	wb_mutex_unlock(&cache->lock);

	/* Succeeded: the response is queued and indexed. */
	wb_buffer_release(&record);
	return 0;
}

/*
 * Removes the response of a URL, when the cache has one: its record
 * goes from the storage and its item from the index.  Never waits, so
 * the loader's network thread may call it.  A URL the cache does not
 * hold is not an error.
 *
 * url, length: the URL as net_cache_store keyed it.
 */
void
net_cache_remove(
	struct net_cache *cache,
	const char *url,
	size_t length)
{
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	size_t index;
	int error;

	/* The URL's key; a URL that has none was never stored. */
	if (length == 0)
		return;
	error = wb_storage_key_text(url, length, 0, key);
	if (error != 0)
		return;

	/* The index and the queue change together. */
	wb_mutex_lock(&cache->lock);

	index = cache_find(cache, key);
	if (index != (size_t)-1) {
		cache_drop(cache, index, 1);
		cache_write_index(cache);
	}

	wb_mutex_unlock(&cache->lock);
}

/*
 * Reads the index record, drops the records it does not list, and
 * makes the total fit the limit.  Returns 0 or ENOMEM.
 */
static int
cache_load(
	struct net_cache *cache)
{
	struct wb_buffer record;
	int found;
	int read_error;
	int error;

	/* The index record's bytes. */
	wb_buffer_init(&record);
	found = 0;
	read_error = wb_storage_queue_read(cache->queue, BROWSER_STORAGE_CACHE, CACHE_INDEX_KEY, &record, &found);
	if (read_error == ENOMEM) {
		wb_buffer_release(&record);
		return ENOMEM;
	}

	/* An index that decodes is the cache's; one that does not is an empty one. */
	if (read_error == 0 && found) {
		error = cache_decode_index(cache, record.data, record.length);
		if (error == ENOMEM) {
			wb_buffer_release(&record);
			return ENOMEM;
		}
	}
	wb_buffer_release(&record);

	/* Records the index does not list are removed, unless the index could not be read at all. */
	if (read_error == 0) {
		error = cache_sweep(cache);
		if (error != 0)
			return error;
	}

	/* A limit lower than the last run's brings the total down, and the index record with it. */
	if (cache->total > cache->limit) {
		cache_evict(cache, NULL, cache->limit, CACHE_ENTRY_MAX);
		cache_write_index(cache);
	}

	/* Succeeded: the index is in memory. */
	return 0;
}

/* Orders pointers to keys by the keys' text (for qsort). */
static int
cache_compare_keys(
	const void *left,
	const void *right)
{
	const char *const *left_key;
	const char *const *right_key;
	int order;

	/* The two keys the pointers point to. */
	left_key = left;
	right_key = right;
	order = strcmp(*left_key, *right_key);

	/* The order of the texts. */
	return order;
}

/* Removes the space's records that the index does not list (and are not the index). */
static int
cache_sweep(
	struct net_cache *cache)
{
	struct wb_buffer keys;
	const char **sorted;
	const char *key;
	const void *listed;
	size_t count;
	size_t index;
	int error;

	/* The keys of the space; a list that fails sweeps nothing. */
	wb_buffer_init(&keys);
	count = 0;
	error = wb_storage_queue_list(cache->queue, BROWSER_STORAGE_CACHE, &keys, &count);
	if (error == ENOMEM) {
		wb_buffer_release(&keys);
		return ENOMEM;
	}
	if (error != 0) {
		wb_buffer_release(&keys);
		return 0;
	}

	/* The index's keys in order, to look each listed key up quickly. */
	sorted = NULL;
	if (cache->count != 0) {
		sorted = malloc(cache->count * sizeof(*sorted));
		if (sorted == NULL) {
			wb_buffer_release(&keys);
			return ENOMEM;
		}
		for (index = 0; index < cache->count; index++)
			sorted[index] = cache->entries[index].key;
		qsort(sorted, cache->count, sizeof(*sorted), cache_compare_keys);
	}

	/* Each key that is neither the index's nor listed in it (each key ends with its NUL). */
	key = (const char *)keys.data;
	for (index = 0; index < count; index++) {
		listed = NULL;
		if (sorted != NULL)
			listed = bsearch(&key, sorted, cache->count, sizeof(*sorted), cache_compare_keys);
		if (listed == NULL && strcmp(key, CACHE_INDEX_KEY) != 0)
			(void)wb_storage_queue_remove(cache->queue, BROWSER_STORAGE_CACHE, key);
		key += strlen(key) + 1U;
	}
	free(sorted);
	wb_buffer_release(&keys);

	/* Succeeded: the space holds what the index lists and the index. */
	return 0;
}

/*
 * Decodes an index record into the cache.  A record that does not decode
 * (a wrong magic, a length past the end, an invalid, repeated or
 * too many keys, bytes left over, a size above a record's largest) leaves
 * the index empty; the cache is the index's alone then.  Returns 0 or
 * ENOMEM.
 */
static int
cache_decode_index(
	struct net_cache *cache,
	const unsigned char *bytes,
	size_t length)
{
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	const size_t magic_length = sizeof(CACHE_INDEX_MAGIC) - 1U;
	size_t position;
	size_t key_length;
	uint64_t record_bytes;
	int64_t stored;
	int64_t used;
	int valid;
	int error;

	/* The magic line. */
	if (length < magic_length || memcmp(bytes, CACHE_INDEX_MAGIC, magic_length) != 0)
		return 0;
	position = magic_length;

	/* Each item, until the end of the record. */
	while (position < length) {
		/* The key's length and bytes. */
		if (length - position < 2U)
			goto corrupt;
		key_length = (size_t)bytes[position] | ((size_t)bytes[position + 1U] << 8);
		position += 2U;
		if (key_length == 0 || key_length > BROWSER_STORAGE_KEY_MAX)
			goto corrupt;
		if (length - position < key_length + 24U)
			goto corrupt;
		memcpy(key, bytes + position, key_length);
		key[key_length] = '\0';
		position += key_length;

		/* A key that is not a response's, or one listed already. */
		valid = wb_storage_key_valid(key);
		if (!valid || strcmp(key, CACHE_INDEX_KEY) == 0)
			goto corrupt;
		if (cache_find(cache, key) != (size_t)-1)
			goto corrupt;

		/* The size and the two times. */
		record_bytes = cache_get64(bytes + position);
		stored = (int64_t)cache_get64(bytes + position + 8U);
		used = (int64_t)cache_get64(bytes + position + 16U);
		position += 24U;
		if (record_bytes > WB_STORAGE_RECORD_MAX)
			goto corrupt;

		/* More items than the index may hold. */
		if (cache->count == CACHE_ENTRY_MAX)
			goto corrupt;

		/* The item. */
		error = cache_push(cache, key, record_bytes, stored, used);
		if (error != 0)
			return error;
		cache->total += record_bytes;
	}

	/* Succeeded: the index is the record's. */
	return 0;

corrupt:
	/* The record is not trusted: the index starts empty. */
	cache->count = 0;
	cache->total = 0;
	return 0;
}

/* Appends the index record to out. */
static int
cache_encode_index(
	const struct net_cache *cache,
	struct wb_buffer *out)
{
	const struct cache_entry *item;
	size_t index;
	size_t key_length;
	int error;

	/* The magic line. */
	error = wb_buffer_append(out, CACHE_INDEX_MAGIC, sizeof(CACHE_INDEX_MAGIC) - 1U);
	if (error != 0)
		return error;

	/* Each item: the key with its length, then the size and the times. */
	error = 0;
	for (index = 0; index < cache->count && error == 0; index++) {
		item = &cache->entries[index];
		key_length = strlen(item->key);
		cache_put16(out, (unsigned)key_length, &error);
		if (error == 0)
			error = wb_buffer_append(out, item->key, key_length);
		cache_put64(out, item->bytes, &error);
		cache_put64(out, (uint64_t)item->stored, &error);
		cache_put64(out, (uint64_t)item->used, &error);
	}
	if (error != 0)
		return error;

	/* Succeeded: out holds the record. */
	return 0;
}

/*
 * Queues the index record (the lock is held).  A record that cannot be
 * made or queued leaves the cache marked as not written, so that the
 * next change or the close tries again.
 */
static void
cache_write_index(
	struct net_cache *cache)
{
	struct wb_buffer record;
	int error;

	/* The record. */
	wb_buffer_init(&record);
	error = cache_encode_index(cache, &record);
	if (error != 0) {
		wb_buffer_release(&record);
		cache->dirty = 1;
		return;
	}

	/* The record queued; the queue copies it. */
	error = wb_storage_queue_write(cache->queue, BROWSER_STORAGE_CACHE, CACHE_INDEX_KEY, record.data, record.length);
	wb_buffer_release(&record);
	if (error != 0) {
		cache->dirty = 1;
		return;
	}

	/* Succeeded: the record has everything up to now. */
	cache->dirty = 0;
	cache->written = cache_now();
}

/* Adds an item at the end of the index (the lock is held, or the cache is not shared yet). Returns 0 or ENOMEM. */
static int
cache_push(
	struct net_cache *cache,
	const char *key,
	uint64_t bytes,
	int64_t stored,
	int64_t used)
{
	struct cache_entry *grown;
	struct cache_entry *item;
	size_t capacity;

	/* Room for one more, the capacity doubling. */
	if (cache->count == cache->capacity) {
		capacity = cache->capacity * 2U;
		if (capacity == 0)
			capacity = 16U;
		grown = realloc(cache->entries, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		cache->entries = grown;
		cache->capacity = capacity;
	}

	/* The item. */
	item = &cache->entries[cache->count];
	strcpy(item->key, key);
	item->bytes = bytes;
	item->stored = stored;
	item->used = used;
	cache->count++;

	/* Succeeded: the index lists the key. */
	return 0;
}

/* Reports the position of a key in the index, or (size_t)-1 when it is not listed. */
static size_t
cache_find(
	const struct net_cache *cache,
	const char *key)
{
	size_t index;
	int differs;

	/* Each item. */
	for (index = 0; index < cache->count; index++) {
		differs = strcmp(cache->entries[index].key, key);
		if (differs == 0)
			return index;
	}

	/* The key is not listed. */
	return (size_t)-1;
}

/*
 * Takes an item out of the index (the lock is held), and its record out
 * of the storage when remove_record is set.  The index record is not
 * queued here.
 */
static void
cache_drop(
	struct net_cache *cache,
	size_t index,
	int remove_record)
{
	/* The record goes from the storage. */
	if (remove_record)
		(void)wb_storage_queue_remove(cache->queue, BROWSER_STORAGE_CACHE, cache->entries[index].key);

	/* The item goes from the index, the last one taking its place (the order of the index is not kept); the index record lacks the change. */
	cache->dirty = 1;
	cache->total -= cache->entries[index].bytes;
	cache->count--;
	if (index != cache->count)
		cache->entries[index] = cache->entries[cache->count];
}

/*
 * Removes the least recently used responses (the lock is held) until the
 * records total at most room bytes and the index lists at most slots
 * items.  The response of keep (NULL for none) is never removed.
 */
static void
cache_evict(
	struct net_cache *cache,
	const char *keep,
	uint64_t room,
	size_t slots)
{
	size_t index;
	size_t oldest;
	int removed;

	/* One response at a time, the one used longest ago. */
	removed = 1;
	while (removed && (cache->total > room || cache->count > slots)) {
		removed = 0;
		oldest = (size_t)-1;
		for (index = 0; index < cache->count; index++) {
			if (keep != NULL && strcmp(cache->entries[index].key, keep) == 0)
				continue;
			if (oldest == (size_t)-1 || cache->entries[index].used < cache->entries[oldest].used)
				oldest = index;
		}

		/* Nothing is left to remove but the kept one. */
		if (oldest == (size_t)-1)
			break;

		cache_drop(cache, oldest, 1);
		removed = 1;
	}
}

/* Reads the wall clock, in milliseconds since 1970 UTC. */
static int64_t
cache_now(void)
{
	/* The wall clock. */
	return wb_os_wall_ns() / INT64_C(1000000);
}

/* Appends a 16-bit value, little-endian; a failure sets *error. */
static void
cache_put16(
	struct wb_buffer *out,
	unsigned value,
	int *error)
{
	unsigned char bytes[2];

	/* Low byte first. */
	if (*error != 0)
		return;
	bytes[0] = (unsigned char)(value & 0xffU);
	bytes[1] = (unsigned char)((value >> 8) & 0xffU);
	*error = wb_buffer_append(out, bytes, 2U);
}

/* Appends a 64-bit value, little-endian; a failure sets *error. */
static void
cache_put64(
	struct wb_buffer *out,
	uint64_t value,
	int *error)
{
	unsigned char bytes[8];
	size_t index;

	/* Low byte first. */
	if (*error != 0)
		return;
	for (index = 0; index < 8U; index++)
		bytes[index] = (unsigned char)((value >> (8U * index)) & 0xffU);
	*error = wb_buffer_append(out, bytes, 8U);
}

/* Reads a 64-bit little-endian value from 8 bytes. */
static uint64_t
cache_get64(
	const unsigned char *bytes)
{
	uint64_t value;
	size_t index;

	/* High byte first, shifting up. */
	value = 0;
	for (index = 8U; index > 0; index--)
		value = (value << 8) | bytes[index - 1U];

	/* The value. */
	return value;
}
