/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_LOCALE_FORMAT_H
#define ZEDBSD_LOCALE_FORMAT_H

#include <stdint.h>

#define ZEDBSD_LOCALE_MAGIC "ZLOCALE1"
#define ZEDBSD_LOCALE_MAGIC_SIZE 8U
#define ZEDBSD_LOCALE_VERSION 1U
#define ZEDBSD_LOCALE_HEADER_SIZE 28U
#define ZEDBSD_LOCALE_ENTRY_SIZE 16U

/* id, category, localedef/locale keyword, C value, C.UTF-8 value */
#define ZEDBSD_LOCALE_KEYS(X)                                                  \
	X(CODESET, LC_CTYPE, "charmap", "US-ASCII", "UTF-8")                   \
	X(DECIMAL_POINT, LC_NUMERIC, "decimal_point", ".", ".")                \
	X(THOUSANDS_SEP, LC_NUMERIC, "thousands_sep", "", "")                  \
	X(GROUPING, LC_NUMERIC, "grouping", "", "")                            \
	X(INT_CURR_SYMBOL, LC_MONETARY, "int_curr_symbol", "", "")             \
	X(CURRENCY_SYMBOL, LC_MONETARY, "currency_symbol", "", "")             \
	X(MON_DECIMAL_POINT, LC_MONETARY, "mon_decimal_point", "", "")         \
	X(MON_THOUSANDS_SEP, LC_MONETARY, "mon_thousands_sep", "", "")         \
	X(MON_GROUPING, LC_MONETARY, "mon_grouping", "", "")                   \
	X(POSITIVE_SIGN, LC_MONETARY, "positive_sign", "", "")                 \
	X(NEGATIVE_SIGN, LC_MONETARY, "negative_sign", "", "")                 \
	X(INT_FRAC_DIGITS, LC_MONETARY, "int_frac_digits", "127", "127")       \
	X(FRAC_DIGITS, LC_MONETARY, "frac_digits", "127", "127")               \
	X(P_CS_PRECEDES, LC_MONETARY, "p_cs_precedes", "127", "127")           \
	X(P_SEP_BY_SPACE, LC_MONETARY, "p_sep_by_space", "127", "127")         \
	X(N_CS_PRECEDES, LC_MONETARY, "n_cs_precedes", "127", "127")           \
	X(N_SEP_BY_SPACE, LC_MONETARY, "n_sep_by_space", "127", "127")         \
	X(P_SIGN_POSN, LC_MONETARY, "p_sign_posn", "127", "127")               \
	X(N_SIGN_POSN, LC_MONETARY, "n_sign_posn", "127", "127")               \
	X(D_T_FMT, LC_TIME, "d_t_fmt", "%a %b %e %H:%M:%S %Y",                 \
	  "%a %b %e %H:%M:%S %Y")                                              \
	X(D_FMT, LC_TIME, "d_fmt", "%m/%d/%y", "%m/%d/%y")                     \
	X(T_FMT, LC_TIME, "t_fmt", "%H:%M:%S", "%H:%M:%S")                     \
	X(T_FMT_AMPM, LC_TIME, "t_fmt_ampm", "%I:%M:%S %p", "%I:%M:%S %p")     \
	X(AM_STR, LC_TIME, "am_pm[0]", "AM", "AM")                             \
	X(PM_STR, LC_TIME, "am_pm[1]", "PM", "PM")                             \
	X(ABDAY_1, LC_TIME, "abday[0]", "Sun", "Sun")                          \
	X(ABDAY_2, LC_TIME, "abday[1]", "Mon", "Mon")                          \
	X(ABDAY_3, LC_TIME, "abday[2]", "Tue", "Tue")                          \
	X(ABDAY_4, LC_TIME, "abday[3]", "Wed", "Wed")                          \
	X(ABDAY_5, LC_TIME, "abday[4]", "Thu", "Thu")                          \
	X(ABDAY_6, LC_TIME, "abday[5]", "Fri", "Fri")                          \
	X(ABDAY_7, LC_TIME, "abday[6]", "Sat", "Sat")                          \
	X(DAY_1, LC_TIME, "day[0]", "Sunday", "Sunday")                        \
	X(DAY_2, LC_TIME, "day[1]", "Monday", "Monday")                        \
	X(DAY_3, LC_TIME, "day[2]", "Tuesday", "Tuesday")                      \
	X(DAY_4, LC_TIME, "day[3]", "Wednesday", "Wednesday")                  \
	X(DAY_5, LC_TIME, "day[4]", "Thursday", "Thursday")                    \
	X(DAY_6, LC_TIME, "day[5]", "Friday", "Friday")                        \
	X(DAY_7, LC_TIME, "day[6]", "Saturday", "Saturday")                    \
	X(ABMON_1, LC_TIME, "abmon[0]", "Jan", "Jan")                          \
	X(ABMON_2, LC_TIME, "abmon[1]", "Feb", "Feb")                          \
	X(ABMON_3, LC_TIME, "abmon[2]", "Mar", "Mar")                          \
	X(ABMON_4, LC_TIME, "abmon[3]", "Apr", "Apr")                          \
	X(ABMON_5, LC_TIME, "abmon[4]", "May", "May")                          \
	X(ABMON_6, LC_TIME, "abmon[5]", "Jun", "Jun")                          \
	X(ABMON_7, LC_TIME, "abmon[6]", "Jul", "Jul")                          \
	X(ABMON_8, LC_TIME, "abmon[7]", "Aug", "Aug")                          \
	X(ABMON_9, LC_TIME, "abmon[8]", "Sep", "Sep")                          \
	X(ABMON_10, LC_TIME, "abmon[9]", "Oct", "Oct")                         \
	X(ABMON_11, LC_TIME, "abmon[10]", "Nov", "Nov")                        \
	X(ABMON_12, LC_TIME, "abmon[11]", "Dec", "Dec")                        \
	X(MON_1, LC_TIME, "mon[0]", "January", "January")                      \
	X(MON_2, LC_TIME, "mon[1]", "February", "February")                    \
	X(MON_3, LC_TIME, "mon[2]", "March", "March")                          \
	X(MON_4, LC_TIME, "mon[3]", "April", "April")                          \
	X(MON_5, LC_TIME, "mon[4]", "May", "May")                              \
	X(MON_6, LC_TIME, "mon[5]", "June", "June")                            \
	X(MON_7, LC_TIME, "mon[6]", "July", "July")                            \
	X(MON_8, LC_TIME, "mon[7]", "August", "August")                        \
	X(MON_9, LC_TIME, "mon[8]", "September", "September")                  \
	X(MON_10, LC_TIME, "mon[9]", "October", "October")                     \
	X(MON_11, LC_TIME, "mon[10]", "November", "November")                  \
	X(MON_12, LC_TIME, "mon[11]", "December", "December")                  \
	X(ERA, LC_TIME, "era", "", "")                                         \
	X(ERA_D_FMT, LC_TIME, "era_d_fmt", "", "")                             \
	X(ERA_D_T_FMT, LC_TIME, "era_d_t_fmt", "", "")                         \
	X(ERA_T_FMT, LC_TIME, "era_t_fmt", "", "")                             \
	X(ALT_DIGITS, LC_TIME, "alt_digits", "", "")                           \
	X(YESEXPR, LC_MESSAGES, "yesexpr", "^[yY]", "^[yY]")                   \
	X(NOEXPR, LC_MESSAGES, "noexpr", "^[nN]", "^[nN]")                     \
	X(YESSTR, LC_MESSAGES, "yesstr", "yes", "yes")                         \
	X(NOSTR, LC_MESSAGES, "nostr", "no", "no")                             \
	X(COLLATE, LC_COLLATE, "order_start", "forward", "forward")

enum zedbsd_locale_key {
	ZEDBSD_LOCALE_KEY_INVALID = 0,
#define ZEDBSD_LOCALE_ENUM(name, category, keyword, c_value, utf8_value)       \
	ZEDBSD_LOCALE_KEY_##name,
	ZEDBSD_LOCALE_KEYS(ZEDBSD_LOCALE_ENUM)
#undef ZEDBSD_LOCALE_ENUM
	    ZEDBSD_LOCALE_KEY_COUNT
};

static inline uint32_t
zedbsd_locale_get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
}

static inline void
zedbsd_locale_put32(unsigned char *bytes, uint32_t value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

#endif
