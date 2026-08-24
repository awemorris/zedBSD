/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#define _POSIX_C_SOURCE 202405L
#define _XOPEN_SOURCE 800

#include <devctl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <endian.h>
#include <fcntl.h>
#include <libintl.h>
#include <locale.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <termios.h>
#include <threads.h>
#include <uchar.h>
#include <unistd.h>
#include <wchar.h>

_Static_assert(sizeof(time_t) >= 8, "POSIX.1-2024 requires 64-bit time_t");
_Static_assert(_POSIX_VERSION == 202405L,
    "POSIX.1-2024 system interfaces must be advertised");
_Static_assert(_POSIX_DEVICE_CONTROL == 202405L,
    "posix_devctl option must be advertised");
_Static_assert(__ZEDBSD_LEGACY_VISIBLE == 0,
    "removed interfaces must not enter the Issue 8 namespace");
_Static_assert(FD_CLOFORK != FD_CLOEXEC, "descriptor flags must be distinct");
_Static_assert(O_CLOFORK != O_CLOEXEC, "open flags must be distinct");
_Static_assert(GETENTROPY_MAX == 256, "getentropy limit");

static atomic_uint counter = ATOMIC_VAR_INIT(0);

struct atomic_record {
	uint32_t first;
	uint32_t second;
	uint32_t third;
};

static _Atomic(struct atomic_record) record;

static int
thread_start(void *argument)
{
	(void)argument;
	return (int)atomic_fetch_add_explicit(&counter, 1U,
	    memory_order_relaxed);
}

int
main(void)
{
	struct atomic_record desired = { 1U, 2U, 3U };
	struct atomic_record expected = { 1U, 2U, 3U };
	struct atomic_record loaded;
	struct posix_dent *dent = NULL;
	struct winsize window = { 0 };
	Dl_info dynamic_information;
	char16_t character16 = 0;
	char32_t character32 = 0;
	thrd_start_t start = thread_start;
	void *(*allocate)(size_t, size_t) = aligned_alloc;
	ssize_t (*getdents_function)(int, void *, size_t, int) = posix_getdents;
	int (*close_function)(int, int) = posix_close;
	int (*devctl_function)(int, int, void *, size_t, int *) = posix_devctl;

	atomic_init(&record, desired);
	loaded = atomic_load_explicit(&record, memory_order_acquire);
	atomic_store_explicit(&record, loaded, memory_order_release);
	loaded = atomic_exchange(&record, desired);
	(void)atomic_compare_exchange_strong(&record, &expected, loaded);

	(void)dent;
	(void)window;
	(void)dynamic_information;
	(void)character16;
	(void)character32;
	(void)start;
	(void)allocate;
	(void)getdents_function;
	(void)close_function;
	(void)devctl_function;
	(void)accept4;
	(void)dladdr;
	(void)getentropy;
	(void)getlocalename_l;
	(void)gettext_l;
	(void)pthread_cond_clockwait;
	(void)pthread_mutex_clocklock;
	(void)pthread_rwlock_clockrdlock;
	(void)pthread_rwlock_clockwrlock;
	(void)sem_clockwait;
	(void)sig2str;
	(void)str2sig;
	(void)tcgetwinsize;
	(void)tcsetwinsize;
	(void)wcslcat;
	(void)wcslcpy;
	return BYTE_ORDER == LITTLE_ENDIAN || BYTE_ORDER == BIG_ENDIAN ? 0 : 1;
}
