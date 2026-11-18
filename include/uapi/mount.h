/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mount interface of zedBSD: the mount flags and the argument record
 * mount(2) passes to the kernel.  <sys/mount.h> of the C library includes
 * this and adds the functions.
 */

#ifndef KERN_UAPI_MOUNT_H
#define KERN_UAPI_MOUNT_H

#include <stdint.h>
#include <uapi/hosted.h>

#define KERN_MOUNT_ARGS_VERSION 1U
#define KERN_MOUNT_FSPEC_MAX 32U

/*
 * Version 2 (ws132-p004) adds the owner a filesystem without owners of
 * its own (FAT) shows for its files: with KERN_MOUNT_ARGS_OWNER in flags,
 * owner_uid and owner_gid replace root.  reserved is zero.  The kernel
 * takes either version, told apart by size and version.
 */
#define KERN_MOUNT_ARGS_VERSION_OWNER 2U
#define KERN_MOUNT_ARGS_OWNER 0x00000001U

struct mount_args {
	uint32_t size;
	uint32_t version;
	char fspec[KERN_MOUNT_FSPEC_MAX];
};

struct mount_args_owner {
	uint32_t size;
	uint32_t version;
	char fspec[KERN_MOUNT_FSPEC_MAX];
	uint32_t owner_uid;
	uint32_t owner_gid;
	uint32_t flags;
	uint32_t reserved;
};

#if !KERN_UAPI_HOST_LIBC
#define MNT_RDONLY 0x00000001U
#define MNT_NOSUID  0x00000002U
/*
 * Every write reaches the device before the call that made it returns.
 * Without it a filesystem that can may keep writes in memory for a few
 * seconds and write them out in the background.
 */
#define MNT_WRITETHRU 0x00000004U
/*
 * The filesystem keeps no journal of its metadata changes.  A filesystem
 * that journals by default otherwise keeps one, creating it when missing.
 */
#define MNT_NOJOURNAL 0x00000008U
/* No file of the filesystem is executed: exec answers EACCES (ws132-p004). */
#define MNT_NOEXEC  0x00000010U
/*
 * The filesystem is held by this machine rather than reached over a
 * network.  A program deciding whether a file is worth watching, or cheap
 * to read twice, asks this.
 */
#define MNT_LOCAL   0x00001000U
#else

/*
 * A host fixture compiles kernel source against the host C library and takes
 * the mount flags from it.
 */
#include <sys/mount.h>
#ifdef LIBC_SYS_MOUNT_H
#error "the zedBSD C library headers are on the include path of a host build; define KERN_UAPI_NATIVE"
#endif
#endif

#endif
