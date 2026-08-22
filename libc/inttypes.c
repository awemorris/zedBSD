/* ISO C integer-format conversion helpers. SPDX-License-Identifier: Zlib */
#include <inttypes.h>
#include <stdlib.h>

intmax_t imaxabs(intmax_t value) { return value < 0 ? -value : value; }
imaxdiv_t imaxdiv(intmax_t n, intmax_t d)
{ imaxdiv_t r = { n / d, n % d }; return r; }
intmax_t strtoimax(const char *s, char **e, int b) { return strtoll(s, e, b); }
uintmax_t strtoumax(const char *s, char **e, int b) { return strtoull(s, e, b); }
intmax_t wcstoimax(const wchar_t *s, wchar_t **e, int b)
{ return wcstoll(s, e, b); }
uintmax_t wcstoumax(const wchar_t *s, wchar_t **e, int b)
{ return wcstoull(s, e, b); }
