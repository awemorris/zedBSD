/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_LIMITS_H
#define LIBC_LIMITS_H

#include <uapi/limits.h>

#define TEXTDOMAIN_MAX 255
#define CHARCLASS_NAME_MAX 14
#define RE_DUP_MAX 255
#define MB_LEN_MAX 4
#define BC_BASE_MAX 99
#define BC_DIM_MAX 2048
#define BC_SCALE_MAX 99
#define BC_STRING_MAX 1000
#define COLL_WEIGHTS_MAX 2
#define EXPR_NEST_MAX 32
#define LINE_MAX 2048

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255U
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif
#define SHRT_MIN (-32767 - 1)
#define SHRT_MAX 32767
#define USHRT_MAX 65535U
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#if __SIZEOF_LONG__ == 8
#define LONG_MIN (-9223372036854775807L - 1L)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL
#else
#define LONG_MIN (-2147483647L - 1L)
#define LONG_MAX 2147483647L
#define ULONG_MAX 4294967295UL
#endif
#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

/*
 * The widths of long and int in bits (XSI): long is as wide as a pointer on
 * the LP64 targets and 32 bits on i386.
 */
#if __SIZEOF_LONG__ == 8
#define LONG_BIT 64
#else
#define LONG_BIT 32
#endif
#define WORD_BIT 32

/*
 * The largest signal number sysconf(_SC_NSIG) can report: the same value as
 * NSIG in <uapi/signal.h>, which this header does not include so as not to
 * define the signals' names here.
 */
#define NSIG_MAX 64

/* The default process priority nice() and getpriority() count from (XSI). */
#define NZERO 20

/*
 * The message catalogues' and printf()'s positional limits (XSI): the
 * smallest values the standard allows, which are what catgets() and the
 * formatting functions are built to.
 */
#define NL_ARGMAX 9
#define NL_LANGMAX 14
#define NL_MSGMAX 32767
#define NL_SETMAX 255
#define NL_TEXTMAX _POSIX2_LINE_MAX

/*
 * The minimum values every conforming system meets (POSIX.1-2024 <limits.h>,
 * "Minimum Values"): fixed by the standard, not by this system.  The ones
 * <uapi/limits.h> already defines (_POSIX_ARG_MAX, _POSIX_HOST_NAME_MAX,
 * _POSIX_RTSIG_MAX, _POSIX_SIGQUEUE_MAX) come from there.
 */
#define _POSIX_AIO_LISTIO_MAX 2
#define _POSIX_AIO_MAX 1
#define _POSIX_CHILD_MAX 25
#define _POSIX_CLOCKRES_MIN 20000000
#define _POSIX_DELAYTIMER_MAX 32
#define _POSIX_LINK_MAX 8
#define _POSIX_LOGIN_NAME_MAX 9
#define _POSIX_MAX_CANON 255
#define _POSIX_MAX_INPUT 255
#define _POSIX_MQ_OPEN_MAX 8
#define _POSIX_MQ_PRIO_MAX 32
#define _POSIX_NAME_MAX 14
#define _POSIX_NGROUPS_MAX 8
#define _POSIX_OPEN_MAX 20
#define _POSIX_PATH_MAX 256
#define _POSIX_PIPE_BUF 512
#define _POSIX_RE_DUP_MAX 255
#define _POSIX_SEM_NSEMS_MAX 256
#define _POSIX_SEM_VALUE_MAX 32767
#define _POSIX_SSIZE_MAX 32767
#define _POSIX_SS_REPL_MAX 4
#define _POSIX_STREAM_MAX 8
#define _POSIX_SYMLINK_MAX 255
#define _POSIX_SYMLOOP_MAX 8
#define _POSIX_THREAD_DESTRUCTOR_ITERATIONS 4
#define _POSIX_THREAD_KEYS_MAX 128
#define _POSIX_THREAD_THREADS_MAX 64
#define _POSIX_TIMER_MAX 32
#define _POSIX_TTY_NAME_MAX 9
#define _POSIX_TZNAME_MAX 6
#define _POSIX2_BC_BASE_MAX 99
#define _POSIX2_BC_DIM_MAX 2048
#define _POSIX2_BC_SCALE_MAX 99
#define _POSIX2_BC_STRING_MAX 1000
#define _POSIX2_CHARCLASS_NAME_MAX 14
#define _POSIX2_COLL_WEIGHTS_MAX 2
#define _POSIX2_EXPR_NEST_MAX 32
#define _POSIX2_LINE_MAX 2048
#define _POSIX2_RE_DUP_MAX 255
#define _XOPEN_IOV_MAX 16
#define _XOPEN_NAME_MAX 255
#define _XOPEN_PATH_MAX 1024

#endif
