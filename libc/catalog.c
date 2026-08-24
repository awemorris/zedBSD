/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "libc/include/nl_types.h"
#include "libc/include/zedbsd/catalog-format.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct __nl_catalog {
	unsigned char *data;
	size_t size;
	uint32_t count;
	uint32_t entries;
};

static int
range_valid(size_t size, uint32_t offset, uint32_t length)
{
	return offset <= size && length <= size - offset;
}

static int
catalog_valid(struct __nl_catalog *catalog)
{
	const unsigned char *data = catalog->data;
	uint32_t strings;
	uint32_t total;
	uint32_t index;
	uint32_t previous_set = 0;
	uint32_t previous_message = 0;

	if (catalog->size < ZEDBSD_CATALOG_HEADER_SIZE ||
	    memcmp(data, ZEDBSD_CATALOG_MAGIC, ZEDBSD_CATALOG_MAGIC_SIZE) !=
		0 ||
	    zedbsd_catalog_get32(data + 8U) != ZEDBSD_CATALOG_VERSION ||
	    zedbsd_catalog_get32(data + 12U) != ZEDBSD_CATALOG_HEADER_SIZE)
		return 0;
	catalog->count = zedbsd_catalog_get32(data + 16U);
	catalog->entries = zedbsd_catalog_get32(data + 20U);
	strings = zedbsd_catalog_get32(data + 24U);
	if (catalog->count > UINT32_MAX / ZEDBSD_CATALOG_ENTRY_SIZE ||
	    !range_valid(catalog->size, catalog->entries,
			 catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE) ||
	    strings <
		catalog->entries + catalog->count * ZEDBSD_CATALOG_ENTRY_SIZE ||
	    strings > catalog->size)
		return 0;
	total = (uint32_t)catalog->size;
	if ((size_t)total != catalog->size)
		return 0;
	for (index = 0; index < catalog->count; index++) {
		const unsigned char *entry =
		    data + catalog->entries + index * ZEDBSD_CATALOG_ENTRY_SIZE;
		uint32_t set = zedbsd_catalog_get32(entry);
		uint32_t message = zedbsd_catalog_get32(entry + 4U);
		uint32_t offset = zedbsd_catalog_get32(entry + 8U);
		uint32_t length = zedbsd_catalog_get32(entry + 12U);

		if (set == 0 || message == 0 || offset < strings ||
		    length == UINT32_MAX ||
		    !range_valid(total, offset, length + 1U) ||
		    data[offset + length] != '\0' ||
		    (index != 0 &&
		     (set < previous_set ||
		      (set == previous_set && message <= previous_message))))
			return 0;
		previous_set = set;
		previous_message = message;
	}
	return 1;
}

static nl_catd
catalog_load(const char *path)
{
	struct __nl_catalog *catalog;
	off_t end;
	size_t done = 0;
	int descriptor;

	descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return (nl_catd)-1;
	end = lseek(descriptor, 0, SEEK_END);
	if (end < 0 || (uint64_t)end > UINT32_MAX ||
	    lseek(descriptor, 0, SEEK_SET) != 0) {
		(void)close(descriptor);
		errno = EINVAL;
		return (nl_catd)-1;
	}
	catalog = calloc(1, sizeof(*catalog));
	if (catalog == NULL) {
		(void)close(descriptor);
		return (nl_catd)-1;
	}
	catalog->size = (size_t)end;
	catalog->data = malloc(catalog->size != 0 ? catalog->size : 1U);
	if (catalog->data == NULL)
		goto failed;
	while (done < catalog->size) {
		ssize_t count = read(descriptor, catalog->data + done,
				     catalog->size - done);

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
	if (!catalog_valid(catalog))
		goto invalid;
	return catalog;

invalid:
	errno = EINVAL;
failed:
	if (descriptor >= 0)
		(void)close(descriptor);
	free(catalog->data);
	free(catalog);
	return (nl_catd)-1;
}

static int
path_expand(char *result, size_t capacity, const char *pattern,
	    const char *name, const char *locale)
{
	char language[64];
	char territory[64];
	char codeset[64];
	size_t used = 0;
	const char *cursor;
	const char *locale_end;
	const char *underscore;
	const char *dot;
	size_t length;

	locale_end = strchr(locale, '@');
	if (locale_end == NULL)
		locale_end = locale + strlen(locale);
	underscore = memchr(locale, '_', (size_t)(locale_end - locale));
	dot = memchr(locale, '.', (size_t)(locale_end - locale));
	if (underscore != NULL && dot != NULL && underscore > dot)
		underscore = NULL;
	length = (size_t)((underscore != NULL ? underscore
			   : dot != NULL      ? dot
					      : locale_end) -
			  locale);
	if (length >= sizeof(language))
		return 0;
	memcpy(language, locale, length);
	language[length] = '\0';
	if (underscore != NULL) {
		const char *end = dot != NULL ? dot : locale_end;

		length = (size_t)(end - underscore - 1);
		if (length >= sizeof(territory))
			return 0;
		memcpy(territory, underscore + 1, length);
		territory[length] = '\0';
	} else
		territory[0] = '\0';
	if (dot != NULL) {
		length = (size_t)(locale_end - dot - 1);
		if (length >= sizeof(codeset))
			return 0;
		memcpy(codeset, dot + 1, length);
		codeset[length] = '\0';
	} else
		codeset[0] = '\0';

	for (cursor = pattern; *cursor != '\0'; cursor++) {
		const char *text = NULL;
		char literal[2] = {*cursor, '\0'};

		if (*cursor == '%' && cursor[1] != '\0') {
			cursor++;
			if (*cursor == 'N')
				text = name;
			else if (*cursor == 'L')
				text = locale;
			else if (*cursor == 'l')
				text = language;
			else if (*cursor == 't')
				text = territory;
			else if (*cursor == 'c')
				text = codeset;
			else if (*cursor == '%')
				text = "%";
			else {
				if (used + 2U >= capacity)
					return 0;
				result[used++] = '%';
				literal[0] = *cursor;
			}
		}
		if (text == NULL)
			text = literal;
		if (strlen(text) >= capacity - used)
			return 0;
		memcpy(result + used, text, strlen(text));
		used += strlen(text);
	}
	result[used] = '\0';
	return 1;
}

nl_catd
catopen(const char *name, int flag)
{
	static const char default_path[] =
	    "/usr/share/nls/%L/%N.cat:/usr/share/nls/%N/%L";
	const char *locale;
	const char *paths;
	const char *begin;
	char pattern[PATH_MAX + 1U];
	char path[PATH_MAX + 1U];

	if (name == NULL || *name == '\0') {
		errno = EINVAL;
		return (nl_catd)-1;
	}
	if (strchr(name, '/') != NULL)
		return catalog_load(name);
	locale = flag == NL_CAT_LOCALE ? setlocale(LC_MESSAGES, NULL)
				       : getenv("LANG");
	if (locale == NULL || *locale == '\0')
		locale = "C";
	paths = getenv("NLSPATH");
	if (paths == NULL)
		paths = default_path;
	begin = paths;
	for (;;) {
		const char *end = strchr(begin, ':');
		size_t length =
		    end != NULL ? (size_t)(end - begin) : strlen(begin);

		if (length == 0) {
			if (strlen(name) <= PATH_MAX) {
				nl_catd catalog;

				(void)strcpy(path, name);
				catalog = catalog_load(path);
				if (catalog != (nl_catd)-1)
					return catalog;
			}
		} else if (length <= PATH_MAX) {
			memcpy(pattern, begin, length);
			pattern[length] = '\0';
			if (path_expand(path, sizeof(path), pattern, name,
					locale)) {
				nl_catd catalog = catalog_load(path);

				if (catalog != (nl_catd)-1)
					return catalog;
			}
		}
		if (end == NULL)
			break;
		begin = end + 1;
	}
	errno = ENOENT;
	return (nl_catd)-1;
}

char *
catgets(nl_catd catalog, int set, int message, const char *fallback)
{
	uint32_t low = 0;
	uint32_t high;

	if (catalog == NULL || catalog == (nl_catd)-1 || set <= 0 ||
	    message <= 0) {
		errno = EBADF;
		return (char *)fallback;
	}
	high = catalog->count;
	while (low < high) {
		uint32_t middle = low + (high - low) / 2U;
		const unsigned char *entry = catalog->data + catalog->entries +
					     middle * ZEDBSD_CATALOG_ENTRY_SIZE;
		uint32_t entry_set = zedbsd_catalog_get32(entry);
		uint32_t entry_message = zedbsd_catalog_get32(entry + 4U);

		if (entry_set < (uint32_t)set ||
		    (entry_set == (uint32_t)set &&
		     entry_message < (uint32_t)message))
			low = middle + 1U;
		else
			high = middle;
	}
	if (low < catalog->count) {
		const unsigned char *entry = catalog->data + catalog->entries +
					     low * ZEDBSD_CATALOG_ENTRY_SIZE;

		if (zedbsd_catalog_get32(entry) == (uint32_t)set &&
		    zedbsd_catalog_get32(entry + 4U) == (uint32_t)message)
			return (char *)catalog->data +
			       zedbsd_catalog_get32(entry + 8U);
	}
	errno = ENOMSG;
	return (char *)fallback;
}

int
catclose(nl_catd catalog)
{
	if (catalog == NULL || catalog == (nl_catd)-1) {
		errno = EBADF;
		return -1;
	}
	free(catalog->data);
	free(catalog);
	return 0;
}
