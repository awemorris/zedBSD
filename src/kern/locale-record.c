/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Kernel-only built-in locale records; the filesystem locale DB is userland. */
#include "libc/locale-db.h"

#include <locale.h>
#include <string.h>

struct zed_locale_record {
	const char *name;
	unsigned utf8;
};

static struct zed_locale_record c_locale = {"C", 0};
static struct zed_locale_record utf8_locale = {"C.UTF-8", 1};
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

struct zed_locale_record *
zed_locale_record_load(const char *name)
{
	return name != NULL &&
		       (!strcmp(name, "C.UTF-8") || !strcmp(name, "C.utf8") ||
			!strcmp(name, "UTF-8"))
		   ? &utf8_locale
		   : &c_locale;
}

const char *
zed_locale_record_name(const struct zed_locale_record *record)
{
	return record != NULL ? record->name : "C";
}

const char *
zed_locale_record_value(const struct zed_locale_record *record,
			enum zedbsd_locale_key key)
{
	if (key <= ZEDBSD_LOCALE_KEY_INVALID || key >= ZEDBSD_LOCALE_KEY_COUNT)
		return "";
	return record != NULL && record->utf8 ? utf8_values[key]
					      : c_values[key];
}

unsigned
zed_locale_record_utf8(const struct zed_locale_record *record)
{
	return record != NULL ? record->utf8 : 0;
}

int
zed_locale_key_category(enum zedbsd_locale_key key)
{
	return key > ZEDBSD_LOCALE_KEY_INVALID && key < ZEDBSD_LOCALE_KEY_COUNT
		   ? key_categories[key]
		   : -1;
}
