/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_LIBC_LOCALE_DB_H
#define ZEDBSD_LIBC_LOCALE_DB_H

#include <zedbsd/locale-format.h>

struct zed_locale_record;

struct zed_locale_record *zed_locale_record_load(const char *);
const char *zed_locale_record_name(const struct zed_locale_record *);
const char *zed_locale_record_value(const struct zed_locale_record *,
				    enum zedbsd_locale_key);
unsigned zed_locale_record_utf8(const struct zed_locale_record *);
int zed_locale_key_category(enum zedbsd_locale_key);

#endif
