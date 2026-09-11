/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Kernel-only built-in locale records; the filesystem locale DB is userland.
 */

#include "libc/locale-db.h"

#include <locale.h>
#include <string.h>

struct zed_locale_record {
	const char *name;
	unsigned utf8;
};

static struct zed_locale_record c_locale = {"C", 0};
static struct zed_locale_record utf8_locale = {"C.UTF-8", 1};
static const int key_categories[KERN_LOCALE_KEY_COUNT] = {
    [KERN_LOCALE_KEY_INVALID] = -1,
#define KERN_LOCALE_CATEGORY(name, category, keyword, c_value, utf8_value)   \
	[KERN_LOCALE_KEY_##name] = category,
    KERN_LOCALE_KEYS(KERN_LOCALE_CATEGORY)
#undef KERN_LOCALE_CATEGORY
};
static const char *const c_values[KERN_LOCALE_KEY_COUNT] = {
    [KERN_LOCALE_KEY_INVALID] = "",
#define KERN_LOCALE_C_VALUE(name, category, keyword, c_value, utf8_value)    \
	[KERN_LOCALE_KEY_##name] = c_value,
    KERN_LOCALE_KEYS(KERN_LOCALE_C_VALUE)
#undef KERN_LOCALE_C_VALUE
};
static const char *const utf8_values[KERN_LOCALE_KEY_COUNT] = {
    [KERN_LOCALE_KEY_INVALID] = "",
#define KERN_LOCALE_UTF8_VALUE(name, category, keyword, c_value, utf8_value) \
	[KERN_LOCALE_KEY_##name] = utf8_value,
    KERN_LOCALE_KEYS(KERN_LOCALE_UTF8_VALUE)
#undef KERN_LOCALE_UTF8_VALUE
};

/*
 * Selects the built-in record for a locale name.
 *
 * Every UTF-8 spelling selects the UTF-8 record; anything else, including
 * a missing name, selects the C locale.
 */
struct zed_locale_record *
zed_locale_record_load(
	const char *name)
{
	/* A missing name means the C locale. */
	if (name == NULL)
		return &c_locale;

	/* Recognizes the accepted UTF-8 spellings. */
	if (!strcmp(name, "C.UTF-8"))
		return &utf8_locale;
	if (!strcmp(name, "C.utf8"))
		return &utf8_locale;
	if (!strcmp(name, "UTF-8"))
		return &utf8_locale;

	/* Falls back to the C locale. */
	return &c_locale;
}

/*
 * Reports the name of a locale record.
 */
const char *
zed_locale_record_name(
	const struct zed_locale_record *record)
{
	/* A missing record reads as the C locale. */
	if (record == NULL)
		return "C";

	/* Reports the recorded name. */
	return record->name;
}

/*
 * Reports the value of one locale key in a record.
 */
const char *
zed_locale_record_value(
	const struct zed_locale_record *record,
	enum kern_locale_key key)
{
	/* An invalid key has an empty value. */
	if (key <= KERN_LOCALE_KEY_INVALID || key >= KERN_LOCALE_KEY_COUNT)
		return "";

	/* Selects the UTF-8 table only for a UTF-8 record. */
	if (record != NULL && record->utf8)
		return utf8_values[key];

	/* Reports the C locale value. */
	return c_values[key];
}

/*
 * Tests whether a locale record uses UTF-8.
 */
unsigned
zed_locale_record_utf8(
	const struct zed_locale_record *record)
{
	/* A missing record is not UTF-8. */
	if (record == NULL)
		return 0;

	/* Reports the recorded flag. */
	return record->utf8;
}

/*
 * Reports the locale category that owns a key.
 */
int
zed_locale_key_category(
	enum kern_locale_key key)
{
	/* An invalid key belongs to no category. */
	if (key <= KERN_LOCALE_KEY_INVALID || key >= KERN_LOCALE_KEY_COUNT)
		return -1;

	/* Reports the recorded category. */
	return key_categories[key];
}
