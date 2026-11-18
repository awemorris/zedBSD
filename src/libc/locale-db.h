/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_LIBC_LOCALE_DB_H
#define KERN_LIBC_LOCALE_DB_H

#include <locale-format.h>

struct zed_locale_record;

struct zed_locale_record *zed_locale_record_load(const char *);
const char *zed_locale_record_name(const struct zed_locale_record *);
const char *zed_locale_record_value(const struct zed_locale_record *,
				    enum kern_locale_key);
unsigned zed_locale_record_utf8(const struct zed_locale_record *);
int zed_locale_key_category(enum kern_locale_key);

#endif
