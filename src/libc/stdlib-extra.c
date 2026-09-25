/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ISO C and commonly used BSD stdlib interfaces.
 */

#include "src/libc/heap.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
extern pid_t waitpid(pid_t, int *, int);

#define EXIT_HANDLER_MAX 64

/*
 * An exit handler is either a plain atexit function or the C++ ABI's form,
 * which takes an argument and names the shared object it belongs to.  Both
 * run in the reverse of their registration order, in one sequence, because
 * C++ requires a static object to be destroyed before anything registered
 * before it.
 */
struct exit_handler {
	void (*plain)(void);
	void (*with_argument)(void *);
	void *argument;
	void *owner;
};

static struct exit_handler exit_handlers[EXIT_HANDLER_MAX];
static void (*quick_handlers[EXIT_HANDLER_MAX])(void);
static size_t exit_handler_count, quick_handler_count;
static volatile uint32_t handler_lock;

static void lock_handlers(void)
{ while (__atomic_exchange_n(&handler_lock, 1U, __ATOMIC_ACQUIRE) != 0) ; }
static void unlock_handlers(void)
{ __atomic_store_n(&handler_lock, 0U, __ATOMIC_RELEASE); }

int atexit(void (*fn)(void))
{
	if (fn == NULL) return -1;
	lock_handlers();
	if (exit_handler_count == EXIT_HANDLER_MAX) { unlock_handlers(); return -1; }
	exit_handlers[exit_handler_count].plain = fn;
	exit_handlers[exit_handler_count].with_argument = NULL;
	exit_handlers[exit_handler_count].argument = NULL;
	exit_handlers[exit_handler_count].owner = NULL;
	exit_handler_count++;
	unlock_handlers(); return 0;
}

/*
 * Implements the cxa atexit operation.
 *
 * The Itanium C++ ABI's registration for a static object's destructor: the
 * destructor takes the object, and the handle names the shared object it
 * lives in so that unloading that object can run it early.  Nothing here
 * unloads a shared object while its static objects are alive, so the handle
 * is recorded and __cxa_finalize has nothing of its own to do.
 */
int __cxa_atexit(void (*destructor)(void *), void *object, void *handle)
{
	if (destructor == NULL) return -1;
	lock_handlers();
	if (exit_handler_count == EXIT_HANDLER_MAX) { unlock_handlers(); return -1; }
	exit_handlers[exit_handler_count].plain = NULL;
	exit_handlers[exit_handler_count].with_argument = destructor;
	exit_handlers[exit_handler_count].argument = object;
	exit_handlers[exit_handler_count].owner = handle;
	exit_handler_count++;
	unlock_handlers(); return 0;
}

/*
 * Implements the cxa finalize operation.
 *
 * Runs the destructors registered for one shared object, or for all of them
 * when the handle is NULL.  Each runs once: it is taken out of the list
 * before it is called, so a destructor that exits again does not repeat it.
 */
void __cxa_finalize(void *handle)
{
	size_t index;

	for (;;) {
		void (*with_argument)(void *) = NULL;
		void *argument = NULL;

		lock_handlers();
		for (index = exit_handler_count; index > 0; index--) {
			struct exit_handler *entry = &exit_handlers[index - 1U];
			if (entry->with_argument == NULL) continue;
			if (handle != NULL && entry->owner != handle) continue;
			with_argument = entry->with_argument;
			argument = entry->argument;
			/* Closes the gap so the remaining order is kept. */
			for (; index < exit_handler_count; index++)
				exit_handlers[index - 1U] = exit_handlers[index];
			exit_handler_count--;
			break;
		}
		unlock_handlers();
		if (with_argument == NULL) return;
		with_argument(argument);
	}
}
int at_quick_exit(void (*fn)(void))
{
	if (fn == NULL) return -1;
	lock_handlers();
	if (quick_handler_count == EXIT_HANDLER_MAX) { unlock_handlers(); return -1; }
	quick_handlers[quick_handler_count++] = fn;
	unlock_handlers(); return 0;
}
void __libc_run_exit_handlers(void)
{
	for (;;) {
		struct exit_handler entry;

		lock_handlers();
		if (!exit_handler_count) { unlock_handlers(); return; }
		entry = exit_handlers[--exit_handler_count];
		unlock_handlers();

		/* Runs whichever form this registration was. */
		if (entry.with_argument != NULL)
			entry.with_argument(entry.argument);
		else if (entry.plain != NULL)
			entry.plain();
	}
}
void __libc_run_quick_exit_handlers(void)
{ for (;;) { void (*fn)(void); lock_handlers(); if (!quick_handler_count) { unlock_handlers(); return; } fn = quick_handlers[--quick_handler_count]; unlock_handlers(); fn(); } }
void _Exit(int status) { _exit(status); }
void quick_exit(int status)
{ __libc_run_quick_exit_handlers(); _Exit(status); }

void *aligned_alloc(size_t alignment, size_t size)
{
	void *result;

	if (alignment == 0 || (alignment & (alignment - 1)) ||
	    size % alignment != 0) {
		errno = EINVAL;
		return NULL;
	}
	result = heap_aligned_alloc_active(alignment, size);
	if (result == NULL)
		errno = ENOMEM;
	return result;
}

/*
 * POSIX aligned allocation: the alignment is a power of two and a multiple
 * of sizeof(void *), and the size need not be a multiple of it.  errno is
 * left alone; the error is the result.
 */
int posix_memalign(void **memory, size_t alignment, size_t size)
{
	void *result;

	if (memory == NULL || alignment < sizeof(void *) ||
	    (alignment & (alignment - 1)) != 0)
		return EINVAL;
	result = heap_aligned_alloc_active(alignment, size != 0 ? size : 1U);
	if (result == NULL)
		return ENOMEM;
	*memory = result;
	return 0;
}

static void swap_bytes(unsigned char *a, unsigned char *b, size_t n)
{ while (n-- != 0) { unsigned char t = *a; *a++ = *b; *b++ = t; } }
static void insertion_sort(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *))
{
	unsigned char *bytes = base;
	for (size_t i = 1; i < count; i++)
		for (size_t j = i; j && compare(bytes + (j - 1) * size,
		    bytes + j * size) > 0; j--)
			swap_bytes(bytes + (j - 1) * size, bytes + j * size, size);
}
void qsort(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *))
{ if (size && compare) insertion_sort(base, count, size, compare); }
void *bsearch(const void *key, const void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *))
{
	const unsigned char *bytes = base;
	while (count) { size_t middle = count / 2; const void *item = bytes + middle * size;
		int order = compare(key, item); if (!order) return (void *)item;
		if (order > 0) { bytes = (const unsigned char *)item + size; count -= middle + 1; }
		else count = middle; }
	return NULL;
}
static void insertion_sort_r(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *, void *), void *context)
{
	unsigned char *bytes = base;
	for (size_t i = 1; i < count; i++) for (size_t j = i; j &&
	    compare(bytes + (j - 1) * size, bytes + j * size, context) > 0; j--)
		swap_bytes(bytes + (j - 1) * size, bytes + j * size, size);
}
void qsort_r(void *base, size_t count, size_t size,
    int (*compare)(const void *, const void *, void *), void *context)
{ if (size && compare) insertion_sort_r(base, count, size, compare, context); }
int heapsort(void *b, size_t n, size_t s, int (*c)(const void *, const void *))
{ if (!s) { errno = EINVAL; return -1; } qsort(b, n, s, c); return 0; }
int mergesort(void *b, size_t n, size_t s, int (*c)(const void *, const void *))
{ if (!s) { errno = EINVAL; return -1; } qsort(b, n, s, c); return 0; }

div_t div(int n, int d) { div_t r = { n / d, n % d }; return r; }
ldiv_t ldiv(long n, long d) { ldiv_t r = { n / d, n % d }; return r; }
lldiv_t lldiv(long long n, long long d)
{ lldiv_t r = { n / d, n % d }; return r; }
long long llabs(long long n) { return n < 0 ? -n : n; }
int mblen(const char *s, size_t n)
{ static mbstate_t state; size_t r; if (!s) { memset(&state, 0, sizeof(state)); return 0; }
	r = mbrlen(s, n, &state); return r == (size_t)-1 || r == (size_t)-2 ? -1 : (int)r; }
int mbtowc(wchar_t *w, const char *s, size_t n)
{ static mbstate_t state; size_t r; if (!s) { memset(&state, 0, sizeof(state)); return 0; }
	r = mbrtowc(w, s, n, &state); return r == (size_t)-1 || r == (size_t)-2 ? -1 : (int)r; }
int wctomb(char *s, wchar_t w)
{ static mbstate_t state; size_t r; if (!s) { memset(&state, 0, sizeof(state)); return 0; }
	r = wcrtomb(s, w, &state); return r == (size_t)-1 ? -1 : (int)r; }
size_t mbstowcs(wchar_t *d, const char *s, size_t n)
{ mbstate_t state = {0}; return mbsrtowcs(d, &s, n, &state); }
size_t wcstombs(char *d, const wchar_t *s, size_t n)
{ mbstate_t state = {0}; return wcsrtombs(d, &s, n, &state); }

void *reallocarray(void *p, size_t n, size_t s)
{ if (n && s > SIZE_MAX / n) { errno = ENOMEM; return NULL; } return realloc(p, n * s); }
void *reallocf(void *p, size_t n)
{ void *r = realloc(p, n); if (!r && p) free(p); return r; }
void *recallocarray(void *p, size_t oldn, size_t n, size_t s)
{
	if ((oldn && s > SIZE_MAX / oldn) || (n && s > SIZE_MAX / n)) { errno = ENOMEM; return NULL; }
	size_t oldsize = oldn * s, newsize = n * s; void *r = realloc(p, newsize);
	if (r && newsize > oldsize) memset((unsigned char *)r + oldsize, 0, newsize - oldsize);
	return r;
}

static const char *program_name = "";
const char *getprogname(void) { return program_name; }
void setprogname(const char *name)
{ const char *slash = name ? strrchr(name, '/') : NULL; program_name = name ? (slash ? slash + 1 : name) : ""; }
long long strtonum(const char *s, long long minimum, long long maximum, const char **error)
{
	char *end; long long value; const char *message = NULL; errno = 0;
	value = strtoll(s, &end, 10);
	if (minimum > maximum) message = "invalid";
	else if (end == s || *end) message = "invalid";
	else if (errno == ERANGE || value < minimum) message = "too small";
	else if (value > maximum) message = "too large";
	if (error) *error = message;
	if (message) { errno = !strcmp(message, "invalid") ? EINVAL : ERANGE; return 0; }
	return value;
}

static uint32_t random_state_words[16];
static uint64_t random_counter;
static volatile uint32_t random_lock;

static uint32_t rotate_left(uint32_t value, unsigned count)
{ return (value << count) | (value >> (32U - count)); }
#define QUARTER(a,b,c,d) do { \
	a += b; d ^= a; d = rotate_left(d, 16); \
	c += d; b ^= c; b = rotate_left(b, 12); \
	a += b; d ^= a; d = rotate_left(d, 8); \
	c += d; b ^= c; b = rotate_left(b, 7); \
} while (0)
static void random_block(unsigned char output[64])
{
	uint32_t x[16];
	for (unsigned i = 0; i < 16; i++) x[i] = random_state_words[i];
	x[12] ^= (uint32_t)random_counter;
	x[13] ^= (uint32_t)(random_counter++ >> 32);
	for (unsigned i = 0; i < 10; i++) {
		QUARTER(x[0],x[4],x[8],x[12]); QUARTER(x[1],x[5],x[9],x[13]);
		QUARTER(x[2],x[6],x[10],x[14]); QUARTER(x[3],x[7],x[11],x[15]);
		QUARTER(x[0],x[5],x[10],x[15]); QUARTER(x[1],x[6],x[11],x[12]);
		QUARTER(x[2],x[7],x[8],x[13]); QUARTER(x[3],x[4],x[9],x[14]);
	}
	for (unsigned i = 0; i < 16; i++) {
		x[i] += random_state_words[i];
		for (unsigned j = 0; j < 4; j++) output[i * 4 + j] = (unsigned char)(x[i] >> (j * 8));
	}
}
#undef QUARTER
static void fallback_random(unsigned char *output, size_t length)
{
	while (__atomic_exchange_n(&random_lock, 1U, __ATOMIC_ACQUIRE) != 0) ;
	if (random_counter == 0) {
		struct timespec real = {0}, monotonic = {0};
		(void)clock_gettime(CLOCK_REALTIME, &real);
		(void)clock_gettime(CLOCK_MONOTONIC, &monotonic);
		uint64_t seed[4] = { (uint64_t)real.tv_sec ^ (uint64_t)real.tv_nsec,
		    (uint64_t)monotonic.tv_sec ^ (uint64_t)monotonic.tv_nsec,
		    (uint64_t)(uintptr_t)&real, (uint64_t)(unsigned)getpid() ^ (uint64_t)clock() };
		static const uint32_t constants[4] = {0x61707865U,0x3320646eU,0x79622d32U,0x6b206574U};
		for (unsigned i = 0; i < 4; i++) random_state_words[i] = constants[i];
		for (unsigned i = 0; i < 8; i++) random_state_words[i + 4] = (uint32_t)(seed[i / 2] >> ((i & 1U) * 32));
		random_state_words[12] = (uint32_t)(uintptr_t)output;
		random_state_words[13] = (uint32_t)((uint64_t)(uintptr_t)output >> 32);
		random_state_words[14] = (uint32_t)length;
		random_state_words[15] = (uint32_t)((uint64_t)length >> 32);
		random_counter = 1;
	}
	while (length) {
		unsigned char block[64]; size_t count = length < sizeof(block) ? length : sizeof(block);
		random_block(block); memcpy(output, block, count); memset_explicit(block, 0, sizeof(block));
		output += count; length -= count;
	}
	__atomic_store_n(&random_lock, 0U, __ATOMIC_RELEASE);
}
/* The kernel generator, 256 bytes a call; the local one only if it fails. */
void arc4random_buf(void *buffer, size_t length)
{
	unsigned char *out = buffer;
	while (length) {
		size_t count = length < 256 ? length : 256;
		if (getentropy(out, count) != 0) break;
		out += count; length -= count;
	}
	if (length) fallback_random(out, length);
}
uint32_t arc4random(void) { uint32_t value; arc4random_buf(&value, sizeof(value)); return value; }
uint32_t arc4random_uniform(uint32_t upper)
{ if (upper < 2) return 0; uint32_t minimum = (uint32_t)(-upper) % upper, value; do value = arc4random(); while (value < minimum); return value % upper; }
void srandomdev(void) { srand(arc4random()); }

int system(const char *command)
{
	if (command == NULL) return access("/bin/sh", X_OK) == 0;
	pid_t child = fork(); if (child < 0) return -1;
	if (child == 0) { execl("/bin/sh", "sh", "-c", command, (char *)NULL); _Exit(127); }
	int status; while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return -1;
	return status;
}
