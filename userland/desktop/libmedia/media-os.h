/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of the operating system beyond ISO C: loading the
 * Vulkan loader at run time, reading a media file at any offset, and the
 * threads, mutexes, conditions, one-time calls, monotonic clock and sleep
 * the decoders and the engine share.  media-os-posix.c (POSIX threads) and
 * media-os-win32.c (the system's own threads and locks, without a POSIX
 * thread layer) implement it; the build compiles one of them.  It is
 * private to libmedia (and the mediafile code built into it).
 *
 * Text is UTF-8 and NUL-terminated (the Windows implementation converts to
 * UTF-16 and calls the W functions only).  Fallible functions return 0 or
 * an errno value of <errno.h>: EINVAL (an argument breaks the contract),
 * ENOENT, EACCES, EISDIR, EILSEQ (a path that is not UTF-8), ENAMETOOLONG,
 * ENOMEM, ENODEV (no Vulkan loader), EIO (anything else).  Every function
 * may be called from any thread; a file is used by one thread at a time.
 */

#ifndef KEILAND_MEDIA_OS_H
#define KEILAND_MEDIA_OS_H

#ifndef _WIN32
#include <pthread.h>
#endif
#include <stddef.h>
#include <stdint.h>

/*
 * The Vulkan loader once loaded (opaque).  media_os_vulkan_open makes it;
 * libmedia keeps it for the life of the process and never unloads it.
 */
struct media_os_library;

/*
 * A media file open for reading (a POSIX descriptor or a Windows handle,
 * opaque).  media_os_file_open makes it and media_os_file_close frees it.
 */
struct media_os_file;

/* A function found in a library, cast by the caller to its real type. */
typedef void (*media_os_function)(void);

/*
 * A mutex (not recursive: a thread that holds it does not lock it again).
 * On POSIX it is a pthread_mutex_t.  On Windows it is a slim reader-writer
 * lock taken exclusively, kept in room (the system's SRWLOCK is one
 * pointer, and this header does not include <windows.h>).
 */
struct media_os_mutex {
#ifdef _WIN32
	void *room;
#else
	pthread_mutex_t mutex;
#endif
};

/*
 * A condition, waited on under a media_os_mutex.  On POSIX it is a
 * pthread_cond_t; on Windows a CONDITION_VARIABLE (one pointer) kept in
 * room.
 */
struct media_os_cond {
#ifdef _WIN32
	void *room;
#else
	pthread_cond_t cond;
#endif
};

/*
 * A call made once in the process (media_os_once).  On POSIX it is a
 * pthread_once_t; on Windows an INIT_ONCE (one pointer) kept in room.  It
 * starts as MEDIA_OS_ONCE_INIT.
 */
struct media_os_once {
#ifdef _WIN32
	void *room;
#else
	pthread_once_t once;
#endif
};

/* A media_os_once whose call was not made yet. */
#ifdef _WIN32
#define MEDIA_OS_ONCE_INIT	{ NULL }
#else
#define MEDIA_OS_ONCE_INIT	{ PTHREAD_ONCE_INIT }
#endif

/*
 * A thread libmedia started (media_os_thread_start): what it runs and its
 * argument, and the system's thread (a pthread_t on POSIX, the handle of
 * _beginthreadex on Windows).  The record stays where it is from the start
 * until media_os_thread_join returns: the new thread reads it.
 */
struct media_os_thread {
	void (*run)(void *argument);
	void *argument;
#ifdef _WIN32
	void *handle;
#else
	pthread_t thread;
#endif
};

/*
 * Loads the system's Vulkan loader.
 *
 * library: receives the loader, or NULL on failure.
 * POSIX: dlopen of "libvulkan.so.1", then "libvulkan.so", with
 * RTLD_NOW | RTLD_LOCAL (the order and flags libmedia uses today).
 * Windows: LoadLibraryExW(L"vulkan-1.dll", NULL,
 * LOAD_LIBRARY_SEARCH_SYSTEM32) (the loader the GPU driver installs; never
 * a copy beside the program or on PATH).
 * Returns 0, EINVAL (library NULL), ENODEV (no loader), ENOMEM.
 */
int media_os_vulkan_open(struct media_os_library **library);

/*
 * Finds a function in the loader ("vkGetInstanceProcAddr").
 * Returns it, or NULL when absent or an argument is NULL.
 */
media_os_function media_os_library_symbol(struct media_os_library *library, const char *name);

/*
 * Opens a file for reading.
 *
 * path: not NULL, not empty.
 * file: receives the file, or NULL on failure.
 * size: receives the file's size in bytes (0 on failure).
 * POSIX: open(O_RDONLY | O_CLOEXEC) and fstat.  Windows: CreateFileW with
 * GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE (a
 * file being downloaded can still be read), OPEN_EXISTING, not inherited,
 * and GetFileSizeEx.  A directory is EISDIR.
 * Returns 0, EINVAL, ENOENT, EACCES, EISDIR, EILSEQ, ENAMETOOLONG, ENOMEM,
 * EIO.
 */
int media_os_file_open(const char *path, struct media_os_file **file, uint64_t *size);

/*
 * Reads exactly size bytes at offset.
 *
 * data: room for size bytes; NULL only when size is 0 (which succeeds and
 *       reads nothing).
 * Returns 0 (all read), EINVAL (file NULL, data NULL with size above 0,
 * offset + size overflowing 64 bits), EIO (a read failed, or the file ended
 * before size bytes: it shrank).  The file position is not used (POSIX
 * pread, retried on EINTR and short reads; Windows ReadFile with an
 * OVERLAPPED offset, in pieces of at most 1 GiB, on a synchronous handle).
 */
int media_os_file_read_at(struct media_os_file *file, uint64_t offset, void *data, size_t size);

/* Closes a file and frees it.  NULL does nothing. */
void media_os_file_close(struct media_os_file *file);

/*
 * Closes a descriptor that came with a GPU picture (POSIX only: the
 * dma-buf or opaque fd of VK_KHR_external_memory_fd).  Windows never makes
 * such a picture, so its implementation does nothing.  A negative value
 * does nothing.
 */
void media_os_close_descriptor(int descriptor);

/*
 * Makes a mutex.
 *
 * POSIX: pthread_mutex_init with the default attributes.  Windows:
 * InitializeSRWLock, which cannot fail.
 * Returns 0, or what pthread_mutex_init reported (EAGAIN, ENOMEM).
 */
int media_os_mutex_init(struct media_os_mutex *mutex);

/* Lets a mutex go (nobody holds it). */
void media_os_mutex_destroy(struct media_os_mutex *mutex);

/* Takes a mutex, waiting while another thread holds it. */
void media_os_mutex_lock(struct media_os_mutex *mutex);

/* Gives back a mutex the caller holds. */
void media_os_mutex_unlock(struct media_os_mutex *mutex);

/*
 * Makes a condition.
 *
 * POSIX: pthread_cond_init with the default attributes.  Windows:
 * InitializeConditionVariable, which cannot fail.
 * Returns 0, or what pthread_cond_init reported (EAGAIN, ENOMEM).
 */
int media_os_cond_init(struct media_os_cond *cond);

/* Lets a condition go (nobody waits on it). */
void media_os_cond_destroy(struct media_os_cond *cond);

/*
 * Waits on a condition at most a number of milliseconds, with the mutex
 * (held by the caller) given back meanwhile and held again afterwards.
 * The wait may end early for nothing; the caller looks at its state again.
 * POSIX: pthread_cond_timedwait until the realtime clock plus the
 * milliseconds.  Windows: SleepConditionVariableSRW, counted from now.
 */
void media_os_cond_wait_for(struct media_os_cond *cond, struct media_os_mutex *mutex, unsigned milliseconds);

/* Wakes every thread that waits on a condition. */
void media_os_cond_broadcast(struct media_os_cond *cond);

/*
 * Makes a call the first time a once is passed in the process, and never
 * again; a thread that passes it while the call runs waits until it
 * returned, and then sees everything the call wrote.
 *
 * POSIX: pthread_once.  Windows: InitOnceExecuteOnce.
 * Returns 0, or what pthread_once reported.
 */
int media_os_once(struct media_os_once *once, void (*run)(void));

/*
 * Starts a thread that runs run(argument).
 *
 * thread: the record the thread reads; it stays in place until
 *         media_os_thread_join returns.
 * POSIX: pthread_create with the default attributes.  Windows: the C
 * runtime's _beginthreadex (so that the runtime is set up for the thread).
 * Returns 0, or EAGAIN (or what pthread_create reported).
 */
int media_os_thread_start(struct media_os_thread *thread, void (*run)(void *argument), void *argument);

/* Waits for a thread started by media_os_thread_start to end, and lets it go. */
void media_os_thread_join(struct media_os_thread *thread);

/*
 * Reads the monotonic clock in nanoseconds (from an unspecified start).
 * POSIX: clock_gettime(CLOCK_MONOTONIC).  Windows:
 * QueryPerformanceCounter.  Returns 0 when the clock cannot be read.
 */
uint64_t media_os_clock_ns(void);

/*
 * Sleeps a number of milliseconds.  POSIX: one nanosleep (a signal may end
 * it early).  Windows: Sleep.
 */
void media_os_sleep_ms(unsigned milliseconds);

#endif
