/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_LIBINTL_H
#define ZEDBSD_LIBINTL_H

#include <limits.h>
#include <locale.h>

#define TEXTDOMAINMAX	TEXTDOMAIN_MAX

char *
bind_textdomain_codeset(
	const char *domainname,
	const char *codeset);

char *
bindtextdomain(
	const char *domainname,
	const char *dirname);

char *
dcgettext(
	const char *domainname,
	const char *msgid,
	int category);

char *
dcgettext_l(
	const char *domainname,
	const char *msgid,
	int category,
	locale_t locale);

char *
dcngettext(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	int category);

char *
dcngettext_l(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	int category,
	locale_t locale);

char *
dgettext(
	const char *domainname,
	const char *msgid);

char *
dgettext_l(
	const char *domainname,
	const char *msgid,
	locale_t locale);

char *
dngettext(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n);

char *
dngettext_l(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	locale_t locale);

char *
gettext(
	const char *msgid);

char *
gettext_l(
	const char *msgid,
	locale_t locale);

char *
ngettext(
	const char *msgid1,
	const char *msgid2,
	unsigned long int n);

char *
ngettext_l(
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	locale_t locale);

char *
textdomain(
	const char *domainname);

#endif
