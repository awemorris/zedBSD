/*
 * zedBSD C/POSIX and C.UTF-8 locale core
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "libc/stdio-internal.h"
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
	const char *name;
	unsigned utf8;
};

static struct __locale locale_c = { "C", 0 };
static struct __locale locale_utf8 = { "C.UTF-8", 1 };
static struct __locale *global_category[6] = {
	&locale_c, &locale_c, &locale_c, &locale_c, &locale_c, &locale_c
};
static volatile uint32_t locale_lock_word;
static mbstate_t bootstrap_states[3];

#define LOCALE_CATEGORY_COUNT	6U
#define LOCALE_COMPOSITE_LENGTH	48U
#define DOMAIN_BINDING_COUNT	16U
#define DOMAIN_CODESET_LENGTH	64U
#define MESSAGE_CATALOG_COUNT	16U

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

static char locale_composites[1U << LOCALE_CATEGORY_COUNT]
    [LOCALE_COMPOSITE_LENGTH];
static unsigned char locale_composite_ready[1U << LOCALE_CATEGORY_COUNT];
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
	while (__atomic_exchange_n(&locale_lock_word, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		;
}

static void
locale_unlock(void)
{
	__atomic_store_n(&locale_lock_word, 0U, __ATOMIC_RELEASE);
}

static struct __locale *
locale_named(const char *name)
{
	if (name == NULL)
		return NULL;
	if (name[0] == '\0') {
		const char *environment = NULL;
		if (getenv != NULL) {
			environment = getenv("LC_ALL");
			if (environment == NULL || environment[0] == '\0')
				environment = getenv("LANG");
		}
		name = environment != NULL && environment[0] != '\0' ?
		    environment : "C";
	}
	if (!strcmp(name, "C") || !strcmp(name, "POSIX"))
		return &locale_c;
	if (!strcmp(name, "C.UTF-8") || !strcmp(name, "C.utf8") ||
	    !strcmp(name, "UTF-8"))
		return &locale_utf8;
	return NULL;
}

static unsigned
global_locale_mask_locked(void)
{
	unsigned i;
	unsigned mask = 0;

	for (i = 0; i < LOCALE_CATEGORY_COUNT; i++)
		if (global_category[i]->utf8)
			mask |= 1U << i;
	return mask;
}

static char *
locale_composite_locked(unsigned mask)
{
	char *result = locale_composites[mask];
	char *destination = result;
	unsigned i;

	if (locale_composite_ready[mask])
		return result;
	for (i = 0; i < LOCALE_CATEGORY_COUNT; i++) {
		const char *name = (mask & (1U << i)) != 0 ? "C.UTF-8" : "C";
		while (*name != '\0')
			*destination++ = *name++;
		if (i + 1U != LOCALE_CATEGORY_COUNT)
			*destination++ = '/';
	}
	*destination = '\0';
	locale_composite_ready[mask] = 1;
	return result;
}

static char *
global_locale_name(void)
{
	char *result;
	unsigned mask;

	locale_lock();
	mask = global_locale_mask_locked();
	if (mask == 0)
		result = (char *)locale_c.name;
	else if (mask == (1U << LOCALE_CATEGORY_COUNT) - 1U)
		result = (char *)locale_utf8.name;
	else
		result = locale_composite_locked(mask);
	locale_unlock();
	return result;
}

static int
locale_composite_parse(
	const char *name,
	struct __locale *categories[LOCALE_CATEGORY_COUNT])
{
	const char *begin = name;
	const char *end;
	unsigned i;

	for (i = 0; i < LOCALE_CATEGORY_COUNT; i++) {
		end = begin;
		while (*end != '\0' && *end != '/')
			end++;
		if (end - begin == 1 && begin[0] == 'C')
			categories[i] = &locale_c;
		else if (end - begin == 7 && !memcmp(begin, "C.UTF-8", 7))
			categories[i] = &locale_utf8;
		else
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
	return local != NULL ? (struct __locale *)local :
	    __atomic_load_n(&global_category[LC_CTYPE], __ATOMIC_ACQUIRE);
}

char *
setlocale(int category, const char *name)
{
	struct __locale *categories[LOCALE_CATEGORY_COUNT];
	struct __locale *locale;
	char *result;
	unsigned first, last, i;

	if (category < LC_CTYPE || category > LC_ALL) {
		errno = EINVAL;
		return NULL;
	}
	first = category == LC_ALL ? 0U : (unsigned)category;
	last = category == LC_ALL ? LOCALE_CATEGORY_COUNT : first + 1U;
	if (name == NULL) {
		if (category == LC_ALL)
			return global_locale_name();
		locale = __atomic_load_n(&global_category[first],
		    __ATOMIC_ACQUIRE);
		return (char *)locale->name;
	}
	if (category == LC_ALL && locale_composite_parse(name, categories)) {
		unsigned mask;

		locale_lock();
		for (i = 0; i < LOCALE_CATEGORY_COUNT; i++)
			global_category[i] = categories[i];
		mask = global_locale_mask_locked();
		if (mask == 0)
			result = (char *)locale_c.name;
		else if (mask == (1U << LOCALE_CATEGORY_COUNT) - 1U)
			result = (char *)locale_utf8.name;
		else
			result = locale_composite_locked(mask);
		locale_unlock();
		return result;
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
	return locale->name;
}

locale_t
newlocale(int mask, const char *name, locale_t base)
{
	struct __locale *wanted;
	if (mask < 0 || ((unsigned)mask & ~LC_ALL_MASK) != 0 ||
	    name == NULL || base == LC_GLOBAL_LOCALE) {
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
	/*
	 * The initial database has internally uniform locales. A later database
	 * may replace this with immutable per-category composite objects.
	 */
	return wanted;
}

locale_t
duplocale(locale_t locale)
{
	if (locale == NULL) {
		errno = EINVAL;
		return NULL;
	}
	if (locale == LC_GLOBAL_LOCALE)
		return effective_locale();
	return locale;
}

void
freelocale(locale_t locale)
{
	(void)locale;
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
		result = binding != NULL && binding->directory_set ?
		    binding->directory : default_locale_directory;
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
		result = binding != NULL && binding->codeset_set ?
		    binding->codeset : NULL;
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
		uint32_t original_length = catalog_word(data + catalog->originals +
		    index * 8U, catalog->swapped);
		uint32_t original_offset = catalog_word(data + catalog->originals +
		    index * 8U + 4U, catalog->swapped);
		uint32_t translated_length = catalog_word(data +
		    catalog->translations + index * 8U, catalog->swapped);
		uint32_t translated_offset = catalog_word(data +
		    catalog->translations + index * 8U + 4U, catalog->swapped);

		if (!catalog_range_valid(catalog->size, original_offset,
		    original_length, 1U) || (size_t)original_offset +
		    original_length >= catalog->size ||
		    data[original_offset + original_length] != 0 ||
		    !catalog_range_valid(catalog->size, translated_offset,
		    translated_length, 1U) || (size_t)translated_offset +
		    translated_length >= catalog->size ||
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
		count = read(descriptor, loaded.data + done, loaded.size - done);
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
		return locale;
	local = __pthread_locale_exchange != NULL ?
	    __pthread_locale_exchange(NULL, 0) : NULL;
	if (local != NULL)
		return (struct __locale *)local;
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
	directory = binding != NULL && binding->directory_set ?
	    binding->directory : default_locale_directory;
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
		uint32_t length = catalog_word(data + catalog->originals +
		    index * 8U, catalog->swapped);
		uint32_t offset = catalog_word(data + catalog->originals +
		    index * 8U + 4U, catalog->swapped);
		uint32_t translated_length;
		uint32_t translated_offset;
		const char *translated;
		unsigned form;

		if (length < identifier_length ||
		    memcmp(data + offset, identifier, identifier_length) != 0 ||
		    (length != identifier_length &&
		    data[offset + identifier_length] != 0))
			continue;
		translated_length = catalog_word(data + catalog->translations +
		    index * 8U, catalog->swapped);
		translated_offset = catalog_word(data + catalog->translations +
		    index * 8U + 4U, catalog->swapped);
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
	const char *plural_identifier, unsigned long int number, int category,
	locale_t locale)
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
		return (char *)(number == 1 || plural_identifier == NULL ?
		    identifier : plural_identifier);
	locale_lock();
	if (domainname == NULL)
		strcpy(domain, current_textdomain);
	else if (strlen(domainname) <= TEXTDOMAIN_MAX)
		strcpy(domain, domainname);
	else
		domain[0] = '\0';
	locale_unlock();
	if (domain[0] == '\0' || !message_catalog_path(path, sizeof(path),
	    domain, selected))
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
	return (char *)(number == 1 || plural_identifier == NULL ? identifier :
	    plural_identifier);
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
dcngettext_l(const char *domainname, const char *msgid1,
    const char *msgid2, unsigned long int n, int category, locale_t locale)
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
	return effective_locale()->utf8 ? 4U : 1U;
}

struct lconv *
localeconv(void)
{
	static char empty[] = "";
	static char decimal[] = ".";
	static struct lconv value = {
		decimal, empty, empty, empty, empty, empty, empty, empty,
		empty, empty, 127, 127, 127, 127, 127, 127, 127, 127
	};
	return &value;
}

char *
nl_langinfo(nl_item item)
{
	static char empty[] = "";
	static char decimal[] = ".";
	static char ascii[] = "US-ASCII";
	static char utf8[] = "UTF-8";
	if (item == CODESET)
		return effective_locale()->utf8 ? utf8 : ascii;
	if (item == RADIXCHAR)
		return decimal;
	return empty;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

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
	void *state = __pthread_mbstate != NULL ?
	    __pthread_mbstate(which) : NULL;
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
	if (!effective_locale()->utf8) {
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
			s->needed = 2; s->value = first & 0x1fU;
		} else if (first >= 0xe0U && first <= 0xefU) {
			s->needed = 3; s->value = first & 0x0fU;
		} else if (first >= 0xf0U && first <= 0xf4U) {
			s->needed = 4; s->value = first & 0x07U;
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
	if ((total == 2 && value < 0x80U) ||
	    (total == 3 && value < 0x800U) ||
	    (total == 4 && value < 0x10000U) ||
	    value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
		memset(s, 0, sizeof(*s));
		errno = EILSEQ;
		return (size_t)-1;
	}
	memset(s, 0, sizeof(*s));
	if (result != NULL)
		*result = (wchar_t)value;
	return used;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *st)
{ return mbrtowc(NULL, s, n, st); }

size_t
wcrtomb(char *bytes, wchar_t character, mbstate_t *state)
{
	mbstate_t *s = state != NULL ? state : internal_state(1);
	uint32_t value = (uint32_t)character;
	memset(s, 0, sizeof(*s));
	if (bytes == NULL)
		return 1;
	if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU) ||
	    (!effective_locale()->utf8 && value > 0x7fU)) {
		errno = EILSEQ;
		return (size_t)-1;
	}
	if (value < 0x80U) { bytes[0] = (char)value; return 1; }
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

int wctob(wint_t value) { return value <= 0x7fU ? (int)value : EOF; }

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
