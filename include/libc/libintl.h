/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_LIBINTL_H
#define LIBC_LIBINTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <locale.h>

#define TEXTDOMAINMAX	TEXTDOMAIN_MAX

/*
 * The translation functions return a message in place of a msgid.  Each is
 * marked format_arg for the msgid arguments, so a compiler checks a call
 * such as printf(gettext("%d files"), count) against the msgid's format, as
 * it would the literal itself; without the mark it sees a format that is
 * not a string literal.
 */

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
	int category)
	__attribute__((__format_arg__(2)));

char *
dcgettext_l(
	const char *domainname,
	const char *msgid,
	int category,
	locale_t locale)
	__attribute__((__format_arg__(2)));

char *
dcngettext(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	int category)
	__attribute__((__format_arg__(2))) __attribute__((__format_arg__(3)));

char *
dcngettext_l(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	int category,
	locale_t locale)
	__attribute__((__format_arg__(2))) __attribute__((__format_arg__(3)));

char *
dgettext(
	const char *domainname,
	const char *msgid)
	__attribute__((__format_arg__(2)));

char *
dgettext_l(
	const char *domainname,
	const char *msgid,
	locale_t locale)
	__attribute__((__format_arg__(2)));

char *
dngettext(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n)
	__attribute__((__format_arg__(2))) __attribute__((__format_arg__(3)));

char *
dngettext_l(
	const char *domainname,
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	locale_t locale)
	__attribute__((__format_arg__(2))) __attribute__((__format_arg__(3)));

char *
gettext(
	const char *msgid)
	__attribute__((__format_arg__(1)));

char *
gettext_l(
	const char *msgid,
	locale_t locale)
	__attribute__((__format_arg__(1)));

char *
ngettext(
	const char *msgid1,
	const char *msgid2,
	unsigned long int n)
	__attribute__((__format_arg__(1))) __attribute__((__format_arg__(2)));

char *
ngettext_l(
	const char *msgid1,
	const char *msgid2,
	unsigned long int n,
	locale_t locale)
	__attribute__((__format_arg__(1))) __attribute__((__format_arg__(2)));

char *
textdomain(
	const char *domainname);

#ifdef __cplusplus
}
#endif

#endif
