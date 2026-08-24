/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "libc/locale-db.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCALE_CACHE_COUNT 16U

struct zed_locale_record {
	char name[64];
	unsigned char *data;
	size_t size;
	const char *values[ZEDBSD_LOCALE_KEY_COUNT];
	unsigned utf8;
	unsigned used;
};

static struct zed_locale_record locale_cache[LOCALE_CACHE_COUNT];
static volatile uint32_t locale_cache_lock;

static const int key_categories[ZEDBSD_LOCALE_KEY_COUNT] = {
    [ZEDBSD_LOCALE_KEY_INVALID] = -1,
#define ZEDBSD_LOCALE_CATEGORY(name, category, keyword, c_value, utf8_value)   \
	[ZEDBSD_LOCALE_KEY_##name] = category,
    ZEDBSD_LOCALE_KEYS(ZEDBSD_LOCALE_CATEGORY)
#undef ZEDBSD_LOCALE_CATEGORY
};

static const char *const c_values[ZEDBSD_LOCALE_KEY_COUNT] = {
    [ZEDBSD_LOCALE_KEY_INVALID] = "",
#define ZEDBSD_LOCALE_C_VALUE(name, category, keyword, c_value, utf8_value)    \
	[ZEDBSD_LOCALE_KEY_##name] = c_value,
    ZEDBSD_LOCALE_KEYS(ZEDBSD_LOCALE_C_VALUE)
#undef ZEDBSD_LOCALE_C_VALUE
};

static const char *const utf8_values[ZEDBSD_LOCALE_KEY_COUNT] = {
    [ZEDBSD_LOCALE_KEY_INVALID] = "",
#define ZEDBSD_LOCALE_UTF8_VALUE(name, category, keyword, c_value, utf8_value) \
	[ZEDBSD_LOCALE_KEY_##name] = utf8_value,
    ZEDBSD_LOCALE_KEYS(ZEDBSD_LOCALE_UTF8_VALUE)
#undef ZEDBSD_LOCALE_UTF8_VALUE
};

static struct zed_locale_record locale_c = {
    .name = "C",
};
static struct zed_locale_record locale_utf8 = {
    .name = "C.UTF-8",
    .utf8 = 1,
};

static void
cache_lock(void)
{
	while (__atomic_exchange_n(&locale_cache_lock, 1U, __ATOMIC_ACQUIRE) !=
	       0)
		;
}

static void
cache_unlock(void)
{
	__atomic_store_n(&locale_cache_lock, 0U, __ATOMIC_RELEASE);
}

int
zed_locale_key_category(enum zedbsd_locale_key key)
{
	return key > ZEDBSD_LOCALE_KEY_INVALID && key < ZEDBSD_LOCALE_KEY_COUNT
		   ? key_categories[key]
		   : -1;
}

const char *
zed_locale_record_name(const struct zed_locale_record *record)
{
	return record->name;
}

const char *
zed_locale_record_value(const struct zed_locale_record *record,
			enum zedbsd_locale_key key)
{
	if (key <= ZEDBSD_LOCALE_KEY_INVALID || key >= ZEDBSD_LOCALE_KEY_COUNT)
		return "";
	if (record->values[key] != NULL)
		return record->values[key];
	return record->utf8 ? utf8_values[key] : c_values[key];
}

unsigned
zed_locale_record_utf8(const struct zed_locale_record *record)
{
	return record->utf8;
}

static int
range_valid(size_t size, uint32_t offset, uint32_t length)
{
	return offset <= size && length <= size - offset;
}

static int
record_validate(struct zed_locale_record *record)
{
	const unsigned char *data = record->data;
	uint32_t count;
	uint32_t entries;
	uint32_t strings;
	uint32_t index;
	uint32_t previous = 0;

	if (record->size < ZEDBSD_LOCALE_HEADER_SIZE ||
	    memcmp(data, ZEDBSD_LOCALE_MAGIC, ZEDBSD_LOCALE_MAGIC_SIZE) != 0 ||
	    zedbsd_locale_get32(data + 8U) != ZEDBSD_LOCALE_VERSION ||
	    zedbsd_locale_get32(data + 12U) != ZEDBSD_LOCALE_HEADER_SIZE)
		return 0;
	count = zedbsd_locale_get32(data + 16U);
	entries = zedbsd_locale_get32(data + 20U);
	strings = zedbsd_locale_get32(data + 24U);
	if (count > UINT32_MAX / ZEDBSD_LOCALE_ENTRY_SIZE ||
	    !range_valid(record->size, entries,
			 count * ZEDBSD_LOCALE_ENTRY_SIZE) ||
	    strings < entries + count * ZEDBSD_LOCALE_ENTRY_SIZE ||
	    strings > record->size)
		return 0;
	for (index = 0; index < count; index++) {
		const unsigned char *entry =
		    data + entries + index * ZEDBSD_LOCALE_ENTRY_SIZE;
		uint32_t key = zedbsd_locale_get32(entry);
		uint32_t category = zedbsd_locale_get32(entry + 4U);
		uint32_t offset = zedbsd_locale_get32(entry + 8U);
		uint32_t length = zedbsd_locale_get32(entry + 12U);

		if (key <= previous || key >= ZEDBSD_LOCALE_KEY_COUNT ||
		    category != (uint32_t)key_categories[key] ||
		    offset < strings || length == UINT32_MAX ||
		    !range_valid(record->size, offset, length + 1U) ||
		    data[offset + length] != '\0' ||
		    memchr(data + offset, '\0', length) != NULL)
			return 0;
		record->values[key] = (const char *)data + offset;
		previous = key;
	}
	if (strcmp(zed_locale_record_value(record, ZEDBSD_LOCALE_KEY_CODESET),
		   "UTF-8") == 0)
		record->utf8 = 1;
	else if (strcmp(
		     zed_locale_record_value(record, ZEDBSD_LOCALE_KEY_CODESET),
		     "US-ASCII") != 0)
		return 0;
	return 1;
}

static int
path_copy(char *path, size_t capacity, const char *directory, const char *name)
{
	int length = snprintf(path, capacity, "%s/%s", directory, name);

	return length >= 0 && (size_t)length < capacity;
}

static int
locale_path(char path[PATH_MAX + 1U], const char *name)
{
	const char *locations;
	const char *begin;

	if (strchr(name, '/') != NULL) {
		if (strlen(name) > PATH_MAX)
			return 0;
		(void)strcpy(path, name);
		return 1;
	}
	locations = getenv("LOCPATH");
	if (locations != NULL) {
		begin = locations;
		for (;;) {
			const char *end = strchr(begin, ':');
			size_t length =
			    end != NULL ? (size_t)(end - begin) : strlen(begin);
			char directory[PATH_MAX + 1U];

			if (length <= PATH_MAX) {
				if (length == 0)
					(void)strcpy(directory, ".");
				else {
					memcpy(directory, begin, length);
					directory[length] = '\0';
				}
				if (path_copy(path, PATH_MAX + 1U, directory,
					      name) &&
				    access(path, R_OK) == 0)
					return 1;
			}
			if (end == NULL)
				break;
			begin = end + 1;
		}
	}
	{
		int length = snprintf(path, PATH_MAX + 1U,
				      "/usr/share/locale/%s/locale.zloc", name);

		return length >= 0 && (size_t)length <= PATH_MAX;
	}
}

static struct zed_locale_record *
record_find_locked(const char *name)
{
	unsigned index;

	for (index = 0; index < LOCALE_CACHE_COUNT; index++)
		if (locale_cache[index].used &&
		    strcmp(locale_cache[index].name, name) == 0)
			return &locale_cache[index];
	return NULL;
}

struct zed_locale_record *
zed_locale_record_load(const char *name)
{
	struct zed_locale_record loaded;
	struct zed_locale_record *record;
	char path[PATH_MAX + 1U];
	off_t end;
	size_t done = 0;
	unsigned index;
	int descriptor;

	if (name == NULL)
		return NULL;
	if (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0)
		return &locale_c;
	if (strcmp(name, "C.UTF-8") == 0 || strcmp(name, "C.utf8") == 0 ||
	    strcmp(name, "UTF-8") == 0)
		return &locale_utf8;
	if (strlen(name) >= sizeof(loaded.name) || !locale_path(path, name)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	cache_lock();
	record = record_find_locked(name);
	cache_unlock();
	if (record != NULL)
		return record;
	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return NULL;
	memset(&loaded, 0, sizeof(loaded));
	(void)strcpy(loaded.name, name);
	end = lseek(descriptor, 0, SEEK_END);
	if (end < 0 || (uint64_t)end > UINT32_MAX ||
	    lseek(descriptor, 0, SEEK_SET) != 0)
		goto invalid;
	loaded.size = (size_t)end;
	loaded.data = malloc(loaded.size != 0 ? loaded.size : 1U);
	if (loaded.data == NULL)
		goto failed;
	while (done < loaded.size) {
		ssize_t count =
		    read(descriptor, loaded.data + done, loaded.size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			goto invalid;
		done += (size_t)count;
	}
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto failed;
	}
	descriptor = -1;
	if (!record_validate(&loaded))
		goto invalid;
	cache_lock();
	record = record_find_locked(name);
	if (record == NULL)
		for (index = 0; index < LOCALE_CACHE_COUNT; index++)
			if (!locale_cache[index].used) {
				record = &locale_cache[index];
				*record = loaded;
				record->used = 1;
				break;
			}
	cache_unlock();
	if (record != NULL && record->data != loaded.data)
		free(loaded.data);
	if (record == NULL) {
		free(loaded.data);
		errno = ENOMEM;
	}
	return record;

invalid:
	errno = EINVAL;
failed:
	if (descriptor >= 0)
		(void)close(descriptor);
	free(loaded.data);
	return NULL;
}
