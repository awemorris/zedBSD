/* Remaining ISO C wide-character and UTF conversion interfaces. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uchar.h>
#include <wchar.h>

extern void *__libc_internal_mbstate(unsigned) __attribute__((weak));

static mbstate_t *
uchar_internal_state(void)
{
	static mbstate_t bootstrap;
	void *state = __libc_internal_mbstate != NULL ?
	    __libc_internal_mbstate(2) : NULL;

	return state != NULL ? state : &bootstrap;
}

wchar_t *wcscat(wchar_t *d, const wchar_t *s) { wcscpy(d + wcslen(d), s); return d; }
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{ wchar_t *p = d + wcslen(d); while (n-- && *s) *p++ = *s++; *p = 0; return d; }
size_t wcsspn(const wchar_t *s, const wchar_t *accept)
{ size_t n = 0; while (s[n] && wcschr(accept, s[n])) n++; return n; }
size_t wcscspn(const wchar_t *s, const wchar_t *reject)
{ size_t n = 0; while (s[n] && !wcschr(reject, s[n])) n++; return n; }
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept)
{ s += wcscspn(s, accept); return *s ? (wchar_t *)s : NULL; }
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n)
{ size_t length = wcslen(n); if (!length) return (wchar_t *)h; for (; *h; h++) if (!wcsncmp(h, n, length)) return (wchar_t *)h; return NULL; }
wchar_t *wcstok(wchar_t *s, const wchar_t *separators, wchar_t **state)
{
	if (!s)
		s = *state;
	if (!s)
		return NULL;
	s += wcsspn(s, separators);
	if (!*s) { *state = NULL; return NULL; } wchar_t *end = s + wcscspn(s, separators);
	if (*end) { *end = 0; *state = end + 1; } else *state = NULL; return s;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{ while (n-- != 0) { if (*s == c) return (wchar_t *)s; s++; } return NULL; }

static char *wide_to_bytes(const wchar_t *s, size_t *wide_length)
{
	const wchar_t *cursor = s; mbstate_t state = {0}; size_t n = wcsrtombs(NULL, &cursor, 0, &state);
	if (n == (size_t)-1)
		return NULL;
	char *out = malloc(n + 1);
	if (!out)
		return NULL;
	cursor = s; memset(&state, 0, sizeof(state)); (void)wcsrtombs(out, &cursor, n + 1, &state);
	if (wide_length)
		*wide_length = wcslen(s);
	return out;
}

/* Wide printf/scanf use wide arguments for unqualified %c and %s.  The
 * narrow core uses the ISO %lc/%ls spellings for those same arguments. */
static char *
wide_format_to_bytes(const wchar_t *format)
{
	char *input = wide_to_bytes(format, NULL);
	char *output;
	size_t in, out = 0, length;

	if (input == NULL)
		return NULL;
	length = strlen(input);
	output = malloc(length * 2U + 1U);
	if (output == NULL) {
		free(input);
		return NULL;
	}
	for (in = 0; in < length;) {
		int has_h = 0, has_l = 0;
		if (input[in] != '%') {
			output[out++] = input[in++];
			continue;
		}
		output[out++] = input[in++];
		if (input[in] == '%') {
			output[out++] = input[in++];
			continue;
		}
		while (in < length && strchr("diouxXfFeEgGaAcspn[", input[in]) == NULL) {
			has_h |= input[in] == 'h';
			has_l |= input[in] == 'l';
			output[out++] = input[in++];
		}
		if (in < length && (input[in] == 'c' || input[in] == 's') &&
		    !has_h && !has_l)
			output[out++] = 'l';
		if (in < length)
			output[out++] = input[in++];
	}
	output[out] = 0;
	free(input);
	return output;
}
static char *wide_numeric(const wchar_t *s, wchar_t **end, char **bytes)
{
	*bytes = wide_to_bytes(s, NULL); if (!*bytes) { if (end) *end = (wchar_t *)s; return NULL; } return *bytes;
}
static void numeric_end(const wchar_t *start, wchar_t **end, char *bytes, char *at)
{
	if (end) { size_t byte_count = (size_t)(at - bytes), chars = 0, used = 0; mbstate_t state = {0};
		while (used < byte_count) { wchar_t wc; size_t n = mbrtowc(&wc, bytes + used, byte_count - used, &state); if (n == (size_t)-1 || n == (size_t)-2) break; used += n ? n : 1; chars++; } *end = (wchar_t *)start + chars; }
	free(bytes);
}
long wcstol(const wchar_t *s, wchar_t **e, int b) { char *x, *at; if (!wide_numeric(s,e,&x)) return 0; long v=strtol(x,&at,b); numeric_end(s,e,x,at); return v; }
unsigned long wcstoul(const wchar_t *s, wchar_t **e, int b) { char *x,*at; if(!wide_numeric(s,e,&x))return 0; unsigned long v=strtoul(x,&at,b); numeric_end(s,e,x,at); return v; }
long long wcstoll(const wchar_t *s, wchar_t **e, int b) { char *x,*at; if(!wide_numeric(s,e,&x))return 0; long long v=strtoll(x,&at,b); numeric_end(s,e,x,at); return v; }
unsigned long long wcstoull(const wchar_t *s, wchar_t **e, int b) { char *x,*at; if(!wide_numeric(s,e,&x))return 0; unsigned long long v=strtoull(x,&at,b); numeric_end(s,e,x,at); return v; }
double wcstod(const wchar_t *s, wchar_t **e) { char *x,*at; if(!wide_numeric(s,e,&x))return 0; double v=strtod(x,&at); numeric_end(s,e,x,at); return v; }
float wcstof(const wchar_t *s, wchar_t **e) { return (float)wcstod(s,e); }
long double wcstold(const wchar_t *s, wchar_t **e) { return (long double)wcstod(s,e); }

wchar_t *fgetws(wchar_t *s, int n, FILE *stream)
{ int i = 0; wint_t c; if (!s || n <= 0) return NULL; while (i + 1 < n && (c = fgetwc(stream)) != WEOF) { s[i++] = (wchar_t)c; if (c == L'\n') break; } if (!i) return NULL; s[i] = 0; return s; }
int fputws(const wchar_t *s, FILE *stream)
{ while (*s) if (fputwc(*s++, stream) == WEOF) return -1; return 0; }
int ungetwc(wint_t c, FILE *stream)
{ char bytes[4]; mbstate_t state = {0}; size_t n = wcrtomb(bytes, (wchar_t)c, &state); return n == 1 && ungetc((unsigned char)bytes[0], stream) != EOF ? (int)c : (int)WEOF; }

int vswprintf(wchar_t *output, size_t size, const wchar_t *format, va_list ap)
{
	char *narrow_format = wide_format_to_bytes(format); if (!narrow_format) return -1;
	va_list copy; va_copy(copy, ap); int bytes = vsnprintf(NULL, 0, narrow_format, copy); va_end(copy);
	if (bytes < 0) { free(narrow_format); return -1; } char *text = malloc((size_t)bytes + 1); if (!text) { free(narrow_format); return -1; }
	(void)vsnprintf(text, (size_t)bytes + 1, narrow_format, ap); const char *cursor = text; mbstate_t state = {0}; size_t chars = mbsrtowcs(output, &cursor, size, &state);
	free(text); free(narrow_format); if (chars == (size_t)-1 || chars >= size) { if (size) output[0] = 0; return -1; } return (int)chars;
}
int swprintf(wchar_t *o, size_t n, const wchar_t *f, ...) { va_list ap; va_start(ap,f); int r=vswprintf(o,n,f,ap); va_end(ap); return r; }
int vfwprintf(FILE *stream, const wchar_t *format, va_list ap)
{ char *f=wide_format_to_bytes(format); if(!f)return -1; int r=vfprintf(stream,f,ap); free(f); return r; }
int fwprintf(FILE *s,const wchar_t *f,...) { va_list ap;va_start(ap,f);int r=vfwprintf(s,f,ap);va_end(ap);return r; }
int vwprintf(const wchar_t *f,va_list ap) { return vfwprintf(stdout,f,ap); }
int wprintf(const wchar_t *f,...) { va_list ap;va_start(ap,f);int r=vwprintf(f,ap);va_end(ap);return r; }
int vswscanf(const wchar_t *input,const wchar_t *format,va_list ap)
{ char *i=wide_to_bytes(input,NULL),*f=wide_format_to_bytes(format); if(!i||!f){free(i);free(f);return EOF;} int r=vsscanf(i,f,ap);free(i);free(f);return r; }
int swscanf(const wchar_t *i,const wchar_t *f,...) {va_list ap;va_start(ap,f);int r=vswscanf(i,f,ap);va_end(ap);return r;}
int vfwscanf(FILE *s,const wchar_t *f,va_list ap) {char *x=wide_format_to_bytes(f);if(!x)return EOF;int r=vfscanf(s,x,ap);free(x);return r;}
int fwscanf(FILE *s,const wchar_t *f,...) {va_list ap;va_start(ap,f);int r=vfwscanf(s,f,ap);va_end(ap);return r;}
int vwscanf(const wchar_t *f,va_list ap) {return vfwscanf(stdin,f,ap);}
int wscanf(const wchar_t *f,...) {va_list ap;va_start(ap,f);int r=vwscanf(f,ap);va_end(ap);return r;}

size_t wcsftime(wchar_t *output,size_t size,const wchar_t *format,const struct tm *timeptr)
{ char *f=wide_to_bytes(format,NULL); if(!f)return 0; size_t cap=size?size*4:1;char *text=malloc(cap);if(!text){free(f);return 0;}size_t n=strftime(text,cap,f,timeptr);free(f);if(!n){free(text);return 0;}const char *p=text;mbstate_t state={0};size_t r=mbsrtowcs(output,&p,size,&state);free(text);return r==(size_t)-1||r>=size?0:r; }

size_t mbrtoc32(char32_t *out,const char *s,size_t n,mbstate_t *state)
{ wchar_t wc; size_t r=mbrtowc(&wc,s,n,state);if(out&&r!=(size_t)-1&&r!=(size_t)-2)*out=(char32_t)wc;return r; }
size_t c32rtomb(char *s,char32_t c,mbstate_t *state)
{ if(c>0x10ffffU||(c>=0xd800U&&c<=0xdfffU)){errno=EILSEQ;return(size_t)-1;}return wcrtomb(s,(wchar_t)c,state); }
size_t mbrtoc16(char16_t *out,const char *s,size_t n,mbstate_t *state)
{
	if (!state)
		state = uchar_internal_state();
	if(state->needed==0xff){if(out)*out=(char16_t)state->value;state->needed=0;return(size_t)-3;}
	wchar_t wc;size_t r=mbrtowc(&wc,s,n,state);if(r==(size_t)-1||r==(size_t)-2)return r;
	if((uint32_t)wc<=0xffffU){if(out)*out=(char16_t)wc;return r;}
	uint32_t value=(uint32_t)wc-0x10000U;if(out)*out=(char16_t)(0xd800U+(value>>10));state->value=0xdc00U+(value&0x3ffU);state->needed=0xff;return r;
}
size_t c16rtomb(char *s,char16_t c,mbstate_t *state)
{
	if (!state)
		state = uchar_internal_state();
	if(!s){memset(state,0,sizeof(*state));return 1;}
	if(c>=0xd800U&&c<=0xdbffU){
		if (state->needed == 0xfe) {
			memset(state, 0, sizeof(*state));
			errno = EILSEQ;
			return (size_t)-1;
		}
		state->value=c;state->needed=0xfe;return 0;
	}
	if(state->needed==0xfe && !(c>=0xdc00U&&c<=0xdfffU))
		{ memset(state,0,sizeof(*state));errno=EILSEQ;return(size_t)-1; }
	uint32_t value=c;if(c>=0xdc00U&&c<=0xdfffU&&state->needed==0xfe){value=0x10000U+((state->value-0xd800U)<<10)+(c-0xdc00U);state->needed=0;}
	else if(c>=0xd800U&&c<=0xdfffU){errno=EILSEQ;return(size_t)-1;}
	return c32rtomb(s,value,state);
}

size_t
wcslcpy(wchar_t *destination, const wchar_t *source, size_t size)
{
	size_t length = wcslen(source);
	size_t copied;

	if (size != 0U) {
		copied = length < size - 1U ? length : size - 1U;
		wmemcpy(destination, source, copied);
		destination[copied] = L'\0';
	}
	return length;
}

size_t
wcslcat(wchar_t *destination, const wchar_t *source, size_t size)
{
	size_t destination_length = 0;
	size_t source_length = wcslen(source);
	size_t available;
	size_t copied;

	while (destination_length < size && destination[destination_length] != L'\0')
		destination_length++;
	if (destination_length == size)
		return size + source_length;
	available = size - destination_length;
	copied = source_length < available - 1U ? source_length : available - 1U;
	wmemcpy(destination + destination_length, source, copied);
	destination[destination_length + copied] = L'\0';
	return destination_length + source_length;
}
