/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file control interface of zedBSD: the open flags, the *at() flags, the
 * fcntl(2) commands and zedBSD's own file requests.  <fcntl.h> of the C
 * library includes this and adds struct flock and the functions.
 */

#include <uapi/hosted.h>

#ifndef KERN_UAPI_FCNTL_H
#define KERN_UAPI_FCNTL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <uapi/ioctl.h>

#define KERN_FILE_FORMAT_VERSION 1U

/* The opened description owns this mutation lease until its final close. */
struct kern_file_format_reserve {
	uint32_t version;
	uint32_t struct_size;
	uint64_t size_bytes;
	uint64_t reserved[2];
};

typedef char kern_file_format_size_check[
	sizeof(struct kern_file_format_reserve) == 32U ? 1 : -1];
typedef char kern_file_format_alignment_check[
	offsetof(struct kern_file_format_reserve, size_bytes) == 8U ? 1 : -1];

#define KERN_FILE_FORMAT_RESERVE \
	_IOW('f', 1, struct kern_file_format_reserve)

/*
 * Operations for flock(2).
 *
 * The lock is whole-file and belongs to the open file description, so a
 * descriptor passed through fork or dup shares one lock rather than taking
 * a second.  These are the values every system with this call uses.
 */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

struct flock_record {
	int16_t type;
	int16_t whence;
	int32_t reserved0;
	int64_t start;
	int64_t length;
	int32_t pid;
	uint32_t reserved1;
};

#if !KERN_UAPI_HOST_LIBC
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0100
#define O_EXCL      0x0200
#define O_TRUNC     0x0400
#define O_APPEND    0x0800
#define O_DIRECTORY 0x1000
#define O_NONBLOCK  0x2000
#define O_CLOEXEC   0x4000
#define O_NOCTTY    0x8000
#define O_DSYNC     0x10000
#define O_SYNC      0x20000
#define O_NOFOLLOW  0x40000
#define O_CLOFORK   0x80000

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_REMOVEDIR        0x0200
#define AT_EACCESS          0x0400
#define AT_SYMLINK_FOLLOW   0x0800

#define FD_CLOEXEC  0x0001
#define FD_CLOFORK  0x0002

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 5
#define F_GETLK          6
#define F_SETLK          7
#define F_SETLKW         8
#define F_GETOWN         9
#define F_SETOWN         10
#define F_DUPFD_CLOFORK  11
#define F_OFD_GETLK      12
#define F_OFD_SETLK      13
#define F_OFD_SETLKW     14

#define F_RDLCK 1
#define F_WRLCK 2
#define F_UNLCK 3
#endif

#ifdef __cplusplus
}
#endif

#endif

#if !KERN_UAPI_HOST_LIBC
/*
 * A host fixture that asks for zedBSD's ABI (KERN_UAPI_NATIVE) and puts
 * include/uapi itself on the include path reaches this header under the
 * standard name <fcntl.h>; the zedBSD C library's <fcntl.h>, later on the
 * path, then still supplies the rest of the standard header.  The chain is
 * taken only when the zedBSD C library headers do come later (rtld-abi.h is
 * one of them and exists nowhere else), so it never reaches the host's
 * header.  A zedBSD build never chains: the kernel has no such header, and
 * the C library's includes this one.
 */
#if !defined(__ZEDBSD__) && !defined(LIBC_FCNTL_H) && defined(__has_include_next)
#if __has_include_next(<rtld-abi.h>) && __has_include_next(<fcntl.h>)
#pragma GCC system_header
#include_next <fcntl.h>
#endif
#endif
#else

/*
 * A host fixture compiles kernel source against the host C library, whose
 * open flags the fixture's own calls take.  The host's <fcntl.h> is the next
 * one on the include path; #include_next reaches it even when this directory
 * is itself on the path under the standard name.  The pragma keeps the
 * extension quiet in a -pedantic fixture.
 */
#pragma GCC system_header
#include_next <fcntl.h>
#ifdef LIBC_FCNTL_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif
