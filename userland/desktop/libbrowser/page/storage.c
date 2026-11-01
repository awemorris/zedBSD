/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The storage areas of Web Storage: the window's host callbacks behind
 * sessionStorage and localStorage.
 *
 * An area belongs to an origin: an http or https page's origin, one for
 * every file: page, and one for the pages without either (about:blank),
 * which is never saved.  The areas belong to the page's instance
 * (storage/instance.h), whose pages run on one thread: a session area
 * lasts as long as the instance, and a local area is read from the
 * instance's storage backend the first time it is used and written back
 * (behind, through the instance's storage queue) after each change.  Its
 * record (BROWSER_STORAGE_LOCAL, storage/records.h) is kept under the key
 * of the origin's bytes; the record holds the origin, so that a record of
 * another origin under the same key (a hash collision of a long origin)
 * is passed over for the next collision number.  A record of the earlier
 * format, which does not hold its origin, is read under the key of a short
 * origin (its old file name) and written again in the new format at the
 * next change.  An instance without a backend keeps its local areas in
 * memory only, as does an area whose record could not be written.  The
 * items are kept in the order of their keys' code units, which key(n)
 * reports.  An origin's area holds at most STORAGE_QUOTA_BYTES of keys
 * and values counted as UTF-16.
 */

#include "page/page.h"
#include "net/net.h"
#include "storage/instance.h"
#include "storage/queue.h"
#include "storage/records.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most an origin's area holds: its keys' and values' UTF-16, in bytes (5 MiB, as other browsers). */
#define STORAGE_QUOTA_BYTES	(5UL * 1024UL * 1024UL)

/* The most collision numbers tried for the key of a local area's record. */
#define STORAGE_COLLISIONS	16U

/*
 * What a page's scheme means for its storage (storage_scheme_kind).
 */
enum storage_scheme {
	STORAGE_SCHEME_OTHER,	/* about:blank and the like: the empty origin, never saved */
	STORAGE_SCHEME_WEB,	/* http and https: the page's origin */
	STORAGE_SCHEME_FILE	/* file: one origin for every file */
};

/*
 * One item: its key and its value, UTF-16 in memory of its own.
 */
struct storage_item {
	uint16_t *key;
	size_t key_length;
	uint16_t *value;
	size_t value_length;
};

/*
 * One origin's storage area of an instance: the origin, which area it is
 * (BIND_STORAGE_*), whether it is saved (a local area of a web or file:
 * page whose record has a key), the instance's queue it is saved through
 * and the key of its record, its items in the order of their keys, and
 * the bytes they take.  It lives as long as its instance
 * (page_storage_release).
 */
struct storage_area {
	char *origin;
	int area;
	int saved;
	struct wb_storage_queue *queue;
	char key[BROWSER_STORAGE_KEY_MAX + 1];
	struct wb_vector items;
	size_t bytes;
};

static int storage_length(void *context, int area, size_t *count);
static int storage_key(void *context, int area, size_t index, struct wb_units *key, int *found);
static int storage_get(void *context, int area, const uint16_t *key, size_t key_length, struct wb_units *value, int *found);
static int storage_set(void *context, int area, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
static int storage_remove(void *context, int area, const uint16_t *key, size_t key_length);
static int storage_clear(void *context, int area);
static int storage_area_of(struct page *page, int area, struct storage_area **found);
static int storage_origin(struct page *page, char **origin, int *saved);
static int storage_scheme_kind(const char *scheme);
static int storage_find(const struct storage_area *area, const uint16_t *key, size_t key_length, size_t *index);
static int storage_compare(const uint16_t *left, size_t left_length, const uint16_t *right, size_t right_length);
static int storage_insert(struct storage_area *area, size_t index, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
static void storage_remove_at(struct storage_area *area, size_t index);
static void storage_empty(struct storage_area *area);
static uint16_t *storage_copy_units(const uint16_t *units, size_t length);
static void storage_load(struct storage_area *area);
static int storage_load_item(void *context, const uint16_t *key, size_t key_length, const uint16_t *value, size_t value_length);
static void storage_save(const struct storage_area *area);

/*
 * The callbacks of the window's host for Web Storage.  The table is
 * constant for the life of the program.
 */
static const struct bind_storage_calls storage_calls = {
	storage_length,
	storage_key,
	storage_get,
	storage_set,
	storage_remove,
	storage_clear
};

/*
 * Reports the window's host callbacks for Web Storage (the page is their
 * context).
 */
const struct bind_storage_calls *
page_storage_calls(void)
{
	/* The table. */
	return &storage_calls;
}

/*
 * Frees the Web Storage areas of an instance (a vector of
 * struct storage_area pointers), leaving the vector empty.  Their records
 * stay in the storage.
 */
void
page_storage_release(
	struct wb_vector *areas)
{
	struct storage_area **list;
	size_t index;

	/* Each area's items, origin and memory. */
	list = areas->items;
	for (index = 0; index < areas->count; index++) {
		storage_empty(list[index]);
		wb_vector_release(&list[index]->items);
		free(list[index]->origin);
		free(list[index]);
	}

	/* The vector holds none. */
	wb_vector_clear(areas);
}

/* Reports how many items an area of the page's origin has. */
static int
storage_length(
	void *context,
	int area,
	size_t *count)
{
	struct storage_area *found;
	int error;

	/* The area. */
	error = storage_area_of(context, area, &found);
	if (error != 0)
		return error;

	/* Succeeded: its count. */
	*count = found->items.count;
	return 0;
}

/* Reports the key at a place of an area, in the order of the keys. */
static int
storage_key(
	void *context,
	int area,
	size_t index,
	struct wb_units *key,
	int *found)
{
	struct storage_area *storage;
	struct storage_item *item;
	int error;

	/* The area. */
	*found = 0;
	error = storage_area_of(context, area, &storage);
	if (error != 0)
		return error;

	/* A place past the last item has no key. */
	if (index >= storage->items.count)
		return 0;

	/* Succeeded: the key. */
	item = wb_vector_at(&storage->items, index);
	error = wb_units_append(key, item->key, item->key_length);
	if (error != 0)
		return error;
	*found = 1;
	return 0;
}

/* Reports a key's value in an area. */
static int
storage_get(
	void *context,
	int area,
	const uint16_t *key,
	size_t key_length,
	struct wb_units *value,
	int *found)
{
	struct storage_area *storage;
	struct storage_item *item;
	size_t index;
	int present;
	int error;

	/* The area. */
	*found = 0;
	error = storage_area_of(context, area, &storage);
	if (error != 0)
		return error;

	/* A key not there has no value. */
	present = storage_find(storage, key, key_length, &index);
	if (!present)
		return 0;

	/* Succeeded: the value. */
	item = wb_vector_at(&storage->items, index);
	error = wb_units_append(value, item->value, item->value_length);
	if (error != 0)
		return error;
	*found = 1;
	return 0;
}

/*
 * Sets a key's value in an area, within the origin's quota (ENOSPC past
 * it), and saves a local area.
 */
static int
storage_set(
	void *context,
	int area,
	const uint16_t *key,
	size_t key_length,
	const uint16_t *value,
	size_t value_length)
{
	struct storage_area *storage;
	struct storage_item *item;
	uint16_t *copy;
	size_t index;
	size_t bytes;
	int present;
	int error;

	/* The area and where the key is, or goes. */
	error = storage_area_of(context, area, &storage);
	if (error != 0)
		return error;
	present = storage_find(storage, key, key_length, &index);

	/* The bytes the area would take with the item. */
	bytes = storage->bytes + (key_length + value_length) * 2U;
	if (present) {
		item = wb_vector_at(&storage->items, index);
		bytes -= (item->key_length + item->value_length) * 2U;
	}

	/* An item that would take the area past its quota is refused. */
	if (bytes > STORAGE_QUOTA_BYTES)
		return ENOSPC;

	/* A new key is inserted in its place. */
	if (!present) {
		error = storage_insert(storage, index, key, key_length, value, value_length);
		if (error != 0)
			return error;
	} else {
		/* A key there takes the new value. */
		copy = storage_copy_units(value, value_length);
		if (copy == NULL)
			return ENOMEM;
		item = wb_vector_at(&storage->items, index);
		free(item->value);
		item->value = copy;
		item->value_length = value_length;
	}

	/* The area's bytes with the item. */
	storage->bytes = bytes;

	/* Succeeded: the item is set, and a local area saved. */
	if (storage->saved)
		storage_save(storage);
	return 0;
}

/* Removes a key from an area, and saves a local area. */
static int
storage_remove(
	void *context,
	int area,
	const uint16_t *key,
	size_t key_length)
{
	struct storage_area *storage;
	size_t index;
	int present;
	int error;

	/* The area. */
	error = storage_area_of(context, area, &storage);
	if (error != 0)
		return error;

	/* A key not there changes nothing. */
	present = storage_find(storage, key, key_length, &index);
	if (!present)
		return 0;

	/* Succeeded: the item goes, and a local area is saved. */
	storage_remove_at(storage, index);
	if (storage->saved)
		storage_save(storage);
	return 0;
}

/* Removes every item of an area, and saves a local area. */
static int
storage_clear(
	void *context,
	int area)
{
	struct storage_area *storage;
	int error;

	/* The area. */
	error = storage_area_of(context, area, &storage);
	if (error != 0)
		return error;

	/* An empty area stays as it is. */
	if (storage->items.count == 0)
		return 0;

	/* Each item. */
	storage_empty(storage);

	/* Succeeded: the area is empty, and a local area saved. */
	if (storage->saved)
		storage_save(storage);
	return 0;
}

/*
 * Finds the area of a kind for the page's origin in the page's instance,
 * made (and a local area read from the instance's storage) the first time.
 */
static int
storage_area_of(
	struct page *page,
	int area,
	struct storage_area **found)
{
	struct browser_instance *instance;
	struct storage_area **areas;
	struct storage_area *made;
	char *origin;
	size_t index;
	int saved;
	int same;
	int error;

	/* The instance whose areas the page sees. */
	instance = page_instance(page);
	if (instance == NULL)
		return ENOMEM;

	/* The page's origin. */
	error = storage_origin(page, &origin, &saved);
	if (error != 0)
		return error;

	/* An area of the origin made before. */
	areas = instance->storage_areas.items;
	for (index = 0; index < instance->storage_areas.count; index++) {
		if (areas[index]->area != area)
			continue;
		same = strcmp(areas[index]->origin, origin);
		if (same == 0) {
			free(origin);
			*found = areas[index];
			return 0;
		}
	}

	/* A new one, which keeps the origin. */
	made = calloc(1, sizeof(*made));
	if (made == NULL) {
		free(origin);
		return ENOMEM;
	}

	/* Its origin and kind; only a local area of a web or file: page is saved. */
	made->origin = origin;
	made->area = area;
	made->saved = 0;
	if (area == BIND_STORAGE_LOCAL)
		made->saved = saved;
	made->queue = instance->queue;
	wb_vector_init(&made->items, sizeof(struct storage_item));

	/* In the instance's table. */
	error = wb_vector_push(&instance->storage_areas, &made);
	if (error != 0) {
		free(made->origin);
		free(made);
		return ENOMEM;
	}

	/* A local area has what the storage kept; a record that cannot be read is an empty area. */
	if (made->saved)
		storage_load(made);

	/* Succeeded: the area. */
	*found = made;
	return 0;
}

/*
 * Finds the origin a page's storage belongs to (an allocated string):
 * an http or https page's origin, "file://" for a file: page, and the
 * empty string for any other; saved says whether its local area is kept
 * in a file.
 */
static int
storage_origin(
	struct page *page,
	char **origin,
	int *saved)
{
	struct net_url url;
	struct wb_buffer text;
	int kind;
	int error;

	/* No origin until one is found. */
	*origin = NULL;
	*saved = 0;
	wb_buffer_init(&text);

	/* The page's URL; a page without one is the empty origin. */
	error = page_url(page, &url);
	if (error == ENOMEM)
		return error;
	if (error == 0) {
		/* An http or https page has its origin, a file: page the one of every file. */
		kind = storage_scheme_kind(url.scheme);
		if (kind == STORAGE_SCHEME_WEB) {
			error = net_url_component(&url, NET_URL_ORIGIN, &text);
			*saved = 1;
		} else if (kind == STORAGE_SCHEME_FILE) {
			error = wb_buffer_append_string(&text, "file://");
			*saved = 1;
		}

		/* The URL is done with. */
		net_url_release(&url);
	}

	/* An origin that could not be written. */
	if (error != 0) {
		wb_buffer_release(&text);
		return error;
	}

	/* The origin's own copy. */
	*origin = strdup(wb_buffer_string(&text));
	wb_buffer_release(&text);
	if (*origin == NULL)
		return ENOMEM;

	/* Succeeded: the origin. */
	return 0;
}

/* Tells what a URL's scheme means for its page's storage (a storage_scheme). */
static int
storage_scheme_kind(
	const char *scheme)
{
	int differs;

	/* http and https. */
	differs = strcmp(scheme, "http");
	if (differs == 0)
		return STORAGE_SCHEME_WEB;
	differs = strcmp(scheme, "https");
	if (differs == 0)
		return STORAGE_SCHEME_WEB;

	/* file. */
	differs = strcmp(scheme, "file");
	if (differs == 0)
		return STORAGE_SCHEME_FILE;

	/* Any other. */
	return STORAGE_SCHEME_OTHER;
}

/*
 * Finds a key in an area's items (in the order of their keys): reports
 * whether it is there, and its place or the place it would go.
 */
static int
storage_find(
	const struct storage_area *area,
	const uint16_t *key,
	size_t key_length,
	size_t *index)
{
	const struct storage_item *items;
	size_t low;
	size_t high;
	size_t middle;
	int order;

	/* A binary search of the keys. */
	items = area->items.items;
	low = 0;
	high = area->items.count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		order = storage_compare(items[middle].key, items[middle].key_length, key, key_length);
		if (order == 0) {
			*index = middle;
			return 1;
		}

		/* The half the key is in. */
		if (order < 0) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* Succeeded: it is not there, and would go at low. */
	*index = low;
	return 0;
}

/* Orders two keys by their code units, a shorter one first when it is the other's start. */
static int
storage_compare(
	const uint16_t *left,
	size_t left_length,
	const uint16_t *right,
	size_t right_length)
{
	size_t index;

	/* The first unit that differs. */
	for (index = 0; index < left_length && index < right_length; index++) {
		if (left[index] < right[index])
			return -1;
		if (left[index] > right[index])
			return 1;
	}

	/* The shorter one first. */
	if (left_length < right_length)
		return -1;
	if (left_length > right_length)
		return 1;

	/* Succeeded: they are the same. */
	return 0;
}

/* Inserts an item at a place of an area's items. */
static int
storage_insert(
	struct storage_area *area,
	size_t index,
	const uint16_t *key,
	size_t key_length,
	const uint16_t *value,
	size_t value_length)
{
	struct storage_item item;
	struct storage_item *items;
	int error;

	/* The item's own copies. */
	memset(&item, 0, sizeof(item));
	item.key = storage_copy_units(key, key_length);
	if (item.key == NULL)
		return ENOMEM;
	item.value = storage_copy_units(value, value_length);
	if (item.value == NULL) {
		free(item.key);
		return ENOMEM;
	}

	/* Their lengths. */
	item.key_length = key_length;
	item.value_length = value_length;

	/* A place at the end, then the later items moved up to make room at index. */
	error = wb_vector_push(&area->items, &item);
	if (error != 0) {
		free(item.key);
		free(item.value);
		return ENOMEM;
	}

	/* The item at index, the later ones one place up. */
	items = area->items.items;
	memmove(&items[index + 1U], &items[index], (area->items.count - 1U - index) * sizeof(*items));
	items[index] = item;

	/* Succeeded: the item is in its place. */
	return 0;
}

/* Removes the item at a place of an area's items, and the bytes it took. */
static void
storage_remove_at(
	struct storage_area *area,
	size_t index)
{
	struct storage_item *items;

	/* The item's memory and bytes. */
	items = area->items.items;
	area->bytes -= (items[index].key_length + items[index].value_length) * 2U;
	free(items[index].key);
	free(items[index].value);

	/* The later items move down. */
	memmove(&items[index], &items[index + 1U], (area->items.count - 1U - index) * sizeof(*items));
	area->items.count--;
}

/* Returns an allocated copy of UTF-16 units (one unit when there are none), or NULL without memory. */
static uint16_t *
storage_copy_units(
	const uint16_t *units,
	size_t length)
{
	uint16_t *copy;

	/* Room for them, never nothing. */
	copy = malloc((length + 1U) * sizeof(*copy));
	if (copy == NULL)
		return NULL;

	/* Succeeded: the copy. */
	if (length > 0)
		memcpy(copy, units, length * sizeof(*copy));
	return copy;
}

/* Removes every item of an area and the bytes they took. */
static void
storage_empty(
	struct storage_area *area)
{
	/* Each item, from the last. */
	while (area->items.count > 0)
		storage_remove_at(area, area->items.count - 1U);
}

/*
 * Reads a local area's items from the instance's storage, and finds the
 * key its record is kept under: the first collision number whose record
 * is missing, or is this origin's.  A record that cannot be read or does
 * not decode is taken as this origin's lost one: the area starts empty
 * and the record is removed (the next change writes it again).  An area
 * for which no key is left, or that cannot be read for lack of memory,
 * is not saved.
 */
static void
storage_load(
	struct storage_area *area)
{
	struct wb_buffer bytes;
	const unsigned char *origin;
	size_t origin_length;
	size_t length;
	unsigned collision;
	int found;
	int differs;
	int error;

	/* Each collision number until a key is this origin's. */
	length = strlen(area->origin);
	wb_buffer_init(&bytes);
	for (collision = 0; collision < STORAGE_COLLISIONS; collision++) {
		/* The key of this try. */
		error = wb_storage_key_text(area->origin, length, collision, area->key);
		if (error != 0)
			break;

		/* The record under it. */
		wb_buffer_clear(&bytes);
		found = 0;
		error = wb_storage_queue_read(area->queue, BROWSER_STORAGE_LOCAL, area->key, &bytes, &found);
		if (error == ENOMEM)
			break;

		/* A record that could not be read is removed, and its key taken. */
		if (error != 0) {
			(void)wb_storage_queue_remove(area->queue, BROWSER_STORAGE_LOCAL, area->key);
			wb_buffer_release(&bytes);
			return;
		}

		/* No record: the key is free, and the area empty. */
		if (!found) {
			wb_buffer_release(&bytes);
			return;
		}

		/* The record's items into the area. */
		origin = NULL;
		origin_length = 0;
		error = wb_storage_local_decode(bytes.data, bytes.length, &origin, &origin_length, storage_load_item, area);
		if (error == ENOMEM) {
			storage_empty(area);
			break;
		}

		/* A record that does not decode is removed, and its key taken. */
		if (error != 0) {
			storage_empty(area);
			(void)wb_storage_queue_remove(area->queue, BROWSER_STORAGE_LOCAL, area->key);
			wb_buffer_release(&bytes);
			return;
		}

		/* A record of this origin. */
		differs = 1;
		if (origin != NULL && origin_length == length)
			differs = memcmp(origin, area->origin, length);
		if (differs == 0) {
			wb_buffer_release(&bytes);
			return;
		}

		/*
		 * A record of the earlier format holds no origin: it is this
		 * origin's under the key its old file was named by (a short
		 * origin's bytes in hexadecimal).
		 */
		if (origin == NULL && collision == 0 && strlen(area->key) == length * 2U) {
			wb_buffer_release(&bytes);
			return;
		}

		/* Another origin's record: its items go, and the next key is tried. */
		storage_empty(area);
	}

	/* No key: the area lives in memory only. */
	wb_buffer_release(&bytes);
	area->saved = 0;
	area->key[0] = '\0';
}

/* Puts an item read from a record into an area (a key given twice keeps the first); wb_storage_local_each. */
static int
storage_load_item(
	void *context,
	const uint16_t *key,
	size_t key_length,
	const uint16_t *value,
	size_t value_length)
{
	struct storage_area *area;
	size_t place;
	int present;
	int error;

	/* A key already read. */
	area = context;
	present = storage_find(area, key, key_length, &place);
	if (present)
		return 0;

	/* The item in its place, and the bytes it takes. */
	error = storage_insert(area, place, key, key_length, value, value_length);
	if (error != 0)
		return error;
	area->bytes += (key_length + value_length) * 2U;

	/* Succeeded: the item is in the area. */
	return 0;
}

/*
 * Queues the write of a local area's record (the origin and the items).
 * A record that cannot be made or queued leaves the area in memory; it is
 * written again at its next change, and the queue keeps a backend's error
 * for the view to tell.
 */
static void
storage_save(
	const struct storage_area *area)
{
	const struct storage_item *items;
	struct wb_storage_local_item *record;
	struct wb_buffer out;
	size_t index;
	int error;

	/* The items as the record takes them. */
	record = NULL;
	if (area->items.count > 0) {
		record = calloc(area->items.count, sizeof(*record));
		if (record == NULL)
			return;
	}
	items = area->items.items;
	for (index = 0; index < area->items.count; index++) {
		record[index].key = items[index].key;
		record[index].key_length = items[index].key_length;
		record[index].value = items[index].value;
		record[index].value_length = items[index].value_length;
	}

	/* The record's bytes. */
	wb_buffer_init(&out);
	error = wb_storage_local_encode(area->origin, strlen(area->origin), record, area->items.count, &out);
	free(record);

	/* Queued for the storage thread, which writes it behind. */
	if (error == 0)
		(void)wb_storage_queue_write(area->queue, BROWSER_STORAGE_LOCAL, area->key, out.data, out.length);
	wb_buffer_release(&out);
}
