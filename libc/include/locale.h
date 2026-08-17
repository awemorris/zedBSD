/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_LOCALE_H
#define ZEDBSD_LOCALE_H

#include <stddef.h>
#include <stdint.h>

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

#define LC_CTYPE_MASK    (1U << LC_CTYPE)
#define LC_NUMERIC_MASK  (1U << LC_NUMERIC)
#define LC_TIME_MASK     (1U << LC_TIME)
#define LC_COLLATE_MASK  (1U << LC_COLLATE)
#define LC_MONETARY_MASK (1U << LC_MONETARY)
#define LC_MESSAGES_MASK (1U << LC_MESSAGES)
#define LC_ALL_MASK      ((1U << 6) - 1U)

struct __zedbsd_locale;
typedef struct __zedbsd_locale *locale_t;
#define LC_GLOBAL_LOCALE ((locale_t)(intptr_t)-1)

struct lconv {
	char *decimal_point;
	char *thousands_sep;
	char *grouping;
	char *int_curr_symbol;
	char *currency_symbol;
	char *mon_decimal_point;
	char *mon_thousands_sep;
	char *mon_grouping;
	char *positive_sign;
	char *negative_sign;
	char int_frac_digits;
	char frac_digits;
	char p_cs_precedes;
	char p_sep_by_space;
	char n_cs_precedes;
	char n_sep_by_space;
	char p_sign_posn;
	char n_sign_posn;
};

char *setlocale(int, const char *);
struct lconv *localeconv(void);
locale_t newlocale(int, const char *, locale_t);
locale_t duplocale(locale_t);
void freelocale(locale_t);
locale_t uselocale(locale_t);

size_t __zedbsd_mb_cur_max(void);
#define MB_CUR_MAX (__zedbsd_mb_cur_max())

#endif
