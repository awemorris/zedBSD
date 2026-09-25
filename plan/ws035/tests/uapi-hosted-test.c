/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host/native switch of include/uapi (include/uapi/hosted.h).
 *
 * Built by plan/ws035/tests/run-uapi-hosted-test.sh in several ways:
 *
 * UAPI_TEST_HOST     the host compiler and C library.  The uapi headers and
 *                    the host headers are included in one translation unit
 *                    and the standard names are the host's.
 * UAPI_TEST_NATIVE   KERN_UAPI_NATIVE: the uapi headers alone give zedBSD's
 *                    values and layouts.
 * UAPI_TEST_LIBC     KERN_UAPI_NATIVE with the zedBSD C library headers on
 *                    the include path, as the fixtures that read them do.
 * UAPI_TEST_SHADOW   with UAPI_TEST_HOST: include/uapi is itself on the
 *                    include path (-Iinclude/uapi), so <errno.h>, <fcntl.h>,
 *                    <limits.h>, <time.h> and <unistd.h> first find the uapi
 *                    headers of the same name, which must pass on to the
 *                    host's.  Only those headers are included: the older uapi
 *                    headers named like host headers (signal.h, poll.h) were
 *                    never usable that way and are not part of this test.
 *
 * The runner also compiles UAPI_TEST_LIBC without KERN_UAPI_NATIVE and
 * expects the #error of the uapi headers.
 */

#if defined(UAPI_TEST_HOST)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(UAPI_TEST_LIBC)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/ioctl.h>
#include <uapi/limits.h>
#include <uapi/mount.h>
#include <uapi/time.h>
#include <uapi/types.h>
#include <uapi/unistd.h>
#if !defined(UAPI_TEST_SHADOW)
#include <uapi/mman.h>
#include <uapi/resource.h>
#include <uapi/stat.h>
#include <uapi/statvfs.h>
#include <uapi/un.h>
#include <uapi/wait.h>
#endif

#if defined(UAPI_TEST_HOST)

/* Reports whether the standard names are the host's, as the host calls need. */
int
main(void)
{
	struct stat status;
	struct timespec now;

	/* The host's numbers, not zedBSD's (zedBSD's EINVAL is 3, O_CREAT 0x100). */
	if (EINVAL == 3 || O_CREAT == 0x100 || ENOENT != 2) {
		printf("uapi-hosted: host names are not the host's\n");
		return 1;
	}

	/* A host call through the names the uapi headers passed through. */
	if (stat("/", &status) != 0 || !S_ISDIR(status.st_mode)) {
		printf("uapi-hosted: host stat failed\n");
		return 1;
	}

	/* The host's clock and time types. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		printf("uapi-hosted: host clock_gettime failed\n");
		return 1;
	}

	/* zedBSD's own names are defined in every build. */
	if (KERN_IOC_INOUT != 0xc0000000UL || KERN_MOUNT_ARGS_VERSION != 1U ||
	    sizeof(struct mount_args) != 40U) {
		printf("uapi-hosted: zedBSD names missing in a host build\n");
		return 1;
	}

	printf("uapi-hosted: host EINVAL=%d O_CREAT=0x%x PATH_MAX=%d: PASS\n", EINVAL, O_CREAT, PATH_MAX);
	return 0;
}

#else

_Static_assert(EINVAL == 3, "zedBSD EINVAL");
_Static_assert(ENOTSUP == 21, "zedBSD ENOTSUP");
_Static_assert(O_CREAT == 0x100, "zedBSD O_CREAT");
_Static_assert(AT_FDCWD == -100, "zedBSD AT_FDCWD");
_Static_assert(PATH_MAX == 256, "zedBSD PATH_MAX");
_Static_assert(SSIZE_MAX == 9223372036854775807L, "zedBSD SSIZE_MAX (LP64)");
_Static_assert(SEEK_END == 2 && SEEK_HOLE == 4 && R_OK == 4, "zedBSD seek and access");
_Static_assert(MAP_FIXED_NOREPLACE == 0x100000, "zedBSD MAP_FIXED_NOREPLACE");
_Static_assert(WNOWAIT == 0x10 && P_PGID == 2, "zedBSD wait");
_Static_assert(S_IFMT == 0170000U && S_ISDIR(0040755), "zedBSD file types");
_Static_assert(sizeof(struct stat) == 112, "zedBSD stat (LP64)");
_Static_assert(sizeof(struct timeval) == 16, "zedBSD timeval (LP64)");
_Static_assert(sizeof(struct statvfs) == 88, "zedBSD statvfs");
_Static_assert(sizeof(struct rusage) == 144, "zedBSD rusage (LP64)");
_Static_assert(sizeof(struct sockaddr_un) == 110, "zedBSD sockaddr_un");
_Static_assert(sizeof(tid_t) == 4 && sizeof(off_t) == 8, "zedBSD scalar types");
_Static_assert(_IOW('f', 1, struct kern_file_format_reserve) == 0x80206601UL, "zedBSD ioctl encoding");

#if defined(UAPI_TEST_LIBC)
/* The C library adds errno to the numbers it takes from uapi. */
int
uapi_hosted_errno(void)
{
	return errno;
}
#else
/* Keeps the native translation unit non-empty. */
int uapi_hosted_native;
#endif

#endif
