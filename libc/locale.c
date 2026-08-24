/*
 * zedBSD C/POSIX and C.UTF-8 locale core
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "libc/stdio-internal.h"
#include "libc/locale-db.h"
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <libintl.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

struct __locale {
	struct zed_locale_record *categories[6];
	char name[64];
	unsigned allocated;
	unsigned used;
};

static struct __locale locale_c;
static struct __locale locale_utf8;
static struct __locale *global_category[6] = {&locale_c, &locale_c, &locale_c,
					      &locale_c, &locale_c, &locale_c};
static volatile uint32_t locale_lock_word;
static mbstate_t bootstrap_states[3];

#define LOCALE_CATEGORY_COUNT 6U
#define LOCALE_COMPOSITE_LENGTH 384U
#define NAMED_LOCALE_COUNT 16U
#define DOMAIN_BINDING_COUNT 16U
#define DOMAIN_CODESET_LENGTH 64U
#define MESSAGE_CATALOG_COUNT 16U

struct domain_binding {
	char name[TEXTDOMAIN_MAX + 1U];
	char directory[PATH_MAX + 1U];
	char codeset[DOMAIN_CODESET_LENGTH];
	unsigned used;
	unsigned directory_set;
	unsigned codeset_set;
};

struct message_catalog {
	char path[PATH_MAX + 1U];
	unsigned char *data;
	size_t size;
	uint32_t strings;
	uint32_t originals;
	uint32_t translations;
	unsigned swapped;
	unsigned used;
};

static char global_locale_composite[LOCALE_COMPOSITE_LENGTH];
static struct __locale named_locales[NAMED_LOCALE_COUNT];
static struct domain_binding domain_bindings[DOMAIN_BINDING_COUNT];
static struct message_catalog message_catalogs[MESSAGE_CATALOG_COUNT];
static char current_textdomain[TEXTDOMAIN_MAX + 1U] = "messages";
static char default_locale_directory[] = "/usr/share/locale";

extern const void *__pthread_locale_exchange(const void *, int)
    __attribute__((weak));
extern void *__pthread_mbstate(unsigned) __attribute__((weak));
extern void __pthread_cancel_point(void) __attribute__((weak));

static void
locale_cancel_point(void)
{
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}
extern char *getenv(const char *) __attribute__((weak));

static void
locale_lock(void)
{
	while (__atomic_exchange_n(&locale_lock_word, 1U, __ATOMIC_ACQUIRE) !=
	       0)
		;
}

static void
locale_unlock(void)
{
	__atomic_store_n(&locale_lock_word, 0U, __ATOMIC_RELEASE);
}

static void
locale_initialize(void)
{
	unsigned index;

	if (locale_c.used)
		return;
	locale_lock();
	if (!locale_c.used) {
		(void)strcpy(locale_c.name, "C");
		(void)strcpy(locale_utf8.name, "C.UTF-8");
		for (index = 0; index < LOCALE_CATEGORY_COUNT; index++) {
			locale_c.categories[index] =
			    zed_locale_record_load("C");
			locale_utf8.categories[index] =
			    zed_locale_record_load("C.UTF-8");
		}
		locale_c.used = 1;
		locale_utf8.used = 1;
	}
	locale_unlock();
}

static struct __locale *
locale_category(struct __locale *locale, unsigned category)
{
	struct zed_locale_record *record = locale->categories[category];
	unsigned index;

	if (locale == &locale_c || locale == &locale_utf8)
		return locale;
	for (index = 0; index < NAMED_LOCALE_COUNT; index++)
		if (named_locales[index].used &&
		    named_locales[index].categories[category] == record &&
		    named_locales[index].categories[0] == record &&
		    named_locales[index].categories[1] == record &&
		    named_locales[index].categories[2] == record &&
		    named_locales[index].categories[3] == record &&
		    named_locales[index].categories[4] == record &&
		    named_locales[index].categories[5] == record)
			return &named_locales[index];
	return locale;
}

static struct __locale *
locale_named(const char *name)
{
	struct zed_locale_record *record;
	struct __locale *result = NULL;
	unsigned index;

	locale_initialize();
	if (name == NULL)
		return NULL;
	if (name[0] == '\0') {
		const char *environment = NULL;
		if (getenv != NULL) {
			environment = getenv("LC_ALL");
			if (environment == NULL || environment[0] == '\0')
				environment = getenv("LANG");
		}
		name = environment != NULL && environment[0] != '\0'
			   ? environment
			   : "C";
	}
	if (!strcmp(name, "C") || !strcmp(name, "POSIX"))
		return &locale_c;
	if (!strcmp(name, "C.UTF-8") || !strcmp(name, "C.utf8") ||
	    !strcmp(name, "UTF-8"))
		return &locale_utf8;
	record = zed_locale_record_load(name);
	if (record == NULL)
		return NULL;
	locale_lock();
	for (index = 0; index < NAMED_LOCALE_COUNT; index++)
		if (named_locales[index].used &&
		    named_locales[index].categories[0] == record) {
			result = &named_locales[index];
			break;
		}
	if (result == NULL)
		for (index = 0; index < NAMED_LOCALE_COUNT; index++)
			if (!named_locales[index].used) {
				unsigned category;

				result = &named_locales[index];
				memset(result, 0, sizeof(*result));
				for (category = 0;
				     category < LOCALE_CATEGORY_COUNT;
				     category++)
					result->categories[category] = record;
				(void)snprintf(result->name,
					       sizeof(result->name), "%s",
					       name);
				result->used = 1;
				break;
			}
	locale_unlock();
	if (result == NULL)
		errno = ENOMEM;
	return result;
}

static char *
global_locale_name(void)
{
	char *destination = global_locale_composite;
	unsigned index;

	locale_initialize();
	locale_lock();
	for (index = 1; index < LOCALE_CATEGORY_COUNT; index++)
		if (global_category[index] != global_category[0])
			break;
	if (index == LOCALE_CATEGORY_COUNT) {
		locale_unlock();
		return global_category[0]->name;
	}
	for (index = 0; index < LOCALE_CATEGORY_COUNT; index++) {
		size_t length = strlen(global_category[index]->name);

		memcpy(destination, global_category[index]->name, length);
		destination += length;
		if (index + 1U != LOCALE_CATEGORY_COUNT)
			*destination++ = '/';
	}
	*destination = '\0';
	locale_unlock();
	return global_locale_composite;
}

static int
locale_composite_parse(const char *name,
		       struct __locale *categories[LOCALE_CATEGORY_COUNT])
{
	const char *begin = name;
	const char *end;
	unsigned i;

	for (i = 0; i < LOCALE_CATEGORY_COUNT; i++) {
		end = begin;
		while (*end != '\0' && *end != '/')
			end++;
		char component[64];
		size_t length = (size_t)(end - begin);

		if (length == 0 || length >= sizeof(component))
			return 0;
		memcpy(component, begin, length);
		component[length] = '\0';
		categories[i] = locale_named(component);
		if (categories[i] == NULL)
			return 0;
		if (i + 1U == LOCALE_CATEGORY_COUNT)
			return *end == '\0';
		if (*end != '/')
			return 0;
		begin = end + 1;
	}
	return 0;
}

static struct __locale *
effective_locale(void)
{
	const void *local = NULL;
	if (__pthread_locale_exchange != NULL)
		local = __pthread_locale_exchange(NULL, 0);
	locale_initialize();
	return local != NULL
		   ? locale_category((struct __locale *)local, LC_CTYPE)
		   : __atomic_load_n(&global_category[LC_CTYPE],
				     __ATOMIC_ACQUIRE);
}

static struct __locale *
effective_locale_category(int category)
{
	const void *local = NULL;

	locale_initialize();
	if (__pthread_locale_exchange != NULL)
		local = __pthread_locale_exchange(NULL, 0);
	return local != NULL ? locale_category((struct __locale *)local,
					       (unsigned)category)
			     : __atomic_load_n(&global_category[category],
					       __ATOMIC_ACQUIRE);
}

char *
setlocale(int category, const char *name)
{
	struct __locale *categories[LOCALE_CATEGORY_COUNT];
	struct __locale *locale;
	unsigned first, last, i;

	locale_initialize();
	if (category < LC_CTYPE || category > LC_ALL) {
		errno = EINVAL;
		return NULL;
	}
	first = category == LC_ALL ? 0U : (unsigned)category;
	last = category == LC_ALL ? LOCALE_CATEGORY_COUNT : first + 1U;
	if (name == NULL) {
		if (category == LC_ALL)
			return global_locale_name();
		locale =
		    __atomic_load_n(&global_category[first], __ATOMIC_ACQUIRE);
		return (char *)locale->name;
	}
	if (category == LC_ALL && locale_composite_parse(name, categories)) {
		locale_lock();
		for (i = 0; i < LOCALE_CATEGORY_COUNT; i++)
			global_category[i] = categories[i];
		locale_unlock();
		return global_locale_name();
	}
	locale = locale_named(name);
	if (locale == NULL) {
		errno = ENOENT;
		return NULL;
	}
	locale_lock();
	for (i = first; i < last; i++)
		global_category[i] = locale;
	locale_unlock();
	return category == LC_ALL ? global_locale_name() : (char *)locale->name;
}

const char *
getlocalename_l(int category, locale_t locale)
{
	struct __locale *selected;

	if (category < LC_CTYPE || category > LC_ALL || locale == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (locale == LC_GLOBAL_LOCALE) {
		if (category == LC_ALL)
			return global_locale_name();
		selected = __atomic_load_n(&global_category[category],
					   __ATOMIC_ACQUIRE);
		return selected->name;
	}
	if (category == LC_ALL)
		return locale->name;
	return locale_category(locale, (unsigned)category)->name;
}

locale_t
newlocale(int mask, const char *name, locale_t base)
{
	struct __locale *created;
	struct __locale *wanted;
	unsigned category;

	locale_initialize();
	if (mask < 0 || ((unsigned)mask & ~LC_ALL_MASK) != 0 || name == NULL ||
	    base == LC_GLOBAL_LOCALE) {
		errno = EINVAL;
		return NULL;
	}
	wanted = locale_named(name);
	if (wanted == NULL) {
		errno = ENOENT;
		return NULL;
	}
	if (mask == 0)
		return base != NULL ? base : &locale_c;
	if ((unsigned)mask == LC_ALL_MASK)
		return wanted;
	created = calloc(1, sizeof(*created));
	if (created == NULL)
		return NULL;
	for (category = 0; category < LOCALE_CATEGORY_COUNT; category++) {
		struct __locale *source =
		    ((unsigned)mask & (1U << category)) != 0 ? wanted
		    : base != NULL ? locale_category(base, category)
				   : &locale_c;

		created->categories[category] = source->categories[category];
	}
	(void)snprintf(created->name, sizeof(created->name), "%s", name);
	created->allocated = 1;
	created->used = 1;
	return created;
}

locale_t
duplocale(locale_t locale)
{
	struct __locale *copy;
	unsigned category;

	locale_initialize();
	if (locale == NULL) {
		errno = EINVAL;
		return NULL;
	}
	copy = calloc(1, sizeof(*copy));
	if (copy == NULL)
		return NULL;
	if (locale == LC_GLOBAL_LOCALE) {
		for (category = 0; category < LOCALE_CATEGORY_COUNT; category++)
			copy->categories[category] =
			    effective_locale_category((int)category)
				->categories[category];
		(void)strcpy(copy->name, "global");
	} else
		*copy = *locale;
	copy->allocated = 1;
	copy->used = 1;
	return copy;
}

void
freelocale(locale_t locale)
{
	if (locale != NULL && locale != LC_GLOBAL_LOCALE && locale->allocated)
		free(locale);
}

locale_t
uselocale(locale_t locale)
{
	const void *previous;
	if (__pthread_locale_exchange == NULL) {
		if (locale != NULL && locale != LC_GLOBAL_LOCALE) {
			errno = ENOSYS;
			return NULL;
		}
		return LC_GLOBAL_LOCALE;
	}
	previous = __pthread_locale_exchange(
	    locale == LC_GLOBAL_LOCALE ? NULL : locale, locale != NULL);
	return previous != NULL ? (locale_t)previous : LC_GLOBAL_LOCALE;
}

static struct domain_binding *
domain_binding_find_locked(const char *domainname)
{
	unsigned i;

	for (i = 0; i < DOMAIN_BINDING_COUNT; i++)
		if (domain_bindings[i].used &&
		    !strcmp(domain_bindings[i].name, domainname))
			return &domain_bindings[i];
	return NULL;
}

static struct domain_binding *
domain_binding_create_locked(const char *domainname)
{
	struct domain_binding *binding;
	unsigned i;

	binding = domain_binding_find_locked(domainname);
	if (binding != NULL)
		return binding;
	for (i = 0; i < DOMAIN_BINDING_COUNT; i++) {
		binding = &domain_bindings[i];
		if (!binding->used) {
			strcpy(binding->name, domainname);
			binding->used = 1;
			return binding;
		}
	}
	return NULL;
}

char *
bindtextdomain(const char *domainname, const char *dirname)
{
	struct domain_binding *binding;
	char *result;
	int saved_errno;

	locale_cancel_point();
	if (domainname == NULL || domainname[0] == '\0') {
		saved_errno = errno;
		errno = saved_errno;
		return NULL;
	}
	if (strlen(domainname) > TEXTDOMAIN_MAX) {
		errno = EINVAL;
		return NULL;
	}
	if (dirname != NULL && strlen(dirname) > PATH_MAX) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	locale_lock();
	binding = domain_binding_find_locked(domainname);
	if (dirname == NULL) {
		result = binding != NULL && binding->directory_set
			     ? binding->directory
			     : default_locale_directory;
		locale_unlock();
		return result;
	}
	binding = domain_binding_create_locked(domainname);
	if (binding == NULL) {
		locale_unlock();
		errno = ENOMEM;
		return NULL;
	}
	strcpy(binding->directory, dirname);
	binding->directory_set = 1;
	result = binding->directory;
	locale_unlock();
	return result;
}

char *
bind_textdomain_codeset(const char *domainname, const char *codeset)
{
	struct domain_binding *binding;
	char *result;
	int saved_errno;

	locale_cancel_point();
	if (domainname == NULL || domainname[0] == '\0') {
		saved_errno = errno;
		errno = saved_errno;
		return NULL;
	}
	if (strlen(domainname) > TEXTDOMAIN_MAX) {
		errno = EINVAL;
		return NULL;
	}
	if (codeset != NULL && strlen(codeset) >= DOMAIN_CODESET_LENGTH) {
		errno = EINVAL;
		return NULL;
	}
	locale_lock();
	binding = domain_binding_find_locked(domainname);
	if (codeset == NULL) {
		result = binding != NULL && binding->codeset_set
			     ? binding->codeset
			     : NULL;
		locale_unlock();
		return result;
	}
	binding = domain_binding_create_locked(domainname);
	if (binding == NULL) {
		locale_unlock();
		errno = ENOMEM;
		return NULL;
	}
	strcpy(binding->codeset, codeset);
	binding->codeset_set = 1;
	result = binding->codeset;
	locale_unlock();
	return result;
}

char *
textdomain(const char *domainname)
{
	char *result;

	locale_lock();
	if (domainname == NULL) {
		result = current_textdomain;
		locale_unlock();
		return result;
	}
	if (domainname[0] == '\0')
		domainname = "messages";
	if (strlen(domainname) > TEXTDOMAIN_MAX) {
		locale_unlock();
		errno = EINVAL;
		return NULL;
	}
	strcpy(current_textdomain, domainname);
	result = current_textdomain;
	locale_unlock();
	return result;
}

static uint32_t
catalog_word(const unsigned char *data, unsigned swapped)
{
	if (swapped)
		return (uint32_t)data[3] | (uint32_t)data[2] << 8 |
		       (uint32_t)data[1] << 16 | (uint32_t)data[0] << 24;
	return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
	       (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static int
catalog_range_valid(size_t size, uint32_t offset, uint32_t count,
		    size_t element_size)
{
	return offset <= size && count <= (size - offset) / element_size;
}

static int
catalog_validate(struct message_catalog *catalog)
{
	const unsigned char *data = catalog->data;
	uint32_t magic, revision;
	uint32_t index;

	if (catalog->size < 28U)
		return 0;
	magic = catalog_word(data, 0);
	if (magic == 0x950412deU)
		catalog->swapped = 0;
	else if (magic == 0xde120495U)
		catalog->swapped = 1;
	else
		return 0;
	revision = catalog_word(data + 4U, catalog->swapped);
	if ((revision >> 16) > 1U)
		return 0;
	catalog->strings = catalog_word(data + 8U, catalog->swapped);
	catalog->originals = catalog_word(data + 12U, catalog->swapped);
	catalog->translations = catalog_word(data + 16U, catalog->swapped);
	if (!catalog_range_valid(catalog->size, catalog->originals,
				 catalog->strings, 8U) ||
	    !catalog_range_valid(catalog->size, catalog->translations,
				 catalog->strings, 8U))
		return 0;
	for (index = 0; index < catalog->strings; index++) {
		uint32_t original_length = catalog_word(
		    data + catalog->originals + index * 8U, catalog->swapped);
		uint32_t original_offset =
		    catalog_word(data + catalog->originals + index * 8U + 4U,
				 catalog->swapped);
		uint32_t translated_length =
		    catalog_word(data + catalog->translations + index * 8U,
				 catalog->swapped);
		uint32_t translated_offset =
		    catalog_word(data + catalog->translations + index * 8U + 4U,
				 catalog->swapped);

		if (!catalog_range_valid(catalog->size, original_offset,
					 original_length, 1U) ||
		    (size_t)original_offset + original_length >=
			catalog->size ||
		    data[original_offset + original_length] != 0 ||
		    !catalog_range_valid(catalog->size, translated_offset,
					 translated_length, 1U) ||
		    (size_t)translated_offset + translated_length >=
			catalog->size ||
		    data[translated_offset + translated_length] != 0)
			return 0;
	}
	return 1;
}

static struct message_catalog *
catalog_cached_locked(const char *path)
{
	unsigned index;

	for (index = 0; index < MESSAGE_CATALOG_COUNT; index++)
		if (message_catalogs[index].used &&
		    !strcmp(message_catalogs[index].path, path))
			return &message_catalogs[index];
	return NULL;
}

static struct message_catalog *
catalog_load(const char *path)
{
	struct message_catalog loaded;
	struct message_catalog *catalog;
	off_t end;
	ssize_t count;
	size_t done = 0;
	unsigned index;
	int descriptor;

	locale_lock();
	catalog = catalog_cached_locked(path);
	locale_unlock();
	if (catalog != NULL)
		return catalog;
	memset(&loaded, 0, sizeof(loaded));
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return NULL;
	end = lseek(descriptor, 0, SEEK_END);
	if (end < 0 || (uint64_t)end > SIZE_MAX ||
	    lseek(descriptor, 0, SEEK_SET) != 0) {
		(void)close(descriptor);
		return NULL;
	}
	loaded.size = (size_t)end;
	loaded.data = malloc(loaded.size != 0 ? loaded.size : 1U);
	if (loaded.data == NULL) {
		(void)close(descriptor);
		return NULL;
	}
	while (done < loaded.size) {
		count =
		    read(descriptor, loaded.data + done, loaded.size - done);
		if (count <= 0) {
			free(loaded.data);
			(void)close(descriptor);
			return NULL;
		}
		done += (size_t)count;
	}
	if (close(descriptor) != 0 || !catalog_validate(&loaded)) {
		free(loaded.data);
		return NULL;
	}
	locale_lock();
	catalog = catalog_cached_locked(path);
	if (catalog != NULL) {
		locale_unlock();
		free(loaded.data);
		return catalog;
	}
	for (index = 0; index < MESSAGE_CATALOG_COUNT; index++) {
		if (!message_catalogs[index].used) {
			catalog = &message_catalogs[index];
			*catalog = loaded;
			strcpy(catalog->path, path);
			catalog->used = 1;
			locale_unlock();
			return catalog;
		}
	}
	locale_unlock();
	free(loaded.data);
	return NULL;
}

static struct __locale *
message_locale(int category, locale_t locale)
{
	const void *local;

	if (category < LC_CTYPE || category >= LC_ALL)
		return NULL;
	if (locale != LC_GLOBAL_LOCALE)
		return locale_category(locale, (unsigned)category);
	local = __pthread_locale_exchange != NULL
		    ? __pthread_locale_exchange(NULL, 0)
		    : NULL;
	if (local != NULL)
		return locale_category((struct __locale *)local,
				       (unsigned)category);
	return __atomic_load_n(&global_category[category], __ATOMIC_ACQUIRE);
}

static int
message_catalog_path(char *path, size_t size, const char *domainname,
		     struct __locale *locale)
{
	struct domain_binding *binding;
	const char *directory;
	int length;

	locale_lock();
	binding = domain_binding_find_locked(domainname);
	directory = binding != NULL && binding->directory_set
			? binding->directory
			: default_locale_directory;
	length = snprintf(path, size, "%s/%s/LC_MESSAGES/%s.mo", directory,
			  locale->name, domainname);
	locale_unlock();
	return length >= 0 && (size_t)length < size;
}

static const char *
catalog_translation(struct message_catalog *catalog, const char *identifier,
		    unsigned long int number, int plural)
{
	const unsigned char *data = catalog->data;
	size_t identifier_length = strlen(identifier);
	uint32_t index;

	for (index = 0; index < catalog->strings; index++) {
		uint32_t length = catalog_word(
		    data + catalog->originals + index * 8U, catalog->swapped);
		uint32_t offset =
		    catalog_word(data + catalog->originals + index * 8U + 4U,
				 catalog->swapped);
		uint32_t translated_length;
		uint32_t translated_offset;
		const char *translated;
		unsigned form;

		if (length < identifier_length ||
		    memcmp(data + offset, identifier, identifier_length) != 0 ||
		    (length != identifier_length &&
		     data[offset + identifier_length] != 0))
			continue;
		translated_length =
		    catalog_word(data + catalog->translations + index * 8U,
				 catalog->swapped);
		translated_offset =
		    catalog_word(data + catalog->translations + index * 8U + 4U,
				 catalog->swapped);
		translated = (const char *)data + translated_offset;
		form = plural && number != 1 ? 1U : 0U;
		while (form != 0) {
			size_t current = strlen(translated) + 1U;

			if (current > translated_length)
				return NULL;
			translated += current;
			translated_length -= (uint32_t)current;
			form--;
		}
		return translated[0] != '\0' ? translated : NULL;
	}
	return NULL;
}

static char *
message_translate(const char *domainname, const char *identifier,
		  const char *plural_identifier, unsigned long int number,
		  int category, locale_t locale)
{
	struct message_catalog *catalog;
	struct __locale *selected;
	const char *translated;
	char domain[TEXTDOMAIN_MAX + 1U];
	char path[PATH_MAX + 1U];
	int saved_errno = errno;

	locale_cancel_point();
	selected = message_locale(category, locale);
	if (selected == NULL || identifier == NULL)
		return (char *)(number == 1 || plural_identifier == NULL
				    ? identifier
				    : plural_identifier);
	locale_lock();
	if (domainname == NULL)
		strcpy(domain, current_textdomain);
	else if (strlen(domainname) <= TEXTDOMAIN_MAX)
		strcpy(domain, domainname);
	else
		domain[0] = '\0';
	locale_unlock();
	if (domain[0] == '\0' ||
	    !message_catalog_path(path, sizeof(path), domain, selected))
		goto fallback;
	catalog = catalog_load(path);
	if (catalog != NULL) {
		translated = catalog_translation(catalog, identifier, number,
						 plural_identifier != NULL);
		if (translated != NULL) {
			errno = saved_errno;
			return (char *)translated;
		}
	}
fallback:
	errno = saved_errno;
	return (char *)(number == 1 || plural_identifier == NULL
			    ? identifier
			    : plural_identifier);
}

char *
dcgettext_l(const char *domainname, const char *msgid, int category,
	    locale_t locale)
{
	return message_translate(domainname, msgid, NULL, 1, category, locale);
}

char *
dcgettext(const char *domainname, const char *msgid, int category)
{
	return dcgettext_l(domainname, msgid, category, LC_GLOBAL_LOCALE);
}

char *
dgettext(const char *domainname, const char *msgid)
{
	return dcgettext(domainname, msgid, LC_MESSAGES);
}

char *
dgettext_l(const char *domainname, const char *msgid, locale_t locale)
{
	return dcgettext_l(domainname, msgid, LC_MESSAGES, locale);
}

char *
gettext(const char *msgid)
{
	return dgettext(NULL, msgid);
}

char *
gettext_l(const char *msgid, locale_t locale)
{
	return dgettext_l(NULL, msgid, locale);
}

char *
dcngettext_l(const char *domainname, const char *msgid1, const char *msgid2,
	     unsigned long int n, int category, locale_t locale)
{
	return message_translate(domainname, msgid1, msgid2, n, category,
				 locale);
}

char *
dcngettext(const char *domainname, const char *msgid1, const char *msgid2,
	   unsigned long int n, int category)
{
	return dcngettext_l(domainname, msgid1, msgid2, n, category,
			    LC_GLOBAL_LOCALE);
}

char *
dngettext(const char *domainname, const char *msgid1, const char *msgid2,
	  unsigned long int n)
{
	return dcngettext(domainname, msgid1, msgid2, n, LC_MESSAGES);
}

char *
dngettext_l(const char *domainname, const char *msgid1, const char *msgid2,
	    unsigned long int n, locale_t locale)
{
	return dcngettext_l(domainname, msgid1, msgid2, n, LC_MESSAGES, locale);
}

char *
ngettext(const char *msgid1, const char *msgid2, unsigned long int n)
{
	return dngettext(NULL, msgid1, msgid2, n);
}

char *
ngettext_l(const char *msgid1, const char *msgid2, unsigned long int n,
	   locale_t locale)
{
	return dngettext_l(NULL, msgid1, msgid2, n, locale);
}

size_t
__libc_mb_cur_max(void)
{
	return zed_locale_record_utf8(effective_locale()->categories[LC_CTYPE])
		   ? 4U
		   : 1U;
}

static char
locale_numeric_byte(const char *text)
{
	char *end;
	long value;

	if (text == NULL || *text == '\0')
		return CHAR_MAX;
	errno = 0;
	value = strtol(text, &end, 10);
	return errno == 0 && *end == '\0' && value >= 0 && value <= CHAR_MAX
		   ? (char)value
		   : CHAR_MAX;
}

struct lconv *
localeconv(void)
{
	static struct lconv value;
	struct zed_locale_record *numeric =
	    effective_locale_category(LC_NUMERIC)->categories[LC_NUMERIC];
	struct zed_locale_record *monetary =
	    effective_locale_category(LC_MONETARY)->categories[LC_MONETARY];

#define LOCALE_VALUE(record, key)                                              \
	((char *)zed_locale_record_value((record), ZEDBSD_LOCALE_KEY_##key))
	value.decimal_point = LOCALE_VALUE(numeric, DECIMAL_POINT);
	value.thousands_sep = LOCALE_VALUE(numeric, THOUSANDS_SEP);
	value.grouping = LOCALE_VALUE(numeric, GROUPING);
	value.int_curr_symbol = LOCALE_VALUE(monetary, INT_CURR_SYMBOL);
	value.currency_symbol = LOCALE_VALUE(monetary, CURRENCY_SYMBOL);
	value.mon_decimal_point = LOCALE_VALUE(monetary, MON_DECIMAL_POINT);
	value.mon_thousands_sep = LOCALE_VALUE(monetary, MON_THOUSANDS_SEP);
	value.mon_grouping = LOCALE_VALUE(monetary, MON_GROUPING);
	value.positive_sign = LOCALE_VALUE(monetary, POSITIVE_SIGN);
	value.negative_sign = LOCALE_VALUE(monetary, NEGATIVE_SIGN);
	value.int_frac_digits =
	    locale_numeric_byte(LOCALE_VALUE(monetary, INT_FRAC_DIGITS));
	value.frac_digits =
	    locale_numeric_byte(LOCALE_VALUE(monetary, FRAC_DIGITS));
	value.p_cs_precedes =
	    locale_numeric_byte(LOCALE_VALUE(monetary, P_CS_PRECEDES));
	value.p_sep_by_space =
	    locale_numeric_byte(LOCALE_VALUE(monetary, P_SEP_BY_SPACE));
	value.n_cs_precedes =
	    locale_numeric_byte(LOCALE_VALUE(monetary, N_CS_PRECEDES));
	value.n_sep_by_space =
	    locale_numeric_byte(LOCALE_VALUE(monetary, N_SEP_BY_SPACE));
	value.p_sign_posn =
	    locale_numeric_byte(LOCALE_VALUE(monetary, P_SIGN_POSN));
	value.n_sign_posn =
	    locale_numeric_byte(LOCALE_VALUE(monetary, N_SIGN_POSN));
#undef LOCALE_VALUE
	return &value;
}

char *
nl_langinfo(nl_item item)
{
	static char empty[] = "";
	int category = zed_locale_key_category((enum zedbsd_locale_key)item);
	struct __locale *locale;

	if (category < 0)
		return empty;
	locale = effective_locale_category(category);
	return (char *)zed_locale_record_value(locale->categories[category],
					       (enum zedbsd_locale_key)item);
}

int
strcoll(const char *a, const char *b)
{
	return strcmp(a, b);
}

size_t
strxfrm(char *destination, const char *source, size_t count)
{
	size_t length = strlen(source);
	if (destination != NULL && count != 0) {
		size_t copied = length < count - 1U ? length : count - 1U;
		memcpy(destination, source, copied);
		destination[copied] = '\0';
	}
	return length;
}

static mbstate_t *
internal_state(unsigned which)
{
	void *state =
	    __pthread_mbstate != NULL ? __pthread_mbstate(which) : NULL;
	return state != NULL ? (mbstate_t *)state : &bootstrap_states[which];
}

void *
__libc_internal_mbstate(unsigned which)
{
	return which < 3U ? internal_state(which) : NULL;
}

int
mbsinit(const mbstate_t *state)
{
	return state == NULL || state->needed == 0;
}

size_t
mbrtowc(wchar_t *result, const char *bytes, size_t count, mbstate_t *state)
{
	mbstate_t *s = state != NULL ? state : internal_state(0);
	size_t used = 0;
	uint32_t value;
	unsigned total;

	if (bytes == NULL) {
		memset(s, 0, sizeof(*s));
		return 0;
	}
	if (!zed_locale_record_utf8(effective_locale()->categories[LC_CTYPE])) {
		unsigned char byte;
		memset(s, 0, sizeof(*s));
		if (count == 0)
			return (size_t)-2;
		byte = (unsigned char)bytes[0];
		if (byte > 0x7fU) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		if (result != NULL)
			*result = (wchar_t)byte;
		return byte == 0 ? 0 : 1;
	}
	if (s->needed == 0) {
		unsigned char first;
		if (count == 0)
			return (size_t)-2;
		first = (unsigned char)bytes[used++];
		if (first < 0x80U) {
			if (result != NULL)
				*result = (wchar_t)first;
			return first == 0 ? 0 : 1;
		}
		if (first >= 0xc2U && first <= 0xdfU) {
			s->needed = 2;
			s->value = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			s->needed = 3;
			s->value = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			s->needed = 4;
			s->value = first & 0x07U;
		} else {
			memset(s, 0, sizeof(*s));
			errno = EILSEQ;
			return (size_t)-1;
		}
		s->seen = 1;
	}
	total = s->needed;
	while (s->seen < s->needed && used < count) {
		unsigned char byte = (unsigned char)bytes[used];
		if ((byte & 0xc0U) != 0x80U) {
			memset(s, 0, sizeof(*s));
			errno = EILSEQ;
			return (size_t)-1;
		}
		s->value = (s->value << 6) | (byte & 0x3fU);
		s->seen++;
		used++;
	}
	if (s->seen != s->needed)
		return (size_t)-2;
	value = s->value;
	if ((total == 2 && value < 0x80U) || (total == 3 && value < 0x800U) ||
	    (total == 4 && value < 0x10000U) || value > 0x10ffffU ||
	    (value >= 0xd800U && value <= 0xdfffU)) {
		memset(s, 0, sizeof(*s));
		errno = EILSEQ;
		return (size_t)-1;
	}
	memset(s, 0, sizeof(*s));
	if (result != NULL)
		*result = (wchar_t)value;
	return used;
}

size_t
mbrlen(const char *s, size_t n, mbstate_t *st)
{
	return mbrtowc(NULL, s, n, st);
}

size_t
wcrtomb(char *bytes, wchar_t character, mbstate_t *state)
{
	mbstate_t *s = state != NULL ? state : internal_state(1);
	uint32_t value = (uint32_t)character;
	memset(s, 0, sizeof(*s));
	if (bytes == NULL)
		return 1;
	if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU) ||
	    (!zed_locale_record_utf8(
		 effective_locale()->categories[LC_CTYPE]) &&
	     value > 0x7fU)) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	if (value < 0x80U) {
		bytes[0] = (char)value;
		return 1;
	}
	if (value < 0x800U) {
		bytes[0] = (char)(0xc0U | (value >> 6));
		bytes[1] = (char)(0x80U | (value & 0x3fU));
		return 2;
	}
	if (value < 0x10000U) {
		bytes[0] = (char)(0xe0U | (value >> 12));
		bytes[1] = (char)(0x80U | ((value >> 6) & 0x3fU));
		bytes[2] = (char)(0x80U | (value & 0x3fU));
		return 3;
	}
	bytes[0] = (char)(0xf0U | (value >> 18));
	bytes[1] = (char)(0x80U | ((value >> 12) & 0x3fU));
	bytes[2] = (char)(0x80U | ((value >> 6) & 0x3fU));
	bytes[3] = (char)(0x80U | (value & 0x3fU));
	return 4;
}

wint_t
btowc(int byte)
{
	if (byte == EOF || (unsigned)byte > 0x7fU)
		return WEOF;
	return (wint_t)(unsigned char)byte;
}

int
wctob(wint_t value)
{
	return value <= 0x7fU ? (int)value : EOF;
}

size_t
mbsrtowcs(wchar_t *destination, const char **source, size_t count,
	  mbstate_t *state)
{
	const char *input;
	size_t output = 0;
	mbstate_t local;
	if (source == NULL || *source == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}
	input = *source;
	if (state == NULL) {
		memset(&local, 0, sizeof(local));
		state = &local;
	}
	for (;;) {
		wchar_t value;
		size_t available = strlen(input) + 1U;
		size_t used = mbrtowc(&value, input, available, state);
		if (used == (size_t)-1 || used == (size_t)-2) {
			*source = input;
			return (size_t)-1;
		}
		if (value == 0) {
			if (destination != NULL && output < count)
				destination[output] = 0;
			*source = NULL;
			return output;
		}
		if (destination != NULL) {
			if (output == count) {
				*source = input;
				return output;
			}
			destination[output] = value;
		}
		output++;
		input += used;
	}
}

size_t
wcsrtombs(char *destination, const wchar_t **source, size_t count,
	  mbstate_t *state)
{
	const wchar_t *input;
	size_t output = 0;
	char encoded[4];
	mbstate_t local;
	if (source == NULL || *source == NULL) {
		errno = EINVAL;
		return (size_t)-1;
	}
	input = *source;
	if (state == NULL) {
		memset(&local, 0, sizeof(local));
		state = &local;
	}
	while (*input != 0) {
		size_t bytes = wcrtomb(encoded, *input, state);
		if (bytes == (size_t)-1) {
			*source = input;
			return (size_t)-1;
		}
		if (destination != NULL) {
			if (bytes > count - output) {
				*source = input;
				return output;
			}
			memcpy(destination + output, encoded, bytes);
		}
		output += bytes;
		input++;
	}
	if (destination != NULL && output < count)
		destination[output] = '\0';
	*source = NULL;
	return output;
}
