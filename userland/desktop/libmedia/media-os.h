/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of the operating system beyond ISO C and
 * <pthread.h>: loading the Vulkan loader at run time, and reading a media
 * file at any offset.  media-os-posix.c and media-os-win32.c implement it;
 * the build compiles one of them.  It is private to libmedia (and the
 * mediafile code built into it).
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

#endif
