/* ISO C and commonly used BSD string extensions. SPDX-License-Identifier: Zlib */
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

void *
memccpy(void *destination, const void *source, int character, size_t count)
{
	unsigned char *out = destination;
	const unsigned char *in = source;
	unsigned char stop = (unsigned char)character;

	while (count-- != 0) {
		*out++ = *in;
		if (*in++ == stop)
			return out;
	}
	return NULL;
}

size_t strspn(const char *s, const char *accept)
{ size_t n = 0; while (s[n] && strchr(accept, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *reject)
{ size_t n = 0; while (s[n] && !strchr(reject, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *accept)
{ s += strcspn(s, accept); return *s ? (char *)s : NULL; }
char *strtok(char *s, const char *separators)
{
	static char *next;
	char *end;
	if (s == NULL) s = next;
	if (s == NULL) return NULL;
	s += strspn(s, separators);
	if (*s == 0) { next = NULL; return NULL; }
	end = s + strcspn(s, separators);
	if (*end) { *end = 0; next = end + 1; } else next = NULL;
	return s;
}

void *memmem(const void *h, size_t hn, const void *n, size_t nn)
{
	const unsigned char *p = h;
	if (nn == 0) return (void *)p;
	if (nn > hn) return NULL;
	for (size_t i = 0; i <= hn - nn; i++)
		if (p[i] == *(const unsigned char *)n && !memcmp(p + i, n, nn))
			return (void *)(p + i);
	return NULL;
}
void *mempcpy(void *d, const void *s, size_t n)
{ return (unsigned char *)memcpy(d, s, n) + n; }
void *memrchr(const void *s, int c, size_t n)
{
	const unsigned char *p = (const unsigned char *)s + n;
	while (n-- != 0) if (*--p == (unsigned char)c) return (void *)p;
	return NULL;
}
void *memset_explicit(void *s, int c, size_t n)
{
	static void *(*volatile erase)(void *, int, size_t) = memset;
	return erase(s, c, n);
}
void explicit_bzero(void *s, size_t n) { (void)memset_explicit(s, 0, n); }

int strcasecmp(const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		{ a++; b++; }
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
	while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
		{ a++; b++; n--; }
	return n ? tolower((unsigned char)*a) - tolower((unsigned char)*b) : 0;
}
char *strcasestr(const char *h, const char *n)
{
	size_t length = strlen(n);
	if (!length) return (char *)h;
	for (; *h; h++) if (!strncasecmp(h, n, length)) return (char *)h;
	return NULL;
}
char *strchrnul(const char *s, int c)
{ while (*s && *s != (char)c) s++; return (char *)s; }
size_t strlcpy(char *d, const char *s, size_t n)
{
	size_t length = strlen(s);
	if (n) { size_t copy = length < n - 1 ? length : n - 1; memcpy(d, s, copy); d[copy] = 0; }
	return length;
}
size_t strlcat(char *d, const char *s, size_t n)
{
	size_t dl = strnlen(d, n), sl = strlen(s);
	if (dl < n) strlcpy(d + dl, s, n - dl);
	return dl + sl;
}
char *strnstr(const char *h, const char *n, size_t length)
{
	size_t nl = strlen(n);
	if (!nl) return (char *)h;
	for (size_t i = 0; i + nl <= length && h[i]; i++)
		if (!memcmp(h + i, n, nl)) return (char *)(h + i);
	return NULL;
}
char *strsep(char **sp, const char *delimiters)
{
	char *s, *end;
	if (sp == NULL || (s = *sp) == NULL) return NULL;
	end = s + strcspn(s, delimiters);
	if (*end) { *end = 0; *sp = end + 1; } else *sp = NULL;
	return s;
}
int strverscmp(const char *a, const char *b)
{
	while (*a == *b && *a) { a++; b++; }
	if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
		const char *aa = a, *bb = b;
		while (*aa == '0') aa++;
		while (*bb == '0') bb++;
		const char *ae = aa, *be = bb;
		while (isdigit((unsigned char)*ae)) ae++;
		while (isdigit((unsigned char)*be)) be++;
		if (ae - aa != be - bb) return ae - aa < be - bb ? -1 : 1;
	}
	return (unsigned char)*a - (unsigned char)*b;
}
int timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b; unsigned char difference = 0;
	while (n-- != 0) difference |= *x++ ^ *y++;
	return difference != 0;
}
int timingsafe_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	unsigned int result = 0, decided = 0;
	while (n-- != 0) {
		unsigned int left = *x++, right = *y++;
		unsigned int different = ((left ^ right) | (0U - (left ^ right))) >> 31;
		unsigned int less = (left - right) >> 31;
		unsigned int choice = (0U - less) | 1U;
		result |= choice & (0U - different) & ~decided;
		decided |= 0U - different;
	}
	return (int)result;
}
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void bcopy(const void *s, void *d, size_t n) { (void)memmove(d, s, n); }
void bzero(void *s, size_t n) { (void)memset(s, 0, n); }
static int
first_set_word(unsigned long value)
{
	int bit = 1;

	if (value == 0)
		return 0;
	while ((value & 1U) == 0) {
		value >>= 1;
		bit++;
	}
	return bit;
}

static int
last_set_word(unsigned long value)
{
	int bit = 0;

	while (value != 0) {
		value >>= 1;
		bit++;
	}
	return bit;
}

int ffs(int x) { return first_set_word((unsigned int)x); }
int ffsl(long x) { return first_set_word((unsigned long)x); }

int
ffsll(long long x)
{
	unsigned long long value = (unsigned long long)x;
	unsigned long low = (unsigned int)value;

	if (low != 0)
		return first_set_word(low);
	return value >> 32 ? first_set_word((unsigned int)(value >> 32)) + 32 : 0;
}

int fls(int x) { return last_set_word((unsigned int)x); }
int flsl(long x) { return last_set_word((unsigned long)x); }

int
flsll(long long x)
{
	unsigned long long value = (unsigned long long)x;
	unsigned long high = (unsigned int)(value >> 32);

	if (high != 0)
		return last_set_word(high) + 32;
	return last_set_word((unsigned int)value);
}
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }

void strmode(unsigned int mode, char *p)
{
	static const char bits[] = "rwxrwxrwx";
	p[0] = (mode & 0170000U) == 0040000U ? 'd' :
	    (mode & 0170000U) == 0120000U ? 'l' :
	    (mode & 0170000U) == 0020000U ? 'c' :
	    (mode & 0170000U) == 0060000U ? 'b' : '-';
	for (int i = 0; i < 9; i++) p[i + 1] = mode & (0400U >> i) ? bits[i] : '-';
	if (mode & 04000U) p[3] = p[3] == 'x' ? 's' : 'S';
	if (mode & 02000U) p[6] = p[6] == 'x' ? 's' : 'S';
	if (mode & 01000U) p[9] = p[9] == 'x' ? 't' : 'T';
	p[10] = ' '; p[11] = 0;
}
